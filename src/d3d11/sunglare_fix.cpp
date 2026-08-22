#include "sunglare_fix.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "billboard_fix.h"
#include "binding_shadow.h"
#include "exposure_fix.h"  // exposureDampingActive, lookupShaderHash
#include "sunglare_vs.h"

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
float    g_recenter = 0;     // fix.sun_glare_recenter, REPURPOSED after
                             // the eye-pair measurement: the eye-sync
                             // gain. Each eye's elements shift by half
                             // the inter-eye sun-position difference
                             // toward the pair mean -- anti-symmetric, so
                             // the lateral position stays stock and only
                             // the disparity (the depth artifact)
                             // collapses.
float    g_spOther[3] = {};  // the OTHER eye's sun position, one matched
                             // draw ago -- the train alternates eyes
bool     g_spOtherValid = false;
uint64_t g_spOtherMs = 0;
float    g_eyeshape = 0;     // fix.sun_glare_eyeshape: strength of the
                             // per-eye anisotropic corner compensation
                             // that equalizes the angular patch the two
                             // eyes' quads subtend -- the disparity
                             // GRADIENT the uniform knob could not touch
uint64_t g_lastSeenMs = 0;   // when the train last drew

// The eye-pair logger, the eye-sync approach's measurement: the train
// draws once per eye, alternating, so two consecutive matched draws are
// the two eyes' CBs -- log both sides' candidate fields and the diff
// names what the eyes actually disagree about, BEFORE anything is
// synced. Capped per session; re-toggle steady for another run.
uint64_t g_pairLastMs = 0;
int      g_pairState = 0;    // 0 idle, 1 log-as-A, 2 log-as-B
uint32_t g_pairsLogged = 0;
constexpr uint32_t kPairMax = 24;
bool     g_shadersNoted = false;

// The shader swap. Compiled once per session through d3dcompiler_47
// (present on every Windows 10/11); any failure logs once and stands
// the swap down for the session -- the game then draws stock, which is
// the house's failure posture everywhere.
// fix.sun_glare_world selects a compiled VARIANT, so in-shader
// bisection happens live from the ini without a rebuild: 1 = normal,
// 2 = visibility gate bypassed, 3 = every element world-anchored,
// 4 = every element on the ported flat path.
constexpr int kWorldVariants = 4;
int                  g_world = 0;
ID3D11VertexShader*  g_worldVs[kWorldVariants] = {};   // owned, lazy
bool                 g_worldTried[kWorldVariants] = {};
ID3D11VertexShader*  g_savedVs = nullptr;  // the game's, across one draw
bool                 g_worldEngaged = false;
uint64_t             g_worldDraws = 0;
uint64_t             g_worldDrawsAtNote = 0;
uint64_t             g_worldNoteMs = 0;
float                g_worldEccMin = 1e9f;
float                g_worldEccMax = -1e9f;

// The true camera rows, captured from the SCENE camera constants the
// transition-flash tee already observes -- because the glare system is
// fed the game's internal head-look camera, which CLAMPS at 45 degrees
// from ship-forward: past the clamp the glare CB simply does not know
// where the star is, and no shader logic can recover information its
// constants lack. The scene camera knows. Rows 4, 5 and 7 of the same
// engine-standard layout, latest write wins (the frame interleaves per
// eye, scene block before glare block, so the latest write is this
// eye's).
float                g_trueRows[16] = {};
bool                 g_trueRowsValid = false;
uint64_t             g_trueRowsMs = 0;
// The true head-tracked camera POSE (3x4 rows from scene-block offset
// 932), kept for the alignment telemetry -- the reading that finally
// closed the case: the glare CB's rows follow the head COMPLETELY
// (align 1.0, cf identical to tf through a full sweep). There is no
// camera clamp in the constants, so no calibrated projection to hold;
// what goes stale past the clamp is the game's CPU-computed element
// position, and that is fixed per draw from the rows alone.
float                g_trueView[12] = {};
bool                 g_trueViewValid = false;
uint64_t             g_trueViewMs = 0;
float                g_camDist = 0.0f;   // |camera| in the glare frame,
                                         // from the per-draw solve. NOT
                                         // the sun distance: the field
                                         // showed the origin is Elite's
                                         // drifting floating-origin
                                         // anchor (re-anchored at the
                                         // ship every ~25s, then ~190
                                         // units/s away), not the sun.
