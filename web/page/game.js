// game.js -- starting and stopping the game.
//
// Three workers: the game (worker.js, the game's program in WebAssembly), the
// enhanced renderer (render-worker.js, WebGPU; only with Enhanced graphics),
// and the streamed game data (data-worker.js, already running: gamedata.js).
// The page shares with the game a control block, the controller and the sound
// ring (SharedArrayBuffers); sound is played by audio-worklet.js.
import { DEV, $, log, app } from './common.js';
import { cardGet, cardPut, idbGet, idbPut } from './storage.js';
import { have, dataWorker, renderReady, pauseFill } from './gamedata.js';
import { gfxBits } from './graphics.js';
import { onFrame, setHdSize } from './display.js';
import { loader, loaderStage, buildingEnglish } from './loader.js';
import { showSaveInfo } from './saves.js';

let worker = null;            // the game worker
let ctrl = null;              // Int32Array: [0] quit [1] audio running [2] sleep cell
let audioCtx = null, audioNode = null;

// online: null for a normal game. For online co-op (page/online.js):
//   { net: {role, delay, inbound, names}, card: Uint8Array | null (the host's card; the guest's
//     game never saves to this browser), gfx: {scale, bits} (the host's settings),
//     onMessage(m) for the worker's 'net-*' messages, onStop() }
// Returns false if the game could not start.
async function startGame(onlineSession) {
  if (!self.crossOriginIsolated) { $('status').textContent = 'This page cannot run the game: it is not cross-origin isolated. See Diagnostics.'; return false; }
  if (!app.gameData) { $('status').textContent = 'The game data is not available on this site.'; return false; }
  const online = app.online = onlineSession;
  app.running = true; renderReady(); $('stop').disabled = false;
  document.body.classList.add('live');
  loaderStage('prep');
  $('status').textContent = 'Starting…';

  ctrl = new Int32Array(new SharedArrayBuffer(16));
  const hd = await startRenderer(online && online.gfx);   // null: original picture
  // online co-op: widescreen and atmosphere change the game itself, so both computers need the host's
  if (online && online.gfx && online.gfx.scale > 0 && (online.gfx.bits & (1 | 64)) && !hd) {
    log('Online co-op: the host plays with Enhanced graphics and widescreen or atmosphere, which this browser cannot do here.', 'bad');
    stop(); return false;
  }
  const padSab = new SharedArrayBuffer(7 * 4); app.padView = new Int32Array(padSab);
  const ring = await startAudio();

  // the game data is streamed (data-worker.js): the game reads it through a port
  const english = !!have.patch[1];
  buildingEnglish.length = 0;
  const discs = [];
  let remote = null;
  {
    const ch = new MessageChannel(), sab = new SharedArrayBuffer(16 + (1 << 20));
    dataWorker.postMessage({ type: 'game', port: ch.port2, sab }, [ch.port2]);
    remote = { port: ch.port1, sab, files: app.gameData.files };
    if (navigator.storage && navigator.storage.persist) navigator.storage.persist();
  }
  // the English version: Project NEMO's patches, and the English data built
  // from them (stored in this browser, or prebuilt in the game data)
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
    else if (remote && app.gameData.files['en' + n]) { /* prebuilt English data comes with the streamed game data */ }
    else { log(`English data for disc ${n} will be built now (first time only, this can take a minute)…`); buildingEnglish.push(n); }
  }
  // memory card 1: this browser's, or online the host's
  const stored = online && online.card !== undefined ? (online.card ? { data: online.card } : null) : await cardGet(0);
  const cards = stored ? [{ slot: 0, data: stored.data.buffer.slice(0) }] : [];

  if (online) watchLongTasks();
  worker = new Worker('worker.js');
  worker.onmessage = fromGame;
  worker.onerror = e => { log('worker error: ' + e.message, 'bad'); stop(); };
  worker.postMessage({ type: 'start', ctrl: ctrl.buffer, pad: padSab, ring,
    discs, patches, caches, english, cards, hd, remote, net: online ? online.net : undefined }, [...(hd ? [hd.port] : []), ...(remote ? [remote.port] : [])]);
  $('status').textContent = 'Running. Click the screen area, then use the keyboard or a controller.';
  return true;
}

