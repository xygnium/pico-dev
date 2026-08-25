#ifndef TEMP_SENSE_XFER_PROTO_H
#define TEMP_SENSE_XFER_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Wire primitives for the v1.2 UDP transfer protocol (see
 * temp-logger-udp-protocol.md), logger (device) side only. Pure pack/unpack
 * + sizing here — no session/retry state machine (that is xfer_session.h,
 * a later stage).
 *
 * This only covers what the *logger* needs: it builds DATA (the only thing
 * it ever sends) and parses REQUEST/ACK/NACK (the only things it ever
 * receives). The other direction — packing REQUEST/ACK/NACK and parsing
 * DATA — belongs to the receiver, which is collector.py (a later stage),
 * not C; there is intentionally no receiver-side pack/parse code here to
 * keep the firmware image free of a permanently-unreachable other half. If
 * that changes (a C-based receiver), reinstate the mirror-image functions
 * rather than repurposing these.
 *
 * All multi-byte fields are little-endian, packed with explicit per-field
 * byte writes rather than a struct cast over a raw buffer: the DATA header
 * places a 4-byte field at offset 2, which is not 4-byte aligned, and
 * Cortex-M0+ hard-faults on an unaligned word load/store through a typed
 * pointer.
 *
 * XFER_MAGIC (0xA5) is outside the printable-ASCII range every existing
 * text command starts with, so temp-sense.c's dispatcher can check the
 * first received byte to route a REQUEST/ACK/NACK here before falling into
 * the ASCII strncmp chain, with no ambiguity either way.
 */

#define XFER_MAGIC   0xA5u
#define XFER_VERSION 1u

/* --------------------------------------------------------------- sizing */

// UDP payload budget the protocol doc sizes to, to stay clear of IP
// fragmentation.
#define XFER_MAX_UDP_PAYLOAD 508u

#define XFER_DATA_HEADER_LEN 16u

// Bytes for one "set": timestamp(4) + count(1) + count * [sensor_id(1) +
// temperature(2)]. N_sensors is fixed per transfer (the roster doesn't
// change mid-session), so every set in a transfer is this same size.
#define XFER_SET_SIZE(n_sensors) (4u + 1u + (size_t)(n_sensors) * 3u)

// Sets per packet: the most that fit in one packet's payload after the
// 16-byte header, for a transfer with `n_sensors` active sensors. Computed
// at runtime rather than fixed, because a constant tuned for 20 sensors
// wastes bandwidth at 3, and one tuned for 3 overflows the MTU at 20.
size_t xfer_sets_per_packet(int n_sensors);

// ceil(total_sets / spp). Returns 0 if there is nothing to send.
uint16_t xfer_total_packets(uint32_t total_sets, size_t spp);

/* ------------------------------------------------- DATA header (logger -> receiver) */

// flags bit0: this is the last packet of the transfer.
#define XFER_FLAG_END 0x01u

void xfer_pack_data_header(uint8_t out[XFER_DATA_HEADER_LEN],
                            uint32_t transfer_id, uint16_t seq,
                            uint16_t total_packets, uint8_t set_count,
                            uint8_t flags, uint32_t crc32);

/* ------------------------------------- inbound header (receiver -> logger) */

// magic(1) + msg_type(1) + version(1) + reserved(1), common to REQUEST/
// ACK/NACK so the logger can tell which one it received before parsing the
// type-specific payload that follows at this offset.
#define XFER_MSG_HEADER_LEN 4u

#define XFER_MSG_REQUEST 1u
#define XFER_MSG_ACK     2u
#define XFER_MSG_NACK    3u

// False if magic/version don't match. On success, *msg_type is one of
// XFER_MSG_*  (an unrecognized type byte is still reported — validating it
// is the caller's job).
bool xfer_unpack_msg_header(const uint8_t *in, size_t len, uint8_t *msg_type);

// REQUEST carries no payload -- there is exactly one collector for this
// device, always (see the project's single-requestor constraint), so the
// logger always resumes from its own sd_ring_confirmed_seq() + 1 rather
// than trusting a receiver-asserted position. The message header alone
// (XFER_MSG_HEADER_LEN, above) is the entire REQUEST message.

/* ------------------------------------------------------------- ACK payload */

// transfer_id(4) + seq(2).
#define XFER_ACK_PAYLOAD_LEN 6u

bool xfer_unpack_ack_payload(const uint8_t *in, size_t len,
                              uint32_t *transfer_id, uint16_t *seq);

/* ------------------------------------------------------------ NACK payload */

// transfer_id(4) + count(1) + count * seq(2). Stop-and-wait only ever
// populates one seq today, but the format carries a count so it does not
// need to change if windowing (more than one outstanding packet) is added
// later.
#define XFER_NACK_MAX_SEQS 32u

// *count is capped at max_seqs; a NACK naming more seqs than that is
// truncated rather than rejected, since a stop-and-wait sender only ever
// needs the first one anyway.
bool xfer_unpack_nack_payload(const uint8_t *in, size_t len,
                               uint32_t *transfer_id, uint16_t *seqs,
                               uint8_t max_seqs, uint8_t *count);

/* --------------------------------------------- DATA payload (logger -> receiver) */

// One set's fixed prefix: timestamp(4) + count(1). Returns bytes written (5).
size_t xfer_pack_set_header(uint8_t *out, uint32_t timestamp, uint8_t count);

// One set's per-sensor entry: sensor_id(1) + temperature(2, signed
// fixed-point, DS18B20 native 1/16 degC). Returns bytes written (3).
size_t xfer_pack_set_entry(uint8_t *out, uint8_t sensor_id, int16_t temperature);

#endif
