#include "engine_velocity.h"
#include "temporal_shader_bytecode.h"
#include "dxbc_flat_overlay.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
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
#include "gpu_census.h"
#include "gpu_timing.h"
#include "dxbc_engine_velocity.h"
#include "engine_velocity_emit.h"
#include "engine_velocity_primary_copy.h"
#include "engine_velocity_families.h"
#include "engine_velocity_unkeyed.h"
#include "engine_velocity_state.h"
#include "engine_motion_cpu.h"   // the CPU instrument: the draw side, apply, the tees and the lazy patches are timed here
#include "exposure_fix.h"   // lookupShaderHash: the PS shadow probe reads the registry
#include "flat_compute_readback.h"
#include "flat_query_cut.h"   // the flat bracket's kept answers, and the sampled check of each
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
namespace primaryCopy = engine_velocity_primary_copy;

// primaryCopy::apply as engine motion's own CPU time (engine_motion_cpu.h,
// Part kApply): the render thread's share of the pool scatter, including any
// wait for the engine's copier threads on primaryCopy's mutex. Nested inside
// the draw side's scope, so the draw side's figure excludes it.
template <class... A> bool timedApply(A&&... a) {
    emcpu::Scope timed(emcpu::kApply);
    return primaryCopy::apply(std::forward<A>(a)...);
}

std::atomic<bool> live{false};
DrawCache cache;
uint64_t familyDraws[kMaxFamilies] = {};
uint64_t g_stateCalls = 0;
// The flat census drains the two counts once a frame (engineVelocityTakeWrapperCounts): what was
// already handed over, and the draws the 30 s summary zeroed out of familyDraws before it could.
uint64_t g_stateCallsTaken = 0, g_substitutedBase = 0, g_substitutedTaken = 0;
uint64_t g_stateCallsAtWindow = 0;   // g_stateCalls when the 30 s summary last ran: its window's share is the difference
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
// (40,960 texels, 0 mismatches; MRT6 8192 checked, 0 bad). DE54/91F8 is now
// flat-only: its SV_IsFrontFace occupies the register formerly assumed for
// PS position. A separate rasterizer input passes the actual VS draw gate.
// DE54/A607 remains unqualified and unkeyed.
//
// Eye run 055427 (2026-09-26, parked at the port, the "close range" entry):
// vs_61AE's stock pixel shader ps_4504BC268E109C31 drew hull parts all
// session, and the two big rotating families drew NOTHING through their keyed
// pairs at this range -- vs_4361 through ps_51EE1F922FD220B0, vs_889A through
// ps_D31DCAFA7C05CB47, both stock -- so most of the hull kept the camera term
// while it moved. All three keyed: ps_4504 from the 09-06 dump, ps_51EE and
// ps_D31D dumped by the 2026-09-27 glare_shader_dump flight at the port; each
// pair through the corpus identity harness (40,960 texels, 0 mismatches;
// MRT6 8192 checked, 0 bad). ps_BBDE4E71FB78528A (vs_66DE) keyed from the
// 15:46 session's dumps the same day. Provenance correction, 2026-09-28:
// ps_BA58469C3D6120A7 is the exact EDVR patch of ps_4375B72964F386CD,
// not another stock variant. Its occupied slot is our existing export.
// Likewise all kSelfMarking hashes are generated substitutions; see their
// lineage in engine_velocity_families.h and the investigation doc.
using engine_velocity_family::Family;
using engine_velocity_family::kFamilies;
using engine_velocity_family::kFamilyCount;
using engine_velocity_family::familyForProfile;
using engine_velocity_family::keyedPs;
using engine_velocity_family::selfMarkingPair;
static_assert(kFamilyCount <= kMaxFamilies, "familyDraws holds every family");
bool anyKeyedPs(uint64_t hash) {
    for (int i = 0; i < kFamilyCount; ++i)
        if ((!kFamilies[i].flatOnly || runtimeFlatProfile()) && keyedPs(i, hash, runtimeFlatProfile())) return true;
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
    std::unordered_map<ID3D11PixelShader*, Ptr<ID3D11PixelShader>> guardedPs;
    std::unordered_map<ID3D11PixelShader*, std::string> psFailed;
    uint64_t binds = 0;                            // substitutions made (this window)
    uint64_t unkeyedPsDraws = 0;                   // bind events with a pixel shader outside the keyed set
    uint64_t unkeyedPsHash = 0;
    uint64_t selfMarked = 0;                       // self-marking pair's stock draws (the game's own slot+depth channel)
    uint64_t selfMarkedLatched = 0;                // ...of those, the draws that latched the game's texture for the eye-frame
};
FamilyState g_families[kFamilyCount];
// The flat census: every unkeyed (vs, ps) pair of a known family that bound in the
// current 5 s window, four named at most. Written by the slow half and read by
// engineVelocityFormatUnkeyed, both under g_mutex.
engine_velocity_unkeyed::Table g_unkeyed;

// What EDVR bound in place of the game's state, and at which generation of
// the game's own binding, so a later look can tell whether it is still bound.
struct Bound {
    ID3D11PixelShader* originalPs = nullptr;
    ID3D11PixelShader* patchedPs = nullptr;
    uint32_t psGen = 0;
    Ptr<ID3D11ShaderResourceView> gameSrv3;
    Ptr<ID3D11ShaderResourceView> guardSrv3;
    uint32_t srv3Gen = 0;
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
    bool flatProvenance=false;
};
std::vector<BlendEntry> g_blends;
constexpr size_t kBlendCap = 64;

// --- The watched sources --------------------------------------------------------
// Per watch slot (eye0 pool, eye0 scene, eye1 pool, eye1 scene, source pool,
// source scene): the writes seen since the slot was assigned, and for the
// scene constants the registers 270..275 the last Unmap left.
constexpr unsigned kRowsFirst = 270, kRowsBytes = 6 * 16;
// The freshness stamp (2026-09-25): the engine-motion shaders read
// EN[276].x (SEN[276].x on foot) as the present-frame clock the emit folded
// into each marker, uint bits. Our scene-constants copies are sized to hold
// it even where the game's own buffer stops at row 275.
constexpr unsigned kStampFloat4 = 276, kStampBytes = (kStampFloat4 + 1) * 16;
struct WatchInfo {
    const ID3D11Resource* resource = nullptr;
    void* mapped = nullptr;
    int mapType = 0;
    uint32_t replaceEpoch = 0, appendEpoch = 0, writeEpoch = 0;
    bool rowsKnown = false;
    uint8_t rows[kRowsBytes] = {};
};
WatchInfo g_watchInfo[kWatchSlots];
std::atomic<const ID3D11Resource*> primaryPoolResources[kPrimaryPoolResources] = {};
struct PrimaryMap {
    Ptr<ID3D11Buffer> buffer;
    uint32_t bytes=0,lastFrame=0;
    uint64_t sequence=0;
    bool mapped=false;
};
// The pool cache holds each registered game buffer by reference (an equal
// pointer is then the same object), so a slot exists only while the feature is
// live: notePrimaryBufferCreated refuses a buffer otherwise, and
// releasePrimaryPoolsLocked lets every one go when the feature stands down.
PrimaryMap g_primaryMaps[kPrimaryPoolResources];
uint64_t g_primaryMapSequence=0,g_primaryMapOverflow=0,g_primaryApplied=0;   // the engine mutex's
// Bumped by observePoolCopy on game job threads, which take no engine lock, so atomic.
std::atomic<uint64_t> g_primaryCopyCalls{0};
// The cache-full line is printed once per live period (cleared with the slots);
// g_primaryMapOverflow, the 30 s line's figure, keeps counting past it.
bool g_primaryOverflowNoted=false;

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
    DXGI_FORMAT slotFormat=DXGI_FORMAT_UNKNOWN;
    Ptr<ID3D11RenderTargetView> slotsRtv;
    Ptr<ID3D11ShaderResourceView> slotsSrv;
    Ptr<ID3D11Texture2D> overlayBase;
    Ptr<ID3D11ShaderResourceView> overlayBaseSrv;
    bool overlayGroup = false;
    unsigned width = 0, height = 0;
    // The game's own target-6 texture, latched at this eye-frame's first
    // self-marking draw (kSelfMarking): the detail shaders natively write the
    // marker encoding (2*slot+1, z) there. Validated against the pass's depth
    // size and R32G32_FLOAT at capture; handed to the compose with the views.
    Ptr<ID3D11Texture2D> gameMark;
    Ptr<ID3D11ShaderResourceView> gameMarkSrv;
    uint32_t gameMarkFrame = ~0u;
    Ptr<ID3D11Buffer> pool;              // the snapshot copies
    Ptr<ID3D11ShaderResourceView> poolSrv;
    primaryCopy::OutputCache poolOutput; // dies with this eye/source or a pool replacement
    UINT poolBytes = 0;
    Ptr<ID3D11Buffer> scene[2];          // the game's cb1, by present-frame parity
    uint32_t sceneFrame[2] = {~0u, ~0u};
    UINT sceneBytes = 0;
    Ptr<ID3D11Buffer> stampCell;         // 16 bytes: the frame stamp's carrier into scene[slot]
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
// Marker ownership can start before world naming, on more than one depth in
// the same present frame. Scene/pool snapshots keep their world-only lifecycle.
struct FlatMarkerPlane {
    Ptr<ID3D11Texture2D> depth, slots;
    Ptr<ID3D11RenderTargetView> rtv;
    Ptr<ID3D11ShaderResourceView> srv;
    uint32_t frame=~0u;
};
std::array<FlatMarkerPlane,4> g_flatMarkerPlanes;
struct FlatDomainSaved {
    Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11VertexShader> vs;
    Ptr<ID3D11PixelShader> ps;
    Ptr<ID3D11BlendState> blend;
    Ptr<ID3D11RenderTargetView> targets[8];
    Ptr<ID3D11DepthStencilView> depth;
    Ptr<ID3D11DeviceContext1> rangedContext;
    Ptr<ID3D11Buffer> tokenOriginal;
    UINT tokenSlot=~0u,tokenFirst=0,tokenCount=0;
    FLOAT factor[4]{};UINT sampleMask=~0u;
};
FlatDomainSaved g_flatDomainSaved;
struct FlatDomainShader {
    Ptr<ID3D11VertexShader> originalVs,patchedVs;
    Ptr<ID3D11PixelShader> originalPs,patchedPs;
    FlatEngineDomain domain=FlatEngineDomain::World;
    bool coverage=false;
    unsigned tokenSlot=~0u;
};
std::vector<FlatDomainShader> g_flatDomainShaders;
Ptr<ID3D11Buffer> g_flatDomainTokenBuffer;

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
emit::Stats g_primaryEmit;
std::atomic<uint64_t> g_primaryAttempts{0};
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
    uint64_t overlayCopies = 0, overlayBytes = 0, overlayGuardedDraws = 0;
    uint64_t overlayDeclinedState = 0, overlayDeclinedCreate = 0, overlayDeclinedShader = 0;
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
    // The self-marking detail shaders' draw-path census (the seam arc,
    // 2026-09-27): reached slowPath with a self-marking PS bound, of those
    // the ones where neither the eye-pass nor the depth map named an eye,
    // and their PS's binds through vscreen's shader hook.
    uint64_t selfMarkSeen = 0, selfMarkNoEye = 0, selfMarkBinds = 0;
    // The shadow probe (the same arc): sampled pool-context draws, and the
    // ones where the live pixel shader was not the shadow's -- the bind
    // bypassed the hook, and the truth was written in before the slow half.
    uint64_t poolShadowProbes = 0, poolShadowHealed = 0;
    uint64_t pixelsJoined = 0, pixelsMasked = 0, pixelsCamera = 0, pixelsStale = 0, pixelsCorrupt = 0, pixelsStamped = 0,
             pixelReads = 0;
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
    // kPanelSampleFrames, one eye pixel in kPanelSampleStride squared). The
    // kinds are the compose's six (stale stamp last; see enginePixelZ).
    uint64_t panel[2][6] = {}, panelDraws[2] = {};
    uint64_t burstFrames = 0, burstGaps = 0;
    // The flat lazy bracket (engineVelocityFlatBeginDraw): the runs of substituted draws EDVR's state was
    // bound for, the draws that found it still bound from the one before, and why the game's was put back.
    uint64_t flatRuns = 0, flatKept = 0, flatFlushBy[static_cast<unsigned>(EngineVelocityFlushCause::kCount)] = {};
    void clear() { *this = DrawStats{}; }
};
DrawStats g_draw;
uint64_t g_windowStartMs = 0;
uint64_t g_lastGaps = 0;                 // g_emit.gaps at the last frame boundary
uint32_t g_lastSubstitution = ~0u;       // the present frame of the last substitution
uint32_t g_psShadowProbeN = 0;           // the header's sampling counter for the probe below
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

