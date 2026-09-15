#pragma once
#include "projection_math.h"
#include <vector>
#include <memory>
#include <algorithm>

namespace edvr::openxr {
struct NativeHiddenMask {
  std::vector<XrVector2f> vertices;
  std::vector<uint32_t> indices;
};
struct NativeHiddenMasks {
  uint64_t generation=0, revision=0;
  NativeHiddenMask eyes[2];
  // UV guard for two pixels at the minimum EDVR render scale. It is fixed
  // for this runtime recommendation, so live resolution changes reuse it.
  float guard[2][2]{};
};

// Owner-only optional query. Malformed, unavailable or changing output never
// escapes as a partially populated mesh. Allocation and retry work is bounded.
inline XrResult readHiddenMask(PFN_xrGetVisibilityMaskKHR query,XrSession session,
                              unsigned eye,NativeHiddenMask& out) {
  out={};
  if(!query||!session||eye>1)return XR_ERROR_VALIDATION_FAILURE;
  for(unsigned attempt=0;attempt<3;++attempt) {
    XrVisibilityMaskKHR mask{XR_TYPE_VISIBILITY_MASK_KHR};
    auto r=query(session,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,eye,
                 XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR,&mask);
    if(r!=XR_SUCCESS&&r!=XR_SESSION_LOSS_PENDING)return XR_FAILED(r)?r:XR_ERROR_RUNTIME_FAILURE;
    if(!mask.vertexCountOutput&&!mask.indexCountOutput)return XR_SUCCESS;
    if(!mask.vertexCountOutput||!mask.indexCountOutput||mask.indexCountOutput%3||
       mask.vertexCountOutput>8192||mask.indexCountOutput>24576)return XR_ERROR_LIMIT_REACHED;
    NativeHiddenMask candidate;
    candidate.vertices.resize(mask.vertexCountOutput);candidate.indices.resize(mask.indexCountOutput);
    mask.vertexCapacityInput=uint32_t(candidate.vertices.size());mask.vertices=candidate.vertices.data();
    mask.indexCapacityInput=uint32_t(candidate.indices.size());mask.indices=candidate.indices.data();
    r=query(session,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,eye,
            XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR,&mask);
    if(r==XR_ERROR_SIZE_INSUFFICIENT)continue;
    if(r!=XR_SUCCESS&&r!=XR_SESSION_LOSS_PENDING)return XR_FAILED(r)?r:XR_ERROR_RUNTIME_FAILURE;
    if(mask.vertexCountOutput>mask.vertexCapacityInput||mask.indexCountOutput>mask.indexCapacityInput||
       mask.indexCountOutput%3)return XR_ERROR_RUNTIME_FAILURE;
    candidate.vertices.resize(mask.vertexCountOutput);candidate.indices.resize(mask.indexCountOutput);
    for(const auto& v:candidate.vertices)if(!finite(v.x)||!finite(v.y))return XR_ERROR_RUNTIME_FAILURE;
    for(auto i:candidate.indices)if(i>=candidate.vertices.size())return XR_ERROR_RUNTIME_FAILURE;
    out=std::move(candidate);return XR_SUCCESS;
  }
  return XR_ERROR_SIZE_INSUFFICIENT;
}

// OpenXR mask vertices are rays on z=-1, not normalized OpenVR mesh UVs.
// Use each eye's actual asymmetric frustum; cant is already in its view pose.
// Inset each triangle, never enlarge it, to leave room for jitter/filter taps.
// Thin triangles disappear and internal seams may draw extra pixels safely.
inline bool projectHiddenMask(const NativeHiddenMask& mask,const RawFov& fov,
                              float guardU,float guardV,std::vector<vr::HmdVector2_t>& out) {
  out.clear();RawFov checked{};
  if(!shiftedRawFov(fov,0,0,checked)||!finite(guardU)||!finite(guardV)||guardU<=0||guardV<=0||
     mask.indices.size()%3||mask.indices.size()>24576)return false;
  const double width=double(fov.right)-fov.left,height=double(fov.bottom)-fov.top;
  std::vector<vr::HmdVector2_t> candidate;candidate.reserve(mask.indices.size());
  for(size_t t=0;t<mask.indices.size();t+=3) {
    double x[3],y[3];
    for(unsigned j=0;j<3;++j) {
      const auto index=mask.indices[t+j];if(index>=mask.vertices.size())return false;
      const auto v=mask.vertices[index];if(!finite(v.x)||!finite(v.y))return false;
      x[j]=(double(v.x)-fov.left)/width/guardU;
      y[j]=(double(v.y)-fov.top)/height/guardV;
      if(!std::isfinite(x[j])||!std::isfinite(y[j]))return false;
    }
    const double area=(x[1]-x[0])*(y[2]-y[0])-(y[1]-y[0])*(x[2]-x[0]);
    if(!std::isfinite(area))return false;
    if(std::fabs(area)<1e-12)continue;
    const double sign=area>0?1:-1;
    double a[3],b[3],c[3];bool valid=true;
    for(unsigned j=0;j<3;++j) {
      const unsigned k=(j+1)%3;const double length=std::hypot(x[k]-x[j],y[k]-y[j]);
      if(length<1e-12){valid=false;break;}
      a[j]=sign*(y[j]-y[k])/length;b[j]=sign*(x[k]-x[j])/length;
      c[j]=a[j]*x[j]+b[j]*y[j]+1;
    }
    if(!valid)continue;
    vr::HmdVector2_t triangle[3]{};
    for(unsigned j=0;j<3&&valid;++j) {
      const unsigned k=(j+2)%3;const double det=a[j]*b[k]-a[k]*b[j];
      if(std::fabs(det)<1e-12){valid=false;break;}
      const double px=(c[j]*b[k]-c[k]*b[j])/det,py=(a[j]*c[k]-a[k]*c[j])/det;
      for(unsigned edge=0;edge<3;++edge)if(a[edge]*px+b[edge]*py<c[edge]-1e-7)valid=false;
      const double u=px*guardU,v=py*guardV;
      if(!std::isfinite(u)||!std::isfinite(v)||std::fabs(u)>16||std::fabs(v)>16)return false;
      triangle[j].v[0]=float(u);triangle[j].v[1]=float(v);
    }
    if(valid)candidate.insert(candidate.end(),triangle,triangle+3);
  }
  out.swap(candidate);return true;
}
}
