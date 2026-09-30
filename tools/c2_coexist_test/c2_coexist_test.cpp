// C2-C coexistence, ownership and closure tests
// (docs/design-flat-camera-integration.md, C2 test plan addendum, C2-C table,
// revised per the C2-plan review's R5). Exercises the PRODUCTION-INTENDED
// owner-selection policy (src/d3d11/flat_camera_ownership.h) with the C2-A
// derive model as the camera-lineage reference and the PRODUCTION projection
// ownership classifier (src/d3d11/flat_projection_ownership.h) as the
// legacy-evidence check. No abstract XOR of booleans stands in for the
// policy; the game-camera model generates the scenarios.
//
// C7-C12 are the C3 wiring: the frame protocol and the Legacy fallback
// hysteresis, the admission table, the frame window, the flush, the census and
// the text of the new log lines, all through the same header the DLL compiles
// (src/d3d11/flat_camera_phase.h). C13-C15 (2026-09-29, the camera path on with
// no setting): a prologue mismatch and a write-failure stand-down each hand the
// frame to the draw-time path with a history reset, the write-failure limit's
// boundary, and the AA-off invariant -- each with its negative control.
//
// --self-test runs C1-C15 and prints "c2 coexist: PASS" only when every check
// holds. Exit 1 with the failures named otherwise.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../c2_derive_test/c2_derive_model.h"
#include "../../src/d3d11/flat_camera_ownership.h"
#include "../../src/d3d11/flat_camera_phase.h"
#include "../../src/d3d11/flat_projection_ownership.h"

