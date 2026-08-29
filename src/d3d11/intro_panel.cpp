#include "intro_panel.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstring>

#include <string>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/frame_flag.h"   // headForward / eyeTangents, from the vr half
#include "binding_shadow.h"

namespace edvr {
namespace {

// cb2 is five float4s. Both the shader's declaration (CB2[5]) and the
// buffers the census found (80 bytes each) say so.
constexpr uint32_t kCbFloats = 20;
constexpr uint32_t kCbBytes = kCbFloats * 4;

// The constant buffer slot the transform lives in, from the disassembly.
constexpr uint32_t kVsSlot = 2;

// One buffer per eye is what the game uses; a third would mean the model is
// wrong and is worth refusing rather than guessing at.
constexpr uint32_t kMaxSlots = 2;

// Frames to let the GPU copy execute before mapping it. quad_probe's number,
// for the same reason: long enough that the map never stalls the render
// thread, short enough to retire within a blink.
constexpr uint32_t kSettleFrames = 4;

// The splash's own half-width in NDC, measured 2026-08-28 from its constants
// and its vertices in ONE frame: 1.0440 across, 0.5934 tall, both eyes
// agreeing to four decimals. It spans the whole view and overspills about
// four percent, so a movie matched to it is clipped at the edges exactly as
// the splash is.
//
// The WIDTH only; the height follows, because both quads are 16:9 and the
// movie's pixel-to-NDC scale is near-isotropic (1/2712 by 1/2678). Deriving
// the scale from either axis gave 5.53 and 5.52 -- that agreement is the
// cross-check that says the decode is right, not a coincidence.
//
// Kept as a FRACTION OF THE VIEW rather than as the scale itself. The movie's
// panel is a fixed 512x288 PIXELS, so how much of the view it covers depends
// on the eye's resolution, and the multiple that matches the splash is a
// different number on a different headset; deriving it at runtime from the
// panel's own constants survives that. What does not survive is the premise
// -- that the splash fills the view -- which was measured on one rig with one
// field of view, and is the thing to re-measure if this looks wrong on
// somebody else's.
constexpr float kSplashHalfWidthNdc = 1.0440f;

// 5.5 is what matching the splash takes here; 8 leaves room for a headset
// where the same match needs more.
constexpr float kMaxSize = 8.0f;

// THE SPLASH'S OWN SCREEN, measured 2026-08-28 from its constants and its
// vertices in one frame (docs/intro-video.md):
//
//   half-extents 4.44444 x 2.5 world units -- 16:9, and the same +-1 unit
//   quad the movie uses, so only the transform differs;
//   corner w of 3.3259 3.6643 3.3196 3.6580 -- about 3.35 units away and
//   very nearly fronto-parallel;
//   106.4 degrees wide by 73.4 tall, centred on the game's forward.
//
// Placing the movie on exactly these numbers is the point: the field asked
// for it to play "as if anchored on the virtual screen the splash screen
// appears on", and the game has already told us where that screen is.
constexpr float kScreenHalfW = 4.44444f;
constexpr float kScreenHalfH = 2.5f;
constexpr float kScreenDistDefault = 3.35f;

// Half the interpupillary distance, metres. The panel's stereo comes from
// offsetting each eye's ray origin by this; the runtime publishes no IPD,
// and at 3.35 m a few millimetres of error is far under a tenth of a
// degree. Named rather than buried so it can be corrected if it ever
// matters.
constexpr float kHalfIpd = 0.0315f;

// World lock (fix.intro_video_lock). The counter-move is witchstar_fix's,
// which already holds a head-locked sprite on a world direction by shifting
// the VIEWPORT for one draw -- no matrix, no new channel, and field-proven on
// a different draw. It counter-moves against the game's world forward, and
// that is exactly the anchor wanted here -- the field's own words, "the
// game's forward is where the splash screen anchors". Sharing it is what
// makes the cut work: if forward is wrong the movie and the splash are wrong
// TOGETHER and one recentre fixes both, where a movie anchored to the
// player's own view would need the cut to jump.
//
// On the measured rig forward IS 180 degrees out, but that is the runtime's
// doing and not the game's: the pose log shows valid, tracking-OK poses
// reporting yaw 180 and y -23 m from the first frame (docs/intro-video.md).
// A play space is the place to fix that, not a panel transform.
bool  g_worldLock = false;
bool  g_anchored = false;   // the "holding" line has been said
bool  g_lockRefusedNoted = false;

float g_screenDist = kScreenDistDefault;

// Set when fix.intro_video_size names the splash rather than a number.
bool  g_matchSplash = false;
float g_engagedScale = 1.0f;

FaultBudget g_budget("introPanel", 4);

float    g_size = 1.0f;
bool     g_retired = false;      // the intro is over; stood down for good
bool     g_refused = false;      // the constants did not read as screen-space
uint32_t g_frame = 0;

// The frame's movie marker: the fill's target size, and the frame it was
// seen in. A composite is the movie's only if it samples that surface in
// that frame.
uint32_t g_fillW = 0, g_fillH = 0;
uint32_t g_fillFrame = 0;

struct Slot {
    void*         key = nullptr;   // the game's cb2 buffer for one eye
    ID3D11Buffer* stage = nullptr; // the readback copy, transient
    uint32_t      dueFrame = 0;    // 0 = nothing settling
    ID3D11Buffer* ours = nullptr;  // our replacement constants (dynamic)
    bool          ready = false;
    // Which eye this buffer belongs to, read from the GAME's own constants:
    // cb2[4].x is the centre of an asymmetric frustum and is positive in
    // the eye whose outer tangent is on the left. Measured +0.1939 and
    // -0.1940 on this headset, against a frustum centre of 0.193973
    // computed from its published tangents -- so this is a reading, not a
    // convention anybody chose.
    bool          leftEye = false;
};
Slot     g_slot[kMaxSlots];
uint32_t g_slotCount = 0;

void*    g_restore = nullptr;    // the game's buffer, for endDraw
uint32_t g_applied = 0;

// Is this buffer the screen-space placement, and not a world-space one?
//
// The discriminator is the perspective divide. A world-placed panel -- the
// splash, the menu, anything that goes through a view-projection -- has a
// varying w, which means non-zero w terms in cb2[1..3] and a cb2[4].w that
// is not 1. The movie's has neither: cb2[3] is all zeros and w is a constant
// 1. Measured on both, 2026-08-28.
//
// This is why the fix cannot wander into the splash even if the draw match
// were wrong: the splash's own numbers refuse it.
bool looksScreenSpace(const float* f) {
    auto zero = [](float v) { return v > -1e-9f && v < 1e-9f; };
    if (!zero(f[7]) || !zero(f[11])) return false;       // cb2[1].w, cb2[2].w
    for (uint32_t i = 12; i < 16; ++i) {                 // cb2[3] entirely
        if (!zero(f[i])) return false;
    }
    if (f[19] < 0.999f || f[19] > 1.001f) return false;  // cb2[4].w == 1
    // The scale must be a plausible half-size in pixels. A world-space quad
    // measured 4.4 by 2.5 units; a screen-space one measured 512 by 288.
    if (f[0] < 16.0f || f[1] < 16.0f) return false;
    return true;
}

// Build cb2 for a world-space panel on the splash's screen.
//
// The vertex shader is  o1 = x*cb2[1] + y*cb2[2] + z*cb2[3] + cb2[4], with
// (x,y) the unit quad already multiplied by cb2[0]. Set cb2[0] to 1 and the
// four remaining float4s ARE the columns of a 4x4 acting on (x, y, z, 1) --
// which is what the splash's own constants are, measured.
//
// leftEye picks the frustum and the eye offset. It is read from the GAME's
// own constants: cb2[4].x is the centre of an asymmetric frustum, positive
// in the eye whose outer tangent is on the left. No guessing which submit
// this is.
//
// Returns false when the pose or the tangents are not published -- without
// openvr_api.dll installed there is no world to lock to, and stock is the
// honest answer.
bool buildWorldCb(bool leftEye, float dist, float vpW, float vpH,
                  float* out) {
    float pose[12];
    if (!headPose(pose)) return false;
    float outer = 0.0f, inner = 0.0f;
    if (!eyeTangents(&outer, &inner)) return false;
    const float span = outer + inner;
    if (span < 1e-3f) return false;

    // The eye's frustum. The OUTER tangent is temporal: left edge of the
    // left eye, right edge of the right. The vertical span is not published
    // and follows from the eye texture's shape -- the same derivation
    // fss_theater makes, and it reproduces this headset's published
    // +-1.2648 to four decimals from 5424x5356.
    const float lt = leftEye ? -outer : -inner;
    const float rt = leftEye ? inner : outer;
    const float vt = span * 0.5f * (vpW > 0.0f ? vpH / vpW : 1.0f);
    const float tp = -vt, bt = vt;
    const float m00 = 2.0f / (rt - lt), m02 = (rt + lt) / (rt - lt);
    const float m11 = 2.0f / (bt - tp), m12 = (bt + tp) / (bt - tp);

    // View: the seated frame through the head's inverse, plus this eye's
    // lateral offset. R is the pose's rotation; A = R-transpose.
    const float* R = pose;   // row-major 3x4: R[r*4+c]
    auto At = [&](int i, int j) { return R[j * 4 + i]; };   // A[i][j] = R[j][i]
    const float tx = R[3], ty = R[7], tz = R[11];
    const float ex = leftEye ? -kHalfIpd : kHalfIpd;

    // The three basis vectors of the panel, in view space.
    //
    // The panel's +x points to the viewer's LEFT, which is the game's own
    // convention on both of its panels and not a choice: the movie's
    // cb2[1].x is -1/2712 and the splash's is -0.780645 against an m00 of
    // 0.78073, so each maps a unit of quad x to -1 units of view x. Built
    // the other way the picture comes out MIRRORED -- which is exactly what
    // the first flight showed, with everything else right.
    float cx[3], cy[3], c0[3];
    for (int i = 0; i < 3; ++i) {
        cx[i] = At(i, 0) * -kScreenHalfW;
        cy[i] = At(i, 1) * kScreenHalfH;
        // the panel's centre at (0,0,-dist) in the seated frame, brought
        // into view space, with the head's translation and the eye offset
        const float o = -(At(i, 0) * tx + At(i, 1) * ty + At(i, 2) * tz);
        c0[i] = At(i, 2) * (-dist) + o;
    }
    c0[0] -= ex;

    // No depth buffer is bound for this draw (census "d=-"), so z is free;
    // half of w keeps ndc.z at 0.5, safely inside [0,1] whatever the panel
    // does.
    auto col = [&](const float* v, bool centre, float* dst) {
        dst[0] = m00 * v[0] + m02 * v[2];
        dst[1] = m11 * v[1] + m12 * v[2];
        dst[3] = -v[2];
        dst[2] = 0.5f * dst[3];
        (void)centre;
    };
    for (int i = 0; i < 20; ++i) out[i] = 0.0f;
    out[0] = 1.0f;   // cb2[0]: the unit quad is already the panel's shape
    out[1] = 1.0f;
    col(cx, false, out + 4);
    col(cy, false, out + 8);
    // cb2[3] stays zero: the quad's z is zero on every vertex (measured).
    col(c0, true, out + 16);
    return true;
}

Slot* findSlot(void* key) {
    for (uint32_t i = 0; i < g_slotCount; ++i) {
        if (g_slot[i].key == key) return &g_slot[i];
    }
    return nullptr;
}

void releaseSlot(Slot& s) {
    if (s.stage) { s.stage->Release(); s.stage = nullptr; }
    if (s.ours) { s.ours->Release(); s.ours = nullptr; }
    s = Slot();
}

}  // namespace

void introPanelConfigure(Config& cfg) {
    const float wasSize = g_size;
    const bool  wasMatch = g_matchSplash;
    const std::string v = cfg.getString("fix.intro_video_size", "stock");
    g_matchSplash = (v == "splash");
    if (g_matchSplash) {
        g_size = 0.0f;   // derived at readback, from the panel's own numbers
    } else {
        g_size = static_cast<float>(atof(v.c_str()));
        if (g_size < 1.0f) g_size = 1.0f;   // "stock" and anything unparsed
        if (g_size > kMaxSize) g_size = kMaxSize;
    }
    const std::string lk = cfg.getString("fix.intro_video_lock", "head");
    const bool wasLock = g_worldLock;
    g_worldLock = (lk == "world");
    g_screenDist = cfg.getFloat("advanced.intro_video_distance",
                                kScreenDistDefault);
    if (g_screenDist < 1.0f) g_screenDist = 1.0f;
    if (g_screenDist > 20.0f) g_screenDist = 20.0f;
    if (g_worldLock != wasLock) {
        Log::get().note(
            g_worldLock
                ? "intro video lock: WORLD. The movie's panel is held on the "
                  "game's own forward -- the direction the splash after it "
                  "anchors to -- instead of riding your head, by counter-"
                  "moving the viewport per draw from the pose the vr half "
                  "publishes. Sharing the splash's anchor is the point: one "
                  "recentre then fixes both. Needs openvr_api.dll installed."
                : "intro video lock: head. The movie's panel rides your head, "
                  "which is the game's own behaviour.");
    }
    if (g_size == wasSize && g_matchSplash == wasMatch && g_worldLock == wasLock) {
        return;
    }
    if (g_matchSplash) {
        Log::get().note(
            "intro video size: SPLASH. The launch movie's panel is grown until "
            "it covers what the splash after it covers -- derived from the "
            "game's own numbers on this rig rather than a fixed multiple, so "
            "the cut from movie to splash keeps its size. It stays "
            "head-locked, which the splash is not, so the two still will not "
            "agree while you are looking away (docs\\intro-video.md).");
        return;
    }
    if (g_size == 1.0f) {
        Log::get().note("intro video size: stock. The launch movie's panel is "
                        "left at the game's own size.");
        return;
    }
    Log::get().note(
        "intro video size: x%.2f. The launch movie's panel is drawn that much "
        "larger, about the point it already sits on -- straight ahead, both "
        "eyes, aspect kept. It stays head-locked: this changes its SIZE, not "
        "where it lives (docs\\intro-video.md). Stock is 1.0.",
        static_cast<double>(g_size));
}

bool introPanelWants() {
    return (g_matchSplash || g_size != 1.0f || g_worldLock) && !g_retired &&
           !g_refused;
}

void introPanelNoteFill(uint32_t targetW, uint32_t targetH) {
    g_fillW = targetW;
    g_fillH = targetH;
    g_fillFrame = g_frame;
}

bool introPanelOnComposite(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                           uint32_t instances, uint32_t srvW, uint32_t srvH) {
    if (!introPanelWants() || !ctx) return false;
    // The composite's shape, from the census: a six-index instanced quad.
    if (kind != 'X' || count != 6 || instances != 1) return false;
    // ...sampling the surface the movie was converted into, THIS frame. The
    // fill draws before the composite in the same frame (census q ordering),
    // so a marker from this frame is the movie playing and nothing else.
    if (g_fillFrame != g_frame || !g_fillW) return false;
    if (srvW != g_fillW || srvH != g_fillH) return false;

    // The world lock is not a nudge to the game's geometry -- it is a
    // REPLACEMENT transform, built here and written into cb2 exactly as the
    // splash's own is. A viewport shift (the first attempt, witchstar_fix's
    // pattern) can only translate: it gives no stereo, so the picture always
    // reads as being at infinity, and the splash's screen is not at
    // infinity. That is why it could never have worked, whatever its sign.
    //
    // What is built: clip = Proj * View * Model, for a 16:9 quad of the
    // splash's own half-extents at the splash's own distance, centred on the
    // game's forward -- which in the seated frame is -Z, so the panel sits at
    // (0,0,-d) and needs no anchor of its own. Ordinary world geometry, so
    // the compositor reprojects it like anything else in the scene.
    bool bound = false;
    guardedBudget(g_budget, [&] {
        ID3D11Buffer* cb = nullptr;
        ctx->VSGetConstantBuffers(kVsSlot, 1, &cb);
        if (!cb) return;
        Slot* s = findSlot(cb);
        if (!s && g_slotCount < kMaxSlots) {
            s = &g_slot[g_slotCount++];
            s->key = cb;
        }
        if (!s) { cb->Release(); return; }

        if (!g_matchSplash && g_size == 1.0f) {
            // Lock only: nothing to substitute, but the caller still has to
            // call endDraw so the viewport goes back.
            cb->Release();
            return;
        }
        if (s->ready && s->ours) {
            // The world panel is rebuilt EVERY draw -- the view moves with
            // the head, which is the entire point -- so the buffer is
            // dynamic and written here rather than baked once.
            if (g_worldLock) {
                UINT nvp = 1;
                D3D11_VIEWPORT vp{};
                ctx->RSGetViewports(&nvp, &vp);
                float world[kCbFloats];
                if (nvp == 0 || vp.Width <= 0.0f ||
                    !buildWorldCb(s->leftEye, g_screenDist, vp.Width, vp.Height,
                                  world)) {
                    // No pose, no tangents, no viewport: stock rather than a
                    // panel placed on guesses.
                    if (!g_lockRefusedNoted) {
                        g_lockRefusedNoted = true;
                        Log::get().note(
                            "intro video lock: no head pose or eye tangents "
                            "published -- openvr_api.dll is what publishes "
                            "them. The movie stays as the game drew it.");
                    }
                    cb->Release();
                    return;
                }
                D3D11_MAPPED_SUBRESOURCE m{};
                if (FAILED(ctx->Map(s->ours, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) ||
                    !m.pData) {
                    cb->Release();
                    return;
                }
                memcpy(m.pData, world, kCbBytes);
                ctx->Unmap(s->ours, 0);
            }
            g_restore = cb;
            ID3D11Buffer* ours = s->ours;
            ctx->VSSetConstantBuffers(kVsSlot, 1, &ours);
            bound = true;
            if (++g_applied == 1) {
                Log::get().note(
                    "intro video size: engaged -- the movie's panel is drawn "
                    "x%.2f its own size. Said once.",
                    static_cast<double>(g_engagedScale));
            }
            cb->Release();
            return;
        }
        if (s->stage || s->dueFrame) { cb->Release(); return; }

        // Copy the constants off the GPU. They are never written while the
        // movie plays -- measured -- so a single copy is the whole story.
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) { cb->Release(); return; }
        ResourceInfo info;
        const bool ok = bindingResolveResource(cb, &info) && info.isBuffer &&
                        info.a >= kCbBytes;
        if (ok) {
            D3D11_BUFFER_DESC sd{};
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.ByteWidth = kCbBytes;
            if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &s->stage)) && s->stage) {
                D3D11_BOX box{};
                box.right = kCbBytes;
                box.bottom = 1;
                box.back = 1;
                ctx->CopySubresourceRegion(s->stage, 0, 0, 0, 0,
                                           static_cast<ID3D11Resource*>(cb), 0,
                                           &box);
                s->dueFrame = g_frame + kSettleFrames;
            }
        }
        dev->Release();
        cb->Release();
    });
    // Either change is a match. Returning false with a shifted viewport
    // would leak it into every draw after this one -- the caller only calls
    // endDraw when this says yes.
    return bound;
}

