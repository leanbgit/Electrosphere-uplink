// run_worker_node.js -- runs the *browser* files (web/dist/worker.js + ac3.js +
// ac3.wasm) unmodified under Node, by providing the few Web Worker APIs they use.
// This is the automated test bench for the web build: same code path as the
// browser, but headless, scriptable, and next to the real disc images.
//
// Usage:
//   node run_worker_node.js <dist dir> <disc1.bin> [disc2.bin] [options]
// Options:
//   --fields N            stop after N fields (default 1800)
//   --press a-b:mask,...  hold PS1 buttons (Start=8 Cross=16384 Circle=8192 ...)
//   --shot-every N        write shot_NNNNNN.ppm every N fields
//   --english P1 P2       NEMO patches (CD1, CD2)
//   --caches C1 C2        optional .en.cache files (disc 1, 2)
//   --save-caches DIR     write English data built during the run into DIR
//   --no-pace             run as fast as possible (speed measurement)
//   --shot-at F1,F2,...   screenshots at these fields
//   --find-subtitles      log letterboxed scenes with text in the bottom bar
//   --play-input REC      replay a native input recording (and its card snapshots)
//   --card file.mcd       preload memory card 1
//   --hd-cpu              run the enhanced renderer's game side (widescreen) without
//                         a GPU: original pictures, enhanced game logic (hd skip mode)
//   --no-wide             with --hd-cpu: widescreen off
//   --game-args "a b"     extra game command-line arguments (e.g. "--no-fx")
//   --coop REC2           co-op local test (as web/test/coop.html): controller 2
//                         from port 2 of REC2 (tools/coop/make_p2.py), both views;
//                         screenshots of the guest's view are written as guest_NNNNNN.ppm
'use strict';
const fs = require('fs'), path = require('path'), vm = require('vm');
const a = process.argv.slice(2);
const dist = path.resolve(a[0]);
const opt = (k, d) => (a.includes(k) ? a[a.indexOf(k) + 1] : d);
const discPaths = [];
for (let i = 1; i < a.length && !a[i].startsWith('--'); i++) discPaths.push(a[i]);
const maxFields = +opt('--fields', 1800);
const shotEvery = +opt('--shot-every', 0);
const shotAt = new Set((opt('--shot-at', '') || '').split(',').filter(Boolean).map(Number));
const findSubs = a.includes('--find-subtitles');
let subsOn = false;
const presses = (opt('--press', '') || '').split(',').filter(Boolean).map(x => {
  const [r, m] = x.split(':'); const [s, e] = r.split('-').map(Number); return { s, e: e || s, m: +m }; });

// ---- File / Blob stand-in backed by a real file (read lazily, like a browser File)
class DiskFile {
  constructor(p, start = 0, end = fs.statSync(p).size) { this.p = p; this.start = start; this.end = end; this.name = path.basename(p); }
  get size() { return this.end - this.start; }
  slice(s = 0, e = this.size) { return new DiskFile(this.p, this.start + s, this.start + Math.min(e, this.size)); }
}
const readStats = {};                           // per file: [reads, bytes]
globalThis.FileReaderSync = class {
  readAsArrayBuffer(f) {
    const st = readStats[f.name] || (readStats[f.name] = [0, 0]); st[0]++; st[1] += f.size;
    const buf = Buffer.alloc(f.size), fd = fs.openSync(f.p, 'r');
    fs.readSync(fd, buf, 0, f.size, f.start); fs.closeSync(fd);
    return buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length);
  }
};

