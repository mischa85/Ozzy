# Ploytec RE — Open Questions and Leads

## Open Questions

### Sample rate change: 5-call dance vs official driver
Our driver sends SET_CUR 5 times alternating 0x86/0x05 (from Windows USB capture). The official
macOS driver only calls `setFrequency` once per pipe (2 total: input + output). The 5-call
pattern may be a Windows driver quirk, or redundant. Need to test if 2 calls works for all devices.

### Junction encoder vs our bit-interleaved codec
`pcmTo24Junction6CH` does **channel reordering** (0→0, 3→1, 1→2, 4→3, 2→4, 5→5) and
extracts 3 bytes from 4-byte int samples. It does NOT do bit-level interleaving.
Our codec (`ploytec_codec.h`) does bit-interleaved encoding. These likely produce different
wire formats, but `pcmTo24Junction8CH` (used for DB2/DB4 8-channel output) has not yet been
decompiled to confirm whether IT does bit-scatter or linear.

**Current status (DB2 debug session) — USB capture confirms bit-scatter IS correct:**
- `PloytecEncodePCM` matches Linux `ploytec_encode_frame` exactly (confirmed byte-for-byte).
- DB2 does NOT appear in the Linux driver's USB ID table — Linux is NOT a reliable reference.
- Windows USB capture shows audio data only in bytes 0x00-0x01 of each
  sub-packet — consistent with bit-scatter encoding where ch0 occupies bit 0 of every byte.
  This **confirms bit-scatter is the correct encoding** for DB2 output.
- Audio flows correctly into the HAL (peak ~0.1 confirmed with Glass.aiff), MIDI at byte 480.
- Root cause of silence was two mismatches vs Windows driver (both now fixed):
  1. Sample rate: we used 96000 Hz, Windows uses 48000 Hz.
  2. Interrupt sub-packet format: we used 482 bytes (no padding), device expects 512 bytes
     (bulk format: 480 PCM + 2 MIDI + 30 padding), confirmed by Windows URBs (14×512=7168).

### Missing configurationDone initialization steps (SUSPECTED ROOT CAUSE)
Our `Start()` handshake sequence is minimal vs the official driver's `configurationDone`:

Our sequence:
1. ReadFirmwareVersion
2. GetHardwareFrameRate / SetHardwareFrameRate
3. STATUS read-modify-write (set MODE5 bit)
4. Submit USB transfers

Official `configurationDone` also calls:
- `updateAjInputSelector` (twice — before and after encoder selection)
- `usbInitDeviceControls` + `usbApplyDeviceControls` (writes device state via vendor 'I' requests)
- `startStreaming` (may be needed to enable USB audio output at the hardware level)
- `AJ::configurationDone` (purpose unclear — AJ subsystem)

Without `usbApplyDeviceControls`, the device may not route USB audio to its physical
outputs. This is the most likely cause of the silence on DB2.

**What's needed:** USB capture of the official driver initializing a DB2, then diff
against our init sequence to find the missing vendor requests.

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
- ~~Interrupt sub-packet MIDI position~~ — MIDI at byte 480 (after 10 samples), same as bulk. NOT at 432.
- ~~wValue wrong for DB2~~ — `ploytec_confirm_wvalue` correctly sign-extends via `(uint16_t)(int16_t)(int8_t)modified`. DB2 status=0x12 → writes 0x0032 (not 0xFF32). Already fixed in `common/devices/ploytec/ploytec_protocol.h`.
- ~~DB2 interrupt sub-packet format~~ — Confirmed 512 bytes (bulk format: 480 PCM + 2 MIDI + 30 pad). NOT 482. Windows capture shows 14×512=7168-byte URBs on interrupt endpoint.
- ~~DB2 sample rate~~ — Windows driver sets 48000 Hz, NOT 96000 Hz. Device default is 44100 Hz. Fixed in `PloytecEngine::Start()` and `OzzyHAL`.
- ~~DB2 bit-scatter vs linear encoding~~ — Bit-scatter confirmed correct by capture: audio data only in bytes 0x00-0x01 per sub-packet, consistent with ch0 at bit 0.

## Leads

- **pcmTo24Junction8CH** — decompile to verify our 8-channel codec against the official encoder
- **USB::buildDummyDescriptor** — how the driver synthesizes USB audio descriptors for Ploytec devices
- **sendElektronChannelMap** — USB transactions for Elektron channel config (for Elektron support)
- **rtsProcessBulkOut** — the actual real-time audio processing loop (partially decompiled, shows MIDI embedding)
- **Real-time service functions**: `rtsBulkOutBulkIn`, `rtsInterruptOutInterruptIn` — the streaming state machines
- Windows driver RE for sample rate change pattern comparison
