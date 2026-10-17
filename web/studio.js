// studio.js - the "Gen1 OS": 8 synth tracks, step sequencer with live record, mixer, patch editor,
// RK3288 budget simulation, autosave, JSON save/load, loop export to WAV.

const N_TRACKS = 8;
const STEPS_PER_BAR = 16;
const PATCH_FILES = ['control_bass', 'supersaw_pad', 'screaming_lead', 'snappy_pluck', 'ambient_tail'];
const INLINE_PRESETS = {
  'Init saw':   { osc1Wave: 0, cutoff: 3000, resonance: 0.1, ampA: 0.005, ampD: 0.3, ampS: 0.8, ampR: 0.3 },
  'Keys':       { osc1Wave: 3, osc1Level: 0.8, osc2Wave: 2, osc2Level: 0.4, osc2Semi: 12, cutoff: 2600, resonance: 0.15, filterEnvAmt: 18, filA: 0.003, filD: 0.5, filS: 0.1, ampA: 0.003, ampD: 0.9, ampS: 0.45, ampR: 0.35, chorusMix: 0.25, reverbMix: 0.12, reverbSize: 0.4 },
  'Bells':      { osc1Wave: 2, osc1Level: 0.7, osc2Wave: 2, osc2Level: 0.5, osc2Semi: 19, osc2Fine: 8, cutoff: 6000, filterEnvAmt: 24, filA: 0.002, filD: 1.2, filS: 0, ampA: 0.002, ampD: 1.6, ampS: 0, ampR: 0.8, delayMix: 0.25, delayTime: 0.33, delayFeedback: 0.35, reverbMix: 0.2 },
  'Strings':    { osc1Wave: 0, unisonVoices: 5, unisonDetune: 18, unisonSpread: 0.8, cutoff: 1800, resonance: 0.05, filterKeyTrack: 0.4, ampA: 0.5, ampD: 1, ampS: 0.9, ampR: 1.0, filA: 0.6, filS: 0.5, filterEnvAmt: 10, chorusMix: 0.35, reverbMix: 0.25, reverbSize: 0.7 },
  'Sub bass':   { osc1Wave: 2, osc1Level: 1, osc2Wave: 0, osc2Level: 0.25, cutoff: 250, resonance: 0.1, filterEnvAmt: 20, filA: 0.003, filD: 0.15, filS: 0, mono: 1, glide: 0.04, ampA: 0.003, ampD: 0.3, ampS: 0.9, ampR: 0.08 },
  'Square lead':{ osc1Wave: 1, osc2Wave: 1, osc2Level: 0.5, osc2Fine: 12, cutoff: 2200, resonance: 0.4, filterEnvAmt: 24, filD: 0.25, filS: 0.3, mono: 1, glide: 0.06, drive: 0.4, lfo1Rate: 5.5, lfo1ToPitch: 0.15, delayMix: 0.2, delayTime: 0.25, ampA: 0.004, ampD: 0.2, ampS: 0.8, ampR: 0.15 },
  'Organ':      { osc1Wave: 2, osc2Wave: 2, osc2Level: 0.6, osc2Semi: 12, cutoff: 5000, ampA: 0.004, ampD: 0.1, ampS: 1, ampR: 0.05, filA: 0.004, filS: 1, chorusMix: 0.4, chorusRate: 1.2, chorusDepth: 0.3, drive: 0.25 },
  'Noise stab': { osc1Wave: 1, unisonVoices: 3, unisonDetune: 60, cutoff: 900, resonance: 0.7, filterEnvAmt: 48, filA: 0.001, filD: 0.12, filS: 0, ampA: 0.001, ampD: 0.18, ampS: 0, ampR: 0.08, drive: 0.6, reverbMix: 0.15 },
};

const $ = (s) => document.querySelector(s);
const state = {
  ctx: null, node: null, gain: null, module: null,
  paramIds: {}, paramInfo: [], presets: {}, presetNames: [],
  project: null, sel: 0, noteLen: 2, vel: 0.8, rollBase: 48, octave: 4,
  held: new Set(), status: null, playing: false, recording: false,
  clipboard: null, saveTimer: null, calibQueue: [], calibBusy: false, calibSeq: 0,
};

