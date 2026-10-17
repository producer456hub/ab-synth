// Effects.h - chorus, stereo delay, 16-line modulated FDN reverb, Schroeder/Freeverb-lite reverb.
// Buffers are sized in prepare(); nothing allocates on the audio path.
#pragma once
#include "Common.h"
#include "Profile.h"
#include <vector>

namespace absynth {

struct DelayLine {
    std::vector<float> buf;
    int mask = 0;
    int w = 0;

    void prepare(int maxSamples) {
        int size = 16;
        while (size < maxSamples + 8) size <<= 1;
        buf.assign((size_t)size, 0.0f);
        mask = size - 1;
        w = 0;
    }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); w = 0; }
    inline void write(float x) { buf[(size_t)w] = x; w = (w + 1) & mask; }
    // delay d >= 1 measured from the next write position
    inline float readInt(int d) const { return buf[(size_t)((w - d) & mask)]; }
    inline float readLinear(float d) const {
        const int i = (int)d; const float f = d - (float)i;
        const float a = readInt(i), b = readInt(i + 1);
        return a + (b - a) * f;
    }
    inline float readCubic(float d) const {
        const int i = (int)d; const float f = d - (float)i;
        const float y0 = readInt(i - 1), y1 = readInt(i), y2 = readInt(i + 1), y3 = readInt(i + 2);
        // 4-point Hermite
        const float c0 = y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + c0;
    }
};

// ---------------------------------------------------------------- Chorus
struct Chorus {
    DelayLine dl[2];
    float sr = 48000.0f;
    float phase = 0.0f;
    Smoother mix, rate, depth;

    void prepare(float sampleRate) {
        sr = sampleRate;
        for (auto& d : dl) d.prepare((int)(0.040f * sr) + 16);
        mix.prepare(sr, 20.0f); rate.prepare(sr, 50.0f); depth.prepare(sr, 50.0f);
        phase = 0.0f;
    }
    void reset() { for (auto& d : dl) d.clear(); phase = 0.0f; }
    void set(float m, float r, float dp) { mix.set(m); rate.set(r); depth.set(dp); }

    inline void process(float& l, float& r) {
        const float m = mix.next();
        const float rt = rate.next();
        const float dp = depth.next();
        dl[0].write(l); dl[1].write(r);
        if (m <= 0.0005f) return;
        phase += rt / sr; if (phase >= 1.0f) phase -= 1.0f;
        const float base = 0.015f * sr;
        const float dev  = 0.008f * sr * dp;
        const float dL = base + dev * (0.5f + 0.5f * std::sin(kTwoPi * phase));
        const float dR = base + dev * (0.5f + 0.5f * std::cos(kTwoPi * phase));
        const float wl = dl[0].readCubic(dL);
        const float wr = dl[1].readCubic(dR);
        l = l + (wl - l) * m * 0.7f;
        r = r + (wr - r) * m * 0.7f;
    }
};

// ---------------------------------------------------------------- Stereo delay (ping-pong)
struct StereoDelay {
    DelayLine dl[2];
    float sr = 48000.0f;
    float wobblePhase = 0.0f;
    Smoother time, feedback, mix, pingpong, wobble;
    DelayInterp interp = DelayInterp::Cubic;

    void prepare(float sampleRate) {
        sr = sampleRate;
        for (auto& d : dl) d.prepare((int)(2.1f * sr) + 16);
        time.prepare(sr, 80.0f); feedback.prepare(sr, 20.0f); mix.prepare(sr, 20.0f);
        pingpong.prepare(sr, 20.0f); wobble.prepare(sr, 50.0f);
        wobblePhase = 0.0f;
    }
    void reset() { for (auto& d : dl) d.clear(); wobblePhase = 0.0f; }
    void set(float t, float fb, float m, float pp, float wb) {
        time.set(t); feedback.set(fb); mix.set(m); pingpong.set(pp); wobble.set(wb);
    }

    inline float read(const DelayLine& d, float samples) const {
        return (interp == DelayInterp::Cubic) ? d.readCubic(samples) : d.readLinear(samples);
    }

