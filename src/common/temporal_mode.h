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
