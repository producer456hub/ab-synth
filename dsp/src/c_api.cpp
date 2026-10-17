// c_api.cpp - flat C ABI over Dual, used by the WebAssembly build (and linkable natively).
// One Dual per wasm instance; the AudioWorklet and the offline worker each get their own.
#include "absynth/Dual.h"
#include "absynth/c_api.h"

using namespace absynth;

namespace {
    Dual* g = nullptr;
    float* outBuf[2] = { nullptr, nullptr };
    int outCap = 0;
}

extern "C" {

int ab_init(float sampleRate, int maxBlock) {
    delete g;
    g = new Dual();
    g->prepare(sampleRate, maxBlock);
    delete[] outBuf[0]; delete[] outBuf[1];
    outCap = std::max(maxBlock, 128);
    outBuf[0] = new float[(size_t)outCap]();
    outBuf[1] = new float[(size_t)outCap]();
    return 1;
}

float* ab_out_ptr(int ch) { return outBuf[ch & 1]; }
int    ab_out_capacity()  { return outCap; }

void ab_process(int n) {
    if (!g) return;
    n = std::min(n, outCap);
    g->process(outBuf[0], outBuf[1], n);
}

// Render ONE engine only (offline measurement / benchmark). inst 0 = A, 1 = B.
void ab_process_single(int inst, int n) {
    if (!g) return;
    n = std::min(n, outCap);
    float* p[2] = { outBuf[0], outBuf[1] };
    g->engine(inst).process(p, n);
}

void ab_set_mix(float x)                   { if (g) g->setMix(x); }
void ab_set_trim_db(int inst, float db)    { if (g) g->setTrimDb(inst, db); }
void ab_note_on(int note, float vel)       { if (g) g->noteOn(note, vel); }
void ab_note_off(int note)                 { if (g) g->noteOff(note); }
void ab_all_notes_off()                    { if (g) g->allNotesOff(); }
void ab_panic()                            { if (g) g->panic(); }
void ab_set_param(int id, float v)         { if (g) g->setParam(id, v); }
float ab_get_param(int id)                 { return g ? g->engine(0).getParam(id) : 0.0f; }

void ab_set_profile(int inst, int profile) { if (g) g->engine(inst).setProfile(profile ? Profile::Gen1 : Profile::Full); }
void ab_set_constraint(int inst, int c, int on) { if (g) g->engine(inst).setConstraint(c, on != 0); }
int  ab_active_voices(int inst)            { return g ? g->engine(inst).activeVoices() : 0; }
int  ab_latency(int inst)                  { return g ? g->engine(inst).latencySamples() : 0; }
int  ab_max_voices(int inst)               { return g ? g->engine(inst).limits().maxVoices : 0; }

int   ab_param_count()                     { return kNumParams; }
const char* ab_param_name(int id)          { return (id >= 0 && id < kNumParams) ? paramInfo(id).name : ""; }
float ab_param_min(int id)                 { return (id >= 0 && id < kNumParams) ? paramInfo(id).min : 0.0f; }
float ab_param_max(int id)                 { return (id >= 0 && id < kNumParams) ? paramInfo(id).max : 0.0f; }
float ab_param_default(int id)             { return (id >= 0 && id < kNumParams) ? paramInfo(id).def : 0.0f; }
int   ab_param_index(const char* name)     { return paramIndex(name); }
int   ab_constraint_count()                { return kNumConstraints; }

} // extern "C"
