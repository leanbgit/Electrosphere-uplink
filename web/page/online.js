// online.js -- online co-op on the main page (docs/coop/S6_network.md).
//
// 1. Rooms: the room server (server/, a WebSocket at /api/ws) gives the host a
//    code; a public room is also listed. Joining, with the code or from the
//    list, asks the host first: the browsers only connect (and so only learn
//    each other's network address) once the host accepts.
// 2. The two browsers then connect directly (WebRTC data channel; the room
//    server only passes the connection offers along). No relay yet: some
//    networks cannot connect directly, and the page says so.
// 3. They check they can play together (same site version, same game data, the
//    guest's browser can use the host's graphics), the host sends its memory
//    card and graphics settings, and both games start from power-on, in
//    lockstep (web/netplay.js). The host is controller 1, the guest
//    controller 2; the guest's game never saves to the guest's browser.
import { log, app } from './common.js';
import { cardGet } from './storage.js';
import { have } from './gamedata.js';
import { gfxBits } from './graphics.js';
import { game } from './game.js';
// NetRing (web/netplay.js, a plain script shared with the game worker) is a global.

const ICE = [{ urls: 'stun:stun.cloudflare.com:3478' }, { urls: 'stun:stun.l.google.com:19302' }];
const CONNECT_TIMEOUT = 20000;
const FIELD_MS = 1000 / 59.94;
const S = {
  ws: null, role: null, code: null, peer: '', pc: null, dc: null, pending: [],
  version: '', hello: null, setup: null, ready: false, rtt: [], inbound: null, started: false, timer: 0,
  setupOk: false,          // guest: the host's set-up was valid and we answered "ready"
  helloTimer: 0, repeats: 0, refusal: null, refused: false,   // the hello, repeated until answered (channel())
};
// Which set-up messages each side may receive (security audit, H1): the guest
// must never be able to give the host a memory card or settings, or start its game.
const FROM_HOST = new Set(['hello', 'setup', 'go', 'refuse', 'ping', 'pong', 'bye']);
const FROM_GUEST = new Set(['hello', 'ready', 'refuse', 'ping', 'pong', 'bye']);
const CARD_BYTES = 131072;
const GFX_SCALES = [0, 2, 3, 4, 6];       // the page's "Graphics" choices (index.html, #gfx)
const GFX_BITS = 1023;                    // the option bits (index.html, data-bit)
const DELAY_MIN = 3, DELAY_MAX = 15;      // delayFor()
// the host's set-up, checked before the guest uses it; returns the problem or ''
function badSetup(m) {
  const g = m.gfx;
  if (!g || typeof g !== 'object' || !GFX_SCALES.includes(g.scale) || !Number.isInteger(g.bits) || g.bits < 0 || (g.bits & ~GFX_BITS)) return 'graphics settings';
  if (m.card !== null) {
    if (typeof m.card !== 'string' || m.card.length > 4 * CARD_BYTES / 3 + 4) return 'memory card';
    let c; try { c = unb64(m.card); } catch { return 'memory card'; }
    if (c.length !== CARD_BYTES || c[0] !== 0x4D || c[1] !== 0x43) return 'memory card';
  }
  return '';
}
const goodDelay = d => Number.isInteger(d) && d >= DELAY_MIN && d <= DELAY_MAX;
const el = id => document.getElementById(id);
const hex = b => [...new Uint8Array(b)].map(x => x.toString(16).padStart(2, '0')).join('');
const b64 = u8 => { let s = ''; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode(...u8.subarray(i, i + 0x8000)); return btoa(s); };
const unb64 = s => Uint8Array.from(atob(s), c => c.charCodeAt(0));

// ---- the panel
const name = () => (el('on-name').value.trim() || 'Pilot').slice(0, 24);
try { el('on-name').value = localStorage.getItem('ac3.name') || ''; } catch {}
el('on-name').onchange = () => { try { localStorage.setItem('ac3.name', el('on-name').value.trim()); } catch {} };
function state(text, cls = '') { el('on-state').textContent = text; el('on-state').className = 'note ' + cls; }
function view(inRoom) {
  el('on-idle').hidden = inRoom; el('on-room').hidden = !inRoom;
  el('on-hostbits').hidden = S.role !== 'host';
  el('on-go').hidden = S.role !== 'host';
  el('on-go').disabled = !S.ready || S.started;
}
function needDiscs() {          // the game data, streamed from the site (gamedata.js)
  if (app.gameData) return false;
  state(app.gameDataState === 'loading' ? 'The game data is still loading: try again in a moment.' : 'The game data is not available on this site.', 'bad');
  return true;
}

