// server/src/index.js -- the room server for online co-op (docs/coop/S6_network.md).
//
// A Cloudflare Worker that serves the website's files (site/, see
// wrangler.toml) and, at /api/ws, a WebSocket for rooms. It never carries
// the game itself: it introduces two browsers (WebRTC offer, answer and
// network candidates), which then talk directly.
//
// Rooms live in one Durable Object (Lobby). With the hibernation API it may
// be put to sleep between messages, so everything about a connection is kept
// in that connection's attachment and rebuilt from them when it wakes.
//
//   host                 a room: its code (to share), its listing id (for the
//                        public list), the host's name, public or private
//   joining              by code or from the list: the host is asked first
//                        (Accept / Decline), and the two browsers only start
//                        connecting (and so only learn each other's network
//                        address) once the host accepts
// One guest per room. A room disappears when its host leaves; a room whose
// game has started leaves the public list.
//
// Limits (security audit, M1 and M3): each connection may send LIMIT.rate
// messages a second (bursts up to LIMIT.burst) and try LIMIT.wrongCodes wrong
// codes a minute (also counted per network address); each network address may
// have LIMIT.perIp connections; a room has at most LIMIT.pending people
// waiting for the host's answer. Only the site's own pages may connect.
//
// Messages are JSON objects with a type `t`:
//   client -> server
//     {t:'host', name, public, version}       create a room
//     {t:'set', public}                       host: public or private
//     {t:'start'}                             host: the game started (off the list)
//     {t:'list'}                              the public rooms
//     {t:'join', code | id, name, version}    ask to join (by code, or a listing id)
//     {t:'answer', to, accept}                host: let a listed joiner in or not
//     {t:'signal', data}                      to the other player of the room
//   server -> client
//     {t:'room', code, id}                    your room
//     {t:'rooms', rooms: [{id, name, waiting}]}
//     {t:'request', from, name, via}          host: someone asks to join (via 'code' or 'list')
//     {t:'withdrawn', from}                   host: they stopped asking (left)
//     {t:'joined', name, peer}                both: the room has two players (peer = the other's name)
//     {t:'declined'} {t:'full'} {t:'unknown'} {t:'version', want}   join refused
//     {t:'busy'}                              join refused: too many people already waiting
//     {t:'limit', text}                       a limit was reached (the connection then closes)
//     {t:'signal', data}                      from the other player
//     {t:'left'}                              the other player left
//     {t:'error', text}

const CODE_LETTERS = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';   // no 0/O, 1/I (32: no bias below)
const MAX_MESSAGE = 64 * 1024;
const MAX_NAME = 24;
const MAX_FIELD = 64;                    // version, code, listing id
const LIMIT = { rate: 20, burst: 60, wrongCodes: 5, wrongWindow: 60000, perIp: 6, pending: 3 };

// a player's name, shown as plain text by the page: no control or invisible
// formatting characters (direction overrides, zero-width), spaces collapsed,
// at most MAX_NAME characters
function cleanName(s) {
  const t = String(s || '').normalize('NFC').replace(/[\p{Cc}\p{Cf}\p{Zl}\p{Zp}]/gu, '').replace(/\s+/gu, ' ').trim();
  return Array.from(t).slice(0, MAX_NAME).join('') || 'Pilot';
}
const field = v => typeof v === 'string' && v.length <= MAX_FIELD ? v : '';
// loopback (wrangler dev and its tests) is not limited per address
const limitedIp = ip => !!ip && ip !== '127.0.0.1' && ip !== '::1' && ip !== '::ffff:127.0.0.1';