// ---------------------------------------------------------------- wasm + presets
async function loadWasm() {
  const res = await fetch('absynth.wasm', { cache: 'no-cache' });
  state.module = await WebAssembly.compile(await res.arrayBuffer());
  const stubs = {};
  for (const imp of WebAssembly.Module.imports(state.module)) { stubs[imp.module] = stubs[imp.module] || {}; stubs[imp.module][imp.name] = () => 0; }
  const ex = (await WebAssembly.instantiate(state.module, stubs)).exports;
  if (ex._initialize) ex._initialize();
  const mem = new Uint8Array(ex.memory.buffer);
  for (let i = 0; i < ex.ab_param_count(); i++) {
    const p = ex.ab_param_name(i); let e = p; while (mem[e]) e++;
    const name = new TextDecoder().decode(mem.subarray(p, e));
    state.paramIds[name] = i;
    state.paramInfo.push({ id: i, name, min: ex.ab_param_min(i), max: ex.ab_param_max(i), def: ex.ab_param_default(i) });
  }
}
async function loadPresets() {
  for (const f of PATCH_FILES) {
    const j = await (await fetch(`patches/${f}.json`, { cache: 'no-cache' })).json();
    state.presets[j.name] = j.params;
  }
  for (const [k, v] of Object.entries(INLINE_PRESETS)) state.presets[k] = v;
  state.presetNames = Object.keys(state.presets);
}

// ---------------------------------------------------------------- project
function defaultProject() {
  const names = ['Keys', 'Sub bass', 'Supersaw pad', 'Snappy pluck', 'Bells', 'Strings', 'Square lead', 'Noise stab'];
  const tracks = names.map((preset) => ({ preset, params: {}, gain: 0.8, pan: 0, mute: false, solo: false, pattern: { bars: 1, notes: [] } }));
  // a little starter so the first Play makes sound
  tracks[0].pattern.notes = [{ step: 0, len: 8, pitch: 60, vel: 0.8 }, { step: 0, len: 8, pitch: 64, vel: 0.8 }, { step: 0, len: 8, pitch: 67, vel: 0.8 }, { step: 8, len: 8, pitch: 57, vel: 0.8 }, { step: 8, len: 8, pitch: 60, vel: 0.8 }, { step: 8, len: 8, pitch: 65, vel: 0.8 }];
  tracks[1].pattern.notes = [{ step: 0, len: 3, pitch: 36, vel: 0.9 }, { step: 4, len: 3, pitch: 36, vel: 0.8 }, { step: 8, len: 3, pitch: 33, vel: 0.9 }, { step: 12, len: 3, pitch: 41, vel: 0.8 }];
  tracks[1].pattern.notes.forEach(() => {});
  return { bpm: 110, master: 0.8, factor: 20, simulate: true, profile: 1, tracks };
}
function fullParams(tr) {
  // defaults -> preset -> per-track overrides, as [id, value] pairs
  const vals = {};
  for (const p of state.paramInfo) vals[p.name] = p.def;
  for (const [k, v] of Object.entries(state.presets[tr.preset] || {})) vals[k] = v;
  for (const [k, v] of Object.entries(tr.params || {})) vals[k] = v;
  return state.paramInfo.map((p) => [p.id, vals[p.name]]);
}
function paramValue(tr, name) { const f = fullParams(tr); const id = state.paramIds[name]; return f.find((x) => x[0] === id)[1]; }

function scheduleSave() {
  clearTimeout(state.saveTimer);
  state.saveTimer = setTimeout(() => { try { localStorage.setItem('gen1-studio-project', JSON.stringify(state.project)); } catch (e) {} }, 300);
}
function loadSaved() {
  try { const s = localStorage.getItem('gen1-studio-project'); if (s) { const p = JSON.parse(s); if (p && p.tracks && p.tracks.length === N_TRACKS) return p; } } catch (e) {}
  return null;
}

// ---------------------------------------------------------------- audio
function send(m) { if (state.node) state.node.port.postMessage(m); }

