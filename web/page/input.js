// input.js -- the controller: the keyboard (bindings changeable on the page)
// and standard game controllers. Each animation frame, pollInput writes the
// buttons and sticks into the controller shared with the game worker.
import { $, app } from './common.js';

const BTN = { SELECT: 1, L3: 2, R3: 4, START: 8, UP: 16, RIGHT: 32, DOWN: 64, LEFT: 128,
  L2: 256, R2: 512, L1: 1024, R1: 2048, TRIANGLE: 4096, CIRCLE: 8192, CROSS: 16384, SQUARE: 32768 };
// Keyboard bindings: one key per action, changeable on the page and kept in
// localStorage ('ac3.keys'). Actions are pad buttons, or the left stick's
// four directions (the game's flight stick).
const KEY_ACTIONS = [
  ['Flight stick', [['STICK_UP', 'Up', 'KeyW'], ['STICK_DOWN', 'Down', 'KeyS'], ['STICK_LEFT', 'Left', 'KeyA'], ['STICK_RIGHT', 'Right', 'KeyD']]],
  ['Directional pad', [['UP', 'Up', 'ArrowUp'], ['DOWN', 'Down', 'ArrowDown'], ['LEFT', 'Left', 'ArrowLeft'], ['RIGHT', 'Right', 'ArrowRight']]],
  ['Buttons', [['CROSS', 'Cross', 'KeyX'], ['CIRCLE', 'Circle', 'KeyC'], ['SQUARE', 'Square', 'KeyZ'], ['TRIANGLE', 'Triangle', 'KeyV']]],
  ['Shoulders', [['L1', 'L1', 'KeyQ'], ['R1', 'R1', 'KeyE'], ['L2', 'L2', 'Digit1'], ['R2', 'R2', 'Digit3']]],
  ['System', [['START', 'Start', 'Enter'], ['SELECT', 'Select', 'Backspace']]],
];
const KEY_DEFAULTS = Object.fromEntries(KEY_ACTIONS.flatMap(([, a]) => a.map(([id, , code]) => [id, code])));
let keyBind = { ...KEY_DEFAULTS };                       // action -> code
try {                                                    // only known actions, string codes
  const saved = JSON.parse(localStorage.getItem('ac3.keys')) || {};
  for (const id of Object.keys(KEY_DEFAULTS))
    if (Object.prototype.hasOwnProperty.call(saved, id) && typeof saved[id] === 'string' && saved[id].length < 40) keyBind[id] = saved[id];
} catch {}
let KEYS = Object.create(null);                          // code -> action
function rebuildKeys() { KEYS = Object.create(null); for (const [a, c] of Object.entries(keyBind)) if (c) KEYS[c] = a; }
rebuildKeys();
function keyName(code) {
  if (!code) return '—';
  const m = code.match(/^Key(.)$/) || code.match(/^Digit(.)$/);
  if (m) return m[1];
  const arrows = { ArrowUp: '↑', ArrowDown: '↓', ArrowLeft: '←', ArrowRight: '→' };
  if (arrows[code]) return arrows[code];
  if (code.startsWith('Numpad')) return 'Num ' + code.slice(6);
  const side = code.match(/^(Shift|Control|Alt|Meta)(Left|Right)$/);
  if (side) return `${side[2]} ${side[1] === 'Control' ? 'Ctrl' : side[1]}`;
  return { Space: 'Space', Backquote: '`', Minus: '-', Equal: '=', BracketLeft: '[', BracketRight: ']', Backslash: '\\',
           Semicolon: ';', Quote: "'", Comma: ',', Period: '.', Slash: '/' }[code] || code;
}
let listening = null;                                    // the action waiting for a new key
function renderKeys(flash) {
  const g = $('keygrid'); g.replaceChildren();
  for (const [title, acts] of KEY_ACTIONS) {
    const box = document.createElement('div'); box.className = 'keygroup';
    const h3 = document.createElement('h3'); h3.textContent = title; box.appendChild(h3);
    for (const [id, label] of acts) {
      const row = document.createElement('div'); row.className = 'keyrow';
      const b = document.createElement('button'); b.className = 'keycap' + (listening === id ? ' listening' : '') + (flash && flash.includes(id) ? ' flash' : '');
      b.textContent = listening === id ? 'press a key…' : keyName(keyBind[id]);
      b.setAttribute('aria-label', `${title} ${label}: ${keyName(keyBind[id])}. Click to change.`);
      b.onclick = () => { listening = listening === id ? null : id; renderKeys(); };
      const sp = document.createElement('span'); sp.textContent = label;
      row.append(sp, b); box.appendChild(row);
    }
    g.appendChild(box);
  }
}
function setKey(id, code) {
  const other = Object.keys(keyBind).find(a => a !== id && keyBind[a] === code);
  if (other) keyBind[other] = keyBind[id];                 // the key was in use: swap
  keyBind[id] = code;
  try { localStorage.setItem('ac3.keys', JSON.stringify(keyBind)); } catch {}
  rebuildKeys(); listening = null; renderKeys(other ? [id, other] : [id]);
}

