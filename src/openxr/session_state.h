#pragma once
// Externally serialized policy for one borrowed session. The owner creates and
// destroys handles, validates layers, and marshals game threads. No destructor
// calls OpenXR. This is the frame policy behind openvr_api.dll's own
// WaitGetPoses/Submit: waitAndBegin's blocking wait is the default pacing,
// and openDeferred/beginDeferred split that wait from its xrBeginFrame so a
// frame can be handed to the game before the runtime's own xrWaitFrame has
// returned (turbo mode), with the real begin completed later, just before
// the frame ends.
#include <openxr/openxr.h>
#include <atomic>
#include <cstdint>
#include <algorithm>

namespace edvr::openxr {
struct Dispatch {
  PFN_xrPollEvent pollEvent = nullptr;
  PFN_xrBeginSession beginSession = nullptr;
  PFN_xrEndSession endSession = nullptr;
  PFN_xrWaitFrame waitFrame = nullptr;
  PFN_xrBeginFrame beginFrame = nullptr;
  PFN_xrEndFrame endFrame = nullptr;
};
enum class Lifecycle { Uninitialized, Idle, Ready, Synchronized, Visible,
  Focused, Stopping, Ended, Exiting, LossPending, SessionLost,
  InstanceLossPending, InstanceLost, Failed };
enum class FrameStatus { None, Open, NoRender, Discarded, SessionLossPending, Ended, Failed };
struct RenderReady { bool poseValid = true, renderReady = true; };
struct Frame {
  XrTime predictedDisplayTime = 0;
  XrDuration predictedDisplayPeriod = 0;
  bool shouldRender = false, discarded = false;
  FrameStatus status = FrameStatus::None;
  std::uint64_t sequence = 0, generation = 0, owner = 0;
};
// Invoked synchronously for events outside this policy. Copy needed data;
// the buffer is borrowed for this call only. The sink must not re-enter us.
using UnhandledEventSink = void (*)(const XrEventDataBuffer&, void*) noexcept;

class SessionState {
 public:
  static constexpr std::uint32_t kMaxEvents = 32;
  SessionState() = default;
  SessionState(const SessionState&) = delete;
  SessionState& operator=(const SessionState&) = delete;
  SessionState(SessionState&&) = delete;
  SessionState& operator=(SessionState&&) = delete;
  SessionState(const Dispatch& d, XrInstance instance, XrSession session,
               XrEnvironmentBlendMode blend) {
    const XrResult r = reset(d, instance, session, blend);
    if (XR_FAILED(r)) fail(r);
  }

  // Rebinding never destroys handles. A running session/open frame cannot be
  // forgotten here; the owner must stop it or explicitly destroy it first.
  XrResult reset(const Dispatch& d, XrInstance instance, XrSession session,
                 XrEnvironmentBlendMode blend) {
    if (running_ || frameOpen_) return XR_ERROR_CALL_ORDER_INVALID;
    if (!instance || !session) return XR_ERROR_HANDLE_INVALID;
    if (!d.pollEvent || !d.beginSession || !d.endSession || !d.waitFrame ||
        !d.beginFrame || !d.endFrame) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (blend != XR_ENVIRONMENT_BLEND_MODE_OPAQUE && blend != XR_ENVIRONMENT_BLEND_MODE_ADDITIVE &&
        blend != XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
    clear(); dispatch_ = d; instance_ = instance; session_ = session; blend_ = blend;
    return XR_SUCCESS;
  }
  // Explicit owner acknowledgement: borrowed resources have already been
  // destroyed. Invalidate even an outstanding frame, and erase stale handles.
  void abandonAfterOwnerDestruction() { clear(); dispatch_ = {}; instance_ = XR_NULL_HANDLE; session_ = XR_NULL_HANDLE; }
  void setUnhandledEventSink(UnhandledEventSink sink, void* context) { eventSink_ = sink; eventContext_ = context; }

  XrResult pollEvents(std::uint32_t budget = 8) {
    if (terminal_) return lastResult_;
    if (!instance_ || !session_) return fail(XR_ERROR_HANDLE_INVALID);
    if (!dispatch_.pollEvent) return fail(XR_ERROR_FUNCTION_UNSUPPORTED);
    // Leave the actionable STOPPING event intact until the caller ends it.
    if (state_ == XR_SESSION_STATE_STOPPING && running_) return lastResult_;
    budget = (std::min)(budget, kMaxEvents);
    for (std::uint32_t i = 0; i < budget; ++i) {
      XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
      const XrResult r = dispatch_.pollEvent(instance_, &buffer);
      if (r == XR_EVENT_UNAVAILABLE) return lastResult_;
      if (r != XR_SUCCESS) return fail(XR_FAILED(r) ? r : XR_ERROR_RUNTIME_FAILURE);
      if (buffer.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
        terminal_ = true; lifecycle_ = Lifecycle::InstanceLossPending;
        return lastResult_; // distinct lifecycle signal; not a session-loss result
      }
      if (buffer.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) { unhandled(buffer); continue; }
      const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&buffer);
      if (changed.session != session_) { unhandled(buffer); continue; }
      state_ = changed.state;
      switch (state_) {
        case XR_SESSION_STATE_IDLE: lifecycle_ = Lifecycle::Idle; break;
        case XR_SESSION_STATE_READY: lifecycle_ = Lifecycle::Ready; return lastResult_;
        case XR_SESSION_STATE_SYNCHRONIZED: lifecycle_ = Lifecycle::Synchronized; break;
        case XR_SESSION_STATE_VISIBLE: lifecycle_ = Lifecycle::Visible; break;
        case XR_SESSION_STATE_FOCUSED: lifecycle_ = Lifecycle::Focused; break;
        case XR_SESSION_STATE_STOPPING: lifecycle_ = Lifecycle::Stopping; return lastResult_;
        case XR_SESSION_STATE_EXITING: terminal_ = true; lifecycle_ = Lifecycle::Exiting; return lastResult_;
        case XR_SESSION_STATE_LOSS_PENDING: return note(XR_SESSION_LOSS_PENDING);
        default: return fail(XR_ERROR_RUNTIME_FAILURE);
      }
    }
    return lastResult_;
  }

