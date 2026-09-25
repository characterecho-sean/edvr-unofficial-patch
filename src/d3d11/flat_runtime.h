#pragma once
#include <atomic>
#include <d3d11.h>
#include <dxgi.h>
#include "flat_compute_readback.h"
#include "flat_projection_scope.h"
#include <optional>
namespace edvr {
extern std::atomic<bool> g_flatRuntimeLive;
inline bool flatRuntimeActive() { return g_flatRuntimeLive.load(std::memory_order_relaxed) && !g_flatComputeInternal; }
// Last qualified flat scene extent, published for the menu on any thread.
bool flatRuntimeNativeScale();
void flatRuntimePresent(IDXGISwapChain*, uint64_t frame, HRESULT, UINT flags);
void flatRuntimeBeforePresent();
void flatRuntimeResize();
void flatRuntimeConstantBuffers(UINT start, UINT count, ID3D11Buffer* const*);
void flatRuntimeUavs(UINT start, UINT count, ID3D11UnorderedAccessView* const*);
struct FlatRuntimeDispatchScope {
    std::optional<FlatProjectionBindingScope> projection;
    explicit FlatRuntimeDispatchScope(ID3D11DeviceContext*);
};
void flatRuntimeViewport(UINT, const D3D11_VIEWPORT*);
void flatRuntimeMap(ID3D11Resource*, D3D11_MAP, void*);
void flatRuntimeUnmap(ID3D11Resource*);
void flatRuntimeUpdate(ID3D11Resource*, const void*, const D3D11_BOX*);
void flatRuntimeWritten(ID3D11Resource*);
void flatRuntimeUnknown();
void flatRuntimeArmProjectionAudit();
void flatRuntimeCreateBuffer(ID3D11Buffer*, const void* initialData);
void flatRuntimeClearBindings();
struct FlatRuntimeDrawScope {
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* targets[8]{};
    ID3D11DepthStencilView* depth = nullptr;
    ID3D11ShaderResourceView* original = nullptr;
    bool producer = false, replaced = false;
    bool drawCaptureStarted = false;
    std::optional<FlatProjectionBindingScope> projection;
    FlatRuntimeDrawScope(ID3D11DeviceContext*, uint32_t instances,
                         char kind='?', uint32_t count=0, uint32_t start=0,
                         int32_t base=0, uint32_t startInstance=0);
    ~FlatRuntimeDrawScope();
    bool recover(const char* reason);
};
} // namespace edvr
