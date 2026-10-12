# Ploytec MIDI framing (in and out)

Source: XONE_DB2 3.4.9 kext (`DB2_decompile/XONE_DB2_kext_all.c`), verified against usbmon
captures of a DB4 fw 1.4.1 on Linux (2026-10-12). Implemented in
`common/devices/ploytec/ploytec_midi.h`, used by both platforms.

## MIDI out: embedded in PCM out, paced to 2500 bytes/s

Ploytec high-speed devices (`isPloytecBulkDevice`) get no MIDI out pipe: `AJ::assignPipes`
clears `m_pcMidiOutPipe` (`this+0x13f8 = 0`) for them, even on models that have an EP 0x04
OUT bulk endpoint (DB2, 4D). MIDI out always rides in the PCM out packets.

`PGDevice::rtsProcessBulkOut` (@0x1667a), per output sub-packet (10 frames):

```c
// pacing table: PGDevice+0x4130 is a USBMidiPattern; +0x4530 = its table, +0x52fc index, +0x5300 length
idx = this[0x52fc]; if (idx >= this[0x5300]) idx = 0; this[0x52fc] = idx + 1;
if (table[idx] == 0 || !Queue64K::get(midiQueue, slot, 1))
    *slot = 0xFD;                                   // idle
ctrl = Queue256::get(controlQueue) ? (c | 0x18) : 0xFF;
slot[1] = ctrl;                                     // control byte right after the slot
```

Slots: 512-byte layout at 480 (control 481); 964-byte layout at 432 and 914 (control 433, 915).
So the second byte of the interrupt layout is a **control byte, not a second MIDI byte**.
(`AudioBulkIRP::useOut` pre-fills 0xFDFD there, but the runtime path overwrites it with FF.)
The Windows driver sends `00 00` in the slot when idle (DB2 capture, PR #70).

The table comes from `USBMidiPattern::initForBulk(rate)` (@0x27140): an error accumulator
(`acc += 1000; open = acc >= 1; acc -= (rate/4*1000)/6250` per entry, period up to 882 entries)
that opens 25000/rate of the slots. At rate/10 sub-packets per second that is
**2500 bytes/s at every rate**, 80% of a 31250-baud MIDI cable (3125 bytes/s at 10 bits/byte):

| Rate | Sub-packets/s | Open slots | Bytes/s |
|------|---------------|------------|---------|
| 44.1k | 4410 | 250 / 441 | 2500 |
| 48k | 4800 | 25 / 48 | 2500 |
| 88.2k | 8820 | 250 / 882 | 2500 |
| 96k | 9600 | 25 / 96 | 2500 |

The index advances whether or not there is data, so unused slots don't add up to bursts.
`checkBulkRate1K` computes length*1000/ones (bytes per 1000 sub-packets, unused here).

What fills the control queue (`Queue256` at `AJ+0x50` / the local in rtsProcessBulkOut) is
unknown. Ozzy always sends 0xFF, as the official driver does when it is empty.

Before 2026-10-12 Ozzy put one byte in sub-packet 0 of each packet only: 1200 bytes/s at 96k,
600 bytes/s at 48k (DB2 runs at 48k only), and FD as the interrupt layout's second byte.

## MIDI in: one frame per transfer on EP 0x83, format depends on rate

`AJ::bulkRead` reads `m_pcMidiInPipe`; completion calls the handler at `AJ+0x30`, chosen in
`AJ::streamingStarted` (@0x25b2) for Ploytec high-speed devices (case 1):

| Rate | Handler | Frame |
|------|---------|-------|
| 32k / 44.1k / 48k | `inACR_M4_C1` | 4 MIDI slots + control byte (`putFrame(buf, 4)`) |
| 88.2k / 96k | `inACR_M1_C1` | 1 MIDI slot + control byte (skips the frame if the slot is FD) |
| 88.2k / 96k, pipe at `PGDevice+0xa90` interrupt with wMaxPacketSize 0x400 | `inACR_M2` | 2 MIDI slots |

`MidiIO::putFrame` drops every 0xFD. `PGDevice+0xa90` is taken to be the PCM IN pipe
(inferred: the DB4 fw 1.4.1's EP 0x86 is interrupt 1024 and its 96k frames are 2 bytes).

Measured on DB4 fw 1.4.1 (Linux, usbmon):
- 48 kHz: 5-byte transfers, `FD FD FD B0 FF`, `FD FD B0 25 FF` (byte 5 always FF)
- 96 kHz: 2-byte transfers, `FD B0`, `FD 25` (one MIDI byte per transfer in practice)

Before 2026-10-12 Ozzy passed whole transfers on: apps got FD (undefined real-time) and FF
(**System Reset**) at 48k. On macOS at 96k the DB4 looked clean (no control byte at 96k),
but a DB2 (48k only) would have sent FF to apps.

## Open

- What the control bytes mean (out: `c | 0x18`; in: always FF so far).
- Whether the 4D (#19) / DB2 (#88) MIDI out problems were the pacing / control byte.
