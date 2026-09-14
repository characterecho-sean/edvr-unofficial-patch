#pragma once
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

#define EDVR_NATIVE_SHARPEN_VERSION_1 1u

struct EdvrNativeSharpenRequest {
  uint32_t size, version;
  ID3D11Device* gameDevice;
  uint64_t generation;
};
// Acquire and treatEye run on the bound game producer thread. invalidate and
// close are CPU-only and may run on the XR owner. The host retains gameDevice
// and the module until close returns; the provider owns no device reference.
// A valid nonzero sequence advances the latch once, consuming each eye at
// most once. invalidate preserves the sequence floor and rejects that value;
// a greater sequence recovers. S_OK returns an AddRef'd output and full-span
// bounds; S_FALSE consumes the eye as passthrough; failures clear outputs.
typedef HRESULT(WINAPI *EdvrNativeSharpenTreat)(void*, uint64_t, uint32_t,
    ID3D11Texture2D*, const float*, ID3D11Texture2D**, float*);
typedef HRESULT(WINAPI *EdvrNativeSharpenInvalidate)(void*);
typedef HRESULT(WINAPI *EdvrNativeSharpenClose)(void*);
struct EdvrNativeSharpenTable {
  uint32_t size, version;
  void* context;
  EdvrNativeSharpenTreat treatEye;
  EdvrNativeSharpenInvalidate invalidate;
  EdvrNativeSharpenClose close;
};
extern "C" HRESULT WINAPI edvrAcquireNativeSharpen(const EdvrNativeSharpenRequest*, EdvrNativeSharpenTable*);
namespace edvr { bool nativeSharpenActive(); }
