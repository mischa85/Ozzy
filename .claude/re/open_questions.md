# Ploytec RE — Open Questions and Leads

## Open Questions

### Status 'I' idx0 bit 7: DB4 0x92 vs DB2 0x12
Not USB speed. The official driver preserves it but never interprets it for A&H devices.
Meaning unknown. Ozzy logs it as `[b7:x]`.

## Decided / not needed

- **SET_CUR count**: the official macOS driver sends 5, Windows 7, Ozzy 2. Decision (project owner):
  2 is enough; the 5/7-call sequence is not needed.
- **Isoc keep-alive on EP 0x02**: the official driver runs it in bulk mode only. Decision (project
  owner): not needed for Ozzy.
- **Output packet layout**: Ozzy now picks it from the OUT endpoint's wMaxPacketSize (≤512 → bulk
  layout), on both macOS and Linux. That fixes the DB2 fw 1.2.0 (interrupt, 512).
- **DB2 sample rate**: Ozzy now runs the DB2 at 48 kHz only (macOS fixed rate; Linux rate table).

## GitHub claims audit (DB2, issue #72 / PR #70 / PR #83)

Checked against the official v3.4.9 driver and the DB2 Windows capture (2026-10-10):

| Claim | Source | Verdict |
|-------|--------|---------|
| DB2 needs 48 kHz; 96k plays at half speed | #70, #72 | **Confirmed.** Official plist table = 48000 only; endpoints can't carry more than 64 kHz in |
| DB2 only supports 44.1 kHz | #72, #83 description | **Wrong.** Boots at 44.1k, but the official table and the Windows driver use 48k only. (#83's code actually uses 48k) |
| Interrupt sub-packet is 512, not 482 | #70, #83 | **True for DB2 only.** The layout follows wMaxPacketSize (512 vs 964), not transfer type. Applying it to all interrupt devices breaks DB4 fw 1.4.1 / 4D |
| 7× SET_CUR needed or the PLL won't lock | #70, #72 | **Not needed** (owner decision). Official macOS sends 5; tested together with the framing fix, so the cause wasn't isolated |
| Channel layout UseChannelDescriptions silences stereo | #70 | **Unproven, unlikely.** Same HAL works on DB4. Not device-specific |
| willDoInPlace=true leaves zeros in the ring buffer | #70 | **Wrong.** Ozzy reads `ioMainBuffer` on WriteMix, which is the documented in-place contract. Same HAL works on DB4 |
| hostTime uninitialised at start | #70 | **Minor, plausible.** Only affects the window before the first USB completion; not DB2-specific |
| Residual distortion = SoundSource | #72 | Environmental, not a driver bug |
| `[FullSpeed]` despite High Speed enumeration | #72 title | **Log label bug.** Status bit 7 isn't USB speed; relabelled `[b7:x]` |
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
- **SET_CUR while streaming is not answered** (DB4 fw 1.4.1, Linux usbmon capture 2026-10-11,
  `captures/db4_linux_18d3e0a/`). `ozzy_pcm_set_rate` sends SET_CUR 48000 → EP 0x86 while the
  PCM URBs run; the control transfer never completes and is killed after ~2.1 s (`-ENOENT`).
  The USB reset that follows makes the post-reset handshake set the rate anyway, so that SET_CUR
  is dead weight that only adds 2 s (removed in 3ff2089; verified on DB4: reset now follows in
  13 µs). The official driver only sends SET_CUR from the start
  path (sample_rates.md §5), i.e. before streaming. Related to PR #86's live rate change.
- **Xone:4D does not survive the Linux rate-change USB reset** (reported in PR #86, 4D fw 1.4.1,
  not verified here; the DB4 survives it). After `usb_reset_device` descriptor reads time out
  (-110) and the mixer needs a power cycle; WirePlumber probing at another rate is enough to
  trigger it. #86 proposed a live rate change instead: drain in-flight URBs, kill them, SET_CUR,
  clear the PCM endpoint halts, resubmit. Its author also saw that killing a URB mid-packet makes
  the 4D drop the next control transfer (hence draining first). Not in `main`; #86 was closed
  with a request for a separate PR. Open: does the official driver ever reset the device on a
  rate change, or always stop/SET_CUR/restart (the start path in sample_rates.md §5 suggests the latter)?
