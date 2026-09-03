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
// samples read at the LAST moment in a frame the game switches away from
// a target -- its contents complete, and the view no longer bound as a
// target, which is the one state in which a shader may read it -- both
// through a view over the texture and through a copy of it, so a read
// that fails one way and not the other says so in one line. The third
// flight (2026-09-03) read at the clear, while the view was still bound,
// and got 256 zeros per grid; the fourth read at the first unbind with
// the stage cleared and got zeros again. Whether that is the read or the
// buffer is what the two paths, the last-unbind rule and the desk
// self-test below now decide.
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

// EVERY draw on the owner context, with the depth-stencil view bound for
// it (the binding shadow's Dsv0, which may be null), whether the colour
// target beside it was eye-sized, and whether there was none: the census
// of where the game's depth actually goes. The fifth flight (2026-09-03)
// read empty buffers from every target the EYE draws bind, both through a
// view and through a copy, so the scene's depth is written by draws the
// eye classifier never counts -- a depth pre-pass with no colour target
// is the usual shape -- and this is how they are found. One pointer
// compare per draw.
void depthProbeNoteDraw(ID3D11DeviceContext* ctx, void* dsv, bool rtvEyeSized,
                        bool rtvNull);

// The indirect draws (DrawIndexedInstancedIndirect and its twin), which
// never reach the classifier: counted and their depth target noted, so a
// scene drawn GPU-side is not invisible to the census.
void depthProbeNoteIndirectDraw(ID3D11DeviceContext* ctx, void* dsv);

// Every eye-sized draw, with the same view and the draw's index within
// the frame (1 = the frame's first eye draw), for which eye a target is.
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

extern "C" {
// The desk test of the read path (tools/smoke): a depth texture of the
// family the game uses (R32G8X24_TYPELESS under a D32_FLOAT_S8X24_UINT
// view), cleared to 0.5 through the view, unbound, then read both ways
// with the probe's own sampler. Returns bits: 1 the setup was made, 2 the
// direct view read the value everywhere, 4 the copy did. 7 is a pass.
__declspec(dllexport) unsigned edvrDepthProbeSelftest(void* device);
}
