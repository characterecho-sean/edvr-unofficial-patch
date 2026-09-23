#include "kinematic_eval_hook.h"
#include "kinematic_eval_probe.h"
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
// FUN_142819D90 (decomp_2819D90.txt): f(param_1 ctx, param_2 the 48-byte
// header, param_3 the view vector, param_4, param_5 float* x, param_6,
// param_7) -- SEVEN params, the last three on the stack (it reads param_5 at
// [rsp+60h], param_6/7 at +68h/+70h after its prologue). The callback
// declares all seven so the compiler lays the forward's stack arguments out
// exactly as the caller did, the builder bracket's rule.
using SetterFn = uintptr_t (__fastcall*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,
                                         uintptr_t,uintptr_t,uintptr_t);

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
// eval callbacks -- the probe while attached (eye-dump captures), the
// kinematic tracker while fix.temporal_aa is on (no dump involved), the
// scheduler stack probe while armed, or the static prop gate while
// fix.static_prop_updates is on.
// observer stays the probe's own cell; the relays gate on evalGate.
alignas(8) std::atomic<uintptr_t> evalGate{0};
static_assert(decltype(evalGate)::is_always_lock_free,
              "The x64 relay reads the aligned atomic pointer directly.");
std::atomic<bool> trackerWanted{false};
alignas(8) std::atomic<KinematicTrackerObserverFn> trackerObserver{nullptr};
// Engine-record velocity's emit observer (direct producer 0's post-forward).
alignas(8) std::atomic<EngineEmitObserverFn> emitObserver{nullptr};
// The scheduler stack probe's want: its targets 0/1 are the job bodies
// themselves, already patched by this file, so it observes through the
// job-0/1 relays and holds this gate open while armed.
std::atomic<bool> schedulerWanted{false};
// The static prop gate's want (fix.static_prop_updates): job 0's relay is
// its hook site, so it observes through the bracket and holds this gate
// open while enabled.
std::atomic<bool> staticGateWanted{false};
alignas(8) std::atomic<StaticGateDecideFn> staticGateObserver{nullptr};
// The cull gate probe's want (advanced.cull_gate_capture): observes the
// evaluator (the traversal's per-view gate) after its forward and the
// bucket bracket (the draw-item builder) before its forward, for the few
// frames of an armed eye run.
std::atomic<bool> gateProbeWanted{false};
alignas(8) std::atomic<GateProbeGateFn> gateProbeGate{nullptr};
alignas(8) std::atomic<GateProbeBuilderFn> gateProbeBuilder{nullptr};
alignas(8) std::atomic<GateProbePartFn> gateProbePart{nullptr};
// The settlement LOD governor's want (fix.settlement_detail): it observes the
// draw-item builder and the per-part test, and brackets the LOD-scale setter.
std::atomic<bool> governorWanted{false};
alignas(8) std::atomic<LodGovernorBuilderFn> governorBuilder{nullptr};
alignas(8) std::atomic<LodGovernorPartFn> governorPart{nullptr};
alignas(8) std::atomic<LodGovernorSetterFn> governorSetter{nullptr};
// FUN_142819D90's relay gate: a cell of its own, open only while the governor
// is attached AND the patch is verified ours. Two calls a frame.
alignas(8) std::atomic<uintptr_t> setterGate{0};
static_assert(decltype(setterGate)::is_always_lock_free,
              "The x64 relay reads the aligned atomic cell directly.");
std::atomic<const char*> g_setterStatus{"not requested"};
// FUN_1442B3FC0's relay gate: a cell of its own, not evalGate, so the
// per-part patch (installed only for the gate probe and the governor) costs
// the tracker and the other evalGate consumers nothing. Open only while the
// probe or the governor is attached AND the patch is verified ours.
alignas(8) std::atomic<uintptr_t> partGate{0};
static_assert(decltype(partGate)::is_always_lock_free,
              "The x64 relay reads the aligned atomic cell directly.");
