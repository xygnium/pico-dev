# temp-sense

Project-specific guidance for `temp-sense`. The root `CLAUDE.md` covers repo-wide
conventions (external deps, build/flash/serial console commands) that apply
here too — Claude Code loads both when you're working in this directory.

A temperature sensor data logger — same shape as `../gmcount/` (sensor input
→ RTC-timestamped record → SD/WiFi output) but for temperature instead of
Geiger-Müller pulses.

## Current status

*Hardware test evidence for everything in this section — exact timestamps,
serial excerpts, specific `seq` numbers — lives in `VERIFICATION.md`, not
here, so routine sessions aren't paying to load it.*

- **Last updated:** 2026-08-09
- **Done:** Hardware verified on real DS18B20 sensors (3 devices, CRC clean —
  fixed pico-examples Issues #422 and #569 along the way). WiFi/UDP is live
  via the shared `../common/wifi/` library (`picowifi`, extracted from
  gmcount's `wifi.c` — see `../common/wifi/wifi.h`): listens on UDP port
  8080, answers `read` with the latest DS18B20 readings (one rom-keyed line
  per sensor), any other command gets a generic ack. Credentials live in a
  gitignored `wifi_secrets.h`.
  Fixed a real DS3231 hardware bug on this board: SDA/SCL were wired to
  GPIO26/27 (I2C1), which hung the per-cycle RTC read
  (`ds3231_get_datetime()`) indefinitely on a NACKed I2C transaction —
  silently killing the whole sensor loop and, with it, `wifi_udp_poll()`.
  Rewired to GPIO8/9 (I2C0); see Architecture below.
  `settime YYYY-MM-DD HH:MM:SS D` (D = day-of-week 1..7, 1=Monday, per
  `api_ds3231.h`) sets the DS3231's clock over UDP; `udp_client.py settime`
  with no arguments fills in the current time, so the Pico does no timezone
  handling — it stores whatever digits it's sent, and the host owns that
  decision. Deliberately manual rather than a hardcoded
  `ds3231_set_datetime()` call, which would reset the clock to build time on
  every boot; the battery-backed RTC holds across power cycles, so this is
  normally a one-time step.
  **Phase 3, part 1 (canonical record + RAM ring) is built:**
  `temp_record_t`/`temp_record.c` — serial, `read` and `settime` are all
  serializers over one ring rather than three separate formatting paths.
  CRC failures are pushed as flagged records rather than dropped, so a bad
  read shows as a gap instead of vanishing. `read` looks up the newest
  record **by romcode** (`temp_ring_latest_for_rom()`, closing a latent
  index-remapping bug — see "Planned" below), and timestamps are a 4-byte
  epoch formatted only at output (`temp_format_epoch()`), using
  days-from-civil rather than `mktime()` (see UTC convention below for why).
- **Boot-time RTC plausibility check** (`temp_time_is_plausible()`,
  `g_rtc_time_valid`): if the DS3231's backup cell dies it powers up at
  2000-01-01 and would timestamp everything confidently wrong, so any epoch
  before 2020-01-01 (`TEMP_TIME_PLAUSIBLE_EPOCH`) triggers a loud serial
  warning at boot and a `warning:` line on the `read` reply. Re-checked each
  sensor cycle, so `settime` clears it on its own. This is what replaced the
  dropped SNTP sync — see "Planned" below for that reasoning.
  **The warning path has not been exercised on hardware** — that needs a
  boot with the RTC actually unset (pull the CR2032 while unpowered, or
  temporarily raise `TEMP_TIME_PLAUSIBLE_EPOCH` past the current time to
  force one trip).
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
- **SD bring-up, step 1 of 4 (mount + report), superseded in the build by
  step 2's `sd_ring.c`** — `sd_probe.c` stays on disk as a simpler fallback
  smoke test (mounts, prints size/free space/cluster size/directory listing,
  writes nothing). `hw_config.c` is copied from `../gmcount/` (byte-identical
  to `../sdsc/`'s): SPI0, SCK=18, MOSI=19, MISO=16, CS=17, clear of the
  1-Wire GPIO15, I2C0 GPIO8/9 and the CYW43's 23/24/25/29. Wiring follows the
  spec in `notes.md`. Mount failure reports and returns rather than
  panicking the way `../gmcount/fatfs.c` does: a headless logger that halts
  on a bad card also stops answering UDP.
- **The SD work was deliberately staged into four flashable steps** — mount
  + report, ring open + preallocate, write path + `sd` status command,
  `format` command — because the only real test here is flash-and-observe
  and a large unflashed change mixes unverified hardware assumptions with
  unverified software ones. **All four are done and hardware-verified
  (2026-08-05, see `VERIFICATION.md`).**
  Step 2 (`sd_ring.c`/`sd_ring.h`, superseding `sd_probe.c`) is in
  `CMakeLists.txt`. `sd_ring_init(NULL)` is called from `temp-sense.c`
  before the DS3231 has been read, so FatFs file timestamps aren't seeded
  yet — cosmetic only, doesn't touch ring data; worth revisiting when RTC
  init is reordered ahead of SD.
  Step 3 needed a real fix beyond wiring calls in: the RAM ring's
  `next_seq` used to always start at 0, which would have collided with
  seqs already on the SD card after a reboot, so
  `temp_ring_set_next_seq()` (`temp_record.c`) now seeds it from
  `sd_ring_next_seq()` at boot — and `temp_ring_count()`/`oldest_seq()`
  were fixed to bound RAM presence by that seed too, so they no longer
  claim slots are populated that were never written this boot.
  Step 4: `temp-sense.c` answers a `format` UDP command, guarded by the
  exact confirmation token `format yes-erase-the-card` (anything else,
  including a bare `format`, gets a refusal reply); `udp_client.py` also
  prompts before sending the real token.
  **Seq preservation across format: tried, then deliberately reverted
  (2026-08-05).** First pass captured `next_seq`/`published_seq` before the
  wipe and restored them afterward, on the premise that seq-not-restarting
  was a decided requirement — and it worked on hardware. But that premise
  didn't hold up under scrutiny: `sd_ring_init()` already treats a
  **swapped** card as a legitimate reset (see the `scan_next_seq()` comment
  on a swapped card, below) — a formatted card has no more prior history
  than a swapped blank one, so preserving continuity for one and not the
  other was an inconsistency, not a fix. It also didn't close the risk it
  was meant to: a consumer mistaking a reused low seq for a duplicate is
  equally possible after a swap, which was never guarded. Reverted in favor
  of a uniform rule: **seq legitimately restarts at 0 after either a format
  or a swap**, and a future MQTT consumer detects a seq decrease as a
  lineage reset rather than assuming monotonicity across the device's
  entire operational history.
  One nuance worth keeping: a *runtime* `format` command never itself
  produces a visible seq decrease, only a **reboot** onto a
  freshly-formatted or swapped card does — `sd_ring_put()` writes whatever
  seq the RAM ring hands it, and the RAM ring is only re-seeded from SD's
  recovered state at boot, so a runtime format leaves the RAM ring's live
  counter untouched and the first post-format write just brings SD's
  bookkeeping back in line with it. Left as-is deliberately rather than
  adding a RAM-reseed call to force an immediate reset — no duplicate-seq
  risk exists to guard against here, so that would be complexity added on
  spec, not for a demonstrated need.
- **MQTT step 1 of 3 (connect + Last Will) is done and hardware-verified
  (2026-08-09, see `VERIFICATION.md`).** The broker is mosquitto on dev10 at
  `192.168.1.88:1883` (address and credentials in the gitignored
  `wifi_secrets.h`; `./ctl.sh {start|stop|restart|status}` on dev10 controls
  it, useful for exercising outage/recovery behavior). `mqtt_client.c`/`.h`
  connect after `wifi_connect()` succeeds, register a Last Will on
  `sensors/temp-sense/status` (retained, `offline`) and publish retained
  `online` on CONNACK. **No sensor data is published yet** — that's steps
  2/3, so `read`/`sd` remain the only query paths and SD is still the only
  place readings land.
  This is the last leg of the 3-phase WiFi path: (1) hello-world UDP echo
  [done], (2) useful on-demand query capability [done,
  `read`/`sd`/`settime`/`format` commands], (3) migrate to MQTT [in
  progress] — lwIP already vendors an MQTT client at
  `pico-sdk/lib/lwip/src/apps/mqtt`, so no new dependency was needed. See
  "Planned: storage & MQTT reporting" below for the agreed design: retained
  per-sensor topics, Last Will, a published watermark read back from the SD
  ring, v1 publishes everything with no thinning. **Staging mirrors the SD
  4-step plan** — (1) connect + Last Will only [done], (2) publish one
  retained reading on command, (3) full watermark-driven publish loop — not
  one large unflashed change.
  Two things step 1 needed that weren't in the plan:
  - **`MEMP_NUM_SYS_TIMEOUT` had to be raised** in the *shared*
    `../common/wifi/lwipopts.h` to `LWIP_NUM_SYS_TIMEOUT_INTERNAL + 1`.
    lwIP's MQTT app re-arms its own cyclic timer via `sys_timeout()`
    (`mqtt.c:620`) and isn't counted in the internal total, so without this
    the timeout pool empties ~5s after connect (`MQTT_CYCLIC_TIMER_INTERVAL`)
    and panics. That file is shared, so this affects gmcount/wifi/wifi2 too.
  - **The broker refuses anonymous connects** (CONNACK status 5,
    `MQTT_CONNECT_REFUSED_NOT_AUTHORIZED_`, `lwip/apps/mqtt.h:110`).
    `client_user`/`client_pass` are set from `MQTT_USER`/`MQTT_PASS`; note
    MQTT 3.1.1 has no password-without-username form, so it must be both or
    neither.
  **The Last Will is verified under real power loss.** The observable
  window after a real DUT failure is **30-90s**, not a fixed 90s: the broker
  publishes the will 1.5x the 60s keep_alive after the *last packet it
  received*, not from the failure instant, so the delay depends on where in
  the 60s ping cycle the device dies. Earlier `offline` messages seen during
  broker restarts are a different path (mosquitto terminating sessions on
  shutdown) and don't exercise keepalive expiry at all — the power-loss test
  is the faithful one.
  **Reconnect is built and hardware-verified.** lwIP's MQTT client never
  retries on its own, so before this a single drop left the firmware
  silently offline until reboot. `mqtt_temp_poll()` is called once per
  sensor cycle from `use_ds18b20.c` alongside `wifi_udp_poll()` and retries
  with a 5s..60s doubling backoff. Shape worth keeping:
  - **The lwIP connection callback only records state; the main loop does
    the reconnecting.** The client is mid-teardown when the callback fires
    for a disconnect, so calling `mqtt_client_connect()` from inside it is
    not safe. `s_connected`/`s_connecting` are `volatile` — they are the
    only variables crossing the callback/main-loop boundary.
  - **`s_ci` is file scope, not a stack local.** The Last Will and the
    credentials must be re-presented on every CONNECT, so it has to outlive
    `mqtt_temp_init()`.
  - **Retries are quantized to the poll cadence** (~5.9s), so the nominal
    5/10/20/40/60s backoff is observed a few seconds later each step.
    Expected, not drift.
  - **If the broker *host* black-holes packets** (unplugged rather than
    refusing), `s_connecting` stays set until lwIP's `MQTT_CONNECT_TIMOUT` of
    100s — longer than the 60s cap, so retries pace at ~100s in that case.
  **One defect found by testing and fixed:** resetting the backoff on
  connect originally reset only the *interval* (`s_retry_ms`) and not the
  *deadline* (`s_next_attempt`), which `try_connect()` had last computed
  using the old, possibly capped interval. A disconnect shortly after
  recovering from a long outage was therefore retried up to a full 60s late.
  Both halves are now reset together — verified by reproducing the exact
  precondition (capped backoff, reconnect, then a second failure inside the
  60s window).
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