void observePrimaryEmit(const emit::PrimaryIdentity& identity, uintptr_t owner, uintptr_t key,
                        uintptr_t position, uintptr_t quaternion, int32_t before, int32_t after) noexcept {
    if (!live.load(std::memory_order_acquire) || !g_table) return;
    g_primaryAttempts.fetch_add(1,std::memory_order_relaxed);
    const auto sink=+[](uintptr_t item,const emit::Pose&,const emit::Pose& previous,uint32_t marker,uint32_t frame) noexcept {
        uint32_t native[84]{};
        return emit::read(item,native,sizeof(native)) && primaryCopy::recordEmission(item,native,previous,marker,frame);
    };
    // Invalidate an older claim at the exact newly-appended primary item,
    // even when this call will be declined by the pose/deformation checks.
    if(int64_t(after)-int64_t(before)==1) {
        const auto lookup=g_lookup.load(std::memory_order_acquire);
        const uintptr_t entry=lookup?emit::guardedLookup(lookup,owner+emit::kOwnerDictionary,key):0;
        uintptr_t item=0;
        if(entry && emit::collectItems(entry,1,&item))primaryCopy::invalidateEmission(item);
        else primaryCopy::invalidateEmission(0);
    } else primaryCopy::invalidateEmission(0); // ownership/fault: no stale CPU claim may survive
    emit::observePrimary(identity,owner,key,position,quaternion,before,after,frameNow(),
                         g_lookup.load(std::memory_order_acquire),*g_table,g_primaryEmit,sink);
}

// The engine's pool copier (game job threads, once per copy-list entry). No
// engine lock: everything this touches is primaryCopy's own state -- g_stats,
// g_pools, g_emissions, g_overflowFrame and g_emissionEpoch, each read and
// written only under primaryCopy::g_mutex, which copier and invalidateMapped
// take themselves -- plus the atomic counter and the atomic frame clock. It
// never reaches g_gpu or g_gpuMutex (apply and reset), and primaryCopy calls
// nothing back into this file, so the order stays engine g_mutex -> g_gpuMutex
// -> primaryCopy::g_mutex on the render thread and a job thread holds only the
// last. The engine mutex is held by the render thread across its D3D calls, so
// taking it here stalled a job thread behind the slow half for a counter.
void observePoolCopy(uintptr_t mapped,uint32_t stride,uintptr_t source,uint64_t slot,uint32_t count) noexcept {
    if(!live.load(std::memory_order_acquire))return;
    g_primaryCopyCalls.fetch_add(1,std::memory_order_relaxed);
    if(count==UINT32_MAX || slot>UINT32_MAX){primaryCopy::invalidateMapped(mapped);return;}
    primaryCopy::copier(mapped,stride,source,static_cast<uint32_t>(slot),count,frameNow());
}
void* observeMergeBegin(uintptr_t destination,uintptr_t source) noexcept {
    if(!live.load(std::memory_order_acquire))return nullptr;
    return primaryCopy::beginMergeOpaque(destination,source,frameNow());
}
void observeMergeEnd(void* plan,bool completed) noexcept {primaryCopy::endMergeOpaque(plan,completed);}
void observeDictionaryClear(uintptr_t dictionary) noexcept {
    if(live.load(std::memory_order_acquire))primaryCopy::invalidateDictionary(dictionary);
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
    emcpu::Scope patch(emcpu::kPatch);   // a cache miss only: the DXBC patch and CreateVertexShader
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
    emcpu::Scope patch(emcpu::kPatch);   // a cache miss only: the DXBC patch and CreatePixelShader
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

ID3D11PixelShader* guardedOverlayPsFor(ID3D11DeviceContext* ctx, int f, ID3D11PixelShader* ps) {
    FamilyState& s = g_families[f];
    const auto found = s.guardedPs.find(ps);
    if (found != s.guardedPs.end()) return found->second.Get();
    emcpu::Scope patch(emcpu::kPatch);   // a cache miss only: the guarded overlay patch and its CreatePixelShader
    Ptr<ID3D11PixelShader> patched;
    const auto info = g_ps.find(ps);
    if (info != g_ps.end() && !info->second.linked) {
        std::vector<BYTE> out;
        std::string why;
        if (engineVelocityPatchPs(info->second.bytes.data(), info->second.bytes.size(), s.inputs, out, why, true)) {
            Ptr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            t_creating = true;
            const HRESULT hr = dev->CreatePixelShader(out.data(), out.size(), nullptr, &patched);
            t_creating = false;
            if (FAILED(hr)) patched.Reset();
        }
    }
    s.guardedPs.emplace(ps, patched);
    return patched.Get();
}

bool overlayDepthState(ID3D11DeviceContext* ctx) {
    Ptr<ID3D11DepthStencilState> state;
    UINT reference = 0;
    ctx->OMGetDepthStencilState(&state, &reference);
    engineVelocityNoteStateCalls(1);
    if (!state) return false;
    D3D11_DEPTH_STENCIL_DESC d{};
    state->GetDesc(&d);
    // Stencil testing/writes retain the game's state. MRT6 receives only the
    // fragments that passed it, while depth remains unchanged.
    return d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO &&
           d.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL;
}

bool snapshotOverlayBase(ID3D11DeviceContext* ctx, Eye& e) {
    if (!e.slots || !e.slotsRtv || !e.width || !e.height) return false;
    if (!e.overlayBase) {
        D3D11_TEXTURE2D_DESC d{};
        e.slots->GetDesc(&d);
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = 0;
        d.MiscFlags = 0;
        Ptr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &e.overlayBase)) ||
            FAILED(dev->CreateShaderResourceView(e.overlayBase.Get(), nullptr, &e.overlayBaseSrv))) {
            e.overlayBase.Reset(); e.overlayBaseSrv.Reset();
            return false;
        }
    }
    // FlatRuntimeDrawScope restored the game's MRTs after the prior draw. Do
    // not copy a texture still bound for output or while the private SRV is
    // live. A later bindTarget reattaches MRT6 for this draw.
    ID3D11RenderTargetView* rt[8] = {};
    ctx->OMGetRenderTargets(8, rt, nullptr);
    engineVelocityNoteStateCalls(1);
    std::array<Ptr<ID3D11RenderTargetView>, 8> held;
    for (unsigned i = 0; i < 8; ++i) held[i].Attach(rt[i]);
    for (auto* view : rt) if (view == e.slotsRtv.Get()) return false;
    Ptr<ID3D11ShaderResourceView> boundSrv;
    ctx->PSGetShaderResources(kEngineVelocityOverlaySnapshotSlot, 1, &boundSrv);
    engineVelocityNoteStateCalls(1);
    if (boundSrv.Get() == e.overlayBaseSrv.Get()) return false;
    ctx->CopyResource(e.overlayBase.Get(), e.slots.Get());
    engineVelocityNoteStateCalls(1);
    ++g_draw.overlayCopies;
    g_draw.overlayBytes += uint64_t(e.width) * e.height * 8u;
    return true;
}

// The derived blend state for the game's current one (null = the default).
ID3D11BlendState* derivedBlendFor(ID3D11DeviceContext* ctx, ID3D11BlendState* game, const char** refused,bool flatProvenance=false) {
    for (const auto& b : g_blends)
        if (b.game.Get() == game && b.flatProvenance==flatProvenance) { *refused = b.refused; return b.derived.Get(); }
    if (g_blends.size() >= kBlendCap) g_blends.clear();   // g_bound holds its own references
    Ptr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    BlendEntry entry;
    entry.game = game;
    entry.flatProvenance=flatProvenance;
    entry.derived = engineVelocityCreateDerivedBlend(dev.Get(), game, &entry.refused);
    if(flatProvenance && entry.derived) {
        D3D11_BLEND_DESC d{};entry.derived->GetDesc(&d);
        d.RenderTarget[kEngineVelocityTarget].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        Ptr<ID3D11BlendState> complete;
        if(FAILED(dev->CreateBlendState(&d,&complete))){entry.derived.Reset();entry.refused="flat-domain-provenance-blend-create";}
        else entry.derived=std::move(complete);
    }
    g_blends.push_back(entry);
    *refused = entry.refused;
    return entry.derived.Get();
}

// Put the game's own state back where EDVR's is still bound (the game has not
// rebound since: the generation says so).
void restore(ID3D11DeviceContext* ctx) {
    if (g_bound.guardSrv3 && bindingGeneration(BindSlot::PsSrv3) == g_bound.srv3Gen) {
        Ptr<ID3D11ShaderResourceView> actual;
        ctx->PSGetShaderResources(kEngineVelocityOverlaySnapshotSlot, 1, &actual);
        engineVelocityNoteStateCalls(1);
        if (actual.Get() == g_bound.guardSrv3.Get()) {
            ID3D11ShaderResourceView* game = g_bound.gameSrv3.Get();
            FlatComputeInternalScope internal;
            ctx->PSSetShaderResources(kEngineVelocityOverlaySnapshotSlot, 1, &game);
            engineVelocityNoteStateCalls(1);
            ++g_draw.restores;
        }
    }
    if (g_bound.patchedPs && bindingGeneration(BindSlot::Ps) == g_bound.psGen) {
        vScreenPSSetShaderRaw(ctx, g_bound.originalPs, nullptr, 0);
        engineVelocityNoteStateCalls(1);
        ++g_draw.restores;
    }
    if (g_bound.patchedVs && bindingGeneration(BindSlot::Vs) == g_bound.vsGen) {
        vScreenVSSetShaderRaw(ctx, g_bound.originalVs, nullptr, 0);
        engineVelocityNoteStateCalls(1);
        ++g_draw.restores;
    }
    if (g_bound.derivedBlend && bindingGeneration(BindSlot::Blend) == g_bound.blendGen) {
        vScreenOMSetBlendStateRaw(ctx, g_bound.gameBlend.Get(), g_bound.blendFactor, g_bound.sampleMask);
        engineVelocityNoteStateCalls(1);
        ++g_draw.restores;
    }
    g_bound = Bound{};
    g_anyBound.store(false, std::memory_order_release);
    cache.family = -1;
}

// --- The flat draw bracket, lazy form (engine_velocity.h says why) -------------------------------------
// What the game had bound at the render targets when EDVR added MRT6 over them, so the set can go back:
// the eight views and the depth view, and the generations of the game's slot-0 and depth bindings they
// were read at. Owner thread only; restore()'s g_bound is the other half (shaders, blend, t3).
struct FlatGame {
    Ptr<ID3D11RenderTargetView> rtv[8];
    Ptr<ID3D11DepthStencilView> dsv;
    uint32_t rtvGen = 0, dsvGen = 0;
    bool saved = false;   // rtv and dsv are the game's set as of those generations
    bool bound = false;   // EDVR's MRT6 is bound over it
};
FlatGame g_flatGame;
bool g_flatLazy = false;   // the runtime's switch: false while a diagnostic capture is armed
std::atomic<bool> g_flatPending{false};

// Two more answers the flat bracket used to ask the context for at every run of substituted draws, and now keeps
// (flat_query_cut.h says why, and how they are checked). Owner thread only; released with the frame.
//
// The game's blend state as it was last read, with the factor and mask, and the generation of the game's blend
// binding it was read at: while that generation stands the game has set nothing since, and EDVR's own sets and
// restores go around the shadow, so the state is what it was.
struct GameBlendMemo {
    Ptr<ID3D11BlendState> state;
    float factor[4] = {};
    UINT mask = ~0u;
    uint32_t gen = 0;
    bool valid = false;
};
GameBlendMemo g_gameBlendMemo;
// The render-target binding (generations of slot 0 and the depth) whose set the runtime accepted with MRT6 added and
// read back as kept: the same set under the same generations is accepted again.
struct KeptMemo {
    uint32_t rtvGen = 0, dsvGen = 0;
    bool valid = false;
};
KeptMemo g_keptMemo;

