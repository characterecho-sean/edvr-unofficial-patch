// Compile against the installed SDK's C vtable, independently of C++ thunks.
#define CINTERFACE
#define D3D11_NO_HELPERS
#include <d3d11.h>
#include <cstddef>
#include "../../src/common/d3d11_gpu_command_slots.h"
static_assert(offsetof(ID3D11DeviceContextVtbl, DrawAuto) / sizeof(void*) == edvr::kGpuSlotDrawAuto, "DrawAuto slot");
static_assert(offsetof(ID3D11DeviceContextVtbl, ClearUnorderedAccessViewUint) / sizeof(void*) == edvr::kGpuSlotClearUavUint, "UAV uint slot");
static_assert(offsetof(ID3D11DeviceContextVtbl, ClearUnorderedAccessViewFloat) / sizeof(void*) == edvr::kGpuSlotClearUavFloat, "UAV float slot");
static_assert(offsetof(ID3D11DeviceContextVtbl, GenerateMips) / sizeof(void*) == edvr::kGpuSlotGenerateMips, "GenerateMips slot");
static_assert(offsetof(ID3D11DeviceContextVtbl, PSSetConstantBuffers) / sizeof(void*) == 16, "slot 16 is a state bind");
