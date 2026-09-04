#include "xfer_session.h"

#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"

#include "xfer_proto.h"
#include "crc32.h"
#include "sd_ring.h"
#include "temp_store.h"

/*
 * A "set" on the wire is one sensor cycle: temp_ring_push()/sd_ring_put() are
 * called once per active sensor per cycle (see use_ds18b20.c), in the same
 * enumeration order every time, so g_temp_num_devs consecutive SD-ring seqs
 * starting at a set's first seq share one epoch and *are* that set, with
 * sensor_id == the offset within the group. This assumes the roster (and
 * therefore the group size) never changes mid-ring, which matches the
 * project's existing "roster fixed at boot" assumption for the `sensors`
 * command.
 */

typedef struct {
    bool     active;        // false only until the first REQUEST of this
                             // boot; never cleared afterward -- a completed
                             // transfer stays matchable by transfer_id so a
                             // duplicate final ACK still gets its idempotent
                             // resend instead of an empty reply
    uint32_t transfer_id;
    uint32_t start_seq;      // first SD-ring seq covered by this transfer
    uint32_t total_sets;     // X, snapshotted at REQUEST time
    int      n_sensors;      // snapshot of g_temp_num_devs at REQUEST time
    size_t   spp;
    uint16_t total_packets;  // TP; always >= 1 once active
    uint16_t last_sent_seq;
} xfer_session_t;

static xfer_session_t s_session;

static uint32_t next_transfer_id(void) {
    // Monotonic within a boot (never repeats a still-live id) and seeded
    // from uptime so consecutive boots don't restart at the same value.
    static uint32_t s_id;
    static bool s_seeded;
    if (!s_seeded) {
        s_id = to_ms_since_boot(get_absolute_time());
        s_seeded = true;
    }
    return ++s_id;
}

// There is exactly one collector for this device, always -- no need to let
// a REQUEST assert its own position. Always resume right after the last
// fully-ACK'd transfer.
static void start_transfer(void) {
    uint32_t oldest = sd_ring_oldest_seq();
    uint32_t next = sd_ring_next_seq();
    uint32_t start_seq = sd_ring_confirmed_seq() + 1;
    if (start_seq < oldest) start_seq = oldest;  // defensive; shouldn't happen
    if (start_seq > next) start_seq = next;      // defensive; shouldn't happen

    int n_sensors = g_temp_num_devs;
    if (n_sensors <= 0) n_sensors = 1;  // guards the division below; with no
                                         // sensors there is never any pending
                                         // data, so total_sets ends up 0
                                         // regardless.

    uint32_t pending = (next > start_seq) ? next - start_seq : 0;
    uint32_t total_sets = pending / (uint32_t)n_sensors;

    size_t spp = xfer_sets_per_packet(n_sensors);
    if (spp == 0) spp = 1;  // defensive; XFER_SET_SIZE stays well under the
                              // payload budget for n_sensors in 3..20

    uint16_t tp = xfer_total_packets(total_sets, spp);
    if (tp == 0) tp = 1;  // always send at least one packet — a lone
                           // set_count=0/END packet when nothing is pending
                           // — so the receiver's flow needs no special case

    s_session.active        = true;
    s_session.transfer_id    = next_transfer_id();
    s_session.start_seq      = start_seq;
    s_session.total_sets     = total_sets;
    s_session.n_sensors      = n_sensors;
    s_session.spp            = spp;
    s_session.total_packets  = tp;
    s_session.last_sent_seq  = 0;
}

// Rebuilds packet `seq` from the SD ring and writes it into resp. Assumes
// s_session.active and 0 <= seq < s_session.total_packets.
static void build_data_packet(uint16_t seq, char *resp, size_t resp_size,
                               size_t *resp_len) {
    (void)resp_size;  // payload is bounded by XFER_MAX_UDP_PAYLOAD (508),
                       // well under WIFI_UDP_BUF_SIZE (1024)

    uint32_t set_offset = (uint32_t)seq * (uint32_t)s_session.spp;
    uint8_t set_count = 0;
    if (s_session.total_sets > set_offset) {
        uint32_t remaining = s_session.total_sets - set_offset;
        set_count = (uint8_t)(remaining < s_session.spp ? remaining
                                                          : s_session.spp);
    }

    uint8_t *payload = (uint8_t *)resp + XFER_DATA_HEADER_LEN;
    size_t payload_len = 0;
    for (uint8_t j = 0; j < set_count; j++) {
        uint32_t base_seq = s_session.start_seq +
                             (set_offset + j) * (uint32_t)s_session.n_sensors;
        temp_record_t rec;
        uint32_t timestamp = sd_ring_get(base_seq, &rec) ? rec.epoch : 0;
        payload_len += xfer_pack_set_header(payload + payload_len, timestamp,
                                             (uint8_t)s_session.n_sensors);
        for (int k = 0; k < s_session.n_sensors; k++) {
            int16_t raw = 0;
            uint8_t valid = 0;
            if (sd_ring_get(base_seq + (uint32_t)k, &rec)) {
                raw = rec.raw;
                valid = (rec.flags & TEMP_FLAG_VALID) ? 1u : 0u;
            }
            payload_len += xfer_pack_set_entry(payload + payload_len,
                                                (uint8_t)k, raw, valid);
        }
    }

    uint32_t crc = crc32_of(payload, payload_len);
    uint8_t flags = (seq == s_session.total_packets - 1) ? XFER_FLAG_END : 0;
    xfer_pack_data_header((uint8_t *)resp, s_session.transfer_id, seq,
                           s_session.total_packets, set_count, flags, crc);

    s_session.last_sent_seq = seq;
    *resp_len = XFER_DATA_HEADER_LEN + payload_len;
}

