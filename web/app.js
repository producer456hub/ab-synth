// app.js - AB Synth web UI: patch selection, phrase player, A/B + blind ABX, Gen1 constraint
// toggles, loudness matching (offline worker), Gen1 budget projection, on-screen keyboard.

const PATCHES = ['control_bass', 'supersaw_pad', 'screaming_lead', 'snappy_pluck', 'ambient_tail'];
const CONSTRAINTS = [
  { id: 0, label: 'Polyphony 32 → 8', short: 'Poly 8' },
  { id: 1, label: 'Unison 7 → 3', short: 'Unison 3' },
  { id: 2, label: 'Oversampling 4× → 1×', short: 'OS 1×' },
  { id: 3, label: 'Mod rate per-sample → 32-sample block', short: 'Mod /32' },
  { id: 4, label: 'Reverb FDN-16 → Schroeder', short: 'Schroeder' },
  { id: 5, label: 'Delay cubic → linear', short: 'Lin delay' },
];
const ABX_TRIALS = 10;

const $ = (s) => document.querySelector(s);
const state = {
  ctx: null, node: null, gain: null, module: null, bytes: null,
  paramIds: {}, patches: {}, patch: null,
  constraints: [true, true, true, true, true, true],
  mix: 0, loop: true, playing: false,
  matchEnabled: true, trimB: 0, lastMeasure: null, measureSeq: 0,
  abx: null, octave: 4, held: new Set(), factor: 20,
};

// ---------------------------------------------------------------- wasm + audio
async function loadWasm() {
  const res = await fetch('absynth.wasm', { cache: 'no-cache' });
  state.bytes = await res.arrayBuffer();
  state.module = await WebAssembly.compile(state.bytes);
  // query parameter names from a throwaway main-thread instance
  const stubs = {};
  for (const imp of WebAssembly.Module.imports(state.module)) { stubs[imp.module] = stubs[imp.module] || {}; stubs[imp.module][imp.name] = () => 0; }
  const inst = await WebAssembly.instantiate(state.module, stubs);
  const ex = inst.exports;
  if (ex._initialize) ex._initialize();
  const mem = new Uint8Array(ex.memory.buffer);
  state.paramDefaults = [];
  for (let i = 0; i < ex.ab_param_count(); i++) {
    const p = ex.ab_param_name(i); let e = p; while (mem[e]) e++;
    state.paramIds[new TextDecoder().decode(mem.subarray(p, e))] = i;
    state.paramDefaults.push([i, ex.ab_param_default(i)]);
  }
}

async function startAudio() {
  if (state.ctx) { await state.ctx.resume(); return; }
  const ctx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
  await ctx.audioWorklet.addModule('worklet.js');
  const node = new AudioWorkletNode(ctx, 'absynth', {
    numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
    processorOptions: { module: state.module, sampleRate: ctx.sampleRate },
  });
  const gain = ctx.createGain();
  gain.gain.value = parseFloat($('#volume').value);
  node.connect(gain).connect(ctx.destination);
  node.port.onmessage = (e) => onWorkletMessage(e.data);
  state.ctx = ctx; state.node = node; state.gain = gain;
  await ctx.resume();
  $('#srInfo').textContent = `${ctx.sampleRate} Hz · ${ctx.baseLatency ? Math.round(ctx.baseLatency * 1000) + ' ms' : ''}`;
  send({ type: 'profile', inst: 0, profile: 0 });
  send({ type: 'profile', inst: 1, profile: 1 });
  CONSTRAINTS.forEach((c) => send({ type: 'constraint', inst: 1, c: c.id, on: state.constraints[c.id] }));
  applyPatch(state.patch || PATCHES[0]);
}

function send(m) { if (state.node) state.node.port.postMessage(m); }

// ---------------------------------------------------------------- patches + phrases
async function loadPatches() {
  for (const name of PATCHES.concat(['session_8track'])) {
    const r = await fetch(`patches/${name}.json`, { cache: 'no-cache' });
    state.patches[name] = await r.json();
  }
}

