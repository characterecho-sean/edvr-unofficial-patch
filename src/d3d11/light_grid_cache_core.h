#pragma once
// The light grid cache, pure half (docs/light-grid-cache-2026-10-13.md). No Windows, no engine: tools\light_grid_cache_test drives it on a synthetic context laid out at
// the real offsets, with a fake builder standing in for the game's. light_grid_cache_hook.cpp is the glue that finds the function, patches its entry, reads the call's inputs
// under SEH and hands them here.
//
// THE FUNCTION. EliteDangerous64.exe (build 332841, PE stamp 1788384820, size 104894464) RVA 0x58B150, the owner of the .pdata chunks 0x58B150, 0x58B474, 0x58B530, 0x58BA2F
// and 0x58BDD7; one direct caller, 0x590368. It builds the fragment-culling cluster grid: rcx the cluster context (this, at owner+0x20), rdx (a2) the renderer holder
// (*a2 the renderer), r8 (a3) a sub-block of the caller's, r9 (a4) the view block, then width and height as 32-bit stack arguments.
//
// WHAT THE GAME'S OWN MEMO PROVES, AND WHAT IT LEAVES OUT (read from the disassembly; RVAs are the exe's):
//   The key is this+0x00..0x77: four 16-byte rows, near and far, then six 64-bit values: the grid x, the grid y, the slice count, the width, the height, the tile.
//   58B3D4..58B465 compare it (cmpeqps on the rows, ucomiss on the floats, cmp on the qwords); 58B467 jumps to the epilogue 58BDD7 when the global byte at RVA 0x5F27612 is
//   clear. Otherwise 58BD6B..58BDD1 store the new key. The rows, near and far come from one of two places (58B273): the view block's own (a4+0x1B0.., +0x234, +0x238) when the
//   byte at 0x5E70F7E is 0, or, when it is set (it is 1 in the exe's data and nothing in .text writes it: the usual case), from a local copy of the block with the near and far
//   replaced by the floats at 0x5E70F94 and 0x5E70F98 and the rows recomputed by 0x4F2FF0 (see kOverrideNearRva). Either way the rows are a function of the view block
//   and those globals, and the glue reads them the way the build does.
//   The grid x and y are CEIL((float)width / (float)tile) and the same for the height (58B353..58B3E0, ceilf at 0x48AAAE8; gameCeilf transcribes it), the tile is the global u32 at 0x5E70F80 (120 in the
//   exe's data; read at 58B33E and again at 58B4D0), the slice count the global u32 at 0x5E70F90 (32; 58B3A5): a 17 x 17 x 32 grid is 9248 clusters and 9280 elements of 32
//   bytes, the 296,960 bytes the flight measured. The build's loops read ONE thing the key does not hold: the view block's own near plane, at 58B5E4 (a4+0x234, used for the
//   first depth slice; on the plain path it IS the key's near, on the override path it is not), so the cache's key is the game's key plus that value. The two calls inside the
//   loops, 0x5E9ED0 and 0x55F620, make no call and read no writable global (capstone scan of both bodies), and the writable globals the whole function reads are exactly the
//   list above plus the stack cookie. Nothing it reads varies otherwise; a2 and a3 reach only the GPU object creation.
//   WHAT THE MEMO LEAVES OUT: the build's outputs are not only the two 32-byte-element tables the probe saw. It also fills a THIRD vector (this+0x110, 48-byte elements, one per
//   cluster, 58B8F5..58B94F), and it rebuilds four refcounted GPU objects (this+0x130 and +0x138 the buffers made from the two tables, +0x140 and +0x148 the views of them;
//   58BA2F..58BD57 release the old four and 58BB70, 58BC0A, 58BC8E, 58BD11 create the new ones from the tables). So "the output" is a STATE of the context, not two tables.
//   Its first call also copies the view's first 64 bytes to this+0xD0 and clears this+0xC0 (58B198..58B1E0): that is not part of a state, and the cache never serves a call
//   that finds the byte set.
//
// THE DESIGN: STATES ARE MOVED, NEVER COPIED. The state of a build is three regions of the context: the key (0x00..0x77), the vectors at 0x80 and 0xA0 (0x40 bytes) and the
// vector at 0x110 with the four GPU pointers (0x40 bytes): kStateBytes in all. Each vector is a 32-byte header {capacity, floor, size, data} (the resize routines 0x5954A0 and
// 0x5955E0 read and write exactly those). A slot holds a whole state. A HIT swaps the context's state with the slot's: the context now owns the slot's tables and GPU objects,
// the slot owns what the context had (a state that is valid for ITS key, because it was). A MISS first swaps the context with the least recently used slot, so the context's old
// state is kept, and then lets the game rebuild in place over the state it was handed (exactly what the game does to its own, only into recycled memory). No allocation, no
// refcount, no release, no copy of a table: ownership of every buffer is unique at every instant, so nothing the game frees is ever ours and nothing ours is ever freed by the
// game. An empty slot starts from the TEMPLATE, an empty state made at first sight (key zero, empty
// vectors, no GPU objects; makeEmptyTemplate says why the game's code accepts it). It is made rather than captured because the contexts of the first flight were already
// built when the hook first saw them.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::lgc {

// ---- identity: the pair every build-keyed hook here checks, and the sixteen bytes of the entry ---------------------------------------------------------------------
constexpr uint32_t kExpectedTimestamp = 1788384820u;
constexpr uint32_t kExpectedImageSize = 104894464u;
constexpr uintptr_t kBuilderRva = 0x58B150u;
constexpr size_t kPrologueBytes = 16;
// mov rax,rsp; push rbp; push rbx; push rsi; push rdi; push r12; push r14; push r15; lea rbp,[rax-0x4D8]. CodeHook moves the first five bytes (no relative operand).
constexpr uint8_t kPrologue[kPrologueBytes] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8};

enum class Install : uint8_t { kInstall = 0, kUnreadable, kWrongIdentity, kWrongPrologue };
// Whether to patch: the identity first (a stamp or size that is not the build's is final), then the sixteen bytes. A refusal is final for the session.
inline Install installDecision(bool identityRead, uint32_t stamp, uint32_t size, const uint8_t* got, size_t n) noexcept {
    if (!identityRead || got == nullptr || n < kPrologueBytes) return Install::kUnreadable;
    if (stamp != kExpectedTimestamp || size != kExpectedImageSize) return Install::kWrongIdentity;
    if (std::memcmp(got, kPrologue, kPrologueBytes) != 0) return Install::kWrongPrologue;
    return Install::kInstall;
}

