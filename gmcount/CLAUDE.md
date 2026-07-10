# gmcount

Project-specific guidance for `gmcount`, the main project in this repo. The
root `CLAUDE.md` covers repo-wide conventions (external deps, build/flash/
serial console commands) that apply here too — Claude Code loads both when
you're working in this directory.

## Current status

This project gets worked on in short, irregular sessions, so keep this
section up to date as a "where I left off" note — update it at the end of a
session rather than trying to reconstruct state from git log next time.

- **Last updated:** 2026-07-10
- **Doing now:** wiring up gmcount's real-time data collection path (pulse
  counting → RTC-timestamped records → SD logging).
- **Known gap:** `fatfs.c` (`init_FatFs`) currently just mounts the SD card,
  appends a fixed `"Hello, world!"` line to `filename.txt`, and unmounts —
  it does not yet write real per-count records. That's the next piece of the
  pipeline to build.
- **Next up:** (nothing queued yet — update this when you pick a next step)

## Architecture

gmcount wires together four subsystems in `gmcount.c`:
- **Pulse counting**: a GPIO interrupt (`gpio_callback`, rising edge on GPIO
  22) increments `count_total`/`count_per_interval` from ISR context. The
  main loop hands off a snapshot every 5s via the
  `report_interval_count` / `count_per_interval_report` flag-and-copy
  handshake (not a mutex) — keep that pattern in mind if touching the ISR or
  the reporting loop, it's how the two contexts stay in sync.
- **RTC timestamping**: DS3231 over I2C1 (SDA=GPIO26, SCL=GPIO27), via
  `api_ds3231.c/h`.
- **SD logging**: FatFs over SPI0 (SCK=18, MOSI=19, MISO=16, CS=17), wired up
  in `hw_config.c` (SPI/SD hardware description) and `fatfs.c`
  (`init_FatFs`/`close_FatFs`/`listFiles`). Currently just mounts, appends a
  fixed "Hello, world!" line to `filename.txt`, and unmounts — this is a
  placeholder, not the real logging path yet.
- **WiFi/UDP**: `wifi.c` brings up the Pico W (`cyw43_arch`) with hardcoded
  SSID/password/auth constants at file scope, then listens for UDP commands
  on port 8080 and replies with a canned response. Don't change the
  credentials without checking with the user — they're real network config,
  not placeholders.

## Shared/duplicated files, not a common library

`api_ds3231.c/h` (DS3231 driver, BSD-3-licensed, upstream by Antonio
González) is copied verbatim into both `gmcount/` and `../rtc/`.
`hw_config.c`/`fatfs.c` (SD/FatFs glue, Apache-2.0, upstream
carlk3/no-OS-FatFS-SD-SPI-RPi-Pico pattern) are copied with minor variations
into `gmcount/` and `../sdsc/` — this project's version adds
`close_FatFs`/`listFiles`/`ls` on top of the base example. When fixing a bug
in one copy, check whether the same bug exists in the sibling copy; they are
not symlinked or built as a shared library.

## Related testbeds

- **`../rtc/`** is the DS3231 driver testbed: `rtc.c` is a minimal main that
  calls into `use_ds3231.c`'s `example_ds3231()`, which exercises the same
  `api_ds3231` API gmcount uses. Useful reference for expected DS3231 usage
  before changing the shared driver.
- **`../wifi/` vs `../wifi2/`**: `wifi/` is a working Pico W UDP client/
  server, the reference for gmcount's `wifi.c`. `wifi2/` is a newer,
  RP2350/Pico2 W bring-up target — its `main.c` has the equivalent WiFi/UDP
  setup present but commented out, currently just prints a heartbeat in a
  loop. Not yet integrated into gmcount; treat it as in-progress platform
  bring-up, not a regression.
