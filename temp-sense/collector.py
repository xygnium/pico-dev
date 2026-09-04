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
    ./collector.py --table               # refresh sensor_table.csv instead

Exit status is 0 on a fully-completed transfer, 1 if it gave up partway
(no response, or a packet failed CRC past the device's configured
max_retries). A partial run is always safe to just retry next cycle: the
device only advances its watermark on a confirmed final ACK, and rows are
deduped here by (label, timestamp), so re-pulled data is a no-op.

The wire sensor_id is only a transport-layer shorthand -- it exists to keep
DATA packets compact, and its only stability guarantee is "constant for the
life of any unconfirmed backlog on the device" (see label_store.h and
OPERATIONS.md's decomm/wipetable workflow). This script resolves it to the
device's current label immediately, from sensor_table.csv, and stores rows
by label rather than by index -- run --table by hand once after any
add/decomm/wipetable roster change (it is never fetched automatically as
part of a normal pull), and a normal pull refuses to run if sensor_table.csv
doesn't exist yet.

Note: a temp_data.csv written by an older version of this script is keyed
by raw sensor_id, not label, and its `valid` column doesn't exist -- it
won't merge cleanly with rows this version writes. Archive or rename an
existing file before first use of this version.
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
                sensor_id, raw, valid = struct.unpack_from("<BhB", payload, off)
                off += 4
                readings.append((sensor_id, raw, valid))
            sets.append((timestamp, readings))

    return {
        "transfer_id": transfer_id,
        "seq": seq,
        "total_packets": total_packets,
        "flags": flags,
        "crc_ok": crc_ok,
        "sets": sets,
    }


def fetch_table(sock, addr):
    """`table` -> [(index, romcode_str, label), ...], the device's current
    persistent sensor table -- index is stable across reboots (only an
    explicit register/decomm/wipe changes it, see label_store.h). For the
    operator-triggered --table refresh, not the normal pull path."""
    sock.settimeout(SETUP_TIMEOUT)
    sock.sendto(b"table", addr)
    data, _ = sock.recvfrom(BUFSIZE)
    lines = data.decode().strip("\n").splitlines()
    if not lines or not lines[0].startswith("table:"):
        raise RuntimeError("unexpected 'table' reply: {!r}".format(data))
    n = int(lines[0].split()[1])
    entries = []
    for line in lines[1:1 + n]:
        idx_str, _, rest = line.partition(" ")
        romcode_str, _, label = rest.partition(" ")
        entries.append((int(idx_str), romcode_str, label))
    return entries


def write_table_csv(entries, path):
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["index", "romcode", "label"])
        writer.writerows(entries)


def load_table(path):
    """sensor_id (int) -> label, from a --table refresh. Raises if the file
    doesn't exist -- a normal pull must not guess at identities it hasn't
    been told."""
    if not os.path.exists(path):
        raise RuntimeError(
            "{} not found -- run `./collector.py --table` at least once "
            "before pulling".format(path))
    table = {}
    with open(path, newline="") as f:
        reader = csv.reader(f)
        next(reader, None)  # header
        for row in reader:
            if len(row) < 3:
                continue
            table[int(row[0])] = row[2]
    return table


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
    """(label, timestamp) keys already on disk, so a re-pulled packet
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
            seen.add((row[1], int(row[0])))  # (label, timestamp_epoch)
    return seen


def write_sets(sets, writer, csv_file, seen_keys, table):
    for timestamp, readings in sets:
        for sensor_id, raw, valid in readings:
            if sensor_id not in table:
                raise RuntimeError(
                    "sensor_id {} not in sensor_table.csv -- the device's "
                    "table has changed since the last --table refresh; "
                    "re-run `./collector.py --table` before pulling again"
                    .format(sensor_id))
            label = table[sensor_id]
            key = (label, timestamp)
            if key in seen_keys:
                continue
            seen_keys.add(key)
            iso = datetime.datetime.fromtimestamp(
                timestamp, tz=datetime.timezone.utc).isoformat()
            writer.writerow([
                timestamp,
                label,
                iso,
                "{:.4f}".format(raw / 16.0),
                valid,
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
                  writer, csv_file, seen_keys, table):
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
        write_sets(pkt["sets"], writer, csv_file, seen_keys, table)

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
    ap.add_argument("--table", action="store_true",
                     help="fetch the device's current persistent sensor "
                          "table into --table-file and exit, instead of "
                          "pulling readings. Run this by hand once after "
                          "any add/decomm/wipetable roster change (see "
                          "label_store.h) -- it is never done as part of "
                          "a normal pull.")
    ap.add_argument("--table-file", default="sensor_table.csv",
                     help="output/input path for the sensor table "
                          "(default %(default)s)")
    args = ap.parse_args()
    addr = (args.host, args.port)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    if args.table:
        try:
            entries = fetch_table(sock, addr)
        except (socket.timeout, RuntimeError) as e:
            sock.close()
            raise SystemExit("collector: table fetch failed: {}".format(e))
        sock.close()
        write_table_csv(entries, args.table_file)
        print("collector: wrote {} sensor(s) to {}".format(
            len(entries), args.table_file))
        return

    try:
        table = load_table(args.table_file)
    except RuntimeError as e:
        sock.close()
        raise SystemExit("collector: {}".format(e))

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
                ["timestamp_epoch", "label", "timestamp_utc", "temp_c",
                 "valid"])
            f.flush()
        ok = run_transfer(sock, addr, max_retries, retry_interval_s,
                           writer, f, seen_keys, table)
    sock.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
