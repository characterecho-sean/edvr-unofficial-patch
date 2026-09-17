#pragma once
#include "session_state.h"
#include "../openvr/compat/openvr_v0_9_20.h"
#include "frame_pacer.h"
#include <chrono>
#include <thread>
#include "submission_measurement.h"

namespace edvr::openxr {

// Callbacks execute on the frame owner's thread, under its runtime operation
// lease. They must not re-enter the boundary. The source owns copied pixels,
// located views and layer backing storage until endFrame returns.
struct FrameSink {
  virtual ~FrameSink() = default;
  virtual vr::EVRCompositorError capture(vr::EVREye, const vr::Texture_t*,
      const vr::VRTextureBounds_t*, vr::EVRSubmitFlags, bool copyPixels) = 0;
  virtual XrResult compose(XrCompositionLayerProjection&) = 0;
  // A withheld first frame may have no safe stereo image to replay. Keep
  // frame pacing with zero layers until a complete good pair is available.
  virtual bool sceneLayerAvailable() const { return true; }
  virtual void sceneFinished(bool, XrResult) {}
  virtual XrResult composeBackground(XrCompositionLayerProjection&) {
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  }
};

// Runtime = today's blocking wait, in WaitGetPoses. Deferred is OpenXR
// Toolkit's "Turbo mode": waitAndBegin hands the game a synthesized
// predicted time immediately, without blocking, while the real xrWaitFrame
// keeps running on a FramePacer thread; the real xrBeginFrame completes at
// finish() instead, just before the game's own xrEndFrame-equivalent
// (Submit's second eye), so the game's work overlaps the wait rather than
// preceding it.
enum class FramePacing { Runtime, Deferred };

// Submit pairing policy shared by a future IVRCompositor and the native test.
// SessionState, the sink, and their borrowed handles outlive this object. The
// owner serializes operations and excludes destruction using RuntimeGate.
// This is not yet the historical 29-slot IVRCompositor facade.
class FrameBoundary final {
 public:
  using Clock = std::chrono::steady_clock;

  FrameBoundary(SessionState& session, FrameSink& sink, FramePacer* pacer = nullptr)
      : session_(session), sink_(sink), thread_(std::this_thread::get_id()), pacer_(pacer) {}
  FrameBoundary(const FrameBoundary&) = delete;
  FrameBoundary& operator=(const FrameBoundary&) = delete;

  XrResult waitAndBegin(FramePacing pacing = FramePacing::Runtime) {
    if (!ownerThread()) return XR_ERROR_CALL_ORDER_INVALID;
    if (failed_) return lastResult_;
    // Missing/one-eye submits cannot strand the preceding begun frame.
    const auto closed = clear();
    if (closed != XR_SUCCESS) return closed;
    frame_ = {};
    endFrameMs_ = waitBlockMs_ = pacerBlockMs_ = 0;
    waitEntry_ = Clock::now();
    turbo_ = pacing == FramePacing::Deferred && pacer_ && pacer_->bound();

    if (pacer_ && pacer_->pending()) {
      if (turbo_ && !pacer_->ready() && canSynthesize()) {
        // The runtime's own wait has not returned yet: hand the game a
        // synthesized time now and complete the real begin at finish().
        lastResult_ = session_.openDeferred(synthesizedTime(), lastPeriod_, frame_);
        if (XR_FAILED(lastResult_)) failed_ = true;
        else { deferred_ = true; ++synthesized_; admitFrame(); }
        return lastResult_;
      }
      // The pacer already has a result (a prior turbo frame's kick caught
      // up, or a runtime-paced call finds a leftover pending wait). Consume
      // it synchronously; this is not a deferred frame.
      XrResult waited = XR_SUCCESS;
      XrFrameState fs{XR_TYPE_FRAME_STATE};
      { SubmissionWallScope measured(&waitBlockMs_); pacer_->take(waited, fs); }
      noteReal(waited, fs.predictedDisplayTime, fs.predictedDisplayPeriod);
      lastResult_ = session_.beginWaited(waited, fs, frame_);
      if (XR_FAILED(lastResult_)) failed_ = true;
      else { if (turbo_) ++readyAtWait_; admitFrame(); }
      return lastResult_;
    }

    if (turbo_ && canSynthesize() && pacer_->kick()) {
      ++kicks_;
      lastResult_ = session_.openDeferred(synthesizedTime(), lastPeriod_, frame_);
      if (XR_FAILED(lastResult_)) failed_ = true;
      else { deferred_ = true; ++synthesized_; admitFrame(); }
      return lastResult_;
    }

    // No pacer, no turbo, or turbo with nothing yet to synthesize from
    // (canSynthesize() false before the first real wait): the original,
    // blocking wait.
    { SubmissionWallScope measured(&waitBlockMs_); lastResult_ = session_.waitAndBegin(frame_); }
    if (XR_FAILED(lastResult_)) failed_ = true;
    else {
      noteReal(XR_SUCCESS, frame_.predictedDisplayTime, frame_.predictedDisplayPeriod);
      admitFrame();
    }
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
    return finish(false)==XR_SUCCESS ? vr::VRCompositorError_None : vr::VRCompositorError_InvalidTexture;
  }

