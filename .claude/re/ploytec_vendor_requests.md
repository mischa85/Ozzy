# Ploytec Vendor Requests — Overview

All known USB control requests used by the Ploytec chipset, gathered from
USB traffic capture and Ghidra RE of the official macOS driver (`PGDevice`/`PGKernelDeviceXONEDB4`).

## Vendor Requests (bmRequestType 0xC0 read / 0x40 write)

### 'I' (0x49) — Hardware Control Registers

Detailed in [ploytec_vendor_request_I.md](ploytec_vendor_request_I.md).

Read/write hardware control registers selected by wIndex:
- wIndex 0: AJ input selector / digital status / clock source / USB1 mode
- wIndex 1: US3XX / mixer / CPLD config
- wIndex 2: digital output selector (write only)

Reads return 1 byte in data phase. Writes encode the value in wValue (no data phase).
Write wValue is sign-extended from byte via `(short)(char)byte` in the official driver.

### 'V' (0x56) — Firmware Version

```
bmRequestType: 0xC0 (read)
bRequest:      0x56 ('V')
wValue:        0x0000
wIndex:        0x0000
wLength:       15 (0x0F)
```

Returns 15-byte firmware version. Known fields:
- byte[0]: chip ID
- byte[2]: version (decimal encoded: v1.{byte/10}.{byte%10})

### 'A' (0x41) — ESU Clock Source Config (ESI, VID 0x2573, ESU product line only)

Called via `sendEsuARequest(device, param)` during ESU's `updateAjInputSelector_Esu186`.
Also called from `configurationDone` for product 0x2573/0x12.

```
bmRequestType: 0x40 (write)
bRequest:      0x41 ('A')
wValue:        0x348 | (param & 0x07)
wIndex:        0x0101
wLength:       0
```

The param is 0 or 1, derived from clock source setting:
- Clock source = 1 (STATE_B/0xB0): param = 1, wValue = 0x349
- Clock source = 2 (STATE_A/0x32) or default (STATE_C/0xB2): param = 0, wValue = 0x348

Log: `"WRITE ESU A-Request value: %d"` with `param & 7`.
Appears to configure the ESU's clock source at the hardware level, complementing
the composite state written to index 0 of vendor request 'I'.

Order in `PGDevice::configurationDone` (all before `startStreaming` sets the rate):
`updateAjInputSelector` (ESI: `_Esu186`, guarded by plist flags), then 'A' 0x349 for 2573:0012
only, then `updateAjInputSelector(NULL)` again (ESI: another 'A', 0x348 for STATE_A, which
overrides the 0x349), ..., then `setEsuCpldByte`. Ozzy (`hs_esi_setup` in
`common/devices/ploytec/ploytec_handshake.c`, `OZZY_QUIRK_ESI` / `OZZY_QUIRK_ESI_U168`) sends
'A' 0x349 (U168 XT), the ESI selector with STATE_A and 'A' 0x348, then the CPLD byte 0xE7, all
before SET_CUR, and skips the Xone status confirm. Untested on hardware.

### 'A' (0x41) — S/PDIF output registers (writeDigitalOutMode, wIndex 0x0101)

Plist flag 0x7a6 (Tascams but the US-200, MPC Renaissance) or Tascam 0x8041/0x8048: read 5 bytes
(0xC0, wValue 0x0D00), then write registers 0x0D..0x11 that differ (wValue `reg << 8 | val`).
Mode 0 (default): 04, 00 (0x0E written only if non-zero), 00, `(old & 0xF0) | rate code`
(32k 3, 44.1k 0, 48k 2, 88.2k 8, 96k 0xA, 176.4k 0xC, 192k 0xE), 0B.
Mode 1: 0x0D 0xC1/0x41/0x81 at 32k/44.1k/48k, 0x01 at 88.2k and up (other rates: unchanged),
0x0E 0, 0x0F 0x2C, 0x10 0, 0x11 0x50/0x10/0x58/0x18 at 88.2k/96k/176.4k/192k, else 0. Mode 1 sets
IEC 60958 byte 0 bit 0 (professional). Runs in `startStreaming` after SET_CUR, and from
`setDigitalOutMode` with streaming suspended. Ozzy: `hs_spdif_regs` / `hs_spdif_out`,
`OZZY_QUIRK_SPDIF_OUT`, setting `OZZY_SETTING_SPDIF_OUT_MODE`.

### 'A' (0x41) — Wolfson Codec Register Write (devices with Wolfson DAC/ADC)

From `writeWolfsonRegister(this, chipIndex, registerIndex, value)`:

```
bmRequestType: 0x40 (write)
bRequest:      0x41 ('A')
wValue:        value + registerIndex * 0x200
wIndex:        0x102 (codec 0) or 0x106 (codec 1)
wLength:       0
```

**Note:** Same bRequest as ESU clock config, but different wIndex values distinguish them:
- wIndex=0x0101 → ESU clock source config
- wIndex=0x0102/0x0106 → Wolfson codec register write

The one known user is the Musonik PHASE X64 USB (0A4A:5B01, two WM8776). Its Windows 2.9.34
driver (`WM8776SetDefault`, in configurationDone) writes the registry values, or else these
defaults: codec 0 R7=0x090, R21=0x001, R22=0x001; codec 1 R7=0x090, R21=0x004, R22=0x001. The
Mac kext 2.0.4 writes them only from its control panel. R21 is the ADC input mux (bit 0 Mic/Line/
HiZ, 1 Phono, 2 Aux, 3 sound module, 4 loopback of the other output pair; 0xC0 = muted), R22
the output mux (bit 0 playback, 1/2 direct monitoring), R7 0x090 stereo / 0x0F0 mono playback.
Ozzy writes the defaults (`OZZY_QUIRK_WM8776`). Details: `OEM_DRIVERS/Musonik_PhaseX64/findings.md`.

