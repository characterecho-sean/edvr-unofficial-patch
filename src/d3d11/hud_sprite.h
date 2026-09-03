// The cockpit HUD's sprite atlases, upscaled once and handed to the game.
//
// WHAT THIS IS, and the four wrong answers it took to find it
//
// The target direction indicator -- the orange wireframe sphere with the
// alignment dot, hollow when the target is behind you -- is drawn straight
// into the eye by ONE six-vertex quad per eye (vs 5DA53D8B0133341E, ps
// E23C45251B7ECDFE), sampling a 1024x256 BC7 atlas at slot 0. An authored
// texture of fixed size, magnified onto a quad.
//
// That is why it never sharpened when render resolution rose while every
// HUD element around it did, and it is the fact that four earlier
// hypotheses each failed to explain. Ruled out by suppression, each with
// the draw counter confirming the probe engaged:
//
//   vs E508648660A352B2   the interface-surface composite -- 86k draws
//                         dropped, indicator untouched. This is the one the
//                         census A/B named, and it is NOT this widget: it
//                         appears when a target is selected, which is not
//                         the same claim.
//   vs B7790CBFC6554097   the HUD's dynamic vector family -- 116k dropped,
//                         untouched.
//   1774x1774 surface     ~6.4k dropped, untouched.
//   2342x1464, 3407x732,
//   1186x346 surfaces     ~6.6k dropped, untouched.
//
// Found in the end by positional bisection (advanced.census_skip_range),
// which cannot miss because it does not care what draws a thing -- five
// halvings from 324 eye draws to one. Worth remembering that the census
// names draws and the eye names elements, and connecting the two takes a
// suppression test, not an inference.
//
// THE FIX. The atlas is a fixed-size asset that never changes, so it is
// resampled ONCE at the first draw that samples it -- AMD's EASU, then
// optionally RCAS -- into an EDVR-owned texture, and that texture is bound
// into slot 0 for the matched draws and taken out again after. Nothing is
// recomputed per frame; the cost is one dispatch per atlas per session and
// a few megabytes.
//
// The game's own pixel shader is left completely alone. It samples through
// normalized UVs, so a bigger texture of the same aspect simply arrives
// sharper -- which means this needs no disassembly of it, unlike a shader
// swap.
//
// WHAT IT CANNOT DO. No resampler invents detail. A 1024x256 atlas holds
// this widget in some fraction of itself, and that is how many texels the
// artwork has however it is filtered. Cleaner edges under magnification is
// the whole prize; if the source is blocky for its own reasons -- BC7
// blocking on thin lines, say -- this will not hide it.
//
// Off by default. Every failure -- no compiler, no compile, no texture --
// stands it down for the session and lets the game sample its own atlas.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads experimental.hud_icons (stock | sharp), advanced.hud_icon_scale,
// advanced.hud_icon_sharpen and the shader pin advanced.hud_icon_vs.
// Install and reload; live, but a change of scale or sharpening only
// reaches atlases resampled after it.
void hudSpriteConfigure(Config& cfg);

// False in stock mode and once stood down, which keeps the draw path free.
bool hudSpriteWantsDraws();

// Is this eye draw one of the atlas quads? Shape first (six vertices, one
// instance), then slot 0 being a small Texture2D, then the vertex shader's
// content hash -- cheapest test first, and the hash last because it costs a
// VSGetShader.
bool hudSpriteOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                        uint32_t instances);

// Around the real draw: resample this atlas if it has not been done yet,
// bind ours into PS slot 0, put the game's back after. A begin that cannot
// build degrades to the draw running untouched.
void hudSpriteBegin(ID3D11DeviceContext* ctx);
void hudSpriteEnd(ID3D11DeviceContext* ctx);

void hudSpriteShutdown();

}  // namespace edvr
