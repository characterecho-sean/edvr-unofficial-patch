// Test rig for the light grid cache's pure half (src/d3d11/light_grid_cache_core.h): the identity and pin decision, the key built as the game builds its own, the state
// regions, the bit-exact compare and the LRU, whole-state swaps against a fake builder that owns real allocations (so a state that ever pointed at freed memory, or at one
// buffer from two places, fails an audit), the forward reasons, the verify-and-disable path, faults in the memory policy, the thread rule, the jitter probe and the exact
// line texts. No DLL and no game.
#include "../../src/d3d11/light_grid_cache_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lg = edvr::lgc;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// ---- a fake game: a context at the real offsets, vectors with the real header, allocations that are tracked --------------------------------------------------------------
struct Heap {
    std::map<uintptr_t, size_t> live;
    uint64_t allocs = 0, frees = 0;
    void* alloc(size_t n) {
        void* p = std::calloc(1, n ? n : 1);
        live[reinterpret_cast<uintptr_t>(p)] = n;
        ++allocs;
        return p;
    }
    void release(void* p) {
        if (!p) return;
        const auto it = live.find(reinterpret_cast<uintptr_t>(p));
        check(it != live.end(), "the game frees only what is live (no double free, no foreign pointer)");
        if (it == live.end()) return;
        live.erase(it);
        std::free(p);
        ++frees;
    }
    bool isLive(uint64_t p) const { return p == 0 || live.count(static_cast<uintptr_t>(p)) != 0; }
};

struct Ctx {
    alignas(16) uint8_t mem[0x160] = {};
    Ctx() { mem[lg::kCtxFirstFlag] = 1; }   // as constructed: key zero, no vectors, no GPU objects, the first-call flag set
    uint8_t* p() { return mem; }
};

uint64_t ld64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
void st64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

// The game's vector (0x5954A0): capacity at +0, size at +0x10, data at +0x18; growth past the capacity reallocates, new 32-byte elements are filled with the default.
void vecResize(Heap& h, uint8_t* hdr, uint64_t newSize, size_t elem) {
    const uint64_t cap = ld64(hdr), size = ld64(hdr + 0x10);
    auto* data = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(ld64(hdr + 0x18)));
    if (newSize > cap) {
        const uint64_t newCap = newSize > cap + cap / 2 ? newSize : cap + cap / 2;
        auto* fresh = static_cast<uint8_t*>(h.alloc(newCap * elem));
        if (data) std::memcpy(fresh, data, size * elem);
        h.release(data);
        data = fresh;
        st64(hdr, newCap);
        st64(hdr + 0x18, reinterpret_cast<uintptr_t>(data));
    }
    if (elem == 32) {
        for (uint64_t i = size; i < newSize; ++i) {
            const float one = 1.0f;
            std::memcpy(data + i * 32 + 12, &one, 4);
            std::memcpy(data + i * 32 + 28, &one, 4);
        }
    }
    st64(hdr + 0x10, newSize);
}

struct Knobs {
    bool impure = false;        // the tables depend on something that is not in the key
    int hidden = 0;
    bool wrongKey = false;      // the stored key is not the one built from the inputs
    bool paddingNoise = false;  // the padding lanes of the 32-byte elements differ between builds
    uint64_t builds = 0;        // calls that rebuilt
    uint64_t calls = 0;
};

float valueFor(const uint8_t* key, uint64_t i, int j, int hidden) {
    uint64_t h = lg::hashKey(key) ^ (i * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(j) * 0xC2B2AE3D27D4EB4Full) ^ static_cast<uint64_t>(hidden);
    h ^= h >> 31;
    h *= 0xD6E8FEB86659FD93ull;
    h ^= h >> 32;
    return static_cast<float>(h & 0xFFFFFF) / 1000.0f;
}

// The game's builder, as far as the cache can tell: the first-call copy, the memo, the key stored last, three tables, four GPU objects replaced.
void fakeBuild(Heap& h, Knobs& k, uint8_t* ctx, const lg::CallInputs& in) {
    ++k.calls;
    if (ctx[lg::kCtxFirstFlag]) {
        ctx[lg::kCtxFirstFlag] = 0;
        std::memcpy(ctx + 0xD0, in.rows, 0x40);
    }
    uint8_t key[lg::kKeyBytes];
    if (!lg::buildKey(in, key)) return;   // a real build of such inputs is not modelled
    if (in.forceRebuild == 0 && std::memcmp(ctx, key, lg::kKeyBytes) == 0) return;   // the game's own memo
    ++k.builds;
    const uint64_t gx = ld64(key + 0x48), gy = ld64(key + 0x50), slices = ld64(key + 0x58);
    const uint64_t total = gx * gy * slices;
    vecResize(h, ctx + 0x80, total, 32);
    vecResize(h, ctx + 0xA0, slices, 32);
    vecResize(h, ctx + 0x110, total, 48);
    const int hid = (k.impure ? k.hidden : 0) ^ static_cast<int>(in.extraNearBits);   // the first slice's near is read again from the view block
    for (int t = 0; t < 3; ++t) {
        const uint8_t* hdr = ctx + (t == 0 ? 0x80 : t == 1 ? 0xA0 : 0x110);
        auto* data = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(ld64(hdr + 0x18)));
        const uint64_t n = ld64(hdr + 0x10);
        for (uint64_t i = 0; i < n; ++i) {
            if (t < 2) {
                for (int j = 0; j < 3; ++j) {
                    const float a = valueFor(key, i, j + 10 * t, hid), b = valueFor(key, i, j + 20 + 10 * t, hid);
                    std::memcpy(data + i * 32 + 4 * j, &a, 4);
                    std::memcpy(data + i * 32 + 16 + 4 * j, &b, 4);
                }
                if (k.paddingNoise) {
                    const float noise = static_cast<float>(k.hidden);
                    std::memcpy(data + i * 32 + 12, &noise, 4);
                    std::memcpy(data + i * 32 + 28, &noise, 4);
                }
            } else {
                for (int j = 0; j < 12; ++j) {
                    const float a = valueFor(key, i, j + 40, hid);
                    std::memcpy(data + i * 48 + 4 * j, &a, 4);
                }
            }
        }
    }
    for (int g = 0; g < 4; ++g) {
        h.release(reinterpret_cast<void*>(static_cast<uintptr_t>(ld64(ctx + 0x130 + 8 * g))));
        st64(ctx + 0x130 + 8 * g, 0);
    }
    // two buffers made from the tables (a copy of each), then their two views (each names its buffer)
    for (int t = 0; t < 2; ++t) {
        const uint8_t* hdr = ctx + (t == 0 ? 0x80 : 0xA0);
        const uint64_t bytes = ld64(hdr + 0x10) * 32;
        auto* buf = static_cast<uint8_t*>(h.alloc(bytes + 8));
        std::memcpy(buf + 8, reinterpret_cast<const void*>(static_cast<uintptr_t>(ld64(hdr + 0x18))), bytes);
        st64(ctx + 0x130 + 8 * t, reinterpret_cast<uintptr_t>(buf));
    }
    for (int t = 0; t < 2; ++t) {
        auto* view = static_cast<uint8_t*>(h.alloc(16));
        st64(view, ld64(ctx + 0x130 + 8 * t));
        st64(ctx + 0x140 + 8 * t, reinterpret_cast<uintptr_t>(view));
    }
    uint8_t stored[lg::kKeyBytes];
    std::memcpy(stored, key, lg::kKeyBytes);
    if (k.wrongKey) stored[0x20] ^= 1;
    std::memcpy(ctx, stored, lg::kKeyBytes);
}

// A fresh game's build of exactly these inputs, for the oracle.
struct Oracle {
    Heap h;
    Knobs k;
    Ctx c;
    void build(const lg::CallInputs& in) {
        fakeBuild(h, k, c.p(), in);   // the first call builds: the key is zero
    }
};

bool sameTables(const uint8_t* a, const uint8_t* b) {   // two contexts' tables, byte for byte
    for (int t = 0; t < 3; ++t) {
        const size_t off = t == 0 ? 0x80 : t == 1 ? 0xA0 : 0x110;
        const size_t elem = t == 2 ? 48 : 32;
        if (ld64(a + off + 0x10) != ld64(b + off + 0x10)) return false;
        const uint64_t n = ld64(a + off + 0x10) * elem;
        if (n == 0) continue;
        if (std::memcmp(reinterpret_cast<const void*>(static_cast<uintptr_t>(ld64(a + off + 0x18))),
                        reinterpret_cast<const void*>(static_cast<uintptr_t>(ld64(b + off + 0x18))), static_cast<size_t>(n)) != 0) return false;
    }
    return true;
}

