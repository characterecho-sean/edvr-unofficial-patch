// Bending the on-foot screen, by replacing the quad it is drawn on.
//
// WHY THIS WORKS THE WAY IT DOES
//
// A flat quad cannot curve, whatever its transform says. So the panel
// composite has to be re-issued over a finer mesh with the bend baked into
// its local-space positions -- and the whole art of it is changing nothing
// else. The game's vertex shader, pixel shader, input layout, samplers,
// blend and rasterizer state all stay bound and untouched; only the vertex
// buffer, the index buffer and the topology are swapped for one draw, and
// put back immediately after.
//
// That is possible because of what the field sessions measured (all of it in
// docs/screen-curvature.md): the composite draws the CANONICAL UNIT QUAD --
// four vertices of float3 position plus float2 UV at stride 20, corners at
// plus and minus one, z = 0 -- through a 208-byte constant buffer that sizes
// it, places it at its distance and projects it per eye. Bending coordinates
// in that space is bending the screen in its own space, and the game's own
// transform carries it through per eye for free. EDVR never learns what the
// 208 bytes mean.
//
// It composes with the panel distance fix rather than competing with it:
// that fix substitutes the CONSTANT BUFFER for the same draw, this one
// substitutes the GEOMETRY, and the substituted transform serves the
// substituted mesh exactly as it served the flat quad.
//
// THE STAGED PROOF, which is why segments is a setting and not a constant.
// The strip is numbered bottom row first and its triangles are wound with
// the game's own index pattern, so at segments = 1 and curvature = 0 the
// buffers this builds are BYTE-IDENTICAL to the game's quad and index
// buffer. That splits one ambiguous black screen into three failures that
// can be told apart:
//
//   segments = 1,  curvature = 0   the substitution MECHANISM only
//   segments = 64, curvature = 0   the grid GENERATOR
//   curvature > 0                  the bend, and the sign of z
//
// Off by default. At curvature = 0 with the default segment count nothing is
// built, nothing is bound and the composite path does not call in.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// The game's real DrawIndexedInstanced, passed in rather than looked up.
//
// The substituted draw MUST go through the original function pointer. Issuing
// it through the context's vtable would re-enter our own draw thunk, which
// would recognise the composite again and substitute again, without end. The
// pointer lives in vscreen's state; this module is handed it rather than
// keeping a second copy that could drift from it.
// Spelled in plain C++ types rather than UINT/INT so this header needs no
// windows.h. They are the same types -- UINT is unsigned int and INT is int --
// so vscreen's PFN_DrawIndexedInstanced assigns to this without a cast, which
// is the point: a cast here would paper over a signature that had drifted.
typedef void(__stdcall* PanelCurveDrawFn)(ID3D11DeviceContext*, unsigned int,
                                          unsigned int, unsigned int, int,
                                          unsigned int);

// Reads fix.panel_curvature and the two advanced keys. Both config paths,
// live -- the whole staged proof above depends on being able to walk the
// three cases without restarting the game.
void panelCurveConfigure(Config& cfg);

// Is a substitution wanted at all? False when curvature is 0 and the segment
// count is the default, and false for the rest of the session once the fault
// budget has stood the feature down.
//
// Inline: asked per draw, and the build has no /GL to fold a cross-TU
// getter for four scalar loads. kDefaultSegments lives here too, so this
// can compare against it; the .cpp's segment default and clamp keep using
// it unqualified through a using-declaration.
namespace detail {
constexpr int kDefaultSegments = 64;
extern bool  g_panelCurveStoodDown;
extern float g_panelCurveCurvature;
extern int   g_panelCurveSegments;
// What the VR world route's lines say about the curve (vr_world_route.cpp, panelCurveInfo below). Written only by panel_curve.cpp:
// the depth gain the strip in hand was built with (0 until one is), whether the substitution drew its strip at its last attempt, and how
// many strips the route's layer has re-issued (panelCurveReissue).
extern float    g_panelCurveGain;
extern bool     g_panelCurveReady;
extern uint64_t g_panelCurveReissues;
}  // namespace detail
// __forceinline, not inline: beginPanelOverride is large enough that MSVC's
// inliner declined this one and called an out-of-line copy per eye draw
// (21 innermost samples of the 1355-frame parked-5 window; the built DLL of
// 2026-09-22 round three still called it).
__forceinline bool panelCurveWants() {
    if (detail::g_panelCurveStoodDown) return false;
    // Curvature 0 at the default segment count is the shipped state and does
    // nothing at all. A non-default segment count at curvature 0 is the
    // deliberate identity test, which has to substitute in order to prove
    // anything -- so it counts as wanting.
    return detail::g_panelCurveCurvature > 0.0f ||
           detail::g_panelCurveSegments != detail::kDefaultSegments;
}

