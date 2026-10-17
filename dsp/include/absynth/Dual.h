// Dual.h - two Engine instances (A = Full, B = Gen1) rendered in parallel with a 10 ms
// equal-power crossfade, per-instance loudness trims and latency alignment.
#pragma once
#include "Engine.h"

namespace absynth {

class Dual {
public:
    static constexpr int kMaxLatency = 64;

    void prepare(float sampleRate, int maxBlock) {
        sr = sampleRate;
        a.prepare(sampleRate, maxBlock);
        b.prepare(sampleRate, maxBlock);
        a.setProfile(Profile::Full);
        b.setProfile(Profile::Gen1);
        bufA[0].assign((size_t)maxBlock, 0.0f); bufA[1].assign((size_t)maxBlock, 0.0f);
        bufB[0].assign((size_t)maxBlock, 0.0f); bufB[1].assign((size_t)maxBlock, 0.0f);
        for (auto& d : align) d.prepare(kMaxLatency + 8);
        xfade.prepare(sampleRate, 10.0f);
        xfade.snap(0.0f);
        trimA.prepare(sampleRate, 20.0f); trimA.snap(1.0f);
        trimB.prepare(sampleRate, 20.0f); trimB.snap(1.0f);
    }

    Engine& engine(int i) { return i == 0 ? a : b; }

    void setMix(float x) { xfade.set(clampf(x, 0.0f, 1.0f)); }      // 0 = A, 1 = B
    float mix() const { return xfade.target; }
    void setTrimDb(int inst, float db) { (inst == 0 ? trimA : trimB).set(dbToGain(clampf(db, -24.0f, 24.0f))); }

    // Both engines receive every event.
    void noteOn(int note, float vel) { a.noteOn(note, vel); b.noteOn(note, vel); }
    void noteOff(int note) { a.noteOff(note); b.noteOff(note); }
    void allNotesOff() { a.allNotesOff(); b.allNotesOff(); }
    void panic() { a.panic(); b.panic(); for (auto& d : align) d.clear(); }
    void setParam(int id, float v) { a.setParam(id, v); b.setParam(id, v); }

    void process(float* outL, float* outR, int n) {
        float* pa[2] = { bufA[0].data(), bufA[1].data() };
        float* pb[2] = { bufB[0].data(), bufB[1].data() };
        a.process(pa, n);
        b.process(pb, n);
        // latency alignment: delay the lower-latency instance so a switch mid-note is phase-aligned
        const int la = a.latencySamples(), lb = b.latencySamples();
        const int maxL = std::max(la, lb);
        const int dA = std::min(kMaxLatency, maxL - la), dB = std::min(kMaxLatency, maxL - lb);
        for (int i = 0; i < n; ++i) {
            const float x = xfade.next();
            const float ga = std::cos(x * kPi * 0.5f) * trimA.next();
            const float gb = std::sin(x * kPi * 0.5f) * trimB.next();
            float al = pa[0][i], ar = pa[1][i], bl = pb[0][i], br = pb[1][i];
            align[0].write(al); align[1].write(ar); align[2].write(bl); align[3].write(br);
            if (dA > 0) { al = align[0].readInt(dA + 1); ar = align[1].readInt(dA + 1); }
            if (dB > 0) { bl = align[2].readInt(dB + 1); br = align[3].readInt(dB + 1); }
            outL[i] = al * ga + bl * gb;
            outR[i] = ar * ga + br * gb;
        }
    }

private:
    float sr = 48000.0f;
    Engine a, b;
    std::vector<float> bufA[2], bufB[2];
    DelayLine align[4];
    Smoother xfade, trimA, trimB;
};

} // namespace absynth
