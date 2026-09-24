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
// Round 6's fix idea was a consume-time write: cache the mailbox's OWN last
// known-refilled value (flight 062910: the never-reset ship+0x130 block
// agreed with a refilled mailbox 0 times in 11,084 calls, so it is not a fit
// stand-in -- see "Ruled out (062910)" in the design doc) and write it back
// into the mailbox on an un-refilled call. That idea is SUPERSEDED as of
// CHANGE 15 (2026-09-24): flight 134813's skip 22726 proved a consume-time
// write cannot serve scene-new transitions (the base the scene wants is not
// in any consume-indexed record), and flight 125237 proved the render-time
// patch with the live mailbox. What remains from round 6 is the WATCH
// machinery (this header's reset-value compare, the writer watch) and the
// held base as one of the render patch's candidate bases.
//
// off | watch | on | alternate is the same four-way shape as advanced.
// transition_flash_prevent, so this reuses tfp::Mode/parseMode/Treatment/
// alternateTreatmentFor/modeAllowsActing/EventTracker/ringFrameInWindow/
// foldDumpTrigger/frameInDumpWindow directly rather than re-deriving them --
// see this header's own tests for what is NOT shared (the reset-mailbox bit
// compare and the M/F validation arithmetic both use different thresholds
// and a different index set than transition_flash_prevent_core.h's
// comparePose/classify/isValidated, which compare a cached DOUBLE pose
// against a from-root recompute; the mailbox and its stand-in are FLOATS
// read straight out of game memory).
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
// CHANGE 1 (task of 2026-09-24, "the substitute becomes the held base"), as
// SUPERSEDED by CHANGE 15: the held base survives as the render-time patch's
// scene-old/unclear base (choosePatchBase below), but the guard chain that
// gated writing it into the mailbox at consume time (HeldBaseRefusal and
// friends) is gone with that write. What remains of CHANGE 1: the held-base
// cache itself, in transition_flash_eye_base.cpp, and the M/F agreement
// counter (EyeBaseValidation above), which stays only for the periodic
// report line.

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

// ---------------------------------------------------------------------------
// CHANGE 5 (2026-09-24, static round 7: "the writer's own entry"). The
// writer FUN_142874b20 (true entry, 239 bytes, chained pdata -- design doc
// "Static round 7") is never CodeHooked: its first two instructions (TEST
// RDX,RDX; JZ rel32) are a pattern CodeHook's decoder refuses. Instead
// transition_flash_eye_base.cpp arms a DR1 EXECUTE breakpoint at its entry,
// alongside DR0's existing write watch on the mailbox -- both slots share
// one VEH, one all-thread arm/disarm sweep, one lifecycle.
inline constexpr uintptr_t kWriterExtentRva = 0x2874B20u;
inline constexpr uintptr_t kWriterExtentSize = 0xEFu;  // 239 bytes: [0x2874B20, 0x2874C0F)

// A DR0 (write) hit whose RIP lands in here is the writer's own body
// WRITING the mailbox -- its _stricmp name gate let it through. Distinct
// from rvaInsideConsumerExtent above (the consumer's own reset write) and
// from every other rva, which is some other, not-yet-identified writer.
inline bool rvaInsideWriterExtent(uintptr_t rva) noexcept {
    return rva >= kWriterExtentRva && rva < kWriterExtentRva + kWriterExtentSize;
}

// Dr7 slot 1, this module's own copy of the bit layout pose_reader_watch_
// core.h's armSlot0Dr7/disarmSlot0Dr7 state for slot 0 (that header is left
// untouched -- every Dr7 user here keeps its own copy; see this file's own
// top-of-file comment and the relay machinery's for why). Bit 2 = L1 (local
// enable, slot 1); bits 20-21 = RW1; bits 22-23 = LEN1. RW1=00/LEN1=00 is an
// EXECUTE breakpoint -- x86 defines no read-only condition and no length for
// one; the CPU traps on fetch of the single byte at Dr1 regardless of
// LEN1's value, and 00/00 is the bit pattern the design doc states.
constexpr uint32_t kDr7L1Bit = 1u << 2;
constexpr uint32_t kDr7Slot1Mask = 0x00F00004u;  // bit2 | bits20-21 | bits22-23

