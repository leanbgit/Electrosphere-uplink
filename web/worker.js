// worker.js -- runs the whole game (ac3.wasm) inside a dedicated Web Worker.
//
// Why a worker: the recompiled game has its own endless main loop and waits for
// each video field. A worker may block; the page's main thread may not.
//
// Shared memory with the page (SharedArrayBuffers created by app.js):
//   ctrl  Int32Array: [0] quit flag, [1] audio running flag, [2] sleep cell
//   hd.done Int32Array: [0] last field the render worker's GPU has finished
// Enhanced graphics: fields go to web/render-worker.js through hd.port.
//   pad   Int32Array(7): connected, analog, buttons, lx, ly, rx, ry
//   pad2  the same for controller 2 (co-op guest; optional)
//   ring  audio ring: Int32 header [read, write] + Int16 stereo samples
'use strict';
const DEV = true;    // site/ (build.sh): false (test-bench options ignored in the website)

const RAW = 2352, USER_OFF = 24, USER = 2048;
const EXE_SHA256 = '2c3f2ce41268176128973ea0cd9b28784a26c0061bff5550c99599388f13ebe9';
const DISC_NAME = n => `Ace Combat 3 - Electrosphere (Japan, Asia) (Disc ${n}) (Rev 1).bin`;

let ctrl, pad, pad2 = null, ringHead, ringData, ringFrames;
// enhanced graphics: port to the render worker and its "fields done" counter
let hdPort = null, hdDone = null, hdSkip = 0, hdSet = null, hdSetSeen = 0;
const reader = new FileReaderSync();

function log(text) { postMessage({ type: 'log', text }); }
let lastProgress = {};
function fail(text) { postMessage({ type: 'error', text }); }

// ---- ISO 9660 on a raw MODE2/2352 image (same logic as tools/ac3_disc.py)
function readUser(file, lba, nbytes) {
  const out = new Uint8Array(Math.ceil(nbytes / USER) * USER);
  for (let i = 0; i * USER < nbytes; i++) {
    const off = (lba + i) * RAW + USER_OFF;
    out.set(new Uint8Array(reader.readAsArrayBuffer(file.slice(off, off + USER))), i * USER);
  }
  return out.subarray(0, nbytes);
}
function volumeId(file) {
  const pvd = readUser(file, 16, USER);
  return String.fromCharCode(...pvd.subarray(40, 72)).trim();
}
function findRootFile(file, want) {
  const pvd = readUser(file, 16, USER);
  const dv = new DataView(pvd.buffer, pvd.byteOffset);
  const rootLba = dv.getUint32(156 + 2, true), rootSize = dv.getUint32(156 + 10, true);
  const dir = readUser(file, rootLba, rootSize);
  for (let off = 0; off < dir.length;) {
    const len = dir[off];
    if (len === 0) { off = (Math.floor(off / USER) + 1) * USER; continue; }
    const d = new DataView(dir.buffer, dir.byteOffset + off);
    const nlen = dir[off + 32];
    const name = String.fromCharCode(...dir.subarray(off + 33, off + 33 + nlen)).split(';')[0];
    if (name.toUpperCase() === want) return { lba: d.getUint32(2, true), size: d.getUint32(10, true) };
    off += len;
  }
  return null;
}
async function sha256hex(bytes) {
  const h = await crypto.subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(h)].map(b => b.toString(16).padStart(2, '0')).join('');
}

