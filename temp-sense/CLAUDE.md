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
  the latest DS18B20 readings, one rom-keyed line per sensor. Any other command still
  gets a generic ack. Verified hardware round-trip with `udp_client.py` (points at
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
  The DS3231's clock is now settable over the same UDP channel: `settime YYYY-MM-DD
  HH:MM:SS D` (D = day-of-week 1..7, 1=Monday, per `api_ds3231.h`). Verified on
  hardware. `udp_client.py settime` with no further arguments fills in the current
  time, so the Pico does no timezone handling — it stores whatever digits it is
  sent, and the host owns that decision. Deliberately a manual command rather than
  a hardcoded `ds3231_set_datetime()` call, which would re-run on every boot and
  reset the clock to build time. The battery-backed RTC keeps time across power
  cycles, so this is normally a one-time step.
  **Phase 3, part 1 is built:** the canonical `temp_record_t` and its in-RAM ring
  buffer now exist (`temp_record.h`/`temp_record.c`), and serial, `read` and
  `settime` are all serializers over them rather than three formatting paths. Each
  DS18B20 reading is pushed as a record (`seq`, epoch, romcode, raw 1/16 degC,
  flags); CRC failures are pushed too, flagged rather than dropped, so a bad read
  shows as a gap instead of vanishing. `read` looks up the newest record per sensor
  by **romcode** via `temp_ring_latest_for_rom()`, which closes the index-remapping
  bug described below, and prints each sensor's own timestamp so a sensor that
  stopped reporting reads as stale rather than hiding behind a batch header.
  Timestamps are stored as a 4-byte epoch and formatted only at output, by
  `temp_format_epoch()`; `g_temp_celsius`/`g_temp_valid`/`g_temp_timestamp` are gone
  and `temp_store.h` is now just the sensor roster plus the RTC handle.
  The epoch conversions use days-from-civil rather than `mktime()`, which would
  apply the C library's local timezone to datetimes that are UTC by convention —
  verified on the host against `timegm()`/`strftime()` across the DS3231's full
  2000–2099 range. Verified on hardware: contiguous `seq`, correct UTC timestamps,
  `read` and `settime` both round-tripping.
- **The RTC is kept in UTC, by convention.** Nothing in the firmware enforces it —
  the DS3231 just stores digits — so every reported timestamp is labelled `UTC`
  (serial output, the `read` reply, and the `settime` confirmation) to keep the
  convention visible. The reason is DST: the DS3231 has no timezone rules, so a
  local-time clock sits an hour wrong after each transition until someone notices
  and re-runs `settime` — a *silent* wrongness, whereas a UTC serial log is merely
  an offset you can see. It also keeps the phase-3 stored log monotonic across the
  autumn fall-back hour (where local time repeats 01:00–02:00), and matches what
  MQTT consumers like Home Assistant/InfluxDB/Grafana assume on ingest. The cost is
  that raw serial output reads as UTC; `udp_client.py` prints the local equivalent
  alongside, and doing the conversion on-device would mean putting DST rules in
  firmware, which is exactly the complexity being avoided.
- **Known gap:** no SD (FatFs) logging yet — WiFi is query-on-demand only, nothing is
  persisted to storage. `ONEWIRE_GPIO_PIN` is set to 15 (GPIO 15 → DS18B20 DQ, external
  ~4k pull-up to 3V3).
- **Next up:** planned 3-phase WiFi path — (1) hello-world UDP echo [done], (2) useful
  on-demand query capability [done, `read` command], (3) migrate to MQTT (lwIP already
  vendors an MQTT client at `pico-sdk/lib/lwip/src/apps/mqtt`, so no new dependency
  needed) — see "Planned: storage & MQTT reporting" below for the agreed design.
  Phase 3's record + ring foundation is done (above); remaining, in the order they
  should be taken: SNTP sync on connect → publish on change-threshold + heartbeat →
  MQTT (retained per-sensor topics, Last Will) → the SD tier with its published
  watermark.
  Priority is to iterate on WiFi here in temp-sense first, then backport the
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

## Planned: storage & MQTT reporting (phase 3)

The agreed design for phase 3, recorded here so the shape stays settled. **The
record and ring buffer are now built** (`temp_record.h`/`temp_record.c` — see
Current status); SNTP, the publish policy, MQTT and SD are all still unbuilt,
and the reasoning below is what they should be built to.

The central change was a **canonical record plus an in-RAM ring buffer**, with
SD, UDP, and MQTT all becoming serializers over it rather than separate
formatting paths (`use_ds18b20.c` used to format for serial while
`temp-sense.c` re-formatted the same data for UDP):

```c
typedef struct {
    uint32_t seq;      // monotonic — dedup + gap detection
    uint32_t epoch;    // unix time, not a ctime string
    uint64_t romcode;  // sensor identity travels with the reading
    int16_t  raw;      // DS18B20 native 1/16 degC; celsius = raw / 16.0
    uint8_t  flags;    // valid / CRC-error
} temp_record_t;       // 24 bytes
```

Design decisions behind that, each with a reason worth not re-litigating:

- **Key by `romcode`, not array index.** [done] This was a latent bug
  independent of MQTT: `device 0/1/2` came from bus enumeration order in
  `ow_romsearch()`. If a sensor drops off the bus or one is added, the indices
  shift and historical data silently re-maps to different physical sensors.
  The romcodes in `g_temp_romcode[]` are stable hardware IDs — keep using
  those as identity in topics and log lines, as `read` now does.
- **Store epoch, format late.** [done] `g_temp_timestamp[25]` used to hold a
  formatted `ds3231_ctime()` string — 25 bytes, lossy, awkward for any
  consumer to parse. Records now carry a 4-byte epoch and text is produced
  only at the output boundary, by `temp_format_epoch()`.
- **Fix time via SNTP, not a one-off `ds3231_set_datetime()`.** Resolves the
  `Jan 01 2000` gap above more durably: WiFi is already up, so sync from SNTP
  on connect and write the result to the DS3231. The RTC then covers reboots
  and WiFi outages, and re-syncs whenever the network returns. lwIP vendors an
  SNTP client at `pico-sdk/lib/lwip/src/apps/sntp` — verified present, no new
  dependency, same as the MQTT client.
- **Decouple sample rate from publish rate.** 5s sampling is very fast for
  temperature; publishing 3 sensors every 5s is ~52k messages/day of
  mostly-identical values. Keep sampling as-is but publish on *change
  threshold + heartbeat* — e.g. if delta > 0.25 degC, or 60s since last
  publish.

MQTT layer specifics:

- One **retained** topic per sensor, `sensors/temp-sense/<romcode>/temperature`,
  so a new subscriber gets current state immediately instead of waiting for the
  next sample.
- **QoS 0** for the periodic stream; **QoS 1** only for records that can't be
  lost. Note QoS 1 is at-least-once, so consumers dedup on `seq`.
- Set a **Last Will** (`sensors/temp-sense/status` → `offline`, retained, with
  `online` published on connect). This lets consumers distinguish "temperature
  is steady" from "the logger died" — a distinction the current
  query-on-demand model cannot express at all.

SD (still unstarted, `../sdsc/` pattern) then becomes the durable tier:
append-only CSV, one line per record including `seq`, rotated daily. Track a
**published watermark** (last `seq` successfully published) so a WiFi/broker
outage is replayed from SD on reconnect rather than lost — that watermark is
what makes SD genuinely additive here rather than redundant with MQTT.

The fields sum to 19 bytes, but `uint64_t romcode` forces 8-byte alignment so
the struct pads to 24 — verified by compiling it for cortex-m0plus. Field
order does not help (romcode-first is also 24), and 16 was never reachable
with a full 8-byte ROM code. Pinned with a `_Static_assert` in
`temp_record.h`, since the capacity budget below depends on it.

Sizing is a non-issue: 24 bytes x 3 sensors at 5s is ~864 B/min, so an hour of
RAM backlog is ~51KB against the RP2040's 264KB SRAM. `TEMP_RING_CAPACITY` is
set to 2048 records (49KB) accordingly — roughly that hour. Records occupy a
fixed slot (`seq % capacity`), so `temp_ring_get(seq)` is a direct index rather
than a scan, and it distinguishes "not written yet" from "already overwritten"
by comparison against `temp_ring_next_seq()` — that is the accessor the SD
replay watermark above needs.
