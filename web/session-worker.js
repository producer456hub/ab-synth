// session-worker.js - renders ONE session track through both engines (A = Full, B = Gen1) and
// returns the two stereo renders plus the single-thread render time of each. The main thread
// runs one of these per track in parallel and sums the mixes.

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
  const { id, track, module, sampleRate, params, constraints, events, lengthSamples } = e.data;
  const ex = instantiate(module).exports;
  const block = 256;
  ex.ab_init(sampleRate, block);
  const outL = new Float32Array(ex.memory.buffer, ex.ab_out_ptr(0), block);
  const outR = new Float32Array(ex.memory.buffer, ex.ab_out_ptr(1), block);
  for (const [pid, v] of params) ex.ab_set_param(pid, v);
  for (let c = 0; c < constraints.length; c++) ex.ab_set_constraint(1, c, constraints[c] ? 1 : 0);

  const total = lengthSamples;
  const AL = new Float32Array(total), AR = new Float32Array(total), BL = new Float32Array(total), BR = new Float32Array(total);
  let pos = 0, ev = 0, timeA = 0, timeB = 0, peakVA = 0, peakVB = 0;
  while (pos < total) {
    let next = Math.min(total, pos + block);
    while (ev < events.length) {
      const x = events[ev];
      if (x.t <= pos) { if (x.on) ex.ab_note_on(x.note, x.vel); else ex.ab_note_off(x.note); ev++; }
      else { next = Math.min(next, x.t); break; }
    }
    const k = next - pos;
    let t0 = performance.now();
    ex.ab_process_single(0, k);
    timeA += performance.now() - t0;
    AL.set(outL.subarray(0, k), pos); AR.set(outR.subarray(0, k), pos);
    t0 = performance.now();
    ex.ab_process_single(1, k);
    timeB += performance.now() - t0;
    BL.set(outL.subarray(0, k), pos); BR.set(outR.subarray(0, k), pos);
    peakVA = Math.max(peakVA, ex.ab_active_voices(0));
    peakVB = Math.max(peakVB, ex.ab_active_voices(1));
    pos = next;
  }
  self.postMessage({
    id, track, AL, AR, BL, BR,
    timeA: timeA / 1000, timeB: timeB / 1000, seconds: total / sampleRate,
    peakVA, peakVB, stealsA: ex.ab_steals(0), stealsB: ex.ab_steals(1),
  }, [AL.buffer, AR.buffer, BL.buffer, BR.buffer]);
};
