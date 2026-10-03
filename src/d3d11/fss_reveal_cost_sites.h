#pragma once

#include "../common/plugin_cost.h"

namespace edvr { namespace fss_reveal_cost {

// Direct D3D11 calls made by FSS Reveal's Begin/End path. IDs 53-73 follow
// the existing source-site ledger and fit the collector's fixed site mask.
enum class Site : uint16_t {
    LearnGetPsCb = 53,
    LearnBufferGetDesc = 54,
    CaptureGetPsSrvs = 55,
    CaptureContextGetDevice = 56,
    CaptureSrvGetResource = 57,
    CaptureQueryTexture2D = 58,
    CaptureTextureGetDesc = 59,
    CaptureSrvGetDesc = 60,
    CreateTexture2D = 61,
    CreateShaderResourceView = 62,
    CopyResource = 63,
    ApplyGetPsSrvs = 64,
    ApplySetPsSrvs = 65,
    ApplyContextGetDevice = 66,
    CreateSceneBuffer = 67,
    MapSceneBuffer = 68,
    UnmapSceneBuffer = 69,
    ApplyGetPsCb = 70,
    ApplySetPsCb = 71,
    RestorePsSrvs = 72,
    RestorePsCb = 73,
};

constexpr uint16_t id(Site site) noexcept {
    return static_cast<uint16_t>(site);
}

inline void note(Site site, plugin_cost::ApiClass apiClass) noexcept {
    edvrPluginCostNoteD3dCall(
        static_cast<uint8_t>(plugin_cost::Owner::Scanners), id(site),
        static_cast<uint8_t>(apiClass));
}

static_assert(id(Site::LearnGetPsCb) == 53 &&
                  id(Site::RestorePsCb) == 73,
              "FSS Reveal API site IDs are stable.");
static_assert(id(Site::RestorePsCb) <= plugin_cost::kMaxSiteId &&
                  plugin_cost::kMaxSiteId < 2 * 64,
              "FSS Reveal sites must fit the fixed two-word coverage mask.");

}}  // namespace edvr::fss_reveal_cost