function phraseEvents(p, sr) {
  const bpm = p.bpm || 120, spb = 60 / bpm;
  const repeat = p.repeat || 1, loopBeats = p.loopBeats || 0;
  const ev = [];
  let lastOff = 0;
  for (let r = 0; r < repeat; r++) {
    const off = r * loopBeats * spb;
    for (const e of (p.phrase || [])) {
      const t0 = off + e[0] * spb, t1 = t0 + e[1] * spb;
      const vel = (e.length > 3 ? e[3] : 100) / 127;
      ev.push({ t: Math.round(t0 * sr), note: e[2], vel, on: true });
      ev.push({ t: Math.round(t1 * sr), note: e[2], vel, on: false });
      lastOff = Math.max(lastOff, t1);
    }
  }
  ev.sort((a, b) => a.t - b.t || (a.on === b.on ? 0 : a.on ? 1 : -1));
  const tail = p.tail == null ? 2 : p.tail;
  return { events: ev, length: Math.round((lastOff + tail) * sr) };
}

function paramList(p) {
  const list = [];
  for (const [k, v] of Object.entries(p.params || {})) if (k in state.paramIds) list.push([state.paramIds[k], v]);
  return list;
}

function applyPatch(name) {
  state.patch = name;
  const p = state.patches[name];
  document.querySelectorAll('#patches button').forEach((b) => b.classList.toggle('on', b.dataset.patch === name));
  $('#patchDesc').textContent = p.description || '';
  if (!state.node) return;
  stopPhrase();
  if (state.session && state.session.playing) sessionStop();
  send({ type: 'panic' });
  // reset everything to defaults first, then the patch (a patch only lists what it changes)
  send({ type: 'params', list: state.paramDefaults.concat(paramList(p)) });
  if (state.matchEnabled) measureLoudness(); else setTrim(0);
}

function playPhrase() {
  if (!state.node) return;
  const p = state.patches[state.patch];
  const ph = phraseEvents(p, state.ctx.sampleRate);
  ph.loop = state.loop;
  send({ type: 'phrase', phrase: ph });
  state.playing = true;
  $('#play').textContent = '■ Stop';
}
function stopPhrase() {
  send({ type: 'stop' });
  state.playing = false;
  $('#play').textContent = '▶ Play phrase';
}

// ---------------------------------------------------------------- A / B / X
function setMix(x, fromAbx) {
  state.mix = x;
  send({ type: 'mix', value: x });
  if (state.session && state.session.playing) sessionApplyMix(x);
  const abx = state.abx;
  $('#btnA').classList.toggle('on', !abx && x === 0);
  $('#btnB').classList.toggle('on', !abx && x === 1);
  if (abx) {
    $('#btnA').classList.toggle('on', abx.listening === 'A');
    $('#btnB').classList.toggle('on', abx.listening === 'B');
    $('#btnX').classList.toggle('on', abx.listening === 'X');
  } else $('#btnX').classList.remove('on');
}

function listen(which) {
  if (state.abx) {
    state.abx.listening = which;
    const target = which === 'X' ? state.abx.xIs : which;
    setMix(target === 'A' ? 0 : 1);
  } else if (which !== 'X') setMix(which === 'A' ? 0 : 1);
}

function abxStart() {
  state.abx = { trial: 0, correct: 0, log: [], xIs: null, listening: null };
  abxNext();
  $('#abxPanel').classList.add('active');
  $('#btnX').disabled = false;
  updateAbx();
}
function abxNext() {
  const a = state.abx;
  a.trial++;
  a.xIs = Math.random() < 0.5 ? 'A' : 'B';
  a.listening = null;
  setMix(0);
  $('#btnA').classList.remove('on'); $('#btnB').classList.remove('on'); $('#btnX').classList.remove('on');
}
function abxAnswer(guess) {
  const a = state.abx; if (!a) return;
  const ok = guess === a.xIs;
  if (ok) a.correct++;
  a.log.push({ trial: a.trial, xIs: a.xIs, guess, ok });
  if (a.trial >= ABX_TRIALS) abxFinish(); else { abxNext(); updateAbx(); }
}
function binomialTail(n, k) {         // P(X >= k) for X ~ Bin(n, 0.5)
  let p = 0;
  for (let i = k; i <= n; i++) { let c = 1; for (let j = 1; j <= i; j++) c = c * (n - i + j) / j; p += c; }
  return p / Math.pow(2, n);
}
function abxFinish() {
  const a = state.abx;
  const p = binomialTail(ABX_TRIALS, a.correct);
  const verdict = p < 0.05 ? 'You can hear the difference (p < 0.05).' : 'Not distinguishable at p < 0.05 (could be chance).';
  $('#abxStatus').innerHTML = `<b>${a.correct} / ${ABX_TRIALS} correct</b> · p = ${p.toFixed(3)}<br>${verdict}`;
  $('#abxLog').textContent = a.log.map((l) => `${l.trial}: X was ${l.xIs}, you said ${l.guess} ${l.ok ? '✓' : '✗'}`).join('\n');
  state.abx = null;
  $('#btnX').disabled = true;
  $('#btnA').classList.remove('on'); $('#btnB').classList.remove('on'); $('#btnX').classList.remove('on');
  setMix(0);
  $('#abxAnswerA').disabled = true; $('#abxAnswerB').disabled = true;
  $('#abxStart').textContent = 'Blind X again';
}
function updateAbx() {
  const a = state.abx;
  $('#abxStatus').textContent = `Trial ${a.trial} of ${ABX_TRIALS}. Listen to A, B and X, then answer.`;
  $('#abxLog').textContent = a.log.map((l) => `${l.trial}: ${l.ok ? '✓' : '✗'}`).join('  ');
  $('#abxAnswerA').disabled = false; $('#abxAnswerB').disabled = false;
  $('#abxStart').textContent = 'Restart';
}

