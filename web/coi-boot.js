// coi-boot.js -- loaded first by index.html (a file, not an inline script, so the
// Content-Security-Policy needs no 'unsafe-inline').
'use strict';
  // The game needs a cross-origin isolated page (SharedArrayBuffer). Hosts that
  // cannot send the COOP/COEP headers get them from a tiny service worker.
  if (!window.crossOriginIsolated && window.isSecureContext && 'serviceWorker' in navigator) {
    navigator.serviceWorker.register('coi-sw.js').then(() => navigator.serviceWorker.ready).then(() => {
      if (!sessionStorage.getItem('coi-reloaded')) { sessionStorage.setItem('coi-reloaded', '1'); location.reload(); }
    }).catch(() => {});
  }
