#include "kinematic_eval_hook.h"
#include "kinematic_eval_probe.h"
#include "../common/code_hook.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <mutex>

namespace edvr {
namespace {

using EvalFn = uintptr_t (__fastcall*)(uintptr_t,uintptr_t,uintptr_t);
using JobFn = uintptr_t (__fastcall*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t);
// FUN_14431AFE0 (decomp_431AFE0.txt): ulonglong f(longlong rig, longlong
// poseCtx) -- exactly two register params, rig state checked at +0x380.
using RigEvalFn = uintptr_t (__fastcall*)(uintptr_t,uintptr_t);

// The evaluator and the job bodies live in the GAME'S module; this DLL loads
// more than two gigabytes away, which a five-byte E9 patch cannot reach
// (first flight, 2026-09-19: "more than two gigabytes from the replacement.
// Not hooked."). The answer is the record-writer hook's relay pattern: a
// 44-byte stub allocated within reach of the target, which tail-jumps
// through absolute eight-byte operands that have no range limit. Do not
// "simplify" this back to passing the replacement to CodeHook directly.
alignas(8) std::atomic<KinematicEvalProbe*> observer{nullptr};
static_assert(decltype(observer)::is_always_lock_free,
              "The x64 relay reads the aligned atomic pointer directly.");

// The relay gate, distinct from observer: non-zero while EITHER consumer
// wants eval callbacks -- the probe while attached (eye-dump captures) or
// the kinematic tracker while fix.engine_motion is on (no dump involved).
// observer stays the probe's own cell; the relays gate on evalGate.
alignas(8) std::atomic<uintptr_t> evalGate{0};
static_assert(decltype(evalGate)::is_always_lock_free,
              "The x64 relay reads the aligned atomic pointer directly.");
std::atomic<bool> trackerWanted{false};
alignas(8) std::atomic<KinematicTrackerObserverFn> trackerObserver{nullptr};

// The probe is a process-lifetime global (kinematicEvalProbe), so a bracket
// that loaded the pointer before a detach remains safe while it finishes;
// detach only stops NEW callbacks. No in-flight drain is needed.
int64_t g_qpcFreq=0;

struct HookEntry {
    const char* name;
    uintptr_t rva;
    void* callback;
    std::atomic<uintptr_t> forward{0};
    CodeHook hook;
    uint8_t* relay=nullptr;
    // Published release AFTER the relay+trampoline are live; readers acquire.
    // patchIsOurs reads this from contexts that do not hold g_installMutex,
    // so a plain bool could observe a half-installed entry (finding 5).
    std::atomic<bool> ready{false};
};

// Job bodies behind the run thunks (recovered from the hash-verified exe,
// SHA-256 e6be8bbe...; thunk jmps decoded 2026-09-19):
constexpr uintptr_t kJobRvas[KinematicEvalProbe::kJobCount]={
    0x4321940, // UpdateRenderDataJob   (thunk 0x42DF520)
    0x4320340, // render-data batch     (thunk 0x42DFAF0)
    0x432B2A0, // UpdatePhysicsObjectsJob (thunk 0x42DF540)
    0x42DF530, // PrePhysicsAdvanceJob  (run IS the body)
    0x42DF550, // PrePhysicsAdvanceCurveJob (run IS the body)
    0x4321810, // unnamed table BA0     (thunk 0x42DFBA0)
};

int64_t qpcNow() noexcept {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Job 2 (UpdatePhysicsObjectsJob, 0x432B2A0) bit-compare-gates its NODE
// matrix writes and appends every changed node to a dirty queue: the job
// descriptor's +0x18 is a pointer to the queue's atomic append counter
// (decomp_432B2A0, param_1[3]; flushed in 0x20 batches by FUN_144899F50).
// Reading the counter at job entry and exit yields the per-run append
// count -- the write-volume number the perf arc's L4 needs and the
// lifecycle evidence a node capture would have to respect. Counts only.
bool readQueueCount(uintptr_t descriptor,uint32_t* out) noexcept {
    __try {
        uintptr_t counter=0;
        std::memcpy(&counter,reinterpret_cast<const void*>(descriptor+0x18),8);
        if(!counter)return false;
        uint32_t count=0;
        std::memcpy(&count,reinterpret_cast<const void*>(counter),4);
        *out=count;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

// --- Relay machinery, mirrored from object_record_writer_hook.cpp. --------
// Kept as a copy rather than a shared unit so the flight-proven writer hook
// file is not touched; if one changes, change both.
constexpr size_t kRelayBytes=44,kOriginalLiteral=36;

uint8_t* allocateRelay(uintptr_t target) noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);
    const uintptr_t granularity=info.dwAllocationGranularity;
    const uintptr_t floor=reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t ceiling=reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t distance=uintptr_t(INT32_MAX)-0x10000u;
    uintptr_t at=target>distance?target-distance:floor;
    if(at<floor)at=floor;
    const uintptr_t limit=target>ceiling-distance?ceiling:target+distance;
    while(at<limit) {
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&region,sizeof(region)))break;
        const uintptr_t start=reinterpret_cast<uintptr_t>(region.BaseAddress);
        if(region.RegionSize>UINTPTR_MAX-start)break;
        const uintptr_t end=start+region.RegionSize;
        if(region.State==MEM_FREE) {
            uintptr_t candidate=at>start?at:start;
            if(candidate>UINTPTR_MAX-(granularity-1))break;
            candidate=(candidate+granularity-1)&~(granularity-1);
            if(candidate<limit && candidate<end && end-candidate>=4096) {
                auto* p=static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate),4096,
                                                         MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
                if(p)return p;
            }
        }
        if(end<=at)break;at=end;
    }
    return nullptr;
}

