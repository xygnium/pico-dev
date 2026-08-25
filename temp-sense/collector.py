#!/usr/bin/env python3
"""Pull pending readings from the temp-sense Pico W over the v1.2 binary
UDP protocol (see temp-logger-udp-protocol.md) and append them to a CSV.

One-shot: does exactly one REQUEST -> DATA/ACK/NACK -> (watermark advance)
cycle per invocation, then exits. Meant to be run periodically by cron/a
systemd timer (the protocol doc's "hourly, normally"), not to loop itself.

Usage:
    ./collector.py                       # pull into ./temp_data.csv
    ./collector.py --csv /path/to.csv
    ./collector.py --host 1.2.3.4
    ./collector.py --labels              # refresh sensor_labels.csv instead

Exit status is 0 on a fully-completed transfer, 1 if it gave up partway
(no response, or a packet failed CRC past the device's configured
max_retries). A partial run is always safe to just retry next cycle: the
device only advances its watermark on a confirmed final ACK, and rows are
deduped here by (sensor_id, timestamp), so re-pulled data is a no-op.

Readings are stored by sensor_id (this session's wire index) only, not
romcode or a location string -- resolve those separately via --labels,
which mirrors the device's romcode->location table (see label_store.h) on
demand. Run --labels by hand once a sensor-addition session is complete;
it is never fetched automatically as part of a normal pull.
"""

import argparse
import csv
import datetime
import os
import re
import socket
import struct
import sys
import zlib

HOST = "192.168.1.120"
PORT = 8080
BUFSIZE = 1024
SETUP_TIMEOUT = 5  # seconds, for the plain-ASCII bootstrap commands

XFER_MAGIC = 0xA5
XFER_VERSION = 1

XFER_MSG_REQUEST = 1
XFER_MSG_ACK = 2
XFER_MSG_NACK = 3

XFER_FLAG_END = 0x01

DATA_HEADER_FMT = "<BBIHHBBI"
DATA_HEADER_LEN = struct.calcsize(DATA_HEADER_FMT)
assert DATA_HEADER_LEN == 16


def msg_header(msg_type):
    return bytes([XFER_MAGIC, msg_type, XFER_VERSION, 0])


def pack_ack(transfer_id, seq):
    return msg_header(XFER_MSG_ACK) + struct.pack("<IH", transfer_id, seq)


def pack_nack(transfer_id, seqs):
    body = struct.pack("<IB", transfer_id, len(seqs))
    for s in seqs:
        body += struct.pack("<H", s)
    return msg_header(XFER_MSG_NACK) + body


def parse_data_packet(buf):
    """Returns None if this isn't a well-formed v1.2 DATA packet at all
    (wrong magic/version/too short — garbage, ignore and keep retrying).
    Otherwise a dict with the header fields, crc_ok, and (only if crc_ok)
    the decoded sets — CRC failure is a real protocol event (NACK it), not
    something to silently drop.
    """
    if len(buf) < DATA_HEADER_LEN:
        return None
    magic, version, transfer_id, seq, total_packets, set_count, flags, crc = \
        struct.unpack_from(DATA_HEADER_FMT, buf, 0)
    if magic != XFER_MAGIC or version != XFER_VERSION:
        return None

    payload = buf[DATA_HEADER_LEN:]
    crc_ok = (zlib.crc32(payload) & 0xFFFFFFFF) == crc

    sets = []
    if crc_ok:
        off = 0
        for _ in range(set_count):
            timestamp, count = struct.unpack_from("<IB", payload, off)
            off += 5
            readings = []
            for _ in range(count):
                sensor_id, raw = struct.unpack_from("<Bh", payload, off)
                off += 3
                readings.append((sensor_id, raw))
            sets.append((timestamp, readings))

    return {
        "transfer_id": transfer_id,
        "seq": seq,
        "total_packets": total_packets,
        "flags": flags,
        "crc_ok": crc_ok,
        "sets": sets,
    }


def fetch_labels(sock, addr):
    """`labels` -> [(romcode_str, label), ...], the full romcode->location
    table (labels.dat) -- independent of what's on the bus right now. For
    the operator-triggered --labels refresh, not the normal pull path."""
    sock.settimeout(SETUP_TIMEOUT)
    sock.sendto(b"labels", addr)
    data, _ = sock.recvfrom(BUFSIZE)
    lines = data.decode().strip("\n").splitlines()
    if not lines or not lines[0].startswith("labels:"):
        raise RuntimeError("unexpected 'labels' reply: {!r}".format(data))
    n = int(lines[0].split()[1])
    entries = []
    for line in lines[1:1 + n]:
        romcode_str, _, label = line.partition(" ")
        entries.append((romcode_str, label))
    return entries


def write_labels_csv(entries, path):
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["romcode", "label"])
        writer.writerows(entries)


def fetch_config(sock, addr):
    """`config get` -> (max_retries, retry_interval_ms)."""
    sock.settimeout(SETUP_TIMEOUT)
    sock.sendto(b"config get", addr)
    data, _ = sock.recvfrom(BUFSIZE)
    text = data.decode().strip()
    m = re.match(r"config: max_retries=(\d+) retry_interval_ms=(\d+)", text)
    if not m:
        raise RuntimeError("unexpected 'config get' reply: {!r}".format(data))
    return int(m.group(1)), int(m.group(2))