// ---------------------------------------------------------------- loudness matching + budget
function setTrim(db) {
  state.trimB = db;
  send({ type: 'trim', inst: 1, db });
  $('#trimInfo').textContent = `B trim ${db >= 0 ? '+' : ''}${db.toFixed(1)} dB`;
}

let worker = null;
function measureLoudness() {
  if (!state.module || !state.ctx) return;
  const p = state.patches[state.patch];
  const sr = state.ctx.sampleRate;
  const ph = phraseEvents(p, sr);
  const id = ++state.measureSeq;
  if (!worker) worker = new Worker('offline-worker.js');
  $('#trimInfo').textContent = 'matching…';
  worker.onmessage = (e) => {
    const r = e.data;
    if (r.id !== id) return;
    state.lastMeasure = r;
    if (state.matchEnabled) setTrim(Math.max(-12, Math.min(12, r.trimB))); else setTrim(0);
    $('#levels').textContent = `A ${r.rmsA.toFixed(1)} dBFS · B ${r.rmsB.toFixed(1)} dBFS (RMS over phrase)`;
    updateBudget();
  };
  worker.postMessage({
    id, module: state.module, sampleRate: sr,
    params: paramList(p), constraints: state.constraints,
    events: ph.events, lengthSamples: ph.length, maxSeconds: 12,
  });
}

function updateBudget() {
  const factor = state.factor;
  if (state.session) {
    const s = state.session;
    const gen1 = s.loadB * factor / 3, pct = gen1 / 0.70 * 100;
    const perTrack = s.loadB / s.tracks.length;
    const fit = Math.floor(0.70 * 3 / (perTrack * factor));
    $('#budgetBar').style.width = Math.min(100, pct) + '%';
    $('#budgetBar').classList.toggle('over', pct > 100);
    $('#budgetText').textContent =
      `Session on this device: A ${(s.loadA * 100).toFixed(0)} % of a core, B ${(s.loadB * 100).toFixed(0)} %. ` +
      `Gen1 projection ×${factor}: B needs ${pct.toFixed(0)} % of the 70 % budget on 3 cores → about ${fit} tracks like these fit. ` +
      `A (Full) would need ${(s.loadA * factor / 3 / 0.70 * 100).toFixed(0)} %.`;
    return;
  }
  const r = state.lastMeasure; if (!r) return;
  // fraction of ONE core this machine needs for B, projected onto the RK3288 and its 3 usable cores at 70 % target
  const hostLoadB = 1 / r.xrtB, hostLoadA = 1 / r.xrtA;
  const gen1Load = hostLoadB * factor / 3;
  const pct = gen1Load / 0.70 * 100;
  $('#budgetBar').style.width = Math.min(100, pct) + '%';
  $('#budgetBar').classList.toggle('over', pct > 100);
  $('#budgetText').textContent =
    `This device: A ${r.xrtA.toFixed(0)}× realtime (${(hostLoadA * 100).toFixed(1)} % of a core), ` +
    `B ${r.xrtB.toFixed(0)}× (${(hostLoadB * 100).toFixed(1)} %). ` +
    `Gen1 projection ×${factor}: ${pct.toFixed(0)} % of the 70 % budget on 3 cores.`;
}

