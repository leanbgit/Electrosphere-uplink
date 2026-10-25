// app.js -- page side: file selection, display, input, audio, saves.
'use strict';
const DEV = true;    // site/ (build.sh): false (test hooks off in the website)
// run state (declared first: the file panels read it while rendering)
let running = false, worker = null, ctrl = null, audioCtx = null, audioNode = null;
const $ = id => document.getElementById(id);
const logEl = $('log');
function log(t, cls) {
  const line = document.createElement('div');
  if (cls) line.className = cls;
  line.textContent = t;
  logEl.appendChild(line);
  if (logEl.childNodes.length > 2000) logEl.removeChild(logEl.firstChild);
  logEl.scrollTop = logEl.scrollHeight;
}

if (!self.crossOriginIsolated)
  log('This page is not cross-origin isolated, so the game cannot start. Open it through a web server ' +
      '(serve.bat locally, or a host that sends the COOP/COEP headers); opening the file directly does not work.', 'bad');

// --------------------------------------------------------------- game files
// Each needed file can come from three places, in this order of preference:
//   selected  - picked/dropped in this visit
//   stored    - remembered in this browser (disc images, in OPFS)
//   bundled   - shipped with the site: Project NEMO's patches/CD1.xdelta, CD2.xdelta
const RAW = 2352;
const have = { disc: {}, patch: {} };                // [kind][1|2] = { file | url, src }
let storing = 0;

// Ace Combat 3 "Rev 1" (SLPS-02020/02021): both discs carry this ACE3.BIN.
// Same value as web/worker.js, which checks again when the game starts.
const EXE_SHA256 = '2c3f2ce41268176128973ea0cd9b28784a26c0061bff5550c99599388f13ebe9';
const read = async (file, start, n) => new Uint8Array(await file.slice(start, start + n).arrayBuffer());
const text = b => String.fromCharCode(...b).trim();

// user data of raw MODE2/2352 sectors (the format the game needs)
async function readUser(file, lba, nbytes) {
  if (nbytes > 4 << 20) throw new Error('directory or file too large');
  const out = new Uint8Array(Math.ceil(nbytes / 2048) * 2048);
  for (let i = 0; i * 2048 < nbytes; i++) out.set(await read(file, (lba + i) * RAW + 24, 2048), i * 2048);
  return out.subarray(0, nbytes);
}
async function volumeId(file) { return text(await read(file, 16 * RAW + 24 + 40, 32)); }
// a 2048-byte-per-sector copy (.iso) loses the CD-XA audio and video the game streams
async function isoVolumeId(file) {
  const pvd = await read(file, 16 * 2048, 72);
  return text(pvd.subarray(1, 6)) === 'CD001' ? text(pvd.subarray(40, 72)) : '';
}
// SHA-256 of ACE3.BIN in the root directory, or '' when the image has none
async function exeHash(file) {
  const pvd = await readUser(file, 16, 2048), pv = new DataView(pvd.buffer);
  const dir = await readUser(file, pv.getUint32(158, true), pv.getUint32(166, true));
  for (let off = 0; off < dir.length;) {
    const len = dir[off];
    if (len === 0) { off = (Math.floor(off / 2048) + 1) * 2048; continue; }
    const name = text(dir.subarray(off + 33, off + 33 + dir[off + 32])).split(';')[0];
    if (name.toUpperCase() === 'ACE3.BIN') {
      const dv = new DataView(dir.buffer, dir.byteOffset + off);
      const exe = await readUser(file, dv.getUint32(2, true), dv.getUint32(10, true));
      return [...new Uint8Array(await crypto.subtle.digest('SHA-256', exe))].map(b => b.toString(16).padStart(2, '0')).join('');
    }
    off += len;
  }
  return '';
}

const NOT_RAW = 'The game needs a raw .bin image (2352-byte sectors, usually next to a .cue file): ' +
                'its music and videos are stored in a way that .iso, .img, .chd and .pbp copies lose or compress.';

