// Oscillator.h - polyBLEP saw/square + band-limited (mip-mapped) wavetable sine/triangle.
// Identical in both profiles: band-limiting is cheap and not where the Gen 1 loses.
#pragma once
#include "Common.h"

namespace absynth {

enum Wave : int { wSaw = 0, wSquare = 1, wSine = 2, wTriangle = 3 };

// Wavetables are built once (first call, from prepare(), never on the audio thread).
struct Wavetables {
    static constexpr int N = 2048;
    static constexpr int kTriTables = 11;      // harmonics 1024, 512, ... 1
    float sine[N + 1];
    float tri[kTriTables][N + 1];

    Wavetables() {
        for (int i = 0; i <= N; ++i)
            sine[i] = std::sin(kTwoPi * (float)i / (float)N);
        for (int t = 0; t < kTriTables; ++t) {
            const int maxH = 1024 >> t;
            for (int i = 0; i <= N; ++i) {
                const double ph = 2.0 * 3.14159265358979323846 * (double)i / (double)N;
                double acc = 0.0;
                double sign = 1.0;
                for (int h = 1; h <= maxH; h += 2) {
                    acc += sign * std::sin(h * ph) / (double)(h * h);
                    sign = -sign;
                }
                tri[t][i] = (float)(acc * 8.0 / (3.14159265358979323846 * 3.14159265358979323846));
            }
        }
    }
    static const Wavetables& get() { static Wavetables w; return w; }
};

// 4-point polyBLEP: residual of an integrated cubic B-spline step, scaled for a step of height 2.
// Spans two samples on each side of the discontinuity (needs dt < 0.25); falls back to the
// classic 2-point form above fs/4.
inline float polyBlep(float t, float dt) {
    if (dt >= 0.25f) {
        if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
        if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
        return 0.0f;
    }
    const float dt2 = dt + dt;
    float r;
    if (t < dt2) {                          // after the step, x in [0, 2)
        const float x = t / dt;
        if (x < 1.0f) r = -0.5f + (2.0f / 3.0f) * x - x * x * x * (1.0f / 3.0f) + x * x * x * x * 0.125f;
        else { const float y = 2.0f - x; const float y2 = y * y; r = -y2 * y2 * (1.0f / 24.0f); }
    } else if (t > 1.0f - dt2) {            // before the step, x in (-2, 0)
        const float x = (t - 1.0f) / dt;
        if (x > -1.0f) r = 0.5f + (2.0f / 3.0f) * x - x * x * x * (1.0f / 3.0f) - x * x * x * x * 0.125f;
        else { const float y = x + 2.0f; const float y2 = y * y; r = y2 * y2 * (1.0f / 24.0f); }
    } else return 0.0f;
    return 2.0f * r;
}

struct Oscillator {
    float phase = 0.0f;   // 0..1
    float inc   = 0.0f;   // cycles per sample
    const Wavetables* wt = nullptr;
    int triTable = 0;

    void prepare() { wt = &Wavetables::get(); }
    void reset(float ph = 0.0f) { phase = ph; }

    inline void setFreq(float hz, float sampleRate) {
        inc = clampf(hz / sampleRate, 0.0f, 0.49f);
        // choose the triangle mip level whose highest harmonic stays below Nyquist
        const float needed = (inc > 1e-9f) ? (0.5f / inc) : 1024.0f;   // harmonics that fit
        int t = 0;
        while (t < Wavetables::kTriTables - 1 && (float)(1024 >> t) > needed) ++t;
        triTable = t;
    }

    inline float next(int wave) {
        const float t = phase;
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
        switch (wave) {
            case wSaw: {
                return (2.0f * t - 1.0f) - polyBlep(t, inc);
            }
            case wSquare: {
                float y = (t < 0.5f) ? 1.0f : -1.0f;
                y += polyBlep(t, inc);
                float t2 = t + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f;
                y -= polyBlep(t2, inc);
                return y;
            }
            case wSine: {
                const float pos = t * (float)Wavetables::N;
                const int i = (int)pos;
                const float f = pos - (float)i;
                return wt->sine[i] + (wt->sine[i + 1] - wt->sine[i]) * f;
            }
            default: {
                const float* tb = wt->tri[triTable];
                const float pos = t * (float)Wavetables::N;
                const int i = (int)pos;
                const float f = pos - (float)i;
                return tb[i] + (tb[i + 1] - tb[i]) * f;
            }
        }
    }

    // Naive (aliased) saw, used only by the unit test as the reference "bad" oscillator.
    inline float nextNaiveSaw() {
        const float t = phase;
        phase += inc;
        if (phase >= 1.0f) phase -= 1.0f;
        return 2.0f * t - 1.0f;
    }
};

} // namespace absynth
