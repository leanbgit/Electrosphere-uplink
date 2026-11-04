// server/test/limits.test.js -- the room server's limits and checks (src/index.js;
// security audit M1, M3, L1, L3). Against a running server:
//   npx wrangler dev   then   node test/limits.test.js [ws://localhost:8787/api/ws]
// (needs the "ws" package: npm i ws)
'use strict';
const WebSocket = require('ws');
const URL = process.argv[2] || 'ws://localhost:8787/api/ws';
let failed = 0;
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failed++; };

function client(label, headers) {
  const ws = new WebSocket(URL, headers ? { headers } : undefined), inbox = [], waiters = [];
  let closed = false;
  ws.on('message', d => {
    const m = JSON.parse(d);
    const i = waiters.findIndex(w => w.t === m.t);
    if (i >= 0) { waiters.splice(i, 1)[0].res(m); } else inbox.push(m);
  });
  ws.on('close', () => { closed = true; });
  const open = new Promise((r, j) => { ws.on('open', () => r(true)); ws.on('unexpected-response', (q, res) => r(res.statusCode)); ws.on('error', () => r(false)); });
  return {
    label, open, ws, inbox, closed: () => closed,
    send: m => { try { ws.send(JSON.stringify(m)); } catch {} },
    next: (t, ms = 3000) => new Promise((res, rej) => {
      const i = inbox.findIndex(m => m.t === t);
      if (i >= 0) return res(inbox.splice(i, 1)[0]);
      const w = { t, res }; waiters.push(w);
      setTimeout(() => { const k = waiters.indexOf(w); if (k >= 0) { waiters.splice(k, 1); rej(new Error(`${label}: no '${t}' within ${ms} ms`)); } }, ms);
    }),
    close: () => ws.close(),
  };
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
const codeLike = c => /^NEMO-[A-Z2-9]{4}$/.test(c);

(async () => {
  const V = 'test-1';
  const host = client('host'); await host.open;
  host.send({ t: 'host', name: 'Nemo', public: true, version: V });
  const room = await host.next('room');
  check(codeLike(room.code), `room code ${room.code}`);

  // codes come from a secure source and stay in the same format
  const codes = new Set();
  for (let i = 0; i < 6; i++) { const h = client('h' + i); await h.open; h.send({ t: 'host', name: 'x', public: false, version: V }); codes.add((await h.next('room')).code); h.close(); }
  check([...codes].every(codeLike) && codes.size === 6, 'new codes: same format, all different');

  // M1: wrong codes are limited (5 a minute per connection)
  const guess = client('guess'); await guess.open;
  let unknown = 0;
  for (let i = 0; i < 5; i++) { guess.send({ t: 'join', code: 'NEMO-ZZZ' + 'ABCDE'[i], name: 'g', version: V }); try { await guess.next('unknown'); unknown++; } catch {} }
  check(unknown === 5, 'five wrong codes: each answered "no room"');
  guess.send({ t: 'join', code: 'NEMO-ZZZF', name: 'g', version: V });
  const lim = await guess.next('limit').catch(() => null);
  await sleep(300);
  check(!!lim && guess.closed(), 'the sixth: refused with a message, connection closed');

  // M1/M2: one request at a time; requests withdrawn when the asker leaves; at most 3 waiting
  const a = client('a'); await a.open;
  a.send({ t: 'join', code: room.code, name: 'A', version: V });
  const ra = await host.next('request');
  check(ra.via === 'code', 'joining by code asks the host');
  a.send({ t: 'join', code: room.code, name: 'A', version: V });
  check(/already asking/.test((await a.next('error')).text), 'a second request from the same connection is refused');
  const b = client('b'); await b.open; b.send({ t: 'join', id: room.id, name: 'B', version: V }); await host.next('request');
  const c = client('c'); await c.open; c.send({ t: 'join', id: room.id, name: 'C', version: V }); await host.next('request');
  const d = client('d'); await d.open; d.send({ t: 'join', id: room.id, name: 'D', version: V });
  check(!!(await d.next('busy')), 'a fourth person waiting: "busy"');
  a.close();
  check((await host.next('withdrawn')).from === ra.from, 'the asker left: the host\'s request is withdrawn');
  check(!host.inbox.some(m => m.t === 'signal') && !b.inbox.some(m => m.t === 'signal'), 'nothing is passed along before the host accepts');

  // L1: names lose invisible and direction characters
  b.close(); c.close(); d.close();
  await sleep(200);
  const h3 = client('h3'); await h3.open;
  h3.send({ t: 'host', name: '‮evil​  name\u0007 ﻿', public: true, version: V });
  const r3 = await h3.next('room');
  const look = client('look'); await look.open; look.send({ t: 'list' });
  const listed = (await look.next('rooms')).rooms.find(r => r.id === r3.id);
  check(listed && listed.name === 'evil name', `names cleaned ("${listed && listed.name}")`);
  const h4 = client('h4'); await h4.open;
  h4.send({ t: 'host', name: 'Ícaro 空 ✈️ Ünal', public: true, version: V });
  const r4 = await h4.next('room'); look.send({ t: 'list' });
  check((await look.next('rooms')).rooms.find(r => r.id === r4.id).name === 'Ícaro 空 ✈️ Ünal', 'accents, other scripts and emoji are kept');
  const h5 = client('h5'); await h5.open;
  h5.send({ t: 'host', name: 'x'.repeat(100), public: false, version: 'v'.repeat(5000) });
  check(!!(await h5.next('room')), 'oversized fields: still answered (clipped)');

  // M3: too many messages close the connection
  const flood = client('flood'); await flood.open;
  for (let i = 0; i < 200; i++) flood.send({ t: 'list' });
  await sleep(1000);
  check(flood.closed() && flood.inbox.some(m => m.t === 'limit'), `200 messages at once: closed after ${flood.inbox.filter(m => m.t === 'rooms').length} answers`);
  const calm = client('calm'); await calm.open;
  let answered = 0;
  for (let i = 0; i < 30; i++) { calm.send({ t: 'list' }); try { await calm.next('rooms'); answered++; } catch {} await sleep(60); }
  check(answered === 30 && !calm.closed(), 'normal use (about 15 a second): never limited');

  // M3 / M1 per network address. Cloudflare sets CF-Connecting-IP itself; the
  // local runtime passes the test's value through, which lets this be checked.
  const ipA = '203.0.113.' + (1 + Math.floor(Math.random() * 250)), ipB = '198.51.100.' + (1 + Math.floor(Math.random() * 250));
  const conns = [];
  const firstReply = async ip => { const x = client('ip', { 'CF-Connecting-IP': ip }); await x.open; conns.push(x); x.send({ t: 'list' });
    return x.next('rooms', 1500).then(() => 'rooms', () => (x.inbox.some(m => m.t === 'limit') ? 'limit' : 'none')); };
  const replies = []; for (let i = 0; i < 7; i++) replies.push(await firstReply(ipA));
  check(replies.slice(0, 6).every(r => r === 'rooms') && replies[6] === 'limit', `connections from one address: 6 allowed, the 7th refused (${replies.join(' ')})`);
  check((await firstReply(ipB)) === 'rooms', 'another address is not affected');
  for (const x of conns) x.close();
  await sleep(300);
  const ipC = '192.0.2.' + (1 + Math.floor(Math.random() * 250));
  const tries = [];
  for (let i = 0; i < 6; i++) {           // a new connection for every guess: still counted per address
    const x = client('g', { 'CF-Connecting-IP': ipC }); await x.open;
    x.send({ t: 'join', code: 'NEMO-YYY' + 'ABCDE2'[i], name: 'g', version: V });
    tries.push(await x.next('unknown', 1500).then(() => 'unknown', () => (x.inbox.some(m => m.t === 'limit') ? 'limit' : 'none')));
    x.close(); await sleep(100);
  }
  check(tries.slice(0, 5).every(r => r === 'unknown') && tries[5] === 'limit', `wrong codes from one address over new connections: limited too (${tries.join(' ')})`);

  // L3: other websites' pages are refused
  const foreign = client('foreign', { Origin: 'https://evil.example' });
  check((await foreign.open) === 403, 'a page from another site is refused (403)');
  const own = client('own', { Origin: new globalThis.URL(URL.replace(/^ws/, 'http')).origin });
  check((await own.open) === true, 'the site\'s own pages connect');

  for (const x of [host, h3, h4, h5, look, calm, own]) x.close();
  console.log(failed ? `${failed} FAILED` : 'all passed');
  process.exit(failed ? 1 : 0);
})().catch(e => { console.log('FAIL ' + e.message); process.exit(1); });
