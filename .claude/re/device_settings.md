# Device settings: plan and facts (clock source, digital I/O, routing)

Status: plan, nothing implemented. Written 2026-10-17 at the end of the device-list work; start here.

## Goal

Expose the settings the official Ploytec driver offered (its control panel / user client) through
the tooling the OS already has, with one shared model in `common/`, and a small app only for what
the OS can't show. Order: external clock first (it's what users need, and it builds the plumbing).

## Design

1. **Shared settings model** in `common/` (pure C, like `ozzy_devices.h`): per device, which settings
   exist (from the quirk flags / plist flags), their values, and the control requests each value
   sends. Protocol logic lives here once; platforms only map it.
2. **Native controls per platform:**
   - Linux: ALSA kcontrols (enum / bool / volume). snd-usb-audio's names where they exist ("Clock Source").
     Mixer modes that change channel counts: PipeWire card profiles (pavucontrol "Configuration").
   - macOS: CoreAudio properties in OzzyHAL; the kext only receives the value and sends the requests.
   - Windows later: KS properties / an app.
3. **App later**, on the same interfaces, for what has no OS home.

| Setting | Linux | macOS | Windows |
|---|---|---|---|
| Clock source (internal / S/PDIF) | enum kcontrol "Clock Source" | `kAudioDevicePropertyClockSource` (Audio MIDI Setup menu) | app |
| Follow the locked rate | limit hw_params to the lock rate | offer only the lock rate (the official driver did this) | driver |
| Lock status | read-only bool kcontrol | no standard UI: app (or clock source item name) | app |
| S/PDIF mode consumer/pro | enum kcontrol | no natural fit: app | app |
| Digital out source (US-16xx, ESU1808 pair) | enum kcontrol | output data source (**verify Audio MIDI Setup shows it** for a 3rd-party HAL) | app |
| Input routing (US-144MKII, US-600) | enum per pair | input data source (same check) | app |
| US-3xx mixer modes | PipeWire profiles | app | app |
| Reverb / level processors | volume / enum kcontrols | app | app |
| Firmware update (#21) | app | app | app |

## Facts from the official driver (EIE kext 3.3.17 decompile `AKAI_EIE/AKAI_EIE_kext_all.c`, DB2 `DB2_decompile/XONE_DB2_kext_all.c`)

**Clock source = bit 1 of 'I' wIndex 0** (generic `updateAjInputSelector`): setting AjExtData[1] = 0
→ write `b | 0x02`, else `b & ~0x02`; read back as `AjExtData[1] = !(b >> 1 & 1)`. Ozzy's
`hs_selector` writes the default (bit 1 set). Which value is "internal" vs "S/PDIF" is **inferred**
(0 = default = internal). `setClockSource` (user client): suspend streaming, `updateAjInputSelector`,
resume. ESI U-series: `updateAjInputSelector_Esu186` states 2/1/0 → 0x32/0xB0/0xB2 plus 'A' bit 0
(`ploytec_vendor_request_I.md`); which state is which source is open.

**Digital lock** = bit 2 of 'I' wIndex 0 (`ploytec_protocol.h`: `(b & 0x05) == 0x04`). In
`updateAjInputSelector` (EIE kext ~L13916-13976), when locked, up to 20 tries: GET_CUR (0xA2/0x81,
wValue 0x0100, wIndex 0, 3 bytes) into AjExtData[0]; if it isn't 192000 the current rate becomes the
lock frequency; at 192000 it checks for a 192k alt setting, sleeps 10 ms, retries. While locked,
CoreAudio is offered only the current rate (`ploytec_sample_rates.md`). Tail: GET_CUR; if the device
rate differs from the current one, sleep 50 ms and SET_CUR to the in pipe.

**Lock is forced off** (AjExtData+0x28 = 0) when: no plist (`0x7d7 == 0`: all fallback devices,
the ESI and Xone-protocol devices without plist), plist flag 0x7a0 ("no digital input": DB2, DB4,
Jockey 3 ME, US-200, Lexicon IO22, Numarks, Alesis), or 15E4:0079. **So the Xones can't test this.**
Devices with digital in per their plists: Tascam US-144/MKII/600/1641/1800/2000, Lexicon IO42/IO82,
MPC Renaissance, M-Track Quad (check), plus ESI via its own path.

**kClockSourceSettings** (20 bytes, Tascams): copied to PGDevice+8/9/0xa by `initSpecificFeatures`,
only reported to user space (dispatch 0x0E); never sent to the device; wiped by
`initDeviceSpecificData` anyway.

**S/PDIF out** ('A' 0x0D..0x11, `writeDigitalOutMode`): mode 0 implemented (`hs_spdif_out`); mode 1
values in `ploytec_vendor_requests.md`. **Digital out selector** ('I' wIndex 2 = AjExtData[4], 0..1):
implemented at 0. **ESU1808 mirror pair**: AjExtData+0x10 selects `dmaEncode10_12/34/56/78`; Ozzy
has pair 1/2 (`ploytec_spdif_mirror_12`).

**Routing**: US-144MKII `_postprocessUS144_00/10/11` and US-600 `_postprocessUS600_<idx>` (out pair k
= in pair r_k), output `pcmTo24LSB_US144_*` (AjExtData[5..9], user client dispatch 0x28); details in
the Tascam section of `device_inventory.md`.

## Open before coding

- Where settings persist (official: AjExtData in the kext's data block, reloaded per session?).
- macOS: kext ↔ HAL path for a setting (SHM field + doorbell, like the rate change).
- Linux: kcontrol put while streaming → the official suspends/resumes; Ozzy's rate change already
  does a device reset (`ploytec_reset`), reuse that.
- Confirm the clock source bit mapping on a device with digital in (capture of the official driver
  switching source, or a tester).