async function startAudio() {
  const ctx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
  await ctx.audioWorklet.addModule('seq-worklet.js');
  const node = new AudioWorkletNode(ctx, 'absynth-seq', {
    numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
    processorOptions: { module: state.module, sampleRate: ctx.sampleRate, tracks: N_TRACKS },
  });
  const gain = ctx.createGain(); gain.gain.value = 1;
  node.connect(gain).connect(ctx.destination);
  node.port.onmessage = (e) => onWorklet(e.data);
  state.ctx = ctx; state.node = node; state.gain = gain;
  await ctx.resume();
  pushProject();
}

function pushProject() {
  const p = state.project;
  send({ type: 'transport', bpm: p.bpm });
  send({ type: 'master', gain: p.master });
  send({ type: 'budget', factor: p.factor, simulate: p.simulate });
  p.tracks.forEach((tr, t) => pushTrack(t));
}
function pushTrack(t) {
  const tr = state.project.tracks[t];
  send({ type: 'profile', track: t, profile: state.project.profile });
  send({ type: 'params', track: t, list: fullParams(tr) });
  send({ type: 'mix', track: t, gain: tr.gain, pan: tr.pan, mute: tr.mute, solo: tr.solo });
  send({ type: 'pattern', track: t, steps: tr.pattern.bars * STEPS_PER_BAR, notes: tr.pattern.notes });
  queueCalibration(t);
}

function onWorklet(m) {
  if (m.type === 'status') {
    state.status = m;
    const bar = Math.floor(m.step / STEPS_PER_BAR) + 1, beat = Math.floor((m.step % STEPS_PER_BAR) / 4) + 1;
    $('#position').textContent = m.playing ? `${bar}.${beat}` : '1.1';
    const pct = m.load * 100;
    const bar2 = $('#loadBar');
    bar2.style.width = Math.min(100, pct) + '%';
    bar2.className = pct > 100 ? 'over' : (pct > 80 ? 'warn' : '');
    $('#loadText').textContent = `Gen1 CPU ${pct.toFixed(0)} % of budget (${(m.est * 100).toFixed(1)} % of this core ×${state.project.factor} / 3 cores / 70 %)`;
    $('#dropText').textContent = `${m.dropouts} dropouts`;
    m.voices.forEach((v, t) => {
      const el = document.querySelector(`.track[data-t="${t}"]`); if (!el) return;
      el.querySelector('.v').textContent = `${v} voices`;
      el.querySelector('.stl').textContent = m.steals[t] ? `${m.steals[t]} stolen` : '';
      el.querySelector('.meter > div').style.width = Math.min(100, m.peaks[t] * 100) + '%';
    });
    drawPlayhead(m);
  } else if (m.type === 'recorded') {
    const tr = state.project.tracks[m.track];
    tr.pattern.notes = tr.pattern.notes.filter((n) => !(n.pitch === m.note.pitch && n.step === m.note.step));
    tr.pattern.notes.push(m.note);
    send({ type: 'pattern', track: m.track, steps: tr.pattern.bars * STEPS_PER_BAR, notes: tr.pattern.notes });
    if (m.track === state.sel) drawRoll();
    scheduleSave();
  }
}

