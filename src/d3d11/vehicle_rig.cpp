// The player vehicle's rigs, and the vehicle frame they supply (vehicle_rig.h; the pure half and the offsets are vehicle_rig_core.h, the ring and the gate vehicle_frame.h). It hooks the camera's
// SetTarget to learn the player's ship and cockpit rigs, re-reads each rig's record array from the game's own data every few ms, and records the pose of the cockpit rig's root at the emit.
// It writes nothing into the game: every read is SEH-guarded, no draw, state or shader changes. The one thing it calls in the game is GetComponent (the entity's own virtual lookup, exactly as
// SetTarget does a few instructions earlier), and only on the thread that is inside SetTarget.
//
// LOG LINES (all prefixed "vehicle_rig"):
//   vehicle_rig: installed ...                    the hook went in (build 334925, found by signature)
//   vehicle_rig: not installed (...)              not build 334925 / the signature or a data address did not resolve / the prologue differs / no relay memory: nothing patched
//   vehicle_rig target ship=.. cockpit=..         the player's vehicle changed (one line per change, not per SetTarget call)
//   vehicle_rig ready role=.. name=".." ...       a ship or cockpit rig first reads ready: records, root sphere; the cockpit's two largest record radii
//   vehicle_rig rig role=.. status=..             a rig read went wrong: a fault, a vtable mismatch, an absurd count or a bad array; capped
//   vehicle_rig_pose_frame / vehicle_rig_pose     at "flat pixels: copied" (Shift+NumLock): each ship and cockpit record's pose bits, joinable to the pool by exact bits; capped
//   vehicle_rig: ... (every 30 s)                 "no target seen", or the counts: targets, faults caught, vtable mismatches, absurd counts, and the refresh's cost
//
// THE VEHICLE FRAME (the flat profile's use of this, issue 186). The emit tap records the pose of each recent COCKPIT RIG's root record (index 0) at the emit, keyed by the present-frame clock the
// emit folds into its markers, in a small lock-free ring per rig (vehicle_frame.h, which says why more than one). The flat resolve takes this frame's entry and last frame's from the newest rig that
// has both (vehicleRigFrameTake); a root's sphere is read when its rig is first seen. A build that is not 334925, a hook that could not be installed, or no such rig mean no frame, and the resolve is the old path.
#include "vehicle_rig.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"
#include "engine_velocity.h"
#include "kinematic_eval_hook.h"
#include "vehicle_frame.h"
#include "vehicle_rig_core.h"

