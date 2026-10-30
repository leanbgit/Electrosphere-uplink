# Co-op research bench

Work for the online co-op campaign (branch `experimental-coop`). The plan is in
the project's `COOP_ROADMAP.md`; findings so far:

- [memory_map.md](memory_map.md): what is known about the game's player, entities and controllers
- [S1_determinism.md](S1_determinism.md): spike S1, can two browsers stay in step
- [S2_guest.md](S2_guest.md): spike S2, a second aircraft flown from controller port 2
- [S3_view.md](S3_view.md): spike S3, the guest's own view

## The code

The co-op code is in `runtime/hle/`, split by topic (`coop_internal.h` has
the overview and the shared state; `coop_game.h` names the game's addresses):

| File | What it does |
|---|---|
| `coop.h` | the public entry points the rest of the runtime calls |
| `coop_game.h` | a map of the game: every function address, data address and structure field the co-op code uses, with a name and what it is |
| `coop.c` | routes each intercepted game call to the file that handles it; mission start and end |
| `coop_guest.c` | the guest's aircraft: creating it, swapping it in as "the player" around the player update, its copy of player memory |
| `coop_view.c` | the guest's own picture: replaying the drawing part of the frame with the guest swapped in |
| `coop_hud.c` | HUD state per player, warnings, missiles coming at each player, the other player on the radar and on screen |
| `coop_enemies.c` | enemies treating the guest as a player: waking up, the aircraft AI, ground and ship weapons, missile launches, lock-on alerts |
| `coop_mission.c` | mission rules about the player's position, hazards, effects around the player's aircraft |
| `coop_probe.c` | research probes for the headless bench (hashes, logs, RAM images) |

## Local co-op test page (the Phase 2 checkpoint)

`web/test/coop.html`, copied into `web/dist-public/` by `platform/web/build.sh`
(not part of `site/`). Open it from the same address as the game, e.g.
`http://localhost:8080/coop.html` with `serve.bat`: it uses the discs, English
data and memory card the main page remembered (the card is only read).

- Player 1 (host) and player 2 (guest) each pick a source: keyboard (the main
  page's default keys), a gamepad, or nothing.
- Both views side by side; original graphics only for now.
- "Record this session" records both controllers (`--record-input`); the files
  are offered for download when you press Stop. Replay them with the bench
  (`--coop-guest`, plus `--coop-view` for the guest's view) to reproduce a
  session exactly.
- The game gets `--coop-guest --coop-split` (development option, ignored on the
  website). Co-op switches are parsed in `host/main.c` in every build.

`platform/web/run_worker_node.js ... --game-args "--coop-guest --coop-split"`
runs the same thing under Node; with `--shot-every N` it also writes the
guest's view as `guest_NNNNNN.ppm`.

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
| `--coop-hash FILE` | per field: scene ids and hashes of RAM, scratchpad, VRAM, SPU state, and "sim" (RAM without the two picture buffers) |
| `--coop-ents FROM-TO[:N] FILE` | entity slot table every N fields (decode with `tools/coop/ents.py`) |
| `--coop-ram F1,F2,... DIR` | full RAM + scratchpad images (compare with `tools/coop/ramdiff.py`) |
| `--coop-wlog ADDR LEN FROM-TO FILE` | every guest write into the range: field, pc, last call's return address, value |
| `--coop-rlog ADDR LEN FROM-TO FILE` | the same for reads |
| `--coop-calls FROM-TO FILE` | every guest function entered |
| `--coop-args F1,F2,... FROM-TO FILE` | a0–a3 and sp at each call of those functions |
| `--coop-wfn FUNC FROM-TO FILE` | every write (outside the stack) made while FUNC and its callees run |
| `--coop-rfn FUNC FROM-TO FILE` | the same for reads |
| `--coop-p2 REC` | second controller taken from another recording's port 2 (make one with `tools/coop/make_p2.py`) |
| `--coop-guest` | the co-op guest aircraft (`runtime/hle/coop*.c`, see S2_guest.md) |
| `--coop-view` | with `--coop-guest`: draw the guest's view (S3_view.md); screenshots show it |

Field windows: the game often runs at 30 frames per second in flight (it waits
two fields per frame), so a one-field window can contain no game code at all.
Use two-field windows.

`tools/coop/hashcmp.py A.tsv B.tsv ...` prints, for each run, the first field
where it stops matching run A.

All probes are compiled only with `AC3_COOP_PROBE`; the website build has none
of this.
## Performance

Co-op runs the game's frame, then the guest's update, then a replay of the
frame's drawing part for the guest's view. With `AC3_COOP_PROF=1` the
headless and web builds print a timing line every 600 frames (host update,
guest update, snapshot, replay copies, replay tick, replay draw, enemy AI).

- The replay runs directly in the snapshot buffer (`ac3_ram` points at it
  for the duration) instead of copying 2 MB of memory three times a frame.
  The host's aircraft block for the effects replay is kept in
  `G.host_block` at the snapshot.
- On the host's computer nobody sees the guest's picture, so the replay
  skips the flight frame's 3D drawing (`FN_DRAW_TERRAIN`, `FN_DRAW_WORLD`,
  `coop_view_skip_3d`). The HUD's decisions are unchanged: both computers'
  memory hashes stay identical (`tools/coop/hashcmp.py`).

Flight frame on the host's computer: 3.37 ms before, about 2.7 ms after.
