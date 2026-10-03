#pragma once

#include "../common/plugin_cost.h"

namespace edvr { namespace binding_cost {

enum class Site : uint16_t {
    GetResource = 112,
    GetType = 113,
    BufferGetDesc = 114,
    Texture2DGetDesc = 115,
};

constexpr uint16_t id(Site site) noexcept { return static_cast<uint16_t>(site); }

inline void note(Site site) noexcept {
    edvrPluginCostNoteD3dCall(static_cast<uint8_t>(plugin_cost::Owner::Core), id(site),
                              static_cast<uint8_t>(plugin_cost::ApiClass::ReadQuery));
}

static_assert(id(Site::GetResource) <= plugin_cost::kMaxSiteId &&
              id(Site::Texture2DGetDesc) <= plugin_cost::kMaxSiteId,
              "Binding resolver sites must fit the fixed two-word coverage mask.");
static_assert(id(Site::GetResource) == 112 && id(Site::GetType) == 113 &&
              id(Site::BufferGetDesc) == 114 && id(Site::Texture2DGetDesc) == 115,
              "Binding resolver site IDs are stable.");
static_assert(plugin_cost::kMaxSiteId < 2 * 64,
              "Binding resolver site coverage requires exactly two 64-bit words.");

}} // namespace edvr::binding_cost
