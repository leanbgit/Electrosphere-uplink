// discs.js -- the player's own game discs, and whether the game can start.
//
// The site provides no game data. The player selects (or drops) the .bin
// images of both discs of the Japanese release; each is checked here (an Ace
// Combat 3 disc, as a raw 2352-byte image, with the "Rev 1" ACE3.BIN) and can
// be remembered in this browser's own file storage (OPFS) so later visits
// start without selecting them again. Project NEMO's English patches ship
// with the site; the English data is built from them on the first start and
// kept (storage.js, 'encache').
//
// Each disc can come from two places, in this order of preference:
//   selected  picked or dropped in this visit
//   stored    remembered in this browser
import { $, log, app } from './common.js';
import { idbDel, opfs } from './storage.js';
import { loader } from './loader.js';

export const have = { disc: {}, patch: {} };          // [kind][1|2] = { file | url, src }
let storing = 0;                                      // discs being copied into the browser's storage
const MB = n => (n / 1048576).toFixed(0);

// Ace Combat 3 "Rev 1" (SLPS-02020/02021): both discs carry this ACE3.BIN.
// Same value as web/worker.js, which checks again when the game starts.
const EXE_SHA256 = '2c3f2ce41268176128973ea0cd9b28784a26c0061bff5550c99599388f13ebe9';
const RAW = 2352;
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

// the files the player selected or dropped: which disc each one is, if any
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
        log(`${f.name}: too small for an Ace Combat 3 disc (${MB(f.size)} MB; each disc is about 690 MB). ` +
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
  renderReady();
}

// ---- remembered disc images: the origin private file system (OPFS)
const discMark = d => `ac3.disc${d}`;               // set only once a copy is complete
async function storeDisc(d, file) {
  const dir = await opfs();
  if (!dir) return log('This browser cannot store disc images; select them again next visit.', 'dim');
  try {
    const est = await navigator.storage.estimate();
    const old = have.disc[d] && have.disc[d].src === 'stored' ? have.disc[d].file.size : 0;
    if (est.quota - est.usage + old < file.size * 1.02)
      return log(`Not enough browser storage to remember disc ${d} (needs ${MB(file.size)} MB).`, 'bad');
    if (navigator.storage.persist) await navigator.storage.persist();
    const h = await dir.getFileHandle(`disc${d}.bin`, { create: true });
    if (!h.createWritable) return log('This browser cannot write to its file storage; discs will not be remembered.', 'dim');
    localStorage.removeItem(discMark(d));
    storing++; renderReady();
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
        if (!app.running) {
          loader.show('store', `Remembering disc ${d} in this browser`, 'Only once: next time the discs load by themselves.');
          loader.set('store', pct, pct);
        }
      }
    }
    await w.close();
    localStorage.setItem(discMark(d), String(file.size));
    log(`Disc ${d} remembered in this browser; next time it loads by itself.`, 'ok');
  } catch (e) {
    log(`Could not remember disc ${d}: ${e.message || e}`, 'bad');
  } finally {
    loader.hide('store');
    storing = Math.max(0, storing - 1);
    if (!storing) $('storeprog').textContent = '';
    renderReady();
  }
}

// What this browser remembers (the discs), and the English patches that come
// with the site. Also clears what older versions of the page kept.
export async function loadRemembered() {
  const dir = await opfs();
  for (const d of [1, 2]) {
    if (dir && localStorage.getItem(discMark(d)) && !(have.disc[d] && have.disc[d].src === 'selected')) {
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
  // game data downloaded from the site by older versions of the page
  if (dir) {
    try {
      for await (const name of dir.keys())
        if (name.startsWith('gamedata-')) { await dir.removeEntry(name, { recursive: true }); log('Removed the game data an older version of this site had stored in this browser.', 'dim'); }
    } catch { /* not listable, or in use by another tab: tried again next visit */ }
  }
}

async function forgetAll() {
  const sizes = [1, 2].filter(d => have.disc[d] && have.disc[d].src === 'stored').map(d => have.disc[d].file.size);
  if (sizes.length && !confirm(`Forget the discs remembered in this browser (${MB(sizes.reduce((a, b) => a + b, 0))} MB)? ` +
                               'You will need to select them again to play. Your saves and settings are kept.')) return;
  const dir = await opfs();
  for (const d of [1, 2]) {
    localStorage.removeItem(discMark(d));
    if (dir) await dir.removeEntry(`disc${d}.bin`).catch(() => {});
    await idbDel('patches', d); await idbDel('encache', d);
    if (have.disc[d] && have.disc[d].src === 'stored') delete have.disc[d];
  }
  await idbDel('encache', 'packs');
  log('Remembered discs and English data removed from this browser (saves kept).', 'ok');
  await loadRemembered();
  renderReady();
}

// The disc list, the Play button, the language readout and the hint on the
// idle screen. In an online room (app.lobby) the button is Play Online: it
// starts the co-op game (the host's, once the other player is connected).
const SRC = { selected: 'selected', stored: 'remembered in this browser' };
export const discsReady = () => !!have.disc[1];
export function renderReady() {
  const ul = $('files');
  if (ul) {
    ul.replaceChildren();
    for (const d of [1, 2]) {
      const x = have.disc[d], li = document.createElement('li');
      li.textContent = x ? `Disc ${d}: ${SRC[x.src]} (${MB(x.file.size)} MB)` : `Disc ${d}: not selected`;
      li.className = x ? 'ok' : 'dim';
      ul.appendChild(li);
    }
  }
  const ready = discsReady(), lobby = app.lobby;
  $('start').disabled = !ready || app.running || (lobby && !lobby.canStart);
  $('start').querySelector('span').textContent = lobby ? 'Play Online' : 'Play';
  $('to-online').hidden = !!lobby;
  if ($('forget')) $('forget').disabled = app.running || storing > 0 || ![1, 2].some(d => localStorage.getItem(discMark(d)));
  // always English (Project NEMO's patches ship with the site); a copy of the
  // site without them can only run the original Japanese text
  if ($('enhint')) $('enhint').textContent = !have.patch[1] ? 'This copy of the site is missing patches/CD1.xdelta, so the game will be in Japanese.'
    : !have.patch[2] ? 'This copy of the site is missing patches/CD2.xdelta, so disc 2 will be in Japanese.' : '';
  // checklist and readouts
  if ($('step-discs')) $('step-discs').classList.toggle('done', ready);
  $('ro-lang').textContent = have.patch[1] ? 'English' : 'Japanese';
  const touchOnly = matchMedia('(pointer: coarse)').matches && !matchMedia('(any-pointer: fine)').matches;
  $('idle-hint').textContent = (ready ? (lobby ? lobby.hint : '') : 'Add your game discs under Settings, Game discs, to play.') +
    (ready && !have.disc[2] && !lobby ? ' Disc 2 is not selected: the game will stop at the disc change.' : '') +
    (touchOnly ? ' You will need a keyboard or a game controller: touch controls are not available yet.' : '');
}

export function initDiscs() {
  $('pick').onchange = e => { classify([...e.target.files]); e.target.value = ''; };
  if ($('forget')) $('forget').onclick = () => { if (!app.running) forgetAll(); };
  const drop = $('drop');
  drop.ondragover = e => { e.preventDefault(); drop.classList.add('over'); };
  drop.ondragleave = () => drop.classList.remove('over');
  drop.ondrop = e => { e.preventDefault(); drop.classList.remove('over'); classify([...e.dataTransfer.files]); };
}
