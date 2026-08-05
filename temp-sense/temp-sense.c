#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "wifi.h"
#include "wifi_secrets.h"
#include "temp_record.h"
#include "temp_store.h"
#include "sd_ring.h"

extern int example_ds18b20();

// `settime YYYY-MM-DD HH:MM:SS D` — D is day-of-week 1..7, 1=Monday, as
// api_ds3231.h defines it. The client sends already-broken-down time so the
// Pico needs no timezone handling; see udp_client.py.
//
// The clock is kept in **UTC** by convention: the DS3231 has no timezone or
// DST rules, so a local-time clock would sit an hour wrong after every DST
// transition until someone re-ran this command. Nothing here enforces the
// convention — the RTC stores whatever digits it is sent — so every reported
// timestamp is labelled UTC to keep it visible.
//
// Runs in main-loop context (wifi_udp_poll() is called from the sensor
// loop, not from the lwIP receive callback), so blocking I2C here is safe.
static void handle_settime(const char *cmd, char *resp, size_t resp_size) {
    int year, month, day, hour, minute, second, dotw;

    if (sscanf(cmd, "settime %d-%d-%d %d:%d:%d %d",
               &year, &month, &day, &hour, &minute, &second, &dotw) != 7) {
        snprintf(resp, resp_size,
                 "usage: settime YYYY-MM-DD HH:MM:SS D  "
                 "(D=1..7, 1=Monday; send UTC)\n");
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
    char check_str[20];
    ds3231_get_datetime(&check, &g_rtc);
    temp_format_epoch(check_str, sizeof(check_str),
                      temp_epoch_from_datetime(&check));
    snprintf(resp, resp_size, "rtc set: %s UTC\n", check_str);
}

// `format` destroys everything on the SD card, and the UDP port is reachable
// by anything on the LAN — so it requires the exact confirmation token
// rather than the bare word, to keep a stray or mistyped `format` from
// wiping the log. udp_client.py also prompts before sending the real token.
#define FORMAT_CONFIRM_CMD "format yes-erase-the-card"

static void handle_format(const char *cmd, char *resp, size_t resp_size) {
    if (strcmp(cmd, FORMAT_CONFIRM_CMD) != 0) {
        snprintf(resp, resp_size,
                 "format: refused — this destroys all data on the SD card. "
                 "Send exactly \"%s\" to confirm.\n", FORMAT_CONFIRM_CMD);
        return;
    }
    if (sd_ring_format()) {
        snprintf(resp, resp_size, "sd: format complete, ring ready, next seq %lu\n",
                 (unsigned long)sd_ring_next_seq());
    } else {
        snprintf(resp, resp_size, "sd: format failed — check the serial log\n");
    }
}

static void handle_wifi_cmd(const char *cmd, char *resp, size_t resp_size) {
    if (strncmp(cmd, "settime", 7) == 0) {
        handle_settime(cmd, resp, resp_size);
    } else if (strncmp(cmd, "format", 6) == 0) {
        handle_format(cmd, resp, resp_size);
    } else if (strcmp(cmd, "sd") == 0) {
        sd_ring_status(resp, resp_size);
    } else if (strcmp(cmd, "read") == 0) {
        if (temp_ring_next_seq() == 0) {
            snprintf(resp, resp_size, "no readings yet\n");
            return;
        }
        // One line per sensor, keyed by ROM code and carrying its own
        // timestamp — so a sensor that has stopped reporting shows as stale
        // rather than hiding behind a batch header.
        size_t off = 0;
        // Lead with the clock warning if it applies: the timestamps below are
        // meaningless without it, and serial output alone would not reach
        // whoever is querying over the network.
        if (!g_rtc_time_valid) {
            off += snprintf(resp + off, resp_size - off,
                            "warning: rtc not set — timestamps are wrong "
                            "(run settime)\n");
        }
        for (int i = 0; i < g_temp_num_devs && off < resp_size; i++) {
            temp_record_t rec;
            if (!temp_ring_latest_for_rom(g_temp_romcode[i], &rec)) {
                continue;
            }
            char ts[20];
            temp_format_epoch(ts, sizeof(ts), rec.epoch);
            if (rec.flags & TEMP_FLAG_VALID) {
                off += snprintf(resp + off, resp_size - off,
                                "0x%016llx  %s UTC  seq %lu  %.2f C\n",
                                rec.romcode, ts, (unsigned long)rec.seq,
                                temp_record_celsius(&rec));
            } else {
                off += snprintf(resp + off, resp_size - off,
                                "0x%016llx  %s UTC  seq %lu  CRC error\n",
                                rec.romcode, ts, (unsigned long)rec.seq);
            }
        }
    } else {
        snprintf(resp, resp_size, "temp-sense ack: %s", cmd);
    }
}

int main() {
    stdio_init_all();
    printf("Hello, world!\n");

    // Bring up the SD ring early, before the WiFi connect retries, so its
    // output is not buried in the boot log. NULL: the DS3231 hasn't been read
    // yet at this point in boot (that happens in example_ds18b20() below), so
    // FatFs file timestamps fall back to the RP2040's un-seeded internal
    // clock for now — cosmetic only, doesn't affect ring data. Timed because
    // step 2's open question is how slow first-boot preallocation is.
    uint32_t sd_init_start = to_ms_since_boot(get_absolute_time());
    bool sd_ok = sd_ring_init(NULL);
    uint32_t sd_init_ms = to_ms_since_boot(get_absolute_time()) - sd_init_start;
    printf("sd: ring_init took %lu ms (%s)\n",
           (unsigned long)sd_init_ms, sd_ok ? "ok" : "failed");

    // Seed the RAM ring's counter from the SD ring's recovered high-water
    // mark, before any temp_ring_push() happens (example_ds18b20() below is
    // the first caller) — otherwise seq would restart at 0 every boot while
    // the SD ring keeps counting, and newly-pushed records would collide with
    // slots the SD ring already considers occupied.
    if (sd_ok) {
        temp_ring_set_next_seq(sd_ring_next_seq());
    }

    int wifi_err = wifi_connect(WIFI_COUNTRY, WIFI_SSID, WIFI_PASS, WIFI_AUTH);
    if (wifi_err) {
        printf("wifi: connect failed (err %d)\n", wifi_err);
    }
    wifi_udp_start(8080, handle_wifi_cmd);

    example_ds18b20();
    for (;;);
}
