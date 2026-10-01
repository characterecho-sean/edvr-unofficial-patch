// Compile against the installed SDK's C vtable, independently of C++ thunks: the two
// slots the device hook patches for the format-support log must be the SDK's own
// CheckFormatSupport and CheckFeatureSupport, and the neighbours on either side are
// the methods a miscount would hook instead (CheckMultisampleQualityLevels, CheckCounter,
// GetPrivateData). A wrong number fails the build here, not on a user's startup.
// Same technique as tools\gpu_timing_test\context_slots.cpp for the context.
#define CINTERFACE
#define D3D11_NO_HELPERS
#include <d3d11.h>
#include <cstddef>
#include "../../src/common/d3d11_device_slots.h"

#define EDVR_SLOT(method) (offsetof(ID3D11DeviceVtbl, method) / sizeof(void*))

static_assert(EDVR_SLOT(CheckFormatSupport) == edvr::kDevSlotCheckFormatSupport,
              "CheckFormatSupport slot");
static_assert(EDVR_SLOT(CheckFeatureSupport) == edvr::kDevSlotCheckFeatureSupport,
              "CheckFeatureSupport slot");
// The neighbours, so the two above cannot each be off by one in the same direction.
static_assert(EDVR_SLOT(OpenSharedResource) == edvr::kDevSlotCheckFormatSupport - 1,
              "OpenSharedResource is the slot before CheckFormatSupport");
static_assert(EDVR_SLOT(CheckMultisampleQualityLevels) == edvr::kDevSlotCheckFormatSupport + 1,
              "CheckMultisampleQualityLevels is the slot after CheckFormatSupport");
static_assert(EDVR_SLOT(CheckCounter) == edvr::kDevSlotCheckFeatureSupport - 1,
              "CheckCounter is the slot before CheckFeatureSupport");
static_assert(EDVR_SLOT(GetPrivateData) == edvr::kDevSlotCheckFeatureSupport + 1,
              "GetPrivateData is the slot after CheckFeatureSupport");
// And two the device hook already counts in device_hook.cpp, the anchors the count runs from.
static_assert(EDVR_SLOT(CreateTexture2D) == 5, "CreateTexture2D slot");
static_assert(EDVR_SLOT(CreateSamplerState) == 23, "CreateSamplerState slot");