namespace edvr {
namespace {

namespace vr = vrig;

constexpr uint32_t kSetTargetRva = 0x1096640u;
constexpr uint32_t kRigTokenRva = 0x5F05D38u;     // *(u32*) = the IKinematicRig token
constexpr uint32_t kRigVtableRva = 0x55956B0u;    // the concrete IKinematicRig vtable
// mov [rsp+10h],rbx; push rdi; sub rsp,20h; mov rdi,rcx; mov rbx,rdx
constexpr uint8_t kPrologue[16] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0xDA};
constexpr size_t kRelayBytes = 44, kOriginalLiteral = 36;
constexpr uint32_t kPoseLinesPerCapture = 240;
constexpr uint32_t kStatusLineCap = 64;

using SetTargetFn = uintptr_t(__fastcall*)(void* self, void* handle);
using GetComponentFn = uintptr_t(__fastcall*)(uintptr_t container, uint32_t token);

__declspec(noinline) uintptr_t __fastcall setTargetDetour(void* self, void* handle) noexcept;

alignas(8) std::atomic<uintptr_t> g_gate{0};
static_assert(decltype(g_gate)::is_always_lock_free, "The relay reads the aligned atomic cell directly.");

struct Detour {
    std::atomic<uintptr_t> forward{0};
    CodeHook hook;
    uint8_t* relay = nullptr;
};
Detour g_detour;
std::atomic<bool> g_attempted{false};
uintptr_t g_base = 0;
uintptr_t g_rigVtable = 0, g_rigTokenAddr = 0;   // the running build's addresses, set once at install before the hook goes live
std::atomic<bool> g_live{false};
int64_t g_ticksPerSec = 1;

// ---- counters ----------------------------------------------------------------------------------------------------------------------------------------------------------------
std::atomic<uint64_t> g_faultsCaught{0};
std::atomic<uint64_t> g_vtableMismatches{0};
std::atomic<uint64_t> g_absurd{0};
std::atomic<uint64_t> g_targetCalls{0};         // every SetTarget call
std::atomic<uint64_t> g_targetChanges{0};       // the calls that named another vehicle
std::atomic<uint64_t> g_refreshes{0};
std::atomic<uint64_t> g_refreshTicks{0};
std::atomic<uint64_t> g_lastRefreshTicks{0};
std::atomic<bool> g_refreshBusy{false};
std::atomic<uint32_t> g_statusLines{0};

int64_t ticks() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// ---- the SEH memory policy ---------------------------------------------------------------------------------------------------------------------------------------------------
struct SehMem {
    bool read(void* dst, uintptr_t src, size_t n) noexcept {
        __try {
            std::memcpy(dst, reinterpret_cast<const void*>(src), n);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_faultsCaught.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
};
SehMem g_mem;

uintptr_t getComponent(uintptr_t entity, uint32_t token) noexcept {
    __try {
        const uintptr_t container = entity + 8;
        const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(container);
        const auto fn = *reinterpret_cast<const GetComponentFn*>(vtable);
        return fn(container, token);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faultsCaught.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
}

// ---- the tracked rigs (written by the SetTarget hook, read by whoever refreshes) -------------------------------------------------------------------------------------------
// Four words per rig: handle block, entity, interface, role. A seqlock; one writer at a time (a spin flag around plain stores, never held across a game call).
struct Slot {
    uint64_t h = 0, E = 0, I = 0, role = 0;
};
std::atomic<uint64_t> g_trk[vr::kRigs][4];
std::atomic<uint32_t> g_trkSeq{0}, g_trkCount{0};
std::atomic_flag g_trkWriter = ATOMIC_FLAG_INIT;

void publishTracked(const Slot* slots, uint32_t n) noexcept {
    while (g_trkWriter.test_and_set(std::memory_order_acquire)) {}
    if (n > vr::kRigs) n = vr::kRigs;
    const uint32_t s = g_trkSeq.load(std::memory_order_relaxed);
    g_trkSeq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_trkCount.store(n, std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i) {
        g_trk[i][0].store(slots[i].h, std::memory_order_relaxed);
        g_trk[i][1].store(slots[i].E, std::memory_order_relaxed);
        g_trk[i][2].store(slots[i].I, std::memory_order_relaxed);
        g_trk[i][3].store(slots[i].role, std::memory_order_relaxed);
    }
    g_trkSeq.store(s + 2, std::memory_order_release);
    g_trkWriter.clear(std::memory_order_release);
}
bool readTracked(Slot* out, uint32_t* n) noexcept {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t s1 = g_trkSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        uint32_t count = g_trkCount.load(std::memory_order_relaxed);
        if (count > vr::kRigs) count = vr::kRigs;
        for (uint32_t i = 0; i < count; ++i) {
            out[i].h = g_trk[i][0].load(std::memory_order_relaxed);
            out[i].E = g_trk[i][1].load(std::memory_order_relaxed);
            out[i].I = g_trk[i][2].load(std::memory_order_relaxed);
            out[i].role = g_trk[i][3].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_trkSeq.load(std::memory_order_relaxed) == s1) {
            *n = count;
            return true;
        }
    }
    *n = 0;
    return false;
}

// ---- the refresh: each known rig re-read ---------------------------------------------------------------------------------------------------------------------------------
// One at a time (g_refreshBusy). Its state below is touched only while that flag is held.
struct RigMemo {
    uint64_t key = 0;            // h, E, I of the target the ready line was written for
    bool readyLogged = false;
    vr::Status status = vr::Status::kOk;
};
RigMemo g_memo[vr::kRigs];

// ---- the vehicle frame: the recent cockpit rigs, each with its root, pose ring and sphere ---------------------------------------------------------------------------------------
// vframe::kRecentRigs says why more than one.
struct CockpitRig {
    vframe::Ring ring;
    std::atomic<uint64_t> root{0};            // the root record's address (the first element of the rig's array); 0: not ready
    std::atomic<uint64_t> order{0};           // larger: the camera target named it more recently
    std::atomic<uint32_t> gen{0};             // moved whenever this slot's rig, its handle or its record array changes (0: never used)
    std::atomic<uint32_t> sphereBits[4] = {}; // the root's sphere in its own frame: centre xyz, radius, as float bits
    std::atomic<uint64_t> nameWords[6] = {};  // the rig's name, 48 bytes
    // refresh-owned
    uint64_t h = 0, E = 0, I = 0;             // the rig; I == 0: an empty slot
    uint64_t key = 0;
    uint64_t unreadKey = 0;                   // the rig and array whose root sphere last failed to read, and when
    int64_t unreadAt = 0;
};
CockpitRig g_cockpits[vframe::kRecentRigs];
uint64_t g_cockpitOrder = 0;                  // refresh-owned
std::atomic<uint32_t> g_cockpitGenNext{0};
std::atomic<uint64_t> g_frameAsked{0}, g_frameWith{0}, g_frameFromOlder{0}, g_frameMissing{0}, g_frameNotAdjacent{0}, g_frameChanged{0};

// The old root goes before the generation moves, and the tap reads the generation before the root: a tap holding the new generation can only see no root or the new one, so one
// generation's poses all come from one rig. The new root goes last, so a reader that sees it sees the whole sphere.
void retireCockpit(CockpitRig& c) noexcept {
    c.root.store(0, std::memory_order_release);
    c.gen.store(g_cockpitGenNext.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_release);
}

void publishCockpit(CockpitRig& c, uint64_t root, const float sphere[4], const char* name) noexcept {
    retireCockpit(c);
    for (int i = 0; i < 4; ++i) {
        uint32_t b = 0;
        std::memcpy(&b, &sphere[i], 4);
        c.sphereBits[i].store(b, std::memory_order_relaxed);
    }
    uint64_t w[6] = {};
    if (name) std::memcpy(w, name, strnlen(name, 47));
    for (int i = 0; i < 6; ++i) c.nameWords[i].store(w[i], std::memory_order_relaxed);
    c.root.store(root, std::memory_order_release);
}

uint64_t mixKey(uint64_t h, uint64_t e, uint64_t i) noexcept { return (h * 0x9E3779B97F4A7C15ull) ^ (e * 0xC2B2AE3D27D4EB4Full) ^ (i * 0x165667B19E3779F9ull); }

const char* roleOf(const Slot& s) noexcept { return vr::roleName(static_cast<uint32_t>(s.role)); }

// A rig first reads ready: its name, record count and root sphere; for the cockpit rig the two largest record radii (whether a loose root sphere comes from one mesh).
void logReady(const Slot& s, const vr::RigRead& rr) noexcept {
    vr::RigInfo info;
    vr::describeRig(g_mem, rr, s.role == vr::kCockpit, &info);
    char sphere[96] = "n/a";
    if (info.haveRoot) std::snprintf(sphere, sizeof(sphere), "(%.4f,%.4f,%.4f,%.4f)", info.rootSphere[0], info.rootSphere[1], info.rootSphere[2], info.rootSphere[3]);
    char radii[120] = "";
    if (s.role == vr::kCockpit) {
        if (info.topCount == 0) std::snprintf(radii, sizeof(radii), " largest_r=n/a");
        else if (info.topCount == 1) std::snprintf(radii, sizeof(radii), " largest_r=(i=%llu,%.4f)", static_cast<unsigned long long>(info.topIndex[0]), info.topRadius[0]);
        else std::snprintf(radii, sizeof(radii), " largest_r=(i=%llu,%.4f),(i=%llu,%.4f)", static_cast<unsigned long long>(info.topIndex[0]), info.topRadius[0],
                           static_cast<unsigned long long>(info.topIndex[1]), info.topRadius[1]);
    }
    Log::get().note("vehicle_rig ready role=%s rigName=\"%s\" records=%llu rootSphere=%s%s", roleOf(s), info.haveName ? info.name : "?", static_cast<unsigned long long>(rr.count), sphere, radii);
}

// A cockpit rig that is ready: when its array is a new one read its root's sphere once and publish the root.
void trackCockpit(CockpitRig& c, const vr::RigRead& rr) noexcept {
    const uint64_t ck = mixKey(c.h, c.E, c.I) ^ (rr.lo * 0x9E3779B97F4A7C15ull) ^ (rr.count * 0xC2B2AE3D27D4EB4Full);
    if (ck == c.key && c.root.load(std::memory_order_relaxed) == rr.lo) return;
    // A rig whose root sphere did not read is tried again once a second and reported once, not on every refresh.
    const int64_t now = ticks();
    if (ck == c.unreadKey && now - c.unreadAt < g_ticksPerSec) return;
    c.key = ck;
    vr::RigInfo info;
    vr::describeRig(g_mem, rr, false, &info);
    if (!info.haveRoot || !std::isfinite(info.rootSphere[3]) || !(info.rootSphere[3] > 0) || !std::isfinite(info.rootSphere[0]) || !std::isfinite(info.rootSphere[1]) ||
        !std::isfinite(info.rootSphere[2])) {
        retireCockpit(c);
        const bool first = ck != c.unreadKey;
        c.unreadKey = ck;
        c.unreadAt = now;
        if (first && g_statusLines.fetch_add(1, std::memory_order_relaxed) < kStatusLineCap)
            Log::get().note("vehicle_rig: the cockpit rig's root sphere could not be read (or is not a sphere); no vehicle frame for this rig until it can");
        return;
    }
    c.unreadKey = 0;
    publishCockpit(c, rr.lo, info.rootSphere, info.haveName ? info.name : "ShipCockpit");
}

// The rig the target names is the newest in the list: one already there moves up, a new one takes an empty slot or the oldest's.
void adoptCockpit(const Slot& s) noexcept {
    CockpitRig* oldest = &g_cockpits[0];
    for (CockpitRig& c : g_cockpits) {
        if (c.I == s.I && c.h == s.h && c.E == s.E) {
            if (c.order.load(std::memory_order_relaxed) != g_cockpitOrder) c.order.store(++g_cockpitOrder, std::memory_order_release);
            return;
        }
        if (oldest->I && (!c.I || c.order.load(std::memory_order_relaxed) < oldest->order.load(std::memory_order_relaxed))) oldest = &c;
    }
    CockpitRig& c = *oldest;
    retireCockpit(c);
    c.h = s.h;
    c.E = s.E;
    c.I = s.I;
    c.key = 0;
    c.unreadKey = 0;
    c.order.store(++g_cockpitOrder, std::memory_order_release);
}

// The rigs with a root, newest first; the generation before the root, as the tap reads them.
uint32_t recentCockpits(CockpitRig** rigs, uint32_t* gens) noexcept {
    uint64_t orders[vframe::kRecentRigs] = {};
    uint32_t n = 0;
    for (CockpitRig& c : g_cockpits) {
        const uint32_t gen = c.gen.load(std::memory_order_acquire);
        if (!c.root.load(std::memory_order_acquire)) continue;
        const uint64_t order = c.order.load(std::memory_order_acquire);
        uint32_t at = n++;
        for (; at > 0 && orders[at - 1] < order; --at) {
            orders[at] = orders[at - 1];
            rigs[at] = rigs[at - 1];
            gens[at] = gens[at - 1];
        }
        orders[at] = order;
        rigs[at] = &c;
        gens[at] = gen;
    }
    return n;
}

// Every recent rig is re-read: one that is ready keeps its root current, one that is not has no root, and one that is gone for good leaves the list.
void refreshCockpits() noexcept {
    for (CockpitRig& c : g_cockpits) {
        if (!c.I) continue;
        vr::RigRead rr;
        const vr::Status st = vr::readRig(g_mem, c.h, c.E, c.I, g_rigVtable, &rr);
        if (st == vr::Status::kOk) {
            trackCockpit(c, rr);
            continue;
        }
        if (c.root.load(std::memory_order_relaxed)) retireCockpit(c);
        c.key = 0;
        if (st == vr::Status::kEntityGone || st == vr::Status::kNoHandle || st == vr::Status::kFault || st == vr::Status::kVtableMismatch) c.h = c.E = c.I = 0;
    }
}

bool wrongRead(vr::Status st) noexcept {
    return st == vr::Status::kFault || st == vr::Status::kVtableMismatch || st == vr::Status::kAbsurd || st == vr::Status::kBadArray;
}

void refresh() noexcept {
    if (g_refreshBusy.exchange(true, std::memory_order_acquire)) return;
    const int64_t t0 = ticks();
    Slot slots[vr::kRigs];
    uint32_t n = 0;
    if (readTracked(slots, &n)) {
        for (uint32_t i = 0; i < n; ++i) {
            vr::RigRead rr;
            const vr::Status st = vr::readRig(g_mem, slots[i].h, slots[i].E, slots[i].I, g_rigVtable, &rr);
            if (st == vr::Status::kVtableMismatch) g_vtableMismatches.fetch_add(1, std::memory_order_relaxed);
            if (st == vr::Status::kAbsurd) g_absurd.fetch_add(1, std::memory_order_relaxed);
            RigMemo& memo = g_memo[i];
            const uint64_t key = mixKey(slots[i].h, slots[i].E, slots[i].I);
            if (memo.key != key) memo = RigMemo{};
            memo.key = key;
            if (memo.status != st) {
                memo.status = st;
                if (wrongRead(st) && g_statusLines.fetch_add(1, std::memory_order_relaxed) < kStatusLineCap) {
                    if (st == vr::Status::kAbsurd)
                        Log::get().note("vehicle_rig rig role=%s status=%s: the record count %llu is past %llu, so the read at *(*(I+0x2F0)+0x298) is wrong; nothing was walked", roleOf(slots[i]),
                                        vr::statusName(st), static_cast<unsigned long long>(rr.count), static_cast<unsigned long long>(vr::kMaxRecords));
                    else
                        Log::get().note("vehicle_rig rig role=%s status=%s", roleOf(slots[i]), vr::statusName(st));
                }
            }
            if (st == vr::Status::kOk && !memo.readyLogged) {
                memo.readyLogged = true;
                logReady(slots[i], rr);
            }
            if (slots[i].role == vr::kCockpit) adoptCockpit(slots[i]);
        }
        refreshCockpits();
    }
    g_refreshes.fetch_add(1, std::memory_order_relaxed);
    g_refreshTicks.fetch_add(static_cast<uint64_t>(ticks() - t0), std::memory_order_relaxed);
    g_refreshBusy.store(false, std::memory_order_release);
}

// ---- the emit tap (job threads, only while the game's evaluator gate is open for engine motion) -----------------------------------------------------------------------
void emitTap(uintptr_t record) noexcept {
    for (CockpitRig& c : g_cockpits) {
        const uint32_t gen = c.gen.load(std::memory_order_acquire);   // before the root: retireCockpit
        if (c.root.load(std::memory_order_acquire) != record || !record) continue;
        // A recent cockpit rig's root, right after the game's own emit: its pose is what the pool holds. Keyed by the clock the emit folds into its markers.
        vframe::Pose pose;
        if (g_mem.read(pose.pos, static_cast<uintptr_t>(record + vr::kRecPos), 12) && g_mem.read(pose.quat, static_cast<uintptr_t>(record + vr::kRecQuat), 8))
            c.ring.record(engineVelocityPresentClock(), gen, pose);
        break;
    }
    thread_local uint32_t t_calls = 0;
    if ((++t_calls & 15u) == 0) {
        // Once a frame, roughly: the first tap to find the last refresh older than 4 ms takes it.
        const int64_t now = ticks();
        uint64_t last = g_lastRefreshTicks.load(std::memory_order_relaxed);
        if (static_cast<uint64_t>(now) - last >= static_cast<uint64_t>(g_ticksPerSec / 250) &&
            g_lastRefreshTicks.compare_exchange_strong(last, static_cast<uint64_t>(now), std::memory_order_relaxed))
            refresh();
    }
}

// ---- the flat pixel capture tap (render thread) ---------------------------------------------------------------------------------------------------------------------------
void captureTap(uint64_t frame, const float* eye) noexcept {
    if (!eye || !std::isfinite(eye[0]) || !std::isfinite(eye[1]) || !std::isfinite(eye[2])) {
        Log::get().note("vehicle_rig_pose_frame frame=%llu the capture's camera row 5 (the eye) is not finite; no pose lines", static_cast<unsigned long long>(frame));
        return;
    }
    Slot slots[vr::kRigs];
    uint32_t n = 0;
    if (!readTracked(slots, &n) || n == 0) {
        Log::get().note("vehicle_rig_pose_frame frame=%llu no rig is tracked (no SetTarget seen, or none read); no pose lines", static_cast<unsigned long long>(frame));
        return;
    }
    uint32_t budget = kPoseLinesPerCapture;
    Log::get().note("vehicle_rig_pose_frame frame=%llu eye=(%.6f,%.6f,%.6f) tracked=%u scale=1 assumed for eye_in_rec; slot=-1: the emit does not know the pool slot, join by pos_bits+quat_bits",
                    static_cast<unsigned long long>(frame), eye[0], eye[1], eye[2], n);
    for (uint32_t i = 0; i < n; ++i) {
        vr::RigRead rr;
        const vr::Status st = vr::readRig(g_mem, slots[i].h, slots[i].E, slots[i].I, g_rigVtable, &rr);
        if (st != vr::Status::kOk) {
            Log::get().note("vehicle_rig_pose_frame frame=%llu role=%s status=%s: no records read", static_cast<unsigned long long>(frame), roleOf(slots[i]), vr::statusName(st));
            continue;
        }
        uint64_t logged = 0;
        for (uint64_t k = 0; k < rr.count; ++k) {
            if (budget == 0) break;
            const uint64_t rec = rr.lo + k * vr::kRecordStride;
            vr::PoseRead p;
            if (!vr::readPose(g_mem, rec, &p)) continue;
            float pos[3], out[3];
            vr::bitsToFloats(p.posBits, pos);
            vr::eyeInRec(pos, p.quatBits, eye, out);
            char scale[24] = "n/a";
            if (p.haveScale) std::snprintf(scale, sizeof(scale), "%.4f", p.xfScale);
            Log::get().note("vehicle_rig_pose role=%s i=%llu slot=-1 pos_bits=%08x%08x%08x quat_bits=%08x%08x eye_in_rec=(%.4f,%.4f,%.4f) xf_scale=%s", roleOf(slots[i]),
                            static_cast<unsigned long long>(k), p.posBits[0], p.posBits[1], p.posBits[2], p.quatBits[0], p.quatBits[1], out[0], out[1], out[2], scale);
            ++logged;
            --budget;
        }
        if (logged < rr.count)
            Log::get().note("vehicle_rig_pose_frame frame=%llu role=%s CAPPED: %llu of %llu records logged (cap %u lines per capture)", static_cast<unsigned long long>(frame),
                            roleOf(slots[i]), static_cast<unsigned long long>(logged), static_cast<unsigned long long>(rr.count), kPoseLinesPerCapture);
    }
}

// ---- the hook: what SetTarget just did ---------------------------------------------------------------------------------------------------------------------------------------
struct HandleRead {
    uint64_t h = 0, E = 0, I = 0;
    int32_t state = -1;
    bool rigVtableOk = false;
};
HandleRead readHandle(uint64_t h, uint32_t rigToken) noexcept {
    HandleRead r;
    r.h = h;
    if (!h) return r;
    int32_t st = -1;
    uint64_t e = 0;
    if (g_mem.read(&st, static_cast<uintptr_t>(h + vr::kHandleState), 4)) r.state = st;
    if (g_mem.read(&e, static_cast<uintptr_t>(h + vr::kHandleEntity), 8)) r.E = e;
    if (r.E) r.I = getComponent(r.E, rigToken);
    uint64_t vt = 0;
    r.rigVtableOk = r.I && g_mem.read(&vt, static_cast<uintptr_t>(r.I), 8) && vt == g_rigVtable;
    return r;
}

uint64_t g_lastTargetKey[2] = {};   // the hook thread's: the ship's and the cockpit's handle, entity and interface, as last published

void onTarget(uintptr_t self) noexcept {
    g_targetCalls.fetch_add(1, std::memory_order_relaxed);
    uint32_t rigToken = 0;
    uint64_t hs[2] = {};
    if (!g_mem.read(&rigToken, g_rigTokenAddr, 4) || !g_mem.read(&hs[0], self + vr::kTargetShip, 8) || !g_mem.read(&hs[1], self + vr::kTargetCockpit, 8)) {
        if (g_statusLines.fetch_add(1, std::memory_order_relaxed) < kStatusLineCap)
            Log::get().note("vehicle_rig target: the camera target's handles or the token could not be read");
        return;
    }
    const HandleRead ship = readHandle(hs[0], rigToken), cockpit = readHandle(hs[1], rigToken);
    const uint64_t shipKey = mixKey(ship.h, ship.E, ship.I), cockpitKey = mixKey(cockpit.h, cockpit.E, cockpit.I);
    if (shipKey == g_lastTargetKey[0] && cockpitKey == g_lastTargetKey[1]) return;   // the same vehicle: nothing to publish or say
    g_lastTargetKey[0] = shipKey;
    g_lastTargetKey[1] = cockpitKey;
    Slot slots[vr::kRigs];
    uint32_t n = 0;
    auto add = [&](const HandleRead& h, uint64_t role) {
        if (!h.h || !h.E || !h.I || n >= vr::kRigs) return;
        slots[n].h = h.h;
        slots[n].E = h.E;
        slots[n].I = h.I;
        slots[n].role = role;
        ++n;
    };
    add(ship, vr::kShip);
    add(cockpit, vr::kCockpit);
    publishTracked(slots, n);
    // The first refresh happens here too: the ready lines then follow at once.
    g_lastRefreshTicks.store(static_cast<uint64_t>(ticks()), std::memory_order_relaxed);
    refresh();
    const uint64_t changes = g_targetChanges.fetch_add(1, std::memory_order_relaxed) + 1;
    Log::get().note("vehicle_rig target #%llu ship=%p(s=%d) cockpit=%p(s=%d) E=%p,%p I=%p,%p%s%s tracked=%u", static_cast<unsigned long long>(changes), reinterpret_cast<void*>(ship.h), ship.state,
                    reinterpret_cast<void*>(cockpit.h), cockpit.state, reinterpret_cast<void*>(ship.E), reinterpret_cast<void*>(cockpit.E), reinterpret_cast<void*>(ship.I),
                    reinterpret_cast<void*>(cockpit.I), ship.I && !ship.rigVtableOk ? " SHIP-VTABLE-MISMATCH" : "", cockpit.I && !cockpit.rigVtableOk ? " COCKPIT-VTABLE-MISMATCH" : "", n);
}

__declspec(noinline) uintptr_t __fastcall setTargetDetour(void* self, void* handle) noexcept {
    const auto forward = reinterpret_cast<SetTargetFn>(g_detour.forward.load(std::memory_order_acquire));
    if (!forward) return 0;
    // The game's own work first, and its result returned unchanged.
    const uintptr_t result = forward(self, handle);
    onTarget(reinterpret_cast<uintptr_t>(self));
    return result;
}

// ---- the relay (the light grid cache's, in the same shape): mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback]; original: jmp [trampoline] -------------------------------
uint8_t* allocateRelay(uintptr_t target) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t floorAddr = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t ceiling = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t distance = uintptr_t(INT32_MAX) - 0x10000u;
    uintptr_t at = target > distance ? target - distance : floorAddr;
    if (at < floorAddr) at = floorAddr;
    const uintptr_t limit = target > ceiling - distance ? ceiling : target + distance;
    while (at < limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region))) break;
        const uintptr_t start = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > UINTPTR_MAX - start) break;
        const uintptr_t end = start + region.RegionSize;
        if (region.State == MEM_FREE) {
            uintptr_t candidate = at > start ? at : start;
            if (candidate > UINTPTR_MAX - (granularity - 1)) break;
            candidate = (candidate + granularity - 1) & ~(granularity - 1);
            if (candidate < limit && candidate < end && end - candidate >= 4096) {
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    // SetTarget's first instruction is mov [rsp+10h],rbx; RAX is not an argument, so the gate test may use it; the detour sees rcx and rdx as the caller left them.
    const uint8_t body[kRelayBytes] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x83, 0x38, 0, 0x74, 0x0E, 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                       0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &gateAddress, 8);
    std::memcpy(code + 22, &callbackAddress, 8);
}

