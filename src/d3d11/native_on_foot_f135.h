// F135 (native on-foot VR): the on-foot menu probe, MEASURE ONLY. Header only, no game or D3D11 state, so native_frame_test runs the very functions the hooks and the frame thread
// use. Journal: the arc doc, 2026-10-27, F135.
//
// Look to aim moves the on-foot Esc (pause) menu with the head. F131 holds look to aim on one signal, the placement count (g_f131PanelSeq), and in the flight
// edvr_gfx_20261027_091608 it fired only at 09:24:06.4, apparently once the mouse was over the menu, about 11 s after the menu was up; the game's HUD hide counters (F116) did
// not move. This probe counts the calls of every hook EDVR already has that can see a panel or a menu, per frame, and watches the Escape key (and the pad's Start and Back) as ground
// truth for the flight only (never a signal), to find the hook, or the field, that is active exactly while a menu or an on-foot panel is up, mouse still or not.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace edvr {
namespace nofoot {
namespace f135 {

enum Counter : int {
    kUpdate = 0,       // the pointer controller's update (F111, +0x454F520)
    kPlace,            // the controller's placement on one panel (+0x4559C50)
    kRay,              // the panel's ray against its triangles (+0x452B6A0)
    kWindow,           // the panel's cursor window (+0x4541A20)
    kOwner,            // the pick mesh owner's transform (F114b)
    kPanelSubmit,      // P1: the panel submit (+0x19B14E0)
    kInstSubmit,       // P3: the instance submit
    kBridgeUpdate,     // the menu bridge's update scopes opened (an armed capture only)
    kBridgePlace,      // the menu input's placement scopes opened (an armed capture only)
    kCount
};
inline const char* counterName(int c) noexcept {
    static const char* const names[kCount] = {"update", "placement", "ray", "cursor window", "owner transform", "panel submit", "inst submit", "bridge update", "bridge placement"};
    return c >= 0 && c < kCount ? names[c] : "?";
}
// Counters that decide whether a quiet second prints: the ones a menu or a panel could move (the update and the instance submit run every frame, panel or not).
inline bool gatesLine(int c) noexcept { return c != kUpdate && c != kInstSubmit; }

// The hooks' side: relaxed atomics, one fetch_add a call. The frame thread takes them to zero.
struct Counters {
    std::atomic<uint32_t> n[kCount];
    Counters() noexcept {
        for (auto& c : n) c.store(0, std::memory_order_relaxed);
    }
    void add(int c) noexcept {
        if (c >= 0 && c < kCount) n[c].fetch_add(1, std::memory_order_relaxed);
    }
    void take(uint32_t out[kCount]) noexcept {
        for (int i = 0; i < kCount; ++i) out[i] = n[i].exchange(0, std::memory_order_relaxed);
    }
};

// The distinct objects a hook saw in the second (up to 8): the object's pointer (low 32 bits) and a vtable as an exe RVA. Lock free: a slot holds 1 << 63 | rva << 32 | ptr.
struct Obj {
    uint32_t ptr = 0, rva = 0;
};
struct ObjSet {
    static constexpr int kMax = 8;
    std::atomic<uint64_t> slot[kMax];
    std::atomic<uint32_t> overflow{0};
    ObjSet() noexcept {
        for (auto& s : slot) s.store(0, std::memory_order_relaxed);
    }
    void add(uint32_t ptr, uint32_t rva) noexcept {
        const uint64_t key = (1ull << 63) | (static_cast<uint64_t>(rva) << 32) | ptr;
        for (int i = 0; i < kMax; ++i) {
            uint64_t cur = slot[i].load(std::memory_order_relaxed);
            if (cur == key) return;
            if (cur == 0 && slot[i].compare_exchange_strong(cur, key, std::memory_order_relaxed)) return;
            if (cur == key) return;   // another thread put the same key
        }
        overflow.fetch_add(1, std::memory_order_relaxed);
    }
    // The distinct objects seen since the last take, cleared; *more is the count that did not fit.
    int take(Obj out[kMax], uint32_t* more) noexcept {
        int n = 0;
        for (int i = 0; i < kMax; ++i) {
            const uint64_t v = slot[i].exchange(0, std::memory_order_relaxed);
            if (v) {
                out[n].ptr = static_cast<uint32_t>(v & 0xFFFFFFFFull);
                out[n].rva = static_cast<uint32_t>((v >> 32) & 0x7FFFFFFFull);
                ++n;
            }
        }
        *more = overflow.exchange(0, std::memory_order_relaxed);
        return n;
    }
};
// "0x1F40A010/+0x4DC1B50 0x..." (the pointer's low 32 bits, then the vtable's RVA); empty for none.
inline int formatObjs(char* out, size_t cap, const Obj* o, int n, uint32_t more) noexcept {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    size_t used = 0;
    for (int i = 0; i < n; ++i) {
        const int w = std::snprintf(out + used, cap - used, "%s0x%08X/+0x%X", i ? " " : "", o[i].ptr, o[i].rva);
        if (w < 0 || static_cast<size_t>(w) >= cap - used) break;
        used += static_cast<size_t>(w);
    }
    if (more) {
        const int w = std::snprintf(out + used, cap - used, "%s+%u more", used ? " " : "", more);
        if (w > 0 && static_cast<size_t>(w) < cap - used) used += static_cast<size_t>(w);
    }
    return static_cast<int>(used);
}

// ---- the edges ---------------------------------------------------------------------------------------------------------------------------------
// A counter that goes from 0 to non-zero is a rise; one that stays 0 for kQuietMs after being non-zero is a fall. Frame thread, per frame with the frame's counts.
constexpr uint64_t kQuietMs = 500;
enum EdgeKind : uint8_t { kNoEdge = 0, kRose = 1, kFell = 2 };
struct Edges {
    bool active[kCount] = {};
    uint64_t lastMs[kCount] = {};
    EdgeKind step(int c, uint64_t nowMs, uint32_t count) noexcept {
        if (c < 0 || c >= kCount) return kNoEdge;
        if (count > 0) {
            lastMs[c] = nowMs ? nowMs : 1;
            if (!active[c]) {
                active[c] = true;
                return kRose;
            }
            return kNoEdge;
        }
        if (active[c] && nowMs >= lastMs[c] && nowMs - lastMs[c] >= kQuietMs) {
            active[c] = false;
            return kFell;
        }
        return kNoEdge;
    }
    void reset() noexcept { *this = Edges{}; }
};

// A key's or a button's down and up edges (the ground truth).
struct KeyEdge {
    bool down = false, known = false;
    // +1 down, -1 up, 0 none. The first sample is the baseline (no edge).
    int step(bool nowDown) noexcept {
        if (!known) {
            known = true;
            down = nowDown;
            return 0;
        }
        if (nowDown == down) return 0;
        down = nowDown;
        return nowDown ? +1 : -1;
    }
};

// The event lines' budget: at most `limit` in any 60 s window, the rest counted and said in the next.
struct Budget {
    uint64_t windowMs = 0;
    uint32_t used = 0, suppressed = 0;
    bool allow(uint64_t nowMs, uint32_t limit) noexcept {
        if (windowMs == 0 || nowMs < windowMs || nowMs - windowMs >= 60000u) {
            windowMs = nowMs ? nowMs : 1;
            used = 0;
        }
        if (used >= limit) {
            ++suppressed;
            return false;
        }
        ++used;
        return true;
    }
    uint32_t takeSuppressed() noexcept {
        const uint32_t s = suppressed;
        suppressed = 0;
        return s;
    }
};

// Whether a second is worth a line: a gating counter non-zero in it, or a key edge within kKeyWindowMs. The window is longer than the brief's 2 s so the seconds of a menu left
// with the mouse still (the interval the probe exists for) are printed too.
constexpr uint64_t kKeyWindowMs = 8000;
inline bool lineWanted(const uint32_t counts[kCount], uint64_t nowMs, uint64_t lastKeyEdgeMs) noexcept {
    for (int c = 0; c < kCount; ++c)
        if (gatesLine(c) && counts[c]) return true;
    return lastKeyEdgeMs != 0 && nowMs >= lastKeyEdgeMs && nowMs - lastKeyEdgeMs <= kKeyWindowMs;
}

}  // namespace f135
}  // namespace nofoot
}  // namespace edvr
