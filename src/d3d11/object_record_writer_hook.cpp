#include "object_record_writer_hook.h"
#include "object_record_writer_probe.h"
#include "../common/code_hook.h"
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstring>
#include <mutex>

namespace edvr {
namespace {
using Lookup = uintptr_t (__fastcall*)(uintptr_t,uintptr_t);
alignas(8) std::atomic<ObjectRecordWriterProbe*> observer{nullptr};
std::atomic<Lookup> original{nullptr};
std::atomic<uint32_t> observersInFlight{0};
static_assert(sizeof(observer)==8 && decltype(observer)::is_always_lock_free,
              "The x64 relay reads the aligned atomic pointer directly.");

struct Observing {
    Observing(){observersInFlight.fetch_add(1,std::memory_order_seq_cst);}
    ~Observing(){observersInFlight.fetch_sub(1,std::memory_order_seq_cst);}
};

// The observer never runs from inside the relocated prologue. Consequently the
// captured stack consists of ordinary compiled frames with valid unwind data.
// Do not hold an observer reference over the original game call: it may enter
// D3D, whose classification lock can be held by the thread ending a capture.
__declspec(noinline) uintptr_t __fastcall lookupObserved(uintptr_t dictionary,uintptr_t key) {
    ObjectRecordWriterProbe::Pending pending{};
    ObjectRecordWriterProbe* started=nullptr;
    {
        Observing guard;
        started=observer.load(std::memory_order_seq_cst);
        if(started) {
            const uintptr_t caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
            CONTEXT context{};
            RtlCaptureContext(&context);
            pending=started->beginLookup(caller,dictionary,key,context);
        }
    }
    const auto forward=original.load(std::memory_order_acquire);
    const uintptr_t entry=forward(dictionary,key);
    if(started && pending.record!=ObjectRecordWriterProbe::kNone) {
        Observing guard;
        auto* current=observer.load(std::memory_order_seq_cst);
        if(current==started)current->completeLookup(pending,entry);
    }
    return entry;
}

constexpr size_t kRelayBytes=44,kOriginalLiteral=36;
struct PreparedRelay {
    uint8_t* bytes=nullptr;
    std::atomic<Lookup>* forward=nullptr;
};
struct HookState {
    std::mutex mutex;
    CodeHook hook;
    PreparedRelay relay;
    uintptr_t target=0;
    std::atomic<bool> ready{false};
};
std::atomic<HookState*> publishedState{nullptr};
std::mutex installMutex;

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
    // original: jmp [trampoline]. RAX/flags are volatile, and lookup consumes
    // only RCX/RDX. No stack adjustment or nonvolatile modification occurs.
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
    auto& relay=*static_cast<PreparedRelay*>(context);
    const uintptr_t address=reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(relay.bytes+kOriginalLiteral,&address,8);
    DWORD oldProtect=0;
    if(!VirtualProtect(relay.bytes,4096,PAGE_EXECUTE_READ,&oldProtect) ||
       !FlushInstructionCache(GetCurrentProcess(),relay.bytes,kRelayBytes))return false;
    relay.forward->store(reinterpret_cast<Lookup>(trampoline),std::memory_order_release);
    return true;
}

bool matchesBytes(const HookState* state) noexcept {
    if(!state || !state->ready.load(std::memory_order_acquire))return false;
    const uintptr_t target=state->target;
    const intptr_t displacement=reinterpret_cast<intptr_t>(state->relay.bytes)-intptr_t(target+5);
    const uint8_t tail[11]={0x48,0x89,0x6C,0x24,0x18,0x56,0x57,0x41,0x56,0x48,0x83};
    uint8_t bytes[16]{};
    __try {std::memcpy(bytes,reinterpret_cast<const void*>(target),sizeof(bytes));}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    int32_t actual=0;std::memcpy(&actual,bytes+1,4);
    return bytes[0]==0xE9 && actual==displacement && std::memcmp(bytes+5,tail,sizeof(tail))==0;
}
}

