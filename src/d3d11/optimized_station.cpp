// The optimized station (fix.optimized_stations): inside a station, skip rendering what cannot be seen. While the ship sits in a sealed hangar
// (docked, the pad lowered and still, the hatch closed), everything outside the hangar is skipped, including the sun's light that leaks into it.
// The rules and their geometry are in optimized_station_core.h. The seal, the release and the status lines are here. The engine hooks it uses:
// store_hook.cpp (the store's record judgement, path A), kinematic_eval_hook.cpp (the producer-B and rig vetoes), vscreen.cpp (the sun pair).
#include "optimized_station.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/log.h"
#include "journal_watch.h"
#include "kinematic_eval_hook.h"
#include "optimized_station_core.h"
#include "station_gather_core.h"
#include "station_lockfree_core.h"
#include "station_pass_core.h"
#include "pad_state_probe.h"
#include "store_hook.h"
#include "view_source.h"

namespace edvr {
namespace {

// ---- The frame's state (the store's observations, the own pad, the held cell) --------------------------------------------------------------
std::recursive_mutex g_sealedMutex;        // the lock: the view, the cell, the pads and explicit parts, the rig tables (never held across the game)
std::atomic<uint32_t> g_frameStamp{0};     // advanced at every frame tick: the observer resolves the view once per frame
std::atomic<bool> g_cameraKnown{false};
std::atomic<uint64_t> g_kept{0}, g_keptUnknown{0}, g_culled{0}, g_faults{0};
std::atomic<uint64_t> g_worldSrc[5] = {};               // the named rule's world centre by source (worldOfRecord)
// Built v14.27: the faults by reason (the window's counts; the summary names each). g_winFaults is their sum.
enum FaultReason : size_t { kFaultBucket, kFaultCullRead, kFaultCullRestored, kFaultCullTorn, kFaultTickView, kFaultReasons };
std::atomic<uint64_t> g_winFaultBy[kFaultReasons] = {};
std::atomic<uint64_t> g_winFaults{0};                  // reads or writes that failed in the window
std::atomic<uint64_t> g_winNoCamera{0};                // callbacks that found no camera yet (not faults: the first pass after the gate opens)
void faultNote(FaultReason r) noexcept {
    g_faults.fetch_add(1, std::memory_order_relaxed);
    g_winFaults.fetch_add(1, std::memory_order_relaxed);
    g_winFaultBy[r].fetch_add(1, std::memory_order_relaxed);
}
float g_margin = sealed::kMarginDefaultM;  // the box's margin (fixed; the trial's tuning ladder is removed)
// The sealed rule's sphere radius while no cell is held (the trial's starting radius; the named cell does not use it).
constexpr float kFallbackSphereM = 80.0f;
// Forward: the held cell is localized and the shell picked by setHeldFrame (below), called from the engage.
void setHeldFrame(const sealed::OwnCell& cell, uint32_t frame);
struct RigTally {   // built v14.26: atomic (the callbacks count their vetoes)
    std::atomic<uint64_t> seen{0}, vetoed{0}, mixed{0}, unknown{0}, refresh{0};
    std::atomic<uint64_t> assocSync{0}, assocJob{0}, assocNone{0}, matched{0}, unevaluated{0};
    // Built v14.27: what a rig veto's lookup found in the publication (the window's counts; the cost line names each).
    std::atomic<uint64_t> hitVetoed{0}, hitKept{0}, hitMoved{0}, miss{0}, stale{0}, asked{0};
};
RigTally g_rigTally;
// Built v14.29: the per-call counters of the callbacks are the thread's own, flushed to the shared atomics once per pass the thread works in (the first
// call it makes in a new pass), so no call writes a cache line another thread touches. The summary reads the shared totals (at most one pass per thread
// behind).
struct TlsCounters {
    uint32_t stamp = 0;   // the pass these counts belong to
    uint64_t callsStore = 0, callsProducer = 0, callsRig = 0;
    uint64_t kept = 0, keptUnknown = 0, culled = 0, noCamera = 0;
    uint64_t worldSrc[5] = {};
    uint64_t producerVetoes = 0, rigVetoes = 0;
    uint64_t rigSeen = 0, rigVetoed = 0, rigMixed = 0, rigUnknown = 0, rigRefresh = 0;
    uint64_t rigHitVetoed = 0, rigHitKept = 0, rigHitMoved = 0, rigMiss = 0, rigStale = 0, rigAsked = 0;
    uint32_t sampleStore = 0, sampleProducer = 0, sampleRig = 0;   // the 1-in-32 sampling counters (the thread's own)
};
thread_local TlsCounters tlsTc;
void tcFlush(TlsCounters& t) noexcept;
inline TlsCounters& tcFor(uint32_t stamp) noexcept {
    TlsCounters& t = tlsTc;
    if (t.stamp != stamp) {
        tcFlush(t);
        t.stamp = stamp;
    }
    return t;
}
sealed::OwnCell g_heldCell;               // the sealed cell, in the world frame of the frame that reads it (refreshed each frame)
bool g_heldValid = false;                  // sealed
std::atomic<bool> g_sealedFlag{false};     // mirror of g_heldValid for the draw path and the hooks (atomic: read on the render thread)
bool g_engagePending = false;
uintptr_t g_engagePad = 0;     // built v14.20 (review 1): the pad a pending engage was requested for
uintptr_t g_engageShell = 0;   // built v14.20 (review 1): the shell it was requested against (0: none)
uint32_t g_engageTries = 0;
// Built v14.5: the pad's height above the hangar shell, read live each tick (frame-independent). The shell is picked once per pad.
bool g_relOk = false;
sealed::ShipWatch g_shipWatch;   // the own ship's type prefix, read every tick: a change is a ship change (built v14.6)
float g_relM = 0.0f;
bool g_preShellValid = false;
uintptr_t g_preShellAddr = 0;
uintptr_t g_preShellPad = 0;
char g_preShellName[48] = {};
char g_ownPrefix[32] = {};
char g_ownCandidateName[48] = {};
// Built v14.8: the records within kShipRecordsM of the camera by type prefix, this frame (the ship's part count).
struct PrefixTally {
    char p[32] = {};
    uint32_t n = 0;
};
constexpr size_t kPrefixSlots = 24;
PrefixTally g_prefixTally[kPrefixSlots];
size_t g_prefixTallyN = 0;
float g_ownCandidateDist = -1.0f;
// Built v14.27: the pads and explicit parts are windows over the informative passes (station_pass_core.h): a record stays for kSeenKeep passes after the
// pass that saw it, so a pass that carries only part of a frame's callbacks cannot make the own pad absent. g_pads and g_padCount keep their old names.
sealed::SeenWindow<sealed::PadObs, sealed::kPadSlots> g_padWin;
sealed::PadObs (&g_pads)[sealed::kPadSlots] = g_padWin.items;
size_t& g_padCount = g_padWin.n;
sealed::OwnPadSticky g_ownSticky;   // Built v14 (fix 1): the own pad, kept while its record is seen (reset when it is replaced)
sealed::SeenWindow<sealed::ExplicitObs, sealed::kExplicitSlots> g_explicitWin;
sealed::ExplicitObs (&g_explicits)[sealed::kExplicitSlots] = g_explicitWin.items;
size_t& g_explicitCount = g_explicitWin.n;
float g_frameCam[3] = {};
bool g_frameCamValid = false;
sealed::CamHold g_camHold;                  // built v14.27: the camera is held through a failed resolution
// Built v14.27: the pass rules. g_passInformative is this tick's verdict on the drained passes: complete and at least one store callback in them.
// Everything that reads the passes (the promote, the latch samples, the shell motion sample, the rig publication) runs only on an informative tick.
sealed::PassGate g_passGate;
bool g_passInformative = false;
uint64_t g_passCalls = 0;                   // callbacks the last drained passes held
uint64_t g_winPadSeen = 0, g_winPadMissed = 0, g_winPadAbsentWindow = 0;   // informative passes with / without the sticky pad's record, and without it in the window
uint64_t g_winCamHold = 0, g_winCamJump = 0, g_winCtxChange = 0;           // camera resolutions covered by the hold, jumps over kCamJumpM, context changes
uintptr_t g_lastPassCtx = 0;
uintptr_t g_passCtx = 0;                    // the context the last informative pass's busiest thread saw (the camera is resolved from it)
bool g_engagePublished = false;             // built v14.31: the last published view had an engage pending (every pass gathers the near list, the candidate, the tally)
uint32_t g_recordsLast = 0;                 // store callbacks the last drained passes held (the waiting line's evidence)
bool g_stickyPadInPass = false;
constexpr float kCamJumpM = 2.0f;
sealed::OwnCell g_pickCell;                // the frame's pick before the seal (what the seal rule reads)
sealed::OwnCell g_cellPrev;                // the cell the observer judges against this frame
uintptr_t g_explicitPrev[sealed::kExplicitNodes] = {};
size_t g_explicitPrevCount = 0;
uint32_t g_cellStamp = 0;
uint32_t g_heldAbsent = 0, g_otherPicked = 0;
uint32_t g_viewStamp = 0;
bool g_viewReady = false;
uint32_t g_viewTries = 0;
view::Resolved g_view;
// The pad-local frame and the shell (the release by the pad's motion along the shaft).
sealed::OwnCell g_heldLocal;
bool g_heldLocalValid = false;
float g_engageCentre[3] = {};
float g_engageAxes[3][3] = {};
uintptr_t g_shellAddr = 0;
char g_shellName[48] = {};
bool g_shellValid = false;
sealed::ShellMotion g_shellMotion;        // the pad against its shell: the reference is captured live at the first frame held (built v14.4)
uint32_t g_shellSampleStamp = 0xFFFFFFFFu;// the frame stamp of the last motion sample (the tick and the observer both refresh; one sample a frame)
sealed::SealRateGuard g_sealGuard;        // the flicker safety net: seals in a minute (built v14.4)
float g_frameShiftM = 0.0f;
float g_shellDeltaM = 0.0f;
bool g_releaseMotion = false;
// The own record (the own ship's live sphere) and the near list the engage's own-record pick reads.
struct NearRec {
    uintptr_t addr = 0;
    char name[48] = {};
    float centre[3] = {};
    float radius = 0.0f;
    float dist = 0.0f;
};
constexpr float kOwnNearM = 40.0f;
constexpr float kOwnSphereMarginM = 5.0f;
constexpr size_t kNearSlots = 24;
NearRec g_nearCur[kNearSlots], g_nearPrev[kNearSlots];
size_t g_nearCurN = 0, g_nearPrevN = 0;
uintptr_t g_ownRecAddr = 0;
char g_ownRecName[48] = {};
bool g_ownLiveValid = false;
float g_ownLiveCentre[3] = {};
float g_ownLiveRadius = 0.0f;


// The below distance for the held pad's size (the constants in the core; diag keys are not in the product).
float belowFor(sealed::PadSize size) noexcept {
    return sealed::belowForSize(size, sealed::kBelowDefaultsM, false, 0.0f);
}

bool cameraOf(const view::Resolved& v, float out[3]) noexcept { return view::sphereCentre(v, out); }

// The render context's layout (the same offsets as shadow_gate_core.h): views at +0x40, 0x6A0 bytes each; the view count at +0x1A940.
constexpr uintptr_t kCtxCount = 0x1A940;

constexpr uintptr_t kRecCentre = 0x240;     // float3: the world sphere centre

constexpr uintptr_t kRecMask = 0x208;       // u64: the view mask the store wrote

constexpr uintptr_t kRecAdmitted = 0x234;   // u8: the admitted flag the store wrote

constexpr uintptr_t kRecRadius = 0x280;     // float: the sphere radius

constexpr uintptr_t kRecChildren = 0x2A8;   // pointer: the child list, an array of 8-byte record pointers

constexpr uintptr_t kRecChildCount = 0x2B0; // u64: the child count

constexpr uintptr_t kBucketBase = 0x28;     // the bucket's base record

constexpr uintptr_t kRecName = 0x18;        // pointer: the record's node (node+0 is the text pointer)

constexpr size_t kNodeCacheSlots = 16384;    // built v14.31: the node cache (class, flags and name per node, probed within the table's bound)


constexpr uint32_t kViewTries = 64;

// Built v8.4: the centre measurements of the window (under the lock). rec+0x240 exactly zero; the pose unreadable (the fallback); and the distance
// between rec+0x240 and the pose centre for the records where both are readable and rec+0x240 is not zero (reserved at a toggle-on; a write past
// the reserve is not kept, so the hot path never allocates).
constexpr size_t kCentreDistCap = 8192;

uint64_t g_prodCalls = 0, g_prodVetoed = 0, g_prodKept = 0, g_prodStyle = 0, g_prodUnread = 0;


// The class cache (lock-free): a slot per node, a word per slot (sealed_trial_core.h packs it). Written by any thread that runs the store.
sealed::NodeCache<kNodeCacheSlots> g_nodeCache;   // built v14.31: one entry per node and lifecycle generation (a node's name does not change while it lives)
std::atomic<uint64_t> g_nodeNameReads{0}, g_nodeCacheFull{0}, g_nodeReuseCaught{0};
std::atomic<uint64_t> g_winSunDrawsSkipped{0};   // built v14.34: sun-pair draws skipped in the window (the summary line)   // the window's name reads and uncached reads (the cost line)
std::atomic<uint32_t> g_classGen{0};   // moves at every lifecycle point (bumpRigEpoch): every node's class and name are read afresh

bool readMem(void* dst, uintptr_t src, size_t n) noexcept {
    if (!src) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeMem(uintptr_t dst, const void* src, size_t n) noexcept {
    if (!dst) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(dst), src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool readVal(T* dst, uintptr_t src) noexcept { return readMem(dst, src, sizeof(T)); }

// --- Built v8.4: a record's centre. The builder's centre is computed from the pose it reads, *(rec+0x290): q = pose+0x10, t = pose+0x20, c =
// pose+0x80 (sealed_trial_core.h builderCentre; decomp_42B4420.txt:196-237). rec+0x240 is the fallback when the pose cannot be read (counted).
constexpr uintptr_t kRecPose = 0x290;     // pointer: the builder's rcx, the pose context

constexpr uintptr_t kPoseQuat = 0x10;     // float4: the quaternion x, y, z, w

constexpr uintptr_t kPoseT = 0x20;        // float4: the translation

constexpr uintptr_t kPoseCentre = 0x80;   // float4: the local centre c

struct CentreRead {
    sealed::CentreSource source = sealed::CentreSource::kNone;
    float world[3] = {};      // the centre the rule uses
    float pose[3] = {};       // the builder's centre (valid when poseOk)
    bool poseOk = false;
    float raw[3] = {};        // rec+0x240 (valid when rawOk)
    bool rawOk = false;
    bool rawZero = false;     // rec+0x240 is exactly the origin
    float dist = -1.0f;       // |raw - pose| when both are readable and raw is not zero; else -1
};

// The builder's centre from the pose (SEH-guarded reads only). False when the pose pointer or any of its fields cannot be read.
bool poseCentreOf(uintptr_t rec, float out[3]) noexcept {
    uintptr_t pose = 0;
    if (!readVal(&pose, rec + kRecPose) || !pose) return false;
    float q[4] = {}, t[4] = {}, c[4] = {}, w[4] = {};
    if (!readMem(q, pose + kPoseQuat, sizeof(q)) || !readMem(t, pose + kPoseT, sizeof(t)) ||
        !readMem(c, pose + kPoseCentre, sizeof(c)))
        return false;
    sealed::builderCentre(q, t, c, w);
    out[0] = w[0];
    out[1] = w[1];
    out[2] = w[2];
    return sealed::finite3(out);
}

CentreRead recordCentreRead(uintptr_t rec) noexcept {
    CentreRead r;
    r.poseOk = poseCentreOf(rec, r.pose);
    r.rawOk = readMem(r.raw, rec + kRecCentre, sizeof(r.raw)) && sealed::finite3(r.raw);
    r.rawZero = r.rawOk && sealed::isZeroCentre(r.raw);
    r.source = sealed::chooseCentre(r.poseOk, r.pose, r.rawOk, r.raw, r.world);
    if (r.poseOk && r.rawOk && !r.rawZero) r.dist = sealed::distance(r.pose, r.raw);
    return r;
}

// The store-style world sphere (decomp_4312040.txt lines 212-262, sealed_trial_core.h storeWorldSphere). False when any read fails or the result
// is not finite. Reads only.
bool storeSphereOf(uintptr_t rec, float centre[3], float* radius) noexcept {
    constexpr uintptr_t kNodeOff = 0x18;       // rec+0x18: the node (census's name pointer)
    constexpr uintptr_t kBlockOff = 0x08;      // node+0x08: the bounds block
    constexpr uintptr_t kPoseOff = 0x190;      // rec+0x190: the pose block A
    constexpr uintptr_t kMatrixOff = 0x50;     // A+0x50: the matrix B
    float L[3] = {}, A[15] = {}, B[16] = {};
    float r = 0.0f;
    uintptr_t node = 0, block = 0, pose = 0, matrix = 0;
    bool ok = readVal(&node, rec + kNodeOff) && node && readVal(&block, node + kBlockOff) && block &&
              readMem(L, block + 0x20, sizeof(L)) && readVal(&r, block + 0x2C) && readVal(&pose, rec + kPoseOff) && pose &&
              readMem(A, pose, sizeof(A)) && readVal(&matrix, pose + kMatrixOff) && matrix && readMem(B, matrix, sizeof(B));
    ok = ok && std::isfinite(r) && r > 0.0f && sealed::storeWorldSphere(L, A, B, centre);
    if (ok) *radius = r;
    return ok;
}

// Built v11 (second addendum): a record's local axes in the world. The builder's quaternion (the pose at rec+0x290: q at +0x10, the extent at
// +0x90) when the record has one (path A); else the store-style pose (rec+0x190 and its +0x50 matrix; decomp_4312040.txt). src: 1 builder,
// 2 store-style, 4 neither (false). extOk: the builder extent was read (only the builder pose gives one).
bool poseOrientOf(uintptr_t rec, float axes[3][3], float ext[3], bool* extOk, uint8_t* src) noexcept {
    *extOk = false;
    *src = 4;
    uintptr_t pose = 0;
    float q[4] = {};
    if (readVal(&pose, rec + kRecPose) && pose && readMem(q, pose + 0x10, sizeof(q)) && std::isfinite(q[0]) && std::isfinite(q[1]) &&
        std::isfinite(q[2]) && std::isfinite(q[3])) {
        sealed::quatAxes(q, axes);
        *src = 1;
        if (readMem(ext, pose + 0x90, 3 * sizeof(float)) && std::isfinite(ext[0]) && std::isfinite(ext[1]) && std::isfinite(ext[2])) *extOk = true;
        return true;
    }
    uintptr_t pa = 0, matrix = 0;
    float A[15] = {}, B[16] = {};
    if (readVal(&pa, rec + 0x190) && pa && readMem(A, pa, sizeof(A)) && readVal(&matrix, pa + 0x50) && matrix && readMem(B, matrix, sizeof(B))) {
        sealed::storeAxes(A, B, axes);
        *src = 2;
        return true;
    }
    return false;
}

// The producer B cache: per record, the centre and radius B's veto decision used, in a generation (the frame). Built v14.25: lock-free. Each cell is a
// seqlock (station_lockfree_core.h): an insert claims a cell by compare-and-swap and retries on a contended cell, and a lookup copies the cell and
// checks it. When the bounded probe finds no cell the record runs uncached (counted). The generation advances on the tick, under the lock.
constexpr size_t kProdSlots = 8192;
sealed::SeqCell g_prodCells[kProdSlots];
std::atomic<uint32_t> g_prodGenA{1};   // the generation the cells are valid for (0 is never valid)
uint32_t g_prodStampSeen = 0;          // the tick's: the stamp the generation last moved for

// The producer table's probe statistics, atomic (the producer path writes them without the lock).
struct ProdStats {
    std::atomic<uint32_t> maxProbes{0};
    std::atomic<uint64_t> full{0};
};
ProdStats g_prodStats;

void noteProdProbe(uint32_t probes) noexcept {
    uint32_t m = g_prodStats.maxProbes.load(std::memory_order_relaxed);
    while (probes > m && !g_prodStats.maxProbes.compare_exchange_weak(m, probes, std::memory_order_relaxed)) {
    }
}

// Built v14.20 (review 5): the occupancy and the longest probe of each locked table this window (the 30 s summary reports them and resets the maximum),
// and the lookups or inserts that reached the probe bound (a full table is a miss, never a walk of every slot).
struct TableStats {
    uint32_t used = 0;
    uint32_t maxProbes = 0;
    uint64_t full = 0;
};
TableStats g_collStats;
TableStats g_rigStats;

void noteProbe(TableStats& t, const sealed::ProbeResult& p) noexcept {
    if (p.probes > t.maxProbes) t.maxProbes = p.probes;
    if (p.exhausted) ++t.full;
}

// The tick's: moves the generation when the frame moved on (under the lock).
void prodSync() noexcept {
    const uint32_t s = g_frameStamp.load(std::memory_order_relaxed);
    if (s != g_prodStampSeen) {
        g_prodStampSeen = s;
        g_prodGenA.fetch_add(1, std::memory_order_relaxed);
    }
}

size_t prodIndex(uintptr_t rec) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(rec) >> 4) * 0x9E3779B97F4A7C15ull >> 51) & (kProdSlots - 1);
}

// Lock-free insert for the producer path (any thread), in the generation the snapshot carries.
void prodPutGen(uintptr_t rec, const float c[3], float r, uint32_t gen) noexcept {
    const float v[4] = {c[0], c[1], c[2], r};
    uint32_t probes = 0;
    const bool ok = sealed::seqTableInsert(g_prodCells, kProdSlots, prodIndex(rec), rec, gen, v, &probes);
    noteProdProbe(probes);
    if (!ok) g_prodStats.full.fetch_add(1, std::memory_order_relaxed);   // uncached: a later lookup misses it, as any record the producer never offered
}

// Lookup in the current generation (the locked callers: the world read).
bool prodGet(uintptr_t rec, float c[3], float* r) noexcept {
    float v[4] = {};
    uint32_t probes = 0;
    const bool ok = sealed::seqTableFind(g_prodCells, kProdSlots, prodIndex(rec), rec, g_prodGenA.load(std::memory_order_relaxed), v, &probes);
    noteProdProbe(probes);
    if (!ok) return false;
    c[0] = v[0];
    c[1] = v[1];
    c[2] = v[2];
    *r = v[3];
    return true;
}

// The view the per-record path reads (built v14.25): published by the tick, and by the first store callback of a frame, under the lock. Read by
// pinning the ring; never under the lock. The frame's first callbacks may read the previous frame's view: at most one frame old.
struct PadSnapshot {
    uint32_t frameStamp = 0;
    uint32_t prodGen = 0;
    bool held = false;
    bool camValid = false;
    float cam[3] = {};
    sealed::OwnCell cell;
    size_t explicitCount = 0;
    uintptr_t explicitPrev[sealed::kExplicitNodes] = {};
    char ownPrefix[32] = {};
    bool giveUp = false;          // the hangar watch gave up: the explicit parts are not gathered
    bool latchPending = false;    // a latch is pending: the mechanism candidates are gathered
    bool namesPending = false;    // the hangar names are wanted: the names near the pad are gathered
    float padC[3] = {};           // the pad's centre the names are measured from
    bool ownLiveValid = false;
    float ownLiveCentre[3] = {};
    float ownLiveRadius = 0.0f;
    bool lost = false;            // gathered entries were lost before this publication: the callbacks keep every record
    bool engagePending = false;   // built v14.31: an engage is pending: every pass gathers the near list, the own candidate and the tally
};
sealed::SnapshotRing<PadSnapshot, 40, 32> g_snapRing;

// The published view for one callback (built v14.26: every callback reads the view this way). Built v14.29: the pin is the thread's own hazard cell, held
// until the thread's next call, so a batch of calls on one published view touches no cache line another thread writes; a thread past the last cell
// pins by the shared count and releases it here.
class SnapshotPin {
public:
    SnapshotPin() noexcept : p_(g_snapRing.pinThread(&counted_)) {}
    ~SnapshotPin() {
        if (counted_ != 0) g_snapRing.unpin(counted_ - 1);
    }
    SnapshotPin(const SnapshotPin&) = delete;
    SnapshotPin& operator=(const SnapshotPin&) = delete;
    const PadSnapshot& get() const noexcept { return *p_; }

private:
    size_t counted_ = 0;
    const PadSnapshot* p_;
};

// Built v14.26: what a callback gathers for the tick. Each callback appends to its own thread's slot (sealed::GatherBuffers); the tick drains the entries of
// the previous pass in the order they were written, and applies them to its own tables.
enum class GKind : uint8_t { kNone, kPad, kExplicit, kNear, kOwnCand, kTally, kLatch, kHangar, kRigNote, kRigAssoc, kCost };
constexpr uint8_t kFlagJob = 1;   // kRigNote: the store's bucket path (the rig comes from the bucket's collection, resolved on the tick)
struct GatherEntry {
    GKind kind = GKind::kNone;
    uint8_t flags = 0;            // kPad: the PadSize; kRigNote: kFlagJob; kCost: 0 store, 1 producer, 2 rig
    uint32_t stamp = 0;
    uintptr_t record = 0;
    uintptr_t aux = 0;            // kExplicit: the node; kRigNote: the rig (synchronous path) or the bucket's owner (job path); kRigAssoc: the rig
    uintptr_t bucket = 0;         // kRigNote: the store's bucket
    uintptr_t coll = 0;           // kRigAssoc: the collection the association moved to
    float centre[3] = {};
    float radius = 0.0f;
    float dist = 0.0f;
    float ms = 0.0f;              // kCost: the sampled call's time
    char name[64] = {};
};
constexpr size_t kGatherSlots = 16;
constexpr size_t kGatherCap = 1024;
sealed::PassGather<GatherEntry, kGatherSlots, kGatherCap> g_gather(g_frameStamp);
using GatherWriter = sealed::PassGather<GatherEntry, kGatherSlots, kGatherCap>::Writer;
bool g_lossPublished = false;                // the tick: entries were lost in the pass it last drained (the next publication keeps every record)
uint64_t g_winLost = 0;                      // entries lost in the window (the cost line)
std::atomic<uint64_t> g_lockOnCallback{0};   // state-lock acquisitions made by a callback (must stay 0; the cost line reports it)
thread_local bool tlsOnCallback = false;

// Marks a callback's extent for the lock counter above.
class CallbackScope {
public:
    CallbackScope() noexcept { tlsOnCallback = true; }
    ~CallbackScope() { tlsOnCallback = false; }
    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
};

// A gathered entry with a centre and a name: the callbacks' common shape.
GatherEntry gatherEntry(GKind kind, uintptr_t record, const float c[3], float r, uint32_t stamp, const char* name) noexcept {
    GatherEntry e;
    e.kind = kind;
    e.stamp = stamp;
    e.record = record;
    e.centre[0] = c[0];
    e.centre[1] = c[1];
    e.centre[2] = c[2];
    e.radius = r;
    if (name != nullptr) std::snprintf(e.name, sizeof(e.name), "%s", name);
    return e;
}

// A callback's cost sample, pushed to the tick (no lock). 0 store, 1 producer, 2 rig.
void costPush(uint8_t which, double ms) noexcept {
    GatherEntry e;
    e.kind = GKind::kCost;
    e.flags = which;
    e.ms = static_cast<float>(ms);
    GatherWriter w(g_gather);
    w.push(e);
}

// Built v14.31: the node's class, flags and name come from the shared node cache: the name is read from game memory once per node per lifecycle
// generation (a node's name does not change while the node lives; undock, hangar change, ship change and a setting toggle start every node afresh).
struct NodeInfo {
    sealed::NameClass cls = sealed::NameClass::kUnnamed;
    uint32_t flags = 0;
    uint64_t validator = 0;   // built v14.32: the node's name-text validator, taken on the lookup (0: the text was unreadable)
};

// Built v14.32: the validator of a node's name: the text pointer (node+0) and the first 16 bytes of the text, two guarded reads. A node reallocated within
// a generation holds another name, so another validator, and a cache hit under the old one is refused.
uint64_t nodeValidatorOf(uintptr_t node) noexcept {
    uintptr_t text = 0;
    uint8_t first[16] = {};
    if (!readVal(&text, node) || !text || !readMem(first, text, sizeof(first))) return 0;
    return sealed::nodeValidator(static_cast<uint64_t>(text), first);
}

// Reads a node's name from game memory (guarded) and classifies it; name (64 bytes) is filled when the name is readable.
NodeInfo classifyNode(uintptr_t node, char* name) noexcept {
    NodeInfo info;
    if (name != nullptr) name[0] = '\0';
    uintptr_t text = 0;
    uint8_t bytes[sealed::kNameBytes] = {};
    g_nodeNameReads.fetch_add(1, std::memory_order_relaxed);
    if (!readVal(&text, node) || !text || !readMem(bytes, text, sizeof(bytes))) return info;
    std::string parsed;
    const bool ok = sealed::textName(bytes, sizeof(bytes), false, &parsed) || sealed::textName(bytes, sizeof(bytes), true, &parsed);
    info.cls = sealed::classifyName(ok, parsed.c_str());
    info.flags = ok ? sealed::nodeFlagsOf(parsed.c_str()) : 0u;
    if (ok && name != nullptr) std::snprintf(name, sealed::NodeCache<kNodeCacheSlots>::kNameBytes, "%s", parsed.c_str());
    return info;
}

// The node's info (and name when `name` is given), cached. A node that is not 8-aligned, or a full probe window, is read uncached. A hit is trusted only
// when the node's name-text validator still equals the entry's (one guarded read of the text pointer and 16 bytes); a reused node is classified afresh.
// knownValidator: the validator the caller already took for this node in this call (nullptr: take it here).
NodeInfo nodeInfo(uintptr_t node, char* name, const uint64_t* knownValidator = nullptr) noexcept {
    NodeInfo info;
    const uint32_t gen = g_classGen.load(std::memory_order_relaxed);
    const bool cacheable = sealed::slotFits(node);
    const uint64_t validator = knownValidator != nullptr ? *knownValidator : nodeValidatorOf(node);
    info.validator = validator;
    bool stale = false;
    if (cacheable) {
        NodeInfo hit;
        if (g_nodeCache.find(node, gen, &hit.cls, &hit.flags, name, validator, &stale)) {
            hit.validator = validator;
            return hit;
        }
    }
    if (stale) g_nodeReuseCaught.fetch_add(1, std::memory_order_relaxed);
    char local[sealed::NodeCache<kNodeCacheSlots>::kNameBytes];
    char* const buf = name != nullptr ? name : local;
    const NodeInfo read = classifyNode(node, buf);
    info.cls = read.cls;
    info.flags = read.flags;
    if (cacheable) {
        if (!g_nodeCache.put(node, gen, info.cls, info.flags, buf, validator)) g_nodeCacheFull.fetch_add(1, std::memory_order_relaxed);
    }
    return info;
}

// A node's name; false when it has none or it is unreadable.
bool cachedNameOfNode(uintptr_t node, char* out, size_t cap, const uint64_t* knownValidator = nullptr) noexcept {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!node) return false;
    char name[sealed::NodeCache<kNodeCacheSlots>::kNameBytes];
    const NodeInfo info = nodeInfo(node, name, knownValidator);
    if (info.cls == sealed::NameClass::kUnnamed || name[0] == '\0') return false;
    std::snprintf(out, cap, "%s", name);
    return true;
}

