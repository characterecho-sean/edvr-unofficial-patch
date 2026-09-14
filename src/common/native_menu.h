#pragma once
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

#define EDVR_NATIVE_MENU_VERSION_1 1u
// Private menu-only capability, independent of the legacy glitch consumer.
// Acquire/publish/treat run on the bound producer thread. Provider owns the
// context for its lifetime; close retires it without GPU work from any thread.
struct EdvrNativeMenuRequest {
  uint32_t size, version;
  ID3D11Device* gameDevice;
  uint64_t generation;
};
// Poses are row-major rigid 3x4 world transforms; frusta are signed tangents
// {left, right, down, up}. Last arguments: runtime and reference generations.
// All pose pointers null and reference generation zero is CPU invalidation,
// allowed on the XR owner. It closes visibility and keyboard capture.
typedef HRESULT(WINAPI *EdvrNativeMenuPublishPose)(void*, const float*, const float (*)[12], const float (*)[4], uint64_t, uint64_t);
// S_OK supplies an AddRef'd texture and full bounds retaining submitted flips.
// S_FALSE is passthrough; failure outputs are null/zero. Input bounds are UVUV.
typedef HRESULT(WINAPI *EdvrNativeMenuTreatEye)(void*, uint32_t, ID3D11Texture2D*, const float*, ID3D11Texture2D**, float*);
typedef HRESULT(WINAPI *EdvrNativeMenuClose)(void*);
struct EdvrNativeMenuTable {
  uint32_t size, version;
  void* context;
  EdvrNativeMenuPublishPose publishPose;
  EdvrNativeMenuTreatEye treatEye;
  EdvrNativeMenuClose close;
};
#ifdef __cplusplus
extern "C" {
#endif
HRESULT WINAPI edvrAcquireNativeMenu(const EdvrNativeMenuRequest*, EdvrNativeMenuTable*);
bool nativeMenuAvailable();
bool nativeMenuActive(); // graphics-local session presence, independent of pose validity
uint64_t nativeMenuRevision(); // graphics-local menu model retirement epoch
#ifdef __cplusplus
}
#endif