  // Owner-only loading frame. It must have its own wait/begin and cannot
  // replace a partially submitted game frame. No eye captures or game Submit
  // counts are manufactured. The caller decides when loading is active.
  XrResult background() {
    if (!ownerThread()) return XR_ERROR_CALL_ORDER_INVALID;
    if (failed_) return lastResult_;
    if (!session_.frameOpen() || accepted_[0] || accepted_[1]) return XR_ERROR_CALL_ORDER_INVALID;
    return finish(true);
  }

 private:
  // Records the last REAL (runtime-reported) predicted time/period and when
  // it was learned, whichever pacing produced it: a synchronous wait, or a
  // pacer result taken at wait/finish/clear/drain. waited is checked against
  // the same two acceptable codes everywhere it is called from.
  void noteReal(XrResult waited, XrTime predictedTime, XrDuration period) {
    if ((waited == XR_SUCCESS || waited == XR_SESSION_LOSS_PENDING) && predictedTime > 0) {
      lastReal_ = predictedTime; lastPeriod_ = period; lastRealAt_ = Clock::now();
    }
  }
  bool canSynthesize() const {
    return lastReal_ > 0 && lastPeriod_ > 0 && lastRealAt_ != Clock::time_point{};
  }
  // OpenXR Toolkit's formula: the last real predicted time plus how long it
  // has been since that value was learned, clamped to between one and two
  // periods. In steady turbo the game asks again right after the previous
  // wait was taken, so the step is one period (the next slot); after a
  // stall -- a slow frame, or the runtime-paced fallback blocking a while --
  // the step is the wall-clock time since that wait returned, capped at two
  // periods so a synthesized value can never run ahead of the real one that
  // follows it (consecutive real predictions advance by at least a period).
  XrTime synthesizedTime() const {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        waitEntry_ - lastRealAt_).count();
    XrDuration step = static_cast<XrDuration>(elapsed);
    if (step < lastPeriod_) step = lastPeriod_;
    const XrDuration cap = lastPeriod_ * XrDuration(2);
    if (step > cap) step = cap;
    return lastReal_ + step;
  }
  // Every frame this boundary admits -- synchronous, taken at wait, or
  // synthesized -- is floored to the last value handed out, whichever
  // pacing produced it. A well-behaved runtime's own predictions already
  // increase, so this is a no-op under plain FramePacing::Runtime; under
  // turbo it is the guarantee that a synthesized time built from a stale
  // "last real" value can never hand the game (and ReferenceChanges::advance,
  // which retires and kills the service on a decrease) a time that runs
  // backwards. It touches only this Frame token, never SessionState's own
  // record of what the runtime predicted.
  void admitFrame() {
    if (frame_.predictedDisplayTime < lastHandedOut_) frame_.predictedDisplayTime = lastHandedOut_;
    lastHandedOut_ = frame_.predictedDisplayTime;
  }

