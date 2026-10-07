#pragma once

#include <cstring>
#include <cmath>
#include <string>

namespace edvr {

// Every temporal mode needs the same depth and rigid-object motion inputs.
// Keep their producers coupled to the mode, including live on/off changes.
inline bool temporalModeEnabled(const std::string& mode) {
    return _stricmp(mode.c_str(), "on") == 0 ||
           _stricmp(mode.c_str(), "dlaa") == 0 ||
           _stricmp(mode.c_str(), "dlss") == 0 ||
           _stricmp(mode.c_str(), "fsr") == 0;
}

struct TemporalPresetSelection {
    unsigned full = 11, fovea = 11;
    bool known = true;
};
// Shared model names; feature creation still applies the backend's DLAA guard.
inline TemporalPresetSelection temporalPresetFor(const std::string& model) {
    if (_stricmp(model.c_str(), "k") == 0 || _stricmp(model.c_str(), "quality") == 0)
        return {11, 11, true};
    if (_stricmp(model.c_str(), "steady") == 0) return {11, 12, true};
    if (_stricmp(model.c_str(), "auto") == 0 || _stricmp(model.c_str(), "default") == 0)
        return {0, 0, true};
    if (_stricmp(model.c_str(), "j") == 0 || _stricmp(model.c_str(), "responsive") == 0)
        return {10, 10, true};
    if (_stricmp(model.c_str(), "l") == 0) return {12, 12, true};
    if (_stricmp(model.c_str(), "m") == 0) return {13, 13, true};
    return {11, 11, model.empty()};
}

inline constexpr float kTemporalShipMetres = 10.0f;

// ---- The ship split on foot ----------------------------------------------------------------------------------------------
// The temporal pass tells what moves with the head from what moves with the camera by DISTANCE: a pixel nearer than the
// split (advanced.temporal_aa_ship_metres, kTemporalShipMetres) is the ship's -- the cockpit, the hull -- and takes the
// head's delta alone; every pixel farther, and the far plane, takes the camera's own (the head and the ship together, the
// game's view rows). That is right in a cockpit, where the near things ride with the head. On foot there is no ship: in
// Explorer Cam, the one on-foot mode Elite draws in stereo through the eye path, the camera walks with the commander and the
// ground 1.5 to 10 m away is the world, not a cockpit. Eye dump 090359 (Steam, v0.18.2-90-g6c63f6aa) had 55% of its decision
// crop on the head path -- the ground within 10 m, 53.5% of the frame, no cockpit stencil anywhere -- and the head path
// missed the camera's whole walk: by block matching the raw crops its prediction was off by 24.9 px at the median, the camera's
// own rows by 0.84 px (docs/per-object-motion.md, 2026-10-07).
//
// So while the game says the commander is on foot, and not seated in a ship, a fighter, an SRV, a taxi or someone else's ship,
// the split is a millimetre: nothing a camera can draw is nearer than its 0.025 m near plane, so every pixel with a depth takes
// the camera's rows and only the far plane is as it was. The shader is not touched. The value is a TINY POSITIVE number and
// never zero: the shader reads split.x == 0 as "the world path is off" and would put EVERYTHING on the head path.
inline constexpr float kTemporalOnFootSplitMetres = 0.001f;

// The journal watcher's Status.json read count must have moved this recently, or what it says is stale. The worker reads every
// 500 ms (100 ms eager), so five seconds is ten reads missed: a stuck worker, not a quiet game (a successful read counts
// whether or not the file changed).
inline constexpr unsigned kTemporalFootStaleMs = 5000;

// What the journal watcher says about the commander, as one value (src/d3d11/journal_watch.h reads it from Status.json).
struct TemporalFoot {
    bool watching = false;       // the watcher is reading at all (journalWatchActive): off, no folder or faults spent = false
    bool gameplay = false;       // LoadGame seen in THIS process's journal: before it Status.json may be the last session's
    bool known = false;          // the last Status.json carried Flags2 (a menu's, or no file, does not)
    bool onFoot = false;         // Flags2 bit 0
    bool vehicleKnown = false;   // ...and Flags
    bool seated = false;         // Flags bit 24 (main ship), 25 (fighter) or 26 (SRV); Flags2 bit 1 (taxi) or 2 (multicrew)
    unsigned sampleAgeMs = 0;    // since the watcher's read count last moved
};

// Why the split is as it is. Everything but OnFoot leaves the split exactly as configured -- the fail-safe direction: the
// cockpit's rule is the one in force wherever the answer is anything but a clear yes.
enum class TemporalFootWhy : unsigned char {
    OnFoot,        // yes: the ship split is off
    NotWatching,   // the journal is not being read: cannot tell
    NoGameplay,    // no LoadGame in this process yet: the file may be the last session's
    Unknown,       // Status.json carried no Flags2 (a menu) or no Flags
    Stale,         // the watcher's reads stopped: what it last said is not now
    Seated,        // in a ship, a fighter, an SRV, a taxi or multicrew
    NotOnFoot,     // Flags2 says not on foot, and nothing seats the commander
};

inline TemporalFootWhy temporalFootVerdict(const TemporalFoot& f) {
    if (!f.watching) return TemporalFootWhy::NotWatching;
    if (!f.gameplay) return TemporalFootWhy::NoGameplay;
    if (!f.known || !f.vehicleKnown) return TemporalFootWhy::Unknown;
    if (f.sampleAgeMs > kTemporalFootStaleMs) return TemporalFootWhy::Stale;
    if (f.seated) return TemporalFootWhy::Seated;
    if (!f.onFoot) return TemporalFootWhy::NotOnFoot;
    return TemporalFootWhy::OnFoot;
}

inline bool temporalOnFoot(const TemporalFoot& f) { return temporalFootVerdict(f) == TemporalFootWhy::OnFoot; }

inline const char* temporalFootWhyName(TemporalFootWhy w) {
    switch (w) {
        case TemporalFootWhy::OnFoot: return "on foot";
        case TemporalFootWhy::NotWatching: return "the journal is not being read";
        case TemporalFootWhy::NoGameplay: return "no LoadGame in this session's journal yet";
        case TemporalFootWhy::Unknown: return "no Flags2 in Status.json (a menu, or no file yet)";
        case TemporalFootWhy::Stale: return "the Status.json reads stopped";
        case TemporalFootWhy::Seated: return "in a ship, an SRV or another seat";
        case TemporalFootWhy::NotOnFoot: return "Flags2 says not on foot";
    }
    return "?";
}

// The split the shader is given. A split of zero is the world path OFF (advanced.temporal_aa_ship_metres = 0), and on foot it
// stays off: the mode never turns on what the player turned off.
inline float temporalShipSplitMetres(float configured, bool onFoot) {
    return onFoot && configured > 0.0f ? kTemporalOnFootSplitMetres : configured;
}

// A gap between two questions longer than this is the asker's own stall -- a load, a hitch of seconds -- and not the watcher's
// silence: the watcher's count moves at the frame boundary's tick, so the first answer after a stall can still hold the count
// from before it.
inline constexpr unsigned kTemporalFootGapMs = 1000;

// The verdict over time: the staleness clock (the age of the watcher's read count, across continuous looking only), the mode and
// the changes of it, as one value so a rig can step it through a scripted timeline. The pass asks it once per eye evaluation.
struct TemporalFootTracker {
    unsigned long long samplesMs = 0;   // when the read count last moved, or the clock was last restarted
    unsigned long long callMs = 0;      // when it was last asked
    unsigned samples = 0;               // the read count as last seen
    unsigned changes = 0;               // changes of mode so far
    bool started = false;               // asked at least once: the clock's start is the first question's
    bool on = false;                    // the mode now
    TemporalFootWhy why = TemporalFootWhy::NotWatching;   // ...and the verdict's reason

