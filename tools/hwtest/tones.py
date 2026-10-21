#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write the test tones: one sine per channel (440, 660, 880, 1000, 1200, 1400, 1600, 1800 Hz),
-20 dBFS, 5 s, 24-bit WAV. Play it while recording, then run tonecheck.py on the recording.

  python3 tones.py 48000 tone_48000.wav [CHANNELS]

Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
"""
import sys
import wave

import numpy as np

FREQS = (440, 660, 880, 1000, 1200, 1400, 1600, 1800)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    rate, path = int(sys.argv[1]), sys.argv[2]
    channels = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    t = np.arange(5 * rate) / rate
    x = np.stack([0.1 * np.sin(2 * np.pi * FREQS[c % len(FREQS)] * t) for c in range(channels)], axis=1)
    s = np.round(x * (1 << 23)).astype(np.int32)
    b = np.stack([s & 0xFF, (s >> 8) & 0xFF, (s >> 16) & 0xFF], axis=2).astype(np.uint8)
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(3)
        w.setframerate(rate)
        w.writeframes(b.tobytes())


if __name__ == "__main__":
    main()