// Replace one recognised composite draw with the bent strip: save the input
// assembler state actually touched, bind ours, issue the equivalent draw,
// put the saved state back.
//
// Returns whether the game's own draw must now be SWALLOWED. False means
// nothing was substituted and the caller must forward the draw as usual --
// which is the honest answer when the buffers could not be built, and is why
// a failure here is a flat screen rather than a missing one.
//
// withMotion: also issue the screen's per-eye motion pass (screenMotionDraw) with the strip, as the game's own draw's tail does for a flat
// screen. The VR world route passes false for a frame it owns: the eye it re-issues into the layer (panelCurveReissue) goes through the
// layer-only door, no temporal pass runs for it, and the motion would be drawn for nothing -- the flat screen's tail skips it for the
// same frames (vscreen.cpp). The default is the substitution as it always was.
bool panelCurveSubstitute(ID3D11DeviceContext* ctx, PanelCurveDrawFn draw, bool withMotion = true);

// The VR world route's re-issue of a CURVED screen (ui_layer.h uiLayerWorldReissueBegin/End): the very strip panelCurveSubstitute has
// just drawn for the game's own draw, drawn once more by the same code (one helper binds the strip and issues the draw, so the two
// cannot differ), into whatever the caller has bound -- the eye's layer, the mipped screen at PS slot 0 -- with every other binding the
// game's still bound: its vertex shader, its placement constants (the panel-distance override's included, while the caller is inside
// that bracket), its rasterizer state. Nothing is learned or built here: the strip exists because the substitution just used it.
//
// panelCurveReissueReady: the strip in hand is the one the current configuration asks for and the feature has not stood down. Asked
// BEFORE the layer's bracket is opened, so a re-issue that cannot happen never opens one. panelCurveReissue returns whether it drew;
// a fault stands the whole feature down (the substitution's policy: the first fault, for the session) and returns false.
bool panelCurveReissueReady();
bool panelCurveReissue(ID3D11DeviceContext* ctx, PanelCurveDrawFn draw);

