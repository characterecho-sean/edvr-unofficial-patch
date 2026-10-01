// The VR on-foot world route, the pure half (docs/design-flat-temporal-aa-2026-09-23.md, section 82).
//
// WHY. On foot Elite draws the world ONCE, flat, into the 2D screen's target and shows that screen to each eye
// with one composite draw; today each eye then gets its own upscaler pass over its view of that finished
// texture. The route resolves the world once, where the game drew it (the HDR scene target H, at the flat HDR
// route's trigger, the tone: flat_hdr_route.h), and hands the eyes the resolved screen through the UI layer, so
// the two eye passes stand aside for the frames the route owns.
//
// WHAT IS HERE, all pure (tools\vr_world_route_test drives every function, and the runtime calls the very same
// code):
//   - the key: experimental.temporal_aa_on_foot_world, off (the default) or auto;
//   - the ownership machine: the route treats frames, is WARM after kVrWorldWarmFrames treated frames in a row,
//     OWNS the world from then on, and is released by the gate, the layer, a scene reset, a run of frames it did not
//     treat, or the late-write latch. Only an owned route moves the eye shift and the screen draws;
//   - the selector's facts and reasons (what "the trigger is a world the route may resolve" means in VR, where
//     there is no prefix model: screen motion's named source stands in for it);
//   - the layer's and the door's per-frame predicates, and the 5 s window and the take/release lines.
// It never touches D3D: the runtime hands it facts.
//
// THE KEY-OFF CONTRACT. With the key off, every function here answers "the route is not there": the machine never
// leaves Off, owned() is false, the eye shift is never suppressed, the layer is never told the world is the route's,
// and the door never runs layer-only. The rig pins each answer.
#pragma once
#include "../common/temporal_math.h"   // kTemporalJitterCount: the window's default phase count
#include "flat_hdr_route.h"
#include "flat_mono_refusal.h"   // the stage 2 experiment build's pixel classes and the census's counters (pure)
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace edvr {

// ---- the key ---------------------------------------------------------------------------------------------
// experimental.temporal_aa_on_foot_world: auto (the route where it applies) or off. Off by default: the route has
// not flown. A key that is absent reads as the default; a value that is present and is not "auto" reads as off, so a
// typo leaves on-foot VR exactly as it was and never switches the route on by accident.
enum class VrWorldKey : uint8_t { Off, Auto };
inline VrWorldKey vrWorldKeyFromText(const char* text) {
    if (!text) return VrWorldKey::Off;
    const char* a = "auto";
    for (; *a; ++a, ++text) {
        char c = *text;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != *a) return VrWorldKey::Off;
    }
    return *text == 0 ? VrWorldKey::Auto : VrWorldKey::Off;
}
inline const char* vrWorldKeyName(VrWorldKey key) { return key == VrWorldKey::Auto ? "auto" : "off"; }

// ---- the ownership machine --------------------------------------------------------------------------------
// Treated frames in a row before the layer takes the screen: the world's own history is warm (an upscaler needs
// about this many frames to stop reading its first ones as new). The eye route serves the frames until then.
constexpr uint32_t kVrWorldWarmFrames = 8;
// Frames in a row the route may fail to treat while it owns the world before it lets go (the eye route serves each
// of those frames, with the eye shift still off: a single refusal is not worth a flap back to the eye jitter).
constexpr uint32_t kVrWorldGraceFrames = 3;

enum class VrWorldState : uint8_t {
    Off,        // the key is off: the route does nothing and is not there
    Observing,  // the key is on, the route is not treating (no gate, no layer, no trigger, or a refusal)
    Warming,    // treating, fewer than kVrWorldWarmFrames in a row: the eyes are still the eye route's
    Owned,      // warm: the layer takes the screen on every frame the route treats, and the eye shift is off
    Latched,    // the late-write latch tripped: off until the key is flipped
};
inline const char* vrWorldStateName(VrWorldState s) {
    switch (s) {
        case VrWorldState::Off: return "off";
        case VrWorldState::Observing: return "observing";
        case VrWorldState::Warming: return "warming";
        case VrWorldState::Owned: return "owned";
        case VrWorldState::Latched: return "latched";
    }
    return "?";
}

// Why an owned route stopped owning, for the release line.
enum class VrWorldRelease : uint8_t { None, KeyOff, LayerNotLive, GateLost, Declined, SceneReset, Latched };
inline const char* vrWorldReleaseName(VrWorldRelease r) {
    switch (r) {
        case VrWorldRelease::None: return "none";
        case VrWorldRelease::KeyOff: return "key-off";
        case VrWorldRelease::LayerNotLive: return "layer-not-live";
        case VrWorldRelease::GateLost: return "on-foot-gate-lost";
        case VrWorldRelease::Declined: return "frames-not-treated";
        case VrWorldRelease::SceneReset: return "scene-reset";
        case VrWorldRelease::Latched: return "late-writes-latched";
    }
    return "?";
}

// What the frame that just ended showed.
struct VrWorldFrameEnd {
    bool keyOn = false;
    bool layerLive = false;    // fix.ui_quality is on and a temporal mode runs: the layer can take the screen
    bool gate = false;         // the on-foot world-screen gate held for the frame
    bool treated = false;      // the route resolved the frame (and its H write-back is in the frame)
    bool lateWrites = false;   // a treated frame whose H was written after the resolve (the latch's input)
    bool sceneReset = false;   // boarding, disembarking or a jump: the transition detector reset the eyes' history
};
struct VrWorldStep {
    bool entered = false;                         // the route became owned at this boundary
    VrWorldRelease released = VrWorldRelease::None;   // it stopped owning, and why
};

struct VrWorldMachine {
    VrWorldState state = VrWorldState::Off;
    uint32_t run = 0;    // treated frames in a row
    uint32_t miss = 0;   // frames in a row, with the gate held, the route did not treat
    FlatHdrLatch latch;

    bool owned() const { return state == VrWorldState::Owned; }
    // The route has work for the per-draw hooks: the key is on, the layer is live and the gate holds (and the latch has
    // not turned the route off). Decided from the boundary's facts, so a draw never asks the gate itself.
    bool wantsDraws() const { return state == VrWorldState::Observing || state == VrWorldState::Warming || state == VrWorldState::Owned; }

