// Elite's Supersampling below 1.0 in VR, read from the sizes the DLL measures (design doc
// docs/design-flat-temporal-aa-2026-09-23.md, section 83, "the VR warning").
//
// WHY. In VR, Elite's own Supersampling below 1.0 makes it draw the 3D world into a render target smaller than the eye
// texture and scale it up on the way out, before EDVR ever sees the eye. EDVR's DLSS then upscales an image that is
// already upscaled: the world is soft and the holograms (drawn into the same target) suffer with it. The 2026-09-24
// OpenXR audit chased a DLSS and loading-hologram "regression" that was exactly this (docs/openxr-performance-review-
// 2026-09-14.md, the 2026-09-24 entry). EDVR's own upscaling starts from HMD Image Quality, which scales the eye texture
// and everything drawn into it together, so that is where the user should turn the quality down, with Supersampling at 1.
//
// WHAT IS READ. Never Elite's settings file: vscreen.cpp already measures the size the world is drawn at (the render
// target the busiest frame puts at least kSceneEyeDraws draws into, promoted when it is the eye's shape at a scale:
// vscreen.h eyeShapedAtScale) against the size the headset is handed. When that measured size is under kBelowPercent of
// the eye's width AND of its height, the rig is rendering the world below the eye texture and this says so. An upscaler in
// the chain (FSR or NIS "ultra quality") reads the same, and so the words name Supersampling as the likely cause, not the
// only one. A flat session has no eye texture (the sizes are zero), so none of this ever applies to it.
//
// Pure and dependency-free: tools\vscreen_fit_test drives every function here, and the DLL calls the very same ones.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace vrss {

// The world renders under this share of the eye's width and height (each): Elite's Supersampling steps by 0.05, so 0.95 is
// the smallest below-one setting, and the two percent under 100 absorb a rounded size.
constexpr uint32_t kBelowPercent = 98;

// The world's share of the eye's width, in percent, rounded; 0 when either size is unknown.
inline uint32_t percentOfEye(uint32_t renderW, uint32_t eyeW) {
    if (!renderW || !eyeW) return 0;
    return static_cast<uint32_t>((static_cast<uint64_t>(renderW) * 100u + eyeW / 2u) / eyeW);
}

// Whether the world is rendered below the eye texture on both axes.
inline bool below(uint32_t renderW, uint32_t renderH, uint32_t eyeW, uint32_t eyeH) {
    return renderW && renderH && eyeW && eyeH &&
           static_cast<uint64_t>(renderW) * 100u < static_cast<uint64_t>(eyeW) * kBelowPercent &&
           static_cast<uint64_t>(renderH) * 100u < static_cast<uint64_t>(eyeH) * kBelowPercent;
}

// The four sizes in one 64-bit word, 16 bits each (a size past 65535 cannot be an eye), 0 meaning "no notice": what the
// vScreen module publishes for the menu to read without a lock.
inline uint64_t pack(uint32_t renderW, uint32_t renderH, uint32_t eyeW, uint32_t eyeH) {
    if (!below(renderW, renderH, eyeW, eyeH) || renderW > 0xFFFFu || renderH > 0xFFFFu || eyeW > 0xFFFFu || eyeH > 0xFFFFu)
        return 0;
    return uint64_t(renderW) | uint64_t(renderH) << 16 | uint64_t(eyeW) << 32 | uint64_t(eyeH) << 48;
}
inline bool unpack(uint64_t packed, uint32_t* renderW, uint32_t* renderH, uint32_t* eyeW, uint32_t* eyeH) {
    if (!packed) return false;
    if (renderW) *renderW = static_cast<uint32_t>(packed & 0xFFFFu);
    if (renderH) *renderH = static_cast<uint32_t>(packed >> 16 & 0xFFFFu);
    if (eyeW) *eyeW = static_cast<uint32_t>(packed >> 32 & 0xFFFFu);
    if (eyeH) *eyeH = static_cast<uint32_t>(packed >> 48 & 0xFFFFu);
    return true;
}

// ---- the words ---------------------------------------------------------------------------------------------------------
// The log line, once a session. One line (the log's buffer is 1200 bytes): the measurement, what it does to EDVR's upscaler
// and the holograms, and what to set instead.
inline int formatLog(char* out, size_t size, uint32_t renderW, uint32_t renderH, uint32_t eyeW, uint32_t eyeH) {
    return std::snprintf(out, size,
        "vr supersampling: Elite draws the 3D world at %ux%u, %u%% of the %ux%u eye texture, and scales it up before EDVR "
        "sees it: Elite's Supersampling is below 1 (an upscaler in the chain reads the same). EDVR's DLSS then upscales an "
        "image that is already upscaled, which softens the world and the holograms. Set Elite's Supersampling to 1 and "
        "raise HMD Image Quality instead: EDVR's DLSS upscales from that. Measured from the render sizes, not read from "
        "Elite's settings file.",
        renderW, renderH, percentOfEye(renderW, eyeW), eyeW, eyeH);
}
// The headset toast (one short line: the queue's text must fit one line of the toast card).
inline int formatToast(char* out, size_t size) {
    return std::snprintf(out, size, "Elite Supersampling is below 1: use HMD Image Quality");
}
// The Status page's hint: the two-line area under its lines, which the page has whatever it says, so this replaces the text and
// adds no line. That is the point of it being the one place the open menu says this. The menu bitmap is refused above 2048 px
// (menu_panel.cpp, rasterise) and the panel then keeps its old bitmap: a settings page with a note under its nine rows
// (sixteen lines and a hint, 42.6 caps) trips it from a 48-px cap, the Status page with one more line (sixteen at 1.7 caps)
// from 54, and the Pimax at the default 1.1 deg (~46-50 ppd) has a 51-55 px cap, so a note or a line would freeze the panel on
// exactly the headsets that run Supersampling below 1. At most 78 characters, the length of the hint it replaces.
inline int formatStatusHint(char* out, size_t size) {
    return std::snprintf(out, size, "Elite Supersampling is below 1: set it to 1 and use HMD Image Quality.");
}

}  // namespace vrss
}  // namespace edvr
