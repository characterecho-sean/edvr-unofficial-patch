#include "shader_swap.h"
#include "temporal_shader_bytecode.h"
#include "sunglare_fix.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/config.h"
#include "../common/runtime_profile.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "billboard_fix.h"
#include "binding_shadow.h"
#include "exposure_fix.h"  // exposureDampingActive
// sunglare_vs.h is compiled by the build shader generator.

namespace edvr {

// sunglareWorldActive reads this from the header with no call: it is asked
// per draw, and the build has no /GL to fold a cross-TU getter.
namespace detail {
int  g_sunglareWorld = 0;
}  // namespace detail

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
// The shape half of the test is sunglareTrainShape (sunglare_fix.h), inline
// so the draw path can ask it first; it must say what these two say.
static_assert(kKind == 'N' && kVerts == 6,
              "sunglareTrainShape (sunglare_fix.h) must match the train's measured shape");

// The shipped surface: one key, three modes. realistic = the record-
// selected anchored elements, world-drawn (corona and smudge class,
// static in the world); vivid = every element, anchored ones world-
// pinned, the lens-flare sliders keeping their stock camera slide.
// kOff survives as a legacy spelling (hide the whole train).
enum class Mode { kStock, kOff, kRealistic, kVivid };

Mode     g_mode = Mode::kStock;
uint32_t g_keep = 0;
uint64_t g_lastSeenMs = 0;   // when the train last drew

// The shader swap. Bytecode is compiled by the build; any creation
// failure logs once and stands
// the swap down for the session -- the game then draws stock, which is
// the house's failure posture everywhere.
ID3D11VertexShader*  g_worldVs = nullptr;   // owned, lazy
bool                 g_worldTried = false;
ID3D11VertexShader*  g_savedVs = nullptr;  // the game's, across one draw
bool                 g_worldEngaged = false;

// The true head-tracked camera POSE (3x4 rows from scene-block offset
// 932), kept for the alignment telemetry -- the reading that finally
// closed the case: the glare CB's rows follow the head COMPLETELY
// (align 1.0, cf identical to tf through a full sweep). There is no
// camera clamp in the constants, so no calibrated projection to hold;
// what goes stale past the clamp is the game's CPU-computed element
// position, and that is fixed per draw from the rows alone.
float                g_trueView[12] = {};
bool                 g_trueViewValid = false;
bool                 g_camDumped = false;
ID3D11Buffer*        g_trueCb = nullptr;      // owned; bound at b2
ID3D11Buffer*        g_savedCb2 = nullptr;    // the game's, across a draw
bool                 g_cb2Engaged = false;

FaultBudget g_worldBudget("sunglareWorld", 3);


void buildWorldShaderInner(ID3D11DeviceContext* ctx,
                           ID3D11Device*& device, ID3D11VertexShader*& created,
                           HRESULT& result) {
    const void* bytes = kSunglareDefaultBytecode;
    const size_t size = sizeof(kSunglareDefaultBytecode);
    ctx->GetDevice(&device);
    if (device) {
        result = device->CreateVertexShader(bytes, size, nullptr, &created);
        ID3D11Device* released = device;
        device = nullptr;
        released->Release();
    }
}

void buildWorldShader(ID3D11DeviceContext* ctx) {
    g_worldTried = true;
    ID3D11Device* device = nullptr;
    ID3D11VertexShader* created = nullptr;
    HRESULT result = E_FAIL;
    const bool ran = guardedBudget(g_worldBudget,
                  [&] { buildWorldShaderInner(ctx, device, created, result); });
    if (device) guarded("sunglareWorld release", [&] { device->Release(); });
    if (!ran || FAILED(result)) {
        if (created) guarded("sunglareWorld shader release", [&] { created->Release(); });
    } else g_worldVs = created;
    Log::get().note("sun glare world: shader %s.",
                    g_worldVs ? "CREATED" : "creation FAILED; stock");
}

}  // namespace

