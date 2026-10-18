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
- 0x2573 (ESI Audiotechnik, ESU product line): 0x0012, 0x0013, 0x002A (bitmask)

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

## Transfer sizes are the host's choice (down to one sub-packet)

The device's units are the sub-packets: out 10 frames (one 512-byte packet, or one 482-byte half
of a 964-byte packet), in 8 frames (one 512-byte packet, 64 bytes per frame). How many of them
go into one USB transfer, and how many transfers are queued, is up to the host:
- The official driver sends `wMaxPacketSize × 20` bytes per OUT transfer (above).
- Ozzy used to send 80 frames per transfer each way (8 out sub-packets, 10 in), 4 queued: 320
  frames ahead (6.7 ms at 48 kHz). That 80 was Ozzy's own grouping (one logical packet for the
  HAL's ring and timestamps), not the device's. Since 2026-10-18 the size is a setting per
  direction (`OZZY_SETTING_PCM_OUT_FRAMES` / `_IN_FRAMES`, 1-20 sub-packets out, 1-25 in, up to
  200 frames), 80 / 80 the default, and nothing in Ozzy groups frames by 80 any more.
- **Tested by the user early in Ozzy's development (told 2026-10-18): 10-frame OUT and 8-frame IN
  transfers work on the DB4, in both out layouts: the 512-byte packet with its padding, and the
  964 layout sent as a single 482-byte half (a short packet on the 964-byte endpoint). The
  device just completes more transfers.** In and out don't need the same size.
- **Also on Linux (told 2026-10-18): one transfer in flight ("no queue") at 10 / 8 frames works.**
  The device paces OUT itself (NAKs until it has room), so its own buffer covers the gap while
  the next transfer is submitted. Not known: that buffer's depth, and whether the macOS kext's
  and Windows' completion paths resubmit fast enough for one in flight.
- The queue of 4 Ozzy used had no basis in the protocol: d785fd4 (2026-10-17) copied Linux's old
  `OZZY_PCM_N_URBS`; before that the engine kept 2. It is a setting now too
  (`OZZY_SETTING_PCM_OUT_QUEUE` / `_IN_QUEUE`, 1-4, default 4).

- Open (from the 2026-10-18 review): a PCM IN endpoint with wMaxPacketSize 1024 (interrupt; Ozzy's
  `pcm_in_int1024`, as the tests model the DB4 fw 1.4.1) could send two sub-packets per USB
  packet, which an odd sub-packet count (8, 24, ... frames) would overflow. The user's 8-frame
  in transfers worked on the DB4, so the device sends 512-byte packets at least there; Ozzy keeps
  the 8-frame steps. Worth a usbmon look at the IN packet sizes during the next DB4 test.

So the host's latency is transfers in flight × transfer size, in 10-frame steps out and 8-frame
steps in (plus the device's own buffer, which the host can't see). MIDI out
waits behind the same queue (its slot is in every out sub-packet), so it gets faster with it.