// ---- globals the build reads (RVAs from the module base) and the view block's offsets --------------------------------------------------------------------------------
constexpr uintptr_t kOverridePathRva = 0x5E70F7Eu;     // byte: set sends the build down the override path (rows from a copy of the view block)
constexpr uintptr_t kTileRva = 0x5E70F80u;          // u32 tile size in pixels
constexpr uintptr_t kSlicesRva = 0x5E70F90u;        // u32 depth slice count
constexpr uintptr_t kForceRebuildRva = 0x5F27612u;  // byte: set disables the game's own memo
constexpr size_t kViewRows = 0x1B0, kViewFlags = 0x230, kViewNear = 0x234, kViewFar = 0x238;
constexpr uint8_t kViewDirtyBit = 4;
// The function the builder calls on the view block when the dirty bit is set (58B2F4): it recomputes the four rows at +0x1B0..0x1EF from the block's projection, whichever of
// its modes the block is in, and clears the bit (4F3745). Its only stores are those rows and that bit (a capstone listing of all of its stores), it calls only CRT math, and it
// reads no writable global. The glue runs it ahead of reading the rows when the bit is set: the block the builder then sees is the block it would have made itself.
constexpr uintptr_t kRefreshRva = 0x4F2FF0u;
constexpr uint8_t kRefreshPrologue[kPrologueBytes] = {0x48, 0x8B, 0xC4, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x0F, 0x29, 0x70, 0xE8, 0x48};
inline bool refreshPinMatches(const uint8_t* got, size_t n) noexcept { return got != nullptr && n >= kPrologueBytes && std::memcmp(got, kRefreshPrologue, kPrologueBytes) == 0; }
// THE OVERRIDE PATH. The global byte at 0x5E70F7E is 1 in the exe's own data (and nothing in the code writes it: a capstone sweep of .text finds only reads), so the build's
// usual path is this one (58B275..58B2E8): it copies the view block into a local with the game's copy constructor (0x57A460, 58B27F), ORs 0xD into the copy's flags (+0x230), puts
// the two float globals 0x5E70F94 (1.0 in the file) and 0x5E70F98 (10000.0) in the copy's near and far (+0x234, +0x238), runs 0x4F2FF0 on the copy (58B2B2) and keys on the copy's
// four rows and its near and far. The real block is only read, never written. The glue does the same on a local of its own to learn the rows before it decides; the object is 0x2F0 bytes
// (the copy constructor's last store is at +0x2EF).
constexpr uintptr_t kOverrideNearRva = 0x5E70F94u, kOverrideFarRva = 0x5E70F98u;
constexpr uintptr_t kCopyRva = 0x57A460u;
constexpr uint8_t kCopyPrologue[kPrologueBytes] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0x10, 0x02, 0x48, 0x8B, 0xF9};
inline bool copyPinMatches(const uint8_t* got, size_t n) noexcept { return got != nullptr && n >= kPrologueBytes && std::memcmp(got, kCopyPrologue, kPrologueBytes) == 0; }
constexpr size_t kViewObjectBytes = 0x2F0;
constexpr uint32_t kOverrideFlagBits = 0xD;
constexpr size_t kViewRowsOut = 0x1B0;

// ---- the context and its state -----------------------------------------------------------------------------------------------------------------------------------------
constexpr size_t kKeyBytes = 0x78;
constexpr size_t kRowsBytes = 0x40;
constexpr size_t kCtxFirstFlag = 0xC0;   // byte: set until the first call copies the view's first 64 bytes to +0xD0 and clears it
constexpr size_t kStateBytes = 0x78 + 0x40 + 0x40;
struct Region { size_t ctxOff, stateOff, len; };
constexpr Region kRegions[3] = {{0x00, 0x00, 0x78}, {0x80, 0x78, 0x40}, {0x110, 0xB8, 0x40}};
// Inside a state: vector i's header (32 bytes) and its element size; the GPU pointers follow the third vector.
constexpr size_t kVecOff[3] = {0x78, 0x98, 0xB8};
constexpr size_t kVecElem[3] = {32, 32, 48};
constexpr size_t kVecSizeAt = 0x10, kVecDataAt = 0x18;
constexpr size_t kGpuOff = 0xD8;   // four pointers: the two buffers, then their two views
constexpr uint64_t kMaxTableBytes = 256ull * 1024ull * 1024ull;

// How memory is touched. Production's is SEH-guarded (the glue); the rig's is plain, or a faulting one. A false result means the access faulted and nothing is to be trusted.
struct PlainMem {
    static bool read(void* dst, const void* src, size_t n) noexcept { std::memcpy(dst, src, n); return true; }
    static bool write(void* dst, const void* src, size_t n) noexcept { std::memcpy(dst, src, n); return true; }
};

template <class Mem>
inline bool gather(const uint8_t* ctx, uint8_t* out) noexcept {
    for (const Region& r : kRegions) {
        if (!Mem::read(out + r.stateOff, ctx + r.ctxOff, r.len)) return false;
    }
    return true;
}
template <class Mem>
inline bool scatter(uint8_t* ctx, const uint8_t* in) noexcept {
    for (const Region& r : kRegions) {
        if (!Mem::write(ctx + r.ctxOff, in + r.stateOff, r.len)) return false;
    }
    return true;
}

