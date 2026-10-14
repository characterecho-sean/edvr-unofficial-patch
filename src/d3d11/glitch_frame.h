// The camera-buffer tap of the one-frame transition-flash fix.
//
// Jumping, dropping out of supercruise, or closing the galaxy map occasionally
// shows a single frame drawn from the wrong viewpoint -- a hard cut to somewhere
// else and straight back. On a monitor it is a blink. In a headset it reads as
// the world lurching, which is the part that hurts.
//
// The fix is the engine fix in transition_flash_eye_base.cpp: it knows from the
// game's own camera driver which frame will be drawn from a reset mailbox, and
// holds that frame back (the compositor keeps the previous one on screen). This
// file is the half that sees the frame: the game fills a 5376-byte scene
// constant buffer for every eye pass, vScreen's Map and Unmap hooks hand each
// fill here, and while an engine event's window is open the fill is passed on to
// the engine fix, which decides whether it is the bad one.
//
// It repairs nothing and writes nothing: it looks at a constant buffer the game
// has already filled. See docs/transition-flash.md for what is actually wrong.
//
// There used to be a second, independent detector in this file that judged the
// camera's jumps from the outside and was the fallback when the engine fix could
// not arm. It is gone (docs/transition-flash.md, 2026-10-13); a build the engine
// fix does not recognise now has no protection, and says so in the log.
#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr {

// Sets the tap up on the game's 5376-byte scene buffer (the camera origin at
// float 1100, both constants in this module, the fit checked at compile time) and
// says so. Reads no ini key. The test rigs switch the tap off with the
// environment variable EDVR_TEST_NO_CAMERA_TAP=1 (a transport-only install).
void installGlitchFrameFix();

// Does vScreen need its Map and Unmap hooks for this tap? True when the camera
// buffer is observed at all. Asked at hook install, before the engine fix has
// reported, so it cannot depend on that.
bool glitchFrameWantsHooks();

// The engine fix's report: armed when it installed its consumer hook and the
// tap below can deliver to it; not armed otherwise. (The tap only ever hands a
// fill over while the engine fix's own event window is open, which a fix that
// is not armed never opens.)
void glitchFrameNoteEngineFix(bool armed);

// Can the tap deliver (the camera buffer is observed and is the game's 5376-byte
// scene buffer)? Checked before Armed is reported; `why` says what is missing
// when not.
bool glitchFrameEngineTapAvailable(char* why, size_t whySize);

// The engine fix acted on a frame it found bad: always notes the camera jump
// (the FSS arrival trigger keys on it) and marks the frame so the compositor
// does not show it. Returns whether a compositor was in a position to honour
// the mark (frame_flag.h's glitchConsumerPresent).
bool glitchFrameEngineFixHold();

// The temporal pass's verdict for an engine hold: "the camera stayed". Call it
// AFTER the held frame's eyes were handed to the compositor (the frame boundary
// after the mark), never at the mark itself: the pass latches the verdict word
// when the eye is withheld and reads a CHANGE since as the verdict, so a word
// published before the latch reads as no verdict at all and ends in a history
// reset (flight 091951).
void glitchFrameEngineFixVerdict();

// The engine fix's event window (armed event .. its two bad renders): while it
// is open the camera-CB tap runs; closed, glitchFrameObserve returns at once.
void glitchFrameEngineWindow(bool open);

// Called from the Map/Unmap hooks. The tap picks out the buffer it cares about
// by size, so passing it everything is intended. The nomination also serves the
// other readers of the same block (the sun-glare fix, the temporal pass), so it
// does not depend on the engine fix being armed.
bool glitchFrameWantsBuffer(uint32_t bytes);
void glitchFrameObserve(const void* data, uint32_t bytes, const void* resource);

// glitchFrameIsSceneDraw is a pure classifier of five rigid-scene vertex shader
// hashes, defined here inline: the draw lambda asks it for every eye geometry
// draw (the temporal pass's rigid-draw provenance wants it), and as a cross-TU
// call (/O2, no /GL) that was a call per draw for five compares.
inline bool glitchFrameIsSceneDraw(uint64_t vertexShaderHash) {
    return vertexShaderHash==0xEB5234DB6ADB491Dull || vertexShaderHash==0xDE545DC8EE4FBB87ull ||
        vertexShaderHash==0x61AE8EB05FDC18DDull || vertexShaderHash==0x66DE2CADB1F4AE6Bull ||
        vertexShaderHash==0xAACFDCF2FB9AD809ull;
}

// Called once per frame, after Present: the frame counter the engine fix's
// window tests count in.
void glitchFrameBoundary();

void shutdownGlitchFrameFix();

}  // namespace edvr
