// proto.js -- the design prototype's own bits (proto.html): the two panels
// beside the screen as tabs, and the "Play online" shortcut on the idle screen.
// Everything else is the site's own app.js / online.js, unchanged.
'use strict';
(() => {
  const tabs = [...document.querySelectorAll('.tabs [role=tab]')];
  function show(tab, focus) {
    for (const t of tabs) {
      const on = t === tab;
      t.setAttribute('aria-selected', on);
      t.tabIndex = on ? 0 : -1;
      document.getElementById(t.getAttribute('aria-controls')).hidden = !on;
    }
    if (focus) tab.focus();
    try { localStorage.setItem('ac3.tab', tab.id); } catch {}
  }
  for (const t of tabs) {
    t.addEventListener('click', () => show(t));
    t.addEventListener('keydown', e => {           // arrow keys move between tabs
      const i = tabs.indexOf(t), d = e.key === 'ArrowRight' ? 1 : e.key === 'ArrowLeft' ? -1 : 0;
      if (d) { e.preventDefault(); show(tabs[(i + d + tabs.length) % tabs.length], true); }
    });
  }
  try { const last = document.getElementById(localStorage.getItem('ac3.tab')); if (last && tabs.includes(last)) show(last); } catch {}

  document.getElementById('to-online').addEventListener('click', () => {
    const t = document.getElementById('tab-online');
    show(t);
    document.querySelector('.console').scrollIntoView({ behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth', block: 'start' });
    document.getElementById('on-name').focus({ preventScroll: true });
  });
})();
