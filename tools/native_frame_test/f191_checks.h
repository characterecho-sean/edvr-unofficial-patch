// F191 checks (native on-foot VR), run191 below. Header only, run by native_frame_test.
//
// F191 (native_on_foot_f191.h, native_on_foot.cpp): the hip reticle is found by its marker type (1001), not by pool slot 10. The pilot's rig has had another marker owner on
// slot 10 since 2026-10-27 10:36, the reticle on 11 (then 12, 13, 14), and every key-10 test missed it. The rig runs the tracker on the sequences the logs show: the fallback
// before any type 1001 call, the switch to 11, the overlap of two live keys, the previous key expiring, an owner of slot 10 that never passes type 1001, the clock's wrap,
// a reset, and the writer and reader threads on one tracker; then the lines, and the source sites (no slot-10 test is left; the hook feeds the tracker; the sites read it).
#pragma once

#include "../../src/d3d11/native_on_foot_f87.h"
#include "../../src/d3d11/native_on_foot_f191.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace f191test {

namespace f87 = edvr::nofoot::f87;
namespace f191 = edvr::nofoot::f191;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
inline size_t countOf(const std::string& hay, const std::string& needle) {
    size_t n = 0, at = 0;
    while ((at = hay.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
    return n;
}

template <class Check>
inline void run191(Check&& check) {
    char buf[200];

    // ---- 1. the fallback: before any type 1001 call the reticle is key 10, as the code assumed ---------------------------------------------------------------------------------------
    {
        f191::ReticleKeys k;
        bool others = false;
        for (uint32_t key : {0u, 1u, 9u, 11u, 12u, 43u, 45u, 0x70u, 0x8Au, 0xFFFFu, 0xFFFFFFFFu}) others = others || k.isReticle(key, 1000u);
        check(k.isReticle(10u, 1000u) && !others && !k.seen() && k.newest() == 10u,
              "F191: before any type 1001 call the reticle is key 10 and only key 10 (the fallback, as before F191; no other key takes the hit depth or the centring)");
        k.describe(buf, sizeof(buf));
        check(std::strstr(buf, "10") != nullptr && std::strstr(buf, "assumed") != nullptr && std::strstr(buf, "1001") != nullptr,
              "F191: the fallback says it is assumed (\"10 (assumed: no type 1001 call seen yet)\"), never that type 1001 named it");
    }

    // ---- 2. the switch to 11: the pilot's rig --------------------------------------------------------------------------------------------------------------------------------------
    {
        f191::ReticleKeys k;
        const f191::Observed first = k.observe(11u, 5000u);
        check(first.changed && first.previous == f191::kNoKey, "F191: the first type 1001 call (key 11) is a change, from no key held");
        check(k.seen() && k.newest() == 11u && k.isReticle(11u, 5000u), "F191: key 11 is the reticle once type 1001 has passed it");
        check(!k.isReticle(10u, 5000u) && !k.isReticle(10u, 5000u + 60000u),
              "F191: key 10 is NOT the reticle once type 1001 has used 11: the other marker owner on slot 10 is left alone (no centring, no tag, no hit depth), at once and a minute later");
        k.describe(buf, sizeof(buf));
        check(std::strstr(buf, "11 (type 1001)") != nullptr && std::strstr(buf, "assumed") == nullptr, "F191: the key as the logs say it: \"11 (type 1001)\"");
        // the same key every frame: no change, the newest never expires
        int changes = 0;
        for (uint32_t t = 5000; t < 5000 + 90 * 20; t += 11) changes += k.observe(11u, t).changed ? 1 : 0;
        check(changes == 0 && k.isReticle(11u, 5000u + 3600000u), "F191: key 11 passed every frame is one key, one line; the newest key never expires");
        // the pilot's later session: the reticle re-created as 12, then 13, then 14 (a second apart)
        uint32_t t = 100000;
        bool walk = true;
        uint32_t prev = 11;
        for (uint32_t key : {12u, 13u, 14u}) {
            t += 1000;
            const f191::Observed o = k.observe(key, t);
            walk = walk && o.changed && o.previous == prev && k.isReticle(key, t) && !k.isReticle(10u, t);
            prev = key;
        }
        check(walk && k.newest() == 14u, "F191: the reticle walking 11, 12, 13, 14 as it is re-created: each new key is the reticle at once, slot 10 never");
    }

    // ---- 3. the overlap: the allocator hands out the next slot while the old reticle is alive -----------------------------------------------------------------------------------
    {
        f191::ReticleKeys k;
        k.observe(10u, 1000u);
        const f191::Observed o = k.observe(11u, 1100u);
        check(o.changed && o.previous == 10u && k.isReticle(10u, 1100u) && k.isReticle(11u, 1100u),
              "F191: a new key beside a live one: both are the reticle (the log's `[keys 10,11]`)");
        // both keys SetMarker'd every frame for 3 s, alternating: the newest does not flip, nothing is logged, both stay the reticle
        int changes = 0;
        bool both = true;
        uint32_t t = 1100;
        for (; t < 4100; t += 11) {
            changes += k.observe(10u, t).changed ? 1 : 0;
            changes += k.observe(11u, t + 5u).changed ? 1 : 0;
            both = both && k.isReticle(10u, t + 6u) && k.isReticle(11u, t + 6u);
        }
        check(changes == 0 && both && k.newest() == 11u,
              "F191: two live keys alternating through a 3 s overlap: no change is reported (one line for the whole overlap), both are the reticle throughout, the newest stays 11");
        // the old reticle dies: its key stops (its last SetMarker was the loop's last, t - 11), and 500 ms after that it is no longer the reticle
        const uint32_t lastOld = t - 11u;
        for (uint32_t u = t; u < t + 1500u; u += 11) k.observe(11u, u);
        check(k.isReticle(10u, lastOld + f191::kOverlapMs) && !k.isReticle(10u, lastOld + f191::kOverlapMs + 1u) && !k.isReticle(10u, t + 1500u) && k.isReticle(11u, t + 1500u),
              "F191: the old reticle gone: key 10 is the reticle for 500 ms after its last SetMarker and then released, key 11 stays");
    }

    // ---- 4. the previous key expiring -----------------------------------------------------------------------------------------------------------------------------------------------
    {
        f191::ReticleKeys k;
        k.observe(10u, 0u);
        k.observe(10u, 50u);
        k.observe(11u, 100u);   // 10 was last seen at 50
        check(k.isReticle(10u, 100u) && k.isReticle(10u, 50u + f191::kOverlapMs), "F191: the replaced key is the reticle up to 500 ms after its last SetMarker");
        check(!k.isReticle(10u, 50u + f191::kOverlapMs + 1u) && !k.isReticle(10u, 10000u), "F191: ... and not 1 ms later");
        check(k.isReticle(11u, 50u + f191::kOverlapMs + 1u) && k.isReticle(11u, 0xF0000000u), "F191: the newest is the reticle whatever the clock says");
        // a replaced key that comes back after it expired is a new allocation, not an overlap: the dead newest does not linger
        f191::ReticleKeys j;
        j.observe(10u, 0u);
        j.observe(11u, 100u);
        const f191::Observed back = j.observe(10u, 10100u);   // 10 was last seen at 0: gone for 10 s
        check(back.changed && back.previous == 11u && j.newest() == 10u && j.isReticle(10u, 10100u) && !j.isReticle(11u, 10100u),
              "F191: a key that comes back after its 500 ms ended is a new allocation (a change from 11 to 10), and the key it replaced, silent since 100 ms, is no longer the reticle");
        // the pair of keys never extends the older one past three: a third key replaces the other one
        f191::ReticleKeys m;
        m.observe(10u, 0u);
        m.observe(11u, 10u);
        m.observe(12u, 20u);
        check(m.isReticle(12u, 20u) && m.isReticle(11u, 20u) && !m.isReticle(10u, 20u), "F191: a third key within the window replaces the oldest: 12 and 11 are the reticle, 10 is not");
    }

    // ---- 5. an owner of slot 10 that never passes type 1001 --------------------------------------------------------------------------------------------------------------------------
    {
        f191::ReticleKeys k;
        // the hook sees only type 1001 calls: the waypoint's, the nearby markers' and slot 10's other owner pass other types and are never observed. Only 11 is.
        for (uint32_t t = 0; t < 30000; t += 11) k.observe(11u, t);
        bool never = true;
        for (uint32_t t = 0; t < 30000; t += 997) never = never && !k.isReticle(10u, t) && !k.isReticle(43u, t) && !k.isReticle(45u, t) && !k.isReticle(0x70u, t);
        check(never && k.isReticle(11u, 30000u), "F191: markers the hook never saw pass type 1001 (slot 10's other owner, the waypoint's 43 and 45, the weapon UI's 0x70 ...) are never taken for the reticle");
        // a key past the pool's range is not a pool slot: ignored, nothing changes
        const f191::Observed bad = k.observe(0xFFFFu, 31000u), big = k.observe(0x12345678u, 31000u);
        check(!bad.changed && !big.changed && k.newest() == 11u && !k.isReticle(0xFFFFu, 31000u), "F191: a key past the pool is ignored");
    }

    // ---- 6. the 32-bit millisecond clock wraps --------------------------------------------------------------------------------------------------------------------------------------
    {
        f191::ReticleKeys k;
        k.observe(10u, 0xFFFFFF00u);
        k.observe(11u, 0xFFFFFF80u);   // 10 last seen at 0xFFFFFF00
        check(k.isReticle(10u, 0x000000F0u) && !k.isReticle(10u, 0x00000100u), "F191: the replaced key's 500 ms window holds across the 32-bit wrap (0xFFFFFF00 + 496 ms yes, + 512 ms no)");
        // a stamp a few ms ahead of a reader that read the clock earlier is fresh; one far in the future is a wrapped old stamp
        f191::ReticleKeys j;
        j.observe(10u, 1000u);
        j.observe(11u, 1005u);
        check(j.isReticle(10u, 998u), "F191: a stamp 2 ms ahead of the reader's clock (another thread stamped it after the reader read the time) is fresh");
        check(!j.isReticle(10u, 1000u + 0x80000000u), "F191: a stamp half the clock's range away is not fresh");
    }

    // ---- 7. a reset starts from the fallback ---------------------------------------------------------------------------------------------------------------------------------------
    {
        f191::ReticleKeys k;
        k.observe(11u, 100u);
        k.observe(12u, 200u);
        k.reset();
        check(!k.seen() && k.newest() == 10u && k.isReticle(10u, 300u) && !k.isReticle(11u, 300u) && !k.isReticle(12u, 300u),
              "F191: after a reset (the hook goes in or comes out) the answer is the fallback again: key 10, no key of the last session");
        const f191::Observed o = k.observe(12u, 400u);
        check(o.changed && o.previous == f191::kNoKey && k.isReticle(12u, 400u) && !k.isReticle(11u, 400u), "F191: and the next session's first call is a change from no key held");
    }

    // ---- 8. the lines --------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f191::formatKeyChange(buf, sizeof(buf), f191::kNoKey, 11u);
        check(std::strcmp(buf, "reticle key (F191): 10 -> 11 (type 1001)") == 0, "F191: the first call names the fallback it replaces (\"reticle key (F191): 10 -> 11 (type 1001)\")");
        f191::formatKeyChange(buf, sizeof(buf), 11u, 12u);
        check(std::strcmp(buf, "reticle key (F191): 11 -> 12 (type 1001)") == 0, "F191: a later change names both keys");
        f191::formatKeyChange(buf, sizeof(buf), f191::kNoKey, 10u);
        check(std::strstr(buf, "10 (type 1001)") != nullptr && std::strstr(buf, "->") == nullptr, "F191: the first call on key 10 says the assumption held, not a change");
        check(f191::kReticleMarkerType == f87::kIndicatorTypeInteractable && f87::indicatorTypeOfCaller(0x1AB67D0) == f191::kReticleMarkerType &&
                  f87::indicatorTypeOfCaller(0x1AAE649) == f191::kReticleMarkerType && f87::indicatorTypeOfCaller(0x1AA977D) != f191::kReticleMarkerType,
              "F191: the type the tracker follows is the one F92c names (1001: the other interactables' owner), not the focus indicator's 1000");
    }

    // ---- 9. threads: the hook's writer and the readers on one tracker ------------------------------------------------------------------------------------------------------------------
    {
        // Two writers (the HUD update thread is one; two prove the compare-and-swap) alternate keys 11 and 12 while three readers ask for the keys the hook never passed.
        // A reader must never see 10, 13 or 99 as the reticle (a torn word would), and at the end the state names 11 and 12 only.
        f191::ReticleKeys k;
        k.observe(11u, 1u);   // the sighting that ends the fallback (key 10 would otherwise be the reticle by design before it)
        std::atomic<bool> stop{false};
        std::atomic<int> wrong{0};
        std::atomic<uint32_t> clock{1};
        std::vector<std::thread> th;
        for (int w = 0; w < 2; ++w)
            th.emplace_back([&, w] {
                for (int i = 0; i < 200000; ++i) {
                    const uint32_t t = clock.fetch_add(1, std::memory_order_relaxed);
                    k.observe((i + w) % 3 == 0 ? 11u : 12u, t);
                }
            });
        for (int r = 0; r < 3; ++r)
            th.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    const uint32_t t = clock.load(std::memory_order_relaxed);
                    if (k.isReticle(10u, t) || k.isReticle(13u, t) || k.isReticle(99u, t) || k.isReticle(0xFFFFu, t)) wrong.fetch_add(1, std::memory_order_relaxed);
                    const uint32_t n = k.newest();
                    if (n != 11u && n != 12u) wrong.fetch_add(1, std::memory_order_relaxed);
                }
            });
        th[0].join();
        th[1].join();
        stop.store(true, std::memory_order_relaxed);
        for (size_t i = 2; i < th.size(); ++i) th[i].join();
        const uint32_t end = clock.load();
        check(wrong.load() == 0 && (k.newest() == 11u || k.newest() == 12u) && k.isReticle(k.newest(), end),
              "F191: two writers and three readers on one tracker (400000 calls): a reader never sees a key the hook did not pass, and the newest is always one of the two it did");
    }

    // ---- 10. the source sites ---------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), f85h = slurp("src/d3d11/native_on_foot_f85.h");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const auto at = [&](const std::string& needle) { return cpp.find(needle); };
        check(cpp.find("key == 10") == std::string::npos && cpp.find("keyNow == 10u") == std::string::npos && cpp.find("isReticleKey") == std::string::npos &&
                  f85h.find("kReticleKey") == std::string::npos && f85h.find("isReticleKey") == std::string::npos,
              "F191: no test for the reticle by slot 10 is left (the probe, the post-hook, the draw decision, F68b's NDC, f85's isReticleKey and kReticleKey)");
        check(countOf(cpp, "if (f191IsReticle(key)) {") == 2 && has("f191IsReticle(key) || !f87p::keyDesignOffset(key, frame, &ox, &oy)") && has("const bool keyIsReticle = f191IsReticle(keyNow);") &&
                  has("if (haveP && keyIsReticle) {") && has("if (!keyIsReticle && f87p::keyDesignOffset(keyNow, frame, &offX, &offY)) {") && has("if (keyIsReticle) retNdcPublish(e, frame, nW, wc[3]);"),
              "F191: the pre-hook (the centring and its line), the post-hook (the F52 tag, not the world-marker path), F95b's early refusal, the hit depth, the label test and F68b's NDC all ask the tracker");
        // the feed: one place, inside the interactable branch of the indicator owner's SetMarker (type 1001), not the focus branch (1000)
        const size_t focusAt = at("if (type == nofoot::f87::kIndicatorTypeFocus) {"), interactAt = at("} else if (type == nofoot::f87::kIndicatorTypeInteractable) {"),
                     observeAt = at("g_reticleKeys.observe(static_cast<uint32_t>(key), f191Now())"), unknownAt = at("g_indUnknownN.fetch_add(1, std::memory_order_relaxed);");
        check(countOf(cpp, "g_reticleKeys.observe(") == 1 && focusAt != std::string::npos && interactAt != std::string::npos && observeAt != std::string::npos && unknownAt != std::string::npos &&
                  focusAt < interactAt && interactAt < observeAt && observeAt < unknownAt && has("static_assert(nofoot::f191::kReticleMarkerType == nofoot::f87::kIndicatorTypeInteractable") &&
                  has("nofoot::f191::formatKeyChange(line, sizeof(line), o.previous, static_cast<uint32_t>(key));"),
              "F191: the tracker is fed once, by the SetMarker trampoline, from the type 1001 branch only (not the focus indicator's 1000), and a change is queued as one line");
        check(countOf(cpp, "g_reticleKeys.reset();") == 2 && has("nofoot::f191::ReticleKeys g_reticleKeys;"),
              "F191: the tracker is reset when the trampoline goes in and when it comes out (a new session starts from the fallback)");
        check(has("reticle depth (F85c): key %s draws %llu, patched at the hit depth %llu;") &&
                  has("reticle depth (F85c): no reticle composite draw over %u s (reticle key %s; no tool or weapon out, ADS, or the marker fix is off)") &&
                  has("native on foot reticle: follows aim (key %s centred on the aim axis: x and y zeroed)") && has("native on foot reticle: NOT centred -- the instance could not be written; key %s stays at the game's aim"),
              "F191: the F85c lines and the \"follows aim\" line name the reticle's key and its type (\"key 11 (type 1001)\"), and the idle line says which key it waited for");
    }
}

}  // namespace f191test
