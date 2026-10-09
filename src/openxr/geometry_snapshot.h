#pragma once

#include "projection_math.h"
#include <openxr/openxr.h>
#include <cstdint>
#include <cmath>
#include <limits>

namespace edvr::openxr {

struct GeometryInput {
  uint64_t generation = 0, sequence = 0;
  XrTime displayTime = 0;
  XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  XrViewStateFlags viewFlags = 0;
  XrPosef headPose{};
  XrSpaceLocationFlags headFlags = 0;
  uint32_t width[2]{}, height[2]{};
};

struct GeometrySnapshot {
  GeometryInput native;
  vr::HmdMatrix34_t headToLocal{}, eyeToHead[2]{};
  RawFov raw[2]{};
};

namespace detail {
inline bool poseValid(const XrPosef& p) {
  const double q = double(p.orientation.x)*p.orientation.x + double(p.orientation.y)*p.orientation.y +
                   double(p.orientation.z)*p.orientation.z + double(p.orientation.w)*p.orientation.w;
  return finite(p.orientation.x) && finite(p.orientation.y) && finite(p.orientation.z) &&
         finite(p.orientation.w) && finite(p.position.x) && finite(p.position.y) &&
         finite(p.position.z) && std::fabs(q - 1.0f) <= 1.0e-3f;
}
inline void rotation(const XrQuaternionf& q, double r[3][3]) {
  const double x=q.x,y=q.y,z=q.z,w=q.w;
  r[0][0]=1-2*(y*y+z*z); r[0][1]=2*(x*y-z*w); r[0][2]=2*(x*z+y*w);
  r[1][0]=2*(x*y+z*w); r[1][1]=1-2*(x*x+z*z); r[1][2]=2*(y*z-x*w);
  r[2][0]=2*(x*z-y*w); r[2][1]=2*(y*z+x*w); r[2][2]=1-2*(x*x+y*y);
}
inline void rigid(const XrPosef& p, double m[4][4]) {
  double r[3][3]; rotation(p.orientation,r); for(int i=0;i<3;++i)for(int j=0;j<3;++j)m[i][j]=r[i][j];
  m[0][3]=p.position.x; m[1][3]=p.position.y; m[2][3]=p.position.z;
  for(int j=0;j<3;++j)m[3][j]=0; m[3][3]=1;
}
inline void inverseRigid(const double a[4][4], double out[4][4]) {
  for(int i=0;i<3;++i)for(int j=0;j<3;++j)out[i][j]=a[j][i];
  for(int i=0;i<3;++i){out[i][3]=0;for(int j=0;j<3;++j)out[i][3]-=out[i][j]*a[j][3];}
  for(int j=0;j<3;++j)out[3][j]=0; out[3][3]=1;
}
inline void product(const double a[4][4],const double b[4][4],double out[4][4]) {
  for(int i=0;i<4;++i)for(int j=0;j<4;++j){out[i][j]=0;for(int k=0;k<4;++k)out[i][j]+=a[i][k]*b[k][j];}
}
inline bool narrow(const double m[3][4], vr::HmdMatrix34_t& out) {
  vr::HmdMatrix34_t c{};
  for(int i=0;i<3;++i)for(int j=0;j<4;++j){if(!std::isfinite(m[i][j])||std::fabs(m[i][j])>(std::numeric_limits<float>::max)())return false;c.m[i][j]=(float)m[i][j];}
  out=c; return true;
}
}

inline bool makeGeometrySnapshot(const GeometryInput& in, GeometrySnapshot& out) {
  if (!in.generation || !in.sequence ||
      !(in.viewFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) ||
      !(in.viewFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ||
      !(in.headFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ||
      !(in.headFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) || !detail::poseValid(in.headPose)) return false;
  for(int i=0;i<2;++i) if(!in.width[i] || !in.height[i] || in.width[i]>0x7fffffffu || in.height[i]>0x7fffffffu ||
      in.views[i].type!=XR_TYPE_VIEW || in.views[i].next || !detail::poseValid(in.views[i].pose)) return false;
  RawFov raw[2]{}; for(int i=0;i<2;++i) if(!fovToRaw(in.views[i].fov,raw[i])) return false;
  double head[4][4], inv[4][4]; detail::rigid(in.headPose,head); detail::inverseRigid(head,inv);
  GeometrySnapshot c{}; c.native=in; if(!detail::narrow(head,c.headToLocal))return false;
  for(int i=0;i<2;++i){c.raw[i]=raw[i];double eye[4][4],rel[4][4];detail::rigid(in.views[i].pose,eye);detail::product(inv,eye,rel);if(!detail::narrow(rel,c.eyeToHead[i]))return false;}
  out=c; return true;
}
}