// ---- inputs -----------------------------------------------------------------------------------------------------------------------------------------------------------------
lg::CallInputs mk(int eye, int phase, int variant = 0) {
    lg::CallInputs in;
    float rows[16] = {1.2f + 0.01f * eye, 0, 0, 0.001f * phase + 0.5f * eye,   //
                      0, 0.9f + 0.01f * eye, 0, 0.002f,                         //
                      0, 0, 0.7f, 0.003f * phase - 0.25f * eye,                 //
                      0, 0, 1.0f, -0.4f};
    if (variant == 1) rows[1] = 0.1f;   // a third view, differently shaped
    std::memcpy(in.rows, rows, sizeof(rows));
    const float nearZ = 0.1f, farZ = 10000.0f;
    std::memcpy(&in.nearBits, &nearZ, 4);
    std::memcpy(&in.farBits, &farZ, 4);
    in.extraNearBits = in.nearBits;
    in.tile = 16;
    in.slices = 4;
    in.width = variant == 1 ? 96 : 128;
    in.height = variant == 1 ? 64 : 128;
    in.renderer = 0xABCD;
    return in;
}

struct Clock {
    double t = 0;
    double operator()() { return t += 0.001; }
};

// One call through the cache onto one fake context.
struct Rig {
    lg::Cache cache;
    Heap h;
    Knobs k;
    Clock clock;
    bool moveNext = false;   // the next call: the view block changes under the call (the game builds from a different row than the cache derived)
    lg::Result call(Ctx& ctx, const lg::CallInputs& in) {
        // A second derivation gives what the view block holds NOW: the same inputs, or (a race) the inputs the game built from.
        lg::CallInputs seen = in;
        lg::CallInputs moved = in;
        float f;
        std::memcpy(&f, moved.rows, 4);
        f += 0.25f;
        std::memcpy(moved.rows, &f, 4);
        const bool move = moveNext;
        moveNext = false;
        const lg::CallInputs& gameSees = move ? moved : in;
        seen = gameSees;
        lg::CallInputs withRederive = in;
        withRederive.rederive = [](const void* user, lg::CallInputs* out) { *out = *static_cast<const lg::CallInputs*>(user); return true; };
        withRederive.rederiveUser = &seen;
        return lg::serve<lg::PlainMem>(cache, ctx.p(), withRederive, [&] { fakeBuild(h, k, ctx.p(), gameSees); clock.t += 0.3; }, clock);
    }
};

// Every pointer a state holds, over the contexts and the slots: live, and held from exactly one place.
bool auditOwnership(Rig& r, std::vector<Ctx*> ctxs) {
    std::set<uint64_t> seen;
    bool ok = true;
    auto take = [&](const uint8_t* state, bool ctxLayout) {
        const uint8_t* vec[3] = {state + (ctxLayout ? 0x80 : lg::kVecOff[0]), state + (ctxLayout ? 0xA0 : lg::kVecOff[1]), state + (ctxLayout ? 0x110 : lg::kVecOff[2])};
        for (const uint8_t* v : vec) {
            const uint64_t d = ld64(v + 0x18);
            if (d == 0) continue;
            ok = ok && r.h.isLive(d) && seen.insert(d).second;
        }
        const uint8_t* gpu = state + (ctxLayout ? 0x130 : lg::kGpuOff);
        for (int g = 0; g < 4; ++g) {
            const uint64_t p = ld64(gpu + 8 * g);
            if (p == 0) continue;
            ok = ok && r.h.isLive(p) && seen.insert(p).second;
        }
    };
    for (Ctx* c : ctxs) take(c->mem, true);
    for (int i = 0; i < lg::kPoolSlots; ++i) {
        if (r.cache.slot(i).owner >= 0 && r.cache.slot(i).populated) take(r.cache.slot(i).state, false);
    }
    return ok;
}

// The state must be coherent: the views name the buffers, the buffers hold the tables, the key is the stored one.
bool coherent(Ctx& c) {
    for (int t = 0; t < 2; ++t) {
        const uint64_t buf = ld64(c.mem + 0x130 + 8 * t), view = ld64(c.mem + 0x140 + 8 * t);
        if (buf == 0 || view == 0) return false;
        if (ld64(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(view))) != buf) return false;
        const uint8_t* hdr = c.mem + (t == 0 ? 0x80 : 0xA0);
        const uint64_t bytes = ld64(hdr + 0x10) * 32;
        if (std::memcmp(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(buf)) + 8, reinterpret_cast<const void*>(static_cast<uintptr_t>(ld64(hdr + 0x18))), static_cast<size_t>(bytes)) != 0) return false;
    }
    return true;
}

// ---- memory policies that fail ---------------------------------------------------------------------------------------------------------------------------------------
struct FailReads {
    static inline int failAt;   // fail the Nth read (1-based); 0 never
    static inline int count;
    static bool read(void* dst, const void* src, size_t n) noexcept {
        if (failAt != 0 && ++count == failAt) return false;
        std::memcpy(dst, src, n);
        return true;
    }
    static bool write(void* dst, const void* src, size_t n) noexcept { std::memcpy(dst, src, n); return true; }
};
struct FailWrites {
    static inline int failAt;
    static inline int count;
    static inline bool always;
    static bool read(void* dst, const void* src, size_t n) noexcept { std::memcpy(dst, src, n); return true; }
    static bool write(void* dst, const void* src, size_t n) noexcept {
        ++count;
        if (always ? count >= failAt : count == failAt) return false;
        std::memcpy(dst, src, n);
        return true;
    }
};

void plant(lg::Cache& c, int g, const uint8_t* key, bool populated = true) {
    const int i = c.victim(g, -1, nullptr);
    lg::Slot& s = c.slot(i);
    s.populated = populated;
    s.extra = 0;
    std::memcpy(s.state, key, lg::kKeyBytes);
}

void testIdentityAndPin() {
    uint8_t good[16];
    std::memcpy(good, lg::kPrologue, 16);
    check(lg::installDecision(true, lg::kExpectedTimestamp, lg::kExpectedImageSize, good, 16) == lg::Install::kInstall, "the pinned build and prologue install");
    check(lg::installDecision(false, lg::kExpectedTimestamp, lg::kExpectedImageSize, good, 16) == lg::Install::kUnreadable, "an unreadable header refuses");
    check(lg::installDecision(true, lg::kExpectedTimestamp, lg::kExpectedImageSize, nullptr, 16) == lg::Install::kUnreadable, "an unreadable entry refuses");
    check(lg::installDecision(true, lg::kExpectedTimestamp, lg::kExpectedImageSize, good, 8) == lg::Install::kUnreadable, "a short read of the entry refuses");
    check(lg::installDecision(true, lg::kExpectedTimestamp + 1, lg::kExpectedImageSize, good, 16) == lg::Install::kWrongIdentity, "another PE stamp refuses");
    check(lg::installDecision(true, lg::kExpectedTimestamp, lg::kExpectedImageSize + 4096, good, 16) == lg::Install::kWrongIdentity, "another image size refuses");
    uint8_t moved[16];
    std::memcpy(moved, good, 16);
    moved[15] ^= 1;
    check(lg::installDecision(true, lg::kExpectedTimestamp, lg::kExpectedImageSize, moved, 16) == lg::Install::kWrongPrologue, "one wrong prologue byte refuses");
    // the identity is judged before the bytes: a wrong build whose bytes also differ is a wrong build
    check(lg::installDecision(true, 1, 2, moved, 16) == lg::Install::kWrongIdentity, "the identity is judged first");
    check(lg::kExpectedTimestamp == 1788384820u && lg::kExpectedImageSize == 104894464u && lg::kBuilderRva == 0x58B150u, "the pinned constants are build 332841's");
    const uint8_t pin[16] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8};
    check(std::memcmp(pin, lg::kPrologue, 16) == 0, "the prologue is the sixteen bytes read from the exe");
    // the view-block refresh the glue may run ahead of the rows is pinned the same way
    uint8_t refresh[16];
    std::memcpy(refresh, lg::kRefreshPrologue, 16);
    check(lg::refreshPinMatches(refresh, 16) && lg::kRefreshRva == 0x4F2FF0u, "the refresh function's sixteen bytes match the pin");
    refresh[7] ^= 1;
    check(!lg::refreshPinMatches(refresh, 16) && !lg::refreshPinMatches(nullptr, 16) && !lg::refreshPinMatches(refresh, 8), "a changed byte, a null read or a short read does not");
    const uint8_t refreshPin[16] = {0x48, 0x8B, 0xC4, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x0F, 0x29, 0x70, 0xE8, 0x48};
    check(std::memcmp(refreshPin, lg::kRefreshPrologue, 16) == 0, "the refresh pin is the sixteen bytes read from the exe");
}

