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
sensor_id, timestamp_utc, temp_c`). `sensor_id` is a wire index, not a
sensor's physical identity — see "Reading sensor locations" below to
resolve it to a location.

Exit status is `0` on a complete pull, `1` if it gave up partway (device
unreachable, or a packet failed CRC past the device's configured retry
budget). **A failed run is always safe to just retry** — the device only
advances its watermark on a confirmed final ACK, and rows are deduped by
`(sensor_id, timestamp)`, so nothing is lost or duplicated by rerunning.

### Reading sensor locations

`temp_data.csv` deliberately doesn't carry a romcode or location string per
row (see "Adding a new sensor" for why). To find out what `sensor_id`
values correspond to, in physical terms:

```
./collector.py --labels
```

This fetches the device's full romcode→location table into
`sensor_labels.csv` and exits — it does not pull readings. Cross-reference
readings by also checking that session's `sensors` output (device
command), which maps this boot's `sensor_id` to romcode. Run `--labels` by
hand whenever you want a fresh copy; it's never fetched automatically as
part of a normal pull.

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

Sensor indices (`sensor_id`) are only stable for the life of one boot's
1-Wire scan — adding or removing a probe can shift indices for *other*,
already-labeled probes too. Add sensors **one at a time**, and test each
before adding the next:

1. **Stop the collector cron/timer** for the duration of this process —
   the sensor roster is in flux and pulled data shouldn't be trusted until
   it's done.
2. Physically connect **one** new probe to the 1-Wire bus.
3. Reboot the Pico (power-cycle, or reflash) so the boot-time scan finds
   it. The device auto-registers any new romcode with the placeholder
   label `unlabeled` and logs how many new sensors it found.
4. Check `sensors` to find the new probe's index, and `labels` to confirm
   it shows up as `unlabeled`.
5. Name it: `label <index> <location string>` (e.g. `label 3 attic` or
   `label 3 outdoor north wall` — multi-word strings are fine).
6. Verify: `labels` again, confirm the new location string is there, and
   let a few sample cycles pass to sanity-check the reading looks
   reasonable (`read`, or `sd` for ring status).
7. Repeat from step 2 for the next probe, if any.
8. Once all additions are done, **restart the collector cron/timer**, and
   run `./collector.py --labels` once to refresh `sensor_labels.csv` with
   the finished roster.

## Command reference

| Command | Effect |
|---|---|
| `sensors` | This boot's wire index → romcode table (only currently-connected probes). |
| `labels` | Full romcode → location table (`labels.dat`), including probes not currently connected. |
| `label <index> <string>` | Rename the probe at wire index `<index>` (must already have a `labels.dat` entry — auto-created at boot). |
| `config get` | Show the receiver's retry policy (`max_retries`, `retry_interval_ms`) that `collector.py` reads at session start. |
| `config set <max_retries> <retry_interval_ms>` | Update that policy. Bounds: retries 1–255, interval 100–600000ms. Defaults: 5 / 5000ms. |
| `settime YYYY-MM-DD HH:MM:SS D` | Set the RTC. `D` is day-of-week, 1=Monday. Send **UTC** — `udp_client.py settime` (no args) does this for you from your host clock. |
| `sd` | Ring buffer status: capacity, records stored, seq range, confirmed watermark, backlog. |
| `read` | Most recent reading, for a quick manual check. |
| `fetch <from_seq> [count]` / `ack <seq>` | Legacy manual pull-and-confirm mechanism, superseded by `collector.py`'s binary protocol for normal use — still available for manual inspection. |
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
