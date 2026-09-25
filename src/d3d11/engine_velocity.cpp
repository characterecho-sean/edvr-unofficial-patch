#include "engine_velocity.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "depth_probe.h"
#include "gpu_timing.h"
#include "dxbc_engine_velocity.h"
#include "engine_velocity_emit.h"
#include "engine_velocity_families.h"
#include "engine_velocity_state.h"
#include "kinematic_eval_hook.h"
#include "kinematic_eval_probe.h"
#include "vscreen.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "../common/timing.h"

namespace edvr {
namespace engine_velocity_detail {

template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
namespace emit = engine_velocity_emit;

std::atomic<bool> live{false};
DrawCache cache;
uint64_t familyDraws[kMaxFamilies] = {};
std::atomic<const ID3D11Resource*> watch[kWatchSlots] = {};

// --- The keyed pool families -------------------------------------------------
// Vertex-shader hash -> the pixel shaders measured with it (blur-on run 043720
// and the 09-06 dump; docs/kinematic-motion-injection-2026-09-19.md). A hash
// that is not here is not substituted: after a game update the family stands
// down by name, the way every keyed fix in EDVR does -- and so does a shader
// another mod (EDHM's 3Dmigoto) replaces: its hash is not here.
//
// The station (eye run 143416, docs/kinematic-motion-injection-2026-09-19.md
// "The station"): ps_CB429E043DBB2506 (vs_DE54, 108 station instances a frame)
// and ps_451A82D4DD1BA254 (vs_61AE) drew station parts stock, so their pixels
// kept the camera term while the station turned; both keyed, each proven by
// the corpus identity harness (o0..o3 and depth bit-identical). Not keyed:
// ps_B7D50283329322C3 (vs_EB52 -- the commander's legs, records 2 m away;
// decided).
//
// Flight 6 (153446: the shader dump armed at a station, the "Flight 6" entry):
// vs_436193B352A2897E, the station's biggest pool shader (547 of its 1193
// instances a frame, 143416) with its only pixel shader ps_16940F576006BE65;
// vs_889A5279E68F0672 with ps_B46E52A1E0B2F39C (the station) and
// ps_EBA95E15B0A66102 -- each pair through the corpus identity harness
// (40,960 texels, 0 mismatches; MRT6 8192 checked, 0 bad). Not keyable:
// vs_DE54's ps_91F8937EDA723663 and ps_A6070F9DD1CFB601, whose input register
// the family's SV_Position sits at holds another semantic (the patcher's
// refusal; engine_velocity_test's --corpus candidates print it).
using engine_velocity_family::Family;
using engine_velocity_family::kFamilies;
using engine_velocity_family::kFamilyCount;
using engine_velocity_family::familyForProfile;
using engine_velocity_family::keyedPs;
static_assert(kFamilyCount <= kMaxFamilies, "familyDraws holds every family");
bool anyKeyedPs(uint64_t hash) {
    for (int i = 0; i < kFamilyCount; ++i)
        if ((!kFamilies[i].flatOnly || runtimeFlatProfile()) && keyedPs(i, hash)) return true;
    return false;
}

std::recursive_mutex g_mutex;   // shader memory, patches, eyes, the draw path's slow half

struct VsInfo {
    Ptr<ID3D11VertexShader> object;   // held: no address reuse while remembered
    int family = -1;
    std::vector<BYTE> bytes;
    bool linked = false;
};
struct PsInfo {
    Ptr<ID3D11PixelShader> object;
    uint64_t hash = 0;
    std::vector<BYTE> bytes;
    bool linked = false;
};
std::unordered_map<ID3D11VertexShader*, VsInfo> g_vs;
std::unordered_map<ID3D11PixelShader*, PsInfo> g_ps;
constexpr size_t kRememberCap = 512;   // keyed shader objects kept (a few KB of bytecode each)

struct FamilyState {
    bool derived = false, valid = false;
    EngineVelocityInputs inputs{};
    std::string reason;                            // why it stood down (empty = live)
    std::unordered_map<ID3D11VertexShader*, Ptr<ID3D11VertexShader>> patchedVs;
    std::unordered_map<ID3D11PixelShader*, Ptr<ID3D11PixelShader>> patchedPs;
    std::unordered_map<ID3D11PixelShader*, std::string> psFailed;
    uint64_t binds = 0;                            // substitutions made (this window)
    uint64_t unkeyedPsDraws = 0;                   // bind events with a pixel shader outside the keyed set
    uint64_t unkeyedPsHash = 0;
};
FamilyState g_families[kFamilyCount];

// What EDVR bound in place of the game's state, and at which generation of
// the game's own binding, so a later look can tell whether it is still bound.
struct Bound {
    ID3D11PixelShader* originalPs = nullptr;
    ID3D11PixelShader* patchedPs = nullptr;
    uint32_t psGen = 0;
    ID3D11VertexShader* originalVs = nullptr;
    ID3D11VertexShader* patchedVs = nullptr;
    uint32_t vsGen = 0;
    // The blend state: the game's (held, for the restore) and the derived
    // one EDVR bound in its place (held: its address is an identity below).
    Ptr<ID3D11BlendState> gameBlend;
    Ptr<ID3D11BlendState> derivedBlend;
    float blendFactor[4] = {};
    UINT sampleMask = ~0u;
    uint32_t blendGen = 0;
    int family = -1;
};
Bound g_bound;
std::atomic<bool> g_anyBound{false};   // EDVR state may still be bound (owner thread restores)

// The derived blend states, one per game state (held, so a pointer is an
// identity). A few in practice; cleared past the cap.
struct BlendEntry {
    Ptr<ID3D11BlendState> game;
    Ptr<ID3D11BlendState> derived;
    const char* refused = nullptr;
};
std::vector<BlendEntry> g_blends;
constexpr size_t kBlendCap = 64;

// --- The watched sources --------------------------------------------------------
// Per watch slot (eye0 pool, eye0 scene, eye1 pool, eye1 scene, source pool,
// source scene): the writes seen since the slot was assigned, and for the
// scene constants the registers 270..275 the last Unmap left.
constexpr unsigned kRowsFirst = 270, kRowsBytes = 6 * 16;
struct WatchInfo {
    const ID3D11Resource* resource = nullptr;
    void* mapped = nullptr;
    int mapType = 0;
    uint32_t replaceEpoch = 0, appendEpoch = 0, writeEpoch = 0;
    bool rowsKnown = false;
    uint8_t rows[kRowsBytes] = {};
};
WatchInfo g_watchInfo[kWatchSlots];

// --- Per eye -------------------------------------------------------------------
enum Invalid : int {
    kPoolRewritten = 0, kPoolRebound, kPoolView, kSceneRebound, kSceneRows, kSceneUnknown, kDepthChanged,
    kShadow, kNoPool, kNoScene, kCreate, kInvalidCount
};
const char* const kInvalidNames[kInvalidCount] = {
    "pool rewritten", "pool rebound", "pool view changed", "scene constants rebound",
    "scene rows 270..275 changed", "scene constants rewritten, rows unknown", "depth changed",
    "binding shadow disagreed", "no pool", "no scene constants", "create failed"};

struct Eye {
    Ptr<ID3D11Texture2D> depth;          // the scene depth this eye's slot target matches
    Ptr<ID3D11Texture2D> slots;
    Ptr<ID3D11RenderTargetView> slotsRtv;
    Ptr<ID3D11ShaderResourceView> slotsSrv;
    unsigned width = 0, height = 0;
    Ptr<ID3D11Buffer> pool;              // the snapshot copies
    Ptr<ID3D11ShaderResourceView> poolSrv;
    UINT poolBytes = 0;
    Ptr<ID3D11Buffer> scene[2];          // the game's cb1, by present-frame parity
    uint32_t sceneFrame[2] = {~0u, ~0u};
    UINT sceneBytes = 0;
    uint32_t frame = ~0u;                // the present frame this eye's data belongs to
    uint32_t rtvGen = 0, dsvGen = 0;     // the pass binding MRT6 was added to
    bool bindingStale = false;           // an internal flat restore removed MRT6 without a game generation
    // The snapshot's sources, held (an equal pointer is then the same object),
    // and what they held: the view's element range, the watch epochs, the rows.
    Ptr<ID3D11ShaderResourceView> poolView;
    Ptr<ID3D11Buffer> poolBuffer;
    Ptr<ID3D11Buffer> sceneBuffer;
    UINT poolFirst = 0, poolCount = 0;
    uint32_t poolReplaceEpoch = 0, poolAppendEpoch = 0, sceneWriteEpoch = 0;
    bool sceneRowsKnown = false;
    uint8_t sceneRows[kRowsBytes] = {};
    // written: a substituted draw was issued this eye-frame (the views need
    // it); boundCounted: MRT6's bind counted once per eye-frame.
    bool bound = false, boundCounted = false, written = false, invalid = false, consumed = false;
    // The frames this eye last saw a pool family draw and last substituted
    // (the performance review's item 2 accounting).
    uint32_t seenFrame = ~0u, substFrame = ~0u;
};
// Eyes 0 and 1, and the on-foot source (kEngineVelocitySourceEye): the same
// eye-frame rules for a pass into the source's depth.
Eye g_eyes[3];
static_assert(kEngineVelocitySourceEye == 2 && kWatchSlots == 6, "one watch pair per eye, the source's last");
// The on-foot source's depth as screen_motion last named it, and the present
// frame it did (~0u: never). A pool draw into this depth is the source pass
// while the naming is at most two frames old.
Ptr<ID3D11Texture2D> g_sourceDepth;
uint32_t g_sourceNoted = ~0u;
// The source camera (flight 5, 2026-09-23 140351): the scene constants the
// naming draw reads (screen_motion's terrain/scene draw, the camera its own
// camera term uses) and their rows 270..275 as the watch last saw them
// written, for the present frame of that naming. The source's pool draws are
// held to it draw by draw: a handful drawn under other rows (the same as the
// world's standing still, different walking) no longer drop the frame.
struct SourceCamera {
    Ptr<ID3D11Buffer> scene;
    bool rowsKnown = false;
    uint8_t rows[kRowsBytes] = {};
    uint32_t frame = ~0u;
};
SourceCamera g_sourceCamera;
// Why a source pool draw was declined (not substituted; the frame kept).
enum SourceDecline : int { kBeforeNaming = 0, kNamingUnseen, kOtherScene, kRowsUnseen, kOtherCamera, kSourceDeclineCount };
const char* const kSourceDeclineNames[kSourceDeclineCount] = {
    "before this frame's naming", "the naming's rows not seen", "other scene constants", "rows not seen",
    "another camera"};
uint32_t g_sourceDeclineFrame = ~0u;   // the present frame whose first decline was counted

std::atomic<uint32_t> g_frame{0};

// --- The emit side (job threads) ---------------------------------------------
std::unique_ptr<emit::Table> g_table;
std::unique_ptr<emit::Census> g_census;
emit::Stats g_emit;
std::atomic<emit::LookupFn> g_lookup{nullptr};
std::atomic<uint64_t> g_emitSampled{0}, g_emitSampledTicks{0};
const char* g_verifyWhy = nullptr;            // the build check's refusal, null = passed
const char* g_hookWhy = "not checked yet";    // the emit hook's, null = installed
std::atomic<bool> g_emitLive{false};          // both passed: the feature stands up
// The emit's own want on the shared eval hooks (kinematicEvalEmitAttach),
// its own since the 2026-09-23 performance review, item 1. Retried quietly
// at later configures.
bool g_emitAttached = false;
// Engine motion's diagnostics (engineVelocityDiagnostics): the emit's census
// of one record in eight runs only while they are wanted.
std::atomic<bool> g_diagnosticsWanted{false};

// --- Draw-side and pixel counters (owner thread) -------------------------------
struct DrawStats {
    uint64_t slowPaths = 0, slowTicks = 0, quickPaths = 0;
    uint64_t eyeFrames = 0, eyeFramesBound = 0;
    // Item 2: eye-frames with a pool family draw (the old order prepared all
    // of these) and with a substitution; eyeFrames above is those prepared.
    uint64_t eyeFramesSeen = 0, eyeFramesSubstituted = 0;
    uint64_t invalid[kInvalidCount] = {};
    uint64_t poolRefreshed = 0, sceneRowsKept = 0;
    uint64_t targetOccupied = 0, uavBound = 0, bindRejected = 0, depthUnsupported = 0, createFailed = 0;
    uint64_t bindRefused[static_cast<int>(EngineVelocityBindRefusal::Count)] = {};
    uint64_t blendApplied = 0, blendRefused = 0, blendShadowDisagreed = 0;
    uint64_t settersIssued = 0, settersSkipped = 0;   // item 3: raw shader setters, and those found installed
    // Item 4 (measure only): the snapshots' copies -- pool and scene constants
    // at each prepared eye-frame, the pool again on each append refresh -- and
    // the largest pool (records) and view (elements) seen this window.
    uint64_t poolSnapshots = 0, poolSnapshotBytes = 0, poolRefreshBytes = 0, sceneSnapshots = 0, sceneSnapshotBytes = 0;
    uint64_t poolCapacity = 0, poolExposed = 0;
    const char* blendRefusedWhy = nullptr;
    uint64_t viewsAsked = 0, viewsGiven = 0;
    // Why a view request was refused, first failing test: the emit side stood
    // down, another depth texture, another frame's data (a clock-order problem
    // shows here), the eye-frame invalidated, MRT6 never bound, last frame's
    // scene constants missing (the first frame, or a gap).
    uint64_t refusedNoEmit = 0, refusedDepth = 0, refusedFrame = 0, refusedInvalid = 0, refusedUnwritten = 0,
             refusedPrevious = 0;
    uint64_t restores = 0;
    uint64_t frames = 0;
    uint64_t pixelsJoined = 0, pixelsMasked = 0, pixelsCamera = 0, pixelsStale = 0, pixelsCorrupt = 0, pixelReads = 0;
    // The on-foot source: its eye-frames (also counted in eyeFrames above),
    // the screen shader's view requests and refusals, and its panel pixels.
    uint64_t sourceFrames = 0, sourceFramesBound = 0;
    uint64_t sourceViewsAsked = 0, sourceViewsGiven = 0;
    uint64_t sourceRefusedNoEmit = 0, sourceRefusedDepth = 0, sourceRefusedFrame = 0, sourceRefusedInvalid = 0,
             sourceRefusedUnwritten = 0, sourceRefusedPrevious = 0;
    // The source camera rule (flight 5), per check (a slow-path visit: a new
    // binding or a cb1 write; a draw repeating the last one's state runs as
    // it did): checks held to the naming's camera, checks declined by reason
    // and (another camera) by family, the frames with a decline, which rows
    // the other cameras changed (270..272, 273, 274, 275) and the largest
    // camera-position distance among them (m);
    // namings whose rows the watch had not seen; the source's own
    // invalidations by reason (they share g_draw.invalid with the eyes).
    uint64_t sourceHeld = 0, sourceDeclined[kSourceDeclineCount] = {}, sourceDeclinedFamily[kFamilyCount] = {};
    uint64_t sourceDeclineFrames = 0, sourceOtherRows[4] = {}, sourceNamings = 0, sourceNamingsUnseen = 0;
    uint64_t sourceNamingsBy[2] = {};   // by EngineVelocitySourceSignal: terrain or scene draw, the screen's depth
    double sourceOtherShiftMax = 0.0;
    uint64_t sourceInvalid[kInvalidCount] = {};
    // The screen shader's per-kind eye-pixel counts: [0] every pixel of every
    // frame (diagnostics or motion_source), [1] sampled (one frame in
    // kPanelSampleFrames, one eye pixel in kPanelSampleStride squared).
    uint64_t panel[2][5] = {}, panelDraws[2] = {};
    uint64_t burstFrames = 0, burstGaps = 0;
    void clear() { *this = DrawStats{}; }
};
DrawStats g_draw;
uint64_t g_windowStartMs = 0;
uint64_t g_lastGaps = 0;                 // g_emit.gaps at the last frame boundary
uint32_t g_lastSubstitution = ~0u;       // the present frame of the last substitution
constexpr uint64_t kSummaryMs = 30000;
constexpr uint32_t kResumeFrames = 90;   // a substitution after this many quiet frames is logged
constexpr uint64_t kBurstGaps = 32;      // gaps in one frame that make it a burst

uint32_t frameNow() { return g_frame.load(std::memory_order_acquire); }

// --- The eye-pass capture's GPU time (the performance review, item 5) ---------
// The slot target's clear, the snapshots at preparation and the append
// refreshes run in the game's own eye pass, before the temporal pass's prep;
// its timers never saw them. GPU timestamps around each (GpuTimer: a shared
// clock lease, polled without flushing or waiting at the owner's frame
// boundary), taken per price window by the temporal pass.
enum CaptureKind : int { kCaptureClear = 0, kCaptureSnapshot, kCaptureRefresh, kCaptureKinds };
static_assert(kCaptureKinds == 3, "EngineVelocityCaptureGpu carries the three kinds");
struct CaptureTimer { GpuTimer timer; int kind = -1; bool pending = false; };
constexpr int kCaptureTimers = 24;
constexpr size_t kCaptureSamples = 4096;
CaptureTimer g_captureTimers[kCaptureTimers];
std::vector<double> g_captureMs[kCaptureKinds];
uint64_t g_captureUntimed = 0, g_captureInvalid = 0;

int beginCapture(ID3D11DeviceContext* ctx, int kind) {
    for (int i = 0; i < kCaptureTimers; ++i) {
        CaptureTimer& t = g_captureTimers[i];
        if (t.pending) continue;
        Ptr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (!dev || !t.timer.begin(dev.Get(), ctx)) { ++g_captureUntimed; return -1; }
        t.kind = kind;
        t.pending = true;
        return i;
    }
    ++g_captureUntimed;   // every timer still in flight: this one goes untimed, counted
    return -1;
}
void endCapture(ID3D11DeviceContext* ctx, int i) {
    if (i >= 0) g_captureTimers[i].timer.end(ctx);   // a refused end polls Invalid
}
void pollCaptures(ID3D11DeviceContext* ctx) {
    for (auto& t : g_captureTimers) {
        if (!t.pending) continue;
        double ms = 0.0;
        const GpuTimerPoll r = t.timer.poll(ctx, ms);
        if (r == GpuTimerPoll::Pending) continue;
        if (r == GpuTimerPoll::Ready) {
            if (g_captureMs[t.kind].size() < kCaptureSamples) g_captureMs[t.kind].push_back(ms);
        } else {
            ++g_captureInvalid;
        }
        t.pending = false;
    }
}

// --- The build-keyed engine side ----------------------------------------------
// FUN_143696FA0's first 32 bytes (the dictionary lookup the bracket calls) and
// FUN_144312E00's append sequence at 0x144313185 (the node count at +0x18,
// the mask store at +0xAA0+i*8, the owner count at +0x2A4): the layout the
// bracket's reads and writes rest on, in the hash-verified exe (332841).
constexpr uintptr_t kLookupRva = 0x3696FA0u;
constexpr uint8_t kLookupPrologue[32] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83,
    0xEC, 0x40, 0x8B, 0x42, 0x18, 0x48, 0x8B, 0xDA, 0x33, 0xD2, 0x4C, 0x8B, 0xF1, 0x48, 0xF7, 0x71};
constexpr uintptr_t kAppendRva = 0x4313185u;
constexpr uint8_t kAppendBytes[23] = {
    0x48, 0x8B, 0x42, 0x18,                                // mov rax,[rdx+18h]      the node's count
    0x4C, 0x89, 0xBC, 0xC2, 0xA0, 0x0A, 0x00, 0x00,        // mov [rdx+rax*8+0AA0h],r15  the record's mask
    0x48, 0xFF, 0x42, 0x18,                                // inc qword [rdx+18h]
    0x41, 0xFF, 0x85, 0xA4, 0x02, 0x00, 0x00};             // inc dword [r13+2A4h]   the owner's count

#ifndef EDVR_ENGINE_VELOCITY_RIG
const char* verifyEngine(uintptr_t base) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) return "not build 332841 (no PE header)";
        uint32_t timestamp = 0, imageSize = 0;
        std::memcpy(&timestamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(&imageSize, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
        if (timestamp != KinematicEvalProbe::kExpectedTimestamp || imageSize != KinematicEvalProbe::kExpectedImageSize)
            return "not build 332841 (PE timestamp/size)";
        if (std::memcmp(reinterpret_cast<const void*>(base + kLookupRva), kLookupPrologue, sizeof(kLookupPrologue)))
            return "lookup mismatch at RVA 0x3696FA0";
        if (std::memcmp(reinterpret_cast<const void*>(base + kAppendRva), kAppendBytes, sizeof(kAppendBytes)))
            return "append sequence mismatch at RVA 0x4313185";
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "engine image unreadable";
    }
}
#else
// tools/engine_velocity_test drives this file's draw half on WARP: there is
// no engine image to check, and the rig's own fixture stands in for the emit.
const char* verifyEngine(uintptr_t) noexcept { return nullptr; }
#endif

