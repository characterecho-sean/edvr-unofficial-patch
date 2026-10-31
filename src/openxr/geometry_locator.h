#pragma once
#include "session_binding.h"
#include "session_state.h"
#include "geometry_snapshot.h"
#include "seat_transform.h"

namespace edvr::openxr {
struct LocateDispatch {PFN_xrLocateViews locateViews=nullptr;PFN_xrLocateSpace locateSpace=nullptr;};
// Call only on the serialized frame owner, within a successfully begun frame.
// It does not wait/begin/end or advance a frame, and it also runs on no-render
// frames for startup. Both locates use exactly the same predicted time/origin.
// seatOrigin, when given (F189), is the origin of the seated space the caller means: `origin` is then the natural space, and the located views, the head and the head's
// velocity are turned into that seated space (seat_transform.h) before they are returned, each part by the validity the runtime gave it, the flags untouched.
inline XrResult locateGeometryIn(const LocateDispatch& api,const SessionBinding& binding,
                                const Frame& frame,uint64_t generation,
                                const XrViewConfigurationView (&sizes)[2],GeometryInput& out,
                                XrSpaceVelocity* headVelocity,XrSpace origin,const XrPosef* seatOrigin) {
  if(!api.locateViews||!api.locateSpace)return XR_ERROR_FUNCTION_UNSUPPORTED;
  if(!binding.session()||!binding.localSpace()||!binding.viewSpace())return XR_ERROR_HANDLE_INVALID;
  if(!origin)origin=binding.localSpace();
  if(!generation||!frame.sequence||(frame.status!=FrameStatus::Open&&frame.status!=FrameStatus::NoRender&&frame.status!=FrameStatus::Discarded))
    return XR_ERROR_CALL_ORDER_INVALID;
  GeometryInput input{};input.generation=generation;input.sequence=frame.sequence;input.displayTime=frame.predictedDisplayTime;
  for(unsigned eye=0;eye<2;++eye){input.width[eye]=sizes[eye].recommendedImageRectWidth;input.height[eye]=sizes[eye].recommendedImageRectHeight;}
  XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate.displayTime=frame.predictedDisplayTime;locate.space=origin;
  XrViewState state{XR_TYPE_VIEW_STATE};uint32_t count=0;
  XrResult r=api.locateViews(binding.session(),&locate,&state,2,&count,input.views);if(r!=XR_SUCCESS)return r;
  if(count!=2)return XR_ERROR_RUNTIME_FAILURE;
  input.viewFlags=state.viewStateFlags;
  XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
  XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
  if(headVelocity)head.next=&velocity;
  r=api.locateSpace(binding.viewSpace(),origin,frame.predictedDisplayTime,&head);if(r!=XR_SUCCESS)return r;
  if(head.type!=XR_TYPE_SPACE_LOCATION||head.next!=(headVelocity?&velocity:nullptr)||
     velocity.type!=XR_TYPE_SPACE_VELOCITY||velocity.next)return XR_ERROR_VALIDATION_FAILURE;
  if(seatOrigin) {
    seatxf::viewsToSeated(*seatOrigin,input.viewFlags,input.views,2);
    seatxf::locationToSeated(*seatOrigin,head);
    if(headVelocity)seatxf::velocityToSeated(*seatOrigin,velocity);
  }
  input.headFlags=head.locationFlags;input.headPose=head.pose;
  // Missing validity bits are an ordinary tracking result, not fabricated
  // geometry or a runtime error. Publication validates/invalidate that sample.
  out=input;if(headVelocity)*headVelocity=velocity;return XR_SUCCESS;
}
// The locate in `origin` as the runtime gives it (a null origin is the binding's LOCAL): for a space that really is the one asked about.
inline XrResult locateGeometry(const LocateDispatch& api,const SessionBinding& binding,
                              const Frame& frame,uint64_t generation,
                              const XrViewConfigurationView (&sizes)[2],GeometryInput& out,
                              XrSpaceVelocity* headVelocity=nullptr,XrSpace origin=XR_NULL_HANDLE) {
  return locateGeometryIn(api,binding,frame,generation,sizes,out,headVelocity,origin,nullptr);
}
// F189: the same locate in a frame's seated space: against the natural space the SeatedFrame names, turned by the origin that frame stood for. A SeatedFrame with no
// space falls back to the binding's LOCAL exactly as a null origin does, and is then not turned.
inline XrResult locateGeometry(const LocateDispatch& api,const SessionBinding& binding,
                              const Frame& frame,uint64_t generation,
                              const XrViewConfigurationView (&sizes)[2],GeometryInput& out,
                              XrSpaceVelocity* headVelocity,const SeatedFrame& seat) {
  return locateGeometryIn(api,binding,frame,generation,sizes,out,headVelocity,seat.space,seat.space?&seat.origin:nullptr);
}

enum class NaturalSpaceProbe {Stage,LocalFallback,Pending};
// Enumeration and creation do not guarantee a usable STAGE. Probe both eye
// views and the head at the same begun frame's predicted time before the
// first seated origin or saved seat is selected.
inline XrResult probeNaturalSpace(const LocateDispatch& api,const SessionBinding& binding,
                                  const Frame& frame,uint64_t generation,
                                  const XrViewConfigurationView (&sizes)[2],NaturalSpaceProbe& choice) {
  if(binding.naturalType()!=XR_REFERENCE_SPACE_TYPE_STAGE){choice=NaturalSpaceProbe::LocalFallback;return XR_SUCCESS;}
  GeometryInput stage{},local{};GeometrySnapshot snapshot{};
  const XrResult stageResult=locateGeometry(api,binding,frame,generation,sizes,stage,nullptr,binding.naturalSpace());
  if(stageResult==XR_SUCCESS&&makeGeometrySnapshot(stage,snapshot)){choice=NaturalSpaceProbe::Stage;return XR_SUCCESS;}
  const XrResult localResult=locateGeometry(api,binding,frame,generation,sizes,local,nullptr,binding.localSpace());
  if(localResult==XR_SUCCESS&&makeGeometrySnapshot(local,snapshot)){choice=NaturalSpaceProbe::LocalFallback;return XR_SUCCESS;}
  if(stageResult!=XR_SUCCESS)return stageResult;
  if(localResult!=XR_SUCCESS)return localResult;
  choice=NaturalSpaceProbe::Pending;return XR_SUCCESS;
}
}
