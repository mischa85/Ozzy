# Tascam US-366 (0644:8041): descriptors and the official Windows driver on the wire

Source: #91, from d-hey (2026-10-28): `lsusb`-style descriptors (USBView) and a USBPcap capture
of plugging in and playing, US-366 firmware 1.03, Tascam driver 1.04. Saved in the
investigation folder's `logs/github_issue_91/` (`x/us366.pcapng`, `x/tascam_US366_descriptors.txt`);
parser `tools/usbpcap.py`. Two sessions in the capture: attach at 44.1 kHz → 48 kHz, and after
a replug at 29 s, 48 → 96 kHz. Ozzy drives it since 2026-11-01 (family `PLOYTEC_US3XX`,
untested on hardware): see "What Ozzy does" at the end.

## Descriptors

High Speed, bcdDevice 2.00, one configuration, 4 vendor interfaces (class 0xFF):

| Interface | Alt | Endpoints |
|---|---|---|
| 0 | 0 | none |
| 1 | 1 | 0x02 isoc OUT async, 294 B; 0x85 isoc IN, 3 B (feedback, sync none) |
| 1 | 2 | 0x02 isoc OUT async, 512 B (no feedback) |
| 2 | 1 | 0x81 isoc IN async, 294 B |
| 2 | 2 | 0x81 isoc IN async, 512 B |
| 3 | 0 | 0x83 bulk IN 512 B; 0x04 interrupt OUT 16 B |

Not the Xone protocol (no bulk/interrupt PCM, no Ploytec sub-packets) and not the "bulk in /
isoc out" family either: **both PCM directions are isochronous**.

## What the Windows driver does

- Interface selection inside SELECT_CONFIGURATION (no SELECT_INTERFACE URB): **alt 2 on
  interfaces 1 and 2** (512-byte packets), so the 0x85 feedback endpoint is never used.
- Every microframe carries the same number of frames: 6 at 48 kHz, 12 at 96 kHz (no 5/7
  alternation, no feedback), in transfers of 16 packets (2 ms).
- **PCM out (0x02): plain S24_3LE, 6 channels** (18 bytes/frame: 108 B at 48 kHz, 216 at 96).
  In the playback, channels 1-2 and 3-4 carried the same stereo signal at different levels;
  5-6 stayed silent (probably S/PDIF out).
- **PCM in (0x81): plain S24_3LE, 4 channels** (12 bytes/frame: 72 B / 144 B). Signal on 1-2
  in the capture. The kext's `readUS3XXChannelConfig` makes the US-366's input count 2/4/6 by
  mixer mode ('I' wIndex 1 bits 0x08/0x10, `ploytec_vendor_request_I.md`); here 'I' wIndex 1
  read 0x0F.
- **MIDI in (0x83, bulk): USB-MIDI 1.0 event packets** (`0B B0 66 7F`: CIN 0xB, cable 0,
  control change), 8 or 20 bytes per transfer. CCs 0x66-0x6C with 7F/00 were seen (front panel
  or status, unknown). MIDI out (0x04) wasn't used.

Control requests, first session (times from attach):

| t (s) | Request | Notes |
|---|---|---|
| 0.054 | 'V' (0x56) read 15 | `33 01 03 56 31 2E 30 33 00 20...`: chip 0x33, "V1.03" |
| 0.058 | SET_CONFIGURATION 1 | |
| 0.196 | 'I' read wIndex 1 → 0x0F; 'I' read wIndex 0 → 0x02; 'I' write wIndex 0 = 0x000A | |
| 0.243 | GET_CUR (bmRequestType 0xA2, bRequest 0x81, wValue 0x0100, **wIndex 0**) → 44100; 'I' read → 0x0A; GET_CUR twice more | |
| 0.243-0.608 | SET_CUR 48000 (0x22 0x01, wValue 0x0100) to wIndex 0x0081, then 0x0002, three pairs | each request takes the device **~54 ms** to complete (it holds the status stage), hence the spacing |
| 0.352 | **'M' (0x4D) write 64 B**: 32 zero bytes, then `7FFF 7FFF 4000 4000 3E8B 3E8B 0000...0024` | mixer? first session only |
| 0.353 | **'B' (0x42) write 256 B**: starts `7FFF 7FFF 7FFF 7FFF 0C30 1B36...` | DSP coefficients? first session only |
| 0.498 | 'A' (0x41) wIndex 0x0101: read 5 from 0x0D00 → `81 00 2C 00 00`; writes 0x0D04, 0x0E00, 0x0F00, 0x1002, 0x110B | the S/PDIF transmitter's channel status bytes 0-4 (Ozzy's `hs_spdif_out`): consumer, 48 kHz (0x02; 0x0A at 96 kHz), 24 bit |

