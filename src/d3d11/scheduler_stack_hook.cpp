#include "scheduler_stack_hook.h"
#include "scheduler_stack_probe.h"
#include "kinematic_eval_hook.h"
#include "../common/code_hook.h"
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstring>
#include <mutex>

namespace edvr {
namespace {

// The forward signature for both own targets. Four register params cover
// the originals (the drain takes a handful of register args, the reset
// fewer); stack-passed params, if any, sit above the return address and are
// untouched by a four-param forward. The kinematic hook's direct-producer
// brackets make the same call for the same reason.
using ObservedFn = uintptr_t (__fastcall*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t);

alignas(8) std::atomic<SchedulerStackProbe*> observer{nullptr};
static_assert(decltype(observer)::is_always_lock_free,
              "The x64 relay reads the aligned atomic pointer directly.");

// The relays gate on this cell, not on `observer` directly: the static
// prop gate (fix.static_prop_updates) needs resetObserved to fire even
// while the probe is dark, so it holds the relays open through its own
// want and the cell is recomputed from both under g_installMutex (the
// kinematic eval hook's evalGate/recomputeGateLocked discipline -- the
// 2026-09-20 review finding 4 race was an unlocked check-then-set).
alignas(8) std::atomic<uintptr_t> relayGate{0};
std::atomic<bool> gateWanted{false};
alignas(8) std::atomic<SchedulerResetObserverFn> resetObserver{nullptr};

// The prologues, from the hash-verified exe (SHA-256 e6be8bbe...4e988;
// verified 2026-09-21 against analysis/EliteDangerous64.exe and against
// tools/build_diff_targets.json's prologue_hex for 0x36A0F50):
//   0x42DF940: mov [rsp+8],rbx; mov [rsp+10h],rsi; push rdi; sub rsp,28h
//   0x36A0F50: mov [rsp+8],rcx; push rbx; push rsi; push rdi; sub rsp,28h
constexpr uint8_t kDrainPrologue[14]={
    0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC};
constexpr uint8_t kResetPrologue[14]={
    0x48,0x89,0x4C,0x24,0x08,0x55,0x56,0x48,0x83,0xEC,0x48,0x80,0xB9,0x78};

struct HookEntry {
    const char* name;
    uintptr_t rva;
    const uint8_t* prologue;
    void* callback;
    std::atomic<uintptr_t> forward{0};
    CodeHook hook;
    uint8_t* relay=nullptr;
    std::atomic<bool> ready{false};
};

__declspec(noinline) uintptr_t __fastcall drainObserved(uintptr_t,uintptr_t,
                                                        uintptr_t,uintptr_t) noexcept;
__declspec(noinline) uintptr_t __fastcall resetObserved(uintptr_t,uintptr_t,
                                                        uintptr_t,uintptr_t) noexcept;

HookEntry g_entries[2]={
    {"scheduler-drain-1442df940",SchedulerStackProbe::kHookRvas[0],kDrainPrologue,
     reinterpret_cast<void*>(&drainObserved)},
    {"scheduler-reset-1436a0f50",SchedulerStackProbe::kHookRvas[1],kResetPrologue,
     reinterpret_cast<void*>(&resetObserved)},
};

// --- Relay machinery, copied from object_record_writer_hook.cpp via
// kinematic_eval_hook.cpp. Kept as a copy rather than a shared unit so the
// flight-proven writer hook file is not touched; if one changes, change
// all three. -----------------------------------------------------------
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
    // original: jmp [trampoline]. RAX/flags are volatile, and neither
    // target consumes RAX on entry. No stack adjustment or nonvolatile
    // modification occurs.
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

// The two callbacks: capture the stack FIRST (pre-forward, at the observed
// function's entry -- the relay tail-jumped here, so this frame's return
// address slot is the target's entry RSP and [it] is the engine caller's
// return address), then run the original through the trampoline.
__declspec(noinline) uintptr_t __fastcall drainObserved(uintptr_t a,uintptr_t b,
                                                        uintptr_t c,uintptr_t d) noexcept {
    SchedulerStackProbe* probe=observer.load(std::memory_order_acquire);
    if(probe)probe->noteEntry(2,reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()));
    const auto forward=reinterpret_cast<ObservedFn>(g_entries[0].forward.load(std::memory_order_acquire));
    if(!forward)return 0; // stood down at install; the relay is unreachable then
    return forward(a,b,c,d);
}

__declspec(noinline) uintptr_t __fastcall resetObserved(uintptr_t a,uintptr_t b,
                                                        uintptr_t c,uintptr_t d) noexcept {
    SchedulerStackProbe* probe=observer.load(std::memory_order_acquire);
    if(probe)probe->noteEntry(3,reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()));
    // The static prop gate's invalidation trigger: the reset path is about
    // to rebuild the bucket population, so every cached collection must
    // re-run once. One atomic load when no observer is registered.
    const auto gateReset=resetObserver.load(std::memory_order_acquire);
    if(gateReset)gateReset();
    const auto forward=reinterpret_cast<ObservedFn>(g_entries[1].forward.load(std::memory_order_acquire));
    if(!forward)return 0;
    return forward(a,b,c,d);
}

std::mutex g_installMutex;

bool patchIsOurs(const HookEntry& entry,uintptr_t base) noexcept;

