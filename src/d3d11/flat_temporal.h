// Flat Elite scene discovery. No temporal treatment is admitted until a live
// desktop capture proves camera, depth, scene completion and output ownership.
#pragma once

#include <atomic>
#include <cstdint>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "flat_temporal_model.h"
#include "flat_compute_readback.h"

namespace edvr {
namespace detail {
extern std::atomic<bool> g_flatTemporalCapturing;
extern std::atomic<DWORD> g_flatTemporalOwnerThread;
extern std::atomic<uint64_t> g_flatTemporalForeignCalls;
}
inline bool flatTemporalCapturing() {
    if (!detail::g_flatTemporalCapturing.load(std::memory_order_relaxed)) return false;
    if (g_flatComputeInternal) return false;
    const DWORD owner = detail::g_flatTemporalOwnerThread.load(std::memory_order_acquire);
    if (flatCaptureThreadEligible(true, owner, GetCurrentThreadId())) return true;
    detail::g_flatTemporalForeignCalls.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void flatTemporalStart(ID3D11Device* device);
void flatTemporalArm();  // existing dump_draws hotkey starts a fresh flat window
void flatTemporalStop();
void flatTemporalBeforePresent(IDXGISwapChain* swap, uint64_t frame, UINT flags);
void flatTemporalAfterPresent(uint64_t frame, HRESULT result, UINT flags);
void flatTemporalBind(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv);
void flatTemporalViewport(UINT count, const D3D11_VIEWPORT* viewports);
void flatTemporalConstantBuffers(bool pixelStage, UINT start, UINT count, ID3D11Buffer* const* buffers);
void flatTemporalClearBindings();  // an observed ClearState, explicitly unbound
void flatTemporalDraw(ID3D11DeviceContext* ctx, uint32_t count, uint32_t instances);
void flatTemporalClearColor(ID3D11RenderTargetView* rtv);
void flatTemporalClearUav(ID3D11UnorderedAccessView* uav);
void flatTemporalClearDepth(ID3D11DepthStencilView* dsv, UINT flags, float depth);
void flatTemporalTransfer(ID3D11Resource* dst, ID3D11Resource* src, char kind);
void flatTemporalDispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z, ID3D11Buffer* args = nullptr, UINT offset = 0);
void flatTemporalExecuteList(bool foreign);
void flatTemporalMap(ID3D11Resource* res, UINT sub, D3D11_MAP type, void* data);
void flatTemporalUnmap(ID3D11Resource* res);
void flatTemporalUpdate(ID3D11Resource* dst, const void* data, const D3D11_BOX* box);
}  // namespace edvr