async function classify(list) {
  for (const f of list) {
    const n = f.name.toLowerCase(), ext = n.includes('.') ? n.slice(n.lastIndexOf('.')) : '';
    if (n.endsWith('.en.cache')) {
      log(`${f.name}: not needed, the English data is built in this browser.`, 'dim');
    } else if (ext === '.xdelta') {
      log(`${f.name}: not needed, the English translation is included with the site.`, 'dim');
    } else if (ext === '.cue' || ext === '.ccd' || ext === '.sub') {
      log(`${f.name}: not needed, select the .bin file it belongs with.`, 'dim');
    } else if (['.zip', '.7z', '.rar', '.gz', '.ecm'].includes(ext)) {
      log(`${f.name}: compressed. Extract it first, then select the .bin file inside.`, 'bad');
    } else if (['.iso', '.img', '.chd', '.pbp', '.mdf', '.nrg', '.cso'].includes(ext)) {
      const v = f.size > 100e6 && (ext === '.iso' || ext === '.img') ? await isoVolumeId(f).catch(() => '') : '';
      log(`${f.name}: ${/^ACE3ES[12]$/.test(v) ? 'this is Ace Combat 3, but as a 2048-byte copy. ' : 'not a supported disc image format. '}${NOT_RAW}`, 'bad');
    } else if (ext === '.bin') {
      if (f.size < 100e6) {
        log(`${f.name}: too small for an Ace Combat 3 disc (${(f.size / 1048576).toFixed(0)} MB; each disc is about 690 MB). ` +
            'If the game is split into several .bin files, the large one is the disc.', 'bad');
        continue;
      }
      const v = await volumeId(f).catch(() => '');
      const d = v === 'ACE3ES1' ? 1 : v === 'ACE3ES2' ? 2 : 0;
      if (!d) {
        const iv = await isoVolumeId(f).catch(() => '');
        if (/^ACE3ES[12]$/.test(iv)) log(`${f.name}: this is Ace Combat 3, but as a 2048-byte copy. ${NOT_RAW}`, 'bad');
        else log(`${f.name}: not an Ace Combat 3 disc image (volume "${v.replace(/[^\x20-\x7e]/g, '?')}")`, 'bad');
        continue;
      }
      // the version check, before anything is kept: only "Rev 1" works
      let hash = '';
      try { hash = await exeHash(f); } catch { /* unreadable directory: hash stays '' */ }
      if (hash !== EXE_SHA256) {
        log(`${f.name}: Ace Combat 3 disc ${d}, but ${hash ? 'not the Japanese "Rev 1" release (SLPS-02020/02021), or a modified copy' : 'its files could not be read (damaged or modified image)'}. ` +
            'Only the unmodified Rev 1 discs are supported.', 'bad');
        continue;
      }
      have.disc[d] = { file: f, src: 'selected' };
      log(`${f.name}: disc ${d} (Rev 1)`, 'ok');
      if ($('remember').checked) storeDisc(d, f);    // runs in the background
    } else if (f.size === 0 && !f.type) {
      log(`${f.name}: a folder or an empty file. Select the .bin files themselves.`, 'dim');
    } else {
      log(`ignored ${f.name}: not a disc image (.bin)`, 'dim');
    }
  }
  renderFiles();
}

// ---- remembered disc images: Origin Private File System (OPFS)
async function opfs() {
  try { return navigator.storage && navigator.storage.getDirectory ? await navigator.storage.getDirectory() : null; }
  catch { return null; }
}
const discMark = d => `ac3.disc${d}`;               // set only once a copy is complete
async function storeDisc(d, file) {
  const dir = await opfs();
  if (!dir) return log('This browser cannot store disc images; select them again next visit.', 'dim');
  try {
    const est = await navigator.storage.estimate();
    const old = have.disc[d] && have.disc[d].src === 'stored' ? have.disc[d].file.size : 0;
    if (est.quota - est.usage + old < file.size * 1.02)
      return log(`Not enough browser storage to remember disc ${d} (needs ${(file.size / 1048576).toFixed(0)} MB).`, 'bad');
    if (navigator.storage.persist) await navigator.storage.persist();
    const h = await dir.getFileHandle(`disc${d}.bin`, { create: true });
    if (!h.createWritable) return log('This browser cannot write to its file storage; discs will not be remembered.', 'dim');
    localStorage.removeItem(discMark(d));
    storing++; renderFiles();
    const w = await h.createWritable(), rd = file.stream().getReader();
    let done = 0, last = 0;
    for (;;) {
      const { value, done: end } = await rd.read();
      if (end) break;
      await w.write(value);
      done += value.length;
      if (performance.now() - last > 250) {
        last = performance.now();
        const pct = done * 100 / file.size;
        $('storeprog').textContent = `Remembering disc ${d}: ${Math.floor(pct)}%`;
        loader.show('store', `Remembering disc ${d} in this browser`, 'Only once: next time the discs load by themselves.');
        loader.set('store', pct, pct);
      }
    }
    await w.close();
    loader.set('store', 100); loader.hide('store');
    localStorage.setItem(discMark(d), String(file.size));
    log(`Disc ${d} remembered in this browser; next time it loads automatically.`, 'ok');
  } catch (e) {
    log(`Could not remember disc ${d}: ${e.message || e}`, 'bad');
  } finally {
    loader.hide('store');
    storing = Math.max(0, storing - 1);
    $('storeprog').textContent = storing ? $('storeprog').textContent : '';
    renderFiles();
  }
}

async function loadRemembered() {
  const dir = await opfs();
  for (const d of [1, 2]) {
    if (dir && localStorage.getItem(discMark(d))) {
      try {
        const f = await (await dir.getFileHandle(`disc${d}.bin`)).getFile();
        if (String(f.size) === localStorage.getItem(discMark(d))) have.disc[d] = { file: f, src: 'stored' };
      } catch { /* gone */ }
    }
    // Project NEMO's patches ship with the site (used with their permission).
    const r = await fetch(`patches/CD${d}.xdelta`, { method: 'HEAD' }).catch(() => null);
    if (r && r.ok) have.patch[d] = { url: `patches/CD${d}.xdelta`, src: 'bundled' };
    await idbDel('patches', d);                        // copies selected by older versions of the page
  }
}

async function forgetAll() {
  const dir = await opfs();
  for (const d of [1, 2]) {
    localStorage.removeItem(discMark(d));
    if (dir) await dir.removeEntry(`disc${d}.bin`).catch(() => {});
    await idbDel('patches', d); await idbDel('encache', d);
    for (const k of ['disc', 'patch']) if (have[k][d] && have[k][d].src === 'stored') delete have[k][d];
  }
  log('Remembered disc images and English data removed from this browser (saves kept).', 'ok');
  await loadRemembered();
  renderFiles();
}