bool sunglareIsGlareTrain(char kind, uint32_t count, uint32_t instances) {
    if (!sunglareTrainShape(kind, count, instances)) return false;
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
    const std::string v = runtimeVrProfile() ?
        cfg.getString("fix.sun_glare", "vivid") : "stock";
    bool legacy = false;
    if (v == "stock") {
        g_mode = Mode::kStock;
    } else if (v == "realistic") {
        g_mode = Mode::kRealistic;
    } else if (v == "vivid") {
        g_mode = Mode::kVivid;
    } else if (v == "off") {
        g_mode = Mode::kOff;         // legacy spelling, kept working
        legacy = true;
    } else if (v.compare(0, 6, "first:") == 0) {
        g_mode = Mode::kRealistic;   // the debug arc's clamp; the record
        legacy = true;               // selection is its successor
    } else {
        Log::get().note("sun glare: \"%s\" is not stock, realistic or "
                        "vivid; staying stock.", v.c_str());
        g_mode = Mode::kStock;
    }
    // Both modes ride the world-anchored shader.
    const int wasWorld = detail::g_sunglareWorld;
    detail::g_sunglareWorld = (g_mode == Mode::kRealistic || g_mode == Mode::kVivid)
                  ? 1
                  : 0;
    // The billboard loan's tee stays armed for world mode: the per-draw
    // camera solve reads the shadowed CB.
    billboardGlareWatch(detail::g_sunglareWorld != 0);
    if (legacy && (g_mode != was || detail::g_sunglareWorld != wasWorld)) {
        Log::get().note("sun glare: legacy value \"%s\" accepted (%s). The "
                        "shipped modes are stock, realistic and vivid.",
                        v.c_str(),
                        g_mode == Mode::kOff ? "train hidden"
                                             : "treated as realistic");
    }
    if (g_mode != was) {
        if (g_mode == Mode::kOff) {
            Log::get().note("sun glare: OFF -- the glare element train "
                            "(corona, smudge, beams, rays, flare) is not "
                            "drawn. The star's own disc is a different "
                            "draw and is untouched.");
        } else if (g_mode == Mode::kRealistic) {
            Log::get().note("sun glare: REALISTIC -- anchored elements "
                            "(corona and smudge class) drawn world-locked "
                            "by the replacement vertex shader; beams, rays "
                            "and lens-flare sliders removed. Selection is "
                            "by record, immune to the game's head-look "
                            "element reordering.");
        } else if (g_mode == Mode::kVivid) {
            Log::get().note("sun glare: VIVID -- every element drawn; "
                            "anchored ones world-locked, the lens-flare "
                            "sliders keeping their stock camera slide.");
        } else {
            Log::get().note("sun glare: stock.");
        }
    }
}

// The damper rides along: while it is configured on, the train matcher
// must keep running even with the glare fix itself stock, because the
// last-seen stamp is what scopes the damper to the sun.
bool sunglareWantsDraws() {
    return g_mode != Mode::kStock || exposureDampingActive();
}

uint64_t sunglareLastSeenMs() { return g_lastSeenMs; }

SunglareAction sunglareOnEyeDraw(char kind, uint32_t count,
                                 uint32_t instances) {
    if (!sunglareWantsDraws() ||
        !sunglareIsGlareTrain(kind, count, instances)) {
        return SunglareAction::kStock;
    }
    g_lastSeenMs = nowMs();
    if (g_mode == Mode::kOff) return SunglareAction::kSkip;
    // Matched and not skipped -- kMatch tells the caller a train draw
    // is happening. The first:K clamp is retired: the game's element
    // list reorders with its head-look camera, so a positional prefix
    // named different elements as the head turned; the world shader
    // selects by record instead.
    return SunglareAction::kMatch;
}

uint32_t sunglareKeep() { return g_keep; }

// The scene-CB follow: any big eye-target draw's 208-byte constants are
// the engine-standard camera block carrying that eye's TRUE view rows --
// the flash detector's 5376-byte buffer was the wrong well (its head is
// zeros; my first grab fed the shader nothing). The cockpit geometry
// draws before each eye's glare, so the last scene write before a glare
// draw is that same eye's true camera.
void* g_sceneCbTarget = nullptr;

