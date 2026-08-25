#ifndef TEMP_SENSE_CRC32_H
#define TEMP_SENSE_CRC32_H

#include <stddef.h>
#include <stdint.h>

// Bitwise CRC-32 (reflected, poly 0xEDB88320) — the standard CRC-32 used by
// zlib/gzip/PNG, so it matches Python's zlib.crc32() byte for byte. Used for
// both SD-record/ring-state integrity (sd_ring.c) and, going forward, the
// wire CRC in the v1.2 UDP transfer protocol.
uint32_t crc32_of(const void *data, size_t len);

#endif