// ---------------------------------------------------------------- CPU calibration (per track, offline worker)
let calibWorker = null;
function queueCalibration(t) {
  if (!state.calibQueue.includes(t)) state.calibQueue.push(t);
  runCalibration();
}
function renderOffline(msg) {
  return new Promise((resolve) => {
    if (!calibWorker) calibWorker = new Worker('export-worker.js');
    const id = ++state.calibSeq;
    const h = (e) => { if (e.data.id !== id) return; calibWorker.removeEventListener('message', h); resolve(e.data); };
    calibWorker.addEventListener('message', h);
    calibWorker.postMessage(Object.assign({ id, module: state.module }, msg));
  });
}
async function runCalibration() {
  if (state.calibBusy || !state.ctx) return;
  state.calibBusy = true;
  while (state.calibQueue.length) {
    const t = state.calibQueue.shift();
    const tr = state.project.tracks[t];
    const sr = state.ctx.sampleRate, secs = 1.5, total = Math.round(secs * sr);
    const track = { params: fullParams(tr), gain: 1, pan: 0, profile: state.project.profile };
    // events MUST be time-sorted: the worker stops scanning at the first future event
    const mk = (nNotes) => { const ev = []; for (let i = 0; i < nNotes; i++) ev.push({ t: 0, track: 0, pitch: 48 + i * 3, vel: 0.8, on: true }); for (let i = 0; i < nNotes; i++) ev.push({ t: total - 100, track: 0, pitch: 48 + i * 3, vel: 0.8, on: false }); return ev; };
    const r1 = await renderOffline({ sampleRate: sr, tracks: [track], master: 1, events: mk(1), lengthSamples: total, wantAudio: false });
    const r8 = await renderOffline({ sampleRate: sr, tracks: [track], master: 1, events: mk(8), lengthSamples: total, wantAudio: false });
    const l1 = r1.renderSeconds / secs, l8 = r8.renderSeconds / secs;
    const perVoice = Math.max(0, (l8 - l1) / 7), fixed = Math.max(0, l1 - perVoice);
    send({ type: 'costs', track: t, fixed, perVoice });
    const el = document.querySelector(`.track[data-t="${t}"] .cost`);
    if (el) el.textContent = `${(fixed * 100).toFixed(2)} + ${(perVoice * 100).toFixed(2)} %/voice`;
  }
  state.calibBusy = false;
}

// ---------------------------------------------------------------- track strips
function buildTracks() {
  const host = $('#tracks'); host.innerHTML = '';
  state.project.tracks.forEach((tr, t) => {
    const el = document.createElement('div');
    el.className = 'track' + (t === state.sel ? ' sel' : ''); el.dataset.t = t;
    el.innerHTML = `
      <div class="top"><span class="num">${t + 1}</span><select class="preset"></select></div>
      <div class="mix"><input type="range" class="vol" min="0" max="1.5" step="0.01" value="${tr.gain}"><input type="range" class="pan" min="-1" max="1" step="0.05" value="${tr.pan}"><button class="mb m${tr.mute ? ' on' : ''}">M</button><button class="mb s${tr.solo ? ' on' : ''}">S</button></div>
      <div class="meter bar"><div></div></div>
      <div class="voices"><span class="v">0 voices</span><span class="stl"></span><span class="cost"></span></div>`;
    const sel = el.querySelector('.preset');
    state.presetNames.forEach((n) => { const o = document.createElement('option'); o.value = n; o.textContent = n; sel.appendChild(o); });
    sel.value = tr.preset;
    sel.onchange = () => { tr.preset = sel.value; tr.params = {}; send({ type: 'params', track: t, list: fullParams(tr) }); queueCalibration(t); if (t === state.sel) buildParams(); scheduleSave(); };
    el.querySelector('.vol').oninput = (e) => { tr.gain = +e.target.value; sendMix(t); scheduleSave(); };
    el.querySelector('.pan').oninput = (e) => { tr.pan = +e.target.value; sendMix(t); scheduleSave(); };
    el.querySelector('.m').onclick = (e) => { tr.mute = !tr.mute; e.target.classList.toggle('on', tr.mute); sendMix(t); scheduleSave(); };
    el.querySelector('.s').onclick = (e) => { tr.solo = !tr.solo; e.target.classList.toggle('on', tr.solo); sendMix(t); scheduleSave(); };
    el.addEventListener('pointerdown', (e) => { if (e.target.closest('select,input,button')) return; selectTrack(t); });
    host.appendChild(el);
  });
}
function sendMix(t) { const tr = state.project.tracks[t]; send({ type: 'mix', track: t, gain: tr.gain, pan: tr.pan, mute: tr.mute, solo: tr.solo }); }
function selectTrack(t) {
  state.sel = t;
  document.querySelectorAll('.track').forEach((el) => el.classList.toggle('sel', +el.dataset.t === t));
  const tr = state.project.tracks[t];
  $('#trackTitle').textContent = `Track ${t + 1} · ${tr.preset}`;
  document.querySelectorAll('#barsSeg button').forEach((b) => b.classList.toggle('on', +b.dataset.bars === tr.pattern.bars));
  drawRoll(); buildParams();
}

