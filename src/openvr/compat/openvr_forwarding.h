#pragma once
#include "openvr_call_census.h"
#include <intrin.h>
#include <type_traits>
#include <array>
#include <mutex>
#include <new>

namespace edvr::openvr_abi {
using namespace vr;

// The runtime call stays outside this guard. A broken diagnostic pointer must
// not change forwarding or turn a successful call into an access violation.
template<class Fill> void captureEvidence(Evidence& e, const Fill& fill) noexcept {
    __try { fill(e); }
    __except(1) { e=Evidence{}; e.flags=EvidenceReadFault; }
}
template<class Fill> void capture(Census* census, unsigned index, const void* caller,
                                 uint32_t key, const Fill& fill) noexcept {
    if(!census || !census->enabled()) return;
    const Origin origin=census->classify(caller);
    uint32_t record=0;
    if(!census->claim(kMethods[index],origin,key,record)) return;
    Evidence e{};
    captureEvidence(e,fill);
    census->observe(kMethods[index],origin,e,record);
}
inline uint32_t propertyKey(uint32_t device, uint32_t property) noexcept {
    return device<65536 && property<65536 ? (device<<16)|property : UINT32_MAX;
}


class OpenVRSystemForward : public vr::IVRSystem {
public:
    OpenVRSystemForward(vr::IVRSystem* target, Census* census) noexcept : target_(target), census_(census) {}
    void GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight) override {
        target_->GetRecommendedRenderTargetSize(pnWidth,pnHeight);
        capture(census_,0,_ReturnAddress(),0,[&](Evidence& e) {
            e.schema="size u0=width u1=height";
            e.flags=EvidenceArgs;
            if(pnWidth&&pnHeight){e.u(0,*pnWidth);e.u(1,*pnHeight);e.flags|=EvidenceOutputs;}
        });

    }
    HmdMatrix44_t GetProjectionMatrix(EVREye eEye, float fNearZ, float fFarZ, EGraphicsAPIConvention eProjType) override {
        const auto result=target_->GetProjectionMatrix(eEye,fNearZ,fFarZ,eProjType);
        capture(census_,1,_ReturnAddress(),uint32_t(eEye),[&](Evidence& e) {
            e.schema="projection u0=eye u1=convention f0=near f1=far";
            e.flags=EvidenceArgs;
            e.u(0,uint32_t(eEye));e.u(1,uint32_t(eProjType));e.f(0,fNearZ);e.f(1,fFarZ);for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)e.matrix44[r*4+c]=result.m[r][c];e.flags|=EvidenceMatrix44;
        });
        return result;
    }
    void GetProjectionRaw(EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom) override {
        target_->GetProjectionRaw(eEye,pfLeft,pfRight,pfTop,pfBottom);
        capture(census_,2,_ReturnAddress(),uint32_t(eEye),[&](Evidence& e) {
            e.schema="raw u0=eye f0=left f1=right f2=top f3=bottom";
            e.flags=EvidenceArgs;
            e.u(0,uint32_t(eEye));if(pfLeft&&pfRight&&pfTop&&pfBottom){e.f(0,*pfLeft);e.f(1,*pfRight);e.f(2,*pfTop);e.f(3,*pfBottom);e.flags|=EvidenceOutputs;}
        });

    }
    DistortionCoordinates_t ComputeDistortion(EVREye eEye, float fU, float fV) override { note(3, _ReturnAddress()); return target_->ComputeDistortion(eEye, fU, fV); }
    HmdMatrix34_t GetEyeToHeadTransform(EVREye eEye) override {
        const auto result=target_->GetEyeToHeadTransform(eEye);
        capture(census_,4,_ReturnAddress(),uint32_t(eEye),[&](Evidence& e) {
            e.schema="eye_transform u0=eye";
            e.flags=EvidenceArgs;
            e.u(0,uint32_t(eEye));for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)e.matrix34[r*4+c]=result.m[r][c];e.flags|=EvidenceMatrix34;
        });
        return result;
    }
    bool GetTimeSinceLastVsync(float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter) override { note(5, _ReturnAddress()); return target_->GetTimeSinceLastVsync(pfSecondsSinceLastVsync, pulFrameCounter); }
    int32_t GetD3D9AdapterIndex() override { note(6, _ReturnAddress()); return target_->GetD3D9AdapterIndex(); }
    void GetDXGIOutputInfo(int32_t *pnAdapterIndex) override { note(7, _ReturnAddress()); target_->GetDXGIOutputInfo(pnAdapterIndex); }
    bool IsDisplayOnDesktop() override {
        const bool result=target_->IsDisplayOnDesktop();
        capture(census_,8,_ReturnAddress(),0,[&](Evidence& e) {
            e.schema="desktop u0=result";
            e.flags=EvidenceArgs;
            e.u(0,result);e.flags|=EvidenceResult;
        });
        return result;
    }
    bool SetDisplayVisibility(bool bIsVisibleOnDesktop) override { note(9, _ReturnAddress()); return target_->SetDisplayVisibility(bIsVisibleOnDesktop); }
    void GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin eOrigin, float fPredictedSecondsToPhotonsFromNow, TrackedDevicePose_t *pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount) override {
        target_->GetDeviceToAbsoluteTrackingPose(eOrigin,fPredictedSecondsToPhotonsFromNow,pTrackedDevicePoseArray,unTrackedDevicePoseArrayCount);
        capture(census_,10,_ReturnAddress(),uint32_t(eOrigin),[&](Evidence& e) {
            e.schema="absolute_pose u0=origin u1=capacity u2=hmd_connected u3=hmd_valid i0=tracking f0=prediction";
            e.flags=EvidenceArgs;
            e.u(0,uint32_t(eOrigin));e.u(1,unTrackedDevicePoseArrayCount);e.f(0,fPredictedSecondsToPhotonsFromNow);if(pTrackedDevicePoseArray&&unTrackedDevicePoseArrayCount){e.u(2,pTrackedDevicePoseArray[0].bDeviceIsConnected);e.u(3,pTrackedDevicePoseArray[0].bPoseIsValid);e.i(0,int32_t(pTrackedDevicePoseArray[0].eTrackingResult));e.flags|=EvidenceOutputs;}
        });

    }
    void ResetSeatedZeroPose() override { note(11, _ReturnAddress()); target_->ResetSeatedZeroPose(); }
    HmdMatrix34_t GetSeatedZeroPoseToStandingAbsoluteTrackingPose() override { note(12, _ReturnAddress()); return target_->GetSeatedZeroPoseToStandingAbsoluteTrackingPose(); }
    HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose() override { note(13, _ReturnAddress()); return target_->GetRawZeroPoseToStandingAbsoluteTrackingPose(); }
    uint32_t GetSortedTrackedDeviceIndicesOfClass(ETrackedDeviceClass eTrackedDeviceClass, vr::TrackedDeviceIndex_t *punTrackedDeviceIndexArray, uint32_t unTrackedDeviceIndexArrayCount, vr::TrackedDeviceIndex_t unRelativeToTrackedDeviceIndex) override { note(14, _ReturnAddress()); return target_->GetSortedTrackedDeviceIndicesOfClass(eTrackedDeviceClass, punTrackedDeviceIndexArray, unTrackedDeviceIndexArrayCount, unRelativeToTrackedDeviceIndex); }
    EDeviceActivityLevel GetTrackedDeviceActivityLevel(vr::TrackedDeviceIndex_t unDeviceId) override { note(15, _ReturnAddress()); return target_->GetTrackedDeviceActivityLevel(unDeviceId); }
    void ApplyTransform(TrackedDevicePose_t *pOutputPose, const TrackedDevicePose_t *pTrackedDevicePose, const HmdMatrix34_t *pTransform) override { note(16, _ReturnAddress()); target_->ApplyTransform(pOutputPose, pTrackedDevicePose, pTransform); }
    vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole unDeviceType) override { note(17, _ReturnAddress()); return target_->GetTrackedDeviceIndexForControllerRole(unDeviceType); }
    vr::ETrackedControllerRole GetControllerRoleForTrackedDeviceIndex(vr::TrackedDeviceIndex_t unDeviceIndex) override { note(18, _ReturnAddress()); return target_->GetControllerRoleForTrackedDeviceIndex(unDeviceIndex); }
    ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t unDeviceIndex) override { note(19, _ReturnAddress()); return target_->GetTrackedDeviceClass(unDeviceIndex); }
    bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t unDeviceIndex) override {
        const bool result=target_->IsTrackedDeviceConnected(unDeviceIndex);
        capture(census_,20,_ReturnAddress(),unDeviceIndex,[&](Evidence& e) {
            e.schema="connected u0=device u1=result";
            e.flags=EvidenceArgs;
            e.u(0,unDeviceIndex);e.u(1,result);e.flags|=EvidenceResult;
        });
        return result;
    }
    bool GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override {
        const auto result=target_->GetBoolTrackedDeviceProperty(unDeviceIndex,prop,pError);
        capture(census_,21,_ReturnAddress(),propertyKey(unDeviceIndex,uint32_t(prop)),[&](Evidence& e) {
            e.schema="property u0=device u1=property u2_or_i1_or_f0_or_u64=result i0=error";
            e.flags=EvidenceArgs;
            e.u(0,unDeviceIndex);e.u(1,uint32_t(prop));e.u(2,result);e.flags|=EvidenceResult;if(pError){e.i(0,int32_t(*pError));e.flags|=EvidenceOutputs;}
        });
        return result;
    }
    float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override {
        const auto result=target_->GetFloatTrackedDeviceProperty(unDeviceIndex,prop,pError);
        capture(census_,22,_ReturnAddress(),propertyKey(unDeviceIndex,uint32_t(prop)),[&](Evidence& e) {
            e.schema="property u0=device u1=property u2_or_i1_or_f0_or_u64=result i0=error";
            e.flags=EvidenceArgs;
            e.u(0,unDeviceIndex);e.u(1,uint32_t(prop));e.f(0,result);e.flags|=EvidenceResult;if(pError){e.i(0,int32_t(*pError));e.flags|=EvidenceOutputs;}
        });
        return result;
    }
    int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override {
        const auto result=target_->GetInt32TrackedDeviceProperty(unDeviceIndex,prop,pError);
        capture(census_,23,_ReturnAddress(),propertyKey(unDeviceIndex,uint32_t(prop)),[&](Evidence& e) {
            e.schema="property u0=device u1=property u2_or_i1_or_f0_or_u64=result i0=error";
            e.flags=EvidenceArgs;
            e.u(0,unDeviceIndex);e.u(1,uint32_t(prop));e.i(1,result);e.flags|=EvidenceResult;if(pError){e.i(0,int32_t(*pError));e.flags|=EvidenceOutputs;}
        });
        return result;
    }
    uint64_t GetUint64TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override {
        const auto result=target_->GetUint64TrackedDeviceProperty(unDeviceIndex,prop,pError);
        capture(census_,24,_ReturnAddress(),propertyKey(unDeviceIndex,uint32_t(prop)),[&](Evidence& e) {
            e.schema="property u0=device u1=property u2_or_i1_or_f0_or_u64=result i0=error";
            e.flags=EvidenceArgs;
            e.u(0,unDeviceIndex);e.u(1,uint32_t(prop));e.u64=result;e.haveU64=true;e.flags|=EvidenceResult;if(pError){e.i(0,int32_t(*pError));e.flags|=EvidenceOutputs;}
        });
        return result;
    }
    HmdMatrix34_t GetMatrix34TrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError) override { note(25, _ReturnAddress()); return target_->GetMatrix34TrackedDeviceProperty(unDeviceIndex, prop, pError); }
    uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, char *pchValue, uint32_t unBufferSize, ETrackedPropertyError *pError) override {
        const auto result=target_->GetStringTrackedDeviceProperty(unDeviceIndex,prop,pchValue,unBufferSize,pError);
        capture(census_,26,_ReturnAddress(),propertyKey(unDeviceIndex,uint32_t(prop)),[&](Evidence& e) {
            e.schema="string_property u0=device u1=property u2=capacity u3=required u4=buffer_present i0=error";
            e.flags=EvidenceArgs;
            e.u(0,unDeviceIndex);e.u(1,uint32_t(prop));e.u(2,unBufferSize);e.u(3,result);e.u(4,pchValue!=nullptr);e.bytes=unBufferSize;e.flags|=EvidenceResult;if(pError){e.i(0,int32_t(*pError));e.flags|=EvidenceOutputs;}
        });
        return result;
    }
    const char *GetPropErrorNameFromEnum(ETrackedPropertyError error) override { note(27, _ReturnAddress()); return target_->GetPropErrorNameFromEnum(error); }
    bool PollNextEvent(VREvent_t *pEvent, uint32_t uncbVREvent) override {
        const bool result=target_->PollNextEvent(pEvent,uncbVREvent);
        capture(census_,28,_ReturnAddress(),result?1u:0u,[&](Evidence& e) {
            e.schema="event u0=result u1=type u2=device u3=capacity f0=age";
            e.flags=EvidenceArgs;
            e.u(0,result);e.u(3,uncbVREvent);e.bytes=uncbVREvent;e.flags|=EvidenceResult;if(result&&pEvent&&uncbVREvent>=sizeof(VREvent_t)){e.u(1,pEvent->eventType);e.u(2,pEvent->trackedDeviceIndex);e.f(0,pEvent->eventAgeSeconds);e.flags|=EvidenceEvent;}
        });
        return result;
    }
    bool PollNextEventWithPose(ETrackingUniverseOrigin eOrigin, VREvent_t *pEvent, uint32_t uncbVREvent, vr::TrackedDevicePose_t *pTrackedDevicePose) override {
        const bool result=target_->PollNextEventWithPose(eOrigin,pEvent,uncbVREvent,pTrackedDevicePose);
        capture(census_,29,_ReturnAddress(),result?1u:0u,[&](Evidence& e) {
            e.schema="event_pose u0=result u1=type u2=device u3=capacity u4=origin u5=pose_valid";
            e.flags=EvidenceArgs;
            e.u(0,result);e.u(3,uncbVREvent);e.u(4,uint32_t(eOrigin));e.bytes=uncbVREvent;e.flags|=EvidenceResult;if(result&&pEvent&&uncbVREvent>=sizeof(VREvent_t)){e.u(1,pEvent->eventType);e.u(2,pEvent->trackedDeviceIndex);e.flags|=EvidenceEvent;}if(result&&pTrackedDevicePose){e.u(5,pTrackedDevicePose->bPoseIsValid);e.flags|=EvidenceOutputs;}
        });
        return result;
    }
    const char *GetEventTypeNameFromEnum(EVREventType eType) override { note(30, _ReturnAddress()); return target_->GetEventTypeNameFromEnum(eType); }
    HiddenAreaMesh_t GetHiddenAreaMesh(EVREye eEye) override {
        const auto result=target_->GetHiddenAreaMesh(eEye);
        capture(census_,31,_ReturnAddress(),uint32_t(eEye),[&](Evidence& e) {
            e.schema="hidden_mesh u0=eye u1=triangle_count u2=vertices_present";
            e.flags=EvidenceArgs;
            e.u(0,uint32_t(eEye));e.u(1,result.unTriangleCount);e.u(2,result.pVertexData!=nullptr);e.flags|=EvidenceResult;
        });
        return result;
    }
    bool GetControllerState(vr::TrackedDeviceIndex_t unControllerDeviceIndex, vr::VRControllerState_t *pControllerState) override {
        const bool result=target_->GetControllerState(unControllerDeviceIndex,pControllerState);
        capture(census_,32,_ReturnAddress(),unControllerDeviceIndex,[&](Evidence& e) {
            e.schema="controller u0=device u1=result u2=packet u64=pressed bytes=fixed_abi";
            e.flags=EvidenceArgs;
            e.u(0,unControllerDeviceIndex);e.u(1,result);e.bytes=sizeof(VRControllerState_t);e.flags|=EvidenceResult;if(result&&pControllerState){e.u(2,pControllerState->unPacketNum);e.u64=pControllerState->ulButtonPressed;e.haveU64=true;e.flags|=EvidenceOutputs;}
        });
        return result;
    }
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
    void SetTrackingSpace(ETrackingUniverseOrigin eOrigin) override {
        target_->SetTrackingSpace(eOrigin);
        capture(census_,44,_ReturnAddress(),uint32_t(eOrigin),[&](Evidence& e) {
            e.schema="tracking_space u0=origin";
            e.flags=EvidenceArgs;
            e.u(0,uint32_t(eOrigin));
        });

    }
    ETrackingUniverseOrigin GetTrackingSpace() override {
        const auto result=target_->GetTrackingSpace();
        capture(census_,45,_ReturnAddress(),0,[&](Evidence& e) {
            e.schema="tracking_space u0=result";
            e.flags=EvidenceArgs;
            e.u(0,uint32_t(result));e.flags|=EvidenceResult;
        });
        return result;
    }
    EVRCompositorError WaitGetPoses(TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount, TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount) override { note(2, _ReturnAddress()); return target_->WaitGetPoses(pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount); }
    EVRCompositorError GetLastPoses(TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount, TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount) override { note(3, _ReturnAddress()); return target_->GetLastPoses(pRenderPoseArray, unRenderPoseArrayCount, pGamePoseArray, unGamePoseArrayCount); }
    EVRCompositorError GetLastPoseForTrackedDeviceIndex(TrackedDeviceIndex_t unDeviceIndex, TrackedDevicePose_t *pOutputPose, TrackedDevicePose_t *pOutputGamePose) override { note(4, _ReturnAddress()); return target_->GetLastPoseForTrackedDeviceIndex(unDeviceIndex, pOutputPose, pOutputGamePose); }
    EVRCompositorError Submit(EVREye eEye, const Texture_t *pTexture, const VRTextureBounds_t* pBounds, EVRSubmitFlags nSubmitFlags) override { note(5, _ReturnAddress()); return target_->Submit(eEye, pTexture, pBounds, nSubmitFlags); }
    void ClearLastSubmittedFrame() override { note(6, _ReturnAddress()); target_->ClearLastSubmittedFrame(); }
    void PostPresentHandoff() override { note(7, _ReturnAddress()); target_->PostPresentHandoff(); }
    bool GetFrameTiming(Compositor_FrameTiming *pTiming, uint32_t unFramesAgo) override { note(8, _ReturnAddress()); return target_->GetFrameTiming(pTiming, unFramesAgo); }
    float GetFrameTimeRemaining() override { note(9, _ReturnAddress()); return target_->GetFrameTimeRemaining(); }
    void FadeToColor(float fSeconds, float fRed, float fGreen, float fBlue, float fAlpha, bool bBackground) override {
        target_->FadeToColor(fSeconds,fRed,fGreen,fBlue,fAlpha,bBackground);
        capture(census_,54,_ReturnAddress(),bBackground?1u:0u,[&](Evidence& e) {
            e.schema="fade u0=background f0=seconds f1=red f2=green f3=blue f4=alpha";
            e.flags=EvidenceArgs;
            e.u(0,bBackground);e.f(0,fSeconds);e.f(1,fRed);e.f(2,fGreen);e.f(3,fBlue);e.f(4,fAlpha);e.flags|=EvidenceColor;
        });

    }
    void FadeGrid(float fSeconds, bool bFadeIn) override {
        target_->FadeGrid(fSeconds,bFadeIn);
        capture(census_,55,_ReturnAddress(),bFadeIn?1u:0u,[&](Evidence& e) {
            e.schema="fade_grid u0=in f0=seconds";
            e.flags=EvidenceArgs;
            e.u(0,bFadeIn);e.f(0,fSeconds);
        });

    }
    EVRCompositorError SetSkyboxOverride(const Texture_t *pTextures, uint32_t unTextureCount) override {
        const auto result=target_->SetSkyboxOverride(pTextures,unTextureCount);
        capture(census_,56,_ReturnAddress(),unTextureCount,[&](Evidence& e) {
            e.schema="skybox u0=count u1=first_type u2=first_color u3=handle_present i0=result";
            e.flags=EvidenceArgs;
            e.u(0,unTextureCount);e.i(0,int32_t(result));e.flags|=EvidenceResult;if(pTextures&&unTextureCount){e.u(1,uint32_t(pTextures[0].eType));e.u(2,uint32_t(pTextures[0].eColorSpace));e.u(3,pTextures[0].handle!=nullptr);e.flags|=EvidenceTexture;}
        });
        return result;
    }
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
    void SuspendRendering(bool bSuspend) override {
        target_->SuspendRendering(bSuspend);
        capture(census_,72,_ReturnAddress(),bSuspend?1u:0u,[&](Evidence& e) {
            e.schema="suspend u0=requested";
            e.flags=EvidenceArgs;
            e.u(0,bSuspend);
        });

    }
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
    ChaperoneCalibrationState GetCalibrationState() override {
        const auto result=target_->GetCalibrationState();
        capture(census_,73,_ReturnAddress(),0,[&](Evidence& e) {
            e.schema="calibration u0=result";
            e.flags=EvidenceArgs;
            e.u(0,uint32_t(result));e.flags|=EvidenceResult;
        });
        return result;
    }
    bool GetPlayAreaSize(float *pSizeX, float *pSizeZ) override {
        const bool result=target_->GetPlayAreaSize(pSizeX,pSizeZ);
        capture(census_,74,_ReturnAddress(),result?1u:0u,[&](Evidence& e) {
            e.schema="play_area u0=result f0=x f1=z";
            e.flags=EvidenceArgs;
            e.u(0,result);e.flags|=EvidenceResult;if(result&&pSizeX&&pSizeZ){e.f(0,*pSizeX);e.f(1,*pSizeZ);e.flags|=EvidenceOutputs;}
        });
        return result;
    }
    bool GetPlayAreaRect(HmdQuad_t *rect) override { note(2, _ReturnAddress()); return target_->GetPlayAreaRect(rect); }
    void ReloadInfo(void) override { note(3, _ReturnAddress()); target_->ReloadInfo(); }
    void SetSceneColor(HmdColor_t color) override {
        target_->SetSceneColor(color);
        capture(census_,77,_ReturnAddress(),0,[&](Evidence& e) {
            e.schema="scene_color f0=red f1=green f2=blue f3=alpha";
            e.flags=EvidenceArgs;
            e.f(0,color.r);e.f(1,color.g);e.f(2,color.b);e.f(3,color.a);e.flags|=EvidenceColor;
        });

    }
    void GetBoundsColor(HmdColor_t *pOutputColorArray, int nNumOutputColors, float flCollisionBoundsFadeDistance, HmdColor_t *pOutputCameraColor) override { note(5, _ReturnAddress()); target_->GetBoundsColor(pOutputColorArray, nNumOutputColors, flCollisionBoundsFadeDistance, pOutputCameraColor); }
    bool AreBoundsVisible() override {
        const bool result=target_->AreBoundsVisible();
        capture(census_,79,_ReturnAddress(),0,[&](Evidence& e) {
            e.schema="bounds_visible u0=result";
            e.flags=EvidenceArgs;
            e.u(0,result);e.flags|=EvidenceResult;
        });
        return result;
    }
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
    void GetWindowBounds(int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight) override {
        target_->GetWindowBounds(pnX,pnY,pnWidth,pnHeight);
        capture(census_,81,_ReturnAddress(),0,[&](Evidence& e) {
            e.schema="window i0=x i1=y u0=width u1=height";
            e.flags=EvidenceArgs;
            if(pnX&&pnY&&pnWidth&&pnHeight){e.i(0,*pnX);e.i(1,*pnY);e.u(0,*pnWidth);e.u(1,*pnHeight);e.flags|=EvidenceOutputs;}
        });

    }
    void GetEyeOutputViewport(EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight) override {
        target_->GetEyeOutputViewport(eEye,pnX,pnY,pnWidth,pnHeight);
        capture(census_,82,_ReturnAddress(),uint32_t(eEye),[&](Evidence& e) {
            e.schema="viewport u0=eye u1=x u2=y u3=width u4=height";
            e.flags=EvidenceArgs;
            e.u(0,uint32_t(eEye));if(pnX&&pnY&&pnWidth&&pnHeight){e.u(1,*pnX);e.u(2,*pnY);e.u(3,*pnWidth);e.u(4,*pnHeight);e.flags|=EvidenceOutputs;}
        });

    }
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