const SRC = { selected: 'selected', stored: 'remembered in this browser', bundled: 'included with the site' };
function renderFiles() {
  const ul = $('files'); ul.replaceChildren();
  const add = (t, cls) => { const li = document.createElement('li'); li.textContent = t; li.className = cls; ul.appendChild(li); };
  for (const d of [1, 2]) {
    const x = have.disc[d];
    add(x ? `Disc ${d}: ${SRC[x.src]} (${(x.file.size / 1048576).toFixed(0)} MB)` : `Disc ${d}: not selected`, x ? 'ok' : 'dim');
  }
  $('start').disabled = !have.disc[1] || running;
  // always English (Project NEMO's patches ship with the site); a copy of the
  // site without them can only run the original Japanese text
  $('enhint').textContent = !have.patch[1] ? 'This copy of the site is missing patches/CD1.xdelta, so the game will be in Japanese.'
    : !have.patch[2] ? 'This copy of the site is missing patches/CD2.xdelta, so disc 2 will be in Japanese.' : '';
  // checklist and readouts
  $('step-discs').classList.toggle('done', !!have.disc[1]);
  $('step-settings').classList.toggle('done', !!have.disc[1]);
  $('ro-lang').textContent = have.patch[1] ? 'English' : 'Japanese';
  const touchOnly = matchMedia('(pointer: coarse)').matches && !matchMedia('(any-pointer: fine)').matches;
  $('idle-hint').textContent = (have.disc[1] ? '' : 'Add your game discs in the pre-flight check to play.') +
    (touchOnly ? ' You will need a keyboard or a game controller: touch controls are not available yet.' : '');
}
$('pick').onchange = e => classify(e.target.files);
$('forget').onclick = () => { if (!running) forgetAll(); };
const drop = $('drop');
drop.ondragover = e => { e.preventDefault(); drop.classList.add('over'); };
drop.ondragleave = () => drop.classList.remove('over');
drop.ondrop = e => { e.preventDefault(); drop.classList.remove('over'); classify(e.dataTransfer.files); };

// ------------------------------------------------------------------- saves
const DB = 'ac3online', STORE = 'cards';   // storage name from the project's first name; kept so saves and English data survive
function db() {
  return new Promise((res, rej) => {
    const r = indexedDB.open(DB, 3);
    r.onupgradeneeded = () => {
      for (const name of [STORE, 'encache', 'patches'])
        if (!r.result.objectStoreNames.contains(name)) r.result.createObjectStore(name);
    };
    r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
  });
}
async function cardGet(slot) {
  const d = await db();
  return new Promise(res => { const r = d.transaction(STORE).objectStore(STORE).get(slot); r.onsuccess = () => res(r.result || null); r.onerror = () => res(null); });
}
async function cardPut(slot, data, time = Date.now()) {
  const d = await db();
  return new Promise(res => { const t = d.transaction(STORE, 'readwrite'); t.objectStore(STORE).put({ data, time }, slot); t.oncomplete = res; t.onerror = res; });
}
async function idbGet(store, key) {
  const d = await db();
  return new Promise(res => { const r = d.transaction(store).objectStore(store).get(key); r.onsuccess = () => res(r.result || null); r.onerror = () => res(null); });
}
async function idbPut(store, key, value) {
  const d = await db();
  return new Promise(res => { const t = d.transaction(store, 'readwrite'); t.objectStore(store).put(value, key); t.oncomplete = res; t.onerror = res; });
}
async function idbDel(store, key) {
  const d = await db();
  return new Promise(res => { const t = d.transaction(store, 'readwrite'); t.objectStore(store).delete(key); t.oncomplete = res; t.onerror = res; });
}
async function showSaveInfo() {
  const c = await cardGet(0), b = await cardGet(1);
  $('saveinfo').textContent = c ? `Last saved ${new Date(c.time).toLocaleString()}.` : 'No save in this browser yet. The game creates one the first time it saves.';
  $('restorecard').hidden = !b;
}
showSaveInfo();
renderFiles();
loadRemembered().then(renderFiles);
$('export').onclick = async () => {
  const c = await cardGet(0);
  if (!c) { $('saveinfo').textContent = 'There is no save to download yet.'; return; }
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([c.data])); a.download = 'card1.mcd'; a.click();
};
$('importbtn').onclick = () => { if (!running) $('import').click(); };
$('restorecard').onclick = async () => {
  const b = await cardGet(1); if (!b || running) return;
  if (!confirm(`Bring back the save from ${new Date(b.time).toLocaleString()}? The current save becomes the backup.`)) return;
  const c = await cardGet(0);
  await cardPut(0, b.data, b.time);
  if (c) await cardPut(1, c.data, c.time);
  showSaveInfo();
  log('Previous memory card restored. It is used the next time you press Play.', 'ok');
};
$('import').onchange = async e => {
  const f = e.target.files[0]; if (!f) return;
  let data = new Uint8Array(await f.arrayBuffer());
  // DexDrive .gme: a 3904-byte header ("123-456-STD") followed by the raw card
  const sig = String.fromCharCode(...data.subarray(0, 11));
  if (data.length === 3904 + 131072 && sig === '123-456-STD') data = data.slice(3904);
  if (data.length !== 131072 || data[0] !== 0x4D || data[1] !== 0x43)   // "MC"
    return log(`${f.name}: not a memory card image (.mcd/.mcr 128 KB raw, or DexDrive .gme)`, 'bad');
  const old = await cardGet(0);
  if (old) {
    if (!confirm(`Replace your current save (last saved ${new Date(old.time).toLocaleString()}) with ${f.name}?\n\n` +
                 'Your current save is kept as a backup: "Restore previous card" brings it back.')) return;
    await cardPut(1, old.data, old.time);                // backup slot (not used by the game)
  }
  await cardPut(0, data); showSaveInfo();
  log(`Loaded ${f.name} as memory card 1. It is used the next time the game starts.`, 'ok');
  $('saveinfo').textContent = `Loaded ${f.name}. It is used the next time you press Play.`;
};

