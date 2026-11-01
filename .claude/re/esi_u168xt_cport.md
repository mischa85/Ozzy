# ESI U168 XT: control panel settings and the C-port

Source: ESI's U168 XT Mac driver 1.4 (`OEM_DRIVERS/ESI_U168XT/extracted/U168_XT-Mac-v1_4`): the
panel app `ESI U168 XT Panel` (x86_64, Objective-C) and the kext `ESI_U168_XT.kext` (Ploytec
PGDevice code). Ghidra decompiles of both in `OEM_DRIVERS/ESI_U168XT/decompile/` (panel.c,
kext.c). Found 2026-11-01 for #98; **not yet tested on hardware**. ESI's 2.0 beta driver
(EsiUSB, HAL plugin) has no control panel ("all analog audio I/O is supported").

## What the panel offers

| Panel control | Method | Reaches the device as |
|---|---|---|
| Clock Internal / Auto | `setInternalAUTO:` → `configAnlogDigitalSettings` | kext dispatch 0x10 (setClockSource), value 0/1/2 → ESI selector ('I' wIndex 0) + 'A' |
| Inputs 15/16 Analog / Digital | `setAnalogDigital:` → `configAnlogDigitalSettings` | same call (digital only selectable with Auto) |
| S/PDIF format Consumer / Professional | `setDigitalFormat:` | C-port: AK4114 register 0x0D = 0 / 1 |
| Output level knob, mute, link (per output) | `setChannelLevel:`, `setChannelMute:`, `appliedChannelLevel:` | C-port: AK4358 attenuator of that output |
| +4 dBu / -10 dBV per output pair | `setOutBalanced:` | no request of its own: "-10" lowers the pair's attenuator by 10 steps (5 dB) |

After a clock change the panel sends dispatch 0x25 (resumeStreaming).

### Clock / inputs 15-16 (kext `updateAjInputSelector_Esu186`)

`configAnlogDigitalSettings`: Internal → 1; Auto + Digital → 0; Auto + Analog → 2 (then sets
the sample rate again). The kext keeps the read byte's bits `& 0x4C` and ORs:

| AjExtData[1] | Panel | 'I' wIndex 0 | 'A' request |
|---|---|---|---|
| 2 | Auto, inputs 15/16 analog | 0x32 | 0 (wValue 0x348) |
| 1 | Internal (panel shows 15/16 analog, digital greyed) | 0xB0 | 1 (0x349) |
| 0 | Auto, inputs 15/16 digital | 0xB2 (power-up value seen in #98) | 0 (0x348) |

Read back: bit 7 clear → 2; else bit 1 clear → 1, set → 0. **Open:** the bits don't split into
"clock" and "route" (0xB0 shares bit 7 with the digital state). Ozzy wrote 0x32 on every start
before this, and on the #98 unit inputs 15/16 did **not** record the analog inputs at 0x32. So
either the panel's labels don't match the hardware, or analog 15/16 needs something else.

### The C-port

Kext dispatch 0x24 → `AJ::sendCPort` queues up to 255 bytes in a 256-byte queue (AJ+0x50). For
Ploytec bulk/interrupt devices `PGDevice::rtsProcessBulkOut` takes **one byte per output
sub-packet** into the control byte after the MIDI slot (offset 481 of a 482-byte sub-packet;
433/915 in the 964-byte packet), as `byte | 0x18`; with the queue empty the control byte is
0xFF. (For devices with their own MIDI-out endpoint, `AJ::fireOutputInternal` puts one queue
byte at the end of each MIDI transfer, repeating the last one.)

The panel bit-bangs two converters through it (`FUN_10000b75c`, `FUN_10000b9aa`, bytes
collected by `FUN_10000b27a`, flushed by `FUN_1000089e8` in chunks of 255):

- bit 7: chip select (low during a write), shared
- bits 6/5: data / clock of the DAC; bits 1/0: data / clock of the S/PDIF transmitter
- a write: `FF 7F`, then 16 bits MSB first, each as clock-low byte then clock-high byte, then `FF`
  - DAC bit 0/1: `1F 3F` / `5F 7F`; transmitter bit 0/1: `7C 7D` / `7E 7F`
  - the 16 bits: chip address (2 bits), R/W = 1, register (5 bits), data (8 bits): AKM's
    3-wire format. Chip address 2 (`1 0 1`) for the DAC, 0 (`0 0 1`) for the transmitter.

**DAC = AK4358** (inferred from the register table): the panel's output → register table is
`4 5 6 7 8 9 11 12` for outputs 1-8 (register 10 skipped: AK4358's control 3), written as
`level | 0x80` with level 0-127 (`(level - 127) * 0.5` dB in the panel), so 0xFF = 0 dB,
0.5 dB steps. "-10 dBV" subtracts 10 steps (clamped at 0). Mute writes level 0 (0x80).

**Transmitter = AK4114** (inferred): register 0x0D is its transmit channel status byte 0, bit 0
IEC 60958's PRO flag; the panel writes 1 for Professional, 0 for Consumer.

Power-up state of both (datasheet defaults, assumed): attenuators at 0 dB, channel status 0,
i.e. +4 dBu / Consumer: what Ozzy's defaults send.

## In Ozzy

`common/devices/ploytec/ploytec_cport.h` builds the writes; the Ploytec bulk engine queues them
at every start (`cport_queue`: the S/PDIF mode, then all 8 attenuators) and puts one per
sub-packet in the control bytes (`cport_out`), then sends every place idle once more. Settings
(OZZY_QUIRK_ESI_CPORT, U168 XT only): "Clock and Inputs 15-16" (three states, default Auto
analog = 0x32 as before), "IEC958 Playback Mode", "Output 1-2 Level" ... "Output 7-8 Level".
A change restarts the engine like every device setting. Tests: `tests/test_cport.c`.

Not done: the per-output level knobs and mute (a hardware volume control would fit them; Ozzy
doesn't scale samples), the U86 XT and U108 PRE (their panels not checked).
