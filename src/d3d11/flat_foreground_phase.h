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
    for (unsigned r = 0; r < 4; ++r)
        for (float value : rows[r]) if (!std::isfinite(value)) return false;
    return std::isfinite(expectedNear) && expectedNear > 0 &&
        rows[0][2] == 0 && rows[1][2] == 0 && rows[2][2] == 0 &&
        rows[3][2] == expectedNear &&
        flatProjectionJitter(pixelX, pixelY, width, height, expected) &&
        flatCameraMeasureRowShift(rows, actualX, actualY) &&
        std::abs(actualX - expected.ndcX) <= kFlatCameraPairTolerance &&
        std::abs(actualY - expected.ndcY) <= kFlatCameraPairTolerance;
}

inline bool flatForegroundProjectionRowsCarryPhase(const float (&projectionRows)[4][4],
                                     FlatProjectionPatchLayout layout,
                                     float pixelX, float pixelY,
                                     uint32_t width, uint32_t height,
                                     float expectedNear) {
    float rows[6][4]{};
    if (layout == FlatProjectionPatchLayout::ForwardColumns)
        std::memcpy(rows, projectionRows, sizeof(projectionRows));
    else if (layout == FlatProjectionPatchLayout::ForwardDp4) {
        for (unsigned r = 0; r < 4; ++r)
            for (unsigned c = 0; c < 4; ++c) rows[r][c] = projectionRows[c][r];
    } else return false;
    return flatForegroundRowsCarryPhase(rows, pixelX, pixelY, width, height, expectedNear);
}

// The shader recipe must establish that these are its actual four clip rows;
// DP4 additionally needs homogeneous source.w == 1 for constant clip Z.
inline bool flatForegroundBoundProjectionPhase(ID3D11DeviceContext* ctx,
                                     FlatProjectionRuntime* projection,
                                     UINT slot, UINT row,
                                     FlatProjectionPatchLayout layout,
                                     float pixelX, float pixelY,
                                     uint32_t width, uint32_t height,
                                     float expectedNear,
                                     const char** reason = nullptr) {
    auto refuse = [&](const char* why) { if (reason) *reason = why; return false; };
    if (reason) *reason = nullptr;
    if (!ctx || !projection || slot >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT || row > 4092)
        return refuse("foreign phase missing CPU witness or recipe range");
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ctx1;
    UINT first = 0, count = 4096;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1))))
        ctx1->VSGetConstantBuffers1(slot, 1, &buffer, &first, &count);
    else ctx->VSGetConstantBuffers(slot, 1, &buffer);
    if (!buffer || count < row + 4) return refuse("foreign phase short bound projection");
    const uint64_t offset = (uint64_t(first) + row) * 16;
    D3D11_BUFFER_DESC desc{};
    buffer->GetDesc(&desc);
    if (offset + 64 > desc.ByteWidth) return refuse("foreign phase projection range outside buffer");
    float rows[4][4]{};
    if (!projection->copyConstants(buffer.Get(), uint32_t(offset), 64, rows))
        return refuse("foreign phase missing bound projection publication");
    if (!flatForegroundProjectionRowsCarryPhase(rows, layout, pixelX, pixelY, width, height, expectedNear))
        return refuse("foreign phase or near differs from bound projection");
    return true;
}

inline bool flatForegroundBoundPhase(ID3D11DeviceContext* ctx,
                                     FlatProjectionRuntime* projection,
                                     float pixelX, float pixelY,
                                     uint32_t width, uint32_t height,
                                     float expectedNear,
                                     const char** reason = nullptr) {
    return flatForegroundBoundProjectionPhase(ctx, projection, 1, 270,
        FlatProjectionPatchLayout::ForwardColumns, pixelX, pixelY, width, height, expectedNear, reason);
}

} // namespace edvr
