#define VR_API_EXPORT
#include "../../src/openvr/compat/openvr_v0_9_20.h"
#include <windows.h>
#include <cstring>

namespace {
class FakeCompositor final : public vr::IVRCompositor {
public:
    void SetTrackingSpace(vr::ETrackingUniverseOrigin) override {}
    vr::ETrackingUniverseOrigin GetTrackingSpace() override { return vr::TrackingUniverseStanding; }
    vr::EVRCompositorError WaitGetPoses(vr::TrackedDevicePose_t*,uint32_t,vr::TrackedDevicePose_t*,uint32_t) override { return vr::VRCompositorError_None; }
    vr::EVRCompositorError GetLastPoses(vr::TrackedDevicePose_t*,uint32_t,vr::TrackedDevicePose_t*,uint32_t) override { return vr::VRCompositorError_None; }
    vr::EVRCompositorError GetLastPoseForTrackedDeviceIndex(vr::TrackedDeviceIndex_t,vr::TrackedDevicePose_t*,vr::TrackedDevicePose_t*) override { return vr::VRCompositorError_None; }
    vr::EVRCompositorError Submit(vr::EVREye,const vr::Texture_t*,const vr::VRTextureBounds_t*,vr::EVRSubmitFlags) override { return vr::VRCompositorError_None; }
    void ClearLastSubmittedFrame() override {} void PostPresentHandoff() override {}
    bool GetFrameTiming(vr::Compositor_FrameTiming*,uint32_t) override { return false; }
    float GetFrameTimeRemaining() override { return 0.0f; }
    void FadeToColor(float,float,float,float,float,bool) override {} void FadeGrid(float,bool) override {}
    vr::EVRCompositorError SetSkyboxOverride(const vr::Texture_t*,uint32_t) override { return vr::VRCompositorError_None; }
    void ClearSkyboxOverride() override {} void CompositorBringToFront() override {} void CompositorGoToBack() override {}
    void CompositorQuit() override {} bool IsFullscreen() override { return false; } uint32_t GetCurrentSceneFocusProcess() override { return 0; }
    uint32_t GetLastFrameRenderer() override { return 0; } bool CanRenderScene() override { return true; }
    void ShowMirrorWindow() override {} void HideMirrorWindow() override {} bool IsMirrorWindowVisible() override { return false; }
    void CompositorDumpImages() override {} bool ShouldAppRenderWithLowResources() override { return false; }
    void ForceInterleavedReprojectionOn(bool) override {} void ForceReconnectProcess() override {} void SuspendRendering(bool) override {}
};
FakeCompositor g_compositor;
}
extern "C" __declspec(dllexport) void* __cdecl VR_GetGenericInterface(const char* v, vr::EVRInitError* e) {
    if (v && strcmp(v, vr::IVRCompositor_Version) == 0) { if(e)*e=vr::VRInitError_None; return &g_compositor; }
    if(e)*e=vr::VRInitError_Init_InterfaceNotFound; return nullptr;
}
extern "C" __declspec(dllexport) uint32_t __cdecl VR_InitInternal(vr::EVRInitError* e, vr::EVRApplicationType) { if(e)*e=vr::VRInitError_None; return 1; }
extern "C" __declspec(dllexport) void __cdecl VR_ShutdownInternal() {}
extern "C" __declspec(dllexport) bool __cdecl VR_IsInterfaceVersionValid(const char* v) { return v && strcmp(v,vr::IVRCompositor_Version)==0; }
extern "C" __declspec(dllexport) uint32_t __cdecl VR_GetInitToken() { return 1; }