inline uint32_t armSlot1ExecuteDr7(uint32_t existing) noexcept {
    return (existing & ~kDr7Slot1Mask) | kDr7L1Bit;
}
inline uint32_t disarmSlot1Dr7(uint32_t existing) noexcept {
    return existing & ~kDr7L1Bit;
}

// Dr6 bit 1 (B1): slot 1's condition was detected. Sticky until cleared by
// the handler, same convention as pose_reader_watch_core.h's kDr6B0Bit/
// dr6HasSlot0Hit for slot 0.
constexpr uint32_t kDr6B1Bit = 1u << 1;
inline bool dr6HasSlot1Hit(uint32_t dr6) noexcept { return (dr6 & kDr6B1Bit) != 0; }

// ---------------------------------------------------------------------------
// CHANGE 6 (2026-09-24, static round 7's substitute policy): the offered
// matrix -- what the writer was about to copy into the mailbox (param_3, the
// DR1 handler's own capture), whether or not its name gate let it through.
// When the gate refused it, this IS "the base the writer would have
// written" (design doc, "What a flight can settle") and is a strictly
// better substitute than the held base for an un-refilled consume, because
// it is the frame's own value rather than up to two frames old.
//
// ("Fresh" once gated the offered matrix's use as a consume-time
// substitute; CHANGE 15 removed that use with the write it gated.)
// (kOfferedMaxAgeFrames / offeredIsFresh / offeredSubstituteUsable lived
// here until CHANGE 15, 2026-09-24, superseded them with the consume-time
// write they gated: the offered matrix survives as instrumentation -- the
// DR1 capture, the per-frame dump row, the event log line all still show it
// -- but the render-time patch chooses its base with choosePatchBase below,
// never from the offered matrix.)


// (The substitute policy's five-condition gate, offeredSubstituteUsable,
// lived here until CHANGE 15 removed it with the consume-time write.)

// ---------------------------------------------------------------------------
// CHANGE 7 (2026-09-24, static round 7's per-frame classification): which of
// four states this frame's writer-watch counts describe -- the controller
// counter (RVA 0x10730A0) and the writer's own DR1/DR0 counts together tell
// apart the design doc's ranked skip candidates: (1) the controller's own
// validity gate refusing for the whole frame; (2) the writer's name gate
// refusing after being entered; anything else is the writer actually
// writing.
enum class FrameWriterClass : uint8_t {
    ControllerNotCalled,               // the controller tick did not run at all this frame
    ControllerCalledWriterNotEntered,  // ran, but skip candidate (1): its own vtable[0x80] gate
    WriterEnteredNotWritten,           // entered, but skip candidate (2): the name gate refused
    WriterWrote,                       // entered and wrote -- an ordinary, fully-refilled frame
};

inline FrameWriterClass classifyFrameWriter(uint32_t controllerCalls, uint32_t writerEntered,
                                             uint32_t writerWrote) noexcept {
    if (controllerCalls == 0) return FrameWriterClass::ControllerNotCalled;
    if (writerEntered == 0) return FrameWriterClass::ControllerCalledWriterNotEntered;
    if (writerWrote == 0) return FrameWriterClass::WriterEnteredNotWritten;
    return FrameWriterClass::WriterWrote;
}

