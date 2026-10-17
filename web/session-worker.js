// session-worker.js - renders ONE session track through both engines (A = Full, B = Gen1) and
// returns the two stereo renders plus the single-thread render time of each. The main thread
// runs one of these per track in parallel and sums the mixes.
// Each profile is rendered on its own wasm instance and timed as ONE whole render (worker clocks
// are coarsened to ~100 us, so per-block timing would be meaningless).

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
  const L = new Float32Array(total), R = new Float32Array(total);
  let pos = 0, ev = 0, peakV = 0;
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
    L.set(outL.subarray(0, k), pos); R.set(outR.subarray(0, k), pos);
    peakV = Math.max(peakV, ex.ab_active_voices(inst));
    pos = next;
  }
  const time = (performance.now() - t0) / 1000;
  return { L, R, time, peakV, steals: ex.ab_steals(inst) };
}

self.onmessage = (e) => {
  const { id, track, module, sampleRate, params, constraints, events, lengthSamples } = e.data;
  const total = lengthSamples;
  const a = renderOne(module, 0, sampleRate, params, constraints, events, total);
  const b = renderOne(module, 1, sampleRate, params, constraints, events, total);
  self.postMessage({
    id, track, AL: a.L, AR: a.R, BL: b.L, BR: b.R,
    timeA: a.time, timeB: b.time, seconds: total / sampleRate,
    peakVA: a.peakV, peakVB: b.peakV, stealsA: a.steals, stealsB: b.steals,
  }, [a.L.buffer, a.R.buffer, b.L.buffer, b.R.buffer]);
};
