# Ozzy - macOS Audio Driver

Clean, modern kernel extension (kext) driver for Ploytec-based USB audio interfaces on macOS.

## 🚀 Quick Start

**Double-click** [install.command](install.command) to install everything.

The installer will:
- ✅ Check System Integrity Protection (SIP) status
- ✅ Compile the kernel extension
- ✅ Compile HAL and MIDI drivers
- ✅ Install everything to system directories
- ✅ Configure auto-loading at boot
- ✅ Start the drivers immediately

## 🗑️ Uninstall

**Double-click** [uninstall.command](uninstall.command) to remove all components.

## 📋 Requirements

- **macOS 11+** (Big Sur or later)
- **Apple Silicon or Intel** (Universal binary)
- **SIP modification required**: Run `csrutil enable --without kext` in Recovery Mode
- **Apple Developer certificate** in Keychain (free Apple ID works)

## 🏗️ Architecture

This driver uses a **shared memory model** for zero-copy audio streaming:

```
┌─────────────────┐
│  Audio Apps     │
│  (Logic, etc)   │
└────────┬────────┘
         │ CoreAudio
┌────────▼────────┐      Shared Memory      ┌─────────────┐
│   OzzyHAL       │◄────────────────────────►│  OzzyKext   │
│  (HAL Driver)   │      Ring Buffers        │   (Kernel)  │
└─────────────────┘                          └──────┬──────┘
                                                    │ USB
                                             ┌──────▼──────┐
                                             │  Hardware   │
                                             │  (Xone:DB4) │
                                             └─────────────┘
```

### Components

- **OzzyKext** - Kernel extension handling USB communication
- **OzzyHAL** - CoreAudio HAL driver for audio I/O
- **OzzyMIDI** - CoreMIDI driver for MIDI I/O

### Supported Devices

See the [device list in the main README](../README.md#️-supported-devices). The kext matches exactly the devices in [`common/ozzy_devices.def`](../common/ozzy_devices.def); `install.command` generates its `Info.plist` from it.

## 🔧 Manual Build

If you prefer to build manually:

```bash
cd macos
xcodebuild -project Ozzy.xcodeproj -scheme OzzyHAL -configuration Release
xcodebuild -project Ozzy.xcodeproj -scheme OzzyMIDI -configuration Release
```

The build system will compile the kext inline during installation.

## 📖 Documentation

- [OzzyCore/](OzzyCore/) - Device-agnostic audio engine base classes
- [Backends/OzzyKext/](Backends/OzzyKext/) - Kernel extension implementation  
- [Devices/Ploytec/](Devices/Ploytec/) - Ploytec protocol implementation
- [Shared/](Shared/) - Shared memory structures and logging

## 🐛 Troubleshooting

**Kext won't load?**
- Verify SIP: `csrutil status` should show "Kext Signing: disabled"
- Check logs: `log stream --predicate 'sender CONTAINS "Ozzy"'`

**No audio device showing?**
- Ensure hardware is connected
- Restart CoreAudio: `sudo killall coreaudiod`
- Check Audio MIDI Setup

**Installation fails?**
- Make sure you have an Apple Developer certificate in Keychain
- Try running from Terminal: `./install.command`

## 📄 License

See [LICENSE](../LICENSE) in repository root.
