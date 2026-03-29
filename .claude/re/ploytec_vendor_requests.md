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

### 'A' (0x41) — ESU Clock Source Config (ESU/product 0x2573 only)

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

Sends 3 bytes, little-endian sample rate. Sent 5 times alternating between endpoints
0x0086 and 0x0005 (3x to 0x0086, 2x to 0x0005).

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
