#pragma once

#include <cstdint>

namespace edvr { namespace cockpit_cost {

// Stable CockpitVisuals direct-context API sites. Night Vision owns IDs 0–38;
// keep these disjoint so one fixed collector coverage mask can include both.
enum class Site : uint16_t {
    TargetVsGetShader = 64,
    TargetPsGetShader = 65,
    TargetPsSetShaderApply = 66,
    TargetPsSetShaderRestore = 67,

    RemlokGetDevice = 72,
    RemlokRsGetState = 73,
    RemlokRsGetViewportsCurrent = 74,
    RemlokRsGetScissorRects = 75,
    RemlokRsGetViewportsSaved = 76,
    RemlokRsSetViewportsApply = 77,
    RemlokRsSetScissorRectsApply = 78,
    RemlokRsSetStateApply = 79,
    RemlokRsSetStateRestore = 80,
    RemlokRsSetScissorRectsRestore = 81,
    RemlokRsSetViewportsRestore = 82,
};

constexpr uint16_t id(Site site) noexcept { return static_cast<uint16_t>(site); }

static_assert(id(Site::TargetVsGetShader) == 64 && id(Site::TargetPsSetShaderRestore) == 67,
              "TargetSharp direct-context API site IDs are stable.");
static_assert(id(Site::RemlokGetDevice) == 72 && id(Site::RemlokRsSetViewportsRestore) == 82,
              "RemLok direct-context API site IDs are stable.");
static_assert(id(Site::TargetPsSetShaderRestore) < id(Site::RemlokGetDevice),
              "TargetSharp and RemLok site ranges must not overlap.");

}} // namespace edvr::cockpit_cost
