#pragma once

#include "../common/plugin_cost.h"

namespace edvr { namespace loader_panel_cost {

// Intro-owned, direct staging capture/readback calls. Site IDs 91-110 follow
// SplashDim's Intro IDs 83-90; all sites stay within Intro's own mask.
enum class Site : uint16_t {
    IaGetIndexBuffer = 91,
    IaGetVertexBuffers = 92,
    ContextGetDevice = 93,
    CreateIndexStage = 94,
    VertexGetDesc = 95,
    CreateVertexStage = 96,
    CopyVertexStage = 97,
    CopyIndexStage = 98,
    VsGetShaderResources = 99,
    SrvGetResource = 100,
    ResourceGetType = 101,
    TableGetDesc = 102,
    SrvGetDesc = 103,
    CreateTableStage = 104,
    CopyTableStage = 105,
    VsGetConstantBuffers = 106,
    CreateConstantsStage = 107,
    CopyConstantsStage = 108,
    MapReadback = 109,
    UnmapReadback = 110,
};

constexpr uint16_t id(Site site) noexcept { return static_cast<uint16_t>(site); }

inline void note(Site site, plugin_cost::ApiClass apiClass) noexcept {
    edvrPluginCostNoteD3dCall(static_cast<uint8_t>(plugin_cost::Owner::Intro), id(site),
                              static_cast<uint8_t>(apiClass));
}

static_assert(id(Site::IaGetIndexBuffer) == 91 && id(Site::UnmapReadback) == 110,
              "Loader-panel API site IDs are stable.");
static_assert(id(Site::UnmapReadback) <= plugin_cost::kMaxApiSiteId &&
              plugin_cost::kMaxApiSiteId < 4 * 64,
              "Loader-panel sites must fit the fixed four-word V2 API mask.");

}} // namespace edvr::loader_panel_cost