    // One frame boundary. The state it leaves is what the NEXT frame starts in: the eye shift (native_temporal begin)
    // and the layer read it from there.
    VrWorldStep frameEnd(const VrWorldFrameEnd& f) {
        VrWorldStep step;
        const VrWorldState before = state;
        const auto release = [&](VrWorldRelease why) { if (before == VrWorldState::Owned) step.released = why; };
        if (!f.keyOn) {
            release(VrWorldRelease::KeyOff);
            state = VrWorldState::Off; run = miss = 0; latch.reset();   // flipping the key is what un-latches
            return step;
        }
        if (before == VrWorldState::Latched) return step;   // stays off until the key is flipped
        if (before == VrWorldState::Off) { state = VrWorldState::Observing; run = miss = 0; }   // the key just came on
        if (!f.layerLive) { release(VrWorldRelease::LayerNotLive); state = VrWorldState::Observing; run = miss = 0; return step; }
        if (!f.gate) { release(VrWorldRelease::GateLost); state = VrWorldState::Observing; run = miss = 0; return step; }
        if (f.sceneReset) { release(VrWorldRelease::SceneReset); state = VrWorldState::Observing; run = miss = 0; return step; }
        if (f.treated) {
            miss = 0; ++run;
            if (latch.treatedFrame(f.lateWrites)) { release(VrWorldRelease::Latched); state = VrWorldState::Latched; run = 0; return step; }
            if (before != VrWorldState::Owned && run >= kVrWorldWarmFrames) { state = VrWorldState::Owned; step.entered = true; }
            else if (before != VrWorldState::Owned) state = VrWorldState::Warming;
            return step;
        }
        run = 0; ++miss;
        if (before == VrWorldState::Owned && miss < kVrWorldGraceFrames) return step;   // one refusal is not a release
        release(VrWorldRelease::Declined);
        state = VrWorldState::Observing;
        return step;
    }
};

// ---- what the state means for the rest of the frame -----------------------------------------------------------
// The eye shift (native_temporal begin): advertised to the game for each eye, every frame, today. An owned route
// has no eye pass to feed, and an eye shift with no pass to resolve it would be a shimmer; so it is off from the frame
// after the route became owned until it is released. The state is the one the last boundary left (begin runs before
// the frame's draws), so an entry is one frame late and a release is one frame late, and each costs nothing worse
// than one frame of the eye route with or without its jitter.
inline bool vrWorldSuppressesEyeShift(VrWorldState stateAtBegin) { return stateAtBegin == VrWorldState::Owned; }

// The layer takes the screen draw (the world-screen composite) only for a frame the route treated and owns.
inline bool vrWorldLayerMayTake(VrWorldState state, bool treatedThisFrame) {
    return state == VrWorldState::Owned && treatedThisFrame;
}

// The door runs layer-only for an eye whose screen draw the layer took in THIS sequence: no eye upscaler, no motion
// prep, no UI resolve. Sequence 0 is "never": a zero-initialised tag cannot match the first frame.
inline bool vrWorldDoorLayerOnly(uint64_t takenSequence, uint64_t sequence) {
    return sequence != 0 && takenSequence == sequence;
}

// ---- the selector ---------------------------------------------------------------------------------------------
// The HDR route's selector (flat_hdr_route.h) reads the flat runtime's prefix model of every draw of the frame. The
// VR path has no such model and cannot afford one on 21.8k draws a frame; what it has is the source screen motion
// names from the first non-weapon pool draw into the screen-sized depth, and the engine-record views keyed by it. So
// the VR selection is a handful of facts the runtime gathers at the trigger, and the reasons a frame is refused.
struct VrWorldSelectFacts {
    bool triggered = false;       // the detector found the tone: H, its consumer and the extent rules
    bool ambiguous = false;       // more than one H candidate matched the consumer
    bool depthKnown = false;      // every draw into H used one depth target, and it is known
    bool depthNamed = false;      // that depth is the source screen motion named THIS frame
    bool engineViews = false;     // the engine-record views for it answered (slots, pool, scene constants now and before)
    bool cameraRows = false;      // the source camera's CPU rows for this frame are known
    bool extentOk = false;        // H is the screen's size on both axes (R = D: the route resolves at E = R)
    bool engineKnown = false;     // fix.temporal_aa names an engine the resolver runs
};
enum class VrWorldSelect : uint8_t {
    Selected, NoTrigger, Ambiguous, NoEngine, ExtentMismatch, DepthMixed, DepthNotNamed, NoEngineViews, NoCameraRows,
};
inline VrWorldSelect vrWorldSelect(const VrWorldSelectFacts& f) {
    if (!f.triggered) return VrWorldSelect::NoTrigger;
    if (f.ambiguous) return VrWorldSelect::Ambiguous;
    if (!f.engineKnown) return VrWorldSelect::NoEngine;
    if (!f.extentOk) return VrWorldSelect::ExtentMismatch;
    if (!f.depthKnown) return VrWorldSelect::DepthMixed;
    if (!f.depthNamed) return VrWorldSelect::DepthNotNamed;
    if (!f.engineViews) return VrWorldSelect::NoEngineViews;
    if (!f.cameraRows) return VrWorldSelect::NoCameraRows;
    return VrWorldSelect::Selected;
}
inline const char* vrWorldSelectName(VrWorldSelect s) {
    switch (s) {
        case VrWorldSelect::Selected: return "selected";
        case VrWorldSelect::NoTrigger: return "no-trigger";
        case VrWorldSelect::Ambiguous: return "ambiguous-hdr";
        case VrWorldSelect::NoEngine: return "no-temporal-engine";
        case VrWorldSelect::ExtentMismatch: return "hdr-not-screen-sized";
        case VrWorldSelect::DepthMixed: return "hdr-depth-not-single";
        case VrWorldSelect::DepthNotNamed: return "depth-not-screen-motion-source";
        case VrWorldSelect::NoEngineViews: return "engine-views-unavailable";
        case VrWorldSelect::NoCameraRows: return "camera-rows-unavailable";
    }
    return "?";
}

// ---- the detector's glue ------------------------------------------------------------------------------------------
// A view as the draw hook resolved it from the binding shadow, once a frame per distinct view pointer: what the detector
// needs of a render target or a depth target and nothing else.
struct VrWorldView {
    const void* resource = nullptr;   // the resource behind the view: an identity, compared and never dereferenced
    uint32_t width = 0, height = 0, format = 0;
    bool known = false;               // the shadow could resolve the view
    bool texture2d = false;
};
// One coloured draw's observation for flatHdrObserveDraw, from the two views the draw hook read. The caller has tested the
// render target slot for null (a draw with no colour target is neither a candidate, a consumer nor a write into H), so
// `color` is the slot's view. A draw whose colour target cannot be read is ignored (false). A DEPTH target that cannot be
// read still counts as a bound depth target (dsv set, depth resource null): rule (i) of the trigger is "no depth bound",
// and an unreadable one is not "none". Only the fields the detector reads are written; the rest of the observation keeps
// whatever it held, because nothing in the route reads them (the observation is one static instance, not ~360 bytes of
// zeroing per draw).
inline bool vrWorldFillObservation(FlatContractObservation& k, const VrWorldView* color, const VrWorldView* depth,
                                   const void* rtv, const void* dsv, uint64_t vs, uint64_t ps) {
    if (!color || !color->known || !color->texture2d || !color->resource) return false;
    k.color = color->resource; k.rtv = rtv;
    k.width = color->width; k.height = color->height; k.format = color->format;
    if (depth && depth->known && depth->texture2d && depth->resource) {
        k.depth = depth->resource; k.dsv = dsv;
        k.depthWidth = depth->width; k.depthHeight = depth->height;
    } else {
        k.depth = nullptr; k.dsv = dsv;
        k.depthWidth = k.depthHeight = 0;
    }
    k.vs = vs; k.ps = ps;
    return true;
}