const held = new Set();
let padName = 'keyboard', shownPad = '';           // the controller in use, and the one shown
const axis = v => Math.max(0, Math.min(255, Math.round((v + 1) * 127.5)));
const dz = v => Math.abs(v) < 0.1 ? 0 : v;
export function pollInput() {
  if (!app.padView) return;
  let b = 0, lx = 128, ly = 128, rx = 128, ry = 128, analog = 0;
  for (const k of held) {
    const a = KEYS[k];
    if (!a) continue;
    if (a === 'STICK_LEFT') lx = 0; else if (a === 'STICK_RIGHT') lx = 255;
    else if (a === 'STICK_UP') ly = 0; else if (a === 'STICK_DOWN') ly = 255;
    else b |= BTN[a];
  }
  for (const gp of navigator.getGamepads ? navigator.getGamepads() : []) {
    if (!gp || gp.mapping !== 'standard') continue;
    analog = 1;
    const p = i => gp.buttons[i] && gp.buttons[i].pressed;
    const map = [[0, 'CROSS'], [1, 'CIRCLE'], [2, 'SQUARE'], [3, 'TRIANGLE'], [4, 'L1'], [5, 'R1'], [6, 'L2'], [7, 'R2'],
      [8, 'SELECT'], [9, 'START'], [10, 'L3'], [11, 'R3'], [12, 'UP'], [13, 'DOWN'], [14, 'LEFT'], [15, 'RIGHT']];
    for (const [i, n] of map) if (p(i)) b |= BTN[n];
    lx = axis(dz(gp.axes[0])); ly = axis(dz(gp.axes[1])); rx = axis(dz(gp.axes[2])); ry = axis(dz(gp.axes[3]));
    padName = gp.id.replace(/\s*\(.*$/, '').slice(0, 28) || 'gamepad';
    break;
  }
  if (!analog) padName = 'keyboard';
  if (padName !== shownPad) { shownPad = padName; $('ro-pad').textContent = padName; }
  const padView = app.padView;
  padView[0] = 1; padView[1] = analog; padView[2] = b;
  padView[3] = lx; padView[4] = ly; padView[5] = rx; padView[6] = ry;
}

export function initInput() {
  $('keys-reset').onclick = () => {
    keyBind = { ...KEY_DEFAULTS };
    try { localStorage.removeItem('ac3.keys'); } catch {}
    rebuildKeys(); listening = null; renderKeys(Object.keys(KEY_DEFAULTS));
  };
  renderKeys();
  addEventListener('keydown', e => {
    if (listening) {                                       // choosing a key: it does not reach the game
      e.preventDefault(); e.stopPropagation();
      if (e.code === 'Escape') { listening = null; renderKeys(); } else setKey(listening, e.code);
      return;
    }
    if (app.running && KEYS[e.code]) e.preventDefault();
    held.add(e.code);
  }, true);
  addEventListener('keyup', e => held.delete(e.code));
  addEventListener('blur', () => held.clear());
}
