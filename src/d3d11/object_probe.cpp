#include "object_probe.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
constexpr uint32_t kReadAfter = 3;       // frames before a copy is asked for (never waited on)
constexpr uint32_t kDropAfter = 30;      // ...and after which a copy still in flight is given up
constexpr int      kRing = 4;
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
bool     g_motionValid = false;
uint32_t g_motionAge = 0;
float    g_reachM = 700.0f;
uint8_t  g_grid[kObjectGrid * kObjectGrid * kObjectGrid];
uint32_t g_gridVersion = 0;
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
void buildGrid(const uint8_t* now, uint32_t n, const std::vector<uint8_t>& liveNow,
               const std::vector<uint64_t>& sigNow, const std::unordered_set<uint64_t>& bodySigs,
               const int* memberSlots, int count, const float* pos, const float* camPos) {
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
    {
        float wlo[3], whi[3];
        for (int k = 0; k < 3; ++k) { wlo[k] = lo[k]; whi[k] = hi[k]; }
        for (uint32_t i = 0; i < n; ++i) {
            if (!liveNow[i] || !bodySigs.count(sigNow[i])) continue;
            const Pose pr = decodePose(now + i * kRecordBytes);
            bool near = true;
            for (int k = 0; k < 3; ++k) {
                if (pr.p[k] < lo[k] - kBodyTipM || pr.p[k] > hi[k] + kBodyTipM) near = false;
            }
            if (!near) continue;
            for (int k = 0; k < 3; ++k) {
                if (pr.p[k] < wlo[k]) wlo[k] = pr.p[k];
                if (pr.p[k] > whi[k]) whi[k] = pr.p[k];
            }
        }
        for (int k = 0; k < 3; ++k) { lo[k] = wlo[k]; hi[k] = whi[k]; }
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
    memset(g_grid, 0, sizeof(g_grid));
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
    bool     keep = false;   // the first of a pair: its bytes are kept for the second
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
constexpr int   kMaxClusters = 64;
// 0.01 deg: three quanta. It was 0.03 on the body path's first two flights
// and that is WIDER than a station's turn (0.021-0.043 deg a frame), so
// every static thing in view merged into the station's cluster -- a box
// fifteen kilometres across, and a representative that was whichever
// record came first: 0.012 deg one pair, 0.000 deg and 1.57 m the next,
// handed to the pass as the station and re-registering its pixels by a
// pixel or two every eighth frame. That was the shimmer.
constexpr float kClusterAngleDeg = 0.01f;
constexpr float kClusterPosM = 0.03f;       // three centimetres...
constexpr float kClusterPosPerM = 1.0e-4f;  // ...plus the quantum's lever arm on the record's distance
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

bool rigidDelta(const Pose& prev, const Pose& now, float qd[4], float t[3], float* angleDeg,
                float* transM, float* distM) {
    quatMulConj(prev.q, now.q, qd);
    if (qd[3] < 0.0f) {
        for (int i = 0; i < 4; ++i) qd[i] = -qd[i];
    }
    const float w = qd[3] > 1.0f ? 1.0f : qd[3];
    *angleDeg = 2.0f * acosf(w) * 57.2957795f;
    float rp[3];
    quatRotate(qd, now.p, rp);
    for (int i = 0; i < 3; ++i) t[i] = prev.p[i] - rp[i];
    *transM = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    *distM = sqrtf(now.p[0] * now.p[0] + now.p[1] * now.p[1] + now.p[2] * now.p[2]);
    return std::isfinite(*angleDeg) && std::isfinite(*transM) && std::isfinite(*distM);
}

// The cluster this motion belongs to, made if none is near; -1 past the table.
int clusterOf(Cluster* cs, int* n, const float qd[4], const float t[3], float angleDeg,
              float transM, float distM) {
    const float posTol = kClusterPosM + kClusterPosPerM * distM;
    for (int j = 0; j < *n; ++j) {
        Cluster& c = cs[j];
        float dot = 0.0f;
        for (int i = 0; i < 4; ++i) dot += qd[i] * c.q[i];
        dot = fabsf(dot) > 1.0f ? 1.0f : fabsf(dot);
        if (2.0f * acosf(dot) * 57.2957795f > kClusterAngleDeg) continue;
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
    uint32_t changed = 0, poseChanged = 0, otherOnly = 0, moved = 0;
    uint32_t allocated = 0, freed = 0;
    uint32_t sigKept = 0, sigMoved = 0, sigNew = 0;
    uint32_t twinQuatPrev = 0, twinPosPrev = 0, twinScalePrev = 0, twinQuatSelf = 0, twinPosSelf = 0;
    Cluster clusters[kMaxClusters];
    int nc = 0;
    uint32_t overflow = 0;
    // Each pose change's cluster and positions, for the body's fit and grid.
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
        if (poseDiff) {
            ++poseChanged;
            float qd[4], t[3], angle = 0.0f, trans = 0.0f, dist = 0.0f;
            const Pose pa = decodePose(a);
            const Pose pb = decodePose(b);
            if (rigidDelta(pa, pb, qd, t, &angle, &trans, &dist)) {
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
            const float fitDeg = fitOk ? sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * 57.2957795f : 0.0f;
            const float fitM = fitOk ? sqrtf(tf[0] * tf[0] + tf[1] * tf[1] + tf[2] * tf[2]) : 0.0f;
            // A body for the pass TURNS (the feature is rotating stations; a
            // pure translation is another ship, or the player's own parts,
            // and hands the station a shift it never made), fits as one
            // rigid thing (the residual), and keeps a sane rate.
            const float dt = (dtMs >= 5.0f && dtMs <= 50.0f) ? dtMs : 11.1f;
            if (fitOk && rms <= kMotionMaxRmsM && fitDeg >= kMotionMinDeg && fitDeg <= kMotionMaxDeg &&
                fitM <= kMotionMaxM) {
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
                bool blend = false;
                if (g_motionValid) {
                    const float* ho = g_motion.omegaPerMs;
                    const float hn = sqrtf(ho[0] * ho[0] + ho[1] * ho[1] + ho[2] * ho[2]);
                    const float nn = sqrtf(wr[0] * wr[0] + wr[1] * wr[1] + wr[2] * wr[2]);
                    if (hn > 0.0f && nn > 0.0f) {
                        const float cosA = (ho[0] * wr[0] + ho[1] * wr[1] + ho[2] * wr[2]) / (hn * nn);
                        blend = cosA > 0.94f && nn > 0.7f * hn && nn < 1.3f * hn;
                    }
                }
                const float a = blend ? 0.3f : 1.0f;
                for (int k = 0; k < 3; ++k) {
                    g_motion.omegaPerMs[k] = (1.0f - a) * g_motion.omegaPerMs[k] + a * wr[k];
                    g_motion.tPerMs[k] = (1.0f - a) * g_motion.tPerMs[k] + a * tr[k];
                }
                g_motion.dtMs = dt;
                g_motion.rms = rms;
                g_motion.share = share;
                g_motion.records = static_cast<uint32_t>(members.size());
                for (int k = 0; k < 3; ++k) g_motion.camPos[k] = camPos ? camPos[k] : 0.0f;
                g_motion.age = 0;
                g_motionAge = 0;
                g_motionValid = true;
                std::unordered_set<uint64_t> bodySigs;
                bodySigs.reserve(members.size());
                for (int m : members) bodySigs.insert(sigNow[static_cast<size_t>(m)]);
                buildGrid(now, n, liveNow, sigNow, bodySigs, members.data(),
                          static_cast<int>(members.size()), posNow.data(), camPos);
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
        "%.1f m); the pool object changed %u times. The fields of a changed record that changed, "
        "by range with the share of changed records they changed in (under 5%% left out): %s.",
        static_cast<double>(kClusterAngleDeg), 100.0 * static_cast<double>(kClusterPosM),
        1000.0 * static_cast<double>(kClusterPosPerM),
        static_cast<double>(g_clusterSum) / pairs, g_clusterMax,
        g_clusterOverflow ? " (and more past the table)" : "",
        100.0 * g_bigShareSum / bigPairs, static_cast<unsigned long long>(g_commonPairs),
        static_cast<unsigned long long>(g_bigPairs), g_bigAngleSum / bigPairs,
        g_bigTransSum / bigPairs, static_cast<double>(g_outsideSum) / bigPairs,
        static_cast<double>(g_secondSum) / bigPairs,
        static_cast<unsigned long long>(g_rebasePairs), g_rebaseMaxM, g_poolChanges,
        ranges[0] ? ranges : "none");
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

void issueCopy(ID3D11DeviceContext* ctx, bool keep) {
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
        s.keep = keep;
        return;
    }
    ++g_skipped;
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
        if (s->keep) {
            g_keep.assign(bytes, bytes + s->bytes);
            g_keepScene.assign(scene, scene + sceneBytes);
            g_keepFrame = s->frame;
            g_keepStamp = s->stampMs;
            g_keepValid = true;
        } else if (g_keepValid && g_keepFrame + 1 == s->frame && g_keep.size() == s->bytes) {
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
            diffPair(g_keep.data(), bytes, s->bytes,
                     static_cast<float>(s->stampMs > g_keepStamp ? s->stampMs - g_keepStamp : 0.0), camPos);
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
        } else {
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
    if (!g_motionValid || !out) return false;
    *out = g_motion;
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
            g_motionValid = false;
        }
        return;
    }
    g_wasOn = true;
    ++g_frame;
    // The body's motion ages a frame; past the hold it is nobody's.
    if (g_motionValid && ++g_motionAge > kMotionHoldFrames) g_motionValid = false;
    g_checksLeft = (g_pool && !dueMs(g_checkMs, kRecheckMs)) ? 0 : kChecksPerFrame;
    if (!ctx) return;
    guardedBudget(g_budget, [&] {
        if (g_pool) {
            g_framesWithoutPool = 0;
            const uint32_t phase = g_frame % kPairEvery;
            if (phase == 0 || phase == 1) issueCopy(ctx, phase == 0);
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
    releaseRing();
    releasePool();
    g_on = false;
    g_wasOn = false;
    g_motionValid = false;
}

}  // namespace edvr
