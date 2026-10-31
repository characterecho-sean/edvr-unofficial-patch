// The SRV turret view (issue 140): the module. srv_turret.h says what it does and what the log shows; srv_turret_math.h has the maths, the families and the seven TEMPORARY
// keys; srv_turret_hud_binding.h holds the constants and the binding, which tools/srv_turret_hud_test runs on WARP; srv_turret_hud_shader.h is the geometry shaders' HLSL,
// compiled at build time.
#include "srv_turret.h"

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
std::atomic<int> g_depthMode{0};            // fix.srv_turret_hud_depth as a st::DepthMode (0 game, 1 real, 2 front)
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
    const std::string depthText = cfg.getString("fix.srv_turret_hud_depth", "game");
    const st::DepthMode depth = st::depthModeOf(depthText.c_str());
    const bool changed = !g_keysRead || back != g_backCm.load(std::memory_order_relaxed) || forward != g_forwardCm.load(std::memory_order_relaxed) ||
                         radarForward != g_radarForwardCm.load(std::memory_order_relaxed) || panels != g_panelsCm.load(std::memory_order_relaxed) || radar != g_radarCm.load(std::memory_order_relaxed) ||
                         gap != g_gapCm.load(std::memory_order_relaxed) || static_cast<int>(depth) != g_depthMode.load(std::memory_order_relaxed);
    g_keysRead = true;
    g_keysGeneration = generation;
    g_backCm.store(back, std::memory_order_relaxed);
    g_forwardCm.store(forward, std::memory_order_relaxed);
    g_radarForwardCm.store(radarForward, std::memory_order_relaxed);
    g_panelsCm.store(panels, std::memory_order_relaxed);
    g_radarCm.store(radar, std::memory_order_relaxed);
    g_gapCm.store(gap, std::memory_order_relaxed);
    g_depthMode.store(static_cast<int>(depth), std::memory_order_relaxed);
    if (changed)
        Log::get().note("srv turret settings: camera back %d cm, hud forward %d cm, radar forward %d cm, panels back %d cm, radar back %d cm, hud gap %d cm, hud depth %s (live; fix.srv_turret_camera_back, "
                        "fix.srv_turret_hud_forward, fix.srv_turret_radar_forward, fix.srv_turret_panels_back, fix.srv_turret_radar_back, fix.srv_turret_hud_gap and fix.srv_turret_hud_depth, TEMPORARY; "
                        "0 is the game's own; the depth key is game, real or front)",
                        back, forward, radarForward, panels, radar, gap, st::depthModeName(depth));
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
    const uint32_t bound = g_bound.exchange(0, std::memory_order_relaxed);
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
                    "draws[L,R]=%u,%u bound=%u pose refreshed at draw: %u of %u frames "
                    "refused[no-eye,game-gs,topology,not-ready,no-geometry]=%u/%u/%u/%u/%u families[%s]=%s",
                    panels, radar,
                    static_cast<double>(g_anchorLog[0].load(std::memory_order_relaxed)), static_cast<double>(g_anchorLog[1].load(std::memory_order_relaxed)),
                    static_cast<double>(g_anchorLog[2].load(std::memory_order_relaxed)), static_cast<double>(g_headOffsetCm.load(std::memory_order_relaxed)),
                    static_cast<double>(g_translationLog[0].load(std::memory_order_relaxed)), static_cast<double>(g_translationLog[1].load(std::memory_order_relaxed)),
                    static_cast<double>(g_translationLog[2].load(std::memory_order_relaxed)), static_cast<double>(g_translationRadarLog[0].load(std::memory_order_relaxed)),
                    static_cast<double>(g_translationRadarLog[1].load(std::memory_order_relaxed)), static_cast<double>(g_translationRadarLog[2].load(std::memory_order_relaxed)),
                    1000.0 * static_cast<double>(g_ipd.load(std::memory_order_relaxed)),
                    okL ? static_cast<double>(st::p00OfTangents(&frames[0].l)) : 0.0, okR ? static_cast<double>(st::p00OfTangents(&frames[1].l)) : 0.0,
                    left, right, bound, poseRefreshed, poseFrames, refused[0], refused[1], refused[2], refused[3], refused[4], names, counts);
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
        Log::get().note("srv turret hud state: family=%s eye=%d issue=%u target=%s %ux%u format=%u depth_view=%s | depth: enable=%d func=%d write=%d | stencil: enable=%d ref=%u read=0x%02X "
                        "write=0x%02X front[func,pass,fail,zfail]=%d,%d,%d,%d | blend rt0: enable=%d src=%d dst=%d op=%d srcA=%d dstA=%d opA=%d mask=0x%X",
                        st::kFamilies[g_noteFamily].name, g_noteEye, g_noteIssue, target, w, h, fmt, dsv ? "yes" : "no", static_cast<int>(dd.DepthEnable), static_cast<int>(dd.DepthFunc),
                        static_cast<int>(dd.DepthWriteMask), static_cast<int>(dd.StencilEnable), stencilRef, static_cast<unsigned>(dd.StencilReadMask), static_cast<unsigned>(dd.StencilWriteMask),
                        static_cast<int>(dd.FrontFace.StencilFunc), static_cast<int>(dd.FrontFace.StencilPassOp), static_cast<int>(dd.FrontFace.StencilFailOp),
                        static_cast<int>(dd.FrontFace.StencilDepthFailOp), static_cast<int>(rt.BlendEnable), static_cast<int>(rt.SrcBlend), static_cast<int>(rt.DestBlend), static_cast<int>(rt.BlendOp),
                        static_cast<int>(rt.SrcBlendAlpha), static_cast<int>(rt.DestBlendAlpha), static_cast<int>(rt.BlendOpAlpha), static_cast<unsigned>(rt.RenderTargetWriteMask));
        if (dsv) dsv->Release();
    });
}

}  // namespace

