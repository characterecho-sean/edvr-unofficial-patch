// The SRV turret view (issue 140): the module. srv_turret.h says what it does and what the log shows; srv_turret_math.h has the maths, the families and the nine TEMPORARY
// keys; srv_turret_hud_binding.h holds the constants and the binding, which tools/srv_turret_hud_test runs on WARP; srv_turret_hud_shader.h is the geometry shaders' HLSL,
// compiled at build time; srv_radar_anchor.cpp is the hook that moves the radar's schematic in the game.
#include "srv_turret.h"

#include "srv_radar_anchor.h"
#include "srv_turret_hud_binding.h"
#include "srv_turret_math.h"

#include "draw_gate.h"
#include "journal_watch.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"
#include "vr_camera_census.h"   // nativeTemporalEyeGeometry, nativeTemporalHeadPose
#include "vscreen.h"   // vScreenCopyResourceRaw: the instrument's staging copy goes below the hooks

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"

#include <windows.h>

#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace edvr {

namespace detail {
std::atomic<bool> g_srvTurretHudWanted{false};
std::atomic<bool> g_srvTurretHudCounting{false};
std::atomic<uint32_t> g_srvTurretHudIssues{0};
std::atomic<bool> g_srvTurretHoloWatching{false};
std::atomic<bool> g_srvTurretRadarCamOn{false};
std::atomic<bool> g_srvTurretCameraRowsWanted{false};
std::atomic<bool> g_srvTurretRadarWritersOn{false};
uint32_t g_srvTurretDrawCount = 0, g_srvTurretDrawInstances = 0;
}  // namespace detail

namespace st = srv_turret;

namespace {

static_assert(st::kBackDefaultCm == 0 && st::kBackMaxCm == 150 && st::kForwardDefaultCm == 70 && st::kForwardMaxCm == 150 && st::kRadarForwardDefaultCm == 70 && st::kRadarForwardMaxCm == 150 &&
                  st::kPanelsDefaultCm == 120 && st::kRadarDefaultCm == 0 &&
                  st::kPushMaxCm == 200 && st::kGapDefaultCm == 0 && st::kGapMaxCm == 200,
              "the literals in the integer reads below are these, written as numbers for tools/gen_settings_schema.py");
constexpr uint32_t kFamilyN = st::kFamilyCount, kSigN = st::kSignatureCount;   // the draws' families, and the geometry shaders (one for each distinct output signature)

// ---- the frame thread's state ----------------------------------------------------------------------------------------------------------------------------------
bool g_keysRead = false;
uint32_t g_keysGeneration = 0;
std::atomic<int> g_backCm{0}, g_forwardCm{0}, g_radarForwardCm{0}, g_panelsCm{0}, g_radarCm{0}, g_gapCm{0};   // TEMPORARY keys, as last read (the five HUD keys are read by the draw path too)
static_assert(st::kDepthDefault == st::DepthMode::kFront, "the literal default of fix.srv_turret_hud_depth in readKeys is front, which the ini ships");
std::atomic<int> g_depthMode{static_cast<int>(st::kDepthDefault)};   // fix.srv_turret_hud_depth as a st::DepthMode (0 game, 1 real, 2 front)
std::atomic<bool> g_scissorOpen{true};      // fix.srv_turret_hud_scissor: open (the default) or game
static_assert(st::kRadarAnchorDefault == st::RadarAnchorMode::kOn, "the literal default of fix.srv_turret_radar_anchor in readKeys is on, which the ini ships");
std::atomic<int> g_anchorMode{static_cast<int>(st::kRadarAnchorDefault)};   // fix.srv_turret_radar_anchor as a st::RadarAnchorMode (0 on, 1 reversed, 2 off)
bool g_on = false;                          // the Scorpion's turret view, as the frame tick last saw it
bool g_ignored = false;                     // the turret view is on but the vehicle is not the Scorpion: said once for each stay
double g_backSeatedZ = 0.0;                 // what alignTick adds
double g_loggedBackM = 0.0;                 // what the log last said was published
bool g_windowSeen = false;                  // the turret view was on at some point of the 10 s window
uint64_t g_windowStartMs = 0;

// The geometry the move is made from, refreshed once a frame while the pass is wanted: each eye's tangents as Elite projects (the jitter included), the anchor and the HUD-forward
// translation in the eye's space. Written by the frame tick, read by the draw path under a sequence lock (the version is odd while the tick writes).
struct EyeSlot {
    std::atomic<uint32_t> version{0};
    std::atomic<float> v[13] = {};   // l, r, dn, up, the anchor (3), the panels' translation (3), the radar's (3)
    std::atomic<bool> ok{false};
};
EyeSlot g_eyeSlot[2];
std::atomic<float> g_ipd{0.0f};
// What the 10 s line says of it (log only): the anchor in the seated frame, the head's distance from it, eye 0's translation in the shader's axes.
std::atomic<float> g_anchorLog[3] = {}, g_headOffsetCm{0.0f}, g_translationLog[3] = {};
st::AnchorTracker g_anchor;   // the frame thread's
std::atomic<float> g_translationRadarLog[3] = {};
// What the draw path needs of the tick: the anchor the tracker holds (valid or following), and the pose sequence the tick built its eye frames from.
std::atomic<double> g_anchorPos[3] = {};
std::atomic<bool> g_anchorOk{false};
std::atomic<uint64_t> g_tickSeq{0};
// The draw path's own eye frames, the render thread's alone. The boundary tick runs BEFORE the host takes the next frame (native_temporal.cpp begin(), when the game starts the frame),
// so the frames the tick built for frame N come from frame N-1's pose: a head turn of 100 degrees a second is 1.1 degrees a frame, and the translation (0.7 m) and the push (times
// (d - gap) / d, 5 to 6) amplify that into a visible judder. The draw path therefore rebuilds the eye frames once per new frame sequence (nativeTemporalFrameSequence) from the pose of the
// frame being drawn and the tick's anchor; the counters say how often that found a pose newer than the tick's (the 10 s line).
uint64_t g_drawSeq = 0;
st::EyeFrame g_drawFrames[2];
bool g_drawOk[2] = {};
std::atomic<uint32_t> g_poseFrames{0}, g_poseRefreshed{0};
void publishEye(EyeSlot& slot, const st::EyeFrame& f, bool ok) {
    slot.version.fetch_add(1, std::memory_order_release);
    const float values[13] = {f.l, f.r, f.dn, f.up, f.a[0], f.a[1], f.a[2], f.t[0], f.t[1], f.t[2], f.tRadar[0], f.tRadar[1], f.tRadar[2]};
    for (int i = 0; i < 13; ++i) slot.v[i].store(values[i], std::memory_order_relaxed);
    slot.ok.store(ok, std::memory_order_relaxed);
    slot.version.fetch_add(1, std::memory_order_release);
}
// The eye's frame as last published; false when it is not ok or the tick kept writing.
bool readEye(int eye, st::EyeFrame& f) {
    EyeSlot& slot = g_eyeSlot[eye];
    for (int tries = 0; tries < 4; ++tries) {
        const uint32_t before = slot.version.load(std::memory_order_acquire);
        if (before & 1u) continue;
        float values[13];
        for (int i = 0; i < 13; ++i) values[i] = slot.v[i].load(std::memory_order_relaxed);
        const bool ok = slot.ok.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.version.load(std::memory_order_relaxed) != before) continue;
        f.l = values[0];
        f.r = values[1];
        f.dn = values[2];
        f.up = values[3];
        for (int i = 0; i < 3; ++i) {
            f.a[i] = values[4 + i];
            f.t[i] = values[7 + i];
            f.tRadar[i] = values[10 + i];
        }
        return ok;
    }
    return false;
}

// ---- the draw path's state (the render thread; the window's counters are read by the frame tick) -------------------------------------------------------------
// The build's bytecode of each signature's geometry shader (srv_turret_math.h kSignatures, in that order; the stream-output declaration of the instrument's twin comes from the
// same table, st::soDeclaration).
struct SigShader {
    const unsigned char* bytecode;
    size_t size;
    const char* name;
};
#define EDVR_SRV_TURRET_SHADER(letter, hex) {kSrvTurretHudGs##letter##Bytecode, sizeof(kSrvTurretHudGs##letter##Bytecode), "turret shift " hex}
const SigShader kShader[kSigN] = {
    EDVR_SRV_TURRET_SHADER(A, "1989E6D3"), EDVR_SRV_TURRET_SHADER(B, "2CECEC30"), EDVR_SRV_TURRET_SHADER(C, "DF3503CD"),
    EDVR_SRV_TURRET_SHADER(D, "44DDCDEB"), EDVR_SRV_TURRET_SHADER(E, "9B34C331"), EDVR_SRV_TURRET_SHADER(F, "9611A454"),
    EDVR_SRV_TURRET_SHADER(G, "B932058F"), EDVR_SRV_TURRET_SHADER(H, "5559BD94"),
};
#undef EDVR_SRV_TURRET_SHADER
ID3D11GeometryShader* g_gs[kSigN] = {};
ID3D11Device* g_gsDevice = nullptr;   // identity only: the shaders hold their device alive
bool g_gsTried = false;
st::Constants g_constants[2];
st::Binding g_binding;
st::Scissor g_scissor;                         // the game's scissor rectangles while a bound draw is issued (fix.srv_turret_hud_scissor)
std::atomic<uint32_t> g_scissorOpened{0};      // draws whose scissor was opened in the 10 s window
bool g_scissorNoted[kFamilyN] = {}, g_scissorRestoreNoted = false;
// The unmoved family draws (srv_turret_math.h UnmovedRoute), counted per family (the extra row: unknown) and route from any thread; the 10 s line spells the window's counts and the first of each
// route is said once (THE UNMOVED FAMILY DRAWS below). A pureDrawReissue call site is said once for each distinct return address, up to kReissueSitesMax.
std::atomic<uint32_t> g_unmoved[kFamilyN + 1][st::kUnmovedRoutes] = {};
std::atomic<uint32_t> g_unmovedNoted{0};
constexpr uint32_t kReissueSitesMax = 8;
std::atomic<uintptr_t> g_reissueSites[kReissueSitesMax] = {};
std::atomic<uint32_t> g_draws[2] = {};
std::atomic<uint32_t> g_familyDraws[kFamilyN] = {};
std::atomic<uint32_t> g_bound{0};
std::atomic<uint32_t> g_refused[static_cast<size_t>(st::Refuse::kCount)] = {};
uint32_t g_saidRefuse = 0;
bool g_firstNoted[kFamilyN] = {}, g_restoreNoted = false, g_settleNoted = false;
uint32_t g_frame = 0;
// THE LOCK's targets start again (TEMPORARY; srv_turret_math.h lockResetWhy): the frame tick asks, with the reason, and the render thread's frame boundary does it.
std::atomic<uint32_t> g_lockResetGen{0};
std::atomic<int> g_lockResetWhy{0};

void refuse(st::Refuse r) {
    g_refused[static_cast<size_t>(r)].fetch_add(1, std::memory_order_relaxed);
    const uint32_t bit = 1u << static_cast<unsigned>(r);
    if (g_saidRefuse & bit) return;
    g_saidRefuse |= bit;
    Log::get().note("srv turret hud: refused a draw (%s) -- the draw went on as the game made it. Said once per reason; the 10 s line counts every one.", st::refuseName(r));
}

// The instrument's stream-output twin of each signature's shader (the same bytecode; it also streams the vertices it writes out), made with the shaders and released with them.
ID3D11GeometryShader* g_soGs[kSigN] = {};
bool g_soTried = false;

void releaseGs() {
    for (uint32_t f = 0; f < kSigN; ++f) {
        st::release(g_gs[f]);
        st::release(g_soGs[f]);
    }
    g_soTried = false;
    g_gsDevice = nullptr;
}

// The twins: stream 0 is both streamed out (every signature element in order, then `pre`, sigFloats + 4 floats a vertex) and rasterized, so a draw issued through one looks exactly as one through
// the shader itself. A failure is said once and leaves the instrument to its statistics alone.
void makeTwins(ID3D11DeviceContext* ctx) {
    g_soTried = true;
    for (uint32_t f = 0; f < kSigN; ++f) {
        ID3D11Device* d = nullptr;
        HRESULT hr = E_FAIL;
        const bool ran = guarded("srv.turret.twin", [&] {
            ctx->GetDevice(&d);
            if (!d) return;
            D3D11_SO_DECLARATION_ENTRY decl[st::kMaxSigElements] = {};
            const UINT entries = st::soDeclaration(f, decl);
            const UINT stride = (st::sigFloats(f) + st::kPreFloats) * 4;
            hr = d->CreateGeometryShaderWithStreamOutput(kShader[f].bytecode, kShader[f].size, decl, entries, &stride, 1, 0, nullptr, &g_soGs[f]);
        });
        st::release(d);
        if (!ran || FAILED(hr)) st::release(g_soGs[f]);
        Log::get().note("srv turret hud: stream-output twin of the %s shader (the GPU instrument's capture) %s (0x%08X); %s", st::kSignatures[f].name,
                        g_soGs[f] ? "created" : "NOT made", static_cast<unsigned>(hr),
                        g_soGs[f] ? "one draw a second per eye and family is issued through it" : "the instrument keeps to its statistics");
    }
}

// The shaders, made from the build's bytecode once per device (a device that comes and goes re-makes them; a failure is not retried on the same device).
// shaderSwapCreateGs is guarded and says each outcome in the log. True when the signature's shader is there: a draw is never bound for a family whose shader is missing, and
// one signature's failing leaves the others' families bound.
bool ensureGs(ID3D11DeviceContext* ctx, uint32_t sig) {
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
        for (uint32_t f = 0; f < kSigN; ++f) g_gs[f] = shaderSwapCreateGs(ctx, kShader[f].bytecode, kShader[f].size, kShader[f].name, "srv turret hud");
    }
    if (!g_soTried) makeTwins(ctx);
    return sig < kSigN && g_gs[sig] != nullptr;
}

// The keys, read when the configuration changes (Config::generation()) and not on every frame; said in the log at the first read and at each change.
void readKeys() {
    Config& cfg = Config::get();
    const uint32_t generation = cfg.generation();
    if (g_keysRead && generation == g_keysGeneration) return;
    const int back = cfg.getIntInRange("fix.srv_turret_camera_back", 0, 0, 150);
    const int forward = cfg.getIntInRange("fix.srv_turret_hud_forward", 70, 0, 150);
    const int radarForward = cfg.getIntInRange("fix.srv_turret_radar_forward", 70, 0, 150);
    const int panels = cfg.getIntInRange("fix.srv_turret_panels_back", 120, 0, 200);
    const int radar = cfg.getIntInRange("fix.srv_turret_radar_back", 0, 0, 200);
    const int gap = cfg.getIntInRange("fix.srv_turret_hud_gap", 0, 0, 200);
    const std::string depthText = cfg.getString("fix.srv_turret_hud_depth", "front");
    const st::DepthMode depth = st::depthModeOf(depthText.c_str());
    const bool scissorOpen = cfg.getString("fix.srv_turret_hud_scissor", "open") != "game";
    const std::string anchorText = cfg.getString("fix.srv_turret_radar_anchor", "on");
    const st::RadarAnchorMode anchorMode = st::radarAnchorModeOf(anchorText.c_str());
    const bool changed = !g_keysRead || back != g_backCm.load(std::memory_order_relaxed) || forward != g_forwardCm.load(std::memory_order_relaxed) ||
                         radarForward != g_radarForwardCm.load(std::memory_order_relaxed) || panels != g_panelsCm.load(std::memory_order_relaxed) || radar != g_radarCm.load(std::memory_order_relaxed) ||
                         gap != g_gapCm.load(std::memory_order_relaxed) || static_cast<int>(depth) != g_depthMode.load(std::memory_order_relaxed) ||
                         scissorOpen != g_scissorOpen.load(std::memory_order_relaxed) || static_cast<int>(anchorMode) != g_anchorMode.load(std::memory_order_relaxed);
    g_keysRead = true;
    g_keysGeneration = generation;
    g_backCm.store(back, std::memory_order_relaxed);
    g_forwardCm.store(forward, std::memory_order_relaxed);
    g_radarForwardCm.store(radarForward, std::memory_order_relaxed);
    g_panelsCm.store(panels, std::memory_order_relaxed);
    g_radarCm.store(radar, std::memory_order_relaxed);
    g_gapCm.store(gap, std::memory_order_relaxed);
    g_depthMode.store(static_cast<int>(depth), std::memory_order_relaxed);
    g_scissorOpen.store(scissorOpen, std::memory_order_relaxed);
    g_anchorMode.store(static_cast<int>(anchorMode), std::memory_order_relaxed);
    if (changed)
        Log::get().note("srv turret settings: camera back %d cm, hud forward %d cm, radar forward %d cm, panels back %d cm, radar back %d cm, hud gap %d cm, hud depth %s, hud scissor %s, radar anchor %s (live; "
                        "fix.srv_turret_camera_back, fix.srv_turret_hud_forward, fix.srv_turret_radar_forward, fix.srv_turret_panels_back, fix.srv_turret_radar_back, fix.srv_turret_hud_gap, "
                        "fix.srv_turret_hud_depth, fix.srv_turret_hud_scissor and fix.srv_turret_radar_anchor, TEMPORARY; 0 is the game's own; the depth key is front, game or real, the scissor key "
                        "open or game, the radar anchor key on, reversed, off or camera)",
                        back, forward, radarForward, panels, radar, gap, st::depthModeName(depth), scissorOpen ? "open" : "game", st::radarAnchorModeName(anchorMode));
}

// The constants a group's draws run with at the keys in force, on this eye frame: the group's push-back when above 0, else the pull when the gap is above 0, else the translation alone
// when the group's forward key is above 0 (HUD forward for the panels, radar forward for the radar), else none (false: the draw is the game's own).
bool paramsOfGroup(st::Group group, const st::EyeFrame& frame, st::TurretParams& out) {
    if (!st::paramsForGroup(frame, group, st::pushCmOf(group, g_panelsCm.load(std::memory_order_relaxed), g_radarCm.load(std::memory_order_relaxed)), g_gapCm.load(std::memory_order_relaxed),
                            st::forwardCmOf(group, g_forwardCm.load(std::memory_order_relaxed), g_radarForwardCm.load(std::memory_order_relaxed)), out))
        return false;
    out = st::withDepth(out, static_cast<st::DepthMode>(g_depthMode.load(std::memory_order_relaxed)));   // fix.srv_turret_hud_depth
    return true;
}

// Both eyes' frames from a head and eye pose and an anchor: the tangents are the eye's frustum the host gave plus the tangent shift the eye jitter moves it by (nativeTemporalEyeGeometry; the
// game projects with the sum); each frame holds the anchor and the two groups' forward translations in the eye's own space. ok[e] is false for an eye that cannot be built (no anchor, no
// frustum, not a frame): its draws are refused (no-geometry).
void buildEyeFrames(const float head[12], const float eyeToHead[2][12], bool haveAnchor, const double anchor[3], int forwardCm, int radarForwardCm, st::EyeFrame frames[2], bool ok[2]) {
    for (uint32_t e = 0; e < 2; ++e) {
        float tan[4] = {}, shift[2] = {};
        uint64_t sequence = 0;
        const bool haveTan = nativeTemporalEyeGeometry(e, &sequence, tan, shift);
        const float tangents[4] = {tan[0] + shift[0], tan[1] + shift[0], tan[2] + shift[1], tan[3] + shift[1]};
        ok[e] = haveAnchor && haveTan && st::p00Usable(st::p00OfTangents(tangents)) &&
                st::eyeFrameFrom(head, eyeToHead[e], anchor, static_cast<double>(forwardCm) / 100.0, static_cast<double>(radarForwardCm) / 100.0, tangents, frames[e]);
        if (!ok[e]) frames[e] = st::EyeFrame{};
    }
}

// The pose, the tangents and the anchor as the host gave them at this frame boundary (native_temporal.cpp: nativeTemporalHeadPose is the head and eye matrices Elite is given). The anchor
// is the head's seated position, taken by the tracker (srv_turret_math.h AnchorTracker); each eye's frame holds the anchor and the translations in its own space. This is the tick's
// side: it steps the anchor, publishes it for the draw path, and publishes frames for the log and as the draw path's fallback; the frames the draws use are rebuilt at draw time
// (drawEyeFrame), from the pose of the frame being drawn.
void cacheGeometry(bool on, int backCm, int forwardCm, int radarForwardCm) {
    uint64_t poseSequence = 0;
    float head[12] = {}, eyeToHead[2][12] = {};
    const bool havePose = nativeTemporalHeadPose(&poseSequence, head, eyeToHead);
    float ipd = 0.0f;
    if (havePose) {
        const float dx = eyeToHead[0][3] - eyeToHead[1][3], dy = eyeToHead[0][7] - eyeToHead[1][7], dz = eyeToHead[0][11] - eyeToHead[1][11];
        ipd = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    g_ipd.store(st::ipdUsable(ipd) ? ipd : 0.0f, std::memory_order_relaxed);
    const double headAt[3] = {head[3], head[7], head[11]};
    if (havePose) {
        const st::AnchorReason latched = g_anchor.step(headAt, on, backCm, forwardCm, radarForwardCm);
        if (latched != st::AnchorReason::kNone) {
            const double* a = g_anchor.anchor();
            Log::get().note("srv turret anchor: latched at %.3f,%.3f,%.3f m seated (%s)", a[0], a[1], a[2], st::anchorReasonName(latched));
        }
    }
    const bool haveAnchor = havePose && g_anchor.valid();
    if (haveAnchor) {
        const double* a = g_anchor.anchor();
        const double dx = headAt[0] - a[0], dy = headAt[1] - a[1], dz = headAt[2] - a[2];
        g_headOffsetCm.store(static_cast<float>(100.0 * std::sqrt(dx * dx + dy * dy + dz * dz)), std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i) {
            g_anchorLog[i].store(static_cast<float>(a[i]), std::memory_order_relaxed);
            g_anchorPos[i].store(a[i], std::memory_order_relaxed);
        }
    }
    g_anchorOk.store(haveAnchor, std::memory_order_release);   // after the position
    st::EyeFrame frames[2];
    bool ok[2] = {};
    buildEyeFrames(head, eyeToHead, haveAnchor, g_anchor.anchor(), forwardCm, radarForwardCm, frames, ok);
    for (uint32_t e = 0; e < 2; ++e) publishEye(g_eyeSlot[e], frames[e], ok[e]);
    if (ok[0])
        for (int i = 0; i < 3; ++i) {
            g_translationLog[i].store(frames[0].t[i], std::memory_order_relaxed);
            g_translationRadarLog[i].store(frames[0].tRadar[i], std::memory_order_relaxed);
        }
    if (havePose) g_tickSeq.store(poseSequence, std::memory_order_release);
}

// The eye frame the draw uses: rebuilt ONCE for each new frame sequence from the pose of the frame being drawn (render thread). Falls back to the tick's frames (a frame behind) when the
// pose or the anchor is not there.
bool drawEyeFrame(int eye, st::EyeFrame& out) {
    const uint64_t seq = nativeTemporalFrameSequence();
    if (seq != 0 && seq != g_drawSeq) {
        g_drawSeq = seq;
        g_poseFrames.fetch_add(1, std::memory_order_relaxed);
        if (seq != g_tickSeq.load(std::memory_order_acquire)) g_poseRefreshed.fetch_add(1, std::memory_order_relaxed);   // the tick had built from an older frame's pose
        g_drawOk[0] = g_drawOk[1] = false;
        uint64_t poseSequence = 0;
        float head[12] = {}, eyeToHead[2][12] = {};
        if (g_anchorOk.load(std::memory_order_acquire) && nativeTemporalHeadPose(&poseSequence, head, eyeToHead)) {
            const double anchor[3] = {g_anchorPos[0].load(std::memory_order_relaxed), g_anchorPos[1].load(std::memory_order_relaxed), g_anchorPos[2].load(std::memory_order_relaxed)};
            buildEyeFrames(head, eyeToHead, true, anchor, g_forwardCm.load(std::memory_order_relaxed), g_radarForwardCm.load(std::memory_order_relaxed), g_drawFrames, g_drawOk);
        }
    }
    if (g_drawOk[eye]) {
        out = g_drawFrames[eye];
        return true;
    }
    return readEye(eye, out);
}

// One group's constants as the 10 s line spells them: the gap and the window of anchor distances its draws run with at the keys in force, signed (a push has a negative gap; a gap of 0 is
// the translation alone), or "off" when none of its push, the gap key and the HUD-forward key is above 0.
void groupWindow(char (&out)[96], st::Group group) {
    st::TurretParams p;
    if (paramsOfGroup(group, st::EyeFrame{}, p))
        std::snprintf(out, sizeof(out), "gap=%.2f m dMin=%.2f dMax=%.2f", static_cast<double>(p.gap), static_cast<double>(p.dMin), static_cast<double>(p.dMax));
    else
        std::snprintf(out, sizeof(out), "off");
}

void windowLine(uint64_t nowMs) {
    char panels[96], radar[96];
    groupWindow(panels, st::Group::kPanels);
    groupWindow(radar, st::Group::kRadar);
    uint32_t refused[static_cast<size_t>(st::Refuse::kCount)];
    for (size_t i = 0; i < static_cast<size_t>(st::Refuse::kCount); ++i) refused[i] = g_refused[i].exchange(0, std::memory_order_relaxed);
    const uint32_t left = g_draws[0].exchange(0, std::memory_order_relaxed), right = g_draws[1].exchange(0, std::memory_order_relaxed);
    const uint32_t bound = g_bound.exchange(0, std::memory_order_relaxed), scissorOpened = g_scissorOpened.exchange(0, std::memory_order_relaxed);
    uint32_t unmovedCounts[kFamilyN + 1][st::kUnmovedRoutes];
    for (uint32_t f = 0; f <= kFamilyN; ++f)
        for (uint32_t r = 0; r < st::kUnmovedRoutes; ++r) unmovedCounts[f][r] = g_unmoved[f][r].exchange(0, std::memory_order_relaxed);
    char unmoved[512];
    st::unmovedList(unmoved, sizeof(unmoved), unmovedCounts);
    // The matched draws of each family in the window, as two comma-separated lists in the table's order: the names, then the counts.
    char names[512] = {}, counts[256] = {};
    size_t nn = 0, nc = 0;
    for (uint32_t f = 0; f < kFamilyN; ++f) {
        const uint32_t n = g_familyDraws[f].exchange(0, std::memory_order_relaxed);
        const int a = std::snprintf(names + nn, sizeof(names) - nn, "%s%s", f ? "," : "", st::kFamilies[f].name);
        const int b = std::snprintf(counts + nc, sizeof(counts) - nc, "%s%u", f ? "," : "", n);
        if (a > 0 && static_cast<size_t>(a) < sizeof(names) - nn) nn += static_cast<size_t>(a);
        if (b > 0 && static_cast<size_t>(b) < sizeof(counts) - nc) nc += static_cast<size_t>(b);
    }
    (void)nowMs;
    st::EyeFrame frames[2];
    const bool okL = readEye(0, frames[0]), okR = readEye(1, frames[1]);
    const uint32_t poseFrames = g_poseFrames.exchange(0, std::memory_order_relaxed), poseRefreshed = g_poseRefreshed.exchange(0, std::memory_order_relaxed);
    Log::get().note("srv turret hud: panels: %s; radar: %s; anchor=(%.3f,%.3f,%.3f) head_offset=%.1f cm T=(%.3f,%.3f,%.3f) Tradar=(%.3f,%.3f,%.3f) ipd_elite=%.1fmm P00[L,R]=%.3f,%.3f "
                    "draws[L,R]=%u,%u bound=%u scissor opened %u pose refreshed at draw: %u of %u frames unmoved family draws: %s "
                    "refused[no-eye,game-gs,topology,not-ready,no-geometry]=%u/%u/%u/%u/%u families[%s]=%s",
                    panels, radar,
                    static_cast<double>(g_anchorLog[0].load(std::memory_order_relaxed)), static_cast<double>(g_anchorLog[1].load(std::memory_order_relaxed)),
                    static_cast<double>(g_anchorLog[2].load(std::memory_order_relaxed)), static_cast<double>(g_headOffsetCm.load(std::memory_order_relaxed)),
                    static_cast<double>(g_translationLog[0].load(std::memory_order_relaxed)), static_cast<double>(g_translationLog[1].load(std::memory_order_relaxed)),
                    static_cast<double>(g_translationLog[2].load(std::memory_order_relaxed)), static_cast<double>(g_translationRadarLog[0].load(std::memory_order_relaxed)),
                    static_cast<double>(g_translationRadarLog[1].load(std::memory_order_relaxed)), static_cast<double>(g_translationRadarLog[2].load(std::memory_order_relaxed)),
                    1000.0 * static_cast<double>(g_ipd.load(std::memory_order_relaxed)),
                    okL ? static_cast<double>(st::p00OfTangents(&frames[0].l)) : 0.0, okR ? static_cast<double>(st::p00OfTangents(&frames[1].l)) : 0.0,
                    left, right, bound, scissorOpened, poseRefreshed, poseFrames, unmoved, refused[0], refused[1], refused[2], refused[3], refused[4], names, counts);
}

// ---- THE GPU INSTRUMENT ------------------------------------------------------------------------------------------------------------------------------------------------
// The flight of 2026-10-30 (Steam, d662d26c) bound an earlier build of this pass for every matched draw (bound = draws, no refusals) and the panels did not move; that build
// targeted the reticle's sprite, whose clip w is about 500 m. From the CPU there is no way to tell a shader that never ran from one that ran on vertices outside its window, so:
// once a second per eye and family, one matched draw (the ordinal of the family's draws in the frame rotates, so the near layer's draws and the deeper panels' are all measured
// within a few seconds) is issued through the stream-output twin with a pipeline-statistics query and a stream-output-statistics query around the scope, Begin right after the
// bind, End right before the restore. gs=0 with ia_prims above 0 is a shader that never ran; the captured vertices say where the shader found each draw's clip w and what it did
// to the clip x: with a push the weapon panels' and the radar's draws read in_window above 0 at 0.1 to 0.7 m and the larger panels' in_window 0 at 1.3 m and more (with the pull it is
// the other way round). Read back without stalling.
constexpr uint32_t kProbeEveryMs = 1000, kProbeGiveUpFrames = 8, kProbeOrdinals = 8;
// The radar disc (vs DF3503CD, ps 8C08EB25): its sampled draw also carries an occlusion query (the samples that passed the depth and stencil tests) and a staging copy of two registers its
// pixel shader reads, cb2[11] (the alpha, .w) and cb1[90] (the colour's scale, .y). Never awaited; read back like the statistics.
constexpr uint32_t kDiscFamily = 2;
static_assert(st::kFamilies[kDiscFamily].vs == 0xDF3503CD07F9B10Cull && st::kFamilies[kDiscFamily].ps == 0x8C08EB252B0F6095ull, "the disc probe is the radar disc's family");
constexpr uint32_t kDiscAlphaSlot = 2, kDiscAlphaRow = 11, kDiscIntensitySlot = 1, kDiscIntensityRow = 90;

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
    // the radar disc's extras (family kDiscFamily only)
    ID3D11Query* occlusion = nullptr;
    ID3D11Buffer* cbStage = nullptr;  // 32 bytes: cb2[11] at 0, cb1[90] at 16
    bool discFailed = false;          // creation failed on this device: not retried
    bool planePose = false;           // eye 0's pose at the sampled disc draw, for the plane lines
    double poseR[9] = {}, poseE[3] = {}, poseHead[3] = {};
    bool discArmed = false;           // the query was begun in the open sampled scope
    bool discPending = false;         // End issued, the results not yet logged
    bool discCopied[2] = {};          // the alpha register, the intensity register were copied
    uint32_t discWaited = 0, discIssues = 0, discDraw = 0;
    // a sample of the radar correction's moved billboard (radarCorrectionBegin): its kind (1 disc, 2 icon), the delta in the eye, the content depth (0: none, the plain delta)
    bool corr = false;
    int corrKind = 0;
    float corrDelta[3] = {};
};
Probe g_probe[2][kFamilyN];           // by eye and family
Probe* g_sample = nullptr;            // the probe of the draw whose scope is open and sampled
int g_sampleEye = -1, g_sampleFamily = -1;
bool g_sampleTwin = false;            // ... and whether it was issued through the twin with its stream-output target
bool g_soBusyNoted = false;
bool g_pairTaking = false;            // THE PAIR CAPTURE (TEMPORARY) takes the radar billboard being bound through the twin: the probe's sample stands aside for it (probeDue)

