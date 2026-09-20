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

KinematicEvalProbe* g_probe=nullptr;
EvalFn g_evalOriginal=nullptr;
JobFn g_jobOriginal[KinematicEvalProbe::kJobCount]={};
CodeHook g_hooks[KinematicEvalProbe::kJobCount+1];
bool g_installed=false;
std::mutex g_installMutex;
int64_t g_qpcFreq=0;

// Job bodies behind the run thunks (recovered from the hash-verified exe,
// SHA-256 e6be8bbe...; thunk jmps decoded 2026-09-19):
const uintptr_t kJobRvas[KinematicEvalProbe::kJobCount]={
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

uintptr_t __fastcall bracket(uint32_t job,JobFn original,
                             uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) noexcept {
    if(!original)return 0; // install stood down; the wrapper is unreachable then, but never chase a null
    KinematicEvalProbe* probe=g_probe;
    if(!probe || !probe->active())return original(a,b,c,d);
    const int64_t start=qpcNow();
    const uintptr_t result=original(a,b,c,d);
    const int64_t elapsed=qpcNow()-start;
    if(elapsed>0 && g_qpcFreq>0) {
        auto* stats=probe->jobStats();
        const uint64_t ns=uint64_t(elapsed)*1000000000ull/uint64_t(g_qpcFreq);
        stats[job].calls.fetch_add(1,std::memory_order_relaxed);
        stats[job].totalNs.fetch_add(ns,std::memory_order_relaxed);
        uint64_t prev=stats[job].maxNs.load(std::memory_order_relaxed);
        while(prev<ns && !stats[job].maxNs.compare_exchange_weak(prev,ns,std::memory_order_relaxed)){}
    }
    return result;
}

__declspec(noinline) uintptr_t __fastcall evalObserved(uintptr_t descriptor,uintptr_t param2,
                                                       uintptr_t renderRecord) noexcept {
    KinematicEvalProbe* probe=g_probe;
    if(probe && probe->active())probe->observe(descriptor,renderRecord);
    return g_evalOriginal(descriptor,param2,renderRecord);
}

#define EDVR_JOB_WRAPPER(i) \
    __declspec(noinline) uintptr_t __fastcall job##i(uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) noexcept { \
        return bracket(i,g_jobOriginal[i],a,b,c,d); \
    }
EDVR_JOB_WRAPPER(0) EDVR_JOB_WRAPPER(1) EDVR_JOB_WRAPPER(2)
EDVR_JOB_WRAPPER(3) EDVR_JOB_WRAPPER(4) EDVR_JOB_WRAPPER(5)
#undef EDVR_JOB_WRAPPER

void* const kJobReplacements[KinematicEvalProbe::kJobCount]={
    reinterpret_cast<void*>(&job0),reinterpret_cast<void*>(&job1),
    reinterpret_cast<void*>(&job2),reinterpret_cast<void*>(&job3),
    reinterpret_cast<void*>(&job4),reinterpret_cast<void*>(&job5),
};

bool patchIsOurs(uintptr_t target,void* replacement) noexcept {
    uint8_t bytes[5]{};
    __try {std::memcpy(bytes,reinterpret_cast<const void*>(target),sizeof(bytes));}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    if(bytes[0]!=0xE9)return false;
    int32_t disp=0;std::memcpy(&disp,bytes+1,4);
    return target+5+disp==reinterpret_cast<uintptr_t>(replacement);
}

} // namespace

bool kinematicEvalHooksMatch(uintptr_t evalTarget) noexcept {
    return g_installed && patchIsOurs(evalTarget,reinterpret_cast<void*>(&evalObserved));
}

const char* attachKinematicEvalHooks(KinematicEvalProbe* probe) noexcept {
    if(!probe)return "install_failed";
    try {
        std::lock_guard<std::mutex> lock(g_installMutex);
        if(!g_installed) {
            const uintptr_t base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            if(!base)return "identity_mismatch";
            LARGE_INTEGER freq{};
            if(!QueryPerformanceFrequency(&freq)||freq.QuadPart<=0)return "install_failed";
            g_qpcFreq=freq.QuadPart;
            g_probe=probe;
            if(!g_hooks[0].install(reinterpret_cast<void*>(base+KinematicEvalProbe::kEvalRva),
                                   reinterpret_cast<void*>(&evalObserved),
                                   reinterpret_cast<void**>(&g_evalOriginal),
                                   "kinematic-eval")) {
                g_probe=nullptr;return "install_failed";
            }
            if(!patchIsOurs(base+KinematicEvalProbe::kEvalRva,
                            reinterpret_cast<void*>(&evalObserved)))return "opcode_mismatch";
            for(uint32_t i=0;i<KinematicEvalProbe::kJobCount;++i) {
                if(!g_hooks[i+1].install(reinterpret_cast<void*>(base+kJobRvas[i]),
                                         kJobReplacements[i],
                                         reinterpret_cast<void**>(&g_jobOriginal[i]),
                                         "kinematic-job")) {
                    // A job that cannot be hooked stands down alone; the eval
                    // capture is the flight's primary evidence.
                    g_jobOriginal[i]=nullptr;
                }
            }
            g_installed=true;
            return "installed";
        }
        if(!patchIsOurs(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+
                        KinematicEvalProbe::kEvalRva,
                        reinterpret_cast<void*>(&evalObserved)))return "opcode_mismatch";
        g_probe=probe;
        return "installed";
    } catch(...) {return "install_failed";}
}

} // namespace edvr
