#pragma once
// F44 (TEMPORARY instrument, fix.native_on_foot_marker_probe; docs\design-native-onfoot-vr-2026-10-13.md): tag a value the game builds on its
// own thread, find the GPU pool record it is copied into, and name the draw that reads that record. Written for the waypoint marker; meant to be
// reused for the weapon ammo readout. Measure only: nothing here writes a constant buffer, a pool or a binding. F45 adds the whole-stream
// match (matchStream), the per-eye view cache (ViewCache, markerAngle) and the per-frame append staging (FrameStaging) the draw side uses.
//
// THE TAG. A 24-bit sentinel (an 8-bit key and a 16-bit frame count, salted so key 0 at frame 0 is not all zero bits) in the lowest 8 mantissa
// bits of three floats: x carries sentinel bits 0-7, y bits 8-15, z bits 16-23. Each component moves by at most 255 ulps, under 2^-15 of its
// size (3e-5 relative), and never changes exponent or sign. A zero, denormal, infinite or NaN component is not tagged (the tag would change
// what it is), and the record scan refuses such components for the same reason: an all-zero record never matches.
//
// THE FIND. Each record of a structured pool (stride 336, the position at +16) is read with one 16-byte load at +16 and its low bits are
// compared against the sentinels in flight. The caller supplies the load: on mapped (write-combined) memory a streaming load reads a whole line
// at line speed (streamLoad16 below), where an ordinary load is uncached.
//
// THE MATCH. A draw's instance stream entry (stride 8; x is the record index counted from the t33 view's FirstElement) names the record the
// draw reads: record = FirstElement + x. The view-space position the vertex shader forms is rows . (X - o, 1): the view rows (b0 rows 9-11)
// applied to the record position less the scene origin (b1 row 275).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <smmintrin.h>   // _mm_stream_load_si128 (SSE4.1)

namespace edvr {
namespace drawtag {

constexpr uint32_t kRecordStride = 336;     // the t33 pool record
constexpr uint32_t kPositionOffset = 16;    // the record's position (xyz floats)
constexpr uint32_t kStreamStride = 8;       // the instance stream entry (INSTANCEANDMODELDATAINDEX.xy)
constexpr uint32_t kSalt = 0x00A5C35Au;
constexpr uint32_t kSentinelMask = 0x00FFFFFFu;

inline uint32_t sentinel(uint32_t key, uint32_t frame) noexcept {
    return ((((key & 0xFFu) << 16) | (frame & 0xFFFFu)) ^ kSalt) & kSentinelMask;
}
inline uint32_t sentinelKey(uint32_t s) noexcept { return ((s ^ kSalt) >> 16) & 0xFFu; }
inline uint32_t sentinelFrame(uint32_t s) noexcept { return (s ^ kSalt) & 0xFFFFu; }

inline uint32_t bitsOf(float f) noexcept {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
inline float floatOf(uint32_t u) noexcept {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
// A normal, finite float: the only class the tag may touch.
inline bool taggableBits(uint32_t u) noexcept {
    const uint32_t e = (u >> 23) & 0xFFu;
    return e != 0u && e != 0xFFu;
}

// Writes the sentinel into v's low mantissa bits; false (v unchanged) when a component is not taggable.
inline bool encode(float v[3], uint32_t s) noexcept {
    for (int i = 0; i < 3; ++i)
        if (!taggableBits(bitsOf(v[i]))) return false;
    for (int i = 0; i < 3; ++i) v[i] = floatOf((bitsOf(v[i]) & ~0xFFu) | ((s >> (8 * i)) & 0xFFu));
    return true;
}
inline uint32_t decodeWords(const uint32_t w[3]) noexcept {
    return (w[0] & 0xFFu) | ((w[1] & 0xFFu) << 8) | ((w[2] & 0xFFu) << 16);
}
inline uint32_t decode(const float v[3]) noexcept {
    const uint32_t w[3] = {bitsOf(v[0]), bitsOf(v[1]), bitsOf(v[2])};
    return decodeWords(w);
}

// The 16 bytes at p (16-byte aligned), read with a streaming load where the CPU has one (write-combined memory), else copied.
inline bool cpuHasSse41() noexcept {
    static const bool has = [] {
        int r[4] = {};
        __cpuid(r, 1);
        return (r[2] & (1 << 19)) != 0;
    }();
    return has;
}
inline void streamLoad16(const uint8_t* p, uint32_t out[4]) noexcept {
    if (cpuHasSse41() && (reinterpret_cast<uintptr_t>(p) & 15u) == 0) {
        const __m128i v = _mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<uint8_t*>(p)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out), v);
    } else {
        std::memcpy(out, p, 16);
    }
}
inline void plainLoad16(const uint8_t* p, uint32_t out[4]) noexcept { std::memcpy(out, p, 16); }

struct Hit {
    uint32_t index = 0;      // record index in the buffer (byte offset / stride)
    uint32_t sentinel = 0;
    float pos[3] = {};       // the record's position, as stored (the tag included)
};

// Record `index` against the sentinels: true with `hit` filled when its position carries one of them.
template <class Load>
inline bool checkRecord(const uint8_t* base, uint32_t bytes, uint32_t index, const uint32_t* wants, uint32_t nWants, Hit& hit,
                        Load load) noexcept {
    const uint64_t at = static_cast<uint64_t>(index) * kRecordStride + kPositionOffset;
    if (at + 16 > bytes) return false;
    uint32_t w[4];
    load(base + at, w);
    if (!taggableBits(w[0]) || !taggableBits(w[1]) || !taggableBits(w[2])) return false;
    const uint32_t s = decodeWords(w);
    for (uint32_t k = 0; k < nWants; ++k) {
        if (wants[k] == s) {
            hit.index = index;
            hit.sentinel = s;
            for (int i = 0; i < 3; ++i) hit.pos[i] = floatOf(w[i]);
            return true;
        }
    }
    return false;
}

// Every record of [base, base + bytes) against the sentinels; at most maxHits hits, in index order. Returns the hit count.
template <class Load>
inline uint32_t scanRecords(const uint8_t* base, uint32_t bytes, const uint32_t* wants, uint32_t nWants, Hit* hits, uint32_t maxHits,
                            Load load) noexcept {
    if (!base || !nWants || !maxHits) return 0;
    const uint32_t records = bytes / kRecordStride;
    uint32_t n = 0;
    for (uint32_t i = 0; i < records && n < maxHits; ++i)
        if (checkRecord(base, bytes, i, wants, nWants, hits[n], load)) ++n;
    return n;
}

// rows: three float4 rows (b0 rows 9-11), applied to (p - o, 1), in double.
inline void viewPoint(const float rows[12], const double p[3], const double o[3], double out[3]) noexcept {
    const double d[3] = {p[0] - o[0], p[1] - o[1], p[2] - o[2]};
    for (int r = 0; r < 3; ++r)
        out[r] = rows[4 * r] * d[0] + rows[4 * r + 1] * d[1] + rows[4 * r + 2] * d[2] + rows[4 * r + 3];
}
inline double length3(const double v[3]) noexcept { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
// The angle between two directions in degrees; -1 when either is (near) zero.
inline double angleDeg(const double a[3], const double b[3]) noexcept {
    const double la = length3(a), lb = length3(b);
    if (!(la > 1e-9) || !(lb > 1e-9)) return -1.0;
    double c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
    if (c > 1.0) c = 1.0;
    if (c < -1.0) c = -1.0;
    return std::acos(c) * 180.0 / 3.14159265358979323846;
}
// The 3x3 part of three float4 rows is a rotation (unit rows, mutually orthogonal, within tol).
inline bool rowsRotation(const float rows[12], double tol) noexcept {
    for (int i = 0; i < 3; ++i) {
        const double n = double(rows[4 * i]) * rows[4 * i] + double(rows[4 * i + 1]) * rows[4 * i + 1] + double(rows[4 * i + 2]) * rows[4 * i + 2];
        if (!(std::fabs(n - 1.0) < tol)) return false;
    }
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b) {
            const double d = double(rows[4 * a]) * rows[4 * b] + double(rows[4 * a + 1]) * rows[4 * b + 1] + double(rows[4 * a + 2]) * rows[4 * b + 2];
            if (!(std::fabs(d) < tol)) return false;
        }
    return true;
}
// The angle in degrees between the rotations of two row triples: arccos((trace(A B^T) - 1) / 2).
inline double rowsAngleDeg(const float a[12], const float b[12]) noexcept {
    double tr = 0.0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) tr += double(a[4 * r + c]) * b[4 * r + c];
    double c = (tr - 1.0) * 0.5;
    if (c > 1.0) c = 1.0;
    if (c < -1.0) c = -1.0;
    return std::acos(c) * 180.0 / 3.14159265358979323846;
}

// F45: THE STREAM MATCH over a draw's whole instance stream (n entries of stride 8, x then y): record = first + x. The y component (the model
// data index) is checked the same way and counted apart, so a wrong guess about which component names the record shows as y hits rather than
// as "read by no draw".
struct StreamMatch {
    uint32_t xHits = 0, yHits = 0;   // entries whose x (y) names a wanted record
    uint32_t entry = 0;              // the first x hit's entry (the first y hit's when no x hit)
    uint32_t record = 0;             // the record it names
    uint32_t want = 0;               // its position in wants
    bool viaY = false;
};
inline bool matchStream(const uint8_t* entries, uint32_t n, uint32_t first, const uint32_t* wants, uint32_t nWants, StreamMatch& m) noexcept {
    m = StreamMatch{};
    bool any = false;
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t e[2];
        std::memcpy(e, entries + static_cast<size_t>(k) * kStreamStride, sizeof(e));
        for (int c = 0; c < 2; ++c) {
            const uint32_t rec = first + e[c];
            for (uint32_t w = 0; w < nWants; ++w) {
                if (wants[w] != rec) continue;
                if (c == 0) ++m.xHits;
                else ++m.yHits;
                if (!any || (m.viaY && c == 0)) {
                    m.entry = k;
                    m.record = rec;
                    m.want = w;
                    m.viaY = c == 1;
                    any = true;
                }
                break;
            }
        }
    }
    return any;
}

