#pragma once
#include "openvr_call_census.h"
#include <intrin.h>
#include <type_traits>
#include <array>
#include <mutex>
#include <new>

namespace edvr::openvr_abi {
using namespace vr;

class OpenVRSystemForward : public vr::IVRSystem {
public:
    OpenVRSystemForward(vr::IVRSystem* target, Census* census) noexcept : target_(target), census_(census) {}
    void GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight) override { note(0, _ReturnAddress()); target_->GetRecommendedRenderTargetSize(pnWidth, pnHeight); }
    HmdMatrix44_t GetProjectionMatrix(EVREye eEye, float fNearZ, float fFarZ, EGraphicsAPIConvention eProjType) override { note(1, _ReturnAddress()); return target_->GetProjectionMatrix(eEye, fNearZ, fFarZ, eProjType); }
    void GetProjectionRaw(EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom) override { note(2, _ReturnAddress()); target_->GetProjectionRaw(eEye, pfLeft, pfRight, pfTop, pfBottom); }
    DistortionCoordinates_t ComputeDistortion(EVREye eEye, float fU, float fV) override { note(3, _ReturnAddress()); return target_->ComputeDistortion(eEye, fU, fV); }
    HmdMatrix34_t GetEyeToHeadTransform(EVREye eEye) override { note(4, _ReturnAddress()); return target_->GetEyeToHeadTransform(eEye); }
    bool GetTimeSinceLastVsync(float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter) override { note(5, _ReturnAddress()); return target_->GetTimeSinceLastVsync(pfSecondsSinceLastVsync, pulFrameCounter); }
    int32_t GetD3D9AdapterIndex() override { note(6, _ReturnAddress()); return target_->GetD3D9AdapterIndex(); }
    void GetDXGIOutputInfo(int32_t *pnAdapterIndex) override { note(7, _ReturnAddress()); target_->GetDXGIOutputInfo(pnAdapterIndex); }
    bool IsDisplayOnDesktop() override { note(8, _ReturnAddress()); return target_->IsDisplayOnDesktop(); }
    bool SetDisplayVisibility(bool bIsVisibleOnDesktop) override { note(9, _ReturnAddress()); return target_->SetDisplayVisibility(bIsVisibleOnDesktop); }
    void GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin eOrigin, float fPredictedSecondsToPhotonsFromNow, TrackedDevicePose_t *pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount) override { note(10, _ReturnAddress()); target_->GetDeviceToAbsoluteTrackingPose(eOrigin, fPredictedSecondsToPhotonsFromNow, pTrackedDevicePoseArray, unTrackedDevicePoseArrayCount); }
    void ResetSeatedZeroPose() override { note(11, _ReturnAddress()); target_->ResetSeatedZeroPose(); }
    HmdMatrix34_t GetSeatedZeroPoseToStandingAbsoluteTrackingPose() override { note(12, _ReturnAddress()); return target_->GetSeatedZeroPoseToStandingAbsoluteTrackingPose(); }
    HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose() override { note(13, _ReturnAddress()); return target_->GetRawZeroPoseToStandingAbsoluteTrackingPose(); }
    uint32_t GetSortedTrackedDeviceIndicesOfClass(ETrackedDeviceClass eTrackedDeviceClass, vr::TrackedDeviceIndex_t *punTrackedDeviceIndexArray, uint32_t unTrackedDeviceIndexArrayCount, vr::TrackedDeviceIndex_t unRelativeToTrackedDeviceIndex) override { note(14, _ReturnAddress()); return target_->GetSortedTrackedDeviceIndicesOfClass(eTrackedDeviceClass, punTrackedDeviceIndexArray, unTrackedDeviceIndexArrayCount, unRelativeToTrackedDeviceIndex); }
    EDeviceActivityLevel GetTrackedDeviceActivityLevel(vr::TrackedDeviceIndex_t unDeviceId) override { note(15, _ReturnAddress()); return target_->GetTrackedDeviceActivityLevel(unDeviceId); }
    void ApplyTransform(TrackedDevicePose_t *pOutputPose, const TrackedDevicePose_t *pTrackedDevicePose, const HmdMatrix34_t *pTransform) override { note(16, _ReturnAddress()); target_->ApplyTransform(pOutputPose, pTrackedDevicePose, pTransform); }
    vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole unDeviceType) override { note(17, _ReturnAddress()); return target_->GetTrackedDeviceIndexForControllerRole(unDeviceType); }
    vr::ETrackedControllerRole GetControllerRoleForTrackedDeviceIndex(vr::TrackedDeviceIndex_t unDeviceIndex) override { note(18, _ReturnAddress()); return target_->GetControllerRoleForTrackedDeviceIndex(unDeviceIndex); }
    ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t unDeviceIndex) override { note(19, _ReturnAddress()); return target_->GetTrackedDeviceClass(unDeviceIndex); }
    bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t unDeviceIndex) override { note(20, _ReturnAddress()); return target_->IsTrackedDeviceConnected(unDeviceIndex); }
    bool GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override { note(21, _ReturnAddress()); return target_->GetBoolTrackedDeviceProperty(unDeviceIndex, prop, pError); }
    float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override { note(22, _ReturnAddress()); return target_->GetFloatTrackedDeviceProperty(unDeviceIndex, prop, pError); }
    int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override { note(23, _ReturnAddress()); return target_->GetInt32TrackedDeviceProperty(unDeviceIndex, prop, pError); }
    uint64_t GetUint64TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override { note(24, _ReturnAddress()); return target_->GetUint64TrackedDeviceProperty(unDeviceIndex, prop, pError); }
    HmdMatrix34_t GetMatrix34TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override { note(25, _ReturnAddress()); return target_->GetMatrix34TrackedDeviceProperty(unDeviceIndex, prop, pError); }
    uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, char *pchValue, uint32_t unBufferSize, ETrackedPropertyError *pError) override { note(26, _ReturnAddress()); return target_->GetStringTrackedDeviceProperty(unDeviceIndex, prop, pchValue, unBufferSize, pError); }
    const char *GetPropErrorNameFromEnum(ETrackedPropertyError error) override { note(27, _ReturnAddress()); return target_->GetPropErrorNameFromEnum(error); }
    bool PollNextEvent(VREvent_t *pEvent, uint32_t uncbVREvent) override { note(28, _ReturnAddress()); return target_->PollNextEvent(pEvent, uncbVREvent); }
    bool PollNextEventWithPose(ETrackingUniverseOrigin eOrigin, VREvent_t *pEvent, uint32_t uncbVREvent, vr::TrackedDevicePose_t *pTrackedDevicePose) override { note(29, _ReturnAddress()); return target_->PollNextEventWithPose(eOrigin, pEvent, uncbVREvent, pTrackedDevicePose); }
    const char *GetEventTypeNameFromEnum(EVREventType eType) override { note(30, _ReturnAddress()); return target_->GetEventTypeNameFromEnum(eType); }
    HiddenAreaMesh_t GetHiddenAreaMesh(EVREye eEye) override { note(31, _ReturnAddress()); return target_->GetHiddenAreaMesh(eEye); }
    bool GetControllerState(vr::TrackedDeviceIndex_t unControllerDeviceIndex, vr::VRControllerState_t *pControllerState) override { note(32, _ReturnAddress()); return target_->GetControllerState(unControllerDeviceIndex, pControllerState); }
    bool GetControllerStateWithPose(ETrackingUniverseOrigin eOrigin, vr::TrackedDeviceIndex_t unControllerDeviceIndex, vr::VRControllerState_t *pControllerState, TrackedDevicePose_t *pTrackedDevicePose) override { note(33, _ReturnAddress()); return target_->GetControllerStateWithPose(eOrigin, unControllerDeviceIndex, pControllerState, pTrackedDevicePose); }
    void TriggerHapticPulse(vr::TrackedDeviceIndex_t unControllerDeviceIndex, uint32_t unAxisId, unsigned short usDurationMicroSec) override { note(34, _ReturnAddress()); target_->TriggerHapticPulse(unControllerDeviceIndex, unAxisId, usDurationMicroSec); }
    const char *GetButtonIdNameFromEnum(EVRButtonId eButtonId) override { note(35, _ReturnAddress()); return target_->GetButtonIdNameFromEnum(eButtonId); }
    const char *GetControllerAxisTypeNameFromEnum(EVRControllerAxisType eAxisType) override { note(36, _ReturnAddress()); return target_->GetControllerAxisTypeNameFromEnum(eAxisType); }
    bool CaptureInputFocus() override { note(37, _ReturnAddress()); return target_->CaptureInputFocus(); }
    void ReleaseInputFocus() override { note(38, _ReturnAddress()); target_->ReleaseInputFocus(); }
    bool IsInputFocusCapturedByAnotherProcess() override { note(39, _ReturnAddress()); return target_->IsInputFocusCapturedByAnotherProcess(); }
    uint32_t DriverDebugRequest(vr::TrackedDeviceIndex_t unDeviceIndex, const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize) override { note(40, _ReturnAddress()); return target_->DriverDebugRequest(unDeviceIndex, pchRequest, pchResponseBuffer, unResponseBufferSize); }
    vr::EVRFirmwareError PerformFirmwareUpdate(vr::TrackedDeviceIndex_t unDeviceIndex) override { note(41, _ReturnAddress()); return target_->PerformFirmwareUpdate(unDeviceIndex); }
    void AcknowledgeQuit_Exiting() override { note(42, _ReturnAddress()); target_->AcknowledgeQuit_Exiting(); }
    void AcknowledgeQuit_UserPrompt() override { note(43, _ReturnAddress()); target_->AcknowledgeQuit_UserPrompt(); }
