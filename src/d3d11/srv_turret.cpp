// The SRV turret view (issue 140): the module. srv_turret.h says what it does and what the log shows; srv_turret_math.h has the maths, the two families and the two TEMPORARY
// keys; srv_turret_hud_binding.h holds the constants and the binding, which tools/srv_turret_hud_test runs on WARP; srv_turret_hud_shader.h is the geometry shaders' HLSL,
// compiled at build time.
#include "srv_turret.h"

#include "srv_turret_hud_binding.h"
#include "srv_turret_math.h"

#include "draw_gate.h"
#include "journal_watch.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"
#include "vr_camera_census.h"
#include "vscreen.h"   // vScreenCopyResourceRaw: the instrument's staging copy goes below the hooks

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"

#include <windows.h>

#include <d3d11.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace edvr {

namespace detail {
std::atomic<bool> g_srvTurretHudWanted{false};
std::atomic<bool> g_srvTurretHudCounting{false};
std::atomic<uint32_t> g_srvTurretHudIssues{0};
}  // namespace detail

namespace st = srv_turret;

namespace {

static_assert(st::kBackDefaultCm == 60 && st::kBackMaxCm == 150 && st::kGapDefaultCm == 96 && st::kGapMaxCm == 200,
              "the literals in the two reads below are these, written as numbers for tools/gen_settings_schema.py");
constexpr uint32_t kFamilies = st::kFamilyCount;

// ---- the frame thread's state ----------------------------------------------------------------------------------------------------------------------------------
bool g_keysRead = false;
uint32_t g_keysGeneration = 0;
std::atomic<int> g_backCm{0}, g_gapCm{0};   // TEMPORARY keys, as last read (the gap is read by the draw path too)
bool g_on = false;                          // the turret view, as the frame tick last saw it
double g_backSeatedZ = 0.0;                 // what alignTick adds
double g_loggedBackM = 0.0;                 // what the log last said was published
bool g_windowSeen = false;                  // the turret view was on at some point of the 10 s window
uint64_t g_windowStartMs = 0;

// The geometry the per-eye shifts are made from, refreshed once a frame while the pass is wanted: Elite's own tangents and eye separation as the host gave them.
// Written by the frame tick, read by the draw path: atomics, the validity word last.
struct EyeGeometry {
    std::atomic<float> p00{0.0f}, s{0.0f};
    std::atomic<bool> ok{false};
};
EyeGeometry g_eyeGeo[2];
std::atomic<float> g_ipd{0.0f};

// ---- the draw path's state (the render thread; the window's counters are read by the frame tick) -------------------------------------------------------------
// The build's bytecode of each family's geometry shader, and the stream-output declaration of its signature (the instrument's twin streams the vertex out in this order).
struct FamilyShader {
    const unsigned char* bytecode;
    size_t size;
    const char* name;
    D3D11_SO_DECLARATION_ENTRY decl[3];
};
const FamilyShader kShader[kFamilies] = {
    {kSrvTurretHudGsABytecode, sizeof(kSrvTurretHudGsABytecode), "turret shift 1989E6D3",
     {{0, "TEXCOORD", 0, 0, 4, 0}, {0, "TEXCOORD", 6, 0, 3, 0}, {0, "SV_Position", 0, 0, 4, 0}}},
    {kSrvTurretHudGsBBytecode, sizeof(kSrvTurretHudGsBBytecode), "turret shift 2CECEC30",
     {{0, "TEXCOORD", 0, 0, 4, 0}, {0, "TEXCOORD", 7, 0, 2, 0}, {0, "SV_Position", 0, 0, 4, 0}}},
};
ID3D11GeometryShader* g_gs[kFamilies] = {};
ID3D11Device* g_gsDevice = nullptr;   // identity only: the shaders hold their device alive
bool g_gsTried = false;
st::Constants g_constants[2];
st::Binding g_binding;
std::atomic<uint32_t> g_draws[2] = {};
std::atomic<uint32_t> g_familyDraws[kFamilies] = {};
std::atomic<uint32_t> g_bound{0};
std::atomic<uint32_t> g_refused[static_cast<size_t>(st::Refuse::kCount)] = {};
uint32_t g_saidRefuse = 0;
bool g_firstNoted[kFamilies] = {}, g_restoreNoted = false, g_settleNoted = false;
uint32_t g_frame = 0;

void refuse(st::Refuse r) {
    g_refused[static_cast<size_t>(r)].fetch_add(1, std::memory_order_relaxed);
    const uint32_t bit = 1u << static_cast<unsigned>(r);
    if (g_saidRefuse & bit) return;
    g_saidRefuse |= bit;
    Log::get().note("srv turret hud: refused a draw (%s) -- the draw went on as the game made it. Said once per reason; the 10 s line counts every one.", st::refuseName(r));
}

// The instrument's stream-output twin of each family's shader (the same bytecode; it also streams the vertices it writes out), made with the shaders and released with them.
ID3D11GeometryShader* g_soGs[kFamilies] = {};
bool g_soTried = false;

void releaseGs() {
    for (uint32_t f = 0; f < kFamilies; ++f) {
        st::release(g_gs[f]);
        st::release(g_soGs[f]);
    }
    g_soTried = false;
    g_gsDevice = nullptr;
}

// The twins: stream 0 is both streamed out (every signature element in order, st::Family::strideBytes a vertex) and rasterized, so a draw issued through one looks exactly as
// one through the shader itself. A failure is said once and leaves the instrument to its statistics alone.
void makeTwins(ID3D11DeviceContext* ctx) {
    g_soTried = true;
    for (uint32_t f = 0; f < kFamilies; ++f) {
        ID3D11Device* d = nullptr;
        HRESULT hr = E_FAIL;
        const bool ran = guarded("srv.turret.twin", [&] {
            ctx->GetDevice(&d);
            if (!d) return;
            const UINT stride = st::kFamilies[f].strideBytes;
            hr = d->CreateGeometryShaderWithStreamOutput(kShader[f].bytecode, kShader[f].size, kShader[f].decl, 3, &stride, 1, 0, nullptr, &g_soGs[f]);
        });
        st::release(d);
        if (!ran || FAILED(hr)) st::release(g_soGs[f]);
        Log::get().note("srv turret hud: stream-output twin of the %s shader (the GPU instrument's capture) %s (0x%08X); %s", st::kFamilies[f].name,
                        g_soGs[f] ? "created" : "NOT made", static_cast<unsigned>(hr),
                        g_soGs[f] ? "one draw a second per eye is issued through it" : "the instrument keeps to its statistics");
    }
}

// The shaders, made from the build's bytecode once per device (a device that comes and goes re-makes them; a failure is not retried on the same device).
// shaderSwapCreateGs is guarded and says each outcome in the log. True when every family's shader is there: a draw is never bound for a family whose shader is missing.
bool ensureGs(ID3D11DeviceContext* ctx) {
    ID3D11Device* d = nullptr;
    if (!guarded("srv.turret.device", [&] { ctx->GetDevice(&d); }) || !d) {
        st::release(d);
        return false;
    }
    if (d != g_gsDevice) {
        releaseGs();
        g_gsDevice = d;
        g_gsTried = false;
    }
    st::release(d);
    if (!g_gsTried) {
        g_gsTried = true;
        for (uint32_t f = 0; f < kFamilies; ++f) g_gs[f] = shaderSwapCreateGs(ctx, kShader[f].bytecode, kShader[f].size, kShader[f].name, "srv turret hud");
    }
    bool all = true;
    for (uint32_t f = 0; f < kFamilies; ++f) all = all && g_gs[f] != nullptr;
    if (all && !g_soTried) makeTwins(ctx);
    return all;
}

// The two keys, read when the configuration changes (Config::generation()) and not on every frame; said in the log at the first read and at each change.
void readKeys() {
    Config& cfg = Config::get();
    const uint32_t generation = cfg.generation();
    if (g_keysRead && generation == g_keysGeneration) return;
    const int back = cfg.getIntInRange("fix.srv_turret_camera_back", 60, 0, 150);
    const int gap = cfg.getIntInRange("fix.srv_turret_hud_gap", 96, 0, 200);
    const bool changed = !g_keysRead || back != g_backCm.load(std::memory_order_relaxed) || gap != g_gapCm.load(std::memory_order_relaxed);
    g_keysRead = true;
    g_keysGeneration = generation;
    g_backCm.store(back, std::memory_order_relaxed);
    g_gapCm.store(gap, std::memory_order_relaxed);
    if (changed)
        Log::get().note("srv turret settings: camera back %d cm, hud gap %d cm (live; fix.srv_turret_camera_back and fix.srv_turret_hud_gap, TEMPORARY; 0 is the game's own)", back, gap);
}

// Elite's tangents and eye separation as the host gave this frame (native_temporal.cpp): P00 of each eye and the shift s = P00 hx. One eye missing leaves it not ok.
void cacheGeometry() {
    float ipd = 0.0f;
    const bool haveIpd = nativeTemporalEyeSeparation(&ipd) && st::ipdUsable(ipd);
    g_ipd.store(haveIpd ? ipd : 0.0f, std::memory_order_relaxed);
    for (uint32_t e = 0; e < 2; ++e) {
        EyeGeometry& g = g_eyeGeo[e];
        float tan[4] = {};
        uint64_t sequence = 0;
        const bool haveTan = nativeTemporalEyeGeometry(e, &sequence, tan, nullptr);
        const float p00 = haveTan ? st::p00OfTangents(tan) : 0.0f;
        const bool ok = haveIpd && st::p00Usable(p00);
        g.ok.store(false, std::memory_order_release);
        g.p00.store(ok ? p00 : 0.0f, std::memory_order_relaxed);
        g.s.store(ok ? p00 * st::eyeHx(static_cast<int>(e), ipd) : 0.0f, std::memory_order_relaxed);
        g.ok.store(ok, std::memory_order_release);
    }
}

void windowLine(uint64_t nowMs) {
    const float gap = static_cast<float>(g_gapCm.load(std::memory_order_relaxed)) / 100.0f;
    const st::TurretParams p = st::paramsFor(0.0f, gap);
    uint32_t refused[static_cast<size_t>(st::Refuse::kCount)];
    for (size_t i = 0; i < static_cast<size_t>(st::Refuse::kCount); ++i) refused[i] = g_refused[i].exchange(0, std::memory_order_relaxed);
    const uint32_t left = g_draws[0].exchange(0, std::memory_order_relaxed), right = g_draws[1].exchange(0, std::memory_order_relaxed);
    const uint32_t bound = g_bound.exchange(0, std::memory_order_relaxed);
    const uint32_t famA = g_familyDraws[0].exchange(0, std::memory_order_relaxed), famB = g_familyDraws[1].exchange(0, std::memory_order_relaxed);
    (void)nowMs;
    Log::get().note("srv turret hud: gap=%.2f m wMin=%.2f wMax=%.2f s[L,R]=%+.4f,%+.4f ipd_elite=%.1fmm P00[L,R]=%.3f,%.3f draws[L,R]=%u,%u bound=%u "
                    "refused[no-eye,game-gs,topology,not-ready,no-geometry]=%u/%u/%u/%u/%u families[%s,%s]=%u,%u",
                    static_cast<double>(gap), static_cast<double>(p.wMin), static_cast<double>(p.wMax),
                    static_cast<double>(g_eyeGeo[0].s.load(std::memory_order_relaxed)), static_cast<double>(g_eyeGeo[1].s.load(std::memory_order_relaxed)),
                    1000.0 * static_cast<double>(g_ipd.load(std::memory_order_relaxed)),
                    static_cast<double>(g_eyeGeo[0].p00.load(std::memory_order_relaxed)), static_cast<double>(g_eyeGeo[1].p00.load(std::memory_order_relaxed)),
                    left, right, bound, refused[0], refused[1], refused[2], refused[3], refused[4], st::kFamilies[0].name, st::kFamilies[1].name, famA, famB);
}

// ---- THE GPU INSTRUMENT ------------------------------------------------------------------------------------------------------------------------------------------------
// The flight of 2026-10-30 (Steam, d662d26c) bound an earlier build of this pass for every matched draw (bound = draws, no refusals) and the panels did not move; that build
// targeted the reticle's sprite, whose clip w is about 500 m. From the CPU there is no way to tell a shader that never ran from one that ran on vertices outside its window, so:
// once a second per eye and family, one matched draw (the ordinal of the family's draws in the frame rotates, so the near layer's draws and the deep panels' are all measured
// within a few seconds) is issued through the stream-output twin with a pipeline-statistics query and a stream-output-statistics query around the scope, Begin right after the
// bind, End right before the restore. gs=0 with ia_prims above 0 is a shader that never ran; the captured vertices say where the shader found each draw's clip w and what it did
// to the clip x: the near layer's draws read in_window 0 at about 0.4 m, the deep panels' in_window above 0 at about 1.3 to 1.4 m. Read back without stalling.
constexpr uint32_t kProbeEveryMs = 1000, kProbeGiveUpFrames = 8, kProbeOrdinals = 8;

struct Probe {
    ID3D11Device* device = nullptr;   // identity only: the queries hold their device alive
    ID3D11Query* stats = nullptr;
    ID3D11Query* soStats = nullptr;
    ID3D11Buffer* soBuf = nullptr;
    ID3D11Buffer* staging = nullptr;
    bool failed = false;              // creation failed on this device: not retried
    bool inFlight = false;            // End issued, the results not all read
    bool statsLogged = false, captureLogged = false;
    uint32_t waited = 0, lost = 0;    // frames since End; samples given up on (cumulative)
    uint32_t issues = 0, draw = 0;    // draw calls issued inside the sampled scope; the sampled draw's ordinal in its frame
    uint32_t ordinal = 0, wantOrd = 0;
    uint64_t lastMs = 0;
    st::TurretParams params;          // the constants the sampled draw ran with
};
Probe g_probe[2][kFamilies];          // by eye and family
Probe* g_sample = nullptr;            // the probe of the draw whose scope is open and sampled
int g_sampleEye = -1, g_sampleFamily = -1;
bool g_sampleTwin = false;            // ... and whether it was issued through the twin with its stream-output target
bool g_soBusyNoted = false;

void probeReleaseQueries(Probe& p) {
    st::release(p.stats);
    st::release(p.soStats);
}
void probeReset(Probe& p) {
    probeReleaseQueries(p);
    st::release(p.soBuf);
    st::release(p.staging);
    const uint32_t lost = p.lost, ordinal = p.ordinal;
    p = Probe{};
    p.lost = lost;
    p.ordinal = ordinal;
}

// The probe's objects for this device, made once (the queries again after a sample was given up on).
bool probeEnsure(ID3D11DeviceContext* ctx, int eye, int family) {
    Probe& p = g_probe[eye][family];
    ID3D11Device* d = nullptr;
    if (!guarded("srv.turret.probe.device", [&] { ctx->GetDevice(&d); }) || !d) {
        st::release(d);
        return false;
    }
    if (d != p.device) {
        probeReset(p);
        p.device = d;
    }
    if (p.failed) {
        st::release(d);
        return false;
    }
    HRESULT hr = S_OK;
    const char* what = nullptr;
    const bool twinMade = g_soGs[family] != nullptr;
    const bool ran = guarded("srv.turret.probe.make", [&] {
        if (!p.stats) {
            const D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS, 0};
            hr = d->CreateQuery(&qd, &p.stats);
            if (FAILED(hr)) { what = "the pipeline-statistics query"; return; }
        }
        if (twinMade && !p.soStats) {
            const D3D11_QUERY_DESC qd{D3D11_QUERY_SO_STATISTICS, 0};
            hr = d->CreateQuery(&qd, &p.soStats);
            if (FAILED(hr)) { what = "the stream-output-statistics query"; return; }
        }
        if (twinMade && !p.soBuf) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = st::kCaptureBufferBytes;
            bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
            hr = d->CreateBuffer(&bd, nullptr, &p.soBuf);
            if (FAILED(hr)) { what = "the stream-output buffer"; return; }
        }
        if (twinMade && !p.staging) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = st::kCaptureBufferBytes;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = d->CreateBuffer(&bd, nullptr, &p.staging);
            if (FAILED(hr)) { what = "the staging buffer"; return; }
        }
    });
    st::release(d);
    if (!ran || what) {
        Log::get().note("srv turret hud: the GPU instrument could not be made for eye %d, family %s (%s, 0x%08X); the pass itself is unaffected.", eye, st::kFamilies[family].name,
                        what ? what : "a fault", static_cast<unsigned>(hr));
        probeReleaseQueries(p);
        st::release(p.soBuf);
        st::release(p.staging);
        p.failed = true;   // not retried on this device
        return false;
    }
    return true;
}

