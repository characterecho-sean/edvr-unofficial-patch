#pragma once

namespace edvr::openxr {

enum class LoadingWork { None, Skybox, Empty };

// Owner-thread policy only: no timer, API calls or texture ownership. Scene
// waits/submits and accepted override changes drive it explicitly. In-flight
// game frames take priority over both loading and deferred empty-frame work.
class LoadingState final {
 public:
  void overrideSet() {
    hasOverride_=true;
    if(!sceneSeen_)visible_=true;
    clearPending_=false;
  }
  void overrideCleared() {
    clearPending_=clearPending_||visible_;
    hasOverride_=visible_=false;
  }
  void sceneCleared() {
    visible_=hasOverride_;
    clearPending_=!hasOverride_;
  }
  void sceneWaited() { clearPending_=false; }
  bool sceneSubmitted() {
    const bool transition=visible_;
    sceneSeen_=true;visible_=clearPending_=false;
    return transition;
  }
  bool visible() const { return visible_; }
  LoadingWork work(bool gameFrameOpen) const {
    if(gameFrameOpen)return LoadingWork::None;
    if(clearPending_)return LoadingWork::Empty;
    return visible_&&hasOverride_?LoadingWork::Skybox:LoadingWork::None;
  }
  void emptyCompleted() { clearPending_=false; }

 private:
  bool hasOverride_=false,visible_=false,clearPending_=false,sceneSeen_=false;
};

} // namespace edvr::openxr