// F45: THE VIEW CACHE. An eye's view as a draw read it: b0 rows 9-11 and b1 row 275, by frame and eye; the last N kept.
struct View {
    bool valid = false;
    uint32_t frame = 0;
    int eye = -1;
    float rows[12] = {};
    float o[4] = {};
};
inline double rowsDistance(const float a[12], const float b[12]) noexcept {
    double s = 0.0;
    for (int i = 0; i < 12; ++i) s += std::fabs(static_cast<double>(a[i]) - b[i]);
    return s;
}
template <uint32_t N>
class ViewCache {
public:
    void put(const View& v) noexcept {
        views_[next_] = v;
        views_[next_].valid = true;
        last_ = static_cast<int>(next_);
        next_ = (next_ + 1) % N;
    }
    const View* get(uint32_t frame, int eye) const noexcept {
        for (const View& v : views_)
            if (v.valid && v.frame == frame && v.eye == eye) return &v;
        return nullptr;
    }
    const View* latest() const noexcept { return last_ >= 0 ? &views_[last_] : nullptr; }
    // The eye of `frame` whose rows are nearest to `rows`: 0 or 1, or -1 when the frame has neither eye's view. *dOther is negative when
    // only one eye's view was there (nothing to tell apart).
    int nearestEye(uint32_t frame, const float rows[12], double* dChosen, double* dOther) const noexcept {
        const View* w0 = get(frame, 0);
        const View* w1 = get(frame, 1);
        *dChosen = *dOther = -1.0;
        if (!w0 && !w1) return -1;
        const double d0 = w0 ? rowsDistance(rows, w0->rows) : -1.0, d1 = w1 ? rowsDistance(rows, w1->rows) : -1.0;
        const int eye = !w0 ? 1 : !w1 ? 0 : d0 <= d1 ? 0 : 1;
        *dChosen = eye == 0 ? d0 : d1;
        *dOther = eye == 0 ? d1 : d0;
        return eye;
    }

private:
    View views_[N];
    uint32_t next_ = 0;
    int last_ = -1;
};

// v0 = rowsA . (x0 - oA, 1) and vt = rowsB . (p - oB, 1); the angle between them in degrees, -1 when either is at the eye.
inline double markerAngle(const float rowsA[12], const float oA[4], const float x0[3], const float rowsB[12], const float oB[4], const float p[3],
                          double v0[3], double vt[3]) noexcept {
    const double X[3] = {x0[0], x0[1], x0[2]}, a[3] = {oA[0], oA[1], oA[2]}, P[3] = {p[0], p[1], p[2]}, b[3] = {oB[0], oB[1], oB[2]};
    viewPoint(rowsA, X, a, v0);
    viewPoint(rowsB, P, b, vt);
    return angleDeg(v0, vt);
}

