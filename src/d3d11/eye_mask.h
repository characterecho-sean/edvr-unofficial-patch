// The eye mask -- a per-eye depth-only ring drawn into the game's own eye
// depth target so later game draws lose the depth test outside the lens'
// visible circle.
//
// WHAT IT IS
//
// A VR runtime hands the game a hidden-area mesh once at startup: the
// pixels a round lens can never show, so the game need not shade them.
// Pimax's runtime hands over none, so Elite shades every corner pixel of a
// rectangular render target for a circular lens -- G-buffer raster work
// spent on pixels nobody sees. This fix draws a ring of triangles into the
// eye's depth buffer, depth-only, so every later draw that touches those
// pixels fails the depth test and its pixel shader never runs there.
//
// This saves G-BUFFER RASTER WORK ONLY. It does not and cannot change
// NVIDIA DLSS's own cost: DLSS runs once over the whole submitted frame at
// a fixed internal resolution regardless of how many of its input pixels
// were geometry or mask, so trimming raster work upstream of it changes
// nothing DLSS does. Nothing here lowers DLSS cost, and nothing should be
// added to this file that claims it does.
//
// GEOMETRY
//
// Given the eye's four frustum tangents (l<0<r left/right, d<0<u down/up,
// from the native timing table's gameFov, radians converted to tangents),
// an ellipse approximates the lens' visible circle in NDC:
//
//   centre  (x_c,y_c) = (-(r+l)/(r-l), -(u+d)/(u-d))
//   R0      = widest of atan(-l), atan(r), atan(-d), atan(u)   -- degrees
//   R       = clamp(R0 - trim, 5, 89)                          -- degrees
//   a,b     = 2 tan(R)/(r-l), 2 tan(R)/(u-d)                   -- NDC semi-axes
//
// The ring is a triangle strip E_0,O_0,...,E_N,O_N (N = kRingSegments),
// built in the vertex shader from SV_VertexID alone -- no vertex buffer,
// no input layout:
//
//   E_i = centre + (a cos th_i, b sin th_i)          th_i = i * 2pi/N
//   O_i = centre + K * (a cos th_i, b sin th_i)      K = 4 / min(a,b)
//
// K = 4/min(a,b) puts the SMALLER semi-axis of the outer ellipse at 4 NDC
// units, so the outer ring clears every corner of the viewport (the
// farthest an NDC point can be is a corner, at sqrt(2)) no matter how
// eccentric the eye's frustum is. The pixel shader is NULL: nothing this
// draw does can write colour, because it binds zero render targets.
//
// DEPTH VALUE
//
// The ring is written at the NEAR value of whatever depth convention the
// game's own clear established for the target actually bound at the draw
// (depth_probe.h's depthProbeClearValueFor) -- 1.0 if the probe read a
// reversed-Z clear, 0.0 otherwise. Never guessed: if the probe has no
// clear on record for this exact view, this fix stands down for that eye
// this frame and says so.
//
// PAST THE HOOKS
//
// The draw and every state change around it go through vscreen.h's *Raw
// wrappers, and for the D3D11 methods this module never hooks at all
// (IASetPrimitiveTopology, IASetInputLayout, GSSetShader, HSSetShader,
// DSSetShader, RSSetState, OMSetDepthStencilState, every getter) straight
// through the context -- so the draw census, the eye-draw gate, foveation
// and the temporal pass never see this draw, the same bargain vscreen.cpp
// and ui_depth.cpp already keep for their own rebinds. Every piece of
// pipeline state this fix touches is saved before the draw and restored
// after; the game's own viewports are never touched.
//
// WHICH EYE, WHEN
//
// Once per eye per frame, at that eye's FIRST draw into its scene target
// this frame (vscreen.cpp's beginPanelOverride) -- identified by depth
// target, not colour: depth_probe.h's depthProbeSceneEyeOf says which eye
// a bound depth-stencil view is, among the two busiest same-size targets
// of last frame, the identical identity the temporal pass keys on. Not
// foveation.cpp's own eyeOf/settleEyesByOrder: under the OpenXR port
// nothing publishes the game's own eye texture into the slot eyeOf roots
// against, so it never settles for any target (docs/eye-mask-2026-09-16.md)
// -- only once the view is recognised as the scene's pick and has a clear
// on record.
//
// RUNTIME MASK, ACROSS THE DLL BOUNDARY
//
// The VR half already asks the runtime for its own hidden-area mesh
// (native_runtime_host.h's refreshHiddenMasks) and now also publishes how
// many triangles it got, per eye, over the versioned shared-memory table
// (common/frame_flag.h's announceRuntimeMaskTriangles/runtimeMaskTriangles,
// mapping version bumped to v31 for this field). An older VR half that
// never calls announceRuntimeMaskTriangles leaves runtimeMaskTriangles
// answering false -- "not reported" -- and `auto` mode stands down while
// that holds, logging "runtime mask: not reported", rather than assume
// either way.
//
// MODES
//
//   fix.eye_mask = off | auto | lens          (default off until flown)
//   fix.eye_mask_trim = <degrees>, -15..30, default 0, live
//
// off    nothing.
// auto   the runtime's own report decides: reported with triangles ->
//        trust it, draw our ring only if trim asks for MORE than that
//        (trim > 0); reported as zero -> the runtime supplies none, draw
//        the ring; not reported at all -> stand down rather than guess.
// lens   always draw the ring, regardless of what the runtime reports.
//
// FAIL-SAFES
//
// Needs a depth view bound and recognised as the scene's current pick (not
// merely of the right size -- depth_probe.h's depthProbeSceneEyeOf), a
// recorded depth clear, a fresh FOV sample, a writable (not read-only)
// depth view and a clear OM stage (no UAV bound through
// OMSetRenderTargetsAndUnorderedAccessViews, which this fix's own plain
// OMSetRenderTargets bind and restore would otherwise silently drop);
// missing any one stands this fix down for that eye that frame and logs
// why, at most once per distinct reason (see the reason table in
// eye_mask.cpp). Every D3D11 call this fix makes runs under this module's
// own fault budget.
#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace edvr {

