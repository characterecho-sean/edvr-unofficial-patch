#pragma once
#include "session_binding.h"
#include "session_state.h"
#include "geometry_snapshot.h"

namespace edvr::openxr {
struct LocateDispatch {PFN_xrLocateViews locateViews=nullptr;PFN_xrLocateSpace locateSpace=nullptr;};
// Call only on the serialized frame owner, within a successfully begun frame.
// It does not wait/begin/end or advance a frame, and it also runs on no-render
// frames for startup. Both locates use exactly the same predicted time/LOCAL.
inline XrResult locateGeometry(const LocateDispatch& api,const SessionBinding& binding,
                              const Frame& frame,uint64_t generation,
                              const XrViewConfigurationView (&sizes)[2],GeometryInput& out) {
  if(!api.locateViews||!api.locateSpace)return XR_ERROR_FUNCTION_UNSUPPORTED;
  if(!binding.session()||!binding.localSpace()||!binding.viewSpace())return XR_ERROR_HANDLE_INVALID;
  if(!generation||!frame.sequence||(frame.status!=FrameStatus::Open&&frame.status!=FrameStatus::NoRender&&frame.status!=FrameStatus::Discarded))
    return XR_ERROR_CALL_ORDER_INVALID;
  GeometryInput input{};input.generation=generation;input.sequence=frame.sequence;input.displayTime=frame.predictedDisplayTime;
  for(unsigned eye=0;eye<2;++eye){input.width[eye]=sizes[eye].recommendedImageRectWidth;input.height[eye]=sizes[eye].recommendedImageRectHeight;}
  XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate.displayTime=frame.predictedDisplayTime;locate.space=binding.localSpace();
  XrViewState state{XR_TYPE_VIEW_STATE};uint32_t count=0;
  XrResult r=api.locateViews(binding.session(),&locate,&state,2,&count,input.views);if(r!=XR_SUCCESS)return r;
  if(count!=2)return XR_ERROR_RUNTIME_FAILURE;
  input.viewFlags=state.viewStateFlags;
  XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
  r=api.locateSpace(binding.viewSpace(),binding.localSpace(),frame.predictedDisplayTime,&head);if(r!=XR_SUCCESS)return r;
  input.headFlags=head.locationFlags;input.headPose=head.pose;
  // Missing validity bits are an ordinary tracking result, not fabricated
  // geometry or a runtime error. Publication validates/invalidate that sample.
  out=input;return XR_SUCCESS;
}
}
