#pragma once
// A draw's own flags (VR draw path CPU, build 6, item 3; #184; docs\vr-draw-path-cpu-2026-11-01.md, "Build 6"). Pure decisions, so the rig (tools/aa_off_test) runs the same
// functions vscreen.cpp's draw thunks and forwardWithVerdict call.
//
// beginPanelOverride fills a DrawCallFlags for the draw it is deciding and the thunk passes it to forwardWithVerdict. It is all false on every early return of
// beginPanelOverride by construction (the thunk declares it false and only the eye branch writes it). Before build 6 these were three thread_local booleans that an early
// return left set, so a draw that was blocked (the UI layer's fence), skipped, or on a foreign context could take the flags of an earlier draw: a foreign-context draw
// then ran the UI-depth and hologram re-issue Begins on the foreign context, and the first non-eye owner draw after the fence re-issued into the hologram scratch targets.
// The pending flags (planetPending, worldScreenPending) leaked past the same returns, and past the `layered` case, which is not a fault.
#include <cstdint>

namespace edvr {

struct DrawCallFlags {
    bool uiDepth = false;            // ui_depth.h: this eye draw writes its coverage depth (a reissue follows the game's own draw)
    bool holoDepth = false;          // the generic hologram/icon depth pass classified this draw
    bool composite = false;          // this eye draw samples a learned interface surface (the layer's census of composites left in the scene)
    bool planetPending = false;      // uiDepthPlanetPending(): the opaque planet coverage follows this draw
    bool worldScreenPending = false; // uiDepthWorldScreenPending(): the F105 in-world screen coverage follows this draw
};

// True when this draw set a pending flag in the ui_depth module, so a return that does not run the Begin that clears it must clear it. ONLY then: a draw that set
// neither (a foreign-context draw, a non-eye draw) must never clear the module's copy, which an owner draw between its classification and its Begin is waiting on.
inline bool drawCallOwnsPending(const DrawCallFlags& f) { return f.planetPending || f.worldScreenPending; }

// The second issues forwardWithVerdict makes after the game's own draw, for these flags.
struct DrawReissues {
    bool uiDepthCoverage = false;   // the family reissue (still needs uiDepthWantsReissue() and a true Begin)
    bool hologram = false;          // the hologram contribution and element-depth passes
    bool planet = false;            // the planet coverage (still needs a true Begin)
    bool worldScreen = false;       // the in-world screen coverage (still needs a true Begin)
};

// `layered`: the UI layer took the draw. `owner`: the draw is the owner context's.
inline DrawReissues drawReissuesFor(const DrawCallFlags& f, bool owner, bool layered) {
    DrawReissues r;
    r.uiDepthCoverage = !layered && f.uiDepth;
    r.hologram = !layered && f.holoDepth;
    r.planet = owner && f.planetPending;
    r.worldScreen = owner && !layered && f.worldScreenPending;
    return r;
}

}  // namespace edvr