// The routine at 0x48AAAE8, transcribed instruction by instruction with the registers as variables (edx, eax, ecx, r8d), as an independent reading to hold the core's gameCeilf to.
float asmCeilf(float xmm0) {
    uint32_t r8d = 0, edx, eax, ecx;
    std::memcpy(&edx, &xmm0, 4);   // movss [rsp+8], xmm0 / mov edx, [rsp+8]
    eax = edx;
    eax &= 0x7FFFFFFFu;            // btr eax, 0x1f
    r8d = edx != eax ? 1u : 0u;    // cmp edx, eax / setne r8b
    auto fromBits = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; };
    if (eax >= 0x4B800000u) return xmm0;               // jb not taken: 48AAB08 jbe 48AAB6E (infinity, NaN excepted) returns xmm0
    if (eax < 0x3F800000u) {                           // 48AAB16 jae 48AAB40 not taken
        if (eax == 0) return xmm0;                     // je 48AAB6E
        if (r8d != 0) return fromBits(0x80000000u);    // 48AAB26
        return 1.0f;                                   // 48AAB36: movss xmm0, [0x4DDE614]
    }
    eax = edx;                                         // 48AAB40
    ecx = 0x96;
    eax >>= 0x17;                                      // shr eax, 0x17
    ecx -= eax;                                        // sub ecx, eax
    eax = 0xFFFFFFFFu;                                 // or eax, -1
    eax <<= (ecx & 31u);                               // shl eax, cl
    eax &= edx;                                        // and eax, edx
    xmm0 = fromBits(eax);                              // 48AAB53..57
    if (r8d != 0) return xmm0;                         // test r8d, r8d / jne
    if (edx == eax) return xmm0;                       // cmp edx, eax / je
    return xmm0 + 1.0f;                                // addss xmm0, [0x4DDE614]
}

void testGridCeil() {
    // Flight 3 (2026-10-14): the game's grid for 2016 / 120 was 17, not 16; 9248 elements = 17 x 17 x 32.
    uint64_t g = 0;
    check(lg::gridCells(2016, 120, &g) && g == 17, "2016 / 120 = 16.8 gives 17");
    check(lg::gridCells(1920, 120, &g) && g == 16, "1920 / 120 = 16 exactly gives 16 (a width divisible by the tile is not rounded up)");
    check(lg::gridCells(1921, 120, &g) && g == 17, "1921 / 120 gives 17");
    check(lg::gridCells(2040, 120, &g) && g == 17 && lg::gridCells(2041, 120, &g) && g == 18, "2040 / 120 = 17 exactly gives 17, 2041 gives 18");
    check(lg::gridCells(256, 120, &g) && g == 3 && lg::gridCells(612, 120, &g) && g == 6 && lg::gridCells(248, 120, &g) && g == 3, "the flight's other sizes: 256, 612 and 248 over 120 give 3, 6, 3");
    check(lg::gridCells(1, 120, &g) && g == 1 && lg::gridCells(120, 120, &g) && g == 1 && lg::gridCells(121, 120, &g) && g == 2, "1, 120 and 121 over 120 give 1, 1 and 2");
    check(lg::gridCells(3840, 120, &g) && g == 32 && lg::gridCells(2560, 120, &g) && g == 22 && lg::gridCells(1080, 120, &g) && g == 9, "flat sizes: 3840, 2560 and 1080 over 120 give 32, 22 and 9");
    uint8_t key[lg::kKeyBytes];
    lg::CallInputs in = mk(0, 0);
    in.width = in.height = 2016;
    in.tile = 120;
    in.slices = 32;
    check(lg::buildKey(in, key) && ld64(key + 0x48) == 17 && ld64(key + 0x50) == 17 && ld64(key + 0x48) * ld64(key + 0x50) * ld64(key + 0x58) == 9248,
          "a 2016x2016 target with tile 120 and 32 slices is the flight's 17 x 17 x 32 = 9248 clusters");
    // gameCeilf against the instruction-by-instruction transcription and against ceil, over every quotient a tile and a size can make, and a sweep of bit patterns
    uint64_t compared = 0;
    bool same = true, sameAsCeil = true;
    const uint32_t tiles[] = {1, 2, 3, 8, 16, 24, 32, 48, 64, 100, 120, 128, 240, 256, 1000};
    for (uint32_t tile : tiles) {
        for (uint32_t n = 1; n <= 9000; ++n) {
            const float q = static_cast<float>(static_cast<int64_t>(n)) / static_cast<float>(static_cast<int64_t>(tile));
            const float a = lg::gameCeilf(q), b = asmCeilf(q);
            uint32_t ba, bb;
            std::memcpy(&ba, &a, 4);
            std::memcpy(&bb, &b, 4);
            same = same && ba == bb;
            sameAsCeil = sameAsCeil && a == std::ceil(q);
            ++compared;
        }
    }
    uint32_t state = 12345u;
    for (int i = 0; i < 400000; ++i) {
        state = state * 1664525u + 1013904223u;
        uint32_t bits = state;
        if (i % 3 == 0) bits &= 0x7FFFFFFFu;   // positive
        float x;
        std::memcpy(&x, &bits, 4);
        if (!std::isfinite(x)) continue;
        const float a = lg::gameCeilf(x), b = asmCeilf(x);
        uint32_t ba, bb;
        std::memcpy(&ba, &a, 4);
        std::memcpy(&bb, &b, 4);
        same = same && ba == bb;
        sameAsCeil = sameAsCeil && (a == std::ceil(x) || (a == 0.0f && std::ceil(x) == 0.0f));
        ++compared;
    }
    check(compared > 100000 && same, "gameCeilf equals the transcription of 0x48AAAE8 on every quotient and 400000 bit patterns, bit for bit");
    check(sameAsCeil, "and both equal ceilf (the routine is ceilf)");
    check(lg::gameCeilf(0.5f) == 1.0f && lg::gameCeilf(16.0f) == 16.0f && lg::gameCeilf(16.0001f) == 17.0f && lg::gameCeilf(-0.5f) == 0.0f && lg::gameCeilf(0.0f) == 0.0f,
          "spot values: 0.5 gives 1, 16 gives 16, 16.0001 gives 17, -0.5 gives -0, 0 gives 0");
}

void testKey() {
    lg::CallInputs in = mk(0, 3);
    uint8_t key[lg::kKeyBytes];
    check(lg::buildKey(in, key), "ordinary inputs build a key");
    check(std::memcmp(key, in.rows, 0x40) == 0, "the rows are the key's first 64 bytes");
    check(std::memcmp(key + 0x40, &in.nearBits, 4) == 0 && std::memcmp(key + 0x44, &in.farBits, 4) == 0, "near and far follow at +0x40 and +0x44");
    check(ld64(key + 0x48) == 8 && ld64(key + 0x50) == 8, "the grid is ceil(size / tile): 128 / 16 = 8");
    check(ld64(key + 0x58) == 4 && ld64(key + 0x60) == 128 && ld64(key + 0x68) == 128 && ld64(key + 0x70) == 16, "slices, width, height and tile fill the six quadwords");
    in.width = 1000;
    in.height = 2016;
    in.tile = 16;
    check(lg::buildKey(in, key) && ld64(key + 0x48) == 63 && ld64(key + 0x50) == 126, "the grid rounds up: 1000 / 16 = 62.5 gives 63, 2016 / 16 = 126 exactly gives 126");
    in.tile = 0;
    check(!lg::buildKey(in, key), "a zero tile is not built (the game's divide is the game's)");
    in = mk(0, 0);
    in.width = 0;
    check(!lg::buildKey(in, key), "a zero width is not built");
    in = mk(0, 0);
    in.tile = 0xFFFFFFFFu;
    check(lg::buildKey(in, key) && ld64(key + 0x48) == 1, "a tile larger than the target gives a grid of 1, as the game's ceil does");
    // The key a fake game stores for these inputs is the key the core builds (the self-check the hook runs on every miss)
    Oracle o;
    in = mk(1, 5);
    o.build(in);
    uint8_t want[lg::kKeyBytes];
    lg::buildKey(in, want);
    check(std::memcmp(o.c.mem, want, lg::kKeyBytes) == 0, "the stored key equals the built key");
}

