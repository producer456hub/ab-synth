// Envelope.h - exponential-segment ADSR plus a multi-shape LFO.
// Both are advanced in steps of k samples so the engine can run them per-sample (Full)
// or per 32-sample block (Gen1).
#pragma once
#include "Common.h"

namespace absynth {

struct ADSR {
    enum Stage { Idle, Attack, Decay, Sustain, Release };
    Stage stage = Idle;
    float value = 0.0f;
    float aCoef = 0.0f, dCoef = 0.0f, rCoef = 0.0f;
    float sustain = 0.8f;
    float sr = 48000.0f;

    static float coefFor(float seconds, float sr) {
        return std::exp(-1.0f / (std::max(seconds, 0.0001f) * sr));
    }
    void prepare(float sampleRate) { sr = sampleRate; }
    void set(float a, float d, float s, float r) {
        aCoef = coefFor(a * 0.6f, sr);     // attack overshoots to 1.3 so the knee lands at ~a
        dCoef = coefFor(d, sr);
        rCoef = coefFor(r, sr);
        sustain = clampf(s, 0.0f, 1.0f);
    }
    void gate(bool on) {
        if (on) stage = Attack;
        else if (stage != Idle) stage = Release;
    }
    void reset() { stage = Idle; value = 0.0f; }
    bool active() const { return stage != Idle; }

    // Advance by k samples and return the new value.
    inline float advance(int k) {
        switch (stage) {
            case Idle: return 0.0f;
            case Attack: {
                const float c = (k == 1) ? aCoef : std::pow(aCoef, (float)k);
                value = 1.3f + (value - 1.3f) * c;
                if (value >= 1.0f) { value = 1.0f; stage = Decay; }
                return value;
            }
            case Decay: {
                const float c = (k == 1) ? dCoef : std::pow(dCoef, (float)k);
                value = sustain + (value - sustain) * c;
                if (value - sustain < 1e-4f) { value = sustain; stage = Sustain; }
                return value;
            }
            case Sustain:
                value = sustain;
                return value;
            case Release: {
                const float c = (k == 1) ? rCoef : std::pow(rCoef, (float)k);
                value = -0.01f + (value + 0.01f) * c;      // aims below zero so it ends in finite time (~4.3 tau)
                if (value <= 1e-3f) { value = 0.0f; stage = Idle; }
                return value;
            }
        }
        return value;
    }
};

struct LFO {
    enum Shape { Sine = 0, Tri = 1, Saw = 2, Square = 3, SampleHold = 4 };
    float phase = 0.0f, inc = 0.0f;
    float held = 0.0f;
    Rng rng;
    int shape = Sine;

    void reset() { phase = 0.0f; held = rng.nextF(); }
    void set(float rateHz, int shp, float sr) { inc = rateHz / sr; shape = shp; }

    // Advance by k samples, return value in [-1, 1].
    inline float advance(int k) {
        phase += inc * (float)k;
        bool wrapped = false;
        while (phase >= 1.0f) { phase -= 1.0f; wrapped = true; }
        switch (shape) {
            case Sine:   return std::sin(kTwoPi * phase);
            case Tri:    return (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
            case Saw:    return 2.0f * phase - 1.0f;
            case Square: return (phase < 0.5f) ? 1.0f : -1.0f;
            default:
                if (wrapped) held = rng.nextF();
                return held;
        }
    }
};

} // namespace absynth