  XrResult startIfReady() {
    if (terminal_) return hardFailure_ ? lastResult_ : XR_ERROR_CALL_ORDER_INVALID;
    if (state_ != XR_SESSION_STATE_READY || running_) return XR_ERROR_CALL_ORDER_INVALID;
    const XrResult r = dispatch_.beginSession(session_, &beginInfo_);
    if (XR_FAILED(r)) return fail(r);
    if (r != XR_SUCCESS && r != XR_SESSION_LOSS_PENDING) return fail(XR_ERROR_RUNTIME_FAILURE);
    running_ = true; ++generation_; sequence_ = 0; clearFrame();
    return note(r);
  }

  XrResult waitAndBegin(Frame& out) {
    if (hardFailure_) return lastResult_;
    if (terminal_ || frameOpen_ || !running_ || state_ == XR_SESSION_STATE_STOPPING ||
        state_ == XR_SESSION_STATE_IDLE) return XR_ERROR_CALL_ORDER_INVALID;
    out = {};
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    const XrResult r = dispatch_.waitFrame(session_, &wi, &fs);
    return beginFromWait(r, fs, out);
  }

  // The wait plus everything waitAndBegin does after it returns, for a caller
  // that already has a completed wait's result (FrameBoundary, taking one
  // from the FramePacer). Real time -- and possibly a session state change --
  // may have passed since that wait was started, so the preconditions are
  // re-checked here exactly as waitAndBegin checks them before its own wait.
  XrResult beginWaited(XrResult waited, const XrFrameState& fs, Frame& out) {
    if (hardFailure_) return lastResult_;
    if (terminal_ || frameOpen_ || !running_ || state_ == XR_SESSION_STATE_STOPPING ||
        state_ == XR_SESSION_STATE_IDLE) return XR_ERROR_CALL_ORDER_INVALID;
    out = {};
    return beginFromWait(waited, fs, out);
  }

  // Opens a frame WITHOUT calling xrBeginFrame, handing the game a predicted
  // time synthesized by the caller while the real xrWaitFrame is still
  // in flight on the pacer thread (turbo mode). beginDeferred performs the
  // real begin later, just before the frame ends.
  XrResult openDeferred(XrTime predictedTime, XrDuration period, Frame& out) {
    if (hardFailure_) return lastResult_;
    if (terminal_ || frameOpen_ || !running_ || state_ == XR_SESSION_STATE_STOPPING ||
        state_ == XR_SESSION_STATE_IDLE) return XR_ERROR_CALL_ORDER_INVALID;
    out = {};
    frameOpen_ = true; deferred_ = true; frameTime_ = predictedTime;
    frameShouldRender_ = true;
    out.predictedDisplayTime = predictedTime;
    out.predictedDisplayPeriod = period;
    out.shouldRender = !terminal_;
    out.status = terminal_ ? FrameStatus::SessionLossPending : FrameStatus::Open;
    out.sequence = ++sequence_; out.generation = generation_; out.owner = owner_;
    return lastResult_;
  }

