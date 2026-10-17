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
| `kmdf_bus.c` | the engine's bus (`ozzy_bus`) on KMDF USB: requests into the shared memory, control requests |
| `ozzy.inf.in` | INF template: endpoints, service; the build fills in the devices |
| `GenInf.c` | writes `ozzy.inf`: one line per supported device in `common/ozzy_devices.def` (compiled and run by `ozzy.vcxproj` before the build, like `GenPersonalities.c` for the kext) |

Supported so far: the Ploytec bulk/interrupt family (the Xones and their relatives). Not yet:
- the Ploytec isochronous family (Akai EIE Pro, M-Track Quad): needs isochronous URBs
- USB MIDI and MIDI in/out: needs a MIDI circuit
- device settings and ozzyctl

Sample rates: each endpoint offers a format per rate the device lists (the highest first). A
stream at another rate restarts the engine at it (`ozzy_engine_change_rate`), unless the other
direction has a stream open at the current rate, or the device is locked to S/PDIF at another
rate; then the stream is refused.

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
