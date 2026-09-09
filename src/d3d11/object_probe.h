// The pool probe -- tier 2 stage 1 of docs/per-object-motion.md (2026-09-08).
//
// WHY THIS EXISTS. Tier 2 gives each rigid mover its own motion in the
// temporal pass by reading the pose the game itself stores per instance: a
// 336-byte record in a structured buffer the scene's vertex shaders index at
// t33 (bone base at byte 0, scale at 4, the orientation as four unorm16 at
// 8, the position at 16 -- the head the FSS shader replacement already
// transcribes, fss_panel_vs.h). Two questions gate the design and neither
// can be answered from a census: does a RECORD keep its slot in the pool
// from one frame to the next (question 3 -- the identity the classifier
// would key on, now that question 4 has shown no draw-level identity
// survives a frame), and how many distinct rigid motions does a frame carry
// (question 6 -- the tag budget's demand, against the two codes the stencil
// offers)? Both are read off the pool's bytes on two consecutive frames.
//
// HOW. Once the pool is recognised (the first instanced eye draws of a frame
// are asked for their t33 binding until one resolves to a 336-stride
// structured buffer; after that, one look a second), two frames in a row
// are copied on the GPU into staging buffers every eighth frame and mapped
// three frames later, never waited on. The pair is diffed record by record:
// unchanged, pose changed with the rest of the record intact, rewritten, or
// found at another slot (the same bytes elsewhere last frame: a repacked
// pool). The changed records' rigid motions -- D = W_prev * W_now^-1, the
// same transform for every part of one assembly -- are bucketed within a
// hundredth of a degree and a centimetre, and a pair where more than half
// the pool's positions shifted by one translation is an origin rebase
// (question 7's first half). A totals line prints every 20 s.
//
// A mapped WRITE_DISCARD pointer is write-combined memory and reading
// megabytes of it on the CPU costs milliseconds, which is why the copy is
// the GPU's and the readback is late; the diff itself runs on one sampled
// pair in eight, off the draw path. Off by default (advanced.object_probe).
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// advanced.object_probe, live.
void objectProbeConfigure(Config& cfg);

// For the draw chain's early-return list: true while the probe is on.
bool objectProbeWantsDraws();

// One eye draw, after the eye gate: an instanced draw may be asked for its
// t33 binding, a few times a frame until the pool is known and then once a
// second. One bool when the probe is off.
void objectProbeOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t instances);

// The frame edge, with the owner context: the pair's copies, the late
// readbacks, the diff and the totals.
void objectProbeFrameBoundary(ID3D11DeviceContext* ctx);

void objectProbeShutdown();

// The dominant rigid body's motion between two consecutive frames, in the
// game's world frame (the one the camera rows share, docs/per-object-motion.md
// phase 0 question 7): a point at p now was at R p + t last frame. From the
// largest rigid cluster of the last pair diffed -- a station's turn, with
// every part of it in one cluster -- and held for a while after, since a
// station's rate is constant. False until a pair has given one, and again
// once it has gone stale.
constexpr uint32_t kObjectGrid = 64;   // cells a side of the body's occupancy grid
struct ObjectMotion {
    float    R[9];       // row-major 3x3, over the pair's own interval (for the record)
    float    t[3];
    // The motion as RATES, per millisecond: the axis-angle and the
    // translation of the least-squares rigid fit over the cluster's parts,
    // divided by the pair's interval, so a reader scales them by its own
    // frame's length (a pair on a long frame is a larger turn, and a station
    // turns at a constant rate). Rodrigues of omega * dt is the frame's R.
    float    omegaPerMs[3];
    float    tPerMs[3];
    float    dtMs;       // the pair's own interval
    float    rms;        // the fit's residual, metres
    float    share;      // of the pair's pose changes the cluster held, 0..1
    uint32_t records;    // records in it
    uint32_t age;        // frames since the pair's second frame
    // The camera's position in the pair's second frame, from its scene
    // block: the frame the positions and the box below are in. The floating
    // origin moves on an approach (a rebase of 13 km read from the dumps of
    // 2026-09-08), so a reader whose own camera rows stand far from this is
    // in another frame and should wait for a pair taken in its own.
    float    camPos[3];
    // Where the body IS: the box around its parts' positions now (world
    // frame, padded by the reach), and an occupancy grid over that box --
    // a cell is set within the reach of any part -- so a pixel whose depth
    // puts it inside a set cell is the body's, and one in the empty space
    // between (a ship crossing the slot) is not. The grid is the probe's
    // own buffer, kObjectGrid cubed bytes, x fastest; gridVersion bumps on
    // every rebuild so a reader uploads only what changed.
    float    bmin[3];
    float    bmax[3];
    uint32_t gridVersion;
    const uint8_t* grid;
};
bool objectMotionGet(ObjectMotion* out);
// The reach, metres, that a part marks around itself in the grid (the
// temporal pass's setting, read once a reload); before a rebuild it takes
// the value it finds here.
void objectMotionSetReach(float metres);

}  // namespace edvr