void probeReleaseQueries(Probe& p) {
    st::release(p.stats);
    st::release(p.soStats);
}
void probeReset(Probe& p) {
    probeReleaseQueries(p);
    st::release(p.occlusion);
    st::release(p.cbStage);
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
    const bool twinMade = g_soGs[st::kFamilies[family].sig] != nullptr;
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
    if (ran && !what && static_cast<uint32_t>(family) == kDiscFamily && !p.discFailed && (!p.occlusion || !p.cbStage)) {   // the disc's extras: a failure here leaves the rest of the instrument
        const char* discWhat = nullptr;
        HRESULT discHr = S_OK;
        const bool discRan = guarded("srv.turret.probe.disc.make", [&] {
            if (!p.occlusion) {
                const D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
                discHr = d->CreateQuery(&qd, &p.occlusion);
                if (FAILED(discHr)) { discWhat = "the occlusion query"; return; }
            }
            if (!p.cbStage) {
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = 32;
                bd.Usage = D3D11_USAGE_STAGING;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                discHr = d->CreateBuffer(&bd, nullptr, &p.cbStage);
                if (FAILED(discHr)) { discWhat = "the constant staging buffer"; return; }
            }
        });
        if (!discRan || discWhat) {
            Log::get().note("srv turret hud: the GPU instrument could not be made for eye %d, family %s (%s, 0x%08X); the pass itself is unaffected.", eye, st::kFamilies[family].name,
                            discWhat ? discWhat : "a fault", static_cast<unsigned>(discHr));
            st::release(p.occlusion);
            st::release(p.cbStage);
            p.discFailed = true;
        }
    }
    st::release(d);
    if (!ran || what) {
        Log::get().note("srv turret hud: the GPU instrument could not be made for eye %d, family %s (%s, 0x%08X); the pass itself is unaffected.", eye, st::kFamilies[family].name,
                        what ? what : "a fault", static_cast<unsigned>(hr));
        probeReleaseQueries(p);
        st::release(p.soBuf);
        st::release(p.staging);
        st::release(p.occlusion);
        st::release(p.cbStage);
        p.failed = true;   // not retried on this device
        return false;
    }
    return true;
}

// The sample is due: this eye's and family's matched draw of the wanted ordinal, a second since the last, nothing in flight, the objects made, and no stream-output target of
// the game's bound.
bool probeDue(ID3D11DeviceContext* ctx, int eye, int family, uint32_t ordinal) {
    Probe& p = g_probe[eye][family];
    if (g_pairTaking || p.inFlight || ordinal != p.wantOrd) return false;
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
    p.corr = false;   // radarCorrectionBegin marks its own samples after this
    p.issues = 0;
    g_sample = &p;
    g_sampleEye = eye;
    g_sampleFamily = family;
    g_sampleTwin = twin;
    detail::g_srvTurretHudIssues.store(0, std::memory_order_relaxed);
    detail::g_srvTurretHudCounting.store(true, std::memory_order_relaxed);
    p.planePose = false;
    if (static_cast<uint32_t>(family) == kDiscFamily && eye == 0) {   // the eye's pose of the frame being drawn, for the plane lines at the readback
        uint64_t poseSequence = 0;
        float head[12] = {}, eyeToHead[2][12] = {};
        if (nativeTemporalHeadPose(&poseSequence, head, eyeToHead) && st::eyePoseOf(head, eyeToHead[0], p.poseR, p.poseE)) {
            p.poseHead[0] = head[3];
            p.poseHead[1] = head[7];
            p.poseHead[2] = head[11];
            p.planePose = true;
        }
    }
    const bool disc = static_cast<uint32_t>(family) == kDiscFamily && p.occlusion && p.cbStage && !p.discFailed && !p.discPending;
    if (!guarded("srv.turret.probe.begin", [&] {
            if (twin) {
                ID3D11Buffer* target = p.soBuf;
                const UINT offset = 0;
                ctx->SOSetTargets(1, &target, &offset);
            }
            ctx->Begin(p.stats);
            if (twin) ctx->Begin(p.soStats);
            if (disc) {
                ctx->Begin(p.occlusion);
                p.discArmed = true;
            }
        })) {
        p.failed = true;   // a fault in the instrument's own calls ends the instrument, not the pass
    }
}

// The disc's two pixel-shader registers are copied past the hooks (st::copyPsRegister, which the rig runs on WARP).
void rawCopyRegion(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT dstX, ID3D11Resource* src, const D3D11_BOX* box) { vScreenCopySubresourceRegionRaw(ctx, dst, dstX, src, box); }
bool probeCopyRegister(ID3D11DeviceContext* ctx, ID3D11Buffer* stage, uint32_t slot, uint32_t row, UINT to) { return st::copyPsRegister(ctx, stage, slot, row, to, rawCopyRegion); }

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
            if (p.discArmed) ctx->End(p.occlusion);
        })) {
        p.failed = true;
    }
    if (p.discArmed && !p.failed) {   // the registers as the game left them for the last issue
        p.discCopied[0] = probeCopyRegister(ctx, p.cbStage, kDiscAlphaSlot, kDiscAlphaRow, 0);
        p.discCopied[1] = probeCopyRegister(ctx, p.cbStage, kDiscIntensitySlot, kDiscIntensityRow, 16);
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
    if (p.discArmed) {
        p.discArmed = false;
        p.discPending = !p.failed;
        p.discWaited = 0;
        p.discIssues = p.issues;
        p.discDraw = p.draw;
    }
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

// The radar disc's quads in the seated frame (srv_turret_math.h quadInSeated): one line for each quad of the capture, up to kDiscPlaneQuads, from the PRE-move clip positions, with the head's seated
// position at that frame. The capture is every issue of the scope in turn, so when the scope made more than one issue the later quads repeat the first issue's.
constexpr uint32_t kDiscPlaneQuads = 4;
void logDiscPlanes(const float* records, uint32_t vertices, const st::Family& fam, const Probe& p) {
    const float tangents[4] = {p.params.l, p.params.r, p.params.dn, p.params.up};
    for (uint32_t k = 0; k < kDiscPlaneQuads; ++k) {
        st::SeatedQuad q;
        if (!st::quadInSeated(records, fam.strideBytes / 4, fam.preFloat, 6 * k, vertices, tangents, p.poseR, p.poseE, q)) break;
        Log::get().note("srv turret hud disc plane: inst=%u centre=(%.3f,%.3f,%.3f) normal=(%.3f,%.3f,%.3f) head=(%.3f,%.3f,%.3f)", k, q.centre[0], q.centre[1], q.centre[2], q.normal[0], q.normal[1],
                        q.normal[2], p.poseHead[0], p.poseHead[1], p.poseHead[2]);
    }
}

// The hologram billboard's vertices (families 7 and 8, signature H: lighting position 3, UV 2, the moved clip position 4, `pre` 4): for eye 0's sampled draw when the billboard is near (the game's
// clip w under 1 m at every vertex), each vertex's clip position before and after the move and its UV, with the tangents and the translation it ran with, so the billboard's corners can be put in
// the eye's space offline and its UV range read (a fixed 0..1 quad, or the picture's own window).
static_assert(st::kSignatures[st::kSceneSignature].el[0].comps == 3 && st::kSignatures[st::kSceneSignature].el[1].comps == 2, "signature H: the lighting position, then the UV");
void logHoloQuad(const float* records, uint32_t vertices, const st::Family& fam, const Probe& p, int eye) {
    if (eye != 0 || !records || vertices < 3) return;
    const uint32_t stride = fam.strideBytes / 4, uvFloat = 3, n = vertices < 6 ? vertices : 6;
    for (uint32_t i = 0; i < n; ++i)
        if (!(records[stride * i + fam.preFloat + 3] < 1.0f)) return;   // a far billboard (the target panel's): not the radar's
    char line[1100];
    int len = std::snprintf(line, sizeof(line), "srv turret hud holo quad: family=%s eye=%d draw=%u tangents=(%.4f,%.4f,%.4f,%.4f) T=(%.3f,%.3f,%.3f)", fam.name, eye, p.draw,
                            static_cast<double>(p.params.l), static_cast<double>(p.params.r), static_cast<double>(p.params.dn), static_cast<double>(p.params.up),
                            static_cast<double>(p.params.tx), static_cast<double>(p.params.ty), static_cast<double>(p.params.tz));
    for (uint32_t i = 0; i < n && len > 0 && len < static_cast<int>(sizeof(line)) - 140; ++i) {
        const float* r = records + static_cast<size_t>(stride) * i;
        const float* pre = r + fam.preFloat;
        const float* post = r + fam.posFloat;
        len += std::snprintf(line + len, sizeof(line) - len, " | v%u pre=(%.4f,%.4f,%.4f,%.4f) post=(%.4f,%.4f,%.4f) uv=(%.3f,%.3f)", i, static_cast<double>(pre[0]), static_cast<double>(pre[1]),
                             static_cast<double>(pre[2]), static_cast<double>(pre[3]), static_cast<double>(post[0]), static_cast<double>(post[1]), static_cast<double>(post[3]),
                             static_cast<double>(r[uvFloat]), static_cast<double>(r[uvFloat + 1]));
    }
    Log::get().note("%s", line);
}

// The radar correction's sample (radarCorrectionBegin, THE CONTENT DEPTH): the quad's clip w range as the game made it, the centre of its NDC box before the move and how far the move took
// that centre, and where content at the content depth z_c on the centre's ray would go when moved by the same delta (st::contentShiftNdc). The first quad of the capture (its first six
// vertices); a vertex behind the eye before or after the move has no NDC, and the line is not said.
void logQuadProbe(const float* records, uint32_t vertices, const st::Family& fam, const Probe& p, int eye) {
    if (!records || vertices < 3) return;
    const uint32_t stride = fam.strideBytes / 4, n = vertices < 6 ? vertices : 6;
    double wLo = 1.0e30, wHi = -1.0e30, pre[4] = {1.0e30, -1.0e30, 1.0e30, -1.0e30}, post[4] = {1.0e30, -1.0e30, 1.0e30, -1.0e30};   // x min, x max, y min, y max
    for (uint32_t i = 0; i < n; ++i) {
        const float* a = records + static_cast<size_t>(stride) * i + fam.preFloat;
        const float* b = records + static_cast<size_t>(stride) * i + fam.posFloat;
        if (!(a[3] > 1.0e-6f) || !(b[3] > 1.0e-6f)) return;
        wLo = std::min(wLo, double(a[3]));
        wHi = std::max(wHi, double(a[3]));
        const double ax = double(a[0]) / a[3], ay = double(a[1]) / a[3], bx = double(b[0]) / b[3], by = double(b[1]) / b[3];
        pre[0] = std::min(pre[0], ax);
        pre[1] = std::max(pre[1], ax);
        pre[2] = std::min(pre[2], ay);
        pre[3] = std::max(pre[3], ay);
        post[0] = std::min(post[0], bx);
        post[1] = std::max(post[1], bx);
        post[2] = std::min(post[2], by);
        post[3] = std::max(post[3], by);
    }
    const double cx = (pre[0] + pre[1]) * 0.5, cy = (pre[2] + pre[3]) * 0.5, mx = (post[0] + post[1]) * 0.5 - cx, my = (post[2] + post[3]) * 0.5 - cy;
    char depth[48] = "none (the plain delta)", expected[96] = "none";
    double e[2];
    if (p.params.invZc > 0.0f) {
        const double zc = 1.0 / double(p.params.invZc);
        std::snprintf(depth, sizeof(depth), "%.3f m", zc);
        if (st::contentShiftNdc(p.params, cx, cy, p.corrDelta, zc, e)) std::snprintf(expected, sizeof(expected), "(%.4f,%.4f) ndc for content at z_c", e[0], e[1]);
    }
    Log::get().note("srv turret radar anchor: quad probe: eye %d %s w=[%.3f,%.3f] pre ndc centre (%.4f,%.4f) moved by (%.4f,%.4f) ndc; content depth z_c %s; expected %s; delta in the eye "
                    "(%.2f,%.2f,%.2f) cm",
                    eye, p.corrKind == 1 ? "disc" : "icon", wLo, wHi, cx, cy, mx, my, depth, expected, static_cast<double>(p.corrDelta[0]) * 100.0,
                    static_cast<double>(p.corrDelta[1]) * 100.0, static_cast<double>(p.corrDelta[2]) * 100.0);
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
                const st::CaptureSummary c = st::summarizeCapture(static_cast<const float*>(m.pData), vertices, fam.strideBytes / 4, fam.posFloat, fam.preFloat, p.params);
                if (fam.sig == st::kSceneSignature) logHoloQuad(static_cast<const float*>(m.pData), vertices, fam, p, eye);
                if (p.corr) logQuadProbe(static_cast<const float*>(m.pData), vertices, fam, p, eye);
                if (static_cast<uint32_t>(family) == kDiscFamily && eye == 0 && p.planePose) logDiscPlanes(static_cast<const float*>(m.pData), vertices, fam, p);
                ctx->Unmap(p.staging, 0);
                p.captureLogged = true;
                // dist: the anchor distance of the vertices the shader moved, as the game made them -> after the move ("none" when it moved nothing)
                char dist[96];
                if (c.inWindow)
                    std::snprintf(dist, sizeof(dist), "[%.2f,%.2f]->[%.2f,%.2f]", static_cast<double>(c.beforeMin), static_cast<double>(c.beforeMax), static_cast<double>(c.afterMin),
                                  static_cast<double>(c.afterMax));
                else
                    std::snprintf(dist, sizeof(dist), "none");
                Log::get().note("srv turret hud capture: family=%s eye=%d draw=%u so_prims=%llu needed=%llu vertices=%u in_window=%u w=[%.3f,%.3f] ndc_x=[%.3f,%.3f] "
                                "ndc_y=[%.3f,%.3f] dist=%s a=(%.3f,%.3f,%.3f) zfix=%u gap=%.2f",
                                fam.name, eye, p.draw, static_cast<unsigned long long>(written), static_cast<unsigned long long>(so.PrimitivesStorageNeeded), c.vertices,
                                c.inWindow, static_cast<double>(c.wMin), static_cast<double>(c.wMax), static_cast<double>(c.ndcXMin), static_cast<double>(c.ndcXMax),
                                static_cast<double>(c.ndcYMin), static_cast<double>(c.ndcYMax), dist, static_cast<double>(p.params.ax), static_cast<double>(p.params.ay),
                                static_cast<double>(p.params.az), c.zFixed, static_cast<double>(p.params.gap));
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

// Once a frame, the radar disc's extras: the samples that passed, and the two registers, logged together; given up on after kProbeGiveUpFrames.
void probeReadDisc(ID3D11DeviceContext* ctx, int eye) {
    Probe& p = g_probe[eye][kDiscFamily];
    if (!p.discPending || !ctx) return;
    uint64_t samples = 0;
    HRESULT hr = S_FALSE;
    const bool ran = guarded("srv.turret.probe.disc.data", [&] { hr = ctx->GetData(p.occlusion, &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH); });
    if (!ran || FAILED(hr)) {
        p.discPending = false;
        st::release(p.occlusion);   // made again at the next sample
        return;
    }
    if (hr == S_OK) {
        float regs[2][4] = {};
        bool have[2] = {false, false};
        if ((p.discCopied[0] || p.discCopied[1]) && p.cbStage) {
            D3D11_MAPPED_SUBRESOURCE m{};
            HRESULT mh = E_FAIL;
            const bool mapped = guarded("srv.turret.probe.disc.map", [&] { mh = ctx->Map(p.cbStage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m); });
            if (mapped && mh == DXGI_ERROR_WAS_STILL_DRAWING) {   // the copy is not done: try again next frame
                if (++p.discWaited <= kProbeGiveUpFrames) return;
            } else if (mapped && SUCCEEDED(mh) && m.pData) {
                std::memcpy(regs, m.pData, sizeof(regs));
                ctx->Unmap(p.cbStage, 0);
                have[0] = p.discCopied[0];
                have[1] = p.discCopied[1];
            }
        }
        char alpha[24], intensity[24];
        if (have[0]) std::snprintf(alpha, sizeof(alpha), "%.4f", static_cast<double>(regs[0][3])); else std::snprintf(alpha, sizeof(alpha), "n/a");
        if (have[1]) std::snprintf(intensity, sizeof(intensity), "%.4f", static_cast<double>(regs[1][1])); else std::snprintf(intensity, sizeof(intensity), "n/a");
        Log::get().note("srv turret hud disc: family=%s eye=%d draw=%u issues=%u samples_passed=%llu alpha=%s intensity=%s (cb2[11].w, cb1[90].y)", st::kFamilies[kDiscFamily].name, eye,
                        p.discDraw, p.discIssues, static_cast<unsigned long long>(samples), alpha, intensity);
        p.discPending = false;
    } else if (++p.discWaited > kProbeGiveUpFrames) {
        p.discPending = false;
        st::release(p.occlusion);   // a query that never resolved is made again
    }
}

// ---- THE UNMOVED FAMILY DRAWS ---------------------------------------------------------------------------------------------------------------------------------------
// Counted per family (the extra row: unknown) and route (srv_turret_math.h UnmovedRoute), from any thread; the 10 s line spells the window's counts, and the first of each route is said once. A
// pureDrawReissue call site is said once for each distinct return address, up to kReissueSitesMax.
void siteText(const void* site, char (&out)[160]) {
    out[0] = 0;
    if (!site) return;
    HMODULE module = nullptr;
    char path[MAX_PATH] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(site), &module) && module &&
        GetModuleFileNameA(module, path, MAX_PATH)) {
        const char* name = std::strrchr(path, '\\');
        std::snprintf(out, sizeof(out), ", called from %s+0x%llX", name ? name + 1 : path, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(site) - reinterpret_cast<uintptr_t>(module)));
    } else {
        std::snprintf(out, sizeof(out), ", called from 0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(site)));
    }
}

// ---- THE FIRST SCOPE'S STATE ----------------------------------------------------------------------------------------------------------------------------------------
// Whether the depth and stencil tests, the blend or the target keep a family's pixels from the eye image is not in the draw's numbers. At each family's first matched draw, every issue the
// scope makes (the game's own, the UI layer's, the interface depth's; up to kNoteIssues) says what it ran with: whether the target is the one the game bound at the start of the scope or another
// (the UI layer's), its size and format, the depth-stencil state and the blend state of render target 0.
constexpr uint32_t kNoteIssues = 8;
bool g_stateNoted[kFamilyN] = {};
int g_noteFamily = -1, g_noteEye = -1;
uint32_t g_noteIssue = 0;
const ID3D11DeviceContext* g_noteCtx = nullptr;
const void* g_noteGameTarget = nullptr;   // identity only: the resource the game's first render target of the scope was

void noteBegin(ID3D11DeviceContext* ctx, int family, int eye) {
    g_stateNoted[family] = true;
    g_noteFamily = family;
    g_noteEye = eye;
    g_noteIssue = 0;
    g_noteCtx = ctx;
    g_noteGameTarget = nullptr;
    guarded("srv.turret.note.begin", [&] {
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv) {
            ID3D11Resource* res = nullptr;
            rtv->GetResource(&res);
            g_noteGameTarget = res;
            if (res) res->Release();
            rtv->Release();
        }
    });
    detail::g_srvTurretHudCounting.store(true, std::memory_order_relaxed);
}
void noteEnd() {
    if (g_noteFamily < 0) return;
    g_noteFamily = -1;
    g_noteCtx = nullptr;
    g_noteGameTarget = nullptr;
    if (!g_sample) detail::g_srvTurretHudCounting.store(false, std::memory_order_relaxed);
}
void noteState(ID3D11DeviceContext* ctx) {
    guarded("srv.turret.note.state", [&] {
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        const char* target = "none";
        UINT w = 0, h = 0;
        UINT fmt = 0;
        if (rtv) {
            ID3D11Resource* res = nullptr;
            rtv->GetResource(&res);
            target = static_cast<const void*>(res) == g_noteGameTarget ? "game" : "other";
            if (res) {
                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
                    D3D11_TEXTURE2D_DESC td{};
                    tex->GetDesc(&td);
                    w = td.Width;
                    h = td.Height;
                    fmt = static_cast<UINT>(td.Format);
                    tex->Release();
                }
                res->Release();
            }
            rtv->Release();
        }
        ID3D11DepthStencilState* dss = nullptr;
        UINT stencilRef = 0;
        ctx->OMGetDepthStencilState(&dss, &stencilRef);
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;   // the D3D11 defaults a null state means
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS;
        dd.StencilReadMask = dd.StencilWriteMask = 0xFF;
        dd.FrontFace = dd.BackFace = D3D11_DEPTH_STENCILOP_DESC{D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
        if (dss) {
            dss->GetDesc(&dd);
            dss->Release();
        }
        ID3D11BlendState* bs = nullptr;
        float factor[4] = {};
        UINT sampleMask = 0;
        ctx->OMGetBlendState(&bs, factor, &sampleMask);
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (bs) {
            bs->GetDesc(&bd);
            bs->Release();
        }
        const D3D11_RENDER_TARGET_BLEND_DESC& rt = bd.RenderTarget[0];
        ID3D11RasterizerState* rs = nullptr;
        ctx->RSGetState(&rs);
        D3D11_RASTERIZER_DESC rd{};   // the D3D11 defaults a null state means
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_BACK;
        rd.DepthClipEnable = TRUE;
        if (rs) {
            rs->GetDesc(&rd);
            rs->Release();
        }
        UINT rectCount = 0;
        ctx->RSGetScissorRects(&rectCount, nullptr);
        D3D11_RECT rects[st::Scissor::kMax] = {};
        UINT take = rectCount > st::Scissor::kMax ? st::Scissor::kMax : rectCount;
        if (take) ctx->RSGetScissorRects(&take, rects);
        UINT viewports = 1;
        D3D11_VIEWPORT vp{};
        ctx->RSGetViewports(&viewports, &vp);
        Log::get().note("srv turret hud state: family=%s eye=%d issue=%u target=%s %ux%u format=%u depth_view=%s | depth: enable=%d func=%d write=%d | stencil: enable=%d ref=%u read=0x%02X "
                        "write=0x%02X front[func,pass,fail,zfail]=%d,%d,%d,%d | blend rt0: enable=%d src=%d dst=%d op=%d srcA=%d dstA=%d opA=%d mask=0x%X | raster: scissor=%d rects=%u first=[%ld,%ld,%ld,%ld] "
                        "depth_clip=%d cull=%d front_ccw=%d | viewport: x=%.0f y=%.0f w=%.0f h=%.0f",
                        st::kFamilies[g_noteFamily].name, g_noteEye, g_noteIssue, target, w, h, fmt, dsv ? "yes" : "no", static_cast<int>(dd.DepthEnable), static_cast<int>(dd.DepthFunc),
                        static_cast<int>(dd.DepthWriteMask), static_cast<int>(dd.StencilEnable), stencilRef, static_cast<unsigned>(dd.StencilReadMask), static_cast<unsigned>(dd.StencilWriteMask),
                        static_cast<int>(dd.FrontFace.StencilFunc), static_cast<int>(dd.FrontFace.StencilPassOp), static_cast<int>(dd.FrontFace.StencilFailOp),
                        static_cast<int>(dd.FrontFace.StencilDepthFailOp), static_cast<int>(rt.BlendEnable), static_cast<int>(rt.SrcBlend), static_cast<int>(rt.DestBlend), static_cast<int>(rt.BlendOp),
                        static_cast<int>(rt.SrcBlendAlpha), static_cast<int>(rt.DestBlendAlpha), static_cast<int>(rt.BlendOpAlpha), static_cast<unsigned>(rt.RenderTargetWriteMask),
                        static_cast<int>(rd.ScissorEnable), rectCount, take ? rects[0].left : 0L, take ? rects[0].top : 0L, take ? rects[0].right : 0L, take ? rects[0].bottom : 0L,
                        static_cast<int>(rd.DepthClipEnable), static_cast<int>(rd.CullMode), static_cast<int>(rd.FrontCounterClockwise), static_cast<double>(vp.TopLeftX),
                        static_cast<double>(vp.TopLeftY), static_cast<double>(vp.Width), static_cast<double>(vp.Height));
        if (dsv) dsv->Release();
    });
}

// ---- THE HOLOGRAM'S SOURCE (TEMPORARY instrument) -------------------------------------------------------------------------------------------------------------------------
// What the eye sees of the radar (its dark fill, its yellow rim, the Scorpion's outline) is not the disc DF3503CD draws: it is the hologram billboard, vs 5559BD94 with ps EA02FAC2, whose
// pixel shader draws a PICTURE it reads at the billboard's own UV (the fill where t4 is under 1000, the outline from an edge filter over t4, the colours from an ID in t5, more from t6). That
// picture is made earlier in the frame by another pass, from the game's own camera, so the moved billboard shows the disc as it looked from where it was (under the chin, cut by that
// pass's near plane) at the moved place. This finds that pass. Every kHoloEveryMs, one frame's 5559BD94 draws (both pixel shaders, both eyes) say what they read at t0..t7, and the
// render or depth targets among t2..t7 are watched (t0 is the exposure and t1 the scene's linear depth: the scene's own); in the next frame every owner-context draw with one of them bound
// as a render target or the depth target is a WRITER, and says its shaders, its draw, its targets and its first viewport. The vertex constants of the first billboards and writers (b0 rows
// 0..15, b1 rows 270..279 -- the camera block the billboard reads: the view-projection columns 270..273, the camera 275, its right, up and forward 277..279 -- and b2 rows 0..15) are copied
// to a staging buffer at the draw and logged when the GPU has them. A draw the game makes through an indirect or auto draw, a compute pass or a copy is not seen; zero writers says so.
// Nothing here changes a draw.
constexpr uint64_t kHoloEveryMs = 15000;
constexpr uint32_t kHoloFirstWatchedSlot = 2, kHoloSlots = 8;
constexpr uint32_t kHoloTexMax = 12, kHoloBillboardsMax = 6, kHoloWritersMax = 10, kHoloEntries = kHoloBillboardsMax + kHoloWritersMax;
constexpr uint32_t kHoloBuffers = 3;
constexpr uint32_t kHoloSlot[kHoloBuffers] = {0, 1, 2}, kHoloFirstRow[kHoloBuffers] = {0, 270, 0}, kHoloRows[kHoloBuffers] = {16, 10, 16};
constexpr uint32_t kHoloEntryRows = 16 + 10 + 16;
constexpr UINT kHoloEntryBytes = kHoloEntryRows * 16;
constexpr uint32_t kHoloWaitFrames = 8;
constexpr uint32_t kHoloWriterLinesMax = 24, kHoloPairsMax = 24;   // writer lines a round; distinct writer shader pairs tallied a round
constexpr int kHoloFamilyA = 7, kHoloFamilyB = 8;
static_assert(st::kFamilies[kHoloFamilyA].vs == 0x5559BD94B6852E83ull && st::kFamilies[kHoloFamilyB].vs == 0x5559BD94B6852E83ull, "the hologram billboards are families 7 and 8");
static_assert(kHoloRows[0] + kHoloRows[1] + kHoloRows[2] == kHoloEntryRows, "an entry holds the three buffers' rows");

