#include "session_binding.h"
#include <dxgi.h>
#include <vector>
#include <algorithm>

namespace edvr::openxr {
namespace {
XrResult enumerate(const BindingDispatch& d,XrSession session,std::vector<XrReferenceSpaceType>& out) {
  uint32_t count=0;XrResult r=d.enumerateSpaces(session,0,&count,nullptr);
  if(r!=XR_SUCCESS)return r;
  for(unsigned attempt=0;attempt<3;++attempt) {
    if(!count)return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    if(count>64)return XR_ERROR_LIMIT_REACHED;
    std::vector<XrReferenceSpaceType> values(count);uint32_t written=0;
    r=d.enumerateSpaces(session,count,&written,values.data());
    if(r==XR_ERROR_SIZE_INSUFFICIENT&&written>count){count=written;continue;}
    if(r!=XR_SUCCESS)return r;
    if(written>count)return XR_ERROR_RUNTIME_FAILURE;
    values.resize(written);out.swap(values);return XR_SUCCESS;
  }
  return XR_ERROR_SIZE_INSUFFICIENT;
}
bool compatibleDevice(ID3D11Device* device,const XrGraphicsRequirementsD3D11KHR& requirements) {
  if(FAILED(device->GetDeviceRemovedReason())||device->GetFeatureLevel()<requirements.minFeatureLevel)return false;
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if(FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))||FAILED(dxgi->GetAdapter(&adapter)))return false;
  DXGI_ADAPTER_DESC desc{};
  return SUCCEEDED(adapter->GetDesc(&desc))&&desc.AdapterLuid.LowPart==requirements.adapterLuid.LowPart&&
         desc.AdapterLuid.HighPart==requirements.adapterLuid.HighPart;
}
}
XrResult SessionBinding::initialize(const BindingDispatch& d,XrInstance instance,XrSystemId system,ID3D11Device* candidate) {
  if(device_||session_||local_||view_)return XR_ERROR_CALL_ORDER_INVALID;
  if(!instance||!candidate)return XR_ERROR_HANDLE_INVALID;
  if(system==XR_NULL_SYSTEM_ID)return XR_ERROR_SYSTEM_INVALID;
  if(!d.graphicsRequirements||!d.createSession||!d.destroySession||!d.enumerateSpaces||!d.createSpace||!d.destroySpace)
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
  XrResult r=d.graphicsRequirements(instance,system,&requirements);if(r!=XR_SUCCESS)return r;
  if(!compatibleDevice(candidate,requirements))return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  dispatch_=d;device_=candidate;
  auto failed=[&](XrResult primary){shutdown();return primary;};
  XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};binding.device=candidate;
  XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};create.next=&binding;create.systemId=system;
  // Only the command's specified success codes transfer an output handle.
  XrSession session=XR_NULL_HANDLE;r=d.createSession(instance,&create,&session);
  if(r==XR_SUCCESS)session_=session;
  if(r!=XR_SUCCESS)return failed(r);
  if(!session_)return failed(XR_ERROR_RUNTIME_FAILURE);
  std::vector<XrReferenceSpaceType> spaces;r=enumerate(d,session_,spaces);if(r!=XR_SUCCESS)return failed(r);
  for(auto type:{XR_REFERENCE_SPACE_TYPE_LOCAL,XR_REFERENCE_SPACE_TYPE_VIEW}) {
    if(std::find(spaces.begin(),spaces.end(),type)==spaces.end())return failed(XR_ERROR_REFERENCE_SPACE_UNSUPPORTED);
  }
  for(auto type:{XR_REFERENCE_SPACE_TYPE_LOCAL,XR_REFERENCE_SPACE_TYPE_VIEW}) {
    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};info.referenceSpaceType=type;info.poseInReferenceSpace.orientation.w=1;
    XrSpace space=XR_NULL_HANDLE;r=d.createSpace(session_,&info,&space);
    if(r==XR_SUCCESS||r==XR_SESSION_LOSS_PENDING){if(type==XR_REFERENCE_SPACE_TYPE_LOCAL)local_=space;else view_=space;}
    if(r!=XR_SUCCESS)return failed(r);
    if(!space)return failed(XR_ERROR_RUNTIME_FAILURE);
  }
  return XR_SUCCESS;
}
XrResult SessionBinding::shutdown() {
  XrResult first=XR_SUCCESS;
  auto record=[&](XrResult r){if(r!=XR_SUCCESS&&first==XR_SUCCESS)first=r;};
  if(view_&&dispatch_.destroySpace)record(dispatch_.destroySpace(view_));
  if(local_&&dispatch_.destroySpace)record(dispatch_.destroySpace(local_));
  if(session_&&dispatch_.destroySession)record(dispatch_.destroySession(session_));
  view_=local_=XR_NULL_HANDLE;session_=XR_NULL_HANDLE;device_.Reset();dispatch_={};return first;
}
XrResult SessionBinding::validateTexture(ID3D11Texture2D* texture)const {
  if(!texture||!device_||!session_)return XR_ERROR_HANDLE_INVALID;
  if(FAILED(device_->GetDeviceRemovedReason()))return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  Microsoft::WRL::ComPtr<ID3D11Device> owner;texture->GetDevice(&owner);
  Microsoft::WRL::ComPtr<IUnknown> expected,actual;
  if(FAILED(device_.As(&expected))||FAILED(owner.As(&actual))||expected.Get()!=actual.Get())return XR_ERROR_GRAPHICS_DEVICE_INVALID;
  return XR_SUCCESS;
}
}