// ---- the site version: both players must run exactly the same game files
async function version() {
  if (S.version) return S.version;
  const parts = [];
  for (const f of ['ac3.wasm', 'worker.js', 'netplay.js']) {
    const r = await fetch(f, { cache: 'no-cache' });
    parts.push(hex(await crypto.subtle.digest('SHA-256', await r.arrayBuffer())).slice(0, 10));
  }
  return (S.version = parts.join('-'));
}

// ---- the room server
function server() {
  return new Promise((res, rej) => {
    if (S.ws && S.ws.readyState === WebSocket.OPEN) return res(S.ws);
    const ws = new WebSocket(location.origin.replace(/^http/, 'ws') + '/api/ws');
    ws.onopen = () => { S.ws = ws; res(ws); };
    ws.onerror = () => rej(new Error('The room server is not reachable from this copy of the site.'));
    ws.onclose = () => { if (S.ws === ws) S.ws = null; };
    ws.onmessage = e => { try { fromServer(JSON.parse(e.data)); } catch (err) { log('online: ' + err, 'bad'); } };
  });
}
const toServer = m => S.ws && S.ws.readyState === WebSocket.OPEN && S.ws.send(JSON.stringify(m));

function fromServer(m) {
  switch (m.t) {
  case 'room':
    S.code = m.code;
    el('on-roomcode').textContent = m.code;
    state('Share the code with the other player. Waiting for them to join…');
    view(true);
    break;
  case 'rooms': {                                  // open rooms only: full or started ones are not listed
    const ul = el('on-rooms'); ul.replaceChildren();
    el('on-rooms-tools').hidden = false;
    el('on-rooms-time').textContent = `as of ${new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })}`;
    if (!m.rooms.length) { const li = document.createElement('li'); li.className = 'dim'; li.textContent = 'No open public rooms right now.'; ul.appendChild(li); }
    for (const r of m.rooms) {
      const li = document.createElement('li'), b = document.createElement('button');
      li.textContent = `${r.name}'s room `;
      b.className = 'quiet small'; b.textContent = 'Ask to join';
      b.disabled = r.version !== S.version;
      if (b.disabled) li.title = 'a different version of the site';
      b.onclick = () => join({ id: r.id });
      li.appendChild(b); ul.appendChild(li);
    }
    break;
  }
  case 'request': {                                // host: someone asks to join (with the code, or from the list)
    const li = document.createElement('li'), ok = document.createElement('button'), no = document.createElement('button');
    li.dataset.from = m.from;
    li.textContent = `${m.name} asks to join${m.via === 'code' ? ' (with your code)' : ''} `;
    ok.className = 'small'; ok.textContent = 'Accept'; no.className = 'quiet small'; no.textContent = 'Decline';
    ok.onclick = () => { toServer({ t: 'answer', to: m.from, accept: true }); li.remove(); };
    no.onclick = () => { toServer({ t: 'answer', to: m.from, accept: false }); li.remove(); };
    li.append(ok, no); el('on-requests').appendChild(li);
    break;
  }
  case 'joined':
    S.peer = m.peer;
    el('on-requests').replaceChildren();
    if (S.role === 'asking') S.role = 'guest';
    state(`${m.peer} is here. Connecting your browsers…`);
    view(true);
    if (S.role === 'host') connectHost();
    break;
  case 'withdrawn':                                // host: they stopped asking
    for (const li of el('on-requests').children) if (li.dataset.from === m.from) li.remove();
    break;
  case 'declined': state('The host declined.', 'bad'); leave(true); break;
  case 'busy': state('Too many people are already asking to join that room. Try again in a moment.', 'bad'); leave(true); break;
  case 'limit': state(String(m.text || 'The room server closed the connection.').slice(0, 200), 'bad'); leave(true); break;
  case 'full': state('That room already has two players.', 'bad'); leave(true); break;
  case 'unknown': state('No room with that code.', 'bad'); leave(true); break;
  case 'version': state('That room runs a different version of the site. Both players need to reload the page.', 'bad'); leave(true); break;
  case 'signal': signal(m.data); break;
  case 'left':
    if (S.started) break;                          // the game itself notices (data channel)
    if (S.role === 'host') { closePeer(); S.ready = false; state(`${S.peer} left. Waiting for another player…`); view(true); }
    else { state('The host closed the room.', 'bad'); leave(true); }
    break;
  case 'error': log('online: ' + m.text, 'bad'); break;
  }
}

