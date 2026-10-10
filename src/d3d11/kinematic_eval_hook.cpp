#include "kinematic_eval_hook.h"
#include "kinematic_eval_probe.h"
#include "engine_velocity_emit.h"
#include "engine_motion_cpu.h"   // the CPU instrument: EDVR's own work in every relay below is clocked per call
#include "../common/code_hook.h"
#include <windows.h>
#include <intrin.h>
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
// FUN_1442B4420 (decomp_42B4420.txt): f(param_1 rigOwner, param_2 ctx,
// param_3 passedMask, param_4, param_5, param_6) -- SIX params, the last two
// on the stack (param_6's 16 bytes are copied into every appended item).
// The callback must declare all six so the compiler reproduces the stack
// layout for the trampoline verbatim; truncating to four would shift
// param_5/param_6 and corrupt the items.
using BucketFn = uintptr_t (__fastcall*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,
                                         uintptr_t,uintptr_t);
// The direct bucket producers take the bucket as param_2: FUN_144312E00
// has four register params, FUN_14369C9C0 three. One four-param forward
// covers both -- the three-param callee never reads r9, which is volatile.
using DirectBuildFn = uintptr_t (__fastcall*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t);

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

// The relay gate, distinct from observer: non-zero while ANY consumer wants
// eval callbacks -- the probe while attached (eye-dump captures), the emit
// bracket while fix.temporal_aa is on (no dump involved), the scheduler stack
// probe while armed, or the cull gate probe while its capture is armed.
// observer stays the probe's own cell; the relays gate on evalGate.
alignas(8) std::atomic<uintptr_t> evalGate{0};
static_assert(decltype(evalGate)::is_always_lock_free,
              "The x64 relay reads the aligned atomic pointer directly.");
// Engine-record velocity's own want on the hook set (the 2026-09-23
// performance review, item 1): the emit bracket rides the direct-producer
// relay, which gates on evalGate, so the emit holds the gate open itself.
std::atomic<bool> emitWanted{false};
// Engine-record velocity's emit observer (direct producer 0's post-forward).
alignas(8) std::atomic<EngineEmitObserverFn> emitObserver{nullptr};
alignas(8) std::atomic<EnginePrimaryEmitObserverFn> primaryEmitObserver{nullptr};
alignas(8) std::atomic<uintptr_t> primaryGate{0};
std::atomic<uintptr_t> g_primaryReturn{0};
std::atomic<const char*> g_primaryStatus{"not requested"};
std::atomic<uint64_t> g_primaryCalls{0},g_primaryUnowned{0};
alignas(8) std::atomic<EnginePoolCopyObserverFn> poolCopyObserver{nullptr};
std::atomic<const char*> g_poolCopyStatus{"not requested"};
alignas(8) std::atomic<EngineMergeBeginFn> mergeBeginObserver{nullptr};
alignas(8) std::atomic<EngineMergeEndFn> mergeEndObserver{nullptr};
std::atomic<const char*> g_mergeStatus{"not requested"};
alignas(8) std::atomic<EngineClearObserverFn> clearObserver{nullptr};
std::atomic<const char*> g_clearStatus{"not requested"};
// The scheduler stack probe's want: its targets 0/1 are the job bodies
// themselves, already patched by this file, so it observes through the
// job-0/1 relays and holds this gate open while armed.
std::atomic<bool> schedulerWanted{false};
// The cull gate probe's want (advanced.cull_gate_capture): observes the
// evaluator (the traversal's per-view gate) after its forward and the
// bucket bracket (the draw-item builder) before its forward, for the few
// frames of an armed eye run.
std::atomic<bool> gateProbeWanted{false};
alignas(8) std::atomic<GateProbeGateFn> gateProbeGate{nullptr};
alignas(8) std::atomic<GateProbeBuilderFn> gateProbeBuilder{nullptr};
alignas(8) std::atomic<GateProbePartFn> gateProbePart{nullptr};
// FUN_1442B3FC0's relay gate: a cell of its own, not evalGate, so the
// per-part patch (installed only for the gate probe) costs the evalGate
// consumers nothing. Open only while the probe is attached AND the patch is
// verified ours.
alignas(8) std::atomic<uintptr_t> partGate{0};
static_assert(decltype(partGate)::is_always_lock_free,
              "The x64 relay reads the aligned atomic cell directly.");
// The builder call's return address for the part test (base+0x42B4B96), set
// before the part patch goes in; the observer compares _ReturnAddress().
std::atomic<uintptr_t> g_partReturn{0};
std::atomic<const char*> g_partStatus{"not requested"};

// The probe is a process-lifetime global (kinematicEvalProbe), so a bracket
// that loaded the pointer before a detach remains safe while it finishes;
// detach only stops NEW callbacks. No in-flight drain is needed.
int64_t g_qpcFreq=0;

