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

## What bulk vs interrupt actually changes (official driver)

For Ploytec junction devices, `configureDevice` puts the PCM pipes into the *bulk* slots no
matter their transfer type:
- IN: `getBulkInPipe()`, else `getInterruptInPipe()` → `this+0xa90` (`this[0xb53]=1` if interrupt)
- OUT: `getPipeByAddress(0x05)` (or `getInterruptOutPipe()`) → `this+0xa98`
- isoc slots `0xab0` / `0xac0` are cleared; the keep-alive isoc pipe (EP 1–4 range, EP 0x02)
  goes to `0xac8`

`resumeStreaming` then always runs `bulkAudioRun` + `rtsBulkOutBulkIn`, and the OUT buffers are
always `AudioBulkIRP` (`wMaxPacketSize × 20` bytes). The dedicated interrupt path
(`interruptAudioRun`, `0xaa0/0xaa8`) is only used for JCT30 / Elektron Overbridge.

So transfer type only matters in two places:
1. **The URB/pipe type** used on the host side (obviously).
2. **The isoc keep-alive on EP 0x02**: `rtsIsocAudioRunKeepAlive` is started only if the IN
   pipe *or* the OUT pipe is bulk. All-interrupt devices (DB2 fw 1.2.0, DB4 fw 1.4.1, 4D) get no
   keep-alive. **Ozzy never sends the keep-alive.** This could matter for bulk-mode firmware
   (DB4 fw 1.3.9, DB2 fw 1.0.8). Decided not needed for Ozzy.

## Output framing depends on wMaxPacketSize, NOT on bulk vs interrupt

`AudioBulkIRP::useOut` pre-fills the MIDI idle pattern per packet of `wMaxPacketSize` bytes,
for 20 packets:

```c
if (pipe->wMaxPacketSize <= 0x200) {       // 512
    *(u16*)(pkt + 0x1e0) = 0xfffd;          // 480: FD, 481: FF
} else {                                    // e.g. 964
    *(u16*)(pkt + 0x1b0) = 0xfdfd;          // 432
    *(u16*)(pkt + 0x392) = 0xfdfd;          // 914 = 482 + 432
}
```

| Device / fw | EP 0x05 | wMaxPacketSize | Framing |
|-------------|---------|----------------|---------|
| DB4 fw 1.3.9 | bulk | 512 | `[480 PCM][MIDI][ctrl][30 pad]` |
| **DB2 fw 1.2.0** | **interrupt** | **512** | `[480 PCM][MIDI][ctrl][30 pad]` |
| DB4 fw 1.4.1 | interrupt | 964 (inferred: Ozzy's 3856 B URB = 4×964 works) | 2× `[432 PCM][MIDI][ctrl][48 PCM]` |
| Xone:4D | interrupt | 964 (issue #9 lsusb; Win log `mExpectedEndpointBytes:964`) | 2× `[432][MIDI][ctrl][48]` |

The byte after each MIDI slot is a control byte (FF when idle), and every slot can carry MIDI,
paced to 2500 bytes/s: see `ploytec_midi.md`.

Verified on the DB2 Windows capture: across 2352 non-silent 512-byte chunks, bytes 0–479
behave as audio (≈50% non-zero at every offset, including 432–433), and bytes 480–511 are
always 0. The Windows driver sends `00 00` in the MIDI slot when idle.

**Consequences:**
- Ozzy (macOS commit c1f48bd and Linux `is_bulk`) picked the 482 layout for *any* interrupt
  endpoint. That's wrong for the DB2 and was the root cause of the white noise in #72 / #48.
  Fixed: the layout now follows wMaxPacketSize.
- PR #70 and PR #83 switch interrupt to the 512 layout for *all* devices. That fixes the DB2
  but breaks the DB4 fw 1.4.1 and the 4D. The selector has to be wMaxPacketSize.
- The c1f48bd commit message ("DB2 (older firmware) exposes bulk endpoints") is only true for
  DB2 fw 1.0.8 (WeDigJames' first capture in #5). DB2 fw 1.2.0 is interrupt.