export class Lobby {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
    // in memory only (reset if the object sleeps, which needs a quiet spell)
    this.buckets = new Map();            // connection id -> { tokens, at }
    this.wrong = new Map();              // connection id or 'ip:' + address -> [times of wrong codes]
  }

  async fetch(request) {
    if (request.headers.get('Upgrade') !== 'websocket') return new Response('WebSocket expected', { status: 426 });
    const ip = request.headers.get('CF-Connecting-IP') || '';
    const pair = new WebSocketPair();
    // too many from one address: refused at its first message, so the page can say why
    const over = limitedIp(ip) && this.sockets().filter(ws => this.info(ws).ip === ip).length >= LIMIT.perIp;
    this.ctx.acceptWebSocket(pair[1]);
    pair[1].serializeAttachment({ id: crypto.randomUUID(), ip, ...(over ? { over: true } : {}) });
    return new Response(null, { status: 101, webSocket: pair[0] });
  }
  refuse(ws, text) {
    this.send(ws, { t: 'limit', text });
    try { ws.close(1008, 'limit'); } catch { /* gone */ }
    this.leave(ws);
  }
  // token bucket: LIMIT.rate messages a second, bursts up to LIMIT.burst
  allow(id) {
    const now = Date.now(), b = this.buckets.get(id) || { tokens: LIMIT.burst, at: now };
    b.tokens = Math.min(LIMIT.burst, b.tokens + (now - b.at) * LIMIT.rate / 1000); b.at = now;
    if (b.tokens < 1) { this.buckets.set(id, b); return false; }
    b.tokens -= 1; this.buckets.set(id, b);
    return true;
  }
  // a wrong code: false once the connection, or its network address, has had too many this minute
  wrongCode(me) {
    const now = Date.now(), keys = [me.id, ...(limitedIp(me.ip) ? ['ip:' + me.ip] : [])];
    if (this.wrong.size > 10000)                // forget addresses that have been quiet for a minute
      for (const [k, v] of this.wrong) if (!v.length || now - v[v.length - 1] >= LIMIT.wrongWindow) this.wrong.delete(k);
    let ok = true;
    for (const k of keys) {
      const list = (this.wrong.get(k) || []).filter(t => now - t < LIMIT.wrongWindow);
      list.push(now); this.wrong.set(k, list);
      if (list.length > LIMIT.wrongCodes) ok = false;
    }
    return ok;
  }

  // ---- everyone's state, from the connections' attachments
  sockets() { return this.ctx.getWebSockets(); }
  info(ws) { return ws.deserializeAttachment() || {}; }
  save(ws, info) { ws.serializeAttachment(info); }
  byId(id) { return this.sockets().find(ws => this.info(ws).id === id); }
  host(match) { return this.sockets().find(ws => { const i = this.info(ws); return i.role === 'host' && match(i); }); }
  send(ws, msg) { try { ws.send(JSON.stringify(msg)); } catch { /* gone */ } }
  other(ws) {                              // the other player of ws's room
    const i = this.info(ws);
    if (i.role === 'host') return i.guest ? this.byId(i.guest) : null;
    if (i.role === 'guest') return this.host(h => h.code === i.code) || null;
    return null;
  }
  newCode() {
    const used = new Set(this.sockets().map(ws => this.info(ws).code).filter(Boolean));
    for (;;) {
      let c = 'NEMO-';
      for (const b of crypto.getRandomValues(new Uint8Array(4))) c += CODE_LETTERS[b & 31];
      if (!used.has(c)) return c;
    }
  }

  async webSocketMessage(ws, raw) {
    const me = this.info(ws);
    if (me.over) return this.refuse(ws, 'Too many connections from your network. Close other tabs of this site and try again.');
    if (!this.allow(me.id)) return this.refuse(ws, 'Too many messages: the connection to the room server was closed.');
    if (typeof raw !== 'string' || raw.length > MAX_MESSAGE) return;
    let m;
    try { m = JSON.parse(raw); } catch { return; }
    if (!m || typeof m !== 'object') return;
    const name = cleanName;
    switch (m.t) {
    case 'host': {
      if (me.role) return this.send(ws, { t: 'error', text: 'already in a room' });
      Object.assign(me, { role: 'host', code: this.newCode(), listing: crypto.randomUUID().slice(0, 8),
                          name: name(m.name), public: m.public === true, version: field(m.version), started: false, guest: null });
      this.save(ws, me);
      return this.send(ws, { t: 'room', code: me.code, id: me.listing });
    }
    case 'set':
      if (me.role !== 'host') return;
      me.public = m.public === true; this.save(ws, me);
      return;
    case 'start':
      if (me.role !== 'host') return;
      me.started = true; this.save(ws, me);
      return;
    case 'list': {
      const rooms = this.sockets().map(s => this.info(s))
        .filter(i => i.role === 'host' && i.public && !i.started && !i.guest)
        .map(i => ({ id: i.listing, name: i.name, version: i.version }));
      return this.send(ws, { t: 'rooms', rooms });
    }
    case 'join': {
      // one room, and one request, at a time
      if (me.role) return this.send(ws, { t: 'error', text: me.role === 'asking' ? 'already asking to join' : 'already in a room' });
      const code = field(m.code).toUpperCase().trim(), id = field(m.id);
      const h = code ? this.host(i => i.code === code || i.code === 'NEMO-' + code)
                     : id ? this.host(i => i.listing === id && i.public) : null;
      if (!h) {
        if (code && !this.wrongCode(me)) return this.refuse(ws, 'Too many wrong room codes. Wait a minute and try again.');
        return this.send(ws, { t: 'unknown' });
      }
      const hi = this.info(h);
      if (hi.guest || hi.started) return this.send(ws, { t: 'full' });
      if (field(m.version) !== hi.version) return this.send(ws, { t: 'version', want: hi.version });
      const waiting = this.sockets().filter(s => { const i = this.info(s); return i.role === 'asking' && i.code === hi.code; }).length;
      if (waiting >= LIMIT.pending) return this.send(ws, { t: 'busy' });
      // by code or from the list, the host decides (the browsers connect only after that)
      Object.assign(me, { role: 'asking', code: hi.code, name: name(m.name) });
      this.save(ws, me);
      return this.send(h, { t: 'request', from: me.id, name: me.name, via: code ? 'code' : 'list' });
    }
    case 'answer': {
      if (me.role !== 'host') return;
      const g = this.byId(field(m.to));
      if (!g) return;
      const gi = this.info(g);
      if (gi.role !== 'asking' || gi.code !== me.code) return;
      if (m.accept && !me.guest && !me.started) return this.admit(ws, g);
      Object.assign(gi, { role: null, code: null }); this.save(g, gi);
      return this.send(g, { t: m.accept ? 'full' : 'declined' });
    }
    case 'signal': {
      const o = this.other(ws);
      if (o) this.send(o, { t: 'signal', data: m.data });
      return;
    }
    }
  }

  admit(h, g) {
    const hi = this.info(h), gi = this.info(g);
    hi.guest = gi.id; this.save(h, hi);
    gi.role = 'guest'; this.save(g, gi);
    this.send(h, { t: 'joined', name: hi.name, peer: gi.name });
    this.send(g, { t: 'joined', name: gi.name, peer: hi.name });
  }

  async webSocketClose(ws) { this.leave(ws); }
  async webSocketError(ws) { this.leave(ws); }
  leave(ws) {
    const me = this.info(ws), o = this.other(ws);
    this.buckets.delete(me.id); this.wrong.delete(me.id);
    if (me.role === 'asking') {                  // stopped waiting: the host's request goes away
      const h = this.host(i => i.code === me.code);
      if (h) this.send(h, { t: 'withdrawn', from: me.id });
    } else if (me.role === 'host') {
      for (const s of this.sockets()) {          // everyone waiting on this room
        const i = this.info(s);
        if (s !== ws && i.code === me.code && (i.role === 'guest' || i.role === 'asking')) {
          this.send(s, { t: 'left' }); Object.assign(i, { role: null, code: null }); this.save(s, i);
        }
      }
    } else if (me.role === 'guest' && o) {
      const hi = this.info(o); hi.guest = null; this.save(o, hi);
      this.send(o, { t: 'left' });
    }
    this.save(ws, { id: me.id, ip: me.ip });
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/api/ws') {
      // only this site's own pages (browsers always send Origin; other tools may not)
      const origin = request.headers.get('Origin');
      if (origin && origin !== url.origin) return new Response('forbidden', { status: 403 });
      // one lobby for everyone (rooms are few and small)
      return env.LOBBY.get(env.LOBBY.idFromName('lobby')).fetch(request);
    }
    if (url.pathname.startsWith('/api/')) return new Response('not found', { status: 404 });
    return env.ASSETS.fetch(request);
  },
};
