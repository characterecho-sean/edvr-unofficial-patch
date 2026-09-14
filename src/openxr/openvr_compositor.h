#pragma once

// Historical IVRCompositor facade. This is an internal compatibility boundary,
// not an advertisement of complete compositor support. The CompositorSource
// must outlive this object.
#include "compositor_source.h"

#include <atomic>
#include <cstdint>

namespace edvr::openxr {

class OpenVRCompositor final : public vr::IVRCompositor {
 public:
  explicit OpenVRCompositor(CompositorSource* source) noexcept;
  ~OpenVRCompositor() = default;

  void SetTrackingSpace(vr::ETrackingUniverseOrigin origin) override;
  vr::ETrackingUniverseOrigin GetTrackingSpace() override;
  vr::EVRCompositorError WaitGetPoses(vr::TrackedDevicePose_t*, uint32_t,
                                      vr::TrackedDevicePose_t*, uint32_t) override;
  vr::EVRCompositorError GetLastPoses(vr::TrackedDevicePose_t*, uint32_t,
                                      vr::TrackedDevicePose_t*, uint32_t) override;
  vr::EVRCompositorError GetLastPoseForTrackedDeviceIndex(
      vr::TrackedDeviceIndex_t, vr::TrackedDevicePose_t*, vr::TrackedDevicePose_t*) override;
  vr::EVRCompositorError Submit(vr::EVREye, const vr::Texture_t*,
                                const vr::VRTextureBounds_t*, vr::EVRSubmitFlags) override;
  void ClearLastSubmittedFrame() override;
  void PostPresentHandoff() override;
  bool GetFrameTiming(vr::Compositor_FrameTiming*, uint32_t) override;
  float GetFrameTimeRemaining() override;
  void FadeToColor(float, float, float, float, float, bool) override;
  void FadeGrid(float, bool) override;
  vr::EVRCompositorError SetSkyboxOverride(const vr::Texture_t*, uint32_t) override;
  void ClearSkyboxOverride() override;
  void CompositorBringToFront() override;
  void CompositorGoToBack() override;
  void CompositorQuit() override;
  bool IsFullscreen() override;
  uint32_t GetCurrentSceneFocusProcess() override;
  uint32_t GetLastFrameRenderer() override;
  bool CanRenderScene() override;
  void ShowMirrorWindow() override;
  void HideMirrorWindow() override;
  bool IsMirrorWindowVisible() override;
  void CompositorDumpImages() override;
  bool ShouldAppRenderWithLowResources() override;
  void ForceInterleavedReprojectionOn(bool) override;
  void ForceReconnectProcess() override;
  void SuspendRendering(bool) override;

 private:
  void unsupported(unsigned slot) noexcept;
  CompositorSource* source_;
  std::atomic<uint32_t> unsupportedMask_{0};
};

} // namespace edvr::openxr
