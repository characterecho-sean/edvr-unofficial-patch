#pragma once
#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif
#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>

namespace edvr::openxr {
struct BindingDispatch {
  PFN_xrGetD3D11GraphicsRequirementsKHR graphicsRequirements=nullptr;
  PFN_xrCreateSession createSession=nullptr;
  PFN_xrDestroySession destroySession=nullptr;
  PFN_xrEnumerateReferenceSpaces enumerateSpaces=nullptr;
  PFN_xrCreateReferenceSpace createSpace=nullptr;
  PFN_xrDestroySpace destroySpace=nullptr;
};
// Externally serialized owner. The instance/dispatch outlive this binding.
// Caller has enabled D3D11 and selected a supported view/blend configuration.
// Candidate must be a live COM object; this class never creates another device.
// The owner ends all frame/renderer use before shutdown (outside DllMain).
class SessionBinding {
 public:
  SessionBinding()=default;
  ~SessionBinding(){shutdown();}
  SessionBinding(const SessionBinding&)=delete;
  SessionBinding& operator=(const SessionBinding&)=delete;
  SessionBinding(SessionBinding&&)=delete;
  SessionBinding& operator=(SessionBinding&&)=delete;
  XrResult initialize(const BindingDispatch&,XrInstance,XrSystemId,ID3D11Device*);
  XrResult shutdown();
  XrSession session()const{return session_;}
  XrSpace localSpace()const{return local_;}
  XrSpace viewSpace()const{return view_;}
  ID3D11Device* device()const{return device_.Get();}
  XrResult validateTexture(ID3D11Texture2D*)const;
 private:
  BindingDispatch dispatch_{};
  XrSession session_=XR_NULL_HANDLE;
  XrSpace local_=XR_NULL_HANDLE,view_=XR_NULL_HANDLE;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
};
}
