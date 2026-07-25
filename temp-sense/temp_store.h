#ifndef TEMP_SENSE_TEMP_STORE_H
#define TEMP_SENSE_TEMP_STORE_H

#include <stdint.h>
#include <stdbool.h>

#define TEMP_STORE_MAX_DEVICES 10

// Latest batch of DS18B20 readings, refreshed once per read cycle in
// use_ds18b20.c and read by temp-sense.c's WiFi command handler.
extern int g_temp_num_devs;
extern uint64_t g_temp_romcode[TEMP_STORE_MAX_DEVICES];
extern double g_temp_celsius[TEMP_STORE_MAX_DEVICES];
extern bool g_temp_valid[TEMP_STORE_MAX_DEVICES];
extern char g_temp_timestamp[25];

#endif