// The game's render-target set as the context holds it now: eight views and the depth view, MRT6 only when it is
// EDVR's own (which is then not part of the set, and reported by the flag).
struct TargetRead {
    ID3D11RenderTargetView* rtv[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    bool ourMrt6 = false;
};
void readTargets(ID3D11DeviceContext* ctx, TargetRead* out) {
    ctx->OMGetRenderTargets(8, out->rtv, &out->dsv);
    engineVelocityNoteStateCalls(1);
    ID3D11RenderTargetView*& six = out->rtv[kEngineVelocityTarget];
    bool ours=six && six == g_eyes[kEngineVelocitySourceEye].slotsRtv.Get();
    if(six && runtimeFlatProfile())for(const auto& plane:g_flatMarkerPlanes)
        if(six==plane.rtv.Get()){ours=true;break;}
    if (ours) {
        six->Release();
        six = nullptr;
        out->ourMrt6 = true;
    }
}

// The game's render-target set, read once per binding: not again while the game has not rebound the
// targets (the generations of the two slots the hooks keep say so; a set that keeps the render targets
// and changes UAVs does not move them, and does not move MRT6 either), and not again after a restore that put
// the same set back. The read comes back with EDVR's own MRT6 in it only if a restore was skipped, and MRT6 is
// never part of the game's set. A held set is checked against the context on the sampled frames.
void flatSaveTargets(ID3D11DeviceContext* ctx) {
    const uint32_t rtvGen = bindingGeneration(BindSlot::Rtv0), dsvGen = bindingGeneration(BindSlot::Dsv0);
    const bool held = g_flatGame.saved && g_flatGame.rtvGen == rtvGen && g_flatGame.dsvGen == dsvGen;
    FlatQueryPlan plan = FlatQueryPlan::Ask;
    if (held) {
        plan = flatQueryCut().plan(FlatQuery::GameTargets);
        if (plan == FlatQueryPlan::Shortcut) return;
    }
    TargetRead now;
    readTargets(ctx, &now);
    if (plan == FlatQueryPlan::Sample) {
        bool agree = now.dsv == g_flatGame.dsv.Get() && now.ourMrt6 == g_flatGame.bound;
        for (unsigned i = 0; i < 8 && agree; ++i) agree = now.rtv[i] == g_flatGame.rtv[i].Get();
        if (flatQueryCut().compared(FlatQuery::GameTargets, agree)) {
            char line[400];
            flatQueryFallbackLine(line, sizeof(line), FlatQuery::GameTargets, "the render targets it held were not the bound ones");
            Log::get().note("%s", line);
        }
    }
    g_flatGame = FlatGame{};   // a rebind by the game took MRT6 with the old set
    for (unsigned i = 0; i < 8; ++i) g_flatGame.rtv[i].Attach(now.rtv[i]);
    g_flatGame.dsv.Attach(now.dsv);
    if (now.ourMrt6) g_flatGame.bound = true;   // EDVR's MRT6 is over this set already: a restore has to take it off
    g_flatGame.rtvGen = rtvGen;
    g_flatGame.dsvGen = dsvGen;
    g_flatGame.saved = true;
}

// The game's state back where EDVR's is still bound: shaders, blend and t3 by generation (restore), the
// render targets by the saved set when the game has not rebound them since. Leaves the draw cache alone:
// slowPath calls this mid-visit, and the visit goes on to bind MRT6 over the set again. The saved set stays: the
// context holds it again, and the generations say for how long that stays true. It does not stay past the frame
// (engineVelocityFlatFrameEnd): a set held that long would keep the game's views alive.
void flatRestoreLocked(ID3D11DeviceContext* ctx) {
    restore(ctx);
    if (g_flatGame.bound && g_flatGame.saved && ctx && bindingGeneration(BindSlot::Rtv0) == g_flatGame.rtvGen &&
        bindingGeneration(BindSlot::Dsv0) == g_flatGame.dsvGen) {
        ID3D11RenderTargetView* rt[8];
        for (unsigned i = 0; i < 8; ++i) rt[i] = g_flatGame.rtv[i].Get();
        FlatComputeInternalScope internal;   // the binding shadow does not hear EDVR's own restore
        ctx->OMSetRenderTargets(8, rt, g_flatGame.dsv.Get());
        engineVelocityNoteStateCalls(1);
        ++g_draw.restores;
    }
    g_flatGame.bound = false;   // MRT6 is out; the game's set is still the one saved
    g_flatPending.store(false, std::memory_order_release);
}

// The same, and the next producer draw takes the slow half and binds MRT6 again. `counted`: something of
// EDVR's was bound, so this is a restore worth naming in the census (a declined draw with nothing kept bound
// still resets the cache, as every flat draw always did, but restores nothing).
void flatFlushLocked(ID3D11DeviceContext* ctx, EngineVelocityFlushCause cause, bool counted = true) {
    if (counted) ++g_draw.flatFlushBy[static_cast<unsigned>(cause)];
    flatRestoreLocked(ctx);
    g_eyes[kEngineVelocitySourceEye].bindingStale = true;   // MRT6 went without a game generation
    cache = DrawCache{};
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
FlatMarkerPlane* flatMarkerPlane(ID3D11DeviceContext* ctx, ID3D11Texture2D* depth) {
    if(!ctx || !depth)return nullptr;
    D3D11_TEXTURE2D_DESC dd{};depth->GetDesc(&dd);
    if(dd.SampleDesc.Count!=1 || dd.ArraySize!=1){++g_draw.depthUnsupported;return nullptr;}
    const uint32_t frame=frameNow();
    FlatMarkerPlane* plane=nullptr;
    for(auto& entry:g_flatMarkerPlanes)if(entry.depth.Get()==depth){plane=&entry;break;}
    if(!plane) {
        for(auto& entry:g_flatMarkerPlanes)if(!entry.depth){plane=&entry;break;}
        if(!plane)for(auto& entry:g_flatMarkerPlanes)
            if(entry.frame!=frame && entry.depth!=g_sourceDepth &&
               entry.depth!=g_eyes[kEngineVelocitySourceEye].depth){plane=&entry;break;}
        // A current-frame marker, or either source-depth pin, is never evicted.
        if(!plane)return nullptr;
        *plane=FlatMarkerPlane{};
        D3D11_TEXTURE2D_DESC d{};
        d.Width=dd.Width;d.Height=dd.Height;d.MipLevels=1;d.ArraySize=1;d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;d.Usage=D3D11_USAGE_DEFAULT;
        d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);
        if(FAILED(dev->CreateTexture2D(&d,nullptr,&plane->slots)) ||
           FAILED(dev->CreateRenderTargetView(plane->slots.Get(),nullptr,&plane->rtv)) ||
           FAILED(dev->CreateShaderResourceView(plane->slots.Get(),nullptr,&plane->srv))) {
            *plane=FlatMarkerPlane{};++g_draw.createFailed;return nullptr;
        }
        plane->depth=depth;
    }
    if(plane->frame!=frame) {
        const float empty[4]={-1,0,0,0};ctx->ClearRenderTargetView(plane->rtv.Get(),empty);
        plane->frame=frame;
    }
    return plane;
}

bool ensureSlots(ID3D11DeviceContext* ctx, Eye& e, int eye, ID3D11Texture2D* depth) {
    D3D11_TEXTURE2D_DESC dd{};
    depth->GetDesc(&dd);
    // A single-sample, single-slice scene depth only: MRT6 must match the
    // pass's depth target exactly or the runtime drops the game's draw.
    if (dd.SampleDesc.Count != 1 || dd.ArraySize != 1) { ++g_draw.depthUnsupported; return false; }
    if(runtimeFlatProfile() && eye==kEngineVelocitySourceEye) {
        FlatMarkerPlane* plane=flatMarkerPlane(ctx,depth);
        if(!plane)return false;
        if(e.depth.Get()!=depth || e.slots!=plane->slots) {
            e.overlayBase.Reset();e.overlayBaseSrv.Reset();e.overlayGroup=false;
            e.gameMark.Reset();e.gameMarkSrv.Reset();e.gameMarkFrame=~0u;
            e.depth=depth;e.slots=plane->slots;e.slotsRtv=plane->rtv;e.slotsSrv=plane->srv;
            e.width=dd.Width;e.height=dd.Height;e.slotFormat=DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
        return true;
    }
    const DXGI_FORMAT slotFormat=runtimeFlatProfile() && eye==kEngineVelocitySourceEye?
        DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_R32G32_FLOAT;
    if (e.depth.Get() == depth && e.slots && e.width == dd.Width && e.height == dd.Height && e.slotFormat==slotFormat) return true;
    const void* wasDepth = e.depth.Get();
    const unsigned wasW = e.width, wasH = e.height;
    e.slots.Reset(); e.slotsRtv.Reset(); e.slotsSrv.Reset();
    e.overlayBase.Reset(); e.overlayBaseSrv.Reset(); e.overlayGroup = false;
    // The latched game channel was latched for the old depth's size: it goes
    // with it (the review's F3; a stale SRV of the wrong size would read as
    // cleared until the next capture re-latches anyway).
    e.gameMark.Reset(); e.gameMarkSrv.Reset(); e.gameMarkFrame = ~0u;
    e.depth = depth; e.width = dd.Width; e.height = dd.Height;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = dd.Width; d.Height = dd.Height; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1;
    d.Format = slotFormat;e.slotFormat=slotFormat;
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
    engineVelocityNoteStateCalls(1);
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
    // The pool's recognition needs no buffer seen at creation: the creation tee
    // registers only while the feature is live, so a pool the game made before
    // it went live (the feature switched on mid-session) is nominated here, from
    // the very buffer this eye-frame's draws read -- the same filter, the same
    // slot. Its private-copy coverage then starts at the next observed map.
    const bool knownPrimary=watchesPrimaryResource(poolBuf.Get());
    notePrimaryBufferCreated(poolBuf.Get(),pd); // existing buffer on mid-session activation
    if(!knownPrimary && watchesPrimaryResource(poolBuf.Get()))
        Log::get().note("engine motion: primary pool %p nominated at draw frame %u after its initial upload; "
                        "private-copy coverage warms up on its next observed map.",static_cast<void*>(poolBuf.Get()),frame);
    Ptr<ID3D11Buffer> scene;
    ctx->VSGetConstantBuffers(kEngineVelocitySceneSlot, 1, &scene);
    engineVelocityNoteStateCalls(1);
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
        e.poolOutput = {}; e.pool.Reset(); e.poolSrv.Reset();
        D3D11_BUFFER_DESC d = pd;
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
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
        d.ByteWidth = std::max<UINT>(sd.ByteWidth, kStampBytes); d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        for (auto& b : e.scene) if (FAILED(dev->CreateBuffer(&d, nullptr, &b))) {
            for (auto& c : e.scene) c.Reset();
            e.sceneBytes = 0; ++g_draw.createFailed;
            invalidate(e, kCreate);
            return false;
        }
        e.sceneBytes = sd.ByteWidth;
    }
    {
        // The census (issue #38): both copies as one span -- they always run
        // together, back to back, so one timer pair covers the real work
        // instead of paying two pairs' worth of overhead for it.
        GpuCensusScope census(ctx, GpuCensusSection::FrameEngineVelocity);
        ctx->CopyResource(e.pool.Get(), poolBuf.Get());
        engineVelocityNoteStateCalls(1);
        if(timedApply(ctx,e.pool.Get(),poolBuf.Get(),frame,e.poolOutput))++g_primaryApplied;
        // The copy by region, not resource: our buffer can be a float4
        // larger than the game's (the stamp), which CopyResource would
        // reject. The stamp is this eye-frame's present-frame clock -- the
        // same g_frame the emit folded into the markers this window -- so a
        // joined record the engine did not re-evaluate this frame declines
        // to the camera term instead of replaying its stale delta. A boxed
        // UpdateSubresource on a buffer is dropped (WARP no-ops it), so the
        // stamp rides a 16-byte cell: a whole-subresource update, then a
        // boxed copy into the scene constants' shadow.
        ctx->CopySubresourceRegion(e.scene[slot].Get(), 0, 0, 0, 0, scene.Get(), 0, nullptr);
        engineVelocityNoteStateCalls(1);
        if (!e.stampCell) {
            D3D11_BUFFER_DESC cd{};
            cd.ByteWidth = 16; cd.Usage = D3D11_USAGE_DEFAULT;
            if (FAILED(dev->CreateBuffer(&cd, nullptr, &e.stampCell))) {
                ++g_draw.createFailed;
                invalidate(e, kCreate);
                return false;
            }
        }
        // Whole-subresource upload of the 16-byte cell needs a 16-byte SOURCE:
        // &frame alone is four bytes and the null box read twelve past it
        // (rc-since-rc2 review F3). Only the first word is consumed today;
        // the rest is defined zero rather than adjacent stack storage.
        const uint32_t stamp[4] = {frame, 0, 0, 0};
        ctx->UpdateSubresource(e.stampCell.Get(), 0, nullptr, stamp, 16, 0);
        const D3D11_BOX stampBox{0, 0, 0, 16, 1, 1};
        ctx->CopySubresourceRegion(e.scene[slot].Get(), 0, kStampFloat4 * 16u, 0, 0, e.stampCell.Get(), 0, &stampBox);
        engineVelocityNoteStateCalls(2);
    }
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
        engineVelocityNoteStateCalls(1);
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
        engineVelocityNoteStateCalls(1);
        if (scene.Get() != e.sceneBuffer.Get()) { invalidate(e, kSceneRebound); return; }
    }
    const WatchInfo& wp = g_watchInfo[static_cast<unsigned>(eye) * 2u];
    const WatchInfo& ws = g_watchInfo[static_cast<unsigned>(eye) * 2u + 1u];
    if (wp.replaceEpoch != e.poolReplaceEpoch) { invalidate(e, kPoolRewritten); return; }
    if (wp.appendEpoch != e.poolAppendEpoch) {
        const int refreshTimer = beginCapture(ctx, kCaptureRefresh);
        {
            // The census (issue #38): nested inside the existing capture
            // timer above, which is fine -- gpu_census.h -- and counted as
            // its own occurrence since it is a separate call site from
            // snapshot()'s pair.
            GpuCensusScope census(ctx, GpuCensusSection::FrameEngineVelocity);
            ctx->CopyResource(e.pool.Get(), e.poolBuffer.Get());
            engineVelocityNoteStateCalls(1);
            if(timedApply(ctx,e.pool.Get(),e.poolBuffer.Get(),e.frame,e.poolOutput))++g_primaryApplied;
        }
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
        engineVelocityNoteStateCalls(1);
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
    std::array<Ptr<ID3D11RenderTargetView>, 8> held;
    Ptr<ID3D11DepthStencilView> heldDsv;
    // The flat bracket read the game's set when this draw began (flatSaveTargets, itself checked against the context
    // on the sampled frames) and holds it under the generations that say the game has not rebound since: that is what
    // the context holds, with EDVR's own MRT6 on top while it is still bound. Taken from there. The context is asked
    // only when nothing is held, which is the VR path's case always.
    const uint32_t rtvGenNow = bindingGeneration(BindSlot::Rtv0), dsvGenNow = bindingGeneration(BindSlot::Dsv0);
    if (g_flatGame.saved && g_flatGame.rtvGen == rtvGenNow && g_flatGame.dsvGen == dsvGenNow) {
        for (unsigned i = 0; i < 8; ++i) rt[i] = g_flatGame.rtv[i].Get();
        if (g_flatGame.bound) rt[kEngineVelocityTarget] = e.slotsRtv.Get();
        bound = g_flatGame.dsv.Get();
    } else {
        ctx->OMGetRenderTargets(8, rt, &bound);
        engineVelocityNoteStateCalls(1);
        for (unsigned i = 0; i < 8; ++i) held[i].Attach(rt[i]);
        heldDsv.Attach(bound);
    }
    if (bound != dsv) return false;
    if (rt[kEngineVelocityTarget] && rt[kEngineVelocityTarget] != e.slotsRtv.Get()) { ++g_draw.targetOccupied; return false; }
    ID3D11UnorderedAccessView* uav[8] = {};
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, uav);
    engineVelocityNoteStateCalls(1);
    bool anyUav = false;
    for (auto* u : uav) if (u) { anyUav = true; u->Release(); }
    if (anyUav) { ++g_draw.uavBound; return false; }
    if (rt[kEngineVelocityTarget] == e.slotsRtv.Get()) return true;
    const EngineVelocityBindRefusal refusal = engineVelocityValidateTargets(rt, dsv, e.width, e.height);
    if (refusal != EngineVelocityBindRefusal::None) { ++g_draw.bindRefused[static_cast<int>(refusal)]; return false; }
    rt[kEngineVelocityTarget] = e.slotsRtv.Get();
    // All eight slots: whatever the game has at 7 stays bound.
    vScreenSetRenderTargetsRaw(ctx, 8, rt, dsv);
    engineVelocityNoteStateCalls(1);
    // The runtime drops a set it cannot take; then the game's own goes back. It takes or drops a given set the
    // same way every time, so the read-back that says which is made once per binding (the same set under the same
    // generations), and again on the sampled frames to check that.
    const bool remembered = g_flatGame.saved && g_keptMemo.valid && g_keptMemo.rtvGen == rtvGenNow && g_keptMemo.dsvGen == dsvGenNow;
    const FlatQueryPlan plan = remembered ? flatQueryCut().plan(FlatQuery::TargetsKept) : FlatQueryPlan::Ask;
    bool kept = true;
    if (plan != FlatQueryPlan::Shortcut) {
        ID3D11RenderTargetView* now[8] = {};
        ID3D11DepthStencilView* nowDsv = nullptr;
        ctx->OMGetRenderTargets(8, now, &nowDsv);
        engineVelocityNoteStateCalls(1);
        kept = nowDsv == dsv;
        for (unsigned i = 0; i < 8; ++i) {
            if (now[i] != rt[i]) kept = false;
            if (now[i]) now[i]->Release();
        }
        if (nowDsv) nowDsv->Release();
        if (plan == FlatQueryPlan::Sample && flatQueryCut().compared(FlatQuery::TargetsKept, kept)) {
            char line[400];
            flatQueryFallbackLine(line, sizeof(line), FlatQuery::TargetsKept, "the runtime dropped a set it had taken before");
            Log::get().note("%s", line);
        }
    }
    if (!kept) {
        ++g_draw.bindRejected;
        g_keptMemo = KeptMemo{};
        rt[kEngineVelocityTarget] = nullptr;
        vScreenSetRenderTargetsRaw(ctx, 8, rt, dsv);
        engineVelocityNoteStateCalls(1);
        return false;
    }
    // The flat lazy bracket: MRT6 is over the game's saved set from here, whichever way this visit ends (a blend it
    // cannot derive declines the draw after this), so a restore knows to take it off.
    if (g_flatGame.saved) {
        g_flatGame.bound = true;
        g_keptMemo.rtvGen = rtvGenNow;
        g_keptMemo.dsvGen = dsvGenNow;
        g_keptMemo.valid = true;
    }
    return true;
}

