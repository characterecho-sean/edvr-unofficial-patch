// The deferred lighting resolve, replaced by a shader that answers a
// question instead of lighting the scene.
//
// WHERE THIS COMES FROM (2026-08-30, the black planet in the right eye)
//
// A planetary body renders as a featureless black disc in one eye and
// correctly in the other, on a stock game. The eye-split dump placed the
// loss precisely: the body's surface reaches BOTH eyes' geometry buffers,
// and one eye loses it during lighting. The single draw between those two
// states is the deferred resolve -- one full-screen quad, ps hash
// 7CECABDE34FFBE9E, reading four full-eye buffers.
//
// Its disassembly (docs/shaders/, and the write-up in docs/black-body.md)
// then gave the arithmetic. With this rig's measured constants substituted
// in -- cb2[46] = (1,1,1,0), cb2[36].x = 0 -- the shader's material branch
// collapses to
//
//     r4.x = r4.w = t2.w
//
// and BOTH the diffuse and specular terms are multiplied by it. A t2.w of
// zero produces a black surface with its geometry, depth and silhouette
// entirely intact, which is the symptom exactly. The other candidate is the
// flag byte in t1.w, whose bit 128 bypasses that path.
//
// WHY A REPLACEMENT SHADER AND NOT ANOTHER PROBE
//
// t1 and t2 have never been seen. They are MRT slots 1 and 2 of the terrain
// pass, and every instrument here reads render-target slot 0 -- the census
// (BindSlot::Rtv0) and eye_split alike -- so nothing has ever captured them,
// in any state. Rather than build a third capture path and hope it is
// pointed correctly, this asks the shader itself: it runs in the exact
// place, with the exact bindings, that the real one does.
//
//   white   the resolve emits a constant. The pass covers the whole screen,
//           so the screen goes white -- and any region that does NOT is a
//           region where the pixel shader's output is being rejected
//           downstream. That tests stencil, blend masks and predication in
//           one look, none of which the census records and the last of
//           which is invisible by construction.
//
//   inputs  the resolve emits (t1.w, t2.w, t3.r) as red, green, blue. The
//           body's disc showing green in one eye and not the other IS the
//           finding: t2.w is the multiplier above. Red carries the flag
//           byte, blue the shadow mask, so one look covers all three.
//
// Off by default and the only shipped state. Every failure -- no compiler,
// a compile error, a shader that will not create -- stands down and lets
// the game draw its own, which is shader_swap.h's standing contract.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

void resolveProbeConfigure(Config& cfg);

// One bool for the draw path's early-out set.
bool resolveProbeWantsDraws();

// Called for every eye draw while armed: true when this draw is the resolve,
// which is recognised by its PIXEL shader's content hash rather than by its
// vertex count or target, because the hash is the one key that cannot
// collide with another full-screen quad.
bool resolveProbeOnEyeDraw(ID3D11DeviceContext* ctx);

// Around the matched draw: swap the pixel shader in, and put the game's own
// back. End must run even when Begin declined, and does.
void resolveProbeBegin(ID3D11DeviceContext* ctx);
void resolveProbeEnd(ID3D11DeviceContext* ctx);

void resolveProbeShutdown();

}  // namespace edvr
