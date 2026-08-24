#include <stddef.h>
#include <string.h>

#include "ff.h"
#include "f_util.h"

#include "config_store.h"
#include "sd_ring.h"
#include "crc32.h"

#define CONFIG_PATH "config.dat"

#define CONFIG_MAGIC   0x31435354u  // "TSC1"
#define CONFIG_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t max_retries;
    uint32_t retry_interval_ms;
    uint32_t crc32;  // over bytes [0, 16)
} config_t;

_Static_assert(sizeof(config_t) == 20, "config_t must be 20 bytes");

static uint32_t s_max_retries       = CONFIG_DEFAULT_MAX_RETRIES;
static uint32_t s_retry_interval_ms = CONFIG_DEFAULT_RETRY_INTERVAL_MS;

static bool config_load(config_t *out) {
    FIL f;
    UINT br = 0;
    if (f_open(&f, CONFIG_PATH, FA_READ) != FR_OK) return false;
    FRESULT fr = f_read(&f, out, sizeof *out, &br);
    f_close(&f);
    if (fr != FR_OK || br != sizeof *out) return false;
    if (out->magic != CONFIG_MAGIC || out->version != CONFIG_VERSION) {
        return false;
    }
    return crc32_of(out, offsetof(config_t, crc32)) == out->crc32;
}

static bool config_save(void) {
    config_t c;
    memset(&c, 0, sizeof c);
    c.magic             = CONFIG_MAGIC;
    c.version           = CONFIG_VERSION;
    c.max_retries       = s_max_retries;
    c.retry_interval_ms = s_retry_interval_ms;
    c.crc32             = crc32_of(&c, offsetof(config_t, crc32));

    FIL f;
    UINT bw = 0;
    if (f_open(&f, CONFIG_PATH, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        return false;
    }
    FRESULT fr = f_write(&f, &c, sizeof c, &bw);
    f_close(&f);
    return fr == FR_OK && bw == sizeof c;
}

bool config_store_init(void) {
    if (!sd_ring_available()) return false;
    config_t c;
    if (!config_load(&c)) return false;
    s_max_retries = c.max_retries;
    s_retry_interval_ms = c.retry_interval_ms;
    return true;
}

uint32_t config_store_max_retries(void) {
    return s_max_retries;
}

uint32_t config_store_retry_interval_ms(void) {
    return s_retry_interval_ms;
}

bool config_store_set(uint32_t max_retries, uint32_t retry_interval_ms) {
    if (max_retries < CONFIG_MIN_MAX_RETRIES ||
        max_retries > CONFIG_MAX_MAX_RETRIES ||
        retry_interval_ms < CONFIG_MIN_RETRY_INTERVAL_MS ||
        retry_interval_ms > CONFIG_MAX_RETRY_INTERVAL_MS) {
        return false;
    }
    if (!sd_ring_available()) return false;

    uint32_t prev_retries = s_max_retries;
    uint32_t prev_interval = s_retry_interval_ms;
    s_max_retries = max_retries;
    s_retry_interval_ms = retry_interval_ms;
    if (!config_save()) {
        s_max_retries = prev_retries;
        s_retry_interval_ms = prev_interval;
        return false;
    }
    return true;
}
