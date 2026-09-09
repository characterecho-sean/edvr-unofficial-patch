#include "object_probe.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/temporal_math.h"   // temporalRigidFit, temporalRodrigues: the body's motion from its parts
#include "../common/timing.h"
#include "binding_shadow.h"

namespace edvr {
namespace {

constexpr uint32_t kRecordBytes = 336;   // docs/per-object-motion.md, question 5: 71 of 71 shaders
constexpr uint32_t kPoolSlot = 33;       // ...at t33, likewise
constexpr uint32_t kChecksPerFrame = 4;  // instanced eye draws asked for t33 before a frame gives up
constexpr uint64_t kRecheckMs = 1000;    // once the pool is known, one look a second
constexpr uint32_t kPairEvery = 8;       // a frame PAIR is copied every this many frames
// THE PAIR IS TWO FRAMES APART, from a start that alternates between even
// and odd frames (2026-09-09, the flight of 16:22). The pool's tail -- 271
// to 298 records at the station, the docking hub's among them -- was live
// in the second frame of every saved pair and never in the first: the
// game writes those records on alternate frames only, and a pair of
// consecutive frames holds them once at most, so the diff never saw the
// hub and the hub took the ring's path (its face smeared while the ring
// was crisp). Two frames apart, a pair whose start has the right parity
// holds them twice; the start alternates so every other pair does, and
// the second body's hold (kBody2HoldPairs) spans the pairs between. The
// rates divide by the pair's own interval, so nothing downstream changes
// but the per-pair thresholds, which a doubled motion meets more easily.
constexpr uint32_t kPairSpan = 2;
constexpr uint32_t kReadAfter = 2;       // frames before a copy is asked for (never waited on); two, so a
                                         // stepped part's multiple (trackFrame) is a frame fresher
constexpr uint32_t kDropAfter = 30;      // ...and after which a copy still in flight is given up
constexpr int      kRing = 6;            // a copy a frame (the stepped parts), two to three in flight
constexpr uint64_t kReportMs = 20000;
constexpr float    kRotQuantDeg = 0.01f; // the design's tolerance: a hundredth of a degree
constexpr uint32_t kAbsentFrames = 600;  // frames with the probe on and no pool before saying so
// Raw pairs to disk for the desk (the third flight, 2026-09-08 12:00: the
// totals could not tell a frame that moves from a decode that is off from a
// stale slot; two frames' raw records with the scene block of each can).
constexpr uint64_t kDumpEveryMs = 30000;
constexpr uint32_t kDumpMax = 8;
constexpr uint32_t kSceneSlot = 1;       // the scene block, VS b1: cb1[275] is the camera in the record's frame

bool     g_on = false;        // the pool is copied and diffed (the probe, or the fix that reads it)
bool     g_verbose = false;   // advanced.object_probe: the totals, the dumps, the absent line
bool     g_wasOn = false;
uint32_t g_checksLeft = 0;
// The dominant body's motion for the temporal pass (tier 2): the largest
// cluster of the last pair, held kMotionHoldFrames after it. The gates
// below keep a shuffled pool's garbage out: a pair whose largest cluster
// is a scatter of re-slotted records has no body in it.
constexpr uint32_t kMotionHoldFrames = 120;
constexpr uint32_t kMotionMinRecords = 40;
constexpr float    kMotionMinShare = 0.25f;
constexpr float    kMotionMinDeg = 0.004f;  // a pair; a station turns 0.02-0.045, a static scatter 0
constexpr float    kMotionMaxDeg = 1.0f;    // a frame; a station turns a twentieth of that
constexpr float    kMotionMaxM = 20.0f;
constexpr float    kMotionMaxRmsM = 0.5f;   // the rigid fit's residual: parts that moved as one
ObjectMotion g_motion = {};
uint32_t g_motionAge = 0;          // frames since the published body's pair (the render thread's, under g_publish)
float    g_reachM = 1500.0f;
// THE PUBLISHED BODY (objectMotionGet) and its lock. The diff runs on a
// worker thread (workerMain says why) and writes g_motion, its own
// working copy, as it goes; what the render thread reads is g_motionPub,
// copied from it in one short critical section when a pair is taken,
// with the age reset. The grid is double-buffered: the worker fills the
// back buffer and publishes its pointer, so the upload the pass makes
// from the last pointer is never overwritten under it (the next fill is
// eight frames away at the soonest).
std::mutex   g_publish;
ObjectMotion g_motionPub = {};
bool         g_motionPubValid = false;
constexpr size_t kGridBytes = static_cast<size_t>(kObjectGrid) * kObjectGrid * kObjectGrid;
uint8_t  g_gridBufs[2][kGridBytes];
int      g_gridBack = 0;
uint8_t* g_grid = g_gridBufs[0];   // the buffer being filled (buildGrid picks it)
uint32_t g_gridVersion = 0;
// THE WORKER (workerMain): one job at a time, the pair's two copies and
// the camera positions; a pair that finds the worker still on the last
// is dropped and counted.
struct DiffJob {
    std::vector<uint8_t> prev, now;
    float    dtMs = 0.0f;
    float    cam[3] = {0.0f, 0.0f, 0.0f};
    bool     haveCam = false;
    float    camPrev[3] = {0.0f, 0.0f, 0.0f};
    bool     haveCamPrev = false;
    uint32_t lagFrames = 0;
};
std::mutex              g_jobLock;
std::condition_variable g_jobCv;
DiffJob                 g_job;
bool                    g_jobPending = false;
bool                    g_jobRunning = false;
bool                    g_workerStop = false;
std::thread*            g_worker = nullptr;   // a pointer: a joinable std::thread destroyed at unload would terminate the process
std::atomic<bool>       g_resetWorker{false};   // switched off and on: the worker's continuity and rings start over
uint64_t                g_busySkipped = 0;
float    g_gridCell = 0.0f;   // the lattice's cell, metres; 0 = not chosen yet
constexpr float kShipRadiusM = 100.0f;   // the player's own parts sit here, co-rotating in a slot; not the body's
// The body's near floor, shared with the pass (objects.y): parts nearer the
// camera than this do not mark the grid, and the pass claims no pixel nearer
// than this -- the ship's own hull as seen from the seat is within it, a
// hangar's walls before the ship is latched to the pad are not (2026-09-09).
constexpr float kBodyNearM = 50.0f;
// How far past the members' box a recorded part of the body's types still
// widens it (buildGrid says why): a station's unslotted tips, not a second
// station of the kind.
constexpr float kBodyTipM = 3000.0f;
// The same body from pair to pair (diffPair says why): its parts' centroid,
// camera-relative, within this of the last pair's, unless the last body is
// this old.
constexpr float    kBodyContinuityM = 2000.0f;
constexpr uint32_t kBodyContinuityFrames = 60;
float    g_lastRel[3] = {};
bool     g_lastRelValid = false;
uint64_t g_otherBodyPairs = 0;
uint64_t g_otherBodyNoteMs = 0;
// The held rate: the rolling median of the last sixteen pairs' rates
// (diffPair says why a median).
struct RatePair {
    float w[3];   // the pair's turn per ms, an axis-angle in radians
    float t[3];   // ...and its translation per ms, metres
};
constexpr uint32_t kRateRing = 16;
RatePair g_rateRing[kRateRing] = {};
uint32_t g_rateRingN = 0;
uint64_t g_rateOutliers = 0;
uint64_t g_rateOutlierNoteMs = 0;
// THE SECOND BODY's own (ObjectMotion::body2 says what it is): its rate
// ring and last centroid for continuity, the pairs it is held over when a
// pair does not find it, its parts' positions for the grid meanwhile, and
// what the report says of it.
constexpr uint32_t kBody2MinRecords = 40;
constexpr float    kBody2NearM = 6000.0f;   // its centroid within this of the body's, camera-relative: the same structure
constexpr uint32_t kBody2HoldPairs = 4;
RatePair g_rateRing2[kRateRing] = {};
uint32_t g_rateRing2N = 0;
float    g_lastRel2[3] = {};
bool     g_lastRel2Valid = false;
uint32_t g_body2Hold = 0;
std::vector<float> g_body2Pos;   // x y z per part, the last pair it was found
float    g_body2ReachM = 0.0f;
uint32_t g_body2LastRecords = 0;
float    g_body2LastDeg = 0.0f;
float    g_body2LastRelDeg = 0.0f;   // its turn's difference from the body's, degrees a pair
uint64_t g_body2Pairs = 0, g_body2HeldPairs = 0, g_body2Fragments = 0;
constexpr uint32_t kBody2Confirm = 3;   // consistent pairs before a second body is taken (the stepped parts alternate)
uint32_t g_body2Streak = 0;
float    g_body2LastW[3] = {};
// THE STEPPED PARTS' state (object_probe.h says what they are; trackFrame
// does the work): the frame before's copy, each slot's history of its
// multiple of the body's turn, the predictions made, this frame's cells,
// and the report's counts.
constexpr uint32_t kMHist = 8;
constexpr float    kMAxisSlack = 0.35f;   // of the body's turn: a turn off the body's axis is no multiple of it
std::vector<uint8_t>  g_lastBytes;        // the frame before's copy
uint32_t g_lastBytesFrame = 0;
double   g_lastBytesStamp = 0.0;
std::vector<int8_t>   g_mHist;            // n * kMHist, [i * kMHist] the newest; 127 = no entry
std::vector<int8_t>   g_mPred;            // the prediction made per slot, for g_mPredFrame; 127 = none
std::vector<uint32_t> g_mPredFrame;
std::vector<SteppedCell> g_steppedCells;
uint64_t g_stTrackFrames = 0, g_stStepped = 0, g_stPeriod2 = 0, g_stSteady = 0, g_stIrregular = 0;
uint64_t g_stPredicted = 0, g_stHit = 0;
double   g_stMsSum = 0.0;
// Moving ships (object_probe.h, takeShips): taken within this of the
// camera (the pass's setting), held this long past their pair -- three
// pair intervals, so a pair that lost a ship to a slot shuffle does not
// drop it, and a ship that left the pool or the range is gone within a
// third of a second -- and boxed this far past their parts' recorded
// positions: a part's mesh around its origin, a hull section or a
// thruster housing, is tens of metres on the largest hulls.
constexpr uint32_t kShipHoldFrames = 24;
constexpr float    kShipPadM = 30.0f;
constexpr uint32_t kShipMinRecords = 3;     // the rigid fit's floor
constexpr float    kShipMaxRmsM = 0.5f;     // the fit's residual: parts that moved as one
constexpr float    kShipMinMoveM = 0.02f;   // a pair: under this and under kMotionMinDeg it stands still (the world path's)
constexpr float    kShipTailSpeedM = 0.5f;  // a pair (forty-five metres a second): slower, the plume is short and the way it flies a guess
constexpr float    kShipTailM = 5.0f;       // behind the rearmost part: the drive's nozzle sits a few metres behind its part's origin
constexpr uint32_t kDtRing = 16;
float      g_shipRangeM = 1000.0f;
ObjectShip g_ships[kObjectShipsMax] = {};
uint32_t   g_shipCount = 0;
uint32_t   g_shipAge = 0;
float      g_shipCamPos[3] = {};
float      g_dtRing[kDtRing] = {};
uint32_t   g_dtRingN = 0;
uint64_t   g_shipPairs = 0, g_shipsTaken = 0, g_shipsOutOfRange = 0, g_shipsSlices = 0;
uint64_t   g_shipsStill = 0, g_shipsUnfit = 0, g_shipsOverCap = 0, g_shipsFew = 0;
uint32_t   g_shipsMax = 0;
uint64_t   g_shipNoteMs = 0;
uint32_t   g_pairLagFrames = 0;   // frames from the pair's second copy to its diff (poll sets it): the ships' age starts there
// The pair's FIRST camera position (the kept copy's), so takeShips can tell
// the player's own parts by their motion: they move with the camera.
float      g_keepCam[3] = {};
bool       g_keepCamValid = false;
float      g_pairCamPrev[3] = {};
bool       g_pairCamPrevValid = false;
constexpr float kShipOwnRadiusM = 20.0f;   // ships: parts this near the seat are the player's whatever they do
uint64_t   g_shipsOwn = 0;

// The record's head, decoded the way the game's own shaders decode it
// (fss_panel_vs.h: edvrDecodeQuat, the position at byte 16).
struct Pose {
    float s;
    float q[4];   // x y z w
    float p[3];
};

Pose decodePose(const uint8_t* r) {
    Pose o;
    memcpy(&o.s, r + 4, 4);
    uint32_t xy = 0, zw = 0;
    memcpy(&xy, r + 8, 4);
    memcpy(&zw, r + 12, 4);
    o.q[0] = static_cast<float>(xy & 0xFFFFu) * 0.000031f - 1.0f;
    o.q[1] = static_cast<float>(xy >> 16) * 0.000031f - 1.0f;
    o.q[2] = static_cast<float>(zw & 0xFFFFu) * 0.000031f - 1.0f;
    o.q[3] = static_cast<float>(zw >> 16) * 0.000031f - 1.0f;
    const float n = sqrtf(o.q[0] * o.q[0] + o.q[1] * o.q[1] + o.q[2] * o.q[2] + o.q[3] * o.q[3]);
    if (n > 1e-6f) {
        for (float& c : o.q) c /= n;
    }
    memcpy(o.p, r + 16, 12);
    return o;
}

// The body's occupancy: boxed by its members' positions now, padded by the
// reach, and marked by EVERY live record of the body's types -- each
// marking the cells within the reach of it. The members alone marked it
// on the first grid builds, and that flickered the sparse parts: a record
// not rewritten this frame, or sitting in a shuffled slot, is never a pose
// change and never a member, and the flight of 2026-09-08 17:21 had 548 of
// 956 live records of a station's types outside its cluster on one pair
// (153 and 222 on the two before). The dense core stayed claimed because
// some member always marked its cells; a panel boom with a few records
// depended on those few being members THAT pair, and the panels "appear
// jerky at any distance". The signature is the type (the fourth flight:
// 0-8% unique), which is what a body's occupancy should be keyed on: a
// station's parts are its own types, and a stray of the same type
// elsewhere falls outside the box. A station's parts are placed metres
// apart and tens of metres across, so at sixty metres the slot's walls and
// the rim read solid and the space between the arms stays empty.
// The second body's reach around each of its parts: three times their
// median spacing (a hub's skin pieces sit tens of metres apart), never
// under two cells nor over the body's reach. Its parts are few, so the
// pairwise pass is nothing.
float body2Reach(const float* pos, int count, float cell) {
    std::vector<float> nn(static_cast<size_t>(count), 1e30f);
    for (int a = 0; a < count; ++a) {
        const float* pa = pos + a * 3;
        for (int b = a + 1; b < count; ++b) {
            const float* pb = pos + b * 3;
            const float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < nn[static_cast<size_t>(a)]) nn[static_cast<size_t>(a)] = d2;
            if (d2 < nn[static_cast<size_t>(b)]) nn[static_cast<size_t>(b)] = d2;
        }
    }
    std::vector<float> sorted(nn);
    std::sort(sorted.begin(), sorted.end());
    float reach = 3.0f * sqrtf(sorted[sorted.size() / 2]);
    if (reach < 2.0f * cell) reach = 2.0f * cell;
    if (reach > g_reachM) reach = g_reachM;
    return reach;
}

void buildGrid(const uint8_t* now, uint32_t n, const std::vector<uint8_t>& liveNow,
               const std::vector<uint64_t>& sigNow, const std::unordered_set<uint64_t>& bodySigs,
               const int* memberSlots, int count, const float* pos, const float* camPos,
               const float* b2Pos, int b2Count) {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int m = 0; m < count; ++m) {
        const float* p = pos + memberSlots[m] * 3;
        for (int k = 0; k < 3; ++k) {
            if (p[k] < lo[k]) lo[k] = p[k];
            if (p[k] > hi[k]) hi[k] = p[k];
        }
    }
    // ...widened to the body's other recorded parts near it. A station's
    // tips land in new slots every frame (the dumps of 2026-09-09 06:43: the
    // spine's last parts "shuffled" in every pair, a kilometre or two past
    // the last member), so they can never be members; keyed by type they
    // would mark, but only inside the box, and the box was the members'.
    // Any live record of the body's types within kBodyTipM of the members'
    // box widens it; a stray of the same type farther off (another station
    // of the kind, tens of kilometres away) does not, which keeps the cell
    // from growing to cover it.
    // ...and grown again from what it took in, until nothing more is within
    // reach of it: a pair whose members are one end of the station (the
    // whole station flashed "outside the cells" for a split second on
    // 2026-09-09) still boxes the station's every recorded part, since no
    // two of them sit more than kBodyTipM apart along it.
    for (int pass = 0; pass < 8; ++pass) {
        bool grew = false;
        for (uint32_t i = 0; i < n; ++i) {
            if (!liveNow[i] || !bodySigs.count(sigNow[i])) continue;
            const Pose pr = decodePose(now + i * kRecordBytes);
            bool nearBox = true;   // ("near" is a Windows macro)
            for (int k = 0; k < 3; ++k) {
                if (pr.p[k] < lo[k] - kBodyTipM || pr.p[k] > hi[k] + kBodyTipM) nearBox = false;
            }
            if (!nearBox) continue;
            for (int k = 0; k < 3; ++k) {
                if (pr.p[k] < lo[k]) { lo[k] = pr.p[k]; grew = true; }
                if (pr.p[k] > hi[k]) { hi[k] = pr.p[k]; grew = true; }
            }
        }
        if (!grew) break;
    }
    // The cells sit on a FIXED world lattice: a power of two of metres a
    // side, the box's corner at a multiple of it, sixty-two cells covering
    // the parts' extent with the reach either side. A box cut to the
    // parts' extent moved its cell boundaries by up to a cell every pair
    // as members came and went, and the pixels along the station's outer
    // skin flipped between the body's vector and the camera's -- the
    // shimmer at the rim the player saw on the second flight. On the
    // lattice the same place in the world is the same cell, pair after
    // pair, while the cell size holds (it moves only when the extent
    // crosses a power of two).
    const int gn = static_cast<int>(kObjectGrid);
    float ext = 1.0f;
    for (int k = 0; k < 3; ++k) {
        const float e = (hi[k] - lo[k]) + 2.0f * g_reachM;
        if (e > ext) ext = e;
    }
    // The cell size holds unless the extent outgrows the grid or shrinks
    // under a third of it: a body whose extent hovers at a power of two
    // would otherwise flip the lattice every other pair.
    float cell = g_gridCell;
    if (cell <= 0.0f || cell * static_cast<float>(gn - 2) < ext || cell * static_cast<float>(gn - 2) > 3.0f * ext) {
        cell = 16.0f;
        while (cell * static_cast<float>(gn - 2) < ext && cell < 8192.0f) cell *= 2.0f;
    }
    g_gridCell = cell;
    for (int k = 0; k < 3; ++k) {
        lo[k] = floorf((lo[k] - g_reachM) / cell) * cell;
        hi[k] = lo[k] + cell * static_cast<float>(gn);
        g_motion.bmin[k] = lo[k];
        g_motion.bmax[k] = hi[k];
    }
    g_grid = g_gridBufs[g_gridBack];   // the back buffer (g_publish says why two)
    g_gridBack ^= 1;
    memset(g_grid, 0, kGridBytes);
    // The seeds: one cell per recorded part of the body's types, skipping
    // the parts at the pilot's elbow (the ship's own, within the body's
    // near floor) and anything outside the box.
    for (uint32_t i = 0; i < n; ++i) {
        if (!liveNow[i] || !bodySigs.count(sigNow[i])) continue;
        const Pose pr = decodePose(now + i * kRecordBytes);
        const float* p = pr.p;
        if (camPos) {
            const float dx = p[0] - camPos[0], dy = p[1] - camPos[1], dz = p[2] - camPos[2];
            if (dx * dx + dy * dy + dz * dz < kBodyNearM * kBodyNearM) continue;
        }
        bool inBox = true;
        int c[3];
        for (int k = 0; k < 3; ++k) {
            if (p[k] < lo[k] || p[k] >= hi[k]) inBox = false;
            int idx = static_cast<int>((p[k] - lo[k]) / cell);
            c[k] = idx < 0 ? 0 : (idx >= gn ? gn - 1 : idx);
        }
        if (!inBox) continue;
        g_grid[(c[2] * gn + c[1]) * gn + c[0]] = 255;
    }
    // The reach, as cells either side of every seed: a station's biggest
    // single parts -- the docking hub's skin, a ring's deck -- reach
    // several hundred metres from where the game records them, and at one
    // cell either side (sixty metres at 256 m cells, 2026-09-09) the hub's
    // skin ran past the marked cells along a lattice plane, a straight cut
    // between clear and smeared that the player saw. Three one-dimensional
    // passes make a box of 2*dil+1 cells a side around every seed; each
    // pass is a prefix count per line, so the cost is the grid's size
    // whatever the reach, and a line with no seed is skipped.
    int dil = static_cast<int>(ceilf(g_reachM / cell));
    dil = dil < 1 ? 1 : (dil > gn - 1 ? gn - 1 : dil);
    {
        uint16_t cnt[kObjectGrid + 1];
        const int strides[3] = {1, gn, gn * gn};
        for (int axis = 0; axis < 3; ++axis) {
            const int s = strides[axis];
            const int s1 = strides[(axis + 1) % 3];
            const int s2 = strides[(axis + 2) % 3];
            for (int a = 0; a < gn; ++a) {
                for (int b = 0; b < gn; ++b) {
                    uint8_t* base = g_grid + a * s1 + b * s2;
                    cnt[0] = 0;
                    for (int i = 0; i < gn; ++i) {
                        cnt[i + 1] = static_cast<uint16_t>(cnt[i] + (base[i * s] ? 1u : 0u));
                    }
                    if (cnt[gn] == 0) continue;
                    for (int i = 0; i < gn; ++i) {
                        const int from = i - dil < 0 ? 0 : i - dil;
                        const int to = i + dil >= gn ? gn - 1 : i + dil;
                        base[i * s] = cnt[to + 1] - cnt[from] > 0 ? 255 : 0;
                    }
                }
            }
        }
    }
    // THE SECOND BODY's cells, over the body's (ObjectMotion::body2): a cube
    // of its reach around each of its parts, stamped 128 where the body's
    // dilation wrote 255. The body's seeds are its TYPES' records, and the
    // hub's parts are of the ring's types, so without this the hub's cells
    // were the ring's. The parts are few and close, so the cubes are
    // stamped outright rather than dilated.
    if (b2Pos && b2Count > 0) {
        const float reach2 = body2Reach(b2Pos, b2Count, cell);
        g_body2ReachM = reach2;
        int r2 = static_cast<int>(ceilf(reach2 / cell));
        r2 = r2 < 1 ? 1 : (r2 > 8 ? 8 : r2);
        for (int m = 0; m < b2Count; ++m) {
            const float* p = b2Pos + m * 3;
            bool inBox = true;
            int c[3];
            for (int k = 0; k < 3; ++k) {
                if (p[k] < lo[k] || p[k] >= hi[k]) inBox = false;
                int idx = static_cast<int>((p[k] - lo[k]) / cell);
                c[k] = idx < 0 ? 0 : (idx >= gn ? gn - 1 : idx);
            }
            if (!inBox) continue;
            for (int z = c[2] - r2; z <= c[2] + r2; ++z) {
                if (z < 0 || z >= gn) continue;
                for (int y = c[1] - r2; y <= c[1] + r2; ++y) {
                    if (y < 0 || y >= gn) continue;
                    uint8_t* row = g_grid + (z * gn + y) * gn;
                    const int x0 = c[0] - r2 < 0 ? 0 : c[0] - r2;
                    const int x1 = c[0] + r2 >= gn ? gn - 1 : c[0] + r2;
                    for (int x = x0; x <= x1; ++x) row[x] = 128;
                }
            }
        }
    }
    ++g_gridVersion;
    g_motion.gridVersion = g_gridVersion;
    g_motion.grid = g_grid;
}
uint64_t g_checkMs = 0;
ID3D11Buffer* g_pool = nullptr;      // held (AddRef) while recognised
uint32_t g_poolBytes = 0;
uint32_t g_records = 0;
ID3D11Buffer* g_scene = nullptr;     // the scene block bound with it, held likewise
uint32_t g_sceneBytes = 0;
bool     g_noted = false;
uint32_t g_frame = 0;
uint32_t g_framesWithoutPool = 0;
bool     g_absentNoted = false;
uint64_t g_dumpMs = 0;
uint32_t g_dumps = 0;
bool     g_dumpDirMade = false;