// The draw-item builder bracket's relay gate: evalGate OR the governor's
// want, so the governor alone opens this one relay (and the part test's)
// without waking the evaluator, the job brackets or the direct producers.
// Every evalGate store below keeps it in step (recomputeGateLocked, and the
// two direct stores in the attach paths).
alignas(8) std::atomic<uintptr_t> builderGate{0};
static_assert(decltype(builderGate)::is_always_lock_free,
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
__declspec(noinline) uintptr_t __fastcall partTestObserved(uintptr_t,uintptr_t,uintptr_t) noexcept;
__declspec(noinline) uintptr_t __fastcall setterObserved(uintptr_t,uintptr_t,uintptr_t,uintptr_t,
                                                         uintptr_t,uintptr_t,uintptr_t) noexcept;
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
                        reinterpret_cast<void*>(&bucketBuildObserved),&builderGate};
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

// --- The LOD-scale setter (FUN_142819D90), the settlement LOD governor's write site
// It rebuilds the render context every frame (the views, the view-bit table,
// the count) and ends by storing the LOD scale, `*(float *)(param_1 + 6) =
// 1 + (1 - *param_5)` (decomp_2819D90.txt:108; the design note's section 9
// has the per-frame evidence). Its only code caller is FUN_1401EA920, twice a
// frame, once per context, before either context is handed to the traversal.
// The observer reads ctx+0x30 after the forward, so the signature is every
// instruction that read rests on, in the hash-verified exe (build 332841):
// the prologue (one five-byte instruction is moved, not RIP-relative; no
// branch in the function lands inside it), the context kept in rbx, the
// scale's arithmetic, the 16-byte store at rbx+0x30, and the epilogue right
// after it (no later store). Any mismatch stands this hook down alone.
constexpr uintptr_t kSetterRva=0x2819D90u;
// mov [rsp+20h],rbx; push rbp; push rsi; push r12; sub rsp,20h; mov rbp,[r8]
constexpr uint8_t kSetterPrologue[16]={0x48,0x89,0x5C,0x24,0x20,0x55,0x56,0x41,0x54,
                                       0x48,0x83,0xEC,0x20,0x49,0x8B,0x28};
constexpr CodeBytes kSetterBody[]={
    {0x2819DB4u,3,{0x48,0x8B,0xD9},
     "setter mismatch at RVA 0x2819DB4 (mov rbx,rcx: the context)"},
    {0x2819F40u,7,{0x0F,0x28,0x0D,0x39,0x59,0x61,0x02},
     "setter mismatch at RVA 0x2819F40 (movaps xmm1,[1.0])"},
    {0x2819F47u,16,{0x48,0x8B,0x44,0x24,0x60,0x0F,0x28,0xC1,0xF3,0x0F,0x5C,0x00,0xF3,0x0F,0x58,0xC8},
     "setter mismatch at RVA 0x2819F47 (s = 1 + (1 - *param_5))"},
    {0x2819F57u,4,{0x0F,0x11,0x4B,0x30},
     "setter mismatch at RVA 0x2819F57 (movups [rbx+30h]: the store)"},
    {0x2819F5Bu,14,{0x48,0x8B,0x5C,0x24,0x58,0x48,0x83,0xC4,0x20,0x41,0x5C,0x5E,0x5D,0xC3},
     "setter mismatch at RVA 0x2819F5B (the epilogue after the store)"},
};
HookEntry g_setterEntry{"lod-scale-setter",kSetterRva,reinterpret_cast<void*>(&setterObserved),&setterGate};

// FUN_1404F4E10's first sixteen bytes in the hash-verified executable, as
// object_probe.cpp checks them before the cull gate probe calls it: movups
// xmm3,[r8]; xor r9d,r9d; movups xmm2,[rdx]; movaps xmm0,xmm3; movzx r8d,...
constexpr uintptr_t kFrustumRva=0x4F4E10u;
constexpr uint8_t kFrustumPrologue[16]={0x41,0x0F,0x10,0x18,0x45,0x33,0xC9,0x0F,
                                        0x10,0x12,0x0F,0x28,0xC3,0x44,0x0F,0xB7};

