// patch.h - load a /patches JSON file (params + stored phrase) and render it through an Engine.
#pragma once
#include "json.h"
#include "absynth/Engine.h"
#include <algorithm>
#include <string>
#include <vector>

struct NoteEvent { double time; int note; float vel; bool on; };

struct PatchData {
    std::string name;
    std::vector<std::pair<int, float>> params;
    std::vector<NoteEvent> events;   // sorted by time (seconds)
    double lengthSec = 0.0;          // last note-off + tail
    double tailSec = 2.0;
};

// Patch JSON: { "name", "params": {name: value}, "bpm", "phrase": [[beat, durBeats, note, vel0to127], ...],
//               "repeat": n, "loopBeats": beats, "tail": seconds }
inline bool loadPatch(const std::string& path, PatchData& out) {
    Json j;
    if (!parseJsonFile(path, j)) return false;
    out = PatchData();
    if (auto n = j.get("name")) out.name = n->str;
    if (auto p = j.get("params")) {
        for (auto& kv : p->obj) {
            const int id = absynth::paramIndex(kv.first.c_str());
            if (id >= 0) out.params.emplace_back(id, (float)kv.second.number());
        }
    }
    const double bpm = j.get("bpm") ? j.get("bpm")->number(120.0) : 120.0;
    const double secPerBeat = 60.0 / bpm;
    const int repeat = j.get("repeat") ? (int)j.get("repeat")->number(1) : 1;
    const double loopBeats = j.get("loopBeats") ? j.get("loopBeats")->number(0) : 0.0;
    out.tailSec = j.get("tail") ? j.get("tail")->number(2.0) : 2.0;
    double lastOff = 0.0;
    if (auto ph = j.get("phrase")) {
        for (int r = 0; r < repeat; ++r) {
            const double off = r * loopBeats * secPerBeat;
            for (size_t i = 0; i < ph->size(); ++i) {
                const Json& e = (*ph)[i];
                if (e.size() < 3) continue;
                const double t0 = off + e[0].number() * secPerBeat;
                const double t1 = t0 + e[1].number() * secPerBeat;
                const int note = (int)e[2].number();
                const float vel = (e.size() > 3) ? (float)e[3].number(100) / 127.0f : 0.8f;
                out.events.push_back({ t0, note, vel, true });
                out.events.push_back({ t1, note, vel, false });
                lastOff = std::max(lastOff, t1);
            }
        }
    }
    std::stable_sort(out.events.begin(), out.events.end(), [](const NoteEvent& a, const NoteEvent& b) {
        if (a.time != b.time) return a.time < b.time;
        return (!a.on) && b.on;   // note-offs before note-ons at the same instant
    });
    out.lengthSec = lastOff + out.tailSec;
    return true;
}

inline void applyPatch(absynth::Engine& e, const PatchData& p) {
    for (int i = 0; i < absynth::kNumParams; ++i) e.setParam(i, absynth::paramInfo(i).def);
    for (auto& kv : p.params) e.setParam(kv.first, kv.second);
}

// Render the patch's phrase into L/R. Events are applied sample-accurately at sub-block boundaries.
inline void renderPatch(absynth::Engine& e, const PatchData& p, double seconds, int sampleRate,
                        std::vector<float>& L, std::vector<float>& R, int block = 128) {
    const size_t total = (size_t)(seconds * sampleRate);
    L.assign(total, 0.0f); R.assign(total, 0.0f);
    size_t pos = 0, ev = 0;
    while (pos < total) {
        size_t next = std::min(total, pos + (size_t)block);
        while (ev < p.events.size()) {
            const size_t t = (size_t)(p.events[ev].time * sampleRate);
            if (t <= pos) { if (p.events[ev].on) e.noteOn(p.events[ev].note, p.events[ev].vel); else e.noteOff(p.events[ev].note); ++ev; }
            else { next = std::min(next, t); break; }
        }
        float* out[2] = { L.data() + pos, R.data() + pos };
        e.process(out, (int)(next - pos));
        pos = next;
    }
}

inline double rmsOf(const std::vector<float>& a, const std::vector<float>& b) {
    double acc = 0.0;
    for (size_t i = 0; i < a.size(); ++i) acc += (double)a[i] * a[i] + (double)b[i] * b[i];
    return std::sqrt(acc / (double)std::max<size_t>(1, 2 * a.size()));
}