void sunglareSceneCb(void* cb) { g_sceneCbTarget = cb; }

void* sunglareSceneCbTarget() {
    return detail::g_sunglareWorld ? g_sceneCbTarget : nullptr;
}

void sunglareSceneRows(const void* data, uint32_t bytes) {
    // The TRUE head-tracked view matrix lives at float offset 932 of
    // the big scene block -- named by the two-shot dump: this camera
    // turned the full 150 degrees with the head while the glare
    // camera's constants froze at the clamp. Three 3x4 rows, rotation
    // plus translation; validated as near-unit orthogonal before use.
    if (!detail::g_sunglareWorld || !data || bytes < (944 * 4)) return;
    const float* f = static_cast<const float*>(data) + 932;
    const float l0 = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    const float l1 = sqrtf(f[4] * f[4] + f[5] * f[5] + f[6] * f[6]);
    const float l2 = sqrtf(f[8] * f[8] + f[9] * f[9] + f[10] * f[10]);
    if (!(l0 > 0.9f && l0 < 1.1f)) return;
    if (!(l1 > 0.9f && l1 < 1.1f)) return;
    if (!(l2 > 0.9f && l2 < 1.1f)) return;
    memcpy(g_trueView, f, sizeof(g_trueView));
    g_trueViewValid = true;
    if (!g_camDumped) {
        g_camDumped = true;
        Log::get().note("true view matrix live (offset 932; |rows| %.3f "
                        "%.3f %.3f).", l0, l1, l2);
    }
}

