# Ploytec Sample Rates and Stream Formats — From the Official Driver

Short answer: **the device is never asked which rates it supports.** The official driver ships
a per-device format table in the kext's Info.plist (`PtSpecific.kConfigurationData`).
**Xone:DB2 = 48 kHz only. Xone:DB4 = 44.1/48 kHz (2ch) and 44.1/48/88.2/96 kHz (8ch).**

Sources:
- Ghidra decompile of official macOS drivers v3.4.9: `XONE_DB2.kext` + `XONE_DB2.driver`
  (x86_64, with symbols). The DB4 v3.4.9 kext has the same code; only the plist differs.
- Windows DB2 USB capture from PR #70 (`db2.pcapng`, driver 2.9.91.0, audio fw 1.2.0).

## 1. The device advertises nothing

DB2 (0x0A4A:0xFFD2, audio fw 1.2.0) configuration descriptor, 80 bytes, from the Windows capture:

```
Config 1, 2 interfaces, self-powered
  Interface 0 alt 0: class 0xFF, 0 endpoints
  Interface 0 alt 1: class 0xFF, 4 endpoints
    EP 0x02 OUT  isoc       wMaxPacketSize 4     (keep-alive, only used in bulk mode)
    EP 0x83 IN   bulk       512                  (MIDI in)
    EP 0x05 OUT  interrupt  512                  (PCM out + embedded MIDI out)
    EP 0x04 OUT  bulk       512
  Interface 1 alt 0: class 0xFF, 0 endpoints
  Interface 1 alt 1: class 0xFF, 1 endpoint
    EP 0x86 IN   interrupt  512                  (PCM in)
```

