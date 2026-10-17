# Device settings: plan and facts (clock source, digital I/O, routing)

Status: clock source and S/PDIF out mode implemented 2026-10-17 (untested on hardware: no device
with digital I/O); the other settings are still plan. Written 2026-10-17 at the end of the device-list work; start here.

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
| Clock source (automatic / internal) (done) | enum kcontrol "Clock Source" | `kAudioDevicePropertyClockSource` (Audio MIDI Setup menu) | app |
| Follow the locked rate | limit hw_params to the lock rate | offer only the lock rate (the official driver did this) | driver |
| Lock status | read-only bool kcontrol | no standard UI: app (or clock source item name) | app |
| S/PDIF mode consumer/pro (done) | enum kcontrol "IEC958 Playback Mode" | no natural fit: kext path ready, needs the app | app |
| Digital out channels (US-16xx/US-2000: output routing; ESU1808: mirror pair) | part of routing (encoder side) | output data source (**verify Audio MIDI Setup shows it** for a 3rd-party HAL) | app |
| Input routing (US-144MKII, US-600) | enum per pair | input data source (same check) | app |
| US-3xx mixer modes | PipeWire profiles | app | app |
| Reverb / level processors | volume / enum kcontrols | app | app |
| Firmware update (#21) | app | app | app |

## Clock source: how it is built

- **Model** (`common/ozzy_settings.h`): `ozzy_setting_descs[]` (name, value names, default, the
  quirk that enables it), `struct ozzy_settings` (the user's values), `struct ozzy_clock_status`
  (lock, lock rate). Devices with `OZZY_QUIRK_DIGITAL_IN` have it: MPC Renaissance, Lexicon
  IO42/IO82, Tascam US-144/MKII/600/1641/1800/2000. Not ESI (3-state selector, open mapping), not
  the M-Track Quad (its plist flags weren't decoded for this; it has no S/PDIF jack).
- **Values** (from Tascam's US-1641/1800/2000 Control Panel 3.22/3.25, `OEM_DRIVERS/Tascam_*`):
  its Sample Clock Source menu is [Automatic, Internal] (nib `MainMenuUS1641.nib`), and
  `SampleClockSource(long)` passes the menu index straight to `USBDevice::setClockSource`, i.e.
  ClockSource (AjExtData[1]) 0 = **Automatic** (the default: the digital input when it carries a
  valid signal, else internal, per the panel help), 1 = **Internal** (always). With the decompile
  (0 → bit 1 set): bit 1 set = automatic, clear = internal. The panel saves it as "ForceInternal".
  (Ozzy first shipped this inferred the other way round, a6f2070; fixed.)
- **Protocol** (`ploytec_handshake.c`): `hs_selector` sets bit 1 from the clock source with
  `ploytec_aj_clock_byte()`. On `OZZY_QUIRK_DIGITAL_IN` devices with Automatic, `hs_follow_lock`
  re-reads the selector and, when locked, the rate (up to 20 tries while it reads 192000, as the
  official driver). Locked: the handshake sets and verifies the lock rate instead of the requested
  one; `info->rate` / `info->clock` say what it did. A device clocked at a rate it can't stream
  (192k) fails the handshake's rate verify. So at the defaults these devices now follow an S/PDIF
  lock, like the official driver; devices without digital in send the same requests as before.
- **Linux**: `chip->settings` / `chip->clock` (kept across resets), `linux/ozzy_controls.c`: enum
  control "Clock Source" and read-only "S/PDIF Clock Validity". Names as snd-usb-audio builds
  them (`sound/usb/mixer.c` v6.18, copy in the investigation folder's `linux_ref/sound_usb/`):
  a UAC2 clock selector is "<selector name> Clock Source", a clock source's validity a read-only
  mixer boolean "<clock source string> Validity". A change runs
  `ozzy_pcm_reconfigure`: xrun on running substreams, then the USB reset whose post-reset handshake
  sends it. While locked a hw_params rule allows only the lock rate and prepare rejects other
  rates. `alsactl store/restore` persists the value. Also fixed on the way: `ozzy_pcm_init_urbs`
  ends the panic of the pre-reset abort (before, a reset outside prepare left PCM refusing to open),
  and `ploytec_reset` takes the device lock (`usb_lock_device_for_reset`).
- **macOS**: there was no rate-change path (the kext ran the highest rate, the HAL offered only
  it). Now: the HAL publishes a clock source selector control object (4th object per device slot,
  `kAudioClockSourceControlClassID`; CoreAudio derives `kAudioDevicePropertyClockSource` from it,
  per AudioHardware.h). Setting it → `RequestDeviceConfigurationChange` (action = id << 32 | value)
  → CoreAudio stops IO → `PerformDeviceConfigurationChange` → user client selector 1
  (`kOzzyMethodApplySetting`, in: id, value) → kext:
  `engine->Suspend()` (no resubmits), synchronous abort of every pipe, `engine->Start()` again with
  the setting; `hardwarePresent`/`driverReady` stay set so the HAL keeps the device. A lock rate
  the device doesn't list fails the start on both platforms (`ozzy_device_has_rate`). The engine
  publishes the result in `audio.settings` (SHM version 6); the HAL re-reads format and rate,
  stores the value with `WriteToStorage` (key "<device UID> Clock Source") and notifies. On attach
  the HAL sends a stored value the kext doesn't have before publishing the device. The selected
  Automatic item's name says "(S/PDIF, N Hz)" / "(internal, no S/PDIF signal)".
- **Still to check on hardware**: that the lock bit and GET_CUR behave as the
  decompile says; that Audio MIDI Setup shows the Clock Source menu for this plug-in; that a lock
  gained or lost while streaming is picked up only at the next change (selecting the same source
  again re-reads it on macOS; on Linux any reset does).

## S/PDIF out mode: how it is built

- `OZZY_SETTING_SPDIF_OUT_MODE` (Consumer / Professional = the official DigitalOutMode 0/1,
  `PtAjExtData::validate` clamps it to 0..1), on `OZZY_QUIRK_SPDIF_OUT` devices (MPC Renaissance,
  Tascams but the US-200). `hs_spdif_regs()` has both modes' registers (EIE kext
  `writeDigitalOutMode` @ 00012212). "Professional" is from IEC 60958: mode 1 sets bit 0 of
  channel status byte 0, the professional flag; the driver only says 0/1.
- The official `setDigitalOutMode` suspends streaming, writes the registers, resumes. Ozzy uses
  the same path as the clock source: Linux resets the device (heavier than the official, which
  only rewrites the registers, but one path), macOS restarts the engine via
  `kOzzyMethodApplySetting`. Mode follows rate changes because the handshake writes it after SET_CUR.
- Linux: "IEC958 Playback Mode" enum (ALSA's name for S/PDIF is IEC958). macOS: CoreAudio has no
  property for it, so no UI yet; the HAL restores a stored value, and the app can set it through
  the user client.

## Facts from the official driver (EIE kext 3.3.17 decompile `AKAI_EIE/AKAI_EIE_kext_all.c`, DB2 `DB2_decompile/XONE_DB2_kext_all.c`)

**Clock source = bit 1 of 'I' wIndex 0** (generic `updateAjInputSelector`): setting AjExtData[1] = 0
→ write `b | 0x02`, else `b & ~0x02`; read back as `AjExtData[1] = !(b >> 1 & 1)`. Ozzy's
`hs_selector` writes it from the setting: 0 = Automatic (default), 1 = Internal (Tascam panel, see
above). `setClockSource` (user client): suspend streaming, `updateAjInputSelector`, resume. ESI U-series: `updateAjInputSelector_Esu186` states 2/1/0 → 0x32/0xB0/0xB2 plus 'A' bit 0
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
implemented at 0. It is *not* the Tascams' "Digital Output Channels" (1&2 / 3&4) choice: the
US-1641 Control Panel 3.22 does that with `setRouting(false, 1, pair)` (output routing,
AjExtData[5..9]), and calls `USBDevice::setDigitalOutSelector` only from `Usb11Mode(long)`, next
to an "analog / digital" menu (`doUsb1ModeInputSelection`); the help says USB 1.1 is unsupported
on these devices. So no setting for it. `setDigitalOutSelector` also accepts 0..1 on ESI
2573:0012/0013, but they have no flag 0x7ac, so nothing is written. **ESU1808 mirror pair**: the
same AjExtData[4] (0..3) picks `dmaEncode10_12/34/56/78` in `chooseDMAEncoder` (encoder side, no
request); Ozzy has pair 1/2 (`ploytec_spdif_mirror_12`).

**Routing**: US-144MKII `_postprocessUS144_00/10/11` and US-600 `_postprocessUS600_<idx>` (out pair k
= in pair r_k), output `pcmTo24LSB_US144_*` (AjExtData[5..9], user client dispatch 0x28); details in
the Tascam section of `device_inventory.md`.

## Open

- Lock changes while streaming: the official driver only reads lock in `updateAjInputSelector`;
  polling would need a test that an 'I' read during streaming is safe.
- ESI U-series clock source (states A/B/C), 192k lock handling (alt setting), and the remaining
  settings in the table above.