def load_seen_keys(path):
    """(sensor_id, timestamp) keys already on disk, so a re-pulled packet
    (retry, or a whole session re-offered after a prior abandoned run) is
    skipped rather than duplicated."""
    seen = set()
    if not os.path.exists(path):
        return seen
    with open(path, newline="") as f:
        reader = csv.reader(f)
        next(reader, None)  # header
        for row in reader:
            if len(row) < 2:
                continue
            seen.add((int(row[1]), int(row[0])))  # (sensor_id, timestamp_epoch)
    return seen


def write_sets(sets, writer, csv_file, seen_keys):
    for timestamp, readings in sets:
        for sensor_id, raw in readings:
            key = (sensor_id, timestamp)
            if key in seen_keys:
                continue
            seen_keys.add(key)
            iso = datetime.datetime.fromtimestamp(
                timestamp, tz=datetime.timezone.utc).isoformat()
            writer.writerow([
                timestamp,
                sensor_id,
                iso,
                "{:.4f}".format(raw / 16.0),
            ])
    csv_file.flush()


def request_response(sock, addr, send_bytes, timeout_s):
    sock.settimeout(timeout_s)
    sock.sendto(send_bytes, addr)
    try:
        data, _ = sock.recvfrom(BUFSIZE)
        return data
    except socket.timeout:
        return None


def send_final_ack(sock, addr, transfer_id, seq, max_retries, retry_interval_s):
    """Confirms the final packet so the device advances its watermark. A
    lost confirmation is not fatal here — data is already written, and the
    device just re-offers the same range next REQUEST (safe, deduped)."""
    ack = pack_ack(transfer_id, seq)
    for _ in range(max_retries + 1):
        if request_response(sock, addr, ack, retry_interval_s) is not None:
            return True
    return False


def run_transfer(sock, addr, max_retries, retry_interval_s,
                  writer, csv_file, seen_keys):
    expected_seq = 0
    transfer_id = None
    outbound = msg_header(XFER_MSG_REQUEST)
    attempts = 0

    while True:
        raw = request_response(sock, addr, outbound, retry_interval_s)

        if raw is None:
            attempts += 1
            if attempts > max_retries:
                print("collector: giving up -- no response after {} "
                      "retries".format(max_retries), file=sys.stderr)
                return False
            continue

        pkt = parse_data_packet(raw)
        if pkt is None:
            attempts += 1
            if attempts > max_retries:
                print("collector: giving up -- malformed reply", file=sys.stderr)
                return False
            continue

        if transfer_id is None:
            transfer_id = pkt["transfer_id"]
        elif pkt["transfer_id"] != transfer_id or pkt["seq"] != expected_seq:
            # Stale packet from a superseded transfer, or a duplicate/
            # crossed-in-flight resend of one we've already moved past.
            continue

        if not pkt["crc_ok"]:
            attempts += 1
            if attempts > max_retries:
                sock.sendto(pack_nack(transfer_id, [expected_seq]), addr)
                print("collector: giving up -- CRC failures on packet {}"
                      .format(expected_seq), file=sys.stderr)
                return False
            outbound = pack_nack(transfer_id, [expected_seq])
            continue

        attempts = 0
        write_sets(pkt["sets"], writer, csv_file, seen_keys)

        if pkt["flags"] & XFER_FLAG_END:
            if not send_final_ack(sock, addr, transfer_id, expected_seq,
                                   max_retries, retry_interval_s):
                print("collector: warning -- final ACK unconfirmed, "
                      "watermark may not have advanced (safe to retry)",
                      file=sys.stderr)
            return True

        outbound = pack_ack(transfer_id, expected_seq)
        expected_seq += 1


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=HOST,
                     help="Pico W address (default %(default)s)")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--csv", default="temp_data.csv",
                     help="output CSV path (default %(default)s)")
    ap.add_argument("--labels", action="store_true",
                     help="fetch the device's romcode->location table into "
                          "--labels-file and exit, instead of pulling "
                          "readings. Run this by hand once a sensor-"
                          "addition session is complete (see "
                          "label_store.h) -- it is never done as part of "
                          "a normal pull.")
    ap.add_argument("--labels-file", default="sensor_labels.csv",
                     help="output path for --labels (default %(default)s)")
    args = ap.parse_args()
    addr = (args.host, args.port)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    if args.labels:
        try:
            entries = fetch_labels(sock, addr)
        except (socket.timeout, RuntimeError) as e:
            sock.close()
            raise SystemExit("collector: labels fetch failed: {}".format(e))
        sock.close()
        write_labels_csv(entries, args.labels_file)
        print("collector: wrote {} label(s) to {}".format(
            len(entries), args.labels_file))
        return

    try:
        max_retries, retry_interval_ms = fetch_config(sock, addr)
    except (socket.timeout, RuntimeError) as e:
        sock.close()
        raise SystemExit("collector: setup failed: {}".format(e))
    retry_interval_s = retry_interval_ms / 1000.0

    seen_keys = load_seen_keys(args.csv)
    file_exists = os.path.exists(args.csv)
    with open(args.csv, "a", newline="") as f:
        writer = csv.writer(f)
        if not file_exists:
            writer.writerow(
                ["timestamp_epoch", "sensor_id", "timestamp_utc", "temp_c"])
            f.flush()
        ok = run_transfer(sock, addr, max_retries, retry_interval_s,
                           writer, f, seen_keys)
    sock.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