bool                 g_sunSolveOk = false;
float                g_solvedCam[3] = {};
// The camera-block census: the glare CB is 208 bytes and the shader
// only reads rows 4..7 (floats 16..31). The sun's own position or
// direction is somewhere in the rest; a few timed prints of the
// unmapped floats, with a head turn between them, separate the
// head-locked from the world-locked candidates.
int                  g_blockShots = 0;
uint64_t             g_blockShotMs = 0;
bool                 g_camDumped = false;
int                  g_camDumpShot = 0;
uint64_t             g_camDumpMs = 0;
ID3D11Buffer*        g_trueCb = nullptr;      // owned; bound at b2
ID3D11Buffer*        g_savedCb2 = nullptr;    // the game's, across a draw
bool                 g_cb2Engaged = false;

// The FULL eleven-parameter signature. The first field build declared
// ten -- no ppErrorMsgs -- so D3DCompile wrote its error-blob pointer
// through whatever garbage sat in the eleventh slot, and the game
// crashed at the first matched draw. The project's first crash, bought
// by an FFI signature nobody proof-read. The HLSL itself desk-compiles
// clean; the game is never again the compiler's first audience.
typedef HRESULT(WINAPI* PFN_D3DCompile)(const void*, SIZE_T, const char*,
                                        const void*, void*, const char*,
                                        const char*, UINT, UINT, void**,
                                        void**);

// ID3DBlob vtable through raw COM: 0-2 IUnknown, 3 GetBufferPointer,
// 4 GetBufferSize.
void* blobPtr(void* blob) {
    typedef void*(STDMETHODCALLTYPE * Fn)(void*);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(blob))[3])(blob);
}
SIZE_T blobSize(void* blob) {
    typedef SIZE_T(STDMETHODCALLTYPE * Fn)(void*);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(blob))[4])(blob);
}
void blobRelease(void* blob) {
    typedef ULONG(STDMETHODCALLTYPE * Fn)(void*);
    reinterpret_cast<Fn>((*reinterpret_cast<void***>(blob))[2])(blob);
}

FaultBudget g_worldBudget("sunglareWorld", 3);

struct ShaderMacro { const char* name; const char* def; };

void buildWorldShaderInner(ID3D11DeviceContext* ctx, int variant) {
    HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!mod) {
        Log::get().note("sun glare world: d3dcompiler_47.dll not found; "
                        "the swap stands down and the game draws stock.");
        return;
    }
    PFN_D3DCompile compile = reinterpret_cast<PFN_D3DCompile>(
        GetProcAddress(mod, "D3DCompile"));
    if (!compile) return;
    ShaderMacro macros[3] = {};
    int m = 0;
    if (variant == 2) macros[m++] = {"NOGATE", "1"};
    if (variant == 3) macros[m++] = {"ALLWORLD", "1"};
    if (variant == 4) macros[m++] = {"ALLFLAT", "1"};
    void* blob = nullptr;
    void* errors = nullptr;
    const HRESULT hr = compile(kSunglareWorldVS, sizeof(kSunglareWorldVS) - 1,
                               "sunglare_world_vs",
                               m ? macros : nullptr, nullptr, "main",
                               "vs_5_0", 0, 0, &blob, &errors);
    if (errors) {
        if (FAILED(hr)) {
            Log::get().note("sun glare world: compile errors: %.300s",
                            static_cast<const char*>(blobPtr(errors)));
        }
        blobRelease(errors);
    }
    if (FAILED(hr) || !blob) {
        Log::get().note("sun glare world: shader compile failed (0x%08X); "
                        "the swap stands down and the game draws stock.",
                        static_cast<unsigned>(hr));
        return;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (dev) {
        dev->CreateVertexShader(blobPtr(blob), blobSize(blob), nullptr,
                                &g_worldVs[variant - 1]);
        dev->Release();
    }
    blobRelease(blob);
    Log::get().note("sun glare world: variant %d %s.", variant,
                    g_worldVs[variant - 1] ? "COMPILED"
                                           : "creation FAILED; stock");
}

