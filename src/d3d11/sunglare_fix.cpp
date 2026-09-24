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
#include "sunglare_vs.h"

namespace edvr {

// sunglareProbeActive and sunglareWorldActive read these from the header
// with no call: sunglareWorldActive in particular is asked per draw, and the
// build has no /GL to fold a cross-TU getter.
namespace detail {
bool g_sunglareProbe = false;
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

// The shader swap. Compiled once per session through d3dcompiler_47
// (present on every Windows 10/11); any failure logs once and stands
// the swap down for the session -- the game then draws stock, which is
// the house's failure posture everywhere.
// fix.sun_glare_world selects a compiled VARIANT, so in-shader
// bisection happens live from the ini without a rebuild: 1 = normal,
// 2 = visibility gate bypassed, 3 = every element world-anchored,
// 4 = every element on the ported flat path.
constexpr int kWorldVariants = 4;
ID3D11VertexShader*  g_worldVs[kWorldVariants] = {};   // owned, lazy
bool                 g_worldTried[kWorldVariants] = {};
ID3D11VertexShader*  g_savedVs = nullptr;  // the game's, across one draw
bool                 g_worldEngaged = false;
uint64_t             g_worldDraws = 0;
uint64_t             g_worldDrawsAtNote = 0;
uint64_t             g_worldNoteMs = 0;
float                g_worldEccMin = 1e9f;
float                g_worldEccMax = -1e9f;

// The true head-tracked camera POSE (3x4 rows from scene-block offset
// 932), kept for the alignment telemetry -- the reading that finally
// closed the case: the glare CB's rows follow the head COMPLETELY
// (align 1.0, cf identical to tf through a full sweep). There is no
// camera clamp in the constants, so no calibrated projection to hold;
// what goes stale past the clamp is the game's CPU-computed element
// position, and that is fixed per draw from the rows alone.
float                g_trueView[12] = {};
bool                 g_trueViewValid = false;
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
float                g_blockCf[3] = {};   // camera forward at the last
                                          // shot; the next fires on a
                                          // real head turn, not a timer
// The instance-stream discovery dump: the adversarial review showed the
// disappearance line could live in ANY of the per-instance attributes
// (position v1, the size chain, or the alpha at t4.x) and none has ever
// been observed directly. Every two seconds, the first bytes of every
// bound IA slot go to the log; the death moment ends up bracketed by
// dumps and the stale attribute names itself offline.
ID3D11Buffer*        g_streamStaging = nullptr;
int                  g_streamDumps = 0;      // idle-tick budget spent
int                  g_streamTurnDumps = 0;  // head-turn budget spent
uint64_t             g_streamDumpMs = 0;
float                g_streamCf[3] = {};     // camera forward at the last
                                             // dump; the 2026-08-22 pass
                                             // burned its whole timer
                                             // budget before the sweeps
                                             // began -- the death is
                                             // head-angle-driven, so the
                                             // trigger must be too
constexpr uint32_t   kStreamDumpBytes = 2688;   // 21 records of 128B --
                                                // enough to classify a
                                                // whole i20 train
// This draw's DrawInstanced window (set by the thunk) and the
// per-second roster of distinct windows. The record buffer multiplexes
// several trains at different instance offsets: the buffer HEAD the
// first dumps read was whichever train wrote last, and the visible
// disc's own records were never certainly sampled. The roster names
// every window per second -- a train vanishing from it at the death
// line is the culprit naming itself.
uint32_t             g_drawInst = 0;
uint32_t             g_drawStart = 0;
struct TrainSlot { uint32_t start; uint32_t inst; uint32_t hits; };
TrainSlot            g_trains[8] = {};
int                  g_trainCount = 0;
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

// The instance-stream discovery dump. Blocking Map straight after the
// copy -- a deliberate pipeline sync, acceptable at half a hertz on a
// diagnostic budget of thirty. Everything the glare draw feeds its
// vertex shader flows through these slots; the constants have been
// interrogated for weeks while the streams were never once looked at.
void dumpInstanceStreams(ID3D11DeviceContext* ctx) {
    if (!g_streamStaging) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kStreamDumpBytes;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            dev->CreateBuffer(&bd, nullptr, &g_streamStaging);
            dev->Release();
        }
        if (!g_streamStaging) return;
    }
    for (UINT slot = 0; slot < 8; ++slot) {
        ID3D11Buffer* vb = nullptr;
        UINT stride = 0, off = 0;
        ctx->IAGetVertexBuffers(slot, 1, &vb, &stride, &off);
        if (!vb) continue;
        D3D11_BUFFER_DESC bd{};
        vb->GetDesc(&bd);
        // Per-instance streams (big strides) are read at THIS DRAW's
        // instance window, not the buffer head -- the trains share one
        // buffer at different offsets, and the head is whichever train
        // wrote last. Per-vertex streams (the 8-byte corners) have no
        // instance window; they stay at zero.
        uint32_t base = 0;
        if (stride >= 64) {
            base = off + g_drawStart * stride;
            if (base >= bd.ByteWidth) base = 0;
        }
        uint32_t n = bd.ByteWidth - base;
        if (n > kStreamDumpBytes) n = kStreamDumpBytes;
        D3D11_BOX box{base, 0, 0, base + n, 1, 1};
        ctx->CopySubresourceRegion(g_streamStaging, 0, 0, 0, 0, vb, 0, &box);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(g_streamStaging, 0, D3D11_MAP_READ, 0, &m)) &&
            m.pData) {
            const float* f = static_cast<const float*>(m.pData);
            const int count = static_cast<int>(n / 4);
            char line[640];
            int o = 0;
            for (int k = 0; k < count && k < 48 && o < 600; ++k)
                o += snprintf(line + o, sizeof(line) - o, "%s%.4g",
                              k ? " " : "", f[k]);
            Log::get().note("glare stream slot %u stride=%u off=%u bytes=%u"
                            " draw s=%u i=%u base=%u f0..: %s",
                            slot, stride, off, bd.ByteWidth, g_drawStart,
                            g_drawInst, base, line);
            if (count > 48) {
                o = 0;
                for (int k = 48; k < count && k < 96 && o < 600; ++k)
                    o += snprintf(line + o, sizeof(line) - o, "%s%.4g",
                                  k > 48 ? " " : "", f[k]);
                Log::get().note("glare stream slot %u f48..: %s", slot,
                                line);
            }
            // The whole-train classification, one line: per record, the
            // selection class -- 'a' anchored, 'x' axis-locked (beam),
            // 's' slider -- plus tile x, the slide length (t7.z at
            // f29, the routing input itself) and the base-size source
            // p1.z (f6): every quantity the vivid routing rides, per
            // record, straight from the data the draw consumed.
            if (stride >= 64) {
                const int recs = static_cast<int>(n / 128);
                o = 0;
                for (int k = 0; k < recs && k < 21 && o < 600; ++k) {
                    const float* r = f + k * 32;
                    const bool anch = r[12] > 0.999f && r[13] > 0.999f;
                    const bool sld = r[6] > 0.001f || r[7] > 0.001f;
                    const bool bar = r[4] > 4.0f * r[5] ||
                                     r[5] > 4.0f * r[4];
                    // Mirrors the shader's routing: 'x' axis-locked
                    // and 'b' bar-shaped (world-pinned), 's' sliders
                    // (flat), 'a' anchored (world), 'w' weights-slider
                    // (flat). Fields: tile / t7.z / p1.z / aspect.
                    const char c = r[19] > 0.0f ? 'x'
                                   : (bar ? 'b'
                                          : (sld ? 's'
                                                 : (anch ? 'a' : 'w')));
                    const float asp =
                        r[5] > 1e-6f ? r[4] / r[5] : 0.0f;
                    o += snprintf(line + o, sizeof(line) - o,
                                  "%s%c%.0f/%.2g/%.2g/%.2g",
                                  k ? " " : "", c, r[20], r[29], r[6],
                                  asp);
                }
                if (recs > 0)
                    Log::get().note("glare stream classes: %s", line);
            }
            ctx->Unmap(g_streamStaging, 0);
        }
        vb->Release();
    }
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
    // Both modes ride the world-anchored shader; the variant is a live
    // in-shader diagnostic override kept from the debug arc (2 = gate
    // bypassed, 3 = all elements world-anchored, 4 = all flat).
    const int variant =
        cfg.getIntInRange("advanced.sun_glare_variant", 0, 0, kWorldVariants);
    const int wasWorld = detail::g_sunglareWorld;
    detail::g_sunglareWorld = (g_mode == Mode::kRealistic || g_mode == Mode::kVivid)
                  ? (variant ? variant : 1)
                  : 0;
    // The debug instruments (per-second telemetry, b0 identity line,
    // camera-block census, instance-stream dumps) behind one switch,
    // default silent: a shipped log should carry findings, not vitals.
    detail::g_sunglareProbe = cfg.getBool("advanced.sun_glare_probe", false);
    // The billboard loan's tee stays armed for world mode and for the
    // probe: the telemetry and the per-draw camera solve read the
    // shadowed CB.
    billboardGlareWatch(detail::g_sunglareWorld != 0 || detail::g_sunglareProbe);
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
    return g_mode != Mode::kStock || exposureDampingActive() || detail::g_sunglareProbe;
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
    if (!detail::g_sunglareProbe || !detail::g_sunglareWorld || g_camDumpShot >= 2 || g_lastSeenMs == 0 ||
        !data ||
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

void sunglareDrawArgs(uint32_t instances, uint32_t startInstance) {
    g_drawInst = instances;
    g_drawStart = startInstance;
}

void sunglareBegin(ID3D11DeviceContext* ctx) {
    g_worldEngaged = false;
    if (!ctx) return;

    // With the world shader in, position, orientation, facing and per-eye
    // agreement are all computed correctly inside the pipeline, and the
    // corner stream stays the game's own. The probe instruments run in
    // EVERY mode -- including stock, where the draws go untouched -- so a
    // stock-vs-mode record diff is one hot swap apart.
    if (detail::g_sunglareWorld || detail::g_sunglareProbe) {
        // The cutoff telemetry: matched draws per second and the
        // eccentricity range the CB reports, so a disappearance names
        // its side -- draws stopping = the game culled upstream; draws
        // continuing = our shader killed them.
        ++g_worldDraws;
        if (detail::g_sunglareProbe) {
            bool found = false;
            for (int i = 0; i < g_trainCount; ++i) {
                if (g_trains[i].start == g_drawStart &&
                    g_trains[i].inst == g_drawInst) {
                    ++g_trains[i].hits;
                    found = true;
                    break;
                }
            }
            if (!found && g_trainCount < 8) {
                g_trains[g_trainCount] = {g_drawStart, g_drawInst, 1};
                ++g_trainCount;
            }
        }
        if (detail::g_sunglareProbe) {
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
                // The SIGNED projected origin -- (w4/w7, w5/w7) -- so
                // "pinned vs tracking under head motion" is measured,
                // not inferred from the sign-blind eccentricity
                // aggregate that misled the clamp arc. And the shadow's
                // age: a stale shadow disproves buffer identity on its
                // own.
                float onx = 0.0f, ony = 0.0f;
                if (sh && nf >= 32 && fabsf(sh[31]) > 1e-4f) {
                    onx = sh[19] / sh[31];
                    ony = sh[23] / sh[31];
                }
                const uint64_t age = billboardShadowAgeMs();
                Log::get().note("glare world: %llu draw(s)/s, ecc tan "
                                "%.2f..%.2f, align %.4f S%s d=%.1f "
                                "org=(%.2f %.2f) age=%llu cf=(%.2f %.2f "
                                "%.2f) tf=(%.2f %.2f %.2f).",
                                static_cast<unsigned long long>(
                                    g_worldDraws - g_worldDrawsAtNote),
                                g_worldEccMin, g_worldEccMax, align,
                                g_sunSolveOk ? "OK" : "--", g_camDist,
                                onx, ony,
                                static_cast<unsigned long long>(age),
                                cf[0], cf[1], cf[2], tf[0], tf[1], tf[2]);
                g_worldNoteMs = now;
                g_worldDrawsAtNote = g_worldDraws;
                g_worldEccMin = 1e9f;
                g_worldEccMax = -1e9f;

                // The train roster: every distinct DrawInstanced
                // (start, count) window seen this second, with hits.
                {
                    char tr[200];
                    int to = 0;
                    for (int i = 0; i < g_trainCount && to < 160; ++i)
                        to += snprintf(tr + to, sizeof(tr) - to,
                                       "%s[s%u i%u x%u]", i ? " " : "",
                                       g_trains[i].start, g_trains[i].inst,
                                       g_trains[i].hits);
                    Log::get().note("glare trains: %s",
                                    g_trainCount ? tr : "(none)");
                    g_trainCount = 0;
                }

                // THE BUFFER IDENTITY CHECK -- the adversarial review's
                // softest joint. Every CPU-side conclusion in this arc
                // rides the assumption that the tee'd shadow is the very
                // buffer bound at b0 of this draw. The mirror only sees
                // plain VSSetConstantBuffers; the runtime's own answer
                // is the authority. A MISMATCH here voids the honest-
                // rows reading and restores the clamp theory whole.
                {
                    ID3D11Buffer* rb0 = nullptr;
                    ctx->VSGetConstantBuffers(0, 1, &rb0);
                    D3D11_BUFFER_DESC b0d{};
                    if (rb0) rb0->GetDesc(&b0d);
                    // Viewport and scissor beside it: with the constants
                    // and (soon) the streams proven honest, raster state
                    // is the one remaining door a clean disappearance
                    // line could walk through.
                    UINT nvp = 1;
                    D3D11_VIEWPORT vp{};
                    ctx->RSGetViewports(&nvp, &vp);
                    UINT nsc = 1;
                    D3D11_RECT sc{};
                    ctx->RSGetScissorRects(&nsc, &sc);
                    Log::get().note(
                        "glare b0 identity: runtime=%p mirror=%p "
                        "b0_bytes=%u usage=%u shadow_floats=%u %s "
                        "vp=(%.0f %.0f %.0fx%.0f) sc=(%ld %ld %ld %ld)",
                        static_cast<void*>(rb0), billboardTarget(),
                        b0d.ByteWidth, static_cast<unsigned>(b0d.Usage),
                        nf,
                        static_cast<void*>(rb0) == billboardTarget()
                            ? "MATCH"
                            : "MISMATCH",
                        vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height,
                        sc.left, sc.top, sc.right, sc.bottom);
                    if (rb0) rb0->Release();
                }

                // The camera-block census: every float the shader does
                // NOT read, plus the solved camera. A unit triplet at
                // the sun's angle is a direction; a triplet that lands
                // on the sun after subtracting cam is a position;
                // anything that moves with the head between shots is
                // view-space. Shots fire on an actual HEAD TURN (~11
                // degrees since the last), not a timer -- the review
                // showed a timer burns the budget while the headset
                // sits on the desk.
                bool turned = align > -1.5f;
                if (turned && g_blockShots > 0) {
                    const float dp = cf[0] * g_blockCf[0] +
                                     cf[1] * g_blockCf[1] +
                                     cf[2] * g_blockCf[2];
                    turned = dp < 0.98f;
                }
                if (sh && nf >= 52 && g_blockShots < 8 && turned &&
                    now - g_blockShotMs >= 2000) {
                    ++g_blockShots;
                    g_blockShotMs = now;
                    g_blockCf[0] = cf[0];
                    g_blockCf[1] = cf[1];
                    g_blockCf[2] = cf[2];
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

            // The instance-stream dump triggers, PER DRAW: a slow idle
            // tick, plus a HEAD-TURN trigger (~3 degrees since the last
            // dump, at most ~2.5Hz). The first pass burned its whole
            // timer budget before the sweeps began -- the death line is
            // crossed by turning the head, so the coverage follows the
            // turn. A sweep through the line now yields a dump every
            // few degrees, bracketing whichever attribute dies.
            {
                bool fire = false;
                if (g_streamDumps < 10 && now - g_streamDumpMs >= 2000) {
                    ++g_streamDumps;
                    fire = true;
                }
                if (!fire && g_streamTurnDumps < 60 &&
                    now - g_streamDumpMs >= 400 && sh && nf >= 32) {
                    const float* fc = sh + 28;
                    const float l = sqrtf(fc[0] * fc[0] + fc[1] * fc[1] +
                                          fc[2] * fc[2]);
                    if (l > 1e-4f) {
                        const float dp =
                            (fc[0] * g_streamCf[0] + fc[1] * g_streamCf[1] +
                             fc[2] * g_streamCf[2]) /
                            l;
                        if (dp < 0.9986f) {
                            ++g_streamTurnDumps;
                            fire = true;
                        }
                    }
                }
                if (fire) {
                    g_streamDumpMs = now;
                    if (sh && nf >= 32) {
                        const float* fc = sh + 28;
                        const float l = sqrtf(fc[0] * fc[0] +
                                              fc[1] * fc[1] +
                                              fc[2] * fc[2]);
                        if (l > 1e-4f) {
                            g_streamCf[0] = fc[0] / l;
                            g_streamCf[1] = fc[1] / l;
                            g_streamCf[2] = fc[2] / l;
                        }
                    }
                    dumpInstanceStreams(ctx);
                }
            }
        }
        if (!detail::g_sunglareWorld) return;   // probe-only: instruments ran, draw stock
        const int v = detail::g_sunglareWorld - 1;
        if (!g_worldTried[v]) buildWorldShader(ctx, detail::g_sunglareWorld);
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
