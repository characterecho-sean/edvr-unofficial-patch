#pragma once
// The pure logic of ui_depth's depth-surface slot cache, VR draw-path test build 1, item 4 (docs\vr-draw-path-cpu-2026-11-01.md). No Windows and no D3D here, so
// tools\ui_depth_test drives every decision without a device.
//
// uiDepthSampledSurfaceSlot asks, for each qualifying eye draw, which of PS SRV 0..3 holds a sampled depth surface (viewIsSurface on up to four views). The answer
// depends on the bindings (bindingEyeKeyGeneration moves with PS SRV 0..3, binding_shadow.h), the classifier's epoch (which bumps on every surface-table write, on
// configure and at every frame boundary), UI depth on, UI depth stood down, and the key. The cache remembers the answer while all five are unchanged.
//
// experimental.vr_cpu_depth_slot_cache is TEMPORARY. Off recomputes every call and counts nothing; a change of the key since the last call clears the cache, so a
// flip off and on never serves an answer from before the flip. The counts are read and reset once per perf window (takeCounts).
#include <cstdint>

namespace edvr {
namespace depthslot {

struct Inputs {
    uint32_t generation = 0;   // bindingEyeKeyGeneration()
    uint32_t epoch = 0;        // g_classifierEpoch
    bool     on = false;       // g_uiDepthOn
    bool     stoodDown = false;   // g_uiDepthStoodDown
    bool     key = false;      // experimental.vr_cpu_depth_slot_cache
};

struct Cache {
    Inputs   seen;
    int      slot = -1;
    bool     stored = false;
    bool     keyKnown = false;   // a call has run, so lastKey means something
    bool     lastKey = false;
    uint32_t hits = 0;
    uint32_t misses = 0;
};

inline bool same(const Inputs& a, const Inputs& b) {
    return a.generation == b.generation && a.epoch == b.epoch && a.on == b.on && a.stoodDown == b.stoodDown;
}

// compute() is the uncached lookup. Returns its answer, or the remembered one on a hit.
template <class F>
inline int lookup(Cache& c, const Inputs& in, F compute) {
    if (c.keyKnown && c.lastKey != in.key) c.stored = false;   // a key change clears the cache
    c.keyKnown = true;
    c.lastKey = in.key;
    if (!in.key) return compute();
    if (c.stored && same(c.seen, in)) { ++c.hits; return c.slot; }
    ++c.misses;
    c.slot = compute();
    c.seen = in;
    c.stored = true;
    return c.slot;
}

inline void takeCounts(Cache& c, uint32_t* hits, uint32_t* misses) {
    *hits = c.hits; *misses = c.misses;
    c.hits = c.misses = 0;
}

}  // namespace depthslot
}  // namespace edvr