inline uint64_t load64(const uint8_t* p) noexcept { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint64_t stateVecSize(const uint8_t* state, int i) noexcept { return load64(state + kVecOff[i] + kVecSizeAt); }
inline uint64_t stateVecData(const uint8_t* state, int i) noexcept { return load64(state + kVecOff[i] + kVecDataAt); }

// A state the game constructed and has not touched: the key zero, every vector empty, no GPU object.
inline bool statePristine(const uint8_t* state) noexcept {
    for (size_t i = 0; i < kKeyBytes; ++i) {
        if (state[i] != 0) return false;
    }
    for (int v = 0; v < 3; ++v) {
        if (stateVecSize(state, v) != 0 || stateVecData(state, v) != 0) return false;
    }
    for (int g = 0; g < 4; ++g) {
        if (load64(state + kGpuOff + 8 * g) != 0) return false;
    }
    return true;
}

// THE EMPTY STATE a fresh slot starts from: key zero, three vectors with capacity 0, size 0 and no data, no GPU object. It cannot be captured from a context (the flight met
// contexts already built, and a built state owns buffers that must never be in two places), so it is made. It is valid because the game's own vector code is written for it:
// every grow path (0x598740, 0x5988F0, 0x598F10 under 0x5954A0 and 0x5955E0) reallocates through 0x55B720 / 0x53A590, and 0x55B720 takes a null old block (58B735: test rcx, je to
// a fresh allocation), 0x55B810 (the release) returns at once for a null block (55B813), the shrink path (0x598CC0) is entered only when the size falls below half the capacity, and
// the build tests each GPU pointer for null before it releases it (58BA42, 58BA7F, 58BABA, 58BAF6). The one word the vectors keep besides capacity, size and data is the second
// header word (+8, read only by the shrink path); it is copied from the context's own vector so a nonzero floor the game set stays.
inline void makeEmptyTemplate(const uint8_t* ctxState, uint8_t* out) noexcept {
    std::memset(out, 0, kStateBytes);
    for (int v = 0; v < 3; ++v) std::memcpy(out + kVecOff[v] + 8, ctxState + kVecOff[v] + 8, 8);
}

// One line about a state as first seen: for the log, so a context that is not what was expected is described rather than guessed at.
inline void describeState(const uint8_t* state, uint8_t flag, char* out, size_t cap) noexcept {
    bool keyZero = true;
    for (size_t i = 0; i < kKeyBytes; ++i) keyZero = keyZero && state[i] == 0;
    int gpu = 0;
    for (int g = 0; g < 4; ++g) gpu += load64(state + kGpuOff + 8 * g) != 0 ? 1 : 0;
    std::snprintf(out, cap, "first-call flag %u, key %s, vector sizes %llu/%llu/%llu, vector data %s/%s/%s, %d GPU objects", static_cast<unsigned>(flag), keyZero ? "zero" : "set",
                  static_cast<unsigned long long>(stateVecSize(state, 0)), static_cast<unsigned long long>(stateVecSize(state, 1)),
                  static_cast<unsigned long long>(stateVecSize(state, 2)), stateVecData(state, 0) ? "set" : "null", stateVecData(state, 1) ? "set" : "null",
                  stateVecData(state, 2) ? "set" : "null", gpu);
}

// EDVR's own jitter at the time of a call (part 2's probe; published by the temporal pass).
struct JitterSnap {
    uint64_t frame = 0;       // EDVR's temporal frame counter when the shift was published
    float jx = 0, jy = 0;     // the sub-pixel offset in render pixels
    float shift[2][2] = {};   // each eye's tangent shift {dx, dy}
    bool known = false;
};

// ---- the key, built exactly as the game builds its own (58B33E..58B3E0) ------------------------------------------------------------------------------------------------
struct CallInputs {
    uint8_t rows[kRowsBytes] = {};   // the four rows the build keys on (see the glue: a4+0x1B0..0x1EF, or the override path's)
    uint32_t nearBits = 0, farBits = 0;   // the key's near and far (the view's own, or the override cvars')
    uint32_t extraNearBits = 0;      // a4+0x234: the build reads it again for the first depth slice, whichever near the key holds
    bool rowsOk = true;              // false: the glue could not derive the rows (a pinned function missing or faulting)
    uint8_t overridePath = 0;        // global 0x5E70F7E: the build takes its rows from a copy with the override near and far (stats only)
    uint8_t viewFlags = 0;           // a4+0x230
    uint8_t forceRebuild = 0;        // global 0x5F27612
    uint32_t tile = 0, slices = 0;   // globals 0x5E70F80 and 0x5E70F90
    uint32_t width = 0, height = 0;  // the stack arguments
    uint64_t renderer = 0;           // *a2
    JitterSnap jitter;               // EDVR's current jitter (the probe only)
    // Derives the same inputs again (the glue's reading of the view block, as it is now). The self-check calls it when the game's stored key differs from the derived one, to tell a
    // view block that changed under the call (a race with whoever writes it) from a derivation that is wrong.
    bool (*rederive)(const void* user, CallInputs* out) = nullptr;
    const void* rederiveUser = nullptr;
};

inline void put64(uint8_t* p, uint64_t v) noexcept { std::memcpy(p, &v, 8); }

// The game's CEILING, a transcription of the routine at 0x48AAAE8 that 58B378 and 58B3A0 call on the quotient (float in xmm0, float out). It is ceilf, not floorf (flight 3,
// 2026-10-14: 2016 / 120 is 16.8 and the game's grid is 17). Read instruction by instruction:
//   48AAAF1..48AAAFD  edx = the bits; eax = the bits with the sign cleared; r8 = whether the sign was set.
//   48AAB01           |x| >= 2^23 (0x4B800000): already integral (48AAB08: NaN and infinity leave to 0x48C5AB0 or return unchanged) -> return x.
//   48AAB16           |x| < 1: zero returns itself (48AAB1F); a negative gives -0.0 (48AAB26); a positive gives +1.0 (48AAB36, the float at 0x4DDE614).
//   48AAB40..48AAB53  1 <= |x| < 2^23: e = bits >> 23 (the sign bit is part of it), mask = 0xFFFFFFFF << (150 - e), truncated = bits & mask.
//   48AAB5D..48AAB66  a negative returns the truncation; a positive returns it too when nothing was cut (edx == eax), else the truncation plus 1.0 (48AAB66, 0x4DDE614).
inline float gameCeilf(float x) noexcept {
    uint32_t bits;
    std::memcpy(&bits, &x, 4);
    const uint32_t mag = bits & 0x7FFFFFFFu;
    const bool negative = mag != bits;
    if (mag >= 0x4B800000u) return x;
    if (mag < 0x3F800000u) {
        if (mag == 0) return x;
        if (negative) {
            const uint32_t negZero = 0x80000000u;
            float z;
            std::memcpy(&z, &negZero, 4);
            return z;
        }
        return 1.0f;
    }
    const uint32_t e = bits >> 23;
    const uint32_t mask = 0xFFFFFFFFu << ((150u - e) & 31u);
    const uint32_t cut = bits & mask;
    float r;
    std::memcpy(&r, &cut, 4);
    if (negative || bits == cut) return r;
    return r + 1.0f;
}

// ceil((float)n / (float)tile) as the game computes it: cvtsi2ss from a zero-extended 64-bit value, divss, gameCeilf, cvttss2si to 64 bits, then the low 32 bits.
inline bool gridCells(uint32_t n, uint32_t tile, uint64_t* out) noexcept {
    if (n == 0 || tile == 0) return false;
    const float q = static_cast<float>(static_cast<int64_t>(n)) / static_cast<float>(static_cast<int64_t>(tile));
    if (!std::isfinite(q) || q < 0.0f || q >= 4294967296.0f) return false;
    *out = static_cast<uint32_t>(static_cast<int64_t>(gameCeilf(q)));
    return true;
}

// False when the inputs are ones the game's arithmetic would turn into zero, NaN or a divide by zero: the game gets those calls whole.
inline bool buildKey(const CallInputs& in, uint8_t out[kKeyBytes]) noexcept {
    uint64_t gx = 0, gy = 0;
    if (!gridCells(in.width, in.tile, &gx) || !gridCells(in.height, in.tile, &gy)) return false;
    std::memcpy(out, in.rows, kRowsBytes);
    std::memcpy(out + 0x40, &in.nearBits, 4);
    std::memcpy(out + 0x44, &in.farBits, 4);
    put64(out + 0x48, gx);
    put64(out + 0x50, gy);
    put64(out + 0x58, in.slices);
    put64(out + 0x60, in.width);
    put64(out + 0x68, in.height);
    put64(out + 0x70, in.tile);
    return true;
}

// What differs between the key built from the inputs and the key the game stored, in words and numbers, for the line that turns the cache off: the field (row.column of the sixteen
// row floats, near, far, or one of the six quadwords), what was derived and what was stored. At most eight fields are written; the rest are counted. One line, never longer than cap.
inline size_t describeKeyDiff(const uint8_t* derived, const uint8_t* stored, char* out, size_t cap) noexcept {
    static const char* const qnames[6] = {"grid x", "grid y", "slices", "width", "height", "tile"};
    size_t used = 0;
    int total = 0, written = 0;
    auto add = [&](const char* fmt, auto... args) {
        if (used + 1 >= cap) return;
        const int n = std::snprintf(out + used, cap - used, fmt, args...);
        if (n > 0) used += static_cast<size_t>(n) < cap - used ? static_cast<size_t>(n) : cap - used - 1;
    };
    out[0] = '\0';
    auto floatField = [&](const char* name, int index) {
        const uint8_t* a = derived + 4 * index;
        const uint8_t* b = stored + 4 * index;
        if (std::memcmp(a, b, 4) == 0) return;
        ++total;
        if (written >= 8) return;
        ++written;
        uint32_t ba, bb;
        std::memcpy(&ba, a, 4);
        std::memcpy(&bb, b, 4);
        float fa, fb;
        std::memcpy(&fa, a, 4);
        std::memcpy(&fb, b, 4);
        add("%s%s derived %.9g (0x%08x) stored %.9g (0x%08x)", written > 1 ? "; " : "", name, static_cast<double>(fa), ba, static_cast<double>(fb), bb);
    };
    for (int e = 0; e < 16; ++e) {
        char name[16];
        std::snprintf(name, sizeof(name), "row %d.%d", e / 4, e % 4);
        floatField(name, e);
    }
    floatField("near", 16);
    floatField("far", 17);
    for (int q = 0; q < 6; ++q) {
        const uint8_t* a = derived + 0x48 + 8 * q;
        const uint8_t* b = stored + 0x48 + 8 * q;
        if (std::memcmp(a, b, 8) == 0) continue;
        ++total;
        if (written >= 8) continue;
        ++written;
        add("%s%s derived %llu stored %llu", written > 1 ? "; " : "", qnames[q], static_cast<unsigned long long>(load64(a)), static_cast<unsigned long long>(load64(b)));
    }
    if (total > written) add("; and %d more", total - written);
    return used;
}

inline uint64_t hashKey(const uint8_t* key) noexcept {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < kKeyBytes; i += 8) {
        h = (h ^ load64(key + i)) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

// ---- comparing two states' tables (verification) -----------------------------------------------------------------------------------------------------------------------
enum class Same : uint8_t { kEqual = 0, kPaddingOnly, kDiffers, kUnreadable };
struct Compare {
    Same verdict = Same::kEqual;
    int table = -1;          // the first table that differs
    uint64_t offset = 0;     // the first differing byte within it
    uint64_t sizeA = 0, sizeB = 0;
    uint64_t paddingBytes = 0;
};

// The 32-byte elements keep a float3 in each half and leave the fourth float of each half to the vector's default fill; those lanes are not written by the build.
inline bool paddingLane(int table, uint64_t byteInElement) noexcept {
    if (table == 2) return false;
    return (byteInElement >= 12 && byteInElement < 16) || (byteInElement >= 28 && byteInElement < 32);
}

// The key, the three vectors' sizes and every byte of the three tables. A difference only in a padding lane is its own verdict.
template <class Mem>
inline Compare compareStates(const uint8_t* a, const uint8_t* b) noexcept {
    Compare c;
    if (std::memcmp(a, b, kKeyBytes) != 0) {
        c.verdict = Same::kDiffers;
        c.table = -2;   // the key itself
        for (size_t i = 0; i < kKeyBytes; ++i) {
            if (a[i] != b[i]) { c.offset = i; break; }
        }
        return c;
    }
    for (int t = 0; t < 3; ++t) {
        const uint64_t na = stateVecSize(a, t), nb = stateVecSize(b, t);
        const uint64_t da = stateVecData(a, t), db = stateVecData(b, t);
        c.sizeA = na;
        c.sizeB = nb;
        if (na != nb) { c.verdict = Same::kDiffers; c.table = t; return c; }
        if (na == 0) continue;
        if (da == 0 || db == 0 || na > kMaxTableBytes / kVecElem[t]) { c.verdict = Same::kUnreadable; c.table = t; return c; }
        const uint64_t bytes = na * kVecElem[t];
        constexpr size_t kChunk = 3072;   // a multiple of 32 and of 48
        uint8_t bufA[kChunk], bufB[kChunk];
        for (uint64_t at = 0; at < bytes; at += kChunk) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(kChunk, bytes - at));
            if (!Mem::read(bufA, reinterpret_cast<const void*>(static_cast<uintptr_t>(da + at)), n) ||
                !Mem::read(bufB, reinterpret_cast<const void*>(static_cast<uintptr_t>(db + at)), n)) {
                c.verdict = Same::kUnreadable;
                c.table = t;
                return c;
            }
            if (std::memcmp(bufA, bufB, n) == 0) continue;
            for (size_t i = 0; i < n; ++i) {
                if (bufA[i] == bufB[i]) continue;
                const uint64_t where = at + i;
                if (paddingLane(t, where % kVecElem[t])) {
                    ++c.paddingBytes;
                    if (c.verdict == Same::kEqual) { c.verdict = Same::kPaddingOnly; c.table = t; c.offset = where; }
                } else {
                    c.verdict = Same::kDiffers;
                    c.table = t;
                    c.offset = where;
                    return c;
                }
            }
        }
    }
    return c;
}

