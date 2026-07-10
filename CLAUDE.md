# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository overview

Each top-level directory is an independent Pico SDK/CMake project — its own
executable, built and flashed separately. There is no single repo-wide build.
This file covers conventions shared across all projects (external deps,
build/flash/serial patterns); project-specific architecture and "current
status" notes live in that project's own `CLAUDE.md`, which Claude Code loads
automatically alongside this one when you're working in that directory.

- `gmcount/` — the main project: a Geiger-Müller particle counter/real-time
  data logger for a Pico W. See `gmcount/CLAUDE.md`.
- `rtc/` — testbed for the DS3231 RTC driver (`api_ds3231.c/h`), which
  gmcount uses for timestamping.
- `sdsc/` — testbed for SD-card-over-SPI (FatFs) logging, which gmcount uses
  for on-device storage.
- `wifi/` — testbed for Pico W UDP client/server networking (port 8080),
  which gmcount uses for its command/response interface.
- `wifi2/` — bring-up testbed for the Pico2 W / RP2350 platform; WiFi code is
  present but currently commented out. Not yet integrated into gmcount.
- `temp/` — new project, not yet fleshed out. See `temp/CLAUDE.md`.

See `README.md` for hardware setup (debugprobe, openocd build, minicom,
sigrok/pulseview) — that's reference material for the physical dev
environment, not something to re-derive from source.

## External dependencies (outside this repo)

Every project's `pico_sdk_import.cmake` and build scripts assume these exist
on the host filesystem, already built:

- `$HOME/dev/pico/pico-sdk` — Raspberry Pi Pico SDK (`PICO_SDK_PATH`).
- `$HOME/dev/pico/openocd` — openocd built from source with `--enable-cmsis-dap`
  (stock distro openocd doesn't support the debugprobe).
- `$HOME/dev/pico/no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI` — SD card / FatFs
  library, referenced by **absolute path** in `gmcount/CMakeLists.txt` and
  `sdsc/CMakeLists.txt` via `add_subdirectory(...)`.

If any of these paths don't exist on the current machine, build/flash
commands below will fail at the cmake or link step, not because of a code bug.

## Build

Each project has up to two board variants, selected by which script you stage
into `build/`: `wbuild.sh` (`pico_w`/WiFi-capable board) and, where present,
`xbuild.sh` (plain `pico`/RP2040 board, no WiFi). Both just set
`PICO_SDK_PATH`, run `cmake ..`, then `make` — they must be run from inside
`build/`. `build/` is gitignored; there is no top-level build script, and each
project is built independently.

`wifi/` and `wifi2/` have **no `xbuild.sh`** — WiFi is the point of those
projects, so only the `pico_w`/RP2350 variant exists.

`init_project.sh` (present in `gmcount`, `rtc`, `wifi`, `wifi2` — **not**
`sdsc`) wipes `build/` and stages `wbuild.sh` into it:

```bash
cd <project>
./init_project.sh        # rm -rf build/*; cp wbuild.sh build/
cd build
./wbuild.sh
```

`init_project.sh` never stages `xbuild.sh`. To build the non-W variant (where
it exists) or to build `sdsc` (no `init_project.sh` at all), stage manually:

```bash
mkdir -p build && cp xbuild.sh build/ && cd build && ./xbuild.sh
```

Per-project quirks:
- `wifi2/wbuild.sh` targets RP2350 explicitly: `cmake -DPICO_PLATFORM=rp2350-arm-s ..`.
- `gmcount` and `sdsc` link `FatFs_SPI` via the absolute-path `add_subdirectory`
  above; `rtc`, `wifi`, `wifi2` do not use the SD card library.
- Outputs land in `build/`: `<name>.elf`, `.uf2`, `.hex`, `.bin`, `.dis`, `.elf.map`.
- `wifi2/xload_wifi.sh` is stale leftover cruft (a copy of `wifi/load_wifi.sh`
  that still points at `wifi/build/main.elf`, not `wifi2`'s). Use
  `wifi2/load_wifi.sh` instead — it correctly targets `wifi2/build/main.elf`
  with `rp2350.cfg`.

There is no unit test suite in this repo (it's embedded firmware); "testing"
means building, flashing to a physical Pico, and observing serial output —
see Load/flash and Serial console below.

## Load/flash

Each project has a `load_<name>.sh` script that flashes the built `.elf` via
openocd + a CMSIS-DAP debug probe over SWD:

```bash
./load_gmcount.sh   # or load_rtc.sh, load_sdsc.sh, load_wifi.sh
```

Note: both `wifi/` and `wifi2/` name their script `load_wifi.sh` (same name,
different directories — not a duplicate, don't confuse the two).

This runs `sudo openocd -s tcl -f <interface-cfg> -f <target-cfg> -c "adapter speed 5000" -c "program <elf> verify reset exit"`.
RP2040 boards (`gmcount`, `rtc`, `wifi`, `sdsc`) use `tcl/target/rp2040.cfg`;
`wifi2` (RP2350/Pico2 W) uses `tcl/target/rp2350.cfg`. Requires `sudo` and the
debug probe to be connected/captured (see README for VirtualBox USB capture
notes if developing in a VM).

## Serial console

`start_minicom.sh` in each project dir opens the target's serial output:
`sudo minicom -D /dev/ttyACM0 -b 115200` (debug probe) — an alternate line for
`/dev/ttyUSB0` (USB-serial dongle) is present but commented out. Start
minicom *before* flashing so early boot output isn't missed.
