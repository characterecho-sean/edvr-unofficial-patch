// The flight HUD's holographic grain, held steady.
//
// WHAT IT IS (measured 2026-09-02, from the shader's own disassembly)
//
// The flight HUD -- the altimeter ladder, the speed and altitude readouts,
// the surface coordinates -- is VECTOR geometry: vs B7790CBFC6554097 takes
// POSITION plus eight TEXCOORD streams at stride 96 and draws straight into
// the eye. It is already rasterised at full eye resolution, so there is no
// low-resolution source anywhere in it and nothing an upscaler could
// improve. That was worth establishing, because the obvious reading of "the
// HUD text looks soft" is that it is being magnified from something, and it
// is not.
//
// What its pixel shader (8DEF46452FA459F5) also does is lay THREE OCTAVES OF
// VALUE NOISE over it, using the 256x256 texture at slot 1 as the lookup
// table. From the disassembly, unmistakably:
//
//   mul r8.xy, r8.xy, l(0.003906, ...)         1/256 -- a table, not a page
//   mad r8.zw, r9.zzzz, l(0,0,37.0,17.0), ...  37 and 17, hash offsets
//   frc / round_ni / t*t / (3 - 2t)            smoothstep, value-noise lerp
//   ... sampled three times, weighted 1.0, then 0.5, then 0.25
//
// So the softness the field reports on HUD text is GRAIN, not blur. The
// glyph edges are as sharp as the headset can show; what sits over them is
// a procedural shimmer.
//
// THE FIX is holo_fix's exactly, and for the same reason: for these matched
// draws, substitute slot 1 with a uniform 1x1 texture so every octave reads
// the same value and the noise term becomes a constant. The game's shader is
// untouched, and its texture is restored immediately after.
//
// THE LEVEL IS A TASTE CONTROL, not a correctness one. The noise drives a
// saturated smoothstep, so a uniform does not have one provably right value
// the way holo_fix's multiply did -- 255 means the HUD is drawn at the
// intensity the noise's peak would have given it. Lower it if flat reads too
// bright. And this grain is presumably deliberate styling: steady may look
// wrong to you rather than better, which is why stock is the default.
//
// Off by default, and free when off.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads experimental.hud_grain (stock | steady), advanced.hud_grain_level
// and the shader pin advanced.hud_grain_vs. Install and reload; both live.
void hudGrainConfigure(Config& cfg);

// False in stock mode, which keeps the eye-draw path free.
bool hudGrainWantsDraws();

// Is this eye draw one of the flight HUD's? Slot 1 being the 256x256 noise
// table and slot 0 an eye-sized depth resolve, then the vertex shader's
// hash -- cheapest test first, the hash last because it costs a call.
bool hudGrainOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                       uint32_t instances);

// Around the real draw: bind the uniform into slot 1, restore the game's
// texture after. A begin that cannot build the substitute degrades to the
// draw running untouched.
void hudGrainBegin(ID3D11DeviceContext* ctx);
void hudGrainEnd(ID3D11DeviceContext* ctx);

void hudGrainShutdown();

}  // namespace edvr