// ---------------------------------------------------------------- piano roll
const CELL_W = 34, CELL_H = 26, ROWS = 24;   // finger-sized cells on an 11" iPad
function rollDims() { const tr = state.project.tracks[state.sel]; return { steps: tr.pattern.bars * STEPS_PER_BAR, w: tr.pattern.bars * STEPS_PER_BAR * CELL_W + 36, h: ROWS * CELL_H }; }
function drawRoll() {
  const c = $('#roll'); const d = rollDims();
  const dpr = window.devicePixelRatio || 1;
  c.width = d.w * dpr; c.height = d.h * dpr; c.style.width = d.w + 'px'; c.style.height = d.h + 'px';
  const g = c.getContext('2d'); g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.fillStyle = '#0b0d11'; g.fillRect(0, 0, d.w, d.h);
  const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  for (let r = 0; r < ROWS; r++) {
    const pitch = state.rollBase + ROWS - 1 - r;
    const black = [1, 3, 6, 8, 10].includes(pitch % 12);
    g.fillStyle = black ? '#111319' : '#161922';
    g.fillRect(36, r * CELL_H, d.w - 36, CELL_H);
    g.fillStyle = black ? '#1b1d24' : '#e8eaf0'; g.fillRect(0, r * CELL_H, 34, CELL_H - 1);
    if (pitch % 12 === 0) { g.fillStyle = '#556'; g.font = '11px system-ui'; g.fillText('C' + (pitch / 12 - 1), 6, r * CELL_H + 15); }
  }
  for (let s = 0; s <= d.steps; s++) {
    g.strokeStyle = s % 16 === 0 ? '#4a5068' : (s % 4 === 0 ? '#2b3040' : '#1c1f28');
    g.beginPath(); g.moveTo(36 + s * CELL_W, 0); g.lineTo(36 + s * CELL_W, d.h); g.stroke();
  }
  const tr = state.project.tracks[state.sel];
  for (const n of tr.pattern.notes) {
    const r = state.rollBase + ROWS - 1 - n.pitch; if (r < 0 || r >= ROWS) continue;
    g.fillStyle = `rgba(78,161,255,${0.45 + 0.55 * n.vel})`;
    g.fillRect(37 + n.step * CELL_W, r * CELL_H + 2, Math.max(1, n.len) * CELL_W - 3, CELL_H - 4);
  }
  drawPlayhead(state.status);
}
function drawPlayhead(m) {
  const ph = $('#rollWrap');
  if (!ph._line) { const l = document.createElement('div'); l.style.cssText = 'position:absolute;top:0;width:2px;height:100%;background:#ff8a4e;pointer-events:none;display:none'; ph.appendChild(l); ph._line = l; }
  const d = rollDims();
  if (!m || !m.playing) { ph._line.style.display = 'none'; return; }
  ph._line.style.display = 'block';
  ph._line.style.left = (36 + (m.step % d.steps) * CELL_W) + 'px';
  ph._line.style.height = d.h + 'px';
}
function rollTap(ev) {
  const c = $('#roll'); const rect = c.getBoundingClientRect();
  const x = ev.clientX - rect.left - 36, y = ev.clientY - rect.top;
  if (x < 0) return;
  const step = Math.floor(x / CELL_W), r = Math.floor(y / CELL_H);
  const pitch = state.rollBase + ROWS - 1 - r;
  const tr = state.project.tracks[state.sel];
  const hit = tr.pattern.notes.findIndex((n) => n.pitch === pitch && step >= n.step && step < n.step + n.len);
  if (hit >= 0) tr.pattern.notes.splice(hit, 1);
  else tr.pattern.notes.push({ step, len: state.noteLen, pitch, vel: state.vel });
  send({ type: 'pattern', track: state.sel, steps: tr.pattern.bars * STEPS_PER_BAR, notes: tr.pattern.notes });
  drawRoll(); scheduleSave();
}

