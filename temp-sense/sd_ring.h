#ifndef TEMP_SENSE_SD_RING_H
#define TEMP_SENSE_SD_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "temp_record.h"

/*
 * All four staged steps of the SD work are wired into the build and
 * hardware-verified; see the note at the top of sd_ring.c and CLAUDE.md,
 * "Current status".
 *
 * The durable tier: the same ring scheme as the in-RAM buffer
 * (temp_record.h), just far larger and on the SD card. A record lives at a
 * fixed slot, seq % SD_RING_CAPACITY, so lookup by seq is a direct seek
 * rather than a scan, and space is bounded with no rotation or compaction.
 *
 * Every reading is written here; a designated collector pulls a range of
 * seqs back off it over UDP (`fetch`) and advances the confirm watermark
 * once it has durably stored them (`ack`) — see the UDP commands in
 * temp-sense.c. That is what makes a collector or WiFi outage replayable
 * instead of lost.
 */

// On-SD record. Padded to 32 bytes because SD/FatFs sectors are 512 bytes,
// which the in-RAM struct's 24 does not divide — records would straddle
// sector boundaries and every update would become a read-modify-write of two
// sectors. 32 gives exactly 16 records per sector. The spare bytes are not
// waste: they carry a format version and a CRC, so a corrupt sector is
// detectable rather than silently returned as data.
typedef struct {
    uint32_t seq;          // 0
    uint32_t epoch;        // 4
    uint64_t romcode;      // 8
    int16_t  raw;          // 16
    uint8_t  flags;        // 18  TEMP_FLAG_*
    uint8_t  version;      // 19  SD_RECORD_VERSION
    uint32_t reserved[2];  // 20  future fields, zeroed
    uint32_t crc32;        // 28  over bytes [0, 28)
} sd_record_t;

_Static_assert(sizeof(sd_record_t) == 32, "sd_record_t must be 32 bytes");

#define SD_RECORD_VERSION 1u

// 2^21 records x 32 bytes = exactly 64MiB. At 3 sensors every 5s (~51,840
// records/day, ~1.66 MB/day) that is ~40 days — far past any realistic broker
// outage, and stopping at half of the 128MB card on hand leaves room for a
// config file later. A power of two matters: cortex-m0plus has no hardware
// divide, so seq % capacity compiles to a mask rather than a division call.
#define SD_RING_CAPACITY (1u << 21)
#define SD_RING_BYTES    ((uint64_t)SD_RING_CAPACITY * sizeof(sd_record_t))

/*! \brief Bring up the card: init driver, mount, open and size the ring.
 *
 * Never panics and never formats. A missing or unmountable card leaves the
 * logger running on the RAM ring and UDP alone — see g_sd_ready. Recovers the
 * sequence high-water mark so seq stays monotonic across reboots.
 *
 * \param boot_dt the DS3231's current time, used to seed the RP2040's internal
 *        clock so FatFs stamps files with a real date (FF_FS_NORTC is 0, so
 *        FatFs asks the internal RTC, which otherwise boots at year 0). Must
 *        be seeded here rather than by the caller: this function calls the
 *        library's time_init(), which calls rtc_init() and would reset any
 *        earlier setting. Pass NULL to skip (e.g. when the RTC is implausible).
 * \return true if the ring is open and writable
 */
bool sd_ring_init(const ds3231_datetime_t *boot_dt);

/*! \brief Whether the ring is usable. Mirrors g_sd_ready. */
bool sd_ring_available(void);

/*! \brief Write one record to its slot. Does not flush; see sd_ring_sync(). */
bool sd_ring_put(const temp_record_t *rec);

/*! \brief Read a record back by seq.
 *
 * \return false if that seq was never written, has been overwritten, or the
 *         stored CRC does not match.
 */
bool sd_ring_get(uint32_t seq, temp_record_t *out);

/*! \brief The seq the next put will use, i.e. one past the newest on the card. */
uint32_t sd_ring_next_seq(void);

/*! \brief The oldest seq still on the card (older ones have been overwritten). */
uint32_t sd_ring_oldest_seq(void);

/*! \brief The confirm watermark: the last seq the collector has durably
 *  stored (i.e. ack'd — see the `ack` UDP command in temp-sense.c).
 *
 * Unambiguous under pull-and-confirm: a seq <= this is safe to let the ring
 * overwrite, because the collector already has it on its own disk. (An
 * earlier MQTT-push design had this mean "last considered" rather than "last
 * published", to survive publish-side thinning that never got built — that
 * distinction no longer applies now that the collector, not the device,
 * decides what's durable.)
 */
uint32_t sd_ring_confirmed_seq(void);
void     sd_ring_set_confirmed(uint32_t seq);

/*! \brief Flush pending record writes, and persist metadata if it is due.
 *
 * Call once per sensor cycle. Metadata is written lazily rather than on every
 * update — see sd_ring.c for why that is safe.
 */
void sd_ring_sync(void);

/*! \brief Create a fresh filesystem on the card, destroying all data.
 *
 * Exposed for the deliberate `format` command only. Re-runs sd_ring_init()
 * afterwards, so the ring is preallocated and ready on return.
 */
bool sd_ring_format(void);

/*! \brief One-line-per-item human readable state, for the `sd` UDP command. */
void sd_ring_status(char *buf, size_t buf_size);

#endif
