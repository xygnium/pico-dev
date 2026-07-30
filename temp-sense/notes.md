# Hardware setup

Debugprobe brought up on VirtualBox archlinux VM:
1. Hold BOOTSEL on Pico while plugging USB from workstation
2. In VB head view, claim USB device: Devices → USB → Raspberry Pi Debugprobe on Pico (CMSIS-DAP) [0231]
3. Device appears in /dev: `lsblk -f` to find (e.g., /dev/sdb1)
4. Mount: `sudo mkdir /mnt/RPI-RP2 && sudo mount -o gid=users,fmask=113,dmask=002 /dev/sdb1 /mnt/RPI-RP2`
5. Copy debugprobe firmware: `sudo cp -v debugprobe_on_pico.uf2 /mnt/RPI-RP2/`
6. LED illuminates when ready; SWD download works

# SD card module wiring

Defined by `hw_config.c` (copied from `../gmcount/`, byte-identical to
`../sdsc/`'s). These are the **default SPI0 pins** on the RP2040, so no GPIO
function remapping is involved.

| SD module pin | Pico GPIO | Pico physical pin | Notes |
|---|---|---|---|
| `CS` / `SS` / `CD` | GP17 | 22 | slave select, driven by the driver |
| `SCK` / `CLK` | GP18 | 24 | |
| `MOSI` / `DI` / `SDI` | GP19 | 25 | Pico → card |
| `MISO` / `DO` / `SDO` | GP16 | 21 | card → Pico |
| `GND` | — | 23 | pin 23 sits right between 22 and 24 |
| `VCC` | — | 36 (3V3 OUT) **or** 40 (VBUS, 5V) | **depends on the module — see below** |

## VCC: check which module you have before powering it

- **Bare/passive microSD breakout** (just a socket, maybe a few resistors):
  wire `VCC` to **3V3 OUT (pin 36)**. Feeding it 5V puts 5V on the card and on
  the Pico's GPIOs, which are not 5V tolerant.
- **Module with an onboard regulator and level shifter** (the common
  "HW-125"/Catalex style, with an LDO and a 74LVC125): wire `VCC` to **VBUS
  (pin 40, 5V)**. These need the headroom — the LDO drops ~1.2V, so feeding
  3V3 in leaves the card at ~2.1V and it will behave erratically or not
  enumerate at all. Some of these have a separate 3V3 pad that bypasses the
  regulator; if so, use that with pin 36 instead.

Getting this wrong is the single most common cause of "card not detected" on
these modules, and it fails in a way that looks like a wiring or software
fault rather than a power one.

## Not wired

- **Card detect.** `hw_config.c` sets `.use_card_detect = false`, so the
  `.card_detect_gpio = 13` entry is ignored — leave the module's `CD` pin (if
  it has a separate one) unconnected. Note some modules label chip-select `CD`;
  that one goes to GP17.

## Existing pin assignments on this board — no conflicts

| Function | GPIO | Physical |
|---|---|---|
| DS18B20 1-Wire DQ | GP15 | 20 | (external ~4k pull-up to 3V3) |
| DS3231 I2C0 SDA | GP8 | 11 |
| DS3231 I2C0 SCL | GP9 | 12 |
| CYW43 (WiFi, internal) | GP23/24/25/29 | — |

SPI0's 16–19 are clear of all of these. **One trap:** the commented-out `spi1`
alternative at the top of `hw_config.c` uses GP15 for MOSI, which would collide
with the 1-Wire DQ line. Don't uncomment it on this board without moving one of
them.

## Signal integrity

Bus speed is `125 MHz / 8` = **15.625 MHz** (`hw_config.c`). That is fine on a
short, tidy harness but optimistic for long breadboard jumpers. If the card
enumerates but reads/writes fail intermittently, halve it first
(`125 * 1000 * 1000 / 16`) before suspecting the firmware — the card's own
initialisation runs at ≤400 kHz regardless, so a card that mounts and then
misbehaves under load is the classic symptom of too fast a clock.

SD cards draw current spikes on write; if the module has no bulk capacitor,
put 10–100 µF across its `VCC`/`GND` close to the module.

# DS18B20 temperature conversion wait strategy

**Current approach (working):** Fixed 800ms sleep after CONVERT_T command.
- DS18B20 max conversion time: 750ms (12-bit resolution)
- Appropriate for this project: 3 sensors, 5-second measurement loop, simplicity valued

**Alternative: Completion bit polling** — check bit 7 of scratchpad byte 0 to exit early.
Use if/when:
- Many sensors (>10): reduce cumulative wait time
- Tight timing: need response ASAP after measurement
- Battery-powered: every 100ms of idle time matters
- Precise timestamping: know exact conversion end time

Polling pattern (not needed now, reference for future):
```c
bool done = false;
uint32_t start = time_us_32();
while (!done && time_us_32() - start < 1000000) {
    ow_reset(&ow);
    ow_send(&ow, OW_SKIP_ROM);
    ow_send(&ow, DS18B20_READ_SCRATCHPAD);
    done = (ow_read(&ow) & 0x80) != 0;  // bit 7 = conversion complete
    if (!done) sleep_ms(10);
}
```

# Bug history

**pico-examples Issue #422 — Conversion wait bug (2026-07-16):**
- Original code: `while (ow_read(&ow) == 0);` on line 74
- Problem: Reads full bytes from 1-Wire without protocol, interferes with device state/timing
- Symptom: Garbage temperature reads (-0.06°C = 0xFFFF, all devices report same invalid value)
- Fix: Replace with `sleep_ms(800)` (DS18B20 max conversion: 750ms @ 12-bit resolution)
- Status: **FIXED**. Related to pico-examples Issue #422 ("1-Wire bad signal after DS18B20_CONVERT_T")

**pico-examples Issue #569 — Missing CRC validation (2026-07-16):**
- Problem: Scratchpad read doesn't validate 9-byte CRC (byte 8), allowing silent data corruption
- Fix: Read all 9 bytes, validate CRC-8 (Dallas 1-Wire polynomial 0x8C), reject reads with invalid CRC
- Status: **FIXED**. All sensor reads now pass CRC validation; no bus errors detected in testing

