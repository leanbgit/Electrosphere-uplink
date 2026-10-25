// coi-sw.js -- makes the page cross-origin isolated on hosts that cannot send
// the COOP/COEP headers themselves (the game needs SharedArrayBuffer).
// It re-serves every same-origin response with those headers added.
// Hosts that do send them (see _headers) never activate this.
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));
self.addEventListener('fetch', e => {
  const req = e.request;
  if (req.cache === 'only-if-cached' && req.mode !== 'same-origin') return;
  e.respondWith(fetch(req).then(res => {
    if (res.status === 0) return res;                       // opaque: leave alone
    const h = new Headers(res.headers);
    h.set('Cross-Origin-Embedder-Policy', 'require-corp');
    h.set('Cross-Origin-Opener-Policy', 'same-origin');
    h.set('Cross-Origin-Resource-Policy', 'cross-origin');
    return new Response(res.body, { status: res.status, statusText: res.statusText, headers: h });
  }).catch(err => new Response('Offline: ' + err, { status: 503 })));
});
