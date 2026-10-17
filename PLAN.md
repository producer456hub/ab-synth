# AB Synth — Gen 1 MPC vs iPad M5 Quality Demo

## Goal
Build a single synth engine that runs in two modes and can be switched instantly on the iPad Pro M5:

- **A: "Full"** (M5-class budget)
- **B: "Gen1"** (the same engine limited the way an RK3288 MPC Key 37 Gen 1 would force it to be)

The point is to hear *exactly* what the Gen 1 compute ceiling costs in sound quality, and to measure per-voice CPU cost so it can be projected onto the RK3288.

## Platform decision (read first)
- A native iPad app or AUv3 needs **Xcode on a Mac**. The dev machine here is a **Surface Pro (Windows)**, so there's no Mac.
- **Phase 1 target: a web app.** The DSP core is C++ compiled to WebAssembly and runs in an `AudioWorklet`. It is built on Windows, hosted over HTTPS (GitHub Pages), and opened in Safari on the iPad.
- **Limitation:** Safari has **no Web MIDI**, so input comes from the on-screen keyboard and a built-in phrase player. That's actually better for fair A/B tests, because every take plays identically.
- **Phase 3 (optional):** wrap the same C++ core in a native AUv3 if a Mac (local or cloud) becomes available. A later port to the Gen 1 MPC (armv7hf Linux, ALSA) reuses the same core unchanged.

## Architecture
```
/dsp         portable C++17 DSP core (no platform headers, no allocation on audio path)
/dsp/tests   native unit tests + offline render tool (builds on Windows/Linux)
/web         Emscripten build, AudioWorklet processor, UI (vanilla TS or Svelte)
/patches     JSON patch definitions
/renders     offline WAV output for null tests / spectrum comparison
```
- The core exposes `prepare(sampleRate, maxBlock)`, `noteOn/noteOff`, `setParam`, `process(float** out, int n)`, and `setProfile(Full|Gen1)`.
- **Two engine instances run in parallel** (A and B), and the UI crossfades between them over 10 ms. Switching is seamless mid-note because both always render (the M5 can afford it).
- **Loudness matching:** run RMS/LUFS-ish auto gain on each instance so B is never judged by being quieter or louder.

## Engine (identical code for A and B)
- **Oscillators:** polyBLEP saw/square plus band-limited wavetable sine/tri. These are identical in both modes, because band-limiting is cheap and not where the Gen 1 loses.
- **Unison:** up to 7 voices with detune and stereo spread.
- **Filter:** ZDF 4-pole ladder with tanh saturation. This is the oversampled stage.
- **Drive:** waveshaper (also oversampled).
- **Envelopes:** 2× ADSR, exponential segments.
- **LFO:** 2 LFOs, rate up to audio range.
- **FX:** chorus, stereo delay, reverb.

## What the Gen1 profile changes
Each item has an **individual toggle** so the cause of any audible difference can be isolated.

| Parameter | Full (A) | Gen1 (B) |
|---|---|---|
| Max polyphony | 32 | 8 (oldest-note stealing) |
| Max unison | 7 | 3 |
| Oversampling (filter + drive) | 4× (polyphase halfband) | 1× |
| Modulation rate | per-sample | per 32-sample block, linear-interpolated |
| Reverb | 16-line modulated FDN | Schroeder/Freeverb-lite (4 comb + 2 allpass) |
| Delay interpolation | cubic | linear |

These are starting estimates. Phase 2 replaces them with measured values.

## Test patches (`/patches`)
1. **Control bass:** mono saw → filter, no drive, no reverb. A and B should be indistinguishable. If they're not, there's a bug.
2. **Supersaw pad:** 7-voice unison chords. Tests the polyphony and unison cuts.
3. **Screaming lead:** high resonance plus heavy drive, played high. Tests aliasing.
4. **Snappy pluck:** 1 ms attack with fast filter envelope. Tests block-rate modulation.
5. **Ambient tail:** long reverb on sparse notes. Tests reverb quality.

## Built-in phrase player
Each patch has a stored MIDI phrase (chords, high runs, fast plucks) so that A and B hear the identical performance. The on-screen keyboard is for free play.

## A/B UI (iPad-first, touch, landscape)
- Big **A / B** buttons plus a **Blind X** mode: the app randomly assigns X, you guess, and it scores after 10 trials (ABX test).
- Per-toggle switches from the table above, so you can hear one constraint at a time.
- **Gen1 budget meter** (see Phase 2): shows whether the current patch and voice count would overrun on the RK3288.
- Patch selector, play/stop phrase, master volume.

## Phase 1: core + web A/B (build first)
1. Scaffold the repo. Build `/dsp` as a native static lib plus a CLI test runner with CMake and MSVC/Clang on Windows.
2. Implement oscillators, unison, ladder filter, envelopes, LFOs, voice allocator. Write unit tests for:
   - polyBLEP aliasing (FFT check)
   - filter stability at max resonance
   - no NaN/denormals (flush-to-zero)
3. Implement oversampling (2×/4× halfband), the FDN and Schroeder reverbs, and chorus/delay.
4. Implement profile switching and individual toggles.
5. Build with Emscripten. Write the AudioWorklet processor with dual instances and crossfade, using a SharedArrayBuffer or message port for params.
6. Write the UI, phrase player, loudness matching, and ABX mode.
7. Deploy to GitHub Pages (HTTPS is required for AudioWorklet). Verify it on the iPad in Safari: tap-to-start audio, check for no glitches at 128-frame quantum.

**Done when:** all 5 patches play on the iPad with seamless A/B, the control bass nulls in an offline render, and ABX mode works.

## Phase 2: measurement + Gen1 projection
1. **Offline benchmark mode** (in a Web Worker, and natively in the CLI):
   - Render 10 s of each patch at fixed polyphony.
   - Report the realtime factor and the **ns per voice per sample** for each processing stage: oscillators, filter at 1×/4×, reverb types.
   - Don't time inside the AudioWorklet; Safari timing there is unreliable.
2. **Gen1 projection:** estimated RK3288 load = measured M5 cost × `factor`.
   - `factor` defaults to 20 and is user-editable.
   - It gets replaced by a real number once the native CLI benchmark runs on actual Gen 1 hardware (armv7hf, `-mfpu=neon-vfpv4 -mfloat-abi=hard`).
   - Show the per-patch budget against a 70% CPU target across 3 usable cores.
3. **Offline render tool:** export A and B WAVs for each patch to `/renders`, plus a difference (null) file and a spectrogram PNG, so the differences are visible as well as audible.
4. Re-tune the Gen1 profile defaults from the benchmark: the highest voice, unison and oversampling settings that fit the budget.

**Done when:** each patch shows a measured Full cost, a projected Gen1 cost, and the max voices that fit.

## Phase 3 (optional)
- Native AUv3 wrapper around `/dsp` (needs Xcode), with CoreMIDI so a hardware keyboard or MPC can play it.
- Linux/ALSA host for the Gen 1 MPC: same core, rawmidi for pads and keys, run via the SD bootstrap.

## Rules for the implementation
- No allocation, locks, logging or exceptions on the audio thread.
- Enable flush-to-zero/denormals-are-zero. Use float32 throughout; this matches the NEON constraint on the Gen 1.
- The DSP core must compile with `-std=c++17` using no platform headers. That keeps the Gen 1 port free.
- Parameter smoothing everywhere, so the only zipper noise heard is the intentional block-rate test.
- Keep A and B on identical code paths. The only differences allowed are those listed in the Gen1 profile table.
