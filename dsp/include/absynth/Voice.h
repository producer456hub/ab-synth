// Voice.h - one polyphonic voice: unison oscillators -> (oversampled) ladder + drive -> amp.
// The SAME code runs for both profiles; Limits only change modBlock, oversample and unison count.
#pragma once
#include "Common.h"
#include "Params.h"
#include "Profile.h"
#include "Oscillator.h"
#include "Filter.h"
#include "Oversampler.h"
#include "Envelope.h"

namespace absynth {

struct Voice {
    // state
    int   note = -1;
    bool  held = false;
    uint32_t age = 0;           // allocation order, for oldest-note stealing
    float velocity = 0.0f;
    float sr = 48000.0f;

    Oscillator osc1[kMaxUnison], osc2[kMaxUnison];
    float pan[kMaxUnison] = {};
    float detuneRatio[kMaxUnison] = {};
    int   unisonN = 1;

    ADSR ampEnv, filtEnv;
    LFO  lfo1, lfo2;
    Ladder ladderL, ladderR;
    Drive  drive;
    Oversampler osL, osR;
    int   osFactor = 1;
    int   modBlock = 1;

    // glide
    float pitchCur = 60.0f, pitchTarget = 60.0f, glideCoef = 0.0f;

    // per-chunk ramps
    bool  snapRamps = true;
    float gCur = 0.1f, ampCur = 0.0f, inGainCur = 1.0f;
    float incCur1[kMaxUnison] = {}, incCur2[kMaxUnison] = {};

    void prepare(float sampleRate) {
        sr = sampleRate;
        for (int i = 0; i < kMaxUnison; ++i) { osc1[i].prepare(); osc2[i].prepare(); }
        ampEnv.prepare(sr); filtEnv.prepare(sr);
        lfo1.rng.s = 0x1234567u; lfo2.rng.s = 0x89ABCDEu;
        osL.setFactor(1); osR.setFactor(1);
        reset();
    }

    void reset() {
        note = -1; held = false; velocity = 0.0f;
        ampEnv.reset(); filtEnv.reset();
        ladderL.reset(); ladderR.reset();
        osL.reset(); osR.reset();
        lfo1.reset(); lfo2.reset();
        gCur = 0.1f; ampCur = 0.0f; inGainCur = 1.0f;
    }

    bool active() const { return ampEnv.active(); }

    void applyLimits(const Limits& lim) {
        modBlock = lim.modBlock;
        if (lim.oversample != osFactor) {
            osFactor = lim.oversample;
            osL.setFactor(osFactor); osR.setFactor(osFactor);
        }
    }

    void setUnison(int n, float detuneCents, float spread) {
        unisonN = std::max(1, std::min(n, kMaxUnison));
        for (int i = 0; i < unisonN; ++i) {
            const float pos = (unisonN == 1) ? 0.0f : ((float)i / (float)(unisonN - 1)) * 2.0f - 1.0f;
            detuneRatio[i] = std::exp2(pos * detuneCents * 0.5f / 1200.0f);
            pan[i] = pos * spread;
        }
    }

    void start(int n, float vel, bool legato, float glideSeconds) {
        const bool wasActive = active();
        note = n; held = true; velocity = vel;
        pitchTarget = (float)n;
        if (!wasActive || glideSeconds <= 0.0f) pitchCur = pitchTarget;
        glideCoef = (glideSeconds > 0.0f) ? std::exp(-1.0f / (glideSeconds * sr)) : 0.0f;
        if (!(legato && wasActive)) {
            ampEnv.gate(true); filtEnv.gate(true);
            lfo1.reset(); lfo2.reset();
            if (!wasActive) {
                for (int i = 0; i < kMaxUnison; ++i) {
                    osc1[i].reset((float)i / (float)kMaxUnison);
                    osc2[i].reset((float)(i + 3) / (float)kMaxUnison);
                }
                ladderL.reset(); ladderR.reset();
                osL.reset(); osR.reset();
                ampCur = 0.0f;
                snapRamps = true;      // a fresh voice starts AT its targets, it does not ramp from the old note
            }
        }
    }

    void release() { held = false; ampEnv.gate(false); filtEnv.gate(false); }
    void kill() { reset(); }

