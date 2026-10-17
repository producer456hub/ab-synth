// offline-worker.js - renders a patch + phrase through both engines OFF the audio thread.
// Used for loudness matching (RMS of A vs B -> trim) and as the Phase 2 benchmark harness
// (per-instance render time, never measured inside the AudioWorklet).

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

self.onmessage = (e) => {
  const { id, module, sampleRate, params, constraints, events, lengthSamples, maxSeconds } = e.data;
  const t0 = performance.now();
  const ex = instantiate(module).exports;
  const block = 256;
  ex.ab_init(sampleRate, block);
  const outL = new Float32Array(ex.memory.buffer, ex.ab_out_ptr(0), block);
  const outR = new Float32Array(ex.memory.buffer, ex.ab_out_ptr(1), block);
  for (const [pid, v] of params) ex.ab_set_param(pid, v);
  for (let c = 0; c < constraints.length; c++) ex.ab_set_constraint(1, c, constraints[c] ? 1 : 0);

  const total = Math.min(lengthSamples, Math.floor(maxSeconds * sampleRate));
  let pos = 0, ev = 0;
  let sumA = 0, sumB = 0, peakA = 0, peakB = 0;
  let timeA = 0, timeB = 0;
  while (pos < total) {
    let next = Math.min(total, pos + block);
    while (ev < events.length) {
      const x = events[ev];
      if (x.t <= pos) { if (x.on) ex.ab_note_on(x.note, x.vel); else ex.ab_note_off(x.note); ev++; }
      else { next = Math.min(next, x.t); break; }
    }
    const k = next - pos;
    let ta = performance.now();
    ex.ab_process_single(0, k);
    timeA += performance.now() - ta;
    for (let i = 0; i < k; i++) { const l = outL[i], r = outR[i]; sumA += l * l + r * r; const p = Math.max(Math.abs(l), Math.abs(r)); if (p > peakA) peakA = p; }
    ta = performance.now();
    ex.ab_process_single(1, k);
    timeB += performance.now() - ta;
    for (let i = 0; i < k; i++) { const l = outL[i], r = outR[i]; sumB += l * l + r * r; const p = Math.max(Math.abs(l), Math.abs(r)); if (p > peakB) peakB = p; }
    pos = next;
  }
  const rmsA = Math.sqrt(sumA / Math.max(1, 2 * total));
  const rmsB = Math.sqrt(sumB / Math.max(1, 2 * total));
  const db = (x) => 20 * Math.log10(Math.max(x, 1e-9));
  const seconds = total / sampleRate;
  self.postMessage({
    id,
    rmsA: db(rmsA), rmsB: db(rmsB), peakA: db(peakA), peakB: db(peakB),
    trimB: db(rmsA) - db(rmsB),
    seconds, timeA: timeA / 1000, timeB: timeB / 1000,
    xrtA: seconds / (timeA / 1000), xrtB: seconds / (timeB / 1000),
    totalMs: performance.now() - t0,
  });
};
