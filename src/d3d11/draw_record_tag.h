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
// F47 (kept ready, NOT used by F46): the per-eye NDC offset on a private copy of the marker composite's b0. row4' = row4 + dx row7 and
// row5' = row5 + dy row7 move the marker's NDC by (dx, dy) wherever r7 . p > 0 (the shader divides by |w|: behind the eye the move flips).
inline void ndcOffsetRows(float r4[4], float r5[4], const float r7[4], double dx, double dy) noexcept {
    for (int i = 0; i < 4; ++i) {
        r4[i] = static_cast<float>(r4[i] + dx * r7[i]);
        r5[i] = static_cast<float>(r5[i] + dy * r7[i]);
    }
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
