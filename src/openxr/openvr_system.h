#pragma once
#include "system_source.h"
#include <atomic>

namespace edvr::openxr {
using namespace vr;
// Exact historical ABI. This object is not advertised by the shipping proxy.
// The source outlives the object and synchronizes runtime/space ownership.
class OpenVRSystem final : public vr::IVRSystem {
 public:
  explicit OpenVRSystem(SystemSource& source):source_(source){}
  OpenVRSystem(const OpenVRSystem&)=delete;
  OpenVRSystem& operator=(const OpenVRSystem&)=delete;
  void GetRecommendedRenderTargetSize( uint32_t *pnWidth, uint32_t *pnHeight ) override;
  HmdMatrix44_t GetProjectionMatrix( EVREye eEye, float fNearZ, float fFarZ, EGraphicsAPIConvention eProjType ) override;
  void GetProjectionRaw( EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom ) override;
  DistortionCoordinates_t ComputeDistortion( EVREye eEye, float fU, float fV ) override;
  HmdMatrix34_t GetEyeToHeadTransform( EVREye eEye ) override;
  bool GetTimeSinceLastVsync( float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter ) override;
  int32_t GetD3D9AdapterIndex() override;
  void GetDXGIOutputInfo( int32_t *pnAdapterIndex ) override;
  bool IsDisplayOnDesktop() override;
  bool SetDisplayVisibility( bool bIsVisibleOnDesktop ) override;
  void GetDeviceToAbsoluteTrackingPose( ETrackingUniverseOrigin eOrigin, float fPredictedSecondsToPhotonsFromNow, VR_ARRAY_COUNT(unTrackedDevicePoseArrayCount) TrackedDevicePose_t *pTrackedDevicePoseArray, uint32_t unTrackedDevicePoseArrayCount ) override;
  void ResetSeatedZeroPose() override;
  HmdMatrix34_t GetSeatedZeroPoseToStandingAbsoluteTrackingPose() override;
  HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose() override;
  uint32_t GetSortedTrackedDeviceIndicesOfClass( ETrackedDeviceClass eTrackedDeviceClass, VR_ARRAY_COUNT(unTrackedDeviceIndexArrayCount) vr::TrackedDeviceIndex_t *punTrackedDeviceIndexArray, uint32_t unTrackedDeviceIndexArrayCount, vr::TrackedDeviceIndex_t unRelativeToTrackedDeviceIndex = k_unTrackedDeviceIndex_Hmd ) override;
  EDeviceActivityLevel GetTrackedDeviceActivityLevel( vr::TrackedDeviceIndex_t unDeviceId ) override;
  void ApplyTransform( TrackedDevicePose_t *pOutputPose, const TrackedDevicePose_t *pTrackedDevicePose, const HmdMatrix34_t *pTransform ) override;
  vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole( vr::ETrackedControllerRole unDeviceType ) override;
  vr::ETrackedControllerRole GetControllerRoleForTrackedDeviceIndex( vr::TrackedDeviceIndex_t unDeviceIndex ) override;
  ETrackedDeviceClass GetTrackedDeviceClass( vr::TrackedDeviceIndex_t unDeviceIndex ) override;
  bool IsTrackedDeviceConnected( vr::TrackedDeviceIndex_t unDeviceIndex ) override;
  bool GetBoolTrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError = 0L ) override;
  float GetFloatTrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError = 0L ) override;
  int32_t GetInt32TrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError = 0L ) override;
  uint64_t GetUint64TrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError = 0L ) override;
  HmdMatrix34_t GetMatrix34TrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, ETrackedPropertyError *pError = 0L ) override;
  uint32_t GetStringTrackedDeviceProperty( vr::TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop, VR_OUT_STRING() char *pchValue, uint32_t unBufferSize, ETrackedPropertyError *pError = 0L ) override;
  const char *GetPropErrorNameFromEnum( ETrackedPropertyError error ) override;
  bool PollNextEvent( VREvent_t *pEvent, uint32_t uncbVREvent ) override;
  bool PollNextEventWithPose( ETrackingUniverseOrigin eOrigin, VREvent_t *pEvent, uint32_t uncbVREvent, vr::TrackedDevicePose_t *pTrackedDevicePose ) override;
  const char *GetEventTypeNameFromEnum( EVREventType eType ) override;
  HiddenAreaMesh_t GetHiddenAreaMesh( EVREye eEye ) override;
  bool GetControllerState( vr::TrackedDeviceIndex_t unControllerDeviceIndex, vr::VRControllerState_t *pControllerState ) override;
  bool GetControllerStateWithPose( ETrackingUniverseOrigin eOrigin, vr::TrackedDeviceIndex_t unControllerDeviceIndex, vr::VRControllerState_t *pControllerState, TrackedDevicePose_t *pTrackedDevicePose ) override;
  void TriggerHapticPulse( vr::TrackedDeviceIndex_t unControllerDeviceIndex, uint32_t unAxisId, unsigned short usDurationMicroSec ) override;
  const char *GetButtonIdNameFromEnum( EVRButtonId eButtonId ) override;
  const char *GetControllerAxisTypeNameFromEnum( EVRControllerAxisType eAxisType ) override;
  bool CaptureInputFocus() override;
  void ReleaseInputFocus() override;
  bool IsInputFocusCapturedByAnotherProcess() override;
  uint32_t DriverDebugRequest( vr::TrackedDeviceIndex_t unDeviceIndex, const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize ) override;
  vr::EVRFirmwareError PerformFirmwareUpdate( vr::TrackedDeviceIndex_t unDeviceIndex ) override;
  void AcknowledgeQuit_Exiting() override;
  void AcknowledgeQuit_UserPrompt() override;
 private:
  void unavailable(unsigned slot) noexcept;
  SystemSource& source_;
  std::atomic<uint64_t> unavailable_{0};
};
}