void buildRelay(uint8_t* code,const void* gate,void* callback) noexcept {
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback];
    // original: jmp [trampoline]. RAX/flags are volatile, and neither the
    // evaluator nor the job bodies consume RAX on entry. No stack adjustment
    // or nonvolatile modification occurs.
    const uint8_t body[kRelayBytes]={
        0x48,0xB8,0,0,0,0,0,0,0,0, 0x48,0x83,0x38,0,
        0x74,0x0E, 0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0,
        0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0};
    std::memcpy(code,body,sizeof(body));
    const uintptr_t gateAddress=reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress=reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code+2,&gateAddress,8);std::memcpy(code+22,&callbackAddress,8);
}

bool prepareRelay(void* trampoline,void* context) noexcept {
    auto& entry=*static_cast<HookEntry*>(context);
    const uintptr_t address=reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(entry.relay+kOriginalLiteral,&address,8);
    DWORD oldProtect=0;
    if(!VirtualProtect(entry.relay,4096,PAGE_EXECUTE_READ,&oldProtect) ||
       !FlushInstructionCache(GetCurrentProcess(),entry.relay,kRelayBytes))return false;
    entry.forward.store(address,std::memory_order_release);
    return true;
}
// ---------------------------------------------------------------------------

// Forward declarations: the entry table initialises with callback addresses,
// the callbacks read the entries' forward trampolines.
__declspec(noinline) uintptr_t __fastcall evalObserved(uintptr_t,uintptr_t,uintptr_t) noexcept;
__declspec(noinline) uintptr_t __fastcall rigEvalObserved(uintptr_t,uintptr_t) noexcept;
#define EDVR_JOB_PROTO(i) \
    __declspec(noinline) uintptr_t __fastcall job##i(uintptr_t,uintptr_t,uintptr_t,uintptr_t) noexcept;
EDVR_JOB_PROTO(0) EDVR_JOB_PROTO(1) EDVR_JOB_PROTO(2)
EDVR_JOB_PROTO(3) EDVR_JOB_PROTO(4) EDVR_JOB_PROTO(5)
#undef EDVR_JOB_PROTO

HookEntry g_evalEntry{"kinematic-eval",KinematicEvalProbe::kEvalRva,
                      reinterpret_cast<void*>(&evalObserved)};
HookEntry g_rigEvalEntry{"kinematic-rig-eval",KinematicEvalProbe::kRigEvalRva,
                         reinterpret_cast<void*>(&rigEvalObserved)};
HookEntry g_jobEntries[KinematicEvalProbe::kJobCount]={
    {"kinematic-job-0",kJobRvas[0],reinterpret_cast<void*>(&job0)},
    {"kinematic-job-1",kJobRvas[1],reinterpret_cast<void*>(&job1)},
    {"kinematic-job-2",kJobRvas[2],reinterpret_cast<void*>(&job2)},
    {"kinematic-job-3",kJobRvas[3],reinterpret_cast<void*>(&job3)},
    {"kinematic-job-4",kJobRvas[4],reinterpret_cast<void*>(&job4)},
    {"kinematic-job-5",kJobRvas[5],reinterpret_cast<void*>(&job5)},
};