// ---- the jitter probe's reading of two keys (part 2) ------------------------------------------------------------------------------------------------------------------
inline float floatAt(const uint8_t* p, int i) noexcept { float f; std::memcpy(&f, p + 4 * i, 4); return f; }

// ---- stats and the 30 s lines --------------------------------------------------------------------------------------------------------------------------------------------
enum Reason : int { kFirst = 0, kDirtyView, kNoRows, kForceRebuild, kUnbuildable, kNoTemplate, kRendererChange, kOtherThread, kContextsFull, kUnreadable, kSwapFault, kReasons };
inline const char* reasonName(int r) noexcept {
    static const char* const names[kReasons] = {"first call", "dirty view", "rows not derivable", "forced rebuild", "unbuildable inputs", "no template", "renderer change",
                                                "other thread", "contexts full", "unreadable", "swap fault"};
    return r >= 0 && r < kReasons ? names[r] : "?";
}

struct Sample {
    float delta[16] = {};
    bool rows13Equal = true;
    JitterSnap jitter;
};

constexpr int kSamples = 4;
constexpr double kWindowMs = 30000.0;

struct Stats {
    uint64_t calls = 0, gameMemo = 0, hits = 0, misses = 0, forwarded = 0, refreshes = 0, derived = 0;
    uint64_t pathOverride = 0, pathPlain = 0;   // calls by the build's two ways of getting its rows
    uint64_t byReason[kReasons] = {};
    double missMs = 0.0, hitMs = 0.0, missOverheadMs = 0.0, refreshMs = 0.0, deriveMs = 0.0;
    uint64_t verifyChecks = 0, verifyPadding = 0, verifyMismatches = 0;
    uint64_t selfOk = 0, selfFail = 0, selfRace = 0, capacityMisses = 0;
    uint64_t statesKept = 0;   // parked states a reset (a new context life, a new renderer) kept as stale slots for the game's rebuild to reuse, instead of dropping them
    // the jitter probe
    uint64_t jNeighbour = 0, jOnly02 = 0, jOther = 0;
    uint64_t jDiffers[16] = {};
    double jMax[16] = {};
    Sample samples[kSamples];
    int nSamples = 0;
};