// ---------------------------------------------------------------- patch panel
function buildParams() {
  const host = $('#params'); host.innerHTML = '';
  const tr = state.project.tracks[state.sel];
  const vals = Object.fromEntries(fullParams(tr).map(([id, v]) => [state.paramInfo[id].name, v]));
  const presetSel = $('#preset'); presetSel.innerHTML = '';
  state.presetNames.forEach((n) => { const o = document.createElement('option'); o.value = n; o.textContent = n; presetSel.appendChild(o); });
  presetSel.value = tr.preset;
  presetSel.onchange = () => { tr.preset = presetSel.value; tr.params = {}; send({ type: 'params', track: state.sel, list: fullParams(tr) }); queueCalibration(state.sel); buildTracks(); selectTrack(state.sel); scheduleSave(); };
  for (const p of state.paramInfo) {
    const log = p.min > 0 && p.max / p.min >= 100;
    const toSlider = (v) => log ? Math.log(v / p.min) / Math.log(p.max / p.min) : (v - p.min) / (p.max - p.min);
    const fromSlider = (s) => log ? p.min * Math.pow(p.max / p.min, s) : p.min + s * (p.max - p.min);
    const l = document.createElement('label');
    l.innerHTML = `<span>${p.name}</span><input type="range" min="0" max="1" step="0.001"><span class="val"></span>`;
    const inp = l.querySelector('input'), val = l.querySelector('.val');
    inp.value = toSlider(vals[p.name]); val.textContent = fmt(vals[p.name]);
    inp.oninput = () => {
      let v = fromSlider(+inp.value);
      if (['osc1Wave', 'osc2Wave', 'unisonVoices', 'lfo1Shape', 'lfo2Shape', 'mono', 'osc2Semi'].includes(p.name)) v = Math.round(v);
      tr.params[p.name] = v; val.textContent = fmt(v);
      send({ type: 'param', track: state.sel, id: p.id, value: v });
      scheduleSave(); clearTimeout(state.calibTimer); state.calibTimer = setTimeout(() => queueCalibration(state.sel), 800);
    };
    host.appendChild(l);
  }
}
function fmt(v) { return Math.abs(v) >= 100 ? v.toFixed(0) : (Math.abs(v) >= 10 ? v.toFixed(1) : v.toFixed(3)); }

// ---------------------------------------------------------------- keyboard
function buildKeyboard() {
  const kb = $('#keys'); kb.innerHTML = '';
  for (let i = 0; i < 25; i++) {
    const n = 12 * state.octave + i;
    const k = document.createElement('div');
    k.className = 'key' + ([1, 3, 6, 8, 10].includes(i % 12) ? ' black' : '');
    k.dataset.note = n;
    if (i % 12 === 0) k.textContent = 'C' + (state.octave - 1 + i / 12);
    kb.appendChild(k);
  }
  const down = (ev) => { const el = ev.target.closest('.key'); if (!el) return; ev.preventDefault(); const note = +el.dataset.note; if (state.held.has(note)) return; state.held.add(note); el.classList.add('down'); send({ type: 'noteOn', track: state.sel, pitch: note, vel: state.vel }); };
  const up = (ev) => { const el = ev.target.closest('.key'); if (!el) return; const note = +el.dataset.note; if (!state.held.has(note)) return; state.held.delete(note); el.classList.remove('down'); send({ type: 'noteOff', track: state.sel, pitch: note }); };
  kb.onpointerdown = down; kb.onpointerup = up; kb.onpointercancel = up;
  kb.onpointerout = (ev) => { if (ev.target.classList && ev.target.classList.contains('key')) up(ev); };
}

// ---------------------------------------------------------------- transport + files
function setPlaying(on) {
  state.playing = on;
  send({ type: 'transport', playing: on, bpm: state.project.bpm });
  $('#play').classList.toggle('on', on);
  if (!on) { setRecording(false); }
}
function setRecording(on) { state.recording = on; send({ type: 'transport', record: on }); $('#rec').classList.toggle('on', on); }