// ---- the interface ac3.wasm calls (see platform/web/web_platform.c)
self.AC3W = {
  quitRequested: () => Atomics.load(ctrl, 0) !== 0,
  padState: () => pad,
  pad2State: () => pad2,                             // co-op: controller 2, or null
  postFrame2(view, w, h, seq, own) {                 // co-op local test: the guest's view (own = 0: the host's picture)
    const copy = new Uint8Array(view);
    postMessage({ type: 'frame2', w, h, seq, own, buf: copy.buffer }, [copy.buffer]);
  },
  postFrame(view, w, h, seq) {
    if (seq === 1) sendNewEnglishCaches();
    if (seq === 1 && hdPort && !hdSkip)   // enhanced renderer missing from this build, or its setup failed
      postMessage({ type: 'hd-off', text: 'Enhanced graphics could not start (see Diagnostics); showing the original picture.' });
    const copy = new Uint8Array(view);               // detach from wasm memory
    postMessage({ type: 'frame', w, h, seq, buf: copy.buffer }, [copy.buffer]);
  },
  sleep(ms) { Atomics.wait(ctrl, 2, 0, ms); },
  // ---- enhanced graphics (platform/web/web_platform.c, AC3_HW_WEBGPU)
  hdWanted: () => !!hdPort,
  hdSend(bytes, seq) {
    if (seq === 1) sendNewEnglishCaches();
    if (seq > hdSkip) hdPort.postMessage(bytes.buffer, [bytes.buffer]);
    else Atomics.store(hdDone, 0, seq);     // development: fast-forward without drawing
    postMessage({ type: 'frame', seq });   // for the page's speed readout
  },
  hdFirst: seq => seq === hdSkip + 1,
  // live graphics settings from the page: the bits, or -1 when unchanged
  hdSettings() {
    if (!hdSet) return -1;
    const v = Atomics.load(hdSet, 1);
    if (v === hdSetSeen) return -1;
    hdSetSeen = v;
    return Atomics.load(hdSet, 0);
  },
  hdSkipping: seq => seq <= hdSkip,
  hdWait(seq) {
    // the game may run at most two fields ahead of the GPU (native: two frames
    // in flight). If the render worker stops answering, carry on after 2 s.
    for (let t = 0; seq - Atomics.load(hdDone, 0) > 2 && t < 20; t++) {
      if (Atomics.wait(hdDone, 0, Atomics.load(hdDone, 0), 100) === 'timed-out' && t === 19)
        log('render worker is not answering; continuing without waiting');
    }
  },
  audioOk: () => Atomics.load(ctrl, 1) !== 0,
  audioQueued() {
    const r = Atomics.load(ringHead, 0), w = Atomics.load(ringHead, 1);
    return (w - r + ringFrames) % ringFrames;
  },
  audioPush(samples) {                               // interleaved s16 stereo
    let w = Atomics.load(ringHead, 1);
    const r = Atomics.load(ringHead, 0);
    const free = ringFrames - 1 - ((w - r + ringFrames) % ringFrames);
    const n = Math.min(free, samples.length >> 1);
    for (let i = 0; i < n; i++) {
      ringData[w * 2] = samples[i * 2];
      ringData[w * 2 + 1] = samples[i * 2 + 1];
      w = (w + 1) % ringFrames;
    }
    Atomics.store(ringHead, 1, w);
  },
  // start-up progress for the page's loading screen (stage, disc, done, total)
  progress(stage, disc, done, total) {
    const pct = total ? Math.floor(done * 100 / total) : 0;
    if (stage === lastProgress.stage && disc === lastProgress.disc && pct === lastProgress.pct) return;
    lastProgress = { stage, disc, pct };
    postMessage({ type: 'progress', stage, disc, done, total });
  },
  cardChanged(slot, path) {
    const data = self.Module.FS.readFile(path);
    postMessage({ type: 'card', slot, data }, [data.buffer]);
  },
};

// Development: input recordings the test pages asked for (--record-input
// /rec/...) go back to the page when the game stops, with their card snapshots.
function sendRecordings() {
  if (!DEV || !self.Module) return;
  const FS = self.Module.FS;
  if (!FS.analyzePath('/rec').exists) return;
  const files = [];
  for (const n of FS.readdir('/rec')) if (n !== '.' && n !== '..' && !n.includes('.run.')) files.push({ name: n, data: FS.readFile('/rec/' + n) });
  if (files.length) postMessage({ type: 'recording', files }, files.map(f => f.data.buffer));
}

// English data built by the game during start-up ("<disc>.en.cache") lands in
// /game/disc (in memory). Send it to the page once, to keep for the next start.
let providedCaches = new Set();
function sendNewEnglishCaches() {
  const FS = self.Module.FS;
  for (const n of [1, 2]) {
    const p = '/game/disc/' + DISC_NAME(n) + '.en.cache';
    if (providedCaches.has(n) || !FS.analyzePath(p).exists) continue;
    const data = FS.readFile(p);
    postMessage({ type: 'encache', disc: n, data }, [data.buffer]);
  }
}

