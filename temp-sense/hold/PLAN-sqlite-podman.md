# Plan: collector.py → sqlite, podman packaging, report generator

Status: draft, not started. Nothing in this plan has been implemented.

## 1. collector.py: CSV -> sqlite

- New `--db` flag (default `temp_data.db`), replacing `--csv`.
  `sensor_table.csv` stays a CSV, untouched -- it's separate,
  rarely-changed reference data (device sensor_id -> label), not the
  per-cycle data path this change is about.
- New `readings` table:
  `timestamp_epoch INTEGER, label TEXT, timestamp_utc TEXT, temp_c REAL,
  valid INTEGER`, `PRIMARY KEY (label, timestamp_epoch)`.
- `PRAGMA journal_mode=WAL` on open, so a future reader (report generator)
  can read concurrently with this script's writes.
- Dedup moves from the in-memory `seen_keys` set + CSV rescan on startup to
  `INSERT OR IGNORE`, relying on the primary key -- drops the need to
  reload prior state into memory on every run.
- Still one-shot: open db, insert per packet, commit, close, exit -- same
  invocation model as today (run periodically by cron/systemd timer, not a
  daemon).
- Exit codes, retry/CRC/ACK protocol logic: unchanged.

## 2. Podman packaging

- `Containerfile`: `python:3.12-alpine` base, `COPY collector.py`,
  entrypoint `python3 collector.py`. No dependencies beyond the stdlib
  (`sqlite3`, `socket`, `struct`, `zlib`, `csv`, `argparse`), so no
  requirements.txt needed.
- `temp_data.db` (+ its `-wal`/`-shm` sidecar files) and `sensor_table.csv`
  live on a named podman volume (`temp-sense-data`), not baked into the
  image -- the container runs with `--rm` once per pull, so anything not
  on a volume is lost between runs.
- Networking: collector only makes outbound UDP request/reply to the
  Pico's LAN address. Rootless podman's default `slirp4netns` networking
  should be sufficient for that (no inbound listener needed). Fall back to
  `--network=host` only if reachability from the container host turns out
  to be a problem in practice.
- One-time bootstrap after first volume creation:
  `podman run --rm -v temp-sense-data:/data:Z temp-sense-collector
  --host <pico-ip> --table --table-file /data/sensor_table.csv`
- Periodic invocation: a systemd timer on the container host (the
  containers run on a separate machine from this one) running
  `podman run --rm -v temp-sense-data:/data:Z temp-sense-collector
  --host <pico-ip> --db /data/temp_data.db
  --table-file /data/sensor_table.csv` hourly -- replacing today's bare
  `./collector.py` cron/timer entry.

## 3. Report generator (deferred -- shape not yet decided)

- Separate image from the collector, not bundled into the same container.
  Rationale: different dependency footprint (collector is stdlib-only;
  a report generator likely wants plotting/templating libs), different
  cadence, and isolating the UDP protocol path from report-rendering bugs.
- Mounted `:ro` against the same `temp-sense-data` volume. WAL mode lets
  it read while the collector writes concurrently -- no locking/
  coordination needed between the two containers.
- Runtime shape (one-shot/cron script, on-demand CLI, or a persistent web
  dashboard) is still open -- doesn't change anything above, since all
  three consume the same read-only volume mount the same way. Revisit
  when ready to design it.

## 4. Docs / .gitignore (repo-hygiene follow-up, do alongside step 1-2)

- `OPERATIONS.md` "Normal operation" section: update CSV wording to
  describe the sqlite db instead (column list, dedup-by-primary-key
  phrasing).
- `OPERATIONS.md`: new subsection for podman build/run/volume/timer steps,
  placed near "Reading sensor locations" (both are about running
  collector.py), not appended at the end of the file.
- `.gitignore`: add `temp-sense/temp_data.db`, `temp-sense/temp_data.db-wal`,
  `temp-sense/temp_data.db-shm` alongside the existing
  `temp-sense/temp_data.csv` / `temp-sense/sensor_table.csv` entries.

## Open questions

- Report generator's runtime shape (see section 3) -- not decided yet.
- Whether existing `temp_data.csv` history should be imported into the new
  sqlite db, or left behind/archived. Not addressed by this plan as
  written; the new collector.py does not read or migrate the old CSV.

## Sequencing note

Test order matters: verify the sqlite migration (step 1) against the real
Pico W on hardware *before* wrapping it in a container, so a failure is
attributable to one change at a time, not conflated with podman/networking
variables.
