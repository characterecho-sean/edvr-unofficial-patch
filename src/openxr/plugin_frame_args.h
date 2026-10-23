// What a located frame becomes for the plugin API (include/edvr_plugin_api.h): one head pose for onUpdate and, per eye, the eye pose and field of view for
// onRenderEye. Pure, so a rig can check which pose lands where without a runtime or a GPU.
//
// headPose is the located head (the VIEW reference space located into the frame's origin: geometry_locator.h's input.headPose), never an eye. The eye
// poses are the views' poses, in that same origin. The eyes sit half the interpupillary distance either side of the head and may be canted, so a plugin that
// aims from the head (a gaze ray) must not be handed an eye.
#pragma once
#include <openxr/openxr.h>
#include "../../include/edvr_plugin_api.h"

namespace edvr::openxr {

struct PluginFrameArgs {
  EdvrPosef head{};
  EdvrPosef eyePose[2]{};
  EdvrFovf eyeFov[2]{};
};

inline EdvrPosef toPluginPose(const XrPosef& p) {
  EdvrPosef out{};
  out.position.x = p.position.x; out.position.y = p.position.y; out.position.z = p.position.z;
  out.orientation.x = p.orientation.x; out.orientation.y = p.orientation.y;
  out.orientation.z = p.orientation.z; out.orientation.w = p.orientation.w;
  return out;
}

inline EdvrFovf toPluginFov(const XrFovf& f) {
  EdvrFovf out{};
  out.angleLeft = f.angleLeft; out.angleRight = f.angleRight; out.angleUp = f.angleUp; out.angleDown = f.angleDown;
  return out;
}

// `head` is the located head pose of the frame whose `views` these are.
inline PluginFrameArgs makePluginFrameArgs(const XrPosef& head, const XrView (&views)[2]) {
  PluginFrameArgs a{};
  a.head = toPluginPose(head);
  for (unsigned i = 0; i < 2; ++i) {
    a.eyePose[i] = toPluginPose(views[i].pose);
    a.eyeFov[i] = toPluginFov(views[i].fov);
  }
  return a;
}

}  // namespace edvr::openxr
