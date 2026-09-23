#pragma once
// Transition-flash engine fix (docs/design-transition-flash-engine-fix-
// 2026-09-23.md). Elite's camera compose (FUN_1423bc8a0) resolves its
// parent node and decodes the parent's CACHED world pose
// (FUN_143cee4c0: a 128-byte copy of the parent's pool slot +0x100, no
// validity check). At a transition that block can be one frame stale --
// unwritten since the key re-targeted -- so the eye composes from the head
// pose alone: the one-frame "transition flash". The engine already has a
// pure from-root recompute (FUN_143cee650) that never races the write; when
// the two disagree, handing the compose the recompute is the fix.
//
// One advanced key carries both the read-only anatomy instrument (Phase 1)
// and the fix (Phase 2), so one flight exercises both:
//   advanced.transition_flash_prevent = off | watch | on | alternate
//     off        nothing installed. One log line.
//     watch      the four hooks are installed and compare; nothing is ever
//                written back to the game.
//     on         acts whenever the guards in transition_flash_prevent.cpp
//                allow it (validated, the disagreeing parent's own history,
//                the session cap).
//     alternate  transition EVENTS alternate watched, acted, watched,
//                acted... (the first event is watched) -- a field way to
//                see the fix's own effect against an adjacent untreated
//                event without a second flight.
// See transition_flash_prevent_core.h for the pure logic (classifier, the
// per-parent guard table, event grouping, the alternate latch and the ring
// window test) -- it has no game or Windows dependency and is what
// tools\transition_flash_prevent_test drives.
#include <cstdint>

namespace edvr {

class Config;

// Read on every config reload (vscreen.cpp's vScreenRefreshConfig). Off:
// nothing installed; a single "off" line is said once. The first call that
// sees a non-off value installs the four CodeHooks (build- and prologue-
// keyed; each stands down on its own mismatch and says why) and arms.
// Later calls only move the live mode among off/watch/on/alternate, which
// is hot -- no reinstall, matching "hot reload among watch/on/alternate is
// welcome if trivial" -- because the guard and event logic already reads
// the mode live on every call.
void transitionFlashPreventConfigure(Config& cfg);

// Once a frame, from vscreen.cpp's frame boundary beside
// glitchFrameBoundary(): publishes the frame number the hooks (which can
// run on a scheduler job thread, not necessarily the render thread) read
// from anywhere, and services one deferred dump request if its due frame
// has arrived. Never call this from inside a game hook.
void transitionFlashPreventFrameBoundary(uint32_t frameNo);

// H3 (design doc): glitch_frame.cpp's glitchFrameObserve already reads the
// scene camera each frame (cb1[275], the float b1 buffer) to feed the
// transition-flash DETECTOR; this reports the same read so it can be
// compared, in float precision, against the last pose this file's push
// hook (FUN_143d0cf10) recorded. `pos` is the three floats already read
// from the constant buffer at the configured offset.
void transitionFlashPreventNoteH3(uint32_t frame, const float pos[3]);

// The existing transition-flash DETECTOR's per-frame verdict
// (glitch_frame.cpp's RingVerdict, cast to uint8_t; kept as a raw byte here
// since the enum itself is private to that file), reported from each of its
// three ring-write sites -- including while fix.transition_flash = 0, when
// the detector keeps observing but never acts. `withheldClass` is true for
// kVerdictWithheld, kVerdictWithheldSepWould and kVerdictSceneReset --
// glitch_frame.cpp computes it locally, where the enum is in scope -- and is
// this file's dump trigger: a flight that never enables the engine fix
// still shows whether the old detector would have caught what this file saw
// happen inside the engine.
void transitionFlashPreventNoteDetectorVerdict(uint32_t frame, uint8_t verdict, bool withheldClass);

// The camera history key (device_hook.cpp's dumpCameraRing call sites,
// bound to Pause by default): dumps this file's own ring in the same
// gesture as the camera history dump, so both rings describe the same
// keypress.
void transitionFlashPreventDumpRing(const char* trigger);

// Final session summary. Mirrors shutdownGlitchFrameFix's call site
// (device_hook.cpp).
void transitionFlashPreventShutdown();

}  // namespace edvr