// The historical self-marking compatibility path latches the currently bound
// SV_Target6. Its known hashes are EDVR's generated patches, not native game
// writers (2026-09-28 exact-hash proof in engine_velocity_families.h). The PS
// shadow probe must not adopt our installed patch and enter this path as if
// it were game state. Latch for the eye-frame here: R32G32_FLOAT, single-slice,
// at the pass's depth size -- the shape the compose's float2 Load reads. Once per
// eye-frame; nothing about the game's state is touched. True only when the
// texture was latched (first latch of the eye-frame) -- the caller counts it,
// so the family line can tell "drawn" and "actually read at the compose"
// apart.
bool captureGameMark(ID3D11DeviceContext* ctx, Eye& e, int eye, uint32_t frame, ID3D11DepthStencilView* dsv) {
    if (eye == kEngineVelocitySourceEye || e.gameMarkFrame == frame) return false;
    ID3D11RenderTargetView* rts[8] = {};
    Ptr<ID3D11DepthStencilView> dsvNow;
    ctx->OMGetRenderTargets(8, rts, &dsvNow);
    engineVelocityNoteStateCalls(1);
    // OMGet returns owned references: adopt slot 6's, release the rest
    // (the 2026-09-27 review's F2 -- assigning the raw pointer into a smart
    // pointer that addrefs, then releasing around it, leaked one reference per
    // capture attempt).
    Ptr<ID3D11RenderTargetView> rt6;
    rt6.Attach(rts[6]);
    rts[6] = nullptr;
    for (unsigned i = 0; i < 8; ++i)
        if (rts[i]) rts[i]->Release();
    Ptr<ID3D11Texture2D> tex;
    if (rt6) {
        Ptr<ID3D11Resource> res;
        rt6->GetResource(&res);
        if (res) res.As(&tex);
    }
    if (tex) {
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC vd{};
        rt6->GetDesc(&vd);
        Ptr<ID3D11Resource> depthRes;
        dsv->GetResource(&depthRes);
        Ptr<ID3D11Texture2D> depthTex;
        if (depthRes) depthRes.As(&depthTex);
        D3D11_TEXTURE2D_DESC dd{};
        if (depthTex) depthTex->GetDesc(&dd);
        // The shape the compose's float2 Load reads: the VIEW the game writes
        // through is R32G32_FLOAT single-slice at mip 0 (the texture itself may
        // be typeless), at the pass's depth size.
        const bool shapeOk = vd.Format == DXGI_FORMAT_R32G32_FLOAT &&
                             vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && vd.Texture2D.MipSlice == 0 &&
                             td.ArraySize == 1 && td.SampleDesc.Count == 1 && depthTex &&
                             td.Width == dd.Width && td.Height == dd.Height;
        if (shapeOk) {
            if (e.gameMarkSrv && e.gameMark.Get() == tex.Get()) {
                // The same texture as the eye's: reuse the SRV.
            } else {
                // A new channel texture (a resolution/quality change): the old
                // SRV -- and the texture it kept alive -- retires here, not at
                // shutdown (the 2026-09-27 review's F3).
                e.gameMarkSrv.Reset();
                e.gameMark.Reset();
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = DXGI_FORMAT_R32G32_FLOAT;
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                sd.Texture2D.MostDetailedMip = 0;
                Ptr<ID3D11Device> dev;
                ctx->GetDevice(&dev);
                dev->CreateShaderResourceView(tex.Get(), &sd, &e.gameMarkSrv);
                if (!e.gameMarkSrv) return false;
                e.gameMark = tex;
            }
            e.gameMarkFrame = frame;
            return true;
        }
    }
    return false;
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
    // The self-marking detail shaders (kSelfMarking) can draw in the eye's
    // pass without the eye's colour at slot 0 -- rtv0Eye never names those an
    // eye pass. The capture needs only which eye, and the depth probe's own
    // map of the scene pair answers it from the depth view alone.
    const bool selfMarkPs = f >= 0 && selfMarkingPair(vsHash, psHash);
    if (selfMarkPs) ++g_draw.selfMarkSeen;
    int markEye = -1, markTarget = -1;
    if (selfMarkPs && !eyePass && dsv &&
        !(depthProbeCurrentSceneEyeOf(dsv, &markEye, &markTarget) && (markEye == 0 || markEye == 1)))
        markEye = -1;
    const bool selfMarkEyePass = markEye >= 0;
    // On foot no pool draw targets an eye: the source pass is a pool family
    // draw into the depth screen_motion named this frame or the last two.
    bool sourcePass = false;
    if (!eyePass && f >= 0 && dsv && g_sourceDepth && frameNow() - g_sourceNoted <= 2u) {
        Ptr<ID3D11Resource> depthRes;
        dsv->GetResource(&depthRes);
        sourcePass = depthRes.Get() == static_cast<ID3D11Resource*>(g_sourceDepth.Get());
        if (sourcePass) eye = kEngineVelocitySourceEye;
    }
    const bool overlayPair = sourcePass && runtimeFlatProfile() &&
        vsHash == 0xBBE58E40FE88EC80ull && psHash == 0xDB3E8D20CF53FBC0ull;
    // A different pool producer can change MRT6's substrate. Other scene
    // draws cannot write our private target; their depth writes remain guarded
    // by the consumer's exact DSV comparison.
    if (sourcePass && !overlayPair) g_eyes[kEngineVelocitySourceEye].overlayGroup = false;
    auto vsInfo = vs ? g_vs.find(vs) : g_vs.end();
    if (!g_emitLive.load(std::memory_order_acquire) || !(eyePass || sourcePass || selfMarkEyePass) || f < 0 || vsInfo == g_vs.end() ||
        vsInfo->second.family != f) {
        if (selfMarkPs && !eyePass && !selfMarkEyePass) ++g_draw.selfMarkNoEye;
        restore(ctx);
        return;
    }
    deriveFamily(f);
    FamilyState& fam = g_families[f];
    if (!fam.valid) { restore(ctx); return; }
    Eye& e = g_eyes[eyePass || sourcePass ? eye : markEye];
    const uint32_t frame = frameNow();
    // An eye-frame with a recognised pool family draw: the old order prepared
    // on this alone (the 2026-09-23 performance review, item 2).
    if (e.seenFrame != frame) { e.seenFrame = frame; ++g_draw.eyeFramesSeen; }
    // Eligibility FIRST: a keyed pixel shader whose patch exists, and the
    // vertex patch where the family needs one. Nothing -- slot target, clear,
    // snapshot, MRT6 -- is prepared for a draw that cannot export ownership;
    // a declined draw puts the game's state back, as before.
    if (!keyedPs(f, psHash, runtimeFlatProfile())) {
        if (selfMarkPs) {
            // Compatibility path for a marker-bearing shader: latch target
            // 6 without adding a second export. Known hashes are our own
            // generated patches; the probe excludes our installed identity.
            // The eye comes from the pass's colour (eyePass) or, for the detail
            // pass that never binds it, from the depth probe's map of the
            // scene pair (selfMarkEyePass).
            const int which = eyePass ? eye : markEye;
            if (which >= 0) {
                if (captureGameMark(ctx, g_eyes[which], which, frame, dsv)) ++fam.selfMarkedLatched;
            } else {
                ++g_draw.selfMarkNoEye;
            }
            ++fam.selfMarked;
        } else if (!anyKeyedPs(psHash)) {
            ++fam.unkeyedPsDraws;
            fam.unkeyedPsHash = psHash;
            g_unkeyed.note(vsHash, psHash);
        }
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
        e.overlayGroup = false;
        e.rtvGen = e.dsvGen = 0;
        e.bindingStale = false;
        const float cleared[4] = {-1.0f, 0.0f, 0.0f, 0.0f};
        const int clearTimer = beginCapture(ctx, kCaptureClear);
        {
            // The census (issue #38): the eye-frame's clear, its own span --
            // a separate call site from the snapshot below, not merged with it.
            GpuCensusScope census(ctx, GpuCensusSection::FrameEngineVelocity);
            // The flat plane was cleared on its first use this frame. A
            // pre-world foreign marker must survive the source's first draw.
            if(!runtimeFlatProfile())ctx->ClearRenderTargetView(e.slotsRtv.Get(), cleared);
            engineVelocityNoteStateCalls(1);
        }
        endCapture(ctx, clearTimer);
        const int snapTimer = beginCapture(ctx, kCaptureSnapshot);
        snapshot(ctx, e, eye, frame);
        endCapture(ctx, snapTimer);
    }
    if (e.invalid) { restore(ctx); return; }
    // Verify the pool/scene source before copying the substrate. A replaced
    // t33 record makes the whole eye-frame invalid, including its overlay.
    if (!newFrame) {
        checkSources(ctx, e, eye);
        if (e.invalid) { e.overlayGroup = false; restore(ctx); return; }
    }
    bool guardOverlay = false;
    // The overlay guard's base snapshot wants the game's own bindings (no MRT6 over the slot target, no
    // private t3): a run of substituted draws kept bound lazily is put back first, as the per-draw
    // restore always had before this draw. The cache is left alone -- this visit is under way.
    if (overlayPair && g_flatPending.load(std::memory_order_relaxed)) {
        ++g_draw.flatFlushBy[static_cast<unsigned>(EngineVelocityFlushCause::kOverlay)];
        flatRestoreLocked(ctx);
        e.bindingStale = true;   // MRT6 went: the binding below is made again, and needs the depth texture it reads
        if (!depthTex) {
            Ptr<ID3D11Resource> depthRes;
            dsv->GetResource(&depthRes);
            if (!depthRes || FAILED(depthRes.As(&depthTex))) { restore(ctx); return; }
        }
    }
    if (overlayPair) {
        if (!overlayDepthState(ctx)) {
            e.overlayGroup = false;
            ++g_draw.overlayDeclinedState;
        } else {
            ID3D11PixelShader* guarded = guardedOverlayPsFor(ctx, f, ps);
            if (!guarded) {
                e.overlayGroup = false;
                ++g_draw.overlayDeclinedShader;
            } else if ((e.overlayGroup && e.overlayBaseSrv) || snapshotOverlayBase(ctx, e)) {
                e.overlayGroup = true;
                usePs = guarded;
                guardOverlay = true;
            } else {
                e.overlayGroup = false;
                ++g_draw.overlayDeclinedCreate;
            }
        }
    }
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
    if (!e.bound) { e.overlayGroup = false; restore(ctx); return; }
    // The blend state MRT6 must not inherit: the derived one, unless it is
    // still bound (the game has not set another since).
    if (!(g_bound.derivedBlend && bindingGeneration(BindSlot::Blend) == g_bound.blendGen)) {
        Ptr<ID3D11BlendState> game;
        float factor[4] = {};
        UINT mask = 0;
        // The game's state, factor and mask: read from the context, and kept (flat only) under the generation of the
        // game's blend binding. A later run of substituted draws under the same generation has the same state, so it
        // does not read it again; the sampled frames read it as well and compare.
        const uint32_t blendGenNow = bindingGeneration(BindSlot::Blend);
        const bool flat = runtimeFlatProfile() && g_flatGame.saved;   // the lazy bracket's draw, not VR's
        const bool remembered = flat && g_gameBlendMemo.valid && g_gameBlendMemo.gen == blendGenNow;
        const FlatQueryPlan plan = remembered ? flatQueryCut().plan(FlatQuery::GameBlend) : FlatQueryPlan::Ask;
        if (plan == FlatQueryPlan::Shortcut) {
            game = g_gameBlendMemo.state;
            std::memcpy(factor, g_gameBlendMemo.factor, sizeof(factor));
            mask = g_gameBlendMemo.mask;
        } else {
            ctx->OMGetBlendState(&game, factor, &mask);
            engineVelocityNoteStateCalls(1);
            if (game.Get() != bindingGet(BindSlot::Blend)) ++g_draw.blendShadowDisagreed;
            if (plan == FlatQueryPlan::Sample) {
                const bool agree = game.Get() == g_gameBlendMemo.state.Get() && mask == g_gameBlendMemo.mask &&
                                   std::memcmp(factor, g_gameBlendMemo.factor, sizeof(factor)) == 0;
                if (flatQueryCut().compared(FlatQuery::GameBlend, agree)) {
                    char line[400];
                    flatQueryFallbackLine(line, sizeof(line), FlatQuery::GameBlend, "the blend state it held was not the bound one");
                    Log::get().note("%s", line);
                }
            }
            if (flat) {
                g_gameBlendMemo.state = game;
                std::memcpy(g_gameBlendMemo.factor, factor, sizeof(factor));
                g_gameBlendMemo.mask = mask;
                g_gameBlendMemo.gen = blendGenNow;
                g_gameBlendMemo.valid = true;
            }
        }
        const char* refused = nullptr;
        ID3D11BlendState* derived = derivedBlendFor(ctx, game.Get(), &refused);
        if (!derived) {
            ++g_draw.blendRefused;
            g_draw.blendRefusedWhy = refused;
            restore(ctx);
            return;
        }
        vScreenOMSetBlendStateRaw(ctx, derived, factor, mask);
        engineVelocityNoteStateCalls(1);
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
    bool psInstalled = g_bound.patchedPs == usePs && g_bound.originalPs == ps &&
                             bindingGeneration(BindSlot::Ps) == g_bound.psGen;
    const bool vsInstalled = g_bound.patchedVs == useVs && g_bound.originalVs == vs &&
                             bindingGeneration(BindSlot::Vs) == g_bound.vsGen;
    if (useVs != vs) {
        if (vsInstalled) ++g_draw.settersSkipped;
        else { vScreenVSSetShaderRaw(ctx, useVs, nullptr, 0); engineVelocityNoteStateCalls(1); ++g_draw.settersIssued; }
    }
    if (guardOverlay) {
        Ptr<ID3D11ShaderResourceView> gameSrv;
        ctx->PSGetShaderResources(kEngineVelocityOverlaySnapshotSlot, 1, &gameSrv);
        ID3D11ShaderResourceView* privateSrv = e.overlayBaseSrv.Get();
        {
            FlatComputeInternalScope internal;
            ctx->PSSetShaderResources(kEngineVelocityOverlaySnapshotSlot, 1, &privateSrv);
        }
        Ptr<ID3D11ShaderResourceView> actual;
        ctx->PSGetShaderResources(kEngineVelocityOverlaySnapshotSlot, 1, &actual);
        engineVelocityNoteStateCalls(3);
        if (actual.Get() != privateSrv) {
            e.overlayGroup = false;
            ++g_draw.overlayDeclinedCreate;
            ID3D11ShaderResourceView* original = gameSrv.Get();
            {
                FlatComputeInternalScope internal;
                ctx->PSSetShaderResources(kEngineVelocityOverlaySnapshotSlot, 1, &original);
                engineVelocityNoteStateCalls(1);
            }
            usePs = patchedPsFor(ctx, f, ps);
            psInstalled = false;
        } else {
            g_bound.gameSrv3 = gameSrv;
            g_bound.guardSrv3 = e.overlayBaseSrv;
            g_bound.srv3Gen = bindingGeneration(BindSlot::PsSrv3);
            if (++g_draw.overlayGuardedDraws == 1)
                Log::get().note("engine motion: flat overlay guard active at frame %u: exact BBE/DB3E, %ux%u base, "
                                "private PS t3, original depth/stencil state retained.", frame, e.width, e.height);
        }
    }
    if (psInstalled) ++g_draw.settersSkipped;
    else { vScreenPSSetShaderRaw(ctx, usePs, nullptr, 0); engineVelocityNoteStateCalls(1); ++g_draw.settersIssued; }
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
    uint64_t primaryCalls=0,primaryUnowned=0;
    kinematicEvalPrimaryEmitCounters(primaryCalls,primaryUnowned);
    const uint64_t attempts=g_primaryAttempts.load(std::memory_order_relaxed);
    const uint64_t written=g_primaryEmit.itemsJoined.load()+g_primaryEmit.itemsMasked.load();
    Log::get().note("engine motion: primary rigid emit (%s): relay calls %llu, unowned %llu, observer calls %llu, "
                    "joined %llu, moving %llu, masked %llu, declined %llu; disagreements %llu, locate failures %llu, "
                    "read faults %llu, write faults %llu. Zero relay calls means this producer did not run.",
                    kinematicEvalPrimaryEmitStatus(),u(primaryCalls),u(primaryUnowned),u(attempts),
                    r(g_primaryEmit.itemsJoined),r(g_primaryEmit.itemsMoving),r(g_primaryEmit.itemsMasked),
                    u(attempts>=written?attempts-written:0),r(g_primaryEmit.disagreements),r(g_primaryEmit.locateFailures),
                    r(g_primaryEmit.readFaults),r(g_primaryEmit.writeFaults));
    Log::get().note("engine motion: primary private copy cumulative (copier %s, merge %s, clear %s): copier spans %llu, apply successes %llu, "
                    "positive map cache overflow %llu (capacity %u); primary native records are unchanged.",
                    kinematicEvalPoolCopyStatus(),kinematicEvalMergeStatus(),kinematicEvalClearStatus(),r(g_primaryCopyCalls),u(g_primaryApplied),u(g_primaryMapOverflow),kPrimaryPoolResources);
    const auto copyStats=primaryCopy::stats();
    Log::get().note("engine motion: primary copy certificates cumulative: emissions %llu, native copy ranges %llu, joined slots %llu, "
                    "declined %llu, invalidated %llu, overflow %llu; private scatter batches %llu, rows %llu, empty %llu, "
                    "failed %llu (%s). Empty means no certified private rows; zero emissions means no primary record qualified.",
                    u(copyStats.emissions),u(copyStats.copies),u(copyStats.joined),u(copyStats.declined),u(copyStats.invalidated),
                    u(copyStats.overflows),u(copyStats.scatterBatches),u(copyStats.scatterRows),u(copyStats.scatterEmpty),
                    u(copyStats.scatterFailed),primaryCopy::scatterFailureName(copyStats.lastScatterFailure));
    Log::get().note("engine motion: primary copy routing cumulative: source resets %llu, no active map %llu, "
                    "ambiguous map %llu, invalid range %llu, merge plans %llu, merge failures %llu, "
                    "clear calls %llu, cleared claims %llu, clear failures %llu.",
                    u(copyStats.sourceResets),u(copyStats.copierNoLease),u(copyStats.copierAmbiguous),
                    u(copyStats.copierInvalidRange),u(copyStats.mergePlans),u(copyStats.mergeFailed),
                    u(copyStats.clearCalls),u(copyStats.clearedClaims),u(copyStats.clearFailed));
    Log::get().note("engine motion: primary copy observer work cumulative: clear calls %llu, no-claim skips %llu, "
                    "nodes walked %llu; merge calls %llu, started without claims %llu, nodes walked %llu; "
                    "active source claims %llu, detached plans %llu. Skips do not inspect native dictionaries; "
                    "clear failures describe attempted observer work, not skipped calls. Merge walks are retained for clear/unwind safety.",
                    u(copyStats.clearCalls),u(copyStats.clearNoClaims),u(copyStats.clearNodes),
                    u(copyStats.mergeCalls),u(copyStats.mergeWithoutClaims),u(copyStats.mergeNodes),
                    u(copyStats.activeClaims),u(copyStats.activePlans));
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
                    "constants %llu.",
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
                    u(g_draw.refusedPrevious));
    // The draw side's CPU and driver-call figures, on a line of their own (the 2026-09-29 motion-CPU
    // review, C5). They were the tail of the line above, past the 1,160 characters the log keeps, and
    // no log ever showed them. The D3D calls are every Get, Set, clear and copy the draw wrapper asked
    // of the immediate context (engineVelocityNoteStateCalls), so a slow half that costs little of its
    // own but issues eleven driver calls a draw shows here.
    const uint64_t stateCallsWindow = g_stateCalls - g_stateCallsAtWindow;
    Log::get().note("engine motion: draw side over %.0f s, %.0f frames: draw hook slow half %llu calls, %.2f us each, "
                    "~%.3f ms/frame on the caller thread (plus %llu lock-free looks); restores %llu; shader setters "
                    "issued %llu, skipped %llu (ours still bound); D3D context calls the draw wrapper made %llu, "
                    "%.0f a frame.",
                    seconds, double(g_draw.frames), u(g_draw.slowPaths),
                    g_draw.slowPaths ? double(g_draw.slowTicks) * 1e6 / freq / double(g_draw.slowPaths) : 0.0,
                    double(g_draw.slowTicks) * 1e3 / freq / frames, u(g_draw.quickPaths), u(g_draw.restores),
                    u(g_draw.settersIssued), u(g_draw.settersSkipped), u(stateCallsWindow),
                    double(stateCallsWindow) / frames);
    // The flat draw bracket's lazy form (engine_velocity.h): how many substituted draws there were, how many runs of
    // them EDVR's state was bound for, how many draws found it still bound from the one before, why the game's state
    // was put back, and what a substituted draw cost the context in calls. Flat only.
    if (runtimeFlatProfile()) {
        uint64_t substituted = 0;
        for (uint64_t n : familyDraws) substituted += n;
        const auto& by = g_draw.flatFlushBy;
        using Cause = EngineVelocityFlushCause;
        Log::get().note("engine motion: flat draw bracket over %.0f s: %llu substituted draws in %llu runs (%llu found EDVR's state "
                        "still bound from the draw before); the game's state put back %llu times, for: a declined draw %llu, "
                        "another draw %llu, a dispatch %llu, a clear %llu, a copy %llu, a resolve %llu, a set keeping the targets %llu, "
                        "a command list %llu, the present %llu, an overlay draw %llu, not lazy %llu; %.1f context calls per substituted draw.",
                        seconds, u(substituted), u(g_draw.flatRuns), u(g_draw.flatKept),
                        u(by[unsigned(Cause::kDeclined)] + by[unsigned(Cause::kOtherDraw)] + by[unsigned(Cause::kDispatch)] +
                          by[unsigned(Cause::kClear)] + by[unsigned(Cause::kCopy)] + by[unsigned(Cause::kResolve)] +
                          by[unsigned(Cause::kKeepTargets)] + by[unsigned(Cause::kCommandList)] + by[unsigned(Cause::kPresent)] +
                          by[unsigned(Cause::kOverlay)] +
                          by[unsigned(Cause::kEager)]),
                        u(by[unsigned(Cause::kDeclined)]), u(by[unsigned(Cause::kOtherDraw)]), u(by[unsigned(Cause::kDispatch)]),
                        u(by[unsigned(Cause::kClear)]), u(by[unsigned(Cause::kCopy)]), u(by[unsigned(Cause::kResolve)]),
                        u(by[unsigned(Cause::kKeepTargets)]), u(by[unsigned(Cause::kCommandList)]), u(by[unsigned(Cause::kPresent)]),
                        u(by[unsigned(Cause::kOverlay)]),
                        u(by[unsigned(Cause::kEager)]),
                        substituted ? double(stateCallsWindow) / double(substituted) : 0.0);
    }
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
                        "recorded depth is not the pixel's), corrupt slot code %.0f (declined; must be 0), stale stamp %.0f "
                        "(a joined marker from an older frame: the camera term); %llu readbacks.",
                        double(g_draw.pixelsJoined) / double(g_draw.pixelReads), double(g_draw.pixelsMasked) / double(g_draw.pixelReads),
                        double(g_draw.pixelsCamera) / double(g_draw.pixelReads), double(g_draw.pixelsStale) / double(g_draw.pixelReads),
                        double(g_draw.pixelsCorrupt) / double(g_draw.pixelReads), double(g_draw.pixelsStamped) / double(g_draw.pixelReads),
                        u(g_draw.pixelReads));
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
        char pixels[720];
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
                        "record %.0f (camera term), stale slot %.0f, corrupt slot code %.0f (declined; must be 0), "
                        "stale stamp %.0f over %llu counted eye draws%s", mode ? "sampled" : "counted",
                        double(p[0]) / draws, double(p[1]) / draws, double(p[2]) / draws, double(p[3]) / draws,
                        double(p[4]) / draws, double(p[5]) / draws, u(g_draw.panelDraws[mode]), sampleNote);
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
    if (g_draw.overlayCopies || g_draw.overlayGuardedDraws || g_draw.overlayDeclinedState ||
        g_draw.overlayDeclinedCreate || g_draw.overlayDeclinedShader)
        Log::get().note("engine motion: flat overlay guard: copies %llu (%.1f MB), guarded draws %llu, "
                        "declined state %llu, resource %llu, shader %llu.",
                        u(g_draw.overlayCopies), double(g_draw.overlayBytes) / 1e6, u(g_draw.overlayGuardedDraws),
                        u(g_draw.overlayDeclinedState), u(g_draw.overlayDeclinedCreate), u(g_draw.overlayDeclinedShader));
    if (g_draw.selfMarkSeen || g_draw.selfMarkBinds)
        Log::get().note("engine motion: self-marking pixel shaders at the draw path: %llu draws seen, %llu with no "
                        "eye attributable (not the eye's colour at slot 0, and the depth probe named none), %llu "
                        "binds through the PS hook.",
                        u(g_draw.selfMarkSeen), u(g_draw.selfMarkNoEye), u(g_draw.selfMarkBinds));
    if (g_draw.poolShadowHealed)
        Log::get().note("engine motion: the PS shadow was stale on %llu sampled pool draws (the live shader set it "
                        "right, the slow half saw the truth) of %llu probed.",
                        u(g_draw.poolShadowHealed), u(g_draw.poolShadowProbes));
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
        Log::get().note("engine motion: family %s: %s%s%s; substituted %llu binds, %llu draws; patched [%s]%s%s%s%s%s.",
                        kFamilies[f].name, state, s.reason.empty() ? "" : " -- ", s.reason.c_str(),
                        u(s.binds), u(familyDraws[f]), patched.c_str(),
                        failed.empty() ? "" : "; refused [", failed.c_str(), failed.empty() ? "" : "]",
                        s.unkeyedPsDraws ? (" ; unkeyed pixel shader ps_" + hex64(s.unkeyedPsHash) + " left stock").c_str() : "",
                        s.selfMarked ? ("; self-marked " + std::to_string(s.selfMarked) + " draws (the game's own slot+depth channel, latched " +
                                        std::to_string(s.selfMarkedLatched) + " eye-frames)").c_str() : "");
        s.binds = 0;
        s.unkeyedPsDraws = 0;
        s.selfMarked = 0;
        s.selfMarkedLatched = 0;
        g_substitutedBase += familyDraws[f];   // kept for the flat census's drain
        familyDraws[f] = 0;
    }
    g_emit.clear();
    g_primaryEmit.clear(); g_primaryAttempts.store(0,std::memory_order_relaxed);
    g_lastGaps = 0;
    g_draw.clear();
    g_stateCallsAtWindow = g_stateCalls;
    g_windowStartMs = now;
}