async function create() {
  if (needDiscs()) return;
  try {
    await server();
    S.role = 'host';
    toServer({ t: 'host', name: name(), public: !el('on-private').checked, version: await version() });
  } catch (e) { state(e.message, 'bad'); }
}
async function join(how) {
  if (needDiscs()) return;
  try {
    await server();
    S.role = 'asking';
    state('Asking the host…');
    toServer({ t: 'join', ...how, name: name(), version: await version() });
    view(true);
  } catch (e) { state(e.message, 'bad'); }
}
async function browse() {
  try { await server(); await version(); toServer({ t: 'list' }); } catch (e) { state(e.message, 'bad'); }
}

// ---- the direct connection
function newPeer() {
  const pc = new RTCPeerConnection({ iceServers: ICE });
  pc.onicecandidate = e => { if (e.candidate) toServer({ t: 'signal', data: { cand: e.candidate.toJSON() } }); };
  pc.onconnectionstatechange = () => {
    if (pc.connectionState === 'failed' && !S.started)
      state('Your browsers could not connect directly. This happens on some networks (no relay yet); try another network.', 'bad');
  };
  clearTimeout(S.timer);
  S.timer = setTimeout(() => {
    if (!S.dc || S.dc.readyState !== 'open')
      state('Your browsers could not connect directly. This happens on some networks (no relay yet); try another network.', 'bad');
  }, CONNECT_TIMEOUT);
  return pc;
}
async function connectHost() {
  closePeer();
  S.pc = newPeer();
  channel(S.pc.createDataChannel('game', { ordered: true }));
  await S.pc.setLocalDescription(await S.pc.createOffer());
  toServer({ t: 'signal', data: { sdp: S.pc.localDescription.toJSON() } });
}
async function signal(d) {
  try {
    if (d.sdp && d.sdp.type === 'offer') {               // the guest's side
      closePeer();
      S.pc = newPeer();
      S.pc.ondatachannel = e => channel(e.channel);
      await S.pc.setRemoteDescription(d.sdp);
      await S.pc.setLocalDescription(await S.pc.createAnswer());
      toServer({ t: 'signal', data: { sdp: S.pc.localDescription.toJSON() } });
    } else if (d.sdp && S.pc) {
      await S.pc.setRemoteDescription(d.sdp);
    }
    if (d.cand) { if (S.pc && S.pc.remoteDescription) await S.pc.addIceCandidate(d.cand); else S.pending.push(d.cand); }
    if (S.pc && S.pc.remoteDescription) for (const c of S.pending.splice(0)) await S.pc.addIceCandidate(c);
  } catch (e) { log('online: connection setup: ' + e, 'bad'); }
}
function closePeer() {
  if (S.dc) { S.dc.onclose = null; S.dc.close(); }
  if (S.pc) S.pc.close();
  S.dc = S.pc = null; S.pending = []; S.ready = false; S.hello = null; S.setup = null; S.setupOk = false; S.rtt = [];
  S.refusal = null; S.refused = false; S.repeats = 0; clearInterval(S.helloTimer);
  clearTimeout(S.timer);
}

// ---- talking over the data channel: text = setting up, binary = the game
function channel(dc) {
  S.dc = dc;
  dc.binaryType = 'arraybuffer';
  dc.onopen = async () => {
    clearTimeout(S.timer);
    state(`Connected to ${S.peer}. Checking that you can play together…`);
    const hello = { t: 'hello', version: await version(), data: app.gameData ? app.gameData.version : null,
                    english: !!have.patch[1], gpu: !!navigator.gpu };
    send(hello);
    // The first message on a new channel is sometimes never delivered (seen in
    // Chrome when the guest's channel is already open as it appears): the guest
    // repeats its hello until the host answers; the host answers a repeat by
    // sending its set-up (or its refusal) again.
    if (S.role !== 'host') {
      clearInterval(S.helloTimer);
      S.helloTimer = setInterval(() => {
        if (S.dc !== dc || dc.readyState !== 'open' || S.setupOk || S.refused) return clearInterval(S.helloTimer);
        send(hello);
      }, 1500);
    }
    for (let i = 0; i < 5; i++) setTimeout(() => send({ t: 'ping', at: performance.now() }), 200 * i);
  };
  dc.onmessage = e => {
    if (typeof e.data !== 'string') { if (S.inbound) NetRing.write(S.inbound, e.data); return; }
    let m; try { m = JSON.parse(e.data); } catch { return; }
    if (!m || typeof m !== 'object' || !(S.role === 'host' ? FROM_GUEST : FROM_HOST).has(m.t)) return;
    control(m);
  };
  dc.onclose = () => {
    if (S.inbound) NetRing.write(S.inbound, new Int32Array([NET_BYE, 0, 0, 0]));   // the game stops waiting for them
    if (S.started) { state(`${S.peer} left the game.`, 'bad'); }
    else if (S.role === 'host') { state(`${S.peer} disconnected. Waiting for another player…`); S.ready = false; view(true); }
    else { state('The connection to the host was lost.', 'bad'); }
  };
}
const send = m => { if (S.dc && S.dc.readyState === 'open') S.dc.send(JSON.stringify(m)); };

