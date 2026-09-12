#pragma once
#include <cstddef>
namespace edvr {
// Checked against ID3D11DeviceContextVtbl in context_slots.cpp.
constexpr size_t kGpuSlotDrawAuto = 38;
constexpr size_t kGpuSlotClearUavUint = 51;
constexpr size_t kGpuSlotClearUavFloat = 52;
constexpr size_t kGpuSlotGenerateMips = 54;
}