void testRegions() {
    Ctx c;
    std::memset(c.mem, 0xAA, sizeof(c.mem));
    uint8_t state[lg::kStateBytes];
    for (size_t i = 0; i < sizeof(state); ++i) state[i] = static_cast<uint8_t>(i);
    check(lg::scatter<lg::PlainMem>(c.p(), state), "a state scatters");
    for (size_t i = 0; i < 0x78; ++i) check(c.mem[i] == state[i], "the key region receives the state's first 0x78 bytes");
    check(c.mem[0x78] == 0xAA && c.mem[0x7F] == 0xAA, "the eight bytes after the key are not touched");
    check(c.mem[0xC0] == 0xAA && c.mem[0xD0] == 0xAA && c.mem[0x10F] == 0xAA, "the first-call flag and the copied matrix (0xC0..0x10F) are not touched");
    check(c.mem[0x150] == 0xAA && c.mem[0x15F] == 0xAA, "nothing past the four GPU pointers is touched");
    check(std::memcmp(c.mem + 0x80, state + 0x78, 0x40) == 0 && std::memcmp(c.mem + 0x110, state + 0xB8, 0x40) == 0, "the vector headers and the GPU pointers land where they came from");
    uint8_t back[lg::kStateBytes];
    check(lg::gather<lg::PlainMem>(c.p(), back) && std::memcmp(back, state, sizeof(state)) == 0, "gather is the inverse of scatter");
    check(lg::kStateBytes == 0xF8 && lg::kGpuOff + 32 == lg::kStateBytes, "a state is 0xF8 bytes and ends with the four GPU pointers");
    // pristine
    Ctx fresh;
    uint8_t s0[lg::kStateBytes];
    lg::gather<lg::PlainMem>(fresh.p(), s0);
    check(lg::statePristine(s0), "a constructed context is pristine");
    for (size_t at : {size_t(3), size_t(0x78 + 0x10), size_t(0x98 + 0x18), size_t(0xB8 + 0x10), size_t(0xD8), size_t(0xF0)}) {
        uint8_t t[lg::kStateBytes];
        std::memcpy(t, s0, sizeof(t));
        t[at] = 1;
        check(!lg::statePristine(t), "a key byte, a size, a data pointer or a GPU pointer set means built");
    }
    uint8_t cap[lg::kStateBytes];
    std::memcpy(cap, s0, sizeof(cap));
    cap[0x78] = 7;   // a capacity or hint alone is still a constructed vector
    check(lg::statePristine(cap), "a nonzero capacity header with no data is still pristine");
}

void testKeyCompareAndLru() {
    lg::Cache c;
    const int g = c.groupFor(0x1000);
    check(g == 0, "the first context is group 0");
    uint8_t key[lg::kKeyBytes];
    lg::buildKey(mk(0, 1), key);
    plant(c, g, key);
    check(c.find(g, key, 0) >= 0, "an equal key is found");
    int missed = 0;
    for (size_t byte = 0; byte < lg::kKeyBytes; ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            uint8_t other[lg::kKeyBytes];
            std::memcpy(other, key, sizeof(other));
            other[byte] ^= static_cast<uint8_t>(1u << bit);
            if (c.find(g, other, 0) < 0) ++missed;
        }
    }
    check(missed == static_cast<int>(lg::kKeyBytes) * 8, "every single-bit difference in all 120 key bytes misses (the compare is bit exact)");
    uint8_t zero[lg::kKeyBytes], negzero[lg::kKeyBytes];
    std::memcpy(zero, key, sizeof(zero));
    std::memcpy(negzero, key, sizeof(negzero));
    const float z = 0.0f, nz = -0.0f;
    std::memcpy(zero + 4, &z, 4);
    std::memcpy(negzero + 4, &nz, 4);
    lg::Cache c2;
    const int g2 = c2.groupFor(0x2000);
    plant(c2, g2, zero);
    check(c2.find(g2, negzero, 0) < 0, "+0.0 and -0.0 are different keys (the game's float compare would call them equal; the cache does not)");
    // dead, other groups and skip
    const int slot = c.find(g, key, 0);
    c.slot(slot).dead = true;
    check(c.find(g, key, 0) < 0, "a dead slot is not matched");
    c.slot(slot).dead = false;
    const int gOther = c.groupFor(0x3000);
    check(gOther == 1 && c.find(gOther, key, 0) < 0, "another context never matches this context's slot");
    check(c.find(g, key, 0, slot) < 0, "skip excludes a slot");

    // victim order: dead, then an unassigned slot while under the cap, then the least recently used
    lg::Cache d;
    const int a = d.groupFor(0xA000);
    for (int i = 0; i < lg::kSlotsPerContext; ++i) {
        uint8_t k[lg::kKeyBytes] = {};
        k[0] = static_cast<uint8_t>(i + 1);
        const int v = d.victim(a, -1, nullptr);
        check(v >= 0 && d.slot(v).owner == a, "under the cap a fresh slot is assigned from the pool");
        d.slot(v).populated = true;
        d.slot(v).extra = 0;
        d.slot(v).used = static_cast<uint64_t>(100 + i);
        std::memcpy(d.slot(v).state, k, sizeof(k));
    }
    int owned = 0;
    for (int i = 0; i < lg::kPoolSlots; ++i) owned += d.slot(i).owner == a ? 1 : 0;
    check(owned == lg::kSlotsPerContext, "a context owns at most kSlotsPerContext slots");
    const int lru = d.victim(a, -1, nullptr);
    check(lru >= 0 && d.slot(lru).used == 100, "at the cap the least recently used slot is the victim");
    check(d.slot(d.victim(a, lru, nullptr)).used == 101, "the avoided slot is skipped, the next oldest is taken");
    uint8_t avoidKey[lg::kKeyBytes] = {};
    avoidKey[0] = 1;   // the oldest slot holds this key
    check(d.slot(d.victim(a, -1, avoidKey)).used == 101, "a slot that already holds the key being checked is not the victim");
    d.slot(5).dead = true;
    check(d.victim(a, -1, nullptr) == 5, "a dead slot is taken first");
    const int b = d.groupFor(0xB000);
    int taken = 0;
    for (int i = 0; i < lg::kPoolSlots; ++i) {
        const int v = d.victim(b, -1, nullptr);
        if (v < 0) break;
        if (d.slot(v).owner == b && !d.slot(v).populated) { d.slot(v).populated = true; ++taken; }
        else break;
    }
    check(taken == lg::kPoolSlots - lg::kSlotsPerContext, "a second context gets the rest of the pool and no more");
    // the evicted ring
    lg::Cache e;
    uint8_t k1[lg::kKeyBytes] = {1}, k2[lg::kKeyBytes] = {2};
    e.noteEvicted(k1);
    check(e.wasEvicted(k1) && !e.wasEvicted(k2), "a recycled key is remembered, another is not");
}

void testSteadyHits() {
    Rig r;
    Ctx eyes[1];
    // one context serves both eyes and 8 jitter phases, plus a third view: 17 keys
    std::vector<lg::CallInputs> keys;
    for (int eye = 0; eye < 2; ++eye) {
        for (int ph = 0; ph < 8; ++ph) keys.push_back(mk(eye, ph));
    }
    keys.push_back(mk(0, 0, 1));
    uint64_t callsMade = 0;
    for (int frame = 0; frame < 6 * 8; ++frame) {
        for (int eye = 0; eye < 2; ++eye) {
            const lg::CallInputs& in = keys[static_cast<size_t>(eye * 8 + frame % 8)];
            r.call(eyes[0], in);
            ++callsMade;
            if (frame % 8 == 0 && eye == 0) {
                r.call(eyes[0], keys[16]);
                ++callsMade;
            }
            check(coherent(eyes[0]), "after a call the context's state is coherent (views name buffers, buffers hold the tables)");
        }
        check(auditOwnership(r, {&eyes[0]}), "every pointer is live and held from exactly one place");
    }
    const lg::Stats& s = r.cache.stats;
    check(s.calls == callsMade, "every call is counted");
    check(!r.cache.disabled, "a clean run never turns the cache off");
    check(s.selfFail == 0 && s.verifyMismatches == 0, "no key self-check failed and no verification mismatched");
    check(s.misses <= 17 + s.verifyChecks, "after 17 keys the misses are the first sights and the verifications only");
    check(s.hits >= callsMade - 17 - 3 - s.verifyChecks, "after warm-up every call is a hit");
    check(s.verifyChecks >= 1, "the first hit is verified");
    check(r.k.builds == s.misses + s.byReason[lg::kFirst], "the game rebuilt only on misses (and the first call)");
    // the tables a hit leaves are the tables a fresh build of the same inputs makes
    for (size_t i = 0; i < keys.size(); ++i) {
        r.call(eyes[0], keys[i]);
        Oracle o;
        o.build(keys[i]);
        check(sameTables(eyes[0].mem, o.c.mem) && std::memcmp(eyes[0].mem, o.c.mem, lg::kKeyBytes) == 0, "the state after a call equals a fresh build of the same inputs");
    }
    // the live allocations stay bounded: nothing leaks per frame
    check(r.h.live.size() <= static_cast<size_t>(lg::kSlotsPerContext + 1) * 7, "live allocations are bounded by the states held");
    check(r.cache.slotsInUse() <= lg::kSlotsPerContext && r.cache.slotsInUse() >= 16, "the slots in use are 16 to 20 for 17 keys");
}

