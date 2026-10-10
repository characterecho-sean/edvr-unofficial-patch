#pragma once

#include "../../common/plugin_cost.h"

namespace edvr { namespace weapon_motion_cost {

enum class Site : uint16_t {
    TargetRead = 136,
    BlendRead = 137,
    PixelShaderRead = 138,
    VertexShaderRead = 139,
    PixelConstantBufferRead = 140,
    VertexConstantBufferRead = 141,
    VertexResourcesRead = 142,
    SettingsWrite = 143,
    MotionTargetBind = 144,
    DepthStateBind = 145,
    BlendStateBind = 146,
    MotionVertexShaderBind = 147,
    MotionVertexResourcesBind = 148,
    MotionVertexConstantBufferBind = 149,
    MotionPixelShaderBind = 150,
    MotionPixelConstantBufferBind = 151,
    SequentialLayoutBind = 152,
    SequentialIndexBufferBind = 153,
    RasterDraw = 154,
    VertexResourcesClear = 155,
    HostLayoutRestore = 156,
    HostIndexBufferRestore = 157,
    HostVertexShaderRestore = 158,
    HostVertexResourcesRestore = 159,
    HostVertexConstantBufferRestore = 160,
    HostPixelShaderRestore = 161,
    HostPixelConstantBufferRestore = 162,
    HostTargetRestore = 163,
    HostDepthStateRestore = 164,
    HostBlendStateRestore = 165,
};

constexpr uint16_t id(Site site) noexcept { return static_cast<uint16_t>(site); }

template<Site S, plugin_cost::ApiClass Class>
inline void note() noexcept {
    plugin_cost::SampledApi<>::template note<plugin_cost::Owner::OnFootPanel, id(S), Class>();
}

static_assert(id(Site::TargetRead) == 136 && id(Site::BlendRead) == 137 &&
              id(Site::PixelShaderRead) == 138 && id(Site::VertexShaderRead) == 139 &&
              id(Site::PixelConstantBufferRead) == 140 && id(Site::VertexConstantBufferRead) == 141 &&
              id(Site::VertexResourcesRead) == 142 && id(Site::SettingsWrite) == 143 &&
              id(Site::MotionTargetBind) == 144 && id(Site::DepthStateBind) == 145 &&
              id(Site::BlendStateBind) == 146 && id(Site::MotionVertexShaderBind) == 147 &&
              id(Site::MotionVertexResourcesBind) == 148 && id(Site::MotionVertexConstantBufferBind) == 149 &&
              id(Site::MotionPixelShaderBind) == 150 && id(Site::MotionPixelConstantBufferBind) == 151 &&
              id(Site::SequentialLayoutBind) == 152 && id(Site::SequentialIndexBufferBind) == 153 &&
              id(Site::RasterDraw) == 154 && id(Site::VertexResourcesClear) == 155 &&
              id(Site::HostLayoutRestore) == 156 && id(Site::HostIndexBufferRestore) == 157 &&
              id(Site::HostVertexShaderRestore) == 158 && id(Site::HostVertexResourcesRestore) == 159 &&
              id(Site::HostVertexConstantBufferRestore) == 160 && id(Site::HostPixelShaderRestore) == 161 &&
              id(Site::HostPixelConstantBufferRestore) == 162 && id(Site::HostTargetRestore) == 163 &&
              id(Site::HostDepthStateRestore) == 164 && id(Site::HostBlendStateRestore) == 165,
              "Weapon-motion API site IDs are stable.");
static_assert(id(Site::TargetRead) > plugin_cost::kMaxSiteId &&
              id(Site::HostBlendStateRestore) <= plugin_cost::kMaxApiSiteId,
              "Weapon-motion API sites fit the API-only V2 coverage mask.");

}} // namespace edvr::weapon_motion_cost