inline size_t formatMainLine(char* out, size_t cap, const Stats& s, uint64_t seconds, uint64_t frames, double estMissMs, unsigned slotsInUse, unsigned contexts) noexcept {
    const double calls = s.calls ? static_cast<double>(s.calls) : 1.0;
    const double fr = frames ? static_cast<double>(frames) : 1.0;
    const double saved = static_cast<double>(s.hits) * estMissMs;
    auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    const int n = std::snprintf(
        out, cap,
        "light grid cache: last %llu s: %llu calls in %llu frames (%.2f a frame); rows by the override path %llu, by the plain path %llu; the game's own memo %llu, "
        "cache hits %llu (%.1f%% of calls), misses %llu (%.3f ms each); rows derived ahead %llu times (%.4f ms each), dirty view refreshed ahead %llu times (%.4f ms each); "
        "forwarded whole %llu (first call %llu, dirty view %llu, rows not derivable %llu, forced rebuild %llu, unbuildable %llu, no template %llu, renderer change %llu, "
        "other thread %llu, contexts full %llu, unreadable %llu, swap fault %llu); estimated saving %.2f ms (%.3f ms a frame, from %.3f ms a miss); "
        "swap cost %.4f ms a hit, %.4f ms a miss; verify %llu checks, %llu padding-only, %llu mismatches; key self-check %llu ok, %llu failed, %llu with the view block changing under the call; "
        "misses on a key evicted lately %llu; %u slots in use, %u contexts; states kept for reuse by resets %llu.",
        u(seconds), u(s.calls), u(frames), static_cast<double>(s.calls) / fr, u(s.pathOverride), u(s.pathPlain), u(s.gameMemo), u(s.hits),
        100.0 * static_cast<double>(s.hits) / calls, u(s.misses), s.misses ? s.missMs / static_cast<double>(s.misses) : 0.0, u(s.derived),
        s.derived ? s.deriveMs / static_cast<double>(s.derived) : 0.0, u(s.refreshes), s.refreshes ? s.refreshMs / static_cast<double>(s.refreshes) : 0.0, u(s.forwarded),
        u(s.byReason[kFirst]), u(s.byReason[kDirtyView]), u(s.byReason[kNoRows]), u(s.byReason[kForceRebuild]), u(s.byReason[kUnbuildable]), u(s.byReason[kNoTemplate]),
        u(s.byReason[kRendererChange]), u(s.byReason[kOtherThread]), u(s.byReason[kContextsFull]), u(s.byReason[kUnreadable]), u(s.byReason[kSwapFault]), saved, saved / fr,
        estMissMs, s.hits ? s.hitMs / static_cast<double>(s.hits) : 0.0, s.misses ? s.missOverheadMs / static_cast<double>(s.misses) : 0.0, u(s.verifyChecks),
        u(s.verifyPadding), u(s.verifyMismatches), u(s.selfOk), u(s.selfFail), u(s.selfRace), u(s.capacityMisses), slotsInUse, contexts, u(s.statesKept));
    return n < 0 ? 0 : static_cast<size_t>(n);
}

// The probe's line: for each of the sixteen row elements, in how many of the misses that had a neighbour of the same size it differed (bit for bit), and the largest |delta|.
inline size_t formatJitterLine(char* out, size_t cap, const Stats& s, uint64_t seconds) noexcept {
    size_t used = 0;
    int n = std::snprintf(out, cap,
                          "light grid cache: jitter probe last %llu s: %llu misses had a neighbour of the same size and depth range; %llu differ only in rows 0 and 2, %llu also in "
                          "row 1 or 3. Elements that differ (row.column count/largest |delta|):",
                          static_cast<unsigned long long>(seconds), static_cast<unsigned long long>(s.jNeighbour), static_cast<unsigned long long>(s.jOnly02),
                          static_cast<unsigned long long>(s.jOther));
    if (n < 0) return 0;
    used = static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
    bool any = false;
    for (int e = 0; e < 16 && used + 40 < cap; ++e) {
        if (s.jDiffers[e] == 0) continue;
        any = true;
        n = std::snprintf(out + used, cap - used, " %d.%d %llu/%.6g;", e / 4, e % 4, static_cast<unsigned long long>(s.jDiffers[e]), s.jMax[e]);
        if (n < 0) break;
        used += static_cast<size_t>(n) < cap - used ? static_cast<size_t>(n) : cap - used - 1;
    }
    if (!any && used + 8 < cap) {
        n = std::snprintf(out + used, cap - used, " none.");
        if (n > 0) used += static_cast<size_t>(n);
    }
    return used;
}

