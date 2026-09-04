#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"

#include "wifi.h"
#include "wifi_secrets.h"
#include "temp_record.h"
#include "temp_store.h"
#include "sd_ring.h"
#include "config_store.h"
#include "label_store.h"
#include "xfer_proto.h"
#include "xfer_session.h"

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

// `table` — the persistent sensor table (see label_store.h) in index
// order, which *is* the wire sensor_id the v1.2 UDP protocol's DATA
// packets use as a 1-byte index instead of the full 8-byte ROM code. The
// index only changes on an explicit registration, never as a side effect
// of a boot's bus scan, so the collector fetches this once after adding a
// sensor (not once per session) to build its id<->label table. If a sensor
// goes bad, there is no in-place removal -- run `format` and rebuild the
// table from scratch (see OPERATIONS.md).
static void handle_table(char *resp, size_t resp_size) {
    size_t off = 0;
    off += snprintf(resp + off, resp_size - off, "table: %d\n",
                     g_temp_num_devs);
    for (int i = 0; i < g_temp_num_devs && off < resp_size; i++) {
        char label[LABEL_STRING_LEN];
        if (!label_store_lookup(g_temp_romcode[i], label, sizeof label)) {
            snprintf(label, sizeof label, "%s", LABEL_PLACEHOLDER);
        }
        off += snprintf(resp + off, resp_size - off, "%d 0x%016llx %s\n", i,
                         (unsigned long long)g_temp_romcode[i], label);
    }
}

// `config get` / `config set <max_retries> <retry_interval_ms>` — the
// v1.2 protocol's receiver-side retry policy. The device only stores this;
// it never times anything out itself (see xfer_proto.h). A receiver reads
// it once at session start and drives its own retry loop.
//
// `config sample <ms>` — the DS18B20 sampling interval. Unlike the retry
// policy, this is read by example_ds18b20() only once, before its loop
// starts, so a change here takes effect on the *next reboot*, not live —
// deliberately, see config_store.h.
static void handle_config(const char *cmd, char *resp, size_t resp_size) {
    unsigned long retries_arg = 0, interval_arg = 0;
    int n = sscanf(cmd, "config set %lu %lu", &retries_arg, &interval_arg);
    if (n == 2) {
        if (config_store_set((uint32_t)retries_arg, (uint32_t)interval_arg)) {
            snprintf(resp, resp_size, "config: set max_retries=%lu "
                     "retry_interval_ms=%lu\n", retries_arg, interval_arg);
        } else {
            snprintf(resp, resp_size,
                     "config: refused — max_retries must be %u..%u, "
                     "retry_interval_ms must be %u..%u (or sd unavailable)\n",
                     CONFIG_MIN_MAX_RETRIES, CONFIG_MAX_MAX_RETRIES,
                     CONFIG_MIN_RETRY_INTERVAL_MS, CONFIG_MAX_RETRY_INTERVAL_MS);
        }
        return;
    }
    unsigned long sample_arg = 0;
    if (sscanf(cmd, "config sample %lu", &sample_arg) == 1) {
        if (config_store_set_sample_interval_ms((uint32_t)sample_arg)) {
            snprintf(resp, resp_size,
                     "config: set sample_interval_ms=%lu — takes effect on "
                     "next reboot\n", sample_arg);
        } else {
            snprintf(resp, resp_size,
                     "config: refused — sample_interval_ms must be %u..%u "
                     "(or sd unavailable)\n",
                     CONFIG_MIN_SAMPLE_INTERVAL_MS, CONFIG_MAX_SAMPLE_INTERVAL_MS);
        }
        return;
    }
    if (strcmp(cmd, "config get") == 0) {
        snprintf(resp, resp_size,
                 "config: max_retries=%lu retry_interval_ms=%lu "
                 "sample_interval_ms=%lu\n",
                 (unsigned long)config_store_max_retries(),
                 (unsigned long)config_store_retry_interval_ms(),
                 (unsigned long)config_store_sample_interval_ms());
        return;
    }
    snprintf(resp, resp_size,
             "usage: config get | config set <max_retries> <retry_interval_ms> "
             "| config sample <ms>\n");
}

