// The SRV turret view (issue 140): the module. srv_turret.h says what it does and what the log shows; srv_turret_math.h has the maths, the families and the ten experimental
// keys; srv_turret_hud_binding.h holds the constants and the binding, which tools/srv_turret_hud_test runs on WARP; srv_turret_hud_shader.h is the geometry shaders' HLSL,
// compiled at build time; srv_radar_anchor.cpp holds the hooks that move the radar in the game (the placement setter for on and reversed, the schematic slot's update for camera).
#include "srv_turret.h"

#include "srv_radar_anchor.h"
#include "srv_turret_hud_binding.h"
#include "srv_turret_math.h"

#include "draw_gate.h"
#include "journal_watch.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"
#include "vr_camera_census.h"   // nativeTemporalEyeGeometry, nativeTemporalHeadPose
#include "vscreen.h"   // vScreenCopySubresourceRegionRaw: the GPU path's copy of the camera rows goes below the hooks

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
std::atomic<bool> g_srvTurretCameraRowsWanted{false};
std::atomic<bool> g_srvTurretRadarWritersOn{false};
}  // namespace detail

namespace st = srv_turret;

namespace {

static_assert(st::kBackDefaultCm == 80 && st::kBackMaxCm == 150 && st::kForwardDefaultCm == 0 && st::kForwardMaxCm == 150 && st::kRadarForwardDefaultCm == 0 && st::kRadarForwardMaxCm == 150 &&
                  st::kPanelsDefaultCm == 0 && st::kRadarDefaultCm == 0 && st::kSystemsNearDefaultCm == 70 && st::kSystemsNearMaxCm == 200 &&
                  st::kHudUpDefaultCm == 10 && st::kHudUpMaxCm == 100 && st::kGunnerUpDefaultCm == 0 && st::kGunnerUpMaxCm == 100 &&
                  st::kPushMaxCm == 200 && st::kGapDefaultCm == 0 && st::kGapMaxCm == 200,
              "the literals in the integer reads below are these, written as numbers for tools/gen_settings_schema.py");
constexpr uint32_t kFamilyN = st::kFamilyCount, kSigN = st::kSignatureCount;   // the draws' families, and the geometry shaders (one for each distinct output signature)

// ---- the frame thread's state ----------------------------------------------------------------------------------------------------------------------------------
bool g_keysRead = false;
uint32_t g_keysGeneration = 0;
std::atomic<int> g_backCm{0}, g_forwardCm{0}, g_radarForwardCm{0}, g_panelsCm{0}, g_radarCm{0}, g_gapCm{0}, g_systemsNearCm{0}, g_hudUpCm{0}, g_gunnerUpCm{0};   // experimental keys, as last read (the HUD keys are read by the draw path too)
static_assert(st::kDepthDefault == st::DepthMode::kGame, "the literal default of experimental.srv_turret_hud_depth in readKeys is game, which the ini ships");
std::atomic<int> g_depthMode{static_cast<int>(st::kDepthDefault)};   // experimental.srv_turret_hud_depth as a st::DepthMode (0 game, 1 real, 2 front)
std::atomic<bool> g_scissorOpen{false};      // experimental.srv_turret_hud_scissor: game (the default) or open
static_assert(st::kRadarAnchorDefault == st::RadarAnchorMode::kOff, "the literal default of experimental.srv_turret_radar_anchor in readKeys is off, which the ini ships");
std::atomic<int> g_anchorMode{static_cast<int>(st::kRadarAnchorDefault)};   // experimental.srv_turret_radar_anchor as a st::RadarAnchorMode (0 on, 1 reversed, 2 off, 3 camera)
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
    std::atomic<float> v[16] = {};   // l, r, dn, up, the anchor (3), the panels' translation (3), the radar's (3), the gunner panel's (3)
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
    const float values[16] = {f.l, f.r, f.dn, f.up, f.a[0], f.a[1], f.a[2], f.t[0], f.t[1], f.t[2], f.tRadar[0], f.tRadar[1], f.tRadar[2], f.tNear[0], f.tNear[1], f.tNear[2]};
    for (int i = 0; i < 16; ++i) slot.v[i].store(values[i], std::memory_order_relaxed);
    slot.ok.store(ok, std::memory_order_relaxed);
    slot.version.fetch_add(1, std::memory_order_release);
}
// The eye's frame as last published; false when it is not ok or the tick kept writing.
bool readEye(int eye, st::EyeFrame& f) {
    EyeSlot& slot = g_eyeSlot[eye];
    for (int tries = 0; tries < 4; ++tries) {
        const uint32_t before = slot.version.load(std::memory_order_acquire);
        if (before & 1u) continue;
        float values[16];
        for (int i = 0; i < 16; ++i) values[i] = slot.v[i].load(std::memory_order_relaxed);
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
            f.tNear[i] = values[13 + i];
        }
        return ok;
    }
    return false;
}

