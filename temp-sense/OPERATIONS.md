# temp-sense Operator's Manual

A Pico W reads up to 20 DS18B20 probes on a shared 1-Wire bus at a fixed
interval (5s today — see "Sample rate & retention"), timestamps each
reading against a battery-backed DS3231 RTC, and logs to a ring buffer on
the SD card. `collector.py` pulls pending readings off the device over UDP
and appends them to a CSV on your machine.

All device commands below are sent with `udp_client.py <command>` (add
`--host <ip>` if the Pico's DHCP lease has changed from the script's
default).

## Normal operation

Run `collector.py` periodically (cron/systemd timer — hourly is the
protocol's normal cadence):

```
./collector.py
```

This appends new readings to `./temp_data.csv` (columns: `timestamp_epoch,
label, timestamp_utc, temp_c, valid`). The wire's `sensor_id` is only a
transport shorthand — `collector.py` resolves it to the sensor's current
location label immediately, using its local `sensor_table.csv`, so the CSV
is keyed by label rather than by an index that's meaningful only within one
collector run. `valid` is `1` for a real CRC-checked reading and `0` when
that sensor didn't respond or failed its CRC that cycle (the row is still
written, with `temp_c` meaningless) — a sensor going quiet shows up as a
run of `valid=0` rows rather than silently disappearing from the log.

Exit status is `0` on a complete pull, `1` if it gave up partway (device
unreachable, or a packet failed CRC past the device's configured retry
budget). **A failed run is always safe to just retry** — the device only
advances its watermark on a confirmed final ACK, and rows are deduped by
`(label, timestamp)`, so nothing is lost or duplicated by rerunning.

A normal pull refuses to run if `sensor_table.csv` doesn't exist yet — see
"Reading sensor locations" below.

### Reading sensor locations

```
./collector.py --table
```

This fetches the device's current persistent sensor table (index, romcode,
label) into `sensor_table.csv` and exits — it does not pull readings. Run
it once before your first pull, and again after adding a sensor or
relabeling one (see "Adding a new sensor" below); it's never fetched
automatically as part of a normal pull.

## Sample rate & retention

`TEMP_SAMPLE_INTERVAL_MS` in `use_ds18b20.c` (currently 5000ms) sets how
often every sensor is read. It's a **compile-time constant** — changing it
means a firmware rebuild and reflash, not a runtime command like `config
set`.

The SD ring holds a fixed 2,097,152 records total, shared across all active
sensors — one sampling round ("set") uses `N_sensors` of them. So retention
before the ring wraps (oldest unconfirmed data starts getting overwritten)
is:

```
retention = 2,097,152 / N_sensors × sample_interval
```

| N_sensors | 5s interval | 30s interval | 60s interval |
|---|---|---|---|
| 3 (today) | ~40 days | ~243 days | ~486 days |
| 20 (planned production ceiling) | ~6 days | ~36 days | ~73 days |

Wraparound isn't signalled in-protocol (see the protocol doc's note on
this), so it's on the operator to keep the collector running often enough
relative to whatever retention the chosen interval/sensor-count combo
gives. In practice, with `collector.py` run daily or more often, there's a
comfortable margin at any interval/sensor-count combination in this table
— this table matters most if collection is ever expected to lapse for an
extended stretch (e.g. an extended remote deployment between site visits).

## Adding a new sensor

A sensor's index in the persistent table is stable across reboots — it
only changes on an explicit registration, never as a side effect of which
probes happen to answer a boot's bus scan. Add sensors **one at a time**,
and test each before adding the next:

1. **Stop the collector cron/timer** for the duration of this process —
   the sensor roster is in flux and pulled data shouldn't be trusted until
   it's done.
2. Physically connect **one** new probe to the 1-Wire bus.
3. Reboot the Pico (power-cycle, or reflash) so the boot-time scan finds
   it. The device auto-registers any new romcode at the next free table
   index with the placeholder label `unlabeled` and logs how many new
   sensors it found.
4. Check `table` to find the new probe's index and confirm it shows up as
   `unlabeled`.
