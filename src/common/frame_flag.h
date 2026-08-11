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
// nothing to either DLL's export table. It is per-session and holds three
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
// verdict is no. That is legitimate -- but it must not be counted. Reusing
// clearGlitchFrame() for it made the next mark in the same frame look like a
// fresh frame, and the session total read 1258 for four frames actually
// withheld.
void unmarkGlitchFrame();

// Called once per frame, so a mark applies to exactly one frame. Without this a
// single detection would suppress every frame that followed. This is also what
// re-arms the counter, which is why the mid-frame case above needs its own entry
// point rather than reusing this one.
void clearGlitchFrame();

}  // namespace edvr