struct Slot {
    ID3D11Buffer* staging = nullptr;
    ID3D11Buffer* sceneStaging = nullptr;
    uint32_t bytes = 0;
    uint32_t sceneBytes = 0;
    uint32_t frame = 0;
    double   stampMs = 0.0;  // when the copy was issued, QPC milliseconds: the pair's interval is the two stamps' difference
    float    camPass[3] = {};  // the pass's chosen camera when the copy was issued (objectProbeNoteCamera)
    bool     camPassValid = false;
    bool     inUse = false;
    uint8_t  role = 0;       // 0 a frame's own copy (the stepped parts: trackFrame), 1 the pair's first (kept), 2 its second (diffed)
};
float g_camPass[3] = {};
bool  g_camPassValid = false;
Slot g_ring[kRing];
std::vector<uint8_t> g_keep;        // the first frame of a pair, copied out of its staging buffer
std::vector<uint8_t> g_keepScene;   // ...and its scene block
uint32_t g_keepFrame = 0;
double   g_keepStamp = 0.0;
bool     g_keepValid = false;

// The clock for the pair's interval: QPC, not the millisecond tick. The
// tick's 15.6 ms grain read a 90 Hz frame as 15 ms on two pairs in three
// (the body path's third flight, 2026-09-08 16:11), and the rate handed
// to the pass swung by a quarter from pair to pair -- the rim's textures
// vibrating every few frames, and the turn stuttering.
double qpcMs() {
    LARGE_INTEGER c{}, f{};
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return f.QuadPart > 0 ? static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
}

// The interval's figures.
uint64_t g_pairs = 0, g_skipped = 0;
uint64_t g_live = 0;                       // non-empty records, summed over pairs
uint64_t g_sigUnique = 0, g_twins = 0;     // ...with a signature no other has; byte-for-byte twins
uint64_t g_changed = 0, g_poseChanged = 0, g_otherOnly = 0, g_moved = 0;
uint64_t g_allocated = 0, g_freed = 0;
uint64_t g_sigKept = 0, g_sigMoved = 0, g_sigNew = 0;
uint64_t g_twinQuatPrev = 0, g_twinPosPrev = 0, g_twinScalePrev = 0;
uint64_t g_twinQuatSelf = 0, g_twinPosSelf = 0;
uint32_t g_maxChanged = 0;
uint64_t g_clusterSum = 0;
uint32_t g_clusterMax = 0;
uint64_t g_clusterOverflow = 0;
uint64_t g_bigPairs = 0;                   // pairs with any pose change (a largest cluster exists)
double   g_bigShareSum = 0.0, g_bigAngleSum = 0.0, g_bigTransSum = 0.0;
uint64_t g_commonPairs = 0;
uint64_t g_outsideSum = 0, g_secondSum = 0;
uint64_t g_shuffled = 0;                   // pose changes in a repacked slot, left unclustered (diffPair says)
double   g_diffMsSum = 0.0, g_diffMsMax = 0.0;   // the diff's own time on the render thread, a pair
uint32_t g_diffN = 0;
uint64_t g_rebasePairs = 0;
double   g_rebaseMaxM = 0.0;
uint32_t g_poolChanges = 0;
uint64_t g_reportMs = 0;
// Which bytes of a rewritten record changed, summed over the interval's
// changed records (the first flight, 2026-09-08: every changed record was
// "rewritten", so the fields the game touches per frame have to be learned
// before an identity can be keyed on the ones it does not).
uint64_t g_byteHist[kRecordBytes] = {};
uint64_t g_byteHistN = 0;

FaultBudget g_budget("objectProbe", 5);

// a * conj(b): the rotation taking b's frame to a's.
void quatMulConj(const float a[4], const float b[4], float out[4]) {
    const float bx = -b[0], by = -b[1], bz = -b[2], bw = b[3];
    out[3] = a[3] * bw - a[0] * bx - a[1] * by - a[2] * bz;
    out[0] = a[3] * bx + a[0] * bw + a[1] * bz - a[2] * by;
    out[1] = a[3] * by - a[0] * bz + a[1] * bw + a[2] * bx;
    out[2] = a[3] * bz + a[0] * by - a[1] * bx + a[2] * bw;
}

// v rotated by q: v + 2w (q x v) + 2 q x (q x v), the expansion the
// game's shader builds (fss_panel_vs.h edvrQuatRotate).
void quatRotate(const float q[4], const float v[3], float out[3]) {
    const float c[3] = {q[1] * v[2] - q[2] * v[1], q[2] * v[0] - q[0] * v[2],
                        q[0] * v[1] - q[1] * v[0]};
    const float cc[3] = {q[1] * c[2] - q[2] * c[1], q[2] * c[0] - q[0] * c[2],
                         q[0] * c[1] - q[1] * c[0]};
    for (int i = 0; i < 3; ++i) out[i] = v[i] + 2.0f * (q[3] * c[i] + cc[i]);
}

uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// The record's stable bytes, the identity's key. Read off the second flight
// of the probe (2026-09-08 11:36, the byte histogram): the pose at 8-27 and
// a twin of it at 288-319 change on nearly every rewritten record in flight,
// 0-7 and 30-55 on some, and these never -- so a record whose bytes here
// match last frame's is the same object with a new pose, whichever slot it
// sits in.
struct ByteRange { uint32_t b, e; };
constexpr ByteRange kSigRanges[] = {{28, 30}, {31, 32}, {56, 288}, {304, 308}, {320, 336}};

uint64_t signatureOf(const uint8_t* r) {
    uint64_t h = 1469598103934665603ull;
    for (const ByteRange& g : kSigRanges) {
        for (uint32_t k = g.b; k < g.e; ++k) {
            h ^= r[k];
            h *= 1099511628211ull;
        }
    }
    return h;
}

