#pragma once

#include "../common/plugin_cost.h"

namespace edvr { namespace fss_dump_cost {

// Direct D3D11 calls made by the FSS dump capture and readback helpers.
// IDs 39-52 follow the previously assigned source sites and remain in the fixed
// four-word V2 API coverage mask.
enum class Site : uint16_t {
    OmGetRenderTargets = 39,
    RtvGetResource = 40,
    ResourceQueryTexture2D = 41,
    TextureGetDesc = 42,
    ContextGetDevice = 43,
    CreateStagingTexture = 44,
    CopyToStaging = 45,
    RememberedQueryTexture2D = 46,
    RememberedTextureGetDesc = 47,
    RememberedContextGetDevice = 48,
    RememberedCreateStagingTexture = 49,
    RememberedCopyToStaging = 50,
    MapReadback = 51,
    UnmapReadback = 52,
};

constexpr uint16_t id(Site site) noexcept {
    return static_cast<uint16_t>(site);
}

inline void note(Site site, plugin_cost::ApiClass apiClass) noexcept {
    edvrPluginCostNoteD3dCall(
        static_cast<uint8_t>(plugin_cost::Owner::Scanners), id(site),
        static_cast<uint8_t>(apiClass));
}

static_assert(id(Site::OmGetRenderTargets) == 39 &&
                  id(Site::UnmapReadback) == 52,
              "FSS dump API site IDs are stable.");
static_assert(id(Site::UnmapReadback) <= plugin_cost::kMaxApiSiteId &&
              plugin_cost::kMaxApiSiteId < 4 * 64,
              "FSS dump sites must fit the fixed four-word V2 API mask.");

}}  // namespace edvr::fss_dump_cost
