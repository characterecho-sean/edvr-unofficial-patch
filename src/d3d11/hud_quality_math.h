// The pure arithmetic behind fix.hud_quality, kept apart from fss_res.cpp's
// Config/Log/device_hook/ui_depth wiring so a rig can test it without a
// device, a config file or the other modules' own state (tools/hud_quality_test).
//
// WHAT THE KEY MEANS. fix.hud_quality names a TARGET HMD Quality (1.0 or
// 1.25, off is the default). The game already scales the cockpit's interface
// surfaces -- the vector, text and icon draws -- as a fixed fraction of its
// internal render resolution (fss_res.h, the surface_inflate header comment),
// so HMD Quality 0.7 rasterises those panels smaller than HMD Quality 1.0
// would. This asks the SAME surface_inflate mechanism (generalised from an
// integer 2..4 to a float) to create them at the size the game would have
// used at the target quality instead, independent of what the player's own
// HMD Quality slider says.
#pragma once

#include <cstdint>

namespace edvr {

// "off" | "1.0" | "1.25" -> 0.0f (off) | 1.0f | 1.25f. Anything else is
// refused as off; *recognized (when given) is set false so the caller can
// log why. Comparison is exact-text, the same as fix.settlement_detail's
// choices -- no float parsing of the key itself, so "1.00" is not "1.0".
float hudQualityParseTarget(const char* text, bool* recognized);

// factorOut = target / hmdMultiplier, capped at 4 (the same ceiling
// advanced.surface_inflate uses, for the same reason: 4x is already sixteen
// times the pixels of a surface that may not even be the right one). False,
// *factorOut untouched, when there is nothing worth doing:
//   - target <= 0 (the key is off)
//   - hmdMultiplier <= 0 (unknown -- no fxcfg read, deviceHookHmdQuality
//     said so)
//   - the resulting factor does not exceed 1.0 by more than 1%: HMD Quality
//     already at or above the target, so the game's own surface is already
//     the size (or bigger) than asked for -- the settlement_detail_max
//     style "moving the ceiling below the floor changes nothing" case.
bool hudQualityFactor(float target, float hmdMultiplier, float* factorOut);

// round(v * factor) -- a texture's width or height, which must land on an
// exact pixel count, and (cast back from LONG at the call site) a scissor
// rect's corner, which must move by the same rule the viewport already
// does or the two disagree about where the panel's edge is.
uint32_t hudQualityRoundDim(uint32_t v, float factor);

// The index of the first learned size that exactly matches (w, h), or -1.
// Linear scan: n is at most ui_depth's own learned-surface ring (64), and
// this runs once per CreateTexture2D while the key is on, not once a frame.
// Used only as fix.hud_quality's cross-check counter now (how many of what
// it resized by ratio the classifier also independently recognised) --
// see hudQualityRatioObserve below for the actual (post-flight) matcher.
int hudQualityMatchLearned(uint32_t w, uint32_t h, const uint32_t* learnedW,
                           const uint32_t* learnedH, uint32_t n);

// --- ratio matching (replaces the classifier as the primary matcher) ------
//
// WHY: the classifier (ui_depth.cpp) learns a surface's size from DRAWS INTO
// it, which happen AFTER CreateTexture2D -- too late to size the create
// itself. A panel created before any draw into it (every session's first
// one) can never match through the classifier. fss_res.h's own census found
// that interface surfaces are a FIXED FRACTION of the game's internal
// render resolution, stable to four significant figures across sessions at
// different resolutions, so that fraction -- not anything learned from a
// draw -- is what CreateTexture2D can be matched against, from the first
// frame of the first session with fix.temporal_aa off.
//
// A ratio is trusted only once it has been seen at two DIFFERENT internal
// resolutions and agreed to a small tolerance -- one session's number is a
// coincidence waiting to happen, two sessions at different sizes agreeing
// is the fraction fss_res.h measured. Unverified candidates are recorded,
// not used ("seen, not resized").

// round(dim * 10000 / internalDim) -- the ratio in ten-thousandths, four
// significant figures. 0 when internalDim is 0 (nothing to divide by).
uint32_t hudQualityRatioX10000(uint32_t dim, uint32_t internalDim);

// Is `candidate` within `tolerance` ten-thousandths of `known`? Exact
// integer equality is too strict across independent roundings of the same
// real fraction measured at two different internal resolutions.
bool hudQualityRatioNear(uint32_t candidateX10000, uint32_t knownX10000,
                         uint32_t toleranceX10000);

// One entry in the cross-session ratio table: a candidate surface shape,
// as (width, height) ratios to the internal render resolution, the internal
// WIDTH it was last observed at (the "was this a different session's
// resolution" test), and whether two different resolutions have now agreed.
struct HudQualityRatioSlot {
    uint32_t ratioWx10000 = 0;
    uint32_t ratioHx10000 = 0;
    uint32_t lastInternalW = 0;
    bool     confirmed = false;
};

enum class HudQualityRatioVerdict : uint8_t {
    kNoSlot,           // table full and this ratio was not already in it
    kNewCandidate,     // not seen before; recorded, not yet usable
    kSameSession,      // matches an unconfirmed entry, but at the SAME
                       // internal width already on file -- no new evidence
    kConfirmed,        // matched (or already matched) at a DIFFERENT
                       // internal width: usable now
};

// Looks up (rw, rh) in slots[0..*count) (capacity `capacity`), mutating the
// table in place: a new ratio is appended if there is room; an existing
// entry's lastInternalW is refreshed and its confirmed bit is set the first
// time a second, different internalW agrees with it. Pure -- no file I/O,
// no global state -- so the caller owns persistence; fss_res.cpp loads the
// table at configure time and saves it after any call that changes it
// (kNewCandidate or the kSameSession->kConfirmed transition).
HudQualityRatioVerdict hudQualityRatioObserve(HudQualityRatioSlot* slots, uint32_t* count,
                                              uint32_t capacity, uint32_t rw, uint32_t rh,
                                              uint32_t internalW, uint32_t toleranceX10000);

}  // namespace edvr