// Per-thread mask of the job brackets currently on the stack (1u<<jobId),
// maintained by bracket() and read by evalObserved: attributes every eval
// observation to the engine job that scheduled it. Nesting-safe (save/
// restore), zero when no observed job is running.
thread_local uint32_t t_jobMask=0;

__declspec(noinline) uintptr_t __fastcall evalObserved(uintptr_t descriptor,uintptr_t param2,
                                                       uintptr_t renderRecord) noexcept {
    const uint32_t jobMask=t_jobMask;
    KinematicEvalProbe* probe=observer.load(std::memory_order_acquire);
    if(probe)probe->observe(descriptor,renderRecord,jobMask); // observe() gates on active()
    const auto tracker=trackerObserver.load(std::memory_order_acquire);
    if(tracker)tracker(descriptor,jobMask); // kinematicMotionObserve gates on its own flag
    const auto forward=reinterpret_cast<EvalFn>(g_evalEntry.forward.load(std::memory_order_acquire));
    return forward(descriptor,param2,renderRecord);
}

__declspec(noinline) uintptr_t __fastcall rigEvalObserved(uintptr_t rig,uintptr_t poseCtx) noexcept {
    KinematicEvalProbe* probe=observer.load(std::memory_order_acquire);
    if(probe)probe->noteRigLink(rig,poseCtx); // noteRigLink() gates on active()
    const auto forward=reinterpret_cast<RigEvalFn>(g_rigEvalEntry.forward.load(std::memory_order_acquire));
    return forward(rig,poseCtx);
}

uintptr_t __fastcall bracket(uint32_t job,uintptr_t a,uintptr_t b,
                             uintptr_t c,uintptr_t d) noexcept {
    const auto forward=reinterpret_cast<JobFn>(g_jobEntries[job].forward.load(std::memory_order_acquire));
    if(!forward)return 0; // this job stood down at install; the relay is unreachable then
    // Job attribution: every eval observation made while this job runs on
    // this thread carries its bit. Save/restore so nested jobs keep both
    // bits and early returns always unwind the mask.
    struct JobBit {
        uint32_t prev;
        explicit JobBit(uint32_t job):prev(t_jobMask){t_jobMask=prev|(1u<<job);}
        ~JobBit(){t_jobMask=prev;}
    } jobBit(job);
    KinematicEvalProbe* probe=observer.load(std::memory_order_acquire);
    // Timing runs whenever the hooks own the job bodies, probe armed or not
    // (the L1 brackets-only remeasure, perf doc 2026-09-20 17:55): the
    // detailed observer's cost enters the measured region only while it is
    // actually capturing, which is exactly the with/without comparison L1
    // wants. The probe global is process-lifetime, so the tracker-only path
    // (fix.engine_motion on, probe never attached) times too. jobs[] now
    // accumulates per-session, not per capture window. Ownership capture
    // stays capture-gated: observe()/noteOwnership keep their active() gates.
    if(probe && probe->active())probe->noteOwnership(job,a);
    // Job 2's dirty-queue append counter, read at entry (exit read after
    // the body below). Outside the timed region: L1 measures the job body,
    // not this probe. Goes to the process-lifetime global, not the observer
    // pointer, so tracker-only flights capture it too.
    uint32_t queueEntry=0;
    const bool queueArmed=(job==2)&&readQueueCount(a,&queueEntry);
    // Capture boundary (2026-09-20 review finding 8): samples commit only to
    // the generation they started in -- a reset/re-arm mid-job drops the
    // completion instead of contaminating the new capture -- and the three
    // counters publish between seq toggles so the JSON writer's seqlocked
    // read never serializes a torn snapshot. No drain: the finishing thread
    // may itself be inside an observed job, so draining could self-deadlock.
    const uint64_t gen=kinematicEvalProbe.jobGeneration();
    const int64_t start=qpcNow();
    const uintptr_t result=forward(a,b,c,d);
    const int64_t elapsed=qpcNow()-start;
    if(queueArmed) {
        // Exit read after the timing stops: the measured region stays the
        // job body alone. A torn/missing exit read drops the pair.
        uint32_t queueExit=0;
        if(readQueueCount(a,&queueExit))
            kinematicEvalProbe.notePhysQueue(queueEntry,queueExit);
    }
    if(elapsed>0 && g_qpcFreq>0 && gen==kinematicEvalProbe.jobGeneration()) {
        auto* stats=kinematicEvalProbe.jobStats();
        const uint64_t ns=uint64_t(elapsed)*1000000000ull/uint64_t(g_qpcFreq);
        stats[job].seq.fetch_add(1,std::memory_order_relaxed); // odd: commit in flight
        stats[job].calls.fetch_add(1,std::memory_order_relaxed);
        stats[job].totalNs.fetch_add(ns,std::memory_order_relaxed);
        uint64_t prev=stats[job].maxNs.load(std::memory_order_relaxed);
        while(prev<ns && !stats[job].maxNs.compare_exchange_weak(prev,ns,std::memory_order_relaxed)){}
        stats[job].seq.fetch_add(1,std::memory_order_release); // even: coherent
    }
    return result;
}

