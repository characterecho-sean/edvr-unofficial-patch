// The view the aim comes from (view_source.h; TEMPORARY). In order: the explorer cam's free camera, from its activity's world pose while its
// placement stands (explorer_cam.cpp, explorerCamWorldPose); seated, the rows the temporal pass chose for the frame (the game's view-to-world
// [R | c], view_source_core.h), which exist only after the eye treat, so the admission store often finds none; otherwise the origin: the
// render context's view 0 origin (ctx + 0x40 + 0x540), with no pose. On foot without a placement no pose is read, because the chosen rows
// are not the on-foot camera and the on-foot camera's rows are not read yet. Every result names its reason, so a log says why there is no aim.
#include "view_source.h"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "journal_watch.h"   // journalOnFootKnown(), journalOnFoot()
#include "temporal_pass.h"   // temporalPassChosenRows()
#include "ui_layer.h"        // uiLayerLiveForWorldRoute(), uiLayerWorldScreenHeld()

namespace edvr::view {
namespace {

constexpr uintptr_t kCtxView0 = 0x40;      // the render context's first view
constexpr uintptr_t kViewOrigin = 0x540;   // the view's float3 origin

bool readMem(void* dst, uintptr_t src, size_t n) noexcept {
    if (!src) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The on-foot test the vscreen footprint uses (vscreen.cpp, the frame boundary): the world route's screen flag while the route is live,
// else the journal's on-foot state.
bool onFootNow() noexcept {
    return uiLayerLiveForWorldRoute() ? uiLayerWorldScreenHeld() : (journalOnFootKnown() && journalOnFoot());
}

float distance3(const float a[3], const float b[3]) noexcept {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

bool resolveView(uintptr_t ctx, Resolved* out) noexcept {
    if (!out) return false;
    *out = Resolved{};
    out->originOk = ctx != 0 && readMem(out->origin, ctx + kCtxView0 + kViewOrigin, sizeof(out->origin));

    // The explorer cam is not read by the product: the station releases while it is on (optimized_station.cpp).
    const bool explorerPose = false;
    const bool explorerOn = false;
    const bool foot = onFootNow();

    // 2. Seated: the rows the temporal pass chose for this frame. They exist only after the eye treat, so a call made before it finds none.
    bool rowsOk = false;
    Pose rowPose;
    const char* rowsReason = "";
    if (!foot) {
        float rows[12] = {};
        bool bound = false;
        if (!temporalPassChosenRows(rows, &bound)) {
            rowsReason = "no chosen rows at this call (the temporal pass chooses them at the eye treat)";
        } else if (!poseFromRows(rows, &rowPose)) {
            rowsReason = "the chosen rows are not a frame (orthonormal axes and a finite origin are required)";
        } else {
            rowsOk = true;
        }
    }

    const bool poseOk = explorerPose || rowsOk;
    out->source = chooseSource(explorerPose, foot, poseOk);
    out->poseOk = poseOk;
    if (rowsOk) {
        out->pose = rowPose;
        out->reason = "eye rows: the chosen rows of this frame";
    } else if (foot) {
        out->reason = "on foot without an explorer cam placement: the on-foot camera's rows are not read yet";
    } else {
        out->reason = rowsReason;
    }
    if (poseOk && out->originOk) out->originGapM = distance3(out->pose.pos, out->origin);
    return poseOk || out->originOk;
}

}  // namespace edvr::view