  XrResult finish(bool background) {
    if (deferred_) {
      XrResult waited = XR_SUCCESS;
      XrFrameState fs{XR_TYPE_FRAME_STATE};
      bool taken;
      { SubmissionWallScope measured(&pacerBlockMs_); taken = pacer_ && pacer_->take(waited, fs); }
      deferred_ = false;
      if (!taken) { failed_ = true; lastResult_ = XR_ERROR_CALL_ORDER_INVALID; return lastResult_; }
      noteReal(waited, fs.predictedDisplayTime, fs.predictedDisplayPeriod);
      lastResult_ = session_.beginDeferred(waited, fs);
      if (lastResult_ != XR_SUCCESS && lastResult_ != XR_FRAME_DISCARDED && lastResult_ != XR_SESSION_LOSS_PENDING) {
        failed_ = true;
        if (!background) sink_.sceneFinished(false, lastResult_);
        return lastResult_;
      }
      // The runtime's real answer for a frame the game was already handed:
      // a false shouldRender or a discard here must still close it empty,
      // exactly as a NoRender frame does under plain FramePacing::Runtime.
      frame_.shouldRender = session_.frameShouldRender();
      frame_.discarded = lastResult_ == XR_FRAME_DISCARDED;
      ++deferredFrames_;
    }
    const bool pixels = geometryReady_ && frame_.shouldRender && !session_.terminal() &&
        (background || sink_.sceneLayerAvailable());
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    if (pixels) {
      const auto composed = background ? sink_.composeBackground(layer) : sink_.compose(layer);
      if (composed != XR_SUCCESS) {
        // A hard external XR failure may have invalidated the session. The
        // owner must destroy it; do not dispatch endFrame through lost handles.
        if (!XR_FAILED(composed)) clear();
        failed_ = true; lastResult_ = composed;
        return lastResult_;
      }
      if (layer.type != XR_TYPE_COMPOSITION_LAYER_PROJECTION || layer.next ||
          !layer.space || layer.viewCount != 2 || !layer.views) {
        clear(); failed_ = true; lastResult_ = XR_ERROR_VALIDATION_FAILURE;
        return lastResult_;
      }
    }
    const auto* header = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.layerCount = pixels ? 1u : 0u; end.layers = pixels ? &header : nullptr;
    { SubmissionWallScope measured(&endFrameMs_);
      lastResult_ = session_.end(frame_, end, {geometryReady_, pixels}); }
    if (!background) sink_.sceneFinished(pixels, lastResult_);
    if (lastResult_ != XR_SUCCESS) failed_ = true;
    // Kick the next frame's wait now, as the toolkit does right after its
    // own real xrEndFrame, so it has the whole rest of this frame to finish.
    if (turbo_ && lastResult_ == XR_SUCCESS && !session_.terminal() && pacer_ && pacer_->kick()) ++kicks_;
    return lastResult_;
  }

