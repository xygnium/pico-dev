#!/usr/bin/env python3
"""Send a command to the temp-sense Pico W and print its reply.

Usage:
    ./udp_client.py                 # prompt for a command (original behaviour)
    ./udp_client.py read            # send a command directly
    ./udp_client.py settime         # set the RTC from this host's local clock
    ./udp_client.py --host 1.2.3.4 read
"""

import argparse
import datetime
import socket

HOST = "192.168.1.120"
PORT = 8080
BUFSIZE = 1024
TIMEOUT = 5


def build_settime():
    """Format this host's current local time for the Pico's settime command.

    api_ds3231.h wants day-of-week 1..7 with 1=Monday, which is exactly what
    Python's isoweekday() returns. Sending already-broken-down local time
    keeps timezone handling on this side — the Pico just stores the digits.
    """
    now = datetime.datetime.now()
    return "settime {:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d} {:d}".format(
        now.year, now.month, now.day,
        now.hour, now.minute, now.second, now.isoweekday())


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", nargs="*",
                    help="command to send; omit to be prompted")
    ap.add_argument("--host", default=HOST,
                    help="Pico W address (default %(default)s; update if the "
                         "DHCP lease changes)")
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()

    if args.command:
        cmd = " ".join(args.command)
    else:
        print("Enter command:")
        cmd = input()

    # Bare `settime` means "use this host's clock".
    if cmd.strip() == "settime":
        cmd = build_settime()

    print("cmd=", cmd)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(TIMEOUT)
    try:
        sock.sendto(cmd.encode("ascii"), (args.host, args.port))
        payload, _addr = sock.recvfrom(BUFSIZE)
        print(payload.decode())
    except socket.timeout:
        raise SystemExit("no reply from {}:{} within {}s".format(
            args.host, args.port, TIMEOUT))
    finally:
        sock.close()


if __name__ == "__main__":
    main()