function loopEvents(loops, sr) {
  const p = state.project; const sps = sr * 60 / p.bpm / 4;
  const loopSteps = Math.max(...p.tracks.map((t) => t.pattern.bars)) * STEPS_PER_BAR;
  const ev = [];
  for (let s = 0; s < loopSteps * loops; s++) {
    p.tracks.forEach((tr, t) => {
      const steps = tr.pattern.bars * STEPS_PER_BAR;
      for (const n of tr.pattern.notes) if (n.step === s % steps) {
        ev.push({ t: Math.round(s * sps), track: t, pitch: n.pitch, vel: n.vel, on: true });
        ev.push({ t: Math.round((s + Math.max(1, n.len)) * sps) - 2, track: t, pitch: n.pitch, vel: n.vel, on: false });
      }
    });
  }
  ev.sort((a, b) => a.t - b.t || (a.on === b.on ? 0 : a.on ? 1 : -1));
  return { events: ev, lengthSamples: Math.round(loopSteps * loops * sps) + Math.round(2 * sr) };
}
async function exportWav() {
  if (!state.ctx) return;
  const btn = $('#exportWav'); btn.textContent = 'rendering…'; btn.disabled = true;
  const sr = state.ctx.sampleRate;
  const { events, lengthSamples } = loopEvents(2, sr);
  const p = state.project;
  const r = await renderOffline({ sampleRate: sr, master: p.master, events, lengthSamples, wantAudio: true,
    tracks: p.tracks.map((tr) => ({ params: fullParams(tr), gain: tr.gain, pan: tr.pan, mute: tr.mute, solo: tr.solo, profile: p.profile })) });
  const n = r.L.length, buf = new ArrayBuffer(44 + n * 4), v = new DataView(buf);
  const str = (o, s) => { for (let i = 0; i < s.length; i++) v.setUint8(o + i, s.charCodeAt(i)); };
  str(0, 'RIFF'); v.setUint32(4, 36 + n * 4, true); str(8, 'WAVE'); str(12, 'fmt '); v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 2, true);
  v.setUint32(24, sr, true); v.setUint32(28, sr * 4, true); v.setUint16(32, 4, true); v.setUint16(34, 16, true); str(36, 'data'); v.setUint32(40, n * 4, true);
  for (let i = 0; i < n; i++) { v.setInt16(44 + i * 4, Math.max(-1, Math.min(1, r.L[i])) * 32767, true); v.setInt16(46 + i * 4, Math.max(-1, Math.min(1, r.R[i])) * 32767, true); }
  const a = document.createElement('a'); a.href = URL.createObjectURL(new Blob([buf], { type: 'audio/wav' })); a.download = `gen1-studio-${p.profile ? 'gen1' : 'full'}.wav`; a.click();
  btn.textContent = `export .wav (${(r.seconds / r.renderSeconds).toFixed(0)}× rt)`; btn.disabled = false;
}