// F46: THE NDC PAIR. The world pool draws' view-projection is b1 rows 270..273 in row-vector form, clip = x r270 + y r271 + z r272 + r273,
// applied to (P - o, 1) with o = b1 row 275 (rows[16]: rows 270..273, row after row). The marker composite (VS B10B032BDFD46700) reads b0
// rows 4, 5 and 7 only: clip.x = r4 . p, clip.y = r5 . p and w = |r7 . p|, with p = (X0 - o_hud, 1).
inline void worldClip(const float rows[16], const double p[3], const double o[3], double clip[4]) noexcept {
    const double v[3] = {p[0] - o[0], p[1] - o[1], p[2] - o[2]};
    for (int j = 0; j < 4; ++j) clip[j] = v[0] * rows[j] + v[1] * rows[4 + j] + v[2] * rows[8 + j] + rows[12 + j];
}
// clip[0] = r4 . p, clip[1] = r5 . p, clip[2] = r7 . p (signed: the shader takes its absolute value).
inline void markerClip(const float r4[4], const float r5[4], const float r7[4], const double x0[3], const double o[3], double clip[3]) noexcept {
    const double v[4] = {x0[0] - o[0], x0[1] - o[1], x0[2] - o[2], 1.0};
    clip[0] = clip[1] = clip[2] = 0.0;
    for (int i = 0; i < 4; ++i) {
        clip[0] += r4[i] * v[i];
        clip[1] += r5[i] * v[i];
        clip[2] += r7[i] * v[i];
    }
}
// The angle in degrees between the view rays through two NDC points of the world projection `rows`; -1 when the rows have no forward axis.
// The eye is where clip x, y and w all vanish, so a ray d from it lands at ndc = (c_x . d, c_y . d) / (c_w . d) for the columns c of the
// rows' 3x3 part. Forward is c_w; each of c_x and c_y splits into an offset along forward and a scale along an axis normal to it, so the ray
// through (nx, ny) is forward + right (nx - ox) / sx + up (ny - oy) / sy.
inline double ndcAngleDeg(const float rows[16], const double a[2], const double b[2]) noexcept {
    const int which[3] = {0, 1, 3};
    double col[3][3];
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 3; ++i) col[c][i] = rows[4 * i + which[c]];
    const double len = length3(col[2]);
    if (!(len > 1e-12)) return -1.0;
    const double fwd[3] = {col[2][0] / len, col[2][1] / len, col[2][2] / len};
    double axis[2][3], off[2], scale[2];
    for (int k = 0; k < 2; ++k) {
        const double u[3] = {col[k][0] / len, col[k][1] / len, col[k][2] / len};
        off[k] = u[0] * fwd[0] + u[1] * fwd[1] + u[2] * fwd[2];
        for (int i = 0; i < 3; ++i) axis[k][i] = u[i] - off[k] * fwd[i];
        scale[k] = length3(axis[k]);
        if (!(scale[k] > 1e-12)) return -1.0;
        for (int i = 0; i < 3; ++i) axis[k][i] /= scale[k];
    }
    double ra[3], rb[3];
    const double tax = (a[0] - off[0]) / scale[0], tay = (a[1] - off[1]) / scale[1];
    const double tbx = (b[0] - off[0]) / scale[0], tby = (b[1] - off[1]) / scale[1];
    for (int i = 0; i < 3; ++i) {
        ra[i] = fwd[i] + axis[0][i] * tax + axis[1][i] * tay;
        rb[i] = fwd[i] + axis[0][i] * tbx + axis[1][i] * tby;
    }
    return angleDeg(ra, rb);
}
// F47: the per-eye NDC offset on a private copy of the marker composite's b0. row4' = row4 + dx row7 and row5' = row5 + dy row7 move the
// marker's NDC by (dx, dy) wherever r7 . p > 0 (the shader divides by |w|: behind the eye the move flips).
inline void ndcOffsetRows(float r4[4], float r5[4], const float r7[4], double dx, double dy) noexcept {
    for (int i = 0; i < 4; ++i) {
        r4[i] = static_cast<float>(r4[i] + dx * r7[i]);
        r5[i] = static_cast<float>(r5[i] + dy * r7[i]);
    }
}
// F47: THE DELTA. The same point P through both views: (dx, dy) = ndc(P through the world rows at o_w) - ndc(P through the marker composite's
// rows 4, 5, 7 at o_hud). One point through both cancels the icon's design height above P. The gate: both w > 0 and |dx|, |dy| < gate (NDC).
enum NdcDeltaWhy : int { kDeltaOk = 0, kDeltaWorldBehind = 1, kDeltaMarkerBehind = 2, kDeltaGate = 3 };
inline int ndcDelta(const float world[16], const double ow[3], const float r4[4], const float r5[4], const float r7[4], const double oh[3],
                    const double P[3], double gate, double* dx, double* dy) noexcept {
    *dx = *dy = 0.0;
    double wc[4], mc[3];
    worldClip(world, P, ow, wc);
    if (!(wc[3] > 1e-9)) return kDeltaWorldBehind;
    markerClip(r4, r5, r7, P, oh, mc);
    if (!(mc[2] > 1e-9)) return kDeltaMarkerBehind;
    *dx = wc[0] / wc[3] - mc[0] / mc[2];
    *dy = wc[1] / wc[3] - mc[1] / mc[2];
    if (!(std::fabs(*dx) < gate) || !(std::fabs(*dy) < gate)) return kDeltaGate;
    return kDeltaOk;
}
// F50: THE LAG DELTA. F47's delta put the WORLD point P through the marker's b0, which lives in the tracking (seat) frame, so it was never
// small. Here each projection gets its own point: ndc_marker = the record's X0 through the draw's b0 rows 4, 5, 7 at o_hud (what the marker
// draw itself shows), ndc_P = P through this eye's world rows at o_w; m = ndc_marker - ndc_P. c is the eye's static calibration (the
// frustum-centre difference between the HUD's and the world's projections: m with the head still); (dx, dy) = -(m - c) moves the marker to
// ndc_P + c. The gate: both w > 0 and |dx|, |dy| < gate (NDC). m is filled whenever both w > 0 (also past the gate).
inline int ndcLagDelta(const float world[16], const double ow[3], const float r4[4], const float r5[4], const float r7[4], const double oh[3],
                       const double X0[3], const double P[3], const double c[2], double gate, double m[2], double* dx, double* dy) noexcept {
    *dx = *dy = 0.0;
    m[0] = m[1] = 0.0;
    double wc[4], mc[3];
    worldClip(world, P, ow, wc);
    if (!(wc[3] > 1e-9)) return kDeltaWorldBehind;
    markerClip(r4, r5, r7, X0, oh, mc);
    if (!(mc[2] > 1e-9)) return kDeltaMarkerBehind;
    m[0] = mc[0] / mc[2] - wc[0] / wc[3];
    m[1] = mc[1] / mc[2] - wc[1] / wc[3];
    *dx = -(m[0] - c[0]);
    *dy = -(m[1] - c[1]);
    if (!(std::fabs(*dx) < gate) || !(std::fabs(*dy) < gate)) return kDeltaGate;
    return kDeltaOk;
}
// F50: an eye's baseline c. A frame qualifies when the head is under 3 deg/s and |m - c| < 0.01 NDC (any m while the seed is empty). The
// first 30 qualifying frames seed it (their mean, their variance); after that an EMA (alpha 0.05) of m, its variance an EMA beside it. 90
// slow-head frames in a row refused for distance start the seed again (a first sample taken in motion, or a frustum change not caught).
struct LagBaseline {
    static constexpr uint32_t kSeedN = 30, kMissReset = 90;
    static constexpr double kAlpha = 0.05, kHeadDps = 3.0, kNear = 0.01;
    bool seeded = false;
    uint32_t n = 0, misses = 0;
    double c[2] = {}, var[2] = {}, sum[2] = {}, sumSq[2] = {};
    void reset() noexcept { *this = LagBaseline{}; }
    // True when the frame was taken; *restarted when the misses started the seed again.
    bool update(const double m[2], double headDps, bool* restarted = nullptr) noexcept {
        if (restarted) *restarted = false;
        if (!(headDps >= 0.0) || !(headDps < kHeadDps)) return false;
        if (n > 0) {
            const double ex = m[0] - c[0], ey = m[1] - c[1];
            if (!(std::sqrt(ex * ex + ey * ey) < kNear)) {
                if (++misses >= kMissReset) {
                    reset();
                    if (restarted) *restarted = true;
                }
                return false;
            }
        }
        misses = 0;
        if (!seeded) {
            ++n;
            for (int k = 0; k < 2; ++k) {
                sum[k] += m[k];
                sumSq[k] += m[k] * m[k];
                c[k] = sum[k] / n;
                const double v = sumSq[k] / n - c[k] * c[k];
                var[k] = v > 0.0 ? v : 0.0;
            }
            if (n >= kSeedN) seeded = true;
            return true;
        }
        ++n;
        for (int k = 0; k < 2; ++k) {
            const double d = m[k] - c[k];
            c[k] += kAlpha * d;
            var[k] = (1.0 - kAlpha) * (var[k] + kAlpha * d * d);
        }
        return true;
    }
    // The standard deviation in pixels of a W x H target (NDC spans 2).
    double stdPx(double W, double H) const noexcept {
        const double sx = std::sqrt(var[0]) * W * 0.5, sy = std::sqrt(var[1]) * H * 0.5;
        return sx > sy ? sx : sy;
    }
    bool ready(double W, double H, double maxStdPx) const noexcept { return seeded && stdPx(W, H) < maxStdPx; }
};

// F52: THE TAG CELLS. One cell per 8-bit tag key (a key's first post of a frame in key & 0xFF; a second post's key carries bit 7), so two
// keys never share a cell (F47's 32 cells put keys 16 apart in one cell, and the later post overwrote the earlier before it was read).
constexpr uint32_t kTagCells = 256;
inline uint32_t tagCellOf(uint32_t key) noexcept { return key & (kTagCells - 1u); }

// F52: THE REFERENCE. Which eye view the marker correction measures against: world_first (F46: b1 rows 270..273 of the eye's first world pool
// draw) or temporal (the temporal pass's camera rows for the eye, at the world rows' projection and eye: swapRotation). The key's value
// "world_first" asks for world_first; anything else (the default, "temporal") asks for temporal, which is used while the temporal pass has
// given the eye's rows recently and world_first otherwise.
enum RefMode : int { kRefWorldFirst = 0, kRefTemporal = 1 };
inline int parseRefMode(const char* value) noexcept {
    return value && std::strcmp(value, "world_first") == 0 ? kRefWorldFirst : kRefTemporal;
}
inline int chooseReference(int asked, bool temporalRecent) noexcept {
    return asked == kRefTemporal && temporalRecent ? kRefTemporal : kRefWorldFirst;
}

