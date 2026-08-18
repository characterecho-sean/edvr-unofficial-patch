// A stand-in for the game's openvr_api.dll, for testing the proxy's startup.
//
// Two jobs.
//
// 1. Have a DllMain that does real work, so "the proxy loaded and nothing hung"
//    means something. It allocates, touches the CRT, and calls LoadLibrary
//    itself -- the nested case.
//
//    It does NOT try to deadlock on purpose. The obvious way to write that --
//    create a thread and wait for it -- deadlocks under ANY LoadLibrary, not
//    just a nested one, because DllMain always runs holding the loader lock and
//    a new thread cannot finish its attach notifications until that lock is
//    free. Such a DLL is simply broken, and a test built on one proves nothing
//    about the proxy. The assertion that actually catches the bug is the second
//    one in openvr_smoke: after loading the proxy, this module must not be
//    mapped yet.
//
// 2. One export takes arguments in every register class a thunk has to leave
//    alone, plus two on the stack, and returns a value derived from all of
//    them. The proxy's lazy path saves and restores rcx/rdx/r8/r9 and
//    xmm0-xmm3 around its initialiser; if it got any of that wrong, the
//    checksum comes back different.
//
// Signatures here are ours, not OpenVR's. The proxy forwards by jumping, so it
// never looks at them -- which is precisely why the argument check has to be
// done through a real call.
#include <windows.h>

#include <cstdlib>
#include <cstring>

#include "fake_system_012.h"

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Fault on demand, so the proxy's lazy-load path can be tested for the
        // thing that matters more than whether it works: whether it stays
        // catchable when it does not.
        //
        // The proxy loads this DLL from inside a thunk, through an assembly shim
        // that moves the stack pointer. If that shim has no unwind info, an
        // exception raised here cannot be unwound past it and every SEH handler
        // above -- including the game's -- is skipped, turning a recoverable
        // fault into a hard process kill. That shipped once.
        char buf[8]{};
        if (GetEnvironmentVariableA("EDVR_FAKEVR_FAULT", buf, sizeof(buf)) && buf[0] == '1') {
            volatile int* boom = nullptr;
            *boom = 1;
        }
        void* p = malloc(64 * 1024);
        if (p) {
            memset(p, 0xA5, 64 * 1024);
            free(p);
        }
        // Nested LoadLibrary. Safe because this one is already mapped, which is
        // exactly the distinction the proxy's own comments turn on.
        HMODULE m = LoadLibraryW(L"kernel32.dll");
        if (m) FreeLibrary(m);
    }
    return TRUE;
}

// A real IVRSystem_012 stand-in, implemented with ordinary C++ virtuals so
// every call into it is a genuine member-convention call site -- the same
// shape the game's compiled calls have. The proxy hooks this object's vtable
// in place; openvr_smoke then asserts that self, every argument and both
// by-value struct returns cross the hook unchanged, which is the end-to-end
// test of the observation hook's one real hazard (the calling-convention
// split on struct returns, EVIDENCE 6bo).
//
// Values come from fake_system_012.h, shared with the asserting side.
namespace {

struct FakeSystem : fakevr::ISystem012 {
    void GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) override {
        if (w) *w = fakevr::kSizeW;
        if (h) *h = fakevr::kSizeH;
    }
    fakevr::M44 GetProjectionMatrix(int32_t eye, float nearZ, float farZ,
                                    int32_t projType) override {
        return fakevr::expectedMatrix(eye, nearZ, farZ, projType);
    }
    void GetProjectionRaw(int32_t eye, float* l, float* r, float* t,
                          float* b) override {
        float v[4];
        fakevr::expectedRaw(eye, v);
        if (l) *l = v[0];
        if (r) *r = v[1];
        if (t) *t = v[2];
        if (b) *b = v[3];
    }
    int32_t Filler3() override { return 3; }
    fakevr::M34 GetEyeToHeadTransform(int32_t eye) override {
        return fakevr::expectedEyeToHead(eye);
    }
    int32_t Filler5() override { return 5; }
    int32_t Filler6() override { return 6; }
    int32_t Filler7() override { return 7; }
};

FakeSystem g_system;
// A SECOND object of the same class: in-place patching hooks the class, so
// calls through this one reach the same thunks with a self the hook did NOT
// attach to -- the forward-untouched case, asserted by the smoke test.
FakeSystem g_secondSystem;

}  // namespace

extern "C" {

// Reached by openvr_smoke through GetModuleHandle on this module directly --
// they are not part of the real openvr surface, so the proxy neither knows
// nor forwards them.
__declspec(dllexport) void* VR_FakeSystemPtr() { return &g_system; }
__declspec(dllexport) void* VR_FakeSecondSystemPtr() { return &g_secondSystem; }

// The argument-integrity probe. Mixed integer and floating point so the value
// lands in rcx, xmm1, r8, xmm3 and then the stack.
__declspec(dllexport) unsigned long long VR_IsHmdPresent(unsigned long long a, double b,
                                                         unsigned long long c, double d,
                                                         unsigned long long e,
                                                         unsigned long long f) {
    return a + static_cast<unsigned long long>(b) * 10ull +
           c * 100ull + static_cast<unsigned long long>(d) * 1000ull +
           e * 10000ull + f * 100000ull;
}

__declspec(dllexport) unsigned long long VR_GetInitToken() { return 0xABCDEF01ull; }

// The rest exist so the proxy's generated table resolves completely; a missing
// one would be substituted with the stub and the "did every export resolve"
// count would be non-zero for a reason unrelated to what is being tested.
__declspec(dllexport) void VRControlPanel() {}
__declspec(dllexport) void VRDashboardManager() {}
__declspec(dllexport) void VRTrackedCamera() {}
__declspec(dllexport) void VR_GetStringForHmdError() {}
__declspec(dllexport) void VR_GetVRInitErrorAsEnglishDescription() {}
__declspec(dllexport) void VR_GetVRInitErrorAsSymbol() {}
__declspec(dllexport) void VR_InitInternal() {}
__declspec(dllexport) void VR_IsInterfaceVersionValid() {}
__declspec(dllexport) void VR_IsRuntimeInstalled() {}
__declspec(dllexport) void VR_RuntimePath() {}
__declspec(dllexport) void VR_ShutdownInternal() {}
__declspec(dllexport) void* VR_GetGenericInterface(const char* version, int* err) {
    if (version && strcmp(version, "IVRSystem_012") == 0) {
        if (err) *err = 0;
        return &g_system;
    }
    if (err) *err = 105;  // VRInitError_Init_InterfaceNotFound
    return nullptr;
}

}  // extern "C"