// ---------------------------------------------------------------- 8-track session (offline render, buffer A/B)
// Event logic mirrors dsp/tests/session.cpp (trackEvents): keep both in sync.
function sessionTrackEvents(session, t, sr) {
  const spb = 60 / (session.bpm || 100), bpc = session.beatsPerChord || 4;
  const ev = []; let lastOff = 0;
  session.progression.forEach((chord, c) => {
    const start = (t.offsetBeats || 0) + c * bpc;
    const step = t.stabBeats > 0 ? t.stabBeats : bpc;
    for (let s = 0; s < bpc - 1e-9; s += step) {
      const t0 = (start + s) * spb, t1 = t0 + (t.hold == null ? 0.9 : t.hold) * step * spb;
      for (const n of chord) {
        const note = n + (t.transpose || 0);
        ev.push({ t: Math.round(t0 * sr), note, vel: 0.8, on: true });
        ev.push({ t: Math.round(t1 * sr), note, vel: 0.8, on: false });
        lastOff = Math.max(lastOff, t1);
      }
    }
  });
  ev.sort((a, b) => a.t - b.t || (a.on === b.on ? 0 : a.on ? 1 : -1));
  return { events: ev, lastOff };
}

async function renderSession() {
  if (!state.ctx) return;
  const q = $;
  const session = state.patches.session_8track;
  if (!session) { q('#sessionInfo').textContent = 'session file missing'; return; }
  sessionStop();
  const sr = state.ctx.sampleRate;
  const tail = session.tail == null ? 2.5 : session.tail;
  let lengthSec = 0;
  const tracks = session.tracks.map((t) => { const r = sessionTrackEvents(session, t, sr); lengthSec = Math.max(lengthSec, r.lastOff + tail); return { t, events: r.events }; });
  const total = Math.round(lengthSec * sr);
  q('#sessionRender').disabled = true; q('#sessionPlay').disabled = true;
  q('#sessionInfo').textContent = `rendering ${tracks.length} tracks × 2 profiles, ${lengthSec.toFixed(1)} s each…`;
  q('#sessionProgress').style.width = '0%';
  const results = new Array(tracks.length);
  let done = 0;
  const seq = ++state.measureSeq;
  await Promise.all(tracks.map((tr, i) => new Promise((resolve) => {
    const w = new Worker('session-worker.js');
    const patch = state.patches[tr.t.patch] || { params: {} };
    const params = state.paramDefaults.concat(paramList(patch));
    for (const [k, v] of Object.entries(tr.t.params || {})) if (k in state.paramIds) params.push([state.paramIds[k], v]);
    w.onmessage = (e) => { results[i] = e.data; w.terminate(); done++; q('#sessionProgress').style.width = (100 * done / tracks.length) + '%'; resolve(); };
    w.postMessage({ id: seq, track: i, module: state.module, sampleRate: sr, params, constraints: state.constraints, events: tr.events, lengthSamples: total });
  })));
  if (seq !== state.measureSeq) return;
  // sum with per-track gain, common normalization
  const AL = new Float32Array(total), AR = new Float32Array(total), BL = new Float32Array(total), BR = new Float32Array(total);
  results.forEach((r, i) => {
    const g = tracks[i].t.gain == null ? 0.3 : tracks[i].t.gain;
    for (let k = 0; k < total; k++) { AL[k] += r.AL[k] * g; AR[k] += r.AR[k] * g; BL[k] += r.BL[k] * g; BR[k] += r.BR[k] * g; }
  });
  let peak = 0, sA = 0, sB = 0;
  for (let k = 0; k < total; k++) { peak = Math.max(peak, Math.abs(AL[k]), Math.abs(AR[k]), Math.abs(BL[k]), Math.abs(BR[k])); }
  const norm = peak > 0.9 ? 0.9 / peak : 1;
  for (let k = 0; k < total; k++) { AL[k] *= norm; AR[k] *= norm; BL[k] *= norm; BR[k] *= norm; sA += AL[k] * AL[k] + AR[k] * AR[k]; sB += BL[k] * BL[k] + BR[k] * BR[k]; }
  const db = (x) => 20 * Math.log10(Math.max(x, 1e-9));
  const rmsA = db(Math.sqrt(sA / (2 * total))), rmsB = db(Math.sqrt(sB / (2 * total)));
  const bufA = state.ctx.createBuffer(2, total, sr), bufB = state.ctx.createBuffer(2, total, sr);
  bufA.copyToChannel(AL, 0); bufA.copyToChannel(AR, 1); bufB.copyToChannel(BL, 0); bufB.copyToChannel(BR, 1);
  const loadA = results.reduce((a, r) => a + r.timeA / r.seconds, 0), loadB = results.reduce((a, r) => a + r.timeB / r.seconds, 0);
  state.session = { bufA, bufB, rmsA, rmsB, loadA, loadB, results, tracks, playing: false, nodes: null, lengthSec };
  const rows = results.map((r, i) => `${(tracks[i].t.name || tracks[i].t.patch).padEnd(11)} A ${(100 * r.timeA / r.seconds).toFixed(1).padStart(5)} %  B ${(100 * r.timeB / r.seconds).toFixed(1).padStart(5)} %  voices ${String(r.peakVA).padStart(2)}/${String(r.peakVB).padStart(2)}  B stole ${r.stealsB}`);
  q('#sessionTable').textContent = rows.join('\n') + `\n${'TOTAL'.padEnd(11)} A ${(100 * loadA).toFixed(1).padStart(5)} %  B ${(100 * loadB).toFixed(1).padStart(5)} %  of one core on this device`;
  q('#sessionInfo').textContent = `Rendered ${lengthSec.toFixed(1)} s. RMS A ${rmsA.toFixed(1)} / B ${rmsB.toFixed(1)} dBFS. Press Play session, then A / B / Blind X as usual.`;
  q('#sessionRender').disabled = false; q('#sessionPlay').disabled = false; q('#sessionExit').disabled = false;
  updateBudget();
}