// F52: THE VIEW BASES. World rows' basis: forward = c_w normalised; right = c_x less its forward part, normalised; up = c_y less its forward
// and right parts, normalised (c_k the 3-vector (rows[k], rows[4 + k], rows[8 + k])). False when a column is degenerate.
inline bool worldBasis(const float rows[16], double right[3], double up[3], double fwd[3]) noexcept {
    double cx[3], cy[3], cw[3];
    for (int i = 0; i < 3; ++i) {
        cx[i] = rows[4 * i + 0];
        cy[i] = rows[4 * i + 1];
        cw[i] = rows[4 * i + 3];
    }
    const double lw = length3(cw);
    if (!(lw > 1e-12)) return false;
    for (int i = 0; i < 3; ++i) fwd[i] = cw[i] / lw;
    const double xf = cx[0] * fwd[0] + cx[1] * fwd[1] + cx[2] * fwd[2];
    for (int i = 0; i < 3; ++i) right[i] = cx[i] - xf * fwd[i];
    const double lr = length3(right);
    if (!(lr > 1e-12)) return false;
    for (int i = 0; i < 3; ++i) right[i] /= lr;
    const double yf = cy[0] * fwd[0] + cy[1] * fwd[1] + cy[2] * fwd[2], yr = cy[0] * right[0] + cy[1] * right[1] + cy[2] * right[2];
    for (int i = 0; i < 3; ++i) up[i] = cy[i] - yf * fwd[i] - yr * right[i];
    const double lu = length3(up);
    if (!(lu > 1e-12)) return false;
    for (int i = 0; i < 3; ++i) up[i] /= lu;
    return true;
}
// The temporal pass's camera rows (view->world [R | c], cam[4r + c]) as a basis: right = R column 0, up = column 1, forward = s . column 2,
// s = +1 or -1 whichever lies nearer fwdRef (the view's z convention is not assumed). False when a column is degenerate.
inline bool cameraBasis(const float cam[12], const double fwdRef[3], double right[3], double up[3], double fwd[3], int* sign) noexcept {
    double* out[3] = {right, up, fwd};
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) out[c][r] = cam[4 * r + c];
        const double l = length3(out[c]);
        if (!(l > 1e-12)) return false;
        for (int r = 0; r < 3; ++r) out[c][r] /= l;
    }
    const double d = fwd[0] * fwdRef[0] + fwd[1] * fwdRef[1] + fwd[2] * fwdRef[2];
    *sign = d < 0.0 ? -1 : 1;
    if (d < 0.0)
        for (int r = 0; r < 3; ++r) fwd[r] = -fwd[r];
    return true;
}
inline double det3(const double a[3], const double b[3], const double c[3]) noexcept {
    return a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
}
// Two bases against each other: each axis's angle (deg), and the rotation angle between them when both have the same handedness (else -1).
struct BasisDiff {
    double total = -1.0, right = -1.0, up = -1.0, fwd = -1.0;
    bool sameHand = false;
};
inline BasisDiff basisDiff(const double rA[3], const double uA[3], const double fA[3], const double rB[3], const double uB[3],
                           const double fB[3]) noexcept {
    BasisDiff d;
    d.right = angleDeg(rA, rB);
    d.up = angleDeg(uA, uB);
    d.fwd = angleDeg(fA, fB);
    d.sameHand = (det3(rA, uA, fA) > 0.0) == (det3(rB, uB, fB) > 0.0);
    if (d.sameHand) {
        const double tr = rA[0] * rB[0] + rA[1] * rB[1] + rA[2] * rB[2] + uA[0] * uB[0] + uA[1] * uB[1] + uA[2] * uB[2] + fA[0] * fB[0] +
                          fA[1] * fB[1] + fA[2] * fB[2];
        double c = (tr - 1.0) * 0.5;
        if (c > 1.0) c = 1.0;
        if (c < -1.0) c = -1.0;
        d.total = std::acos(c) * 180.0 / 3.14159265358979323846;
    }
    return d;
}
// F52: world rows with their rotation replaced: every column c_k (k = x, y, z, w) keeps its components along the world basis (A) but takes
// them along basis B; the eye (where clip x, y and w vanish, relative to o) stays where it was, and clip z there keeps its value. With B = A
// the rows come back unchanged. False when the eye cannot be solved (the x, y, w columns are not independent).
inline bool swapRotation(const float world[16], const double rA[3], const double uA[3], const double fA[3], const double rB[3], const double uB[3],
                         const double fB[3], float out[16]) noexcept {
    double col[4][3], t[4];
    for (int k = 0; k < 4; ++k) {
        for (int i = 0; i < 3; ++i) col[k][i] = world[4 * i + k];
        t[k] = world[12 + k];
    }
    // The eye v: v . c_x = -t_x, v . c_y = -t_y, v . c_w = -t_w (Cramer, the columns as the system's rows).
    const double* M[3] = {col[0], col[1], col[3]};
    const double rhs[3] = {-t[0], -t[1], -t[3]};
    const double D = det3(M[0], M[1], M[2]);
    if (!(std::fabs(D) > 1e-18)) return false;
    double v[3];
    for (int j = 0; j < 3; ++j) {
        double m[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) m[r][c] = c == j ? rhs[r] : M[r][c];
        v[j] = det3(m[0], m[1], m[2]) / D;
    }
    for (int k = 0; k < 4; ++k) {
        const double a = col[k][0] * rA[0] + col[k][1] * rA[1] + col[k][2] * rA[2];
        const double b = col[k][0] * uA[0] + col[k][1] * uA[1] + col[k][2] * uA[2];
        const double c = col[k][0] * fA[0] + col[k][1] * fA[1] + col[k][2] * fA[2];
        double n[3];
        for (int i = 0; i < 3; ++i) n[i] = a * rB[i] + b * uB[i] + c * fB[i];
        const double vOld = v[0] * col[k][0] + v[1] * col[k][1] + v[2] * col[k][2], vNew = v[0] * n[0] + v[1] * n[1] + v[2] * n[2];
        for (int i = 0; i < 3; ++i) out[4 * i + k] = static_cast<float>(n[i]);
        out[12 + k] = static_cast<float>(t[k] + vOld - vNew);
    }
    return true;
}

// F52: THE KEY TABLE. N slots, each named by a tag key: find, or acquire (an unused slot, else the one seen longest ago; *fresh says the slot
// was (re)claimed and its state must be reset by the caller).
template <uint32_t N>
struct KeyIndex {
    uint32_t key[N] = {}, last[N] = {};
    bool used[N] = {};
    int find(uint32_t k) const noexcept {
        for (uint32_t i = 0; i < N; ++i)
            if (used[i] && key[i] == k) return static_cast<int>(i);
        return -1;
    }
    int acquire(uint32_t k, uint32_t frame, bool* fresh) noexcept {
        *fresh = false;
        int i = find(k);
        if (i < 0) {
            for (uint32_t j = 0; j < N && i < 0; ++j)
                if (!used[j]) i = static_cast<int>(j);
            if (i < 0) {
                i = 0;
                for (uint32_t j = 1; j < N; ++j)
                    if (frame - last[j] > frame - last[i]) i = static_cast<int>(j);
            }
            used[i] = true;
            key[i] = k;
            *fresh = true;
        }
        last[i] = frame;
        return i;
    }
};

// F52: THE BOOKKEEPING. An eye's marker composite draws at read-back: tagged (read a found record by x), read but untagged (its stream read,
// no record of a find), and not copied (its stream was not staged: unknown).
struct B10bTally {
    uint64_t draws = 0, tagged = 0, notCopied = 0, skipped = 0;
    // F63: skipped: not copied on purpose (the CPU identification named a record that is neither a find nor a held match's record: an
    // untagged composite), so "not copied" stays a loss.
    void note(bool streamRead, bool matchedByX, bool skippedByCpu = false) noexcept {
        ++draws;
        if (skippedByCpu) ++skipped;
        else if (!streamRead) ++notCopied;
        else if (matchedByX) ++tagged;
    }
    uint64_t untagged() const noexcept { return draws - tagged - notCopied - skipped; }
};

// F53: THE SCENE REFERENCE. The F52 audit's world rows came from the first-person body pass (VS 7B0DC42D383F694C, PS 0DF03E64DF9DBEF1), whose
// camera is head-locked. A world draw gives the eye's reference rows only when it is a main-scene pass: not the first-person body's pixel
// shader, the depth view one of the scene pair (depth probe), and one of the scene geometry vertex shaders. The first refusal that holds.
constexpr uint64_t kFpBodyPs = 0x0DF03E64DF9DBEF1ull;
constexpr uint64_t kSceneVs[3] = {0xEB5234DB6ADB491Dull, 0xDE545DC8EE4FBB87ull, 0x5B4D8E894EEDA8B4ull};
enum SceneRefWhy : int { kSceneOk = 0, kSceneFpBodyPs = 1, kSceneNotSceneDepth = 2, kSceneNotSceneVs = 3 };
inline int sceneRefWhy(uint64_t vs, uint64_t ps, bool sceneDepth) noexcept {
    if (ps == kFpBodyPs) return kSceneFpBodyPs;
    if (!sceneDepth) return kSceneNotSceneDepth;
    for (uint64_t s : kSceneVs)
        if (vs == s) return kSceneOk;
    return kSceneNotSceneVs;
}
// The scene rows are trusted on a frame when P through them and through the temporal pass's camera (the temporal reference) agree under
// kSceneTrustPx pixels of the marker's target. A frame without the comparison (no temporal rows, its axes do not correspond, no P) is untrusted.
// F62c: measure only (the F52 per-eye line); the patch no longer reads it (patchDecide).
constexpr double kSceneTrustPx = 1.0;
inline bool sceneRowsTrusted(bool compared, double px) noexcept { return compared && px >= 0.0 && px < kSceneTrustPx; }
// Two world projections' rotations: the angle between their forward axes and (same handedness) the whole rotation, in degrees; -1 when a
// basis is degenerate (or the handedness differs, for the rotation).
inline void rowsBasisAngles(const float a[16], const float b[16], double* fwdDeg, double* rotDeg) noexcept {
    *fwdDeg = *rotDeg = -1.0;
    double rA[3], uA[3], fA[3], rB[3], uB[3], fB[3];
    if (!worldBasis(a, rA, uA, fA) || !worldBasis(b, rB, uB, fB)) return;
    const BasisDiff d = basisDiff(rA, uA, fA, rB, uB, fB);
    *fwdDeg = d.fwd;
    *rotDeg = d.total;
}

