#pragma once
#include <d3d11.h>

namespace edvr::openxr {
// Optional measurement seam, borrowed only for this synchronous operation.
// 0/1 are left/right consumer copies; 2/3 are left/right composition lists.
// Implementations must be noexcept and must never alter rendering results.
struct GpuWorkObserver {
    virtual ~GpuWorkObserver() = default;
    virtual void beginGpuWork(unsigned phase, ID3D11DeviceContext*) noexcept = 0;
    virtual void endGpuWork(unsigned phase, ID3D11DeviceContext*) noexcept = 0;
};
}