// Per-thread mask of the job brackets currently on the stack (1u<<jobId),
// maintained by bracket() and read by evalObserved: attributes every eval
// observation to the engine job that scheduled it. Nesting-safe (save/
// restore), zero when no observed job is running.
thread_local uint32_t t_jobMask=0;
// The gate probe's row for the builder call on this thread's stack (set by
// bucketBuildObserved around its forward, save/restore), read by the part
// test's observer: joins each FUN_1442B3FC0 verdict to its builder row.
thread_local uint32_t t_builderRow=kGateProbeNoRow;

__declspec(noinline) uintptr_t __fastcall evalObserved(uintptr_t descriptor,uintptr_t param2,
                                                       uintptr_t renderRecord) noexcept {
    const uint32_t jobMask=t_jobMask;
    KinematicEvalProbe* probe=observer.load(std::memory_order_acquire);
    if(probe)probe->observe(descriptor,renderRecord,jobMask); // observe() gates on active()
    const auto tracker=trackerObserver.load(std::memory_order_acquire);
    if(tracker)tracker(descriptor,jobMask); // kinematicMotionObserve gates on its own flag
    const auto forward=reinterpret_cast<EvalFn>(g_evalEntry.forward.load(std::memory_order_acquire));
    const uintptr_t result=forward(descriptor,param2,renderRecord);
    // The cull gate probe reads the verdict the call just wrote (param2:
    // u32 LOD index, u8 passed); renderRecord is the VIEW (design doc §9).
    const auto gateProbe=gateProbeGate.load(std::memory_order_acquire);
    if(gateProbe)gateProbe(descriptor,param2,renderRecord);
    return result;
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
    // The static prop gate's change test (fix.static_prop_updates): job 0
    // only, before the timed region. A skip verdict forwards past the call
    // entirely -- no forward, no bracket timing, no probe observations: a
    // skipped call is one the engine never runs, so nothing downstream of
    // this point may record it as executed.
    if(job==0) {
        const auto gate=staticGateObserver.load(std::memory_order_acquire);
        if(gate && gate(a))return 0;
    }
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
    // (fix.temporal_aa on, probe never attached) times too. jobs[] now
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
    // The settlement LOD governor (shadow only) reads the record's own LOD
    // test results as the traversal left them -- rec+0x208/+0x210, before
    // the builder runs -- and counts the call: one call, one engine record.
    const auto governor=governorBuilder.load(std::memory_order_acquire);
    if(governor)governor(a,b,c,d);
    // The cull gate probe sees the builder's inputs as the builder will:
    // before the forward (a = pose context, b = render context, c = the
    // collection's active view mask, d = rec+0x210). The row it keeps is
    // this thread's for the forward: the per-part tests inside carry it.
    const auto builderProbe=gateProbeBuilder.load(std::memory_order_acquire);
    const uint32_t outerRow=t_builderRow;
    t_builderRow=builderProbe?builderProbe(a,b,c,d):kGateProbeNoRow;
    // The bucket census is the eval-gate consumers' (kinematicEvalProbe's
    // bucket_items): it runs only while the eval gate is open, exactly as it
    // did when that gate was this relay's own. The governor alone holding the
    // builder gate costs the census nothing.
    const bool census=evalGate.load(std::memory_order_acquire)!=0;
    BucketSnap snaps[kBucketWalkCap];
    uint32_t flags=0;
    const uint32_t n=census?collectBuckets(a,snaps,kBucketWalkCap,&flags):0;
    const uintptr_t result=forward(a,b,c,d,e,f);
    t_builderRow=outerRow;
    if(!census)return result;
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
    return result;
}

// --- The per-part test's observer (FUN_1442B3FC0) ---------------------------
// Reached only through its own relay (partGate: the gate probe or the LOD
// governor attached and the patch verified ours). Forwards first; the probe
// then reads the verdict the call just wrote (param_2) and the builder's
// frame around it, and the governor recomputes the verdict's LOD term --
// both read-only: the verdict is never written.
// The return address names the caller: the relay JUMPS here, so the slot
// holds the builder's return address (base+0x42B4B96) when the call came
// from its sub-item loop.
__declspec(noinline) uintptr_t __fastcall partTestObserved(uintptr_t items,uintptr_t out,
                                                           uintptr_t view) noexcept {
    const auto forward=reinterpret_cast<EvalFn>(g_partEntry.forward.load(std::memory_order_acquire));
    if(!forward)return 0; // stood down at install; the relay is unreachable then
    const uintptr_t result=forward(items,out,view);
    const auto part=gateProbePart.load(std::memory_order_acquire);
    const auto governor=governorPart.load(std::memory_order_acquire);
    if(part||governor) {
        const uintptr_t from=reinterpret_cast<uintptr_t>(_ReturnAddress());
        const bool fromBuilder=from==g_partReturn.load(std::memory_order_relaxed);
        if(part)part(items,out,view,t_builderRow,fromBuilder);
        if(governor)governor(items,out,view,fromBuilder);
    }
    return result;
}

