#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "wifi.h"
#include "wifi_secrets.h"
#include "temp_store.h"

extern int example_ds18b20();

static void handle_wifi_cmd(const char *cmd, char *resp, size_t resp_size) {
    if (strcmp(cmd, "read") == 0) {
        if (g_temp_num_devs == 0) {
            snprintf(resp, resp_size, "no readings yet\n");
            return;
        }
        size_t off = snprintf(resp, resp_size, "%s\n", g_temp_timestamp);
        for (int i = 0; i < g_temp_num_devs && off < resp_size; i++) {
            if (g_temp_valid[i]) {
                off += snprintf(resp + off, resp_size - off,
                                 "device %d: %.2f C\n", i, g_temp_celsius[i]);
            } else {
                off += snprintf(resp + off, resp_size - off,
                                 "device %d: CRC error\n", i);
            }
        }
    } else {
        snprintf(resp, resp_size, "temp-sense ack: %s", cmd);
    }
}

int main() {
    stdio_init_all();
    printf("Hello, world!\n");

    int wifi_err = wifi_connect(WIFI_COUNTRY, WIFI_SSID, WIFI_PASS, WIFI_AUTH);
    if (wifi_err) {
        printf("wifi: connect failed (err %d)\n", wifi_err);
    }
    wifi_udp_start(8080, handle_wifi_cmd);

    example_ds18b20();
    for (;;);
}
