// Standalone desk test for the historical OpenVR ABI used by Elite.
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <windows.h>
#include "../../src/openvr/compat/openvr_v0_9_20.h"
#include "../../src/openvr/compat/openvr_abi_manifest.h"
#include "../../src/openvr/compat/openvr_call_census.h"
#include "../../src/openvr/compat/openvr_forwarding.h"

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

static unsigned g_events = 0;
static edvr::openvr_abi::Event g_last{};
static void census_sink(const edvr::openvr_abi::Event& event) noexcept {
    ++g_events;
    g_last = event;
    if (event.origin == edvr::openvr_abi::Origin::Game && event.slot == 1)
        std::printf("census: %s slot %u game\n", event.interface_name, event.slot);
}

struct ProbeSystem : edvr::openvr_abi::OpenVRSystemForward {
    EVREye seenEye = Eye_Left;
    float nearZ = 0, farZ = 0;
    EGraphicsAPIConvention convention = API_OpenGL;
    ProbeSystem() noexcept : OpenVRSystemForward(nullptr, nullptr) {}
    HmdMatrix44_t GetProjectionMatrix(EVREye eye, float nearValue, float farValue, EGraphicsAPIConvention api) override {
        seenEye = eye; nearZ = nearValue; farZ = farValue; convention = api;
        HmdMatrix44_t m = {};
        m.m[0][0] = 42.0f;
        m.m[3][3] = 17.0f;
        return m;
    }
    HmdMatrix34_t GetEyeToHeadTransform(EVREye eye) override {
        HmdMatrix34_t m{};
        m.m[0][3] = eye == Eye_Right ? 0.03f : -0.03f;
        m.m[2][2] = 1;
        return m;
    }
    void GetProjectionRaw(EVREye, float* l, float* r, float* t, float* b) override {
        *l = -1.0f; *r = 2.0f; *t = 3.0f; *b = -4.0f;
    }
};
struct ProbeCompositor : edvr::openvr_abi::OpenVRCompositorForward {
    EVREye eye = Eye_Left;
    const Texture_t* texture = nullptr;
    const VRTextureBounds_t* bounds = nullptr;
    EVRSubmitFlags flags = Submit_Default;
    ProbeCompositor() : OpenVRCompositorForward(nullptr, nullptr) {}
    EVRCompositorError Submit(EVREye e, const Texture_t* t, const VRTextureBounds_t* b, EVRSubmitFlags f) override {
        eye = e; texture = t; bounds = b; flags = f;
        return VRCompositorError_InvalidTexture;
    }
};
static edvr::openvr_abi::Origin fake_origin(const void* caller) noexcept {
    return caller ? edvr::openvr_abi::Origin::Game : edvr::openvr_abi::Origin::Unknown;
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

    edvr::openvr_abi::Census census;
    if (census.enabled()) return 3;
    census.enable(&census_sink);
    if (!census.enabled()) return 4;
    census.observe(kMethods[1], edvr::openvr_abi::Origin::Game);
    census.observe(kMethods[45], edvr::openvr_abi::Origin::Edvr);
    if (g_events != 2) return 6;
    if (census.enabled() == false) return 7;
    census.disable();
    if (census.enabled()) return 8;
    census.observe(kMethods[0], edvr::openvr_abi::Origin::Game);
    if (g_events != 2) return 9;

    ProbeSystem probe;
    edvr::openvr_abi::Census forwarding_census;
    forwarding_census.enable(&census_sink);
    forwarding_census.set_origin_resolver(&fake_origin);
    edvr::openvr_abi::OpenVRSystemForward forwarding(&probe, &forwarding_census);
    const unsigned before = g_events;
    HmdMatrix44_t matrix = forwarding.GetProjectionMatrix(Eye_Right, 0.1f, 1000.0f, API_DirectX);
    if (matrix.m[0][0] != 42.0f || matrix.m[3][3] != 17 || probe.seenEye != Eye_Right ||
        probe.nearZ != 0.1f || probe.farZ != 1000 || probe.convention != API_DirectX) return 10;
    if (g_events != before + 1 || g_last.slot != 1 || g_last.origin != edvr::openvr_abi::Origin::Game) return 14;
    float raw[] = {99, 99, 99, 99, 99, 99};
    forwarding.GetProjectionRaw(Eye_Left, raw + 1, raw + 2, raw + 3, raw + 4);
    if (raw[0] != 99 || raw[1] != -1 || raw[2] != 2 || raw[3] != 3 || raw[4] != -4 || raw[5] != 99) return 11;
    const auto eyeMatrix = forwarding.GetEyeToHeadTransform(Eye_Right);
    if (eyeMatrix.m[0][3] != 0.03f || eyeMatrix.m[2][2] != 1) return 15;
    forwarding_census.disable();
    const unsigned disabledCount = g_events;
    forwarding.GetEyeToHeadTransform(Eye_Left);
    if (g_events != disabledCount) return 16;
    ProbeCompositor compositor;
    edvr::openvr_abi::OpenVRCompositorForward submit(&compositor, &forwarding_census);
    Texture_t texture{};
    VRTextureBounds_t bounds{0.5f, 1.0f, 1.0f, 0.0f}; // Double-wide half with V flipped.
    for (auto eye : {Eye_Right, Eye_Left, Eye_Left, Eye_Right}) {
        if (submit.Submit(eye, &texture, &bounds, Submit_LensDistortionAlreadyApplied) != VRCompositorError_InvalidTexture ||
            compositor.eye != eye || compositor.texture != &texture || compositor.bounds != &bounds ||
            compositor.flags != Submit_LensDistortionAlreadyApplied) return 17;
    }

    edvr::openvr_abi::ForwardingCache cache;
    edvr::openvr_abi::OpenVRSystemForward targets[5] = {
        {nullptr, nullptr}, {nullptr, nullptr}, {nullptr, nullptr},
        {nullptr, nullptr}, {nullptr, nullptr}
    };
    auto* w0 = cache.wrapSystem(&targets[0]);
    if (!w0 || cache.wrapSystem(&targets[0]) != w0) return 12;
    if (!cache.wrapSystem(&targets[1]) || !cache.wrapSystem(&targets[2]) ||
        !cache.wrapSystem(&targets[3]) || cache.wrapSystem(&targets[4]) != nullptr)
        return 13;
    std::puts("openvr ABI census: 84 slots, four support entries, ABI checks OK");
    return 0;
}