// The record's name: the one name reader on every path (built v14.25; lock-free since v14.26; the shared node cache since v14.31).
bool cachedName(uintptr_t record, char* out, size_t cap) noexcept {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    uintptr_t node = 0;
    if (!record || !readVal(&node, record + kRecName) || !node) return false;
    return cachedNameOfNode(node, out, cap);
}

// The producer-B decision from the published view (built v14.25; was producerDecide under the lock). Caches the record's sphere in the snapshot's
// generation. True skips the record: outside the cell, and not the own ship.
bool producerDecideSnap(uintptr_t rec, const PadSnapshot& s) noexcept {
    float centre[3] = {};
    float radius = 0.0f;
    const bool ok = storeSphereOf(rec, centre, &radius);
    if (ok) prodPutGen(rec, centre, radius, s.prodGen);
    if (!ok || !s.camValid) return false;
    if (sealed::keepNamedCell(centre, radius, s.cam, kFallbackSphereM, s.cell, s.explicitPrev, s.explicitCount, rec).keep) return false;
    char nm[48] = {};
    if (s.ownPrefix[0] != '\0') cachedName(rec, nm, sizeof(nm));
    if (sealed::ownShipExempt(sealed::containsCamera(centre, radius, s.cam), radius, nm, s.ownPrefix)) return false;
    return true;
}

struct WorldRead {
    float world[3] = {};
    float radius = 0.0f;
    uint8_t source = 4;   // 0 producer B, 1 pose, 2 store-style, 3 raw fallback, 4 none
};

// The named rule's world centre and radius for a record (see the block comment above). Lock-free (built v14.26): any thread.
WorldRead worldOfRecord(uintptr_t rec) noexcept {
    WorldRead w;
    if (prodGet(rec, w.world, &w.radius)) {
        w.source = 0;
        return w;
    }
    float p[3] = {};
    if (poseCentreOf(rec, p)) {
        float r = 0.0f;
        if (readVal(&r, rec + kRecRadius) && std::isfinite(r) && r > 0.0f) {
            for (int k = 0; k < 3; ++k) w.world[k] = p[k];
            w.radius = r;
            w.source = 1;
        }
        return w;
    }
    if (storeSphereOf(rec, w.world, &w.radius)) {
        w.source = 2;
        return w;
    }
    float raw[3] = {};
    float r = 0.0f;
    // Built v13.3 (c): a zero raw centre is unreadable, not the origin: no sphere (the record is kept as unknown in named mode).
    if (readMem(raw, rec + kRecCentre, sizeof(raw)) && readVal(&r, rec + kRecRadius) && sealed::rawCentreUsable(raw, r)) {
        for (int k = 0; k < 3; ++k) w.world[k] = raw[k];
        w.radius = r;
        w.source = 3;
    }
    return w;
}

// The class of a node's name, read from game memory with no lock (the census's accept rules). The miss path of the class cache.
// The class and flags of a record's node: one guarded read of the node pointer and the cache lookup.
NodeInfo nodeInfoOf(uintptr_t record, uintptr_t* nodeOut) noexcept {
    *nodeOut = 0;
    uintptr_t node = 0;
    if (!record || !readVal(&node, record + kRecName) || !node) return NodeInfo{};
    *nodeOut = node;
    return nodeInfo(node, nullptr);
}

// Built v14.8: count one record of a type prefix (the records within kShipRecordsM of the camera).
void tallyShipPrefix(const char* name) noexcept {
    char pfx[32] = {};
    sealed::ownPrefixOf(name, pfx, sizeof(pfx));
    if (pfx[0] == '\0') return;
    for (size_t i = 0; i < g_prefixTallyN; ++i) {
        if (std::strcmp(g_prefixTally[i].p, pfx) == 0) {
            ++g_prefixTally[i].n;
            return;
        }
    }
    if (g_prefixTallyN < kPrefixSlots) {
        std::snprintf(g_prefixTally[g_prefixTallyN].p, sizeof(g_prefixTally[g_prefixTallyN].p), "%s", pfx);
        g_prefixTally[g_prefixTallyN].n = 1;
        ++g_prefixTallyN;
    }
}

uint32_t prefixTallyOf(const char* pfx) noexcept {
    for (size_t i = 0; i < g_prefixTallyN; ++i) {
        if (std::strcmp(g_prefixTally[i].p, pfx) == 0) return g_prefixTally[i].n;
    }
    return 0;
}

// Built v13 (fix 1): at each named engage, the own ship\'s type prefix from the candidate record (the one nearest the camera that contains it), logged.
void setOwnPrefixAtEngage(uint32_t frame) {
    (void)frame;
    sealed::ownPrefixOf(g_ownCandidateName, g_ownPrefix, sizeof(g_ownPrefix));
}

// Promotes the previous frame's pads into the cell for the frame now starting, and the explicit instances for that cell. Called under the lock
// at the frame's first record. The pads and explicit parts are the previous frame's; the camera is the previous frame's too.
// Built v13.2 (b): re-reads the own record by address (its centre and radius move with the pad). A record whose name no longer matches is dropped:
// the address was freed or reused, and the rule stands down until the next engage.
void readOwnLive() noexcept {
    g_ownLiveValid = false;
    if (!g_ownRecAddr) return;
    const WorldRead wr = worldOfRecord(g_ownRecAddr);
    char nm[48] = {};
    if (wr.source == 4 || !cachedName(g_ownRecAddr, nm, sizeof(nm)) || !sealed::ownRecordMatch(nm, g_ownRecName)) {
        g_ownRecAddr = 0;
        return;
    }
    if (!std::isfinite(wr.radius) || !(wr.radius > 0.0f)) return;
    std::memcpy(g_ownLiveCentre, wr.world, sizeof(g_ownLiveCentre));
    g_ownLiveRadius = wr.radius;
    g_ownLiveValid = true;
}

// Built v13.2 (b): at a named engage the own record is the one named as the stripped type prefix in the previous frame's near list: containing the
// camera preferred, then the nearest. Logged either way.
void pickOwnRecord(uint32_t frame) {
    (void)frame;
    g_ownRecAddr = 0;
    g_ownLiveValid = false;
    g_ownRecName[0] = '\0';
    char stripped[48] = {};
    if (!sealed::strippedPrefix(g_ownPrefix, stripped, sizeof(stripped))) {
        return;
    }
    int best = -1;
    bool bestIn = false;
    for (size_t i = 0; i < g_nearPrevN; ++i) {
        const NearRec& r = g_nearPrev[i];
        if (!sealed::ownRecordMatch(r.name, stripped)) continue;
        const bool in = r.dist <= r.radius;
        if (best < 0 || (in && !bestIn) || (in == bestIn && r.dist < g_nearPrev[best].dist)) {
            best = static_cast<int>(i);
            bestIn = in;
        }
    }
    if (best < 0) {
        return;
    }
    const NearRec& r = g_nearPrev[best];
    g_ownRecAddr = r.addr;
    std::snprintf(g_ownRecName, sizeof(g_ownRecName), "%s", r.name);
    readOwnLive();
}

