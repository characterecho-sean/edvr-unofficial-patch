#pragma once

#include "../common/plugin_cost.h"
#include "shader_cost_sites.h"

namespace edvr { namespace panel_distance_cost {

// Direct D3D calls in the OnFootPanel distance-constant-buffer transaction.
enum class Site : uint16_t {
    Map = 122,
    Unmap = 123,
    OverrideVSSetCB = 124,
    RestoreVSSetCB = 125,
};

constexpr uint16_t id(Site site) noexcept { return static_cast<uint16_t>(site); }

constexpr plugin_cost::Owner owner = plugin_cost::Owner::OnFootPanel;

static_assert(id(Site::Map) == 122, "PanelDistance Map site ID is stable.");
static_assert(id(Site::Unmap) == 123, "PanelDistance Unmap site ID is stable.");
static_assert(id(Site::OverrideVSSetCB) == 124,
              "PanelDistance override VSSetConstantBuffers site ID is stable.");
static_assert(id(Site::RestoreVSSetCB) == 125,
              "PanelDistance restore VSSetConstantBuffers site ID is stable.");
static_assert(id(Site::Map) < id(Site::Unmap) &&
              id(Site::Unmap) < id(Site::OverrideVSSetCB) &&
              id(Site::OverrideVSSetCB) < id(Site::RestoreVSSetCB),
              "PanelDistance site IDs must not overlap or reorder.");
static_assert(id(Site::Map) > shader_cost::id(shader_cost::Site::CreatePsShader) &&
              id(Site::RestoreVSSetCB) <= plugin_cost::kMaxApiSiteId &&
              plugin_cost::kMaxApiSiteId < 4 * 64,
              "PanelDistance sites must follow shader IDs and fit the fixed V2 API mask.");

}} // namespace edvr::panel_distance_cost
