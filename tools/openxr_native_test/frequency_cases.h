#pragma once
#include "../../src/openxr/native_runtime_host.h"
#include <thread>

namespace edvr::openxr::test {
namespace frequency_fixture {
inline unsigned calls=0;
inline float value=120;
inline XrResult status=XR_SUCCESS;
inline XrResult XRAPI_PTR query(XrSession,float* hz) {
  ++calls;*hz=value;return status;
}
}
template<class Check> void runFrequencyCases(Check&& check) {
  using namespace frequency_fixture;
  OwnerService owner;RenderThreadDispatcher dispatcher(owner);RenderRoute route{dispatcher};
  auto host=std::make_unique<NativeRuntimeHost>(owner,dispatcher,route);
  SystemRead metadata{};metadata.connected=true;
  host->geometryGeneration=host->geometry.begin(metadata);
  host->session=reinterpret_cast<XrSession>(uintptr_t(2));
  host->api.displayRefreshRate=query;
  calls=0;value=120;status=XR_SUCCESS;
  host->refreshDisplayFrequency("fixture_no_extension");
  auto current=host->read();
  vr::ETrackedPropertyError error=vr::TrackedProp_UnknownProperty;
  check(current.displayFrequencyAvailable&&current.displayFrequencyEstimated&&current.displayFrequency==90&&calls==0,
    "unsupported extension uses explicit 90 Hz compatibility default without querying XR");
  check(host->systemInterface.GetFloatTrackedDeviceProperty(0,vr::Prop_DisplayFrequency_Float,&error)==90&&error==vr::TrackedProp_Success,
    "real OpenVR facade returns compatibility frequency successfully");
  host->displayRefreshExtension=true;
  host->api.displayRefreshRate=nullptr;
  host->refreshDisplayFrequency("fixture_missing_function");
  check(host->read().displayFrequency==90&&host->read().displayFrequencyEstimated&&calls==0,
    "missing optional function keeps startup-compatible value");
  host->api.displayRefreshRate=query;
  host->refreshDisplayFrequency("fixture_runtime");
  current=host->read();
  check(current.displayFrequencyAvailable&&!current.displayFrequencyEstimated&&current.displayFrequency==120&&calls==1,
    "validated runtime frequency replaces compatibility placeholder");
  bool cached=true;
  for(unsigned i=0;i<100;++i)
    cached&=host->systemInterface.GetFloatTrackedDeviceProperty(0,vr::Prop_DisplayFrequency_Float,nullptr)==120;
  check(cached&&calls==1,"repeated game property reads return cached value without making XR queries");
  for(float invalid:{0.0f,-1.0f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
    value=invalid;host->refreshDisplayFrequency("fixture_invalid");
    check(host->read().displayFrequency==90&&host->read().displayFrequencyEstimated,"invalid runtime frequency is not published");
  }
  value=72;status=XR_ERROR_RUNTIME_FAILURE;host->refreshDisplayFrequency("fixture_error");
  check(host->read().displayFrequency==90&&host->read().displayFrequencyEstimated,"failed query cannot publish its output buffer");
  status=XR_SESSION_LOSS_PENDING;host->refreshDisplayFrequency("fixture_loss_pending");
  check(host->read().displayFrequency==72&&!host->read().displayFrequencyEstimated,"defined positive success result supplies runtime rate");
  status=XR_SUCCESS;
  XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
  XrEventDataDisplayRefreshRateChangedFB event{XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB};
  event.fromDisplayRefreshRate=72;event.toDisplayRefreshRate=80;
  std::memcpy(&buffer,&event,sizeof(event));
  NativeRuntimeHost::referenceEvent(buffer,host.get());
  check(host->read().displayFrequency==80&&!host->read().displayFrequencyEstimated,"runtime refresh-change event updates cached frequency");
  host->geometry.invalidate(host->geometryGeneration);
  check(host->read().displayFrequency==80,"tracking invalidation preserves display frequency");
  event.toDisplayRefreshRate=0;std::memcpy(&buffer,&event,sizeof(event));
  NativeRuntimeHost::referenceEvent(buffer,host.get());
  check(host->read().displayFrequency==80,"invalid refresh event preserves last valid rate");
  host->displayRefreshExtension=false;
  event.toDisplayRefreshRate=144;std::memcpy(&buffer,&event,sizeof(event));
  NativeRuntimeHost::referenceEvent(buffer,host.get());
  check(host->read().displayFrequency==80,"unenabled extension event cannot change frequency");
  host->displayRefreshExtension=true;
  const unsigned before=calls;
  std::thread wrongOwner([&]{host->refreshDisplayFrequency("fixture_wrong_thread");
    NativeRuntimeHost::referenceEvent(buffer,host.get());});wrongOwner.join();
  check(calls==before&&host->read().displayFrequency==80,"foreign thread cannot query or publish runtime frequency");
  const uint64_t old=host->geometryGeneration;
  host->geometry.retire(old);
  host->refreshDisplayFrequency("fixture_retired");
  NativeRuntimeHost::referenceEvent(buffer,host.get());
  check(!host->read().displayFrequencyAvailable&&calls==before,"retired session cannot republish frequency");
  host->geometryGeneration=host->geometry.begin(metadata);
  check(!host->geometry.displayFrequency(old,144,false)&&!host->read().displayFrequencyAvailable,
    "old generation cannot publish into replacement session");
  host->api.displayRefreshRate=nullptr;host->refreshDisplayFrequency("fixture_new_session");
  check(host->read().displayFrequency==90&&host->read().displayFrequencyEstimated,"replacement session starts with its own frequency source");
  host->session=XR_NULL_HANDLE;
}
}
