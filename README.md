# Electrosphere Uplink

**Ace Combat 3: Electrosphere in your web browser**, played from your own discs,
in English thanks to Project NEMO's fan translation, with an optional enhanced
renderer: up to 6× resolution, widescreen, a draw distance to the horizon,
atmosphere, volumetric clouds, forests and an animated ocean.

Nothing is emulated chip by chip. The game's own program runs on a recreation
of the PlayStation's system libraries, compiled to WebAssembly, and the
enhanced picture is drawn with WebGPU.

> **No game data is included.** No disc images, no executable and no game
> code. You need your own copy of the Japanese release (both discs). Everything
> you select stays in your browser. Project NEMO's English translation patches
> are included, with their permission.

## Features

- The full campaign, both discs, with memory card saves kept in the browser.
  Saves can be exported and imported, including DexDrive `.gme` files and
  `.mcd`/`.mcr` cards from emulators.
- English text from Project NEMO's patch, built in the browser on the first
  start (about a minute) and remembered after that.
- **Original** graphics (the console's picture, pixel for pixel) or
  **Enhanced** 2×, 3×, 4× or 6×. Enhanced graphics can be switched live while
  playing, effect by effect, including an *Original fog* mode that keeps the
  game's own draw distance.
- Keyboard controls you can change, and any standard game controller.
- Disc images are remembered in the browser's private storage, so later visits
  start with one click.

## Requirements

**To play:** a desktop browser with WebAssembly and SharedArrayBuffer (all
current ones). Enhanced graphics need **WebGPU**: current Chrome or Edge,
Safari 26, or Firefox 141 on Windows. Without WebGPU the page falls back to the
original picture. A keyboard or a game controller is needed; there are no
touch controls yet.

**The game:** *Ace Combat 3: Electrosphere*, Japanese release, **Rev 1**,
both discs (SLPS-02020 and SLPS-02021), as raw `.bin` images with 2352-byte
sectors. Redump names them:

    Ace Combat 3 - Electrosphere (Japan, Asia) (Disc 1) (Rev 1).bin
    Ace Combat 3 - Electrosphere (Japan, Asia) (Disc 2) (Rev 1).bin

The page checks the disc and refuses other versions.

**The translation:** Project NEMO's English patches for this release (v0.9b3)
are included in `web/patches/` (`CD1.xdelta`, `CD2.xdelta`), with Project
NEMO's permission. The page applies them to your discs in the browser.

## Build and run it yourself

You need [Emscripten](https://emscripten.org/docs/getting_started/downloads.html)
(tested with 3.1.52; `emcc` on your `PATH`), Python 3 and a bash shell (on
Windows: Git Bash or WSL).

```sh
git clone <this repository> electrosphere-uplink
cd electrosphere-uplink

# the build: no game code in it (see "No game code" below)
platform/web/build.sh

# serve it locally with the headers the page needs, and open the browser
python web/dist-public/serve.py       # http://localhost:8080
```

Then drop your two disc images on the page and press **Play**.

The build writes two folders (both ignored by git):

| Folder | What it is |
|---|---|
| `web/dist-public/` | The build, plus development pages (`hdtest.html`, `bench.html`) |
| `site/` | The website: only what a visitor needs (about 12 MB, most of it the translation patches) |

To put the website online (Cloudflare Pages, Netlify or GitHub Pages), see
[`web/DEPLOY.md`](web/DEPLOY.md).

### No game code

`ac3.wasm` contains no code from the game. When you press Play, `ACE3.BIN` is
read from *your* disc 1 and run by a MIPS interpreter (`runtime/ac3_interp.c`).
On a desktop PC it runs several times faster than the game needs. The build
checks that no translated game function ended up in it.

(`build.sh recomp` makes a faster variant from game code translated ahead of
time by the native port's recompiler. That tool is not part of this
repository, and such a build must never be published.)

## How it works

```
 Page (main thread)        Game worker                       Render worker
 web/app.js, index.html    web/worker.js + ac3.wasm          web/render-worker.js + render.wasm
 ───────────────────       ─────────────────────             ──────────────────────────────
 buttons, settings,  ──►   runs the game's MIPS code,   ──►  owns the GPU (WebGPU) and
 keyboard/controller,      recreated PlayStation             draws each field at 2–6×
 sound output,             libraries and hardware;           with the world effects
 original picture          sends one packet per field
```

The three threads share memory (SharedArrayBuffer) for the controller state,
the audio ring buffer, the live graphics settings and the GPU's "field
finished" counter. That is why the site must be cross-origin isolated
(`_headers`, or the `coi-sw.js` service worker on hosts that cannot set headers).

**Start-up** (`web/worker.js`): the disc images are read in place. The ISO 9660
file system is parsed in JavaScript, each disc is identified by its volume ID,
and `ACE3.BIN` is extracted and checked by SHA-256. The NEMO patch
(xdelta/VCDIFF) is applied as an *overlay* (`runtime/hle/ac3_english.c`): a
table of the sectors that differ, instead of a patched 700 MB image. The
overlay is kept in IndexedDB.

**The game's code** runs in the interpreter. Every call into
Sony's Psy-Q libraries is intercepted by its address and handled by a C rewrite
in `runtime/hle/` ("high-level emulation"). The address table comes from
`tools/gen_hle_table.py`, using address lists only. The hardware those
libraries drove is recreated too:

| File | Recreates |
|---|---|
| `runtime/gte.c` | The geometry coprocessor (3D transforms, perspective) |
| `runtime/hle/gpu_sw.c`, `psyq_gpu.c` | The graphics chip: a software rasteriser into a 1024×512 VRAM |
| `runtime/hle/spu_core.c`, `psyq_spu.c`, `xa_adpcm.c` | The sound chip and CD audio |
| `runtime/hle/mdec.c` | The movie decoder |
| `runtime/hle/psyq_cd.c` | The CD drive, reading sectors from the images and the English overlay |
| `runtime/hle/psyq_card.c`, `psyq_pad.c` | Memory cards and controllers |

**Each field** (`platform/web/web_platform.c`): the game calls `VSync()` 60
times a second, which ends in `ac3_frame_advance()`. That function presents the
picture, pushes the field's sound into the ring read by `web/audio-worklet.js`,
reads the controller state the page wrote, and sleeps with `Atomics.wait` until
the next 1/60 s. Because the game runs in a worker, its original blocking loop
works unchanged.

**The enhanced renderer** is a layer on top of the software GPU, which keeps
drawing everything as before, so the game cannot tell the difference. Every
primitive it draws is also reported through `gpu_sw_hooks`:

- `renderer/hw/hw_glue.c` follows the game's frame (sky, terrain, objects,
  flare, HUD), reads the camera and decides what is drawn differently. It
  defers semi-transparent effects until the world is drawn and handles the
  game's distance fog.
- `runtime/pgxp.c` keeps sub-pixel vertex positions and depth from the GTE, so
  the high-resolution picture does not wobble.
- `renderer/hw/hw.c` and `renderer/webgpu/hw_fx_game.c` record the field's
  commands and effect data, and `renderer/webgpu/hw_packet.c` packs them into
  one packet for the render worker.
- In the render worker:
  - `renderer/webgpu/hw_webgpu.c` draws the primitives at high resolution with
    the console's blend modes;
  - `hw_fx_webgpu.c` runs far terrain, sky and atmosphere, clouds, forests,
    sun, post-processing and bloom;
  - `hw_ocean_webgpu.c` runs the FFT ocean on compute shaders;
  - the shaders are the GLSL of the native renderer (`renderer/shaders/`),
    converted to WGSL by `tools/shaders_to_wgsl.py`.

WebGPU reports finished work only through promises, which cannot resolve in a
worker that never returns to its event loop, as the game worker never does. So
the GPU lives in its own worker. That worker counts finished fields in shared
memory, and the game waits when it is two fields ahead.

## Repository layout

| Path | Contents |
|---|---|
| `web/` | The page (`index.html`, `app.js`), the workers, the audio worklet, local server, fonts, Project NEMO's patches (`web/patches/`), development pages in `web/test/` |
| `platform/web/` | Browser glue (`web_platform.c`), the build script, Node test benches |
| `runtime/` | CPU (interpreter and recompiled-code support), GTE, PGXP, settings |
| `runtime/hle/` | The recreated Psy-Q libraries and hardware, the English patcher, the disc and save handling |
| `renderer/hw/` | The enhanced renderer's game side: following the game's frame, recording its primitives |
| `renderer/webgpu/` | The enhanced renderer on WebGPU (render worker) and its CPU halves |
| `renderer/shaders/` | The GLSL shaders, converted to WGSL (`renderer/webgpu/wgsl_shaders.h`) by `tools/shaders_to_wgsl.py` |
| `host/` | Program entry, boot, self-tests |
| `tools/` | The Psy-Q address table generator and the shader converter |
| `recomp/` | Address lists of the game's functions and of the Psy-Q library functions it contains (addresses only, no code), used to route library calls to `runtime/hle/` |
| `tests/` | Runtime unit tests |

## Testing

Input recordings make every run repeatable. A recorded play session
(`--record-input`, made with the native port) is replayed field for field:

- `platform/web/run_worker_node.js` runs the browser files under Node against
  real discs, with replays, screenshots and speed measurement. `--hd-cpu` runs
  the enhanced renderer's game side without a GPU.
- `web/test/hdtest.html` renders replays with the enhanced renderer in a
  (headless) browser, for screenshots at chosen fields.
- `web/test/bench.html` measures the enhanced renderer on your own GPU: 2×, 4×
  and 6×, effects on and off, settings switched live.
- `tests/run_tests.sh` runs the runtime unit tests.

Replays of English recordings must run with English on, or they drift out of
step.

## Credits

- The native recompiled port of *Ace Combat 3* that this project builds on:
  its runtime, recreated libraries and enhanced renderer were brought to the
  browser here.
- Project NEMO's English fan translation (v0.9b3), included with their
  permission. The translation and the patches are their work.
- The Barlow and Barlow Condensed fonts by Jeremy Tribby (SIL Open Font
  License, `web/fonts/OFL.txt`).
- The PGXP idea (precise geometry for PlayStation 3D) comes from the emulator
  community.

## Legal

This is a fan project. It is not affiliated with or endorsed by Bandai Namco
Entertainment. *Ace Combat* is a trademark of its owner. The repository and the
website contain no game code and no disc data. Players supply their own discs,
which never leave their browser.

The code in this repository is released under the [MIT licence](LICENSE).
Project NEMO's translation patches in `web/patches/` are their work, included
with their permission, and are not covered by the MIT licence.
Third-party components keep their own licences (see Credits).
