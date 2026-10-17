// seq-worklet.js - the "Gen1 OS" audio thread: 8 synth tracks (wasm Session), a sample-accurate
// step sequencer with per-track looping patterns (polymeter allowed), live input + record capture,
// a metronome, and the RK3288 budget simulator (dropped blocks when the projected load is over).

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

const STEPS_PER_BAR = 16;

class SeqProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const { module, sampleRate: sr, tracks } = options.processorOptions;
    this.sr = sr;
    this.nTracks = tracks || 8;
    this.ex = instantiate(module).exports;
    this.ex.ab_session_init(sr, 128, this.nTracks);
    this.outL = new Float32Array(this.ex.memory.buffer, this.ex.ab_out_ptr(0), this.ex.ab_out_capacity());
    this.outR = new Float32Array(this.ex.memory.buffer, this.ex.ab_out_ptr(1), this.ex.ab_out_capacity());
    this.bpm = 120;
    this.playing = false;
    this.recording = false;
    this.metronome = false;
    this.pos = 0;                 // transport position in samples (double)
    this.step = 0;                // global step counter
    this.nextStepAt = 0;          // sample position of the next step
    this.patterns = [];           // per track: { steps, notes: [{step, len, pitch, vel}] }
    for (let t = 0; t < this.nTracks; t++) this.patterns.push({ steps: STEPS_PER_BAR, notes: [] });
    this.pendingOffs = [];        // { at, track, pitch }
    this.liveHeld = new Map();    // key track:pitch -> { startPos, vel }
    this.costs = [];              // per track { fixed, perVoice } in fractions of this device's core
    this.factor = 20; this.simulate = true; this.budget = 0.70; this.cores = 3;
    this.dropouts = 0; this.load = 0; this.rngState = 0x1234567;
    this.click = { left: 0, phase: 0, freq: 1000 };
    this.statusCounter = 0;
    this.port.onmessage = (e) => this.handle(e.data);
  }

  samplesPerStep() { return this.sr * 60 / this.bpm / 4; }

  handle(m) {
    const ex = this.ex;
    switch (m.type) {
      case 'transport':
        if (m.bpm) this.bpm = m.bpm;
        if (m.playing != null && m.playing !== this.playing) {
          this.playing = m.playing;
          if (this.playing) { this.pos = 0; this.step = 0; this.nextStepAt = 0; }
          else { this.pendingOffs = []; this.liveHeld.clear(); ex.ab_session_panic(); }
        }
        if (m.record != null) this.recording = m.record;
        if (m.metronome != null) this.metronome = m.metronome;
        break;
      case 'pattern': this.patterns[m.track] = { steps: Math.max(1, m.steps | 0), notes: m.notes || [] }; break;
      case 'param':   ex.ab_session_set_param(m.track, m.id, m.value); break;
      case 'params':  for (const [id, v] of m.list) ex.ab_session_set_param(m.track, id, v); break;
      case 'mix':     ex.ab_session_mix(m.track, m.gain, m.pan, m.mute ? 1 : 0, m.solo ? 1 : 0); break;
      case 'master':  ex.ab_session_master(m.gain); break;
      case 'profile': ex.ab_session_set_profile(m.track, m.profile); break;
      case 'constraint': ex.ab_session_set_constraint(m.track, m.c, m.on ? 1 : 0); break;
      case 'noteOn': {
        ex.ab_session_note_on(m.track, m.pitch, m.vel);
        if (this.playing && this.recording) this.liveHeld.set(m.track + ':' + m.pitch, { startPos: this.pos, vel: m.vel });
        break;
      }
      case 'noteOff': {
        ex.ab_session_note_off(m.track, m.pitch);
        const key = m.track + ':' + m.pitch;
        const h = this.liveHeld.get(key);
        if (h) {
          this.liveHeld.delete(key);
          const sps = this.samplesPerStep();
          const steps = this.patterns[m.track].steps;
          const stepAbs = Math.round(h.startPos / sps);
          const len = Math.max(1, Math.round((this.pos - h.startPos) / sps));
          this.port.postMessage({ type: 'recorded', track: m.track, note: { step: ((stepAbs % steps) + steps) % steps, len, pitch: m.pitch, vel: h.vel } });
        }
        break;
      }
      case 'allOff': ex.ab_session_all_off(m.track); break;
      case 'panic':  ex.ab_session_panic(); this.pendingOffs = []; this.liveHeld.clear(); break;
      case 'costs':  this.costs[m.track] = { fixed: m.fixed, perVoice: m.perVoice }; break;
      case 'budget': if (m.factor) this.factor = m.factor; if (m.simulate != null) this.simulate = m.simulate; if (m.cores) this.cores = m.cores; if (m.budget) this.budget = m.budget; break;
      case 'resetStats': this.dropouts = 0; for (let t = 0; t < this.nTracks; t++) ex.ab_session_reset_steals(t); break;
    }
  }

  fireStep() {
    const ex = this.ex;
    const sps = this.samplesPerStep();
    for (let t = 0; t < this.nTracks; t++) {
      const p = this.patterns[t];
      const s = this.step % p.steps;
      for (const n of p.notes) {
        if (n.step !== s) continue;
        ex.ab_session_note_on(t, n.pitch, n.vel);
        this.pendingOffs.push({ at: this.nextStepAt + Math.max(1, n.len) * sps - 2, track: t, pitch: n.pitch });
      }
    }
    if (this.metronome && this.step % 4 === 0) { this.click.left = Math.round(this.sr * 0.03); this.click.phase = 0; this.click.freq = (this.step % STEPS_PER_BAR === 0) ? 1600 : 1000; }
    this.step++;
    this.nextStepAt += sps;
  }

  rand() { let s = this.rngState; s ^= s << 13; s ^= s >>> 17; s ^= s << 5; this.rngState = s >>> 0; return (this.rngState >>> 8) / 16777216; }

  process(inputs, outputs) {
    const out = outputs[0];
    const n = out[0].length;
    const ex = this.ex;
    let done = 0;
    while (done < n) {
      let next = n;
      if (this.playing) {
        // sub-block boundaries: next step, earliest pending note-off
        const blockEnd = this.pos + (n - done);
        while (this.nextStepAt <= this.pos) this.fireStep();
        if (this.nextStepAt < blockEnd) next = Math.min(next, done + Math.ceil(this.nextStepAt - this.pos));
        for (let i = 0; i < this.pendingOffs.length; i++) {
          const o = this.pendingOffs[i];
          if (o.at <= this.pos) { ex.ab_session_note_off(o.track, o.pitch); this.pendingOffs.splice(i, 1); i--; }
          else if (o.at < blockEnd) next = Math.min(next, done + Math.ceil(o.at - this.pos));
        }
      }
      const k = Math.max(1, next - done);
      ex.ab_session_process(k);
      out[0].set(this.outL.subarray(0, k), done);
      if (out[1]) out[1].set(this.outR.subarray(0, k), done);
      done += k;
      if (this.playing) this.pos += k;
    }
    // metronome click (synthesized, not part of the Gen1 budget)
    if (this.click.left > 0) {
      for (let i = 0; i < n && this.click.left > 0; i++, this.click.left--) {
        const env = this.click.left / (this.sr * 0.03);
        const v = Math.sin(this.click.phase) * 0.25 * env;
        this.click.phase += 2 * Math.PI * this.click.freq / this.sr;
        out[0][i] += v; if (out[1]) out[1][i] += v;
      }
    }
    // ---- RK3288 budget simulator
    let est = 0;
    for (let t = 0; t < this.nTracks; t++) {
      const c = this.costs[t]; if (!c) continue;
      est += c.fixed + c.perVoice * ex.ab_session_voices(t);
    }
    this.load = est * this.factor / this.cores / this.budget;      // 1.0 = exactly at the 70 % budget
    if (this.simulate && this.load > 1 && this.rand() < 1 - 1 / this.load) {
      out[0].fill(0); if (out[1]) out[1].fill(0);                  // the callback missed its deadline
      this.dropouts++;
    }
    if (++this.statusCounter >= 8) {
      this.statusCounter = 0;
      const voices = [], peaks = [], steals = [];
      for (let t = 0; t < this.nTracks; t++) { voices.push(ex.ab_session_voices(t)); peaks.push(ex.ab_session_peak(t)); steals.push(ex.ab_session_steals(t)); }
      const sps = this.samplesPerStep();
      this.port.postMessage({ type: 'status', playing: this.playing, step: this.playing ? Math.floor(this.pos / sps) : 0, voices, peaks, steals, load: this.load, dropouts: this.dropouts, est });
    }
    return true;
  }
}

registerProcessor('absynth-seq', SeqProcessor);