private:
    void note(unsigned slot, const void* caller) const noexcept {
        if (!census_ || !census_->enabled()) return;
        census_->observe(kMethods[0 + slot], census_->classify(caller));
    }
    vr::IVRSystem* target_;
    Census* census_;
};

class OpenVRCompositorForward : public vr::IVRCompositor {
public:
    OpenVRCompositorForward(vr::IVRCompositor* target, Census* census) noexcept : target_(target), census_(census) {}
    void SetTrackingSpace(ETrackingUniverseOrigin eOrigin) override { note(0, _ReturnAddress()); target_->SetTrackingSpace(eOrigin); }
    ETrackingUniverseOrigin GetTrackingSpace() override { note(1, _ReturnAddress()); return target_->GetTrackingSpace(); }
    EVRCompositorError WaitGetPoses(TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount, TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount) override { note(2, _ReturnAddress()); return target_->WaitGetPoses(pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount); }
    EVRCompositorError GetLastPoses(TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount, TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount) override { note(3, _ReturnAddress()); return target_->GetLastPoses(pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount); }
    EVRCompositorError GetLastPoseForTrackedDeviceIndex(TrackedDeviceIndex_t unDeviceIndex, TrackedDevicePose_t *pOutputPose, TrackedDevicePose_t *pOutputGamePose) override { note(4, _ReturnAddress()); return target_->GetLastPoseForTrackedDeviceIndex(unDeviceIndex, pOutputPose, pOutputGamePose); }
    EVRCompositorError Submit(EVREye eEye, const Texture_t *pTexture, const VRTextureBounds_t* pBounds, EVRSubmitFlags nSubmitFlags) override { note(5, _ReturnAddress()); return target_->Submit(eEye, pTexture, pBounds, nSubmitFlags); }
    void ClearLastSubmittedFrame() override { note(6, _ReturnAddress()); target_->ClearLastSubmittedFrame(); }
    void PostPresentHandoff() override { note(7, _ReturnAddress()); target_->PostPresentHandoff(); }
    bool GetFrameTiming(Compositor_FrameTiming *pTiming, uint32_t unFramesAgo) override { note(8, _ReturnAddress()); return target_->GetFrameTiming(pTiming, unFramesAgo); }
    float GetFrameTimeRemaining() override { note(9, _ReturnAddress()); return target_->GetFrameTimeRemaining(); }
    void FadeToColor(float fSeconds, float fRed, float fGreen, float fBlue, float fAlpha, bool bBackground) override { note(10, _ReturnAddress()); target_->FadeToColor(fSeconds, fRed, fGreen, fBlue, fAlpha, bBackground); }
    void FadeGrid(float fSeconds, bool bFadeIn) override { note(11, _ReturnAddress()); target_->FadeGrid(fSeconds, bFadeIn); }
    EVRCompositorError SetSkyboxOverride(const Texture_t *pTextures, uint32_t unTextureCount) override { note(12, _ReturnAddress()); return target_->SetSkyboxOverride(pTextures, unTextureCount); }
    void ClearSkyboxOverride() override { note(13, _ReturnAddress()); target_->ClearSkyboxOverride(); }
    void CompositorBringToFront() override { note(14, _ReturnAddress()); target_->CompositorBringToFront(); }
    void CompositorGoToBack() override { note(15, _ReturnAddress()); target_->CompositorGoToBack(); }
    void CompositorQuit() override { note(16, _ReturnAddress()); target_->CompositorQuit(); }
    bool IsFullscreen() override { note(17, _ReturnAddress()); return target_->IsFullscreen(); }
    uint32_t GetCurrentSceneFocusProcess() override { note(18, _ReturnAddress()); return target_->GetCurrentSceneFocusProcess(); }
    uint32_t GetLastFrameRenderer() override { note(19, _ReturnAddress()); return target_->GetLastFrameRenderer(); }
    bool CanRenderScene() override { note(20, _ReturnAddress()); return target_->CanRenderScene(); }
    void ShowMirrorWindow() override { note(21, _ReturnAddress()); target_->ShowMirrorWindow(); }
    void HideMirrorWindow() override { note(22, _ReturnAddress()); target_->HideMirrorWindow(); }
    bool IsMirrorWindowVisible() override { note(23, _ReturnAddress()); return target_->IsMirrorWindowVisible(); }
    void CompositorDumpImages() override { note(24, _ReturnAddress()); target_->CompositorDumpImages(); }
    bool ShouldAppRenderWithLowResources() override { note(25, _ReturnAddress()); return target_->ShouldAppRenderWithLowResources(); }
    void ForceInterleavedReprojectionOn(bool bOverride) override { note(26, _ReturnAddress()); target_->ForceInterleavedReprojectionOn(bOverride); }
    void ForceReconnectProcess() override { note(27, _ReturnAddress()); target_->ForceReconnectProcess(); }
    void SuspendRendering(bool bSuspend) override { note(28, _ReturnAddress()); target_->SuspendRendering(bSuspend); }
private:
    void note(unsigned slot, const void* caller) const noexcept {
        if (!census_ || !census_->enabled()) return;
        census_->observe(kMethods[44 + slot], census_->classify(caller));
    }
    vr::IVRCompositor* target_;
    Census* census_;
};

