#include "openvr_compositor.h"
#include "native_cpu_trace.h"
#include "native_trace.h"
#include "../common/frame_flag.h"
#include "../common/game_call_probe.h"

#include <windows.h>
#include <cstring>
#include <cmath>
#include <mutex>

namespace edvr::openxr {
namespace {

constexpr vr::EVRCompositorError kInvalid = vr::VRCompositorError_InvalidTexture;

// --- advanced.eye_origin_readers, part A1 (docs/design-transition-flash-
// engine-fix-2026-09-23.md): every WaitGetPoses/GetLastPoses call publishes
// the CALLER's own render/game array pointers -- Elite's memory, not ours --
// through frame_flag.h, so pose_reader_watch.cpp (d3d11.dll) can hardware-
// watch the exact address Elite passed us. When the graphics half has asked
// (frame_flag.h's poseReaderTraceRequested), this also captures this DLL's
// own call stack: who is calling INTO the OpenVR API. Off, the cost is one
// relaxed read of the request flag plus the handful of volatile writes
// publishPoseReaderCall already does for every other WaitGetPoses/
// GetLastPoses field.

// A small FNV-1a-64 dedupe table for the captured stacks, capped at 16 and
// logged once each -- eye_origin_trace.h's StackTable shape, kept as its
// own copy here rather than shared: that header has no game or Windows
// dependency by design, and this is a different process (openvr_api.dll,
// not d3d11.dll) with its own log to write to.
constexpr uint32_t kPoseReaderStackCap = 16;
struct PoseReaderStackTable {
    uint64_t hash[kPoseReaderStackCap]{};
    char     chain[kPoseReaderStackCap][512]{};
    uint32_t used = 0;
};
std::mutex g_poseReaderStackMutex;
PoseReaderStackTable g_poseReaderStackTable;

uint64_t fnv1a64OfChain(const char* s) noexcept {
    uint64_t h = 1469598103934665603ull;
    for (; *s; ++s) {
        h ^= static_cast<unsigned char>(*s);
        h *= 1099511628211ull;
    }
    return h;
}

void noteGameCallStack(bool wasGetLastPoses) noexcept {
    const GameCallStack stack = captureGameCallStack();
    if (stack.gameFrames == 0) return;
    const uint64_t hash = fnv1a64OfChain(stack.rvas);
    bool isNew = false;
    {
        std::lock_guard<std::mutex> lock(g_poseReaderStackMutex);
        PoseReaderStackTable& t = g_poseReaderStackTable;
        bool found = false;
        for (uint32_t i = 0; i < t.used; ++i) {
            if (t.hash[i] == hash && std::strcmp(t.chain[i], stack.rvas) == 0) { found = true; break; }
        }
        if (!found && t.used < kPoseReaderStackCap) {
            t.hash[t.used] = hash;
            strncpy_s(t.chain[t.used], sizeof(t.chain[t.used]), stack.rvas, _TRUNCATE);
            ++t.used;
            isNew = true;
        }
    }
    if (isNew) {
        nativeTracePrintf(
            "pose_reader_call_stack,api=%s,game_frames=%u,game_rvas=%s\n",
            wasGetLastPoses ? "GetLastPoses" : "WaitGetPoses", stack.gameFrames, stack.rvas);
    }
}

void notePoseReaderCall(bool wasGetLastPoses, vr::TrackedDevicePose_t* render, uint32_t renderCount,
                        vr::TrackedDevicePose_t* game, uint32_t gameCount) noexcept {
    ULONG_PTR stackLow = 0, stackHigh = 0;
    GetCurrentThreadStackLimits(&stackLow, &stackHigh);
    const auto onStack = [&](void* p) noexcept {
        if (!p) return false;
        const auto a = reinterpret_cast<ULONG_PTR>(p);
        return a >= stackLow && a < stackHigh;
    };
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);

    PoseReaderCall call{};
    call.renderPtr = reinterpret_cast<uint64_t>(render);
    call.gamePtr = reinterpret_cast<uint64_t>(game);
    call.qpc = static_cast<uint64_t>(qpc.QuadPart);
    call.renderCount = renderCount;
    call.gameCount = gameCount;
    call.threadId = GetCurrentThreadId();
    call.renderOnStack = onStack(render);
    call.gameOnStack = onStack(game);
    call.wasGetLastPoses = wasGetLastPoses;
    publishPoseReaderCall(call);

    if (poseReaderTraceRequested()) noteGameCallStack(wasGetLastPoses);
}

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
  NativeCpuTraceSpan trace(EdvrCpuWaitGetPoses);
  notePoseReaderCall(false, render, renderCount, game, gameCount);
  // Validate and initialize caller buffers before asking the source to wait.
  initialize(render, renderCount);
  initialize(game, gameCount);
  if ((renderCount && !render) || (gameCount && !game) || renderCount > vr::k_unMaxTrackedDeviceCount ||
      gameCount > vr::k_unMaxTrackedDeviceCount || !source_) return trace.finish(kInvalid);

