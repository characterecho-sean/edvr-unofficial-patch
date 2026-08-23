#include "panel_curve.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"

namespace edvr {
namespace {

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
constexpr int kDefaultSegments = 64;

// OpenVR's convention, adopted outright: the fraction of a full circle the
// bent screen occupies. 0 is flat and off; 1 would wrap it into a closed
// cylinder, which is past useful but is the honest end of the range.
constexpr float kMaxCurvature = 1.0f;

constexpr float kPi = 3.14159265358979323846f;

float    g_curvature = 0.0f;
int      g_segments = kDefaultSegments;
int      g_sign = 1;              // +1 or -1: which way z goes. See below.
bool     g_stoodDown = false;     // a fault took the feature out for good

ID3D11Buffer* g_vb = nullptr;
ID3D11Buffer* g_ib = nullptr;
uint32_t      g_indexCount = 0;
// What the buffers currently in hand were built for, so a live config edit
// rebuilds them and nothing else does.
float         g_builtCurvature = -1.0f;
int           g_builtSegments = -1;
int           g_builtSign = 0;

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
void bend(float x, float c, int sign, float* xOut, float* zOut) {
    if (c <= 0.0f) {
        *xOut = x;
        *zOut = 0.0f;
        return;
    }
    const float k = kPi * c;
    const float theta = k * x;
    *xOut = sinf(theta) / k;
    *zOut = kTowardViewer * static_cast<float>(sign) * (1.0f - cosf(theta)) / k;
}

// Build the strip. Returns false if anything failed, which the caller turns
// into "forward the game's draw", not into a missing screen.
//
// A STRIP, not a mesh: the bend is constant along y, so two rows are all the
// geometry there is to have. N columns is 2(N+1) vertices and 6N indices --
// 130 and 384 at the default.
bool build(ID3D11DeviceContext* ctx) {
    const int n = g_segments;
    const uint32_t verts = static_cast<uint32_t>(2 * (n + 1));
    const uint32_t idxs = static_cast<uint32_t>(6 * n);

    Vertex vb[2 * (kMaxSegments + 1)];
    unsigned short ib[6 * kMaxSegments];

    for (int i = 0; i <= n; ++i) {
        // The ORIGINAL x drives the UV, and the bent one only the position:
        // the bend moves where a column is, never which texel it shows.
        const float x = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(n);
        float bx = 0.0f, bz = 0.0f;
        bend(x, g_curvature, g_sign, &bx, &bz);
        const float u = (x + 1.0f) * 0.5f;

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

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;

    if (g_vb) { g_vb->Release(); g_vb = nullptr; }
    if (g_ib) { g_ib->Release(); g_ib = nullptr; }

    D3D11_BUFFER_DESC bd{};
    D3D11_SUBRESOURCE_DATA sd{};

    bd.ByteWidth = verts * sizeof(Vertex);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    sd.pSysMem = vb;
    HRESULT hr = dev->CreateBuffer(&bd, &sd, &g_vb);

    if (SUCCEEDED(hr)) {
        bd.ByteWidth = idxs * sizeof(unsigned short);
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        sd.pSysMem = ib;
        hr = dev->CreateBuffer(&bd, &sd, &g_ib);
    }
    dev->Release();

    if (FAILED(hr) || !g_vb || !g_ib) {
        if (g_vb) { g_vb->Release(); g_vb = nullptr; }
        if (g_ib) { g_ib->Release(); g_ib = nullptr; }
        return false;
    }

    g_indexCount = idxs;
    g_builtCurvature = g_curvature;
    g_builtSegments = g_segments;
    g_builtSign = g_sign;
    Log::get().note(
        "panel curvature: built a %d-column strip -- %u vertices, %u indices -- at "
        "curvature %.3f, depth sign %+d. At curvature 0 and 1 column this is the "
        "game's own quad to the byte, which is what makes a difference on screen "
        "there a fault in the substitution rather than in the geometry.",
        g_segments, verts, idxs, g_curvature, g_sign);
    return true;
}

}  // namespace

void panelCurveConfigure(Config& cfg) {
    const float wasCurve = g_curvature;
    const int wasSeg = g_segments;
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
    g_curvature = c;
    g_segments = cfg.getIntInRange("advanced.panel_curvature_segments",
                                   kDefaultSegments, kMinSegments, kMaxSegments);
    // Which way the bend goes. 1 is toward the viewer and is correct on the
    // build this was measured against; -1 is the escape hatch if a game
    // update ever flips the handedness of the panel's transform. See
    // kTowardViewer for what was measured and how.
    g_sign = cfg.getIntInRange("advanced.panel_curvature_sign", 1, -1, 1) < 0 ? -1 : 1;

    // A strip of N columns has N-1 INTERIOR vertex columns, and the whole
    // bend lives in those: the two edges receive the SAME z whatever the
    // curvature, because cos is even. So at one column there is no curve at
    // all -- the quad stays flat and merely moves in depth and narrows in x,
    // which looks exactly like the screen receding and reads as the bend
    // going the wrong way. That cost the first flight, and the log said
    // nothing because nothing was wrong.
    if (g_curvature > 0.0f && g_segments < 8) {
        Log::get().note(
            "panel curvature: %d column%s is too few to bend anything. The curve "
            "lives in the INTERIOR vertex columns and %d columns has %d of them; "
            "the two edges always get the same depth, because cos is even. So the "
            "screen will stay FLAT and merely move away and narrow, which looks "
            "like it receding rather than curving. Raise "
            "advanced.panel_curvature_segments to 64 to see the bend.",
            g_segments, g_segments == 1 ? "" : "s", g_segments, g_segments - 1);
    }

    if (g_curvature != wasCurve || g_segments != wasSeg || g_sign != wasSign) {
        if (g_curvature > 0.0f) {
            Log::get().note(
                "panel curvature: %.3f of a circle over %d columns, depth sign %+d. "
                "The screen bends toward you and keeps its width, so the edges come "
                "nearer rather than the middle going further.",
                g_curvature, g_segments, g_sign);
        } else if (g_segments != kDefaultSegments) {
            Log::get().note(
                "panel curvature: off (0), but the segment count is %d rather than "
                "the default, so the FLAT strip is substituted anyway. That is the "
                "identity test: the screen must look exactly as it does without "
                "EDVR. Set the segment count back to %d to stop substituting.",
                g_segments, kDefaultSegments);
        } else {
            Log::get().note("panel curvature: off; the game's own quad is drawn.");
        }
    }
}

bool panelCurveWants() {
    if (g_stoodDown) return false;
    // Curvature 0 at the default segment count is the shipped state and does
    // nothing at all. A non-default segment count at curvature 0 is the
    // deliberate identity test, which has to substitute in order to prove
    // anything -- so it counts as wanting.
    return g_curvature > 0.0f || g_segments != kDefaultSegments;
}

bool panelCurveSubstitute(ID3D11DeviceContext* ctx, PanelCurveDrawFn draw) {
    if (!ctx || !draw || g_stoodDown) return false;

    bool substituted = false;
    const bool ok = guardedBudget(g_budget, [&] {
        if (!g_vb || !g_ib || g_builtCurvature != g_curvature ||
            g_builtSegments != g_segments || g_builtSign != g_sign) {
            if (!build(ctx)) return;
        }

        // Save exactly what is about to be changed and nothing else. None of
        // these slots goes through an EDVR hook, so there is no shadow to
        // consult and none to confuse: the context is the only authority on
        // them, and IAGet* is how it is asked.
        ctx->IAGetVertexBuffers(0, 1, &g_savedVb, &g_savedStride, &g_savedOffset);
        ctx->IAGetIndexBuffer(&g_savedIb, &g_savedFmt, &g_savedIbOffset);
        ctx->IAGetPrimitiveTopology(&g_savedTopo);
        g_saveHeld = true;

        ID3D11Buffer* ours = g_vb;
        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        ctx->IASetVertexBuffers(0, 1, &ours, &stride, &offset);
        ctx->IASetIndexBuffer(g_ib, DXGI_FORMAT_R16_UINT, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Through the ORIGINAL function pointer. The context's vtable entry is
        // our own thunk, and calling it here would recognise this composite
        // again and substitute again, without end.
        draw(ctx, g_indexCount, 1, 0, 0, 0);

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
                g_segments, g_curvature);
        }
    });