class OpenVRChaperoneForward : public vr::IVRChaperone {
public:
    OpenVRChaperoneForward(vr::IVRChaperone* target, Census* census) noexcept : target_(target), census_(census) {}
    ChaperoneCalibrationState GetCalibrationState() override { note(0, _ReturnAddress()); return target_->GetCalibrationState(); }
    bool GetPlayAreaSize(float *pSizeX, float *pSizeZ) override { note(1, _ReturnAddress()); return target_->GetPlayAreaSize(pSizeX, pSizeZ); }
    bool GetPlayAreaRect(HmdQuad_t *rect) override { note(2, _ReturnAddress()); return target_->GetPlayAreaRect(rect); }
    void ReloadInfo(void) override { note(3, _ReturnAddress()); target_->ReloadInfo(); }
    void SetSceneColor(HmdColor_t color) override { note(4, _ReturnAddress()); target_->SetSceneColor(color); }
    void GetBoundsColor(HmdColor_t *pOutputColorArray, int nNumOutputColors, float flCollisionBoundsFadeDistance, HmdColor_t *pOutputCameraColor) override { note(5, _ReturnAddress()); target_->GetBoundsColor(pOutputColorArray, nNumOutputColors, flCollisionBoundsFadeDistance, pOutputCameraColor); }
    bool AreBoundsVisible() override { note(6, _ReturnAddress()); return target_->AreBoundsVisible(); }
    void ForceBoundsVisible(bool bForce) override { note(7, _ReturnAddress()); target_->ForceBoundsVisible(bForce); }
private:
    void note(unsigned slot, const void* caller) const noexcept {
        if (!census_ || !census_->enabled()) return;
        census_->observe(kMethods[73 + slot], census_->classify(caller));
    }
    vr::IVRChaperone* target_;
    Census* census_;
};

