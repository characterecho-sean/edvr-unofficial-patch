#pragma once
// F44 (TEMPORARY instrument, fix.native_on_foot_marker_probe; docs\design-native-onfoot-vr-2026-10-13.md): tag a value the game builds on its
// own thread, find the GPU pool record it is copied into, and name the draw that reads that record. Written for the waypoint marker; meant to be
// reused for the weapon ammo readout. Measure only: nothing here writes a constant buffer, a pool or a binding.
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

}  // namespace drawtag
}  // namespace edvr
#endif