// ----------------------------------------------------------------- graphics
// "Original" = the PS1 picture, converted in the worker and drawn here.
// "Enhanced" = renderer/hw at 2-6x the PS1 resolution: the game worker records
// the drawing, render-worker.js draws it with WebGPU straight into the
// #screen-hd canvas (handed over with transferControlToOffscreen).
const gfxPref = { get() { try { return JSON.parse(localStorage.getItem('ac3.gfx')) || {}; } catch { return {}; } },
                  set(v) { try { localStorage.setItem('ac3.gfx', JSON.stringify(v)); } catch {} } };
// Widescreen and the world effects are one bit each (same bits as
// platform/web/web_platform.c, hd_apply_settings). They change live: the page
// writes them into a small shared buffer that the game reads every field.
const GFX_DEFAULT = 1 | 2 | 4 | 16 | 32 | 64 | 128 | 256 | 512;   // everything but the animated ocean
const GFX_VERSION = 2;          // 2: bit 512 (original fog) added, on for remembered settings too
const bitBoxes = [...document.querySelectorAll('input[data-bit]')];
let hdSet = null;                                           // Int32Array: [0] bits, [1] version
function gfxBits() { let b = 0; for (const x of bitBoxes) if (x.checked) b |= +x.dataset.bit; return b; }
function showGfxBits(b) { for (const x of bitBoxes) x.checked = !!(b & +x.dataset.bit); }
function gfxEnable() {
  const sd = !self.navigator.gpu || $('gfx').value === '0', fx = !!(gfxBits() & 2), atmo = !!(gfxBits() & 64);
  for (const x of bitBoxes) {
    const bit = +x.dataset.bit;
    x.disabled = sd || (bit > 2 && !fx) || (bit === 4 && !atmo);
  }
  document.body.classList.toggle('sd', sd);
}
function applyLive() {
  const b = gfxBits();
  if (hdSet) { Atomics.store(hdSet, 0, b); Atomics.add(hdSet, 1, 1); }
  if (document.body.classList.contains('hd')) {
    $('screen-wrap').classList.toggle('wide', !!(b & 1));
    $('ro-gfx').textContent = `enhanced ${$('ro-gfx').dataset.scale}×${b & 1 ? ', 16:9' : ''}`;
  }
}
{
  const g = gfxPref.get();
  $('gfx').value = self.navigator.gpu ? String(g.scale ?? 0) : '0';
  showGfxBits(g.bits !== undefined ? (g.v >= GFX_VERSION ? g.bits : g.bits | 512)
                                    : (GFX_DEFAULT & ~(g.wide === false ? 1 : 0) | (g.ocean ? 8 : 0)));
  if (!self.navigator.gpu) {
    for (const o of $('gfx').options) if (o.value !== '0') o.disabled = true;
    $('gfxhint').textContent = 'Enhanced graphics need WebGPU (current Chrome or Edge, Safari 26, Firefox 141 on Windows).';
  }
  const save = () => {
    gfxPref.set({ scale: +$('gfx').value, bits: gfxBits(), v: GFX_VERSION });
    gfxEnable();
  };
  $('gfx').onchange = () => { save(); if (running) $('gfxhint').textContent = 'The new resolution applies the next time you press Play.'; };
  for (const x of bitBoxes) x.onchange = () => { save(); applyLive(); };
  $('gfx-defaults').onclick = () => { showGfxBits(GFX_DEFAULT); save(); applyLive(); };
  gfxEnable();
}