struct HoloTex {
    const void* res = nullptr;   // identity only
    UINT w = 0, h = 0, fmt = 0, bind = 0;
};
struct HoloEntry {
    bool writer = false;
    int family = -1, eye = -1;   // a billboard's family and eye
    uint32_t ordinal = 0;        // ... and its place among the frame's draws of its family and eye
    uint64_t vs = 0, ps = 0;     // a writer's
    char kind = 0;
    UINT count = 0, instances = 0;
    uint32_t rows[kHoloBuffers] = {};               // the rows copied of each buffer (from kHoloFirstRow)
    UINT cbFirst[kHoloBuffers] = {}, cbCount[kHoloBuffers] = {};   // the range the game bound each with (0, 0: the whole buffer, or none)
};
struct HoloWatch {
    enum class Phase : uint8_t { kIdle, kCollect, kWatch, kRead };
    Phase phase = Phase::kIdle;
    ID3D11Device* device = nullptr;   // identity: the staging buffer's
    ID3D11Buffer* stage = nullptr;
    bool failed = false;              // the staging buffer could not be made on this device: the constants are not copied (the rest still runs)
    uint64_t lastMs = 0;
    uint32_t round = 0, frame = 0, waited = 0;
    HoloTex tex[kHoloTexMax];
    uint32_t texN = 0, billboards = 0;
    HoloEntry entry[kHoloEntries];
    uint32_t entryN = 0, writers = 0, draws = 0;
    struct Pair {
        uint64_t vs = 0, ps = 0;
        int tex = -1;    // the first watched texture it wrote
        uint32_t n = 0;
    };
    Pair pairs[kHoloPairsMax];   // every writer's shader pair, counted, so a scene-sized target's many writers cannot hide the picture's
    uint32_t pairN = 0, pairsDropped = 0;
};
HoloWatch g_holo;

// A texture's size, format and bind flags, from a view's resource (false: not a 2D texture, or a call faulted).
bool holoDescribe(ID3D11Resource* res, HoloTex& out) {
    out = HoloTex{};
    if (!res) return false;
    out.res = res;
    bool ok = false;
    guarded("srv.turret.holo.desc", [&] {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            out.w = td.Width;
            out.h = td.Height;
            out.fmt = static_cast<UINT>(td.Format);
            out.bind = td.BindFlags;
            tex->Release();
            ok = true;
        }
    });
    return ok;
}
int holoTexIndex(const void* res) {
    for (uint32_t i = 0; i < g_holo.texN; ++i)
        if (g_holo.tex[i].res == res) return static_cast<int>(i);
    return -1;
}
unsigned holoShortId(const void* res) { return static_cast<unsigned>(reinterpret_cast<uintptr_t>(res) & 0xFFFFFFu); }

bool holoEnsureStage(ID3D11DeviceContext* ctx) {
    if (g_holo.failed) return false;
    ID3D11Device* d = nullptr;
    if (!guarded("srv.turret.holo.device", [&] { ctx->GetDevice(&d); }) || !d) {
        st::release(d);
        return false;
    }
    if (d != g_holo.device) {
        st::release(g_holo.stage);
        g_holo.device = d;
    }
    HRESULT hr = S_OK;
    if (!g_holo.stage) {
        guarded("srv.turret.holo.stage", [&] {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kHoloEntries * kHoloEntryBytes;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = d->CreateBuffer(&bd, nullptr, &g_holo.stage);
        });
        if (!g_holo.stage) {
            g_holo.failed = true;
            Log::get().note("srv turret holo: the staging buffer for the constants could not be made (0x%08X); the sources and writers are still said, without their constants.", static_cast<unsigned>(hr));
        }
    }
    st::release(d);
    return g_holo.stage != nullptr;
}

// Rows [first, first + rows) of the vertex-stage buffer at `slot`, counted from the first constant the game bound it with, copied into the stage at byte `to`, clipped to the buffer and to the
// bound range. Returns the rows copied (0: nothing bound, nothing in range, or a fault).
uint32_t holoCopyVsRows(ID3D11DeviceContext* ctx, ID3D11Buffer* stage, uint32_t slot, uint32_t first, uint32_t rows, UINT to, UINT& boundFirst, UINT& boundCount) {
    boundFirst = boundCount = 0;
    if (!stage) return 0;
    st::ExtraCb cb;
    if (!st::readGameCb(ctx, slot, cb, false)) return 0;
    boundFirst = cb.ranged ? cb.first : 0;
    boundCount = cb.ranged ? cb.count : 0;
    D3D11_BUFFER_DESC bd{};
    if (!guarded("srv.turret.holo.cbdesc", [&] { cb.buffer->GetDesc(&bd); })) return 0;
    const uint64_t base = static_cast<uint64_t>(boundFirst) + first;
    const uint64_t inBuffer = bd.ByteWidth / 16u;
    if (base >= inBuffer || (cb.ranged && cb.count && first >= cb.count)) return 0;
    uint64_t n = rows;
    if (base + n > inBuffer) n = inBuffer - base;
    if (cb.ranged && cb.count && first + n > cb.count) n = cb.count - first;
    const D3D11_BOX box{static_cast<UINT>(base * 16u), 0, 0, static_cast<UINT>((base + n) * 16u), 1, 1};
    if (!guarded("srv.turret.holo.copy", [&] { rawCopyRegion(ctx, stage, to, cb.buffer, &box); })) return 0;
    return static_cast<uint32_t>(n);
}
void holoCopyConstants(ID3D11DeviceContext* ctx, HoloEntry& e, uint32_t index) {
    if (!holoEnsureStage(ctx)) return;
    UINT to = index * kHoloEntryBytes;
    for (uint32_t b = 0; b < kHoloBuffers; ++b) {
        e.rows[b] = holoCopyVsRows(ctx, g_holo.stage, kHoloSlot[b], kHoloFirstRow[b], kHoloRows[b], to, e.cbFirst[b], e.cbCount[b]);
        to += kHoloRows[b] * 16u;
    }
}

// srvTurretHudBegin (and radarCorrectionBegin), at each draw of a hologram billboard family while a round collects: what it reads at t0..t7; the render and depth targets among t2..t7 are
// watched in the next frame.
void holoCollect(ID3D11DeviceContext* ctx, int family, int eye, uint32_t ordinal) {
    if (g_holo.phase != HoloWatch::Phase::kCollect || !ctx) return;
    ID3D11ShaderResourceView* srvs[kHoloSlots] = {};
    if (!guarded("srv.turret.holo.srvs", [&] { ctx->PSGetShaderResources(0, kHoloSlots, srvs); })) return;
    char line[1100];
    int len = std::snprintf(line, sizeof(line), "srv turret holo source: round %u frame %u %s eye=%d draw=%u reads", g_holo.round, g_frame, st::kFamilies[family].name, eye, ordinal);
    for (uint32_t s = 0; s < kHoloSlots; ++s) {
        if (!srvs[s]) continue;
        ID3D11Resource* res = nullptr;
        guarded("srv.turret.holo.res", [&] { srvs[s]->GetResource(&res); });
        HoloTex t;
        const bool tex2d = holoDescribe(res, t);
        int index = res ? holoTexIndex(res) : -1;
        const bool target = tex2d && (t.bind & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) != 0;
        if (index < 0 && target && s >= kHoloFirstWatchedSlot && g_holo.texN < kHoloTexMax) {
            index = static_cast<int>(g_holo.texN);
            g_holo.tex[g_holo.texN++] = t;
        }
        if (len > 0 && len < static_cast<int>(sizeof(line)) - 96) {
            if (tex2d)
                len += std::snprintf(line + len, sizeof(line) - len, " t%u=%06X %ux%u fmt %u bind 0x%X%s", s, holoShortId(res), t.w, t.h, t.fmt, t.bind,
                                     index >= 0 ? " WATCHED" : "");
            else
                len += std::snprintf(line + len, sizeof(line) - len, " t%u=%06X (not a 2D texture)", s, holoShortId(res));
        }
        st::release(res);
        guarded("srv.turret.holo.srvrelease", [&] { srvs[s]->Release(); });
    }
    Log::get().note("%s", line);
    if (g_holo.billboards < kHoloBillboardsMax && g_holo.entryN < kHoloEntries) {
        HoloEntry& e = g_holo.entry[g_holo.entryN];
        e = HoloEntry{};
        e.family = family;
        e.eye = eye;
        e.ordinal = ordinal;
        holoCopyConstants(ctx, e, g_holo.entryN);
        ++g_holo.entryN;
        ++g_holo.billboards;
    }
}

// One line per buffer of an entry, from the mapped stage.
void holoLogEntry(const HoloEntry& e, uint32_t index, const float* data) {
    char head[160];
    if (e.writer)
        std::snprintf(head, sizeof(head), "srv turret holo writer constants: round %u #%u vs=%016llX ps=%016llX", g_holo.round, index, static_cast<unsigned long long>(e.vs),
                      static_cast<unsigned long long>(e.ps));
    else
        std::snprintf(head, sizeof(head), "srv turret holo billboard constants: round %u #%u %s eye=%d draw=%u", g_holo.round, index, st::kFamilies[e.family].name, e.eye, e.ordinal);
    const float* row = data + static_cast<size_t>(index) * kHoloEntryRows * 4;
    for (uint32_t b = 0; b < kHoloBuffers; ++b) {
        char line[1100];
        int len = std::snprintf(line, sizeof(line), "%s b%u (bound first %u count %u) rows %u..:", head, kHoloSlot[b], e.cbFirst[b], e.cbCount[b], kHoloFirstRow[b]);
        if (!e.rows[b] && len > 0) std::snprintf(line + len, sizeof(line) - len, " n/a");
        for (uint32_t r = 0; r < e.rows[b] && len > 0 && len < static_cast<int>(sizeof(line)) - 64; ++r) {
            const float* v = row + 4 * r;
            len += std::snprintf(line + len, sizeof(line) - len, " (%.5g,%.5g,%.5g,%.5g)", static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]),
                                 static_cast<double>(v[3]));
        }
        Log::get().note("%s", line);
        row += kHoloRows[b] * 4;
    }
}

// The frame boundary: a round collects for one frame, watches the next, then reads the constants back.
void holoBoundary(ID3D11DeviceContext* ctx) {
    const uint64_t now = GetTickCount64();
    switch (g_holo.phase) {
        case HoloWatch::Phase::kIdle:
            if (detail::g_srvTurretHudWanted.load(std::memory_order_relaxed) && now - g_holo.lastMs >= kHoloEveryMs) {
                g_holo.texN = g_holo.billboards = g_holo.entryN = g_holo.writers = g_holo.draws = g_holo.pairN = g_holo.pairsDropped = 0;
                ++g_holo.round;
                g_holo.phase = HoloWatch::Phase::kCollect;
            }
            break;
        case HoloWatch::Phase::kCollect:
            g_holo.lastMs = now;
            if (!g_holo.texN) {
                Log::get().note("srv turret holo source: round %u: %s", g_holo.round,
                                g_holo.billboards ? "no 5559BD94 draw of the frame read a render or depth target at t2..t7; nothing to watch" : "no 5559BD94 draw in the frame");
                g_holo.phase = g_holo.entryN ? HoloWatch::Phase::kRead : HoloWatch::Phase::kIdle;
                g_holo.waited = 0;
            } else {
                g_holo.frame = g_frame;
                g_holo.phase = HoloWatch::Phase::kWatch;
                detail::g_srvTurretHoloWatching.store(true, std::memory_order_relaxed);
            }
            break;
        case HoloWatch::Phase::kWatch: {
            detail::g_srvTurretHoloWatching.store(false, std::memory_order_relaxed);
            char pairs[900];
            int len = 0;
            pairs[0] = 0;
            for (uint32_t k = 0; k < g_holo.pairN && len >= 0 && len < static_cast<int>(sizeof(pairs)) - 64; ++k) {
                const auto& q = g_holo.pairs[k];
                len += std::snprintf(pairs + len, sizeof(pairs) - len, " %016llX/%016llX x%u (#%d %06X)", static_cast<unsigned long long>(q.vs), static_cast<unsigned long long>(q.ps), q.n,
                                     q.tex, q.tex >= 0 ? holoShortId(g_holo.tex[q.tex].res) : 0u);
            }
            Log::get().note("srv turret holo watch: round %u frame %u: %u of %u owner-context draws had one of the %u watched textures bound as a render or depth target (%u with their "
                            "constants below)%s", g_holo.round, g_holo.frame, g_holo.writers, g_holo.draws, g_holo.texN, g_holo.entryN - g_holo.billboards,
                            g_holo.writers ? "" : "; none: the picture is made by an indirect or auto draw, a compute pass, a copy or another context");
            if (g_holo.pairN) Log::get().note("srv turret holo watch: round %u writer pairs vs/ps xN (first watched texture):%s%s", g_holo.round, pairs, g_holo.pairsDropped ? " (more not listed)" : "");
            g_holo.phase = HoloWatch::Phase::kRead;
            g_holo.waited = 0;
            break;
        }
        case HoloWatch::Phase::kRead: {
            if (!g_holo.entryN || !g_holo.stage || !ctx) {
                g_holo.phase = HoloWatch::Phase::kIdle;
                break;
            }
            D3D11_MAPPED_SUBRESOURCE m{};
            HRESULT hr = E_FAIL;
            const bool ran = guarded("srv.turret.holo.map", [&] { hr = ctx->Map(g_holo.stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m); });
            if (ran && hr == DXGI_ERROR_WAS_STILL_DRAWING && ++g_holo.waited <= kHoloWaitFrames) break;
            if (ran && SUCCEEDED(hr) && m.pData) {
                for (uint32_t i = 0; i < g_holo.entryN; ++i) holoLogEntry(g_holo.entry[i], i, static_cast<const float*>(m.pData));
                guarded("srv.turret.holo.unmap", [&] { ctx->Unmap(g_holo.stage, 0); });
            } else {
                Log::get().note("srv turret holo: round %u: the constants could not be read back (0x%08X)", g_holo.round, static_cast<unsigned>(hr));
            }
            g_holo.phase = HoloWatch::Phase::kIdle;
            break;
        }
    }
}

// ---- THE RADAR PICTURE'S CAMERA (TEMPORARY instrument) --------------------------------------------------------------------------------------------------------------------
// V0, the camera the radar's terrain schematic (vs 4EDE4BC9) draws the disc's picture from, decoded from that draw's cb0 rows 4..11 (srv_turret_math.h radarCamFrom). The game sets it on the CPU,
// by a rule not yet found in the binary, and it did not follow the head in the flight of 707c0ab5. This logs every move of it against the head and the camera back, so one flight says when it
// updates (view-on, a camera key, a recentre, a head distance) and whether EDVR's camera back reaches it. While the Scorpion's turret view is on, the frame's first such draw has the eight rows
// copied to a staging buffer (a ring of kRadarCamRing, read back without stalling, given up after kRadarCamWait frames). A camera that differs from the last one said (its schematic origin by
// more than 2 mm, its forward by more than 0.05 deg) is logged, at most kRadarCamLinesPerSecond lines a second (the rest counted into the next line). The view's end says how many were read;
// zero says the draw never reached this path (it runs only while one of the turret keys is above 0, the draw gate's term). Nothing here changes a draw.
constexpr uint64_t kSchematicTerrainVs = 0x4EDE4BC9003BE352ull;
constexpr uint32_t kRadarCamRing = 3, kRadarCamRows = 8, kRadarCamWait = 8, kRadarCamLinesPerSecond = 4;
struct RadarCamWatch {
    ID3D11Device* device = nullptr;   // identity: the stages'
    ID3D11Buffer* stage[kRadarCamRing] = {};
    bool pending[kRadarCamRing] = {};
    uint32_t frameOf[kRadarCamRing] = {}, waited[kRadarCamRing] = {};
    uint32_t next = 0, lastCopyFrame = 0xFFFFFFFFu;
    bool failed = false, on = false, have = false;
    st::RadarCam last;
    uint32_t lastFrame = 0, draws = 0, reads = 0, moves = 0, saidThisSecond = 0, suppressed = 0;
    uint32_t lastReadFrame = 0;   // the frame of the last good read, moved or not (the radar correction's content depth is taken only while it is fresh)
    uint64_t secondMs = 0;
};
RadarCamWatch g_radarCam;

bool radarCamEnsure(ID3D11DeviceContext* ctx) {
    if (g_radarCam.failed) return false;
    ID3D11Device* d = nullptr;
    if (!guarded("srv.turret.radarcam.device", [&] { ctx->GetDevice(&d); }) || !d) {
        st::release(d);
        return false;
    }
    if (d != g_radarCam.device) {
        for (auto& s : g_radarCam.stage) st::release(s);
        for (auto& p : g_radarCam.pending) p = false;
        g_radarCam.device = d;
    }
    HRESULT hr = S_OK;
    for (auto& s : g_radarCam.stage) {
        if (s) continue;
        guarded("srv.turret.radarcam.stage", [&] {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kRadarCamRows * 16u;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = d->CreateBuffer(&bd, nullptr, &s);
        });
        if (!s) {
            g_radarCam.failed = true;
            Log::get().note("srv turret radar camera: the staging buffers could not be made (0x%08X); the instrument stands down.", static_cast<unsigned>(hr));
            break;
        }
    }
    st::release(d);
    return !g_radarCam.failed;
}

void radarCamSay(const st::RadarCam& rc, uint32_t frame, uint32_t copiedAt) {
    uint64_t seq = 0;
    float head[12] = {}, eyeToHead[2][12] = {};
    const bool haveHead = nativeTemporalHeadPose(&seq, head, eyeToHead);
    char what[160];
    if (!g_radarCam.have) {
        std::snprintf(what, sizeof(what), "first seen");
    } else {
        const double d[3] = {rc.centre[0] - g_radarCam.last.centre[0], rc.centre[1] - g_radarCam.last.centre[1], rc.centre[2] - g_radarCam.last.centre[2]};
        std::snprintf(what, sizeof(what), "moved by (%.1f,%.1f,%.1f) mm and turned %.2f deg after %u frames unchanged", d[0] * 1000.0, d[1] * 1000.0, d[2] * 1000.0,
                      st::angleBetweenDeg(rc.fwd, g_radarCam.last.fwd), frame - g_radarCam.lastFrame);
    }
    char suppressed[48] = "";
    if (g_radarCam.suppressed) std::snprintf(suppressed, sizeof(suppressed), " (%u moves not said before this)", g_radarCam.suppressed);
    Log::get().note("srv turret radar camera: %s at frame %u (copied at frame %u): the schematic's origin at (%.3f,%.3f,%.3f) m in the camera (x right, y up, z forward), forward (%.4f,%.4f,%.4f), "
                    "window tan x [%.3f,%.3f] y [%.3f,%.3f], scale %.4f; head %s(%.3f,%.3f,%.3f) seated, camera back %d cm%s",
                    what, frame, copiedAt, rc.centre[0], rc.centre[1], rc.centre[2], rc.fwd[0], rc.fwd[1], rc.fwd[2], rc.tanX[0], rc.tanX[1], rc.tanY[0], rc.tanY[1], rc.scale,
                    haveHead ? "" : "(none) ", static_cast<double>(head[3]), static_cast<double>(head[7]), static_cast<double>(head[11]), g_backCm.load(std::memory_order_relaxed), suppressed);
    g_radarCam.suppressed = 0;
}

