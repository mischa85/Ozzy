#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""MIDI from the command line on Windows, through WinMM: the MIDI 1.0 ports Windows MIDI Services
makes for a driver's endpoint, as older apps see them. Python 3, no packages.

  python winmm_midi.py list
  python winmm_midi.py monitor NAME SECONDS       print what arrives on the input whose name has NAME
  python winmm_midi.py send NAME HEX [HEX ...]    short messages, status byte first: 903c64 803c00
  python winmm_midi.py sysex NAME HEX             one sysex: f07e7f0601f7

It runs over SSH, which Windows MIDI Services' own `midi endpoint ... monitor` doesn't (it prints
nothing until it exits).

Copyright (C) 2026 Marcel Bierling <marcel@hackerman.art>
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

winmm = ctypes.WinDLL("winmm")
CALLBACK_FUNCTION = 0x30000
MIM_DATA, MIM_LONGDATA, MIM_ERROR = 0x3C3, 0x3C4, 0x3C5


class MIDIINCAPS(ctypes.Structure):
    _fields_ = [("wMid", wt.WORD), ("wPid", wt.WORD), ("vDriverVersion", wt.UINT),
                ("szPname", wt.WCHAR * 32), ("dwSupport", wt.DWORD)]


class MIDIOUTCAPS(ctypes.Structure):
    _fields_ = [("wMid", wt.WORD), ("wPid", wt.WORD), ("vDriverVersion", wt.UINT),
                ("szPname", wt.WCHAR * 32), ("wTechnology", wt.WORD), ("wVoices", wt.WORD),
                ("wNotes", wt.WORD), ("wChannelMask", wt.WORD), ("dwSupport", wt.DWORD)]


class MIDIHDR(ctypes.Structure):
    pass


MIDIHDR._fields_ = [("lpData", ctypes.c_void_p), ("dwBufferLength", wt.DWORD), ("dwBytesRecorded", wt.DWORD),
                    ("dwUser", ctypes.c_size_t), ("dwFlags", wt.DWORD), ("lpNext", ctypes.POINTER(MIDIHDR)),
                    ("reserved", ctypes.c_size_t), ("dwOffset", wt.DWORD), ("dwReserved", ctypes.c_size_t * 8)]

MIDIINPROC = ctypes.WINFUNCTYPE(None, wt.HANDLE, wt.UINT, ctypes.c_size_t, ctypes.c_size_t, ctypes.c_size_t)


def ins():
    for i in range(winmm.midiInGetNumDevs()):
        c = MIDIINCAPS()
        winmm.midiInGetDevCapsW(i, ctypes.byref(c), ctypes.sizeof(c))
        yield i, c.szPname


def outs():
    for i in range(winmm.midiOutGetNumDevs()):
        c = MIDIOUTCAPS()
        winmm.midiOutGetDevCapsW(i, ctypes.byref(c), ctypes.sizeof(c))
        yield i, c.szPname


def find(ports, name):
    for i, n in ports:
        if name.lower() in n.lower():
            return i
    sys.exit(f"no port matching {name!r}")


def monitor(name, seconds):
    t0 = time.perf_counter()
    count = [0]
    bufs = [ctypes.create_string_buffer(1024) for _ in range(4)]
    hdrs = [MIDIHDR() for _ in bufs]

    def cb(handle, msg, inst, p1, p2):
        if msg == MIM_DATA:
            b = [p1 & 0xFF, (p1 >> 8) & 0xFF, (p1 >> 16) & 0xFF]
            count[0] += 1
            print(f"{time.perf_counter() - t0:8.3f}  {b[0]:02x} {b[1]:02x} {b[2]:02x}", flush=True)
        elif msg == MIM_LONGDATA:
            h = ctypes.cast(p1, ctypes.POINTER(MIDIHDR)).contents
            if h.dwBytesRecorded:
                data = ctypes.string_at(h.lpData, h.dwBytesRecorded)
                count[0] += 1
                print(f"{time.perf_counter() - t0:8.3f}  sysex {data.hex(' ')}", flush=True)
        elif msg == MIM_ERROR:
            print(f"{time.perf_counter() - t0:8.3f}  error {p1:06x}", flush=True)

    proc = MIDIINPROC(cb)
    h = wt.HANDLE()
    r = winmm.midiInOpen(ctypes.byref(h), find(ins(), name), proc, 0, CALLBACK_FUNCTION)
    if r:
        sys.exit(f"midiInOpen: {r}")
    for b, hd in zip(bufs, hdrs):
        hd.lpData = ctypes.cast(b, ctypes.c_void_p)
        hd.dwBufferLength = len(b)
        winmm.midiInPrepareHeader(h, ctypes.byref(hd), ctypes.sizeof(hd))
        winmm.midiInAddBuffer(h, ctypes.byref(hd), ctypes.sizeof(hd))
    winmm.midiInStart(h)
    print(f"listening for {seconds} s", flush=True)
    time.sleep(seconds)
    winmm.midiInStop(h)
    winmm.midiInReset(h)
    for hd in hdrs:
        winmm.midiInUnprepareHeader(h, ctypes.byref(hd), ctypes.sizeof(hd))
    winmm.midiInClose(h)
    print(f"{count[0]} messages")


def open_out(name):
    h = wt.HANDLE()
    r = winmm.midiOutOpen(ctypes.byref(h), find(outs(), name), 0, 0, 0)
    if r:
        sys.exit(f"midiOutOpen: {r}")
    return h


def send(name, msgs):
    h = open_out(name)
    for m in msgs:
        b = bytes.fromhex(m)
        word = b[0] | (b[1] << 8 if len(b) > 1 else 0) | (b[2] << 16 if len(b) > 2 else 0)
        r = winmm.midiOutShortMsg(h, word)
        print(f"sent {b.hex(' ')}" + (f": error {r}" if r else ""))
        time.sleep(0.05)
    time.sleep(0.2)
    winmm.midiOutClose(h)


def sysex(name, data):
    h = open_out(name)
    b = ctypes.create_string_buffer(bytes.fromhex(data))
    hd = MIDIHDR(lpData=ctypes.cast(b, ctypes.c_void_p), dwBufferLength=len(b.raw) - 1)
    winmm.midiOutPrepareHeader(h, ctypes.byref(hd), ctypes.sizeof(hd))
    r = winmm.midiOutLongMsg(h, ctypes.byref(hd), ctypes.sizeof(hd))
    while not hd.dwFlags & 1:   # MHDR_DONE
        time.sleep(0.01)
    winmm.midiOutUnprepareHeader(h, ctypes.byref(hd), ctypes.sizeof(hd))
    winmm.midiOutClose(h)
    print(f"sent sysex {len(b.raw) - 1} bytes" + (f": error {r}" if r else ""))


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "list"
    if cmd == "list":
        for i, n in ins():
            print(f"in  {i}: {n}")
        for i, n in outs():
            print(f"out {i}: {n}")
    elif cmd == "monitor":
        monitor(sys.argv[2], float(sys.argv[3]))
    elif cmd == "send":
        send(sys.argv[2], sys.argv[3:])
    elif cmd == "sysex":
        sysex(sys.argv[2], sys.argv[3])
