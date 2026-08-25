#ifndef TEMP_SENSE_LABEL_STORE_H
#define TEMP_SENSE_LABEL_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Operator-assigned location names for individual DS18B20 probes,
 * persisted in labels.dat (same magic/version/CRC32 pattern as
 * config_store.c's config.dat). Keyed by romcode -- the stable hardware
 * identity -- not by sensor_id, which is only a snapshot of this boot's
 * 1-wire search order (see temp_store.h) and can shift when the roster
 * changes.
 *
 * Growth workflow: connect one new probe, reboot so the 1-wire scan finds
 * it, label_store_register_new() auto-adds it with a placeholder label,
 * then the operator renames it with the `label <index> <string>` command
 * before adding the next probe. The collector is expected to be paused
 * during this process and only re-download the `labels` table once the
 * operator says the roster change is complete.
 */

#define LABEL_MAX_ENTRIES 20  // matches TEMP_STORE_MAX_DEVICES
#define LABEL_STRING_LEN  24  // includes the NUL terminator
#define LABEL_PLACEHOLDER "unlabeled"

/*! \brief Load labels.dat if present and valid; start with an empty table
 *  otherwise, without writing to disk until something changes.
 * \return true if a valid labels.dat was loaded, false if starting empty
 *         (not an error -- a freshly-provisioned device has none yet).
 */
bool label_store_init(void);

// For each romcode in `romcodes` not already in the table, appends a new
// entry with LABEL_PLACEHOLDER and persists once if anything was added.
// A no-op (returns 0) if SD is unavailable or the table is already full --
// any romcodes past LABEL_MAX_ENTRIES just stay unregistered rather than
// failing the whole call.
int label_store_register_new(const uint64_t *romcodes, int count);

// Renames an *existing* entry -- false if romcode isn't already in the
// table (there is no way to invent one for a romcode never seen on the
// bus), `label` is empty or doesn't fit LABEL_STRING_LEN (with NUL), or
// the write failed.
bool label_store_set(uint64_t romcode, const char *label);

int label_store_count(void);

// Iterates [0, label_store_count()). False if index is out of range.
bool label_store_get(int index, uint64_t *romcode, char *out, size_t out_size);

#endif
