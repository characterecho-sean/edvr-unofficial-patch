#include <windows.h>
#include <atomic>
#include <cstring>
#include "call_census.h"
#include "compat/openvr_forwarding.h"
#include "../common/vr_census.h"

namespace edvr {
namespace {
using namespace openvr_abi;
Census g_census;
ForwardingCache g_cache(&g_census);
HMODULE g_game = nullptr, g_self = nullptr, g_gfx = nullptr;
std::atomic<uint32_t> g_counts[kMethodCount][4]{};
Origin callerOrigin(const void* address) noexcept {
    HMODULE module = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module)) return Origin::Unknown;
    if (module == g_game) return Origin::Game;
    if (module == g_self || module == g_gfx) return Origin::Edvr;
    return Origin::Injected; // Logged as other-module; no guess about its purpose.
}
void observe(const Event& event) noexcept {
    try {
        size_t index = kMethodCount;
        for (size_t i = 0; i < kMethodCount; ++i)
            if (kMethods[i].slot == event.slot && !std::strcmp(kMethods[i].interface_name, event.interface_name)) { index = i; break; }
        const unsigned origin = static_cast<unsigned>(event.origin);
        if (index == kMethodCount || origin >= 4) return;
        auto& count = g_counts[index][origin];
        uint32_t n = count.load(std::memory_order_relaxed);
        do { if (n >= 4) return; }
        while (!count.compare_exchange_weak(n, n + 1, std::memory_order_relaxed));
        static const char* origins[] = {"unknown", "game-exe", "edvr", "other-module"};
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        Log::get().note("VR ABI census: qpc=%lld thread=%lu origin=%s interface=%s slot=%u signature=%s sample=%u/4",
            qpc.QuadPart, GetCurrentThreadId(), origins[origin], event.interface_name,
            event.slot, kMethods[index].signature, n + 1);
    } catch (...) { /* Diagnostics must not change the forwarded method result. */ }
}
}
void configureCallCensus() {
    if (!vrCensusEnabled()) return;
    g_game = GetModuleHandleW(nullptr);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&configureCallCensus), &g_self);
    g_gfx = GetModuleHandleW(L"d3d11.dll");
    if (g_gfx && !GetProcAddress(g_gfx, "edvrDoorGpuBegin")) g_gfx = nullptr;
    g_census.set_origin_resolver(&callerOrigin);
    g_census.enable(&observe);
    Log::get().note("VR ABI census enabled: 84 exact v0.9.20 methods, first four calls per method/caller category. "
        "Internal direct runtime calls bypass these wrappers. Unsupported versions pass through; no absence claim.");
}
void* wrapCensusInterface(void* original, const char* version) {
    if (!g_census.enabled() || !original || !version) return original;
    void* wrapped = nullptr;
    if (!std::strcmp(version, vr::IVRSystem_Version)) wrapped = g_cache.wrapSystem(static_cast<vr::IVRSystem*>(original));
    else if (!std::strcmp(version, vr::IVRCompositor_Version)) wrapped = g_cache.wrapCompositor(static_cast<vr::IVRCompositor*>(original));
    else if (!std::strcmp(version, vr::IVRChaperone_Version)) wrapped = g_cache.wrapChaperone(static_cast<vr::IVRChaperone*>(original));
    else if (!std::strcmp(version, vr::IVRExtendedDisplay_Version)) wrapped = g_cache.wrapExtendedDisplay(static_cast<vr::IVRExtendedDisplay*>(original));
    else return original;
    if (wrapped) return wrapped;
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) Log::get().note("VR ABI census cache exhausted: returning original interface, coverage incomplete.");
    return original;
}
}