// ---- F66f: THE MARKER'S OFFSET FROM ITS OWN GEOMETRY, AND THE AFFINE ROW PATCH ------------------------------------------------------------
// The F65 measurement (4 series, both eyes, 124 samples): the drawn dot sits at P, not 36.3 px above it, and its static error is a scale of about
// 0.94% about EACH EYE'S OPTICAL AXIS (kx 0.011, ky 0.0081; both eyes agree; a scale about NDC 0 is excluded): 12 px at 30 deg off axis. The marker's
// projection (b0 rows 4, 5, 7) does not have the focal scale of the texture it is composited into, so a translation of the anchor to ndc_P + e
// leaves the dot short of its place by (sigma - 1) times its offset from the axis. Two projections are compared through their decomposition: the
// NDC of the optical axis (off) and the NDC per unit tangent (scale) per axis, read from the rows alone (ndcAngleDeg's decomposition).
//   sigma = scale_M / scale_W per axis; the display of a UI NDC n is axis + (n - off_M) / sigma, so for the anchor to be SHOWN at the target T
//   (texture NDC; P through the scene rows less the TAA jitter, plus the icon offset) and every other vertex at the offset the game designed, the
//   rows become   n_final = sigma n_M + u,  u = off_M + sigma (T - axis) - sigma n_M(X0),   i.e.  r4' = sigma_x r4 + u_x r7,  r5' = sigma_y r5 + u_y r7.
// sigma = 1 with off_M = axis gives u = T - n_M(X0): today's translation. The axis is the texture's (the runtime's per-eye tangents, unjittered); the
// scene rows' own principal point less the axis is the jitter they carry, which P must not (the dot does not).
struct ProjDecomp {
    bool ok = false;
    double off[2] = {}, scale[2] = {};
};
inline bool projDecompose(const double cx[3], const double cy[3], const double cw[3], ProjDecomp& o) noexcept {
    o = ProjDecomp{};
    const double len = length3(cw);
    if (!(len > 1e-12)) return false;
    const double fwd[3] = {cw[0] / len, cw[1] / len, cw[2] / len};
    const double* c[2] = {cx, cy};
    for (int k = 0; k < 2; ++k) {
        const double u[3] = {c[k][0] / len, c[k][1] / len, c[k][2] / len};
        const double off = u[0] * fwd[0] + u[1] * fwd[1] + u[2] * fwd[2];
        const double ax[3] = {u[0] - off * fwd[0], u[1] - off * fwd[1], u[2] - off * fwd[2]};
        const double sc = length3(ax);
        if (!(sc > 1e-12) || !std::isfinite(off) || !std::isfinite(sc)) return false;
        o.off[k] = off;
        o.scale[k] = sc;
    }
    o.ok = true;
    return true;
}
// The world rows (b1 270..273, row-vector form: column j of the clip is rows[j], rows[4 + j], rows[8 + j]): x, y and w are the columns 0, 1, 3.
inline bool projDecomposeWorld(const float rows[16], ProjDecomp& o) noexcept {
    double c[3][3];
    static const int kWhich[3] = {0, 1, 3};
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 3; ++i) c[k][i] = rows[4 * i + kWhich[k]];
    return projDecompose(c[0], c[1], c[2], o);
}
// The marker composite's b0 rows 4, 5 and 7: x, y and w are r4 . p, r5 . p, r7 . p.
inline bool projDecomposeMarker(const float r4[4], const float r5[4], const float r7[4], ProjDecomp& o) noexcept {
    const double cx[3] = {r4[0], r4[1], r4[2]}, cy[3] = {r5[0], r5[1], r5[2]}, cw[3] = {r7[0], r7[1], r7[2]};
    return projDecompose(cx, cy, cw, o);
}
constexpr double kSigmaMin = 0.9, kSigmaMax = 1.1;   // a scale ratio outside this is another camera, not a projection mismatch: the plain translation
struct MarkerAffine {
    bool ok = false;              // both decompositions were usable and sigma is within kSigmaMin..kSigmaMax on both axes
    bool haveAxis = false;        // the texture's tangents were known (else the axis is the scene rows' own centre and the jitter is not removed)
    double sigma[2] = {1.0, 1.0}; // scale_M / scale_W
    double axis[2] = {}, offM[2] = {}, offW[2] = {}, jitter[2] = {};
};
// axisOrNull: the texture's optical axis in NDC ({-(R + L) / (R - L), -(U + D) / (U - D)} from the tangents), or null.
inline MarkerAffine markerAffine(const ProjDecomp& M, const ProjDecomp& W, const double* axisOrNull) noexcept {
    MarkerAffine a;
    if (!M.ok || !W.ok) return a;
    for (int k = 0; k < 2; ++k) {
        a.sigma[k] = M.scale[k] / W.scale[k];
        a.offM[k] = M.off[k];
        a.offW[k] = W.off[k];
        a.axis[k] = axisOrNull ? axisOrNull[k] : W.off[k];
        a.jitter[k] = axisOrNull ? W.off[k] - axisOrNull[k] : 0.0;
    }
    a.haveAxis = axisOrNull != nullptr;
    const bool fine = std::isfinite(a.sigma[0]) && std::isfinite(a.sigma[1]) && std::isfinite(a.axis[0]) && std::isfinite(a.axis[1]) &&
                      a.sigma[0] >= kSigmaMin && a.sigma[0] <= kSigmaMax && a.sigma[1] >= kSigmaMin && a.sigma[1] <= kSigmaMax;
    if (!fine) {
        a = MarkerAffine{};
        return a;
    }
    a.ok = true;
    return a;
}
// u of the row patch for the anchor's target T (texture NDC) given the anchor's own projection nM0 through the unpatched rows. A MarkerAffine that
// is not ok (sigma 1, axis and off 0) gives u = T - nM0: the plain translation.
inline void markerAffineU(const MarkerAffine& a, const double T[2], const double nM0[2], double u[2]) noexcept {
    for (int k = 0; k < 2; ++k) u[k] = a.offM[k] + a.sigma[k] * (T[k] - a.axis[k]) - a.sigma[k] * nM0[k];
}
// r4' = sigma_x r4 + u_x r7 and r5' = sigma_y r5 + u_y r7 (NDC' = sigma NDC + u wherever r7 . p > 0).
inline void affineOffsetRows(float r4[4], float r5[4], const float r7[4], const double sigma[2], const double u[2]) noexcept {
    for (int i = 0; i < 4; ++i) {
        r4[i] = static_cast<float>(sigma[0] * r4[i] + u[0] * r7[i]);
        r5[i] = static_cast<float>(sigma[1] * r5[i] + u[1] * r7[i]);
    }
}
// The texture NDC where a UI NDC n is shown, and the unjittered NDC of a point the scene rows put at n (the TAA jitter is a translation of the
// whole projection, so the point's place without it is n less the rows' principal point's offset from the axis).
inline void displayOfUi(const MarkerAffine& a, const double n[2], double out[2]) noexcept {
    for (int k = 0; k < 2; ++k) out[k] = a.axis[k] + (n[k] - a.offM[k]) / a.sigma[k];
}
inline void unjitter(const MarkerAffine& a, const double n[2], double out[2]) noexcept {
    for (int k = 0; k < 2; ++k) out[k] = n[k] - a.jitter[k];
}
// The optical axis of a texture from its tangents {L, R, D, U} (L and D negative): where the forward direction lands in NDC.
inline bool axisFromTangents(const float tan[4], double axis[2]) noexcept {
    const double L = tan[0], R = tan[1], D = tan[2], U = tan[3];
    if (!(R - L > 1e-6) || !(U - D > 1e-6)) return false;
    axis[0] = -(R + L) / (R - L);
    axis[1] = -(U + D) / (U - D);
    return std::isfinite(axis[0]) && std::isfinite(axis[1]);
}

