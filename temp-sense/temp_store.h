#ifndef TEMP_SENSE_TEMP_STORE_H
#define TEMP_SENSE_TEMP_STORE_H

#include <stdint.h>
#include <stdbool.h>

#include "api_ds3231.h"

#define TEMP_STORE_MAX_DEVICES 20

// The sampling roster, mirrored from the persistent sensor table
// (label_store.h) rather than derived from a 1-Wire bus scan directly — see
// temp_store_sync_from_table(). Readings themselves live in the ring buffer
// (temp_record.h) and are keyed by ROM code, so this roster can be
// refreshed (on decomm, wipe, or a new registration) without re-mapping any
// history.
extern int g_temp_num_devs;
extern uint64_t g_temp_romcode[TEMP_STORE_MAX_DEVICES];

// Refreshes g_temp_num_devs/g_temp_romcode[] from the current label_store
// table. Call after anything that changes the table's contents or ordering
// (boot-time registration, `decomm`, table wipe) so the sampling loop and
// the `table`/`read` commands see the new roster immediately.
void temp_store_sync_from_table(void);

// Runs a fresh 1-Wire SEARCH ROM, registers any newly-found romcode, and
// calls temp_store_sync_from_table(). Used by the `wipetable` command to
// repopulate the table immediately after a wipe, without requiring a
// reboot. A no-op if called before the bus driver is initialized (i.e.
// before example_ds18b20() has run).
void temp_store_rediscover(void);

// Set by the `pause`/`resume` commands. While true, the sample loop does
// not read sensors or push ring records, so the ring's backlog can reach
// exactly zero -- the precondition `decomm`/`wipetable` require, since
// they change the table's sensor count and the ring's replay
// (xfer_session.c's build_data_packet) assumes a constant count across any
// unconfirmed backlog. Sampling resumes cleanly from "now" rather than
// bursting through missed cycles (see the sample loop in use_ds18b20.c).
extern bool g_temp_sampling_paused;

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
