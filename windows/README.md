# Ozzy for Windows (ACX)

A KMDF + ACX kernel driver (Windows 11). The device shows up as a normal audio device with one
output and one input endpoint, so every app reaches it through WASAPI. It runs the same engine
as the Linux module and the macOS kext (`engine/`) on a KMDF USB bus (`kmdf_bus.c`), and does the real-time work in the
USB completion path:

- **Playback:** when a PCM OUT transfer completes, the engine encodes the next packet's 80 frames
  straight from the WaveRT buffer into the USB packet (`ploytec_encode_frame_Nch`), then sends it.
- **Recording:** when a PCM IN transfer completes, the engine decodes it straight into the WaveRT
  buffer (`ploytec_decode_frame_Nch`).
- **No float:** the endpoints use packed 24-bit PCM (S24_3LE), and Windows' audio engine converts.
- **Packet completion:** the ACX stream completes WaveRT packets (`AcxRtStreamNotifyPacketComplete`)
  from the same path, so the device's USB clock drives everything.

| File | What |
|---|---|
| `driver.c` | DriverEntry: WDF and ACX driver |
| `device.c` | device add (model from the hardware ID), engine, bus and shared memory, D0 entry/exit |
| `circuit.c` | render and capture circuits: host and bridge pins, the one S24_3LE format |
| `stream.c` | RT stream: WaveRT packets as one ring, positions, packet completion |
| `midi.c` | MIDI in and out as a UMP MIDI circuit for Windows MIDI Services: a looped ring per direction mapped into the MIDI service, MIDI 1.0 bytes converted by `common/midi_ump.c` |
| `settings.c` | the device settings for `ozzy-cli` and the ozzy app: a KS property set on each circuit (`ozzy_ksproperty.h`: status, settings and the driver's record `struct ozzy_info`), kept in the device's registry key, and a KS event that tells of every change |
| `kmdf_bus.c` | the engine's bus (`ozzy_bus`) on KMDF USB: bulk/interrupt requests and isochronous URBs on the engine's own transfer buffers (`ozzy_engine_alloc_memory`), control requests |
| `ozzy.inf.in` | INF template: endpoints, service; the build fills in the devices |
| `GenInf.c` | writes `ozzy.inf`: one line per supported device in `common/ozzy_devices.def` (compiled and run by `ozzy.vcxproj` before the build, like `GenPersonalities.c` for the kext) |

Supported so far: the Ploytec bulk/interrupt family (the Xones and their relatives) and the
Ploytec isochronous family (Akai EIE Pro, M-Track Quad, the Lexicon I-ONIX range, ...): an
isochronous transfer is one `URB_FUNCTION_ISOCH_TRANSFER` (started ASAP) over a buffer of its
own, its packets back to back, so each OUT packet's length is the gap to the next. The URB
reports no per-frame times; every packet gets the completion's time.

MIDI (`midi.c`): a MIDI circuit next to the audio ones, the way Microsoft's USB MIDI 2 driver
(also ACX) exposes its ports, so Windows MIDI Services (Windows 11, 25H2 for the full API) finds
it. Apps on the WinMM and WinRT MIDI APIs see a MIDI 1.0 port through the service. Each
direction is a stream whose ring of UMP messages is mapped into the service
(`KSPROPSETID_MidiLoopedStreaming`): MIDI in goes from the engine's ring into it in the USB
completion path, MIDI out comes from it on a worker thread into the engine's ring. Not covered:
the legacy MIDI stack (Windows MIDI Services switched off, or before it), and the Xone:1D (USB
MIDI class: Windows' own driver may take it). If ACX doesn't take a MIDI circuit next to WaveRT
ones, audio still comes up and the log says so.

Sample rates: each endpoint offers a format per rate the device lists (the highest first). A
stream at another rate restarts the engine at it (`ozzy_engine_change_rate`), unless the other
direction has a stream open at the current rate, or the device is locked to S/PDIF at another
rate; then the stream is refused.

Device settings (clock source, S/PDIF out mode, routing, for the devices that have them): Windows
has no place for them in its own sound settings, so they are `ozzy-cli`'s
(`apps/cli/CMakeLists.txt`): `ozzy-cli show`, `ozzy-cli set clock-source internal`. The same settings are in the ozzy app (`apps/gui/`, meson from a VS developer prompt), which follows every change through the driver's KS event. A device
setting restarts the engine (an open stream glitches); every change is kept in the device's
registry key and applied when the device comes up.

## Build

Visual Studio 2022 with the Windows Driver Kit (with ACX 1.1 headers, WDK 10.0.22621 or later):

```
msbuild ozzy.vcxproj /p:Configuration=Release /p:Platform=x64
```

## Install (test signing)

The driver is test-signed by the build. On the test PC, once:

```
bcdedit /set testsigning on      (then reboot)
```

Then install it for the device (this replaces the official driver; roll it back in Device Manager):

```
pnputil /add-driver x64\Release\ozzy\ozzy.inf /install
```

Log output: DebugView (Capture Kernel) or WinDbg. It prints `[Ozzy]` / `[Ploytec]` lines under
the IHVAUDIO component (`ed nt!Kd_IHVAUDIO_Mask 0xffffffff`).

Distributing it to others needs attestation signing (EV certificate + Partner Center).
