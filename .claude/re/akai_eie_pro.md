# Akai EIE Pro (USB 09E8:0010)

Source: the official EIE Pro kext v3.3.17 ("Dec 28 2016", x86_64, same generic Ploytec `PGDevice`
codebase as the Xone kexts), Ghidra decompile `AKAI_EIE/AKAI_EIE_kext_all.c`, compared with
`DB2_decompile/XONE_DB2_kext_all.c`. Addresses are from the EIE kext. Everything here comes from the
code. Nobody has captured the device yet, and we have no descriptor dump. Anything that depends on
the device's descriptors is marked **inferred**.

`PGKernelDeviceDrvAkaiEIE::start` @0x1c720 refuses any device other than VID 0x09E8 / PID 0x0010.

## Bottom line

**Confirmed:** the EIE Pro is a "bulk in, isochronous out" device.

| Direction | Interface | Transfer | Format | Same as Xone? |
|-----------|-----------|----------|--------|---------------|
| PCM out (4 ch, 24 bit) | 0 | **isochronous** OUT, 32 microframe packets per URB | plain interleaved 3-byte LE (`pcmTo24LSB`), 12 bytes per frame | **no** |
| Rate feedback | 1 | **isochronous** IN, 3-byte packets, byte 0 = sample count | `PGFramePatternObserver` | **no** (new) |
| PCM in (4 ch) | 1 | bulk IN (or interrupt IN) | Ploytec 64-byte bit-plane frame, `dmaDecode16To04At32Bit` | yes |
| MIDI out | 0 | **own** bulk (or interrupt) OUT EP, 9-byte transfers | 3-4 MIDI bytes + FD padding + control byte | **no** (Xones embed MIDI in PCM out) |
| MIDI in | 0 | interrupt (or bulk) IN, 9-byte reads | `inACR_M4_C1` at every rate | yes at ≤48k |
| Keep-alive | - | none | - | Xones don't need it either |

The vendor/class control sequence is the Xone one ('V', 'I' read-modify-write, UAC1 endpoint
SET_CUR/GET_CUR rate), but the rate goes to the actual endpoint addresses, not the hardcoded
0x86/0x05.

## 1. Device classification and plist flags

`kConfigurationData` (376 bytes, `AKAI_EIE/config/3.3.17_AkaiEIE.bin`) is copied to `PGDevice+0x660`
by `PGDevice::initialSettings` @0x12e96, which also sets `this[0x7d7] = 1` ("using plist"). The flag
block at blob offset 0x130 lands at `PGDevice+0x790`:

```
+0x790: 01 01 00 01 00 01 00 00 01 00 01 01 00 00 01 00 01 00 00 01   (rest 0)
```

| Offset | Value | Read by | Meaning for the EIE |
|--------|-------|---------|---------------------|
| 0x790 / 0x792 | 1 / 0 | `findInterfacesInConfig` @0xcd28 | IN interface = 1, OUT interface = 0 |
| 0x794 / 0x795 | 0 / 1 | `findInterfacesInConfig` | MIDI fallback interface 0, alt 1 (only if no class MIDI or vendor interface is found) |
| 0x791 / 0x793 | 1 / 1 | not referenced | alt settings? (**inferred**) |
| 0x798 | 1 | `isJunctionCompatible` @0xc862 | Ploytec "junction" device: 'V' read, 'I' handshake, setFrequency in `resumeStreaming` |
| 0x799 | **0** | `isPloytecBulkDevice` @0xe53e | **not** a Ploytec bulk device (DB4: 1) |
| 0x79a | **1** | `isPloytecBulkInIsocOutDevice` @0xe756 | **bulk in / isoc out** (DB4: 0) |
| 0x79b | 1 | `isJunctionCompatibleWithRawMidi` @0x12f88 | AJ raw MIDI on the OUT interface |
| 0x79c | 0 | `isJunctionCompatibleWithRegularMidi` @0x12fa4 | no class-compliant MIDI framing |
| 0x79d | 0 | `PGInterfaceDescriptor::scan` @0x20dda | don't force interfaces 0..3 to AS/MIDI class |
| 0x79f | 0 | `canHandleRequestsWhileStreaming` | |
| 0x7a0 | 1 | `ajHasNoDigital` @0x10052 | no digital input |

`isPloytecBulkInIsocOutDevice` (the plist branch) returns `this[0x79a] && this[0x1888]`, and
`this[0x1888]` is the High Speed flag. The hardcoded VID/PID list that names 09E8:0010 only applies
to devices without a plist. At Full Speed the device is neither bulk-in/isoc-out nor Ploytec-bulk.
The FS config in the blob is 2 ch at 44.1/48k, which would use plain isoc both ways (**inferred**,
not traced).

