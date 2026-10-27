// F130 (native on-foot VR): one disagreeing scene mesh no longer switches the eye rows' sources off. Header only, no game or D3D11 state, so native_frame_test runs the
// very functions the render thread uses. Journal: the arc doc, 2026-10-27, F130.
//
// F113 made the depth prepass and the terrain patch row sources for an eye's scene rows (the camera the eye was drawn with) beside the scene meshes, and latched them OFF
// for the session at the first disagreement with a scene mesh. The Frontier SRV training flight (edvr_gfx_20261027_064839) latched at 06:54:46.322 after one check: a
// mesh that drew into eye 0 for 8 frames (most likely the SRV, whose meshes carry other rows) differed from the terrain's rows by 0.239. From then on only scene meshes
// gave rows, the weapon UI pin refused every draw where none drew ("no scene rows for the eye", 90 of 90 a second) and, near the SRV, moved the readout 25-43 deg using
// the SRV mesh's rows. Before the latch the sources pinned 100% of draws with residuals under 0.005 px; all earlier checks of the session agreed (max diff 0).
//
// So a disagreement is a finding about the MESH, not the source:
//  - a mesh whose rows disagree with a source's in the same frame is marked by its identity (the shaders, the buffer and the first constant of its cb1) and gives no rows
//    again this session (up to kMarkMax identities; the rest are counted);
//  - a source that draws after a mesh published the eye's rows for the frame compares them, and its rows replace the mesh's when they differ;
//  - the sources are never switched off.
#pragma once

#include "native_on_foot_f113.h"   // RowSource
#include "native_on_foot_f87.h"    // kFbTolerance

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f130 {

// A scene mesh draw's identity as the render thread sees it: its shaders, the buffer bound as cb1 (identity only: the pointer) and cb1's first constant.
struct MeshId {
    uint64_t vs = 0, ps = 0, cb1 = 0;
    uint32_t first = 0;
    bool same(const MeshId& o) const noexcept { return vs == o.vs && ps == o.ps && cb1 == o.cb1 && first == o.first; }
};

constexpr size_t kMarkMax = 32;
enum MarkResult : uint8_t { kMarkedNew = 0, kMarkedAlready = 1, kMarkTableFull = 2 };
struct MarkTable {
    MeshId ids[kMarkMax];
    uint32_t n = 0;
    uint32_t overflow = 0;   // identities that disagreed with the table full: counted, not marked
    bool isMarked(const MeshId& id) const noexcept {
        for (uint32_t i = 0; i < n; ++i)
            if (ids[i].same(id)) return true;
        return false;
    }
    MarkResult mark(const MeshId& id) noexcept {
        if (isMarked(id)) return kMarkedAlready;
        if (n >= kMarkMax) {
            ++overflow;
            return kMarkTableFull;
        }
        ids[n++] = id;
        return kMarkedNew;
    }
};

// The largest difference, in the 16 row floats and in the 4 origin floats (a NaN counts as infinite, as f87::rowsMaxDiff counts it).
struct RowsDiff {
    double rows = 0.0, origin = 0.0;
    double max() const noexcept { return rows > origin ? rows : origin; }
};
inline double maxAbsDiff(const float* a, const float* b, int n) noexcept {
    double m = 0.0;
    for (int i = 0; i < n; ++i) {
        const double d = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        if (!(d == d)) return 1e30;
        if (d > m) m = d;
    }
    return m;
}
// a, b: b1 rows 270..273 (16 floats) then the origin row 275 (4).
inline RowsDiff rowsDiff(const float a[20], const float b[20]) noexcept {
    RowsDiff d;
    d.rows = maxAbsDiff(a, b, 16);
    d.origin = maxAbsDiff(a + 16, b + 16, 4);
    return d;
}
inline bool disagree(double diff) noexcept { return !(diff <= f87::kFbTolerance); }

// What a row source's draw does for the eye it draws into.
//  haveRows: the eye already has this frame's rows; fromMeshes: they came from a scene mesh; meshCpu: that mesh's rows are known on the CPU (not a staged copy);
//  comparePending: the once-a-second comparison is due for the eye this frame.
enum SourceAction : uint8_t { kSrcPublish = 0, kSrcCompare = 1, kSrcSkip = 2 };
inline SourceAction sourceAction(bool haveRows, bool fromMeshes, bool meshCpu, bool comparePending) noexcept {
    if (!haveRows) return kSrcPublish;
    if (fromMeshes && meshCpu && comparePending) return kSrcCompare;
    return kSrcSkip;
}

// The source's name in the event line ("against the depth prepass").
inline const char* sourceNoun(int s) noexcept {
    return s == f113::kFromPrepass ? "depth prepass" : s == f113::kFromTerrain ? "terrain" : "scene meshes";
}

}  // namespace f130
}  // namespace nofoot
}  // namespace edvr
