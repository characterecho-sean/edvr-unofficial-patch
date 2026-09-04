// DLAA at the door -- NVIDIA's trained temporal anti-aliasing (DLSS with
// no upscaling), fed by the plumbing this branch built for its own pass.
//
// The own pass proved the inputs (docs/anti-aliasing.md): the jitter goes
// through the projection edit, the head's pose delta and the scene's
// depth register the cockpit (three docked flights, 2026-09-03), and the
// reprojection of every pixel is a motion vector by another name. DLSS
// consumes exactly that set -- colour, depth, per-pixel motion, the
// jitter offset -- and replaces the hand-rolled history clip with a
// trained one, which is where text goes from "registered" to "steady".
// The player asked for it after the twelfth build; the rig's GPU is an
// RTX 5090 (measured, Win32_VideoController).
//
// This file is the NGX glue and nothing else. It compiles with the calls
// only when the build has NVIDIA's SDK (EDVR_HAVE_NGX, set by build.bat
// when EDVR_NGX_SDK names the SDK's directory); without it, every entry
// answers "not built in" and the temporal pass runs its own history as
// before. The runtime DLL (nvngx_dlss.dll) must sit beside the game's
// executable, where NGX looks for it; its absence is also a plain answer.
#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace edvr {

// Is DLAA usable on this device? Initialises NGX on the first ask (once
// per session, whatever the answer) and says why not when it is not:
// the reason is a static string for the log. Cheap after the first call.
bool dlaaAvailable(ID3D11Device* dev, const char** reason);

// One eye, one frame: colour (R8G8B8A8_UNORM, w x h, a shader view
// possible), depth (R32_FLOAT, the game's reversed-Z values copied), the
// motion vectors (R16G16_FLOAT, pixels, current -> previous), into the
// output (R8G8B8A8_UNORM, outW x outH, an unordered-access view
// possible). Equal sizes are DLAA; an output larger than the input is
// DLSS proper, the quality mode being the one whose own render size (as
// the runtime names it for this output) is nearest the input, among the
// modes whose range holds it; the size ratio decides only when the
// runtime will not say. The jitter is this frame's offset in input
// pixels; reset breaks the history and must be true ONLY when it is
// broken (an eye's first frame, a size change, a withhold) -- the review
// of 2026-09-04 found it raised every frame; frameMs is the time since
// this eye's previous evaluation, zero when unknown. A feature per eye is
// created on first use and rebuilt on a size change. False on any
// refusal, with its reason.
bool dlaaEvaluate(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour,
                  ID3D11Texture2D* depth, ID3D11Texture2D* motion,
                  ID3D11Texture2D* output, uint32_t w, uint32_t h,
                  uint32_t outW, uint32_t outH, float jx, float jy, bool reset,
                  float frameMs, const char** reason);

// The measured price, for the totals line: evaluations, the mean
// milliseconds by timestamp query, and how many evaluations carried the
// reset. False when nothing has run.
bool dlaaTotals(uint32_t* evaluations, double* avgMs, double* maxMs,
                uint32_t* resets);

void dlaaShutdown();

}  // namespace edvr