// Built v13.7 (a, b): the held cell rebuilt in the held pad's CURRENT frame from its live pose (its centre by worldOfRecord, its axes by
// poseOrientOf), and the pad's height along the shaft against the shell. Called at the frame's promote (before the drop decisions) and at the frame
// tick (before the rig verdicts), so every judgement of a frame uses the cell as the frame's records are expressed. A pose that cannot be read
// this frame keeps the cell as it was. A shell that cannot be read stands the motion check down until the next engage.
void refreshHeldWorld() {
    if (!g_heldValid || !g_heldLocalValid) return;
    const WorldRead pw = worldOfRecord(g_heldCell.pad);
    float axes[3][3] = {};
    float ext[3] = {};
    bool extOk = false;
    uint8_t src = 4;
    if (pw.source == 4 || !poseOrientOf(g_heldCell.pad, axes, ext, &extOk, &src)) return;
    sealed::OwnCell w = g_heldCell;
    sealed::worldizeCell(g_heldLocal, pw.world, axes, &w);
    g_heldCell = w;
    g_cellPrev = w;
    g_frameShiftM = sealed::distance(pw.world, g_engageCentre);
    if (!g_shellValid) return;
    // One sample a frame: the tick and the observer both refresh the held cell.
    const uint32_t shellStamp = g_frameStamp.load(std::memory_order_relaxed);
    if (g_shellSampleStamp == shellStamp) return;
    if (!g_passInformative) return;   // built v14.27: the motion sample is taken on an informative pass only (an empty or unfinished pass is no reading)
    g_shellSampleStamp = shellStamp;
    char nm[48] = {};
    const WorldRead sw = worldOfRecord(g_shellAddr);
    if (sw.source == 4 || !cachedName(g_shellAddr, nm, sizeof(nm)) || std::strcmp(nm, g_shellName) != 0) {
        // Built v14 (fix 2): the reference is not switched to another part; the pad's motion check stands down for this seal.
        g_shellValid = false;
        Log::get().note("optimized station: the reference shell %s is no longer at its record; the pad's motion check stands down (no other shell is taken)", g_shellName);
        return;
    }
    const float up[3] = {g_engageAxes[2][0], g_engageAxes[2][1], g_engageAxes[2][2]};
    const bool first = !g_shellMotion.captured;
    const bool release = g_shellMotion.sample(pw.world, pw.source, sw.world, sw.source, up);
    if (first && g_shellMotion.captured) {
        Log::get().note("optimized station: motion reference read live at the first frame held: the pad sits %.2f m along the shaft above the shell %s "
                        "(centre sources pad %u, shell %u).", static_cast<double>(g_shellMotion.ref), g_shellName,
                        static_cast<unsigned>(pw.source), static_cast<unsigned>(sw.source));
    }
    g_shellDeltaM = g_shellMotion.delta;
    if (release) g_releaseMotion = true;
}

// The record's view mask and admitted flag through the guarded reads and writes, for the fault-safe cull (sealed_trial_core.h).
struct RecordFields {
    uintptr_t rec;
    bool readMask(uint64_t* out) noexcept { return readVal(out, rec + kRecMask); }
    bool readAdmitted(uint8_t* out) noexcept { return readVal(out, rec + kRecAdmitted); }
    bool writeMask(uint64_t v) noexcept { return writeMem(rec + kRecMask, &v, sizeof(v)); }
    bool writeAdmitted(uint8_t v) noexcept { return writeMem(rec + kRecAdmitted, &v, sizeof(v)); }
};

constexpr size_t kRigSlots = 4096;

struct RigSlot {
    uintptr_t rig = 0;               // 0: an empty slot
    uint32_t epoch = 0;              // built v14.20 (review 4): the lifecycle epoch the slot was claimed in; an older one is free and forwards
    uint32_t curStamp = 0;           // the frame stamp the current box was accumulated in
    bool curValid = false;
    float curMn[3] = {}, curMx[3] = {};
    bool lastValid = false;          // the box of the last frame that had records (the one before curStamp)
    float lastMn[3] = {}, lastMx[3] = {};
    bool named = false;
    char name[48] = {};              // the name of the rig's first record (for the own-ship prefix)
    uintptr_t coll = 0;              // the rig's collection (rig+0x348), as last read at a decision
    uint8_t verdict = 0;             // kVerdictUnknown / kVerdictMixed / kVerdictOutside, refreshed once per frame (refreshRigVerdicts)
};

constexpr uint8_t kVerdictUnknown = 0, kVerdictMixed = 1, kVerdictOutside = 2;

RigSlot g_rigSlots[kRigSlots];
// The rig verdicts the tick publishes for the callbacks (built v14.26): the rig veto reads it, with no lock.
sealed::RigPubTable g_rigPub;

// The collection table (Built v12.1): collection -> rig, for the job path's association. Open addressing, written at a decision when a rig's
// collection is read, read per bucket by the store's observer. Under the trial's lock.
constexpr size_t kCollSlots = 1024;

// Built v14.20 (review 4): every association carries the lifecycle epoch it was made in. An association from an earlier epoch is free: a lookup misses
// it and an insert reuses it. The epoch is bumped at undock, hangar change, ship change, setting toggle and an association change (bumpRigEpoch).
uint32_t g_rigEpoch = 1;
std::atomic<uint32_t> g_rigEpochA{1};   // built v14.27: the epoch the rig veto compares a published cell's epoch to (a cell of an older epoch forwards)
uint64_t g_rigEpochs = 0;          // the lifecycle bumps this window (the summary's count)
uint64_t g_winRigPublications = 0, g_winRigPublished = 0;   // built v14.27: publications committed this window, and the rigs they held

struct CollSlot {
    uintptr_t coll = 0;
    uintptr_t rig = 0;
    uint32_t epoch = 0;
};

CollSlot g_collSlots[kCollSlots];

size_t collIndex(uintptr_t coll) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(coll) >> 4) * 0x9E3779B97F4A7C15ull >> 54) & (kCollSlots - 1);
}

void collInsert(uintptr_t coll, uintptr_t rig) noexcept {
    const sealed::ProbeResult p = sealed::boundedProbe(
        collIndex(coll), kCollSlots, sealed::kTableProbeMax,
        [coll](size_t i) { const CollSlot& s = g_collSlots[i]; return s.epoch == g_rigEpoch && s.coll == coll; },
        [](size_t i) { return g_collSlots[i].epoch != g_rigEpoch; });
    noteProbe(g_collStats, p);
    if (p.exhausted) return;   // not associated: the record forwards its rig's own rules, the same as a rig with no association
    CollSlot& s = g_collSlots[p.index];
    if (p.empty) ++g_collStats.used;
    s.coll = coll;
    s.rig = rig;
    s.epoch = g_rigEpoch;
}

uintptr_t collFind(uintptr_t coll) noexcept {
    const sealed::ProbeResult p = sealed::boundedProbe(
        collIndex(coll), kCollSlots, sealed::kTableProbeMax,
        [coll](size_t i) { const CollSlot& s = g_collSlots[i]; return s.epoch == g_rigEpoch && s.coll == coll; },
        [](size_t i) { return g_collSlots[i].epoch != g_rigEpoch; });
    noteProbe(g_collStats, p);
    return p.found ? g_collSlots[p.index].rig : 0;
}

// The job path's one-entry cache per frame: the last store bucket and the rig it resolved to (a bucket's records come in a run).
uintptr_t g_rigBucketCache = 0, g_rigBucketRig = 0;
uintptr_t g_rigBucketColl = 0;   // built v14.24: the collection the cached bucket names (the job path's association check)


uint64_t g_rigNamesLogged = 0;

size_t rigIndex(uintptr_t rig) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(rig) >> 4) * 0x9E3779B97F4A7C15ull >> 52) & (kRigSlots - 1);
}

// Built v14.20 (review 4): claims a slot for a rig in the current epoch: its bounds, its name and its verdict start empty, so the first evaluation
// forwards until a fresh bound exists.
RigSlot* rigClaim(RigSlot& s, uintptr_t rig) noexcept {
    s = RigSlot{};
    s.rig = rig;
    s.epoch = g_rigEpoch;
    ++g_rigStats.used;
    return &s;
}

// Built v14.20 (review 5): a bounded probe. A slot of an earlier epoch is free; a full bound returns null (the rig forwards: its records are not
// bounded, the same as a rig the table never saw).
RigSlot* rigFind(uintptr_t rig, bool insert) noexcept {
    const sealed::ProbeResult p = sealed::boundedProbe(
        rigIndex(rig), kRigSlots, sealed::kTableProbeMax,
        [rig](size_t i) { return g_rigSlots[i].rig == rig; },
        [](size_t i) { const RigSlot& s = g_rigSlots[i]; return s.rig == 0 || s.epoch != g_rigEpoch; });
    noteProbe(g_rigStats, p);
    if (p.found) {
        RigSlot& s = g_rigSlots[p.index];
        return s.epoch == g_rigEpoch ? &s : rigClaim(s, rig);
    }
    if (p.empty && insert) return rigClaim(g_rigSlots[p.index], rig);
    return nullptr;
}

// Built v12.1 (flight fix d), v14.26: the per-frame refresh of every rig's verdict, on the frame tick, from its bound: the box of its records' spheres in
// the last frame that had records. The verdicts are published for the callbacks: the rig veto reads the publication, with no lock. A rig with no bound,
// the own ship's rig, and every rig after a loss of gathered entries are unknown, and forward.
void refreshRigVerdicts() noexcept {
    g_rigPub.begin();
    // No cell held, no verdict is published (the veto answers only while sealed).
    if (!g_heldValid) {
        g_rigPub.commit();
        return;
    }
    const bool allMode = false;
    uint32_t published = 0;
    for (RigSlot& s : g_rigSlots) {
        if (s.rig == 0 || s.epoch != g_rigEpoch) continue;
        const float* mn = nullptr;
        const float* mx = nullptr;
        if (s.curValid) {
            mn = s.curMn;
            mx = s.curMx;
        } else if (s.lastValid) {
            mn = s.lastMn;
            mx = s.lastMx;
        }
        const bool bounded = mn != nullptr;
        float c[3] = {0.0f, 0.0f, 0.0f};
        float r = 0.0f;
        if (bounded) {
            float d2 = 0.0f;
            for (int k = 0; k < 3; ++k) {
                c[k] = 0.5f * (mn[k] + mx[k]);
                const float h = 0.5f * (mx[k] - mn[k]);
                d2 += h * h;
            }
            r = std::sqrt(d2);
        }
        const sealed::RigVerdict v = sealed::rigVerdict(bounded, c, r, g_cellPrev.has, g_cellPrev, allMode, 0);
        // Built v13 (fix 1): the own ship's rig is not vetoed in sphere or named mode: its bound holds the camera with a small radius, or its first
        // record's name has the own-ship prefix. (All mode vetoes it.)
        bool ownRig = false;
        if (!allMode && bounded && g_frameCamValid) ownRig = sealed::containsCamera(c, r, g_frameCam) && r < sealed::kOwnShipRadiusM;
        if (!allMode && !ownRig && g_ownPrefix[0] != '\0') ownRig = sealed::ownShipExempt(false, 0.0f, s.name, g_ownPrefix);
        s.verdict = ownRig ? kVerdictUnknown : sealed::verdictCode(v);   // 2 outside, 1 mixed, 0 unknown
        const uint8_t code = g_lossPublished ? kVerdictUnknown : s.verdict;
        // The run of vetoes the rig has in the committed publication moves into this one (the refresh every kRigRefreshN calls needs it across ticks).
        uint32_t carried = 0;
        uint8_t ov = 0;
        uintptr_t oc = 0;
        size_t oidx = 0;
        if (g_rigPub.lookup(s.rig, rigIndex(s.rig), &ov, &oc, &oidx)) carried = g_rigPub.runAt(oidx);
        size_t idx = 0;
        if (g_rigPub.insert(s.rig, rigIndex(s.rig), code, s.coll, &idx, g_rigEpoch, carried)) ++published;
    }
    g_rigPub.commit();
    ++g_winRigPublications;
    g_winRigPublished += published;
}

// ---- Built v14.24 (2026-10-13): the station's own costs, clocked with QPC and reported in the 30 s summary ---------------------------------------
// The store observer, the producer veto and the rig veto are timed one call in 32 (their p50, p95 and max are per call); the frame tick is timed every
// frame (per frame). The state lock's wait is timed on every acquisition it guards. The samples are the tick's (built v14.26: the callbacks' samples arrive as gathered entries).
int64_t costTicks() noexcept {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return static_cast<int64_t>(q.QuadPart);
}
int64_t costFreq() noexcept {
    static const int64_t f = [] {
        LARGE_INTEGER q{};
        QueryPerformanceFrequency(&q);
        return static_cast<int64_t>(q.QuadPart);
    }();
    return f;
}
double costMs(int64_t a, int64_t b) noexcept { return static_cast<double>(b - a) * 1000.0 / static_cast<double>(costFreq()); }

constexpr size_t kCostSamples = 4096;
struct CostRing {
    double v[kCostSamples] = {};
    size_t n = 0;
    void add(double x) noexcept {
        v[n % kCostSamples] = x;
        ++n;
    }
    size_t used() const noexcept { return n < kCostSamples ? n : kCostSamples; }
    void clear() noexcept { n = 0; }
};
CostRing g_costStore;
CostRing g_costProducer;
CostRing g_costRig;
CostRing g_costFrame;
std::atomic<uint64_t> g_callsStore{0};      // built v14.25: every call (the per-frame counts)
std::atomic<uint64_t> g_callsProducer{0};
std::atomic<uint64_t> g_callsRig{0};

// The mean of a ring's samples (tick only).
double ringMean(const CostRing& r) noexcept {
    const size_t n = r.used();
    if (n == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) sum += r.v[i];
    return sum / static_cast<double>(n);
}

// The producer cache's occupancy: the cells of the current generation (read under the lock, once per summary).
uint32_t prodOccupancy() noexcept {
    const uint32_t gen = g_prodGenA.load(std::memory_order_relaxed);
    uint32_t n = 0;
    for (size_t i = 0; i < kProdSlots; ++i) {
        uintptr_t r = 0;
        uint32_t g = 0;
        float tmp[4];
        if (sealed::seqCellRead(g_prodCells[i], &r, &g, tmp) && g == gen && r != 0) ++n;
    }
    return n;
}
double g_mutexWaitTotalMs = 0.0;
double g_mutexWaitMaxMs = 0.0;
uint64_t g_mutexBlocked = 0;          // acquisitions that waited more than 0.1 ms
uint64_t g_waitingLinesWritten = 0;   // waiting lines written this window (the change-only line)

// The tick's sample (built v14.26: the callbacks push theirs as entries, and the tick adds them here, with no lock).
void costAdd(CostRing& r, double ms) noexcept { r.add(ms); }
bool costSampleCall(uint32_t& calls) noexcept { return ((++calls) & 31u) == 0u; }   // built v14.29: the thread's own counter (no shared line)


// The state lock, with its wait timed. Built v14.26: taken by the tick (and the setting's toggle) only. An acquisition made by a callback is counted
// (the cost line reports the count, which must stay 0).
class TimedStateLock {
public:
    TimedStateLock() noexcept {
        if (tlsOnCallback) g_lockOnCallback.fetch_add(1, std::memory_order_relaxed);
        const int64_t t0 = costTicks();
        g_sealedMutex.lock();
        const double waitMs = costMs(t0, costTicks());
        g_mutexWaitTotalMs += waitMs;
        if (waitMs > g_mutexWaitMaxMs) g_mutexWaitMaxMs = waitMs;
        if (waitMs > 0.1) ++g_mutexBlocked;
    }
    ~TimedStateLock() { g_sealedMutex.unlock(); }
    TimedStateLock(const TimedStateLock&) = delete;
    TimedStateLock& operator=(const TimedStateLock&) = delete;
};

// Built v14.20 (review 4): a lifecycle point (undock, hangar change, ship change, setting toggle, a changed association) moves to a new epoch. Every rig
// slot, association and verdict of the old epoch is free; a rig forwards until it has a fresh bound. Caller holds g_sealedMutex.
void bumpRigEpoch() noexcept {
    ++g_rigEpoch;
    g_rigEpochA.store(g_rigEpoch, std::memory_order_release);
    g_classGen.fetch_add(1, std::memory_order_relaxed);   // built v14.29: every node's class and name are read afresh
    ++g_rigEpochs;
    g_rigStats.used = 0;
    g_collStats.used = 0;
    g_rigBucketCache = 0;
    g_rigBucketRig = 0;
}

// The rig early veto, lock-free (built v14.26). The verdict and the collection come from the tick's publication (RigPubTable). A rig the publication does
// not hold forwards, as a rig the tick never bounded did. A collection that moved since the publication forwards; the tick applies the move, one frame
// behind. The run of vetoes per rig is a compare-and-swap in the table.
bool rigVetoLockFree(uintptr_t rig) noexcept {
    if (!rig) return false;
    SnapshotPin pin;
    if (!pin.get().held) return false;
    const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
    TlsCounters& tc = tcFor(stamp);   // built v14.29: the thread's own counters
    ++tc.rigSeen;
    uint8_t verdict = kVerdictUnknown;
    uintptr_t published = 0;
    size_t idx = 0;
    uint32_t epoch = 0;
    uintptr_t coll = 0;
    const bool haveColl = readVal(&coll, rig + 0x348) && coll != 0;
    if (!g_rigPub.lookup(rig, rigIndex(rig), &verdict, &published, &idx, &epoch)) {
        // Built v14.27: a rig the publication does not hold asks the tick to register it (once per thread per frame). v14.26 only asked for rigs already
        // published, and the tick learns a rig no other way on the job path, so no rig was ever published and the veto never fired.
        ++tc.rigMiss;
        ++tc.rigUnknown;
        if (haveColl) {
            struct Asked {
                uintptr_t rig = 0;
                uint32_t stamp = 0;
            };
            thread_local Asked asked[256];
            Asked& a = asked[(rig >> 4) & 255];
            if (a.rig != rig || a.stamp != stamp) {
                a.rig = rig;
                a.stamp = stamp;
                GatherEntry e;
                e.kind = GKind::kRigAssoc;
                e.aux = rig;
                e.coll = coll;
                GatherWriter w(g_gather);
                if (w.push(e)) ++tc.rigAsked;
            }
        }
        return false;
    }
    if (epoch != g_rigEpochA.load(std::memory_order_acquire)) {
        ++tc.rigStale;   // published in an earlier lifecycle epoch: forwards until the tick republishes
        ++tc.rigUnknown;
        return false;
    }
    if (haveColl && coll != published) {
        g_rigPub.touch(idx, stamp);
        ++tc.rigHitMoved;
        if (g_rigPub.askOnce(idx, stamp)) {
            GatherEntry e;
            e.kind = GKind::kRigAssoc;
            e.aux = rig;
            e.coll = coll;
            GatherWriter w(g_gather);
            w.push(e);
        }
        ++tc.rigUnknown;
        return false;
    }
    switch (g_rigPub.decide(idx, verdict, stamp)) {
        case sealed::RigOutcome::kVetoed:
            ++tc.rigVetoed;
            ++tc.rigHitVetoed;
            return true;
        case sealed::RigOutcome::kRefresh:
            ++tc.rigRefresh;
            ++tc.rigHitKept;
            return false;
        case sealed::RigOutcome::kMixed:
            ++tc.rigMixed;
            ++tc.rigHitKept;
            return false;
        default:
            ++tc.rigUnknown;
            ++tc.rigHitKept;
            return false;
    }
}

