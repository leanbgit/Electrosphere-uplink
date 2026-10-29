#!/usr/bin/env python3
"""strip_buttons.py IN.ac3i OUT.ac3i MASK FROM-TO: a copy of an input recording
with the buttons in MASK released on port 1 during fields FROM..TO (its card
snapshots are copied too). Example, the kill-credit test in docs/coop/S2_guest.md:
    strip_buttons.py session.ac3i nofire.ac3i 0x2000 6110-14000   # no Circle (missiles)
    make_p2.py p2copy.ac3i --copy session.ac3i                    # the guest keeps them"""
import os, shutil, sys
src, dst, mask = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)
lo, hi = (int(x) for x in sys.argv[4].split('-'))
d = bytearray(open(src, 'rb').read())
for f in range(lo, hi + 1):
    i = 16 + 24 * f
    if i + 24 > len(d): break
    b = (d[i + 6] | d[i + 7] << 8) & ~mask
    d[i + 6], d[i + 7] = b & 0xFF, b >> 8
open(dst, 'wb').write(d)
for s in (1, 2):
    if os.path.exists(f'{src}.card{s}.mcd'): shutil.copy(f'{src}.card{s}.mcd', f'{dst}.card{s}.mcd')