// One rigid motion D = W_prev * W_now^-1: the rotation taking now's frame to
// prev's and the translation p_prev - R p_now, the same for every part of
// one rigid assembly (the design's arithmetic). Clustered within a tolerance
// rather than bucketed by a quantised key: the unorm16 quaternion's quantum
// is 0.0035 deg, and at a kilometre that is six centimetres of translation,
// so the first flight's exact keys at a centimetre split one motion across
// more buckets than the table had (64 of 64, every interval in flight).
// 256 since 2026-09-09 (from 64): the table filled every pair -- "64 at
// most (and more past the table)" on every report -- and a ship whose
// first part came after it filled was invisible that pair. Most of the
// filling was records in a repacked slot, which diffPair no longer
// clusters; the rest is headroom, and the angle test below is a dot
// against a cosine so a table this size costs nothing to walk.
constexpr int   kMaxClusters = 256;
// 0.01 deg: three quanta. It was 0.03 on the body path's first two flights
// and that is WIDER than a station's turn (0.021-0.043 deg a frame), so
// every static thing in view merged into the station's cluster -- a box
// fifteen kilometres across, and a representative that was whichever
// record came first: 0.012 deg one pair, 0.000 deg and 1.57 m the next,
// handed to the pass as the station and re-registering its pixels by a
// pixel or two every eighth frame. That was the shimmer.
// 0.02 deg since 2026-09-09 14:28, from 0.01: with the test in rotation
// vectors the quantisation of the two quaternions a delta is made of (a
// half quantum each, per component, on both) reaches a hundredth of a
// degree between two parts of one body often enough that a station's parts
// still split into 95-150 clusters a pair; two hundredths keeps them
// together and is still under a station's own turn (0.035-0.045 deg), so
// a part that does not turn with it stays apart.
constexpr float kClusterAngleDeg = 0.02f;
// The tolerance as the squared distance between two small rotations'
// quaternion xyz parts: |q1.xyz - q2.xyz| is half the angle between them,
// in radians, to the angle squared.
constexpr float kClusterHalfRad = kClusterAngleDeg * 0.5f * 0.01745329f;
constexpr float kClusterHalfRad2 = kClusterHalfRad * kClusterHalfRad;
// Five centimetres plus four tenths of a millimetre a metre of the part's
// distance from the camera (the term's reference since 2026-09-09; rigidDelta
// says). The quaternion's quantum is a tenth of a millimetre a metre at
// worst -- two quantised quaternions in a delta, three components each --
// and a tolerance equal to the worst case split a station's parts down
// the middle on every flight (the largest cluster 14-57% of the pose
// changes, 135-239 clusters a pair on the flight of 14:52); four times
// it keeps a body together, and the angle test still parts a ship that
// does not turn with it.
constexpr float kClusterPosM = 0.05f;
constexpr float kClusterPosPerM = 4.0e-4f;
// The body's fit is ROBUST: members whose residual under the fit exceeds
// this (or three times the fit's rms) are dropped and the fit repeated.
// The clusters admit slot shuffles between neighbouring ring parts -- the
// per-slot delta of two parts that swapped slots is a turn about the
// station's own axis by their angular spacing, within the angle tolerance
// when the spacing is small and within the distance-scaled position
// tolerance ten kilometres off -- and the flight of 2026-09-08 17:08 read
// the station's translation term at 0.35, 0.49 and then 1.26 m from pair
// to pair with the residual tripled: the fit pulled by shuffles, and the
// panels at the slot "crisp for a few frames, then jerk and blur".
constexpr float    kFitTrimM = 0.25f;
constexpr int      kFitPasses = 3;

struct Cluster {
    float    q[4];       // the running MEAN delta, normalised: a member's noise averages out
    float    t[3];
    double   qSum[4];
    double   tSum[3];
    uint32_t count;
    double   angleSum;
    double   transSum;
};

// The translation term is taken about REF -- the camera, when the pair has
// one -- and not the world origin: t = prev - R (now - ref) - ref, with
// distM the part's distance from ref. The quaternion's quantum (0.0035 deg
// a component) on the lever arm from the origin is a metre at ten
// kilometres, the floating origin's reach, and it split a station's parts
// over a hundred clusters at the position tolerance even with the angle
// test fixed (the flight of 2026-09-09 14:06: 125-154 clusters a pair).
// About the camera the arm is the part's distance in view.
bool rigidDelta(const Pose& prev, const Pose& now, const float* ref, float qd[4], float t[3],
                float* angleDeg, float* transM, float* distM) {
    quatMulConj(prev.q, now.q, qd);
    if (qd[3] < 0.0f) {
        for (int i = 0; i < 4; ++i) qd[i] = -qd[i];
    }
    // The angle from the xyz part against w, not from acos(w): acos at
    // w = 1 - 4e-9 (a hundredth of a degree) sees 1.0 in float and says
    // nought, and at the next float below one says 0.04 deg, so every
    // small turn read as one of a few values (the review of 2026-09-09).
    const float xyz = sqrtf(qd[0] * qd[0] + qd[1] * qd[1] + qd[2] * qd[2]);
    *angleDeg = 2.0f * atan2f(xyz, qd[3]) * 57.2957795f;
    float rel[3], rp[3];
    for (int i = 0; i < 3; ++i) rel[i] = now.p[i] - ref[i];
    quatRotate(qd, rel, rp);
    for (int i = 0; i < 3; ++i) t[i] = prev.p[i] - rp[i] - ref[i];
    *transM = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    *distM = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
    return std::isfinite(*angleDeg) && std::isfinite(*transM) && std::isfinite(*distM);
}

// The cluster this motion belongs to, made if none is near; -1 past the table.
int clusterOf(Cluster* cs, int* n, const float qd[4], const float t[3], float angleDeg,
              float transM, float distM) {
    const float posTol = kClusterPosM + kClusterPosPerM * distM;
    for (int j = 0; j < *n; ++j) {
        Cluster& c = cs[j];
        // The angle between two small rotations as the distance between
        // their quaternions' xyz parts (both with w >= 0, rigidDelta's
        // doing): half the angle in radians, and the float keeps every
        // quantum of it. A dot of unit quaternions cannot: at a hundredth
        // of a degree the dot is 1 - 4e-9, under the float's own step
        // below one (6e-8), so the old tests -- acos of the dot, then the
        // dot against a cosine -- passed a record when its dot rounded to
        // 1.0 and failed it otherwise, luck per record per pair, and a
        // station's parts split into a hundred or two clusters of one (the
        // review of 2026-09-09 emulated it: 51-60% in the largest cluster
        // at a station's rate, 135-194 clusters).
        const float qx = qd[0] - c.q[0], qy = qd[1] - c.q[1], qz = qd[2] - c.q[2];
        if (qx * qx + qy * qy + qz * qz > kClusterHalfRad2) continue;
        const float dx = t[0] - c.t[0], dy = t[1] - c.t[1], dz = t[2] - c.t[2];
        if (sqrtf(dx * dx + dy * dy + dz * dz) > posTol) continue;
        ++c.count;
        c.angleSum += angleDeg;
        c.transSum += transM;
        // The mean, the sign of the quaternion aligned to the cluster's.
        float dotRaw = 0.0f;
        for (int i = 0; i < 4; ++i) dotRaw += qd[i] * c.q[i];
        const double sg = dotRaw < 0.0f ? -1.0 : 1.0;
        for (int i = 0; i < 4; ++i) c.qSum[i] += sg * qd[i];
        for (int i = 0; i < 3; ++i) c.tSum[i] += t[i];
        double qn = 0.0;
        for (int i = 0; i < 4; ++i) qn += c.qSum[i] * c.qSum[i];
        qn = sqrt(qn);
        if (qn > 1e-12) {
            for (int i = 0; i < 4; ++i) c.q[i] = static_cast<float>(c.qSum[i] / qn);
        }
        for (int i = 0; i < 3; ++i) c.t[i] = static_cast<float>(c.tSum[i] / c.count);
        return j;
    }
    if (*n >= kMaxClusters) return -1;
    Cluster& c = cs[*n];
    memcpy(c.q, qd, sizeof(c.q));
    memcpy(c.t, t, sizeof(c.t));
    for (int i = 0; i < 4; ++i) c.qSum[i] = qd[i];
    for (int i = 0; i < 3; ++i) c.tSum[i] = t[i];
    c.count = 1;
    c.angleSum = angleDeg;
    c.transSum = transM;
    return (*n)++;
}

bool emptyRecord(const uint8_t* r) {
    for (uint32_t i = 0; i < kRecordBytes; ++i) {
        if (r[i]) return false;
    }
    return true;
}

// The pair's clock interval, eased: the game's step per frame is steady
// where the probe's own clock on the copies is not (8.8 to 12.6 ms for
// the same turn on the flight of 2026-09-09 09:52), and a ship's rate has
// no sixteen-pair median to hide that in -- its motion changes from pair
// to pair, and each pair's own must serve. The median of the last sixteen
// pairs' intervals is the frame's own length, and a ship's rate is the
// pair's delta over that.
float easedPairDt(float dtMs) {
    if (dtMs >= 5.0f && dtMs <= 50.0f) {
        g_dtRing[g_dtRingN % kDtRing] = dtMs;
        ++g_dtRingN;
    }
    const uint32_t cnt = g_dtRingN < kDtRing ? g_dtRingN : kDtRing;
    if (cnt == 0) return 11.1f;
    float v[kDtRing];
    memcpy(v, g_dtRing, sizeof(float) * cnt);
    for (uint32_t i = 1; i < cnt; ++i) {   // an insertion sort: sixteen at most
        const float x = v[i];
        uint32_t j = i;
        while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; --j; }
        v[j] = x;
    }
    return v[cnt / 2];
}

// THE MOVING SHIPS (object_probe.h; 2026-09-09): every other rigid
// cluster of the pair that moves, near enough, as a body of its own.
// Which: not the dominant body's cluster (the station, diffPair's); not a
// slice of it -- a cluster within a few tolerances of the station's own
// delta, its parts split from the station's cluster by the quaternion's
// quantum on their lever arm from the origin, whose pixels the station's
// grid claims already; not standing still, which is the world path's;
// and not the player's own parts, within kShipRadiusM of the camera,
// which move with it. Each is fitted as the station is (the least-squares
// rigid motion over its parts, the residual saying they moved as one) and
// keeps its own rate: a ship's changes, so no median across pairs, and
// the eased interval in place of the pair's own noisy one. On a rebase
// pair (every live record shifted together) nothing is taken: the deltas
// are the origin's. The nearest kObjectShipsMax are kept, nearest first,
// and replace the last pair's; a pair that finds none leaves the last
// pair's to age out (kShipHoldFrames). Since the review of 2026-09-09 the
// tests are on the FIT: the centroid's own motion for own, still and
// tail, and the body's fitted motion for the slice test (the body's
// cluster is skipped only when it was taken as the body).
// The rigid fit with the body's trim (fit, drop what the fit does not
// explain, fit again), for the second body: the members in, the survivors
// out, false under kBody2MinRecords of them.
bool fitTrimmed(std::vector<int>& members, const std::vector<float>& posNow,
                const std::vector<float>& posPrev, float w[3], float tf[3], float* rms) {
    std::vector<float> fitNow, fitPrev;
    fitNow.reserve(members.size() * 3);
    fitPrev.reserve(members.size() * 3);
    for (int m : members) {
        for (int k = 0; k < 3; ++k) {
            fitNow.push_back(posNow[static_cast<size_t>(m) * 3 + k]);
            fitPrev.push_back(posPrev[static_cast<size_t>(m) * 3 + k]);
        }
    }
    bool ok = false;
    for (int pass = 0; pass < kFitPasses; ++pass) {
        ok = temporalRigidFit(fitNow.data(), fitPrev.data(), static_cast<int>(members.size()), w, tf, rms);
        if (!ok || pass + 1 == kFitPasses) break;
        float R[9];
        temporalRodrigues(w, R);
        const float lim = *rms * 3.0f > kFitTrimM ? *rms * 3.0f : kFitTrimM;
        std::vector<int> keptMembers;
        std::vector<float> keptNow, keptPrev;
        for (size_t m = 0; m < members.size(); ++m) {
            float q[3];
            temporalApply3(R, &fitNow[m * 3], q);
            float e2 = 0.0f;
            for (int k = 0; k < 3; ++k) {
                const float e = q[k] + tf[k] - fitPrev[m * 3 + k];
                e2 += e * e;
            }
            if (e2 > lim * lim) continue;
            keptMembers.push_back(members[m]);
            for (int k = 0; k < 3; ++k) {
                keptNow.push_back(fitNow[m * 3 + k]);
                keptPrev.push_back(fitPrev[m * 3 + k]);
            }
        }
        if (keptMembers.size() == members.size()) break;
        if (keptMembers.size() < kBody2MinRecords) return false;
        members.swap(keptMembers);
        fitNow.swap(keptNow);
        fitPrev.swap(keptPrev);
    }
    return ok && members.size() >= kBody2MinRecords;
}

// The rolling median of a rate ring (diffPair says why a median): the axis
// the ring's sum, the magnitude the median's, each translation component
// its own median. Returns the median magnitude, for the outlier test; the
// axis is left as it was when the ring sums to nothing.
float rateMedian(const RatePair* ring, uint32_t count, float omega[3], float t[3]) {
    float mags[kRateRing];
    float axis[3] = {0.0f, 0.0f, 0.0f};
    for (uint32_t i = 0; i < count; ++i) {
        const float* rw = ring[i].w;
        mags[i] = sqrtf(rw[0] * rw[0] + rw[1] * rw[1] + rw[2] * rw[2]);
        for (int k = 0; k < 3; ++k) axis[k] += rw[k];
    }
    for (uint32_t i = 1; i < count; ++i) {   // an insertion sort: sixteen at most
        const float v = mags[i];
        uint32_t jj = i;
        while (jj > 0 && mags[jj - 1] > v) { mags[jj] = mags[jj - 1]; --jj; }
        mags[jj] = v;
    }
    const float medMag = mags[count / 2];
    const float an = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (an > 0.0f) {
        for (int k = 0; k < 3; ++k) omega[k] = axis[k] / an * medMag;
    }
    for (int k = 0; k < 3; ++k) {
        float comps[kRateRing];
        for (uint32_t i = 0; i < count; ++i) comps[i] = ring[i].t[k];
        for (uint32_t i = 1; i < count; ++i) {
            const float v = comps[i];
            uint32_t jj = i;
            while (jj > 0 && comps[jj - 1] > v) { comps[jj] = comps[jj - 1]; --jj; }
            comps[jj] = v;
        }
        t[k] = comps[count / 2];
    }
    return medMag;
}

