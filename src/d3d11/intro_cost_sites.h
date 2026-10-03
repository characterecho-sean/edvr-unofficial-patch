#pragma once

#include "../common/plugin_cost.h"

namespace edvr { namespace intro_cost {

// Direct SplashDim API calls. Shared binding resolver queries stay attributed
// to Core's sites 112-115, and shaderSwapCreatePs owns its helper internals.
enum class Site : uint16_t {
    SplashPsGetShader = 83,
    SplashOmGetBlendState = 84,
    SplashPsSetShaderApply = 85,
    SplashOmSetBlendStateApply = 86,
    SplashPsSetShaderRestore = 87,
    SplashOmSetBlendStateRestore = 88,
    SplashBlendGetDevice = 89,
    SplashCreateBlendState = 90,
};

constexpr uint16_t id(Site site) noexcept { return static_cast<uint16_t>(site); }

inline void note(Site site, plugin_cost::ApiClass apiClass) noexcept {
    edvrPluginCostNoteD3dCall(static_cast<uint8_t>(plugin_cost::Owner::Intro),
                              id(site), static_cast<uint8_t>(apiClass));
}

static_assert(id(Site::SplashPsGetShader) == 83 &&
              id(Site::SplashBlendGetDevice) == 89 &&
              id(Site::SplashCreateBlendState) == 90,
              "SplashDim API site IDs are stable.");
static_assert(id(Site::SplashCreateBlendState) <= plugin_cost::kMaxSiteId,
              "SplashDim API sites must fit the fixed two-word coverage mask.");

}} // namespace edvr::intro_cost
