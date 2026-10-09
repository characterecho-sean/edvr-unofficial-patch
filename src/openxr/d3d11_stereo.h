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
#include "eye_capture.h"
#include "skybox_capture.h"
#include "graphics_bridge_client.h"
#include "immediate_executor.h"
#include "gpu_work_observer.h"
#include <vector>

namespace edvr::openxr {

// Where a treated eye's pixels belong inside the swapchain image, as
// fractions of it, v running down from the top. The default is the whole
// image, which is what every path that does not narrow the game's own
// projection hands over. A narrower one leaves the rest of the image black:
// the layer still advertises the runtime's full field, because a runtime
// that ignores a narrower layer FOV (the Oculus one does) would stretch a
// trimmed image across the lens instead. Deliberately not the trim's own
// bounds type -- this header stays free of the native feature headers.
struct StereoPlacement { float left = 0, top = 0, right = 1, bottom = 1; };

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
  // XR renderer: records private command lists unless an exclusively owned
  // immediate scene context is explicitly selected. Not a game-device pass.
  // Borrowed session/dispatch must remain valid until shutdown/destruction.
  D3D11Stereo() = default;
  ~D3D11Stereo();
  D3D11Stereo(const D3D11Stereo&) = delete;
  D3D11Stereo& operator=(const D3D11Stereo&) = delete;
  D3D11Stereo(D3D11Stereo&&) = delete;
  D3D11Stereo& operator=(D3D11Stereo&&) = delete;

  XrResult initialize(const StereoDispatch&, XrSession, ID3D11Device*,
                      const XrViewConfigurationView (&)[2], HMODULE graphicsProvider = nullptr,
                      ImmediateExecutor* immediateExecutor = nullptr,
                      bool ownedImmediateScene = false);
  // ownedImmediateScene is an explicit promise of exclusive owner-thread
  // access to a separate XR device. It cannot accompany a provider/executor.
  // Only the steady-state captured-eye draw bypasses deferred command lists;
  // borrowed-context state preservation remains unchanged.
  // An explicit provider requires the paired private-submission capability.
  // Omission is for a standalone diagnostic device only. Neither mode grants
  // ownership of a game's context or permits background game-device access.
  // A supplied executor outlives this renderer's shutdown and synchronously
  // runs immediate-context work at the host's exclusive render boundary.
  // XR calls and private deferred recording stay on the renderer owner.
  // placement, when given, points at one entry per eye. Absent, or the whole
  // image, is the path this renderer has always taken.
  // fade, 0..1 (Explorer Cam's comfort fade, comfort_fade.h), blends black over
  // each finished eye image by that much: out = in * (1 - fade). Exactly 0 (and
  // anything that is not a number or is negative) runs no extra pass at all, so
  // the image is the one this renderer has always drawn, bit for bit; 1 or more
  // is plain black. The same level applies on a replayed pair, because it is an
  // argument of each draw, not a property of the kept pixels.
  XrResult renderCaptured(const XrView (&)[2], XrSpace, const EyeCapture&,
                          XrCompositionLayerProjection&, GpuWorkObserver* observer = nullptr,
                          StereoWallTimes* times = nullptr,
                          const StereoPlacement* placement = nullptr,
                          float fade = 0.0f);
  XrResult renderSkybox(const XrView (&)[2], XrSpace, const SkyboxCapture&,
                        XrCompositionLayerProjection&, float fade = 0.0f);
  // Complete submitted GPU work before retiring the render caller. Uses its
  // immediate-context boundary, without changing pipeline state. Timeout or
  // unavailable admission leaves resources alive for an explicit retry.
  XrResult drain();
  bool needsGpuDrain() const { return gpuPending_; }
  // Must finish before the host destroys the session/device/executor. A host
  // using an executor drains on its render caller before closing admission.
  // Destroying this object with uncompleted GPU work is a contract violation.
  XrResult shutdown();
  int64_t format() const { return format_; }
  // Wall time the last initialize spent creating the two swapchains (format
  // enumeration through the image views) and creating the runtime shaders
  // (the four shader objects, from the build's bytecode, and the blit's
  // buffer and sampler; it held two D3DCompile pairs until 2026-10-01). Measured with
  // std::chrono::steady_clock around each stretch, for the host's startup
  // trace; 0 when initialize has not run or the clock was unavailable.
  double initSwapchainMs() const { return initSwapchainMs_; }
  double initShaderMs() const { return initShaderMs_; }

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
  // The deferred context records the borrowed-device passes.
  // Captured scenes can draw directly only on an explicitly owned context;
  // other paths execute command lists and preserve caller state. Both modes
  // require serialized context access.
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediateContext_;
  GraphicsBridgeClient graphicsBridge_;
  ImmediateExecutor* immediateExecutor_ = nullptr;
  bool ownedImmediateScene_ = false;
  DWORD sceneOwnerThread_ = 0;
  Microsoft::WRL::ComPtr<ID3D11Query> completion_;
  bool gpuPending_ = false;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
  // dest * (1 - blend factor) on red, green and blue; alpha untouched. The comfort fade's one pass.
  Microsoft::WRL::ComPtr<ID3D11BlendState> fadeBlend_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> blitVertexShader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> blitPixelShader_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> blitConstants_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> blitSampler_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> skyboxVertexShader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> skyboxPixelShader_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> skyboxConstants_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> skyboxSampler_;
  int64_t format_ = 0;
  bool ready_ = false;
  XrResult lastResult_ = XR_SUCCESS;
  double initSwapchainMs_ = 0.0, initShaderMs_ = 0.0;

  XrResult submitCommands(GpuWorkObserver* observer = nullptr, unsigned phase = 0);
};

} // namespace edvr::openxr