void takeShips(const Cluster* clusters, int nc, int big, bool bodyTaken, int skip2, const float* bodyW,
               const float* bodyT, const std::vector<int>& clusterIdx,
               const std::vector<float>& posNow, const std::vector<float>& posPrev, uint32_t n,
               const float* camPos, float dtMs, uint32_t live) {
    if (g_shipRangeM <= 0.0f || !camPos || nc <= 0) return;
    const float dt = easedPairDt(dtMs);
    if (big >= 0) {
        // A rebase pair: the live records shifted together, without a turn,
        // by kilometres. Without the distance the player's own parts
        // qualified -- over half the live records when the pool holds
        // little else, moving four metres a frame -- and the ships' fifth
        // flight took none on those pairs.
        const Cluster& b = clusters[big];
        if (b.angleSum / b.count < static_cast<double>(kRotQuantDeg) && live && b.count * 2 > live &&
            b.transSum / b.count > 50.0) {
            return;
        }
    }
    ++g_shipPairs;
    // The camera's displacement over the pair, now less last, for the
    // player's own parts: their centroid moves with it (to the head's few
    // centimetres), wherever they sit.
    float camDisp[3] = {0.0f, 0.0f, 0.0f};
    bool haveCam = false;
    if (g_pairCamPrevValid) {
        for (int k = 0; k < 3; ++k) camDisp[k] = camPos[k] - g_pairCamPrev[k];
        haveCam = true;
    }
    const float camSpeed = sqrtf(camDisp[0] * camDisp[0] + camDisp[1] * camDisp[1] + camDisp[2] * camDisp[2]);
    ObjectShip found[kObjectShipsMax];
    uint32_t nf = 0;
    std::vector<int> members;
    std::vector<float> fitNow, fitPrev;
    // The records by cluster, gathered once (a counting sort): a scan of
    // the pool per cluster was half a million steps a pair at 256 clusters.
    std::vector<uint32_t> firstOf(static_cast<size_t>(nc) + 1, 0u);
    for (uint32_t i = 0; i < n; ++i) {
        if (clusterIdx[i] >= 0) ++firstOf[static_cast<size_t>(clusterIdx[i]) + 1];
    }
    for (int j = 0; j < nc; ++j) firstOf[static_cast<size_t>(j) + 1] += firstOf[static_cast<size_t>(j)];
    std::vector<int> byCluster(firstOf[static_cast<size_t>(nc)]);
    {
        std::vector<uint32_t> fill(firstOf.begin(), firstOf.end() - 1);
        for (uint32_t i = 0; i < n; ++i) {
            if (clusterIdx[i] >= 0) byCluster[fill[static_cast<size_t>(clusterIdx[i])]++] = static_cast<int>(i);
        }
    }
    for (int j = 0; j < nc; ++j) {
        // The largest cluster is skipped only when it was taken as the body:
        // alone in open space with one other ship, the largest cluster IS
        // that ship (the review of 2026-09-09).
        if ((j == big && bodyTaken) || j == skip2) continue;   // the body's cluster, and the second body's
        const Cluster& c = clusters[j];
        if (c.count < kShipMinRecords) {
            ++g_shipsFew;
            continue;
        }
        const double angle = c.angleSum / c.count;
        if (angle > static_cast<double>(kMotionMaxDeg)) continue;   // a scatter, or a shuffle's turn
        members.clear();
        fitNow.clear();
        fitPrev.clear();
        float cen[3] = {0.0f, 0.0f, 0.0f};
        for (uint32_t bi = firstOf[static_cast<size_t>(j)]; bi < firstOf[static_cast<size_t>(j) + 1]; ++bi) {
            const uint32_t i = static_cast<uint32_t>(byCluster[bi]);
            const float* pn = &posNow[static_cast<size_t>(i) * 3];
            const float dx = pn[0] - camPos[0], dy = pn[1] - camPos[1], dz = pn[2] - camPos[2];
            if (dx * dx + dy * dy + dz * dz < kShipOwnRadiusM * kShipOwnRadiusM) continue;
            members.push_back(static_cast<int>(i));
            for (int k = 0; k < 3; ++k) {
                fitNow.push_back(pn[k]);
                fitPrev.push_back(posPrev[static_cast<size_t>(i) * 3 + k]);
                cen[k] += pn[k];
            }
        }
        if (members.size() < kShipMinRecords) {
            ++g_shipsFew;
            continue;
        }
        for (int k = 0; k < 3; ++k) cen[k] /= static_cast<float>(members.size());
        const float cx = cen[0] - camPos[0], cy = cen[1] - camPos[1], cz = cen[2] - camPos[2];
        const float dist = sqrtf(cx * cx + cy * cy + cz * cz);
        if (dist > g_shipRangeM) {
            ++g_shipsOutOfRange;
            continue;
        }
        float w[3] = {}, tf[3] = {}, rms = 0.0f;
        if (!temporalRigidFit(fitNow.data(), fitPrev.data(), static_cast<int>(members.size()), w, tf, &rms) ||
            rms > kShipMaxRmsM) {
            ++g_shipsUnfit;
            continue;
        }
        // What the ship DID over the pair: its centroid's motion, now less
        // last, which is -((R - I) cen + t) = -(w x cen + t) to the angle
        // squared. The fit's t alone is the rigid motion's term about the
        // world origin and carries (I - R) times the parts' distance from
        // the floating origin -- metres a frame for a turning body
        // kilometres out, in a direction the body does not move (the
        // review of 2026-09-09: the player's own hull read as a ship moving
        // 13.6 m a frame on every turn) -- so the own, still, speed and
        // tail tests take the centroid's motion, and the path keeps t.
        const float wxc[3] = {w[1] * cen[2] - w[2] * cen[1], w[2] * cen[0] - w[0] * cen[2],
                              w[0] * cen[1] - w[1] * cen[0]};
        float move[3];
        for (int k = 0; k < 3; ++k) move[k] = -(wxc[k] + tf[k]);
        const float speed = sqrtf(move[0] * move[0] + move[1] * move[1] + move[2] * move[2]);
        if (speed > kMotionMaxM) continue;   // twenty metres a pair: nothing that flies
        const float fitDeg = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * 57.2957795f;
        if (fitDeg < kMotionMinDeg && speed < kShipMinMoveM) {
            ++g_shipsStill;
            continue;
        }
        // A slice of the body: the body's own rigid motion, turn and term
        // alike, to a quarter of the turn and the position tolerance. The
        // old test compared the clusters' mean quaternions by their dot,
        // which the float cannot resolve at these angles (clusterOf says),
        // and half of the station's splinters passed as ships.
        if (bodyW && bodyT) {
            const float bwn = sqrtf(bodyW[0] * bodyW[0] + bodyW[1] * bodyW[1] + bodyW[2] * bodyW[2]);
            const float ex = w[0] - bodyW[0], ey = w[1] - bodyW[1], ez = w[2] - bodyW[2];
            const float tx = tf[0] - bodyT[0], ty = tf[1] - bodyT[1], tz = tf[2] - bodyT[2];
            const float originM = sqrtf(cen[0] * cen[0] + cen[1] * cen[1] + cen[2] * cen[2]);
            const float posTol = kClusterPosM + kClusterPosPerM * originM;
            if (sqrtf(ex * ex + ey * ey + ez * ez) <= 0.25f * bwn + 6.0e-5f &&
                sqrtf(tx * tx + ty * ty + tz * tz) <= 0.5f + 4.0f * posTol) {
                ++g_shipsSlices;
                continue;
            }
        }
        // The player's own parts: their centroid moves with the camera.
        if (haveCam) {
            const float ex = move[0] - camDisp[0], ey = move[1] - camDisp[1], ez = move[2] - camDisp[2];
            if (sqrtf(ex * ex + ey * ey + ez * ez) <= 0.5f + 0.05f * camSpeed) {
                ++g_shipsOwn;
                continue;
            }
        }
        ObjectShip sh = {};
        for (int k = 0; k < 3; ++k) {
            sh.omegaPerMs[k] = w[k] / dt;
            sh.tPerMs[k] = tf[k] / dt;
            sh.movePerMs[k] = move[k] / dt;
            sh.bmin[k] = 1e30f;
            sh.bmax[k] = -1e30f;
        }
        for (int m : members) {
            const float* pm = &posNow[static_cast<size_t>(m) * 3];
            for (int k = 0; k < 3; ++k) {
                if (pm[k] - kShipPadM < sh.bmin[k]) sh.bmin[k] = pm[k] - kShipPadM;
                if (pm[k] + kShipPadM > sh.bmax[k]) sh.bmax[k] = pm[k] + kShipPadM;
            }
        }
        sh.distM = dist;
        sh.rms = rms;
        sh.records = static_cast<uint32_t>(members.size());
        // The tail and the parts (ObjectShip says): the way it flies is its
        // centroid's motion; the rearmost part is the one farthest back
        // along it.
        sh.dir[0] = sh.dir[1] = sh.dir[2] = 0.0f;
        sh.rear = -1e30f;
        if (speed >= kShipTailSpeedM) {
            for (int k = 0; k < 3; ++k) sh.dir[k] = move[k] / speed;
            float rearAlong = 1e30f;
            for (int m : members) {
                const float* pm = &posNow[static_cast<size_t>(m) * 3];
                const float a = pm[0] * sh.dir[0] + pm[1] * sh.dir[1] + pm[2] * sh.dir[2];
                if (a < rearAlong) rearAlong = a;
            }
            sh.rear = rearAlong - kShipTailM;
        }
        const size_t stride = (members.size() + kObjectShipParts - 1) / kObjectShipParts;
        sh.partCount = 0;
        for (size_t m = 0; m < members.size() && sh.partCount < kObjectShipParts; m += stride) {
            memcpy(sh.parts[sh.partCount], &posNow[static_cast<size_t>(members[m]) * 3], sizeof(float) * 3);
            ++sh.partCount;
        }
        // Nearest first; past the table the farthest yields.
        if (nf < kObjectShipsMax) {
            found[nf++] = sh;
        } else {
            uint32_t farthest = 0;
            for (uint32_t f = 1; f < nf; ++f) {
                if (found[f].distM > found[farthest].distM) farthest = f;
            }
            if (dist < found[farthest].distM) found[farthest] = sh;
            ++g_shipsOverCap;
        }
    }
    if (!nf) return;
    for (uint32_t i = 1; i < nf; ++i) {   // an insertion sort, nearest first
        const ObjectShip x = found[i];
        uint32_t j = i;
        while (j > 0 && found[j - 1].distM > x.distM) { found[j] = found[j - 1]; --j; }
        found[j] = x;
    }
    {
        std::lock_guard<std::mutex> lk(g_publish);
        memcpy(g_ships, found, sizeof(ObjectShip) * nf);
        g_shipCount = nf;
        g_shipAge = g_pairLagFrames;   // the parts' positions are the copy's frame's, three or more frames ago
        memcpy(g_shipCamPos, camPos, sizeof(g_shipCamPos));
    }
    g_shipsTaken += nf;
    if (nf > g_shipsMax) g_shipsMax = nf;
    if (dueMs(g_shipNoteMs, 30000)) {
        g_shipNoteMs = stampMs();   // dueMs reads the stamp; it does not set it (the first flight noted every pair)
        const ObjectShip& s0 = found[0];
        const float* ow = s0.omegaPerMs;
        const float* om = s0.movePerMs;
        Log::get().note(
            "object probe: %u moving ship%s within %.0f m -- the nearest %u parts at %.0f m, fit to "
            "%.3f m, turning %.3f deg and moving %.2f m a frame of %.1f ms, its box %.0f x %.0f x "
            "%.0f m. %llu taken so far.",
            nf, nf == 1 ? "" : "s", static_cast<double>(g_shipRangeM), s0.records,
            static_cast<double>(s0.distM), static_cast<double>(s0.rms),
            static_cast<double>(sqrtf(ow[0] * ow[0] + ow[1] * ow[1] + ow[2] * ow[2]) * dt * 57.2957795f),
            static_cast<double>(sqrtf(om[0] * om[0] + om[1] * om[1] + om[2] * om[2]) * dt),
            static_cast<double>(dt), static_cast<double>(s0.bmax[0] - s0.bmin[0]),
            static_cast<double>(s0.bmax[1] - s0.bmin[1]), static_cast<double>(s0.bmax[2] - s0.bmin[2]),
            static_cast<unsigned long long>(g_shipsTaken));
    }
}