inline const char* frameWriterClassText(FrameWriterClass c) noexcept {
    switch (c) {
    case FrameWriterClass::ControllerNotCalled:              return "controller not called";
    case FrameWriterClass::ControllerCalledWriterNotEntered: return "controller called, writer not entered";
    case FrameWriterClass::WriterEnteredNotWritten:          return "writer entered, did not write";
    case FrameWriterClass::WriterWrote:                      return "writer wrote";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// CHANGE 9 (2026-09-24, task "dump triggers become mode-switch edges"): a low
// wake's controller-idle stretch is thousands of un-refilled consumes long
// (flight 091726, design doc: 5,041 of them, one dump trigger each, 2.3 MB
// for a single low wake) -- only the EDGES are informative. ENTRY is the
// first un-refilled mode!=1 consume after a refilled one (a single-frame
// skip is its own entry, with no exit to follow); EXIT is the first refilled
// mode!=1 consume after a run of at least kModeSwitchExitRun consecutive
// un-refilled ones. The detector's scene-judged CameraReset verdict
// (tfeb::eyeBaseDumpTrigger) is a separate, untouched trigger.
//
// updateConsecutiveUnrefilled is the mirror of updateConsecutiveRefilled
// above: a mode==1 call carries no information about the mailbox either way
// and leaves the streak untouched, exactly as that function already treats
// it. classifyModeSwitchEdge takes the streak's value from BEFORE this call
// folds in (updateConsecutiveUnrefilled's own `counter` argument, not its
// return), so entry/exit are each named on the one call that crosses the
// edge, not on every call inside the run.
inline constexpr uint32_t kModeSwitchExitRun = 30;

enum class ModeSwitchEdge : uint8_t { None, Entry, Exit };

inline uint32_t updateConsecutiveUnrefilled(uint32_t counter, int32_t gameMode, bool unrefilled) noexcept {
    if (gameMode == 1) return counter;
    if (!unrefilled) return 0u;
    return counter < 0xFFFFFFFFu ? counter + 1 : counter;
}

inline ModeSwitchEdge classifyModeSwitchEdge(int32_t gameMode, bool unrefilled,
                                              uint32_t consecutiveUnrefilledBeforeThisCall) noexcept {
    if (gameMode == 1) return ModeSwitchEdge::None;
    if (unrefilled) {
        return consecutiveUnrefilledBeforeThisCall == 0 ? ModeSwitchEdge::Entry : ModeSwitchEdge::None;
    }
    return consecutiveUnrefilledBeforeThisCall >= kModeSwitchExitRun ? ModeSwitchEdge::Exit : ModeSwitchEdge::None;
}

// ---------------------------------------------------------------------------
// CHANGE 10 (2026-09-24, the endgame's watch-only validation, task "render-
// time patch simulation"): the pure logic the simulation runs at render time.
// Flight 100043 (design doc, "Flight 100043") settled the design the acting
// build will implement: the bad render at R = N+2 (N = the un-refilled
// consume's frame) shows the head pose alone because the eye composed
// against the identity base; premultiplying that bad eye P by a chosen base
// B -- patched = R(B).P + t(B) -- restores it. Which B is right depends on
// whether the scene has already switched frames at R, and the detector's own
// per-frame geometry decides that (patchSceneChoice below). Everything the
// simulation does is passive: it logs what the acting build would write.

// The eye composer's own multiply, verified two independent ways:
//  - Data (flight 100043, f13550's ordinary frame): the mailbox translation
//    (-0.660, +11.066, -7.725) plus the head offset through the base's 3x3
//    lands at the rendered scene eye (-0.66, +11.08, -7.64) -- centimetres
//    apart, the size of head-pose quantisation, not of a frame switch (which
//    measures in metres to kilometres).
//  - Static (analysis\decomp\flash\r6\decomp_283D4C0.txt, FUN_14283d4c0,
//    called twice from the consumer FUN_1428431d0's own body): the decompile
//    composes the eye as out[k] = hx*B[k] + hy*B[4+k] + hz*B[8+k] + B[12+k]
//    -- a row-vector head position through the base B = param_2, translation
//    at [12..14]. Exactly this function's shape; it confirms the formula
//    rather than complicating it.
inline void patchEyeOrigin(const float B[16], const float P[3], float out[3]) noexcept {
    for (int k = 0; k < 3; ++k) {
        out[k] = P[0] * B[k] + P[1] * B[4 + k] + P[2] * B[8 + k] + B[12 + k];
    }
}

// The full 4x4 premultiply the ACTING build will need when it patches the
// eye's matrix wholesale rather than only its origin -- same row convention
// (rotation at [r*4+c], translation row at [12..14], last row (0,0,0,1)).
// The simulation patches origins only; this sits beside patchEyeOrigin so
// the acting build's arithmetic is gated by the same flight-proven test
// cell. Writes through a local so out may alias B or M.
inline void premul4x4(const float B[16], const float M[16], float out[16]) noexcept {
    float r[16];
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += B[row * 4 + k] * M[k * 4 + col];
            r[row * 4 + col] = sum;
        }
    }
    std::memcpy(out, r, sizeof(r));
}