// The post-store cull of one record, with its fault handling: true when it was culled. A failed read or write leaves the record as the store wrote
// it (kept, counted as a fault); a torn put-back is counted as a fault too.
bool cullCounted(uintptr_t record, bool containing) noexcept {
    (void)containing;
    RecordFields fields{record};
    sealed::CullField failed = sealed::CullField::kMaskRead;
    uint64_t value = 0;
    switch (sealed::cullRecordFaultSafe(fields, &failed, &value)) {
        case sealed::Cull::kCulled:
            return true;   // the caller counts it (the thread's own counter)
        case sealed::Cull::kUntouched:
            faultNote(kFaultCullRead);
            return false;
        case sealed::Cull::kRestored:
            faultNote(kFaultCullRestored);
            return false;
        case sealed::Cull::kTorn:
            faultNote(kFaultCullTorn);
            return false;
    }
    return false;
}
// ---- The product's state: the setting, the seal, the release, the window counts ----------------------------------------------------------------
std::atomic<bool> g_setting{false};        // fix.optimized_stations is on (read at install and every kSettingPollFrames frames)
bool g_vetoesAttached = false;             // the producer and rig vetoes are attached (under g_installMutex-free single writer: the render tick)
bool g_storeWanted = false;                // the store observer is wanted (the setting is on)
std::atomic<bool> g_dockedNow{false};     // Status.json Flags bit 0 (Docked), read at each tick while known (the store thread reads it)
bool g_dockedWasNow = false;               // built v14.20 (review 4): the docked flag at the last tick (an undock is a lifecycle point)
std::atomic<bool> g_dockedKnownNow{false};// Status.json has a Flags sample (the waiting line says unknown until then)
sealed::GameView g_waitLast;              // the waiting line last written (built v14.23: the game's pad state)
bool g_waitLastSet = false;
sealed::GameView g_waitCand;              // built v14.27: the key this tick had, and how long it has held (the line is written on a stable change)
bool g_waitCandSet = false;
sealed::StableGate g_waitStable;
uint64_t g_waitLastMs = 0;                // when the waiting line was last written (at most every 10 s while docked)
sealed::SealState g_sealState;             // the seal trigger (optimized_station_core.h sealStep)
bool g_flagHangarChanged = false;          // promote saw the held pad go or another own pad nearest: the tick releases on it
// Built v14.13: the sun inside the hangar (its own state, the skip read by the draw), the hangar watch, the names near a pad with no sized shell.
sealed::SunState g_sun;
std::atomic<bool> g_sunOff{false};
sealed::HangarWatch g_hangarWatch;
char g_hangarWhy[160] = "searching for a hangar shell";
struct NameNote {
    char name[48] = {};
    float dist = 0.0f;
    float radius = 0.0f;
};
NameNote g_nameLog[12];
size_t g_nameLogN = 0;
bool g_namesPending = false;
float g_namesPadC[3] = {};
uintptr_t g_hangarPad = 0;
// Built v14.12: the bay's mechanism, latched once per descent (core LatchSet). Candidates are gathered only while a latch is pending.
sealed::LatchSet g_latch;
sealed::LatchIdentity g_latchId;   // built v14.20 (review 2): the pad and shell the latch was built for
// Built v14.24: each latched record's pose at the seal. A release by the mechanism (the Lift, a hatch) re-seals on these poses (all back within the
// tolerance for kPoseBackFrames frames, with r1 1), not only on the r1 edge. Taken when the seal is admitted; the latch is the same set while sealed.
struct SealPoseRec {
    uintptr_t addr = 0;
    char name[48] = {};
    float rel[3] = {};
    float ax0[3] = {};
    float ax2[3] = {};
};
SealPoseRec g_sealPose[sealed::kLatchCap];
size_t g_sealPoseN = 0;
sealed::ChangeGate g_poseAwayGate;   // the away line, written when its text changes
sealed::ChangeGate g_poseBackGate;   // the back line, written once per release
uint64_t g_poseLastLogMs = 0;        // the away line is written at most 2 Hz

void snapshotSealPose() noexcept {
    g_sealPoseN = 0;
    for (size_t i = 0; i < g_latch.n && i < sealed::kLatchCap; ++i) {
        const sealed::LatchRec& r = g_latch.recs[i];
        SealPoseRec& p = g_sealPose[g_sealPoseN++];
        p.addr = r.addr;
        std::snprintf(p.name, sizeof(p.name), "%s", r.name);
        std::memcpy(p.rel, r.rel, sizeof(p.rel));
        std::memcpy(p.ax0, r.ax0, sizeof(p.ax0));
        std::memcpy(p.ax2, r.ax2, sizeof(p.ax2));
    }
}

// Built v14.23: the own pad, from the game (the object the docking reader r0 was last called on this dock). Reset at undock and hangar change.
sealed::GameTrack g_track;
// The pad in the shell's frame this frame (the sealed line's lateral and yaw).
sealed::ShellFrame g_frameShell;
sealed::LatchCand g_latchCands[sealed::kLatchCandCap];
size_t g_latchCandN = 0;
bool g_latchPending = false;         // docked, unsealed, after a descent, not yet latched
bool g_bayWatching = false;          // docked, unsealed, after a descent: the latch is kept
bool g_bayNoting = false;            // watching, and the shell read this frame: the latched records are read
uint64_t g_watchNowMs = 0;
float g_shellNow[3] = {};
uint8_t g_shellNowSrc = 4;
bool g_shellNowValid = false;
bool g_mechanismMoved = false;             // built v14.14: a latched bay mechanism moved while sealed
sealed::MechArm g_mechArm;                 // built v14.33: the mechanism release arms after the latched records were still for 60 informative passes
bool g_flagPadMoved = false;               // promote saw the held pad move along the shaft against the shell: the tick releases on it
char g_releaseWhy[160] = {};               // the release text for the flags above (the pad move or the hangar change, with its names)
uint32_t g_settingPoll = 0;                // frames since the setting was last read
uint64_t g_summaryStartMs = 0;             // the 30 s summary's window
uint64_t g_winFrames = 0;                  // ticks in the window
uint64_t g_winSealedFrames = 0;            // ticks in the window while sealed
uint64_t g_winDockedFrames = 0;            // built v14.31: ticks in the window while docked (the 30 s lines are written only for a window that was docked or sealed)
std::atomic<uint64_t> g_winRigVetoes{0};               // rig vetoes (before the forward) in the window
std::atomic<uint64_t> g_winProducerVetoes{0};          // records the producer-B veto skipped in the window
std::atomic<uint64_t> g_winCulled{0};                  // records culled after the store in the window
std::atomic<uint64_t> g_winKeptUnknown{0};             // records kept because their centre could not be read, in the window


void tcFlush(TlsCounters& t) noexcept {
    g_callsStore.fetch_add(t.callsStore, std::memory_order_relaxed);
    g_callsProducer.fetch_add(t.callsProducer, std::memory_order_relaxed);
    g_callsRig.fetch_add(t.callsRig, std::memory_order_relaxed);
    g_kept.fetch_add(t.kept, std::memory_order_relaxed);
    g_keptUnknown.fetch_add(t.keptUnknown, std::memory_order_relaxed);
    g_winKeptUnknown.fetch_add(t.keptUnknown, std::memory_order_relaxed);
    g_culled.fetch_add(t.culled, std::memory_order_relaxed);
    g_winCulled.fetch_add(t.culled, std::memory_order_relaxed);
    g_winNoCamera.fetch_add(t.noCamera, std::memory_order_relaxed);
    for (int k = 0; k < 5; ++k) g_worldSrc[k].fetch_add(t.worldSrc[k], std::memory_order_relaxed);
    g_winProducerVetoes.fetch_add(t.producerVetoes, std::memory_order_relaxed);
    g_winRigVetoes.fetch_add(t.rigVetoes, std::memory_order_relaxed);
    g_rigTally.seen.fetch_add(t.rigSeen, std::memory_order_relaxed);
    g_rigTally.vetoed.fetch_add(t.rigVetoed, std::memory_order_relaxed);
    g_rigTally.mixed.fetch_add(t.rigMixed, std::memory_order_relaxed);
    g_rigTally.unknown.fetch_add(t.rigUnknown, std::memory_order_relaxed);
    g_rigTally.refresh.fetch_add(t.rigRefresh, std::memory_order_relaxed);
    g_rigTally.hitVetoed.fetch_add(t.rigHitVetoed, std::memory_order_relaxed);
    g_rigTally.hitKept.fetch_add(t.rigHitKept, std::memory_order_relaxed);
    g_rigTally.hitMoved.fetch_add(t.rigHitMoved, std::memory_order_relaxed);
    g_rigTally.miss.fetch_add(t.rigMiss, std::memory_order_relaxed);
    g_rigTally.stale.fetch_add(t.rigStale, std::memory_order_relaxed);
    g_rigTally.asked.fetch_add(t.rigAsked, std::memory_order_relaxed);
    const uint32_t keepStamp = t.stamp;
    const uint32_t s0 = t.sampleStore, s1 = t.sampleProducer, s2 = t.sampleRig;
    t = TlsCounters{};
    t.stamp = keepStamp;
    t.sampleStore = s0;
    t.sampleProducer = s1;
    t.sampleRig = s2;
}
uint32_t g_sealCount = 0;                  // seals since the setting was turned on (for the status lines)

// The sun pass the station skips while sealed (the pair the trial measured: VS 7E38A6AA1269C901 with PS 7CECABDE34FFBE9E).
constexpr uint64_t kSunVs = 0x7E38A6AA1269C901ull;
constexpr uint64_t kSunPs = 0x7CECABDE34FFBE9Eull;

// The pad must be at its lowest seen height, and still to within kSealStillM, for kSealDelayMs before the seal (the hatch closes in this time:
// a guess to be measured in flight; the maintainer will tell us the hatch close time).
// The camera leaves the held box by more than this and the seal is released (riding the elevator down, an external camera flying out).
constexpr uint32_t kSettingPollFrames = 30;
constexpr uint64_t kSummaryMs = 30000;
constexpr uint32_t kEngageTries = 90;
constexpr float kLoweredToleranceM = 15.0f;
constexpr uint32_t kRigRefreshFramesMax = 30;   // (the rig verdict refresh cadence is sealed::kRigRefreshN; kept here for the docs)

const char* sizeWord(sealed::PadSize s) noexcept {
    switch (s) {
        case sealed::PadSize::kSmall: return "small";
        case sealed::PadSize::kMedium: return "medium";
        case sealed::PadSize::kLarge: return "large";
        default: return "unknown-size";
    }
}

// Releases the seal: the held cell, its pad-local frame, the own record and the shell go; the vetoes stop from this frame (they test the
// atomic mirror). One status line says why. The next seal needs the whole seal condition again (still for kSealDelayMs at the lowest height).
// The release's words for the status line, by the rule that released.
const char* releaseText(sealed::ReleaseWhy w) noexcept {
    switch (w) {
        case sealed::ReleaseWhy::kSettingOff: return "the setting is off";
        case sealed::ReleaseWhy::kUndocked: return "undocked";
        case sealed::ReleaseWhy::kCameraLeft: return "the camera left the bay by more than 2 m (riding out of the hangar, or an external camera)";
        case sealed::ReleaseWhy::kPadMoved: return g_releaseWhy;
        case sealed::ReleaseWhy::kHangarChanged: return g_releaseWhy;
        case sealed::ReleaseWhy::kLaunch: return "launch (the pad's state object flipped)";
        case sealed::ReleaseWhy::kMechanism: return g_releaseWhy;
        case sealed::ReleaseWhy::kInHangarLeft: return "the own pad left the hangar (the game's r1 went from 1 to 0)";
        default: return "released";
    }
}

void releaseSeal(const char* why, uint32_t frame) noexcept {
    // Built v14.20 (review 3): the release is a state change: it runs under the state lock, serialized with the store callbacks.
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    if (!g_heldValid) return;
    padStateEvent("release");   // built v14.22: the pad-state probe writes its line at the next tick
    Log::get().note("optimized station: released (frame %u): %s. Everything is drawn as without the station until the seal conditions hold again.",
                    frame, why);
    g_heldValid = false;
    g_heldCell = sealed::OwnCell{};
    g_heldLocalValid = false;
    g_shellValid = false;
    g_releaseMotion = false;
    g_ownRecAddr = 0;
    g_ownLiveValid = false;
    g_heldAbsent = 0;
    g_otherPicked = 0;
    g_engagePending = false;
    sealed::clearTrigger(&g_sealState);   // the pad's motion memory stays: a release by motion makes the next seal wait its delay (built v14.4)
    g_mechArm.reset();
    g_sealedFlag.store(false, std::memory_order_release);
    // the latch is kept across a release (built v14.16): undock, a hangar change and a ship change clear it
}

// Built v14.12: the latch is kept while docked, unsealed, after a descent; it is cleared on a hangar change, a ship change, an undock or a seal.
bool bayWatchKeep() noexcept {
    return g_setting.load(std::memory_order_acquire) &&   // kept through the seal too (built v14.14)
           g_dockedNow.load(std::memory_order_acquire);   // from docking, not only after a descent: a load-in seal latches too (built v14.15)
}

// Built v14.12, v14.26: a gathered record that may be a mechanism record (its name from the cache), within 400 m of the camera. Only while a latch is
// pending (the view says so). The tick applies it.
void latchCandNote(GatherWriter& w, uintptr_t record, uintptr_t node, const float* centre, const PadSnapshot& s, uint32_t flags) noexcept {
    if (!s.latchPending || (flags & sealed::kNodeMechanism) == 0) return;
    if (s.camValid && sealed::distance(centre, s.cam) > 400.0f) return;   // a bay's span reaches ~270 m along the shaft (built v14.15)
    char nm[64] = {};
    if (!cachedNameOfNode(node, nm, sizeof(nm)) || !sealed::mechanismName(nm)) return;
    w.push(gatherEntry(GKind::kLatch, record, centre, 0.0f, 0, nm));
}

// Built v14.28: the latch backstop's last samples (latchRead).
float g_latchLastWorld[sealed::kLatchCap][3] = {};
float g_latchLastShell[3] = {};
uint32_t g_latchLastFrame = 0;
size_t g_latchLastN = 0;
uint64_t g_winOriginShifts = 0;   // the rebases the backstop saw this window

// Built v14.12: latch the bay's mechanism once: the own pad's shaft (its up axis), the pads of its size and the mechanism records near the shaft.
void latchNow(uint64_t now) noexcept {
    float axes[3][3] = {};
    float ext[3] = {};
    bool extOk = false;
    uint8_t src = 4;
    if (!poseOrientOf(g_pickCell.pad, axes, ext, &extOk, &src)) return;
    const float up[3] = {axes[2][0], axes[2][1], axes[2][2]};
    static sealed::LatchCand cands[sealed::kPadSlots + sealed::kLatchCandCap];
    size_t n = 0;
    for (size_t i = 0; i < g_padCount && n < sealed::kPadSlots; ++i) {
        cands[n].addr = g_pads[i].addr;
        std::snprintf(cands[n].name, sizeof(cands[n].name), "%s", g_pads[i].name);
        for (int k = 0; k < 3; ++k) cands[n].centre[k] = g_pads[i].centre[k];
        ++n;
    }
    for (size_t i = 0; i < g_latchCandN; ++i) cands[n++] = g_latchCands[i];
    const float alongMin = -belowFor(g_pickCell.size);
    // The span: the target's distance when the shaft target is built, and never less than the bay's span (built v14.15).
    const float alongMax = g_pickCell.capsule ? std::max(g_pickCell.capLen + 10.0f, sealed::kLatchSpanM) : sealed::kLatchSpanM;
    sealed::LatchReject rej[10];
    size_t rejN = 0;
    const size_t k = sealed::selectMechanism(cands, n, g_pickCell.pad, g_pickCell.padName, g_pickCell.size, g_pickCell.centre, up, alongMin, alongMax,
                                             g_latch.recs, rej, 10, &rejN);
    g_latch.n = k;
    g_latchLastN = 0;   // built v14.28: the backstop's last world positions belong to the old set
    g_latch.latched = true;
    g_latchId.bind(g_pickCell.pad, g_preShellValid ? g_preShellAddr : 0);   // built v14.20 (review 2)
    g_latch.lastMotionMs = 0;
    char names[200] = {};
    size_t used = 0;
    for (size_t i = 0; i < k; ++i) {
        const int w = std::snprintf(names + used, sizeof(names) - used, "%s%s", i > 0 ? ", " : "", g_latch.recs[i].name);
        if (w < 0 || used + static_cast<size_t>(w) >= sizeof(names)) break;
        used += static_cast<size_t>(w);
    }
    Log::get().note("optimized station: watching the bay's mechanism (%u records): %s", static_cast<unsigned>(k), names);
    for (size_t i = 0; i < rejN; ++i) Log::get().note("optimized station: latch refused %s: %s", rej[i].name, rej[i].why);
}

// Built v14.12: each latched record is read by address in the tick (its world centre and orientation), against the shell read this frame. A record
// unreadable this frame is skipped and re-based on the next reading.
// Built v14.28: every record is measured in the hangar SHELL's frame (its centre and its two axes along the shell's own axes), with the shell read in this
// same call. The game's origin shifts (on foot to cockpit) moved every record by one vector (50.85 m) and the latch, which compared world positions and
// read the shell from an earlier part of the tick, released the seal as a launch (flight 2d4ba6db). The shell unreadable: no sample, the references hold.
// Backstop: when the shell and every latched record moved by the same vector since the last sample, it is a rebase: the references are re-based.
void latchRead(uint64_t now) noexcept {
    const uint32_t frame = g_frameStamp.load(std::memory_order_relaxed);
    const uintptr_t shellAddr = g_shellValid ? g_shellAddr : (g_preShellValid ? g_preShellAddr : 0);
    if (shellAddr == 0) return;
    const WorldRead sw = worldOfRecord(shellAddr);
    float sAx[3][3] = {};
    float sExt[3] = {};
    bool sExtOk = false;
    uint8_t sSrc = 4;
    if (sw.source == 4 || !poseOrientOf(shellAddr, sAx, sExt, &sExtOk, &sSrc)) return;   // the shell is unreadable in this pass: no sample
    struct Samp {
        bool ok = false;
        float world[3] = {};
        float rel[3] = {};
        float l0[3] = {};
        float l2[3] = {};
        uint8_t src = 4;
    };
    Samp samp[sealed::kLatchCap];
    bool allOk = true;
    for (size_t i = 0; i < g_latch.n && i < sealed::kLatchCap; ++i) {
        const WorldRead pw = worldOfRecord(g_latch.recs[i].addr);
        float axes[3][3] = {};
        float ext[3] = {};
        bool extOk = false;
        uint8_t src = 4;
        if (pw.source == 4 || !poseOrientOf(g_latch.recs[i].addr, axes, ext, &extOk, &src)) {
            allOk = false;
            continue;
        }
        Samp& s = samp[i];
        s.ok = true;
        s.src = pw.source;
        std::memcpy(s.world, pw.world, sizeof(s.world));
        sealed::shellLocalSample(pw.world, axes[0], axes[2], sw.world, sAx, s.rel, s.l0, s.l2);
    }
    bool shifted = false;
    if (allOk && g_latchLastFrame + 1 == frame && g_latchLastN == g_latch.n && g_latch.n > 0) {
        float deltas[sealed::kLatchCap][3];
        const float sd[3] = {sw.world[0] - g_latchLastShell[0], sw.world[1] - g_latchLastShell[1], sw.world[2] - g_latchLastShell[2]};
        for (size_t i = 0; i < g_latch.n; ++i) {
            for (int k = 0; k < 3; ++k) deltas[i][k] = samp[i].world[k] - g_latchLastWorld[i][k];
        }
        shifted = sealed::commonFrameShift(deltas, g_latch.n, sd, 1.0f, 0.1f);
        if (shifted) {
            ++g_winOriginShifts;
            Log::get().note("optimized station: the coordinate origin shifted by %.2f m (the shell and all %u latched records moved together); the bay's "
                            "motion references are re-based and this is not motion.",
                            static_cast<double>(std::sqrt(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2])), static_cast<unsigned>(g_latch.n));
        }
    }
    g_latchLastN = allOk ? g_latch.n : 0;
    g_latchLastFrame = frame;
    for (size_t i = 0; i < g_latch.n && i < sealed::kLatchCap; ++i) std::memcpy(g_latchLastWorld[i], samp[i].world, sizeof(g_latchLastWorld[i]));
    std::memcpy(g_latchLastShell, sw.world, sizeof(g_latchLastShell));
    for (size_t i = 0; i < g_latch.n && i < sealed::kLatchCap; ++i) {
        sealed::LatchRec& r = g_latch.recs[i];
        if (!samp[i].ok) continue;
        if (shifted) {
            r.stamp = 0;   // the next step re-bases without counting
            r.movedRun = 0;
        }
        r.step(frame, samp[i].rel, samp[i].l0, samp[i].l2, samp[i].src, sw.source, now);
    }
    g_latch.recomputeMotion();
}

