#!/usr/bin/env python3
"""Pull pending readings from the temp-sense Pico W over the v1.2 binary
UDP protocol (see temp-logger-udp-protocol.md) and store them in a SQLite
database.

By default does exactly one REQUEST -> DATA/ACK/NACK -> (watermark advance)
cycle per invocation, then exits -- meant to be run periodically by cron/a
systemd timer (the protocol doc's "hourly, normally"). Pass --loop to run
forever instead, as a container's main process: see the --loop section
below.

Usage:
    ./collector.py                       # pull into ./temp_sense.db
    ./collector.py --db-path /path/to.db
    ./collector.py --host 1.2.3.4
    ./collector.py --table               # refresh the sensor table instead
    ./collector.py --loop                # run forever; see below

--loop mode: runs forever, pulling once and then sleeping until the next
cycle, repeating until SIGTERM. The interval is read from --config-path
(default ./collector.conf, a plain-text `interval_seconds=N` line) fresh
at the top of every cycle -- not just once at startup -- so it can be
changed with a text edit while the process keeps running. A missing or
unparseable config file falls back to DEFAULT_INTERVAL_SECONDS (the
protocol's normal hourly cadence) rather than erroring, so a bad edit
degrades to "runs hourly" instead of crash-looping. A single failed poll
(network timeout, stale sensor table, etc.) is logged and skipped rather
than ending the loop -- the next cycle tries again.

Stopping --loop mode: SIGTERM (what `podman stop` sends) and SIGINT
(Ctrl-C) both request a clean exit. The loop sleeps in 1-second
increments rather than one long sleep so it notices the request and exits
within about a second, whether it's idle or mid-poll -- an interrupted
poll isn't a correctness problem (see the note on retries below).

Exit status is 0 on a fully-completed transfer, 1 if it gave up partway
(no response, or a packet failed CRC past the device's configured
max_retries). A partial run is always safe to just retry next cycle: the
device only advances its watermark on a confirmed final ACK, and rows are
deduped by the database's own (label, timestamp_epoch) primary key, so
re-pulled data is a no-op (`INSERT OR IGNORE`).

The wire sensor_id is only a transport-layer shorthand -- it exists to keep
DATA packets compact and carries no identity of its own (see
label_store.h). This script resolves it to the device's current label
immediately, from the database's `sensors` table, and stores readings by
label rather than by index -- run --table by hand once after adding a
sensor (it is never fetched automatically as part of a normal pull), and a
normal pull refuses to run if the `sensors` table is empty.

Database schema (created automatically if the file doesn't exist yet):
    sensors(id INTEGER PRIMARY KEY, romcode TEXT UNIQUE, label TEXT)
    readings(timestamp_epoch INTEGER, label TEXT, timestamp_utc TEXT,
              temp_c REAL, valid INTEGER,
              PRIMARY KEY (timestamp_epoch, label))
journal_mode is set to WAL so a separate read-only report process can query
the database concurrently without blocking, or being blocked by, a pull in
progress.
"""

import argparse
import datetime
import os
import re
import signal
import socket
import sqlite3
import struct
import sys
import time
import zlib

HOST = "192.168.1.120"
PORT = 8080
BUFSIZE = 1024
SETUP_TIMEOUT = 5  # seconds, for the plain-ASCII bootstrap commands
DEFAULT_INTERVAL_SECONDS = 3600  # the protocol's normal cadence; --loop's fallback

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
    explicit registration changes it, see label_store.h). For the
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


def init_db(conn):
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute(
        "CREATE TABLE IF NOT EXISTS sensors ("
        "id INTEGER PRIMARY KEY, romcode TEXT UNIQUE, label TEXT)")
    conn.execute(
        "CREATE TABLE IF NOT EXISTS readings ("
        "timestamp_epoch INTEGER, label TEXT, timestamp_utc TEXT, "
        "temp_c REAL, valid INTEGER, "
        "PRIMARY KEY (timestamp_epoch, label))")
    conn.commit()


def replace_sensor_table(conn, entries):
    """Wholesale refresh, mirroring the old write_table_csv's overwrite
    semantics: the device's table is the source of truth, so a --table run
    replaces the local copy rather than merging into it."""
    conn.execute("DELETE FROM sensors")
    conn.executemany(
        "INSERT INTO sensors (id, romcode, label) VALUES (?, ?, ?)",
        entries)
    conn.commit()


def load_table(conn):
    """sensor_id (int) -> label, from the last --table refresh. Raises if
    the table is empty -- a normal pull must not guess at identities it
    hasn't been told."""
    rows = conn.execute("SELECT id, label FROM sensors").fetchall()
    if not rows:
        raise RuntimeError(
            "sensors table is empty -- run `./collector.py --table` at "
            "least once before pulling")
    return dict(rows)


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


