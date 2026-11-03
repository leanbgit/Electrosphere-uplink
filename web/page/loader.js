// loader.js -- the loading screen: a fighter seen from above that fills up
// from tail to nose (#loader in index.html). Each start-up stage moves the bar
// to its own range; within a stage it creeps forward, and the English build
// (first start only) reports real progress.
import { $ } from './common.js';

export const loader = (() => {
  const el = $('loader'), fill = $('ld-fill'), edge = $('ld-edge'), lit = $('ld-litrect');
  const NOSE = 6, TAIL = 284;                               // the jet's extent in the SVG (y, nose up)
  const still = matchMedia('(prefers-reduced-motion: reduce)');
  let v = 0, target = 0, ceil = 0, raf = 0, owner = null;
  function draw() {
    const y = TAIL - (TAIL - NOSE) * v / 100;
    fill.setAttribute('y', y.toFixed(2)); fill.setAttribute('height', (TAIL - y + 1).toFixed(2));
    edge.setAttribute('y', (y - 0.8).toFixed(2));
    edge.style.opacity = v > 0.5 && v < 99.5 ? 1 : 0;
    lit.setAttribute('y', y.toFixed(2)); lit.setAttribute('height', (330 - y).toFixed(2));
    $('ld-pct').textContent = Math.floor(v);
    el.setAttribute('aria-valuenow', Math.floor(v));
  }
  function tick() {
    raf = 0;
    if (el.hidden) return;
    if (target < ceil) target = Math.min(ceil, target + (ceil - target) * 0.006 + 0.004);   // creep
    v = still.matches ? target : v + (target - v) * 0.12;
    if (Math.abs(target - v) < 0.05) v = target;
    draw();
    raf = requestAnimationFrame(tick);
  }
  return {
    // who: 'game' or 'store' (remembering discs); the game wins
    show(who, stage, sub = '') {
      if (owner === 'game' && who !== 'game') return;
      if (owner !== who) { v = target = ceil = 0; draw(); }
      owner = who;
      el.hidden = false; el.classList.remove('gone');
      $('ld-stage').textContent = stage; $('ld-sub').textContent = sub;
      if (!raf) raf = requestAnimationFrame(tick);
    },
    // jump to at least `from` %, then creep towards `to` %
    set(who, from, to = from) {
      if (who !== owner) return;
      target = Math.max(target, from); ceil = Math.max(target, to);
    },
    hide(who) {
      if (who !== owner) return;
      owner = null;
      target = ceil = 100;
      setTimeout(() => {
        if (owner) return;
        el.classList.add('gone');
        setTimeout(() => { if (!owner) { el.hidden = true; v = target = ceil = 0; draw(); } }, still.matches ? 0 : 450);
      }, still.matches ? 0 : 250);
    },
    cancel() { owner = null; el.hidden = true; el.classList.remove('gone'); v = target = ceil = 0; draw(); },
  };
})();
// start-up stages (web/worker.js posts 'progress'): label and range in %
const STAGES = {
  prep:    ['Loading your files', 0, 8],
  check:   ['Checking the game disc', 8, 12],
  engine:  ['Starting the game engine', 12, 22],
  boot:    ['Booting', 22, 92],
};
export const buildingEnglish = [];            // discs whose English data must be built this start (game.js)

export function loaderStage(stage, disc, done, total) {
  if (stage === 'english') {
    // first start only: the English text is built from the discs and the NEMO patch
    const n = Math.max(1, buildingEnglish.length), i = Math.max(0, buildingEnglish.indexOf(disc));
    const lo = 24 + 68 * i / n, hi = 24 + 68 * (i + 1) / n, f = total ? done / total : 0;
    loader.show('game', `Building the English version, disc ${disc}`, 'First start only: later starts skip this.');
    loader.set('game', lo + (hi - lo) * f, lo + (hi - lo) * Math.min(1, f + 0.02));
    return;
  }
  const s = STAGES[stage]; if (!s) return;
  loader.show('game', s[0]);
  loader.set('game', s[1], stage === 'boot' && buildingEnglish.length ? 24 : s[2]);
}
