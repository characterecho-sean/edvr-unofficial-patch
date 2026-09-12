#include "../../src/openvr/compat/openvr_v0_9_20.h"
#include "fixture.h"
#include <windows.h>
#include <cstring>
static ExportFixtureStats g_stats;
static uint32_t g_token = 40;
static int g_interface = 7;
extern "C" __declspec(dllexport) const ExportFixtureStats* __cdecl edvrExportFixtureStats() { return &g_stats; }
extern "C" __declspec(dllexport) uint32_t __cdecl VR_InitInternal(vr::EVRInitError* e, vr::EVRApplicationType app) {
    ++g_stats.calls[0]; g_stats.lastApplication = app;
    if (!e) ++g_stats.nullErrors;
    if (e && e != reinterpret_cast<vr::EVRInitError*>(1))
        *e = app == vr::VRApplication_Scene ? vr::VRInitError_None : vr::VRInitError_Unknown;
    return app == vr::VRApplication_Scene ? ++g_token : 0;
}
extern "C" __declspec(dllexport) void __cdecl VR_ShutdownInternal() { ++g_stats.calls[1]; }
extern "C" __declspec(dllexport) bool __cdecl VR_IsInterfaceVersionValid(const char* v) {
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_stats.calls[3]));
    return v && v != reinterpret_cast<const char*>(1) && std::strcmp(v, "EDVRTest_001") == 0;
}
extern "C" __declspec(dllexport) uint32_t __cdecl VR_GetInitToken() { ++g_stats.calls[4]; return g_token; }
extern "C" __declspec(dllexport) void* __cdecl VR_GetGenericInterface(const char* v, vr::EVRInitError* e) {
    ++g_stats.calls[2];
    const bool found = v && std::strcmp(v, "EDVRTest_001") == 0;
    if (e) *e = found ? vr::VRInitError_None : vr::VRInitError_Init_InterfaceNotFound;
    return found ? &g_interface : nullptr; // Unsupported name bypasses production vtable hooks.
}
#ifdef EDVR_REENTRY_FIXTURE
BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        const HMODULE proxy = GetModuleHandleW(L"openvr_api.dll");
        auto token = reinterpret_cast<uint32_t (__cdecl*)()>(GetProcAddress(proxy, "VR_GetInitToken"));
        if (token) { ++g_stats.reentryCalls; g_stats.reentryResult = token(); }
    }
    return TRUE;
}
#endif
