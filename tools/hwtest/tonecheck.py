#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check a recording of the test tones (tones.py) that went through the device and back: per
channel, when the tone ran and for how long (a dropped or doubled transfer changes it), its
frequency and level, and the glitches in it.

  python3 tonecheck.py rec.wav

A glitch is a sample the sine before it doesn't predict: for a pure sine
x[n+1] + x[n-1] = 2 cos(w) x[n], so the residual stays near zero and any gap, repeat or
garbled transfer stands out. "max residual" is the largest one, near 1e-5 at 440 Hz when
clean, and growing with the frequency (24-bit rounding); clean recordings have 0 glitches.

Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
"""
import sys
import wave

import numpy as np

GLITCH = 1e-3   # residual that counts as a glitch (clean: well below at these levels)


def read(path):
    w = wave.open(path)
    n, ch, rate = w.getnframes(), w.getnchannels(), w.getframerate()
    if w.getsampwidth() != 3:
        sys.exit("tonecheck: 24-bit WAV only")
    raw = np.frombuffer(w.readframes(n), dtype=np.uint8)
    n = len(raw) // (3 * ch)   # a recording cut short holds fewer frames than its header says
    raw = raw[: n * 3 * ch].reshape(n, ch, 3).astype(np.int32)
    x = raw[:, :, 0] | (raw[:, :, 1] << 8) | (raw[:, :, 2] << 16)
    return np.where(x >= 1 << 23, x - (1 << 24), x) / float(1 << 23), rate


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    x, rate = read(sys.argv[1])
    print(f"recorded {len(x) / rate:.2f}s at {rate} Hz")
    for c in range(x.shape[1]):
        s = x[:, c]
        on = np.nonzero(np.abs(s) > 0.005)[0]
        if len(on) < rate // 10:
            print(f"ch{c + 1}: silent")
            continue
        a, b = on[0] + rate // 100, on[-1] - rate // 100   # skip the edges
        seg = s[a:b]
        f = np.fft.rfftfreq(len(seg), 1 / rate)[np.argmax(np.abs(np.fft.rfft(seg * np.hanning(len(seg)))))]
        k = 2 * np.cos(2 * np.pi * f / rate)
        r = np.abs(seg[2:] + seg[:-2] - k * seg[1:-1])
        bad = np.nonzero(r > GLITCH)[0]
        glitches = len(np.split(bad, np.nonzero(np.diff(bad) > 20)[0] + 1)) if len(bad) else 0
        print(f"ch{c + 1}: tone {on[0] / rate:.2f}-{on[-1] / rate:.2f}s ({(on[-1] - on[0]) / rate:.2f}s) "
              f"{f:.1f} Hz, peak {np.max(np.abs(seg)):.4f}, max residual {r.max():.1e}, {glitches} glitches")


if __name__ == "__main__":
    main()
