# Temp-Logger UDP Download Protocol (v1.2)

## Design assumptions carried over from discussion
- There is exactly one collector (receiver) for this device, always. The protocol does not need to support multiple, replaceable, or amnesiac receivers — the logger trusts its own bookkeeping rather than anything a REQUEST might assert about the receiver's position.
- Each reading is self-identifying: `(sensor_id, timestamp)` is unique and never collides, because the RTC is battery-backed and time inconsistencies are validated/handled downstream.
- Because readings are self-identifying, the receiver does **not** need in-order delivery or explicit dedup logic beyond keying its store by `(sensor_id, timestamp)`. Sequence numbers exist purely to track *completeness*, not ordering.
- A failed/incomplete transfer is safe to simply retry wholesale next cycle — resent data that already exists at the receiver is a no-op.

## Terms
- `N_sensors`: 3–20, count of active sensors.
- `set`: one reading round — one timestamp + one temperature value per active sensor.
- `X`: total sets pending since the logger's own last confirmed watermark.
- `SPP` (sets per packet): computed at runtime from `N_sensors` so the packet stays under the safe UDP payload (see Sizing below).
- `TP` (total packets): `ceil(X / SPP)` for a given transfer.
- `transfer_id`: 32-bit ID chosen by the logger when it begins responding to a REQUEST. Distinguishes this transfer's packets from a prior/retried one.

## Message types

| Type | Direction | Purpose |
|---|---|---|
| `REQUEST` | receiver → logger | "Send me everything since your last confirmed watermark." No payload — the logger always resumes from its own bookkeeping (there is exactly one collector, always). |
| `DATA` | logger → receiver | One packet of up to `SPP` sets. |
| `ACK` / `NACK` | receiver → logger | Sent after each window. `ACK` reports the window (or full transfer) complete — on final-window `ACK`, logger may advance its watermark. `NACK` lists missing `seq` numbers in the current window, requesting retransmission. A `NACK` sent after the receiver's retry budget is exhausted signals transfer abandonment — logger does **not** advance its watermark, so this data is naturally re-offered next cycle. |

## DATA packet header (16 bytes, before payload)

| Field | Size | Notes |
|---|---|---|
| magic | 1 B | protocol identifier, catches misdirected/garbage packets |
| version | 1 B | protocol version |
| transfer_id | 4 B | ties packet to a specific transfer attempt |
| seq | 2 B | 0..TP-1 (16 bits supports up to 65535 packets — plenty) |
| total_packets | 2 B | TP, repeated in every packet so receiver always knows total without a separate metadata message |
| set_count | 1 B | number of sets actually in this packet (last packet may be a partial window) |
| flags | 1 B | bit0 = END (last packet) |
| crc32 | 4 B | CRC32 over the payload only |

**Why an app-level CRC32 in addition to UDP's own checksum:** UDP's checksum is 16-bit, and some stacks/paths leave it zeroed (checksum disabled) or don't validate it end-to-end. CRC32 at the application layer is cheap on the Pico and gives you real corruption detection independent of the transport.

## Payload (set format)

Each set: `timestamp (4B) + count (1B) + count × [sensor_id (1B) + temperature (2B fixed-point) + valid (1B)]`.
Timestamp is shared per set (all sensors read together), not repeated per-sensor, saving bytes — sensor_id is a 1-byte index into the logger's persistent sensor table (see `label_store.h`; the `table` command fetches it), not the full 8-byte DS18B20 ROM code. `valid` is nonzero only if the reading is a real CRC-checked temperature; a sensor that didn't respond or failed its CRC that cycle still gets an entry (so every set always has exactly `N_sensors` entries), with `valid = 0` and `temperature` meaningless. The table's index only changes on an explicit register/decomm/wipe (never as a side effect of which probes happen to answer a boot's bus scan), so the receiver re-fetches it after such a change rather than once per session.

## Sizing (computed at runtime, not hardcoded)

Keep total UDP payload ≤ 508 bytes to avoid IP fragmentation.

```
set_size = 4 + 1 + N_sensors * 4
SPP = floor((508 - 16) / set_size)
```

Example: `N_sensors = 20` → `set_size = 85` → `SPP ≈ 5` sets/packet.
Example: `N_sensors = 3` → `set_size = 17` → `SPP ≈ 28` sets/packet.
Since `N_sensors` varies 3–20, compute `SPP` fresh per transfer rather than fixing it — a fixed SPP tuned for 20 sensors wastes bandwidth at 3, and a fixed SPP tuned for 3 sensors overflows the MTU at 20.

## Protocol flow

1. Receiver sends `REQUEST` (hourly, normally).
2. Logger computes `X`, `SPP`, `TP`, generates `transfer_id`.
3. Logger sends one `DATA` packet and waits for the receiver's response before sending the next.
4. Receiver sends `ACK` (packet received correctly) or `NACK` (packet missing or failed CRC, requesting retransmission) for each `DATA` packet.
5. On `NACK`, logger retransmits the same packet (same `seq` + `transfer_id`).
6. Repeat until all `TP` packets are `ACK`'d or `max_retries` is hit for a given packet. `max_retries` and the per-packet retry interval are configurable values, provided through a UDP-based configuration interface rather than hardcoded (defaults: `max_retries = 5`, retry interval = 5s).
7. Once all `TP` packets are `ACK`'d, the transfer is complete and the logger advances its watermark to the last seq in this transfer.
   If retry budget is exhausted on any packet, receiver sends a final `NACK` and abandons the transfer_id → logger leaves its watermark unchanged; the same data (plus whatever accumulated since) is offered again next `REQUEST`, deduped for free at the receiver by `(sensor_id, timestamp)`.

## What this closes from the original gap list

- **Y naming collision** — split into `SPP` (per-packet) and `TP` (total), both explicit in the header.
- **Retransmit of "last packet" only** — replaced with per-window NACK covering any missing packet, not just the last.
- **No transfer/session ID** — `transfer_id` added.
- **No explicit end-of-transfer signal** — `total_packets` in every header + `END` flag + receiver's `COMPLETE`/`FAILED`.
- **Weak/optional UDP checksum** — app-level CRC32 added.
- **MTU sizing** — formula ties `SPP` to actual `N_sensors` at runtime instead of a guessed constant.
- **Ordering/duplicates** — resolved by the `(sensor_id, timestamp)` keying already in place; sequence numbers are now purely a completeness mechanism.

## Still open / worth deciding before implementation
- Retry count and retry interval are not fixed in this protocol — they are configurable values provided through a UDP-based configuration interface (defaults: `max_retries = 5`, retry interval = 5s).

## Note on ring buffer overflow
This protocol does not signal ring-buffer wraparound (permanent data loss when the receiver is offline longer than buffer retention). Overflow gaps are handled downstream rather than in-protocol.
