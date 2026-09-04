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
it once before your first pull, and again after any `label`, `decomm`, or
`wipetable` change (see "Adding a new sensor" and "Removing/replacing a
sensor" below); it's never fetched automatically as part of a normal pull.

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
only changes on an explicit register/decomm/wipe, never as a side effect
of which probes happen to answer a boot's bus scan. Add sensors **one at a
time**, and test each before adding the next:

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

## Removing/replacing a sensor

`decomm` removes a table entry and **compacts** the table — every later
index shifts down by one, and that freed slot is whatever the next new
registration gets. This is the normal way to retire a broken sensor or
swap it for a new one at the same physical location. Because the SD ring
replays history assuming a constant sensor count across any unconfirmed
backlog, `decomm` (and the table wipe below) require the ring to be fully
drained first:

1. **Stop the collector cron/timer.**
2. `pause` — stops sampling. The ring's backlog can only ever reach exactly
   zero once sampling has actually stopped; otherwise a new reading always
   lands between the collector's last ACK and your next command.
3. Run `./collector.py` one more time to drain whatever backlog remains.
   Check `sd` — you need `confirmed` to equal `seq`'s upper bound (backlog
   of 1). If it isn't quite there yet, just rerun the collector; no new
   readings are being generated while paused, so it will finish.
4. `decomm <index>` (found via `table`). Refused if not paused, or if the
   ring still has backlog.
5. `resume` — restarts sampling immediately, without trying to catch up on
   the paused interval.
6. **Restart the collector cron/timer**, and run `./collector.py --table`
   once to refresh `sensor_table.csv` — the freed index will be reused by
   whatever sensor is registered next, so don't skip this.

`wipetable yes-erase-sensor-table` clears the *entire* table (distinct from
`format`, which erases the whole SD card) and immediately re-scans the bus
to repopulate it from scratch — useful for starting a location scheme over.
Same pause/drain precondition and `udp_client.py` confirmation-prompt
pattern as `format`.

## Command reference

| Command | Effect |
|---|---|
| `table` | The persistent sensor table: index, romcode, label for every registered probe (including one not currently on the bus — its readings show as invalid rather than disappearing). |
| `label <index> <string>` | Rename the probe at table index `<index>` (must already have a `labels.dat` entry — auto-created at boot). |
| `pause` | Stop the sample loop. Required before `decomm`/`wipetable`, and before the ring backlog can reach exactly zero. |
| `resume` | Restart the sample loop, from now (no catch-up burst for the paused interval). |
| `decomm <index>` | Remove that table entry and compact — later indices shift down. Refused unless paused with a fully drained ring backlog. |
| `wipetable yes-erase-sensor-table` | **Erases the whole sensor table** and immediately re-scans the bus to repopulate it. Distinct from `format`. Same pause/drain precondition; requires the exact confirmation token (`udp_client.py wipetable` prompts before sending it). |
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
