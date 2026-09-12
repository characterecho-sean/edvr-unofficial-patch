#pragma once

#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>
#include <vector>

namespace edvr::openxr {

struct StereoDispatch {
  PFN_xrEnumerateSwapchainFormats enumerateSwapchainFormats = nullptr;
  PFN_xrCreateSwapchain createSwapchain = nullptr;
  PFN_xrDestroySwapchain destroySwapchain = nullptr;
  PFN_xrEnumerateSwapchainImages enumerateSwapchainImages = nullptr;
  PFN_xrAcquireSwapchainImage acquireSwapchainImage = nullptr;
  PFN_xrWaitSwapchainImage waitSwapchainImage = nullptr;
  PFN_xrReleaseSwapchainImage releaseSwapchainImage = nullptr;
};

class D3D11Stereo final {
 public:
  // Diagnostic renderer: owns all immediate-context state. Not a game pass.
  // Borrowed session/dispatch must remain valid until shutdown/destruction.
  D3D11Stereo() = default;
  ~D3D11Stereo();
  D3D11Stereo(const D3D11Stereo&) = delete;
  D3D11Stereo& operator=(const D3D11Stereo&) = delete;
  D3D11Stereo(D3D11Stereo&&) = delete;
  D3D11Stereo& operator=(D3D11Stereo&&) = delete;

  XrResult initialize(const StereoDispatch&, XrSession, ID3D11Device*,
                      const XrViewConfigurationView (&)[2]);
  XrResult render(const XrView (&)[2], XrSpace, XrCompositionLayerProjection&);
  XrResult shutdown();
  int64_t format() const { return format_; }

 private:
  struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> images;
    std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> rtvs;
    uint32_t width = 0, height = 0;
  } eyes_[2];
  XrCompositionLayerProjectionView layerViews_[2]{};
  StereoDispatch dispatch_{};
  XrSession session_ = XR_NULL_HANDLE;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> vertices_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
  int64_t format_ = 0;
  bool ready_ = false;
  XrResult lastResult_ = XR_SUCCESS;
};

} // namespace edvr::openxr