// The sample is due: this eye's and family's matched draw of the wanted ordinal, a second since the last, nothing in flight, the objects made, and no stream-output target of
// the game's bound.
bool probeDue(ID3D11DeviceContext* ctx, int eye, int family, uint32_t ordinal) {
    Probe& p = g_probe[eye][family];
    if (p.inFlight || ordinal != p.wantOrd) return false;
    if (GetTickCount64() - p.lastMs < kProbeEveryMs) return false;
    if (!probeEnsure(ctx, eye, family)) return false;
    ID3D11Buffer* so[1] = {};
    const bool read = guarded("srv.turret.probe.so", [&] { ctx->SOGetTargets(1, so); });
    const bool busy = !read || so[0] != nullptr;
    st::release(so[0]);
    if (busy) {
        if (!g_soBusyNoted) {
            g_soBusyNoted = true;
            Log::get().note("srv turret hud: the GPU instrument skips a draw: the game has a stream-output target bound (said once).");
        }
        return false;
    }
    return true;
}

// Right after the bind: the stream-output target, the queries, the issue count.
void probeBegin(ID3D11DeviceContext* ctx, int eye, int family, uint32_t ordinal, bool twin, const st::TurretParams& params) {
    Probe& p = g_probe[eye][family];
    p.params = params;
    p.draw = ordinal;
    p.issues = 0;
    g_sample = &p;
    g_sampleEye = eye;
    g_sampleFamily = family;
    g_sampleTwin = twin;
    detail::g_srvTurretHudIssues.store(0, std::memory_order_relaxed);
    detail::g_srvTurretHudCounting.store(true, std::memory_order_relaxed);
    if (!guarded("srv.turret.probe.begin", [&] {
            if (twin) {
                ID3D11Buffer* target = p.soBuf;
                const UINT offset = 0;
                ctx->SOSetTargets(1, &target, &offset);
            }
            ctx->Begin(p.stats);
            if (twin) ctx->Begin(p.soStats);
        })) {
        p.failed = true;   // a fault in the instrument's own calls ends the instrument, not the pass
    }
}

