#!/usr/bin/env python3
"""Poll the temp-sense Pico W's `read` command repeatedly and log every
romcode whose reading drops below a cold threshold -- used to empirically
identify which physical sensor is currently sitting in an ice bath, when
sensor_table.csv labels are suspected to be wrong.

Bypasses sensor_table.csv/labels entirely: reports raw romcodes as seen
directly from the device's live `read` output, cross-referenced against
`table` (fetched once at startup) only to show each romcode's current
table index for the `label <index> <name>` step afterward.

Usage:
    ./ice_bath_id.py --host <pico-ip>
    ./ice_bath_id.py --host <pico-ip> --log dunk_log.txt

Runs until Ctrl+C. Every detection is printed immediately and (if --log is
given) appended to a log file with a wall-clock timestamp, so a long
unattended run covering several dunks can be reviewed afterward.
"""

import argparse
import re
import socket
import sys
import time

HOST = "192.168.1.120"
PORT = 8080
BUFSIZE = 2048
TIMEOUT = 5  # seconds, per read request

COLD_THRESHOLD_C = 10.0  # below this = "in the ice bath"

READ_LINE_RE = re.compile(
    r"^(0x[0-9a-f]{16})\s+\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} UTC\s+"
    r"seq\s+\d+\s+(?:([\d.]+) C|CRC error)$")


def fetch_table(sock, addr):
    sock.settimeout(TIMEOUT)
    sock.sendto(b"table", addr)
    data, _ = sock.recvfrom(BUFSIZE)
    lines = data.decode().strip("\n").splitlines()
    if not lines or not lines[0].startswith("table:"):
        raise RuntimeError("unexpected 'table' reply: {!r}".format(data))
    n = int(lines[0].split()[1])
    index_by_romcode = {}
    for line in lines[1:1 + n]:
        idx_str, _, rest = line.partition(" ")
        romcode_str, _, _label = rest.partition(" ")
        index_by_romcode[romcode_str.lower()] = int(idx_str)
    return index_by_romcode


def fetch_read(sock, addr):
    sock.settimeout(TIMEOUT)
    sock.sendto(b"read", addr)
    data, _ = sock.recvfrom(BUFSIZE)
    text = data.decode()
    readings = {}
    for line in text.strip("\n").splitlines():
        line = line.strip()
        if not line or line.startswith("warning:") or line == "no readings yet":
            continue
        m = READ_LINE_RE.match(line)
        if not m:
            continue
        romcode, temp_str = m.groups()
        if temp_str is None:
            continue  # CRC error this cycle -- not a usable sample
        readings[romcode] = float(temp_str)
    return readings


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=HOST)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--poll-interval", type=float, default=1.0,
                     help="seconds between `read` polls (default %(default)s)")
    ap.add_argument("--threshold", type=float, default=COLD_THRESHOLD_C,
                     help="temp_c below this = flagged as in the ice bath "
                          "(default %(default)s)")
    ap.add_argument("--log", default=None,
                     help="append every detection to this file too")
    args = ap.parse_args()
    addr = (args.host, args.port)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    try:
        index_by_romcode = fetch_table(sock, addr)
    except (socket.timeout, RuntimeError) as e:
        sock.close()
        raise SystemExit("ice_bath_id: table fetch failed: {}".format(e))

    logf = open(args.log, "a") if args.log else None

    print("ice_bath_id: {} sensor(s) on the table. Dunk one at a time; "
          "this prints the romcode the moment it drops below {}C."
          .format(len(index_by_romcode), args.threshold), flush=True)
    print("Ctrl+C to stop.\n", flush=True)

    flagged = set()
    try:
        while True:
            try:
                readings = fetch_read(sock, addr)
            except socket.timeout:
                print("ice_bath_id: no reply, retrying...", file=sys.stderr,
                      flush=True)
                time.sleep(args.poll_interval)
                continue

            for romcode, temp_c in readings.items():
                is_cold = temp_c < args.threshold
                if is_cold and romcode not in flagged:
                    flagged.add(romcode)
                    idx = index_by_romcode.get(romcode, "?")
                    line = (">>> {}  table index {}  {}  dropped to {:.2f}C "
                            "-- in the bath now <<<".format(
                                romcode, idx,
                                time.strftime("%Y-%m-%d %H:%M:%S"), temp_c))
                    print(line, flush=True)
                    if logf:
                        logf.write(line + "\n")
                        logf.flush()
                elif not is_cold and romcode in flagged:
                    # Pulled back out -- ready to flag again on the next dunk.
                    flagged.discard(romcode)

            time.sleep(args.poll_interval)
    except KeyboardInterrupt:
        pass
    finally:
        if logf:
            logf.close()
        sock.close()


if __name__ == "__main__":
    main()
