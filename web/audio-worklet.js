// audio-worklet.js -- plays the SPU output that the game worker writes into a
// shared ring buffer: Int32 [read, write] header, then interleaved s16 stereo.
class AC3Audio extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const sab = options.processorOptions.ring;
    this.head = new Int32Array(sab, 0, 2);
    this.data = new Int16Array(sab, 8);
    this.frames = this.data.length >> 1;
    this.volume = 1;
    this.port.onmessage = e => { if (e.data.volume !== undefined) this.volume = e.data.volume; };
  }
  process(inputs, outputs) {
    const L = outputs[0][0], R = outputs[0][1] || outputs[0][0];
    let r = Atomics.load(this.head, 0);
    const w = Atomics.load(this.head, 1);
    const k = this.volume / 32768;
    for (let i = 0; i < L.length; i++) {
      if (r === w) { L[i] = 0; R[i] = 0; continue; }   // underrun: silence
      L[i] = this.data[r * 2] * k;
      R[i] = this.data[r * 2 + 1] * k;
      r = (r + 1) % this.frames;
    }
    Atomics.store(this.head, 0, r);
    return true;
  }
}
registerProcessor('ac3-audio', AC3Audio);
