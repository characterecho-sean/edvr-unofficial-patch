// F152 (native on-foot VR): the origin of the weapon readout, the marker and F85 when engine motion is off. Header only, no game or D3D11 state, so native_frame_test runs the very
// functions the draw tee uses.
//
// The readout, the marker and F85 read row 275 of the scene constants (b1). Engine motion's CB shadow has it only while it is live; native on foot keeps its own CPU shadow of b1's
// rows 270..275 (as it does for b0) and takes it when the engine's has none. Which of the two gave the origin is counted per window, and where both exist their rows are compared.
//
// (F152 also gated the coat's Gear_Back on the head's yaw against the body; F157 removed that gate and leaves the pack's draw out of both eyes instead.)
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f152 {

// ---- the origin of the readout, the marker and F85 -----------------------------------------------------------------------------------------------------------------------
enum OriginSource : int { kOriginNone = 0, kOriginEngine = 1, kOriginOwn = 2, kOriginCount = 3 };
inline const char* originText(int s) noexcept { return s == kOriginEngine ? "the engine shadow" : s == kOriginOwn ? "the own shadow" : "none"; }

// Rows 270..275 of the scene constants, six float4, as both shadows keep them.
constexpr uint32_t kRowsFirst = 270, kRowCount = 6, kRowsOffset = kRowsFirst * 16u, kRowsBytes = kRowCount * 16u;

// The largest absolute difference between two copies of the rows (infinite where either holds a value that is not a number).
inline double rowsMaxDiff(const float (&a)[6][4], const float (&b)[6][4]) noexcept {
    double m = 0.0;
    for (int r = 0; r < 6; ++r)
        for (int c = 0; c < 4; ++c) {
            const double d = std::fabs(static_cast<double>(a[r][c]) - static_cast<double>(b[r][c]));
            if (!std::isfinite(d)) return HUGE_VAL;
            if (d > m) m = d;
        }
    return m;
}

// The origin taken for a draw: the engine's rows first when it has them, else the own shadow's, else none. Where both exist `*compared` is set and `*diff` is their largest difference.
// engine: the rows the engine's shadow gave (haveEngine); own: the own shadow's (haveOwn). Returns the source and copies its rows to out.
inline int chooseOrigin(bool haveEngine, const float (&engine)[6][4], bool haveOwn, const float (&own)[6][4], float (&out)[6][4], bool* compared, double* diff) noexcept {
    if (compared) *compared = false;
    if (diff) *diff = 0.0;
    if (haveEngine) {
        if (haveOwn) {
            if (compared) *compared = true;
            if (diff) *diff = rowsMaxDiff(engine, own);
        }
        std::memcpy(out, engine, sizeof(out));
        return kOriginEngine;
    }
    if (haveOwn) {
        std::memcpy(out, own, sizeof(out));
        return kOriginOwn;
    }
    return kOriginNone;
}

// Writes into an own shadow's rows by a ranged UpdateSubresource: the part of [left, right) (bytes of the buffer) that lies in rows 270..275 lands in place from `data` (which
// starts at byte `left`). False when nothing of it lies in the rows (nothing changed).
inline bool overlayRange(float (&rows)[6][4], const uint8_t* data, uint32_t left, uint32_t right) noexcept {
    if (!data || right <= left) return false;
    const uint32_t from = left > kRowsOffset ? left : kRowsOffset, to = right < kRowsOffset + kRowsBytes ? right : kRowsOffset + kRowsBytes;
    if (to <= from) return false;
    std::memcpy(reinterpret_cast<uint8_t*>(rows) + (from - kRowsOffset), data + (from - left), to - from);
    return true;
}

}  // namespace f152
}  // namespace nofoot
}  // namespace edvr