// The pair's diff: one sampled frame against the one before it. An empty
// (all-zero) slot is nobody: a slot freed to zeros matched every other
// empty slot as "moved" on the first flight and inflated that figure a
// hundredfold, so allocation and freeing are counted on their own and only
// live records take part in the rest. A record whose pose bytes changed is
// a pose change whatever else in it changed -- the game rewrites more than
// the pose each frame, and the byte histogram says which fields. The
// second flight added the identity by signature, the second pose block's
// provenance and the tolerance clustering.
void diffPair(const uint8_t* prev, const uint8_t* now, uint32_t bytes, float dtMs,
              const float* camPos) {
    const uint32_t n = bytes / kRecordBytes;
    std::vector<uint64_t> hashNow(n, 0), sigPrev(n, 0), sigNow(n, 0);
    std::vector<uint8_t> livePrev(n, 0), liveNow(n, 0);
    std::unordered_map<uint64_t, uint32_t> prevByHash, prevSigCount, nowSigCount, nowHashCount;
    prevByHash.reserve(n);
    prevSigCount.reserve(n);
    nowSigCount.reserve(n);
    nowHashCount.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* a = prev + i * kRecordBytes;
        const uint8_t* b = now + i * kRecordBytes;
        if (!emptyRecord(a)) {
            livePrev[i] = 1;
            sigPrev[i] = signatureOf(a);
            prevByHash[fnv1a(a, kRecordBytes)] = i;
            ++prevSigCount[sigPrev[i]];
        }
        if (!emptyRecord(b)) {
            liveNow[i] = 1;
            hashNow[i] = fnv1a(b, kRecordBytes);
            sigNow[i] = signatureOf(b);
            ++nowHashCount[hashNow[i]];
            ++nowSigCount[sigNow[i]];
        }
    }

    uint32_t live = 0, sigUnique = 0, twins = 0;
    uint32_t changed = 0, poseChanged = 0, otherOnly = 0, moved = 0, shuffled = 0;
    uint32_t allocated = 0, freed = 0;
    uint32_t sigKept = 0, sigMoved = 0, sigNew = 0;
    uint32_t twinQuatPrev = 0, twinPosPrev = 0, twinScalePrev = 0, twinQuatSelf = 0, twinPosSelf = 0;
    Cluster clusters[kMaxClusters];
    int nc = 0;
    uint32_t overflow = 0;
    // The held body as the render thread has it, taken once: whether one
    // is published and how old it is (the age is the render thread's count,
    // under g_publish).
    bool heldValid = false;
    uint32_t heldAge = 0;
    {
        std::lock_guard<std::mutex> lk(g_publish);
        heldValid = g_motionPubValid;
        heldAge = g_motionAge;
    }
    // Each pose change's cluster and positions, for the body's fit and grid.
    // The deltas' translation term is about the camera (rigidDelta says).
    const float zeroRef[3] = {0.0f, 0.0f, 0.0f};
    const float* deltaRef = camPos ? camPos : zeroRef;
    std::vector<int> clusterIdx(n, -1);
    std::vector<float> posNow(static_cast<size_t>(n) * 3, 0.0f);
    std::vector<float> posPrev(static_cast<size_t>(n) * 3, 0.0f);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* a = prev + i * kRecordBytes;
        const uint8_t* b = now + i * kRecordBytes;
        if (liveNow[i]) {
            ++live;
            if (nowSigCount[sigNow[i]] == 1) ++sigUnique;
            if (nowHashCount[hashNow[i]] > 1) ++twins;
        }
        if (memcmp(a, b, kRecordBytes) == 0) continue;
        if (!liveNow[i]) { ++freed; continue; }
        if (!livePrev[i]) { ++allocated; continue; }
        ++changed;
        for (uint32_t k = 0; k < kRecordBytes; ++k) {
            if (a[k] != b[k]) ++g_byteHist[k];
        }
        ++g_byteHistN;
        // The identity: the same signature at the same slot is the same
        // object moved; the signature at another slot last frame is a
        // repacked pool; neither is a new object, or one whose "stable"
        // bytes were not.
        if (sigNow[i] == sigPrev[i]) {
            ++sigKept;
        } else if (prevSigCount.count(sigNow[i])) {
            ++sigMoved;
        } else {
            ++sigNew;
        }
        // The second pose block (288-319, byte for byte the first block's
        // change pattern on the 11:36 flight) against last frame's first
        // block at this slot, and against this frame's own: last frame's
        // pose in the record would be the game's own motion source, and
        // the per-object motion in hand without any identity at all.
        if (memcmp(b + 312, a + 8, 8) == 0) ++twinQuatPrev;
        if (memcmp(b + 292, a + 16, 12) == 0) ++twinPosPrev;
        if (memcmp(b + 288, a + 4, 4) == 0) ++twinScalePrev;
        if (memcmp(b + 312, b + 8, 8) == 0) ++twinQuatSelf;
        if (memcmp(b + 292, b + 16, 12) == 0) ++twinPosSelf;
        const bool poseDiff = memcmp(a + 4, b + 4, 24) != 0;
        if (poseDiff && sigNow[i] != sigPrev[i]) {
            // Another object in the slot (the pool repacked, sigMoved or
            // sigNew above): its delta is the difference of two objects'
            // poses, not a motion, and clustered it made a cluster of its
            // own per record -- sixty a pair on the ships' first flight
            // (2026-09-09 11:19), filling the table before a ship's parts
            // came. Counted, not clustered.
            ++shuffled;
        } else if (poseDiff) {
            ++poseChanged;
            float qd[4], t[3], angle = 0.0f, trans = 0.0f, dist = 0.0f;
            const Pose pa = decodePose(a);
            const Pose pb = decodePose(b);
            if (rigidDelta(pa, pb, deltaRef, qd, t, &angle, &trans, &dist)) {
                const int cj = clusterOf(clusters, &nc, qd, t, angle, trans, dist);
                if (cj < 0) {
                    ++overflow;
                } else {
                    clusterIdx[i] = cj;
                    memcpy(&posNow[static_cast<size_t>(i) * 3], pb.p, sizeof(pb.p));
                    memcpy(&posPrev[static_cast<size_t>(i) * 3], pa.p, sizeof(pa.p));
                }
            }
        } else {
            ++otherOnly;
        }
        // The same live bytes at another slot last frame, and that slot has
        // since changed: the record moved whole, which a pose that changes
        // every frame should make impossible -- unless the pool holds a
        // copy that lags a frame.
        auto it = prevByHash.find(hashNow[i]);
        if (it != prevByHash.end() && it->second != i) {
            const uint32_t j = it->second;
            if (memcmp(prev + j * kRecordBytes, now + j * kRecordBytes, kRecordBytes) != 0) ++moved;
        }
    }
    // The largest cluster: over half the pose changes is a common motion --
    // the frame's own, if the pool's poses are in the ship's frame and the
    // ship turned; without a turn and over half the LIVE records, an origin
    // rebase.
    int big = -1, second = -1;
    // The body's fit this pair, for the ships' slice test (takeShips): its
    // rigid motion, and whether it was taken as the body.
    float bodyW[3] = {0.0f, 0.0f, 0.0f}, bodyT[3] = {0.0f, 0.0f, 0.0f};
    bool bodyFit = false, bodyTaken = false;
    int body2Cluster = -1;   // the second body's cluster this pair, kept from the ships (takeShips)
    for (int j = 0; j < nc; ++j) {
        if (big < 0 || clusters[j].count > clusters[big].count) {
            second = big;
            big = j;
        } else if (second < 0 || clusters[j].count > clusters[second].count) {
            second = j;
        }
    }
    ++g_pairs;
    g_live += live;
    g_sigUnique += sigUnique;
    g_twins += twins;
    g_changed += changed;
    g_poseChanged += poseChanged;
    g_shuffled += shuffled;
    g_otherOnly += otherOnly;
    g_moved += moved;
    g_allocated += allocated;
    g_freed += freed;
    g_sigKept += sigKept;
    g_sigMoved += sigMoved;
    g_sigNew += sigNew;
    g_twinQuatPrev += twinQuatPrev;
    g_twinPosPrev += twinPosPrev;
    g_twinScalePrev += twinScalePrev;
    g_twinQuatSelf += twinQuatSelf;
    g_twinPosSelf += twinPosSelf;
    if (changed > g_maxChanged) g_maxChanged = changed;
    g_clusterSum += static_cast<uint64_t>(nc);
    if (static_cast<uint32_t>(nc) > g_clusterMax) g_clusterMax = static_cast<uint32_t>(nc);
    g_clusterOverflow += overflow;
    if (big >= 0 && poseChanged) {
        const Cluster& c = clusters[big];
        const double angle = c.angleSum / c.count;
        const double trans = c.transSum / c.count;
        // The body for the temporal pass: the cluster's own delta, as a
        // rotation matrix (the quaternion's, in quatRotate's convention:
        // p_prev = R p_now + t) -- when it is a body and not a scatter.
        const float share = static_cast<float>(c.count) / static_cast<float>(poseChanged);
        if (c.count >= kMotionMinRecords && share >= kMotionMinShare &&
            angle <= static_cast<double>(kMotionMaxDeg) && trans <= static_cast<double>(kMotionMaxM)) {
            // The body's motion from ALL its parts' positions, not one
            // record's quantised delta: the least-squares rigid fit, and
            // its residual says whether these parts moved as one.
            std::vector<int> members;
            members.reserve(c.count);
            std::vector<float> fitNow, fitPrev;
            fitNow.reserve(static_cast<size_t>(c.count) * 3);
            fitPrev.reserve(static_cast<size_t>(c.count) * 3);
            for (uint32_t i = 0; i < n; ++i) {
                if (clusterIdx[i] != big) continue;
                const float* pn = &posNow[static_cast<size_t>(i) * 3];
                if (camPos) {
                    // The player's own parts, co-rotating with a station
                    // inside its slot: near the camera, and not the body's.
                    const float dx = pn[0] - camPos[0], dy = pn[1] - camPos[1], dz = pn[2] - camPos[2];
                    if (dx * dx + dy * dy + dz * dz < kShipRadiusM * kShipRadiusM) continue;
                }
                members.push_back(static_cast<int>(i));
                for (int k = 0; k < 3; ++k) {
                    fitNow.push_back(pn[k]);
                    fitPrev.push_back(posPrev[static_cast<size_t>(i) * 3 + k]);
                }
            }
            // The robust fit: fit, drop what the fit does not explain, fit
            // again -- a shuffled slot is two objects' poses and sits metres
            // from any rigid motion of the rest.
            float w[3] = {}, tf[3] = {}, rms = 0.0f;
            bool fitOk = false;
            for (int pass = 0; pass < kFitPasses; ++pass) {
                fitOk = temporalRigidFit(fitNow.data(), fitPrev.data(), static_cast<int>(members.size()),
                                         w, tf, &rms);
                if (!fitOk || pass + 1 == kFitPasses) break;
                float R[9];
                temporalRodrigues(w, R);
                const float lim = rms * 3.0f > kFitTrimM ? rms * 3.0f : kFitTrimM;
                std::vector<int> keptMembers;
                std::vector<float> keptNow, keptPrev;
                keptMembers.reserve(members.size());
                keptNow.reserve(fitNow.size());
                keptPrev.reserve(fitPrev.size());
                for (size_t m = 0; m < members.size(); ++m) {
                    float q[3];
                    temporalApply3(R, &fitNow[m * 3], q);
                    float e2 = 0.0f;
                    for (int k = 0; k < 3; ++k) {
                        const float e = q[k] + tf[k] - fitPrev[m * 3 + k];
                        e2 += e * e;
                    }
                    if (e2 > lim * lim) continue;
                    keptMembers.push_back(members[m]);
                    for (int k = 0; k < 3; ++k) {
                        keptNow.push_back(fitNow[m * 3 + k]);
                        keptPrev.push_back(fitPrev[m * 3 + k]);
                    }
                }
                if (keptMembers.size() == members.size()) break;   // nothing to drop: the fit stands
                if (keptMembers.size() < kMotionMinRecords) { fitOk = false; break; }
                members.swap(keptMembers);
                fitNow.swap(keptNow);
                fitPrev.swap(keptPrev);
            }
            if (members.size() < kMotionMinRecords) fitOk = false;
            if (fitOk) {
                memcpy(bodyW, w, sizeof(bodyW));
                memcpy(bodyT, tf, sizeof(bodyT));
                bodyFit = true;
            }
            const float fitDeg = fitOk ? sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * 57.2957795f : 0.0f;
            const float fitM = fitOk ? sqrtf(tf[0] * tf[0] + tf[1] * tf[1] + tf[2] * tf[2]) : 0.0f;
            // A body for the pass TURNS (the feature is rotating stations; a
            // pure translation is another ship, or the player's own parts,
            // and hands the station a shift it never made), fits as one
            // rigid thing (the residual), and keeps a sane rate.
            const float dt = (dtMs >= 5.0f && dtMs <= 50.0f) ? dtMs : 11.1f;
            // The same body as the last pair's? Its parts' centroid, taken
            // relative to the camera so the floating origin's moves drop
            // out, sits where the last one's did to within two kilometres
            // while the ship flies -- and a pair whose largest rigid cluster
            // is another object (a ship's parts on a frame the station's
            // slots were shuffled), or a slice of the station at one end,
            // sits kilometres off. The player saw the whole station flash
            // from claimed to "outside the cells" for a split second on
            // 2026-09-09 (v0.14.1-89): a pair's box around something else.
            // Such a pair keeps the last body; only a body older than
            // kBodyContinuityFrames yields to it.
            bool sameBody = true;
            float relC[3] = {0.0f, 0.0f, 0.0f};
            if (fitOk && camPos && !members.empty()) {
                for (int m : members) {
                    for (int k = 0; k < 3; ++k) relC[k] += posNow[static_cast<size_t>(m) * 3 + k];
                }
                for (int k = 0; k < 3; ++k) relC[k] = relC[k] / static_cast<float>(members.size()) - camPos[k];
                if (heldValid && g_lastRelValid && heldAge < kBodyContinuityFrames) {
                    const float dx = relC[0] - g_lastRel[0], dy = relC[1] - g_lastRel[1], dz = relC[2] - g_lastRel[2];
                    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (dist > kBodyContinuityM) {
                        sameBody = false;
                        ++g_otherBodyPairs;
                        if (dueMs(g_otherBodyNoteMs, 30000)) {
                            g_otherBodyNoteMs = stampMs();
                            Log::get().note(
                                "object probe: a pair's body (%u parts, fit to %.3f m) sat %.0f m from the last "
                                "one's, camera-relative -- another object, or a slice of the station -- and the "
                                "last body is kept (%u frames old). %llu such pairs so far.",
                                static_cast<unsigned>(members.size()), static_cast<double>(rms),
                                static_cast<double>(dist), heldAge,
                                static_cast<unsigned long long>(g_otherBodyPairs));
                        }
                    }
                }
            }
            if (fitOk && sameBody && rms <= kMotionMaxRmsM && fitDeg >= kMotionMinDeg &&
                fitDeg <= kMotionMaxDeg && fitM <= kMotionMaxM) {
                if (camPos) {
                    memcpy(g_lastRel, relC, sizeof(g_lastRel));
                    g_lastRelValid = true;
                }
                // THE SECOND BODY (ObjectMotion::body2 says what it is): the
                // largest other cluster of kBody2MinRecords parts whose
                // centroid sits within kBody2NearM of the body's, camera-
                // relative -- the same structure, moving otherwise -- fitted
                // as the body is, its rates their own median, held over
                // kBody2HoldPairs pairs that do not find it (its cluster
                // comes and goes with the table's overflow) so its cells do
                // not flicker to the camera's path.
                bool found2 = false;
                if (camPos) {
                    int best = -1;
                    for (int ci = 0; ci < nc; ++ci) {
                        if (ci == big || clusters[ci].count < kBody2MinRecords) continue;
                        if (best < 0 || clusters[ci].count > clusters[best].count) best = ci;
                    }
                    if (best >= 0) {
                        std::vector<int> members2;
                        members2.reserve(clusters[best].count);
                        float cen2[3] = {0.0f, 0.0f, 0.0f};
                        for (uint32_t i = 0; i < n; ++i) {
                            if (clusterIdx[i] != best) continue;
                            const float* pn = &posNow[static_cast<size_t>(i) * 3];
                            const float dx = pn[0] - camPos[0], dy = pn[1] - camPos[1], dz = pn[2] - camPos[2];
                            if (dx * dx + dy * dy + dz * dz < kShipRadiusM * kShipRadiusM) continue;
                            members2.push_back(static_cast<int>(i));
                            for (int k = 0; k < 3; ++k) cen2[k] += pn[k];
                        }
                        if (members2.size() >= kBody2MinRecords) {
                            for (int k = 0; k < 3; ++k) {
                                cen2[k] = cen2[k] / static_cast<float>(members2.size()) - camPos[k];
                            }
                            const float ddx = cen2[0] - relC[0], ddy = cen2[1] - relC[1], ddz = cen2[2] - relC[2];
                            float w2[3] = {}, t2[3] = {}, rms2 = 0.0f;
                            if (ddx * ddx + ddy * ddy + ddz * ddz <= kBody2NearM * kBody2NearM &&
                                fitTrimmed(members2, posNow, posPrev, w2, t2, &rms2) && rms2 <= kMotionMaxRmsM) {
                                const float deg2 = sqrtf(w2[0] * w2[0] + w2[1] * w2[1] + w2[2] * w2[2]) * 57.2957795f;
                                const float m2 = sqrtf(t2[0] * t2[0] + t2[1] * t2[1] + t2[2] * t2[2]);
                                // A second body TURNS OTHERWISE than the body: the ring's own
                                // cluster splits at the table's overflow, and its other half
                                // (267 and 394 parts at the body's own 0.042 deg, the flight
                                // of 16:22) is the body, not a second one. The ships' slice
                                // test, the other way about: a quarter of the body's turn
                                // apart, at least.
                                const float rx = w2[0] - w[0], ry = w2[1] - w[1], rz = w2[2] - w[2];
                                const float rel = sqrtf(rx * rx + ry * ry + rz * rz);
                                const float wn = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
                                const bool ownTurn = rel >= 0.25f * wn + 6e-5f;
                                if (!ownTurn) ++g_body2Fragments;
                                // ...and turns the same way it did on kBody2Confirm pairs
                                // running: a part the game updates at a lower rate steps
                                // and alternates from pair to pair (the stepped parts,
                                // object_probe.h) and must not be taken for a body.
                                bool steady2 = false;
                                if (ownTurn) {
                                    const float sx = w2[0] - g_body2LastW[0], sy = w2[1] - g_body2LastW[1],
                                                sz = w2[2] - g_body2LastW[2];
                                    const float w2n = sqrtf(w2[0] * w2[0] + w2[1] * w2[1] + w2[2] * w2[2]);
                                    if (g_body2Streak > 0 && sqrtf(sx * sx + sy * sy + sz * sz) <= 0.25f * w2n + 6e-5f) {
                                        ++g_body2Streak;
                                    } else {
                                        g_body2Streak = 1;
                                    }
                                    memcpy(g_body2LastW, w2, sizeof(g_body2LastW));
                                    steady2 = g_body2Streak >= kBody2Confirm;
                                } else {
                                    g_body2Streak = 0;
                                }
                                if (steady2 && deg2 >= kMotionMinDeg && deg2 <= kMotionMaxDeg && m2 <= kMotionMaxM) {
                                    g_body2LastRelDeg = rel * 57.2957795f;
                                    // Continuity against its own last centroid, else a new ring.
                                    bool same2 = g_lastRel2Valid && g_body2Hold > 0;
                                    if (same2) {
                                        const float ex = cen2[0] - g_lastRel2[0], ey = cen2[1] - g_lastRel2[1],
                                                    ez = cen2[2] - g_lastRel2[2];
                                        same2 = ex * ex + ey * ey + ez * ez <= kBodyContinuityM * kBodyContinuityM;
                                    }
                                    if (!same2) g_rateRing2N = 0;
                                    memcpy(g_lastRel2, cen2, sizeof(g_lastRel2));
                                    g_lastRel2Valid = true;
                                    RatePair& rp2 = g_rateRing2[g_rateRing2N % kRateRing];
                                    for (int k = 0; k < 3; ++k) {
                                        rp2.w[k] = w2[k] / dt;
                                        rp2.t[k] = t2[k] / dt;
                                    }
                                    ++g_rateRing2N;
                                    const uint32_t rc2 = g_rateRing2N < kRateRing ? g_rateRing2N : kRateRing;
                                    rateMedian(g_rateRing2, rc2, g_motion.omega2PerMs, g_motion.t2PerMs);
                                    temporalRodrigues(w2, g_motion.R2);
                                    memcpy(g_motion.t2, t2, sizeof(g_motion.t2));
                                    g_motion.records2 = static_cast<uint32_t>(members2.size());
                                    g_motion.body2 = true;
                                    g_body2Hold = kBody2HoldPairs;
                                    g_body2Pos.clear();
                                    for (int m : members2) {
                                        for (int k = 0; k < 3; ++k) g_body2Pos.push_back(posNow[static_cast<size_t>(m) * 3 + k]);
                                    }
                                    g_body2LastRecords = g_motion.records2;
                                    g_body2LastDeg = deg2;
                                    body2Cluster = best;
                                    found2 = true;
                                    ++g_body2Pairs;
                                }
                            }
                        }
                    }
                }
                if (!found2) {
                    if (g_body2Hold > 0) {
                        --g_body2Hold;
                        ++g_body2HeldPairs;
                    }
                    if (g_body2Hold == 0) {
                        g_motion.body2 = false;
                        g_body2Pos.clear();
                    }
                }
                temporalRodrigues(w, g_motion.R);
                memcpy(g_motion.t, tf, sizeof(g_motion.t));
                // The rates: blended into the held ones when this pair
                // agrees with them (the same body, turning at its constant
                // rate: within 20 deg of axis and 30% of rate), so each
                // pair nudges the vectors and never steps them; a pair that
                // disagrees is a new body, and replaces.
                float wr[3], tr[3];
                for (int k = 0; k < 3; ++k) {
                    wr[k] = w[k] / dt;
                    tr[k] = tf[k] / dt;
                }
                // The held rate is the ROLLING MEDIAN of the last sixteen
                // pairs' rates, magnitude and axis apart. A pair's own rate
                // is noisy where the station is not: the fitted turn per pair
                // ran 0.036-0.050 deg at the same interval on 2026-09-09, and
                // 0.025 or 0.076 on odd ones -- the game's step landing early
                // or late against the probe's own clock -- and a blend that
                // adopted or refused each pair on a thirty-percent test
                // either wobbled with them (the outer ring smeared) or
                // refused seventy-seven pairs in one flight. The median of
                // sixteen takes neither the wobble nor the outliers, and a
                // new body starts its own.
                if (!heldValid || heldAge >= kBodyContinuityFrames) g_rateRingN = 0;
                RatePair& rp = g_rateRing[g_rateRingN % kRateRing];
                memcpy(rp.w, wr, sizeof(rp.w));
                memcpy(rp.t, tr, sizeof(rp.t));
                ++g_rateRingN;
                const uint32_t ringCount = g_rateRingN < kRateRing ? g_rateRingN : kRateRing;
                const float medMag = rateMedian(g_rateRing, ringCount, g_motion.omegaPerMs, g_motion.tPerMs);
                // A pair far from the median is counted and, now and then, said.
                {
                    const float nn = sqrtf(wr[0] * wr[0] + wr[1] * wr[1] + wr[2] * wr[2]);
                    if (ringCount >= 4 && medMag > 0.0f && (nn > 2.0f * medMag || nn < 0.5f * medMag)) {
                        ++g_rateOutliers;
                        if (dueMs(g_rateOutlierNoteMs, 30000)) {
                            g_rateOutlierNoteMs = stampMs();
                            Log::get().note(
                                "object probe: a pair's turn (%.4f deg over its %.1f ms) sits %.1fx the held "
                                "median rate; the median of the last %u pairs holds. %llu such pairs so far.",
                                static_cast<double>(fitDeg), static_cast<double>(dt),
                                static_cast<double>(nn / medMag), ringCount,
                                static_cast<unsigned long long>(g_rateOutliers));
                        }
                    }
                }
                g_motion.dtMs = dt;
                g_motion.rms = rms;
                g_motion.share = share;
                g_motion.records = static_cast<uint32_t>(members.size());
                for (int k = 0; k < 3; ++k) g_motion.camPos[k] = camPos ? camPos[k] : 0.0f;
                g_motion.age = 0;
                bodyTaken = true;
                std::unordered_set<uint64_t> bodySigs;
                bodySigs.reserve(members.size());
                for (int m : members) bodySigs.insert(sigNow[static_cast<size_t>(m)]);
                buildGrid(now, n, liveNow, sigNow, bodySigs, members.data(),
                          static_cast<int>(members.size()), posNow.data(), camPos,
                          g_motion.body2 && !g_body2Pos.empty() ? g_body2Pos.data() : nullptr,
                          g_motion.body2 ? static_cast<int>(g_body2Pos.size() / 3) : 0);
                // Published for the render thread in one short section: the
                // struct with its grid's pointer, the age reset.
                {
                    std::lock_guard<std::mutex> lk(g_publish);
                    g_motionPub = g_motion;
                    g_motionPubValid = true;
                    g_motionAge = 0;
                }
            }
        }
        ++g_bigPairs;
        g_bigShareSum += static_cast<double>(c.count) / static_cast<double>(poseChanged);
        g_bigAngleSum += angle;
        g_bigTransSum += trans;
        g_outsideSum += poseChanged - c.count;
        if (second >= 0) g_secondSum += clusters[second].count;
        if (c.count * 2 > poseChanged) ++g_commonPairs;
        if (angle < kRotQuantDeg && live && c.count * 2 > live) {
            ++g_rebasePairs;
            if (trans > g_rebaseMaxM) g_rebaseMaxM = trans;
        }
    }
    // The body's motion over this pair for the slice test: the fit when
    // there was one, else the held rates over the interval.
    float heldW[3], heldT[3];
    const float* bw = nullptr;
    const float* bt = nullptr;
    if (bodyFit) {
        bw = bodyW;
        bt = bodyT;
    } else if (heldValid) {
        const float dtb = (dtMs >= 5.0f && dtMs <= 50.0f) ? dtMs : 11.1f;
        for (int k = 0; k < 3; ++k) {
            heldW[k] = g_motion.omegaPerMs[k] * dtb;
            heldT[k] = g_motion.tPerMs[k] * dtb;
        }
        bw = heldW;
        bt = heldT;
    }
    takeShips(clusters, nc, big, bodyTaken, body2Cluster, bw, bt, clusterIdx, posNow, posPrev, n, camPos, dtMs,
              live);
}

