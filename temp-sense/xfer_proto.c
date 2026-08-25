#include "xfer_proto.h"

/* ----------------------------------------------------- little-endian I/O */

static inline void put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline uint16_t get_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* --------------------------------------------------------------- sizing */

size_t xfer_sets_per_packet(int n_sensors) {
    size_t set_size = XFER_SET_SIZE(n_sensors);
    return (XFER_MAX_UDP_PAYLOAD - XFER_DATA_HEADER_LEN) / set_size;
}

uint16_t xfer_total_packets(uint32_t total_sets, size_t spp) {
    if (total_sets == 0 || spp == 0) return 0;
    return (uint16_t)((total_sets + spp - 1) / spp);
}

/* ------------------------------------------------------------ DATA header */

void xfer_pack_data_header(uint8_t out[XFER_DATA_HEADER_LEN],
                            uint32_t transfer_id, uint16_t seq,
                            uint16_t total_packets, uint8_t set_count,
                            uint8_t flags, uint32_t crc32) {
    out[0] = XFER_MAGIC;
    out[1] = (uint8_t)XFER_VERSION;
    put_u32le(out + 2, transfer_id);
    put_u16le(out + 6, seq);
    put_u16le(out + 8, total_packets);
    out[10] = set_count;
    out[11] = flags;
    put_u32le(out + 12, crc32);
}

/* ------------------------------------------------------------ msg header */

bool xfer_unpack_msg_header(const uint8_t *in, size_t len, uint8_t *msg_type) {
    if (len < XFER_MSG_HEADER_LEN) return false;
    if (in[0] != XFER_MAGIC || in[2] != (uint8_t)XFER_VERSION) return false;
    *msg_type = in[1];
    return true;
}

/* --------------------------------------------------------------- REQUEST */

bool xfer_unpack_request_payload(const uint8_t *in, size_t len,
                                  uint32_t *watermark_epoch) {
    if (len < XFER_REQUEST_PAYLOAD_LEN) return false;
    *watermark_epoch = get_u32le(in);
    return true;
}

/* ------------------------------------------------------------------- ACK */

bool xfer_unpack_ack_payload(const uint8_t *in, size_t len,
                              uint32_t *transfer_id, uint16_t *seq) {
    if (len < XFER_ACK_PAYLOAD_LEN) return false;
    *transfer_id = get_u32le(in);
    *seq = get_u16le(in + 4);
    return true;
}

/* ------------------------------------------------------------------ NACK */

bool xfer_unpack_nack_payload(const uint8_t *in, size_t len,
                               uint32_t *transfer_id, uint16_t *seqs,
                               uint8_t max_seqs, uint8_t *count) {
    if (len < 5u) return false;
    *transfer_id = get_u32le(in);
    uint8_t n = in[4];
    if (n > max_seqs) n = max_seqs;
    if (len < 5u + (size_t)n * 2u) return false;
    for (uint8_t i = 0; i < n; i++) {
        seqs[i] = get_u16le(in + 5 + (size_t)i * 2u);
    }
    *count = n;
    return true;
}

/* ------------------------------------------------------------ DATA payload */

size_t xfer_pack_set_header(uint8_t *out, uint32_t timestamp, uint8_t count) {
    put_u32le(out, timestamp);
    out[4] = count;
    return 5u;
}

size_t xfer_pack_set_entry(uint8_t *out, uint8_t sensor_id, int16_t temperature) {
    out[0] = sensor_id;
    put_u16le(out + 1, (uint16_t)temperature);
    return 3u;
}
