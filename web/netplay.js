// netplay.js -- online co-op in lockstep (docs/coop/S6_network.md).
//
// Both computers run the whole game; each field only the two controllers'
// states travel between them. Controller 1 is the host's, controller 2 the
// guest's, on both computers. A controller sampled at field F is used at
// field F + delay on both computers, so each side has `delay` fields to get
// its input to the other before it is needed; if it has not arrived, the
// game waits for it.
//
// Every SYNC_EVERY fields both send a fingerprint of the game's shared state
// (runtime/hle/coop_sync.c, "sim"); a difference means the two games have
// gone apart (a desync), which is reported to the page.
//
// Messages between the computers (and between the page and the game worker)
// are records of four little-endian int32: kind, field, a, b.
//   INPUT  a, b: a controller (packPad)
//   HASH   a, b: the fingerprint's low and high halves
//   BYE    the other player left
// The page writes the other computer's records into a shared ring
// (NetRing) that the game worker reads; the worker's records for the other
// computer go to the page as 'net-out' messages.
'use strict';
const NET_INPUT = 1, NET_HASH = 2, NET_BYE = 3;
const NET_SYNC_EVERY = 60;
const NET_RECORD = 4;                       // int32 per record
const NET_RING_RECORDS = 8192;

// controller: Int32Array(7) [connected, analog, buttons, lx, ly, rx, ry] <-> two int32
function netPackPad(p) {
  return [(p[0] ? 1 : 0) | (p[1] ? 2 : 0) | ((p[2] & 0xFFFF) << 16),
          (p[3] & 255) | (p[4] & 255) << 8 | (p[5] & 255) << 16 | (p[6] & 255) << 24];
}
function netUnpackPad(a, b, out) {
  out[0] = a & 1; out[1] = (a >> 1) & 1; out[2] = (a >>> 16) & 0xFFFF;
  out[3] = b & 255; out[4] = (b >>> 8) & 255; out[5] = (b >>> 16) & 255; out[6] = (b >>> 24) & 255;
  return out;
}
const NET_NEUTRAL = netPackPad([1, 0, 0, 128, 128, 128, 128]);   // before the first real input arrives

// The ring from the page to the game worker: Int32 [written] + records.
const NetRing = {
  create() { return new SharedArrayBuffer(4 * (1 + NET_RING_RECORDS * NET_RECORD)); },
  // records: Int32Array (or an ArrayBuffer of them) from the other computer
  write(sab, records) {
    const ring = new Int32Array(sab), r = records instanceof Int32Array ? records : new Int32Array(records);
    let n = Atomics.load(ring, 0);
    for (let i = 0; i + NET_RECORD <= r.length; i += NET_RECORD, n++) {
      const at = 1 + (n % NET_RING_RECORDS) * NET_RECORD;
      for (let k = 0; k < NET_RECORD; k++) ring[at + k] = r[i + k];
    }
    Atomics.store(ring, 0, n);
    Atomics.notify(ring, 0);
  },
};

// The game worker's side. cfg: { role: 'host' | 'guest', delay, inbound (NetRing),
// test: optional Uint8Array of an .ac3i recording (development: local input from it) }
function NetPlay(cfg, hooks) {
  const me = cfg.role === 'guest' ? 1 : 0;
  const delay = Math.max(1, cfg.delay | 0 || 4);
  const ring = new Int32Array(cfg.inbound);
  let read = 0, field = 0, gone = false, desynced = false, waitingSince = 0, lastWaitReport = 0;
  const local = new Map(), remote = new Map(), hashes = new Map(), peerHashes = new Map();
  const p1 = new Int32Array(7), p2 = new Int32Array(7);
  let out = [];
  const stats = { waits: 0, waitMs: 0, maxAhead: 0 };

  function send(kind, f, a, b) { out.push(kind, f, a, b); }
  function flush() { if (out.length) { hooks.send(new Int32Array(out)); out = []; } }
  function compare(f) {
    const h = hashes.get(f), p = peerHashes.get(f);
    if (!h || !p) return;
    hashes.delete(f); peerHashes.delete(f);
    if ((h[0] !== p[0] || h[1] !== p[1]) && !desynced) { desynced = true; hooks.desync(f); }
  }
  function drain() {
    const n = Atomics.load(ring, 0);
    for (; read < n; read++) {
      const at = 1 + (read % NET_RING_RECORDS) * NET_RECORD;
      const kind = ring[at], f = ring[at + 1], a = ring[at + 2], b = ring[at + 3];
      if (kind === NET_INPUT) { remote.set(f, [a, b]); if (f - field > stats.maxAhead) stats.maxAhead = f - field; }
      else if (kind === NET_HASH) { peerHashes.set(f, [a, b]); compare(f); }
      else if (kind === NET_BYE) gone = true;
    }
    return n;
  }
  // development: the controller as a recording has it at field f (port 0 = the host's)
  function testSample(f, port = me) {
    const t = cfg.test, o = 16 + 24 * f + 4 + 10 * port;
    if (!t || o + 8 > t.length) return NET_NEUTRAL;
    return netPackPad([t[o], t[o + 1] === 0x73 ? 1 : 0, t[o + 2] | t[o + 3] << 8, t[o + 4], t[o + 5], t[o + 6], t[o + 7]]);
  }

  return {
    // once per field, before the game reads the controllers
    step(localPad) {
      const f = field++;
      const s = cfg.test ? testSample(f + delay) : netPackPad(localPad);
      local.set(f + delay, s);
      send(NET_INPUT, f + delay, s[0], s[1]);
      flush();
      // before the first sent controller can arrive: the same on both computers
      const first = port => cfg.test ? testSample(f, port) : NET_NEUTRAL;
      const mine = f < delay ? first(me) : local.get(f);
      local.delete(f);
      let theirs = first(1 - me);
      if (f >= delay) {
        let n = drain();
        if (!remote.has(f) && !gone) {
          const t0 = performance.now();
          while (!remote.has(f) && !gone) {
            if (hooks.quit()) break;
            Atomics.wait(ring, 0, n, 100);
            n = drain();
            const waited = performance.now() - t0;
            if (waited > 1000 && performance.now() - lastWaitReport > 1000) {
              lastWaitReport = performance.now();
              hooks.waiting(Math.round(waited / 1000));
            }
          }
          stats.waits++; stats.waitMs += performance.now() - t0;
          if (lastWaitReport) { lastWaitReport = 0; hooks.waiting(0); }
        }
        const r = remote.get(f);
        if (r) { theirs = r; remote.delete(f); }
      }
      const host = me === 0 ? mine : theirs, guest = me === 0 ? theirs : mine;
      netUnpackPad(host[0], host[1], p1);
      netUnpackPad(guest[0], guest[1], p2);
    },
    pad1: () => p1,
    pad2: () => p2,
    field: () => field,
    syncDue: () => !gone && field > 0 && field % NET_SYNC_EVERY === 0,
    // the game's fingerprint for this field (after syncDue)
    sync(lo, hi) {
      hashes.set(field, [lo | 0, hi | 0]);
      send(NET_HASH, field, lo | 0, hi | 0);
      flush();
      drain();
      compare(field);
    },
    stats: () => ({ ...stats, field, delay }),
  };
}

if (typeof module !== 'undefined') module.exports = { NetRing, NetPlay, NET_INPUT, NET_HASH, NET_BYE, netPackPad, netUnpackPad };