async function control(m) {
  switch (m.t) {
  case 'ping': if (typeof m.at === 'number') send({ t: 'pong', at: m.at }); break;
  case 'pong': if (typeof m.at === 'number' && S.rtt.length < 20) S.rtt.push(performance.now() - m.at); break;
  case 'hello': {
    if (S.role !== 'host') { S.hello = m; break; }
    if (S.hello) {                                      // once per connection; a repeat: answer again (up to 10 times)
      if (S.started || ++S.repeats > 10) break;
      if (S.refusal) send(S.refusal); else if (S.setup) send(S.setup);
      break;
    }
    S.hello = m;
    const problem = m.version !== await version() ? 'a different version of the site'
      : m.data !== (app.gameData && app.gameData.version) ? 'different game data'
      : m.english !== !!have.patch[1] ? 'a different language' : '';
    if (problem) { S.refusal = { t: 'refuse', why: `Your copy has ${problem}.` }; send(S.refusal); state(`${S.peer} cannot play: ${problem}.`, 'bad'); break; }
    const card = await cardGet(0);
    S.setup = { t: 'setup', gfx: { scale: navigator.gpu ? +el('gfx').value : 0, bits: gfxBits() },
                card: card ? b64(card.data) : null };
    send(S.setup);
    break;
  }
  case 'setup': {                                       // the guest: can this browser use the host's settings?
    if (S.setup || S.started) {                           // once per connection (a repeat: our "ready" may be lost too)
      if (S.setupOk && !S.started) send({ t: 'ready' });
      break;
    }
    const bad = badSetup(m);
    if (bad) {
      send({ t: 'refuse', why: `Their browser could not use your ${bad}.` });
      state(`The host sent an invalid ${bad}. Ask them to reload the page and try again.`, 'bad');
      break;
    }
    S.setup = { t: 'setup', gfx: { scale: m.gfx.scale, bits: m.gfx.bits }, card: m.card };
    const g = S.setup.gfx;
    if (g.scale > 0 && (g.bits & (1 | 64)) && !navigator.gpu) {
      send({ t: 'refuse', why: 'Their browser cannot do Enhanced graphics, and your widescreen or atmosphere setting needs it.' });
      state('The host plays with Enhanced graphics and widescreen or atmosphere, which this browser cannot do (it needs WebGPU).', 'bad');
      break;
    }
    S.setupOk = true;
    send({ t: 'ready' });
    state(`Ready. Waiting for ${S.peer} to start the game…`, 'ok');
    break;
  }
  case 'ready':
    if (!S.setup) break;                                  // only after the host's set-up went out
    S.ready = true;
    state(`${S.peer} is ready. Press Start the game when you are.`, 'ok');
    view(true);
    break;
  case 'refuse': S.refused = true; state(String(m.why || 'The other player cannot play.').slice(0, 200), 'bad'); break;
  case 'go':                                              // the guest: the host pressed Start
    if (!S.setupOk || S.started) break;
    if (!goodDelay(m.delay)) { state('The host sent an invalid input delay. Ask them to reload the page and try again.', 'bad'); break; }
    start(m.delay);
    break;
  case 'bye': if (S.inbound) NetRing.write(S.inbound, new Int32Array([NET_BYE, 0, 0, 0])); state(`${S.peer} left the game.`, 'bad'); break;
  }
}