void observeEmit(uintptr_t record, uintptr_t owner, int32_t before, int32_t after) noexcept {
    if (!live.load(std::memory_order_acquire) || !g_table) return;
    const uint64_t n = g_emit.calls.load(std::memory_order_relaxed);
    const bool sample = (n & 63u) == 0;
    const int64_t t0 = sample ? qpcNow() : 0;
    // The census (a hash, a pose read and a lock even on zero-item calls) is
    // a diagnostic: off unless engine motion's diagnostics want it.
    emit::observe(record, owner, before, after, frameNow(), g_lookup.load(std::memory_order_acquire), *g_table, g_emit,
                  g_diagnosticsWanted.load(std::memory_order_relaxed) ? g_census.get() : nullptr);
    if (sample) {
        g_emitSampled.fetch_add(1, std::memory_order_relaxed);
        g_emitSampledTicks.fetch_add(static_cast<uint64_t>(qpcNow() - t0), std::memory_order_relaxed);
    }
}

// The stand-down rule (the 2026-09-23 review): with no emit, no record gets a
// previous pose, and zero emit calls must never pass for correct static
// motion. So: no substitution, every view refused, and the log says why.
void logStoodDown() {
    Log::get().note("engine motion: STOOD DOWN -- the emit hook is not installed (%s): no record gets a previous pose, "
                    "so no pool shader is substituted and the temporal pass is refused every engine input (its other "
                    "motion sources stand).",
                    g_verifyWhy ? g_verifyWhy : g_hookWhy ? g_hookWhy : "unknown");
}
// Re-read the hook's state (three relaxed loads); log a change.
void refreshEmitStatus(bool quiet) {
    const char* why = nullptr;
    const bool hook = kinematicEvalEmitHookLive(&why);
    g_hookWhy = hook ? nullptr : why;
    const bool up = hook && !g_verifyWhy;
    const bool was = g_emitLive.exchange(up, std::memory_order_acq_rel);
    if (quiet || up == was) return;
    if (up) Log::get().note("engine motion: the emit hook is installed; engine-record velocity stands up.");
    else logStoodDown();
}

