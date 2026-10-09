#pragma once
// The transition-flash engine fix, round 6 (docs/design-transition-flash-
// engine-fix-2026-09-23.md, "Static round 6: the eye's base is a
// consume-and-reset mailbox"). The camera driver FUN_1428431d0 copies a 4x4
// "mailbox" at ship+0x3330..+0x336F into a local before composing the eye
// views, then (mode != 1) resets the mailbox to an identity constant. Some
// writer has to refill it every frame; on the frame after a render-frame
// switch it is not refilled in time, so the eye composes against the head
// pose alone -- the flash.
//
// Static round 7 (design doc, "Static round 7: the writer and its gates")
// found that writer, FUN_142874b20, and its own name-gate skip. A DR1
// EXECUTE breakpoint at its entry (alongside DR0's existing write watch, one
// VEH, one lifecycle) captures what it was OFFERED whether or not the name
// gate let it through; when the writer was entered for our ship and refused
// by that gate, the offered matrix -- fresh, finite, not the reset value --
// is "the base the writer would have written" and takes priority over the
// held base. Otherwise the candidate is still the mailbox's OWN last
// known-refilled value, cached every refilled call and guarded call by call
// (age, ship pointer, finite, not itself the reset value, session cap)
// before anything acts on it -- see transition_flash_eye_base_core.h's
// offeredSubstituteUsable and heldBaseRefusal. ship+0x130 is still read and
// logged, for the record, but no longer gates or supplies the write.
//
// BUILD 2a (2026-10-09, "the supercruise fix"; docs/design-transition-flash-
// engine-fix-2026-09-23.md, "Flight 050558"): this module is now the fix
// itself, armed by fix.transition_flash (on by default), and the old
// advanced.transition_flash_eye_base switch (off | watch | on | alternate) is
// no longer read. Flight 050558 settled the mechanism: the HMDCamera is
// deactivated at S-1 and re-activated after the S consume, so the controller
// tick is absent exactly on the frame whose mailbox the consume reads, and the
// first tick that follows writes the base the engine meant. So:
//   * the consumer hook arms an event at the mode-2 ENTRY edge;
//   * the controller hook, right AFTER the original tick returns, reads the
//     mailbox as the event's base B_new (race-free with the consume's reset);
//   * at the render tap (bad frames skip+1, and skip+2 while the gap lasted),
//     B_new present -> the fills whose row 275 is the frame's head-only eye
//     get the eye-origin/view correction; B_new absent -> the frame is
//     WITHHELD (glitchFrameEngineFixEvent). The pool selector is not asked.
//   * one always-on log line per bad frame acted on.
// While the fix is armed the transition-flash DETECTOR (glitch_frame.cpp) is
// dormant; when identity or a hook fails, doInstall reports Lost
// (glitchFrameNoteEngineFix) and the detector runs exactly as before.
//
//   advanced.transition_flash_diagnostics = off | on   (TEMPORARY, default off)
//     on         the instrument: the writer watch (DR0/DR1), the call and
//                frame rings and their dumps, the passive patch-sim lines
//                (base1-> beside held-> new-> live->), the HMDCamera
//                lifecycle hooks and gap lines, and the pool comparison
//                logged as pool=<choice> beside each act. Off, none of it
//                runs and none of its storage is allocated.
//
// See transition_flash_eye_base_core.h for the pure logic (the bit-exact
// reset check, the M/F validation arithmetic, the base selector, the view
// locator and the correction math) -- it has no game or Windows dependency
// and is what tools\transition_flash_prevent_test drives. Deliberately a
// separate module from pose_reader_watch.cpp (and, until it was removed
// 2026-09-29, transition_flash_prevent.cpp): this file touches none of them.
#include "glitch_scene.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace edvr {

class Config;

// Called on every config reload (vscreen.cpp, both call sites, beside
// poseReaderWatchConfigure). The FIRST call reads fix.transition_flash: off,
// one log line and nothing installed; on, it installs the consumer and
// controller hooks (build- and prologue-keyed; each stands down on a mismatch
// and says why) and reports Armed or Lost to the detector exactly once.
// Later calls only move advanced.transition_flash_diagnostics, which is hot.
void transitionFlashEyeBaseConfigure(Config& cfg);

// Once a frame, from vscreen.cpp beside glitchFrameBoundary/
// poseReaderWatchFrameBoundary. Publishes the frame number the consumer hook
// reads (it can run on a scheduler job thread, not necessarily this one),
// runs the writer watch's ship-pointer stability gate and arms/re-arms/
// sweeps it, and services one deferred dump if its due frame has arrived.
// Never call this from inside a game hook.
void transitionFlashEyeBaseFrameBoundary(uint32_t frameNo);

