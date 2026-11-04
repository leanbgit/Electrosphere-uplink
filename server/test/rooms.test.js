// server/test/rooms.test.js -- checks the room server's protocol (src/index.js)
// against a running server:  npx wrangler dev  then  node test/rooms.test.js [ws://localhost:8787/api/ws]
// (needs the "ws" package: npm i ws)
'use strict';
const WebSocket = require('ws');
const URL = process.argv[2] || 'ws://localhost:8787/api/ws';
let failed = 0;
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) failed++; };

function client(label) {
  const ws = new WebSocket(URL), inbox = [], waiters = [];
  ws.on('message', d => {
    const m = JSON.parse(d);
    const i = waiters.findIndex(w => w.t === m.t);
    if (i >= 0) { waiters.splice(i, 1)[0].res(m); } else inbox.push(m);
  });
  const open = new Promise(r => ws.on('open', r));
  return {
    label, open,
    send: m => ws.send(JSON.stringify(m)),
    next: (t, ms = 3000) => new Promise((res, rej) => {
      const i = inbox.findIndex(m => m.t === t);
      if (i >= 0) return res(inbox.splice(i, 1)[0]);
      const w = { t, res }; waiters.push(w);
      setTimeout(() => { const k = waiters.indexOf(w); if (k >= 0) { waiters.splice(k, 1); rej(new Error(`${label}: no '${t}' within ${ms} ms`)); } }, ms);
    }),
    close: () => ws.close(),
  };
}

(async () => {
  const V = 'test-1';
  // a private room, joined by its code: the host is asked too
  const host = client('host'); await host.open;
  host.send({ t: 'host', name: 'Nemo', public: false, version: V });
  const room = await host.next('room');
  check(/^NEMO-[A-Z2-9]{4}$/.test(room.code), `room code ${room.code}`);
  const look = client('looker'); await look.open;
  look.send({ t: 'list' });
  check(!(await look.next('rooms')).rooms.some(r => r.id === room.id), 'a private room is not listed');
  const g = client('guest'); await g.open;
  g.send({ t: 'join', code: room.code.slice(5).toLowerCase(), name: 'Rena', version: V });
  const creq = await host.next('request');
  check(creq.name === 'Rena' && creq.via === 'code', 'joining by code: the host is asked');
  g.send({ t: 'signal', data: { sdp: 'too-early' } });
  host.send({ t: 'answer', to: creq.from, accept: true });
  const gj = await g.next('joined'), hj = await host.next('joined');
  check(gj.peer === 'Nemo' && hj.peer === 'Rena', 'accepted: both told, with names');
  g.send({ t: 'signal', data: { sdp: 'offer-from-guest' } });
  check((await host.next('signal')).data.sdp === 'offer-from-guest', 'signals reach the host (none before the host accepted)');
  host.send({ t: 'signal', data: { sdp: 'answer' } });
  check((await g.next('signal')).data.sdp === 'answer', 'signals reach the guest');
  const late = client('late'); await late.open;
  late.send({ t: 'join', code: room.code, name: 'Late', version: V });
  check(!!(await late.next('full')), 'a third player: the room is full');
  const old = client('old'); await old.open;

  // a public room: listed; joining from the list asks the host
  const h2 = client('host2'); await h2.open;
  h2.send({ t: 'host', name: 'Abyss', public: true, version: V });
  const r2 = await h2.next('room');
  look.send({ t: 'list' });
  const listed = (await look.next('rooms')).rooms.find(r => r.id === r2.id);
  check(listed && listed.name === 'Abyss' && !('code' in listed), 'a public room is listed (without its code)');
  old.send({ t: 'join', id: r2.id, name: 'Old', version: 'other' });
  check((await old.next('version')).want === V, 'a different site version is refused');
  look.send({ t: 'join', id: r2.id, name: 'Looker', version: V });
  const req = await h2.next('request');
  check(req.name === 'Looker' && req.via === 'list', 'the host is asked');
  h2.send({ t: 'answer', to: req.from, accept: false });
  check(!!(await look.next('declined')), 'declined');
  look.send({ t: 'join', id: r2.id, name: 'Looker', version: V });
  const req2 = await h2.next('request');
  h2.send({ t: 'answer', to: req2.from, accept: true });
  check(!!(await look.next('joined')) && !!(await h2.next('joined')), 'accepted: both told');
  const other = client('other'); await other.open;
  other.send({ t: 'list' });
  check(!(await other.next('rooms')).rooms.some(r => r.id === r2.id), 'a room with two players leaves the list');

  // leaving
  look.close();
  check(!!(await h2.next('left')), 'the guest left: the host is told');
  other.send({ t: 'list' });
  check((await other.next('rooms')).rooms.some(r => r.id === r2.id), 'and the room is listed again');
  h2.send({ t: 'start' });
  other.send({ t: 'list' });
  check(!(await other.next('rooms')).rooms.some(r => r.id === r2.id), 'a started game is not listed');
  host.close();
  check(!!(await g.next('left')), 'the host left: the guest is told');
  other.send({ t: 'join', code: room.code, name: 'X', version: V });
  check(!!(await other.next('unknown')), 'its code no longer works');

  for (const c of [g, late, old, h2, other]) c.close();
  console.log(failed ? `${failed} FAILED` : 'all passed');
  process.exit(failed ? 1 : 0);
})().catch(e => { console.log('FAIL ' + e.message); process.exit(1); });
