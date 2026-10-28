# Co-op research bench

Work for the online co-op campaign (branch `experimental-coop`). The plan is in
the project's `COOP_ROADMAP.md`; findings so far:

- [memory_map.md](memory_map.md): what is known about the game's player, entities and controllers
- [S1_determinism.md](S1_determinism.md): spike S1, can two browsers stay in step
- [S2_guest.md](S2_guest.md): spike S2, a second aircraft flown from controller port 2

## The headless bench

A native build of the website's engine (interpreter-only: it runs the player's
own `ACE3.BIN`) with no window and no sound device, plus research probes. It
runs about 400 fields per second (7× real time) on a 2-core Linux VM.

```sh
platform/headless/build.sh            # -> build-headless/ac3_headless
HD=1 platform/headless/build.sh       # + the enhanced renderer's game side
                                      #   (widescreen, effects), as the site's
                                      #   Enhanced mode, without a GPU
```

Needs a C compiler and Python 3; the HD build downloads Emscripten's `webgpu.h`
once (the renderer headers name its types).

### Replaying a recording

```sh
GAME=/path/to/game WORK=out tools/coop/replay.sh NAME rec/session.ac3i 20000 [options]
```

`GAME` holds `ACE3.BIN`, `disc/` (both Rev 1 images) and `translation/` (the
NEMO patches). Output goes to `out/runs/NAME.*`: the console log, a per-field
hash log, and whatever the options ask for.

### Probe options (`runtime/hle/coop_probe.h`)

| Option | Writes |
|---|---|
| `--coop-hash FILE` | per field: scene ids and hashes of RAM, scratchpad, VRAM, SPU state |
| `--coop-ents FROM-TO[:N] FILE` | entity slot table every N fields (decode with `tools/coop/ents.py`) |
| `--coop-ram F1,F2,... DIR` | full RAM + scratchpad images (compare with `tools/coop/ramdiff.py`) |
| `--coop-wlog ADDR LEN FROM-TO FILE` | every guest write into the range: field, pc, last call's return address, value |
| `--coop-rlog ADDR LEN FROM-TO FILE` | the same for reads |
| `--coop-calls FROM-TO FILE` | every guest function entered |
| `--coop-args F1,F2,... FROM-TO FILE` | a0–a3 and sp at each call of those functions |
| `--coop-wfn FUNC FROM-TO FILE` | every write (outside the stack) made while FUNC and its callees run |
| `--coop-p2 REC` | second controller taken from another recording's port 2 (make one with `tools/coop/make_p2.py`) |
| `--coop-guest` | the co-op guest aircraft (`runtime/hle/coop.c`, see S2_guest.md) |

Field windows: the game often runs at 30 frames per second in flight (it waits
two fields per frame), so a one-field window can contain no game code at all.
Use two-field windows.

`tools/coop/hashcmp.py A.tsv B.tsv ...` prints, for each run, the first field
where it stops matching run A.

All probes are compiled only with `AC3_COOP_PROBE`; the website build has none
of this.