// ---- the draw path's state (the render thread; the window's counters are read by the frame tick) -------------------------------------------------------------
// The build's bytecode of each signature's geometry shader (srv_turret_math.h kSignatures, in that order).
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
    EDVR_SRV_TURRET_SHADER(I, "5DAD41AA"),
};
#undef EDVR_SRV_TURRET_SHADER
ID3D11GeometryShader* g_gs[kSigN] = {};
ID3D11Device* g_gsDevice = nullptr;   // identity only: the shaders hold their device alive
bool g_gsTried = false;
st::Constants g_constants[2];
st::Binding g_binding;
st::Scissor g_scissor;                         // the game's scissor rectangles while a bound draw is issued (experimental.srv_turret_hud_scissor)
std::atomic<uint32_t> g_scissorOpened{0};      // draws whose scissor was opened in the 10 s window
bool g_scissorNoted[kFamilyN] = {}, g_scissorRestoreNoted = false;
std::atomic<uint32_t> g_draws[2] = {};
std::atomic<uint32_t> g_familyDraws[kFamilyN] = {};
std::atomic<uint32_t> g_bound{0};
std::atomic<uint32_t> g_refused[static_cast<size_t>(st::Refuse::kCount)] = {};
uint32_t g_saidRefuse = 0;
bool g_firstNoted[kFamilyN] = {}, g_restoreNoted = false, g_settleNoted = false, g_gunnerNoted = false;
uint32_t g_frame = 0;

void refuse(st::Refuse r) {
    g_refused[static_cast<size_t>(r)].fetch_add(1, std::memory_order_relaxed);
    const uint32_t bit = 1u << static_cast<unsigned>(r);
    if (g_saidRefuse & bit) return;
    g_saidRefuse |= bit;
    Log::get().note("srv turret hud: refused a draw (%s) -- the draw went on as the game made it. Said once per reason; the 10 s line counts every one.", st::refuseName(r));
}

void releaseGs() {
    for (uint32_t f = 0; f < kSigN; ++f) st::release(g_gs[f]);
    g_gsDevice = nullptr;
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
    return sig < kSigN && g_gs[sig] != nullptr;
}

// The keys, read when the configuration changes (Config::generation()) and not on every frame; said in the log at the first read and at each change.
void readKeys() {
    Config& cfg = Config::get();
    const uint32_t generation = cfg.generation();
    if (g_keysRead && generation == g_keysGeneration) return;
    const int back = cfg.getIntInRange("experimental.srv_turret_camera_back", 80, 0, 150);
    const int forward = cfg.getIntInRange("experimental.srv_turret_hud_forward", 0, 0, 150);
    const int radarForward = cfg.getIntInRange("experimental.srv_turret_radar_forward", 0, 0, 150);
    const int panels = cfg.getIntInRange("experimental.srv_turret_panels_back", 0, 0, 200);
    const int radar = cfg.getIntInRange("experimental.srv_turret_radar_back", 0, 0, 200);
    const int gap = cfg.getIntInRange("experimental.srv_turret_hud_gap", 0, 0, 200);
    const int systemsNear = cfg.getIntInRange("experimental.srv_turret_systems_near", 70, 0, 200);
    const int hudUp = cfg.getIntInRange("experimental.srv_turret_hud_up", 10, 0, 100);
    const int gunnerUp = cfg.getIntInRange("experimental.srv_turret_gunner_up", 0, 0, 100);
    const std::string depthText = cfg.getString("experimental.srv_turret_hud_depth", "game");
    const st::DepthMode depth = st::depthModeOf(depthText.c_str());
    const bool scissorOpen = cfg.getString("experimental.srv_turret_hud_scissor", "game") != "game";
    const std::string anchorText = cfg.getString("experimental.srv_turret_radar_anchor", "off");
    const st::RadarAnchorMode anchorMode = st::radarAnchorModeOf(anchorText.c_str());
    const bool changed = !g_keysRead || back != g_backCm.load(std::memory_order_relaxed) || forward != g_forwardCm.load(std::memory_order_relaxed) ||
                         radarForward != g_radarForwardCm.load(std::memory_order_relaxed) || panels != g_panelsCm.load(std::memory_order_relaxed) || radar != g_radarCm.load(std::memory_order_relaxed) ||
                         gap != g_gapCm.load(std::memory_order_relaxed) || systemsNear != g_systemsNearCm.load(std::memory_order_relaxed) ||
                         hudUp != g_hudUpCm.load(std::memory_order_relaxed) || gunnerUp != g_gunnerUpCm.load(std::memory_order_relaxed) ||
                         static_cast<int>(depth) != g_depthMode.load(std::memory_order_relaxed) ||
                         scissorOpen != g_scissorOpen.load(std::memory_order_relaxed) || static_cast<int>(anchorMode) != g_anchorMode.load(std::memory_order_relaxed);
    g_keysRead = true;
    g_keysGeneration = generation;
    g_backCm.store(back, std::memory_order_relaxed);
    g_forwardCm.store(forward, std::memory_order_relaxed);
    g_radarForwardCm.store(radarForward, std::memory_order_relaxed);
    g_panelsCm.store(panels, std::memory_order_relaxed);
    g_radarCm.store(radar, std::memory_order_relaxed);
    g_gapCm.store(gap, std::memory_order_relaxed);
    g_systemsNearCm.store(systemsNear, std::memory_order_relaxed);
    g_hudUpCm.store(hudUp, std::memory_order_relaxed);
    g_gunnerUpCm.store(gunnerUp, std::memory_order_relaxed);
    g_depthMode.store(static_cast<int>(depth), std::memory_order_relaxed);
    g_scissorOpen.store(scissorOpen, std::memory_order_relaxed);
    g_anchorMode.store(static_cast<int>(anchorMode), std::memory_order_relaxed);
    if (changed)
        Log::get().note("srv turret settings: camera back %d cm, hud forward %d cm, radar forward %d cm, panels back %d cm, radar back %d cm, hud gap %d cm, hud depth %s, hud scissor %s, radar anchor %s, systems near %d cm, hud up %d cm, gunner up %d cm (live; "
                        "experimental.srv_turret_camera_back, experimental.srv_turret_hud_forward, experimental.srv_turret_radar_forward, experimental.srv_turret_panels_back, experimental.srv_turret_radar_back, experimental.srv_turret_hud_gap, "
                        "experimental.srv_turret_hud_depth, experimental.srv_turret_hud_scissor, experimental.srv_turret_radar_anchor, experimental.srv_turret_systems_near, experimental.srv_turret_hud_up and experimental.srv_turret_gunner_up, TEMPORARY; 0 is the game's own; the depth key is front, game or real, the scissor key "
                        "open or game, the radar anchor key on, reversed, off or camera)",
                        back, forward, radarForward, panels, radar, gap, st::depthModeName(depth), scissorOpen ? "open" : "game", st::radarAnchorModeName(anchorMode), systemsNear, hudUp, gunnerUp);
}

