#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The MIDI out bytes in a USBPcap capture of a Ploytec device: the MIDI slot of each PCM out
sub-packet on EP 0x05 (.claude/re/ploytec_midi.md), idle slots (FD) left out.

  python midi_out_slots.py capture.pcap

964-byte packets carry two 482-byte sub-packets with the slot at 432; 512-byte ones one with the
slot at 480.

Copyright (C) 2026 Marcel Bierling <marcel@hackerman.art>
"""
import struct
import sys

d = open(sys.argv[1], "rb").read()
off, midi, slots = 24, [], 0
while off + 16 <= len(d):
    incl = struct.unpack_from("<I", d, off + 8)[0]
    rec = d[off + 16:off + 16 + incl]
    off += 16 + incl
    header = struct.unpack_from("<H", rec, 0)[0]     # USBPCAP_BUFFER_PACKET_HEADER
    info, endpoint = rec[16], rec[21]
    if endpoint != 0x05 or info & 1:                 # OUT only
        continue
    data = rec[header:]
    step, slot = (482, 432) if len(data) % 482 == 0 else (512, 480)
    for k in range(0, len(data) - slot, step):
        slots += 1
        if data[k + slot] != 0xFD:
            midi.append(data[k + slot])
print(f"{slots} slots, {len(midi)} MIDI bytes:", " ".join(f"{b:02x}" for b in midi))