`initSpecificFeatures` @0x1c342 reads only `kMsWait`, `kInACR` (→ `PGDevice+0x810`),
`kClockSourceSettings`, `kConfigurationData` and `kCustomName`. `kHasMIDIAdapter` and
`kIsJunctionCompatibleWithRawMidi` are not read as keys. Their effect comes from the flag block.

## 2. Pipe assignment

`PGDevice::findInterfacesInConfig` @0xcd28:

```c
if (isPloytecBulkDevice(this) || isPloytecBulkInIsocOutDevice(this)) {
    this->m_pcOutInterface (+0xb70) = getInterface(cfg, 0);
    this->m_pcInInterface  (+0xb68) = getInterface(cfg, 1);
}
```

Then `tryFindInterfaces` @0xe9a4 replaces both with the alt setting that `findAlternateSetting`
@0x2211a finds for rate/channels/bits. Interface 1 is forced to class 1/2 (AS) by
`PGInterfaceDescriptor::scan` because `0x798 = 1`. Interface 0 must declare AS class itself, or no
OUT alt is found (**inferred**). Right after that, `AJ::assignPipes` @0x2986 runs.

`PGKernelDeviceDrvAkaiEIE::configureDevice` @0x1ddfe:

```c
m_pcOutPipeIsoc (+0xac0) = getIsocOutPipe(m_pcOutInterface);          // PCM out
m_pcInPipeIsoc  (+0xab0) = getIsocInPipe(m_pcInInterface);            // cleared again below
// isPloytecBulkDevice == false: no EP 0x05 lookup, no bulk out (+0xa98 stays 0),
//                               no keep-alive pipe (+0xac8 stays 0)
...
else if (isPloytecBulkInIsocOutDevice(dev) && !terminating) {      // VID 0x9e8, not ACV0 bcd 0x300
    dev->m_pcInPipeIsoc     (+0xab0) = 0;
    dev->m_pcInPipeFeedback (+0xab8) = getIsocInPipe(m_pcInInterface);
    _IOLog("got isoc in pipe %p from interface %p num:%d alt:%d", ...);
    dev->m_pcBulkInPipe (+0xa90) = getBulkInPipe(m_pcInInterface);
    if (!+0xa90) { +0xa90 = getInterruptInPipe(m_pcInInterface); "Use interrupt transfers"; +0xb53 = 1; }
}
...
m_pcInPipeFeedback->setBandwidth(wMaxPacketSize)          // "setBandwidth for feedback"
```

| Slot | Pipe | Interface | Use |
|------|------|-----------|-----|
| `+0xac0` m_pcOutPipeIsoc | first isoc OUT | 0 (alt from tryFindInterfaces) | PCM out |
| `+0xab8` m_pcInPipeFeedback | first isoc IN | 1 | rate feedback |
| `+0xa90` m_pcBulkInPipe | first bulk IN, else first interrupt IN | 1 | PCM in |
| `+0xa98`, `+0xab0`, `+0xac8`, `+0xaa0/8` | - | - | unused |
| AJ `+0x13f8` m_pcMidiOutPipe | first bulk OUT, else interrupt OUT | 0 (`+0xb70`) | MIDI out |
| AJ `+0x13f0` m_pcMidiInPipe | first interrupt IN, else bulk IN | 0 (`+0xb70`) | MIDI in |

PCM in may be bulk or interrupt. The code accepts both, and which one depends on the firmware
descriptors (**unknown**). Bulk IN errors are ignored on Darwin > 14 (`bulkInpEvent` @0x7102).

MIDI: `AJ::assignPipes` uses `m_pcOutInterface` because the device is junction + raw-MIDI. It
clears the MIDI OUT pipe only for `isPloytecBulkDevice`, so the EIE **keeps a separate MIDI OUT
endpoint**. A `USBMidiDevice` is also created (`onServiceStart` ~0x15750), but
`USBMidiDevice::assignPipes` @0x290b4 is empty, so it never gets pipes. MIDI goes through `AJ`.

Endpoint numbers are not in the kext (every pipe is found by type and direction). They are in
[MatTaylorSharrow/eie-pro-linux](https://github.com/MatTaylorSharrow/eie-pro-linux) (2016, GPL-2.0,
an unfinished ALSA driver plus a libusb prototype that played and recorded 4 ch at 44.1k on a real
EIE Pro, fw 1.04). Facts only, no code taken. Its prototype claims both interfaces and sets
**alt 1 on both**:

