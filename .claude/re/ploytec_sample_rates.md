# Ploytec Sample Rates — How the Official Driver Decides

Short answer: **the official driver never asks the device which sample rates it supports.**
Ploytec devices have no USB Audio Class descriptors, so there is nothing to ask. The rate list
is a hardcoded per-VID/PID table inside the driver.

Sources: Ghidra decompile of the official macOS DB2 driver v3.4.9 (`XONE_DB2.kext` +
`XONE_DB2.driver` CoreAudio plugin, both x86_64, with symbols), and the Windows DB2 USB capture
from PR #70 (`db2.pcapng`, driver 2.9.91.0, audio firmware 1.2.0).

## 1. The device advertises nothing

DB2 (0x0A4A:0xFFD2) configuration descriptor, 80 bytes, from the Windows capture:

```
Config 1, 2 interfaces, self-powered
  Interface 0 alt 0: class 0xFF, 0 endpoints
  Interface 0 alt 1: class 0xFF, 4 endpoints
    EP 0x02 OUT  isoc       wMaxPacketSize 4    (keep-alive)
    EP 0x83 IN   bulk       512                 (PCM in)
    EP 0x05 OUT  interrupt  512                 (PCM out, fw 1.2.0)
    EP 0x04 OUT  bulk       512                 (MIDI out)
  Interface 1 alt 0: class 0xFF, 0 endpoints
  Interface 1 alt 1: class 0xFF, 1 endpoint
    EP 0x86 IN   interrupt  512
```

Everything is vendor-specific class. There are no class-specific AS_GENERAL / FORMAT_TYPE
descriptors, so no `tSamFreq` list.

## 2. The driver makes up the descriptors: `USB::buildDummyDescriptor`

`PGDevice::scanConfigurationDescriptors` calls
`USB::buildDummyDescriptor(VIDPID&, PGSimpleArray&, bool isHighSpeed, DeviceSpecificData&)`.
This function builds fake audio-streaming descriptors (channels, bit depth, sample rates) on
top of the vendor interfaces, using `genInit(numChannels, bitDepth)` and `genAddSR(rate)`.
Later code reads them back as if they were real UAC descriptors (`"In frequencies:%d"`,
`"Found frequency %d"` logs).

For VID 0x0A4A, these PIDs share one generic "Ploytec High Speed" branch
(bitmask `0x2000000000a01` on `PID + 0x2E`, plus individual checks):

0xFFD2 (DB2), 0xFFDB (DB4), 0xFFDD (DX), 0xFF4D (4D), 0xFFAD (Wizard 4), 0xFF01, 0xAFFE,
0xCF00, 0xCF04, 0xDA01, 0x5A16, 0xD064.

The resulting rate table:

| PID | High Speed rates | Full Speed rates | Notes |
|-----|------------------|------------------|-------|
| **0xFFD2 (DB2)** | 44100, 48000, 88200, 96000 | 44100, 48000 (2ch) | No DB2-specific entry. Same as DB4 |
| 0xFFDB (DB4), 0xFFDD (DX), 0xFF4D (4D), 0xFFAD (WZ4) | 44100, 48000, 88200, 96000 | 44100, 48000 (2ch) | |
| 0xD064 (Dotec) | 32ch @ 44.1/48, 16ch @ 88.2/96, 8ch @ 176.4/192 | 2ch @ 44.1/48 | Channel count depends on rate |
| 0xC150 (SPL Crimson) | 6ch @ 44.1–96, 4ch @ 176.4/192 | 2ch @ 44.1/48 | |
| 0x2416 (JCT2416) | 44100, 48000 | 44100, 48000 | |

At Full Speed (`isHighSpeed == false`) the driver also clamps channel counts to 2.

### Pruning is hardcoded too

`PGDevice::disableAllByFrequency(rate)` removes a rate from the table. It is only called for
two non-A&H devices: 0x0A4E/0x4040 (removes 88.2k and 96k) and 0x0DBA/0x3000 (removes 32k).
Nothing is ever removed based on what the device says.

## 3. CoreAudio plugin just mirrors the kext table

`PtASPlugInDevice::getSampleRates` loops over 32k, 44.1k, 48k, 64k, 88.2k, 96k, 176.4k, 192k and
asks `canSampleRate(rate)`. That function reads the kext's capability matrix (`getCaps`), which
comes from the dummy descriptor. One exception, see below.

## 4. The only things the device tells the host about rates

