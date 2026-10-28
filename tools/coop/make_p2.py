#!/usr/bin/env python3
"""make_p2.py OUT.ac3i FIELDS [START]: a scripted second controller for the bench
   make_p2.py OUT.ac3i --copy REC: port 2 = port 1 of recording REC (the
   guest repeats the host's inputs, which makes the two planes easy to compare)
(--coop-p2 OUT.ac3i). Port 2 is an analog pad, plugged in from field 0; from
START (default 0) it repeats a 10-second pattern so its effect is easy to see:

    0-2 s  stick right (roll)        2-4 s  stick back (pull up)
    4-6 s  stick left                6-8 s  centred, Circle pressed every 0.5 s
    8-10 s centred, Square held

Port 1 in the file is left unplugged; the bench only takes port 2 from it."""
import struct, sys
CIRCLE, SQUARE = 1 << 13, 1 << 15
if sys.argv[2] == '--copy':
    src = open(sys.argv[3], 'rb').read()
    with open(sys.argv[1], 'wb') as o:
        o.write(src[:16])
        for i in range(16, len(src) - 23, 24):
            r = src[i:i + 24]
            o.write(r[:14] + r[4:14])
    sys.exit(0)
out, n = sys.argv[1], int(sys.argv[2])
start = int(sys.argv[3]) if len(sys.argv) > 3 else 0

def pad(f):
    lx = ly = 0x80; b = 0
    if f >= start:
        t = (f - start) % 600
        if t < 120: lx = 0xFF
        elif t < 240: ly = 0xFF
        elif t < 360: lx = 0x00
        elif t < 480: b = CIRCLE if (t % 30) < 4 else 0
        else: b = SQUARE
    return bytes([1, 0x73, b & 0xFF, b >> 8, lx, ly, 0x80, 0x80, 0, 0])

with open(out, 'wb') as o:
    o.write(b'AC3I' + struct.pack('<3I', 1, 24, 0))
    unplugged = bytes([0, 0x41, 0, 0, 0x80, 0x80, 0x80, 0x80, 0, 0])
    for f in range(n):
        o.write(struct.pack('<I', f) + unplugged + pad(f))
