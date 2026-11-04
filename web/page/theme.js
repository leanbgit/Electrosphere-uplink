// theme.js -- the Auto / Light / Dark switch in the sky band. Auto follows the
// system; Light and Dark set data-theme on the page (style.css has both) and
// are remembered in this browser (coi-boot.js applies them before the page is
// drawn).
export function initTheme() {
  const buttons = [...document.querySelectorAll('.theme-btn')];
  const show = t => { for (const b of buttons) b.setAttribute('aria-pressed', String(b.dataset.set === t)); };
  let current = 'auto';
  try { const t = localStorage.getItem('ac3.theme'); if (t === 'light' || t === 'dark') current = t; } catch {}
  show(current);
  for (const b of buttons) b.onclick = () => {
    const t = b.dataset.set;
    if (t === 'auto') delete document.documentElement.dataset.theme;
    else document.documentElement.dataset.theme = t;
    try { if (t === 'auto') localStorage.removeItem('ac3.theme'); else localStorage.setItem('ac3.theme', t); } catch {}
    show(t);
  };
}