// The existing transition-flash DETECTOR's scene-judged eye-camera-reset
// verdict (glitch_frame.cpp's kVerdictSceneReset, from each of its three
// ring-write sites -- the same tap pose_reader_watch.h's dump-trigger narrow
// uses), one of this file's two automatic dump triggers. The other (CHANGE
// 9: a mode-switch entry or exit edge, not every unrefilled consumer call --
// see transition_flash_eye_base_core.h's classifyModeSwitchEdge) is internal
// and needs no call from outside.
void transitionFlashEyeBaseNoteDetectorVerdict(uint32_t frame, bool sceneResetVerdict);

// The detector's own per-frame scene-camera tap -- glitch_frame.cpp's
// s->sceneDrawPos/e.scenePos/e.sceneValid, cb1[275], the same value
// recordScenePosition folds into its own ring entry. Computed whether or not
// advanced.eye_origin_trace is on (only the call-stack id beside it is
// trace-gated), so this module's own dump can show it regardless of that
// key. Called from the same three ring-write sites as
// transitionFlashEyeBaseNoteDetectorVerdict/transitionFlashEyeBaseFrameSnapshot,
// right after recordScenePosition; a read-and-reset per real frame, folded
// straight into this module's own per-frame dump row (no snapshot struct --
// nothing outside this file needs the value back).
//
// CHANGE 9 (2026-09-24, "the object side and the camera side of each frame
// on one line"): `geometry`/`decision` are glitch_scene.h's own per-frame
// measurement of the object pool matched against the camera -- the same
// value recordScenePosition folds into e.geometry, and glitchSceneDecision()
// classifies. `geometryFresh` is recordScenePosition's own freshness gate
// (the pool was actually compared this frame), handed back explicitly
// rather than inferred from an all-zero geometry, which a real coherent
// frame can also measure. CHANGE 12: a pending render-time patch sim also
// fires from here, on covered frames (see the .cpp) -- using this same pos
// as P and the geometry's own cameraStep/poolStep as the scene-old/new
// selector's inputs.
void transitionFlashEyeBaseNoteSceneCamera(uint32_t frame, const float pos[3], bool valid,
                                            const GlitchSceneGeometry& geometry, bool geometryFresh,
                                            GlitchSceneDecision decision);

// CHANGE 14/15 (2026-09-24): the scene-CB fill tap, beside NoteSceneCamera.
// glitch_frame.cpp's glitchFrameObserve reads the 5376-byte camera buffer
// pre-Unmap (the tee vscreen.cpp's hookedUnmap runs before forwarding) and
// calls this AFTER its own sceneWrites update, so the detector's per-fill
// record of row 275 -- the value its scene verdict referees with -- is
// always the ORIGINAL; on a patched event's act frame (CHANGE 15) this
// function then WRITES the correction into the same mapped buffer for the
// fills that carry the bad eye. Each fill is handed with the detector's own
// frame attribution (s->frameNo at Unmap time -- the PRE-advance counter, so
// a fill of the frame the boundary records as F carries F-1; the module
// compensates) and its per-frame pool measurement, the selector's inputs.
void transitionFlashEyeBaseNoteSceneCB(uint32_t frame, const void* mapped, size_t sizeBytes,
                                       const GlitchSceneGeometry& geometry, bool geometryFresh);

// This frame's consumer/writer activity, read once per ring-write site
// (glitch_frame.cpp's RingEntry) -- poseReaderWatchFrameSnapshot's read-and-
// reset convention. `writerMask` bit N: writer-table id N hit this frame;
// `mTranslation`/`fTranslation` are the LAST call this frame's mailbox and
// stand-in translations (NAN when no call happened this frame).
struct EyeBaseFrameSnapshot {
    uint16_t calls = 0;
    uint16_t unrefilledCalls = 0;
    uint8_t  treatment = 0;      // 0 none, 1 watched, 2 acted (this frame's last unrefilled call)
    uint32_t writerMask = 0;
    bool     writerSinceLastConsume = false;
    bool     shipChanged = false;
    float    mTranslation[3] = {NAN, NAN, NAN};
    float    fTranslation[3] = {NAN, NAN, NAN};
};
EyeBaseFrameSnapshot transitionFlashEyeBaseFrameSnapshot();

// Final session summary. Mirrors poseReaderWatchShutdown's call site
// (device_hook.cpp).
void transitionFlashEyeBaseShutdown();

}  // namespace edvr
