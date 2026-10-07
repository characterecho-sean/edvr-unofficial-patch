#pragma once

#include "../common/plugin_cost.h"

namespace edvr { namespace shader_cost {

// Bounded to precompiled shaderSwapCreate* attempts: GetDevice is ReadQuery,
// Create*Shader is Work. Releases and runtime D3DCompile paths are excluded.

enum class Site : uint16_t {
    CreateVsGetDevice = 116,
    CreateVsShader = 117,
    CreateCsGetDevice = 118,
    CreateCsShader = 119,
    CreatePsGetDevice = 120,
    CreatePsShader = 121,
};

constexpr uint16_t id(Site site) noexcept { return static_cast<uint16_t>(site); }

inline void note(Site site, plugin_cost::ApiClass apiClass) noexcept {
    edvrPluginCostNoteD3dCall(static_cast<uint8_t>(plugin_cost::Owner::Core), id(site),
                              static_cast<uint8_t>(apiClass));
}

static_assert(id(Site::CreateVsGetDevice) == 116 && id(Site::CreateVsShader) == 117 &&
              id(Site::CreateCsGetDevice) == 118 && id(Site::CreateCsShader) == 119 &&
              id(Site::CreatePsGetDevice) == 120 && id(Site::CreatePsShader) == 121,
              "Precompiled shader creation site IDs are stable.");
static_assert(id(Site::CreatePsShader) <= plugin_cost::kMaxApiSiteId &&
              plugin_cost::kMaxApiSiteId < 4 * 64,
              "Precompiled shader sites must fit the fixed four-word V2 API mask.");

}} // namespace edvr::shader_cost
