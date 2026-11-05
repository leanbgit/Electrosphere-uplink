// storage.js -- what the page keeps in this browser: IndexedDB (memory cards,
// English data) and the origin private file system (the remembered discs).
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

// memory cards: slot 0 is the game's card 1, slot 1 its backup
export async function cardGet(slot) {
  const d = await db();
  return new Promise(res => { const r = d.transaction(STORE).objectStore(STORE).get(slot); r.onsuccess = () => res(r.result || null); r.onerror = () => res(null); });
}
export async function cardPut(slot, data, time = Date.now()) {
  const d = await db();
  return new Promise(res => { const t = d.transaction(STORE, 'readwrite'); t.objectStore(STORE).put({ data, time }, slot); t.oncomplete = res; t.onerror = res; });
}

export async function idbGet(store, key) {
  const d = await db();
  return new Promise(res => { const r = d.transaction(store).objectStore(store).get(key); r.onsuccess = () => res(r.result || null); r.onerror = () => res(null); });
}
export async function idbPut(store, key, value) {
  const d = await db();
  return new Promise(res => { const t = d.transaction(store, 'readwrite'); t.objectStore(store).put(value, key); t.oncomplete = res; t.onerror = res; });
}
export async function idbDel(store, key) {
  const d = await db();
  return new Promise(res => { const t = d.transaction(store, 'readwrite'); t.objectStore(store).delete(key); t.oncomplete = res; t.onerror = res; });
}

export async function opfs() {
  try { return navigator.storage && navigator.storage.getDirectory ? await navigator.storage.getDirectory() : null; }
  catch { return null; }
}