#define EDVR_JOB_WRAPPER(i) \
    __declspec(noinline) uintptr_t __fastcall job##i(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) noexcept { \
        return bracket(i,a,b,c,d); \
    }
EDVR_JOB_WRAPPER(0) EDVR_JOB_WRAPPER(1) EDVR_JOB_WRAPPER(2)
EDVR_JOB_WRAPPER(3) EDVR_JOB_WRAPPER(4) EDVR_JOB_WRAPPER(5)
#undef EDVR_JOB_WRAPPER

std::mutex g_installMutex;

bool installOne(HookEntry& entry,uintptr_t base) noexcept;

// One install path for both consumers: process-lifetime hooks, QPC for the
// job brackets. Jobs and the rig-link hook stand down alone on failure (the
// eval capture is the primary evidence; CodeHook logs each reason).
bool ensureInstalled(uintptr_t base) noexcept {
    if(g_evalEntry.ready.load(std::memory_order_acquire))return true;
    LARGE_INTEGER freq{};
    if(!QueryPerformanceFrequency(&freq)||freq.QuadPart<=0)return false;
    g_qpcFreq=freq.QuadPart;
    if(!installOne(g_evalEntry,base))return false;
    for(uint32_t i=0;i<KinematicEvalProbe::kJobCount;++i) {
        if(!installOne(g_jobEntries[i],base)) {
            // A job that cannot be hooked stands down alone; the eval
            // capture is the flight's primary evidence. CodeHook has
            // already logged the reason under the job's own name.
        }
    }
    if(!installOne(g_rigEvalEntry,base)) {
        // The rig-link hook stands down alone too; riglink_checks == 0
        // with installed status then means the stand-down, and
        // CodeHook has logged the reason under kinematic-rig-eval.
    }
    return true;
}

