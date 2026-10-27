// F126 (native on-foot VR): three review findings, pure parts. Header only, no game or D3D11 state, so native_frame_test runs the very functions the hooks use.
// Journal: the arc doc, 2026-10-25, F126.
//
// 1. The body pin takes the midpoint of the eyes from the two eyes' scene rows. The rows are published one eye at a time by the render thread, each with the frame
//    it was published for, and the Animate hook reads them whenever it runs: between the two publications it reads the left eye's rows of frame N + 1 beside the
//    right eye's of frame N. The difference of the two eye positions then holds a frame of the head's motion as well as the IPD, and still passes the IPD window.
// 2. The arm IK's snapshot of the FP skeleton belongs to the FP rig that was current when it was taken (the FP plan's generation). After a skeleton change the
//    snapshot of the earlier rig stays until the next FP call replaces it.
#pragma once

#include "native_on_foot_core.h"

#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f126 {

// ---- 1. the eye pair ------------------------------------------------------------------------------------------------------------------------------
constexpr int32_t kEyeRowsMaxAgeFrames = 8;                 // each eye's rows may be this many frames behind the frame counter
constexpr double kIpdMinM = 0.05, kIpdMaxM = 0.08;          // the distance between the eyes the pair must show (at 100% world scale: see eyeScale below)

struct Cyclo {
    double off[3] = {};   // the world offset from the left eye to the midpoint of the eyes
    double fwd[3] = {};   // the eyes' mean forward
    double up[3] = {};    // the eyes' mean up
};
// Why a pair was or was not taken. Everything but kUse leaves the pin on its last good pair.
enum class Pair : uint8_t { kUse = 0, kUnread, kStale, kFrames, kGeometry };

// rows[e], o[e] and fr[e] as the seqlock read of eye e returned them (read[e]: the read held); cur: the frame counter now. eyeScale: the pair Elite draws with
// against the headset's (world scale, common/world_scale.h worldScaleEyeFactor of the percent the runtime was told; 1 at 100), which the IPD window moves with.
inline Pair cycloFromRows(const bool read[2], const uint32_t fr[2], const float rows[2][16], const float o[2][4], uint32_t cur, Cyclo* out, double eyeScale = 1.0) noexcept {
    double eye[2][3] = {}, up[2][3] = {}, fw[2][3] = {};
    for (int e = 0; e < 2; ++e) {
        if (!read[e]) return Pair::kUnread;
        const int32_t age = static_cast<int32_t>(cur - fr[e]);
        if (age < 0 || age > kEyeRowsMaxAgeFrames) return Pair::kStale;
        const double od[3] = {o[e][0], o[e][1], o[e][2]};
        double r[3];
        if (!eyeFromRows(rows[e], od, eye[e]) || !rotAxesFromClip(rows[e], r, up[e], fw[e])) return Pair::kGeometry;
    }
    // A pair is two eyes of one frame. Rows a frame apart both pass the age test above, and their difference holds the head's motion over that frame as well as
    // the IPD (44 mm at 4 m/s and 90 Hz, 22 mm of it in the midpoint) while the IPD window still accepts it.
    if (fr[0] != fr[1]) return Pair::kFrames;
    const double d[3] = {eye[1][0] - eye[0][0], eye[1][1] - eye[0][1], eye[1][2] - eye[0][2]};
    const double ipd = ikLen(d);
    const double mf[3] = {fw[0][0] + fw[1][0], fw[0][1] + fw[1][1], fw[0][2] + fw[1][2]};
    const double mu[3] = {up[0][0] + up[1][0], up[0][1] + up[1][1], up[0][2] + up[1][2]};
    double uf[3], uu[3];
    if (!(ipd > kIpdMinM * eyeScale && ipd < kIpdMaxM * eyeScale && ikUnit(mf, uf) && ikUnit(mu, uu))) return Pair::kGeometry;
    for (int i = 0; i < 3; ++i) {
        out->off[i] = 0.5 * d[i];
        out->fwd[i] = uf[i];
        out->up[i] = uu[i];
    }
    return Pair::kUse;
}

// ---- 2. the FP snapshot's rig ---------------------------------------------------------------------------------------------------------------------
// The FP plan is published under a sequence counter (odd while it is written) whose value, once even, is the generation the plan carries; its X is 0 until the
// plan is complete. A snapshot is of the current rig when it was taken from that X under that generation.
inline bool snapshotRigCurrent(uint32_t planSeq, uint64_t planX, uint64_t snapX, uint32_t snapGeneration) noexcept {
    return !(planSeq & 1u) && snapX == planX && snapGeneration == planSeq;
}
// Whether the arm IK may read the snapshot's matrices at all: the anatomical retarget and the attach fallback both do.
inline bool armsMayUseSnapshot(bool ageUsable, bool rigCurrent) noexcept { return ageUsable && rigCurrent; }

}  // namespace f126
}  // namespace nofoot
}  // namespace edvr