namespace {

using namespace c2derive;
using namespace edvr;

int g_failures = 0;
void check(bool ok, const char* what) {
    if (ok) { std::printf("  ok    %s\n", what); return; }
    ++g_failures;
    std::printf("  FAIL  %s\n", what);
}
bool feq(float a, float b, float tol) {
    const float d = std::fabs(a - b);
    return d <= tol || d <= tol * (std::fabs(a) + std::fabs(b));
}

// A 6-row scene-camera block in the classifier's measured encoding
// (flat_projection_ownership.h:75-80), composed by the derive itself: rows
// 270..273 from composeSceneCb (rotation included), row 274 the repeated
// view direction (column 3 of rows 270..272), row 275 the camera position.
void sceneCameraRows(Cam& c, float out[24]) {
    composeSceneCb(c, out);
    out[16] = out[3]; out[17] = out[7]; out[18] = out[11]; out[19] = 0.0f;
    out[20] = camF(c, kCamOrigin + 0);
    out[21] = camF(c, kCamOrigin + 4);
    out[22] = camF(c, kCamOrigin + 8);
    out[23] = 1.0f;
}

// ---------------------------------------------------------------------------
// C1 single owner before mutation, legacy paths suppressed.
// ---------------------------------------------------------------------------
void testC1() {
    std::printf("C1 single owner\n");
    FlatCameraOwnershipState s{};
    flatCameraOwnerBegin(s);
    FlatCameraGroupInput in;
    in.upstreamCertified = true;
    in.legacyEligible = true;
    const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
    check(d.owner == FlatCameraOwner::Upstream &&
          d.legacyGraphicsSuppressed && d.legacyComputeSuppressed &&
          d.outcome == FlatCameraOutcome::Treated,
          "C1 a both-eligible frame gets upstream as the single owner, legacy graphics+compute suppressed");
    flatCameraOwnerClose(s, d, true, true);
    check(s.lastOwner == FlatCameraOwner::Upstream && s.historyValid,
          "C1 closure records the upstream owner and valid history");
}

// ---------------------------------------------------------------------------
// C2 unknown shaders cannot veto certified lineage.
// ---------------------------------------------------------------------------
void testC2() {
    std::printf("C2 unknown shaders cannot veto certified lineage\n");
    FlatCameraOwnershipState s{};
    flatCameraOwnerBegin(s);
    FlatCameraGroupInput in;
    in.upstreamCertified = true;
    in.legacyEligible = false;   // the legacy selector does not know this pair
    in.legacyObserving = true;   // ...and is parked in observation
    const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
    // Suppression follows the selected owner for every mutation in the
    // group, independently of overall legacy eligibility: individually
    // qualified draws may still exist here and must not mutate.
    check(d.owner == FlatCameraOwner::Upstream &&
          d.outcome == FlatCameraOutcome::Treated &&
          d.legacyGraphicsSuppressed && d.legacyComputeSuppressed,
          "C2 certified lineage reaches treatment without legacy admission hashes");
    flatCameraOwnerClose(s, d, true, true);
    check(s.lastOwner == FlatCameraOwner::Upstream,
          "C2 legacy observation continues without a veto");
}

// ---------------------------------------------------------------------------
// C3 named safe outcomes.
// ---------------------------------------------------------------------------
void testC3() {
    std::printf("C3 named safe outcomes\n");
    {
        FlatCameraOwnershipState s{}; s.historyValid = true;
        FlatCameraGroupInput in;
        in.upstreamCertified = true;
        in.lateProjectionRewrite = true;
        const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        check(d.owner == FlatCameraOwner::None &&
              d.outcome == FlatCameraOutcome::Recovery && d.historyReset &&
              d.preserveCameraInputs && std::strcmp(d.reason, "late-projection-rewrite") == 0,
              "C3 late projection rewrite: named recovery, history reset, inputs preserved");
    }
    {
        FlatCameraOwnershipState s{}; s.historyValid = true;
        FlatCameraGroupInput in;
        in.uncoveredSecondary = true;
        const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        check(d.owner == FlatCameraOwner::None &&
              d.outcome == FlatCameraOutcome::Recovery &&
              std::strcmp(d.reason, "uncovered-secondary-camera") == 0,
              "C3 uncovered secondary camera: named recovery");
    }
    {
        FlatCameraOwnershipState s{};
        FlatCameraGroupInput in;
        in.upstreamUnsupported = true; // kinds 4/5 territory, named
        const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        check(d.owner == FlatCameraOwner::None &&
              d.outcome == FlatCameraOutcome::Unsupported &&
              std::strcmp(d.reason, "upstream-unsupported-projection-kind") == 0,
              "C3 unsupported projection kind: named, not silent");
    }
    {
        FlatCameraOwnershipState s{};
        FlatCameraGroupInput in;
        in.upstreamUnsupported = true;
        in.legacyEligible = true; // the legacy route may still own here
        const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        check(d.owner == FlatCameraOwner::Legacy &&
              d.outcome == FlatCameraOutcome::Treated &&
              std::strcmp(d.reason, "legacy-eligible-upstream-unsupported") == 0,
              "C3 unsupported upstream with an eligible legacy: legacy owns, named");
    }
}

// ---------------------------------------------------------------------------
// C4 interleaved views retain group-level ownership.
// ---------------------------------------------------------------------------
void testC4() {
    std::printf("C4 interleaved views\n");
    // Three groups in one frame: main certified, cockpit legacy-eligible,
    // auxiliary unsupported. Per-group selection must give each its own
    // owner -- a frame-wide OR would have hidden the auxiliary's Unsupported.
    FlatCameraOwnershipState mainS{}, cockpitS{}, auxS{};
    flatCameraOwnerBegin(mainS); flatCameraOwnerBegin(cockpitS); flatCameraOwnerBegin(auxS);
    FlatCameraGroupInput mainIn, cockpitIn, auxIn;
    mainIn.upstreamCertified = true; mainIn.legacyEligible = true;
    cockpitIn.legacyEligible = true;
    auxIn.upstreamUnsupported = true;
    const FlatCameraOwnershipDecision mainD = flatCameraOwnerSelect(mainS, mainIn);
    const FlatCameraOwnershipDecision cockpitD = flatCameraOwnerSelect(cockpitS, cockpitIn);
    const FlatCameraOwnershipDecision auxD = flatCameraOwnerSelect(auxS, auxIn);
    check(mainD.owner == FlatCameraOwner::Upstream &&
          cockpitD.owner == FlatCameraOwner::Legacy &&
          auxD.owner == FlatCameraOwner::None &&
          auxD.outcome == FlatCameraOutcome::Unsupported,
          "C4 main=upstream, cockpit=legacy, auxiliary=named-unsupported -- no frame-wide count hides the mix");
    // The auxiliary's unsupported camera must not poison the others'
    // closures either.
    flatCameraOwnerClose(mainS, mainD, true, true);
    flatCameraOwnerClose(cockpitS, cockpitD, true, true);
    flatCameraOwnerClose(auxS, auxD, false, false);
    check(mainS.historyValid && cockpitS.historyValid && !auxS.historyValid,
          "C4 closures stay per-group (auxiliary history invalid, others valid)");
}

// ---------------------------------------------------------------------------
// C5 ownership lifecycle across frames, exercised through the real API
// sequence (no manually preloaded states).
// ---------------------------------------------------------------------------
void testC5() {
    std::printf("C5 ownership lifecycle\n");
    // Frame 1: legacy treats and closes cleanly. Frame 2's certified
    // upstream must switch cleanly -- the completed frame never blocks the
    // switch (the review's "closed legacy work blocks upstream indefinitely"
    // defect, now guarded by the begin-transition's retirement).
    {
        FlatCameraOwnershipState s{};
        FlatCameraGroupInput in;
        in.legacyEligible = true;
        flatCameraOwnerBegin(s);
        FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        flatCameraOwnerNoteApplied(s, d);
        flatCameraOwnerClose(s, d, true, true);
        check(s.lastOwner == FlatCameraOwner::Legacy && s.historyValid,
              "C5 frame 1: legacy treats and closes cleanly");
        FlatCameraGroupInput in2;
        in2.upstreamCertified = true;
        in2.legacyEligible = true;
        flatCameraOwnerBegin(s); // retirement: frame 1's work is done
        d = flatCameraOwnerSelect(s, in2);
        check(d.owner == FlatCameraOwner::Upstream && d.historyReset &&
              d.preserveCameraInputs,
              "C5 frame 2: certified upstream switches cleanly (history reset, inputs preserved)");
        flatCameraOwnerClose(s, d, true, true);
        check(s.lastOwner == FlatCameraOwner::Upstream && s.historyValid,
              "C5 frame 2 closes with the new owner and fresh history");
    }
    // Mid-frame mix guard, forward: legacy applied, a later re-selection
    // this frame keeps the owner.
    {
        FlatCameraOwnershipState s{};
        FlatCameraGroupInput in;
        in.legacyEligible = true;
        flatCameraOwnerBegin(s);
        FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        check(d.owner == FlatCameraOwner::Legacy, "C5 mid-frame: legacy selected");
        flatCameraOwnerNoteApplied(s, d);
        FlatCameraGroupInput in2;
        in2.upstreamCertified = true;
        d = flatCameraOwnerSelect(s, in2);
        check(d.owner == FlatCameraOwner::Legacy &&
              std::strcmp(d.reason, "switch-deferred-applied-legacy") == 0,
              "C5 mid-frame: an applied legacy defers the switch (no mixed phase)");
    }
    // Mid-frame mix guard, reverse: upstream applied, a later legacy-only
    // selection keeps upstream.
    {
        FlatCameraOwnershipState s{};
        FlatCameraGroupInput in;
        in.upstreamCertified = true;
        flatCameraOwnerBegin(s);
        FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        check(d.owner == FlatCameraOwner::Upstream, "C5 mid-frame: upstream selected");
        flatCameraOwnerNoteApplied(s, d);
        FlatCameraGroupInput in2;
        in2.legacyEligible = true;
        d = flatCameraOwnerSelect(s, in2);
        check(d.owner == FlatCameraOwner::Upstream &&
              std::strcmp(d.reason, "switch-deferred-applied-upstream") == 0,
              "C5 mid-frame: an applied upstream defers the legacy takeover (symmetric guard)");
    }
    // Dirty closure ALWAYS invalidates history, even after a valid one.
    {
        FlatCameraOwnershipState s{};
        FlatCameraGroupInput in;
        in.upstreamCertified = true;
        flatCameraOwnerBegin(s);
        FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        flatCameraOwnerClose(s, d, true, true);
        check(s.historyValid, "C5 a clean closure validates history");
        flatCameraOwnerClose(s, d, true, false);
        check(!s.historyValid,
              "C5 a dirty closure invalidates history unconditionally");
    }
}

// ---------------------------------------------------------------------------
// C6 steady-state cost and the legacy-evidence classifier's consistency.
// ---------------------------------------------------------------------------
void testC6() {
    std::printf("C6 steady-state cost and classifier consistency\n");
    // The policy allocates nothing and touches no globals: it runs on the
    // stack, which is the whole steady-state resource claim (offline
    // measurement; equal-scene game cost is C3/C4, per the plan).
    FlatCameraOwnershipState s{};
    FlatCameraGroupInput in;
    in.upstreamCertified = true;
    in.legacyEligible = true;
    const auto t0 = std::chrono::steady_clock::now();
    uint32_t upstream = 0;
    for (uint32_t i = 0; i < 1000000; ++i) {
        flatCameraOwnerBegin(s);
        const FlatCameraOwnershipDecision d = flatCameraOwnerSelect(s, in);
        flatCameraOwnerClose(s, d, true, true);
        upstream += d.owner == FlatCameraOwner::Upstream ? 1u : 0u;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double nsPer = std::chrono::duration<double, std::nano>(t1 - t0).count() / 1000000.0;
    std::printf("  note  1,000,000 selections: %.1f ns/decision, stack-only, no allocation\n", nsPer);
    check(upstream == 1000000, "C6 selection is deterministic over a million identical frames");

    // The production ownership classifier, fed the derive's own scene-camera
    // encoding: identity match certifies, a changed near does not.
    Cam c; makeCamera(c);
    camF(c, kCamBoundX) = 0.0f; camF(c, kCamBoundY) = 0.0f;
    derive(c);
    float rows[24];
    sceneCameraRows(c, rows);
    FlatProjectionOwnershipInput oi{};
    oi.referenceBuffer = rows;
    oi.referenceCamera = reinterpret_cast<const unsigned char*>(rows);
    oi.referenceBytes = sizeof(rows);
    oi.currentReference = true;
    oi.candidateBuffer = rows;
    oi.candidateRows = reinterpret_cast<const unsigned char*>(rows);
    oi.candidateBytes = sizeof(rows);
    oi.currentCandidate = true;
    oi.layout = FlatProjectionOwnershipLayout::CanonicalVsB1;
    const FlatProjectionOwnershipResult same = flatClassifyProjectionOwnership(oi);
    check(same.kind == FlatProjectionOwnershipKind::CanonicalSceneCamera,
          "C6 the derive's scene-camera encoding certifies as the canonical scene camera");
    float changed[24];
    std::memcpy(changed, rows, sizeof(changed));
    changed[14] *= 1.5f; // a different near term
    oi.candidateBuffer = changed;
    oi.candidateRows = reinterpret_cast<const unsigned char*>(changed);
    const FlatProjectionOwnershipResult diff = flatClassifyProjectionOwnership(oi);
    check(diff.kind == FlatProjectionOwnershipKind::Unmatched,
          "C6 a changed camera is Unmatched (classifier evidence stays honest)");
}

// ---------------------------------------------------------------------------
// The C3 wiring (docs/design-flat-camera-integration.md, the 2026-09-29 wiring
// addendum). Everything below runs the SAME code the DLL runs
// (src/d3d11/flat_camera_phase.h): the frame protocol, the admission table,
// the frame-window gate, the flush, the fallback hysteresis, the census and
// the text of every new log line.
// ---------------------------------------------------------------------------
bool has(const char* hay, const char* needle) { return std::strstr(hay, needle) != nullptr; }

// C7: the frame protocol through the real FlatCameraFrameCore.
void testC7() {
    std::printf("C7 frame protocol, closure and the Legacy fallback\n");
    {
        // What the tick prints when nothing ever closed a frame -- which is also
        // what it prints if flatCameraInjectClose never runs -- against what it
        // prints once frames close: the two must be distinguishable.
        FlatCameraTickFields never;
        char neverText[640];
        flatCameraFormatTick(neverText, sizeof(neverText), never);
        FlatCameraFrameCore core;
        core.begin(true);
        check(core.route() == FlatCameraRoute::Upstream && !core.historyValid(),
              "C7 a begun frame is Upstream and its history is not valid until it closes");
        check(core.close(true, true, true, true, true) && core.historyValid(),
              "C7 a clean close records valid history (the tick's history= turns valid)");
        FlatCameraTickFields ran;
        ran.owner = "upstream"; ran.history = core.historyValid() ? "valid" : "invalid";
        ran.closes = 1; ran.cleanCloses = 1;
        char ranText[640];
        flatCameraFormatTick(ranText, sizeof(ranText), ran);
        check(has(neverText, "history=invalid") && has(neverText, "closes=0 clean-closes=0") &&
              has(ranText, "history=valid") && has(ranText, "closes=1 clean-closes=1") &&
              std::strcmp(neverText, ranText) != 0,
              "C7 the tick of a run that closes frames differs from the tick of a run that never does");
        core.begin(true);
        check(core.close(true, true, false, true, true) && !core.historyValid(),
              "C7 a failed close invalidates history");
        core.begin(true);
        check(core.close(true, true, true, true, true) && core.historyValid(),
              "C7 the next clean close revalidates it");
        check(!core.close(true, true, false, true, true) && core.historyValid(),
              "C7 a second close of the same frame changes nothing");
        core.reset();
        check(!core.valid() && !core.historyValid() && core.route() == FlatCameraRoute::Off,
              "C7 a resize drops the decision and the history");
    }
    {
        // The fallback engages after exactly ON missed frames; warm-up frames in
        // between neither count nor reset, because after a failed frame the phase
        // machine warms up again and the misses arrive interleaved with them.
        FlatCameraFrameCore core;
        core.begin(true); core.close(true, true, true, true, true); // a clean Upstream frame: lastOwner = Upstream
        (void)core.takeHistoryReset();
        bool engagedEarly = false;
        for (uint32_t miss = 1; miss <= kFlatCameraFallbackFramesOn; ++miss) {
            core.begin(true);
            core.close(true, false, false, true, true); // armed, scene named, nothing landed
            if (miss < kFlatCameraFallbackFramesOn) {
                engagedEarly = engagedEarly || core.fallback().active();
                for (int warm = 0; warm < 2; ++warm) { core.begin(true); core.close(false, false, true, true, true); }
                engagedEarly = engagedEarly || core.fallback().active();
            }
        }
        check(!engagedEarly && core.fallback().active() && core.fallback().engagements() == 1,
              "C7 the fallback engages at the ON-th missed frame and not before, warm-up frames between");
        const FlatCameraOwnershipDecision d = core.begin(true);
        check(d.owner == FlatCameraOwner::Legacy && d.historyReset && d.preserveCameraInputs &&
              std::strcmp(d.reason, "legacy-eligible-upstream-unsupported") == 0 &&
              core.route() == FlatCameraRoute::Legacy,
              "C7 the frame after the fallback engages is Legacy, with a history reset and inputs preserved");
        check(core.takeHistoryReset() && !core.takeHistoryReset(), "C7 the history reset is handed over once");
        core.close(false, true, true, true, true); // a clean Legacy frame
        const FlatCameraOwnershipDecision d2 = core.begin(true);
        check(d2.owner == FlatCameraOwner::Legacy && !d2.historyReset && !core.takeHistoryReset(),
              "C7 one-shot: after the first clean Legacy close there is no further reset");
        core.close(false, true, true, true, true);
        // Release: OFF-1 consecutive frames of kind-3 calls (however many of them the frames above already were)
        // keep it on Legacy, the OFF-th releases.
        while (core.fallback().kind3Frames() + 1 < kFlatCameraFallbackFramesOff) { core.begin(true); core.close(false, true, true, true, true); }
        check(core.fallback().active() && core.fallback().kind3Frames() + 1 == kFlatCameraFallbackFramesOff,
              "C7 OFF-1 consecutive frames of kind-3 calls do not release the fallback");
        core.begin(true); core.close(false, true, true, false, false); // a frame with no kind-3 call resets the run
        check(core.fallback().active() && core.fallback().kind3Frames() == 0, "C7 a frame with no kind-3 call restarts the count");
        for (uint32_t i = 1; i < kFlatCameraFallbackFramesOff; ++i) { core.begin(true); core.close(false, true, true, true, true); }
        check(core.fallback().active(), "C7 the release needs CONSECUTIVE kind-3 frames: OFF-1 after the gap is still not enough");
        core.begin(true); core.close(false, true, true, true, true);
        check(!core.fallback().active() && core.fallback().releases() == 1,
              "C7 the OFF-th consecutive frame of kind-3 calls releases the fallback");
        const FlatCameraOwnershipDecision d3 = core.begin(true);
        check(d3.owner == FlatCameraOwner::Upstream && d3.historyReset && core.takeHistoryReset(),
              "C7 Upstream is tried again with a one-shot history reset (the identity switches back)");
    }
    {
        // Without a legacy plan the fallback is a named safe outcome, not a silent one.
        FlatCameraFrameCore core;
        core.begin(false); core.close(true, true, true, true, true);
        for (uint32_t miss = 0; miss < kFlatCameraFallbackFramesOn; ++miss) { core.begin(false); core.close(true, false, false, true, true); }
        const FlatCameraOwnershipDecision d = core.begin(false);
        check(d.owner == FlatCameraOwner::None && d.outcome == FlatCameraOutcome::Unsupported &&
              core.route() == FlatCameraRoute::None,
              "C7 fallback with no legacy plan: owner None, outcome Unsupported, route None (no phase)");
    }
    {
        // A landing resets the miss count; frames without a scene do not count.
        FlatCameraFrameCore core;
        core.begin(true); core.close(true, true, true, true, true);
        for (uint32_t i = 0; i + 1 < kFlatCameraFallbackFramesOn; ++i) { core.begin(true); core.close(true, false, false, true, true); }
        core.begin(true); core.close(true, true, true, true, true); // one landed
        for (uint32_t i = 0; i + 1 < kFlatCameraFallbackFramesOn; ++i) { core.begin(true); core.close(true, false, false, true, true); }
        for (uint32_t i = 0; i < 5; ++i) { core.begin(true); core.close(true, false, false, false, true); } // menus: no scene
        check(!core.fallback().active() && core.fallback().engagements() == 0,
              "C7 a landing resets the miss count, and frames without a scene never count as misses");
    }
    check(kFlatCameraFallbackFramesOn >= 1 && kFlatCameraFallbackFramesOff >= 1,
          "C7 the two hysteresis defaults live in one place and are positive");
    std::printf("  note  fallback hysteresis defaults: ON=%u armed scene frames with no injection, OFF=%u consecutive frames of kind-3 calls\n",
                kFlatCameraFallbackFramesOn, kFlatCameraFallbackFramesOff);
}

// C8: the admission table, exhaustively.
void testC8() {
    std::printf("C8 admission table\n");
    const uint32_t kinds[] = {0, 1, 2, 3, 4, 5, 6, 0xFFFFFFFFu};
    const FlatCameraGateVerdict gates[] = {FlatCameraGateVerdict::Admit, FlatCameraGateVerdict::Disarmed,
                                           FlatCameraGateVerdict::Expired, FlatCameraGateVerdict::OffThread};
    unsigned combos = 0, injects = 0;
    bool injectIffSpec = true, offThreadAlways = true, namedOthers = true, neverOutsideKind3 = true, namesOk = true;
    for (int readable = 0; readable < 2; ++readable)
        for (uint32_t kind : kinds)
            for (FlatCameraGateVerdict gate : gates)
                for (int upstream = 0; upstream < 2; ++upstream)
                    for (int phase = 0; phase < 2; ++phase) {
                        FlatCameraAdmitInput in;
                        in.readable = readable != 0; in.kind = kind; in.gate = gate;
                        in.upstreamOwns = upstream != 0; in.phaseNonzero = phase != 0;
                        const FlatCameraAdmit a = flatCameraAdmit(in);
                        ++combos;
                        const bool spec = readable && kind == 3 && gate == FlatCameraGateVerdict::Admit && upstream && phase;
                        if ((a == FlatCameraAdmit::Inject) != spec) injectIffSpec = false;
                        if (a == FlatCameraAdmit::Inject) { ++injects; if (!readable || kind != 3) neverOutsideKind3 = false; }
                        if (gate == FlatCameraGateVerdict::OffThread && a != FlatCameraAdmit::OffThread) offThreadAlways = false;
                        if (gate != FlatCameraGateVerdict::OffThread) {
                            if (!readable && a != FlatCameraAdmit::Unreadable) namedOthers = false;
                            if (readable && (kind == 4 || kind == 5) && a != FlatCameraAdmit::Unsupported) namedOthers = false;
                            if (readable && kind != 3 && kind != 4 && kind != 5 && a != FlatCameraAdmit::OtherKind) namedOthers = false;
                            if (readable && kind == 3) {
                                const FlatCameraAdmit want = gate != FlatCameraGateVerdict::Admit ? FlatCameraAdmit::GateClosed
                                    : !upstream ? FlatCameraAdmit::NotUpstream
                                    : !phase ? FlatCameraAdmit::Warming : FlatCameraAdmit::Inject;
                                if (a != want) namedOthers = false;
                            }
                        }
                        if (!flatCameraAdmitName(a) || !flatCameraAdmitName(a)[0]) namesOk = false;
                    }
    std::printf("  note  %u input combinations, %u of them inject\n", combos, injects);
    check(injectIffSpec && injects == 1 && neverOutsideKind3,
          "C8 a call is injected only for a readable kind-3 camera with an open window, Upstream ownership and a phase");
    check(offThreadAlways, "C8 a call on another thread is OffThread whatever else is true");
    check(namedOthers, "C8 every other call is named: unreadable, kinds 4/5 unsupported, other kinds, closed window, not-upstream, warming");
    check(namesOk, "C8 every admission has a name for the counters");
}

// C9: the frame window.
void testC9() {
    std::printf("C9 frame window\n");
    FlatCameraGate gate;
    check(gate.check(7, 1000) == FlatCameraGateVerdict::Disarmed, "C9 a gate that never saw a Present admits nothing");
    gate.disarm(7);
    check(gate.check(7, 1000) == FlatCameraGateVerdict::Disarmed && gate.ownerThread() == 7,
          "C9 the Present edge closes the window and records the owner thread");
    gate.arm(1000);
    check(gate.check(7, 1000) == FlatCameraGateVerdict::Admit, "C9 arming opens the window for the owner thread");
    check(gate.check(9, 1000) == FlatCameraGateVerdict::OffThread, "C9 any other thread is OffThread, armed or not");
    check(gate.check(7, 1000 + FlatCameraGate::kExpiryMs) == FlatCameraGateVerdict::Admit,
          "C9 the window is still open at exactly the expiry");
    check(gate.check(7, 1001 + FlatCameraGate::kExpiryMs) == FlatCameraGateVerdict::Expired,
          "C9 the window lapses on its own after the expiry (a Present that never came)");
    gate.arm(2000);
    check(gate.check(7, 2100) == FlatCameraGateVerdict::Admit, "C9 re-arming reopens a lapsed window");
    gate.disarm(7);
    check(gate.check(7, 2100) == FlatCameraGateVerdict::Disarmed && gate.check(9, 2100) == FlatCameraGateVerdict::OffThread,
          "C9 the next Present edge closes it again");
    gate.arm(0);
    check(gate.check(7, 5) == FlatCameraGateVerdict::Admit, "C9 arming at time zero still arms (zero is the disarmed marker)");
    // The skipped-Present case: a frame whose Present returned early never re-arms.
    gate.disarm(7);
    check(gate.check(7, 3000) == FlatCameraGateVerdict::Disarmed,
          "C9 a Present that returns early leaves the window closed, so the previous phase cannot keep injecting");
}

// C10: the flush.
void testC10() {
    std::printf("C10 flush of cameras left with a stale phase\n");
    const FlatCameraAdmit all[] = {FlatCameraAdmit::Inject, FlatCameraAdmit::Warming, FlatCameraAdmit::NotUpstream,
                                   FlatCameraAdmit::GateClosed, FlatCameraAdmit::OffThread, FlatCameraAdmit::Unsupported,
                                   FlatCameraAdmit::OtherKind, FlatCameraAdmit::Unreadable};
    {
        FlatCameraInjectedSet set;
        bool never = true;
        for (FlatCameraAdmit a : all)
            for (int i = 0; i < 3; ++i)
                if (flatCameraFlushDecision(set, 0x1000, a)) never = false;
        check(never && set.empty(), "C10 a camera this session never injected is never flushed, whatever the admission");
    }
    {
        FlatCameraInjectedSet set;
        const uintptr_t A = 0x2000, B = 0x3000, C = 0x4000;
        set.noteInjected(A);
        set.noteInjected(B);
        check(!flatCameraFlushDecision(set, C, FlatCameraAdmit::Warming),
              "C10 only the cameras injected are flushed: a bystander (never injected) is untouched while others are pending");
        check(!flatCameraFlushDecision(set, A, FlatCameraAdmit::Inject) && set.contains(A),
              "C10 an injected call never flushes and keeps the camera a candidate");
        for (FlatCameraAdmit a : {FlatCameraAdmit::OffThread, FlatCameraAdmit::Unsupported, FlatCameraAdmit::OtherKind,
                                  FlatCameraAdmit::Unreadable})
            if (flatCameraFlushDecision(set, A, a)) check(false, "C10 an ineligible admission flushed a camera");
        check(set.contains(A), "C10 ineligible admissions (off-thread, wrong kind, unreadable) neither flush nor consume the entry");
        check(flatCameraFlushDecision(set, A, FlatCameraAdmit::Warming),
              "C10 the first un-injected call of an injected camera flushes it");
        check(!flatCameraFlushDecision(set, A, FlatCameraAdmit::Warming) &&
              !flatCameraFlushDecision(set, A, FlatCameraAdmit::NotUpstream) &&
              !flatCameraFlushDecision(set, A, FlatCameraAdmit::GateClosed),
              "C10 exactly once per edge: the following un-injected calls do not flush again");
        check(set.contains(B) && flatCameraFlushDecision(set, B, FlatCameraAdmit::GateClosed) && set.empty(),
              "C10 each pending camera flushes on its own edge, and the set empties");
    }
    {
        // Edges repeat: inject, uninject (1 flush), uninject (still 1), inject, uninject (2).
        FlatCameraInjectedSet set;
        unsigned flushes = 0;
        const uintptr_t cam = 0x5000;
        const FlatCameraAdmit script[] = {FlatCameraAdmit::Inject, FlatCameraAdmit::Inject, FlatCameraAdmit::NotUpstream,
                                          FlatCameraAdmit::NotUpstream, FlatCameraAdmit::Warming, FlatCameraAdmit::Inject,
                                          FlatCameraAdmit::GateClosed, FlatCameraAdmit::GateClosed, FlatCameraAdmit::Inject,
                                          FlatCameraAdmit::Inject, FlatCameraAdmit::Warming};
        unsigned edges = 0;
        FlatCameraAdmit last = FlatCameraAdmit::Warming;
        for (FlatCameraAdmit a : script) {
            if (a == FlatCameraAdmit::Inject) set.noteInjected(cam);
            else if (flatCameraFlushDecision(set, cam, a)) ++flushes;
            if (last == FlatCameraAdmit::Inject && a != FlatCameraAdmit::Inject) ++edges;
            last = a;
        }
        check(flushes == edges && flushes == 3, "C10 one flush per injected-to-uninjected edge over a script with three edges");
    }
    {
        // Bounded: a full set forgets the camera injected least recently, and says so.
        FlatCameraInjectedSet set;
        for (uintptr_t i = 1; i <= FlatCameraInjectedSet::kCapacity; ++i) set.noteInjected(i * 0x100);
        set.noteInjected(0x100); // refresh the oldest: it is now the most recent
        set.noteInjected(0xFFFF00);
        check(set.size() == FlatCameraInjectedSet::kCapacity && set.evicted() == 1 && set.contains(0x100) &&
              !set.contains(0x200) && set.contains(0xFFFF00),
              "C10 a full set evicts the least recently injected camera (counted) and keeps a refreshed one");
        check(!flatCameraFlushDecision(set, 0x200, FlatCameraAdmit::Warming),
              "C10 an evicted camera cannot flush (that is why the eviction count is on the tick)");
        unsigned flushed = 0;
        for (uintptr_t i = 1; i <= FlatCameraInjectedSet::kCapacity; ++i)
            if (flatCameraFlushDecision(set, i * 0x100, FlatCameraAdmit::Warming)) ++flushed;
        if (flatCameraFlushDecision(set, 0xFFFF00, FlatCameraAdmit::Warming)) ++flushed;
        check(flushed == FlatCameraInjectedSet::kCapacity && set.empty(), "C10 every remembered camera flushes exactly once");
    }
    check(!flatCameraFlushEligible(FlatCameraAdmit::Inject) && flatCameraFlushEligible(FlatCameraAdmit::GateClosed) &&
          !flatCameraFlushEligible(FlatCameraAdmit::OffThread),
          "C10 only calls proven to be a kind-3 camera on the owner thread are eligible to flush");
}

// C11: the census and the text of the new log lines.
void testC11() {
    std::printf("C11 census and log lines\n");
    FlatCameraCensus census;
    char text[1200];
    flatCameraFormatCensus(text, sizeof(text), census, 0, 0);
    check(has(text, "flat camera census 5s:") && has(text, "cameras=0") && has(text, "top=[]"),
          "C11 an empty census still prints a line (cameras=0): an absent line means the code never ran");
    // A steady cockpit second: one main camera and a few auxiliaries, four callers.
    for (int i = 0; i < 300; ++i) { census.noteKind(true, 3); census.note(0x210654F6CA0, 3, true); census.noteCaller(0x594E13); }
    for (int i = 0; i < 300; ++i) { census.noteKind(true, 3); census.note(0x21065500000, 3, true); census.noteCaller(0x594EAB); }
    for (int i = 0; i < 90; ++i) { census.noteKind(true, 1); census.note(0x21065510000, 1, false); census.noteCaller(0x58DE73); }
    for (int i = 0; i < 10; ++i) { census.noteKind(true, 4); census.note(0x21065520000, 4, false); census.noteCaller(0x123456); }
    census.noteKind(false, 0);
    check(census.used() == 4 && census.kindCount(3) == 600 && census.kindCount(1) == 90 && census.kindCount(4) == 10 &&
          census.kindCount(7) == 1 && census.callerCount(0) == 300 && census.callerCount(1) == 300 &&
          census.callerCount(3) == 90 && census.callerCount(4) == 10,
          "C11 the census keeps per-camera, per-kind and per-call-site counts");
    flatCameraFormatCensus(text, sizeof(text), census, 300, 600);
    check(has(text, "cameras=4") && has(text, "injected-per-frame=2.0") && has(text, "0x210654f6ca0:3:300:300") &&
          has(text, "+0x594E13=300") && has(text, "+0x58DE73=90") && has(text, "other=10") && std::strlen(text) < 1000,
          "C11 the census line names the cameras (busiest first), the kinds and the call sites, and fits a log line");
    std::printf("  note  %s\n", text);
    for (uintptr_t i = 0; i < 40; ++i) census.note(0x900000 + i * 0x1000, 3, false);
    check(census.used() == FlatCameraCensus::kCapacity && census.overflow() == 4 + 40 - FlatCameraCensus::kCapacity,
          "C11 a full census table counts what it cannot hold (4 cameras were known, 40 more arrived, 16 fit)");
    census.reset();
    check(census.used() == 0 && census.overflow() == 0 && census.kindCount(3) == 0, "C11 the window resets the census");

    FlatCameraTickFields tick;
    tick.refreshCalls = 680; tick.injected = 515; tick.kindRefusals = 165;
    tick.owner = "upstream"; tick.history = "valid"; tick.closes = 300; tick.cleanCloses = 298;
    tick.flushed = 2; tick.historyResets = 0;
    flatCameraFormatTick(text, sizeof(text), tick);
    check(has(text, "flat camera inject 5s: refresh-calls=680 injected=515 warming=0 kind-refusals=165 unsupported=0 owner=upstream history=valid closes=300") &&
          has(text, "closes=300 clean-closes=298 stale=0 off-thread=0 not-upstream=0 flushed=2 flush-failed=0 write-failures=0 history-resets=0") &&
          has(text, "fallback-frames=3/60 fallback=upstream fallbacks=0 set-evicted=0") &&
          !has(text, "trace"),
          "C11 the tick keeps its first seven fields in their old order and appends the wiring's counters and the hysteresis defaults, and no trace field (the trace setting is gone)");
    std::printf("  note  %s\n", text);
    FlatCameraTickFields dead;
    flatCameraFormatTick(text, sizeof(text), dead);
    std::printf("  note  (never ran) %s\n", text);

    FlatCameraRowsFields rows;
    rows.frames = 300; rows.unjitteredResolves = 298; rows.zeroPhaseResolves = 2;
    rows.pairs.checked = 296; rows.pairs.skipped = 3; rows.pairs.maxError = 3.1e-7f;
    rows.legacyPrepSkipped = 1240;
    flatCameraFormatRows(text, sizeof(text), rows);
    check(has(text, "flat camera rows 5s: frames=300 unjittered-resolves=298 zero-phase-resolves=2 row-pairs=296 row-pairs-skipped=3 row-pair-mismatch=0") &&
          has(text, "legacy-applied-under-upstream=0 legacy-prep-skipped=1240"),
          "C11 the rows line reports the unjittered resolves, the pair checks, the tripwire and the skipped preparations");
    std::printf("  note  %s\n", text);
    flatCameraFormatOwner(text, sizeof(text), 71905, "upstream", "legacy", "legacy-eligible-upstream-unsupported", true, true);
    check(has(text, "flat camera inject owner: frame=71905 upstream -> legacy reason=legacy-eligible-upstream-unsupported history-reset=1 fallback=legacy"),
          "C11 an owner transition names both routes, the reason, the history reset and the fallback");
    std::printf("  note  %s\n", text);
    flatCameraFormatRowsMismatch(text, sizeof(text), 71906, 0.25f, -0.375f, -0.25f, 0.375f, 1.9e-3f);
    check(has(text, "flat camera rows mismatch: frame=71906"), "C11 a rows mismatch names the frame, the claimed phases and the error");
    std::printf("  note  %s\n", text);
}

// C12: the route decisions.
void testC12() {
    std::printf("C12 route decisions\n");
    bool legacyExpr = true, upstreamIgnoresPlan = true, upstreamYieldsToObserving = true, noneNever = true;
    for (int wanted = 0; wanted < 2; ++wanted)
        for (int observing = 0; observing < 2; ++observing)
            for (int plan = 0; plan < 2; ++plan) {
                const bool old = wanted && !observing && plan; // the expression flat_runtime had before the wiring
                if (flatCameraPhaseEnabled(FlatCameraRoute::Off, wanted, observing, plan) != old) legacyExpr = false;
                if (flatCameraPhaseEnabled(FlatCameraRoute::Legacy, wanted, observing, plan) != old) legacyExpr = false;
                if (flatCameraPhaseEnabled(FlatCameraRoute::Upstream, wanted, observing, plan) != (wanted && !observing)) upstreamIgnoresPlan = false;
                if (observing && flatCameraPhaseEnabled(FlatCameraRoute::Upstream, wanted, observing, plan)) upstreamYieldsToObserving = false;
                if (flatCameraPhaseEnabled(FlatCameraRoute::None, wanted, observing, plan)) noneNever = false;
            }
    check(legacyExpr, "C12 Off (the injector is not wanted) and Legacy run the phase machine exactly as the line did before the wiring");
    check(upstreamIgnoresPlan && upstreamYieldsToObserving,
          "C12 Upstream needs no legacy plan but yields to observation and to the jitter kill switch");
    check(noneNever, "C12 a frame no route may mutate never jitters");
    const FlatCameraRoute routes[] = {FlatCameraRoute::Off, FlatCameraRoute::None, FlatCameraRoute::Legacy, FlatCameraRoute::Upstream};
    bool rowsOk = true;
    for (FlatCameraRoute r : routes)
        for (uint32_t applied : {0u, 3u}) {
            const FlatCameraRowsPhase p = flatCameraRowsPhase(r, applied, 0.25f, -0.375f);
            const bool carries = r == FlatCameraRoute::Upstream && applied != 0;
            if (carries ? (p.x != 0.25f || p.y != -0.375f) : (p.x != 0.0f || p.y != 0.0f)) rowsOk = false;
        }
    check(rowsOk, "C12 the captured rows carry a phase only on an Upstream frame in which an injection landed");
}

// C13-C15 (2026-09-29): the camera path is ON with a temporal mode and there is no setting, so the
// draw-time adapter is only the automatic fallback. These are the ways a frame reaches it.

// One frame's decision as the runtime sees it.
struct FallbackFrame { FlatCameraRoute route; bool historyReset; bool phaseMachineRuns; };

// A session of `frames` frames. The hook lands an injection on the first `healthy` frames (on all of
// them when `hookRuns`); after that nothing lands and no kind-3 camera reaches the detour, exactly what
// the DLL does after a prologue mismatch (the hook is never installed: healthy = 0) or a write-failure
// stand-down (the gate closes after `healthy` frames). An Upstream frame closes armed, scene named, and
// lands only while the hook lives; a Legacy frame is the draw-time adapter's, which applies the phase
// itself and closes clean. Returns how many times the fallback engaged.
uint64_t fallbackSession(bool hookRuns, uint32_t healthy, uint32_t frames, bool legacyPlan, FallbackFrame* out) {
    FlatCameraFrameCore core;
    for (uint32_t i = 0; i < frames; ++i) {
        core.begin(legacyPlan);
        const FlatCameraRoute route = core.route();
        out[i] = {route, core.takeHistoryReset(), flatCameraPhaseEnabled(route, true, false, legacyPlan)};
        const bool lands = hookRuns || i < healthy;
        if (route == FlatCameraRoute::Legacy) core.close(false, true, true, true, lands);
        else core.close(true, lands, lands, true, lands);
    }
    return core.fallback().engagements();
}
constexpr uint32_t kHealthyStretch = 20;
constexpr uint32_t kFallbackFrames = kHealthyStretch + kFlatCameraFallbackFramesOn + 1 + 3 * kFlatCameraFallbackFramesOff;
FallbackFrame g_frames[4][kFallbackFrames];

// C13: a prologue mismatch. The game changed under the hook (an update), so the DLL's install check
// refuses the refresh's prologue, latches "no hook", and no kind-3 camera ever reaches the detour.
void testC13() {
    std::printf("C13 prologue mismatch: the draw-time path takes the frame\n");
    FallbackFrame* const mismatch = g_frames[0];
    FallbackFrame* const control = g_frames[1];
    FallbackFrame* const afterHealthy = g_frames[2];
    FallbackFrame* const noPlan = g_frames[3];
    const uint64_t engaged = fallbackSession(false, 0, kFallbackFrames, true, mismatch);
    bool upstreamFirst = true;
    for (uint32_t i = 0; i < kFlatCameraFallbackFramesOn; ++i) upstreamFirst = upstreamFirst && mismatch[i].route == FlatCameraRoute::Upstream;
    check(engaged == 1 && upstreamFirst && mismatch[kFlatCameraFallbackFramesOn].route == FlatCameraRoute::Legacy,
          "C13 with the hook never running, Upstream is tried for ON frames and then Legacy takes the frame");
    bool phaseMachineRuns = true, stays = true, noReset = true;
    for (uint32_t i = 0; i < kFallbackFrames; ++i) {
        noReset = noReset && !mismatch[i].historyReset;
        if (i >= kFlatCameraFallbackFramesOn) {
            phaseMachineRuns = phaseMachineRuns && mismatch[i].phaseMachineRuns;
            stays = stays && mismatch[i].route == FlatCameraRoute::Legacy;
        }
    }
    check(phaseMachineRuns, "C13 on Legacy the phase machine runs: the draw-time adapter jitters the frame");
    check(stays, "C13 nothing ever reaches the detour, so the fallback never releases: Legacy for the rest of the session");
    // A mismatch is decided at the first frame, so no Upstream frame ever closed clean and the
    // ownership policy holds no Upstream history: the switch owes no history reset (the runtime's own
    // reset on an unclean frame already covers the three failed ones). The reset is owed, and carried,
    // when there WAS a clean Upstream history: the same switch after a healthy stretch.
    check(noReset, "C13 a mismatch from the first frame switches with no ownership history reset: there was no Upstream history");
    const uint64_t engagedAfter = fallbackSession(false, kHealthyStretch, kFallbackFrames, true, afterHealthy);
    uint32_t resets = 0, firstReset = 0;
    for (uint32_t i = 0; i < kFallbackFrames; ++i) if (afterHealthy[i].historyReset) { if (!resets) firstReset = i; ++resets; }
    check(engagedAfter == 1 && resets == 1 && firstReset == kHealthyStretch + kFlatCameraFallbackFramesOn &&
          afterHealthy[firstReset].route == FlatCameraRoute::Legacy,
          "C13 the same switch after a clean Upstream stretch carries exactly one history reset, on the first Legacy frame");
    // The negative control: with a hook that runs the same frames stay on Upstream, engage nothing, reset nothing.
    const uint64_t controlEngaged = fallbackSession(true, 0, kFallbackFrames, true, control);
    bool controlUpstream = true, controlNoReset = true;
    for (uint32_t i = 0; i < kFallbackFrames; ++i) {
        controlUpstream = controlUpstream && control[i].route == FlatCameraRoute::Upstream;
        controlNoReset = controlNoReset && !control[i].historyReset;
    }
    check(controlEngaged == 0 && controlUpstream && controlNoReset,
          "C13 control: with the hook running the same frames stay Upstream with no fallback and no reset, so the switch above is the mismatch's doing");
    // Without a legacy plan the outcome is named, not silent: no phase at all (C7 covers the decision itself).
    fallbackSession(false, 0, kFallbackFrames, false, noPlan);
    check(noPlan[kFlatCameraFallbackFramesOn].route == FlatCameraRoute::None && !noPlan[kFlatCameraFallbackFramesOn].phaseMachineRuns,
          "C13 with no legacy plan either, the frame goes to None and nothing jitters it");
}

// C14: write failures. The limit is one named constant; the boundary is 7 keep, 8 stand down; and a
// stand-down closes the gate for the session, so nothing lands from then on and the same hysteresis
// hands every frame to the draw-time path, with the one history reset the Upstream history is owed.
void testC14() {
    std::printf("C14 write failures: the boundary, and what a stand-down hands over\n");
    auto boundary = [](bool (*standDown)(uint64_t)) {
        return !standDown(0) && !standDown(kFlatCameraWriteFailureLimit - 1) &&
               standDown(kFlatCameraWriteFailureLimit) && standDown(kFlatCameraWriteFailureLimit + 1) && standDown(1000);
    };
    check(kFlatCameraWriteFailureLimit == 8, "C14 the limit is 8 failed camera writes in one 5 s window, named in one place");
    check(boundary(&flatCameraWriteFailureStandDown), "C14 7 failed writes in a window keep the hook, 8 stand it down");
    // Negative controls: an off-by-one either way, and a strict comparison, all fail the same boundary check.
    check(!boundary([](uint64_t n) { return n >= kFlatCameraWriteFailureLimit - 1; }), "C14 control: a limit one too low is seen by the boundary check");
    check(!boundary([](uint64_t n) { return n >= kFlatCameraWriteFailureLimit + 1; }), "C14 control: a limit one too high is seen by the boundary check");
    check(!boundary([](uint64_t n) { return n > kFlatCameraWriteFailureLimit; }), "C14 control: a strict comparison is seen by the boundary check");
    // The consequence: the boundary decides whether the gate closes after the healthy stretch.
    auto outcome = [](uint64_t failuresInWindow, uint32_t* switchedAt, uint32_t* resets, FlatCameraRoute* last) {
        const bool standsDown = flatCameraWriteFailureStandDown(failuresInWindow);
        FallbackFrame* const frames = g_frames[0];
        fallbackSession(!standsDown, kHealthyStretch, kFallbackFrames, true, frames);
        *switchedAt = 0; *resets = 0;
        for (uint32_t i = 0; i < kFallbackFrames; ++i) {
            if (frames[i].route == FlatCameraRoute::Legacy && *switchedAt == 0) *switchedAt = i;
            if (frames[i].historyReset) ++*resets;
        }
        *last = frames[kFallbackFrames - 1].route;
    };
    uint32_t at = 0, resets = 0; FlatCameraRoute last = FlatCameraRoute::Off;
    outcome(kFlatCameraWriteFailureLimit, &at, &resets, &last);
    check(at == kHealthyStretch + kFlatCameraFallbackFramesOn && resets == 1 && last == FlatCameraRoute::Legacy,
          "C14 8 failed writes: ON frames after the stand-down the draw-time path takes over with one history reset, and stays there");
    outcome(kFlatCameraWriteFailureLimit - 1, &at, &resets, &last);
    check(at == 0 && resets == 0 && last == FlatCameraRoute::Upstream,
          "C14 control: 7 failed writes stand nothing down, and the same frames stay on Upstream with no reset");
}
// C15: the camera path exists with a temporal mode and only then. The AA-off invariant: with the mode
// off the injector is not wanted, so no hook is installed, nothing is written and no "flat camera" line
// is logged (every such line is written from flatCameraInjectFrame's paths, which the flat runtime
// reaches only with a mode selected; flatCameraInjectFrame asks this predicate a second time).
void testC15() {
    std::printf("C15 the camera path is on with a temporal mode, and only then\n");
    check(flatCameraPathWanted(true, true), "C15 the flat profile with a temporal mode selected wants the camera path, with no setting");
    check(!flatCameraPathWanted(true, false),
          "C15 the flat profile with the mode OFF does not: nothing installed, nothing written, no \"flat camera\" line");
    check(!flatCameraPathWanted(false, true) && !flatCameraPathWanted(false, false), "C15 another profile never wants it");
    auto ignoresMode = [](bool flat, bool) { return flat; };   // what an unconditional "always on" would be
    check(ignoresMode(true, false) && !flatCameraPathWanted(true, false),
          "C15 control: a predicate that ignored the mode would want the camera path with the mode off, and the row above sees it");
}

int runSelfTest() {
    testC1();
    testC2();
    testC3();
    testC4();
    testC5();
    testC6();
    testC7();
    testC8();
    testC9();
    testC10();
    testC11();
    testC12();
    testC13();
    testC14();
    testC15();
    if (g_failures == 0) {
        std::printf("c2 coexist: PASS\n");
        return 0;
    }
    std::printf("c2 coexist: %d FAILED check(s)\n", g_failures);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return runSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("c2 coexist test: dry run (no checks run)\n");
        return 0;
    }
    std::printf("usage: c2_coexist_test --self-test|--dry-run\n");
    return 2;
}
