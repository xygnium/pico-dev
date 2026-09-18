# temp-sense Operator's Manual

A Pico W reads up to 20 DS18B20 probes on a shared 1-Wire bus at a fixed
interval (5s today — see "Sample rate & retention"), timestamps each
reading against a battery-backed DS3231 RTC, and logs to a ring buffer on
the SD card. `collector.py` pulls pending readings off the device over UDP
and stores them in a SQLite database — normally running continuously
inside the `temp-collector` Podman container
(`~/containers/temp-collector/`), not invoked by hand or by cron. See
that project's own `README.md`, `RECOVERY.md`, and `ACCEPTANCE.md` for
container-level operations (backup, disaster recovery, what's been
verified) not repeated here.

All device commands below are sent with `udp_client.py <command>` (add
`--host <ip>` if the Pico's DHCP lease has changed from the script's
default).

## Normal operation

The collector runs continuously as the `temp-collector` container, polling
on its own schedule — currently every 30 minutes
(`~/containers/temp-collector/config/collector.conf`'s `interval_seconds`,
editable live, no restart needed). Day to day, there's nothing to invoke
by hand — see `~/containers/temp-collector/README.md`'s "Operating: the
short version" for `ctl.sh`/`podman logs` commands (not repeated here).

Readings land in `data/temp_sense.db` (SQLite): a `readings` table
(columns: `timestamp_epoch, label, timestamp_utc, temp_c, valid`) and a
`sensors` table (`id, romcode, label`). The wire's `sensor_id` is only a
transport shorthand — `collector.py` resolves it to the sensor's current
location label immediately, using the `sensors` table, so `readings` is
keyed by label rather than by an index that's meaningful only within one
pull. `valid` is `1` for a real CRC-checked reading and `0` when that
sensor didn't respond or failed its CRC that cycle (the row is still
written, with `temp_c` meaningless) — a sensor going quiet shows up as a
run of `valid=0` rows rather than silently disappearing from the log.

Quick manual query:
```
python3 -c "
import sqlite3
conn = sqlite3.connect('/home/mike/containers/temp-collector/data/temp_sense.db')
print(conn.execute('select * from readings order by timestamp_epoch desc limit 10').fetchall())
"
```

A single failed poll cycle (device unreachable, or a packet failed CRC
past the device's configured retry budget) is logged and skipped, not
fatal to the running container — the device only advances its watermark
on a confirmed final ACK, and rows are deduped by the `readings` table's
own `(timestamp_epoch, label)` primary key, so nothing is lost or
duplicated by the next cycle retrying.

**Running `collector.py` by hand** still works as a one-shot pull, for
local testing or a one-off check outside the container:
```
./collector.py --db-path <path-to-a.db>
```
Exit status `0` on a complete pull, `1` if it gave up partway — same
retry-is-always-safe guarantee as above. This is the same code the
container runs, so it's a reasonable way to test the wire protocol
against a firmware change without touching the container at all.

A pull refuses to run if the `sensors` table is empty — see "Reading
sensor locations" below.

### Reading sensor locations

Fetches the device's current persistent sensor table (index, romcode,
label) into the `sensors` table and exits — it does not pull readings.
It's a full replace, not a merge (the device's table is the source of
truth). Run it once before the first pull, and again after adding a
sensor or relabeling one (see "Adding a new sensor" below); it's never
fetched automatically as part of a normal pull.

For the exact command (podman-exec vs. running `collector.py` directly on
the host, and the container-running caveats for each), see
`~/containers/temp-collector/README.md`'s "First-time setup / after
adding a sensor" section — not repeated here.

## Sample rate & retention

`config sample <ms>` sets how often every sensor is read (default 5000ms,
bounds 1000–3600000ms). It's a runtime command, but the sampling loop only
reads it once, before it starts — so **a change takes effect on the next
reboot, not live**. Set it, then send `reboot` (or power-cycle/reflash) to apply it.
`config get`'s `sample_interval_ms=...` reports the *stored* value (updated
immediately by `config sample`) — not necessarily what the currently-running
loop is actually doing, which stays at whatever it read at its last boot
until you reboot again. There's no in-band way to query the live cadence;
watch the timestamp spacing between reads if you need to confirm it.

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
this), so it's on the operator to keep the collector *container* running
often enough relative to whatever retention the chosen interval/sensor-count
combo gives. In practice, with the container polling every 30 minutes,
there's a comfortable margin at any interval/sensor-count combination in
this table — this matters most if the container itself is ever expected
to be down for an extended stretch (not just a single missed poll), e.g.
during a host migration or an extended remote deployment between site
visits.