inline size_t formatSampleLine(char* out, size_t cap, const Sample& m) noexcept {
    const int n = std::snprintf(
        out, cap,
        "light grid cache: jitter sample: delta row 0 [%.6g %.6g %.6g %.6g] row 2 [%.6g %.6g %.6g %.6g]; rows 1 and 3 %s; EDVR jitter %s: frame %llu, pixels (%.4f, %.4f), "
        "tangent shift eye 0 (%.6g, %.6g) eye 1 (%.6g, %.6g).",
        m.delta[0], m.delta[1], m.delta[2], m.delta[3], m.delta[8], m.delta[9], m.delta[10], m.delta[11], m.rows13Equal ? "equal" : "differ too",
        m.jitter.known ? "published" : "not published (no VR temporal pass)", static_cast<unsigned long long>(m.jitter.frame), m.jitter.jx, m.jitter.jy, m.jitter.shift[0][0],
        m.jitter.shift[0][1], m.jitter.shift[1][0], m.jitter.shift[1][1]);
    return n < 0 ? 0 : static_cast<size_t>(n);
}

// ---- the slots ---------------------------------------------------------------------------------------------------------------------------------------------------------------
constexpr int kPoolSlots = 32;       // all contexts together
constexpr int kMaxContexts = 4;
constexpr int kSlotsPerContext = 20; // 8 jitter phases x 2 eyes, a third view, and room
constexpr uint64_t kVerifyEvery = 1024;   // one hit in 1024 is compared with a real build (was 1 in 64 until 2026-10-14: several flights showed 0 mismatches)
constexpr int kEvictedRing = 64;

// The build reads one thing the key does not hold: the view block's own near plane (a4+0x234) for the first depth slice. A state is only matched by a call whose value is the
// one the state was built with; a state whose build is not known (the game rebuilt it behind the cache's back) carries kExtraUnknown, which no call can equal.
constexpr uint64_t kExtraUnknown = 1ull << 32;

struct Slot {
    int owner = -1;           // group index; -1 unassigned
    uint64_t extra = kExtraUnknown;   // the extra near the state was built with
    bool populated = false;   // false: its state is the owner's template, not yet swapped in
    bool dead = false;        // holds a valid state but is not to be matched (a verified duplicate); first to be recycled
    uint64_t used = 0;
    alignas(16) uint8_t state[kStateBytes] = {};
};

struct Group {
    bool inUse = false;
    uintptr_t ctx = 0;
    uint64_t renderer = 0;
    bool haveTemplate = false;
    uint64_t clock = 0;
    uint64_t ctxExtra = kExtraUnknown;   // the extra near of the build behind the context's current state
    uint8_t templ[kStateBytes] = {};
    char firstSeen[200] = {};   // the context's state as first seen, for the log
};

class Cache {
public:
    Stats stats;
    bool disabled = false;
    char disabledWhy[1024] = {};
    uint64_t hitsSeen = 0;
    unsigned noTemplateNoted = 0;

    int groupFor(uintptr_t ctx, bool* created = nullptr) noexcept {
        if (created) *created = false;
        for (int g = 0; g < kMaxContexts; ++g) {
            if (groups_[g].inUse && groups_[g].ctx == ctx) return g;
        }
        for (int g = 0; g < kMaxContexts; ++g) {
            if (!groups_[g].inUse) {
                groups_[g] = Group{};
                groups_[g].inUse = true;
                groups_[g].ctx = ctx;
                if (created) *created = true;
                return g;
            }
        }
        return -1;
    }
    Group& group(int g) noexcept { return groups_[g]; }
    Slot& slot(int i) noexcept { return slots_[i]; }
    int contexts() const noexcept {
        int n = 0;
        for (const Group& g : groups_) n += g.inUse ? 1 : 0;
        return n;
    }
    int slotsInUse() const noexcept {
        int n = 0;
        for (const Slot& s : slots_) n += (s.owner >= 0 && s.populated) ? 1 : 0;
        return n;
    }

    // A new context life or a new renderer: no kept state may be served again, and none may be lost. A parked state owns the game's vector blocks and four refcounted GPU objects,
    // and nothing else will ever release them: the game frees only the state in its context. So a populated slot is NOT cleared (that would drop the pointers: a leak). It becomes a
    // stale slot: dead (never matched, recycled before anything else) with its key zeroed, so that no call's key can equal it, neither the cache's find nor the game's own memo when a
    // miss later swaps it into the context. The next miss that recycles it swaps it into the context and the game's build runs over it, which resizes its vectors in place and releases
    // and recreates its four GPU objects exactly as it does for its own state: the game's own release path frees it. (An unpopulated slot holds the empty template and owns nothing.)
    // Returns how many states were kept.
    int resetGroup(int g) noexcept {
        int kept = 0;
        for (Slot& s : slots_) {
            if (s.owner != g) continue;
            if (!s.populated) {
                s = Slot{};
                continue;
            }
            std::memset(s.state, 0, kKeyBytes);
            s.extra = kExtraUnknown;
            s.dead = true;
            ++kept;
        }
        stats.statesKept += static_cast<uint64_t>(kept);
        return kept;
    }
    // The slots a call could be served from: populated, and not stale or duplicate.
    int slotsMatchable() const noexcept {
        int n = 0;
        for (const Slot& s : slots_) n += (s.owner >= 0 && s.populated && !s.dead) ? 1 : 0;
        return n;
    }

    // The populated, live slot of the group whose key equals `key` bit for bit, other than `skip`.
    int find(int g, const uint8_t* key, uint64_t extra, int skip = -1) const noexcept {
        for (int i = 0; i < kPoolSlots; ++i) {
            const Slot& s = slots_[i];
            if (i == skip || s.owner != g || !s.populated || s.dead || s.extra != extra) continue;
            if (std::memcmp(s.state, key, kKeyBytes) == 0) return i;
        }
        return -1;
    }

    // The slot a miss recycles: a dead one, else a fresh one from the pool while the group is under its cap, else the least recently used. Never `avoid`, and never one that
    // already holds `avoidKey` (a verification must see the game rebuild, not meet a copy of the key it is checking).
    int victim(int g, int avoid, const uint8_t* avoidKey, uint64_t avoidExtra = 0) noexcept {
        int owned = 0, lru = -1, dead = -1, spare = -1;
        for (int i = 0; i < kPoolSlots; ++i) {
            Slot& s = slots_[i];
            if (s.owner == g) {
                ++owned;
                if (i == avoid) continue;
                if (avoidKey && s.populated && s.extra == avoidExtra && std::memcmp(s.state, avoidKey, kKeyBytes) == 0) continue;
                if (s.dead && dead < 0) dead = i;
                if (lru < 0 || s.used < slots_[lru].used) lru = i;
            } else if (s.owner < 0 && spare < 0) {
                spare = i;
            }
        }
        if (dead >= 0) return dead;
        if (owned < kSlotsPerContext && spare >= 0) {
            slots_[spare] = Slot{};
            slots_[spare].owner = g;
            return spare;
        }
        return lru;
    }

