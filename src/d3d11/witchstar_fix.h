// The witchspace destination star, pinned to the tunnel instead of the head.
//
// During a hyperspace jump the destination star is a 2D sprite composited at
// a fixed IMAGE position -- head-locked in VR, like the loading hologram's
// pattern, except this one should not be removed: it is the thing you are
// flying toward. The correct behaviour for a star is to hold a DIRECTION.
// A star sits at optical infinity, so zero stereo disparity (which the
// sprite already has) is right, head translation producing no parallax is
// right, and the only wrong part is that it follows head rotation.
//
// Found by census and position bisection (2026-08-19): the star and its
// corona are the LARGE members -- 120 to ~5,000 indices, radial glow fans --
// of the depth-clipped HUD material family (PS slot 0 the eye-sized depth
// resolve, slot 1 a 1024x1024 atlas), drawn as one contiguous cluster per
// eye with small flare quads interleaved. The cluster's position jitters a
// few draws frame to frame, which is why a fixed position range flickered
// it; the matcher here follows shape, not position.
//
// THE FIX: the openvr half publishes, every frame, where the ship's forward
// axis points in the current head frame (frame_flag headForward). For every
// draw in the star cluster, the viewport is substituted with one shifted by
// that offset -- converted to pixels through the same published frustum the
// RemLok fix uses -- and restored after. Turn your head and the star stays
// on the tunnel's axis, like a thing that is actually far away.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Read fix.witchstar (stock | pinned) and the two sign knobs. Both config
// paths, live.
void witchstarConfigure(Config& cfg);

// False in stock mode; the per-draw path costs nothing then.
bool witchstarWantsDraws();

// Every eye draw while enabled, match or not: the cluster is recognised as
// a RUN -- armed by a family member with a large index count, extended
// while family members stay contiguous, broken by anything else -- and the
// run state needs to see the breaks. Returns true when THIS draw belongs
// to the star cluster and should be wrapped in witchstarBegin/End.
bool witchstarOnEyeDraw(char kind, uint32_t count, uint32_t instances);

// Around the real draw: substitute a viewport shifted by the published
// head-forward offset; restore after. Begin failing to get its inputs
// (nobody publishing, no viewport) degrades to the draw running untouched.
void witchstarBegin(ID3D11DeviceContext* ctx);
void witchstarEnd(ID3D11DeviceContext* ctx);

// Frame edge: run state and per-frame accounting reset.
void witchstarFrameBoundary();

}  // namespace edvr
