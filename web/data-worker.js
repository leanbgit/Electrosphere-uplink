// data-worker.js -- the game data, streamed from the website.
//
// The site hosts the two disc images (and the prebuilt English data) as 4 MiB
// pieces under gamedata/ (tools/make_gamedata.py writes them, with
// gamedata/manifest.json). This worker:
//   - serves reads to the game worker, which cannot wait for promises (it
//     blocks in Atomics.wait between fields): the game worker posts a request
//     on a MessagePort and sleeps on a shared buffer; this worker answers by
//     filling the buffer and waking it;
//   - keeps everything it has downloaded in the browser's private storage
//     (OPFS, one file per disc): whole pieces, and the 64 KiB blocks the game
//     read before their piece was complete, so later starts need no network;
//   - while a game runs ('download'), fills the rest in the background: the
//     pieces the game is reading first (and the one after), then the rest in
//     order. The game's own reads always come first.
// Without OPFS (some private windows) it keeps the most recent pieces in memory.
'use strict';

let man = null;                 // manifest: { version, chunk, files: { name: { size, chunks } } }
let store = null;               // name -> { handle (SyncAccessHandle) | null, have: Uint8Array, haveHandle, subHave: Uint8Array, subHandle }
const inflight = new Map();     // 'name:i' -> Promise<void>
const mem = new Map();          // fallback without OPFS: 'name:i' -> Uint8Array (LRU)
const MEM_MAX = 48;
let background = false, bgRunning = 0, urgent = 0;
let storeLocked = false;        // the stored copy could not be opened (another tab has it, or no storage)
let storeFull = false, fetched = 0;   // bytes downloaded in this session          // the browser's storage quota ran out: further pieces are kept in memory only
const BG_CONCURRENCY = 3;
let statusTimer = 0;
const ORDER = ['en1', 'en2', 'disc1', 'disc2'];   // what the game reads first

const post = m => self.postMessage(m);

async function init(url) {
  const r = await fetch(url, { cache: 'no-cache' });
  if (!r.ok) throw new Error(`no game data on this site (${r.status})`);
  man = await r.json();
  if (man.format !== 1 || !man.files || !man.chunk) throw new Error('unknown game data format');
  store = {};
  let dir = null;
  try {
    const root = await navigator.storage.getDirectory();
    for await (const [name] of root.entries())     // older versions of the data
      if (name.startsWith('gamedata-') && name !== 'gamedata-' + man.version) await root.removeEntry(name, { recursive: true }).catch(() => {});
    dir = await root.getDirectoryHandle('gamedata-' + man.version, { create: true });
  } catch { dir = null; }
  for (const [name, f] of Object.entries(man.files)) {
    const s = { handle: null, have: new Uint8Array(f.chunks), haveHandle: null,
                subHave: new Uint8Array(Math.ceil(f.size / SUB)), subHandle: null };
    if (dir && typeof FileSystemFileHandle !== 'undefined' && FileSystemFileHandle.prototype.createSyncAccessHandle) {
      try {
        s.handle = await (await dir.getFileHandle(name + '.img', { create: true })).createSyncAccessHandle();
        s.haveHandle = await (await dir.getFileHandle(name + '.have', { create: true })).createSyncAccessHandle();
        s.subHandle = await (await dir.getFileHandle(name + '.subs', { create: true })).createSyncAccessHandle();
        const n = s.haveHandle.read(s.have, { at: 0 });
        if (n < f.chunks) s.have.fill(0, n);
        const k = s.subHandle.read(s.subHave, { at: 0 });
        if (k < s.subHave.length) s.subHave.fill(0, k);
        // (the image grows as pieces are written: no space is taken up front)
      } catch (e) {
        s.handle = null; s.haveHandle = null; s.subHandle = null; s.have.fill(0); s.subHave.fill(0);
        if (!storeLocked) post({ type: 'log', text: e && e.name === 'NoModificationAllowedError'
          ? 'game data: another tab of this site is using the stored copy; this tab downloads what it reads (kept in memory)'
          : 'game data: this browser cannot store it (' + (e && e.message || e) + '); the game downloads what it reads' });
        storeLocked = true;
      }
    }
    store[name] = s;
  }
  status(true);
}