// The icon's offset from its anchor, from the draw's own geometry as the vertex shader forms it (B10B032BDFD46700, disassembled): a vertex position
// is decoded from PACKEDVERTEXDATAA.xyz (two forms, selected by bits 24..30 of z), turned by the instance record's quaternion (words 2 and 3 of the
// record: four unorm16 as u / 32768 - 1; q = (w2 lo, w2 hi, w3 lo, w3 hi); v' = (2 w^2 - 1) v + 2 (q . v) q + 2 w (q x v)), scaled by the record's
// word 1 (a float) and added to the anchor (record +16 less b1 row 275). e = ndc(anchor) - ndc(the centre vertex): the anchor sits e above the disc.
inline bool decodePackedPosition(uint32_t a0, uint32_t a1, uint32_t a2, double out[3]) noexcept {
    const uint32_t mode = (a2 >> 24) & 0x7Fu;
    if (mode == 64u) {
        const double xlo = static_cast<double>(a0 & 0xFFFFu), xhi = static_cast<double>(a0 >> 16), ylo = static_cast<double>(a1 & 0xFFFFu),
                     yhi = static_cast<double>(a1 >> 16);
        const double s = std::exp2(yhi * (1.0 / 4096.0)) - 1.0;
        out[0] = s * (xlo / 32768.0 - 1.0);
        out[1] = s * (xhi / 32768.0 - 1.0);
        out[2] = s * (ylo / 32768.0 - 1.0);
    } else {
        const uint32_t shift = (20u - mode) & 31u;
        const double inv = 1.0 / (static_cast<double>(1u << shift) - 1.0);
        const double sub = static_cast<double>(1u << ((a2 >> 24) & 31u));
        const uint32_t qx = a0 & 0x1FFFFFu, qy = (a0 >> 21) + ((a1 & 0x3FFu) << 11), qz = (a1 >> 10) & 0x1FFFFFu;
        out[0] = static_cast<double>(qx) * inv - sub;
        out[1] = static_cast<double>(qy) * inv - sub;
        out[2] = static_cast<double>(qz) * inv - sub;
    }
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}
inline void rotateByPackedQuat(uint32_t w2, uint32_t w3, const double v[3], double out[3]) noexcept {
    const double q[4] = {static_cast<double>(w2 & 0xFFFFu) / 32768.0 - 1.0, static_cast<double>(w2 >> 16) / 32768.0 - 1.0,
                         static_cast<double>(w3 & 0xFFFFu) / 32768.0 - 1.0, static_cast<double>(w3 >> 16) / 32768.0 - 1.0};
    const double qv = q[0] * v[0] + q[1] * v[1] + q[2] * v[2];
    const double c[3] = {q[1] * v[2] - v[1] * q[2], q[2] * v[0] - v[2] * q[0], q[0] * v[1] - v[0] * q[1]};
    const double k = 2.0 * q[3] * q[3] - 1.0;
    for (int i = 0; i < 3; ++i) out[i] = k * v[i] + 2.0 * qv * q[i] + 2.0 * q[3] * c[i];
}
// e (NDC) through the draw's rows; false when the anchor or the centre is not in front of the marker's projection.
inline bool iconEdrawNdc(const float r4[4], const float r5[4], const float r7[4], const double x0rel[3], double scale, uint32_t w2, uint32_t w3,
                         const double vc[3], double e[2]) noexcept {
    double rv[3];
    rotateByPackedQuat(w2, w3, vc, rv);
    const double zero[3] = {0.0, 0.0, 0.0};
    const double c[3] = {x0rel[0] + scale * rv[0], x0rel[1] + scale * rv[1], x0rel[2] + scale * rv[2]};
    double a[3], b[3];
    markerClip(r4, r5, r7, x0rel, zero, a);
    markerClip(r4, r5, r7, c, zero, b);
    if (!(a[2] > 1e-9) || !(b[2] > 1e-9)) return false;
    e[0] = a[0] / a[2] - b[0] / b[2];
    e[1] = a[1] / a[2] - b[1] / b[2];
    return std::isfinite(e[0]) && std::isfinite(e[1]);
}

// F62c: THE PATCH DECISION (fix.native_on_foot_marker_fix). F53's trust gate (P through the scene rows and through the temporal pass's camera
// agreeing under 1 px) is gone: the two are not meant to agree, and it refused about 45% of frames. The target stays ndc(P through the eye's
// scene rows) + e: the 16-frame image dump of log 034818 shows P through cb1 270..273 following the displayed world within 5..10%, and the
// drawn marker lagging it about 1.75 frames. A draw whose key is matched in this eye and in the other is patched when the eye's scene rows
// are usable (sceneRowsState), P has a find, both w > 0 and |dx|, |dy| < kPatchGateNdc; otherwise the draw is refused for the first reason
// that holds.
// F64: the gate is 0.4 NDC (was 0.1): the F63 flight's worst real lag, about 0.15 NDC at 90 deg/s, was refused, and the fastest frames
// flicked 100 to 305 px. F62c's one-frame hold of the last good delta is gone: a held delta is wrong by the head's acceleration. The eye's
// newest scene rows stand in for this frame's when they are at most kSceneRowsMaxAge frames older than the draw (the delta is computed
// through them, never carried over); older rows, or none, refuse the draw as stale.
constexpr double kPatchGateNdc = 0.4;
enum PatchWhy : int {
    kPatchApplied = 0,
    kPatchUnmatched = 1,   // no key's read-back match is fresh in this eye
    kPatchBothEyes = 2,    // the key is matched in this eye but not in the other
    kPatchStale = 3,       // the eye's newest scene rows are more than kSceneRowsMaxAge frames older than the draw (or there are none)
    kPatchNoP = 4,         // no find of the key's P
    kPatchW = 5,           // w not positive (P behind the world view or the marker behind its own)
    kPatchGate = 6,        // |dx| or |dy| not under kPatchGateNdc
    kPatchNoIcon = 7,      // F66f: no icon offset yet (the geometry not read, the head-still baseline not seeded)
    kPatchWhyCount = 8
};
// F64: the eye's scene rows at a marker draw: this frame's (fresh), the eye's newest at most kSceneRowsMaxAge frames older (newest), or none
// usable. rowsFrame is the probe frame the rows were taken in (~0u: never); a rows frame after the draw's (a counter that went back) is none.
constexpr uint32_t kSceneRowsMaxAge = 1;
enum SceneRows : int { kRowsFresh = 0, kRowsNewest = 1, kRowsNone = 2, kRowsStates = 3 };
inline int sceneRowsState(uint32_t rowsFrame, uint32_t frame) noexcept {
    if (rowsFrame == ~0u) return kRowsNone;
    const int32_t age = static_cast<int32_t>(frame - rowsFrame);
    return age == 0 ? kRowsFresh : age > 0 && static_cast<uint32_t>(age) <= kSceneRowsMaxAge ? kRowsNewest : kRowsNone;
}
struct PatchVerdict {
    int why = kPatchUnmatched;
    bool patch = false, newest = false;   // newest: patched through the eye's newest rows (one frame older), not this frame's
    double dx = 0.0, dy = 0.0;
};
// rows: sceneRowsState at the draw; deltaWhy: ndcLagDelta's verdict at kPatchGateNdc (read only when the rows are usable and P is found);
// dx, dy its delta.
inline PatchVerdict patchDecide(bool matched, bool bothEyes, int rows, bool haveP, int deltaWhy, double dx, double dy) noexcept {
    PatchVerdict v;
    if (!matched) {
        v.why = kPatchUnmatched;
        return v;
    }
    if (!bothEyes) {
        v.why = kPatchBothEyes;
        return v;
    }
    if (rows != kRowsFresh && rows != kRowsNewest) v.why = kPatchStale;
    else if (!haveP) v.why = kPatchNoP;
    else if (deltaWhy == kDeltaWorldBehind || deltaWhy == kDeltaMarkerBehind) v.why = kPatchW;
    else if (deltaWhy != kDeltaOk || !(std::fabs(dx) < kPatchGateNdc) || !(std::fabs(dy) < kPatchGateNdc)) v.why = kPatchGate;
    else {
        v.why = kPatchApplied;
        v.patch = true;
        v.newest = rows == kRowsNewest;
        v.dx = dx;
        v.dy = dy;
    }
    return v;
}
// F64: the success tally per eye: each eye-frame in which a key's match named one of the eye's marker composite draws (the marker visible) is
// counted once, by the head's speed band at its first decision, as none until a patch bound at the issue upgrades it (fresh over newest).
// The applied fraction of a band is applied / frames.
constexpr int kSpeedBands = 4;   // 0..30, 30..90, >= 90 deg/s, the head's speed unknown
inline int speedBand(double dps) noexcept { return !(dps >= 0.0) ? 3 : dps < 30.0 ? 0 : dps < 90.0 ? 1 : 2; }
struct EyeFrameTally {
    uint32_t frame = ~0u;
    int state = kRowsNone, band = 3;
    uint64_t frames[kSpeedBands] = {}, applied[kSpeedBands] = {}, inState[kRowsStates] = {};
    void visible(uint32_t f, double dps) noexcept {
        if (frame == f) return;
        frame = f;
        state = kRowsNone;
        band = speedBand(dps);
        ++frames[band];
        ++inState[kRowsNone];
    }
    void bound(uint32_t f, int rows, double dps) noexcept {
        if (rows != kRowsFresh && rows != kRowsNewest) return;
        visible(f, dps);
        if (rows >= state) return;
        if (state == kRowsNone) ++applied[band];
        --inState[state];
        ++inState[rows];
        state = rows;
    }
    uint64_t total() const noexcept { return frames[0] + frames[1] + frames[2] + frames[3]; }
};

