// Profile.h - the Full (A) vs Gen1 (B) constraint set. Each constraint has an individual toggle.
#pragma once

namespace absynth {

enum class Profile : int { Full = 0, Gen1 = 1 };

// Individually toggleable Gen1 constraints (only have an effect when the profile is Gen1).
enum Constraint : int {
    cPolyphony = 0,     // 32 -> 8 voices, oldest-note stealing
    cUnison,            // 7 -> 3 unison voices
    cOversampling,      // 4x -> 1x on filter + drive
    cModRate,           // per-sample -> per 32-sample block, linearly interpolated
    cReverb,            // 16-line modulated FDN -> Schroeder/Freeverb-lite
    cDelayInterp,       // cubic -> linear delay interpolation
    kNumConstraints
};

enum class ReverbType : int { FDN = 0, Schroeder = 1 };
enum class DelayInterp : int { Cubic = 0, Linear = 1 };

// Resolved limits the engine actually runs with.
struct Limits {
    int maxVoices    = 32;
    int maxUnison    = 7;
    int oversample   = 4;    // 1, 2 or 4
    int modBlock     = 1;    // samples between modulation updates
    ReverbType reverb = ReverbType::FDN;
    DelayInterp delayInterp = DelayInterp::Cubic;
};

struct ProfileSettings {
    Profile profile = Profile::Full;
    bool enabled[kNumConstraints] = { true, true, true, true, true, true };

    Limits resolve() const {
        Limits l;                             // Full defaults
        if (profile == Profile::Gen1) {
            if (enabled[cPolyphony])    l.maxVoices = 8;
            if (enabled[cUnison])       l.maxUnison = 3;
            if (enabled[cOversampling]) l.oversample = 1;
            if (enabled[cModRate])      l.modBlock = 32;
            if (enabled[cReverb])       l.reverb = ReverbType::Schroeder;
            if (enabled[cDelayInterp])  l.delayInterp = DelayInterp::Linear;
        }
        return l;
    }
};

constexpr int kMaxVoices     = 32;
constexpr int kMaxUnison     = 7;
constexpr int kMaxOversample = 4;
constexpr int kMaxModBlock   = 32;

} // namespace absynth