// Right before the restore: the queries end, the stream-output target comes off.
void probeEnd(ID3D11DeviceContext* ctx) {
    if (!g_sample) return;
    Probe& p = *g_sample;
    detail::g_srvTurretHudCounting.store(false, std::memory_order_relaxed);
    p.issues = detail::g_srvTurretHudIssues.load(std::memory_order_relaxed);
    if (!guarded("srv.turret.probe.end", [&] {
            ctx->End(p.stats);
            if (g_sampleTwin) ctx->End(p.soStats);
            if (g_sampleTwin) ctx->SOSetTargets(0, nullptr, nullptr);
        })) {
        p.failed = true;
    }
}

// After the restore: the captured records are copied for the later read, and the sample is in flight.
void probeFinish(ID3D11DeviceContext* ctx) {
    if (!g_sample) return;
    Probe& p = *g_sample;
    const bool twin = g_sampleTwin;
    g_sample = nullptr;
    g_sampleEye = g_sampleFamily = -1;
    g_sampleTwin = false;
    if (p.failed) return;
    if (twin && !guarded("srv.turret.probe.copy", [&] { vScreenCopyResourceRaw(ctx, p.staging, p.soBuf); })) {
        p.failed = true;
        return;
    }
    p.inFlight = true;
    p.waited = 0;
    p.statsLogged = false;
    p.captureLogged = !twin;
    p.lastMs = GetTickCount64();
    p.wantOrd = (p.wantOrd + 1) % kProbeOrdinals;
}