// ---- the census -----------------------------------------------------------------------------------------------
// One 5 s window, reset when it prints. The HDR route's window is the detector's half; the rest is the route's.
// The camera injector's per-frame counters summed over one 5 s window (stage 2; flat_camera_inject.h FlatCameraVrCounters),
// and the evidence the world's rows carried the phase the injector was given. Printed on its own line
// (vrWorldFormatInjectWindow): the route line is near the log's 1200-character limit already.
struct VrWorldInjectWindow {
    uint64_t scene = 0, firstPerson = 0;     // calls injected with the Scene and the FirstPerson role
    uint64_t refused = 0;                    // screen-view calls the detour admitted and could not write
    uint64_t warming = 0;                    // screen-view calls admitted with a zero phase (nothing written)
    uint64_t auxiliary = 0;                  // kind-3 calls excluded by role
    uint64_t afterTrigger = 0;               // screen-view calls after the trigger
    uint64_t unsupported = 0, otherKind = 0; // kinds 4 and 5, and kinds 0, 1, 2 ...
    uint64_t unreadable = 0, offThread = 0, writeFail = 0;
    uint64_t injectedKind[8] = {};           // injected calls by kind (0..5, 6 other, 7 unreadable): only kind 3 may be non-zero
    uint64_t pairChecked = 0, pairBad = 0;   // consecutive resolved frames whose rows differed by the phases they were claimed to carry
    // The naming rule (the window opens only after a frame that named the screen's source): frames whose window was open and that
    // turned out to name nothing (a map, a menu or a transition cannot be told from a world before its cameras refresh, so the
    // first such frame after a named one is expected, at most one per change), and injections on a frame whose window the route
    // had SHUT (a STOP: a shut window is never written through).
    uint64_t unnamedFrames = 0, shutInjected = 0;
    // The narrowest and widest struct field of view (rad) any screen view showed in the window (the injector's fovNarrowest/fovWidest):
    // 0 and 0 when none did. Two values say the struct carries a tighter weapon camera for the role's field-of-view test to find.
    float fovNarrowest = 0.0f, fovWidest = 0.0f;
};
struct VrWorldWindow {
    FlatHdrWindow hdr;            // frames, hdr-frames, trigger, none, ambiguous, treated, declined, late writes, selection tally
    uint64_t gateFrames = 0;      // frames the route watched with the gate held
    uint64_t gateFlips = 0;       // times the on-foot gate changed between two boundaries (either way)
    uint64_t ownedFrames = 0;     // frames that started owned (eye shift off)
    uint64_t takes = 0;           // eye screen draws the layer took (two a frame)
    uint64_t layerOnly = 0;       // eyes whose door ran layer-only
    uint64_t enters = 0, releases = 0, resets = 0;
    const char* lastRelease = "none";
    // Stage 2. The last boundary's jitter decision (vrWorldJitterName), the phase the frame that ended was given and the
    // phase its rows carried (render pixels, positive right/down), and the fold-in's mode counts.
    const char* jitter = "idle";
    float phaseX = 0.0f, phaseY = 0.0f, rowsX = 0.0f, rowsY = 0.0f;
    // How many phases the route's sequence ran through (temporal_math.h): the route resolves at the game's render size, render ==
    // output, so the ratio rule gives the fixed eight whatever experimental.temporal_aa_jitter_follows_upscale says. A default-
    // constructed window says eight, the count every path has always run.
    uint32_t phases = kTemporalJitterCount;
    uint64_t foldMode[3] = {};    // frames whose resolve ran the weapon fold-in with mode 0, 1, 2 (FlatMonoResolveFrame::firstPersonPhaseMode)
    // The stage 2 experiment build: the state of experimental.temporal_aa_on_foot_world_steady_detail at the boundary that printed the
    // line (vrWorldSteadyKeyName: "on" or "off"; the key defaults to on, and off is the route as flight 2 flew it). A window nothing has
    // set (a default-constructed one) says off: the boundary sets it from the key before every line.
    const char* steady = "off";
    // The screen's curve at the boundary that printed the line (vrWorldFormatCurve: "off", "pending", "stood-down" or "C/S/G") and
    // how many strips the route's layer re-issued in the window (panel_curve.h panelCurveReissue). A default-constructed window says
    // off and 0: the boundary sets both before every line.
    char curve[48] = "off";
    uint64_t curveReissues = 0;
    VrWorldInjectWindow inject;
    void reset() { *this = VrWorldWindow{}; }
};
// The screen's curve as the route's lines say it (the curved route, design doc section 82): "off" when the game's own quad is
// drawn (fix.panel_curvature 0 and the default segment count: `configured` false), "stood-down" when the substitution turned
// itself off for the session (a fault, or a panel SIZE that cannot be one), "pending" while it is asked for and has not drawn its
// strip yet (it learns the panel's SIZE for a few frames after the first composite and draws the game's flat quad meanwhile, and
// the layer re-issues that flat quad to match), else "C/S/G": the curvature (a fraction of a full circle), the strip's columns and
// the depth gain in the panel's model units -- the numbers the strip in hand was built from, which the layer's re-issue draws too.
inline int vrWorldFormatCurve(char* out, size_t size, bool configured, bool standDown, bool ready, float curvature, int segments,
                              float gain) {
    if (!configured) return std::snprintf(out, size, "off");
    if (standDown) return std::snprintf(out, size, "stood-down");
    if (!ready) return std::snprintf(out, size, "pending");
    return std::snprintf(out, size, "%.3f/%d/%.3f", static_cast<double>(curvature), segments, static_cast<double>(gain));
}
// The 5 s line, printed every window while the key is auto, zeros included: an absent line is what "the route never
// ran" looks like, and the stop signals in the flight plan read it.
inline int vrWorldFormatWindow(char* out, size_t size, VrWorldKey key, VrWorldState state, bool layerLive, bool gate,
                               const VrWorldWindow& w) {
    int n = std::snprintf(out, size,
        "vr world route 5s: key=%s state=%s layer=%s gate=%s frames=%llu gate-frames=%llu gate-flips=%llu hdr-frames=%llu trigger=%llu "
        "none=%llu ambiguous=%llu treated=%llu declined=%llu owned-frames=%llu eye-takes=%llu door-layer-only=%llu "
        "enters=%llu releases=%llu (last=%s) scene-resets=%llu late-hdr-writes=%llu (in %llu frames) last=%s "
        "phases=%u jitter=%s phase=%.4f,%.4f rows=%.4f,%.4f fp-mode=%llu/%llu/%llu steady-detail=%s curve=%s curve-reissues=%llu "
        "last-trigger=VS=%016llX PS=%016llX target=%ux%u hdr=%ux%u selection=",
        vrWorldKeyName(key), vrWorldStateName(state), layerLive ? "live" : "not-live", gate ? "held" : "no",
        static_cast<unsigned long long>(w.hdr.frames), static_cast<unsigned long long>(w.gateFrames),
        static_cast<unsigned long long>(w.gateFlips),
        static_cast<unsigned long long>(w.hdr.hdrFrames), static_cast<unsigned long long>(w.hdr.triggerFrames),
        static_cast<unsigned long long>(w.hdr.noTriggerFrames), static_cast<unsigned long long>(w.hdr.ambiguousFrames),
        static_cast<unsigned long long>(w.hdr.treated), static_cast<unsigned long long>(w.hdr.declined),
        static_cast<unsigned long long>(w.ownedFrames), static_cast<unsigned long long>(w.takes),
        static_cast<unsigned long long>(w.layerOnly), static_cast<unsigned long long>(w.enters),
        static_cast<unsigned long long>(w.releases), w.lastRelease, static_cast<unsigned long long>(w.resets),
        static_cast<unsigned long long>(w.hdr.lateWrites), static_cast<unsigned long long>(w.hdr.lateWriteFrames),
        w.hdr.lastVerdict, w.phases, w.jitter, static_cast<double>(w.phaseX), static_cast<double>(w.phaseY),
        static_cast<double>(w.rowsX), static_cast<double>(w.rowsY), static_cast<unsigned long long>(w.foldMode[0]),
        static_cast<unsigned long long>(w.foldMode[1]), static_cast<unsigned long long>(w.foldMode[2]), w.steady,
        w.curve, static_cast<unsigned long long>(w.curveReissues),
        static_cast<unsigned long long>(w.hdr.lastTriggerVs),
        static_cast<unsigned long long>(w.hdr.lastTriggerPs), w.hdr.lastTargetWidth, w.hdr.lastTargetHeight,
        w.hdr.lastHdrWidth, w.hdr.lastHdrHeight);
    const auto append = [&](const char* first, const char* name, unsigned long long count) {
        if (n < 0 || static_cast<size_t>(n) >= size) return;
        const int more = std::snprintf(out + n, size - static_cast<size_t>(n), "%s%s:%llu", first, name, count);
        if (more > 0) n += more;
    };
    bool any = false;
    for (const auto& s : w.hdr.selections) {
        if (!s.name) break;
        append(any ? "," : "", s.name, static_cast<unsigned long long>(s.count));
        any = true;
    }
    if (w.hdr.selectionOther) { append(any ? "," : "", "other", static_cast<unsigned long long>(w.hdr.selectionOther)); any = true; }
    if (!any && n >= 0 && static_cast<size_t>(n) < size) { std::snprintf(out + n, size - static_cast<size_t>(n), "none"); n += 4; }
    return n;
}
// The route took the world: the first frame its layer took a screen draw after being warm.
// `curve` is vrWorldFormatCurve's text at that boundary, and the line adds ONE sentence that says what is true of the screen in that state
// (each keeps "(curve=<text>)", the part a reader keys on):
//   off, or null    nothing: a flat screen's line is what it always was;
//   "C/S/G"         a strip is in hand: the layer draws the very strip the game's own draw is substituted with;
//   "pending"       asked for and no strip built yet: the game and the layer both draw the flat quad until it is;
//   "stood-down"    the substitution turned itself off for the session: the game draws its own flat quad and the layer re-issues it flat.
constexpr char kVrWorldEnteredStripHead[] = "; the screen is curved (curve=";
constexpr char kVrWorldEnteredStripTail[] =
    "): the layer draws the same strip the game's own draw is substituted with, so the bend and the placement are the game's";