// ---- Web Worker globals
globalThis.self = globalThis;
self.location = { href: 'file://' + path.join(dist, 'worker.js') };
globalThis.importScripts = (...urls) => { for (const u of urls) vm.runInThisContext(fs.readFileSync(path.join(dist, u), 'utf8'), { filename: u }); };
const realFetch = globalThis.fetch;
globalThis.fetch = async (url, o) => {
  const u = String(url);
  if (/^https?:/.test(u)) return realFetch(url, o);
  let p = decodeURIComponent(u.replace(/^file:\/\//, ''));
  if (!path.isAbsolute(p)) p = path.join(dist, p);
  return new Response(fs.readFileSync(p), { headers: { 'Content-Type': 'application/wasm' } });
};

// ---- shared memory, exactly as app.js creates it
const ctrl = new Int32Array(new SharedArrayBuffer(16));
const pad = new Int32Array(new SharedArrayBuffer(28));
const pad2 = new Int32Array(new SharedArrayBuffer(28));
pad2.set([1, 1, 0, 128, 128, 128, 128]);
const coopRec = opt('--coop', null) ? fs.readFileSync(opt('--coop', null)) : null;
let frames2 = 0, own2 = 0;
const ringFrames = 44100, ring = new SharedArrayBuffer(8 + ringFrames * 4), ringHead = new Int32Array(ring, 0, 2);
Atomics.store(ctrl, 1, 1);                     // "audio running": exercise the audio path too
pad.set([1, 0, 0, 128, 128, 128, 128]);

let fields = 0, t0 = Date.now(), lastBeat = Date.now(), cardWrites = 0;
globalThis.postMessage = (m) => {
  switch (m.type) {
    case 'log': console.log(m.text); break;
    case 'error': console.log('[page] ERROR: ' + m.text); process.exitCode = 3; break;
    case 'card': cardWrites++; console.log(`[page] memory card ${m.slot + 1} stored (${m.data.byteLength} bytes)`); break;
    case 'encache': {
      const out = opt('--save-caches', null);
      console.log(`[page] English data for disc ${m.disc} built (${(m.data.byteLength / 1048576).toFixed(1)} MB)`);
      if (out) fs.writeFileSync(path.join(out, `disc${m.disc}.en.cache`), Buffer.from(m.data));
      break;
    }
    case 'exit': console.log(`[page] program exited (${m.code})`); break;
    case 'recording':                                   // --record-input (coop.html's "Record this session")
      for (const f of m.files) { fs.writeFileSync(f.name, Buffer.from(f.data)); console.log(`[page] recording ${f.name} (${f.data.byteLength} bytes)`); }
      break;
    case 'frame2': {                                    // co-op: the guest's view
      frames2++; if (m.own) own2++;
      if ((shotEvery && fields % shotEvery === 0 || shotAt.has(fields)) && m.w) {
        const px = new Uint8Array(m.buf), rgb = Buffer.alloc(m.w * m.h * 3);
        for (let i = 0; i < m.w * m.h; i++) { rgb[i * 3] = px[i * 4]; rgb[i * 3 + 1] = px[i * 4 + 1]; rgb[i * 3 + 2] = px[i * 4 + 2]; }
        fs.writeFileSync(`guest_${String(fields).padStart(6, '0')}.ppm`, Buffer.concat([Buffer.from(`P6\n${m.w} ${m.h}\n255\n`), rgb]));
      }
      break;
    }
    case 'frame': {
      fields++;
      // drain one field of audio, like the AudioWorklet would
      const r = Atomics.load(ringHead, 0), w = Atomics.load(ringHead, 1);
      const q = (w - r + ringFrames) % ringFrames;
      Atomics.store(ringHead, 0, (r + Math.min(q, 735)) % ringFrames);
      // scripted input for the next field
      let b = 0; for (const p of presses) if (fields >= p.s && fields <= p.e) b |= p.m; pad[2] = b;
      if (coopRec) {                                   // controller 2 from the co-op recording's port 2
        const o = 16 + 24 * fields + 14;
        if (o + 8 <= coopRec.length) pad2.set([coopRec[o], coopRec[o + 1] === 0x73 ? 1 : 0, coopRec[o + 2] | coopRec[o + 3] << 8,
                                               coopRec[o + 4], coopRec[o + 5], coopRec[o + 6], coopRec[o + 7]]);
      }
      // --find-subtitles: log fields that look like a letterboxed scene with
      // text in the bottom bar (dark top/bottom bands, bright pixels inside the lower band)
      if (findSubs && m.w) {
        const px = new Uint8Array(m.buf), W = m.w, H = m.h;
        const lum = (x, y) => { const i = (y * W + x) * 4; return px[i] + px[i + 1] + px[i + 2]; };
        let topDark = true;
        for (let y = 0; y < Math.round(H * 0.08) && topDark; y++) for (let x = 0; x < W; x += 4) if (lum(x, y) > 60) { topDark = false; break; }
        let barStart = H;
        for (let y = H - 1; y > H / 2; y--) { let bright = 0; for (let x = 0; x < W; x += 2) if (lum(x, y) > 60) bright++; if (bright > W * 0.6) { barStart = y + 1; break; } }
        let text = 0;
        if (topDark && barStart < H - 8) for (let y = barStart; y < H; y++) for (let x = 0; x < W; x++) if (lum(x, y) > 300) text++;
        const on = topDark && barStart < H - 8 && text > 40;
        if (on && !subsOn) console.error(`[bench] subtitle scene starts at field ${fields} (bar ${H - barStart} px, ${text} text px)`);
        if (!on && subsOn) console.error(`[bench] subtitle scene ends at field ${fields}`);
        subsOn = on;
      }
      if ((shotEvery && fields % shotEvery === 0 || shotAt.has(fields)) && m.w) {
        const px = new Uint8Array(m.buf), rgb = Buffer.alloc(m.w * m.h * 3);
        for (let i = 0; i < m.w * m.h; i++) { rgb[i * 3] = px[i * 4]; rgb[i * 3 + 1] = px[i * 4 + 1]; rgb[i * 3 + 2] = px[i * 4 + 2]; }
        fs.writeFileSync(`shot_${String(fields).padStart(6, '0')}.ppm`, Buffer.concat([Buffer.from(`P6\n${m.w} ${m.h}\n255\n`), rgb]));
      }
      if (Date.now() - lastBeat > 5000) { console.error(`[bench] field ${fields}, ${(fields / ((Date.now() - t0) / 1000)).toFixed(0)} fields/s`); lastBeat = Date.now(); }
      if (fields >= maxFields) Atomics.store(ctrl, 0, 1);   // ask the game to quit, like the Stop button
      break;
    }
  }
};
self.addEventListener = (type, fn) => { if (type === 'unhandledrejection') process.on('unhandledRejection', reason => fn({ reason })); };
process.on('uncaughtException', e => { console.log('[bench] UNCAUGHT: ' + (e && e.stack || e)); process.exitCode = 4; });
process.on('exit', () => { for (const [n, [r, b]] of Object.entries(readStats)) console.error(`[bench] reads ${String(r).padStart(8)}  ${(b / 1048576).toFixed(1).padStart(8)} MB  ${n}`); });
process.on('exit', c => console.error(`[bench] end: ${fields} fields in ${((Date.now() - t0) / 1000).toFixed(1)} s, ${cardWrites} card writes, exit code ${c}` +
  (coopRec ? `, ${frames2} guest pictures (${own2} the guest's own view)` : '')));

// --play-input REC: replay a controller recording made by the native build
// (ac3.exe --record-input REC), starting from the card snapshots saved with it.
function playInput(rec) {
  if (!rec) return {};
  const name = path.basename(rec), extraFiles = [{ name, data: fs.readFileSync(rec).buffer }];
  for (const s of [1, 2]) if (fs.existsSync(`${rec}.card${s}.mcd`))
    extraFiles.push({ name: `${name}.card${s}.mcd`, data: fs.readFileSync(`${rec}.card${s}.mcd`).buffer });
  return { extraFiles, extraArgs: ['--play-input', '/rec/' + name] };
}

// ---- go: load worker.js and send it the same 'start' message the page sends
vm.runInThisContext(fs.readFileSync(path.join(dist, 'worker.js'), 'utf8'), { filename: 'worker.js' });
// --no-pace: skip the 59.94 Hz waits, to measure how fast the build can run
if (a.includes('--no-pace')) self.AC3W.sleep = () => {};
const en = a.includes('--english') ? a.slice(a.indexOf('--english') + 1, a.indexOf('--english') + 3) : null;
const ca = a.includes('--caches') ? a.slice(a.indexOf('--caches') + 1, a.indexOf('--caches') + 3) : null;
const card = opt('--card', null);
self.onmessage({ data: {
  type: 'start', ctrl: ctrl.buffer, pad: pad.buffer, ring, pad2: coopRec ? pad2.buffer : undefined,
  discs: discPaths.map(p => new DiskFile(p)),
  patches: en ? [{ disc: 1, file: new DiskFile(en[0]) }, { disc: 2, file: new DiskFile(en[1]) }] : [],
  caches: ca ? [{ disc: 1, file: new DiskFile(ca[0]) }, { disc: 2, file: new DiskFile(ca[1]) }] : [],
  english: !!en,
  cards: card ? [{ slot: 0, data: fs.readFileSync(card).buffer }] : [],
  ...(() => { const pi = playInput(opt('--play-input', null)); const ga = opt('--game-args', '');
              if (ga) pi.extraArgs = (pi.extraArgs || []).concat(ga.split(' '));
              if (coopRec) pi.extraArgs = (pi.extraArgs || []).concat(['--coop-guest', '--coop-split']);
              return pi; })(),
  hd: a.includes('--hd-cpu') ? { scale: 1, wide: !a.includes('--no-wide'), port: new MessageChannel().port1,
        done: new SharedArrayBuffer(4), skip: 1e9 } : null,
} });