bool prepareRelay(void* trampoline, void* context) noexcept {
    auto& d = *static_cast<Detour*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(d.relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(d.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) || !FlushInstructionCache(GetCurrentProcess(), d.relay, kRelayBytes)) return false;
    d.forward.store(address, std::memory_order_release);
    return true;
}

// ---- poll state ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint64_t g_lastSummaryMs = 0;

}  // namespace

void vehicleRigInstall() {
    if (g_attempted.exchange(true, std::memory_order_acq_rel)) return;
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_base) {
        Log::get().note("vehicle_rig: not installed (the module base is unknown); nothing patched.");
        return;
    }
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    g_ticksPerSec = f.QuadPart > 0 ? f.QuadPart : 1;
    const bool identityRead = gameBuildIdentity(nullptr, nullptr);
    const uintptr_t at = identityRead ? gameRva(kSetTargetRva) : 0;
    if (!identityRead || !gameBuildKnown()) {
        // The handle, rig and record offsets (vehicle_rig_core.h) are build 334925's and no signature can find them: on any other build nothing is read.
        Log::get().note("vehicle_rig: not installed: %s is not build 334925; the offsets this reads through are that build's, so it reads nothing and patches "
                        "nothing (SetTarget %s by signature).", gameBuildText(), at ? "was found" : "was not found");
        return;
    }
    if (!at) {
        Log::get().note("vehicle_rig: not installed: SetTarget (0x1096640) did not resolve to exactly one site; nothing patched.");
        return;
    }
    if (!gameRva(kRigTokenRva) || !gameRva(kRigVtableRva)) {
        Log::get().note("vehicle_rig: not installed: the IKinematicRig token or vtable (0x5F05D38, 0x55956B0) was not found by signature; nothing patched.");
        return;
    }
    g_rigTokenAddr = g_base + gameRva(kRigTokenRva);
    g_rigVtable = g_base + gameRva(kRigVtableRva);
    uint8_t got[sizeof(kPrologue)] = {};
    bool readOk = false;
    __try {
        std::memcpy(got, reinterpret_cast<const void*>(g_base + at), sizeof(got));
        readOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        readOk = false;
    }
    if (!readOk || !gamePinMatches(kSetTargetRva, got, kPrologue, sizeof(got))) {
        Log::get().note("vehicle_rig: not installed: the SetTarget prologue at 0x%llX is not the pinned one (or is unreadable); nothing patched.", static_cast<unsigned long long>(at));
        return;
    }
    g_detour.relay = allocateRelay(g_base + at);
    if (!g_detour.relay) {
        Log::get().note("vehicle_rig: not installed: no memory for the relay within range of the game; nothing patched.");
        return;
    }
    buildRelay(g_detour.relay, &g_gate, reinterpret_cast<void*>(&setTargetDetour));
    if (!g_detour.hook.install(reinterpret_cast<void*>(g_base + at), g_detour.relay, nullptr, "vehicle-rig-target", &prepareRelay, &g_detour)) {
        VirtualFree(g_detour.relay, 0, MEM_RELEASE);
        g_detour.relay = nullptr;
        Log::get().note("vehicle_rig: not installed: CodeHook refused SetTarget (its own line says why); nothing patched.");
        return;
    }
    g_live.store(true, std::memory_order_release);
    kinematicEvalSetVehicleRigTap(&emitTap);
    g_vehicleRigCaptureTap.store(&captureTap, std::memory_order_release);
    g_gate.store(1, std::memory_order_release);
    Log::get().note("vehicle_rig: installed: SetTarget (0x1096640, found by signature at 0x%llX) forwards first and returns the game's result unchanged; reads only (SEH-guarded reads, no writes into the "
                    "game). The cockpit rig's root pose is taken at the emit, which only runs while the game's evaluator gate is open for engine motion (temporal AA on); pose lines come at "
                    "\"flat pixels: copied\".", static_cast<unsigned long long>(at));
}