class OpenVRExtendedDisplayForward : public vr::IVRExtendedDisplay {
public:
    OpenVRExtendedDisplayForward(vr::IVRExtendedDisplay* target, Census* census) noexcept : target_(target), census_(census) {}
    void GetWindowBounds(int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight) override { note(0, _ReturnAddress()); target_->GetWindowBounds(pnX, pnY, pnWidth, pnHeight); }
    void GetEyeOutputViewport(EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight) override { note(1, _ReturnAddress()); target_->GetEyeOutputViewport(eEye, pnX, pnY, pnWidth, pnHeight); }
    void GetDXGIOutputInfo(int32_t *pnAdapterIndex, int32_t *pnAdapterOutputIndex) override { note(2, _ReturnAddress()); target_->GetDXGIOutputInfo(pnAdapterIndex, pnAdapterOutputIndex); }
private:
    void note(unsigned slot, const void* caller) const noexcept {
        if (!census_ || !census_->enabled()) return;
        census_->observe(kMethods[81 + slot], census_->classify(caller));
    }
    vr::IVRExtendedDisplay* target_;
    Census* census_;
};

class ForwardingCache {
public:
    explicit ForwardingCache(Census* census = nullptr) noexcept : census_(census) {}
    vr::IVRSystem* wrapSystem(vr::IVRSystem* original) noexcept { return wrap(system_, original); }
    vr::IVRCompositor* wrapCompositor(vr::IVRCompositor* original) noexcept { return wrap(compositor_, original); }
    vr::IVRChaperone* wrapChaperone(vr::IVRChaperone* original) noexcept { return wrap(chaperone_, original); }
    vr::IVRExtendedDisplay* wrapExtendedDisplay(vr::IVRExtendedDisplay* original) noexcept { return wrap(extended_, original); }
private:
    template<class Wrapper, class Interface>
    struct Slot { Interface* original = nullptr; Interface* wrapper = nullptr; typename std::aligned_storage<sizeof(Wrapper), alignof(Wrapper)>::type storage; bool occupied = false; };
    template<class Wrapper, class Interface>
    Interface* wrap(std::array<Slot<Wrapper, Interface>, 4>& slots, Interface* original) noexcept {
        if (!original) return nullptr;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& slot : slots)
            if (slot.occupied && slot.original == original)
                return slot.wrapper;
        for (auto& slot : slots) if (!slot.occupied) {
            slot.original = original;
            slot.wrapper = new (&slot.storage) Wrapper(original, census_);
            slot.occupied = true;
            return slot.wrapper;
        }
        return nullptr;
    }
    Census* census_;
    std::mutex mutex_;
    std::array<Slot<OpenVRSystemForward, vr::IVRSystem>, 4> system_{};
    std::array<Slot<OpenVRCompositorForward, vr::IVRCompositor>, 4> compositor_{};
    std::array<Slot<OpenVRChaperoneForward, vr::IVRChaperone>, 4> chaperone_{};
    std::array<Slot<OpenVRExtendedDisplayForward, vr::IVRExtendedDisplay>, 4> extended_{};
};

static_assert(!std::is_abstract<OpenVRSystemForward>::value, "system forwarding wrapper incomplete");
static_assert(!std::is_abstract<OpenVRCompositorForward>::value, "compositor forwarding wrapper incomplete");
static_assert(!std::is_abstract<OpenVRChaperoneForward>::value, "chaperone forwarding wrapper incomplete");
static_assert(!std::is_abstract<OpenVRExtendedDisplayForward>::value, "extended display forwarding wrapper incomplete");

}