function counts() {
  let have = 0, total = 0, bytesHave = 0, bytesTotal = 0;
  for (const [name, f] of Object.entries(man.files)) {
    const h = store[name].have, sh = store[name].subHave, per = man.chunk / SUB;
    for (let i = 0; i < f.chunks; i++) {
      const sz = Math.min(man.chunk, f.size - i * man.chunk);
      total++; bytesTotal += sz;
      if (h[i]) { have++; bytesHave += sz; }
      else for (let j = i * per; j < Math.min(sh.length, (i + 1) * per); j++) if (sh[j]) bytesHave += Math.min(SUB, f.size - j * SUB);
    }
  }
  return { have, total, bytesHave, bytesTotal };
}

function status(now) {
  if (statusTimer && !now) return;
  const send = () => { statusTimer = 0; post({ type: 'status', ...counts(), stored: !!Object.values(store)[0].handle, full: storeFull, fetched, background, version: man.version }); };
  if (now) send(); else statusTimer = setTimeout(send, 400);
}

async function fetchPiece(name, i) {
  const f = man.files[name];
  const want = Math.min(man.chunk, f.size - i * man.chunk);
  const url = pieceUrl(name, i);
  let lastErr = null;
  for (let attempt = 0; attempt < 4; attempt++) {
    try {
      const r = await fetch(url);
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const b = new Uint8Array(await r.arrayBuffer());
      fetched += b.length;
      if (b.length !== want) throw new Error(`piece ${name}/${i} has ${b.length} bytes, expected ${want}`);
      return b;
    } catch (e) {
      lastErr = e;
      await new Promise(res => setTimeout(res, 500 * (attempt + 1)));
    }
  }
  throw lastErr;
}

// make piece i of `name` available (downloaded and stored)
function ensure(name, i) {
  const s = store[name];
  if (mem.has(name + ':' + i) || (s.have[i] && s.handle)) return Promise.resolve();
  const key = name + ':' + i;
  if (inflight.has(key)) return inflight.get(key);
  const p = fetchPiece(name, i).then(b => {
    let stored = false;
    if (s.handle && !storeFull) {
      try {
        stored = s.handle.write(b, { at: i * man.chunk }) === b.length;
        if (stored) s.handle.flush();
      } catch { stored = false; }
      if (stored) {
        s.have[i] = 1;
        s.haveHandle.write(s.have.subarray(i, i + 1), { at: i });
        s.haveHandle.flush();
      } else {
        storeFull = true; background = false;
        post({ type: 'log', text: 'game data: this browser\'s storage is full; the rest is downloaded when needed' });
      }
    }
    if (!stored) {                       // kept in memory only (no storage, or storage full)
      mem.set(key, b);
      while (mem.size > MEM_MAX) mem.delete(mem.keys().next().value);
    }
    status(false);
  }).finally(() => inflight.delete(key));
  inflight.set(key, p);
  return p;
}

function readInto(name, off, len, out) {
  const s = store[name];
  let p = off, o = 0;
  while (o < len) {
    const i = Math.floor(p / man.chunk), at = p - i * man.chunk;
    const n = Math.min(len - o, man.chunk - at);
    if (!mem.has(name + ':' + i)) s.handle.read(out.subarray(o, o + n), { at: p });
    else { const b = mem.get(name + ':' + i); out.set(b.subarray(at, at + n), o); mem.delete(name + ':' + i); mem.set(name + ':' + i, b); }
    p += n; o += n;
  }
}

