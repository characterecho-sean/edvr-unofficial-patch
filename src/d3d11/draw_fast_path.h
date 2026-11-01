#pragma once
// The AA-off draw path (vscreen.cpp's draw thunks and binding hooks, 2026-10-13). Pure decisions, so the rig
// (tools/aa_off_test) runs the same functions the thunks call.
//
// With VR's temporal anti-aliasing off, every consumer of the engine-record velocity shadows stands down (engine
// velocity is closed, PROVEN in engine_velocity.h's gate), and the draw thunks still ran a verdict scope, an
// engine velocity check and the shadow records for every draw. The fast path skips those, and only for a draw that
// is unclaimed by any fix: the verdict is kNone AND the subscriber gate (draw_gate.h, the forty-term list of every
// feature that acts on a draw) is closed. A gate that is open means some feature may act, so the full path runs.
//
// Readers of the two shadows the bind hooks record (binding_shadow.h), checked 2026-10-13 by grep over src\:
//   VsSrv33  -- engine_velocity.h:159; engine_velocity.cpp:1272, 1385, 2913. Engine velocity only.
//   Blend    -- engine_velocity.h:159; engine_velocity.cpp:935, 1867, 1875, 1890, 1923, 2912. Engine velocity only.
//   VsCb1    -- engine velocity (as above) AND flat_runtime.cpp:5151, 5159 and flat_temporal.cpp:1052, 1061, 1088.
//               Another reader exists, so VsCb1 is recorded whatever the AA mode.
// bindingAudit (vscreen.cpp) reads the VS shadow only; ui_layer.cpp reads PsSrv0..1; the PS-set self-marking
// note (engineVelocityNoteSelfMarkingPs) is the draw-path census's counter, so it stays on the PS hook.
#include <cstdint>

namespace edvr {

// The loads the draw thunks take. All relaxed; none is a lock or a call beyond the atomics themselves.
struct DrawPathInputs {
    bool aaOff;               // VR's fix.temporal_aa reads off (vscreen's config refresh)
    bool gateWanted;          // draw_gate.h: some subscriber may act on a draw this frame
    bool engineVelocityLive;  // engine-record velocity is open (engineVelocityLiveNow)
    bool vrWorld;             // the VR world route is armed (g_vrWorldWants)
    bool claimed;             // the verdict is not kNone: a fix claims this draw
};

// F201: is the draw claimed? A fix claims it by a verdict (not kNone), and native on foot's marker fix claims it when the draw tee has DECIDED this very draw and is waiting to bind its
// patched b0 at the game's issue (nativeOnFootMarkerFixPending): the bind sits in forwardWithVerdict, which the fast path skips. Asked only while the AA is off, so with the AA on the
// claim, and the whole decision, is what it was.
inline bool drawClaimed(bool verdictClaims, bool markerFixPending) { return verdictClaims || markerFixPending; }

// F201: does forwardWithVerdict bind the marker fix's patched b0 for this draw? The bind was inside the UI layer's bracket (the game's own issue and the layer's second issues), so a
// draw the layer did not take never got it: with the temporal path off (Anti-aliasing off) the layer is not live, no marker draw was ever layered, and every decision of the marker lock,
// the weapon readout pin and the labels went unbound. With no layer there is no second issue: the bind covers the game's own single issue. With the temporal path on the answer is
// `layered`, as before, bit for bit.
inline bool markerFixBindWanted(bool layered, bool temporalPathRuns) { return layered || !temporalPathRuns; }

// True when the draw takes the fast path: no verdict scopes, no engine velocity call, the game's draw issued directly.
inline bool drawFastPathTaken(const DrawPathInputs& in) {
    return !in.claimed && in.aaOff && !in.gateWanted && !in.engineVelocityLive && !in.vrWorld;
}

// True when the VsSrv33 and Blend bind records are skipped. Their only reader is engine velocity, which is closed
// while the AA is off; an open engine velocity (a diagnostic) keeps them recorded.
inline bool bindShadowsSkipped(bool aaOff, bool engineVelocityLive) {
    return aaOff && !engineVelocityLive;
}

}  // namespace edvr