// --- The LOD-scale setter's bracket (FUN_142819D90) --------------------------
// Reached only through its own relay (setterGate: the governor attached and
// the patch verified ours). Forwards first -- the engine rebuilds the context
// and stores its LOD scale at +0x30 -- then hands the governor the context,
// which reads that value and, while acting, scales it (lod_governor.cpp).
// This bracket reads and writes nothing itself.
__declspec(noinline) uintptr_t __fastcall setterObserved(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d,
                                                         uintptr_t e,uintptr_t f,uintptr_t g) noexcept {
    const auto forward=reinterpret_cast<SetterFn>(g_setterEntry.forward.load(std::memory_order_acquire));
    if(!forward)return 0; // stood down at install; the relay is unreachable then
    const uintptr_t result=forward(a,b,c,d,e,f,g);
    const auto governor=governorSetter.load(std::memory_order_acquire);
    if(governor)governor(a);
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
    int32_t start=0;
    bool fault=false;
    __try {
        std::memcpy(&start,reinterpret_cast<const void*>(b+0x2A4),4);
    } __except(EXCEPTION_EXECUTE_HANDLER) {fault=true;}
    const uintptr_t result=forward(a,b,c,d);
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

// The image is build 332841: null, else why not (the part test's first check).
const char* buildSignature(uintptr_t base) noexcept {
    __try {
        uint32_t peOff=0;
        std::memcpy(&peOff,reinterpret_cast<const void*>(base+0x3C),4);
        if(peOff>0x1000)return "not build 332841 (no PE header)";
        uint32_t timestamp=0,imageSize=0;
        std::memcpy(&timestamp,reinterpret_cast<const void*>(base+peOff+8),4);
        std::memcpy(&imageSize,reinterpret_cast<const void*>(base+peOff+0x50),4);
        if(timestamp!=KinematicEvalProbe::kExpectedTimestamp ||
           imageSize!=KinematicEvalProbe::kExpectedImageSize)return "not build 332841 (PE timestamp/size)";
        return nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return "unreadable executable image";}
}

// FUN_142819D90's build-keyed signature: null when the image is build 332841
// and every byte the setter observer's read rests on is in place (the
// prologue, or our own patch followed by the prologue's rest); otherwise the
// reason the hook stands down.
const char* setterSignature(uintptr_t base,const HookEntry& entry) noexcept {
    if(const char* why=buildSignature(base))return why;
    __try {
        uint8_t got[sizeof(kSetterPrologue)]{};
        std::memcpy(got,reinterpret_cast<const void*>(base+kSetterRva),sizeof(got));
        if(std::memcmp(got,kSetterPrologue,sizeof(got))!=0) {
            int32_t actual=0;std::memcpy(&actual,got+1,4);
            const intptr_t displacement=entry.relay
                ?reinterpret_cast<intptr_t>(entry.relay)-intptr_t(base+kSetterRva+5):0;
            if(!(entry.relay && got[0]==0xE9 && actual==displacement &&
                 std::memcmp(got+5,kSetterPrologue+5,sizeof(got)-5)==0))
                return "prologue mismatch at RVA 0x2819D90";
        }
        for(const CodeBytes& c:kSetterBody)
            if(std::memcmp(reinterpret_cast<const void*>(base+c.rva),c.b,c.n)!=0)return c.why;
        return nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return "unreadable executable image";}
}

bool setterPatchIsOurs(uintptr_t base) noexcept {
    if(!g_setterEntry.ready.load(std::memory_order_acquire) || !g_setterEntry.relay)return false;
    const uintptr_t target=base+kSetterRva;
    const intptr_t displacement=reinterpret_cast<intptr_t>(g_setterEntry.relay)-intptr_t(target+5);
    uint8_t bytes[sizeof(kSetterPrologue)]{};
    __try {std::memcpy(bytes,reinterpret_cast<const void*>(target),sizeof(bytes));}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    int32_t actual=0;std::memcpy(&actual,bytes+1,4);
    return bytes[0]==0xE9 && actual==displacement &&
           std::memcmp(bytes+5,kSetterPrologue+5,sizeof(bytes)-5)==0;
}

// Installs the setter's patch once (process lifetime, like the others), under
// g_installMutex. Stands down alone: the status says why.
void ensureSetter(uintptr_t base) noexcept {
    if(g_setterEntry.ready.load(std::memory_order_acquire)) {
        g_setterStatus.store(setterPatchIsOurs(base)?"hooked"
                             :"our patch is gone (FUN_142819D90's entry was rewritten)",std::memory_order_release);
        return;
    }
    const char* why=setterSignature(base,g_setterEntry);
    if(why) {g_setterStatus.store(why,std::memory_order_release);return;}
    if(!installOne(g_setterEntry,base)) {
        g_setterStatus.store("CodeHook refused the patch (its lod-scale-setter line says why)",
                             std::memory_order_release);
        return;
    }
    g_setterStatus.store(setterPatchIsOurs(base)?"hooked":"the patch did not verify after install",
                         std::memory_order_release);
}

// FUN_142819D90's cell, from the governor's want and whether the patch is
// still ours; every transition holds g_installMutex.
void recomputeSetterGateLocked(uintptr_t base) noexcept {
    const bool wanted=governorWanted.load(std::memory_order_acquire);
    setterGate.store(wanted && base && setterPatchIsOurs(base)?uintptr_t(1):uintptr_t(0),
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
        // pre-check raced a concurrent tracker install that had patched the
        // prologue but not yet published ready, and rejected the supported
        // executable (2026-09-20 review finding 5).
        if(!g_evalEntry.ready.load(std::memory_order_acquire) && !targetValid(base))
            return "identity_mismatch";
        if(!ensureInstalled(base))return "install_failed";
        if(!patchIsOurs(g_evalEntry,base))return "opcode_mismatch";
        observer.store(probe,std::memory_order_release);
        evalGate.store(1,std::memory_order_release);
        builderGate.store(1,std::memory_order_release);   // evalGate open implies it
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
                    trackerWanted.load(std::memory_order_acquire) ||
                    schedulerWanted.load(std::memory_order_acquire) ||
                    staticGateWanted.load(std::memory_order_acquire) ||
                    gateProbeWanted.load(std::memory_order_acquire);
    evalGate.store(open?uintptr_t(1):uintptr_t(0),std::memory_order_release);
    // The builder bracket's own cell: every eval consumer's want, plus the
    // settlement LOD governor's (which needs this relay and no other).
    const bool builder=open || governorWanted.load(std::memory_order_acquire);
    builderGate.store(builder?uintptr_t(1):uintptr_t(0),std::memory_order_release);
}

// FUN_1442B3FC0's cell, from its two consumers' wants and whether the patch
// is still ours; every transition holds g_installMutex.
void recomputePartGateLocked(uintptr_t base) noexcept {
    const bool wanted=gateProbeWanted.load(std::memory_order_acquire) ||
                      governorWanted.load(std::memory_order_acquire);
    partGate.store(wanted && base && partPatchIsOurs(base)?uintptr_t(1):uintptr_t(0),
                   std::memory_order_release);
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

void kinematicEvalSetEmitObserver(EngineEmitObserverFn fn) noexcept {
    emitObserver.store(fn,std::memory_order_release);
}

bool kinematicEvalEmitHookLive(const char** why) noexcept {
    const char* reason=nullptr;
    if(!g_evalEntry.ready.load(std::memory_order_acquire))
        reason="the kinematic hook set is not installed; the tracker's attach line names why";
    else if(!g_directEntries[0].ready.load(std::memory_order_acquire))
        reason="CodeHook refused kinematic-build-144312e00; its own line names why";
    else if(evalGate.load(std::memory_order_acquire)==0)
        reason="the hook set's gate is closed (the tracker is detached), so the relay never calls the bracket";
    if(why)*why=reason;
    return reason==nullptr;
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
        builderGate.store(1,std::memory_order_release);   // evalGate open implies it
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

void kinematicEvalSetStaticGateObserver(StaticGateDecideFn fn) noexcept {
    staticGateObserver.store(fn,std::memory_order_release);
}

const char* kinematicEvalStaticGateAttach() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if(!base)return "identity_mismatch";
        if(!g_evalEntry.ready.load(std::memory_order_acquire) && !targetValid(base))
            return "identity_mismatch";
        if(!ensureInstalled(base))return "install_failed";
        if(!patchIsOurs(g_evalEntry,base))return "opcode_mismatch";
        staticGateWanted.store(true,std::memory_order_release);
        recomputeGateLocked();
        return "installed";
    } catch(...) {return "install_failed";}
}

void kinematicEvalStaticGateDetach() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        staticGateWanted.store(false,std::memory_order_release);
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
        recomputePartGateLocked(base);
        return "installed";
    } catch(...) {return "install_failed";}
}