| EP | Interface | Type | Use (prototype) | Kext slot |
|----|-----------|------|-----------------|-----------|
| 0x02 | 0 | isoc OUT | PCM out, 12 bytes per frame, packet length = frames × 12 | `m_pcOutPipeIsoc` |
| 0x83 | 0 | bulk IN | MIDI in, 9-byte transfers, 0xFD filler | AJ MIDI in |
| 0x04 | 0 | bulk OUT | MIDI out, 9 bytes: MIDI bytes, 0xFD padding, ends in 0xE0 | AJ MIDI out |
| 0x81 | 1 | isoc IN | "clock": 3 bytes per microframe | `m_pcInPipeFeedback` |
| 0x86 | 1 | bulk IN | PCM in, 64-byte bit-plane frames (ch1/ch3 bits 0/1 of bytes 0-23, ch2/ch4 of bytes 32-55) | `m_pcBulkInPipe` |

(The README calls PCM in "Endpoint 5"; the prototype code uses 0x86, which matches the Xones.)

**Feedback bytes 1-2:** byte 0 = frames the device consumed since the previous microframe, bytes 1
and 2 = byte 0 of the previous two microframes (a shift register). The kext only reads byte 0.

**Same endpoint map as the Xones**, except where PCM out goes: the Xones' EP 0x02 is the 4-byte
isoc "keep-alive", EP 0x04 is the bulk OUT that `AJ::assignPipes` leaves unused (MIDI rides in PCM
on EP 0x05), and 0x83 / 0x86 are MIDI in / PCM in on both. The Xone design looks like the EIE's
with PCM out moved from isoc EP 0x02 to bulk/interrupt EP 0x05 (**inferred** from the layout).

Init sequence in the prototype (from a capture of the official driver, per its README): 'V'
(0x56) read 3 then 5 bytes (`31 01 04`, fw 1.04), 'I' (0x49) read = 0x32, UAC1 GET_CUR rate,
SET_CUR rate on EP 0x86, SET_CUR on EP 0x02, SET_CUR on 0x86 again, GET_CUR to verify, 'I' read,
'I' write wValue 0x32. That is the Xone handshake, with EP 0x02 where the Xones use 0x05.

## 3. Streaming path

`PGDevice::resumeStreaming` @0x173c2 picks the path from which pipes exist:

```c
if (isJunctionCompatible && !isJCT30Compatible) {
    setFrequency(ab0 /*0*/, a90, rate);    // -> bulk IN endpoint
    setFrequency(ac0, a98 /*0*/, rate);    // -> isoc OUT endpoint
}
...
else if (ac0 && a90) {                     // EIE
    bulkAudioRun(this);                    // PCM in, as on the Xones
    rtsIsocAudioRun(this);                 // PCM out + feedback
    KeThread::setService(rtsIsocOutBulkIn);
}
```

The keep-alive (`rtsIsocAudioRunKeepAlive`) runs only in the bulk/bulk branch. The EIE never sends
it.

`rtsIsocOutBulkIn` @0x17aa6 is `rtsProcessIsocOut_Std` + `rtsProcessBulkIn`.

### Isochronous output

`rtsIsocAudioRun` @0x1797e: `prepareForStream(currentFrame + 50)` on the in/out/feedback
`IsocData`, two `requestIOFeedback`s, then three `rtsRequestOut`s. Each completion
(`rtsIsoWriteComplete` @0x1978e) queues the next one. If the completion comes after the URB's
frame has passed, the stream stops ("rtsIsoWriteComplete to late").

URB shape (`PGIsocRB::PGIsocRB` @0x1b31a, `IsocData::next` @0x1bcce):
- `PGIsocRB` holds the `IOUSBLowLatencyIsocFrame` list at +0. Per-packet {samples, offset} is at +0x200.
- On a HS device with the pipe's **bInterval == 1**: 32 packets (microframes) per URB, 32 KB
  buffer. Otherwise: 4 packets.
- Each URB is 4 ms (`IsocData` start frame += 4). Write `updateFrequency` = 4 at HS
  (`rtsRequestOut` @0x1939e).
- Packet `i` = `pattern[i] × (out_bytes × out_ch)` = `pattern[i] × 12` bytes
  (`PGIsocRB::initWrite_Std` @0x1b818). No header, no MIDI, no padding.
- `rtsProcessIsocOut_Std` @0x17cbc fills 8 microframe packets (1 ms) at a time with
  `DriverData::rtsIsocOutput` @0x7e3a. That mixes clients into int32, then runs the converter over
  the contiguous bytes.

`pattern[]` is the output ring of `PGFramePatternObserver` (`PGDevice+0xda0`, 128 entries at HS,
16 at FS). `PGFramePatternObserver::init` @0x1a97a seeds it with the nominal per-microframe
sample counts (`initFramePattern` @0x1665e, from `resumeStreaming`):