// ---- small ranges for the game's urgent reads
// At start-up the game looks at a few sectors spread over the whole disc
// (its stream index). Fetching whole 4 MiB pieces for that would download
// hundreds of MB before the title screen, so a read of a piece that is not
// stored yet fetches only the 128 KiB blocks it touches (HTTP Range). The
// background download still fetches and stores whole pieces.
const SUB = 64 << 10, SUB_MAX = 1024;
const subs = new Map(), subInflight = new Map();   // 'name:j' -> Uint8Array / Promise (blocks not stored)
const hot = new Set();                              // 'name:i': pieces the game is reading (downloaded whole first)
// block j is in the browser's storage (on its own, or as part of a whole piece)
const blockStored = (name, j) => { const s = store[name]; return !!s.handle && (s.subHave[j] === 1 || s.have[Math.floor(j * SUB / man.chunk)] === 1); };
// keep fetched blocks j0..j1 (b: their bytes) in storage; false: storage not usable
function storeBlocks(name, j0, j1, b) {
  const s = store[name];
  if (!s.handle || storeFull) return false;
  try {
    if (s.handle.write(b, { at: j0 * SUB }) !== b.length) throw new Error('short write');
    s.handle.flush();
    s.subHave.fill(1, j0, j1 + 1);
    s.subHandle.write(s.subHave.subarray(j0, j1 + 1), { at: j0 });
    s.subHandle.flush();
    return true;
  } catch {
    storeFull = true; background = false;
    post({ type: 'log', text: 'game data: this browser\'s storage is full; the rest is downloaded when needed' });
    return false;
  }
}
let rangeOK = true;                                 // the host honours Range requests
function pieceUrl(name, i) { return `gamedata/${man.version}/${name}/${String(i).padStart(3, '0')}.bin`; }
// fetch blocks j0..j1 (same piece) in one request
function fetchSubs(name, j0, j1) {
  const f = man.files[name], start = j0 * SUB, end = Math.min(f.size, (j1 + 1) * SUB);
  const i = Math.floor(start / man.chunk), at = start - i * man.chunk;
  const p = (async () => {
    let lastErr = null;
    for (let attempt = 0; attempt < 4; attempt++) {
      try {
        const r = await fetch(pieceUrl(name, i), { headers: { Range: `bytes=${at}-${at + (end - start) - 1}` } });
        let b;
        if (r.status === 200) { rangeOK = false; b = new Uint8Array(await r.arrayBuffer()).slice(at, at + (end - start)); }
        else if (r.status === 206) b = new Uint8Array(await r.arrayBuffer());
        else throw new Error(`HTTP ${r.status}`);
        fetched += b.length; status(false);
        if (b.length !== end - start) throw new Error(`range of ${b.length} bytes, expected ${end - start}`);
        if (!storeBlocks(name, j0, j1, b)) {
          for (let j = j0; j <= j1; j++) subs.set(name + ':' + j, b.subarray((j - j0) * SUB, Math.min(b.length, (j - j0 + 1) * SUB)));
          while (subs.size > SUB_MAX) subs.delete(subs.keys().next().value);
        }
        return;
      } catch (e) { lastErr = e; await new Promise(res => setTimeout(res, 500 * (attempt + 1))); }
    }
    throw lastErr;
  })();
  for (let j = j0; j <= j1; j++) subInflight.set(name + ':' + j, p);
  p.finally(() => { for (let j = j0; j <= j1; j++) if (subInflight.get(name + ':' + j) === p) subInflight.delete(name + ':' + j); }).catch(() => {});
  return p;
}
// make blocks j0..j1 of one piece available; missing neighbours share a request
function ensureSubs(name, j0, j1) {
  const waits = [];
  let run = -1;
  for (let j = j0; j <= j1 + 1; j++) {
    const key = name + ':' + j;
    const missing = j <= j1 && !subs.has(key) && !subInflight.has(key) && !blockStored(name, j);
    if (missing && run < 0) run = j;
    if (!missing && run >= 0) { waits.push(fetchSubs(name, run, j - 1)); run = -1; }
    if (j <= j1 && subInflight.has(key)) waits.push(subInflight.get(key));
  }
  return Promise.all(waits);
}
function ensureSub(name, j) { return ensureSubs(name, j, j); }
const pieceReady = (name, i) => mem.has(name + ':' + i) || (store[name].have[i] && store[name].handle);

