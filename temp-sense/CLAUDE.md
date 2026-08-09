# temp-sense

Project-specific guidance for `temp-sense`. The root `CLAUDE.md` covers repo-wide
conventions (external deps, build/flash/serial console commands) that apply
here too — Claude Code loads both when you're working in this directory.

A temperature sensor data logger — same shape as `../gmcount/` (sensor input
→ RTC-timestamped record → SD/WiFi output) but for temperature instead of
Geiger-Müller pulses.

## Current status

- **Last updated:** 2026-08-05
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
- **Boot-time RTC plausibility check** (`temp_time_is_plausible()`,
  `g_rtc_time_valid`): if the DS3231's backup cell dies it powers up at 2000-01-01
  and would timestamp everything confidently wrong, so any epoch before 2020-01-01
  triggers a loud serial warning at boot and a `warning:` line on the `read` reply.
  Re-checked each sensor cycle, so `settime` clears it on its own. This is what
  replaced the dropped SNTP sync — see phase 3 below for that reasoning. Threshold
  host-verified against `timegm()`; verified on hardware for the good-clock path
  (boot prints `rtc: <time> UTC`, no warning on `read`). The **warning path has
  not been exercised on hardware** — that needs a boot with the RTC actually
  unset (pull the CR2032 while unpowered, or temporarily raise
  `TEMP_TIME_PLAUSIBLE_EPOCH` past the current time to force one trip).
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
- **SD bring-up, step 1 of 4 (mount + report), is hardware-verified (2026-08-05)**
  — superseded in the build by step 2's `sd_ring.c` below, but `sd_probe.c`
  stays on disk as a simpler fallback smoke test. It mounts the card and
  prints its size, free space, cluster size and directory listing, then
  stops — it writes nothing. `hw_config.c` is copied from
  `../gmcount/` (byte-identical to `../sdsc/`'s): SPI0, SCK=18, MOSI=19, MISO=16,
  CS=17, clear of the 1-Wire GPIO15, I2C0 GPIO8/9 and the CYW43's 23/24/25/29.
  Wiring follows the spec in `notes.md`. On hardware: `245760 sectors (120 MB)`,
  `119 MB free, cluster size 2048 bytes`, card empty, `sd probe: OK` — the card
  labelled 128MB reports 120MB formatted, as expected. Mount failure reports and
  returns rather than panicking the way `../gmcount/fatfs.c` does: a headless
  logger that halts on a bad card also stops answering UDP. Cluster size is
  printed deliberately — it decides how much slack the 64MB ring costs in step
  2, and boot is the cheapest moment to learn it.
- **The SD work is deliberately staged into four flashable steps**, because the
  only real test here is flash-and-observe and a large unflashed change mixes
  unverified hardware assumptions with unverified software ones:
  1. mount and report [in the build, hardware-verified 2026-08-05] — *is it wired right?*
  2. ring open + preallocate [in the build, hardware-verified 2026-08-05] —
     *does `ring.dat` get created at 64MB, how slow?*
  3. write path in the sensor loop + `sd` status command [in the build,
     hardware-verified 2026-08-05] — *do records accumulate, does `seq`
     survive a reboot?*
  4. `format` command [in the build, hardware-verified 2026-08-05] — *does a
     blank card recover?*
  **All four steps are done.** The SD ring is fully built and verified.
  Step 2 (`sd_ring.c`/`sd_ring.h`, superseding `sd_probe.c`) is in
  `CMakeLists.txt`; on hardware, first boot preallocated `ring.dat` to 64MB in
  1621ms, a later boot with `ring.dat` already sized took 127-377ms, found no
  `meta.dat` (expected — first boot), and the fallback scan correctly
  recovered an empty ring (`next seq 0`). `sd_ring_init(NULL)` is called from
  `temp-sense.c` before the DS3231 has been read, so FatFs file timestamps
  aren't seeded yet — cosmetic only, doesn't touch ring data; worth revisiting
  when RTC init is reordered ahead of SD. Step 3: `use_ds18b20.c`'s sensor
  loop now calls `sd_ring_put()` alongside `temp_ring_push()` for every
  reading, and `sd_ring_sync()` once per cycle; `temp-sense.c` answers a new
  `sd` UDP command via `sd_ring_status()`. Needed a real fix beyond just
  wiring calls in: the RAM ring's `next_seq` used to always start at 0, which
  would have collided with seqs already on the SD card after a reboot, so
  `temp_ring_set_next_seq()` (`temp_record.c`) now seeds it from
  `sd_ring_next_seq()` at boot — and `temp_ring_count()`/`oldest_seq()` were
  fixed to bound RAM presence by that seed too, so they no longer claim slots
  are populated that were never written this boot. Verified on hardware:
  `seq` incremented across cycles, survived a reflash (continued at seq 39
  rather than resetting to 0), and `sd` replied
  `capacity 2097152 stored 81 seq 0..80 published 0 backlog 81` — consistent
  with what had actually been written. Step 4: `temp-sense.c` answers a new
  `format` UDP command, guarded by the exact confirmation token
  `format yes-erase-the-card` (anything else, including a bare `format`, gets
  a refusal reply); `udp_client.py` also prompts before sending the real
  token. Verified on hardware: a bare `format` was refused/cancelled at the
  client prompt with no data touched; a confirmed format wiped the card and
  recreated the ring.
  **Seq preservation across format: tried, then deliberately reverted
  (2026-08-05).** First pass captured `next_seq`/`published_seq` before the
  wipe and restored them afterward, on the premise that CLAUDE.md already
  called seq-not-restarting a decided requirement — and hardware confirmed it
  worked (`next seq 261` in the format confirmation, continuing from the live
  counter rather than resetting). But that "decided requirement" didn't hold
  up under scrutiny: `sd_ring_init()` already treats a **swapped** card as a
  legitimate reset (see the `scan_next_seq()` comment on a swapped card,
  below) — a formatted card has no more prior history than a swapped blank
  one, so preserving continuity for one and not the other was an
  inconsistency, not a fix. It also didn't close the risk it was meant to: a
  consumer mistaking a reused low seq for a duplicate is equally possible
  after a swap, which was never guarded. Reverted in favor of a uniform rule:
  seq legitimately restarts at 0 after either a format or a swap, and a
  future MQTT consumer detects a seq decrease as a lineage reset rather than
  assuming monotonicity across the device's entire operational history.
  Re-verified on hardware post-revert: the format confirmation showed
  `sd: ring ready — ... next seq 0, published 0`, confirming the SD ring's
  own bookkeeping resets exactly as coded, with no restoration. **One nuance
  worth recording:** the very next records after that still carried `seq
  1446, 1447...`, not 0 — not a bug. `sd_ring_put()` never generates a seq,
  it only writes whatever seq the RAM ring (`temp_record.c`) hands it, and
  the RAM ring only gets re-seeded from SD's recovered state at **boot**
  (`temp_ring_set_next_seq()` in `main()`). A runtime `format` command
  doesn't reboot, so the RAM ring's counter is untouched, and the first
  post-format write just brings SD's bookkeeping back in line with it. So in
  practice a *runtime* format never actually produces a seq decrease —
  only a **reboot** onto a freshly-formatted or swapped card does, since
  that's the one place the RAM ring's counter is re-seeded from SD. Left
  as-is deliberately rather than adding a RAM-reseed call to the runtime
  `format` handler to force an immediate reset — no duplicate-seq risk
  exists to guard against here (nothing decreased), so that would be
  complexity added on spec, not for a demonstrated need. **Confirmed on
  hardware:** power-cycling after a format does show `seq` restart at 0, as
  predicted — closing the loop on both halves of this behavior.
- **MQTT step 1 of 3 (connect + Last Will) is hardware-verified (2026-08-09).**
  The broker is mosquitto on dev10 at `192.168.1.88:1883` (address and
  credentials in the gitignored `wifi_secrets.h`). `mqtt_client.c`/`.h` connect
  after `wifi_connect()` succeeds, register a Last Will on
  `sensors/temp-sense/status` (retained, `offline`) and publish retained
  `online` on CONNACK. **No sensor data is published yet** — that's steps 2/3,
  so `read`/`sd` remain the only query paths and SD is still the only place
  readings land.
  This is the last leg of the 3-phase WiFi path: (1) hello-world UDP echo
  [done], (2) useful on-demand query capability [done,
  `read`/`sd`/`settime`/`format` commands], (3) migrate to MQTT [in progress]
  — lwIP already vendors an MQTT client at `pico-sdk/lib/lwip/src/apps/mqtt`,
  so no new dependency was needed. See "Planned: storage & MQTT reporting"
  below for the agreed design: retained per-sensor topics, Last Will, a
  published watermark read back from the SD ring, v1 publishes everything with
  no thinning. **Staging mirrors the SD 4-step plan** — (1) connect + Last
  Will only [done], (2) publish one retained reading on command, (3) full
  watermark-driven publish loop — not one large unflashed change.
  Two things step 1 needed that weren't in the plan:
  - **`MEMP_NUM_SYS_TIMEOUT` had to be raised** in the *shared*
    `../common/wifi/lwipopts.h` to `LWIP_NUM_SYS_TIMEOUT_INTERNAL + 1`.
    lwIP's MQTT app re-arms its own cyclic timer via `sys_timeout()`
    (`mqtt.c:620`) and isn't counted in the internal total, so without this
    the timeout pool empties ~5s after connect (`MQTT_CYCLIC_TIMER_INTERVAL`)
    and panics. That file is shared, so this affects gmcount/wifi/wifi2 too.
  - **The broker refuses anonymous connects.** First flash returned CONNACK
    status 5 (`MQTT_CONNECT_REFUSED_NOT_AUTHORIZED_`, `lwip/apps/mqtt.h:110`)
    because `memset(&ci, 0, ...)` left `client_user`/`client_pass` NULL.
    Both are now set from `MQTT_USER`/`MQTT_PASS`; note MQTT 3.1.1 has no
    password-without-username form, so it must be both or neither.
  Verified on hardware: `mqtt: connected to 192.168.1.88:1883`, 112s uptime
  with no disconnect or panic (past the 5s cyclic timer and the 60s keepalive
  PINGREQ), and `mosquitto_sub -t 'sensors/temp-sense/#'` returns
  `sensors/temp-sense/status online` immediately — which is what proves the
  retain flag took. Only `status` is present, as expected for step 1.
  **The Last Will is verified under real power loss (2026-08-09.)** DUT power
  pulled at 13:21:59, retained `offline` published at 13:22:46 — 47s later —
  and the DUT reconnected and republished retained `online` ~8s after boot.
  The timing confirms the mechanism, not just the outcome: the broker
  publishes the will 90s (1.5 x the 60s keep_alive) after the **last packet
  it received**, and 13:22:46 - 90s = 13:21:16, exactly the DUT's last
  keepalive PINGREQ. Power was pulled 43s into that ping cycle, so expect
  `offline` anywhere in a **30-90s window** after a real failure depending on
  where in the cycle the device dies — not a fixed 90s. Note this is the
  faithful test: earlier `offline` messages seen during broker restarts came
  from mosquitto terminating sessions on shutdown, which is a different path
  and does not exercise keepalive expiry at all.
  **Reconnect is built and hardware-verified (2026-08-09).** lwIP's MQTT
  client never retries on its own, so before this a single drop left the
  firmware silently offline until reboot. `mqtt_temp_poll()` is called once
  per sensor cycle from `use_ds18b20.c` alongside `wifi_udp_poll()` and
  retries with a 5s..60s doubling backoff. Shape worth keeping:
  - **The lwIP connection callback only records state; the main loop does the
    reconnecting.** The client is mid-teardown when the callback fires for a
    disconnect, so calling `mqtt_client_connect()` from inside it is not
    safe. `s_connected`/`s_connecting` are `volatile` — they are the only
    variables crossing the callback/main-loop boundary.
  - **`s_ci` is file scope, not a stack local.** The Last Will and the
    credentials must be re-presented on every CONNECT, so it has to outlive
    `mqtt_temp_init()`.
  - **Retries are quantized to the poll cadence** (~5.9s), so the nominal
    5/10/20/40/60s backoff is observed as 5/12/24/41/64s. Expected, not drift.
  - **If the broker *host* black-holes packets** (unplugged rather than
    refusing), `s_connecting` stays set until lwIP's `MQTT_CONNECT_TIMOUT` of
    100s — longer than the 60s cap, so retries pace at ~100s in that case.
  **One defect found by testing and fixed:** resetting the backoff on connect
  originally reset only the *interval* (`s_retry_ms`) and not the *deadline*
  (`s_next_attempt`), which `try_connect()` had last computed using the old,
  possibly capped interval. A disconnect shortly after recovering from a long
  outage was therefore retried up to a full 60s late — measured at 23s where
  ~6s was intended. Both halves are now reset together. Verified by
  reproducing the exact precondition (capped backoff, reconnect, then a
  broker restart inside the 60s window): 6s observed against ~48s for the
  unfixed path.
  Verified on hardware across `./ctl.sh` broker restart/stop/start: recovery
  in ~6s from a restart, backoff growth to the 60s cap over a ~29 minute
  outage, reconnect on the first attempt once the broker returned, and `seq`
  contiguous throughout — MQTT being down never stalls the sensor loop or SD.
  Priority is to iterate on WiFi here in temp-sense first, then backport the
  shared `common/wifi` usage to gmcount once this is working.

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
record and RAM ring buffer are now built** (`temp_record.h`/`temp_record.c` —
see Current status), as is the boot-time RTC plausibility check; the SD ring,
MQTT and the publish policy are all still unbuilt, and the reasoning below is
what they should be built to. SNTP was in this plan and has since been
**dropped** — see the entry below for why, so it doesn't get re-proposed.

**The pipeline is SD-first:**

```
sensor -> temp_record_t -> RAM ring (backlog) -> SD ring (durable)
                                                    |
                                       publisher reads a seq range -> MQTT
```

Every reading lands on SD; nothing is filtered on the way in. The publisher
then reads a **range of seqs** back off SD and sends them as a group, advancing
a watermark on success. So **build order is SD ring → MQTT → (thinning, later)**
— the publisher reads from SD, so SD comes first. An earlier version of this
plan had the publish policy first and SD last, which had the dependency
backwards.

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
- **SNTP: dropped, deliberately.** [decided 2026-07-28] An earlier version of
  this design called for syncing time from SNTP on connect and writing the
  result to the DS3231. That is *not* being built, and the reasoning is worth
  keeping so it doesn't get proposed again. Its stated justification here was
  resolving the `Jan 01 2000` gap "more durably" — but that gap was simply the
  clock never having been set, and the `settime` command (commit `1237023`)
  already closed it. Drift doesn't justify it either: the DS3231 is +/-2ppm
  over 0–40°C, about **±63 seconds per year**, against a project requirement
  that time be good to roughly a minute. The battery-backed RTC therefore holds
  for about a year unattended, and syncing it from the network solves a problem
  this project does not have. lwIP does vendor an SNTP client at
  `pico-sdk/lib/lwip/src/apps/sntp` if a future requirement ever needs
  sub-second accuracy — the objection is to the need, not the availability.
- **The one real risk SNTP would have covered is a dead backup cell** — the
  DS3231 then powers up at 2000-01-01 and timestamps everything confidently
  wrong. That is handled far more cheaply by a **boot-time plausibility check**
  [done]: `temp_time_is_plausible()` rejects any epoch before 2020-01-01
  (`TEMP_TIME_PLAUSIBLE_EPOCH`), and `g_rtc_time_valid` drives a loud serial
  warning at boot plus a `warning:` line on the `read` reply — the latter
  because a headless logger has no other way to tell whoever is querying it.
  The check is re-evaluated each sensor cycle, so running `settime` clears it
  without `settime` needing to know about it. Deliberately a floor test rather
  than a drift test, for the ±2ppm reason above. This follows the same
  principle as keeping the RTC in UTC: convert a *silent* wrongness into a
  visible one.
- **Publish everything, in batches — do not thin, at least at first.**
  [decided 2026-07-28] The tempting optimisation is to publish only on a change
  threshold plus a heartbeat (5s sampling of 3 sensors is ~52k messages/day of
  mostly-identical values). Don't, yet. Thinning suppresses publishes exactly
  when readings are steady, which is indistinguishable from a logger that has
  died — the optimisation makes a healthy system look broken. v1 publishes
  every record from the watermark to newest, so publish rate tracks sample rate
  and silence unambiguously means something is wrong. Thinning becomes safe
  once the retained topics and Last Will below exist, since those carry
  liveness independently of the data rate; layer it in then, not before.

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

SD (written, staged into the build — see Current status; `../sdsc/` pattern) is
the durable tier, and is **a ring**
— the same scheme as the RAM ring, just far larger: a fixed-size region with
records at `seq % sd_capacity`, overwriting oldest. That keeps lookup by `seq`
a direct index rather than a scan, and bounds space with no rotation or
compaction logic. (An earlier draft here said append-only CSV rotated daily;
a ring supersedes that.)

A **published watermark** — the last `seq` handed to the broker successfully —
is what makes SD additive rather than redundant with MQTT: a WiFi or broker
outage is replayed from SD on reconnect instead of lost. Two consequences:

- **The SD ring's capacity sets the maximum survivable outage.** Once
  unpublished records are overwritten they are gone, so capacity should be
  chosen against how long a broker outage this should tolerate.
- **The watermark means "last `seq` considered", not "last published".** Those
  are identical while v1 publishes everything, but they diverge the moment
  thinning is added — and reading it as "last published" would make every
  deliberately-skipped record look like a gap and get re-sent on reconnect,
  defeating the thinning entirely. Define it the safe way now.

### SD sizing and card layout

Sized against the **128MB card currently on hand** (larger is available later;
the ring size is one constant).

- **On-SD record: pad `temp_record_t`'s 24 bytes to 32.** SD/FatFs sectors are
  512 bytes, which 24 does not divide — records would straddle sector
  boundaries and every update would become a read-modify-write of two sectors.
  32 gives exactly 16 records/sector. The 8 spare bytes are not waste: they
  hold a per-record CRC (so a corrupt sector is detectable) and a format
  version byte. Note this is the *on-SD* layout; the in-RAM struct stays 24.
- **Ring: 64MB = 2^21 records ≈ 40 days.** At 3 sensors / 5s that is ~51,840
  records/day ≈ 1.66 MB/day, so 64MB holds ~40 days and the full 128MB would
  hold ~81. 40 days is already far past any realistic broker outage, and
  stopping at half the card leaves room for the config file below.
- **Keep the record count a power of two.** cortex-m0plus has no hardware
  divide, so `seq % capacity` compiles to a mask rather than a division call —
  the RAM ring's `TEMP_RING_CAPACITY` 2048 already works this way.
- **A bigger ring wears the card *less*.** A ring rewrites its own region
  forever, so at 1.66 MB/day a 64MB ring recycles every ~38 days where a 2MB
  one would recycle every ~1.2 days. Oversizing is nearly free and buys card
  life — which is the other reason not to size the ring down to just the
  retention actually needed.

Card layout (FatFs, `../sdsc/` pattern — so "reserving" space is simply not
filling the card):

| File | Size | Note |
|---|---|---|
| `ring.dat` | 64MB | preallocated at first boot, never grows |
| `meta.dat` | ~1 sector | seq high-water + publish watermark; must survive reboot |
| `config.txt` | future, a few KB | space deliberately reserved for it |
| free | ~60MB | headroom |

- **Preallocate `ring.dat` to full size once, at first boot.** That is what
  makes the reservation real: the ring's footprint becomes fixed and known, so
  a config file added later cannot be squeezed out, and the ring can never fail
  a write by trying to extend onto a full card.
- **Don't let the watermark be one fixed sector rewritten forever.** It is
  updated after every publish batch, which makes it a far worse wear hotspot
  than the ring itself. Give it a small ring of its own, or write it less
  often — decide when it's built. **Resolved: write it less often.** It is
  `meta.dat`, persisted only every 256 records (`META_PERSIST_INTERVAL`),
  because two properties already in the design make an exact watermark
  unnecessary — MQTT consumers dedup on `seq` anyway (QoS 1 is at-least-once),
  so a stale watermark costs a few duplicate publishes the consumer discards;
  and the boot-time probe repairs a stale sequence hint by reading forward from
  it. No second ring needed.

### SD implementation notes

What `sd_ring.c` already accounts for. It is written and host-tested but held
out of the build until step 1 is confirmed — don't undo these without reading
the reason.

- **`f_expand()` does not exist in this FatFs build.** `FF_USE_EXPAND` is `0` in
  `.../FatFs_SPI/ff15/source/ffconf.h` (FatFs R0.15), and it cannot be overridden
  from here: `ff.c` compiles inside the shared `FatFs_SPI` target, so this
  project's include path never shadows `ffconf.h`, and editing it would change
  the library out from under gmcount and sdsc too. **`f_lseek()` past EOF is used
  instead** — on a write-mode handle it expands the file, allocating clusters
  without pushing 64MB of zeros through the card.
- **A preallocated ring is full of stale data, not zeros.** `f_lseek()` expansion
  does not clear the new clusters, so a fresh `ring.dat` holds whatever the card
  had before. There is no "written" flag to trust: a slot is valid only if its
  CRC checks *and* the `seq` it carries belongs in that slot
  (`seq % capacity == slot`). The record has to prove it belongs.
- **Sequence numbers must survive reboot — the original design missed this.** The
  RAM ring counts from 0 every boot while the SD ring persists, so a restart
  would re-issue seqs already on the card, breaking both dedup and the
  slot-ownership test above. `sd_ring_init()` recovers the high-water mark and
  feeds it to `temp_ring_set_next_seq()`; `temp_ring_count()`/`oldest_seq()` also
  have to learn about a non-zero starting seq or they report RAM records never
  written this boot. Recovery is the `meta.dat` hint plus a doubling/binary-search
  probe forward; if `meta.dat` is missing *or its hint is no longer on the card*
  (a swapped card — trusting it would restart numbering mid-ring), it falls back
  to a binary search over the ring itself, finding the high-water mark in ~21
  sector reads rather than scanning two million records. **Host-verified** across
  empty, partial, exactly-full and multiply-wrapped rings with stale hints — the
  wrapped cases need ~40 days of data to reach on hardware, so they are not
  testable there. Record CRC-32 checked against the standard `"123456789"` →
  `0xCBF43926` vector.
- **`FF_USE_FASTSEEK` is `1`, so use it.** A cluster link map is built for
  `ring.dat` at open; without it every seek into a 64MB file walks the FAT chain
  and the ring stops being O(1) by `seq`. Falls back to ordinary seeking if the
  file is fragmented enough to overflow the table.
- **`format` is deliberately awkward to trigger** (step 4). It needs the exact
  token `format yes-erase-the-card` on the wire, because the UDP port is reachable
  by anything on the LAN and a stray or mistyped `format` must not wipe the log;
  `udp_client.py format` prompts as well. Nothing formats automatically — an
  unmountable card is reported and left alone, since auto-formatting on mount
  failure destroys recoverable data exactly when the card is flaky. `f_mkfs` is
  restricted to `FM_FAT|FM_FAT32` (no exFAT), which any host will mount without
  argument at 128MB. **Sequence numbering restarts at 0 after a format** —
  deliberately not preserved (see "Current status" for why this was tried and
  reverted). A formatted card has no more prior history than a swapped-in
  blank one, and `sd_ring_init()` already treats a swapped card as a
  legitimate reset (the `scan_next_seq()` fallback above), so format doing
  the same is consistent rather than a special case. Once MQTT exists, the
  consumer has to detect a seq decrease as a lineage reset anyway, to handle
  a swapped card — format doesn't need its own mechanism to dodge a problem
  that already exists elsewhere. In practice this reset only takes effect on
  the **next reboot**: `sd_ring_put()` writes whatever seq the RAM ring
  hands it, and the RAM ring is only re-seeded from SD's recovered state at
  boot, so a `format` issued at runtime leaves the RAM ring's live counter
  untouched and the very next write just brings SD's bookkeeping back in
  line with it — confirmed on hardware (seq continued climbing straight
  through a runtime format with no decrease at all).
- Two smaller ones: `FF_MIN_SS == FF_MAX_SS == 512` confirms the premise behind
  padding the on-SD record to 32 bytes; and `FF_FS_NORTC` is `0`, so FatFs asks
  the RP2040's *internal* RTC for file timestamps. That clock boots at year 0, so
  it is seeded from the DS3231 — and must be seeded *after* the library's
  `time_init()`, which calls `rtc_init()` and would reset an earlier setting.

### Deferred, with reasons — do not re-derive

Raised and deliberately parked while the SD steps land. Each is worth doing;
none belongs in an unflashed change.

- **Sync on sector completion, not every cycle.** 16 records/sector at 3
  records/cycle means a sector fills every ~5.3 cycles, so syncing every cycle
  rewrites the same partial sector ~5 times — roughly 5x write amplification on
  the data path, plus a directory-entry rewrite each time (`f_sync()` updates
  mtime, not just data). Syncing when `seq / 16` changes fixes both and is
  correct regardless of sensor count or sample rate.
- **A time ceiling on that sync, ~300s.** Only fires when a sector takes longer
  than that to fill (slow sampling, or a shrunken sensor roster). Keep it a
  compile-time constant, **not** derived at runtime from sensor count: deriving
  it would mean a sensor dropping off the bus silently loosens the data-loss
  guarantee. Set it several times the sector fill time —
  `(16 / sensor_count) x interval` — or firing mid-sector reintroduces the
  amplification above. Do log a warning at boot when the configuration is in
  that zone, since `num_devs` is known then.
- **Sample rate should not be shaped to the sector geometry.** Tempting, but
  unreachable: records-per-sector is always a power of two (the record size must
  divide 512), and a power of two is never divisible by 3 sensors. More
  importantly the roster is *discovered* by `ow_romsearch()`, so any layout
  invariant built on a fixed sensor count breaks quietly when one drops off —
  the same fragility already designed out by keying on `romcode`.
- **Slow the sample rate to ~30s before production deployment — 5s is far too
  fast.** [advised 2026-08-09, deliberately deferred to pre-deployment] The
  loop is `sleep_ms(5000)` at `use_ds18b20.c:189`, but the real period is
  **~5.9s**: the sleep runs *after* the 800ms conversion wait and the reads,
  so work time adds on top (confirmed in serial — 11:04:56 → 11:05:02 →
  11:05:07 → 11:05:13). The evidence that this oversamples is in the data
  itself: sensor `0x2424` across 142s read 26.75, 26.69, 26.69, 26.69, 26.75,
  26.75 — a 0.06 °C swing, i.e. exactly one LSB (1/16 °C) on a part specced
  at ±0.5 °C. That is quantization dither, not signal; a TO-92 DS18B20 in
  still air has a thermal time constant in the minutes, so 5s oversamples the
  physics by ~2 orders of magnitude. Going to 30s takes records/day from
  ~43,900 to 8,640, SD ring retention from ~48 days to **~242 days**, ring
  recycles/year from ~7.6 to ~1.5, and step-3 MQTT volume down by the same 5x
  (which is what makes the "publish everything, no thinning" v1 decision above
  comfortable rather than merely defensible). Cost: the loss window on an
  unclean power-down grows with sector fill time, ~32s → ~160s.
  **The interaction to check when this is picked up:** the ~300s sync ceiling
  two bullets above must stay several times the sector fill time
  `(16 / sensor_count) x interval`. At 30s that fill is 160s, so 300s is only
  1.9x — thin; raise the ceiling to ~900s at the same time. At **60s the fill
  is 320s and exceeds the 300s ceiling outright**, so the ceiling would fire
  mid-sector every cycle and reintroduce exactly the write amplification it
  exists to prevent. 30s is therefore the largest interval that doesn't force
  re-deciding the ceiling; 60s is fine only if the ceiling moves with it.
- **Do not thin the SD write path.** Considered and rejected: SD is the system of
  record, so a record not written is gone permanently, and the numbers show
  nothing to gain — 40 days retention against a need for far less, and ~9.5
  rewrites/year of the ring region is negligible wear. If write frequency is the
  concern, batch the syncs (reversible) rather than discard data (not). If
  thinning ever happens it belongs at the *publish* layer, as deadband **plus a
  forced heartbeat**, and note the DS18B20's 1/16°C quantization makes a naive
  change-detector dither on exactly the steady readings it meant to suppress.
- **There is no clean shutdown, so every power-down is the unclean case.** The
  firmware is a `for(;;)` loop with no stop path. Correctness is fine — per-record
  CRC, slot ownership and boot recovery all assume torn writes — but the loss
  window applies to *every* power cycle, not just faults. Proposed definition: a
  shutdown is clean when all buffered records are on the card, `meta.dat` reflects
  the true `next_seq` and watermark, and a flag records that this was reached
  deliberately. Cheap to satisfy with a `sync` UDP command plus a clean-stop flag
  cleared on first write after boot, which also lets startup report whether the
  previous run ended cleanly.
  **The loss window has still never been measured**, and a 2026-08-09 power-pull
  test did *not* measure it despite looking like it might: SD recovery worked
  (`next seq 136839`, continuing forward with no restart or reuse), but serial
  capture had stopped ~5 minutes before the cut, so the last *observed* record
  was not the last *written* one. Extrapolating across ~52 cycles puts the
  uncertainty at more than a full cycle, which swamps the quantity being
  measured. To get a real number: capture serial continuously right up to the
  power cut, then compare the last printed `seq` against `next seq` on the
  following boot. Worth doing before relying on any specific durability claim.
- **Operational logging is missing.** Every carefully-surfaced warning — RTC
  implausible, SD unavailable, CRC failures, mount errors, which recovery path
  ran, `format` invoked — goes to `printf` on a UART that nobody is attached to on
  a headless logger. Suggested shape: a small in-RAM event ring (~48 entries of
  `{epoch, severity, text}`, ~5KB) queryable as a `log` UDP command — RAM first,
  because SD faults cannot be logged to SD and a headless box needs them reachable
  over the network — mirrored to a size-capped `events.log` on SD when the card is
  up (~60MB headroom in the layout above).

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