// F63: THE HELD MATCH (fix.native_on_foot_marker_fix). F62's flight (log 054928): the patch was accurate when applied, but a read-back's match
// went stale after 8 frames whenever the record was not copied, so the marker flicked. The match is now an identity held for kMatchHoldFrames:
// the eye's marker composite draw at a place (its order among the eye's B10B draws) reading a record. Evidence about the draw at a place in
// frame f is a read-back of frame f or the CPU identification at the draw (the instance stream's CPU shadow): it read record `rec`;
// `recIsKeyFind` rec carried the key's tag in frame f; `keyHadFind` the key had a find in frame f (any record). Evidence older than the hold's
// newest confirmation says nothing. Reading the key's record confirms (or moves) the hold. At the held place, another record contradicts it
// (dropped): another record while the key's own was tagged elsewhere, or a record other than the held one with no find at all. The held
// record read untagged (no find in frame f) is consistent and leaves the hold as it was. Evidence about another place says nothing.
constexpr uint32_t kMatchHoldFrames = 90;
enum HoldEvent : int { kHoldNone = 0, kHoldConfirm = 1, kHoldContradict = 2 };
struct HeldMatch {
    uint32_t frame = ~0u;   // the newest frame whose evidence confirmed it (~0u: none)
    uint16_t ord = 0;       // the draw's place among the eye's marker composite draws
    uint32_t record = 0;    // the record it read
    uint32_t key = 0;
    bool held(uint32_t now) const noexcept {
        return frame != ~0u && static_cast<int32_t>(now - frame) >= 0 && now - frame <= kMatchHoldFrames;
    }
    void drop() noexcept { frame = ~0u; }
};
inline int holdEvidence(HeldMatch& h, uint32_t f, uint16_t ord, uint32_t rec, uint32_t key, bool recIsKeyFind, bool keyHadFind) noexcept {
    if (h.frame != ~0u && static_cast<int32_t>(f - h.frame) < 0) return kHoldNone;
    if (recIsKeyFind) {
        h.frame = f;
        h.ord = ord;
        h.record = rec;
        h.key = key;
        return kHoldConfirm;
    }
    if (!h.held(f) || ord != h.ord) return kHoldNone;
    if (!keyHadFind && rec == h.record) return kHoldNone;
    h.drop();
    return kHoldContradict;
}

// F63: THE CPU IDENTIFICATION's window. The instance stream (one DYNAMIC vertex buffer of stride 8, about 192k entries, WRITE_DISCARD once a
// frame) is copied at its Unmap, before the real one, only about each eye's newest marker composite entry: `want` entries centred there,
// starting on an even entry (16-byte loads), inside the buffer's `total` entries. Its first entry and count go to *lo, *n.
inline void streamWindow(uint32_t centre, uint32_t total, uint32_t want, uint32_t* lo, uint32_t* n) noexcept {
    want &= ~1u;
    uint32_t a = centre > want / 2 ? centre - want / 2 : 0u;
    a &= ~1u;
    if (total < want) a = 0;
    else if (a > total - want) a = (total - want) & ~1u;
    const uint32_t left = total > a ? total - a : 0u;
    *lo = a;
    *n = (left < want ? left : want) & ~1u;
}

// F62c: THE MARKER DUMP'S CROP (the waypoint series, temporal_pass.cpp). Each crop is kMarkerDumpCrop square (the marker moves about 55 px
// a frame at 4032 in a fast turn), centred on THIS frame's P through the eye's scene rows (its frame equal to the capture's: age 0), never on
// the drawn marker (read back 2..3 frames late, 31..156 ms old in the F60 dump). A frame whose P is off the eye (w <= 0, or the NDC outside
// -1..1) is skipped, as is one with no P this frame.
constexpr uint32_t kMarkerDumpCrop = 1024;
enum CropPick : int { kCropOk = 0, kCropNoP = 1, kCropNotThisFrame = 2, kCropOffScreen = 3 };
inline int markerCropPick(bool haveP, uint32_t pFrame, uint32_t nowFrame, double w, const double ndc[2]) noexcept {
    if (!haveP) return kCropNoP;
    if (pFrame != nowFrame) return kCropNotThisFrame;
    if (!(w > 1e-9)) return kCropOffScreen;
    if (!(std::fabs(ndc[0]) <= 1.0) || !(std::fabs(ndc[1]) <= 1.0)) return kCropOffScreen;
    return kCropOk;
}
// An NDC point (y up) in the eye's region (left, top, right, bottom of the texture), with the region's flips, in texture pixels.
inline void markerCropCentre(const double ndc[2], const uint32_t region[4], bool flipU, bool flipV, double centre[2]) noexcept {
    const double w = static_cast<double>(region[2] - region[0]), h = static_cast<double>(region[3] - region[1]);
    centre[0] = region[0] + (flipU ? (1.0 - ndc[0]) : (ndc[0] + 1.0)) * 0.5 * w;
    centre[1] = region[1] + (flipV ? (1.0 + ndc[1]) : (1.0 - ndc[1])) * 0.5 * h;
}
// A crop of cw x ch about `centre`, kept inside the region that starts at lo and spans span (cw <= span[0], ch <= span[1]): its left and
// top (a NaN centre takes the region's start). The eye run's stageEyeCrop places every centred crop through this.
inline void cropOrigin(const double centre[2], const uint32_t lo[2], const uint32_t span[2], uint32_t cw, uint32_t ch, uint32_t out[2]) noexcept {
    const uint32_t size[2] = {cw, ch};
    for (int k = 0; k < 2; ++k) {
        const double a = static_cast<double>(lo[k]), b = a + static_cast<double>(span[k] - size[k]);
        double v = centre[k] - 0.5 * size[k];
        if (!(v == v)) v = a;
        v = v < a ? a : v > b ? b : v;
        out[k] = static_cast<uint32_t>(v + 0.5);
    }
}

// F53: THE KEY CENSUS. The post stub's keys as seen (full 32-bit values): two keys share a sentinel when their 8-bit tag bytes match, and a
// first post shares one with another key's second post when its byte equals that key's low 7 bits with bit 7 set. True when every pair of
// the n keys gets distinct sentinels (first posts and second posts alike).
inline uint32_t tagByte(uint32_t key, bool second) noexcept { return second ? ((key & 0x7Fu) | 0x80u) : (key & 0xFFu); }
inline bool keysDistinct(const uint32_t* keys, uint32_t n, uint32_t* aOut = nullptr, uint32_t* bOut = nullptr) noexcept {
    // Every (key, post) pair against every later one: a key's first post against its own second post too (a key with bit 7 set).
    for (uint32_t a = 0; a < 2 * n; ++a)
        for (uint32_t b = a + 1; b < 2 * n; ++b)
            if (tagByte(keys[a / 2], (a & 1u) != 0) == tagByte(keys[b / 2], (b & 1u) != 0)) {
                if (aOut) *aOut = keys[a / 2];
                if (bOut) *bOut = keys[b / 2];
                return false;
            }
    return true;
}

}  // namespace drawtag
}  // namespace edvr

