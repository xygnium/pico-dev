/*
 * SD-backed durable ring for temp_record_t.
 *
 * All four staged steps (see CLAUDE.md, "Current status") are wired in and
 * hardware-verified: sd_ring_init() runs from temp-sense.c's main()
 * (superseding sd_probe.c, which stays on disk out of the build as a simpler
 * fallback smoke test); sd_ring_put()/sd_ring_sync() run from
 * use_ds18b20.c's sensor loop; the `sd` and `format` UDP commands in
 * temp-sense.c answer via sd_ring_status() and sd_ring_format().
 *
 * Layout on the card (FatFs, ../sdsc/ pattern — "reserving" space is simply
 * not filling the card):
 *
 *   ring.dat   64MiB, preallocated once at first boot, never grows
 *   meta.dat   one sector: sequence high-water and confirmed watermark
 *
 * Records occupy a fixed slot (seq % SD_RING_CAPACITY), so reading a seq back
 * is a seek rather than a scan.
 */

#include <stdio.h>
#include <string.h>

#include "hardware/rtc.h"
#include "pico/util/datetime.h"

#include "ff.h"
#include "f_util.h"
#include "rtc.h"
#include "hw_config.h"
#include "sd_card.h"

#include "sd_ring.h"
#include "temp_store.h"
#include "crc32.h"

#define RING_PATH "ring.dat"
#define META_PATH "meta.dat"

#define META_MAGIC   0x314d5354u  // "TSM1"
#define META_VERSION 1u

// Metadata, one record per sector-aligned write.
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t next_seq;   // high-water hint; the boot probe refines it
    uint32_t confirmed;  // last seq the collector has durably stored (ack'd)
    uint32_t capacity;   // guards against the ring being resized under us
    uint32_t reserved[2];
    uint32_t crc32;      // over bytes [0, 28)
} sd_meta_t;

_Static_assert(sizeof(sd_meta_t) == 32, "sd_meta_t must be 32 bytes");

// Metadata is persisted lazily rather than after every update. That is safe
// because of two properties that already hold: the boot-time probe below
// repairs a stale next_seq by reading forward from it, and the collector
// re-fetches and re-acks idempotently, so a stale watermark after an unclean
// power-down costs at most a few records it already had — harmless. Writing
// it eagerly would instead make one sector a far worse wear hotspot than the
// whole ring, since it would be rewritten after every single ack.
#define META_PERSIST_INTERVAL 256u

// Cluster link map for fast seek. FF_USE_FASTSEEK is enabled, so with this
// table a seek into the 64MB ring is a lookup instead of a FAT chain walk —
// which is what keeps "index by seq" genuinely O(1). A freshly preallocated
// ring.dat is contiguous and needs only a handful of entries; if the file is
// fragmented enough to overflow this, we fall back to ordinary seeking.
#define CLMT_ENTRIES 64

bool g_sd_ready = false;

static sd_card_t *pSD;
static FIL   ring_fil;
static bool  mounted   = false;
static bool  ring_open = false;
static DWORD clmt[CLMT_ENTRIES];

static uint32_t next_seq      = 0;
static uint32_t confirmed_seq = 0;
static uint32_t puts_since_meta = 0;
static bool     meta_dirty    = false;

/* ------------------------------------------------------------- slot access */

static inline uint32_t slot_of(uint32_t seq) {
    return seq & (SD_RING_CAPACITY - 1u);  // capacity is a power of two
}

static bool read_slot(uint32_t slot, sd_record_t *out) {
    if (!ring_open) return false;
    FSIZE_t off = (FSIZE_t)slot * sizeof(sd_record_t);
    UINT br = 0;
    if (f_lseek(&ring_fil, off) != FR_OK) return false;
    if (f_read(&ring_fil, out, sizeof *out, &br) != FR_OK) return false;
    return br == sizeof *out;
}

// A slot is only meaningful if its CRC checks, its version is one we know,
// and the seq it carries actually belongs in this slot. That last test is
// what makes a preallocated ring safe: f_lseek() expansion does not zero the
// new clusters, so a fresh ring.dat is full of whatever the card held before.
// There is no "written" flag to trust — the record has to prove it belongs.
static bool record_ok(const sd_record_t *r, uint32_t slot) {
    if (r->version != SD_RECORD_VERSION) return false;
    if (crc32_of(r, offsetof(sd_record_t, crc32)) != r->crc32) return false;
    return slot_of(r->seq) == slot;
}

