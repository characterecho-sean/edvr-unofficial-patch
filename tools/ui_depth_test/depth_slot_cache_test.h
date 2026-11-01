// VR draw-path test build 1, item 4: the depth-surface slot cache (src/d3d11/depth_slot_cache.h), the CPU half. No device. Included by ui_depth_test.cpp.
//   * a repeat call is a hit, and the cached answer is the one computed;
//   * each of the four inputs (eye-key generation, classifier epoch, UI depth on, UI depth stood down) changed alone forces a miss;
//   * key off recomputes every call and counts nothing;
//   * a key flip clears the cache, so off then on never serves an answer from before the flip;
//   * takeCounts reads and resets;
//   * the production site: uiDepthSampledSurfaceSlot goes through depthslot::lookup keyed on bindingEyeKeyGeneration and g_classifierEpoch, and the setter and counts exist.
#pragma once
#include "../../src/d3d11/depth_slot_cache.h"
#include <fstream>
#include <iterator>
#include <string>

namespace dsc {

inline bool cpuHalf() {
    using namespace edvr::depthslot;
    bool ok = true;
    auto fail = [&](const char* what) { std::printf("depth slot cache CPU half: %s\n", what); ok = false; };
    int calls = 0, answer = 2;
    auto compute = [&] { ++calls; return answer; };
    Inputs in; in.generation = 5; in.epoch = 9; in.on = true; in.stoodDown = false; in.key = true;
    Cache c;
    if (lookup(c, in, compute) != 2 || calls != 1 || c.misses != 1 || c.hits != 0) fail("the first call is not a miss that computes");
    answer = 3;   // a hit must return the remembered answer, not a fresh one
    if (lookup(c, in, compute) != 2 || calls != 1 || c.hits != 1) fail("a repeat call is not a hit on the remembered answer");
    struct Change { const char* name; void (*apply)(Inputs&); };
    const Change changes[] = {
        {"generation", [](Inputs& i) { ++i.generation; }},
        {"epoch", [](Inputs& i) { ++i.epoch; }},
        {"on", [](Inputs& i) { i.on = !i.on; }},
        {"stoodDown", [](Inputs& i) { i.stoodDown = !i.stoodDown; }},
    };
    for (const Change& ch : changes) {
        Cache d; Inputs base = in; int n = 0;
        auto cmp = [&] { return ++n; };
        lookup(d, base, cmp);
        lookup(d, base, cmp);   // hit
        Inputs moved = base; ch.apply(moved);
        const int before = n;
        lookup(d, moved, cmp);
        if (n != before + 1 || d.misses != 2) { std::printf("depth slot cache CPU half: %s changed alone did not force a miss\n", ch.name); ok = false; }
        lookup(d, moved, cmp);
        if (n != before + 1 || d.hits != 2) { std::printf("depth slot cache CPU half: %s: the call after the miss is not a hit\n", ch.name); ok = false; }
    }
    {   // key off: every call computes, nothing is counted
        Cache d; Inputs off = in; off.key = false; int n = 0;
        auto cmp = [&] { return ++n; };
        lookup(d, off, cmp); lookup(d, off, cmp); lookup(d, off, cmp);
        if (n != 3 || d.hits != 0 || d.misses != 0) fail("key off did not recompute every call without counting");
    }
    {   // a key flip clears: on (stored) -> off -> on must miss though nothing else moved
        Cache d; int n = 0;
        auto cmp = [&] { return ++n; };
        lookup(d, in, cmp);
        Inputs off = in; off.key = false;
        lookup(d, off, cmp);
        const int before = n;
        if (lookup(d, in, cmp) != before + 1 || d.misses != 2) fail("a key flip did not clear the cache");
    }
    {
        uint32_t h = 99, m = 99;
        takeCounts(c, &h, &m);
        if (h != 1 || m != 1 || c.hits != 0 || c.misses != 0) fail("takeCounts did not return and reset the counts");
    }
    // The production site, as text (ui_depth_test does not link vscreen, so the key arrives through uiDepthSetSlotCache).
    std::ifstream f("src/d3d11/ui_depth.cpp", std::ios::binary);
    if (!f) f.open("../../src/d3d11/ui_depth.cpp", std::ios::binary);
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t at = src.find("\nint uiDepthSampledSurfaceSlot() {");
    const size_t end = at == std::string::npos ? at : src.find("\n}\n", at);
    if (at == std::string::npos || end == std::string::npos) fail("uiDepthSampledSurfaceSlot not found in ui_depth.cpp (run from the repo root)");
    else {
        const std::string body = src.substr(at, end - at);
        if (body.find("depthslot::lookup") == std::string::npos || body.find("bindingEyeKeyGeneration()") == std::string::npos ||
            body.find("g_classifierEpoch") == std::string::npos || body.find("g_slotCacheKey") == std::string::npos)
            fail("uiDepthSampledSurfaceSlot does not go through depthslot::lookup on the eye key, the epoch and the key");
    }
    edvr::uiDepthSetSlotCache(true);   // exists and links
    uint32_t h = 0, m = 0;
    edvr::uiDepthSlotCacheTakeCounts(&h, &m);
    if (h || m) fail("a fresh production cache has counts");
    return ok;
}

}  // namespace dsc
