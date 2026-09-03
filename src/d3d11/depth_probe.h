// The depth probe -- docs/anti-aliasing.md Phase 0 item 3, measured.
//
// The temporal pass's second build flew on 2026-09-03 and registered the
// world but not near content: text ghosted under head motion and stayed
// soft under a strong sharpen. A rotation-only reprojection cannot
// register what is close -- the head's translation moves a panel at 0.6 m
// by about two pixels a frame during an ordinary turn at Quest 3
// densities, and by a fraction of a pixel from tracking noise alone -- so
// v2 needs depth, and depth needs three facts this probe collects from a
// flight: WHICH textures the eye draws use as their depth targets (size,
// format, bind flags, whether a shader view can be made over one or it
// must be copied), which eye's each is (the order they are bound in a
// frame), and HOW the values are encoded (standard or reversed, what the
// far plane reads as, what a metre reads as), from a 16x16 grid of
// samples read at the moment the game UNBINDS a target -- its contents
// complete, and the view no longer bound as a target, which is the one
// state in which a shader may read it. The third flight (2026-09-03) read
// at the clear instead, while the view was still bound, and D3D handed
// the shader nulls: 256 zeros per grid. Measured, and moved.
//
// Runs only while fix.temporal_aa is on (it exists for that pass), costs
// one pointer compare per eye draw and per render-target change, and one
// tiny dispatch every few seconds; dereferences a view only inside the
// call the game made with it; stands down for the session on any
// repeated fault. Nothing it does reaches the picture.
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

// From the render-target hooks, BEFORE the game's rebind is forwarded:
// is `current` (the view bound until now) a target the eye draws use,
// about to be replaced by `next`, and due for a sample? When this says
// yes the caller unbinds the output-merger stage and calls
// depthProbeSample with the same view, then lets the game's rebind go.
bool depthProbeWantsSample(void* current, void* next);
void depthProbeSample(ID3D11DeviceContext* ctx, void* dsv);

// From the ClearDepthStencilView hook: the value the game clears an eye-
// draw target to, which says which way its depth runs.
void depthProbeNoteClear(ID3D11DepthStencilView* dsv, float depth);

// Once per frame: the per-frame bookkeeping, the readback poll, the lines.
void depthProbeFrameBoundary(ID3D11DeviceContext* ctx);

void depthProbeShutdown();

}  // namespace edvr