    // Render n samples, ADDING into outL/outR. p[id][i] are the per-sample smoothed parameter curves.
    void render(float* outL, float* outR, int n, const float* const* p) {
        if (!active()) return;
        float tmpL[kMaxOversample], tmpR[kMaxOversample];
        const float fsOS = sr * (float)osFactor;
        int pos = 0;
        while (pos < n) {
            const int k = std::min(modBlock, n - pos);
            // ---- modulation update (once per chunk; chunk == 1 sample in the Full profile)
            const float ae = ampEnv.advance(k);
            const float fe = filtEnv.advance(k);
            const float l1 = lfo1.advance(k);
            const float l2 = lfo2.advance(k);
            if (glideCoef > 0.0f) {
                const float c = (k == 1) ? glideCoef : std::pow(glideCoef, (float)k);
                pitchCur = pitchTarget + (pitchCur - pitchTarget) * c;
            } else pitchCur = pitchTarget;

            const float semis = pitchCur + l1 * p[pLfo1ToPitch][pos];
            const float hz1 = noteToHz(semis);
            const float hz2 = noteToHz(semis + p[pOsc2Semi][pos] + p[pOsc2Fine][pos] * 0.01f);
            const float cutMod = fe * p[pFilterEnvAmt][pos] + l1 * p[pLfo1ToCutoff][pos] + l2 * p[pLfo2ToCutoff][pos]
                               + p[pFilterKeyTrack][pos] * (pitchCur - 60.0f);
            const float cutoff = clampf(p[pCutoff][pos] * std::exp2(cutMod * (1.0f / 12.0f)), 20.0f, 0.45f * fsOS);
            const float trem = 1.0f - p[pLfo2ToAmp][pos] * 0.5f * (1.0f - l2);
            const float ampTarget = ae * velocity * trem * (1.0f / std::sqrt((float)unisonN));
            ladderL.setCoefs(cutoff, p[pResonance][pos], fsOS);
            ladderR.g = ladderL.g; ladderR.k = ladderL.k;
            const float gTarget = ladderL.g;
            drive.set(p[pDrive][pos], p[pDriveMix][pos]);
            const float inGainTarget = p[pFilterInputGain][pos];
            const int wave1 = (int)p[pOsc1Wave][pos], wave2 = (int)p[pOsc2Wave][pos];
            const float lvl1 = p[pOsc1Level][pos], lvl2 = p[pOsc2Level][pos];

            // ---- ramps across the chunk (k == 1 means "jump to target")
            if (snapRamps) {
                snapRamps = false;
                gCur = gTarget; inGainCur = inGainTarget;
                for (int u = 0; u < unisonN; ++u) {
                    incCur1[u] = clampf(hz1 * detuneRatio[u] / sr, 0.0f, 0.49f);
                    incCur2[u] = clampf(hz2 * detuneRatio[u] / sr, 0.0f, 0.49f);
                }
            }
            const float invK = 1.0f / (float)k;
            const float gStep = (gTarget - gCur) * invK;
            const float ampStep = (ampTarget - ampCur) * invK;
            const float inGainStep = (inGainTarget - inGainCur) * invK;
            float incStep1[kMaxUnison], incStep2[kMaxUnison];
            for (int u = 0; u < unisonN; ++u) {
                const float t1 = clampf(hz1 * detuneRatio[u] / sr, 0.0f, 0.49f);
                const float t2 = clampf(hz2 * detuneRatio[u] / sr, 0.0f, 0.49f);
                incStep1[u] = (t1 - incCur1[u]) * invK;
                incStep2[u] = (t2 - incCur2[u]) * invK;
            }

            for (int i = 0; i < k; ++i) {
                gCur += gStep; ampCur += ampStep; inGainCur += inGainStep;
                ladderL.g = ladderR.g = gCur;
                float l = 0.0f, r = 0.0f;
                for (int u = 0; u < unisonN; ++u) {
                    incCur1[u] += incStep1[u]; incCur2[u] += incStep2[u];
                    osc1[u].inc = incCur1[u]; osc2[u].inc = incCur2[u];
                    if (wave2 == wTriangle) osc2[u].setFreq(incCur2[u] * sr, sr);
                    if (wave1 == wTriangle) osc1[u].setFreq(incCur1[u] * sr, sr);
                    const float s = osc1[u].next(wave1) * lvl1 + ((lvl2 > 0.0f) ? osc2[u].next(wave2) * lvl2 : 0.0f);
                    const float pr = pan[u] * 0.5f + 0.5f;     // 0 = left, 1 = right
                    l += s * (1.0f - pr);
                    r += s * pr;
                }
                l *= inGainCur; r *= inGainCur;
                // ---- oversampled filter + drive (identical code, factor 1 just passes through)
                osL.up(l, tmpL); osR.up(r, tmpR);
                // the amp gain is applied INSIDE the oversampled zone so the envelope travels through the
                // same latency as the signal (otherwise the Full path's envelope would lead by 2*M samples)
                for (int j = 0; j < osFactor; ++j) {
                    tmpL[j] = drive.process(ladderL.process(tmpL[j])) * ampCur;
                    tmpR[j] = drive.process(ladderR.process(tmpR[j])) * ampCur;
                }
                outL[pos + i] += osL.down(tmpL);
                outR[pos + i] += osR.down(tmpR);
            }
            pos += k;
        }
    }
};

} // namespace absynth