void releaseRing() {
    for (Slot& s : g_ring) {
        if (s.staging) s.staging->Release();
        if (s.sceneStaging) s.sceneStaging->Release();
        s = Slot();
    }
    g_keep.clear();
    g_keepScene.clear();
    g_keepValid = false;
}

void releasePool() {
    if (g_pool) g_pool->Release();
    g_pool = nullptr;
    g_poolBytes = 0;
    g_records = 0;
    if (g_scene) g_scene->Release();
    g_scene = nullptr;
    g_sceneBytes = 0;
}

// One frame of a pair to disk: a 32-byte header, the scene block, the pool.
// tools/pool_pair.py reads it.
bool writeDump(const uint8_t* pool, uint32_t poolBytes, const uint8_t* scene, uint32_t sceneBytes,
               uint32_t frame, wchar_t* path, size_t pathN) {
    const std::wstring dir = Log::get().dir() + L"\\pool";
    if (!g_dumpDirMade) {
        g_dumpDirMade = true;
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    _snwprintf_s(path, pathN, _TRUNCATE, L"%s\\pool_%02u%02u%02u_%u.bin", dir.c_str(),
                 static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
                 static_cast<unsigned>(st.wSecond), frame);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        path[0] = 0;
        return false;
    }
    struct Header {
        char     magic[8];
        uint32_t version;
        uint32_t frame;
        uint32_t poolBytes;
        uint32_t sceneBytes;
        uint32_t recordBytes;
        uint32_t records;
    };
    static_assert(sizeof(Header) == 32, "the reader assumes a 32-byte header");
    Header hd = {{'E', 'D', 'V', 'R', 'P', 'O', 'O', 'L'}, 1u, frame, poolBytes, sceneBytes,
                 kRecordBytes, poolBytes / kRecordBytes};
    DWORD w = 0;
    bool ok = WriteFile(h, &hd, sizeof(hd), &w, nullptr) != 0;
    if (ok && sceneBytes) ok = WriteFile(h, scene, sceneBytes, &w, nullptr) != 0;
    if (ok) ok = WriteFile(h, pool, poolBytes, &w, nullptr) != 0;
    CloseHandle(h);
    return ok;
}

// The byte histogram as ranges: which bytes of a changed record change in
// nearly every one (per-frame fields), which sometimes, which never -- the
// record's layout read off its behaviour, and the identity's key is the
// bytes that never move for a live object.
void byteRanges(char* buf, size_t n) {
    size_t used = 0;
    buf[0] = 0;
    if (!g_byteHistN) return;
    int ranges = 0;
    uint32_t k = 0;
    while (k < kRecordBytes && ranges < 40) {
        const double f0 = static_cast<double>(g_byteHist[k]) / static_cast<double>(g_byteHistN);
        const int cls0 = f0 >= 0.9 ? 2 : (f0 >= 0.05 ? 1 : 0);
        uint32_t e = k;
        double sum = f0;
        while (e + 1 < kRecordBytes) {
            const double f = static_cast<double>(g_byteHist[e + 1]) / static_cast<double>(g_byteHistN);
            const int cls = f >= 0.9 ? 2 : (f >= 0.05 ? 1 : 0);
            if (cls != cls0) break;
            ++e;
            sum += f;
        }
        if (cls0 != 0) {
            const int m = snprintf(buf + used, n - used, "%s%u-%u %.0f%%", ranges ? ", " : "",
                                   k, e, 100.0 * sum / static_cast<double>(e - k + 1));
            if (m < 0 || static_cast<size_t>(m) >= n - used) break;
            used += static_cast<size_t>(m);
            ++ranges;
        }
        k = e + 1;
    }
}

void report() {
    if (!g_pairs && !g_skipped) return;
    const double pairs = g_pairs ? static_cast<double>(g_pairs) : 1.0;
    const double live = g_live ? static_cast<double>(g_live) : 1.0;
    const double changed = g_changed ? static_cast<double>(g_changed) : 1.0;
    const double bigPairs = g_bigPairs ? static_cast<double>(g_bigPairs) : 1.0;
    char ranges[640];
    byteRanges(ranges, sizeof(ranges));
    Log::get().note(
        "object probe: over %llu frame pairs (%llu skipped, a copy not ready in time): %.0f live "
        "records of %u a frame, %.0f%% of them with a signature no other record carries (bytes "
        "28-29, 31, 56-287, 304-307 and 320-335, the ones that held still on the 11:36 flight) "
        "and %.0f%% byte-for-byte twins of another; per pair %.0f changed (%.0f with a new pose, "
        "%.0f other fields only; at most %u), %.1f allocated, %.1f freed. Of the changed, per "
        "pair: %.0f kept their signature at their slot (the same object, moved), %.0f had it at "
        "another slot last frame (a repacked pool), %.0f had one nobody had (new, or stable "
        "bytes that were not); %.1f were whole-byte copies of another slot's last frame. The "
        "second pose block (288-319) against last frame's first block at the slot: the "
        "quaternion equal on %.0f%% of the changed, the position on %.0f%%, the float at 288 "
        "equal to the old scale on %.0f%%; against this frame's own first block: %.0f%% and "
        "%.0f%%.",
        static_cast<unsigned long long>(g_pairs), static_cast<unsigned long long>(g_skipped),
        static_cast<double>(g_live) / pairs, g_records,
        100.0 * static_cast<double>(g_sigUnique) / live, 100.0 * static_cast<double>(g_twins) / live,
        static_cast<double>(g_changed) / pairs, static_cast<double>(g_poseChanged) / pairs,
        static_cast<double>(g_otherOnly) / pairs, g_maxChanged,
        static_cast<double>(g_allocated) / pairs, static_cast<double>(g_freed) / pairs,
        static_cast<double>(g_sigKept) / pairs, static_cast<double>(g_sigMoved) / pairs,
        static_cast<double>(g_sigNew) / pairs, static_cast<double>(g_moved) / pairs,
        100.0 * static_cast<double>(g_twinQuatPrev) / changed,
        100.0 * static_cast<double>(g_twinPosPrev) / changed,
        100.0 * static_cast<double>(g_twinScalePrev) / changed,
        100.0 * static_cast<double>(g_twinQuatSelf) / changed,
        100.0 * static_cast<double>(g_twinPosSelf) / changed);
    Log::get().note(
        "object probe, the motions: among the pose changes, rigid motions clustered within %.2f "
        "deg and %.0f cm plus %.1f mm per metre of the record's distance: %.1f clusters a pair on "
        "average, %u at most%s; the largest held %.0f%% of the pose changes (over half on %llu "
        "of %llu pairs: a common motion, %.4f deg and %.3f m a frame on average -- the frame's "
        "own if it matches the ship's turn and speed on the registration line), %.0f pose "
        "changes a pair outside it (the movers, or noise) and %.0f in the second-largest; the "
        "live records shifted together without a turn on %llu pairs (an origin rebase, up to "
        "%.1f m); %.0f a pair sat in a repacked slot and went unclustered; the pool object changed "
        "%u times; the diff took %.2f ms a pair on its worker thread, %.2f at most, and %llu pairs "
        "were dropped with the worker busy. The fields of a changed record that changed, "
        "by range with the share of changed records they changed in (under 5%% left out): %s.",
        static_cast<double>(kClusterAngleDeg), 100.0 * static_cast<double>(kClusterPosM),
        1000.0 * static_cast<double>(kClusterPosPerM),
        static_cast<double>(g_clusterSum) / pairs, g_clusterMax,
        g_clusterOverflow ? " (and more past the table)" : "",
        100.0 * g_bigShareSum / bigPairs, static_cast<unsigned long long>(g_commonPairs),
        static_cast<unsigned long long>(g_bigPairs), g_bigAngleSum / bigPairs,
        g_bigTransSum / bigPairs, static_cast<double>(g_outsideSum) / bigPairs,
        static_cast<double>(g_secondSum) / bigPairs,
        static_cast<unsigned long long>(g_rebasePairs), g_rebaseMaxM,
        static_cast<double>(g_shuffled) / pairs, g_poolChanges,
        g_diffN ? g_diffMsSum / g_diffN : 0.0, g_diffMsMax,
        static_cast<unsigned long long>(g_busySkipped),
        ranges[0] ? ranges : "none");
    if (g_shipRangeM > 0.0f && g_shipPairs) {
        const double sp = static_cast<double>(g_shipPairs);
        Log::get().note(
            "object probe, the ships: over %llu pairs with the moving ships on, %.2f taken a pair within "
            "%.0f m (%u at most in one pair, %llu more past the %u kept); left out a pair: %.2f out of "
            "range, %.2f slices of the dominant body, %.2f standing still, %.2f moving with the camera "
            "(the player's own), %.2f with under %u parts of their own, %.2f that did not fit as one "
            "thing.",
            static_cast<unsigned long long>(g_shipPairs), static_cast<double>(g_shipsTaken) / sp,
            static_cast<double>(g_shipRangeM), g_shipsMax, static_cast<unsigned long long>(g_shipsOverCap),
            kObjectShipsMax, static_cast<double>(g_shipsOutOfRange) / sp,
            static_cast<double>(g_shipsSlices) / sp, static_cast<double>(g_shipsStill) / sp,
            static_cast<double>(g_shipsOwn) / sp,
            static_cast<double>(g_shipsFew) / sp, kShipMinRecords, static_cast<double>(g_shipsUnfit) / sp);
    }
    if (g_body2Pairs || g_body2HeldPairs || g_body2Fragments) {
        Log::get().note(
            "object probe, the second body: found on %llu pairs and held over %llu that did not find it; "
            "the last had %u parts turning %.4f deg a pair, %.4f deg from the body's turn, marked %.0f m "
            "around each in the grid. %llu pairs offered a fragment of the body (the same turn) instead.",
            static_cast<unsigned long long>(g_body2Pairs), static_cast<unsigned long long>(g_body2HeldPairs),
            g_body2LastRecords, static_cast<double>(g_body2LastDeg), static_cast<double>(g_body2LastRelDeg),
            static_cast<double>(g_body2ReachM), static_cast<unsigned long long>(g_body2Fragments));
    }
    g_body2Pairs = g_body2HeldPairs = g_body2Fragments = 0;
    if (g_stTrackFrames) {
        const double f = static_cast<double>(g_stTrackFrames);
        Log::get().note(
            "object probe, the stepped parts: over %llu frames, %.1f records a frame at the station turned by "
            "something other than the body's turn every frame -- %.1f with a period of two frames, %.1f steady "
            "on one multiple (nought for a part holding still), %.1f irregular -- and their next multiple was "
            "predicted right on %.0f%% of %llu checks; the tracking took %.2f ms a frame.",
            static_cast<unsigned long long>(g_stTrackFrames), static_cast<double>(g_stStepped) / f,
            static_cast<double>(g_stPeriod2) / f, static_cast<double>(g_stSteady) / f,
            static_cast<double>(g_stIrregular) / f,
            g_stPredicted ? 100.0 * static_cast<double>(g_stHit) / static_cast<double>(g_stPredicted) : 0.0,
            static_cast<unsigned long long>(g_stPredicted), g_stMsSum / f);
    }
    g_stTrackFrames = g_stStepped = g_stPeriod2 = g_stSteady = g_stIrregular = g_stPredicted = g_stHit = 0;
    g_stMsSum = 0.0;
    g_shipPairs = g_shipsTaken = g_shipsOutOfRange = g_shipsSlices = 0;
    g_shipsOwn = 0;
    g_shipsStill = g_shipsUnfit = g_shipsOverCap = g_shipsFew = 0;
    g_shipsMax = 0;
    g_pairs = g_skipped = 0;
    g_live = g_sigUnique = g_twins = 0;
    g_changed = g_poseChanged = g_otherOnly = g_moved = 0;
    g_allocated = g_freed = 0;
    g_sigKept = g_sigMoved = g_sigNew = 0;
    g_twinQuatPrev = g_twinPosPrev = g_twinScalePrev = g_twinQuatSelf = g_twinPosSelf = 0;
    g_maxChanged = 0;
    g_clusterSum = 0;
    g_clusterMax = 0;
    g_clusterOverflow = 0;
    g_bigPairs = 0;
    g_bigShareSum = g_bigAngleSum = g_bigTransSum = 0.0;
    g_commonPairs = 0;
    g_outsideSum = g_secondSum = 0;
    g_rebasePairs = 0;
    g_rebaseMaxM = 0.0;
    g_shuffled = 0;
    g_diffMsSum = g_diffMsMax = 0.0;
    g_diffN = 0;
    g_busySkipped = 0;
    g_poolChanges = 0;
    memset(g_byteHist, 0, sizeof(g_byteHist));
    g_byteHistN = 0;
}