// The executable check for the tracker path: PE timestamp/image size of the
// hash-verified build plus the evaluator's prologue (or our own patch
// already there). Mirrors KinematicEvalProbe::validateExecutableLocked
// without touching probe state -- this file's copy-culture is deliberate.
bool targetValid(uintptr_t base) noexcept {
    __try {
        uint32_t peOff=0;
        std::memcpy(&peOff,reinterpret_cast<const void*>(base+0x3C),4);
        if(peOff>0x1000)return false;
        uint32_t timestamp=0,imageSize=0;
        std::memcpy(&timestamp,reinterpret_cast<const void*>(base+peOff+8),4);
        std::memcpy(&imageSize,reinterpret_cast<const void*>(base+peOff+0x50),4);
        if(timestamp!=KinematicEvalProbe::kExpectedTimestamp ||
           imageSize!=KinematicEvalProbe::kExpectedImageSize)return false;
        // push rbx; push rbp; push rsi; push rdi; push r12; r14; r15; sub rsp,0x70
        static const uint8_t kPrologue[14]={0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,
                                            0x56,0x41,0x57,0x48,0x83,0xEC};
        uint8_t got[sizeof(kPrologue)]{};
        std::memcpy(got,reinterpret_cast<const void*>(base+KinematicEvalProbe::kEvalRva),
                    sizeof(got));
        return std::memcmp(got,kPrologue,sizeof(kPrologue))==0 ||
               kinematicEvalHooksMatch(base+KinematicEvalProbe::kEvalRva);
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

bool installOne(HookEntry& entry,uintptr_t base) noexcept {
    entry.relay=allocateRelay(base+entry.rva);
    if(!entry.relay)return false;
    buildRelay(entry.relay,&evalGate,entry.callback);
    if(!entry.hook.install(reinterpret_cast<void*>(base+entry.rva),entry.relay,nullptr,
                           entry.name,&prepareRelay,&entry)) {
        VirtualFree(entry.relay,0,MEM_RELEASE);
        entry.relay=nullptr;
        return false;
    }
    entry.ready.store(true,std::memory_order_release);
    return true;
    // Process-lifetime storage: do not free a relay or trampoline which an
    // in-flight call may already be executing. Captures only gate it.
}

bool patchIsOurs(const HookEntry& entry,uintptr_t base) noexcept {
    if(!entry.ready.load(std::memory_order_acquire) || !entry.relay)return false;
    const uintptr_t target=base+entry.rva;
    const intptr_t displacement=reinterpret_cast<intptr_t>(entry.relay)-intptr_t(target+5);
    // The evaluator's prologue past the five patch bytes, from the
    // hash-verified exe: 41 54 41 56 41 57 48 83.
    const uint8_t tail[8]={0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x83};
    uint8_t bytes[13]{};
    __try {std::memcpy(bytes,reinterpret_cast<const void*>(target),sizeof(bytes));}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    int32_t actual=0;std::memcpy(&actual,bytes+1,4);
    return bytes[0]==0xE9 && actual==displacement && std::memcmp(bytes+5,tail,sizeof(tail))==0;
}

} // namespace

bool kinematicEvalHooksMatch(uintptr_t evalTarget) noexcept {
    const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return base && evalTarget==base+KinematicEvalProbe::kEvalRva && patchIsOurs(g_evalEntry,base);
}

const char* attachKinematicEvalHooks(KinematicEvalProbe* probe) noexcept {
    if(!probe)return "install_failed";
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        auto* active=observer.load(std::memory_order_acquire);
        if(active && active!=probe)return "observer_busy";
        const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if(!base)return "identity_mismatch";
        // Validation is part of the locked install operation: an unlocked
        // pre-check raced a concurrent tracker install that had patched the
        // prologue but not yet published ready, and rejected the supported
        // executable (2026-09-20 review finding 5).
        if(!g_evalEntry.ready.load(std::memory_order_acquire) && !targetValid(base))
            return "identity_mismatch";
        if(!ensureInstalled(base))return "install_failed";
        if(!patchIsOurs(g_evalEntry,base))return "opcode_mismatch";
        observer.store(probe,std::memory_order_release);
        evalGate.store(1,std::memory_order_release);
        return "installed";
    } catch(...) {return "install_failed";}
}

// The gate is recomputed under the install mutex from BOTH consumer cells;
// every attach/detach transition holds that mutex. An unlocked check-then-set
// raced: probe detach reads trackerWanted=false, tracker attach publishes
// wanted=true/gate=1, detach then writes gate=0 and the live tracker goes
// deaf (2026-09-20 review finding 4).
void recomputeGateLocked() noexcept {
    const bool open=observer.load(std::memory_order_acquire)!=nullptr ||
                    trackerWanted.load(std::memory_order_acquire);
    evalGate.store(open?uintptr_t(1):uintptr_t(0),std::memory_order_release);
}

void detachKinematicEvalHooks(KinematicEvalProbe* probe) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        observer.compare_exchange_strong(probe,nullptr,std::memory_order_acq_rel);
        recomputeGateLocked();
    } catch(...) {}
}

void kinematicEvalSetTrackerObserver(KinematicTrackerObserverFn fn) noexcept {
    trackerObserver.store(fn,std::memory_order_release);
}

const char* kinematicEvalTrackerAttach() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if(!base)return "identity_mismatch";
        if(!g_evalEntry.ready.load(std::memory_order_acquire) && !targetValid(base))
            return "identity_mismatch";
        if(!ensureInstalled(base))return "install_failed";
        if(!patchIsOurs(g_evalEntry,base))return "opcode_mismatch";
        trackerWanted.store(true,std::memory_order_release);
        evalGate.store(1,std::memory_order_release);
        return "installed";
    } catch(...) {return "install_failed";}
}

void kinematicEvalTrackerDetach() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        trackerWanted.store(false,std::memory_order_release);
        recomputeGateLocked();
    } catch(...) {}
}

} // namespace edvr
