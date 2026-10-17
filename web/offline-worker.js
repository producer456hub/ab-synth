// offline-worker.js - renders a patch + phrase through both engines OFF the audio thread.
// Used for loudness matching (RMS of A vs B -> trim) and as the Phase 2 benchmark harness
// (per-instance render time, never measured inside the AudioWorklet).
// Each profile gets its own wasm instance and is timed as ONE whole render: worker clocks are
// coarsened to ~100 us, so per-block timing would be meaningless.

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

function renderOne(module, inst, sampleRate, params, constraints, events, total) {
  const ex = instantiate(module).exports;
  const block = 256;
  ex.ab_init(sampleRate, block);
  const outL = new Float32Array(ex.memory.buffer, ex.ab_out_ptr(0), block);
  const outR = new Float32Array(ex.memory.buffer, ex.ab_out_ptr(1), block);
  for (const [pid, v] of params) ex.ab_set_param(pid, v);
  for (let c = 0; c < constraints.length; c++) ex.ab_set_constraint(1, c, constraints[c] ? 1 : 0);
  // warm-up so the JIT has tiered up before the timed render (first run would read slow)
  for (let i = 0; i < 8; i++) ex.ab_note_on(48 + i * 3, 0.8);
  for (let k = 0; k < sampleRate; k += block) ex.ab_process_single(inst, block);
  ex.ab_panic(); ex.ab_reset_steals(inst);
  let pos = 0, ev = 0, sum = 0, peak = 0;
  const t0 = performance.now();
  while (pos < total) {
    let next = Math.min(total, pos + block);
    while (ev < events.length) {
      const x = events[ev];
      if (x.t <= pos) { if (x.on) ex.ab_note_on(x.note, x.vel); else ex.ab_note_off(x.note); ev++; }
      else { next = Math.min(next, x.t); break; }
    }
    const k = next - pos;
    ex.ab_process_single(inst, k);
    for (let i = 0; i < k; i++) { const l = outL[i], r = outR[i]; sum += l * l + r * r; const p = Math.max(Math.abs(l), Math.abs(r)); if (p > peak) peak = p; }
    pos = next;
  }
  const time = (performance.now() - t0) / 1000;
  return { rms: Math.sqrt(sum / Math.max(1, 2 * total)), peak, time };
}

self.onmessage = (e) => {
  const { id, module, sampleRate, params, constraints, events, lengthSamples, maxSeconds } = e.data;
  const t0 = performance.now();
  const total = Math.min(lengthSamples, Math.floor(maxSeconds * sampleRate));
  const a = renderOne(module, 0, sampleRate, params, constraints, events, total);
  const b = renderOne(module, 1, sampleRate, params, constraints, events, total);
  const db = (x) => 20 * Math.log10(Math.max(x, 1e-9));
  const seconds = total / sampleRate;
  self.postMessage({
    id,
    rmsA: db(a.rms), rmsB: db(b.rms), peakA: db(a.peak), peakB: db(b.peak),
    trimB: db(a.rms) - db(b.rms),
    seconds, timeA: a.time, timeB: b.time,
    xrtA: seconds / Math.max(a.time, 1e-6), xrtB: seconds / Math.max(b.time, 1e-6),
    totalMs: performance.now() - t0,
  });
};