The device only reports one value: its **current** rate, via the UAC1 endpoint
sampling-frequency control (GET_CUR, `bmRequestType 0xA2`, `bRequest 0x81`, `wValue 0x0100`).
The driver uses this in two places:

1. **Readback check** (`updateAjInputSelector`): after configuring, it reads the rate back. If it
   differs from what it asked for, it logs `"***** Device Frequency mismatch expected:%d current:%d"`
   and sends SET_CUR again. It does not remove the rate from the list.
2. **Digital lock**: if status register 'I' index 0 bit 2 (0x04, digital lock) is set, the
   driver reads the current rate and stores it as `AjExtData.mDigitalLockFrequency`. The lock bit
   is saved at `AjExtData+0x28` (= data block `+0x146C`). When that flag is set,
   `canSampleRate` only accepts the current rate (data block `+0x142C`), so CoreAudio sees a
   single rate. In other words: when the device is locked to an external clock, the clock decides.
   - 0x0A4A/0x6A02 and 0xF1A0 get a forced SET_CUR 44100 first.
   - It retries up to 20× when the device reports 192000 and the device has no 192k alt setting.

There is no GET_MIN / GET_MAX / GET_RES / GET_RANGE anywhere, in the kext or in the
Windows capture.

## 5. What the Windows driver does with a DB2 (capture, PR #70)

```
GET 'V'  (0xC0/0x56)           → 33 01 14
SET_CONFIGURATION 1
GET 'I'  idx0  (0xC0/0x49)     → 0x12
GET_CUR  (0xA2/0x81, wIdx 0)   → 0x00AC44 = 44100   ← power-on default
SET_CUR  48000 → EP 0x86, 0x05, 0x86, 0x05, 0x86, 0x05, 0x86   (7 calls, ~27 ms apart)
GET_CUR  (wIdx 0x86)           → 0x00BB80 = 48000
GET 'I'  idx0                  → 0x12
SET 'I'  idx0 = 0x0032
```

The DB2 starts up at 44100 (this also matches every Ozzy debug log: "hardware sample rate:
44100 Hz" before the first SET_CUR). The Windows driver picks 48000 itself. It does not ask
the device first.

## 6. So does the DB2 support 96 kHz?

**The official driver thinks so.** It offers 44.1/48/88.2/96 for the DB2 at High Speed, the same
as the DB4. The DB2 is not special-cased anywhere in `buildDummyDescriptor`,
`scanConfigurationDescriptors` or `canSampleRate`.

**A&H spec sheets say otherwise:** "USB soundcard sampling frequency: 48kHz", "DSP core
sampling frequency: 48kHz". S/PDIF out is fixed at 48k/24-bit.

Possible explanations, none confirmed yet:
- The Ploytec USB chip runs at any of the 4 rates and the DB2 DSP sample-rate-converts.
- The table was shared across Ploytec products and nobody tested a DB2 at 88.2/96k.

Evidence so far:
- No log exists of a DB2 receiving SET_CUR 96000 followed by a GET_CUR readback. Ozzy's
  DB2 logs either predate the readback logging or use the patched 48k build.
- The "noise at 96k" reports in #72 / #48 happened while the DB2 interrupt packet format
  was still wrong (482-byte layout, fixed in PR #70). They don't tell us anything about 96k.
- Status register bit 7: DB4 reads 0x92, DB2 reads 0x12. This is the only hardware
  difference seen in the status register. Ozzy writes 0xFFB2 to the DB2 and gets 0x32 back, so
  bit 7 does not stick. The official driver never reads or sets bit 7 for A&H devices; it
  just preserves it. Whether bit 7 means "high rate capable" is **pure speculation**, but
  worth checking.

### How to settle it (test plan)

1. With the fixed DB2 transport, send SET_CUR 96000 to EP 0x86 and 0x05, then GET_CUR.
   - Readback ≠ 96000 → the device refuses. Ozzy can use readback-based detection
     (try each rate, keep the ones that read back the same).
   - Readback = 96000 → go to step 2.
2. Stream a sine at 96k out and loop it back in (or record the master out). If pitch and
   duration are correct, 96k works (possibly with SRC). If it plays at half speed or garbles,
   the chip accepts the value but the DSP doesn't follow.
3. Repeat for 88200.
4. Check whether 'I' idx0 bit 7 changes at any rate.

Until this is tested, defaulting the DB2 to 48000 is the safe choice. It matches both the
A&H spec and what the official Windows driver picks.