// --- Patching -------------------------------------------------------------------
thread_local bool t_creating = false;

void deriveFamily(int f) {
    FamilyState& s = g_families[f];
    if (s.derived) return;
    for (auto& [ptr, info] : g_vs) {
        if (info.family != f) continue;
        s.derived = true;
        if (info.linked) { s.reason = "vertex shader uses class linkage"; return; }
        std::string why;
        s.valid = engineVelocityDeriveInputs(info.bytes.data(), info.bytes.size(), s.inputs, why);
        if (!s.valid) s.reason = "signature: " + why;
        return;
    }
}

ID3D11VertexShader* patchedVsFor(ID3D11DeviceContext* ctx, int f, ID3D11VertexShader* vs) {
    FamilyState& s = g_families[f];
    auto found = s.patchedVs.find(vs);
    if (found != s.patchedVs.end()) return found->second.Get();
    auto info = g_vs.find(vs);
    Ptr<ID3D11VertexShader> patched;
    if (info != g_vs.end() && !info->second.linked) {
        std::vector<BYTE> out;
        std::string why;
        if (engineVelocityPatchVs(info->second.bytes.data(), info->second.bytes.size(), s.inputs, out, why)) {
            Ptr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            t_creating = true;
            const HRESULT hr = dev->CreateVertexShader(out.data(), out.size(), nullptr, &patched);
            t_creating = false;
            if (FAILED(hr)) { char t[64]; _snprintf_s(t, _TRUNCATE, "CreateVertexShader 0x%08X", unsigned(hr)); s.reason = t; patched.Reset(); }
        } else {
            s.reason = "vertex patch: " + why;
        }
    }
    s.patchedVs.emplace(vs, patched);
    return patched.Get();
}

ID3D11PixelShader* patchedPsFor(ID3D11DeviceContext* ctx, int f, ID3D11PixelShader* ps) {
    FamilyState& s = g_families[f];
    auto found = s.patchedPs.find(ps);
    if (found != s.patchedPs.end()) return found->second.Get();
    auto info = g_ps.find(ps);
    Ptr<ID3D11PixelShader> patched;
    std::string why = info == g_ps.end() ? "pixel shader bytecode not kept" : info->second.linked ? "class linkage" : "";
    if (why.empty()) {
        std::vector<BYTE> out;
        if (engineVelocityPatchPs(info->second.bytes.data(), info->second.bytes.size(), s.inputs, out, why)) {
            Ptr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            t_creating = true;
            const HRESULT hr = dev->CreatePixelShader(out.data(), out.size(), nullptr, &patched);
            t_creating = false;
            if (FAILED(hr)) { char t[64]; _snprintf_s(t, _TRUNCATE, "CreatePixelShader 0x%08X", unsigned(hr)); why = t; patched.Reset(); }
        }
    }
    if (!patched) s.psFailed[ps] = why;
    s.patchedPs.emplace(ps, patched);
    return patched.Get();
}

// The derived blend state for the game's current one (null = the default).
ID3D11BlendState* derivedBlendFor(ID3D11DeviceContext* ctx, ID3D11BlendState* game, const char** refused) {
    for (const auto& b : g_blends)
        if (b.game.Get() == game) { *refused = b.refused; return b.derived.Get(); }
    if (g_blends.size() >= kBlendCap) g_blends.clear();   // g_bound holds its own references
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    BlendEntry entry;
    entry.game = game;
    entry.derived = engineVelocityCreateDerivedBlend(dev.Get(), game, &entry.refused);
    g_blends.push_back(entry);
    *refused = entry.refused;
    return entry.derived.Get();
}

// Put the game's own state back where EDVR's is still bound (the game has not
// rebound since: the generation says so).
void restore(ID3D11DeviceContext* ctx) {
    if (g_bound.patchedPs && bindingGeneration(BindSlot::Ps) == g_bound.psGen) {
        vScreenPSSetShaderRaw(ctx, g_bound.originalPs, nullptr, 0);
        ++g_draw.restores;
    }
    if (g_bound.patchedVs && bindingGeneration(BindSlot::Vs) == g_bound.vsGen) {
        vScreenVSSetShaderRaw(ctx, g_bound.originalVs, nullptr, 0);
        ++g_draw.restores;
    }
    if (g_bound.derivedBlend && bindingGeneration(BindSlot::Blend) == g_bound.blendGen) {
        vScreenOMSetBlendStateRaw(ctx, g_bound.gameBlend.Get(), g_bound.blendFactor, g_bound.sampleMask);
        ++g_draw.restores;
    }
    g_bound = Bound{};
    g_anyBound.store(false, std::memory_order_release);
    cache.family = -1;
}

// --- The watched sources --------------------------------------------------------
// Assign a watch slot to a snapshot's source. The same resource in the same
// slot keeps its epochs (the common case: one pool, one cb1, every frame); a
// new one starts over, with the rows another slot already knows for it.
WatchInfo& assignWatch(unsigned index, const ID3D11Resource* resource) {
    WatchInfo& w = g_watchInfo[index];
    if (w.resource != resource) {
        const WatchInfo* other = nullptr;
        for (const auto& o : g_watchInfo) if (&o != &w && o.resource == resource) { other = &o; break; }
        w = WatchInfo{};
        w.resource = resource;
        if (other && other->rowsKnown) { w.rowsKnown = true; std::memcpy(w.rows, other->rows, kRowsBytes); }
        watch[index].store(resource, std::memory_order_release);
    }
    return w;
}

void invalidate(Eye& e, Invalid why) {
    if (!e.invalid) {
        ++g_draw.invalid[why];
        if (&e == &g_eyes[kEngineVelocitySourceEye]) ++g_draw.sourceInvalid[why];
    }
    e.invalid = true;
}

// --- The eye pass --------------------------------------------------------------
bool ensureSlots(ID3D11DeviceContext* ctx, Eye& e, int eye, ID3D11Texture2D* depth) {
    D3D11_TEXTURE2D_DESC dd{};
    depth->GetDesc(&dd);
    // A single-sample, single-slice scene depth only: MRT6 must match the
    // pass's depth target exactly or the runtime drops the game's draw.
    if (dd.SampleDesc.Count != 1 || dd.ArraySize != 1) { ++g_draw.depthUnsupported; return false; }
    if (e.depth.Get() == depth && e.slots && e.width == dd.Width && e.height == dd.Height) return true;
    const void* wasDepth = e.depth.Get();
    const unsigned wasW = e.width, wasH = e.height;
    e.slots.Reset(); e.slotsRtv.Reset(); e.slotsSrv.Reset();
    e.depth = depth; e.width = dd.Width; e.height = dd.Height;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = dd.Width; d.Height = dd.Height; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R32G32_FLOAT;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &e.slots)) ||
        FAILED(dev->CreateRenderTargetView(e.slots.Get(), nullptr, &e.slotsRtv)) ||
        FAILED(dev->CreateShaderResourceView(e.slots.Get(), nullptr, &e.slotsSrv))) {
        e.slots.Reset(); e.slotsRtv.Reset(); e.slotsSrv.Reset(); e.depth.Reset();
        ++g_draw.createFailed;
        return false;
    }
    // When the slot target was made, for the flight's timeline (the depth
    // pair is re-created on boarding, and this follows it).
    if (eye == kEngineVelocitySourceEye)
        Log::get().note("engine motion: on-foot source slot target %s %ux%u R32G32 (%.1f MB) for the source depth %p at "
                        "present frame %u -- the 2D screen's scene is drawn there, not in the eyes, so its pool draws "
                        "record slot and depth here%s.", wasDepth ? "re-created" : "created", dd.Width, dd.Height,
                        double(dd.Width) * dd.Height * 8.0 / 1e6, static_cast<const void*>(depth),
                        frameNow(), wasDepth ? " (the source was re-made)" : "");
    else if (wasDepth)
        Log::get().note("engine motion: eye %d slot target re-created %ux%u for depth texture %p (was %ux%u for %p) at "
                        "present frame %u.", eye, dd.Width, dd.Height, static_cast<const void*>(depth), wasW, wasH,
                        wasDepth, frameNow());
    else
        Log::get().note("engine motion: eye %d slot target created %ux%u for depth texture %p at present frame %u.", eye,
                        dd.Width, dd.Height, static_cast<const void*>(depth), frameNow());
    return true;
}

// This eye-frame's pool and scene constants, exactly as its draws read them:
// VS t33 and b1 of the first substituted draw, copied on the GPU, and what
// the watch knows of them now.
bool snapshot(ID3D11DeviceContext* ctx, Eye& e, int eye, uint32_t frame) {
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    Ptr<ID3D11ShaderResourceView> poolView;
    ctx->VSGetShaderResources(kEngineVelocityPoolSlot, 1, &poolView);
    if (!poolView) { invalidate(e, kNoPool); return false; }
    // The slot the vertex shader indexes is relative to the view's first
    // element; the snapshot and the compose's view start at 0, so must this.
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    poolView->GetDesc(&vd);
    if (vd.ViewDimension != D3D11_SRV_DIMENSION_BUFFER || vd.Buffer.FirstElement != 0) { invalidate(e, kNoPool); return false; }
    Ptr<ID3D11Resource> poolRes;
    Ptr<ID3D11Buffer> poolBuf;
    poolView->GetResource(&poolRes);
    if (!poolRes || FAILED(poolRes.As(&poolBuf))) { invalidate(e, kNoPool); return false; }
    D3D11_BUFFER_DESC pd{};
    poolBuf->GetDesc(&pd);
    if (pd.StructureByteStride != emit::kItemBytes || !(pd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) ||
        pd.ByteWidth < emit::kItemBytes) { invalidate(e, kNoPool); return false; }
    Ptr<ID3D11Buffer> scene;
    ctx->VSGetConstantBuffers(kEngineVelocitySceneSlot, 1, &scene);
    if (!scene) { invalidate(e, kNoScene); return false; }
    D3D11_BUFFER_DESC sd{};
    scene->GetDesc(&sd);
    if (sd.ByteWidth < (kRowsFirst + 6u) * 16u || sd.ByteWidth > 65536u) { invalidate(e, kNoScene); return false; }
    // The quick path trusts the binding shadow's pointers for t33 and b1: it
    // must agree with the context here, or a bind went past the hooks.
    if (bindingGet(BindSlot::VsSrv33) != poolView.Get() || bindingGet(BindSlot::VsCb1) != scene.Get()) {
        invalidate(e, kShadow);
        return false;
    }
    if (!e.pool || e.poolBytes != pd.ByteWidth) {
        e.pool.Reset(); e.poolSrv.Reset();
        D3D11_BUFFER_DESC d = pd;
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateBuffer(&d, nullptr, &e.pool)) || FAILED(dev->CreateShaderResourceView(e.pool.Get(), nullptr, &e.poolSrv))) {
            e.pool.Reset(); e.poolSrv.Reset(); e.poolBytes = 0; ++g_draw.createFailed;
            invalidate(e, kCreate);
            return false;
        }
        e.poolBytes = pd.ByteWidth;
    }
    const unsigned slot = frame & 1u;
    if (!e.scene[slot] || e.sceneBytes != sd.ByteWidth) {
        for (auto& b : e.scene) b.Reset();
        e.sceneFrame[0] = e.sceneFrame[1] = ~0u;
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = sd.ByteWidth; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        for (auto& b : e.scene) if (FAILED(dev->CreateBuffer(&d, nullptr, &b))) {
            for (auto& c : e.scene) c.Reset();
            e.sceneBytes = 0; ++g_draw.createFailed;
            invalidate(e, kCreate);
            return false;
        }
        e.sceneBytes = sd.ByteWidth;
    }
    ctx->CopyResource(e.pool.Get(), poolBuf.Get());
    ctx->CopyResource(e.scene[slot].Get(), scene.Get());
    // What the snapshots copy (the performance review, item 4: measured
    // before any storage change): the whole pool buffer, whatever the view
    // exposes, and the scene constants.
    ++g_draw.poolSnapshots;
    g_draw.poolSnapshotBytes += pd.ByteWidth;
    ++g_draw.sceneSnapshots;
    g_draw.sceneSnapshotBytes += sd.ByteWidth;
    g_draw.poolCapacity = std::max<uint64_t>(g_draw.poolCapacity, pd.ByteWidth / emit::kItemBytes);
    g_draw.poolExposed = std::max<uint64_t>(g_draw.poolExposed, vd.Buffer.NumElements);
    e.sceneFrame[slot] = frame;
    e.poolView = poolView;
    e.poolBuffer = poolBuf;
    e.sceneBuffer = scene;
    e.poolFirst = vd.Buffer.FirstElement;
    e.poolCount = vd.Buffer.NumElements;
    const WatchInfo& wp = assignWatch(static_cast<unsigned>(eye) * 2u, poolBuf.Get());
    const WatchInfo& ws = assignWatch(static_cast<unsigned>(eye) * 2u + 1u, scene.Get());
    e.poolReplaceEpoch = wp.replaceEpoch;
    e.poolAppendEpoch = wp.appendEpoch;
    e.sceneWriteEpoch = ws.writeEpoch;
    e.sceneRowsKnown = ws.rowsKnown;
    std::memcpy(e.sceneRows, ws.rows, kRowsBytes);
    return true;
}

