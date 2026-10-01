// The splash's curve: which composite draws are the game's own world-space panel, and what the strip that bends them takes.
//
// WHAT THIS IS FOR. Elite draws the intro movie, the splash still and the main menu's loops with ONE composite: a six-index quad
// (DrawIndexedInstanced(6, 1), VS EF103A7CB4A8369A) that places itself entirely through the 80 bytes in VS b2 (see
// docs\shaders\intro-composite-vs.asm). The movie's constants are screen-space and head-locked: intro_panel.cpp swaps in a world
// placement of its own and bends that itself. The splash's constants are the GAME's world-space placement, which EDVR never touches
// (docs\intro-video.md).
// With fix.panel_curvature above 0 the splash bends like the on-foot screen: vscreen.cpp draws it as the surface strip
// (panel_curve.h, panelCurveSurfaceDraw) in place of the game's flat quad. The strip takes two numbers from its caller -- the panel's
// half-width in metres and which way its placement matrix moves +z -- and this module is where they come from.
//
// HOW IT KNOWS. Nothing is read off the draw but its shape. The constants are on the GPU, so this does what intro_panel.cpp has flown:
// the first time a (VS b2 buffer, PS slot 0 view) pair is seen, copy its 80 bytes to a staging buffer, and read them kSettleFrames
// frames later, when the copy cannot stall the render thread. intro_curve_math.h says what 20 floats must look like to be a world-space
// panel and what to take from them (introReadWorldCb). A pair that reads as one is `world` and its draws are handed to the strip from
// then on; anything else is `flat` and stays as the game drew it, with the reason and the 20 floats in the log for the next flight.
//
//   per pair:   (first seen) -> copying -> world
//                                      \-> flat -> (copied again every kFlatRecheckFrames) -> world | flat
//
// Until a pair is learned its draws are the game's own: a few frames of flat, never a missing screen. A new constants buffer or a new
// surface is a new pair and is learned afresh (the cut from the movie to the splash is expected to re-create both; if it does not, the
// re-read below is what catches the change). The table holds 16 pairs, the least
// recently drawn pushed out when a 17th arrives; a pair not drawn for kExpireFrames (180: three seconds at 60 Hz, two at 90) frames is
// forgotten, so an address the game freed and reused is learned again instead of inheriting a verdict. Staging buffers are released the
// moment they are read, on eviction, on expiry, at retirement and at shutdown.
//
// A FLAT PAIR IS READ AGAIN. The game reuses its per-eye constant buffers and its surfaces, and nothing says the cut from the movie to the
// splash gives them new identities: a pair first read as screen-space would then stay flat for ever and the splash would inherit it. So a
// flat pair whose last copy is kFlatRecheckFrames (60) frames old is copied again at its next draw -- the same copy as the first, issued
// where the buffer is bound -- and read back and judged exactly as a first read is. The pair STAYS flat while that copy is pending, so
// nothing changes until a read says otherwise and a re-read never flickers a draw. Read back as a world-space panel it is settled as
// world with the same line a first learn writes (and counts as learned; a world line is never capped, a flat line is); read back flat
// again it is silent, or one line when the reason is of a different class from the last one said for that pair. The cost is bounded: at
// most one staging buffer a pair, one re-read a pair per kFlatRecheckFrames drawn frames, and none of it at fix.panel_curvature 0.
//
// KNOWN LIMIT: A WORLD VERDICT IS FOR THE LIFE OF THE PAIR. A world pair is never read again, so a world pair the game later reuses for
// another placement (the same buffer and view, other constants) keeps its first reading -- the strip's old half-width and direction --
// until the pair is forgotten (evicted, or unseen for kExpireFrames). Nothing in the intro is known to do that.
//
// WHAT IT NEVER CLAIMS. The movie (its placement is intro_panel's own, and a draw intro_panel claims is drawn before this is asked: the
// wiring puts this right after that claim. The few settle frames before intro_panel has a placement of its own do reach this, and the
// movie's stock constants read screen-space and flat there, said once); the on-foot HUD (a different VS; and the intro retires at the first
// rendered scene, the signal introPanelTick uses); the loader dialogs (the game's own curved mesh, not six indices). At fix.panel_curvature
// 0 NOTHING in here runs: introCurveWants() is false and no staging buffer is created, no copy issued, no line logged.
//
// FAULTS. A fault in here stands THIS module down for the session -- its own fault budget and its own log line -- and every composite is
// then drawn as the game drew it. It never touches the strip's stand-down (panel_curve.h) or any other module's.
//
// THE WIRING (vscreen.cpp). In beginPanelOverride's eye branch, right AFTER the movie's own claim (a draw the movie claims is never asked),
// a flag and not a verdict, so it composes with kBackdrop's slot swap and the splash dim:
//
//     if (kind == 'X' && count == 6 && introCurveWants()) s->introCurveThisDraw = introCurveOnComposite(self, kind, count, instances);
//
// then in forwardWithVerdict, after the verdict's Begin and in place of the game's own issue:
//
//     panelCurveSurfaceDraw(self, introCurveGain(), introCurveToward(), realDraw);   // false: the game's own quad is issued, flat
//
// the splash dim's re-issue draws the strip again with the same two numbers, and the thunk puts them away after the draw and the dim:
// introCurveEndDraw(). Once a frame, from the frame boundary: introCurveTick(ownerCtx, eyeDrawsLastFrame >= kSceneEyeDraws).
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
#ifdef EDVR_INTRO_CURVE_RIG
struct ID3D11Buffer;
#endif