// The seq stored at a slot, if any.
static bool slot_seq(uint32_t slot, uint32_t *seq_out) {
    sd_record_t r;
    if (!read_slot(slot, &r)) return false;
    if (!record_ok(&r, slot)) return false;
    *seq_out = r.seq;
    return true;
}

// Whether this exact seq is still on the card.
static bool seq_present(uint32_t seq) {
    uint32_t got;
    return slot_seq(slot_of(seq), &got) && got == seq;
}

/* --------------------------------------------------------------- recovery */

/*
 * Sequence numbers must stay monotonic across reboots: the RAM ring restarts
 * its counter at 0 on every boot, and if the SD ring did too, new records
 * would collide with old ones and the slot-ownership test above would start
 * rejecting perfectly good data. So the high-water mark is recovered here and
 * fed back into the RAM ring.
 */

// Walk forward from a known-good starting point to find the true end. The
// metadata hint is deliberately stale (see META_PERSIST_INTERVAL), so this
// closes the gap: doubling to bracket the end, then a binary search inside
// the bracket. O(log gap) sector reads, no fixed bound on how stale the hint
// may be.
static uint32_t probe_forward(uint32_t start) {
    if (!seq_present(start)) return start;

    uint32_t lo = start;     // known present
    uint32_t hi = 0;
    bool have_hi = false;

    for (uint32_t step = 1; step <= SD_RING_CAPACITY; step <<= 1) {
        uint32_t probe = start + step;
        if (probe < start) break;  // uint32 wrapped; stop
        if (!seq_present(probe)) {
            hi = probe;
            have_hi = true;
            break;
        }
        lo = probe;
    }
    if (!have_hi) return lo + 1;

    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (seq_present(mid)) lo = mid; else hi = mid;
    }
    return lo + 1;
}

// Used only when meta.dat is missing or unreadable — a replaced or corrupted
// card. Recovers the high-water mark from the ring contents alone in ~21
// sector reads rather than scanning two million records.
static uint32_t scan_next_seq(void) {
    uint32_t last;
    if (!slot_seq(SD_RING_CAPACITY - 1, &last)) {
        // The final slot is empty, so the ring has never wrapped: slots are
        // filled contiguously from 0 and seq == slot. Binary search the edge.
        uint32_t dummy;
        if (!slot_seq(0, &dummy)) return 0;  // completely empty
        uint32_t lo = 0, hi = SD_RING_CAPACITY - 1;  // lo filled, hi empty
        while (hi - lo > 1) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (slot_seq(mid, &dummy)) lo = mid; else hi = mid;
        }
        return lo + 1;
    }

    // The ring has wrapped, so every slot is occupied and seq ascends with
    // slot except for a single drop at the wrap point — a rotated sorted
    // array. Binary search for the drop; the newest record sits just before
    // it.
    uint32_t first;
    if (!slot_seq(0, &first)) return last + 1;
    if (first < last) return last + 1;  // not actually rotated

    uint32_t lo = 0, hi = SD_RING_CAPACITY - 1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t s_mid, s_hi;
        if (!slot_seq(mid, &s_mid) || !slot_seq(hi, &s_hi)) return last + 1;
        if (s_mid > s_hi) lo = mid + 1; else hi = mid;
    }
    uint32_t newest_slot = (lo == 0) ? SD_RING_CAPACITY - 1 : lo - 1;
    uint32_t newest;
    if (!slot_seq(newest_slot, &newest)) return last + 1;
    return newest + 1;
}

/* --------------------------------------------------------------- metadata */

static bool meta_load(sd_meta_t *out) {
    FIL f;
    UINT br = 0;
    if (f_open(&f, META_PATH, FA_READ) != FR_OK) return false;
    FRESULT fr = f_read(&f, out, sizeof *out, &br);
    f_close(&f);
    if (fr != FR_OK || br != sizeof *out) return false;
    if (out->magic != META_MAGIC || out->version != META_VERSION) return false;
    if (crc32_of(out, offsetof(sd_meta_t, crc32)) != out->crc32) return false;
    return out->capacity == SD_RING_CAPACITY;
}

