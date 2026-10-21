# hwtest

Small tools for testing a driver on the device itself: play test tones and check the recording,
change the sample rate, watch MIDI, measure the round trip. They rely on the device looping its
playback back to its capture. The Xones do: each USB return goes to the matching USB send, and the
DB4 also feeds inputs 7-8 from outputs 1-2.

| Tool | Platform | Build |
|---|---|---|
| `tones.py`, `tonecheck.py` | any (Python 3, numpy) | — |
| `rtlat.c` | JACK, or PipeWire through `pw-jack` | `cc -O2 -o rtlat rtlat.c -ljack -lm` |
| `ca_rate.c` | macOS | `cc -O2 -o ca_rate ca_rate.c -framework CoreAudio -framework CoreFoundation` |
| `midi_listen.c` | macOS (Linux: `amidi -l`, `amidi -p hw:N,0 -d`) | `cc -O2 -o midi_listen midi_listen.c -framework CoreMIDI -framework CoreFoundation` |

## Play and record

```sh
python3 tones.py 48000 tone_48000.wav

# Linux, straight to the card (PipeWire leaves it alone while it's idle)
arecord -D hw:2 -r 48000 -c 8 -f S24_3LE -d 8 rec.wav & sleep 0.5; aplay -D hw:2 tone_48000.wav; wait
# Linux through PipeWire: the pro-audio ports are AUX0..7, so map them
pw-record --target <input node> --rate 48000 --channels 8 --channel-map AUX0,AUX1,AUX2,AUX3,AUX4,AUX5,AUX6,AUX7 --format s24 rec.wav
# macOS (sox's default buffer overruns at 8 x 96 kHz: give it a larger one)
./ca_rate DB4 48000
sox -q --buffer 262144 -t coreaudio "Allen&Heath Xone:DB4" -r 48000 -c 8 -b 24 rec.wav trim 0 8 &
sleep 0.5; sox -q --buffer 262144 tone_48000.wav -t coreaudio "Allen&Heath Xone:DB4"; wait

python3 tonecheck.py rec.wav
```

Clean: every tone lasts 5.00 s at its own frequency, the residual stays near 1e-5 and there are 0
glitches. Start recording after a rate change has settled: a recording that opens the device at a
new rate also catches the restart.

## Round trip

```sh
pw-jack ./rtlat "Allen&Heath Xone DB4 Pro:playback_AUX4" "Allen&Heath Xone DB4 Pro:capture_AUX4" 6
pw-metadata -n settings 0 clock.force-quantum 64    # smaller host buffers; 0 undoes it
```

Use a channel nothing else plays on. The DB4's measurements are in
`.claude/re/ploytec_bulk_devices.md`.