// The constants a group's draws run with at the keys in force, on this eye frame: the group's push-back when above 0, else the pull when the gap is above 0, else the translation alone
// when the group's forward key is above 0 (HUD forward for the panels, radar forward for the radar), else none (false: the draw is the game's own). The systems-near key is the curved menu, not this.
bool paramsOfGroup(st::Group group, const st::EyeFrame& frame, st::TurretParams& out) {
    if (!st::paramsForGroup(frame, group, st::pushCmOf(group, g_panelsCm.load(std::memory_order_relaxed), g_radarCm.load(std::memory_order_relaxed)), g_gapCm.load(std::memory_order_relaxed),
                            st::forwardCmOf(group, g_forwardCm.load(std::memory_order_relaxed), g_radarForwardCm.load(std::memory_order_relaxed)), out, group == st::Group::kPanels ? g_systemsNearCm.load(std::memory_order_relaxed) : 0, group == st::Group::kPanels ? g_hudUpCm.load(std::memory_order_relaxed) : 0))
        return false;
    out = st::withDepth(out, static_cast<st::DepthMode>(g_depthMode.load(std::memory_order_relaxed)));   // experimental.srv_turret_hud_depth
    return true;
}

// Both eyes' frames from a head and eye pose and an anchor: the tangents are the eye's frustum the host gave plus the tangent shift the eye jitter moves it by (nativeTemporalEyeGeometry; the
// game projects with the sum); each frame holds the anchor and the two groups' forward translations in the eye's own space. ok[e] is false for an eye that cannot be built (no anchor, no
// frustum, not a frame): its draws are refused (no-geometry).
void buildEyeFrames(const float head[12], const float eyeToHead[2][12], bool haveAnchor, const double anchor[3], int forwardCm, int radarForwardCm, int nearCm, int hudUpCm, int gunnerUpCm, st::EyeFrame frames[2], bool ok[2]) {
    for (uint32_t e = 0; e < 2; ++e) {
        float tan[4] = {}, shift[2] = {};
        uint64_t sequence = 0;
        const bool haveTan = nativeTemporalEyeGeometry(e, &sequence, tan, shift);
        const float tangents[4] = {tan[0] + shift[0], tan[1] + shift[0], tan[2] + shift[1], tan[3] + shift[1]};
        ok[e] = haveAnchor && haveTan && st::p00Usable(st::p00OfTangents(tangents)) &&
                st::eyeFrameFrom(head, eyeToHead[e], anchor, static_cast<double>(forwardCm) / 100.0, static_cast<double>(radarForwardCm) / 100.0, tangents, frames[e], static_cast<double>(nearCm) / 100.0, static_cast<double>(hudUpCm) / 100.0, static_cast<double>(gunnerUpCm) / 100.0);
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
    buildEyeFrames(head, eyeToHead, haveAnchor, g_anchor.anchor(), forwardCm, radarForwardCm, g_systemsNearCm.load(std::memory_order_relaxed), g_hudUpCm.load(std::memory_order_relaxed), g_gunnerUpCm.load(std::memory_order_relaxed), frames, ok);
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
            buildEyeFrames(head, eyeToHead, true, anchor, g_forwardCm.load(std::memory_order_relaxed), g_radarForwardCm.load(std::memory_order_relaxed), g_systemsNearCm.load(std::memory_order_relaxed), g_hudUpCm.load(std::memory_order_relaxed), g_gunnerUpCm.load(std::memory_order_relaxed), g_drawFrames, g_drawOk);
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
                    "draws[L,R]=%u,%u bound=%u scissor opened %u pose refreshed at draw: %u of %u frames "
                    "refused[no-eye,game-gs,topology,not-ready,no-geometry]=%u/%u/%u/%u/%u families[%s]=%s",
                    panels, radar,
                    static_cast<double>(g_anchorLog[0].load(std::memory_order_relaxed)), static_cast<double>(g_anchorLog[1].load(std::memory_order_relaxed)),
                    static_cast<double>(g_anchorLog[2].load(std::memory_order_relaxed)), static_cast<double>(g_headOffsetCm.load(std::memory_order_relaxed)),
                    static_cast<double>(g_translationLog[0].load(std::memory_order_relaxed)), static_cast<double>(g_translationLog[1].load(std::memory_order_relaxed)),
                    static_cast<double>(g_translationLog[2].load(std::memory_order_relaxed)), static_cast<double>(g_translationRadarLog[0].load(std::memory_order_relaxed)),
                    static_cast<double>(g_translationRadarLog[1].load(std::memory_order_relaxed)), static_cast<double>(g_translationRadarLog[2].load(std::memory_order_relaxed)),
                    1000.0 * static_cast<double>(g_ipd.load(std::memory_order_relaxed)),
                    okL ? static_cast<double>(st::p00OfTangents(&frames[0].l)) : 0.0, okR ? static_cast<double>(st::p00OfTangents(&frames[1].l)) : 0.0,
                    left, right, bound, scissorOpened, poseRefreshed, poseFrames, refused[0], refused[1], refused[2], refused[3], refused[4], names, counts);
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
    detail::g_srvTurretHudCounting.store(false, std::memory_order_relaxed);
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

// ---- the radar anchor moved in the game (experimental.srv_turret_radar_anchor; srv_radar_anchor.h) ----------------------------------------------------------------------
// The frame tick arms the hook the mode needs the first time the Scorpion's turret view is on in it (the placement setter for on and reversed, the schematic slot's update for camera), publishes
// the setter's offset and the slot camera's back distance every frame (zero off the view, in flat and, for the setter, in camera mode: the game's next setter call then gets its own placement)
// and says what the hooks did; the hooks never log. While the last setter call for the radar left our offset in its placement (added, or our own value fed back) the draw path leaves the two
// 5559BD94 billboard families to the game (g_anchorYield), so the picture the game now frames and places itself is not moved a second time. The setter is not called by EDVR: a key change
// reaches the game at its next call.
std::atomic<bool> g_anchorYield{false};
// CAMERA MODE (experimental.srv_turret_radar_anchor = camera; srv_turret_math.h THE RADAR'S OWN CAMERA): set by the frame tick while the view is on in camera mode; the draw path then sends the radar's
// own picture billboards through the regular radar path and leaves every other 5559BD94 draw to the game (cameraLeaves).
std::atomic<bool> g_cameraBillboards{false};

// The turret's forward for the native move (render thread). While the anchor is wanted, eye 0's first draw of a panel or 5559BD94 family each frame gives a camera sample: the live
// camera's right, up and forward in the world (VS b1 rows 277..279) paired with eye 0's seated rotation of the same frame (the host's pose at the draw, as drawEyeFrame uses), which
// srv_turret_math.h turretForwardWorld turns into u, published to the hook of the mode (the setter for on and reversed, the slot camera for camera) with the host frame it belongs to.
//   * THE CPU PATH (the default since the flight of c1275025, whose 2-3 frame GPU readback made the disc lag the turret): the Unmap tee hands every write of the 5376-byte scene camera
//     block to srvTurretNoteCameraWrite, which keeps rows 277..279 by buffer with the host frame; at the draw the bound b1 buffer is looked up, and the rows its last write put there are
//     this draw's (written before it, on this thread). Used when written in this host frame and bound from its first constant.
//   * THE GPU PATH (the fallback when the CPU lookup misses): the rows copied to a staging ring and read back at the frame boundary without stalling (given up after kDirWait frames).
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
    bool failed = false;
};
DirWatch g_dir;
std::atomic<bool> g_anchorDirWanted{false};   // the frame tick's: the Scorpion's turret view is on and the hook of the mode (the slot for camera, the setter for on and reversed) is armed

