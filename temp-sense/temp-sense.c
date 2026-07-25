#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "wifi.h"
#include "wifi_secrets.h"
#include "temp_store.h"

extern int example_ds18b20();

// `settime YYYY-MM-DD HH:MM:SS D` — D is day-of-week 1..7, 1=Monday, as
// api_ds3231.h defines it. The client sends already-broken-down local time
// so the Pico needs no timezone handling; see udp_client.py.
//
// Runs in main-loop context (wifi_udp_poll() is called from the sensor
// loop, not from the lwIP receive callback), so blocking I2C here is safe.
static void handle_settime(const char *cmd, char *resp, size_t resp_size) {
    int year, month, day, hour, minute, second, dotw;

    if (sscanf(cmd, "settime %d-%d-%d %d:%d:%d %d",
               &year, &month, &day, &hour, &minute, &second, &dotw) != 7) {
        snprintf(resp, resp_size,
                 "usage: settime YYYY-MM-DD HH:MM:SS D  (D=1..7, 1=Monday)\n");
        return;
    }
    if (!g_rtc_ready) {
        snprintf(resp, resp_size, "settime: rtc not initialised yet\n");
        return;
    }
    if (year < 2000 || year > 2099 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59 ||
        dotw < 1 || dotw > 7) {
        snprintf(resp, resp_size, "settime: value out of range\n");
        return;
    }

    ds3231_datetime_t dt = {
        .year    = (uint16_t)year,
        .month   = (uint8_t)month,
        .day     = (uint8_t)day,
        .dotw    = (uint8_t)dotw,
        .hour    = (uint8_t)hour,
        .minutes = (uint8_t)minute,
        .seconds = (uint8_t)second,
    };
    ds3231_set_datetime(&dt, &g_rtc);

    // Read back, so the reply reflects what the RTC actually holds rather
    // than what we asked for.
    ds3231_datetime_t check;
    char check_str[25];
    ds3231_get_datetime(&check, &g_rtc);
    ds3231_ctime(check_str, sizeof(check_str), &check);
    snprintf(resp, resp_size, "rtc set: %s\n", check_str);
}

static void handle_wifi_cmd(const char *cmd, char *resp, size_t resp_size) {
    if (strncmp(cmd, "settime", 7) == 0) {
        handle_settime(cmd, resp, resp_size);
    } else if (strcmp(cmd, "read") == 0) {
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