// ---- starting both games
function delayFor() {                                   // fields of input delay from the measured round trip
  const r = S.rtt.slice().sort((a, b) => a - b), rtt = r.length ? r[r.length >> 1] : 100;
  return Math.max(3, Math.min(15, Math.ceil((rtt / 2 + 10) / FIELD_MS) + 1));
}
async function go() {
  if (S.role !== 'host' || !S.ready || S.started || app.running) return;
  const delay = delayFor();
  S.inbound = NetRing.create();
  send({ t: 'go', delay });
  await start(delay);
}
async function start(delay) {
  if (S.started || app.running) return;
  if (!S.inbound) S.inbound = NetRing.create();
  S.started = true;
  view(true);
  const card = S.setup && S.setup.card ? unb64(S.setup.card) : null;
  if (S.role === 'host') toServer({ t: 'start' });
  state(`Playing with ${S.peer} (input delay ${delay} fields).`, 'ok');
  const ok = await game.start({
    net: { role: S.role, delay, inbound: S.inbound },
    card, gfx: S.setup ? S.setup.gfx : null,
    onMessage: m => {
      if (m.type === 'net-out') { if (S.dc && S.dc.readyState === 'open') S.dc.send(m.data); }
      else if (m.type === 'net-desync') { state(`The two games went out of step at field ${m.field}. Please stop and start again (and tell the developer).`, 'bad'); log(`online: desync at field ${m.field}`, 'bad'); }
      else if (m.type === 'net-wait') {
        state(m.seconds ? `Waiting for ${S.peer}… ${m.seconds} s` : `Playing with ${S.peer}.`, m.seconds ? 'bad' : 'ok');
        if (m.seconds) veil(`Waiting for ${S.peer}… ${m.seconds} s`, true); else phaseVeil(S.phase);
      }
      else if (m.type === 'coop-phase') phaseVeil(m.phase);
    },
    onStop: () => { send({ t: 'bye' }); leave(false); },
  });
  if (!ok) { send({ t: 'bye' }); leave(false); }
}

// ---- on the game screen: who has the controls (runtime/hle/coop.h, AC3_COOP_PHASE_*)
function veil(text, dim) {
  const v = el('coop-veil');
  v.hidden = !text;
  v.firstElementChild.textContent = text || '';
  el('screen-wrap').classList.toggle('veiled', !!(text && dim));
}
function phaseVeil(phase) {
  S.phase = phase;
  const other = S.peer || 'the other player';
  clearTimeout(S.veilTimer);
  if (S.role === 'host') {
    if (phase === 1) return veil(`${other} is choosing a plane and weapons`, true);
    if (phase === 5) return veil(`Shot down: watching ${other} fly on until the mission ends`, true);
    return veil('');
  }
  switch (phase) {
  case 0: return veil(`${other} has the controls`, true);                         // menus, briefings, the host's hangar turn
  case 1: return veil('Your turn: choose your plane and weapons', false);
  case 3: return veil(`You join when ${other} gets control of their plane`, true);
  case 4: return veil(`Shot down: watching ${other} until the mission ends`, true);
  case 5:                                                                           // the host was shot down: the guest flies on
    veil(`${other} was shot down. The mission is yours now!`, false);
    S.veilTimer = setTimeout(() => { if (S.phase === 5) veil(''); }, 5000);
    return;
  default: return veil('');
  }
}

function leave(keepMessage) {
  veil('');
  closePeer();
  if (S.ws) { S.ws.onclose = null; S.ws.close(); S.ws = null; }
  Object.assign(S, { role: null, code: null, peer: '', inbound: null, started: false, ready: false });
  el('on-requests').replaceChildren();
  view(false);
  if (!keepMessage) state('');
}

export function initOnline() {
  el('on-create').onclick = create;
  el('on-join').onclick = () => { const c = el('on-code').value.trim(); if (c) join({ code: c }); };
  el('on-code').onkeydown = e => { if (e.key === 'Enter') el('on-join').click(); };
  el('on-browse').onclick = browse;
  el('on-refresh').onclick = browse;
  el('on-private').onchange = () => { if (S.role === 'host') toServer({ t: 'set', public: !el('on-private').checked }); };
  el('on-go').onclick = go;
  el('on-leave').onclick = () => { if (S.started && app.running) game.stop(); leave(false); };
  el('on-copy').onclick = () => navigator.clipboard && navigator.clipboard.writeText(S.code || '');
  view(false);
}
