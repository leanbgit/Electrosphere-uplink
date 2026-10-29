// coop.js -- script of the co-op test page (coop.html). A separate file, so the
// page works under the site's Content-Security-Policy (no inline scripts).
'use strict';
const $ = id => document.getElementById(id);
const log = (t, cls) => { const l = $('log'); const d = document.createElement('div'); d.textContent = t; if (cls) d.className = cls; l.appendChild(d); l.scrollTop = l.scrollHeight; };
const status = (t, cls = 'dim') => { $('status').textContent = t; $('status').className = cls; };

// ---------------------------------------------------------------- files
// Same storage as web/app.js: discs in the origin-private file system,
// English data and the memory card in IndexedDB ("ac3online").
const have = { discs: [], patches: [], caches: [], card: null };
function db() {
  return new Promise((res, rej) => {
    const r = indexedDB.open('ac3online', 3);
    r.onupgradeneeded = () => { for (const n of ['cards', 'encache', 'patches']) if (!r.result.objectStoreNames.contains(n)) r.result.createObjectStore(n); };
    r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
  });
}
async function idbGet(store, key) {
  const d = await db();
  return new Promise(res => { const r = d.transaction(store).objectStore(store).get(key); r.onsuccess = () => res(r.result || null); r.onerror = () => res(null); });
}
async function loadRemembered() {
  try {
    const dir = await navigator.storage.getDirectory();
    for (const d of [1, 2]) {
      const mark = localStorage.getItem(`ac3.disc${d}`);
      if (!mark) continue;
      const f = await (await dir.getFileHandle(`disc${d}.bin`)).getFile();
      if (String(f.size) === mark) have.discs[d] = f;
    }
  } catch { /* none */ }
  for (const d of [1, 2]) {
    const r = await fetch(`patches/CD${d}.xdelta`).catch(() => null);
    if (r && r.ok) have.patches.push({ disc: d, file: await r.blob() });
    const c = await idbGet('encache', d);
    if (c) have.caches.push({ disc: d, file: new Blob([c.data]) });
  }
  const card = await idbGet('cards', 0);
  if (card && card.data) have.card = card.data;
  const n = [1, 2].filter(d => have.discs[d]).length;
  if (!have.discs[1]) return status('No remembered discs: play once on the main page from this same address, then come back.', 'bad');
  status((n === 2 ? 'Both discs found.' : 'Disc 1 found (disc 2 missing).') + (have.card ? ' Using your memory card (read only).' : ' No save found: starting with an empty card.'), 'ok');
  $('start').disabled = false;
}

// ---------------------------------------------------------------- input
const BTN = { SELECT: 1, L3: 2, R3: 4, START: 8, UP: 16, RIGHT: 32, DOWN: 64, LEFT: 128,
  L2: 256, R2: 512, L1: 1024, R1: 2048, TRIANGLE: 4096, CIRCLE: 8192, CROSS: 16384, SQUARE: 32768 };
// The main page's default keys (web/app.js KEY_ACTIONS).
const KEYS = { KeyW: 'STICK_UP', KeyS: 'STICK_DOWN', KeyA: 'STICK_LEFT', KeyD: 'STICK_RIGHT',
  ArrowUp: 'UP', ArrowDown: 'DOWN', ArrowLeft: 'LEFT', ArrowRight: 'RIGHT',
  KeyX: 'CROSS', KeyC: 'CIRCLE', KeyZ: 'SQUARE', KeyV: 'TRIANGLE',
  KeyQ: 'L1', KeyE: 'R1', Digit1: 'L2', Digit3: 'R2', Enter: 'START', Backspace: 'SELECT' };
$('keytable').innerHTML = [['Flight stick', 'W A S D'], ['D-pad', 'arrow keys'], ['Cross / Circle / Square / Triangle', 'X / C / Z / V'],
  ['L1 / R1 / L2 / R2', 'Q / E / 1 / 3'], ['Start / Select', 'Enter / Backspace']].map(([a, k]) => `<tr><td>${a}</td><td>${k}</td></tr>`).join('');
