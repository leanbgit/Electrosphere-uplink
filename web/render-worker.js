// render-worker.js -- draws the enhanced picture (render.wasm, WebGPU).
//
// The game worker records each field's drawing and sends it here as a packet
// (renderer/webgpu/hw_packet.h). This worker owns the WebGPU device and the
// visible canvas (handed over by the page with transferControlToOffscreen).
// Unlike the game worker it always returns to its event loop, so:
//   - the browser shows each finished canvas frame by itself,
//   - it learns when the GPU has finished a field and tells the game through
//     the shared counter `done` (the game waits when it is 2 fields ahead),
//   - GPU errors and a lost device are reported to the page.
'use strict';

let mod = null, dev = null, port = null, done = null, busy = false;
const queue = [];                    // packets waiting (never dropped: they carry VRAM uploads)
const shots = [];                     // development: canvas snapshots requested by tests
self.RW = { ctx: null, format: 'bgra8unorm' };

function log(text) { postMessage({ type: 'log', text }); }

onmessage = async (e) => {
  const m = e.data;
  if (m.type === 'size') {
    const c = self.RW.ctx && self.RW.ctx.canvas;
    if (c && m.w >= 64 && m.h >= 48 && (c.width !== m.w || c.height !== m.h)) { c.width = m.w; c.height = m.h; }
  } else if (m.type === 'shot') {
    shots.push(m.id);
  } else if (m.type === 'init') {
    try {
      done = new Int32Array(m.done);
      if (!self.navigator.gpu) throw new Error('this browser has no WebGPU');
      const ad = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
      if (!ad) throw new Error('no graphics adapter available to WebGPU');
      const feats = ['float32-filterable'].filter(f => ad.features.has(f));
      dev = await ad.requestDevice({ requiredFeatures: feats,
        requiredLimits: { maxTextureDimension2D: ad.limits.maxTextureDimension2D } });
      let errors = 0;
      dev.addEventListener('uncapturederror', ev => { if (errors++ < 10) log('WebGPU error: ' + ev.error.message); });
      dev.lost.then(i => postMessage({ type: 'lost', text: 'The graphics device was lost: ' + (i.message || i.reason) }));
      self.RW.ctx = m.canvas.getContext('webgpu');
      self.RW.format = navigator.gpu.getPreferredCanvasFormat();
      self.RW.ctx.configure({ device: dev, format: self.RW.format, alphaMode: 'opaque' });
      importScripts('render.js');
      mod = await createRender({ preinitializedWebGPUDevice: dev, print: log, printErr: log });
      const scale = mod._render_init(m.scale);
      if (!scale) throw new Error('the enhanced renderer could not be set up');
      const info = ad.info || {};
      log(`WebGPU: ${[info.vendor, info.architecture, info.description].filter(Boolean).join(' ') || 'adapter ready'}` +
          `, max texture ${ad.limits.maxTextureDimension2D}${feats.length ? ', ' + feats.join(' ') : ''}`);
      port = m.port;
      port.onmessage = ev => { queue.push(ev.data); if (!busy) drawQueued(); };
      postMessage({ type: 'ready', scale });
    } catch (err) {
      postMessage({ type: 'failed', text: String(err && err.message || err) });
    }
  }
};

// Draw the oldest waiting packet. The game stays at most two fields ahead, so
// the queue stays short.
function drawQueued() {
  const buf = queue.shift();
  if (!buf) return;
  busy = true;
  const bytes = new Uint8Array(buf);
  const p = mod._render_buffer(bytes.length);
  mod.HEAPU8.set(bytes, p);
  const c = self.RW.ctx.canvas;
  const seq = mod._render_frame(bytes.length, c.width, c.height);
  if (shots.length) {
    const id = shots.shift();
    c.convertToBlob({ type: 'image/png' }).then(b => postMessage({ type: 'shot', id, seq, blob: b }));
  }
  dev.queue.onSubmittedWorkDone().then(() => {
    if (seq) { Atomics.store(done, 0, seq); Atomics.notify(done, 0); }
    busy = false;
    if (queue.length) drawQueued();
  });
}