// A later substituted draw of the same eye-frame: its sources must be the
// snapshot's -- the same view (or one over the same buffer and elements), the
// same constant buffer, the pool not replaced (a NO_OVERWRITE append leaves
// every earlier record alone: the copy is refreshed), and registers 270..275
// unchanged since the snapshot (the game re-maps cb1 three to five times an
// eye pass, capture 043720, and those rows stay put; the other eye's rows go
// through the same buffer between the eyes' passes, which is why this is
// asked at the draw and not at the write).
void checkSources(ID3D11DeviceContext* ctx, Eye& e, int eye) {
    // The shadow's pointers were checked against the context at the snapshot,
    // and the snapshot holds both objects: an equal pointer is the same view
    // and buffer, with no call into the runtime. Only a different one is
    // looked at through the context.
    if (bindingGet(BindSlot::VsSrv33) != e.poolView.Get()) {
        Ptr<ID3D11ShaderResourceView> poolView;
        ctx->VSGetShaderResources(kEngineVelocityPoolSlot, 1, &poolView);
        if (poolView.Get() != e.poolView.Get()) {
            if (!poolView) { invalidate(e, kPoolRebound); return; }
            Ptr<ID3D11Resource> res;
            poolView->GetResource(&res);
            if (res.Get() != static_cast<ID3D11Resource*>(e.poolBuffer.Get())) { invalidate(e, kPoolRebound); return; }
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            poolView->GetDesc(&vd);
            if (vd.ViewDimension != D3D11_SRV_DIMENSION_BUFFER || vd.Buffer.FirstElement != e.poolFirst ||
                vd.Buffer.NumElements != e.poolCount) { invalidate(e, kPoolView); return; }
        }
    }
    if (bindingGet(BindSlot::VsCb1) != e.sceneBuffer.Get()) {
        Ptr<ID3D11Buffer> scene;
        ctx->VSGetConstantBuffers(kEngineVelocitySceneSlot, 1, &scene);
        if (scene.Get() != e.sceneBuffer.Get()) { invalidate(e, kSceneRebound); return; }
    }
    const WatchInfo& wp = g_watchInfo[static_cast<unsigned>(eye) * 2u];
    const WatchInfo& ws = g_watchInfo[static_cast<unsigned>(eye) * 2u + 1u];
    if (wp.replaceEpoch != e.poolReplaceEpoch) { invalidate(e, kPoolRewritten); return; }
    if (wp.appendEpoch != e.poolAppendEpoch) {
        const int refreshTimer = beginCapture(ctx, kCaptureRefresh);
        ctx->CopyResource(e.pool.Get(), e.poolBuffer.Get());
        endCapture(ctx, refreshTimer);
        e.poolAppendEpoch = wp.appendEpoch;
        ++g_draw.poolRefreshed;
        g_draw.poolRefreshBytes += e.poolBytes;
    }
    if (ws.writeEpoch != e.sceneWriteEpoch) {
        if (!ws.rowsKnown || !e.sceneRowsKnown) { invalidate(e, kSceneUnknown); return; }
        if (std::memcmp(ws.rows, e.sceneRows, kRowsBytes) != 0) { invalidate(e, kSceneRows); return; }
        e.sceneWriteEpoch = ws.writeEpoch;
        ++g_draw.sceneRowsKept;
    }
}

// The source camera rule (flight 5, 2026-09-23 140351). The eyes' rule above
// holds an eye-frame to its first draw's rows; the on-foot source is drawn by
// more than one camera -- every source frame of that flight's walk was dropped
// after two runs of vs_AACF draws, the first substituted, whose rows 270..275
// matched the world's standing still and not walking -- so a source pool draw
// is held to the NAMING's camera instead (engineVelocityNoteSource: the
// scene constants the terrain/scene draw read this present frame, the camera
// screen_motion's own camera term uses). A draw before this frame's naming,
// under other scene constants, with rows the watch has not seen, or under
// other rows is declined -- not substituted, the frame and its other draws
// kept -- and counted; the frame's snapshot is then taken at its first draw
// under the naming's camera, so SEN is the world's rows by construction.
bool sourceCameraHolds(ID3D11DeviceContext* ctx, int f, uint32_t frame) {
    const SourceCamera& c = g_sourceCamera;
    int why = -1;
    if (c.frame != frame) why = kBeforeNaming;
    else if (!c.scene || !c.rowsKnown) why = kNamingUnseen;
    else if (bindingGet(BindSlot::VsCb1) != c.scene.Get()) {
        // The shadow first; a different pointer is asked of the context.
        Ptr<ID3D11Buffer> scene;
        ctx->VSGetConstantBuffers(kEngineVelocitySceneSlot, 1, &scene);
        if (scene.Get() != c.scene.Get()) why = kOtherScene;
    }
    if (why < 0) {
        const WatchInfo& ws = g_watchInfo[static_cast<unsigned>(kEngineVelocitySourceEye) * 2u + 1u];
        if (ws.resource != static_cast<const ID3D11Resource*>(c.scene.Get()) || !ws.rowsKnown) why = kRowsUnseen;
        else if (std::memcmp(ws.rows, c.rows, kRowsBytes) != 0) {
            why = kOtherCamera;
            ++g_draw.sourceDeclinedFamily[f];
            // Which rows the other camera changed: 270..272 (the clip rows'
            // xyz: rotation and projection), 273 (the near plane and the
            // jitter), 274 (the view axis), 275 (the camera position).
            float a[24], b[24];
            std::memcpy(a, ws.rows, kRowsBytes);
            std::memcpy(b, c.rows, kRowsBytes);
            if (std::memcmp(a, b, 48) != 0) ++g_draw.sourceOtherRows[0];
            if (std::memcmp(a + 12, b + 12, 16) != 0) ++g_draw.sourceOtherRows[1];
            if (std::memcmp(a + 16, b + 16, 16) != 0) ++g_draw.sourceOtherRows[2];
            if (std::memcmp(a + 20, b + 20, 16) != 0) ++g_draw.sourceOtherRows[3];
            const double dx = double(a[20]) - b[20], dy = double(a[21]) - b[21], dz = double(a[22]) - b[22];
            const double shift = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (std::isfinite(shift) && shift > g_draw.sourceOtherShiftMax) g_draw.sourceOtherShiftMax = shift;
        }
    }
    if (why < 0) { ++g_draw.sourceHeld; return true; }
    ++g_draw.sourceDeclined[why];
    if (g_sourceDeclineFrame != frame) { g_sourceDeclineFrame = frame; ++g_draw.sourceDeclineFrames; }
    return false;
}

// Add MRT6 to the game's binding, once per pass binding, only where the
// binding can take it (engine_velocity_state.h), and only if the runtime
// kept it. False: not here.
bool bindTarget(ID3D11DeviceContext* ctx, Eye& e, ID3D11DepthStencilView* dsv) {
    ID3D11RenderTargetView* rt[8] = {};
    ID3D11DepthStencilView* bound = nullptr;
    ctx->OMGetRenderTargets(8, rt, &bound);
    std::array<Ptr<ID3D11RenderTargetView>, 8> held;
    for (unsigned i = 0; i < 8; ++i) held[i].Attach(rt[i]);
    Ptr<ID3D11DepthStencilView> heldDsv;
    heldDsv.Attach(bound);
    if (bound != dsv) return false;
    if (rt[kEngineVelocityTarget] && rt[kEngineVelocityTarget] != e.slotsRtv.Get()) { ++g_draw.targetOccupied; return false; }
    ID3D11UnorderedAccessView* uav[8] = {};
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, uav);
    bool anyUav = false;
    for (auto* u : uav) if (u) { anyUav = true; u->Release(); }
    if (anyUav) { ++g_draw.uavBound; return false; }
    if (rt[kEngineVelocityTarget] == e.slotsRtv.Get()) return true;
    const EngineVelocityBindRefusal refusal = engineVelocityValidateTargets(rt, dsv, e.width, e.height);
    if (refusal != EngineVelocityBindRefusal::None) { ++g_draw.bindRefused[static_cast<int>(refusal)]; return false; }
    rt[kEngineVelocityTarget] = e.slotsRtv.Get();
    // All eight slots: whatever the game has at 7 stays bound.
    vScreenSetRenderTargetsRaw(ctx, 8, rt, dsv);
    // The runtime drops a set it cannot take; then the game's own goes back.
    ID3D11RenderTargetView* now[8] = {};
    ID3D11DepthStencilView* nowDsv = nullptr;
    ctx->OMGetRenderTargets(8, now, &nowDsv);
    bool kept = nowDsv == dsv;
    for (unsigned i = 0; i < 8; ++i) {
        if (now[i] != rt[i]) kept = false;
        if (now[i]) now[i]->Release();
    }
    if (nowDsv) nowDsv->Release();
    if (!kept) {
        ++g_draw.bindRejected;
        rt[kEngineVelocityTarget] = nullptr;
        vScreenSetRenderTargetsRaw(ctx, 8, rt, dsv);
        return false;
    }
    return true;
}