// Once a frame: read what the GPU has finished, without waiting for it.
void probeRead(ID3D11DeviceContext* ctx, int eye, int family) {
    Probe& p = g_probe[eye][family];
    if (!p.inFlight || !ctx) return;
    const st::Family& fam = st::kFamilies[family];
    if (!p.statsLogged) {
        D3D11_QUERY_DATA_PIPELINE_STATISTICS s{};
        HRESULT hr = S_FALSE;
        const bool ran = guarded("srv.turret.probe.stats", [&] { hr = ctx->GetData(p.stats, &s, sizeof(s), D3D11_ASYNC_GETDATA_DONOTFLUSH); });
        if (!ran || FAILED(hr)) {
            ++p.lost;
            p.inFlight = false;
            probeReleaseQueries(p);
            return;
        }
        if (hr == S_OK) {
            p.statsLogged = true;
            Log::get().note("srv turret hud stats: eye=%d issues=%u ia_prims=%llu vs=%llu gs=%llu gs_prims=%llu c_inv=%llu c_prims=%llu ps=%llu (lost %u) draw=%u family=%s", eye,
                            p.issues, static_cast<unsigned long long>(s.IAPrimitives), static_cast<unsigned long long>(s.VSInvocations),
                            static_cast<unsigned long long>(s.GSInvocations), static_cast<unsigned long long>(s.GSPrimitives), static_cast<unsigned long long>(s.CInvocations),
                            static_cast<unsigned long long>(s.CPrimitives), static_cast<unsigned long long>(s.PSInvocations), p.lost, p.draw, fam.name);
        }
    }
    if (p.statsLogged && !p.captureLogged) {
        D3D11_QUERY_DATA_SO_STATISTICS so{};
        HRESULT hr = S_FALSE;
        const bool ran = guarded("srv.turret.probe.soStats", [&] { hr = ctx->GetData(p.soStats, &so, sizeof(so), D3D11_ASYNC_GETDATA_DONOTFLUSH); });
        if (ran && hr == S_OK) {
            D3D11_MAPPED_SUBRESOURCE m{};
            HRESULT mh = E_FAIL;
            const bool mapped = guarded("srv.turret.probe.map", [&] { mh = ctx->Map(p.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m); });
            if (mapped && SUCCEEDED(mh) && m.pData) {
                const uint64_t written = so.NumPrimitivesWritten;
                const uint32_t maxVertices = st::captureMaxVertices(fam.strideBytes);
                const uint32_t vertices = static_cast<uint32_t>(written > maxVertices / 3 ? maxVertices / 3 : written) * 3;
                const st::CaptureSummary c = st::summarizeCapture(static_cast<const float*>(m.pData), vertices, fam.strideBytes / 4, fam.posFloat, p.params);
                ctx->Unmap(p.staging, 0);
                p.captureLogged = true;
                Log::get().note("srv turret hud capture: family=%s eye=%d draw=%u so_prims=%llu needed=%llu vertices=%u in_window=%u w=[%.3f,%.3f] ndc_x=[%.3f,%.3f] "
                                "ndc_y=[%.3f,%.3f] shift_clip=[%+.4f,%+.4f] s=%+.4f gap=%.2f",
                                fam.name, eye, p.draw, static_cast<unsigned long long>(written), static_cast<unsigned long long>(so.PrimitivesStorageNeeded), c.vertices,
                                c.inWindow, static_cast<double>(c.wMin), static_cast<double>(c.wMax), static_cast<double>(c.ndcXMin), static_cast<double>(c.ndcXMax),
                                static_cast<double>(c.ndcYMin), static_cast<double>(c.ndcYMax), static_cast<double>(c.shiftMin), static_cast<double>(c.shiftMax),
                                static_cast<double>(p.params.s), static_cast<double>(p.params.gap));
            }
        } else if (!ran || FAILED(hr)) {
            p.captureLogged = true;   // the stream-output statistics are gone: the statistics line stands alone
        }
    }
    if (p.statsLogged && p.captureLogged) {
        p.inFlight = false;
    } else if (++p.waited > kProbeGiveUpFrames) {
        ++p.lost;
        p.inFlight = false;
        probeReleaseQueries(p);   // a query that never resolved is made again
    }
}

}  // namespace