void kinematicEvalGateProbeDetach() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        gateProbeWanted.store(false,std::memory_order_release);
        recomputeGateLocked();
        // Still open while the LOD governor holds it.
        recomputePartGateLocked(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    } catch(...) {}
}

void kinematicEvalSetLodGovernorObservers(LodGovernorBuilderFn builder,LodGovernorPartFn part,
                                          LodGovernorSetterFn setter) noexcept {
    governorBuilder.store(builder,std::memory_order_release);
    governorPart.store(part,std::memory_order_release);
    governorSetter.store(setter,std::memory_order_release);
}

const char* kinematicEvalLodGovernorAttach() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if(!base)return "identity_mismatch";
        if(!g_evalEntry.ready.load(std::memory_order_acquire) && !targetValid(base))
            return "identity_mismatch";
        if(!ensureInstalled(base))return "install_failed";
        if(!patchIsOurs(g_evalEntry,base))return "opcode_mismatch";
        // The per-part test's patch: the probe's, installed once, standing
        // down alone (kinematicEvalPartTestStatus says why).
        ensurePartTest(base);
        // The LOD-scale setter's patch: the governor's own, installed once,
        // standing down alone (kinematicEvalLodSetterStatus says why).
        ensureSetter(base);
        governorWanted.store(true,std::memory_order_release);
        recomputeGateLocked();
        recomputePartGateLocked(base);
        recomputeSetterGateLocked(base);
        return "installed";
    } catch(...) {return "install_failed";}
}

