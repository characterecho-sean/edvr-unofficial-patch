#include "scheduler_stack_hook.h"
#include "scheduler_stack_probe.h"
#include "kinematic_eval_hook.h"
#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"
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

// The prologues, from the hash-verified exe (SHA-256 ed5a8f8c...e264b63;
// verified 2026-09-21 against analysis/EliteDangerous64.exe and against
// tools/build_diff_targets.json's prologue_hex for 0x36A3BB0):
//   0x42E2560: mov [rsp+8],rbx; mov [rsp+10h],rsi; push rdi; sub rsp,28h
//   0x36A3BB0: mov [rsp+8],rcx; push rbx; push rsi; push rdi; sub rsp,28h
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
    bool refusalLogged=false; // an unknown build's refusal, said once (under g_installMutex)
};

__declspec(noinline) uintptr_t __fastcall drainObserved(uintptr_t,uintptr_t,
                                                        uintptr_t,uintptr_t) noexcept;
__declspec(noinline) uintptr_t __fastcall resetObserved(uintptr_t,uintptr_t,
                                                        uintptr_t,uintptr_t) noexcept;

HookEntry g_entries[2]={
    {"scheduler-drain-1442e2560",SchedulerStackProbe::kHookRvas[0],kDrainPrologue,
     reinterpret_cast<void*>(&drainObserved)},
    {"scheduler-reset-1436a3bb0",SchedulerStackProbe::kHookRvas[1],kResetPrologue,
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
    const auto forward=reinterpret_cast<ObservedFn>(g_entries[1].forward.load(std::memory_order_acquire));
    if(!forward)return 0;
    return forward(a,b,c,d);
}

std::mutex g_installMutex;

bool patchIsOurs(const HookEntry& entry,uintptr_t base) noexcept;

// An unknown build (issue 124): says once, per entry, why the probe stands down. The known build is as silent as it always was.
void noteUnknownBuildRefusal(HookEntry& entry,uintptr_t found) noexcept {
    if(entry.refusalLogged)return;
    entry.refusalLogged=true;
    if(!found)
        Log::get().note("scheduler stack hook: refused: %s, and %s (0x%llX in build 334925) was not found by signature; "
                        "the scheduler stack probe stands down.",
                        gameBuildText(),entry.name,static_cast<unsigned long long>(entry.rva));
    else
        Log::get().note("scheduler stack hook: refused: %s: %s was found by signature at 0x%llX, but its prologue bytes are not "
                        "build 334925's outside the masked fields; the scheduler stack probe stands down.",
                        gameBuildText(),entry.name,static_cast<unsigned long long>(found));
}

// The scan's upper bound, an RVA. Build 334925: the probe's constant, as it always was. Another build: the end of its own executable
// section, from its PE header (read under __try).
bool scanBoundRva(uintptr_t base,uintptr_t* hi) noexcept {
    if(gameBuildKnown()){*hi=SchedulerStackProbe::kRangeHi;return true;}
    uint8_t headers[4096]{};
    __try {std::memcpy(headers,reinterpret_cast<const void*>(base),sizeof(headers));}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    *hi=SchedulerStackProbe::executableEndRva(headers,sizeof(headers));
    return *hi!=0;
}

bool targetValid(uintptr_t base) noexcept {
    if(!gameBuildIdentity(nullptr,nullptr))return false;
    const bool known=gameBuildKnown();
    __try {
        for(HookEntry& entry:g_entries) {
            const uintptr_t found=gameRva(static_cast<uint32_t>(entry.rva));
            if(!found){if(!known)noteUnknownBuildRefusal(entry,0);return false;}
            uint8_t got[14]{};
            std::memcpy(got,reinterpret_cast<const void*>(base+found),sizeof(got));
            if(!gamePinMatches(static_cast<uint32_t>(entry.rva),got,entry.prologue,sizeof(got)) &&
               !(entry.ready.load(std::memory_order_acquire) && patchIsOurs(entry,base))) {
                if(!known)noteUnknownBuildRefusal(entry,found);
                return false;
            }
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

bool installOne(HookEntry& entry,uintptr_t base) noexcept {
    const uintptr_t found=gameRva(static_cast<uint32_t>(entry.rva));
    if(!found)return false;
    entry.relay=allocateRelay(base+found);
    if(!entry.relay)return false;
    buildRelay(entry.relay,&observer,entry.callback);
    if(!entry.hook.install(reinterpret_cast<void*>(base+found),entry.relay,nullptr,
                           entry.name,&prepareRelay,&entry)) {
        VirtualFree(entry.relay,0,MEM_RELEASE);
        entry.relay=nullptr;
        return false;
    }
    entry.ready.store(true,std::memory_order_release);
    if(!gameBuildKnown())
        Log::get().note("scheduler stack hook: %s (0x%llX in build 334925) was found by signature on %s at 0x%llX and is detoured; "
                        "the scheduler stack probe observes it.",
                        entry.name,static_cast<unsigned long long>(entry.rva),gameBuildText(),
                        static_cast<unsigned long long>(found));
    return true;
    // Process-lifetime storage: do not free a relay or trampoline which an
    // in-flight call may already be executing. Captures only gate it.
}

bool patchIsOurs(const HookEntry& entry,uintptr_t base) noexcept {
    if(!entry.ready.load(std::memory_order_acquire) || !entry.relay)return false;
    const uintptr_t found=gameRva(static_cast<uint32_t>(entry.rva));
    if(!found)return false;
    const uintptr_t target=base+found;
    const intptr_t displacement=reinterpret_cast<intptr_t>(entry.relay)-intptr_t(target+5);
    uint8_t bytes[13]{};
    __try {std::memcpy(bytes,reinterpret_cast<const void*>(target),sizeof(bytes));}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    int32_t actual=0;std::memcpy(&actual,bytes+1,4);
    if(bytes[0]!=0xE9 || actual!=displacement)return false;
    // Our five-byte jump is in place; the rest of the stolen prologue (bytes 5..12) is still the pin's, outside the fields
    // an unknown build's signature masks. The expected row is the jump as read plus the pin's tail.
    uint8_t expected[sizeof(bytes)];
    std::memcpy(expected,bytes,5);
    std::memcpy(expected+5,entry.prologue+5,8);
    return gamePinMatches(static_cast<uint32_t>(entry.rva),bytes,expected,sizeof(bytes));
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
        uintptr_t scanHi=0;
        if(!scanBoundRva(base,&scanHi)) {
            static bool logged=false;
            if(!logged) {
                logged=true;
                Log::get().note("scheduler stack hook: refused: %s, and the end of its executable section could not be read from the "
                                "PE header; the scheduler stack probe stands down.",gameBuildText());
            }
            return "identity_mismatch";
        }
        probe->setScanHi(scanHi);
        if(!ensureInstalled(base))return "install_failed";
        if(!schedulerStackHooksMatch(base))return "opcode_mismatch";
        // Targets 0/1 are observed through the kinematic eval hook's job
        // relays; hold that gate open (it may already be open for the probe
        // or the emit -- the gate is recomputed from every consumer's cell).
        const char* feed=kinematicEvalSchedulerAttach();
        if(std::strcmp(feed,"installed")!=0)return feed;
        observer.store(probe,std::memory_order_release);
        return "installed";
    } catch(...) {return "install_failed";}
}

void detachSchedulerStackHooks(SchedulerStackProbe* probe) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        if(observer.load(std::memory_order_acquire)!=probe)return;
        observer.store(nullptr,std::memory_order_release);
        kinematicEvalSchedulerDetach();
    } catch(...) {}
}

void schedulerStackNoteJobEntry(uint32_t target,uintptr_t entryRsp) noexcept {
    SchedulerStackProbe* probe=observer.load(std::memory_order_acquire);
    if(probe)probe->noteEntry(target,entryRsp);
}

} // namespace edvr