constexpr char kVrWorldEnteredPending[] =
    "; the screen is set to curve (curve=pending): the strip is not built yet, so the game and the layer both draw the flat quad until it is";
constexpr char kVrWorldEnteredStoodDown[] =
    "; the screen is set to curve but the curve stood down (curve=stood-down): the game draws its own flat quad and the layer re-issues it flat";
constexpr size_t kVrWorldEnteredTailBytes = 224;
// The longest sentence of each state fits the suffix buffer (the strip's with the widest curve text a window holds): a longer one fails the
// build, not the log line.
static_assert(sizeof(kVrWorldEnteredStripHead) + sizeof(kVrWorldEnteredStripTail) - 1 + (sizeof(VrWorldWindow::curve) - 1) <= kVrWorldEnteredTailBytes,
              "the OWNS line's strip sentence (with the widest curve text) must fit the suffix buffer");
static_assert(sizeof(kVrWorldEnteredPending) <= kVrWorldEnteredTailBytes, "the OWNS line's pending sentence must fit the suffix buffer");
static_assert(sizeof(kVrWorldEnteredStoodDown) <= kVrWorldEnteredTailBytes, "the OWNS line's stood-down sentence must fit the suffix buffer");
inline int vrWorldFormatEntered(char* out, size_t size, uint64_t frame, uint32_t warmFrames, const char* curve = nullptr) {
    char tail[kVrWorldEnteredTailBytes] = "";
    if (curve && std::strcmp(curve, "off") != 0) {
        if (std::strcmp(curve, "pending") == 0) std::snprintf(tail, sizeof(tail), "%s", kVrWorldEnteredPending);
        else if (std::strcmp(curve, "stood-down") == 0) std::snprintf(tail, sizeof(tail), "%s", kVrWorldEnteredStoodDown);
        else std::snprintf(tail, sizeof(tail), "%s%s%s", kVrWorldEnteredStripHead, curve, kVrWorldEnteredStripTail);
    }
    return std::snprintf(out, size,
        "vr world route: OWNS the world from frame=%llu after %u treated frames in a row; the eye shift is off and the "
        "layer takes the screen draw on every frame the route treats (the eye route serves the rest)%s",
        static_cast<unsigned long long>(frame), warmFrames, tail);
}
// The route let go: why, and what it had done.
// lastDecline: the selector's reason for the frames the route declined just before it let go (null or empty when it
// declined none: a gate lost, a key turned off), declineRun how many frames in a row.
inline int vrWorldFormatReleased(char* out, size_t size, uint64_t frame, VrWorldRelease why, uint64_t ownedFrames,
                                 const char* lastDecline = nullptr, uint32_t declineRun = 0) {
    char tail[160] = "";
    if (lastDecline && lastDecline[0] && declineRun)
        std::snprintf(tail, sizeof(tail), "; last decline: %s x%u", lastDecline, declineRun);
    return std::snprintf(out, size,
        "vr world route: RELEASED the world at frame=%llu (%s%s) after %llu owned frame(s); the eye shift is back on and "
        "the eye route serves the eyes",
        static_cast<unsigned long long>(frame), vrWorldReleaseName(why), tail, static_cast<unsigned long long>(ownedFrames));
}
// The first trigger of a session, once.
inline int vrWorldFormatFirstTrigger(char* out, size_t size, uint64_t frame, const FlatHdrFrame& f) {
    const auto& t = f.trigger;
    const FlatHdrCandidate* h = flatHdrFindCandidate(f, t.hdr);
    return std::snprintf(out, size,
        "vr world route: first trigger at frame=%llu seq=%u: VS=%016llX PS=%016llX target=%ux%u fmt=%u reads the scene "
        "HDR %ux%u at t%d; its %u draw(s) ran seq %u..%u%s",
        static_cast<unsigned long long>(frame), t.sequence, static_cast<unsigned long long>(t.vs),
        static_cast<unsigned long long>(t.ps), t.targetWidth, t.targetHeight, t.targetFormat, t.hdrWidth, t.hdrHeight,
        t.srvKnown ? static_cast<int>(t.srvSlot) : -1, h ? h->draws : 0u, h ? h->firstSeq : 0u, h ? h->lastSeq : 0u,
        t.ambiguous ? "; MORE THAN ONE HDR candidate matched, so the route declines" : "");
}

