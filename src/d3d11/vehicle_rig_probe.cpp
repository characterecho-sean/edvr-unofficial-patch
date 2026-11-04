// The vehicle rig probe's glue (vehicle_rig_probe.h; the pure half and the offsets are vehicle_rig_core.h). READ-ONLY: every read of the game is SEH-guarded, nothing is written into the
// game, no draw, state or shader changes. The one thing it calls in the game is GetComponent (the entity's own virtual lookup, exactly as SetTarget does a few instructions earlier),
// and only on the thread that is inside SetTarget.
//
// LOG LINES (all prefixed "vehicle_rig"):
//   vehicle_rig: installed ...                    the hook went in (build 334925, found by signature)
//   vehicle_rig: not installed (...)              the key is off / not 334925 / the signature did not resolve to one site / the prologue differs: nothing patched
//   vehicle_rig target this=.. ship=..            every SetTarget call, after the game's own work
//   vehicle_rig module i=.. name=".."             the ship's module list, once per target (capped)
//   vehicle_rig rig role=.. status=..             a rig's status changed (first evaluation included); capped
//   vehicle_rig ready role=.. name=".." ...       a rig first reads ready: records, stride, root sphere, world position; + ready_box and ready_world lines
//   vehicle_rig emits ship=N cockpit=N ...        every 5 s while the counts change
//   vehicle_rig_pose_frame / vehicle_rig_pose     at "flat pixels: copied": each ship and cockpit record's pose bits, joinable to the pool by exact bits; capped
//   vehicle_rig: ... (every 30 s)                 "no target seen", or the counts: targets, rigs ready, faults caught, vtable mismatches, rigs not ready; and whether the emit gate was open
#include "vehicle_rig_probe.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/game_build.h"
#include "../common/log.h"
#include "journal_watch.h"
#include "kinematic_eval_hook.h"
#include "vehicle_rig_core.h"

