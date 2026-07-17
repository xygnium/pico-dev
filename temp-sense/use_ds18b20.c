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

// Modify these definitions as required, to match connections.
#define ONEWIRE_GPIO_PIN 15

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

#define DS3231_I2C_PORT i2c1
#define DS3231_I2C_SDA_PIN 26
#define DS3231_I2C_SCL_PIN 27

int example_ds18b20() {
    PIO pio = pio0;

#if 0
    // Create a real-time clock structure and initiate this, used to
    // timestamp each temperature reading.
    struct ds3231_rtc rtc;
    ds3231_init(DS3231_I2C_PORT, DS3231_I2C_SDA_PIN, DS3231_I2C_SCL_PIN,
                &rtc);
    ds3231_datetime_t dt;
    uint8_t dt_str[25];
#endif

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

    while (num_devs > 0) {
        // start temperature conversion in parallel on all devices
        // (see ds18b20 datasheet)
        ow_reset(&ow);
        ow_send(&ow, OW_SKIP_ROM);
        ow_send(&ow, DS18B20_CONVERT_T);

        // wait for the conversions to finish (max 750ms for 12-bit resolution)
        sleep_ms(800);

#if 0
        // timestamp this batch of readings
        ds3231_get_datetime(&dt, &rtc);
        ds3231_ctime(dt_str, sizeof(dt_str), &dt);
#endif

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
            if (ow_crc8(scratchpad, 9) == 0) {
                int16_t temp = scratchpad[0] | (scratchpad[1] << 8);
                double celsius = temp / 16.0;
                double fahrenheit = (celsius * 9.0 / 5.0) + 32.0;
                //printf("%s\tdevice %d: %.2f C (%.2f F)\n", dt_str, i, celsius, fahrenheit);
                printf("device %d: %.2f C (%.2f F)\n", i, celsius, fahrenheit);
            } else {
                printf("device %d: CRC error\n", i);
            }
        }

        sleep_ms(5000);
    }

    return 0;
}
