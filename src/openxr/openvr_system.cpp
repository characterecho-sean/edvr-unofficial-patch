#include "openvr_system.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace edvr::openxr {
namespace {
HmdMatrix34_t identity() { HmdMatrix34_t m{}; m.m[0][0]=m.m[1][1]=m.m[2][2]=1; return m; }
TrackedDevicePose_t invalidPose(bool connected=false) {
  TrackedDevicePose_t p{}; p.mDeviceToAbsoluteTracking=identity();
  p.bDeviceIsConnected=connected; p.eTrackingResult=connected?TrackingResult_Running_OutOfRange:TrackingResult_Uninitialized; return p;
}
bool live(const SystemRead& s) { return s.connected && s.generation; }
bool eyeValid(EVREye e) { return e==Eye_Left || e==Eye_Right; }
bool originValid(ETrackingUniverseOrigin o) { return o==TrackingUniverseSeated || o==TrackingUniverseStanding || o==TrackingUniverseRawAndUncalibrated; }
bool geometryValid(const SystemRead& s) { return live(s) && s.geometryValid && s.geometry.native.generation==s.generation; }
enum class PropertyType { Unknown, Bool, Float, Int, Uint, Matrix, String };
PropertyType propertyType(ETrackedDeviceProperty p) {
  switch(p) {
    case Prop_TrackingSystemName_String:return PropertyType::String;
    case Prop_ModelNumber_String:return PropertyType::String;
    case Prop_SerialNumber_String:return PropertyType::String;
    case Prop_RenderModelName_String:return PropertyType::String;
    case Prop_WillDriftInYaw_Bool:return PropertyType::Bool;
    case Prop_ManufacturerName_String:return PropertyType::String;
    case Prop_TrackingFirmwareVersion_String:return PropertyType::String;
    case Prop_HardwareRevision_String:return PropertyType::String;
    case Prop_AllWirelessDongleDescriptions_String:return PropertyType::String;
    case Prop_ConnectedWirelessDongle_String:return PropertyType::String;
    case Prop_DeviceIsWireless_Bool:return PropertyType::Bool;
    case Prop_DeviceIsCharging_Bool:return PropertyType::Bool;
    case Prop_DeviceBatteryPercentage_Float:return PropertyType::Float;
    case Prop_StatusDisplayTransform_Matrix34:return PropertyType::Matrix;
    case Prop_Firmware_UpdateAvailable_Bool:return PropertyType::Bool;
    case Prop_Firmware_ManualUpdate_Bool:return PropertyType::Bool;
    case Prop_Firmware_ManualUpdateURL_String:return PropertyType::String;
    case Prop_HardwareRevision_Uint64:return PropertyType::Uint;
    case Prop_FirmwareVersion_Uint64:return PropertyType::Uint;
    case Prop_FPGAVersion_Uint64:return PropertyType::Uint;
    case Prop_VRCVersion_Uint64:return PropertyType::Uint;
    case Prop_RadioVersion_Uint64:return PropertyType::Uint;
    case Prop_DongleVersion_Uint64:return PropertyType::Uint;
    case Prop_BlockServerShutdown_Bool:return PropertyType::Bool;
    case Prop_CanUnifyCoordinateSystemWithHmd_Bool:return PropertyType::Bool;
    case Prop_ContainsProximitySensor_Bool:return PropertyType::Bool;
    case Prop_DeviceProvidesBatteryStatus_Bool:return PropertyType::Bool;
    case Prop_DeviceCanPowerOff_Bool:return PropertyType::Bool;
    case Prop_Firmware_ProgrammingTarget_String:return PropertyType::String;
    case Prop_DeviceClass_Int32:return PropertyType::Int;
    case Prop_HasCamera_Bool:return PropertyType::Bool;
    case Prop_DriverVersion_String:return PropertyType::String;
    case Prop_Firmware_ForceUpdateRequired_Bool:return PropertyType::Bool;
    case Prop_ReportsTimeSinceVSync_Bool:return PropertyType::Bool;
    case Prop_SecondsFromVsyncToPhotons_Float:return PropertyType::Float;
    case Prop_DisplayFrequency_Float:return PropertyType::Float;
    case Prop_UserIpdMeters_Float:return PropertyType::Float;
    case Prop_CurrentUniverseId_Uint64:return PropertyType::Uint;
    case Prop_PreviousUniverseId_Uint64:return PropertyType::Uint;
    case Prop_DisplayFirmwareVersion_Uint64:return PropertyType::Uint;
    case Prop_IsOnDesktop_Bool:return PropertyType::Bool;
    case Prop_DisplayMCType_Int32:return PropertyType::Int;
    case Prop_DisplayMCOffset_Float:return PropertyType::Float;
    case Prop_DisplayMCScale_Float:return PropertyType::Float;
    case Prop_EdidVendorID_Int32:return PropertyType::Int;
    case Prop_DisplayMCImageLeft_String:return PropertyType::String;
    case Prop_DisplayMCImageRight_String:return PropertyType::String;
    case Prop_DisplayGCBlackClamp_Float:return PropertyType::Float;
    case Prop_EdidProductID_Int32:return PropertyType::Int;
    case Prop_CameraToHeadTransform_Matrix34:return PropertyType::Matrix;
    case Prop_DisplayGCType_Int32:return PropertyType::Int;
    case Prop_DisplayGCOffset_Float:return PropertyType::Float;
    case Prop_DisplayGCScale_Float:return PropertyType::Float;
    case Prop_DisplayGCPrescale_Float:return PropertyType::Float;
    case Prop_DisplayGCImage_String:return PropertyType::String;
    case Prop_LensCenterLeftU_Float:return PropertyType::Float;
    case Prop_LensCenterLeftV_Float:return PropertyType::Float;
    case Prop_LensCenterRightU_Float:return PropertyType::Float;
    case Prop_LensCenterRightV_Float:return PropertyType::Float;
    case Prop_UserHeadToEyeDepthMeters_Float:return PropertyType::Float;
    case Prop_CameraFirmwareVersion_Uint64:return PropertyType::Uint;
    case Prop_CameraFirmwareDescription_String:return PropertyType::String;
    case Prop_DisplayFPGAVersion_Uint64:return PropertyType::Uint;
    case Prop_DisplayBootloaderVersion_Uint64:return PropertyType::Uint;
    case Prop_DisplayHardwareVersion_Uint64:return PropertyType::Uint;
    case Prop_AudioFirmwareVersion_Uint64:return PropertyType::Uint;
    case Prop_CameraCompatibilityMode_Int32:return PropertyType::Int;
    case Prop_AttachedDeviceId_String:return PropertyType::String;
    case Prop_SupportedButtons_Uint64:return PropertyType::Uint;
    case Prop_Axis0Type_Int32:return PropertyType::Int;
    case Prop_Axis1Type_Int32:return PropertyType::Int;
    case Prop_Axis2Type_Int32:return PropertyType::Int;
    case Prop_Axis3Type_Int32:return PropertyType::Int;
    case Prop_Axis4Type_Int32:return PropertyType::Int;
    case Prop_FieldOfViewLeftDegrees_Float:return PropertyType::Float;
    case Prop_FieldOfViewRightDegrees_Float:return PropertyType::Float;
    case Prop_FieldOfViewTopDegrees_Float:return PropertyType::Float;
    case Prop_FieldOfViewBottomDegrees_Float:return PropertyType::Float;
    case Prop_TrackingRangeMinimumMeters_Float:return PropertyType::Float;
    case Prop_TrackingRangeMaximumMeters_Float:return PropertyType::Float;
    case Prop_ModeLabel_String:return PropertyType::String;
    default:return PropertyType::Unknown;
  }
}
ETrackedPropertyError propertyError(const SystemRead& s,TrackedDeviceIndex_t index,ETrackedDeviceProperty p,PropertyType expected) {
  if(!live(s)||index!=k_unTrackedDeviceIndex_Hmd)return TrackedProp_InvalidDevice;
  const auto type=propertyType(p);
  if(type==PropertyType::Unknown)return TrackedProp_UnknownProperty;
  if(type!=expected)return TrackedProp_WrongDataType;
  switch(p) {
    case Prop_ReportsTimeSinceVSync_Bool:case Prop_IsOnDesktop_Bool:
    case Prop_DisplayFrequency_Float:case Prop_DeviceClass_Int32:
    case Prop_EdidVendorID_Int32:case Prop_EdidProductID_Int32:
    case Prop_TrackingSystemName_String:case Prop_ModelNumber_String:case Prop_ManufacturerName_String:
      return TrackedProp_Success;
    default:return TrackedProp_ValueNotProvidedByDevice;
  }
}
void error(ETrackedPropertyError* out,ETrackedPropertyError value) { if(out)*out=value; }
}
void OpenVRSystem::unavailable(unsigned slot) noexcept {
  const uint64_t bit=uint64_t(1)<<slot;
  if(!(unavailable_.fetch_or(bit,std::memory_order_relaxed)&bit))source_.unsupported(slot);
}
void OpenVRSystem::GetRecommendedRenderTargetSize(uint32_t* w,uint32_t* h) {
  const auto s=source_.read();uint32_t width=0,height=0;
  if(geometryValid(s)){width=(std::max)(s.geometry.native.width[0],s.geometry.native.width[1]);height=(std::max)(s.geometry.native.height[0],s.geometry.native.height[1]);}
  if(w)*w=width;if(h)*h=height;
}
HmdMatrix44_t OpenVRSystem::GetProjectionMatrix(EVREye e,float nearZ,float farZ,EGraphicsAPIConvention api) {
  const auto s=source_.read();HmdMatrix44_t out{};
  if(geometryValid(s)&&eyeValid(e))projectionMatrix(s.geometry.native.views[unsigned(e)].fov,nearZ,farZ,api,out);
  return out;
}
void OpenVRSystem::GetProjectionRaw(EVREye e,float* l,float* r,float* t,float* b) {
  const auto s=source_.read();RawFov out{};
  if(geometryValid(s)&&eyeValid(e))out=s.geometry.raw[unsigned(e)];
  if(l)*l=out.left;if(r)*r=out.right;if(t)*t=out.top;if(b)*b=out.bottom;
}
DistortionCoordinates_t OpenVRSystem::ComputeDistortion(EVREye,float,float) { unavailable(3);return {}; }
HmdMatrix34_t OpenVRSystem::GetEyeToHeadTransform(EVREye e) {
  const auto s=source_.read();return geometryValid(s)&&eyeValid(e)?s.geometry.eyeToHead[unsigned(e)]:HmdMatrix34_t{};
}
bool OpenVRSystem::GetTimeSinceLastVsync(float* seconds,uint64_t* frame) {
  if(seconds)*seconds=0;if(frame)*frame=0;unavailable(5);return false;
}
int32_t OpenVRSystem::GetD3D9AdapterIndex() { unavailable(6);return -1; }
void OpenVRSystem::GetDXGIOutputInfo(int32_t* index) {const auto s=source_.read();if(index)*index=live(s)?s.adapterIndex:-1;}
bool OpenVRSystem::IsDisplayOnDesktop() { return false; }
bool OpenVRSystem::SetDisplayVisibility(bool) { unavailable(9);return false; }
void OpenVRSystem::GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin origin,float prediction,TrackedDevicePose_t* poses,uint32_t count) {
  if(!poses||!count)return;
  const auto s=source_.read();for(uint32_t i=0;i<count;++i)poses[i]=invalidPose();
  if(!live(s))return;poses[0]=invalidPose(true);
  if(!originValid(origin)||!std::isfinite(prediction))return;
  TrackedDevicePose_t p=invalidPose(true);
  if(source_.locateHead(s.generation,origin,prediction,p))poses[0]=p;
}
void OpenVRSystem::ResetSeatedZeroPose() { const auto s=source_.read();if(!live(s)||!source_.resetSeated(s.generation))unavailable(11); }
HmdMatrix34_t OpenVRSystem::GetSeatedZeroPoseToStandingAbsoluteTrackingPose() {
  const auto s=source_.read();if(live(s)&&s.seatedToStandingValid)return s.seatedToStanding;unavailable(12);return {};
}
HmdMatrix34_t OpenVRSystem::GetRawZeroPoseToStandingAbsoluteTrackingPose() {
  const auto s=source_.read();if(live(s)&&s.rawToStandingValid)return s.rawToStanding;unavailable(13);return {};
}
uint32_t OpenVRSystem::GetSortedTrackedDeviceIndicesOfClass(ETrackedDeviceClass cls,TrackedDeviceIndex_t* out,uint32_t capacity,TrackedDeviceIndex_t relative) {
  const auto s=source_.read();if(out)for(uint32_t i=0;i<capacity;++i)out[i]=k_unTrackedDeviceIndexInvalid;
  if(!live(s)||cls!=TrackedDeviceClass_HMD||(relative!=k_unTrackedDeviceIndex_Hmd&&relative!=k_unTrackedDeviceIndexInvalid))return 0;
  if(out&&capacity)out[0]=k_unTrackedDeviceIndex_Hmd;return 1;
}
EDeviceActivityLevel OpenVRSystem::GetTrackedDeviceActivityLevel(TrackedDeviceIndex_t) {unavailable(15);return k_EDeviceActivityLevel_Unknown;}
void OpenVRSystem::ApplyTransform(TrackedDevicePose_t* out,const TrackedDevicePose_t*,const HmdMatrix34_t*) {
  // Composition/velocity compatibility is a release blocker until independently
  // checked. A void API cannot signal unsupported except via invalid pose/log.
  if(out)*out=invalidPose();unavailable(16);
}
TrackedDeviceIndex_t OpenVRSystem::GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole) { return k_unTrackedDeviceIndexInvalid; }
ETrackedControllerRole OpenVRSystem::GetControllerRoleForTrackedDeviceIndex(TrackedDeviceIndex_t) { return TrackedControllerRole_Invalid; }
ETrackedDeviceClass OpenVRSystem::GetTrackedDeviceClass(TrackedDeviceIndex_t i) {const auto s=source_.read();return live(s)&&i==0?TrackedDeviceClass_HMD:TrackedDeviceClass_Invalid;}
bool OpenVRSystem::IsTrackedDeviceConnected(TrackedDeviceIndex_t i) {const auto s=source_.read();return live(s)&&i==0;}
bool OpenVRSystem::GetBoolTrackedDeviceProperty(TrackedDeviceIndex_t i,ETrackedDeviceProperty p,ETrackedPropertyError* out) {
  const auto s=source_.read();const auto e=propertyError(s,i,p,PropertyType::Bool);error(out,e);if(e==TrackedProp_UnknownProperty||e==TrackedProp_ValueNotProvidedByDevice)unavailable(21);return false;
}
float OpenVRSystem::GetFloatTrackedDeviceProperty(TrackedDeviceIndex_t i,ETrackedDeviceProperty p,ETrackedPropertyError* out) {
  const auto s=source_.read();auto e=propertyError(s,i,p,PropertyType::Float);float value=0;
  if(e==TrackedProp_Success){if(s.displayFrequencyAvailable&&std::isfinite(s.displayFrequency)&&s.displayFrequency>0)value=s.displayFrequency;else e=TrackedProp_ValueNotProvidedByDevice;}
  error(out,e);if(e==TrackedProp_UnknownProperty||e==TrackedProp_ValueNotProvidedByDevice)unavailable(22);return value;
}
int32_t OpenVRSystem::GetInt32TrackedDeviceProperty(TrackedDeviceIndex_t i,ETrackedDeviceProperty p,ETrackedPropertyError* out) {
  const auto s=source_.read();auto e=propertyError(s,i,p,PropertyType::Int);int32_t value=0;
  if(e==TrackedProp_Success){if(p==Prop_DeviceClass_Int32)value=TrackedDeviceClass_HMD;else e=TrackedProp_ValueNotProvidedByDevice;}
  error(out,e);if(e==TrackedProp_UnknownProperty||e==TrackedProp_ValueNotProvidedByDevice)unavailable(23);return value;
}
uint64_t OpenVRSystem::GetUint64TrackedDeviceProperty(TrackedDeviceIndex_t i,ETrackedDeviceProperty p,ETrackedPropertyError* out) {
  const auto s=source_.read();const auto e=propertyError(s,i,p,PropertyType::Uint);error(out,e);if(e==TrackedProp_UnknownProperty||e==TrackedProp_ValueNotProvidedByDevice)unavailable(24);return 0;
}
HmdMatrix34_t OpenVRSystem::GetMatrix34TrackedDeviceProperty(TrackedDeviceIndex_t i,ETrackedDeviceProperty p,ETrackedPropertyError* out) {
  const auto s=source_.read();const auto e=propertyError(s,i,p,PropertyType::Matrix);error(out,e);if(e==TrackedProp_UnknownProperty||e==TrackedProp_ValueNotProvidedByDevice)unavailable(25);return identity();
}
uint32_t OpenVRSystem::GetStringTrackedDeviceProperty(TrackedDeviceIndex_t i,ETrackedDeviceProperty p,char* buffer,uint32_t capacity,ETrackedPropertyError* out) {
  if(buffer&&capacity)buffer[0]=0;
  const auto s=source_.read();auto e=propertyError(s,i,p,PropertyType::String);const char* value=nullptr;size_t limit=0;
  if(e==TrackedProp_Success){
    if(p==Prop_ModelNumber_String){value=s.systemName;limit=sizeof(s.systemName);}
    else if(p==Prop_TrackingSystemName_String){value=s.runtimeName;limit=sizeof(s.runtimeName);}
    else e=TrackedProp_ValueNotProvidedByDevice;
  }
  size_t length=0;if(value)while(length<limit&&value[length])++length;
  if(value&&(!length||length==limit))e=TrackedProp_ValueNotProvidedByDevice;
  if(e!=TrackedProp_Success){error(out,e);if(e==TrackedProp_UnknownProperty||e==TrackedProp_ValueNotProvidedByDevice)unavailable(26);return 0;}
  const uint32_t needed=uint32_t(length+1);
  if(!buffer||capacity<needed){error(out,TrackedProp_BufferTooSmall);return needed;}
  std::memcpy(buffer,value,needed);error(out,TrackedProp_Success);return needed;
}
bool OpenVRSystem::PollNextEvent(VREvent_t* event,uint32_t size) {
  return PollNextEventWithPose(TrackingUniverseSeated,event,size,nullptr);
}
bool OpenVRSystem::PollNextEventWithPose(ETrackingUniverseOrigin origin,VREvent_t* event,uint32_t size,TrackedDevicePose_t* pose) {
  if(pose)*pose=invalidPose();if(event)std::memset(event,0,(std::min)(size,uint32_t(sizeof(*event))));
  if(!event||size<sizeof(*event)||!originValid(origin))return false;
  const auto s=source_.read();if(!live(s))return false;
  VREvent_t next{};TrackedDevicePose_t atEvent=invalidPose();
  if(!source_.pollEvent(s.generation,origin,next,atEvent))return false;
  *event=next;if(pose)*pose=atEvent;return true;
}
HiddenAreaMesh_t OpenVRSystem::GetHiddenAreaMesh(EVREye) { return {}; }
bool OpenVRSystem::GetControllerState(TrackedDeviceIndex_t,VRControllerState_t* state) {if(state)*state={};return false;}
bool OpenVRSystem::GetControllerStateWithPose(ETrackingUniverseOrigin,TrackedDeviceIndex_t,VRControllerState_t* state,TrackedDevicePose_t* pose) {if(state)*state={};if(pose)*pose=invalidPose();return false;}
void OpenVRSystem::TriggerHapticPulse(TrackedDeviceIndex_t,uint32_t,unsigned short) {unavailable(34);}
bool OpenVRSystem::CaptureInputFocus() {unavailable(37);return false;}
void OpenVRSystem::ReleaseInputFocus() {unavailable(38);}
bool OpenVRSystem::IsInputFocusCapturedByAnotherProcess() {
  const auto s=source_.read();if(live(s)&&s.focusKnown)return !s.focused;unavailable(39);return false;
}
uint32_t OpenVRSystem::DriverDebugRequest(TrackedDeviceIndex_t,const char*,char* response,uint32_t capacity) {if(response&&capacity)response[0]=0;unavailable(40);return 0;}
EVRFirmwareError OpenVRSystem::PerformFirmwareUpdate(TrackedDeviceIndex_t) {unavailable(41);return VRFirmwareError_Fail;}
void OpenVRSystem::AcknowledgeQuit_Exiting() {unavailable(42);}
void OpenVRSystem::AcknowledgeQuit_UserPrompt() {unavailable(43);}

