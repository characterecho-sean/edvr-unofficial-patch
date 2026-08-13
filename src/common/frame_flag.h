// GENERATED from src/common/frame_flag.h in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 95719844830959cd]
// A one-bit channel between the two proxies, for the frame that must not be
// shown.
//
// The bad frame is detected in d3d11.dll, part-way through rendering, by
// watching where the game is drawing from. The decision it feeds -- whether to
// hand that frame to the headset -- belongs to openvr_api.dll. Those are
// separate modules loaded from different directories, so they cannot share a
// global.
//
// A named shared mapping is used rather than an exported symbol because it does
// not care which module loads first, works if one of them is absent, and adds
// nothing to either DLL's export table. It is per-process and holds two
// integers.
//
// Nothing here reads or writes game state. It carries one flag between two parts
// of EDVR.
#pragma once

#include <cstdint>

namespace edvr {

// Mark the frame in progress as one that should not reach the headset. Cheap and
// safe to call from the draw path.
void markGlitchFrame();

// True if the frame in progress has been marked.
bool glitchFrameMarked();

// Withdraw a mark WITHIN the frame that made it.
//
// The detector re-decides several times per frame, as each new camera candidate
// arrives, and an early candidate can look like a jump while the frame's final
// verdict is no.
//
// Identical to clearGlitchFrame() today, and kept as its own name because the
// two mean different things to a reader: one is "I was wrong", the other is
// "this frame is over". They used to differ -- a `counted` field distinguished
// them, so a withdrawn mark would not be tallied twice -- but that total moved
// into glitch_frame.cpp and the field became write-only. It has been removed
// rather than left as a mechanism the header describes and nothing implements.
void unmarkGlitchFrame();

// Called once per frame, so a mark applies to exactly one frame. Without this a
// single detection would suppress every frame that followed.
void clearGlitchFrame();

// Announced by openvr_api.dll once its hook is validated, and read by d3d11.dll.
//
// The two halves install separately and the openvr one is optional, so d3d11 can
// detect a bad frame and have nothing on the other end to act on it. Without
// this the log would report "12 frames withheld" to somebody who skipped the
// second file and had none withheld at all -- which is exactly the kind of
// counter that gets believed and wastes a day.
void announceGlitchConsumer();
bool glitchConsumerPresent();

// The player is on foot in the external camera, having arrived there from the
// flat panel -- the one state where moving the head pose is wanted.
//
// Set by d3d11.dll, which is the only half that can tell the modes apart: it
// watches the panel composite, and the panel stopping while a full scene is
// drawn into the eyes is what the transition looks like. Read by
// openvr_api.dll, which is the only half that can act on it, because the head
// pose passes through there. Neither can do the other's job, which is why this
// is a channel rather than a local.
//
// UNLIKE the glitch flag, this one is a STATE and persists across frames. It is
// not cleared at the frame boundary; it is cleared when the panel comes back.
void setExternalCameraOnFoot(bool on);
bool externalCameraOnFoot();

}  // namespace edvr
