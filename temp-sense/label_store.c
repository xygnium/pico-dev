#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "ff.h"
#include "f_util.h"

#include "label_store.h"
#include "sd_ring.h"
#include "crc32.h"

#define LABEL_PATH "labels.dat"

#define LABEL_MAGIC   0x314C5354u  // "TSL1"
#define LABEL_VERSION 1u

typedef struct {
    uint64_t romcode;
    char     label[LABEL_STRING_LEN];
} label_entry_t;

typedef struct {
    uint32_t      magic;
    uint32_t      version;
    uint32_t      count;
    label_entry_t entries[LABEL_MAX_ENTRIES];
    uint32_t      crc32;  // over bytes [0, offsetof(label_table_t, crc32))
} label_table_t;

static label_table_t s_table;

static bool label_load(label_table_t *out) {
    FIL f;
    UINT br = 0;
    if (f_open(&f, LABEL_PATH, FA_READ) != FR_OK) return false;
    FRESULT fr = f_read(&f, out, sizeof *out, &br);
    f_close(&f);
    if (fr != FR_OK || br != sizeof *out) return false;
    if (out->magic != LABEL_MAGIC || out->version != LABEL_VERSION) return false;
    if (out->count > LABEL_MAX_ENTRIES) return false;
    return crc32_of(out, offsetof(label_table_t, crc32)) == out->crc32;
}

static bool label_save(void) {
    s_table.magic   = LABEL_MAGIC;
    s_table.version = LABEL_VERSION;
    s_table.crc32   = crc32_of(&s_table, offsetof(label_table_t, crc32));

    FIL f;
    UINT bw = 0;
    if (f_open(&f, LABEL_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        return false;
    }
    FRESULT fr = f_write(&f, &s_table, sizeof s_table, &bw);
    f_close(&f);
    return fr == FR_OK && bw == sizeof s_table;
}

bool label_store_init(void) {
    memset(&s_table, 0, sizeof s_table);
    if (!sd_ring_available()) return false;
    label_table_t loaded;
    if (!label_load(&loaded)) return false;
    s_table = loaded;
    return true;
}

int label_store_register_new(const uint64_t *romcodes, int count) {
    if (!sd_ring_available()) return 0;

    int added = 0;
    for (int i = 0; i < count; i++) {
        bool known = false;
        for (uint32_t j = 0; j < s_table.count; j++) {
            if (s_table.entries[j].romcode == romcodes[i]) {
                known = true;
                break;
            }
        }
        if (known) continue;
        if (s_table.count >= LABEL_MAX_ENTRIES) break;

        label_entry_t *e = &s_table.entries[s_table.count++];
        e->romcode = romcodes[i];
        memset(e->label, 0, sizeof e->label);
        strncpy(e->label, LABEL_PLACEHOLDER, sizeof e->label - 1);
        added++;
    }

    // Best-effort: even if the write fails, the table stays correct in RAM
    // for this boot, and a later `label`/register call will retry
    // persisting it rather than losing track of what's already known.
    if (added > 0) label_save();
    return added;
}

bool label_store_set(uint64_t romcode, const char *label) {
    if (!sd_ring_available()) return false;
    size_t len = strlen(label);
    if (len == 0 || len >= LABEL_STRING_LEN) return false;

    for (uint32_t j = 0; j < s_table.count; j++) {
        if (s_table.entries[j].romcode != romcode) continue;

        char prev[LABEL_STRING_LEN];
        memcpy(prev, s_table.entries[j].label, sizeof prev);

        memset(s_table.entries[j].label, 0, sizeof s_table.entries[j].label);
        memcpy(s_table.entries[j].label, label, len);

        if (!label_save()) {
            memcpy(s_table.entries[j].label, prev, sizeof prev);
            return false;
        }
        return true;
    }
    return false;  // unknown romcode -- register_new() adds it, this only renames
}

int label_store_count(void) {
    return (int)s_table.count;
}

bool label_store_get(int index, uint64_t *romcode, char *out, size_t out_size) {
    if (index < 0 || (uint32_t)index >= s_table.count) return false;
    *romcode = s_table.entries[index].romcode;
    snprintf(out, out_size, "%s", s_table.entries[index].label);
    return true;
}

bool label_store_lookup(uint64_t romcode, char *out, size_t out_size) {
    for (uint32_t j = 0; j < s_table.count; j++) {
        if (s_table.entries[j].romcode != romcode) continue;
        snprintf(out, out_size, "%s", s_table.entries[j].label);
        return true;
    }
    return false;
}

bool label_store_decomm(int index) {
    if (!sd_ring_available()) return false;
    if (index < 0 || (uint32_t)index >= s_table.count) return false;

    label_table_t prev = s_table;

    for (uint32_t j = (uint32_t)index; j + 1 < s_table.count; j++) {
        s_table.entries[j] = s_table.entries[j + 1];
    }
    s_table.count--;

    if (!label_save()) {
        s_table = prev;
        return false;
    }
    return true;
}

bool label_store_wipe(void) {
    if (!sd_ring_available()) return false;

    label_table_t prev = s_table;
    s_table.count = 0;

    if (!label_save()) {
        s_table = prev;
        return false;
    }
    return true;
}
