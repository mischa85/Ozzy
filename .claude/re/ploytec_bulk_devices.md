# this[0x1888] — USB High Speed Flag (NOT "bulk device")

## What it actually is

`this[0x1888]` is a **USB 2.0 High Speed flag**, NOT a bulk transfer flag.

Set in `PGDevice::setIsUSB2Device()`:
```c
this[0x1888] = 1;
```

Called from `onServiceStart` when USB speed > 1:
```c
if (1 < usb_speed) {
    setIsUSB2Device(this);  // sets this[0x1888] = 1
}
```

USB speed values (from data block +4):
- 0 = Low Speed
- 1 = Full Speed (USB 1.1)
- 2 = High Speed (USB 2.0)
- 3 = Super Speed (USB 3.0)

## What isPloytecBulkDevice really means

`isPloytecBulkDevice` should be read as `isPloytecHighSpeedDevice` — it returns true
for known Ploytec-family devices running at USB 2.0+ speed, meaning they're **capable**
of bulk transfers and higher channel counts.

The actual bulk vs interrupt decision is made later in `configureDevice`:
1. Tries `getBulkInPipe()` — if found, uses bulk
2. Falls back to `getInterruptInPipe()` — if found, uses interrupt
3. For output: checks if pipe at address 0x05 has type 0x04 (interrupt)

## Impact on channel counts

The flag controls channel topology throughout the driver:
- **High Speed (flag=1)**: full channel counts (8, 10, 16 channels depending on device)
- **Full Speed (flag=0)**: reduced to 2 channels

Examples from `onServiceStart`:
- 0x0A4A/0x5A16: HS=16out, FS=2out
- 0x0A4A/0xDA01: HS=16in+16out, FS=2in+2out
- 0x0A4A/0xD064: HS=16in+16out+96kHz, FS=2in+2out+44100Hz
- 0x2573/0x0012: HS=16in+8out, FS=2in+2out
- 0x200C/0x1006: HS=6in+6out, FS=2in+2out

## Interrupt sub-packet wire format (confirmed for DB2 by Windows USB capture)

Both bulk and interrupt sub-packets use **identical 512-byte wire format**:

```
Bulk:      [480 bytes PCM (10 frames × 48)][2 bytes MIDI][30 bytes padding] = 512 bytes
Interrupt: [480 bytes PCM (10 frames × 48)][2 bytes MIDI][30 bytes padding] = 512 bytes
```

**MIDI is always at byte offset 480** — immediately after the 10th PCM frame.

Evidence from Windows USB capture (Windows driver, DB2 interrupt endpoint 0x05):
- URBs are 7168 bytes = 14 × 512 sub-packets (bulk format, NOT 14 × 482)
- Zero regions appear every 512 bytes at offset 480 (MIDI + 30-byte pad)
- Audio data only in bytes 0x00-0x01 per sub-packet (bit-scatter ch0 confirmed)

The Windows driver sends 14 sub-packets per URB (7168 bytes). Our driver uses 8 per URB
(4096 bytes). Sub-packet count per URB should not affect correctness — the device consumes
sub-packets independently. The 512-byte wire format is the critical invariant.

**DB2 sample rate**: 48000 Hz (0x00BB80). Windows driver sets 48kHz, NOT 96kHz.
Device power-on default is 44100 Hz (0x00AC44).

## Firmware version and transfer types

For the DB4 specifically:
- **Firmware 1.3.9**: USB descriptors expose bulk endpoints → `getBulkInPipe` succeeds
- **Firmware 1.4.1+**: USB descriptors expose interrupt endpoints → falls back to interrupt

The `this[0x1888]` flag is 1 in BOTH cases (device is still High Speed). The transfer
type difference comes from the firmware's endpoint descriptors, not from this flag.

## All Ploytec High Speed devices (from isPloytecBulkDevice)

### VID 0x0A4A (Allen & Heath / Ploytec)
0xAFFE, 0xCF00, 0xCF04, 0xD064, 0xDA01, 0xFF01, 0xFF4D,
0xFFD2 (DB2), 0xFFDB (DB4), 0xFFDD (DX), 0xFFAD (Wizard 4), 0x5A16

### Other VIDs
- 0x0A92: 0x0111
- 0x1ACC: 0x0102, 0x0103
- 0x1D03: 0x005C
- 0x200C: range 0x1005–0x1037 (bitmask)
- 0x2573 (ESU): PID < 0x2B (bitmask)