async function gather(name, off, len, out) {
  const f = man.files[name];
  const end = Math.min(f.size, off + len);
  const waits = [];
  for (let p = off; p < end;) {
    const i = Math.floor(p / man.chunk), pe = Math.min(end, (i + 1) * man.chunk);
    if (!pieceReady(name, i)) {
      if (rangeOK) waits.push(ensureSubs(name, Math.floor(p / SUB), Math.floor((pe - 1) / SUB)));
      else waits.push(ensure(name, i));
      // read ahead: this piece and the next are downloaded whole, next in line
      hot.add(name + ':' + i);
      if (i + 1 < f.chunks) hot.add(name + ':' + (i + 1));
    }
    p = pe;
  }
  await Promise.all(waits);
  for (let p = off; p < end;) {
    const i = Math.floor(p / man.chunk), pe = Math.min(end, (i + 1) * man.chunk);
    if (pieceReady(name, i)) readInto(name, p, pe - p, out.subarray(p - off));
    else for (let q = p; q < pe;) {
      const j = Math.floor(q / SUB), qe = Math.min(pe, (j + 1) * SUB);
      if (blockStored(name, j)) store[name].handle.read(out.subarray(q - off, qe - off), { at: q });
      else {
        let b = subs.get(name + ':' + j);
        if (!b) { await ensureSub(name, j); b = subs.get(name + ':' + j); }
        if (b) out.set(b.subarray(q - j * SUB, qe - j * SUB), q - off);
        else store[name].handle.read(out.subarray(q - off, qe - off), { at: q });   // stored meanwhile
      }
      q = qe;
    }
    p = pe;
  }
  return end - off;
}

// ---- the game worker's requests: { name, off, len } -> shared buffer
function attachGame(port, sab) {
  const ctrl = new Int32Array(sab, 0, 4), data = new Uint8Array(sab, 16);
  port.onmessage = async e => {
    const { name, off, len } = e.data;
    urgent++;
    try {
      if (!man.files[name]) throw new Error('unknown game file ' + name);
      const n = await gather(name, off, len, data);
      Atomics.store(ctrl, 1, n);
      Atomics.store(ctrl, 0, 2);
    } catch (err) {
      post({ type: 'error', text: 'Could not download the game data: ' + (err && err.message || err) });
      Atomics.store(ctrl, 0, 3);
    }
    urgent--;
    Atomics.notify(ctrl, 0);
    pump();
  };
}

// ---- background download of everything not stored yet
// read-ahead: the pieces the game is reading now (also without storage: then
// they are kept in memory)
function nextHot() {
  for (const key of hot) {
    hot.delete(key);
    const [name, i] = [key.slice(0, key.lastIndexOf(':')), +key.slice(key.lastIndexOf(':') + 1)];
    if (!pieceReady(name, i) && !inflight.has(key)) return [name, i];
  }
  return null;
}
function nextMissing() {
  for (const name of ORDER) {
    const f = man.files[name]; if (!f) continue;
    const h = store[name].have;
    for (let i = 0; i < f.chunks; i++) if (!h[i] && !inflight.has(name + ':' + i)) return [name, i];
  }
  return null;
}
function pump() {
  // filling the storage needs storage; the read-ahead does not
  const fill = background && !storeFull && !!store[ORDER.find(n => man.files[n])].handle;
  while (bgRunning < (urgent ? 1 : BG_CONCURRENCY)) {       // the game's own reads come first
    const n = nextHot() || (fill ? nextMissing() : null);
    if (!n) { if (fill && !bgRunning) { background = false; status(true); } return; }
    bgRunning++;
    ensure(n[0], n[1]).catch(err => post({ type: 'log', text: 'game data: ' + (err && err.message || err) }))
      .finally(() => { bgRunning--; pump(); });
  }
}

self.onmessage = async e => {
  const m = e.data;
  try {
    if (m.type === 'init') { await init(m.url); post({ type: 'ready', manifest: man }); }
    else if (m.type === 'game') attachGame(m.port, m.sab);
    else if (m.type === 'download') { background = true; status(true); pump(); }
    else if (m.type === 'pause') { background = false; status(true); }
    else if (m.type === 'forget') {                 // the page's "Delete": frees the space too
      background = false; hot.clear();
      for (const s of Object.values(store)) {
        s.have.fill(0); s.subHave.fill(0);
        if (s.haveHandle) { s.haveHandle.truncate(0); s.haveHandle.write(s.have, { at: 0 }); s.haveHandle.flush(); }
        if (s.subHandle) { s.subHandle.truncate(0); s.subHandle.flush(); }
        if (s.handle) { s.handle.truncate(0); s.handle.flush(); }
      }
      mem.clear(); subs.clear(); storeFull = false; status(true);
    }
  } catch (err) {
    post({ type: m.type === 'init' ? 'unavailable' : 'error', text: String(err && err.message || err) });
  }
};