The 96 kHz session: 'V', SET_CONFIGURATION, 'I' reads (0x0F, 0x0A, no write), GET_CUR →
48000, SET_CUR 96000 ×3 each, the same 'A' writes with 0x100A (all five registers again, even
the ones that already hold the value). No 'M' / 'B'. The device still had the selector (0x0A)
and the rate (48000) from the first session, so the "replug" at 29 s didn't power it off; it
probably kept the 'M' / 'B' state too (a guess).

Streaming starts once the last SET_CUR completes (0.608 s), in this order: isoc IN 0x81, isoc OUT
0x02, bulk IN 0x83. Every transfer is 16 packets (2 ms); Windows keeps up to **9 IN** and **4 OUT**
transfers in flight. IN packets were 72 bytes (6 frames) at 48 kHz and 144 (12) at 96 kHz,
except the first packets of each session (0, 60 or 132 bytes: the stream starting mid-way).

The full 'M' and 'B' payloads (hex):

    M: 0000000000000000000000000000000000000000000000000000000000000000
       7fff7fff400040003e8b3e8b0000000000000000000000002400000000000000
    B: 7fff7fff7fff7fff0c301b361b001800278c13800c301b101b00180027be1380
       16edac78d60c189f132335543b42848ae4948cabff0017c8ff005e8863e74000
       ff008b4066734000ff00b36667774000ff00d9c2600080426b0240006d5d4000
       713840009c94b2a4595b595bbc32c878595b595bcf2ad75201d495e003a92bbf
       01d495e027010f2ff1ac99480000000000000000000000001a0f4820d1c8bf80
       2874fe4033db5880ebd1b4000718a50821760ac0c5d93040198af2403a26cfc0
       e4ff0300600000001a0f4820d1c8bf802874fe4033db5880ebd1b4005472d14e
       21760ac0c5d93040198af2403a26cfc0e4ff03000ec6768d0ec6768d00000000

## What Ozzy would need

A new engine path: isochronous PCM in and out, plain S24_3LE both ways (the isoc output
engine already writes plain S24_3LE), alt 2 on interfaces 1 and 2, a fixed frame count per
microframe (no feedback), MIDI as USB-MIDI packets on 0x83 / 0x04 (common/usb_midi.c), the
handshake above (with the 'M' and 'B' blocks sent as captured, without knowing their meaning),
and the rate on both endpoints.

Open: the US-322 (0644:8040); the rest of the 'M' block; what the 'A' 0x0D read-back means in
input mode.

## Second capture (d-hey, 2026-11-01, `captures/us366_dhey_2026-11-01/`, #91)

Output session (all Interface tab options, 44.1/48/88.2/96 kHz, video alongside: capture time =
video time - 22.6 s) and an input session (optical S/PDIF in, 6 inputs). Confirmed unless marked.

- **44.1 kHz:** packets alternate 5/6 frames, plus one extra 6 every 80 microframes (a 6,6,6 run):
  441 frames per 10 ms, 5.5125 per microframe. **88.2 kHz:** 11 frames, a 12 every 40
  microframes. IN carries the same pattern (99.9% of microframes match); the host computes OUT's
  own pattern, about 6 ms ahead. No traffic on feedback EP 0x85 at any rate. Ozzy's nominal
  accumulator gives the same averages, and the device takes any nominal distribution.
- **Rate requests** as at 48/96: SET_CUR (`22 01 0100`) to 0x81 then 0x02, three pairs per
  session with 'A' between the second and third (44100 `44ac00`, 88200 `885801`). S/PDIF
  register 0x10 rate code 0x00 / 0x02 / 0x08 / 0x0A at 44.1 / 48 / 88.2 / 96, as `hs_spdif_regs`.
  A rate change is SET_CONFIGURATION 0 then 1 and the handshake again without 'M'/'B'.
- **'B' is the same 256 bytes at every rate** (also sent at 96 kHz), not a 48 kHz block.
- **Clock source:** 'I' wIndex 0 bit 1: set = automatic (0x0A), clear = internal (0x08). The
  stream stops, one GET_CUR/SET_CUR pair, the stream restarts without SET_CONFIGURATION.
- **Digital Output Format:** 'A' 0x0D-0x11 written live: AES/EBU `41 00 2C 00 00` (professional,
  = Ozzy's table and the device's own read-back at every rate), S/PDIF `04 00 00 00 0B`.
