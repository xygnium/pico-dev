# temp-sense manual test runbooks

Step-by-step procedures for a human to re-run hardware verification by hand,
using `udp_client.py` against the live device. Not a log of what was already
tested — see `VERIFICATION.md` for that. This is for re-running the same
checks later (after a change, after a suspected regression, or just to
confirm current firmware still behaves as documented).

Each test reads the device's *current* state first and picks values relative
to it — seq numbers keep advancing while the device runs, so don't use
hardcoded seq values from a previous run.

## Stage 2 — `ack` command (confirm watermark)

Prerequisites: firmware with the `ack`/`fetch` commands flashed, device on
the network, SD card mounted with existing records (`./udp_client.py sd`
shows `stored` > 0).

### Test 1 — forward ack advances the watermark

1. `./udp_client.py sd` — note the `confirmed` value (call it `C0`) and the
   upper end of the `seq` range (call it `N`, the newest written seq).
2. Pick a target seq strictly between `C0` and `N` — e.g. `C0 + 100`, or just
   `N` itself if you want to confirm everything.
3. `./udp_client.py "ack <target>"` — expect
   `ack: confirmed <target>`.
4. `./udp_client.py sd` — expect `confirmed` now equals `<target>`, and
   `backlog` reduced by exactly `<target> - C0`.

### Test 2 — idempotent re-ack

1. Re-send the same command from Test 1: `./udp_client.py "ack <target>"`.
2. Expect the identical reply, `ack: confirmed <target>`.
3. `./udp_client.py sd` — expect no change at all from Test 1's result. A
   lost ack reply must be safe to retry, so this is the check that matters
   most.

### Test 3 — backwards ack is refused

1. Using the `<target>` now confirmed (from Test 1), pick a value strictly
   less than it — e.g. `<target> - 50`.
2. `./udp_client.py "ack <target - 50>"` — expect
   `ack: refused — ... is behind the current watermark ... (would move backwards)`.
3. `./udp_client.py sd` — expect `confirmed` unchanged from Test 1/2's value.
   The refusal must not have side effects.

### Test 4 — out-of-range (unwritten) ack is refused

1. `./udp_client.py sd` — note the current newest seq, `N`.
2. `./udp_client.py "ack <N + 1000>"` — expect
   `ack: refused — ... has not been written yet (next seq is ...)`.
3. `./udp_client.py sd` — expect `confirmed` unchanged.

### Test 5 — watermark survives a power cycle

This one has a real gotcha: `meta.dat` is only written lazily, after
**256 records have been written since the device's own last flush** (not
since your last `ack`) — see `META_PERSIST_INTERVAL` in `sd_ring.c`. The
device's own last flush is normally its most recent boot. Power-cycling
before that threshold is crossed will "pass" for the wrong reason — the
watermark you expect was never actually written to disk, so this step
must wait for the real flush, not just any ack.

1. `./udp_client.py sd` — note `seq`'s upper bound as your baseline, `B`
   (this stands in for "records written since last boot/flush", which isn't
   directly queryable).
2. `./udp_client.py "ack <something recent and valid>"` to set a watermark
   worth checking, if you haven't already from the earlier tests.
3. Wait until the newest seq has advanced by **at least 256** past `B` — at
   the current 5s sample rate (3 sensors/cycle) that's roughly 7–9 minutes.
   (If the sample interval has since moved to the planned 30s, expect
   roughly 6x longer — recompute from the current interval rather than
   assuming this number.) Poll `./udp_client.py sd` periodically to check.
4. Once past the threshold, power-cycle the device (physically — this is
   not something `udp_client.py` can trigger).
5. Wait ~10s for it to reboot and reconnect, then `./udp_client.py sd`.
6. Expect `confirmed` to show the value you set in step 2, and `seq`'s
   upper bound to have continued climbing from where it left off (not reset
   to 0 — that would mean the card was re-scanned as blank, a different
   failure).