// Let every registered pool go: the lock-free watch slots first (the Map and
// Unmap tees stop matching), then the held references. A run of the feature
// starts and ends here through clearLocked, so it holds no game buffer while off.
void releasePrimaryPoolsLocked() {
    for (unsigned i = 0; i < kPrimaryPoolResources; ++i) { primaryPoolResources[i].store(nullptr); g_primaryMaps[i] = {}; }
    g_primaryOverflowNoted = false;
}

void clearLocked() {
    g_flatDomainSaved={};g_flatDomainShaders.clear();g_flatDomainTokenBuffer.Reset();
    for(auto& plane:g_flatMarkerPlanes)plane=FlatMarkerPlane{};
    releasePrimaryPoolsLocked();
    for (auto& e : g_eyes) e = Eye{};
    g_sourceDepth.Reset();
    g_sourceNoted = ~0u;
    g_sourceCamera = SourceCamera{};
    g_sourceDeclineFrame = ~0u;
    for (auto& w : watch) w.store(nullptr);
    for (auto& w : g_watchInfo) w = WatchInfo{};
    for (auto& s : g_families) {
        s.patchedVs.clear(); s.patchedPs.clear(); s.guardedPs.clear(); s.psFailed.clear();
        s.derived = s.valid = false; s.reason.clear(); s.binds = 0; s.unkeyedPsDraws = 0; s.selfMarked = 0; s.selfMarkedLatched = 0;
    }
    for (auto& d : familyDraws) d = 0;
    g_unkeyed.reset();
    // g_bound stays: only the owner thread may put the game's state back
    // (engineVelocityFrameBoundary, g_anyBound says it is owed); it holds its
    // own references to the blend states, so the cache can go.
    g_blends.clear();
    g_gameBlendMemo = GameBlendMemo{};   // a held reference to the game's blend state must not outlive the session
    g_keptMemo = KeptMemo{};
    cache = DrawCache{};
    if (g_table) g_table->clear();
    if (g_census) g_census->clear();
    g_emit.clear();
    g_primaryEmit.clear(); g_primaryAttempts.store(0,std::memory_order_relaxed);
    g_lastGaps = 0;
    g_lastSubstitution = ~0u;
    g_draw.clear();
}

} // namespace engine_velocity_detail