// ---- stage 2: the world jitter (design doc section 82, stage 2) ---------------------------------------------------------
// experimental.temporal_aa_on_foot_world_jitter: on (the default) or off. While the route owns the world, "on" puts the
// sub-pixel phase into the world's cameras through the camera injector (flat_camera_inject.h), and the resolver gets that
// phase for its jitter input and for the rows; "off" keeps the route and zeroes the phase, which is flight 1's unjittered
// world, so one session can compare the two at the same spot. A value that is present and is not "on" reads as off: a typo
// can never start writing the game's cameras. (experimental.temporal_aa_jitter, the global jitter key, off also means off.)
enum class VrWorldJitterKey : uint8_t { Off, On };
inline VrWorldJitterKey vrWorldJitterKeyFromText(const char* text) {
    if (!text) return VrWorldJitterKey::Off;
    const char* on = "on";
    for (; *on; ++on, ++text) {
        char c = *text;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != *on) return VrWorldJitterKey::Off;
    }
    return *text == 0 ? VrWorldJitterKey::On : VrWorldJitterKey::Off;
}
inline const char* vrWorldJitterKeyName(VrWorldJitterKey key) { return key == VrWorldJitterKey::On ? "on" : "off"; }

// What the route decides about the world phase for the frame that starts. Only On asks the injector to write a camera.
enum class VrWorldJitter : uint8_t {
    On,         // inject kind-3 screen-view calls before the trigger with this frame's phase
    KeyOff,     // experimental.temporal_aa_on_foot_world_jitter is off: the route runs unjittered (nothing is installed for it)
    GlobalOff,  // experimental.temporal_aa_jitter is off
    Idle,       // the route is not Warming or Owned (or its key is off): nothing to jitter
    Unnamed,    // the route is Warming or Owned but the last frame named no source for the screen (a map, a menu, a transition):
                // the window stays shut, through the route's grace frames, until a frame names one again
    NoHook,     // wanted, but the camera hook is not live (not installed, failed, stood down): the world stays unjittered
    Fault,      // the injector was switched off for the session after a STOP line (an injected kind other than 3)
};
// namedLast: the frame that just ended named the screen's source (the engine's pool-family draw into the screen-sized depth,
// the selector's depthNamed fact). The frame's kind-3 refreshes come before the draws that name the source (the scene camera is
// refreshed before the pass that draws with it), so the frame that starts cannot be asked;
// a map frame refreshes about thirty kind-3 cameras and must never pick up the world's phase, so the window opens only after
// a frame that named its source and stays shut after one that did not (design-world-camera-motion-2026-09-30.md section 5).
inline VrWorldJitter vrWorldJitterDecide(bool routeKeyAuto, VrWorldJitterKey jitterKey, bool globalJitter,
                                         bool routeWantsInjection, bool namedLast, bool hookLive, bool fault) {
    if (!routeKeyAuto) return VrWorldJitter::Idle;
    if (fault) return VrWorldJitter::Fault;
    if (jitterKey != VrWorldJitterKey::On) return VrWorldJitter::KeyOff;
    if (!globalJitter) return VrWorldJitter::GlobalOff;
    if (!routeWantsInjection) return VrWorldJitter::Idle;
    if (!namedLast) return VrWorldJitter::Unnamed;
    if (!hookLive) return VrWorldJitter::NoHook;
    return VrWorldJitter::On;
}
// The token of the 5 s line (jitter=): on, off (either key), idle, unnamed, no-hook, fault.
inline const char* vrWorldJitterName(VrWorldJitter j) {
    switch (j) {
        case VrWorldJitter::On: return "on";
        case VrWorldJitter::KeyOff:
        case VrWorldJitter::GlobalOff: return "off";
        case VrWorldJitter::Idle: return "idle";
        case VrWorldJitter::Unnamed: return "unnamed";
        case VrWorldJitter::NoHook: return "no-hook";
        case VrWorldJitter::Fault: return "fault";
    }
    return "?";
}

// A phase as the frame APPLIED it to one group of cameras (render pixels, positive right/down). Equality is exact: both
// sides of every comparison come from the same float the route handed the injector.
struct VrWorldAppliedPhase {
    float x = 0.0f, y = 0.0f;
    bool zero() const { return x == 0.0f && y == 0.0f; }
    bool operator==(const VrWorldAppliedPhase& o) const { return x == o.x && y == o.y; }
};
// The weapon fold-in's mode for the resolver (FlatMonoResolveFrame::firstPersonPhaseMode): the map's vector is previous
// minus current at the two frames' OWN raster phases, so the fold-in needs to know whether the first-person camera carried
// the world's phase in BOTH frames.
//   0  no phase anywhere in either frame (the world is unjittered): the map is used as it is given;
//   1  the first-person camera carried exactly the world's phase in both frames: the map's vector gets the phase term;
//   2  it did not (a first-person call was not injected in one of the frames, or was refused): attached pixels reject.
// fpNow / fpPrev are what the first-person camera carried this frame and last frame: the phase when its calls were all
// injected, zero when one was excluded or refused. A frame with no first-person call at all (no weapon drawn) has no
// attached pixels, so the caller passes the world's phase for it (vacuously the same).
inline uint32_t vrWorldFirstPersonMode(const VrWorldAppliedPhase& worldNow, const VrWorldAppliedPhase& worldPrev,
                                       const VrWorldAppliedPhase& fpNow, const VrWorldAppliedPhase& fpPrev) {
    if (worldNow.zero() && worldPrev.zero() && fpNow.zero() && fpPrev.zero()) return 0;
    return (fpNow == worldNow && fpPrev == worldPrev) ? 1u : 2u;
}