void testCapacityMissAndThrash() {
    Rig r;
    Ctx c;
    // 40 keys cycling through a group that keeps 20: every call misses; the capacity ring says so; ownership stays clean
    std::vector<lg::CallInputs> keys;
    for (int i = 0; i < 40; ++i) keys.push_back(mk(0, i));
    for (int round = 0; round < 3; ++round) {
        for (const lg::CallInputs& in : keys) {
            r.call(c, in);
            check(coherent(c), "a thrashing cache keeps the context coherent");
        }
        check(auditOwnership(r, {&c}), "a thrashing cache keeps every pointer unique and live");
    }
    check(r.cache.stats.hits == 0, "a cycle longer than the slots never hits (LRU)");
    check(r.cache.stats.capacityMisses > 0, "the capacity ring counts the misses on a key evicted lately");
    check(!r.cache.disabled && r.cache.stats.selfFail == 0, "thrashing is not a failure");
    check(r.h.live.size() <= static_cast<size_t>(lg::kSlotsPerContext + 1) * 7 + 7, "thrashing does not grow the heap");
}

void testTwoContexts() {
    Rig r;
    Ctx a, b;
    for (int round = 0; round < 40; ++round) {
        r.call(a, mk(0, round % 4));
        r.call(b, mk(1, round % 4));
    }
    check(r.cache.contexts() == 2, "two contexts have two groups");
    check(auditOwnership(r, {&a, &b}), "two contexts never share a pointer");
    for (int i = 0; i < lg::kPoolSlots; ++i) {
        if (r.cache.slot(i).owner < 0 || !r.cache.slot(i).populated) continue;
        const uint8_t* k = r.cache.slot(i).state;
        // a slot owned by group 0 holds eye-0 keys (row 0 col 0 is 1.2) and group 1 eye-1 keys (1.21)
        float row00;
        std::memcpy(&row00, k, 4);
        check((r.cache.slot(i).owner == 0) == (row00 < 1.205f) || ld64(k) == 0, "a slot holds only the states of the context that owns it");
    }
    check(r.cache.stats.hits > 40, "each context hits on its own states");
    Ctx c, d, e;   // contexts past the fourth go to the game
    r.call(c, mk(0, 0));
    r.call(d, mk(0, 1));
    const uint64_t before = r.cache.stats.byReason[lg::kContextsFull];
    r.call(e, mk(0, 2));
    check(r.cache.stats.byReason[lg::kContextsFull] == before + 1 && r.cache.contexts() == 4, "a fifth context is forwarded whole and counted");
}

void testForwardReasons() {
    Rig r;
    Ctx c;
    r.call(c, mk(0, 0));   // first call
    check(r.cache.stats.byReason[lg::kFirst] == 1 && r.cache.group(0).haveTemplate, "the first call of a constructed context is forwarded and takes the template");
    for (int i = 0; i < 3; ++i) r.call(c, mk(0, i + 1));
    uint64_t swapsBefore = r.cache.stats.misses + r.cache.stats.hits;
    lg::CallInputs in = mk(0, 1);
    in.viewFlags = lg::kViewDirtyBit;
    check(r.call(c, in).served == lg::Served::kGame && r.cache.stats.byReason[lg::kDirtyView] == 1, "a dirty view block is forwarded whole");
    in = mk(0, 1);
    in.rowsOk = false;
    r.call(c, in);
    check(r.cache.stats.byReason[lg::kNoRows] == 1, "a call whose rows the glue could not derive is forwarded whole");
    in = mk(0, 2);
    in.overridePath = 1;
    in.viewFlags = lg::kViewDirtyBit;
    check(r.call(c, in).served != lg::Served::kGame && r.cache.stats.byReason[lg::kDirtyView] == 1, "on the override path the view block's dirty bit does not matter (the build works on a copy)");
    check(r.cache.stats.pathOverride == 1 && r.cache.stats.pathPlain >= 6, "calls are counted by the path they take");
    swapsBefore = r.cache.stats.misses + r.cache.stats.hits;
    in = mk(0, 1);
    in.forceRebuild = 1;
    r.call(c, in);
    check(r.cache.stats.byReason[lg::kForceRebuild] == 1, "the forced rebuild flag is forwarded whole");
    in = mk(0, 1);
    in.tile = 0;
    r.call(c, in);
    check(r.cache.stats.byReason[lg::kUnbuildable] == 1, "inputs the key cannot be built from are forwarded whole");
    check(r.cache.stats.misses + r.cache.stats.hits == swapsBefore, "forwarded calls are neither hits nor misses");
    // the game's own memo: the same inputs twice
    r.call(c, mk(0, 6));
    const uint64_t memo = r.cache.stats.gameMemo;
    const uint64_t builds = r.k.builds;
    r.call(c, mk(0, 6));
    check(r.cache.stats.gameMemo == memo + 1 && r.k.builds == builds, "a call that equals the context's own key is the game's memo and rebuilds nothing");
    // a renderer change drops the slots and forwards
    in = mk(0, 2);
    in.renderer = 0xBEEF;
    r.call(c, in);
    check(r.cache.stats.byReason[lg::kRendererChange] == 1 && r.cache.slotsInUse() == 0, "a new renderer drops every kept state and is forwarded");
    // the first-call flag appearing again is a new life of the context
    r.call(c, mk(0, 3));
    r.call(c, mk(0, 4));
    check(r.cache.slotsInUse() > 0, "the cache refills after a reset");
    c.mem[lg::kCtxFirstFlag] = 1;
    std::memset(c.mem, 0, 0x78);
    for (int t = 0; t < 3; ++t) {
        // the old buffers belong to the game; a rebirth is the game's own destroy and construct, modelled by freeing them
        const size_t off = t == 0 ? 0x80 : t == 1 ? 0xA0 : 0x110;
        r.h.release(reinterpret_cast<void*>(static_cast<uintptr_t>(ld64(c.mem + off + 0x18))));
        std::memset(c.mem + off, 0, 0x20);
    }
    for (int g = 0; g < 4; ++g) {
        r.h.release(reinterpret_cast<void*>(static_cast<uintptr_t>(ld64(c.mem + 0x130 + 8 * g))));
        st64(c.mem + 0x130 + 8 * g, 0);
    }
    const uint64_t firsts = r.cache.stats.byReason[lg::kFirst];
    r.call(c, mk(0, 0));
    check(r.cache.stats.byReason[lg::kFirst] == firsts + 1 && r.cache.slotsInUse() == 0, "a context whose first-call flag is set again starts over: its kept states are dropped");
}

void testExtraNear() {
    // The build reads the view block's own near a second time, so a state is matched only by a call with the same one.
    Rig r;
    Ctx c;
    auto withExtra = [](lg::CallInputs in, float nearZ) {
        std::memcpy(&in.extraNearBits, &nearZ, 4);
        return in;
    };
    const lg::CallInputs a = withExtra(mk(0, 1), 0.1f), b = withExtra(mk(0, 1), 0.2f), other = mk(0, 2);
    r.call(c, mk(0, 0));            // first call
    r.call(c, a);                   // miss: a's state
    r.call(c, other);               // miss: a's state is parked
    const uint64_t hitsBefore = r.cache.stats.hits;
    r.call(c, b);                   // same key as a, another extra near: must NOT be served from a's state
    check(r.cache.stats.hits == hitsBefore, "the same key with another first-slice near is a miss");
    Oracle o;
    o.build(b);
    check(sameTables(c.mem, o.c.mem), "and the state it leaves is the fresh build with that near");
    r.call(c, other);
    r.call(c, a);
    check(r.cache.stats.hits > hitsBefore, "a call with the extra near a state was built with hits it");
    Oracle oa;
    oa.build(a);
    check(sameTables(c.mem, oa.c.mem) && coherent(c) && auditOwnership(r, {&c}), "and gets exactly that build, coherently and singly owned");
    // after the game builds behind the cache's back (a forwarded call), the context's extra is unknown: the parked copy never matches
    lg::CallInputs dirty = mk(0, 3);
    dirty.forceRebuild = 1;
    r.call(c, dirty);               // forwarded whole: the game rebuilt, extra now unknown
    r.call(c, other);               // miss: the unknown state is parked
    const uint64_t hits2 = r.cache.stats.hits;
    r.call(c, mk(0, 3));            // the parked state has the key of mk(0,3) but an unknown build
    check(r.cache.stats.hits == hits2, "a state the game rebuilt behind the cache's back is parked unmatchable");
    check(!r.cache.disabled && r.cache.stats.selfFail == 0, "none of this trips the self-check");
}

