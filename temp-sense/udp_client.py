#!/usr/bin/env python3
"""Send a command to the temp-sense Pico W and print its reply.

Usage:
    ./udp_client.py                 # prompt for a command (original behaviour)
    ./udp_client.py read            # send a command directly
    ./udp_client.py settime         # set the RTC from this host's clock, as UTC
    ./udp_client.py --host 1.2.3.4 read

The device keeps its clock in UTC, so timestamps it reports are UTC. This
script also prints the local equivalent for convenience.
"""

import argparse
import datetime
import socket

HOST = "192.168.1.120"
PORT = 8080
BUFSIZE = 1024
TIMEOUT = 5


def build_settime():
    """Format the current UTC time for the Pico's settime command.

    api_ds3231.h wants day-of-week 1..7 with 1=Monday, which is exactly what
    Python's isoweekday() returns. Sending already-broken-down time keeps all
    timezone handling on this side — the Pico just stores the digits.

    UTC rather than local time so the RTC never needs re-setting at a DST
    transition (it has no timezone rules and would otherwise sit an hour wrong
    until noticed), and so a future stored log stays monotonic across the
    autumn fall-back hour. This host's own timezone setting is irrelevant —
    the system clock is UTC-based underneath and we just ask for that view.
    """
    now = datetime.datetime.now(datetime.timezone.utc)
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

    # Bare `settime` means "use this host's clock, expressed as UTC".
    if cmd.strip() == "settime":
        cmd = build_settime()
        now = datetime.datetime.now(datetime.timezone.utc)
        print("sending UTC {} (local {})".format(
            now.strftime("%Y-%m-%d %H:%M:%S"),
            now.astimezone().strftime("%Y-%m-%d %H:%M:%S %Z")))

    # `format` destroys everything on the card. The device itself refuses
    # anything but the exact confirmation token, but prompt here too so a
    # bare `format` typed interactively doesn't silently send it.
    if cmd.strip() == "format":
        confirm = input(
            "This destroys all data on the SD card. Type 'yes' to confirm: ")
        if confirm.strip().lower() != "yes":
            raise SystemExit("format cancelled")
        cmd = "format yes-erase-the-card"

    print("cmd=", cmd)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(TIMEOUT)
    try:
        sock.sendto(cmd.encode("ascii"), (args.host, args.port))
        payload, _addr = sock.recvfrom(BUFSIZE)
        print(payload.decode())
        print("(device timestamps above are UTC; local now is {})".format(
            datetime.datetime.now().astimezone().strftime(
                "%Y-%m-%d %H:%M:%S %Z")))
    except socket.timeout:
        raise SystemExit("no reply from {}:{} within {}s".format(
            args.host, args.port, TIMEOUT))
    finally:
        sock.close()


if __name__ == "__main__":
    main()
