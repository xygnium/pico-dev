/*
 * In-RAM ring buffer of temp_record_t, plus the epoch <-> civil-time
 * conversions that let records store a 4-byte timestamp instead of a
 * formatted string.
 */

#include <stdio.h>

#include "temp_record.h"

static temp_record_t ring[TEMP_RING_CAPACITY];
static uint32_t next_seq = 0;  // also the total ever pushed

// seq maps to a fixed slot (seq % capacity), so a record's position never
// moves and no head index is needed.
static inline size_t slot_of(uint32_t seq) {
    return (size_t)(seq % TEMP_RING_CAPACITY);
}

// The oldest seq still buffered.
static inline uint32_t oldest_seq(void) {
    return next_seq > TEMP_RING_CAPACITY ? next_seq - TEMP_RING_CAPACITY : 0;
}

uint32_t temp_ring_push(uint64_t romcode, uint32_t epoch, int16_t raw,
                        uint8_t flags) {
    uint32_t seq = next_seq++;
    ring[slot_of(seq)] = (temp_record_t){
        .seq     = seq,
        .epoch   = epoch,
        .romcode = romcode,
        .raw     = raw,
        .flags   = flags,
    };
    return seq;
}

size_t temp_ring_count(void) {
    return next_seq < TEMP_RING_CAPACITY ? next_seq : TEMP_RING_CAPACITY;
}

uint32_t temp_ring_next_seq(void) {
    return next_seq;
}

bool temp_ring_get(uint32_t seq, temp_record_t *out) {
    if (seq >= next_seq || seq < oldest_seq()) {
        return false;
    }
    *out = ring[slot_of(seq)];
    return true;
}

bool temp_ring_latest_for_rom(uint64_t romcode, temp_record_t *out) {
    uint32_t oldest = oldest_seq();
    // Post-decrement in the condition, so the body sees seq in
    // [oldest, next_seq-1] and the loop still terminates at oldest == 0
    // without seq underflowing inside it.
    for (uint32_t seq = next_seq; seq-- > oldest; ) {
        const temp_record_t *rec = &ring[slot_of(seq)];
        if (rec->romcode == romcode) {
            *out = *rec;
            return true;
        }
    }
    return false;
}

/*
 * Days between 1970-01-01 and y-m-d, proleptic Gregorian (Howard Hinnant's
 * days_from_civil). Used instead of mktime() because that applies the C
 * library's local timezone, and these datetimes are UTC by convention.
 */
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d) {
    y -= (m <= 2);
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);              // [0, 399]
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  // [0, 146096]
    return era * 146097 + (int32_t)doe - 719468;
}

// Inverse of the above (civil_from_days).
static void civil_from_days(int32_t z, int32_t *y, uint32_t *m, uint32_t *d) {
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);           // [0, 146096]
    const uint32_t yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;   // [0, 399]
    const int32_t yr = (int32_t)yoe + era * 400;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;                     // [0, 11]
    *d = doy - (153 * mp + 2) / 5 + 1;                           // [1, 31]
    *m = mp + (mp < 10 ? 3 : -9);                                // [1, 12]
    *y = yr + (*m <= 2);
}

uint32_t temp_epoch_from_datetime(const ds3231_datetime_t *dt) {
    int32_t days = days_from_civil(dt->year, dt->month, dt->day);
    return (uint32_t)days * 86400u +
           dt->hour * 3600u + dt->minutes * 60u + dt->seconds;
}

bool temp_time_is_plausible(uint32_t epoch) {
    return epoch >= TEMP_TIME_PLAUSIBLE_EPOCH;
}

void temp_format_epoch(char *buf, size_t buf_size, uint32_t epoch) {
    int32_t days = (int32_t)(epoch / 86400u);
    uint32_t secs = epoch % 86400u;

    int32_t year;
    uint32_t month, day;
    civil_from_days(days, &year, &month, &day);

    snprintf(buf, buf_size, "%04ld-%02lu-%02lu %02lu:%02lu:%02lu",
             (long)year, (unsigned long)month, (unsigned long)day,
             (unsigned long)(secs / 3600), (unsigned long)(secs % 3600 / 60),
             (unsigned long)(secs % 60));
}