// Online co-op: the controllers travel through this page (worker -> page ->
// the connection -> the other page -> its worker), so a busy page holds up
// both games. Where the browser reports it (Chrome), long busy spells of this
// page go to the log next to the game's "stutter" lines.
let longTasks = null, longLogged = 0;
function watchLongTasks() {
  if (longTasks || typeof PerformanceObserver === 'undefined' ||
      !(PerformanceObserver.supportedEntryTypes || []).includes('longtask')) return;
  longTasks = new PerformanceObserver(list => {
    for (const e of list.getEntries())
      if (e.duration > 80 && performance.now() - longLogged > 2000) {
        longLogged = performance.now();
        log(`stutter: this page was busy for ${Math.round(e.duration)} ms`);
      }
  });
  longTasks.observe({ type: 'longtask' });
}

// Sound: an AudioWorklet reads the ring the game writes. Audio must start from
// a click, so this runs inside startGame. Returns the ring.
async function startAudio() {
  const ringFrames = 44100;                                // 1 s of headroom
  const ring = new SharedArrayBuffer(8 + ringFrames * 4);
  try {
    audioCtx = new AudioContext({ sampleRate: 44100, latencyHint: 'interactive' });
    await audioCtx.audioWorklet.addModule('audio-worklet.js');
    audioNode = new AudioWorkletNode(audioCtx, 'ac3-audio', { outputChannelCount: [2], processorOptions: { ring } });
    audioNode.connect(audioCtx.destination);
    audioNode.port.postMessage({ volume: $('volume').value / 100 });
    await audioCtx.resume();
    Atomics.store(ctrl, 1, 1);
  } catch (e) { log('Audio unavailable: ' + e, 'bad'); }
  return ring;
}

// The rest of the game data is stored in the background while playing, but
// only from a while after the game is up: during start-up it would compete
// with the game's own reads (and the browser storage writes hold the data
// worker up), so the game started much later.
const FILL_AFTER_MS = 15000;
let fillTimer = 0;
function fillLater() {
  if (fillTimer) return;                             // once per game
  const w = worker;
  fillTimer = setTimeout(() => { if (app.running && worker === w) dataWorker.postMessage({ type: 'download' }); }, FILL_AFTER_MS);
}

// messages from the game worker
async function fromGame(e) {
  const m = e.data, online = app.online;
  if (m.type === 'frame') { if (onFrame(m)) { loader.hide('game'); fillLater(); } }
  else if (m.type === 'log') log(m.text);
  else if (m.type === 'progress') loaderStage(m.stage, m.disc, m.done, m.total);
  else if (m.type === 'hd-off') hdOff(m.text);
  else if (m.type === 'error') { log(m.text, 'bad'); $('status').textContent = m.text.split('\n')[0]; stop(); }
  else if (m.type === 'card') {
    if (online && online.net.role === 'guest') return;    // the host's card: saved on the host's computer only
    if (m.slot === 0) { await cardPut(0, new Uint8Array(m.data)); showSaveInfo(); log('memory card 1 saved to browser storage', 'ok'); }
  }
  else if (m.type.startsWith('net-') || m.type === 'coop-phase') { if (online) online.onMessage(m); }
  else if (m.type === 'encache') { await idbPut('encache', m.disc, { data: new Uint8Array(m.data), time: Date.now() }); log(`English data for disc ${m.disc} saved in this browser (${(m.data.byteLength / 1048576).toFixed(1)} MB); later starts skip the build.`, 'ok'); }
  else if (m.type === 'exit') { log(`program exited (${m.code})`); $('status').textContent = 'Stopped.'; stop(); }
}