## Adding a new sensor

A sensor's index in the persistent table is stable across reboots — it
only changes on an explicit registration, never as a side effect of which
probes happen to answer a boot's bus scan. Add sensors **one at a time**,
and test each before adding the next:

1. **Stop the collector** (`cd ~/containers/temp-collector && ./ctl.sh
   stop`) for the duration of this process — the sensor roster is in flux
   and pulled data shouldn't be trusted until it's done.
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
8. Once all additions are done, refresh the sensor table with the
   finished roster (see "Reading sensor locations" above — this works
   with the collector stopped, since it writes straight to the
   bind-mounted database), then **restart the collector**
   (`./ctl.sh start`).

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

1. **Stop the logger** and the collector (`ctl.sh stop`).
2. `format` (see the command reference below). Any unconfirmed readings
   are lost; that's accepted as part of a full reset.
3. Reattach only the probes that should be on the new roster (all of them,
   for a fresh start; only the good ones, if retiring a bad sensor), then
   reboot so the boot-time scan registers them fresh (see "Adding a new
   sensor" above for naming each one).
4. Refresh the sensor table with the new roster (see "Reading sensor
   locations" above), then **restart the collector** (`ctl.sh start`).

## Command reference

All of these are sent as `udp_client.py <command>` (see the intro above)
— the table lists the command name/args you pass to `udp_client.py`, not
raw wire bytes.

| `udp_client.py` command | Effect |
|---|---|
| `table` | The persistent sensor table: index, romcode, label for every registered probe (including one not currently on the bus — its readings show as invalid rather than disappearing). |
| `label <index> <string>` | Rename the probe at table index `<index>` (must already have a `labels.dat` entry — auto-created at boot). |
| `config get` | Show the receiver's retry policy (`max_retries`, `retry_interval_ms`) that `collector.py` reads at session start, plus the *currently-running* `sample_interval_ms`. |
| `config set <max_retries> <retry_interval_ms>` | Update the retry policy. Bounds: retries 1–255, interval 100–600000ms. Defaults: 5 / 5000ms. |
| `config sample <ms>` | Set the sampling interval for the next reboot (see "Sample rate & retention" above — **not live**). Bounds: 1000–3600000ms. Default: 5000ms. |
| `settime YYYY-MM-DD HH:MM:SS D` | Set the RTC. `D` is day-of-week, 1=Monday. Send **UTC** — `udp_client.py settime` (no args) does this for you from your host clock. |
| `sd` | Ring buffer status: capacity, records stored, seq range, confirmed watermark, backlog. |
| `read` | Most recent reading, for a quick manual check. |
| `format` | **Destroys everything on the SD card** — the ring, `config.dat`, and `labels.dat` alike, since it's a full card reformat, not a per-file delete. Requires the exact confirmation token; `udp_client.py format` prompts before sending it. |
| `reboot` | Restart the device (e.g. to apply a `config sample` change). Nothing is destroyed — the ring/config/label tables are all on SD and survive untouched — so no confirmation token is needed. |

## Troubleshooting

- **"rtc: clock not set" at boot** — every timestamp will be wrong until
  you run `udp_client.py settime`.
- **No reply from the device** — check it's on the network (DHCP lease may
  have changed; update `--host`), and that nothing else has the serial
  console (`fuser -v /dev/ttyACM0`) if you need to check the boot log.
- **`collector.py` exits 1** (standalone/manual run) — just rerun it; see
  "Normal operation" above for why this is always safe. Inside the
  running container this happens automatically — a failed cycle is
  logged and retried next cycle, no operator action needed; check
  `podman logs temp-collector` if it keeps failing.
- **"sensors table is empty" / "sensor_id N not in the sensors table"** —
  see "Reading sensor locations" above; the device's roster has changed
  (or this is a fresh database) and the local copy needs refreshing.
- **Container-specific issues** (won't start, `podman logs` shows
  nothing despite new data appearing, backup/recovery) — see
  `~/containers/temp-collector/README.md`, `RECOVERY.md`, and
  `ACCEPTANCE.md`, which cover the container's own operational history
  and known gotchas rather than duplicating them here.
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
