# Device inventory: what Ploytec's driver (and its neighbours) covered

Built 2026-10-16 from four research passes over the official drivers. The detailed findings,
download locations and sources are in the investigation folder:
- `OEM_DRIVERS/findings_xone_protocol.md`: devices on the Xone protocol (`isPloytecBulkDevice`)
- `OEM_DRIVERS/findings_isoc_out.md`: bulk in / isoc out devices (`isPloytecBulkInIsocOutDevice`)
- `OEM_DRIVERS/findings_other_and_projects.md`: the other IDs in the kext, and other driver projects
- `OEM_DRIVERS/findings_dj_brands.md`: Rane, Numark, NI, Pioneer and other DJ brands

The official drivers for most devices below are downloaded and extracted (never run) under
`OEM_DRIVERS/<Vendor>_<Product>/`. "Plist" means the kext's `PtSpecific.kConfigurationData`
(decoded with `tools/ploytec_config_decode.py`, family flags at blob offsets 0x139 = Xone protocol,
0x13A = bulk in / isoc out). "Fallback" means the per-PID table in `PGDevice::findInterfacesInConfig`
(for kexts without a plist block); "spec" means a vendor spec sheet. Claims marked (?) are
unverified.

**Every device below has no working driver on Apple Silicon** (all official Mac drivers are Intel
kexts from 2020 or earlier). Linux status is per device.

## Families

| Family | Transport | Devices found | Ozzy today |
|---|---|---|---|
| Xone protocol | bulk/interrupt PCM out (junction frames, MIDI embedded), bulk/interrupt PCM in, MIDI in EP 0x83 | 20+ | `ploytec_ops` (DB4, DB2, DX, 4D, 2D, WZ4, Reloop DJ2 ME) |
| Bulk in / isoc out | isoc PCM out (plain S24) paced by a 3-byte isoc feedback EP, bulk PCM in (junction frames), MIDI on its own endpoints | 25+ | `ploytec_isoc_ops` / `PloytecIsocEngine` (EIE Pro) |
| Rane (not Ploytec) | isoc audio + 64-byte HID command channel | 7 | none |
| caiaq (not Ploytec) | NI's protocol | 6 | none (Linux has snd-usb-caiaq) |

The two Ploytec families cover nearly every licensee. Per-device differences are channel counts,
rates and a few vendor extras (listed per device).

## Xone protocol (`isPloytecBulkDevice`)

