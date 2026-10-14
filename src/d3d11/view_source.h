#pragma once
// The view the sealed rule centres on (view_source.cpp). Resolves which camera renders
// the frame and where it is: the explorer cam (its session on) and the seated view read the rows the temporal pass chose (the game's
// view-to-world [R | c]); on foot, without the explorer cam, no pose is read (the on-foot camera's rows are not read yet), and the view 0
// origin is the only position. Read-only: nothing is written and nothing is chosen.
#include <cstdint>

#include "view_source_core.h"

namespace edvr::view {

struct Resolved {
    Source source = Source::kOrigin;   // where the pose came from; kOrigin when only the origin is known
    Pose pose;                         // world position and forward; valid when poseOk
    bool poseOk = false;
    float origin[3] = {};              // the render context's view 0 origin (ctx + 0x40 + 0x540): the position the earlier code used
    bool originOk = false;
    float originGapM = -1.0f;          // distance from the pose position to the origin (-1 when either is missing)
    const char* reason = "";           // why the pose is or is not there: a sentence for the log (never empty after a resolve)
};

// Resolves the frame's view. ctx is the render context (a bucket's first qword); null reads only the rows. False when neither the rows
// nor the origin can be read.
bool resolveView(uintptr_t ctx, Resolved* out) noexcept;

// The centre the sphere rules use (the sealed trial and the asset census): the explorer cam's rows in the explorer cam; otherwise the
// view 0 origin, which the flights proved; otherwise the rows. False when none can be read. fromRows (optional) says whether the rows
// were the source.
inline bool sphereCentre(const Resolved& v, float out[3], bool* fromRows = nullptr) noexcept {
    const bool rows = v.poseOk && (v.source == Source::kExplorerCam || !v.originOk);
    const float* src = nullptr;
    if (rows) src = v.pose.pos;
    else if (v.originOk) src = v.origin;
    if (!src) return false;
    out[0] = src[0];
    out[1] = src[1];
    out[2] = src[2];
    if (fromRows) *fromRows = rows;
    return true;
}

}  // namespace edvr::view