    inline void process(float& l, float& r) {
        const float t  = time.next();
        const float fb = feedback.next();
        const float m  = mix.next();
        const float pp = pingpong.next();
        const float wb = wobble.next();
        wobblePhase += 0.3f / sr; if (wobblePhase >= 1.0f) wobblePhase -= 1.0f;
        const float wobSamples = wb * 0.0015f * sr * std::sin(kTwoPi * wobblePhase);
        const float dSamples = clampf(t * sr + wobSamples, 4.0f, 2.0f * sr);
        const float oL = read(dl[0], dSamples);
        const float oR = read(dl[1], dSamples);
        // ping-pong: L line is fed by (input + R output), R line by (input + L output)
        const float inL = l, inR = r;
        const float fbL = fb * (oL + (oR - oL) * pp);
        const float fbR = fb * (oR + (oL - oR) * pp);
        dl[0].write(undenormal(inL + fbL));
        dl[1].write(undenormal(inR + fbR));
        l = inL + oL * m;
        r = inR + oR * m;
    }
};

// ---------------------------------------------------------------- FDN reverb (16 lines, modulated)
struct FDNReverb {
    static constexpr int N = 16;
    DelayLine dl[N];
    float lp[N] = {};
    float modPhase[N] = {};
    float modInc[N] = {};
    float baseLen[N] = {};
    float gain[N] = {};
    float sr = 48000.0f;
    Smoother size, damp, mix;
    float lastSize = -1.0f;

    static constexpr int kBase48k[N] = { 1123, 1327, 1489, 1601, 1777, 1913, 2053, 2203,
                                         2341, 2473, 2609, 2741, 2887, 3001, 3169, 3301 };

    void prepare(float sampleRate) {
        sr = sampleRate;
        for (int i = 0; i < N; ++i) {
            dl[i].prepare((int)(kBase48k[i] * 1.3f * sr / 48000.0f) + 64);
            modInc[i] = (0.07f + 0.023f * (float)i) / sr;   // 0.07 .. 0.41 Hz
            modPhase[i] = (float)i / (float)N;
        }
        size.prepare(sr, 100.0f); damp.prepare(sr, 50.0f); mix.prepare(sr, 20.0f);
        lastSize = -1.0f;
        updateLengths(0.7f);
    }
    void reset() { for (auto& d : dl) d.clear(); for (auto& v : lp) v = 0.0f; }
    void set(float s, float d, float m) { size.set(s); damp.set(d); mix.set(m); }

    inline void updateLengths(float s) {
        const float scale = (0.35f + 0.85f * s) * sr / 48000.0f;
        const float rt60 = 0.6f + 7.0f * s;
        for (int i = 0; i < N; ++i) {
            baseLen[i] = kBase48k[i] * scale;
            gain[i] = std::pow(10.0f, -3.0f * baseLen[i] / (rt60 * sr));
        }
        lastSize = s;
    }

    inline void process(float& l, float& r) {
        const float s = size.next();
        const float d = damp.next();
        const float m = mix.next();
        if (std::fabs(s - lastSize) > 0.002f) updateLengths(s);
        if (m <= 0.0005f) { return; }
        const float in = (l + r) * 0.5f;
        const float dampCoef = 0.05f + 0.9f * d;
        float out[N];
        float sum = 0.0f;
        for (int i = 0; i < N; ++i) {
            modPhase[i] += modInc[i]; if (modPhase[i] >= 1.0f) modPhase[i] -= 1.0f;
            const float len = baseLen[i] + 6.0f * std::sin(kTwoPi * modPhase[i]);
            float y = dl[i].readCubic(len);
            lp[i] = undenormal(lp[i] + (y - lp[i]) * (1.0f - dampCoef));   // one-pole damping
            y = lp[i] * gain[i];
            out[i] = y;
            sum += y;
        }
        // Householder feedback: v_i = y_i - (2/N) * sum
        const float h = sum * (2.0f / (float)N);
        float wl = 0.0f, wr = 0.0f;
        for (int i = 0; i < N; ++i) {
            const float fb = out[i] - h;
            const float sign = (i & 1) ? -1.0f : 1.0f;
            dl[i].write(undenormal(fb + in * sign * 0.35f));
            if (i & 1) wr += out[i]; else wl += out[i];
        }
        wl *= 0.25f; wr *= 0.25f;
        l += (wl - l) * m * 0.5f + wl * m * 0.5f;
        r += (wr - r) * m * 0.5f + wr * m * 0.5f;
    }
};

