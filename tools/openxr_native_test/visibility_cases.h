#pragma once
#include "../../src/openxr/native_runtime_host.h"

namespace edvr::openxr::test {
namespace visibility_fixture {
inline unsigned calls=0;
inline unsigned mode=0;
inline XrResult XRAPI_PTR query(XrSession,XrViewConfigurationType view,unsigned eye,
                               XrVisibilityMaskTypeKHR type,XrVisibilityMaskKHR* mask) {
  ++calls;
  if(view!=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO||type!=XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR||eye>1)
    return XR_ERROR_VALIDATION_FAILURE;
  if(mode==1)return XR_ERROR_RUNTIME_FAILURE;
  if(mode==2){mask->vertexCountOutput=mask->indexCountOutput=0;return XR_SUCCESS;}
  if(mode==3){mask->vertexCountOutput=8193;mask->indexCountOutput=3;return XR_SUCCESS;}
  if(mode==8&&eye==1)return XR_ERROR_RUNTIME_FAILURE;
  if(mode==9)return XR_TIMEOUT_EXPIRED;
  mask->vertexCountOutput=mask->indexCountOutput=3;
  if(!mask->vertexCapacityInput||!mask->indexCapacityInput)return XR_SUCCESS;
  if(mode==4){mask->vertexCountOutput=mask->indexCountOutput=6;return XR_ERROR_SIZE_INSUFFICIENT;}
  if(mode==5){mask->vertexCountOutput=mask->vertexCapacityInput+1;return XR_SUCCESS;}
  mask->vertices[0]={-1,-1};mask->vertices[1]={-1,0};mask->vertices[2]={0,-1};
  mask->indices[0]=0;mask->indices[1]=1;mask->indices[2]=2;
  if(mode==6)mask->vertices[0].x=NAN;
  if(mode==7)mask->indices[1]=99;
  return XR_SUCCESS;
}
}
template<class Check> void runVisibilityCases(Check&& check) {
  using namespace visibility_fixture;
  NativeHiddenMask raw;calls=0;mode=0;
  const auto session=reinterpret_cast<XrSession>(uintptr_t(2));
  check(readHiddenMask(query,session,0,raw)==XR_SUCCESS&&raw.indices.size()==3&&calls==2,
    "optional visibility query follows two-call protocol");
  for(unsigned failure=1;failure<=9;++failure) {
    mode=failure;calls=0;raw.indices={99};const auto result=readHiddenMask(query,session,failure==8?1:0,raw);
    check((failure==2?result==XR_SUCCESS:XR_FAILED(result))&&raw.indices.empty(),
      "failed, unavailable or malformed visibility output never leaks a partial mesh");
    if(failure==4)check(calls==6,"changing mask sizes stop after three attempts");
  }
  OwnerService owner;RenderThreadDispatcher dispatcher(owner);RenderRoute route{dispatcher};
  auto host=std::make_unique<NativeRuntimeHost>(owner,dispatcher,route);
  SystemRead metadata{};metadata.connected=true;host->geometryGeneration=host->geometry.begin(metadata);
  host->session=session;host->api.visibilityMask=query;
  for(unsigned eye=0;eye<2;++eye)host->renderBounds[eye]={4000,3200,8192,8192};
  mode=0;calls=0;host->refreshHiddenMasks("fixture_disabled");
  check(calls==0&&!host->read().hiddenMasks,"unenabled visibility extension is never called");
  host->visibilityMaskExtension=true;host->api.visibilityMask=nullptr;host->refreshHiddenMasks("fixture_missing");
  check(calls==0&&!host->read().hiddenMasks,"missing optional query leaves rendering unmasked");
  host->api.visibilityMask=query;host->refreshHiddenMasks("fixture_runtime");
  auto first=host->read().hiddenMasks;
  check(first&&first->eyes[0].indices.size()==3&&first->eyes[1].indices.size()==3&&calls==4,
    "real host atomically publishes both runtime masks");
  check(first&&std::fabs(first->guard[0][0]-.002f)<1e-7f&&std::fabs(first->guard[0][1]-.0025f)<1e-7f,
    "guard uses the minimum live render scale rather than current slider position");
  const unsigned before=calls;
  std::thread wrong([&]{host->refreshHiddenMasks("fixture_wrong_owner");});wrong.join();
  check(calls==before&&host->read().hiddenMasks==first,"foreign thread cannot make XR mask calls");
  XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
  XrEventDataVisibilityMaskChangedKHR event{XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR};
  event.session=session;event.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;event.viewIndex=1;
  std::memcpy(&buffer,&event,sizeof(event));NativeRuntimeHost::referenceEvent(buffer,host.get());
  check(calls==before&&host->visibilityMaskDirty&&!host->read().hiddenMasks,
    "mask event invalidates cache and defers XR query outside event callback");
  host->refreshHiddenMasks("fixture_event");
  check(host->read().hiddenMasks&&host->read().hiddenMasks!=first&&!host->visibilityMaskDirty,
    "replacement mask is a new immutable publication");
  auto current=host->read().hiddenMasks;event.session=reinterpret_cast<XrSession>(uintptr_t(99));
  std::memcpy(&buffer,&event,sizeof(event));NativeRuntimeHost::referenceEvent(buffer,host.get());
  check(host->read().hiddenMasks==current&&!host->visibilityMaskDirty,"foreign-session mask event ignored");
  mode=8;host->refreshHiddenMasks("fixture_right_error");
  check(!host->read().hiddenMasks,"right-eye failure cannot publish a partial stereo revision");
  mode=0;host->visibilityRefreshes=32;const auto exhausted=calls;
  host->refreshHiddenMasks("fixture_budget");
  check(calls==exhausted&&!host->read().hiddenMasks,"runtime mask refresh budget is bounded");
  host->geometry.retire(host->geometryGeneration);host->visibilityRefreshes=0;
  host->refreshHiddenMasks("fixture_retired");
  check(calls==exhausted&&!host->read().hiddenMasks,"retired generation admits no mask query");
  host->session=XR_NULL_HANDLE;
}
}
