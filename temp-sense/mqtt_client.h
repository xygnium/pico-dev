#ifndef TEMP_SENSE_MQTT_CLIENT_H
#define TEMP_SENSE_MQTT_CLIENT_H

#include <stdbool.h>

// Step 1 of the MQTT staging plan (see temp-sense/CLAUDE.md): connect to the
// broker with a Last Will on sensors/temp-sense/status (retained, "offline"),
// and publish "online" (retained) once CONNACK arrives. No sensor data is
// published yet — that's steps 2/3.
//
// Call once, after wifi_connect() succeeds. Non-blocking: the connect
// handshake and reconnects happen on lwIP's background thread
// (pico_cyw43_arch_lwip_threadsafe_background), not inline here.
void mqtt_temp_init(void);

// True once CONNACK has been received for the current connection.
bool mqtt_temp_connected(void);

#endif