void srvTurretFrameTick(uint32_t frameNo) {
    if (!runtimeVrProfile()) return;   // flat (or not yet decided): every state stays at its zero, alignTick adds an exact 0
    readKeys();
    const int backCm = g_backCm.load(std::memory_order_relaxed), gapCm = g_gapCm.load(std::memory_order_relaxed);
    const bool on = journalSrvTurretView();
    if (on != g_on) {
        g_on = on;
        Log::get().note("srv turret view: %s at frame %u (Status.json Flags 0x%08X, seat kind 0x%X)", on ? "on" : "off", frameNo, static_cast<unsigned>(journalStatusFlags()),
                        static_cast<unsigned>(journalSeatKind()));
    }
    g_backSeatedZ = st::backSeatedZ(on, backCm);
    const bool wanted = on && gapCm > 0;
    if (wanted) cacheGeometry();
    if (wanted != detail::g_srvTurretHudWanted.load(std::memory_order_relaxed)) {
        detail::g_srvTurretHudWanted.store(wanted, std::memory_order_relaxed);
        if (wanted) drawGateArm();   // the gate re-samples at the frame boundary after this tick; this makes the very next draw see it too
    }
    // The 10 s line: only for a window in which the turret view was on at some point.
    const uint64_t now = GetTickCount64();
    if (!g_windowStartMs) g_windowStartMs = now;
    if (on) g_windowSeen = true;
    if (now - g_windowStartMs >= 10000) {
        if (g_windowSeen) windowLine(now);
        g_windowSeen = on;
        g_windowStartMs = now;
        for (auto& r : g_refused) r.store(0, std::memory_order_relaxed);
        for (auto& d : g_draws) d.store(0, std::memory_order_relaxed);
        for (auto& d : g_familyDraws) d.store(0, std::memory_order_relaxed);
        g_bound.store(0, std::memory_order_relaxed);
    }
}

