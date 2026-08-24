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

// `fetch <from_seq> [count]` — read-only pull of records off the SD ring for
// a designated collector to archive durably. This is the read half of the
// UDP pull-and-confirm replacement for MQTT push: mosquitto only ever holds
// the newest retained value per topic and cannot durably ack a delivery, so
// the collector confirms receipt itself via `ack`, below. This command never
// touches the confirm watermark — safe to call regardless of collector
// state. See CLAUDE.md, "Planned: storage & MQTT reporting" for the design.
//
// A from_seq below oldest is not an error: the reply starts at oldest and
// the trailer's oldest field tells the collector how much aged out unacked.
// Records stop short of resp_size — a budget is reserved for the trailer
// before each line is added — so a reply is truncated at a record boundary,
// never mid-line. That precaution is not hypothetical: the MQTT topic-buffer
// bug (see CLAUDE.md) showed a silently truncated snprintf is invisible from
// the device's own log, only visible from the far end.
#define FETCH_LINE_MAX    64  // "<seq> <epoch> 0x<16 hex> <raw> <flags>\n" is ~52; rounded up
#define FETCH_TRAILER_MAX 64  // "end <first> <last> <oldest> <next> <confirmed>\n" is ~59

static void handle_fetch(const char *cmd, char *resp, size_t resp_size) {
    if (!sd_ring_available()) {
        snprintf(resp, resp_size, "fetch: sd not available\n");
        return;
    }

    unsigned long from_arg = 0, count_arg = 0;
    int n = sscanf(cmd, "fetch %lu %lu", &from_arg, &count_arg);
    if (n < 1) {
        snprintf(resp, resp_size, "usage: fetch <from_seq> [count]\n");
        return;
    }
    bool have_limit = (n == 2);
    uint32_t from_seq = (uint32_t)from_arg;
    uint32_t limit = (uint32_t)count_arg;

    uint32_t oldest = sd_ring_oldest_seq();
    uint32_t next = sd_ring_next_seq();
    uint32_t confirmed = sd_ring_confirmed_seq();

    uint32_t cur = (from_seq < oldest) ? oldest : from_seq;
    uint32_t first = cur;
    uint32_t last = first;  // stays == first if nothing gets emitted below
    uint32_t emitted = 0;

    size_t off = 0;
    while (cur < next) {
        if (have_limit && emitted >= limit) {
            break;
        }
        // Reserve room for the trailer before committing to another record
        // line, so running out of space ends the batch cleanly instead of
        // mid-record.
        if (off + FETCH_LINE_MAX + FETCH_TRAILER_MAX > resp_size) {
            break;
        }
        temp_record_t rec;
        if (sd_ring_get(cur, &rec)) {
            // CRC-error records are emitted too, same as `read` — the flags
            // field carries the error, so a flagged record is a visible gap
            // rather than a vanished one.
            off += snprintf(resp + off, resp_size - off,
                            "%lu %lu 0x%016llx %d %u\n",
                            (unsigned long)rec.seq, (unsigned long)rec.epoch,
                            (unsigned long long)rec.romcode, rec.raw,
                            rec.flags);
            last = cur;
            emitted++;
        }
        // A slot that fails sd_ring_get() (stale/corrupt on-SD CRC) is
        // skipped rather than aborting the batch — the seq is still
        // considered serviced so the collector doesn't get stuck re-asking
        // for a slot that will never validate.
        cur++;
    }

    off += snprintf(resp + off, resp_size - off, "end %lu %lu %lu %lu %lu\n",
                     (unsigned long)first, (unsigned long)last,
                     (unsigned long)oldest, (unsigned long)next,
                     (unsigned long)confirmed);
}

// `ack <seq>` — the write half of pull-and-confirm: tells the device every
// record up to and including <seq> is now durably on the collector's own
// disk, so the ring is free to overwrite it. This is the one command in the
// protocol that changes device state, so it is deliberately narrow: it can
// only move the watermark forward, and only up to a seq that has actually
// been written.
//
// The UDP port is LAN-reachable and this has no confirmation token the way
// `format` does — the clamp below *is* the guard, since a forward jump past
// unwritten data would skip records permanently, whereas `format`'s mistake
// (an accidental wipe) is recoverable in the sense that it's at least
// visible immediately.
//
// Idempotent by construction: re-acking the current watermark, or a value
// already behind it that equals it, is accepted as a no-op success — so a
// lost ack reply is safe to retry rather than needing its own error path.
static void handle_ack(const char *cmd, char *resp, size_t resp_size) {
    if (!sd_ring_available()) {
        snprintf(resp, resp_size, "ack: sd not available\n");
        return;
    }

    unsigned long seq_arg = 0;
    if (sscanf(cmd, "ack %lu", &seq_arg) != 1) {
        snprintf(resp, resp_size, "usage: ack <seq>\n");
        return;
    }
    uint32_t seq = (uint32_t)seq_arg;

    uint32_t confirmed = sd_ring_confirmed_seq();
    uint32_t next = sd_ring_next_seq();

    if (seq < confirmed) {
        snprintf(resp, resp_size,
                 "ack: refused — %lu is behind the current watermark %lu "
                 "(would move backwards)\n",
                 (unsigned long)seq, (unsigned long)confirmed);
        return;
    }
    // seq >= next covers "never written" (next==0) too: any seq is >= 0.
    if (seq >= next) {
        snprintf(resp, resp_size,
                 "ack: refused — %lu has not been written yet (next seq is "
                 "%lu)\n",
                 (unsigned long)seq, (unsigned long)next);
        return;
    }

    // ack only ever carried a seq, but sd_ring's watermark is now
    // dual-tracked by (seq, epoch) for the v1.2 protocol's benefit. Look up
    // the epoch this seq actually carries so the two stay in step; if the
    // slot doesn't validate (stale/corrupt CRC — seq is still in-range, just
    // unreadable), leave confirmed_epoch where it was rather than guessing.
    temp_record_t rec;
    uint32_t epoch = sd_ring_get(seq, &rec) ? rec.epoch : sd_ring_confirmed_epoch();
    sd_ring_set_confirmed(seq, epoch);
    snprintf(resp, resp_size, "ack: confirmed %lu\n", (unsigned long)seq);
}

// All commands today are plain ASCII in and out, so this just forwards to
// the existing string-based handlers and measures the reply with strlen()
// at the end. `cmd_len` is unused for now — it exists so a future binary
// command (checked by magic byte, before this ASCII chain) can dispatch
// without relying on NUL-termination.
static void handle_wifi_cmd(const char *cmd, size_t cmd_len, char *resp,
                             size_t resp_size, size_t *resp_len) {
    (void)cmd_len;
    if (strncmp(cmd, "settime", 7) == 0) {
        handle_settime(cmd, resp, resp_size);
    } else if (strncmp(cmd, "format", 6) == 0) {
        handle_format(cmd, resp, resp_size);
    } else if (strcmp(cmd, "sd") == 0) {
        sd_ring_status(resp, resp_size);
    } else if (strncmp(cmd, "fetch", 5) == 0) {
        handle_fetch(cmd, resp, resp_size);
    } else if (strncmp(cmd, "ack", 3) == 0) {
        handle_ack(cmd, resp, resp_size);
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
    }

    int wifi_err = wifi_connect(WIFI_COUNTRY, WIFI_SSID, WIFI_PASS, WIFI_AUTH);
    if (wifi_err) {
        printf("wifi: connect failed (err %d)\n", wifi_err);
    }
    wifi_udp_start(8080, handle_wifi_cmd);

    example_ds18b20();
    for (;;);
}