void buildWorldShader(ID3D11DeviceContext* ctx, int variant) {
    g_worldTried[variant - 1] = true;
    guardedBudget(g_worldBudget,
                  [&] { buildWorldShaderInner(ctx, variant); });
}
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
        if (g_steady) {
            g_pairsLogged = 0;   // each steady rising edge buys a fresh
                                 // eye-pair budget
            g_pairState = 0;
        }
        if (!g_steady) releaseSteadyObjects();
    }
    const float wasRecenter = g_recenter;
    float rc = cfg.getFloat("fix.sun_glare_recenter", 0.0f);
    if (rc < -4.0f) rc = -4.0f;
    if (rc > 4.0f) rc = 4.0f;
    g_recenter = rc;
    const int wasWorld = g_world;
    g_world = cfg.getIntInRange("fix.sun_glare_world", 0, 0, kWorldVariants);
    if (g_world != wasWorld) {
        static const char* kVariantNames[] = {
            "off", "normal", "GATE BYPASSED (diagnostic)",
            "ALL ELEMENTS WORLD-ANCHORED (diagnostic)",
            "ALL ELEMENTS FLAT (diagnostic)"};
        Log::get().note("sun glare world: %s -- the train's vertex shader "
                        "is %s the world-anchored replacement (written "
                        "against the dumped vs 94D5C556DFD6D705).",
                        kVariantNames[g_world],
                        g_world ? "substituted with" : "no longer");
    }
    // The billboard loan's tee arms for world mode too: the telemetry
    // reads the sun position out of the shadowed CB.
    billboardGlareWatch(g_steady || g_world != 0);
    const float wasEyeshape = g_eyeshape;
    float es = cfg.getFloat("fix.sun_glare_eyeshape", 0.0f);
    if (es < 0.0f) es = 0.0f;
    if (es > 2.0f) es = 2.0f;
    g_eyeshape = es;
    if (g_eyeshape != wasEyeshape) {
        Log::get().note("sun glare eyeshape: strength %.2f -- each eye's "
                        "glare quad is reshaped so both subtend the same "
                        "angular patch (geometric, from each eye's own sun "
                        "direction; 1 = the full sec-squared answer).",
                        g_eyeshape);
    }
    if (g_recenter != wasRecenter) {
        Log::get().note("sun glare recenter: gain %.2f -- the kept glare "
                        "elements are pushed back toward the star's true "
                        "direction against the flare placement slide. Tune "
                        "live at a bright star, sun held well off-centre: "
                        "the right gain glues the smudge to the star; the "
                        "wrong sign doubles the slide.", g_recenter);
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
    return g_mode != Mode::kStock || g_steady || g_world ||
           exposureDampingActive();
}

bool sunglareSteady() { return g_steady; }

bool sunglareWorldActive() { return g_world != 0; }

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

// The scene-CB follow: any big eye-target draw's 208-byte constants are
// the engine-standard camera block carrying that eye's TRUE view rows --
// the flash detector's 5376-byte buffer was the wrong well (its head is
// zeros; my first grab fed the shader nothing). The cockpit geometry
// draws before each eye's glare, so the last scene write before a glare
// draw is that same eye's true camera.
void* g_sceneCbTarget = nullptr;

void sunglareSceneCb(void* cb) { g_sceneCbTarget = cb; }

void* sunglareSceneCbTarget() {
    return g_world ? g_sceneCbTarget : nullptr;
}

