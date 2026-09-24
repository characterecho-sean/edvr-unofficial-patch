#pragma once
#include <cstdint>
#include <d3d11.h>
#include "flat_mono_frame.h"

namespace edvr {
// All getters and copies are limited to two manually armed candidate frames.
void flatComputeArm(ID3D11Device* device, uint64_t present);
bool flatComputeCandidate() noexcept;
bool flatComputeManual() noexcept;
void flatComputePoll(uint64_t present);
void flatComputeBoundary(uint64_t present, uint64_t nextEpoch);
void flatComputeFinish(uint64_t present, const FlatMonoFrame& mono);
void flatComputeDispatch(ID3D11DeviceContext* ctx, uint64_t epoch, uint32_t sequence,
                         UINT x, UINT y, UINT z, ID3D11Buffer* args, UINT offset);
void flatComputeDraw(ID3D11DeviceContext* ctx, uint64_t epoch, uint32_t sequence,
                     uint64_t vs, uint64_t ps, const void* depth, const void* color);
// CPU shadow ownership remains in flat_temporal; a false result has no bytes.
bool flatTemporalCopyConstants(ID3D11Buffer* buffer, uint32_t offset, uint32_t bytes,
                              void* out, uint32_t& width, uint64_t& epoch, uint32_t& sequence);
}