 public:
  // ClearLastSubmittedFrame and the next wait use the same closure path. A
  // handoff after a complete pair has no work; it cannot end a frame twice.
  XrResult clear() {
    if (!ownerThread()) return XR_ERROR_CALL_ORDER_INVALID;
    if (failed_) return lastResult_;
    if (session_.frameOpen()) {
      if (deferred_) {
        // The open frame never got its real begin. Take the pacer's result
        // (blocking if it has not returned yet -- clear() has never had a
        // timeout budget either) and complete it before ending it empty.
        XrResult waited = XR_SUCCESS;
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        bool taken;
        { SubmissionWallScope measured(&pacerBlockMs_); taken = pacer_ && pacer_->take(waited, fs); }
        deferred_ = false;
        if (!taken) { failed_ = true; lastResult_ = XR_ERROR_CALL_ORDER_INVALID; return lastResult_; }
        noteReal(waited, fs.predictedDisplayTime, fs.predictedDisplayPeriod);
        lastResult_ = session_.beginDeferred(waited, fs);
        if (lastResult_ != XR_SUCCESS && lastResult_ != XR_FRAME_DISCARDED && lastResult_ != XR_SESSION_LOSS_PENDING) {
          failed_ = true; return lastResult_;
        }
        ++drainedFrames_;
      }
      const XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};
      lastResult_ = session_.end(frame_, empty, {false, false});
      if (lastResult_ != XR_SUCCESS) { failed_ = true; return lastResult_; }
    }
    accepted_[0] = accepted_[1] = geometryReady_ = false;
    return XR_SUCCESS;
  }

  // Owner-only: consumes whatever the pacer still holds, so a stop or a
  // destroy that follows never leaves its worker thread mid xrWaitFrame with
  // nobody who will ever take the result. clear() above already handles a
  // frame this boundary had open; drain() additionally reaches for a pending
  // wait that was only ever kicked (finish()'s post-end kick, or a turbo
  // entry that opened its own deferred frame and left this one behind). A
  // wait that comes back admitting a real frame is begun and ended with zero
  // layers, so the runtime's own loop -- whose next xrWaitFrame only
  // unblocks after a matching xrBeginFrame -- stays consistent; a wait that
  // comes back any other way (a failure, or the session no longer running)
  // is simply dropped, exactly as clear() drops history on session loss.
  XrResult drain() {
    if (!ownerThread()) return XR_ERROR_CALL_ORDER_INVALID;
    if (failed_) return lastResult_;
    const auto cleared = clear();
    if (cleared != XR_SUCCESS) return cleared;
    if (pacer_ && pacer_->pending()) {
      XrResult waited = XR_SUCCESS;
      XrFrameState fs{XR_TYPE_FRAME_STATE};
      pacer_->take(waited, fs);
      ++drainedWaits_;
      if (session_.running() && !session_.terminal() &&
          (waited == XR_SUCCESS || waited == XR_SESSION_LOSS_PENDING)) {
        noteReal(waited, fs.predictedDisplayTime, fs.predictedDisplayPeriod);
        Frame scratch{};
        if (session_.beginWaited(waited, fs, scratch) == XR_SUCCESS) {
          const XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};
          const auto ended = session_.end(scratch, empty, {false, false});
          if (XR_FAILED(ended)) failed_ = true;
          lastResult_ = ended;
        }
      }
    }
    return lastResult_;
  }

  XrResult lastResult() const { return lastResult_; }
  double endFrameMs() const { return endFrameMs_; }
  // A failed boundary has consumed an uncertain XR operation or observed an
  // irreversible composition/end failure. Callers must retire their native
  // publications before admitting another frame.
  bool failed() const { return failed_; }
  bool ownerThread() const { return std::this_thread::get_id() == thread_; }

  // deferred() is true between an admitted synthesized frame and its
  // completion at finish()/clear(). turbo() reflects the pacing the most
  // recent waitAndBegin call actually latched -- Deferred with a bound
  // pacer -- and is recomputed fresh every call, so a caller can tell
  // "this frame was asked for with turbo pacing" (turbo(), possibly still
  // true after a synchronous bootstrap or a ready-at-wait consume) from
  // "this frame is currently the synthesized one, awaiting its real begin"
  // (deferred()). It is not sticky across a later call that asks for
  // FramePacing::Runtime instead; that call turns it back off.
  bool deferred() const { return deferred_; }
  bool turbo() const { return turbo_; }
  double waitBlockMs() const { return waitBlockMs_; }
  double pacerBlockMs() const { return pacerBlockMs_; }
  uint64_t deferredFrames() const { return deferredFrames_; }
  uint64_t synthesized() const { return synthesized_; }
  uint64_t readyAtWait() const { return readyAtWait_; }
  uint64_t kicks() const { return kicks_; }
  uint64_t drainedFrames() const { return drainedFrames_; }
  uint64_t drainedWaits() const { return drainedWaits_; }

 private:
  SessionState& session_;
  FrameSink& sink_;
  const std::thread::id thread_;
  FramePacer* pacer_ = nullptr;
  Frame frame_{};
  bool accepted_[2]{}, geometryReady_ = false, failed_ = false;
  XrResult lastResult_ = XR_SUCCESS;
  double endFrameMs_ = 0;
  // Turbo bookkeeping. turbo_ is latched per waitAndBegin call from the
  // requested pacing; deferred_ is true only while THIS frame is the
  // synthesized one, awaiting its real begin at finish()/clear().
  bool turbo_ = false, deferred_ = false;
  XrTime lastReal_ = 0;
  XrDuration lastPeriod_ = 0;
  Clock::time_point waitEntry_{}, lastRealAt_{};
  XrTime lastHandedOut_ = 0;
  double waitBlockMs_ = 0, pacerBlockMs_ = 0;
  uint64_t deferredFrames_ = 0, synthesized_ = 0, readyAtWait_ = 0, kicks_ = 0;
  uint64_t drainedFrames_ = 0, drainedWaits_ = 0;
};
} // namespace edvr::openxr
