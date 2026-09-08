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
constexpr int      kMaxBuckets = 64;
constexpr float    kRotQuantDeg = 0.01f; // the design's tolerance: a hundredth of a degree...
constexpr float    kPosQuantM = 0.01f;   // ...and a centimetre
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
uint64_t g_changed = 0, g_poseOnly = 0, g_other = 0, g_moved = 0;
uint32_t g_maxChanged = 0;
uint64_t g_bucketSum = 0;
uint32_t g_bucketMax = 0;
uint64_t g_bucketOverflow = 0;
uint64_t g_rebasePairs = 0;
double   g_rebaseMaxM = 0.0;
uint32_t g_poolChanges = 0;
uint64_t g_reportMs = 0;

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

// One rigid motion, quantised: the rotation's angle and axis and the
// translation p_prev - R p_now, which is the same for every part of one
// rigid assembly (the design's arithmetic), so a station's forty parts
// fall into one bucket and a passing ship into another.
struct MotionKey {
    int32_t v[7];
    bool operator==(const MotionKey& o) const { return memcmp(v, o.v, sizeof(v)) == 0; }
};

bool motionOf(const Pose& prev, const Pose& now, MotionKey* key, bool* pureTranslation,
              float* translationM) {
    float qd[4];
    quatMulConj(prev.q, now.q, qd);
    if (qd[3] < 0.0f) {
        for (float& c : qd) c = -c;
    }
    const float w = qd[3] > 1.0f ? 1.0f : qd[3];
    const float angleDeg = 2.0f * acosf(w) * 57.2957795f;
    float rp[3];
    quatRotate(qd, now.p, rp);
    const float t[3] = {prev.p[0] - rp[0], prev.p[1] - rp[1], prev.p[2] - rp[2]};
    const float tm = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    if (!std::isfinite(angleDeg) || !std::isfinite(tm)) return false;
    const bool still = angleDeg < 0.5f * kRotQuantDeg;
    key->v[0] = static_cast<int32_t>(lroundf(angleDeg / kRotQuantDeg));
    const float axisN = sqrtf(qd[0] * qd[0] + qd[1] * qd[1] + qd[2] * qd[2]);
    for (int i = 0; i < 3; ++i) {
        key->v[1 + i] = (still || axisN < 1e-6f) ? 0 : static_cast<int32_t>(lroundf(qd[i] / axisN * 100.0f));
        key->v[4 + i] = static_cast<int32_t>(lroundf(t[i] / kPosQuantM));
    }
    *pureTranslation = still;
    *translationM = tm;
    return true;
}