function wire() {
  $('#start').onclick = async () => {
    $('#start').textContent = 'starting…';
    try { await startAudio(); $('#overlay').classList.add('hidden'); } catch (e) { $('#start').textContent = 'Failed: ' + e.message; console.error(e); }
  };
  $('#play').onclick = () => setPlaying(!state.playing);
  $('#stop').onclick = () => setPlaying(false);
  $('#rec').onclick = () => { if (!state.playing) setPlaying(true); setRecording(!state.recording); };
  $('#bpmDown').onclick = () => setBpm(state.project.bpm - 1);
  $('#bpmUp').onclick = () => setBpm(state.project.bpm + 1);
  $('#metro').onchange = (e) => send({ type: 'transport', metronome: e.target.checked });
  $('#simulate').onchange = (e) => { state.project.simulate = e.target.checked; send({ type: 'budget', simulate: e.target.checked }); scheduleSave(); };
  $('#factor').onchange = (e) => { state.project.factor = Math.max(1, +e.target.value || 20); send({ type: 'budget', factor: state.project.factor }); scheduleSave(); };
  $('#profileAll').onchange = (e) => { state.project.profile = +e.target.value; state.project.tracks.forEach((_, t) => { send({ type: 'profile', track: t, profile: state.project.profile }); queueCalibration(t); }); scheduleSave(); };
  $('#master').oninput = (e) => { state.project.master = +e.target.value; send({ type: 'master', gain: state.project.master }); scheduleSave(); };
  $('#panic').onclick = () => { send({ type: 'panic' }); state.held.clear(); document.querySelectorAll('.key.down').forEach((k) => k.classList.remove('down')); };
  document.querySelectorAll('#lenSeg button').forEach((b) => b.onclick = () => { state.noteLen = +b.dataset.len; document.querySelectorAll('#lenSeg button').forEach((x) => x.classList.toggle('on', x === b)); });
  $('#vel').oninput = (e) => { state.vel = +e.target.value; };
  document.querySelectorAll('#barsSeg button').forEach((b) => b.onclick = () => {
    const tr = state.project.tracks[state.sel]; tr.pattern.bars = +b.dataset.bars;
    const steps = tr.pattern.bars * STEPS_PER_BAR; tr.pattern.notes = tr.pattern.notes.filter((n) => n.step < steps);
    send({ type: 'pattern', track: state.sel, steps, notes: tr.pattern.notes });
    document.querySelectorAll('#barsSeg button').forEach((x) => x.classList.toggle('on', x === b)); drawRoll(); scheduleSave();
  });
  $('#rollUp').onclick = () => { state.rollBase = Math.min(96, state.rollBase + 12); drawRoll(); };
  $('#rollDown').onclick = () => { state.rollBase = Math.max(12, state.rollBase - 12); drawRoll(); };
  $('#clearPat').onclick = () => { const tr = state.project.tracks[state.sel]; tr.pattern.notes = []; send({ type: 'pattern', track: state.sel, steps: tr.pattern.bars * STEPS_PER_BAR, notes: [] }); drawRoll(); scheduleSave(); };
  $('#patchToggle').onclick = () => { $('#patchPanel').classList.toggle('hidden'); };
  $('#copyPatch').onclick = () => { const tr = state.project.tracks[state.sel]; state.clipboard = { preset: tr.preset, params: Object.assign({}, tr.params) }; };
  $('#pastePatch').onclick = () => { if (!state.clipboard) return; const tr = state.project.tracks[state.sel]; tr.preset = state.clipboard.preset; tr.params = Object.assign({}, state.clipboard.params); send({ type: 'params', track: state.sel, list: fullParams(tr) }); queueCalibration(state.sel); buildTracks(); selectTrack(state.sel); scheduleSave(); };
  $('#roll').addEventListener('click', rollTap);
  $('#octDown').onclick = () => { state.octave = Math.max(1, state.octave - 1); buildKeyboard(); };
  $('#octUp').onclick = () => { state.octave = Math.min(7, state.octave + 1); buildKeyboard(); };
  $('#new').onclick = () => { if (!confirm('Start a new empty project? The current one is replaced.')) return; state.project = defaultProject(); state.project.tracks.forEach((t) => { t.pattern.notes = []; }); applyProjectToUi(); pushProject(); scheduleSave(); };
  $('#save').onclick = () => { const a = document.createElement('a'); a.href = URL.createObjectURL(new Blob([JSON.stringify(state.project, null, 1)], { type: 'application/json' })); a.download = 'gen1-studio-project.json'; a.click(); };
  $('#load').onclick = () => $('#loadFile').click();
  $('#loadFile').onchange = async (e) => { const f = e.target.files[0]; if (!f) return; try { const p = JSON.parse(await f.text()); if (p && p.tracks && p.tracks.length === N_TRACKS) { state.project = p; applyProjectToUi(); pushProject(); scheduleSave(); } } catch (err) { alert('Not a project file'); } e.target.value = ''; };
  $('#exportWav').onclick = exportWav;
  buildKeyboard();
}
function setBpm(b) { state.project.bpm = Math.max(40, Math.min(240, b)); $('#bpm').textContent = state.project.bpm; send({ type: 'transport', bpm: state.project.bpm }); scheduleSave(); }
function applyProjectToUi() {
  const p = state.project;
  $('#bpm').textContent = p.bpm; $('#master').value = p.master; $('#factor').value = p.factor; $('#simulate').checked = p.simulate; $('#profileAll').value = String(p.profile == null ? 1 : p.profile);
  buildTracks(); selectTrack(Math.min(state.sel, N_TRACKS - 1));
}

(async () => {
  try {
    await Promise.all([loadWasm(), loadPresets()]);
    state.project = loadSaved() || defaultProject();
    if (state.project.profile == null) state.project.profile = 1;
    wire(); applyProjectToUi();
    $('#start').disabled = false; $('#start').textContent = 'Tap to start';
  } catch (e) { $('#start').textContent = 'Load failed: ' + e.message; console.error(e); }
})();
