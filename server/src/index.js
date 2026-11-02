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
//   joining by code      accepted straight away (knowing the code is the host's
//                        permission)
//   joining from the list   the host is asked first
// One guest per room. A room disappears when its host leaves; a room whose
// game has started leaves the public list.
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
//     {t:'request', from, name}               host: someone from the list asks to join
//     {t:'joined', name, peer}                both: the room has two players (peer = the other's name)
//     {t:'declined'} {t:'full'} {t:'unknown'} {t:'version', want}   join refused
//     {t:'signal', data}                      from the other player
//     {t:'left'}                              the other player left
//     {t:'error', text}

const CODE_LETTERS = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';   // no 0/O, 1/I
const MAX_MESSAGE = 64 * 1024;
const MAX_NAME = 24;

export class Lobby {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
  }

  async fetch(request) {
    if (request.headers.get('Upgrade') !== 'websocket') return new Response('WebSocket expected', { status: 426 });
    const pair = new WebSocketPair();
    this.ctx.acceptWebSocket(pair[1]);
    pair[1].serializeAttachment({ id: crypto.randomUUID() });
    return new Response(null, { status: 101, webSocket: pair[0] });
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
      for (let k = 0; k < 4; k++) c += CODE_LETTERS[Math.floor(Math.random() * CODE_LETTERS.length)];
      if (!used.has(c)) return c;
    }
  }

  async webSocketMessage(ws, raw) {
    if (typeof raw !== 'string' || raw.length > MAX_MESSAGE) return;
    let m;
    try { m = JSON.parse(raw); } catch { return; }
    const me = this.info(ws);
    const name = s => String(s || 'Pilot').replace(/[\u0000-\u001f]/g, '').slice(0, MAX_NAME) || 'Pilot';
    switch (m.t) {
    case 'host': {
      if (me.role) return this.send(ws, { t: 'error', text: 'already in a room' });
      Object.assign(me, { role: 'host', code: this.newCode(), listing: crypto.randomUUID().slice(0, 8),
                          name: name(m.name), public: !!m.public, version: String(m.version || ''), started: false, guest: null });
      this.save(ws, me);
      return this.send(ws, { t: 'room', code: me.code, id: me.listing });
    }
    case 'set':
      if (me.role !== 'host') return;
      me.public = !!m.public; this.save(ws, me);
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
      if (me.role === 'host' || me.role === 'guest') return this.send(ws, { t: 'error', text: 'already in a room' });
      const code = String(m.code || '').toUpperCase().trim();
      const h = code ? this.host(i => i.code === code || i.code === 'NEMO-' + code)
                     : this.host(i => i.listing === m.id && i.public);
      if (!h) return this.send(ws, { t: 'unknown' });
      const hi = this.info(h);
      if (hi.guest || hi.started) return this.send(ws, { t: 'full' });
      if (String(m.version || '') !== hi.version) return this.send(ws, { t: 'version', want: hi.version });
      Object.assign(me, { role: 'asking', code: hi.code, name: name(m.name) });
      this.save(ws, me);
      if (code) return this.admit(h, ws);
      return this.send(h, { t: 'request', from: me.id, name: me.name });
    }
    case 'answer': {
      if (me.role !== 'host') return;
      const g = this.byId(String(m.to || ''));
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
    if (me.role === 'host') {
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
    this.save(ws, { id: me.id });
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === '/api/ws') {
      // one lobby for everyone (rooms are few and small)
      return env.LOBBY.get(env.LOBBY.idFromName('lobby')).fetch(request);
    }
    if (url.pathname.startsWith('/api/')) return new Response('not found', { status: 404 });
    return env.ASSETS.fetch(request);
  },
};