vframe::Why vehicleRigFrameTake(uint32_t stamp, VehicleFrameRead* out) {
    g_frameAsked.fetch_add(1, std::memory_order_relaxed);
    vframe::Why why = vframe::Why::kMissing;
    if (g_live.load(std::memory_order_acquire)) {
        CockpitRig* rigs[vframe::kRecentRigs] = {};
        uint32_t gens[vframe::kRecentRigs] = {};
        const uint32_t n = recentCockpits(rigs, gens);
        const vframe::Ring* rings[vframe::kRecentRigs] = {};
        for (uint32_t i = 0; i < n; ++i) rings[i] = &rigs[i]->ring;
        uint32_t chosen = 0;
        why = vframe::selectRecent(rings, gens, n, stamp, &out->frame, &chosen);
        if (why == vframe::Why::kOk) {
            CockpitRig& c = *rigs[chosen];
            for (int i = 0; i < 4; ++i) {
                const uint32_t b = c.sphereBits[i].load(std::memory_order_relaxed);
                std::memcpy(&out->sphere[i], &b, 4);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
            if (c.gen.load(std::memory_order_acquire) != gens[chosen] || !c.root.load(std::memory_order_acquire)) why = vframe::Why::kChanged;
            else if (chosen != 0) g_frameFromOlder.fetch_add(1, std::memory_order_relaxed);
        }
    }
    switch (why) {
        case vframe::Why::kOk: g_frameWith.fetch_add(1, std::memory_order_relaxed); break;
        case vframe::Why::kMissing: g_frameMissing.fetch_add(1, std::memory_order_relaxed); break;
        case vframe::Why::kNotAdjacent: g_frameNotAdjacent.fetch_add(1, std::memory_order_relaxed); break;
        default: g_frameChanged.fetch_add(1, std::memory_order_relaxed); break;
    }
    return why;
}

void vehicleRigFrameWindow(VehicleFrameWindow* out) {
    out->asked = g_frameAsked.exchange(0, std::memory_order_relaxed);
    out->with = g_frameWith.exchange(0, std::memory_order_relaxed);
    out->missing = g_frameMissing.exchange(0, std::memory_order_relaxed);
    out->notAdjacent = g_frameNotAdjacent.exchange(0, std::memory_order_relaxed);
    out->changed = g_frameChanged.exchange(0, std::memory_order_relaxed);
    out->fromOlder = g_frameFromOlder.exchange(0, std::memory_order_relaxed);
    out->live = g_live.load(std::memory_order_acquire);
    // The newest rig that has a root.
    CockpitRig* rigs[vframe::kRecentRigs] = {};
    uint32_t gens[vframe::kRecentRigs] = {};
    if (recentCockpits(rigs, gens)) {
        const CockpitRig& c = *rigs[0];
        uint64_t w[6];
        for (int k = 0; k < 6; ++k) w[k] = c.nameWords[k].load(std::memory_order_relaxed);
        std::memcpy(out->name, w, 48);
        out->name[47] = 0;
        const uint32_t b = c.sphereBits[3].load(std::memory_order_relaxed);
        std::memcpy(&out->sphereRadius, &b, 4);
        out->rigKnown = true;
    }
}

void vehicleRigPoll(uint64_t nowMs) {
    if (!g_live.load(std::memory_order_acquire)) return;
    if (!g_lastSummaryMs) g_lastSummaryMs = nowMs;
    // The emit tap refreshes the rigs about once a frame while it runs; this keeps them read (and writes the ready lines) when the gate is shut.
    if (g_targetCalls.load(std::memory_order_relaxed)) {
        g_lastRefreshTicks.store(static_cast<uint64_t>(ticks()), std::memory_order_relaxed);
        refresh();
    }
    if (nowMs - g_lastSummaryMs >= 30000) {
        g_lastSummaryMs = nowMs;
        const uint64_t refreshes = g_refreshes.load(std::memory_order_relaxed);
        const double refreshUs = refreshes ? 1e6 * static_cast<double>(g_refreshTicks.load(std::memory_order_relaxed)) / static_cast<double>(g_ticksPerSec) / static_cast<double>(refreshes) : 0.0;
        char tail[160];
        std::snprintf(tail, sizeof(tail), "faults_caught=%llu vtable_mismatch=%llu absurd_count=%llu; cost: refresh %.1f us avg over %llu",
                      static_cast<unsigned long long>(g_faultsCaught.load(std::memory_order_relaxed)), static_cast<unsigned long long>(g_vtableMismatches.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_absurd.load(std::memory_order_relaxed)), refreshUs, static_cast<unsigned long long>(refreshes));
        if (!g_targetCalls.load(std::memory_order_relaxed))
            Log::get().note("vehicle_rig: no target seen (SetTarget has not been called since the hook went in); %s", tail);
        else
            Log::get().note("vehicle_rig: targets=%llu changes=%llu; %s", static_cast<unsigned long long>(g_targetCalls.load(std::memory_order_relaxed)),
                            static_cast<unsigned long long>(g_targetChanges.load(std::memory_order_relaxed)), tail);
    }
}

}  // namespace edvr
