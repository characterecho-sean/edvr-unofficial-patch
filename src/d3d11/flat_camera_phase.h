#pragma once

// The pure half of the upstream camera injector's wiring into the flat runtime
// (docs/design-flat-camera-integration.md, the 2026-09-29 wiring addendum).
// Header-only, no I/O, no game or D3D dependency, so the rigs run the same code
// the DLL does. What lives here: the provenance of the camera rows (the phase
// the injector applies at the source is IN the rows the game uploads, and the
// resolver wants them without it) and the checks that prove the claim; then the
// route decisions, the frame-window gate, the per-call admission table, the
// flush of cameras left with a stale phase, the Legacy fallback hysteresis (its
// two defaults), the camera census, and the text of every new log line.

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "flat_camera_ownership.h"
#include "flat_projection_math.h"

namespace edvr {

// ---------------------------------------------------------------------------
// Row provenance.
//
// The scene CB rows b1[270..275] the flat runtime captures are the game's own
// upload. Under the injector the game derived them from a jittered frustum, so
// rows 0..3 carry the raster phase; the resolver's contract is unjittered rows
// (flat_mono_resolve.h) evaluated at the unjittered screen coordinate. The
// shift the game's derivation applies is the legacy scope's shift exactly
// (flat_projection_math.h flatJitterForwardColumns: x += ndcX*w, y += ndcY*w,
// per row, w = component 3; c2_derive_test proves the equality on the derive
// model), so its inverse is a subtraction of the same terms. Rows 4 and 5
// (view direction, camera position) and the depth row's near term are not
// touched by a bound-pair shift and are not touched here. The phase is in
// render pixels, positive right/down, exactly what the injector was given;
// the D3D sign convention is flatProjectionJitter's.
//
// The HLSL twin (flat_mono_shader_source.h, unjitterRows) implements the same
// two lines; flat_mono_resolve_test runs the real shader against this
// function's expectation.
// ---------------------------------------------------------------------------
inline bool flatCameraUnjitterRows(float (&rows)[6][4], float pixelX, float pixelY,
                                   uint32_t renderW, uint32_t renderH) {
    // Nothing to remove: leave the rows bit-identical, whatever their content.
    if (pixelX == 0.0f && pixelY == 0.0f) return true;
    FlatProjectionJitter jitter{};
    if (!flatProjectionJitter(pixelX, pixelY, renderW, renderH, jitter)) return false;
    using flat_projection_detail::checkedFloat;
    float next[4][4];
    for (size_t i = 0; i < 4; ++i) {
        if (!std::isfinite(rows[i][0]) || !std::isfinite(rows[i][1]) || !std::isfinite(rows[i][3])) return false;
        if (!checkedFloat(rows[i][0] - static_cast<double>(jitter.ndcX) * rows[i][3], next[i][0])) return false;
        if (!checkedFloat(rows[i][1] - static_cast<double>(jitter.ndcY) * rows[i][3], next[i][1])) return false;
    }
    for (size_t i = 0; i < 4; ++i) { rows[i][0] = next[i][0]; rows[i][1] = next[i][1]; }
    return true;
}

// The phase a set of rows carries can be MEASURED from the rows alone, because
// the composed rows are a rotation times the projection: with f the view
// direction (component 3 of rows 0..2) and the x column (component 0 of rows
// 0..2) equal to p0*right + ndcX*f, the component of the x column along f is
// exactly ndcX (right is perpendicular to f); the y column likewise gives
// ndcY. The measurement is the total shift including any off-centre bound the
// camera has of its own, so a single frame proves nothing about the injected
// part -- two frames do: the DIFFERENCE of the measured shifts must equal the
// difference of the phases the frames are CLAIMED to carry, for any camera
// motion (a roll or a turn changes right, up and f but not this projection).
// A claim that the rows carry a phase they do not (or the reverse) leaves
// exactly the claimed difference behind.
inline bool flatCameraMeasureRowShift(const float (&rows)[6][4], double& ndcX, double& ndcY) {
    const double f0 = rows[0][3], f1 = rows[1][3], f2 = rows[2][3];
    const double ff = f0 * f0 + f1 * f1 + f2 * f2;
    if (!std::isfinite(ff) || !(ff > 1.0e-6)) return false;
    ndcX = (static_cast<double>(rows[0][0]) * f0 + static_cast<double>(rows[1][0]) * f1 +
            static_cast<double>(rows[2][0]) * f2) / ff;
    ndcY = (static_cast<double>(rows[0][1]) * f0 + static_cast<double>(rows[1][1]) * f1 +
            static_cast<double>(rows[2][1]) * f2) / ff;
    return std::isfinite(ndcX) && std::isfinite(ndcY);
}

enum class FlatCameraPairVerdict : uint8_t { Skipped, Consistent, Inconsistent };
struct FlatCameraPairResult {
    FlatCameraPairVerdict verdict = FlatCameraPairVerdict::Skipped;
    float maxError = 0.0f; // largest |measured difference - claimed difference| over x and y, NDC units
};
constexpr double kFlatCameraPairMinNdc = 2.0e-5;    // claimed difference below this in both axes has no power
constexpr double kFlatCameraPairTolerance = 2.0e-6; // NDC (float rounding of the game's composition is ~2e-7)
inline FlatCameraPairResult flatCameraCheckRowPair(const float (&cur)[6][4], float curX, float curY,
                                                   const float (&prev)[6][4], float prevX, float prevY,
                                                   uint32_t renderW, uint32_t renderH) {
    FlatCameraPairResult out{};
    FlatProjectionJitter now{}, before{};
    if (!flatProjectionJitter(curX, curY, renderW, renderH, now) ||
        !flatProjectionJitter(prevX, prevY, renderW, renderH, before)) return out;
    const double claimX = static_cast<double>(now.ndcX) - before.ndcX;
    const double claimY = static_cast<double>(now.ndcY) - before.ndcY;
    if (std::fabs(claimX) < kFlatCameraPairMinNdc && std::fabs(claimY) < kFlatCameraPairMinNdc) return out;
    double cx = 0, cy = 0, px = 0, py = 0;
    if (!flatCameraMeasureRowShift(cur, cx, cy) || !flatCameraMeasureRowShift(prev, px, py)) return out;
    const double errX = std::fabs((cx - px) - claimX), errY = std::fabs((cy - py) - claimY);
    const double worst = errX > errY ? errX : errY;
    out.maxError = static_cast<float>(worst);
    out.verdict = worst <= kFlatCameraPairTolerance ? FlatCameraPairVerdict::Consistent
                                                    : FlatCameraPairVerdict::Inconsistent;
    return out;
}

// What the live pair checks add up to (one window, printed on the rows line).
struct FlatCameraPairStats {
    uint64_t checked = 0, skipped = 0, mismatched = 0;
    float maxError = 0.0f;
    void note(const FlatCameraPairResult& r) {
        if (r.verdict == FlatCameraPairVerdict::Skipped) { ++skipped; return; }
        ++checked;
        if (r.verdict == FlatCameraPairVerdict::Inconsistent) ++mismatched;
        if (r.maxError > maxError) maxError = r.maxError;
    }
};

// ---------------------------------------------------------------------------
// Which route owns the frame, and the two decisions that follow from it.
// ---------------------------------------------------------------------------
enum class FlatCameraRoute : uint8_t { Off, None, Legacy, Upstream };
inline const char* flatCameraRouteName(FlatCameraRoute r) {
    switch (r) {
        case FlatCameraRoute::Off: return "off";
        case FlatCameraRoute::None: return "none";
        case FlatCameraRoute::Legacy: return "legacy";
        case FlatCameraRoute::Upstream: return "upstream";
    }
    return "?";
}

// Whether the phase machine runs this frame. Off (the injector is not wanted)
// and Legacy are EXACTLY the expression flat_runtime used before the wiring:
// (jitter wanted && not observing && a legacy plan exists). Upstream needs no
// legacy plan but still yields to observation -- a frame the resolve will skip
// must not be jittered at the source, or the player sees the raw phase. None
// (neither route may mutate) never jitters.
inline bool flatCameraPhaseEnabled(FlatCameraRoute route, bool jitterWanted, bool observing,
                                   bool legacyPlanExists) {
    switch (route) {
        case FlatCameraRoute::Upstream: return jitterWanted && !observing;
        case FlatCameraRoute::None: return false;
        case FlatCameraRoute::Off:
        case FlatCameraRoute::Legacy: break;
    }
    return jitterWanted && !observing && legacyPlanExists;
}

// The phase the camera rows captured this frame carry. Only the injector
// writes a phase into the game's own upload, so only an Upstream frame in
// which at least one injection landed has any; the legacy scope patches
// private copies bound for the draw and leaves the captured bytes alone.
struct FlatCameraRowsPhase { float x = 0.0f, y = 0.0f; };
inline FlatCameraRowsPhase flatCameraRowsPhase(FlatCameraRoute route, uint32_t applied,
                                               float phaseX, float phaseY) {
    FlatCameraRowsPhase out;
    if (route == FlatCameraRoute::Upstream && applied != 0) { out.x = phaseX; out.y = phaseY; }
    return out;
}

// ---------------------------------------------------------------------------
// The frame window. The phase is read live by the refresh detour, so a stale
// non-zero phase would keep injecting after a skipped Present, a mode change
// or a resize. The gate closes at every Present edge and opens only after the
// frame's phase is chosen; it also lapses on its own after kExpiryMs, and it
// admits only the thread that runs Present, the one thread the phase machine
// and the counters are safe on.
// ---------------------------------------------------------------------------
enum class FlatCameraGateVerdict : uint8_t { Admit, Disarmed, Expired, OffThread };

class FlatCameraGate {
public:
    static constexpr uint64_t kExpiryMs = 500;
    // The Present edge, on the thread that runs Present: the window closes and
    // that thread becomes the owner.
    void disarm(uint32_t presentThread) {
        owner_.store(presentThread, std::memory_order_relaxed);
        armedAtMs_.store(0, std::memory_order_release);
    }
    // The frame's phase is chosen: the window opens.
    void arm(uint64_t nowMs) { armedAtMs_.store(nowMs ? nowMs : 1, std::memory_order_release); }
    uint32_t ownerThread() const { return owner_.load(std::memory_order_relaxed); }
    FlatCameraGateVerdict check(uint32_t callerThread, uint64_t nowMs) const {
        const uint32_t owner = owner_.load(std::memory_order_relaxed);
        if (!owner) return FlatCameraGateVerdict::Disarmed;
        if (callerThread != owner) return FlatCameraGateVerdict::OffThread;
        const uint64_t at = armedAtMs_.load(std::memory_order_acquire);
        if (!at) return FlatCameraGateVerdict::Disarmed;
        if (nowMs > at && nowMs - at > kExpiryMs) return FlatCameraGateVerdict::Expired;
        return FlatCameraGateVerdict::Admit;
    }
private:
    std::atomic<uint32_t> owner_{0};
    std::atomic<uint64_t> armedAtMs_{0};
};

// ---------------------------------------------------------------------------
// Admission: one table, one answer per refresh call. Only a kind-3 camera in
// an armed window on the owner thread under Upstream ownership with a phase to
// apply is ever mutated; every other call passes through untouched and is
// counted by the reason it was.
// ---------------------------------------------------------------------------
enum class FlatCameraAdmit : uint8_t {
    Inject,      // kind 3, gate open, Upstream owns, phase non-zero
    Warming,     // kind 3, gate open, Upstream owns, phase zero (warm-up, no plan)
    NotUpstream, // kind 3, gate open, another route owns the frame
    GateClosed,  // kind 3 but the window is closed or lapsed (the stale class)
    OffThread,   // not the thread that runs Present: nothing here may be touched
    Unsupported, // kinds 4 and 5, named and never mutated
    OtherKind,   // any other readable kind (0, 1, 2, ...): passes through
    Unreadable,  // the kind could not be read
};
struct FlatCameraAdmitInput {
    bool readable = false;
    uint32_t kind = 0;
    FlatCameraGateVerdict gate = FlatCameraGateVerdict::Disarmed;
    bool upstreamOwns = false;
    bool phaseNonzero = false;
};
inline FlatCameraAdmit flatCameraAdmit(const FlatCameraAdmitInput& in) {
    if (in.gate == FlatCameraGateVerdict::OffThread) return FlatCameraAdmit::OffThread;
    if (!in.readable) return FlatCameraAdmit::Unreadable;
    if (in.kind == 4 || in.kind == 5) return FlatCameraAdmit::Unsupported;
    if (in.kind != 3) return FlatCameraAdmit::OtherKind;
    if (in.gate != FlatCameraGateVerdict::Admit) return FlatCameraAdmit::GateClosed;
    if (!in.upstreamOwns) return FlatCameraAdmit::NotUpstream;
    if (!in.phaseNonzero) return FlatCameraAdmit::Warming;
    return FlatCameraAdmit::Inject;
}
inline const char* flatCameraAdmitName(FlatCameraAdmit a) {
    switch (a) {
        case FlatCameraAdmit::Inject: return "inject";
        case FlatCameraAdmit::Warming: return "warming";
        case FlatCameraAdmit::NotUpstream: return "not-upstream";
        case FlatCameraAdmit::GateClosed: return "stale";
        case FlatCameraAdmit::OffThread: return "off-thread";
        case FlatCameraAdmit::Unsupported: return "unsupported";
        case FlatCameraAdmit::OtherKind: return "other-kind";
        case FlatCameraAdmit::Unreadable: return "unreadable";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// The flush. The injection is transient (the bound pair is restored after the
// call) but the derived blocks keep the phase they were derived with, and the
// game re-derives a camera only when its dirty bits say so. A camera this
// session injected, then not (a warm-up, a fallback, a closed window, F8, a
// resize), would keep rendering with the last phase while the phase machine
// says zero. So the first un-injected call of such a camera raises the dirty
// bits once and the game re-derives it clean. Only cameras this session
// injected are ever touched, and the set forgets a camera the moment it is
// flushed: once per injected-to-uninjected edge.
// ---------------------------------------------------------------------------
class FlatCameraInjectedSet {
public:
    static constexpr size_t kCapacity = 64;
    bool empty() const { return used_ == 0; }
    size_t size() const { return used_; }
    uint64_t evicted() const { return evicted_; }
    bool contains(uintptr_t camera) const { return camera && find(camera) < kCapacity; }
    // An injection landed on this camera. Idempotent (it refreshes the camera's
    // recency); a full set forgets the camera injected least recently (counted)
    // -- a camera forgotten is a flush that cannot happen, so the count is on
    // the tick.
    void noteInjected(uintptr_t camera) {
        if (!camera) return;
        const size_t at = find(camera);
        if (at < kCapacity) { stamp_[at] = ++clock_; return; }
        size_t victim = kCapacity;
        for (size_t i = 0; i < kCapacity; ++i) {
            if (!slot_[i]) { victim = i; break; }
            if (victim == kCapacity || stamp_[i] < stamp_[victim]) victim = i;
        }
        if (slot_[victim]) ++evicted_; else ++used_;
        slot_[victim] = camera;
        stamp_[victim] = ++clock_;
    }
    // True exactly once for a camera that was injected since it was last
    // flushed: the caller must raise its dirty bits now.
    bool takeForFlush(uintptr_t camera) {
        const size_t i = camera ? find(camera) : kCapacity;
        if (i >= kCapacity) return false;
        slot_[i] = 0;
        --used_;
        return true;
    }
    void clear() { for (size_t i = 0; i < kCapacity; ++i) { slot_[i] = 0; stamp_[i] = 0; } used_ = 0; }
private:
    size_t find(uintptr_t camera) const {
        for (size_t i = 0; i < kCapacity; ++i) if (slot_[i] == camera) return i;
        return kCapacity;
    }
    uintptr_t slot_[kCapacity] = {};
    uint64_t stamp_[kCapacity] = {};
    uint64_t clock_ = 0;
    size_t used_ = 0;
    uint64_t evicted_ = 0;
};
// Which calls may flush: kind 3 (so a memory address that now holds something
// else is never written), on the owner thread, not injected this call.
inline bool flatCameraFlushEligible(FlatCameraAdmit a) {
    return a == FlatCameraAdmit::Warming || a == FlatCameraAdmit::NotUpstream ||
           a == FlatCameraAdmit::GateClosed;
}
inline bool flatCameraFlushDecision(FlatCameraInjectedSet& set, uintptr_t camera, FlatCameraAdmit a) {
    return flatCameraFlushEligible(a) && set.takeForFlush(camera);
}

// ---------------------------------------------------------------------------
// The Legacy fallback hysteresis. The injector is only useful while the
// detour is landing injections. When frames that were asked for a phase (an
// armed window, a named scene) keep passing with none landed, the frame fails
// every time (no-raster-application) and nothing ever treats it; the existing
// ownership policy already knows how to hand such a frame to Legacy, through
// upstreamUnsupported. This is the counter that feeds it.
//
// THE DEFAULTS, in the one place they live. Sean may override either:
//   ON  = closed armed scene frames with no injection, since the last one that
//         landed, before Legacy takes the frame;
//   OFF = consecutive frames in which a kind-3 camera reached the detour, while
//         on Legacy, before Upstream is tried again.
// Warm-up frames (no phase) and frames without a scene neither count nor reset:
// after a failed frame the phase machine warms up again for two frames, so the
// misses arrive interleaved with them.
// ---------------------------------------------------------------------------
constexpr uint32_t kFlatCameraFallbackFramesOn = 3;
constexpr uint32_t kFlatCameraFallbackFramesOff = 60;

// The camera path is on whenever the flat profile has a temporal mode selected
// (2026-09-29: there is no setting; the draw-time adapter is only the automatic
// fallback). With the mode off the injector is not wanted: no hook is installed,
// nothing is written, and no "flat camera" line is logged. What hands a frame to
// the draw-time adapter, all through the hysteresis above:
//   no injectable camera      armed scene frames end with nothing landed;
//   a prologue mismatch       the game changed under the hook (an update): it never
//                             installs, so nothing ever lands and the same count fills;
//   failed camera writes      kFlatCameraWriteFailureLimit in one window stands the
//                             hook down (below), so nothing lands from then on.
constexpr bool flatCameraPathWanted(bool flatProfile, bool temporalModeEnabled) {
    return flatProfile && temporalModeEnabled;
}

// THE write-failure limit, in the one place it lives: this many failed camera
// writes (a camera the game unmapped under us, a page protection that changed)
// in one 5 s window stand the hook down for the session.
constexpr uint64_t kFlatCameraWriteFailureLimit = 8;
constexpr bool flatCameraWriteFailureStandDown(uint64_t failuresInWindow) {
    return failuresInWindow >= kFlatCameraWriteFailureLimit;
}

class FlatCameraFallback {
public:
    bool active() const { return active_; }
    uint32_t misses() const { return misses_; }
    uint32_t kind3Frames() const { return kind3Frames_; }
    uint64_t engagements() const { return engagements_; }
    uint64_t releases() const { return releases_; }
    // A frame closed. armed: the injector was selected and the frame's phase was
    // non-zero. sceneNamed: a scene depth was named. injected: at least one
    // injection landed. kind3Called: a kind-3 camera reached the detour, whoever
    // owned the frame.
    void closeFrame(bool armed, bool sceneNamed, bool injected, bool kind3Called) {
        if (!active_) {
            if (injected) { misses_ = 0; return; }
            if (armed && sceneNamed && ++misses_ >= kFlatCameraFallbackFramesOn) {
                active_ = true; kind3Frames_ = 0; ++engagements_;
            }
            return;
        }
        if (!kind3Called) { kind3Frames_ = 0; return; }
        if (++kind3Frames_ >= kFlatCameraFallbackFramesOff) {
            active_ = false; misses_ = 0; kind3Frames_ = 0; ++releases_;
        }
    }
    void reset() { active_ = false; misses_ = kind3Frames_ = 0; }
private:
    bool active_ = false;
    uint32_t misses_ = 0, kind3Frames_ = 0;
    uint64_t engagements_ = 0, releases_ = 0;
};

// ---------------------------------------------------------------------------
// The per-frame ownership protocol, as flat_runtime drives it: begin() at the
// frame start (before the phase machine's beginFrame), close() after the
// phase machine has finished the frame that just ended. It owns the ownership
// machine, the decision and the fallback, so the rig runs the very code the
// DLL runs. Before the wiring nothing called the close: the history flag the
// tick printed was permanently "invalid".
// ---------------------------------------------------------------------------
class FlatCameraFrameCore {
public:
    // legacyPlanExists is flat_runtime's own fact (a legacy projection plan is
    // available); everything else the selection needs the core knows.
    const FlatCameraOwnershipDecision& begin(bool legacyPlanExists) {
        flatCameraOwnerBegin(owner_);
        FlatCameraGroupInput in;
        in.upstreamCertified = true;              // the detour re-verifies kind 3 per call
        in.upstreamUnsupported = fallback_.active();
        in.legacyEligible = legacyPlanExists;
        decision_ = flatCameraOwnerSelect(owner_, in);
        valid_ = true;
        closed_ = false;
        if (decision_.historyReset) pendingHistoryReset_ = true;
        return decision_;
    }
    // The frame that just ended. clean is the phase machine's own verdict on it
    // (backend acceptance, complete coverage, no phase failure); applied is
    // whether an injection landed; sceneNamed whether a scene depth was named;
    // kind3Called whether a kind-3 camera reached the detour. False when there
    // was no open frame (a second close, or none begun): nothing changes.
    bool close(bool phaseNonzero, bool applied, bool clean, bool sceneNamed, bool kind3Called) {
        if (!valid_ || closed_) return false;
        closed_ = true;
        flatCameraOwnerClose(owner_, decision_, applied, clean);
        fallback_.closeFrame(decision_.owner == FlatCameraOwner::Upstream && phaseNonzero,
                             sceneNamed, applied, kind3Called);
        return true;
    }
    // One-shot: true once for a decision that switched the history identity,
    // and the runtime resets its temporal history when it is.
    bool takeHistoryReset() { const bool r = pendingHistoryReset_; pendingHistoryReset_ = false; return r; }
    bool valid() const { return valid_; }
    bool historyValid() const { return owner_.historyValid; }
    const FlatCameraOwnershipDecision& decision() const { return decision_; }
    FlatCameraRoute route() const {
        if (!valid_) return FlatCameraRoute::Off;
        switch (decision_.owner) {
            case FlatCameraOwner::Upstream: return FlatCameraRoute::Upstream;
            case FlatCameraOwner::Legacy: return FlatCameraRoute::Legacy;
            case FlatCameraOwner::None: break;
        }
        return FlatCameraRoute::None;
    }
    const FlatCameraFallback& fallback() const { return fallback_; }
    // The cached history and the decision do not survive a resize or a device
    // change; the fallback's verdict does not either (a new contract may work).
    void reset() {
        owner_ = FlatCameraOwnershipState{};
        decision_ = FlatCameraOwnershipDecision{};
        valid_ = false;
        closed_ = true;
        pendingHistoryReset_ = false;
        fallback_.reset();
    }
    // The decision goes stale when the injector is not wanted (key off).
    void invalidate() { valid_ = false; closed_ = true; }
private:
    FlatCameraOwnershipState owner_{};
    FlatCameraOwnershipDecision decision_{};
    FlatCameraFallback fallback_{};
    bool valid_ = false, closed_ = true, pendingHistoryReset_ = false;
};

// ---------------------------------------------------------------------------
// The census: which cameras reach the refresh, of what kind, how often, and
// from which call site. A fixed table, no allocation, touched on the owner
// thread only and reported once per window -- never on the call path's I/O.
// It is the evidence the main-versus-auxiliary grouping decision waits for.
// ---------------------------------------------------------------------------
struct FlatCameraCensusEntry {
    uintptr_t camera = 0;
    uint32_t kind = 0;
    uint64_t calls = 0, injected = 0;
};
constexpr uint32_t kFlatCameraKnownCallers[4] = {0x594E13, 0x594EAB, 0x594FE1, 0x58DE73};

class FlatCameraCensus {
public:
    static constexpr size_t kCapacity = 16;
    size_t used() const { return used_; }
    uint64_t overflow() const { return overflow_; }
    const FlatCameraCensusEntry& at(size_t i) const { return entry_[i]; }
    uint64_t kindCount(size_t i) const { return kinds_[i]; }        // 0..5, 6 = other, 7 = unreadable
    uint64_t callerCount(size_t i) const { return callers_[i]; }    // 0..3 known, 4 = other
    void note(uintptr_t camera, uint32_t kind, bool injected) {
        for (size_t i = 0; i < used_; ++i) if (entry_[i].camera == camera) {
            entry_[i].kind = kind; ++entry_[i].calls; if (injected) ++entry_[i].injected; return;
        }
        if (used_ < kCapacity) {
            entry_[used_] = {camera, kind, 1, injected ? 1u : 0u};
            ++used_;
        } else ++overflow_;
    }
    void noteKind(bool readable, uint32_t kind) {
        ++kinds_[!readable ? 7 : kind <= 5 ? kind : 6];
    }
    // callerRva: the call site's offset from the game module, or 0 when unknown.
    void noteCaller(uint64_t callerRva) {
        size_t slot = 4;
        for (size_t i = 0; i < 4; ++i) if (callerRva == kFlatCameraKnownCallers[i]) slot = i;
        ++callers_[slot];
    }
    void reset() { *this = FlatCameraCensus{}; }
private:
    FlatCameraCensusEntry entry_[kCapacity] = {};
    size_t used_ = 0;
    uint64_t overflow_ = 0, kinds_[8] = {}, callers_[5] = {};
};

// ---------------------------------------------------------------------------
// The text of the new log lines, built here so the rigs print exactly what the
// DLL writes. Each returns the length written (snprintf's convention).
// ---------------------------------------------------------------------------
struct FlatCameraTickFields {
    uint64_t refreshCalls = 0, injected = 0, warming = 0, kindRefusals = 0, unsupported = 0;
    const char* owner = "legacy/none";
    const char* history = "invalid";
    uint64_t closes = 0, cleanCloses = 0, stale = 0, offThread = 0, notUpstream = 0;
    uint64_t flushed = 0, flushFailed = 0, writeFailures = 0, historyResets = 0;
    uint32_t fallbackOn = kFlatCameraFallbackFramesOn, fallbackOff = kFlatCameraFallbackFramesOff;
    bool fallbackActive = false;
    uint64_t fallbacks = 0, setEvicted = 0;
};
// The first seven fields, in this order, are the tick as it was before the wiring
// (its eighth, trace=, went with the trace setting on 2026-09-29: a reader of the
// old line finds the rest where it was, closes= now following history=).
inline int flatCameraFormatTick(char* out, size_t size, const FlatCameraTickFields& t) {
    return std::snprintf(out, size,
        "flat camera inject 5s: refresh-calls=%llu injected=%llu warming=%llu "
        "kind-refusals=%llu unsupported=%llu owner=%s history=%s "
        "closes=%llu clean-closes=%llu stale=%llu off-thread=%llu not-upstream=%llu "
        "flushed=%llu flush-failed=%llu write-failures=%llu history-resets=%llu "
        "fallback-frames=%u/%u fallback=%s fallbacks=%llu set-evicted=%llu",
        (unsigned long long)t.refreshCalls, (unsigned long long)t.injected,
        (unsigned long long)t.warming, (unsigned long long)t.kindRefusals,
        (unsigned long long)t.unsupported, t.owner, t.history,
        (unsigned long long)t.closes, (unsigned long long)t.cleanCloses,
        (unsigned long long)t.stale, (unsigned long long)t.offThread,
        (unsigned long long)t.notUpstream, (unsigned long long)t.flushed,
        (unsigned long long)t.flushFailed, (unsigned long long)t.writeFailures,
        (unsigned long long)t.historyResets, t.fallbackOn, t.fallbackOff,
        t.fallbackActive ? "legacy" : "upstream", (unsigned long long)t.fallbacks,
        (unsigned long long)t.setEvicted);
}

// Printed every window while the hook is installed, INCLUDING when nothing was
// observed (cameras=0): a line that is absent means the code never ran.
inline int flatCameraFormatCensus(char* out, size_t size, const FlatCameraCensus& c,
                                  uint64_t frames, uint64_t injectedCalls) {
    size_t n = 0;
    auto put = [&](const char* fmt, auto... args) {
        if (n >= size) return;
        const int w = std::snprintf(out + n, size - n, fmt, args...);
        if (w > 0) n += static_cast<size_t>(w);
    };
    put("flat camera census 5s: frames=%llu injected-per-frame=%.1f cameras=%u overflow=%llu "
        "kinds=[0:%llu,1:%llu,2:%llu,3:%llu,4:%llu,5:%llu,other:%llu,unreadable:%llu] top=[",
        (unsigned long long)frames, frames ? static_cast<double>(injectedCalls) / frames : 0.0,
        static_cast<unsigned>(c.used()), (unsigned long long)c.overflow(),
        (unsigned long long)c.kindCount(0), (unsigned long long)c.kindCount(1),
        (unsigned long long)c.kindCount(2), (unsigned long long)c.kindCount(3),
        (unsigned long long)c.kindCount(4), (unsigned long long)c.kindCount(5),
        (unsigned long long)c.kindCount(6), (unsigned long long)c.kindCount(7));
    bool taken[FlatCameraCensus::kCapacity] = {};
    for (size_t rank = 0; rank < 8 && rank < c.used(); ++rank) {
        size_t best = FlatCameraCensus::kCapacity;
        for (size_t i = 0; i < c.used(); ++i)
            if (!taken[i] && (best == FlatCameraCensus::kCapacity || c.at(i).calls > c.at(best).calls)) best = i;
        if (best == FlatCameraCensus::kCapacity) break;
        taken[best] = true;
        put("%s0x%llx:%u:%llu:%llu", rank ? " " : "", (unsigned long long)c.at(best).camera,
            c.at(best).kind, (unsigned long long)c.at(best).calls, (unsigned long long)c.at(best).injected);
    }
    put("] callers=[+0x%X=%llu,+0x%X=%llu,+0x%X=%llu,+0x%X=%llu,other=%llu]",
        kFlatCameraKnownCallers[0], (unsigned long long)c.callerCount(0),
        kFlatCameraKnownCallers[1], (unsigned long long)c.callerCount(1),
        kFlatCameraKnownCallers[2], (unsigned long long)c.callerCount(2),
        kFlatCameraKnownCallers[3], (unsigned long long)c.callerCount(3),
        (unsigned long long)c.callerCount(4));
    return static_cast<int>(n);
}

struct FlatCameraRowsFields {
    uint64_t frames = 0, unjitteredResolves = 0, zeroPhaseResolves = 0;
    FlatCameraPairStats pairs;
    uint64_t legacyAppliedUnderUpstream = 0, legacyPrepSkipped = 0;
};
inline int flatCameraFormatRows(char* out, size_t size, const FlatCameraRowsFields& r) {
    return std::snprintf(out, size,
        "flat camera rows 5s: frames=%llu unjittered-resolves=%llu zero-phase-resolves=%llu "
        "row-pairs=%llu row-pairs-skipped=%llu row-pair-mismatch=%llu max-err=%.3e "
        "legacy-applied-under-upstream=%llu legacy-prep-skipped=%llu",
        (unsigned long long)r.frames, (unsigned long long)r.unjitteredResolves,
        (unsigned long long)r.zeroPhaseResolves, (unsigned long long)r.pairs.checked,
        (unsigned long long)r.pairs.skipped, (unsigned long long)r.pairs.mismatched,
        static_cast<double>(r.pairs.maxError), (unsigned long long)r.legacyAppliedUnderUpstream,
        (unsigned long long)r.legacyPrepSkipped);
}

// A note when the owner or the fallback changes, so a flight shows the
// transition and not only the counters either side of it.
inline int flatCameraFormatOwner(char* out, size_t size, uint64_t frame, const char* from,
                                 const char* to, const char* reason, bool historyReset,
                                 bool fallbackActive) {
    return std::snprintf(out, size,
        "flat camera inject owner: frame=%llu %s -> %s reason=%s history-reset=%u fallback=%s",
        (unsigned long long)frame, from, to, reason, historyReset ? 1u : 0u,
        fallbackActive ? "legacy" : "upstream");
}
// The first few pair checks that disagree, with what was claimed and measured.
inline int flatCameraFormatRowsMismatch(char* out, size_t size, uint64_t frame, float phaseX,
                                        float phaseY, float previousPhaseX, float previousPhaseY,
                                        float error) {
    return std::snprintf(out, size,
        "flat camera rows mismatch: frame=%llu claimed phase=(%.5g,%.5g) previous=(%.5g,%.5g) "
        "row-shift error=%.3e NDC (the rows do not carry the difference of the claimed phases)",
        (unsigned long long)frame, phaseX, phaseY, previousPhaseX, previousPhaseY,
        static_cast<double>(error));
}

} // namespace edvr