  // The real xrBeginFrame for a frame openDeferred already handed to the
  // game, once its xrWaitFrame has actually returned. Unlike beginFromWait,
  // a failure here must explicitly reopen the frame as closed (clearFrame()):
  // the frame token is already live with the game, but the runtime never
  // received a matching xrBeginFrame, so nothing else may treat one as open.
  XrResult beginDeferred(XrResult waited, const XrFrameState& fs) {
    if (hardFailure_) return lastResult_;
    if (!frameOpen_ || !deferred_) return XR_ERROR_CALL_ORDER_INVALID;
    deferred_ = false;
    if (XR_FAILED(waited)) { clearFrame(); return fail(waited); }
    if (waited != XR_SUCCESS && waited != XR_SESSION_LOSS_PENDING) {
      clearFrame(); return fail(XR_ERROR_RUNTIME_FAILURE);
    }
    note(waited);
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    const XrResult r = dispatch_.beginFrame(session_, &bi);
    if (XR_FAILED(r)) { clearFrame(); return fail(r); }
    if (r != XR_SUCCESS && r != XR_FRAME_DISCARDED && r != XR_SESSION_LOSS_PENDING) {
      clearFrame(); return fail(XR_ERROR_RUNTIME_FAILURE);
    }
    note(r);
    // The layer's view poses carry what the game rendered with (the
    // synthesized time from openDeferred); displayTime given to xrEndFrame
    // is frameTime_, now overwritten with what the runtime itself predicted.
    // Deliberate: the runtime paced itself against its own prediction, not
    // the game's guess of it.
    frameTime_ = fs.predictedDisplayTime;
    frameShouldRender_ = fs.shouldRender == XR_TRUE;
    return lastResult_;
  }

 private:
  // Everything waitAndBegin/beginWaited do once a wait's result is known:
  // the failure/loss checks, xrBeginFrame, and the frame bookkeeping. No
  // preconditions here -- both public callers already checked them against
  // the state as of their own call.
  XrResult beginFromWait(XrResult waited, const XrFrameState& fs, Frame& out) {
    if (XR_FAILED(waited)) { out.status = FrameStatus::Failed; return fail(waited); }
    if (waited != XR_SUCCESS && waited != XR_SESSION_LOSS_PENDING) return fail(XR_ERROR_RUNTIME_FAILURE);
    note(waited);
    // Successful wait owns a begin obligation even when loss is pending.
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    const XrResult r = dispatch_.beginFrame(session_, &bi);
    if (XR_FAILED(r)) { out.status = FrameStatus::Failed; return fail(r); }
    if (r != XR_SUCCESS && r != XR_FRAME_DISCARDED && r != XR_SESSION_LOSS_PENDING)
      return fail(XR_ERROR_RUNTIME_FAILURE);
    note(r);
    frameOpen_ = true; frameTime_ = fs.predictedDisplayTime;
    frameShouldRender_ = fs.shouldRender == XR_TRUE;
    out.predictedDisplayTime = fs.predictedDisplayTime;
    out.predictedDisplayPeriod = fs.predictedDisplayPeriod;
    out.shouldRender = frameShouldRender_ && !terminal_;
    out.discarded = r == XR_FRAME_DISCARDED;
    out.status = terminal_ ? FrameStatus::SessionLossPending : out.discarded ? FrameStatus::Discarded :
                 out.shouldRender ? FrameStatus::Open : FrameStatus::NoRender;
    out.sequence = ++sequence_; out.generation = generation_; out.owner = owner_;
    return lastResult_;
  }

 public:

  XrResult end(Frame& frame, const XrFrameEndInfo& supplied, const RenderReady& ready = {}) {
    if (hardFailure_) return lastResult_;
    if (!frameOpen_ || frame.sequence != sequence_ || frame.generation != generation_ || frame.owner != owner_)
      return XR_ERROR_CALL_ORDER_INVALID;
    // A deferred frame's real begin has not happened yet; ending it now would
    // hand the runtime an end for a begin it never received. The caller must
    // complete it with beginDeferred first.
    if (deferred_) return XR_ERROR_CALL_ORDER_INVALID;
    // Only token fields are trusted from the public frame; timing and the
    // runtime's no-render decision come from the stored frame state.
    const bool layers = frameShouldRender_ && !terminal_ && state_ != XR_SESSION_STATE_STOPPING &&
                        ready.poseValid && ready.renderReady;
    if (layers && supplied.layerCount && !supplied.layers) return XR_ERROR_VALIDATION_FAILURE;
    const XrResult r = finish(supplied, layers);
    frame.shouldRender = false;
    frame.status = XR_FAILED(r) ? FrameStatus::Failed : terminal_ ? FrameStatus::SessionLossPending :
                   layers && supplied.layerCount ? FrameStatus::Ended : FrameStatus::NoRender;
    return r;
  }