using namespace engine_velocity_detail;

bool engineVelocityActive() noexcept { return live.load(std::memory_order_acquire); }

int engineVelocityFormatUnkeyed(char* out, size_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const int n = g_unkeyed.format(out, size, live.load(std::memory_order_acquire));
    g_unkeyed.reset();
    return n;
}

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
        kinematicEvalSetPrimaryEmitObserver(nullptr);
        kinematicEvalSetPoolCopyObserver(nullptr);
        kinematicEvalSetMergeObserver(nullptr,nullptr);
        kinematicEvalSetClearObserver(nullptr);
    } else {
        g_lookup.store(reinterpret_cast<emit::LookupFn>(base + kLookupRva), std::memory_order_release);
        kinematicEvalSetEmitObserver(&observeEmit);
        kinematicEvalSetPrimaryEmitObserver(&observePrimaryEmit);
        kinematicEvalSetPoolCopyObserver(&observePoolCopy);
        kinematicEvalSetMergeObserver(&observeMergeBegin,&observeMergeEnd);
        kinematicEvalSetClearObserver(&observeDictionaryClear);
    }
    refreshEmitStatus(true);
    Log::get().note("engine motion: primary rigid producer 42B4130 %s; canonical collection-record history, "
                    "unit-scale unskinned records only; unsupported records remain native.",kinematicEvalPrimaryEmitStatus());
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
    kinematicEvalSetPrimaryEmitObserver(nullptr);
    kinematicEvalSetPoolCopyObserver(nullptr);
    kinematicEvalSetMergeObserver(nullptr,nullptr);
    kinematicEvalSetClearObserver(nullptr);
    primaryCopy::reset();   // its leases hold the mapped pools too; clearLocked below releases the cache's
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

