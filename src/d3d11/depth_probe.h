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
// Runs only while fix.temporal_aa or fix.eye_mask is on (it exists for the
// temporal pass; eye_mask reuses its clear-value census rather than keeping
// a second one), costs one pointer compare per eye draw and per render-
// target change, and one tiny dispatch every few seconds; dereferences a
// view only inside the call the game made with it; stands down for the
// session on any repeated fault. Nothing it does reaches the picture.
#pragma once

#include <cstdint>

#include <d3d11.h>   // DXGI_FORMAT: depthReadFormat below shares the probe's table

struct ID3D11DeviceContext;
struct ID3D11DepthStencilView;
struct ID3D11Texture2D;

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

// Is the probe watching? depthProbeNoteDraw's own first test, published so
// the draw path can skip the call: the probe is armed only by fix.temporal_aa,
// fix.eye_mask or advanced.eye_depth_capture, and with all three off the note
// was a cross-TU call per draw (this build has no /GL) that returned at once --
// 49 innermost samples of the flown 2026-09-22 window, all on its prologue and
// epilogue.
namespace detail { extern bool g_depthProbeWanted; }
inline bool depthProbeWanted() { return detail::g_depthProbeWanted; }

// The indirect draws (DrawIndexedInstancedIndirect and its twin), which
// never reach the classifier: counted and their depth target noted, so a
// scene drawn GPU-side is not invisible to the census.
void depthProbeNoteIndirectDraw(ID3D11DeviceContext* ctx, void* dsv);

// Every eye-sized draw, with the same view and the draw's index within
// the frame (1 = the frame's first eye draw), for which eye a target is.
void depthProbeNoteEyeDraw(ID3D11DeviceContext* ctx, void* dsv,
                           uint32_t eyeDrawIndex);

// Would depthProbeNoteEyeDraw do anything for this view? Its common case is
// "watching, this frame already flagged, same view as the last eye draw":
// it sets a flag that is already set and returns at one compare. That call
// was made on every eye draw (/O2, no /GL) -- about 35 innermost samples of
// the 1355-frame parked-5 window between the callee and its call site. The
// two fields are the callee's own (depth_probe.cpp binds its names to them),
// so false here is exactly the case in which the call changes nothing.
namespace detail {
extern void* g_depthProbeLastDsv;
extern bool g_depthProbeEyeDrawThisFrame;
}  // namespace detail
inline bool depthProbeEyeDrawNeedsNote(void* dsv) {
    return detail::g_depthProbeWanted &&
           (!detail::g_depthProbeEyeDrawThisFrame || dsv != detail::g_depthProbeLastDsv);
}

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

// The clear value on record for THIS EXACT view, and whether it reads as
// reversed-Z (< 0.5) -- for fix.eye_mask, which must write the near value
// into the game's own depth convention and must never guess it. False when
// nothing is on record: the probe is off (fix.temporal_aa and fix.eye_mask
// both off), this view has never been cleared while watched, or it is not a
// target the probe tracks. A caller that gets false must not draw.
bool depthProbeClearValueFor(ID3D11DepthStencilView* dsv, float* outClearValue, bool* outReversed);

// Once per frame: the per-frame bookkeeping, the readback poll, the lines.
void depthProbeFrameBoundary(ID3D11DeviceContext* ctx);

// THE SCENE'S DEPTH for one eye of the frame being submitted, for the
// temporal pass: among the targets of the frame's render size, the ones
// with the scene's draws (hundreds a frame; the cockpit census of
// 2026-09-03 read 302 and 325 against 1, 6 and 7 for the composites and
// the cockpit layer), ordered by when in the frame each was first bound --
// the first is the first eye rendered, which is assumed to be the left
// (the registration instrument's "depth, eyes swapped" candidate checks
// that). Returns the texture the probe holds a reference on, valid for
// the rest of this frame, or null when the census has not settled.
bool depthProbeSceneDepth(uint32_t w, uint32_t h, int eye, ID3D11Texture2D** tex);

// Is this texture (a resource identity, compared and never dereferenced)
// one of the scene pair the pass reads, as last chosen? For ui_depth,
// which writes the interface's depth only where the pass will read it:
// the main menu's panel binds a depth of its own that nothing reads.
// False until the pair has been chosen.
bool depthProbeIsSceneDepth(const void* resource);

