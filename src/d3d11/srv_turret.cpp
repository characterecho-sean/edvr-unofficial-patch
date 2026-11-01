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

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace edvr {

namespace detail {
std::atomic<bool> g_srvTurretHudWanted{false};
std::atomic<bool> g_srvTurretHudCounting{false};
std::atomic<uint32_t> g_srvTurretHudIssues{0};
std::atomic<bool> g_srvTurretHoloWatching{false};
std::atomic<bool> g_srvTurretRadarCamOn{false};
std::atomic<bool> g_srvTurretCameraRowsWanted{false};
std::atomic<bool> g_srvTurretRadarWritersOn{false};
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
                        "open or game, the radar anchor key on, reversed or off)",
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
};
Probe g_probe[2][kFamilyN];           // by eye and family
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

// srvTurretHudBegin, at each draw of a hologram billboard family while a round collects: what it reads at t0..t7; the render and depth targets among t2..t7 are watched in the next frame.
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

void anchorTick(bool on, uint32_t frameNo, int radarForwardCm) {
    AnchorWatch& w = g_anchorWatch;
    const auto mode = static_cast<st::RadarAnchorMode>(g_anchorMode.load(std::memory_order_relaxed));
    if (on && mode != st::RadarAnchorMode::kOff && !w.armTried) {
        w.armTried = true;
        (void)srvRadarAnchorArm(nullptr, 0);   // says its own lines: "setter hook armed: ..." or "setter hook stood down: <why>; nothing was patched", then the update counter's
    }
    w.offset = st::radarAnchorOffset(on, mode, radarForwardCm);
    srvRadarAnchorPublish(w.offset.bits, frameNo);
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
    g_anchorYield.store(on && mode != st::RadarAnchorMode::kOff && a.state == RadarAnchorHookState::Armed && a.holdsOurs, std::memory_order_relaxed);
    const bool yieldNow = g_anchorYield.load(std::memory_order_relaxed);
    detail::g_srvTurretRadarWritersOn.store(yieldNow, std::memory_order_relaxed);   // the draw-time correction's writer watch
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
std::atomic<uint64_t> g_cwDisc{0}, g_cwIcon{0}, g_cwAmbiguous{0};   // writer draws seen, and frames with two or more 256x256 icon writers
enum CorrMiss : int { kCorrOk = 0, kCorrNoMatch, kCorrNoRows, kCorrNoCall, kCorrNoAnchor, kCorrOver, kCorrOther, kCorrMissCount };
struct CorrCounters {
    std::atomic<uint64_t> moved[2]{}, disc{0}, icon{0}, frames{0}, deltaSumUm{0}, deltaMaxUm{0}, yawMaxMilli{0}, pitchMaxMilli{0}, family8{0}, lastFrameOnly{0};
    std::atomic<int64_t> yawSumMicro{0}, pitchSumMicro{0};
    std::atomic<uint64_t> left[kCorrMissCount]{};
};
CorrCounters g_cc;
struct FrameCorrection {
    uint32_t frame = 0xFFFFFFFFu;
    int reason = kCorrOther;
    float deltaW[3] = {};
    double yawDeg = 0.0, pitchDeg = 0.0, gapMs = 0.0;
    bool counted = false;
};
FrameCorrection g_corr;
bool g_corrFirstSaid = false;
int64_t g_qpcFreq = 0;

void writersFrame() {
    if (g_writers.frame == g_frame) return;
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
    lastFrameOnly = res && !kind && kindOf(g_writersPrev, res) != 0;
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
    if (!(norm <= st::kCorrectionMaxM)) return kCorrOver;
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
    return kCorrOk;
}

// srvTurretHudBegin, at a draw of family 7 or 8 while the anchor yield is on (anchorLeaves): true when the draw is moved and bound (srvTurretHudEnd puts the game's stage back).
bool radarCorrectionBegin(ID3D11DeviceContext* ctx, int family, int eye) {
    if (eye != 0 && eye != 1) return false;
    bool lastFrameOnly = false;
    const int kind = billboardKind(ctx, lastFrameOnly);
    if (family != 7) {   // family 8 is never moved; one on a radar writer's target would contradict the model
        if (kind) g_cc.family8.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto leave = [](int why) {
        g_cc.left[why].fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    if (lastFrameOnly) g_cc.lastFrameOnly.fetch_add(1, std::memory_order_relaxed);
    if (!kind) return leave(kCorrNoMatch);
    if (g_corr.frame != g_frame) {
        g_corr = FrameCorrection{};
        g_corr.frame = g_frame;
        g_corr.reason = correctionCompute(ctx, eye, g_corr);
    }
    if (g_corr.reason != kCorrOk) return leave(g_corr.reason);
    float rows[12];
    if (anchorDirCpuRows(ctx, nativeTemporalFrameSequence(), rows, nullptr, nullptr) != 0) return leave(kCorrNoRows);   // this draw's own camera rows, this host frame
    float dEye[3];
    st::radarDeltaInEye(rows, g_corr.deltaW, dEye);
    if (dEye[0] == 0.0f && dEye[1] == 0.0f && dEye[2] == 0.0f) return leave(kCorrOther);
    st::EyeFrame ef;
    if (!drawEyeFrame(eye, ef)) return leave(kCorrOther);
    for (int i = 0; i < 3; ++i) ef.tRadar[i] = dEye[i];
    // the translation alone (gap 0: every vertex moves by delta), and the game's own NDC depth (the hologram's scene-depth discard reads it), whatever fix.srv_turret_hud_depth says
    const st::TurretParams cparams = st::withDepth(st::translateParamsFor(ef, st::Group::kRadar), st::DepthMode::kGame);
    if (!st::paramsUsable(cparams)) return leave(kCorrOther);
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    if (!guarded("srv.turret.corr.topology", [&] { ctx->IAGetPrimitiveTopology(&topo); }) || !st::topologyCarried(static_cast<int>(topo))) return leave(kCorrOther);
    if (g_binding.needsRestore() || g_scissor.owed() || !ensureGs(ctx, st::kSceneSignature)) return leave(kCorrOther);
    ID3D11Buffer* const cbuf = g_constants[eye].get(ctx, cparams);
    if (!cbuf) return leave(kCorrOther);
    st::ExtraCb camera;
    if (!st::readGameCb(ctx, st::kGameCbSlot, camera)) return leave(kCorrOther);   // signature H's lighting position needs the draw's own camera rows at GS slot 1
    if (g_binding.begin(ctx, g_gs[st::kSceneSignature], cbuf, st::directSetGs, &camera) != st::Binding::Begin::kBound) return leave(kCorrOther);
    if (g_scissorOpen.load(std::memory_order_relaxed)) {   // the game's scissor rectangle is from the unmoved place, as for the other moved draws
        st::Scissor::Seen seen;
        if (g_scissor.open(ctx, seen, true) == st::Scissor::Open::kOpened) g_scissorOpened.fetch_add(1, std::memory_order_relaxed);
    }
    g_cc.moved[eye].fetch_add(1, std::memory_order_relaxed);
    (kind == 1 ? g_cc.disc : g_cc.icon).fetch_add(1, std::memory_order_relaxed);
    const double norm = std::sqrt(double(g_corr.deltaW[0]) * g_corr.deltaW[0] + double(g_corr.deltaW[1]) * g_corr.deltaW[1] + double(g_corr.deltaW[2]) * g_corr.deltaW[2]);
    if (!g_corr.counted) {   // the per-frame figures, once a frame
        g_corr.counted = true;
        g_cc.frames.fetch_add(1, std::memory_order_relaxed);
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
        Log::get().note("srv turret radar anchor: correction first applied at frame %u, eye %d, %s: delta (%.2f,%.2f,%.2f) cm in the world, (%.2f,%.2f,%.2f) cm in the eye; the shown call was "
                        "%.2f ms before the camera write",
                        g_frame, eye, kind == 1 ? "disc" : "icon", static_cast<double>(g_corr.deltaW[0]) * 100.0, static_cast<double>(g_corr.deltaW[1]) * 100.0,
                        static_cast<double>(g_corr.deltaW[2]) * 100.0, static_cast<double>(dEye[0]) * 100.0, static_cast<double>(dEye[1]) * 100.0, static_cast<double>(dEye[2]) * 100.0,
                        g_corr.gapMs);
    }
    return true;
}

// The frame tick, at each 10 s boundary: the correction's line whenever the anchor yield was on in the window, even with nothing moved (the reasons say why); the baselines move on
// either way. No line at all means this code never ran.
void correctionWindow(bool yieldSeen) {
    struct Base {
        uint64_t moved0 = 0, moved1 = 0, disc = 0, icon = 0, frames = 0, deltaSumUm = 0, family8 = 0, lastFrameOnly = 0, cwDisc = 0, cwIcon = 0, cwAmbiguous = 0, left[kCorrMissCount] = {};
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
    n.cwDisc = g_cwDisc.load(std::memory_order_relaxed);
    n.cwIcon = g_cwIcon.load(std::memory_order_relaxed);
    n.cwAmbiguous = g_cwAmbiguous.load(std::memory_order_relaxed);
    for (int i = 0; i < kCorrMissCount; ++i) n.left[i] = g_cc.left[i].load(std::memory_order_relaxed);
    n.yawSumMicro = g_cc.yawSumMicro.load(std::memory_order_relaxed);
    n.pitchSumMicro = g_cc.pitchSumMicro.load(std::memory_order_relaxed);
    const uint64_t deltaMaxUm = g_cc.deltaMaxUm.exchange(0, std::memory_order_relaxed), yawMax = g_cc.yawMaxMilli.exchange(0, std::memory_order_relaxed),
                   pitchMax = g_cc.pitchMaxMilli.exchange(0, std::memory_order_relaxed);
    if (yieldSeen) {
        const uint64_t frames = n.frames - b.frames;
        const double perFrame = frames ? 1.0 / double(frames) : 0.0;
        Log::get().note("srv turret radar anchor: 10 s correction: %llu billboard draws moved (eye 0 %llu, eye 1 %llu; disc %llu, icon %llu) over %llu frames, |delta| mean %.2f max %.2f cm, "
                        "yaw mean %.3f max |%.3f| deg, pitch mean %.3f max |%.3f| deg; left to the game: no writer match %llu (of them on last frame's radar target %llu), no camera rows %llu, no shown call %llu, no anchor %llu, "
                        "over 10 cm %llu, other %llu; family 8 on a radar target %llu; writers: 4EDE4BC9 %llu, 256x256 5B0068AF %llu (frames with two or more: %llu)",
                        static_cast<unsigned long long>((n.moved0 - b.moved0) + (n.moved1 - b.moved1)), static_cast<unsigned long long>(n.moved0 - b.moved0),
                        static_cast<unsigned long long>(n.moved1 - b.moved1), static_cast<unsigned long long>(n.disc - b.disc), static_cast<unsigned long long>(n.icon - b.icon),
                        static_cast<unsigned long long>(frames), double(n.deltaSumUm - b.deltaSumUm) * 1.0e-4 * perFrame, double(deltaMaxUm) * 1.0e-4,
                        double(n.yawSumMicro - b.yawSumMicro) * 1.0e-6 * perFrame, double(yawMax) / 1000.0, double(n.pitchSumMicro - b.pitchSumMicro) * 1.0e-6 * perFrame,
                        double(pitchMax) / 1000.0, static_cast<unsigned long long>(n.left[kCorrNoMatch] - b.left[kCorrNoMatch]),
                        static_cast<unsigned long long>(n.lastFrameOnly - b.lastFrameOnly), static_cast<unsigned long long>(n.left[kCorrNoRows] - b.left[kCorrNoRows]), static_cast<unsigned long long>(n.left[kCorrNoCall] - b.left[kCorrNoCall]),
                        static_cast<unsigned long long>(n.left[kCorrNoAnchor] - b.left[kCorrNoAnchor]), static_cast<unsigned long long>(n.left[kCorrOver] - b.left[kCorrOver]),
                        static_cast<unsigned long long>(n.left[kCorrOther] - b.left[kCorrOther]), static_cast<unsigned long long>(n.family8 - b.family8),
                        static_cast<unsigned long long>(n.cwDisc - b.cwDisc), static_cast<unsigned long long>(n.cwIcon - b.cwIcon),
                        static_cast<unsigned long long>(n.cwAmbiguous - b.cwAmbiguous));
    }
    b = n;
}

}  // namespace

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
void srvTurretRadarWriterDraw(ID3D11DeviceContext* ctx, uint64_t vs) {
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
                if (addWriter(g_writers.icon, g_writers.nIcon, res) && g_writers.nIcon == 2) g_cwAmbiguous.fetch_add(1, std::memory_order_relaxed);
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
    const auto done = g_binding.finish(ctx);
    probeFinish(ctx);
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
    g_binding.clear();
    g_scissor.clear();
    for (auto& c : g_constants) c.reset();
    releaseGs();
    g_gsTried = false;
}

}  // namespace edvr
