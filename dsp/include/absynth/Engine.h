// Engine.h - one complete synth instance: voices + allocator + FX + profile.
// Public API: prepare, noteOn/noteOff, setParam, process, setProfile, setConstraint.
#pragma once
#include "Common.h"
#include "Params.h"
#include "Profile.h"
#include "Voice.h"
#include "Effects.h"
#include <vector>

namespace absynth {

class Engine {
public:
    void prepare(float sampleRate, int maxBlock) {
        sr = sampleRate; maxBlockSize = std::max(1, maxBlock);
        enableFlushToZero();
        for (int i = 0; i < kNumParams; ++i) {
            const auto& inf = paramInfo(i);
            target[i] = inf.def;
            smooth[i].prepare(sr, 5.0f);
            smooth[i].snap(inf.def);
        }
        curves.assign((size_t)kNumParams * (size_t)maxBlockSize, 0.0f);
        for (int i = 0; i < kNumParams; ++i) curvePtr[i] = &curves[(size_t)i * (size_t)maxBlockSize];
        for (auto& v : voices) v.prepare(sr);
        chorus.prepare(sr); delay.prepare(sr); fdn.prepare(sr); schroeder.prepare(sr);
        mixL.assign((size_t)maxBlockSize, 0.0f); mixR.assign((size_t)maxBlockSize, 0.0f);
        applyProfile();
        ageCounter = 0;
    }

    // ---- profile
    void setProfile(Profile p) { settings.profile = p; applyProfile(); }
    void setConstraint(int c, bool on) { if (c >= 0 && c < kNumConstraints) { settings.enabled[c] = on; applyProfile(); } }
    Profile profile() const { return settings.profile; }
    const Limits& limits() const { return lim; }
    int latencySamples() const { return lim.oversample > 1 ? Oversampler::latencySamples() : 0; }

    // ---- parameters
    void setParam(int id, float v) {
        if (id < 0 || id >= kNumParams) return;
        const auto& inf = paramInfo(id);
        v = clampf(v, inf.min, inf.max);
        target[id] = v;
        if (isDiscrete(id)) smooth[id].snap(v); else smooth[id].set(v);
        if (id == pUnisonVoices || id == pUnisonDetune || id == pUnisonSpread) refreshUnison();
        if (id >= pAmpA && id <= pFilR) refreshEnvelopes();
        if (id >= pLfo1Rate && id <= pLfo2ToCutoff) refreshLfos();
    }
    float getParam(int id) const { return (id >= 0 && id < kNumParams) ? target[id] : 0.0f; }

    // ---- notes
    void noteOn(int note, float vel) {
        note = std::max(0, std::min(127, note));
        vel = clampf(vel, 0.0f, 1.0f);
        const bool mono = target[pMono] >= 0.5f;
        const float glide = target[pGlide];
        if (mono) {
            Voice& v = voices[0];
            bool legato = v.held;              // key still down -> glide without retrigger
            v.age = ++ageCounter;
            v.start(note, vel, legato, glide);
            return;
        }
        // retrigger if the same note is already held
        for (int i = 0; i < lim.maxVoices; ++i)
            if (voices[i].held && voices[i].note == note) { voices[i].age = ++ageCounter; voices[i].start(note, vel, false, glide); return; }
        // free voice
        int idx = -1;
        for (int i = 0; i < lim.maxVoices; ++i) if (!voices[i].active()) { idx = i; break; }
        if (idx < 0) {
            // oldest-note stealing: prefer released voices, else the oldest held
            uint32_t best = 0xFFFFFFFFu;
            for (int i = 0; i < lim.maxVoices; ++i) {
                const uint32_t a = voices[i].age + (voices[i].held ? 0x80000000u : 0u);
                if (a < best) { best = a; idx = i; }
            }
        }
        Voice& v = voices[idx];
        v.age = ++ageCounter;
        v.start(note, vel, false, glide);
    }

    void noteOff(int note) {
        for (auto& v : voices) if (v.held && v.note == note) v.release();
    }
    void allNotesOff() { for (auto& v : voices) v.release(); }
    void panic() { for (auto& v : voices) v.kill(); chorus.reset(); delay.reset(); fdn.reset(); schroeder.reset(); }

