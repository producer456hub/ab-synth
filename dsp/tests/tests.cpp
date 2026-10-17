// tests.cpp - native unit tests for the DSP core. Usage: absynth_tests [patches_dir]
#include "absynth/Engine.h"
#include "absynth/Dual.h"
#include "fft.h"
#include "patch.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

using namespace absynth;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static bool isDenormal(float x) { return x != 0.0f && std::fabs(x) < 1.17549435e-38f; }

// 1. polyBLEP aliasing: harmonic bins vs everything else, compared with a naive saw.
static void testPolyBlepAliasing() {
    std::printf("[1] polyBLEP aliasing (FFT)\n");
    const int N = 4096;
    const float sr = 44100.0f;
    const int harmBin = 160;                        // f0 = sr/N * 160 = 1722.66 Hz -> harmonics land exactly on bins
    const float f0 = sr / N * harmBin;
    auto measure = [&](bool naive) {
        Oscillator o; o.prepare(); o.reset(0.0f); o.setFreq(f0, sr);
        std::vector<std::complex<double>> buf(N);
        for (int i = 0; i < N; ++i) buf[i] = naive ? o.nextNaiveSaw() : o.next(wSaw);
        fft(buf);
        double harm = 0.0, alias = 0.0;
        for (int k = 1; k < N / 2; ++k) {
            const double e = std::norm(buf[k]);
            if (k % harmBin == 0) harm += e; else alias += e;
        }
        return 10.0 * std::log10(alias / harm);
    };
    const double naiveDb = measure(true), blepDb = measure(false);
    std::printf("  naive saw alias/harmonic = %.1f dB, polyBLEP = %.1f dB\n", naiveDb, blepDb);
    // 4-point polyBLEP lands around -37 dB total alias-to-harmonic energy at 1.7 kHz (2-point was -28 dB).
    CHECK(blepDb < -35.0, "polyBLEP alias floor %.1f dB is above -35 dB", blepDb);
    CHECK(blepDb < naiveDb - 20.0, "polyBLEP only %.1f dB better than naive", naiveDb - blepDb);
}

// 2. Ladder stability at max resonance, swept cutoff, hot input.
static void testFilterStability() {
    std::printf("[2] ladder stability at max resonance\n");
    Ladder f; f.reset();
    Rng rng;
    const float fs = 48000.0f * 4;
    float peak = 0.0f; bool bad = false;
    for (int i = 0; i < 48000 * 4 * 2; ++i) {
        const float sweep = 0.5f + 0.5f * std::sin(kTwoPi * (float)i / (48000.0f * 4));
        f.setCoefs(20.0f * std::pow(1000.0f, sweep), 1.0f, fs);
        const float x = rng.nextF() * 4.0f + ((i % 500 < 250) ? 3.0f : -3.0f);
        const float y = f.process(x);
        if (!std::isfinite(y)) { bad = true; break; }
        peak = std::max(peak, std::fabs(y));
    }
    std::printf("  peak output = %.3f\n", peak);
    CHECK(!bad, "non-finite filter output");
    CHECK(peak < 4.0f, "filter output grew to %.3f", peak);
    // self-oscillation check: no input, res max, state kicked
    Ladder g; g.reset(); g.setCoefs(440.0f, 1.0f, 48000.0f);
    g.s[0] = 0.5f;
    float late = 0.0f;
    for (int i = 0; i < 48000; ++i) { const float y = g.process(0.0f); if (i > 40000) late = std::max(late, std::fabs(y)); }
    std::printf("  self-oscillation amplitude after 1 s = %.3f\n", late);
    CHECK(late > 0.05f, "ladder does not self-oscillate at max resonance");
}

// 3. Oversampler sanity: sine round-trip, latency exactly kM.
static void testOversampler() {
    std::printf("[3] oversampler round trip\n");
    for (int R : {2, 4}) {
        Oversampler os; os.setFactor(R);
        const float sr = 48000.0f;
        const int n = 4096;
        std::vector<float> in(n), out(n);
        for (int i = 0; i < n; ++i) in[i] = std::sin(kTwoPi * 1000.0f * i / sr);
        float tmp[4];
        for (int i = 0; i < n; ++i) { os.up(in[i], tmp); out[i] = os.down(tmp); }
        // compare out[i] with in[i - latency]
        const int lat = Oversampler::latencySamples();
        double err = 0.0, sig = 0.0;
        for (int i = 2000; i < n; ++i) { const double d = out[i] - in[i - lat]; err += d * d; sig += in[i - lat] * in[i - lat]; }
        const double db = 10.0 * std::log10(err / sig);
        std::printf("  R=%d: round-trip error = %.1f dB at latency %d\n", R, db, lat);
        CHECK(db < -40.0, "oversampler R=%d round-trip error %.1f dB", R, db);
    }
}

