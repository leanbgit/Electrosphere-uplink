#!/usr/bin/env python3
"""
make_gamedata.py -- prepares the game data the website streams to players.

    python tools/make_gamedata.py <disc1.bin> <disc2.bin> <out_dir>
                                  [--en1 disc1.en.cache --en2 disc2.en.cache
                                   --packs disc1.en.packs]

Splits the two raw disc images (and, optionally, the prebuilt English data,
so players skip the minute-long English build, and the comparison of the two
discs' English ACE.BPB, which would otherwise read most of both discs before
the game starts) into 4 MiB pieces under
<out_dir>/v<id>/<name>/NNN.bin and writes <out_dir>/manifest.json. The
website's data worker (web/data-worker.js) fetches the pieces it needs,
keeps them in the browser's storage, and serves them to the game.

Pieces stay below hosting limits (Cloudflare Pages: 25 MiB per file, 20,000
files), and 4 MiB is a multiple of the game worker's 1 MiB read block.

The output is game data: it goes to the web host only (site/gamedata/),
never into the repository (.gitignore covers *.bin and site/).
"""
import argparse, hashlib, json, os, sys

CHUNK = 4 << 20
RAW, USER_OFF, USER = 2352, 24, 2048
EXE_SHA256 = '2c3f2ce41268176128973ea0cd9b28784a26c0061bff5550c99599388f13ebe9'


def read_user(f, lba, n):
    out = bytearray()
    i = 0
    while len(out) < n:
        f.seek((lba + i) * RAW + USER_OFF)
        out += f.read(USER)
        i += 1
    return bytes(out[:n])


def check_disc(path, want_volume):
    """Japanese Rev 1 disc with the expected volume ID, or exit."""
    with open(path, 'rb') as f:
        pvd = read_user(f, 16, USER)
        vol = pvd[40:72].decode('ascii', 'replace').strip()
        if vol != want_volume:
            sys.exit(f'{path}: volume "{vol}", expected {want_volume}')
        root_lba = int.from_bytes(pvd[158:162], 'little')
        root_size = int.from_bytes(pvd[166:170], 'little')
        d = read_user(f, root_lba, root_size)
        off = 0
        while off < len(d):
            ln = d[off]
            if ln == 0:
                off = (off // USER + 1) * USER
                continue
            name = d[off + 33:off + 33 + d[off + 32]].decode('ascii', 'replace').split(';')[0]
            if name.upper() == 'ACE3.BIN':
                lba = int.from_bytes(d[off + 2:off + 6], 'little')
                size = int.from_bytes(d[off + 10:off + 14], 'little')
                if hashlib.sha256(read_user(f, lba, size)).hexdigest() != EXE_SHA256:
                    sys.exit(f'{path}: not the Japanese "Rev 1" release')
                return
            off += ln
    sys.exit(f'{path}: ACE3.BIN not found')


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('disc1'); ap.add_argument('disc2'); ap.add_argument('out_dir')
    ap.add_argument('--en1'); ap.add_argument('--en2')
    ap.add_argument('--packs', help='<disc 1>.en.packs, written beside disc 1 by an English start of the game')
    a = ap.parse_args()
    check_disc(a.disc1, 'ACE3ES1')
    check_disc(a.disc2, 'ACE3ES2')
    inputs = [('disc1', a.disc1), ('disc2', a.disc2)]
    if a.en1 and a.en2:
        inputs += [('en1', a.en1), ('en2', a.en2)]
    elif a.en1 or a.en2:
        sys.exit('give both --en1 and --en2, or neither')
    if a.packs:
        if not a.en1: sys.exit('--packs goes with --en1 and --en2')
        inputs.append(('packs', a.packs))

    files = {}
    for name, path in inputs:
        files[name] = {'size': os.path.getsize(path), 'sha256': file_sha256(path)}
        print(f'{name}: {files[name]["size"]:,} bytes')
    vid = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()[:10]
    base = os.path.join(a.out_dir, 'v' + vid)
    count = 0
    for name, path in inputs:
        d = os.path.join(base, name)
        os.makedirs(d, exist_ok=True)
        with open(path, 'rb') as f:
            i = 0
            for piece in iter(lambda: f.read(CHUNK), b''):
                out = os.path.join(d, f'{i:03d}.bin')
                # resumable: a piece already written in full is kept
                if not (os.path.exists(out) and os.path.getsize(out) == len(piece)):
                    with open(out + '.part', 'wb') as o:
                        o.write(piece)
                    os.replace(out + '.part', out)
                i += 1
        files[name]['chunks'] = i
        count += i
    manifest = {'format': 1, 'version': 'v' + vid, 'chunk': CHUNK, 'files': files}
    with open(os.path.join(a.out_dir, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
    total = sum(v['size'] for v in files.values())
    print(f'{count} pieces, {total / 1048576:.0f} MB, in {base}')


if __name__ == '__main__':
    main()
