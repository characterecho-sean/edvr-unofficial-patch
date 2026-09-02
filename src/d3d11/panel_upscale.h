// The cockpit's holographic panels, reconstructed instead of smeared.
//
// WHAT IT IS, and what it cost to find
//
// Every cockpit holo panel is painted by ONE shared family -- vs
// 81216C77F90DEDD6, 24 draws a frame, pixel shader A2965EC2931A39C8 -- and
// each draw reads a different INTERFACE SURFACE at sampler slot 2. The
// target direction indicator is on the surface that measures 0.4498 x
// 0.3726 of the render resolution: 2440x1996 on a 5424x5356 eye, 1952x1597
// on a 4340x4284 one.
//
// That surface is rebuilt EVERY FRAME -- 83 draws into it, measured: vector
// geometry (vs 666EF0C4C616F67E, whose pixel shader samples nothing at all),
// text from a 2048x2048 glyph atlas, and small BC7 icon pages. So its
// content is resolution-independent, and yet:
//
//   * it does not sharpen when render resolution rises, while its
//     neighbours do -- measured across a 3072x3264 eye and a 5424x5356 one;
//   * creating the surface at double size (fss_res.h's surface_inflate,
//     mechanism confirmed engaging: textures created, viewports scaled)
//     changed nothing visible.
//
// Both results say the same thing: the panel's content is laid out at a
// FIXED PIXEL SIZE inside the surface however large the surface is, so the
// draw magnifies it by the same factor at every resolution. The
// magnification is the ceiling, and it happens at that one sample.
//
// WHY THIS SUBSTITUTES A TEXTURE RATHER THAN SWAPPING THE SHADER
//
// The obvious move is target_sharp's: replace the pixel shader with one
// that reconstructs. Here that means transcribing ~470 instructions with a
// dynamically indexed 327-entry constant buffer, three textures and two
// samplers -- and it samples t2 in TWO places, one at a computed
// coordinate. A transcription that size is how a subtle colour error
// ships.
//
// So instead: resample the SURFACE with AMD's EASU (then RCAS) into a
// texture EDVR owns, and bind that into slot 2 for the matched draws. The
// game's own 470 instructions run untouched and sample ours through their
// own UVs, which is the same result with none of the risk. hud_sprite.h
// takes the same line for the same reason.
//
// ONCE PER FRAME, not once per draw: up to 30 draws share the surface, and
// its content is identical for all of them. The resample is the cost --
// 2440x1996 at 2x is 19.5 Mpx of EASU a frame -- which is why the scale and
// the sharpening are both tunable and why 1 (off) is a real option.
//
// WHAT IT CANNOT DO. No resampler invents detail. The panel's artwork is
// rasterised at whatever pixel size the game lays it out at, and that is
// how much information exists. Cleaner edges under magnification is the
// prize; if it reads the same, the ceiling is the game's own layout.
//
// Off by default. Every failure -- no compiler, no compile, no texture --
// stands it down for the session and lets the game sample its own surface.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads experimental.holo_panels (stock | sharp), advanced.holo_panel_scale,
// advanced.holo_panel_sharpen, advanced.holo_panel_size and the shader pin
// advanced.holo_panel_vs. Install and reload; live, and a change of scale or
// sharpening rebuilds at the next frame.
void panelUpscaleConfigure(Config& cfg);

// False in stock mode and once stood down, which keeps the draw path free.
bool panelUpscaleWantsDraws();

// Is this eye draw a holo panel reading the surface we are aimed at? The
// vertex shader's hash, then slot 2 being a Texture2D of the configured
// size -- which is what separates the ONE panel being treated from the
// other twenty-three drawn by the same shader.
bool panelUpscaleOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                           uint32_t instances);

// Around the real draw: resample the surface if this frame has not done it
// yet, bind ours into slot 2, put the game's back after.
void panelUpscaleBegin(ID3D11DeviceContext* ctx);
void panelUpscaleEnd(ID3D11DeviceContext* ctx);

// Frame edge: one resample a frame, however many panel draws sample it.
void panelUpscaleFrameEnd();

void panelUpscaleShutdown();

}  // namespace edvr