// 4. No NaN / denormals through every patch, including 5 s of silence afterwards.
static void testPatchesFinite(const std::string& dir, const std::vector<std::string>& names) {
    std::printf("[4] no NaN / denormals across patches\n");
    for (auto& nm : names) {
        PatchData p;
        if (!loadPatch(dir + "/" + nm + ".json", p)) { CHECK(false, "cannot load %s", nm.c_str()); continue; }
        for (int prof = 0; prof < 2; ++prof) {
            Engine e; e.prepare(48000.0f, 128);
            e.setProfile(prof ? Profile::Gen1 : Profile::Full);
            applyPatch(e, p);
            std::vector<float> L, R;
            renderPatch(e, p, std::min(p.lengthSec, 6.0) + 5.0, 48000, L, R);
            size_t nan = 0, den = 0; float peak = 0.0f;
            for (size_t i = 0; i < L.size(); ++i) {
                if (!std::isfinite(L[i]) || !std::isfinite(R[i])) ++nan;
                if (isDenormal(L[i]) || isDenormal(R[i])) ++den;
                peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
            }
            std::printf("  %-16s %s: peak %.3f, nan %zu, denormal %zu\n", nm.c_str(), prof ? "Gen1" : "Full", peak, nan, den);
            CHECK(nan == 0, "%s %s produced NaN/inf", nm.c_str(), prof ? "Gen1" : "Full");
            CHECK(den == 0, "%s %s produced denormals", nm.c_str(), prof ? "Gen1" : "Full");
            CHECK(peak > 0.01f, "%s %s is silent", nm.c_str(), prof ? "Gen1" : "Full");
            CHECK(peak < 4.0f, "%s %s peak %.2f is suspiciously hot", nm.c_str(), prof ? "Gen1" : "Full", peak);
        }
    }
}

// 5. Control bass: A vs B null depth (latency aligned). Also with oversampling disabled (mod-rate only).
static double nullDepthDb(const std::string& dir, bool disableOversampling, bool disableModRate) {
    PatchData p;
    if (!loadPatch(dir + "/control_bass.json", p)) return 0.0;
    Engine a, b;
    a.prepare(48000.0f, 128); b.prepare(48000.0f, 128);
    a.setProfile(Profile::Full); b.setProfile(Profile::Gen1);
    if (disableOversampling) b.setConstraint(cOversampling, false);
    if (disableModRate) b.setConstraint(cModRate, false);
    applyPatch(a, p); applyPatch(b, p);
    std::vector<float> la, ra, lb, rb;
    const double secs = std::min(p.lengthSec, 8.0);
    renderPatch(a, p, secs, 48000, la, ra);
    renderPatch(b, p, secs, 48000, lb, rb);
    const int shift = a.latencySamples() - b.latencySamples();
    double sig = 0.0, diff = 0.0;
    for (size_t i = (size_t)std::max(shift, 0); i + 64 < la.size(); ++i) {
        const double xa = la[i], xb = lb[i - shift];
        sig += xa * xa; diff += (xa - xb) * (xa - xb);
    }
    return 10.0 * std::log10(diff / std::max(sig, 1e-20));
}

static void testControlBassNull(const std::string& dir) {
    std::printf("[5] control bass null (Full vs Gen1)\n");
    const double full = nullDepthDb(dir, false, false);
    const double noOs = nullDepthDb(dir, true, false);
    const double noMod = nullDepthDb(dir, false, true);
    const double none = nullDepthDb(dir, true, true);
    std::printf("  all Gen1 constraints on : %.1f dB\n", full);
    std::printf("  oversampling toggle off : %.1f dB (mod-rate only)\n", noOs);
    std::printf("  mod-rate toggle off     : %.1f dB (oversampling only)\n", noMod);
    std::printf("  both off (identical)    : %.1f dB\n", none);
    CHECK(none < -100.0, "identical configurations do not null (%.1f dB)", none);
    CHECK(full < -30.0, "control bass A/B difference is only %.1f dB", full);
}

// 6. Dual: a crossfade mid-note produces no discontinuity larger than the signal's own slope.
static void testDualCrossfade() {
    std::printf("[6] dual crossfade continuity\n");
    Dual d; d.prepare(48000.0f, 128);
    d.setParam(pCutoff, 800.0f);
    d.noteOn(48, 0.8f);
    std::vector<float> L(48000), R(48000);
    for (int i = 0; i < 48000; i += 128) {
        if (i == 24064) d.setMix(1.0f);
        d.process(L.data() + i, R.data() + i, 128);
    }
    float maxStep = 0.0f, maxStepElsewhere = 0.0f;
    for (int i = 1; i < 48000; ++i) {
        const float s = std::fabs(L[i] - L[i - 1]);
        if (i > 24000 && i < 25000) maxStep = std::max(maxStep, s); else if (i > 6000) maxStepElsewhere = std::max(maxStepElsewhere, s);
    }
    std::printf("  max sample step during switch %.4f, elsewhere %.4f\n", maxStep, maxStepElsewhere);
    CHECK(maxStep < maxStepElsewhere * 2.0f + 0.01f, "crossfade introduced a discontinuity");
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "../patches";
    const std::vector<std::string> names = { "control_bass", "supersaw_pad", "screaming_lead", "snappy_pluck", "ambient_tail" };
    testPolyBlepAliasing();
    testFilterStability();
    testOversampler();
    testPatchesFinite(dir, names);
    testControlBassNull(dir);
    testDualCrossfade();
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
