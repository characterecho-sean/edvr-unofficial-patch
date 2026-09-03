// The depth probe -- docs/anti-aliasing.md Phase 0 item 3, measured.
//
// The temporal pass's second build flew on 2026-09-03 and registered the
// world but not the cockpit: text ghosted under head motion and stayed
// soft under a strong sharpen, and the clip rate rose to a quarter of the
// pixels. A rotation-only reprojection cannot register near content --
// the head's translation moves a panel at 0.6 m by about two pixels a
// frame during an ordinary turn at Quest 3 densities, and by a fraction
// of a pixel from tracking noise alone -- so v2 needs depth, and depth
// needs three facts this probe collects from a flight: WHICH texture the
// eye draws use as their depth target (size, format, bind flags, whether
// a shader view can be made over it or it must be copied), which eye's
// it is (the order they are bound in each frame), and HOW the values are
// encoded (standard or reversed, what the far plane reads as), taken
// from a 16x16 grid of samples read at the moment the game clears it for
// the next frame, when the previous frame's depth is complete.
//
// Runs only while fix.temporal_aa is on (it exists for that pass), costs
// one pointer compare per eye draw and one tiny dispatch every few
// seconds, dereferences a view only while the game has it bound or is
// passing it to a clear, and stands down for the session on any repeated
// fault. Nothing it does reaches the picture.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11DepthStencilView;

namespace edvr {

class Config;

// Reads fix.temporal_aa: the probe is wanted while the pass is.
void depthProbeConfigure(Config& cfg);

// Every eye-sized draw, with the depth-stencil view bound for it (the
// binding shadow's Dsv0, which may be null) and the draw's index within
// the frame (1 = the frame's first eye draw).
void depthProbeNoteEyeDraw(ID3D11DeviceContext* ctx, void* dsv,
                           uint32_t eyeDrawIndex);

// From the ClearDepthStencilView hook, BEFORE the clear is forwarded: if
// this view is one the eye draws use, and the interval has passed, sample
// the finished depth behind it. `depth` is the value the game clears to.
void depthProbeBeforeClear(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv,
                           float depth);

// Once per frame: the per-frame bookkeeping, the readback poll, the lines.
void depthProbeFrameBoundary(ID3D11DeviceContext* ctx);

void depthProbeShutdown();

}  // namespace edvr
