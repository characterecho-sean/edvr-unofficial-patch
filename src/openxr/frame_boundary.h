#pragma once
#include "session_state.h"
#include "../openvr/compat/openvr_v0_9_20.h"
#include <thread>

namespace edvr::openxr {

// Callbacks execute on the frame owner's thread, under its runtime operation
// lease. They must not re-enter the boundary. The source owns copied pixels,
// located views and layer backing storage until endFrame returns.
struct FrameSink {
  virtual ~FrameSink() = default;
  virtual vr::EVRCompositorError capture(vr::EVREye, const vr::Texture_t*,
      const vr::VRTextureBounds_t*, vr::EVRSubmitFlags, bool copyPixels) = 0;
  virtual XrResult compose(XrCompositionLayerProjection&) = 0;
};

// Submit pairing policy shared by a future IVRCompositor and the native test.
// SessionState, the sink, and their borrowed handles outlive this object. The
// owner serializes operations and excludes destruction using RuntimeGate.
// This is not yet the historical 29-slot IVRCompositor facade.
class FrameBoundary final {
 public:
  FrameBoundary(SessionState& session, FrameSink& sink)
      : session_(session), sink_(sink), thread_(std::this_thread::get_id()) {}
  FrameBoundary(const FrameBoundary&) = delete;
  FrameBoundary& operator=(const FrameBoundary&) = delete;

  XrResult waitAndBegin() {
    if (!ownerThread()) return XR_ERROR_CALL_ORDER_INVALID;
    if (failed_) return lastResult_;
    // Missing/one-eye submits cannot strand the preceding begun frame.
    const auto closed = clear();
    if (closed != XR_SUCCESS) return closed;
    frame_ = {};
    lastResult_ = session_.waitAndBegin(frame_);
    if (XR_FAILED(lastResult_)) failed_ = true;
    return lastResult_;
  }

  Frame frame() const { return frame_; } // owner thread only; copy is not a token setter
  void setGeometryReady(bool ready) {
    if (ownerThread() && session_.frameOpen() && !accepted_[0] && !accepted_[1])
      geometryReady_ = ready;
  }
  vr::EVRCompositorError submit(vr::EVREye eye, const vr::Texture_t* texture,
      const vr::VRTextureBounds_t* bounds = nullptr,
      vr::EVRSubmitFlags flags = vr::Submit_Default) {
    if (!ownerThread()) return vr::VRCompositorError_InvalidTexture;
    if (eye != vr::Eye_Left && eye != vr::Eye_Right)
      return vr::VRCompositorError_IndexOutOfRange;
    const auto index = unsigned(eye);
    // v0.9.20 has no AlreadySubmitted or request-failed enum. InvalidTexture
    // is the deliberately conservative historical error for unusable submits.
    if (failed_ || !session_.frameOpen() || accepted_[index])
      return vr::VRCompositorError_InvalidTexture;
    const bool pixels = geometryReady_ && frame_.shouldRender && !session_.terminal();
    const auto captured = sink_.capture(eye, texture, bounds, flags, pixels);
    if (captured != vr::VRCompositorError_None) return captured;
    accepted_[index] = true;
    if (!accepted_[0] || !accepted_[1]) return vr::VRCompositorError_None;

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    if (pixels) {
      const auto composed = sink_.compose(layer);
      if (composed != XR_SUCCESS) {
        // A hard external XR failure may have invalidated the session. The
        // owner must destroy it; do not dispatch endFrame through lost handles.
        if (!XR_FAILED(composed)) clear();
        failed_ = true; lastResult_ = composed;
        return vr::VRCompositorError_InvalidTexture;
      }
      if (layer.type != XR_TYPE_COMPOSITION_LAYER_PROJECTION || layer.next ||
          !layer.space || layer.viewCount != 2 || !layer.views) {
        clear(); failed_ = true; lastResult_ = XR_ERROR_VALIDATION_FAILURE;
        return vr::VRCompositorError_InvalidTexture;
      }
    }
    const auto* header = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.layerCount = pixels ? 1u : 0u; end.layers = pixels ? &header : nullptr;
    lastResult_ = session_.end(frame_, end, {geometryReady_, pixels});
    if (lastResult_ != XR_SUCCESS) failed_ = true;
    return lastResult_ == XR_SUCCESS ? vr::VRCompositorError_None : vr::VRCompositorError_InvalidTexture;
  }

  // ClearLastSubmittedFrame and the next wait use the same closure path. A
  // handoff after a complete pair has no work; it cannot end a frame twice.
  XrResult clear() {
    if (!ownerThread()) return XR_ERROR_CALL_ORDER_INVALID;
    if (failed_) return lastResult_;
    if (session_.frameOpen()) {
      const XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};
      lastResult_ = session_.end(frame_, empty, {false, false});
      if (lastResult_ != XR_SUCCESS) { failed_ = true; return lastResult_; }
    }
    accepted_[0] = accepted_[1] = geometryReady_ = false;
    return XR_SUCCESS;
  }
  XrResult lastResult() const { return lastResult_; }
  bool ownerThread() const { return std::this_thread::get_id() == thread_; }

 private:
  SessionState& session_;
  FrameSink& sink_;
  const std::thread::id thread_;
  Frame frame_{};
  bool accepted_[2]{}, geometryReady_ = false, failed_ = false;
  XrResult lastResult_ = XR_SUCCESS;
};
} // namespace edvr::openxr
