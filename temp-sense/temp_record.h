#ifndef TEMP_SENSE_TEMP_RECORD_H
#define TEMP_SENSE_TEMP_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "api_ds3231.h"

// The canonical reading. Every output path — serial, UDP, and later SD and
// MQTT — serializes this record rather than formatting sensor data its own
// way, so they cannot drift apart.
typedef struct {
    uint32_t seq;      // monotonic — dedup + gap detection
    uint32_t epoch;    // unix time, UTC (the RTC is kept in UTC by convention)
    uint64_t romcode;  // sensor identity travels with the reading
    int16_t  raw;      // DS18B20 native 1/16 degC; celsius = raw / 16.0
    uint8_t  flags;    // TEMP_FLAG_*
} temp_record_t;

// The fields sum to 19 bytes, but uint64_t romcode forces 8-byte alignment so
// the struct pads to 24 — reordering does not help. Pinned because the ring
// capacity budget below is stated in bytes.
_Static_assert(sizeof(temp_record_t) == 24, "temp_record_t must be 24 bytes");

#define TEMP_FLAG_VALID     0x01u  // raw holds a CRC-checked reading
#define TEMP_FLAG_CRC_ERROR 0x02u  // scratchpad CRC failed; raw is meaningless

static inline double temp_record_celsius(const temp_record_t *rec) {
    return rec->raw / 16.0;
}

// ~1 hour of backlog at 3 sensors every 5s (36 records/min): 2048 * 24 bytes
// = 49KB of the RP2040's 264KB SRAM.
#define TEMP_RING_CAPACITY 2048

/*! \brief Append a reading, assigning it the next sequence number.
 *
 * Overwrites the oldest record once the ring is full — the ring is a backlog
 * for consumers that fall behind, not durable storage.
 *
 * \return the seq assigned to this record
 */
uint32_t temp_ring_push(uint64_t romcode, uint32_t epoch, int16_t raw,
                        uint8_t flags);

/*! \brief Number of records currently buffered (<= TEMP_RING_CAPACITY). */
size_t temp_ring_count(void);

/*! \brief The seq the next push will be given, i.e. the total ever pushed. */
uint32_t temp_ring_next_seq(void);

/*! \brief Fetch a record by seq.
 *
 * \return false if seq has not been written yet, or has already been
 *         overwritten — the caller (e.g. the phase-3 SD replay watermark) can
 *         then tell "not yet" from "dropped" by comparing against
 *         temp_ring_next_seq().
 */
bool temp_ring_get(uint32_t seq, temp_record_t *out);

/*! \brief Fetch the most recent record for one sensor, by ROM code.
 *
 * Keyed by romcode rather than bus-enumeration index: indices shift if a
 * sensor drops off the bus or one is added, which would silently re-map
 * history to different physical sensors.
 */
bool temp_ring_latest_for_rom(uint64_t romcode, temp_record_t *out);

/*! \brief Convert a DS3231 datetime to a unix epoch, treating it as UTC. */
uint32_t temp_epoch_from_datetime(const ds3231_datetime_t *dt);

// 2020-01-01T00:00:00Z. Any RTC reading older than this is taken as "the clock
// was never set, or lost its backup battery" rather than a real timestamp —
// the DS3231 powers up at 2000-01-01, and this firmware did not exist in 2019.
#define TEMP_TIME_PLAUSIBLE_EPOCH 1577836800u

/*! \brief Whether an epoch looks like a real time rather than an unset clock.
 *
 * Guards against the one failure mode the battery-backed RTC still has: if the
 * cell dies, the DS3231 comes back at 2000-01-01 and every record would carry
 * a confidently wrong timestamp. This turns that into a visible error. It is
 * deliberately a floor test, not a drift test — the DS3231 is +/-2ppm (~63
 * s/year), so drift is not a concern at this project's accuracy needs.
 */
bool temp_time_is_plausible(uint32_t epoch);

/*! \brief Format a unix epoch as "YYYY-MM-DD HH:MM:SS" UTC.
 *
 * \param buf_size must be at least 20
 */
void temp_format_epoch(char *buf, size_t buf_size, uint32_t epoch);

#endif
