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
int hudQualityMatchLearned(uint32_t w, uint32_t h, const uint32_t* learnedW,
                           const uint32_t* learnedH, uint32_t n);

}  // namespace edvr