// The seal's account for the status line (built v14.23): the game's pad state said the own pad was in the hangar (r1 1). The codes are the last
// read at the seal (the seal line prints them with the own pad's key).
const char* engageHow() noexcept {
    static char how[160];
    std::snprintf(how, sizeof(how), "sealed on the game's pad state: own pad r1 1 (in the hangar)");
    return how;
}

// Built v14.13, v14.26: a gathered record that may name the hangar, near the pad, while the names are wanted (the view says so). The tick applies it.
void hangarNameNote(GatherWriter& w, uintptr_t record, uintptr_t node, const float* centre, float radius, const PadSnapshot& s, uint32_t flags) noexcept {
    if (!s.namesPending || (flags & sealed::kNodeHangarLike) == 0) return;
    const float d = sealed::distance(centre, s.padC);
    if (d > 150.0f) return;
    char nm[64] = {};
    if (!cachedNameOfNode(node, nm, sizeof(nm)) || !sealed::hangarLikeName(nm)) return;
    GatherEntry e = gatherEntry(GKind::kHangar, record, centre, radius, 0, nm);
    e.dist = d;
    w.push(e);
}

// Built v14.13: the once-per-dock note with no sized shell: the hangar-like and pad records near the pad (the first carrier dock tells us its
// names), or, when there are none, that there is no hangar here.
void hangarNote() noexcept {
    if (g_nameLogN == 0) {
        std::snprintf(g_hangarWhy, sizeof(g_hangarWhy), "%s", "no hangar here (outpost or open pad): nothing to do");
        Log::get().note("optimized station: no hangar here (outpost or open pad): nothing to do");
        return;
    }
    std::snprintf(g_hangarWhy, sizeof(g_hangarWhy), "%s", "no sized hangar shell matched near the pad (names logged)");
    char text[400] = {};
    size_t used = 0;
    for (size_t i = 0; i < g_nameLogN; ++i) {
        const int w = std::snprintf(text + used, sizeof(text) - used, "%s%s (%.1f m, r %.1f m)", i > 0 ? "; " : "", g_nameLog[i].name,
                                    static_cast<double>(g_nameLog[i].dist), static_cast<double>(g_nameLog[i].radius));
        if (w < 0 || used + static_cast<size_t>(w) >= sizeof(text)) break;
        used += static_cast<size_t>(w);
    }
    Log::get().note("optimized station: no sized hangar shell near the pad; the hangar-like and pad records near it: %s", text);
}

// Built v14.17: a log value with two decimals, or "n/a" when it is unreadable.
void numOrNa(char* out, size_t cap, bool ok, float v) noexcept {
    if (ok) std::snprintf(out, cap, "%.2f", static_cast<double>(v));
    else std::snprintf(out, cap, "n/a");
}

// The seal's status line, once per seal (called from the engage, after the cell is held and localized).
void sealStatus(const sealed::OwnCell& cell, uint32_t frame) noexcept {
    ++g_sealCount;
    padStateEvent("seal");   // built v14.22: the pad-state probe writes its line at the next tick
    char lat[24] = {};
    char yaw[24] = {};
    numOrNa(lat, sizeof(lat), g_frameShell.ok, g_frameShell.lateral);   // the last frame's shell frame (built v14.17)
    numOrNa(yaw, sizeof(yaw), g_frameShell.yawOk, g_frameShell.yawDeg);
    Log::get().note("optimized station: sealed in %s hangar (%s, frame %u, seal %u): pad %s, box %.0f x %.0f x %.0f m (length, width, height), "
                    "floor %.0f m under the pad, shell %s, lateral %s m, yaw %s deg.", sizeWord(cell.size), engageHow(),
                    frame, g_sealCount, cell.padName,
                    static_cast<double>(cell.boxHalf[0] * 2.0f), static_cast<double>(cell.boxHalf[1] * 2.0f),
                    static_cast<double>(cell.boxHalf[2] * 2.0f), static_cast<double>(belowFor(cell.size)),
                    g_shellValid ? g_shellName : "none (the pad's motion is not checked)", lat, yaw);
}

// The 30 s summary: what the station did in the window. A user's log shows the station's work here.
void summaryLine(uint64_t now) noexcept {
    // Built v14.31: the lines are written for a window in which the commander was docked or sealed (a flight in space writes none); the window resets either way.
    const bool quiet = g_winDockedFrames == 0 && g_winSealedFrames == 0;
#define SUMMARY_NOTE(...) \
    do {                  \
        if (!quiet) Log::get().note(__VA_ARGS__); \
    } while (0)
    SUMMARY_NOTE("optimized station: last %llu s: sealed for %llu of %llu frames; rigs vetoed early %llu, records vetoed early %llu, "
                    "records culled after the store %llu, records kept unread %llu, faults %llu (unreadable bucket %llu, cull read %llu, cull restored "
                    "%llu, cull torn %llu, tick view %llu; callbacks before the first camera %llu, not faults); sun draws skipped %llu.",
                    static_cast<unsigned long long>((now - g_summaryStartMs) / 1000), static_cast<unsigned long long>(g_winSealedFrames),
                    static_cast<unsigned long long>(g_winFrames), static_cast<unsigned long long>(g_winRigVetoes),
                    static_cast<unsigned long long>(g_winProducerVetoes), static_cast<unsigned long long>(g_winCulled),
                    static_cast<unsigned long long>(g_winKeptUnknown), static_cast<unsigned long long>(g_winFaults.load()),
                    static_cast<unsigned long long>(g_winFaultBy[kFaultBucket].load()), static_cast<unsigned long long>(g_winFaultBy[kFaultCullRead].load()),
                    static_cast<unsigned long long>(g_winFaultBy[kFaultCullRestored].load()),
                    static_cast<unsigned long long>(g_winFaultBy[kFaultCullTorn].load()),
                    static_cast<unsigned long long>(g_winFaultBy[kFaultTickView].load()), static_cast<unsigned long long>(g_winNoCamera.load()),
                    static_cast<unsigned long long>(g_winSunDrawsSkipped.exchange(0, std::memory_order_relaxed)));
    // Built v14.27: the passes the tick read, and what it did with them; the rig veto's lookups. A dead veto or a flapping pad shows here.
    {
        const sealed::PassGate& g = g_passGate;
        const double avg = g.informative > 0 ? static_cast<double>(g.callsSum) / static_cast<double>(g.informative) : 0.0;
        SUMMARY_NOTE("optimized station: last %llu s passes: %llu informative, %llu empty, %llu unfinished (a writer was still inside), %llu thin (under "
                        "half the usual calls); callbacks per informative pass min/avg/max %llu/%.0f/%llu; the own pad's record in %llu informative passes, "
                        "missing from %llu, missing from the whole window %llu; camera held through %llu failed resolutions, %llu jumps over %.0f m, %llu "
                        "context changes; rig publications %llu (%.0f rigs each); rig requests sent %llu; origin shifts re-based by the latch %llu.",
                        static_cast<unsigned long long>((now - g_summaryStartMs) / 1000), static_cast<unsigned long long>(g.informative),
                        static_cast<unsigned long long>(g.empty), static_cast<unsigned long long>(g.deferred), static_cast<unsigned long long>(g.thin),
                        static_cast<unsigned long long>(g.informative > 0 ? g.callsMin : 0), avg, static_cast<unsigned long long>(g.callsMax),
                        static_cast<unsigned long long>(g_winPadSeen), static_cast<unsigned long long>(g_winPadMissed),
                        static_cast<unsigned long long>(g_winPadAbsentWindow), static_cast<unsigned long long>(g_winCamHold),
                        static_cast<unsigned long long>(g_winCamJump), static_cast<double>(kCamJumpM), static_cast<unsigned long long>(g_winCtxChange),
                        static_cast<unsigned long long>(g_winRigPublications),
                        g_winRigPublications > 0 ? static_cast<double>(g_winRigPublished) / static_cast<double>(g_winRigPublications) : 0.0,
                        static_cast<unsigned long long>(g_rigTally.asked.load()), static_cast<unsigned long long>(g_winOriginShifts));
        g_winOriginShifts = 0;
        g_passGate.clearWindow();
        g_winPadSeen = g_winPadMissed = g_winPadAbsentWindow = g_winCamHold = g_winCamJump = g_winCtxChange = 0;
        g_winRigPublications = g_winRigPublished = 0;
    }
    // Built v14.20 (review 5), v14.31: the tables' occupancy now (written only when a probe bound was hit), the longest probe and the bounded (full) count this window, and the rig epoch count.
    if (g_prodStats.full.load(std::memory_order_relaxed) + g_rigStats.full + g_collStats.full > 0)
    SUMMARY_NOTE("optimized station: last %llu s tables: producer %u of %u used, longest probe %u, bounded %llu; rigs %u of %u, longest %u, "
                    "bounded %llu; collections %u of %u, longest %u, bounded %llu; rig epochs %llu (epoch %u).",
                    static_cast<unsigned long long>((now - g_summaryStartMs) / 1000), static_cast<unsigned>(prodOccupancy()),
                    static_cast<unsigned>(kProdSlots), static_cast<unsigned>(g_prodStats.maxProbes),
                    static_cast<unsigned long long>(g_prodStats.full), static_cast<unsigned>(g_rigStats.used),
                    static_cast<unsigned>(kRigSlots), static_cast<unsigned>(g_rigStats.maxProbes), static_cast<unsigned long long>(g_rigStats.full),
                    static_cast<unsigned>(g_collStats.used), static_cast<unsigned>(kCollSlots), static_cast<unsigned>(g_collStats.maxProbes),
                    static_cast<unsigned long long>(g_collStats.full), static_cast<unsigned long long>(g_rigEpochs),
                    static_cast<unsigned>(g_rigEpoch));
    // Built v14.25: the station's own costs over the window. Per call (one in 32 sampled): p50, p95 and max. Per frame: the calls each callback made, and
    // the estimated ms per frame (the sampled mean times the calls, over the frames). The frame tick per frame. The state lock's wait. Change-only waits.
    // Built v14.26: the callbacks' samples are drained by the tick; the entries lost in the window; the state-lock acquisitions a callback made (must be 0).
    {
        double s50 = 0.0, s95 = 0.0, smax = 0.0, p50 = 0.0, p95 = 0.0, pmax = 0.0, r50 = 0.0, r95 = 0.0, rmax = 0.0, f50 = 0.0, f95 = 0.0, fmax = 0.0;
        double meanStore = 0.0, meanProd = 0.0, meanRig = 0.0;
        size_t nStore = 0, nProd = 0, nRig = 0, nFrame = 0;
        uint64_t blocked = 0, waitingLines = 0;
        double waitTotal = 0.0, waitMax = 0.0;
        const uint64_t frames = g_winFrames;
        const uint64_t callsStore = g_callsStore.exchange(0, std::memory_order_relaxed);
        const uint64_t callsProd = g_callsProducer.exchange(0, std::memory_order_relaxed);
        const uint64_t callsRig = g_callsRig.exchange(0, std::memory_order_relaxed);
        nStore = g_costStore.used();
        meanStore = ringMean(g_costStore);
        sealed::samplePercentiles(g_costStore.v, nStore, &s50, &s95, &smax);
        nProd = g_costProducer.used();
        meanProd = ringMean(g_costProducer);
        sealed::samplePercentiles(g_costProducer.v, nProd, &p50, &p95, &pmax);
        nRig = g_costRig.used();
        meanRig = ringMean(g_costRig);
        sealed::samplePercentiles(g_costRig.v, nRig, &r50, &r95, &rmax);
        nFrame = g_costFrame.used();
        sealed::samplePercentiles(g_costFrame.v, nFrame, &f50, &f95, &fmax);
        blocked = g_mutexBlocked;
        waitTotal = g_mutexWaitTotalMs;
        waitMax = g_mutexWaitMaxMs;
        waitingLines = g_waitingLinesWritten;
        const uint64_t lost = g_winLost;
        const uint64_t onCallback = g_lockOnCallback.load(std::memory_order_relaxed);
        g_costStore.clear();
        g_costProducer.clear();
        g_costRig.clear();
        g_costFrame.clear();
        g_mutexBlocked = 0;
        g_mutexWaitTotalMs = 0.0;
        g_mutexWaitMaxMs = 0.0;
        g_waitingLinesWritten = 0;
        g_winLost = 0;
        g_lockOnCallback.store(0, std::memory_order_relaxed);
        const double fr = frames > 0 ? static_cast<double>(frames) : 1.0;
        SUMMARY_NOTE("optimized station: cost last %llu s: store observer %llu calls (%.1f per frame, ~%.4f ms per frame; p50/p95/max %.4f/%.4f/%.4f "
                        "ms per call, %llu sampled), producer veto %llu calls (%.1f per frame, ~%.4f ms per frame; %.4f/%.4f/%.4f ms per call, %llu "
                        "sampled), rig veto %llu calls (%.1f per frame, ~%.4f ms per frame; %.4f/%.4f/%.4f ms per call, %llu sampled), frame tick "
                        "%.3f/%.3f/%.3f ms per frame (%llu frames), mutex wait total %.3f ms max %.3f ms (blocked over 0.1 ms: %llu), waiting lines "
                        "written %llu; gathered entries lost %llu; state lock taken from a callback %llu; rig veto lookups: hit vetoed %llu, hit kept %llu, "
                        "collection moved %llu, miss %llu, stale epoch %llu (rig requests sent %llu); node names read from the game %llu, uncached node reads %llu, node reuse caught %llu.",
                        static_cast<unsigned long long>((now - g_summaryStartMs) / 1000), static_cast<unsigned long long>(callsStore),
                        static_cast<double>(callsStore) / fr, meanStore * static_cast<double>(callsStore) / fr, s50, s95, smax,
                        static_cast<unsigned long long>(nStore), static_cast<unsigned long long>(callsProd), static_cast<double>(callsProd) / fr,
                        meanProd * static_cast<double>(callsProd) / fr, p50, p95, pmax, static_cast<unsigned long long>(nProd),
                        static_cast<unsigned long long>(callsRig), static_cast<double>(callsRig) / fr, meanRig * static_cast<double>(callsRig) / fr,
                        r50, r95, rmax, static_cast<unsigned long long>(nRig), f50, f95, fmax, static_cast<unsigned long long>(nFrame), waitTotal,
                        waitMax, static_cast<unsigned long long>(blocked), static_cast<unsigned long long>(waitingLines),
                        static_cast<unsigned long long>(lost), static_cast<unsigned long long>(onCallback),
                        static_cast<unsigned long long>(g_rigTally.hitVetoed.load()), static_cast<unsigned long long>(g_rigTally.hitKept.load()),
                        static_cast<unsigned long long>(g_rigTally.hitMoved.load()), static_cast<unsigned long long>(g_rigTally.miss.load()),
                        static_cast<unsigned long long>(g_rigTally.stale.load()), static_cast<unsigned long long>(g_rigTally.asked.load()),
                        static_cast<unsigned long long>(g_nodeNameReads.exchange(0, std::memory_order_relaxed)),
                        static_cast<unsigned long long>(g_nodeCacheFull.exchange(0, std::memory_order_relaxed)),
                        static_cast<unsigned long long>(g_nodeReuseCaught.exchange(0, std::memory_order_relaxed)));
        g_rigTally.hitVetoed = 0;
        g_rigTally.hitKept = 0;
        g_rigTally.hitMoved = 0;
        g_rigTally.miss = 0;
        g_rigTally.stale = 0;
        g_rigTally.asked = 0;
    }

    g_prodStats.maxProbes.store(0, std::memory_order_relaxed);
    g_rigStats.maxProbes = g_collStats.maxProbes = 0;
    g_prodStats.full.store(0, std::memory_order_relaxed);
    g_rigStats.full = g_collStats.full = 0;
    g_rigEpochs = 0;
    g_summaryStartMs = now;
    g_winFrames = g_winSealedFrames = g_winDockedFrames = g_winRigVetoes = g_winProducerVetoes = 0;
    g_winCulled = g_winKeptUnknown = g_winFaults = 0;
    g_winNoCamera = 0;
    for (auto& f : g_winFaultBy) f = 0;
}
#undef SUMMARY_NOTE

// The producer-B and rig vetoes: attached while the setting is on, and they answer only while sealed.
bool producerVetoBody(uintptr_t rec, uintptr_t owner) noexcept {
    (void)owner;
    if (!g_sealedFlag.load(std::memory_order_acquire)) return false;
    // Built v14.25: no lock. The decision reads the published view (pinned), and the producer cache is lock-free.
    SnapshotPin pin;   // built v14.29: the thread's own hazard cell
    const PadSnapshot& s = pin.get();
    const bool veto = s.held && producerDecideSnap(rec, s);
    if (veto) ++tlsTc.producerVetoes;
    return veto;
}

// The producer-B veto, timed one call in 32 (built v14.24; the sample is gathered since v14.26).
bool producerVetoHook(uintptr_t rec, uintptr_t owner) noexcept {
    CallbackScope scope;
    TlsCounters& tc = tcFor(g_frameStamp.load(std::memory_order_relaxed));
    ++tc.callsProducer;
    const bool sample = costSampleCall(tc.sampleProducer);
    const int64_t t0 = sample ? costTicks() : 0;
    const bool veto = producerVetoBody(rec, owner);
    if (sample) costPush(1, costMs(t0, costTicks()));
    return veto;
}

bool rigVetoBody(uintptr_t rig) noexcept {
    if (!g_sealedFlag.load(std::memory_order_acquire)) return false;
    const bool veto = rigVetoLockFree(rig);
    if (veto) ++tlsTc.rigVetoes;
    return veto;
}

// The rig early veto, timed one call in 32 (built v14.24; the sample is gathered since v14.26).
bool rigVetoHook(uintptr_t rig, uintptr_t ctx) noexcept {
    (void)ctx;
    CallbackScope scope;
    TlsCounters& tc = tcFor(g_frameStamp.load(std::memory_order_relaxed));
    ++tc.callsRig;
    const bool sample = costSampleCall(tc.sampleRig);
    const int64_t t0 = sample ? costTicks() : 0;
    const bool veto = rigVetoBody(rig);
    if (sample) costPush(2, costMs(t0, costTicks()));
    return veto;
}