    if (!ok) {
        // Stand down permanently on the FIRST fault, rather than spending a
        // budget of five. The other users of guardedBudget in this tree are
        // observers, where retrying costs a log line; this one has the
        // player's view riding on it, and a substitution that faulted once has
        // no business being attempted again mid-flight.
        g_stoodDown = true;
        // And put the game's state back, which is the whole reason the saved
        // state is not a set of locals. Under its own guard: if the context is
        // far enough gone that restoring faults too, there is nothing further
        // to be done and the process should survive to say so.
        guarded("panelCurve.restore", [&] { restoreSaved(ctx); });
        Log::get().note(
            "panel curvature: the substitution faulted, so it is off for the rest "
            "of this session and the game's own quad is drawn again. The input "
            "assembler was put back, so the screen should look exactly as it did "
            "before -- if it does not, restart the game and report the log.");
        return false;
    }
    return substituted;
}

void panelCurveShutdown() {
    // Any held references are dropped WITHOUT touching the context: shutdown
    // runs when the device may already be going away, and the bindings are
    // about to stop mattering. Releasing is still owed.
    g_saveHeld = false;
    if (g_savedVb) { g_savedVb->Release(); g_savedVb = nullptr; }
    if (g_savedIb) { g_savedIb->Release(); g_savedIb = nullptr; }
    if (g_vb) { g_vb->Release(); g_vb = nullptr; }
    if (g_ib) { g_ib->Release(); g_ib = nullptr; }
    g_indexCount = 0;
    g_builtCurvature = -1.0f;
    g_builtSegments = -1;
    g_builtSign = 0;
}

}  // namespace edvr
