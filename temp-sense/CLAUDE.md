# temp-sense

Project-specific guidance for `temp-sense`. The root `CLAUDE.md` covers repo-wide
conventions (external deps, build/flash/serial console commands) that apply
here too — Claude Code loads both when you're working in this directory.

A temperature sensor data logger — same shape as `../gmcount/` (sensor input
→ RTC-timestamped record → SD/WiFi output) but for temperature instead of
Geiger-Müller pulses.

## Current status

- **Last updated:** 2026-07-25
- **Done:** Hardware verified on real DS18B20 sensors (3 devices detected and working).
  Fixed two pico-examples bugs: Issue #422 (conversion wait timing) and Issue #569 (CRC
  validation). All sensor reads now pass CRC validation; no bus errors detected.
  WiFi/UDP is now live: connects via a new shared `../common/wifi/` library (`picowifi`,
  extracted from gmcount's `wifi.c` so it isn't duplicated per-project — see
  `../common/wifi/wifi.h`), listens on UDP port 8080, and answers a `read` command with
  the latest DS18B20 readings (`device N: XX.XX C`), sourced from shared globals in
  `temp_store.h` that `use_ds18b20.c` refreshes each cycle. Any other command still gets
  a generic ack. Verified hardware round-trip with `udp_client.py` (points at
  `192.168.1.120` — update if the Pico W's DHCP lease changes). Credentials live in a
  gitignored `wifi_secrets.h` (currently reusing gmcount's `abzu2` network).
  Fixed a real DS3231 hardware bug found on this board: SDA/SCL were wired to GPIO26/27
  (I2C1), which hung the per-cycle RTC read (`ds3231_get_datetime()`) indefinitely on a
  NACKed I2C transaction — silently killing the whole sensor loop (and, with it,
  `wifi_udp_poll()`, so UDP stopped responding too). Rewired to GPIO8/9 (I2C0); see
  Architecture below. Also fixed `cyw43_arch_wifi_connect_blocking()` intermittently
  failing on the first attempt or two (`wifi_connect()` in `../common/wifi/wifi.c` now
  retries up to 5 times with a 1s delay, logging each failed attempt) — confirmed
  connecting reliably across repeated power-cycles.
- **Known gap:** no SD (FatFs) logging yet — WiFi is query-on-demand only, nothing is
  persisted to storage. `ONEWIRE_GPIO_PIN` is set to 15 (GPIO 15 → DS18B20 DQ, external
  ~4k pull-up to 3V3). DS3231's clock has never been set (reads back `Jan 01 2000`) —
  needs a one-off `ds3231_set_datetime()` call.
- **Next up:** planned 3-phase WiFi path — (1) hello-world UDP echo [done], (2) useful
  on-demand query capability [done, `read` command], (3) migrate to MQTT (lwIP already
  vendors an MQTT client at `pico-sdk/lib/lwip/src/apps/mqtt`, so no new dependency
  needed). Priority is to iterate on WiFi here in temp-sense first, then backport the
  shared `common/wifi` usage to gmcount once this is working. SD logging (reuse
  `../sdsc/` hw_config.c/fatfs.c pattern) is a separate, still-unstarted piece.

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
- **RTC timestamping**: DS3231 over I2C0 (SDA=GPIO8, SCL=GPIO9 — this
  board's wiring; note gmcount uses I2C1/GPIO26/27 instead), via
  `api_ds3231.c/h` copied verbatim from `../rtc/` (same duplication pattern
  as `../gmcount/`).
- `CMakeLists.txt` follows `../rtc/`'s shape (single `add_executable`, no
  FatFs) plus `add_subdirectory(onewire_library)` and `hardware_pio` for the
  PIO driver.

See `../gmcount/CLAUDE.md` for how the SD/WiFi pieces fit together once
they're added here.