// The pair's diff: one sampled frame against the one before it.
void diffPair(const uint8_t* prev, const uint8_t* now, uint32_t bytes) {
    const uint32_t n = bytes / kRecordBytes;
    std::unordered_map<uint64_t, uint32_t> prevByHash;
    prevByHash.reserve(n);
    for (uint32_t i = 0; i < n; ++i) prevByHash[fnv1a(prev + i * kRecordBytes, kRecordBytes)] = i;

    uint32_t changed = 0, poseOnly = 0, other = 0, moved = 0;
    MotionKey keys[kMaxBuckets];
    uint32_t keyCount[kMaxBuckets] = {};
    bool keyPure[kMaxBuckets] = {};
    float keyT[kMaxBuckets] = {};
    int nk = 0;
    uint32_t overflow = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* a = prev + i * kRecordBytes;
        const uint8_t* b = now + i * kRecordBytes;
        if (memcmp(a, b, kRecordBytes) == 0) continue;
        ++changed;
        const bool restSame = memcmp(a, b, 4) == 0 &&
                              memcmp(a + 28, b + 28, kRecordBytes - 28) == 0;
        const bool poseDiff = memcmp(a + 4, b + 4, 24) != 0;
        if (restSame && poseDiff) {
            ++poseOnly;
            MotionKey k;
            bool pure = false;
            float tm = 0.0f;
            if (motionOf(decodePose(a), decodePose(b), &k, &pure, &tm)) {
                int found = -1;
                for (int j = 0; j < nk; ++j) {
                    if (keys[j] == k) { found = j; break; }
                }
                if (found >= 0) {
                    ++keyCount[found];
                } else if (nk < kMaxBuckets) {
                    keys[nk] = k;
                    keyCount[nk] = 1;
                    keyPure[nk] = pure;
                    keyT[nk] = tm;
                    ++nk;
                } else {
                    ++overflow;
                }
            }
        } else {
            ++other;
        }
        // The same bytes at another slot last frame: the record moved, which
        // is a repacked pool and the death of a per-slot identity.
        auto it = prevByHash.find(fnv1a(b, kRecordBytes));
        if (it != prevByHash.end() && it->second != i) ++moved;
    }
    // An origin rebase: more than half the pool's records took one and the
    // same pure translation.
    bool rebase = false;
    float rebaseM = 0.0f;
    for (int j = 0; j < nk; ++j) {
        if (keyPure[j] && keyCount[j] * 2 > n) {
            rebase = true;
            rebaseM = keyT[j];
        }
    }
    ++g_pairs;
    g_changed += changed;
    g_poseOnly += poseOnly;
    g_other += other;
    g_moved += moved;
    if (changed > g_maxChanged) g_maxChanged = changed;
    g_bucketSum += static_cast<uint64_t>(nk);
    if (static_cast<uint32_t>(nk) > g_bucketMax) g_bucketMax = static_cast<uint32_t>(nk);
    g_bucketOverflow += overflow;
    if (rebase) {
        ++g_rebasePairs;
        if (rebaseM > g_rebaseMaxM) g_rebaseMaxM = rebaseM;
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

void report() {
    if (!g_pairs && !g_skipped) return;
    const double pairs = g_pairs ? static_cast<double>(g_pairs) : 1.0;
    Log::get().note(
        "object probe: over %llu frame pairs (%llu skipped, a copy not ready in time): "
        "per pair %.0f records changed of %u (%.0f pose only, %.0f rewritten), at most %u; "
        "%.1f found at another slot per pair (a repacked pool, if not zero); distinct rigid "
        "motions among the changed: %.1f on average, %u at most%s; the whole pool's positions "
        "shifted together on %llu pairs (an origin rebase, up to %.1f m); the pool object "
        "changed %u times. Question 3 is 'found at another slot' near zero; question 6 is the "
        "motions figure.",
        static_cast<unsigned long long>(g_pairs), static_cast<unsigned long long>(g_skipped),
        static_cast<double>(g_changed) / pairs, g_records,
        static_cast<double>(g_poseOnly) / pairs, static_cast<double>(g_other) / pairs,
        g_maxChanged, static_cast<double>(g_moved) / pairs,
        static_cast<double>(g_bucketSum) / pairs, g_bucketMax,
        g_bucketOverflow ? " (and more past the table)" : "",
        static_cast<unsigned long long>(g_rebasePairs), g_rebaseMaxM, g_poolChanges);
    g_pairs = g_skipped = 0;
    g_changed = g_poseOnly = g_other = g_moved = 0;
    g_maxChanged = 0;
    g_bucketSum = 0;
    g_bucketMax = 0;
    g_bucketOverflow = 0;
    g_rebasePairs = 0;
    g_rebaseMaxM = 0.0;
    g_poolChanges = 0;
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
                    Log::get().note(
                        "object probe: the instanced-mesh pool is at VS t33 on the scene's draws -- "
                        "a structured buffer of %u bytes, %u records of %u (%.1f MB), object %p. Two "
                        "frames in a row are copied on the GPU every %u frames and read back late; the "
                        "totals every 20 s say whether a record keeps its slot between frames "
                        "(question 3 of docs\\per-object-motion.md), how many distinct rigid motions "
                        "a frame carries (question 6) and when the origin rebased (question 7). "
                        "Nothing on the draw path but one shader-resource read a second.",
                        g_poolBytes, g_records, kRecordBytes,
                        static_cast<double>(g_poolBytes) / 1048576.0, kPairEvery,
                        static_cast<void*>(g_pool));
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
