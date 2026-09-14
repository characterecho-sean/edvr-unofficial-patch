#pragma once

#include "../openvr/compat/openvr_v0_9_20.h"

namespace edvr::openxr {

// These helpers deliberately do not call through the OpenVR API.  The native
// module can report an initialization result before an OpenVR runtime exists,
// and the returned literals remain valid for the lifetime of the module.
inline const char* initErrorSymbol(vr::EVRInitError error) noexcept {
  switch (error) {
    case vr::VRInitError_None: return "VRInitError_None";
    case vr::VRInitError_Unknown: return "VRInitError_Unknown";
    case vr::VRInitError_Init_InstallationNotFound: return "VRInitError_Init_InstallationNotFound";
    case vr::VRInitError_Init_InstallationCorrupt: return "VRInitError_Init_InstallationCorrupt";
    case vr::VRInitError_Init_VRClientDLLNotFound: return "VRInitError_Init_VRClientDLLNotFound";
    case vr::VRInitError_Init_FileNotFound: return "VRInitError_Init_FileNotFound";
    case vr::VRInitError_Init_FactoryNotFound: return "VRInitError_Init_FactoryNotFound";
    case vr::VRInitError_Init_InterfaceNotFound: return "VRInitError_Init_InterfaceNotFound";
    case vr::VRInitError_Init_InvalidInterface: return "VRInitError_Init_InvalidInterface";
    case vr::VRInitError_Init_UserConfigDirectoryInvalid: return "VRInitError_Init_UserConfigDirectoryInvalid";
    case vr::VRInitError_Init_HmdNotFound: return "VRInitError_Init_HmdNotFound";
    case vr::VRInitError_Init_NotInitialized: return "VRInitError_Init_NotInitialized";
    case vr::VRInitError_Init_PathRegistryNotFound: return "VRInitError_Init_PathRegistryNotFound";
    case vr::VRInitError_Init_NoConfigPath: return "VRInitError_Init_NoConfigPath";
    case vr::VRInitError_Init_NoLogPath: return "VRInitError_Init_NoLogPath";
    case vr::VRInitError_Init_PathRegistryNotWritable: return "VRInitError_Init_PathRegistryNotWritable";
    case vr::VRInitError_Init_AppInfoInitFailed: return "VRInitError_Init_AppInfoInitFailed";
    case vr::VRInitError_Init_Retry: return "VRInitError_Init_Retry";
    case vr::VRInitError_Init_InitCanceledByUser: return "VRInitError_Init_InitCanceledByUser";
    case vr::VRInitError_Init_AnotherAppLaunching: return "VRInitError_Init_AnotherAppLaunching";
    case vr::VRInitError_Init_SettingsInitFailed: return "VRInitError_Init_SettingsInitFailed";
    case vr::VRInitError_Init_ShuttingDown: return "VRInitError_Init_ShuttingDown";
    case vr::VRInitError_Init_TooManyObjects: return "VRInitError_Init_TooManyObjects";
    case vr::VRInitError_Init_NoServerForBackgroundApp: return "VRInitError_Init_NoServerForBackgroundApp";
    case vr::VRInitError_Init_NotSupportedWithCompositor: return "VRInitError_Init_NotSupportedWithCompositor";
    case vr::VRInitError_Init_NotAvailableToUtilityApps: return "VRInitError_Init_NotAvailableToUtilityApps";
    case vr::VRInitError_Init_Internal: return "VRInitError_Init_Internal";
    case vr::VRInitError_Driver_Failed: return "VRInitError_Driver_Failed";
    case vr::VRInitError_Driver_Unknown: return "VRInitError_Driver_Unknown";
    case vr::VRInitError_Driver_HmdUnknown: return "VRInitError_Driver_HmdUnknown";
    case vr::VRInitError_Driver_NotLoaded: return "VRInitError_Driver_NotLoaded";
    case vr::VRInitError_Driver_RuntimeOutOfDate: return "VRInitError_Driver_RuntimeOutOfDate";
    case vr::VRInitError_Driver_HmdInUse: return "VRInitError_Driver_HmdInUse";
    case vr::VRInitError_Driver_NotCalibrated: return "VRInitError_Driver_NotCalibrated";
    case vr::VRInitError_Driver_CalibrationInvalid: return "VRInitError_Driver_CalibrationInvalid";
    case vr::VRInitError_Driver_HmdDisplayNotFound: return "VRInitError_Driver_HmdDisplayNotFound";
    case vr::VRInitError_IPC_ServerInitFailed: return "VRInitError_IPC_ServerInitFailed";
    case vr::VRInitError_IPC_ConnectFailed: return "VRInitError_IPC_ConnectFailed";
    case vr::VRInitError_IPC_SharedStateInitFailed: return "VRInitError_IPC_SharedStateInitFailed";
    case vr::VRInitError_IPC_CompositorInitFailed: return "VRInitError_IPC_CompositorInitFailed";
    case vr::VRInitError_IPC_MutexInitFailed: return "VRInitError_IPC_MutexInitFailed";
    case vr::VRInitError_IPC_Failed: return "VRInitError_IPC_Failed";
    case vr::VRInitError_Compositor_Failed: return "VRInitError_Compositor_Failed";
    case vr::VRInitError_Compositor_D3D11HardwareRequired: return "VRInitError_Compositor_D3D11HardwareRequired";
    case vr::VRInitError_Compositor_FirmwareRequiresUpdate: return "VRInitError_Compositor_FirmwareRequiresUpdate";
    case vr::VRInitError_VendorSpecific_UnableToConnectToOculusRuntime: return "VRInitError_VendorSpecific_UnableToConnectToOculusRuntime";
    case vr::VRInitError_VendorSpecific_HmdFound_CantOpenDevice: return "VRInitError_VendorSpecific_HmdFound_CantOpenDevice";
    case vr::VRInitError_VendorSpecific_HmdFound_UnableToRequestConfigStart: return "VRInitError_VendorSpecific_HmdFound_UnableToRequestConfigStart";
    case vr::VRInitError_VendorSpecific_HmdFound_NoStoredConfig: return "VRInitError_VendorSpecific_HmdFound_NoStoredConfig";
    case vr::VRInitError_VendorSpecific_HmdFound_ConfigTooBig: return "VRInitError_VendorSpecific_HmdFound_ConfigTooBig";
    case vr::VRInitError_VendorSpecific_HmdFound_ConfigTooSmall: return "VRInitError_VendorSpecific_HmdFound_ConfigTooSmall";
    case vr::VRInitError_VendorSpecific_HmdFound_UnableToInitZLib: return "VRInitError_VendorSpecific_HmdFound_UnableToInitZLib";
    case vr::VRInitError_VendorSpecific_HmdFound_CantReadFirmwareVersion: return "VRInitError_VendorSpecific_HmdFound_CantReadFirmwareVersion";
    case vr::VRInitError_VendorSpecific_HmdFound_UnableToSendUserDataStart: return "VRInitError_VendorSpecific_HmdFound_UnableToSendUserDataStart";
    case vr::VRInitError_VendorSpecific_HmdFound_UnableToGetUserDataStart: return "VRInitError_VendorSpecific_HmdFound_UnableToGetUserDataStart";
    case vr::VRInitError_VendorSpecific_HmdFound_UnableToGetUserDataNext: return "VRInitError_VendorSpecific_HmdFound_UnableToGetUserDataNext";
    case vr::VRInitError_VendorSpecific_HmdFound_UserDataAddressRange: return "VRInitError_VendorSpecific_HmdFound_UserDataAddressRange";
    case vr::VRInitError_VendorSpecific_HmdFound_UserDataError: return "VRInitError_VendorSpecific_HmdFound_UserDataError";
    case vr::VRInitError_VendorSpecific_HmdFound_ConfigFailedSanityCheck: return "VRInitError_VendorSpecific_HmdFound_ConfigFailedSanityCheck";
    case vr::VRInitError_Steam_SteamInstallationNotFound: return "VRInitError_Steam_SteamInstallationNotFound";
    default: return "VRInitError_Unknown";
  }
}

inline const char* initErrorDescription(vr::EVRInitError error) noexcept {
  switch (error) {
    case vr::VRInitError_None: return "Native OpenXR initialization succeeded";
    case vr::VRInitError_Unknown: return "Unknown native OpenXR initialization error";
    case vr::VRInitError_Init_InstallationNotFound: return "Legacy OpenVR: the installation was not found";
    case vr::VRInitError_Init_InstallationCorrupt: return "The native OpenXR startup configuration or installation is invalid";
    case vr::VRInitError_Init_VRClientDLLNotFound: return "Legacy OpenVR: the VR client DLL was not found";
    case vr::VRInitError_Init_FileNotFound: return "Legacy OpenVR: a required file was not found";
    case vr::VRInitError_Init_FactoryNotFound: return "Legacy OpenVR: a required factory was not found";
    case vr::VRInitError_Init_InterfaceNotFound: return "The requested native OpenXR compatibility interface was not found";
    case vr::VRInitError_Init_InvalidInterface: return "The requested native OpenXR compatibility interface is invalid";
    case vr::VRInitError_Init_UserConfigDirectoryInvalid: return "Legacy OpenVR: the user configuration directory is invalid";
    case vr::VRInitError_Init_HmdNotFound: return "No usable OpenXR headset or graphics startup path was available";
    case vr::VRInitError_Init_NotInitialized: return "The native OpenXR module has not been initialized";
    case vr::VRInitError_Init_PathRegistryNotFound: return "Legacy OpenVR: the path registry entry was not found";
    case vr::VRInitError_Init_NoConfigPath: return "Legacy OpenVR: no configuration path is available";
    case vr::VRInitError_Init_NoLogPath: return "Legacy OpenVR: no log path is available";
    case vr::VRInitError_Init_PathRegistryNotWritable: return "Legacy OpenVR: the path registry entry is not writable";
    case vr::VRInitError_Init_AppInfoInitFailed: return "Legacy OpenVR: application information could not be initialized";
    case vr::VRInitError_Init_Retry: return "Native OpenXR initialization is already in progress; retry later";
    case vr::VRInitError_Init_InitCanceledByUser: return "Legacy OpenVR: initialization was canceled by the user";
    case vr::VRInitError_Init_AnotherAppLaunching: return "Legacy OpenVR: another application is launching";
    case vr::VRInitError_Init_SettingsInitFailed: return "Legacy OpenVR: settings could not be initialized";
    case vr::VRInitError_Init_ShuttingDown: return "The native OpenXR module is shutting down";
    case vr::VRInitError_Init_TooManyObjects: return "The native OpenXR module exhausted its bounded initialization generation budget";
    case vr::VRInitError_Init_NoServerForBackgroundApp: return "Legacy OpenVR: no server is available for a background application";
    case vr::VRInitError_Init_NotSupportedWithCompositor: return "This application mode is not supported by the native OpenXR compositor bridge";
    case vr::VRInitError_Init_NotAvailableToUtilityApps: return "Legacy OpenVR: the service is unavailable to utility applications";
    case vr::VRInitError_Init_Internal: return "An internal native OpenXR initialization failure occurred";
    case vr::VRInitError_Driver_Failed: return "Legacy OpenVR: the driver failed to initialize";
    case vr::VRInitError_Driver_Unknown: return "Legacy OpenVR: the driver reported an unknown failure";
    case vr::VRInitError_Driver_HmdUnknown: return "Legacy OpenVR: the headset identity is unknown";
    case vr::VRInitError_Driver_NotLoaded: return "Legacy OpenVR: the driver is not loaded";
    case vr::VRInitError_Driver_RuntimeOutOfDate: return "Legacy OpenVR: the runtime is out of date for this driver";
    case vr::VRInitError_Driver_HmdInUse: return "Legacy OpenVR: the headset is already in use";
    case vr::VRInitError_Driver_NotCalibrated: return "Legacy OpenVR: the headset is not calibrated";
    case vr::VRInitError_Driver_CalibrationInvalid: return "Legacy OpenVR: the headset calibration is invalid";
    case vr::VRInitError_Driver_HmdDisplayNotFound: return "Legacy OpenVR: the headset display was not found";
    case vr::VRInitError_IPC_ServerInitFailed: return "Legacy OpenVR: server IPC initialization failed";
    case vr::VRInitError_IPC_ConnectFailed: return "Legacy OpenVR: connection to the service failed";
    case vr::VRInitError_IPC_SharedStateInitFailed: return "Legacy OpenVR: shared state initialization failed";
    case vr::VRInitError_IPC_CompositorInitFailed: return "Legacy OpenVR: compositor IPC initialization failed";
    case vr::VRInitError_IPC_MutexInitFailed: return "Legacy OpenVR: IPC mutex initialization failed";
    case vr::VRInitError_IPC_Failed: return "Legacy OpenVR: service IPC failed";
    case vr::VRInitError_Compositor_Failed: return "Legacy OpenVR: compositor initialization failed";
    case vr::VRInitError_Compositor_D3D11HardwareRequired: return "Legacy OpenVR: Direct3D 11 hardware is required";
    case vr::VRInitError_Compositor_FirmwareRequiresUpdate: return "Legacy OpenVR: headset firmware requires an update";
    case vr::VRInitError_VendorSpecific_UnableToConnectToOculusRuntime: return "Legacy OpenVR: unable to connect to the Oculus runtime";
    case vr::VRInitError_VendorSpecific_HmdFound_CantOpenDevice: return "Legacy OpenVR: a found headset device could not be opened";
    case vr::VRInitError_VendorSpecific_HmdFound_UnableToRequestConfigStart: return "Legacy OpenVR: unable to start the headset configuration request";
    case vr::VRInitError_VendorSpecific_HmdFound_NoStoredConfig: return "Legacy OpenVR: the headset has no stored configuration";
    case vr::VRInitError_VendorSpecific_HmdFound_ConfigTooBig: return "Legacy OpenVR: the headset configuration is too large";
    case vr::VRInitError_VendorSpecific_HmdFound_ConfigTooSmall: return "Legacy OpenVR: the headset configuration is too small";
    case vr::VRInitError_VendorSpecific_HmdFound_UnableToInitZLib: return "Legacy OpenVR: unable to initialize configuration compression";
    case vr::VRInitError_VendorSpecific_HmdFound_CantReadFirmwareVersion: return "Legacy OpenVR: unable to read the headset firmware version";
    case vr::VRInitError_VendorSpecific_HmdFound_UnableToSendUserDataStart: return "Legacy OpenVR: unable to start sending headset user data";
    case vr::VRInitError_VendorSpecific_HmdFound_UnableToGetUserDataStart: return "Legacy OpenVR: unable to start reading headset user data";
    case vr::VRInitError_VendorSpecific_HmdFound_UnableToGetUserDataNext: return "Legacy OpenVR: unable to read the next headset user data block";
    case vr::VRInitError_VendorSpecific_HmdFound_UserDataAddressRange: return "Legacy OpenVR: headset user data address is out of range";
    case vr::VRInitError_VendorSpecific_HmdFound_UserDataError: return "Legacy OpenVR: the headset reported a user data error";
    case vr::VRInitError_VendorSpecific_HmdFound_ConfigFailedSanityCheck: return "Legacy OpenVR: the headset configuration failed its sanity check";
    case vr::VRInitError_Steam_SteamInstallationNotFound: return "Legacy OpenVR: the Steam installation was not found";
    default: return "Unknown native OpenXR initialization error";
  }
}

} // namespace edvr::openxr
