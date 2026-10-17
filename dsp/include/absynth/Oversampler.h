// Oversampler.h - polyphase FIR up/down sampler for factor 1, 2 or 4.
// Kaiser-windowed sinc lowpass at the base Nyquist, L = 2*R*M + 1 taps. Each FIR has a
// group delay of M base-rate samples, so the up+down round trip is exactly 2*M samples
// whatever the factor is (integer, so A and B can be aligned sample-exactly).
#pragma once
#include "Common.h"

namespace absynth {

struct Oversampler {
    static constexpr int kM       = 12;                       // half-length in base samples
    static constexpr int kMaxR    = 4;
    static constexpr int kMaxTaps = 2 * kMaxR * kM + 1;       // 97
    static constexpr int kMaxPhaseTaps = 2 * kM + 1;          // 25

    int   R = 1;
    int   L = 1;
    float h[kMaxTaps];                                        // full prototype
    // polyphase for the upsampler: poly[p][q] = R * h[q*R + p]
    float poly[kMaxR][kMaxPhaseTaps];
    int   polyLen[kMaxR];
    // histories
    float xin[kMaxPhaseTaps * 2];      // base-rate input history (doubled for wrap-free reads)
    int   xw = 0;
    static constexpr int kHist = kMaxTaps + 8;   // ring longer than the FIR so the R-1 "future" samples never alias in
    float hi[kHist * 2];               // high-rate history for the decimator (doubled for wrap-free reads)
    int   hw = 0;

    static constexpr int latencySamples() { return 2 * kM; }

    void reset() {
        std::memset(xin, 0, sizeof(xin));
        std::memset(hi, 0, sizeof(hi));
        xw = hw = 0;
    }

    static double besselI0(double x) {
        double sum = 1.0, term = 1.0;
        const double q = x * x * 0.25;
        for (int k = 1; k < 50; ++k) { term *= q / (double)(k * k); sum += term; if (term < 1e-12 * sum) break; }
        return sum;
    }

    // factor: 1, 2 or 4. Called from prepare()/profile changes, never on the audio path mid-block.
    void setFactor(int factor) {
        R = (factor == 4) ? 4 : (factor == 2 ? 2 : 1);
        L = 2 * R * kM + 1;
        const int c = (L - 1) / 2;
        const double beta = 7.0;
        const double i0b = besselI0(beta);
        const double fc = 0.46 / (double)R;    // cutoff as fraction of the high rate (base Nyquist * 0.92)
        for (int k = 0; k < L; ++k) {
            const int d = k - c;
            double sinc = (d == 0) ? 2.0 * fc : std::sin(2.0 * 3.14159265358979323846 * fc * d) / (3.14159265358979323846 * d);
            const double r = (double)d / (double)c;
            const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
            h[k] = (float)(sinc * w);
        }
        // normalize DC gain to 1
        double sum = 0.0; for (int k = 0; k < L; ++k) sum += h[k];
        for (int k = 0; k < L; ++k) h[k] = (float)(h[k] / sum);
        for (int p = 0; p < R; ++p) {
            int n = 0;
            for (int q = 0; q * R + p < L; ++q) poly[p][n++] = (float)R * h[q * R + p];
            polyLen[p] = n;
        }
        reset();
    }

    // Push one base-rate sample, write R high-rate samples to out.
    inline void up(float x, float* out) {
        if (R == 1) { out[0] = x; return; }
        xin[xw] = x; xin[xw + kMaxPhaseTaps] = x;
        // newest sample is at xin[xw], older ones go backwards: x[n-q] = xin[xw - q + kMaxPhaseTaps]
        const float* base = &xin[xw + kMaxPhaseTaps];
        for (int p = 0; p < R; ++p) {
            const float* c = poly[p];
            const int n = polyLen[p];
            float acc = 0.0f;
            for (int q = 0; q < n; ++q) acc += c[q] * base[-q];
            out[p] = acc;
        }
        xw = (xw + 1) % kMaxPhaseTaps;
    }

    // Consume R high-rate samples, return one base-rate sample.
    // The output is evaluated at the FIRST of the R new samples (the one on the base-rate grid),
    // so the decimator adds exactly M base samples of delay and no fractional offset.
    inline float down(const float* in) {
        if (R == 1) return in[0];
        const int first = hw;
        for (int p = 0; p < R; ++p) {
            hi[hw] = in[p]; hi[hw + kHist] = in[p];
            hw = (hw + 1) % kHist;
        }
        const float* base = &hi[first + kHist];
        float acc = 0.0f;
        for (int k = 0; k < L; ++k) acc += h[k] * base[-k];
        return acc;
    }
};

} // namespace absynth
