# Electrosphere Uplink

**Ace Combat 3: Electrosphere in your web browser**, played from your own discs,
in English with an optional enhanced renderer: up to 6× resolution, widescreen, a draw distance to the horizon,
atmosphere, volumetric clouds, forests and an animated ocean.

The game's own program runs on a recreation
of the PlayStation's system libraries, compiled to WebAssembly, and the
enhanced picture is drawn with WebGPU.

> **No game data is included.** No disc images, no executable and no game
> code. You need your own copy of the Japanese release (both discs). Everything
> you select stays in your browser. Project NEMO's English translation patches
> are included.

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
- Customizable keyboard controls
- Automatic controller compatibility

## Requirements

**To play:** a desktop browser with WebAssembly and SharedArrayBuffer (all
current ones). Enhanced graphics need **WebGPU**: current Chrome or Edge,
Safari 26, or Firefox 141 on Windows. Without WebGPU the page falls back to the
original picture. A keyboard or a game controller is needed; there are no
touch controls yet.

**The game:** *Ace Combat 3: Electrosphere*, Japanese release, **Rev 1**,
both discs.

## Running it locally

You need [Emscripten](https://emscripten.org/docs/getting_started/downloads.html)
(tested with 3.1.52), Python 3 and a bash shell (on
Windows: Git Bash or WSL).

```sh
platform/web/build.sh

# serve it locally with the headers the page needs, and open the browser
python web/dist-public/serve.py       # http://localhost:8080
```

Then drop your two disc images on the page and press **Play**.
### No game code

`ac3.wasm` contains no code from the game. When you press Play, `ACE3.BIN` is
read from *your* disc 1 and run by a MIPS interpreter (`runtime/ac3_interp.c`).
On a desktop PC it runs several times faster than the game needs. The build
checks that no translated game function ended up in it.


## Credits

- [Ace combat 3 static recompilation](https://github.com/sal063/Ace-Combat-3-static-recompilation)
- [Project NEMO's fan translation](https://projectnemo.net/wiki/index.php)