const char* OpenVRSystem::GetPropErrorNameFromEnum(ETrackedPropertyError value) {
  static const struct { ETrackedPropertyError value; const char* name; } names[]={
    {TrackedProp_Success, "TrackedProp_Success"},
    {TrackedProp_WrongDataType, "TrackedProp_WrongDataType"},
    {TrackedProp_WrongDeviceClass, "TrackedProp_WrongDeviceClass"},
    {TrackedProp_BufferTooSmall, "TrackedProp_BufferTooSmall"},
    {TrackedProp_UnknownProperty, "TrackedProp_UnknownProperty"},
    {TrackedProp_InvalidDevice, "TrackedProp_InvalidDevice"},
    {TrackedProp_CouldNotContactServer, "TrackedProp_CouldNotContactServer"},
    {TrackedProp_ValueNotProvidedByDevice, "TrackedProp_ValueNotProvidedByDevice"},
    {TrackedProp_StringExceedsMaximumLength, "TrackedProp_StringExceedsMaximumLength"},
    {TrackedProp_NotYetAvailable, "TrackedProp_NotYetAvailable"}
  };
  for(const auto& entry:names)if(entry.value==value)return entry.name;
  return "UnknownPropertyError";
}

const char* OpenVRSystem::GetEventTypeNameFromEnum(EVREventType value) {
  static const struct { EVREventType value; const char* name; } names[]={
    {VREvent_None, "VREvent_None"},
    {VREvent_TrackedDeviceActivated, "VREvent_TrackedDeviceActivated"},
    {VREvent_TrackedDeviceDeactivated, "VREvent_TrackedDeviceDeactivated"},
    {VREvent_TrackedDeviceUpdated, "VREvent_TrackedDeviceUpdated"},
    {VREvent_TrackedDeviceUserInteractionStarted, "VREvent_TrackedDeviceUserInteractionStarted"},
    {VREvent_TrackedDeviceUserInteractionEnded, "VREvent_TrackedDeviceUserInteractionEnded"},
    {VREvent_IpdChanged, "VREvent_IpdChanged"},
    {VREvent_EnterStandbyMode, "VREvent_EnterStandbyMode"},
    {VREvent_LeaveStandbyMode, "VREvent_LeaveStandbyMode"},
    {VREvent_TrackedDeviceRoleChanged, "VREvent_TrackedDeviceRoleChanged"},
    {VREvent_ButtonPress, "VREvent_ButtonPress"},
    {VREvent_ButtonUnpress, "VREvent_ButtonUnpress"},
    {VREvent_ButtonTouch, "VREvent_ButtonTouch"},
    {VREvent_ButtonUntouch, "VREvent_ButtonUntouch"},
    {VREvent_MouseMove, "VREvent_MouseMove"},
    {VREvent_MouseButtonDown, "VREvent_MouseButtonDown"},
    {VREvent_MouseButtonUp, "VREvent_MouseButtonUp"},
    {VREvent_FocusEnter, "VREvent_FocusEnter"},
    {VREvent_FocusLeave, "VREvent_FocusLeave"},
    {VREvent_Scroll, "VREvent_Scroll"},
    {VREvent_TouchPadMove, "VREvent_TouchPadMove"},
    {VREvent_InputFocusCaptured, "VREvent_InputFocusCaptured"},
    {VREvent_InputFocusReleased, "VREvent_InputFocusReleased"},
    {VREvent_SceneFocusLost, "VREvent_SceneFocusLost"},
    {VREvent_SceneFocusGained, "VREvent_SceneFocusGained"},
    {VREvent_SceneApplicationChanged, "VREvent_SceneApplicationChanged"},
    {VREvent_SceneFocusChanged, "VREvent_SceneFocusChanged"},
    {VREvent_InputFocusChanged, "VREvent_InputFocusChanged"},
    {VREvent_HideRenderModels, "VREvent_HideRenderModels"},
    {VREvent_ShowRenderModels, "VREvent_ShowRenderModels"},
    {VREvent_OverlayShown, "VREvent_OverlayShown"},
    {VREvent_OverlayHidden, "VREvent_OverlayHidden"},
    {VREvent_DashboardActivated, "VREvent_DashboardActivated"},
    {VREvent_DashboardDeactivated, "VREvent_DashboardDeactivated"},
    {VREvent_DashboardThumbSelected, "VREvent_DashboardThumbSelected"},
    {VREvent_DashboardRequested, "VREvent_DashboardRequested"},
    {VREvent_ResetDashboard, "VREvent_ResetDashboard"},
    {VREvent_RenderToast, "VREvent_RenderToast"},
    {VREvent_ImageLoaded, "VREvent_ImageLoaded"},
    {VREvent_ShowKeyboard, "VREvent_ShowKeyboard"},
    {VREvent_HideKeyboard, "VREvent_HideKeyboard"},
    {VREvent_OverlayGamepadFocusGained, "VREvent_OverlayGamepadFocusGained"},
    {VREvent_OverlayGamepadFocusLost, "VREvent_OverlayGamepadFocusLost"},
    {VREvent_OverlaySharedTextureChanged, "VREvent_OverlaySharedTextureChanged"},
    {VREvent_DashboardGuideButtonDown, "VREvent_DashboardGuideButtonDown"},
    {VREvent_DashboardGuideButtonUp, "VREvent_DashboardGuideButtonUp"},
    {VREvent_Notification_Shown, "VREvent_Notification_Shown"},
    {VREvent_Notification_Hidden, "VREvent_Notification_Hidden"},
    {VREvent_Notification_BeginInteraction, "VREvent_Notification_BeginInteraction"},
    {VREvent_Notification_Destroyed, "VREvent_Notification_Destroyed"},
    {VREvent_Quit, "VREvent_Quit"},
    {VREvent_ProcessQuit, "VREvent_ProcessQuit"},
    {VREvent_QuitAborted_UserPrompt, "VREvent_QuitAborted_UserPrompt"},
    {VREvent_QuitAcknowledged, "VREvent_QuitAcknowledged"},
    {VREvent_DriverRequestedQuit, "VREvent_DriverRequestedQuit"},
    {VREvent_ChaperoneDataHasChanged, "VREvent_ChaperoneDataHasChanged"},
    {VREvent_ChaperoneUniverseHasChanged, "VREvent_ChaperoneUniverseHasChanged"},
    {VREvent_ChaperoneTempDataHasChanged, "VREvent_ChaperoneTempDataHasChanged"},
    {VREvent_ChaperoneSettingsHaveChanged, "VREvent_ChaperoneSettingsHaveChanged"},
    {VREvent_SeatedZeroPoseReset, "VREvent_SeatedZeroPoseReset"},
    {VREvent_AudioSettingsHaveChanged, "VREvent_AudioSettingsHaveChanged"},
    {VREvent_BackgroundSettingHasChanged, "VREvent_BackgroundSettingHasChanged"},
    {VREvent_CameraSettingsHaveChanged, "VREvent_CameraSettingsHaveChanged"},
    {VREvent_ReprojectionSettingHasChanged, "VREvent_ReprojectionSettingHasChanged"},
    {VREvent_StatusUpdate, "VREvent_StatusUpdate"},
    {VREvent_MCImageUpdated, "VREvent_MCImageUpdated"},
    {VREvent_FirmwareUpdateStarted, "VREvent_FirmwareUpdateStarted"},
    {VREvent_FirmwareUpdateFinished, "VREvent_FirmwareUpdateFinished"},
    {VREvent_KeyboardClosed, "VREvent_KeyboardClosed"},
    {VREvent_KeyboardCharInput, "VREvent_KeyboardCharInput"},
    {VREvent_KeyboardDone, "VREvent_KeyboardDone"},
    {VREvent_ApplicationTransitionStarted, "VREvent_ApplicationTransitionStarted"},
    {VREvent_ApplicationTransitionAborted, "VREvent_ApplicationTransitionAborted"},
    {VREvent_ApplicationTransitionNewAppStarted, "VREvent_ApplicationTransitionNewAppStarted"},
    {VREvent_Compositor_MirrorWindowShown, "VREvent_Compositor_MirrorWindowShown"},
    {VREvent_Compositor_MirrorWindowHidden, "VREvent_Compositor_MirrorWindowHidden"},
    {VREvent_Compositor_ChaperoneBoundsShown, "VREvent_Compositor_ChaperoneBoundsShown"},
    {VREvent_Compositor_ChaperoneBoundsHidden, "VREvent_Compositor_ChaperoneBoundsHidden"},
    {VREvent_TrackedCamera_StartVideoStream, "VREvent_TrackedCamera_StartVideoStream"},
    {VREvent_TrackedCamera_StopVideoStream, "VREvent_TrackedCamera_StopVideoStream"},
    {VREvent_TrackedCamera_PauseVideoStream, "VREvent_TrackedCamera_PauseVideoStream"},
    {VREvent_TrackedCamera_ResumeVideoStream, "VREvent_TrackedCamera_ResumeVideoStream"},
    {VREvent_PerformanceTest_EnableCapture, "VREvent_PerformanceTest_EnableCapture"},
    {VREvent_PerformanceTest_DisableCapture, "VREvent_PerformanceTest_DisableCapture"},
    {VREvent_PerformanceTest_FidelityLevel, "VREvent_PerformanceTest_FidelityLevel"},
    {VREvent_VendorSpecific_Reserved_Start, "VREvent_VendorSpecific_Reserved_Start"},
    {VREvent_VendorSpecific_Reserved_End, "VREvent_VendorSpecific_Reserved_End"}
  };
  for(const auto& entry:names)if(entry.value==value)return entry.name;
  return "UnknownEventType";
}