// Attaches or detaches the vetoes and the store observer to the setting. Attach is refused (logged once) when a hook stands down.
// Built v14.20 (review 3): the toggle's hook operations run OUTSIDE the state lock (a detach may wait for callbacks, and a callback needs the lock).
// Off: the callbacks are detached and the gate closed first, so no new callback starts; then the state changes under the lock, which waits for a
// callback already inside. On: the state resets under the lock, then the hooks attach. Either way the rig and collection epoch moves on.
void applySetting(bool on, uint32_t frame) noexcept {
    if (on == g_vetoesAttached) return;
    if (!on) {
        kinematicEvalSetDirectVeto(nullptr);
        kinematicEvalSetRigVeto(nullptr);
        kinematicEvalDirectVetoDetach();
        kinematicEvalRigVetoDetach();
        g_storeWanted = false;
        storeHookRecompute();   // closes the gate before the state changes
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
        g_sealState = sealed::SealState{};   // a setting toggle starts the trigger afresh; the own pad (GameTrack) is kept (built v14.23)
        g_preShellValid = false;
        g_sealGuard = sealed::SealRateGuard{};
        bumpRigEpoch();                      // built v14.20 (review 4): the rig and collection tables start afresh
        if (!on && g_sun.off) {   // setting off: the sun comes back on (built v14.13)
            g_sun.off = false;
            g_sunOff.store(false, std::memory_order_release);
            Log::get().note("optimized station: sun light inside the hangar back on (setting off; Status docked=%s)",
                            g_dockedNow.load(std::memory_order_acquire) ? "yes" : "no");
        }
        if (!on) releaseSeal("setting off", frame);
    }
    if (on) {
        const char* direct = kinematicEvalDirectVetoAttach();
        const char* rig = kinematicEvalRigVetoAttach();
        kinematicEvalSetDirectVeto(&producerVetoHook);
        kinematicEvalSetRigVeto(&rigVetoHook);
        g_storeWanted = true;
        storeHookRecompute();
        Log::get().note("optimized station: on (frame %u). Producer veto: %s; rig veto: %s; store observer: %s.", frame, direct, rig,
                        storeHookStatus());
    } else {
        Log::get().note("optimized station: off (frame %u).", frame);
    }
    padStateSetOn(on);   // built v14.22: the pad-state probe (temporary, log only) installs with the setting
    g_vetoesAttached = on;
}

// The oriented box for the pad at the freeze: the pad's own orientation and extent (its builder pose), the wiki footprint doubled along its length
// (the hangar is the pad doubled along one axis), the margin, and the floor. Without a pose or an extent there is no box (the capsule is the cell).
void buildBoxFor(sealed::OwnCell& cell, const float target[3]) {
    cell.box = false;
    if (!cell.has || cell.size == sealed::PadSize::kNone) return;
    float axes[3][3] = {};
    float ext[3] = {};
    bool extOk = false;
    uint8_t src = 4;
    if (!poseOrientOf(cell.pad, axes, ext, &extOk, &src) || !extOk) return;
    float u[3] = {target[0] - cell.centre[0], target[1] - cell.centre[1], target[2] - cell.centre[2]};
    const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(ul >= 1.0f)) return;
    for (float& v : u) v /= ul;
    int li = -1, wi = -1, ui = -1;
    if (!sealed::footprintAxes(axes, u, ext, &li, &wi, &ui)) return;
    const float* wiki = sealed::kPadWikiM[sealed::sizeIndex(cell.size)];
    const float halfLen = wiki[0] * 0.5f * 2.0f;
    const float halfWid = wiki[1] * 0.5f;
    sealed::buildBox(cell.centre, target, axes[li], halfLen, halfWid, g_margin, belowFor(cell.size), &cell);
}

// The promote: the frame's first record. The pad pick, the pad's heights, the capsule and box, the engage, the hangar-change drops.
// ---- Built v14.26: the tick applies the gathered entries of the previous pass, in the order the callbacks wrote them ------------------------------

// The rig note of one judged record: the rig, its bounds, and its name the first time. The job path's rig is the bucket's collection, resolved here with
// the bucket's one cache entry (a bucket's records come in a run).
void applyRigNote(const GatherEntry& e) {
    uintptr_t rig = e.aux;
    const bool jobPath = (e.flags & kFlagJob) != 0;
    if (!jobPath) {
        ++g_rigTally.assocSync;
    } else {
        if (e.bucket != 0 && e.bucket == g_rigBucketCache) {
            rig = g_rigBucketRig;
        } else {
            const uintptr_t owner = e.aux;
            rig = 0;
            if (e.bucket != 0 && owner > 0x300) rig = collFind(owner - 0x300);
            g_rigBucketColl = owner > 0x300 ? owner - 0x300 : 0;   // built v14.24: the collection this bucket names
            g_rigBucketCache = e.bucket;
            g_rigBucketRig = rig;
        }
        if (rig) ++g_rigTally.assocJob;
        else ++g_rigTally.assocNone;
    }
    if (!rig) return;
    RigSlot* s = rigFind(rig, true);
    if (!s) return;
    // Built v14.24: a job-path association is current only while the rig still holds that collection.
    if (jobPath && !sealed::collAssociationIs(s->coll, g_rigBucketColl)) return;
    size_t pidx = 0;
    uint8_t pv = 0;
    uintptr_t pc = 0;
    if (g_rigPub.lookup(rig, rigIndex(rig), &pv, &pc, &pidx) && g_rigPub.evalStampAt(pidx) == e.stamp) ++g_rigTally.matched;
    else ++g_rigTally.unevaluated;
    if (s->curStamp != e.stamp) {
        if (s->curValid) {
            for (int k = 0; k < 3; ++k) {
                s->lastMn[k] = s->curMn[k];
                s->lastMx[k] = s->curMx[k];
            }
            s->lastValid = true;
        }
        s->curStamp = e.stamp;
        s->curValid = false;
    }
    for (int k = 0; k < 3; ++k) {
        const float lo = e.centre[k] - e.radius, hi = e.centre[k] + e.radius;
        if (!s->curValid) {
            s->curMn[k] = lo;
            s->curMx[k] = hi;
        } else {
            if (lo < s->curMn[k]) s->curMn[k] = lo;
            if (hi > s->curMx[k]) s->curMx[k] = hi;
        }
    }
    s->curValid = true;
    if (!s->named) {
        s->named = true;
        char name[64] = {};
        if (!cachedName(e.record, name, sizeof(name))) std::snprintf(name, sizeof(name), "(none)");
        std::snprintf(s->name, sizeof(s->name), "%s", name);   // Built v13: the own-ship prefix test reads it
    }
}

// A rig's collection moved (its veto found the new one): its bounds are forgotten, the association is recorded, and its run of vetoes starts afresh.
void applyRigAssoc(uintptr_t rig, uintptr_t coll) {
    RigSlot* s = rigFind(rig, true);
    if (!s || coll == s->coll) return;
    uint32_t run = 0;
    sealed::rigForgetBounds(&s->curValid, &s->lastValid, &s->verdict, &run);
    s->coll = coll;
    collInsert(coll, rig);
    size_t idx = 0;
    uint8_t v = 0;
    uintptr_t c = 0;
    if (g_rigPub.lookup(rig, rigIndex(rig), &v, &c, &idx)) g_rigPub.resetRun(idx);
}

void applyEntry(const GatherEntry& e) {
    switch (e.kind) {
        case GKind::kPad: {
            sealed::PadObs p;
            p.addr = e.record;
            for (int k = 0; k < 3; ++k) p.centre[k] = e.centre[k];
            p.radius = e.radius;
            p.size = static_cast<sealed::PadSize>(e.flags);
            std::snprintf(p.name, sizeof(p.name), "%s", e.name);
            if (p.size != sealed::PadSize::kNone) g_padWin.put(p);
            if (e.record == g_ownSticky.pad) g_stickyPadInPass = true;
            break;
        }
        case GKind::kExplicit: {
            sealed::ExplicitObs x;
            x.addr = e.record;
            x.node = e.aux;
            for (int k = 0; k < 3; ++k) x.centre[k] = e.centre[k];
            std::snprintf(x.name, sizeof(x.name), "%s", e.name);
            g_explicitWin.put(x);
            break;
        }
        case GKind::kNear:
            if (g_nearCurN < kNearSlots) {
                NearRec& nr = g_nearCur[g_nearCurN++];
                nr.addr = e.record;
                std::snprintf(nr.name, sizeof(nr.name), "%s", e.name);
                for (int k = 0; k < 3; ++k) nr.centre[k] = e.centre[k];
                nr.radius = e.radius;
                nr.dist = e.dist;
            }
            break;
        case GKind::kOwnCand:
            if (g_ownCandidateDist < 0.0f || e.dist < g_ownCandidateDist) {
                std::snprintf(g_ownCandidateName, sizeof(g_ownCandidateName), "%s", e.name);
                g_ownCandidateDist = e.dist;
            }
            break;
        case GKind::kTally:
            tallyShipPrefix(e.name);
            break;
        case GKind::kLatch:
            if (g_latchCandN < sealed::kLatchCandCap) {
                sealed::LatchCand& cand = g_latchCands[g_latchCandN++];
                cand.addr = e.record;
                std::snprintf(cand.name, sizeof(cand.name), "%s", e.name);
                for (int k = 0; k < 3; ++k) cand.centre[k] = e.centre[k];
            }
            break;
        case GKind::kHangar:
            if (g_nameLogN < 12) {
                NameNote& nn = g_nameLog[g_nameLogN++];
                std::snprintf(nn.name, sizeof(nn.name), "%s", e.name);
                nn.dist = e.dist;
                nn.radius = e.radius;
            }
            break;
        case GKind::kRigNote:
            applyRigNote(e);
            break;
        case GKind::kRigAssoc:
            applyRigAssoc(e.aux, e.coll);
            break;
        case GKind::kCost:
            if (e.flags == 0) costAdd(g_costStore, static_cast<double>(e.ms));
            else if (e.flags == 1) costAdd(g_costProducer, static_cast<double>(e.ms));
            else costAdd(g_costRig, static_cast<double>(e.ms));
            break;
        case GKind::kNone:
            break;
    }
}

// Drains the gathered entries (the tick only, under the lock). A loss in the drained pass is kept for the next publication.
void drainGather() {
    g_rigBucketCache = 0;
    g_rigBucketRig = 0;
    g_stickyPadInPass = false;
    const sealed::PassDrain d = g_gather.drain([](const GatherEntry& e) { applyEntry(e); });
    g_winLost += d.lost;
    // Built v14.27: the verdict on the drained passes. Only a complete pass that held a store callback is evidence; an empty or unfinished one leaves the
    // pad, the camera, the latch samples and the rig publication as they were.
    const sealed::PassVerdict v = g_passGate.step(d.complete, d.passes, d.calls);
    g_passInformative = v == sealed::PassVerdict::kInformative;
    g_passCalls = d.calls;
    g_recordsLast = static_cast<uint32_t>(d.calls > 0xFFFFFFFFull ? 0xFFFFFFFFull : d.calls);
    // A loss keeps every record in the next publication; it ends with the first informative pass that lost nothing.
    if (d.lost != 0) g_lossPublished = true;
    else if (g_passInformative) g_lossPublished = false;
    if (g_passInformative) {
        if (d.ctx != 0 && g_lastPassCtx != 0 && d.ctx != g_lastPassCtx) ++g_winCtxChange;
        if (d.ctx != 0) g_lastPassCtx = d.ctx;
        g_passCtx = d.ctx;
    } else {
        g_passCtx = 0;
    }
}

void promoteFrame(uint32_t stamp, uint32_t frame) {
    sealed::OwnCell next;
    g_ownCandidateDist = -1.0f;
    g_prefixTallyN = 0;   // the tally is this frame's records (built v14.8)
    // Built v14.29: a pass that did not sample the near list leaves the last sampled one standing (the engage reads it).
    if (g_nearCurN > 0 || g_engagePublished) {
        std::memcpy(g_nearPrev, g_nearCur, sizeof(g_nearPrev));
        g_nearPrevN = g_nearCurN;
    }
    g_nearCurN = 0;
    readOwnLive();
    refreshHeldWorld();
    if (g_frameCamValid) {
        const float radii[3] = {sealed::kCellDefaultM[0], sealed::kCellDefaultM[1], sealed::kCellDefaultM[2]};
        for (size_t i = 0; i < g_padCount; ++i) {
            float axes[3][3] = {};
            float ext[3] = {};
            bool extOk = false;
            uint8_t src = 4;
            g_pads[i].upOk = poseOrientOf(g_pads[i].addr, axes, ext, &extOk, &src);
            if (g_pads[i].upOk) {
                for (int k = 0; k < 3; ++k) g_pads[i].up[k] = axes[2][k];
            }
        }
        sealed::pickOwnPadSticky(g_pads, g_padCount, g_frameCam, radii, &g_ownSticky, &next);
        if (next.has) {
            float e2[3] = {};
            float axes[3][3] = {};
            float ext[3] = {};
            bool extOk = false;
            uint8_t src = 4;
            const bool haveAxes = poseOrientOf(next.pad, axes, ext, &extOk, &src);
            if (haveAxes) {
                for (int k = 0; k < 3; ++k) e2[k] = axes[2][k];
            }
            float q[3] = {};
            char qn[48] = {};
            float perp = 0.0f, along = 0.0f;
            bool picked = false;
            if (haveAxes) {
                picked = sealed::pickTargetOnLine(g_explicits, g_explicitCount, next.centre, e2, 0, q, qn, sizeof(qn), &perp, &along) ||
                         sealed::pickTargetOnLine(g_explicits, g_explicitCount, next.centre, e2, 1, q, qn, sizeof(qn), &perp, &along);
            }
            if (!picked) picked = sealed::pickCapsuleTarget(g_explicits, g_explicitCount, next.centre, q, qn, sizeof(qn));
            if (picked && sealed::buildCapsule(next.centre, q, sealed::kCapsuleExtendM, next.capA, next.capB, &next.capLen)) {
                next.capsule = true;
                std::snprintf(next.capTarget, sizeof(next.capTarget), "%s", qn);
                buildBoxFor(next, q);
            }
        }
    } else {
        std::snprintf(next.reason, sizeof(next.reason), "no camera for the previous frame");
    }
    g_pickCell = next;
    // The held pad: absent from the store for kHeldLostFrames frames, or another own pad nearest for that long, is a hangar change.
    if (g_heldValid) {
        bool heldSeen = false;
        const sealed::PadObs* best = nullptr;
        float bestD = 0.0f;
        for (size_t i = 0; i < g_padCount; ++i) {
            const sealed::PadObs& p = g_pads[i];
            if (p.addr == g_heldCell.pad) heldSeen = true;
            if (!g_frameCamValid) continue;
            const float d = sealed::distance(p.centre, g_frameCam);
            if (!sealed::ownPadCandidate(d, p.radius)) continue;
            if (!best || d < bestD) {
                best = &p;
                bestD = d;
            }
        }
        g_heldAbsent = heldSeen ? 0 : g_heldAbsent + 1;
        const bool otherPick = next.has && next.pad != g_heldCell.pad;   // the sticky pick: another pad only after the held one is unseen
        g_otherPicked = otherPick ? g_otherPicked + 1 : 0;
        if (sealed::heldCellDrops(g_heldAbsent, g_otherPicked, sealed::kHeldLostFrames)) {
            char why[160];
            if (g_heldAbsent >= sealed::kHeldLostFrames)
                std::snprintf(why, sizeof(why), "the hangar changed: the held pad %s is no longer in the store's records", g_heldCell.padName);
            else
                std::snprintf(why, sizeof(why), "the hangar changed: the nearest own pad is now %s", best ? best->name : "none");
            g_flagHangarChanged = true;
            std::snprintf(g_releaseWhy, sizeof(g_releaseWhy), "%s", why);
        }
    }
    if (g_releaseMotion && g_heldValid) {
        char why[160];
        std::snprintf(why, sizeof(why), "the pad moved %.1f m along the shaft relative to the hangar shell %s (more than %.0f m)",
                      static_cast<double>(g_shellDeltaM), g_shellName, static_cast<double>(sealed::kPadMoveReleaseM));
        g_flagPadMoved = true;
        std::snprintf(g_releaseWhy, sizeof(g_releaseWhy), "%s", why);
    }
    // The engage: the cell is frozen from the pad when the seal rule asked for it. Held frames take the held cell.
    if (g_engagePending) {
        ++g_engageTries;
        // Built v14.20 (review 1): the freeze needs the request still in force for this pad and shell, a lowered pad in the bay, and a valid cell.
        const sealed::EngageRequest req{g_engagePending, g_engagePad, g_engageShell};
        const bool cellValid = next.has && next.radius > 0.0f && sealed::cellInvalidWhy(next) == nullptr;
        const bool admitted = sealed::engageAdmitted(req, g_sealState, next.pad, g_preShellValid ? g_preShellAddr : 0, cellValid);
        if (admitted) {
            setOwnPrefixAtEngage(frame);
            pickOwnRecord(frame);
            g_heldCell = next;
            g_heldValid = true;
            snapshotSealPose();              // built v14.24: each latched record's pose at the seal (v14.33: taken again when the mechanism release arms)
            g_mechArm.reset();               // built v14.33: mechanism motion neither releases nor counts until 60 still passes
            setHeldFrame(g_heldCell, frame);
            g_engagePending = false;
            g_sealedFlag.store(true, std::memory_order_release);
            sealStatus(g_heldCell, frame);
        } else if (!g_sealState.requested || (next.has && next.pad != g_engagePad)) {
            // The request was withdrawn, or it belongs to another pad: cancelled, and never frozen later (built v14.20, review 1).
            g_engagePending = false;
            g_engageTries = 0;
        } else if (g_engageTries >= kEngageTries) {
            // Built v14.8: the cell is not valid (no box, a zero box, no target), so no seal. The seal is requested again (the pad may gain a target);
            // the note is written once per distinct reason.
            g_engagePending = false;
            g_sealState.requested = false;
            const char* why = sealed::cellInvalidWhy(next);
            if (why == nullptr) why = next.reason;
            static char lastWhy[120] = {};
            if (std::strcmp(lastWhy, why) != 0) {
                std::snprintf(lastWhy, sizeof(lastWhy), "%s", why);
                Log::get().note("optimized station: no seal: the cell is not valid (%s); the station keeps drawing everything, and the seal is tried again.", why);
            }
            g_engageTries = 0;
        }
        if (!g_heldValid) next = sealed::OwnCell{};
    } else if (g_heldValid) {
        next = g_heldCell;
    } else {
        next = sealed::OwnCell{};
    }
    if (next.has && next.capsule)
        g_explicitPrevCount = sealed::pickExplicitSegment(g_explicits, g_explicitCount, next.capA, next.capB, g_explicitPrev, sealed::kExplicitNodes);
    else
        g_explicitPrevCount = next.has ? sealed::pickExplicit(g_explicits, g_explicitCount, next.centre, g_explicitPrev, sealed::kExplicitNodes) : 0;
    g_cellPrev = next;
    g_padWin.endPass();        // built v14.27: the windows age by one informative pass; a record unseen for kSeenKeep passes is gone
    g_explicitWin.endPass();
    g_cellStamp = stamp;
}

// Localizes the held cell to the held pad's frame and picks the shell. No logs: the seal's status line is written by the caller.
void setHeldFrame(const sealed::OwnCell& cell, uint32_t frame) {
    (void)frame;
    g_heldLocalValid = false;
    g_shellValid = false;
    g_releaseMotion = false;
    g_shellMotion = sealed::ShellMotion{};
    g_shellSampleStamp = 0xFFFFFFFFu;
    float axes[3][3] = {};
    float ext[3] = {};
    bool extOk = false;
    uint8_t src = 4;
    if (!poseOrientOf(cell.pad, axes, ext, &extOk, &src)) return;
    std::memcpy(g_engageAxes, axes, sizeof(g_engageAxes));
    std::memcpy(g_engageCentre, cell.centre, sizeof(g_engageCentre));
    sealed::localizeCell(cell, cell.centre, axes, &g_heldLocal);
    g_heldLocalValid = true;
    const size_t bestI = sealed::pickShell(g_explicits, g_explicitCount, cell.centre, cell.size, 150.0f);   // chosen once, here
    if (bestI < g_explicitCount) {
        const sealed::ExplicitObs& x = g_explicits[bestI];
        g_shellAddr = x.addr;
        std::snprintf(g_shellName, sizeof(g_shellName), "%s", x.name);
        g_shellValid = true;
    }
}