// Enhanced graphics: start the render worker before the game, so that a
// missing or failing WebGPU falls back to the original picture up front.
async function startRenderer(override) {     // override: {scale, bits} (online co-op: the host's)
  const bits = override ? override.bits : gfxBits();
  const want = self.navigator.gpu ? (override ? override.scale : +$('gfx').value) : 0, wide = want > 0 && !!(bits & 1);
  $('gfxhint').textContent = '';
  if (!want) { hdOff(null); return null; }
  // a canvas can be handed to a worker only once: use a fresh one every time
  const old = $('screen-hd'), cv = document.createElement('canvas');
  cv.id = 'screen-hd'; cv.setAttribute('aria-label', 'Game screen (enhanced)'); old.replaceWith(cv);
  document.body.classList.add('hd'); $('screen-wrap').classList.toggle('wide', wide);
  const r = $('screen-wrap').getBoundingClientRect();
  cv.width = Math.max(320, Math.round(r.width * devicePixelRatio)); cv.height = Math.max(240, Math.round(r.height * devicePixelRatio));
  setHdSize(cv.width, cv.height);
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
  app.renderWorker = rw;
  rw.onmessage = e => {
    const m = e.data;
    if (m.type === 'log') log(m.text);
    else if (m.type === 'lost') { log(m.text, 'bad'); $('status').textContent = m.text + ' Press Stop, then Play to continue from your last save.'; }
    else if (DEV && m.type === 'shot' && self.onShot) self.onShot(m);   // used by tests
  };
  $('ro-gfx').dataset.scale = result.scale;
  $('ro-gfx').textContent = `enhanced ${result.scale}×${wide ? ', 16:9' : ''}`;
  app.hdSet = new Int32Array(new SharedArrayBuffer(8));
  app.hdSet[0] = bits; app.hdSet[1] = 1;
  if (result.scale !== want) log(`enhanced graphics: ${want}× is too large for this GPU, using ${result.scale}×`);
  return { scale: result.scale, wide, settings: app.hdSet.buffer, port: chan.port1, done, skip: DEV && self.hdSkip || 0 };
}

// back to the original picture (text: why, for the page and the log)
function hdOff(text) {
  document.body.classList.remove('hd'); $('screen-wrap').classList.remove('wide');
  $('ro-gfx').textContent = 'original';
  if (text) { $('gfxhint').textContent = text; log(text, 'bad'); }
  if (app.renderWorker) { app.renderWorker.terminate(); app.renderWorker = null; }
  app.hdSet = null;
}

function stop() {
  if (app.online) { const o = app.online; app.online = null; o.onStop(); }
  if (app.renderWorker) { const r = app.renderWorker; app.renderWorker = null; setTimeout(() => r.terminate(), 1600); }
  if (worker) {
    Atomics.store(ctrl, 0, 1);                             // ask the game to quit cleanly…
    const w = worker; setTimeout(() => w.terminate(), 1500); // …then make sure
    worker = null;
  }
  if (audioCtx) { audioCtx.close(); audioCtx = null; audioNode = null; }
  clearTimeout(fillTimer); fillTimer = 0;
  pauseFill();
  app.padView = null; app.running = false; $('stop').disabled = true; loader.cancel();
  document.body.classList.remove('live', 'hd'); $('screen-wrap').classList.remove('wide');
  $('fps').textContent = '–'; $('ro-gfx').textContent = '–'; renderReady();
}

// Online co-op (online.js) starts and stops the game through this object; the
// online test (server/test/online.test.js) replaces start with a stand-in.
export const game = { start: startGame, stop };

export function initGame() {
  $('volume').oninput = () => audioNode && audioNode.port.postMessage({ volume: $('volume').value / 100 });
  $('start').onclick = () => game.start(null);
  $('stop').onclick = () => { stop(); $('status').textContent = 'Stopped.'; };
}
