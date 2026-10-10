// The served floor of a DLSS output, and the output the door asks for when
// the game renders under it (docs/anti-aliasing.md, "The served floor: the
// output follows the input (2026-09-23)").
//
// NGX names, for each output size, every quality mode's accepted INPUT
// range. The flight of 2026-09-23 15:34 (edvr_gfx_20260923_153446.log, line
// 435) printed them for the first time:
//
//   dlss: modes for 4074x4076: quality 2716x2717 (2037x2038..4074x4076),
//   balanced 2363x2364 (2037x2038..4074x4076), performance 2037x2038
//   (2037x2038..4074x4076), ultra performance 1358x1359 (1358x1359..1358x1359)
//
// The three upper modes share one floor at half the output, and ultra
// performance's range is a single point at a third. An input between a
// third and a half of the output, or under a third, is served by NO mode,
// and the pass refused and ran its own history -- "DLSS disengaged": HMD
// Quality under 0.5, or a trim's two-step adoption (a 1229x1412 frame
// against a still-untrimmed 3070x3032 output, 40%, 09:29:10). The selection
// (dlaa.h) refuses correctly there. What can change is the OUTPUT: the
// largest one, at or under the output the door would hand, whose floor the
// input reaches -- twice the input on those ranges. The runtime's submit
// blit (src/openxr/d3d11_stereo.cpp) samples whatever the door hands into
// the headset's full swapchain with the same field of view, so the rest of
// the way is an ordinary upsample.
//
// SDK-free and device-free, like dlaa.h's selection, so the rigs drive the
// rule with the flights' own ranges; the one NGX call it needs,
// dlssModeRanges, lives in dlaa.cpp beside the query ensureFeature makes.
#pragma once

#include "dlaa.h"

#include <cstdint>

struct ID3D11Device;