5. Name it: `label <index> <location string>` (e.g. `label 3 attic` or
   `label 3 outdoor north wall` — multi-word strings are fine).
6. Verify: `table` again, confirm the new location string is there, and
   let a few sample cycles pass to sanity-check the reading looks
   reasonable (`read`, or `sd` for ring status).
7. Repeat from step 2 for the next probe, if any.
8. Once all additions are done, **restart the collector cron/timer**, and
   run `./collector.py --table` once to refresh `sensor_table.csv` with
   the finished roster.

## Full reset (wiping the logger)

There is no in-place removal of a single bad sensor, and no partial-wipe
command — `format` always erases everything on the SD card together (the
ring, `config.dat`, and `labels.dat` alike, since it's a full card
reformat, not a per-file delete). So a full reset is the only reset there
is; use this procedure whenever you need one, for example:

- **A sensor has gone bad** and needs to drop off the roster (no in-place
  removal exists — this is the only way to retire one).
- **Starting the logger over from scratch** — a new deployment, a card
  swap, or discarding accumulated history entirely.

Steps:

1. **Stop the logger** and the collector cron/timer.
2. `format` (see the command reference below). Any unconfirmed readings
   are lost; that's accepted as part of a full reset.
3. Reattach only the probes that should be on the new roster (all of them,
   for a fresh start; only the good ones, if retiring a bad sensor), then
   reboot so the boot-time scan registers them fresh (see "Adding a new
   sensor" above for naming each one).
4. **Restart the collector cron/timer**, and run `./collector.py --table`
   once to refresh `sensor_table.csv` with the new roster.

## Command reference

| Command | Effect |
|---|---|
| `table` | The persistent sensor table: index, romcode, label for every registered probe (including one not currently on the bus — its readings show as invalid rather than disappearing). |
| `label <index> <string>` | Rename the probe at table index `<index>` (must already have a `labels.dat` entry — auto-created at boot). |
| `config get` | Show the receiver's retry policy (`max_retries`, `retry_interval_ms`) that `collector.py` reads at session start. |
| `config set <max_retries> <retry_interval_ms>` | Update that policy. Bounds: retries 1–255, interval 100–600000ms. Defaults: 5 / 5000ms. |
| `settime YYYY-MM-DD HH:MM:SS D` | Set the RTC. `D` is day-of-week, 1=Monday. Send **UTC** — `udp_client.py settime` (no args) does this for you from your host clock. |
| `sd` | Ring buffer status: capacity, records stored, seq range, confirmed watermark, backlog. |
| `read` | Most recent reading, for a quick manual check. |
| `format` | **Destroys everything on the SD card** — the ring, `config.dat`, and `labels.dat` alike, since it's a full card reformat, not a per-file delete. Requires the exact confirmation token; `udp_client.py format` prompts before sending it. |

## Troubleshooting

- **"rtc: clock not set" at boot** — every timestamp will be wrong until
  you run `udp_client.py settime`.
- **No reply from the device** — check it's on the network (DHCP lease may
  have changed; update `--host`), and that nothing else has the serial
  console (`fuser -v /dev/ttyACM0`) if you need to check the boot log.
- **`collector.py` exits 1** — just rerun it; see "Normal operation" above
  for why this is always safe.
- **Ring backlog growing** — check `sd` status; if the collector has been
  down long enough to threaten wraparound (permanent data loss for the
  oldest unconfirmed records), get it running again soon — this protocol
  does not signal wraparound in-band.
- **CRC errors on reads, worse with more sensors/longer wire** — before
  suspecting the pull-up value, check the bus *topology*. Mixing a very
  short stub (e.g. a sensor 20mm from the logger) with long branches
  (e.g. others 6+ meters out) on the same trunk causes impedance-mismatch
  reflections at the near tap that corrupt the bus's fast edges for every
  sensor, not just the close one — confirmed on this hardware: removing
  the near sensors eliminated the CRC errors on the far ones, while
  pull-up changes (2.2K, then 1K) had not. Prefer a single consistent run
  length (avoid very short stubs on a bus that also has long branches)
  over further pull-up tuning if errors persist across a stronger pull-up.
