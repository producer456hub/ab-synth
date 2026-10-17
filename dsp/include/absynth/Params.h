// Params.h - the parameter table. Shared by the engine, the patches and the web UI.
#pragma once
#include "Common.h"

namespace absynth {

enum ParamId : int {
    // oscillators
    pOsc1Wave, pOsc1Level, pOsc2Wave, pOsc2Level, pOsc2Semi, pOsc2Fine,
    pUnisonVoices, pUnisonDetune, pUnisonSpread,
    // filter / drive
    pCutoff, pResonance, pFilterEnvAmt, pFilterKeyTrack, pFilterInputGain,
    pDrive, pDriveMix,
    // envelopes
    pAmpA, pAmpD, pAmpS, pAmpR,
    pFilA, pFilD, pFilS, pFilR,
    // lfos
    pLfo1Rate, pLfo1Shape, pLfo1ToPitch, pLfo1ToCutoff,
    pLfo2Rate, pLfo2Shape, pLfo2ToAmp,   pLfo2ToCutoff,
    // voice
    pMono, pGlide,
    // fx
    pChorusMix, pChorusRate, pChorusDepth,
    pDelayTime, pDelayFeedback, pDelayMix, pDelayPingPong, pDelayWobble,
    pReverbSize, pReverbDamp, pReverbMix,
    pMasterGain,
    kNumParams
};

struct ParamInfo { const char* name; float min, max, def; };

// Units: times in seconds, rates in Hz, cutoff in Hz, mod amounts in semitones, mixes 0..1.
inline const ParamInfo& paramInfo(int id) {
    static const ParamInfo table[kNumParams] = {
        {"osc1Wave",        0, 3, 0},      // 0 saw, 1 square, 2 sine, 3 triangle
        {"osc1Level",       0, 1, 1},
        {"osc2Wave",        0, 3, 0},
        {"osc2Level",       0, 1, 0},
        {"osc2Semi",      -24, 24, 0},
        {"osc2Fine",      -100, 100, 0},   // cents
        {"unisonVoices",    1, 7, 1},
        {"unisonDetune",    0, 100, 15},   // cents, total spread
        {"unisonSpread",    0, 1, 0.5f},   // stereo width
        {"cutoff",         20, 20000, 2000},
        {"resonance",       0, 1, 0.2f},
        {"filterEnvAmt",  -96, 96, 0},     // semitones
        {"filterKeyTrack",  0, 1, 0},
        {"filterInputGain", 0, 4, 1},      // linear, >1 pushes the ladder's tanh stages
        {"drive",           0, 1, 0},      // waveshaper amount
        {"driveMix",        0, 1, 1},
        {"ampA", 0.0005f, 10, 0.005f}, {"ampD", 0.001f, 10, 0.2f}, {"ampS", 0, 1, 0.8f}, {"ampR", 0.001f, 10, 0.3f},
        {"filA", 0.0005f, 10, 0.005f}, {"filD", 0.001f, 10, 0.3f}, {"filS", 0, 1, 0.0f}, {"filR", 0.001f, 10, 0.3f},
        {"lfo1Rate",   0.01f, 2000, 5},  {"lfo1Shape", 0, 4, 0}, {"lfo1ToPitch", 0, 12, 0}, {"lfo1ToCutoff", 0, 96, 0},
        {"lfo2Rate",   0.01f, 2000, 1},  {"lfo2Shape", 0, 4, 0}, {"lfo2ToAmp",   0, 1,  0}, {"lfo2ToCutoff", 0, 96, 0},
        {"mono",            0, 1, 0},
        {"glide",           0, 1, 0},      // seconds
        {"chorusMix",       0, 1, 0}, {"chorusRate", 0.05f, 5, 0.5f}, {"chorusDepth", 0, 1, 0.5f},
        {"delayTime",     0.01f, 2, 0.375f}, {"delayFeedback", 0, 0.95f, 0.4f}, {"delayMix", 0, 1, 0},
        {"delayPingPong",   0, 1, 1}, {"delayWobble", 0, 1, 0.2f},
        {"reverbSize",      0, 1, 0.7f}, {"reverbDamp", 0, 1, 0.4f}, {"reverbMix", 0, 1, 0},
        {"masterGain",      0, 2, 0.8f},
    };
    return table[id];
}

inline int paramIndex(const char* name) {
    for (int i = 0; i < kNumParams; ++i)
        if (std::strcmp(paramInfo(i).name, name) == 0) return i;
    return -1;
}

} // namespace absynth