bool objectRecordWriterHookMatches(uintptr_t target) noexcept {
    const auto* state=publishedState.load(std::memory_order_acquire);
    return state && state->target==target && matchesBytes(state);
}

const char* attachObjectRecordWriterHook(ObjectRecordWriterProbe* probe) noexcept {
    if(!probe)return "install_failed";
    try {
        std::lock_guard<std::mutex> lock(installMutex);
        auto* active=observer.load(std::memory_order_seq_cst);
        if(active && active!=probe)return "observer_busy";
        auto* state=publishedState.load(std::memory_order_acquire);
        if(!state) {
            state=new HookState;
            state->target=reinterpret_cast<uintptr_t>(probe->targetAddress());
            if(!state->target){delete state;return "identity_mismatch";}
            state->relay.bytes=allocateRelay(state->target);
            state->relay.forward=&original;
            if(!state->relay.bytes){delete state;return "relay_allocation_failed";}
            buildRelay(state->relay.bytes,&observer,reinterpret_cast<void*>(&lookupObserved));
            if(!state->hook.install(reinterpret_cast<void*>(state->target),state->relay.bytes,nullptr,
                                    "record-writer-lookup",&prepareRelay,&state->relay)) {
                original.store(nullptr,std::memory_order_release);
                VirtualFree(state->relay.bytes,0,MEM_RELEASE);delete state;return "install_failed";
            }
            state->ready.store(true,std::memory_order_release);
            publishedState.store(state,std::memory_order_release);
            // Process lifetime storage: do not free a trampoline which an
            // inactive call may already be executing. Captures only gate it.
        }
        if(!matchesBytes(state))return "opcode_mismatch";
        observer.store(probe,std::memory_order_seq_cst);
        return "installed";
    } catch(...) {return "install_failed";}
}

void detachObjectRecordWriterHook(ObjectRecordWriterProbe* probe) noexcept {
    auto* expected=probe;
    if(!observer.compare_exchange_strong(expected,nullptr,std::memory_order_seq_cst))return;
    while(observersInFlight.load(std::memory_order_seq_cst))SwitchToThread();
}

#ifdef EDVR_RECORD_WRITER_TEST
namespace {
std::atomic<Lookup> testOriginal{nullptr};
uintptr_t __fastcall testObserved(uintptr_t a,uintptr_t b) {return testOriginal.load()(a,b)+1;}
}
unsigned objectRecordWriterHookSelfTest() noexcept {
    unsigned failures=0;
    auto* target=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!target)return 1;
    const uint8_t body[]={0x48,0x89,0x5C,0x24,0x10,0x48,0x8D,0x04,0x11,0xC3};
    std::memcpy(target,body,sizeof(body));DWORD previous=0;
    if(!VirtualProtect(target,4096,PAGE_EXECUTE_READ,&previous)) {VirtualFree(target,0,MEM_RELEASE);return 1;}
    FlushInstructionCache(GetCurrentProcess(),target,sizeof(body));
    alignas(8) std::atomic<uintptr_t> gate{0};
    PreparedRelay relay{allocateRelay(reinterpret_cast<uintptr_t>(target)),&testOriginal};
    if(!relay.bytes){VirtualFree(target,0,MEM_RELEASE);return 1;}
    buildRelay(relay.bytes,&gate,reinterpret_cast<void*>(&testObserved));
    auto call=reinterpret_cast<Lookup>(target);
    if(call(10,23)!=33)++failures;
    CodeHook hook;
    if(!hook.install(target,relay.bytes,nullptr,"record-writer-relay-test",&prepareRelay,&relay))++failures;
    else {
        if(call(10,23)!=33)++failures;
        gate.store(1);if(call(10,23)!=34)++failures;
        gate.store(0);if(call(10,23)!=33)++failures;
        hook.uninstall();if(call(10,23)!=33 || std::memcmp(target,body,sizeof(body)))++failures;
    }
    testOriginal.store(nullptr);VirtualFree(relay.bytes,0,MEM_RELEASE);VirtualFree(target,0,MEM_RELEASE);
    return failures;
}
#endif
}
