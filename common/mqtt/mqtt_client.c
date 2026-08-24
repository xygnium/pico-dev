#include <stdio.h>
#include <string.h>

#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"

#include "mqtt_client.h"
#include "wifi_secrets.h"

#define MQTT_STATUS_TOPIC "sensors/temp-sense/status"
#define MQTT_CLIENT_ID "temp-sense"

// Reconnect backoff. Starts short because the common case is a broker
// restart or a brief WiFi blip that clears in seconds, and caps at a minute
// so a broker that stays down doesn't have us retrying forever at 5s — the
// sensor loop and SD ring keep working regardless, and the SD ring is what
// makes a long outage survivable (records replay from the watermark later).
#define MQTT_RETRY_MIN_MS 5000
#define MQTT_RETRY_MAX_MS 60000

static mqtt_client_t *s_client;
static ip_addr_t s_broker_addr;

// Retained across reconnects rather than built on the stack per attempt: the
// Last Will and credentials have to be presented again on every CONNECT, so
// this has to outlive mqtt_temp_init().
static struct mqtt_connect_client_info_t s_ci;

// Written by lwIP callbacks (background thread), read by the sensor loop.
// volatile so the compiler can't hoist the flag test in mqtt_temp_poll() out
// of the loop — these are the only two variables crossing that boundary.
static volatile bool s_connected;
static volatile bool s_connecting;

// Main-loop only; no guarding needed.
static absolute_time_t s_next_attempt;
static uint32_t s_retry_ms = MQTT_RETRY_MIN_MS;

static void mqtt_temp_pub_request_cb(void *arg, err_t err) {
    if (err != ERR_OK) {
        printf("mqtt: publish failed (err %d)\n", err);
    }
}

// Runs on lwIP's background thread, not the sensor loop — see
// pico_cyw43_arch_lwip_threadsafe_background. Deliberately does not retry the
// connection itself: the client is mid-teardown when this fires for a
// disconnect, so reconnecting is left to mqtt_temp_poll() on the main loop.
static void mqtt_temp_connection_cb(mqtt_client_t *client, void *arg,
                                     mqtt_connection_status_t status) {
    s_connecting = false;
    if (status == MQTT_CONNECT_ACCEPTED) {
        printf("mqtt: connected to %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);
        s_connected = true;
        // Retained "online", mirroring the Last Will below, so a subscriber
        // that joins later sees current status immediately rather than
        // waiting on the next state change. Re-published on every reconnect,
        // which is what clears a stale "offline" left by the will.
        err_t err = mqtt_publish(client, MQTT_STATUS_TOPIC, "online",
                                  strlen("online"), 1, 1,
                                  mqtt_temp_pub_request_cb, NULL);
        if (err != ERR_OK) {
            printf("mqtt: online publish failed (err %d)\n", err);
        }
    } else {
        printf("mqtt: disconnected (status %d)\n", status);
        s_connected = false;
    }
}

// Fires one connect attempt and schedules when the next may happen. Shared by
// init and poll so there is exactly one place that decides backoff.
static void mqtt_temp_try_connect(void) {
    s_connecting = true;

    // Called from main-loop context (not an lwIP callback), so the connect
    // call itself needs the threadsafe_background lock — mirrors the
    // existing gap in ../common/wifi/wifi.c, closed here rather than
    // extended.
    cyw43_arch_lwip_begin();
    err_t err = mqtt_client_connect(s_client, &s_broker_addr, MQTT_BROKER_PORT,
                                     mqtt_temp_connection_cb, NULL, &s_ci);
    cyw43_arch_lwip_end();
    if (err != ERR_OK) {
        // No callback will fire for a call that never got started, so clear
        // the in-flight flag here or poll() would wait forever.
        s_connecting = false;
        printf("mqtt: connect call failed (err %d)\n", err);
    }

    s_next_attempt = make_timeout_time_ms(s_retry_ms);
    if (s_retry_ms < MQTT_RETRY_MAX_MS) {
        s_retry_ms *= 2;
        if (s_retry_ms > MQTT_RETRY_MAX_MS) {
            s_retry_ms = MQTT_RETRY_MAX_MS;
        }
    }
}

void mqtt_temp_init(void) {
    if (!ip4addr_aton(MQTT_BROKER_IP, &s_broker_addr)) {
        printf("mqtt: bad broker address %s\n", MQTT_BROKER_IP);
        return;
    }

    s_client = mqtt_client_new();
    if (!s_client) {
        printf("mqtt: client alloc failed\n");
        return;
    }

    memset(&s_ci, 0, sizeof(s_ci));
    s_ci.client_id = MQTT_CLIENT_ID;
    // The broker refuses anonymous connects (observed: CONNACK status 5,
    // MQTT_CONNECT_REFUSED_NOT_AUTHORIZED_). MQTT 3.1.1 has no
    // password-without-username form, so both must be set or neither.
    s_ci.client_user = MQTT_USER;
    s_ci.client_pass = MQTT_PASS;
    s_ci.keep_alive = 60;
    s_ci.will_topic = MQTT_STATUS_TOPIC;
    s_ci.will_msg = "offline";
    s_ci.will_qos = 1;
    s_ci.will_retain = 1;

    mqtt_temp_try_connect();
}

void mqtt_temp_poll(void) {
    if (!s_client) {
        return;  // init failed; nothing to retry against
    }
    if (s_connected) {
        // Reset both halves of the backoff, not just the interval. The
        // deadline was last computed by try_connect() using whatever
        // s_retry_ms was at the time — up to the 60s cap after a long
        // outage — and it outlives the connection that cleared it. Leaving
        // it stale delays the first retry of the *next* outage by up to a
        // full cap interval, which is precisely the fast recovery this
        // reset exists to provide. Observed on hardware as a 23s retry
        // where ~6s was intended.
        s_retry_ms = MQTT_RETRY_MIN_MS;
        s_next_attempt = get_absolute_time();
        return;
    }
    if (s_connecting) {
        return;  // attempt in flight; lwIP will time it out on its own
    }
    if (!time_reached(s_next_attempt)) {
        return;
    }

    printf("mqtt: reconnecting to %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);
    mqtt_temp_try_connect();
}

bool mqtt_temp_connected(void) {
    return s_connected;
}

bool mqtt_temp_publish_record(const temp_record_t *rec) {
    if (!s_connected) {
        return false;
    }
    if (!(rec->flags & TEMP_FLAG_VALID)) {
        return false;
    }

    // "sensors/temp-sense/" (19) + "0x" + 16 hex digits (18) +
    // "/temperature" (12) + NUL = 50; round up for headroom.
    char topic[64];
    snprintf(topic, sizeof(topic), "sensors/temp-sense/0x%016llx/temperature",
             (unsigned long long)rec->romcode);

    char payload[64];
    int len = snprintf(payload, sizeof(payload),
                        "{\"seq\":%lu,\"epoch\":%lu,\"c\":%.2f}",
                        (unsigned long)rec->seq, (unsigned long)rec->epoch,
                        temp_record_celsius(rec));

    // Main-loop context (not an lwIP callback), so this needs the
    // threadsafe_background lock — mirrors mqtt_temp_try_connect().
    cyw43_arch_lwip_begin();
    err_t err = mqtt_publish(s_client, topic, payload, (u16_t)len, 0, 1,
                              mqtt_temp_pub_request_cb, NULL);
    cyw43_arch_lwip_end();

    if (err != ERR_OK) {
        printf("mqtt: publish to %s failed (err %d)\n", topic, err);
        return false;
    }
    return true;
}
