# AB Synth — Gen 1 MPC vs iPad M5 quality demo

One synth engine, two budgets. **A ("Full")** runs with an iPad M5-class budget. **B ("Gen1")** is the
*same code* limited the way an RK3288 MPC Key 37 Gen 1 would force it. The web app crossfades between
them in 10 ms, scores you blind (ABX), and projects per-patch CPU cost onto the Gen 1.

See [PLAN.md](PLAN.md) for the full plan. This repo implements **Phase 1** (core + web A/B) and the
measurement hooks that Phase 2 builds on.

## Layout

```
dsp/include/absynth/   portable C++17 DSP core (header-only engine + flat C ABI), no platform headers
dsp/src/c_api.cpp      the C ABI compiled into the static lib and the wasm module
dsp/tests/             native unit tests + offline render tool (MSVC / Clang / GCC)
web/                   standalone wasm + AudioWorklet + UI (plain ES modules, no bundler)
patches/               the 5 test patches (params + stored MIDI phrase), JSON
renders/               offline WAV output (A, B and the latency-aligned difference)
```

## Build (Windows, MSVC)

```
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release     # from a vcvars64 shell
cmake --build build
build\dsp\tests\absynth_tests.exe patches
build\dsp\tests\absynth_render.exe patches renders
```

## Build the web app

```
powershell -File web\build-wasm.ps1 -Emsdk C:\path\to\emsdk      # writes web/absynth.wasm, copies patches
python -m http.server 8765 --directory web                        # local test (Chrome; Safari needs HTTPS)
```

Deployed from the `web/` folder via GitHub Pages.

## What the Gen1 profile changes (each individually toggleable)

| Constraint | Full (A) | Gen1 (B) |
|---|---|---|
| Max polyphony | 32 | 8, oldest-note stealing |
| Max unison | 7 | 3 |
| Oversampling (ladder + drive) | 4× polyphase FIR | 1× |
| Modulation rate | per sample | per 32-sample block, linearly interpolated |
| Reverb | 16-line modulated FDN | Schroeder / Freeverb-lite (4 comb + 2 allpass) |
| Delay interpolation | cubic | linear |

## 8-track session stress test

`patches/session_8track.json` defines eight tracks (two supersaw pads, two ambient layers, two pluck
stab tracks, a lead stack, a poly bass), every one playing 6-note chords with overlapping releases, up to
48 notes sounding. Native: `absynth_session patches renders session_8track [factor]` renders the summed
session through both profiles (`renders/session_8track_{A_full,B_gen1,diff}.wav`) and prints per-track
cost, peak voices, steals and the RK3288 projection. Web: **Render session A + B** renders the same thing
in parallel workers and A/Bs the two mixes by crossfading buffers (16 live engines would not fit in one
audio thread), so Blind X works on the whole arrangement.

First native numbers (this laptop, one core, factor 20): A needs 214 % of a core, B 20 %; B steals
364 notes in 13 s; projected Gen1 load is 194 % of the 70 % budget on 3 cores, so about 4 such tracks fit.

## Measured so far (native, 48 kHz)

Unit tests (`absynth_tests`): 4-point polyBLEP alias floor −37.6 dB (naive saw −13.1 dB); ladder stable at
max resonance and self-oscillates; oversampler round trip −75 dB at exactly 24 samples latency; no NaN or
denormals through all five patches in both profiles; Dual crossfade adds no discontinuity.

Control-bass null (A vs B, latency aligned): −37.6 dB with every Gen1 constraint on, −48 dB with only the
block-rate modulation, −35 dB with only the 1× filter. The residual sits at note onsets where the ladder's
cutoff sweeps fastest; identical configurations null to −inf.