// Which frame the scene is in at render time R -- flight 100043's measured
// selector (right on 8 of 8 resets there, plus the 2026-09-12 hyperspace
// data): this frame's object-pool step against the camera's own step.
// Scene-new (low-wake drop-in: the pool rebased WITH the camera, pool ~=
// cam) -- the NEW base is right. Scene-old (hyperspace exit: the pool still
// in the old frame, pool << cam) -- the HELD base is right. The caller
// defaults to held on Unclear: held is the proven-safe side (073114's high
// wakes), and the simulation's log line prints both candidates regardless.
enum class SceneChoice : uint8_t { Old, New, Unclear };

inline const char* sceneChoiceText(SceneChoice c) noexcept {
    switch (c) {
    case SceneChoice::Old:     return "scene-old";
    case SceneChoice::New:     return "scene-new";
    case SceneChoice::Unclear: return "unclear";
    }
    return "?";
}

// The measured bands: flight 100043's scene-new resets all sat at ratio
// 0.99-1.09 (2927.2/2925.8 at f13549, 14.7/13.5 at f13939, 3567.3/3594.2 at
// f22217, 3861.7/3860.4 at f23340); the 2026-09-12 hyperspace exits sat at
// 0.0/1600 = 0. [0.5, 2.0] new and < 0.25 old leave a dead band (0.25 <=
// ratio < 0.5) no measured event has entered -- events land there read
// Unclear, not guessed. `geometryFresh` false (the pool was not sampled this
// frame) is Unclear regardless of the numbers: a zeroed default geometry
// would otherwise read as a scene-old 0.
inline constexpr float kSceneChoiceCamFloor = 1e-6f;
inline constexpr float kSceneNewRatioMin = 0.5f;
inline constexpr float kSceneNewRatioMax = 2.0f;
inline constexpr float kSceneOldRatioMax = 0.25f;

inline SceneChoice patchSceneChoice(float camStep, float poolStep, bool geometryFresh) noexcept {
    if (!geometryFresh) return SceneChoice::Unclear;
    const float denom = camStep > kSceneChoiceCamFloor ? camStep : kSceneChoiceCamFloor;
    const float ratio = poolStep / denom;
    if (ratio >= kSceneNewRatioMin && ratio <= kSceneNewRatioMax) return SceneChoice::New;
    if (ratio < kSceneOldRatioMax) return SceneChoice::Old;
    return SceneChoice::Unclear;
}

// A pending simulation armed at the entry-edge consume frame N covers the
// render frames N+1..N+3: the bad render is at N+2, and one frame of slack
// either side still gets logged when a consume/render skew shifts R.
inline constexpr uint32_t kPatchSimWindowFrames = 3;

inline bool patchSimWindowCovers(uint32_t frame, uint32_t skipFrame) noexcept {
    return frame > skipFrame && frame - skipFrame <= kPatchSimWindowFrames;
}

