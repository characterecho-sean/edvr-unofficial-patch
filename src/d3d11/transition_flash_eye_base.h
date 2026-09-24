#pragma once
// The transition-flash engine fix, round 6 (docs/design-transition-flash-
// engine-fix-2026-09-23.md, "Static round 6: the eye's base is a
// consume-and-reset mailbox"). The camera driver FUN_1428431d0 copies a 4x4
// "mailbox" at ship+0x3330..+0x336F into a local before composing the eye
// views, then (mode != 1) resets the mailbox to an identity constant. Some
// writer has to refill it every frame; on the frame after a render-frame
// switch it is not refilled in time, so the eye composes against the head
// pose alone -- the flash. The candidate written back is the mailbox's OWN
// last known-refilled value, cached every refilled call and guarded call by
// call (age, ship pointer, finite, not itself the reset value, session cap)
// before anything acts on it -- see transition_flash_eye_base_core.h's
// heldBaseRefusal. ship+0x130 is still read and logged, for the record, but
// no longer gates or supplies the write.
//
//   advanced.transition_flash_eye_base = off | watch | on | alternate
//     off        nothing installed. One log line.
//     watch      the consumer hook and the writer watch run and compare;
//                nothing the game sees is ever changed.
//     on         writes the validated stand-in into the mailbox on an
//                unrefilled call, before the original runs.
//     alternate  transition EVENTS alternate watched, acted, watched,
//                acted... (the first is watched).
// See transition_flash_eye_base_core.h for the pure logic (the bit-exact
// reset check, the M/F validation arithmetic, the act guards, the consumer's
// own extent) -- it has no game or Windows dependency and is what
// tools\transition_flash_prevent_test drives. Deliberately a separate module
// from transition_flash_prevent.cpp/pose_reader_watch.cpp: neither of those
// is touched by this file (both are left exactly as they were).
#include <cmath>
#include <cstdint>

namespace edvr {

class Config;

// Read on every config reload (vscreen.cpp, both call sites, beside
// transitionFlashPreventConfigure/poseReaderWatchConfigure). Off: one log
// line, nothing installed. The first call that sees a non-off value installs
// the consumer hook (build- and prologue-keyed; stands down on its own on a
// mismatch and says why) and arms; later calls only move the live mode --
// hot among watch/on/alternate, matching transition_flash_prevent's own
// reload discipline. The writer watch arms and disarms on its own schedule
// from transitionFlashEyeBaseFrameBoundary, not from here.
void transitionFlashEyeBaseConfigure(Config& cfg);

// Once a frame, from vscreen.cpp beside transitionFlashPreventFrameBoundary/
// poseReaderWatchFrameBoundary. Publishes the frame number the consumer hook
// reads (it can run on a scheduler job thread, not necessarily this one),
// runs the writer watch's ship-pointer stability gate and arms/re-arms/
// sweeps it, and services one deferred dump if its due frame has arrived.
// Never call this from inside a game hook.
void transitionFlashEyeBaseFrameBoundary(uint32_t frameNo);

// The existing transition-flash DETECTOR's scene-judged eye-camera-reset
// verdict (glitch_frame.cpp's kVerdictSceneReset, from each of its three
// ring-write sites -- the same tap pose_reader_watch.h's dump-trigger narrow
// uses), one of this file's two automatic dump triggers. The other (any
// unrefilled consumer call) is internal and needs no call from outside.
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
void transitionFlashEyeBaseNoteSceneCamera(uint32_t frame, const float pos[3], bool valid);

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

// Final session summary. Mirrors transitionFlashPreventShutdown's and
// poseReaderWatchShutdown's call site (device_hook.cpp).
void transitionFlashEyeBaseShutdown();

}  // namespace edvr
