// The intro movie, resampled before it is magnified.
//
// THE ARITHMETIC THAT MAKES THIS WORTH DOING. The movie decodes at
// 1920x1080. Drawn on the splash's own screen it covers about 5663 x 3185
// eye pixels -- 2.95x linear, 8.7x by area -- and the game's composite gets
// there with a single bilinear `sample` (its pixel shader is one
// instruction; docs/shaders/intro-composite-ps.asm). Bilinear at 3x is what
// the field sees as pixelation.
//
// So: resample once per frame into an EDVR-owned texture at twice the
// source, and let that one draw sample ours. The final step the game's
// sampler still does is then a gentle 1.47x rather than 2.95x. The pattern
// is backdrop_fix's exactly -- compute into our texture, substitute the
// draw's slot 0, restore after -- the only difference being that a movie
// changes every frame where a menu still does not.
//
// WHAT IT CANNOT DO, said plainly because the field asked for FSR. No
// resampler invents detail. The splash this movie is matched to looks
// better at the same size for one reason: its still is 3840x2160, four
// times the pixels. Sharper edges and less blockiness are the whole prize
// here.
//
// AND WHY THIS IS NOT FSR. FSR's EASU is an edge-adaptive kernel whose
// exact taps and constants I would be reciting from memory, and a thing
// labelled FSR that is not FSR is worse than an honest bicubic. What runs
// here is CATMULL-ROM, whose kernel is stated in full in the shader and can
// be checked against any reference. If EASU is wanted properly, AMD's
// ffx_fsr1.h is MIT and public: vendoring it makes the result a
// transcription rather than a recollection, which is the standard the rest
// of this project holds itself to (docs/particle-billboards.md, and the
// loading wash that was solved by reading disassembly rather than guessing).
//
// Off by default. Every failure -- no compiler, no compile, no texture --
// stands it down for the session and lets the game sample its own surface.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace edvr {

class Config;

// Reads fix.intro_video (this module's slice is the resample). Install
// and reload; live.
void introUpscaleConfigure(Config& cfg);

// False in stock mode and once stood down, which keeps the draw path free.
bool introUpscaleWants();

// Substitute our resampled texture for PS slot 0 on this draw, resampling
// first if this frame has not been done yet. srcSrv is what the draw would
// have sampled. True means the caller must call introUpscaleEnd afterwards.
bool introUpscaleBegin(ID3D11DeviceContext* ctx,
                       ID3D11ShaderResourceView* srcSrv);

// Put the game's own view back. Always paired with a true above.
void introUpscaleEnd(ID3D11DeviceContext* ctx);

// Frame edge: one resample per frame, however many eyes sample it. Both
// eyes' surfaces carry identical content (same planes, same shaders,
// measured), so one pass serves both and cannot make them differ.
void introUpscaleFrameEnd();

void introUpscaleShutdown();

}  // namespace edvr