const char* OpenVRSystem::GetButtonIdNameFromEnum(EVRButtonId value) {
  static const struct { EVRButtonId value; const char* name; } names[]={
    {k_EButton_System, "k_EButton_System"},
    {k_EButton_ApplicationMenu, "k_EButton_ApplicationMenu"},
    {k_EButton_Grip, "k_EButton_Grip"},
    {k_EButton_DPad_Left, "k_EButton_DPad_Left"},
    {k_EButton_DPad_Up, "k_EButton_DPad_Up"},
    {k_EButton_DPad_Right, "k_EButton_DPad_Right"},
    {k_EButton_DPad_Down, "k_EButton_DPad_Down"},
    {k_EButton_A, "k_EButton_A"},
    {k_EButton_Axis0, "k_EButton_Axis0"},
    {k_EButton_Axis1, "k_EButton_Axis1"},
    {k_EButton_Axis2, "k_EButton_Axis2"},
    {k_EButton_Axis3, "k_EButton_Axis3"},
    {k_EButton_Axis4, "k_EButton_Axis4"},
    {k_EButton_SteamVR_Touchpad, "k_EButton_SteamVR_Touchpad"},
    {k_EButton_SteamVR_Trigger, "k_EButton_SteamVR_Trigger"},
    {k_EButton_Dashboard_Back, "k_EButton_Dashboard_Back"},
    {k_EButton_Max, "k_EButton_Max"}
  };
  for(const auto& entry:names)if(entry.value==value)return entry.name;
  return "UnknownButtonId";
}

const char* OpenVRSystem::GetControllerAxisTypeNameFromEnum(EVRControllerAxisType value) {
  static const struct { EVRControllerAxisType value; const char* name; } names[]={
    {k_eControllerAxis_None, "k_eControllerAxis_None"},
    {k_eControllerAxis_TrackPad, "k_eControllerAxis_TrackPad"},
    {k_eControllerAxis_Joystick, "k_eControllerAxis_Joystick"},
    {k_eControllerAxis_Trigger, "k_eControllerAxis_Trigger"}
  };
  for(const auto& entry:names)if(entry.value==value)return entry.name;
  return "UnknownControllerAxisType";
}
}
