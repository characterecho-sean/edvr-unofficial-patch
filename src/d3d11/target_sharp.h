// The target direction indicator, resampled instead of smeared.
//
// WHAT IT IS (measured 2026-09-02, docs in this header because the arc that
// found it is worth not repeating)
//
// Selecting a target adds exactly ONE draw per eye, and a census A/B names
// it without ambiguity: a 6-index packed-mesh quad (vs E508648660A352B2)
// whose pixel shader 63ABD86359B57D01 is, in full, one bilinear `sample` of
// slot 0 plus an alpha discard and a HUD colour matrix. Slot 0 is an
// interface surface -- 5110x4471 on a 5424x5356 eye, 4089x3578 on a
// 4340x4284 one, the same 0.9421 x 0.8348 fraction of the render resolution
// both times.
//
// WHY IT LOOKS BLOCKY, AND WHY RESOLUTION DID NOT FIX IT
//
// The surface scales with render resolution, so its neighbours sharpen when
// that goes up. The indicator does not: measured across a 3072x3264 eye and
// a 5424x5356 one, everything around it improved and it did not. Creating
// the surface larger (a developer instrument, since retired) changed nothing
// visible either, with the mechanism confirmed working. What survives both results
// is that the indicator's content occupies a fixed number of surface texels
// however large the surface is -- so the quad magnifies it by the same
// factor at every resolution, and the magnification is where the blockiness
// enters.
//
// THE FIX: that magnification is ONE bilinear tap. Replacing the pixel
// shader with a faithful reimplementation that reconstructs with a
// Catmull-Rom kernel instead is the whole change -- no extra textures, no
// cache, no per-frame compute, nothing whose size depends on the surface.
//
// The colour maths is transcribed instruction for instruction from the
// game's own disassembly and is stated in the shader, so it can be checked
// rather than trusted. Only the fetch differs.
//
// WHAT IT CANNOT DO, said plainly because the field asked for FSR. No
// resampler invents detail. A bicubic reconstruction of a magnified image
// has cleaner edges and no bilinear diamonds; it has exactly as much
// information as it started with. If the source turns out to be blocky for
// some reason other than magnification -- block-compression artifacts in
// whatever was drawn into the surface, say -- this changes nothing, and
// that will be visible immediately.
//
// Off by default. Every failure -- no compiler, no compile -- stands it
// down for the session and lets the game draw its own shader.
#pragma once

#include "draw_interest.h"
#include "target_sharp_observation.h"

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads experimental.target_indicator (stock | sharp). Install and reload; live.
void targetSharpConfigure(Config& cfg);
bool targetSharpDrawInterestConfigured() noexcept;
std::size_t targetSharpDrawInterestFilters(draw_interest::ShaderFilter* out,
                                           std::size_t capacity) noexcept;

// False in stock mode and once stood down, which keeps the draw path free.
//
// Inline: asked per eye draw, and the build has no /GL to fold a cross-TU
// getter for two bool loads.
namespace detail {
extern bool g_targetSharpSharp;
extern bool g_targetSharpFailed;
}  // namespace detail
inline bool targetSharpWantsDraws() {
    return detail::g_targetSharpSharp && !detail::g_targetSharpFailed;
}

bool targetSharpWantsDrawsObserved(TargetSharpObservation& observation,
                                   bool helper = false) noexcept;

// Is this eye draw the indicator's composite? Shape first (6 indices, one
// instance), then slot 0 being a non-eye-sized Texture2D with slots 1-3
// unbound, then the vertex shader's content hash -- cheapest test first,
// and the hash read last because it costs a VSGetShader.
bool targetSharpOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                          uint32_t instances);
bool targetSharpOnEyeDrawObserved(ID3D11DeviceContext* ctx, char kind,
                                  uint32_t count, uint32_t instances,
                                  TargetSharpObservation& observation);

// Around the real draw: bind the replacement pixel shader, put the game's
// own back after. A begin that cannot compile degrades to the draw running
// untouched, for the rest of the session.
void targetSharpBegin(ID3D11DeviceContext* ctx);
void targetSharpEnd(ID3D11DeviceContext* ctx);

void targetSharpShutdown();

#if defined(EDVR_VSCREEN_PREDICATE_TEST)
void targetSharpPredicateTestSeed(bool sharp, bool failed) noexcept;
std::uint64_t targetSharpPredicateTestConfiguredHash() noexcept;
#endif

}  // namespace edvr
