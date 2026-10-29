#pragma once
// The runtime's half of the scope zoom's window (docs\design-vr-scope-magnification-2026-10-28.md, section 11; the request is src\common\scope_zoom_window.h). The pure parts: whether
// the left eye's frustum is narrowed this frame and to what, and one press of it as the runtime did it, which goes back to the d3d11 half as an EdvrNativeScopeZoomReport.
//
// The narrowing is only the TOLD frustum of eye 0, set in WaitGetPoses where the FOV trim sets its own: tangents only, no pose, no size, no eye-to-head. The game draws eye 0 with it. What
// stays true: the located views the compositor lays the layer on, the raw projection getter's answer for eye 0 (which the game's UI scale reads), the frusta the temporal pass announces,
// and the hidden-area mesh (cut for the true frustum; withheld while the zoom stands). It is applied only while the right eye's picture wholly covers the left eye (the share at full
// weight, drawn the frame before), because the zoomed picture is never meant to be seen; otherwise the left eye keeps its own view and the reason is counted.
#include "projection_math.h"
#include "../common/scope_zoom_window.h"

#include <cmath>
#include <cstdint>

namespace edvr::openxr {

enum class ScopeZoomWhy : uint32_t { kApplied = 0, kNotAsked = 1, kVersion = 2, kShare = 3, kShareNotDrawn = 4, kGeometry = 5 };
// Index into EdvrNativeScopeZoomReport::refused for a reason that is a refusal (not kApplied, not kNotAsked).
inline uint32_t scopeZoomRefusalIndex(ScopeZoomWhy w) { return static_cast<uint32_t>(w) - static_cast<uint32_t>(ScopeZoomWhy::kVersion); }

struct ScopeZoomInput {
    scopewin::Request request;
    bool answerCarries = false;     // the provider's answer is version 11 (the words are believed)
    float share = 0.f;              // this frame's left eye share
    bool shareDrawn = false;        // the renderer drew the share in its last compose
    bool shareRefused = false;      // ... or refused it (views it could not map)
    XrFovf base{};                  // eye 0's told frustum without the zoom (the trim's, or the located one)
};
struct ScopeZoomOutput {
    ScopeZoomWhy why = ScopeZoomWhy::kNotAsked;
    XrFovf fov{};                   // the told frustum with the zoom (the base when it is not applied)
    double trueTan[4] = {}, zoomTan[4] = {};   // left, right, down, up tangents
    double narrowing = 1.0;         // the true vertical span over the zoomed one
    float strip = 0.f;              // how black the left eye's uncovered strip is this frame
};
inline ScopeZoomOutput decideScopeZoom(const ScopeZoomInput& in) {
    ScopeZoomOutput o;
    o.fov = in.base;
    o.strip = in.answerCarries ? scopewin::clamp01(in.request.strip) : 0.f;
    if (!in.answerCarries) { o.why = in.request.zoom ? ScopeZoomWhy::kVersion : ScopeZoomWhy::kNotAsked; return o; }
    if (!in.request.zoom) return o;   // kNotAsked
    // the share must wholly cover the left eye, and have been drawn the frame before: a zoomed picture that could be seen is never made
    if (!(in.share >= 0.999f)) { o.why = ScopeZoomWhy::kShare; return o; }
    if (!in.shareDrawn || in.shareRefused) { o.why = ScopeZoomWhy::kShareNotDrawn; return o; }
    RawFov raw{};
    if (!fovToRaw(in.base, raw)) { o.why = ScopeZoomWhy::kGeometry; return o; }
    const double trueTan[4] = {raw.left, raw.right, raw.top, raw.bottom};   // RawFov: top is the -Y ray (down), bottom the +Y ray (up)
    const double tc[2] = {in.request.tc[0], in.request.tc[1]};
    double zoomTan[4];
    if (!scopewin::window(trueTan, tc, static_cast<double>(in.request.m), zoomTan)) { o.why = ScopeZoomWhy::kGeometry; return o; }
    XrFovf fov{};
    fov.angleLeft = static_cast<float>(std::atan(zoomTan[0]));
    fov.angleRight = static_cast<float>(std::atan(zoomTan[1]));
    fov.angleDown = static_cast<float>(std::atan(zoomTan[2]));
    fov.angleUp = static_cast<float>(std::atan(zoomTan[3]));
    RawFov check{};
    if (!fovToRaw(fov, check)) { o.why = ScopeZoomWhy::kGeometry; return o; }
    o.why = ScopeZoomWhy::kApplied;
    o.fov = fov;
    for (int i = 0; i < 4; ++i) { o.trueTan[i] = trueTan[i]; o.zoomTan[i] = zoomTan[i]; }
    o.narrowing = (trueTan[3] - trueTan[2]) / (zoomTan[3] - zoomTan[2]);
    return o;
}
// The vertical angle a RawFov spans (atan |up| + atan |down|), degrees: what the game's fov getter reads, which its UI scale divides by.
inline float rawVerticalDeg(const RawFov& r) {
    return static_cast<float>((std::atan(std::fabs(static_cast<double>(r.top))) + std::atan(std::fabs(static_cast<double>(r.bottom)))) * 180.0 / 3.14159265358979323846);
}

// One press of the zoom: from the first zoomed frame to the first frame without one.
struct ScopeZoomPress {
    bool open = false;
    uint64_t firstUs = 0, lastUs = 0;
    uint32_t frames = 0, refused[4] = {}, stripFrames = 0, meshWithheld = 0;
    float peakM = 0.f;
    double trueTan[4] = {}, zoomTan[4] = {};
    // the game's reads of the projection for eye 0 while the zoom stood, taken from the system object's counters at the press's edges
    uint32_t rawReads = 0, matrixReads = 0;
    float rawVerticalDeg = 0.f, matrixVerticalDeg = 0.f;
    void begin(uint64_t nowUs) { *this = ScopeZoomPress{}; open = true; firstUs = lastUs = nowUs; }
    void applied(const ScopeZoomOutput& o, uint64_t nowUs, bool stripDrawn, bool meshWithheldNow) {
        ++frames;
        lastUs = nowUs;
        if (stripDrawn) ++stripFrames;
        if (meshWithheldNow) ++meshWithheld;
        if (static_cast<float>(o.narrowing) > peakM) peakM = static_cast<float>(o.narrowing);
        for (int i = 0; i < 4; ++i) { trueTan[i] = o.trueTan[i]; zoomTan[i] = o.zoomTan[i]; }
    }
    void refusedBecause(ScopeZoomWhy w) {
        if (w == ScopeZoomWhy::kApplied || w == ScopeZoomWhy::kNotAsked) return;
        ++refused[scopeZoomRefusalIndex(w)];
    }
    void fill(EdvrNativeScopeZoomReport& r) const {
        r = EdvrNativeScopeZoomReport{};
        r.size = sizeof(r);
        r.version = EDVR_NATIVE_SCOPE_ZOOM_REPORT_VERSION_1;
        r.frames = frames;
        for (int i = 0; i < 4; ++i) { r.refused[i] = refused[i]; r.trueTan[i] = static_cast<float>(trueTan[i]); r.zoomTan[i] = static_cast<float>(zoomTan[i]); }
        r.stripFrames = stripFrames;
        r.seconds = static_cast<float>(static_cast<double>(lastUs - firstUs) / 1.0e6);
        r.peakM = peakM;
        r.rawReads = rawReads;
        r.matrixReads = matrixReads;
        r.rawVerticalDeg = rawVerticalDeg;
        r.matrixVerticalDeg = matrixVerticalDeg;
        r.meshWithheld = meshWithheld;
    }
};

}  // namespace edvr::openxr
