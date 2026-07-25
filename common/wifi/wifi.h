#ifndef PICO_COMMON_WIFI_H
#define PICO_COMMON_WIFI_H

#include <stdint.h>
#include <stddef.h>

// Handler invoked once per received UDP command; write a NUL-terminated
// reply into resp (up to resp_size bytes) to send it back to the sender.
typedef void (*wifi_udp_cmd_handler_t)(const char *cmd, char *resp, size_t resp_size);

// Bring up the CYW43 chip in station mode and connect, blocking until
// connected or failed. Returns 0 on success.
int wifi_connect(uint32_t country, const char *ssid, const char *pass, uint32_t auth);

// Bind a UDP listener on `port`. `handler` is invoked from wifi_udp_poll()
// whenever a command arrives.
void wifi_udp_start(uint16_t port, wifi_udp_cmd_handler_t handler);

// Call periodically from the main loop; sends a reply if a command arrived
// since the last poll.
void wifi_udp_poll(void);

#endif
