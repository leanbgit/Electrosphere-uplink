// graphics.js -- the graphics settings.
//
// "Original" = the PS1 picture, converted in the worker and drawn here.
// "Enhanced" = renderer/hw at 2-6x the PS1 resolution: the game worker records
// the drawing, render-worker.js draws it with WebGPU straight into the
// #screen-hd canvas (handed over with transferControlToOffscreen).
//
// Widescreen and the world effects are one bit each (same bits as
// platform/web/web_platform.c, hd_apply_settings). They change live: the page
// writes them into a small shared buffer (app.hdSet) that the game reads
// every field.
import { $, app } from './common.js';

const gfxPref = { get() { try { return JSON.parse(localStorage.getItem('ac3.gfx')) || {}; } catch { return {}; } },
                  set(v) { try { localStorage.setItem('ac3.gfx', JSON.stringify(v)); } catch {} } };
const GFX_DEFAULT = 1 | 2 | 128 | 256 | 512;   // widescreen, effects, sky lighting, sun, original fog (atmosphere, far terrain, clouds and ocean off; bit 32, forests, was removed)
const GFX_VERSION = 3;          // 2: bit 512 (original fog) added, on for remembered settings too
                                // 3: atmosphere (64), far terrain (4) and clouds (16) off by default, also for remembered settings
const bitBoxes = [...document.querySelectorAll('input[data-bit]')];

export function gfxBits() { let b = 0; for (const x of bitBoxes) if (x.checked) b |= +x.dataset.bit; return b; }
function showGfxBits(b) { for (const x of bitBoxes) x.checked = !!(b & +x.dataset.bit); }

// the boxes that do nothing with the current choices are greyed out
function gfxEnable() {
  const sd = !self.navigator.gpu || $('gfx').value === '0', fx = !!(gfxBits() & 2), atmo = !!(gfxBits() & 64);
  for (const x of bitBoxes) {
    const bit = +x.dataset.bit;
    x.disabled = sd || (bit > 2 && !fx) || (bit === 4 && !atmo);
  }
  document.body.classList.toggle('sd', sd);
}

// passes changed boxes to a running game
function applyLive() {
  if (app.online) return;      // online co-op: both computers keep the host's settings for the session
  const b = gfxBits();
  if (app.hdSet) { Atomics.store(app.hdSet, 0, b); Atomics.add(app.hdSet, 1, 1); }
  if (document.body.classList.contains('hd')) {
    $('screen-wrap').classList.toggle('wide', !!(b & 1));
    $('ro-gfx').textContent = `enhanced ${$('ro-gfx').dataset.scale}×${b & 1 ? ', 16:9' : ''}`;
  }
}

export function initGraphics() {
  const g = gfxPref.get();
  $('gfx').value = self.navigator.gpu ? String(g.scale ?? 0) : '0';
  // remembered settings from older versions get the newer defaults (GFX_VERSION)
  showGfxBits(g.bits !== undefined ? (g.v >= GFX_VERSION ? g.bits : (g.v >= 2 ? g.bits : g.bits | 512) & ~(4 | 16 | 64))
                                    : (GFX_DEFAULT & ~(g.wide === false ? 1 : 0) | (g.ocean ? 8 : 0)));
  if (!self.navigator.gpu) {
    for (const o of $('gfx').options) if (o.value !== '0') o.disabled = true;
    $('gfxhint').textContent = 'Enhanced graphics need WebGPU (current Chrome or Edge, Safari 26, Firefox 141 on Windows).';
  }
  const save = () => {
    gfxPref.set({ scale: +$('gfx').value, bits: gfxBits(), v: GFX_VERSION });
    gfxEnable();
  };
  $('gfx').onchange = () => { save(); if (app.running) $('gfxhint').textContent = 'The new resolution applies the next time you press Play.'; };
  for (const x of bitBoxes) x.onchange = () => { save(); applyLive(); };
  $('gfx-defaults').onclick = () => { showGfxBits(GFX_DEFAULT); save(); applyLive(); };
  gfxEnable();
}
