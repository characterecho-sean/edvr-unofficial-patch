#pragma once
#include "../common/game_call_probe.h"
#include "../common/log.h"
#include "../openxr/native_module.h"

namespace edvr {
// Independent budgets preserve post-stop observations even after a long flight.
// Phases come from the existing native CPU status export, not module presence.
struct GameExitProbeSchedule {
    GameCallProbeBudget running, stopped;
    unsigned take(uint64_t now,unsigned phase) noexcept {
        if(phase==EDVR_NATIVE_RUNNING)return running.take(now);
        if(phase==EDVR_NATIVE_STOPPED || phase==EDVR_NATIVE_RETAINED)return stopped.take(now);
        return 0;
    }
};
inline void gameExitProbePresent(HRESULT presentResult,UINT flags,uint64_t frame) noexcept {
    static std::atomic_flag lock=ATOMIC_FLAG_INIT;
    if(lock.test_and_set(std::memory_order_acquire))return;
    struct Unlock { std::atomic_flag& value; ~Unlock(){value.clear(std::memory_order_release);} } unlock{lock};
    static uint64_t lastPoll=0;
    static bool polled=false, armed=false, complete=false;
    static GameExitProbeSchedule schedule;
    const uint64_t now=GetTickCount64();
    if(complete || (polled && (now<lastPoll || now-lastPoll<1000)))return;
    polled=true;lastPoll=now;
    if(!armed) {
        armed=true;
        Log::get().note("game exit probe: armed; native CPU status checked at most once per second after owned Present, independent running/stopped stack budgets, no process termination.");
    }
    HMODULE module=nullptr;
    if(!GetModuleHandleExW(0,L"openvr_api.dll",&module))return;
    const auto statusFn=reinterpret_cast<decltype(&edvrGetNativeRuntimeStatus)>(GetProcAddress(module,"edvrGetNativeRuntimeStatus"));
    EdvrNativeRuntimeStatus status{sizeof(status),EDVR_NATIVE_MODULE_VERSION_1};
    const HRESULT result=statusFn?statusFn(&status):E_NOINTERFACE;
    FreeLibrary(module); // balanced transient reference; never loads a runtime
    if(result!=S_OK || status.size!=sizeof(status) || status.version!=EDVR_NATIVE_MODULE_VERSION_1)return;
    const unsigned sample=schedule.take(now,status.phase);if(!sample)return;
    const auto stack=captureGameCallStack();
    Log::get().note("game exit present: phase=%u attempt=%u sample=%u/16 frame=%llu present=0x%08X flags=%u cleanup=%u retained=%u shutdown_thread=%u stack_frames=%u game_frames=%u game_rvas=%s.",
        status.phase,status.initAttempts,sample,static_cast<unsigned long long>(frame),unsigned(presentResult),flags,
        status.cleanup,status.retained,status.shutdownThread,stack.captured,stack.gameFrames,stack.rvas);
    if(status.phase!=EDVR_NATIVE_RUNNING && sample==16)complete=true;
}
}
