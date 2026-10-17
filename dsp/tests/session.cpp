// session.cpp - multitrack stress test: N engines at once (one per track), every track polyphonic
// with 6-note chords and overlapping releases. Renders the summed session through both profiles,
// writes session WAVs, and prints the per-track cost table + the RK3288 projection.
// Usage: absynth_session <patches_dir> <out_dir> [session_name] [factor]
#include "absynth/Engine.h"
#include "patch.h"
#include "wav.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace absynth;

struct Track {
    std::string name, patch;
    int transpose = 0;
    double hold = 0.9, offsetBeats = 0.0, stabBeats = 0.0, gain = 0.3;
    std::vector<std::pair<int, float>> overrides;
};

// Shared with web/app.js (sessionTrackEvents): keep both in sync.
static std::vector<NoteEvent> trackEvents(const Json& session, const Track& t, double& lengthSec) {
    const double bpm = session.get("bpm") ? session.get("bpm")->number(100) : 100.0;
    const double spb = 60.0 / bpm;
    const double bpc = session.get("beatsPerChord") ? session.get("beatsPerChord")->number(4) : 4.0;
    const Json* prog = session.get("progression");
    std::vector<NoteEvent> ev;
    double lastOff = 0.0;
    for (size_t c = 0; prog && c < prog->size(); ++c) {
        const Json& chord = (*prog)[c];
        const double start = t.offsetBeats + c * bpc;
        const double step = t.stabBeats > 0 ? t.stabBeats : bpc;
        for (double s = 0.0; s < bpc - 1e-9; s += step) {
            const double t0 = (start + s) * spb, t1 = t0 + t.hold * step * spb;
            for (size_t n = 0; n < chord.size(); ++n) {
                const int note = (int)chord[n].number() + t.transpose;
                ev.push_back({ t0, note, 0.8f, true });
                ev.push_back({ t1, note, 0.8f, false });
                lastOff = std::max(lastOff, t1);
            }
        }
    }
    std::stable_sort(ev.begin(), ev.end(), [](const NoteEvent& a, const NoteEvent& b) {
        if (a.time != b.time) return a.time < b.time; return (!a.on) && b.on; });
    const double tail = session.get("tail") ? session.get("tail")->number(2.5) : 2.5;
    lengthSec = std::max(lengthSec, lastOff + tail);
    return ev;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: absynth_session <patches_dir> <out_dir> [session_name] [factor]\n"); return 2; }
    const std::string dir = argv[1], out = argv[2];
    const std::string name = argc > 3 ? argv[3] : "session_8track";
    const double factor = argc > 4 ? std::atof(argv[4]) : 20.0;
    const int sr = 48000;
    Json session;
    if (!parseJsonFile(dir + "/" + name + ".json", session)) { std::printf("cannot load session %s\n", name.c_str()); return 1; }

    std::vector<Track> tracks;
    const Json* tj = session.get("tracks");
    for (size_t i = 0; tj && i < tj->size(); ++i) {
        const Json& j = (*tj)[i];
        Track t;
        if (auto v = j.get("name")) t.name = v->str;
        if (auto v = j.get("patch")) t.patch = v->str;
        if (auto v = j.get("transpose")) t.transpose = (int)v->number();
        if (auto v = j.get("hold")) t.hold = v->number(0.9);
        if (auto v = j.get("offsetBeats")) t.offsetBeats = v->number();
        if (auto v = j.get("stabBeats")) t.stabBeats = v->number();
        if (auto v = j.get("gain")) t.gain = v->number(0.3);
        if (auto v = j.get("params")) for (auto& kv : v->obj) { int id = paramIndex(kv.first.c_str()); if (id >= 0) t.overrides.emplace_back(id, (float)kv.second.number()); }
        tracks.push_back(t);
    }

    double lengthSec = 0.0;
    std::vector<std::vector<NoteEvent>> events;
    for (auto& t : tracks) events.push_back(trackEvents(session, t, lengthSec));
    const size_t total = (size_t)(lengthSec * sr);
    std::vector<float> mixAL(total, 0.0f), mixAR(total, 0.0f), mixBL(total, 0.0f), mixBR(total, 0.0f);

    std::printf("%s: %zu tracks, %.1f s, factor %.0f\n", name.c_str(), tracks.size(), lengthSec, factor);
    std::printf("%-12s %-15s %8s %8s %8s %8s %7s %7s\n", "track", "patch", "A %core", "B %core", "A peakV", "B peakV", "A stl", "B stl");
    double sumA = 0.0, sumB = 0.0;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const Track& t = tracks[i];
        PatchData p;
        if (!loadPatch(dir + "/" + t.patch + ".json", p)) { std::printf("cannot load patch %s\n", t.patch.c_str()); return 1; }
        p.events = events[i];
        double secs[2] = { 0, 0 }; int peakV[2] = { 0, 0 }; uint32_t steals[2] = { 0, 0 };
        for (int prof = 0; prof < 2; ++prof) {
            Engine e; e.prepare((float)sr, 128);
            e.setProfile(prof ? Profile::Gen1 : Profile::Full);
            applyPatch(e, p);
            for (auto& kv : t.overrides) e.setParam(kv.first, kv.second);
            std::vector<float> L(total, 0.0f), R(total, 0.0f);
            // render with voice-count polling (same event logic as renderPatch)
            auto t0 = std::chrono::steady_clock::now();
            size_t pos = 0, ev = 0;
            while (pos < total) {
                size_t next = std::min(total, pos + 128);
                while (ev < p.events.size()) {
                    const size_t ts = (size_t)(p.events[ev].time * sr);
                    if (ts <= pos) { if (p.events[ev].on) e.noteOn(p.events[ev].note, p.events[ev].vel); else e.noteOff(p.events[ev].note); ++ev; }
                    else { next = std::min(next, ts); break; }
                }
                float* o[2] = { L.data() + pos, R.data() + pos };
                e.process(o, (int)(next - pos));
                peakV[prof] = std::max(peakV[prof], e.activeVoices());
                pos = next;
            }
            secs[prof] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            steals[prof] = e.steals();
            std::vector<float>& ML = prof ? mixBL : mixAL; std::vector<float>& MR = prof ? mixBR : mixAR;
            for (size_t k = 0; k < total; ++k) { ML[k] += L[k] * (float)t.gain; MR[k] += R[k] * (float)t.gain; }
        }
        const double loadA = secs[0] / lengthSec * 100.0, loadB = secs[1] / lengthSec * 100.0;
        sumA += loadA; sumB += loadB;
        std::printf("%-12s %-15s %8.1f %8.1f %8d %8d %7u %7u\n", t.name.c_str(), t.patch.c_str(), loadA, loadB, peakV[0], peakV[1], steals[0], steals[1]);
    }
    // common normalization so A and B keep their relative level
    float peak = 0.0f;
    for (size_t k = 0; k < total; ++k) peak = std::max({ peak, std::fabs(mixAL[k]), std::fabs(mixAR[k]), std::fabs(mixBL[k]), std::fabs(mixBR[k]) });
    const float norm = peak > 0.9f ? 0.9f / peak : 1.0f;
    for (size_t k = 0; k < total; ++k) { mixAL[k] *= norm; mixAR[k] *= norm; mixBL[k] *= norm; mixBR[k] *= norm; }
    std::vector<float> dL(total), dR(total);
    for (size_t k = 0; k < total; ++k) { dL[k] = mixAL[k] - mixBL[k]; dR[k] = mixAR[k] - mixBR[k]; }
    writeWav(out + "/" + name + "_A_full.wav", mixAL, mixAR, sr);
    writeWav(out + "/" + name + "_B_gen1.wav", mixBL, mixBR, sr);
    writeWav(out + "/" + name + "_diff.wav", dL, dR, sr);

    const double gen1 = sumB / 100.0 * factor / 3.0;            // fraction of the 3 usable cores
    const double budgetPct = gen1 / 0.70 * 100.0;
    const double perTrackB = sumB / (double)tracks.size();
    const int fitTracks = (int)std::floor(0.70 * 3.0 * 100.0 / (perTrackB * factor));
    std::printf("\nthis machine, single core: A %.0f %% (%.1fx a core), B %.0f %%\n", sumA, sumA / 100.0, sumB);
    std::printf("Gen1 projection x%.0f: B needs %.0f %% of the 70 %% budget on 3 cores -> ~%d tracks of this kind fit\n", factor, budgetPct, fitTracks);
    std::printf("A (Full) on the Gen1 would need %.0f %% of that budget.\n", sumA / 100.0 * factor / 3.0 / 0.70 * 100.0);
    std::printf("mix normalized by %.2f, RMS A %.1f dBFS, B %.1f dBFS, A-B null %.1f dB\n", norm,
                gainToDb((float)rmsOf(mixAL, mixAR)), gainToDb((float)rmsOf(mixBL, mixBR)),
                20.0 * std::log10(rmsOf(dL, dR) / std::max(rmsOf(mixAL, mixAR), 1e-9)));
    return 0;
}