// One whole-buffer binary dump of the big scene-constants block (the
// glare shader's own cb1, by size), to be matched desk-side against the
// clamped rows -- nearly equal at level head -- so the true camera's
// offset names itself. Written once per session while world mode is on.
void sunglareSceneDump(const void* data, uint32_t bytes) {
    // Two shots per session, both gated on a glare draw having happened
    // (the loading-state block is identity matrices wall to wall). Shot
    // one lands at the star with the head level; shot two eight seconds
    // later with the head held past the clamp -- the camera offset that
    // FOLLOWED the head between the shots is the true one, the offset
    // frozen at forty-five degrees is the head-look camera. Level-head
    // dumps alone cannot tell them apart, which shot one proved.
    if (!g_world || g_camDumpShot >= 2 || g_lastSeenMs == 0 || !data ||
        bytes < 1024) {
        return;
    }
    const uint64_t now = nowMs();
    if (g_camDumpShot == 1 && now - g_camDumpMs < 8000) return;
    ++g_camDumpShot;
    g_camDumpMs = now;
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\scenecb%d.bin",
                 Config::get().logDir().c_str(), g_camDumpShot);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, data, bytes, &written, nullptr);
    CloseHandle(h);
    Log::get().note("scene constants shot %d dumped (%u bytes).",
                    g_camDumpShot, bytes);
}

void sunglareSceneRows(const void* data, uint32_t bytes) {
    // The TRUE head-tracked view matrix lives at float offset 932 of
    // the big scene block -- named by the two-shot dump: this camera
    // turned the full 150 degrees with the head while the glare
    // camera's constants froze at the clamp. Three 3x4 rows, rotation
    // plus translation; validated as near-unit orthogonal before use.
    if (!g_world || !data || bytes < (944 * 4)) return;
    const float* f = static_cast<const float*>(data) + 932;
    const float l0 = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    const float l1 = sqrtf(f[4] * f[4] + f[5] * f[5] + f[6] * f[6]);
    const float l2 = sqrtf(f[8] * f[8] + f[9] * f[9] + f[10] * f[10]);
    if (!(l0 > 0.9f && l0 < 1.1f)) return;
    if (!(l1 > 0.9f && l1 < 1.1f)) return;
    if (!(l2 > 0.9f && l2 < 1.1f)) return;
    memcpy(g_trueView, f, sizeof(g_trueView));
    g_trueViewValid = true;
    g_trueViewMs = nowMs();
    if (!g_camDumped) {
        g_camDumped = true;
        Log::get().note("true view matrix live (offset 932; |rows| %.3f "
                        "%.3f %.3f).", l0, l1, l2);
    }
}