// The injector's per-frame counters as the route sees them (the fields of FlatCameraVrCounters it reads; a plain struct so
// the pure rig and the formatters need no injector header).
struct VrWorldInjectFrame {
    uint32_t sceneInjected = 0, sceneRefused = 0, firstPersonInjected = 0, firstPersonRefused = 0;
    uint32_t warming = 0, auxiliary = 0, afterTrigger = 0, unsupported = 0, otherKind = 0, unreadable = 0;
    uint32_t writeFailures = 0, offThread = 0;
    uint32_t injectedKind[8] = {};
    float fovNarrowest = std::numeric_limits<float>::quiet_NaN(), fovWidest = std::numeric_limits<float>::quiet_NaN();   // NaN: no frustum read
};
inline void vrWorldAddInjectFrame(VrWorldInjectWindow& w, const VrWorldInjectFrame& f) {
    if (f.fovNarrowest > 0.0f && f.fovWidest > 0.0f) {   // a NaN is neither
        if (w.fovWidest == 0.0f || f.fovNarrowest < w.fovNarrowest) w.fovNarrowest = f.fovNarrowest;
        if (f.fovWidest > w.fovWidest) w.fovWidest = f.fovWidest;
    }
    w.scene += f.sceneInjected; w.firstPerson += f.firstPersonInjected;
    w.refused += static_cast<uint64_t>(f.sceneRefused) + f.firstPersonRefused;
    w.warming += f.warming; w.auxiliary += f.auxiliary; w.afterTrigger += f.afterTrigger;
    w.unsupported += f.unsupported; w.otherKind += f.otherKind; w.unreadable += f.unreadable;
    w.offThread += f.offThread; w.writeFail += f.writeFailures;
    for (int i = 0; i < 8; ++i) w.injectedKind[i] += f.injectedKind[i];
}
// Any injected call of a kind other than 3: the one thing the admission table must make impossible.
inline bool vrWorldInjectedWrongKind(const VrWorldInjectFrame& f) {
    for (int i = 0; i < 8; ++i) if (i != 3 && f.injectedKind[i]) return true;
    return false;
}

