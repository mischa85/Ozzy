# 🎛️ Ozzy - USB Audio & MIDI Driver for Non-Class Compliant Devices

**Bringing legacy professional audio hardware back to life.**

Modern operating systems dropped support for non-class compliant USB audio devices—hardware that doesn't follow the standard USB Audio Class specification. These devices require vendor-specific drivers, and when manufacturers abandon them, perfectly good professional equipment becomes unusable.

Ozzy fixes that.

This is an open-source, reverse-engineered driver supporting non-class compliant USB audio interfaces—high-end DJ mixers and audio processors that were left behind when official driver support ended.

**Currently Supported Devices:**
* **Allen & Heath Xone:DB4, DB2, DX, 4D** (Ploytec-based protocol)
* **Tascam US-1641**
* **Experimental:** 30+ more Ploytec-based interfaces and DJ controllers from Allen & Heath, Reloop, ESI, Midiplus, Smyth Research, Akai, M-Audio, Lexicon, Numark and Tascam ([full list](#️-supported-devices))

More devices can be added—the architecture separates the audio engine from device protocols.

---

## 🚀 What Makes These Devices Non-Class Compliant?

Standard USB Audio Class devices work automatically with any modern OS—they follow a universal protocol. But professional hardware often needs:

* **Custom audio routing** beyond simple stereo in/out
* **Hardware-specific DSP control** and mixer integration  
* **Proprietary USB protocols** for low-latency performance
* **Special MIDI implementations** for controller integration

These devices never worked with generic OS drivers. They **require** vendor-specific drivers. When vendors stop supporting them, the hardware stops working on new OS versions.

Ozzy reverses the protocol and provides modern drivers—so your equipment keeps working.

---

## 🎚️ Supported Devices

Every device below comes from one list in [`common/ozzy_devices.def`](common/ozzy_devices.def), which both the Linux module and the macOS driver are built from.

### ✅ Supported (tested on hardware)

| Device | In / out | Sample rates |
| :--- | :--- | :--- |
| **Allen & Heath Xone:DB4** | 8 / 8 | 44.1–96 kHz |
| **Allen & Heath Xone:DB2** | 8 / 8 | 48 kHz |
| **Allen & Heath Xone:DX** | 8 / 8 | 44.1–96 kHz |
| **Allen & Heath Xone:4D** | 8 / 8 | 44.1–96 kHz |
| **Tascam US-1641** | 16 / 4 | 44.1–96 kHz |

### 🧪 Experimental (needs testers)

These use the same Ploytec chip. Their channels, rates and setup requests come from the manufacturers' own drivers, but they haven't been confirmed working with Ozzy yet. If you own one, please [try it and report back](#-testing-an-experimental-device).

| Device | In / out | Sample rates | Notes |
| :--- | :--- | :--- | :--- |
| **Akai EIE Pro** | 4 / 4 | 44.1–96 kHz |  |
| **Akai MPC Renaissance** | 2 / 4 | 44.1–96 kHz | First MIDI port only |
| **Allen & Heath MixWizard WZ4 USB** | 16 / 2 | 44.1–96 kHz |  |
| **Allen & Heath Xone:2D** | 8 / 8 | 44.1–96 kHz |  |
| **ESI ESU1808** | 16 / 8 | 44.1–96 kHz | S/PDIF out mirrors outputs 1/2 |
| **ESI U86 XT** | 8 / 6 | 44.1–96 kHz |  |
| **ESI U108 PRE** | 10 / 8 | 44.1–96 kHz |  |
| **ESI U168 XT** | 16 / 8 | 44.1–96 kHz |  |
| **Lexicon I-ONIX U22 / IO22** | 2 / 2 | 44.1–96 kHz |  |
| **Lexicon I-ONIX U42S / IO42** | 6 / 4 | 44.1–96 kHz |  |
| **Lexicon I-ONIX U82S / IO82** | 10 / 4 | 44.1–96 kHz |  |
| **M-Audio M-Track Quad** | 4 / 4 | 44.1–96 kHz |  |
| **Midiplus AudioLink PLUS 4x4** | 6 / 6 | 44.1–96 kHz |  |
| **Numark 4Trak** | 2 / 4 | 44.1 kHz |  |
| **Numark MixDeck** | 4 / 4 | 44.1/88.2 kHz |  |
| **Numark NS6** | 4 / 4 | 44.1 kHz |  |
| **Numark NS7** | 4 / 4 | 44.1 kHz |  |
| **Numark V7** | 4 / 4 | 44.1 kHz |  |
| **Reloop Contour Interface Edition** | 2 / 10 | 44.1 kHz |  |
| **Reloop Digital Jockey 2 Master Edition** | 6 / 4 | 44.1 kHz |  |
| **Reloop IQ2 MIDI** | 4 / 4 | 44.1–96 kHz |  |
| **Reloop Jockey 3 Master Edition** | 6 / 4 | 44.1–96 kHz |  |
| **Reloop Jockey 3 Remix** | 6 / 4 | 44.1–96 kHz |  |
| **Smyth Research Realiser A16** | 2 / 16 | 44.1 kHz |  |
| **Tascam US-144** | 4 / 4 | 44.1–96 kHz | macOS only by default: Linux has its own drivers |
| **Tascam US-144MKII** | 4 / 4 | 44.1–96 kHz | macOS only by default: Linux has its own driver |
| **Tascam US-200** | 2 / 4 | 44.1–96 kHz |  |
| **Tascam US-600** | 6 / 4 | 44.1–96 kHz |  |
| **Tascam US-1800** | 16 / 4 | 44.1–96 kHz |  |
| **Tascam US-2000** | 16 / 4 | 44.1–96 kHz |  |

**MIDI only:** Allen & Heath Xone:1D, on macOS. On Linux the kernel's own USB MIDI driver handles it, no Ozzy needed.

### 🔍 Wanted: devices we need information about

We can't work these out from the drivers alone. If you own one, the linked issue says what would help:

* Alesis MultiMix 8 / 8 FX / 16 USB 2.0: [#90](https://github.com/mischa85/Ozzy/issues/90)
* Dynacord PowerMate-3 / CMS-3: [#15](https://github.com/mischa85/Ozzy/issues/15)
* Reloop, older interfaces (Digital Jockey IE / 2 IE, Spin 2, 10tion, Mixage IE, Play): [#92](https://github.com/mischa85/Ozzy/issues/92)
* Tascam US-322, US-366, US-1200, UH-7000: [#91](https://github.com/mischa85/Ozzy/issues/91)
* Unidentified Ploytec USB IDs: [#93](https://github.com/mischa85/Ozzy/issues/93)

---

## 🖥️ Platform Support

Ozzy provides **native kernel-mode drivers** for maximum performance and compatibility.

### 🍎 macOS (Multiple Backend Options)
Flexible architecture supporting three backend implementations:

**Current: Kernel Extension (Kext)** - *Recommended*
* **Audio:** up to 16 in / 16 out, per device, via CoreAudio HAL
* **MIDI:** Full CoreMIDI support with lock-free ring buffers
* **Latency:** Sub-millisecond performance via zero-copy ring buffers
* **Requirements:** macOS 11+, SIP modified for kext loading (`csrutil enable --without kext`)
* **Status:** ✅ Fully implemented and stable

**Planned: DriverKit Extension (Dext)**
* Modern sandboxed system extension
* Requires Apple entitlements for production use
* Same performance as kext, future-proof architecture
* **Status:** 🚧 In development (legacy prototype in `legacy/mac-coreaudio/`)

**Planned: Userspace Daemon**
* No kernel extension required
* Works with full SIP enabled
* Slightly higher latency but maximum compatibility
* **Status:** 🚧 Planned

All backends share the same CoreAudio HAL and CoreMIDI drivers—only the USB communication layer changes. Choose the backend that fits your security and performance requirements.

**Location:** [`macos/`](macos/)

### 🐧 Linux (ALSA Kernel Module)
Standard ALSA kernel module with automatic transfer mode detection.
* **Audio:** up to 16 in / 16 out, per device (PCM)
* **MIDI:** ALSA Sequencer In/Out
* **Modes:** Automatic BULK/INTERRUPT transfer detection, isochronous output with feedback
* **Integration:** Works seamlessly with JACK, PulseAudio, PipeWire
* **Location:** [`linux/`](linux/)

### 🪟 Windows
*Coming soon.* WASAPI kernel-mode driver in development.

**Why kernel mode?**  
Non-class compliant devices need direct USB pipe access and precise timing that's only possible in kernel space. User-space solutions add latency and complexity—kernel drivers provide the clean, low-latency performance these professional devices were designed for.

---

## 💿 Installation

### 🍎 macOS

**Quick Install:**
1. Open Terminal and navigate to the `macos/` directory
2. Run: `./install.command`
3. Enter your password when prompted
4. Plug in your mixer

The installer automatically:
- ✅ Checks SIP configuration
- ✅ Compiles the kernel extension
- ✅ Builds HAL and MIDI drivers
- ✅ Installs to system directories
- ✅ Configures auto-loading at boot
- ✅ Starts drivers immediately

**Requirements:**
- macOS 11+ (Big Sur or later)
- SIP modification: Run `csrutil enable --without kext` in Recovery Mode
- Apple Developer certificate in Keychain (free Apple ID works)

**Leaving devices out:** `OZZY_DISABLE="TascamUS144 NumarkNS7" ./install.command` builds without those devices (names are the keys in [`common/ozzy_devices.def`](common/ozzy_devices.def)), so another driver can keep them.

**Device settings:** the installer also puts `ozzyctl` in `/usr/local/bin`. `ozzyctl` lists the devices, `ozzyctl show` a device's settings (clock source, S/PDIF out mode, routing, for the devices that have them) and `ozzyctl set clock-source internal` changes one. The clock source is also in Audio MIDI Setup.

**Uninstall:** Run `macos/uninstall.command`

**Troubleshooting:** Run `macos/debuglogs.command` to collect diagnostic logs

### 🐧 Linux

1.  Clone and build:
    ```bash
    git clone https://github.com/mischa85/Ozzy
    cd Ozzy/linux
    make
    sudo make modules_install
    ```
2.  Load the module:
    ```bash
    sudo modprobe snd-usb-ozzy
    ```

**Device settings** are ALSA controls ("Clock Source", "IEC958 Playback Mode", "Output 1-2 Source", ..., for the devices that have them), kept across reboots by `alsactl store`. Use `alsamixer` / `amixer`, or `ozzyctl`, the same tool as on macOS (needs alsa-lib, e.g. `libasound2-dev`):
```bash
make -C ozzyctl && sudo install -m 755 ozzyctl/ozzyctl /usr/local/bin/
ozzyctl show
ozzyctl set clock-source internal
```

**Leaving devices out:** `make OZZY_DISABLE="NumarkNS7 ReloopIQ2MIDI"` builds without those devices (names are the keys in [`common/ozzy_devices.def`](common/ozzy_devices.def)). By default the Linux build leaves out the Tascam US-144 and US-144MKII, which the kernel's own drivers handle; `make OZZY_DISABLE=` builds them in.

---

## 🧪 Testing an Experimental Device

If your device is in the experimental list, you can help get it to "supported":

1. Install Ozzy as above and plug the device in.
2. Try playback, recording and (if it has any) MIDI, at each sample rate it offers.
3. Open an issue with the result, whether it works or not, and include the log:
   * **Linux:** `sudo dmesg | grep -i ozzy` and `lsusb -v -d <vid:pid>`
   * **macOS:** the output of `macos/debuglogs.command`

Even "it works" is useful: it's what moves a device to the supported list.

---

## 🏗️ Building from Source (The "Signing" Pain)

If you are a developer or want to build the latest code, you **must** code-sign the macOS binaries.

### 1. Get a Free Signing Identity
You do not need a paid Apple Developer program membership ($99/yr). A free Apple ID works fine.
1. Open **Xcode**.
2. Go to **Settings (Cmd+,)** -> **Accounts**.
3. Click the **`+`** button and add your Apple ID.
4. Select your Personal Team.
5. Click **Manage Certificates...**.
6. Click the **`+`** in the bottom left and select **Apple Development**.
7. Wait for it to say "Created". You now have a valid certificate in your Keychain.

### 2. Build & Install
Run the build script. It will auto-detect your new certificate:
```bash
./macos/install.command
```

### 🔑 "0 valid identities found"

This is the most common build error. It means `security find-identity -v -p codesigning` returns nothing, even though you added your account in Xcode. **This is an Apple Keychain/certificate chain-of-trust issue, not an Ozzy issue.** Work through the fixes below in order.

> Apple's codesigning infrastructure has several long-standing pain points. The WWDR G1 intermediate certificate expired in February 2023 and broke codesigning for millions of developers — Xcode's automatic certificate management didn't handle the transition cleanly, and machines restored from backup or migrated via Time Machine still carry the expired cert. Keychain Access has a UX trap where right-clicking a certificate and choosing "Always Trust" (the natural instinct when you see a warning) silently adds a custom trust policy that makes `security find-identity` reject it — with no indication that you just broke codesigning. Free Apple Development certificates expire after one year with no notification outside of Xcode. And when things go wrong, the error messages are useless — "0 valid identities found" tells you nothing about *why*. This isn't going away; it's been like this since at least 2016.

#### Diagnose First

Open **Keychain Access**, find your **"Apple Development: [Your Name]"** certificate, and check:
- Does it have a **disclosure triangle** (▶) next to it? If not, the **private key is missing** — see Fix 4.
- Does it show a **red ✕** or say **"This certificate has an invalid issuer"**? The intermediate certificate is missing or expired — see Fix 2.
- Does it say **"This certificate is marked as trusted for all users"**? You hit the trust trap — see Fix 1.

You can also run a formal evaluation: select the certificate, then go to **Keychain Access > Certificate Assistant > Evaluate** and choose **Generic (certificate chain validation only)**. This will tell you exactly what's broken.

#### Fix 1: The "Trust" Trap (Most Common)

Did you (or someone troubleshooting) manually change the certificate trust settings in Keychain Access?

1. Open **Keychain Access**.
2. Find your **"Apple Development: [Your Name]"** certificate.
3. Right-click -> **Get Info**.
4. Expand the **Trust** section.
5. **CRITICAL:** Set everything to **"Use System Defaults"**.

If this was set to "Always Trust", macOS adds a custom trust policy that *breaks* the codesign toolchain. The certificate may *look* valid but `security find-identity` will reject it. It must stay on "System Defaults".

#### Fix 2: Missing or Expired WWDR Intermediate Certificate

Your signing certificate is issued by Apple's **Worldwide Developer Relations (WWDR)** intermediate authority. If that intermediate is missing, expired, or the wrong generation, macOS can't build the certificate chain and your identity becomes invisible.

The old WWDR intermediate (G1) **expired February 7, 2023**. If you see it in your keychain with a red ✕, that's the problem.

**To fix:**
1. Open **Keychain Access** and enable **View > Show Expired Certificates**.
2. Delete any expired **"Apple Worldwide Developer Relations Certification Authority"** entries.
3. Download the current intermediates from [Apple's Certificate Authority page](https://www.apple.com/certificateauthority/). You need **WWDR G3** (for development certificates) — direct link:
   ```bash
   curl -O https://www.apple.com/certificateauthority/AppleWWDRCAG3.cer
   open AppleWWDRCAG3.cer
   ```
4. Double-click to install, or drag into Keychain Access under the **login** keychain.
5. Verify: the certificate should show **"Expires: 2030"** and no red warnings.

If you're on **Xcode 11.4.1 or later**, Xcode should download this automatically — but it often doesn't, especially after a clean macOS install or migration.

#### Fix 3: Certificate Expired

Signing certificates themselves expire (typically after 1 year for free Apple Development certificates).

1. In Keychain Access, select your certificate and check the **Expires** field.
2. If expired, delete it.
3. Recreate via Xcode: **Settings > Accounts > Manage Certificates > + > Apple Development**.

#### Fix 4: Missing Private Key

A signing identity requires both the certificate AND its private key. If you migrated to a new Mac, restored from backup, or created the certificate on a different machine, the private key may not have come along.

**How to check:** In Keychain Access, click the certificate. If there's a **disclosure triangle** (▶) and you can expand it to see a private key — you're fine. If there's no triangle, the private key is missing.

**To fix:** You must delete the orphaned certificate and create a new one via Xcode (step 1 above). Private keys cannot be recovered — they can only be exported from the original machine.

#### Fix 5: Nuclear Option — Start Fresh

If nothing above works:
1. In Keychain Access, delete ALL certificates containing "Apple Development" or "Developer ID" from both **login** and **System** keychains.
2. Delete expired WWDR intermediates (see Fix 2).
3. Download and install fresh WWDR G3 intermediate.
4. Open **Xcode > Settings > Accounts**, remove and re-add your Apple ID.
5. Click **Manage Certificates > + > Apple Development**.
6. Verify: `security find-identity -v -p codesigning` should now show your identity.

#### Further Reading
- [Apple: Fixing an untrusted code signing certificate](https://developer.apple.com/forums/thread/712043)
- [Apple: WWDR Intermediate Certificate Expiration](https://developer.apple.com/support/expiration/)
- [Apple: Certificate Authority downloads](https://www.apple.com/certificateauthority/)
- [StackOverflow: Code sign error - no identity found](https://stackoverflow.com/questions/15068617/code-sign-error-in-xcode-no-identity-found)
- [StackOverflow: Could not find a valid private key/certificate pair](https://stackoverflow.com/questions/8424017/xcode-could-not-find-a-valid-private-key-certificate-pair-for-this-profile-in-yo)
- [How to fix "no identity found" error in Xcode](https://sarunw.com/posts/how-to-fix-command-codesign-failed/)
- [How to purge and re-install code signing identities](https://ohanaware.com/blog/202129/How-to-purge-and-re-install-code-signing-identities.html)

---

## 🏗️ Architecture

### macOS Shared Memory Model

```
┌─────────────────┐
│  Audio Apps     │
│  (Logic, etc)   │
└────────┬────────┘
         │ CoreAudio
┌────────▼────────┐      Shared Memory       ┌─────────────┐
│   OzzyHAL       │◄────────────────────────►│  OzzyKext   │
│  (HAL Driver)   │      Ring Buffers        │   (Kernel)  │
└─────────────────┘                          └──────┬──────┘
                                                    │ USB
                                             ┌──────▼──────┐
                                             │  Hardware   │
                                             │  (Xone:DB4) │
                                             └─────────────┘
```

**Components:**
- **OzzyKext** - Kernel extension handling USB communication
- **OzzyHAL** - CoreAudio HAL driver for audio I/O
- **OzzyMIDI** - CoreMIDI driver for MIDI I/O

Zero-copy architecture with lock-free ring buffers in shared memory for minimal latency.

---

## 🛠️ For Developers

Ozzy is designed as a **reference implementation** for supporting non-class compliant USB audio devices.

### Why This Matters

Most USB audio driver examples you'll find online are either:
- **Class-compliant only** - They work with standard UAC devices but can't handle vendor protocols
- **User-space hacks** - They add latency and complexity to work around kernel restrictions
- **Closed source** - Manufacturers don't share how their protocols work

Ozzy provides a **clean, open-source example** of how to:

* **Reverse engineer** vendor-specific USB audio protocols
* **Implement kernel drivers** for direct USB pipe access
* **Build zero-copy architectures** with shared memory ring buffers
* **Support multiple platforms** with a shared protocol layer

### Architecture Highlights

- **Device-agnostic core:** The audio engine (`engine/`) is separated from device protocol implementations, making it easy to support additional vendors
- **One device list:** [`common/ozzy_devices.def`](common/ozzy_devices.def) has one row per device (IDs, channels, rates, protocol family, quirks). The Linux USB ID table and the macOS kext's matching are generated from it, so adding a device that fits an existing family is one row; per-device differences are quirk flags ([`common/ozzy_quirks.h`](common/ozzy_quirks.h))
- **Platform-portable:** Linux and macOS implementations share the USB protocol code
- **Performance-first:** No unnecessary data copies, context switches, or latency-inducing abstractions
- **Lock-free design:** Ring buffers use atomic operations for thread-safe, real-time audio

### Repository Structure

```
Ozzy/
├── common/         # Shared protocol code (platform-independent)
│   ├── ozzy_devices.def  # The device list (one row per device)
│   ├── ozzy_quirks.h     # Per-device differences
│   └── devices/    # Device-specific protocol definitions and codecs
├── linux/          # Linux ALSA kernel module (USB, PCM, MIDI, controls on the engine)
├── engine/         # Device engines shared by the Linux module, the macOS kext and the Windows driver
│   ├── core/       # Device-agnostic engine (C) and the bus interface (ozzy_bus)
│   ├── devices/    # Device-specific protocol implementations
│   └── shared/     # Shared memory layout, platform layer, logging
├── macos/          # macOS kext + HAL/MIDI drivers
│   └── Backends/   # Platform backends (Kext, future Dext)
├── windows/        # Windows ACX (WASAPI) driver (WIP)
├── ozzyctl/        # Device settings from the command line (macOS, Linux, Windows)
├── tests/          # Host tests of the engine on a fake USB bus
├── tools/
│   └── winusb-harness/  # The engine over WinUSB in a console program (Windows dev tool)
└── legacy/         # Archived experiments and old implementations
```

Whether you need to support your own legacy hardware or understand how professional audio drivers work, this codebase provides a solid foundation.

---

## 🐛 Troubleshooting & Bug Reports

**macOS driver won't load?**
- Check SIP status: `csrutil status` (should show "Kext Signing: disabled")
- View kernel logs: `sudo dmesg | grep Ozzy`
- Collect debug logs: Run `macos/debuglogs.command`

**No audio device appearing?**
- Ensure hardware is connected
- Restart CoreAudio: `sudo killall coreaudiod`
- Check Audio MIDI Setup app

**Linux module issues?**
- Check kernel logs: `sudo dmesg | grep -i ozzy`
- Verify module loaded: `lsmod | grep snd_usb_ozzy`

**Reporting Issues:**
When filing a bug report, please include:
1. Platform and OS version
2. Device model
3. Debug logs (use `macos/debuglogs.command` on macOS)
4. Steps to reproduce

---

## 📜 Legacy Implementations

The [`legacy/`](legacy/) directory contains archived driver implementations:

- **mac-coreaudio/** - DriverKit-based system extension (requires SIP disabled)
- **mac-hal/** - Early user-space HAL plugin experiments
- Old installation scripts and utilities

These are kept for reference but are not actively maintained.

---

## ☕ Support the Project

If this driver saved your mixer from the e-waste bin, consider supporting the development.

<a href="https://www.buymeacoffee.com/mischa85" target="_blank">
<img src="https://cdn.buymeacoffee.com/buttons/default-orange.png" alt="Buy Me A Coffee" height="41" width="174">
</a>

---

## 📄 License

MIT License. Do whatever you want with it.

See [LICENSE](LICENSE) for details.
