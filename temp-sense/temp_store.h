#ifndef TEMP_SENSE_TEMP_STORE_H
#define TEMP_SENSE_TEMP_STORE_H

#include <stdint.h>
#include <stdbool.h>

#include "api_ds3231.h"

#define TEMP_STORE_MAX_DEVICES 10

// The sensors found by the 1-Wire bus scan, in enumeration order. This is the
// *roster* only — readings themselves live in the ring buffer (temp_record.h)
// and are keyed by ROM code, so a sensor appearing or dropping off shifts
// these indices without re-mapping any history.
extern int g_temp_num_devs;
extern uint64_t g_temp_romcode[TEMP_STORE_MAX_DEVICES];

// The DS3231 handle, owned by use_ds18b20.c. Exposed so temp-sense.c's
// `settime` command can set the clock. g_rtc_ready guards against use
// before ds3231_init() has run.
extern ds3231_rtc_t g_rtc;
extern bool g_rtc_ready;

// False when the RTC is reporting an implausible time (see
// temp_time_is_plausible()) — i.e. the clock was never set or lost its backup
// battery, so every timestamp is wrong. Re-evaluated on each sensor cycle, so
// it clears itself once `settime` has been run; no need for settime to touch
// it. Surfaced in the serial log and in the `read` reply, since a headless
// logger has no other way to report it.
extern bool g_rtc_time_valid;

#endif
