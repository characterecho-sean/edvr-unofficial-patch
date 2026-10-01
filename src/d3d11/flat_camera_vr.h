#pragma once

// The pure half of the VR world route's camera injection mode (flat_camera_inject.h, "THE VR WORLD ROUTE'S INJECTION MODE";
// design doc section 82, stage 2). Header-only, no I/O, no game or D3D dependency -- the same discipline as flat_camera_phase.h,
// so tools\flat_camera_vr_test runs the code the DLL runs. What lives here: the role of a kind-3 camera (is it a screen view,
// and is it the first-person weapon's), the per-call admission, the flush decision for a camera that stopped being injected, the
// per-frame counters with their exactly-one-outcome rule, the table of excluded signatures, the per-frame mode word and the
// rules that move it, and the planner that puts the pieces in the order the detour runs them. The detour
// (flat_camera_inject.cpp, refreshPreVr) does only what the planner says and the writes the planner cannot do.
//
// What the census settled (flight 1, 4.63 million refresh calls) and this file encodes:
//   kind 3 on foot: the world, about 60 calls a frame BEFORE the tone -- the scene camera (near 0.025) and the first-person
//     weapon camera (near 0.0675, a tighter field of view), the SAME camera object; plus a few auxiliary kind-3 cameras (a
//     square 90-degree one, a zoom-like one) that need a role before they get a phase;
//   kind 5: the VR eye cameras, once a frame per eye AFTER the tone. Kinds 0 and 1 are shadow and light cameras.
// The camera OBJECT is no identity (the left eye camera of the cockpit became the world camera on foot), so nothing here keys on
// a pointer: the kind and the role are decided on every call, and the only per-pointer fact is the injected set's (a camera
// this session injected and has not yet flushed).

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "flat_camera_inject.h"   // the interface's types: FlatCameraVrFrame, Role, Counters, Excluded
#include "flat_camera_phase.h"    // flatCameraAdmit, the gate verdict, the injected set, the write-failure limit