function sessionApplyMix(x) {
  const s = state.session; if (!s || !s.nodes) return;
  const t = state.ctx.currentTime, tc = 0.004;    // ~10 ms settle
  const trimB = state.matchEnabled ? Math.pow(10, Math.max(-12, Math.min(12, s.rmsA - s.rmsB)) / 20) : 1;
  s.nodes.gA.gain.setTargetAtTime(Math.cos(x * Math.PI / 2), t, tc);
  s.nodes.gB.gain.setTargetAtTime(Math.sin(x * Math.PI / 2) * trimB, t, tc);
}

function sessionPlay() {
  const s = state.session; if (!s) return;
  sessionStop(false);
  stopPhrase(); send({ type: 'panic' });
  const ctx = state.ctx;
  const srcA = ctx.createBufferSource(), srcB = ctx.createBufferSource();
  srcA.buffer = s.bufA; srcB.buffer = s.bufB; srcA.loop = srcB.loop = true;
  const gA = ctx.createGain(), gB = ctx.createGain();
  srcA.connect(gA).connect(state.gain); srcB.connect(gB).connect(state.gain);
  const t0 = ctx.currentTime + 0.05;
  srcA.start(t0); srcB.start(t0);
  s.nodes = { srcA, srcB, gA, gB }; s.playing = true;
  gA.gain.value = 1; gB.gain.value = 0;
  sessionApplyMix(state.mix);
  $('#sessionPlay').textContent = '■ Stop session';
  $('#voices').textContent = 'session: 8 tracks (pre-rendered)';
}

function sessionStop(resetLabel = true) {
  const s = state.session; if (!s || !s.nodes) { if (resetLabel) $('#sessionPlay').textContent = '▶ Play session'; return; }
  try { s.nodes.srcA.stop(); s.nodes.srcB.stop(); } catch (e) {}
  s.nodes = null; s.playing = false;
  if (resetLabel) $('#sessionPlay').textContent = '▶ Play session';
}

function sessionExit() {
  sessionStop();
  state.session = null;
  $('#sessionPlay').disabled = true; $('#sessionExit').disabled = true;
  $('#sessionTable').textContent = '';
  applyPatch(state.patch);
}

// ---------------------------------------------------------------- status from the worklet
function onWorkletMessage(m) {
  if (m.type === 'status') {
    $('#voices').textContent = `voices A ${m.voicesA}/${m.maxA} · B ${m.voicesB}/${m.maxB}`;
    const pk = Math.min(1, m.peak);
    $('#meter').style.width = (pk * 100) + '%';
    if (m.length) $('#playhead').style.width = (100 * Math.min(1, m.pos / m.length)) + '%';
  } else if (m.type === 'phraseEnd') {
    state.playing = false;
    $('#play').textContent = '▶ Play phrase';
  }
}