bool makeStaging(ID3D11Device* dev, uint32_t bytes, ID3D11Buffer** out) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, out)) && *out;
}

bool ensureSlot(ID3D11DeviceContext* ctx, Slot& s) {
    const bool poolOk = s.staging && s.bytes == g_poolBytes;
    const bool sceneOk = (!g_scene && !s.sceneStaging) || (s.sceneStaging && s.sceneBytes == g_sceneBytes);
    if (poolOk && sceneOk) return true;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    bool ok = true;
    if (!poolOk) {
        if (s.staging) { s.staging->Release(); s.staging = nullptr; }
        ok = makeStaging(dev, g_poolBytes, &s.staging);
        s.bytes = ok ? g_poolBytes : 0;
    }
    if (ok && !sceneOk) {
        if (s.sceneStaging) { s.sceneStaging->Release(); s.sceneStaging = nullptr; }
        s.sceneBytes = 0;
        // The scene block is a want, not a need: a pair without it still diffs.
        if (g_scene && g_sceneBytes && makeStaging(dev, g_sceneBytes, &s.sceneStaging)) {
            s.sceneBytes = g_sceneBytes;
        }
    }
    dev->Release();
    return ok;
}

void issueCopy(ID3D11DeviceContext* ctx, uint8_t role) {
    for (Slot& s : g_ring) {
        if (s.inUse) continue;
        if (!ensureSlot(ctx, s)) { ++g_skipped; return; }
        ctx->CopyResource(s.staging, g_pool);
        if (s.sceneStaging && g_scene) ctx->CopyResource(s.sceneStaging, g_scene);
        s.frame = g_frame;
        s.stampMs = qpcMs();
        memcpy(s.camPass, g_camPass, sizeof(s.camPass));
        s.camPassValid = g_camPassValid;
        s.inUse = true;
        s.role = role;
        return;
    }
    ++g_skipped;
}

// THE WORKER. The diff ran on the render thread every eighth frame and
// took 2-13 ms a pair near a station with ships about (the flight of
// 2026-09-09 14:28: "the diff took 9.30 ms a pair, 13.00 at most") -- a
// frame's budget every eighth frame, felt as the station juddering at
// steady frame times. Nothing in it touches the device: the two copies
// are bytes once mapped, and the results are a struct, eight ships and a
// grid. So it runs on a thread of its own, one job at a time; poll copies
// the pair into the job (1.4 MB, a tenth of a millisecond) and a pair that
// finds the worker still on the last is dropped and counted. The results
// are published under g_publish in short sections (the body's struct with
// its grid's pointer, the ships, the ages); the counters the report prints
// are read without it, a torn count being a cosmetic risk taken knowingly.
void workerMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // the render thread first
    DiffJob job;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_jobLock);
            g_jobCv.wait(lk, [] { return g_jobPending || g_workerStop; });
            if (g_workerStop) return;
            std::swap(job, g_job);
            g_jobPending = false;
            g_jobRunning = true;
        }
        if (g_resetWorker.exchange(false)) {
            g_lastRelValid = false;
            g_rateRingN = 0;
            g_dtRingN = 0;
        }
        guarded("objectProbe.diff", [&] {
            g_pairLagFrames = job.lagFrames;
            g_pairCamPrevValid = job.haveCamPrev;
            if (job.haveCamPrev) memcpy(g_pairCamPrev, job.camPrev, sizeof(g_pairCamPrev));
            LARGE_INTEGER dq0{}, dq1{}, dqf{};
            QueryPerformanceCounter(&dq0);
            diffPair(job.prev.data(), job.now.data(), static_cast<uint32_t>(job.now.size()), job.dtMs,
                     job.haveCam ? job.cam : nullptr);
            QueryPerformanceCounter(&dq1);
            QueryPerformanceFrequency(&dqf);
            if (dqf.QuadPart > 0) {
                const double ms = static_cast<double>(dq1.QuadPart - dq0.QuadPart) * 1000.0 /
                                  static_cast<double>(dqf.QuadPart);
                g_diffMsSum += ms;
                if (ms > g_diffMsMax) g_diffMsMax = ms;
                ++g_diffN;
            }
        });
        {
            std::lock_guard<std::mutex> lk(g_jobLock);
            g_jobRunning = false;
        }
    }
}

void enqueueDiff(const uint8_t* now, uint32_t bytes, float dtMs, const float* camPos, bool haveCamPrev,
                 uint32_t lagFrames) {
    {
        std::lock_guard<std::mutex> lk(g_jobLock);
        if (g_jobPending || g_jobRunning) {
            ++g_busySkipped;
            return;
        }
        g_job.prev.assign(g_keep.begin(), g_keep.end());
        g_job.now.assign(now, now + bytes);
        g_job.dtMs = dtMs;
        g_job.haveCam = camPos != nullptr;
        if (camPos) memcpy(g_job.cam, camPos, sizeof(g_job.cam));
        g_job.haveCamPrev = haveCamPrev;
        if (haveCamPrev) memcpy(g_job.camPrev, g_keepCam, sizeof(g_job.camPrev));
        g_job.lagFrames = lagFrames;
        g_jobPending = true;
        if (!g_worker) g_worker = new std::thread(workerMain);
    }
    g_jobCv.notify_one();
}

void stopWorker() {
    if (!g_worker) return;
    {
        std::lock_guard<std::mutex> lk(g_jobLock);
        g_workerStop = true;
    }
    g_jobCv.notify_one();
    g_worker->join();
    delete g_worker;
    g_worker = nullptr;
    g_workerStop = false;
    g_jobPending = false;
    g_jobRunning = false;
}

// THE STEPPED PARTS (object_probe.h says what they are). Every frame's
// copy, read back kReadAfter frames on, is compared with the frame before
// it: each live record at the station turns about the body's axis by some
// multiple of the body's own turn that frame -- one for a part the game
// updates every frame, nought or two or more for one it updates less
// often, minus one when it steps back to an older buffered pose. The
// multiple's history per slot says what the part will do on the frame the
// pass is about to draw: a stepping part repeats with a period of two, so
// the prediction is the entry of the same parity; a part that has kept
// one multiple keeps it; an irregular one gets the mean. Their cells are
// stamped with the prediction for the pass (objectSteppedCells). A part
// that has turned the body's turn on every frame it was seen is the
// body's, and its cells are left alone.

void trackFrame(const uint8_t* bytes, uint32_t nbytes, uint32_t frame, double stampMs) {
    const uint32_t n = nbytes / kRecordBytes;
    const bool consecutive = !g_lastBytes.empty() && g_lastBytes.size() == nbytes && g_lastBytesFrame + 1 == frame;
    if (g_mHist.size() != static_cast<size_t>(n) * kMHist || !consecutive) {
        g_mHist.assign(static_cast<size_t>(n) * kMHist, 127);
        g_mPred.assign(n, 127);
        g_mPredFrame.assign(n, 0);
    }
    g_steppedCells.clear();
    ObjectMotion om;
    const bool haveBody = objectMotionGet(&om) && om.grid != nullptr;
    if (consecutive && haveBody) {
        const double t0 = qpcMs();
        float dt = static_cast<float>(stampMs - g_lastBytesStamp);
        if (dt < 5.0f || dt > 50.0f) dt = 11.1f;
        const float wb[3] = {om.omegaPerMs[0] * dt, om.omegaPerMs[1] * dt, om.omegaPerMs[2] * dt};
        const float wbn2 = wb[0] * wb[0] + wb[1] * wb[1] + wb[2] * wb[2];
        const float cell = (om.bmax[0] - om.bmin[0]) / static_cast<float>(kObjectGrid);
        const int gn = static_cast<int>(kObjectGrid);
        if (wbn2 > 1e-12f && cell > 0.0f) {
            // The frame the pass draws next is g_frame; this copy is `frame`.
            const uint32_t parity = (g_frame - frame) & 1u;
            uint32_t stepped = 0, p2 = 0, steady = 0, irr = 0;
            for (uint32_t i = 0; i < n; ++i) {
                const uint8_t* a = g_lastBytes.data() + static_cast<size_t>(i) * kRecordBytes;
                const uint8_t* b = bytes + static_cast<size_t>(i) * kRecordBytes;
                int8_t* h = &g_mHist[static_cast<size_t>(i) * kMHist];
                if (emptyRecord(a) || emptyRecord(b)) {
                    for (uint32_t k = 0; k < kMHist; ++k) h[k] = 127;
                    continue;
                }
                const Pose pb = decodePose(b);
                bool inBox = true;
                int c[3];
                for (int k = 0; k < 3; ++k) {
                    if (pb.p[k] < om.bmin[k] || pb.p[k] >= om.bmax[k]) inBox = false;
                    const int idx = static_cast<int>((pb.p[k] - om.bmin[k]) / cell);
                    c[k] = idx < 0 ? 0 : (idx >= gn ? gn - 1 : idx);
                }
                const float dx = pb.p[0] - om.camPos[0], dy = pb.p[1] - om.camPos[1], dz = pb.p[2] - om.camPos[2];
                if (!inBox || dx * dx + dy * dy + dz * dz < kBodyNearM * kBodyNearM) {
                    for (uint32_t k = 0; k < kMHist; ++k) h[k] = 127;
                    continue;
                }
                int m;
                if (memcmp(a + 4, b + 4, 24) == 0) {   // scale, quaternion, position: the pose held
                    m = 0;
                } else {
                    const Pose pa = decodePose(a);
                    float qd[4];
                    quatMulConj(pa.q, pb.q, qd);
                    if (qd[3] < 0.0f) {
                        for (int k = 0; k < 4; ++k) qd[k] = -qd[k];
                    }
                    const float xyz = sqrtf(qd[0] * qd[0] + qd[1] * qd[1] + qd[2] * qd[2]);
                    float rv[3] = {0.0f, 0.0f, 0.0f};
                    if (xyz > 1e-9f) {
                        const float ang = 2.0f * atan2f(xyz, qd[3]);
                        for (int k = 0; k < 3; ++k) rv[k] = qd[k] / xyz * ang;
                    }
                    const float proj = (rv[0] * wb[0] + rv[1] * wb[1] + rv[2] * wb[2]) / wbn2;
                    const float ex = rv[0] - proj * wb[0], ey = rv[1] - proj * wb[1], ez = rv[2] - proj * wb[2];
                    if (ex * ex + ey * ey + ez * ez > kMAxisSlack * kMAxisSlack * wbn2 + 1e-10f) {
                        for (uint32_t k = 0; k < kMHist; ++k) h[k] = 127;   // not about the body's axis
                        ++irr;
                        continue;
                    }
                    m = static_cast<int>(floorf(proj + 0.5f));
                    if (m < kSteppedMin) m = kSteppedMin;
                    if (m > kSteppedMax) m = kSteppedMax;
                }
                if (g_mPred[i] != 127 && g_mPredFrame[i] == frame) {
                    ++g_stPredicted;
                    if (g_mPred[i] == m) ++g_stHit;
                }
                for (uint32_t k = kMHist - 1; k > 0; --k) h[k] = h[k - 1];
                h[0] = static_cast<int8_t>(m);
                int have = 0;
                for (uint32_t k = 0; k < kMHist; ++k) {
                    if (h[k] == 127) break;
                    ++have;
                }
                if (have < 4) continue;
                bool allOne = true, allSame = true, per2 = true;
                for (int k = 0; k < have; ++k) {
                    if (h[k] != 1) allOne = false;
                    if (h[k] != h[0]) allSame = false;
                }
                for (int k = 2; k < have; ++k) {
                    if (h[k] != h[k - 2]) per2 = false;
                }
                if (allOne) continue;   // the body's own: every frame, the body's turn
                int pred;
                if (allSame) {
                    pred = h[0];
                    ++steady;
                } else if (per2) {
                    pred = h[parity];
                    ++p2;
                } else {
                    int sum = 0;
                    for (int k = 0; k < have; ++k) sum += h[k];
                    pred = static_cast<int>(floorf(static_cast<float>(sum) / static_cast<float>(have) + 0.5f));
                    ++irr;
                }
                if (pred < kSteppedMin) pred = kSteppedMin;
                if (pred > kSteppedMax) pred = kSteppedMax;
                g_mPred[i] = static_cast<int8_t>(pred);
                g_mPredFrame[i] = g_frame;
                ++stepped;
                const uint8_t v = static_cast<uint8_t>(kSteppedCellBase + (pred - kSteppedMin));
                for (int z = c[2] - 1; z <= c[2] + 1; ++z) {
                    if (z < 0 || z >= gn) continue;
                    for (int y = c[1] - 1; y <= c[1] + 1; ++y) {
                        if (y < 0 || y >= gn) continue;
                        for (int x = c[0] - 1; x <= c[0] + 1; ++x) {
                            if (x < 0 || x >= gn) continue;
                            g_steppedCells.push_back({static_cast<uint32_t>((z * gn + y) * gn + x), v});
                        }
                    }
                }
            }
            ++g_stTrackFrames;
            g_stStepped += stepped;
            g_stPeriod2 += p2;
            g_stSteady += steady;
            g_stIrregular += irr;
        }
        g_stMsSum += qpcMs() - t0;
    }
    g_lastBytes.assign(bytes, bytes + nbytes);
    g_lastBytesFrame = frame;
    g_lastBytesStamp = stampMs;
}

