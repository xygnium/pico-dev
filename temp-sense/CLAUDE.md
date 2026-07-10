# temp-sense

Project-specific guidance for `temp-sense`. The root `CLAUDE.md` covers repo-wide
conventions (external deps, build/flash/serial console commands) that apply
here too — Claude Code loads both when you're working in this directory.

A temperature sensor data logger — same shape as `../gmcount/` (sensor input
→ RTC-timestamped record → SD/WiFi output) but for temperature instead of
Geiger-Müller pulses.

## Current status

- **Last updated:** 2026-07-10
- **Doing now:** initial scaffold is built and compiles clean (`temp-sense.elf`
  produced via `xbuild.sh`, RP2040/`pico` board, not yet flashed to hardware
  or physically tested against a real DS18B20).
- **Known gap:** no SD (FatFs) logging or WiFi/UDP yet — currently just prints
  timestamped readings over serial. `ONEWIRE_GPIO_PIN` is set to 15 (matching
  the pico-examples wiring diagram: GPIO 15 → DS18B20 DQ, with an external
  ~4k pull-up to 3V3 — the internal pull-up is too weak for 1-Wire). Not yet
  verified against physical hardware.
- **Next up:** wire up a real DS18B20 and flash/verify on hardware, then add
  SD logging (reuse `../sdsc/` hw_config.c/fatfs.c pattern) and optionally
  WiFi/UDP (reuse `../wifi/`), following `../gmcount/`'s pattern.

## Architecture

Sensor: **DS18B20** (digital, 1-Wire), not the DS3231's on-die
`ds3231_get_temperature()`. Current shape (`temp-sense.c` →
`use_ds18b20.c`):
- **1-Wire/DS18B20**: `onewire_library/` (PIO-based 1-Wire driver) plus
  `ds18b20.h`/`ow_rom.h` (command codes), copied from
  `/home/mike/dev/pico/pico-examples/pio/onewire/` — no prior driver existed
  in this repo, unlike the RTC/SD testbeds. `use_ds18b20.c`'s
  `example_ds18b20()` scans the bus for connected devices, then loops:
  triggers a parallel temperature conversion on all devices, reads each
  back, and prints `<timestamp>\tdevice <n>: <temp> C` — mirrors
  `pico-examples/pio/onewire/onewire.c`'s structure.
- **RTC timestamping**: DS3231 over I2C1 (SDA=GPIO26, SCL=GPIO27), via
  `api_ds3231.c/h` copied verbatim from `../rtc/` (same duplication pattern
  as `../gmcount/`).
- `CMakeLists.txt` follows `../rtc/`'s shape (single `add_executable`, no
  FatFs) plus `add_subdirectory(onewire_library)` and `hardware_pio` for the
  PIO driver.

See `../gmcount/CLAUDE.md` for how the SD/WiFi pieces fit together once
they're added here.
