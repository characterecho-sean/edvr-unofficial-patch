#pragma once
namespace edvr {
// Present thread: arm the pinned camera wrappers, publish the native on-foot
// gate and drain bounded default diagnostics. No configuration key of its own.
void nativePreviewCameraBoundary(bool wanted,bool active);
void nativePreviewCameraStop() noexcept;
}
