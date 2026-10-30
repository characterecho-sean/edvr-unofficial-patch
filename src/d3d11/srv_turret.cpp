// The SRV turret view (issue 140): the module. srv_turret.h says what it does and what the log shows; srv_turret_math.h has the maths and the two TEMPORARY keys;
// srv_turret_hud_binding.h holds the constants and the binding, which tools/srv_turret_hud_test runs on WARP; srv_turret_hud_shader.h is the geometry shader's HLSL,
// compiled at build time.
#include "srv_turret.h"

#include "srv_turret_hud_binding.h"
#include "srv_turret_math.h"

#include "draw_gate.h"
#include "journal_watch.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"
#include "vr_camera_census.h"

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
}  // namespace detail

namespace st = srv_turret;

namespace {

static_assert(st::kBackDefaultCm == 60 && st::kBackMaxCm == 150 && st::kGapDefaultCm == 102 && st::kGapMaxCm == 200,
              "the literals in the two reads below are these, written as numbers for tools/gen_settings_schema.py");

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
ID3D11GeometryShader* g_gs = nullptr;
ID3D11Device* g_gsDevice = nullptr;   // identity only: the shader holds its device alive
bool g_gsTried = false;
st::Constants g_constants[2];
st::Binding g_binding;
std::atomic<uint32_t> g_draws[2] = {};
std::atomic<uint32_t> g_bound{0};
std::atomic<uint32_t> g_refused[static_cast<size_t>(st::Refuse::kCount)] = {};
uint32_t g_saidRefuse = 0;
bool g_firstNoted = false, g_restoreNoted = false, g_settleNoted = false;
uint32_t g_frame = 0;

void refuse(st::Refuse r) {
    g_refused[static_cast<size_t>(r)].fetch_add(1, std::memory_order_relaxed);
    const uint32_t bit = 1u << static_cast<unsigned>(r);
    if (g_saidRefuse & bit) return;
    g_saidRefuse |= bit;
    Log::get().note("srv turret hud: refused a draw (%s) -- the draw went on as the game made it. Said once per reason; the 10 s line counts every one.", st::refuseName(r));
}

void releaseGs() {
    st::release(g_gs);
    g_gsDevice = nullptr;
}

// The shader, made from the build's bytecode once per device (a device that comes and goes re-makes it; a failure is not retried on the same device).
// shaderSwapCreateGs is guarded and says the outcome in the log.
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
    if (!g_gs && !g_gsTried) {
        g_gsTried = true;
        g_gs = shaderSwapCreateGs(ctx, kSrvTurretHudGsBytecode, sizeof(kSrvTurretHudGsBytecode), "turret shift", "srv turret hud");
    }
    return g_gs != nullptr;
}

// The two keys, read when the configuration changes (Config::generation()) and not on every frame; said in the log at the first read and at each change.
void readKeys() {
    Config& cfg = Config::get();
    const uint32_t generation = cfg.generation();
    if (g_keysRead && generation == g_keysGeneration) return;
    const int back = cfg.getIntInRange("fix.srv_turret_camera_back", 60, 0, 150);
    const int gap = cfg.getIntInRange("fix.srv_turret_hud_gap", 102, 0, 200);
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
    (void)nowMs;
    Log::get().note("srv turret hud: gap=%.2f m wMin=%.2f wMax=%.2f s[L,R]=%+.4f,%+.4f ipd_elite=%.1fmm P00[L,R]=%.3f,%.3f draws[L,R]=%u,%u bound=%u "
                    "refused[no-eye,game-gs,topology,not-ready,no-geometry]=%u/%u/%u/%u/%u",
                    static_cast<double>(gap), static_cast<double>(p.wMin), static_cast<double>(p.wMax),
                    static_cast<double>(g_eyeGeo[0].s.load(std::memory_order_relaxed)), static_cast<double>(g_eyeGeo[1].s.load(std::memory_order_relaxed)),
                    1000.0 * static_cast<double>(g_ipd.load(std::memory_order_relaxed)),
                    static_cast<double>(g_eyeGeo[0].p00.load(std::memory_order_relaxed)), static_cast<double>(g_eyeGeo[1].p00.load(std::memory_order_relaxed)),
                    left, right, bound, refused[0], refused[1], refused[2], refused[3], refused[4]);
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

bool srvTurretHudBegin(ID3D11DeviceContext* ctx, int eye) {
    if (!ctx) return false;
    if (eye != 0 && eye != 1) {
        refuse(st::Refuse::kNoEye);
        return false;
    }
    g_draws[eye].fetch_add(1, std::memory_order_relaxed);
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
    const auto begun = g_binding.begin(ctx, g_gs, constants);
    if (begun == st::Binding::Begin::kGameHasGs) {
        refuse(st::Refuse::kGameGs);
        return false;
    }
    if (begun != st::Binding::Begin::kBound) {
        refuse(st::Refuse::kNotReady);
        return false;
    }
    g_bound.fetch_add(1, std::memory_order_relaxed);
    if (!g_firstNoted) {
        g_firstNoted = true;
        Log::get().note("srv turret hud: first draw through the shader (frame %u, eye %d): the deeper panels move %.2f m nearer in view depth %.2f to %.2f m, s=%+.4f "
                        "(P00 %.3f, ipd_elite %.1f mm); the game's pixel shader, blend and every other state are unchanged, and its geometry stage and constant slot go back "
                        "after the issue.",
                        g_frame, eye, static_cast<double>(gap), static_cast<double>(params.wMin), static_cast<double>(params.wMax), static_cast<double>(params.s),
                        static_cast<double>(geo.p00.load(std::memory_order_relaxed)), 1000.0 * static_cast<double>(g_ipd.load(std::memory_order_relaxed)));
    }
    return true;
}

void srvTurretHudEnd(ID3D11DeviceContext* ctx) {
    const auto done = g_binding.finish(ctx);
    if (!done.restored && !g_restoreNoted) {
        g_restoreNoted = true;
        Log::get().note("srv turret hud: the game's geometry stage could not be put back after an issue; the frame boundary settles it and draws are refused (not-ready) until then.");
    }
}

void srvTurretHudFrameBoundary(ID3D11DeviceContext* ctx) {
    ++g_frame;
    if (g_binding.needsRestore() && ctx && g_binding.settle(ctx) && !g_settleNoted) {
        g_settleNoted = true;
        Log::get().note("srv turret hud: the game's geometry stage and constant slot were put back at the frame boundary.");
    }
}

void srvTurretShutdown() {
    detail::g_srvTurretHudWanted.store(false, std::memory_order_relaxed);
    g_binding.clear();
    for (auto& c : g_constants) c.reset();
    releaseGs();
    g_gsTried = false;
}

}  // namespace edvr