  XrResult stop() {
    if (hardFailure_) return lastResult_;
    if (state_ != XR_SESSION_STATE_STOPPING || !running_) return XR_ERROR_CALL_ORDER_INVALID;
    // As in end(): a still-deferred frame has no real begin yet to close.
    if (frameOpen_ && deferred_) return XR_ERROR_CALL_ORDER_INVALID;
    if (frameOpen_) {
      const XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};
      const XrResult r = finish(empty, false);
      if (XR_FAILED(r)) return r; // owner tears down after uncertain failure; no retry
    }
    const XrResult r = dispatch_.endSession(session_);
    if (XR_FAILED(r)) return fail(r);
    if (r != XR_SUCCESS && r != XR_SESSION_LOSS_PENDING) return fail(XR_ERROR_RUNTIME_FAILURE);
    running_ = false; clearFrame();
    if (!terminal_) lifecycle_ = Lifecycle::Ended;
    return note(r); // Later IDLE/READY may start this same session again.
  }

  Lifecycle lifecycle() const { return lifecycle_; }
  bool frameOpen() const { return frameOpen_; }
  bool running() const { return running_; }
  bool terminal() const { return terminal_; }
  XrTime predictedDisplayTime() const { return frameTime_; }
  XrResult lastResult() const { return lastResult_; }
  // True from openDeferred until beginDeferred (or a clear/drain) completes
  // or discards the real begin it stands in for.
  bool deferred() const { return deferred_; }
  // The runtime's own shouldRender for the open frame -- fs.shouldRender for
  // a normal wait, or true for a still-deferred frame until beginDeferred
  // learns the real answer, matching the optimistic layers a turbo frame's
  // caller must be ready to discard.
  bool frameShouldRender() const { return frameShouldRender_; }

 private:
  void unhandled(const XrEventDataBuffer& buffer) {
    if (eventSink_) eventSink_(buffer, eventContext_);
  }
  void clearFrame() { frameOpen_ = frameShouldRender_ = deferred_ = false; frameTime_ = 0; }
  void clear() {
    ++generation_; sequence_ = 0; clearFrame(); running_ = terminal_ = hardFailure_ = false;
    lifecycle_ = Lifecycle::Uninitialized; state_ = XR_SESSION_STATE_UNKNOWN; lastResult_ = XR_SUCCESS;
    eventSink_ = nullptr; eventContext_ = nullptr;
  }
  XrResult note(XrResult r) {
    if (r == XR_SESSION_LOSS_PENDING) {
      terminal_ = true;
      if (lifecycle_ != Lifecycle::InstanceLossPending) lifecycle_ = Lifecycle::LossPending;
    }
    // Pending loss remains observable after a successful frame/session close.
    if (terminal_ && lifecycle_ == Lifecycle::LossPending)
      return lastResult_ = XR_SESSION_LOSS_PENDING;
    return lastResult_ = r;
  }
  XrResult fail(XrResult r) {
    hardFailure_ = terminal_ = true; lastResult_ = r;
    lifecycle_ = r == XR_ERROR_INSTANCE_LOST ? Lifecycle::InstanceLost :
                 r == XR_ERROR_SESSION_LOST ? Lifecycle::SessionLost : Lifecycle::Failed;
    return r;
  }
  XrResult finish(const XrFrameEndInfo& supplied, bool layers) {
    XrFrameEndInfo info = supplied;
    info.type = XR_TYPE_FRAME_END_INFO; info.displayTime = frameTime_; info.environmentBlendMode = blend_;
    if (!layers) { info.next = nullptr; info.layerCount = 0; info.layers = nullptr; }
    const XrResult r = dispatch_.endFrame(session_, &info);
    clearFrame(); // An attempted end is never retried with different layers.
    if (XR_FAILED(r)) return fail(r);
    if (r != XR_SUCCESS && r != XR_SESSION_LOSS_PENDING) return fail(XR_ERROR_RUNTIME_FAILURE);
    return note(r);
  }
  inline static std::atomic<std::uint64_t> nextOwner_{1};
  const std::uint64_t owner_ = nextOwner_.fetch_add(1, std::memory_order_relaxed);
  const XrSessionBeginInfo beginInfo_{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
  Dispatch dispatch_{};
  UnhandledEventSink eventSink_ = nullptr;
  void* eventContext_ = nullptr;
  XrInstance instance_ = XR_NULL_HANDLE;
  XrSession session_ = XR_NULL_HANDLE;
  XrEnvironmentBlendMode blend_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  Lifecycle lifecycle_ = Lifecycle::Uninitialized;
  XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
  bool running_ = false, frameOpen_ = false, frameShouldRender_ = false;
  bool terminal_ = false, hardFailure_ = false;
  bool deferred_ = false;
  XrTime frameTime_ = 0;
  std::uint64_t sequence_ = 0, generation_ = 0;
  XrResult lastResult_ = XR_SUCCESS;
};
} // namespace edvr::openxr
