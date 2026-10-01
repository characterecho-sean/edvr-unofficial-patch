// ID3D11Device vtable slots the device hook patches for the format-support log
// (src\d3d11\format_support_log.h). The device's create slots are constants in
// device_hook.cpp; these two live here so the rig can hold them against the SDK.
//
// Counted from IUnknown (0-2) in declaration order: CreateBuffer 3, CreateTexture1D 4,
// ... CreateSamplerState 23, CreateQuery 24, CreatePredicate 25, CreateCounter 26,
// CreateDeferredContext 27, OpenSharedResource 28, CheckFormatSupport 29,
// CheckMultisampleQualityLevels 30, CheckCounterInfo 31, CheckCounter 32,
// CheckFeatureSupport 33. tools\format_support_test\device_slots.cpp compiles the same
// two against the SDK's own ID3D11DeviceVtbl (offsetof), so a miscount fails the build
// instead of hooking CheckMultisampleQualityLevels or CheckCounter on a user's startup.
#pragma once

#include <cstddef>

namespace edvr {

constexpr size_t kDevSlotCheckFormatSupport  = 29;
constexpr size_t kDevSlotCheckFeatureSupport = 33;

}  // namespace edvr