// The scene camera block's writes (srvTurretNoteCameraWrite, the Unmap tee): rows 277..279 by buffer, with the host frame. Owner context's thread only.
constexpr uint32_t kCamWrites = 16, kCamRowsFloat = kDirFirstRow * 4, kCamRowsEnd = (kDirFirstRow + kDirRows) * 16;
struct CamWrite {
    const void* res = nullptr;
    float rows[12] = {};
    uint64_t hostSeq = 0;
    int64_t qpc = 0;   // when it was written: the sample's time for the hook's pairing
};
CamWrite g_camWrites[kCamWrites];
uint32_t g_camWriteNext = 0;
std::atomic<uint32_t> g_cameraWriteThread{0};   // the thread the scene camera block is written on (the slot camera's 10 s line says it)

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

// The GPU path's copy of the camera rows goes below the hooks (vscreen.cpp vScreenCopySubresourceRegionRaw).
void rawCopyRegion(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT dstX, ID3D11Resource* src, const D3D11_BOX* box) { vScreenCopySubresourceRegionRaw(ctx, dst, dstX, src, box); }

// Rows [first, first + rows) of the vertex-stage buffer at `slot`, counted from the first constant the game bound it with, copied into the stage at byte `to`, clipped to the buffer and to the
// bound range. Returns the rows copied (0: nothing bound, nothing in range, or a fault).
uint32_t copyVsRows(ID3D11DeviceContext* ctx, ID3D11Buffer* stage, uint32_t slot, uint32_t first, uint32_t rows, UINT to, UINT& boundFirst, UINT& boundCount) {
    boundFirst = boundCount = 0;
    if (!stage) return 0;
    st::ExtraCb cb;
    if (!st::readGameCb(ctx, slot, cb, false)) return 0;
    boundFirst = cb.ranged ? cb.first : 0;
    boundCount = cb.ranged ? cb.count : 0;
    D3D11_BUFFER_DESC bd{};
    if (!guarded("srv.turret.anchordir.cbdesc", [&] { cb.buffer->GetDesc(&bd); })) return 0;
    const uint64_t base = static_cast<uint64_t>(boundFirst) + first;
    const uint64_t inBuffer = bd.ByteWidth / 16u;
    if (base >= inBuffer || (cb.ranged && cb.count && first >= cb.count)) return 0;
    uint64_t n = rows;
    if (base + n > inBuffer) n = inBuffer - base;
    if (cb.ranged && cb.count && first + n > cb.count) n = cb.count - first;
    const D3D11_BOX box{static_cast<UINT>(base * 16u), 0, 0, static_cast<UINT>((base + n) * 16u), 1, 1};
    if (!guarded("srv.turret.anchordir.copy", [&] { rawCopyRegion(ctx, stage, to, cb.buffer, &box); })) return 0;
    return static_cast<uint32_t>(n);
}

