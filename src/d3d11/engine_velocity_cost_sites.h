#pragma once

#include "../common/plugin_cost.h"

namespace edvr { namespace engine_velocity_cost {

// The EngineVelocity target and blend state transactions that run after the
// draw-side cache gate. IDs 126–127 use the existing low mask word; IDs
// 128–135 use the API-only V2 extension. CPU-site coverage remains capped.
enum class Site : uint16_t {
    TargetSnapshotRead = 126,
    TargetCurrentRead = 127,
    TargetUavRead = 128,
    TargetApplyMrt6 = 129,
    TargetKeptReadback = 130,
    TargetRejectRollback = 131,
    TargetRestore = 132,
    BlendRead = 133,
    BlendApply = 134,
    BlendRestore = 135,
};

constexpr uint16_t id(Site site) noexcept { return static_cast<uint16_t>(site); }
constexpr plugin_cost::Owner owner = plugin_cost::Owner::TemporalAa;

template <class ApiPolicy, Site Id, plugin_cost::ApiClass Class>
inline void note() noexcept {
    ApiPolicy::template note<owner, id(Id), Class>();
}

static_assert(id(Site::TargetSnapshotRead) == 126 &&
              id(Site::TargetCurrentRead) == 127 &&
              id(Site::TargetUavRead) == 128 &&
              id(Site::TargetApplyMrt6) == 129 &&
              id(Site::TargetKeptReadback) == 130 &&
              id(Site::TargetRejectRollback) == 131 &&
              id(Site::TargetRestore) == 132 &&
              id(Site::BlendRead) == 133 &&
              id(Site::BlendApply) == 134 &&
              id(Site::BlendRestore) == 135,
              "EngineVelocity target/blend API site IDs are stable.");
static_assert(id(Site::TargetSnapshotRead) <= plugin_cost::kMaxSiteId &&
              id(Site::TargetCurrentRead) <= plugin_cost::kMaxSiteId &&
              id(Site::TargetUavRead) > plugin_cost::kMaxSiteId &&
              id(Site::BlendRestore) <= plugin_cost::kMaxApiSiteId,
              "EngineVelocity API sites fit the API-only V2 coverage mask.");

}} // namespace edvr::engine_velocity_cost