static bool meta_store(void) {
    if (!mounted) return false;
    sd_meta_t m;
    memset(&m, 0, sizeof m);
    m.magic     = META_MAGIC;
    m.version   = META_VERSION;
    m.next_seq  = next_seq;
    m.confirmed = confirmed_seq;
    m.capacity  = SD_RING_CAPACITY;
    m.crc32     = crc32_of(&m, offsetof(sd_meta_t, crc32));

    FIL f;
    UINT bw = 0;
    if (f_open(&f, META_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        return false;
    }
    FRESULT fr = f_write(&f, &m, sizeof m, &bw);
    f_close(&f);
    if (fr == FR_OK && bw == sizeof m) {
        puts_since_meta = 0;
        meta_dirty = false;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------- setup */

static bool open_ring(void) {
    FRESULT fr = f_open(&ring_fil, RING_PATH,
                        FA_READ | FA_WRITE | FA_OPEN_ALWAYS);
    if (fr != FR_OK) {
        printf("sd: f_open(%s) failed: %s (%d)\n", RING_PATH,
               FRESULT_str(fr), fr);
        return false;
    }
    ring_open = true;

    if (f_size(&ring_fil) < SD_RING_BYTES) {
        // Preallocate to full size once. f_expand() is not compiled into this
        // FatFs build (FF_USE_EXPAND is 0, and ff.c builds inside the shared
        // FatFs_SPI target so we cannot override ffconf.h from here), but
        // seeking past EOF on a write handle expands the file just the same —
        // allocating clusters without writing 64MB of zeros through the card.
        printf("sd: preallocating %s to %llu bytes (first boot, please wait)\n",
               RING_PATH, (unsigned long long)SD_RING_BYTES);

        DWORD free_clusters = 0;
        FATFS *fs = NULL;
        if (f_getfree("", &free_clusters, &fs) == FR_OK) {
            uint64_t free_bytes = (uint64_t)free_clusters *
                                  fs->csize * FF_MAX_SS;
            if (free_bytes < SD_RING_BYTES) {
                printf("sd: not enough free space: %llu MB free, need %llu MB\n",
                       (unsigned long long)(free_bytes >> 20),
                       (unsigned long long)(SD_RING_BYTES >> 20));
                f_close(&ring_fil);
                ring_open = false;
                return false;
            }
        }

        fr = f_lseek(&ring_fil, (FSIZE_t)SD_RING_BYTES);
        if (fr != FR_OK || f_size(&ring_fil) < SD_RING_BYTES) {
            printf("sd: preallocation failed: %s (%d)\n", FRESULT_str(fr), fr);
            f_close(&ring_fil);
            ring_open = false;
            return false;
        }
        f_sync(&ring_fil);
        printf("sd: %s preallocated\n", RING_PATH);
    }

    // Build the fast-seek link map. Not fatal if it does not fit.
    ring_fil.cltbl = clmt;
    clmt[0] = CLMT_ENTRIES;
    fr = f_lseek(&ring_fil, CREATE_LINKMAP);
    if (fr != FR_OK) {
        printf("sd: fast-seek map unavailable (%s, needs %lu entries); "
               "using normal seeks\n", FRESULT_str(fr), (unsigned long)clmt[0]);
        ring_fil.cltbl = NULL;
    }
    return true;
}

// The DS3231 is the authority on time here; the RP2040's internal RTC exists
// only so FatFs can stamp files. Kept as a copy so sd_ring_format() can reseed
// without the caller having to re-read the clock.
static ds3231_datetime_t boot_dt_copy;
static bool boot_dt_valid = false;

bool sd_ring_init(const ds3231_datetime_t *boot_dt) {
    g_sd_ready = false;
    next_seq = 0;
    confirmed_seq = 0;

    if (boot_dt) {
        boot_dt_copy = *boot_dt;
        boot_dt_valid = true;
    }

    if (!sd_init_driver()) {
        printf("sd: driver init failed — continuing without SD\n");
        return false;
    }
    pSD = sd_get_by_num(0);
    if (!pSD) {
        printf("sd: no card configured — continuing without SD\n");
        return false;
    }

    // FatFs wants real timestamps (FF_FS_NORTC is 0) and takes them from the
    // RP2040's internal RTC, which boots at year 0. time_init() calls
    // rtc_init(), which resets the clock — so the DS3231 seed has to go in
    // immediately after it, not before.
    time_init();
    if (boot_dt_valid) {
        datetime_t t = {
            .year  = (int16_t)boot_dt_copy.year,
            .month = (int8_t)boot_dt_copy.month,
            .day   = (int8_t)boot_dt_copy.day,
            // api_ds3231 numbers days 1..7 with 1=Monday; the SDK's datetime_t
            // uses 0..6 with 0=Sunday, so Sunday (7) folds to 0.
            .dotw  = (int8_t)(boot_dt_copy.dotw % 7),
            .hour  = (int8_t)boot_dt_copy.hour,
            .min   = (int8_t)boot_dt_copy.minutes,
            .sec   = (int8_t)boot_dt_copy.seconds,
        };
        rtc_set_datetime(&t);
    }

    FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
    if (fr != FR_OK) {
        // Deliberately not a panic (gmcount's fatfs.c does panic here) and
        // deliberately not an automatic reformat. A headless logger that halts
        // on a missing card also stops answering UDP; better to keep sampling
        // into the RAM ring and report the fault. Formatting is destructive,
        // so it stays an explicit operator command.
        printf("sd: mount failed: %s (%d) — continuing without SD "
               "(use the `format` command if the card is blank)\n",
               FRESULT_str(fr), fr);
        return false;
    }
    mounted = true;

    uint64_t sectors = sd_sectors(pSD);
    printf("sd: card mounted, %llu sectors (%llu MB)\n",
           (unsigned long long)sectors,
           (unsigned long long)((sectors * 512ull) >> 20));

    if (!open_ring()) {
        f_unmount(pSD->pcName);
        mounted = false;
        return false;
    }

    // Recover the high-water mark: the metadata hint if we have one, then a
    // forward probe to cover records written since it was last persisted.
    sd_meta_t m;
    bool have_meta = meta_load(&m);

    // Trust the hint only if the record immediately before it is still on the
    // card. If it is not, the hint predates the ring's current contents — a
    // card swapped between devices, or written elsewhere — and probing forward
    // from it would stop at the hint and start re-issuing sequence numbers
    // that are already in use. Falling back to a scan costs ~21 sector reads.
    bool hint_ok = have_meta &&
                   (m.next_seq == 0 || seq_present(m.next_seq - 1));

    if (hint_ok) {
        confirmed_seq = m.confirmed;
        next_seq = probe_forward(m.next_seq);
    } else {
        if (have_meta) {
            printf("sd: %s is stale (seq %lu is no longer on the card) — "
                   "recovering from the ring\n",
                   META_PATH, (unsigned long)m.next_seq);
        } else {
            printf("sd: no usable %s — recovering sequence from the ring\n",
                   META_PATH);
        }
        next_seq = scan_next_seq();
        // Nothing known to be unconfirmed, unless the metadata's watermark is
        // still coherent with what the scan found.
        confirmed_seq =
            (have_meta && m.confirmed <= next_seq) ? m.confirmed : next_seq;
    }

    g_sd_ready = true;
    printf("sd: ring ready — %lu records (%llu MB), next seq %lu, "
           "confirmed %lu\n",
           (unsigned long)SD_RING_CAPACITY,
           (unsigned long long)(SD_RING_BYTES >> 20),
           (unsigned long)next_seq, (unsigned long)confirmed_seq);
    meta_store();
    return true;
}

/* ----------------------------------------------------------------- ring IO */

bool sd_ring_available(void) {
    return g_sd_ready;
}

bool sd_ring_put(const temp_record_t *rec) {
    if (!g_sd_ready) return false;

    sd_record_t r;
    memset(&r, 0, sizeof r);
    r.seq     = rec->seq;
    r.epoch   = rec->epoch;
    r.romcode = rec->romcode;
    r.raw     = rec->raw;
    r.flags   = rec->flags;
    r.version = SD_RECORD_VERSION;
    r.crc32   = crc32_of(&r, offsetof(sd_record_t, crc32));

    FSIZE_t off = (FSIZE_t)slot_of(r.seq) * sizeof(sd_record_t);
    UINT bw = 0;
    FRESULT fr = f_lseek(&ring_fil, off);
    if (fr == FR_OK) fr = f_write(&ring_fil, &r, sizeof r, &bw);
    if (fr != FR_OK || bw != sizeof r) {
        printf("sd: write of seq %lu failed: %s (%d)\n",
               (unsigned long)r.seq, FRESULT_str(fr), fr);
        return false;
    }

    if (r.seq >= next_seq) next_seq = r.seq + 1;
    puts_since_meta++;
    meta_dirty = true;
    return true;
}

bool sd_ring_get(uint32_t seq, temp_record_t *out) {
    if (!g_sd_ready) return false;
    if (seq >= next_seq || seq < sd_ring_oldest_seq()) return false;

    sd_record_t r;
    uint32_t slot = slot_of(seq);
    if (!read_slot(slot, &r)) return false;
    if (!record_ok(&r, slot) || r.seq != seq) return false;

    out->seq     = r.seq;
    out->epoch   = r.epoch;
    out->romcode = r.romcode;
    out->raw     = r.raw;
    out->flags   = r.flags;
    return true;
}

uint32_t sd_ring_next_seq(void) {
    return next_seq;
}

uint32_t sd_ring_oldest_seq(void) {
    return next_seq > SD_RING_CAPACITY ? next_seq - SD_RING_CAPACITY : 0;
}

uint32_t sd_ring_confirmed_seq(void) {
    return confirmed_seq;
}

void sd_ring_set_confirmed(uint32_t seq) {
    confirmed_seq = seq;
    meta_dirty = true;
}

void sd_ring_sync(void) {
    if (!g_sd_ready) return;
    FRESULT fr = f_sync(&ring_fil);
    if (fr != FR_OK) {
        printf("sd: f_sync failed: %s (%d)\n", FRESULT_str(fr), fr);
    }
    if (meta_dirty && puts_since_meta >= META_PERSIST_INTERVAL) {
        meta_store();
    }
}

/* ----------------------------------------------------------------- format */

bool sd_ring_format(void) {
    static BYTE work[4096];  // static: too big for the stack

    if (ring_open) {
        f_close(&ring_fil);
        ring_open = false;
    }
    if (mounted) {
        f_unmount(pSD->pcName);
        mounted = false;
    }
    g_sd_ready = false;

    if (!pSD) {
        pSD = sd_get_by_num(0);
        if (!pSD) return false;
    }

    // FM_FAT|FM_FAT32 deliberately excludes exFAT: on a 128MB card FAT is what
    // every host will mount without argument, and exFAT buys nothing at this
    // size. Cluster size and root-entry count are left to FatFs.
    MKFS_PARM opt = {
        .fmt     = FM_FAT | FM_FAT32,
        .n_fat   = 1,
        .align   = 0,
        .n_root  = 0,
        .au_size = 0,
    };

    printf("sd: formatting card — this destroys all data on it\n");
    FRESULT fr = f_mkfs(pSD->pcName, &opt, work, sizeof work);
    if (fr != FR_OK) {
        printf("sd: f_mkfs failed: %s (%d)\n", FRESULT_str(fr), fr);
        return false;
    }
    printf("sd: format complete, re-initialising\n");

    // Deliberately not preserving next_seq/confirmed_seq across the wipe. A
    // formatted card has no more prior history than a swapped-in blank one —
    // sd_ring_init() already treats *that* case as a legitimate reset (see
    // the scan_next_seq() comment on a swapped card above) — so format
    // restarting the count the same way is consistent rather than a special
    // case. A consumer downstream of MQTT has to detect a seq decrease as a
    // lineage reset regardless, to handle the swap case; format doesn't need
    // its own mechanism to dodge a problem that already exists elsewhere.
    return sd_ring_init(boot_dt_valid ? &boot_dt_copy : NULL);
}

/* ----------------------------------------------------------------- status */

void sd_ring_status(char *buf, size_t buf_size) {
    if (!g_sd_ready) {
        snprintf(buf, buf_size,
                 "sd: not available (no card, unmountable, or ring not open)\n");
        return;
    }
    uint32_t oldest = sd_ring_oldest_seq();
    uint32_t stored = next_seq - oldest;
    snprintf(buf, buf_size,
             "sd: ready  capacity %lu  stored %lu  seq %lu..%lu  "
             "confirmed %lu  backlog %lu\n",
             (unsigned long)SD_RING_CAPACITY,
             (unsigned long)stored,
             (unsigned long)oldest,
             (unsigned long)(next_seq ? next_seq - 1 : 0),
             (unsigned long)confirmed_seq,
             (unsigned long)(next_seq - confirmed_seq));
}
