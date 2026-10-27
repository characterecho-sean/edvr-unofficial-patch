// F113 (native on-foot VR): each eye's scene rows whenever the world is drawn. Header only, no game or D3D11 state, so native_frame_test runs the very
// functions the render thread uses. Journal: the arc doc, 2026-10-25, F113.
//
// An eye's scene rows (b1 rows 270..273 and the origin row 275: the camera the eye was drawn with) were taken only from a world draw of three scene vertex
// shaders (drawtag::kSceneVs) that also passes the engine families' filter. Looking at bare ground or the sky none of them draws, and every consumer refused:
// the weapon UI pin ("no scene rows for the eye" 10294 times in the 33e05706 flight, edvr_gfx_20261025_071049), the marker patch, the labels, the ADS lock (F110's
// model eye on 56% of ADS frames) and F109's look-to-aim guard. F87f had named the depth prepass as a source, but its capture sat behind the families' filter and
// never ran (0 frames given in the 04:41, 05:56 and 07:10 flights).
//
// The census (the F57 row census of the F98 flight edvr_gfx_20261024_192353 and a tester's edvr_gfx_20261025_114257, both before F100 put it behind the probe
// key): the depth prepass (VS F516BF0201303B87, PS B40B0462256E31C2) carried the eye's main scene content in every frame both drew (20,078 and 20,078 frames in the
// F98 flight, 313 and 658 in the tester's; never a different content) and drew in 3,025 and 3,173 of the tester's frames that had no scene mesh; the terrain
// patch (VS ACE405F428C17EF6, no PS) likewise (1,720 and 1,230 of 1,720 and 1,230 compared, 416 and 413 frames with no mesh). The other frequent shader, VS
// 53211E8C072CD02E, carried another camera (up to 176 deg off): not a source.
//
// So both are row sources: the first of them or of the scene meshes to draw into an eye's scene depth gives the eye's rows. Once a second per eye whose rows
// came from a source, the first scene mesh draw of that eye checks them against its own; one difference over f87::kFbTolerance latches the sources off.
#pragma once

#include "native_on_foot_f87.h"   // kFbVs, kFbPs, kFbTolerance, FbGuard

#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f113 {

constexpr uint64_t kTerrainVs = 0xACE405F428C17EF6ull;   // the terrain patch, no pixel shader
enum RowSource : uint8_t { kFromMeshes = 0, kFromPrepass = 1, kFromTerrain = 2, kRowSources = 3 };
inline const char* rowSourceName(int s) noexcept { return s == kFromMeshes ? "the scene meshes" : s == kFromPrepass ? "the depth prepass" : s == kFromTerrain ? "the terrain" : "none"; }
// A vertex shader worth reading the pixel shader for (the cheap test on every draw outside the engine families).
inline bool rowSourceVs(uint64_t vs) noexcept { return vs == f87::kFbVs || vs == kTerrainVs; }
// The source a draw outside the engine families is, or kRowSources.
inline uint8_t rowSourceOf(uint64_t vs, uint64_t ps) noexcept {
    if (vs == f87::kFbVs && ps == f87::kFbPs) return kFromPrepass;
    if (vs == kTerrainVs && ps == 0) return kFromTerrain;
    return kRowSources;
}

// One eye's second: the frames, those whose rows came from each source, those with none.
struct EyeSecond {
    uint32_t frames = 0, from[kRowSources] = {}, none = 0;
    void frameEnd(bool had, uint8_t source) noexcept {
        ++frames;
        if (had && source < kRowSources) ++from[source];
        else ++none;
    }
};

// The check's schedule: an eye whose rows came from a source is checked by its next scene mesh draw at most once a second.
struct CheckClock {
    uint64_t lastMs[2] = {0, 0};
    bool due(int eye, uint64_t nowMs) const noexcept { return (eye == 0 || eye == 1) && (lastMs[eye] == 0 || nowMs - lastMs[eye] >= 1000); }
    void done(int eye, uint64_t nowMs) noexcept {
        if (eye == 0 || eye == 1) lastMs[eye] = nowMs ? nowMs : 1;
    }
};

}  // namespace f113
}  // namespace nofoot
}  // namespace edvr