double srvTurretBackSeatedZ() { return g_backSeatedZ; }

void srvTurretNotePublished(double seatedZ, uint32_t serial) {
    const double back = -seatedZ;   // metres the camera is behind the game's own
    if (std::fabs(back - g_loggedBackM) < 5.0e-4) return;
    g_loggedBackM = back;
    if (back > 0.0)
        Log::get().note("srv turret camera: back %.2f m published (serial %u)", back, static_cast<unsigned>(serial));
    else
        Log::get().note("srv turret camera: back cleared (serial %u)", static_cast<unsigned>(serial));
}

bool srvTurretHudBegin(ID3D11DeviceContext* ctx, int family, int eye) {
    if (!ctx || family < 0 || family >= static_cast<int>(kFamilies)) return false;
    if (eye != 0 && eye != 1) {
        refuse(st::Refuse::kNoEye);
        return false;
    }
    g_draws[eye].fetch_add(1, std::memory_order_relaxed);
    g_familyDraws[family].fetch_add(1, std::memory_order_relaxed);
    const uint32_t ordinal = g_probe[eye][family].ordinal++;   // this draw's place among the frame's matched draws of the eye and family (the instrument's rotation)
    const float gap = static_cast<float>(g_gapCm.load(std::memory_order_relaxed)) / 100.0f;
    if (!(gap > 0.0f)) return false;   // the key went to 0 between the frame tick and this draw: the game's own
    const EyeGeometry& geo = g_eyeGeo[eye];
    if (!geo.ok.load(std::memory_order_acquire)) {
        refuse(st::Refuse::kNoGeometry);
        return false;
    }
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    if (!guarded("srv.turret.topology", [&] { ctx->IAGetPrimitiveTopology(&topology); }) || topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        refuse(st::Refuse::kTopology);
        return false;
    }
    if (g_binding.needsRestore() || !ensureGs(ctx)) {
        refuse(st::Refuse::kNotReady);
        return false;
    }
    const st::TurretParams params = st::paramsFor(geo.s.load(std::memory_order_relaxed), gap);
    ID3D11Buffer* constants = g_constants[eye].get(ctx, params);
    if (!constants) {
        refuse(st::Refuse::kNotReady);
        return false;
    }
    // THE GPU INSTRUMENT: once a second per eye and family this draw goes through the stream-output twin (when there is one) with the queries around its scope.
    const bool sample = probeDue(ctx, eye, family, ordinal);
    const Probe& probe = g_probe[eye][family];
    const bool twin = sample && g_soGs[family] && probe.soBuf && probe.soStats && probe.staging;
    const auto begun = g_binding.begin(ctx, twin ? g_soGs[family] : g_gs[family], constants);
    if (begun == st::Binding::Begin::kGameHasGs) {
        refuse(st::Refuse::kGameGs);
        return false;
    }
    if (begun != st::Binding::Begin::kBound) {
        refuse(st::Refuse::kNotReady);
        return false;
    }
    if (sample) probeBegin(ctx, eye, family, ordinal, twin, params);
    g_bound.fetch_add(1, std::memory_order_relaxed);
    if (!g_firstNoted[family]) {
        g_firstNoted[family] = true;
        Log::get().note("srv turret hud: first draw of family %s through the shader (frame %u, eye %d): a vertex at view depth %.2f to %.2f m moves %.2f m nearer, s=%+.4f "
                        "(P00 %.3f, ipd_elite %.1f mm); the game's pixel shader, blend and every other state are unchanged, and its geometry stage and constant slot go back "
                        "after the issue.",
                        st::kFamilies[family].name, g_frame, eye, static_cast<double>(params.wMin), static_cast<double>(params.wMax), static_cast<double>(gap),
                        static_cast<double>(params.s), static_cast<double>(geo.p00.load(std::memory_order_relaxed)), 1000.0 * static_cast<double>(g_ipd.load(std::memory_order_relaxed)));
    }
    return true;
}