void kinematicEvalLodGovernorDetach() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        governorWanted.store(false,std::memory_order_release);
        recomputeGateLocked();
        const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        recomputePartGateLocked(base);
        recomputeSetterGateLocked(base);
    } catch(...) {}
}

const char* kinematicEvalLodSetterStatus() noexcept {
    return g_setterStatus.load(std::memory_order_acquire);
}

LodGovernorFrustumFn kinematicEvalFrustumFn() noexcept {
    const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if(!base || buildSignature(base))return nullptr;
    uint8_t got[sizeof(kFrustumPrologue)]{};
    __try {std::memcpy(got,reinterpret_cast<const void*>(base+kFrustumRva),sizeof(got));}
    __except(EXCEPTION_EXECUTE_HANDLER){return nullptr;}
    return std::memcmp(got,kFrustumPrologue,sizeof(got))==0
        ?reinterpret_cast<LodGovernorFrustumFn>(base+kFrustumRva):nullptr;
}

bool kinematicEvalBuilderHooked() noexcept {
    return g_bucketEntry.ready.load(std::memory_order_acquire) &&
           g_bucketEntry.forward.load(std::memory_order_acquire)!=0;
}

const char* kinematicEvalPartTestStatus() noexcept {
    return g_partStatus.load(std::memory_order_acquire);
}

} // namespace edvr