// `label <index> <string>` -- renames the location string for the probe
// currently at wire index <index> (as reported by `table`), by resolving
// it to a romcode and updating labels.dat. Only renames an entry that
// already exists -- label_store_register_new() is what creates one, at
// boot, when a romcode is first seen on the bus (see label_store.h).
static void handle_label(const char *cmd, char *resp, size_t resp_size) {
    int idx = -1;
    int consumed = 0;
    if (sscanf(cmd, "label %d %n", &idx, &consumed) != 1 || consumed == 0) {
        snprintf(resp, resp_size, "usage: label <index> <string>\n");
        return;
    }

    const char *raw_label = cmd + consumed;
    size_t len = strlen(raw_label);
    while (len > 0 && (raw_label[len - 1] == ' ' || raw_label[len - 1] == '\r' ||
                        raw_label[len - 1] == '\n')) {
        len--;
    }
    if (len == 0) {
        snprintf(resp, resp_size, "usage: label <index> <string>\n");
        return;
    }
    char label[LABEL_STRING_LEN];
    if (len >= sizeof(label)) {
        snprintf(resp, resp_size, "label: refused -- name too long (max %zu chars)\n",
                 sizeof(label) - 1);
        return;
    }
    memcpy(label, raw_label, len);
    label[len] = '\0';

    if (idx < 0 || idx >= g_temp_num_devs) {
        snprintf(resp, resp_size, "label: refused -- index must be 0..%d\n",
                 g_temp_num_devs - 1);
        return;
    }

    uint64_t romcode = g_temp_romcode[idx];
    if (label_store_set(romcode, label)) {
        snprintf(resp, resp_size, "label: %d (0x%016llx) -> %s\n", idx,
                 (unsigned long long)romcode, label);
    } else {
        snprintf(resp, resp_size,
                 "label: refused -- romcode 0x%016llx not registered yet, "
                 "or sd unavailable\n", (unsigned long long)romcode);
    }
}

// The v1.2 binary protocol (REQUEST/ACK/NACK in, DATA out) is routed by its
// leading magic byte, checked before any ASCII comparison — XFER_MAGIC
// (0xA5) falls outside printable ASCII, so there is no ambiguity either way.
// Every other command today is plain ASCII in and out, so those just
// forward to the existing string-based handlers and measure the reply with
// strlen() at the end.
static void handle_wifi_cmd(const char *cmd, size_t cmd_len, char *resp,
                             size_t resp_size, size_t *resp_len) {
    if (cmd_len >= 1 && (uint8_t)cmd[0] == XFER_MAGIC) {
        xfer_session_handle(cmd, cmd_len, resp, resp_size, resp_len);
        return;
    }
    if (strncmp(cmd, "settime", 7) == 0) {
        handle_settime(cmd, resp, resp_size);
    } else if (strncmp(cmd, "format", 6) == 0) {
        handle_format(cmd, resp, resp_size);
    } else if (strcmp(cmd, "sd") == 0) {
        sd_ring_status(resp, resp_size);
    } else if (strcmp(cmd, "table") == 0) {
        handle_table(resp, resp_size);
    } else if (strncmp(cmd, "label", 5) == 0) {
        handle_label(cmd, resp, resp_size);
    } else if (strncmp(cmd, "config", 6) == 0) {
        handle_config(cmd, resp, resp_size);
    } else if (strcmp(cmd, "read") == 0) {
        if (temp_ring_next_seq() == 0) {
            snprintf(resp, resp_size, "no readings yet\n");
            *resp_len = strlen(resp);
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
    *resp_len = strlen(resp);
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
        // Needs the filesystem sd_ring_init() just mounted; not otherwise
        // related to the ring itself. Missing/invalid config.dat is not an
        // error — config_store_init() seeds in-memory defaults and leaves
        // the file untouched until an explicit `config set`.
        config_store_init();
        label_store_init();
    }

    int wifi_err = wifi_connect(WIFI_COUNTRY, WIFI_SSID, WIFI_PASS, WIFI_AUTH);
    if (wifi_err) {
        printf("wifi: connect failed (err %d)\n", wifi_err);
    }
    wifi_udp_start(8080, handle_wifi_cmd);

    example_ds18b20();
    for (;;);
}
