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

// Modify these definitions as required, to match connections.
#define ONEWIRE_GPIO_PIN 15

int g_temp_num_devs = 0;
uint64_t g_temp_romcode[TEMP_STORE_MAX_DEVICES];

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

    // find and display 64-bit device addresses
    const int maxdevs = 10;
    uint64_t romcode[maxdevs];
    int num_devs = ow_romsearch(&ow, romcode, maxdevs, OW_SEARCH_ROM);

    printf("Found %d DS18B20 device(s)\n", num_devs);
    for (int i = 0; i < num_devs; i += 1) {
        printf("\t%d: 0x%llx\n", i, romcode[i]);
    }

    g_temp_num_devs = num_devs;
    for (int i = 0; i < num_devs; i += 1) {
        g_temp_romcode[i] = romcode[i];
    }

    while (num_devs > 0) {
        // start temperature conversion in parallel on all devices
        // (see ds18b20 datasheet)
        ow_reset(&ow);
        ow_send(&ow, OW_SKIP_ROM);
        ow_send(&ow, DS18B20_CONVERT_T);

        // wait for the conversions to finish (max 750ms for 12-bit resolution)
        sleep_ms(800);

        // Timestamp this batch of readings. Stored as an epoch in each
        // record and formatted only here, at the serial-print boundary.
        ds3231_get_datetime(&dt, &g_rtc);
        uint32_t epoch = temp_epoch_from_datetime(&dt);
        char ts[20];
        temp_format_epoch(ts, sizeof(ts), epoch);

        // Re-checked every cycle rather than only at boot, so that running
        // `settime` clears the warning on its own.
        g_rtc_time_valid = temp_time_is_plausible(epoch);

        // read the result from each device
        for (int i = 0; i < num_devs; i += 1) {
            ow_reset(&ow);
            ow_send(&ow, OW_MATCH_ROM);
            for (int b = 0; b < 64; b += 8) {
                ow_send(&ow, romcode[i] >> b);
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
            bool ok = (ow_crc8(scratchpad, 9) == 0);
            int16_t raw = ok ? (int16_t)(scratchpad[0] | (scratchpad[1] << 8))
                             : 0;
            uint32_t seq = temp_ring_push(romcode[i], epoch, raw,
                                          ok ? TEMP_FLAG_VALID
                                             : TEMP_FLAG_CRC_ERROR);

            // Timestamp is UTC — the RTC is deliberately set to UTC so it
            // needs no DST adjustment; label it so serial logs aren't
            // mistaken for local wall-clock time.
            if (ok) {
                double celsius = raw / 16.0;
                double fahrenheit = (celsius * 9.0 / 5.0) + 32.0;
                printf("%s UTC\tseq %lu\t0x%016llx: %.2f C (%.2f F)\n",
                       ts, (unsigned long)seq, romcode[i], celsius, fahrenheit);
            } else {
                printf("%s UTC\tseq %lu\t0x%016llx: CRC error\n",
                       ts, (unsigned long)seq, romcode[i]);
            }
        }

        wifi_udp_poll();
        sleep_ms(5000);
    }

    return 0;
}