| Rate | Samples per microframe (HS) | Bytes per packet (4 ch × 3) |
|------|-----------------------------|-----------------------------|
| 44.1k | 5,6,5,6,…, +1 every 80 (avg 5.5125) | 60 / 72 (/84) |
| 48k | 6 | 72 |
| 88.2k | 11, +1 every 40 | 132 / 144 |
| 96k | 12 | 144 |

**Inferred:** this only works if the OUT endpoint has bInterval 1. With 4 packets per URB, one
packet per ms would carry a single microframe's worth of samples.

### Feedback

`requestIOFeedback` @0x145a8 → `PGKernelDeviceDrvAkaiEIE::isocReadFeedback` @0x1a824 (isoc read
on `+0xab8`, `updateFrequency` 4). Packet count per URB: 32 if the feedback EP's bInterval is 1,
else 4 (`configurationDone` passes `pipe->bInterval == 1` to `IsocData::initForPipe`). Each
completion re-arms itself (`initNextTransactionFeedback` @0x1474c), with two transfers in flight.

`isocReadFeedbackCompleteStatic` @0x1a6f6:

```c
for (i = 0; i < nFrames; i++) {
    fb[i] = *data;                          // ONLY byte 0 of each packet, as an integer
    if (frame[i].frActCount != 3)           // feedback packets must be 3 bytes long
        this->mIsocInFeedback.mErrorCounter++;   // PGDevice+0xa28
    data += frame[i].frReqCount;
}
PGDevice::isocReadCompleteFeedback(this, rb, fb);
```

So the feedback is **not** the USB 10.14/16.16 fixed-point format. Byte 0 is a plain sample count.
The other two bytes are ignored. A standard 10.14 value for 48k (`00 00 0C`) would read as 0 and
be rejected. **Inferred:** the EIE's firmware sends `[count, ?, ?]`.

`isocReadCompleteFeedback` @0x14696 picks the parser by the feedback EP's **bInterval**
(`PGPipeDescriptor+0x16`):

| bInterval | Parser | Value | Accepted range | Effect |
|-----------|--------|-------|----------------|--------|
| 4 (1 ms) | `isocFeedbackReadCompleteFullFrame` @0x1afba | samples in the last 1 ms | rate/1000 ± 2 | copies the precomputed 8-microframe split for that count (`+0xa40..0xac0`, filled by `fpoInitPattern`) into the ring |
| other | `isocFeedbackReadComplete` @0x1b0b0 | samples per (micro)frame | nominal ± 1 (HS), ± 2 (FS) | writes the value into the ring |

A value out of range is replaced by the nominal pattern entry and drops the "locked" state. When
the ring wraps back to its start without errors, it locks and the error counter resets. More than
40 errors (bad length) → `stopStreaming` + `STC_STREAM_BROKEN`. The OUT path reads the ring
(index `+8`) behind the feedback writer (index `+0xc`). The host just copies the device's
per-interval sample counts into the next OUT packets, with no rate estimation.

**Without feedback** (no `+0xab8`): no feedback requests, and the ring keeps the nominal pattern
from `init`. Output is open-loop at the nominal rate. The PCM IN (bulk) clock is not used for
output pacing.

## 4. PCM encoding

### Output: `pcmTo24LSB` @0x9d3e

`configureDevice` and `chooseISOOutEncoder` @0xf828 only pick the `pcmTo24Junction*` encoders for
`isPloytecBulkDevice`. The EIE falls through to `pcmTo24LSB`, because 0x7a9 is 0 (no US-144
variant):

```c
for (i = 0; i < bytes/3; i++) {          // in: interleaved int32, out: 3 bytes per sample
    out[0] = in[i] >> 8; out[1] = in[i] >> 16; out[2] = in[i] >> 24;   // 24-bit LE, top bits
}
```

Frame = `L1 L1 L1 R1 R1 R1 L2 …`, i.e. ch1..ch4 × 3 bytes LE = 12 bytes. This is the standard UAC
"type I PCM, subslot 3".

For comparison, `pcmTo24Junction4CH` @0xa2a5 (not used here) writes the same 12-byte frame but in
channel order **1, 3, 2, 4**: in bytes 1..3 → out 0..2, in ch2 → out 6..8, in ch3 → out 3..5,
in ch4 → out 9..11.

No DMA encoder: `chooseDMAEncoder` @0x68e8 returns early because `+0xa98` is 0.

### Input: Ploytec bit-plane frames (as on the Xones)

`configureDevice` picks `pcmFrom24LSB` for the EIE, but only for the isoc IN `IsocData`, and
`+0xab0` is cleared, so it is never used. `rtsProcessBulkIn` @0x188a8 always decodes with the DMA
decoder at `+0x55b8`. `bulkAudioRun` @0x635c sets that up:

