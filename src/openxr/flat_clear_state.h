#pragma once

#include <cstdint>

namespace edvr::openxr {

// The headset while flat (docs/design-unified-install-2026-10-12.md section 12, N1). While the graphics half says the game
// is not in its VR profile, the headset keeps the last frame the game submitted, so the runtime runs empty frames of its
// own. The game's calls always win: a frame it holds open, or any call inside the quiet window, stops them.
//
// Owner-thread policy only: no clock and no API calls. The owner passes in what it observed on this pump.
class FlatClearState final {
 public:
  static constexpr uint64_t kQuietMs = 250;
  // A game frame still open this long after the game's last call, while flat, was left open at the switch.
  static constexpr uint64_t kStaleFrameMs = 1000;

  enum class Stop { None, VrProfile, GameFrame, Session };

  struct Step {
    bool run = false;      // run one empty frame now
    bool started = false;  // the first empty frame of a stretch
    bool stopped = false;  // this pump ends a stretch
    Stop reason = Stop::None;
  };

  Step decide(bool vrProfileActive, bool gameFrameOpen, bool sessionRunning, uint64_t quietMs) {
    // The graphics half reading VR again re-arms clearing after a failure.
    if (vrProfileActive) failed_ = false;
    Step step;
    step.run = !vrProfileActive && !failed_ && !gameFrameOpen && sessionRunning && quietMs >= kQuietMs;
    step.started = step.run && !active_;
    step.stopped = !step.run && active_;
    if (step.stopped)
      step.reason = vrProfileActive ? Stop::VrProfile : !sessionRunning ? Stop::Session : Stop::GameFrame;
    active_ = step.run;
    // A new stretch starts the panel's clock again.
    if (step.started) panelStarted_ = darkNoted_ = emptyNoted_ = false;
    return step;
  }

  // The notice's text shows for kPanelTextMs from the stretch's first panel frame. Then the eye images are black for
  // kPanelBlackSettleMs, still as a projection layer: the compositor holds the last projection frame, so black has to be
  // what it holds. After that draw is false and the owner ends frames with no layers. panelFrame runs once per panel frame
  // with the owner's ms clock. wentDark and wentEmpty are each set on the one frame they first apply, with afterMs its age then.
  static constexpr uint64_t kPanelTextMs = 10000;
  static constexpr uint64_t kPanelBlackSettleMs = 1000;
  struct PanelFrame {
    bool text = true;
    bool draw = true;  // false once the black has settled: the frame carries no layers
    bool wentDark = false;
    bool wentEmpty = false;
    uint64_t afterMs = 0;
  };
  PanelFrame panelFrame(uint64_t nowMs) {
    if (!panelStarted_) {
      panelStarted_ = true;
      panelStartMs_ = nowMs;
    }
    PanelFrame frame;
    const uint64_t after = nowMs >= panelStartMs_ ? nowMs - panelStartMs_ : 0;
    frame.text = after < kPanelTextMs;
    frame.draw = after < kPanelTextMs + kPanelBlackSettleMs;
    if (!frame.text && !darkNoted_) {
      darkNoted_ = true;
      frame.wentDark = true;
      frame.afterMs = after;
    }
    if (!frame.draw && !emptyNoted_) {
      emptyNoted_ = true;
      frame.wentEmpty = true;
      frame.afterMs = after;
    }
    return frame;
  }

  // An empty frame failed. Clearing is optional, so it stops and stays stopped until the graphics half reads VR again: the
  // session's own terminal checks, not this policy, decide whether the session is over.
  void fail() {
    failed_ = true;
    active_ = false;
  }

  // The stale game frame, ended by the owner (flatClearTick): a frame open while flat, with the game silent for kStaleFrameMs.
  static bool staleGameFrame(bool vrProfileActive, bool gameFrameOpen, uint64_t quietMs) {
    return !vrProfileActive && gameFrameOpen && quietMs >= kStaleFrameMs;
  }

  bool active() const { return active_; }

 private:
  bool active_ = false;
  bool failed_ = false;
  bool panelStarted_ = false, darkNoted_ = false, emptyNoted_ = false;
  uint64_t panelStartMs_ = 0;
};

}  // namespace edvr::openxr
