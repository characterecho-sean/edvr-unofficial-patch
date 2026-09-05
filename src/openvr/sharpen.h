// Render sharpening at the door -- the openvr half.
//
// This half decides; the d3d11 half sharpens (src/d3d11/sharpen_pass.h).
// The decision is one number, fix.render_sharpness, 0 (off) to 1
// (strongest), read at the compositor hook's install and from the reload
// poll -- a slider judged from inside a headset, so it is live. The
// treatment is the LAST one on every forwarding path of hookedSubmit,
// after the cull guard's crop and the supersample resolve, on whatever
// texture the passes before it produced: docs/anti-aliasing.md's order at
// the door, and the seam the resolve left marked for exactly this. It
// exists because the resolve's calm kernel and the temporal pass's history
// each trade a little edge contrast for calm, and the first temporal
// flight (Quest 3, 2026-09-03) found text a little soft without something
// to hand it back. Nothing is asked of the game; the frame is read, never
// written, and a copy EDVR owns is what goes out.
#pragma once

#include "openvr_min.h"

namespace edvr {

// Reads fix.render_sharpness. Called at the compositor hook's install and
// from the reload poll; a change says so once.
void sharpenConfigure();

// The strength is above zero and the pass has not stood down: the
// per-submit short-circuit, and one of the reasons the compositor hook
// installs.
bool sharpenWanted();

// The treatment. Called with the texture and bounds a path is about to
// forward; returns the sharpened texture (full-span bounds written to
// *outBounds, the original orientation kept) or null, meaning forward
// exactly what was passed in -- off, or stood down. Every failure inside
// stands the sharpening down loudly for the session.
void* sharpenTreat(vr::EVREye eye, void* handle,
                   const vr::VRTextureBounds_t* bounds,
                   vr::VRTextureBounds_t* outBounds);

// The compositor hook's lever for a submit-side failure: inert for the
// session, loudly.
void sharpenStandDown(const char* why);

// The session's totals, for the openvr log's own shutdown line.
void sharpenShutdown();

}  // namespace edvr