void testAlreadyBuilt() {
    // THE FLIGHT OF 2026-10-13: four contexts were met already built, with the first-call flag clear, a key set, vectors holding data and GPU objects present. The first
    // version captured its template only from a constructed context and stood down for all four (0 hits in 36 windows).
    Rig r;
    Ctx c;
    Knobs k;
    st64(c.mem + 0x88, 7);     // each vector's second header word (a floor the game may set)
    st64(c.mem + 0xA8, 9);
    st64(c.mem + 0x118, 11);
    fakeBuild(r.h, k, c.p(), mk(0, 0));
    fakeBuild(r.h, k, c.p(), mk(0, 1));
    check(c.mem[lg::kCtxFirstFlag] == 0 && ld64(c.mem + 0x98) != 0 && ld64(c.mem + 0x130) != 0, "the context is built and its first-call flag is clear, as in the flight");
    std::vector<lg::CallInputs> keys = {mk(0, 2), mk(0, 3), mk(0, 4)};
    for (int round = 0; round < 8; ++round) {
        for (const lg::CallInputs& in : keys) {
            r.call(c, in);
            check(coherent(c), "an already-built context stays coherent");
        }
        check(auditOwnership(r, {&c}), "and singly owned");
    }
    const lg::Stats& s = r.cache.stats;
    check(s.byReason[lg::kNoTemplate] == 0 && r.cache.group(0).haveTemplate, "a context first seen already built gets its template");
    check(s.hits > 0 && s.misses >= 3 && s.hits + s.misses + s.gameMemo == s.calls, "and is cached from its first call: hits and misses, no forwarding");
    check(!r.cache.disabled && s.selfFail == 0 && s.verifyMismatches == 0, "without tripping a check");
    for (const lg::CallInputs& in : keys) {
        r.call(c, in);
        Oracle o;
        o.build(in);
        check(sameTables(c.mem, o.c.mem), "the state it leaves is a fresh build of the same inputs");
    }
    // the template keeps each vector's own second header word
    check(ld64(c.mem + 0x88) == 7 && ld64(c.mem + 0xA8) == 9 && ld64(c.mem + 0x118) == 11, "every state the context holds keeps the game's header words");
    uint8_t cs[lg::kStateBytes], tpl[lg::kStateBytes];
    lg::gather<lg::PlainMem>(c.p(), cs);
    lg::makeEmptyTemplate(cs, tpl);
    bool zeroed = true;
    for (size_t i = 0; i < sizeof(tpl); ++i) {
        const bool hintWord = (i >= lg::kVecOff[0] + 8 && i < lg::kVecOff[0] + 16) || (i >= lg::kVecOff[1] + 8 && i < lg::kVecOff[1] + 16) || (i >= lg::kVecOff[2] + 8 && i < lg::kVecOff[2] + 16);
        if (!hintWord && tpl[i] != 0) zeroed = false;
    }
    check(zeroed && std::memcmp(tpl + lg::kVecOff[0] + 8, cs + lg::kVecOff[0] + 8, 8) == 0 && std::memcmp(tpl + lg::kVecOff[2] + 8, cs + lg::kVecOff[2] + 8, 8) == 0,
          "the made template is all zero except each vector's own second header word");
    check(lg::statePristine(tpl), "and it is an empty state: no key, no data, no GPU object");
    char text[200];
    lg::describeState(cs, 0, text, sizeof(text));
    check(contains(text, "first-call flag 0, key set, vector sizes") && contains(text, "vector data set/set/set") && contains(text, "4 GPU objects"), "the first-seen description names what was found");
    // four such contexts at once, as in the flight
    Rig q;
    Ctx four[4];
    Knobs kk;
    for (Ctx& x : four) {
        fakeBuild(q.h, kk, x.p(), mk(0, 0));
        fakeBuild(q.h, kk, x.p(), mk(0, 1));
    }
    for (int i = 0; i < 40; ++i) {
        for (int ci = 0; ci < 4; ++ci) q.call(four[ci], mk(ci & 1, i % 4));
    }
    check(q.cache.contexts() == 4 && q.cache.stats.byReason[lg::kNoTemplate] == 0 && q.cache.stats.hits > 100, "four already-built contexts are all cached");
    check(auditOwnership(q, {&four[0], &four[1], &four[2], &four[3]}), "and share no pointer");
}

void testSelfCheckDiagnosis() {
    // The line that turns the cache off names the fields, what was derived and what the game stored.
    uint8_t a[lg::kKeyBytes], b[lg::kKeyBytes];
    lg::buildKey(mk(0, 1), a);
    std::memcpy(b, a, sizeof(a));
    char line[700];
    check(lg::describeKeyDiff(a, b, line, sizeof(line)) == 0 && line[0] == '\0', "equal keys describe nothing");
    float f = 2.5f;
    std::memcpy(b + 4 * 8, &f, 4);     // row 2 column 0
    std::memcpy(b + 0x44, &f, 4);      // far
    st64(b + 0x70, 64);                // tile
    lg::describeKeyDiff(a, b, line, sizeof(line));
    float row20;
    std::memcpy(&row20, a + 32, 4);
    check(contains(line, "row 2.0 derived 0 (0x00000000) stored 2.5 (0x40200000)") && contains(line, "far derived 10000 (0x461c4000) stored 2.5 (0x40200000)") &&
              contains(line, "tile derived 16 stored 64") && !contains(line, "near") && !contains(line, "row 0."),
          "the description names row.column, near or far, and the quadword, with both values");
    check(row20 == 0.0f, "(the derived row 2 column 0 of the test inputs is 0)");
    // more than eight differing fields: eight are written, the rest counted, and it fits any cap
    uint8_t c[lg::kKeyBytes];
    std::memset(c, 0x7F, sizeof(c));
    lg::describeKeyDiff(a, c, line, sizeof(line));
    check(contains(line, " more") && std::strlen(line) < sizeof(line), "past eight fields the rest are counted");
    char tiny[40];
    lg::describeKeyDiff(a, c, tiny, sizeof(tiny));
    check(std::strlen(tiny) < sizeof(tiny), "and the text never passes its cap");

    // A real mismatch (the game stores a key the inputs do not give, and a second derivation agrees with the first) turns the cache off with those words.
    {
        Rig r;
        Ctx c1;
        r.call(c1, mk(0, 0));
        r.k.wrongKey = true;
        const lg::Result res = r.call(c1, mk(0, 1));
        check(res.disableNow && r.cache.disabled && r.cache.stats.selfFail == 1 && r.cache.stats.selfRace == 0, "a mismatch the view block did not cause turns the cache off");
        check(contains(r.cache.disabledWhy, "row 2.0 derived") && contains(r.cache.disabledWhy, "stored") && contains(r.cache.disabledWhy, "did not move") &&
                  contains(r.cache.disabledWhy, "vectors hold 256/4/256 elements") && contains(r.cache.disabledWhy, "override path 0"),
              "and the reason names the field, both values, the second derivation and the vector sizes");
    }
    // The view block moved under the call: the game built from a different row. Counted apart, the cache goes on, and nothing wrong is parked as matchable.
    {
        Rig r;
        Ctx c1;
        r.call(c1, mk(0, 0));
        r.call(c1, mk(0, 1));
        r.moveNext = true;
        const lg::Result res = r.call(c1, mk(0, 2));
        check(!res.disableNow && !r.cache.disabled && r.cache.stats.selfRace == 1 && r.cache.stats.selfFail == 0, "a key that differs because the view block moved counts apart and does not disable");
        for (int round = 0; round < 6; ++round) {
            for (int ph = 0; ph < 4; ++ph) r.call(c1, mk(0, ph));
        }
        check(!r.cache.disabled && r.cache.stats.hits > 0 && coherent(c1) && auditOwnership(r, {&c1}), "and the cache goes on serving, coherent and singly owned");
        Oracle o;
        o.build(mk(0, 3));
        r.call(c1, mk(0, 3));
        check(sameTables(c1.mem, o.c.mem), "with exact states");
    }
}

