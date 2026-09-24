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
// value and the eye composes against the head pose alone -- the flash.
//
// The candidate written back is the mailbox's OWN last known-refilled value
// (flight 062910: the never-reset ship+0x130 block agreed with a refilled
// mailbox 0 times in 11,084 calls, so it is not a fit stand-in -- see "Ruled
// out (062910)" in the design doc). transition_flash_eye_base.cpp caches M
// every time it sees a refilled mode!=1 call; heldBaseRefusal below is the
// guard an un-refilled call's cached candidate must clear before anything
// writes it back.
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
// CHANGE 1 (task of 2026-09-24, "the substitute becomes the held base"):
// ship+0x130 (F) is no longer the candidate written into the mailbox -- flight
// 062910 measured it agreeing with a refilled mailbox 0 times in 11,084 calls
// (see "Ruled out (062910)" in the design doc). The candidate is now the
// mailbox's OWN last known-refilled value, cached by transition_flash_eye_
// base.cpp on every mode!=1 call whose mailbox was not the reset value. Every
// guard below is ANDed together in the priority this enum's order states, so
// a test can flip each one alone (the rest held at the acting baseline) and
// see it -- and only it -- refuse. F-vs-M agreement (EyeBaseValidation above)
// no longer participates: it stays only for transition_flash_eye_base.cpp's
// periodic report line, which costs nothing extra to keep computing.
enum class HeldBaseRefusal {
    None,            // every guard passed; the caller may act
    WatchSlot,       // this event's latched treatment is Watch, not Act
    Stale,           // no cache yet, or cached more than kHeldBaseMaxAgeFrames earlier
    PointerChanged,  // the cached ship pointer differs from this call's ship
    NotFinite,       // a cached lane is not finite
    Reset,           // the cached M is itself the reset value
    Cap,             // the session act cap is reached
};

inline const char* heldBaseRefusalText(HeldBaseRefusal r) noexcept {
    switch (r) {
    case HeldBaseRefusal::None:           return "none";
    case HeldBaseRefusal::WatchSlot:      return "watch slot";
    case HeldBaseRefusal::Stale:          return "stale";
    case HeldBaseRefusal::PointerChanged: return "pointer changed";
    case HeldBaseRefusal::NotFinite:      return "not finite";
    case HeldBaseRefusal::Reset:          return "reset";
    case HeldBaseRefusal::Cap:            return "cap";
    }
    return "?";
}

// Cached no more than this many frames earlier still counts as fresh (task:
// "current frame minus cached frame <= 2").
inline constexpr uint32_t kHeldBaseMaxAgeFrames = 2;

inline HeldBaseRefusal heldBaseRefusal(tfp::Treatment treatment, bool haveHeldBase, uint32_t currentFrame,
                                       uint32_t heldFrame, uint64_t currentShip, uint64_t heldShip,
                                       const float heldM[16], bool sessionCapReached) noexcept {
    if (treatment != tfp::Treatment::Act) return HeldBaseRefusal::WatchSlot;
    if (!haveHeldBase) return HeldBaseRefusal::Stale;
    const uint32_t age = currentFrame >= heldFrame ? currentFrame - heldFrame : 0xFFFFFFFFu;
    if (age > kHeldBaseMaxAgeFrames) return HeldBaseRefusal::Stale;
    if (currentShip != heldShip) return HeldBaseRefusal::PointerChanged;
    if (!allFinite16(heldM)) return HeldBaseRefusal::NotFinite;
    if (isResetMailbox(heldM)) return HeldBaseRefusal::Reset;
    if (sessionCapReached) return HeldBaseRefusal::Cap;
    return HeldBaseRefusal::None;
}

inline bool heldBaseMayAct(HeldBaseRefusal r) noexcept { return r == HeldBaseRefusal::None; }

// ---------------------------------------------------------------------------
// CHANGE 2 ("the writer watch gate is self-contained"): the consumer's own
// count of consecutive REFILLED mode!=1 calls -- with the existing 60-
// consecutive-frame ship-pointer stability gate (pose_reader_watch_core.h's
// StabilityState/isStable, unchanged), the second half of "flight, with the
// writer active" that replaces glitchFrameCameraValidated for this module
// only (pose_reader_watch.cpp keeps its own dependency on it). A mode==1
// call is excluded from the sequence -- neither increments nor resets it,
// since the driver never resets the mailbox on mode==1 either, so mode==1
// calls carry no information about the writer either way. An un-refilled
// mode!=1 call breaks the streak.
inline constexpr uint32_t kWriterWatchGateConsecutiveRefilled = 300;

inline uint32_t updateConsecutiveRefilled(uint32_t counter, int32_t gameMode, bool unrefilled) noexcept {
    if (gameMode == 1) return counter;
    if (unrefilled) return 0u;
    return counter < 0xFFFFFFFFu ? counter + 1 : counter;
}

inline bool writerWatchGateSatisfied(bool shipPointerStable, uint32_t consecutiveRefilled) noexcept {
    return shipPointerStable && consecutiveRefilled >= kWriterWatchGateConsecutiveRefilled;
}

// ---------------------------------------------------------------------------
// CHANGE 3 ("dumps work on their own"): which stimuli are this module's own
// dump triggers. Deliberately takes no eye-trace flag at all -- glitch_
// frame.cpp's eyeOriginTraceBoundary gates ITS OWN dump on advanced.eye_
// origin_trace (s->eyeOriginTraceOn); this module's dump must not, so there
// is no such parameter here for it to gate on. The two triggers are an
// un-refilled consumer call and the detector's scene-judged eye-reset
// verdict, ORed.
inline bool eyeBaseDumpTrigger(bool unrefilledCall, bool sceneResetVerdict) noexcept {
    return unrefilledCall || sceneResetVerdict;
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