For comparison, the Xone:4D (issue #9, `lsusb -v`) has the same layout, except
EP 0x05 is interrupt **964** and EP 0x86 is interrupt **1024**.

Everything is vendor-specific class. There are no audio-class descriptors, so there is no rate list.

## 2. Where the official driver gets the format table: the plist

`PGKernelDeviceXONEDB2::initSpecificFeatures` reads `PtSpecific` from the matching IOKit
personality:

| Key | Meaning |
|-----|---------|
| `kConfigurationData` (0x178 bytes) | Copied by `PGDevice::initialSettings` into `APISpecificFeatures` at `PGDevice+0x660` (0x17C bytes); sets `this[0x7d7] = 1` ("configured from plist") |
| `kInACR` | stored at `PGDevice+0x810` |
| `kMsWait` | stored at `PGDevice+0x18`; `configurationDone` sleeps that many ms (capped at 1000) before its setup. Numarks: 200. Tascams without a plist get 100 from `initDeviceSpecificData` |
| `kClockSourceSettings`, `kCustomName` | optional (absent for DB2/DB4) |

### kConfigurationData layout

Four `APIStreamingInfo` entries of 0x4C bytes each. This is the same struct that
`PGInterfaceDescriptorSpecificAS::getAPIStreamingInfo` would build from real UAC descriptors:

```
+0x00 u8  bitResolution
+0x01 u8  numChannels
+0x02 u8  isContinuous
+0x03 u8  numFrequencies (max 16)
+0x04 u32 lowerFrequency  (only if continuous)
+0x08 u32 upperFrequency  (only if continuous)
+0x0C u32 frequencies[16]
```

After the four entries comes a flag block. Offsets 0x130.. map onto `PGDevice+0x790..`:

```
0x130: 01 01 00 01 00 01 00 00  01 01 00 01 00 00 01 00  01 00 00 01
        → this[0x798]=1  isJunctionCompatible() (spec feature 0)
          this[0x7a0]=1
          this[0x7a1]=0  setFreqOnOutPipe() capability → off
          (identical in DB2 and DB4)
```

### The two official tables

| Entry | Xone:DB2 (`XONE_DB2.kext` 3.4.9) | Xone:DB4 (`XONE_DB4.kext` 3.4.9) |
|-------|----------------------------------|----------------------------------|
| 0x000 24-bit 2ch  | **48000** | 44100, 48000 |
| 0x04C 24-bit 2ch  | **48000** | 44100, 48000 |
| 0x098 24-bit 8ch  | **48000** | 44100, 48000, 88200, 96000 |
| 0x0E4 24-bit 8ch  | **48000** | 44100, 48000, 88200, 96000 |

(Entries 0/1 = Full Speed input/output, 2/3 = High Speed input/output. The in/out order is
confirmed by the WIZARD_4 3.3.11 kext, whose table is asymmetric: entry 2 = 16 ch, entry 3 = 2 ch,
matching the WZ4's 16 inputs / 2 outputs. Its flags at 0x130 equal DB2/DB4's.)

Raw dumps: `DB2_decompile/DB2_kConfigurationData.bin`, `DB4_kConfigurationData.bin` in the
investigation folder.

### buildDummyDescriptor is NOT used for DB2/DB4

`USB::buildDummyDescriptor` has a generic hardcoded table that gives every Ploytec High
Speed PID 44.1/48/88.2/96k. `scanConfigurationDescriptors` only calls it when
`this[0x7d7] && !this[0x798] && !this[0x7ae]`. DB2 and DB4 have `0x798` set (junction
compatible), so they skip it and use the plist table above. An earlier version of this file
got this wrong.

## 3. CoreAudio plugin mirrors the kext table

`PtASPlugInDevice::getSampleRates` loops over 32k…192k and asks `canSampleRate(rate)`, which
reads the kext capability matrix (`getCaps`). One exception: if the device is locked to an
external digital clock (see §4), only the current rate is offered.

## 4. The only things the device reports about rates

Only its **current** rate, via UAC1 endpoint GET_CUR (`0xA2`, `0x81`, `wValue 0x0100`).
`getFrequency` maps the 16-bit truncations 0x5888 → 88200 and 0x7700 → 96000.

1. **Readback check** (`updateAjInputSelector`): if GET_CUR ≠ the configured rate, it logs
   `"Device Frequency mismatch expected:%d current:%d"` and sends SET_CUR again.
2. **Digital lock**: if 'I' idx0 bit 2 (0x04) is set, the driver reads the current rate as
   `AjExtData.mDigitalLockFrequency`. The lock flag is kept at `AjExtData+0x28`
   (= data block `+0x146C`); while it is set, CoreAudio is only offered the current rate.

No GET_MIN / GET_MAX / GET_RES anywhere, in the kext or the Windows capture.

## 5. SET_CUR sequence in the official driver

`setFrequency(inPipe, outPipe, rate)` sends exactly **one** SET_CUR, to `inPipe` if non-null,
otherwise to `outPipe` (wIndex = endpoint address). The only retry loop is for VID 0x0499.

For Ploytec junction devices, `configureDevice` clears the isoc slots (`0xab0`, `0xac0`) and
puts EP 0x86 into `0xa90` and EP 0x05 into `0xa98`, whether they are bulk or interrupt. The
start path then sends:

```
startStreaming   SET_CUR → EP 0x86, SET_CUR → EP 0x05   (OUT call because isoc-in slot is empty)
resumeStreaming  SET_CUR → EP 0x86, SET_CUR → EP 0x05   (isJunctionCompatible && !JCT30)
bulkAudioRun     SET_CUR → EP 0x86, GET_CUR ← EP 0x86, sleep 50 ms
```

That's **5 SET_CURs + 1 GET_CUR**. The Windows capture shows the same pattern with one extra
IN/OUT pair (86,05,86,05,86,05,86, then GET_CUR 86), so the Windows build likely goes through one
more of these paths. The exact count is not magic; it's the sum of code paths. Ozzy
currently sends 2 (commit bbfa66d, which was based on an incomplete reading). See open_questions.md.

## 5a. A rate change in the official Windows driver (captured)

DB4 fw 1.4.1, Allen & Heath's Windows package 2.9.9.10 (driver 2.9.91.0, its ASIO driver doing
the change), USBPcap, 2026-10-21; `captures/db4_windows_official/official_rate.pcap` (+ `.log`),
three changes 48 -> 96 -> 44.1 -> 48 kHz driven by `tools/hwtest/asio_probe rate`. Every change
went the same way (times from the last PCM completion before it):

1. Streaming stops; the transfers drain. Then **~100 ms with no traffic at all**.
2. **SET_CONFIGURATION 0** (unconfigure), GET_DESCRIPTOR (device), 'V' (firmware), the
   configuration descriptors, **SET_CONFIGURATION 1**. No USB port reset: the device keeps
   its address and doesn't re-enumerate.
