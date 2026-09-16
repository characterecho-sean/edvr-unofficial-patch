#pragma once
#include <windows.h>
#include <d3d11.h>
#include <cstdint>

#define EDVR_NATIVE_FSS_VERSION_2 2u
struct EdvrNativeFssRequest { uint32_t size, version; ID3D11Device* gameDevice; uint64_t generation; };
// CPU-only frame snapshot. frusta are signed left,right,down,up tangents;
// eyeToHead is the runtime's rigid row-major 3x4 eye-to-head transform.
// Acquire/treat run on the producer; begin/invalidate/close are CPU-only.
// treatEye snapshots the eye's ROI and returns S_FALSE with output null
// (never S_OK); E_INVALIDARG on a bad call. healPair, called after each
// treatEye of the frame:
//   E_PENDING: the heal's target eye is snapshotted and the donor eye has
//   not been treated yet this frame; the host defers the target's later
//   stages until the donor's treat.
//   S_OK: *eye (0/1) and *out (AddRef'd, full positive bounds, the
//   target's cropped ROI) are set; delivered once per frame.
//   S_FALSE: no heal this frame (gate closed, target not yet snapshotted,
//   donor treated but unusable, incompatible pair, shader refusal, or
//   already delivered).
//   E_INVALIDARG: no frame open, wrong thread, sequence mismatch, null
//   out-params.
// The host retains the bound device/module until close. Invalidate preserves
// the sequence floor; old/duplicate eyes cannot reuse retired state.
struct EdvrNativeFssFrame {
  uint32_t size, version; uint64_t generation, referenceGeneration, sequence;
  float frusta[2][4]; float eyeToHead[2][12];
};
typedef HRESULT(WINAPI* EdvrNativeFssBeginFrame)(void*, const EdvrNativeFssFrame*);
typedef HRESULT(WINAPI* EdvrNativeFssTreatEye)(void*, uint64_t, uint32_t, ID3D11Texture2D*, const float*, ID3D11Texture2D**);
typedef HRESULT(WINAPI* EdvrNativeFssHealPair)(void*, uint64_t sequence, uint32_t* eye, ID3D11Texture2D** out);
typedef HRESULT(WINAPI* EdvrNativeFssInvalidate)(void*);
typedef HRESULT(WINAPI* EdvrNativeFssClose)(void*);
struct EdvrNativeFssTable {
  uint32_t size, version; void* context;
  EdvrNativeFssBeginFrame beginFrame; EdvrNativeFssTreatEye treatEye; EdvrNativeFssHealPair healPair;
  EdvrNativeFssInvalidate invalidate; EdvrNativeFssClose close;
};
extern "C" HRESULT WINAPI edvrAcquireNativeFss(const EdvrNativeFssRequest*, EdvrNativeFssTable*);
