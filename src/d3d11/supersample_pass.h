// The supersample resolve's filter -- the d3d11 half of docs/anti-aliasing.md
// Feature A, passive mode.
//
// When the game submits an eye image larger than the runtime asked for
// (Elite's HMD Quality above 1.0 is the usual cause), the compositor
// shrinks it on the fly as part of its distortion pass, with whatever
// sampler it happens to use. This pass does that shrink itself, at submit,
// with a kernel chosen for anti-aliasing -- a separable Gaussian by default
// ("calm"), Mitchell-Netravali on request ("crisp") -- into an EDVR-owned
// texture of exactly the recommended size, which the openvr half then
// forwards with full bounds so the compositor samples it one to one.
//
// Two compute dispatches per eye (horizontal, then vertical through a
// float16 intermediate), the taps clamped INSIDE the eye's own region so a
// double-wide texture's other eye and a cull-guard margin are never read,
// and gamma content decoded to linear light around the filter -- which is
// what the compositor's own sampler does with an sRGB view, so this is the
// same operation with a better kernel, not a different one.
//
// The openvr half owns the decision (src/openvr/supersample_resolve.cpp);
// this half owns the pass, behind one export it resolves by GetProcAddress
// and stands down without in the theater's "mismatched pair?" voice. Null
// from the export means "forward the game's own frame", and every refusal
// says why once.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads fix.supersample_resolve, for two things only this half can do:
// warm the shader ahead of the first engage, and say so when the setting
// is on but no compositor hook ever announces itself (an openvr-only
// install, or every feature that needs that hook was off at launch).
void supersamplePassConfigure(Config& cfg);

// Once per frame, from the frame boundary: the warm compile when wanted
// (fssTheaterWarm's reason -- a first-use D3DCompile measured at 142 ms
// would otherwise land on the submit thread), and the missing-hook note.
void supersamplePassTick(ID3D11DeviceContext* ctx);

// For the periodic totals line: how many eye-submits have been resolved,
// and the measured price. False when nothing has run.
bool supersamplePassTotals(uint32_t* treated, double* avgMs, double* maxMs);

void supersamplePassShutdown();

}  // namespace edvr

extern "C" {
// srcTex:  an ID3D11Texture2D* a submit path is about to forward -- the
//          game's own, the guard's crop, or the theater's rendering.
// eye:     0 left, 1 right; selects the per-eye owned resources.
// bounds:  the Submit's uMin, vMin, uMax, vMax naming this eye's region of
//          srcTex, or null for the whole texture. Flipped spans name the
//          same pixels as their unflipped twins; the openvr half keeps the
//          direction for the outgoing bounds.
// outW/H:  the size to resolve to (the runtime's recommended eye size).
//          Must not exceed the region on either axis: this pass only
//          shrinks, and refuses anything else.
// filter:  0 calm (gaussian), 1 crisp (mitchell); width: the kernel radius
//          in output pixels (clamped to what supersample_math.h allows).
// gamma:   1 when the submitted colours are sRGB-encoded (OpenVR's Gamma
//          colour space, or Auto on an 8-bit format), so the filter runs
//          in linear light; 0 to filter the stored values as they are.
//
// Returns the resolved texture (EDVR-owned, per eye, the source's own
// format family, full-span content), or null: the pass refused or failed,
// its own log line says why, and the caller must forward the original and
// stand the resolve down. The source is never written to, and no reference
// to it outlives the call except a cached shader view over the game's own
// eye texture, released the moment a different one arrives.
__declspec(dllexport) void* edvrSupersampleResolve(void* srcTex, int eye,
                                                   const float* bounds,
                                                   unsigned outW,
                                                   unsigned outH, int filter,
                                                   float width, int gamma);
}
