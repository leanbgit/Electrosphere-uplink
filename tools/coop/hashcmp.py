#!/usr/bin/env python3
"""hashcmp.py A.tsv B.tsv [C.tsv ...]: first field where each run's RAM,
scratchpad, VRAM, SPU state or "sim" (RAM without the two picture buffers)
stops matching run A (--coop-hash logs)."""
import sys
def load(p):
    return {int(r[0]): r for r in (l.rstrip('\n').split('\t') for l in list(open(p))[1:])}
a = load(sys.argv[1])
for p in sys.argv[2:]:
    b, first = load(p), {}
    for f in sorted(a):
        if f not in b: break
        for k, name in [(4, 'ram'), (5, 'scratch'), (6, 'vram'), (7, 'spu'), (8, 'sim')]:
            if k < len(a[f]) and k < len(b[f]) and a[f][k] != b[f][k] and name not in first: first[name] = f
    print(p, {k: first.get(k, 'same') for k in ['ram', 'scratch', 'vram', 'spu', 'sim']})
