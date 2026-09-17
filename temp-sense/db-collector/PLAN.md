# Temp-logger data collector: add DB storage + containerize as a persistent service

## Context

The Pico firmware side of this (`temp-sense/`) is already done: DS18B20 sensors +
DS3231 RTC, SD-card ring buffer, and a complete custom UDP request/data/ack protocol
(`temp-logger-udp-protocol.md`, v1.2). `collector.py`, one level up, already fully
implements that protocol — it pulls pending readings from the Pico over UDP and
currently writes them to two CSV files (`temp_data.csv` for readings,
`sensor_table.csv` for sensor-index→label mapping). It's meant to be run periodically
(hourly, the protocol's normal cadence) but isn't currently scheduled anywhere, isn't
in a database, and isn't containerized.

`~/picodev/PICO-CONTAINER-DESIGN.md` on `dev10` (Mike's own design doc, verified
2026-08-29) already made most of the surrounding architectural calls and just hasn't
been executed yet:
- Collector should be a **long-running container**, following the pattern already
  proven at `~/containers/mosquitto/` (`ctl.sh` / `recreate.sh` / `backup.sh` /
  `RECOVERY.md` / `ACCEPTANCE.md`), not the dev-toolchain container's ephemeral pattern.
- Database should be **SQLite as a bind-mounted file**, not a separate DB service —
  "the database is not a service at all, just a file in that bind mount," matching
  the mosquitto project's lesson that bind mounts (not named volumes) are what survive
  a `podman volume prune` / image rebuild.
- Networking: **default rootless mode**, unicast to the Pico's configured IP. No
  broadcast needed (ruled out already), and *not* `pasta` — pasta would only help
  broadcast/attribution, which don't apply here (single collector, not multiple
  untrusted LAN devices publishing to a shared broker).
- The open gap explicitly flagged there: no DB integration yet, no phase/pattern
  written for the collector container, and no scheduling/persistence story.

This plan closes that gap: add SQLite storage to `collector.py`, then package it as a
`~/containers/temp-collector/` service mirroring the mosquitto project's operational
pattern.

Decisions already made with Mike for this plan:
- **Edit `collector.py` in place** in this clone of `pico-dev` on `dev10`, rather than
  forking a copy — keeps the collector next to the protocol spec it implements.
- **Scheduling = a sleep-loop inside one long-running container** (poll → sleep to next
  hour → repeat), not a host systemd timer firing short-lived container runs. This is
  what makes the mosquitto `ctl.sh start/stop/status/restart` pattern applicable at all.
- **Poll interval is set via a config file, not a startup option/env var.** Mirrors a
  precedent already in this repo: the Pico firmware's own sample interval is runtime
  -configurable via `config.dat` rather than fixed at flash time (commit `fda2587`). A
  file read fresh each loop iteration lets the interval be tuned with a text edit and
  no container rebuild; a startup option would require `ctl.sh restart` (rebuild from
  `recreate.sh`) for every change — worse for something likely to get tuned while
  watching how it behaves.
- **The container must stop gracefully** on `podman stop` (SIGTERM), not rely on the
  10s grace period + SIGKILL. See step 3.

## Approach

### 1. Add SQLite storage to `collector.py`

Modify `temp-sense/collector.py` (one level up from this file) in place:

