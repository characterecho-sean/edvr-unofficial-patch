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

// One eye, one frame, but NVIDIA runs on a CROP of the frame -- the fovea,
// docs/performance.md feature 6. The colour, depth and motion are the same
// full-frame textures dlaaEvaluate is fed; the crop names the sub-rectangle
// (in render pixels, cropX/cropY the top-left, cropW/cropH the size) that
// NVIDIA reads and writes, through the runtime's input and output
// sub-rectangles. DLAA only (1:1), so the output crop is the input crop; a
// per-eye feature is created with output sub-rectangles enabled and rebuilt
// on a size change. The crop must lie inside the frame. `output` is written
// only in the crop region; the periphery is the caller's to fill and blend.
// The moving-crop probe (2026-09-04) proved a crop pans cleanly through
// NVIDIA's history; a fixed centre is the trivial case of that. False on any
// refusal, with its reason. Counts into the same totals as dlaaEvaluate.
bool dlaaEvaluateFovea(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour,
                       ID3D11Texture2D* depth, ID3D11Texture2D* motion,
                       ID3D11Texture2D* output, uint32_t w, uint32_t h,
                       uint32_t cropX, uint32_t cropY, uint32_t cropW, uint32_t cropH,
                       float jx, float jy, bool reset, float frameMs,
                       const char** reason);

// The general form: NVIDIA runs on an INPUT crop (icx,icy,icw,ich) of the
// render-size textures and writes an OUTPUT crop (ocx,ocy,ocw,och) of the
// native output. Equal crops are DLAA (1:1, dlaaEvaluateFovea above); a
// smaller input crop is DLSS upscaling just the fovea (docs/performance.md
// feature 6, the half-render variant). The feature is created at the input
// crop -> output crop and rebuilt when either changes. False on refusal.
bool dlssEvaluateFovea(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour,
                       ID3D11Texture2D* depth, ID3D11Texture2D* motion,
                       ID3D11Texture2D* output, uint32_t inW, uint32_t inH,
                       uint32_t outW, uint32_t outH, uint32_t icx, uint32_t icy,
                       uint32_t icw, uint32_t ich, uint32_t ocx, uint32_t ocy,
                       uint32_t ocw, uint32_t och, float jx, float jy, bool reset,
                       float frameMs, const char** reason);

// The steady periphery (docs/performance.md feature 6): DLAA over the WHOLE
// of a w x h frame -- a copy of the render reduced to the periphery's scale,
// or the render itself when the game rendered small -- through a third
// per-eye feature with its own history, so the fovea, the periphery and the
// full frame never share an accumulation. The composite upscales what comes
// back around the fovea. The inputs are the same kinds as dlaaEvaluate's, at
// w x h; `output` (R8G8B8A8_UNORM, w x h, an unordered-access view possible)
// is written whole. False on any refusal, with its reason.
bool dlaaEvaluatePeriphery(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* colour,
                           ID3D11Texture2D* depth, ID3D11Texture2D* motion,
                           ID3D11Texture2D* output, uint32_t w, uint32_t h, float jx, float jy,
                           bool reset, float frameMs, const char** reason);

// The measured price, for the totals line: evaluations, the mean
// milliseconds by timestamp query, and how many evaluations carried the
// reset. False when nothing has run.
bool dlaaTotals(uint32_t* evaluations, double* avgMs, double* maxMs,
                uint32_t* resets);

void dlaaShutdown();

// The moving-crop probe (docs/performance.md, feature 6 and Phase 0 item
// 16), a desk experiment for the smoke harness: does NVIDIA's history
// survive a crop that moves with the gaze when the shift is folded into
// the motion vectors? Runs a synthetic scene through DLAA on a 512x384
// crop of a 1280x960 frame under six conditions and writes a multi-line
// report into `report`. Returns 1 when a moved crop converges like a
// still one (a pan), 2 when it converges like a fresh history (a reset
// per move), 3 when it is worse than a fresh history (a smear), 4 when
// the scene did not discriminate (the still crop's history did not beat
// its first frame, so nothing can be placed against it), 0 when the
// probe could not run (the report says why).
int dlaaCropProbe(ID3D11Device* dev, ID3D11DeviceContext* ctx, char* report,
                  uint32_t reportBytes);

}  // namespace edvr