void testNoTemplate() {
    // a context met as constructed with the first-call flag clear is cached from its first call
    Rig q;
    Ctx p;
    p.mem[lg::kCtxFirstFlag] = 0;
    for (int i = 0; i < 12; ++i) q.call(p, mk(0, i % 3));
    check(q.cache.stats.byReason[lg::kNoTemplate] == 0 && q.cache.group(0).haveTemplate && q.cache.stats.hits > 0, "a pristine context with the flag clear is cached from its first call");
    check(auditOwnership(q, {&p}) && coherent(p), "and its states stay coherent and singly owned");
}

void testVerifyDisables() {
    {
        Rig r;
        Ctx c;
        r.k.impure = true;
        for (int round = 0; round < 3 && !r.cache.disabled; ++round) {
            for (int ph = 0; ph < 4 && !r.cache.disabled; ++ph) {
                r.k.hidden = round * 10 + ph;   // the tables depend on something outside the key
                r.call(c, mk(0, ph));
            }
        }
        check(r.cache.disabled, "a cached state that differs from a fresh build turns the cache off");
        check(r.cache.stats.verifyMismatches == 1 && r.cache.stats.verifyChecks == 1, "one verification, one mismatch");
        check(contains(r.cache.disabledWhy, "verification") && contains(r.cache.disabledWhy, "table"), "the reason names the verification and the table");
        check(coherent(c), "the context holds the fresh build when the mismatch is found");
    }
    {
        Rig r;
        Ctx c;
        r.k.paddingNoise = true;
        for (int round = 0; round < 4 && !r.cache.disabled; ++round) {
            for (int ph = 0; ph < 4; ++ph) {
                r.k.hidden = round * 10 + ph;
                r.call(c, mk(0, ph));
            }
        }
        check(!r.cache.disabled && r.cache.stats.verifyChecks >= 1 && r.cache.stats.verifyPadding >= 1, "a difference only in the padding lanes is counted apart and does not disable");
    }
    {
        Rig r;
        Ctx c;
        r.call(c, mk(0, 0));
        r.k.wrongKey = true;
        const lg::Result res = r.call(c, mk(0, 1));
        check(r.cache.disabled && res.disableNow && r.cache.stats.selfFail == 1 && contains(r.cache.disabledWhy, "key built from the call's inputs"),
              "a stored key that is not the one built from the inputs turns the cache off");
    }
    {
        Rig r;
        Ctx c;
        // direct compare: size difference and key difference
        Oracle a, b;
        a.build(mk(0, 0));
        b.build(mk(0, 1));
        uint8_t sa[lg::kStateBytes], sb[lg::kStateBytes];
        lg::gather<lg::PlainMem>(a.c.p(), sa);
        lg::gather<lg::PlainMem>(b.c.p(), sb);
        lg::Compare cmp = lg::compareStates<lg::PlainMem>(sa, sb);
        check(cmp.verdict == lg::Same::kDiffers && cmp.table == -2, "different keys differ at the key");
        Oracle c1, c2;
        c1.build(mk(0, 0));
        c2.build(mk(0, 0));
        lg::gather<lg::PlainMem>(c1.c.p(), sa);
        lg::gather<lg::PlainMem>(c2.c.p(), sb);
        check(lg::compareStates<lg::PlainMem>(sa, sb).verdict == lg::Same::kEqual, "two builds of the same inputs compare equal");
        const uint64_t data = ld64(sb + lg::kVecOff[2] + 0x18);
        reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(data))[5] ^= 1;
        cmp = lg::compareStates<lg::PlainMem>(sa, sb);
        check(cmp.verdict == lg::Same::kDiffers && cmp.table == 2 && cmp.offset == 5, "one byte of the third table is a difference at its offset");
        st64(sb + lg::kVecOff[1] + 0x10, 1);
        cmp = lg::compareStates<lg::PlainMem>(sa, sb);
        check(cmp.verdict == lg::Same::kDiffers && cmp.table == 1, "a different size is a difference");
    }
}

void testFaults() {
    {
        // a read that faults while the state is gathered: the call goes to the game and nothing is swapped
        Heap h;
        Knobs k;
        lg::Cache cache;
        Ctx c;
        Clock clock;
        FailReads::failAt = 1;
        FailReads::count = 0;
        const lg::CallInputs in = mk(0, 0);
        const lg::Result res = lg::serve<FailReads>(cache, c.p(), in, [&] { fakeBuild(h, k, c.p(), in); }, clock);
        check(res.served == lg::Served::kGame && cache.stats.byReason[lg::kUnreadable] == 1 && k.calls == 1, "an unreadable context is forwarded whole");
        FailReads::failAt = 0;
    }
    {
        Rig r;
        Ctx c;
        for (int i = 0; i < 6; ++i) r.call(c, mk(0, i % 3));   // fill, so a hit is possible
        check(r.cache.stats.hits > 0 || r.cache.stats.verifyChecks > 0, "a hit or a verification happened");
        // now fail the write of the third region during the next hit
        FailWrites::count = 0;
        FailWrites::failAt = 3;
        FailWrites::always = false;
        Heap& h = r.h;
        Knobs& k = r.k;
        Clock clock;
        // the last call left phase 2 in the context, so phase 1 is a kept state and hitsSeen % kVerifyEvery != 0 takes the plain hit path
        r.cache.hitsSeen = 1;
        lg::Result res;
        const lg::CallInputs in = mk(0, 1);
        res = lg::serve<FailWrites>(r.cache, c.p(), in, [&] { fakeBuild(h, k, c.p(), in); }, clock);
        check(res.disableNow && r.cache.disabled && contains(r.cache.disabledWhy, "faulted during a swap"), "a write that faults part-way disables the cache and says so");
        check(auditOwnership(r, {&c}), "the half-written swap was undone: every pointer is held once and live");
    }
    {
        Rig r;
        Ctx c;
        for (int i = 0; i < 6; ++i) r.call(c, mk(0, i % 3));
        FailWrites::count = 0;
        FailWrites::failAt = 1;
        FailWrites::always = true;
        Clock clock;
        r.cache.hitsSeen = 1;
        const lg::CallInputs in = mk(0, 1);
        Heap& h = r.h;
        Knobs& k = r.k;
        const lg::Result res = lg::serve<FailWrites>(r.cache, c.p(), in, [&] { fakeBuild(h, k, c.p(), in); }, clock);
        check(res.disableNow && contains(r.cache.disabledWhy, "could not be undone"), "when the undo faults too, the reason says so");
    }
    {
        // the same part-way fault on a MISS (the swap that keeps the context's old state): undone, disabled, and the call still reaches the game
        Rig r;
        Ctx c;
        for (int i = 0; i < 6; ++i) r.call(c, mk(0, i % 3));
        FailWrites::count = 0;
        FailWrites::failAt = 3;
        FailWrites::always = false;
        Clock clock;
        const lg::CallInputs in = mk(0, 7);   // a key never seen
        Heap& h = r.h;
        Knobs& k = r.k;
        const uint64_t calls = k.calls;
        const lg::Result res = lg::serve<FailWrites>(r.cache, c.p(), in, [&] { fakeBuild(h, k, c.p(), in); }, clock);
        check(res.disableNow && contains(r.cache.disabledWhy, "faulted during a swap") && k.calls == calls + 1, "a miss whose swap faults part-way disables the cache and still reaches the game");
        check(auditOwnership(r, {&c}) && coherent(c), "and the half-written swap was undone before the game built over the context");
    }
}

void testThreadRule() {
    std::atomic<uint32_t> owner{0};
    check(lg::threadMayServe(owner, 17), "the first thread to call owns the cache");
    check(lg::threadMayServe(owner, 17), "and keeps it");
    check(!lg::threadMayServe(owner, 18), "another thread's call is denied");
    check(owner.load() == 17, "a denied call does not take the cache over");
}