const held = new Set();
addEventListener('keydown', e => { if (running && KEYS[e.code]) e.preventDefault(); held.add(e.code); }, true);
addEventListener('keyup', e => held.delete(e.code));
addEventListener('blur', () => held.clear());
const axis = v => Math.max(0, Math.min(255, Math.round((v + 1) * 127.5)));
const dz = v => Math.abs(v) < 0.1 ? 0 : v;
function gamepads() { return [...(navigator.getGamepads ? navigator.getGamepads() : [])].filter(g => g && g.mapping === 'standard'); }
function fillSources() {
  const pads = gamepads();
  for (const [id, def] of [['src1', 'pad0'], ['src2', 'keys']]) {
    const sel = $(id), keep = sel.value || def;
    const opts = [['keys', 'Keyboard'], ...pads.map((g, i) => ['pad' + i, `Gamepad ${i + 1}: ${g.id.replace(/\s*\(.*$/, '').slice(0, 30)}`]), ['none', 'Nothing']];
    if (!pads.length) opts.splice(1, 0, ['pad0', 'Gamepad 1 (press a button)']);
    sel.replaceChildren(...opts.map(([v, t]) => { const o = document.createElement('option'); o.value = v; o.textContent = t; return o; }));
    sel.value = opts.some(o => o[0] === keep) ? keep : def;
  }
}
addEventListener('gamepadconnected', fillSources); addEventListener('gamepaddisconnected', fillSources);
fillSources();
function readSource(src, view) {
  let b = 0, lx = 128, ly = 128, rx = 128, ry = 128, analog = 0;
  if (src === 'keys') {
    for (const k of held) {
      const a = KEYS[k]; if (!a) continue;
      if (a === 'STICK_LEFT') lx = 0; else if (a === 'STICK_RIGHT') lx = 255;
      else if (a === 'STICK_UP') ly = 0; else if (a === 'STICK_DOWN') ly = 255;
      else b |= BTN[a];
    }
  } else if (src.startsWith('pad')) {
    const gp = gamepads()[+src.slice(3)];
    if (gp) {
      analog = 1;
      const p = i => gp.buttons[i] && gp.buttons[i].pressed;
      const map = [[0, 'CROSS'], [1, 'CIRCLE'], [2, 'SQUARE'], [3, 'TRIANGLE'], [4, 'L1'], [5, 'R1'], [6, 'L2'], [7, 'R2'],
        [8, 'SELECT'], [9, 'START'], [10, 'L3'], [11, 'R3'], [12, 'UP'], [13, 'DOWN'], [14, 'LEFT'], [15, 'RIGHT']];
      for (const [i, n] of map) if (p(i)) b |= BTN[n];
      lx = axis(dz(gp.axes[0])); ly = axis(dz(gp.axes[1])); rx = axis(dz(gp.axes[2])); ry = axis(dz(gp.axes[3]));
    }
  }
  view[0] = src === 'none' ? 0 : 1; view[1] = analog; view[2] = b;
  view[3] = lx; view[4] = ly; view[5] = rx; view[6] = ry;
}

// ---------------------------------------------------------------- pictures
function drawer(canvas) {
  const ctx = canvas.getContext('2d');
  return m => {
    if (!m.w) { ctx.fillStyle = '#000'; ctx.fillRect(0, 0, canvas.width, canvas.height); return; }
    if (canvas.width !== m.w || canvas.height !== m.h) { canvas.width = m.w; canvas.height = m.h; }
    ctx.putImageData(new ImageData(new Uint8ClampedArray(m.buf), m.w, m.h), 0, 0);
  };
}
const draw1 = drawer($('v1')), draw2 = drawer($('v2'));