// ---- The draw side: a ring of small staging buffers, each filled at a draw by GPU copies of what the draw binds (constant buffer rows, its
// instance stream entries) and read back a few frames later without a wait. Only when <d3d11.h> is in (a pure-math rig does not need it).
#ifdef __d3d11_h__
namespace edvr {
namespace drawtag {

struct CopyRegion {
    ID3D11Buffer* src = nullptr;
    UINT srcOffset = 0;
    UINT bytes = 0;
    UINT dstOffset = 0;
};

template <uint32_t N, uint32_t Bytes>
class StagingRing {
public:
    // Copies the regions into a free slot; the slot index, or -1 (no free slot, a region out of range, or no buffer).
    int stage(ID3D11DeviceContext* ctx, const CopyRegion* regions, uint32_t n, uint32_t frame) noexcept {
        if (!ctx) return -1;
        int pick = -1;
        for (uint32_t k = 0; k < N; ++k) {
            const uint32_t i = (next_ + k) % N;
            if (!slots_[i].pending) {
                pick = static_cast<int>(i);
                break;
            }
        }
        if (pick < 0) return -1;
        Slot& s = slots_[pick];
        if (!s.st) {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return -1;
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = Bytes;
            d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            const HRESULT hr = dev->CreateBuffer(&d, nullptr, &s.st);
            dev->Release();
            if (FAILED(hr) || !s.st) {
                s.st = nullptr;
                return -1;
            }
        }
        for (uint32_t r = 0; r < n; ++r) {
            const CopyRegion& c = regions[r];
            if (!c.src || !c.bytes || c.dstOffset + c.bytes > Bytes) return -1;
            D3D11_BUFFER_DESC sd{};
            c.src->GetDesc(&sd);
            if (c.srcOffset + c.bytes > sd.ByteWidth) return -1;
        }
        for (uint32_t r = 0; r < n; ++r) {
            const CopyRegion& c = regions[r];
            const D3D11_BOX box{c.srcOffset, 0, 0, c.srcOffset + c.bytes, 1, 1};
            ctx->CopySubresourceRegion(s.st, 0, c.dstOffset, 0, 0, c.src, 0, &box);
        }
        s.pending = true;
        s.frame = frame;
        next_ = (static_cast<uint32_t>(pick) + 1) % N;
        return pick;
    }
    // Every pending slot at least minAge frames old whose copy has landed: onData(slot, bytes). A slot older than maxAge is dropped (*lost).
    template <class F>
    void poll(ID3D11DeviceContext* ctx, uint32_t frame, uint32_t minAge, uint32_t maxAge, F onData, uint64_t* lost) noexcept {
        if (!ctx) return;
        for (uint32_t i = 0; i < N; ++i) {
            Slot& s = slots_[i];
            if (!s.pending) continue;
            const uint32_t age = frame - s.frame;
            if (age < minAge) continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            const HRESULT hr = ctx->Map(s.st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (SUCCEEDED(hr) && m.pData) {
                uint8_t data[Bytes];
                std::memcpy(data, m.pData, Bytes);
                ctx->Unmap(s.st, 0);
                s.pending = false;
                onData(static_cast<int>(i), data);
            } else if (age > maxAge) {
                s.pending = false;
                if (lost) ++*lost;
            }
        }
    }
    uint32_t slotFrame(int i) const noexcept { return slots_[i].frame; }
    void release() noexcept {
        for (Slot& s : slots_) {
            if (s.st) s.st->Release();
            s.st = nullptr;
            s.pending = false;
        }
    }

private:
    struct Slot {
        ID3D11Buffer* st = nullptr;
        uint32_t frame = 0;
        bool pending = false;
    };
    Slot slots_[N];
    uint32_t next_ = 0;
};

// F45: one staging buffer per frame, appended to draw by draw (each append one GPU copy, 16-byte aligned), read back whole two or more frames
// later without a wait. The caller keeps its own per-slot records (offsets into the slot). The callback gets the mapped bytes in place: a
// slot can be megabytes, so nothing is copied out.
template <uint32_t N, uint32_t Bytes>
class FrameStaging {
public:
    // The open slot for `frame`; a slot open for another frame is committed first. -1: no free slot, or the buffer could not be made.
    int open(ID3D11DeviceContext* ctx, uint32_t frame) noexcept {
        if (open_ >= 0 && slots_[open_].frame == frame) return open_;
        commit();
        if (!ctx) return -1;
        int pick = -1;
        for (uint32_t k = 0; k < N && pick < 0; ++k) {
            const uint32_t i = (next_ + k) % N;
            if (!slots_[i].pending) pick = static_cast<int>(i);
        }
        if (pick < 0) return -1;
        Slot& s = slots_[pick];
        if (!s.st) {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return -1;
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = Bytes;
            d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            const HRESULT hr = dev->CreateBuffer(&d, nullptr, &s.st);
            dev->Release();
            if (FAILED(hr) || !s.st) {
                s.st = nullptr;
                return -1;
            }
        }
        s.frame = frame;
        s.used = 0;
        open_ = pick;
        next_ = (static_cast<uint32_t>(pick) + 1) % N;
        return pick;
    }
    // Copies [srcOffset, srcOffset + bytes) of src into the open slot: its byte offset there, or -1 (no open slot, full, out of range).
    int64_t append(ID3D11DeviceContext* ctx, ID3D11Buffer* src, UINT srcOffset, UINT bytes) noexcept {
        if (open_ < 0 || !ctx || !src || !bytes) return -1;
        Slot& s = slots_[open_];
        const uint32_t at = (s.used + 15u) & ~15u;
        if (static_cast<uint64_t>(at) + bytes > Bytes) return -1;
        D3D11_BUFFER_DESC sd{};
        src->GetDesc(&sd);
        if (static_cast<uint64_t>(srcOffset) + bytes > sd.ByteWidth) return -1;
        const D3D11_BOX box{srcOffset, 0, 0, srcOffset + bytes, 1, 1};
        ctx->CopySubresourceRegion(s.st, 0, at, 0, 0, src, 0, &box);
        s.used = at + bytes;
        return at;
    }
    // The open slot (if any) waits for its read-back.
    void commit() noexcept {
        if (open_ < 0) return;
        slots_[open_].pending = true;
        open_ = -1;
    }
    int openSlot() const noexcept { return open_; }
    uint32_t slotFrame(int i) const noexcept { return slots_[i].frame; }
    // Every pending slot at least minAge frames old whose copies have landed, oldest frame first: onData(slot, frame, bytes, used). A slot
    // older than maxAge is dropped (*lost).
    template <class F>
    void poll(ID3D11DeviceContext* ctx, uint32_t frame, uint32_t minAge, uint32_t maxAge, F onData, uint64_t* lost) noexcept {
        if (!ctx) return;
        bool done[N] = {};
        for (;;) {
            int pick = -1;
            for (uint32_t i = 0; i < N; ++i) {
                const Slot& s = slots_[i];
                if (done[i] || !s.pending || frame - s.frame < minAge) continue;
                if (pick < 0 || frame - s.frame > frame - slots_[pick].frame) pick = static_cast<int>(i);
            }
            if (pick < 0) return;
            done[pick] = true;
            Slot& s = slots_[pick];
            const uint32_t age = frame - s.frame;
            D3D11_MAPPED_SUBRESOURCE m{};
            const HRESULT hr = ctx->Map(s.st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (SUCCEEDED(hr) && m.pData) {
                s.pending = false;
                onData(pick, s.frame, static_cast<const uint8_t*>(m.pData), s.used);
                ctx->Unmap(s.st, 0);
            } else if (age > maxAge) {
                s.pending = false;
                if (lost) ++*lost;
            }
        }
    }
    void release() noexcept {
        for (Slot& s : slots_) {
            if (s.st) s.st->Release();
            s.st = nullptr;
            s.pending = false;
        }
        open_ = -1;
    }

private:
    struct Slot {
        ID3D11Buffer* st = nullptr;
        uint32_t frame = 0, used = 0;
        bool pending = false;
    };
    Slot slots_[N];
    uint32_t next_ = 0;
    int open_ = -1;
};

}  // namespace drawtag
}  // namespace edvr
#endif