    // Remember that a populated key was recycled (its hash), and ask whether a key was.
    void noteEvicted(const uint8_t* key) noexcept {
        evicted_[evictedAt_] = hashKey(key);
        evictedAt_ = (evictedAt_ + 1) % kEvictedRing;
    }
    bool wasEvicted(const uint8_t* key) const noexcept {
        const uint64_t h = hashKey(key);
        for (uint64_t e : evicted_) {
            if (e == h) return true;
        }
        return false;
    }

private:
    Group groups_[kMaxContexts];
    Slot slots_[kPoolSlots];
    uint64_t evicted_[kEvictedRing] = {};
    int evictedAt_ = 0;
};

// ---- the thread rule: the builder's caller is the render thread; the first thread to call owns the cache and any other thread's call goes to the game whole -------------------
inline bool threadMayServe(std::atomic<uint32_t>& owner, uint32_t current) noexcept {
    uint32_t expected = 0;
    owner.compare_exchange_strong(expected, current, std::memory_order_acq_rel);
    return owner.load(std::memory_order_acquire) == current;
}

// ---- the jitter probe: the nearest live key of the same size (part 2) -----------------------------------------------------------------------------------------------------
inline void probeNeighbour(Cache& c, int g, const uint8_t* key, const JitterSnap& jit) noexcept {
    int best = -1;
    double bestScore = 0.0;
    for (int i = 0; i < kPoolSlots; ++i) {
        const Slot& s = c.slot(i);
        if (s.owner != g || !s.populated) continue;
        if (std::memcmp(s.state + 0x40, key + 0x40, kKeyBytes - 0x40) != 0) continue;   // not the same size or depth range
        if (std::memcmp(s.state, key, 0x40) == 0) continue;                               // the same rows: not a neighbour, the same key
        double score = 0.0;
        for (int e = 0; e < 16; ++e) {
            const double d = std::fabs(static_cast<double>(floatAt(s.state, e)) - static_cast<double>(floatAt(key, e)));
            if (std::isfinite(d)) score += d;
        }
        if (best < 0 || score < bestScore) { best = i; bestScore = score; }
    }
    if (best < 0) return;
    Stats& st = c.stats;
    ++st.jNeighbour;
    const uint8_t* nb = c.slot(best).state;
    bool rows13 = true;
    Sample m;
    for (int e = 0; e < 16; ++e) {
        const float a = floatAt(nb, e), b = floatAt(key, e);
        uint32_t ba, bb;
        std::memcpy(&ba, &a, 4);
        std::memcpy(&bb, &b, 4);
        const double d = std::fabs(static_cast<double>(a) - static_cast<double>(b));
        m.delta[e] = std::isfinite(d) ? static_cast<float>(d) : 0.0f;
        if (ba != bb) {
            ++st.jDiffers[e];
            if (std::isfinite(d) && d > st.jMax[e]) st.jMax[e] = d;
            const int row = e / 4;
            if (row == 1 || row == 3) rows13 = false;
        }
    }
    if (rows13) ++st.jOnly02; else ++st.jOther;
    m.rows13Equal = rows13;
    m.jitter = jit;
    if (st.nSamples < kSamples) st.samples[st.nSamples++] = m;
}

// ---- one call -----------------------------------------------------------------------------------------------------------------------------------------------------------------
enum class Served : uint8_t { kGame = 0, kHit, kMiss };
struct Result {
    Served served = Served::kGame;
    bool disableNow = false;   // the cache turned itself off during this call (Cache::disabledWhy says why)
};

inline void disable(Cache& c, const char* why) noexcept {
    c.disabled = true;
    std::snprintf(c.disabledWhy, sizeof(c.disabledWhy), "%s", why);
}