// Publishes the view the callbacks read. The tick only (under g_sealedMutex): the callbacks pin the ring and never write it.
void publishSnapshot() noexcept {
    PadSnapshot& s = g_snapRing.beginWrite();
    s.frameStamp = g_frameStamp.load(std::memory_order_relaxed);
    s.prodGen = g_prodGenA.load(std::memory_order_relaxed);
    s.held = g_heldValid;
    s.camValid = g_frameCamValid;
    std::memcpy(s.cam, g_frameCam, sizeof(s.cam));
    s.cell = g_cellPrev;
    s.explicitCount = g_explicitPrevCount < sealed::kExplicitNodes ? g_explicitPrevCount : sealed::kExplicitNodes;
    std::memcpy(s.explicitPrev, g_explicitPrev, sizeof(s.explicitPrev));
    std::memcpy(s.ownPrefix, g_ownPrefix, sizeof(s.ownPrefix));
    s.giveUp = g_hangarWatch.giveUp;
    s.latchPending = g_latchPending;
    s.namesPending = g_namesPending;
    std::memcpy(s.padC, g_namesPadC, sizeof(s.padC));
    s.ownLiveValid = g_ownLiveValid;
    std::memcpy(s.ownLiveCentre, g_ownLiveCentre, sizeof(s.ownLiveCentre));
    s.ownLiveRadius = g_ownLiveRadius;
    s.lost = g_lossPublished;
    s.engagePending = g_engagePending;
    g_engagePublished = g_engagePending;
    g_snapRing.publish();
}

// The tick's view of the camera: resolved from the context the last callback saw (built v14.26: one frame behind the callbacks that read it). Up to
// kViewTries resolves while the centre the consumer uses is not ready, as the callbacks did.
void resolveTickView() noexcept {
    // Built v14.27: only an informative pass resolves the camera, from the context its busiest thread's callbacks saw (not "the last callback's"). An
    // empty or unfinished pass holds the camera as it was.
    if (!g_passInformative) return;
    const uintptr_t ctx = g_passCtx;
    if (ctx == 0) return;   // the pass named no context: nothing to resolve from; the camera is held
    view::resolveView(ctx, &g_view);
    for (uint32_t t = 1; t < kViewTries && !sealed::viewCentreReady(g_view.originOk, g_view.poseOk, g_view.source == view::Source::kExplorerCam); ++t)
        view::resolveView(ctx, &g_view);
    float cam[3] = {};
    const bool ok = cameraOf(g_view, cam);
    if (ok) {
        if (g_frameCamValid && sealed::distance(cam, g_frameCam) > kCamJumpM) ++g_winCamJump;
        std::memcpy(g_frameCam, cam, sizeof(g_frameCam));
        g_cameraKnown.store(true, std::memory_order_relaxed);
    } else {
        faultNote(kFaultTickView);
        ++g_winCamHold;
    }
    g_frameCamValid = g_camHold.resolved(ok);   // a failed resolution is covered by the last good camera for kCamHoldTicks passes
}

// The gathered rig sphere of one judged record: the rig on the synchronous path (the evaluation's own stack), or the bucket's owner on the job path (the
// tick resolves its collection). A record without a sphere is not gathered.
void gatherRigNote(GatherWriter& w, uintptr_t bucket, uintptr_t record, const float c[3], float r, uint32_t stamp) noexcept {
    if (!(r > 0.0f) || !std::isfinite(r)) return;
    GatherEntry e = gatherEntry(GKind::kRigNote, record, c, r, stamp, nullptr);
    e.bucket = bucket;
    const uintptr_t rig = kinematicEvalCurrentRig();
    if (rig != 0) {
        e.aux = rig;
    } else {
        e.flags = kFlagJob;
        uintptr_t owner = 0;
        if (bucket != 0) readVal(&owner, bucket + 0x10);
        e.aux = owner;
    }
    w.push(e);
}

// The store observer's judgement, for one record the store has written. Built v14.26: no lock. The callback reads the published view, gathers the parts
// the tick needs into its thread's slot, and judges from the view alone. Order: the centre (unreadable: kept), the gathered parts (pads, explicit parts,
// the near list, the own candidate, the ship's tally, the latch and the hangar names), then (judge only) the named keep, the bay keep, the own ship, and
// otherwise culled. judge: the sealed judgement; false: gathering only.
void afterStoreJudge(uintptr_t bucket, uintptr_t record, bool judge) noexcept {
    // Built v14.27: the callback is one Writer. Its entries all go to the pass of the stamp it read on entry, and the pass is not read while it is inside.
    GatherWriter w(g_gather);
    w.call(0);
    TlsCounters& tc = tcFor(g_frameStamp.load(std::memory_order_relaxed));   // built v14.29: the thread's own counters
    SnapshotPin pin;
    const PadSnapshot& s = pin.get();
    if (judge && !s.held) return;
    uintptr_t ctx = 0;
    uint32_t count = 0;
    if (!bucket || !record || !readVal(&ctx, bucket) || !ctx || !readVal(&count, ctx + kCtxCount) || count == 0) {
        faultNote(kFaultBucket);
        return;
    }
    w.context(ctx);   // names the context for the pass (the call itself was counted above)
    if (!s.camValid) {
        // Not a fault: the first pass after the gate opens has no camera yet (v14.26 counted the whole pass, 1268 records, as faults here).
        ++tc.noCamera;
        return;
    }
    const float* cam = s.cam;
    const uint32_t stamp = w.stamp();
    const bool lost = s.lost || w.lostSinceDrain() != 0;   // a loss keeps every record (fail-safe)
    // Built v14.29: the node first. Its class and flags come from the node cache (the name is read once per node per kClassKeyFrames frames). An unsealed
    // pass that does not sample the near list, the own candidate and the tally reads the centre only of the nodes that can matter (pads, explicit parts,
    // and the mechanism and hangar names while they are wanted); every other record returns here.
    uintptr_t node = 0;
    const NodeInfo ni = nodeInfoOf(record, &node);
    const bool full = sealed::fullGatherPass(stamp, s.engagePending, s.held);
    if (!judge && !sealed::nodeWantsCentre(ni.cls, ni.flags, s.giveUp, s.latchPending, s.namesPending, full)) {
        return;
    }
    const WorldRead wr = worldOfRecord(record);
    if (judge) {
        ++tc.worldSrc[wr.source];
        if (wr.source != 4) gatherRigNote(w, bucket, record, wr.world, wr.radius, stamp);
    }
    const float* centre = wr.world;
    const float radius = wr.radius;
    if (wr.source == 4 || !std::isfinite(radius) || !(radius > 0.0f) || !std::isfinite(centre[0]) || !std::isfinite(centre[1]) ||
        !std::isfinite(centre[2])) {
        if (judge) ++tc.keptUnknown;   // the centre cannot be read: kept, never judged
        return;
    }
    if (ni.cls == sealed::NameClass::kPad) {
        char name[64] = {};
        if (cachedNameOfNode(node, name, sizeof(name), &ni.validator)) {
            const sealed::PadSize size = sealed::padSizeOf(name);
            if (size != sealed::PadSize::kNone) {
                GatherEntry e = gatherEntry(GKind::kPad, record, centre, radius, stamp, name);
                e.flags = static_cast<uint8_t>(size);
                w.push(e);
            }
        }
    } else if (ni.cls == sealed::NameClass::kExplicit && !s.giveUp) {   // built v14.13: stops after the give-up
        char xn[64] = {};
        cachedNameOfNode(node, xn, sizeof(xn), &ni.validator);   // no name: the explicit part is still gathered, with an empty name
        GatherEntry e = gatherEntry(GKind::kExplicit, record, centre, radius, stamp, xn);
        e.aux = node;
        w.push(e);
    }
    // The own ship: the own candidate (the nearest small containing sphere), the prefix rule, then the live own sphere. Built v14.29: the near list, the
    // candidate and the tally are gathered on a full pass only (every kSamplePass-th stamp, and every stamp while sealed or while an engage is pending).
    const bool containsCam = sealed::containsCamera(centre, radius, cam);
    const float dCam = sealed::distance(centre, cam);
    if (full) {
        if (dCam <= kOwnNearM) {
            char nn[64] = {};
            if (cachedNameOfNode(node, nn, sizeof(nn), &ni.validator)) {
                GatherEntry e = gatherEntry(GKind::kNear, record, centre, radius, stamp, nn);
                e.dist = dCam;
                w.push(e);
            }
        }
        if (containsCam && radius < sealed::kOwnShipRadiusM) {
            char cn[64] = {};
            if (cachedNameOfNode(node, cn, sizeof(cn), &ni.validator)) {
                GatherEntry e = gatherEntry(GKind::kOwnCand, record, centre, radius, stamp, cn);
                e.dist = dCam;
                w.push(e);
            }
        }
        // The ship's part count by prefix (built v14.8). A name with no prefix counts for nothing, so a node without the prefix flag is not read.
        if (dCam <= sealed::kShipRecordsM && (ni.flags & sealed::kNodeShipPrefix) != 0) {
            char tn[64] = {};
            if (cachedNameOfNode(node, tn, sizeof(tn), &ni.validator)) w.push(gatherEntry(GKind::kTally, record, centre, radius, stamp, tn));
        }
    }
    latchCandNote(w, record, node, centre, s, ni.flags);   // built v14.12: gathered only while a latch is pending
    hangarNameNote(w, record, node, centre, radius, s, ni.flags);   // built v14.13: only while the names are wanted
    if (!judge) return;   // gathering only: the judgement is sealed-only
    if (lost) {
        ++tc.kept;
        return;
    }
    const sealed::NamedKeep v = sealed::keepNamedCell(centre, radius, cam, kFallbackSphereM, s.cell, s.explicitPrev, s.explicitCount, record);
    bool keep = v.keep;
    const bool containing = v.containing;
    // The bay: the own hangar's parts inside the bay volume, even where the box alone would cull them. The shaft is the cell's.
    if (!keep && s.cell.capsule) {
        float u[3] = {};
        if (sealed::unitBetween(s.cell.capA, s.cell.capB, u) && sealed::inBayVolume(centre, s.cell.capA, u, s.cell.capLen, belowFor(s.cell.size))) {
            char bn[64] = {};
            if (cachedNameOfNode(node, bn, sizeof(bn), &ni.validator) && sealed::bayPartName(bn)) keep = true;
        }
    }
    if (!keep) {
        char nm[64] = {};
        if (s.ownPrefix[0] != '\0') cachedNameOfNode(node, nm, sizeof(nm), &ni.validator);
        if (sealed::ownShipExempt(containsCam, radius, nm, s.ownPrefix)) keep = true;
    }
    if (!keep && s.ownLiveValid && sealed::ownSphereKeeps(centre, s.ownLiveCentre, s.ownLiveRadius, kOwnSphereMarginM)) keep = true;
    if (keep) {
        ++tc.kept;
    } else {
        if (cullCounted(record, containing)) ++tc.culled;
    }
}

}  // namespace

// ---- The engine's entry points (declared in optimized_station.h) -------------------------------------------------------------------------

// Once, at install: reads fix.optimized_stations (on in VR, off in flat; the flat profile does not read it).
void optimizedStationInstall(bool flatProfile) {
    const std::string text = Config::get().getString("fix.optimized_stations", "on");
    const bool on = text != "off";
    g_setting.store(on, std::memory_order_release);
    g_summaryStartMs = GetTickCount64();
    if (on) applySetting(true, 0);
    Log::get().note("optimized station: fix.optimized_stations = %s (%s).", on ? "on" : "off",
                    flatProfile ? "flat profile" : "VR profile");
}

bool optimizedStationOn() noexcept { return g_setting.load(std::memory_order_acquire); }
bool optimizedStationGathers() noexcept { return g_setting.load(std::memory_order_acquire) && g_dockedNow.load(std::memory_order_acquire); }
bool optimizedStationSealed() noexcept { return g_sealedFlag.load(std::memory_order_acquire); }

// The draw path asks: while sealed, the sun pair is not drawn.
bool optimizedStationSkipsDraw(uint64_t vs, uint64_t ps) noexcept {
    if (!g_sunOff.load(std::memory_order_acquire) || vs != kSunVs || ps != kSunPs) return false;   // the sun skip follows the pad, not the seal (built v14.13)
    g_winSunDrawsSkipped.fetch_add(1, std::memory_order_relaxed);   // built v14.34: the 30 s line says how many (a flat flight shows 0 if the flat hashes differ)
    return true;
}

// The store calls this after it has written a record (bucket = the collection, record = its record). While sealed it judges the record; while
// docked and unsealed it only gathers the frame's pads, explicit parts and camera for the seal trigger (built v14.1).
void afterStoreDispatch(uintptr_t bucket, uintptr_t record) noexcept {
    const sealed::StoreMode mode = sealed::storeMode(g_setting.load(std::memory_order_acquire), g_sealedFlag.load(std::memory_order_acquire),
                                                     g_dockedNow.load(std::memory_order_acquire));
    if (mode == sealed::StoreMode::kNone) return;
    afterStoreJudge(bucket, record, mode == sealed::StoreMode::kJudge);
}

// The store observer's entry (the store hook calls it per record), timed one call in 32 (built v14.24; the sample is gathered since v14.26).
void optimizedStationAfterStore(uintptr_t bucket, uintptr_t record) noexcept {
    CallbackScope scope;
    TlsCounters& tc = tcFor(g_frameStamp.load(std::memory_order_relaxed));
    ++tc.callsStore;
    const bool sample = costSampleCall(tc.sampleStore);
    const int64_t t0 = sample ? costTicks() : 0;
    afterStoreDispatch(bucket, record);
    if (sample) costPush(0, costMs(t0, costTicks()));
}

// The render thread's frame tick: the setting, the held cell in the pad's frame, the seal and release rules, the rig verdicts, the summary.
// The game's pad state for this tick (built v14.23): the own pad (the object r0 was last called on this dock, GameTrack), its latest r0 and r1 codes,
// and whether each was called fresh (kPadStateFreshMs). Set by gameStateTick, read by the sun, the trigger and the waiting line.
struct GameNow {
    bool ownKnown = false;
    uintptr_t own = 0;
    int r0 = -1;
    bool r0Fresh = false;
    int r1 = -1;
    bool r1Fresh = false;
};
GameNow g_game;

// The own pad from the game, once per tick (built v14.23). The own pad changes only when r0 is called on another object (logged once); it is reset at
// undock and hangar change (GameTrack). Reads the probe; takes no lock of the station.
void gameStateTick() noexcept {
    PadObjState latest;
    padStateLatestR0(&latest);
    const sealed::GameTrackEvent ev = g_track.observe(latest.have, latest.self, latest.r0Qpc);
    if (ev == sealed::GameTrackEvent::kFirst) {
        Log::get().note("optimized station: own pad is obj @%06llx (the game's docking reader r0 was called on it this dock)",
                        static_cast<unsigned long long>(static_cast<uint64_t>(g_track.own) & 0xFFFFFFull));
    } else if (ev == sealed::GameTrackEvent::kSwitched) {
        Log::get().note("optimized station: own pad switched to obj @%06llx (r0 was called on it; the previous own pad is no longer the one)",
                        static_cast<unsigned long long>(static_cast<uint64_t>(g_track.own) & 0xFFFFFFull));
    }
    g_game = GameNow{};
    if (!g_track.have) return;
    PadObjState own;
    padStateObject(g_track.own, &own);
    g_game.ownKnown = true;
    g_game.own = g_track.own;
    g_game.r0 = own.r0;
    g_game.r0Fresh = own.r0 >= 0 && sealed::gameFresh(padStateAgeMs(own.r0Qpc));
    g_game.r1 = own.r1;
    g_game.r1Fresh = own.r1 >= 0 && sealed::gameFresh(padStateAgeMs(own.r1Qpc));
}

// Built v14.24: the mechanism's re-seal on the poses. While the seal is released by the mechanism (the trigger's reseal 1 with the mechanism flag) every
// latched record is compared with its pose at the seal. Returns true when all of them are back this frame (the core counts the frames). Logs: the away
// line while the pad is out of the bay with r1 1 (change-only, at most 2 Hz, the largest four first); the back line once when all are back.
bool mechanismPoseTick(uint64_t now) noexcept {
    if (!(g_sealState.reseal == 1 && g_sealState.mechanism) || g_sealPoseN == 0 || g_latch.n != g_sealPoseN) {
        g_poseAwayGate.reset();
        g_poseBackGate.reset();
        return false;
    }
    const uint32_t frame = g_frameStamp.load(std::memory_order_relaxed);
    struct Away {
        char name[48];
        float dM;
        float dDeg;
    };
    Away away[sealed::kLatchCap];
    size_t nAway = 0;
    bool allBack = true;
    for (size_t i = 0; i < g_sealPoseN; ++i) {
        const sealed::LatchRec& r = g_latch.recs[i];
        const SealPoseRec& p = g_sealPose[i];
        if (r.addr != p.addr || r.stamp != frame) return false;   // a record not read this frame: no comparison, no pose
        float dM = 0.0f;
        float dDeg = 0.0f;
        sealed::latchPoseDelta(r.rel, p.rel, r.ax0, p.ax0, r.ax2, p.ax2, &dM, &dDeg);
        if (sealed::latchPoseWithin(dM, dDeg)) continue;
        allBack = false;
        Away& a = away[nAway++];
        std::snprintf(a.name, sizeof(a.name), "%s", p.name);
        a.dM = dM;
        a.dDeg = dDeg;
    }
    const bool r1Up = g_game.r1Fresh && g_game.r1 == sealed::kReaderInHangar;
    if (allBack) {
        g_poseAwayGate.reset();
        if (r1Up && g_poseBackGate.changed(1)) {
            Log::get().note("optimized station: mechanism back at its sealed pose: all %u latched records within %.2f m and %.0f deg; re-seal allowed "
                            "after %u frames with r1 1.",
                            static_cast<unsigned>(g_sealPoseN), static_cast<double>(sealed::kPoseTolM), static_cast<double>(sealed::kPoseTolDeg),
                            static_cast<unsigned>(sealed::kPoseBackFrames));
        }
        return true;
    }
    if (!r1Up) return false;
    // The largest first, at most four.
    std::sort(away, away + nAway, [](const Away& x, const Away& y) { return x.dM > y.dM; });
    char text[400] = {};
    size_t used = 0;
    const size_t shown = nAway < 4 ? nAway : 4;
    for (size_t i = 0; i < shown; ++i) {
        const int w = std::snprintf(text + used, sizeof(text) - used, "%s%s %.2f m %.1f deg", i > 0 ? "; " : "", away[i].name,
                                    static_cast<double>(away[i].dM), static_cast<double>(away[i].dDeg));
        if (w < 0 || used + static_cast<size_t>(w) >= sizeof(text)) break;
        used += static_cast<size_t>(w);
    }
    const uint64_t key = sealed::textKey(text);
    if (now >= g_poseLastLogMs + 500 && g_poseAwayGate.changed(key)) {
        g_poseLastLogMs = now;
        Log::get().note("optimized station: mechanism away from its sealed pose: %s (largest first, at most 4)", text);
    }
    return false;
}

void optimizedStationFrameBody(uint32_t frame);

// The frame tick, timed every frame while the setting is on (built v14.24).
void optimizedStationFrame(uint32_t frame) {
    const bool on = g_setting.load(std::memory_order_acquire);
    const int64_t t0 = on ? costTicks() : 0;
    optimizedStationFrameBody(frame);
    if (on) costAdd(g_costFrame, costMs(t0, costTicks()));
}