// ---------------------------------------------------------------- keyboard
function buildKeyboard() {
  const kb = $('#keys');
  kb.innerHTML = '';
  const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  for (let i = 0; i < 25; i++) {
    const n = 12 * state.octave + i;
    const k = document.createElement('div');
    const black = [1, 3, 6, 8, 10].includes(i % 12);
    k.className = 'key' + (black ? ' black' : '');
    k.dataset.note = n;
    if (!black && i % 12 === 0) k.textContent = 'C' + (state.octave - 1);
    kb.appendChild(k);
  }
  const down = (ev) => {
    const el = ev.target.closest('.key'); if (!el) return;
    ev.preventDefault();
    const note = +el.dataset.note;
    if (state.held.has(note)) return;
    state.held.add(note); el.classList.add('down');
    send({ type: 'noteOn', note, vel: 0.8 });
  };
  const up = (ev) => {
    const el = ev.target.closest('.key'); if (!el) return;
    const note = +el.dataset.note;
    state.held.delete(note); el.classList.remove('down');
    send({ type: 'noteOff', note });
  };
  kb.onpointerdown = down;
  kb.onpointerup = up; kb.onpointercancel = up; kb.onpointerleave = up;
  kb.onpointerout = (ev) => { if (ev.target.classList && ev.target.classList.contains('key')) up(ev); };
}

// ---------------------------------------------------------------- wiring
function wire() {
  $('#start').addEventListener('click', async () => {
    $('#start').textContent = 'starting…';
    try { await startAudio(); $('#overlay').classList.add('hidden'); }
    catch (e) { $('#start').textContent = 'Failed: ' + e.message; console.error(e); }
  });
  const pb = $('#patches');
  PATCHES.forEach((name) => {
    const b = document.createElement('button');
    b.dataset.patch = name;
    b.textContent = (state.patches[name] && state.patches[name].name) || name;
    b.onclick = () => applyPatch(name);
    pb.appendChild(b);
  });
  $('#play').onclick = () => (state.playing ? stopPhrase() : playPhrase());
  $('#loop').onchange = (e) => { state.loop = e.target.checked; };
  $('#volume').oninput = (e) => { if (state.gain) state.gain.gain.value = parseFloat(e.target.value); };
  $('#btnA').onclick = () => listen('A');
  $('#btnB').onclick = () => listen('B');
  $('#btnX').onclick = () => listen('X');
  $('#abxStart').onclick = abxStart;
  $('#abxAnswerA').onclick = () => abxAnswer('A');
  $('#abxAnswerB').onclick = () => abxAnswer('B');
  const cb = $('#constraints');
  CONSTRAINTS.forEach((c) => {
    const l = document.createElement('label');
    l.innerHTML = `<input type="checkbox" checked data-c="${c.id}"><span>${c.label}</span>`;
    l.querySelector('input').onchange = (e) => {
      state.constraints[c.id] = e.target.checked;
      send({ type: 'constraint', inst: 1, c: c.id, on: e.target.checked });
      if (state.matchEnabled) measureLoudness();
    };
    cb.appendChild(l);
  });
  $('#allOn').onclick = () => cb.querySelectorAll('input').forEach((i) => { if (!i.checked) { i.checked = true; i.onchange({ target: i }); } });
  $('#allOff').onclick = () => cb.querySelectorAll('input').forEach((i) => { if (i.checked) { i.checked = false; i.onchange({ target: i }); } });
  $('#match').onchange = (e) => { state.matchEnabled = e.target.checked; if (state.matchEnabled) measureLoudness(); else setTrim(0); };
  $('#factor').onchange = (e) => { state.factor = Math.max(1, parseFloat(e.target.value) || 20); updateBudget(); };
  $('#octDown').onclick = () => { state.octave = Math.max(1, state.octave - 1); buildKeyboard(); };
  $('#octUp').onclick = () => { state.octave = Math.min(7, state.octave + 1); buildKeyboard(); };
  $('#panic').onclick = () => { send({ type: 'panic' }); stopPhrase(); sessionStop(); };
  $('#sessionRender').onclick = renderSession;
  $('#sessionPlay').onclick = () => (state.session && state.session.playing ? sessionStop() : sessionPlay());
  $('#sessionExit').onclick = sessionExit;
  buildKeyboard();
}

(async () => {
  try {
    await Promise.all([loadWasm(), loadPatches()]);
    wire();
    applyPatch(PATCHES[0]);
    $('#start').disabled = false;
    $('#start').textContent = 'Tap to start audio';
  } catch (e) {
    $('#start').textContent = 'Load failed: ' + e.message;
    console.error(e);
  }
})();
