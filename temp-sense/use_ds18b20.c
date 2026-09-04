/*
 * 1-Wire bus driving and DS18B20 protocol adapted from the Raspberry Pi
 * pico-examples repository (pio/onewire), Copyright (c) 2023 mjcross,
 * SPDX-License-Identifier: BSD-3-Clause.
 *
 * This example illustrates reading one or more DS18B20 1-Wire temperature
 * sensors and timestamping each reading with a DS3231 RTC, combining the
 * onewire_library testbed with the api_ds3231 testbed from ../rtc/.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/i2c.h"

#include "onewire_library.h"    // onewire library functions
#include "ow_rom.h"             // onewire ROM command codes
#include "ds18b20.h"            // ds18b20 function codes

#include "api_ds3231.h"
#include "wifi.h"
#include "temp_record.h"
#include "temp_store.h"
#include "sd_ring.h"
#include "label_store.h"

// Modify these definitions as required, to match connections.
#define ONEWIRE_GPIO_PIN 15

// Sensor cadence and network responsiveness are deliberately independent
// timers. Previously wifi_udp_poll() ran once per sensor cycle and the loop
// then sat in a flat sleep_ms(5000), so a command arriving just after that one
// poll point waited nearly a full cycle to be answered — measured on hardware
// as a bimodal 0.6s / 5.9s response depending only on when the packet landed.
// Sample rate and command latency are unrelated concerns, and coupling them
// means every future increase to the sample interval (30s is planned — see
// CLAUDE.md, "Deferred") would degrade responsiveness by the same factor.
#define TEMP_SAMPLE_INTERVAL_MS 5000  // sensor cadence; the one knob to change
#define NET_POLL_INTERVAL_MS      50  // bounds worst-case UDP command latency
#define DS18B20_CONVERT_MS       800  // 12-bit conversion time, per datasheet

int g_temp_num_devs = 0;
uint64_t g_temp_romcode[TEMP_STORE_MAX_DEVICES];
bool g_temp_sampling_paused = false;

// Mirrors the OW handle set up once in example_ds18b20() -- a plain value
// struct (pio/sm/offset/gpio, see onewire_library.h), safe to copy -- so
// temp_store_rediscover() can drive a bus scan after boot without needing
// its own PIO state machine.
static OW s_ow;
static bool s_ow_ready = false;

void temp_store_sync_from_table(void) {
    int count = label_store_count();
    if (count > TEMP_STORE_MAX_DEVICES) count = TEMP_STORE_MAX_DEVICES;
    for (int i = 0; i < count; i++) {
        char label[LABEL_STRING_LEN];
        label_store_get(i, &g_temp_romcode[i], label, sizeof label);
    }
    g_temp_num_devs = count;
}

void temp_store_rediscover(void) {
    if (!s_ow_ready) return;
    uint64_t romcode[TEMP_STORE_MAX_DEVICES];
    int num_devs = ow_romsearch(&s_ow, romcode, TEMP_STORE_MAX_DEVICES,
                                 OW_SEARCH_ROM);
    label_store_register_new(romcode, num_devs);
    temp_store_sync_from_table();
}

// Service the network until `deadline`. Used for both the DS18B20 conversion
// wait and the idle time between samples, so there is no window anywhere in
// the loop where commands sit unanswered — that is what makes the two timers
// genuinely independent rather than merely separately named.
static void net_poll_until(absolute_time_t deadline) {
    for (;;) {
        wifi_udp_poll();
        if (time_reached(deadline)) {
            return;
        }
        sleep_ms(NET_POLL_INTERVAL_MS);
    }
}

ds3231_rtc_t g_rtc;
bool g_rtc_ready = false;
bool g_rtc_time_valid = false;

// Dallas 1-Wire CRC-8 validation
static uint8_t ow_crc8(uint8_t *data, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int j = 0; j < 8; j++) {
            uint8_t bit = (byte ^ crc) & 1;
            crc >>= 1;
            if (bit) crc ^= 0x8C;
            byte >>= 1;
        }
    }
    return crc;
}

#define DS3231_I2C_PORT i2c0
#define DS3231_I2C_SDA_PIN 8
#define DS3231_I2C_SCL_PIN 9

int example_ds18b20() {
    PIO pio = pio0;

    // Initialise the real-time clock used to timestamp each temperature
    // reading. g_rtc is a global (see temp_store.h) so the `settime` WiFi
    // command in temp-sense.c can reach it too.
    ds3231_init(DS3231_I2C_PORT, DS3231_I2C_SDA_PIN, DS3231_I2C_SCL_PIN,
                &g_rtc);
    g_rtc_ready = true;
    ds3231_datetime_t dt;

    // Sanity-check the clock once at boot. The DS3231's backup cell normally
    // carries the time across power cycles for years, but if it dies the chip
    // comes back at 2000-01-01 and would otherwise log confidently wrong
    // timestamps forever. Warn loudly rather than failing: the temperature
    // readings are still useful, and the operator may not be watching.
    ds3231_get_datetime(&dt, &g_rtc);
    uint32_t boot_epoch = temp_epoch_from_datetime(&dt);
    g_rtc_time_valid = temp_time_is_plausible(boot_epoch);
    {
        char boot_ts[20];
        temp_format_epoch(boot_ts, sizeof(boot_ts), boot_epoch);
        if (g_rtc_time_valid) {
            printf("rtc: %s UTC\n", boot_ts);
        } else {
            printf("\n*** rtc: clock not set (reads %s UTC) ***\n"
                   "*** timestamps will be wrong until you run: "
                   "settime YYYY-MM-DD HH:MM:SS D (UTC) ***\n\n", boot_ts);
        }
    }

    // add the onewire program to the PIO shared address space
    if (!pio_can_add_program(pio, &onewire_program)) {
        puts("could not add the onewire PIO program");
        return -1;
    }
    uint offset = pio_add_program(pio, &onewire_program);

    // claim a state machine and initialise a driver instance
    OW ow;
    if (!ow_init(&ow, pio, offset, ONEWIRE_GPIO_PIN)) {
        puts("could not initialise the onewire driver");
        return -1;
    }
    s_ow = ow;
    s_ow_ready = true;

    // find and display 64-bit device addresses
    const int maxdevs = 20;
    uint64_t romcode[maxdevs];
    int num_devs = ow_romsearch(&ow, romcode, maxdevs, OW_SEARCH_ROM);

    printf("Found %d DS18B20 device(s)\n", num_devs);
    for (int i = 0; i < num_devs; i += 1) {
        printf("\t%d: 0x%llx\n", i, romcode[i]);
    }

    // Auto-register any romcode not already in labels.dat, with a
    // placeholder name -- the operator renames it later with the `label
    // <index> <string>` command, one newly-added probe at a time.
    int new_labels = label_store_register_new(romcode, num_devs);
    if (new_labels > 0) {
        printf("labels: registered %d new sensor(s) as \"%s\", "
               "use `label <index> <string>` to rename\n",
               new_labels, LABEL_PLACEHOLDER);
    }

    // The sampling roster comes from the persistent table, not directly
    // from this scan -- a table entry not found on the bus just now stays
    // on the roster and is sampled as invalid each cycle (see the read
    // loop below) rather than disappearing until the next reboot.
    temp_store_sync_from_table();

    // Anchors the sample schedule. Deadlines are advanced from the previous
    // deadline rather than from "now", so conversion and read time do not
    // accumulate into the cadence — the old loop slept 5000ms *after* ~900ms
    // of work, giving a real period of ~5.9s rather than the nominal 5s.
    absolute_time_t next_sample = get_absolute_time();

    // Tracks whether the *previous* iteration was idle, so sampling resumes
    // from "now" (not a stale deadline) the moment it's no longer idle,
    // instead of bursting through cycles missed while paused/empty.
    bool was_idle = true;

    // Runs forever (not "while there are devices") so `pause`/`resume` and
    // other commands stay reachable even with an empty table — main()'s
    // only other option after this returns is a dead `for (;;);` with no
    // network servicing at all.
    for (;;) {
        // g_temp_num_devs and g_temp_sampling_paused are re-read every
        // iteration (not cached) since commands handled between cycles
        // change them.
        if (g_temp_sampling_paused || g_temp_num_devs == 0) {
            was_idle = true;
            net_poll_until(make_timeout_time_ms(100));
            continue;
        }
        if (was_idle) {
            next_sample = get_absolute_time();
            was_idle = false;
        }

        // start temperature conversion in parallel on all devices
        // (see ds18b20 datasheet)
        ow_reset(&ow);
        ow_send(&ow, OW_SKIP_ROM);
        ow_send(&ow, DS18B20_CONVERT_T);

        // Wait for the conversions to finish (max 750ms for 12-bit
        // resolution), servicing the network throughout instead of going
        // deaf for 800ms.
        net_poll_until(make_timeout_time_ms(DS18B20_CONVERT_MS));

        // Timestamp this batch of readings. Stored as an epoch in each
        // record and formatted only here, at the serial-print boundary.
        ds3231_get_datetime(&dt, &g_rtc);
        uint32_t epoch = temp_epoch_from_datetime(&dt);
        char ts[20];
        temp_format_epoch(ts, sizeof(ts), epoch);

        // Re-checked every cycle rather than only at boot, so that running
        // `settime` clears the warning on its own.
        g_rtc_time_valid = temp_time_is_plausible(epoch);

        // read the result from each device on the persistent table's
        // roster -- including one not found in this boot's search above,
        // whose ow_reset() presence check below will just come back false
        // every cycle until it's reconnected or decommissioned.
        for (int i = 0; i < g_temp_num_devs; i += 1) {
            uint64_t target = g_temp_romcode[i];
            bool present = ow_reset(&ow);
            bool ok = false;
            int16_t raw = 0;
            if (present) {
                ow_send(&ow, OW_MATCH_ROM);
                for (int b = 0; b < 64; b += 8) {
                    ow_send(&ow, target >> b);
                }
                ow_send(&ow, DS18B20_READ_SCRATCHPAD);

                // read all 9 scratchpad bytes (temp LSB, temp MSB, TH, TL, config, res, res, res, CRC)
                uint8_t scratchpad[9];
                for (int b = 0; b < 9; b++) {
                    scratchpad[b] = ow_read(&ow);
                }

                // validate CRC (should be 0 if calculation is correct)
                // A failed read is recorded too, flagged rather than dropped, so
                // the log shows the gap instead of silently skipping a sample.
                ok = (ow_crc8(scratchpad, 9) == 0);
                raw = ok ? (int16_t)(scratchpad[0] | (scratchpad[1] << 8)) : 0;
            }
            uint8_t flags = ok ? TEMP_FLAG_VALID : TEMP_FLAG_CRC_ERROR;
            uint32_t seq = temp_ring_push(target, epoch, raw, flags);

            // SD is the durable tier; every reading lands there too, not just
            // in the RAM backlog. sd_ring_put() is a no-op (returns false) if
            // the card isn't ready, so this is safe whether or not SD came up.
            temp_record_t rec = {
                .seq = seq, .epoch = epoch, .romcode = target,
                .raw = raw, .flags = flags,
            };
            sd_ring_put(&rec);

            // Timestamp is UTC — the RTC is deliberately set to UTC so it
            // needs no DST adjustment; label it so serial logs aren't
            // mistaken for local wall-clock time.
            if (ok) {
                double celsius = raw / 16.0;
                double fahrenheit = (celsius * 9.0 / 5.0) + 32.0;
                printf("%s UTC\tseq %lu\t0x%016llx: %.2f C (%.2f F)\n",
                       ts, (unsigned long)seq, target, celsius, fahrenheit);
            } else {
                printf("%s UTC\tseq %lu\t0x%016llx: %s\n",
                       ts, (unsigned long)seq, target,
                       present ? "CRC error" : "no response");
            }
        }

        // Once per sensor cycle, per sd_ring_sync()'s contract — tied to the
        // sample timer, not the network tick. Syncing more often than a sector
        // fills is a deliberately deferred optimisation (see CLAUDE.md); this
        // is the simple, correct baseline.
        sd_ring_sync();

        next_sample = delayed_by_ms(next_sample, TEMP_SAMPLE_INTERVAL_MS);
        if (time_reached(next_sample)) {
            // The cycle overran its own interval (a slow SD sync, or a sample
            // interval set shorter than the conversion time). Resynchronise
            // rather than firing back-to-back catch-up samples, which would
            // burst-fill the ring trying to make up time it can never recover.
            next_sample = make_timeout_time_ms(TEMP_SAMPLE_INTERVAL_MS);
        }

        // Idle until the next sample is due — still answering commands and
        // driving MQTT reconnects the whole time.
        net_poll_until(next_sample);
    }

    return 0;
}