// `Fwd` runs the game's builder on the context (void()), `Clock` returns monotonic milliseconds (double()). `ctx` is the cluster context.
template <class Mem, class Fwd, class Clock>
inline Result serve(Cache& c, uint8_t* ctx, const CallInputs& in, Fwd&& fwd, Clock&& now) noexcept {
    Result res;
    Stats& st = c.stats;
    ++st.calls;
    if (in.overridePath != 0) ++st.pathOverride; else ++st.pathPlain;
    Group* gp = nullptr;
    // The game runs the call whole. Whatever it does to the context, the cache no longer knows which extra near the state in it was built with.
    auto forwardWhole = [&](Reason why) {
        ++st.forwarded;
        ++st.byReason[why];
        fwd();
        if (gp) gp->ctxExtra = kExtraUnknown;
    };
    uint8_t ctxState[kStateBytes];
    uint8_t flag = 0;
    if (!gather<Mem>(ctx, ctxState) || !Mem::read(&flag, ctx + kCtxFirstFlag, 1)) {
        forwardWhole(kUnreadable);
        return res;
    }
    bool created = false;
    const int g = c.groupFor(reinterpret_cast<uintptr_t>(ctx), &created);
    if (g < 0) {
        forwardWhole(kContextsFull);
        return res;
    }
    Group& grp = c.group(g);
    gp = &grp;
    if (created) grp.renderer = in.renderer;
    // The template (the empty state a fresh slot starts from) is made, not captured: see makeEmptyTemplate. Made at first sight and at every new life, whatever state the
    // context is in then (the flight of 2026-10-13 met four contexts already built, with the first-call flag clear, and a captured template never existed).
    if (created || flag != 0) {
        grp.haveTemplate = true;
        makeEmptyTemplate(ctxState, grp.templ);
        if (created) describeState(ctxState, flag, grp.firstSeen, sizeof(grp.firstSeen));
    }
    // The first call of a context (the game constructs it with this byte set and the call clears it): a new life. Its kept states, if any, are made stale and kept (resetGroup), never dropped.
    if (flag != 0) {
        c.resetGroup(g);
        grp.renderer = in.renderer;
        forwardWhole(kFirst);
        return res;
    }
    if (!grp.haveTemplate) {
        forwardWhole(kNoTemplate);
        return res;
    }
    if (grp.renderer != in.renderer) {
        c.resetGroup(g);
        grp.renderer = in.renderer;
        forwardWhole(kRendererChange);
        return res;
    }
    if (!in.rowsOk) { forwardWhole(kNoRows); return res; }
    // On the plain path the rows are the view block's own, and a dirty block has not made them yet (the glue refreshes it ahead when it can). The override path copies the
    // block and always recomputes, so the original's dirty bit does not matter there.
    if (in.overridePath == 0 && (in.viewFlags & kViewDirtyBit) != 0) { forwardWhole(kDirtyView); return res; }
    if (in.forceRebuild != 0) { forwardWhole(kForceRebuild); return res; }
    uint8_t key[kKeyBytes];
    if (!buildKey(in, key)) { forwardWhole(kUnbuildable); return res; }
    const uint64_t extra = in.extraNearBits;

    const double t0 = now();
    if (std::memcmp(ctxState, key, kKeyBytes) == 0) {   // the game's own memo hits
        ++st.gameMemo;
        fwd();
        return res;
    }
    int hit = c.find(g, key, extra);
    bool verify = false;
    if (hit >= 0) {
        verify = (c.hitsSeen++ % kVerifyEvery) == 0;
        if (!verify) {
            // THE HIT. The slot's state goes into the context, the context's old one into the slot.
            Slot& s = c.slot(hit);
            if (scatter<Mem>(ctx, s.state)) {
                std::memcpy(s.state, ctxState, kStateBytes);
                const uint64_t was = s.extra;
                s.extra = grp.ctxExtra;
                grp.ctxExtra = was;
                s.used = ++grp.clock;
                ++st.hits;
                st.hitMs += now() - t0;
                res.served = Served::kHit;
                return res;
            }
            // A write that faulted part-way leaves the context half-moved: put back what was read, then give the game the call.
            disable(c, scatter<Mem>(ctx, ctxState) ? "a write into the cluster context faulted during a swap"
                                                    : "a write into the cluster context faulted and could not be undone");
            res.disableNow = true;
            ++st.forwarded;
            ++st.byReason[kSwapFault];
            fwd();
            return res;
        }
    }
    // A MISS (or a verification, which the game must rebuild): recycle a slot so the context's current state is kept, then let the game build over what it was handed.
    const bool known = c.wasEvicted(key);
    int v = c.victim(g, hit, verify ? key : nullptr, extra);
    bool rebuilds = std::memcmp(ctxState, key, kKeyBytes) != 0;   // true: the game builds over the state in the context (it is not the call's key)
    if (v >= 0) {
        Slot& s = c.slot(v);
        const uint8_t* incoming = s.populated ? s.state : grp.templ;
        const uint64_t incomingExtra = s.populated ? s.extra : kExtraUnknown;
        uint8_t keep[kStateBytes];
        std::memcpy(keep, incoming, kStateBytes);
        if (scatter<Mem>(ctx, keep)) {
            if (s.populated && !s.dead) c.noteEvicted(s.state);
            std::memcpy(s.state, ctxState, kStateBytes);
            s.extra = grp.ctxExtra;
            s.populated = true;
            s.dead = false;
            s.used = ++grp.clock;
            grp.ctxExtra = incomingExtra;
            rebuilds = std::memcmp(keep, key, kKeyBytes) != 0;
        } else {
            disable(c, scatter<Mem>(ctx, ctxState) ? "a write into the cluster context faulted during a swap"
                                                    : "a write into the cluster context faulted and could not be undone");
            res.disableNow = true;
            ++st.forwarded;
            ++st.byReason[kSwapFault];
            fwd();
            return res;
        }
    }
    if (known) ++st.capacityMisses;
    probeNeighbour(c, g, key, in.jitter);
    const double t1 = now();
    fwd();
    const double t2 = now();
    if (rebuilds) grp.ctxExtra = extra;   // the game built over it with this call's inputs; if its memo hit instead, the state keeps the extra it had
    ++st.misses;
    st.missMs += t2 - t1;
    st.missOverheadMs += (t1 - t0);
    res.served = Served::kMiss;
    // The key self-check: after the game's build its stored key must be the one built from the inputs. On a difference the inputs are derived once more: if THEY have changed, the
    // view block moved under the call (the game read one thing and the cache another) and nothing is wrong with the derivation; the call counts apart and the cache goes on. If they
    // have not, the derivation is wrong and the cache turns itself off, naming the fields.
    uint8_t after[kKeyBytes];
    if (Mem::read(after, ctx, kKeyBytes)) {
        if (std::memcmp(after, key, kKeyBytes) == 0) {
            ++st.selfOk;
        } else {
            bool moved = false;
            if (in.rederive != nullptr) {
                CallInputs again = in;
                uint8_t key2[kKeyBytes];
                moved = in.rederive(in.rederiveUser, &again) && buildKey(again, key2) && std::memcmp(key2, key, kKeyBytes) != 0;
            }
            grp.ctxExtra = kExtraUnknown;
            if (moved) {
                ++st.selfRace;
            } else {
                ++st.selfFail;
                char diff[600];
                describeKeyDiff(key, after, diff, sizeof(diff));
                uint8_t post[kStateBytes];
                unsigned long long n0 = 0, n1 = 0, n2 = 0;
                if (gather<Mem>(ctx, post)) {
                    n0 = stateVecSize(post, 0);
                    n1 = stateVecSize(post, 1);
                    n2 = stateVecSize(post, 2);
                }
                char why[900];
                std::snprintf(why, sizeof(why),
                              "the key built from the call's inputs differs from the one the game stored (%s); a second derivation after the call gave the same key, so the view "
                              "block did not move; after the build the vectors hold %llu/%llu/%llu elements; override path %u, extra near 0x%08x",
                              diff, n0, n1, n2, static_cast<unsigned>(in.overridePath), in.extraNearBits);
                disable(c, why);
                res.disableNow = true;
            }
        }
    }
    if (verify && hit >= 0 && !c.disabled) {
        uint8_t fresh[kStateBytes];
        if (gather<Mem>(ctx, fresh)) {
            ++st.verifyChecks;
            const Compare cmp = compareStates<Mem>(fresh, c.slot(hit).state);
            if (cmp.verdict == Same::kDiffers || cmp.verdict == Same::kUnreadable) {
                ++st.verifyMismatches;
                char why[256];
                std::snprintf(why, sizeof(why), "verification: the cached state differs from a fresh build (%s %d, byte %llu, sizes %llu and %llu)",
                              cmp.verdict == Same::kUnreadable ? "unreadable table" : "table", cmp.table, static_cast<unsigned long long>(cmp.offset),
                              static_cast<unsigned long long>(cmp.sizeA), static_cast<unsigned long long>(cmp.sizeB));
                disable(c, why);
                res.disableNow = true;
            } else {
                if (cmp.verdict == Same::kPaddingOnly) ++st.verifyPadding;
                c.slot(hit).dead = true;   // a verified duplicate of what the context now holds; recycled first
            }
        }
    }
    return res;
}

}  // namespace edvr::lgc
