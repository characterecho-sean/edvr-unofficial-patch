// The re-issue counter's names and window line (GLOSSARY: Re-issue; perf_monitor.cpp prints the line every 1800 frames beside "draw hook CPU:", vscreen.cpp's
// reissue helper feeds it). Every re-issue of a game draw goes through one helper that names its caller, so the draw path's time that is not EDVR's own code gets an
// owner. Written every window, zero re-issues included (0 re-issues, "none"), so a window with nothing in it can be told from a build without the counter.
// tools\edvr_log.py --tally reissue parses the line; its self-test holds the fragments below to this file.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {

// One name per caller of the helper. The log prints the name beside its ms and calls, so a name is a word a person can search the source for.
enum class ReissueCaller : uint8_t {
    ScreenMotion,    // screenMotionDraw: the 2D screen composite's motion vectors
    ScreenMotionUi,  // screenMotionUiDraw: the interface's motion vectors
    WeaponMotion,    // weaponMotionDraw
    PanelCurve,      // the curved panel strip (panelCurveSubstitute, panelCurveReissue, panelCurveSurfaceDraw)
    LoaderPanel,     // loaderPanelSubstitute
    QuadSplit,       // forwardQuadSkip's surviving ranges
    FlatUi,          // the flat profile's uiReissue (the layer's second issues and the tonemap in flat)
    UiDepth,         // ui_depth's interface, hologram, planet and world-screen depth passes
    StarGlow,        // visual.star_glow's second issue of a star pass
    BlendProbe,      // the blend probe's replay
    CrispHud,        // fix.ui_quality's crisp-HUD tonemap re-issue
    Tonemap,         // ...and the weapons fire's extra steps through the same tonemap draw
    WorldScreen,     // the VR world route's 2D screen re-issue
    UiLayerSecond,   // uiLayerSecondIssues: the multiply and the write-back
    SplashDim,       // the splash screen's dim
    Count
};
constexpr size_t kReissueCallers = static_cast<size_t>(ReissueCaller::Count);

inline const char* reissueCallerName(size_t c) {
    static const char* const names[kReissueCallers] = {"screen_motion", "screen_motion_ui", "weapon_motion", "panel_curve", "loader_panel", "quad_split", "flat_ui",
                                                       "ui_depth", "star_glow", "blend_probe", "crisp_hud", "tonemap", "world_screen", "ui_layer_second", "splash_dim"};
    return c < kReissueCallers ? names[c] : "unknown";
}

// ms[c] is the caller's time summed over the window's sampled frames; calls[c] its re-issues on those frames. `totalCalls` counts every frame (so a re-issue on an unsampled
// frame still shows the helper ran). The mean is a mean over sampled frames, the unit "draw hook CPU:" uses.
inline void formatReissueWindow(char* out, size_t cap, unsigned frame, double meanMs, double maxMs, unsigned samples, unsigned every, unsigned totalCalls,
                                unsigned sampledCalls, const double* ms, const unsigned* calls) {
    int n = std::snprintf(out, cap,
                          "draw reissue CPU: 1800-frame window ending %u; %.3f ms/sampled frame mean, %.3f ms max, %u sampled frames (one in %u); %u re-issues in the window, "
                          "%u of them on sampled frames; every re-issue of a sampled frame is timed, so nothing is scaled; excludes the forwarded game draw, inside the draw hook CPU figure. "
                          "Callers (ms a sampled frame, calls on sampled frames): ",
                          frame, meanMs, maxMs, samples, every, totalCalls, sampledCalls);
    if (n < 0 || static_cast<size_t>(n) >= cap) return;
    size_t at = static_cast<size_t>(n);
    bool any = false;
    for (size_t c = 0; c < kReissueCallers; ++c) {
        if (!calls[c]) continue;
        n = std::snprintf(out + at, cap - at, "%s%s %.3f ms %u", any ? ", " : "", reissueCallerName(c), samples ? ms[c] / samples : 0.0, calls[c]);
        if (n < 0 || static_cast<size_t>(n) >= cap - at) return;
        at += static_cast<size_t>(n);
        any = true;
    }
    std::snprintf(out + at, cap - at, "%s.", any ? "" : "none");
}

}  // namespace edvr
