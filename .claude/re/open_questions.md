# Ploytec RE — Open Questions and Leads

## Open Questions

### SET_CUR count: 2 (Ozzy) vs 5 (official macOS) vs 7 (Windows)
The official macOS driver sends 5 SET_CURs + GET_CUR + 50 ms sleep on stream start (see
`ploytec_sample_rates.md` §5). Ozzy sends 2 and works on the DB4. Nobody has tested the DB2
with only the framing fix and 2 calls. Matching the official 5-call sequence is cheap and safe.

### Isoc keep-alive on EP 0x02 (bulk-mode firmware only)
The official driver streams isoc keep-alive on EP 0x02 when the IN or OUT PCM pipe is bulk
(DB4 fw 1.3.9, DB2 fw 1.0.8). Ozzy never does. Unknown whether bulk-mode devices need it.

### Status 'I' idx0 bit 7: DB4 0x92 vs DB2 0x12
Not USB speed (Ozzy's `[FullSpeed]/[HighSpeed]` log label is wrong). The official driver
preserves it but never interprets it for A&H devices. Meaning unknown.

## GitHub claims audit (DB2, issue #72 / PR #70 / PR #83)

Checked against the official v3.4.9 driver and the DB2 Windows capture (2026-10-10):

| Claim | Source | Verdict |
|-------|--------|---------|
| DB2 needs 48 kHz; 96k plays at half speed | #70, #72 | **Confirmed.** Official plist table = 48000 only; endpoints can't carry more than 64 kHz in |
| DB2 only supports 44.1 kHz | #72, #83 description | **Wrong.** Boots at 44.1k, but the official table and the Windows driver use 48k only. (#83's code actually uses 48k) |
| Interrupt sub-packet is 512, not 482 | #70, #83 | **True for DB2 only.** The layout follows wMaxPacketSize (512 vs 964), not transfer type. Applying it to all interrupt devices breaks DB4 fw 1.4.1 / 4D |
| 7× SET_CUR needed or the PLL won't lock | #70, #72 | **Unproven.** Official macOS sends 5. Tested together with the framing fix, so the cause wasn't isolated |
| Channel layout UseChannelDescriptions silences stereo | #70 | **Unproven, unlikely.** Same HAL works on DB4. Not device-specific |
| willDoInPlace=true leaves zeros in the ring buffer | #70 | **Wrong.** Ozzy reads `ioMainBuffer` on WriteMix, which is the documented in-place contract. Same HAL works on DB4 |
| hostTime uninitialised at start | #70 | **Minor, plausible.** Only affects the window before the first USB completion; not DB2-specific |
| Residual distortion = SoundSource | #72 | Environmental, not a driver bug |
| `[FullSpeed]` despite High Speed enumeration | #72 title | **Log label bug.** Status bit 7 isn't USB speed |
| Repeated disconnects (0xe00002ed) | #72 | **Open.** No root cause yet |

### Junction encoder vs our bit-interleaved codec
`pcmTo24Junction6CH` does **channel reordering** (0→0, 3→1, 1→2, 4→3, 2→4, 5→5) and
extracts 3 bytes from 4-byte int samples. It does NOT do bit-level interleaving.
Our codec (`ploytec_codec.h`) does bit-interleaved encoding. Need to verify these produce
the same wire format, or if our codec is wrong.

### Index 0 bits 3-7 (0x08–0x80)
- For product 0x644: bits 3-4 (mask `0x18`) come from `this[0xd2]`, rest preserved with `& 0xE7`
- ESU uses fixed composite states (0x32, 0xB0, 0xB2) rather than individual flags
- Bits 0-1 now understood (USB1 mode and clock source), but upper bits still device-dependent

### PGNoiseGenerator anti-tamper
`onServiceStart` uses a PRNG seeded with VID/PID to verify expected values. This is a
license/anti-clone check. Not relevant to our driver but interesting.

### AJ subsystem
`AJ::configurationDone`, `AJ::sendCPort`, `AJ::getLevelInfo` — purpose unclear. May be
related to level metering or some control protocol. Has its own pipe assignment via `AJ::assignPipes`.

## Resolved (moved from questions)

- ~~Vendor request 'A'~~ — Now understood: ESU clock config (wIndex=0x101) and Wolfson codec (wIndex=0x102/0x106)
- ~~Index 2 digital output selector~~ — Write-only, `AjExtData[4] & 0xFFFF`, only for devices with `this[0x7AC]`
- ~~setEsuCpldByte~~ — Writes to 'I' wIndex=1 with `| 0xE7` mask
- ~~this[0x1888] meaning~~ — USB 2.0 High Speed flag, NOT bulk device flag
- ~~wValue high byte~~ — Sign extension artifact from `(short)(char)byte` cast

## Leads

- **pcmTo24Junction8CH** — decompile to verify our 8-channel codec against the official encoder
- **USB::buildDummyDescriptor** — how the driver synthesizes USB audio descriptors for Ploytec devices
- **sendElektronChannelMap** — USB transactions for Elektron channel config (for Elektron support)
- **rtsProcessBulkOut** — the actual real-time audio processing loop (partially decompiled, shows MIDI embedding)
- **Real-time service functions**: `rtsBulkOutBulkIn`, `rtsInterruptOutInterruptIn` — the streaming state machines
- Windows driver RE for sample rate change pattern comparison