// The scene pair's texture for one eye AND the view format the game binds
// it with, for a module that must bind that texture itself: ui_depth
// binds the pass's depth at a menu or loader composite, whose own depth
// target nothing reads. The texture reference is the probe's (held for the
// frame); the caller creates its own view over it. False when no pair.
bool depthProbeSceneDepthFormat(uint32_t w, uint32_t h, int eye,
                                ID3D11Texture2D** tex, uint32_t* dsvFormat);

// For a caller that already has the scene-depth texture: validate that it
// remains in the refreshed pair, that its first matching target-table entry
// has a usable format, and return its first-bind eye. The cheap current-pair
// identity check happens before the size-based refresh. False clears *outEye.
bool depthProbeSceneTextureEye(uint32_t w, uint32_t h, const void* resource,
                               int* outEye);

// For fix.eye_mask: which eye (if either) THIS EXACT depth-stencil view
// is, among the scene pair depthProbeSceneDepth picks, using the same
// first-bind ordering (one shared helper, so the two can never disagree).
// *outTargetIndex is set whenever the probe has ever seen this view at
// all, even on a false answer, and left at -1 only when it has not -- so
// a caller can tell "unknown to the probe" apart from "known, but not (or
// no longer) the scene's current pick". A view outside the current pair
// re-evaluates the pick by its own size first (the same evaluation the
// temporal pass makes by the eye's size), so the pair forms on a rig
// where nothing else asks for it; call this only at an eye draw, where
// every bound view is eye-sized. No order-based fallback for a same-sized
// target that is still not one of the two picks.
bool depthProbeSceneEyeOf(ID3D11DepthStencilView* dsv, int* outEye, int* outTargetIndex);

// Diagnostic-only identity lookup against the already settled scene pair.
// This never scans, refreshes, or changes the pair; false means the view is
// not one of the two current picks (including when no pair has formed yet).
bool depthProbeCurrentSceneEyeOf(ID3D11DepthStencilView* dsv, int* outEye,
                                 int* outTargetIndex);

// Whether target index (from depthProbeSceneEyeOf's outTargetIndex) shares
// the scene pick's width/height -- a double-buffered twin the game
// alternates with the chosen pair -- for the eye mask summary's "not the
// scene's pick" tally. False with no scene pick yet or an out-of-range
// index.
bool depthProbeTargetIsSceneSized(int targetIndex);

// How many draws the scene pair's lesser target took last frame: the
// temporal pass's test of a REAL scene (hundreds in the cockpit and in
// space; one or two for the main menu's pre-rendered backdrop, whose
// far plane must not be reprojected by a camera that does not follow
// the head). 0 until the census has settled.
uint32_t depthProbeSceneDraws();

void depthProbeShutdown();

// The view format that reads the depth channel of a texture of this
// format, and the typeless format an owned copy of it must have. UNKNOWN
// when this build knows no such view. Inline in the header: the eye-run
// depth capture (eye_depth_capture.h) converts the R32G8X24 family through
// this same table, and its test rig does not link depth_probe.cpp.
inline DXGI_FORMAT depthReadFormat(DXGI_FORMAT tex, DXGI_FORMAT* copyFmt) {
    switch (tex) {
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            *copyFmt = DXGI_FORMAT_R24G8_TYPELESS;
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
            *copyFmt = DXGI_FORMAT_R32_TYPELESS;
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            *copyFmt = DXGI_FORMAT_R32G8X24_TYPELESS;
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
            *copyFmt = DXGI_FORMAT_R16_TYPELESS;
            return DXGI_FORMAT_R16_UNORM;
        default:
            *copyFmt = DXGI_FORMAT_UNKNOWN;
            return DXGI_FORMAT_UNKNOWN;
    }
}

}  // namespace edvr

extern "C" {
// The desk test of the read path (tools/smoke): a depth texture of the
// family the game uses (R32G8X24_TYPELESS under a D32_FLOAT_S8X24_UINT
// view), cleared to 0.5 through the view, unbound, then read both ways
// with the probe's own sampler. Returns bits: 1 the setup was made, 2 the
// direct view read the value everywhere, 4 the copy did. 7 is a pass.
__declspec(dllexport) unsigned edvrDepthProbeSelftest(void* device);
}
