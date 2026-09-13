#include "openvr_compositor.h"

#include <cstring>
#include <cmath>

namespace edvr::openxr {
namespace {

constexpr vr::EVRCompositorError kInvalid = vr::VRCompositorError_InvalidTexture;

void identity(vr::TrackedDevicePose_t& pose) noexcept {
  std::memset(&pose, 0, sizeof(pose));
  pose.mDeviceToAbsoluteTracking.m[0][0] = 1.0f;
  pose.mDeviceToAbsoluteTracking.m[1][1] = 1.0f;
  pose.mDeviceToAbsoluteTracking.m[2][2] = 1.0f;
  pose.eTrackingResult = vr::TrackingResult_Uninitialized;
  pose.bPoseIsValid = false;
  pose.bDeviceIsConnected = false;
}

void initialize(vr::TrackedDevicePose_t* poses, uint32_t count) noexcept {
  if (!poses) return;
  const uint32_t n = count < vr::k_unMaxTrackedDeviceCount
                          ? count : vr::k_unMaxTrackedDeviceCount;
  for (uint32_t i = 0; i < n; ++i) identity(poses[i]);
}

bool validOrigin(vr::ETrackingUniverseOrigin origin) noexcept {
  return origin == vr::TrackingUniverseSeated ||
         origin == vr::TrackingUniverseStanding ||
         origin == vr::TrackingUniverseRawAndUncalibrated;
}

void copyPose(vr::TrackedDevicePose_t* poses, uint32_t count,
              const vr::TrackedDevicePose_t& pose, bool connected,
              bool available) noexcept {
  if (!poses || count == 0) return;
  bool valid=available && connected && pose.bPoseIsValid &&
    (pose.eTrackingResult==vr::TrackingResult_Running_OK||pose.eTrackingResult==vr::TrackingResult_Running_OutOfRange);
  if(valid) {
    for(const auto& row:pose.mDeviceToAbsoluteTracking.m)for(float v:row)valid=valid&&std::isfinite(v);
    for(unsigned i=0;i<3;++i)valid=valid&&std::isfinite(pose.vVelocity.v[i])&&std::isfinite(pose.vAngularVelocity.v[i]);
  }
  if(!valid) {
    identity(poses[0]);poses[0].bDeviceIsConnected=connected;
    poses[0].eTrackingResult=connected?vr::TrackingResult_Running_OutOfRange:vr::TrackingResult_Uninitialized;
    return;
  }
  poses[0] = pose;
  poses[0].bDeviceIsConnected = connected;
}

} // namespace

OpenVRCompositor::OpenVRCompositor(CompositorSource* source) noexcept : source_(source) {}

void OpenVRCompositor::unsupported(unsigned slot) noexcept {
  if (!source_ || slot >= 32) return;
  const uint32_t bit = uint32_t(1u) << slot;
  if ((unsupportedMask_.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
    source_->compositorUnsupported(slot);
}

void OpenVRCompositor::SetTrackingSpace(vr::ETrackingUniverseOrigin origin) {
  if (!validOrigin(origin) || !source_) { unsupported(0); return; }
  const CompositorRead read = source_->compositorRead();
  if (read.generation == 0 || !source_->setTrackingSpace(read.generation, origin))
    unsupported(0);
}

vr::ETrackingUniverseOrigin OpenVRCompositor::GetTrackingSpace() {
  if (!source_) return vr::TrackingUniverseSeated;
  const CompositorRead read = source_->compositorRead();
  return validOrigin(read.origin) ? read.origin : vr::TrackingUniverseSeated;
}

vr::EVRCompositorError OpenVRCompositor::WaitGetPoses(
    vr::TrackedDevicePose_t* render, uint32_t renderCount,
    vr::TrackedDevicePose_t* game, uint32_t gameCount) {
  // Validate and initialize caller buffers before asking the source to wait.
  initialize(render, renderCount);
  initialize(game, gameCount);
  if ((renderCount && !render) || (gameCount && !game) || renderCount > vr::k_unMaxTrackedDeviceCount ||
      gameCount > vr::k_unMaxTrackedDeviceCount || !source_) return kInvalid;

  const CompositorRead before = source_->compositorRead();
  if (!before.connected || before.generation == 0) return kInvalid;
  CompositorRead published{};
  const vr::EVRCompositorError error = source_->waitPoses(before.generation, published);
  if (error != vr::VRCompositorError_None) return error;
  if (published.generation != before.generation || !published.connected || !published.posesAvailable ||
      !validOrigin(published.origin)) return kInvalid;
  copyPose(render, renderCount, published.renderPose, published.connected, true);
  copyPose(game, gameCount, published.gamePose, published.connected, true);
  return vr::VRCompositorError_None;
}

vr::EVRCompositorError OpenVRCompositor::GetLastPoses(
    vr::TrackedDevicePose_t* render, uint32_t renderCount,
    vr::TrackedDevicePose_t* game, uint32_t gameCount) {
  initialize(render, renderCount); initialize(game, gameCount);
  if ((renderCount && !render) || (gameCount && !game) ||
      renderCount > vr::k_unMaxTrackedDeviceCount ||
      gameCount > vr::k_unMaxTrackedDeviceCount) return kInvalid;
  if (!source_) return kInvalid;
  const CompositorRead read = source_->compositorRead();
  const bool connected=read.generation && read.connected;
  const bool sample = connected && read.posesAvailable && validOrigin(read.origin);
  copyPose(render, renderCount, read.renderPose, connected, sample);
  copyPose(game, gameCount, read.gamePose, connected, sample);
  return sample?vr::VRCompositorError_None:kInvalid;
}

vr::EVRCompositorError OpenVRCompositor::GetLastPoseForTrackedDeviceIndex(
    vr::TrackedDeviceIndex_t index, vr::TrackedDevicePose_t* render,
    vr::TrackedDevicePose_t* game) {
  if (render) identity(*render); if (game) identity(*game);
  if (index >= vr::k_unMaxTrackedDeviceCount) return vr::VRCompositorError_IndexOutOfRange;
  if (!source_) return vr::VRCompositorError_None;
  const CompositorRead read = source_->compositorRead();
  if (index == vr::k_unTrackedDeviceIndex_Hmd) {
    const bool connected=read.generation && read.connected;
    const bool sample=connected && read.posesAvailable && validOrigin(read.origin);
    copyPose(render, 1, read.renderPose, connected, sample);
    copyPose(game, 1, read.gamePose, connected, sample);
  }
  return vr::VRCompositorError_None;
}

vr::EVRCompositorError OpenVRCompositor::Submit(vr::EVREye eye, const vr::Texture_t* texture,
    const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags) {
  if (eye != vr::Eye_Left && eye != vr::Eye_Right) return vr::VRCompositorError_IndexOutOfRange;
  if (!source_) return kInvalid;
  const CompositorRead read = source_->compositorRead();
  if (!read.connected || read.generation == 0) return kInvalid;
  return source_->submitEye(read.generation, eye, texture, bounds, flags);
}

void OpenVRCompositor::ClearLastSubmittedFrame() {
  if (!source_) { unsupported(6); return; }
  const CompositorRead read = source_->compositorRead();
  if (read.generation == 0 || !source_->clearSubmitted(read.generation)) unsupported(6);
}

void OpenVRCompositor::PostPresentHandoff() {
  if (!source_) { unsupported(7); return; }
  const CompositorRead read = source_->compositorRead();
  if (read.generation == 0 || !source_->handoff(read.generation)) unsupported(7);
}

bool OpenVRCompositor::GetFrameTiming(vr::Compositor_FrameTiming*, uint32_t) {
  unsupported(8); return false;
}
float OpenVRCompositor::GetFrameTimeRemaining() { unsupported(9); return 0.0f; }
void OpenVRCompositor::FadeToColor(float, float, float, float, float, bool) { unsupported(10); }
void OpenVRCompositor::FadeGrid(float, bool) { unsupported(11); }
vr::EVRCompositorError OpenVRCompositor::SetSkyboxOverride(const vr::Texture_t* textures, uint32_t count) {
  if (!source_ || !textures || count != 6) { unsupported(12); return kInvalid; }
  const auto read=source_->compositorRead();
  if (!read.connected || !read.generation) return kInvalid;
  const auto result=source_->setSkybox(read.generation,textures,count);
  if (result!=vr::VRCompositorError_None) unsupported(12);
  return result;
}
void OpenVRCompositor::ClearSkyboxOverride() {
  if (!source_) { unsupported(13); return; }
  const auto read=source_->compositorRead();
  if (!read.connected || !read.generation || !source_->clearSkybox(read.generation)) unsupported(13);
}
void OpenVRCompositor::CompositorBringToFront() { unsupported(14); }
void OpenVRCompositor::CompositorGoToBack() { unsupported(15); }
void OpenVRCompositor::CompositorQuit() { unsupported(16); }
bool OpenVRCompositor::IsFullscreen() { unsupported(17); return false; }
uint32_t OpenVRCompositor::GetCurrentSceneFocusProcess() { unsupported(18); return 0; }
uint32_t OpenVRCompositor::GetLastFrameRenderer() { unsupported(19); return 0; }
bool OpenVRCompositor::CanRenderScene() {
  if (!source_) { unsupported(20); return false; }
  const CompositorRead read = source_->compositorRead();
  if (!read.canRenderKnown) { unsupported(20); return false; }
  return read.generation && read.connected && read.canRender;
}
void OpenVRCompositor::ShowMirrorWindow() { unsupported(21); }
void OpenVRCompositor::HideMirrorWindow() { unsupported(22); }
bool OpenVRCompositor::IsMirrorWindowVisible() { unsupported(23); return false; }
void OpenVRCompositor::CompositorDumpImages() { unsupported(24); }
bool OpenVRCompositor::ShouldAppRenderWithLowResources() { unsupported(25); return false; }
void OpenVRCompositor::ForceInterleavedReprojectionOn(bool) { unsupported(26); }
void OpenVRCompositor::ForceReconnectProcess() { unsupported(27); }
void OpenVRCompositor::SuspendRendering(bool) { unsupported(28); }

} // namespace edvr::openxr
