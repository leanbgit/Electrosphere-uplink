// saves.js -- the memory card panel. Card 1 lives in this browser (IndexedDB,
// storage.js); it can be downloaded, replaced with a file from an emulator or
// a DexDrive, and the card it replaced brought back.
import { $, log, app } from './common.js';
import { cardGet, cardPut } from './storage.js';

export async function showSaveInfo() {
  const c = await cardGet(0), b = await cardGet(1);
  $('saveinfo').textContent = c ? `Last saved ${new Date(c.time).toLocaleString()}.` : 'No save in this browser yet. The game creates one the first time it saves.';
  $('restorecard').hidden = !b;
}

async function download() {
  const c = await cardGet(0);
  if (!c) { $('saveinfo').textContent = 'There is no save to download yet.'; return; }
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([c.data])); a.download = 'card1.mcd'; a.click();
}

async function restorePrevious() {
  const b = await cardGet(1); if (!b || app.running) return;
  if (!confirm(`Bring back the save from ${new Date(b.time).toLocaleString()}? The current save becomes the backup.`)) return;
  const c = await cardGet(0);
  await cardPut(0, b.data, b.time);
  if (c) await cardPut(1, c.data, c.time);
  showSaveInfo();
  log('Previous memory card restored. It is used the next time you press Play.', 'ok');
}

async function load(e) {
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
}

export function initSaves() {
  showSaveInfo();
  $('export').onclick = download;
  $('importbtn').onclick = () => { if (!app.running) $('import').click(); };
  $('restorecard').onclick = restorePrevious;
  $('import').onchange = load;
}