struct HookEntry {
    const char* name;
    uintptr_t rva;
    void* callback;
    const void* gate=nullptr;   // the relay's gate cell; null = evalGate
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
// lifecycle evidence the node capture respected (kinematic arc, flight
// 193356: append-only within a run, drained entirely between runs).
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

// The queue's node-pointer array base is descriptor +0x10 (param_1[2];
// decomp_432B2A0 lines 257-268: FUN_144899f50(param_1[2] + idx*8, batch,
// count*8)). With the lifecycle decoded, the slice [entry, exit) at job-2
// exit is this run's appended nodes, intact. Reads the newest
// min(delta, cap) entries; the clamped excess counts as overflow. A reset
// (exit < entry) captures only the post-reset residue [0, exit) -- the
// pre-reset slice was drained mid-run and is unknowable (phys_queue.resets
// flags the event). Same __try discipline as readQueueCount: a fault or a
// wild counter drops the run's capture, never the flight.
bool readQueueNodes(uintptr_t descriptor,uint32_t entry,uint32_t exit,
                    uint64_t* out,uint32_t cap,uint32_t* kept,uint32_t* overflow) noexcept {
    __try {
        uintptr_t base=0;
        std::memcpy(&base,reinterpret_cast<const void*>(descriptor+0x10),8);
        if(!base)return false;
        const uint32_t first=exit<entry?0u:entry;
        const uint32_t delta=exit-first;
        // Append-only between drains (max_delta 133 on flight 193356): a
        // large exit means a torn read, not a busy run -- drop it.
        if(delta==0||exit>0x100000u)return false;
        uint32_t start=first;
        uint32_t excess=0;
        if(delta>cap){start=exit-cap;excess=delta-cap;}
        for(uint32_t i=start;i<exit;++i)
            std::memcpy(&out[i-start],reinterpret_cast<const void*>(base+uintptr_t(i)*8),8);
        *kept=exit-start;
        *overflow=excess;
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
__declspec(noinline) uintptr_t __fastcall bucketBuildObserved(uintptr_t,uintptr_t,uintptr_t,
                                                              uintptr_t,uintptr_t,uintptr_t) noexcept;
__declspec(noinline) uintptr_t __fastcall primaryBuildObserved(uintptr_t,uintptr_t,uintptr_t,
                                                               uintptr_t,uintptr_t,uintptr_t) noexcept;
__declspec(noinline) void __fastcall poolCopyObserved(uintptr_t) noexcept;
__declspec(noinline) void __fastcall poolMergeObserved(uintptr_t,uintptr_t) noexcept;
__declspec(noinline) void __fastcall poolClearObserved(uintptr_t,uintptr_t) noexcept;
__declspec(noinline) uintptr_t __fastcall partTestObserved(uintptr_t,uintptr_t,uintptr_t) noexcept;
#define EDVR_DIRECT_PROTO(i) \
    __declspec(noinline) uintptr_t __fastcall directBuild##i(uintptr_t,uintptr_t,uintptr_t,uintptr_t) noexcept;
EDVR_DIRECT_PROTO(0) EDVR_DIRECT_PROTO(1)
#undef EDVR_DIRECT_PROTO
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
HookEntry g_bucketEntry{"kinematic-bucket-build",KinematicEvalProbe::kBucketBuildRva,
                        reinterpret_cast<void*>(&bucketBuildObserved)};
HookEntry g_primaryEntry{"engine-primary-rigid-emit",0x42B4130u,
                         reinterpret_cast<void*>(&primaryBuildObserved),&primaryGate};
HookEntry g_poolCopyEntry{"engine-primary-pool-copy",0x4C81BE0u,
                          reinterpret_cast<void*>(&poolCopyObserved),&primaryGate};
HookEntry g_poolMergeEntry{"engine-primary-list-merge",0x434E740u,
                           reinterpret_cast<void*>(&poolMergeObserved),&primaryGate};
HookEntry g_poolClearEntry{"engine-primary-list-clear",0x36819D0u,
                           reinterpret_cast<void*>(&poolClearObserved),&primaryGate};
HookEntry g_directEntries[KinematicEvalProbe::kDirectProducerCount]={
    {"kinematic-build-144312e00",KinematicEvalProbe::kDirectBuildRvas[0],
     reinterpret_cast<void*>(&directBuild0)},
    {"kinematic-build-14369c9c0",KinematicEvalProbe::kDirectBuildRvas[1],
     reinterpret_cast<void*>(&directBuild1)},
};

// --- The builder's per-part test (FUN_1442B3FC0), the gate probe's third site
// decomp_42B3FC0.txt: f(param_1 = the builder's six-pointer block at its
// rbp+0x70, param_2 = {u32 LOD, u8 passed}, param_3 = the view); frustum
// (FUN_1404F4E10 at 0x1442B4066) and screen-size/LOD on the part's own sphere.
// Its only caller is the builder's sub-item loop (0x1442B4B91; the other
// xref, 0x1462B1388, is outside any function). The observer reads the
// builder's frame around that call, so the signature below is not only the
// prologue: it is every instruction the frame offsets were read from, in the
// hash-verified exe (build 332841). Any mismatch stands this hook down alone.
constexpr uintptr_t kPartTestRva=0x42B3FC0u;
constexpr uintptr_t kPartCallReturnRva=0x42B4B96u;   // after the call at 0x1442B4B91
// mov [rsp+10h],rbx; mov [rsp+18h],rsi; push rdi; sub rsp,50h; mov rax,[rcx]; mov rdi,r8
constexpr uint8_t kPartPrologue[21]={0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,
                                     0x48,0x83,0xEC,0x50,0x48,0x8B,0x01,0x49,0x8B,0xF8};
struct CodeBytes { uintptr_t rva; uint8_t n; uint8_t b[17]; const char* why; };
constexpr CodeBytes kPartFrame[]={
    {0x42B4429u,7,{0x49,0x8D,0xAB,0x08,0xFC,0xFF,0xFF},
     "builder frame mismatch at RVA 0x42B4429 (lea rbp,[r11-3F8h]: the frame base)"},
    {0x42B4455u,4,{0x48,0x89,0x55,0xB8},
     "builder frame mismatch at RVA 0x42B4455 (mov [rbp-48h],rdx: the render context)"},
    {0x42B445Cu,4,{0x48,0x89,0x4D,0x98},
     "builder frame mismatch at RVA 0x42B445C (mov [rbp-68h],rcx: the pose context)"},
    {0x42B4868u,4,{0x48,0x89,0x75,0xA8},
     "builder frame mismatch at RVA 0x42B4868 (mov [rbp-58h],rsi: the instance entry)"},
    {0x42B48B8u,4,{0x48,0x89,0x4D,0xC0},
     "builder frame mismatch at RVA 0x42B48B8 (mov [rbp-40h],rcx: the entry's model)"},
    {0x42B48CDu,4,{0x48,0x89,0x45,0xB0},
     "builder frame mismatch at RVA 0x42B48CD (mov [rbp-50h],rax: the model's +0x40)"},
    {0x42B498Bu,4,{0x48,0x89,0x5D,0x90},
     "builder frame mismatch at RVA 0x42B498B (mov [rbp-70h],rbx: the first sub-item)"},
    {0x42B4FEBu,4,{0x48,0x83,0xC3,0x20},
     "builder frame mismatch at RVA 0x42B4FEB (add rbx,20h: the next sub-item)"},
    {0x42B4FF7u,4,{0x48,0x89,0x5D,0x90},
     "builder frame mismatch at RVA 0x42B4FF7 (mov [rbp-70h],rbx: the sub-item stored back)"},
    {0x42B49E3u,4,{0x48,0x8D,0x45,0x50},
     "builder frame mismatch at RVA 0x42B49E3 (lea rax,[rbp+50h]: the world centre)"},
    {0x42B49F4u,4,{0x48,0x89,0x45,0x70},
     "builder frame mismatch at RVA 0x42B49F4 (mov [rbp+70h],rax: param_1[0])"},
    {0x42B4B1Du,4,{0x48,0x8D,0x45,0x60},
     "builder frame mismatch at RVA 0x42B4B1D (lea rax,[rbp+60h]: the model's +0x10 copy)"},
    {0x42B4B21u,4,{0x48,0x89,0x45,0x78},
     "builder frame mismatch at RVA 0x42B4B21 (mov [rbp+78h],rax: param_1[1])"},
    {0x42B4B67u,7,{0x48,0x8D,0x95,0xB0,0x00,0x00,0x00},
     "builder call-site mismatch at RVA 0x42B4B67 (lea rdx,[rbp+0B0h]: param_2)"},
    {0x42B4B85u,17,{0x4C,0x8B,0xC0,0x48,0x89,0x44,0x24,0x30,0x48,0x8D,0x4D,0x70,0xE8,0x2A,0xF4,0xFF,0xFF},
     "builder call-site mismatch at RVA 0x42B4B85 (mov r8,rax; mov [rsp+30h],rax; lea rcx,[rbp+70h]; "
     "call FUN_1442B3FC0)"},
};
HookEntry g_partEntry{"cull-gate-part-test",kPartTestRva,reinterpret_cast<void*>(&partTestObserved),&partGate};

// Per-thread mask of the job brackets currently on the stack (1u<<jobId),
// maintained by bracket() and read by evalObserved: attributes every eval
// observation to the engine job that scheduled it. Nesting-safe (save/
// restore), zero when no observed job is running.
thread_local uint32_t t_jobMask=0;
// The gate probe's row for the builder call on this thread's stack (set by
// bucketBuildObserved around its forward, save/restore), read by the part
// test's observer: joins each FUN_1442B3FC0 verdict to its builder row.
thread_local uint32_t t_builderRow=kGateProbeNoRow;
thread_local engine_velocity_emit::PrimaryIdentity t_primaryIdentity;

// The two evaluator relays are pass-throughs unless a diagnostic is attached
// (two loads and a call), cheaper than one clock read, so every call is COUNTED
// and only the probe branches, which have real work, are clocked
// (engine_motion_cpu.h, Part kEval; the count bounds the cost).
__declspec(noinline) uintptr_t __fastcall evalObserved(uintptr_t descriptor,uintptr_t param2,
                                                       uintptr_t renderRecord) noexcept {
    emcpu::count(emcpu::kEval);
    const uint32_t jobMask=t_jobMask;
    KinematicEvalProbe* probe=observer.load(std::memory_order_acquire);
    if(probe) {
        emcpu::Scope clocked(emcpu::kEval,false);
        probe->observe(descriptor,renderRecord,jobMask); // observe() gates on active()
    }
    const auto forward=reinterpret_cast<EvalFn>(g_evalEntry.forward.load(std::memory_order_acquire));
    const uintptr_t result=forward(descriptor,param2,renderRecord);
    // The cull gate probe reads the verdict the call just wrote (param2:
    // u32 LOD index, u8 passed); renderRecord is the VIEW (design doc §9).
    const auto gateProbe=gateProbeGate.load(std::memory_order_acquire);
    if(gateProbe) {
        emcpu::Scope clocked(emcpu::kEval,false);
        gateProbe(descriptor,param2,renderRecord);
    }
    return result;
}

__declspec(noinline) uintptr_t __fastcall rigEvalObserved(uintptr_t rig,uintptr_t poseCtx) noexcept {
    emcpu::count(emcpu::kEval);
    KinematicEvalProbe* probe=observer.load(std::memory_order_acquire);
    if(probe) {
        emcpu::Scope clocked(emcpu::kEval,false);
        probe->noteRigLink(rig,poseCtx); // noteRigLink() gates on active()
    }
    const auto forward=reinterpret_cast<RigEvalFn>(g_rigEvalEntry.forward.load(std::memory_order_acquire));
    return forward(rig,poseCtx);
}

uintptr_t __fastcall bracket(uint32_t job,uintptr_t a,uintptr_t b,
                             uintptr_t c,uintptr_t d) noexcept {
    const auto forward=reinterpret_cast<JobFn>(g_jobEntries[job].forward.load(std::memory_order_acquire));
    if(!forward)return 0; // this job stood down at install; the relay is unreachable then
    // EDVR's own work in this bracket, per call (engine_motion_cpu.h): everything
    // below except the job body itself, which the pause around the forward leaves out.
    emcpu::Scope engineMotion(emcpu::kJobs);
    // The scheduler stack probe's targets 0/1 ARE these job bodies (their
    // RVAs already carry this hook's patch), so their capture rides here,
    // before the timed region and before the forward -- the pre-forward
    // point a dedicated hook would sit. The address handed over is this
    // frame's return-address slot: one frame deeper than the target's
    // entry RSP, so this wrapper's own return address (EDVR code, outside
    // the probe's image range) scans as a filtered miss and the engine's
    // return address lands at index 0 of the collected stack.
    if(job<2)schedulerStackNoteJobEntry(job,
        reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()));
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
    // wants. The probe global is process-lifetime, so a flight with the
    // probe never attached (fix.temporal_aa on) times too. jobs[] now
    // accumulates per-session, not per capture window. Ownership capture
    // stays capture-gated: observe()/noteOwnership keep their active() gates.
    if(probe && probe->active())probe->noteOwnership(job,a);
    // Job 2's dirty-queue append counter, read at entry (exit read after
    // the body below). Outside the timed region: L1 measures the job body,
    // not this probe. Goes to the process-lifetime global, not the observer
    // pointer, so flights without an eye run capture it too.
    uint32_t queueEntry=0;
    const bool queueArmed=(job==2)&&readQueueCount(a,&queueEntry);
    // Capture boundary (2026-09-20 review finding 8): samples commit only to
    // the generation they started in -- a reset/re-arm mid-job drops the
    // completion instead of contaminating the new capture -- and the three
    // counters publish between seq toggles so the JSON writer's seqlocked
    // read never serializes a torn snapshot. No drain: the finishing thread
    // may itself be inside an observed job, so draining could self-deadlock.
    const uint64_t gen=kinematicEvalProbe.jobGeneration();
    // The bracket takes its own two clock readings (the job statistics below
    // need them on every call) and hands the same two to the instrument, so a
    // sampled frame adds only the instrument's enter and its leave here, and a
    // frame it does not clock adds nothing (engine_motion_cpu.h: pauseAt and
    // resumeAt read no clock). The timed region is unchanged.
    const int64_t start=qpcNow();
    engineMotion.pauseAt(start);
    const uintptr_t result=forward(a,b,c,d);
    const int64_t stop=qpcNow();
    engineMotion.resumeAt(stop);
    const int64_t elapsed=stop-start;
    if(queueArmed) {
        // Exit read after the timing stops: the measured region stays the
        // job body alone. A torn/missing exit read drops the pair.
        uint32_t queueExit=0;
        if(readQueueCount(a,&queueExit)) {
            kinematicEvalProbe.notePhysQueue(queueEntry,queueExit);
            // The node capture rides the same exit read (sanctioned
            // 2026-09-20 20:27): [entry, exit) is this run's intact
            // appended slice. Zero-delta runs have nothing to walk.
            if(queueExit!=queueEntry) {
                uint64_t nodes[KinematicEvalProbe::kPhysNodeCap];
                uint32_t kept=0,nodeOverflow=0;
                if(readQueueNodes(a,queueEntry,queueExit,nodes,
                                  KinematicEvalProbe::kPhysNodeCap,&kept,&nodeOverflow))
                    kinematicEvalProbe.notePhysNodes(nodes,kept,nodeOverflow);
            }
        }
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

// --- Draw-item-builder bracket (FUN_1442B4420, the census join) ------------
// One call builds the draw items for one rig owner: it walks the owner's
// instance entries (count u64 @ param_1+0x48, array @ +0x50, stride 0x58),
// resolves each entry's model (entry+0x0) to its bucket (*(model+0x20)) and
// appends 0x150-byte items to the bucket's list, counting every append at
// bucket+0x2A4 (decomp_42B4420 lines 377-414, 661-735; the entry-array
// offsets are byte offsets, param_1 being float* in the decompile). Reading
// the counters at entry and exit attributes appends to this call; summed
// per session they are the engine-side item production the D3D11 draw
// census joins against. Both append paths are covered: the inline site and
// FUN_1442B4130 run inside the forwarded call.
constexpr uint32_t kBucketWalkCap=64u;
struct BucketSnap { uintptr_t bucket; int32_t start; };

uint32_t collectBuckets(uintptr_t rigOwner,BucketSnap* out,uint32_t cap,
                        uint32_t* flags) noexcept {
    __try {
        uint64_t count=0;
        std::memcpy(&count,reinterpret_cast<const void*>(rigOwner+0x48),8);
        uintptr_t base=0;
        std::memcpy(&base,reinterpret_cast<const void*>(rigOwner+0x50),8);
        if(count==0)return 0;                    // legitimately empty rig
        if(count>4096 || !base){*flags|=KinematicEvalProbe::kBucketFlagEntryWild;return 0;}
        uint32_t n=0;
        for(uint64_t i=0;i<count;++i) {
            uintptr_t model=0;
            std::memcpy(&model,reinterpret_cast<const void*>(base+i*0x58),8);
            if(!model)continue;
            uintptr_t bucket=0;
            std::memcpy(&bucket,reinterpret_cast<const void*>(model+0x20),8);
            if(!bucket)continue;
            bool dup=false;
            for(uint32_t k=0;k<n;++k)if(out[k].bucket==bucket){dup=true;break;}
            if(dup)continue;
            if(n==cap){*flags|=KinematicEvalProbe::kBucketFlagOverflow;break;}
            int32_t start=0;
            std::memcpy(&start,reinterpret_cast<const void*>(bucket+0x2A4),4);
            out[n].bucket=bucket;out[n].start=start;++n;
        }
        return n;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        *flags|=KinematicEvalProbe::kBucketFlagEntryWild;
        return 0;
    }
}

__declspec(noinline) uintptr_t __fastcall bucketBuildObserved(uintptr_t a,uintptr_t b,
                                                              uintptr_t c,uintptr_t d,
                                                              uintptr_t e,uintptr_t f) noexcept {
    const auto forward=reinterpret_cast<BucketFn>(g_bucketEntry.forward.load(std::memory_order_acquire));
    if(!forward)return 0; // stood down at install; the relay is unreachable then
    // EDVR's own work in this bracket (engine_motion_cpu.h): a POD token, since
    // the function has __try; paused around the builder, left at each return.
    emcpu::Token engineMotion;
    emcpu::enter(engineMotion,emcpu::kBuilder);
    // The cull gate probe sees the builder's inputs as the builder will:
    // before the forward (a = pose context, b = render context, c = the
    // collection's active view mask, d = rec+0x210). The row it keeps is
    // this thread's for the forward: the per-part tests inside carry it.
    const auto builderProbe=gateProbeBuilder.load(std::memory_order_acquire);
    const uint32_t outerRow=t_builderRow;
    t_builderRow=builderProbe?builderProbe(a,b,c,d):kGateProbeNoRow;
    const auto outerIdentity=t_primaryIdentity;
    t_primaryIdentity={};
    if(primaryEmitObserver.load(std::memory_order_acquire))
        engine_velocity_emit::primaryIdentity(a,d,t_primaryIdentity);
    BucketSnap snaps[kBucketWalkCap];
    uint32_t flags=0;
    const uint32_t n=collectBuckets(a,snaps,kBucketWalkCap,&flags);
    uintptr_t result=0;
    emcpu::pause(engineMotion);
    __try { result=forward(a,b,c,d,e,f); }
    __finally { t_builderRow=outerRow; t_primaryIdentity=outerIdentity; }
    emcpu::resume(engineMotion);
    if(n) {
        uint64_t items=0;
        uint32_t neg=0;
        __try {
            for(uint32_t k=0;k<n;++k) {
                int32_t end=0;
                std::memcpy(&end,reinterpret_cast<const void*>(snaps[k].bucket+0x2A4),4);
                const int64_t delta=static_cast<int64_t>(end)-static_cast<int64_t>(snaps[k].start);
                if(delta>0)items+=static_cast<uint64_t>(delta);
                else if(delta<0)++neg; // drained mid-call: the residue is unknowable
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            items=0;neg=0;
            flags|=KinematicEvalProbe::kBucketFlagExitFault;
        }
        kinematicEvalProbe.noteBucketBuild(n,items,neg,flags);
    } else {
        kinematicEvalProbe.noteBucketBuild(0,0,0,flags);
    }
    emcpu::leave(engineMotion);
    return result;
}

// Six arguments are required: the original reads the mask and optional
// four-word tail on the stack. The relay jumps here, preserving the native
// caller's return address; only 42B4420's verified primary call is eligible.
__declspec(noinline) uintptr_t __fastcall primaryBuildObserved(uintptr_t model,uintptr_t lod,
    uintptr_t position,uintptr_t quaternion,uintptr_t mask,uintptr_t tail) noexcept {
    const auto forward=reinterpret_cast<BucketFn>(g_primaryEntry.forward.load(std::memory_order_acquire));
    if(!forward)return 0;
    emcpu::Token engineMotion;   // EDVR's own work only (engine_motion_cpu.h); a POD token, the function has __try
    emcpu::enter(engineMotion,emcpu::kRigid);
    g_primaryCalls.fetch_add(1,std::memory_order_relaxed);
    const auto identity=t_primaryIdentity;
    const auto observe=primaryEmitObserver.load(std::memory_order_acquire);
    const bool eligible=observe && identity.record &&
        reinterpret_cast<uintptr_t>(_ReturnAddress())==g_primaryReturn.load(std::memory_order_relaxed);
    uintptr_t owner=0;
    int32_t before=0,after=0;
    const bool counted=observe && engine_velocity_emit::read(model+0x20,&owner,sizeof(owner)) && owner &&
        engine_velocity_emit::read(owner+engine_velocity_emit::kOwnerCount,&before,sizeof(before));
    uintptr_t result=0;
    bool returned=false;
    emcpu::pause(engineMotion);
    __try { result=forward(model,lod,position,quaternion,mask,tail); returned=true; }
    __finally {
        emcpu::resume(engineMotion);
        const bool complete=returned && counted &&
            engine_velocity_emit::read(owner+engine_velocity_emit::kOwnerCount,&after,sizeof(after));
        // Every append invalidates its prior address claim, including native
        // callers outside the certified outer frame. Only eligible identity
        // can create a replacement claim.
        if(observe)observe(eligible?identity:engine_velocity_emit::PrimaryIdentity{},owner,model+0x40,position,quaternion,
                           complete?before:0,complete?after:0);
        if(!complete || !eligible)g_primaryUnowned.fetch_add(1,std::memory_order_relaxed);
        emcpu::leave(engineMotion);
    }
    return result;
}

// Authoritative whole-record upload, one task argument. Every destination
// range is reported, including unknown sources, so overwritten certificates
// cannot survive merely because the replacement did not receive a pose.
__declspec(noinline) void __fastcall poolCopyObserved(uintptr_t task) noexcept {
    const auto forward=reinterpret_cast<void(__fastcall*)(uintptr_t)>(g_poolCopyEntry.forward.load(std::memory_order_acquire));
    if(!forward)return;
    emcpu::Token engineMotion;   // EDVR's own work only (engine_motion_cpu.h); a POD token, the function has __try
    emcpu::enter(engineMotion,emcpu::kCopier);
    const auto observe=poolCopyObserver.load(std::memory_order_acquire);
    namespace ev=engine_velocity_emit;
    uintptr_t holder=0,buffer=0,descriptor=0,mapped=0;
    uint32_t stride=0;uint64_t offset=0;
    const bool metadata=ev::read(task+0xD0,&holder,8) && holder && ev::read(holder+0x10,&buffer,8) && buffer &&
       ev::read(buffer+0x180,&mapped,8) && mapped && ev::read(buffer+0x100,&descriptor,8) && descriptor &&
       ev::read(descriptor+0x10,&stride,4) && stride==336 && ev::read(task+0xC0,&offset,8);
    bool returned=false;
    emcpu::pause(engineMotion);
    __try {forward(task);returned=true;}
    __finally {emcpu::resume(engineMotion);if(observe && (!returned || !metadata))observe(mapped,stride,0,0,UINT32_MAX);}
    if(!observe || !metadata){emcpu::leave(engineMotion);return;}
    for(unsigned group=0;group<8;++group) {
        uint64_t count=0;uintptr_t entries=0;
        const uintptr_t list=group==0?task+8:task+0x20+uintptr_t(group-1)*0x18;
        const uint32_t entryStride=group==0?0x48u:0x50u;
        if(!ev::read(list,&count,8) || !ev::read(list+8,&entries,8) || count>65536 ||
           (count && (!entries || entries>UINTPTR_MAX-count*entryStride))) {
            observe(mapped,stride,0,0,UINT32_MAX);emcpu::leave(engineMotion);return;
        }
        for(uint64_t i=0;i<count;++i) {
            const uintptr_t entry=entries+i*entryStride;
            uintptr_t source=0;uint32_t slot=0,records=0;
            if(!ev::read(entry+8,&source,8) || !ev::read(entry+0x38,&slot,4) ||
               !ev::read(entry+0x3C,&records,4) || offset>UINT64_MAX-slot) {
                observe(mapped,stride,0,0,UINT32_MAX);emcpu::leave(engineMotion);return;
            }
            observe(mapped,stride,source,offset+slot,records);
        }
    }
    emcpu::leave(engineMotion);
}

// Exact list relocation boundary. The plan stages claims before freed source
// nodes can be recycled; a native unwind discards the staged epoch.
__declspec(noinline) void __fastcall poolMergeObserved(uintptr_t destination,uintptr_t source) noexcept {
    const auto forward=reinterpret_cast<void(__fastcall*)(uintptr_t,uintptr_t)>(g_poolMergeEntry.forward.load(std::memory_order_acquire));
    if(!forward)return;
    emcpu::Token engineMotion;   // EDVR's own work only (engine_motion_cpu.h); a POD token, the function has __try
    emcpu::enter(engineMotion,emcpu::kMerge);
    const auto begin=mergeBeginObserver.load(std::memory_order_acquire);
    const auto end=mergeEndObserver.load(std::memory_order_acquire);
    void* plan=begin && end?begin(destination,source):nullptr;
    bool completed=false;
    emcpu::pause(engineMotion);
    __try {forward(destination,source);completed=true;}
    __finally {emcpu::resume(engineMotion);if(begin && end)end(plan,completed);emcpu::leave(engineMotion);}
}
// Typed 336-byte dictionary clear frees nodes outside the merge/copy path.
// Revoke exact source addresses before allocator recycling; preserve claims
// in unrelated source dictionaries awaiting the destination merge.
__declspec(noinline) void __fastcall poolClearObserved(uintptr_t dictionary,uintptr_t allocatorOwner) noexcept {
    const auto forward=reinterpret_cast<void(__fastcall*)(uintptr_t,uintptr_t)>(g_poolClearEntry.forward.load(std::memory_order_acquire));
    if(!forward)return;
    const auto observe=clearObserver.load(std::memory_order_acquire);
    if(observe) {
        emcpu::Scope engineMotion(emcpu::kClear);   // the observer only; the clear itself is the game's
        observe(dictionary);
    }
    forward(dictionary,allocatorOwner);
}

// --- The per-part test's observer (FUN_1442B3FC0) ---------------------------
// Reached only through its own relay (partGate: the gate probe attached and
// the patch verified ours). Forwards first; the probe then reads the verdict
// the call just wrote (param_2) and the builder's frame around it, read-only:
// the verdict is never written.
// The return address names the caller: the relay JUMPS here, so the slot
// holds the builder's return address (base+0x42B4B96) when the call came
// from its sub-item loop.
__declspec(noinline) uintptr_t __fastcall partTestObserved(uintptr_t items,uintptr_t out,
                                                           uintptr_t view) noexcept {
    const auto forward=reinterpret_cast<EvalFn>(g_partEntry.forward.load(std::memory_order_acquire));
    if(!forward)return 0; // stood down at install; the relay is unreachable then
    emcpu::Scope engineMotion(emcpu::kBuilder);   // EDVR's own work only (engine_motion_cpu.h)
    engineMotion.pause();
    const uintptr_t result=forward(items,out,view);
    engineMotion.resume();
    const auto part=gateProbePart.load(std::memory_order_acquire);
    if(part) {
        const uintptr_t from=reinterpret_cast<uintptr_t>(_ReturnAddress());
        part(items,out,view,t_builderRow,from==g_partReturn.load(std::memory_order_relaxed));
    }
    return result;
}

// --- Direct-producer brackets (bucket = param_2) -----------------------------
// Same counter-delta discipline as the 42B4420 bracket without the entry
// walk: param_2 IS the bucket (decomp_4312E00 line 250, decomp_369C9C0 line
// 318 -- both INC param_2+0x2A4 per appended item). A fault on either read
// drops the call's items, never the flight.
uintptr_t __fastcall directBracket(uint32_t producer,uintptr_t a,uintptr_t b,
                                   uintptr_t c,uintptr_t d) noexcept {
    const auto forward=reinterpret_cast<DirectBuildFn>(
        g_directEntries[producer].forward.load(std::memory_order_acquire));
    if(!forward)return 0; // stood down at install; the relay is unreachable then
    // EDVR's own work around the forward, every call (engine_motion_cpu.h):
    // producer 0 is the emit hook (its census reads and the emit observer),
    // producer 1 the second direct producer (the census reads only). A POD
    // token, the function has __try.
    emcpu::Token engineMotion;
    emcpu::enter(engineMotion,producer==0?emcpu::kEmit:emcpu::kBuilder);
    int32_t start=0;
    bool fault=false;
    __try {
        std::memcpy(&start,reinterpret_cast<const void*>(b+0x2A4),4);
    } __except(EXCEPTION_EXECUTE_HANDLER) {fault=true;}
    emcpu::pause(engineMotion);
    const uintptr_t result=forward(a,b,c,d);
    emcpu::resume(engineMotion);
    uint64_t items=0;
    uint32_t neg=0;
    int32_t end=0;
    if(!fault) {
        __try {
            std::memcpy(&end,reinterpret_cast<const void*>(b+0x2A4),4);
            const int64_t delta=static_cast<int64_t>(end)-static_cast<int64_t>(start);
            if(delta>0)items=static_cast<uint64_t>(delta);
            else if(delta<0)neg=1; // drained mid-call: residue unknowable
        } __except(EXCEPTION_EXECUTE_HANDLER) {items=0;neg=0;fault=true;}
    }
    kinematicEvalProbe.noteDirectBuild(producer,items,neg,fault);
    // Engine-record velocity: FUN_144312E00 (producer 0) only, after the
    // forward, while the records it appended are still this job's alone.
    if(producer==0 && !fault) {
        const auto emit=emitObserver.load(std::memory_order_acquire);
        if(emit)emit(a,b,start,end);
    }
    emcpu::leave(engineMotion);
    return result;
}

#define EDVR_DIRECT_WRAPPER(i) \
    __declspec(noinline) uintptr_t __fastcall directBuild##i(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) noexcept { \
        return directBracket(i,a,b,c,d); \
    }
EDVR_DIRECT_WRAPPER(0) EDVR_DIRECT_WRAPPER(1)
#undef EDVR_DIRECT_WRAPPER

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
    if(!installOne(g_bucketEntry,base)) {
        // The bucket-build bracket stands down alone as well:
        // bucket_items.calls == 0 with installed status is the stand-down
        // signature, and CodeHook has logged the reason under
        // kinematic-bucket-build. (Job 3's thunk refused this way before.)
    }
    for(uint32_t i=0;i<KinematicEvalProbe::kDirectProducerCount;++i) {
        if(!installOne(g_directEntries[i],base)) {
            // Same stand-down-alone rule; bucket_items_direct[i].calls == 0
            // with installed status is the signature, and CodeHook logs
            // under the site's own name (kinematic-build-144312e00 /
            // kinematic-build-14369c9c0).
        }
    }
    return true;
}

// The executable check for the shared install: PE timestamp/image size of the
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
    buildRelay(entry.relay,entry.gate?entry.gate:static_cast<const void*>(&evalGate),entry.callback);
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

// FUN_1442B3FC0's build-keyed signature: null when the image is build
// 332841 and every byte the part observer's frame offsets rest on is in
// place (the prologue, or our own patch followed by the prologue's rest);
// otherwise the reason the hook stands down.
const char* partTestSignature(uintptr_t base,const HookEntry& entry) noexcept {
    __try {
        uint32_t peOff=0;
        std::memcpy(&peOff,reinterpret_cast<const void*>(base+0x3C),4);
        if(peOff>0x1000)return "not build 332841 (no PE header)";
        uint32_t timestamp=0,imageSize=0;
        std::memcpy(&timestamp,reinterpret_cast<const void*>(base+peOff+8),4);
        std::memcpy(&imageSize,reinterpret_cast<const void*>(base+peOff+0x50),4);
        if(timestamp!=KinematicEvalProbe::kExpectedTimestamp ||
           imageSize!=KinematicEvalProbe::kExpectedImageSize)return "not build 332841 (PE timestamp/size)";
        uint8_t got[sizeof(kPartPrologue)]{};
        std::memcpy(got,reinterpret_cast<const void*>(base+kPartTestRva),sizeof(got));
        if(std::memcmp(got,kPartPrologue,sizeof(got))!=0) {
            // Our own patch from an earlier attach: E9 to our relay, then the rest.
            int32_t actual=0;std::memcpy(&actual,got+1,4);
            const intptr_t displacement=entry.relay
                ?reinterpret_cast<intptr_t>(entry.relay)-intptr_t(base+kPartTestRva+5):0;
            if(!(entry.relay && got[0]==0xE9 && actual==displacement &&
                 std::memcmp(got+5,kPartPrologue+5,sizeof(got)-5)==0))
                return "prologue mismatch at RVA 0x42B3FC0";
        }
        for(const CodeBytes& c:kPartFrame)
            if(std::memcmp(reinterpret_cast<const void*>(base+c.rva),c.b,c.n)!=0)return c.why;
        return nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return "unreadable executable image";}
}

bool partPatchIsOurs(uintptr_t base) noexcept {
    if(!g_partEntry.ready.load(std::memory_order_acquire) || !g_partEntry.relay)return false;
    const uintptr_t target=base+kPartTestRva;
    const intptr_t displacement=reinterpret_cast<intptr_t>(g_partEntry.relay)-intptr_t(target+5);
    uint8_t bytes[sizeof(kPartPrologue)]{};
    __try {std::memcpy(bytes,reinterpret_cast<const void*>(target),sizeof(bytes));}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    int32_t actual=0;std::memcpy(&actual,bytes+1,4);
    return bytes[0]==0xE9 && actual==displacement &&
           std::memcmp(bytes+5,kPartPrologue+5,sizeof(bytes)-5)==0;
}

// Verified build 332841: primary ABI/prologue, its sole builder callsite,
// dictionary key/owner loads, and terminal append count. No guessed frame.
const char* primarySignature(uintptr_t base) noexcept {
    constexpr uint8_t prologue[]={0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x20,
        0x56,0x57,0x41,0x56,0x48,0x81,0xEC,0x90,0x01,0x00,0x00};
    constexpr uint8_t call[]={0xE8,0xE8,0xF8,0xFF,0xFF};
    constexpr uint8_t append[]={0x48,0xFF,0x42,0x18,0xFF,0x86,0xA4,0x02,0x00,0x00};
    constexpr uint8_t key[]={0x4C,0x8D,0x71,0x40}; // lea r14,[rcx+40h]
    constexpr uint8_t owner[]={0x48,0x8B,0x76,0x20}; // mov rsi,[rsi+20h]
    __try {
        uint8_t got[sizeof(prologue)]{};
        std::memcpy(got,reinterpret_cast<const void*>(base+0x42B4130),sizeof(got));
        if(g_primaryEntry.ready.load(std::memory_order_acquire) && !std::memcmp(got,prologue,sizeof(got)))
            return "our primary patch is gone";
        if(std::memcmp(got,prologue,sizeof(got))) {
            int32_t jump=0;std::memcpy(&jump,got+1,4);
            const intptr_t expected=reinterpret_cast<intptr_t>(g_primaryEntry.relay)-intptr_t(base+0x42B4135);
            if(!g_primaryEntry.relay || got[0]!=0xE9 || jump!=expected ||
                std::memcmp(got+5,prologue+5,sizeof(got)-5))return "primary prologue mismatch";
        }
        if(std::memcmp(reinterpret_cast<const void*>(base+0x42B4843),call,sizeof(call)))return "primary caller mismatch";
        if(std::memcmp(reinterpret_cast<const void*>(base+0x42B43D8),append,sizeof(append)))return "primary append mismatch";
        if(std::memcmp(reinterpret_cast<const void*>(base+0x42B415B),key,sizeof(key)))return "primary key mismatch";
        if(std::memcmp(reinterpret_cast<const void*>(base+0x42B42DC),owner,sizeof(owner)))return "primary owner mismatch";
        uint64_t primaryHash=0xCBF29CE484222325ull,outerHash=primaryHash;
        for(uintptr_t i=5;i<0x2DA;++i)primaryHash=(primaryHash^*reinterpret_cast<const uint8_t*>(base+0x42B4130+i))*0x100000001B3ull;
        for(uintptr_t i=5;i<0xC96;++i)outerHash=(outerHash^*reinterpret_cast<const uint8_t*>(base+0x42B4420+i))*0x100000001B3ull;
        if(primaryHash!=0xB525B15AAC531498ull)return "primary body mismatch";
        if(outerHash!=0x34D08F7A20C30F09ull)return "outer builder body mismatch";
        // The complete outer body certifies six-argument setup and record/
        // context derivation. Its bracket must still be our installed relay.
        uint8_t outer[5]{};std::memcpy(outer,reinterpret_cast<const void*>(base+0x42B4420),5);
        int32_t jump=0;std::memcpy(&jump,outer+1,4);
        if(!g_bucketEntry.ready.load(std::memory_order_acquire) || !g_bucketEntry.relay || outer[0]!=0xE9 ||
           jump!=reinterpret_cast<intptr_t>(g_bucketEntry.relay)-intptr_t(base+0x42B4425))return "outer builder patch is not ours";
        return nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return "primary image unreadable";}
}

void ensurePrimaryEmit(uintptr_t base) noexcept {
    const char* why=primarySignature(base);
    if(why){g_primaryStatus.store(why,std::memory_order_release);return;}
    g_primaryReturn.store(base+0x42B4848,std::memory_order_release);
    if(!g_primaryEntry.ready.load(std::memory_order_acquire) && !installOne(g_primaryEntry,base)) {
        g_primaryStatus.store("CodeHook refused primary emit",std::memory_order_release);return;
    }
    g_primaryStatus.store("hooked",std::memory_order_release);
}

const char* poolCopySignature(uintptr_t base) noexcept {
    constexpr uint8_t prologue[]={0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,
        0x48,0x89,0x74,0x24,0x20,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x20};
    constexpr uint8_t layout[]={0x48,0x8B,0x81,0xD0,0x00,0x00,0x00,0x4C,0x8B,0xF9,
        0x48,0x8B,0x59,0x10,0x48,0x8B,0x50,0x10,0x48,0x8B,0x82,0x00,0x01,0x00,0x00,
        0x4C,0x8B,0xB2,0x80,0x01,0x00,0x00,0x8B,0x68,0x10};
    __try {
        uint8_t got[sizeof(prologue)]{};
        std::memcpy(got,reinterpret_cast<const void*>(base+0x4C81BE0),sizeof(got));
        if(std::memcmp(got,prologue,sizeof(got))) {
            int32_t actual=0;std::memcpy(&actual,got+1,4);
            if(!g_poolCopyEntry.relay || got[0]!=0xE9 ||
               actual!=reinterpret_cast<intptr_t>(g_poolCopyEntry.relay)-intptr_t(base+0x4C81BE5) ||
               std::memcmp(got+5,prologue+5,sizeof(got)-5))return "pool copier prologue mismatch";
        } else if(g_poolCopyEntry.ready.load(std::memory_order_acquire))return "our pool copier patch is gone";
        if(std::memcmp(reinterpret_cast<const void*>(base+0x4C81BFC),layout,sizeof(layout)))return "pool copier layout mismatch";
        uint64_t hash=0xCBF29CE484222325ull;
        for(uintptr_t i=5;i<0x283;++i)hash=(hash^*reinterpret_cast<const uint8_t*>(base+0x4C81BE0+i))*0x100000001B3ull;
        if(hash!=0x0EEFD3E802137BD5ull)return "pool copier body mismatch";
        return nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return "pool copier image unreadable";}
}
void ensurePoolCopy(uintptr_t base) noexcept {
    const char* why=poolCopySignature(base);
    if(why){g_poolCopyStatus.store(why,std::memory_order_release);return;}
    if(!g_poolCopyEntry.ready.load(std::memory_order_acquire) && !installOne(g_poolCopyEntry,base)) {
        g_poolCopyStatus.store("CodeHook refused pool copier",std::memory_order_release);return;
    }
    g_poolCopyStatus.store("hooked",std::memory_order_release);
}

void ensurePoolMerge(uintptr_t base) noexcept {
    const char* why=nullptr;
    __try {
        constexpr uint8_t prologue[]={0x40,0x53,0x41,0x54,0x48,0x83,0xEC,0x28};
        uint8_t got[sizeof(prologue)]{};std::memcpy(got,reinterpret_cast<const void*>(base+0x434E740),sizeof(got));
        if(std::memcmp(got,prologue,sizeof(got))) {
            int32_t jump=0;std::memcpy(&jump,got+1,4);
            if(!g_poolMergeEntry.relay || got[0]!=0xE9 ||
               jump!=reinterpret_cast<intptr_t>(g_poolMergeEntry.relay)-intptr_t(base+0x434E745) ||
               std::memcmp(got+5,prologue+5,3))why="merge prologue mismatch";
        } else if(g_poolMergeEntry.ready.load(std::memory_order_acquire))why="our merge patch is gone";
        // Entire untouched native body includes full-node splice, partial
        // fill/shift and cold free path, certifying every relocation offset.
        uint64_t hash=0xCBF29CE484222325ull;
        for(uintptr_t i=8;i<0x2A0;++i)hash=(hash^*reinterpret_cast<const uint8_t*>(base+0x434E740+i))*0x100000001B3ull;
        if(hash!=0xFE795C2530FDF4F7ull)why="merge body mismatch";
    } __except(EXCEPTION_EXECUTE_HANDLER){why="merge image unreadable";}
    if(why){g_mergeStatus.store(why,std::memory_order_release);return;}
    if(!g_poolMergeEntry.ready.load(std::memory_order_acquire) && !installOne(g_poolMergeEntry,base)) {
        g_mergeStatus.store("CodeHook refused list merge",std::memory_order_release);return;
    }
    g_mergeStatus.store("hooked",std::memory_order_release);
}
void ensurePoolClear(uintptr_t base) noexcept {
    // Verified build332841: void(dictionary RCX, allocatorOwner RDX),
    // specialized 336-byte nodes using DAT_145EFDD30. Entire native body
    // [36819D5,3681B22) FNV below has no PE relocations; own entry separately.
    const char* why=nullptr;
    __try {
        constexpr uint8_t prologue[]={0x48,0x89,0x4C,0x24,0x08};
        uint8_t got[5]{};std::memcpy(got,reinterpret_cast<const void*>(base+0x36819D0),5);
        if(std::memcmp(got,prologue,5)) {
            int32_t jump=0;std::memcpy(&jump,got+1,4);
            if(!g_poolClearEntry.relay || got[0]!=0xE9 ||
               jump!=reinterpret_cast<intptr_t>(g_poolClearEntry.relay)-intptr_t(base+0x36819D5))why="clear prologue mismatch";
        } else if(g_poolClearEntry.ready.load(std::memory_order_acquire))why="our clear patch is gone";
        uint64_t hash=0xCBF29CE484222325ull;
        for(uintptr_t i=5;i<0x152;++i)hash=(hash^*reinterpret_cast<const uint8_t*>(base+0x36819D0+i))*0x100000001B3ull;
        if(hash!=0x9D89B67124CE2ADFull)why="typed clear body mismatch";
    } __except(EXCEPTION_EXECUTE_HANDLER){why="clear image unreadable";}
    if(why){g_clearStatus.store(why,std::memory_order_release);return;}
    if(!g_poolClearEntry.ready.load(std::memory_order_acquire) && !installOne(g_poolClearEntry,base)) {
        g_clearStatus.store("CodeHook refused typed clear",std::memory_order_release);return;
    }
    g_clearStatus.store("hooked",std::memory_order_release);
}

// Installs the part test's patch once (process lifetime, like the others),
// under g_installMutex. Stands down alone: the status says why.
void ensurePartTest(uintptr_t base) noexcept {
    if(g_partEntry.ready.load(std::memory_order_acquire)) {
        g_partStatus.store(partPatchIsOurs(base)?"hooked"
                           :"our patch is gone (FUN_1442B3FC0's entry was rewritten)",std::memory_order_release);
        return;
    }
    const char* why=partTestSignature(base,g_partEntry);
    if(why) {g_partStatus.store(why,std::memory_order_release);return;}
    g_partReturn.store(base+kPartCallReturnRva,std::memory_order_release);
    if(!installOne(g_partEntry,base)) {
        g_partStatus.store("CodeHook refused the patch (its cull-gate-part-test line says why)",
                           std::memory_order_release);
        return;
    }
    g_partStatus.store(partPatchIsOurs(base)?"hooked":"the patch did not verify after install",
                       std::memory_order_release);
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
        // pre-check raced a concurrent install that had patched the
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
// raced: one consumer's detach read another's want as false, the other's
// attach published gate=1, and the detach then wrote gate=0, leaving a live
// consumer deaf (2026-09-20 review finding 4).
void recomputeGateLocked() noexcept {
    const bool open=observer.load(std::memory_order_acquire)!=nullptr ||
                    emitWanted.load(std::memory_order_acquire) ||
                    schedulerWanted.load(std::memory_order_acquire) ||
                    gateProbeWanted.load(std::memory_order_acquire);
    evalGate.store(open?uintptr_t(1):uintptr_t(0),std::memory_order_release);
    primaryGate.store(emitWanted.load(std::memory_order_acquire) &&
        g_primaryEntry.ready.load(std::memory_order_acquire) &&
        g_poolCopyEntry.ready.load(std::memory_order_acquire) &&
        g_poolMergeEntry.ready.load(std::memory_order_acquire) &&
        g_poolClearEntry.ready.load(std::memory_order_acquire) &&
        std::strcmp(g_primaryStatus.load(std::memory_order_acquire),"hooked")==0 &&
        std::strcmp(g_poolCopyStatus.load(std::memory_order_acquire),"hooked")==0 &&
        std::strcmp(g_mergeStatus.load(std::memory_order_acquire),"hooked")==0 &&
        std::strcmp(g_clearStatus.load(std::memory_order_acquire),"hooked")==0 ? uintptr_t(1):uintptr_t(0),
        std::memory_order_release);
}

void detachKinematicEvalHooks(KinematicEvalProbe* probe) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        observer.compare_exchange_strong(probe,nullptr,std::memory_order_acq_rel);
        recomputeGateLocked();
    } catch(...) {}
}

void kinematicEvalSetEmitObserver(EngineEmitObserverFn fn) noexcept {
    emitObserver.store(fn,std::memory_order_release);
}
void kinematicEvalSetPrimaryEmitObserver(EnginePrimaryEmitObserverFn fn) noexcept {
    primaryEmitObserver.store(fn,std::memory_order_release);
}
const char* kinematicEvalPrimaryEmitStatus() noexcept {
    return g_primaryStatus.load(std::memory_order_acquire);
}
void kinematicEvalPrimaryEmitCounters(uint64_t& calls,uint64_t& unowned) noexcept {
    calls=g_primaryCalls.exchange(0,std::memory_order_relaxed);
    unowned=g_primaryUnowned.exchange(0,std::memory_order_relaxed);
}
void kinematicEvalSetPoolCopyObserver(EnginePoolCopyObserverFn fn) noexcept {
    poolCopyObserver.store(fn,std::memory_order_release);
}
const char* kinematicEvalPoolCopyStatus() noexcept { return g_poolCopyStatus.load(std::memory_order_acquire); }
void kinematicEvalSetMergeObserver(EngineMergeBeginFn begin,EngineMergeEndFn end) noexcept {
    mergeBeginObserver.store(begin,std::memory_order_release);mergeEndObserver.store(end,std::memory_order_release);
}
const char* kinematicEvalMergeStatus() noexcept {return g_mergeStatus.load(std::memory_order_acquire);}
void kinematicEvalSetClearObserver(EngineClearObserverFn fn) noexcept {clearObserver.store(fn,std::memory_order_release);}
const char* kinematicEvalClearStatus() noexcept {return g_clearStatus.load(std::memory_order_acquire);}

bool kinematicEvalEmitHookLive(const char** why) noexcept {
    const char* reason=nullptr;
    if(!g_evalEntry.ready.load(std::memory_order_acquire))
        reason="the kinematic hook set is not installed; the emit's attach line names why";
    else if(!g_directEntries[0].ready.load(std::memory_order_acquire))
        reason="CodeHook refused kinematic-build-144312e00; its own line names why";
    else if(evalGate.load(std::memory_order_acquire)==0)
        reason="the hook set's gate is closed (the emit is detached), so the relay never calls the bracket";
    if(why)*why=reason;
    return reason==nullptr;
}

const char* kinematicEvalEmitAttach() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if(!base)return "identity_mismatch";
        if(!g_evalEntry.ready.load(std::memory_order_acquire) && !targetValid(base))
            return "identity_mismatch";
        if(!ensureInstalled(base))return "install_failed";
        if(!patchIsOurs(g_evalEntry,base))return "opcode_mismatch";
        ensurePrimaryEmit(base);
        ensurePoolCopy(base);
        ensurePoolMerge(base);
        ensurePoolClear(base);
        emitWanted.store(true,std::memory_order_release);
        recomputeGateLocked();
        return "installed";
    } catch(...) {return "install_failed";}
}