bool targetValid(uintptr_t base) noexcept {
    __try {
        uint32_t peOff=0;
        std::memcpy(&peOff,reinterpret_cast<const void*>(base+0x3C),4);
        if(peOff>0x1000)return false;
        uint32_t timestamp=0,imageSize=0;
        std::memcpy(&timestamp,reinterpret_cast<const void*>(base+peOff+8),4);
        std::memcpy(&imageSize,reinterpret_cast<const void*>(base+peOff+0x50),4);
        if(timestamp!=SchedulerStackProbe::kExpectedTimestamp ||
           imageSize!=SchedulerStackProbe::kExpectedImageSize)return false;
        for(const HookEntry& entry:g_entries) {
            uint8_t got[14]{};
            std::memcpy(got,reinterpret_cast<const void*>(base+entry.rva),sizeof(got));
            if(std::memcmp(got,entry.prologue,sizeof(got))!=0 &&
               !(entry.ready.load(std::memory_order_acquire) && patchIsOurs(entry,base)))
                return false;
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

bool installOne(HookEntry& entry,uintptr_t base) noexcept {
    entry.relay=allocateRelay(base+entry.rva);
    if(!entry.relay)return false;
    // The relays gate on relayGate (probe OR static-gate want), not on
    // `observer` -- see the cell's declaration above.
    buildRelay(entry.relay,&relayGate,entry.callback);
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
    uint8_t bytes[13]{};
    __try {std::memcpy(bytes,reinterpret_cast<const void*>(target),sizeof(bytes));}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    int32_t actual=0;std::memcpy(&actual,bytes+1,4);
    return bytes[0]==0xE9 && actual==displacement &&
           std::memcmp(bytes+5,entry.prologue+5,8)==0;
}

bool ensureInstalled(uintptr_t base) noexcept {
    if(g_entries[0].ready.load(std::memory_order_acquire) &&
       g_entries[1].ready.load(std::memory_order_acquire))return true;
    for(HookEntry& entry:g_entries) {
        if(entry.ready.load(std::memory_order_acquire))continue;
        if(!installOne(entry,base)) {
            // A target that cannot be hooked stands down alone; CodeHook
            // logs the reason under the target's own name, and
            // calls==0 with installed status is the stand-down signature.
        }
    }
    return true;
}

} // namespace

bool schedulerStackHooksMatch(uintptr_t base) noexcept {
    return base && patchIsOurs(g_entries[0],base) && patchIsOurs(g_entries[1],base);
}

// Defined below; used by both attach/detach pairs.
void recomputeRelayGateLocked() noexcept;

const char* attachSchedulerStackHooks(SchedulerStackProbe* probe) noexcept {
    if(!probe)return "install_failed";
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        auto* active=observer.load(std::memory_order_acquire);
        if(active && active!=probe)return "observer_busy";
        const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if(!base)return "identity_mismatch";
        // Validation is part of the locked install operation (the kinematic
        // probe's 2026-09-20 review finding 5: an unlocked pre-check raced a
        // concurrent install's half-published patch).
        if(!targetValid(base))return "identity_mismatch";
        if(!ensureInstalled(base))return "install_failed";
        if(!schedulerStackHooksMatch(base))return "opcode_mismatch";
        // Targets 0/1 are observed through the kinematic eval hook's job
        // relays; hold that gate open (it may already be open for the probe
        // or the emit -- the gate is recomputed from every consumer's cell).
        const char* feed=kinematicEvalSchedulerAttach();
        if(std::strcmp(feed,"installed")!=0)return feed;
        observer.store(probe,std::memory_order_release);
        recomputeRelayGateLocked();
        return "installed";
    } catch(...) {return "install_failed";}
}

void detachSchedulerStackHooks(SchedulerStackProbe* probe) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        if(observer.load(std::memory_order_acquire)!=probe)return;
        observer.store(nullptr,std::memory_order_release);
        recomputeRelayGateLocked();
        kinematicEvalSchedulerDetach();
    } catch(...) {}
}

// Recomputed under the install mutex from BOTH consumer cells, exactly like
// the kinematic eval hook's recomputeGateLocked: every attach/detach
// transition holds this mutex, so an unlocked check-then-set cannot race
// (finding 4 over there applies verbatim here now that a second consumer
// exists).
void recomputeRelayGateLocked() noexcept {
    const bool open=observer.load(std::memory_order_acquire)!=nullptr ||
                    gateWanted.load(std::memory_order_acquire);
    relayGate.store(open?uintptr_t(1):uintptr_t(0),std::memory_order_release);
}

void schedulerStackSetResetObserver(SchedulerResetObserverFn fn) noexcept {
    resetObserver.store(fn,std::memory_order_release);
}

const char* schedulerStackGateAttach() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if(!base)return "identity_mismatch";
        if(!targetValid(base))return "identity_mismatch";
        if(!ensureInstalled(base))return "install_failed";
        if(!schedulerStackHooksMatch(base))return "opcode_mismatch";
        gateWanted.store(true,std::memory_order_release);
        recomputeRelayGateLocked();
        return "installed";
    } catch(...) {return "install_failed";}
}

void schedulerStackGateDetach() noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        gateWanted.store(false,std::memory_order_release);
        recomputeRelayGateLocked();
    } catch(...) {}
}

void schedulerStackNoteJobEntry(uint32_t target,uintptr_t entryRsp) noexcept {
    SchedulerStackProbe* probe=observer.load(std::memory_order_acquire);
    if(probe)probe->noteEntry(target,entryRsp);
}

} // namespace edvr
