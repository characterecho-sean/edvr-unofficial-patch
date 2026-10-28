// F152 (native on-foot VR): the pure parts of two rules. Header only, no game or D3D11 state, so native_frame_test runs the very functions the draw tee and the visibility hook use.
//
// 1. The coat's back with the head turned. The coat outfit's EVASuit_Gear_Back (the coat's back and its pack) is promoted to every view for the visibility call (F142, F144) so the
//    coat is drawn in first person. With the head turned well away from the body the pack and its antenna sit in the left eye's periphery (flight edvr_gfx_20261028_072553, the
//    head 55 to 62 deg from the body at the hip, 07:28:56 to 07:29:13; the F147 flight shows it at -60 to +93 deg; the flights that never turned past 43 deg never showed it). The
//    head's yaw against the body is phi = atan2(right, forward) of the body pin's eye-minus-neck vector (F67c) in the body's axes, in degrees. Promotion stops at |phi| >= 45 deg and
//    resumes below 38 deg; past the threshold the game's shadows-only class stands for that part. The pin publishes one sample per call (the float and the millisecond tick in one
//    64-bit word), the visibility hook reads it; a sample older than 100 ms keeps the last decision.
//
// 2. The origin of the weapon readout, the marker and F85 when engine motion is off. They read row 275 of the scene constants (b1). Engine motion's CB shadow has it only while it is
//    live; native on foot keeps its own CPU shadow of b1's rows 270..275 (as it does for b0) and takes it when the engine's has none. Which of the two gave the origin is counted per
//    window, and where both exist their rows are compared.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f152 {

// ---- 1. the coat's back with the head turned -------------------------------------------------------------------------------------------------------------------------------
constexpr double kStopDeg = 45.0;      // |phi| at or past this stops the promotion
constexpr double kResumeDeg = 38.0;    // |phi| below this resumes it
constexpr uint32_t kStaleMs = 100;     // a sample older than this keeps the last decision
constexpr double kMinHorizontalM = 0.03;   // the eye-neck vector's horizontal part under this points nowhere: no sample

// phi (degrees, the head's yaw against the body: positive to the body's right) from the eye-minus-neck vector's right and forward parts (metres, the body's axes). False (phi
// untouched) when a part is not finite or the horizontal part is too short to give a direction.
inline bool headYawDeg(double right, double forward, double* phiDeg) noexcept {
    if (!std::isfinite(right) || !std::isfinite(forward)) return false;
    if (std::sqrt(right * right + forward * forward) < kMinHorizontalM) return false;
    if (phiDeg) *phiDeg = std::atan2(right, forward) * 180.0 / 3.14159265358979323846;
    return true;
}

// The decision with its hysteresis: promoting stops at |phi| >= kStopDeg and resumes below kResumeDeg. A sample that is not a number keeps the decision.
inline bool turnPromote(bool previous, double phiDeg) noexcept {
    const double a = std::fabs(phiDeg);
    if (!std::isfinite(a)) return previous;
    return previous ? a < kStopDeg : a < kResumeDeg;
}

// One sample in one word, so a reader never pairs a yaw with another sample's tick: the float's bits in the low half, the tick (milliseconds, the low 32 bits of the clock) in
// the high half. 0 is "none yet" (a real sample is a zero yaw at tick 0 only).
inline uint64_t packSample(float phiDeg, uint32_t tickMs) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &phiDeg, 4);
    return (static_cast<uint64_t>(tickMs) << 32) | bits;
}
inline void unpackSample(uint64_t word, float* phiDeg, uint32_t* tickMs) noexcept {
    const uint32_t bits = static_cast<uint32_t>(word & 0xFFFFFFFFull);
    if (phiDeg) std::memcpy(phiDeg, &bits, 4);
    if (tickMs) *tickMs = static_cast<uint32_t>(word >> 32);
}
// Fresh: taken within kStaleMs of now (wrap-safe on the 32-bit tick).
inline bool sampleFresh(uint32_t sampleTick, uint32_t nowTick) noexcept { return static_cast<uint32_t>(nowTick - sampleTick) <= kStaleMs; }

struct TurnDecision {
    bool promote = true;    // the decision now (true before any sample)
    bool fresh = false;     // the sample was fresh and decided it; false: the last decision was kept
    float phiDeg = 0.0f;    // the sample read (0 with none)
};
// The decision for the visibility call at nowTick: from the published word when it is a fresh sample, else `previous` kept.
inline TurnDecision turnDecide(bool previous, uint64_t word, uint32_t nowTick) noexcept {
    TurnDecision d;
    d.promote = previous;
    if (word == 0) return d;
    uint32_t tick = 0;
    unpackSample(word, &d.phiDeg, &tick);
    if (!sampleFresh(tick, nowTick) || !std::isfinite(d.phiDeg)) return d;
    d.fresh = true;
    d.promote = turnPromote(previous, d.phiDeg);
    return d;
}

// ---- 2. the origin of the readout, the marker and F85 --------------------------------------------------------------------------------------------------------------------
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
