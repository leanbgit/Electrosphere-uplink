// run_node.js -- headless test driver for the WebAssembly build.
// Runs ac3_node.js (same program as the browser build, built for Node with
// direct file access) and stands in for the browser page:
//   no display (optionally dumps a frame as .ppm), no audio, no frame pacing.
// Usage: node run_node.js <ac3_node.js> [--shot-every N] -- <game arguments>
'use strict';
const fs = require('fs');
const argv = process.argv.slice(2);
const modPath = require('path').resolve(argv[0]);
const sep = argv.indexOf('--');
const opts = argv.slice(1, sep < 0 ? undefined : sep);
const gameArgs = sep < 0 ? [] : argv.slice(sep + 1);
const shotEvery = opts.includes('--shot-every') ? +opts[opts.indexOf('--shot-every') + 1] : 0;

const pad = new Int32Array([1, 0, 0, 128, 128, 128, 128]);
// --press "from-to:mask,from-to:mask"  hold PS1 button mask during those fields
// (Start=8, Cross=16384, Circle=8192, Up=16, Down=64, Left=128, Right=32)
const presses = opts.includes('--press') ? opts[opts.indexOf('--press') + 1].split(',').map(x => {
  const [r, m] = x.split(':'); const [a, b] = r.split('-').map(Number); return { a, b: b || a, m: +m }; }) : [];
let field = 0, queued = 0, pushed = 0;
const audio = opts.includes('--audio');
if (opts.includes('--analog')) pad[1] = 1;   // report a DualShock in analog mode, like the page does with a gamepad
let frames = 0, lastLog = Date.now();
globalThis.AC3W = {
  quitRequested: () => false,
  padState() {
    field++;
    queued = Math.max(0, queued - 735);
    pad[2] = 0;
    for (const p of presses) if (field >= p.a && field <= p.b) pad[2] |= p.m;
    return pad;
  },
  postFrame(view, w, h, seq) {
    frames++;
    if (Date.now() - lastLog > 5000) { console.error(`[node] field ${seq}`); lastLog = Date.now(); }
    if (shotEvery && seq % shotEvery === 0 && w && h) {
      const head = Buffer.from(`P6\n${w} ${h}\n255\n`);
      const rgb = Buffer.alloc(w * h * 3);
      for (let i = 0; i < w * h; i++) { rgb[i * 3] = view[i * 4]; rgb[i * 3 + 1] = view[i * 4 + 1]; rgb[i * 3 + 2] = view[i * 4 + 2]; }
      fs.writeFileSync(`shot_${String(seq).padStart(6, '0')}.ppm`, Buffer.concat([head, rgb]));
    }
  },
  sleep() {},
  // --audio: pretend a sound device consumes 44100 Hz stereo in real time
  audioOk: () => audio,
  audioQueued: () => queued,
  audioPush(samples) { queued += samples.length >> 1; pushed += samples.length >> 1; },
  cardChanged(slot, path) { console.error(`[node] card ${slot + 1} written (${path})`); },
};

const t0 = Date.now();
require(modPath)({ arguments: gameArgs }).then(
  () => console.error(`[node] finished after ${frames} fields, ${((Date.now() - t0) / 1000).toFixed(1)} s`),
  e => { console.error('[node] CRASH:', e && e.stack || e); process.exit(2); });
process.on('exit', c => console.error(`[node] exit ${c}, audio frames pushed ${pushed}, ${frames} fields in ${((Date.now() - t0) / 1000).toFixed(1)} s`));