void slowPath(ID3D11DeviceContext* ctx, bool rtv0Eye) {
    ++g_draw.slowPaths;
    auto* vs = static_cast<ID3D11VertexShader*>(bindingGet(BindSlot::Vs));
    auto* ps = static_cast<ID3D11PixelShader*>(bindingGet(BindSlot::Ps));
    auto* dsv = static_cast<ID3D11DepthStencilView*>(bindingGet(BindSlot::Dsv0));
    const uint64_t vsHash = bindingShaderHash(BindSlot::Vs);
    const uint64_t psHash = bindingShaderHash(BindSlot::Ps);
    int eye = -1, target = -1;
    const bool eyePass = rtv0Eye && dsv && depthProbeCurrentSceneEyeOf(dsv, &eye, &target) && (eye == 0 || eye == 1);
    const int f = familyForProfile(vsHash, runtimeFlatProfile());
    // On foot no pool draw targets an eye: the source pass is a pool family
    // draw into the depth screen_motion named this frame or the last two.
    bool sourcePass = false;
    if (!eyePass && f >= 0 && dsv && g_sourceDepth && frameNow() - g_sourceNoted <= 2u) {
        Ptr<ID3D11Resource> depthRes;
        dsv->GetResource(&depthRes);
        sourcePass = depthRes.Get() == static_cast<ID3D11Resource*>(g_sourceDepth.Get());
        if (sourcePass) eye = kEngineVelocitySourceEye;
    }
    auto vsInfo = vs ? g_vs.find(vs) : g_vs.end();
    if (!g_emitLive.load(std::memory_order_acquire) || !(eyePass || sourcePass) || f < 0 || vsInfo == g_vs.end() ||
        vsInfo->second.family != f) { restore(ctx); return; }
    deriveFamily(f);
    FamilyState& fam = g_families[f];
    if (!fam.valid) { restore(ctx); return; }
    Eye& e = g_eyes[eye];
    const uint32_t frame = frameNow();
    // An eye-frame with a recognised pool family draw: the old order prepared
    // on this alone (the 2026-09-23 performance review, item 2).
    if (e.seenFrame != frame) { e.seenFrame = frame; ++g_draw.eyeFramesSeen; }
    // Eligibility FIRST: a keyed pixel shader whose patch exists, and the
    // vertex patch where the family needs one. Nothing -- slot target, clear,
    // snapshot, MRT6 -- is prepared for a draw that cannot export ownership;
    // a declined draw puts the game's state back, as before.
    if (!keyedPs(f, psHash)) {
        if (!anyKeyedPs(psHash)) { ++fam.unkeyedPsDraws; fam.unkeyedPsHash = psHash; }
        restore(ctx);
        return;
    }
    ID3D11VertexShader* useVs = vs;
    if (fam.inputs.slotFromVsPatch) {
        useVs = patchedVsFor(ctx, f, vs);
        if (!useVs) { restore(ctx); return; }
    }
    ID3D11PixelShader* usePs = patchedPsFor(ctx, f, ps);
    if (!usePs) { restore(ctx); return; }
    // The source's camera, draw by draw, before anything is prepared: the
    // frame starts at its first draw under the naming's camera.
    if (sourcePass && !sourceCameraHolds(ctx, f, frame)) { restore(ctx); return; }
    const bool newFrame = e.frame != frame;
    Ptr<ID3D11Texture2D> depthTex;
    if (newFrame || e.bindingStale || e.rtvGen != cache.rtv || e.dsvGen != cache.dsv) {
        Ptr<ID3D11Resource> depthRes;
        dsv->GetResource(&depthRes);
        if (!depthRes || FAILED(depthRes.As(&depthTex))) { restore(ctx); return; }
    }
    if (newFrame) {
        // The eye-frame: this depth's slot target, cleared, and the sources copied.
        if (!ensureSlots(ctx, e, eye, depthTex.Get())) { restore(ctx); return; }
        ++g_draw.eyeFrames;
        if (sourcePass) ++g_draw.sourceFrames;
        e.frame = frame;
        e.bound = e.boundCounted = e.written = e.invalid = e.consumed = false;
        e.rtvGen = e.dsvGen = 0;
        e.bindingStale = false;
        const float cleared[4] = {-1.0f, 0.0f, 0.0f, 0.0f};
        const int clearTimer = beginCapture(ctx, kCaptureClear);
        ctx->ClearRenderTargetView(e.slotsRtv.Get(), cleared);
        endCapture(ctx, clearTimer);
        const int snapTimer = beginCapture(ctx, kCaptureSnapshot);
        snapshot(ctx, e, eye, frame);
        endCapture(ctx, snapTimer);
    }
    if (e.invalid) { restore(ctx); return; }
    if (e.bindingStale || e.rtvGen != cache.rtv || e.dsvGen != cache.dsv) {
        // Another pass binding of the same eye-frame must draw into the same
        // depth the slot target was made for.
        if (!newFrame && depthTex.Get() != e.depth.Get()) { invalidate(e, kDepthChanged); restore(ctx); return; }
        e.rtvGen = cache.rtv;
        e.dsvGen = cache.dsv;
        e.bound = bindTarget(ctx, e, dsv);
        e.bindingStale = false;
        if (e.bound && !e.boundCounted) {
            e.boundCounted = true;
            ++g_draw.eyeFramesBound;
            if (sourcePass) ++g_draw.sourceFramesBound;
        }
    }
    if (!e.bound) { restore(ctx); return; }
    // A draw that will write MRT6 after the eye-frame's first must read what
    // the snapshot holds (the first one's sources ARE the snapshot).
    if (!newFrame) {
        checkSources(ctx, e, eye);
        if (e.invalid) { restore(ctx); return; }
    }
    // The blend state MRT6 must not inherit: the derived one, unless it is
    // still bound (the game has not set another since).
    if (!(g_bound.derivedBlend && bindingGeneration(BindSlot::Blend) == g_bound.blendGen)) {
        Ptr<ID3D11BlendState> game;
        float factor[4] = {};
        UINT mask = 0;
        ctx->OMGetBlendState(&game, factor, &mask);
        if (game.Get() != bindingGet(BindSlot::Blend)) ++g_draw.blendShadowDisagreed;
        const char* refused = nullptr;
        ID3D11BlendState* derived = derivedBlendFor(ctx, game.Get(), &refused);
        if (!derived) {
            ++g_draw.blendRefused;
            g_draw.blendRefusedWhy = refused;
            restore(ctx);
            return;
        }
        vScreenOMSetBlendStateRaw(ctx, derived, factor, mask);
        g_bound.gameBlend = game;
        g_bound.derivedBlend = derived;
        std::memcpy(g_bound.blendFactor, factor, sizeof(factor));
        g_bound.sampleMask = mask;
        g_bound.blendGen = bindingGeneration(BindSlot::Blend);
        ++g_draw.blendApplied;
    }
    // The setters only where the patched shader is not still installed (the
    // performance review, item 3): a visit that only re-verified the sources
    // -- a cb1 re-map, a pool append, a blend change -- finds ours bound when
    // the game has set nothing in that stage since (same original, same
    // binding generation). Source checks and the blend stay independent.
    const bool psInstalled = g_bound.patchedPs == usePs && g_bound.originalPs == ps &&
                             bindingGeneration(BindSlot::Ps) == g_bound.psGen;
    const bool vsInstalled = g_bound.patchedVs == useVs && g_bound.originalVs == vs &&
                             bindingGeneration(BindSlot::Vs) == g_bound.vsGen;
    if (useVs != vs) {
        if (vsInstalled) ++g_draw.settersSkipped;
        else { vScreenVSSetShaderRaw(ctx, useVs, nullptr, 0); ++g_draw.settersIssued; }
    }
    if (psInstalled) ++g_draw.settersSkipped;
    else { vScreenPSSetShaderRaw(ctx, usePs, nullptr, 0); ++g_draw.settersIssued; }
    g_bound.originalPs = ps; g_bound.patchedPs = usePs; g_bound.psGen = cache.ps;
    g_bound.originalVs = useVs != vs ? vs : nullptr; g_bound.patchedVs = useVs != vs ? useVs : nullptr; g_bound.vsGen = cache.vs;
    g_bound.family = f;
    g_anyBound.store(true, std::memory_order_release);
    cache.family = f;
    ++fam.binds;
    // The eye is usable only now: a draw that exports ownership is about to
    // be issued (MRT6 bound alone once counted as written).
    e.written = true;
    if (e.substFrame != frame) { e.substFrame = frame; ++g_draw.eyeFramesSubstituted; }
    // When substitution starts again after a quiet stretch, for the flight's
    // timeline (boarding, a scene change).
    if (g_lastSubstitution == ~0u || frame - g_lastSubstitution > kResumeFrames) {
        char quiet[48];
        if (g_lastSubstitution == ~0u) _snprintf_s(quiet, _TRUNCATE, "the first this session");
        else _snprintf_s(quiet, _TRUNCATE, "%u frames without one", frame - g_lastSubstitution);
        char where[24];
        if (eye == kEngineVelocitySourceEye) _snprintf_s(where, _TRUNCATE, "on-foot source");
        else _snprintf_s(where, _TRUNCATE, "eye %d", eye);
        Log::get().note("engine motion: substitution starts at present frame %u (%s, %s, ps_%016llX), %s.", frame, where,
                        kFamilies[f].name, static_cast<unsigned long long>(psHash), quiet);
    }
    g_lastSubstitution = frame;
}

std::string hex64(uint64_t v) { char t[24]; _snprintf_s(t, _TRUNCATE, "%016llX", static_cast<unsigned long long>(v)); return t; }