namespace edvr {
namespace {

namespace vr = vrig;

constexpr uint32_t kSetTargetRva = 0x1096640u;
constexpr uint32_t kRigTokenRva = 0x5F05D38u;     // *(u32*) = the IKinematicRig token
constexpr uint32_t kModsTokenRva = 0x5F2F274u;    // *(u32*) = the vehicle modules token
constexpr uint32_t kRigVtableRva = 0x55956B0u;    // the concrete IKinematicRig vtable
constexpr uint32_t kModsVtableRva = 0x559CF58u;   // the concrete modules vtable
// mov [rsp+10h],rbx; push rdi; sub rsp,20h; mov rdi,rcx; mov rbx,rdx
constexpr uint8_t kPrologue[16] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0xDA};
constexpr size_t kRelayBytes = 44, kOriginalLiteral = 36;
constexpr uint32_t kMaxModuleLines = 16;
constexpr uint32_t kPoseLinesPerCapture = 240;
constexpr uint32_t kTransitionLineCap = 64;

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
uintptr_t g_rigVtable = 0, g_modsVtable = 0, g_rigTokenAddr = 0, g_modsTokenAddr = 0;   // the running build's addresses, set once at install before the hook goes live
std::atomic<bool> g_live{false};
int64_t g_ticksPerSec = 1;

// ---- counters ----------------------------------------------------------------------------------------------------------------------------------------------------------------
vr::RoleTable g_table;
vr::Counts g_emits;
std::atomic<uint64_t> g_tapCalls{0};            // every call of the emit tap: zero over a window = the emit gate was shut
std::atomic<uint64_t> g_faultsCaught{0};
std::atomic<uint64_t> g_vtableMismatches{0};
std::atomic<uint64_t> g_absurd{0};
std::atomic<uint64_t> g_targets{0};
std::atomic<uint64_t> g_refreshes{0};
std::atomic<uint64_t> g_refreshTicks{0};
std::atomic<uint64_t> g_emitSamples{0};
std::atomic<uint64_t> g_emitSampleTicks{0};
std::atomic<uint32_t> g_readyNow{0}, g_notReadyNow{0}, g_rigsNow{0};
std::atomic<uint64_t> g_lastRefreshTicks{0};
std::atomic<bool> g_refreshBusy{false};
std::atomic<uint32_t> g_transitionLines{0};

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
// Eight words per rig: handle block, entity, interface, role, then 32 bytes of name. A seqlock; one writer at a time (a spin flag around plain stores, never held across a game call).
struct Slot {
    uint64_t h = 0, E = 0, I = 0, role = 0;
    char name[33] = {};
};
std::atomic<uint64_t> g_trk[vr::kMaxRigs][8];
std::atomic<uint32_t> g_trkSeq{0}, g_trkCount{0};
std::atomic_flag g_trkWriter = ATOMIC_FLAG_INIT;

void publishTracked(const Slot* slots, uint32_t n) noexcept {
    while (g_trkWriter.test_and_set(std::memory_order_acquire)) {}
    if (n > vr::kMaxRigs) n = vr::kMaxRigs;
    const uint32_t s = g_trkSeq.load(std::memory_order_relaxed);
    g_trkSeq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_trkCount.store(n, std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i) {
        g_trk[i][0].store(slots[i].h, std::memory_order_relaxed);
        g_trk[i][1].store(slots[i].E, std::memory_order_relaxed);
        g_trk[i][2].store(slots[i].I, std::memory_order_relaxed);
        g_trk[i][3].store(slots[i].role, std::memory_order_relaxed);
        uint64_t w[4] = {};
        std::memcpy(w, slots[i].name, 32);
        for (int k = 0; k < 4; ++k) g_trk[i][4 + k].store(w[k], std::memory_order_relaxed);
    }
    g_trkSeq.store(s + 2, std::memory_order_release);
    g_trkWriter.clear(std::memory_order_release);
}
bool readTracked(Slot* out, uint32_t* n) noexcept {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t s1 = g_trkSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        uint32_t count = g_trkCount.load(std::memory_order_relaxed);
        if (count > vr::kMaxRigs) count = vr::kMaxRigs;
        for (uint32_t i = 0; i < count; ++i) {
            out[i].h = g_trk[i][0].load(std::memory_order_relaxed);
            out[i].E = g_trk[i][1].load(std::memory_order_relaxed);
            out[i].I = g_trk[i][2].load(std::memory_order_relaxed);
            out[i].role = g_trk[i][3].load(std::memory_order_relaxed);
            uint64_t w[4];
            for (int k = 0; k < 4; ++k) w[k] = g_trk[i][4 + k].load(std::memory_order_relaxed);
            std::memcpy(out[i].name, w, 32);
            out[i].name[32] = 0;
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

// ---- the refresh: each known rig re-read, the role table published -----------------------------------------------------------------------------------------------------------
// One at a time (g_refreshBusy). Its state below is touched only while that flag is held.
struct RigMemo {
    bool seen = false;
    vr::Status status = vr::Status::kOk;
    uint64_t key = 0;        // h, E, I of the target the ready line was written for
    bool readyLogged = false;
    uint64_t lo = 0, count = 0;
};
RigMemo g_memo[vr::kMaxRigs];

uint64_t mixKey(uint64_t h, uint64_t e, uint64_t i) noexcept { return (h * 0x9E3779B97F4A7C15ull) ^ (e * 0xC2B2AE3D27D4EB4Full) ^ (i * 0x165667B19E3779F9ull); }

const char* slotRole(const Slot& s) noexcept { return vr::roleName(static_cast<uint32_t>(s.role)); }

void logReady(const Slot& s, const vr::RigRead& rr) noexcept {
    vr::RigInfo info;
    vr::describeRig(g_mem, rr, &info);
    float worldPos[3] = {};
    if (info.haveWorld) std::memcpy(worldPos, &info.world[12], 12);
    char sphere[96] = "n/a";
    if (info.haveRoot) std::snprintf(sphere, sizeof(sphere), "(%.4f,%.4f,%.4f,%.4f)", info.rootSphere[0], info.rootSphere[1], info.rootSphere[2], info.rootSphere[3]);
    char world[96] = "n/a";
    if (info.haveWorld) std::snprintf(world, sizeof(world), "(%.4f,%.4f,%.4f)", worldPos[0], worldPos[1], worldPos[2]);
    Log::get().note("vehicle_rig ready role=%s name=\"%s\" rigName=\"%s\" records=%llu stride=0x%llX rootSphere=%s worldPos(R+0x60)=%s R=%p arr=[%p,%p) h=%p E=%p I=%p",
                    slotRole(s), s.name, info.haveName ? info.name : "?", static_cast<unsigned long long>(rr.count),
                    static_cast<unsigned long long>(vr::kRecordStride), sphere, world, reinterpret_cast<void*>(rr.R), reinterpret_cast<void*>(rr.lo),
                    reinterpret_cast<void*>(rr.hi), reinterpret_cast<void*>(s.h), reinterpret_cast<void*>(s.E), reinterpret_cast<void*>(s.I));
    if (info.spheresRead) {
        Log::get().note("vehicle_rig ready_box role=%s spheres=%u/%llu skipped=%u parentIsRigWorld=%u nodeBox(c*L +-r)=x[%.3f,%.3f] y[%.3f,%.3f] z[%.3f,%.3f] maxDist=%.3f "
                        "xfScale=[%.4f,%.4f] (in the frame of the records' parent matrix; radii unscaled)",
                        slotRole(s), info.spheresRead, static_cast<unsigned long long>(rr.count), info.spheresSkipped, info.parentIsRig, info.boxMin[0], info.boxMax[0],
                        info.boxMin[1], info.boxMax[1], info.boxMin[2], info.boxMax[2], info.maxDist, info.scaleMin, info.scaleMax);
    } else {
        Log::get().note("vehicle_rig ready_box role=%s no node sphere could be read (%u of %llu records skipped): the sphere chain offsets are not confirmed by this flight", slotRole(s),
                        info.spheresSkipped, static_cast<unsigned long long>(rr.count));
    }
    if (info.haveWorld) {
        Log::get().note("vehicle_rig ready_world role=%s R+0x30=[%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]", slotRole(s), info.world[0],
                        info.world[1], info.world[2], info.world[3], info.world[4], info.world[5], info.world[6], info.world[7], info.world[8], info.world[9], info.world[10],
                        info.world[11], info.world[12], info.world[13], info.world[14], info.world[15]);
    }
}

void refresh() noexcept {
    if (g_refreshBusy.exchange(true, std::memory_order_acquire)) return;
    const int64_t t0 = ticks();
    Slot slots[vr::kMaxRigs];
    uint32_t n = 0;
    if (readTracked(slots, &n)) {
        vr::RoleTable::Range ranges[vr::kMaxRigs];
        uint32_t m = 0, ready = 0, notReady = 0;
        for (uint32_t i = 0; i < n; ++i) {
            vr::RigRead rr;
            const vr::Status st = vr::readRig(g_mem, slots[i].h, slots[i].E, slots[i].I, g_rigVtable, &rr);
            if (st == vr::Status::kVtableMismatch) g_vtableMismatches.fetch_add(1, std::memory_order_relaxed);
            if (st == vr::Status::kAbsurd) g_absurd.fetch_add(1, std::memory_order_relaxed);
            RigMemo& memo = g_memo[i];
            const uint64_t key = mixKey(slots[i].h, slots[i].E, slots[i].I);
            if (memo.key != key) memo = RigMemo{};
            memo.key = key;
            if (!memo.seen || memo.status != st) {
                memo.seen = true;
                memo.status = st;
                if (g_transitionLines.fetch_add(1, std::memory_order_relaxed) < kTransitionLineCap) {
                    if (st == vr::Status::kAbsurd)
                        Log::get().note("vehicle_rig rig role=%s status=%s: the record count %llu is past %llu, so the read at *(*(I+0x2F0)+0x298) is wrong; nothing was walked",
                                        slotRole(slots[i]), vr::statusName(st), static_cast<unsigned long long>(rr.count), static_cast<unsigned long long>(vr::kMaxRecords));
                    else
                        Log::get().note("vehicle_rig rig role=%s name=\"%s\" status=%s", slotRole(slots[i]), slots[i].name, vr::statusName(st));
                }
            }
            if (st == vr::Status::kOk || st == vr::Status::kEmpty) {
                ++ready;
                if (st == vr::Status::kOk) {
                    ranges[m].lo = rr.lo;
                    ranges[m].hi = rr.hi;
                    ranges[m].role = static_cast<uint32_t>(slots[i].role);
                    ++m;
                }
                if (!memo.readyLogged && st == vr::Status::kOk) {
                    memo.readyLogged = true;
                    memo.lo = rr.lo;
                    memo.count = rr.count;
                    logReady(slots[i], rr);
                } else if (memo.readyLogged && (memo.lo != rr.lo || memo.count != rr.count)) {
                    if (g_transitionLines.fetch_add(1, std::memory_order_relaxed) < kTransitionLineCap)
                        Log::get().note("vehicle_rig rearranged role=%s records %llu -> %llu arr %p -> %p", slotRole(slots[i]), static_cast<unsigned long long>(memo.count),
                                        static_cast<unsigned long long>(rr.count), reinterpret_cast<void*>(memo.lo), reinterpret_cast<void*>(rr.lo));
                    memo.lo = rr.lo;
                    memo.count = rr.count;
                }
            } else {
                ++notReady;
            }
        }
        g_table.publish(ranges, m);
        g_rigsNow.store(n, std::memory_order_relaxed);
        g_readyNow.store(ready, std::memory_order_relaxed);
        g_notReadyNow.store(notReady, std::memory_order_relaxed);
    }
    g_refreshes.fetch_add(1, std::memory_order_relaxed);
    g_refreshTicks.fetch_add(static_cast<uint64_t>(ticks() - t0), std::memory_order_relaxed);
    g_refreshBusy.store(false, std::memory_order_release);
}

// ---- the emit tap (job threads, only while the game's evaluator gate is open for engine motion) -----------------------------------------------------------------------
void emitTap(uintptr_t record) noexcept {
    thread_local uint32_t t_calls = 0;
    const uint32_t n = ++t_calls;
    const bool sample = (n & 255u) == 0;
    const int64_t t0 = sample ? ticks() : 0;
    g_tapCalls.fetch_add(1, std::memory_order_relaxed);
    g_emits.note(g_table.classify(record));
    if ((n & 15u) == 0) {
        // Once a frame, roughly: the first tap to find the last refresh older than 4 ms takes it.
        const int64_t now = ticks();
        uint64_t last = g_lastRefreshTicks.load(std::memory_order_relaxed);
        if (static_cast<uint64_t>(now) - last >= static_cast<uint64_t>(g_ticksPerSec / 250) &&
            g_lastRefreshTicks.compare_exchange_strong(last, static_cast<uint64_t>(now), std::memory_order_relaxed))
            refresh();
    }
    if (sample) {
        g_emitSamples.fetch_add(1, std::memory_order_relaxed);
        g_emitSampleTicks.fetch_add(static_cast<uint64_t>(ticks() - t0), std::memory_order_relaxed);
    }
}

// ---- the flat pixel capture tap (render thread) ---------------------------------------------------------------------------------------------------------------------------
void captureTap(uint64_t frame, const float* eye) noexcept {
    if (!eye || !std::isfinite(eye[0]) || !std::isfinite(eye[1]) || !std::isfinite(eye[2])) {
        Log::get().note("vehicle_rig_pose_frame frame=%llu the capture's camera row 5 (the eye) is not finite; no pose lines", static_cast<unsigned long long>(frame));
        return;
    }
    Slot slots[vr::kMaxRigs];
    uint32_t n = 0;
    if (!readTracked(slots, &n) || n == 0) {
        Log::get().note("vehicle_rig_pose_frame frame=%llu no rig is tracked (no SetTarget seen, or none read); no pose lines", static_cast<unsigned long long>(frame));
        return;
    }
    uint32_t budget = kPoseLinesPerCapture;
    Log::get().note("vehicle_rig_pose_frame frame=%llu eye=(%.6f,%.6f,%.6f) tracked=%u scale=1 assumed for eye_in_rec; slot=-1: the emit does not know the pool slot, join by pos_bits+quat_bits",
                    static_cast<unsigned long long>(frame), eye[0], eye[1], eye[2], n);
    for (uint32_t i = 0; i < n; ++i) {
        if (slots[i].role != vr::kShip && slots[i].role != vr::kCockpit) continue;
        vr::RigRead rr;
        const vr::Status st = vr::readRig(g_mem, slots[i].h, slots[i].E, slots[i].I, g_rigVtable, &rr);
        if (st != vr::Status::kOk) {
            Log::get().note("vehicle_rig_pose_frame frame=%llu role=%s status=%s: no records read", static_cast<unsigned long long>(frame), slotRole(slots[i]), vr::statusName(st));
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
            Log::get().note("vehicle_rig_pose role=%s i=%llu slot=-1 pos_bits=%08x%08x%08x quat_bits=%08x%08x eye_in_rec=(%.4f,%.4f,%.4f) xf_scale=%s", slotRole(slots[i]),
                            static_cast<unsigned long long>(k), p.posBits[0], p.posBits[1], p.posBits[2], p.quatBits[0], p.quatBits[1], out[0], out[1], out[2], scale);
            ++logged;
            --budget;
        }
        if (logged < rr.count)
            Log::get().note("vehicle_rig_pose_frame frame=%llu role=%s CAPPED: %llu of %llu records logged (cap %u lines per capture)", static_cast<unsigned long long>(frame),
                            slotRole(slots[i]), static_cast<unsigned long long>(logged), static_cast<unsigned long long>(rr.count), kPoseLinesPerCapture);
    }
}

// ---- the hook: what SetTarget just did ---------------------------------------------------------------------------------------------------------------------------------------
uint64_t g_distinctThis[8] = {};
uint32_t g_distinctCount = 0;   // the hook thread's (SetTarget is the game thread's); an approximation if two threads call it

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

void onTarget(uintptr_t self) noexcept {
    uint32_t rigToken = 0, modsToken = 0;
    uint64_t hs[3] = {};
    if (!g_mem.read(&rigToken, g_rigTokenAddr, 4) || !g_mem.read(&modsToken, g_modsTokenAddr, 4) || !g_mem.read(&hs[0], self + vr::kTargetShip, 8) ||
        !g_mem.read(&hs[1], self + vr::kTargetCockpit, 8) || !g_mem.read(&hs[2], self + vr::kTargetTurret, 8)) {
        Log::get().note("vehicle_rig target this=%p: the camera target's handles or the tokens could not be read", reinterpret_cast<void*>(self));
        g_targets.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const HandleRead ship = readHandle(hs[0], rigToken), cockpit = readHandle(hs[1], rigToken), turret = readHandle(hs[2], rigToken);
    Slot slots[vr::kMaxRigs];
    uint32_t n = 0;
    auto add = [&](const HandleRead& h, uint64_t role, const char* name) {
        if (!h.h || !h.E || !h.I || n >= vr::kMaxRigs) return;
        slots[n].h = h.h;
        slots[n].E = h.E;
        slots[n].I = h.I;
        slots[n].role = role;
        std::snprintf(slots[n].name, sizeof(slots[n].name), "%s", name);
        ++n;
    };
    add(ship, vr::kShip, "ship");
    add(cockpit, vr::kCockpit, "ShipCockpit");
    add(turret, vr::kTurret, "Turret");
    // The ship's own module list: every module with a rig of its own (the cockpit and the turret are two of them, already added).
    uint32_t moduleLines = 0, moduleCount = 0, moduleRigs = 0;
    if (ship.E) {
        const uintptr_t J = getComponent(static_cast<uintptr_t>(ship.E), modsToken);
        uint64_t jvt = 0;
        if (J && g_mem.read(&jvt, J, 8) && jvt == g_modsVtable) {
            uint64_t count = 0, namesTable = 0, handleArray = 0;
            if (g_mem.read(&count, J + vr::kModsCount, 8) && g_mem.read(&namesTable, J + vr::kModsNames, 8) && g_mem.read(&handleArray, J + vr::kModsHandles, 8) && handleArray && count <= 64) {
                moduleCount = static_cast<uint32_t>(count);
                uint32_t nameCount = 0;
                uint64_t nameArray = 0;
                if (namesTable) {
                    g_mem.read(&nameCount, static_cast<uintptr_t>(namesTable + vr::kModsNameCount), 4);
                    g_mem.read(&nameArray, static_cast<uintptr_t>(namesTable + vr::kModsNameArray), 8);
                }
                for (uint32_t i = 0; i < count; ++i) {
                    uint64_t mh = 0;
                    if (!g_mem.read(&mh, static_cast<uintptr_t>(handleArray + i * vr::kModsHandleStride), 8) || !mh) continue;
                    char name[33] = "?";
                    if (nameArray && i < nameCount) {
                        uint64_t np = 0;
                        if (g_mem.read(&np, static_cast<uintptr_t>(nameArray + i * vr::kModsNameStride), 8) && np) {
                            size_t got = 0;
                            while (got < 32 && g_mem.read(&name[got], static_cast<uintptr_t>(np + got), 1) && name[got]) {
                                const unsigned char c = static_cast<unsigned char>(name[got]);
                                if (c < 0x20 || c >= 0x7F || c == '"') name[got] = '?';
                                ++got;
                            }
                            name[got] = 0;
                        }
                    }
                    const HandleRead mod = readHandle(mh, rigToken);
                    const bool isCockpit = mh == hs[1], isTurret = mh == hs[2], isShip = mh == hs[0];
                    if (mod.I) ++moduleRigs;
                    if (moduleLines < kMaxModuleLines) {
                        ++moduleLines;
                        Log::get().note("vehicle_rig module i=%u name=\"%s\" h=%p(s=%d) E=%p I=%p%s", i, name, reinterpret_cast<void*>(mh), mod.state, reinterpret_cast<void*>(mod.E),
                                        reinterpret_cast<void*>(mod.I), isCockpit ? " (the cockpit)" : isTurret ? " (the turret)" : isShip ? " (the ship)" : mod.I ? "" : " (no rig)");
                    }
                    if (!isCockpit && !isTurret && !isShip) add(mod, vr::kModule, name);
                }
            }
        } else {
            Log::get().note("vehicle_rig target: the ship's module interface J=%p is not the modules vtable; no module rigs tracked", reinterpret_cast<void*>(J));
        }
    }
    publishTracked(slots, n);
    // The first refresh happens here too: the target line then carries what the rigs read as at this instant.
    g_lastRefreshTicks.store(static_cast<uint64_t>(ticks()), std::memory_order_relaxed);
    refresh();
    bool novel = true;
    for (uint32_t i = 0; i < g_distinctCount; ++i) novel = novel && g_distinctThis[i] != self;
    if (novel && g_distinctCount < 8) g_distinctThis[g_distinctCount++] = self;
    const uint64_t count = g_targets.fetch_add(1, std::memory_order_relaxed) + 1;
    Log::get().note("vehicle_rig target #%llu tid=%lu this=%p ship=%p(s=%d) cockpit=%p(s=%d) turret=%p(s=%d) E=%p,%p,%p I=%p,%p,%p%s%s%s journal_srv_scorpion=%d "
                    "(the journal latch knows only the Scorpion; anything else reads 0) tracked=%u modules=%u(with rig %u) distinct_this=%u",
                    static_cast<unsigned long long>(count), static_cast<unsigned long>(GetCurrentThreadId()), reinterpret_cast<void*>(self), reinterpret_cast<void*>(ship.h), ship.state,
                    reinterpret_cast<void*>(cockpit.h), cockpit.state, reinterpret_cast<void*>(turret.h), turret.state, reinterpret_cast<void*>(ship.E), reinterpret_cast<void*>(cockpit.E),
                    reinterpret_cast<void*>(turret.E), reinterpret_cast<void*>(ship.I), reinterpret_cast<void*>(cockpit.I), reinterpret_cast<void*>(turret.I),
                    ship.I && !ship.rigVtableOk ? " SHIP-VTABLE-MISMATCH" : "", cockpit.I && !cockpit.rigVtableOk ? " COCKPIT-VTABLE-MISMATCH" : "",
                    turret.I && !turret.rigVtableOk ? " TURRET-VTABLE-MISMATCH" : "", journalSrvScorpion() ? 1 : 0, n, moduleCount, moduleRigs, g_distinctCount);
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
uint64_t g_startMs = 0, g_lastEmitLogMs = 0, g_lastSummaryMs = 0;
uint64_t g_lastEmitBins[vr::kBins] = {};
uint64_t g_windowTapCalls = 0;
bool g_noTargetSaid = false;

}  // namespace

void vehicleRigProbeInstall() {
    if (g_attempted.exchange(true, std::memory_order_acq_rel)) return;
    if (!Config::get().getBool("advanced.vehicle_rig_probe", true)) return;   // off: no hook, no line (the key's default on this branch is on)
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
        // The token, vtable and offset constants are build 334925's and none of them is in the signature table: on any other build the probe reads nothing.
        Log::get().note("vehicle_rig: not installed: %s is not build 334925; the data the probe reads (the component tokens, the two vtables) are that build's, so it reads nothing and patches "
                        "nothing (SetTarget %s by signature).", gameBuildText(), at ? "was found" : "was not found");
        return;
    }
    if (!at) {
        Log::get().note("vehicle_rig: not installed: SetTarget (0x1096640) did not resolve to exactly one site; nothing patched.");
        return;
    }
    if (!gameRva(kRigTokenRva) || !gameRva(kModsTokenRva) || !gameRva(kRigVtableRva) || !gameRva(kModsVtableRva)) {
        Log::get().note("vehicle_rig: not installed: a component token or a vtable (0x5F05D38, 0x5F2F274, 0x55956B0, 0x559CF58) was not found by signature; nothing patched.");
        return;
    }
    g_rigTokenAddr = g_base + gameRva(kRigTokenRva);
    g_modsTokenAddr = g_base + gameRva(kModsTokenRva);
    g_rigVtable = g_base + gameRva(kRigVtableRva);
    g_modsVtable = g_base + gameRva(kModsVtableRva);
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
    Log::get().note("vehicle_rig: installed: SetTarget (0x1096640, found by signature at 0x%llX) forwards first and returns the game's result unchanged; read-only (SEH-guarded reads, no writes into the "
                    "game). Emit counts ride the kinematic emit bracket and only count while the game's evaluator gate is already open for engine motion (temporal AA on); pose lines come at "
                    "\"flat pixels: copied\". advanced.vehicle_rig_probe is TEMPORARY.", static_cast<unsigned long long>(at));
}

void vehicleRigProbePoll(uint64_t nowMs) {
    if (!g_live.load(std::memory_order_acquire)) return;
    if (!g_startMs) {
        g_startMs = g_lastEmitLogMs = g_lastSummaryMs = nowMs;
    }
    // The emit bracket refreshes the table once a frame while it runs; this keeps it read (and writes the ready lines) when the gate is shut.
    if (g_targets.load(std::memory_order_relaxed)) {
        g_lastRefreshTicks.store(static_cast<uint64_t>(ticks()), std::memory_order_relaxed);
        refresh();
    }
    if (nowMs - g_lastEmitLogMs >= 5000) {
        g_lastEmitLogMs = nowMs;
        uint64_t b[vr::kBins];
        bool changed = false;
        for (uint32_t i = 0; i < vr::kBins; ++i) {
            b[i] = g_emits.bin[i].load(std::memory_order_relaxed);
            changed = changed || b[i] != g_lastEmitBins[i];
            g_lastEmitBins[i] = b[i];
        }
        if (changed)
            Log::get().note("vehicle_rig emits ship=%llu cockpit=%llu turret=%llu module=%llu other=%llu raced=%llu (cumulative, classified against the rig record arrays)",
                            static_cast<unsigned long long>(b[vr::kShip]), static_cast<unsigned long long>(b[vr::kCockpit]), static_cast<unsigned long long>(b[vr::kTurret]),
                            static_cast<unsigned long long>(b[vr::kModule]), static_cast<unsigned long long>(b[vr::kOther]), static_cast<unsigned long long>(b[vr::kRaced]));
    }
    if (nowMs - g_lastSummaryMs >= 30000) {
        g_lastSummaryMs = nowMs;
        const uint64_t taps = g_tapCalls.load(std::memory_order_relaxed);
        const bool gateShut = taps == g_windowTapCalls;
        g_windowTapCalls = taps;
        const uint64_t targets = g_targets.load(std::memory_order_relaxed);
        const uint64_t refreshes = g_refreshes.load(std::memory_order_relaxed), emitSamples = g_emitSamples.load(std::memory_order_relaxed);
        const double refreshUs = refreshes ? 1e6 * static_cast<double>(g_refreshTicks.load(std::memory_order_relaxed)) / static_cast<double>(g_ticksPerSec) / static_cast<double>(refreshes) : 0.0;
        const double emitNs = emitSamples ? 1e9 * static_cast<double>(g_emitSampleTicks.load(std::memory_order_relaxed)) / static_cast<double>(g_ticksPerSec) / static_cast<double>(emitSamples) : 0.0;
        char tail[200];
        std::snprintf(tail, sizeof(tail), "faults_caught=%llu vtable_mismatch=%llu absurd_count=%llu rigs_not_ready=%u (of %u tracked, %u ready); cost: refresh %.1f us avg over %llu, emit tap %.0f ns avg (sampled %llu)",
                      static_cast<unsigned long long>(g_faultsCaught.load(std::memory_order_relaxed)), static_cast<unsigned long long>(g_vtableMismatches.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(g_absurd.load(std::memory_order_relaxed)), g_notReadyNow.load(std::memory_order_relaxed), g_rigsNow.load(std::memory_order_relaxed),
                      g_readyNow.load(std::memory_order_relaxed), refreshUs, static_cast<unsigned long long>(refreshes), emitNs, static_cast<unsigned long long>(emitSamples));
        if (!targets)
            Log::get().note("vehicle_rig: no target seen (SetTarget has not been called since the hook went in); %s", tail);
        else
            Log::get().note("vehicle_rig: targets=%llu distinct_this=%u; %s; %s", static_cast<unsigned long long>(targets), g_distinctCount, tail,
                            gateShut ? "emit counts need temporal AA on (the emit bracket did not run for this whole window, so no emit was counted)" : "emit counts live");
    }
}

}  // namespace edvr
