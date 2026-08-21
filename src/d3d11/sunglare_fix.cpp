#include "sunglare_fix.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstring>

#include "../common/config.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "billboard_fix.h"
#include "binding_shadow.h"
#include "exposure_fix.h"  // exposureDampingActive

namespace edvr {
namespace {

// The glare train, as the sun census resolved it: DrawInstanced, 6
// vertices per instance, more than one instance, and BOTH of PS slots 0
// and 1 the 2048x1024 fmt-98 art sheets the elements are stamped from.
// The size-and-format pair is the identity; nothing else in the frame
// samples those sheets. Counts, positions and buffer pointers all proved
// unstable over this hunt -- what a pass READS is what it is.
constexpr char     kKind = 'N';
constexpr uint32_t kVerts = 6;
constexpr uint32_t kSheetW = 2048;
constexpr uint32_t kSheetH = 1024;
constexpr uint32_t kSheetFmt = 98;

enum class Mode { kStock, kOff, kFirst };

Mode     g_mode = Mode::kStock;
uint32_t g_keep = 0;
bool     g_steady = false;
int      g_steadyMode = 0;   // 0 off, 1 counter-rotate, 2 fixed-30 test
uint64_t g_lastSeenMs = 0;   // when the train last drew
float    g_theta = 0;        // low-passed counter-rotation angle
bool     g_thetaValid = false;
uint64_t g_skipped = 0;
uint64_t g_clamped = 0;

// The steady actuator. The camera-block rows are the VIEW MATRIX --
// position flows through them, so both CB replacement formulas displaced
// the elements per eye, and the CB is now measurement only. What spins
// the stamp about its own centre without touching its position is the
// CORNER STREAM: six 8-byte two-float corners, expanded around the
// element's centre by the shader. Rotate the corners, the art
// counter-rotates, and nothing else in the pipeline can tell.
// The corner verts are FLOAT16x4 -- eight bytes is four halfs, (x, y,
// u, v) -- which the first engagement discovered the hard way: rotating
// the bytes as float32 pairs scrambled half bit-patterns into screen-
// spanning streaks. The capture's "0.00781" was 0x3C000000: two halfs
// (0.0, 1.0) wearing a float's clothes.
constexpr uint32_t kCornerBytes = 48;
constexpr uint32_t kCornerHalfs = kCornerBytes / 2;
constexpr uint32_t kCornerVerts = 6;
constexpr uint64_t kReadbackLagMs = 50;

uint16_t       g_corners[kCornerHalfs];

float halfToFloat(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t man = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        bits = sign;             // denormals flush to signed zero; corner
                                 // geometry never lives there
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp + 112u) << 23) | (man << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

uint16_t floatToHalf(float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
    const int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 112;
    const uint32_t man = bits & 0x7FFFFFu;
    if (exp <= 0) return sign;                       // flush tiny to zero
    if (exp >= 31) return sign | 0x7BFFu;            // clamp to max half
    return static_cast<uint16_t>(sign | (exp << 10) | (man >> 13));
}
bool           g_haveCorners = false;
ID3D11Buffer*  g_cornerStaging = nullptr;   // owned
bool           g_cornerPending = false;
uint64_t       g_cornerCopyMs = 0;
ID3D11Buffer*  g_ourVb = nullptr;           // owned; the rotated corners
ID3D11Buffer*  g_savedVb = nullptr;         // the game's, held across one draw
UINT           g_savedStride = 0;
UINT           g_savedOffset = 0;
bool           g_engaged = false;
uint64_t       g_applied = 0;
uint64_t       g_appliedAtNote = 0;
uint64_t       g_lastNoteMs = 0;

void releaseSteadyObjects() {
    if (g_cornerStaging) {
        g_cornerStaging->Release();
        g_cornerStaging = nullptr;
    }
    if (g_ourVb) {
        g_ourVb->Release();
        g_ourVb = nullptr;
    }
    g_haveCorners = false;
    g_cornerPending = false;
    g_thetaValid = false;
}

// One-time capture of the game's corner buffer: it is created with initial
// data and never mapped, so no tee can see it -- a staging copy at the
// draw, read back a few frames later, is the only window. Runs while the
// corners are still unknown; the fix stands aside until they are.
void captureCorners(ID3D11DeviceContext* ctx) {
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    if (!vb) return;
    ResourceInfo info;
    if (!bindingResolveResource(vb, &info) || !info.isBuffer ||
        info.a != kCornerBytes) {
        vb->Release();
        return;   // not the 48-byte dedicated corner buffer; stand aside
    }
    const uint64_t now = nowMs();
    if (g_cornerPending) {
        if (now - g_cornerCopyMs >= kReadbackLagMs && g_cornerStaging) {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(g_cornerStaging, 0, D3D11_MAP_READ, 0,
                                   &m))) {
                memcpy(g_corners, m.pData, kCornerBytes);
                ctx->Unmap(g_cornerStaging, 0);
                g_haveCorners = true;
                g_cornerPending = false;
                Log::get().note("sun glare steady: corner stream captured, "
                                "FLOAT16x4 decode: v0 uv(%.3g %.3g) "
                                "corner(%.3g %.3g), v1 uv(%.3g %.3g), v2 "
                                "uv(%.3g %.3g). The counter-rotation "
                                "engages from the next matched draw.",
                                halfToFloat(g_corners[0]),
                                halfToFloat(g_corners[1]),
                                halfToFloat(g_corners[2]),
                                halfToFloat(g_corners[3]),
                                halfToFloat(g_corners[4]),
                                halfToFloat(g_corners[5]),
                                halfToFloat(g_corners[8]),
                                halfToFloat(g_corners[9]));
            }
        }
        vb->Release();
        return;
    }
    if (!g_cornerStaging) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kCornerBytes;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            dev->CreateBuffer(&bd, nullptr, &g_cornerStaging);
            dev->Release();
        }
        if (!g_cornerStaging) {
            vb->Release();
            return;
        }
    }
    ctx->CopyResource(g_cornerStaging, vb);
    g_cornerCopyMs = now;
    g_cornerPending = true;
    vb->Release();
}

}  // namespace

