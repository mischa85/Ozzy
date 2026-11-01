# Tascam US-366 (0644:8041): descriptors and the official Windows driver on the wire

Source: #91, from d-hey (2026-10-28): `lsusb`-style descriptors (USBView) and a USBPcap capture
of plugging in and playing, US-366 firmware 1.03, Tascam driver 1.04. Saved in the
investigation folder's `logs/github_issue_91/` (`x/us366.pcapng`, `x/tascam_US366_descriptors.txt`);
parser `tools/usbpcap.py`. Two sessions in the capture: attach at 44.1 kHz → 48 kHz, and after
a replug at 29 s, 48 → 96 kHz. **Nothing of this runs in Ozzy yet.**

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
| 0.243 | GET_CUR (0xA2 0x81) → 44100; 'I' read → 0x0A; GET_CUR twice more | |
| 0.243-0.554 | SET_CUR 48000 to EP 0x81, then 0x02, three times, ~55 ms apart | |
| 0.352 | **'M' (0x4D) write 64 B**: 32 zero bytes, then `7FFF 7FFF 4000 4000 3E8B 3E8B 0000...0024` | mixer? first session only |
| 0.353 | **'B' (0x42) write 256 B**: starts `7FFF 7FFF 7FFF 7FFF 0C30 1B36...` | DSP coefficients? first session only |
| 0.498 | 'A' (0x41) wIndex 0x0101: read 5 from 0x0D00 → `81 00 2C 00 00`; writes 0x0D04, 0x0E00, 0x0F00, 0x1002, 0x110B | the S/PDIF transmitter's channel status bytes 0-4 (Ozzy's `hs_spdif_out`): consumer, 48 kHz (0x02; 0x0A at 96 kHz), 24 bit |

The 96 kHz session: 'V', SET_CONFIGURATION, 'I' reads (0x0F, 0x0A, no write), GET_CUR →
48000, SET_CUR 96000 ×3 each, the same 'A' writes with 0x100A. No 'M' / 'B'.

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

Open: 44.1 kHz (whether packets then alternate 5/6 frames and how the device keeps time without
feedback), the mixer modes (input count 2/4/6, what 'I' wIndex 1 and 'M'/'B' change), what the
CCs on 0x83 report, the US-322 (0644:8040).