void radarCamBoundary(ID3D11DeviceContext* ctx) {
    const bool on = detail::g_srvTurretRadarCamOn.load(std::memory_order_relaxed);
    if (on != g_radarCam.on) {
        if (!on) {
            Log::get().note("srv turret radar camera: view off: %u picture-camera draws copied, %u read, %u moves%s", g_radarCam.draws, g_radarCam.reads, g_radarCam.moves,
                            g_radarCam.reads ? "" : " -- NEVER SEEN: the radar's picture draw (vs 4EDE4BC9) did not reach this path (all turret keys at 0, or the radar was not drawn)");
        }
        g_radarCam.on = on;
        g_radarCam.have = false;
        g_radarCam.draws = g_radarCam.reads = g_radarCam.moves = g_radarCam.suppressed = 0;
    }
    if (!ctx) return;
    const uint64_t now = GetTickCount64();
    if (now - g_radarCam.secondMs >= 1000) {
        g_radarCam.secondMs = now;
        g_radarCam.saidThisSecond = 0;
    }
    for (uint32_t k = 0; k < kRadarCamRing; ++k) {
        if (!g_radarCam.pending[k] || !g_radarCam.stage[k]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = E_FAIL;
        const bool ran = guarded("srv.turret.radarcam.map", [&] { hr = ctx->Map(g_radarCam.stage[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m); });
        if (ran && hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            if (++g_radarCam.waited[k] > kRadarCamWait) g_radarCam.pending[k] = false;
            continue;
        }
        g_radarCam.pending[k] = false;
        if (!ran || FAILED(hr) || !m.pData) continue;
        float rows[kRadarCamRows * 4];
        std::memcpy(rows, m.pData, sizeof(rows));
        guarded("srv.turret.radarcam.unmap", [&] { ctx->Unmap(g_radarCam.stage[k], 0); });
        ++g_radarCam.reads;
        st::RadarCam rc;
        if (!st::radarCamFrom(rows, rows + 20, rc)) continue;   // rows 4..7 at 0, rows 9..11 at 20 (row 8 is not read)
        g_radarCam.lastReadFrame = g_frame;
        const bool moved = !g_radarCam.have || std::fabs(rc.centre[0] - g_radarCam.last.centre[0]) > 0.002 || std::fabs(rc.centre[1] - g_radarCam.last.centre[1]) > 0.002 ||
                           std::fabs(rc.centre[2] - g_radarCam.last.centre[2]) > 0.002 || st::angleBetweenDeg(rc.fwd, g_radarCam.last.fwd) > 0.05;
        if (!moved) continue;
        ++g_radarCam.moves;
        if (g_radarCam.saidThisSecond < kRadarCamLinesPerSecond) {
            ++g_radarCam.saidThisSecond;
            radarCamSay(rc, g_frame, g_radarCam.frameOf[k]);
        } else {
            ++g_radarCam.suppressed;
        }
        g_radarCam.last = rc;
        g_radarCam.have = true;
        g_radarCam.lastFrame = g_frame;
    }
}

// ---- the radar anchor moved in the game (TEMPORARY key fix.srv_turret_radar_anchor; srv_radar_anchor.h) ----------------------------------------------------------------------
// The frame tick arms the hooks the first time the Scorpion's turret view is on with the key asking for a move, publishes the offset every frame (zero off the view and in flat:
// the game's next setter call then gets its own placement) and says what the hooks did; the hooks never log. While the last setter call for the radar left our offset in its
// placement (added, or our own value fed back) the draw path leaves the two 5559BD94 billboard families to the game (g_anchorYield), so the picture the game now frames and places
// itself is not moved a second time. The setter is not called by EDVR: a key change reaches the game at its next call (the 10 s line counts the calls).
std::atomic<bool> g_anchorYield{false};
// CAMERA MODE (fix.srv_turret_radar_anchor = camera; srv_turret_math.h THE RADAR'S OWN CAMERA): set by the frame tick while the view is on in camera mode; the draw path then sends the radar's
// own picture billboards through the regular radar path and leaves every other 5559BD94 draw to the game (cameraLeaves).
std::atomic<bool> g_cameraBillboards{false};

// The turret's forward for the native move (render thread). While the anchor is wanted, eye 0's first draw of a panel or 5559BD94 family each frame gives a camera sample: the live
// camera's right, up and forward in the world (VS b1 rows 277..279) paired with eye 0's seated rotation of the same frame (the host's pose at the draw, as drawEyeFrame uses), which
// srv_turret_math.h turretForwardWorld turns into u, published to the setter hook with the host frame it belongs to.
//   * THE CPU PATH (the default since the flight of c1275025, whose 2-3 frame GPU readback made the disc lag the turret): the Unmap tee hands every write of the 5376-byte scene camera
//     block to srvTurretNoteCameraWrite, which keeps rows 277..279 by buffer with the host frame; at the draw the bound b1 buffer is looked up, and the rows its last write put there are
//     this draw's (written before it, on this thread). Used when written in this host frame and bound from its first constant.
//   * THE GPU PATH (the fallback, counted): the rows copied to a staging ring and read back at the frame boundary without stalling (given up after kDirWait frames).
constexpr uint32_t kDirRing = 3, kDirRows = 3, kDirFirstRow = st::kSceneRightRow, kDirWait = 8;
static_assert(st::kSceneRightRow == 277 && st::kSceneUpRow == 278 && st::kSceneForwardRow == 279, "the camera's right, up and forward rows");
struct DirWatch {
    ID3D11Device* device = nullptr;   // identity: the stages'
    ID3D11Buffer* stage[kDirRing] = {};
    bool pending[kDirRing] = {};
    uint32_t waited[kDirRing] = {};
    uint64_t orderOf[kDirRing] = {}, hostSeqOf[kDirRing] = {};
    int64_t qpcOf[kDirRing] = {};
    float headPitchOf[kDirRing] = {};
    double R[kDirRing][9] = {};
    uint32_t next = 0, lastCopyFrame = 0xFFFFFFFFu;
    uint64_t order = 0, publishedOrder = 0, publishedHostSeq = 0;
    bool failed = false, cpuNoted = false, gpuNoted = false;
};
DirWatch g_dir;
std::atomic<uint64_t> g_dirCopies{0}, g_dirReads{0}, g_dirRejects{0}, g_dirCpu{0};   // for the 10 s line (cumulative)
std::atomic<uint64_t> g_dirCpuMiss[4]{};   // why a sample went to the GPU path: no write of the bound buffer seen, bound past its first constant, written in another frame, refused rows
const char* const kCpuMissName[4] = {"the bound b1 buffer was not seen written", "b1 is bound past its first constant", "b1 was written in another frame", "its rows are not a camera"};
std::atomic<bool> g_anchorDirWanted{false};   // the frame tick's: the Scorpion's turret view is on, the key is not off and the setter hook is armed

// The scene camera block's writes (srvTurretNoteCameraWrite, the Unmap tee): rows 277..279 by buffer, with the host frame. Owner context's thread only.
constexpr uint32_t kCamWrites = 16, kCamRowsFloat = kDirFirstRow * 4, kCamRowsEnd = (kDirFirstRow + kDirRows) * 16;
struct CamWrite {
    const void* res = nullptr;
    float rows[12] = {};
    uint64_t hostSeq = 0;
    int64_t qpc = 0;   // when it was written: the sample's time for the setter's lead and residual
};
CamWrite g_camWrites[kCamWrites];
uint32_t g_camWriteNext = 0;
std::atomic<uint32_t> g_cameraWriteThread{0};   // the thread the scene camera block is written on (said once beside the setter's)

bool anchorDirEnsure(ID3D11DeviceContext* ctx) {
    if (g_dir.failed) return false;
    ID3D11Device* d = nullptr;
    if (!guarded("srv.turret.anchordir.device", [&] { ctx->GetDevice(&d); }) || !d) {
        st::release(d);
        return false;
    }
    if (d != g_dir.device) {
        for (auto& s : g_dir.stage) st::release(s);
        for (auto& p : g_dir.pending) p = false;
        g_dir.device = d;
    }
    HRESULT hr = S_OK;
    for (auto& s : g_dir.stage) {
        if (s) continue;
        guarded("srv.turret.anchordir.stage", [&] {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kDirRows * 16u;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = d->CreateBuffer(&bd, nullptr, &s);
        });
        if (!s) {
            g_dir.failed = true;
            Log::get().note("srv turret radar anchor: the staging buffers for the turret's direction could not be made (0x%08X); the GPU path is off, the CPU path stays.",
                            static_cast<unsigned>(hr));
            break;
        }
    }
    st::release(d);
    return !g_dir.failed;
}

// The CPU path's lookup: 0 with the rows when the bound b1 buffer's last write was seen this host frame from its first constant, else the reason (kCpuMissName's index + 1).
int anchorDirCpuRows(ID3D11DeviceContext* ctx, uint64_t hostSeq, float rows[12], uint32_t* bytes, int64_t* qpc) {
    st::ExtraCb cb;
    if (!st::readGameCb(ctx, 1, cb, false)) return 1;
    const void* res = static_cast<ID3D11Resource*>(cb.buffer);
    const CamWrite* w = nullptr;
    for (const CamWrite& c : g_camWrites)
        if (c.res == res) w = &c;
    if (!w) return 1;
    if (cb.ranged && cb.first != 0) return 2;
    if (w->hostSeq != hostSeq) return 3;
    std::memcpy(rows, w->rows, sizeof(w->rows));
    if (qpc) *qpc = w->qpc;
    D3D11_BUFFER_DESC bd{};
    if (bytes && guarded("srv.turret.anchordir.cbdesc", [&] { cb.buffer->GetDesc(&bd); })) *bytes = bd.ByteWidth;
    return 0;
}

// srvTurretHudBegin, eye 0, before anything else: the panel families and the 5559BD94 billboards bind the main camera block at VS b1.
void anchorDirSample(ID3D11DeviceContext* ctx, int family) {
    if (!g_anchorDirWanted.load(std::memory_order_relaxed) || g_dir.lastCopyFrame == g_frame) return;
    if (st::kFamilies[family].group != st::Group::kPanels && !st::radarAnchorLeavesFamily(family)) return;
    uint64_t poseSeq = 0;
    float head[12] = {}, eyeToHead[2][12] = {};
    double R[9], E[3];
    if (!nativeTemporalHeadPose(&poseSeq, head, eyeToHead) || !st::eyePoseOf(head, eyeToHead[0], R, E)) return;
    const float headPitch = static_cast<float>(st::headPitchDeg(R));
    float cpuRows[12];
    uint32_t bytes = 0;
    int64_t writtenAt = 0;
    int miss = anchorDirCpuRows(ctx, poseSeq, cpuRows, &bytes, &writtenAt);
    if (miss == 0) {
        float u[3];
        if (st::turretForwardWorld(cpuRows, cpuRows + 4, cpuRows + 8, R, u)) {
            g_dir.lastCopyFrame = g_frame;
            g_dir.publishedHostSeq = poseSeq;
            srvRadarAnchorPublishDirection(u, true, poseSeq, true, headPitch, writtenAt);
            g_dirCpu.fetch_add(1, std::memory_order_relaxed);
            if (!g_dir.cpuNoted) {
                g_dir.cpuNoted = true;
                Log::get().note("srv turret radar anchor: turret direction from the CPU: eye 0's VS b1 is the scene camera block (%u bytes), rows 277..279 read when the game wrote them in "
                                "host frame %llu; no GPU readback",
                                bytes, static_cast<unsigned long long>(poseSeq));
            }
            return;
        }
        miss = 4;
    }
    g_dirCpuMiss[miss - 1].fetch_add(1, std::memory_order_relaxed);
    if (!g_dir.gpuNoted) {
        g_dir.gpuNoted = true;
        Log::get().note("srv turret radar anchor: turret direction from the GPU readback for host frame %llu (%s); counted in the 10 s line",
                        static_cast<unsigned long long>(poseSeq), kCpuMissName[miss - 1]);
    }
    if (!anchorDirEnsure(ctx)) return;
    const uint32_t k = g_dir.next;
    if (g_dir.pending[k]) return;   // the ring is full: this frame is skipped
    UINT first = 0, count = 0;
    if (holoCopyVsRows(ctx, g_dir.stage[k], 1, kDirFirstRow, kDirRows, 0, first, count) != kDirRows) return;   // this draw's b1 is not the camera block: the next family's may be
    g_dir.lastCopyFrame = g_frame;
    g_dir.pending[k] = true;
    g_dir.waited[k] = 0;
    g_dir.orderOf[k] = ++g_dir.order;
    g_dir.hostSeqOf[k] = poseSeq;
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        g_dir.qpcOf[k] = now.QuadPart;   // the GPU path's sample time: the copy, right after the game's write
    }
    g_dir.headPitchOf[k] = headPitch;
    std::memcpy(g_dir.R[k], R, sizeof(R));
    g_dir.next = (k + 1) % kDirRing;
    g_dirCopies.fetch_add(1, std::memory_order_relaxed);
}

// srvTurretHudFrameBoundary: the GPU path's copies that are ready, the newest good one published unless the CPU path has published a later frame.
void anchorDirBoundary(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    float best[3] = {};
    uint64_t bestOrder = 0;
    uint32_t bestSlot = 0;
    for (uint32_t k = 0; k < kDirRing; ++k) {
        if (!g_dir.pending[k] || !g_dir.stage[k]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = E_FAIL;
        const bool ran = guarded("srv.turret.anchordir.map", [&] { hr = ctx->Map(g_dir.stage[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m); });
        if (ran && hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            if (++g_dir.waited[k] > kDirWait) g_dir.pending[k] = false;
            continue;
        }
        g_dir.pending[k] = false;
        if (!ran || FAILED(hr) || !m.pData) continue;
        float rows[kDirRows * 4];
        std::memcpy(rows, m.pData, sizeof(rows));
        guarded("srv.turret.anchordir.unmap", [&] { ctx->Unmap(g_dir.stage[k], 0); });
        g_dirReads.fetch_add(1, std::memory_order_relaxed);
        float u[3];
        if (!st::turretForwardWorld(rows, rows + 4, rows + 8, g_dir.R[k], u)) {
            g_dirRejects.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (g_dir.orderOf[k] > bestOrder) {
            bestOrder = g_dir.orderOf[k];
            bestSlot = k;
            std::memcpy(best, u, sizeof(best));
        }
    }
    if (bestOrder > g_dir.publishedOrder && g_dir.hostSeqOf[bestSlot] > g_dir.publishedHostSeq) {
        g_dir.publishedOrder = bestOrder;
        g_dir.publishedHostSeq = g_dir.hostSeqOf[bestSlot];
        srvRadarAnchorPublishDirection(best, true, g_dir.hostSeqOf[bestSlot], false, g_dir.headPitchOf[bestSlot], g_dir.qpcOf[bestSlot]);
    }
}

struct AnchorWatch {
    bool armTried = false;
    bool on = false;                // the view, as the last tick saw it
    bool asked = false;             // this stay published an offset that was not zero
    bool said = false;              // this stay's first add has been said
    bool warned = false;            // the feedback warning has been said
    bool yieldSeen = false;         // the anchor yield was on in this 10 s window (the correction line is said)
    bool threadsSaid = false;       // the setter's and the camera writes' threads have been said
    bool slotArmTried = false;      // camera mode: the slot hook's arm was asked for
    bool cameraSeen = false;        // camera mode was on with the view in this 10 s window (the slot camera's line is said)
    bool slotSaid[3][3] = {};       // camera mode: the first move of each kind and list has been said
    uint64_t winSlotCalls = 0, winSlotRadar[3][3] = {}, winSlotMoved = 0, winSlotTurret = 0, winSlotCamera = 0, winSlotFaults = 0;
    uint64_t stayMoved = 0, stayOurs = 0;   // the setter hook's counters when the stay began
    uint64_t winCalls = 0, winRadar = 0, winMoved = 0, winOurs = 0, winRestored = 0, winFaulted = 0, winChanges = 0, winUpdates = 0;   // ... when the 10 s window began
    uint64_t winDirected = 0, winFallback = 0, winNoU = 0, winNoAnchor = 0, winAnchorFaults = 0, winDirCopies = 0, winDirReads = 0, winDirRejects = 0;
    uint64_t winAgeSum = 0, winPaired = 0, winUnpaired = 0, winCpu = 0, winCpuMiss[4] = {};
    uint64_t winResN = 0, winResLagging = 0, winResLeading = 0, winLatN = 0, winHorizonN = 0, winHorizonSumUs = 0, winAgeHist[3] = {}, winGapN = 0, winGapSumUs = 0;
    int64_t winResSumMicro = 0, winLatSumMilli = 0;
    uint64_t dirLineMs = 0;         // when the last direction line was said
    st::RadarAnchorOffset offset;   // the last fallback published ((0, 0, s * F))
};
AnchorWatch g_anchorWatch;

// The direction line: what the last add was made of (the instrument a turret sweep reads: the anchor's rows stay with the vehicle, d turns with the turret in the anchor's axes).
void anchorDirLine(const RadarAnchorStats& a, st::RadarAnchorMode mode) {
    char rows[160];
    if (a.haveAnchor)
        std::snprintf(rows, sizeof(rows), "(%.3f,%.3f,%.3f)(%.3f,%.3f,%.3f)(%.3f,%.3f,%.3f)", static_cast<double>(a.anchorRows[0]), static_cast<double>(a.anchorRows[1]),
                      static_cast<double>(a.anchorRows[2]), static_cast<double>(a.anchorRows[3]), static_cast<double>(a.anchorRows[4]), static_cast<double>(a.anchorRows[5]),
                      static_cast<double>(a.anchorRows[6]), static_cast<double>(a.anchorRows[7]), static_cast<double>(a.anchorRows[8]));
    else
        std::snprintf(rows, sizeof(rows), "not read");
    const bool haveU = a.u[0] != 0.0f || a.u[1] != 0.0f || a.u[2] != 0.0f;
    const char* source = a.lastDirected ? "source vfunc +0x20" : !haveU ? "fallback: no u yet" : "fallback: the anchor's rows not read or refused";
    const double dist = std::sqrt(double(a.d[0]) * a.d[0] + double(a.d[1]) * a.d[1] + double(a.d[2]) * a.d[2]);
    const float* along = a.lastDirected ? a.uAnchor : nullptr;
    const float fallbackZ[3] = {0.0f, 0.0f, 1.0f};
    if (!along) along = fallbackZ;
    // The sample's age and source, and the pitch pair that tells gun elevation (u's pitch on its own) from a pairing error (u's pitch following the head's). No lead: the setter uses the
    // sample as it is and the draw-time correction takes up the rest.
    char timing[360] = "";
    if (a.lastDirected)
        std::snprintf(timing, sizeof(timing),
                      "; u age %llu frames (setter frame %llu, camera sample frame %llu), source %s, anchor %s; aim relative to the vehicle yaw %.1f pitch %.1f deg, head pitch %.1f deg",
                      static_cast<unsigned long long>(a.age), static_cast<unsigned long long>(a.nowSeq), static_cast<unsigned long long>(a.sampleSeq), a.sampleCpu ? "cpu" : "gpu",
                      a.paired ? "of the sample's frame" : "of the setter's frame (the sample's not kept)", static_cast<double>(a.aimYaw), static_cast<double>(a.aimPitch),
                      static_cast<double>(a.headPitch));
    Log::get().note("srv turret radar anchor: direction at frame %u: u (%.3f,%.3f,%.3f) world; anchor rows %s [%s]; P rows (%.3f,%.3f,%.3f)(%.3f,%.3f,%.3f)(%.3f,%.3f,%.3f); d (%.3f,%.3f,%.3f) "
                    "in the anchor's axes = %.3f m along %s (angle to the anchor's z: %.1f deg, yaw %.1f deg; key %s)%s",
                    a.lastAddStamp - 1u, static_cast<double>(a.u[0]), static_cast<double>(a.u[1]), static_cast<double>(a.u[2]), rows, source, static_cast<double>(a.pRows[0]),
                    static_cast<double>(a.pRows[1]), static_cast<double>(a.pRows[2]), static_cast<double>(a.pRows[3]), static_cast<double>(a.pRows[4]), static_cast<double>(a.pRows[5]),
                    static_cast<double>(a.pRows[6]), static_cast<double>(a.pRows[7]), static_cast<double>(a.pRows[8]), static_cast<double>(a.d[0]), static_cast<double>(a.d[1]),
                    static_cast<double>(a.d[2]), dist, a.lastDirected ? "u" : "the anchor's z", st::angleToZDeg(along), st::yawDeg(along), st::radarAnchorModeName(mode), timing);
}

// CAMERA MODE's lines, frame thread. The slot hook's first move of each kind (terrain, icon) and list (A, B: the two eyes), once each.
const char* const kSlotKindName[3] = {"terrain", "icon", "another mode"};
const char* const kSlotListName[3] = {"A", "B", "another list"};
void slotFirstLines(AnchorWatch& w) {
    const RadarSlotStats r = srvRadarSlotStats();
    for (int k = 0; k < 3; ++k)
        for (int l = 0; l < 3; ++l) {
            const RadarSlotFirst& f = r.first[k][l];
            if (!f.have || w.slotSaid[k][l]) continue;
            w.slotSaid[k][l] = true;
            Log::get().note("srv turret radar camera: moved back %.2f m for slot 0x%llX (index %u, %s (mode %u), list %s (%u)), direction from %s, u (%.3f,%.3f,%.3f)", static_cast<double>(f.back),
                            static_cast<unsigned long long>(f.slot), f.index, kSlotKindName[k], f.mode, kSlotListName[l], f.list,
                            f.fromTurret ? "the turret (CPU camera sample)" : "the camera's own forward", static_cast<double>(f.u[0]), static_cast<double>(f.u[1]), static_cast<double>(f.u[2]));
        }
}
// The 10 s line, whenever camera mode was on with the view in the window (radar slots 0: the hook never saw the radar's slots); the baselines move on either way.
void slotWindow(AnchorWatch& w) {
    const RadarSlotStats r = srvRadarSlotStats();
    if (w.cameraSeen) {
        uint64_t radar[3][3], total = 0;
        for (int k = 0; k < 3; ++k)
            for (int l = 0; l < 3; ++l) {
                radar[k][l] = r.radar[k][l] - w.winSlotRadar[k][l];
                total += radar[k][l];
            }
        const uint64_t other = radar[0][2] + radar[1][2] + radar[2][0] + radar[2][1] + radar[2][2];
        char tail[200] = "";
        if (r.state != RadarAnchorHookState::Armed)
            std::snprintf(tail, sizeof(tail), " -- the slot hook is not armed (%s)", r.state == RadarAnchorHookState::StoodDown ? "it stood down: its line says why" : "not tried");
        else if (total == 0)
            std::snprintf(tail, sizeof(tail), " -- NO radar slot in these 10 s: the camera was not moved");
        Log::get().note("srv turret radar camera: 10 s: slot calls %llu, radar slots %llu (terrain A %llu, terrain B %llu, icon A %llu, icon B %llu, other %llu), moved %llu (direction turret %llu, "
                        "camera forward %llu), faults %llu; hook thread %u, camera writes on thread %u%s",
                        static_cast<unsigned long long>(r.calls - w.winSlotCalls), static_cast<unsigned long long>(total), static_cast<unsigned long long>(radar[0][0]),
                        static_cast<unsigned long long>(radar[0][1]), static_cast<unsigned long long>(radar[1][0]), static_cast<unsigned long long>(radar[1][1]),
                        static_cast<unsigned long long>(other), static_cast<unsigned long long>(r.moved - w.winSlotMoved), static_cast<unsigned long long>(r.fromTurret - w.winSlotTurret),
                        static_cast<unsigned long long>(r.fromCamera - w.winSlotCamera), static_cast<unsigned long long>(r.faults - w.winSlotFaults), r.thread,
                        g_cameraWriteThread.load(std::memory_order_relaxed), tail);
    }
    w.cameraSeen = g_cameraBillboards.load(std::memory_order_relaxed);
    w.winSlotCalls = r.calls;
    for (int k = 0; k < 3; ++k)
        for (int l = 0; l < 3; ++l) w.winSlotRadar[k][l] = r.radar[k][l];
    w.winSlotMoved = r.moved;
    w.winSlotTurret = r.fromTurret;
    w.winSlotCamera = r.fromCamera;
    w.winSlotFaults = r.faults;
}

void anchorTick(bool on, uint32_t frameNo, int radarForwardCm) {
    AnchorWatch& w = g_anchorWatch;
    const auto mode = static_cast<st::RadarAnchorMode>(g_anchorMode.load(std::memory_order_relaxed));
    if (on && mode != st::RadarAnchorMode::kOff && !w.armTried) {
        w.armTried = true;
        (void)srvRadarAnchorArm(nullptr, 0);   // says its own lines: "setter hook armed: ..." or "setter hook stood down: <why>; nothing was patched", then the update counter's
    }
    w.offset = st::radarAnchorOffset(on, mode, radarForwardCm);   // zero in camera mode: the restore gives the game its own placement back
    srvRadarAnchorPublish(w.offset.bits, frameNo);
    // CAMERA MODE: the slot hook, armed once when the view is first on in camera mode, and its back distance every frame (0 outside camera mode or the view: the game's camera)
    const bool cameraMode = on && mode == st::RadarAnchorMode::kCamera;
    if (cameraMode && !w.slotArmTried) {
        w.slotArmTried = true;
        (void)srvRadarSlotArm();   // says its own line: "srv turret radar camera: slot hook armed: ..." or "... slot hook stood down: <why>; nothing was patched"
    }
    srvRadarSlotPublish(st::radarCameraBackM(on, mode, radarForwardCm));
    g_cameraBillboards.store(cameraMode, std::memory_order_relaxed);
    if (cameraMode) {
        w.cameraSeen = true;
        slotFirstLines(w);
    }
    const RadarAnchorStats a = srvRadarAnchorStats();
    const bool dirWanted = on && mode != st::RadarAnchorMode::kOff && a.state == RadarAnchorHookState::Armed;
    g_anchorDirWanted.store(dirWanted, std::memory_order_relaxed);
    detail::g_srvTurretCameraRowsWanted.store(dirWanted, std::memory_order_relaxed);   // the Unmap tee's CPU read of the camera rows
    if (on && !w.on) {
        w.stayMoved = a.moved;
        w.stayOurs = a.ours;
        w.asked = w.said = false;
        srvRadarAnchorPublishDirection(nullptr, false);   // a u from an earlier stay is not this one's: the fallback until the render side reads a new one
    }
    const uint64_t nowMs = GetTickCount64();
    if (on) {
        if (!w.offset.zero()) w.asked = true;
        if (w.said && a.moved > w.stayMoved && nowMs - w.dirLineMs >= 2000) {
            w.dirLineMs = nowMs;
            anchorDirLine(a, mode);
        }
        if (!w.said && a.moved > w.stayMoved) {
            w.said = true;
            w.dirLineMs = nowMs;
            char caller[64];
            if (a.callerRva)
                std::snprintf(caller, sizeof(caller), "EliteDangerous64.exe+0x%llX", static_cast<unsigned long long>(a.callerRva));
            else
                std::snprintf(caller, sizeof(caller), "0x%llX (outside EliteDangerous64.exe)", static_cast<unsigned long long>(a.caller));
            Log::get().note("srv turret radar anchor: placement moved at frame %u: the game's (%.3f,%.3f,%.3f) + (%.3f,%.3f,%.3f) -> (%.3f,%.3f,%.3f) in the anchor's axes (key %s), caller %s, "
                            "obj 0x%llX; the 5559BD94 billboards are left to the game",
                            a.lastAddStamp - 1u, static_cast<double>(a.gameT[0]), static_cast<double>(a.gameT[1]), static_cast<double>(a.gameT[2]), static_cast<double>(a.d[0]),
                            static_cast<double>(a.d[1]), static_cast<double>(a.d[2]), static_cast<double>(a.outT[0]), static_cast<double>(a.outT[1]),
                            static_cast<double>(a.outT[2]), st::radarAnchorModeName(mode), caller, static_cast<unsigned long long>(a.obj));
            anchorDirLine(a, mode);
        }
    } else if (w.on) {
        const uint64_t n = a.moved - w.stayMoved, ours = a.ours - w.stayOurs;
        char also[64] = "";
        if (ours) std::snprintf(also, sizeof(also), " (already ours in %llu)", static_cast<unsigned long long>(ours));
        char tail[600] = "";
        if (a.state != RadarAnchorHookState::Armed)
            std::snprintf(tail, sizeof(tail), " -- hook not armed: %s",
                          a.state == RadarAnchorHookState::StoodDown ? srvRadarAnchorWhy() : "fix.srv_turret_radar_anchor was off while the view was on");
        else if (n == 0 && ours == 0 && w.asked)
            std::snprintf(tail, sizeof(tail), " -- NEVER RAN: the radar schematic's placement setter was not called with the radar object while the view was on");
        else if (n == 0 && ours == 0)
            std::snprintf(tail, sizeof(tail), " -- not asked: fix.srv_turret_radar_anchor was off or radar forward 0 while the view was on");
        Log::get().note("srv turret radar anchor: view off: moved in %llu calls%s%s", static_cast<unsigned long long>(n), also, tail);
    }
    w.on = on;
    if (a.suspect && !w.warned) {
        w.warned = true;
        Log::get().note("srv turret radar anchor: WARNING: the game's translation moved by (%.3f,%.3f,%.3f) between setter calls, close to the offset: its caller may feed back our value",
                        static_cast<double>(a.suspectStep[0]), static_cast<double>(a.suspectStep[1]), static_cast<double>(a.suspectStep[2]));
    }
    g_anchorYield.store(on && mode != st::RadarAnchorMode::kOff && mode != st::RadarAnchorMode::kCamera && a.state == RadarAnchorHookState::Armed && a.holdsOurs, std::memory_order_relaxed);
    const bool yieldNow = g_anchorYield.load(std::memory_order_relaxed);
    detail::g_srvTurretRadarWritersOn.store(yieldNow || cameraMode, std::memory_order_relaxed);   // the writer watch: the draw-time correction's, and camera mode's billboard match
    if (yieldNow) w.yieldSeen = true;
    const uint32_t cameraThread = g_cameraWriteThread.load(std::memory_order_relaxed);
    if (!w.threadsSaid && a.setterThread && cameraThread) {
        w.threadsSaid = true;
        Log::get().note("srv turret radar anchor: the setter runs on thread %u, the camera writes on thread %u%s", a.setterThread, cameraThread,
                        a.setterThread == cameraThread ? " (the same thread)" : "");
    }
}

void correctionWindow(bool yieldSeen);   // the draw-time correction's 10 s line (below)

// At each 10 s boundary of the frame tick: the window's line when the view was on in it and the hook is armed; the counters' baselines move on either way.
void anchorWindow(bool seen) {
    AnchorWatch& w = g_anchorWatch;
    const RadarAnchorStats a = srvRadarAnchorStats();
    if (seen && a.state == RadarAnchorHookState::Armed) {
        const uint64_t calls = a.setterCalls - w.winCalls, radar = a.setterRadar - w.winRadar, moved = a.moved - w.winMoved, ours = a.ours - w.winOurs,
                       restored = a.restored - w.winRestored, faulted = a.faulted - w.winFaulted, changes = a.gameChanges - w.winChanges, updates = a.updates - w.winUpdates;
        char game[96], changed[48] = "", upd[48], extra[200] = "";
        if (a.haveGame)
            std::snprintf(game, sizeof(game), "(%.3f,%.3f,%.3f)", static_cast<double>(a.gameT[0]), static_cast<double>(a.gameT[1]), static_cast<double>(a.gameT[2]));
        else
            std::snprintf(game, sizeof(game), "(not seen yet)");
        if (changes) std::snprintf(changed, sizeof(changed), " changed %llu times", static_cast<unsigned long long>(changes));
        if (a.updateArmed)
            std::snprintf(upd, sizeof(upd), "%llu", static_cast<unsigned long long>(updates));
        else
            std::snprintf(upd, sizeof(upd), "not counted (the counter stood down)");
        int at = 0;
        if (restored) at += std::snprintf(extra + at, sizeof(extra) - at, "; restored %llu", static_cast<unsigned long long>(restored));
        if (faulted && at >= 0 && at < static_cast<int>(sizeof(extra))) at += std::snprintf(extra + at, sizeof(extra) - at, "; faults %llu", static_cast<unsigned long long>(faulted));
        if (a.noSlot && at >= 0 && at < static_cast<int>(sizeof(extra))) at += std::snprintf(extra + at, sizeof(extra) - at, "; without a slot %llu in all", static_cast<unsigned long long>(a.noSlot));
        const uint64_t dirCopies = g_dirCopies.load(std::memory_order_relaxed), dirReads = g_dirReads.load(std::memory_order_relaxed), dirRejects = g_dirRejects.load(std::memory_order_relaxed);
        // the samples' source and the age of u at the setter
        uint64_t ageMax = 0;
        double residualMax = 0.0, gapMin = 0.0, gapMax = 0.0;
        srvRadarAnchorTakeWindowMax(&ageMax, &residualMax, &gapMin, &gapMax);
        const uint64_t computed = (a.pairedCount - w.winPaired) + (a.unpairedCount - w.winUnpaired);
        uint64_t miss[4];
        for (int i = 0; i < 4; ++i) miss[i] = g_dirCpuMiss[i].load(std::memory_order_relaxed) - w.winCpuMiss[i];
        char lag[420];
        std::snprintf(lag, sizeof(lag),
                      "; camera samples cpu %llu, gpu reads %llu of %llu copies (%llu refused; cpu missed: not written %llu, offset %llu, another frame %llu, not a camera %llu); u age mean %.2f "
                      "max %llu frames; anchor paired by frame %llu of %llu",
                      static_cast<unsigned long long>(g_dirCpu.load(std::memory_order_relaxed) - w.winCpu), static_cast<unsigned long long>(dirReads - w.winDirReads),
                      static_cast<unsigned long long>(dirCopies - w.winDirCopies), static_cast<unsigned long long>(dirRejects - w.winDirRejects),
                      static_cast<unsigned long long>(miss[0]), static_cast<unsigned long long>(miss[1]), static_cast<unsigned long long>(miss[2]), static_cast<unsigned long long>(miss[3]),
                      computed ? double(a.ageSum - w.winAgeSum) / double(computed) : 0.0, static_cast<unsigned long long>(ageMax),
                      static_cast<unsigned long long>(a.pairedCount - w.winPaired), static_cast<unsigned long long>(computed));
        Log::get().note("srv turret radar anchor: 10 s: setter calls %llu (radar %llu, moved %llu, already ours %llu), the game's translation %s%s, updates %s%s; moves along u %llu, "
                        "fallback %llu (no u %llu, anchor not read or refused %llu; getter faults %llu)%s; last d (%.3f,%.3f,%.3f) m in the anchor's axes (key %s)%s%s",
                        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(radar), static_cast<unsigned long long>(moved), static_cast<unsigned long long>(ours), game,
                        changed, upd, extra, static_cast<unsigned long long>(a.directed - w.winDirected), static_cast<unsigned long long>(a.fallback - w.winFallback),
                        static_cast<unsigned long long>(a.fallbackNoU - w.winNoU), static_cast<unsigned long long>(a.fallbackNoAnchor - w.winNoAnchor),
                        static_cast<unsigned long long>(a.anchorFaults - w.winAnchorFaults), lag, static_cast<double>(a.d[0]), static_cast<double>(a.d[1]), static_cast<double>(a.d[2]),
                        st::radarAnchorModeName(static_cast<st::RadarAnchorMode>(g_anchorMode.load(std::memory_order_relaxed))), a.holdsOurs ? "; the placement holds our offset" : "",
                        radar == 0 ? " -- NO setter call with the radar in these 10 s: the offset (and any key change) reaches the game only when it sets the placement" : "");
        // the residual (srv_turret_math.h THE RESIDUAL; with no lead its mean is the turret's motion over the horizon, which the draw-time correction takes up) and the pairing's margin
        const uint64_t resN = a.resN - w.winResN, latN = a.latN - w.winLatN, horizonN = a.horizonN - w.winHorizonN, gapN = a.gapN - w.winGapN;
        Log::get().note("srv turret radar anchor: 10 s residual: yaw true minus used mean %.3f max |%.3f| deg over %llu writes (lagging %llu, leading %llu, the rest too slow to say), implied "
                        "latency mean %.2f frames over %llu, display horizon mean %.1f ms, sample interval %.1f ms; shown call before the write: mean %.2f min %.2f max %.2f ms; setter ages 0: %llu, "
                        "1: %llu, 2+: %llu",
                        resN ? double(a.resSumMicro - w.winResSumMicro) / 1.0e6 / double(resN) : 0.0, residualMax, static_cast<unsigned long long>(resN),
                        static_cast<unsigned long long>(a.resLagging - w.winResLagging), static_cast<unsigned long long>(a.resLeading - w.winResLeading),
                        latN ? double(a.latSumMilli - w.winLatSumMilli) / 1000.0 / double(latN) : 0.0, static_cast<unsigned long long>(latN),
                        horizonN ? double(a.horizonSumUs - w.winHorizonSumUs) / 1000.0 / double(horizonN) : 0.0, static_cast<double>(a.intervalMs),
                        gapN ? double(a.gapSumUs - w.winGapSumUs) / 1000.0 / double(gapN) : 0.0, gapMin, gapMax,
                        static_cast<unsigned long long>(a.ageHist[0] - w.winAgeHist[0]), static_cast<unsigned long long>(a.ageHist[1] - w.winAgeHist[1]),
                        static_cast<unsigned long long>(a.ageHist[2] - w.winAgeHist[2]));
    } else {
        srvRadarAnchorTakeWindowMax(nullptr, nullptr, nullptr, nullptr);   // the maxima are the window's
    }
    w.winResN = a.resN;
    w.winGapN = a.gapN;
    w.winGapSumUs = a.gapSumUs;
    w.winResSumMicro = a.resSumMicro;
    w.winResLagging = a.resLagging;
    w.winResLeading = a.resLeading;
    w.winLatN = a.latN;
    w.winLatSumMilli = a.latSumMilli;
    w.winHorizonN = a.horizonN;
    w.winHorizonSumUs = a.horizonSumUs;
    for (int i = 0; i < 3; ++i) w.winAgeHist[i] = a.ageHist[i];
    w.winAgeSum = a.ageSum;
    w.winPaired = a.pairedCount;
    w.winUnpaired = a.unpairedCount;
    w.winCpu = g_dirCpu.load(std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) w.winCpuMiss[i] = g_dirCpuMiss[i].load(std::memory_order_relaxed);
    w.winDirected = a.directed;
    w.winFallback = a.fallback;
    w.winNoU = a.fallbackNoU;
    w.winNoAnchor = a.fallbackNoAnchor;
    w.winAnchorFaults = a.anchorFaults;
    w.winDirCopies = g_dirCopies.load(std::memory_order_relaxed);
    w.winDirReads = g_dirReads.load(std::memory_order_relaxed);
    w.winDirRejects = g_dirRejects.load(std::memory_order_relaxed);
    w.winCalls = a.setterCalls;
    w.winRadar = a.setterRadar;
    w.winMoved = a.moved;
    w.winOurs = a.ours;
    w.winRestored = a.restored;
    w.winFaulted = a.faulted;
    w.winChanges = a.gameChanges;
    w.winUpdates = a.updates;
    correctionWindow(w.yieldSeen);
    w.yieldSeen = g_anchorYield.load(std::memory_order_relaxed);
    slotWindow(w);
}

bool anchorLeaves(int family) { return st::radarAnchorLeavesFamily(family) && g_anchorYield.load(std::memory_order_relaxed); }

// ---- THE DRAW-TIME CORRECTION (TEMPORARY, with fix.srv_turret_radar_anchor; srv_turret_math.h THE SHAKE) ---------------------------------------------------------------------------
// While the anchor yield is on, a draw of the radar's own picture billboard (family 7, ps EA02FAC2, whose t4 is a texture the radar's writers drew into: the terrain schematic vs
// 4EDE4BC9, or vs 5B0068AF into a 256x256 target, the Scorpion icon) is moved through signature H's geometry shader by delta = desired - used, in the world, turned into the eye's space
// with that draw's own camera rows. used is the offset the setter call this frame's camera write shows left in force, desired is s * F along this frame's true u. Family 8 (E95634B0)
// and the 512x512 panel hologram are never moved. Render thread; the frame tick says the counts.
constexpr uint64_t kRadarIconVs = 0x5B0068AF5630F96Bull;
constexpr uint32_t kWriterTargets = 4;
struct Writers {
    uint32_t frame = 0xFFFFFFFFu;
    const void* disc[kWriterTargets] = {};
    const void* icon[kWriterTargets] = {};
    uint32_t nDisc = 0, nIcon = 0;
};
// This frame's writers move a billboard. The previous frame's only say, in the 10 s line, how often a billboard read a target a radar writer drew into last frame and none drew into
// this frame: the game's hologram targets can change hands between frames (the holo rounds of flight 707c0ab5), so such a target may now hold a panel's picture and is never moved.
Writers g_writers, g_writersPrev;
std::atomic<uint64_t> g_cwDisc{0}, g_cwIcon{0}, g_cwExtraFrames{0};   // writer draws seen, and frames with more than two 256x256 icon writers (two is normal: one an eye)
// Why a radar billboard draw was left to the game (each is a pop, but for kCorrZero): kCorrOther's causes are split, so the 10 s line says which.
enum CorrMiss : int {
    kCorrOk = 0, kCorrNoMatch, kCorrNoRows, kCorrNoCall, kCorrNoAnchor, kCorrOver, kCorrExtra, kCorrZero, kCorrNotReady, kCorrNoEyeFrame, kCorrParams, kCorrTopology, kCorrNoGameCb,
    kCorrBeginFailed, kCorrMissCount
};
struct CorrCounters {
    std::atomic<uint64_t> moved[2]{}, disc{0}, icon{0}, frames{0}, deltaSumUm{0}, deltaMaxUm{0}, yawMaxMilli{0}, pitchMaxMilli{0}, family8{0}, lastFrameOnly{0}, large{0}, zcSumUm{0}, zcN{0}, zcMinUm{UINT64_MAX}, zcMaxUm{0}, noContentDepth{0};
    std::atomic<int64_t> yawSumMicro{0}, pitchSumMicro{0};
    std::atomic<uint64_t> left[kCorrMissCount]{};
};
CorrCounters g_cc;
struct FrameCorrection {
    uint32_t frame = 0xFFFFFFFFu;
    int reason = kCorrNoRows;
    float deltaW[3] = {};
    double yawDeg = 0.0, pitchDeg = 0.0, gapMs = 0.0;
    bool counted = false;
    // for THE PAIR CAPTURE's log (TEMPORARY): the shown call's number and the offset it left in force (the anchor's axes), the desired aim's yaw in the anchor's axes (the turret's yaw on
    // the vehicle) with the camera write's QPC time, and its rate since the previous frame's (haveRate false without one)
    uint64_t callSerial = 0;
    float callD[3] = {};
    double aimYawDeg = 0.0, yawRateDegS = 0.0;
    int64_t writtenAt = 0;
    bool haveRate = false;
    // THE LOCK's rest (srv_turret_math.h RestWatch): the turret's pitch rate beside its yaw rate, the vehicle's turn rate (the anchor's world rotation, frame to frame), and whether
    // this frame is at rest
    double pitchRateDegS = 0.0, vehicleRateDegS = 0.0;
    bool atRest = false;
};
FrameCorrection g_corr;
struct AimPrev {
    uint32_t frame = 0;
    double yawDeg = 0.0, pitchDeg = 0.0;
    float A[16] = {};
    bool aValid = false;
    int64_t t = 0;
    bool have = false;
};
AimPrev g_aimPrev;   // the last frame's desired aim, anchor rotation and write time, for the rates
st::RestWatch g_rest;
const void* g_lastT4 = nullptr;   // identity: the t4 billboardKind last read (THE PAIR CAPTURE's log)
bool g_corrFirstSaid = false;
int64_t g_qpcFreq = 0;
uint32_t g_movedAt[2][2] = {};   // [eye][disc, icon]: g_frame + 1 of the frame a billboard of that kind was moved in that eye (one of each per eye per frame)
// The first 256x256 icon writer beyond two in a frame, said once at the next frame (with whether a billboard's t4 matched its target that frame).
struct ExtraWriter {
    bool pending = false, said = false, matched = false, haveVp = false;
    const void* target = nullptr;
    uint32_t frame = 0;
    UINT count = 0, instances = 0, w = 0, h = 0;
    D3D11_VIEWPORT vp{};
};
ExtraWriter g_extraWriter;
int g_corrProbeKind[2] = {1, 1};   // by eye: the kind (1 disc, 2 icon) the corrected path's next GPU sample takes; it alternates, once a second (THE GPU INSTRUMENT's rule)

void writersFrame() {
    if (g_writers.frame == g_frame) return;
    if (g_extraWriter.pending && g_extraWriter.frame != g_frame) {
        g_extraWriter.pending = false;
        g_extraWriter.said = true;
        char vp[96] = "none";
        if (g_extraWriter.haveVp)
            std::snprintf(vp, sizeof(vp), "(%.0f,%.0f %.0fx%.0f)", static_cast<double>(g_extraWriter.vp.TopLeftX), static_cast<double>(g_extraWriter.vp.TopLeftY),
                          static_cast<double>(g_extraWriter.vp.Width), static_cast<double>(g_extraWriter.vp.Height));
        Log::get().note("srv turret radar anchor: a third 256x256 5B0068AF writer at frame %u: draw count %u, instances %u, target %06X %ux%u, viewport %s; a billboard's t4 %s that target "
                        "that frame",
                        g_extraWriter.frame, g_extraWriter.count, g_extraWriter.instances, holoShortId(g_extraWriter.target), g_extraWriter.w, g_extraWriter.h, vp,
                        g_extraWriter.matched ? "matched" : "did not match");
    }
    g_writersPrev = g_writers.frame + 1 == g_frame ? g_writers : Writers{};
    g_writers = Writers{};
    g_writers.frame = g_frame;
}
bool addWriter(const void* (&list)[kWriterTargets], uint32_t& n, const void* r) {
    for (uint32_t i = 0; i < n; ++i)
        if (list[i] == r) return false;
    if (n < kWriterTargets) list[n++] = r;
    return true;
}

// The billboard's kind from its PS t4: 1 the disc's picture, 2 the icon's, 0 neither (a texture no radar writer drew into this frame). lastFrameOnly: 0, yet a radar writer drew into
// it last frame (counted, never moved).
int kindOf(const Writers& w, const void* res) {
    for (uint32_t i = 0; i < w.nDisc; ++i)
        if (w.disc[i] == res) return 1;
    for (uint32_t i = 0; i < w.nIcon; ++i)
        if (w.icon[i] == res) return 2;
    return 0;
}
int billboardKind(ID3D11DeviceContext* ctx, bool& lastFrameOnly) {
    lastFrameOnly = false;
    writersFrame();
    ID3D11ShaderResourceView* srv = nullptr;
    if (!guarded("srv.turret.corr.srv", [&] { ctx->PSGetShaderResources(4, 1, &srv); }) || !srv) return 0;
    ID3D11Resource* res = nullptr;
    guarded("srv.turret.corr.srvres", [&] { srv->GetResource(&res); });
    const int kind = res ? kindOf(g_writers, res) : 0;
    if (res && g_extraWriter.pending && g_extraWriter.frame == g_frame && res == g_extraWriter.target) g_extraWriter.matched = true;
    lastFrameOnly = res && !kind && kindOf(g_writersPrev, res) != 0;
    g_lastT4 = res;
    st::release(res);
    st::release(srv);
    return kind;
}

// The frame's correction, once, at its first radar billboard draw (either eye: that draw's rows with that eye's pose give u_true).
int correctionCompute(ID3D11DeviceContext* ctx, int eye, FrameCorrection& c) {
    uint64_t poseSeq = 0;
    float head[12] = {}, eyeToHead[2][12] = {};
    double R[9], E[3];
    if (!nativeTemporalHeadPose(&poseSeq, head, eyeToHead) || !st::eyePoseOf(head, eyeToHead[eye], R, E)) return kCorrNoRows;
    float rows[12];
    int64_t writtenAt = 0;
    if (anchorDirCpuRows(ctx, poseSeq, rows, nullptr, &writtenAt) != 0 || writtenAt <= 0) return kCorrNoRows;
    float uTrue[3];
    if (!st::turretForwardWorld(rows, rows + 4, rows + 8, R, uTrue)) return kCorrNoRows;
    RadarAnchorShownCall call;
    if (!srvRadarAnchorShownCall(writtenAt, call) || call.sF == 0.0f || (call.d[0] == 0.0f && call.d[1] == 0.0f && call.d[2] == 0.0f)) return kCorrNoCall;
    if (!call.aValid) return kCorrNoAnchor;
    float dStar[3];
    if (!st::radarCorrectionWorld(uTrue, call.A, call.sF, call.d, c.deltaW, dStar)) return kCorrNoAnchor;
    const double norm = std::sqrt(double(c.deltaW[0]) * c.deltaW[0] + double(c.deltaW[1]) * c.deltaW[1] + double(c.deltaW[2]) * c.deltaW[2]);
    if (!(norm <= st::kCorrectionMaxM)) return kCorrOver;   // a sanity bound (0.5 m): only garbage is left to the game, since every draw left to it pops
    const float want[3] = {dStar[0] / call.sF, dStar[1] / call.sF, dStar[2] / call.sF}, used[3] = {call.d[0] / call.sF, call.d[1] / call.sF, call.d[2] / call.sF};
    const st::Aim aw = st::aimOf(want), au = st::aimOf(used);
    c.yawDeg = st::wrapDeg(aw.yaw - au.yaw);
    c.pitchDeg = aw.pitch - au.pitch;
    if (!g_qpcFreq) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        g_qpcFreq = f.QuadPart > 0 ? f.QuadPart : 1;
    }
    c.gapMs = double(writtenAt - call.t) * 1000.0 / double(g_qpcFreq);
    c.callSerial = call.serial;   // THE PAIR CAPTURE's log (TEMPORARY)
    for (int i = 0; i < 3; ++i) c.callD[i] = call.d[i];
    c.aimYawDeg = aw.yaw;
    c.writtenAt = writtenAt;
    bool vehicleRate = false;
    if (g_aimPrev.have && g_aimPrev.frame + 1u == g_frame && writtenAt > g_aimPrev.t) {
        const double perS = double(g_qpcFreq) / double(writtenAt - g_aimPrev.t);
        c.yawRateDegS = st::wrapDeg(aw.yaw - g_aimPrev.yawDeg) * perS;
        c.pitchRateDegS = (aw.pitch - g_aimPrev.pitchDeg) * perS;
        c.haveRate = true;
        if (g_aimPrev.aValid) {   // call.aValid holds: the early return above
            c.vehicleRateDegS = st::rotationAngleDeg(call.A, g_aimPrev.A) * perS;
            vehicleRate = true;
        }
    }
    g_aimPrev.frame = g_frame;
    g_aimPrev.yawDeg = aw.yaw;
    g_aimPrev.pitchDeg = aw.pitch;
    std::memcpy(g_aimPrev.A, call.A, sizeof(g_aimPrev.A));
    g_aimPrev.aValid = true;
    g_aimPrev.t = writtenAt;
    g_aimPrev.have = true;
    c.atRest = g_rest.step(c.haveRate && vehicleRate, c.yawRateDegS, c.pitchRateDegS, c.vehicleRateDegS);   // THE LOCK's rest
    return kCorrOk;
}

// THE CONTENT DEPTH (flight e64866f9: the hole is in the disc's own picture). Both radar billboards are moved by one 3D delta, but a quad moved in 3D shifts on screen by delta over its
// OWN depth, while the picture it shows is of content at the schematic's origin, z_c (about 0.34 m in V0's eye space: THE RADAR PICTURE'S CAMERA). With tr.w = 1 / z_c the shader moves each
// vertex by delta * w / z_c (srv_turret_hud_shader.h turretMove, srv_turret_math.h anchoredMove), whose projection is that of the content point on the same ray at depth z_c moved by delta,
// for any quad depth: the disc, the hole in its picture and the icon all shift on screen as the content does. z_c is the V0 instrument's last read (st::contentInvDepth: one read in the
// last kContentDepthFreshFrames frames, 0.1 to 2 m); without one the draw moves by the plain delta, counted as "no content depth". The same z_c for both eyes and both kinds (the icon
// sits at the schematic's origin).
float radarContentInvDepth() { return st::contentInvDepth(g_radarCam.have, g_frame - g_radarCam.lastReadFrame, g_radarCam.last.centre[2]); }

// ---- THE PAIR CAPTURE (TEMPORARY instrument, issue 140: the Scorpion's ghost and the trailing disc) -------------------------------------------------------------------------------------
// The quad probe (one billboard a second) said each quad moves as content at z_c would, yet the hole in the disc's picture sits about |delta| from the icon, and natively (a8465cb3) the two
// agree within a frame. So in one frame BOTH radar billboards of each eye go through the stream-output twin, every issue captured, and each says: its quad's centre in the eye before and
// after the move, its w range, the delta and z_c it ran with, its VS b1 and the forward row (279) the delta was turned with, the setter call (number and d) the frame's correction was
// paired with, and how many issues it made with our shader bound or another. Every frame for kPairDumpFrames frames after an eye dump (srvTurretNoteEyeDump), else once every kPairEveryMs.
// A ring of kPairSlots frames is read back at the frame boundaries without stalling; a frame whose slot is still in flight is skipped (counted). THE PICTURES: at the dump window's frames
// eye 0's disc and icon pictures (t4) are copied once each and written next to the eye dump as grayscale images. THE LOCK: in the dump window each billboard's centre is also put in the
// seated frame with that eye's pose, beside the turret's yaw rate. Every moved radar billboard's scope also counts its issues (srvTurretHudNoteIssue), and the 10 s pair line sums them.
// Render thread, but the 10 s line (the frame tick).
constexpr uint32_t kPairSlots = 6, kPairDumpFrames = 16, kPairGiveUpFrames = 8, kPairGroups = 4;
constexpr uint64_t kPairEveryMs = 2000;
constexpr UINT kPairBufferBytes = 4096;
constexpr int kRadarFamily = 7;   // the radar's own picture billboard: vs 5559BD94 with ps EA02FAC2
static_assert(st::kFamilies[kRadarFamily].sig == st::kSceneSignature && st::kFamilies[kRadarFamily].strideBytes == 52 && st::kFamilies[kRadarFamily].posFloat == 5 &&
                  st::kFamilies[kRadarFamily].preFloat == 9,
              "signature H's record: lighting position 3, UV 2 (float 3), the moved clip position at 5, `pre` at 9");
constexpr uint32_t kPairUvFloat = 3;
struct PairEntry {
    ID3D11Buffer* so = nullptr;      // the twin's stream-output target
    ID3D11Buffer* stage = nullptr;   // its copy for the CPU
    ID3D11Query* stats = nullptr;    // the primitives written
    bool armed = false, copied = false, read = false;
    // what the draw ran with
    float dEye[3] = {}, contentInv = 0.0f, tan[4] = {}, fwd[3] = {}, vpW = 0.0f, vpH = 0.0f;
    const void* b1 = nullptr;
    const void* t4 = nullptr;
    bool pose = false;
    double R[9] = {}, E[3] = {};
    uint32_t issues = 0, ours = 0, other = 0;
    int lockWhy = 0;                 // THE LOCK: 0 locked (target: its place in the eye), else why the draw kept the delta (kLockMissName)
    float target[3] = {};
    // read back: issue 0's NDC box centre before the move, its move and the content's expected move (px, x right, y up), its centre in the eye before and after
    bool measured = false, expected = false;
    double preNdc[2] = {}, movePx[2] = {}, expectPx[2] = {}, preEye[3] = {}, postEye[3] = {};
};
struct PairSlot {
    PairEntry e[2][2];   // [eye][kind - 1]
    uint32_t frame = 0, dumpIndex = 0, waited = 0;
    bool open = false, pending = false, dump = false;
    bool atRest = false, quiet = false;   // THE LOCK: the frame was at rest (its pre-move centres teach the targets); a capture only for that says nothing in the log
    double timeS = 0.0;
    uint64_t callSerial = 0;
    float callD[3] = {};
    double yawRate = 0.0;
    bool haveRate = false;
};
enum class PictureState : uint8_t { kNone, kPending, kWritten, kFailed };
struct PairPicture {
    ID3D11Texture2D* stage = nullptr;
    PictureState state = PictureState::kNone;
    const void* id = nullptr;
    uint32_t w = 0, h = 0, frame = 0, waited = 0, format = 0;
    char why[96] = "";
};
struct PairWatch {
    ID3D11Device* device = nullptr;   // identity: the slots' objects are this device's
    bool failed = false;
    PairSlot slot[kPairSlots];
    int current = -1;                  // this frame's slot (-1: none open yet)
    bool wantFrame = false;            // this frame is a capture frame (decided at the boundary)
    bool quietFrame = false;           // ... only for THE LOCK's learning (every kLockLearnEveryFrames frames while a kind has no target)
    uint64_t lastMs = 0;
    uint32_t lastFrame = 0;
    uint32_t dumpSerial = 0;           // the last eye dump taken up
    wchar_t stamp[16] = L"";
    uint32_t dumpLeft = 0, dumpIndex = 0;
    bool dumpFrame = false, windowOpen = false;
    uint32_t windowFrames = 0, windowRadar = 0, windowCaptured[2][2] = {};   // the window's frames with a slot, radar billboard draws seen, entries captured
    PairPicture picture[2];            // [kind - 1], eye 0
    PairEntry* scope = nullptr;        // the open scope's entry (pairEnd, pairFinish)
};
PairWatch g_pair;
std::atomic<uint32_t> g_eyeDumpSerial{0};   // srvTurretNoteEyeDump (any thread): the stamp is written before the serial moves
wchar_t g_eyeDumpStamp[16] = L"";
// The 10 s pair line's counts, by kind (0 disc, 1 icon) where there are two.
struct PairCounters {
    std::atomic<uint64_t> draws[2]{}, issuesMoved[2]{}, inScopeOther[2]{}, declined[2]{}, outside[2]{}, perDraw[2][4]{};
    std::atomic<uint64_t> constFrames{0}, constApart{0}, constNoDepth{0}, capFrames{0}, capApart{0}, captured{0}, skippedBusy{0}, lost{0};
};
PairCounters g_pc;
// A moved radar billboard's scope (radarCorrectionBegin to srvTurretHudEnd): its issues, each with the geometry shader bound when it went out (ours: moved).
struct RadarScope {
    ID3D11DeviceContext* ctx = nullptr;
    const void* gs = nullptr;   // identity: the shader the binding put in
    int kind = 0;
    uint32_t issues = 0, ours = 0, other = 0;
};
RadarScope g_radarScope;
// THE CONSTANTS' COMPARISON, every frame: each eye's moved disc and icon, the delta and z_c each ran with, and the on-screen move of content at z_c each makes at one point (the eye's last
// captured disc centre, else the centre of the view). A difference over 1 px says the two were given different moves.
struct FrameMove {
    uint32_t frame = 0xFFFFFFFFu;
    float dEye[3] = {}, contentInv = 0.0f, tan[4] = {}, vpW = 0.0f, vpH = 0.0f;
};
FrameMove g_frameMove[2][2];   // [eye][kind - 1]
double g_refNdc[2][2] = {};    // [eye]: the comparison point

// ---- THE LOCK (TEMPORARY, with fix.srv_turret_radar_anchor; srv_turret_math.h THE LOCK) ----------------------------------------------------------------------------------------------------
// Each radar billboard (disc, icon) is moved so its quad's centre lands on its own rest place in the seated frame, learned from the pair capture's pre-move centres while the turret and the
// vehicle are still (st::RestWatch); before a kind's target is learned, and for a draw that is not a single quad, the correction keeps its delta. Render thread; the frame tick asks for the
// resets (g_lockResetGen) and says the 10 s line.
st::LockTarget g_lock[2];   // [kind - 1]
double g_lockEyeSum[2][2][3] = {};   // [kind - 1][eye]: the first learning's samples by eye, for the learned line
uint32_t g_lockEyeN[2][2] = {};
uint32_t g_lockResetSeen = 0;
enum LockMiss : int { kLockOk = 0, kLockNoTarget, kLockShape, kLockNoPose, kLockFar, kLockMissCount };
const char* const kLockMissName[kLockMissCount] = {"locked", "no target yet", "not a single quad", "no eye pose", "target too far"};
struct LockCounters {
    std::atomic<uint64_t> locked{0}, fallback[kLockMissCount]{}, shiftSumUm{0}, shiftN{0}, shiftMaxUm{0}, offSumUm{0}, offMaxUm{0}, resets{0}, relearned{0}, restFrames{0}, frames{0};
};
LockCounters g_lc;
void maxUm(std::atomic<uint64_t>& m, uint64_t v) {
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}
void lockReset() {
    for (st::LockTarget& t : g_lock) t = st::LockTarget{};
    for (auto& k : g_lockEyeSum)
        for (auto& e : k)
            for (double& v : e) v = 0.0;
    for (auto& k : g_lockEyeN)
        for (uint32_t& n : k) n = 0;
}
// radarCorrectionBegin: this eye's target for the kind, in the eye's space (X right, Y up, Z forward), when the draw can be locked; else the reason it keeps the delta.
int lockTargetInEye(ID3D11DeviceContext* ctx, int eye, int kind, float tEye[3]) {
    const st::LockTarget& t = g_lock[kind - 1];
    if (!t.learned) return kLockNoTarget;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    if (!guarded("srv.turret.lock.topology", [&] { ctx->IAGetPrimitiveTopology(&topo); }) ||
        !st::lockShape(static_cast<int>(topo), detail::g_srvTurretDrawCount, detail::g_srvTurretDrawInstances))
        return kLockShape;
    uint64_t seq = 0;
    float head[12] = {}, eyeToHead[2][12] = {};
    double R[9], E[3];
    if (!nativeTemporalHeadPose(&seq, head, eyeToHead) || !st::eyePoseOf(head, eyeToHead[eye], R, E)) return kLockNoPose;
    double e[3];
    st::seatedToEye(R, E, t.seated, e);
    const double n2 = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
    if (!std::isfinite(n2) || !(n2 < double(st::kAnchorLimitM) * st::kAnchorLimitM)) return kLockFar;
    for (int i = 0; i < 3; ++i) tEye[i] = static_cast<float>(e[i]);
    return kLockOk;
}
// The frame boundary, a read slot: a frame at rest teaches each kind's target from its eyes' pre-move centres (seated), the first time said once.
void lockLearn(const PairSlot& s) {
    if (!s.atRest) return;
    for (int k = 0; k < 2; ++k) {
        double sum[3] = {}, one[2][3] = {};
        int n = 0;
        bool have[2] = {false, false};
        for (int eye = 0; eye < 2; ++eye) {
            const PairEntry& e = s.e[eye][k];
            if (!e.measured || !e.pose) continue;
            st::eyeToSeated(e.R, e.E, e.preEye, one[eye]);
            for (int c = 0; c < 3; ++c) sum[c] += one[eye][c];
            have[eye] = true;
            ++n;
        }
        if (!n) continue;
        st::LockTarget& t = g_lock[k];
        const bool wasLearned = t.learned;
        if (!wasLearned)
            for (int eye = 0; eye < 2; ++eye)
                if (have[eye]) {
                    for (int c = 0; c < 3; ++c) g_lockEyeSum[k][eye][c] += one[eye][c];
                    ++g_lockEyeN[k][eye];
                }
        const double mean[3] = {sum[0] / n, sum[1] / n, sum[2] / n};
        const bool first = t.add(mean, s.timeS);
        if (wasLearned) g_lc.relearned.fetch_add(1, std::memory_order_relaxed);
        if (!first) continue;
        char eyes[2][64];
        for (int eye = 0; eye < 2; ++eye) {
            const uint32_t m = g_lockEyeN[k][eye];
            if (m)
                std::snprintf(eyes[eye], sizeof(eyes[eye]), "(%.4f,%.4f,%.4f) m from %u", g_lockEyeSum[k][eye][0] / m, g_lockEyeSum[k][eye][1] / m, g_lockEyeSum[k][eye][2] / m, m);
            else
                std::snprintf(eyes[eye], sizeof(eyes[eye]), "none");
        }
        Log::get().note("srv turret radar lock: %s target learned at frame %u: seated (%.4f,%.4f,%.4f) m, the mean of %u rest samples (eye 0 %s, eye 1 %s); its billboard is now locked to it",
                        k == 0 ? "disc" : "icon", s.frame, t.seated[0], t.seated[1], t.seated[2], t.n, eyes[0], eyes[1]);
    }
}

void pairReleaseObjects() {
    for (PairSlot& s : g_pair.slot)
        for (auto& row : s.e)
            for (PairEntry& e : row) {
                st::release(e.so);
                st::release(e.stage);
                st::release(e.stats);
            }
    for (PairPicture& p : g_pair.picture) st::release(p.stage);
}
void pairResetSlots() {
    for (PairSlot& s : g_pair.slot) s = PairSlot{};
    g_pair.current = -1;
    g_pair.scope = nullptr;
    for (PairPicture& p : g_pair.picture) p = PairPicture{};
}

// One entry's objects on the context's device (a new device releases every slot's, and starts the ring afresh); a failure ends the instrument and is said once.
bool pairEnsure(ID3D11DeviceContext* ctx, PairEntry& e) {
    if (g_pair.failed) return false;
    ID3D11Device* d = nullptr;
    if (!guarded("srv.turret.pair.device", [&] { ctx->GetDevice(&d); }) || !d) {
        st::release(d);
        return false;
    }
    if (d != g_pair.device) {
        pairReleaseObjects();
        pairResetSlots();
        g_pair.device = d;
    }
    HRESULT hr = S_OK;
    const char* what = nullptr;
    const bool ran = guarded("srv.turret.pair.make", [&] {
        if (!e.so) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kPairBufferBytes;
            bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
            hr = d->CreateBuffer(&bd, nullptr, &e.so);
            if (FAILED(hr)) { what = "the stream-output buffer"; return; }
        }
        if (!e.stage) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kPairBufferBytes;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr = d->CreateBuffer(&bd, nullptr, &e.stage);
            if (FAILED(hr)) { what = "the staging buffer"; return; }
        }
        if (!e.stats) {
            const D3D11_QUERY_DESC qd{D3D11_QUERY_SO_STATISTICS, 0};
            hr = d->CreateQuery(&qd, &e.stats);
            if (FAILED(hr)) { what = "the stream-output-statistics query"; return; }
        }
    });
    st::release(d);
    if (!ran || what) {
        Log::get().note("srv turret radar pair: the capture's objects could not be made (%s, 0x%08X); the pair capture is off, the correction is unaffected.", what ? what : "a fault",
                        static_cast<unsigned>(hr));
        pairReleaseObjects();
        pairResetSlots();
        g_pair.failed = true;
        return false;
    }
    return true;
}

// radarCorrectionBegin, before the bind: the entry this draw goes into, when the frame is a capture frame and the eye's billboard of this kind is not captured yet; null otherwise. The
// frame's slot is opened at its first such draw (a ring in flight everywhere skips the frame, counted).
PairEntry* pairTake(ID3D11DeviceContext* ctx, int eye, int kind) {
    if (!g_pair.wantFrame || g_pair.failed || (eye != 0 && eye != 1) || (kind != 1 && kind != 2) || !g_soGs[st::kSceneSignature]) return nullptr;
    if (g_pair.current < 0) {
        int idle = -1;
        for (int i = 0; i < static_cast<int>(kPairSlots); ++i)
            if (!g_pair.slot[i].pending && !g_pair.slot[i].open) {
                idle = i;
                break;
            }
        if (idle < 0) {
            g_pc.skippedBusy.fetch_add(1, std::memory_order_relaxed);
            g_pair.wantFrame = false;
            return nullptr;
        }
        PairSlot& s = g_pair.slot[idle];
        for (auto& row : s.e)
            for (PairEntry& e : row) {
                ID3D11Buffer* so = e.so;
                ID3D11Buffer* stage = e.stage;
                ID3D11Query* stats = e.stats;
                e = PairEntry{};
                e.so = so;
                e.stage = stage;
                e.stats = stats;
            }
        s.frame = g_frame;
        s.open = true;
        s.pending = false;
        s.waited = 0;
        s.dump = g_pair.dumpFrame;
        s.dumpIndex = g_pair.dumpIndex;
        s.callSerial = g_corr.callSerial;
        for (int i = 0; i < 3; ++i) s.callD[i] = g_corr.callD[i];
        s.yawRate = g_corr.yawRateDegS;
        s.haveRate = g_corr.haveRate;
        s.atRest = g_corr.atRest;
        s.quiet = g_pair.quietFrame;
        const uint64_t nowMs = GetTickCount64();
        s.timeS = double(nowMs) / 1000.0;
        g_pair.current = idle;
        g_pair.lastFrame = g_frame;
        if (!s.quiet) g_pair.lastMs = nowMs;   // a learning capture leaves the 2 s cadence where it is
        if (s.dump) ++g_pair.windowFrames;
    }
    PairEntry& e = g_pair.slot[g_pair.current].e[eye][kind - 1];
    if (e.armed || !pairEnsure(ctx, e)) return nullptr;
    if (g_pair.current < 0) return nullptr;   // a new device reset the ring just now: this frame is not captured
    ID3D11Buffer* so[1] = {};
    const bool read = guarded("srv.turret.pair.so", [&] { ctx->SOGetTargets(1, so); });
    const bool busy = !read || so[0] != nullptr;
    st::release(so[0]);
    return busy ? nullptr : &g_pair.slot[g_pair.current].e[eye][kind - 1];
}

// Right after the bind (the twin is bound): what the draw runs with, and the stream-output target and the query around its scope.
void pairBegin(ID3D11DeviceContext* ctx, PairEntry& e, int eye, const float dEye[3], const st::TurretParams& p, const void* b1, const float rows[12], int lockWhy, const float target[3]) {
    for (int i = 0; i < 3; ++i) {
        e.dEye[i] = dEye[i];
        e.fwd[i] = rows[8 + i];   // row 279, as the delta's conversion read it
        e.target[i] = target[i];
    }
    e.lockWhy = lockWhy;
    e.contentInv = p.invZc;
    e.tan[0] = p.l;
    e.tan[1] = p.r;
    e.tan[2] = p.dn;
    e.tan[3] = p.up;
    e.b1 = b1;
    e.t4 = g_lastT4;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    if (guarded("srv.turret.pair.vp", [&] { ctx->RSGetViewports(&n, vp); }) && n > 0) {
        e.vpW = vp[0].Width;
        e.vpH = vp[0].Height;
    }
    uint64_t seq = 0;
    float head[12] = {}, eyeToHead[2][12] = {};
    e.pose = nativeTemporalHeadPose(&seq, head, eyeToHead) && st::eyePoseOf(head, eyeToHead[eye], e.R, e.E);
    g_pair.scope = &e;
    e.armed = guarded("srv.turret.pair.begin", [&] {
        ID3D11Buffer* target = e.so;
        const UINT offset = 0;
        ctx->SOSetTargets(1, &target, &offset);
        ctx->Begin(e.stats);
    });
    if (!e.armed) g_pair.failed = true;   // a fault in the instrument's own calls ends the instrument, not the correction
}
// Right before the restore: the query ends and the stream-output target comes off.
void pairEnd(ID3D11DeviceContext* ctx) {
    PairEntry* e = g_pair.scope;
    if (!e) return;
    if (!guarded("srv.turret.pair.end", [&] {
            if (e->armed) ctx->End(e->stats);
            ctx->SOSetTargets(0, nullptr, nullptr);
        })) {
        e->armed = false;
        g_pair.failed = true;
    }
}
// After the restore: the records are copied for the later read.
void pairFinish(ID3D11DeviceContext* ctx) {
    PairEntry* e = g_pair.scope;
    g_pair.scope = nullptr;
    if (!e || !e->armed) return;
    e->copied = guarded("srv.turret.pair.copy", [&] { vScreenCopyResourceRaw(ctx, e->stage, e->so); });
    if (!e->copied) {
        e->armed = false;
        return;
    }
    if (g_pair.current >= 0 && g_pair.slot[g_pair.current].dump)
        for (int eye = 0; eye < 2; ++eye)
            for (int k = 0; k < 2; ++k)
                if (&g_pair.slot[g_pair.current].e[eye][k] == e) ++g_pair.windowCaptured[eye][k];
}
// srvTurretHudEnd, after the probe's end: a moved radar billboard's issues, tallied by kind (and kept with its pair entry when captured).
void radarScopeEnd() {
    if (!g_radarScope.kind) return;
    const int k = g_radarScope.kind - 1;
    g_pc.draws[k].fetch_add(1, std::memory_order_relaxed);
    g_pc.issuesMoved[k].fetch_add(g_radarScope.ours, std::memory_order_relaxed);
    g_pc.inScopeOther[k].fetch_add(g_radarScope.other, std::memory_order_relaxed);
    g_pc.perDraw[k][g_radarScope.issues < 3 ? g_radarScope.issues : 3].fetch_add(1, std::memory_order_relaxed);
    if (g_pair.scope) {
        g_pair.scope->issues = g_radarScope.issues;
        g_pair.scope->ours = g_radarScope.ours;
        g_pair.scope->other = g_radarScope.other;
    }
    g_radarScope = RadarScope{};
    detail::g_srvTurretHudCounting.store(false, std::memory_order_relaxed);
}

// One quad of a capture: up to six records (two triangles) from `r`. Its pre-move w range, the centre of its NDC box before and after the move, the mean of its vertices in the eye before
// and after (X right, Y up, Z forward, from the clip positions with the tangents the shader was given), and its distinct UV corners (up to four). False when a vertex is behind the eye.
struct QuadMeasure {
    double wLo = 1.0e30, wHi = -1.0e30, preNdc[2] = {}, postNdc[2] = {}, preEye[3] = {}, postEye[3] = {};
    float uv[4][2] = {};
    uint32_t uvN = 0;
};
bool measureQuad(const float* r, uint32_t n, const float tan[4], QuadMeasure& q) {
    const st::Family& fam = st::kFamilies[kRadarFamily];
    const uint32_t stride = fam.strideBytes / 4;
    if (!r || n < 3) return false;
    double pre[4] = {1.0e30, -1.0e30, 1.0e30, -1.0e30}, post[4] = {1.0e30, -1.0e30, 1.0e30, -1.0e30};
    const auto eyeOf = [&](const float* c, double out[3]) {
        const double w = c[3];
        out[0] = c[0] * (double(tan[1]) - tan[0]) / 2.0 + w * (double(tan[1]) + tan[0]) / 2.0;
        out[1] = c[1] * (double(tan[3]) - tan[2]) / 2.0 + w * (double(tan[3]) + tan[2]) / 2.0;
        out[2] = w;
    };
    for (uint32_t i = 0; i < n; ++i) {
        const float* v = r + static_cast<size_t>(stride) * i;
        const float* a = v + fam.preFloat;
        const float* b = v + fam.posFloat;
        if (!(a[3] > 1.0e-6f) || !(b[3] > 1.0e-6f)) return false;
        q.wLo = std::min(q.wLo, double(a[3]));
        q.wHi = std::max(q.wHi, double(a[3]));
        const double ax = double(a[0]) / a[3], ay = double(a[1]) / a[3], bx = double(b[0]) / b[3], by = double(b[1]) / b[3];
        pre[0] = std::min(pre[0], ax);
        pre[1] = std::max(pre[1], ax);
        pre[2] = std::min(pre[2], ay);
        pre[3] = std::max(pre[3], ay);
        post[0] = std::min(post[0], bx);
        post[1] = std::max(post[1], bx);
        post[2] = std::min(post[2], by);
        post[3] = std::max(post[3], by);
        double pe[3], qe[3];
        eyeOf(a, pe);
        eyeOf(b, qe);
        for (int k = 0; k < 3; ++k) {
            q.preEye[k] += pe[k] / n;
            q.postEye[k] += qe[k] / n;
        }
        const float u = v[kPairUvFloat], w = v[kPairUvFloat + 1];
        bool seen = false;
        for (uint32_t c = 0; c < q.uvN; ++c) seen = seen || (q.uv[c][0] == u && q.uv[c][1] == w);
        if (!seen && q.uvN < 4) {
            q.uv[q.uvN][0] = u;
            q.uv[q.uvN][1] = w;
            ++q.uvN;
        }
    }
    q.preNdc[0] = (pre[0] + pre[1]) * 0.5;
    q.preNdc[1] = (pre[2] + pre[3]) * 0.5;
    q.postNdc[0] = (post[0] + post[1]) * 0.5;
    q.postNdc[1] = (post[2] + post[3]) * 0.5;
    return true;
}
const char* kKindName[3] = {"?", "disc", "icon"};

// One captured billboard, said: what it ran with, its issues, and each issue's quad (up to kPairGroups; the records of the scope's issues follow one another).
void pairLogEntry(const PairSlot& s, int eye, int kind, PairEntry& e, const float* records, const D3D11_QUERY_DATA_SO_STATISTICS& so) {
    const st::Family& fam = st::kFamilies[kRadarFamily];
    const uint32_t stride = fam.strideBytes / 4, capacity = kPairBufferBytes / fam.strideBytes / 3 * 3;
    const uint64_t written = so.NumPrimitivesWritten * 3;
    const uint32_t vertices = static_cast<uint32_t>(written < capacity ? written : capacity);
    const uint32_t perIssue = e.issues && vertices % e.issues == 0 ? vertices / e.issues : vertices;
    char tag[64] = "";
    if (s.dump) std::snprintf(tag, sizeof(tag), " (dump %ls, frame %u of %u)", g_pair.stamp, s.dumpIndex + 1, kPairDumpFrames);
    char zc[32] = "none (the plain delta)";
    if (e.contentInv > 0.0f) std::snprintf(zc, sizeof(zc), "%.3f m", 1.0 / double(e.contentInv));
    char line[2400];
    int len = std::snprintf(line, sizeof(line),
                            "srv turret radar pair: frame %u eye %d %s%s: t4 %06X, VS b1 %06X, forward row 279 (%.4f,%.4f,%.4f), setter call #%llu d (%.2f,%.2f,%.2f) cm, delta in the eye "
                            "(%.2f,%.2f,%.2f) cm, z_c %s, viewport %.0fx%.0f; issues %u (our shader %u, another %u), prims %llu (needed %llu)",
                            s.frame, eye, kKindName[kind], tag, holoShortId(e.t4), holoShortId(e.b1), static_cast<double>(e.fwd[0]), static_cast<double>(e.fwd[1]),
                            static_cast<double>(e.fwd[2]), static_cast<unsigned long long>(s.callSerial), static_cast<double>(s.callD[0]) * 100.0, static_cast<double>(s.callD[1]) * 100.0,
                            static_cast<double>(s.callD[2]) * 100.0, static_cast<double>(e.dEye[0]) * 100.0, static_cast<double>(e.dEye[1]) * 100.0, static_cast<double>(e.dEye[2]) * 100.0, zc,
                            static_cast<double>(e.vpW), static_cast<double>(e.vpH), e.issues, e.ours, e.other, static_cast<unsigned long long>(so.NumPrimitivesWritten),
                            static_cast<unsigned long long>(so.PrimitivesStorageNeeded));
    st::TurretParams tp;
    tp.l = e.tan[0];
    tp.r = e.tan[1];
    tp.dn = e.tan[2];
    tp.up = e.tan[3];
    const double hx = double(e.vpW) * 0.5, hy = double(e.vpH) * 0.5;
    QuadMeasure first;
    for (uint32_t g = 0; g < kPairGroups && perIssue >= 3 && (g + 1) * perIssue <= vertices && len > 0 && len < static_cast<int>(sizeof(line)) - 420; ++g) {
        QuadMeasure q;
        if (!measureQuad(records + static_cast<size_t>(stride) * g * perIssue, perIssue < 6 ? perIssue : 6, e.tan, q)) {
            len += std::snprintf(line + len, sizeof(line) - len, "; issue %u: a vertex behind the eye", g);
            continue;
        }
        const double mx = (q.postNdc[0] - q.preNdc[0]) * hx, my = (q.postNdc[1] - q.preNdc[1]) * hy;
        if (g == 0) {
            first = q;
            e.measured = true;
            e.preNdc[0] = q.preNdc[0];
            e.preNdc[1] = q.preNdc[1];
            e.movePx[0] = mx;
            e.movePx[1] = my;
            for (int k = 0; k < 3; ++k) {
                e.preEye[k] = q.preEye[k];
                e.postEye[k] = q.postEye[k];
            }
            double ex[2];
            char expected[64] = "none";
            if (e.contentInv > 0.0f && st::contentShiftNdc(tp, q.preNdc[0], q.preNdc[1], e.dEye, 1.0 / double(e.contentInv), ex)) {
                e.expected = true;
                e.expectPx[0] = ex[0] * hx;
                e.expectPx[1] = ex[1] * hy;
                std::snprintf(expected, sizeof(expected), "(%.2f,%.2f) px", e.expectPx[0], e.expectPx[1]);
            }
            len += std::snprintf(line + len, sizeof(line) - len,
                                 "; issue 0: w=[%.3f,%.3f], centre in the eye pre (%.4f,%.4f,%.4f) post (%.4f,%.4f,%.4f) m, ndc pre (%.4f,%.4f), moved (%.2f,%.2f) px, expected %s, uv",
                                 q.wLo, q.wHi, q.preEye[0], q.preEye[1], q.preEye[2], q.postEye[0], q.postEye[1], q.postEye[2], q.preNdc[0], q.preNdc[1], mx, my, expected);
            for (uint32_t c = 0; c < q.uvN; ++c)
                len += std::snprintf(line + len, sizeof(line) - len, " (%.4f,%.4f)", static_cast<double>(q.uv[c][0]), static_cast<double>(q.uv[c][1]));
        } else {
            len += std::snprintf(line + len, sizeof(line) - len, "; issue %u: pre (%+.2f,%+.2f) px from issue 0's, moved (%.2f,%.2f) px, w=[%.3f,%.3f]", g,
                                 (q.preNdc[0] - first.preNdc[0]) * hx, (q.preNdc[1] - first.preNdc[1]) * hy, mx, my, q.wLo, q.wHi);
        }
    }
    if (vertices < 3) len += std::snprintf(line + len, sizeof(line) - len, "; nothing captured");
    if (e.measured && len > 0 && len < static_cast<int>(sizeof(line)) - 260) {   // THE LOCK: what the draw ran with, and how far the quad's centre moved and landed from its target
        if (e.lockWhy == kLockOk) {
            const double shift[3] = {e.postEye[0] - e.preEye[0], e.postEye[1] - e.preEye[1], e.postEye[2] - e.preEye[2]};
            const double off[3] = {e.postEye[0] - e.target[0], e.postEye[1] - e.target[1], e.postEye[2] - e.target[2]};
            const uint64_t su = static_cast<uint64_t>(std::sqrt(shift[0] * shift[0] + shift[1] * shift[1] + shift[2] * shift[2]) * 1.0e6 + 0.5),
                           ou = static_cast<uint64_t>(std::sqrt(off[0] * off[0] + off[1] * off[1] + off[2] * off[2]) * 1.0e6 + 0.5);
            g_lc.shiftSumUm.fetch_add(su, std::memory_order_relaxed);
            g_lc.offSumUm.fetch_add(ou, std::memory_order_relaxed);
            g_lc.shiftN.fetch_add(1, std::memory_order_relaxed);
            maxUm(g_lc.shiftMaxUm, su);
            maxUm(g_lc.offMaxUm, ou);
            len += std::snprintf(line + len, sizeof(line) - len,
                                 "; LOCKED: target in the eye (%.4f,%.4f,%.4f) m, applied shift (%.2f,%.2f,%.2f) cm, centre post minus target (%.2f,%.2f,%.2f) mm", static_cast<double>(e.target[0]),
                                 static_cast<double>(e.target[1]), static_cast<double>(e.target[2]), shift[0] * 100.0, shift[1] * 100.0, shift[2] * 100.0, off[0] * 1000.0, off[1] * 1000.0,
                                 off[2] * 1000.0);
        } else {
            len += std::snprintf(line + len, sizeof(line) - len, "; delta mode (%s)", kLockMissName[e.lockWhy]);
        }
    }
    if (!s.quiet) Log::get().note("%s", line);
}

// A slot whose entries are all read (or given up on): each eye's disc and icon compared, and in the dump window their centres in the seated frame.
void pairLogFrame(const PairSlot& s) {
    for (int eye = 0; eye < 2; ++eye) {
        const PairEntry& d = s.e[eye][0];
        const PairEntry& i = s.e[eye][1];
        if (!d.armed && !i.armed) continue;
        if (!d.measured || !i.measured) {
            if (!s.quiet)
                Log::get().note("srv turret radar pair: frame %u eye %d: disc %s, icon %s; nothing to compare", s.frame, eye, d.measured ? "measured" : d.armed ? "lost" : "not captured",
                                i.measured ? "measured" : i.armed ? "lost" : "not captured");
            continue;
        }
        const double dx = d.movePx[0] - i.movePx[0], dy = d.movePx[1] - i.movePx[1], apart = std::sqrt(dx * dx + dy * dy);
        g_pc.capFrames.fetch_add(1, std::memory_order_relaxed);
        if (apart > 1.0) g_pc.capApart.fetch_add(1, std::memory_order_relaxed);
        g_refNdc[eye][0] = d.preNdc[0];
        g_refNdc[eye][1] = d.preNdc[1];
        if (s.quiet) continue;   // a capture for THE LOCK's learning alone
        char de[48] = "none", ie[48] = "none";
        if (d.expected) std::snprintf(de, sizeof(de), "(%.2f,%.2f)", d.expectPx[0], d.expectPx[1]);
        if (i.expected) std::snprintf(ie, sizeof(ie), "(%.2f,%.2f)", i.expectPx[0], i.expectPx[1]);
        Log::get().note("srv turret radar pair: frame %u eye %d: disc moved (%.2f,%.2f) px (expected %s), icon moved (%.2f,%.2f) px (expected %s); disc minus icon (%.2f,%.2f) px, |%.2f| px%s",
                        s.frame, eye, d.movePx[0], d.movePx[1], de, i.movePx[0], i.movePx[1], ie, dx, dy, apart, apart > 1.0 ? " -- APART" : "");
        if (!s.dump) continue;
        char rate[32] = "unknown";
        if (s.haveRate) std::snprintf(rate, sizeof(rate), "%.1f deg/s", s.yawRate);
        double dPost[3] = {}, dPre[3] = {}, iPost[3] = {}, iPre[3] = {};
        if (d.pose && i.pose) {
            st::eyeToSeated(d.R, d.E, d.postEye, dPost);
            st::eyeToSeated(d.R, d.E, d.preEye, dPre);
            st::eyeToSeated(i.R, i.E, i.postEye, iPost);
            st::eyeToSeated(i.R, i.E, i.preEye, iPre);
            // THE LOCK: each kind's seated target (as it stands now), the shift its quad was given in the eye, and where its centre landed against the target
            char kinds[2][256];
            const PairEntry* both[2] = {&d, &i};
            const double* post[2] = {dPost, iPost};
            for (int k = 0; k < 2; ++k) {
                const PairEntry& e = *both[k];
                const st::LockTarget& t = g_lock[k];
                const double sh[3] = {e.postEye[0] - e.preEye[0], e.postEye[1] - e.preEye[1], e.postEye[2] - e.preEye[2]};
                if (t.learned)
                    std::snprintf(kinds[k], sizeof(kinds[k]), "%s target seated (%.4f,%.4f,%.4f) m, %s, applied shift in the eye (%.2f,%.2f,%.2f) cm, post minus target (%.1f,%.1f,%.1f) mm",
                                  k == 0 ? "disc" : "icon", t.seated[0], t.seated[1], t.seated[2], e.lockWhy == kLockOk ? "locked" : kLockMissName[e.lockWhy], sh[0] * 100.0, sh[1] * 100.0,
                                  sh[2] * 100.0, (post[k][0] - t.seated[0]) * 1000.0, (post[k][1] - t.seated[1]) * 1000.0, (post[k][2] - t.seated[2]) * 1000.0);
                else
                    std::snprintf(kinds[k], sizeof(kinds[k]), "%s target none (%s), applied shift in the eye (%.2f,%.2f,%.2f) cm", k == 0 ? "disc" : "icon", kLockMissName[e.lockWhy],
                                  sh[0] * 100.0, sh[1] * 100.0, sh[2] * 100.0);
            }
            Log::get().note("srv turret radar pair: lock: frame %u eye %d (dump %ls, frame %u of %u): disc centre seated post (%.4f,%.4f,%.4f) pre (%.4f,%.4f,%.4f) m, icon post (%.4f,%.4f,%.4f) "
                            "pre (%.4f,%.4f,%.4f) m; turret yaw rate %s, %s; %s; %s",
                            s.frame, eye, g_pair.stamp, s.dumpIndex + 1, kPairDumpFrames, dPost[0], dPost[1], dPost[2], dPre[0], dPre[1], dPre[2], iPost[0], iPost[1], iPost[2], iPre[0], iPre[1],
                            iPre[2], rate, s.atRest ? "at rest" : "not at rest", kinds[0], kinds[1]);
        } else {
            Log::get().note("srv turret radar pair: lock: frame %u eye %d (dump %ls, frame %u of %u): no eye pose at the draw; turret yaw rate %s", s.frame, eye, g_pair.stamp, s.dumpIndex + 1,
                            kPairDumpFrames, rate);
        }
    }
}

// The frame boundary: each pending slot's entries read when the GPU has them, without waiting; a slot is closed when all are read or kPairGiveUpFrames have passed.
void pairReadSlot(ID3D11DeviceContext* ctx, PairSlot& s) {
    bool waiting = false;
    for (int eye = 0; eye < 2; ++eye) {
        for (int k = 0; k < 2; ++k) {
            PairEntry& e = s.e[eye][k];
            if (!e.armed || !e.copied || e.read) continue;
            D3D11_QUERY_DATA_SO_STATISTICS so{};
            HRESULT hr = S_FALSE;
            const bool ran = guarded("srv.turret.pair.stats", [&] { hr = ctx->GetData(e.stats, &so, sizeof(so), D3D11_ASYNC_GETDATA_DONOTFLUSH); });
            if (!ran || FAILED(hr)) {
                e.read = true;
                g_pc.lost.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (hr != S_OK) {
                waiting = true;
                continue;
            }
            D3D11_MAPPED_SUBRESOURCE m{};
            HRESULT mh = E_FAIL;
            const bool mapped = guarded("srv.turret.pair.map", [&] { mh = ctx->Map(e.stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m); });
            if (mapped && mh == DXGI_ERROR_WAS_STILL_DRAWING) {
                waiting = true;
                continue;
            }
            if (!mapped || FAILED(mh) || !m.pData) {
                e.read = true;
                g_pc.lost.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            guarded("srv.turret.pair.log", [&] { pairLogEntry(s, eye, k + 1, e, static_cast<const float*>(m.pData), so); });
            guarded("srv.turret.pair.unmap", [&] { ctx->Unmap(e.stage, 0); });
            e.read = true;
        }
    }
    if (waiting && ++s.waited <= kPairGiveUpFrames) return;
    if (waiting)
        for (auto& row : s.e)
            for (PairEntry& e : row)
                if (e.armed && e.copied && !e.read) {
                    e.read = true;
                    g_pc.lost.fetch_add(1, std::memory_order_relaxed);
                }
    lockLearn(s);   // THE LOCK: a frame at rest teaches the targets
    pairLogFrame(s);
    s.pending = false;
}

// THE PICTURES: eye 0's t4 of a radar billboard in the dump window, copied once (the first frame of the window that draws it moved), at the bind; written at a later boundary.
void pictureTake(ID3D11DeviceContext* ctx, int kind) {
    PairPicture& p = g_pair.picture[kind - 1];
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Resource* res = nullptr;
    ID3D11Texture2D* tex = nullptr;
    ID3D11Device* d = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    HRESULT hr = E_FAIL;
    const char* why = nullptr;
    const bool ran = guarded("srv.turret.picture.take", [&] {
        ctx->PSGetShaderResources(4, 1, &srv);
        if (!srv) { why = "no t4"; return; }
        srv->GetResource(&res);
        if (!res || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || !tex) { why = "t4 is not a 2D texture"; return; }
        tex->GetDesc(&td);
        if (td.SampleDesc.Count != 1) { why = "t4 is multisampled"; return; }
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ctx->GetDevice(&d);
        if (!d) { why = "no device"; return; }
        hr = d->CreateTexture2D(&sd, nullptr, &p.stage);
        if (FAILED(hr) || !p.stage) { why = "the staging texture could not be made"; return; }
        vScreenCopyResourceRaw(ctx, p.stage, res);
    });
    p.id = res;
    p.w = td.Width;
    p.h = td.Height;
    p.format = static_cast<uint32_t>(td.Format);
    p.frame = g_frame;
    p.waited = 0;
    if (!ran || why) {
        st::release(p.stage);
        p.state = PictureState::kFailed;
        std::snprintf(p.why, sizeof(p.why), "%s (0x%08X)", why ? why : "a fault", static_cast<unsigned>(hr));
    } else {
        p.state = PictureState::kPending;
    }
    st::release(d);
    st::release(tex);
    st::release(res);
    st::release(srv);
}
// A 24-bit BMP of a grayscale picture (top row first in `gray`).
bool writeGrayBmp(const std::wstring& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& gray) {
    const uint32_t row = (w * 3 + 3) & ~3u, image = row * h;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + image;
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = static_cast<LONG>(h);
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = image;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    bool ok = fwrite(&fh, sizeof(fh), 1, f) == 1 && fwrite(&ih, sizeof(ih), 1, f) == 1;
    std::vector<uint8_t> line(row, 0);
    for (uint32_t y = 0; ok && y < h; ++y) {
        const uint8_t* src = gray.data() + static_cast<size_t>(h - 1 - y) * w;   // bottom-up
        for (uint32_t x = 0; x < w; ++x) line[x * 3] = line[x * 3 + 1] = line[x * 3 + 2] = src[x];
        ok = fwrite(line.data(), 1, row, f) == row;
    }
    return fclose(f) == 0 && ok;
}
void picturesRead(ID3D11DeviceContext* ctx) {
    for (int k = 0; k < 2; ++k) {
        PairPicture& p = g_pair.picture[k];
        if (p.state != PictureState::kPending || !p.stage) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT mh = E_FAIL;
        const bool mapped = guarded("srv.turret.picture.map", [&] { mh = ctx->Map(p.stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m); });
        if (mapped && mh == DXGI_ERROR_WAS_STILL_DRAWING && ++p.waited <= kPairGiveUpFrames) continue;
        if (!mapped || FAILED(mh) || !m.pData) {
            st::release(p.stage);
            p.state = PictureState::kFailed;
            std::snprintf(p.why, sizeof(p.why), "the copy could not be read (0x%08X)", static_cast<unsigned>(mh));
            Log::get().note("srv turret radar picture: dump %ls frame %u eye 0 %s: t4 %06X %ux%u: %s", g_pair.stamp, p.frame, kKindName[k + 1], holoShortId(p.id), p.w, p.h, p.why);
            continue;
        }
        const bool r32 = p.format == static_cast<uint32_t>(DXGI_FORMAT_R32_FLOAT) || p.format == static_cast<uint32_t>(DXGI_FORMAT_R32_TYPELESS);
        uint32_t content = 0;
        float lo = 0.0f, hi = 0.0f;
        bool written = false;
        std::wstring name;
        if (r32) {
            std::vector<float> v(static_cast<size_t>(p.w) * p.h);
            for (uint32_t y = 0; y < p.h; ++y) std::memcpy(v.data() + static_cast<size_t>(y) * p.w, static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, p.w * 4u);
            for (float f : v)
                if (std::isfinite(f) && f < 1000.0f) {
                    lo = content ? std::min(lo, f) : f;
                    hi = content ? std::max(hi, f) : f;
                    ++content;
                }
            std::vector<uint8_t> gray(v.size(), 0);
            const float span = hi - lo;
            for (size_t i = 0; i < v.size(); ++i)
                if (std::isfinite(v[i]) && v[i] < 1000.0f) gray[i] = static_cast<uint8_t>(64.0f + (span > 0.0f ? 191.0f * (v[i] - lo) / span : 191.0f) + 0.5f);
            const std::wstring dir = Log::get().dir() + L"\\eyes";
            CreateDirectoryW(dir.c_str(), nullptr);
            name = std::wstring(L"eye_") + g_pair.stamp + (k == 0 ? L"_RadarDiscT4.bmp" : L"_RadarIconT4.bmp");
            written = writeGrayBmp(dir + L"\\" + name, p.w, p.h, gray);
        }
        guarded("srv.turret.picture.unmap", [&] { ctx->Unmap(p.stage, 0); });
        st::release(p.stage);
        p.state = written ? PictureState::kWritten : PictureState::kFailed;
        if (!r32)
            std::snprintf(p.why, sizeof(p.why), "format %u is not read", p.format);
        else if (!written)
            std::snprintf(p.why, sizeof(p.why), "the file could not be written");
        Log::get().note("srv turret radar picture: dump %ls frame %u eye 0 %s: t4 %06X %ux%u format %u, content (t4 < 1000) %u of %u texels, t4 %.4f to %.4f; %ls%ls%s",
                        g_pair.stamp, p.frame, kKindName[k + 1], holoShortId(p.id), p.w, p.h, p.format, content, p.w * p.h, static_cast<double>(lo), static_cast<double>(hi),
                        written ? L"written " : L"", written ? name.c_str() : L"", written ? " (black: empty, t4 >= 1000; gray 64..255: content, brighter is a larger t4)" : p.why);
    }
}
const char* pictureText(const PairPicture& p) {
    switch (p.state) {
        case PictureState::kPending: return "copied, not yet written";
        case PictureState::kWritten: return "written";
        case PictureState::kFailed: return "FAILED";
        default: return "not taken (no eye-0 billboard of the kind moved in the window)";
    }
}

// THE CONSTANTS' COMPARISON for the frame that just ended: each eye with both kinds moved.
void constantsCompare(uint32_t frame) {
    for (int eye = 0; eye < 2; ++eye) {
        const FrameMove& d = g_frameMove[eye][0];
        const FrameMove& i = g_frameMove[eye][1];
        if (d.frame != frame || i.frame != frame) continue;
        if (!(d.contentInv > 0.0f) || !(i.contentInv > 0.0f)) {
            g_pc.constNoDepth.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        st::TurretParams td, ti;
        td.l = d.tan[0];
        td.r = d.tan[1];
        td.dn = d.tan[2];
        td.up = d.tan[3];
        ti.l = i.tan[0];
        ti.r = i.tan[1];
        ti.dn = i.tan[2];
        ti.up = i.tan[3];
        double sd[2], si[2];
        if (!st::contentShiftNdc(td, g_refNdc[eye][0], g_refNdc[eye][1], d.dEye, 1.0 / double(d.contentInv), sd) ||
            !st::contentShiftNdc(ti, g_refNdc[eye][0], g_refNdc[eye][1], i.dEye, 1.0 / double(i.contentInv), si))
            continue;
        const double dx = (sd[0] - si[0]) * double(d.vpW) * 0.5, dy = (sd[1] - si[1]) * double(d.vpH) * 0.5;
        g_pc.constFrames.fetch_add(1, std::memory_order_relaxed);
        if (std::sqrt(dx * dx + dy * dy) > 1.0) g_pc.constApart.fetch_add(1, std::memory_order_relaxed);
    }
}

// srvTurretHudFrameBoundary, after the frame count moved on: the ended frame's slot goes in flight, the constants are compared, what the GPU has done is read, an eye dump taken up, and
// whether the frame now starting is a capture frame decided.
void pairBoundary(ID3D11DeviceContext* ctx) {
    if (g_pair.current >= 0) {
        PairSlot& s = g_pair.slot[g_pair.current];
        s.open = false;
        bool any = false;
        for (auto& row : s.e)
            for (PairEntry& e : row) any = any || (e.armed && e.copied);
        s.pending = any;
        s.waited = 0;
        if (any) g_pc.captured.fetch_add(1, std::memory_order_relaxed);
        g_pair.current = -1;
    }
    constantsCompare(g_frame - 1u);
    if (ctx) {
        for (PairSlot& s : g_pair.slot)
            if (s.pending) pairReadSlot(ctx, s);
        picturesRead(ctx);
    }
    const uint32_t serial = g_eyeDumpSerial.load(std::memory_order_acquire);
    const bool newDump = serial != g_pair.dumpSerial;
    if ((newDump || g_pair.dumpLeft == 0) && g_pair.windowOpen) {   // the window that ran is over (or a new dump cuts it short)
        g_pair.windowOpen = false;
        if (g_pair.windowRadar > 0)
            Log::get().note("srv turret radar pair: dump %ls window ended: %u of %u frames captured (eye 0 disc %u, icon %u; eye 1 disc %u, icon %u); pictures: disc %s, icon %s; radar "
                            "billboard draws seen %u",
                            g_pair.stamp, g_pair.windowFrames, kPairDumpFrames, g_pair.windowCaptured[0][0], g_pair.windowCaptured[0][1], g_pair.windowCaptured[1][0],
                            g_pair.windowCaptured[1][1], pictureText(g_pair.picture[0]), pictureText(g_pair.picture[1]), g_pair.windowRadar);
    }
    if (newDump) {
        g_pair.dumpSerial = serial;
        wcsncpy_s(g_pair.stamp, g_eyeDumpStamp, _TRUNCATE);
        g_pair.dumpLeft = kPairDumpFrames;
        g_pair.windowOpen = true;
        g_pair.windowFrames = g_pair.windowRadar = 0;
        for (auto& row : g_pair.windowCaptured)
            for (uint32_t& n : row) n = 0;
        for (PairPicture& p : g_pair.picture) {
            st::release(p.stage);
            p = PairPicture{};
        }
    }
    g_pair.dumpFrame = g_pair.dumpLeft > 0;
    if (g_pair.dumpFrame) {
        g_pair.dumpIndex = kPairDumpFrames - g_pair.dumpLeft;
        --g_pair.dumpLeft;
    }
    // THE LOCK: a reset the frame tick asked for; and while a kind has no target, a capture every kLockLearnEveryFrames frames (quiet: for the learning alone, no lines)
    const uint32_t gen = g_lockResetGen.load(std::memory_order_acquire);
    if (gen != g_lockResetSeen) {
        g_lockResetSeen = gen;
        const bool had = g_lock[0].learned || g_lock[1].learned || g_lock[0].n || g_lock[1].n;
        lockReset();
        g_rest = st::RestWatch{};
        g_lc.resets.fetch_add(1, std::memory_order_relaxed);
        Log::get().note("srv turret radar lock: targets reset (%s)%s; each kind keeps the delta until it is learned again",
                        st::lockResetName(static_cast<st::LockReset>(g_lockResetWhy.load(std::memory_order_relaxed))), had ? "" : ", none had been learned");
    }
    const bool timed = g_pair.dumpFrame || GetTickCount64() - g_pair.lastMs >= kPairEveryMs;
    const bool learnTick = (!g_lock[0].learned || !g_lock[1].learned) && g_frame - g_pair.lastFrame >= st::kLockLearnEveryFrames;
    g_pair.wantFrame = timed || learnTick;
    g_pair.quietFrame = !timed;
}

// The frame tick's 10 s line (with the correction line, whenever the anchor yield was on in the window).
void pairWindow(bool yieldSeen) {
    uint64_t v[2][9];
    for (int k = 0; k < 2; ++k) {
        v[k][0] = g_pc.draws[k].exchange(0, std::memory_order_relaxed);
        v[k][1] = g_pc.issuesMoved[k].exchange(0, std::memory_order_relaxed);
        v[k][2] = g_pc.inScopeOther[k].exchange(0, std::memory_order_relaxed);
        v[k][3] = g_pc.declined[k].exchange(0, std::memory_order_relaxed);
        v[k][4] = g_pc.outside[k].exchange(0, std::memory_order_relaxed);
        for (int n = 0; n < 4; ++n) v[k][5 + n] = g_pc.perDraw[k][n].exchange(0, std::memory_order_relaxed);
    }
    uint64_t lk[kLockMissCount];   // THE LOCK's window
    lk[kLockOk] = g_lc.locked.exchange(0, std::memory_order_relaxed);
    for (int r = 1; r < kLockMissCount; ++r) lk[r] = g_lc.fallback[r].exchange(0, std::memory_order_relaxed);
    const uint64_t shN = g_lc.shiftN.exchange(0, std::memory_order_relaxed), shSum = g_lc.shiftSumUm.exchange(0, std::memory_order_relaxed),
                   shMax = g_lc.shiftMaxUm.exchange(0, std::memory_order_relaxed), offSum = g_lc.offSumUm.exchange(0, std::memory_order_relaxed),
                   offMax = g_lc.offMaxUm.exchange(0, std::memory_order_relaxed), resets = g_lc.resets.exchange(0, std::memory_order_relaxed),
                   relearned = g_lc.relearned.exchange(0, std::memory_order_relaxed), restFrames = g_lc.restFrames.exchange(0, std::memory_order_relaxed),
                   lockFrames = g_lc.frames.exchange(0, std::memory_order_relaxed);
    if (yieldSeen) {
        char targets[2][96];
        for (int k = 0; k < 2; ++k) {
            const st::LockTarget& t = g_lock[k];   // the render thread's; read for the line only
            if (t.learned)
                std::snprintf(targets[k], sizeof(targets[k]), "seated (%.4f,%.4f,%.4f) m", t.seated[0], t.seated[1], t.seated[2]);
            else
                std::snprintf(targets[k], sizeof(targets[k]), "none (%u of %u rest samples)", t.n, st::kLockLearnSamples);
        }
        Log::get().note("srv turret radar lock: 10 s: draws locked %llu, on the delta %llu (no target yet %llu, not a single quad %llu, no eye pose %llu, target too far %llu); |T - C| (the shift a "
                        "captured locked draw was given) mean %.2f max %.2f cm, its centre off the target mean %.2f max %.2f mm, over %llu captured draws; targets: disc %s, icon %s; reset %llu, "
                        "re-learned %llu samples; frames at rest %llu of %llu",
                        static_cast<unsigned long long>(lk[kLockOk]), static_cast<unsigned long long>(lk[1] + lk[2] + lk[3] + lk[4]), static_cast<unsigned long long>(lk[kLockNoTarget]),
                        static_cast<unsigned long long>(lk[kLockShape]), static_cast<unsigned long long>(lk[kLockNoPose]), static_cast<unsigned long long>(lk[kLockFar]),
                        shN ? double(shSum) * 1.0e-4 / double(shN) : 0.0, double(shMax) * 1.0e-4, shN ? double(offSum) * 1.0e-3 / double(shN) : 0.0, double(offMax) * 1.0e-3,
                        static_cast<unsigned long long>(shN), targets[0], targets[1], static_cast<unsigned long long>(resets), static_cast<unsigned long long>(relearned),
                        static_cast<unsigned long long>(restFrames), static_cast<unsigned long long>(lockFrames));
    }
    const uint64_t constFrames = g_pc.constFrames.exchange(0, std::memory_order_relaxed), constApart = g_pc.constApart.exchange(0, std::memory_order_relaxed),
                   constNoDepth = g_pc.constNoDepth.exchange(0, std::memory_order_relaxed), capFrames = g_pc.capFrames.exchange(0, std::memory_order_relaxed),
                   capApart = g_pc.capApart.exchange(0, std::memory_order_relaxed), captured = g_pc.captured.exchange(0, std::memory_order_relaxed),
                   skipped = g_pc.skippedBusy.exchange(0, std::memory_order_relaxed), lost = g_pc.lost.exchange(0, std::memory_order_relaxed);
    if (!yieldSeen) return;
    char kinds[2][256];
    for (int k = 0; k < 2; ++k)
        std::snprintf(kinds[k], sizeof(kinds[k]),
                      "%s: draws moved %llu, issues moved %llu, issues not moved %llu (another shader in the scope %llu, declined draws %llu, outside a scope %llu), issues per moved draw "
                      "0/1/2/3+: %llu/%llu/%llu/%llu",
                      kKindName[k + 1], static_cast<unsigned long long>(v[k][0]), static_cast<unsigned long long>(v[k][1]),
                      static_cast<unsigned long long>(v[k][2] + v[k][3] + v[k][4]), static_cast<unsigned long long>(v[k][2]), static_cast<unsigned long long>(v[k][3]),
                      static_cast<unsigned long long>(v[k][4]), static_cast<unsigned long long>(v[k][5]), static_cast<unsigned long long>(v[k][6]),
                      static_cast<unsigned long long>(v[k][7]), static_cast<unsigned long long>(v[k][8]));
    Log::get().note("srv turret radar pair: 10 s: %s; %s; disc and icon moved apart on screen (over 1 px): by the constants %llu of %llu eye frames (no content depth %llu), by the capture %llu "
                    "of %llu captured eye frames; capture frames %llu, skipped (ring busy) %llu, entries lost %llu%s",
                    kinds[0], kinds[1], static_cast<unsigned long long>(constApart), static_cast<unsigned long long>(constFrames), static_cast<unsigned long long>(constNoDepth),
                    static_cast<unsigned long long>(capApart), static_cast<unsigned long long>(capFrames), static_cast<unsigned long long>(captured),
                    static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(lost), g_pair.failed ? "; the capture is OFF (its objects could not be made)" : "");
}

// srvTurretHudBegin, at a draw of family 7 or 8 while the anchor yield is on (anchorLeaves): true when the draw is moved and bound (srvTurretHudEnd puts the game's stage back).
bool radarCorrectionBegin(ID3D11DeviceContext* ctx, int family, int eye) {
    if (eye != 0 && eye != 1) return false;
    // the hologram's source instrument keeps its rounds while the correction is on
    if (family == kHoloFamilyA || family == kHoloFamilyB) holoCollect(ctx, family, eye, g_probe[eye][family].ordinal++);
    bool lastFrameOnly = false;
    const int kind = billboardKind(ctx, lastFrameOnly);
    if (family != 7) {   // family 8 is never moved; one on a radar writer's target would contradict the model
        if (kind) g_cc.family8.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto leave = [kind](int why) {
        g_cc.left[why].fetch_add(1, std::memory_order_relaxed);
        if (kind) g_pc.declined[kind - 1].fetch_add(1, std::memory_order_relaxed);   // THE PAIR CAPTURE's count: a radar billboard's issue that goes out unmoved
        return false;
    };
    if (g_pair.windowOpen) ++g_pair.windowRadar;
    if (lastFrameOnly) g_cc.lastFrameOnly.fetch_add(1, std::memory_order_relaxed);
    if (!kind) return leave(kCorrNoMatch);
    if (g_movedAt[eye][kind - 1] == g_frame + 1u) return leave(kCorrExtra);   // one disc and one icon in each eye each frame
    if (g_corr.frame != g_frame) {
        g_corr = FrameCorrection{};
        g_corr.frame = g_frame;
        g_corr.reason = correctionCompute(ctx, eye, g_corr);
        if (g_corr.reason != kCorrOk) g_rest = st::RestWatch{};   // THE LOCK's rest: a frame without its rates breaks it
    }
    if (g_corr.reason != kCorrOk) return leave(g_corr.reason);
    float rows[12];
    if (anchorDirCpuRows(ctx, nativeTemporalFrameSequence(), rows, nullptr, nullptr) != 0) return leave(kCorrNoRows);   // this draw's own camera rows, this host frame
    float dEye[3];
    st::radarDeltaInEye(rows, g_corr.deltaW, dEye);
    // THE LOCK: the kind's learned rest place, in this eye's space, when the draw is a single quad; else the delta, counted with its reason
    float tEye[3] = {};
    const int lockWhy = lockTargetInEye(ctx, eye, kind, tEye);
    const bool locked = lockWhy == kLockOk;
    if (!locked && dEye[0] == 0.0f && dEye[1] == 0.0f && dEye[2] == 0.0f) return leave(kCorrZero);   // nothing to move: the game's own place is right (no pop)
    st::EyeFrame ef;
    if (!drawEyeFrame(eye, ef)) return leave(kCorrNoEyeFrame);
    for (int i = 0; i < 3; ++i) ef.tRadar[i] = locked ? tEye[i] : dEye[i];
    // the translation alone (gap 0: every vertex moves by delta), and the game's own NDC depth (the hologram's scene-depth discard reads it), whatever fix.srv_turret_hud_depth says;
    // scaled to the content depth (THE CONTENT DEPTH: tr.w = 1 / z_c, or 0 for the plain delta)
    st::TurretParams cparams = st::withDepth(st::translateParamsFor(ef, st::Group::kRadar), st::DepthMode::kGame);
    cparams.invZc = radarContentInvDepth();
    if (!st::paramsUsable(cparams)) return leave(kCorrParams);
    if (locked) {   // ... or THE LOCK: tr is the target and each triangle's longest-edge midpoint is moved onto it (the content depth is not read then)
        cparams.lock = 1.0f;
        if (!st::paramsUsable(cparams)) return leave(kCorrParams);
    }
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    if (!guarded("srv.turret.corr.topology", [&] { ctx->IAGetPrimitiveTopology(&topo); }) || !st::topologyCarried(static_cast<int>(topo))) return leave(kCorrTopology);
    if (g_binding.needsRestore() || g_scissor.owed() || !ensureGs(ctx, st::kSceneSignature)) return leave(kCorrNotReady);
    ID3D11Buffer* const cbuf = g_constants[eye].get(ctx, cparams);
    if (!cbuf) return leave(kCorrNotReady);
    st::ExtraCb camera;
    if (!st::readGameCb(ctx, st::kGameCbSlot, camera)) return leave(kCorrNoGameCb);   // signature H's lighting position needs the draw's own camera rows at GS slot 1
    // THE PAIR CAPTURE (TEMPORARY): in a capture frame each eye's disc and icon go through the twin into the frame's slot; the probe's sample stands aside for such a draw (probeDue).
    PairEntry* const pair = pairTake(ctx, eye, kind);
    // THE GPU INSTRUMENT on the corrected path: once a second per eye, the disc and the icon in turn, through the stream-output twin (the quad probe line at the readback, logQuadProbe)
    Probe& probe = g_probe[eye][family];
    g_pairTaking = pair != nullptr;
    const bool sample = g_corrProbeKind[eye] == kind && probeDue(ctx, eye, family, probe.wantOrd);
    g_pairTaking = false;
    const bool twin = pair || (sample && g_soGs[st::kSceneSignature] && probe.soBuf && probe.soStats && probe.staging);
    if (g_binding.begin(ctx, twin ? g_soGs[st::kSceneSignature] : g_gs[st::kSceneSignature], cbuf, st::directSetGs, &camera) != st::Binding::Begin::kBound) return leave(kCorrBeginFailed);
    // every issue of the moved billboard is counted, with the shader bound when it went out (srvTurretHudNoteIssue; radarScopeEnd tallies it)
    g_radarScope = RadarScope{};
    g_radarScope.ctx = ctx;
    g_radarScope.gs = twin ? g_soGs[st::kSceneSignature] : g_gs[st::kSceneSignature];
    g_radarScope.kind = kind;
    detail::g_srvTurretHudCounting.store(true, std::memory_order_relaxed);
    if (pair) pairBegin(ctx, *pair, eye, dEye, cparams, camera.buffer, rows, lockWhy, tEye);
    if (locked)
        g_lc.locked.fetch_add(1, std::memory_order_relaxed);
    else
        g_lc.fallback[lockWhy].fetch_add(1, std::memory_order_relaxed);
    if (eye == 0 && g_pair.dumpFrame && g_pair.picture[kind - 1].state == PictureState::kNone) pictureTake(ctx, kind);   // THE PICTURES
    {   // THE CONSTANTS' COMPARISON: what this eye's billboard of this kind ran with
        FrameMove& fm = g_frameMove[eye][kind - 1];
        fm.frame = g_frame;
        for (int i = 0; i < 3; ++i) fm.dEye[i] = dEye[i];
        fm.contentInv = cparams.invZc;
        fm.tan[0] = cparams.l;
        fm.tan[1] = cparams.r;
        fm.tan[2] = cparams.dn;
        fm.tan[3] = cparams.up;
        D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        UINT n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        if (guarded("srv.turret.pair.framevp", [&] { ctx->RSGetViewports(&n, vp); }) && n > 0) {
            fm.vpW = vp[0].Width;
            fm.vpH = vp[0].Height;
        }
    }
    if (sample) {
        probeBegin(ctx, eye, family, probe.wantOrd, twin, cparams);
        probe.corr = true;
        probe.corrKind = kind;
        for (int i = 0; i < 3; ++i) probe.corrDelta[i] = dEye[i];
        g_corrProbeKind[eye] = 3 - kind;
    }
    if (g_scissorOpen.load(std::memory_order_relaxed)) {   // the game's scissor rectangle is from the unmoved place, as for the other moved draws
        st::Scissor::Seen seen;
        if (g_scissor.open(ctx, seen, true) == st::Scissor::Open::kOpened) g_scissorOpened.fetch_add(1, std::memory_order_relaxed);
    }
    g_movedAt[eye][kind - 1] = g_frame + 1u;
    g_cc.moved[eye].fetch_add(1, std::memory_order_relaxed);
    (kind == 1 ? g_cc.disc : g_cc.icon).fetch_add(1, std::memory_order_relaxed);
    if (cparams.invZc > 0.0f) {   // the content depth each moved draw ran with
        const uint64_t zum = static_cast<uint64_t>(1.0e6 / double(cparams.invZc) + 0.5);
        g_cc.zcSumUm.fetch_add(zum, std::memory_order_relaxed);
        g_cc.zcN.fetch_add(1, std::memory_order_relaxed);
        uint64_t z = g_cc.zcMinUm.load(std::memory_order_relaxed);
        while (zum < z && !g_cc.zcMinUm.compare_exchange_weak(z, zum, std::memory_order_relaxed)) {
        }
        z = g_cc.zcMaxUm.load(std::memory_order_relaxed);
        while (zum > z && !g_cc.zcMaxUm.compare_exchange_weak(z, zum, std::memory_order_relaxed)) {
        }
    } else {
        g_cc.noContentDepth.fetch_add(1, std::memory_order_relaxed);
    }
    const double norm = std::sqrt(double(g_corr.deltaW[0]) * g_corr.deltaW[0] + double(g_corr.deltaW[1]) * g_corr.deltaW[1] + double(g_corr.deltaW[2]) * g_corr.deltaW[2]);
    if (!g_corr.counted) {   // the per-frame figures, once a frame
        g_corr.counted = true;
        g_cc.frames.fetch_add(1, std::memory_order_relaxed);
        g_lc.frames.fetch_add(1, std::memory_order_relaxed);   // THE LOCK's: frames, and of them at rest
        if (g_corr.atRest) g_lc.restFrames.fetch_add(1, std::memory_order_relaxed);
        if (norm > st::kCorrectionLargeM) g_cc.large.fetch_add(1, std::memory_order_relaxed);
        const uint64_t um = static_cast<uint64_t>(norm * 1.0e6 + 0.5);
        g_cc.deltaSumUm.fetch_add(um, std::memory_order_relaxed);
        uint64_t cur = g_cc.deltaMaxUm.load(std::memory_order_relaxed);
        while (um > cur && !g_cc.deltaMaxUm.compare_exchange_weak(cur, um, std::memory_order_relaxed)) {
        }
        g_cc.yawSumMicro.fetch_add(static_cast<int64_t>(g_corr.yawDeg * 1.0e6), std::memory_order_relaxed);
        g_cc.pitchSumMicro.fetch_add(static_cast<int64_t>(g_corr.pitchDeg * 1.0e6), std::memory_order_relaxed);
        const uint64_t ym = static_cast<uint64_t>(std::fabs(g_corr.yawDeg) * 1000.0 + 0.5), pm = static_cast<uint64_t>(std::fabs(g_corr.pitchDeg) * 1000.0 + 0.5);
        cur = g_cc.yawMaxMilli.load(std::memory_order_relaxed);
        while (ym > cur && !g_cc.yawMaxMilli.compare_exchange_weak(cur, ym, std::memory_order_relaxed)) {
        }
        cur = g_cc.pitchMaxMilli.load(std::memory_order_relaxed);
        while (pm > cur && !g_cc.pitchMaxMilli.compare_exchange_weak(cur, pm, std::memory_order_relaxed)) {
        }
    }
    if (!g_corrFirstSaid) {
        g_corrFirstSaid = true;
        char zc[64] = "none (the plain delta)";
        if (cparams.invZc > 0.0f) std::snprintf(zc, sizeof(zc), "z_c %.3f m", 1.0 / static_cast<double>(cparams.invZc));
        Log::get().note("srv turret radar anchor: correction first applied at frame %u, eye %d, %s: delta (%.2f,%.2f,%.2f) cm in the world, (%.2f,%.2f,%.2f) cm in the eye; the shown call was "
                        "%.2f ms before the camera write; content depth %s",
                        g_frame, eye, kind == 1 ? "disc" : "icon", static_cast<double>(g_corr.deltaW[0]) * 100.0, static_cast<double>(g_corr.deltaW[1]) * 100.0,
                        static_cast<double>(g_corr.deltaW[2]) * 100.0, static_cast<double>(dEye[0]) * 100.0, static_cast<double>(dEye[1]) * 100.0, static_cast<double>(dEye[2]) * 100.0,
                        g_corr.gapMs, zc);
    }
    return true;
}

// The frame tick, at each 10 s boundary: the correction's line whenever the anchor yield was on in the window, even with nothing moved (the reasons say why); the baselines move on
// either way. No line at all means this code never ran.
void correctionWindow(bool yieldSeen) {
    struct Base {
        uint64_t moved0 = 0, moved1 = 0, disc = 0, icon = 0, frames = 0, deltaSumUm = 0, family8 = 0, lastFrameOnly = 0, large = 0, zcSumUm = 0, zcN = 0, noContentDepth = 0, cwDisc = 0,
                 cwIcon = 0, cwExtraFrames = 0, left[kCorrMissCount] = {};
        int64_t yawSumMicro = 0, pitchSumMicro = 0;
    };
    static Base b;
    Base n;
    n.moved0 = g_cc.moved[0].load(std::memory_order_relaxed);
    n.moved1 = g_cc.moved[1].load(std::memory_order_relaxed);
    n.disc = g_cc.disc.load(std::memory_order_relaxed);
    n.icon = g_cc.icon.load(std::memory_order_relaxed);
    n.frames = g_cc.frames.load(std::memory_order_relaxed);
    n.deltaSumUm = g_cc.deltaSumUm.load(std::memory_order_relaxed);
    n.family8 = g_cc.family8.load(std::memory_order_relaxed);
    n.lastFrameOnly = g_cc.lastFrameOnly.load(std::memory_order_relaxed);
    n.large = g_cc.large.load(std::memory_order_relaxed);
    n.zcSumUm = g_cc.zcSumUm.load(std::memory_order_relaxed);
    n.zcN = g_cc.zcN.load(std::memory_order_relaxed);
    n.noContentDepth = g_cc.noContentDepth.load(std::memory_order_relaxed);
    n.cwDisc = g_cwDisc.load(std::memory_order_relaxed);
    n.cwIcon = g_cwIcon.load(std::memory_order_relaxed);
    n.cwExtraFrames = g_cwExtraFrames.load(std::memory_order_relaxed);
    for (int i = 0; i < kCorrMissCount; ++i) n.left[i] = g_cc.left[i].load(std::memory_order_relaxed);
    n.yawSumMicro = g_cc.yawSumMicro.load(std::memory_order_relaxed);
    n.pitchSumMicro = g_cc.pitchSumMicro.load(std::memory_order_relaxed);
    const uint64_t deltaMaxUm = g_cc.deltaMaxUm.exchange(0, std::memory_order_relaxed), yawMax = g_cc.yawMaxMilli.exchange(0, std::memory_order_relaxed),
                   pitchMax = g_cc.pitchMaxMilli.exchange(0, std::memory_order_relaxed), zcMin = g_cc.zcMinUm.exchange(UINT64_MAX, std::memory_order_relaxed),
                   zcMax = g_cc.zcMaxUm.exchange(0, std::memory_order_relaxed);
    if (yieldSeen) {
        const uint64_t frames = n.frames - b.frames;
        const double perFrame = frames ? 1.0 / double(frames) : 0.0;
        const auto left = [&](int why) { return static_cast<unsigned long long>(n.left[why] - b.left[why]); };
        const uint64_t zcN = n.zcN - b.zcN;   // the moved draws that ran with a content depth (none: mean, min and max read 0)
        Log::get().note("srv turret radar anchor: 10 s correction: %llu billboard draws moved (eye 0 %llu, eye 1 %llu; disc %llu, icon %llu) over %llu frames, |delta| mean %.2f max %.2f cm "
                        "(large, over 10 cm: %llu frames), yaw mean %.3f max |%.3f| deg, pitch mean %.3f max |%.3f| deg; content depth z_c mean %.3f min %.3f max %.3f m (no content depth %llu draws); left to the game: no writer match %llu "
                        "(of them on last frame's radar target %llu), extra radar-target billboard %llu, no camera rows %llu, no shown call %llu, no anchor %llu, over 50 cm %llu, "
                        "delta 0 %llu (no pop), not ready %llu, no eye frame %llu, params unusable %llu, topology %llu, no game cb %llu, binding begin failed %llu; family 8 on a radar "
                        "target %llu; writers: 4EDE4BC9 %llu, 256x256 5B0068AF %llu (frames with more than two: %llu)",
                        static_cast<unsigned long long>((n.moved0 - b.moved0) + (n.moved1 - b.moved1)), static_cast<unsigned long long>(n.moved0 - b.moved0),
                        static_cast<unsigned long long>(n.moved1 - b.moved1), static_cast<unsigned long long>(n.disc - b.disc), static_cast<unsigned long long>(n.icon - b.icon),
                        static_cast<unsigned long long>(frames), double(n.deltaSumUm - b.deltaSumUm) * 1.0e-4 * perFrame, double(deltaMaxUm) * 1.0e-4,
                        static_cast<unsigned long long>(n.large - b.large), double(n.yawSumMicro - b.yawSumMicro) * 1.0e-6 * perFrame, double(yawMax) / 1000.0,
                        double(n.pitchSumMicro - b.pitchSumMicro) * 1.0e-6 * perFrame, double(pitchMax) / 1000.0,
                        zcN ? double(n.zcSumUm - b.zcSumUm) * 1.0e-6 / double(zcN) : 0.0, zcN ? double(zcMin) * 1.0e-6 : 0.0, zcN ? double(zcMax) * 1.0e-6 : 0.0,
                        static_cast<unsigned long long>(n.noContentDepth - b.noContentDepth), left(kCorrNoMatch), static_cast<unsigned long long>(n.lastFrameOnly - b.lastFrameOnly), left(kCorrExtra),
                        left(kCorrNoRows), left(kCorrNoCall), left(kCorrNoAnchor), left(kCorrOver), left(kCorrZero), left(kCorrNotReady), left(kCorrNoEyeFrame), left(kCorrParams),
                        left(kCorrTopology), left(kCorrNoGameCb), left(kCorrBeginFailed), static_cast<unsigned long long>(n.family8 - b.family8),
                        static_cast<unsigned long long>(n.cwDisc - b.cwDisc), static_cast<unsigned long long>(n.cwIcon - b.cwIcon),
                        static_cast<unsigned long long>(n.cwExtraFrames - b.cwExtraFrames));
    }
    b = n;
    pairWindow(yieldSeen);   // THE PAIR CAPTURE's 10 s line (TEMPORARY), beside this one
}

}  // namespace

// THE PAIR CAPTURE (TEMPORARY, issue 140): an eye dump was asked for (temporal_pass.cpp beginEyeRun, any thread). The render thread takes it up at its next frame boundary.
void srvTurretNoteEyeDump(const wchar_t* stamp) {
    wcsncpy_s(g_eyeDumpStamp, stamp ? stamp : L"", _TRUNCATE);
    g_eyeDumpSerial.fetch_add(1, std::memory_order_release);
}

// THE PAIR CAPTURE (TEMPORARY): a family-7 issue that went out without the pass's shader (vscreen.cpp srvTurretUnmovedNote, owner context, while the writers are watched), counted by the radar
// picture its t4 is (none: not a radar billboard).
void srvTurretRadarNoteUnmovedIssue(ID3D11DeviceContext* ctx, int family) {
    if (family != kRadarFamily || !ctx) return;
    bool lastFrameOnly = false;
    const int kind = billboardKind(ctx, lastFrameOnly);
    if (kind == 1 || kind == 2) g_pc.outside[kind - 1].fetch_add(1, std::memory_order_relaxed);
}

// The scene camera block's writes (vscreen.cpp hookedUnmap, before the real Unmap, while srvTurretWantsCameraRows()): rows 277..279 kept by buffer with the host frame, for the radar
// anchor's CPU path (anchorDirSample). 48 bytes read from the mapped block.
void srvTurretNoteCameraWrite(const void* res, const void* data, uint32_t bytes) {
    if (!res || !data || bytes < kCamRowsEnd) return;
    CamWrite* w = nullptr;
    for (CamWrite& c : g_camWrites)
        if (c.res == res) w = &c;
    if (!w) {
        w = &g_camWrites[g_camWriteNext];
        g_camWriteNext = (g_camWriteNext + 1) % kCamWrites;
        w->res = res;
    }
    std::memcpy(w->rows, static_cast<const float*>(data) + kCamRowsFloat, sizeof(w->rows));
    w->hostSeq = nativeTemporalFrameSequence();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    w->qpc = now.QuadPart;
    uint32_t none = 0;
    g_cameraWriteThread.compare_exchange_strong(none, GetCurrentThreadId(), std::memory_order_relaxed);
}

// The radar's picture writers (vscreen.cpp forwardWithVerdict, every owner-context draw while the anchor yield is on): the terrain schematic (vs 4EDE4BC9) and the Scorpion icon (vs
// 5B0068AF into a 256x256 target) record their render targets for this frame, which the draw-time correction matches against a billboard's t4. Every reference taken is released.
void srvTurretRadarWriterDraw(ID3D11DeviceContext* ctx, uint64_t vs, uint32_t count, uint32_t instances) {
    const bool disc = vs == kSchematicTerrainVs, icon = vs == kRadarIconVs;
    if (!ctx || (!disc && !icon)) return;
    writersFrame();
    ID3D11RenderTargetView* rtv[3] = {};
    if (!guarded("srv.turret.writers.om", [&] { ctx->OMGetRenderTargets(3, rtv, nullptr); })) return;
    bool counted = false;
    for (uint32_t k = 0; k < 3; ++k) {
        if (!rtv[k]) continue;
        ID3D11Resource* res = nullptr;
        guarded("srv.turret.writers.res", [&] { rtv[k]->GetResource(&res); });
        if (res && disc) {
            addWriter(g_writers.disc, g_writers.nDisc, res);
            counted = true;
        } else if (res && icon && k == 0) {
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            D3D11_TEXTURE2D_DESC td{};
            guarded("srv.turret.writers.desc", [&] {
                res->GetType(&dim);
                if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
            });
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && td.Width == 256 && td.Height == 256) {
                if (addWriter(g_writers.icon, g_writers.nIcon, res) && g_writers.nIcon == 3) {   // two is normal (one an eye); a third is counted, and the first ever is said
                    g_cwExtraFrames.fetch_add(1, std::memory_order_relaxed);
                    if (!g_extraWriter.said && !g_extraWriter.pending) {
                        g_extraWriter.pending = true;
                        g_extraWriter.matched = false;
                        g_extraWriter.target = res;
                        g_extraWriter.frame = g_frame;
                        g_extraWriter.count = count;
                        g_extraWriter.instances = instances;
                        g_extraWriter.w = td.Width;
                        g_extraWriter.h = td.Height;
                        UINT nvp = 1;
                        g_extraWriter.haveVp = guarded("srv.turret.writers.vp", [&] { ctx->RSGetViewports(&nvp, &g_extraWriter.vp); }) && nvp >= 1;
                    }
                }
                counted = true;
            }
        }
        st::release(res);
        st::release(rtv[k]);
    }
    if (counted) (disc ? g_cwDisc : g_cwIcon).fetch_add(1, std::memory_order_relaxed);
}

// The radar picture's camera (the instrument above): every owner-context draw while the Scorpion's turret view is on; acts on the frame's first draw of vs 4EDE4BC9.
void srvTurretRadarCamDraw(ID3D11DeviceContext* ctx, uint64_t vs) {
    if (vs != kSchematicTerrainVs || !ctx || !g_radarCam.on || g_radarCam.lastCopyFrame == g_frame) return;
    g_radarCam.lastCopyFrame = g_frame;
    if (!radarCamEnsure(ctx)) return;
    const uint32_t k = g_radarCam.next;
    if (g_radarCam.pending[k]) return;   // the ring is full: this frame is skipped
    UINT first = 0, count = 0;
    if (holoCopyVsRows(ctx, g_radarCam.stage[k], 0, 4, kRadarCamRows, 0, first, count) != kRadarCamRows) return;
    g_radarCam.pending[k] = true;
    g_radarCam.waited[k] = 0;
    g_radarCam.frameOf[k] = g_frame;
    g_radarCam.next = (k + 1) % kRadarCamRing;
    ++g_radarCam.draws;
}

// The hologram's source pass (the instrument above): every owner-context draw of a watched frame. A draw with a watched texture bound as a render target or the depth target is a writer.
void srvTurretHoloWatchDraw(ID3D11DeviceContext* ctx, uint64_t vs, uint64_t ps, char kind, uint32_t count, uint32_t instances) {
    if (g_holo.phase != HoloWatch::Phase::kWatch || !ctx) return;
    ++g_holo.draws;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    if (!guarded("srv.turret.holo.targets", [&] { ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv); })) return;
    char targets[600];
    int len = 0;
    targets[0] = 0;
    bool writes = false;
    int firstTex = -1;   // the first watched texture among the draw's targets
    auto describe = [&](ID3D11View* view, const char* name) {
        if (!view) return;
        ID3D11Resource* res = nullptr;
        guarded("srv.turret.holo.viewres", [&] { view->GetResource(&res); });
        HoloTex t;
        const bool tex2d = holoDescribe(res, t);
        const int index = res ? holoTexIndex(res) : -1;
        if (index >= 0) {
            writes = true;
            if (firstTex < 0) firstTex = index;
        }
        if (len >= 0 && len < static_cast<int>(sizeof(targets)) - 80) {
            if (tex2d)
                len += std::snprintf(targets + len, sizeof(targets) - len, " %s=%06X %ux%u fmt %u%s", name, holoShortId(res), t.w, t.h, t.fmt, index >= 0 ? " WATCHED" : "");
            else
                len += std::snprintf(targets + len, sizeof(targets) - len, " %s=%06X", name, holoShortId(res));
        }
        st::release(res);
    };
    static const char* const kRtvName[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {"rtv0", "rtv1", "rtv2", "rtv3", "rtv4", "rtv5", "rtv6", "rtv7"};
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) describe(rtvs[i], kRtvName[i]);
    describe(dsv, "dsv");
    for (auto*& v : rtvs) st::release(v);
    st::release(dsv);
    if (!writes) return;
    ++g_holo.writers;
    {
        uint32_t k = 0;
        while (k < g_holo.pairN && !(g_holo.pairs[k].vs == vs && g_holo.pairs[k].ps == ps)) ++k;
        if (k < g_holo.pairN) {
            ++g_holo.pairs[k].n;
        } else if (g_holo.pairN < kHoloPairsMax) {
            g_holo.pairs[g_holo.pairN++] = HoloWatch::Pair{vs, ps, firstTex, 1};
        } else {
            ++g_holo.pairsDropped;
        }
    }
    if (g_holo.writers > kHoloWriterLinesMax) return;   // counted in the tally above, not said again
    D3D11_VIEWPORT vp{};
    UINT vpCount = 1;
    guarded("srv.turret.holo.viewport", [&] { ctx->RSGetViewports(&vpCount, &vp); });
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    guarded("srv.turret.holo.topology", [&] { ctx->IAGetPrimitiveTopology(&topology); });
    const uint32_t index = g_holo.entryN;
    const bool keep = g_holo.entryN < kHoloEntries && g_holo.writers <= kHoloWritersMax;
    Log::get().note("srv turret holo writer: round %u frame %u%s vs=%016llX ps=%016llX draw %c count %u instances %u topology %d viewport (%.0f,%.0f %.0fx%.0f z %.2f..%.2f) targets:%s", g_holo.round,
                    g_frame, keep ? "" : " (constants not kept)", static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps), kind ? kind : '?', count, instances,
                    static_cast<int>(topology), static_cast<double>(vp.TopLeftX), static_cast<double>(vp.TopLeftY), static_cast<double>(vp.Width), static_cast<double>(vp.Height),
                    static_cast<double>(vp.MinDepth), static_cast<double>(vp.MaxDepth), targets);
    if (!keep) return;
    HoloEntry& e = g_holo.entry[index];
    e = HoloEntry{};
    e.writer = true;
    e.vs = vs;
    e.ps = ps;
    e.kind = kind;
    e.count = count;
    e.instances = instances;
    holoCopyConstants(ctx, e, index);
    ++g_holo.entryN;
    Log::get().note("srv turret holo writer: round %u: the writer above is constants entry #%u", g_holo.round, index);
}

// A draw of a family that reached the GPU without the pass's shader bound, by the route it took (srv_turret.h). The first of each route is said once.
void srvTurretHudNoteUnmoved(int family, st::UnmovedRoute route, const void* site) {
    const uint32_t f = family >= 0 && family < static_cast<int>(kFamilyN) ? static_cast<uint32_t>(family) : kFamilyN;
    const uint32_t r = static_cast<uint32_t>(route);
    if (r >= st::kUnmovedRoutes) return;
    g_unmoved[f][r].fetch_add(1, std::memory_order_relaxed);
    bool first = false;
    if (route == st::UnmovedRoute::kReissue && site) {   // one line for each distinct call site
        const uintptr_t s = reinterpret_cast<uintptr_t>(site);
        for (uint32_t i = 0; i < kReissueSitesMax; ++i) {
            uintptr_t seen = g_reissueSites[i].load(std::memory_order_relaxed);
            if (seen == s) break;
            if (seen == 0 && g_reissueSites[i].compare_exchange_strong(seen, s)) {
                first = true;
                break;
            }
            if (seen == s) break;
        }
    } else {
        const uint32_t bit = 1u << r;
        first = (g_unmovedNoted.fetch_or(bit, std::memory_order_relaxed) & bit) == 0;
    }
    if (!first) return;
    char where[160];
    siteText(site, where);
    Log::get().note("srv turret hud: first unmoved family draw by route %s (family %s): %s%s", st::unmovedRouteName(route), f < kFamilyN ? st::kFamilies[f].name : "?", st::unmovedRouteWhere(route), where);
}

void srvTurretHudNoteRefusedDraw(int family) {
    if (family < 0 || family >= static_cast<int>(kFamilyN)) return;
    st::TurretParams unused;
    const bool groupOn = paramsOfGroup(st::kFamilies[family].group, st::EyeFrame{}, unused) && !anchorLeaves(family);   // a billboard left to the game's anchor is group-off
    srvTurretHudNoteUnmoved(family, groupOn ? st::UnmovedRoute::kRefused : st::UnmovedRoute::kGroupOff, nullptr);
}

bool srvTurretHudScopeActive() { return g_binding.needsRestore(); }

// vscreen.cpp, each issue of a matched draw while a scope is counting (srv_turret.h srvTurretHudCountIssue): counted for the sampled scope's statistics line, and said for a family's first scope.
void srvTurretHudNoteIssue(ID3D11DeviceContext* ctx) {
    detail::g_srvTurretHudIssues.fetch_add(1, std::memory_order_relaxed);
    if (g_radarScope.kind && ctx == g_radarScope.ctx) {   // THE PAIR CAPTURE's count: a moved radar billboard's issue, with the geometry shader it went out with
        ++g_radarScope.issues;
        ID3D11GeometryShader* gs = nullptr;
        guarded("srv.turret.radar.issue", [&] { ctx->GSGetShader(&gs, nullptr, nullptr); });
        if (gs && static_cast<const void*>(gs) == g_radarScope.gs)
            ++g_radarScope.ours;
        else
            ++g_radarScope.other;
        st::release(gs);
    }
    if (g_noteFamily >= 0 && ctx == g_noteCtx && g_noteIssue < kNoteIssues) {
        ++g_noteIssue;
        noteState(ctx);
    }
}

void srvTurretFrameTick(uint32_t frameNo) {
    if (!runtimeVrProfile()) {   // flat (or not yet decided): every state stays at its zero, alignTick adds an exact 0
        anchorTick(false, frameNo, 0);   // a radar anchor armed in VR gets a zero offset (a live switch to flat puts the game's own place back)
        return;
    }
    readKeys();
    const int backCm = g_backCm.load(std::memory_order_relaxed), forwardCm = g_forwardCm.load(std::memory_order_relaxed), radarForwardCm = g_radarForwardCm.load(std::memory_order_relaxed),
              panelsCm = g_panelsCm.load(std::memory_order_relaxed), radarCm = g_radarCm.load(std::memory_order_relaxed), gapCm = g_gapCm.load(std::memory_order_relaxed);
    // The Scorpion only: the turret view bit is the Scarab's too, and the journal says which SRV this is (journalSrvScorpion; unknown is "not the Scorpion"). `on` is what both parts act on.
    const bool turret = journalSrvTurretView();
    const bool on = turret && journalSrvScorpion();
    {   // THE LOCK (TEMPORARY): its targets start again when the view comes on or a key that moves where the game's radar rests changes
        static bool s_on = false;
        static int s_back = backCm, s_radarForward = radarForwardCm, s_anchor = g_anchorMode.load(std::memory_order_relaxed);
        const int anchorMode = g_anchorMode.load(std::memory_order_relaxed);
        const st::LockReset why = st::lockResetWhy(s_on, on, s_back, backCm, s_radarForward, radarForwardCm, s_anchor, anchorMode);
        if (why != st::LockReset::kNone) {
            g_lockResetWhy.store(static_cast<int>(why), std::memory_order_relaxed);
            g_lockResetGen.fetch_add(1, std::memory_order_release);
        }
        s_on = on;
        s_back = backCm;
        s_radarForward = radarForwardCm;
        s_anchor = anchorMode;
    }
    if (on != g_on) {
        g_on = on;
        Log::get().note("srv turret view: %s at frame %u (Status.json Flags 0x%08X, seat kind 0x%X)", on ? "on" : "off", frameNo, static_cast<unsigned>(journalStatusFlags()),
                        static_cast<unsigned>(journalSeatKind()));
    }
    if (turret && !on && !g_ignored) {
        g_ignored = true;
        Log::get().note("srv turret view: not the Scorpion at frame %u (Status.json Flags 0x%08X, seat kind 0x%X): the turret view is on in an SRV the journal does not call the Scorpion, so the "
                        "camera and the HUD stay the game's own", frameNo, static_cast<unsigned>(journalStatusFlags()), static_cast<unsigned>(journalSeatKind()));
    } else if (!turret) {
        g_ignored = false;
    }
    g_backSeatedZ = st::backSeatedZ(on, backCm);
    detail::g_srvTurretRadarCamOn.store(on, std::memory_order_relaxed);   // the radar picture's camera instrument (TEMPORARY)
    anchorTick(on, frameNo, radarForwardCm);   // the radar's schematic moved in the game (TEMPORARY key fix.srv_turret_radar_anchor): zero unless `on`
    const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0);   // any of the five HUD keys; a group whose own keys are 0 is left alone per draw
    if (wanted) {
        cacheGeometry(on, backCm, forwardCm, radarForwardCm);
    } else {
        g_anchor.reset();   // the next stay (or the next key) takes the anchor afresh
        g_anchorOk.store(false, std::memory_order_release);
    }
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
        anchorWindow(g_windowSeen);
        g_windowSeen = on;
        g_windowStartMs = now;
        for (auto& r : g_refused) r.store(0, std::memory_order_relaxed);
        for (auto& d : g_draws) d.store(0, std::memory_order_relaxed);
        for (auto& d : g_familyDraws) d.store(0, std::memory_order_relaxed);
        g_bound.store(0, std::memory_order_relaxed);
        for (auto& row : g_unmoved)
            for (auto& n : row) n.store(0, std::memory_order_relaxed);
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

// CAMERA MODE (srv_turret_math.h THE RADAR'S OWN CAMERA): the radar's schematic camera is moved back by D in the game, so the radar's own picture billboards (family 7 on a texture this
// frame's radar writers drew: billboardKind) go through the regular radar path, +D along the seat with the rest of the radar, with no delta, lock or content depth (tr.w 0); every
// other 5559BD94 draw (family 8, the panel holograms) is the game's own, as in on mode. True: the draw is left to the game.
bool cameraLeaves(ID3D11DeviceContext* ctx, int family, int eye) {
    if (!st::radarAnchorLeavesFamily(family) || !g_cameraBillboards.load(std::memory_order_relaxed)) return false;
    bool lastFrameOnly = false;
    if (family == 7 && billboardKind(ctx, lastFrameOnly) != 0) return false;
    // left to the game; the hologram's source instrument keeps its rounds (the regular path collects the radar's own)
    if ((eye == 0 || eye == 1) && (family == kHoloFamilyA || family == kHoloFamilyB)) holoCollect(ctx, family, eye, g_probe[eye][family].ordinal++);
    return true;
}

bool srvTurretHudBegin(ID3D11DeviceContext* ctx, int family, int eye) {
    if (!ctx || family < 0 || family >= static_cast<int>(kFamilyN)) return false;
    if (eye == 0) anchorDirSample(ctx, family);   // the radar anchor's direction: the live camera's axes, read only (one copy a frame while the anchor is wanted)
    // A family whose group has none of its push, the gap and the HUD-forward key above 0 is not the pass's (one group on and the other off is a setting, and the keys can go to 0 between the
    // frame tick and this draw): the draw is the game's own and is not counted among the matched ones, so the 10 s line's bound share is judged on the draws the pass was asked to carry.
    st::TurretParams unused;
    const st::Group group = st::kFamilies[family].group;
    if (!paramsOfGroup(group, st::EyeFrame{}, unused)) return false;
    // The radar's schematic is moved in the game: the 5559BD94 billboards are the game's own (a decline counts as group-off), but for the radar's own picture billboards, which the
    // draw-time correction moves by what the turret did since the setter's sample.
    if (anchorLeaves(family)) return radarCorrectionBegin(ctx, family, eye);
    if (cameraLeaves(ctx, family, eye)) return false;   // camera mode: a billboard that is not the radar's own picture is the game's own
    if (eye != 0 && eye != 1) {
        refuse(st::Refuse::kNoEye);
        return false;
    }
    g_draws[eye].fetch_add(1, std::memory_order_relaxed);
    g_familyDraws[family].fetch_add(1, std::memory_order_relaxed);
    const uint32_t ordinal = g_probe[eye][family].ordinal++;   // this draw's place among the frame's matched draws of the eye and family (the instrument's rotation)
    if (family == kHoloFamilyA || family == kHoloFamilyB) holoCollect(ctx, family, eye, ordinal);   // the hologram's source instrument: what the billboard reads (the game's own bindings)
    st::EyeFrame frame;
    if (!drawEyeFrame(eye, frame)) {
        refuse(st::Refuse::kNoGeometry);
        return false;
    }
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    if (!guarded("srv.turret.topology", [&] { ctx->IAGetPrimitiveTopology(&topology); }) || !st::topologyCarried(static_cast<int>(topology))) {   // triangle lists and strips
        refuse(st::Refuse::kTopology);
        return false;
    }
    const uint32_t sig = st::kFamilies[family].sig;
    if (g_binding.needsRestore() || g_scissor.owed() || !ensureGs(ctx, sig)) {
        refuse(st::Refuse::kNotReady);
        return false;
    }
    st::TurretParams params;
    if (!paramsOfGroup(group, frame, params)) return false;   // the keys went to 0 just now: the game's own
    ID3D11Buffer* constants = g_constants[eye].get(ctx, params);
    if (!constants) {
        refuse(st::Refuse::kNotReady);
        return false;
    }
    // Signature H (the Scorpion's hologram) also needs the draw's own vertex constants, for the lighting position (srv_turret_hud_shader.h): without them the draw is the game's own.
    st::ExtraCb scene;
    const bool needScene = sig == st::kSceneSignature;
    if (needScene && !st::readGameCb(ctx, st::kGameCbSlot, scene)) {
        refuse(st::Refuse::kNotReady);
        return false;
    }
    // THE GPU INSTRUMENT: once a second per eye and family this draw goes through the stream-output twin (when there is one) with the queries around its scope.
    const bool sample = probeDue(ctx, eye, family, ordinal);
    const Probe& probe = g_probe[eye][family];
    const bool twin = sample && g_soGs[sig] && probe.soBuf && probe.soStats && probe.staging;
    const auto begun = g_binding.begin(ctx, twin ? g_soGs[sig] : g_gs[sig], constants, st::directSetGs, needScene ? &scene : nullptr);
    if (begun == st::Binding::Begin::kGameHasGs) {
        refuse(st::Refuse::kGameGs);
        return false;
    }
    if (begun != st::Binding::Begin::kBound) {
        refuse(st::Refuse::kNotReady);
        return false;
    }
    if (sample) probeBegin(ctx, eye, family, ordinal, twin, params);
    // The game's scissor rectangle for this draw is from the HUD's OLD place: with the key at open the rectangles are the whole target while the draw is issued (End puts the game's back); with the key at
    // game a family's first draw only looks, to say whether it is scissored at all.
    {
        const bool apply = g_scissorOpen.load(std::memory_order_relaxed);
        if (apply || !g_scissorNoted[family]) {
            st::Scissor::Seen seen;
            const auto opened = g_scissor.open(ctx, seen, apply);
            if (opened == st::Scissor::Open::kOpened) g_scissorOpened.fetch_add(1, std::memory_order_relaxed);
            if (seen.enabled && !g_scissorNoted[family]) {
                g_scissorNoted[family] = true;
                Log::get().note("srv turret hud: family %s draws with the scissor test on: %u rect(s), first [%ld,%ld,%ld,%ld], target %ldx%ld; the scissor is %s for its draws",
                                st::kFamilies[family].name, seen.rects, seen.first.left, seen.first.top, seen.first.right, seen.first.bottom, seen.targetW, seen.targetH,
                                apply ? "opened to the whole target" : "kept as the game set it");
            }
        }
    }
    if (!g_stateNoted[family]) noteBegin(ctx, family, eye);   // this scope's issues say their state
    g_bound.fetch_add(1, std::memory_order_relaxed);
    if (!g_firstNoted[family]) {
        g_firstNoted[family] = true;
        // A negative gap is the push-back (deeper), a positive one the pull (nearer), a gap of 0 the translation alone; the window is the one the group's constants carry, in metres from
        // the anchor, and the whole HUD has moved forward by the HUD-forward key first.
        char radial[96];
        if (params.gap < 0.0f)
            std::snprintf(radial, sizeof(radial), "moves %.2f m deeper", static_cast<double>(-params.gap));
        else if (params.gap > 0.0f)
            std::snprintf(radial, sizeof(radial), "moves %.2f m nearer", static_cast<double>(params.gap));
        else
            std::snprintf(radial, sizeof(radial), "does not move radially");
        Log::get().note("srv turret hud: first draw of family %s through the shader (frame %u, eye %d), the %s group: a vertex %.2f to %.2f m from the anchor %s, in 3D about the anchor "
                        "(%.3f,%.3f,%.3f m seated), after the group's HUD moves %.2f m forward; the game's pixel shader, blend and every other state are unchanged, and its geometry stage "
                        "and constant slot go back after the issue.",
                        st::kFamilies[family].name, g_frame, eye, st::groupName(group), static_cast<double>(params.dMin), static_cast<double>(params.dMax), radial,
                        static_cast<double>(g_anchorLog[0].load(std::memory_order_relaxed)), static_cast<double>(g_anchorLog[1].load(std::memory_order_relaxed)),
                        static_cast<double>(g_anchorLog[2].load(std::memory_order_relaxed)),
                        static_cast<double>(st::forwardCmOf(group, g_forwardCm.load(std::memory_order_relaxed), g_radarForwardCm.load(std::memory_order_relaxed))) / 100.0);
    }
    return true;
}

void srvTurretHudEnd(ID3D11DeviceContext* ctx) {
    noteEnd();
    probeEnd(ctx);   // the queries end and the stream-output target comes off, right before the game's geometry stage goes back
    pairEnd(ctx);    // ... and THE PAIR CAPTURE's (TEMPORARY)
    radarScopeEnd();
    const auto done = g_binding.finish(ctx);
    probeFinish(ctx);
    pairFinish(ctx);
    if (!g_scissor.restore(ctx) && !g_scissorRestoreNoted) {
        g_scissorRestoreNoted = true;
        Log::get().note("srv turret hud: the game's scissor rectangles could not be put back after an issue; the frame boundary settles them and draws are refused (not-ready) until then.");
    }
    if (!done.restored && !g_restoreNoted) {
        g_restoreNoted = true;
        Log::get().note("srv turret hud: the game's geometry stage could not be put back after an issue; the frame boundary settles it and draws are refused (not-ready) until then.");
    }
}

void srvTurretHudFrameBoundary(ID3D11DeviceContext* ctx) {
    ++g_frame;
    for (int eye = 0; eye < 2; ++eye) {
        for (int family = 0; family < static_cast<int>(kFamilyN); ++family) {
            Probe& p = g_probe[eye][family];
            probeRead(ctx, eye, family);
            if (family == static_cast<int>(kDiscFamily)) probeReadDisc(ctx, eye);
            // A frame with fewer matched draws than the wanted ordinal would never be sampled: start again from the first.
            if (p.ordinal <= p.wantOrd && p.ordinal > 0) p.wantOrd = 0;
            p.ordinal = 0;
        }
    }
    holoBoundary(ctx);
    radarCamBoundary(ctx);
    anchorDirBoundary(ctx);
    pairBoundary(ctx);   // THE PAIR CAPTURE (TEMPORARY)
    if (g_scissor.owed() && ctx) (void)g_scissor.settle(ctx);
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
    noteEnd();
    detail::g_srvTurretHoloWatching.store(false, std::memory_order_relaxed);
    st::release(g_holo.stage);
    g_holo = HoloWatch{};
    detail::g_srvTurretRadarCamOn.store(false, std::memory_order_relaxed);
    for (auto& s : g_radarCam.stage) st::release(s);
    g_radarCam = RadarCamWatch{};
    srvRadarAnchorShutdown();   // the hook's gate shuts: the relay forwards straight to the game's function
    g_anchorYield.store(false, std::memory_order_relaxed);
    g_cameraBillboards.store(false, std::memory_order_relaxed);
    detail::g_srvTurretRadarWritersOn.store(false, std::memory_order_relaxed);
    g_writers = Writers{};
    g_writersPrev = Writers{};
    g_corr = FrameCorrection{};
    g_anchorDirWanted.store(false, std::memory_order_relaxed);
    detail::g_srvTurretCameraRowsWanted.store(false, std::memory_order_relaxed);
    for (auto& w : g_camWrites) w = CamWrite{};
    for (auto& s : g_dir.stage) st::release(s);
    g_dir = DirWatch{};
    for (auto& eyeProbes : g_probe)
        for (Probe& p : eyeProbes) probeReset(p);
    pairReleaseObjects();   // THE PAIR CAPTURE (TEMPORARY)
    pairResetSlots();
    g_pair.device = nullptr;
    g_radarScope = RadarScope{};
    lockReset();   // THE LOCK (TEMPORARY)
    g_rest = st::RestWatch{};
    g_aimPrev = AimPrev{};
    g_binding.clear();
    g_scissor.clear();
    for (auto& c : g_constants) c.reset();
    releaseGs();
    g_gsTried = false;
}

}  // namespace edvr
