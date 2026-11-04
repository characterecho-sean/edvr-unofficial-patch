#pragma once
#include "geometry_snapshot.h"
#include "seat_transform.h"

namespace edvr::openxr {
// F189 (#144): the seated space is the natural LOCAL or STAGE space and a stored origin. Externally serialized (the owner thread), borrowed session and natural space.
// Moving the origin makes no OpenXR call: the runtime is never asked for a reference space for the seat, so nothing is created or destroyed when the origin moves, and a
// layer that is generating frames from ours sees one layer space for the whole session. The numbers the game reads are the located ones turned by toSeated, the layers
// the game's frames become are turned by toNatural (seat_transform.h); both use the origin the frame began with.
// There is nothing to clean up: no child space exists. xrDestroySession ends the natural space with the rest.
class SeatedSpace {
 public:
  SeatedSpace()=default;
  SeatedSpace(const SeatedSpace&)=delete;
  SeatedSpace& operator=(const SeatedSpace&)=delete;
  bool begin(XrSession session,XrSpace naturalSpace,XrReferenceSpaceType naturalType=XR_REFERENCE_SPACE_TYPE_LOCAL) {
    if(session_||!session||!naturalSpace||
       (naturalType!=XR_REFERENCE_SPACE_TYPE_LOCAL&&naturalType!=XR_REFERENCE_SPACE_TYPE_STAGE))return false;
    session_=session;local_=naturalSpace;type_=naturalType;origin_=XrPosef{{0,0,0,1},{0,0,0}};sets_=0;usable_=true;return true;
  }
  // The seated space's origin in the natural space. Validated, stored, and nothing else: no OpenXR call.
  XrResult setOrigin(const XrPosef& originInNatural) {
    if(!usable_)return XR_ERROR_CALL_ORDER_INVALID;
    if(!detail::poseValid(originInNatural))return XR_ERROR_POSE_INVALID;
    origin_=originInNatural;++sets_;return XR_SUCCESS;
  }
  // The handle every locate and every layer uses: the natural space, always. Null until begin and after shutdown.
  XrSpace space() const {return usable_?local_:XR_NULL_HANDLE;}
  // The origin: identity until the first setOrigin.
  const XrPosef& origin() const {return origin_;}
  // The natural space and the origin as they stand now, for a frame to keep.
  SeatedFrame frame() const {SeatedFrame f;f.space=space();f.origin=origin_;return f;}
  XrPosef toSeated(const XrPosef& natural) const {return seatxf::toSeated(origin_,natural);}
  XrPosef toNatural(const XrPosef& seated) const {return seatxf::toNatural(origin_,seated);}
  // A linear or an angular velocity located in the natural space, in seated coordinates (turned by the inverse of the origin's rotation).
  XrVector3f velocityToSeated(const XrVector3f& natural) const {return seatxf::velocityToSeated(origin_,natural);}
  static XrPosef toSeated(const XrPosef& origin,const XrPosef& natural) {return seatxf::toSeated(origin,natural);}
  static XrPosef toNatural(const XrPosef& origin,const XrPosef& seated) {return seatxf::toNatural(origin,seated);}
  static XrVector3f velocityToSeated(const XrPosef& origin,const XrVector3f& natural) {return seatxf::velocityToSeated(origin,natural);}
  XrReferenceSpaceType naturalType() const {return type_;}
  // Origins stored since begin: the diagnostic beside spaceCreates.
  uint64_t originSets() const {return sets_;}
  // Reference spaces this object made in the runtime. None, by construction: it holds no create call. The log line carries it as space_creates, which a log from before
  // F189 lacks, so a zero there is this code running (and origin_sets climbing is the origin moving).
  static constexpr uint64_t spaceCreates() {return 0;}
  XrResult shutdown() {
    local_=XR_NULL_HANDLE;session_=XR_NULL_HANDLE;usable_=false;type_=XR_REFERENCE_SPACE_TYPE_LOCAL;
    origin_=XrPosef{{0,0,0,1},{0,0,0}};sets_=0;
    return XR_SUCCESS;
  }
 private:
  XrSession session_=XR_NULL_HANDLE;
  XrSpace local_=XR_NULL_HANDLE;
  XrPosef origin_{{0,0,0,1},{0,0,0}};
  uint64_t sets_=0;
  XrReferenceSpaceType type_=XR_REFERENCE_SPACE_TYPE_LOCAL;
  bool usable_=false;
};
} // namespace edvr::openxr