// vscreen.cpp, each issue of a matched draw while a scope is counting (srv_turret.h srvTurretHudCountIssue): counted for the sampled scope's statistics line, and said for a family's first scope.
void srvTurretHudNoteIssue(ID3D11DeviceContext* ctx) {
    detail::g_srvTurretHudIssues.fetch_add(1, std::memory_order_relaxed);
    if (g_noteFamily >= 0 && ctx == g_noteCtx && g_noteIssue < kNoteIssues) {
        ++g_noteIssue;
        noteState(ctx);
    }
}

void srvTurretFrameTick(uint32_t frameNo) {
    if (!runtimeVrProfile()) return;   // flat (or not yet decided): every state stays at its zero, alignTick adds an exact 0
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
    if (!ctx || family < 0 || family >= static_cast<int>(kFamilyN)) return false;
    // A family whose group has none of its push, the gap and the HUD-forward key above 0 is not the pass's (one group on and the other off is a setting, and the keys can go to 0 between the
    // frame tick and this draw): the draw is the game's own and is not counted among the matched ones, so the 10 s line's bound share is judged on the draws the pass was asked to carry.
    st::TurretParams unused;
    const st::Group group = st::kFamilies[family].group;
    if (!paramsOfGroup(group, st::EyeFrame{}, unused)) return false;
    if (eye != 0 && eye != 1) {
        refuse(st::Refuse::kNoEye);
        return false;
    }
    g_draws[eye].fetch_add(1, std::memory_order_relaxed);
    g_familyDraws[family].fetch_add(1, std::memory_order_relaxed);
    const uint32_t ordinal = g_probe[eye][family].ordinal++;   // this draw's place among the frame's matched draws of the eye and family (the instrument's rotation)
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
    if (g_binding.needsRestore() || !ensureGs(ctx, sig)) {
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
    for (auto& eyeProbes : g_probe)
        for (Probe& p : eyeProbes) probeReset(p);
    g_binding.clear();
    for (auto& c : g_constants) c.reset();
    releaseGs();
    g_gsTried = false;
}

}  // namespace edvr