namespace edvr {

// NGX's four ranges for one output size: the query ensureFeature makes, for
// a caller that must know the floor BEFORE it asks for an output (the
// door, native_temporal.cpp). Initialises NGX on dev the first time, as
// dlaaAvailable does. False when the build has no SDK or NGX is not
// available; every entry is then left !ok. Render thread only, like the
// rest of dlaa.cpp. Logs the "dlss: modes for WxH" line once per output
// size, as ensureFeature does, so the floor a decision stood on is in the
// log beside it; `quiet` skips that line (the ceiling search asks a dozen
// sizes and logs only the one it keeps).
bool dlssModeRanges(ID3D11Device* dev, uint32_t outW, uint32_t outH,
                    DlssModeRange modes[kDlssModeCount], bool quiet = false);

// Does some answered mode's range hold w x h (inclusive at both ends)? The
// same test dlssChooseMode's first rule makes; true means the selection
// will find a mode at this output.
inline bool dlssRangesServe(const DlssModeRange modes[kDlssModeCount], uint32_t w, uint32_t h) {
    for (int k = 0; k < kDlssModeCount; ++k) {
        const DlssModeRange& m = modes[k];
        if (m.ok && w >= m.minW && h >= m.minH && w <= m.maxW && h <= m.maxH) return true;
    }
    return false;
}

// A mode whose range is a range on both axes (an answered query, 0 < min <
// max). A single point -- ultra performance on the flights -- serves only
// itself, which the selection still finds as it stands; nothing else can
// stand on it, so it is no floor.
inline bool dlssRangeIsRange(const DlssModeRange& m) {
    return m.ok && m.minW && m.minH && m.minW < m.maxW && m.minH < m.maxH;
}

// A mode NGX actually answered with a usable range: the call succeeded AND
// its optimal size, its minimum and its maximum are all nonzero, and the
// range is not inverted. NGX returns success with every size zero for an
// output past its ceiling -- the 2026-10-09 Pimax flight printed "quality
// 0x0 (0x0..0x0)" for all four modes at 8268x3948 -- so `ok` alone proves
// nothing about the output.
inline bool dlssModeAnswered(const DlssModeRange& m) {
    return m.ok && m.optW && m.optH && m.minW && m.minH && m.maxW && m.maxH &&
           m.minW <= m.maxW && m.minH <= m.maxH;
}

// Does any mode answer with a usable range for this output? False is the
// output's ceiling (or NGX said nothing about it at all).
inline bool dlssRangesAnswered(const DlssModeRange modes[kDlssModeCount]) {
    for (int k = 0; k < kDlssModeCount; ++k) {
        if (dlssModeAnswered(modes[k])) return true;
    }
    return false;
}

// The lowest floor among the modes with a range, per axis, for the log; false
// when no answered mode has one.
inline bool dlssRangeFloor(const DlssModeRange modes[kDlssModeCount], uint32_t* floorW,
                           uint32_t* floorH) {
    uint32_t fw = 0, fh = 0;
    for (int k = 0; k < kDlssModeCount; ++k) {
        const DlssModeRange& m = modes[k];
        if (!dlssRangeIsRange(m)) continue;
        if (!fw || m.minW < fw) fw = m.minW;
        if (!fh || m.minH < fh) fh = m.minH;
    }
    if (floorW) *floorW = fw;
    if (floorH) *floorH = fh;
    return fw && fh;
}

// One axis of the rule: the door's own output where the input already
// reaches the floor, otherwise the output that floor -- a fixed fraction of
// its output, floor / door -- puts at the input: input x door / floor,
// rounded down, never above the door's, made even. 0 when that leaves
// nothing to ask for.
inline uint32_t dlssFloorAxis(uint32_t door, uint32_t floor, uint32_t input) {
    if (!door || !floor || !input) return 0;
    if (input >= floor) return door;
    uint64_t out = uint64_t(input) * door / floor;
    if (out > door) out = door;
    out &= ~uint64_t(1);
    return out >= 2 ? static_cast<uint32_t>(out) : 0;
}

// The rule. doorW x doorH is the output the door would hand (the host's
// recommendation), modes that output's four ranges, w x h the input. When
// some mode serves the input there, the door's output stands (true, the
// output unchanged). Otherwise, for each mode with a range, the output its
// floor lets the input reach (dlssFloorAxis on each axis); the largest wins
// and *mode names it. False when nothing is known to stand on: no answered
// range at all (NGX would not say -- the selection's own ratio fallback
// then decides at the door's output, as before).
inline bool dlssFloorOutput(const DlssModeRange modes[kDlssModeCount], uint32_t doorW,
                            uint32_t doorH, uint32_t w, uint32_t h, uint32_t* outW,
                            uint32_t* outH, int* mode = nullptr) {
    if (outW) *outW = doorW;
    if (outH) *outH = doorH;
    if (mode) *mode = -1;
    if (!doorW || !doorH || !w || !h) return false;
    if (dlssRangesServe(modes, w, h)) return true;
    uint64_t bestArea = 0;
    uint32_t bestW = 0, bestH = 0;
    int best = -1;
    for (int k = 0; k < kDlssModeCount; ++k) {
        const DlssModeRange& m = modes[k];
        if (!dlssRangeIsRange(m)) continue;
        const uint32_t ow = dlssFloorAxis(doorW, m.minW, w), oh = dlssFloorAxis(doorH, m.minH, h);
        if (!ow || !oh) continue;
        const uint64_t area = uint64_t(ow) * oh;
        if (area > bestArea) { bestArea = area; bestW = ow; bestH = oh; best = k; }
    }
    if (best < 0) return false;
    if (outW) *outW = bestW;
    if (outH) *outH = bestH;
    if (mode) *mode = best;
    return true;
}

// NVIDIA's ceiling, found by search. When an output has no usable range
// (dlssRangesAnswered false) the output is cut to the largest one that does:
// the largest at or under doorW x doorH, aspect kept, both sides even, that
// query answers. query(w, h, modes) fills the four ranges for w x h -- the
// NGX call in dlaa.cpp, or a table in a rig. Bisects on the width in pairs of
// pixels: NVIDIA's refusal is a size limit, so the answer is monotone in
// practice. The search returns only a size it has seen answered, so a query
// that is not monotone costs precision, never truth. False when no size at or
// under the door is answered.
template <typename Query>
inline bool dlssCeilingOutput(uint32_t doorW, uint32_t doorH, Query&& query, uint32_t* outW,
                              uint32_t* outH) {
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    if (!doorW || !doorH) return false;
    // k answers the width 2k: lo is the largest k verified answered (0: none
    // yet), hi the largest k that could still be the answer.
    uint32_t lo = 0, hi = doorW / 2, loH = 0;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo + 1) / 2;
        const uint32_t w = 2 * mid;
        const uint64_t hh = (uint64_t(doorH) * w / doorW) & ~uint64_t(1);
        bool answered = false;
        if (hh >= 2) {
            DlssModeRange modes[kDlssModeCount];
            query(w, static_cast<uint32_t>(hh), modes);
            answered = dlssRangesAnswered(modes);
        }
        if (answered) {
            lo = mid;
            loH = static_cast<uint32_t>(hh);
        } else {
            hi = mid - 1;
        }
    }
    if (!lo) return false;
    if (outW) *outW = 2 * lo;
    if (outH) *outH = loH;
    return true;
}

}  // namespace edvr
