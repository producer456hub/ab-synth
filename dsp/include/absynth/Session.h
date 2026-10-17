// Session.h - a multitrack session: up to 8 Engines (one per track), per-track gain/pan/mute/solo,
// master gain, summed into a stereo output. This is the "Gen1 OS" audio core.
#pragma once
#include "Engine.h"

namespace absynth {

class Session {
public:
    static constexpr int kMaxTracks = 8;

    void prepare(float sampleRate, int maxBlock, int nTracks) {
        sr = sampleRate;
        n = std::max(1, std::min(nTracks, kMaxTracks));
        for (int t = 0; t < n; ++t) {
            tracks[t].prepare(sampleRate, maxBlock);
            tracks[t].setProfile(Profile::Gen1);
            gain[t].prepare(sr, 20.0f); gain[t].snap(0.8f);
            panL[t].prepare(sr, 20.0f); panL[t].snap(0.7071f);
            panR[t].prepare(sr, 20.0f); panR[t].snap(0.7071f);
            mute[t] = false; solo[t] = false; peak[t] = 0.0f;
        }
        master.prepare(sr, 20.0f); master.snap(0.8f);
        bufL.assign((size_t)maxBlock, 0.0f); bufR.assign((size_t)maxBlock, 0.0f);
    }

    int trackCount() const { return n; }
    Engine& track(int t) { return tracks[std::max(0, std::min(t, n - 1))]; }

    void setMix(int t, float g, float pan, bool m, bool s) {
        if (t < 0 || t >= n) return;
        gain[t].set(clampf(g, 0.0f, 2.0f));
        const float p = clampf(pan, -1.0f, 1.0f) * 0.5f + 0.5f;   // 0 = left, 1 = right
        panL[t].set(std::cos(p * kPi * 0.5f));
        panR[t].set(std::sin(p * kPi * 0.5f));
        mute[t] = m; solo[t] = s;
    }
    void setMaster(float g) { master.set(clampf(g, 0.0f, 2.0f)); }
    float trackPeak(int t) const { return (t >= 0 && t < n) ? peak[t] : 0.0f; }
    void panic() { for (int t = 0; t < n; ++t) tracks[t].panic(); }
    void allNotesOff() { for (int t = 0; t < n; ++t) tracks[t].allNotesOff(); }

    void process(float* outL, float* outR, int len) {
        std::memset(outL, 0, sizeof(float) * (size_t)len);
        std::memset(outR, 0, sizeof(float) * (size_t)len);
        bool anySolo = false;
        for (int t = 0; t < n; ++t) anySolo = anySolo || solo[t];
        float* b[2] = { bufL.data(), bufR.data() };
        for (int t = 0; t < n; ++t) {
            tracks[t].process(b, len);                       // always rendered (voices must advance)
            const bool audible = !mute[t] && (!anySolo || solo[t]);
            float pk = 0.0f;
            for (int i = 0; i < len; ++i) {
                const float g = gain[t].next() * (audible ? 1.0f : 0.0f);
                const float l = b[0][i] * g * panL[t].next();
                const float r = b[1][i] * g * panR[t].next();
                outL[i] += l; outR[i] += r;
                const float a = std::max(std::fabs(l), std::fabs(r));
                if (a > pk) pk = a;
            }
            peak[t] = pk;
        }
        for (int i = 0; i < len; ++i) { const float m = master.next(); outL[i] *= m; outR[i] *= m; }
    }

private:
    float sr = 48000.0f;
    int n = 8;
    Engine tracks[kMaxTracks];
    Smoother gain[kMaxTracks], panL[kMaxTracks], panR[kMaxTracks], master;
    bool mute[kMaxTracks] = {}, solo[kMaxTracks] = {};
    float peak[kMaxTracks] = {};
    std::vector<float> bufL, bufR;
};

} // namespace absynth
