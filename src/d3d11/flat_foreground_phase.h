#pragma once
#include "flat_camera_phase.h"
#include "flat_projection_runtime.h"
#include <d3d11_1.h>
#include <wrl/client.h>

namespace edvr {

// This admission is deliberately for centered projection families. An unknown
// native off-center projection cannot establish the injected phase from one
// publication. Rows must come from the currently bound, complete CPU witness.
inline bool flatForegroundRowsCarryPhase(const float (&rows)[6][4],
                                         float pixelX, float pixelY,
                                         uint32_t width, uint32_t height,
                                         float expectedNear) {
    FlatProjectionJitter expected{};
    double actualX = 0, actualY = 0;
    return std::isfinite(expectedNear) && expectedNear > 0 &&
        rows[3][2] == expectedNear &&
        flatProjectionJitter(pixelX, pixelY, width, height, expected) &&
        flatCameraMeasureRowShift(rows, actualX, actualY) &&
        std::abs(actualX - expected.ndcX) <= kFlatCameraPairTolerance &&
        std::abs(actualY - expected.ndcY) <= kFlatCameraPairTolerance;
}

inline bool flatForegroundBoundPhase(ID3D11DeviceContext* ctx,
                                     FlatProjectionRuntime* projection,
                                     float pixelX, float pixelY,
                                     uint32_t width, uint32_t height,
                                     float expectedNear,
                                     const char** reason = nullptr) {
    auto refuse = [&](const char* why) { if (reason) *reason = why; return false; };
    if (reason) *reason = nullptr;
    if (!ctx || !projection) return refuse("foreign phase missing CPU witness");
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ctx1;
    UINT first = 0, count = 4096;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        ctx1->VSGetConstantBuffers1(1, 1, &buffer, &first, &count);
    else ctx->VSGetConstantBuffers(1, 1, &buffer);
    if (!buffer || count < 274) return refuse("foreign phase short bound b1");
    const uint64_t offset = (uint64_t(first) + 270) * 16;
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    if (offset + 64 > desc.ByteWidth) return refuse("foreign phase b1 range outside buffer");
    float rows[6][4]{};
    if (!projection->copyConstants(buffer.Get(), uint32_t(offset), 64, rows))
        return refuse("foreign phase missing bound b1 publication");
    if (!flatForegroundRowsCarryPhase(rows, pixelX, pixelY, width, height, expectedNear))
        return refuse("foreign phase or near differs from bound b1");
    return true;
}

} // namespace edvr
