// The supersample resolve at the door -- the openvr half of
// docs/anti-aliasing.md Feature A, passive mode.
//
// This half decides; the d3d11 half filters (src/d3d11/supersample_pass.h).
// Both sizes the decision needs live here already: the runtime's
// recommended eye size, captured by the system hook as the game asks for
// it (trueSizeW/H), and the size the game actually submits per eye, read
// off every Submit's texture and bounds. Their ratio is the supersampling
// in force, whoever set it -- Elite's HMD Quality above 1.0 is the usual
// source -- and the resolve keys on it directly: nothing new is asked of
// the game, no size is lied about, nothing is staged. Detect, resolve,
// submit.
//
// The decision is taken once per frame at the boundary, from what both
// eyes submitted during it (the cull guard's changed-size adoption
// discipline; supersample_math.h holds the state machine so a test can
// drive it), and applied through one treatment on EVERY forwarding path of
// hookedSubmit -- the game's own frame, the guard's crop, the theater's
// rendering, the withhold's shadow -- so no path ships an untreated frame
// while the resolve is engaged. Crop first, resolve second, sequentially;
// a fused dispatch is future work.
//
// experimental.supersample_resolve = off | auto | on, fix.supersample_filter =
// calm | crisp, experimental.supersample_width = radius in output pixels. All live.
#pragma once

#include <cstdint>

#include "openvr_min.h"

namespace edvr {

// Reads the three keys. Called at the compositor hook's install and from
// the reload poll -- the filter and its width are judged from inside a
// headset, so they must be live; the mode is live too, within what the
// install-time hook gate allows.
void supersampleResolveConfigure();

// The mode is not off and the resolve has not stood down: the per-submit
// short-circuit, and one of the reasons the compositor hook installs.
bool supersampleResolveWanted();

// The arm/disarm decision, at the frame boundary (WaitGetPoses), after the
// system hook's own so the recommendation it reads is this frame's.
void supersampleResolveFrameBoundary();

// The treatment. Called with the texture and bounds a path is about to
// forward; returns the resolved texture (full-span bounds written to
// *outBounds, the original orientation kept) or null, meaning forward
// exactly what was passed in -- the resolve is off, not engaged, or has
// stood down. Every failure inside stands the resolve down loudly for the
// session; never a silently untreated frame while claiming engagement.
void* supersampleResolveTreat(vr::EVREye eye, void* handle,
                              vr::EColorSpace colorSpace,
                              const vr::VRTextureBounds_t* bounds,
                              vr::VRTextureBounds_t* outBounds);

// The compositor hook's lever for a submit-side failure: the resolve goes
// inert for the session, loudly.
void supersampleResolveStandDown(const char* why);

// The session's totals, for the openvr log's own shutdown line.
void supersampleResolveShutdown();

}  // namespace edvr
