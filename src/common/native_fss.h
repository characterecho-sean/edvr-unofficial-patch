#pragma once
#include <windows.h>
#include <d3d11.h>
#include <cstdint>

#define EDVR_NATIVE_FSS_VERSION_1 1u
struct EdvrNativeFssRequest { uint32_t size, version; ID3D11Device* gameDevice; uint64_t generation; };
// CPU-only frame snapshot. frusta are signed left,right,down,up tangents;
// eyeToHead is the runtime's rigid row-major 3x4 eye-to-head transform.
// Acquire/treat run on the producer; begin/invalidate/close are CPU-only.
// A successful heal returns an AddRef'd texture covering the cropped eye ROI
// with full positive bounds. S_FALSE leaves output null and input unchanged.
// The host retains the bound device/module until close. Invalidate preserves
// the sequence floor; old/duplicate eyes cannot reuse retired state.
struct EdvrNativeFssFrame {
  uint32_t size, version; uint64_t generation, referenceGeneration, sequence;
  float frusta[2][4]; float eyeToHead[2][12];
};
typedef HRESULT(WINAPI* EdvrNativeFssBeginFrame)(void*, const EdvrNativeFssFrame*);
typedef HRESULT(WINAPI* EdvrNativeFssTreatEye)(void*, uint64_t, uint32_t, ID3D11Texture2D*, const float*, ID3D11Texture2D**);
typedef HRESULT(WINAPI* EdvrNativeFssInvalidate)(void*);
typedef HRESULT(WINAPI* EdvrNativeFssClose)(void*);
struct EdvrNativeFssTable {
  uint32_t size, version; void* context;
  EdvrNativeFssBeginFrame beginFrame; EdvrNativeFssTreatEye treatEye;
  EdvrNativeFssInvalidate invalidate; EdvrNativeFssClose close;
};
extern "C" HRESULT WINAPI edvrAcquireNativeFss(const EdvrNativeFssRequest*, EdvrNativeFssTable*);