```c
lanes = setAJDMAInputChannels(this, inCh /*4*/);   // 'I' RMW, returns 0x10 for <= 16 ch
framesPerPacket (+0x7168) = (lanes == 16) ? 8 : 4;  // x2 if bulk IN wMaxPacketSize > 512
chooseDMADecoder();                                 // 4 ch + 16 lanes -> dmaDecode16To04At32Bit
```

`_dmaDecode16To04At32Bit_C` @0x2f43c: 8 frames × 64 bytes per call, MSB first:

```
ch1 = bit0 of bytes 0x00..0x17   ch3 = bit1 of bytes 0x00..0x17
ch2 = bit0 of bytes 0x20..0x37   ch4 = bit1 of bytes 0x20..0x37
```

This is the 16-lane frame from `ploytec_device_table.md`, with 4 lanes in use. Ozzy's 8-channel
decoder already handles it: take ch 1..4 and drop 5..8. No input postprocessor
(`chooseInputPostprocessor` @0xf610 sets `+0x920 = 0`).

## 5. Control requests: sample rate and init

| When | Function | Request |
|------|----------|---------|
| `onServiceStart` → `scanFirmwareVersion` @0x1316a | 'V' | `C0 56 0000 0000 len 8` (`0x798=1`, so it runs) |
| `configureDevice` | SET_CONFIGURATION (only if the current config isn't the plist one), SET_INTERFACE via `matchInterface` | standard |
| `configurationDone` → `updateAjInputSelector` @0x1008c | 'I' | read `C0 49 0000 0000 len 1`. Bit 1 is set because `0x7a0` = no digital in. If the byte changed: write `40 49 (short)(char)v 0000`, then `A2 81 0100 0000 len 3` |
| `startStreaming` @0x1101a | SET_CUR rate | `22 01 0100 <bulk IN EP> len 3` and `22 01 0100 <isoc OUT EP> len 3` |
| `resumeStreaming` | SET_CUR rate | same pair again |
| `bulkAudioRun` | SET_CUR + GET_CUR | `22 01 0100 <bulk IN EP>`, `A2 81 0100 <bulk IN EP> len 3`, sleep 50 ms |
| `bulkAudioRun` → `setAJDMAInputChannels` @0x60f0 | 'I' | read `C0 49 0000 0000 len 1`, write `40 49 (short)(char)(v | 0x30) 0000` (≤16 ch: bits 4+5) |

`setFrequency` @0x1f6da / `getFrequency` @0x1f870 are UAC1 endpoint `SAMPLING_FREQ_CONTROL`
requests. `wIndex` = `PGPipeDescriptor+0xc` = the full bEndpointAddress. `getFrequency` maps
`0x5888` → 88200 and `0x7700` → 96000 (devices that only answer 2 bytes).

Compared with Ozzy's `ploytec_handshake`: the **same requests**, with three differences:
- The OUT rate goes to the **isoc OUT endpoint**. Ozzy hardcodes `wIndex` 0x86 / 0x05
  (`PLOYTEC_EP_RATE_IN/OUT`), so both have to come from the descriptors.
- The kext writes `status | 0x30`, Ozzy writes `| 0x20`. That difference already exists for the
  Xones, and the DB2 kext's `setAJDMAInputChannels` is the same function.
- Ozzy's clear-halt on 0x86/0x05 comes from the Windows driver. The kext doesn't do it. For the
  EIE it would hit the wrong endpoints.

No EIE-specific vendor request exists anywhere. The only VID 0x09E8 checks are in `start`,
`configureDevice` (ACV0) and `chooseInputPostprocessor` (ACV0).

## 6. MIDI framing (`kInACR = 3`)

`AJ::configurationDone` @0x2b08: not regular MIDI and not Elektron, so transfer size
`+0x13d4 = 9` and second-port offset `+0x13d8 = 4`. The MIDI in read buffers are 0x200 bytes at HS,
but each read is 9 bytes long.

`AJ::streamingStarted` @0x2e94: `kInACR = 3` → `inACR_M4_C1`, and `+0x13d8 = 0` (single port).
Unlike the Xones (`kInACR` 0xff, `isPloytecBulkDevice` → case 1; full table in `device_inventory.md`), this is **not** rate-dependent and does not call
`USBMidiPattern::initForBulk`. 16 reads are queued.

**MIDI in** (`AJ::inACR_M4_C1` @0x31d8): `MidiIO::putFrame(buf, 4)`, i.e. the first 4 bytes of
each transfer with every 0xFD dropped. Bytes 4..8 (incl. the control byte) are ignored. This is
the Xone 44.1/48k format. At 88.2/96k the EIE keeps using it (Xone: `inACR_M1_C1`).

**MIDI out** (`AJ::fireOutputInternal` @0x3772), on its own endpoint, one transfer at a time,
re-fired from each completion (`outAsyncCompletionRoutine` @0x372c):

```c
n = USBMidiPattern[idx++ & 0xff];        // _USBMidiPattern_USB1: 4,3,3,3,3,3,3,3 repeating
memset(buf, 0xFD, 9);
Queue64K::get(port0, buf, n);            // up to n MIDI bytes
Queue256::get(cport, &ctrl, 1);          // control byte, default 0xE0 | x
buf[8] = ctrl;
sendRB(buf, 9);                          // bulk timeout 500 ms, interrupt 0
```

The pattern averages 3.125 bytes per transfer. At one transfer per 1 ms that is 3125 bytes/s,
exactly a 31250-baud MIDI cable. **Inferred:** the MIDI OUT endpoint paces at 1 ms
(interrupt bInterval or NAK). The control byte starts as `0xE0 | (*(AJ+0x13c8)+0x10)`. The Xone
embedded path uses `c | 0x18` / 0xFF instead.

## 7. XsDeviceControls / usbInit* / usbApply*

`configurationDone` @0xfae2 calls `usbInitDeviceControls` @0x23aa8 only when
`0x7d7 && !0x798`, so **not for the EIE**. It has no 09E8 case anyway: it is per-device tables of
UAC feature-unit endpoints ("HP 1+2", "Out 1+2", …) for other vendors. `usbApplyDeviceControls`
@0x25c7c still runs. It diffs saved vs current `XsDeviceControls` and sends UAC SET_CUR to feature
units only for listed VID/PIDs (0x499, 0xccd, 0xdba, 0x133e, …). Nothing applies to the EIE. This is
the generic control-panel/mixer layer. **Not needed for audio.**

## What Ozzy would need

Ozzy already has, unchanged: bulk/interrupt PCM in with the 64-byte bit-plane decoder (use
ch 1..4), the 'V' read, the 'I' read-modify-write, and UAC1 rate SET_CUR/GET_CUR.

New:
1. **Descriptor-driven endpoints**: isoc OUT and MIDI OUT/IN on interface 0, bulk/interrupt IN and
   isoc feedback IN on interface 1, with alt settings. The rate requests must use these addresses,
   not 0x86/0x05. Clear-halt on 0x86/0x05 must go.
2. **Isochronous OUT** on both platforms. Linux: an ALSA isoc URB engine (snd-usb-audio style),
   32 microframe packets per URB, 12 bytes × N samples, plain 24-bit LE. macOS: isoc via
   IOUSBHost (`IOUSBHostPipe::io` with isoc frame lists, or the low-latency variant).
3. **Isoc IN feedback reader** with the EIE's quirk: 3-byte packets, byte 0 = sample count per
   frame or microframe (depends on bInterval), feeding the OUT packet sizes, with nominal fallback.
4. A **separate MIDI OUT** path: 9-byte transfers, 4/3 bytes + FD padding + control byte, paced
   ~1 ms. Ozzy's MIDI-in-PCM-out embedding must be off for this device.
5. MIDI in: same `M4_C1` parser at every rate.
6. Device-table entry: 4 in / 4 out, 24 bit, 44.1/48/88.2/96k, and no Xone OUT framing (no
   480/482 layouts, no `dmaEncode`).

Items 2 and 3 are the bulk of the work: Ozzy has no isochronous code at all.

### Implementation status (2026-10-16, untested on hardware)

**Linux: written. It builds with no W=1 warnings on 6.12. macOS: written. The kext, HAL and
MIDI driver build with no warnings.** Both are untested on hardware.

The generic parts, for any isochronous-out device:
- `common/isoc_feedback.h`: the pacing ring (frames per microframe, filled by feedback, played back
  by the output, with a nominal-rate fallback) and two feedback formats: `ISOC_FB_COUNT` (Ploytec
  byte count) and `ISOC_FB_Q16_16` (USB Audio High Speed). It covers both of the kext's parsers
  (per microframe and per 1 ms) and the error limit. Host test: `tools/isoc_feedback_test.c`. It
  checks for no drift against a device clock 0.1% off in both formats, and that ignoring feedback
  fails that check.
- Linux core:
  - With `info->out_isoc`, the core runs the whole isochronous output: 3 × 32-microframe URBs,
    packets sized from the ring, ALSA's samples copied as they are, and 2 feedback URBs. The Xone
    bulk/interrupt handlers are unchanged.
  - Endpoints are `struct ozzy_ep`: a fixed number (the Xones) or "first endpoint of these types
    on interface N" (the EIE, like the kext). They are resolved at probe. Probe fails if a PCM
    endpoint is missing or the OUT transfer type doesn't match `out_isoc`.
  - A dedicated MIDI OUT endpoint is driven by an hrtimer (`info->midi_out_interval_us`), with one
    transfer in flight at a time.
  - MIDI IN follows the endpoint type. The probe logs every endpoint of both interfaces.

The Ploytec parts:
- `ploytec_handshake_eps()` with `ploytec_isoc_rate_endpoints` (0x86 / 0x02) and no clear-halt.
- `ploytec_decode_frame_4ch` (checked in `tools/ploytec_lanes_test.c`).
- In `ploytec_midi.h`: `ploytec_midi_init_slots()` and `ploytec_midi_xfer_begin()` (9-byte
  transfers), both checked in `tools/ploytec_midi_test.c`.
- `linux/devices/ploytec.c`: `ploytec_isoc_ops` is only the handshake, MIDI framing and the info
  table. It refuses Full Speed.

macOS (the same shape as Linux):
- **Shared with Linux:**
  - `common/ozzy_ep.h`: one endpoint spec type and matching rule.
  - The EIE's endpoints and feedback format, once, as `PLOYTEC_ISOC_EP_*` / `PLOYTEC_ISOC_FEEDBACK`
    in `ploytec_defs.h`.
  - `isoc_fb_layout()` and the transfer constants in `isoc_feedback.h`.
  - The handshake and the MIDI framing.
- **Generic kext pieces:**
  - `engine/core/OzzyIsocOutput`: the macOS counterpart of the Linux core's isochronous output. It
    owns the ring, the OUT and feedback transfers, and the timestamps, so any isochronous device
    can use it.
  - `KextBus::SubmitIsoc`: `IOUSBHostPipe::io()` with an `IOUSBHostIsochronousTransaction` per
    microframe (first frame 0 = ASAP, XHCI). It is zero copy: the OUT transactions point into the
    shared frame ring, and `kIsochronousTransactionOptionsWrap` covers the ring's end.
  - Pipes found by type: `StandardUSB::getNextEndpointDescriptor` + `ozzy_ep_matches`, with every
    endpoint logged.
  - A bus timer (`thread_call`, re-armed on an absolute deadline) → `OzzyEngine::OnTimer`.
  - MIDI ring helpers (`ozzy_midi_ring_push/Pop`) used by all three engines.
- **Shared memory v4:**
  - `OZZY_DEVICE_FLAG_PLAIN_OUTPUT`: outputBuffer is a ring of `OZZY_NUM_PACKETS × 80` plain S24
    frames. The HAL writes it with `ploytec_f32_to_s24_Nch` (`common/devices/ploytec/ploytec_codec.c`, via `PloytecPlainOutputWriter`).
  - `safetyOffsetFrames`: 3 × 4 ms read ahead.
  - `isocInUSBBuffer`: the feedback transfers.
- **`PloytecIsocEngine`:** only the EIE-specific parts.
  - The handshake on the endpoints found.
  - PCM in and MIDI in through `PloytecIO.h`, which it shares with the Xone engine.
  - 9-byte MIDI in reads.
  - MIDI out: one 9-byte transfer per timer tick (1 ms) when the previous one is done, the same
    pacing as Linux.

Differences from the kext, on purpose:
- Too many bad feedback packets → log and run at the nominal rate, instead of stopping the
  stream.
- An OUT bInterval > 1 gets one packet per interval carrying that many microframes' frames.

Known gaps:
- PCM in decodes a full 80-frame transfer like the Xones and ignores `actual_length`.
- MIDI out runs at the kext's 3125 bytes/s (the full cable rate). The Xones run at 2500.
- The hrtimer_setup() branch (kernel 6.13+) is not compile-tested yet (only 6.12 was available).

## 8. Other official files (`AKAI_EIE/` in the investigation folder)

- **Mac drivers 2.1.3 to 3.3.17**: every kext matches only 09E8:0010 with
  `PGKernelDeviceDrvAkaiEIE` (2.x also ships a `…Tiger` variant). 3.3.3 and 3.3.17 have the same
  `kConfigurationData`, and 2.x kexts have no `PtSpecific` at all (hardcoded VID/PID path).
- **Windows 2.9.64** (2014-05-21): `akaieieu.inf` binds `USB\VID_09E8&PID_0010`, and `akaieiem.inf`
  binds a separate MIDI driver to `USB\VID_09E8&PID_0010&MI_00`. So the device is composite, with
  MIDI on interface 0. That fits section 2, where MIDI runs on the OUT interface (0).
- **Firmware 1.04** (`akai_eie_pro_104.bin`, 57352 bytes, Mac/Windows `FWUpdater_v104`): an ASCII
  header `10040001`, then version `00 00 01 04`, then unencrypted code. It is the same container and
  the same vector pattern (`e0 69 2a …` at offset 12) as the Xone:DB2 1.20 image (`10200003`,
  `00 00 01 14`). The DB4 1.39 image (`10390001`) looks scrambled. CPU not identified.

## 9. Other EIE Pro driver projects (checked 2026-10-16)

None is MIT. Use them for facts only (endpoint numbers, sequences, behavior), never for code.

| Project | Platform | License | Last push | Status (per its README) |
|---------|----------|---------|-----------|-------------------------|
| [mmm444/eie-pro-linux](https://github.com/mmm444/eie-pro-linux) | Linux ALSA module + libusb prototype (`exp/pokus.c`) | GPL-2.0 | 2025-05 | The original (2014). "Highly experimental". Source of the endpoint map and init sequence in section 2. Forks: MatTaylorSharrow (build targets), ljadach (identical), jforberg (behind), zakerby (2014) |
| [crnogaj/akai-eie-pro](https://github.com/crnogaj/akai-eie-pro) | Linux kernel module | **none** (all rights reserved) | 2025-10 | Written with Claude, based on mmm444's init. Playback and capture work on Kubuntu 24.04. Known issues: a pluck/"robot" sound in the first seconds, a freeze after suspend |
| [EmaP77/AKAI-EIE-Pro](https://github.com/EmaP77/AKAI-EIE-Pro) | Linux kernel module | GPL-3.0 | 2026-08 | "Working": playback S24_3LE, capture S32_LE, 4 ch, 44.1-96k, MIDI 1 in/1 out, JACK and Ardour on 6.12. Reports **firmware v2.00** (Akai's public updater is 1.04). Tells users to blacklist snd-usb-audio |
| [ewan-m/akai-eie-pro-mac](https://github.com/ewan-m/akai-eie-pro-mac) ([site](https://akai.interslicetech.com/)) | macOS DriverKit (AudioDriverKit + USBDriverKit), Apple Silicon | GPL-2.0 | 2026-10-12 | 4 in / 4 out working at 48k, other rates untested. Unsigned: needs Reduced Security plus `systemextensionsctl developer on`, because the DriverKit USB/audio entitlements for 09e8:0010 haven't been granted. Bundles crnogaj's code as `linuex_reference_code/` |
| [my5t3ry/eie-pro-alsa-module](https://github.com/my5t3ry/eie-pro-alsa-module) | Linux (mmm444 copy plus build output) | none | 2021-07 | Nothing new |
| YouTube short "Akai EIE Pro Linux Driver Being updated!!" (Ms.Fixit, ~2026-09) | ? | ? | ? | Not found by web search; need the link |

A YamahaMusicians forum post says snd-usb-audio runs an "EIE Pro" with 2 ports on USB 3 and 4 on USB 2. That is probably the plain (class-compliant) EIE, not the Pro. EmaP77's blacklist hint does suggest snd-usb-audio binds *something* on the Pro, maybe interface 0, which the kext expects to carry AS class descriptors (section 2) (**unverified**).

## Open

- Descriptors: endpoint numbers, alt settings and types are known from eie-pro-linux (section 2).
  Still unknown: bInterval of EP 0x02 and EP 0x81 (the kext branches on both) and wMaxPacketSize.
  Read them at probe time and log them.
- Feedback: the prototype confirms 3 bytes per microframe and what bytes 1-2 hold (section 2).
- What `0x791` / `0x793` mean. They are never read. Both are 1, which matches the alt 1 the prototype sets on both interfaces (**inferred**).
- The meaning of the MIDI control byte (out 0xE0|x, in byte 8).
- FS (USB 1.1) mode: 2 ch, probably plain isoc in + isoc out. Not traced.
- **Correction to existing notes:** `PGPipeDescriptor+0x16` is **bInterval**, not the transfer
  type (`PGPipeDescriptor::scan` @0x23620 logs it as `m_uInterval`, same offset in the DB2 kext at
  0x209b2). So `configureDevice`'s `getPipeByAddress(0x05) == 0 || pipe->+0x16 == 4 →
  getInterruptOutPipe` (DB2 decompile ~line 22660) means "no EP 0x05, or EP 0x05 with
  bInterval 4". `ploytec_bulk_devices.md` / `ploytec_device_table.md` read it as "type 0x04
  (interrupt)". The outcome is probably the same on the DB4/4D, but the wording is wrong.