// REQUEST carries no payload (see xfer_proto.h) -- payload/len are unused,
// kept only so this matches handle_ack()/handle_nack()'s call signature for
// xfer_session_handle()'s dispatch.
static void handle_request(const uint8_t *payload, size_t len, char *resp,
                            size_t resp_size, size_t *resp_len) {
    (void)payload;
    (void)len;
    // A REQUEST always starts a fresh transfer, discarding any prior
    // in-flight one — the logger stays stateless-per-poll and just answers
    // whatever it most recently received.
    start_transfer();
    build_data_packet(0, resp, resp_size, resp_len);
}

static void handle_ack(const uint8_t *payload, size_t len, char *resp,
                        size_t resp_size, size_t *resp_len) {
    uint32_t transfer_id;
    uint16_t seq;
    if (!s_session.active ||
        !xfer_unpack_ack_payload(payload, len, &transfer_id, &seq) ||
        transfer_id != s_session.transfer_id) {
        *resp_len = 0;
        return;
    }

    if (seq != s_session.last_sent_seq) {
        // Stale ack (our retransmit and its ack crossed) — resend whatever
        // we last sent; every DATA packet is deterministic from session
        // state, so this is safe to repeat.
        build_data_packet(s_session.last_sent_seq, resp, resp_size, resp_len);
        return;
    }

    if (seq == s_session.total_packets - 1) {
        // Final packet acked: advance the watermark to the newest record in
        // this transfer, then resend the same packet as an idempotent
        // completion confirmation — a repeated final ACK (ours got lost)
        // lands here again and just re-finalizes.
        if (s_session.total_sets > 0) {
            uint32_t last_seq = s_session.start_seq +
                                 s_session.total_sets *
                                     (uint32_t)s_session.n_sensors -
                                 1;
            sd_ring_set_confirmed(last_seq);
        }
        // Deliberately not marked inactive: a genuinely duplicate final ACK
        // (this reply got lost, receiver retried) must land here again and
        // get the same idempotent resend, not an empty reply from an
        // inactive-session guard. The session is only ever superseded by
        // the next REQUEST (start_transfer() overwrites it wholesale).
        build_data_packet(seq, resp, resp_size, resp_len);
        return;
    }

    build_data_packet((uint16_t)(seq + 1), resp, resp_size, resp_len);
}

static void handle_nack(const uint8_t *payload, size_t len, char *resp,
                         size_t resp_size, size_t *resp_len) {
    uint32_t transfer_id;
    uint16_t seqs[XFER_NACK_MAX_SEQS];
    uint8_t count;
    if (!s_session.active ||
        !xfer_unpack_nack_payload(payload, len, &transfer_id, seqs,
                                   XFER_NACK_MAX_SEQS, &count) ||
        transfer_id != s_session.transfer_id || count == 0) {
        *resp_len = 0;
        return;
    }
    // Stop-and-wait only ever has one packet outstanding — seqs[0] is the
    // one that needs retransmission (see xfer_unpack_nack_payload()).
    build_data_packet(seqs[0], resp, resp_size, resp_len);
}

void xfer_session_handle(const char *cmd, size_t cmd_len, char *resp,
                          size_t resp_size, size_t *resp_len) {
    const uint8_t *in = (const uint8_t *)cmd;
    uint8_t msg_type;
    if (!xfer_unpack_msg_header(in, cmd_len, &msg_type)) {
        *resp_len = 0;
        return;
    }
    const uint8_t *payload = in + XFER_MSG_HEADER_LEN;
    size_t payload_len = cmd_len - XFER_MSG_HEADER_LEN;

    switch (msg_type) {
        case XFER_MSG_REQUEST:
            handle_request(payload, payload_len, resp, resp_size, resp_len);
            break;
        case XFER_MSG_ACK:
            handle_ack(payload, payload_len, resp, resp_size, resp_len);
            break;
        case XFER_MSG_NACK:
            handle_nack(payload, payload_len, resp, resp_size, resp_len);
            break;
        default:
            *resp_len = 0;
            break;
    }
}
