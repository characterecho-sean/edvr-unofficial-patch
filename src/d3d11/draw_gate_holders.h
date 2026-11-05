// What holds the draw gate open (draw_gate.h): one name per term of the gate's subscription, so the log can say which feature keeps the fast path
// (draw_fast_path.h) from running in a scene. vscreen.cpp's drawGateHolders sets one bit per term and the gate is "any bit" (drawGateSample); the line is
// written when the set changes (and once at the first sample). The sharp_weapons_test rig scans drawGateHolders: every name below is used once,
// and a term added to the gate without a name fails the build. tools\edvr_log.py --perf-windows parses the line; its self-test holds the
// fragments below to this file.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

enum class GateHolder : uint8_t {
    Distance, CensusSkip, QuadSkip, CensusAuto, FssRes, FssPanel, Remlok, Holo, TargetSharp, UiDepth, SunGlare, DrawCensus, BlendProbe, ObjectProbe,
    PanelCurve, Particle, Backdrop, Scrim, QuadProbe, LoaderPanel, IntroPanel, WakePulse, NightVision, SpaceDustHidden, WitchspaceStarsHidden,
    DepthProbe, Footprint, IntroCurve, SrvTurretHud,
    Count
};
constexpr size_t kGateHolders = static_cast<size_t>(GateHolder::Count);
static_assert(kGateHolders <= 32, "the holder set is one 32-bit mask");

inline const char* gateHolderName(size_t h) {
    static const char* const names[kGateHolders] = {
        "distance", "census_skip", "quad_skip", "census_auto", "fss_res", "fss_panel", "remlok", "holo", "target_sharp", "ui_depth", "sun_glare",
        "draw_census", "blend_probe", "object_probe", "panel_curve", "particle", "backdrop", "scrim", "quad_probe", "loader_panel", "intro_panel",
        "wake_pulse", "night_vision", "space_dust_hidden", "witchspace_stars_hidden", "depth_probe", "footprint", "intro_curve", "srv_turret_hud"};
    return h < kGateHolders ? names[h] : "unknown";
}
constexpr uint32_t gateHolderBit(GateHolder h) { return 1u << static_cast<unsigned>(h); }

// "draw gate holders: 3 of 29 terms hold the gate open: ui_depth, sun_glare, footprint." / "...: none (the gate is closed)." Written when the set changes.
inline void formatGateHolders(char* out, size_t cap, uint32_t mask) {
    unsigned n = 0;
    for (size_t h = 0; h < kGateHolders; ++h) n += (mask >> h) & 1u;
    int w = std::snprintf(out, cap, "draw gate holders: %u of %u terms hold the gate open: ", n, unsigned(kGateHolders));
    if (w < 0 || static_cast<size_t>(w) >= cap) return;
    size_t at = static_cast<size_t>(w);
    bool any = false;
    for (size_t h = 0; h < kGateHolders; ++h) {
        if (!((mask >> h) & 1u)) continue;
        w = std::snprintf(out + at, cap - at, "%s%s", any ? ", " : "", gateHolderName(h));
        if (w < 0 || static_cast<size_t>(w) >= cap - at) return;
        at += static_cast<size_t>(w);
        any = true;
    }
    std::snprintf(out + at, cap - at, "%s", any ? "." : "none (the gate is closed).");
}

}  // namespace edvr
