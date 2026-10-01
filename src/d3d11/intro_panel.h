// The intro movie's panel, resized.
//
// THE DEFECT, measured across seven flights and finally read out of the
// game's own shader (docs/intro-video.md). Elite plays its launch movie on a
// quad composited into each eye, and that quad is placed by the vertex
// shader's CONSTANT BUFFER 2 -- five float4s, one scale and one transform:
//
//   cb2[0] = 512, 288, 0, 0            the panel's half-size, in PIXELS
//   cb2[1] = -1/2712, 0, 0, 0          pixels to NDC: 2712 = eye width / 2
//   cb2[2] = 0, 1/2678, 0, 0           2678 = eye height / 2
//   cb2[3] = 0, 0, 0, 0
//   cb2[4] = 0.1939, 0, 0, 1           straight ahead in an asymmetric
//                                      frustum; w is a CONSTANT 1
//
// So the movie is 1024x576 pixels in a 5424x5356 eye -- 27 degrees by 15 --
// and because w never varies there is no perspective divide: it is a screen
// -space blit, head-locked by construction rather than by accident. The
// splash that follows it uses the SAME shader and the SAME draw with a
// world-space matrix in the same buffer, which is why the movie cannot tie
// into it however it is sized.
//
// WHAT THIS DOES. Stage one only: multiplies cb2[0].xy, so the panel grows
// about the point it already sits on -- straight ahead, both eyes, aspect
// preserved. It does not move it into world space, so it stays head-locked
// and the cut to the splash still will not line up. That is stage two, it
// needs a view-projection from the movie's own frames, and it is not this.
//
// HOW THE CONSTANTS ARE READ. Not from the game's writes: the buffer is
// filled before any instrument can arm and never rewritten, which is what
// made the census's write tee report "unwritten" for two flights. It is
// copied off the GPU once per buffer and read back a few frames later --
// valid precisely because the contents are static, which was measured, not
// assumed.
//
// WHAT IT WILL NOT TOUCH. The splash, the menu and every world-placed panel
// fail the structural test below by construction: cb2[3] is non-zero and
// cb2[4].w is not 1 for anything that goes through a perspective divide. A
// buffer that does not read as screen-space is refused, said once, and left
// alone for the session.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads fix.intro_video (screen | stock -- this module's slices are the
// size and the world anchor). Install and reload; live, though the panel
// only exists during the intro.
void introPanelConfigure(Config& cfg);

// False at size 1.0, which keeps the per-draw path free in the shipped
// state, and false once the intro is over.
bool introPanelWants();

// The YUV-to-RGB fill that writes the movie into its source surface: a
// four-vertex draw with three planes bound. Marks the frame as one the movie
// is playing in, and records the surface size the composite must sample for
// this frame's composite to be the movie's rather than anything else's.
void introPanelNoteFill(uint32_t targetW, uint32_t targetH);

// One eye-texture draw. True means our constants are bound at VS b2 and the
// caller must call introPanelEndDraw after forwarding the draw.
bool introPanelOnComposite(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                           uint32_t instances, uint32_t srvW, uint32_t srvH);

// THE CURVED MOVIE (fix.panel_curvature above 0; docs\intro-video.md, 2026-10-01; panel_curve.h panelCurveSurfaceDraw is what draws it).
// The world-locked panel's unit quad is replaced, for one draw, by the strip the on-foot screen's arc makes; this file places it. At
// curvature above 0 -- and while the surface strip is not stood down -- the world constants it binds carry the z column in cb2[3]: the
// view-space image of the seated +z axis (the direction from the panel toward the viewer), unit length, which is what the strip's z' is
// measured along. At curvature 0, in the stock and head-locked modes, in the settle frames, when the lock is refused and when the surface
// strip has stood down, cb2[3] stays exactly zero, as it always was. There is no test of the bent edges against the eye, on purpose: D3D
// clips what is behind the eye in homogeneous space, so the part of a bent panel beside or behind the eye (nobody can see it) is clipped off
// by itself, and a test would pop the whole bend off at about 20 degrees of head yaw, differently in the two eyes near its threshold. Only
// the panel's centre is tested, as before, because a centre behind the eye is a silent total vanish.
//
// introPanelStripArmed: true from a successful bind of those constants (introPanelOnComposite returned true) until introPanelEndDraw --
// the caller, between the two, draws the strip with panelCurveSurfaceDraw(ctx, introPanelStripGain(), +1, ...) and, when that returns true,
// swallows the game's own quad; when it returns false it forwards the game's draw, which is flat and in the right place (a z column does
// nothing to a quad whose z is zero). False in every other case. introPanelStripGain: the depth gain in metres, the panel's half-width.
// +1 is the direction: a step in +z' moves toward the viewer, the same convention the splash's own constants read as.
bool introPanelStripArmed();
float introPanelStripGain();

// Restore the game's own constant buffer. Always paired with a true above.
void introPanelEndDraw(ID3D11DeviceContext* ctx);

// Frame edge: retire a settled readback into built constants, and close the
// frame's fill marker. sceneFrame is the caller's scene boundary -- the
// first rendered scene means the intro is over and this stands down for the
// session, the same scope rule loader_panel.h states.
void introPanelTick(ID3D11DeviceContext* ctx, bool sceneFrame);

void introPanelShutdown();

}  // namespace edvr