bool sunglareIsGlareTrain(char kind, uint32_t count, uint32_t instances) {
    if (kind != kKind || count != kVerts || instances < 2) return false;
    ResourceInfo s0, s1;
    if (!bindingResolve(bindingGet(BindSlot::PsSrv0), &s0) ||
        !s0.isTexture2D || s0.a != kSheetW || s0.b != kSheetH ||
        s0.fmt != kSheetFmt) {
        return false;
    }
    if (!bindingResolve(bindingGet(BindSlot::PsSrv1), &s1) ||
        !s1.isTexture2D || s1.a != kSheetW || s1.b != kSheetH ||
        s1.fmt != kSheetFmt) {
        return false;
    }
    return true;
}

void sunglareConfigure(Config& cfg) {
    const Mode was = g_mode;
    const uint32_t wasKeep = g_keep;
    const std::string v = cfg.getString("fix.sun_glare", "stock");
    if (v == "stock") {
        g_mode = Mode::kStock;
    } else if (v == "off") {
        g_mode = Mode::kOff;
    } else if (v.compare(0, 6, "first:") == 0) {
        char* end = nullptr;
        const unsigned long k = strtoul(v.c_str() + 6, &end, 10);
        if (end == v.c_str() + 6 || *end || k < 1 || k > 32) {
            Log::get().note("sun glare: \"%s\" is not stock, off or first:K "
                            "with K 1..32; staying stock.", v.c_str());
            g_mode = Mode::kStock;
        } else {
            g_mode = Mode::kFirst;
            g_keep = static_cast<uint32_t>(k);
        }
    } else {
        Log::get().note("sun glare: \"%s\" is not stock, off or first:K; "
                        "staying stock.", v.c_str());
        g_mode = Mode::kStock;
    }
    const int wasSteady = g_steadyMode;
    g_steadyMode = cfg.getIntInRange("fix.sun_glare_steady", 0, 0, 2);
    g_steady = g_steadyMode != 0;
    billboardGlareWatch(g_steady);
    if (g_steadyMode != wasSteady) {
        if (g_steadyMode == 2) {
            Log::get().note("sun glare steady: FIXED-ANGLE DIAGNOSTIC -- "
                            "the corner directions are rotated a constant "
                            "30 degrees. Elements visibly tilted proves the "
                            "shader consumes the rotation; elements "
                            "unchanged proves it reconstructs the corners "
                            "and the actuator must move to the uv pair.");
        } else {
            Log::get().note("sun glare steady: %s -- the corner stream is "
                            "%s per draw by the head's roll, measured from "
                            "the camera rows the train's own constants "
                            "carry.",
                            g_steady ? "ON" : "off",
                            g_steady ? "counter-rotated"
                                     : "no longer rotated");
        }
        if (!g_steady) releaseSteadyObjects();
    }
    if (g_mode != was || (g_mode == Mode::kFirst && g_keep != wasKeep)) {
        if (g_mode == Mode::kOff) {
            Log::get().note("sun glare: OFF -- the screen-space glare "
                            "element train (both beams, corona flare, "
                            "smudge, rays) is not drawn. The star's own "
                            "disc is a different draw and is untouched.");
        } else if (g_mode == Mode::kFirst) {
            Log::get().note("sun glare: drawing only the FIRST %u "
                            "instance(s) of the glare element train -- the "
                            "mapping walk. Step K and note what appears.",
                            g_keep);
        } else {
            Log::get().note("sun glare: stock.");
        }
    }
}