namespace edvr {

// The intro composite's vertex shader, by content hash (docs\shaders\intro-composite-vs.asm; the draw census line `vh EF103A7CB4A8369A`).
constexpr uint64_t kIntroCompositeVsHash = 0xEF103A7CB4A8369Aull;

// The strip is asked for (fix.panel_curvature above 0, live, and the strip has not stood down) and this module has neither retired nor
// stood down. Cheap, and the only thing that runs at curvature 0.
bool introCurveWants();

// One eye draw of the composite shape: kind 'X', 6 indices, 1 instance, the VS above bound (bindingShaderHash(BindSlot::Vs)). Finds or
// starts the pair's entry; true = THIS draw is a learned world-space panel and is to be drawn as the surface strip, with introCurveGain()
// and introCurveToward() as the strip's arguments. False for everything else, including every draw of a pair that is still being read.
// Any call first clears the armed state, so a false leaves nothing armed.
bool introCurveOnComposite(ID3D11DeviceContext* ctx, char kind, uint32_t count, uint32_t instances);

// For the armed draw: the panel's half-width in metres (the snapshot's cb2[0].x) and the direction of +z' (+1 toward the viewer). Zero
// when nothing is armed.
float introCurveGain();
int introCurveToward();

// Disarm. vscreen calls it after the draw (and after the splash dim's re-issue of it).
void introCurveEndDraw();

// Once a frame, from the frame boundary: counts the frame, reads back the copies that have settled, forgets pairs not drawn for
// kExpireFrames, and on the first rendered scene (sceneFrame) retires this for the session -- the same scope rule intro_panel.h states.
void introCurveTick(ID3D11DeviceContext* ctx, bool sceneFrame);

// Releases everything held. From the vScreen shutdown, the only place that knows the device is still alive.
void introCurveShutdown();

// What the retirement line says, and what a rig reads.
struct IntroCurveInfo {
    bool wanted = false;       // introCurveWants() now
    bool retired = false;      // a rendered scene arrived: the intro is over
    bool standDown = false;    // a fault stood this module down for the session
    uint32_t entries = 0;      // pairs in the table now
    uint32_t copying = 0;      // of those, waiting for their readback
    uint32_t worlds = 0;       // of those, read as world-space panels (armed)
    uint32_t flats = 0;        // of those, read as anything else (left as the game drew them)
    uint32_t staging = 0;      // readback buffers held now
    uint64_t learned = 0;      // pairs read back since the session began, worlds and flats, cumulative (a flat pair read as world counts again)
    uint64_t rereads = 0;      // flat pairs' copies read back after the first, cumulative
    uint64_t flatToWorld = 0;  // of those re-reads, the ones that changed a flat verdict to world
    uint64_t armed = 0;        // composite draws handed to the strip, cumulative
    uint64_t evicted = 0;      // pairs pushed out of a full table
    uint64_t expired = 0;      // pairs forgotten after kExpireFrames unseen
};
IntroCurveInfo introCurveInfo();

#ifdef EDVR_INTRO_CURVE_RIG
// A rig's doors and nobody else's (tools\intro_curve_module_test). The module keeps process-wide state -- a retirement and a fault
// stand-down are both for the session -- so a rig that wants each case from a fresh start resets it here, and the readback buffer the
// table holds for a pair (null when it has none) is handed out with no reference added, so the rig can take its own and see the
// module's go.
ID3D11Buffer* introCurveStageForTest(void* buffer, void* surface);
void introCurveResetForTest();
#endif

}  // namespace edvr