- Replace the CSV writers with a SQLite database (file, e.g. `temp_sense.db`), opened
  with `sqlite3` (stdlib — no new dependency). Two tables, mirroring the existing CSV
  schemas exactly so the dedup logic and column meanings carry over unchanged:
  - `sensors(id INTEGER PRIMARY KEY, romcode TEXT UNIQUE, label TEXT)` — replaces
    `sensor_table.csv`, refreshed by the existing `--table` flag.
  - `readings(timestamp_epoch INTEGER, label TEXT, timestamp_utc TEXT, temp_c REAL,
    valid INTEGER, PRIMARY KEY(timestamp_epoch, label))` — replaces `temp_data.csv`.
    The composite primary key reproduces the script's current dedup-by-`(label,
    timestamp)` behavior via `INSERT OR IGNORE`, which is exactly what makes retried
    transfers safe today.
- Take the DB path as a CLI flag / env var (e.g. `--db-path`, defaulting to something
  sane) rather than hardcoding, so the container can point it at the bind mount.
- Leave the UDP protocol implementation (retries, CRC handling, transfer/ack logic)
  untouched — only the storage layer changes.
- Test this step standalone on `dev10` against the real Pico before touching
  containers at all: run `./collector.py` manually, confirm rows land via
  `sqlite3 temp_sense.db "select * from readings order by timestamp_epoch desc limit 5;"`.

### 2. Scaffold `~/containers/temp-collector/`, mirroring `~/containers/mosquitto/`

New project directory (outside this repo, alongside the mosquitto project), same file
shape:

| File | Role |
|---|---|
| `recreate.sh` | Builds/replaces the container. Bind-mounts this repo's `temp-sense/` directory (code — changes often, never baked into the image), a `config/` directory (holds `collector.conf` — the poll interval, see step 3), and a `data/` directory (holds `temp_sense.db` — the "the image holds tools, the bind mount holds work" split from `PICO-CONTAINER-DESIGN.md` §8). `--userns=keep-id` so files come out owned by `mike`, same reasoning as mosquitto's `config/`/`data/`. Default network mode — no `--network=pasta` flag (see Context). |
| `ctl.sh` | `start` / `stop` / `restart` / `status`, ported directly from `~/containers/mosquitto/ctl.sh`: `start` runs the existing container, `restart` rebuilds from `recreate.sh`, falling back to `recreate.sh` on first run. |
| `Containerfile` | Minimal Python base (stdlib `sqlite3` only — no extra deps), pinned by digest like mosquitto's `eclipse-mosquitto@sha256:...`. |
| `backup.sh` | rsync `data/temp_sense.db` to the external drive, same shape as mosquitto's `backup.sh` (check drive mounted, exit non-zero if not). |
| `README.md`, `RECOVERY.md`, `ACCEPTANCE.md` | Ported structure from the mosquitto project's docs, adapted to this container. |

Reference files to reuse/copy patterns from (don't reinvent):
- `~/containers/mosquitto/recreate.sh`, `ctl.sh`, `backup.sh` — direct structural templates.
- `~/containers/mosquitto/RECOVERY.md`, `ACCEPTANCE.md` — structure/format templates.
- `~/picodev/PICO-CONTAINER-DESIGN.md` §4 (networking decision), §8 (image vs. bind-mount
  split), §10 (this exact container's requirements, already written).

### 3. Entrypoint: the sleep-loop

A thin wrapper script (e.g. `run_loop.sh`, or a `--loop` mode added to `collector.py`
itself) becomes the container's main process — and it has two responsibilities beyond
just looping: reading the interval from a config file each cycle, and stopping
promptly on SIGTERM.

**Interval from a config file.** `config/collector.conf` (bind-mounted, plain text or
minimal key=value — e.g. `interval_seconds=3600`), read fresh at the top of every loop
iteration, not just once at startup. A missing or unparseable file falls back to a
hardcoded default (the protocol's normal hourly cadence) rather than erroring, so a bad
edit degrades to "runs hourly" instead of crash-looping. This means changing the
interval is just editing the file — no `ctl.sh restart`, no container involvement at all.

**Graceful stop.** In a container, the entrypoint script runs as PID 1, and PID 1 does
*not* get the normal default disposition for unhandled signals — an untrapped SIGTERM
is silently ignored rather than terminating the process, so without an explicit trap
`podman stop` would sit out the full grace period (default 10s) and then SIGKILL,
every time. Needs:
- `trap 'graceful_exit' TERM` in the wrapper, and a `sleep N & wait $!` form (not a bare
  foreground `sleep N`) so the trap fires immediately instead of waiting for sleep to
  finish.
- If SIGTERM arrives while a poll cycle is in flight, forward it to the `collector.py`
  child and wait for it to exit before the wrapper exits itself — don't kill it
  mid-transfer. The UDP protocol already tolerates an abandoned/incomplete transfer
  (retried wholesale next cycle per `temp-logger-udp-protocol.md`), but `collector.py`
  should still catch SIGTERM to close its SQLite connection cleanly rather than being
  killed with a transaction half-committed.

```
while true; do
  interval=$(read_interval_or_default config/collector.conf)
  ./collector.py --db-path /app/data/temp_sense.db &
  wait $!
  sleep "$interval" &
  wait $!
done
```
(illustrative — exact signal-forwarding/trap wiring gets nailed down during
implementation, not in this doc.)

### 4. Verification, end to end

1. **Storage layer** (before any container): modified `collector.py` run manually on
   `dev10` against the real Pico → confirm readings and sensor table land correctly in
   SQLite, confirm re-running (retry/no-op case) doesn't duplicate rows.
2. **Single run in container**: `podman build`, then one manual `podman run` (loop
   disabled or interval irrelevant) → confirm the same DB file appears correctly in
   `data/`, owned by `mike`.
3. **Service behavior**: `ctl.sh start` with a short test interval → confirm multiple
   poll cycles actually fire and accumulate distinct readings; `ctl.sh status`;
   `ctl.sh restart` after an edit to `recreate.sh` actually picks up the change (same
   check mosquitto's docs call out — `podman restart` alone would not).
4. **Data survival gate** (mirrors `PICO-CONTAINER-DESIGN.md` §8's proof and
   mosquitto's `ACCEPTANCE.md` TC-7): `podman rm` the container (and optionally
   `podman rmi` the image), re-run `recreate.sh`, confirm `data/temp_sense.db` and its
   rows are untouched.
5. **Graceful stop**: `ctl.sh stop` (or `podman stop`) while idle (sleeping) → confirm
   the container exits well under the SIGKILL grace period, not at the 10s timeout.
   Repeat while a poll cycle is actually in flight → confirm it finishes/aborts cleanly
   (no truncated write, `podman logs` shows a clean shutdown message, not a kill).
6. **Live interval change**: with the container running, edit `config/collector.conf`
   to a short interval → confirm the *next* cycle uses it without any `ctl.sh` command.
   Also confirm a malformed/missing config file falls back to the hourly default
   instead of crash-looping.
7. Write up the executed checks as `ACCEPTANCE.md`, same format as mosquitto's.

### Deliberately deferred (not in this pass)

- Auto-start on boot / `--restart` policy / `loginctl enable-linger` — mosquitto
  shipped without this too ("no auto-start... revisit if that changes"); worth adding
  once the manual flow is proven, not before.
- Backup/off-site (git for scripts, rsync for `data/`) — structurally ported in step 2
  but not exercised against the real backup drive until the rest is verified.
