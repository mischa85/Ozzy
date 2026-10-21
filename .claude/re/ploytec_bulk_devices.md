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
- **Both ways, transfers must be whole packets of the endpoint** (DB4 fw 1.4.1, Linux, 2026-10-21).
  OUT: the DB4 reads every packet on EP 0x05 (interrupt, wMaxPacketSize 964) as a full 964-byte
  packet of two 482-byte halves. Transfers of 20, 40 and 60 frames (964, 1928, 2892 bytes) play
  clean; 10 and 30 frames (482 and 1446 bytes, ending in a 482-byte short packet) play garbage
  (tones come back at wrong frequencies, discontinuities near full scale), with 1 or 4 queued. So
  the out step is 20 frames on the 964 layout and 10 on the 512 layout (one packet each). The
  user's earlier report that the 964 layout works as a single 482-byte half is not reproduced.
  IN: packets are the device's choice, and the DB4 (fw 1.4.1, EP 0x86 interrupt,
  wMaxPacketSize 1024) always sends full 1024-byte packets, 16 frames each (Linux usbmon
  2026-10-21: with 1024-byte requests, 2417 completions, all status 0 and exactly 1024 bytes; no
  short packet). A request that isn't a multiple of 1024 bytes is overrun: 8 frames (512 bytes)
  completes every transfer with -EOVERFLOW (babble) and 0 bytes, and 24 frames babbles as well.
  On Linux one babble on EP 0x86 also leaves capture dead until a replug (xhci "Event dma ... for
  ep 12 status 3 not part of TD" on every restart after it); the engine's restart doesn't recover
  it. So the IN step is wMaxPacketSize / 64 frames: 16 on the DB4, 8 on a 512-byte endpoint (DB2).
  The user's earlier report that 8-frame IN worked on the DB4 is not reproduced; main's pre-engine
  driver always used 80-frame IN transfers. 16 frames in with 1 queued records clean.
- **The official Windows driver does the same** (Allen & Heath 2.9.91.0, its ASIO control panel,
  DB4 at 48 kHz, USBPcap 2026-10-21, `captures/db4_windows_official/official_panel.pcap` with the
  registry changes timestamped in `official_panel.log`). The panel's "USB buffer length" (ms) and
  "Number of USB buffers" are `PerformanceMode_USB_LEN` / `_NUM` under
  `HKLM\SYSTEM\CurrentControlSet\Services\XONE_DB4_USB\Parameters` ("Performance mode" is a
  preset of the two; "Default ASIO buffer size" is `DefBufferSize`), written on Close:

  | Panel | OUT transfer | OUT in flight | IN transfer | IN in flight |
  |---|---|---|---|---|
  | 5 ms x 4 | 12 x 964 B = 240 frames | 4 | 15 x 1024 B = 240 frames | 4 |
  | 4 ms x 4 | 9 x 964 B = 180 frames | 4 | 12 x 1024 B = 192 frames | 4 |
  | 3 ms x 3 | 7 x 964 B = 140 frames | 3 | 9 x 1024 B = 144 frames | 4 |
  | 2 ms x 3 / x 2 | 4 x 964 B = 80 frames | 3 / 2 | 6 x 1024 B = 96 frames | 4 |
  | 1 ms x 3 | 2 x 964 B = 40 frames | 3 | 3 x 1024 B = 48 frames | 4 |

  The length is rounded down to whole packets each way (20 frames out, 16 in at 48 kHz); the
  number of buffers sets only the OUT queue, IN always keeps 4. **Changes apply live**: no control
  request, no restart, at most a 5 ms gap between OUT completions while the sizes switch (Ozzy
  restarts the engine for each). Its smallest: 40 frames out x 2, 48 in x 4. "Number of USB buffers"
  doesn't offer 1 (the user checked the list): the official driver never runs one OUT transfer in
  flight, the case that starves the DB4 on macOS.
- Ozzy offers only whole-packet sizes (`ozzy_pcm_step`, from the endpoints in the driver's info):
  the engine refuses the rest and rounds a kept value up at start.
- **One transfer in flight ("no queue") works on Linux, not on macOS** (DB4, 2026-10-21).
  Linux: 20 frames out / 16 in, one each, records clean at 48 and 96 kHz. The device paces OUT
  itself (NAKs until it has room), so its own buffer covers the gap while the next transfer is
  submitted. macOS (kext): one OUT transfer in flight glitches at every size (20, 40, 80 frames;
  96 kHz: hundreds of glitches in 5 s, the audio about 1-3% longer), two are clean (20/16 x2,
  40/32 x2, 80/80 x4). A sawtooth on output 5 shows why: the DB4 holds the last sample (a frame
  played twice, ~15000 in 6 s) once per 20-frame transfer, so it runs dry before each next
  packet. The kext resubmits 2-12 us after the completion (XHC0 capture: IOUSBHost submit after
  complete), so the gap is below the driver, in how soon macOS's USB stack puts a newly submitted
  interrupt transfer on the wire. One IN transfer in flight is mostly fine there (8 glitches in
  5 s at 96 kHz). Ozzy still offers 1 on macOS (the user's choice: documented, not hidden).
  Not known: the device buffer's depth, and Windows.
- **The DB4's IN side dies when the host stops polling it** (fw 1.4.1, Windows, 2026-10-21). With
  the IN queue run dry for a few ms (the driver held the completion path 5 ms on purpose, 80
  frames x 2 queued), EP 0x86 never completes again: requests stay in flight with no error,
  OUT runs on. An engine restart (abort, handshake, clear halt) doesn't bring it back; a port
  reset (bus reset) does. It doesn't always happen: a ~110 ms stall with 16 x 4 queued came
  back by itself. Same shape as the Linux babble case above and the user's macOS user-mode
  driver. Not known: what the firmware does (an IN FIFO overflow is the guess).
- **What stalled the host on the Windows test box** (Threadripper 1920X, MSI X399 Gaming Pro
  Carbon AC, BIOS 1.D9, AMD fTPM 3.5.0.3): the AMD fTPM stutter. A Windows TPM command (tpm.sys,
  through ACPI) stops every CPU for ~10 ms (no CPU samples on any of 24 CPUs, no interrupts),
  then the xHCI raises no interrupt for ~100 ms while the CPUs run again: 67-130 ms with no
  completion on either pipe (xperf DPC/ISR/profile trace). Each `Get-TpmEndorsementKeyInfo`
  while streaming made 3 late OUT and 2 late IN completions, 6 times out of 6. No DPC or ISR of
  any driver ran longer than 2 ms. AMD's fix (AGESA 1.2.0.7, 2022) was for AM4 boards.
- The queue of 4 Ozzy used had no basis in the protocol: d785fd4 (2026-10-17) copied Linux's old
  `OZZY_PCM_N_URBS`; before that the engine kept 2. It is a setting now too
  (`OZZY_SETTING_PCM_OUT_QUEUE` / `_IN_QUEUE`, 1-4, default 4).

**Measured round trip** (DB4 fw 1.4.1, Linux, PipeWire's JACK layer, 2026-10-21): a one-sample
click on output 5, found again on input 5 (the DB4 loops each USB return to its USB send),
sample-exact in JACK time; every click of a run gave the same number. 48 kHz unless noted:

| Transfers (out/in, queued) | PipeWire quantum | Round trip |
|---|---|---|
| 20/16, 1 | 1024 (44.1 kHz) | 2239 frames (50.8 ms) |
| 20/16, 1 | 256 / 128 | 560 / 460 frames (11.7 / 9.6 ms) |
| 20/16, 1 | 64 | 327-338 frames (6.8-7.0 ms) |
| 20/16, 1 | 32 | 272-300 frames (5.7-6.3 ms) |
| 20/16, 2 | 64 / 32 | 348 / 292 frames |
| 80/80, 4 (main's fixed sizes) | 64 | 780 frames (16.3 ms) |
| 80/80, 4 | 32 | 699-870 frames, varying, and 1284 xruns on the input node |

Above 2 quanta, about 200-240 frames (4-5 ms) are fixed: the driver's 36 (20 out + 16 in), the
rest PipeWire's ALSA side and the DB4's own buffer and mixer, not separated (that needs a raw
ALSA measurement with PipeWire off the device). The numbers move by ~10-30 frames between
restarts (the phase of the transfers to the graph's cycle).

So the host's latency is transfers in flight × transfer size, in steps of one packet each way
(DB4: 20 frames out, 16 in)
(plus the device's own buffer, which the host can't see). MIDI out
waits behind the same queue (its slot is in every out sub-packet), so it gets faster with it.