// ---------------------------------------------------------------------------
// CHANGE 13 (2026-09-24, flight N+1's finding): the sim's tap-time probe of
// the LIVE mailbox. The flight showed the consume-indexed NEW-base candidate
// is structurally stale at the bad render -- the refill consume N+1 runs
// AFTER the bad render's tap (consume frames lag render taps by ~1 wall-
// clock frame), so g_lastRefilledM can never hold the needed base there.
// But that same refill leaves the value SITTING LIVE in ship+0x3330 during
// the tap's wall-clock window (written by the writer from the same scene
// graph the render draws), so a read-only SEH probe of the mailbox AT the
// tap sees exactly the base the bad render needs. This is the sanity gate a
// non-reset live value must clear: every lane finite and the translation
// under 1e7 units (~10,000 km -- the measured rebases are 5 km, the f15513
// tunnel base 1.6 km, so this rejects garbage without ever rejecting a
// real base). The RESET value itself is screened out by the caller with
// isResetMailbox above before this gate is even asked.
inline bool mailboxPlausible(const float m[16]) noexcept {
    if (!allFinite16(m)) return false;
    const float t2 = m[12] * m[12] + m[13] * m[13] + m[14] * m[14];
    return t2 < 1e7f * 1e7f;
}

// ---------------------------------------------------------------------------
// CHANGE 14 (2026-09-24, flight 125237's follow-on): the buffer-row locator's
// pure logic. Flight 125237 proved the live mailbox is the right base
// source; the ACTING patch must apply premul4x4(liveM, badEye) to the eye-
// derived ROWS of the 5376-byte scene CB (cb1 row 275 = the eye origin),
// and the bad frame's eye is wrong in ROTATION too (the base's 3x3 is
// ~1.3-1.7 rad off identity), so the current-view matrix's rows must be
// corrected with it: view' = V_bad x liveM^-1 (corrected eye = liveM x P,
// so view = (liveM x P)^-1 = P^-1 x liveM^-1 = V_bad x liveM^-1). These
// functions locate the view rows structurally and compute that correction;
// the module logs what they WOULD write, passively.

// The scene CB's own constants (glitch_frame.cpp's camera_buffer_bytes/
// camera_buffer_offset defaults -- the detector's own tap already gates on
// them before this module is ever called).
inline constexpr uint32_t kSceneCBSimBytes = 5376;
inline constexpr int kSceneCBSimFloat4Rows = 5376 / 16;   // 336 float4 rows
inline constexpr int kSceneCBSimOriginFloat = 1100;       // cb1[275] = floats [1100..1102]

// A 4x4 group is a view-matrix candidate when its 3x3 (storage [r*4+c]) is
// orthonormal: unit-length, mutually perpendicular columns. The test is
// convention-agnostic (a transpose is orthonormal iff the original is), so
// it holds however the game stores the view.
inline bool isOrtho3x3(const float m[16], float tol) noexcept {
    for (int c = 0; c < 3; ++c) {
        const float n = m[c] * m[c] + m[4 + c] * m[4 + c] + m[8 + c] * m[8 + c];
        if (n < (1.0f - tol) * (1.0f - tol) || n > (1.0f + tol) * (1.0f + tol)) return false;
    }
    for (int a = 0; a < 3; ++a) {
        for (int b = a + 1; b < 3; ++b) {
            const float d = m[a] * m[b] + m[4 + a] * m[4 + b] + m[8 + a] * m[8 + b];
            if (d < -tol || d > tol) return false;
        }
    }
    return true;
}

// |t + origin.R| for a candidate group: the row-vector eye origin through
// the 3x3, negated, against the translation row. ZERO when the group IS
// this frame's view (v_view = v_world.R + t maps the eye origin to 0).
// A previous frame's view is orthonormal too -- but its translation belongs
// to the previous frame's eye, which at a transition is metres to
// kilometres away, so this is what rejects it.
inline float viewOriginMatch(const float m[16], const float origin[3]) noexcept {
    float d2 = 0.0f;
    for (int k = 0; k < 3; ++k) {
        const float e = m[12 + k] + origin[0] * m[k] + origin[1] * m[4 + k] + origin[2] * m[8 + k];
        d2 += e * e;
    }
    return std::sqrt(d2);
}

