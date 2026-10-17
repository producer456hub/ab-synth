// Common.h - shared helpers for the AB Synth DSP core.
// Portable C++17, no platform headers, no allocation on the audio path.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
  #include <xmmintrin.h>
  #include <pmmintrin.h>
  #define ABSYNTH_HAVE_SSE 1
#endif

namespace absynth {

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

// Enable flush-to-zero / denormals-are-zero on the calling thread (x86/SSE). No-op elsewhere.
inline void enableFlushToZero() {
#ifdef ABSYNTH_HAVE_SSE
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

// Kill denormals on platforms without FTZ (wasm): snap tiny magnitudes to zero.
inline float undenormal(float x) {
    return (std::fabs(x) < 1e-25f) ? 0.0f : x;
}

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

inline float noteToHz(float note) { return 440.0f * std::exp2((note - 69.0f) * (1.0f / 12.0f)); }
inline float dbToGain(float db)   { return std::pow(10.0f, db * 0.05f); }
inline float gainToDb(float g)    { return 20.0f * std::log10(std::max(g, 1e-12f)); }

// Fast tanh (rational approximation, accurate to ~1e-3 over the audio range, clamps beyond).
inline float fastTanh(float x) {
    x = clampf(x, -4.97f, 4.97f);
    const float x2 = x * x;
    const float a = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
    const float b = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f));
    return a / b;
}

// Fast tan for filter prewarp, valid for x in [0, ~1.4] (cutoff is clamped below 0.45*fs).
inline float fastTan(float x) {
    const float x2 = x * x;
    return x * (945.0f - 105.0f * x2 + x2 * x2) / (945.0f - 420.0f * x2 + 15.0f * x2 * x2);
}

// One-pole parameter smoother. ~5 ms time constant by default.
struct Smoother {
    float value = 0.0f, target = 0.0f, coef = 0.0f;
    void prepare(float sampleRate, float ms = 5.0f) {
        coef = std::exp(-1.0f / (sampleRate * ms * 0.001f));
    }
    void set(float t) { target = t; }
    void snap(float t) { target = value = t; }
    inline float next() {
        value = target + (value - target) * coef;
        return value;
    }
    bool settled() const { return std::fabs(value - target) < 1e-6f; }
};

// Small xorshift RNG for noise / S&H (deterministic so renders are reproducible).
struct Rng {
    uint32_t s = 0x9E3779B9u;
    inline uint32_t nextU() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    inline float nextF() { return (nextU() >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f; } // [-1,1)
};

} // namespace absynth
