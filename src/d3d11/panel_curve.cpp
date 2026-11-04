#include "panel_curve.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstring>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "binding_shadow.h"
#include "screen_motion.h"

namespace edvr {

// panelCurveWants reads these from the header with no call: asked per
// draw, and the build has no /GL to fold a cross-TU getter.
// kDefaultSegments lives here too, so the inline function can compare
// against it; the alias below keeps every unqualified use in this file
// compiling unchanged.
namespace detail {
bool  g_panelCurveStoodDown = false;
float g_panelCurveCurvature = 0.0f;
int   g_panelCurveSegments = kDefaultSegments;
float    g_panelCurveGain = 0.0f;
bool     g_panelCurveReady = false;
uint64_t g_panelCurveReissues = 0;
}  // namespace detail

namespace {
using detail::kDefaultSegments;

// The measured vertex format. Twenty bytes, position then UV, and the
// static_assert is not decoration: the game's own input layout reads this
// buffer, so a compiler that padded this struct would feed the shader
// garbage with no other symptom.
struct Vertex {
    float x, y, z;
    float u, v;
};
static_assert(sizeof(Vertex) == 20, "the composite's stride is 20 bytes");

// 256 columns is far past visibility and still leaves indices comfortably
// 16-bit (514 vertices, highest index 513). 1 is not a mistake to guard
// against but the first stage of the staged proof.
constexpr int kMinSegments = 1;
constexpr int kMaxSegments = 256;

// OpenVR's convention, adopted outright: the fraction of a full circle the
// bent screen occupies. 0 is flat and off; 1 would wrap it into a closed
// cylinder, which is past useful but is the honest end of the range.
constexpr float kMaxCurvature = 1.0f;

constexpr float kPi = 3.14159265358979323846f;

// Reading the panel's SIZE uses the same staging dance the quad capture does,
// and for the same reasons: a few frames before the map so the copy has run
// and the render thread is not stalled waiting for it, and a size cap that
// refuses anything too big to be this draw's own small buffer.
constexpr uint64_t kReadbackLagMs = 50;
constexpr uint32_t kMaxBytes = 4096;

int      g_sign = 1;              // +1 or -1: which way z goes. See below.

// The first 64-column flight bent nothing: the strip was real, and the
// screen came back flat and narrowed by exactly the arc-length factor the x
// displacement predicts, with no depth whatever. A flat z probe (a constant
// added to every vertex's z; retired 2026-09-23 once it had answered) showed
// z does reach the output, so the failure was magnitude, not the shader.

// THE GAIN, and why the bend needs one at all.
//
// The composite's vertex shader, disassembled 2026-08-23 from the blob the
// game creates (vs_5C36AF051B98B9F1, the only one carrying a SIZE input):
//
//     mul r0.xy, v0.xyxx, v2.xyxx      POSITION.xy * SIZE.xy
//     mov r0.z,  v0.z                  POSITION.z, and nothing else
//
// x and y are scaled to the panel's model size by a per-draw SIZE input. z
// is passed through raw. Everything after that is an honest projective
// transform -- cb0[9..11] to world, cb1[270..273] to clip -- so z does reach
// the screen, which is what the field probe measured.
//
// But it reaches it in the WRONG UNITS. A bend of 0.44 local units at
// curvature 0.3 displaces the edges by 0.44 model units against a panel
// whose half-width is size.x model units. If size.x is tens, that is a
// percent or two of depth: real, correct, and invisible in stereo -- while
// the arc-length narrowing in x rides the scaled basis and shows at full
// strength. Exactly the field's report of a squish with no bend.
//
// So the bend is expressed in the same units as the width it bends by
// multiplying z by size.x. That is read from the game's own SIZE buffer
// rather than guessed, and overridable when the reading is not available.
float    g_zGainCfg = 0.0f;      // advanced key; 0 means "use what was read"
bool     g_sizeLearned = false;
ID3D11Buffer* g_sizeStaging = nullptr;
uint32_t g_sizeStagingBytes = 0;
bool     g_sizePending = false;
uint64_t g_sizeCopyMs = 0;
bool     g_sizeNoted = false;

// AND THE WORLD BASIS RATIO, because SIZE alone is not the width scale on
// every runtime. Measured 2026-08-23: the same 16:9 panel reads SIZE
// 35.556 x 20.000 through OpenComposite and 20.261 x 20.000 -- nearly
// SQUARE -- through native SteamVR. The game parks the content aspect in
// a different home per projection shape: when SIZE does not carry it, the
// world transform (cb0 rows 9..11, the block whose float 47 is the
// distance the panel fix scales) does, in its x basis column. A depth
// gained by SIZE.x alone is then ~1.75x too small against the visual
// width: the arc-length x compression lands at full strength while the
// edges barely come nearer -- the field's "curls at either end and looks
// squashed there" on SteamVR, while OpenComposite looked right.
//
// The gain that is correct on both is SIZE.x times |world x basis| /
// |world z basis| -- world displacement per local x over world
// displacement per local z. On a rig whose world matrix is uniform the
// ratio is 1 and this reduces to the shipped behaviour.
float    g_basisRatio = 1.0f;
bool     g_basisLearned = false;
ID3D11Buffer* g_cbStaging = nullptr;
uint32_t g_cbStagingBytes = 0;
bool     g_cbPending = false;

// SIZE.x is not the visual width on every runtime -- but SIZE.y times the
// content aspect is. Measured across the two rigs (2026-08-23):
//
//     OpenComposite   SIZE 35.556 x 20.000     x/y = 1.778 = 16:9
//     native SteamVR  SIZE 20.254 x 20.000     x/y = 1.013
//
// The SteamVR x/y is EXACTLY the headset's FOV tangent ratio (2.5617
// horizontal over 2.5296 vertical on the measured rig): on that runtime
// the game folds the FOV shape into SIZE.x and the remaining width scale
// lives further down the pipeline, where no single buffer read finds it.
// The world matrix was the first suspect and measured UNIFORM (ratio
// 1.000), which killed the theory and left the invariant: SIZE.y is
// 20.000 exactly on both rigs, and the panel's content is 16:9 by
// construction (the resolution keys accept 16:9 only). The visual
// half-width in model units is therefore SIZE.y * aspect on every
// runtime -- 35.556 on both -- which on OpenComposite is numerically
// identical to the SIZE.x the field already verified.
constexpr float kPanelAspect = 16.0f / 9.0f;
float    g_sizeY = 0.0f;

float activeGain() {
    if (g_zGainCfg > 0.0f) return g_zGainCfg;
    if (!(g_sizeLearned && g_sizeY > 0.0f)) return 0.0f;
    return g_sizeY * kPanelAspect * (g_basisLearned ? g_basisRatio : 1.0f);
}

ID3D11Buffer* g_vb = nullptr;
ID3D11Buffer* g_ib = nullptr;
uint32_t      g_indexCount = 0;
// What the buffers currently in hand were built for, so a live config edit
// rebuilds them and nothing else does.
float         g_builtCurvature = -1.0f;
int           g_builtSegments = -1;
int           g_builtSign = 0;
float         g_builtGain = -1.0f;

uint64_t g_substitutions = 0;

// The saved input-assembler state lives at module scope, NOT in the lambda
// that saves it.
//
// A fault between binding our buffers and putting the game's back would, with
// locals, leave the context holding OUR vertex buffer with the saved pointers
// gone -- and an engine that skips redundant binds would then draw its next
// geometry through a 130-vertex strip. That is a corrupted view rather than
// one bad frame, and it is the failure this whole design exists to not have.
// Kept here, the restore is still reachable from the fault path.
ID3D11Buffer*            g_savedVb = nullptr;
UINT                     g_savedStride = 0;
UINT                     g_savedOffset = 0;
ID3D11Buffer*            g_savedIb = nullptr;
DXGI_FORMAT              g_savedFmt = DXGI_FORMAT_UNKNOWN;
UINT                     g_savedIbOffset = 0;
D3D11_PRIMITIVE_TOPOLOGY g_savedTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
bool                     g_saveHeld = false;   // a restore is owed

FaultBudget g_budget("panelCurve.substitute", 5);

// THE SURFACE STRIP's own state (panel_curve.h panelCurveSurfaceDraw): its buffers and the key they were built for, its stand-down, its
// fault budget and its counters. Nothing below is the screen's: a fault in one never stands the other down, and neither reads the other's
// buffers, gain or flags.
ID3D11Buffer* g_sVb = nullptr;
ID3D11Buffer* g_sIb = nullptr;
uint32_t      g_sIndexCount = 0;
float         g_sBuiltCurvature = -1.0f;   // the key: curvature, columns, gain, direction and which way u runs
int           g_sBuiltSegments = -1;
float         g_sBuiltGain = -1.0f;
int           g_sBuiltToward = 0;
bool          g_sBuiltReverseU = false;
bool          g_sStoodDown = false;
bool          g_sWanted = false;           // curvature above 0 and not stood down -- the one flag panelCurveSurfaceWanted() loads
uint64_t      g_sBuilt = 0;
uint64_t      g_sDrawn = 0;
uint64_t      g_sRasterStates = 0;
FaultBudget   g_sBudget("panelCurve.surface", 5);

// The game's rasterizer state while the surface strip is drawn. Held at module scope for the reason the input assembler's is: a fault
// between swapping it and putting it back must still find it. g_sRsHeld: a reference on the game's state is owed a Release; g_sRsSwapped:
// the context holds our cull-off copy and the game's is owed a bind. Both clear in restoreSurfaceRs.
ID3D11RasterizerState* g_sSavedRs = nullptr;
bool                   g_sRsHeld = false;
bool                   g_sRsSwapped = false;

// The game's rasterizer states with the cull off, one per distinct description (the game binds a handful, and the same description from
// another object derives the same state), kept for the session so a draw creates nothing after the first. Keyed by the DESCRIPTION, not by
// the pointer: a state the game destroyed and a new one made at the same address would otherwise be taken for it.
constexpr int kCullOffCache = 4;
struct CullOffState {
    D3D11_RASTERIZER_DESC game{};            // the description this was derived from
    ID3D11RasterizerState* off = nullptr;    // the same description with CullMode NONE
};
CullOffState g_sCullOff[kCullOffCache];
int          g_sCullOffNext = 0;

// Put the game's input assembler back and drop the references. Safe to call
// when nothing is held, which is what makes it usable from the fault path
// without first having to work out how far the substitution got.
void restoreSaved(ID3D11DeviceContext* ctx) {
    if (!g_saveHeld) return;
    g_saveHeld = false;
    // A null buffer is a legitimate restore -- it is the truth when the game
    // had nothing bound -- and IASet* accepts it.
    ctx->IASetVertexBuffers(0, 1, &g_savedVb, &g_savedStride, &g_savedOffset);
    ctx->IASetIndexBuffer(g_savedIb, g_savedFmt, g_savedIbOffset);
    ctx->IASetPrimitiveTopology(g_savedTopo);
    if (g_savedVb) { g_savedVb->Release(); g_savedVb = nullptr; }
    if (g_savedIb) { g_savedIb->Release(); g_savedIb = nullptr; }
}

// THE STRIP'S DRAW, shared by the substitution and the VR world route's re-issue so that the two draws of one strip cannot differ:
// the game's input assembler saved, ours bound, the one draw issued through the ORIGINAL pointer, and it returns with the game's state
// STILL HELD (g_saveHeld) -- the substitution then issues its motion pass with the strip bound, and both end in restoreSaved. The
// calls and their order are what panelCurveSubstitute has always made. The strip is a parameter, defaulting to the screen's own (so the
// screen's calls are the calls they were), and the surface strip's draw passes its own: one helper binds either, and the two cannot differ.
void drawStripHeld(ID3D11DeviceContext* ctx, PanelCurveDrawFn draw, ID3D11Buffer* vb = g_vb, ID3D11Buffer* ib = g_ib,
                   uint32_t indexCount = g_indexCount) {
    // Save exactly what is about to be changed and nothing else. None of
    // these slots goes through an EDVR hook, so there is no shadow to
    // consult and none to confuse: the context is the only authority on
    // them, and IAGet* is how it is asked.
    ctx->IAGetVertexBuffers(0, 1, &g_savedVb, &g_savedStride, &g_savedOffset);
    ctx->IAGetIndexBuffer(&g_savedIb, &g_savedFmt, &g_savedIbOffset);
    ctx->IAGetPrimitiveTopology(&g_savedTopo);
    g_saveHeld = true;

    ID3D11Buffer* ours = vb;
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ctx->IASetVertexBuffers(0, 1, &ours, &stride, &offset);
    ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Through the ORIGINAL function pointer. The context's vtable entry is
    // our own thunk, and calling it here would recognise this composite
    // again and substitute again, without end.
    draw(ctx, indexCount, 1, 0, 0, 0);
}

// The strip in hand is the one the current configuration and the learned gain ask for.
bool stripCurrent() {
    return g_vb && g_ib && g_builtCurvature == detail::g_panelCurveCurvature &&
           g_builtSegments == detail::g_panelCurveSegments && g_builtSign == g_sign && g_builtGain == activeGain();
}

// Stand down permanently on the FIRST fault, rather than spending a
// budget of five. The other users of guardedBudget in this tree are
// observers, where retrying costs a log line; this one has the
// player's view riding on it, and a substitution that faulted once has
// no business being attempted again mid-flight.
void standDownAfterFault(ID3D11DeviceContext* ctx, const char* what) {
    detail::g_panelCurveStoodDown = true;
    detail::g_panelCurveReady = false;
    // And put the game's state back, which is the whole reason the saved
    // state is not a set of locals. Under its own guard: if the context is
    // far enough gone that restoring faults too, there is nothing further
    // to be done and the process should survive to say so.
    guarded("panelCurve.restore", [&] { restoreSaved(ctx); });
    Log::get().note(
        "panel curvature: %s faulted, so it is off for the rest "
        "of this session and the game's own quad is drawn again. The input "
        "assembler was put back, so the screen should look exactly as it did "
        "before -- if it does not, restart the game and report the log.",
        what);
}

// Put the game's rasterizer state back and give the reference up. Safe to call when nothing is held, as restoreSaved is, so the fault path
// can call it without working out how far the draw got.
void restoreSurfaceRs(ID3D11DeviceContext* ctx) {
    if (!g_sRsHeld) return;
    g_sRsHeld = false;
    const bool swapped = g_sRsSwapped;
    g_sRsSwapped = false;
    // A null state is a legitimate restore -- it is the truth when the game had none bound -- and RSSetState accepts it.
    if (swapped) ctx->RSSetState(g_sSavedRs);
    if (g_sSavedRs) { g_sSavedRs->Release(); g_sSavedRs = nullptr; }
}

// The surface strip has faulted: this consumer is off for the session, the game's state is put back (the input assembler and the
// rasterizer state, each under a guard of its own so a fault in one does not leave the other unrestored), and nothing of the screen's is
// touched -- its flags, its strip, its budget.
void standDownSurfaceAfterFault(ID3D11DeviceContext* ctx) {
    g_sStoodDown = true;
    g_sWanted = false;
    guarded("panelCurve.surface.restore", [&] { restoreSaved(ctx); });
    guarded("panelCurve.surface.restoreRs", [&] { restoreSurfaceRs(ctx); });
    Log::get().note(
        "panel curvature: the surface strip (the intro movie and the splash) faulted, so those two are flat for the rest of this session and "
        "the game's own quad is drawn again. The input assembler and the rasterizer state were put back; the on-foot screen's curve is "
        "untouched.");
}

// The game's rasterizer state with the cull off: found in the cache by its description, or created (once) and kept. Null when it cannot
// be made, which the caller turns into "draw nothing" -- a strip drawn with the game's cull and a winding nobody recorded could vanish.
ID3D11RasterizerState* cullOffFor(ID3D11DeviceContext* ctx, const D3D11_RASTERIZER_DESC& game) {
    for (const CullOffState& s : g_sCullOff) {
        if (s.off && memcmp(&s.game, &game, sizeof(game)) == 0) return s.off;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    D3D11_RASTERIZER_DESC d = game;
    d.CullMode = D3D11_CULL_NONE;
    ID3D11RasterizerState* off = nullptr;
    const HRESULT hr = dev->CreateRasterizerState(&d, &off);
    dev->Release();
    if (FAILED(hr) || !off) return nullptr;
    CullOffState& slot = g_sCullOff[g_sCullOffNext];
    g_sCullOffNext = (g_sCullOffNext + 1) % kCullOffCache;
    if (slot.off) slot.off->Release();
    slot.game = game;
    slot.off = off;
    ++g_sRasterStates;
    return off;
}

// The surface strip in hand is the one the live curvature and columns and the caller's gain, direction and way for u ask for.
bool surfaceCurrent(float gain, int toward, bool reverseU) {
    return g_sVb && g_sIb && g_sBuiltCurvature == detail::g_panelCurveCurvature && g_sBuiltSegments == detail::g_panelCurveSegments &&
           g_sBuiltGain == gain && g_sBuiltToward == toward && g_sBuiltReverseU == reverseU;
}

// Which way +z points in the panel's local space. MEASURED 2026-08-23, and
// the last thing about this geometry that reading it could not settle.
//
// It came back AWAY from the viewer: at curvature 0.3 the screen receded
// instead of wrapping. So the bend is computed against +z, which is what
// makes panel_curvature_sign = 1 -- the default -- mean the direction the
// feature is actually named for. The setting survives as an escape hatch in
// the idiom of panel_distance_index: a documented number to change if a game
// update moves the fact underneath it.
constexpr float kTowardViewer = -1.0f;

// The bend, in the space the capture measured rather than the space the
// design was first written in.
//
//   theta = pi * c * x                x in -1..1, the local coordinate
//   x'    = sin(theta) / (pi * c)     arc length preserved
//   z'    = -(1 - cos(theta)) / (pi * c)    negative: see kTowardViewer
//
// The radius is 1/(pi*c) because the arc has to keep the flat quad's local
// width of 2, which is what makes the image the same size whether it is bent
// or not. At x = +/-1 the sweep is 2*pi*c: 36 degrees at c = 0.1, a gentle
// theatre curve. As c approaches zero this degenerates to x' = x and z' = 0,
// which is why c = 0 is both the off switch and the identity test rather
// than a special case in the code.
void bend(float x, float c, int sign, float gain, float* xOut, float* zOut) {
    if (c <= 0.0f) {
        *xOut = x;
        *zOut = 0.0f;
        return;
    }
    const float k = kPi * c;
    const float theta = k * x;
    *xOut = sinf(theta) / k;
    // Gained into the panel's own model units; see activeGain above.
    *zOut = kTowardViewer * static_cast<float>(sign) * gain *
            (1.0f - cosf(theta)) / k;
}

// Learn size.x from the game's own SIZE buffer.
//
// SIZE arrives at input register 2 and cannot fit the 20-byte stride slot 0
// carries, so it rides another vertex buffer slot -- one the substitution
// never touches and never should. This finds it, copies it, and reads the
// first float2 a few frames later, the same staging dance the quad capture
// uses and for the same reason: mapping a copy in the frame it was queued
// stalls the render thread.
//
// Returns whether the gain is ready. Until it is, the caller forwards the
// game's own draw -- a flat screen while a comfort feature settles, never a
// broken one.
bool learnSize(ID3D11DeviceContext* ctx) {
    if (g_zGainCfg > 0.0f) return true;   // the override needs nothing read

    if (g_sizePending) {
        if (!g_sizeStaging || nowMs() - g_sizeCopyMs < kReadbackLagMs) return false;
        // The world basis first, so the SIZE note below can report the
        // gain that will actually be used. A failed or degenerate basis
        // read leaves the ratio at 1 -- the shipped behaviour -- rather
        // than standing anything down: SIZE alone was field-correct on
        // one runtime of two.
        if (g_cbPending && g_cbStaging) {
            D3D11_MAPPED_SUBRESOURCE cm{};
            if (SUCCEEDED(ctx->Map(g_cbStaging, 0, D3D11_MAP_READ, 0, &cm)) &&
                cm.pData) {
                if (g_cbStagingBytes >= 47 * 4 + 4) {
                    const float* f = static_cast<const float*>(cm.pData);
                    const float x0 = f[36], x1 = f[40], x2 = f[44];
                    const float z0 = f[38], z1 = f[42], z2 = f[46];
                    const float lx = sqrtf(x0 * x0 + x1 * x1 + x2 * x2);
                    const float lz = sqrtf(z0 * z0 + z1 * z1 + z2 * z2);
                    if (lx > 1e-4f && lz > 1e-4f) {
                        float r = lx / lz;
                        if (r < 0.2f) r = 0.2f;
                        if (r > 5.0f) r = 5.0f;
                        g_basisRatio = r;
                        g_basisLearned = true;
                        Log::get().note(
                            "panel curvature: the panel's world transform scales "
                            "x by %.3f and z by %.3f (cb0 rows 9..11), so the "
                            "depth gain carries their ratio %.3f. On this runtime "
                            "the game parks part of the width scale here rather "
                            "than in SIZE -- without the ratio the bend crumples "
                            "at the edges instead of curving (the SteamVR-vs-"
                            "OpenComposite report).",
                            lx, lz, r);
                    }
                }
                ctx->Unmap(g_cbStaging, 0);
            }
            g_cbPending = false;
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(g_sizeStaging, 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
            float sz[2] = {0.0f, 0.0f};
            memcpy(sz, m.pData, sizeof(sz));
            ctx->Unmap(g_sizeStaging, 0);
            if (sz[0] > 0.0f && sz[1] > 0.0f) {
                g_sizeY = sz[1];
                g_sizeLearned = true;
                Log::get().note(
                    "panel curvature: the panel's SIZE is %.3f x %.3f in model "
                    "units, read from the buffer the composite's shader scales its "
                    "x and y by. The bend's depth is gained by %.3f -- SIZE.y "
                    "times the 16:9 content aspect times the world-basis ratio "
                    "%.3f -- the visual half-width, which SIZE.x is not on every "
                    "runtime (it carries the FOV shape on native SteamVR).",
                    sz[0], sz[1], activeGain(), g_basisLearned ? g_basisRatio : 1.0f);
            } else {
                Log::get().note(
                    "panel curvature: the SIZE buffer read back %.3f x %.3f, which "
                    "cannot be a panel size. The bend needs a gain in model units "
                    "and there is none to be had, so it stands down. Set "
                    "advanced.panel_curvature_z_gain to supply one by hand.",
                    sz[0], sz[1]);
                detail::g_panelCurveStoodDown = true;
            }
        }
        g_sizePending = false;
        return g_sizeLearned;
    }
    if (g_sizeLearned) return true;

    // Slots 1..3: the first bound one, preferring a float2 stride, which is
    // what a two-component SIZE is.
    ID3D11Buffer* vbs[3] = {nullptr, nullptr, nullptr};
    UINT strides[3] = {0, 0, 0}, offsets[3] = {0, 0, 0};
    ctx->IAGetVertexBuffers(1, 3, vbs, strides, offsets);
    int pick = -1;
    for (int i = 0; i < 3; ++i) {
        if (vbs[i] && strides[i] == 8) { pick = i; break; }
    }
    if (pick < 0) {
        for (int i = 0; i < 3; ++i) {
            if (vbs[i]) { pick = i; break; }
        }
    }
    if (pick < 0) {
        if (!g_sizeNoted) {
            g_sizeNoted = true;
            Log::get().note(
                "panel curvature: nothing is bound to vertex slots 1..3, so the "
                "panel's SIZE cannot be read and the bend has no gain to put it in "
                "model units. Standing down. advanced.panel_curvature_z_gain "
                "supplies one by hand if this build binds it elsewhere.");
            detail::g_panelCurveStoodDown = true;
        }
        return false;
    }

    ResourceInfo info;
    const bool known = bindingResolveResource(vbs[pick], &info) && info.isBuffer;
    const uint32_t bytes = known ? info.a : 0;
    if (bytes >= 8 && bytes <= kMaxBytes) {
        if (g_sizeStaging && g_sizeStagingBytes != bytes) {
            g_sizeStaging->Release();
            g_sizeStaging = nullptr;
            g_sizeStagingBytes = 0;
        }
        if (!g_sizeStaging) {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (dev) {
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = bytes;
                bd.Usage = D3D11_USAGE_STAGING;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                dev->CreateBuffer(&bd, nullptr, &g_sizeStaging);
                dev->Release();
                if (g_sizeStaging) g_sizeStagingBytes = bytes;
            }
        }
        if (g_sizeStaging) {
            // The stream offset is deliberately ignored: this reads the buffer's
            // first record, and a SIZE buffer that needed an offset would be a
            // pooled one, which the size cap above has already refused.
            ctx->CopyResource(g_sizeStaging, vbs[pick]);
            g_sizeCopyMs = nowMs();
            g_sizePending = true;
            if (!g_sizeNoted) {
                g_sizeNoted = true;
                Log::get().note(
                    "panel curvature: reading the panel's SIZE from vertex slot %d "
                    "(%u bytes, stride %u). The bend has to be gained into model "
                    "units and this is where the game keeps them.",
                    pick + 1, bytes, strides[pick]);
            }

            // The world transform rides the same readback: the composite's
            // cb0 (the 208-byte block whose float 47 is the panel's
            // distance) holds rows 9..11, and the ratio of its x and z
            // basis magnitudes is the part of the gain SIZE does not
            // carry on every runtime. Best-effort: a failed read leaves
            // the ratio at 1, the shipped behaviour.
            ID3D11Buffer* cb = nullptr;
            ctx->VSGetConstantBuffers(0, 1, &cb);
            if (cb) {
                ResourceInfo ci;
                const bool cknown =
                    bindingResolveResource(cb, &ci) && ci.isBuffer;
                const uint32_t cbytes = cknown ? ci.a : 0;
                if (cbytes >= 47 * 4 + 4 && cbytes <= kMaxBytes) {
                    if (g_cbStaging && g_cbStagingBytes != cbytes) {
                        g_cbStaging->Release();
                        g_cbStaging = nullptr;
                        g_cbStagingBytes = 0;
                    }
                    if (!g_cbStaging) {
                        ID3D11Device* dev = nullptr;
                        ctx->GetDevice(&dev);
                        if (dev) {
                            D3D11_BUFFER_DESC bd{};
                            bd.ByteWidth = cbytes;
                            bd.Usage = D3D11_USAGE_STAGING;
                            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                            dev->CreateBuffer(&bd, nullptr, &g_cbStaging);
                            dev->Release();
                            if (g_cbStaging) g_cbStagingBytes = cbytes;
                        }
                    }
                    if (g_cbStaging) {
                        ctx->CopyResource(g_cbStaging, cb);
                        g_cbPending = true;
                    }
                }
                cb->Release();
            }
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (vbs[i]) vbs[i]->Release();
    }
    return false;
}

// THE STRIP'S GEOMETRY, one generator for the screen and for the surfaces (panelCurveSurfaceDraw) so that the two cannot differ: for the
// same column count, curvature, sign and gain (and reverseU false) they are byte for byte the same vertices and indices. n columns are
// 2(n+1) vertices and 6n indices, vb and ib sized for the most there are. reverseU is the one thing a surface chooses that the screen never
// does: which way u runs along x (the screen's strip always passes false; panel_curve.h says who decides it for a surface and why).
void fillStrip(int n, float curvature, int sign, float gain, bool reverseU, Vertex* vb, unsigned short* ib) {
    for (int i = 0; i <= n; ++i) {
        // The ORIGINAL x drives the UV, and the bent one only the position:
        // the bend moves where a column is, never which texel it shows.
        const float x = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(n);
        float bx = 0.0f, bz = 0.0f;
        bend(x, curvature, sign, gain, &bx, &bz);
        const float u = reverseU ? (1.0f - x) * 0.5f : (x + 1.0f) * 0.5f;

        // Bottom row first, then the top -- the game's own ordering, which is
        // what makes segments = 1 come out byte-identical to its quad. The
        // capture measured v = 1 at y = -1 and v = 0 at y = +1, so V falls as
        // Y rises; the other convention renders the screen upside down.
        vb[i] = Vertex{bx, -1.0f, bz, u, 1.0f};
        vb[(n + 1) + i] = Vertex{bx, 1.0f, bz, u, 0.0f};
    }

    // The game's own index pattern, per quad, so the winding is right by
    // construction rather than by reasoning about cross products. Measured:
    // 0,3,1 then 0,2,3 -- which with the corners those vertices sit at reads
    // as BL,TR,BR then BL,TL,TR. Culling is on (back faces, clockwise front),
    // so this is load-bearing: the other way round is culled entirely and
    // the screen goes black.
    uint32_t w = 0;
    for (int i = 0; i < n; ++i) {
        const unsigned short bl = static_cast<unsigned short>(i);
        const unsigned short br = static_cast<unsigned short>(i + 1);
        const unsigned short tl = static_cast<unsigned short>((n + 1) + i);
        const unsigned short tr = static_cast<unsigned short>((n + 1) + i + 1);
        ib[w++] = bl; ib[w++] = tr; ib[w++] = br;
        ib[w++] = bl; ib[w++] = tl; ib[w++] = tr;
    }
}

// The strip's two buffers, replacing the pair in outVb/outIb (released first, so a failure leaves neither). False if the device cannot be
// had or either buffer cannot be made. The screen's strip and the surface strip are made by this one function.
bool createStrip(ID3D11DeviceContext* ctx, const Vertex* vb, uint32_t verts, const unsigned short* ib, uint32_t idxs,
                 ID3D11Buffer*& outVb, ID3D11Buffer*& outIb) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;

    if (outVb) { outVb->Release(); outVb = nullptr; }
    if (outIb) { outIb->Release(); outIb = nullptr; }

    D3D11_BUFFER_DESC bd{};
    D3D11_SUBRESOURCE_DATA sd{};

    bd.ByteWidth = verts * sizeof(Vertex);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    sd.pSysMem = vb;
    HRESULT hr = dev->CreateBuffer(&bd, &sd, &outVb);

    if (SUCCEEDED(hr)) {
        bd.ByteWidth = idxs * sizeof(unsigned short);
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        sd.pSysMem = ib;
        hr = dev->CreateBuffer(&bd, &sd, &outIb);
    }
    dev->Release();

    if (FAILED(hr) || !outVb || !outIb) {
        if (outVb) { outVb->Release(); outVb = nullptr; }
        if (outIb) { outIb->Release(); outIb = nullptr; }
        return false;
    }
    return true;
}

// Build the strip. Returns false if anything failed, which the caller turns
// into "forward the game's draw", not into a missing screen.
//
// A STRIP, not a mesh: the bend is constant along y, so two rows are all the
// geometry there is to have. N columns is 2(N+1) vertices and 6N indices --
// 130 and 384 at the default.
bool build(ID3D11DeviceContext* ctx) {
    const int n = detail::g_panelCurveSegments;
    const uint32_t verts = static_cast<uint32_t>(2 * (n + 1));
    const uint32_t idxs = static_cast<uint32_t>(6 * n);

    Vertex vb[2 * (kMaxSegments + 1)];
    unsigned short ib[6 * kMaxSegments];

    fillStrip(n, detail::g_panelCurveCurvature, g_sign, activeGain(), false, vb, ib);   // the screen's u runs with x: its +x runs to the viewer's right

    if (!createStrip(ctx, vb, verts, ib, idxs, g_vb, g_ib)) return false;

    g_indexCount = idxs;
    g_builtCurvature = detail::g_panelCurveCurvature;
    g_builtSegments = detail::g_panelCurveSegments;
    g_builtSign = g_sign;
    g_builtGain = activeGain();
    detail::g_panelCurveGain = g_builtGain;
    Log::get().note(
        "panel curvature: built a %d-column strip -- %u vertices, %u indices -- at "
        "curvature %.3f, depth sign %+d. At curvature 0 and 1 column this is the "
        "game's own quad to the byte, which is what makes a difference on screen "
        "there a fault in the substitution rather than in the geometry.",
        detail::g_panelCurveSegments, verts, idxs, detail::g_panelCurveCurvature, g_sign);
    return true;
}

// Build the surface strip: the same generator as the screen's, with the caller's gain and the sign its direction asks for. toward = +1
// means a step in +z' moves toward the viewer; the generator's z is -sign * gain * (1 - cos)/(pi c) (negative is toward the viewer for the
// screen, whose measured convention that is), so +z' toward the viewer is sign -1. reverseU: u runs against x (the caller's placement has
// +x running to the viewer's left). False if the buffers cannot be made.
bool buildSurface(ID3D11DeviceContext* ctx, float gain, int toward, bool reverseU) {
    const int n = detail::g_panelCurveSegments;
    const uint32_t verts = static_cast<uint32_t>(2 * (n + 1));
    const uint32_t idxs = static_cast<uint32_t>(6 * n);

    Vertex vb[2 * (kMaxSegments + 1)];
    unsigned short ib[6 * kMaxSegments];

    fillStrip(n, detail::g_panelCurveCurvature, -toward, gain, reverseU, vb, ib);

    if (!createStrip(ctx, vb, verts, ib, idxs, g_sVb, g_sIb)) return false;

    g_sIndexCount = idxs;
    g_sBuiltCurvature = detail::g_panelCurveCurvature;
    g_sBuiltSegments = detail::g_panelCurveSegments;
    g_sBuiltGain = gain;
    g_sBuiltToward = toward;
    g_sBuiltReverseU = reverseU;
    ++g_sBuilt;
    Log::get().note(
        "panel curvature: built a %d-column SURFACE strip -- %u vertices, %u indices -- at curvature %.3f, gain %.3f m, a step in +z' moving "
        "%s the viewer, u running %s. It serves the intro movie and the splash; the on-foot screen's strip is separate.",
        n, verts, idxs, detail::g_panelCurveCurvature, gain, toward > 0 ? "toward" : "away from",
        reverseU ? "against x: the placement's +x runs to the viewer's left" : "with x");
    return true;
}

}  // namespace

void panelCurveConfigure(Config& cfg) {
    const float wasCurve = detail::g_panelCurveCurvature;
    const int wasSeg = detail::g_panelCurveSegments;
    const int wasSign = g_sign;

    float c = cfg.getFloat("fix.panel_curvature", 0.0f);
    if (c < 0.0f || c > kMaxCurvature) {
        Log::get().note(
            "panel curvature: %.3f is outside 0..%.1f -- the number is the fraction "
            "of a full circle the screen wraps, so 0.1 is a gentle theatre curve and "
            "1.0 is a closed cylinder. Treating it as off.",
            c, kMaxCurvature);
        c = 0.0f;
    }
    detail::g_panelCurveCurvature = c;
    g_sWanted = c > 0.0f && !g_sStoodDown;   // the surface strip follows the live curvature, and nothing else of the screen's
    detail::g_panelCurveSegments = cfg.getIntInRange("advanced.panel_curvature_segments",
                                   kDefaultSegments, kMinSegments, kMaxSegments);
    // Which way the bend goes. 1 is toward the viewer and is correct on the
    // build this was measured against; -1 is the escape hatch if a game
    // update ever flips the handedness of the panel's transform. See
    // kTowardViewer for what was measured and how.
    g_sign = cfg.getIntInRange("advanced.panel_curvature_sign", 1, -1, 1) < 0 ? -1 : 1;
    g_zGainCfg = cfg.getFloat("advanced.panel_curvature_z_gain", 0.0f);
    if (g_zGainCfg < 0.0f || g_zGainCfg > 10000.0f) g_zGainCfg = 0.0f;

    // A strip of N columns has N-1 INTERIOR vertex columns, and the whole
    // bend lives in those: the two edges receive the SAME z whatever the
    // curvature, because cos is even. So at one column there is no curve at
    // all -- the quad stays flat and merely moves in depth and narrows in x,
    // which looks exactly like the screen receding and reads as the bend
    // going the wrong way. That cost the first flight, and the log said
    // nothing because nothing was wrong.
    if (detail::g_panelCurveCurvature > 0.0f && detail::g_panelCurveSegments < 8) {
        Log::get().note(
            "panel curvature: %d column%s is too few to bend anything. The curve "
            "lives in the INTERIOR vertex columns and %d columns has %d of them; "
            "the two edges always get the same depth, because cos is even. So the "
            "screen will stay FLAT and merely move away and narrow, which looks "
            "like it receding rather than curving. Raise "
            "advanced.panel_curvature_segments to 64 to see the bend.",
            detail::g_panelCurveSegments, detail::g_panelCurveSegments == 1 ? "" : "s", detail::g_panelCurveSegments, detail::g_panelCurveSegments - 1);
    }

    if (detail::g_panelCurveCurvature != wasCurve || detail::g_panelCurveSegments != wasSeg || g_sign != wasSign) {
        if (detail::g_panelCurveCurvature > 0.0f) {
            Log::get().note(
                "panel curvature: %.3f of a circle over %d columns, depth sign %+d. "
                "The screen bends toward you and keeps its width, so the edges come "
                "nearer rather than the middle going further.",
                detail::g_panelCurveCurvature, detail::g_panelCurveSegments, g_sign);
        } else if (detail::g_panelCurveSegments != kDefaultSegments) {
            Log::get().note(
                "panel curvature: off (0), but the segment count is %d rather than "
                "the default, so the FLAT strip is substituted anyway. That is the "
                "identity test: the screen must look exactly as it does without "
                "EDVR. Set the segment count back to %d to stop substituting.",
                detail::g_panelCurveSegments, kDefaultSegments);
        } else {
            Log::get().note("panel curvature: off; the game's own quad is drawn.");
        }
    }
}

bool panelCurveSubstitute(ID3D11DeviceContext* ctx, PanelCurveDrawFn draw, bool withMotion) {
    if (!ctx || !draw || detail::g_panelCurveStoodDown) { detail::g_panelCurveReady = false; return false; }

    bool substituted = false;
    const bool ok = guardedBudget(g_budget, [&] {
        if (!learnSize(ctx)) return;
        if (!stripCurrent()) {
            if (!build(ctx)) return;
        }

        drawStripHeld(ctx, draw);
        if (withMotion) {
            const float shape[4]={kPi*g_builtCurvature,float(g_builtSegments),
                kTowardViewer*float(g_builtSign)*g_builtGain,0.0f};
            screenMotionDraw(ctx,draw,g_indexCount,1,0,0,0,shape);
        }

        restoreSaved(ctx);

        substituted = true;
        if (++g_substitutions == 1) {
            Log::get().note(
                "panel curvature: substituting the panel's quad for a %d-column "
                "strip at curvature %.3f. If the screen is black from here, the "
                "winding is inverted; if it is flat with several columns, the bend "
                "is being cancelled by the transform; if it bows AWAY from you, "
                "this build's handedness differs from the one this was measured "
                "on and advanced.panel_curvature_sign = -1 is the fix.",
                detail::g_panelCurveSegments, detail::g_panelCurveCurvature);
        }
    });

    if (!ok) {
        standDownAfterFault(ctx, "the substitution");
        return false;
    }
    detail::g_panelCurveReady = substituted;
    return substituted;
}

bool panelCurveReissueReady() {
    return panelCurveWants() && stripCurrent();
}

bool panelCurveReissue(ID3D11DeviceContext* ctx, PanelCurveDrawFn draw) {
    if (!ctx || !draw || !panelCurveReissueReady()) return false;

    bool drawn = false;
    const bool ok = guardedBudget(g_budget, [&] {
        drawStripHeld(ctx, draw);
        restoreSaved(ctx);
        drawn = true;
    });
    if (!ok) {
        standDownAfterFault(ctx, "the VR world route's re-issue of the strip");
        return false;
    }
    if (++detail::g_panelCurveReissues == 1) {
        Log::get().note(
            "panel curvature: the VR world route's layer drew the same %d-column strip at curvature %.3f that the game's own draw is "
            "substituted with, from the mipped resolved screen: the layer's bend and placement are the game's by construction (one "
            "helper binds the strip for both). From here the route owns a curved screen.",
            detail::g_panelCurveSegments, detail::g_panelCurveCurvature);
    }
    return drawn;
}

bool panelCurveSurfaceWanted() {
    return g_sWanted;
}

bool panelCurveSurfaceDraw(ID3D11DeviceContext* ctx, float gain, int toward, bool reverseU, PanelCurveDrawFn draw) {
    if (!ctx || !draw || !g_sWanted) return false;
    // A gain that is not a positive number of metres, or a direction that is not one of the two, is a caller that does not know its
    // surface: flat is the honest answer, and nothing is built from it.
    if (!(gain > 0.0f && gain < 1.0e6f) || (toward != 1 && toward != -1)) return false;

    bool drawn = false;
    const bool ok = guardedBudget(g_sBudget, [&] {
        if (!surfaceCurrent(gain, toward, reverseU)) {
            if (!buildSurface(ctx, gain, toward, reverseU)) return;
        }

        // The game's rasterizer state, with the cull off for this draw (panel_curve.h says why). Nothing bound, or a state that culls
        // nothing already, changes nothing; a state that cannot be derived means no draw at all rather than a draw that may vanish.
        ctx->RSGetState(&g_sSavedRs);
        g_sRsHeld = true;
        if (g_sSavedRs) {
            D3D11_RASTERIZER_DESC game{};
            g_sSavedRs->GetDesc(&game);
            if (game.CullMode != D3D11_CULL_NONE) {
                ID3D11RasterizerState* off = cullOffFor(ctx, game);
                if (!off) {
                    restoreSurfaceRs(ctx);
                    return;
                }
                ctx->RSSetState(off);
                g_sRsSwapped = true;
            }
        }

        drawStripHeld(ctx, draw, g_sVb, g_sIb, g_sIndexCount);
        restoreSaved(ctx);
        restoreSurfaceRs(ctx);

        drawn = true;
        ++g_sDrawn;
    });

    if (!ok) {
        standDownSurfaceAfterFault(ctx);
        return false;
    }
    return drawn;
}

PanelCurveSurfaceInfo panelCurveSurfaceInfo() {
    PanelCurveSurfaceInfo i;
    i.built = g_sBuilt;
    i.drawn = g_sDrawn;
    i.standDown = g_sStoodDown;
    i.rasterStates = g_sRasterStates;
    i.reversed = g_sVb && g_sBuiltReverseU;   // the direction of the strip in hand: false with none in hand
    return i;
}

void panelCurveShutdown() {
    // Any held references are dropped WITHOUT touching the context: shutdown
    // runs when the device may already be going away, and the bindings are
    // about to stop mattering. Releasing is still owed.
    //
    // The surface strip's first, the same way: its saved rasterizer state, the cull-off states, its buffers and the key they were built for.
    // Its counters and its stand-down are the session's and stay.
    g_sRsHeld = false;
    g_sRsSwapped = false;
    if (g_sSavedRs) { g_sSavedRs->Release(); g_sSavedRs = nullptr; }
    for (CullOffState& s : g_sCullOff) {
        if (s.off) { s.off->Release(); s.off = nullptr; }
        s.game = D3D11_RASTERIZER_DESC{};
    }
    g_sCullOffNext = 0;
    if (g_sVb) { g_sVb->Release(); g_sVb = nullptr; }
    if (g_sIb) { g_sIb->Release(); g_sIb = nullptr; }
    g_sIndexCount = 0;
    g_sBuiltCurvature = -1.0f;
    g_sBuiltSegments = -1;
    g_sBuiltGain = -1.0f;
    g_sBuiltToward = 0;
    g_sBuiltReverseU = false;
    g_saveHeld = false;
    if (g_savedVb) { g_savedVb->Release(); g_savedVb = nullptr; }
    if (g_savedIb) { g_savedIb->Release(); g_savedIb = nullptr; }
    if (g_sizeStaging) {
        g_sizeStaging->Release();
        g_sizeStaging = nullptr;
        g_sizeStagingBytes = 0;
    }
    if (g_cbStaging) {
        g_cbStaging->Release();
        g_cbStaging = nullptr;
        g_cbStagingBytes = 0;
        g_cbPending = false;
    }
    if (g_vb) { g_vb->Release(); g_vb = nullptr; }
    if (g_ib) { g_ib->Release(); g_ib = nullptr; }
    g_indexCount = 0;
    g_builtCurvature = -1.0f;
    g_builtSegments = -1;
    g_builtSign = 0;
    detail::g_panelCurveReady = false;
    detail::g_panelCurveGain = 0.0f;
}

}  // namespace edvr