def write_sets(sets, conn, table):
    for timestamp, readings in sets:
        for sensor_id, raw, valid in readings:
            if sensor_id not in table:
                raise RuntimeError(
                    "sensor_id {} not in the sensors table -- the device's "
                    "table has changed since the last --table refresh; "
                    "re-run `./collector.py --table` before pulling again"
                    .format(sensor_id))
            label = table[sensor_id]
            iso = datetime.datetime.fromtimestamp(
                timestamp, tz=datetime.timezone.utc).isoformat()
            conn.execute(
                "INSERT OR IGNORE INTO readings "
                "(timestamp_epoch, label, timestamp_utc, temp_c, valid) "
                "VALUES (?, ?, ?, ?, ?)",
                (timestamp, label, iso, raw / 16.0, valid))
    conn.commit()


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


def run_transfer(sock, addr, max_retries, retry_interval_s, conn, table):
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
        write_sets(pkt["sets"], conn, table)

        if pkt["flags"] & XFER_FLAG_END:
            if not send_final_ack(sock, addr, transfer_id, expected_seq,
                                   max_retries, retry_interval_s):
                print("collector: warning -- final ACK unconfirmed, "
                      "watermark may not have advanced (safe to retry)",
                      file=sys.stderr)
            return True

        outbound = pack_ack(transfer_id, expected_seq)
        expected_seq += 1


def poll_once(conn, sock, addr):
    """One pull attempt against the device's currently-known sensor table.
    Returns True on a fully-completed transfer. Any failure (network,
    protocol, or a stale/missing sensor table) is logged and returns False
    rather than raising, so --loop mode can just move on to the next
    cycle instead of dying."""
    try:
        table = load_table(conn)
    except RuntimeError as e:
        print("collector: {}".format(e), file=sys.stderr)
        return False

    try:
        max_retries, retry_interval_ms = fetch_config(sock, addr)
    except (socket.timeout, RuntimeError) as e:
        print("collector: setup failed: {}".format(e), file=sys.stderr)
        return False
    retry_interval_s = retry_interval_ms / 1000.0

    return run_transfer(sock, addr, max_retries, retry_interval_s, conn, table)


def read_interval_or_default(path, default=DEFAULT_INTERVAL_SECONDS):
    """`interval_seconds=N` from a plain key=value config file, read fresh
    on every call rather than cached -- that's what lets --loop's cadence
    be changed with a text edit while the process keeps running. Anything
    that isn't a clean positive integer (missing file, bad syntax, zero,
    negative) falls back to `default` instead of raising, so a typo in the
    file degrades to "runs at the normal cadence" rather than crashing the
    loop."""
    try:
        with open(path) as f:
            for line in f:
                key, _, value = line.partition("=")
                if key.strip() != "interval_seconds":
                    continue
                seconds = int(value.strip())
                if seconds > 0:
                    return seconds
    except (OSError, ValueError):
        pass
    return default


def run_loop(conn, sock, addr, config_path):
    stop_requested = False

    def handle_stop(signum, frame):
        nonlocal stop_requested
        stop_requested = True

    signal.signal(signal.SIGTERM, handle_stop)
    signal.signal(signal.SIGINT, handle_stop)

    while not stop_requested:
        started = datetime.datetime.now(tz=datetime.timezone.utc).isoformat()
        ok = poll_once(conn, sock, addr)
        print("collector: loop cycle at {} -- {}".format(
            started, "ok" if ok else "failed, will retry next cycle"))

        interval = read_interval_or_default(config_path)
        for _ in range(interval):
            if stop_requested:
                break
            time.sleep(1)

    print("collector: stopping (signal received)", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=HOST,
                     help="Pico W address (default %(default)s)")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--db-path", default="temp_sense.db",
                     help="SQLite database path (default %(default)s)")
    ap.add_argument("--table", action="store_true",
                     help="fetch the device's current persistent sensor "
                          "table into the database and exit, instead of "
                          "pulling readings. Run this by hand once after "
                          "adding a sensor (see label_store.h) -- it is "
                          "never done as part of a normal pull.")
    ap.add_argument("--loop", action="store_true",
                     help="run forever instead of a single pull -- see the "
                          "--loop section in this script's module docstring "
                          "(pass -h with no other args, or read the top of "
                          "collector.py). Ignored if --table is also given.")
    ap.add_argument("--config-path", default="collector.conf",
                     help="--loop's interval config file (default "
                          "%(default)s), re-read every cycle")
    args = ap.parse_args()
    addr = (args.host, args.port)

    conn = sqlite3.connect(args.db_path)
    init_db(conn)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    if args.table:
        try:
            entries = fetch_table(sock, addr)
        except (socket.timeout, RuntimeError) as e:
            sock.close()
            conn.close()
            raise SystemExit("collector: table fetch failed: {}".format(e))
        sock.close()
        replace_sensor_table(conn, entries)
        conn.close()
        print("collector: wrote {} sensor(s) to {}".format(
            len(entries), args.db_path))
        return

    if args.loop:
        run_loop(conn, sock, addr, args.config_path)
        sock.close()
        conn.close()
        return

    ok = poll_once(conn, sock, addr)
    sock.close()
    conn.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