void kinematicEvalEmitDetach() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        emitWanted.store(false,std::memory_order_release);
        recomputeGateLocked();
    } catch(...) {}
}

const char* kinematicEvalSchedulerAttach() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if(!base)return "identity_mismatch";
        if(!g_evalEntry.ready.load(std::memory_order_acquire) && !targetValid(base))
            return "identity_mismatch";
        if(!ensureInstalled(base))return "install_failed";
        if(!patchIsOurs(g_evalEntry,base))return "opcode_mismatch";
        schedulerWanted.store(true,std::memory_order_release);
        recomputeGateLocked();
        return "installed";
    } catch(...) {return "install_failed";}
}

void kinematicEvalSchedulerDetach() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        schedulerWanted.store(false,std::memory_order_release);
        recomputeGateLocked();
    } catch(...) {}
}

void kinematicEvalSetGateProbeObservers(GateProbeGateFn gate,GateProbeBuilderFn builder,
                                        GateProbePartFn part) noexcept {
    gateProbeGate.store(gate,std::memory_order_release);
    gateProbeBuilder.store(builder,std::memory_order_release);
    gateProbePart.store(part,std::memory_order_release);
}

const char* kinematicEvalGateProbeAttach() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if(!base)return "identity_mismatch";
        if(!g_evalEntry.ready.load(std::memory_order_acquire) && !targetValid(base))
            return "identity_mismatch";
        if(!ensureInstalled(base))return "install_failed";
        if(!patchIsOurs(g_evalEntry,base))return "opcode_mismatch";
        // The per-part test: this probe's own patch, standing down alone.
        ensurePartTest(base);
        gateProbeWanted.store(true,std::memory_order_release);
        recomputeGateLocked();
        partGate.store(partPatchIsOurs(base)?uintptr_t(1):uintptr_t(0),std::memory_order_release);
        return "installed";
    } catch(...) {return "install_failed";}
}

void kinematicEvalGateProbeDetach() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        partGate.store(0,std::memory_order_release);
        gateProbeWanted.store(false,std::memory_order_release);
        recomputeGateLocked();
    } catch(...) {}
}

bool kinematicEvalBuilderHooked() noexcept {
    return g_bucketEntry.ready.load(std::memory_order_acquire) &&
           g_bucketEntry.forward.load(std::memory_order_acquire)!=0;
}

const char* kinematicEvalPartTestStatus() noexcept {
    return g_partStatus.load(std::memory_order_acquire);
}

} // namespace edvr
