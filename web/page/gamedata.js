// gamedata.js -- the game data, streamed from the website.
//
// The site hosts the game data (gamedata/manifest.json, written by
// tools/make_gamedata.py); web/data-worker.js streams it to the game and keeps
// what it downloaded in this browser. A copy of the site without it cannot
// start the game (see the README to make it from your own discs). Players no
// longer select disc files. Project NEMO's English patches ship with the site.
// Disc images remembered by older versions of the page (OPFS) are no longer
// used; "Forget stored game data" removes them.
import { $, log, app } from './common.js';
import { idbDel, opfs } from './storage.js';

export const have = { patch: {} };                   // [1|2] = { url, src: 'bundled' }
export const dataWorker = new Worker('data-worker.js');
let dataStatus = null;                               // the data worker's last 'status'
let fillAsked = false;                               // the player pressed "Download … now" (keeps going after Stop)
const MB = n => (n / 1048576).toFixed(0);

const discMark = d => `ac3.disc${d}`;               // set only once a copy is complete

// the English patches that come with the site
export async function loadRemembered() {
  for (const d of [1, 2]) {
    // Project NEMO's patches ship with the site (used with their permission).
    const r = await fetch(`patches/CD${d}.xdelta`, { method: 'HEAD' }).catch(() => null);
    if (r && r.ok) have.patch[d] = { url: `patches/CD${d}.xdelta`, src: 'bundled' };
    await idbDel('patches', d);                        // copies selected by older versions of the page
  }
}

async function forgetAll() {
  const s = dataStatus;
  if (!confirm(`Delete the game data stored in this browser${s && s.bytesHave ? ` (${MB(s.bytesHave)} MB)` : ''}? ` +
               'It is downloaded again the next time you play. Your saves and settings are kept.')) return;
  const dir = await opfs();
  for (const d of [1, 2]) {
    localStorage.removeItem(discMark(d));
    if (dir) await dir.removeEntry(`disc${d}.bin`).catch(() => {});
    await idbDel('patches', d); await idbDel('encache', d);
  }
  fillAsked = false;
  dataWorker.postMessage({ type: 'forget' });
  log('Stored game data and English data removed from this browser (saves kept).', 'ok');
  await loadRemembered();
  renderReady();
}

// The Play button, the language readout and the hint on the idle screen.
export function renderReady() {
  const ready = !!app.gameData;
  $('start').disabled = !ready || app.running;
  // always English (Project NEMO's patches ship with the site); a copy of the
  // site without them can only run the original Japanese text
  if ($('enhint')) $('enhint').textContent = !have.patch[1] ? 'This copy of the site is missing patches/CD1.xdelta, so the game will be in Japanese.'
    : !have.patch[2] ? 'This copy of the site is missing patches/CD2.xdelta, so disc 2 will be in Japanese.' : '';
  // checklist and readouts
  for (const id of ['step-discs', 'step-settings']) if ($(id)) $(id).classList.toggle('done', ready);
  $('ro-lang').textContent = have.patch[1] ? 'English' : 'Japanese';
  renderData();
  const touchOnly = matchMedia('(pointer: coarse)').matches && !matchMedia('(any-pointer: fine)').matches;
  $('idle-hint').textContent = (ready ? '' : app.gameDataState === 'loading' ? 'Getting the game ready…' : 'The game data could not be loaded from this site (see the log).') +
    (touchOnly ? ' You will need a keyboard or a game controller: touch controls are not available yet.' : '');
}

function renderData() {
  const el = $('gd-status');
  if (!el) return;                                  // (a page without the game data line)
  if ($('forget')) $('forget').disabled = app.running || !dataStatus || !dataStatus.bytesHave;
  if (!app.gameData) { el.textContent = app.gameDataState === 'loading' ? 'Checking…' : 'Not available on this copy of the site.'; return; }
  const s = dataStatus;
  // kept in this browser's own storage (data-worker.js), filled in while playing
  el.textContent = !s ? 'Checking…'
    : !s.stored ? 'This browser cannot store it (private window?): the game is downloaded as it plays.'
    : s.bytesHave >= s.bytesTotal ? `All of it is stored in this browser (${MB(s.bytesTotal)} MB): the game plays without downloading.`
    : !s.bytesHave ? 'Nothing stored yet: it downloads as you play.'
    : `Stored in this browser: ${MB(s.bytesHave)} of ${MB(s.bytesTotal)} MB${s.background ? (app.running && !fillAsked ? ', filling in while you play' : ', downloading') : ''}.` +
      (s.full ? ' The browser\'s storage is full.' : '');
  // the whole game ahead of time (optional): into this browser's storage, not a file
  const fill = $('gd-fill');
  if (fill) {
    fill.hidden = !s || !s.stored || s.full || s.bytesHave >= s.bytesTotal;
    fill.disabled = app.running && !fillAsked;      // (while playing it fills in anyway)
    fill.textContent = s && s.background && (fillAsked || !app.running) ? 'Pause the download'
      : s && s.bytesHave ? 'Download the rest into this browser now' : 'Download it into this browser now';
  }
}

// When a game stops, the background download stops too, unless the player
// asked for it.
export function pauseFill() {
  if (!fillAsked) dataWorker.postMessage({ type: 'pause' });
}

function toggleFill() {
  if (dataStatus && dataStatus.background) { fillAsked = false; dataWorker.postMessage({ type: 'pause' }); }
  else { fillAsked = true; if (navigator.storage && navigator.storage.persist) navigator.storage.persist(); dataWorker.postMessage({ type: 'download' }); }
}

export function initGameData() {
  if ($('forget')) $('forget').onclick = () => { if (!app.running) forgetAll(); };
  if ($('gd-fill')) $('gd-fill').onclick = toggleFill;
  dataWorker.onmessage = e => {   // (renderData also runs from renderReady)
    const m = e.data;
    if (m.type === 'ready') { app.gameData = m.manifest; app.gameDataState = 'ready'; renderReady(); renderData(); }
    else if (m.type === 'unavailable') { app.gameDataState = 'none'; renderReady(); renderData(); }
    else if (m.type === 'status') {
      dataStatus = m; renderData();
      if (app.running && !$('loader').hidden && m.fetched) $('ld-sub').textContent = `Downloading game data: ${MB(m.fetched)} MB`;
    }
    else if (m.type === 'log') log(m.text, 'dim');
    else if (m.type === 'error') { log(m.text, 'bad'); if (app.running) $('status').textContent = m.text; }
  };
  dataWorker.onerror = () => { app.gameDataState = 'none'; renderReady(); renderData(); };
  dataWorker.postMessage({ type: 'init', url: 'gamedata/manifest.json' });
}