void summaryLocked(uint64_t now) {
    const double seconds = std::max(1.0, double(now - g_windowStartMs) / 1000.0);
    const double frames = std::max<double>(1.0, double(g_draw.frames));
    const auto r = [](const std::atomic<uint64_t>& c) { return static_cast<unsigned long long>(c.load(std::memory_order_relaxed)); };
    const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    const uint64_t sampled = g_emitSampled.exchange(0), sampledTicks = g_emitSampledTicks.exchange(0);
    const double freq = double(std::max<int64_t>(1, qpcFrequency()));
    const double emitUs = sampled ? double(sampledTicks) * 1e6 / freq / double(sampled) : 0.0;
    const double emitMsPerFrame = emitUs * double(g_emit.calls.load()) / frames / 1000.0;
    if (!g_emitLive.load(std::memory_order_acquire)) logStoodDown();
    Log::get().note("engine motion: emit (%s) over %.0f s, %.0f frames: FUN_144312E00 calls %llu (%llu appended, %llu pool "
                    "records in all); pool records joined %llu (with motion %llu), masked %llu; masked for: first seen %llu, "
                    "gap %llu, reused pointer %llu, pose changed within one frame %llu, previous frame not certified %llu, "
                    "this call unproven %llu, table full %llu; same-frame repeats %llu; pose disagreements %llu (must be 0), "
                    "locate failures %llu, read faults %llu, write faults %llu, tainted %llu, drained %llu, over 7 %llu; "
                    "bracket %.2f us/call sampled, ~%.3f ms/frame on the job threads; table %u live records.",
                    g_verifyWhy ? g_verifyWhy : g_hookWhy ? g_hookWhy : "live", seconds, double(g_draw.frames),
                    r(g_emit.calls), r(g_emit.callsWithItems), r(g_emit.itemsAppended), r(g_emit.itemsJoined),
                    r(g_emit.itemsMoving), r(g_emit.itemsMasked), r(g_emit.firstSeen), r(g_emit.gaps),
                    r(g_emit.identityResets), r(g_emit.sameFrameChanges), r(g_emit.uncertifiedHistory),
                    r(g_emit.callsUnproven), r(g_emit.overflow), r(g_emit.repeats), r(g_emit.disagreements),
                    r(g_emit.locateFailures), r(g_emit.readFaults), r(g_emit.writeFaults), r(g_emit.taints),
                    r(g_emit.drained), r(g_emit.tooMany), emitUs, emitMsPerFrame, g_table ? g_table->live(frameNow()) : 0u);
    Log::get().note("engine motion: history gaps %llu by age in frames (2: %llu, 3-4: %llu, 5-8: %llu, 9-64: %llu, over 64: "
                    "%llu), %llu of them in %llu burst frames (%llu or more in one frame); census of one record in eight "
                    "by address: %llu record-frames evaluated, moving since their last %llu drawn + %llu evaluated but not "
                    "drawn (~%.1f + %.1f records/frame scaled by 8); census gaps: %llu evaluated without items in between "
                    "(culled or not selected), %llu not evaluated at all%s.",
                    r(g_emit.gaps), r(g_emit.gapAge[0]), r(g_emit.gapAge[1]), r(g_emit.gapAge[2]), r(g_emit.gapAge[3]),
                    r(g_emit.gapAge[4]), u(g_draw.burstGaps), u(g_draw.burstFrames), u(kBurstGaps), r(g_emit.censusFrames),
                    r(g_emit.censusMovingDrawn), r(g_emit.censusMovingUndrawn),
                    8.0 * double(g_emit.censusMovingDrawn.load()) / frames,
                    8.0 * double(g_emit.censusMovingUndrawn.load()) / frames, r(g_emit.gapsEvaluatedBetween),
                    r(g_emit.gapsUnevaluated),
                    g_diagnosticsWanted.load(std::memory_order_relaxed)
                        ? "" : " (the census is off: it runs only with engine motion's diagnostics -- "
                               "advanced.temporal_aa_diagnostics or an eye run; these zeros are not counts)");
    uint64_t invalid = 0;
    for (uint64_t v : g_draw.invalid) invalid += v;
    std::string invalidText, refusedText;
    for (int i = 0; i < kInvalidCount; ++i) {
        char t[96];
        _snprintf_s(t, _TRUNCATE, "%s%s %llu", i ? ", " : "", kInvalidNames[i], u(g_draw.invalid[i]));
        invalidText += t;
    }
    for (int i = 1; i < static_cast<int>(EngineVelocityBindRefusal::Count); ++i) {
        char t[128];
        _snprintf_s(t, _TRUNCATE, "%s%s %llu", i > 1 ? ", " : "",
                    engineVelocityBindRefusalName(static_cast<EngineVelocityBindRefusal>(i)), u(g_draw.bindRefused[i]));
        refusedText += t;
    }
    Log::get().note("engine motion: movers joined %.1f records/frame (moving rig records the emit wrote a previous pose for); "
                    "eye-frames %llu, with MRT6 bound %llu (prepared only for an eligible draw: %llu eye-frames "
                    "had a pool family draw, %llu a substitution; prepared for nothing %llu, under the old order %llu); "
                    "invalidated "
                    "%llu (%s); kept: scene constants re-mapped with rows 270..275 unchanged %llu, pool appended and "
                    "refreshed %llu; MRT6 refused: target 6 occupied %llu, UAV bound %llu, %s, runtime rejected the set "
                    "%llu; depth not single-sample %llu, slot target create failed %llu; blend: derived state bound %llu "
                    "times, refused %llu%s%s%s, shadow disagreed %llu; views asked %llu, given %llu, refused: stood down "
                    "%llu, other depth %llu, other frame %llu, invalidated %llu, unwritten %llu, no previous scene "
                    "constants %llu; draw hook slow half %llu calls, %.2f us each, ~%.3f ms/frame on the caller thread "
                    "(plus %llu lock-free looks); restores %llu; shader setters issued %llu, skipped %llu (ours still "
                    "bound).",
                    double(g_emit.recordsMoving.load()) / frames,
                    u(g_draw.eyeFrames), u(g_draw.eyeFramesBound), u(g_draw.eyeFramesSeen), u(g_draw.eyeFramesSubstituted),
                    u(g_draw.eyeFrames > g_draw.eyeFramesSubstituted ? g_draw.eyeFrames - g_draw.eyeFramesSubstituted : 0),
                    u(g_draw.eyeFramesSeen > g_draw.eyeFramesSubstituted ? g_draw.eyeFramesSeen - g_draw.eyeFramesSubstituted : 0),
                    u(invalid), invalidText.c_str(),
                    u(g_draw.sceneRowsKept), u(g_draw.poolRefreshed), u(g_draw.targetOccupied), u(g_draw.uavBound),
                    refusedText.c_str(), u(g_draw.bindRejected), u(g_draw.depthUnsupported), u(g_draw.createFailed),
                    u(g_draw.blendApplied), u(g_draw.blendRefused), g_draw.blendRefusedWhy ? " (" : "",
                    g_draw.blendRefusedWhy ? g_draw.blendRefusedWhy : "", g_draw.blendRefusedWhy ? ")" : "",
                    u(g_draw.blendShadowDisagreed), u(g_draw.viewsAsked), u(g_draw.viewsGiven), u(g_draw.refusedNoEmit),
                    u(g_draw.refusedDepth), u(g_draw.refusedFrame), u(g_draw.refusedInvalid), u(g_draw.refusedUnwritten),
                    u(g_draw.refusedPrevious), u(g_draw.slowPaths),
                    g_draw.slowPaths ? double(g_draw.slowTicks) * 1e6 / freq / double(g_draw.slowPaths) : 0.0,
                    double(g_draw.slowTicks) * 1e3 / freq / frames, u(g_draw.quickPaths), u(g_draw.restores),
                    u(g_draw.settersIssued), u(g_draw.settersSkipped));
    // The snapshots' copy traffic (item 4, measured only): logical bytes
    // submitted, not GPU time -- a CopyResource's CPU submission prices
    // nothing on the GPU.
    Log::get().note("engine motion: snapshots (measure only): pool capacity %llu records (%.1f MB), views expose %llu; "
                    "copies: pool %llu at preparation (%.1f MB) + %llu on append refreshes (%.1f MB), scene constants "
                    "%llu (%.1f KB); ~%.2f MB a frame logical.",
                    u(g_draw.poolCapacity), double(g_draw.poolCapacity) * emit::kItemBytes / 1e6, u(g_draw.poolExposed),
                    u(g_draw.poolSnapshots), double(g_draw.poolSnapshotBytes) / 1e6, u(g_draw.poolRefreshed),
                    double(g_draw.poolRefreshBytes) / 1e6, u(g_draw.sceneSnapshots), double(g_draw.sceneSnapshotBytes) / 1e3,
                    double(g_draw.poolSnapshotBytes + g_draw.poolRefreshBytes + g_draw.sceneSnapshotBytes) / 1e6 / frames);
    if (g_draw.pixelReads)
        Log::get().note("engine motion: pixels per eye-frame on the trained path: engine-joined %.0f (a rig record's "
                        "certified exact motion, moving or still -- not a mover count), masked %.0f "
                        "(no history), pool surface not a rig record %.0f (camera term), stale slot %.0f (the slot's "
                        "recorded depth is not the pixel's), corrupt slot code %.0f (declined; must be 0); %llu readbacks.",
                        double(g_draw.pixelsJoined) / double(g_draw.pixelReads), double(g_draw.pixelsMasked) / double(g_draw.pixelReads),
                        double(g_draw.pixelsCamera) / double(g_draw.pixelReads), double(g_draw.pixelsStale) / double(g_draw.pixelReads),
                        double(g_draw.pixelsCorrupt) / double(g_draw.pixelReads), u(g_draw.pixelReads));
    else
        Log::get().note("engine motion: pixels: not counted this window -- the counts come from the instrumented DLSS/FSR "
                        "motion shader only (advanced.temporal_aa_diagnostics = 1, or a debug view) with the engine inputs "
                        "bound; this is not a zero count.");
    // On foot: the source pass and the screen shader that carries its
    // pixels through the panel (docs/kinematic-motion-injection-2026-09-19.md,
    // 2026-09-23 "On foot"). Printed whenever the source was about.
    const Eye& source = g_eyes[kEngineVelocitySourceEye];
    if (g_draw.sourceFrames || g_draw.sourceViewsAsked || g_draw.panelDraws[0] || g_draw.panelDraws[1] ||
        g_draw.sourceNamings || source.slots) {
        // The source's own invalidations by reason, then the camera rule's
        // declines by reason and another camera's by family and rows.
        std::string dropped, declined, families;
        const auto add = [](std::string& s, const char* name, uint64_t n) {
            char t[96];
            _snprintf_s(t, _TRUNCATE, "%s%s %llu", s.empty() ? "" : ", ", name, static_cast<unsigned long long>(n));
            s += t;
        };
        for (int k = 0; k < kInvalidCount; ++k) if (g_draw.sourceInvalid[k]) add(dropped, kInvalidNames[k], g_draw.sourceInvalid[k]);
        uint64_t declinedAll = 0;
        for (int k = 0; k < kSourceDeclineCount; ++k) {
            declinedAll += g_draw.sourceDeclined[k];
            add(declined, kSourceDeclineNames[k], g_draw.sourceDeclined[k]);
        }
        for (int f = 0; f < kFamilyCount; ++f)
            if (g_draw.sourceDeclinedFamily[f]) add(families, kFamilies[f].name, g_draw.sourceDeclinedFamily[f]);
        char other[768] = "";
        if (g_draw.sourceDeclined[kOtherCamera])
            _snprintf_s(other, _TRUNCATE, "; another camera changed rows 270..272 on %llu, 273 on %llu, 274 on %llu, 275 "
                        "on %llu, its position up to %.3f m from the naming's, by family: %s",
                        u(g_draw.sourceOtherRows[0]), u(g_draw.sourceOtherRows[1]), u(g_draw.sourceOtherRows[2]),
                        u(g_draw.sourceOtherRows[3]), g_draw.sourceOtherShiftMax, families.c_str());
        // The panel's kinds: every pixel (diagnostics, motion_source) when
        // counted, else the sampled count, else nothing.
        const int mode = g_draw.panelDraws[0] ? 0 : g_draw.panelDraws[1] ? 1 : -1;
        char pixels[640];
        if (mode >= 0) {
            const double draws = double(g_draw.panelDraws[mode]);
            const uint64_t* p = g_draw.panel[mode];
            char sampleNote[160] = "";
            if (mode == 1)
                _snprintf_s(sampleNote, _TRUNCATE, " (sampled: one frame in %u, one eye pixel in %u on a %ux%u grid; raw "
                            "counts, not scaled)", kPanelSampleFrames, kPanelSampleStride * kPanelSampleStride,
                            kPanelSampleStride, kPanelSampleStride);
            _snprintf_s(pixels, _TRUNCATE, "panel pixels per %s eye draw: engine-joined %.0f (a rig record's certified "
                        "exact motion, carried through the panel), masked %.0f (no history), pool surface not a rig "
                        "record %.0f (camera term), stale slot %.0f, corrupt slot code %.0f (declined; must be 0) over "
                        "%llu counted eye draws%s", mode ? "sampled" : "counted", double(p[0]) / draws,
                        double(p[1]) / draws, double(p[2]) / draws, double(p[3]) / draws, double(p[4]) / draws,
                        u(g_draw.panelDraws[mode]), sampleNote);
        } else {
            _snprintf_s(pixels, _TRUNCATE, "panel pixels: none counted this window (sampled one frame in %u while the "
                        "source's views are given; every pixel with advanced.temporal_aa_diagnostics = 1 or the "
                        "motion_source view; not a zero count)", kPanelSampleFrames);
        }
        Log::get().note("engine motion: on foot: source frames %llu, with MRT6 bound %llu (slot target %ux%u), "
                        "frames dropped: %s; screen views asked %llu, given %llu, refused: stood down %llu, other depth "
                        "%llu, other frame %llu, invalidated %llu, unwritten %llu, no previous scene constants %llu; "
                        "camera rule: namings %llu (by terrain or a scene draw %llu, by the screen's own depth %llu; rows "
                        "not seen %llu), checks held to the naming's camera %llu, "
                        "declined %llu in %llu frames (%s)%s; %s.",
                        u(g_draw.sourceFrames), u(g_draw.sourceFramesBound), source.width, source.height,
                        dropped.empty() ? "none" : dropped.c_str(),
                        u(g_draw.sourceViewsAsked), u(g_draw.sourceViewsGiven), u(g_draw.sourceRefusedNoEmit),
                        u(g_draw.sourceRefusedDepth), u(g_draw.sourceRefusedFrame), u(g_draw.sourceRefusedInvalid),
                        u(g_draw.sourceRefusedUnwritten), u(g_draw.sourceRefusedPrevious), u(g_draw.sourceNamings),
                        u(g_draw.sourceNamingsBy[0]), u(g_draw.sourceNamingsBy[1]),
                        u(g_draw.sourceNamingsUnseen), u(g_draw.sourceHeld), u(declinedAll),
                        u(g_draw.sourceDeclineFrames), declined.c_str(), other, pixels);
    }
    for (int f = 0; f < kFamilyCount; ++f) {
        FamilyState& s = g_families[f];
        std::string patched, failed;
        for (auto& [ptr, shader] : s.patchedPs) {
            auto info = g_ps.find(ptr);
            const std::string h = info != g_ps.end() ? "ps_" + hex64(info->second.hash) : "ps_?";
            if (shader) { if (!patched.empty()) patched += ","; patched += h; }
            else { if (!failed.empty()) failed += "; "; failed += h + " (" + s.psFailed[ptr] + ")"; }
        }
        const bool seen = std::any_of(g_vs.begin(), g_vs.end(), [&](const auto& v) { return v.second.family == f; });
        const char* state = !seen ? "not created by the game this session" : !s.derived ? "not drawn yet"
                          : s.valid ? "live" : "STOOD DOWN";
        Log::get().note("engine motion: family %s: %s%s%s; substituted %llu binds, %llu draws; patched [%s]%s%s%s%s.",
                        kFamilies[f].name, state, s.reason.empty() ? "" : " -- ", s.reason.c_str(),
                        u(s.binds), u(familyDraws[f]), patched.c_str(),
                        failed.empty() ? "" : "; refused [", failed.c_str(), failed.empty() ? "" : "]",
                        s.unkeyedPsDraws ? (" ; unkeyed pixel shader ps_" + hex64(s.unkeyedPsHash) + " left stock").c_str() : "");
        s.binds = 0;
        s.unkeyedPsDraws = 0;
        familyDraws[f] = 0;
    }
    g_emit.clear();
    g_lastGaps = 0;
    g_draw.clear();
    g_windowStartMs = now;
}

