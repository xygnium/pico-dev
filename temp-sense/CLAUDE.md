# temp-sense

Project-specific guidance for `temp-sense`. The root `CLAUDE.md` covers repo-wide
conventions (external deps, build/flash/serial console commands) that apply
here too — Claude Code loads both when you're working in this directory.

A temperature sensor data logger — same shape as `../gmcount/` (sensor input
→ RTC-timestamped record → SD/WiFi output) but for temperature instead of
Geiger-Müller pulses.

## Current status

- **Last updated:** 2026-07-10
- **Doing now:** not started — directory and CLAUDE.md stub only, no source
  or CMake project yet.
- **Known gap:** n/a
- **Next up:** scaffold the project following the pattern used by the other
  projects (`CMakeLists.txt`, `pico_sdk_import.cmake`,
  `wbuild.sh`/`xbuild.sh`, `load_temp-sense.sh`, `start_minicom.sh` — see
  `../rtc/` for the template). No DS18B20/1-Wire driver exists yet in this
  repo (none of the other projects use one), so that'll need to be sourced or
  written fresh — unlike the DS3231/FatFs testbeds, there's no existing
  sibling code to copy from for the sensor itself.

## Architecture

Not yet built. Sensor: **DS18B20** (digital, 1-Wire) — decided, not the
DS3231's on-die `ds3231_get_temperature()`. Expected shape, following
`../gmcount/`'s pattern: 1-Wire read → DS3231 RTC timestamp (reuse
`../rtc/api_ds3231.c/h`) → SD/FatFs log (reuse `../sdsc/` hw_config.c/fatfs.c
pattern) → optionally WiFi/UDP (reuse `../wifi/`). See
`../gmcount/CLAUDE.md` for how those pieces fit together there.
