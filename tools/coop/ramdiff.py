#!/usr/bin/env python3
"""ramdiff.py A.bin B.bin: differing address ranges between two --coop-ram images
(2 MB main RAM followed by the 1 KB scratchpad), largest first."""
import sys
a, b = open(sys.argv[1], 'rb').read(), open(sys.argv[2], 'rb').read()
runs, i, n = [], 0, min(len(a), len(b))
while i < n:
    if a[i] != b[i]:
        j = i
        while j < n and (a[j] != b[j] or a[j:j + 16] != b[j:j + 16]): j += 1
        runs.append((i, j)); i = j
    else: i += 1
print(sys.argv[1], 'differing bytes', sum(j - i for i, j in runs), 'in', len(runs), 'ranges')
for i, j in sorted(runs, key=lambda r: r[0] - r[1])[:25]:
    base = 0x80000000 + i if i < 0x200000 else 0x1F800000 + (i - 0x200000)
    print('  %08X..%08X  %6d bytes' % (base, base + (j - i), j - i))