// The CPU path's lookup: true with the rows (and the QPC time of the write) when the bound b1 buffer's last write was seen this host frame from its first constant.
bool anchorDirCpuRows(ID3D11DeviceContext* ctx, uint64_t hostSeq, float rows[12], int64_t* qpc) {
    st::ExtraCb cb;
    if (!st::readGameCb(ctx, 1, cb, false)) return false;
    const void* res = static_cast<ID3D11Resource*>(cb.buffer);
    const CamWrite* w = nullptr;
    for (const CamWrite& c : g_camWrites)
        if (c.res == res) w = &c;
    if (!w) return false;
    if (cb.ranged && cb.first != 0) return false;
    if (w->hostSeq != hostSeq) return false;
    std::memcpy(rows, w->rows, sizeof(w->rows));
    if (qpc) *qpc = w->qpc;
    return true;
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
    int64_t writtenAt = 0;
    if (anchorDirCpuRows(ctx, poseSeq, cpuRows, &writtenAt)) {
        float u[3];
        if (st::turretForwardWorld(cpuRows, cpuRows + 4, cpuRows + 8, R, u)) {
            g_dir.lastCopyFrame = g_frame;
            g_dir.publishedHostSeq = poseSeq;
            srvRadarAnchorPublishDirection(u, true, poseSeq, true, headPitch, writtenAt);
            return;
        }
    }
    if (!anchorDirEnsure(ctx)) return;
    const uint32_t k = g_dir.next;
    if (g_dir.pending[k]) return;   // the ring is full: this frame is skipped
    UINT first = 0, count = 0;
    if (copyVsRows(ctx, g_dir.stage[k], 1, kDirFirstRow, kDirRows, 0, first, count) != kDirRows) return;   // this draw's b1 is not the camera block: the next family's may be
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
        float u[3];
        if (!st::turretForwardWorld(rows, rows + 4, rows + 8, g_dir.R[k], u)) continue;
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
    bool armTried = false;          // the setter hook's arm was asked for (on and reversed)
    bool on = false;                // the view, as the last tick saw it
    bool asked = false;             // this stay published an offset that was not zero
    bool said = false;              // this stay's first add has been said
    bool slotArmTried = false;      // camera mode: the slot hook's arm was asked for
    bool cameraSeen = false;        // camera mode was on with the view in this 10 s window (the slot camera's line is said)
    bool slotSaid[3][3] = {};       // camera mode: the first move of each kind and list has been said
    uint64_t winSlotCalls = 0, winSlotRadar[3][3] = {}, winSlotMoved = 0, winSlotTurret = 0, winSlotCamera = 0, winSlotFaults = 0;   // the slot hook's counters when the 10 s window began
    uint64_t stayMoved = 0, stayOurs = 0;   // the setter hook's counters when the stay began
    st::RadarAnchorOffset offset;   // the last fallback published ((0, 0, s * F))
};
AnchorWatch g_anchorWatch;