// The second line of a 5 s window (the route line is near the log's 1200-character limit): the injector's counters and the
// rows-pair evidence. inj-kinds lists the INJECTED calls by kind: "3:<n>" for kind 3, "other:<n>" for everything else, or
// "none". The reader (edvr_log.py --camera-census) parses this text.
inline int vrWorldFormatInjectWindow(char* out, size_t size, const VrWorldInjectWindow& w) {
    char kinds[96] = "none";
    uint64_t other = 0;
    for (int i = 0; i < 8; ++i) if (i != 3) other += w.injectedKind[i];
    if (w.injectedKind[3] || other) {
        int n = 0;
        if (w.injectedKind[3]) n = std::snprintf(kinds, sizeof(kinds), "3:%llu", static_cast<unsigned long long>(w.injectedKind[3]));
        else kinds[0] = 0;
        if (other) std::snprintf(kinds + n, sizeof(kinds) - static_cast<size_t>(n), "%sother:%llu", n ? "," : "",
                                 static_cast<unsigned long long>(other));
    }
    char fov[40] = "-";   // "fov=-": no screen view's frustum was read in the window
    if (w.fovWidest > 0.0f && w.fovNarrowest > 0.0f)
        std::snprintf(fov, sizeof(fov), "%.4f..%.4f", static_cast<double>(w.fovNarrowest), static_cast<double>(w.fovWidest));
    return std::snprintf(out, size,
        "vr world route inject 5s: inj-scene=%llu inj-fp=%llu inj-refused=%llu warming=%llu aux=%llu after=%llu "
        "unsupported=%llu other-kind=%llu unreadable=%llu off-thread=%llu write-fail=%llu inj-kinds=%s "
        "pair-checked=%llu pair-bad=%llu inj-unnamed=%llu inj-shut=%llu fov=%s",
        static_cast<unsigned long long>(w.scene), static_cast<unsigned long long>(w.firstPerson),
        static_cast<unsigned long long>(w.refused), static_cast<unsigned long long>(w.warming),
        static_cast<unsigned long long>(w.auxiliary), static_cast<unsigned long long>(w.afterTrigger),
        static_cast<unsigned long long>(w.unsupported), static_cast<unsigned long long>(w.otherKind),
        static_cast<unsigned long long>(w.unreadable), static_cast<unsigned long long>(w.offThread),
        static_cast<unsigned long long>(w.writeFail), kinds, static_cast<unsigned long long>(w.pairChecked),
        static_cast<unsigned long long>(w.pairBad), static_cast<unsigned long long>(w.unnamedFrames),
        static_cast<unsigned long long>(w.shutInjected), fov);
}
// The first frame the world carries a non-zero phase in an ownership episode, once per episode.
inline int vrWorldFormatJitterLive(char* out, size_t size, uint64_t frame, float phaseX, float phaseY, uint32_t renderW,
                                   uint32_t renderH, const VrWorldInjectFrame& f) {
    return std::snprintf(out, size,
        "vr world route: the world is JITTERED from frame=%llu: phase (%.4f,%.4f) px in %ux%u, %u scene and %u first-person "
        "camera call(s) injected this frame (kind 3 only; the eye cameras, kind 5, are never written); the eye shift stays off "
        "while the route owns the world",
        static_cast<unsigned long long>(frame), static_cast<double>(phaseX), static_cast<double>(phaseY), renderW, renderH,
        f.sceneInjected, f.firstPersonInjected);
}
// A kind-3 call the role test excluded, once per distinct signature (the first eight): the roles are never guessed.
inline int vrWorldFormatExcluded(char* out, size_t size, float aspect, float fov, float nearZ, float farZ,
                                 unsigned long long callerRva, float screenAspect, unsigned long long calls) {
    return std::snprintf(out, size,
        "vr world route: camera call EXCLUDED, not a screen view: kind 3 aspect=%.4f fov=%.4f near=%.4f far=%.1f caller=+0x%llX "
        "(the screen's aspect is %.4f; a screen view is within 4%%): %llu call(s) so far, never injected",
        static_cast<double>(aspect), static_cast<double>(fov), static_cast<double>(nearZ), static_cast<double>(farZ),
        callerRva, static_cast<double>(screenAspect), calls);
}
// The route asked for jitter and no scene camera call was injected: the world is unjittered this frame, and says why.
inline int vrWorldFormatNoSceneInjection(char* out, size_t size, uint64_t frame, const VrWorldInjectFrame& f) {
    return std::snprintf(out, size,
        "vr world route: jitter is wanted but no scene camera call was injected at frame=%llu (warming %u, auxiliary %u, after "
        "the trigger %u, refused %u, unsupported %u, other kinds %u, unreadable %u): the frame resolves unjittered",
        static_cast<unsigned long long>(frame), f.warming, f.auxiliary, f.afterTrigger, f.sceneRefused + f.firstPersonRefused,
        f.unsupported, f.otherKind, f.unreadable);
}
// STOP: an injected kind other than 3. The route switches the injector off for the session (the key flipping off and on
// again clears it).
inline int vrWorldFormatStopWrongKind(char* out, size_t size, uint64_t frame, const VrWorldInjectFrame& f) {
    return std::snprintf(out, size,
        "vr world route: STOP at frame=%llu: camera calls of a kind other than 3 were INJECTED (kind 0: %u, 1: %u, 2: %u, 4: %u, "
        "5: %u, other: %u, unreadable: %u): only kind 3 may be; the injector is switched off for this session (set "
        "experimental.temporal_aa_on_foot_world off and auto again to clear it)",
        static_cast<unsigned long long>(frame), f.injectedKind[0], f.injectedKind[1], f.injectedKind[2], f.injectedKind[4],
        f.injectedKind[5], f.injectedKind[6], f.injectedKind[7]);
}
// A frame whose window was open named no source for the screen: the first frame of a map, a menu or a transition, which cannot
// be told from a world frame before its cameras refresh (the naming draw follows the first scene refresh). Expected, once per change; the window is
// shut from the next frame on, until a frame names one again, and the map or menu is left as it was.
inline int vrWorldFormatUnnamedOpen(char* out, size_t size, uint64_t frame, const VrWorldInjectFrame& f) {
    return std::snprintf(out, size,
        "vr world route: frame=%llu named no source for the screen but its camera window was open (it follows a frame that did: a "
        "map, a menu or a transition cannot be told before its cameras refresh): %u scene and %u first-person call(s) carried "
        "the phase this once; the window stays shut until a frame names its source again",
        static_cast<unsigned long long>(frame), f.sceneInjected, f.firstPersonInjected);
}
// STOP: camera calls were injected on a frame whose window the route had shut (an unnamed frame before it, a route that was not
// Warming or Owned, a jitter key off). The route switches the injector off for the session.
inline int vrWorldFormatStopShut(char* out, size_t size, uint64_t frame, const char* decision, const VrWorldInjectFrame& f) {
    return std::snprintf(out, size,
        "vr world route: STOP at frame=%llu: %u scene and %u first-person camera call(s) were INJECTED on a frame whose window the "
        "route had shut (decision: %s); a shut window is never written through, and the map or menu would pick up the world's "
        "phase; the injector is switched off for this session (set experimental.temporal_aa_on_foot_world off and auto again "
        "to clear it)",
        static_cast<unsigned long long>(frame), f.sceneInjected, f.firstPersonInjected, decision ? decision : "?");
}
// The rows of two consecutive resolved frames did not differ by the phases they were claimed to carry (flat_camera_phase.h
// flatCameraCheckRowPair): the injection missed the camera the scene reads, or the game jitters too. Counted; the first few named.
inline int vrWorldFormatRowsMismatch(char* out, size_t size, uint64_t frame, float rx, float ry, float prx, float pry,
                                     float maxError) {
    return std::snprintf(out, size,
        "vr world route: camera rows disagree with the phase at frame=%llu: claimed now (%.4f,%.4f) previous (%.4f,%.4f) px, the "
        "rows' measured difference is off by %.3g NDC (tolerance 2e-6); the frame is still resolved, this is the evidence",
        static_cast<unsigned long long>(frame), static_cast<double>(rx), static_cast<double>(ry), static_cast<double>(prx),
        static_cast<double>(pry), static_cast<double>(maxError));
}