| VID:PID | Product | In / out | Rates | Source | Linux | Ozzy needs |
|---|---|---|---|---|---|---|
| 0A4A:FFDB/FFD2/FFDD/FF4D/FF01/FFAD | Xone:DB4/DB2/DX/4D/2D, WZ4 | see `common/ozzy_devices.def` | | plist | Ozzy | in Ozzy |
| 200C:1009 | Reloop Digital Jockey 2 ME | 6 / 4 | 44.1k | Reloop INF, PR #80 | Ozzy | in Ozzy |
| 200C:1019 | Reloop Jockey 3 Master Edition | 6 / 4 | 44.1–96k | plist (flags = DB2/DB4, kInACR 1) | fvdpol/alsa-jockey3 (GPL, out of tree; upstream patch pending) | in Ozzy (experimental); no AJ selector requests (`updateAjInputSelector` returns early for it) |
| 200C:1037 | Reloop Jockey 3 Remix | 6 / 4 | 44.1–96k (fvdpol) | fallback | fvdpol/alsa-jockey3 | in Ozzy (experimental) |
| 200C:1005 | Reloop IQ2 MIDI | 4 / 4 | 24/96 (spec) | fallback | none | in Ozzy (experimental) |
| 200C:100A | Reloop Contour Interface Edition | 2 / **10** | 44.1k boot rate, others ? | fallback | none | in Ozzy (experimental, 44.1k only; 10-lane encoder `dmaEncode10` added) |
| 200C:1006 | unidentified Reloop | 6 / 6 | | fallback | | identify (#93) |
| 0A92:0111 | ESI ESU1808 | 16 / 8 (**10-lane** output: S/PDIF mirrors a selectable pair) | 44.1–96k (spec) | fallback | none | in Ozzy (experimental): CPLD byte, S/PDIF mirror of pair 1/2 (the default); the pair selection (`_34/_56/_78`) is not offered |
| 2573:0012 | ESI U168 XT | 16 / 8 | 44.1–96k (spec) | fallback | none | in Ozzy (experimental): ESI selector, 'A' requests (0x349, then 0x348), CPLD byte; ESI's own 2.0.0b90 beta runs on Apple Silicon (user-space, analog only, macOS ≤ 14.3) |
| 2573:0013 | ESI U86 XT | 8 / 6 | 44.1–96k (spec) | fallback | none | in Ozzy (experimental): ESI selector, 'A' 0x348, CPLD byte; 6-lane output (`dmaEncode06`) added |
| 2573:002A | ESI U108 PRE | 10 / 8 | 44.1–96k (spec) | fallback | none | in Ozzy (experimental): as the U86 XT; 10-lane input |
| 1ACC:0103 | Midiplus AudioLink PLUS 4x4 ("Plus II") | 6 / 6 | 24/96 (spec) | fallback | none | in Ozzy (experimental; no device-specific requests); class-compliant mode? (?); Interspock's Ozzy fork adds it |
| 1ACC:0102 | unidentified Midiplus | 4 / 4 | | fallback | | identify (#93) |
| 1D03:005C | unidentified iCON | 2 / 2 | 44.1k | fallback | | identify (#93) |
| 0A4A:5A16 | Smyth Research Realiser A16 | 2 / 16 | 44.1k boot rate, others ? | fallback (3.4.13 kext has no PtSpecific) | ? | in Ozzy (experimental, 44.1k only; 16-lane encoder `dmaEncode16` added) |
| 0A4A:D064 | D.O.Tec EXBOX.UMA (?) ("DOTEC" in the kext) | 16 / 16 | default 88.2k, channels vary with rate | fallback | adiknoth/exboxuma (GPL, 2015 alpha) | not added: packed 24-bit frames (`dmaEncode08/16/32_DOTEC`, not bit-interleaved), 32 / 16 / 8 channels at ≤48k / 88.2–96k / 176.4–192k, per-rate sub-packet counts (`bulkAudioRun`); needs per-rate channel counts and a new packet layout; no known owner |
| 0A4A:2416 | Ploytec JCT 24x16 reference board (?) | 24 / 16 | | fallback | | probably not a product |
| 0A4A:DA01 | unidentified (A&H ICE-16?) | 16 / 16 | | fallback | | identify |
| 0A4A:CF00, 1016 | unidentified | 2 / 2 | | fallback | | identify (#93) |
| 0A4A:AFFE, CF04, 0003, 6A02, F1A0 | unidentified | | | not in fallback | | identify (#93) |
| 0A4A:AA01/AA06/AA09/AA11 | Reloop Digital Jockey IE, Digital Jockey 2 IE, Spin 2, 10tion | ? | | older kexts, no plist | none | check family: descriptor dumps (#92) |
| 200C:1017, 100B, 1030 | Reloop Mixage IE, Play, Play 24/96 | ? | | not on the bulk list | none | check family (#92) |

Not Xone protocol despite Ploytec's VID or driver: SPL Crimson (0A4A:C150; flags 0/0, reportedly
UAC2 class compliant, Ploytec driver needed for 176.4/192k), Ploytec's generic `PGUSBAudio.kext`
(113 class-compliant devices, plus 0A4A:0002 Audio Junction).

## Bulk in / isoc out (`isPloytecBulkInIsocOutDevice`, the EIE path)

All have flags 0x139 = 0, 0x13A = 1 where a plist exists. Mainline Linux's new us144mkii driver
(6.18) uses exactly this endpoint map for the US-144/US-144MKII (EP 0x81 3-byte feedback, 0x02
isoc out, 0x86 bulk in with junction frames, 0x83/0x04 MIDI, alt 1 on interfaces 0 and 1),
independently confirming Ozzy's isochronous path.

| VID:PID | Product | In / out | Rates | Source | Linux | Ozzy needs |
|---|---|---|---|---|---|---|
| 09E8:0010 | Akai EIE Pro | 4 / 4 | 44.1–96k | plist | out-of-tree (EmaP77 and others) | in Ozzy (experimental) |
| 09E8:207F | Akai MPC Renaissance ("ACV0") | 4 / 4 (2 real inputs) | 44.1–96k | plist | none | in Ozzy (experimental) as 2 in / 4 out, first MIDI port only; second MIDI port (bytes 4..7 of the same 9-byte transfer, both directions; needs a second port end to end) and the bcdDevice 0x0300 isoc in + isoc out variant missing |
| 0763:4002 | M-Audio M-Track Quad | 4 / 4 | 44.1–96k | plist (flags closest to the EIE; setFreqOnOutPipe) | none (?) | in Ozzy (experimental) |
| 0644:800F | Tascam US-144 | 4 / 4 | 44.1–96k | plist | snd-usb-us122l; us144mkii (6.18) | in Ozzy (experimental); left out of the Linux build by default |
| 0644:8020 | Tascam US-144MKII | 4 / 4 | 44.1–96k | plist | us144mkii (6.18) | in Ozzy (experimental); left out of the Linux build by default |
| 0644:8034 | Tascam US-200 | 2 / 4 | 44.1–96k | plist | none | in Ozzy (experimental); 100 ms wait, no digital I/O |
| 0644:8035 | Tascam US-600 | 6 / 4 | 44.1–96k | plist | none | in Ozzy (experimental) |
| 0644:8017 | Tascam US-1641 | 16 / 4 | 44.1–96k | plist | none | in Ozzy (experimental); digital out selector |
| 0644:8025 | Tascam US-2000 | 16 / 4 | 44.1–96k | plist | none | in Ozzy (experimental); digital out selector |
| 0644:8030 | Tascam US-1800 | 16 / 4 | 44.1–96k | plist | none | in Ozzy (experimental); digital out selector |
| 0644:801E | Tascam M-164UF (mixer) | ? | | 2.0 kext, no plist | none | captures |
| 0644:8040, 8041 | Tascam US-322, US-366 | ? | | US3XX kext 3.4.13 (Ploytec-signed), no plist | none | descriptors and captures (#91); US-366 4/6-input post-processor |
| 0644:8046, 8048 | Tascam US-1200, UH-7000 (?) | ? | | kext strings | none | descriptors and captures (#91) |
| 1210:0018 | Lexicon I-ONIX U42S / IO42 | 6 / 4 | 44.1–96k | plist | none (?) | in Ozzy (experimental); S/PDIF clock source missing |
| 1210:0019 | Lexicon I-ONIX U82S / IO82 | 10 / 4 | 44.1–96k | plist | none (?) | in Ozzy (experimental; 10-lane decoder added); S/PDIF clock source missing |
| 1210:001A | Lexicon I-ONIX U22 / IO22 | 2 / 2 | 44.1–96k | plist | none (?) | in Ozzy (experimental) |
| 13B2:0030 | Alesis MultiMix 8 / 8 FX / 16 USB 2.0 | 4 / 4 per plist, 10 / 2 per spec | 44.1/48k per plist, up to 96k per spec | plist vs spec conflict | slash909uk JACK hobby project | descriptor dump (#90) |
| 0562:03EB | Dynacord PowerMate-3 / CMS-3 (600/1000/1600/2200) | 4 / 4 (spec, Bosch) | up to 96k | no plist | none (HW:03 revision is class compliant) | not added yet. From the driver: isoc family at High Speed, 4 / 4 24-bit, 44.1k boot, MIDI forced to `M4_C1`. Open: does the class-compliant HW:03 revision share 0562:03EB (a row would take it from the class drivers)? dominics/Ozzy (MIT) has captures and support (#15) |
| 15E4:0071 | Numark NS7 | 4 / 4 | 44.1k | plist (kInACR 8) | none | in Ozzy (experimental, NS7 MIDI framing) |
| 15E4:0075 | Numark V7 | 4 / 4 | 44.1k | plist | none | in Ozzy (experimental, NS7 MIDI framing; tejofjord/openv7, MIT, documents it) |
| 15E4:0079 | Numark NS6 | 4 / 4 | 44.1k | plist (kInACR 8) | none | in Ozzy (experimental, NS7 MIDI framing) |
| 15E4:0040 | Numark MixDeck | 4 / 4 | 44.1/88.2k | plist (kInACR 2) | none | in Ozzy (experimental) |
| 15E4:0026 | Numark 4Trak | 2 / 4 | 44.1k | plist (kInACR 8) | none | in Ozzy (experimental, NS7 MIDI framing) |
| 15E4:0074 | Numark Omni Control (#93, lsusb) | 4 / 4 | 44.1–96k | Windows 2.9.13 nkc2_usb.sys (generic Ploytec build; `OEM_DRIVERS/Numark_OmniControl/findings.md`) | none | in Ozzy (experimental); MIDI IN is `M4_M4_C1` on Windows (Ozzy reads port 1); the driver takes the real counts from 'V' bytes 3/4 ×2, unverified on hardware |
| 15E4:002C, 0070, 0078 | further Numark PIDs in the EIE 3.3.17 kext (0070, 0078 get the Omni Control's path on Windows) | ? | | kext list | | identify (#93) |
| 0A4A:5B01 | Musonik (ex-Terrasoniq) PHASE X64 USB (#93, lsusb) | 6 / 4 (4 analog + 5/6 a digital tap of Out 1/2, hardware-tested by others) | 44.1–96k | kext 2.0.4 (`PGKernelDeviceDrvPHASEX64`, hard-coded, no plist table), Windows 2.9.34 (`OEM_DRIVERS/Musonik_PhaseX64/findings.md`) | none | in Ozzy (experimental): AJ_BIT0, DIGITAL_IN, SPDIF_OUT (kext; Windows omits it), 1000 ms wait (Windows 600 ms), WM8776 defaults ('A' wValue reg<<9\|val, wIndex 0x0102 / 0x0106: Mic/Line on 1/2, Aux on 3/4, no direct monitoring). The codec switches are settings (Input 1-2 / 3-4 Source, Output Playback / Mono / Direct / Sound Module; `device_settings.md`). Missing: the sound module (AIN4) itself, Windows' second MIDI port. No RIAA for Phono by design (official driver: kernel on Windows, HAL on Mac; Ozzy leaves it to user space). Power-on codec state unknown. A Digital Out -> In loopback cable locks the clock to 32 kHz (`OEM_DRIVERS/Musonik_PhaseX64/findings.md` §7) |
| 0A4A:0014 | unidentified | ? | | kext list | | identify (#93) |

Tascam setup, from the official generic PGDevice code (EIE kext decompile; the us144mkii driver
sends the same values, facts only):
- 'A' 0x41, wIndex 0x0101, registers 0x0D..0x11 = 04 00 00 rate 0B: `writeDigitalOutMode` in mode 0,
  the S/PDIF output channel status (rate code 44.1k 0, 48k 2, 88.2k 8, 96k 0xA in the low nibble of
  0x10). Read first (0xC0, 5 bytes), each register written only if it differs, in `startStreaming`
  after the first SET_CUR pair. Only for plist flag 0x7a6: every Tascam but the US-200, and the MPC
  Renaissance (`OZZY_QUIRK_SPDIF_OUT`).
- 'I' wIndex 0: the generic `updateAjInputSelector` (set bit 1; clear bit 0 on flag-0x7a8 devices;
  write only on change, then 40 ms and GET_CUR), and the confirm `setAJDMAInputChannels` (bits 5 and
  4 = 0x30) at every stream start. us144mkii's 0x0010/0x0030 fit that.
- 'I' wIndex 2 = 0: `writeDigitalOutSelector`, flag 0x7ac (US-1641/1800/2000).
- Plist kMsWait 100 for all, but `initDeviceSpecificData` clears it before `configurationDone`; only
  the US-200 waits (100 ms, hard-coded). kInACR 3 (M4_C1 MIDI), like the EIE.
- PCM: output plain 24-bit (`pcmTo24LSB`), input the 16-lane junction decoder. The US-144MKII /
  US-600 routing post-processors and `pcmTo24LSB_US144_*` (US-144MKII, US-600, US-200) are pair
  routing, off at the defaults; Ozzy has them as settings (`device_settings.md`).
- 'D' (0x44) = deep sleep (us144mkii).

MIDI framing on the bulk in / isoc out path (EIE kext `AJ::streamingStarted` @0x2e94, `kInACR`
at `PGDevice+0x810`; `AJ::configurationDone` sets the transfer size):

| kInACR | MIDI IN handler | Transfer | Devices |
|---|---|---|---|
| 0 | `inACR_Reg` | | |
| 1 | Xone rule: `M4_C1` at ≤48k, `M1_C1` (or `M2` on a 1024-byte interrupt pipe) at 88.2/96k | 9 | Reloop Jockey 3 ME; Xone-protocol devices with 0xFF also land here |
| 2, 3 | `inACR_M4_C1`, one port | 9 | MixDeck (2), EIE, M-Track Quad, Lexicon (3) |
| 8 | `inACR_NS7`: 41 MIDI bytes + control byte at 41 | 0x2a = 42 | NS7, V7, NS6, 4Trak; out pattern `initNS7` = 39 bytes per transfer |
| 0xFF (none) | `inACR_M4_M4_C1`, two ports (second at offset 4) | 9 | MPC Renaissance; overrides: Dynacord 0562:03EB and 170B:0020/0021 → `M4_C1`, Tascam 8040/8041 → `inACR_US366`, 8046/8048 → `inACR_UH7000`, Dotec 0A4A:D064 → its own |

Ozzy implements `M4_C1` (default) and NS7 (`OZZY_QUIRK_MIDI_NS7`, `common/devices/ploytec/ploytec_midi.h`);
for `M4_M4_C1` it reads the first port only. MIDI out uses the 4,3,3,3,3,3,3,3 pattern (3125 bytes/s,
31250 baud) for every framing, NS7 included: the official driver's 39 bytes per transfer would be
about 39000 bytes/s.

## Not Ploytec, still candidates

| Device | VID:PID | Protocol | macOS | Linux |
|---|---|---|---|---|
| Rane SL2, SL3, SL4, Sixty-One, Sixty-Two, Sixty-Four, Sixty-Eight | 1CC5:0013/0001/000F/0011/0009/0017/0004 | Rane in-house: isoc audio + 64-byte HID commands (rate = HID cmd 0x31, BE u16) | dead since 10.14/10.15 | SL3 only: nvgeele/snd-rane-sl3 (GPL) |
| NI Audio 4 DJ, Audio 8 DJ, Traktor Kontrol S4 MK1, Audio 2 DJ, Traktor Audio 2 MK1, Audio Kontrol 1 | 17CC:0839/1978/BAFF/041C/041D/0815 | caiaq | dead since 10.15 | snd-usb-caiaq (in tree) |

Notes: Serato DJ Pro 2.5.6+ refuses the Rane SL boxes regardless of driver. Newer Rane, Denon DJ
and Allen & Heath (Xone:K/23C/43C/96/PX5) products are class compliant or supported.

## Other IDs in the Ploytec kext that are not licensees
Mostly workarounds for quirky devices that Linux already handles: Digidesign Mbox 1/2, M-Audio
Audiophile USB (TUSB3200 codecs), Creative Audigy 2 NX / Live! 24-bit, Steinberg MI2/MI4, Novation
ReMOTE/Speedio, Access Virus TI (likely licensed, `pcmTo24Access`), C-Media, Elektron Overbridge 1
(now Elektron's own Overbridge 2 and dagargo/overwitch), Yamaha UW500/EOS BX (UAC1 audio, MIDI
quirk). Details in `findings_other_and_projects.md`.

## What the inventory says about Ozzy's architecture
- Two families cover nearly everything: most devices are a model row (channels, rates).
- **Missing pieces, in order of how many devices need them:**
  1. Per-device handshake extras: done as quirks (`common/ozzy_quirks.h`, Tascam S/PDIF 'A'
     registers, generic selector, waits, digital out selector). Still missing: following an
     external (S/PDIF) clock, Lexicon/ESI clock source (Ozzy fixes the ESI selector at STATE_A).
     The ESU1808 gets only the CPLD byte (`OZZY_QUIRK_ESI_CPLD`): ESI's selector and 'A'
     request are VID 0x2573 only.
  2. Frame codecs: done for every bit-interleaved count the driver has (output 2/4/6/8/10/16 and the
     ESU1808 mirror, input 2/4/6/8/10/16). Not done: the Dotec packed codecs.
  3. Input post-processing quirks (MPC Renaissance zeroes inputs 3/4, US-366).
  4. Rarer: isoc in + isoc out (MPC Renaissance bcdDevice 0x0300), rate-dependent channel counts
     (Dotec, SPL Crimson), `kMsWait` 200 (NS7, NS6, MixDeck; not in the V7/4Trak plists) is a 200 ms sleep at the start of
     `configurationDone`, before any setup request: done (`OZZY_QUIRK_WAIT_200MS`).
- The central device list `common/ozzy_devices.def` (one row per device: VID, PID, name, family,
  in/out, rates, status, quirks) is where a device is added; the Linux ID table, the
  kext's personalities and the engine choice are generated from it.

## Naming correction
200C:1009 is the **Digital Jockey 2 Master Edition** (Ozzy's name; Reloop's INF and PR #80) and
200C:1019 the **Jockey 3 Master Edition** (Reloop's Jockey 3 ME kext). The upstream Jockey 3 patch
v3 (June 2026) used 0x1009 for the Jockey 3 ME, which this suggests is wrong (?).

## Open questions
- Identify: 1ACC:0102, 1D03:005C, 200C:1006, 0A4A AFFE/CF00/CF04/DA01/0003/1016/6A02/F1A0/0014,
  0644:8048, 0644:8012 ("US-144 Pro"), the extra Numark PIDs.
- Decode the remaining flag bytes 0x143, 0x145, 0x146, 0x14a, 0x14b, 0x14e.
- Alesis MultiMix real format; Dynacord original revision layout.
- Is `ah4d_akm_v132.bin` (in the Jockey 3 firmware updaters) Xone:4D firmware 1.32?
- ESI's 2.0.0b90 user-space driver: its init sequence and why "analog only".
