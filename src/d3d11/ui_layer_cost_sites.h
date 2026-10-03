#pragma once

#include "../common/plugin_cost.h"

namespace edvr { namespace ui_layer_cost {

// Core-owned direct state-save/apply/restore calls for uiLayerBegin/End.
// Shader remap, depth seeding, clears, and blend-cache construction remain
// outside this bounded state transaction slice. OM/RS raw setter entries are
// counted at the UI layer's forwarding boundary; the live registered context
// route has those vScreen raw entries wired to the original D3D11 methods.
enum class Site : uint16_t {
    GetBlendState = 83,
    GetRenderTargets = 84,
    GetViewports = 85,
    GetRasterizerState = 86,
    RasterizerGetDesc = 87,
    GetScissorRects = 88,
    ApplyViewports = 89,
    ApplyScissorRects = 90,
    ApplyBlendState = 91,
    ApplyRenderTargets = 92,
    RestoreRenderTargets = 93,
    RestoreViewports = 94,
    RestoreScissorRects = 95,
    RestoreBlendState = 96,
};

constexpr uint16_t id(Site site) noexcept { return static_cast<uint16_t>(site); }

inline void note(bool sampled, Site site, plugin_cost::ApiClass apiClass) noexcept {
    if (sampled) {
        edvrPluginCostNoteD3dCall(static_cast<uint8_t>(plugin_cost::Owner::Core), id(site),
                                  static_cast<uint8_t>(apiClass));
    }
}

static_assert(id(Site::GetBlendState) == 83 && id(Site::RestoreBlendState) == 96,
              "UI-layer state transaction API site IDs are stable.");
static_assert(id(Site::RestoreBlendState) <= plugin_cost::kMaxSiteId &&
              plugin_cost::kMaxSiteId < 2 * 64,
              "UI-layer sites must fit the fixed two-word coverage mask.");

}} // namespace edvr::ui_layer_cost