void introPanelEndDraw(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    if (g_restore) {
        ID3D11Buffer* orig = static_cast<ID3D11Buffer*>(g_restore);
        ctx->VSSetConstantBuffers(kVsSlot, 1, &orig);
        g_restore = nullptr;
    }
}

void introPanelTick(ID3D11DeviceContext* ctx, bool sceneFrame) {
    ++g_frame;
    if (sceneFrame && !g_retired && (g_slotCount || g_applied)) {
        g_retired = true;
        Log::get().note(
            "intro video size: a rendered scene arrived -- the intro is over "
            "and this stands down for the session. It resized %u draw(s).",
            g_applied);
        introPanelShutdown();
        return;
    }
    if (!ctx) return;
    for (uint32_t i = 0; i < g_slotCount; ++i) {
        Slot& s = g_slot[i];
        if (!s.stage || !s.dueFrame || g_frame < s.dueFrame) continue;
        s.dueFrame = 0;
        guardedBudget(g_budget, [&] {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(ctx->Map(s.stage, 0, D3D11_MAP_READ, 0, &m)) || !m.pData) {
                Log::get().note("intro video size: the constants could not be "
                                "read back; stock for this session.");
                g_refused = true;
                return;
            }
            float f[kCbFloats];
            memcpy(f, m.pData, sizeof(f));
            ctx->Unmap(s.stage, 0);

            if (!looksScreenSpace(f)) {
                // Not the movie's placement. Refusing the SESSION rather than
                // the slot: a world-space buffer here means the draw match is
                // reaching something it should not, and resizing that is how
                // a fix damages a screen nobody complained about.
                g_refused = true;
                Log::get().note(
                    "intro video size: the panel's constants do not read as a "
                    "screen-space placement (cb2[3] %.4g %.4g %.4g %.4g, "
                    "cb2[4].w %.4g) -- stock for this session, which is the "
                    "safe answer. docs\\intro-video.md says what the two "
                    "shapes look like.",
                    static_cast<double>(f[12]), static_cast<double>(f[13]),
                    static_cast<double>(f[14]), static_cast<double>(f[15]),
                    static_cast<double>(f[19]));
                return;
            }

            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return;
            // Named as a number, the scale IS the number. Named as the
            // splash, it is derived here -- the first point at which the
            // panel's own half-width in NDC is known: f[0] is its half-size
            // in pixels and f[4] the pixels-to-NDC term, so their product is
            // the fraction of the view one half of it covers on this rig.
            float scale = g_size;
            if (g_matchSplash) {
                const float halfNdc = f[0] * fabsf(f[4]);
                scale = halfNdc > 1e-6f ? kSplashHalfWidthNdc / halfNdc : 1.0f;
                if (scale < 1.0f) scale = 1.0f;
                if (scale > kMaxSize) scale = kMaxSize;
                Log::get().note(
                    "intro video size: this panel covers %.4f of the view's "
                    "half-width and the splash covers %.4f, so matching it is "
                    "x%.2f here.",
                    static_cast<double>(halfNdc),
                    static_cast<double>(kSplashHalfWidthNdc),
                    static_cast<double>(scale));
            }
            g_engagedScale = scale;
            float out[kCbFloats];
            memcpy(out, f, sizeof(out));
            out[0] = f[0] * scale;
            out[1] = f[1] * scale;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kCbBytes;
            // DYNAMIC, not immutable: the world panel is rebuilt every draw
            // because the view moves with the head. The size-only mode
            // writes these bytes once and never again, and pays nothing for
            // the difference.
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            D3D11_SUBRESOURCE_DATA sr{};
            sr.pSysMem = out;
            if (SUCCEEDED(dev->CreateBuffer(&bd, &sr, &s.ours)) && s.ours) {
                s.leftEye = f[16] > 0.0f;
                s.ready = true;
                Log::get().note(
                    "intro video size: read the movie panel's own constants -- "
                    "half-size %.0f x %.0f pixels, centred at %.4f in NDC. "
                    "Drawing it at %.0f x %.0f instead.",
                    static_cast<double>(f[0]), static_cast<double>(f[1]),
                    static_cast<double>(f[16]), static_cast<double>(out[0]),
                    static_cast<double>(out[1]));
            }
            dev->Release();
        });
        if (s.stage) { s.stage->Release(); s.stage = nullptr; }
    }
}

void introPanelShutdown() {
    for (Slot& s : g_slot) releaseSlot(s);
    g_slotCount = 0;
    g_restore = nullptr;
}

}  // namespace edvr