void testJitterProbe() {
    lg::Cache c;
    const int g = c.groupFor(0x10);
    uint8_t base[lg::kKeyBytes], near1[lg::kKeyBytes], near2[lg::kKeyBytes], other[lg::kKeyBytes], different[lg::kKeyBytes];
    lg::buildKey(mk(0, 0), base);
    lg::buildKey(mk(0, 1), near1);
    lg::buildKey(mk(0, 5), near2);
    lg::buildKey(mk(0, 3, 1), different);   // another size
    plant(c, g, near1);
    plant(c, g, near2);
    plant(c, g, different);
    lg::JitterSnap snap;
    snap.known = true;
    snap.frame = 99;
    snap.jx = 0.25f;
    snap.jy = -0.125f;
    snap.shift[0][0] = 0.001f;
    lg::probeNeighbour(c, g, base, snap);
    const lg::Stats& s = c.stats;
    check(s.jNeighbour == 1 && s.jOnly02 == 1 && s.jOther == 0, "a miss whose neighbour differs only in rows 0 and 2 is counted so");
    check(s.nSamples == 1 && s.samples[0].rows13Equal, "and a sample is kept");
    // the nearest is phase 1 (rows differ by 0.001 and 0.003), not phase 5
    check(std::fabs(s.samples[0].delta[3] - 0.001f) < 1e-6f && std::fabs(s.samples[0].delta[11] - 0.003f) < 1e-6f, "the nearest key's deltas are reported, elements 0.3 and 2.3");
    check(s.jDiffers[3] == 1 && s.jDiffers[11] == 1, "only the elements that differ are counted");
    int others = 0;
    for (int e = 0; e < 16; ++e) others += (e != 3 && e != 11 && s.jDiffers[e] != 0) ? 1 : 0;
    check(others == 0, "no other element is counted");
    check(s.samples[0].jitter.frame == 99 && s.samples[0].jitter.jx == 0.25f, "the sample carries EDVR's jitter");
    // rows 1 or 3 differing too is its own count
    lg::buildKey(mk(0, 0), other);
    float r1;
    std::memcpy(&r1, other + 20, 4);
    r1 += 0.5f;
    std::memcpy(other + 20, &r1, 4);
    lg::Cache c2;
    const int g2 = c2.groupFor(0x10);
    plant(c2, g2, near1);
    lg::probeNeighbour(c2, g2, other, snap);
    check(c2.stats.jOther == 1 && c2.stats.jOnly02 == 0 && !c2.stats.samples[0].rows13Equal, "a neighbour that differs in row 1 is counted apart");
    // a key of another size has no neighbour
    lg::Cache c3;
    const int g3 = c3.groupFor(0x10);
    plant(c3, g3, different);
    lg::probeNeighbour(c3, g3, base, snap);
    check(c3.stats.jNeighbour == 0, "a slot of another size is not a neighbour");
    // an equal key is not a neighbour either
    lg::probeNeighbour(c3, g3, different, snap);
    check(c3.stats.jNeighbour == 0, "an equal key is not a neighbour");
    // samples are capped
    for (int i = 0; i < 10; ++i) lg::probeNeighbour(c, g, base, snap);
    check(c.stats.nSamples == lg::kSamples, "at most four samples are kept a window");
}

void testLines() {
    lg::Stats s;
    s.calls = 4400;
    s.pathOverride = 4400;
    s.derived = 4400;
    s.deriveMs = 4400 * 0.0005;
    s.gameMemo = 12;
    s.hits = 4000;
    s.misses = 60;
    s.forwarded = 28;
    s.byReason[lg::kFirst] = 2;
    s.byReason[lg::kDirtyView] = 26;
    s.missMs = 60 * 0.29;
    s.hitMs = 4000 * 0.0002;
    s.missOverheadMs = 60 * 0.001;
    s.verifyChecks = 67;
    s.verifyPadding = 0;
    s.selfOk = 60;
    s.capacityMisses = 3;
    char line[2400];
    lg::formatMainLine(line, sizeof(line), s, 30, 2000, 0.285, 17, 1);
    const std::string main(line);
    check(main == "light grid cache: last 30 s: 4400 calls in 2000 frames (2.20 a frame); rows by the override path 4400, by the plain path 0; the game's own memo 12, cache hits 4000 (90.9% of calls), misses 60 (0.290 ms each); "
                  "rows derived ahead 4400 times (0.0005 ms each), dirty view refreshed ahead 0 times (0.0000 ms each); forwarded whole 28 (first call 2, dirty view 26, rows not derivable 0, forced rebuild 0, unbuildable 0, no template 0, renderer change 0, other thread 0, "
                  "contexts full 0, unreadable 0, swap fault 0); estimated saving 1140.00 ms (0.570 ms a frame, from 0.285 ms a miss); swap cost 0.0002 ms a hit, 0.0010 ms a miss; "
                  "verify 67 checks, 0 padding-only, 0 mismatches; key self-check 60 ok, 0 failed, 0 with the view block changing under the call; misses on a key evicted lately 3; 17 slots in use, 1 contexts.",
          "the 30 s line has exactly this text");
    if (main != "light grid cache: last 30 s: 4400 calls in 2000 frames (2.20 a frame); rows by the override path 4400, by the plain path 0; the game's own memo 12, cache hits 4000 (90.9% of calls), misses 60 (0.290 ms each); "
                "rows derived ahead 4400 times (0.0005 ms each), dirty view refreshed ahead 0 times (0.0000 ms each); forwarded whole 28 (first call 2, dirty view 26, rows not derivable 0, forced rebuild 0, unbuildable 0, no template 0, renderer change 0, other thread 0, "
                "contexts full 0, unreadable 0, swap fault 0); estimated saving 1140.00 ms (0.570 ms a frame, from 0.285 ms a miss); swap cost 0.0002 ms a hit, 0.0010 ms a miss; "
                "verify 67 checks, 0 padding-only, 0 mismatches; key self-check 60 ok, 0 failed, 0 with the view block changing under the call; misses on a key evicted lately 3; 17 slots in use, 1 contexts.") {
        std::printf("  got: %s\n", main.c_str());
    }
    s.jNeighbour = 40;
    s.jOnly02 = 38;
    s.jOther = 2;
    s.jDiffers[3] = 40;
    s.jMax[3] = 0.0012;
    s.jDiffers[11] = 40;
    s.jMax[11] = 0.003;
    lg::formatJitterLine(line, sizeof(line), s, 30);
    const std::string jit(line);
    check(jit == "light grid cache: jitter probe last 30 s: 40 misses had a neighbour of the same size and depth range; 38 differ only in rows 0 and 2, 2 also in row 1 or 3. "
                 "Elements that differ (row.column count/largest |delta|): 0.3 40/0.0012; 2.3 40/0.003;",
          "the jitter line has exactly this text");
    if (jit != "light grid cache: jitter probe last 30 s: 40 misses had a neighbour of the same size and depth range; 38 differ only in rows 0 and 2, 2 also in row 1 or 3. "
               "Elements that differ (row.column count/largest |delta|): 0.3 40/0.0012; 2.3 40/0.003;") {
        std::printf("  got: %s\n", jit.c_str());
    }
    lg::Stats none;
    lg::formatJitterLine(line, sizeof(line), none, 30);
    check(contains(line, "none."), "a window with no differing element says so");
    lg::Sample m;
    m.delta[3] = 0.001f;
    m.delta[11] = 0.003f;
    m.jitter.known = true;
    m.jitter.frame = 12;
    m.jitter.jx = 0.25f;
    m.jitter.jy = -0.125f;
    m.jitter.shift[0][0] = 0.0005f;
    lg::formatSampleLine(line, sizeof(line), m);
    check(std::string(line) == "light grid cache: jitter sample: delta row 0 [0 0 0 0.001] row 2 [0 0 0 0.003]; rows 1 and 3 equal; EDVR jitter published: frame 12, pixels (0.2500, -0.1250), "
                               "tangent shift eye 0 (0.0005, 0) eye 1 (0, 0).",
          "the sample line has exactly this text");
    m.jitter.known = false;
    lg::formatSampleLine(line, sizeof(line), m);
    check(contains(line, "not published"), "a sample with no published jitter says so");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--self-test") != 0) {
        std::printf("usage: light_grid_cache_test --self-test\n");
        return 2;
    }
    testIdentityAndPin();
    testGridCeil();
    testKey();
    testRegions();
    testKeyCompareAndLru();
    testSteadyHits();
    testCapacityMissAndThrash();
    testTwoContexts();
    testForwardReasons();
    testExtraNear();
    testAlreadyBuilt();
    testSelfCheckDiagnosis();
    testNoTemplate();
    testVerifyDisables();
    testFaults();
    testThreadRule();
    testJitterProbe();
    testLines();
    std::printf("light_grid_cache_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