    int activeVoices() const { int n = 0; for (const auto& v : voices) if (v.active()) ++n; return n; }

    // ---- audio. out[0] = left, out[1] = right. n may exceed maxBlock (it is split).
    void process(float** out, int n) {
        enableFlushToZero();
        int done = 0;
        while (done < n) {
            const int k = std::min(maxBlockSize, n - done);
            processBlock(out[0] + done, out[1] + done, k);
            done += k;
        }
    }

private:
    float sr = 48000.0f;
    int   maxBlockSize = 128;
    ProfileSettings settings;
    Limits lim;
    float target[kNumParams] = {};
    Smoother smooth[kNumParams];
    std::vector<float> curves;
    const float* curvePtr[kNumParams] = {};
    Voice voices[kMaxVoices];
    uint32_t ageCounter = 0;
    Chorus chorus;
    StereoDelay delay;
    FDNReverb fdn;
    SchroederReverb schroeder;
    std::vector<float> mixL, mixR;

    static bool isDiscrete(int id) {
        return id == pOsc1Wave || id == pOsc2Wave || id == pUnisonVoices || id == pLfo1Shape ||
               id == pLfo2Shape || id == pMono || id == pOsc2Semi;
    }

    void applyProfile() {
        lim = settings.resolve();
        for (int i = 0; i < kMaxVoices; ++i) {
            voices[i].applyLimits(lim);
            if (i >= lim.maxVoices && voices[i].active()) voices[i].release();
        }
        delay.interp = lim.delayInterp;
        refreshUnison();
    }
    void refreshUnison() {
        const int n = std::min((int)target[pUnisonVoices], lim.maxUnison);
        for (auto& v : voices) v.setUnison(n, target[pUnisonDetune], target[pUnisonSpread]);
    }
    void refreshEnvelopes() {
        for (auto& v : voices) {
            v.ampEnv.set(target[pAmpA], target[pAmpD], target[pAmpS], target[pAmpR]);
            v.filtEnv.set(target[pFilA], target[pFilD], target[pFilS], target[pFilR]);
        }
    }
    void refreshLfos() {
        for (auto& v : voices) {
            v.lfo1.set(target[pLfo1Rate], (int)target[pLfo1Shape], sr);
            v.lfo2.set(target[pLfo2Rate], (int)target[pLfo2Shape], sr);
        }
    }

    void processBlock(float* outL, float* outR, int n) {
        // parameter curves (per-sample smoothing of every continuous parameter)
        for (int id = 0; id < kNumParams; ++id) {
            float* c = &curves[(size_t)id * (size_t)maxBlockSize];
            Smoother& s = smooth[id];
            if (s.settled()) { const float v = s.target; for (int i = 0; i < n; ++i) c[i] = v; s.value = v; }
            else for (int i = 0; i < n; ++i) c[i] = s.next();
        }
        float* L = mixL.data(); float* R = mixR.data();
        std::memset(L, 0, sizeof(float) * (size_t)n);
        std::memset(R, 0, sizeof(float) * (size_t)n);
        for (int i = 0; i < kMaxVoices; ++i)
            if (voices[i].active()) voices[i].render(L, R, n, curvePtr);

        // FX chain: chorus -> delay -> reverb -> master
        chorus.set(target[pChorusMix], target[pChorusRate], target[pChorusDepth]);
        delay.set(target[pDelayTime], target[pDelayFeedback], target[pDelayMix], target[pDelayPingPong], target[pDelayWobble]);
        fdn.set(target[pReverbSize], target[pReverbDamp], lim.reverb == ReverbType::FDN ? target[pReverbMix] : 0.0f);
        schroeder.set(target[pReverbSize], target[pReverbDamp], lim.reverb == ReverbType::Schroeder ? target[pReverbMix] : 0.0f);
        const float* master = curvePtr[pMasterGain];
        for (int i = 0; i < n; ++i) {
            float l = L[i], r = R[i];
            chorus.process(l, r);
            delay.process(l, r);
            fdn.process(l, r);
            schroeder.process(l, r);
            outL[i] = l * master[i];
            outR[i] = r * master[i];
        }
    }
};

} // namespace absynth