void clearLocked() {
    for (auto& e : g_eyes) e = Eye{};
    g_sourceDepth.Reset();
    g_sourceNoted = ~0u;
    g_sourceCamera = SourceCamera{};
    g_sourceDeclineFrame = ~0u;
    for (auto& w : watch) w.store(nullptr);
    for (auto& w : g_watchInfo) w = WatchInfo{};
    for (auto& s : g_families) {
        s.patchedVs.clear(); s.patchedPs.clear(); s.psFailed.clear();
        s.derived = s.valid = false; s.reason.clear(); s.binds = 0; s.unkeyedPsDraws = 0;
    }
    for (auto& d : familyDraws) d = 0;
    // g_bound stays: only the owner thread may put the game's state back
    // (engineVelocityFrameBoundary, g_anyBound says it is owed); it holds its
    // own references to the blend states, so the cache can go.
    g_blends.clear();
    cache = DrawCache{};
    if (g_table) g_table->clear();
    if (g_census) g_census->clear();
    g_emit.clear();
    g_lastGaps = 0;
    g_lastSubstitution = ~0u;
    g_draw.clear();
}

} // namespace engine_velocity_detail

using namespace engine_velocity_detail;

bool engineVelocityActive() noexcept { return live.load(std::memory_order_acquire); }

namespace engine_velocity_detail {
// The emit's want on the shared hook set; quiet on a retry.
void attachEmitLocked(bool quiet) {
    const char* result = kinematicEvalEmitAttach();
    g_emitAttached = std::strcmp(result, "installed") == 0;
    if (!g_emitAttached && !quiet)
        Log::get().note("engine motion: the kinematic eval hook set refused the emit (%s) -- engine-record velocity "
                        "stands down, the stock motion path is untouched; retried quietly at later config polls.",
                        result);
}
}  // namespace engine_velocity_detail

void engineVelocityConfigure(bool on) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (on && live.load(std::memory_order_acquire) && !g_emitAttached) { attachEmitLocked(true); return; }
    if (on == live.load(std::memory_order_acquire)) return;
    if (!on) { engineVelocityShutdown(); return; }
    // The emit bracket rides the shared eval hooks, held open by its own
    // want; the bracket itself needs the lookup verified.
    attachEmitLocked(false);
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_verifyWhy = verifyEngine(base);
    if (!g_table) g_table = std::make_unique<emit::Table>();
    if (!g_census) g_census = std::make_unique<emit::Census>();
    clearLocked();
    if (g_verifyWhy) {
        g_lookup.store(nullptr, std::memory_order_release);
        kinematicEvalSetEmitObserver(nullptr);
    } else {
        g_lookup.store(reinterpret_cast<emit::LookupFn>(base + kLookupRva), std::memory_order_release);
        kinematicEvalSetEmitObserver(&observeEmit);
    }
    refreshEmitStatus(true);
    g_windowStartMs = nowMs();
    live.store(true, std::memory_order_release);
    if (!g_emitLive.load(std::memory_order_acquire)) { logStoodDown(); return; }
    Log::get().note("engine motion: engine-record velocity live (with fix.temporal_aa): FUN_144312E00's records carry "
                    "their previous engine pose when it is continuous and proven; the pool families' own draws write the "
                    "pool slot and depth at MRT6 under an unblended state; the temporal pass takes exact record motion "
                    "there, masks rig records it cannot follow, and keeps the camera term elsewhere. Per-30 s lines "
                    "below; every zero is printed, not omitted.");
}

void engineVelocityDiagnostics(bool on) { g_diagnosticsWanted.store(on, std::memory_order_relaxed); }

void engineVelocityShutdown() {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const bool was = live.exchange(false, std::memory_order_acq_rel);
    kinematicEvalSetEmitObserver(nullptr);
    if (g_emitAttached) { kinematicEvalEmitDetach(); g_emitAttached = false; }
    g_lookup.store(nullptr, std::memory_order_release);
    g_emitLive.store(false, std::memory_order_release);
    clearLocked();
    if (was) Log::get().note("engine motion: engine-record velocity stood down, state cleared.");
}

void engineVelocityRememberVs(ID3D11VertexShader* shader, uint64_t hash, const void* bytecode, size_t bytes, bool linked) {
    if (t_creating || !shader || !bytecode || !bytes || bytes > 1024u * 1024u) return;
    const int f = familyForProfile(hash, runtimeFlatProfile());
    if (f < 0) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    // One entry per shader OBJECT: the game may create a keyed hash many times.
    if (g_vs.size() >= kRememberCap || g_vs.count(shader)) return;
    VsInfo info;
    info.object = shader;
    info.family = f;
    info.linked = linked;
    info.bytes.assign(static_cast<const BYTE*>(bytecode), static_cast<const BYTE*>(bytecode) + bytes);
    g_vs.emplace(shader, std::move(info));
}

void engineVelocityRememberPs(ID3D11PixelShader* shader, uint64_t hash, const void* bytecode, size_t bytes, bool linked) {
    if (t_creating || !shader || !bytecode || !bytes || bytes > 1024u * 1024u || !anyKeyedPs(hash)) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_ps.size() >= kRememberCap || g_ps.count(shader)) return;
    PsInfo info;
    info.object = shader;
    info.hash = hash;
    info.linked = linked;
    info.bytes.assign(static_cast<const BYTE*>(bytecode), static_cast<const BYTE*>(bytecode) + bytes);
    g_ps.emplace(shader, std::move(info));
}

void engineVelocityAfterFlatDraw(ID3D11DeviceContext* ctx) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    restore(ctx);
    // FlatRuntimeDrawScope next restores the game's MRTs under an internal
    // guard, so the binding shadow's generations do not change. The source
    // eye must reattach MRT6 on the next eligible draw of this same pass.
    g_eyes[kEngineVelocitySourceEye].bindingStale = true;
    cache = DrawCache{};
}

namespace engine_velocity_detail {
void beforeDrawSlow(ID3D11DeviceContext* ctx, bool rtv0Eye) {
    if (!ctx) return;
    const int64_t t0 = qpcNow();
    cache.vs = bindingGeneration(BindSlot::Vs);
    cache.ps = bindingGeneration(BindSlot::Ps);
    cache.rtv = bindingGeneration(BindSlot::Rtv0);
    cache.dsv = bindingGeneration(BindSlot::Dsv0);
    cache.blend = bindingGeneration(BindSlot::Blend);
    cache.pool = bindingGet(BindSlot::VsSrv33);
    cache.scene = bindingGet(BindSlot::VsCb1);
    cache.eye = rtv0Eye;
    // The common case -- not a pool family's vertex shader, nothing of ours
    // bound -- costs no lock (terrain, the interface, every other pass).
    if (!g_anyBound.load(std::memory_order_acquire) &&
        familyForProfile(bindingShaderHash(BindSlot::Vs), runtimeFlatProfile()) < 0) {
        cache.family = -1;
        ++g_draw.quickPaths;
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (live.load(std::memory_order_acquire)) slowPath(ctx, rtv0Eye);
    g_draw.slowTicks += static_cast<uint64_t>(qpcNow() - t0);
}

void noteResourceMapped(const ID3D11Resource* resource, void* data, int mapType) noexcept {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto& w : g_watchInfo)
        if (w.resource == resource) { w.mapped = data; w.mapType = mapType; }
}

// A write to a watched source: what it changed, for the next substituted
// draw of that eye-frame to judge (checkSources). Nothing is dropped here --
// the other eye's rows go through the same cb1 between the eyes' passes, and
// only a later draw of THIS eye-frame reading changed contents matters.
void noteResourceWrite(const ID3D11Resource* resource) noexcept {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    bool matched = false, rowsRead = false, rowsOk = false;
    uint8_t rows[kRowsBytes] = {};
    for (unsigned i = 0; i < kWatchSlots; ++i) {
        WatchInfo& w = g_watchInfo[i];
        if (w.resource != resource) continue;
        matched = true;
        const bool scene = (i & 1u) != 0;
        if (w.mapped) {
            if (scene) {
                // Read before the real Unmap (vscreen calls this first): the
                // mapped memory is still the game's write.
                if (!rowsRead) {
                    rowsRead = true;
                    rowsOk = emit::read(reinterpret_cast<uintptr_t>(w.mapped) + kRowsFirst * 16u, rows, kRowsBytes);
                }
                w.rowsKnown = rowsOk;
                if (rowsOk) std::memcpy(w.rows, rows, kRowsBytes);
            } else if (w.mapType == D3D11_MAP_WRITE_NO_OVERWRITE) {
                ++w.appendEpoch;
            } else {
                ++w.replaceEpoch;
            }
        } else if (scene) {
            w.rowsKnown = false;   // a copy or an update: contents not seen
        } else {
            ++w.replaceEpoch;
        }
        w.mapped = nullptr;
        ++w.writeEpoch;
    }
    // The next draw takes the slow half (owner thread: this runs on it).
    if (matched) cache = DrawCache{};
}
} // namespace engine_velocity_detail