// Read-ahead cache for the disc images: WORKERFS would otherwise do one
// FileReaderSync call per read (the English loader makes ~70,000 small reads).
// Reads are served from 1 MB blocks; the 48 most recent blocks stay in memory.
function installReadCache(WORKERFS) {
  const BLOCK = 1 << 20, MAX_BLOCKS = 48, blocks = new Map(), ids = new WeakMap();
  let nextId = 1;
  WORKERFS.stream_ops.read = function (stream, buffer, offset, length, position) {
    const blob = stream.node.contents, size = stream.node.size;
    if (position >= size) return 0;
    let id = ids.get(blob); if (!id) ids.set(blob, id = nextId++);
    const end = Math.min(size, position + length);
    let p = position;
    while (p < end) {
      const bi = Math.floor(p / BLOCK), key = id + ':' + bi;
      let b = blocks.get(key);
      if (b) blocks.delete(key);                      // refresh LRU position
      else {
        const s0 = bi * BLOCK;
        b = new Uint8Array(WORKERFS.reader.readAsArrayBuffer(blob.slice(s0, Math.min(size, s0 + BLOCK))));
        if (blocks.size >= MAX_BLOCKS) blocks.delete(blocks.keys().next().value);
      }
      blocks.set(key, b);
      const from = p - bi * BLOCK, n = Math.min(end - p, b.length - from);
      buffer.set(b.subarray(from, from + n), offset + (p - position));
      p += n;
    }
    return end - position;
  };
}

// A JS error inside one of these callbacks would unwind through the game and
// stop it, so each one is guarded: the error is reported once, the game goes on.
const reported = new Set();
for (const [name, fn] of Object.entries(self.AC3W)) {
  self.AC3W[name] = function (...args) {
    try { return fn.apply(this, args); }
    catch (err) {
      if (!reported.has(name)) { reported.add(name); log(`page-side error in ${name}: ${err && err.stack || err}`); }
      return undefined;
    }
  };
}
// 3. anything else that escapes is reported instead of disappearing
self.addEventListener('unhandledrejection', e => fail('Unhandled error: ' + (e.reason && e.reason.stack || e.reason)));
self.addEventListener('error', e => fail('Worker error: ' + (e.error && e.error.stack || e.message)));