class Config;

void eyeMaskConfigure(Config& cfg);

// One bool for the draw path's early-out set -- mirrors foveationWantsDraws.
//
// Inline: asked per draw, and the build has no /GL to fold a cross-TU
// getter for one scalar load. The enum lives here too, so this can name
// its Off value.
namespace detail {
enum class EyeMaskMode { Off, Auto, Lens };
extern EyeMaskMode g_eyeMaskMode;
}  // namespace detail
inline bool eyeMaskWantsDraws() { return detail::g_eyeMaskMode != detail::EyeMaskMode::Off; }

// From vscreen.cpp's beginPanelOverride, at an eye draw's first reach this
// frame: dsvIdentity is the binding shadow's Dsv0, an IDENTITY only (never
// dereferenced): a null there means nothing is bound and this call returns
// immediately. When something is bound, the actual draw re-fetches a
// REFERENCED pointer via OMGetRenderTargets before touching it, as ground
// truth -- the shadow holds no reference and must never be passed to a
// real D3D11 call. Which eye (if either) that view is comes from
// depth_probe.h's depthProbeSceneEyeOf, keyed on the depth view itself --
// not from foveation's RTV-rooted eyeOf, which the OpenXR port leaves
// unsettled for every target (docs/eye-mask-2026-09-16.md).
void eyeMaskOnEyeDraw(ID3D11DeviceContext* ctx, void* dsvIdentity);

// From the ClearDepthStencilView hook, EVERY clear, any view (an IDENTITY
// only, never dereferenced): was this the depth target a mask was already
// drawn into THIS FRAME? Counts a re-clear per eye for the periodic
// summary -- a clear after the mask wipes it, which would silently undo
// the saving for the rest of that frame.
void eyeMaskOnClear(void* dsv);

// Once per frame on the owner context: ages the per-eye "already drawn"
// latch, prints the periodic summary when due.
void eyeMaskFrameBoundary(ID3D11DeviceContext* ctx);

void eyeMaskShutdown();

}  // namespace edvr

extern "C" {
// The desk test (tools/smoke): pure geometry, no device, no context.
// Exercises computeEyeMaskGeometry (eye_mask.cpp) against the Pimax
// Crystal Super's measured tangents (l=-1.529, r=1.032, d=-1.265,
// u=1.265) and the trim clamp. Bits: 1 centre and semi-axes within
// tolerance, 2 every outer ring vertex outside [-1.5,1.5] NDC, 4 masked
// fraction 6-9% at trim 0, 8 a larger fraction at trim 30, 16 the
// R0-trim<5 degree clamp holds at an extreme trim. 31 is a pass.
__declspec(dllexport) unsigned edvrEyeMaskSelftest();
}
