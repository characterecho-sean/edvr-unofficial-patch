#include "object_probe.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include <d3d11.h>

#include "../common/config.h"
#include "../common/guard.h"
#include "../common/log.h"
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

bool     g_on = false;
bool     g_wasOn = false;
uint32_t g_checksLeft = 0;
uint64_t g_checkMs = 0;
ID3D11Buffer* g_pool = nullptr;      // held (AddRef) while recognised
uint32_t g_poolBytes = 0;
uint32_t g_records = 0;
bool     g_noted = false;
uint32_t g_frame = 0;
uint32_t g_framesWithoutPool = 0;
bool     g_absentNoted = false;

struct Slot {
    ID3D11Buffer* staging = nullptr;
    uint32_t bytes = 0;
    uint32_t frame = 0;
    bool     inUse = false;
    bool     keep = false;   // the first of a pair: its bytes are kept for the second
};
Slot g_ring[kRing];
std::vector<uint8_t> g_keep;   // the first frame of a pair, copied out of its staging buffer
uint32_t g_keepFrame = 0;
bool     g_keepValid = false;

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
constexpr float kClusterAngleDeg = 0.03f;   // eight quanta
constexpr float kClusterPosM = 0.03f;       // three centimetres...
constexpr float kClusterPosPerM = 2.0e-4f;  // ...plus the quantum's lever arm on the record's distance

struct Cluster {
    float    q[4];
    float    t[3];
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
        return j;
    }
    if (*n >= kMaxClusters) return -1;
    Cluster& c = cs[*n];
    memcpy(c.q, qd, sizeof(c.q));
    memcpy(c.t, t, sizeof(c.t));
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
void diffPair(const uint8_t* prev, const uint8_t* now, uint32_t bytes) {
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
            if (rigidDelta(decodePose(a), decodePose(b), qd, t, &angle, &trans, &dist)) {
                if (clusterOf(clusters, &nc, qd, t, angle, trans, dist) < 0) ++overflow;
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
        s = Slot();
    }
    g_keep.clear();
    g_keepValid = false;
}

void releasePool() {
    if (g_pool) g_pool->Release();
    g_pool = nullptr;
    g_poolBytes = 0;
    g_records = 0;
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

bool ensureSlot(ID3D11DeviceContext* ctx, Slot& s) {
    if (s.staging && s.bytes == g_poolBytes) return true;
    if (s.staging) { s.staging->Release(); s.staging = nullptr; }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = g_poolBytes;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const bool ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &s.staging)) && s.staging;
    dev->Release();
    s.bytes = ok ? g_poolBytes : 0;
    return ok;
}

void issueCopy(ID3D11DeviceContext* ctx, bool keep) {
    for (Slot& s : g_ring) {
        if (s.inUse) continue;
        if (!ensureSlot(ctx, s)) { ++g_skipped; return; }
        ctx->CopyResource(s.staging, g_pool);
        s.frame = g_frame;
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
        if (s->keep) {
            g_keep.assign(bytes, bytes + s->bytes);
            g_keepFrame = s->frame;
            g_keepValid = true;
        } else if (g_keepValid && g_keepFrame + 1 == s->frame && g_keep.size() == s->bytes) {
            diffPair(g_keep.data(), bytes, s->bytes);
            g_keepValid = false;
        } else {
            ++g_skipped;
            g_keepValid = false;
        }
        ctx->Unmap(s->staging, 0);
        s->inUse = false;
    }
}

}  // namespace

void objectProbeConfigure(Config& cfg) {
    g_on = cfg.getBool("advanced.object_probe", false);
}

bool objectProbeWantsDraws() { return g_on; }

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
            report();
            releaseRing();
            releasePool();
            g_wasOn = false;
            g_noted = false;
        }
        return;
    }
    g_wasOn = true;
    ++g_frame;
    g_checksLeft = (g_pool && !dueMs(g_checkMs, kRecheckMs)) ? 0 : kChecksPerFrame;
    if (!ctx) return;
    guardedBudget(g_budget, [&] {
        if (g_pool) {
            g_framesWithoutPool = 0;
            const uint32_t phase = g_frame % kPairEvery;
            if (phase == 0 || phase == 1) issueCopy(ctx, phase == 0);
            poll(ctx);
        } else if (++g_framesWithoutPool == kAbsentFrames && !g_absentNoted) {
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
        report();
    }
}

void objectProbeShutdown() {
    releaseRing();
    releasePool();
    g_on = false;
    g_wasOn = false;
}

}  // namespace edvr