// ---------------------------------------------------------------- run
let running = false, worker = null, ctrl = null, pad1 = null, pad2 = null, audioCtx = null;
let fields = 0, lastFields = 0, lastT = 0, guestSeen = false;
// what the right-hand view showed over the last second while the guest flew:
// its own picture, or the host's (the guest's view was not drawn)
let guestLive = false, ownViews = 0, hostViews = 0, reportedHostView = false;
function tick() {
  if (!running) return;
  readSource($('src1').value, pad1);
  readSource($('src2').value, pad2);
  const now = performance.now();
  if (now - lastT > 1000) {
    $('ro1').textContent = `${Math.round((fields - lastFields) * 1000 / (now - lastT))} fields/s (normal: 60)`;
    if (guestLive) {
      const bad = hostViews > 0 && hostViews >= ownViews;
      $('ro2').textContent = bad ? `NOT the guest's view: showing the host's picture (${hostViews} of ${ownViews + hostViews} pictures)` : `flying · own view`;
      $('ro2').className = bad ? 'bad' : 'dim';
      if (bad && !reportedHostView) { reportedHostView = true; log(`Field ~${fields}: the guest's own view is not being drawn; the right side shows the host's picture.`, 'bad'); }
    }
    ownViews = hostViews = 0; lastFields = fields; lastT = now;
  }
  requestAnimationFrame(tick);
}
$('start').onclick = async () => {
  $('start').disabled = true; $('stop').disabled = false; $('downloads').replaceChildren();
  ctrl = new Int32Array(new SharedArrayBuffer(16));
  pad1 = new Int32Array(new SharedArrayBuffer(28)); pad2 = new Int32Array(new SharedArrayBuffer(28));
  pad1.set([1, 0, 0, 128, 128, 128, 128]); pad2.set([1, 0, 0, 128, 128, 128, 128]);
  const ring = new SharedArrayBuffer(8 + 44100 * 4);
  try {
    audioCtx = new AudioContext({ sampleRate: 44100, latencyHint: 'interactive' });
    await audioCtx.audioWorklet.addModule('audio-worklet.js');
    const node = new AudioWorkletNode(audioCtx, 'ac3-audio', { outputChannelCount: [2], processorOptions: { ring } });
    node.connect(audioCtx.destination); await audioCtx.resume(); Atomics.store(ctrl, 1, 1);
  } catch (e) { log('Audio unavailable: ' + e, 'warn'); }
  const extraArgs = ['--coop-guest', '--coop-split'];
  if ($('record').checked) {
    const name = `coop_${new Date().toISOString().slice(0, 19).replace(/[-:T]/g, '')}.ac3i`;
    extraArgs.push('--record-input', '/rec/' + name);
    log('Recording to ' + name + ' (offered for download when you press Stop)');
  }
  fields = 0; guestSeen = false; guestLive = false; reportedHostView = false; lastT = performance.now();
  worker = new Worker('worker.js');
  worker.onmessage = e => {
    const m = e.data;
    if (m.type === 'frame') { fields++; draw1(m); }
    else if (m.type === 'frame2') {
      draw2(m); if (m.own) ownViews++; else if (guestLive) hostViews++;
      // no guest plane (not in the mission yet, or shot down): player 1's view, dimmed
      $('v2').classList.toggle('watching', !m.own);
    }
    else if (m.type === 'log') { log(m.text); if (/^coop:/.test(m.text)) {
      const t = m.text.replace(/^coop:\s*/, '');
      if (/created/.test(t)) { guestSeen = true; guestLive = true; reportedHostView = false; $('ro2').textContent = 'flying'; }
      else if (/destroyed/.test(t)) { guestLive = false; $('ro2').textContent = 'shot down: watching until the mission ends'; }
      else if (/mission over/.test(t)) { guestLive = false; $('ro2').textContent = 'waiting for the next mission'; }
    } }
    else if (m.type === 'error') log(m.text, 'bad');
    else if (m.type === 'progress') status(`Starting: ${m.stage}${m.total > 1 ? ` ${Math.floor(m.done * 100 / m.total)}%` : ''}`);
    else if (m.type === 'recording') {
      // saved straight away (the browser may ask once to allow several
      // downloads); the links stay in case one was blocked
      for (const f of m.files) {
        const a = document.createElement('a'); a.href = URL.createObjectURL(new Blob([f.data])); a.download = f.name; a.textContent = 'Download ' + f.name;
        $('downloads').appendChild(a);
        a.click();
      }
      log(`Recording saved to your Downloads folder: ${m.files.map(f => f.name).join(', ')}. Keep these files together.`, 'ok');
    }
    else if (m.type === 'exit') finish('Stopped.');
  };
  running = true; requestAnimationFrame(tick);
  status('Running. Player 1 starts the game as usual.', 'ok');
  worker.postMessage({ type: 'start', ctrl: ctrl.buffer, pad: pad1.buffer, pad2: pad2.buffer, ring,
    discs: [1, 2].filter(d => have.discs[d]).map(d => have.discs[d]), patches: have.patches, caches: have.caches,
    english: have.patches.length > 0, cards: have.card ? [{ slot: 0, data: have.card.slice(0) }] : [], extraArgs });
};
function finish(text) {
  running = false;
  if (audioCtx) { audioCtx.close().catch(() => {}); audioCtx = null; }
  setTimeout(() => { if (worker) worker.terminate(); worker = null; }, 1000);
  $('start').disabled = false; $('stop').disabled = true; status(text);
}
$('stop').onclick = () => { if (ctrl) Atomics.store(ctrl, 0, 1); $('stop').disabled = true; status('Stopping…'); };
loadRemembered();
