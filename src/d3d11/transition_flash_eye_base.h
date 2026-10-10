#pragma once
// The transition-flash ENGINE FIX (docs/design-transition-flash-engine-fix-
// 2026-09-23.md). The camera driver FUN_1428431d0 copies a 4x4 "mailbox" at
// ship+0x3330 into a local before composing the eye views and (mode != 1)
// resets it to the identity constant. The HMDCamera tick that refills it is
// absent on the frame after a camera switch (the camera is deactivated one
// frame and re-activated after the consume -- flight 050558), so that consume
// reads the reset value, the eye composes against the head pose alone, and the
// frame is drawn from the wrong place: the one-frame flash.
//
// fix.transition_flash arms this module (on by default). It then:
//   * hooks the consumer (build- and prologue-keyed; stands down and says why
//     on a mismatch) and arms an EVENT at the ENTRY of a run of reset
//     mailboxes (mode-2 consumes);
//   * at the camera-CB Unmap tap (glitch_frame.cpp hands it every fill while
//     an event window is open), on the bad renders (tap frames skip+1, and
//     skip+2 while the gap lasted), at the frame's first fill whose row 275 is
//     head-only, WITHHOLDS the frame (glitchFrameEngineFixEvent) -- one held
//     frame per transition, nothing written to the game's buffer, no game
//     function called (flights 091951 and 095137: correcting the frame in
//     place still flashed; holding it did not);
//   * at the frame boundary behind the held frame tells the temporal pass the
//     camera stayed (glitchFrameEngineFixVerdict), so its history is kept;
//   * logs one line per held frame and counts events, held frames and holds
//     the compositor could not honour.
// While armed the old transition-flash DETECTOR (glitch_frame.cpp) is
// dormant; when identity or the hook fails, doInstall reports it to the
// detector (glitchFrameNoteEngineFix) and the detector runs as before.
//
// The pure logic (reset-mailbox compare, ENTRY/EXIT classifier, event window,
// head-only test) is in transition_flash_eye_base_core.h, which
// tools\transition_flash_prevent_test drives without the game.
#include <cstddef>
#include <cstdint>

namespace edvr {

class Config;

// Called on every config reload (vscreen.cpp, both call sites). The FIRST call
// reads fix.transition_flash: off, one log line and nothing installed; on, it
// installs the consumer hook and reports Armed or Lost to the detector exactly
// once. Later calls do nothing.
void transitionFlashEyeBaseConfigure(Config& cfg);

// Once a frame, from vscreen.cpp after glitchFrameBoundary. Publishes the
// frame number the consumer hook reads (it can run on a scheduler job thread,
// not necessarily this one), logs held frames, closes the event window, and
// owes the temporal pass its verdict once a held frame's eyes were handed over.
// Never call this from inside a game hook.
void transitionFlashEyeBaseFrameBoundary(uint32_t frameNo);

// The camera-CB fill tap: glitch_frame.cpp's glitchFrameObserve reads the
// 5376-byte scene CB pre-Unmap and calls this for every fill while an event
// window is open. `frame` is the detector's frame attribution (s->frameNo at
// Unmap time -- the PRE-advance counter, so a fill of the frame the boundary
// records as F carries F-1; the module compensates).
void transitionFlashEyeBaseNoteSceneCB(uint32_t frame, const void* mapped, size_t sizeBytes);

// FOR TESTS: arm the engine fix with no game module (the hook is not
// installed), and run the consume classifier on a synthetic mailbox -- what
// the consumer hook does with the 64 bytes it read. The glue rig drives
// consume -> event -> tap -> mark -> verdict through these.
void transitionFlashEyeBaseArmForTest();
void transitionFlashEyeBaseConsumeForTest(int32_t gameMode, const float mailbox[16]);
void transitionFlashEyeBaseDisarmForTest();
void transitionFlashEyeBaseCountersForTest(uint64_t* events, uint64_t* framesHeld, uint64_t* noHeadOnly,
                                           uint64_t* notHonoured);

// Final session summary. Mirrors the call site in device_hook.cpp's shutdown.
void transitionFlashEyeBaseShutdown();

}  // namespace edvr
