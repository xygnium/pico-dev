#ifndef TEMP_SENSE_MQTT_CLIENT_H
#define TEMP_SENSE_MQTT_CLIENT_H

#include <stdbool.h>

#include "temp_record.h"

// Step 1 of the MQTT staging plan (see temp-sense/CLAUDE.md): connect to the
// broker with a Last Will on sensors/temp-sense/status (retained, "offline"),
// and publish "online" (retained) once CONNACK arrives. No sensor data is
// published yet — that's steps 2/3.
//
// Call once, after wifi_connect() succeeds. Non-blocking: the connect
// handshake and reconnects happen on lwIP's background thread
// (pico_cyw43_arch_lwip_threadsafe_background), not inline here.
void mqtt_temp_init(void);

// Call once per sensor cycle from the main loop, alongside wifi_udp_poll().
// lwIP's MQTT client never reconnects on its own, so without this a single
// dropped connection — a broker restart, a WiFi blip, an auth refusal —
// leaves the firmware silently offline until reboot. Cheap when connected:
// a flag test. Retries use a 5s..60s backoff.
void mqtt_temp_poll(void);

// True once CONNACK has been received for the current connection.
bool mqtt_temp_connected(void);

// Step 2 of the MQTT staging plan: publish one record to its sensor's
// retained topic, sensors/temp-sense/<romcode>/temperature. JSON payload
// carries seq and epoch (not just the value) because the design already
// depends on both downstream: consumers dedup on seq (QoS 1 elsewhere is
// at-least-once) and detect a seq decrease as a lineage reset after a
// format or card swap.
//
// QoS 0, retained. Returns false without publishing if not connected, or if
// the record is flagged CRC-error — a bad reading has no value worth
// retaining on the topic.
bool mqtt_temp_publish_record(const temp_record_t *rec);

#endif
