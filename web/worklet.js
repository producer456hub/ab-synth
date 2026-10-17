// worklet.js - AudioWorkletProcessor hosting the wasm Dual engine (A = Full, B = Gen1).
// The compiled WebAssembly.Module arrives via processorOptions; instantiation here is synchronous.
// Also runs the sample-accurate phrase player so both instances hear the identical performance.

function instantiate(module) {
  const stubs = {};
  for (const imp of WebAssembly.Module.imports(module)) {
    stubs[imp.module] = stubs[imp.module] || {};
    stubs[imp.module][imp.name] = () => 0;
  }
  const inst = new WebAssembly.Instance(module, stubs);
  if (inst.exports._initialize) inst.exports._initialize();
  return inst;
}

class ABSynthProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const { module, sampleRate: sr } = options.processorOptions;
    this.inst = instantiate(module);
    this.ex = this.inst.exports;
    this.ex.ab_init(sr, 128);
    this.sr = sr;
    this.outL = new Float32Array(this.ex.memory.buffer, this.ex.ab_out_ptr(0), this.ex.ab_out_capacity());
    this.outR = new Float32Array(this.ex.memory.buffer, this.ex.ab_out_ptr(1), this.ex.ab_out_capacity());
    this.phrase = null;      // { events: [{t, note, vel, on}], length (samples), loop }
    this.pos = 0;            // playhead in samples
    this.evIdx = 0;
    this.playing = false;
    this.statusCounter = 0;
    this.peakA = 0; this.peakB = 0;
    this.port.onmessage = (e) => this.handle(e.data);
  }

  handle(m) {
    const ex = this.ex;
    switch (m.type) {
      case 'noteOn':  ex.ab_note_on(m.note, m.vel); break;
      case 'noteOff': ex.ab_note_off(m.note); break;
      case 'allOff':  ex.ab_all_notes_off(); break;
      case 'panic':   ex.ab_panic(); this.playing = false; break;
      case 'param':   ex.ab_set_param(m.id, m.value); break;
      case 'params':  for (const [id, v] of m.list) ex.ab_set_param(id, v); break;
      case 'mix':     ex.ab_set_mix(m.value); break;
      case 'trim':    ex.ab_set_trim_db(m.inst, m.db); break;
      case 'profile': ex.ab_set_profile(m.inst, m.profile); break;
      case 'constraint': ex.ab_set_constraint(m.inst, m.c, m.on ? 1 : 0); break;
      case 'phrase':
        this.phrase = m.phrase; this.pos = 0; this.evIdx = 0; this.playing = true;
        ex.ab_all_notes_off();
        break;
      case 'stop':
        this.playing = false; ex.ab_all_notes_off();
        break;
    }
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const n = out[0].length;
    const ex = this.ex;
    let done = 0;
    while (done < n) {
      let next = n;
      if (this.playing && this.phrase) {
        const ev = this.phrase.events;
        while (this.evIdx < ev.length) {
          const e = ev[this.evIdx];
          if (e.t <= this.pos) { if (e.on) ex.ab_note_on(e.note, e.vel); else ex.ab_note_off(e.note); this.evIdx++; }
          else { next = Math.min(next, done + (e.t - this.pos)); break; }
        }
        if (this.evIdx >= ev.length && this.pos >= this.phrase.length) {
          if (this.phrase.loop) { this.pos = 0; this.evIdx = 0; continue; }
          this.playing = false; this.port.postMessage({ type: 'phraseEnd' });
        }
      }
      const k = next - done;
      ex.ab_process(k);
      out[0].set(this.outL.subarray(0, k), done);
      if (out[1]) out[1].set(this.outR.subarray(0, k), done);
      done += k;
      if (this.playing) this.pos += k;
    }
    if (++this.statusCounter >= 12) {            // ~32 ms at 128 frames / 48 kHz
      this.statusCounter = 0;
      let peak = 0;
      for (let i = 0; i < n; i++) { const a = Math.abs(out[0][i]); if (a > peak) peak = a; }
      this.port.postMessage({
        type: 'status',
        voicesA: ex.ab_active_voices(0), voicesB: ex.ab_active_voices(1),
        maxA: ex.ab_max_voices(0), maxB: ex.ab_max_voices(1),
        peak, pos: this.pos, playing: this.playing,
        length: this.phrase ? this.phrase.length : 0,
      });
    }
    return true;
  }
}

registerProcessor('absynth', ABSynthProcessor);