// ------------------------------------------------------------------ display
const canvas = $('screen'), ctx2d = canvas.getContext('2d');
let renderWorker = null, hdSize = '';
const frameCanvas = new OffscreenCanvas(320, 240), frameCtx = frameCanvas.getContext('2d');
let latest = null, frames = 0, lastFpsT = performance.now();
function draw() {
  if (renderWorker) {                        // the enhanced picture is drawn at the box's real pixel size
    const r = $('screen-wrap').getBoundingClientRect();
    const w = Math.max(320, Math.round(r.width * devicePixelRatio)), h = Math.max(240, Math.round(r.height * devicePixelRatio));
    if (hdSize !== w + 'x' + h) { hdSize = w + 'x' + h; renderWorker.postMessage({ type: 'size', w, h }); }
  }
  if (latest && latest.buf) {
    const { w, h, buf } = latest; latest = null;
    const rect = canvas.getBoundingClientRect();
    const cw = Math.max(320, Math.round(rect.width * devicePixelRatio)), ch = Math.round(cw * 3 / 4);
    if (canvas.width !== cw || canvas.height !== ch) { canvas.width = cw; canvas.height = ch; }
    if (w && h) {
      if (frameCanvas.width !== w || frameCanvas.height !== h) { frameCanvas.width = w; frameCanvas.height = h; }
      frameCtx.putImageData(new ImageData(new Uint8ClampedArray(buf), w, h), 0, 0);
      ctx2d.imageSmoothingEnabled = $('smooth').checked;
      ctx2d.drawImage(frameCanvas, 0, 0, w, h, 0, 0, cw, ch);   // PS1 output is always 4:3
    } else {
      ctx2d.fillStyle = '#000'; ctx2d.fillRect(0, 0, cw, ch);
    }
  }
  pollInput();
  requestAnimationFrame(draw);
}
requestAnimationFrame(draw);
$('smooth').onchange = () => canvas.classList.toggle('smooth', $('smooth').checked);
$('fullscreen').onclick = () => $('screen-wrap').requestFullscreen();

// -------------------------------------------------------------------- input
const BTN = { SELECT: 1, L3: 2, R3: 4, START: 8, UP: 16, RIGHT: 32, DOWN: 64, LEFT: 128,
  L2: 256, R2: 512, L1: 1024, R1: 2048, TRIANGLE: 4096, CIRCLE: 8192, CROSS: 16384, SQUARE: 32768 };
