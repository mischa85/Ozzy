# ozzy-harness

Runs Ozzy's engine (`engine/`) against a device bound to WinUSB, as a console program. It is a
tool for protocol work on Windows without loading a kernel driver; the Windows driver itself is
the ACX driver. It plays the CoreAudio HAL's part of the shared memory:

- a test tone on outputs 1/2 (`-q` for silence)
- the input to a 24-bit WAV file (`-r capture.wav`)
- MIDI in to the console

Every second it prints the device clock measured against `QueryPerformanceCounter`, transfers
per second and errors.

## Setup

1. Bind WinUSB to the **whole device** with [Zadig](https://zadig.akeo.ie/): Options > List All
   Devices, pick the device itself (without the official driver it shows as "... (Composite
   Parent)"; not "Interface 0"), WinUSB, Replace Driver. The engine claims the interfaces
   itself. This replaces the device's driver until you roll it back in Device Manager.
2. From a Visual Studio developer prompt:

   ```
   cmake -S . -B build
   cmake --build build --config Release
   ```

## Use

```
ozzy-harness -l                  list supported devices and their drivers
ozzy-harness -s 30 -r in.wav     30 s with the test tone, input to in.wav
```

Devices that stream isochronous OUT (the Akai EIE Pro) can't run here: WinUSB splits an
isochronous write into full-size packets and takes no per-packet lengths, which their feedback
pacing needs.
