// display.js -- the screen. The original picture arrives from the game worker
// as RGBA frames and is scaled onto #screen here; the enhanced picture is drawn
// by the render worker into #screen-hd, which is only told its size.
import { $, app } from './common.js';
import { pollInput } from './input.js';

const canvas = $('screen'), ctx2d = canvas.getContext('2d');
const frameCanvas = new OffscreenCanvas(320, 240), frameCtx = frameCanvas.getContext('2d');
let latest = null;                         // the newest frame not drawn yet
let frames = 0, lastFpsT = performance.now();
let hdSize = '';                           // the size last sent to the render worker

export function setHdSize(w, h) { hdSize = w + 'x' + h; }

// A frame from the game worker. Returns true for the game's first frame.
export function onFrame(m) {
  if (m.buf) latest = m;
  frames++;
  const first = m.seq === 1 || frames === 1;
  const t = performance.now();
  if (t - lastFpsT > 1000) { $('fps').textContent = `${Math.round(frames * 1000 / (t - lastFpsT))} fps`; frames = 0; lastFpsT = t; }
  return first;
}

function draw() {
  if (app.renderWorker) {                    // the enhanced picture is drawn at the box's real pixel size
    const r = $('screen-wrap').getBoundingClientRect();
    const w = Math.max(320, Math.round(r.width * devicePixelRatio)), h = Math.max(240, Math.round(r.height * devicePixelRatio));
    if (hdSize !== w + 'x' + h) { setHdSize(w, h); app.renderWorker.postMessage({ type: 'size', w, h }); }
  }
  if (latest && latest.buf) {
    const { w, h, buf } = latest; latest = null;
    const rect = canvas.getBoundingClientRect();
    const cw = Math.max(320, Math.round(rect.width * devicePixelRatio)), ch = Math.round(cw * 3 / 4);
    if (canvas.width !== cw || canvas.height !== ch) { canvas.width = cw; canvas.height = ch; }
    if (w && h) {
      if (frameCanvas.width !== w || frameCanvas.height !== h) { frameCanvas.width = w; frameCanvas.height = h; }
      frameCtx.putImageData(new ImageData(new Uint8ClampedArray(buf), w, h), 0, 0);
      ctx2d.imageSmoothingEnabled = $('smooth').checked;
      ctx2d.drawImage(frameCanvas, 0, 0, w, h, 0, 0, cw, ch);   // PS1 output is always 4:3
    } else {
      ctx2d.fillStyle = '#000'; ctx2d.fillRect(0, 0, cw, ch);
    }
  }
  pollInput();
  requestAnimationFrame(draw);
}

export function initDisplay() {
  requestAnimationFrame(draw);
  $('smooth').onchange = () => canvas.classList.toggle('smooth', $('smooth').checked);
  $('fullscreen').onclick = () => $('screen-wrap').requestFullscreen();
}
