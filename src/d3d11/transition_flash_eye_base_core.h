#pragma once
// Pure logic for advanced.transition_flash_eye_base (docs/design-transition-
// flash-engine-fix-2026-09-23.md, "Static round 6: the eye's base is a
// consume-and-reset mailbox"). No Windows header, no game memory, no
// CodeHook, no hardware breakpoint -- takes plain data in and hands plain
// data back, so tools\transition_flash_prevent_test can drive it without the
// game. transition_flash_eye_base.cpp is the only includer that also touches
// the process.
//
// Round 6's mechanism: the camera driver FUN_1428431d0 copies a 4x4 "mailbox"
// at ship+0x3330..+0x336F into a local before composing the eye views, and
// (mode != 1) resets the mailbox to the identity constant below right after
// the copy. Some writer has to refill it every frame; on the frame after a
// render-frame switch it is not refilled in time, so the copy IS the reset
// value and the eye composes against the head pose alone -- the flash. A
// second, never-reset block at ship+0x130 (mode 3) is the candidate stand-in
// this file's math validates before anything acts on it.
//
// off | watch | on | alternate is the same four-way shape as advanced.
// transition_flash_prevent, so this reuses tfp::Mode/parseMode/Treatment/
// alternateTreatmentFor/modeAllowsActing/EventTracker/sessionCapReached/
// isNewActedFrame/ringFrameInWindow/foldDumpTrigger/frameInDumpWindow
// directly rather than re-deriving them -- see this header's own tests for
// what is NOT shared (the reset-mailbox bit compare and the M/F validation
// arithmetic both use different thresholds and a different index set than
// transition_flash_prevent_core.h's comparePose/classify/isValidated, which
// compare a cached DOUBLE pose against a from-root recompute; the mailbox and
// its stand-in are FLOATS read straight out of game memory).
#include "transition_flash_prevent_core.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace edvr {
namespace tfeb {

// ---------------------------------------------------------------------------
// The reset value ship+0x3330..+0x336F is set to on every (mode != 1) call:
// rows 0-2 from the identity constant at VA 0x1450C8090 (verified against
// analysis\EliteDangerous64.exe's own bytes -- canonical 0x3F800000/
// 0x00000000, not some other bit pattern that merely prints as 1/0), row 3
// (the translation) zeroed by the two qword stores at +0x3360/+0x3368.
inline constexpr float kResetMailbox[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
};

// Bit-exact, not value-exact: a writer that stores a numerically-equal-but-
// differently-bitted value (-0.0 where the constant holds +0.0, say) has
// still WRITTEN, and value equality (-0.0f == +0.0f under IEEE ==) would hide
// that. memcmp on the raw bytes also does the right thing for NaN -- any NaN
// bit pattern differs from the constant's finite bits, so it never reads as
// "still the reset value" either way, but bit compare is the one asked for
// and the one this file's test cell holds it to.
inline bool bitEquals16(const float a[16], const float b[16]) noexcept {
    return std::memcmp(a, b, sizeof(float) * 16) == 0;
}

inline bool isResetMailbox(const float m[16]) noexcept {
    return bitEquals16(m, kResetMailbox);
}

// ---------------------------------------------------------------------------
// M-vs-F comparison. Translation at [12..14]; rotation at [r*4+c] for r,c in
// 0..2 -- the same 9 lanes transition_flash_prevent_core.h's comparePose
// walks, just named as an explicit index set here because the design doc
// states the thresholds against exactly that list. Lanes [3],[7],[11],[15]
// (padding/w) are never compared.
struct EyeBaseDelta {
    double dt = 0.0;
    double dr = 0.0;
};

inline constexpr int kRotationLanes[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};

inline EyeBaseDelta compareEyeBase(const float m[16], const float f[16]) noexcept {
    EyeBaseDelta d;
    double sq = 0.0;
    for (int a = 12; a <= 14; ++a) {
        const double e = double(m[a]) - double(f[a]);
        sq += e * e;
    }
    d.dt = std::sqrt(sq);
    double maxAbs = 0.0;
    for (int lane : kRotationLanes) {
        double e = double(m[lane]) - double(f[lane]);
        if (e < 0) e = -e;
        if (e > maxAbs) maxAbs = e;
    }
    d.dr = maxAbs;
    return d;
}

// agree: dt < 0.01 and dr < 1e-4. Anything else -- there is no "near" tier
// here, unlike transition_flash_prevent_core.h's PoseClass -- disagrees.
inline bool eyeBaseAgrees(const EyeBaseDelta& d) noexcept {
    return d.dt < 0.01 && d.dr < 1e-4;
}

// ---------------------------------------------------------------------------
// Validation: at least 120 agreements this session, and disagreements under
// 2% of (agree+disagree) so far -- tighter on both counts than transition_
// flash_prevent_core.h's isValidated (60 agreements, under 5%), because F is
// read off a live object every call rather than recomputed from root, so a
// false "validated" here would write real game memory, not just skip a
// compose.
struct EyeBaseValidation {
    uint64_t agree = 0;
    uint64_t disagree = 0;
};

inline bool isEyeBaseValidated(const EyeBaseValidation& c) noexcept {
    if (c.agree < 120) return false;
    const uint64_t total = c.agree + c.disagree;
    if (total == 0) return false;
    // c.disagree < 0.02 * total without floating point on the count path:
    // 50 * disagree < total.
    return 50ull * c.disagree < total;
}

// ---------------------------------------------------------------------------
// Every lane finite: guards against handing the game an Inf/NaN base, which
// a reset-value check alone would not catch (Inf is not the identity, but it
// is not a fit stand-in either).
inline bool allFinite16(const float v[16]) noexcept {
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(v[i])) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The act decision: every guard the design lists, ANDed together, so a test
// can flip each one alone and see it -- and only it -- refuse. `treatment`
// is the event's latched tfp::Treatment (Watch/Act, from tfp::EventTracker
// via tfp::modeAllowsActing); `fIsReset` and `fFinite` describe the
// candidate stand-in F, not the mailbox M.
inline bool eyeBaseMayAct(tfp::Treatment treatment, bool validated, bool fIsReset, bool fFinite,
                          bool sessionCapReached) noexcept {
    return treatment == tfp::Treatment::Act && validated && !fIsReset && fFinite && !sessionCapReached;
}

// ---------------------------------------------------------------------------
// The consumer's own extent, for the writer-watch's hit classifier: a hit
// whose RIP lands inside FUN_1428431d0's own body is the function's own
// reset write -- the (mode != 1) branch's `*(ship+0x3360) = 0` qword store,
// the same branch that resets ship+0x3330..+0x335F -- not an outside writer.
// [0x28431D0, 0x28431D0+0x61F) is the DECOMPILER's function extent (1567
// bytes), not the .pdata RUNTIME_FUNCTION range -- follow_28431D0.txt's own
// dump shows pdata reporting only 0x4F bytes for this entry because it is a
// chained-unwind function, and using that short range would misclassify the
// tail of the function's own body as an outside writer.
inline constexpr uintptr_t kConsumerExtentRva = 0x28431D0u;
inline constexpr uintptr_t kConsumerExtentSize = 0x61Fu;

inline bool rvaInsideConsumerExtent(uintptr_t rva) noexcept {
    return rva >= kConsumerExtentRva && rva < kConsumerExtentRva + kConsumerExtentSize;
}

}  // namespace tfeb
}  // namespace edvr
