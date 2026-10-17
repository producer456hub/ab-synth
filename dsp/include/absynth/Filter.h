// Filter.h - ZDF (topology-preserving transform) 4-pole ladder with tanh saturation,
// plus the drive waveshaper. Both run inside the oversampled zone of a voice.
#pragma once
#include "Common.h"

namespace absynth {

struct Ladder {
    float s[4] = {0, 0, 0, 0};
    float g = 0.1f;      // prewarped cutoff
    float k = 0.0f;      // feedback (0..~4.2)

    void reset() { s[0] = s[1] = s[2] = s[3] = 0.0f; }

    // cutoffHz is clamped to below 0.45 * fs of the (oversampled) rate the filter runs at.
    inline void setCoefs(float cutoffHz, float resonance, float fs) {
        const float wc = clampf(kPi * cutoffHz / fs, 0.0005f, 1.4f);
        g = fastTan(wc);
        k = clampf(resonance, 0.0f, 1.0f) * 4.2f;
    }

    inline float process(float x) {
        const float G  = g / (1.0f + g);
        const float G2 = G * G, G3 = G2 * G, G4 = G3 * G;
        // zero-delay feedback: linear estimate of the ladder output from the states
        const float S = (G3 * s[0] + G2 * s[1] + G * s[2] + s[3]) / (1.0f + g);
        float u = (x - k * S) / (1.0f + k * G4);
        u = fastTanh(u);                       // bounded input keeps every stage stable
        for (int i = 0; i < 4; ++i) {
            const float v = (u - s[i]) * G;
            const float y = v + s[i];
            s[i] = undenormal(y + v);
            u = y;
        }
        return u;
    }
};

// Waveshaper drive: tanh(x * (1 + 15*amount)) normalized, blended by mix.
struct Drive {
    float gain = 1.0f, norm = 1.0f, mix = 0.0f;
    inline void set(float amount, float mixIn) {
        gain = 1.0f + 15.0f * clampf(amount, 0.0f, 1.0f);
        norm = 1.0f / fastTanh(gain * 0.5f);   // unity-ish at half scale
        mix = (amount <= 0.0f) ? 0.0f : clampf(mixIn, 0.0f, 1.0f);
    }
    inline float process(float x) const {
        if (mix <= 0.0f) return x;
        const float y = fastTanh(x * gain) * norm * 0.5f;
        return x + (y - x) * mix;
    }
};

} // namespace absynth