void engineVelocityNotePresentFrame(uint32_t presentFrame) noexcept {
    g_frame.store(presentFrame, std::memory_order_release);
}

void engineVelocityFrameBoundary(ID3D11DeviceContext* ctx) {
    // Runs while live, and once more after a stand-down that left EDVR state
    // bound: configure(false) may come from the config thread, which must not
    // touch the context, so the owner thread puts the game's back.
    if (!live.load(std::memory_order_acquire) && !g_anyBound.load(std::memory_order_acquire)) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    // Leave nothing of EDVR's bound across the frame: the binding shadow's
    // once-a-frame generation bump would otherwise hide whether it still is.
    if (ctx && (g_bound.patchedPs || g_bound.patchedVs || g_bound.derivedBlend)) {
        Ptr<ID3D11PixelShader> ps;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        if (g_bound.patchedPs && ps.Get() == g_bound.patchedPs) { vScreenPSSetShaderRaw(ctx, g_bound.originalPs, nullptr, 0); ++g_draw.restores; }
        Ptr<ID3D11VertexShader> vs;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        if (g_bound.patchedVs && vs.Get() == g_bound.patchedVs) { vScreenVSSetShaderRaw(ctx, g_bound.originalVs, nullptr, 0); ++g_draw.restores; }
        Ptr<ID3D11BlendState> blend;
        float factor[4] = {};
        UINT mask = 0;
        ctx->OMGetBlendState(&blend, factor, &mask);
        if (g_bound.derivedBlend && blend.Get() == g_bound.derivedBlend.Get()) {
            vScreenOMSetBlendStateRaw(ctx, g_bound.gameBlend.Get(), g_bound.blendFactor, g_bound.sampleMask);
            ++g_draw.restores;
        }
    }
    g_bound = Bound{};
    g_anyBound.store(false, std::memory_order_release);
    if (!live.load(std::memory_order_acquire)) return;
    cache = DrawCache{};
    ++g_draw.frames;
    if (ctx) pollCaptures(ctx);   // the eye-pass capture's GPU timers, without waiting
    // The on-foot source's slot target lives only while the source is drawn:
    // kSourceIdleFrames present frames without screen_motion naming it and it
    // goes, with its watches and the held depth.
    Eye& source = g_eyes[kEngineVelocitySourceEye];
    if ((source.slots || g_sourceDepth) && frameNow() - g_sourceNoted > kSourceIdleFrames) {
        if (source.slots)
            Log::get().note("engine motion: on-foot source slot target released (%ux%u, %.1f MB) at present frame %u: "
                            "no on-foot source for %u frames.", source.width, source.height,
                            double(source.width) * source.height * 8.0 / 1e6, frameNow(), kSourceIdleFrames);
        source = Eye{};
        for (unsigned i = 4; i < kWatchSlots; ++i) { watch[i].store(nullptr); g_watchInfo[i] = WatchInfo{}; }
        g_sourceDepth.Reset();
        g_sourceNoted = ~0u;
        g_sourceCamera = SourceCamera{};
    }
    refreshEmitStatus(false);
    // Gaps that land together in one frame are a clock or a scene event, not
    // visibility churn.
    const uint64_t gaps = g_emit.gaps.load(std::memory_order_relaxed);
    const uint64_t burst = gaps - g_lastGaps;
    g_lastGaps = gaps;
    if (burst >= kBurstGaps) { ++g_draw.burstFrames; g_draw.burstGaps += burst; }
    const uint64_t now = nowMs();
    if (now - g_windowStartMs >= kSummaryMs) summaryLocked(now);
}

namespace engine_velocity_detail {
// The counters one kind of view request moves (the eyes' or the source's).
struct ViewCounts {
    uint64_t &asked, &given, &noEmit, &depth, &frame, &invalid, &unwritten, &previous;
};
bool giveViewsLocked(Eye& e, ID3D11Texture2D* sceneDepth, EngineVelocityViews* out, ViewCounts c) {
    ++c.asked;
    if (!g_emitLive.load(std::memory_order_acquire)) { ++c.noEmit; return false; }
    const uint32_t frame = frameNow();
    const unsigned now = frame & 1u, before = (frame - 1u) & 1u;
    if (e.depth.Get() != sceneDepth) { ++c.depth; return false; }
    if (e.frame != frame) { ++c.frame; return false; }
    if (e.invalid) { ++c.invalid; return false; }
    if (!e.written || !e.slotsSrv || !e.poolSrv || e.sceneFrame[now] != frame || !e.scene[now]) {
        ++c.unwritten;
        return false;
    }
    if (e.sceneFrame[before] != frame - 1u || !e.scene[before]) { ++c.previous; return false; }
    e.consumed = true;
    out->slots = e.slotsSrv.Get(); out->slots->AddRef();
    out->pool = e.poolSrv.Get(); out->pool->AddRef();
    out->sceneNow = e.scene[now].Get(); out->sceneNow->AddRef();
    out->scenePrev = e.scene[before].Get(); out->scenePrev->AddRef();
    ++c.given;
    return true;
}
}  // namespace engine_velocity_detail

bool engineVelocityViews(ID3D11DeviceContext*, int eye, ID3D11Texture2D* sceneDepth, EngineVelocityViews* out) {
    if (!out) return false;
    *out = EngineVelocityViews{};
    if (!live.load(std::memory_order_acquire) || eye < 0 || eye > 1 || !sceneDepth) return false;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return giveViewsLocked(g_eyes[eye], sceneDepth, out,
                           {g_draw.viewsAsked, g_draw.viewsGiven, g_draw.refusedNoEmit, g_draw.refusedDepth,
                            g_draw.refusedFrame, g_draw.refusedInvalid, g_draw.refusedUnwritten, g_draw.refusedPrevious});
}

bool engineVelocityTakeCaptureGpu(EngineVelocityCaptureGpu* out) {
    if (!out) return false;
    *out = EngineVelocityCaptureGpu{};
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    bool any = false;
    for (int k = 0; k < kCaptureKinds; ++k) {
        std::vector<double>& v = g_captureMs[k];
        out->events[k] = static_cast<uint32_t>(v.size());
        if (!v.empty()) {
            any = true;
            std::sort(v.begin(), v.end());
            out->medianMs[k] = v[v.size() / 2];
            out->p95Ms[k] = v[std::min(v.size() - 1, static_cast<size_t>(double(v.size()) * 0.95))];
        }
        v.clear();
    }
    out->untimed = g_captureUntimed;
    out->invalid = g_captureInvalid;
    g_captureUntimed = g_captureInvalid = 0;
    return any || out->untimed || out->invalid;
}

bool engineVelocityPoolFamilyVs(uint64_t vsHash) noexcept {
    return familyForProfile(vsHash, runtimeFlatProfile()) >= 0;
}
bool engineVelocityPoolFamilyPair(uint64_t vsHash, uint64_t psHash) noexcept {
    return keyedPs(familyForProfile(vsHash, runtimeFlatProfile()), psHash);
}

void engineVelocityNoteSource(ID3D11Texture2D* sourceDepth, ID3D11Buffer* sceneConstants,
                              EngineVelocitySourceSignal signal) {
    if (!live.load(std::memory_order_acquire) || !sourceDepth) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_sourceDepth.Get() != sourceDepth) g_sourceDepth = sourceDepth;
    g_sourceNoted = frameNow();
    // The source camera for this present frame: the naming draw's scene
    // constants and their rows as last written (the watch's source-scene
    // slot follows this buffer from here; a first sight copies what another
    // slot knows of it, else the rows are unseen until its next write).
    SourceCamera& c = g_sourceCamera;
    c.frame = g_sourceNoted;
    c.scene = sceneConstants;
    c.rowsKnown = false;
    ++g_draw.sourceNamings;
    ++g_draw.sourceNamingsBy[signal == EngineVelocitySourceSignal::ScreenDepth ? 1 : 0];
    if (sceneConstants) {
        D3D11_BUFFER_DESC sd{};
        sceneConstants->GetDesc(&sd);
        if (sd.ByteWidth >= (kRowsFirst + 6u) * 16u && sd.ByteWidth <= 65536u) {
            const WatchInfo& ws = assignWatch(static_cast<unsigned>(kEngineVelocitySourceEye) * 2u + 1u, sceneConstants);
            c.rowsKnown = ws.rowsKnown;
            if (ws.rowsKnown) std::memcpy(c.rows, ws.rows, kRowsBytes);
        }
    }
    if (!c.rowsKnown) ++g_draw.sourceNamingsUnseen;
    // The next pool draw is checked against this camera even if nothing it
    // binds has changed since a draw declined before the naming.
    cache = DrawCache{};
}

bool engineVelocitySourceViews(ID3D11Texture2D* sourceDepth, EngineVelocityViews* out) {
    if (!out) return false;
    *out = EngineVelocityViews{};
    if (!live.load(std::memory_order_acquire) || !sourceDepth) return false;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return giveViewsLocked(g_eyes[kEngineVelocitySourceEye], sourceDepth, out,
                           {g_draw.sourceViewsAsked, g_draw.sourceViewsGiven, g_draw.sourceRefusedNoEmit,
                            g_draw.sourceRefusedDepth, g_draw.sourceRefusedFrame, g_draw.sourceRefusedInvalid,
                            g_draw.sourceRefusedUnwritten, g_draw.sourceRefusedPrevious});
}

void engineVelocityNotePanelPixels(uint32_t joined, uint32_t masked, uint32_t camera, uint32_t stale, uint32_t corrupt,
                                   uint32_t eyeDraws, uint32_t pixelStride) {
    if (!live.load(std::memory_order_acquire)) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const int mode = pixelStride > 1u ? 1 : 0;
    const uint32_t k[5] = {joined, masked, camera, stale, corrupt};
    for (int i = 0; i < 5; ++i) g_draw.panel[mode][i] += k[i];
    g_draw.panelDraws[mode] += eyeDraws;
}

void engineVelocityNotePixels(uint32_t joined, uint32_t masked, uint32_t camera, uint32_t stale, uint32_t corrupt) {
    if (!live.load(std::memory_order_acquire)) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_draw.pixelsJoined += joined;
    g_draw.pixelsMasked += masked;
    g_draw.pixelsCamera += camera;
    g_draw.pixelsStale += stale;
    g_draw.pixelsCorrupt += corrupt;
    ++g_draw.pixelReads;
}

} // namespace edvr
