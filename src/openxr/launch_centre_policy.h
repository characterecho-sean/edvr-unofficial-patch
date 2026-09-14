#pragma once

#include "geometry_snapshot.h"
#include "seated_origin.h"
#include <openxr/openxr.h>
#include <cstdint>

namespace edvr::openxr {

enum class LaunchCentreDecision { Wait, Centre, Expired };

// One decision before Init returns, independent of the runtime and legacy
// OpenVR config. The host also bounds elapsed startup time and never re-arms.
class LaunchCentrePolicy final {
 public:
  bool pending() const noexcept { return pending_; }
  LaunchCentreDecision expire() noexcept { return pending_?finish(LaunchCentreDecision::Expired):final_; }
  LaunchCentreDecision consider(const XrPosef& head,XrSpaceLocationFlags flags) noexcept {
    if(!pending_)return final_;
    constexpr XrSpaceLocationFlags required=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|
      XR_SPACE_LOCATION_POSITION_TRACKED_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    if((flags&required)!=required || !detail::poseValid(head) || placeholder(head))return wait();
    XrPosef seated{}; if(!seatedOriginFromHead(head,flags,seated))return wait();
    return finish(LaunchCentreDecision::Centre);
  }
 private:
  static bool placeholder(const XrPosef& p) noexcept {
    // Some runtimes initially publish valid identity-at-zero poses. Waiting
    // here prevents those placeholders from consuming the one startup reset.
    if(p.position.x!=0||p.position.y!=0||p.position.z!=0)return false;
    const auto& q=p.orientation;
    return (q.x==0&&q.y==0&&q.z==0&&(q.w==1||q.w==-1));
  }
  LaunchCentreDecision wait() noexcept {
    if(++samples_>=600)return finish(LaunchCentreDecision::Expired);
    return LaunchCentreDecision::Wait;
  }
  LaunchCentreDecision finish(LaunchCentreDecision d) noexcept { pending_=false;final_=d;return d; }
  unsigned samples_=0;
  bool pending_=true; LaunchCentreDecision final_=LaunchCentreDecision::Wait;
};
}
