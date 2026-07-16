# Hardware setup

Debugprobe brought up on VirtualBox archlinux VM:
1. Hold BOOTSEL on Pico while plugging USB from workstation
2. In VB head view, claim USB device: Devices → USB → Raspberry Pi Debugprobe on Pico (CMSIS-DAP) [0231]
3. Device appears in /dev: `lsblk -f` to find (e.g., /dev/sdb1)
4. Mount: `sudo mkdir /mnt/RPI-RP2 && sudo mount -o gid=users,fmask=113,dmask=002 /dev/sdb1 /mnt/RPI-RP2`
5. Copy debugprobe firmware: `sudo cp -v debugprobe_on_pico.uf2 /mnt/RPI-RP2/`
6. LED illuminates when ready; SWD download works

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

