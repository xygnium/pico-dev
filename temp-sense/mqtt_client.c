#include <stdio.h>
#include <string.h>

#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"
#include "pico/cyw43_arch.h"

#include "mqtt_client.h"
#include "wifi_secrets.h"

#define MQTT_STATUS_TOPIC "sensors/temp-sense/status"
#define MQTT_CLIENT_ID "temp-sense"

static mqtt_client_t *s_client;
static bool s_connected;

static void mqtt_temp_pub_request_cb(void *arg, err_t err) {
    if (err != ERR_OK) {
        printf("mqtt: publish failed (err %d)\n", err);
    }
}

// Runs on lwIP's background thread, not the sensor loop — see
// pico_cyw43_arch_lwip_threadsafe_background. Only touches s_connected and
// makes one publish call here, so no shared-state guarding beyond that is
// needed yet; revisit when a publish loop reads from the SD ring.
static void mqtt_temp_connection_cb(mqtt_client_t *client, void *arg,
                                     mqtt_connection_status_t status) {
    if (status == MQTT_CONNECT_ACCEPTED) {
        printf("mqtt: connected to %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);
        s_connected = true;
        // Retained "online", mirroring the Last Will below, so a subscriber
        // that joins later sees current status immediately rather than
        // waiting on the next state change.
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

void mqtt_temp_init(void) {
    ip_addr_t broker_addr;
    if (!ip4addr_aton(MQTT_BROKER_IP, &broker_addr)) {
        printf("mqtt: bad broker address %s\n", MQTT_BROKER_IP);
        return;
    }

    s_client = mqtt_client_new();
    if (!s_client) {
        printf("mqtt: client alloc failed\n");
        return;
    }

    struct mqtt_connect_client_info_t ci;
    memset(&ci, 0, sizeof(ci));
    ci.client_id = MQTT_CLIENT_ID;
    // The broker refuses anonymous connects (observed: CONNACK status 5,
    // MQTT_CONNECT_REFUSED_NOT_AUTHORIZED_). MQTT 3.1.1 has no
    // password-without-username form, so both must be set or neither.
    ci.client_user = MQTT_USER;
    ci.client_pass = MQTT_PASS;
    ci.keep_alive = 60;
    ci.will_topic = MQTT_STATUS_TOPIC;
    ci.will_msg = "offline";
    ci.will_qos = 1;
    ci.will_retain = 1;

    // Called from main-loop context (not an lwIP callback), so the connect
    // call itself needs the threadsafe_background lock — mirrors the
    // existing gap in ../common/wifi/wifi.c, closed here rather than
    // extended.
    cyw43_arch_lwip_begin();
    err_t err = mqtt_client_connect(s_client, &broker_addr, MQTT_BROKER_PORT,
                                     mqtt_temp_connection_cb, NULL, &ci);
    cyw43_arch_lwip_end();
    if (err != ERR_OK) {
        printf("mqtt: connect call failed (err %d)\n", err);
    }
}

bool mqtt_temp_connected(void) {
    return s_connected;
}
