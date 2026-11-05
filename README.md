# Electrosphere Uplink

**Ace Combat 3: Electrosphere in your web browser with coop**, in English with an optional enhanced renderer: up to 6× resolution, widescreen, a draw distance to the horizon,
atmosphere, volumetric clouds, forests and an animated ocean.

You can play it yourself right now <a href="https://ac3uplink.com/"> here </a>

The game's own program runs on a recreation
of the PlayStation's system libraries, compiled to WebAssembly, and the
enhanced picture is drawn with WebGPU.

![alt text][menu]

[menu]: https://github.com/leanbgit/Electrosphere-uplink/blob/main/images/ac3main.PNG "Main menu"
## Features

- **Coop campaign**: create or join a public or private room and play throughout the whole campaign with a second player
- The full campaign, both discs, with memory card saves kept in the browser.
  Saves can be exported and imported, including DexDrive `.gme` files and
  `.mcd`/`.mcr` cards from emulators.
- English text from Project NEMO's patch.
- **Original** graphics (the console's picture, pixel for pixel) or
  **Enhanced** 2×, 3×, 4× or 6×. Enhanced graphics can be switched live while
  playing, effect by effect, including an *Original fog* mode that keeps the
  game's own draw distance.
- Customizable keyboard controls
- Automatic controller compatibility

![alt text][in game]

[in game]: https://github.com/leanbgit/Electrosphere-uplink/blob/main/images/ac3ingame.PNG "In game"
## Requirements

**To play:** your own copy of the game: both discs of the Japanese release
(SLPS-02020 and SLPS-02021, "Rev 1") as raw `.bin` images. You select them on
the page (they can be remembered in the browser); the site provides no game
data, and the English translation is added for you.

A desktop browser with WebAssembly and SharedArrayBuffer (all
current ones). Enhanced graphics need **WebGPU**: current Chrome or Edge,
Safari 26, or Firefox 141 on Windows. Without WebGPU the page falls back to the
original picture. A keyboard or a game controller is needed.

## Running it locally

You need [Emscripten](https://emscripten.org/docs/getting_started/downloads.html)
(tested with 3.1.52), Python 3 and a bash shell (on
Windows: Git Bash or WSL).

```sh
platform/web/build.sh

# serve it locally with the headers the page needs, and open the browser
python web/dist-public/serve.py       # http://localhost:8080
```

## Credits

- [Ace combat 3 static recompilation](https://github.com/sal063/Ace-Combat-3-static-recompilation)
- [Project NEMO's fan translation](https://projectnemo.net/wiki/index.php)