// The lazy flat bracket (engine_velocity.h). Owner thread. What is owner-only here (g_flatGame, the cache,
// g_bound's reads) is read without the lock, so a run of draws that changed nothing costs no mutex at all;
// whatever puts state back takes it.
bool engineVelocityFlatDomainBeginDraw(ID3D11DeviceContext* ctx,ID3D11Texture2D* depth,
    const void* vsBytes,size_t vsSize,const void* psBytes,size_t psSize,
    FlatEngineDomain domain,const char** reason,bool coverage,unsigned writerToken,unsigned primitiveCount) {
    const bool pool=domain!=FlatEngineDomain::World;
    auto fail=[&](const char* why){if(reason)*reason=why;return false;};
    if(reason)*reason=nullptr;
    if(!runtimeFlatProfile() || !ctx || !depth ||
       ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return fail("flat-domain-invalid-input");
    if(domain==FlatEngineDomain::ForeignPool) {
        if(!writerToken || writerToken>0xffffffu)return fail("flat-domain-writer-token-unavailable");
        if(!primitiveCount || primitiveCount>0xffffffu)return fail("flat-domain-primitive-overflow");
    }
    engineVelocityFlatFlush(ctx,EngineVelocityFlushCause::kOtherDraw);
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if(g_flatDomainSaved.context)return fail("flat-domain-nested-bracket");
    FlatDomainSaved saved; saved.context=ctx;
    UINT classes=0;ctx->VSGetShader(&saved.vs,nullptr,&classes);if(classes)return fail("flat-domain-VS-linkage");
    ctx->PSGetShader(&saved.ps,nullptr,&classes);if(classes)return fail("flat-domain-PS-linkage");
    if(!saved.vs)return fail("flat-domain-shader-missing");
    if(saved.ps && (!psBytes || !psSize))return fail("flat-domain-PS-bytecode-missing");
    if(saved.ps)try {
        const auto chunks=dxbc_engine_velocity_detail::parseContainer(psBytes,psSize,dxbc_engine_velocity_detail::kPs50);
        for(const auto& chunk:chunks)if(chunk.tag==dxbc_engine_velocity_detail::kTagOsgn)
            for(const auto& output:dxbc_engine_velocity_detail::parseSignature(chunk.bytes))
                if(output.systemValue && output.systemValue!=64)return fail("flat-domain-depth-or-coverage-output");
    } catch(...) {return fail("flat-domain-output-signature");}
    Ptr<ID3D11GeometryShader> gs;Ptr<ID3D11HullShader> hs;Ptr<ID3D11DomainShader> ds;
    ctx->GSGetShader(&gs,nullptr,nullptr);ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&ds,nullptr,nullptr);
    if(gs || hs || ds)return fail("flat-domain-unsupported-shader-stage");
    Ptr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;ctx->GetPredication(&predicate,&predicateValue);
    if(predicate)return fail("flat-domain-predication");
    ID3D11Buffer* so[4]{};ctx->SOGetTargets(4,so);bool busy=false;
    for(auto* p:so)if(p){busy=true;p->Release();}
    if(busy)return fail("flat-domain-stream-output");
    ID3D11UnorderedAccessView* uavs[8]{};
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uavs);
    for(auto* p:uavs)if(p){busy=true;p->Release();}
    if(busy)return fail("flat-domain-uav");
    ID3D11RenderTargetView* targets[8]{};ctx->OMGetRenderTargets(8,targets,&saved.depth);
    for(unsigned i=0;i<8;++i)saved.targets[i].Attach(targets[i]);
    if(!saved.depth || targets[kEngineVelocityTarget])return fail("flat-domain-native-MRT6-or-depth");
    if(coverage && !targets[kFlatOverlayTarget])return fail("flat-domain-coverage-target-missing");
    Ptr<ID3D11Resource> actualDepth;saved.depth->GetResource(&actualDepth);
    if(actualDepth.Get()!=depth)return fail("flat-domain-depth-identity");
    D3D11_TEXTURE2D_DESC desc{};depth->GetDesc(&desc);
    if(desc.SampleDesc.Count!=1 || desc.MipLevels!=1 || desc.ArraySize!=1 ||
       (desc.Format!=DXGI_FORMAT_R32_TYPELESS && desc.Format!=DXGI_FORMAT_R32G8X24_TYPELESS &&
        desc.Format!=DXGI_FORMAT_D32_FLOAT && desc.Format!=DXGI_FORMAT_D32_FLOAT_S8X24_UINT))
        return fail("flat-domain-depth-format");
    auto shader=std::find_if(g_flatDomainShaders.begin(),g_flatDomainShaders.end(),[&](const FlatDomainShader& s){
        return s.originalVs==saved.vs && s.originalPs==saved.ps && s.domain==domain && s.coverage==coverage;});
    if(shader==g_flatDomainShaders.end()) {
        if(g_flatDomainShaders.size()>=64)return fail("flat-domain-shader-cap");
        EngineVelocityInputs inputs{};std::string why;
        if(pool) {
            if(!engineVelocityDeriveInputs(vsBytes,vsSize,inputs,why))return fail("flat-domain-pool-contract");
            if(!saved.ps) {
                uint32_t maximum=0;
                const auto chunks=dxbc_engine_velocity_detail::parseContainer(vsBytes,vsSize,dxbc_engine_velocity_detail::kVs50);
                for(const auto& chunk:chunks)if(chunk.tag==dxbc_engine_velocity_detail::kTagOsgn)
                    for(const auto& output:dxbc_engine_velocity_detail::parseSignature(chunk.bytes))maximum=std::max(maximum,output.registerIndex);
                if(maximum>=31)return fail("flat-domain-null-PS-slot-register");
                inputs.identityRegister=maximum+1;inputs.identityComponent=0;inputs.slotFromVsPatch=true;
            }
        } else {inputs.positionRegister=30;inputs.identityRegister=31;inputs.identityComponent=0;}
        FlatDomainShader entry;entry.originalVs=saved.vs;entry.originalPs=saved.ps;entry.domain=domain;entry.coverage=coverage;
        Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);std::vector<BYTE> bytes;
        const auto kind=domain==FlatEngineDomain::ForeignPool?dxbc_engine_velocity_detail::FlatMarkerKind::ForeignPoolProvenance:
            domain==FlatEngineDomain::WorldPool?dxbc_engine_velocity_detail::FlatMarkerKind::WorldPool:dxbc_engine_velocity_detail::FlatMarkerKind::World;
        if(saved.ps) {
            if(!engineVelocityPatchPs(psBytes,psSize,inputs,bytes,why,false,kind,
                domain==FlatEngineDomain::ForeignPool?&entry.tokenSlot:nullptr))return fail("flat-domain-PS-proof");
        } else {
            const BYTE* code=domain==FlatEngineDomain::ForeignPool?kFlatNullForeignProvenanceMarkerPsBytecode:
                domain==FlatEngineDomain::WorldPool?kFlatNullPoolMarkerPsBytecode:kFlatNullWorldMarkerPsBytecode;
            const size_t size=domain==FlatEngineDomain::ForeignPool?sizeof(kFlatNullForeignProvenanceMarkerPsBytecode):
                domain==FlatEngineDomain::WorldPool?sizeof(kFlatNullPoolMarkerPsBytecode):sizeof(kFlatNullWorldMarkerPsBytecode);
            if(pool)bytes=dxbc_engine_velocity_detail::relocateNullPoolInput(code,size,inputs.identityRegister);
            else bytes.assign(code,code+size);
            if(domain==FlatEngineDomain::ForeignPool)entry.tokenSlot=13;
        }
        if(coverage) {
            std::vector<BYTE> combined;
            if(!flatOverlayPatchPs(bytes.data(),bytes.size(),combined,why))return fail("flat-domain-combined-coverage-proof");
            bytes=std::move(combined);
        }
        t_creating=true;HRESULT hr=dev->CreatePixelShader(bytes.data(),bytes.size(),nullptr,&entry.patchedPs);t_creating=false;
        if(FAILED(hr))return fail("flat-domain-PS-create");
        if(pool && inputs.slotFromVsPatch) {
            if(!engineVelocityPatchVs(vsBytes,vsSize,inputs,bytes,why))return fail("flat-domain-VS-proof");
            t_creating=true;hr=dev->CreateVertexShader(bytes.data(),bytes.size(),nullptr,&entry.patchedVs);t_creating=false;
            if(FAILED(hr))return fail("flat-domain-VS-create");
        }
        g_flatDomainShaders.push_back(std::move(entry));shader=g_flatDomainShaders.end()-1;
    }
    ctx->OMGetBlendState(&saved.blend,saved.factor,&saved.sampleMask);
    const char* blendReason=nullptr;ID3D11BlendState* blend=derivedBlendFor(ctx,saved.blend.Get(),&blendReason,true);
    if(!blend)return fail(blendReason?blendReason:"flat-domain-blend");
    FlatMarkerPlane* plane=flatMarkerPlane(ctx,depth);
    if(!plane)return fail("flat-domain-slot-allocation");
    if(domain==FlatEngineDomain::ForeignPool) {
        if(shader->tokenSlot>=D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
            return fail("flat-domain-token-slot-unavailable");
        if(!g_flatDomainTokenBuffer) {
            Ptr<ID3D11Device> dev;ctx->GetDevice(&dev);D3D11_BUFFER_DESC d{};
            d.ByteWidth=16;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if(FAILED(dev->CreateBuffer(&d,nullptr,&g_flatDomainTokenBuffer)))return fail("flat-domain-token-buffer-allocation");
        }
        saved.tokenSlot=shader->tokenSlot;
        ctx->QueryInterface(IID_PPV_ARGS(&saved.rangedContext));
        if(saved.rangedContext)saved.rangedContext->PSGetConstantBuffers1(saved.tokenSlot,1,&saved.tokenOriginal,&saved.tokenFirst,&saved.tokenCount);
        else ctx->PSGetConstantBuffers(saved.tokenSlot,1,&saved.tokenOriginal);
        const float token[4]={float(writerToken),0,0,0};ctx->UpdateSubresource(g_flatDomainTokenBuffer.Get(),0,nullptr,token,0,0);
        ID3D11Buffer* buffer=g_flatDomainTokenBuffer.Get();ctx->PSSetConstantBuffers(saved.tokenSlot,1,&buffer);
    }
    targets[kEngineVelocityTarget]=plane->rtv.Get();
    vScreenSetRenderTargetsRaw(ctx,8,targets,saved.depth.Get());
    vScreenOMSetBlendStateRaw(ctx,blend,saved.factor,saved.sampleMask);
    if(shader->patchedVs)vScreenVSSetShaderRaw(ctx,shader->patchedVs.Get(),nullptr,0);
    vScreenPSSetShaderRaw(ctx,shader->patchedPs.Get(),nullptr,0);
    g_flatDomainSaved=std::move(saved);
    ID3D11RenderTargetView* verified[8]{};ctx->OMGetRenderTargets(8,verified,nullptr);
    bool bound=verified[kEngineVelocityTarget]==plane->rtv.Get();
    for(unsigned i=0;i<8;++i)if(i!=kEngineVelocityTarget && verified[i]!=g_flatDomainSaved.targets[i].Get())bound=false;
    Ptr<ID3D11PixelShader> heldPs;ctx->PSGetShader(&heldPs,nullptr,nullptr);
    if(heldPs!=shader->patchedPs)bound=false;
    if(g_flatDomainSaved.tokenSlot!=~0u) {
        Ptr<ID3D11Buffer> tokenBound;ctx->PSGetConstantBuffers(g_flatDomainSaved.tokenSlot,1,&tokenBound);
        if(tokenBound!=g_flatDomainTokenBuffer)bound=false;
    }
    for(auto* view:verified)if(view)view->Release();
    if(!bound){engineVelocityFlatDomainEndDraw(ctx);return fail("flat-domain-slot-bind-refused");}
    return true;
}
void engineVelocityFlatDomainEndDraw(ID3D11DeviceContext* ctx) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    auto& s=g_flatDomainSaved;if(!s.context)return;
    if(ctx!=s.context.Get()){s={};return;}
    ID3D11RenderTargetView* targets[8]{};for(unsigned i=0;i<8;++i)targets[i]=s.targets[i].Get();
    vScreenPSSetShaderRaw(ctx,s.ps.Get(),nullptr,0);vScreenVSSetShaderRaw(ctx,s.vs.Get(),nullptr,0);
    vScreenOMSetBlendStateRaw(ctx,s.blend.Get(),s.factor,s.sampleMask);vScreenSetRenderTargetsRaw(ctx,8,targets,s.depth.Get());
    if(s.tokenSlot!=~0u) {
        ID3D11Buffer* original=s.tokenOriginal.Get();
        if(s.rangedContext)s.rangedContext->PSSetConstantBuffers1(s.tokenSlot,1,&original,&s.tokenFirst,&s.tokenCount);
        else ctx->PSSetConstantBuffers(s.tokenSlot,1,&original);
    }
    s={};cache=DrawCache{};
}
bool engineVelocityFlatDomainSlots(ID3D11Texture2D* depth,ID3D11ShaderResourceView** out) {
    if(out)*out=nullptr;std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if(!out || !depth)return false;
    for(const auto& plane:g_flatMarkerPlanes)if(plane.depth.Get()==depth && plane.frame==frameNow() && plane.srv) {
        *out=plane.srv.Get();(*out)->AddRef();return true;
    }
    return false;
}

bool engineVelocityFlatBeginDraw(ID3D11DeviceContext* ctx, bool* gameHadTarget6) {
    const bool wasBound = g_flatPending.load(std::memory_order_relaxed);
    flatSaveTargets(ctx);
    if (gameHadTarget6) *gameHadTarget6 = g_flatGame.rtv[kEngineVelocityTarget] != nullptr;
    engineVelocityBeforeDraw(ctx, false);
    if (!engineVelocityDrawSubstituted()) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        flatFlushLocked(ctx, EngineVelocityFlushCause::kDeclined, wasBound);
        return false;
    }
    if (wasBound) ++g_draw.flatKept; else ++g_draw.flatRuns;
    return true;
}

void engineVelocityFlatEndDraw(ID3D11DeviceContext* ctx) {
    g_flatGame.bound = true;   // the draw was substituted: MRT6 is bound over the game's set, kept or not
    // An overlay guard's private t3 stays bound for its own draw only, and a diagnostic capture wants the
    // game's state back at once: the state goes back now, as it always did.
    if (!g_flatLazy || g_bound.guardSrv3) {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        flatFlushLocked(ctx, EngineVelocityFlushCause::kEager);
        return;
    }
    g_flatPending.store(true, std::memory_order_release);
}

void engineVelocityFlatFlush(ID3D11DeviceContext* ctx, EngineVelocityFlushCause cause) {
    if (!g_flatPending.load(std::memory_order_acquire)) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    flatFlushLocked(ctx, cause);
}

// The frame ends (the runtime's Present, after the flush): what the bracket kept for the frame -- the game's render-target
// set, its blend state, the accepted binding -- goes with it, so no view or state the game replaces stays alive on our account.
void engineVelocityFlatFrameEnd() noexcept {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (g_flatPending.load(std::memory_order_relaxed)) return;   // still bound: the flush is owed first, and does this
    g_flatGame = FlatGame{};
    g_gameBlendMemo = GameBlendMemo{};
    g_keptMemo = KeptMemo{};
}