inline constexpr int kSceneCBFindMaxCandidates = 8;

struct SceneCBViewFind {
    int startRow = -1;         // float4 row of the winning group; -1 = none
    float originMatch = 0.0f;  // its |t + origin.R| (0 when none)
    int orthoGroups = 0;       // ALL orthonormal groups seen (may exceed stored)
    int originRejected = 0;    // orthonormal groups failing the origin test
    int candidateCount = 0;    // stored start rows (capped at kSceneCBFindMaxCandidates)
    int candidateRows[kSceneCBFindMaxCandidates] = {};
};

// Scan float4 rows [0, float4Rows-4] for the group whose 3x3 is orthonormal
// within orthoTol AND whose translation matches `origin` within originTol;
// the winner is the closest match. NaN origins never match (every
// comparison is false), so a garbage fill reads as "none", not a false hit.
inline SceneCBViewFind locateSceneCBView(const float* cb, int float4Rows, const float origin[3],
                                         float orthoTol, float originTol) noexcept {
    SceneCBViewFind out;
    for (int r = 0; r + 4 <= float4Rows; ++r) {
        const float* m = cb + r * 4;
        if (!isOrtho3x3(m, orthoTol)) continue;
        ++out.orthoGroups;
        if (out.candidateCount < kSceneCBFindMaxCandidates) out.candidateRows[out.candidateCount++] = r;
        const float match = viewOriginMatch(m, origin);
        if (match <= originTol && (out.startRow < 0 || match < out.originMatch)) {
            out.startRow = r;
            out.originMatch = match;
        } else {
            ++out.originRejected;
        }
    }
    return out;
}

// The general 3x3+translation inverse, row convention (translation row at
// [12..14], last row (0,0,0,1)). Exact for orthonormal 3x3s (the common
// case: view matrices and the live base are rotations); the cofactor form
// tolerates a little scale. No pivoting -- a near-singular 3x3 is the
// caller's plausibility gate's business, not this function's.
inline void affineInverse4x4(const float m[16], float out[16]) noexcept {
    const float a = m[0], b = m[1], c = m[2];
    const float d = m[4], e = m[5], f = m[6];
    const float g = m[8], h = m[9], i = m[10];
    const float invDet = 1.0f / (a * (e * i - f * h) + d * (c * h - b * i) + g * (b * f - c * e));
    float r[16];
    r[0] = (e * i - f * h) * invDet;
    r[1] = (c * h - b * i) * invDet;
    r[2] = (b * f - c * e) * invDet;
    r[3] = 0.0f;
    r[4] = (f * g - d * i) * invDet;
    r[5] = (a * i - c * g) * invDet;
    r[6] = (c * d - a * f) * invDet;
    r[7] = 0.0f;
    r[8] = (d * h - e * g) * invDet;
    r[9] = (b * g - a * h) * invDet;
    r[10] = (a * e - b * d) * invDet;
    r[11] = 0.0f;
    // Row convention: v.M = v.R + t, so M^-1 = [R^-1, -t.R^-1] -- the
    // translation through the inverse 3x3, NEGATED.
    r[12] = -(m[12] * r[0] + m[13] * r[4] + m[14] * r[8]);
    r[13] = -(m[12] * r[1] + m[13] * r[5] + m[14] * r[9]);
    r[14] = -(m[12] * r[2] + m[13] * r[6] + m[14] * r[10]);
    r[15] = 1.0f;
    std::memcpy(out, r, sizeof(r));
}

// The acting build's postmultiply naming for the one 4x4 multiply: the
// view correction is V_bad x liveM^-1, i.e. the stored view POSTmultiplied
// by the inverse base. Same operation as premul4x4 (first argument is the
// left factor either way); the name exists so the acting call site reads
// the way the math is described.
inline void postmul4x4(const float a[16], const float b[16], float out[16]) noexcept {
    premul4x4(a, b, out);
}

