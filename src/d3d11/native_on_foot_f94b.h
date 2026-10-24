// F94b (native on-foot VR): the interaction label moved by its own anchor. Journal: the arc doc, F94b.
//
// Header only, no game or D3D11 state, so native_frame_test runs the very functions the hooks use.
//
// The label (a marker whose SetMarker carried a designed offset: the charge port's, the maintenance panel's, a body's) is drawn by the game at its record X0, which
// the game laid out a screen-plane offset away from the marker's anchor P (t = P + s (off.x X - off.y Y), s 0.382 times the distance). The plain markers (the dots,
// the waypoints) are moved so that their anchor is shown where this frame's scene rows show P (F62c/F64, F66f). F93c moved the label by the correction of the
// nearest plain marker in the same eye instead: in the flight of f358d3ce (log 124854) that correction came from the frame before in every corrected draw
// ("reference this frame 0, the frame before 83" each second), the nearest marker was one of three dots whose own corrections differ by up to 36 px at rest
// (key 46's record sits 36.3 px off its anchor, keys 45 and 47's 0.1 px), and a label with no plain marker beside it (12:54:40 to 12:54:45: the reticle alone)
// was left as the game placed it, 929 of 929 such draws. Each of those moves the label against the dots it belongs with.
//
// The label's own anchor needs no other marker. Its record and its anchor go through the same unpatched marker rows (the game's projection), so their difference
// is the game's layout offset as the game drew it this frame; the anchor goes through the scene rows as the dot's does. The record is shown at
//   T = ndc_scene(P), unjittered  +  (ndc_M(X0) - ndc_M(v))
// both from this frame and this eye, v being the anchor in the marker rows' own (HUD) frame. The row patch (drawtag::markerAffineU) keeps every vertex's UI offset
// from the record, so with this T the patched rows show the anchor itself exactly at ndc_scene(P), where its dot is, whatever the affine's scale: the whole label
// is the game's layout moved rigidly onto its anchor. F94b took v as P itself, a world point, and never applied; F95b (below) takes v from the designed offset.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace edvr {
namespace nofoot {
namespace f94b {

// The label's target in the texture (the same space as the dots' targets: what drawtag::markerAffineU shows the record at). nWuP: the anchor through the scene
// rows, unjittered; nM0 and nMP: the record and the anchor through the draw's unpatched marker rows.
inline void ownAnchorTarget(const double nWuP[2], const double nM0[2], const double nMP[2], double T[2]) noexcept {
    for (int k = 0; k < 2; ++k) T[k] = nWuP[k] + (nM0[k] - nMP[k]);
}

// One designed-offset draw of one eye, as the measure sees it (pixels at the marker target's size, x right, y up).
struct Sample {
    uint32_t frame = 0, key = 0;
    double off[2] = {};      // the applied target less the anchor's scene position: the label's place against its own dot
    double off93[2] = {};    // F93c's rule, measured: its target (or, with no reference, the game's placement) less the anchor's scene position
    bool ref93 = false;      // F93c's rule had a reference
    uint32_t refKey = 0;     // ... and its key
    double headDps = -1.0;   // the head's speed (deg/s; negative: unknown)
    double p[3] = {};        // the anchor P in the world (metres): a label whose anchor itself moves moves with it
};

// The second's figures of one eye. A step is the change of a label's place against its anchor from one frame to the next (the same key, consecutive frames):
// zero while the label is as fixed to the world as its anchor. A switch is a frame where F93c's rule took another reference than the frame before (another key,
// or a reference against none). The anchor's own step is its move in the world over the same frames. The eyes' difference is the label's offset from its anchor
// in this eye less the other eye's in the same frame: zero when the label is at its anchor's depth (EyeJoin).
constexpr double kFastDps = 30.0;
struct Sec {
    uint64_t n = 0, steps = 0, switches = 0, fastN = 0, eyeN = 0;
    double stepSum = 0.0, stepMax = 0.0, step93Sum = 0.0, step93Max = 0.0;
    double err93Sum = 0.0, err93Max = 0.0, err93FastSum = 0.0;
    double offSum[2] = {};
    double pStepSum = 0.0, pStepMax = 0.0;      // mm
    double eyeSum = 0.0, eyeMax = 0.0;          // px
};
inline double len2(double x, double y) noexcept { return std::sqrt(x * x + y * y); }

// The two eyes' latest label offsets by key (the render thread's): a draw of one eye joins the other eye's draw of the same key in the same frame, whichever came first.
struct EyeJoin {
    struct Slot {
        bool used = false;
        uint32_t key = 0, frame = 0;
        double off[2] = {};
    };
    static constexpr int kSlots = 8;
    Slot s[2][kSlots];
    // Posts eye e's offset; true with the difference (this eye less the other) when the other eye drew the key in the same frame.
    bool post(int e, uint32_t key, uint32_t frame, const double off[2], double diff[2]) noexcept {
        bool joined = false;
        for (const Slot& q : s[1 - e]) {
            if (q.used && q.key == key && q.frame == frame) {
                diff[0] = off[0] - q.off[0];
                diff[1] = off[1] - q.off[1];
                joined = true;
                break;
            }
        }
        Slot* t = &s[e][0];
        for (Slot& q : s[e]) {
            if (q.used && q.key == key) {
                t = &q;
                break;
            }
            if (!q.used || q.frame < t->frame) t = &q;
        }
        t->used = true;
        t->key = key;
        t->frame = frame;
        t->off[0] = off[0];
        t->off[1] = off[1];
        return joined;
    }
};
inline void eyeJoinTally(bool joined, const double diff[2], Sec& sec) noexcept {
    if (!joined) return;
    const double d = len2(diff[0], diff[1]);
    ++sec.eyeN;
    sec.eyeSum += d;
    sec.eyeMax = std::max(sec.eyeMax, d);
}

// The labels' previous frame per key, across seconds (the render thread's, one per eye).
struct Tracker {
    struct Slot {
        bool used = false;
        uint32_t key = 0, frame = 0;
        double off[2] = {}, off93[2] = {}, p[3] = {};
        bool ref93 = false;
        uint32_t refKey = 0;
    };
    static constexpr int kSlots = 8;
    Slot s[kSlots];
    // Adds a sample to the second's figures; a second draw of the same key in the same frame is not a step.
    void add(const Sample& x, Sec& sec) noexcept {
        ++sec.n;
        sec.offSum[0] += x.off[0];
        sec.offSum[1] += x.off[1];
        const double e93 = len2(x.off93[0] - x.off[0], x.off93[1] - x.off[1]);
        sec.err93Sum += e93;
        sec.err93Max = std::max(sec.err93Max, e93);
        if (x.headDps >= kFastDps) {
            ++sec.fastN;
            sec.err93FastSum += e93;
        }
        Slot* t = nullptr;
        for (Slot& q : s)
            if (q.used && q.key == x.key) t = &q;
        if (!t) {
            t = &s[0];
            for (Slot& q : s) {
                if (!q.used) {
                    t = &q;
                    break;
                }
                if (q.frame < t->frame) t = &q;
            }
            *t = Slot{};
        } else if (t->frame == x.frame) {
            return;
        } else if (t->frame + 1u == x.frame) {
            const double st = len2(x.off[0] - t->off[0], x.off[1] - t->off[1]), st93 = len2(x.off93[0] - t->off93[0], x.off93[1] - t->off93[1]);
            ++sec.steps;
            sec.stepSum += st;
            sec.stepMax = std::max(sec.stepMax, st);
            sec.step93Sum += st93;
            sec.step93Max = std::max(sec.step93Max, st93);
            if (x.ref93 != t->ref93 || (x.ref93 && x.refKey != t->refKey)) ++sec.switches;
            const double dp = 1000.0 * std::sqrt((x.p[0] - t->p[0]) * (x.p[0] - t->p[0]) + (x.p[1] - t->p[1]) * (x.p[1] - t->p[1]) + (x.p[2] - t->p[2]) * (x.p[2] - t->p[2]));
            sec.pStepSum += dp;
            sec.pStepMax = std::max(sec.pStepMax, dp);
        }
        t->used = true;
        t->key = x.key;
        t->frame = x.frame;
        t->off[0] = x.off[0];
        t->off[1] = x.off[1];
        t->off93[0] = x.off93[0];
        t->off93[1] = x.off93[1];
        for (int k = 0; k < 3; ++k) t->p[k] = x.p[k];
        t->ref93 = x.ref93;
        t->refKey = x.refKey;
    }
};

}  // namespace f94b

// ---- F95b: the label's anchor in the marker's HUD frame -------------------------------------------------------------------------------------------------------
// F94b never applied (flight 135403: 3616 label draws, every one refused at the 0.4 NDC gate; 14:00:22, key 1: the label from its "anchor" +526.9 -796.6 px
// against the game's +127.0 -86.5): it put the anchor through the marker rows as P - o_hud, but P (node+0x50) is a world point while the marker rows take the
// record's HUD frame (camera axes, origin o_hud); F47's "P through the marker rows" has read about 50 deg off in every flight. A label's record is laid out in
// that HUD frame from its anchor v by the designed offset: X0 = v + scale (-off.x, off.y, 0), the scale being the record's own (0.382 z for the labels: 0.940 at
// z 2.460, 0.799 at 2.091). The flight's head-still baselines bear it out on two labels through one projection: key 1 (offset -0.4791 -0.3320) +127.0 -86.5 px,
// key 3 (-0.2773 +0.1094) +73.6 +28.5 px at 1536x1632. v is the HUD point the world anchor P stands for, as a plain dot's record stands for its own P: the label's
// decision runs on v as the dot's runs on its record (fixDecide), and the row patch keeps the record's UI offset from v.
namespace f95b {
// D = X0 - v in the HUD frame; false for a scale that is not a finite positive length (the decision is then refused: no record scale).
inline bool labelOffsetHud(float scale, float offX, float offY, double D[3]) noexcept {
    if (!std::isfinite(scale) || !(scale > 0.0f) || !(scale < 1.0e3f) || !std::isfinite(offX) || !std::isfinite(offY)) return false;
    D[0] = -static_cast<double>(scale) * static_cast<double>(offX);
    D[1] = static_cast<double>(scale) * static_cast<double>(offY);
    D[2] = 0.0;
    return true;
}
}  // namespace f95b
}  // namespace nofoot
}  // namespace edvr
