#!/usr/bin/env python3

import argparse
import subprocess
import sys

DBG_PRODUCT = "Debugprobe on Pico (CMSIS-DAP)"
TTY_UUID_FALLBACK = "4581ef31-0115-4b2a-a1c9-824356e81fd0"


def get_usbhost_devices():
    result = subprocess.run(["vboxmanage", "list", "usbhost"], capture_output=True, text=True)

    devices = []
    current = None
    for line in result.stdout.strip().splitlines():
        line = line.strip()
        if line == "Host USB Devices:" or not line:
            current = None
            continue
        parts = line.split(": ", 1)
        if len(parts) == 2:
            if current is None:
                current = {}
                devices.append(current)
            current[parts[0].strip()] = parts[1].strip()
        else:
            current = None
    return devices


def resolve_uuid(product, disambiguator=None):
    matches = [d for d in get_usbhost_devices() if d.get("Product") == product]

    if disambiguator:
        matches = [d for d in matches if disambiguator in d.values()]

    if not matches:
        print(f"no USB device found with Product == '{product}'", file=sys.stderr)
        return None

    if len(matches) > 1:
        print(f"ambiguous: {len(matches)} devices match Product == '{product}'", file=sys.stderr)
        for m in matches:
            print(f"  UUID={m.get('UUID')} Address={m.get('Address')} SerialNumber={m.get('SerialNumber')}", file=sys.stderr)
        print("pick a unique field value (e.g. Address) to disambiguate", file=sys.stderr)
        return None

    return matches[0]["UUID"]


def usb_action(node, uuid, action):
    print(f"UUID={uuid}")
    result = subprocess.run(["VBoxManage", "controlvm", node, action, uuid])
    if result.returncode != 0:
        print(f"VBoxManage controlvm {action} failed", file=sys.stderr)
        sys.exit(1)


def add_device_args(subparser):
    subparser.add_argument("--vm", required=True, help="name of the VirtualBox VM")
    subparser.add_argument("--detach", action="store_true", help="detach instead of attach")


def main():
    parser = argparse.ArgumentParser(description="attach/detach usb device to/from VB guest")
    subparsers = parser.add_subparsers(dest="command", required=True)

    dbg_parser = subparsers.add_parser("dbg", help="attach/detach RaspBerry Pi Pico Debugprobe")
    add_device_args(dbg_parser)

    tty_parser = subparsers.add_parser("tty", help="attach/detach USB Serial Adapter")
    add_device_args(tty_parser)

    subparsers.add_parser("list", help="list available usb devices on this host")
    subparsers.add_parser("help", help="show this help message")

    args = parser.parse_args()

    if args.command == "list":
        print("list available usb devices on this host")
        subprocess.run(["vboxmanage", "list", "usbhost"])
        return

    if args.command == "help":
        parser.print_help()
        return

    verb = "detach" if args.detach else "attach"

    if args.command == "dbg":
        print(f"usb {verb}: dbg")
        uuid = resolve_uuid(DBG_PRODUCT)
        if uuid is None:
            sys.exit(1)
    else:  # tty
        print(f"usb {verb}: tty")
        # TODO: hardcoded UUID goes stale when VirtualBox reassigns it.
        # Once the adapter's `Product:` string is known (run
        # `./vbox-usb-attach.py list` with it attached), switch to:
        #   uuid = resolve_uuid("<product name>")
        #   if uuid is None:
        #       sys.exit(1)
        uuid = TTY_UUID_FALLBACK

    usb_action(args.vm, uuid, "usbdetach" if args.detach else "usbattach")
    print(f"{sys.argv[0]} done")


if __name__ == "__main__":
    main()
