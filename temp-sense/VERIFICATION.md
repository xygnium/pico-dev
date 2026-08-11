# temp-sense hardware verification log

Detailed hardware test evidence for `temp-sense`, split out of `CLAUDE.md`
so routine sessions aren't paying to load it. **Not auto-loaded** — read
this when you need the "how was this actually confirmed" detail behind a
`CLAUDE.md` summary line. Sections are ordered chronologically, matching
the order their summaries appear in `CLAUDE.md`.

## DS18B20 / WiFi / RTC bring-up

Hardware verified on real DS18B20 sensors (3 devices detected and
working). Fixed two pico-examples bugs: Issue #422 (conversion wait
timing) and Issue #569 (CRC validation). All sensor reads pass CRC
validation; no bus errors detected.

WiFi/UDP verified hardware round-trip with `udp_client.py` (points at
`192.168.1.120` — update if the Pico W's DHCP lease changes).

The DS3231 hardware bug: SDA/SCL were wired to GPIO26/27 (I2C1), which hung
the per-cycle RTC read (`ds3231_get_datetime()`) indefinitely on a NACKed
I2C transaction — silently killing the whole sensor loop (and, with it,
`wifi_udp_poll()`, so UDP stopped responding too). Rewired to GPIO8/9
(I2C0). Also fixed `cyw43_arch_wifi_connect_blocking()` intermittently
failing on the first attempt or two (`wifi_connect()` in
`../common/wifi/wifi.c` now retries up to 5 times with a 1s delay, logging
each failed attempt) — confirmed connecting reliably across repeated
power-cycles.

`settime` verified on hardware, round-tripping correctly.

Phase 3 part 1 (canonical record + RAM ring) verified on hardware:
contiguous `seq`, correct UTC timestamps, `read` and `settime` both
round-tripping. Epoch conversion (days-from-civil, not `mktime()`)
host-verified against `timegm()`/`strftime()` across the DS3231's full
2000–2099 range.

RTC plausibility check: threshold host-verified against `timegm()`;
verified on hardware for the good-clock path (boot prints
`rtc: <time> UTC`, no warning on `read`). The warning path has **not**
been exercised on hardware — that needs a boot with the RTC actually
unset (pull the CR2032 while unpowered, or temporarily raise
`TEMP_TIME_PLAUSIBLE_EPOCH` past the current time to force one trip).

## SD ring (4-step staging, 2026-08-05)

The SD work was staged into four flashable steps because the only real
test is flash-and-observe, and a large unflashed change mixes unverified
hardware assumptions with unverified software ones:

1. **Mount + report.** On hardware: `245760 sectors (120 MB)`,
   `119 MB free, cluster size 2048 bytes`, card empty, `sd probe: OK` —
   the card labelled 128MB reports 120MB formatted, as expected.
2. **Ring open + preallocate.** First boot preallocated `ring.dat` to
   64MB in 1621ms; a later boot with `ring.dat` already sized took
   127–377ms; found no `meta.dat` (expected — first boot); the fallback
   scan correctly recovered an empty ring (`next seq 0`).
3. **Write path + `sd` status command.** Verified on hardware: `seq`
   incremented across cycles, survived a reflash (continued at seq 39
   rather than resetting to 0), and `sd` replied
   `capacity 2097152 stored 81 seq 0..80 published 0 backlog 81` —
   consistent with what had actually been written.
4. **`format` command.** Verified on hardware: a bare `format` was
   refused/cancelled at the client prompt with no data touched; a
   confirmed format (`format yes-erase-the-card`) wiped the card and
   recreated the ring.

### Seq preservation across format: tried, then reverted (2026-08-05)

First pass captured `next_seq`/`published_seq` before the wipe and
restored them afterward. Hardware confirmed it worked (`next seq 261` in
the format confirmation, continuing from the live counter rather than
resetting) — but the premise didn't hold up under scrutiny (see
`CLAUDE.md`'s SD ring entry for the reasoning) and it was reverted.

Re-verified on hardware post-revert: the format confirmation showed
`sd: ring ready — ... next seq 0, published 0`, confirming the SD ring's
own bookkeeping resets exactly as coded, with no restoration. One nuance:
the very next records after that still carried `seq 1446, 1447...`, not
0 — not a bug, since `sd_ring_put()` never generates a seq, it only
writes whatever seq the RAM ring hands it, and the RAM ring is only
re-seeded from SD's recovered state at boot. A runtime `format` therefore
never itself produces a visible seq decrease — confirmed on hardware
(seq continued climbing straight through a runtime format). **Confirmed
on hardware:** power-cycling after a format does show `seq` restart at 0,
as predicted.

## MQTT step 1 (connect + Last Will, 2026-08-09)

Verified on hardware: `mqtt: connected to 192.168.1.88:1883`, 112s uptime
with no disconnect or panic (past the 5s cyclic timer and the 60s
keepalive PINGREQ), and `mosquitto_sub -t 'sensors/temp-sense/#'` returns
`sensors/temp-sense/status online` immediately — which is what proves the
retain flag took. Only `status` was present, as expected for step 1 (no
sensor data published yet).

First flash returned CONNACK status 5
(`MQTT_CONNECT_REFUSED_NOT_AUTHORIZED_`) because the client connected
anonymously; adding `MQTT_USER`/`MQTT_PASS` from `wifi_secrets.h` fixed
it.

## MQTT reconnect + backoff (2026-08-09)

Tested against `ctl.sh` (`{start|stop|restart|status}`) on the broker
host, dev10.

**A1 — broker restart.** `mqtt: disconnected (status 256)` →
`mqtt: reconnecting` → `mqtt: connected`, recovered in ~6s.

**A2 — extended outage, backoff growth.** `./ctl.sh stop`, left down; the
retry sequence observed:

| Attempt | Time (UTC) | Gap |
|---|---|---|
| 1 | 12:29:56 | — |
| 2 | 12:30:01 | 5s |
| 3 | 12:30:13 | 12s |
| 4 | 12:30:37 | 24s |
| 5 | 12:31:18 | 41s |
| 6 | 12:32:22 | 64s |
| 7 | 12:33:27 | 65s (cap held) |

Outage continued to ~29 minutes total; the DUT reconnected on the first
attempt after `./ctl.sh start`, at the 60s-capped cadence it had settled
into.

**A3 — no regression.** Sensor `seq` ran contiguous through the entire
outage (135260 → 135371 in the first test, 136163 → 136403 across the
29-minute one) — MQTT being down never stalled the sensor loop or the SD
ring.

**Backoff reset defect.** Original code reset only `s_retry_ms`, not
`s_next_attempt`, on reconnect. Reproduced the exact precondition (backoff
capped at 60s, reconnect, then a second failure inside that 60s window):

- Before fix: reconnect at 13:00:19 against a disconnect at ~12:59:58 —
  23s, when ~6s was intended (deadline had been stale at 13:00:15 = old
  connect time + 60s).
- After fix: disconnect at 13:13:03 (triggered by `./ctl.sh restart`
  immediately after a reconnect that had occurred at 13:12:51, itself at
  the 60s cap) → reconnect at 13:13:09 — **6s**, against what would have
  been ~13:13:51 (48s later) unfixed.

Broker-side view for the same fixed-path test:
```
2026-08-09T09:12:51-0400  status  online
2026-08-09T09:13:08-0400  status  offline
2026-08-09T09:13:14-0400  status  online
```

## Last Will under real power loss (2026-08-09)

DUT power pulled at 13:21:59. Serial confirmed the DUT was fully dark (0
bytes captured on `/dev/ttyACM0` after attaching, while the debugprobe
itself remained enumerated). Broker-side:

```
2026-08-09T09:21:16-0400  status  online     (baseline / last PINGREQ)
2026-08-09T09:22:46-0400  status  offline    (+47s after power pull)
```

Power restored: DUT booted (`rtc: 2026-08-09 13:23:46 UTC`,
`mqtt: connected` immediately after), and retained status flipped back:

```
2026-08-09T09:23:54-0400  status  online     (~8s after boot)
```

13:22:46 − 90s (1.5 × the 60s keep_alive) = 13:21:16, exactly the DUT's
last keepalive PINGREQ — confirming the broker times out from last-packet-
received, not from the power-pull instant. Power was pulled 43s into that
ping cycle, hence the 47s-not-90s observed delay.

Also recovered cleanly on the SD side: `sd: ring ready ... next seq
136839` after the unclean power loss, continuing forward with no restart
or reuse. This does **not** measure the actual data-loss window, though —
see the "no clean shutdown" deferred item in `CLAUDE.md` for why (serial
capture had stopped ~5 minutes before the actual cut, so the comparison
point is off by more than the quantity being measured).

## MQTT step 2 (publish on command, 2026-08-11)

`publish` UDP command verified on hardware: replies `0x<romcode>
published seq <n>` per sensor, and `mosquitto_sub -t
'sensors/temp-sense/#' -v` confirmed all three retained topics landed
with the expected JSON payload, e.g.
`sensors/temp-sense/0x2424010000871c28/temperature
{"seq":206925,"epoch":1786456048,"c":28.25}`.

**Bug found by this test, not by inspection:** `topic[48]` was 2 bytes
too small for `sensors/temp-sense/0x<16 hex>/temperature` (49 chars + NUL
= 50), so the first flash silently truncated every topic to
`.../temperatu` — `snprintf` truncated safely (no overflow), but the
broker showed the wrong topic name and nothing in the UDP reply or serial
log indicated it. Only visible by checking the broker side. Fixed by
sizing the buffer to 64 and reflashing; re-verified with a clean
`mosquitto_sub` capture showing the correct topic on a fresh publish, then
cleared the stale truncated retained messages from the broker by hand
(`mosquitto_pub -n -r` to each old topic).