void srvTurretHudEnd(ID3D11DeviceContext* ctx) {
    probeEnd(ctx);   // the queries end and the stream-output target comes off, right before the game's geometry stage goes back
    const auto done = g_binding.finish(ctx);
    probeFinish(ctx);
    if (!done.restored && !g_restoreNoted) {
        g_restoreNoted = true;
        Log::get().note("srv turret hud: the game's geometry stage could not be put back after an issue; the frame boundary settles it and draws are refused (not-ready) until then.");
    }
}

void srvTurretHudFrameBoundary(ID3D11DeviceContext* ctx) {
    ++g_frame;
    for (int eye = 0; eye < 2; ++eye) {
        for (int family = 0; family < static_cast<int>(kFamilies); ++family) {
            Probe& p = g_probe[eye][family];
            probeRead(ctx, eye, family);
            // A frame with fewer matched draws than the wanted ordinal would never be sampled: start again from the first.
            if (p.ordinal <= p.wantOrd && p.ordinal > 0) p.wantOrd = 0;
            p.ordinal = 0;
        }
    }
    if (g_binding.needsRestore() && ctx && g_binding.settle(ctx) && !g_settleNoted) {
        g_settleNoted = true;
        Log::get().note("srv turret hud: the game's geometry stage and constant slot were put back at the frame boundary.");
    }
}

void srvTurretShutdown() {
    detail::g_srvTurretHudWanted.store(false, std::memory_order_relaxed);
    detail::g_srvTurretHudCounting.store(false, std::memory_order_relaxed);
    g_sample = nullptr;
    g_sampleEye = g_sampleFamily = -1;
    for (auto& eyeProbes : g_probe)
        for (Probe& p : eyeProbes) probeReset(p);
    g_binding.clear();
    for (auto& c : g_constants) c.reset();
    releaseGs();
    g_gsTried = false;
}

}  // namespace edvr
