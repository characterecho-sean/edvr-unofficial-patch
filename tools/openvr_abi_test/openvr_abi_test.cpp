// Standalone desk test for the historical OpenVR ABI used by Elite.
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <windows.h>
#include "../../src/openvr/compat/openvr_v0_9_20.h"
#include "../../src/openvr/compat/openvr_abi_manifest.h"

using namespace vr;
using edvr::openvr_abi::kMethods;

static_assert(edvr::openvr_abi::kMethodCount == 84, "the four-interface census must remain exact");
static_assert(sizeof(HmdMatrix34_t) == 48 && sizeof(HmdMatrix44_t) == 64,
              "by-value matrix ABI drifted");
static_assert(sizeof(TrackedDevicePose_t) == 80 && sizeof(Texture_t) == 16,
              "pose or texture packing drifted");
static_assert(sizeof(VRTextureBounds_t) == 16 && sizeof(HmdQuad_t) == 48,
              "output geometry packing drifted");
static_assert(std::is_same<decltype(&IVRSystem::GetProjectionMatrix),
              HmdMatrix44_t (IVRSystem::*)(EVREye, float, float, EGraphicsAPIConvention)>::value,
              "IVRSystem_012 projection signature changed");
static_assert(std::is_same<decltype(&IVRSystem::GetEyeToHeadTransform),
              HmdMatrix34_t (IVRSystem::*)(EVREye)>::value,
              "IVRSystem_012 eye transform signature changed");
static_assert(std::is_same<decltype(&IVRCompositor::Submit),
              EVRCompositorError (IVRCompositor::*)(EVREye, const Texture_t*, const VRTextureBounds_t*, EVRSubmitFlags)>::value,
              "IVRCompositor_014 submit signature changed");

static bool check_slots(const char* name, unsigned expected) {
    unsigned n = 0;
    for (unsigned i = 0; i < edvr::openvr_abi::kMethodCount; ++i)
        if (std::strcmp(kMethods[i].interface_name, name) == 0) {
            if (kMethods[i].slot != n++) return false;
        }
    return n == expected;
}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    const char* supported[] = {"IVRSystem_012", "IVRCompositor_014",
                               "IVRChaperone_003", "IVRExtendedDisplay_001"};
    for (const char* name : supported) {
        unsigned count = (std::strcmp(name, "IVRSystem_012") == 0) ? 44 :
                         (std::strcmp(name, "IVRCompositor_014") == 0) ? 29 : 8;
        if (std::strcmp(name, "IVRExtendedDisplay_001") == 0) count = 3;
        if (!check_slots(name, count)) {
            std::printf("FAIL slot inventory: %s\n", name);
            return 1;
        }
    }
    static_assert(edvr::openvr_abi::kMethodCount == 44 + 29 + 8 + 3,
                  "support table total must remain 84");

    std::puts("openvr ABI census: 84 slots, four support entries, ABI checks OK");
    return 0;
}
