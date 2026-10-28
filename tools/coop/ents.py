#!/usr/bin/env python3
"""ents.py ents.txt [FIELD]: live entity slots from a --coop-ents dump, decoded
with the layout in docs/coop/memory_map.md."""
import sys
want = int(sys.argv[2]) if len(sys.argv) > 2 else None
def s32(x): x = int(x, 16); return x - (1 << 32) if x & 0x80000000 else x
f = None
for line in open(sys.argv[1]):
    if line.startswith('=='):
        f = int(line.split()[2])
        if want is None or f == want: print(line.strip())
        continue
    if want is not None and f != want: continue
    p = line.split(); slot = int(p[1]); w = p[3:]
    if not int(w[0], 16) & 0x10000: continue          # not live
    print('%2d id=%05X type=%08X pos=(%7d,%7d,%7d) target=%s' % (
        slot, int(w[0], 16), int(w[1], 16), s32(w[2]) >> 12, s32(w[3]) >> 12, s32(w[4]) >> 12, w[20]))