- **Output routing: 'M' byte 56**, rewritten live, nothing else in the block changes. Inferred
  layout: bits 0-1 line out 1-2, bits 2-3 digital out, bits 4-5 line out 3-4; each 0 = mixer,
  1 = PCM 1-2, 2 = PCM 3-4, 3 = PCM 5-6. Seen: 0x24 → 0x25 → 0x29 → 0x39. The default 0x24
  (line 1-2 mixer, digital PCM 1-2, line 3-4 PCM 3-4) routes **PCM 5-6 nowhere**, which is why
  5-6 were silent in the first capture. 0x2D would give 1-2 / 3-4 / 5-6 straight (untested).
- **'I' wIndex 1 is the front-panel switch status**, not a mode the host writes (the panel never
  writes it): 0x0F = coax + digital out (4 in / 6 out); 0x03 = optical + digital in (**6 in /
  4 out**: PCM in 108 B = 6 frames x 18 B per microframe at 48 kHz, PCM out 72 B). Bits 0x04 and
  0x08 changed together (coax, output select: inferred, the switches moved at once). In input mode
  'M' words 4 and 13 are 0x5A80 and byte 56 is 0x34; signal came in on 1-2 and 5-6 (5-6 probably
  the S/PDIF input).
- **CCs on 0x83:** 0x68 every 512 ms carries the switch byte (0x0F / 0x03); 0x66/0x67 = 0x7F;
  0x6B/0x6C every 64 ms with 0-127 (meters, inferred).
- **Host only, nothing on the wire:** Audio Performance (transfers of 8/16/24/32 packets, the
  "low" one 16 packets with 2 OUT in flight) and the foot switch settings. Stream start: IN 0x81
  first, 9 IN / 3-4 OUT transfers in flight.

## What Ozzy does (2026-11-14, untested on hardware)

Family `PLOYTEC_US3XX` (`engine/devices/ploytec/ploytec_us3xx_engine.c`), row `TascamUS366`:
4 in / 6 out, 44.1, 48, 88.2 and 96 kHz (nominal 5/6 and 11/12 frames per microframe at 44.1 and
88.2, as on the wire), starting at 48 kHz as Windows did.

- Channels: the engine's probe (`us3xx_probe`, called by every platform after attach, before it
  builds its PCM devices) reads 'I' wIndex 1: digital out 4 in / 6 out (the row), digital in
  6 in / 4 out. A start that finds another mode than the probe did refuses (replug).
- 'M': `ploytec_us366_mixer_block`: digital out, Tascam's block with byte 56 from the three
  routing settings (`OZZY_QUIRK_US366_ROUTING`, default Tascam's 0x24); digital in, Tascam's
  digital-in block as captured. Sent at the first start and again whenever it changed (a routing
  setting restarts the engine); 'B' only at the first start.

- Interfaces 0-3 claimed, alternate settings 0, 2, 2, 0 (interface 0 first: Linux probes it).
- Handshake `ploytec_us3xx_handshake` (`common/devices/ploytec/ploytec_handshake.c`): 'V';
  'I' wIndex 1, 'I' wIndex 0 and the write of the selector with wIndex 1's bits 3-4 (0x02 → 0x0A);
  GET_CUR; SET_CUR pair; 'M' and 'B' at the first start after the device comes ('B' at every
  rate since the second capture showed it is the same block); SET_CUR pair; the five 'A' S/PDIF registers (consumer); SET_CUR pair. A start
  refuses a mixer mode whose channels aren't 4 in / 6 out (`ploytec_us366_channels`): to fix,
  the 6 in / 4 out mode is the front panel's digital-in switch (second capture).
- PCM out: `ozzy_isoc_output` with no feedback (nominal rate), 32-packet transfers, 3 in flight.
- PCM in: `ozzy_isoc_input` (new), 16-packet transfers of 512-byte packets, 4 in flight; frames
  copied to the HAL's plain ring (`OZZY_DEVICE_FLAG_PLAIN_INPUT`) or the attached ring; a failed
  packet leaves its nominal frames of silence. Input safety offset: one transfer plus 1 ms (a guess).
- MIDI: 2 bulk reads of 256 bytes on 0x83 (USB-MIDI packets into the MIDI in ring); MIDI out as
  USB-MIDI packets on 0x04, up to four (16 bytes) per transfer, one transfer in flight.

To find out on hardware: whether the device streams without 'M' / 'B' state from Windows, the
input's offset against the output, whether 'M' byte 56 = 0x2D gives six straight outputs.