namespace edvr {

// ---------------------------------------------------------------------------
// The role of a kind-3 camera.
//
// A SCREEN VIEW is a camera whose aspect is the panel's: finite, positive and within 4% (relative) of the route's screen aspect
// (renderW / renderH). Scene and first-person cameras are screen views; a probe, a spot light's square camera or a zoom-like
// camera is not. Of the screen views the first-person one has either the larger near plane (near >= 1.5 x the smallest near a
// screen view has shown: the census's composed rows read 0.0675 against 0.025) or, relative to the SAME FRAME's scene camera, the
// tighter field of view (fov <= 0.92 x the widest field of view the frame's screen views have shown). Flight 2 found the
// struct's near plane equal for both (0.025, the 0.0675 is in the composed rows) and the struct's fov different (0.8203 against
// about 0.98 rad), so the near test never fired and every first-person call was counted as a scene call. Everything else -- a
// different aspect, a non-finite number, a screen aspect that is not positive -- is AUXILIARY: an unknown role is never guessed,
// and an auxiliary camera is never injected.
// ---------------------------------------------------------------------------
constexpr double kFlatCameraVrScreenTolerance = 0.04;        // relative, against the screen aspect
constexpr double kFlatCameraVrFirstPersonNearRatio = 1.5;    // near / sceneNear at or above this is the weapon
constexpr double kFlatCameraVrFirstPersonFovRatio = 0.92;    // fov / the frame's widest screen-view fov at or below this is the weapon

inline FlatCameraVrRole flatCameraVrRole(float aspect, float nearZ, float screenAspect, float sceneNear,
                                         float fov = std::numeric_limits<float>::quiet_NaN(),
                                         float sceneFov = std::numeric_limits<float>::quiet_NaN()) {
    if (!std::isfinite(screenAspect) || !(screenAspect > 0.0f)) return FlatCameraVrRole::Auxiliary;
    if (!std::isfinite(aspect) || !(aspect > 0.0f)) return FlatCameraVrRole::Auxiliary;
    const double anchor = static_cast<double>(screenAspect);
    if (!(std::fabs(static_cast<double>(aspect) - anchor) <= kFlatCameraVrScreenTolerance * anchor))
        return FlatCameraVrRole::Auxiliary;
    // A NaN on either side of the comparison is false: no anchor (or no near plane) is a scene call, never a guess.
    if (sceneNear > 0.0f && static_cast<double>(nearZ) >= kFlatCameraVrFirstPersonNearRatio * static_cast<double>(sceneNear))
        return FlatCameraVrRole::FirstPerson;
    // The same camera object with a tighter field of view than the frame's scene camera: no field of view, or no scene field of
    // view yet this frame (the weapon's first calls arrive before any scene call), is a scene call, never a guess.
    if (std::isfinite(fov) && fov > 0.0f && std::isfinite(sceneFov) && sceneFov > 0.0f &&
        static_cast<double>(fov) <= kFlatCameraVrFirstPersonFovRatio * static_cast<double>(sceneFov))
        return FlatCameraVrRole::FirstPerson;
    return FlatCameraVrRole::Scene;
}
inline const char* flatCameraVrRoleName(FlatCameraVrRole role) {
    switch (role) {
        case FlatCameraVrRole::Scene: return "scene";
        case FlatCameraVrRole::FirstPerson: return "first-person";
        case FlatCameraVrRole::Auxiliary: return "auxiliary";
    }
    return "?";
}

// The anchor of the near-plane test: the smallest near among the screen views seen, kept across frames so the first call of a
// frame is already classified. A first-person call that arrives before any scene call is taken for a scene call (no anchor
// yet); the scene call that follows lowers the anchor, and every later call is right. Both get the same phase, so the only
// thing the mistake can change is a counter.
//
// The anchor of the field-of-view test is the FRAME's own: the widest field of view a Scene-role screen view has shown since
// beginFrame, and nothing before the first one. It is never carried across frames, because a scene camera that zooms (an aimed
// weapon) must not turn every scene call of the next frame into a first-person one against last frame's wider anchor; the price is
// that a first-person call arriving before the frame's first scene call is counted as a scene call (flight 2's sequence starts
// with three weapon calls, so four of the five groups are recognised: 12 of 15 calls). The anchor follows Scene calls only, so a
// first-person call never raises it.
class FlatCameraVrRoleTracker {
public:
    FlatCameraVrRole classify(float aspect, float nearZ, float screenAspect,
                              float fov = std::numeric_limits<float>::quiet_NaN()) {
        const FlatCameraVrRole role = flatCameraVrRole(aspect, nearZ, screenAspect, sceneNear_, fov, sceneFov_);
        if (role != FlatCameraVrRole::Auxiliary && std::isfinite(nearZ) && nearZ > 0.0f &&
            (!(sceneNear_ > 0.0f) || nearZ < sceneNear_))
            sceneNear_ = nearZ;
        if (role == FlatCameraVrRole::Scene && std::isfinite(fov) && fov > 0.0f && fov > sceneFov_)
            sceneFov_ = fov;
        return role;
    }
    float sceneNear() const { return sceneNear_; }
    float sceneFov() const { return sceneFov_; }
    void beginFrame() { sceneFov_ = 0.0f; }   // the near anchor is kept across frames, the fov anchor is the frame's own
    void reset() { sceneNear_ = 0.0f; sceneFov_ = 0.0f; }
private:
    float sceneNear_ = 0.0f;
    float sceneFov_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Admission: one reason per refresh call.
//
// It does not fork the flat table's answers: the kind, the frame window and the observe-only switch are flatCameraAdmit's (the
// input is mapped onto its input and its answer is translated), and only the role, the route's trigger window and the VR
// activity are added on top. flatCameraAdmit and the pins on it are untouched.
//
//   Inject        screen view, injection on, frame window open, before the trigger, phase non-zero, owner thread
//   Warming       the same with a zero phase: admitted, nothing written
//   AfterTrigger  a screen view after the route's trigger (the tone): the world passes all precede it
//   RoleExcluded  kind 3, auxiliary: never injected, counted, its signature recorded
//   NotActive     kind 3 in a frame that does not inject (pass-through)
//   Observed      kind 3 in an observe-only frame (the census)
//   Stale         kind 3 with the frame window closed or lapsed
//   OffThread     not the thread that runs Present: nothing here may be touched
//   Unsupported   kinds 4 and 5 (the eyes), named and never injected
//   OtherKind     any other readable kind (0, 1, 2 ...)
//   Unreadable    the kind could not be read
// ---------------------------------------------------------------------------
enum class FlatCameraVrMode : uint8_t { PassThrough, Observe, Inject };
enum class FlatCameraVrAdmit : uint8_t {
    Inject, Warming, AfterTrigger, RoleExcluded, NotActive, Observed, Stale, OffThread, Unsupported, OtherKind, Unreadable,
};
inline const char* flatCameraVrAdmitName(FlatCameraVrAdmit a) {
    switch (a) {
        case FlatCameraVrAdmit::Inject: return "inject";
        case FlatCameraVrAdmit::Warming: return "warming";
        case FlatCameraVrAdmit::AfterTrigger: return "after-trigger";
        case FlatCameraVrAdmit::RoleExcluded: return "role-excluded";
        case FlatCameraVrAdmit::NotActive: return "not-active";
        case FlatCameraVrAdmit::Observed: return "observed";
        case FlatCameraVrAdmit::Stale: return "stale";
        case FlatCameraVrAdmit::OffThread: return "off-thread";
        case FlatCameraVrAdmit::Unsupported: return "unsupported";
        case FlatCameraVrAdmit::OtherKind: return "other-kind";
        case FlatCameraVrAdmit::Unreadable: return "unreadable";
    }
    return "?";
}

struct FlatCameraVrAdmitInput {
    bool readable = false;
    uint32_t kind = 0;
    FlatCameraGateVerdict gate = FlatCameraGateVerdict::Disarmed;
    FlatCameraVrMode mode = FlatCameraVrMode::PassThrough;
    bool windowOpen = false;     // the route's injection window: open at the frame step, closed at the trigger
    bool phaseNonzero = false;
    // Meaningful only when flatCameraVrWantsRole(input): a caller that never sets it gets the safe answer, Auxiliary.
    FlatCameraVrRole role = FlatCameraVrRole::Auxiliary;
};

// The mapping onto the flat table: the route is the Upstream owner exactly while the frame injects, and the census's
// observe-only switch is the observe mode. Nothing else is decided here.
inline FlatCameraAdmitInput flatCameraVrBaseInput(const FlatCameraVrAdmitInput& in, bool phaseNonzero) {
    FlatCameraAdmitInput base;
    base.readable = in.readable;
    base.kind = in.kind;
    base.gate = in.gate;
    base.upstreamOwns = in.mode == FlatCameraVrMode::Inject;
    base.phaseNonzero = phaseNonzero;
    base.observeOnly = in.mode == FlatCameraVrMode::Observe;
    return base;
}

// True when the call could be injected but for its role and the trigger window, i.e. the flat table says Inject for it with a
// phase: only then does the detour pay for reading the camera's aspect and near plane. Derived from flatCameraAdmit itself.
inline bool flatCameraVrWantsRole(const FlatCameraVrAdmitInput& in) {
    return flatCameraAdmit(flatCameraVrBaseInput(in, true)) == FlatCameraAdmit::Inject;
}

inline FlatCameraVrAdmit flatCameraVrAdmit(const FlatCameraVrAdmitInput& in) {
    const FlatCameraAdmit base = flatCameraAdmit(flatCameraVrBaseInput(in, in.phaseNonzero));
    switch (base) {
        case FlatCameraAdmit::OffThread: return FlatCameraVrAdmit::OffThread;
        case FlatCameraAdmit::Unreadable: return FlatCameraVrAdmit::Unreadable;
        case FlatCameraAdmit::Unsupported: return FlatCameraVrAdmit::Unsupported;
        case FlatCameraAdmit::OtherKind: return FlatCameraVrAdmit::OtherKind;
        case FlatCameraAdmit::Observed: return FlatCameraVrAdmit::Observed;
        case FlatCameraAdmit::GateClosed: return FlatCameraVrAdmit::Stale;
        case FlatCameraAdmit::NotUpstream: return FlatCameraVrAdmit::NotActive;
        case FlatCameraAdmit::Warming:
        case FlatCameraAdmit::Inject:
            // A screen-view candidate: the role, then the trigger window, then (through the base answer) the phase.
            if (in.role == FlatCameraVrRole::Auxiliary) return FlatCameraVrAdmit::RoleExcluded;
            if (!in.windowOpen) return FlatCameraVrAdmit::AfterTrigger;
            return base == FlatCameraAdmit::Inject ? FlatCameraVrAdmit::Inject : FlatCameraVrAdmit::Warming;
    }
    return FlatCameraVrAdmit::OtherKind;   // unreachable: every flat answer is handled above
}

// ---------------------------------------------------------------------------
// The flush. A camera this session injected and now is not injecting (a release, a role exclusion, a call after the trigger, a
// warm-up, a kind change on a reused object) keeps the last phase in its derived blocks, because the game re-derives only what
// its dirty bits name: its first un-injected call raises the two bits once (flatCameraFlushDecision's rule, through the same
// injected set). Eligible: every readable, owner-thread call that was not injected. Not eligible: an injected call (the camera
// stays a candidate), an off-thread call (the set is the owner thread's) and an unreadable one (nothing about the memory is
// known). Observed IS eligible here, unlike the flat table's: in a frame the route has released, the census still runs, and a
// camera the route injected must not be left with its phase because the census is watching; the only write that frame makes is
// this one, and only for a camera in the set, which only an injection fills.
// ---------------------------------------------------------------------------
inline bool flatCameraVrFlushEligible(FlatCameraVrAdmit a) {
    switch (a) {
        case FlatCameraVrAdmit::Warming:
        case FlatCameraVrAdmit::AfterTrigger:
        case FlatCameraVrAdmit::RoleExcluded:
        case FlatCameraVrAdmit::NotActive:
        case FlatCameraVrAdmit::Observed:
        case FlatCameraVrAdmit::Stale:
        case FlatCameraVrAdmit::Unsupported:
        case FlatCameraVrAdmit::OtherKind:
            return true;
        case FlatCameraVrAdmit::Inject:
        case FlatCameraVrAdmit::OffThread:
        case FlatCameraVrAdmit::Unreadable:
            return false;
    }
    return false;
}
inline bool flatCameraVrFlushDecision(FlatCameraInjectedSet& set, uintptr_t camera, FlatCameraVrAdmit a) {
    return flatCameraVrFlushEligible(a) && set.takeForFlush(camera);
}

// ---------------------------------------------------------------------------
// The per-frame counters. One entry point, note(), gives every owner-thread call exactly one of the first-level outcomes:
//   calls == sceneInjected + sceneRefused + firstPersonInjected + firstPersonRefused + warming + auxiliary + afterTrigger +
//            notActive + unsupported + otherKind + unreadable + stale
// (flatCameraVrOutcomeSum), in every mode. An off-thread call is not an owner-thread call: it is counted in offThread alone,
// lock-free, from any thread. injectedKind[] counts INJECTED calls by kind and is incremented only for a landed injection,
// which only a kind-3 call can have.
// ---------------------------------------------------------------------------
constexpr uint32_t flatCameraVrKindIndex(bool readable, uint32_t kind) {
    return !readable ? 7u : kind <= 5u ? kind : 6u;
}
inline uint32_t flatCameraVrOutcomeSum(const FlatCameraVrCounters& c) {
    return c.sceneInjected + c.sceneRefused + c.firstPersonInjected + c.firstPersonRefused + c.warming + c.auxiliary +
           c.afterTrigger + c.notActive + c.unsupported + c.otherKind + c.unreadable + c.stale;
}
class FlatCameraVrTally {
public:
    void reset() {
        c_ = FlatCameraVrCounters{};
        offThread_.store(0, std::memory_order_relaxed);
    }
    // One call's outcome. `landed`: an Inject call whose phase was written and whose return was redirected. `fov`: the struct's
    // field of view when the call's frustum was read (NaN otherwise); the narrowest and widest of the screen views' (not an
    // outcome: a diagnostic for the route's 5 s line, which shows whether the struct carries two fields of view at all).
    void note(FlatCameraVrAdmit admit, FlatCameraVrRole role, bool readable, uint32_t kind, bool landed,
              float fov = std::numeric_limits<float>::quiet_NaN()) {
        if (admit == FlatCameraVrAdmit::OffThread) { noteOffThread(); return; }
        ++c_.calls;
        if (role != FlatCameraVrRole::Auxiliary && std::isfinite(fov) && fov > 0.0f &&
            (admit == FlatCameraVrAdmit::Inject || admit == FlatCameraVrAdmit::Warming || admit == FlatCameraVrAdmit::AfterTrigger)) {
            if (!(c_.fovNarrowest <= fov)) c_.fovNarrowest = fov;   // a NaN is not <= anything: the first reading sets both
            if (!(c_.fovWidest >= fov)) c_.fovWidest = fov;
        }
        switch (admit) {
            case FlatCameraVrAdmit::Inject:
                if (role == FlatCameraVrRole::FirstPerson) { if (landed) ++c_.firstPersonInjected; else ++c_.firstPersonRefused; }
                else { if (landed) ++c_.sceneInjected; else ++c_.sceneRefused; }
                if (landed) ++c_.injectedKind[flatCameraVrKindIndex(readable, kind)];
                break;
            case FlatCameraVrAdmit::Warming: ++c_.warming; break;
            case FlatCameraVrAdmit::AfterTrigger: ++c_.afterTrigger; break;
            case FlatCameraVrAdmit::RoleExcluded: ++c_.auxiliary; break;
            case FlatCameraVrAdmit::NotActive:
            case FlatCameraVrAdmit::Observed: ++c_.notActive; break;
            case FlatCameraVrAdmit::Stale: ++c_.stale; break;
            case FlatCameraVrAdmit::Unsupported: ++c_.unsupported; break;
            case FlatCameraVrAdmit::OtherKind: ++c_.otherKind; break;
            case FlatCameraVrAdmit::Unreadable: ++c_.unreadable; break;
            case FlatCameraVrAdmit::OffThread: break;   // handled above
        }
    }
    void noteWriteFailure() { ++c_.writeFailures; }
    void noteFlushed() { ++c_.flushed; }
    void noteOffThread() { offThread_.fetch_add(1, std::memory_order_relaxed); }   // any thread
    FlatCameraVrCounters snapshot() const {
        FlatCameraVrCounters c = c_;
        c.offThread = offThread_.load(std::memory_order_relaxed);
        return c;
    }
private:
    FlatCameraVrCounters c_{};
    std::atomic<uint32_t> offThread_{0};
};

// ---------------------------------------------------------------------------
// The excluded signatures: the first eight distinct (aspect, fov, near, far, caller) an auxiliary call showed, for the route to
// log. Written by the owner thread on the call path, read by anyone, lock-free: an entry is filled before the count that
// publishes it, and is never changed afterwards except its call count. Two signatures are the same when every float agrees to
// 0.1% (an animated spot light is one row, not eight) and the call site is the same; a NaN agrees with a NaN, so a stream of
// unreadable cameras is one row. No I/O, no allocation.
// ---------------------------------------------------------------------------
class FlatCameraVrExcludedTable {
public:
    static constexpr size_t kCapacity = 8;
    static constexpr double kSameTolerance = 1.0e-3;
    void note(float aspect, float fov, float nearZ, float farZ, uint64_t callerRva) {
        const size_t n = count_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) {
            Entry& e = entry_[i];
            if (e.callerRva == callerRva && same(e.aspect, aspect) && same(e.fov, fov) && same(e.nearZ, nearZ) && same(e.farZ, farZ)) {
                e.calls.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        if (n >= kCapacity) { overflow_.fetch_add(1, std::memory_order_relaxed); return; }
        Entry& e = entry_[n];
        e.aspect = aspect; e.fov = fov; e.nearZ = nearZ; e.farZ = farZ; e.callerRva = callerRva;
        e.calls.store(1, std::memory_order_relaxed);
        count_.store(n + 1, std::memory_order_release);   // publishes the entry
    }
    // Up to `max` rows, oldest first; returns how many were written.
    size_t copy(FlatCameraVrExcluded* out, size_t max) const {
        if (!out) return 0;
        const size_t n = count_.load(std::memory_order_acquire);
        size_t written = 0;
        for (size_t i = 0; i < n && written < max; ++i, ++written) {
            out[written].aspect = entry_[i].aspect;
            out[written].fov = entry_[i].fov;
            out[written].nearZ = entry_[i].nearZ;
            out[written].farZ = entry_[i].farZ;
            out[written].callerRva = entry_[i].callerRva;
            out[written].calls = entry_[i].calls.load(std::memory_order_relaxed);
        }
        return written;
    }
    size_t used() const { return count_.load(std::memory_order_acquire); }
    uint64_t overflow() const { return overflow_.load(std::memory_order_relaxed); }   // exclusions whose signature found no row
private:
    struct Entry {
        float aspect = 0, fov = 0, nearZ = 0, farZ = 0;
        uint64_t callerRva = 0;
        std::atomic<uint64_t> calls{0};
    };
    static bool same(float a, float b) {
        if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b);
        if (a == b) return true;   // also equal infinities
        const double da = a, db = b;
        const double scale = std::fabs(da) > std::fabs(db) ? std::fabs(da) : std::fabs(db);
        return std::fabs(da - db) <= kSameTolerance * scale;
    }
    Entry entry_[kCapacity];
    std::atomic<size_t> count_{0};
    std::atomic<uint64_t> overflow_{0};
};

// ---------------------------------------------------------------------------
// The injection arithmetic: exactly what the flat injector writes (flat_camera_inject.cpp's inject block), from the frame's
// phase in RENDER pixels, positive right/down, and the render size: the bound pair moves by +phaseX / renderW and
// -phaseY / renderH (the D3D sign convention). A phase that is not finite, or a size of zero, is no phase: nothing is written.
// ---------------------------------------------------------------------------
inline bool flatCameraVrPhaseNonzero(float phaseX, float phaseY, uint32_t renderW, uint32_t renderH) {
    return renderW != 0 && renderH != 0 && std::isfinite(phaseX) && std::isfinite(phaseY) && (phaseX != 0.0f || phaseY != 0.0f);
}
inline void flatCameraVrBound(float entryX, float entryY, float phaseX, float phaseY, uint32_t renderW, uint32_t renderH,
                              float* outX, float* outY) {
    *outX = entryX + phaseX / static_cast<float>(renderW);
    *outY = entryY - phaseY / static_cast<float>(renderH);
}

// ---------------------------------------------------------------------------
// The per-frame mode word. One atomic word the detour reads on every call, written on the thread that runs Present by the
// route's frame step (flatCameraVrFrame) and by the census's (flatCameraInjectObserveFrame). Zero -- the flat profile's whole
// life, and a VR process the route never drives -- means the detour runs the code it always ran; the flat path never reads
// another bit.
//   Driven   the route has stepped a frame (set by flatCameraVrFrame, never by the flat profile)
//   Inject   this frame injects
//   Observe  this frame reports every call to the census's observer (alone: observe-only; with Inject: injection with reports)
// ---------------------------------------------------------------------------
constexpr uint32_t kFlatCameraVrBitDriven = 1u;
constexpr uint32_t kFlatCameraVrBitInject = 2u;
constexpr uint32_t kFlatCameraVrBitObserve = 4u;

// The word for a frame the route stepped. A hook that is not live honours nothing: Driven alone is pass-through.
inline uint32_t flatCameraVrBitsForFrame(bool inject, bool observe, bool live) {
    if (!live) return kFlatCameraVrBitDriven;
    return kFlatCameraVrBitDriven | (inject ? kFlatCameraVrBitInject : 0u) | (observe ? kFlatCameraVrBitObserve : 0u);
}
inline FlatCameraVrMode flatCameraVrModeOfBits(uint32_t bits) {
    return (bits & kFlatCameraVrBitInject) ? FlatCameraVrMode::Inject
         : (bits & kFlatCameraVrBitObserve) ? FlatCameraVrMode::Observe : FlatCameraVrMode::PassThrough;
}
inline bool flatCameraVrObserves(uint32_t bits) { return (bits & kFlatCameraVrBitObserve) != 0u; }
// What the census's frame step does to the word: the route's injection frame keeps its mode (it is never switched to
// observe-only); any other frame of a driven detour becomes observe-only, as it is for the census alone; an undriven detour
// (the word is zero) is not touched -- its observe-only switch is the flat code's own.
inline uint32_t flatCameraVrBitsAfterCensusFrame(uint32_t bits) {
    if (!(bits & kFlatCameraVrBitDriven) || (bits & kFlatCameraVrBitInject)) return bits;
    return bits | kFlatCameraVrBitObserve;
}
// Quiet: the last frame the route stepped asked for neither injection nor observation, and no camera this session injected
// still waits for its first un-injected call (its flush). The route stops stepping once its key is off and this is true.
inline bool flatCameraVrQuietFor(uint32_t bits, bool injectedSetEmpty) {
    return (bits & (kFlatCameraVrBitInject | kFlatCameraVrBitObserve)) == 0u && injectedSetEmpty;
}

// The route's frame step is the detour's Present edge: in an INJECTING frame a window the step armed is honoured for the frame
// window's own expiry (FlatCameraGate::kExpiryMs) from the STEP, even when something else (the census's per-frame re-arm) keeps
// the gate itself open, so a route that stopped stepping cannot leave the last frame's phase injecting. Every other mode keeps
// the gate's own verdict: nothing is injected there, and the census's window field stays what it is for the census alone.
inline FlatCameraGateVerdict flatCameraVrEffectiveGate(FlatCameraVrMode mode, FlatCameraGateVerdict gate, uint64_t stepAtMs, uint64_t nowMs) {
    if (mode != FlatCameraVrMode::Inject || gate != FlatCameraGateVerdict::Admit) return gate;
    if (stepAtMs == 0 || (nowMs > stepAtMs && nowMs - stepAtMs > FlatCameraGate::kExpiryMs)) return FlatCameraGateVerdict::Expired;
    return gate;
}

// The stand-down: the flat path's rule (kFlatCameraWriteFailureLimit failed camera writes in one 5 s window), fed by the frame
// that ended. True when the window has reached the limit.
class FlatCameraVrFailureWindow {
public:
    static constexpr uint64_t kWindowMs = 5000;
    bool note(uint64_t nowMs, uint32_t failures) {
        if (startMs_ == 0 || nowMs < startMs_ || nowMs - startMs_ >= kWindowMs) { startMs_ = nowMs ? nowMs : 1; total_ = 0; }
        total_ += failures;
        return flatCameraWriteFailureStandDown(total_);
    }
    uint64_t total() const { return total_; }
private:
    uint64_t startMs_ = 0, total_ = 0;
};

// ---------------------------------------------------------------------------
// The planner: one call's decisions, in the order the detour runs them. The detour reads the kind, asks wantsFrustum, reads the
// frustum when it does, calls plan, tells the census (pre), does the flush write the plan names, does the injection writes
// the plan names, and tells finish how it went. Nothing in it reads memory or writes a camera.
// ---------------------------------------------------------------------------
struct FlatCameraVrCallIn {
    uintptr_t camera = 0;
    bool readable = false;
    uint32_t kind = 0;
    FlatCameraGateVerdict gate = FlatCameraGateVerdict::Disarmed;   // after flatCameraVrEffectiveGate
    FlatCameraVrMode mode = FlatCameraVrMode::PassThrough;
    uint64_t callerRva = 0;
};
struct FlatCameraVrFrustum {   // NaN in every field: not read
    float aspect = std::numeric_limits<float>::quiet_NaN();
    float fov = std::numeric_limits<float>::quiet_NaN();
    float nearZ = std::numeric_limits<float>::quiet_NaN();
    float farZ = std::numeric_limits<float>::quiet_NaN();
};
struct FlatCameraVrPlan {
    FlatCameraVrAdmit admit = FlatCameraVrAdmit::NotActive;
    FlatCameraVrRole role = FlatCameraVrRole::Auxiliary;
    bool roleKnown = false;   // the role was decided for this call (kind 3, injection, frame window open)
    bool flush = false;       // the camera was in the injected set and has left it: raise its dirty bits now
    bool inject = false;      // write the phase
    float fov = std::numeric_limits<float>::quiet_NaN();   // the struct's field of view, when the role was decided (NaN otherwise)
};

class FlatCameraVrCore {
public:
    // The route's frame step: the frame's phase and anchor, the counters reset, the injection window open.
    void beginFrame(const FlatCameraVrFrame& frame, uint64_t nowMs) {
        phaseX_ = frame.phaseX; phaseY_ = frame.phaseY;
        renderW_ = frame.renderW; renderH_ = frame.renderH;
        screenAspect_ = frame.screenAspect;
        phaseNonzero_ = flatCameraVrPhaseNonzero(frame.phaseX, frame.phaseY, frame.renderW, frame.renderH);
        stepAtMs_ = nowMs ? nowMs : 1;
        tally_.reset();
        roles_.beginFrame();   // the field-of-view anchor is the frame's own (the near anchor is kept)
        windowOpen_.store(true, std::memory_order_release);
    }
    void closeWindow() { windowOpen_.store(false, std::memory_order_release); }
    bool windowOpen() const { return windowOpen_.load(std::memory_order_acquire); }
    uint64_t stepAtMs() const { return stepAtMs_; }
    bool phaseNonzero() const { return phaseNonzero_; }
    float screenAspect() const { return screenAspect_; }
    float sceneNear() const { return roles_.sceneNear(); }