// The damper rides along: while it is configured on, the train matcher
// must keep running even with the glare fix itself stock, because the
// last-seen stamp is what scopes the damper to the sun.
bool sunglareWantsDraws() {
    return g_mode != Mode::kStock || g_steady || exposureDampingActive();
}

bool sunglareSteady() { return g_steady; }

uint64_t sunglareLastSeenMs() { return g_lastSeenMs; }

SunglareAction sunglareOnEyeDraw(char kind, uint32_t count,
                                 uint32_t instances) {
    if (!sunglareWantsDraws() ||
        !sunglareIsGlareTrain(kind, count, instances)) {
        return SunglareAction::kStock;
    }
    g_lastSeenMs = nowMs();
    if (g_mode == Mode::kOff) {
        ++g_skipped;
        return SunglareAction::kSkip;
    }
    if (g_mode == Mode::kFirst && instances > g_keep) {
        ++g_clamped;
        return SunglareAction::kClamp;
    }
    // Matched, not skipped, not clamped -- kMatch tells the caller a train
    // draw is happening, which is all the steady path needs to know.
    return SunglareAction::kMatch;
}

uint32_t sunglareKeep() { return g_keep; }

void sunglareBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    if (!g_steady || !ctx) return;
    if (!g_haveCorners) {
        captureCorners(ctx);
        return;   // this draw goes stock; the capture needs a round trip
    }

    // The roll, measured from the shadowed camera rows and TOUCHING
    // nothing: project world-up into the right/up plane; its in-plane
    // angle from the up row is how far the head has rolled the frame.
    uint32_t nf = 0;
    const float* sh = billboardShadowFloats(&nf);
    if (!sh || nf < 43) return;
    const float* r0 = sh + 16;
    const float* u0 = sh + 20;
    const float* wu = sh + 40;
    float rn[3], un[3], wn[3];
    float lr = 0, lu = 0, lw = 0;
    for (int i = 0; i < 3; ++i) {
        lr += r0[i] * r0[i];
        lu += u0[i] * u0[i];
        lw += wu[i] * wu[i];
    }
    lr = sqrtf(lr); lu = sqrtf(lu); lw = sqrtf(lw);
    if (lr < 1e-6f || lu < 1e-6f || lw < 1e-6f) return;
    for (int i = 0; i < 3; ++i) {
        rn[i] = r0[i] / lr;
        un[i] = u0[i] / lu;
        wn[i] = wu[i] / lw;
    }
    const float a = wn[0] * rn[0] + wn[1] * rn[1] + wn[2] * rn[2];
    const float b = wn[0] * un[0] + wn[1] * un[1] + wn[2] * un[2];
    const float n = sqrtf(a * a + b * b);
    // Near the zenith the horizon is undefined: world-up leaves the view
    // plane, the projection shrinks, and the measured angle turns to
    // noise -- which the field found as a BREATHING corona when pitching
    // up (and only up; pitching down moves AWAY from the pole, which is
    // the asymmetry that named this bug). The correction fades smoothly
    // to stock over the approach instead of jittering or snapping, and
    // the angle is low-passed besides.
    if (n < 0.05f) {
        g_thetaValid = false;
        return;
    }
    // The sign, settled in the field: the fixed-angle diagnostic proved
    // the shader consumes the rotation (a 30-degree spec tilted the beam
    // 30 degrees), which convicted the original positive sign -- the
    // elements were being rotated WITH the head, a doubled spin that
    // reads as "still rolls". The counter-rotation is the negative.
    float theta = atan2f(-a, b);
    float w = (n - 0.05f) / 0.25f;
    if (w > 1.0f) w = 1.0f;
    theta *= w;
    if (g_thetaValid && fabsf(theta - g_theta) < 1.5708f) {
        theta = g_theta + 0.25f * (theta - g_theta);
    }
    g_theta = theta;
    g_thetaValid = true;
    float c = cosf(theta);
    float s = sinf(theta);
    if (g_steadyMode == 2) {   // the fixed-angle diagnostic
        c = 0.86603f;
        s = 0.5f;
    }

    if (!g_ourVb) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kCornerBytes;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&bd, nullptr, &g_ourVb);
        dev->Release();
        if (!g_ourVb) return;
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g_ourVb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) ||
        !m.pData) {
        return;
    }
    // The vertex layout, as the first engagement taught it: halfs 0-1 are
    // the TEXTURE coordinate (0..1) and halfs 2-3 the corner expansion
    // direction (plus-minus one, centred on the element). Rotating the
    // first pair rotated the sampling inside a head-locked quad and
    // dragged neighbouring atlas art into view; the geometry lives in the
    // second pair, already origin-centred, so the rotation is a plain
    // origin spin and the texels ride untouched.
    uint16_t* out = static_cast<uint16_t*>(m.pData);
    memcpy(out, g_corners, kCornerBytes);
    for (uint32_t v = 0; v < kCornerVerts; ++v) {
        const float x = halfToFloat(g_corners[v * 4 + 2]);
        const float y = halfToFloat(g_corners[v * 4 + 3]);
        out[v * 4 + 2] = floatToHalf(c * x - s * y);
        out[v * 4 + 3] = floatToHalf(s * x + c * y);
    }
    ctx->Unmap(g_ourVb, 0);

    ctx->IAGetVertexBuffers(0, 1, &g_savedVb, &g_savedStride, &g_savedOffset);
    UINT stride = 8, offset = 0;
    ID3D11Buffer* ours = g_ourVb;
    ctx->IASetVertexBuffers(0, 1, &ours, &stride, &offset);
    g_engaged = true;

    ++g_applied;
    const uint64_t now = nowMs();
    if (g_applied == 1 || now - g_lastNoteMs >= 2000) {
        Log::get().note("sun glare steady: %llu counter-rotation(s) since "
                        "last note, current angle %.1f deg.",
                        static_cast<unsigned long long>(g_applied -
                                                        g_appliedAtNote),
                        atan2f(s, c) * 57.2958f);
        g_lastNoteMs = now;
        g_appliedAtNote = g_applied;
    }
}

void sunglareEnd(ID3D11DeviceContext* ctx) {
    if (!g_engaged || !ctx) return;
    ctx->IASetVertexBuffers(0, 1, &g_savedVb, &g_savedStride, &g_savedOffset);
    if (g_savedVb) {
        g_savedVb->Release();
        g_savedVb = nullptr;
    }
    g_engaged = false;
}

}  // namespace edvr
