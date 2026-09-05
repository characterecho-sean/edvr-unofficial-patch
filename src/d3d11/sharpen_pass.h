// Render sharpening at the door -- AMD's RCAS on the outgoing frame, the
// d3d11 half.
//
// The LAST treatment before an eye frame leaves for the compositor: one
// compute dispatch of AMD's RCAS (the FSR 1 sharpener, vendored under
// src/d3d11/fsr/ and already carried by the intro upscale) over the eye's
// region, into an EDVR-owned texture of the region's size in the source's
// own format, which the openvr half forwards with full bounds. It exists
// for the two passes ahead of it: the supersample resolve's calm kernel
// and the temporal pass's history each trade a little edge contrast for
// calm, and this hands some of it back at the player's chosen strength --
// docs/anti-aliasing.md's "sharpen" in the order at the door, and the seam
// the resolve left marked for exactly this. Built 2026-09-03, after the
// temporal pass's first flight found text a little soft.
//
// RCAS reads a pixel and its four neighbours, and the loads clamp INTO the
// region the way the resolve's taps do, so a double-wide texture's other
// eye is never read at the seam and the frame's edge is not ringed by the
// zero D3D answers for a load off the texture. Nothing here changes size,
// format or orientation.
//
// The openvr half owns the decision (src/openvr/sharpen.cpp); this half
// owns the pass, behind one export it resolves by GetProcAddress and stands
// down without in the theater's "mismatched pair?" voice. Null from the
// export means "forward what you had", and every refusal says why once.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads fix.render_sharpness, for two things only this half can do: warm
// the shader ahead of the first engage, and say so when the strength is
// set but no compositor hook ever announces itself (an openvr-only
// install, or every feature that needs that hook was off at launch).
void sharpenPassConfigure(Config& cfg);

// Once per frame, from the frame boundary: the warm compile when wanted
// (the resolve's reason -- a first-use D3DCompile on the submit thread),
// and the missing-hook note.
void sharpenPassTick(ID3D11DeviceContext* ctx);

// For the periodic totals line: how many eye-submits have been sharpened,
// and the measured price. False when nothing has run.
bool sharpenPassTotals(uint32_t* treated, double* avgMs, double* maxMs);

void sharpenPassShutdown();

}  // namespace edvr

extern "C" {
// srcTex:   an ID3D11Texture2D* a submit path is about to forward -- the
//           game's own, or the texture the pass before this one produced.
// eye:      0 left, 1 right; selects the per-eye owned resources.
// bounds:   the Submit's uMin, vMin, uMax, vMax naming this eye's region of
//           srcTex, or null for the whole texture. Flipped spans name the
//           same pixels as their unflipped twins; the openvr half keeps the
//           direction for the outgoing bounds.
// strength: 0 to 1, 1 the sharpest -- AMD's RCAS at 2 * (1 - strength)
//           stops of sharpness reduction. 0 has nothing to do and answers
//           null; the openvr half never calls with it.
//
// Returns the sharpened texture (EDVR-owned, per eye, the source's own
// format family, region-sized, full-span content), or null: the pass
// refused or failed, its own log line says why, and the caller must
// forward what it had and stand the sharpening down. The source is never
// written to, and no reference to it outlives the call except a cached
// shader view over it, released the moment a different texture arrives
// for this eye.
__declspec(dllexport) void* edvrSharpen(void* srcTex, int eye,
                                        const float* bounds, float strength);
}
