// export-worker.js - offline render of the studio loop through the wasm Session (Gen1 or Full),
// returned as stereo Float32 buffers. Also used to calibrate per-track CPU cost (see studio.js).

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

// message: { id, module, sampleRate, tracks: [{ params: [[id, v]], gain, pan, mute, solo, profile }],
//            master, events: [{ t (samples), track, pitch, vel, on }], lengthSamples, wantAudio }
self.onmessage = (e) => {
  const { id, module, sampleRate, tracks, master, events, lengthSamples, wantAudio } = e.data;
  const ex = instantiate(module).exports;
  const block = 256;
  ex.ab_session_init(sampleRate, block, tracks.length);
  const outL = new Float32Array(ex.memory.buffer, ex.ab_out_ptr(0), block);
  const outR = new Float32Array(ex.memory.buffer, ex.ab_out_ptr(1), block);
  tracks.forEach((tr, t) => {
    for (const [pid, v] of tr.params) ex.ab_session_set_param(t, pid, v);
    ex.ab_session_set_profile(t, tr.profile == null ? 1 : tr.profile);
    ex.ab_session_mix(t, tr.gain == null ? 0.8 : tr.gain, tr.pan || 0, tr.mute ? 1 : 0, tr.solo ? 1 : 0);
  });
  ex.ab_session_master(master == null ? 0.8 : master);
  // Warm-up: the wasm engine runs baseline (unoptimized) code until the JIT tiers up, so the first
  // render would be timed slow. Render one throwaway second with every track busy, then reset.
  for (let t = 0; t < tracks.length; t++) for (let i = 0; i < 8; i++) ex.ab_session_note_on(t, 48 + i * 3, 0.8);
  for (let k = 0; k < sampleRate; k += block) ex.ab_session_process(block);
  ex.ab_session_panic();
  for (let t = 0; t < tracks.length; t++) ex.ab_session_reset_steals(t);
  const total = lengthSamples;
  const L = wantAudio ? new Float32Array(total) : null, R = wantAudio ? new Float32Array(total) : null;
  let pos = 0, ev = 0, peak = 0;
  const peakVoices = new Array(tracks.length).fill(0);
  // Worker clocks are coarsened (~100 us), so time the WHOLE render, never per block.
  const t0 = performance.now();
  while (pos < total) {
    let next = Math.min(total, pos + block);
    while (ev < events.length) {
      const x = events[ev];
      if (x.t <= pos) { if (x.on) ex.ab_session_note_on(x.track, x.pitch, x.vel); else ex.ab_session_note_off(x.track, x.pitch); ev++; }
      else { next = Math.min(next, x.t); break; }
    }
    const k = next - pos;
    ex.ab_session_process(k);
    if (wantAudio) { L.set(outL.subarray(0, k), pos); R.set(outR.subarray(0, k), pos); }
    for (let i = 0; i < k; i++) { const a = Math.max(Math.abs(outL[i]), Math.abs(outR[i])); if (a > peak) peak = a; }
    for (let t = 0; t < tracks.length; t++) peakVoices[t] = Math.max(peakVoices[t], ex.ab_session_voices(t));
    pos = next;
  }
  const time = performance.now() - t0;
  const msg = { id, seconds: total / sampleRate, renderSeconds: time / 1000, peak, peakVoices, steals: tracks.map((_, t) => ex.ab_session_steals(t)) };
  if (wantAudio) { msg.L = L; msg.R = R; self.postMessage(msg, [L.buffer, R.buffer]); }
  else self.postMessage(msg);
};
