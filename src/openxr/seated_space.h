#pragma once
#include "geometry_snapshot.h"
#include <initializer_list>

namespace edvr::openxr {
struct SeatedDispatch {
  PFN_xrCreateReferenceSpace create=nullptr;
  PFN_xrDestroySpace destroy=nullptr;
};
// Externally serialized, borrowed session and natural LOCAL space. The owner
// must finish/abandon every frame that references the current space before
// replacement or shutdown, and keep this object alive until its lease ends.
// No implicit destructor calls. The session owner cleans up children whose
// destruction had an uncertain result: xrDestroySession implicitly destroys
// every remaining child, without needing its numeric handle. Never retry it.
class SeatedSpace {
 public:
  SeatedSpace()=default;
  SeatedSpace(const SeatedSpace&)=delete;
  SeatedSpace& operator=(const SeatedSpace&)=delete;
  bool begin(SeatedDispatch api,XrSession session,XrSpace naturalLocal) {
    if(session_||!session||!naturalLocal||!api.create||!api.destroy)return false;
    api_=api;session_=session;local_=naturalLocal;usable_=true;return true;
  }
  XrResult replace(const XrPosef& originInNaturalLocal) {
    if(!usable_)return XR_ERROR_CALL_ORDER_INVALID;
    if(!detail::poseValid(originInNaturalLocal))return XR_ERROR_POSE_INVALID;
    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    info.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;
    info.poseInReferenceSpace=originInNaturalLocal;
    XrSpace candidate=XR_NULL_HANDLE;
    const XrResult created=api_.create(session_,&info,&candidate);
    // Both specified success codes transfer ownership, but pending loss must
    // stop use. Retain the pending child separately for explicit shutdown.
    if(created!=XR_SUCCESS) {
      if(created==XR_SESSION_LOSS_PENDING&&candidate)pending_=candidate;
      usable_=false;return created;
    }
    if(!candidate){usable_=false;return XR_ERROR_RUNTIME_FAILURE;}
    const XrSpace previous=owned_;owned_=candidate;
    if(previous) {
      const XrResult destroyed=api_.destroy(previous);
      if(destroyed!=XR_SUCCESS){usable_=false;return destroyed;}
    }
    return XR_SUCCESS;
  }
  XrSpace space() const {return usable_?(owned_?owned_:local_):XR_NULL_HANDLE;}
  XrResult shutdown() {
    XrResult first=XR_SUCCESS;
    for(XrSpace space:{pending_,owned_})if(space) {
      const XrResult result=api_.destroy(space);
      if(first==XR_SUCCESS&&result!=XR_SUCCESS)first=result;
    }
    owned_=pending_=local_=XR_NULL_HANDLE;session_=XR_NULL_HANDLE;api_={};usable_=false;
    return first;
  }
 private:
  SeatedDispatch api_{};
  XrSession session_=XR_NULL_HANDLE;
  XrSpace local_=XR_NULL_HANDLE,owned_=XR_NULL_HANDLE,pending_=XR_NULL_HANDLE;
  bool usable_=false;
};
} // namespace edvr::openxr