3. 50 ms, then the handshake: 'I' read, GET_CUR (EP 0), SET_CUR 86 / 05 / (GET_DESCRIPTOR) /
   86 / 05 / 86 / 05 / 86 (the §5 pattern, 7 SET_CURs here), GET_CUR 86, 'I' read, 'I' write
   0xFFB2. Not one stall.
4. ~310 ms later: ABORT_PIPE + SYNC_RESET_PIPE_AND_CLEAR_STALL on 0x86, then on 0x05, and the
   stream starts again. **~565 ms without audio** in all.

So the official driver neither resets the port (Linux main's way, which the Xone:4D doesn't
survive, #86) nor restarts straight after aborting (the engine's way, which the DB4 answers with
stalls for ~100 ms: the handshake retry, 6feeec5): it waits, unconfigures and reconfigures the
device, and only then talks to it. A candidate for the engine's restart and the 4D.

The engine now waits too (4044527, PLOYTEC_RESTART_QUIET_MS = 120 ms before the handshake of a
restart, not on attach), keeping the retry as a fallback; it doesn't unconfigure the device.
Measured on the DB4 fw 1.4.1, 2026-10-22: Linux, three rate changes without one stalled request
(before: 'V' sent 2 ms after the suspend stalled every time and was answered on the second
attempt, ~115 ms in); macOS (XHC0 capture), two rate changes, 'V' 120 / 121 ms after the last PCM
completion, all 24 control requests answered at once. On Linux the one short IN transfer counted
at each start went away with it.

Its attach (`official_attach2.pcap`, a replug; times from the first request after enumeration):
GET_DESCRIPTOR (device); +172 ms 'V' (firmware, 3 bytes); the configuration descriptors and the
device descriptor again; SET_CONFIGURATION 1; +50 ms 'I' read, GET_CUR (EP 0); SET_CUR 48000 to
86 / 05 / 86 / 05 / 86 (5); GET_CUR 86; 'I' read; 'I' write 0xFFB2; then ~610 ms later
ABORT_PIPE + SYNC_RESET_PIPE_AND_CLEAR_STALL on 0x86 and 0x05, and streaming. The rate change
above is this sequence with SET_CONFIGURATION 0 in front. On the unplug it aborts its pipes and
tries SET_CONFIGURATION 0 (fails: the device is gone).

The same package streams continuously from attach, also with no client: 4 transfers in flight
each way, OUT 11568 bytes (12 x 964 = 240 frames), IN 8192 bytes (8 x 1024 = 128 frames), all
whole packets (`official1.pcap`). A client starting or stopping sends no control request. Its
ASIO driver reports buffer size 4096 frames only (min = max = preferred) and latencies of 484 in /
1286 out frames at 48 kHz. The Windows Update package (2.9.96.6) has the same kernel drivers
without the ASIO driver, so it can't change the rate: WDM-KS clients at another rate get silence.

## 6. The DB2 can't carry more than 48 kHz anyway

Interrupt endpoints with bInterval 1 move one packet per 125 µs microframe:

| Device | EP | wMaxPacketSize | Frame size | Frames/µframe | Max rate |
|--------|----|----------------|------------|---------------|----------|
| DB2 fw 1.2.0 | 0x05 out | 512 | 48 B | 10 | 80 kHz |
| DB2 fw 1.2.0 | 0x86 in  | 512 | 64 B | 8  | **64 kHz** |
| Xone:4D      | 0x05 out | 964 | 48 B | 20 | 160 kHz |
| Xone:4D      | 0x86 in  | 1024| 64 B | 16 | 128 kHz |

So 88.2/96 kHz is physically impossible over the DB2's endpoints. This matches the official
48k-only table and the A&H spec ("USB soundcard sampling frequency: 48kHz").

The device does *accept* SET_CUR 96000. PR #70 reports it then plays at half speed. Ozzy
must not offer anything but 48000 for the DB2.

The DB2 boots at 44100 (GET_CUR before the first SET_CUR, in both the Windows capture and every
Ozzy log), but 44100 is not in the official table either. Only 48000 is.
