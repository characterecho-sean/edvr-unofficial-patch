#pragma once
// Private-pool history: identity comes from an owned emission and the native
// copier's source address, never from a pose/content match. Source relocation
// or reuse must invalidate/transfer its claim before another copier can see it.
#include "engine_velocity_emit.h"
#include "engine_motion_cpu.h"   // the render thread's id (emcpu::g_gate) and the clock, for the lock instrument below
#include <d3d11.h>
#include "engine_velocity_primary_copy_shader.h"
#include "temporal_shader_bytecode.h"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <memory>
#include <unordered_set>
#include <utility>

namespace edvr { namespace engine_velocity_primary_copy {
namespace emit=engine_velocity_emit;
using Microsoft::WRL::ComPtr;
constexpr uint32_t kWords=84,kBytes=336,kMaxEmissions=16384,kMaxPatches=16384,kMaxPools=32;
struct Patch { uint32_t slot=0,marker=0; std::array<uint32_t,kWords> native{}; emit::Pose previous{}; };
static_assert(sizeof(Patch)==364,"GPU structured patch layout");
struct Emission { Patch patch; uint32_t frame=0; };
struct Pool {
    ComPtr<ID3D11Buffer> held;
    uintptr_t base=0; uint32_t bytes=0,frame=0;
    uint64_t sequence=0,generation=0;
    bool active=false,valid=false;
    std::unordered_map<uint32_t,Patch> patches;
    // A conservative bound on the slots of `patches`: every patch's slot is in [patchLo, patchHi] whenever the map is not empty.
    // Raised by notePatch before an insert, reset by clearPatches; an erase leaves it as it was (wider, never narrower). It is what the
    // copier's claim check reads (through the published view) to know that a copy range holds no patch to erase.
    uint32_t patchLo=~0u,patchHi=0;
};
inline void notePatch(Pool& p,uint32_t slot) noexcept {if(slot<p.patchLo)p.patchLo=slot;if(slot>p.patchHi)p.patchHi=slot;}
inline void clearPatches(Pool& p) noexcept {p.patches.clear();p.patchLo=~0u;p.patchHi=0;}
enum class ScatterFailure { None,Arguments,Descriptor,Bound,Shader,Upload,Map,View,Counts,Unexpected };
inline const char* scatterFailureName(ScatterFailure f) {
    constexpr const char* names[]={"none","arguments","descriptor","clone_bound","shader","upload","map","view","counts","unexpected"};
    return names[static_cast<unsigned>(f)];
}
struct Stats { ScatterFailure lastScatterFailure=ScatterFailure::None; uint64_t emissions=0,copies=0,joined=0,declined=0,invalidated=0,overflows=0,scatterBatches=0,scatterRows=0,scatterFailed=0,scatterEmpty=0,sourceResets=0,copierNoLease=0,copierAmbiguous=0,copierInvalidRange=0,mergePlans=0,mergeFailed=0,clearCalls=0,clearedClaims=0,clearFailed=0;
    uint64_t mergeCalls=0,mergeWithoutClaims=0,clearNoClaims=0,mergeNodes=0,clearNodes=0,activeClaims=0,activePlans=0;
    // The calls that never took the mutex, already inside clearCalls/clearNoClaims, mergeCalls/mergeWithoutClaims and
    // copies/copierNoLease above (which keep their meaning): the share of calls the lock-free exits handled.
    uint64_t clearLockFree=0,mergeLockFree=0,copiesLockFree=0,copierNoLeaseLockFree=0;
    // The claim-aware share of those: calls that found claims live, walked the native list without the mutex and touched none
    // (inside clearLockFree / mergeLockFree, and in clearCalls / mergeCalls, but not in clearNoClaims / mergeWithoutClaims, which
    // keep their meaning); and the calls that wanted that check and took the mutex for a reason other than a hit.
    uint64_t clearPrechecked=0,mergePrechecked=0,precheckDeclined=0;
    // The copier's claim check (round 6): copies that found claims live or patches held, none of them in the copy's range, and were handed
    // back without the mutex (inside copies / copiesLockFree, and in copierNoLease / copierNoLeaseLockFree where they found no lease); and
    // the copies that wanted the check and took the mutex for a reason other than a hit.
    uint64_t copierPrechecked=0,copierPrecheckDeclined=0;
};
struct MergePlan;
inline std::mutex g_mutex,g_gpuMutex;
inline std::unordered_map<uintptr_t,Emission> g_emissions;
inline std::unordered_set<MergePlan*> g_mergePlans;
inline std::unordered_map<ID3D11Buffer*,Pool> g_pools;
inline Stats g_stats;
inline uint32_t g_overflowFrame=~0u;
inline uint32_t g_emissionFrame=~0u;
inline uint64_t g_emissionEpoch=1;
// Two published flags, each as of the last release of g_mutex by anyone who locks it. They sit on their own cache lines
// so readers are not invalidated by the mutex's traffic, and each is written only when its value changes.
//   g_claimsLive    = !g_emissions.empty() || !g_mergePlans.empty(). The typed-dictionary clear reads it: with it false
//                     there is no claim to revoke and no in-flight plan to mark invalid, so the clear counts and leaves.
//   g_emissionsLive = !g_emissions.empty(). The list merge and the pool copier read it. A merge stages claims from
//                     g_emissions and from nowhere else, so with it false the plan it would build has no transfers and
//                     the merge gets a token instead. The copier never reads g_mergePlans. A plan that holds nothing
//                     must not keep those two paths locked, which a flag including plans did (a chain: one emission, an
//                     unrelated merge registers an empty plan, the emission is consumed, the plan keeps the flag true,
//                     every merge that starts meanwhile registers another).
alignas(64) inline std::atomic<bool> g_claimsLive{false};
alignas(64) inline std::atomic<bool> g_emissionsLive{false};
// Calls that took that exit, counted without the mutex and folded into stats() as clearCalls/clearNoClaims and
// mergeCalls/mergeWithoutClaims, so every counter the log prints keeps its meaning.
alignas(64) inline std::atomic<uint64_t> g_clearSkipped{0};
alignas(64) inline std::atomic<uint64_t> g_mergeSkipped{0};
// The claim-aware exits (claim snapshot below): clears and merges that found claims live, walked the list without the mutex and
// touched none; and the calls that went to the mutex for a reason other than a hit.
alignas(64) inline std::atomic<uint64_t> g_clearPrechecked{0};
alignas(64) inline std::atomic<uint64_t> g_mergePrechecked{0};
alignas(64) inline std::atomic<uint64_t> g_precheckDeclined{0};
// The pool copier's lock-free exit counts the same way: every copy (stats().copies) and those that found no active
// lease at their base (stats().copierNoLease).
alignas(64) inline std::atomic<uint64_t> g_copierSkipped{0};
inline std::atomic<uint64_t> g_copierNoLeaseSkipped{0};
// The copier's claim check (see copierClaimCheck): copies it handed back, those among them that found no lease, and copies that wanted it
// and took the mutex for a reason other than a hit. Folded into stats() beside the two above.
alignas(64) inline std::atomic<uint64_t> g_copierPrechecked{0};
inline std::atomic<uint64_t> g_copierPrecheckedNoLease{0};
inline std::atomic<uint64_t> g_copierPrecheckDeclined{0};
// What the copier's lock-free exit needs besides g_emissionsLive: whether any pool holds a patch, and the active leases
// (base address, slots = bytes / kBytes). A seqlock: the writer holds g_mutex, makes seq odd, rewrites, makes it even;
// a reader keeps what it read only if seq was even and unchanged on both sides. Both flags are stored inside the
// same window whenever the window opens, so a reader's (emissions, patches, leases) is one published state.
constexpr uint32_t kLeasesUnknown=~0u;
struct PoolView {
    std::atomic<uint32_t> seq{0},count{0};
    std::atomic<bool> patches{false};
    std::atomic<uintptr_t> base[kMaxPools];
    std::atomic<uint32_t> slots[kMaxPools];
    std::atomic<uint32_t> patchLo[kMaxPools],patchHi[kMaxPools];   // per lease: bound on the slots of the pool's patches; lo>hi = none
};
alignas(64) inline PoolView g_poolView;
// One window around everything ClaimLock::publish writes to the two views. g_claimView and g_poolView are separate seqlocks, each exact on its
// own, but a publish writes one and then the other: a reader of both could otherwise see the claims of a critical section and the pools of the
// one before it, a pair no instant of the mutex's order ever held. The copier's claim check reads both, so it reads them inside this window:
// odd while a publish is rewriting either view, unchanged across the reader's reads = the pair is one critical section's state. Written only by
// ClaimLock::publish, under g_mutex, and only when a view changes.
alignas(64) inline std::atomic<uint32_t> g_publishSeq{0};
struct PublishWindow {
    uint32_t seq=0;bool open=false;
    void begin() noexcept {
        if(open)return;
        open=true;seq=g_publishSeq.load(std::memory_order_relaxed);
        g_publishSeq.store(seq+1,std::memory_order_relaxed);std::atomic_thread_fence(std::memory_order_release);
    }
    ~PublishWindow() {if(open)g_publishSeq.store(seq+2,std::memory_order_release);}
};
// Set, under g_mutex, by everything that can change a pool's lease, base, size or patches; ClaimLock then republishes
// the view. Calls that touch only emissions leave it alone and pay a bool test.
inline bool g_poolsDirty=false;
// ---- the claim snapshot -------------------------------------------------------------------------------------------
// WHY. Round 5 of the settlement arc (docs\flat-cz-fps-2026-10-11.md) measured the claim regime: the claim set holds ONE address
// (claim-set size 1 at every claimed call), is live for 14-21% of a window, and 99.98% of the 5.3M merges and 1.9M clears a window
// that find it live touch nothing in it. Each of those took g_mutex only to learn that, and Present p50 rose by 3 ms.
// WHAT. The set of live claim addresses, as an immutable snapshot published like g_poolView (a seqlock: the writer holds g_mutex,
// makes seq odd, rewrites, makes it even; a reader keeps what it read only if seq was even and unchanged on both sides), and
// whether a detached merge plan is in flight. At most kClaimViewMax addresses; with more the view says so and every caller takes
// the mutex as before. ClaimLock::publish writes it, at every unlock, from the containers themselves, so it can only lag a
// critical section that has not finished.
// LINEARIZATION. A clear or a merge that finds the snapshot free of every address its native list holds, and no plan in flight,
// is ordered at the moment it read the snapshot: it is a call that took g_mutex when the claim set was exactly that snapshot, which
// is a state some completed critical section left (publish runs before the unlock). Anything not in the snapshot was either
// published after that moment, so the call is ordered BEFORE it, as the mutex allowed for a producer that had not yet taken it, or
// before the call began, in which case the unlock that published it happened-before the call (the producer's recordEmission had
// returned) and the snapshot holds it. A call that finds a hit, a plan, more than kClaimViewMax claims, a torn read or any refusal
// the locked walk would act on takes the mutex and does what it always did, from scratch. What the locked path does when no
// address of the call is a claim and no plan exists is: count, and change nothing else (merge: register an empty plan whose only
// later effect is a counter, or on a native unwind the revoking of later claims, which the fixed token does more strictly).
constexpr uint32_t kClaimViewMax=4;
struct ClaimView {
    std::atomic<uint32_t> seq{0},count{0};   // count: live claims, or kClaimViewMax+1 when more are live than the view holds
    std::atomic<bool> plans{false};          // a detached merge plan is registered
    std::atomic<uintptr_t> address[kClaimViewMax];
};
alignas(64) inline ClaimView g_claimView;
struct ClaimViewState {   // the same fields as plain values: the writer's mirror (under g_mutex) and a reader's copy
    uint32_t count=0;bool plans=false;uintptr_t address[kClaimViewMax]{};
    bool operator==(const ClaimViewState& o) const {
        if(count!=o.count || plans!=o.plans)return false;
        for(uint32_t i=0;i<kClaimViewMax;++i)if(address[i]!=o.address[i])return false;
        return true;
    }
    // Whether any live claim is one of the 8 record slots of the list node `node` at index [first, last).
    bool holds(uintptr_t node,uint64_t first=0,uint64_t last=8) const {
        for(uint32_t i=0;i<count && i<kClaimViewMax;++i) {
            const uintptr_t records=node+emit::kNodeRecords;
            if(address[i]<records)continue;
            const uintptr_t offset=address[i]-records;
            if(offset%kBytes==0 && offset/kBytes>=first && offset/kBytes<last && offset/kBytes<8)return true;
        }
        return false;
    }
};
inline ClaimViewState g_claimViewMirror;   // under g_mutex
#ifdef EDVR_ENGINE_VELOCITY_RIG
inline void (*g_claimMidRead)() noexcept=nullptr;   // the rig only: publishes a change between the reads and the re-check
inline void (*g_claimAfterWalk)() noexcept=nullptr;  // the rig only: publishes a claim after a walk found none of the live ones touched
inline bool g_claimPrecheck=true;                   // the rig only: false is the code as it was before the snapshot
#else
constexpr bool g_claimPrecheck=true;
#endif
// The reader: false on an odd or moved sequence (a write was in flight; the caller takes the mutex).
inline bool readClaimView(ClaimViewState& out) noexcept {
    const uint32_t before=g_claimView.seq.load(std::memory_order_acquire);
    if(before&1u)return false;
    out.count=g_claimView.count.load(std::memory_order_relaxed);out.plans=g_claimView.plans.load(std::memory_order_relaxed);
    for(uint32_t i=0;i<kClaimViewMax;++i)out.address[i]=g_claimView.address[i].load(std::memory_order_relaxed);
#ifdef EDVR_ENGINE_VELOCITY_RIG
    if(g_claimMidRead)g_claimMidRead();
#endif
    std::atomic_thread_fence(std::memory_order_acquire);
    return g_claimView.seq.load(std::memory_order_relaxed)==before;
}
// ---- the lock instrument ------------------------------------------------------------------------------------------
// Who waits for g_mutex and who holds it, per caller and per thread class (the render thread, taken from engine
// motion's gate, or any other). One acquisition in kLockSamplePeriod per caller on each thread is timed: the QPC is read
// before the acquire, after it and before the release, and only for those. Every other acquisition adds one thread-local
// increment. The sums sit in relaxed atomics, are written after the unlock, and are read and zeroed once a summary.
enum class Caller : unsigned { MergeBegin, MergeEnd, ClearSkip, ClearWalk, Copier, BeginMap, EndMap, Emit, Other, Count };
constexpr unsigned kCallers=static_cast<unsigned>(Caller::Count);
constexpr unsigned kLockSamplePeriod=64;
static_assert((kLockSamplePeriod&(kLockSamplePeriod-1))==0,"the sampling test is a mask");
inline constexpr const char* kCallerNames[kCallers]={"merge begin","merge end","clear skip","clear walk","copier","beginMap","endMap","emit","other"};
struct LockCell { std::atomic<uint64_t> n{0},wait{0},waitMax{0},hold{0},holdMax{0}; };   // ticks
inline LockCell g_lockCells[kCallers][2];   // [caller][0 render thread, 1 any other]
inline thread_local uint32_t t_lockTick[kCallers]{};
inline int64_t (*g_lockClock)() noexcept=&emcpu::qpcNow;   // the rig substitutes a counting clock
inline bool lockSample(Caller c) noexcept {return ((++t_lockTick[static_cast<unsigned>(c)])&(kLockSamplePeriod-1))==0;}
inline void raiseTo(std::atomic<uint64_t>& cell,uint64_t value) noexcept {
    uint64_t seen=cell.load(std::memory_order_relaxed);
    while(seen<value && !cell.compare_exchange_weak(seen,value,std::memory_order_relaxed)){}
}
inline void lockRecord(Caller c,int64_t waited,int64_t held) noexcept {
    const bool render=emcpu::g_renderThreadId.load(std::memory_order_relaxed)==emcpu::currentThreadId();   // not the gate: see there
    LockCell& cell=g_lockCells[static_cast<unsigned>(c)][render?0:1];
    const uint64_t w=waited>0?static_cast<uint64_t>(waited):0,h=held>0?static_cast<uint64_t>(held):0;
    cell.n.fetch_add(1,std::memory_order_relaxed);cell.wait.fetch_add(w,std::memory_order_relaxed);cell.hold.fetch_add(h,std::memory_order_relaxed);
    raiseTo(cell.waitMax,w);raiseTo(cell.holdMax,h);
}
struct LockWindow { uint64_t n[kCallers][2]{},wait[kCallers][2]{},waitMax[kCallers][2]{},hold[kCallers][2]{},holdMax[kCallers][2]{}; };
inline LockWindow takeLockWindow() noexcept {
    LockWindow w;
    for(unsigned c=0;c<kCallers;++c)for(unsigned k=0;k<2;++k) {
        LockCell& cell=g_lockCells[c][k];
        w.n[c][k]=cell.n.exchange(0,std::memory_order_relaxed);w.wait[c][k]=cell.wait.exchange(0,std::memory_order_relaxed);
        w.waitMax[c][k]=cell.waitMax.exchange(0,std::memory_order_relaxed);w.hold[c][k]=cell.hold.exchange(0,std::memory_order_relaxed);
        w.holdMax[c][k]=cell.holdMax.exchange(0,std::memory_order_relaxed);
    }
    return w;
}
constexpr size_t kLockLineBytes=1160;   // the log's line buffer is 1200 and takes about 1166 characters of text
// One of two lines (half 0: callers 0-4, half 1: callers 5-8): engine motion: primary copy lock (sampled 1 in 64, 30 s, 1 of 2):
// merge begin: render n=0; other n=5 wait 0.2/0.5 us hold 0.4/1.0 us | merge end: ... Times are average/maximum microseconds
// over the sampled acquisitions of the window. A caller's lock-free exits never take the mutex and are not in it.
inline void formatLockLine(const LockWindow& w,unsigned half,double ticksPerUs,double seconds,char (&out)[kLockLineBytes]) noexcept {
    const double perUs=ticksPerUs>0?ticksPerUs:1.0;
    size_t used=0;
    auto put=[&](const char* format,auto... args){
        if(used+1>=kLockLineBytes)return;
        const int n=std::snprintf(out+used,kLockLineBytes-used,format,args...);
        if(n>0)used=std::min(used+static_cast<size_t>(n),kLockLineBytes-1);
    };
    put("engine motion: primary copy lock (sampled 1 in %u, %.0f s, %u of 2): ",kLockSamplePeriod,seconds,half+1);
    const unsigned first=half?5u:0u,last=half?kCallers:5u;
    for(unsigned c=first;c<last;++c) {
        put("%s%s: ",c==first?"":" | ",kCallerNames[c]);
        for(unsigned k=0;k<2;++k) {
            const char* label=k?"other":"render";const uint64_t n=w.n[c][k];
            put("%s%s n=%llu",k?"; ":"",label,static_cast<unsigned long long>(n));
            if(n)put(" wait %.1f/%.1f us hold %.1f/%.1f us",double(w.wait[c][k])/double(n)/perUs,double(w.waitMax[c][k])/perUs,
                     double(w.hold[c][k])/double(n)/perUs,double(w.holdMax[c][k])/perUs);
        }
    }
    out[used]=0;
}
// The only way this header locks g_mutex. The destructor publishes both flags (and, when pools were touched, the pool
// view) BEFORE it unlocks, so a lock-free reader that sees "false" is reading a state no later than the last completed
// critical section: it is ordered before any mutation that is still in flight, which is a legal interleaving of the
// call it replaces. A caller whose emission was recorded and returned before this call started sees "true": the
// unlock publishes it. It is also where the lock instrument lives: the caller names itself, and one acquisition in
// kLockSamplePeriod is timed (wait from before the acquire to after it, hold from there to the release).
struct ClaimLock {
    explicit ClaimLock(Caller caller=Caller::Other) noexcept : caller_(caller) {
        if(lockSample(caller)) {
            sampled_=true;acquireStart_=g_lockClock();g_mutex.lock();acquired_=g_lockClock();
        } else g_mutex.lock();
    }
    ~ClaimLock() {
        publish();
        if(sampled_) {
            const int64_t released=g_lockClock();g_mutex.unlock();
            lockRecord(caller_,acquired_-acquireStart_,released-acquired_);
        } else g_mutex.unlock();
    }
    void relabel(Caller caller) noexcept {caller_=caller;}   // a clear that turns out to have nothing to walk
    ClaimLock(const ClaimLock&)=delete;
    ClaimLock& operator=(const ClaimLock&)=delete;
private:
    Caller caller_;
    bool sampled_=false;
    int64_t acquireStart_=0,acquired_=0;
    // The claim snapshot, rebuilt from the containers and rewritten only when it differs from the one published. Sorted, so the
    // same set in another hash order is the same snapshot. At most kClaimViewMax addresses are walked.
    static void publishClaimView(PublishWindow& window) noexcept {
        ClaimViewState next;next.plans=!g_mergePlans.empty();
        const size_t n=g_emissions.size();
        if(n>kClaimViewMax)next.count=kClaimViewMax+1;
        else {
            for(const auto& e:g_emissions) {
                uint32_t at=next.count++;
                while(at && next.address[at-1]>e.first){next.address[at]=next.address[at-1];--at;}
                next.address[at]=e.first;
            }
        }
        if(next==g_claimViewMirror)return;
        g_claimViewMirror=next;window.begin();
        const uint32_t sequence=g_claimView.seq.load(std::memory_order_relaxed);
        g_claimView.seq.store(sequence+1,std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        g_claimView.count.store(next.count,std::memory_order_relaxed);g_claimView.plans.store(next.plans,std::memory_order_relaxed);
        for(uint32_t i=0;i<kClaimViewMax;++i)g_claimView.address[i].store(next.address[i],std::memory_order_relaxed);
        g_claimView.seq.store(sequence+2,std::memory_order_release);
    }
    static void publish() noexcept {
        PublishWindow window;   // closes when publish returns, by any path
        publishClaimView(window);
        const bool emissions=!g_emissions.empty(),claims=emissions || !g_mergePlans.empty();
        const bool claimsChanged=g_claimsLive.load(std::memory_order_relaxed)!=claims || g_emissionsLive.load(std::memory_order_relaxed)!=emissions;
        const auto storeFlags=[&]{
            g_claimsLive.store(claims,std::memory_order_release);g_emissionsLive.store(emissions,std::memory_order_release);
        };
        if(!g_poolsDirty){if(claimsChanged)storeFlags();return;}
        g_poolsDirty=false;
        uint32_t count=0;uintptr_t base[kMaxPools];uint32_t slots[kMaxPools],lo[kMaxPools],hi[kMaxPools];bool patches=false;
        for(const auto& entry:g_pools) {
            const bool held=!entry.second.patches.empty();
            patches=patches || held;
            if(!entry.second.active)continue;
            if(count<kMaxPools) {
                base[count]=entry.second.base;slots[count]=entry.second.bytes/kBytes;
                // No patches: lo>hi. Patches with a bound that was never raised (lo>hi): unknown, the whole range, never "none".
                lo[count]=held?(entry.second.patchLo<=entry.second.patchHi?entry.second.patchLo:0u):~0u;
                hi[count]=held?(entry.second.patchLo<=entry.second.patchHi?entry.second.patchHi:~0u):0u;
            }
            ++count;
        }
        if(count>kMaxPools)count=kLeasesUnknown;
        bool same=patches==g_poolView.patches.load(std::memory_order_relaxed) && count==g_poolView.count.load(std::memory_order_relaxed);
        for(uint32_t i=0;same && i<count && count!=kLeasesUnknown;++i)
            same=base[i]==g_poolView.base[i].load(std::memory_order_relaxed) && slots[i]==g_poolView.slots[i].load(std::memory_order_relaxed) &&
                 lo[i]==g_poolView.patchLo[i].load(std::memory_order_relaxed) && hi[i]==g_poolView.patchHi[i].load(std::memory_order_relaxed);
        if(same){if(claimsChanged)storeFlags();return;}
        window.begin();
        const uint32_t sequence=g_poolView.seq.load(std::memory_order_relaxed);
        g_poolView.seq.store(sequence+1,std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        g_poolView.patches.store(patches,std::memory_order_relaxed);
        g_poolView.count.store(count,std::memory_order_relaxed);
        for(uint32_t i=0;i<count && count!=kLeasesUnknown;++i) {
            g_poolView.base[i].store(base[i],std::memory_order_relaxed);g_poolView.slots[i].store(slots[i],std::memory_order_relaxed);
            g_poolView.patchLo[i].store(lo[i],std::memory_order_relaxed);g_poolView.patchHi[i].store(hi[i],std::memory_order_relaxed);
        }
        storeFlags();
        g_poolView.seq.store(sequence+2,std::memory_order_release);
    }
};
// Address set for the observers' bounded walks of Elite's lists: up to kInline entries live in an inline array searched
// linearly (a walk of eight nodes is 64 item addresses, so it never touches the heap), and the first insert past that
// moves everything into a std::unordered_set for good. insert().second, size(), count() and forEach() answer as the
// unordered_set they replace did; like it, forEach has no defined order.
class AddressSet {
public:
    static constexpr size_t kInline=64;
    std::pair<const uintptr_t*,bool> insert(uintptr_t value) {
        if(spill_) {const auto r=spill_->insert(value);return {&*r.first,r.second};}
        for(size_t i=0;i<count_;++i)if(inline_[i]==value)return {&inline_[i],false};
        if(count_<kInline){inline_[count_]=value;return {&inline_[count_++],true};}
        auto spill=std::make_unique<std::unordered_set<uintptr_t>>(inline_,inline_+count_);
        const auto r=spill->insert(value);spill_=std::move(spill);return {&*r.first,true};
    }
    size_t size() const {return spill_?spill_->size():count_;}
    size_t count(uintptr_t value) const {
        if(spill_)return spill_->count(value);
        for(size_t i=0;i<count_;++i)if(inline_[i]==value)return 1;
        return 0;
    }
    template<class F> void forEach(F&& visit) const {
        if(spill_){for(uintptr_t value:*spill_)visit(value);}
        else for(size_t i=0;i<count_;++i)visit(inline_[i]);
    }
private:
    uintptr_t inline_[kInline];
    size_t count_=0;
    std::unique_ptr<std::unordered_set<uintptr_t>> spill_;
};
inline emit::Pose current(const uint32_t* r) { return emit::Pose{{r[4],r[5],r[6],r[2],r[3]}}; }
inline void overflow(uint32_t frame) {
    g_poolsDirty=true;
    g_overflowFrame=frame;++g_stats.overflows;g_emissions.clear();++g_emissionEpoch;
    for(auto& entry:g_pools)clearPatches(entry.second);
}
inline void invalidateEmission(uintptr_t source) noexcept {
    ClaimLock lock(Caller::Emit);
    if(source)g_emissions.erase(source);else {g_emissions.clear();++g_emissionEpoch;++g_stats.invalidated;++g_stats.sourceResets;}
}
inline void invalidateMapped(uintptr_t base) noexcept {
    ClaimLock lock(Caller::Other);g_poolsDirty=true;
    for(auto& entry:g_pools)if(entry.second.active && (!base || entry.second.base==base)) {
        clearPatches(entry.second);entry.second.valid=false;++g_stats.invalidated;
    }
}
inline bool recordEmission(uintptr_t source,const uint32_t native[kWords],const emit::Pose& previous,
                           uint32_t marker,uint32_t frame) noexcept {
    ClaimLock lock(Caller::Emit);
    if(g_emissionFrame!=frame){g_emissions.clear();g_emissionFrame=frame;++g_emissionEpoch;++g_stats.sourceResets;}
    if(!source || !native || g_overflowFrame==frame)return false;
    const emit::Pose now=current(native);
    const bool joined=marker==(emit::kJoined^emit::markerHash(now,previous,frame));
    const bool masked=marker==(emit::kMasked^emit::markerHash(now,previous,frame));
    // Current-only native rigid records; all second-block bytes stay native.
    if(native[0] || native[1]!=0x3F800000u || native[77]!=native[1] ||
       native[73]!=native[4] || native[74]!=native[5] || native[75]!=native[6] ||
       native[78]!=native[2] || native[79]!=native[3] || (!joined && !masked) || (masked && previous!=now)) {
        g_emissions.erase(source);++g_stats.declined;return false;
    }
    try {
        auto old=g_emissions.find(source);
        if(old!=g_emissions.end() && old->second.frame==frame) {
            const auto& p=old->second.patch;
            if(p.marker!=marker || p.previous!=previous || std::memcmp(p.native.data(),native,kBytes)) {
                g_emissions.erase(old);++g_stats.declined;return false;
            }
            return true;
        }
        if(g_emissions.size()>=kMaxEmissions){overflow(frame);return false;}
        Emission e;e.frame=frame;e.patch.marker=marker;e.patch.previous=previous;
        std::memcpy(e.patch.native.data(),native,kBytes);g_emissions[source]=e;++g_stats.emissions;
        return true;
    } catch(...) {overflow(frame);return false;}
}
inline bool beginMap(ID3D11Buffer* resource,void* base,uint32_t bytes,uint32_t stride,D3D11_MAP type,
                     uint64_t sequence,uint32_t frame) noexcept {
    ClaimLock lock(Caller::BeginMap);g_poolsDirty=true;
    if(!resource)return false;
    try {
        auto old=g_pools.find(resource);
        if(old==g_pools.end() && g_pools.size()>=kMaxPools){overflow(frame);return false;}
        Pool& p=g_pools[resource];if(!p.held)p.held=resource;
        if(p.active || sequence<=p.sequence){clearPatches(p);p.valid=false;p.active=false;++g_stats.declined;return false;}
        if(type!=D3D11_MAP_WRITE_NO_OVERWRITE || p.frame!=frame){clearPatches(p);++p.generation;}
        p.base=reinterpret_cast<uintptr_t>(base);p.bytes=bytes;p.frame=frame;p.sequence=sequence;p.active=true;
        p.valid=base && stride==kBytes && bytes>=kBytes && bytes%kBytes==0 &&
            (type==D3D11_MAP_WRITE_DISCARD || type==D3D11_MAP_WRITE_NO_OVERWRITE) && g_overflowFrame!=frame;
        if(!p.valid){clearPatches(p);++g_stats.declined;}
        return p.valid;
    } catch(...) {overflow(frame);return false;}
}
// The copier's lock-free exit. What the locked copier does with no source claim and no patch in any pool (a detached merge
// plan is irrelevant: the copier reads only g_emissions and g_pools, and a plan's staged claims are not in g_emissions):
// bump copies; scan the pools for active leases at `base`; with none bump copierNoLease; with one whose size
// covers the range erase its patches in that range (none exist); with two, or a range that does not fit, invalidate
// the pools; then walk emissions (none), after refusing a count/source it cannot address (which clears the emissions
// and bumps the epoch even when there are none). This handles exactly the quiet cases that change nothing but a counter:
// count 0, no lease, and one lease that fits. Everything else, or any doubt (an odd or moved sequence, source claims or
// patches in the published state), returns false and the caller takes the mutex. The published state is a state some completed
// critical section left, so the call is ordered before any mutation still in flight.
#ifdef EDVR_ENGINE_VELOCITY_RIG
inline void (*g_copierMidRead)() noexcept=nullptr;   // the rig only: publishes a change between the reads and the re-check
#endif
inline bool copierLockFree(uintptr_t base,uint32_t stride,uintptr_t source,uint32_t firstSlot,uint32_t count) noexcept {
    if(!count){g_copierSkipped.fetch_add(1,std::memory_order_relaxed);return true;}
    if(count>kMaxPatches || !source || source>UINTPTR_MAX-uint64_t(count)*kBytes)return false;
    const uint32_t before=g_poolView.seq.load(std::memory_order_acquire);
    if(before&1u)return false;
    const uint32_t leases=g_poolView.count.load(std::memory_order_relaxed);
    const bool quiet=!g_emissionsLive.load(std::memory_order_relaxed) && !g_poolView.patches.load(std::memory_order_relaxed);
    if(!quiet || leases>kMaxPools)return false;
    uint32_t matches=0,slots=0;
    for(uint32_t i=0;i<leases;++i)if(g_poolView.base[i].load(std::memory_order_relaxed)==base) {
        ++matches;slots=g_poolView.slots[i].load(std::memory_order_relaxed);
    }
#ifdef EDVR_ENGINE_VELOCITY_RIG
    if(g_copierMidRead)g_copierMidRead();
#endif
    std::atomic_thread_fence(std::memory_order_acquire);
    if(g_poolView.seq.load(std::memory_order_relaxed)!=before)return false;
    if(!matches){g_copierSkipped.fetch_add(1,std::memory_order_relaxed);g_copierNoLeaseSkipped.fetch_add(1,std::memory_order_relaxed);return true;}
    if(matches==1 && stride==kBytes && firstSlot<=slots && count<=slots-firstSlot){g_copierSkipped.fetch_add(1,std::memory_order_relaxed);return true;}
    return false;
}
// The copier's claim check (docs\flat-cz-fps-2026-10-11.md, round 6). copierLockFree above hands back only a copy made while NO claim is
// live and NO pool holds a patch. At a settlement a claim is live 13-21% of the time and a patch for most of the frame, and every copy then
// took g_mutex (3.7M acquisitions in a 30 s window, 0.1 us each). This check hands back a copy whose range holds no claim and no patch.
//
// WHAT THE LOCKED COPIER DOES, step by step, for a call with 0 < count <= kMaxPatches and a source that does not wrap:
//   1. ++copies.
//   2. Find the pools with an ACTIVE lease at `base`. Two or more: clear the patches of each and mark them invalid (ambiguous). None: ++copierNoLease.
//   3. One lease: if the range [firstSlot, firstSlot+count) does not fit in its size, or the stride is not kBytes, clear its patches and mark it
//      invalid; if it fits, ERASE the pool's patches in the range (a patch there is lost, and counted).
//   4. For i < count, look up the claim source+i*kBytes in g_emissions. A claim found is CONSUMED (erased, even when the copy is refused), and when
//      the lease is valid, current, of this frame and the bytes match, it becomes a patch in the pool (the join). Nothing found: nothing happens.
// So the call changes shared state only through (a) a lease count other than 1, (b) a range that does not fit, (c) a patch in the range, or
// (d) a claim in the range. With none of the four, the whole call is the counter bump of step 1 and, with no lease, of step 2. The check
// proves none of the four from the published state and bumps the same counters outside the mutex.
//   (d) the claim snapshot (g_claimView): at most kClaimViewMax sorted addresses; with more, or with a merge plan in flight (the plan does
//       not matter to this copier, which never reads g_mergePlans, but the check keeps the clear's and merge's rule: a plan means take the
//       mutex), the check declines. A claim is "in the range" if it is source + i*kBytes for some i < count, the locked loop's own test.
//   (a)(b) the lease table (g_poolView): exactly one lease at `base`, stride kBytes, the range inside its size; none at all is also handled.
//   (c) the lease's patch bound (patchLo/patchHi, a superset of its patch slots): the range does not meet [lo, hi]. A pool with no active lease
//       is not touched by the call, so its patches do not matter.
// LINEARIZATION. Both tables are published by ClaimLock::publish before the unlock, inside g_publishSeq's window, so a read that sees the same
// even g_publishSeq before and after its loads holds the claims AND the pools exactly as one completed critical section left them (never one
// section's claims with the previous one's pools). The call is ordered at the instant of that read: a call that took g_mutex when the state
// was that snapshot, found no claim, no patch and a single fitting lease (or none), and changed nothing but counters. What is not in the
// snapshot is either published after that instant, so the call is ordered BEFORE it (the mutex allowed that order for a section that had not
// yet taken it), or published before the call began: a claim recorded and returned before the call (Elite hands the source over only after
// the producer has returned) was published by an unlock that happens-before the call, so the snapshot holds it, the range test finds it and
// the call takes the mutex. A change in flight (odd seq, or a seq that moved) is not read: the call takes the mutex, from scratch. The locked
// path is unchanged. Without the lock, nothing is read through a pointer: the check holds addresses as integers and never dereferences one.
// x86 hides a weakened memory order from the rig, as for g_poolView.
#ifdef EDVR_ENGINE_VELOCITY_RIG
inline void (*g_copierClaimMidRead)() noexcept=nullptr;   // the rig only: publishes a change between the check's reads and its re-check
#endif
inline bool copierClaimCheck(uintptr_t base,uint32_t stride,uintptr_t source,uint32_t firstSlot,uint32_t count) noexcept {
    if(!g_claimPrecheck)return false;
    if(!count || count>kMaxPatches || !source || source>UINTPTR_MAX-uint64_t(count)*kBytes)return false;   // the cases the locked copier refuses
    const auto declined=[]{g_copierPrecheckDeclined.fetch_add(1,std::memory_order_relaxed);return false;};
    const uint32_t before=g_publishSeq.load(std::memory_order_acquire);
    if(before&1u)return declined();
    const uint32_t claims=g_claimView.count.load(std::memory_order_relaxed);
    const bool plans=g_claimView.plans.load(std::memory_order_relaxed);
    uintptr_t claimed[kClaimViewMax];
    for(uint32_t i=0;i<kClaimViewMax;++i)claimed[i]=g_claimView.address[i].load(std::memory_order_relaxed);
    const uint32_t leases=g_poolView.count.load(std::memory_order_relaxed);
    uint32_t matches=0,slots=0,patchLo=~0u,patchHi=0;
    for(uint32_t i=0;i<leases && leases<=kMaxPools;++i)if(g_poolView.base[i].load(std::memory_order_relaxed)==base) {
        ++matches;slots=g_poolView.slots[i].load(std::memory_order_relaxed);
        patchLo=g_poolView.patchLo[i].load(std::memory_order_relaxed);patchHi=g_poolView.patchHi[i].load(std::memory_order_relaxed);
    }
#ifdef EDVR_ENGINE_VELOCITY_RIG
    if(g_copierClaimMidRead)g_copierClaimMidRead();
#endif
    std::atomic_thread_fence(std::memory_order_acquire);
    if(g_publishSeq.load(std::memory_order_relaxed)!=before)return declined();
    if(plans || claims>kClaimViewMax || leases>kMaxPools)return declined();
    for(uint32_t i=0;i<claims && i<kClaimViewMax;++i) {   // (d) a claim in the range: the locked copier consumes it
        const uintptr_t offset=claimed[i]-source;
        if(claimed[i]>=source && offset%kBytes==0 && offset/kBytes<count)return false;
    }
    if(matches>1)return false;   // (a) ambiguous
    if(matches==1) {
        if(stride!=kBytes || firstSlot>slots || count>slots-firstSlot)return false;   // (b) does not fit
        if(patchLo<=patchHi && uint64_t(patchLo)<uint64_t(firstSlot)+count && patchHi>=firstSlot)return false;   // (c) a patch to erase
    }
#ifdef EDVR_ENGINE_VELOCITY_RIG
    if(g_claimAfterWalk)g_claimAfterWalk();   // the rig only: a claim published now is ordered after this call
#endif
    g_copierPrechecked.fetch_add(1,std::memory_order_relaxed);
    if(!matches)g_copierPrecheckedNoLease.fetch_add(1,std::memory_order_relaxed);
    return true;
}
inline void copier(uintptr_t base,uint32_t stride,uintptr_t source,uint32_t firstSlot,uint32_t count,uint32_t frame) noexcept {
    if(copierLockFree(base,stride,source,firstSlot,count))return;
    if(copierClaimCheck(base,stride,source,firstSlot,count))return;
    ClaimLock lock(Caller::Copier);g_poolsDirty=true;++g_stats.copies;
    if(!count)return;
    Pool* pool=nullptr;
    uint32_t matches=0;
    for(auto& entry:g_pools)if(entry.second.active && entry.second.base==base) {
        pool=&entry.second;++matches;
    }
    if(matches>1){++g_stats.copierAmbiguous;for(auto& entry:g_pools)if(entry.second.active && entry.second.base==base){clearPatches(entry.second);entry.second.valid=false;++g_stats.invalidated;}pool=nullptr;}
    if(!matches)++g_stats.copierNoLease;
    const bool range=pool && stride==kBytes && count && firstSlot<=pool->bytes/kBytes && count<=pool->bytes/kBytes-firstSlot;
    if(pool) {
        if(!range){clearPatches(*pool);pool->valid=false;++g_stats.invalidated;++g_stats.copierInvalidRange;}
        else for(uint32_t i=0;i<count;++i)g_stats.invalidated+=pool->patches.erase(firstSlot+i);
    }
    // Claims are consumed even on a refused copy: an allocator reuse cannot
    // inherit an emission after that source item has been drained.
    if(count>kMaxPatches || !source || source>UINTPTR_MAX-uint64_t(count)*kBytes) {
        g_emissions.clear();++g_emissionEpoch;++g_stats.declined;return;
    }
    for(uint32_t i=0;i<count;++i) {
        auto e=g_emissions.find(source+uintptr_t(i)*kBytes);if(e==g_emissions.end())continue;
        const Emission emission=e->second;g_emissions.erase(e);
        if(!range || !pool->active || !pool->valid || pool->frame!=frame || emission.frame!=frame || g_overflowFrame==frame) {++g_stats.declined;continue;}
        std::array<uint32_t,kWords> src{},dst{};
        if(!emit::read(source+uintptr_t(i)*kBytes,src.data(),kBytes) ||
           !emit::read(base+uintptr_t(firstSlot+i)*kBytes,dst.data(),kBytes) ||
           src!=emission.patch.native || dst!=emission.patch.native) {++g_stats.declined;continue;}
        try {
            if(pool->patches.size()>=kMaxPatches){overflow(frame);return;}
            Patch patch=emission.patch;patch.slot=firstSlot+i;notePatch(*pool,patch.slot);pool->patches[patch.slot]=patch;++g_stats.joined;
        } catch(...) {overflow(frame);return;}
    }
}
inline void endMap(ID3D11Buffer* resource,uint64_t sequence) noexcept {
    ClaimLock lock(Caller::EndMap);g_poolsDirty=true;auto i=g_pools.find(resource);if(i==g_pools.end())return;
    Pool& p=i->second;
    if(!p.active || p.sequence!=sequence){clearPatches(p);p.valid=false;++g_stats.declined;}
    p.active=false;
}
inline void forget(ID3D11Buffer* resource) noexcept {
    ClaimLock lock;g_poolsDirty=true;g_pools.erase(resource);
}
inline Stats stats();
inline std::vector<Patch> patches(ID3D11Buffer* resource,uint32_t frame) {
    ClaimLock lock(Caller::Other);std::vector<Patch> out;
    const auto i=g_pools.find(resource);
    if(i==g_pools.end() || i->second.active || !i->second.valid || i->second.frame!=frame || g_overflowFrame==frame)return out;
    out.reserve(i->second.patches.size());for(const auto& p:i->second.patches)out.push_back(p.second);return out;
}
inline bool privateBound(ID3D11DeviceContext* ctx,ID3D11Buffer* buffer) {
    ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
    auto check=[&](auto getter){
        (ctx->*getter)(0,D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT,views);bool found=false;
        for(auto*& v:views)if(v){ComPtr<ID3D11Resource> r;v->GetResource(&r);found=found || r.Get()==buffer;v->Release();v=nullptr;}
        return found;
    };
    if(check(&ID3D11DeviceContext::CSGetShaderResources) || check(&ID3D11DeviceContext::VSGetShaderResources) ||
       check(&ID3D11DeviceContext::PSGetShaderResources) || check(&ID3D11DeviceContext::GSGetShaderResources) ||
       check(&ID3D11DeviceContext::HSGetShaderResources) || check(&ID3D11DeviceContext::DSGetShaderResources))return true;
    ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
    ctx->CSGetUnorderedAccessViews(0,D3D11_PS_CS_UAV_REGISTER_COUNT,uavs);bool found=false;
    for(auto* v:uavs)if(v){ComPtr<ID3D11Resource> r;v->GetResource(&r);found=found || r.Get()==buffer;v->Release();}
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,D3D11_PS_CS_UAV_REGISTER_COUNT,uavs);
    for(auto* v:uavs)if(v){ComPtr<ID3D11Resource> r;v->GetResource(&r);found=found || r.Get()==buffer;v->Release();}
    return found;
}
struct MergeTransfer { uintptr_t target=0; Emission emission; };
struct MergePlan { std::vector<MergeTransfer> transfers; std::vector<uintptr_t> addresses; uint64_t epoch=0; uint32_t frame=0; bool invalid=false; };
inline Stats stats() {
    ClaimLock lock;Stats result=g_stats;
    const uint64_t clearSkipped=g_clearSkipped.load(std::memory_order_relaxed),mergeSkipped=g_mergeSkipped.load(std::memory_order_relaxed);
    const uint64_t clearPrechecked=g_clearPrechecked.load(std::memory_order_relaxed),mergePrechecked=g_mergePrechecked.load(std::memory_order_relaxed);
    result.clearCalls+=clearSkipped+clearPrechecked;result.clearNoClaims+=clearSkipped;
    result.mergeCalls+=mergeSkipped+mergePrechecked;result.mergeWithoutClaims+=mergeSkipped;
    result.clearPrechecked=clearPrechecked;result.mergePrechecked=mergePrechecked;
    result.precheckDeclined=g_precheckDeclined.load(std::memory_order_relaxed);
    // One read of each counter feeds both its total and its lock-free figure: these advance outside g_mutex, and two loads could straddle a
    // fast copy, leaving a lock-free count above the total it is said to be inside.
    // Each no-lease counter is read before the counter it is a part of (its writer bumps the total first), so a figure never exceeds its total.
    const uint64_t noLeaseSkipped=g_copierNoLeaseSkipped.load(std::memory_order_relaxed),skipped=g_copierSkipped.load(std::memory_order_relaxed);
    const uint64_t noLeasePrechecked=g_copierPrecheckedNoLease.load(std::memory_order_relaxed),prechecked=g_copierPrechecked.load(std::memory_order_relaxed);
    const uint64_t copierSkipped=skipped+prechecked,copierNoLeaseSkipped=noLeaseSkipped+noLeasePrechecked;
    result.copies+=copierSkipped;result.copierNoLease+=copierNoLeaseSkipped;
    result.copierPrechecked=prechecked;result.copierPrecheckDeclined=g_copierPrecheckDeclined.load(std::memory_order_relaxed);
    result.clearLockFree=clearSkipped+clearPrechecked;result.mergeLockFree=mergeSkipped+mergePrechecked;
    result.copiesLockFree=copierSkipped;result.copierNoLeaseLockFree=copierNoLeaseSkipped;
    result.activeClaims=g_emissions.size();result.activePlans=g_mergePlans.size();return result;
}
constexpr size_t kExitLineBytes=760;
// The calls the lock-free exits handled, against all calls of the same kind. The totals are the figures the other lines
// print (clear calls, merge calls, copier spans' native copy ranges), which include these. The claim check's share: the clears
// and merges that found claims live, walked the native list without the mutex and touched none (inside the clear and merge
// figures), and the calls that wanted the check and took the mutex for a reason other than a hit.
inline void formatExitLine(const Stats& s,char (&out)[kExitLineBytes]) noexcept {
    std::snprintf(out,kExitLineBytes,"engine motion: primary copy lock-free exits cumulative (already counted in the figures above; these calls never took "
                  "the mutex): clear %llu of %llu calls, merge %llu of %llu calls, copier %llu of %llu calls (%llu of those found no active map). "
                  "Claim check (live claims, none touched): %llu clears and %llu merges of those, %llu calls declined it. "
                  "Copier claim check (claims live or patches held, none in the range): %llu copies of those, %llu copies declined it.",
                  static_cast<unsigned long long>(s.clearLockFree),static_cast<unsigned long long>(s.clearCalls),
                  static_cast<unsigned long long>(s.mergeLockFree),static_cast<unsigned long long>(s.mergeCalls),
                  static_cast<unsigned long long>(s.copiesLockFree),static_cast<unsigned long long>(s.copies),
                  static_cast<unsigned long long>(s.copierNoLeaseLockFree),
                  static_cast<unsigned long long>(s.clearPrechecked),static_cast<unsigned long long>(s.mergePrechecked),
                  static_cast<unsigned long long>(s.precheckDeclined),
                  static_cast<unsigned long long>(s.copierPrechecked),static_cast<unsigned long long>(s.copierPrecheckDeclined));
}
inline void invalidateClaims() {g_emissions.clear();++g_emissionEpoch;++g_stats.declined;++g_stats.mergeFailed;}
// 3684630 frees every list node in this typed336 dictionary. Destination
// clear precedes a merge of unrelated source collections, so only claims in
// nodes actually being freed may be revoked on a successful bounded walk.
// The bounded walk of a typed336 dictionary's list nodes, shared by the locked clear and by the lock-free claim check, so the two
// refuse exactly the same shapes. `visit(node)` runs for each node after it has been validated and counted in `nodesWalked`;
// returning false stops the walk. The result is true only when the whole dictionary was walked and well formed. Allocates only
// when a set outgrows its inline buffer, and then throws what the allocation throws.
template<class Visit> inline bool walkDictionary(uintptr_t dictionary,uint64_t& nodesWalked,Visit&& visit) {
    uintptr_t buckets=0;uint64_t count=0;
    if(!dictionary || dictionary>UINTPTR_MAX-0x20 ||
       !emit::read(dictionary+0x10,&buckets,8) || !emit::read(dictionary+0x18,&count,8) ||
       count>kMaxEmissions || (count && (!buckets || buckets>UINTPTR_MAX-count*8)))return false;
    AddressSet entries,nodes;
    for(uint64_t b=0;b<count;++b){
        const uintptr_t sentinel=buckets+uintptr_t(b)*8;uintptr_t entry=0;
        if(!emit::read(sentinel,&entry,8))return false;
        while(entry!=sentinel){
            if(!entry || entry>UINTPTR_MAX-0x38 || entries.size()>=kMaxEmissions || !entries.insert(entry).second)return false;
            uintptr_t nextEntry=0,links[2]{};const uintptr_t anchor=entry+0x28;
            if(!emit::read(entry,&nextEntry,8) || !emit::read(anchor,links,16))return false;
            uintptr_t node=links[0],previous=anchor;
            while(node!=anchor){
                uint64_t header[4]{};
                if(!node || node>UINTPTR_MAX-(emit::kNodeRecords+8*kBytes) || nodes.size()>=kMaxEmissions/8 ||
                   !nodes.insert(node).second || !emit::read(node,header,32) || header[1]!=previous || header[3]>8)return false;
                ++nodesWalked;
                if(!visit(node))return false;
                previous=node;node=uintptr_t(header[0]);
            }
            if(links[1]!=previous)return false;
            entry=nextEntry;
        }
    }
    return true;
}
// The claim check of a clear (see the claim snapshot): true when the call is fully handled without the mutex because no live
// claim is in any node it would free. False: take the mutex. An empty snapshot is a flag one release behind: the call takes the
// mutex, which corrects it as it always did. Declined (counted) for a plan in flight, more than kClaimViewMax claims, a write in flight, and a walk the locked
// path must act on (a refusal clears every claim); a hit is not a decline.
inline bool clearClaimCheck(uintptr_t dictionary) noexcept {
    if(!g_claimPrecheck)return false;
    ClaimViewState view;
    if(!readClaimView(view) || view.plans || view.count>kClaimViewMax){g_precheckDeclined.fetch_add(1,std::memory_order_relaxed);return false;}
    if(!view.count)return false;   // the flag is one release behind: under the lock the same rule corrects it
    bool hit=false;
    try {
        uint64_t walked=0;
        if(!walkDictionary(dictionary,walked,[&](uintptr_t node){hit=view.holds(node);return !hit;})){
            if(!hit)g_precheckDeclined.fetch_add(1,std::memory_order_relaxed);
            return false;   // a hit, or a refusal the locked walk acts on (clear)
        }
    } catch(...) {g_precheckDeclined.fetch_add(1,std::memory_order_relaxed);return false;}
#ifdef EDVR_ENGINE_VELOCITY_RIG
    if(g_claimAfterWalk)g_claimAfterWalk();
#endif
    g_clearPrechecked.fetch_add(1,std::memory_order_relaxed);
    return true;
}
template<bool SkipNoClaims> inline void invalidateDictionaryImpl(uintptr_t dictionary) noexcept {
    // Only source claims and detached merge plans consume this walk. With none published there is nothing to revoke
    // and nothing to mark invalid, so the call is counted and returns without the mutex.
    if constexpr(SkipNoClaims) {
        if(!g_claimsLive.load(std::memory_order_acquire)){g_clearSkipped.fetch_add(1,std::memory_order_relaxed);return;}
        // Claims are live. A call whose nodes hold none of them, with no plan in flight, has nothing to revoke either.
        if(clearClaimCheck(dictionary))return;
    }
    ClaimLock lock(Caller::ClearWalk);++g_stats.clearCalls;
    // Copied GPU-pool certificates are independent and must stay intact.
    // Future emissions register fresh ownership after this lock is released.
    if constexpr(SkipNoClaims) {
        if(g_emissions.empty() && g_mergePlans.empty()){lock.relabel(Caller::ClearSkip);++g_stats.clearNoClaims;return;}   // the flag was stale-true
    }
    auto fail=[](){g_emissions.clear();++g_emissionEpoch;++g_stats.sourceResets;++g_stats.clearFailed;};
    try {
        AddressSet addresses;
        if(!walkDictionary(dictionary,g_stats.clearNodes,[&](uintptr_t node){
               for(uint32_t i=0;i<8;++i)addresses.insert(node+emit::kNodeRecords+uintptr_t(i)*kBytes);
               return true;
           })){fail();return;}
        addresses.forEach([](uintptr_t address){const auto n=g_emissions.erase(address);g_stats.invalidated+=n;g_stats.clearedClaims+=n;});
        // A plan owns detached claims between the two native merge callbacks.
        // Drop only plans touching this clear, never unrelated live owners.
        for(auto* plan:g_mergePlans)for(auto address:plan->addresses)if(addresses.count(address)){plan->invalid=true;break;}
    }catch(...){fail();}
}
inline void invalidateDictionary(uintptr_t dictionary) noexcept {invalidateDictionaryImpl<true>(dictionary);}
// 43513A0: full eight-item nodes splice at the HEAD, preserving a nonempty
// destination tail; partial nodes fill the
// destination tail, then shift any remainder within their original node.
// Claims are staged before ALL source slots/overwritten destinations clear.
//
// With no source claim published there is nothing to stage (transfers come from g_emissions and from nowhere else, and a
// plan another merge holds has already taken its claims out of it), so the call is counted and gets the fixed token
// below: no lock, no walk of the native list, no allocation, no entry in g_mergePlans. A detached plan in flight does not
// change that: plans do not read each other, and a plan that holds nothing must not keep the path locked. A producer may still
// register a claim between this callback and its end, and a native unwind must still drop it, so endMergeOpaque
// invalidates the claims on an unwind of this token exactly as it does for a full plan. What the token does not keep is
// an empty plan's other behaviours, all of which act on an empty transfer list: its membership in g_mergePlans (so a
// concurrent clear no longer walks on its account, and cannot mark it invalid), the node validation walk (so a
// malformed list no longer bumps the epoch or counts a merge failure when there is no claim to lose) and the epoch
// compare (an unwind after a clear or a frame change now drops claims the old empty plan, found stale, would have
// spared: the safe direction, and only on a native unwind).
inline void* mergeNoClaimsToken() noexcept {return reinterpret_cast<void*>(uintptr_t(1));}
// The bounded walk of the source list of a merge, shared by the locked merge and by the lock-free claim check. For each source node,
// after it has been validated and counted in `nodesWalked`, `visit(node, count, fill, tail, tailCount)` runs: `count` records are in
// the node, `fill` of them go to the end of the destination's tail node `tail` (which holds `tailCount` already), and the rest stay
// in `node`. Returning false stops the walk. True only when the whole list was walked and well formed. Allocates only when the
// visited set outgrows its inline buffer, and then throws what the allocation throws.
template<class Visit> inline bool walkMerge(uintptr_t destination,uintptr_t source,uint64_t& nodesWalked,Visit&& visit) {
    uintptr_t dstLinks[2]{},srcLinks[2]{};
    if(!destination || !source || destination==source ||
       !emit::read(destination,dstLinks,sizeof(dstLinks)) || !emit::read(source,srcLinks,sizeof(srcLinks)))return false;
    AddressSet visited;
    uintptr_t tail=dstLinks[1],node=srcLinks[0],previous=source;
    uint64_t tailCount=0;
    if(dstLinks[0]==destination) {
        if(tail!=destination)return false;
    } else {
        uintptr_t next=0;
        if(!tail || !emit::read(tail,&next,sizeof(next)) || next!=destination ||
           !emit::read(tail+emit::kNodeCount,&tailCount,8) || tailCount>8)return false;
    }
    while(node!=source) {
        uint64_t header[4]{};
        if(!node || node==destination || node==tail || node>UINTPTR_MAX-(emit::kNodeRecords+8*kBytes) ||
           visited.size()>=kMaxEmissions/8 || !visited.insert(node).second ||
           !emit::read(node,header,sizeof(header)) || header[1]!=previous || header[3]==0 || header[3]>8)return false;
        ++nodesWalked;
        const uint32_t count=uint32_t(header[3]);
        uint32_t fill=0;
        if(count!=8 && tail!=destination && tailCount!=8)fill=std::min(count,uint32_t(8-tailCount));
        if(!visit(node,count,fill,tail,tailCount))return false;
        tailCount+=fill;
        if(count==8){if(tail==destination){tail=node;tailCount=8;}}
        else if(count>fill){tail=node;tailCount=count-fill;}
        previous=node;node=uintptr_t(header[0]);
    }
    return srcLinks[1]==previous;
}
// The claim check of a merge (see the claim snapshot): true when the call is handled without the mutex because no live claim is in
// any record slot the merge would clear or overwrite, in which case the merge gets the fixed token below (no plan, no walk under
// the lock): what the locked merge would build there is a plan with no transfers and nothing erased. False: take the mutex, for a
// hit (the full plan, and its end's epoch check, are unchanged) or a decline (counted: a plan in flight, more than kClaimViewMax
// claims, a write in flight, or a list the locked walk refuses and acts on).
inline bool mergeClaimCheck(uintptr_t destination,uintptr_t source) noexcept {
    if(!g_claimPrecheck)return false;
    ClaimViewState view;
    if(!readClaimView(view) || view.plans || view.count>kClaimViewMax){g_precheckDeclined.fetch_add(1,std::memory_order_relaxed);return false;}
    if(!view.count)return false;   // the flag is one release behind: under the lock the same rule corrects it
    bool hit=false;
    try {
        uint64_t walked=0;
        if(!walkMerge(destination,source,walked,[&](uintptr_t node,uint32_t,uint32_t fill,uintptr_t tail,uint64_t tailCount){
               hit=view.holds(node) || (fill && view.holds(tail,tailCount,tailCount+fill));
               return !hit;
           })){
            if(!hit)g_precheckDeclined.fetch_add(1,std::memory_order_relaxed);
            return false;   // a hit, or a refusal the locked walk acts on (merge)
        }
    } catch(...) {g_precheckDeclined.fetch_add(1,std::memory_order_relaxed);return false;}
#ifdef EDVR_ENGINE_VELOCITY_RIG
    if(g_claimAfterWalk)g_claimAfterWalk();
#endif
    g_mergePrechecked.fetch_add(1,std::memory_order_relaxed);
    return true;
}
inline void* beginMergeOpaque(uintptr_t destination,uintptr_t source,uint32_t frame) noexcept {
    if(!g_emissionsLive.load(std::memory_order_acquire)){g_mergeSkipped.fetch_add(1,std::memory_order_relaxed);return mergeNoClaimsToken();}
    // Claims are live. A merge whose records hold none of them, with no plan in flight, stages and erases nothing: the token.
    if(mergeClaimCheck(destination,source))return mergeNoClaimsToken();
    ClaimLock lock(Caller::MergeBegin);
    ++g_stats.mergeCalls;
    // The flag may be stale-true: the emissions were consumed since it was published. Under the lock the same rule holds.
    if(g_emissions.empty()){++g_stats.mergeWithoutClaims;return mergeNoClaimsToken();}
    try {
        auto plan=std::make_unique<MergePlan>();plan->epoch=g_emissionEpoch;plan->frame=frame;
        if(g_mergePlans.size()>=kMaxPools){invalidateClaims();return nullptr;}
        std::vector<uintptr_t> clear;
        if(!walkMerge(destination,source,g_stats.mergeNodes,[&](uintptr_t node,uint32_t count,uint32_t fill,uintptr_t tail,uint64_t tailCount){
               for(uint32_t i=0;i<8;++i)clear.push_back(node+emit::kNodeRecords+uintptr_t(i)*kBytes);
               for(uint32_t i=0;i<fill;++i)clear.push_back(tail+emit::kNodeRecords+uintptr_t(tailCount+i)*kBytes);
               for(uint32_t i=0;i<count;++i) {
                   const uintptr_t old=node+emit::kNodeRecords+uintptr_t(i)*kBytes;
                   const auto claim=g_emissions.find(old);
                   if(claim==g_emissions.end() || claim->second.frame!=frame)continue;
                   std::array<uint32_t,kWords> native{};
                   if(!emit::read(old,native.data(),kBytes))return false;
                   if(native!=claim->second.patch.native)continue;
                   const uintptr_t target=i<fill?tail+emit::kNodeRecords+uintptr_t(tailCount+i)*kBytes:
                       node+emit::kNodeRecords+uintptr_t(i-fill)*kBytes;
                   plan->transfers.push_back({target,claim->second});
               }
               return true;
           })){invalidateClaims();return nullptr;}
        for(uintptr_t address:clear)g_emissions.erase(address);
        plan->addresses=std::move(clear);g_mergePlans.insert(plan.get());
        ++g_stats.mergePlans;
        return plan.release();
    } catch(...) {invalidateClaims();return nullptr;}
}
inline void endMergeOpaque(void* opaque,bool completed) noexcept {
    if(opaque==mergeNoClaimsToken()) {
        if(!completed){ClaimLock lock(Caller::MergeEnd);invalidateClaims();}
        return;
    }
    std::unique_ptr<MergePlan> plan(static_cast<MergePlan*>(opaque));if(!plan)return;
    ClaimLock lock(Caller::MergeEnd);
    g_mergePlans.erase(plan.get());
    if(plan->invalid || plan->epoch!=g_emissionEpoch){++g_stats.mergeFailed;return;}
    if(!completed){invalidateClaims();return;}
    try {
        for(const auto& move:plan->transfers) {
            std::array<uint32_t,kWords> native{};
            if(!emit::read(move.target,native.data(),kBytes) || native!=move.emission.patch.native) {
                g_emissions.erase(move.target);++g_stats.declined;continue;
            }
            // A concurrently re-registered target is not this merge's owner.
            if(g_emissions.find(move.target)!=g_emissions.end()){invalidateClaims();return;}
            if(g_emissions.size()>=kMaxEmissions){overflow(plan->frame);return;}
            g_emissions.emplace(move.target,move.emission);
        }
    } catch(...) {invalidateClaims();}
}
// Owned by one eye/source private pool, not by the shared compute cache:
// left/right alternate every frame. Held resource identity prevents an old
// view being reused for a different allocation at the same address.
struct OutputCache {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Buffer> pool;
    ComPtr<ID3D11UnorderedAccessView> view;
};
struct GpuCache {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> upload,count;
    ComPtr<ID3D11ShaderResourceView> input;
    uint32_t capacity=0;
};
inline GpuCache g_gpu;
inline bool apply(ID3D11DeviceContext* ctx,ID3D11Buffer* privatePool,ID3D11Buffer* sourcePool,uint32_t frame,OutputCache& output) noexcept {
    auto fail=[](ScatterFailure why){ClaimLock lock;++g_stats.scatterFailed;g_stats.lastScatterFailure=why;return false;};
    if(!ctx || !privatePool || !sourcePool || privatePool==sourcePool)return fail(ScatterFailure::Arguments);
    try {
        const auto work=patches(sourcePool,frame);if(work.empty()){ClaimLock lock;++g_stats.scatterEmpty;return true;}
        std::lock_guard<std::mutex> gpu(g_gpuMutex);
        D3D11_BUFFER_DESC d{},native{};privatePool->GetDesc(&d);sourcePool->GetDesc(&native);
        if(d.ByteWidth!=native.ByteWidth || d.StructureByteStride!=kBytes ||
           !(d.BindFlags&D3D11_BIND_UNORDERED_ACCESS) || !(d.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_STRUCTURED))return fail(ScatterFailure::Descriptor);
        if(privateBound(ctx,privatePool))return fail(ScatterFailure::Bound);
        ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
        if(g_gpu.device.Get()!=device.Get()){g_gpu={};g_gpu.device=device;}
        if(output.device.Get()!=device.Get() || output.pool.Get()!=privatePool)output={};
        if(!g_gpu.shader && FAILED(device->CreateComputeShader(kEnginePrimaryCopyScatterBytecode,sizeof(kEnginePrimaryCopyScatterBytecode),nullptr,&g_gpu.shader)))return fail(ScatterFailure::Shader);
        if(g_gpu.capacity<work.size()) {
            g_gpu.upload.Reset();g_gpu.input.Reset();g_gpu.capacity=0;
            uint32_t capacity=64;while(capacity<work.size())capacity*=2;
            D3D11_BUFFER_DESC upload{};upload.ByteWidth=capacity*sizeof(Patch);upload.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            upload.Usage=D3D11_USAGE_DYNAMIC;upload.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
            upload.StructureByteStride=sizeof(Patch);upload.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            if(FAILED(device->CreateBuffer(&upload,nullptr,&g_gpu.upload)) ||
               FAILED(device->CreateShaderResourceView(g_gpu.upload.Get(),nullptr,&g_gpu.input)))return fail(ScatterFailure::Upload);
            g_gpu.capacity=capacity;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(FAILED(ctx->Map(g_gpu.upload.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped)))return fail(ScatterFailure::Map);
        std::memcpy(mapped.pData,work.data(),work.size()*sizeof(Patch));ctx->Unmap(g_gpu.upload.Get(),0);
        if(!output.view) {
            ComPtr<ID3D11UnorderedAccessView> view;
            if(FAILED(device->CreateUnorderedAccessView(privatePool,nullptr,&view)))return fail(ScatterFailure::View);
            output.device=device;output.pool=privatePool;output.view=std::move(view);
        }
        const uint32_t counts[4]={UINT(work.size()),d.ByteWidth/kBytes,0,0};
        if(!g_gpu.count){D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if(FAILED(device->CreateBuffer(&cb,nullptr,&g_gpu.count)))return fail(ScatterFailure::Counts);}
        ctx->UpdateSubresource(g_gpu.count.Get(),0,nullptr,counts,0,0);
        ComPtr<ID3D11ComputeShader> savedShader;ID3D11ClassInstance* classes[256]{};UINT numClasses=256;
        ctx->CSGetShader(&savedShader,classes,&numClasses);
        ComPtr<ID3D11ShaderResourceView> savedInput;ctx->CSGetShaderResources(0,1,&savedInput);
        ComPtr<ID3D11UnorderedAccessView> savedOutput;ctx->CSGetUnorderedAccessViews(0,1,&savedOutput);
        ComPtr<ID3D11Buffer> savedCount;ctx->CSGetConstantBuffers(0,1,&savedCount);
        ctx->CSSetShader(g_gpu.shader.Get(),nullptr,0);ctx->CSSetShaderResources(0,1,g_gpu.input.GetAddressOf());
        ctx->CSSetUnorderedAccessViews(0,1,output.view.GetAddressOf(),nullptr);ctx->CSSetConstantBuffers(0,1,g_gpu.count.GetAddressOf());
        ctx->Dispatch((UINT(work.size())+63)/64,1,1);
        ID3D11UnorderedAccessView* empty=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&empty,nullptr);
        ctx->CSSetShaderResources(0,1,savedInput.GetAddressOf());ctx->CSSetUnorderedAccessViews(0,1,savedOutput.GetAddressOf(),nullptr);
        ctx->CSSetConstantBuffers(0,1,savedCount.GetAddressOf());ctx->CSSetShader(savedShader.Get(),classes,numClasses);
        for(UINT i=0;i<numClasses;++i)if(classes[i])classes[i]->Release();
        ClaimLock lock;++g_stats.scatterBatches;g_stats.scatterRows+=work.size();return true;
    } catch(...) {return fail(ScatterFailure::Unexpected);}
}
inline void reset() noexcept {
    std::lock_guard<std::mutex> gpu(g_gpuMutex);g_gpu={};
    ClaimLock lock;g_poolsDirty=true;g_emissions.clear();g_pools.clear();g_mergePlans.clear();g_stats={};g_overflowFrame=g_emissionFrame=~0u;++g_emissionEpoch;
    g_clearSkipped.store(0,std::memory_order_relaxed);g_mergeSkipped.store(0,std::memory_order_relaxed);
    g_clearPrechecked.store(0,std::memory_order_relaxed);g_mergePrechecked.store(0,std::memory_order_relaxed);g_precheckDeclined.store(0,std::memory_order_relaxed);
    g_copierSkipped.store(0,std::memory_order_relaxed);g_copierNoLeaseSkipped.store(0,std::memory_order_relaxed);
    g_copierPrechecked.store(0,std::memory_order_relaxed);g_copierPrecheckedNoLease.store(0,std::memory_order_relaxed);g_copierPrecheckDeclined.store(0,std::memory_order_relaxed);
}
} }