### 'B' (0x42) — Reverb Buffer Write (VID 0x0644)

From `startStreaming`, for 0x644 devices with `this[0x7DC]` set:

```
bmRequestType: 0x40 (write)
bRequest:      0x42 ('B')
wValue:        0
wIndex:        0
wLength:       0x100 (256 bytes)
Data:          reverb buffer from this + rateIndex * 256 + 0x220
```

Rate index: 44100→0, 48000→1, 88200→2, 96000→3, 176400→4, 192000→5.
Writes a sample-rate-specific reverb impulse buffer.

### 'M' (0x4D) — Routing Table Write (VID 0x0644)

From `readUS3XXChannelConfig`, called at the end for all VID 0x644 devices:

```
bmRequestType: 0x40 (write)
bRequest:      0x4D ('M')
wValue:        0
wIndex:        0
wLength:       0x40 (64 bytes)
Data:          routing table from this+0x620
```

### 'T' (0x54) — Mixer Values (VID 0x0644 devices at least)

Called from `Req_SetMixerValues` dispatch (case 0x28):

```
bmRequestType: 0x40 (write)
bRequest:      0x54 ('T')
wValue:        mixer knob value (from this+0x21c)
wIndex:        mixer bitmap value (from this+0x21e)
wLength:       0
```

Write-only, no data phase. Sets mixer routing/volume on hardware.
Log: `"Req_SetMixerValues bitmap %02X knob %02X"`

### 0x00 — Elektron UID Read (Elektron only)

From `scanDeviceDescriptor`, for all Elektron devices (VID 0x1935):

```
bmRequestType: 0x80 (read, vendor)
bRequest:      0x00
wValue:        0
wIndex:        0
wLength:       8
```

Returns 8-byte unique device ID. Stored at `this+0x7E4`, published as "ElektronUID".

### 0x01 — Elektron Device Name Read (Elektron only)

From `scanDeviceDescriptor`:

```
bmRequestType: 0x80 (read, vendor)
bRequest:      0x01
wValue:        0
wIndex:        0
wLength:       0x20 (32 bytes)
```

Returns null-terminated device name string. Stored at `this+0x7EC`, published as "ElektronDeviceName".
Also used for writing (bmRequestType 0x40) in `setElektronDeviceName`.

### 0x11 — Elektron Channel Config Read (Elektron/Overbridge2 only)

Seen in `configureDevice` for Elektron Overbridge2 devices (VID 0x1935, specific PIDs):

```
bmRequestType: 0x80 (read, via deviceRequestStd direction=0x80, type=0x40)
bRequest:      0x11
wValue:        0x0000
wIndex:        1 (input) or 2 (output)
wLength:       1
```

Returns 1 byte: current channel configuration for input (wIndex=1) or output (wIndex=2).
Log: `"Ovb2 curChanConfig_In:%d curChanConfig_Out:%d curChanConfigAlt_In:%d curChanConfigAlt_Out:%d"`

### 0x12 — Elektron Alternate Channel Config Read (Elektron/Overbridge2 only)

```
bmRequestType: 0x80 (read)
bRequest:      0x12
wValue:        0x0000
wIndex:        1 (input) or 2 (output)
wLength:       1
```

Returns 1 byte: alternate channel configuration. Only read if the corresponding 0x11
request returned a valid (non-0xFF) value.

## USB Audio Class Requests (standard)

### GET_CUR — Read Sample Rate

```
bmRequestType: 0xA2 (class, interface-to-host)
bRequest:      0x81 (GET_CUR)
wValue:        0x0100
wIndex:        0x0000
wLength:       3
```

Returns 3 bytes, little-endian sample rate in Hz.

Also seen with bmRequestType 0x80/0x20 in `updateAjInputSelector` to read digital lock frequency
after status write-back.

### SET_CUR — Set Sample Rate

```
bmRequestType: 0x22 (class, host-to-interface)
bRequest:      0x01 (SET_CUR)
wValue:        0x0100
wIndex:        0x0086 / 0x0005 (endpoint addresses)
wLength:       3
```

Sends 3 bytes, little-endian sample rate. How many times it is sent differs per source:
- DB4 Windows capture: 5× alternating (3× 0x0086, 2× 0x0005)
- DB2 Windows capture (PR #70): 7× alternating 0x86,0x05,0x86,0x05,0x86,0x05,0x86, ~27 ms apart,
  then GET_CUR with wIndex=0x0086 to read it back
- Official macOS driver: 5× (86,05 in startStreaming; 86,05 in resumeStreaming; 86 + GET_CUR 86 +
  50 ms sleep in bulkAudioRun). Each `setFrequency` call is one request; see `ploytec_sample_rates.md` §5
- Ozzy currently sends 2 (commit bbfa66d, based on the incorrect "once per pipe" reading). Issue #72
  reports the DB2 only works with the 7-call sequence, but that was tested together with the framing
  fix, so it's unproven whether the count matters.

Neither driver ever sends GET_MIN/GET_MAX/GET_RES. The device is never asked which rates it
supports. See `ploytec_sample_rates.md`.

## Standard USB Requests

### SET_INTERFACE (0x0B)

Seen in `configurationDone` for product 0x200c/0x1030:

```
bmRequestType: 0x00
bRequest:      0x0B
wValue:        0
wIndex:        interface number
```

Standard USB SET_INTERFACE to select alternate setting.