// CAMERA MODE's lines, frame thread. The slot hook's first move of each kind (terrain, icon) and list (A, B: the two eyes), once each.
const char* const kSlotKindName[3] = {"terrain", "icon", "another mode"};
const char* const kSlotListName[3] = {"A", "B", "another list"};
void slotFirstLines(AnchorWatch& w, const RadarSlotStats& r) {
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

// At each 10 s boundary of the frame tick: camera mode's line, whenever it was on with the view in the window (radar slots 0: the hook never saw the radar's slots); the baselines move on either way.
void anchorWindow() {
    AnchorWatch& w = g_anchorWatch;
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
    // The setter hook serves on and reversed, the slot hook serves camera: each is armed the first time the view is on in a mode that needs it.
    const bool setterMode = mode == st::RadarAnchorMode::kOn || mode == st::RadarAnchorMode::kReversed;
    const bool cameraMode = on && mode == st::RadarAnchorMode::kCamera;
    if (on && setterMode && !w.armTried) {
        w.armTried = true;
        (void)srvRadarAnchorArm(nullptr, 0);   // says its own line: "setter hook armed: ..." or "setter hook stood down: <why>; nothing was patched"
    }
    w.offset = st::radarAnchorOffset(on, mode, radarForwardCm);   // zero in camera mode: the restore gives the game its own placement back
    srvRadarAnchorPublish(w.offset.bits, frameNo);
    // CAMERA MODE: the slot hook, armed once when the view is first on in camera mode, and its back distance every frame (0 outside camera mode or the view: the game's camera)
    if (cameraMode && !w.slotArmTried) {
        w.slotArmTried = true;
        (void)srvRadarSlotArm();   // says its own line: "srv turret radar camera: slot hook armed: ..." or "... slot hook stood down: <why>; nothing was patched"
    }
    srvRadarSlotPublish(st::radarCameraBackM(on, mode, radarForwardCm));
    g_cameraBillboards.store(cameraMode, std::memory_order_relaxed);
    RadarSlotStats slot;
    if (cameraMode) {
        w.cameraSeen = true;
        slot = srvRadarSlotStats();
        slotFirstLines(w, slot);
    }
    const RadarAnchorStats a = srvRadarAnchorStats();
    // The turret's direction is read while the hook of the mode is armed: the slot for camera, the setter for on and reversed.
    const bool hookArmed = cameraMode ? slot.state == RadarAnchorHookState::Armed : setterMode && a.state == RadarAnchorHookState::Armed;
    const bool dirWanted = on && hookArmed;
    g_anchorDirWanted.store(dirWanted, std::memory_order_relaxed);
    detail::g_srvTurretCameraRowsWanted.store(dirWanted, std::memory_order_relaxed);   // the Unmap tee's CPU read of the camera rows
    if (on && !w.on) {
        w.stayMoved = a.moved;
        w.stayOurs = a.ours;
        w.asked = w.said = false;
        srvRadarAnchorPublishDirection(nullptr, false);   // a u from an earlier stay is not this one's: the fallback until the render side reads a new one
    }
    if (on) {
        if (!w.offset.zero()) w.asked = true;
        if (!w.said && a.moved > w.stayMoved) {
            w.said = true;
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
        }
    } else if (w.on && setterMode) {
        const uint64_t n = a.moved - w.stayMoved, ours = a.ours - w.stayOurs;
        char also[64] = "";
        if (ours) std::snprintf(also, sizeof(also), " (already ours in %llu)", static_cast<unsigned long long>(ours));
        char tail[600] = "";
        if (a.state != RadarAnchorHookState::Armed)
            std::snprintf(tail, sizeof(tail), " -- hook not armed: %s",
                          a.state == RadarAnchorHookState::StoodDown ? srvRadarAnchorWhy() : "experimental.srv_turret_radar_anchor was off while the view was on");
        else if (n == 0 && ours == 0 && w.asked)
            std::snprintf(tail, sizeof(tail), " -- NEVER RAN: the radar schematic's placement setter was not called with the radar object while the view was on");
        else if (n == 0 && ours == 0)
            std::snprintf(tail, sizeof(tail), " -- not asked: experimental.srv_turret_radar_anchor was off or radar forward 0 while the view was on");
        Log::get().note("srv turret radar anchor: view off: moved in %llu calls%s%s", static_cast<unsigned long long>(n), also, tail);
    }
    w.on = on;
    g_anchorYield.store(on && setterMode && a.state == RadarAnchorHookState::Armed && a.holdsOurs, std::memory_order_relaxed);
    detail::g_srvTurretRadarWritersOn.store(cameraMode, std::memory_order_relaxed);   // the writer watch: camera mode's billboard match
}

bool anchorLeaves(int family) { return st::radarAnchorLeavesFamily(family) && g_anchorYield.load(std::memory_order_relaxed); }

// ---- THE RADAR'S PICTURE WRITERS (camera mode's billboard match) -------------------------------------------------------------------------------------------------------------
// A draw of the radar's own picture billboard (family 7, ps EA02FAC2) shows a texture that the radar's writers drew into earlier in the same frame: the terrain schematic (vs 4EDE4BC9) for the
// disc, or vs 5B0068AF into a 256x256 target for the Scorpion icon. srvTurretRadarWriterDraw records those draws' render targets for the frame and billboardKind matches a billboard's t4
// against them, which is how camera mode (cameraLeaves) tells the radar's own picture from every other 5559BD94 draw. Render thread.
constexpr uint64_t kSchematicTerrainVs = 0x4EDE4BC9003BE352ull;
constexpr uint64_t kRadarIconVs = 0x5B0068AF5630F96Bull;
constexpr uint32_t kWriterTargets = 4;
constexpr int kRadarFamily = 7;   // the radar's own picture billboard: vs 5559BD94 with ps EA02FAC2
static_assert(st::kFamilies[kRadarFamily].vs == 0x5559BD94B6852E83ull && st::kFamilies[kRadarFamily].ps == 0xEA02FAC2BD6C643Cull, "the radar's own picture billboard is family 7");
struct Writers {
    uint32_t frame = 0xFFFFFFFFu;
    const void* disc[kWriterTargets] = {};
    const void* icon[kWriterTargets] = {};
    uint32_t nDisc = 0, nIcon = 0;
};
Writers g_writers;   // this frame's writers

void writersFrame() {
    if (g_writers.frame == g_frame) return;
    g_writers = Writers{};
    g_writers.frame = g_frame;
}
bool addWriter(const void* (&list)[kWriterTargets], uint32_t& n, const void* r) {
    for (uint32_t i = 0; i < n; ++i)
        if (list[i] == r) return false;
    if (n < kWriterTargets) list[n++] = r;
    return true;
}

// The billboard's kind from its PS t4: 1 the disc's picture, 2 the icon's, 0 neither (a texture no radar writer drew into this frame).
int kindOf(const Writers& w, const void* res) {
    for (uint32_t i = 0; i < w.nDisc; ++i)
        if (w.disc[i] == res) return 1;
    for (uint32_t i = 0; i < w.nIcon; ++i)
        if (w.icon[i] == res) return 2;
    return 0;
}
int billboardKind(ID3D11DeviceContext* ctx) {
    writersFrame();
    ID3D11ShaderResourceView* srv = nullptr;
    if (!guarded("srv.turret.billboard.srv", [&] { ctx->PSGetShaderResources(4, 1, &srv); }) || !srv) return 0;
    ID3D11Resource* res = nullptr;
    guarded("srv.turret.billboard.srvres", [&] { srv->GetResource(&res); });
    const int kind = res ? kindOf(g_writers, res) : 0;
    st::release(res);
    st::release(srv);
    return kind;
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

// The radar's picture writers (vscreen.cpp forwardWithVerdict, every owner-context draw while the writers are watched, which is camera mode): the terrain schematic (vs 4EDE4BC9) and the
// Scorpion icon (vs 5B0068AF into a 256x256 target) record their render targets for this frame, which billboardKind matches a billboard's t4 against. Every reference taken is released.
void srvTurretRadarWriterDraw(ID3D11DeviceContext* ctx, uint64_t vs) {
    const bool disc = vs == kSchematicTerrainVs, icon = vs == kRadarIconVs;
    if (!ctx || (!disc && !icon)) return;
    writersFrame();
    ID3D11RenderTargetView* rtv[3] = {};
    if (!guarded("srv.turret.writers.om", [&] { ctx->OMGetRenderTargets(3, rtv, nullptr); })) return;
    for (uint32_t k = 0; k < 3; ++k) {
        if (!rtv[k]) continue;
        ID3D11Resource* res = nullptr;
        guarded("srv.turret.writers.res", [&] { rtv[k]->GetResource(&res); });
        if (res && disc) {
            addWriter(g_writers.disc, g_writers.nDisc, res);
        } else if (res && icon && k == 0) {
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            D3D11_TEXTURE2D_DESC td{};
            guarded("srv.turret.writers.desc", [&] {
                res->GetType(&dim);
                if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
            });
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && td.Width == 256 && td.Height == 256) addWriter(g_writers.icon, g_writers.nIcon, res);
        }
        st::release(res);
        st::release(rtv[k]);
    }
}

// vscreen.cpp, each issue of a matched draw while a family's first scope is open (srv_turret.h srvTurretHudCountIssue): says the state that issue ran with.
void srvTurretHudNoteIssue(ID3D11DeviceContext* ctx) {
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
              panelsCm = g_panelsCm.load(std::memory_order_relaxed), radarCm = g_radarCm.load(std::memory_order_relaxed), gapCm = g_gapCm.load(std::memory_order_relaxed),
              systemsNearCm = g_systemsNearCm.load(std::memory_order_relaxed), hudUpCm = g_hudUpCm.load(std::memory_order_relaxed), gunnerUpCm = g_gunnerUpCm.load(std::memory_order_relaxed);
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
    anchorTick(on, frameNo, radarForwardCm);   // the radar moved in the game (experimental.srv_turret_radar_anchor): nothing unless `on`
    const bool wanted = on && (panelsCm > 0 || radarCm > 0 || forwardCm > 0 || radarForwardCm > 0 || gapCm > 0 || systemsNearCm > 0 || hudUpCm > 0 || gunnerUpCm > 0);   // any HUD key, including the gunner frame and the two height keys; a group whose own keys are 0 is left alone per draw
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
        anchorWindow();
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

// CAMERA MODE (srv_turret_math.h THE RADAR'S OWN CAMERA): the radar's schematic camera is moved back by D in the game, so the radar's own picture billboards (family 7 on a texture this
// frame's radar writers drew: billboardKind) go through the regular radar path, +D along the seat with the rest of the radar; every other 5559BD94 draw (family 8, the panel holograms) is
// the game's own, as in on mode. True: the draw is left to the game.
bool cameraLeaves(ID3D11DeviceContext* ctx, int family) {
    if (!st::radarAnchorLeavesFamily(family) || !g_cameraBillboards.load(std::memory_order_relaxed)) return false;
    if (family == kRadarFamily && billboardKind(ctx) != 0) return false;
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
    // The radar's schematic is moved in the game (on and reversed): the 5559BD94 billboards are the game's own.
    if (anchorLeaves(family)) return false;
    if (cameraLeaves(ctx, family)) return false;   // camera mode: a billboard that is not the radar's own picture is the game's own
    if (eye != 0 && eye != 1) {
        refuse(st::Refuse::kNoEye);
        return false;
    }
    g_draws[eye].fetch_add(1, std::memory_order_relaxed);
    g_familyDraws[family].fetch_add(1, std::memory_order_relaxed);
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
    const auto begun = g_binding.begin(ctx, g_gs[sig], constants, st::directSetGs, needScene ? &scene : nullptr);
    if (begun == st::Binding::Begin::kGameHasGs) {
        refuse(st::Refuse::kGameGs);
        return false;
    }
    if (begun != st::Binding::Begin::kBound) {
        refuse(st::Refuse::kNotReady);
        return false;
    }
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

// The gunner camera's curved panel: the 2016-index sheet (vs 5DAD41AA) and the 288-index cards (vs C4B4B334). Every other index count of those shaders, including the Escape menu's 360, is left to the game. The move is the turret-forward translation in tNear, at the game's own depth.
bool srvTurretGunnerBegin(ID3D11DeviceContext* ctx, uint64_t vs, char kind, uint32_t indexCount, int eye) {
    const int sig = st::gunnerSignature(vs, kind, indexCount);
    if (!ctx || sig < 0 || (g_systemsNearCm.load(std::memory_order_relaxed) <= 0 && g_gunnerUpCm.load(std::memory_order_relaxed) <= 0)) return false;
    if (eye != 0 && eye != 1) {
        refuse(st::Refuse::kNoEye);
        return false;
    }
    g_draws[eye].fetch_add(1, std::memory_order_relaxed);
    st::EyeFrame frame;
    const bool haveGunnerFrame = drawEyeFrame(eye, frame);
    if (!haveGunnerFrame) {
        const st::Refuse missingFrame = st::Refuse::kNoGeometry;
        refuse(missingFrame);
        return false;
    }
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    const bool topologyRead = guarded("srv.turret.gunnertopo", [&] { ctx->IAGetPrimitiveTopology(&topology); });
    const bool gunnerTopology = topologyRead && st::topologyCarried(static_cast<int>(topology));
    if (!gunnerTopology) {
        refuse(st::Refuse::kTopology);
        return false;
    }
    if (g_binding.needsRestore() || g_scissor.owed() || !ensureGs(ctx, static_cast<uint32_t>(sig))) {
        refuse(st::Refuse::kNotReady);
        return false;
    }
    st::TurretParams params;
    if (!st::gunnerParamsFor(frame, params)) return false;
    ID3D11Buffer* constants = g_constants[eye].get(ctx, params);
    if (!constants) {
        refuse(st::Refuse::kNotReady);
        return false;
    }
    const auto begun = g_binding.begin(ctx, g_gs[sig], constants, st::directSetGs, nullptr);
    if (begun == st::Binding::Begin::kGameHasGs) {
        refuse(st::Refuse::kGameGs);
        return false;
    }
    if (begun != st::Binding::Begin::kBound) {
        refuse(st::Refuse::kNotReady);
        return false;
    }
    {
        const bool apply = g_scissorOpen.load(std::memory_order_relaxed);
        if (apply || !g_gunnerNoted) {
            st::Scissor::Seen seen;
            const auto gunnerOpened = g_scissor.open(ctx, seen, apply);
            if (gunnerOpened == st::Scissor::Open::kOpened) g_scissorOpened.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_bound.fetch_add(1, std::memory_order_relaxed);
    if (!g_gunnerNoted) {
        g_gunnerNoted = true;
        Log::get().note("srv turret gunner: the curved panel moves %.2f m nearer and %.2f m up along the turret (frame %u, eye %d, depth game). The centre reticle and the Escape menu stay.",
                        static_cast<double>(g_systemsNearCm.load(std::memory_order_relaxed)) / 100.0, static_cast<double>(g_gunnerUpCm.load(std::memory_order_relaxed)) / 100.0, g_frame, eye);
    }
    return true;
}

void srvTurretHudEnd(ID3D11DeviceContext* ctx) {
    noteEnd();
    const auto done = g_binding.finish(ctx);
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
    noteEnd();
    srvRadarAnchorShutdown();   // the hook's gate shuts: the relay forwards straight to the game's function
    g_anchorYield.store(false, std::memory_order_relaxed);
    g_cameraBillboards.store(false, std::memory_order_relaxed);
    detail::g_srvTurretRadarWritersOn.store(false, std::memory_order_relaxed);
    g_writers = Writers{};
    g_anchorDirWanted.store(false, std::memory_order_relaxed);
    detail::g_srvTurretCameraRowsWanted.store(false, std::memory_order_relaxed);
    for (auto& w : g_camWrites) w = CamWrite{};
    for (auto& s : g_dir.stage) st::release(s);
    g_dir = DirWatch{};
    g_binding.clear();
    g_scissor.clear();
    for (auto& c : g_constants) c.reset();
    releaseGs();
    g_gsTried = false;
}

}  // namespace edvr