void engineVelocityFlatAbandon() noexcept {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_bound = Bound{};   // the context lost every binding: nothing of EDVR's is left to put back
    g_anyBound.store(false, std::memory_order_release);
    g_flatGame = FlatGame{};
    g_gameBlendMemo = GameBlendMemo{};
    g_keptMemo = KeptMemo{};
    g_flatPending.store(false, std::memory_order_release);
    g_eyes[kEngineVelocitySourceEye].bindingStale = true;
    cache = DrawCache{};
}

void engineVelocityFlatLazy(bool on) noexcept { g_flatLazy = on; }

// The flat census's once-a-frame drain, owner thread. The substituted draws are the sum of the
// per-family counts plus what the 30 s summary zeroed out of them (g_substitutedBase), so a
// summary between two drains loses none.
EngineVelocityWrapperCounts engineVelocityTakeWrapperCounts() noexcept {
    EngineVelocityWrapperCounts out;
    out.stateCalls = g_stateCalls - g_stateCallsTaken;
    g_stateCallsTaken = g_stateCalls;
    uint64_t substituted = g_substitutedBase;
    for (uint64_t n : familyDraws) substituted += n;
    out.substitutedDraws = substituted - g_substitutedTaken;
    g_substitutedTaken = substituted;
    return out;
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
    // The draw side's CPU time (engine_motion_cpu.h, Part kDraw): the wait for
    // the engine mutex and the slow half, with the pool apply and any lazy
    // shader patch inside it taken out as their own parts. The quick path above
    // is a few compares and is not clocked.
    emcpu::Scope drawSide(emcpu::kDraw);
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (live.load(std::memory_order_acquire)) slowPath(ctx, rtv0Eye);
    g_draw.slowTicks += static_cast<uint64_t>(qpcNow() - t0);
}

// The six tees below are engine motion's CPU on the game's Map/Unmap/write and
// CreateBuffer calls (engine_motion_cpu.h, Part kTee), lock wait included, each
// entered only for a watched resource or an unknown write. The scope goes
// BEFORE the engine mutex, so a stall behind the slow half is counted.
void noteResourceMapped(const ID3D11Resource* resource, void* data, int mapType) noexcept {
    emcpu::Scope tee(emcpu::kTee);
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (auto& w : g_watchInfo)
        if (w.resource == resource) { w.mapped = data; w.mapType = mapType; }
}

void notePrimaryBufferCreated(ID3D11Buffer* buffer,const D3D11_BUFFER_DESC& desc) noexcept {
    if(!buffer || desc.Usage!=D3D11_USAGE_DYNAMIC || desc.StructureByteStride!=336 ||
       !(desc.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || !(desc.CPUAccessFlags&D3D11_CPU_ACCESS_WRITE))return;
    emcpu::Scope tee(emcpu::kTee);
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    // Live only, judged under the lock: the stand-down clears the slots under
    // this same lock, so a CreateBuffer that raced it cannot pin a buffer after
    // the release. The inline caller's unlocked test is only the cheap first cut.
    if(!live.load(std::memory_order_acquire))return;
    for(auto& slot:g_primaryMaps)if(slot.buffer.Get()==buffer)return;
    unsigned chosen=kPrimaryPoolResources;
    for(unsigned i=0;i<kPrimaryPoolResources;++i)if(!g_primaryMaps[i].buffer){chosen=i;break;}
    if(chosen==kPrimaryPoolResources){
        ++g_primaryMapOverflow;
        if(!g_primaryOverflowNoted){
            g_primaryOverflowNoted=true;
            Log::get().note("engine motion: primary pool cache full (%u game buffers held, the most kept): buffer %p "
                            "(%u bytes) seen at present frame %u is not tracked, so its private-copy coverage declines. "
                            "Printed once per run; the 30 s line's \"positive map cache overflow\" keeps counting.",
                            kPrimaryPoolResources,static_cast<void*>(buffer),static_cast<unsigned>(desc.ByteWidth),frameNow());
        }
        return;
    }
    auto& slot=g_primaryMaps[chosen];slot.buffer=buffer;slot.bytes=desc.ByteWidth;slot.lastFrame=frameNow();
    primaryPoolResources[chosen].store(buffer,std::memory_order_release);
}
void notePrimaryResourceMapped(const ID3D11Resource* resource,void* data,int mapType) noexcept {
    emcpu::Scope tee(emcpu::kTee);
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for(auto& slot:g_primaryMaps)if(slot.buffer.Get()==resource) {
        D3D11_BUFFER_DESC desc{};slot.buffer->GetDesc(&desc);
        slot.sequence=++g_primaryMapSequence;slot.lastFrame=frameNow();
        slot.mapped=primaryCopy::beginMap(slot.buffer.Get(),data,desc.ByteWidth,desc.StructureByteStride,
                                        static_cast<D3D11_MAP>(mapType),slot.sequence,frameNow());
        return;
    }
}
void notePrimaryResourceUnknown(const ID3D11Resource* resource) noexcept {
    emcpu::Scope tee(emcpu::kTee);
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for(auto& slot:g_primaryMaps)if(slot.buffer && (!resource || slot.buffer.Get()==resource)) {
        primaryCopy::forget(slot.buffer.Get());slot.mapped=false;
    }
    // A previously patched private snapshot must also stand down: its native
    // source may now have changed without the ordinary owner-context watch.
    for(auto& eye:g_eyes)if(eye.poolBuffer && (!resource || eye.poolBuffer.Get()==resource))invalidate(eye,kPoolRewritten);
    cache=DrawCache{};
}
void notePrimaryResourceWritten(const ID3D11Resource* resource) noexcept {
    emcpu::Scope tee(emcpu::kTee);
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if(!resource){notePrimaryResourceUnknown(nullptr);return;}
    for(auto& slot:g_primaryMaps)if(slot.buffer.Get()==resource) {
        if(slot.mapped){primaryCopy::endMap(slot.buffer.Get(),slot.sequence);slot.mapped=false;}
        else primaryCopy::forget(slot.buffer.Get()); // Copy/Update/unknown mutation
        return;
    }
}

// A write to a watched source: what it changed, for the next substituted
// draw of that eye-frame to judge (checkSources). Nothing is dropped here --
// the other eye's rows go through the same cb1 between the eyes' passes, and
// only a later draw of THIS eye-frame reading changed contents matters.
void noteResourceWrite(const ID3D11Resource* resource) noexcept {
    emcpu::Scope tee(emcpu::kTee);
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

// The seam arc's shadow probe (2026-09-27), retained as a backstop for a
// genuine unobserved bind. Provenance correction, 2026-09-28: the cited
// ps_BCF75CEA37060EAE is our own generated patch of ps_51EE1F922FD220B0.
// Zero hooked binds for it is expected. All live/shadow shader differences
// in the first three 162120 capture frames reproduce our generated hashes;
// those captures supply no evidence of a real bypass. Sampled by the header
// (one quick-pathed pool-context draw in 64): when the live pixel shader is
// neither the shadow's nor our installed patch, the truth is written into
// the shadow and the draw takes the slow half, which
// then reads the real shader. Only ever the owner thread.
void psShadowProbe(ID3D11DeviceContext* ctx) {
    ++g_draw.poolShadowProbes;
    ID3D11PixelShader* livePs = nullptr;
    ctx->PSGetShader(&livePs, nullptr, nullptr);
    engineVelocityNoteStateCalls(1);
    if (!livePs) return;
    // EDVR's own installed substitution is not a bypass (rc-since-rc2 review
    // F7): the shadow correctly holds the game's original and g_bound owns
    // the patch's identity and saved generation. Healing here would adopt
    // the patch as game state, break the generation restore() compares by,
    // and lose the original -- the frame boundary then has nothing to
    // restore with. The pointer compares are free and cover both identities
    // before any registry lookup; EDVR shaders with no registered hash never
    // heal either (the liveHash gate below, as before).
    if (livePs != bindingGet(BindSlot::Ps) && livePs != g_bound.patchedPs) {
        const uint64_t liveHash = lookupShaderHash(livePs);
        if (liveHash != 0) {
            // The truth wins: the bind bypassed the hook (or its memo missed
            // it). The shadow takes the live state, and the draw takes the
            // slow half, which then reads the real shader.
            bindingSetShader(BindSlot::Ps, livePs, liveHash);
            ++g_draw.poolShadowHealed;
            beforeDrawSlow(ctx, cache.eye);
        }
    }
    livePs->Release();
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
        if (g_bound.guardSrv3) {
            Ptr<ID3D11ShaderResourceView> actual;
            ctx->PSGetShaderResources(kEngineVelocityOverlaySnapshotSlot, 1, &actual);
            engineVelocityNoteStateCalls(1);
            if (actual.Get() == g_bound.guardSrv3.Get() &&
                bindingGeneration(BindSlot::PsSrv3) == g_bound.srv3Gen) {
                ID3D11ShaderResourceView* game = g_bound.gameSrv3.Get();
                FlatComputeInternalScope internal;
                ctx->PSSetShaderResources(kEngineVelocityOverlaySnapshotSlot, 1, &game);
                engineVelocityNoteStateCalls(1);
                ++g_draw.restores;
            }
        }
        Ptr<ID3D11PixelShader> ps;
        ctx->PSGetShader(&ps, nullptr, nullptr);
        engineVelocityNoteStateCalls(1);
        if (g_bound.patchedPs && ps.Get() == g_bound.patchedPs) { vScreenPSSetShaderRaw(ctx, g_bound.originalPs, nullptr, 0); engineVelocityNoteStateCalls(1); ++g_draw.restores; }
        Ptr<ID3D11VertexShader> vs;
        ctx->VSGetShader(&vs, nullptr, nullptr);
        engineVelocityNoteStateCalls(1);
        if (g_bound.patchedVs && vs.Get() == g_bound.patchedVs) { vScreenVSSetShaderRaw(ctx, g_bound.originalVs, nullptr, 0); engineVelocityNoteStateCalls(1); ++g_draw.restores; }
        Ptr<ID3D11BlendState> blend;
        float factor[4] = {};
        UINT mask = 0;
        ctx->OMGetBlendState(&blend, factor, &mask);
        engineVelocityNoteStateCalls(1);
        if (g_bound.derivedBlend && blend.Get() == g_bound.derivedBlend.Get()) {
            vScreenOMSetBlendStateRaw(ctx, g_bound.gameBlend.Get(), g_bound.blendFactor, g_bound.sampleMask);
            engineVelocityNoteStateCalls(1);
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
    // The game's own self-marked channel, when a self-marking pair drew this
    // eye-frame: the compose reads it as a fallback beside the slot target.
    if (e.gameMarkFrame == frame && e.gameMarkSrv) {
        out->gameMark = e.gameMarkSrv.Get();
        out->gameMark->AddRef();
    }
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
    return keyedPs(familyForProfile(vsHash, runtimeFlatProfile()), psHash, runtimeFlatProfile());
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

bool engineVelocitySourceIsNamed(const ID3D11Texture2D* depth) {
    if (!depth || !live.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    return g_sourceDepth.Get() == depth && g_sourceNoted == frameNow();
}
bool engineVelocitySourceCameraRows(float (&rows)[6][4]) {
    static_assert(sizeof(rows) == kRowsBytes, "the resolver's camera rows are the watch's rows 270..275");
    if (!live.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const SourceCamera& c = g_sourceCamera;
    if (c.frame != frameNow() || !c.scene || !c.rowsKnown) return false;
    std::memcpy(rows, c.rows, sizeof(rows));
    return true;
}

void engineVelocityNotePanelPixels(uint32_t joined, uint32_t masked, uint32_t camera, uint32_t stale, uint32_t corrupt,
                                   uint32_t stamped, uint32_t eyeDraws, uint32_t pixelStride) {
    if (!live.load(std::memory_order_acquire)) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    const int mode = pixelStride > 1u ? 1 : 0;
    const uint32_t k[6] = {joined, masked, camera, stale, corrupt, stamped};
    for (int i = 0; i < 6; ++i) g_draw.panel[mode][i] += k[i];
    g_draw.panelDraws[mode] += eyeDraws;
}

void engineVelocityNotePixels(uint32_t joined, uint32_t masked, uint32_t camera, uint32_t stale, uint32_t corrupt,
                              uint32_t stamped) {
    if (!live.load(std::memory_order_acquire)) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_draw.pixelsJoined += joined;
    g_draw.pixelsMasked += masked;
    g_draw.pixelsCamera += camera;
    g_draw.pixelsStale += stale;
    g_draw.pixelsCorrupt += corrupt;
    g_draw.pixelsStamped += stamped;
    ++g_draw.pixelReads;
}

void engineVelocityNoteSelfMarkingPs(uint64_t psHash) noexcept {
    // vscreen's PS hook, owner context: the self-marking detail shaders' binds,
    // counted for the draw-path census (the seam arc's live question: do their
    // binds come through the hook at all).
    if (engine_velocity_family::selfMarkingPs(psHash)) ++g_draw.selfMarkBinds;
}

} // namespace edvr