// THE SURFACE STRIP (docs\intro-video.md, 2026-10-01; job 3): the same arc for a composite that is not the on-foot screen -- the intro
// movie (EDVR places its quad) and the splash (the game's own constants). The gain and the depth direction are the CALLER's: the screen
// reads them from its own SIZE record, these two surfaces know them (the half-width of the panel in metres, and which way their placement
// matrix moves +z). It has its OWN strip, stand-down and counters; the on-foot strip, its gain, ready flag and stand-down are never touched.
//
// panelCurveSurfaceWanted: fix.panel_curvature above 0 (live) and this consumer has not stood down. False at curvature 0 whatever else is
// set -- NOT panelCurveWants(), which is also true for the identity test and would swap a placement at 0. One load of a flag that
// panelCurveConfigure and a stand-down keep (a call, not an inline: a rig that does not link panel_curve.cpp supplies its own).
// panelCurveSurfaceDraw: build (or rebuild, when curvature, columns, gain, direction or reverseU changed) the strip, bind it through the same helper
// the screen uses, issue (indices, 1, 0, 0, 0) through `draw` -- the thunk's real draw, as for the screen -- and put the game's input
// assembler state back. The strip is drawn with the game's rasterizer state but CullMode NONE (nobody has recorded this composite's index
// order or cull mode, and a wrong guess makes the surface vanish): a state equal to the game's in every field but the cull is created once
// per distinct description and kept (four are kept, the game binds a handful; a fifth pushes the oldest out and releases it), bound for the
// draw and put back with the reference released; a game state that already culls nothing
// is left alone, and so is an unbound one (the default state's cull is then the game's own to change -- NOT done here). `toward` = +1: a step
// in +z' moves toward the viewer (the splash's measured convention); -1: a step in -z' does. The strip is the screen's own arc (one
// generator builds both, so for the same curvature, columns, gain and sign the two are byte for byte the same): x' = sin(theta)/(pi c) and
// |z'| = gain (1 - cos(theta))/(pi c) with theta = pi c x, the UV from the unbent x, bottom row first, the screen's own index pattern.
// WHICH WAY u RUNS is the caller's, as the gain and the depth direction are (docs\intro-video.md, the mirror fix). The strip assigns its
// own texture coordinates, so the picture comes out the right way round only if u runs to the viewer's RIGHT, and that depends on the
// placement the strip is drawn through. reverseU false: u = (x + 1) / 2, running WITH the strip's local x -- the on-foot screen's, whose +x
// runs to the viewer's right, byte for byte what the screen's own strip has always been. reverseU true: u = (1 - x) / 2, running AGAINST x
// -- a placement whose +x runs to the viewer's LEFT, which the intro composite's does (the movie's cb2[1].x is -1/2712, the splash's
// -0.7807); the game's own six-index quad compensates in its vertex data, and the strip has to do the same. reverseU is derived from the
// caller's own placement constants (intro_curve_math.h, introPlacementXDir), NEVER a constant, and is part of the strip's key: a flip
// rebuilds the strip, an unchanged value does not. Nothing but u changes with it (the vertices' positions, v and the index pattern are
// the same, so the cull-off draw below applies to both).
// Returns false and draws NOTHING when it cannot -- at curvature 0, stood down, a null argument, a gain that is not a positive number, a
// direction that is not +-1, a strip or a state that could not be built (the caller then draws the game's own quad: flat, never missing); a
// fault stands THIS consumer down for the session and puts the game's state back; the screen's strip, gain, ready flag, stand-down and
// counters are never touched, and a fault of the screen's never stands this down.
bool panelCurveSurfaceWanted();
bool panelCurveSurfaceDraw(ID3D11DeviceContext* ctx, float gain, int toward, bool reverseU, PanelCurveDrawFn draw);
struct PanelCurveSurfaceInfo {
    uint64_t built = 0;          // strips built (a change of curvature, columns, gain, direction or reverseU builds another)
    uint64_t drawn = 0;          // strip draws issued, cumulative
    bool standDown = false;      // a fault stood this consumer down for the session
    uint64_t rasterStates = 0;   // rasterizer states created for the cull-off draw (once per distinct state of the game's, not per draw)
    bool reversed = false;       // the strip in hand runs u against x (reverseU true when it was built); false with none in hand
};
PanelCurveSurfaceInfo panelCurveSurfaceInfo();

// What the route's 5 s line and its OWNS line say. Inline over the detail state, so a rig that does not link panel_curve.cpp
// (tools\vr_world_route_gpu_test) supplies the variables the way it supplies the three above.
struct PanelCurveInfo {
    bool wanted = false;       // panelCurveWants(): a substitution is asked for (curvature above 0, or the identity test's segment count)
    bool standDown = false;    // the feature stood itself down for the session (a fault, or a SIZE that cannot be a panel's)
    bool ready = false;        // the substitution drew its strip at its last attempt (false while it is still learning the panel's SIZE)
    float curvature = 0.0f;    // fix.panel_curvature as read
    int segments = 0;          // advanced.panel_curvature_segments as read
    float gain = 0.0f;         // the depth gain the strip in hand was built with, in the panel's model units (0 before the first)
    uint64_t reissues = 0;     // strips the route's layer has re-issued, cumulative
};
inline PanelCurveInfo panelCurveInfo() {
    PanelCurveInfo i;
    i.wanted = panelCurveWants();
    i.standDown = detail::g_panelCurveStoodDown;
    i.ready = detail::g_panelCurveReady;
    i.curvature = detail::g_panelCurveCurvature;
    i.segments = detail::g_panelCurveSegments;
    i.gain = detail::g_panelCurveGain;
    i.reissues = detail::g_panelCurveReissues;
    return i;
}

// Releases the grid buffers. From the vScreen shutdown, which is the only
// place that knows the device is still alive.
void panelCurveShutdown();

}  // namespace edvr