// Keyboard bindings: one key per action, changeable on the page and kept in
// localStorage ('ac3.keys'). Actions are pad buttons, or the left stick's
// four directions (the game's flight stick).
const KEY_ACTIONS = [
  ['Flight stick', [['STICK_UP', 'Up', 'KeyW'], ['STICK_DOWN', 'Down', 'KeyS'], ['STICK_LEFT', 'Left', 'KeyA'], ['STICK_RIGHT', 'Right', 'KeyD']]],
  ['Directional pad', [['UP', 'Up', 'ArrowUp'], ['DOWN', 'Down', 'ArrowDown'], ['LEFT', 'Left', 'ArrowLeft'], ['RIGHT', 'Right', 'ArrowRight']]],
  ['Buttons', [['CROSS', 'Cross', 'KeyX'], ['CIRCLE', 'Circle', 'KeyC'], ['SQUARE', 'Square', 'KeyZ'], ['TRIANGLE', 'Triangle', 'KeyV']]],
  ['Shoulders', [['L1', 'L1', 'KeyQ'], ['R1', 'R1', 'KeyE'], ['L2', 'L2', 'Digit1'], ['R2', 'R2', 'Digit3']]],
  ['System', [['START', 'Start', 'Enter'], ['SELECT', 'Select', 'Backspace']]],
];
const KEY_DEFAULTS = Object.fromEntries(KEY_ACTIONS.flatMap(([, a]) => a.map(([id, , code]) => [id, code])));
let keyBind = { ...KEY_DEFAULTS };                       // action -> code
try {                                                    // only known actions, string codes
  const saved = JSON.parse(localStorage.getItem('ac3.keys')) || {};
  for (const id of Object.keys(KEY_DEFAULTS))
    if (Object.prototype.hasOwnProperty.call(saved, id) && typeof saved[id] === 'string' && saved[id].length < 40) keyBind[id] = saved[id];
} catch {}
let KEYS = Object.create(null);                          // code -> action
function rebuildKeys() { KEYS = Object.create(null); for (const [a, c] of Object.entries(keyBind)) if (c) KEYS[c] = a; }
rebuildKeys();
function keyName(code) {
  if (!code) return '—';
  const m = code.match(/^Key(.)$/) || code.match(/^Digit(.)$/);
  if (m) return m[1];
  const arrows = { ArrowUp: '↑', ArrowDown: '↓', ArrowLeft: '←', ArrowRight: '→' };
  if (arrows[code]) return arrows[code];
  if (code.startsWith('Numpad')) return 'Num ' + code.slice(6);
  const side = code.match(/^(Shift|Control|Alt|Meta)(Left|Right)$/);
  if (side) return `${side[2]} ${side[1] === 'Control' ? 'Ctrl' : side[1]}`;
  return { Space: 'Space', Backquote: '`', Minus: '-', Equal: '=', BracketLeft: '[', BracketRight: ']', Backslash: '\\',
           Semicolon: ';', Quote: "'", Comma: ',', Period: '.', Slash: '/' }[code] || code;
}
let listening = null;                                    // the action waiting for a new key
function renderKeys(flash) {
  const g = $('keygrid'); g.replaceChildren();
  for (const [title, acts] of KEY_ACTIONS) {
    const box = document.createElement('div'); box.className = 'keygroup';
    const h3 = document.createElement('h3'); h3.textContent = title; box.appendChild(h3);
    for (const [id, label] of acts) {
      const row = document.createElement('div'); row.className = 'keyrow';
      const b = document.createElement('button'); b.className = 'keycap' + (listening === id ? ' listening' : '') + (flash && flash.includes(id) ? ' flash' : '');
      b.textContent = listening === id ? 'press a key…' : keyName(keyBind[id]);
      b.setAttribute('aria-label', `${title} ${label}: ${keyName(keyBind[id])}. Click to change.`);
      b.onclick = () => { listening = listening === id ? null : id; renderKeys(); };
      const sp = document.createElement('span'); sp.textContent = label;
      row.append(sp, b); box.appendChild(row);
    }
    g.appendChild(box);
  }
}
function setKey(id, code) {
  const other = Object.keys(keyBind).find(a => a !== id && keyBind[a] === code);
  if (other) keyBind[other] = keyBind[id];                 // the key was in use: swap
  keyBind[id] = code;
  try { localStorage.setItem('ac3.keys', JSON.stringify(keyBind)); } catch {}
  rebuildKeys(); listening = null; renderKeys(other ? [id, other] : [id]);
}
$('keys-reset').onclick = () => {
  keyBind = { ...KEY_DEFAULTS };
  try { localStorage.removeItem('ac3.keys'); } catch {}
  rebuildKeys(); listening = null; renderKeys(Object.keys(KEY_DEFAULTS));
};
renderKeys();
const held = new Set();
addEventListener('keydown', e => {
  if (listening) {                                       // choosing a key: it does not reach the game
    e.preventDefault(); e.stopPropagation();
    if (e.code === 'Escape') { listening = null; renderKeys(); } else setKey(listening, e.code);
    return;
  }
  if (running && KEYS[e.code]) e.preventDefault();
  held.add(e.code);
}, true);
addEventListener('keyup', e => held.delete(e.code));
addEventListener('blur', () => held.clear());
let padView = null, padName = 'keyboard', shownPad = '';
const axis = v => Math.max(0, Math.min(255, Math.round((v + 1) * 127.5)));
const dz = v => Math.abs(v) < 0.1 ? 0 : v;
function pollInput() {
  if (!padView) return;
  let b = 0, lx = 128, ly = 128, rx = 128, ry = 128, analog = 0;
  for (const k of held) {
    const a = KEYS[k];
    if (!a) continue;
    if (a === 'STICK_LEFT') lx = 0; else if (a === 'STICK_RIGHT') lx = 255;
    else if (a === 'STICK_UP') ly = 0; else if (a === 'STICK_DOWN') ly = 255;
    else b |= BTN[a];
  }
  for (const gp of navigator.getGamepads ? navigator.getGamepads() : []) {
    if (!gp || gp.mapping !== 'standard') continue;
    analog = 1;
    const p = i => gp.buttons[i] && gp.buttons[i].pressed;
    const map = [[0, 'CROSS'], [1, 'CIRCLE'], [2, 'SQUARE'], [3, 'TRIANGLE'], [4, 'L1'], [5, 'R1'], [6, 'L2'], [7, 'R2'],
      [8, 'SELECT'], [9, 'START'], [10, 'L3'], [11, 'R3'], [12, 'UP'], [13, 'DOWN'], [14, 'LEFT'], [15, 'RIGHT']];
    for (const [i, n] of map) if (p(i)) b |= BTN[n];
    lx = axis(dz(gp.axes[0])); ly = axis(dz(gp.axes[1])); rx = axis(dz(gp.axes[2])); ry = axis(dz(gp.axes[3]));
    padName = gp.id.replace(/\s*\(.*$/, '').slice(0, 28) || 'gamepad';
    break;
  }
  if (!analog) padName = 'keyboard';
  if (padName !== shownPad) { shownPad = padName; $('ro-pad').textContent = padName; }
  padView[0] = 1; padView[1] = analog; padView[2] = b;
  padView[3] = lx; padView[4] = ly; padView[5] = rx; padView[6] = ry;
}


// ------------------------------------------------------------- loading screen
// A fighter seen from above that fills up from tail to nose (#loader in index.html). Each
// start-up stage moves the bar to its own range; within a stage it creeps
// forward, and the English build (first start only) reports real progress.
const loader = (() => {
  const el = $('loader'), fill = $('ld-fill'), edge = $('ld-edge'), lit = $('ld-litrect');
  const NOSE = 6, TAIL = 284;                               // the jet's extent in the SVG (y, nose up)
  const still = matchMedia('(prefers-reduced-motion: reduce)');
  let v = 0, target = 0, ceil = 0, raf = 0, owner = null;
  function draw() {
    const y = TAIL - (TAIL - NOSE) * v / 100;
    fill.setAttribute('y', y.toFixed(2)); fill.setAttribute('height', (TAIL - y + 1).toFixed(2));
    edge.setAttribute('y', (y - 0.8).toFixed(2));
    edge.style.opacity = v > 0.5 && v < 99.5 ? 1 : 0;
    lit.setAttribute('y', y.toFixed(2)); lit.setAttribute('height', (330 - y).toFixed(2));
    $('ld-pct').textContent = Math.floor(v);
    el.setAttribute('aria-valuenow', Math.floor(v));
  }
  function tick() {
    raf = 0;
    if (el.hidden) return;
    if (target < ceil) target = Math.min(ceil, target + (ceil - target) * 0.006 + 0.004);   // creep
    v = still.matches ? target : v + (target - v) * 0.12;
    if (Math.abs(target - v) < 0.05) v = target;
    draw();
    raf = requestAnimationFrame(tick);
  }
  return {
    // who: 'game' or 'store' (remembering discs); the game wins
    show(who, stage, sub = '') {
      if (owner === 'game' && who !== 'game') return;
      if (owner !== who) { v = target = ceil = 0; draw(); }
      owner = who;
      el.hidden = false; el.classList.remove('gone');
      $('ld-stage').textContent = stage; $('ld-sub').textContent = sub;
      if (!raf) raf = requestAnimationFrame(tick);
    },
    // jump to at least `from` %, then creep towards `to` %
    set(who, from, to = from) {
      if (who !== owner) return;
      target = Math.max(target, from); ceil = Math.max(target, to);
    },
    hide(who) {
      if (who !== owner) return;
      owner = null;
      target = ceil = 100;
      setTimeout(() => {
        if (owner) return;
        el.classList.add('gone');
        setTimeout(() => { if (!owner) { el.hidden = true; v = target = ceil = 0; draw(); } }, still.matches ? 0 : 450);
      }, still.matches ? 0 : 250);
    },
    cancel() { owner = null; el.hidden = true; el.classList.remove('gone'); v = target = ceil = 0; draw(); },
  };
})();
// start-up stages (web/worker.js posts 'progress'): label and range in %
const STAGES = {
  prep:    ['Loading your files', 0, 8],
  check:   ['Checking the game disc', 8, 12],
  engine:  ['Starting the game engine', 12, 22],
  boot:    ['Booting', 22, 92],
};
let buildingEnglish = [];                     // discs whose English data must be built this start

function loaderStage(stage, disc, done, total) {
  if (stage === 'english') {
    // first start only: the English text is built from the discs and the NEMO patch
    const n = Math.max(1, buildingEnglish.length), i = Math.max(0, buildingEnglish.indexOf(disc));
    const lo = 24 + 68 * i / n, hi = 24 + 68 * (i + 1) / n, f = total ? done / total : 0;
    loader.show('game', `Building the English version, disc ${disc}`, 'First start only: later starts skip this.');
    loader.set('game', lo + (hi - lo) * f, lo + (hi - lo) * Math.min(1, f + 0.02));
    return;
  }
  const s = STAGES[stage]; if (!s) return;
  loader.show('game', s[0]);
  loader.set('game', s[1], stage === 'boot' && buildingEnglish.length ? 24 : s[2]);
}

// --------------------------------------------------------------- run / stop
$('volume').oninput = () => audioNode && audioNode.port.postMessage({ volume: $('volume').value / 100 });

$('start').onclick = async () => {
  if (!self.crossOriginIsolated) { $('status').textContent = 'This page cannot run the game: it is not cross-origin isolated. See Diagnostics.'; return; }
  running = true; renderFiles(); $('stop').disabled = false;
  document.body.classList.add('live');
  loaderStage('prep');
  $('status').textContent = 'Starting…';

  ctrl = new Int32Array(new SharedArrayBuffer(16));     // [0] quit [1] audio running [2] sleep cell
  const hd = await startRenderer();                       // null: original picture
  const padSab = new SharedArrayBuffer(7 * 4); padView = new Int32Array(padSab);
  const ringFrames = 44100;                                // 1 s of headroom
  const ring = new SharedArrayBuffer(8 + ringFrames * 4);

  try {                                                    // audio must start from a click
    audioCtx = new AudioContext({ sampleRate: 44100, latencyHint: 'interactive' });
    await audioCtx.audioWorklet.addModule('audio-worklet.js');
    audioNode = new AudioWorkletNode(audioCtx, 'ac3-audio', { outputChannelCount: [2], processorOptions: { ring } });
    audioNode.connect(audioCtx.destination);
    audioNode.port.postMessage({ volume: $('volume').value / 100 });
    await audioCtx.resume();
    Atomics.store(ctrl, 1, 1);
  } catch (e) { log('Audio unavailable: ' + e, 'bad'); }

  const english = !!have.patch[1];
  buildingEnglish = [];
  const discs = [1, 2].filter(d => have.disc[d]).map(d => have.disc[d].file);
  const patches = [];
  if (english) for (const d of [1, 2]) {
    const x = have.patch[d]; if (!x) continue;
    try { patches.push({ disc: d, file: x.file || await (await fetch(x.url)).blob() }); }
    catch (e) { log(`could not load the bundled NEMO patch CD${d}: ${e}`, 'bad'); }
  }
  const caches = [];
  if (english) for (const n of [1, 2]) {
    const c = await idbGet('encache', n);
    if (c) { caches.push({ disc: n, file: new Blob([c.data]) }); log(`using English data for disc ${n} stored in this browser`); }
    else { log(`English data for disc ${n} will be built now (first time only, this can take a minute)…`); if (have.disc[n]) buildingEnglish.push(n); }
  }
  const stored = await cardGet(0);
  const cards = stored ? [{ slot: 0, data: stored.data.buffer.slice(0) }] : [];

  worker = new Worker('worker.js');
  worker.onmessage = async e => {
    const m = e.data;
    if (m.type === 'frame') { if (m.buf) latest = m; frames++; if (m.seq === 1 || frames === 1) loader.hide('game'); const t = performance.now(); if (t - lastFpsT > 1000) { $('fps').textContent = `${Math.round(frames * 1000 / (t - lastFpsT))} fps`; frames = 0; lastFpsT = t; } }
    else if (m.type === 'log') log(m.text);
    else if (m.type === 'progress') loaderStage(m.stage, m.disc, m.done, m.total);
    else if (m.type === 'hd-off') hdOff(m.text);
    else if (m.type === 'error') { log(m.text, 'bad'); $('status').textContent = m.text.split('\n')[0]; stop(); }
    else if (m.type === 'card') { if (m.slot === 0) { await cardPut(0, new Uint8Array(m.data)); showSaveInfo(); log('memory card 1 saved to browser storage', 'ok'); } }
    else if (m.type === 'encache') { await idbPut('encache', m.disc, { data: new Uint8Array(m.data), time: Date.now() }); log(`English data for disc ${m.disc} saved in this browser (${(m.data.byteLength / 1048576).toFixed(1)} MB); later starts skip the build.`, 'ok'); }
    else if (m.type === 'exit') { log(`program exited (${m.code})`); $('status').textContent = 'Stopped.'; stop(); }
  };
  worker.onerror = e => { log('worker error: ' + e.message, 'bad'); stop(); };
  worker.postMessage({ type: 'start', ctrl: ctrl.buffer, pad: padSab, ring,
    discs, patches, caches, english, cards, hd }, hd ? [hd.port] : []);
  $('status').textContent = 'Running. Click the screen area, then use the keyboard or a controller.';
};

// Enhanced graphics: start the render worker before the game, so that a
// missing or failing WebGPU falls back to the original picture up front.
async function startRenderer() {
  const want = self.navigator.gpu ? +$('gfx').value : 0, wide = want > 0 && !!(gfxBits() & 1);
  $('gfxhint').textContent = '';
  if (!want) { hdOff(null); return null; }
  // a canvas can be handed to a worker only once: use a fresh one every time
  const old = $('screen-hd'), cv = document.createElement('canvas');
  cv.id = 'screen-hd'; cv.setAttribute('aria-label', 'Game screen (enhanced)'); old.replaceWith(cv);
  document.body.classList.add('hd'); $('screen-wrap').classList.toggle('wide', wide);
  const r = $('screen-wrap').getBoundingClientRect();
  cv.width = Math.max(320, Math.round(r.width * devicePixelRatio)); cv.height = Math.max(240, Math.round(r.height * devicePixelRatio));
  hdSize = cv.width + 'x' + cv.height;
  const off = cv.transferControlToOffscreen(), chan = new MessageChannel(), done = new SharedArrayBuffer(4);
  const rw = new Worker('render-worker.js');
  const result = await new Promise(res => {
    const t = setTimeout(() => res({ type: 'failed', text: 'the renderer did not start within 15 s' }), 15000);
    rw.onmessage = e => {
      const m = e.data;
      if (m.type === 'ready' || m.type === 'failed') { clearTimeout(t); res(m); }
      else if (m.type === 'log') log(m.text);
    };
    rw.onerror = e => { clearTimeout(t); res({ type: 'failed', text: e.message }); };
    rw.postMessage({ type: 'init', canvas: off, port: chan.port2, done, scale: want }, [off, chan.port2]);
  });
  if (result.type !== 'ready') { rw.terminate(); hdOff(`Enhanced graphics unavailable (${result.text}); showing the original picture.`); return null; }
  renderWorker = rw;
  rw.onmessage = e => {
    const m = e.data;
    if (m.type === 'log') log(m.text);
    else if (m.type === 'lost') { log(m.text, 'bad'); $('status').textContent = m.text + ' Press Stop, then Play to continue from your last save.'; }
    else if (DEV && m.type === 'shot' && self.onShot) self.onShot(m);   // used by tests
  };
  $('ro-gfx').dataset.scale = result.scale;
  $('ro-gfx').textContent = `enhanced ${result.scale}×${wide ? ', 16:9' : ''}`;
  hdSet = new Int32Array(new SharedArrayBuffer(8));
  hdSet[0] = gfxBits(); hdSet[1] = 1;
  if (result.scale !== want) log(`enhanced graphics: ${want}× is too large for this GPU, using ${result.scale}×`);
  return { scale: result.scale, wide, settings: hdSet.buffer, port: chan.port1, done, skip: DEV && self.hdSkip || 0 };
}
function hdOff(text) {
  document.body.classList.remove('hd'); $('screen-wrap').classList.remove('wide');
  $('ro-gfx').textContent = 'original';
  if (text) { $('gfxhint').textContent = text; log(text, 'bad'); }
  if (renderWorker) { renderWorker.terminate(); renderWorker = null; }
  hdSet = null;
}

function stop() {
  if (renderWorker) { const r = renderWorker; renderWorker = null; setTimeout(() => r.terminate(), 1600); }
  if (worker) {
    Atomics.store(ctrl, 0, 1);                             // ask the game to quit cleanly…
    const w = worker; setTimeout(() => w.terminate(), 1500); // …then make sure
    worker = null;
  }
  if (audioCtx) { audioCtx.close(); audioCtx = null; audioNode = null; }
  padView = null; running = false; $('stop').disabled = true; loader.cancel();
  document.body.classList.remove('live', 'hd'); $('screen-wrap').classList.remove('wide');
  $('fps').textContent = '–'; $('ro-gfx').textContent = '–'; renderFiles();
}
$('stop').onclick = () => { stop(); $('status').textContent = 'Stopped.'; };