void optimizedStationFrameBody(uint32_t frame) {
    if (++g_settingPoll >= kSettingPollFrames) {
        g_settingPoll = 0;
        const std::string text = Config::get().getString("fix.optimized_stations", "on");   // on in VR and in flat (built v14.34)
        const bool on = text != "off";
        if (on != g_setting.load(std::memory_order_acquire)) {
            g_setting.store(on, std::memory_order_release);
            applySetting(on, frame);
        }
    }
    const uint64_t now = GetTickCount64();
    if (!g_setting.load(std::memory_order_acquire)) {
        if (g_summaryStartMs == 0) g_summaryStartMs = now;
        return;
    }
    if (g_summaryStartMs == 0) g_summaryStartMs = now;
    // Built v14.2: the frame advances here, once per tick. The observer's promote and view resolve run once per frame off this stamp.
    g_frameStamp.fetch_add(1, std::memory_order_seq_cst);   // built v14.27: the callbacks that read the old stamp are the old pass's writers
    TimedStateLock lock;
    prodSync();   // built v14.26: the producer generation moves with the frame (the callbacks never move it)
    drainGather();          // the entries of the passes the stamp just closed, if every writer of them has left (built v14.27)
    resolveTickView();      // the camera from the pass's busiest context; held when the pass is empty or unfinished (built v14.27)
    ++g_winFrames;
    if (g_dockedNow.load(std::memory_order_acquire)) ++g_winDockedFrames;
    if (g_heldValid) ++g_winSealedFrames;
    // Built v14.34: the Status.json docked flag under either profile (the flat profile's journal accessors answer the no-journal values).
    g_dockedKnownNow.store(journalStatusDockedKnown(), std::memory_order_release);
    g_dockedNow.store(journalStatusDockedKnown() && journalStatusDocked(), std::memory_order_release);
    // Built v14.20 (review 4): an undock is a lifecycle point: the rig and collection tables start afresh.
    if (g_dockedWasNow && !g_dockedNow.load(std::memory_order_acquire)) {
        bumpRigEpoch();
        g_track.reset(padStateNowQpc());   // built v14.23: an undock ends the dock's own pad (the game names a new one on the next dock)
    }
    g_dockedWasNow = g_dockedNow.load(std::memory_order_acquire);
    storeHookGate();   // built v14.20 (review 7b): the store observer gathers only while docked (an atomic gate: no hook is installed here)
    gameStateTick();   // built v14.23: the own pad from the game, before the sun and the trigger read it
    refreshHeldWorld();
    // The camera against the held box: outside it by more than kCameraLeaveM releases the seal.
    bool cameraOutside = false;
    if (g_heldValid && g_frameCamValid) {
        if (g_heldCell.box) {
            float x[3] = {};
            sealed::boxCoordsOf(g_heldCell, g_frameCam, x);
            cameraOutside = !(std::fabs(x[0]) <= g_heldCell.boxHalf[0] + sealed::kCameraLeaveM &&
                              std::fabs(x[1]) <= g_heldCell.boxHalf[1] + sealed::kCameraLeaveM &&
                              std::fabs(x[2]) <= g_heldCell.boxHalf[2] + sealed::kCameraLeaveM);
        } else {
            cameraOutside = sealed::distance(g_heldCell.centre, g_frameCam) > g_heldCell.radius + sealed::kCameraLeaveM;
        }
    }
    // Built v14.6: a ship change (the own ship's type prefix, from the candidate record, changes for kShipChangeTicks readings in a row). It
    // releases the seal, resets the trigger and the safety net, and rebuilds the own-ship rule: a swapped ship in a closed bay then seals at once.
    if (g_ownCandidateDist >= 0.0f) {
        char curPrefix[32] = {};
        sealed::ownPrefixOf(g_ownCandidateName, curPrefix, sizeof(curPrefix));
        char oldPrefix[32] = {};
        std::snprintf(oldPrefix, sizeof(oldPrefix), "%s", g_shipWatch.prefix);
        if (sealed::shipPrefixNamesShip(curPrefix, prefixTallyOf(curPrefix)) && g_shipWatch.step(curPrefix)) {
            char why[160];
            std::snprintf(why, sizeof(why), "ship changed (%s -> %s)", oldPrefix, curPrefix);
            if (g_heldValid) {
                releaseSeal(why, frame);
            } else {
                Log::get().note("optimized station: %s; the trigger starts afresh", why);
            }
            g_sealState = sealed::SealState{};
            g_sealGuard = sealed::SealRateGuard{};
            g_preShellValid = false;
            setOwnPrefixAtEngage(frame);
            g_ownRecAddr = 0;
            g_ownLiveValid = false;
            g_latch.reset();   // a new ship: the mechanism is latched again
            bumpRigEpoch();    // built v14.20 (review 4): a ship change is a lifecycle point
        }
    }
    // Built v14.5: the height above the shell along the pad's own up axis, read live, with the shell picked once per pad (the same pick as the
    // freeze, from the gathered parts). The camera in the candidate box is the seal's other condition.
    // Built v14.9: a pad absent for fewer than kHeldLostFrames frames keeps its last reading (no still-clock reset).
    const bool padHeld = !g_pickCell.has && g_ownSticky.pad != 0 && g_ownSticky.unseen > 0 && g_ownSticky.unseen < sealed::kHeldLostFrames;
    if (!padHeld) {
        g_relOk = false;
        g_shellNowValid = false;
    }
    g_frameShell = sealed::ShellFrame{};   // built v14.17: the pad in the shell's frame, read when the height is
    if (g_pickCell.has) {
        g_relOk = false;
        if (!g_hangarWatch.giveUp && (!g_preShellValid || g_preShellPad != g_pickCell.pad)) {
            const size_t pick = sealed::pickShell(g_explicits, g_explicitCount, g_pickCell.centre, g_pickCell.size, 150.0f);
            g_preShellValid = pick < g_explicitCount;
            g_preShellPad = g_pickCell.pad;
            if (g_preShellValid) {
                g_preShellAddr = g_explicits[pick].addr;
                std::snprintf(g_preShellName, sizeof(g_preShellName), "%s", g_explicits[pick].name);
            }
        }
        if (g_preShellValid) {
            char nm[48] = {};
            if (!cachedName(g_preShellAddr, nm, sizeof(nm)) || std::strcmp(nm, g_preShellName) != 0) {
                g_preShellValid = false;   // the record moved on: the next tick picks again
            } else {
                const WorldRead pw = worldOfRecord(g_pickCell.pad);
                const WorldRead sw = worldOfRecord(g_preShellAddr);
                float axes[3][3] = {};
                float ext[3] = {};
                bool extOk = false;
                uint8_t src = 4;
                if (pw.source != 4 && sw.source != 4 && poseOrientOf(g_pickCell.pad, axes, ext, &extOk, &src)) {
                    const float up[3] = {axes[2][0], axes[2][1], axes[2][2]};
                    g_relM = sealed::shellRelative(pw.world, sw.world, up);
                    g_relOk = true;
                    g_shellNow[0] = sw.world[0];
                    g_shellNow[1] = sw.world[1];
                    g_shellNow[2] = sw.world[2];
                    g_shellNowSrc = sw.source;
                    g_shellNowValid = true;
                    // The pad in the shell's frame (the sealed line's lateral and yaw).
                    float shAxes[3][3] = {};
                    float shExt[3] = {};
                    bool shExtOk = false;
                    uint8_t shSrc = 4;
                    if (poseOrientOf(g_preShellAddr, shAxes, shExt, &shExtOk, &shSrc)) {
                        g_frameShell = sealed::shellFrameOf(pw.world, sw.world, up, shAxes[0], axes[0]);
                    }
                }
            }
        }
    }
    bool camIn = false;
    if (g_frameCamValid && g_pickCell.has) {
        if (g_pickCell.box) {
            float x[3] = {};
            sealed::boxCoordsOf(g_pickCell, g_frameCam, x);
            camIn = std::fabs(x[0]) <= g_pickCell.boxHalf[0] && std::fabs(x[1]) <= g_pickCell.boxHalf[1] && std::fabs(x[2]) <= g_pickCell.boxHalf[2];
        } else {
            camIn = sealed::distance(g_pickCell.centre, g_frameCam) <= g_pickCell.radius;
        }
    }
    // Built v14.12: the latch is kept while docked, unsealed, after a descent; the candidates of this frame are consumed here.
    g_watchNowMs = now;
    g_bayWatching = bayWatchKeep();
    g_bayNoting = g_bayWatching && g_shellNowValid;
    if (!g_bayWatching) {
        g_latch.reset();
    }
    // Built v14.20 (review 2): a genuine change of the unsealed bay's pad or shell clears the latch and its motion references; it is regathered below.
    // A sample that misses the pad keeps the identity (a brief miss is no change).
    if (g_latch.latched && !g_sealedFlag.load(std::memory_order_acquire) &&
        g_latchId.step(g_pickCell.has, g_pickCell.pad, g_preShellValid, g_preShellAddr)) {
        g_latch.reset();
        Log::get().note("optimized station: the bay changed (pad %s): its mechanism is latched again", g_pickCell.padName);
    }
    g_latchPending = g_bayWatching && !g_latch.latched;
    if (g_latchPending && g_pickCell.has) latchNow(now);
    g_latchCandN = 0;
    if (g_latch.latched && g_bayNoting && g_passInformative) latchRead(now);
    // Built v14.33: while sealed and not yet armed, every informative pass in which every latched record was read and none moved counts toward the arm.
    if (g_sealedFlag.load(std::memory_order_acquire) && !g_mechArm.armed && g_passInformative && g_latch.latched) {
        const uint32_t frameNow = g_frameStamp.load(std::memory_order_relaxed);
        bool allStill = true;
        for (size_t i = 0; i < g_latch.n; ++i) {
            if (g_latch.recs[i].stamp != frameNow || g_latch.recs[i].movedRun != 0) allStill = false;
        }
        if (g_mechArm.step(allStill)) {
            snapshotSealPose();   // the sealed poses are the poses of this still bay
            Log::get().note("optimized station: mechanism release armed: %u records still for 1 s.", static_cast<unsigned>(g_latch.n));
        }
    }   // built v14.27: no sample from an empty or unfinished pass (the next sample re-bases)
    // Built v14.14: a latched mechanism record moved on two frames in a row while sealed: the launch releases the seal.
    g_mechanismMoved = false;
    if (g_sealedFlag.load(std::memory_order_acquire) && g_mechArm.armed) {   // built v14.33: only once the bay has been still for 60 passes
        const int mi = g_latch.movedIndex();
        if (mi >= 0) {
            if (mi == 0) std::snprintf(g_releaseWhy, sizeof(g_releaseWhy), "%s", "the pad started moving: launch");
            else std::snprintf(g_releaseWhy, sizeof(g_releaseWhy), "the bay mechanism moved (%s): launch", g_latch.recs[mi].name);
            g_mechanismMoved = true;
        }
    }
    // Built v14.13: the hangar watch (a sized shell near the pad) and its once-per-dock names; the sun inside the hangar (its own state).
    const bool shellFound = g_preShellValid;
    const bool padChanged = g_pickCell.has && g_pickCell.pad != g_hangarPad;
    if (g_pickCell.has) {
        g_hangarPad = g_pickCell.pad;
        for (int k = 0; k < 3; ++k) g_namesPadC[k] = g_pickCell.centre[k];
    }
    if (g_hangarWatch.step(shellFound, g_dockedNow.load(std::memory_order_acquire), padChanged)) hangarNote();
    if (!g_dockedNow.load(std::memory_order_acquire)) g_nameLogN = 0;
    g_namesPending = g_dockedNow.load(std::memory_order_acquire) && !shellFound && g_pickCell.has && !g_hangarWatch.logged;
    {
        sealed::SunInputs su;
        su.setting = g_setting.load(std::memory_order_acquire);
        su.docked = g_dockedNow.load(std::memory_order_acquire);
        su.ownKnown = g_game.ownKnown;
        su.r0Fresh = g_game.r0Fresh;
        su.r0 = static_cast<uint8_t>(g_game.r0 < 0 ? 0 : g_game.r0);
        su.r1Fresh = g_game.r1Fresh;
        su.inHangar = g_game.r1 == sealed::kReaderInHangar;
        su.relOk = g_relOk;
        su.rel = g_relM;
        su.hangarChanged = g_flagHangarChanged;
        const char* why = "";
        if (sealed::sunStep(su, &g_sun, &why)) {
            g_sunOff.store(g_sun.off, std::memory_order_release);
            // Built v14.23: the sun line carries the own pad's r0 and r1 and the Status docked flag.
            const char* dockedText = g_dockedNow.load(std::memory_order_acquire) ? "yes" : "no";
            char r0Text[16];
            char r1Text[16];
            if (g_game.r0 < 0) std::snprintf(r0Text, sizeof(r0Text), "-");
            else std::snprintf(r0Text, sizeof(r0Text), "%d", g_game.r0);
            if (g_game.r1 < 0) std::snprintf(r1Text, sizeof(r1Text), "-");
            else std::snprintf(r1Text, sizeof(r1Text), "%d", g_game.r1);
            if (g_sun.off) {
                Log::get().note("optimized station: sun light inside the hangar off (%s; r0=%s r1=%s; Status docked=%s)", why, r0Text, r1Text, dockedText);
            } else {
                Log::get().note("optimized station: sun light inside the hangar back on (%s; r0=%s r1=%s; Status docked=%s)", why, r0Text, r1Text,
                                dockedText);
            }
        }
    }
    sealed::SealInputs in;
    in.setting = true;   // the tick returns above while the setting is off
    in.docked = g_dockedNow;
    in.ownKnown = g_game.ownKnown;
    in.poseBack = mechanismPoseTick(now);   // built v14.24: the latched records back at their sealed poses (the mechanism re-seal)
    in.r1Fresh = g_game.r1Fresh;
    in.inHangar = g_game.r1 == sealed::kReaderInHangar;
    in.pickOk = g_pickCell.has && g_preShellValid;   // no sized shell: the trigger stays idle (built v14.13)
    in.padHeld = padHeld;
    in.mechanismMoved = g_mechanismMoved;
    in.cameraInBox = camIn;
    in.pad = g_pickCell.has ? g_pickCell.pad : 0;
    in.held = g_heldValid;
    in.cameraOutside = cameraOutside;
    in.padMoved = g_flagPadMoved;
    if (g_flagHangarChanged) g_sealGuard = sealed::SealRateGuard{};   // a new hangar: the safety net starts afresh
    if (g_flagHangarChanged) {   // a new hangar: the trigger, the latch and the own pad start afresh (built v14.23)
        g_sealState = sealed::SealState{};
        g_preShellValid = false;
        g_latch.reset();
        g_track.reset(padStateNowQpc());
        bumpRigEpoch();   // built v14.20 (review 4): a hangar change is a lifecycle point
    }
    in.hangarChanged = g_flagHangarChanged;
    in.launched = false;   // the pad state object's reader is not in this build: the launch is the r1 edge and the mechanism
    in.nowMs = now;
    sealed::ReleaseWhy why = sealed::ReleaseWhy::kNone;
    const sealed::SealAction act = sealed::sealStep(in, &g_sealState, &why);
    g_flagPadMoved = false;
    g_flagHangarChanged = false;
    {
        // Built v14.20 (review 1): the trigger withdrew its request (the pad rose, the camera left, the pad changed): the pending engage is cancelled.
        sealed::EngageRequest er{g_engagePending, g_engagePad, g_engageShell};
        sealed::engageTrigger(&er, g_sealState);
        g_engagePending = er.pending;
    }
    if (act == sealed::SealAction::kRelease) {
        releaseSeal(releaseText(why), frame);
    } else if (act == sealed::SealAction::kRequestEngage) {
        if (g_sealGuard.tripped) {
            // refused until a hangar change or a setting toggle (the safety net, built v14.4)
        } else if (g_sealGuard.note(now)) {
            Log::get().note("optimized station: sealing stopped for this hangar: more than %u seals in a minute (the pad flickers). "
                            "It resumes at a hangar change or a setting toggle.", static_cast<unsigned>(sealed::kSealRateMax));
        } else {
            g_engagePending = true;
            g_engagePad = in.pad;          // built v14.20 (review 1): bound to the pad and the shell this request was made for
            g_engageShell = g_preShellValid ? g_preShellAddr : 0;   // the shell the request is bound to (built v14.20)
            g_engageTries = 0;
        }
    }
    padStateTick(now, g_track.have ? g_track.own : 0);   // built v14.23: the own pad's change lines and the seal or release line (log only)
    // Built v14.23: the waiting line, while docked and unsealed: one line when a key changes, and at most every 10 s while docked.
    if (!g_heldValid) {
        sealed::GameView w;
        w.dockedKnown = g_dockedKnownNow.load(std::memory_order_acquire);
        w.docked = g_dockedNow.load(std::memory_order_acquire);
        w.ownKnown = g_game.ownKnown;
        w.own = g_game.own;
        w.r0 = g_game.r0;
        w.r1 = g_game.r1;
        w.r0Fresh = g_game.r0Fresh;
        w.r1Fresh = g_game.r1Fresh;
        w.cameraKnown = g_frameCamValid;
        w.cameraIn = camIn;
        w.cellBuilt = g_pickCell.has && g_preShellValid && sealed::cellInvalidWhy(g_pickCell) == nullptr;
        if (!w.cellBuilt) {
            if (!g_preShellValid) std::snprintf(w.cellReason, sizeof(w.cellReason), "%s", g_hangarWhy);
            else if (!g_pickCell.has) std::snprintf(w.cellReason, sizeof(w.cellReason), "%s", g_pickCell.reason);
            else std::snprintf(w.cellReason, sizeof(w.cellReason), "%s", sealed::cellInvalidWhy(g_pickCell));
        }
        // Built v14.27: a change is written once it has held kWaitStableTicks ticks (the key flipping every frame wrote 231 to 800 lines per 30 s in v14.26).
        const bool candChanged = !g_waitCandSet || sealed::gameKeyChanged(g_waitCand, w);
        if (candChanged) {
            g_waitCand = w;
            g_waitCandSet = true;
        }
        const bool stable = g_waitStable.step(candChanged, sealed::kWaitStableTicks);
        const bool changed = !g_waitLastSet || (stable && sealed::gameKeyChanged(g_waitLast, g_waitCand));
        const bool periodic = w.docked && g_waitLastSet && now - g_waitLastMs >= 10000;
        if (changed || periodic) {
            char line[480] = {};
            if (changed) g_waitLast = g_waitCand;   // the periodic line repeats the last stable key
            sealed::gameWaitingLine(g_waitLast, line, sizeof(line));
            Log::get().note("%s", line);
            ++g_waitingLinesWritten;   // built v14.24: the waiting lines written this window (the summary's count)
            g_waitLastSet = true;
            g_waitLastMs = now;
        }
    }
    {
        // Built v14.26: the pads, explicit parts and camera of the pass just drained make the cell for the next pass (the observer's promote, moved here).
        const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
        // Built v14.27: only an informative pass makes the cell. An empty or unfinished one leaves the pick, the pad and the windows as they were.
        if (g_passInformative && sealed::framePromoteDue(g_cellStamp, stamp)) promoteFrame(stamp, stamp);
    }
    if (g_passInformative && g_ownSticky.pad != 0) {
        if (g_stickyPadInPass) ++g_winPadSeen;
        else ++g_winPadMissed;
        if (!g_padWin.has(g_ownSticky.pad)) ++g_winPadAbsentWindow;
    }
    // Built v14.27: the rig verdicts are republished from an informative pass only; the last publication stands otherwise. Without a held cell the
    // publication is emptied at once (the veto answers only while sealed).
    if (g_passInformative || !g_heldValid) refreshRigVerdicts();
    publishSnapshot();   // built v14.25: the tick's view for the per-record path (under the lock)
    if (now - g_summaryStartMs >= kSummaryMs) summaryLine(now);
    (void)frame;
}

}  // namespace edvr
