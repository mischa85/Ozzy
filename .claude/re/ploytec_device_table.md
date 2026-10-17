# Ploytec Chipset — Device Table

All devices handled by the official Ploytec macOS driver (`PGDevice` / `PGKernelDeviceXONEDB4`),
extracted from Ghidra decompilation. The driver uses `this+0xa86` (USB vendor ID) and
`this+0xa88` (USB product ID) for device-specific branching.

The `setFreqOnOutPipe` function is the most comprehensive device check — it contains
every VID/PID the driver supports.

## Allen & Heath / Ploytec (VID 0x0A4A)

| USB PID | Device Name | PCM Format | Notes |
|---------|-------------|------------|-------|
| 0xFFDB  | Xone:DB4    | 24-bit Ploytec bulk | Confirmed, supported in Ozzy |
| 0xFFD2  | Xone:DB2    | 24-bit Ploytec bulk | Confirmed, supported in Ozzy. **48 kHz only** (official plist table). fw 1.2.0: EP 0x05 interrupt, wMaxPacketSize 512 → 512 framing. Boots at 44.1k. See `ploytec_sample_rates.md`, `ploytec_bulk_devices.md` |
| 0xFFDD  | Xone:DX     | 24-bit Ploytec bulk | Confirmed, supported in Ozzy. Special-cased in `updateAjInputSelector` |
| 0xFF4D  | Xone:4D     | 24-bit Ploytec bulk | Confirmed, supported in Ozzy |
| 0xFFAD  | Wizard 4    | 24-bit Ploytec, **16 in / 2 out** | In Ozzy as experimental (16-lane input, 2-lane output), untested on hardware |
| 0xFF01  | Xone:2D     | 24-bit Ploytec (same URB sizes as DB4, #77) | In Ozzy as experimental; 6 real analog inputs, 8-ch frames |
| 0x001E  | Xone:1D     | none (MIDI only) | Not a Ploytec audio device. USB-MIDI class descriptors (AC + MIDIStreaming, 1 in / 1 out jack), full speed, but both bulk endpoints (0x81, 0x01) have **wMaxPacketSize 4**, which is illegal for full-speed bulk (8/16/32/64). Linux snd-usb-audio binds it and MIDI in works (#27 aseqdump); MIDI out untested. macOS (10.15+) does not show it as a MIDI device. Apple's `AppleMIDIUSBDriver.plugin` (macOS 26.2, x86_64 disasm) has no wMaxPacketSize check in its descriptor parser or `FindPipes`; it bails with "Couldn't get pipe for endpoint" if the kernel won't give it a pipe, so the rejection is either elsewhere in the plugin or in IOUSBHost pipe creation (unverified, needs a `com.apple.coremidi` debug log from a real 1D). **Ozzy (macOS):** OzzyKext claims it (personality Xone1D, 2634/30) and runs `UsbMidiEngine`: interface 1 alt 0, 4-byte reads on 0x81 (one event packet per transfer, since a longer transfer would only complete when full), one 4-byte OUT transfer at a time on 0x01, packets converted by `common/usb_midi.c`. Untested on hardware; if the kernel refuses to create the pipes the kext logs "failed to open pipe". Official Mac driver 2.0.8 (2010, x86_64/i386/ppc) = generic Ploytec kext `PGKernelDeviceXONE1D` matching IOUSBDevice 2634/30 + CoreMIDI plugin; its `USBMidiDevice::read/write` are plain bulk transfers of wMaxPacketSize on those pipes (no vendor requests seen there). Captures in `captures/xone1d_issue27/` (replug capture ends right after SET_CONFIGURATION; the July capture is of another bus) |
| 0x1016  | ?           | ? | Has `setFreqOnOutPipe` support |
| 0x2416  | ?           | ? | Has `setFreqOnOutPipe` support |
| 0x5A16  | ?           | ? | Excluded from first `updateAjInputSelector` call in `configurationDone` |
| 0x6A02  | ?           | ? | Sets frequency to 44100 on digital lock. Has `setFreqOnOutPipe` support |
| 0xF1A0  | ?           | ? | Sets frequency to 44100 on digital lock. Has `setFreqOnOutPipe` support |
| 0xC150  | "CRIMSON_SPL" | ? | SPL Crimson — named in `_IOLog` string |
| 0xD064  | "DOTEC"     | ? | Dotec-Audio — named in `_IOLog` string. Variable channel count based on rate |

**No serial numbers.** Ploytec firmware has no per-unit serial: the DB2 and DB4 set iSerialNumber = 3 and string 3 is literally `no serial number` (`captures/db2_soulcheck_windows`, `captures/db2_wedigjames`, `captures/db4_linux_18d3e0a`, and the "S/N:" lines in old Ozzy logs); the Xone:1D has iSerialNumber = 0. Two units of the same model can only be told apart by USB port (`locationID` on macOS), so OzzyHAL/OzzyMIDI build device identity from VID:PID + locationID.

## VID 0x0644 (unknown vendor — TEAC?)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x8040  | ?           | Special index 0 write: hardcodes `0x12` instead of using `this[0xd2]`. Has `setFreqOnOutPipe` support |
| 0x8041  | ?           | Has `setFreqOnOutPipe` support |
| 0x8046  | ?           | Uses `this[0xd2] & 0x18` for index 0 bits 3-4. Has `setFreqOnOutPipe` support |
| 0x8048  | ?           | Uses `this[0xd2] & 0x18` for index 0 bits 3-4 |
| 0x8035  | ?           | Uses `choose6InputPostprocessor` (6-input device) |

All 0x644 devices use the read-modify-write with masks `0x18` / `0xE7` on index 0.
PIDs checked in `setFreqOnOutPipe` via switch on `(PID + 0x7FC0)`: cases 0 (0x8040), 1 (0x8041), 6 (0x8046).

## ESI Audiotechnik (VID 0x2573, ESU product line)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x0012  | ?           | Sends vendor request 'A' (0x41) in `configurationDone` |
| < 0x002B | ESU family | Sub-IDs below 0x2B with certain bit patterns → `updateAjInputSelector_Esu186()` |

ESI's ESU devices (VID 0x2573 is ESI Audiotechnik; "ESU" is its product line, e.g. ESU1808, which `Esu186` probably refers to) have a completely separate `updateAjInputSelector_Esu186` code path. 0x0A92 (EGO SYStems, ESI's parent) shares some of it (`setEsuCpldByte`).

10-lane output (DB2 kext decompile):
- `dmaEncode10` (10 host channels, Contour IE): the 8-lane frame plus a fifth lane in bit 4, odd
  channels 1..9 in bytes 0x00..0x17, even 2..10 in 0x18..0x2F, MSB first.
- `dmaEncode10_12` (ESU1808, 8 host channels; `_34/_56/_78` per the selected pair, AjExtData+0x10,
  default 0 = pair 1/2): lanes 1..8 as `dmaEncode08`, lane 10 = channel 1 of the frame, lane 9 =
  channel 2 of the **previous** frame (carried across calls in `param_1[2]`). Probably the S/PDIF
  serialiser takes lane 9 one frame late, keeping L/R aligned (inferred). On a 964-byte interrupt
  output `chooseDMAEncoder` replaces it with the plain `dmaEncode08_Int964` (no mirror).
  Ozzy: `ploytec_encode_frame_10ch`, `ploytec_spdif_mirror` (any pair; from the previous encoded frame,
  so no state on macOS; Linux keeps the last frame's even half per packet).
See [ESU-specific protocol](#esu-specific-protocol-updateajinputselector_esu186) below.

## VID 0x0499 (Yamaha?)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x1004  | ?           | `setFreqOnOutPipe` + retry loop (200x) in `setFrequency` |
| 0x100A  | ?           | Same retry behavior |
| 0x100B  | ?           | Same retry behavior |

PID range 0x1004–0x100B with bitmask 0xC1 (bits 0,6,7 → PIDs 0x1004, 0x100A, 0x100B).
The `setFrequency` function retries the SET_CUR request up to 200 times for these devices.

## VID 0x0582 (Roland?)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x0019  | ?           | `setFreqOnOutPipe` support only |

## VID 0x200C (Reloop)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x1009  | Digital Jockey 2 Master Edition | In Ozzy (experimental). Junction frame, 6 in / 4 out, 44.1 kHz only (Reloop's rldj2meu.sys hardcodes 0xAC44 for 200c:1009), bulk 512-byte output layout, same endpoints and handshake as the Xone mixers. Firmware ignores MIDI running status. Found by julled (PR #80) |
| 0x1005  | sibling model | Gets 96 kHz in rldj2meu.sys (PR #80) |
| 0x1007–0x1027 | ? | Range with bitmask in `setFreqOnOutPipe` |
| 0x1019  | ?           | Returns early from `updateAjInputSelector` with status 0x12 |
| 0x1030  | ?           | Uses SET_INTERFACE (bRequest 0x0B) in `configurationDone` |

## Creative Technology (VID 0x041E)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x3020  | ?           | Has special alternate setting logic; sends SET_CUR to endpoint 0x1200 |
| 0x3040  | ?           | `setFreqOnOutPipe` support |

## VID 0x0451 (Texas Instruments?)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x3200  | ?           | `setFreqOnOutPipe` support only |

## VID 0x0763 (M-Audio?)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x2003  | ?           | Uses `pcmTo24TUSBMSB` encoder (24-bit TUSB MSB format) |

## VID 0x0A4E (unknown)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x2040  | ?           | Uses `pcmTo24Access` encoder (1ch) |
| 0x4040  | ?           | Uses `pcmTo24Access` (1ch) or special path. `setFreqOnOutPipe` support |

## VID 0x0A92 (unknown)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| range from 0x21 | ? | Complex bitmask check in `setFreqOnOutPipe` |

## VID 0x0D8C (C-Media?)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x000C  | ?           | `setFreqOnOutPipe` support only |

## VID 0x1235 (Focusrite?)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x0001  | ?           | `setFreqOnOutPipe` support |
| 0x0002  | ?           | `setFreqOnOutPipe` support |

## VID 0x133E (unknown)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x0815  | ?           | `setFreqOnOutPipe` support |
| 0x0816  | ?           | `setFreqOnOutPipe` support |
| 0x6000  | ?           | Uses `pcmTo16LSBChannelSwap` (16-bit, 2ch) or `pcmTo24LSBChannelSwap` (24-bit, 2ch) |

## VID 0x145F (Trust?)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x0090  | ?           | `setFreqOnOutPipe` support only |

## VID 0x1502 (unknown)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x2AAE  | ?           | `setFreqOnOutPipe` support only |

## VID 0x1654 (unknown)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x0104  | ?           | `setFreqOnOutPipe` support. Needs 50ms delays during pipe init in `configureDevice` |

## Elektron "Overbridge2" (VID 0x1935)

| USB PID | Device Name | PCM Format | Notes |
|---------|-------------|------------|-------|
| < 0x000C | Elektron family | 32-bit | Bitmask 0x950 → `setElektronChannelMap`. Fixed 48kHz. Regular MIDI |
| 0x000A–0x0017 | Overbridge2 family | 32-bit MSB | Interrupt transfers for both in+out. Vendor reqs 0x11/0x12 for channel config |
| 0x0016  | ?           | 32-bit MSB | Specific `pcmTo32Msb` override. Overbridge2 |

- Overbridge2 devices use **interrupt transfers** (not bulk or isoc) for both input and output
- Overbridge2 devices read channel config via vendor requests 0x11 and 0x12 (wIndex=1 for in, 2 for out)
- Log: `"Ovb2 curChanConfig_In:%d curChanConfig_Out:%d curChanConfigAlt_In:%d curChanConfigAlt_Out:%d"`
- Certain Elektron PIDs are locked to 48000 Hz in `setFrequency`
- Some Elektron PIDs use junction-compatible MIDI interface (`isJunctionCompatibleWithRegularMidi`)

## DigiDesign / Avid (VID 0x0DBA)

| USB PID | Device Name | PCM Format | Notes |
|---------|-------------|------------|-------|
| 0x1000  | ?           | 24-bit TUSB 2ch | `(PID \| 0x2000) == 0x3000` check |
| 0x3000  | Mbox 2?     | 24-bit TUSB 2ch | Referenced via `initDigiDesignMbox2` in `setClockSource` |

## Akai Professional (VID 0x09E8)

| USB PID | Device Name | Notes |
|---------|-------------|-------|
| 0x0010  | EIE Pro (#73) | In `isPloytecBulkInIsocOutDevice`: **bulk in, isoc out** (not a bulk/interrupt-out device like the Xones). In Ozzy as experimental on Linux (`ploytec_isoc_ops`), untested on hardware. See `akai_eie_pro.md` |
| 0x207F  | "ACV0"      | Named in `_IOLog`. In `isPloytecBulkInIsocOutDevice`; the bcdDevice 0x300 variant uses isoc in + isoc out (no bulk) |

### Bulk in / isoc out devices (`PGDevice::isPloytecBulkInIsocOutDevice` @ 0xc2a6)

0562:03EB, 0644:(PID-0x800F bitmask 0x6200428101), 0763:4002, 09E8:0010, 09E8:207F,
0A4A:0014, 0A4A:5B01, 1210:0018-001A, 13B2:0030. `PGKernelDeviceXONEDB2` pipe setup
(~line 22720 of the decompile): PCM in from interface 1's bulk IN pipe (`+0xa90`, or its
interrupt IN pipe, then "Use interrupt transfers"), plus interface 1's isoc IN pipe as
`m_pcInPipeFeedback` (`+0xab8`). The out pipe isn't in this snippet: isoc out with explicit
feedback is **inferred** from the function name and the feedback pipe (cf. `isPloytecBulkDevice`,
whose name turned out misleading, `ploytec_bulk_devices.md`).
`bulkInpEvent` ignores bulk IN errors for these devices on Darwin > 14. If the out pipe is
isoc, Ozzy has no path for it, and none of these is just a table entry away.

---

## ESU-specific Protocol (updateAjInputSelector_Esu186)

ESU devices (VID 0x2573, PID < 0x2B) have their own init path with these key differences:

### Clock source → composite state mapping

| AjExtData[1] value | Composite state | Vendor 'A' param | Meaning |
|---------------------|----------------|-------------------|---------|
| 2                   | 0x32 (STATE_A) | 0 (wValue=0x348)  | ? |
| 1                   | 0xB0 (STATE_B) | 1 (wValue=0x349)  | ? |
| other (default)     | 0xB2 (STATE_C) | 0 (wValue=0x348)  | ? |

### Read-modify-write sequence
1. Read index 0 status byte
2. If digital lock bit (0x04) set → sleep 20ms, re-read
3. Preserve bits with mask `0x4C` (`byte & 0x4C`)
4. OR in composite state based on clock source
5. Call `sendEsuARequest` with param 0 or 1
6. If byte changed: write back with `(short)(char)byte` (sign extension), sleep **200ms**
7. Read sample rate (GET_CUR), re-read index 0

### sendEsuARequest detail
```
bRequest:  0x41 ('A')
wValue:    0x348 | (param & 0x07)  → 0x348 (param=0) or 0x349 (param=1)
wIndex:    0x101
wLength:   0
```
Log: `"WRITE ESU A-Request value: %d"` with `param & 7`.

---

## PCM Codec Selection (from configureDevice + chooseISOOutEncoder)

### Device data block layout (at `this+0x3028` pointer)

```
+0x1430: input channel count (iInCh)
+0x1434: input resolution in bits (iInRes)
+0x1438: output channel count (iOutCh)
+0x143c: output resolution in bits (iOutRes)
+0x1440: configuration descriptor index
+0x1444: AjExtData (PtAjExtData)
+0x1464: US-144 config field 1
+0x1468: US-144 config field 2
```

Log: `"configureDevice iInRes=%d, iOutRes=%d, iInCh=%d, iOutCh=%d"`

### Format ID calculation

`format_id = (resolution - 16) / 4` — so 16→0, 20→1, 24→2, 32→4.
Input and output can have different resolutions.

### Output encoders (host → USB)

| Format ID | Bit depth | Encoder functions |
|-----------|-----------|-------------------|
| 0 (16-bit) | 16 | `pcmTo16LSB`, `pcmTo16LSBChannelSwap` (0x133E/0x6000 2ch) |
| 1 (20-bit) | 20 | `pcmTo20LSB` |
| 2 (24-bit) | 24 | `pcmTo24LSB` (default), `pcmTo24Junction4CH`..`10CH` (Ploytec bulk), `pcmTo24TUSBMSB` (0x763), `pcmTo24TUSB2ch` (0xDBA), `pcmTo24Access` (0xA4E), `pcmTo24LSBChannelSwap` (0x133E/0x6000 2ch), `pcmTo24LSB_US144_*` variants |
| 4 (32-bit) | 32 | `pcmTo32LSB`, `pcmTo32Msb` (Elektron Overbridge2) |

### Input decoders (USB → host)

| Format ID | Bit depth | Decoder functions |
|-----------|-----------|-------------------|
| 0 (16-bit) | 16 | `pcmFrom16LSB`, `pcmFrom16LSBChannelSwap` (0x133E/0x6000 2ch), `pcmFrom16LSBChannelSwapMI_4` (0xA4E/0x4040 4ch) |
| 1 (20-bit) | 20 | `pcmFrom20LSB` |
| 2 (24-bit) | 24 | `pcmFrom24LSB` (default), **NULL for Ploytec bulk** (handled separately), `pcmFrom24TUSBMSB` (0x763), `pcmFrom24TUSB2ch` (0xDBA), `pcmFrom24Access` (0xA4E/0x133E 1ch), `pcmFrom24LSBChannelSwap` (0x133E/0x6000 2ch), `pcmFrom24LSBChannelSwapMI_4` (0xA4E/0x4040 4ch) |
| 4 (32-bit) | 32 | `pcmFrom32LSB`, `pcmFrom32Msb` (Elektron Overbridge2) |

Note: Ploytec bulk devices return NULL for the input decoder in `configureDevice` —
input decoding is handled through a separate path (likely the junction decoder).

### Ploytec bulk 24-bit junction encoders
Selected via `isPloytecBulkDevice()` check + output channel count:
```
channels 4  → pcmTo24Junction4CH
channels 5  → pcmTo24Junction5CH (inferred from array)
...
channels 10 → pcmTo24Junction10CH (inferred)
other       → pcmTo24LSB (fallback)
```

### Ploytec junction DMA codecs (`chooseDMADecoder` / `chooseDMAEncoder`)
This is the "separate path" for Ploytec bulk/interrupt devices, and the frame format Ozzy implements.
From the XONE_DB2 3.4.9 kext (Ghidra, `DB2_decompile/XONE_DB2_kext_all.c`); the WIZARD_4 3.3.11 kext
has the same function set.

**Input** (`this+0x55b8`), picked by the input channel count (settings `+4`):
- 2..16 channels → `dmaDecode16To02..16At32Bit`, requires `this+0x55b0 == 0x10` (16 lanes)
- 18..32 channels → `dmaDecode32To18..32At32Bit`, requires `this+0x55b0 == 0x20` (32 lanes, frame not decoded yet)
- PID 0xD064 (Dotec) has its own `_DOTEC` variants

The 16-lane input frame is **64 bytes** for any channel count up to 16 (8 frames per call, returns
0x40 bytes per 8 ch / 0x80 per 16 ch of output):
```
bytes 0x00..0x17  bit b (0..7) = sample bit (23 - byte) of channel 2b+1   (odd channels)
bytes 0x20..0x37  bit b (0..7) = sample bit (23 - byte) of channel 2b+2   (even channels)
bytes 0x18..0x1F, 0x38..0x3F unused
```
`dmaDecode16To08` uses bits 0..3 (= Ozzy's 8-channel decoder), `dmaDecode16To16` uses bits 0..7.

**Output** (`this+0x55c0`), picked by the output channel count (settings `+0xc`): `dmaEncode02`,
`04`, `06` (`dmaEncode06_Int964` when the OUT endpoint has wMaxPacketSize 0x3C4 = 964), `08`, …
All write the same **48-byte frame**, 10 frames per 480-byte block:
```
bytes 0x00..0x17  bit b (0..3) = sample bit (31 - byte) of channel 2b+1
bytes 0x18..0x2F  bit b (0..3) = sample bit (31 - byte) of channel 2b+2
```
`dmaEncode08` uses bits 0..3 (= Ozzy's encoder), `dmaEncode02` only bit 0 (channels 1/2), so a
2-channel device is the 8-channel frame with channels 3..8 silent.

Consequence: input lanes (up to 16) and output lanes (up to 8) share one frame format; a device
differs only in how many lanes carry audio. The MixWizard WZ4 (16 in / 2 out) uses
`dmaDecode16To16` + `dmaEncode02`.

### US-144 special variants (24-bit)
For devices with `this[0x1888]` and `this[0x7a9]` set, based on offsets `+0x1464` and `+0x1468`:
- `pcmTo24LSB_US144_00`, `pcmTo24LSB_US144_10`, `pcmTo24LSB_US144_11`

---

## Transfer Types (from configureDevice)

The driver selects between isoc, bulk, and interrupt transfers per device:

| Device type | Input | Output | Flag |
|-------------|-------|--------|------|
| Ploytec bulk (`isPloytecBulkDevice`) | Bulk (fallback: interrupt) | Bulk (fallback: interrupt) | `this[0xb53]` for interrupt |
| JCT30 compatible | Interrupt | Interrupt | `this[0xb53]` = 1 |
| Elektron Overbridge2 | Interrupt | Interrupt | `this[0xb53]` = 1 |
| `isPloytecBulkInIsocOutDevice` | Bulk/interrupt + isoc feedback | Isoc | Mixed |
| Default | Isoc | Isoc | — |

Ploytec bulk devices also have a "keep-alive" isoc out pipe (`m_pcOutPipeIsocKeepAlive`)
on endpoint range 1–4.

### Pipe name → offset mapping
```
m_pcInPipeIsoc            → this+0xab0
m_pcInPipeFeedback        → this+0xab8
m_pcOutPipeIsoc           → this+0xac0
m_pcOutPipeIsocKeepAlive  → this+0xac8
m_pcBulkInPipe            → this+0xa90
m_pcBulkOutPipe           → this+0xa98
m_pcInterruptInPipe       → this+0xaa0
m_pcInterruptOutPipe      → this+0xaa8
```

For Ploytec bulk: endpoint 0x05 is found via `getPipeByAddress('\x05')`. If there is no
EP 0x05, or its **bInterval** is 4 (`pipe+0x16`; `PGPipeDescriptor::scan` @0x209b2 stores
bInterval there, the transfer type is `bmAttributes & 3` at `+0xe`), it falls back to
`getInterruptOutPipe()`. An earlier version of this file read `+0x16 == 4` as a type check.

---

## Output Mixer (from chooseOutMixer)

Based on `this+0x14` (output channel count):
- 2 → `outMixer2CH`
- 4 → `outMixer4CH`
- 6 → `outMixer6CH`

---

## Init Flow (from reqConfigureDevice → configureDevice → configurationDone)

### reqConfigureDevice (outer wrapper)
1. Check firmware update status (`+0x188c == 2` required)
2. `unconfigureDevice` — tear down previous config
3. `configureDevice` — full configuration (see below)
4. `sendSetPicCommand(device, 0x81)` — **unknown vendor request, investigate in Ghidra**
5. Sleep 10ms
6. `registerMidiInterface`
7. Set `+0x1424` = 1 (configuration complete flag)

### configureDevice (USB setup)
1. Elektron Overbridge2: read channel config via vendor reqs 0x11/0x12
2. `findInterfacesInConfig` — find matching USB interfaces
3. `SetConfiguration` (with retry + reset on failure, up to 5 attempts)
4. For A&H 0x6A02/0xF1A0: `SET_INTERFACE` to interface 2
5. `matchInterface` for control, output, input, HID, MIDI interfaces
6. Find and assign pipes (isoc/bulk/interrupt) based on device type
7. `initPipe` for all assigned pipes + bandwidth check
8. Select PCM converters (input decoder + output encoder) based on resolution + device
9. `configurationDone` (see below)

### configurationDone (device init)
1. `updateAjInputSelector` (first call, conditional)
2. `chooseInputPostprocessor` (or `choose6InputPostprocessor` for 0x644/0x8035)
3. `chooseISOOutEncoder`
4. `updateAjInputSelector` (second call)
5. `chooseOutMixer` (if output channel count > 0)
6. `writeDigitalOutSelector`
7. Initialize isoc/bulk/interrupt data structures
8. `USBMidiDevice::configurationDone`
9. `AJ::configurationDone`
10. `setEsuCpldByte`
11. `setElektronChannelMap` (Elektron only)
12. `usbInitDeviceControls` + `usbApplyDeviceControls`
13. `startStreaming` (if pending)

### Device data block layout (at pointer `this+0x3028`)
```
+0x001C: device name string 1
+0x021C: device name string 2
+0x1424: configuration complete flag (1 = configured)
+0x1428: ? (cleared on reconfig)
+0x142c: frame rate (Hz)
+0x1430: input channel count
+0x1434: input resolution (bits: 16, 20, 24, or 32)
+0x1438: output channel count
+0x143c: output resolution (bits)
+0x1440: configuration descriptor index (set to 0 on reconfig)
+0x1444: AjExtData (PtAjExtData) — see vendor_request_I.md
+0x1464: US-144 config field 1
+0x1468: US-144 config field 2
+0x1470: Elektron channel map
+0x1490: Elektron input channel map (8 x uint32)
+0x14B0: Elektron output channel map (8 x uint32)
+0x1A74: XsDeviceControls (0xF04 bytes) — current device controls/selectors
+0x1A7E: output selector byte (within XsDeviceControls)
+0x2978: XsDeviceControls backup copy (0xF04 bytes)
+0x387C: exclusive client 1 (IOUserClient pointer)
+0x3884: exclusive client 2
+0x388C: exclusive client 3
+0x3894: TimeStampsBufferData
```

### Driver version

`v3.4.9` (encoded as 0x3040900 in dispatch case 0x16, string in case 0x3F)

---

## Index 2 — Digital Output Selector (from writeDigitalOutSelector)

```
bRequest:  0x49 ('I')
wValue:    AjExtData[4] & 0xFFFF
wIndex:    2
wLength:   0
direction: OUT (0x40)
```

- Only written if `this[0x7ac]` is set (device has digital output capability)
- No read-modify-write — just writes the value directly
- `AjExtData[4]` holds the digital output selection

---

## PGDevice Class Layout (updated)

```
this+0x08:   device-specific data (0x14 bytes, cleared by initDeviceSpecificData)
             ESU/0x0A92: this+0x08 = 0x0101
             TEAC 0x8041/0x8048: this+0x18 = 100ms (post-config sleep)
             TEAC 0x8040/0x8046/0x8041/0x8048: this+0x10 = 2, this+0x14 = 2
this+0x0C:   some capability flag (checked in onServiceStart)
this+0x10:   input channel count override
this+0x14:   output channel count override (2, 4, or 6)
this+0x18:   post-config sleep duration in ms (clamped to 1000)
this+0x1E:   Wolfson codec registers array (0xB0 bytes, dispatch 0x29/0x2A)
this+0xCE:   3 x short — some config values (dispatch 0x30)
this+0xD2:   mixer/channel config byte (index 1 value, bits 3-4 reused for index 0 writes)
this+0x190:  ? (dispatch 0x48, 0x80 byte debug buffer)
this+0x218:  mixer mode (0=bypass, 1=active, 2=active variant)
this+0x21C:  mixer knob value (sent as wValue to vendor request 'T')
this+0x21E:  mixer bitmap value (sent as wIndex to vendor request 'T')
this+0x7A0:  capability flag
this+0x7A1:  setFreqOnOutPipe capability flag
this+0x7A8:  has USB1 mode capability
this+0x7A9:  US-144 flag (selects special PCM encoder variants)
this+0x7AC:  has digital output selector (enables writeDigitalOutSelector)
this+0x7D7:  device present flag
this+0x7E4:  Elektron UID (8 bytes, read via vendor req 0x00)
this+0x7EC:  Elektron device name (32 bytes, read via vendor req 0x01)
this+0x798:  ?
this+0x80C:  config changed flag (set on mixer mode change)
this+0x80D:  streaming active flag
this+0x860:  hog ID
this+0xA80:  PT_DeviceDescriptor (parsed USB device descriptor):
  +0xA80: bcdUSB (2 bytes)
  +0xA82: bDeviceClass (1)
  +0xA83: bDeviceSubClass (1)
  +0xA84: bDeviceProtocol (1)
  +0xA85: bMaxPacketSize0 (1)
  +0xA86: idVendor (2) — USB vendor ID
  +0xA88: idProduct (2) — USB product ID
  +0xA8A: bcdDevice (2) — firmware version
  +0xA8C: iManufacturer (1)
  +0xA8D: iProduct (1)
  +0xA8E: iSerialNumber (1)
  +0xA8F: bNumConfigurations (1)
this+0xB53:  interrupt transfer mode flag
this+0xB54:  pending start streaming flag
this+0x1880: PGKernelDeviceXONEDB4 pointer
this+0x1888: USB High Speed flag (set by setIsUSB2Device, NOT a bulk flag)
this+0x188C: firmware update status (must be 2 for normal operation)
this+0x1890: ? (dispatch 0x17)
this+0x1898: 16-byte string (dispatch 0x18)
this+0x3028: pointer to device data block (see layout above)
this+0x3F48: AJ subsystem
this+0x55D0: audio buffer pointer (0x6400 bytes)
this+0x55D8: ESU CPLD byte
this+0x71A8: TimeStampsBuffer pointer
```

### Wolfson Codec Registers (dispatch 0x29/0x2A)

Some devices have Wolfson audio DAC/ADC chips programmable via USB.
The `writeWolfsonRegister(this, chipIndex, registerIndex, value)` function writes
to the codec. Register values are stored at `this+0x1E` (0xB0 bytes total)
and published via IORegistry as "WolfsonRegisters".

---

## Ozzy Support Status

Currently supported in Ozzy (all VID 0x0A4A):
- Xone:DB4 (0xFFDB)
- Xone:DB2 (0xFFD2)
- Xone:DX (0xFFDD)
- Xone:4D (0xFF4D)

Potential future support (same vendor, Ploytec chipset):
- MixWizard WZ4 USB option card (0xFFAD, A&H part 004-232JIT): **16 channels to the computer,
  2 back** (stereo return), 44.1/48/88.2/96 kHz, 24 bit (A&H application note AP8987 issue 2, in
  the investigation folder under `WZ4/`). Same junction frame format as the Xone mixers, with 16
  input lanes and 2 output lanes (`dmaDecode16To16` + `dmaEncode02`, see "Ploytec junction DMA
  codecs"). Supported as experimental since 2026-10-11; before that it was in the Linux table as
  8×8, which could only have produced garbage.
- Xone:2D (0xFF01) — found in legacy driver, may work with existing code
- SPL Crimson (0xC150) — different channel topology
- Dotec-Audio (0xD064) — variable channel count based on sample rate

Non-A&H devices would need more RE work to understand their specific init sequences
and channel topologies, but they share the same Ploytec USB protocol core.