  const CompositorRead before = source_->compositorRead();
  if (!before.connected || before.generation == 0) return trace.finish(kInvalid);
  CompositorRead published{};
  const vr::EVRCompositorError error = source_->waitPoses(before.generation, published);
  if (error != vr::VRCompositorError_None) return trace.finish(error);
  if (published.generation != before.generation || !published.connected || !published.posesAvailable ||
      !validOrigin(published.origin)) return trace.finish(kInvalid);
  copyPose(render, renderCount, published.renderPose, published.connected, true);
  copyPose(game, gameCount, published.gamePose, published.connected, true);
  return trace.finish(vr::VRCompositorError_None);
}

vr::EVRCompositorError OpenVRCompositor::GetLastPoses(
    vr::TrackedDevicePose_t* render, uint32_t renderCount,
    vr::TrackedDevicePose_t* game, uint32_t gameCount) {
  NativeCpuTraceSpan trace(EdvrCpuGetLastPoses);
  notePoseReaderCall(true, render, renderCount, game, gameCount);
  initialize(render, renderCount); initialize(game, gameCount);
  if ((renderCount && !render) || (gameCount && !game) ||
      renderCount > vr::k_unMaxTrackedDeviceCount ||
      gameCount > vr::k_unMaxTrackedDeviceCount) return trace.finish(kInvalid);
  if (!source_) return trace.finish(kInvalid);
  const CompositorRead read = source_->compositorRead();
  const bool connected=read.generation && read.connected;
  const bool sample = connected && read.posesAvailable && validOrigin(read.origin);
  copyPose(render, renderCount, read.renderPose, connected, sample);
  copyPose(game, gameCount, read.gamePose, connected, sample);
  return trace.finish(sample?vr::VRCompositorError_None:kInvalid);
}

vr::EVRCompositorError OpenVRCompositor::GetLastPoseForTrackedDeviceIndex(
    vr::TrackedDeviceIndex_t index, vr::TrackedDevicePose_t* render,
    vr::TrackedDevicePose_t* game) {
  NativeCpuTraceSpan trace(EdvrCpuGetLastPoseForTrackedDeviceIndex);
  if (render) identity(*render); if (game) identity(*game);
  if (index >= vr::k_unMaxTrackedDeviceCount) return trace.finish(vr::VRCompositorError_IndexOutOfRange);
  if (!source_) return trace.finish(vr::VRCompositorError_None);
  const CompositorRead read = source_->compositorRead();
  if (index == vr::k_unTrackedDeviceIndex_Hmd) {
    const bool connected=read.generation && read.connected;
    const bool sample=connected && read.posesAvailable && validOrigin(read.origin);
    copyPose(render, 1, read.renderPose, connected, sample);
    copyPose(game, 1, read.gamePose, connected, sample);
  }
  return trace.finish(vr::VRCompositorError_None);
}

vr::EVRCompositorError OpenVRCompositor::Submit(vr::EVREye eye, const vr::Texture_t* texture,
    const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags) {
  NativeCpuTraceSpan trace(EdvrCpuSubmit);
  if (eye != vr::Eye_Left && eye != vr::Eye_Right) return trace.finish(vr::VRCompositorError_IndexOutOfRange);
  if (!source_) return trace.finish(kInvalid);
  const CompositorRead read = source_->compositorRead();
  if (!read.connected || read.generation == 0) return trace.finish(kInvalid);
  return trace.finish(source_->submitEye(read.generation, eye, texture, bounds, flags));
}

void OpenVRCompositor::ClearLastSubmittedFrame() {
  if (!source_) { unsupported(6); return; }
  const CompositorRead read = source_->compositorRead();
  if (read.generation == 0 || !source_->clearSubmitted(read.generation)) unsupported(6);
}

void OpenVRCompositor::PostPresentHandoff() {
  NativeCpuTraceSpan trace(EdvrCpuPostPresentHandoff);
  if (!source_) { unsupported(7);trace.finishVoid(0); return; }
  const CompositorRead read = source_->compositorRead();
  const bool result=read.generation != 0 && source_->handoff(read.generation);
  if (!result) unsupported(7);trace.finishVoid(result?1:0);
}

bool OpenVRCompositor::GetFrameTiming(vr::Compositor_FrameTiming*, uint32_t) {
  NativeCpuTraceSpan trace(EdvrCpuGetFrameTiming);unsupported(8); return trace.finish(false);
}
float OpenVRCompositor::GetFrameTimeRemaining() { NativeCpuTraceSpan trace(EdvrCpuGetFrameTimeRemaining);unsupported(9); return trace.finish(0.0f); }
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
  NativeCpuTraceSpan trace(EdvrCpuCanRenderScene);
  if (!source_) { unsupported(20); return trace.finish(false); }
  const CompositorRead read = source_->compositorRead();
  if (!read.canRenderKnown) { unsupported(20); return trace.finish(false); }
  return trace.finish(bool(read.generation && read.connected && read.canRender));
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
