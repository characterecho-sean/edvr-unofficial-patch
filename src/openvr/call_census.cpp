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
        const unsigned origin=static_cast<unsigned>(event.origin);
        if(origin>=4) return;
        static const char* origins[]={"unknown","game-exe","edvr","other-module"};
        LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc);
        const auto& e=event.evidence;
        // The production Census reserves the record before any output reads.
        // This sink must not apply a second method-wide budget: it would hide
        // later eyes/properties despite their independent evidence claims.
        Log::get().note("VR ABI census: record=%u qpc=%lld thread=%lu origin=%s interface=%s slot=%u "
            "signature=%s flags=0x%X schema=[%s] u_mask=0x%X u=%u,%u,%u,%u,%u,%u,%u,%u "
            "i_mask=0x%X i=%d,%d,%d,%d f_mask=0x%X f=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g "
            "u64_valid=%u u64=%llu bytes=%u",
            event.record,qpc.QuadPart,GetCurrentThreadId(),origins[origin],event.interface_name,event.slot,
            event.signature,e.flags,e.schema,e.uMask,e.u32[0],e.u32[1],e.u32[2],e.u32[3],e.u32[4],e.u32[5],e.u32[6],e.u32[7],
            e.iMask,e.i32[0],e.i32[1],e.i32[2],e.i32[3],e.fMask,e.f32[0],e.f32[1],e.f32[2],e.f32[3],e.f32[4],e.f32[5],e.f32[6],e.f32[7],
            unsigned(e.haveU64),static_cast<unsigned long long>(e.u64),e.bytes);
        if(e.flags & EvidenceMatrix44)
            Log::get().note("VR ABI census matrix44: record=%u m=%.9g,%.9g,%.9g,%.9g;%.9g,%.9g,%.9g,%.9g;%.9g,%.9g,%.9g,%.9g;%.9g,%.9g,%.9g,%.9g",
                event.record,e.matrix44[0],e.matrix44[1],e.matrix44[2],e.matrix44[3],e.matrix44[4],e.matrix44[5],e.matrix44[6],e.matrix44[7],e.matrix44[8],e.matrix44[9],e.matrix44[10],e.matrix44[11],e.matrix44[12],e.matrix44[13],e.matrix44[14],e.matrix44[15]);
        if(e.flags & EvidenceMatrix34)
            Log::get().note("VR ABI census matrix34: record=%u m=%.9g,%.9g,%.9g,%.9g;%.9g,%.9g,%.9g,%.9g;%.9g,%.9g,%.9g,%.9g",
                event.record,e.matrix34[0],e.matrix34[1],e.matrix34[2],e.matrix34[3],e.matrix34[4],e.matrix34[5],e.matrix34[6],e.matrix34[7],e.matrix34[8],e.matrix34[9],e.matrix34[10],e.matrix34[11]);
    } catch (...) { /* Diagnostics must not change the forwarded result. */ }
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
    Log::get().note("VR ABI census enabled: 84 exact v0.9.20 methods; up to 16 exact keys per method/caller, four samples per key. Masks identify read fields; property strings are omitted. "
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