void sunglareBegin(ID3D11DeviceContext* ctx) {
    g_engaged = false;
    g_worldEngaged = false;
    if (!ctx) return;

    // The shader swap outranks the corner machinery: with the world
    // shader in, position, orientation, facing and per-eye agreement
    // are all computed correctly inside the pipeline, and the corner
    // stream stays the game's own.
    if (g_world) {
        // The cutoff telemetry: matched draws per second and the
        // eccentricity range the CB reports, so a disappearance names
        // its side -- draws stopping = the game culled upstream; draws
        // continuing = our shader killed them.
        ++g_worldDraws;
        {
            uint32_t nf = 0;
            const float* sh = billboardShadowFloats(&nf);
            if (sh && nf >= 32) {
                const float pz = fabsf(sh[31]);
                const float pxy = sqrtf(sh[19] * sh[19] + sh[23] * sh[23]);
                if (pz > 1e-3f) {
                    const float t = pxy / pz;
                    if (t < g_worldEccMin) g_worldEccMin = t;
                    if (t > g_worldEccMax) g_worldEccMax = t;
                }
            }
            const uint64_t now = nowMs();
            if (now - g_worldNoteMs >= 1000) {
                float align = -2.0f;
                float cf[3] = {}, tf[3] = {};
                if (sh && nf >= 32 && g_trueViewValid) {
                    const float* c = sh + 28;
                    const float lc = sqrtf(c[0] * c[0] + c[1] * c[1] +
                                           c[2] * c[2]);
                    if (lc > 1e-4f) {
                        for (int k = 0; k < 3; ++k) {
                            cf[k] = c[k] / lc;
                            tf[k] = g_trueView[k * 4 + 2];   // column 2
                        }
                        align = cf[0] * tf[0] + cf[1] * tf[1] +
                                cf[2] * tf[2];
                    }
                }
                Log::get().note("glare world: %llu draw(s)/s, ecc tan "
                                "%.2f..%.2f, align %.4f S%s d=%.1f "
                                "cf=(%.2f %.2f %.2f) tf=(%.2f %.2f %.2f).",
                                static_cast<unsigned long long>(
                                    g_worldDraws - g_worldDrawsAtNote),
                                g_worldEccMin, g_worldEccMax, align,
                                g_sunSolveOk ? "OK" : "--", g_camDist,
                                cf[0], cf[1], cf[2], tf[0], tf[1], tf[2]);
                g_worldNoteMs = now;
                g_worldDrawsAtNote = g_worldDraws;
                g_worldEccMin = 1e9f;
                g_worldEccMax = -1e9f;

                // The camera-block census, six shots five seconds
                // apart: every float the shader does NOT read, plus the
                // solved camera. A unit triplet at the sun's angle is a
                // direction; a triplet that lands on the sun after
                // subtracting cam is a position; anything that moves
                // with the head between shots is view-space.
                if (sh && nf >= 52 && g_blockShots < 6 &&
                    now - g_blockShotMs >= 5000) {
                    ++g_blockShots;
                    g_blockShotMs = now;
                    char line[640];
                    int o = 0;
                    for (int k = 0; k < 16 && o < 600; ++k)
                        o += snprintf(line + o, sizeof(line) - o, "%s%.3f",
                                      k ? " " : "", sh[k]);
                    Log::get().note("glare camblock %d f00..f15: %s",
                                    g_blockShots, line);
                    o = 0;
                    for (int k = 32; k < 52 && o < 600; ++k)
                        o += snprintf(line + o, sizeof(line) - o, "%s%.3f",
                                      k > 32 ? " " : "", sh[k]);
                    Log::get().note("glare camblock %d f32..f51: %s "
                                    "cam=(%.1f %.1f %.1f)",
                                    g_blockShots, line, g_solvedCam[0],
                                    g_solvedCam[1], g_solvedCam[2]);
                }
            }
        }
        const int v = g_world - 1;
        if (!g_worldTried[v]) buildWorldShader(ctx, g_world);
        if (g_worldVs[v]) {
            ctx->VSGetShader(&g_savedVs, nullptr, nullptr);
            ctx->VSSetShader(g_worldVs[v], nullptr, 0);
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
            bool sunOk = false;
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
                        g_camDist = lc;
                        g_solvedCam[0] = cam[0];
                        g_solvedCam[1] = cam[1];
                        g_solvedCam[2] = cam[2];
                        sunOk = true;
                    }
                }
            }
            g_sunSolveOk = sunOk;

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
                    f[14] = f[15] = 0.0f;
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

    if (!g_steady) return;

    // The shader-swap arc's identification, once per session: which
    // vertex and pixel shader the train binds. With glare_shader_dump
    // armed their blobs are already on disk under these hashes.
    if (!g_shadersNoted) {
        g_shadersNoted = true;
        ID3D11VertexShader* vs = nullptr;
        ID3D11PixelShader* ps = nullptr;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->PSGetShader(&ps, nullptr, nullptr);
        Log::get().note("glare train shaders: vs=%016llX ps=%016llX (blobs "
                        "in edvr_logs\\shaders when glare_shader_dump=1).",
                        static_cast<unsigned long long>(lookupShaderHash(vs)),
                        static_cast<unsigned long long>(lookupShaderHash(ps)));
        if (vs) vs->Release();
        if (ps) ps->Release();
    }
    if (!g_haveCorners) {
        captureCorners(ctx);
        return;   // this draw goes stock; the capture needs a round trip
    }

    // The angle, measured from the shadowed camera rows and TOUCHING
    // nothing. World-up expressed in VIEW space is simply the y-column
    // of the head-basis rows: (sh[17], sh[21], sh[29]). Its x and y
    // components are the roll -- the original formula -- but a sprite
    // off the view axis twists with the projection too, which the G
    // star taught as a corona that held under roll and turned under
    // yaw. The general form projects world-up perpendicular to the
    // SUN'S direction first; the second matrix's translation is the
    // sun's view position, and when it reads degenerate the centre
    // formula stands in.
    uint32_t nf = 0;
    const float* sh = billboardShadowFloats(&nf);
    if (!sh || nf < 48) return;
    const float* r0 = sh + 16;
    const float* u0 = sh + 20;
    float lr = 0, lu = 0;
    for (int i = 0; i < 3; ++i) {
        lr += r0[i] * r0[i];
        lu += u0[i] * u0[i];
    }
    lr = sqrtf(lr); lu = sqrtf(lu);
    if (lr < 1e-6f || lu < 1e-6f) return;
    float wuv[3] = {sh[17], sh[21], sh[29]};
    const float lwv = sqrtf(wuv[0] * wuv[0] + wuv[1] * wuv[1] +
                            wuv[2] * wuv[2]);
    if (lwv < 1e-6f) return;
    for (int i = 0; i < 3; ++i) wuv[i] /= lwv;

    // APPLIED angle: world-up projected perpendicular to the SUN'S view
    // direction, from the position triplet [19/23/31] -- promoted on
    // field evidence after its predecessor ([39/43/47]) turned out to be
    // an accumulator: distance rock-stable while parked, angle equal to
    // the centre formula with the sun centred (delta ~1.5 deg), and
    // stable within ~4 deg across a 30-degree yaw that twisted the
    // centre formula by 33. The centre formula remains the fallback for
    // degenerate reads, and the telemetry prints both so any future
    // divergence names itself.
    // The eye-pair log, before anything else touches the values.
    const uint64_t nowPair = nowMs();
    if (g_pairsLogged < kPairMax && g_pairState == 0 &&
        nowPair - g_pairLastMs >= 3000) {
        g_pairState = 1;
    }
    if (g_pairState >= 1) {
        Log::get().note(
            "glare eye-pair %c: sp=(%.6g %.6g %.6g) f0=(%.4g %.4g %.4g "
            "%.4g) f4=(%.4g %.4g %.4g %.4g) f8=(%.4g %.4g %.4g %.4g) "
            "f12=(%.4g %.4g %.4g %.4g)",
            g_pairState == 1 ? 'A' : 'B', sh[19], sh[23], sh[31], sh[0],
            sh[1], sh[2], sh[3], sh[4], sh[5], sh[6], sh[7], sh[8], sh[9],
            sh[10], sh[11], sh[12], sh[13], sh[14], sh[15]);
        if (g_pairState == 2) {
            g_pairState = 0;
            g_pairLastMs = nowPair;
            ++g_pairsLogged;
            if (g_pairsLogged == kPairMax) {
                Log::get().note("glare eye-pair: session budget spent; "
                                "toggle sun_glare_steady off and on for "
                                "another run.");
            }
        } else {
            g_pairState = 2;
        }
    }

    // The eccentricity geometry, computed once: this eye's sun direction,
    // its tangent-plane stretch, and the radial direction on screen. The
    // angle uses it to express world-up in the DRAWN frame; the corner
    // block below uses it to equalize the eyes' angular patches.
    float a = wuv[0];
    float b = wuv[1];
    bool sunAnchored = false;
    float candDist = 0;
    float eccT = 0, eccRx = 1, eccRy = 0;
    {
        const float sp[3] = {sh[19], sh[23], sh[31]};
        candDist = sqrtf(sp[0] * sp[0] + sp[1] * sp[1] + sp[2] * sp[2]);
        const float pz = fabsf(sp[2]);
        const float pxy = sqrtf(sp[0] * sp[0] + sp[1] * sp[1]);
        if (pz > 1e-3f) {
            eccT = pxy / pz;
            if (pxy > 1e-3f) {
                eccRx = sp[0] / pxy;
                eccRy = sp[1] / pxy;
            }
        }
        if (candDist > 1e-3f) {
            const float d[3] = {sp[0] / candDist, sp[1] / candDist,
                                sp[2] / candDist};
            const float wd =
                wuv[0] * d[0] + wuv[1] * d[1] + wuv[2] * d[2];
            const float px = wuv[0] - d[0] * wd;
            const float py = wuv[1] - d[1] * wd;
            if (px * px + py * py > 0.0025f) {
                a = px;
                b = py;
                sunAnchored = true;
            }
        }
    }
    // Express the reference in the drawn (plane) frame: the projection's
    // local stretch scales the radial component by sec-theta relative to
    // the tangential, and that scaling ROTATES any direction that is
    // neither -- the residual the field saw as the disc spinning about
    // the star's axis under yaw, after everything else held. Blended by
    // the eyeshape strength: the same geometry, the same knob.
    if (sunAnchored && g_eyeshape != 0.0f && eccT > 1e-4f) {
        const float sec1 = sqrtf(1.0f + eccT * eccT);
        const float secEff = 1.0f + g_eyeshape * (sec1 - 1.0f);
        const float ar = (a * eccRx + b * eccRy) * secEff;
        const float at = -a * eccRy + b * eccRx;
        a = ar * eccRx - at * eccRy;
        b = ar * eccRy + at * eccRx;
    }
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
    // origin spin and the texels ride untouched. The recenter term rides
    // the same pair: a uniform translation of all six corners shifts the
    // whole quad, per eye, back toward the star's true direction against
    // the flare placement slide -- gain configured, because the slide
    // factor is the game's secret and the corner-to-screen scale is the
    // instance's.
    float tx = 0, ty = 0;
    {
        const float spNow[3] = {sh[19], sh[23], sh[31]};
        // The eye-sync term. The eye-pair measurement showed the two
        // eyes' sun positions differ by an 11-degree horizontal offset
        // -- each eye's position is relative to its own optical axis,
        // frustum asymmetry baked in -- so the flare slide computes
        // differently per eye, and that disagreement IS the depth
        // artifact. Each eye shifts by half the difference toward the
        // pair mean: anti-symmetric, so the lateral position stays
        // stock and only the disparity collapses. The gain is tuned in
        // the field because the corner-to-screen scale is the
        // instance's secret; the correct value is where the tilt dies.
        if (g_recenter != 0.0f && sunAnchored && g_spOtherValid &&
            nowPair - g_spOtherMs < 50 && fabsf(spNow[2]) > 1e-3f) {
            const float dx = (spNow[0] - g_spOther[0]) * 0.5f;
            const float dy = (spNow[1] - g_spOther[1]) * 0.5f;
            tx = -g_recenter * dx / fabsf(spNow[2]);
            ty = -g_recenter * dy / fabsf(spNow[2]);
        }
        memcpy(g_spOther, spNow, sizeof(spNow));
        g_spOtherValid = true;
        g_spOtherMs = nowPair;
    }
    // The per-eye anisotropic compensation. A fixed plane-size quad
    // subtends an angular patch that shrinks with eccentricity --
    // cos-squared radially, cos tangentially -- and each eye carries its
    // own eccentricity because each eye's axis is its own. The 2x2
    // reshapes this eye's quad toward the angular patch of the PAIR MEAN
    // eccentricity: both eyes then subtend the same patch, and the
    // disparity gradient that read as a tilting, breathing disc
    // collapses. Pure geometry from this eye's own sun direction; the
    // strength knob exists in case the game already half-compensates.
    // The foreshortening pre-compensation, referenced to ON-AXIS -- the
    // field description that fixed the reference: at ninety degrees of
    // eccentricity the disc appeared edge-on, because the game draws it
    // flat in the projection plane, tilted away from the line of sight
    // by exactly the eccentricity. Radial sec-squared (the plane's own
    // magnification) times the facing correction; tangential sec. The
    // disc then subtends the same round patch wherever the head points,
    // and the eyes agree automatically because each is exactly
    // corrected. The pair-mean reference this replaces fixed only the
    // small inter-eye ratio and left the mono foreshortening whole.
    // Clamped: past ~76 degrees the quad would grow without bound.
    // Two corrections, decomposed by what the field taught separately:
    // the DIFFERENTIAL term equalizes this eye to the pair mean, always
    // at full strength -- eye agreement is exact geometry, and its
    // absence was the binocular blur -- and the MONO term corrects the
    // flat-plane foreshortening (the edge-on disc), computed from the
    // PAIR-MEAN eccentricity identically for both eyes so it can never
    // reintroduce a mismatch, scaled by the knob because the game bakes
    // an unknown partial correction of its own.
    float m00 = 1, m01 = 0, m11 = 1;
    if (g_eyeshape != 0.0f && sunAnchored) {
        float tOther = eccT;
        if (g_spOtherValid && nowPair - g_spOtherMs < 50) {
            const float pzo = fabsf(g_spOther[2]);
            const float pxyo = sqrtf(g_spOther[0] * g_spOther[0] +
                                     g_spOther[1] * g_spOther[1]);
            if (pzo > 1e-3f) tOther = pxyo / pzo;
        }
        const float tm = 0.5f * (eccT + tOther);
        float sec2e = 1.0f + eccT * eccT;
        float sec2m = 1.0f + tm * tm;
        if (sec2e > 16.0f) sec2e = 16.0f;
        if (sec2m > 16.0f) sec2m = 16.0f;
        const float sec1e = sqrtf(sec2e);
        const float sec1m = sqrtf(sec2m);
        const float srDiff = sec2e / sec2m;
        const float stDiff = sec1e / sec1m;
        const float srMono = 1.0f + g_eyeshape * (sec2m - 1.0f);
        const float stMono = 1.0f + g_eyeshape * (sec1m - 1.0f);
        const float sr = srDiff * srMono;
        const float st = stDiff * stMono;
        if (eccT > 1e-4f) {
            m00 = sr * eccRx * eccRx + st * eccRy * eccRy;
            m01 = (sr - st) * eccRx * eccRy;
            m11 = sr * eccRy * eccRy + st * eccRx * eccRx;
        } else {
            m00 = m11 = 0.5f * (sr + st);
        }
    }
    uint16_t* out = static_cast<uint16_t*>(m.pData);
    memcpy(out, g_corners, kCornerBytes);
    for (uint32_t v = 0; v < kCornerVerts; ++v) {
        const float x = halfToFloat(g_corners[v * 4 + 2]);
        const float y = halfToFloat(g_corners[v * 4 + 3]);
        const float xr = c * x - s * y;
        const float yr = s * x + c * y;
        out[v * 4 + 2] = floatToHalf(m00 * xr + m01 * yr + tx);
        out[v * 4 + 3] = floatToHalf(m01 * xr + m11 * yr + ty);
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
                        "last note, angle %.1f deg (%s, sun dist %.3g; "
                        "centre formula would say %.1f).",
                        static_cast<unsigned long long>(g_applied -
                                                        g_appliedAtNote),
                        atan2f(s, c) * 57.2958f,
                        sunAnchored ? "sun-anchored" : "centre fallback",
                        candDist, -atan2f(wuv[0], wuv[1]) * 57.2958f);
        g_lastNoteMs = now;
        g_appliedAtNote = g_applied;
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
        return;
    }
    if (!g_engaged) return;
    ctx->IASetVertexBuffers(0, 1, &g_savedVb, &g_savedStride, &g_savedOffset);
    if (g_savedVb) {
        g_savedVb->Release();
        g_savedVb = nullptr;
    }
    g_engaged = false;
}

}  // namespace edvr