// ---------------------------------------------------------------------------
// CHANGE 15 (2026-09-24, "the acting render-time patch"): the pure logic the
// act path runs per fill.

// Which base the patch premultiplies with, chosen by the detector's own
// pool-vs-camera selector -- the rule flight 134813 measured on 16 events
// across three flights, and whose necessity skip 22726 proved (scene-old
// hyperspace entry: the objects had NOT switched while the mailbox already
// held the tunnel base -- a selector-less live patch would have flashed
// 1614 m there). The writer's scene graph and the rendered object pool can
// switch a frame apart; the patch agrees with the POOL.
//   scene-new   -> the LIVE mailbox base (NoPatch when the probe gave
//                  nothing usable -- never guess);
//   scene-old   -> the HELD base, NEVER the live one (22726);
//   unclear     -> the HELD base, the proven-safe side (NoPatch when there
//                  is no held base).
enum class PatchBaseChoice : uint8_t { UseLive, UseHeld, NoPatch };

inline const char* patchBaseChoiceText(PatchBaseChoice c) noexcept {
    switch (c) {
    case PatchBaseChoice::UseLive: return "live";
    case PatchBaseChoice::UseHeld: return "held";
    case PatchBaseChoice::NoPatch: return "none";
    }
    return "?";
}

inline PatchBaseChoice choosePatchBase(SceneChoice scene, bool haveLive, bool haveHeld) noexcept {
    switch (scene) {
    case SceneChoice::New:     return haveLive ? PatchBaseChoice::UseLive : PatchBaseChoice::NoPatch;
    case SceneChoice::Old:     return haveHeld ? PatchBaseChoice::UseHeld : PatchBaseChoice::NoPatch;
    case SceneChoice::Unclear: return haveHeld ? PatchBaseChoice::UseHeld : PatchBaseChoice::NoPatch;
    }
    return PatchBaseChoice::NoPatch;
}

// The VP-defense shape test. A group X found near the located view is the
// composed view-projection only if V_bad^-1 x X comes out PROJ-LIKE: the
// strict D3D perspective shape, row convention, v' = v.M:
//   [ sx  0  0  0 ]      zero lanes:    [1],[2],[3],[4],[6],[7],[8],[9],
//   [ 0 sy  0  0 ]                       [12],[13],[15]
//   [ 0  0 sz  w ]      [11] ~= +/-1    (the perspective-divide lane)
//   [ 0  0 tz  0 ]      [0],[5] same sign, sane magnitude; [10],[14] live.
// STRICT on purpose: a false VP identification must never write, so the
// zero lanes are absolute (1e-3), the perspective lane is within 1% of
// exactly +/-1, and any second candidate makes the whole check ambiguous
// (the caller writes the view group only and logs it).
inline bool projLike4x4(const float m[16]) noexcept {
    const float zeroTol = 1e-3f;
    const int zeroLanes[10] = {1, 2, 3, 4, 6, 7, 8, 9, 12, 13};
    for (int lane : zeroLanes) {
        if (std::fabs(m[lane]) > zeroTol) return false;
    }
    if (std::fabs(m[15]) > zeroTol) return false;
    if (std::fabs(std::fabs(m[11]) - 1.0f) > 0.01f) return false;
    if (m[0] * m[5] <= 0.0f) return false;                 // same sign, both live
    if (std::fabs(m[0]) < 0.01f || std::fabs(m[0]) > 100.0f) return false;
    if (std::fabs(m[5]) < 0.01f || std::fabs(m[5]) > 100.0f) return false;
    if (std::fabs(m[10]) < 1e-6f || std::fabs(m[10]) > 1e6f) return false;
    if (std::fabs(m[14]) < 1e-6f || std::fabs(m[14]) > 1e8f) return false;
    return true;
}

}  // namespace tfeb
}  // namespace edvr
