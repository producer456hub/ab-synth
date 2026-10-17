// render.cpp - offline render tool. For each patch: A.wav (Full), B.wav (Gen1), diff.wav (latency-aligned null).
// Usage: absynth_render <patches_dir> <out_dir> [patch_name ...]
#include "absynth/Engine.h"
#include "patch.h"
#include "wav.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace absynth;

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: absynth_render <patches_dir> <out_dir> [patch_name ...]\n"); return 2; }
    const std::string dir = argv[1], out = argv[2];
    std::vector<std::string> names;
    for (int i = 3; i < argc; ++i) names.push_back(argv[i]);
    if (names.empty()) names = { "control_bass", "supersaw_pad", "screaming_lead", "snappy_pluck", "ambient_tail" };
    const int sr = 48000;
    int rc = 0;
    std::printf("%-16s %9s %9s %9s %10s %10s\n", "patch", "rmsA dB", "rmsB dB", "null dB", "A xRT", "B xRT");
    for (auto& nm : names) {
        PatchData p;
        if (!loadPatch(dir + "/" + nm + ".json", p)) { std::printf("cannot load %s\n", nm.c_str()); rc = 1; continue; }
        Engine a, b;
        a.prepare((float)sr, 128); b.prepare((float)sr, 128);
        a.setProfile(Profile::Full); b.setProfile(Profile::Gen1);
        applyPatch(a, p); applyPatch(b, p);
        std::vector<float> la, ra, lb, rb;
        auto t0 = std::chrono::steady_clock::now();
        renderPatch(a, p, p.lengthSec, sr, la, ra);
        auto t1 = std::chrono::steady_clock::now();
        renderPatch(b, p, p.lengthSec, sr, lb, rb);
        auto t2 = std::chrono::steady_clock::now();
        const double secA = std::chrono::duration<double>(t1 - t0).count();
        const double secB = std::chrono::duration<double>(t2 - t1).count();
        const int shift = a.latencySamples() - b.latencySamples();
        std::vector<float> dl(la.size(), 0.0f), dr(la.size(), 0.0f);
        double sig = 0.0, diff = 0.0;
        for (size_t i = (size_t)std::max(shift, 0); i < la.size(); ++i) {
            dl[i] = la[i] - lb[i - shift]; dr[i] = ra[i] - rb[i - shift];
            sig += (double)la[i] * la[i] + (double)ra[i] * ra[i];
            diff += (double)dl[i] * dl[i] + (double)dr[i] * dr[i];
        }
        const double nullDb = 10.0 * std::log10(diff / std::max(sig, 1e-20));
        writeWav(out + "/" + nm + "_A_full.wav", la, ra, sr);
        writeWav(out + "/" + nm + "_B_gen1.wav", lb, rb, sr);
        writeWav(out + "/" + nm + "_diff.wav", dl, dr, sr);
        std::printf("%-16s %9.1f %9.1f %9.1f %10.1f %10.1f\n", nm.c_str(),
                    gainToDb((float)rmsOf(la, ra)), gainToDb((float)rmsOf(lb, rb)), nullDb,
                    p.lengthSec / secA, p.lengthSec / secB);
    }
    return rc;
}