void poll(ID3D11DeviceContext* ctx) {
    // In frame order, so a pair's first copy is kept before its second is
    // diffed against it.
    for (int pass = 0; pass < kRing; ++pass) {
        Slot* s = nullptr;
        for (Slot& c : g_ring) {
            if (!c.inUse) continue;
            if (g_frame - c.frame < kReadAfter) continue;
            if (!s || c.frame < s->frame) s = &c;
        }
        if (!s) return;
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ctx->Map(s->staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            if (g_frame - s->frame > kDropAfter) { s->inUse = false; ++g_skipped; continue; }
            return;   // the older copies are not ready either
        }
        if (FAILED(hr) || !m.pData) { s->inUse = false; ++g_skipped; continue; }
        const uint8_t* bytes = static_cast<const uint8_t*>(m.pData);
        // The scene block's copy was issued right after the pool's, so it is
        // done when the pool's is; a refusal just means a dump without it.
        const uint8_t* scene = nullptr;
        uint32_t sceneBytes = 0;
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (s->sceneStaging &&
            SUCCEEDED(ctx->Map(s->sceneStaging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms)) &&
            ms.pData) {
            scene = static_cast<const uint8_t*>(ms.pData);
            sceneBytes = s->sceneBytes;
        }
        trackFrame(bytes, s->bytes, s->frame, s->stampMs);
        if (s->role == 1) {
            g_keep.assign(bytes, bytes + s->bytes);
            g_keepScene.assign(scene, scene + sceneBytes);
            g_keepFrame = s->frame;
            g_keepStamp = s->stampMs;
            g_keepValid = true;
            g_keepCamValid = s->camPassValid;
            if (s->camPassValid) memcpy(g_keepCam, s->camPass, sizeof(g_keepCam));
        } else if (s->role == 2 && g_keepValid && g_keepFrame + kPairSpan == s->frame && g_keep.size() == s->bytes) {
            // The camera's position in the record's frame, for the ship-radius
            // exclusion and the pair's frame stamp: the pass's chosen camera
            // when this copy was issued (objectProbeNoteCamera), which is the
            // frame the pass reads the positions in; failing that, the scene
            // block's own camera rows (233-235, their fourth column), which
            // are whichever camera wrote the block last -- another's often
            // enough that the body stood down 16-25 frames an interval on it
            // (2026-09-09 05:48).
            float cam[3];
            const float* camPos = nullptr;
            if (s->camPassValid) {
                memcpy(cam, s->camPass, sizeof(cam));
                camPos = cam;
            } else if (scene && sceneBytes >= 236u * 16u) {
                for (int r = 0; r < 3; ++r) memcpy(&cam[r], scene + (233 + r) * 16 + 12, sizeof(float));
                camPos = cam;
            }
            // The diff goes to the worker with copies of both frames
            // (workerMain says why).
            enqueueDiff(bytes, s->bytes,
                        static_cast<float>(s->stampMs > g_keepStamp ? s->stampMs - g_keepStamp : 0.0), camPos,
                        g_keepCamValid && s->camPassValid, g_frame >= s->frame ? g_frame - s->frame : 0);
            if (g_verbose && g_dumps < kDumpMax && dueMs(g_dumpMs, kDumpEveryMs)) {
                g_dumpMs = stampMs();
                ++g_dumps;
                wchar_t pa[MAX_PATH], pb[MAX_PATH];
                const bool okA = writeDump(g_keep.data(), static_cast<uint32_t>(g_keep.size()),
                                           g_keepScene.data(), static_cast<uint32_t>(g_keepScene.size()),
                                           g_keepFrame, pa, MAX_PATH);
                const bool okB = writeDump(bytes, s->bytes, scene, sceneBytes, s->frame, pb, MAX_PATH);
                Log::get().note(
                    "object probe: the pair of frames %u and %u is on disk for the desk -- %ls and "
                    "%ls (each a 32-byte header, the scene block's %u bytes from VS b%u, then the "
                    "pool's %u bytes; tools/pool_pair.py reads them)%s. At most %u pairs a session, "
                    "one every %u s; the write may show as one long frame.",
                    g_keepFrame, s->frame, okA ? pa : L"(not written)", okB ? pb : L"(not written)",
                    sceneBytes, kSceneSlot, s->bytes,
                    (okA && okB) ? "" : " -- a write FAILED, the directory may be unwritable",
                    kDumpMax, static_cast<unsigned>(kDumpEveryMs / 1000));
            }
            g_keepValid = false;
        } else if (s->role == 2) {
            ++g_skipped;
            g_keepValid = false;
        }
        if (scene) ctx->Unmap(s->sceneStaging, 0);
        ctx->Unmap(s->staging, 0);
        s->inUse = false;
    }
}

}  // namespace

void objectProbeConfigure(Config& cfg) {
    // The probe's readings are for the desk; the pool's copy and diff also
    // feed the temporal pass's body path (fix.temporal_aa_objects), which
    // needs them without the log.
    g_verbose = cfg.getBool("advanced.object_probe", false);
    g_on = g_verbose || cfg.getBool("fix.temporal_aa_objects", false);
}

bool objectProbeWantsDraws() { return g_on; }

bool objectMotionGet(ObjectMotion* out) {
    if (!out) return false;
    std::lock_guard<std::mutex> lk(g_publish);
    if (!g_motionPubValid) return false;
    *out = g_motionPub;
    out->age = g_motionAge;
    return true;
}

void objectProbeNoteCamera(const float pos[3]) {
    if (!pos) return;
    memcpy(g_camPass, pos, sizeof(g_camPass));
    g_camPassValid = true;
}

void objectMotionSetReach(float metres) {
    if (!std::isfinite(metres)) return;
    g_reachM = metres < 1.0f ? 1.0f : (metres > 2000.0f ? 2000.0f : metres);
}

uint32_t objectSteppedCells(const SteppedCell** cells) {
    *cells = g_steppedCells.empty() ? nullptr : g_steppedCells.data();
    return static_cast<uint32_t>(g_steppedCells.size());
}

uint32_t objectShipsGet(ObjectShip* out, uint32_t cap, float camPos[3], uint32_t* age) {
    if (!out || !cap) return 0;
    std::lock_guard<std::mutex> lk(g_publish);
    if (!g_shipCount) return 0;
    const uint32_t n = g_shipCount < cap ? g_shipCount : cap;
    memcpy(out, g_ships, sizeof(ObjectShip) * n);
    if (camPos) memcpy(camPos, g_shipCamPos, sizeof(g_shipCamPos));
    if (age) *age = g_shipAge;
    return n;
}

void objectShipsSetRange(float metres) {
    if (!std::isfinite(metres)) return;
    g_shipRangeM = metres < 0.0f ? 0.0f : (metres > 5000.0f ? 5000.0f : metres);
}

void objectProbeOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t instances) {
    if (!g_on || g_checksLeft == 0 || !ctx) return;
    // The record-carrying families are instanced (question 5: every carrier
    // declares INSTANCEANDMODELDATAINDEX); a plain draw is not asked.
    if ((kind != 'X' && kind != 'N') || instances == 0) return;
    --g_checksLeft;
    ID3D11ShaderResourceView* srv = nullptr;
    bool got = false;
    guardedBudget(g_budget, [&] {
        ctx->VSGetShaderResources(kPoolSlot, 1, &srv);
        got = true;
    });
    if (!got || !srv) return;
    ResourceInfo info;
    if (bindingResolve(srv, &info) && info.isBuffer && info.b == kRecordBytes &&
        info.a >= kRecordBytes) {
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        ID3D11Buffer* buf = nullptr;
        if (res) {
            res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buf));
            res->Release();
        }
        if (buf) {
            if (buf != g_pool) {
                const bool had = g_pool != nullptr;
                releasePool();
                releaseRing();
                g_pool = buf;   // the QueryInterface reference is the one held
                g_poolBytes = info.a;
                g_records = info.a / kRecordBytes;
                if (had) ++g_poolChanges;
                if (!g_noted) {
                    g_noted = true;
                    // The usage decides what an unwritten slot holds: a
                    // dynamic buffer is renamed on every discarding map,
                    // so a slot the game did not write this frame carries
                    // whatever the allocation held last time round -- a
                    // stale record, counted live (the 11:36 flight: 175-206
                    // "allocated" a pair against 3-20 freed, on the pad).
                    D3D11_BUFFER_DESC bd{};
                    buf->GetDesc(&bd);
                    const char* usage = bd.Usage == D3D11_USAGE_DEFAULT     ? "default"
                                        : bd.Usage == D3D11_USAGE_IMMUTABLE ? "immutable"
                                        : bd.Usage == D3D11_USAGE_DYNAMIC
                                            ? "dynamic (renamed on every discarding map: a slot the "
                                              "game did not write this frame may hold a stale record)"
                                            : "staging";
                    Log::get().note(
                        "object probe: the instanced-mesh pool is at VS t33 on the scene's draws -- "
                        "a structured buffer of %u bytes, %u records of %u (%.1f MB), object %p, "
                        "usage %s, cpu access 0x%X, bind 0x%X, misc 0x%X. Two frames in a row are "
                        "copied on the GPU every %u frames and read back late; the totals every 20 s "
                        "say whether a record keeps its slot between frames (question 3 of "
                        "docs\\per-object-motion.md), how many distinct rigid motions a frame carries "
                        "(question 6) and when the origin rebased (question 7). Nothing on the draw "
                        "path but one shader-resource read a second.",
                        g_poolBytes, g_records, kRecordBytes,
                        static_cast<double>(g_poolBytes) / 1048576.0,
                        static_cast<void*>(g_pool), usage, static_cast<unsigned>(bd.CPUAccessFlags),
                        static_cast<unsigned>(bd.BindFlags), static_cast<unsigned>(bd.MiscFlags),
                        kPairEvery);
                }
            } else {
                buf->Release();
            }
            // The scene block bound with the pool on the same draw, for the
            // dumps: cb1[275] is the camera in the record's frame (the game's
            // own shader subtracts it from the record's position), so a pair
            // on disk carries the frame's own motion beside the records'.
            ID3D11Buffer* scene = nullptr;
            guardedBudget(g_budget, [&] { ctx->VSGetConstantBuffers(kSceneSlot, 1, &scene); });
            if (scene) {
                if (scene != g_scene) {
                    if (g_scene) g_scene->Release();
                    g_scene = scene;   // the Get's reference is the one held
                    D3D11_BUFFER_DESC sd{};
                    scene->GetDesc(&sd);
                    g_sceneBytes = sd.ByteWidth;
                } else {
                    scene->Release();
                }
            }
            g_checksLeft = 0;
            g_checkMs = stampMs();
        }
    }
    srv->Release();
}

void objectProbeFrameBoundary(ID3D11DeviceContext* ctx) {
    if (!g_on) {
        if (g_wasOn) {
            // Switched off live: the copies and the reference go, the
            // figures print once more.
            if (g_verbose) report();
            releaseRing();
            releasePool();
            g_wasOn = false;
            g_noted = false;
            {
                std::lock_guard<std::mutex> lk(g_publish);
                g_motionPubValid = false;
                g_shipCount = 0;
            }
            g_resetWorker = true;
        }
        return;
    }
    g_wasOn = true;
    ++g_frame;
    // The body's motion and the ships age a frame, under the publish lock
    // the worker publishes into; past the hold they are nobody's.
    {
        std::lock_guard<std::mutex> lk(g_publish);
        if (g_motionPubValid && ++g_motionAge > kMotionHoldFrames) g_motionPubValid = false;
        if (g_shipCount && ++g_shipAge > kShipHoldFrames) g_shipCount = 0;
    }
    g_checksLeft = (g_pool && !dueMs(g_checkMs, kRecheckMs)) ? 0 : kChecksPerFrame;
    if (!ctx) return;
    guardedBudget(g_budget, [&] {
        if (g_pool) {
            g_framesWithoutPool = 0;
            // A copy EVERY frame for the stepped parts (trackFrame), two of
            // which are the pair's, kPairSpan frames apart from an
            // alternating start (kPairSpan says why).
            const uint32_t phase = g_frame % kPairEvery;
            const uint32_t base = (g_frame / kPairEvery) & 1u;
            issueCopy(ctx, phase == base ? 1 : (phase == base + kPairSpan ? 2 : 0));
            poll(ctx);
        } else if (++g_framesWithoutPool == kAbsentFrames && !g_absentNoted && g_verbose) {
            g_absentNoted = true;
            Log::get().note(
                "object probe: on, but no 336-byte structured buffer has been found at VS t33 on "
                "the first instanced eye draws of %u frames -- not a rendered scene yet (a menu, "
                "a loading screen), or the pool has moved from where the 2026-09-06 dump put it. "
                "It keeps looking.",
                kAbsentFrames);
        }
    });
    if (dueMs(g_reportMs, kReportMs)) {
        g_reportMs = stampMs();
        if (g_verbose) report();
    }
}

void objectProbeShutdown() {
    stopWorker();
    releaseRing();
    releasePool();
    g_on = false;
    g_wasOn = false;
    std::lock_guard<std::mutex> lk(g_publish);
    g_motionPubValid = false;
    g_shipCount = 0;
}

}  // namespace edvr