// ---- start-up
self.onmessage = async (e) => {
  if (e.data.type !== 'start') return;
  const m = e.data;
  ctrl = new Int32Array(m.ctrl);
  pad = new Int32Array(m.pad);
  if (m.pad2) pad2 = new Int32Array(m.pad2);
  ringHead = new Int32Array(m.ring, 0, 2);
  ringData = new Int16Array(m.ring, 8);
  ringFrames = ringData.length >> 1;

  try {
    // 1. identify the discs by their volume ID, whatever the files are called
    self.AC3W.progress('check', 0, 0, 1);
    const discs = {};
    for (const f of m.discs) {
      const v = volumeId(f);
      if (v === 'ACE3ES1') discs[1] = f;
      else if (v === 'ACE3ES2') discs[2] = f;
      else log(`ignored ${f.name}: volume "${v}" is not an Ace Combat 3 disc`);
    }
    if (!discs[1]) return fail('Disc 1 not found among the selected files.');
    if (!discs[2]) log('Disc 2 not selected: the game will stop at the disc change.');

    // 2. extract ACE3.BIN from disc 1 and check it is the Rev 1 executable
    const ent = findRootFile(discs[1], 'ACE3.BIN');
    if (!ent) return fail('ACE3.BIN not found on disc 1.');
    const exe = readUser(discs[1], ent.lba, ent.size).slice();
    const hash = await sha256hex(exe);
    if (hash !== EXE_SHA256)
      return fail('This disc is not the Japanese "Rev 1" release (SLPS-02020/02021). ' +
                  'Only that version is supported.');
    // disc 2 carries the same executable: a different one means another release
    if (discs[2]) {
      const e2 = findRootFile(discs[2], 'ACE3.BIN');
      if (!e2 || await sha256hex(readUser(discs[2], e2.lba, e2.size).slice()) !== EXE_SHA256)
        return fail('Disc 2 is not the Japanese "Rev 1" release (SLPS-02021), or it was modified. ' +
                    'Only that version is supported.');
    }
    log(`disc 1 OK (ACE3.BIN ${ent.size} bytes, Rev 1)` + (discs[2] ? ', disc 2 OK' : ''));

    // 3. enhanced graphics: the page has started the render worker already
    if (m.hd) {
      hdPort = m.hd.port; hdDone = new Int32Array(m.hd.done); hdSkip = DEV && m.hd.skip || 0;
      if (m.hd.settings) hdSet = new Int32Array(m.hd.settings);
    }

    // 4. load and start the program
    self.AC3W.progress('engine', 0, 0, 1);
    importScripts('ac3.js');
    const blobs = [];
    for (const n of [1, 2]) if (discs[n]) blobs.push({ name: DISC_NAME(n), data: discs[n] });
    for (const c of m.caches) { blobs.push({ name: DISC_NAME(c.disc) + '.en.cache', data: c.file }); providedCaches.add(c.disc); }
    const patches = m.patches.map(p => ({ name: `[ProjectNEMO] AC3e CD${p.disc} English.xdelta`, data: p.file }));

    const args = ['--boot', '--window', '--frames', '0', '--exe', '/game/ACE3.BIN',
                  '--disc', '/game/disc/' + DISC_NAME(1), m.english ? '--english' : '--japanese'];
    if (!discs[2]) args.push('--one-disc');
    if (hdPort) {
      args.push('--scale', String(m.hd.scale), m.hd.wide ? '--widescreen' : '--no-widescreen');
    }
    // Development only (test bench): extra files copied into /rec and extra
    // game arguments, e.g. an input recording for --play-input. The page never
    // sends these, and the website's build ignores them.
    const extraFiles = DEV && m.extraFiles || [];
    if (DEV && m.noPace) self.AC3W.sleep = () => {};   // development: run as fast as possible
    if (DEV) args.push(...(m.extraArgs || []));

    await createAC3({
      arguments: args,
      print: t => log(t),
      printErr: t => log(t),
      preRun: [function (mod) {
        // createAC3() never returns (the game's main loop runs inside it), so the
        // module object must be taken here, not from the awaited result.
        self.Module = mod;
        const FS = mod.FS;
        if (!FS.filesystems.WORKERFS.reader) FS.filesystems.WORKERFS.reader = new FileReaderSync();
        installReadCache(FS.filesystems.WORKERFS);
        FS.mkdir('/game');
        FS.mkdir('/game/src');            // the user's files, read-only, read in place
        FS.mkdir('/game/disc');           // in memory: links to them + room for .en.cache
        FS.mkdir('/game/translation');
        FS.mount(FS.filesystems.WORKERFS, { blobs }, '/game/src');
        for (const b of blobs) FS.symlink('/game/src/' + b.name, '/game/disc/' + b.name);
        if (patches.length) FS.mount(FS.filesystems.WORKERFS, { blobs: patches }, '/game/translation');
        FS.writeFile('/game/ACE3.BIN', exe);
        if (extraFiles.length || (DEV && m.extraArgs && m.extraArgs.includes('--record-input'))) {
          FS.mkdir('/rec');
          for (const x of extraFiles) FS.writeFile('/rec/' + String(x.name).replace(/[\/\\]/g, '_'), new Uint8Array(x.data));
        }
        FS.mkdir('/saves');
        for (const c of m.cards) FS.writeFile(`/saves/card${c.slot + 1}.mcd`, new Uint8Array(c.data));
        FS.chdir('/');
        self.AC3W.progress('boot', 0, 0, 1);
      }],
      onExit: code => { sendRecordings(); postMessage({ type: 'exit', code }); },
      onAbort: what => fail('The game stopped (abort): ' + what),
    });
  } catch (err) {
    if (err && err.name === 'ExitStatus') postMessage({ type: 'exit', code: err.status });
    else fail(String(err && err.stack || err));
  }
};
