#include "crc32.h"

// Table-free: the data volumes here (SD records/meta, single UDP packets)
// are small enough that the per-byte bit loop is nothing, and it saves a
// 1KB table.
uint32_t crc32_of(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}