// ---------------------------------------------------------------- Schroeder / Freeverb-lite (4 comb + 2 allpass per channel)
struct SchroederReverb {
    static constexpr int kCombs = 4, kAllpass = 2;
    static constexpr int kComb44[kCombs] = { 1116, 1188, 1277, 1356 };
    static constexpr int kAp44[kAllpass]  = { 556, 441 };
    static constexpr int kStereoSpread = 23;

    DelayLine comb[2][kCombs];
    DelayLine ap[2][kAllpass];
    float lpState[2][kCombs] = {};
    int combLen[2][kCombs] = {};
    int apLen[2][kAllpass] = {};
    float sr = 48000.0f;
    Smoother size, damp, mix;
    float lastSize = -1.0f;

    void prepare(float sampleRate) {
        sr = sampleRate;
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < kCombs; ++i) comb[c][i].prepare((int)((kComb44[i] + kStereoSpread) * 1.3f * sr / 44100.0f) + 8);
            for (int i = 0; i < kAllpass; ++i) ap[c][i].prepare((int)((kAp44[i] + kStereoSpread) * 1.3f * sr / 44100.0f) + 8);
        }
        size.prepare(sr, 100.0f); damp.prepare(sr, 50.0f); mix.prepare(sr, 20.0f);
        lastSize = -1.0f;
        updateLengths(0.7f);
    }
    void reset() {
        for (int c = 0; c < 2; ++c) {
            for (auto& d : comb[c]) d.clear();
            for (auto& d : ap[c]) d.clear();
            for (auto& v : lpState[c]) v = 0.0f;
        }
    }
    void set(float s, float d, float m) { size.set(s); damp.set(d); mix.set(m); }

    inline void updateLengths(float s) {
        const float scale = (0.6f + 0.7f * s) * sr / 44100.0f;
        for (int c = 0; c < 2; ++c) {
            const int spread = c ? kStereoSpread : 0;
            for (int i = 0; i < kCombs; ++i) combLen[c][i] = std::max(4, (int)((kComb44[i] + spread) * scale));
            for (int i = 0; i < kAllpass; ++i) apLen[c][i] = std::max(4, (int)((kAp44[i] + spread) * scale));
        }
        lastSize = s;
    }

    inline float channel(int c, float in, float fbGain, float dampCoef) {
        float acc = 0.0f;
        for (int i = 0; i < kCombs; ++i) {
            const float y = comb[c][i].readInt(combLen[c][i]);
            lpState[c][i] = undenormal(y * (1.0f - dampCoef) + lpState[c][i] * dampCoef);
            comb[c][i].write(undenormal(in + lpState[c][i] * fbGain));
            acc += y;
        }
        float x = acc;
        for (int i = 0; i < kAllpass; ++i) {
            const float bufout = ap[c][i].readInt(apLen[c][i]);
            const float y = -x + bufout;
            ap[c][i].write(undenormal(x + bufout * 0.5f));
            x = y;
        }
        return x;
    }

    inline void process(float& l, float& r) {
        const float s = size.next();
        const float d = damp.next();
        const float m = mix.next();
        if (std::fabs(s - lastSize) > 0.002f) updateLengths(s);
        if (m <= 0.0005f) return;
        const float in = (l + r) * 0.5f * 0.08f;
        const float fbGain = 0.60f + 0.37f * s;
        const float dampCoef = 0.1f + 0.85f * d;
        const float wl = channel(0, in, fbGain, dampCoef);
        const float wr = channel(1, in, fbGain, dampCoef);
        l += (wl - l) * m * 0.5f + wl * m * 0.5f;
        r += (wr - r) * m * 0.5f + wr * m * 0.5f;
    }
};

} // namespace absynth
