#pragma once
// TEMPORARY, a Phase 1 shadow instrument (built 2026-10-27; draw_fast_path_shadow.h, docs\engine-render-performance-2026-09-19.md "Desk design: the
// repeat-draw fast path"). The draw gate's subscriber list as a set: one bit per term of vscreen.cpp's drawGateSubscribed, in its order, each term read
// exactly as that function reads it. The gate's own answer is still drawGateSubscribed, unchanged; this is the same list with the identities kept, so
// the shadow can ask whether every subscriber that wants draws is one the fast path has certified (fastshadow::certifiedOk).
//
// It lives apart from vscreen.cpp so the rigs that pin drawGateSubscribed's text keep their anchors. The two lists must agree term for term:
// tools\aa_off_test reads both and fails the build when a call or a State field appears in one and not the other, so a subscriber added to the gate
// and forgotten here cannot ship. Every getter is a pure read of module state (the gate's own comment says so), which is what lets this ask all of
// them where the gate stops at the first that answers true. Called once a frame and on a live settings change, never per draw.
#include <cstdint>

#include "backdrop_fix.h"
#include "blend_probe.h"
#include "depth_probe.h"
#include "draw_census.h"
#include "draw_fast_path_shadow.h"
#include "fss_panel.h"
#include "fss_res.h"
#include "holo_fix.h"
#include "intro_curve.h"
#include "intro_panel.h"
#include "loader_panel.h"
#include "night_vision.h"
#include "object_probe.h"
#include "panel_curve.h"
#include "particle_fix.h"
#include "quad_probe.h"
#include "remlok_fix.h"
#include "scrim_fix.h"
#include "sunglare_fix.h"
#include "target_sharp.h"
#include "ui_depth.h"
#include "vscreen_footprint.h"
#include "wake_pulse.h"

namespace edvr {

// The four terms of drawGateSubscribed that read vScreen's own State, as the gate reads them.
struct DrawGateState {
    bool distance;      // s->distanceEnabled
    bool censusSkip;    // s->censusSkipCount != 0 || s->censusSkipRangeCount != 0 || s->censusSkipOffCount != 0
    bool quadSkip;      // s->quadSkipArmed
    bool censusAuto;    // s->censusAutoW != 0
};

inline uint32_t drawGateSubscribers(const DrawGateState& st) {
    using namespace fastshadow;
    uint32_t m = 0;
    if (st.distance) m |= kSubDistance;
    if (st.censusSkip) m |= kSubCensusSkip;
    if (st.quadSkip) m |= kSubQuadSkip;
    if (st.censusAuto) m |= kSubCensusAuto;
    if (fssResActive()) m |= kSubFssRes;
    if (fssPanelWantsDraws()) m |= kSubFssPanel;
    if (remlokWantsDraws()) m |= kSubRemlok;
    if (holoWantsDraws()) m |= kSubHolo;
    if (targetSharpWantsDraws()) m |= kSubTargetSharp;
    if (uiDepthWantsDraws()) m |= kSubUiDepth;
    if (sunglareWantsDraws()) m |= kSubSunglare;
    if (drawCensusArmed()) m |= kSubCensus;
    if (blendProbeWantsDraws()) m |= kSubBlendProbe;
    if (objectProbeWantsDraws()) m |= kSubObjectProbe;
    if (panelCurveWants()) m |= kSubPanelCurve;
    if (particleWantsDraws()) m |= kSubParticles;
    if (backdropWantsDraws()) m |= kSubBackdrop;
    if (scrimWantsDraws()) m |= kSubScrim;
    if (quadProbeWants()) m |= kSubQuadProbe;
    if (loaderPanelWants()) m |= kSubLoaderPanel;
    if (introPanelWants()) m |= kSubIntroPanel;
    if (wakePulseWantsDraws()) m |= kSubWakePulse;
    if (nightVisionWantsDraws()) m |= kSubNightVision;
    if (spaceDustHidden()) m |= kSubSpaceDust;
    if (witchspaceStarsHidden()) m |= kSubWitchspaceStars;
    if (depthProbeWanted()) m |= kSubDepthProbe;
    if (vscreenFootprintWanted()) m |= kSubFootprint;
    if (introCurveWants()) m |= kSubIntroCurve;
    return m;
}

}  // namespace edvr