    // `f.sampleAgeMs` is ignored: the age is this tracker's own. True when the mode changed on this step.
    bool step(unsigned long long nowMs, TemporalFoot f, unsigned readCount) {
        if (!started || readCount != samples || nowMs - callMs > kTemporalFootGapMs) {
            started = true;
            samples = readCount;
            samplesMs = nowMs;
        }
        callMs = nowMs;
        const unsigned long long age = nowMs - samplesMs;
        f.sampleAgeMs = age > 0x7FFFFFFFull ? 0x7FFFFFFFu : static_cast<unsigned>(age);
        why = temporalFootVerdict(f);
        const bool now = why == TemporalFootWhy::OnFoot;
        if (now == on) return false;
        on = now;
        ++changes;
        return true;
    }
};

// Which history the temporal pass hands the frame to. Own is the pass's own
// clip; Nvidia and Amd are the two external, trained upscalers (docs\
// fsr-upscaler-design-2026-09-16.md, section 3). Anything the mode string
// does not name (off included) reads as Own: off never reaches the seam,
// and an unrecognised value already falls back to the pass's own history
// (temporalModeEnabled above), so there is nothing else to run.
enum class TemporalEngine { Own, Nvidia, Amd };

inline TemporalEngine temporalEngineFor(const std::string& mode) {
    if (_stricmp(mode.c_str(), "dlaa") == 0 || _stricmp(mode.c_str(), "dlss") == 0) {
        return TemporalEngine::Nvidia;
    }
    if (_stricmp(mode.c_str(), "fsr") == 0) return TemporalEngine::Amd;
    return TemporalEngine::Own;
}

// True for every mode that hands the frame to an external, trained engine
// (NVIDIA's or AMD's) rather than the pass's own history -- the test every
// "a trained engine wants X" reader in src\ shares, so fsr reaches UI depth
// and the rest the same way dlss and dlaa do.
inline bool temporalExternalEngine(const std::string& mode) {
    return temporalEngineFor(mode) != TemporalEngine::Own;
}

// A display name only: automatic NVIDIA mode remains "dlss" in the ini.
inline const char* temporalNvidiaLabel(float hmdQuality) {
    if (!std::isfinite(hmdQuality) || hmdQuality <= 0.0f) return "DLSS / DLAA";
    return hmdQuality >= 1.0f ? "DLAA" : "DLSS";
}

}  // namespace edvr