void sunglareBegin(ID3D11DeviceContext* ctx) {
    g_worldEngaged = false;
    if (!ctx) return;

    // With the world shader in, position, orientation, facing and per-eye
    // agreement are all computed correctly inside the pipeline, and the
    // corner stream stays the game's own.
    if (detail::g_sunglareWorld) {
        if (!g_worldTried) buildWorldShader(ctx);
        if (g_worldVs) {
            ctx->VSGetShader(&g_savedVs, nullptr, nullptr);
            ctx->VSSetShader(g_worldVs, nullptr, 0);
            g_worldEngaged = true;

            // The true-camera constants at b2: rows 4, 5 and 7 of the
            // scene camera, plus a validity flag the shader reads --
            // stale or absent rows fall back to the clamped cb0 rows,
            // which is exactly the pre-b2 behaviour.
            if (!g_trueCb) {
                ID3D11Device* dev = nullptr;
                ctx->GetDevice(&dev);
                if (dev) {
                    D3D11_BUFFER_DESC bd{};
                    bd.ByteWidth = 96;
                    bd.Usage = D3D11_USAGE_DYNAMIC;
                    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                    dev->CreateBuffer(&bd, nullptr, &g_trueCb);
                    dev->Release();
                }
            }
            // The per-draw camera solve -- what the whole b2 arc
            // collapsed into once the field data spoke. The rows follow
            // the head completely (align 1.0, cf identical to tf through
            // a full sweep): no camera clamp, nothing to calibrate. The
            // stale quantity is the game's CPU-computed element position,
            // and the glare world's origin sits ON the sun (the row w
            // components projected the visual sun through every session
            // of eccentricity telemetry). For any perspective projection
            // the fourth column is zero in x, y and w, so the camera
            // position annihilates those rows -- dot(row.xyz, cam) =
            // -row.w -- and cam solves from this draw's own constants.
            // True sun direction = -normalize(cam). Per draw, per eye,
            // nothing latched, nothing to go stale.
            uint32_t nsh = 0;
            const float* sh = billboardShadowFloats(&nsh);
            float cam[3] = {};
            float sunDir[3] = {};
            if (sh && nsh >= 32) {
                const float* r4 = sh + 16;
                const float* r5 = sh + 20;
                const float* r7 = sh + 28;
                const float det =
                    r4[0] * (r5[1] * r7[2] - r5[2] * r7[1]) -
                    r4[1] * (r5[0] * r7[2] - r5[2] * r7[0]) +
                    r4[2] * (r5[0] * r7[1] - r5[1] * r7[0]);
                if (fabsf(det) > 1e-9f) {
                    const float x = -r4[3], y = -r5[3], z = -r7[3];
                    const float inv = 1.0f / det;
                    cam[0] = inv * (x * (r5[1] * r7[2] - r5[2] * r7[1]) -
                                    r4[1] * (y * r7[2] - r5[2] * z) +
                                    r4[2] * (y * r7[1] - r5[1] * z));
                    cam[1] = inv * (r4[0] * (y * r7[2] - r5[2] * z) -
                                    x * (r5[0] * r7[2] - r5[2] * r7[0]) +
                                    r4[2] * (r5[0] * z - y * r7[0]));
                    cam[2] = inv * (r4[0] * (r5[1] * z - y * r7[1]) -
                                    r4[1] * (r5[0] * z - y * r7[0]) +
                                    x * (r5[0] * r7[1] - r5[1] * r7[0]));
                    const float lc = sqrtf(cam[0] * cam[0] +
                                           cam[1] * cam[1] +
                                           cam[2] * cam[2]);
                    if (lc > 1e-3f) {
                        sunDir[0] = -cam[0] / lc;
                        sunDir[1] = -cam[1] / lc;
                        sunDir[2] = -cam[2] / lc;
                    }
                }
            }
            if (g_trueCb) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(ctx->Map(g_trueCb, 0, D3D11_MAP_WRITE_DISCARD,
                                       0, &m)) &&
                    m.pData) {
                    float* f = static_cast<float*>(m.pData);
                    memset(f, 0, 48);            // row substitution retired
                    f[12] = 0.0f;                // tValid.x: rows are honest
                    // tValid.y HELD AT ZERO: the camera solve is proven
                    // (d tracks the floating-origin sawtooth exactly,
                    // resetting to the pose-translation magnitude at
                    // each re-anchor) but the origin it points at is the
                    // game's drifting world anchor, NOT the sun -- the
                    // rebuild pinned the disc to the view axis. Stays
                    // dark until the sun's own triplet is identified in
                    // the camera block.
                    f[13] = 0.0f;
                    // tValid.z: element SELECTION. realistic keeps the
                    // anchored, non-axis-locked elements -- the corona
                    // and smudge class -- selected per RECORD in the
                    // shader, immune to the game's head-look element
                    // reordering. vivid keeps every element.
                    f[14] = (g_mode == Mode::kRealistic) ? 1.0f : 0.0f;
                    f[15] = 0.0f;
                    f[16] = sunDir[0];           // tSun
                    f[17] = sunDir[1];
                    f[18] = sunDir[2];
                    f[19] = 0.0f;
                    f[20] = cam[0];              // tCam
                    f[21] = cam[1];
                    f[22] = cam[2];
                    f[23] = 0.0f;
                    ctx->Unmap(g_trueCb, 0);
                    ctx->VSGetConstantBuffers(2, 1, &g_savedCb2);
                    ID3D11Buffer* ours = g_trueCb;
                    ctx->VSSetConstantBuffers(2, 1, &ours);
                    g_cb2Engaged = true;
                }
            }
        }
        return;
    }
}

void sunglareEnd(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    if (g_worldEngaged) {
        ctx->VSSetShader(g_savedVs, nullptr, 0);
        if (g_savedVs) {
            g_savedVs->Release();
            g_savedVs = nullptr;
        }
        if (g_cb2Engaged) {
            ctx->VSSetConstantBuffers(2, 1, &g_savedCb2);
            if (g_savedCb2) {
                g_savedCb2->Release();
                g_savedCb2 = nullptr;
            }
            g_cb2Engaged = false;
        }
        g_worldEngaged = false;
    }
}

}  // namespace edvr
