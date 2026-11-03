// tabs.js -- the two panels beside the screen (Settings, Play online) as tabs,
// and the "Play online with a friend" shortcut on the idle screen. The last
// tab shown is remembered in this browser.
import { $ } from './common.js';

export function initTabs() {
  const tabs = [...document.querySelectorAll('.tabs [role=tab]')];
  function show(tab, focus) {
    for (const t of tabs) {
      const on = t === tab;
      t.setAttribute('aria-selected', on);
      t.tabIndex = on ? 0 : -1;
      $(t.getAttribute('aria-controls')).hidden = !on;
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
  try { const last = $(localStorage.getItem('ac3.tab')); if (last && tabs.includes(last)) show(last); } catch {}

  $('to-online').addEventListener('click', () => {
    show($('tab-online'));
    document.querySelector('.console').scrollIntoView({ behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth', block: 'start' });
    $('on-name').focus({ preventScroll: true });
  });
}