    FlatCameraVrAdmitInput admitInput(const FlatCameraVrCallIn& in) const {
        FlatCameraVrAdmitInput a;
        a.readable = in.readable;
        a.kind = in.kind;
        a.gate = in.gate;
        a.mode = in.mode;
        a.windowOpen = windowOpen();
        a.phaseNonzero = phaseNonzero_;
        return a;
    }
    // Whether this call's aspect and near plane are needed (the detour reads them only then).
    bool wantsFrustum(const FlatCameraVrCallIn& in) const { return flatCameraVrWantsRole(admitInput(in)); }

    FlatCameraVrPlan plan(const FlatCameraVrCallIn& in, const FlatCameraVrFrustum& frustum, FlatCameraInjectedSet& set) {
        FlatCameraVrPlan p;
        FlatCameraVrAdmitInput a = admitInput(in);
        p.roleKnown = flatCameraVrWantsRole(a);
        if (p.roleKnown) {
            p.role = roles_.classify(frustum.aspect, frustum.nearZ, screenAspect_, frustum.fov);
            p.fov = frustum.fov;
            a.role = p.role;
        }
        p.admit = flatCameraVrAdmit(a);
        if (p.admit == FlatCameraVrAdmit::RoleExcluded)
            excluded_.note(frustum.aspect, frustum.fov, frustum.nearZ, frustum.farZ, in.callerRva);
        p.inject = p.admit == FlatCameraVrAdmit::Inject;
        p.flush = flatCameraVrFlushDecision(set, in.camera, p.admit);
        return p;
    }
    // The call's one outcome. `landed`: the phase was written and the return redirected.
    void finish(const FlatCameraVrPlan& plan, const FlatCameraVrCallIn& in, bool landed) {
        tally_.note(plan.admit, plan.role, in.readable, in.kind, landed, plan.fov);
    }
    // The bound pair for the frame's phase (what the detour writes).
    void bound(float entryX, float entryY, float* outX, float* outY) const {
        flatCameraVrBound(entryX, entryY, phaseX_, phaseY_, renderW_, renderH_, outX, outY);
    }
    FlatCameraVrTally& tally() { return tally_; }
    const FlatCameraVrTally& tally() const { return tally_; }
    const FlatCameraVrExcludedTable& excluded() const { return excluded_; }
private:
    float phaseX_ = 0.0f, phaseY_ = 0.0f, screenAspect_ = 0.0f;
    uint32_t renderW_ = 0, renderH_ = 0;
    bool phaseNonzero_ = false;
    uint64_t stepAtMs_ = 0;
    std::atomic<bool> windowOpen_{false};
    FlatCameraVrRoleTracker roles_;
    FlatCameraVrTally tally_;
    FlatCameraVrExcludedTable excluded_;
};

} // namespace edvr