// ---- the stage 2 experiment build (design doc section 82) ------------------------------------------------------------------
// experimental.temporal_aa_on_foot_world_steady_detail: on (the default since flight 4) or off. Flight 2's shimmer on fine patterns is the prep
// refusing the history of a pixel whose engine slot a LATER draw overdrew (the slot's depth is no longer the pixel's) and the finish
// then showing the raw jittered input there (flight 3 confirmed it at the main menu: 5.674% of the pixels, all stale, calm with the key
// on). "on" gives those pixels the camera term instead, but only where last frame's depth confirms it (the depth at the position the
// camera term sends the pixel to matches the depth this surface would have had there had it not moved, within 1%: the depth-validated
// steady detail; the blanket form was flight 3's experiment); where it does not the pixel is refused as before. Only the stale-slot
// refusal is relaxed: a masked record, a corrupt slot, the sky and the weapon's pixels stay refused. A lateral mover's interior can
// still pass the depth test and ghost while it is on (flight 4: none seen). Read only while the route key is auto. A file with no line
// reads on (the reader's fallback); a value that is present and is not "on" reads as off, a typo included, which is the refusal exactly
// as before the key existed. The in-headset menu's developer mode flips it live, like the jitter key.
enum class VrWorldSteadyKey : uint8_t { Off, On };
inline VrWorldSteadyKey vrWorldSteadyKeyFromText(const char* text) {
    if (!text) return VrWorldSteadyKey::Off;
    const char* on = "on";
    for (; *on; ++on, ++text) {
        char c = *text;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != *on) return VrWorldSteadyKey::Off;
    }
    return *text == 0 ? VrWorldSteadyKey::On : VrWorldSteadyKey::Off;
}
inline const char* vrWorldSteadyKeyName(VrWorldSteadyKey key) { return key == VrWorldSteadyKey::On ? "on" : "off"; }
// The key's state at the boundary that first read it with the route on, and each change after: one line, saying what the state does.
inline int vrWorldFormatSteadyChanged(char* out, size_t size, uint64_t frame, VrWorldSteadyKey key) {
    if (key == VrWorldSteadyKey::On)
        return std::snprintf(out, size,
            "vr world route: steady-detail is ON from frame=%llu (experimental.temporal_aa_on_foot_world_steady_detail): a pixel whose "
            "engine slot a later draw overdrew takes the camera term instead of refusing its history where last frame's depth confirms "
            "it, and is refused where it does not; masked records and corrupt slots stay refused; a lateral mover's interior can still "
            "pass the depth test and ghost",
            static_cast<unsigned long long>(frame));
    return std::snprintf(out, size,
        "vr world route: steady-detail is OFF from frame=%llu (experimental.temporal_aa_on_foot_world_steady_detail): a pixel whose "
        "engine slot a later draw overdrew refuses its history, as in flight 2",
        static_cast<unsigned long long>(frame));
}
// The refusal view (advanced.temporal_aa_debug = motion_source while the route resolves): said when it comes on and when it goes off.
inline int vrWorldFormatViewChanged(char* out, size_t size, uint64_t frame, bool on) {
    if (on)
        return std::snprintf(out, size,
            "vr world route: the refusal view is ON from frame=%llu (advanced.temporal_aa_debug = motion_source): the world's picture is "
            "painted by the prep's own classification before the game's tone pass (%s); the hue survives the tone pass, the absolute "
            "colour does not",
            static_cast<unsigned long long>(frame), flatMonoViewLegend());
    return std::snprintf(out, size, "vr world route: the refusal view is OFF from frame=%llu", static_cast<unsigned long long>(frame));
}
// The third line of a 5 s window, printed while the refusal census is wanted (advanced.vr_camera_census on), while samples of a census
// that has just gone off are still draining, and while the steady-detail key is on (its depth check's own frames are counted
// whatever the census asks): an absent line is "the census and the key were off", and a line with sampled=0 is "on, and no sample was
// read back" -- the two are never the same text, and a census that ran and found nothing says so with pixels > 0 and refused=0.
// `treated` is the route's own count of treated frames in the window; asked, sampled, dropped and read are the resolver's
// (flat_mono_refusal.h FlatMonoRefusalCensus). The stale pixels are two numbers: `stale-refused` (refused: with the key off all of
// them, with it on the ones last frame's depth did not confirm) and `stale-kept` (not refused: the camera term, confirmed). `depth-check`
// is ran/skipped: the resolves with the key on whose prep ran the depth check, and those that could not (no last-frame depth). The
// reader (edvr_log.py --camera-census) parses this text; it still reads the flight-3 spellings `stale=` and `forgiven=`.
struct VrWorldRefusalWindow {
    bool census = false;                  // the census key is on at the boundary that printed the line
    uint64_t treated = 0;
    uint64_t asked = 0, sampled = 0, dropped = 0, read = 0;
    uint32_t every = 0;
    uint32_t width = 0, height = 0;       // the render size of the last sample read back
    uint64_t pixels = 0;                  // what the read-back samples examined
    uint64_t counts[kFlatMonoRefusalSlots] = {};
    uint64_t checked = 0, skipped = 0;    // the resolver's depth-check frames this window (FlatMonoRefusalCensus::checked, skipped)
    const char* steady = "off";           // vrWorldSteadyKeyName at the boundary
    const char* view = "off";             // "on" while the refusal view is painting
};
inline uint64_t vrWorldRefusalTotal(const VrWorldRefusalWindow& w) {
    uint64_t n = 0;
    for (uint32_t i = 0; i < kFlatMonoRefusalStaleKept; ++i) n += w.counts[i];
    return n;
}
inline int vrWorldFormatRefusalWindow(char* out, size_t size, const VrWorldRefusalWindow& w) {
    const uint64_t refused = vrWorldRefusalTotal(w);
    const uint64_t named = w.counts[kFlatMonoClassStale] + w.counts[kFlatMonoClassMasked] + w.counts[kFlatMonoClassCorrupt] +
                           w.counts[kFlatMonoClassSentinel] + w.counts[kFlatMonoClassUnreprojectable] + w.counts[kFlatMonoClassCamera] +
                           w.counts[kFlatMonoClassRange] + w.counts[kFlatMonoClassDepth] + w.counts[kFlatMonoClassWeaponRefused];
    const double pct = w.pixels ? 100.0 * static_cast<double>(refused) / static_cast<double>(w.pixels) : 0.0;
    return std::snprintf(out, size,
        "vr world route refusal 5s: census=%s every=%u treated=%llu asked=%llu sampled=%llu read=%llu dropped=%llu size=%ux%u "
        "pixels=%llu refused=%llu refused-pct=%.3f stale-refused=%llu masked=%llu corrupt=%llu sentinel=%llu unreprojectable=%llu camera=%llu "
        "range=%llu depth=%llu weapon=%llu other=%llu stale-kept=%llu depth-check=%llu/%llu steady-detail=%s view=%s",
        w.census ? "on" : "off", w.every, static_cast<unsigned long long>(w.treated), static_cast<unsigned long long>(w.asked),
        static_cast<unsigned long long>(w.sampled), static_cast<unsigned long long>(w.read), static_cast<unsigned long long>(w.dropped),
        w.width, w.height, static_cast<unsigned long long>(w.pixels), static_cast<unsigned long long>(refused), pct,
        static_cast<unsigned long long>(w.counts[kFlatMonoClassStale]), static_cast<unsigned long long>(w.counts[kFlatMonoClassMasked]),
        static_cast<unsigned long long>(w.counts[kFlatMonoClassCorrupt]), static_cast<unsigned long long>(w.counts[kFlatMonoClassSentinel]),
        static_cast<unsigned long long>(w.counts[kFlatMonoClassUnreprojectable]), static_cast<unsigned long long>(w.counts[kFlatMonoClassCamera]),
        static_cast<unsigned long long>(w.counts[kFlatMonoClassRange]), static_cast<unsigned long long>(w.counts[kFlatMonoClassDepth]),
        static_cast<unsigned long long>(w.counts[kFlatMonoClassWeaponRefused]), static_cast<unsigned long long>(refused - named),
        static_cast<unsigned long long>(w.counts[kFlatMonoRefusalStaleKept]), static_cast<unsigned long long>(w.checked),
        static_cast<unsigned long long>(w.skipped), w.steady, w.view);
}

// The decline log. Flight 1 capped it per SESSION (twelve lines) and spent all twelve on the entry and the first seconds of one
// gap, so later declines said nothing. It now caps per RUN of declines (consecutive frames the route did not treat; a treated
// frame ends the run and re-arms the cap) and, as a backstop against a flapping selector, per session. The 5 s line's counters
// and the RELEASED line's "last decline" carry what the cap drops.
constexpr uint32_t kVrWorldDeclineLinesPerRun = 3;
constexpr uint32_t kVrWorldDeclineLinesPerSession = 64;
inline bool vrWorldDeclineLogAllowed(uint32_t runLines, uint32_t sessionLines) {
    return runLines < kVrWorldDeclineLinesPerRun && sessionLines < kVrWorldDeclineLinesPerSession;
}

}  // namespace edvr
