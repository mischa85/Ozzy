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
| 200C:1019 | Reloop Jockey 3 Master Edition | 6 / 4 | 44.1–96k | plist (flags = DB2/DB4, kInACR 1) | fvdpol/alsa-jockey3 (GPL, out of tree; upstream patch pending) | in Ozzy (experimental) |
| 200C:1037 | Reloop Jockey 3 Remix | 6 / 4 | 44.1–96k (fvdpol) | fallback | fvdpol/alsa-jockey3 | in Ozzy (experimental) |
| 200C:1005 | Reloop IQ2 MIDI | 4 / 4 | 24/96 (spec) | fallback | none | model row |
| 200C:100A | Reloop Contour Interface Edition | 2 / **10** | ? | fallback | none | 10-lane output frame (`dmaEncode10_*`) |
| 200C:1006 | unidentified Reloop | 6 / 6 | | fallback | | identify |
| 0A92:0111 | ESI ESU1808 | 16 / 8 (**10-lane** output: S/PDIF mirrors a selectable pair) | 44.1–96k (spec) | fallback | none | 10-lane output, ESI CPLD write ('I', wIndex 1, wValue byte\|0xE7) |
| 2573:0012 | ESI U168 XT | 16 / 8 | 44.1–96k (spec) | fallback | none | ESI CPLD write + 'A' request (0x349, wIndex 0x101); ESI's own 2.0.0b90 beta runs on Apple Silicon (user-space, analog only, macOS ≤ 14.3) |
| 2573:0013 | ESI U86 XT | 8 / 6 | 44.1–96k (spec) | fallback | none | ESI CPLD write; 6-lane output codec |
| 2573:002A | ESI U108 PRE | 10 / 8 | 44.1–96k (spec) | fallback | none | ESI CPLD write; 10-lane input codec |
| 1ACC:0103 | Midiplus AudioLink PLUS 4x4 ("Plus II") | 6 / 6 | 24/96 (spec) | fallback | none | 6-lane output codec; class-compliant mode? (?); Interspock's Ozzy fork adds it |
| 1ACC:0102 | unidentified Midiplus | 4 / 4 | | fallback | | identify |
| 1D03:005C | unidentified iCON | 2 / 2 | 44.1k | fallback | | identify |
| 0A4A:5A16 | Smyth Research Realiser A16 | 2 / 16 | ? | fallback, Revival page | ? | 16-lane output (?) |
| 0A4A:D064 | D.O.Tec EXBOX.UMA (?) ("DOTEC" in the kext) | 16 / 16 | default 88.2k, channels vary with rate | fallback | adiknoth/exboxuma (GPL, 2015 alpha) | own codecs; rate-dependent channels |
| 0A4A:2416 | Ploytec JCT 24x16 reference board (?) | 24 / 16 | | fallback | | probably not a product |
| 0A4A:DA01 | unidentified (A&H ICE-16?) | 16 / 16 | | fallback | | identify |
| 0A4A:CF00, 1016 | unidentified | 2 / 2 | | fallback | | identify |
| 0A4A:AFFE, CF04, 0003, 6A02, F1A0 | unidentified | | | not in fallback | | identify |
| 0A4A:AA01/AA06/AA09/AA11 | Reloop Digital Jockey IE, Digital Jockey 2 IE, Spin 2, 10tion | ? | | older kexts, no plist | none | check family; captures |
| 200C:1017, 100B, 1030 | Reloop Mixage IE, Play, Play 24/96 | ? | | not on the bulk list | none | check family |

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
| 09E8:207F | Akai MPC Renaissance ("ACV0") | 4 / 4 (2 real inputs) | 44.1–96k | plist | none | in Ozzy (experimental) as 2 in / 4 out, first MIDI port only; second MIDI port and the bcdDevice 0x0300 isoc in + isoc out variant missing |
| 0763:4002 | M-Audio M-Track Quad | 4 / 4 | 44.1–96k | plist (flags closest to the EIE; setFreqOnOutPipe) | none (?) | in Ozzy (experimental) |
| 0644:800F | Tascam US-144 | 4 / 4 | 44.1–96k | plist | snd-usb-us122l; us144mkii (6.18) | model row + Tascam 'A' register writes |
| 0644:8020 | Tascam US-144MKII | 4 / 4 | 44.1–96k | plist | us144mkii (6.18) | same |
| 0644:8034 | Tascam US-200 | 2 / 4 | 44.1–96k | plist | none | same (?) |
| 0644:8035 | Tascam US-600 | 6 / 4 | 44.1–96k | plist | none | same (?) |
| 0644:8017 | Tascam US-1641 | 16 / 4 | 44.1–96k | plist | none | same (?); 16-lane decoder (have it) |
| 0644:8025 | Tascam US-2000 | 16 / 4 | 44.1–96k | plist | none | same |
| 0644:8030 | Tascam US-1800 | 16 / 4 | 44.1–96k | plist | none | same |
| 0644:801E | Tascam M-164UF (mixer) | ? | | 2.0 kext, no plist | none | captures |
| 0644:8040, 8041 | Tascam US-322, US-366 | ? | | US3XX kext 3.4.13 (Ploytec-signed), no plist | none | captures; US-366 4/6-input post-processor |
| 0644:8046, 8048 | Tascam US-1200, UH-7000 (?) | ? | | kext strings | none | identify / captures |
| 1210:0018 | Lexicon I-ONIX U42S / IO42 | 6 / 4 | 44.1–96k | plist | none (?) | in Ozzy (experimental); S/PDIF clock source missing |
| 1210:0019 | Lexicon I-ONIX U82S / IO82 | 10 / 4 | 44.1–96k | plist | none (?) | in Ozzy (experimental; 10-lane decoder added); S/PDIF clock source missing |
| 1210:001A | Lexicon I-ONIX U22 / IO22 | 2 / 2 | 44.1–96k | plist | none (?) | in Ozzy (experimental) |
| 13B2:0030 | Alesis MultiMix 8 / 8 FX / 16 USB 2.0 | 4 / 4 per plist, 10 / 2 per spec | 44.1/48k per plist, up to 96k per spec | plist vs spec conflict | slash909uk JACK hobby project | descriptor dump first |
| 0562:03EB | Dynacord PowerMate-3 / CMS-3 (600/1000/1600/2200) | 4 / 4 (spec, Bosch) | up to 96k | no plist | none (HW:03 revision is class compliant) | captures (dominics/Ozzy fork has captures and support; issue #15) |
| 15E4:0071 | Numark NS7 | 4 / 4 | 44.1k | plist (kInACR 8) | none | in Ozzy (experimental, NS7 MIDI framing) |
| 15E4:0075 | Numark V7 | 4 / 4 | 44.1k | plist | none | in Ozzy (experimental, NS7 MIDI framing; tejofjord/openv7, MIT, documents it) |
| 15E4:0079 | Numark NS6 | 4 / 4 | 44.1k | plist (kInACR 8) | none | in Ozzy (experimental, NS7 MIDI framing) |
| 15E4:0040 | Numark MixDeck | 4 / 4 | 44.1/88.2k | plist (kInACR 2) | none | in Ozzy (experimental) |
| 15E4:0026 | Numark 4Trak | 2 / 4 | 44.1k | plist (kInACR 8) | none | in Ozzy (experimental, NS7 MIDI framing) |
| 15E4:002C, 0070, 0074, 0078 | further Numark PIDs in the EIE 3.3.17 kext | ? | | kext list | | identify |
| 0A4A:0014, 5B01 | unidentified | ? | | kext list | | identify |

Tascam 'A' register writes (from the us144mkii driver; facts only): bRequest 0x41, wIndex 0x0101,
wValue 0x0d04, 0x0e00, 0x0f00, rate (0x1000 44.1k, 0x1002 48k, 0x1008 88.2k, 0x100a 96k), 0x110b;
'I' write 0x0010 before and 0x0030 to start; 'D' (0x44) = deep sleep. Whether all Tascams need
them is open.

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
for `M4_M4_C1` it reads the first port only.

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
  1. Per-device handshake extras: Tascam 'A' register writes (≥7 devices), ESI CPLD byte and 'A'
     request (4 devices), Lexicon/ESI S/PDIF clock source.
  2. Frame codecs for more lane counts: 6-lane output (U86 XT, AudioLink), 10-lane output with the S/PDIF mirror (ESU1808, Contour IE), 16-lane output
     (Realiser A16).
  3. Input post-processing quirks (MPC Renaissance zeroes inputs 3/4, US-366).
  4. Rarer: isoc in + isoc out (MPC Renaissance bcdDevice 0x0300), rate-dependent channel counts
     (Dotec, SPL Crimson), the Numarks' `kMsWait` 200 (not implemented; its effect is open).
- The central device list `common/ozzy_devices.def` (one row per device: VID, PID, name, family,
  in/out, rates, status, quirks) is where a device is added; the Linux ID table, the
  kext's personalities and the engine choice are generated from it.

## Naming correction
200C:1009 is the **Digital Jockey 2 Master Edition** (Ozzy's name; Reloop's INF and PR #80) and
200C:1019 the **Jockey 3 Master Edition** (Reloop's Jockey 3 ME kext). The upstream Jockey 3 patch
v3 (June 2026) used 0x1009 for the Jockey 3 ME, which this suggests is wrong (?).

## Open questions
- Identify: 1ACC:0102, 1D03:005C, 200C:1006, 0A4A AFFE/CF00/CF04/DA01/0003/1016/6A02/F1A0/0014/5B01,
  0644:8048, 0644:8012 ("US-144 Pro"), the extra Numark PIDs.
- Decode the remaining flag bytes 0x143, 0x145, 0x146, 0x14a, 0x14b, 0x14e.
- Decompile `dmaEncode10_*` (10-lane output packing).
- Alesis MultiMix real format; Dynacord original revision layout.
- Is `ah4d_akm_v132.bin` (in the Jockey 3 firmware updaters) Xone:4D firmware 1.32?
- ESI's 2.0.0b90 user-space driver: its init sequence and why "analog only".
