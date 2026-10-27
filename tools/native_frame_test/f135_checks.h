// F135 checks (native on-foot VR): the on-foot menu probe's pure parts (measure only). Header only, run by native_frame_test (run135).
#pragma once

#include "../../src/d3d11/native_on_foot_f135.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace f135test {

namespace f135 = edvr::nofoot::f135;

template <class Check>
inline void run135(Check&& check) {
    {   // the counters: added by the hooks, taken to zero by the frame thread
        f135::Counters c;
        c.add(f135::kPlace);
        c.add(f135::kPlace);
        c.add(f135::kOwner);
        c.add(-1);
        c.add(f135::kCount);
        uint32_t out[f135::kCount];
        c.take(out);
        uint32_t again[f135::kCount];
        c.take(again);
        check(out[f135::kPlace] == 2 && out[f135::kOwner] == 1 && out[f135::kUpdate] == 0 && again[f135::kPlace] == 0 && again[f135::kOwner] == 0,
              "F135: the hooks' counts are taken to zero by the frame thread; an out-of-range counter is ignored");
    }
    {   // the distinct objects of the second: up to 8, the same one once, the rest counted
        f135::ObjSet set;
        for (int i = 0; i < 3; ++i) set.add(0x1000u, 0x4DC1B50u);
        set.add(0x2000u, 0x4DC1B50u);
        set.add(0x1000u, 0x5120A80u);   // the same pointer under another vtable is another object
        set.add(0u, 0u);                 // a null pointer with no vtable is still a key
        f135::Obj out[f135::ObjSet::kMax];
        uint32_t more = 0;
        const int n = set.take(out, &more);
        char text[200];
        f135::formatObjs(text, sizeof(text), out, n, more);
        check(n == 4 && more == 0 && std::string(text).find("0x00001000/+0x4DC1B50") != std::string::npos && std::string(text).find("0x00002000/+0x4DC1B50") != std::string::npos &&
                  std::string(text).find("0x00001000/+0x5120A80") != std::string::npos,
              "F135: the distinct (pointer, vtable RVA) pairs of the second are kept once each and printed as 0xPTR/+0xRVA");
        f135::ObjSet many;
        for (uint32_t i = 1; i <= 11; ++i) many.add(i * 0x100u, 0x10u);
        const int nm = many.take(out, &more);
        char text2[300];
        f135::formatObjs(text2, sizeof(text2), out, nm, more);
        const uint32_t moreMany = more;
        const int none = set.take(out, &more);
        check(nm == f135::ObjSet::kMax && moreMany == 3 && std::string(text2).find("+3 more") != std::string::npos && none == 0 && more == 0,
              "F135: 11 distinct objects keep 8 and count 3 more; a take clears the set");
        char empty[16];
        check(f135::formatObjs(empty, sizeof(empty), out, 0, 0) == 0 && empty[0] == 0, "F135: no objects print as nothing (the line says none)");
    }
    {   // the edges: a rise at once, a fall after 0.5 s of zero, a gap shorter than that is no fall
        f135::Edges e;
        const int c = f135::kPlace;
        uint64_t t = 1000;
        const f135::EdgeKind quiet = e.step(c, t, 0);
        t += 11;
        const f135::EdgeKind rose = e.step(c, t, 3);
        t += 11;
        const f135::EdgeKind still = e.step(c, t, 2);
        bool flickerFell = false;
        for (int i = 0; i < 40; ++i) {   // 440 ms of zero: not yet
            t += 11;
            flickerFell = flickerFell || e.step(c, t, 0) != f135::kNoEdge;
        }
        t += 11;
        const f135::EdgeKind back = e.step(c, t, 1);   // it came back: no new rise (it never fell)
        f135::EdgeKind fell = f135::kNoEdge;
        for (int i = 0; i < 60 && fell == f135::kNoEdge; ++i) {
            t += 11;
            fell = e.step(c, t, 0);
        }
        t += 11;
        const f135::EdgeKind again = e.step(c, t, 4);
        check(quiet == f135::kNoEdge && rose == f135::kRose && still == f135::kNoEdge && !flickerFell && back == f135::kNoEdge && fell == f135::kFell && again == f135::kRose,
              "F135: a counter's rise is an edge at once, a fall needs 0.5 s of zero (a shorter gap is no fall), and a later rise is an edge again");
        f135::Edges two;
        check(two.step(f135::kRay, 5000, 0) == f135::kNoEdge && two.step(f135::kRay, 9000, 0) == f135::kNoEdge && two.step(f135::kCount, 1, 1) == f135::kNoEdge,
              "F135: a counter that never moved has no edge, and an out-of-range counter has none");
    }
    {   // the key edges: the first sample is the baseline
        f135::KeyEdge k;
        const int a = k.step(true);   // Escape already down when the probe armed
        const int b = k.step(true);
        const int c = k.step(false);
        const int d = k.step(true);
        const int e = k.step(true);
        check(a == 0 && b == 0 && c == -1 && d == +1 && e == 0, "F135: a key held when the probe armed is the baseline, not a press; then one edge per change");
    }
    {   // when a second is worth a line: a gating counter moved or a key edge came recently; the update and the instance submit alone are not
        uint32_t counts[f135::kCount] = {};
        counts[f135::kUpdate] = 90;
        counts[f135::kInstSubmit] = 90;
        const bool quiet = !f135::lineWanted(counts, 100000, 0);
        const bool afterKey = f135::lineWanted(counts, 100000, 95000);
        const bool oldKey = f135::lineWanted(counts, 100000, 80000);
        counts[f135::kPanelSubmit] = 1;
        const bool gated = f135::lineWanted(counts, 100000, 0);
        check(quiet && afterKey && !oldKey && gated && !f135::gatesLine(f135::kUpdate) && !f135::gatesLine(f135::kInstSubmit) && f135::gatesLine(f135::kPlace) &&
                  f135::gatesLine(f135::kBridgeUpdate),
              "F135: a second prints when a panel counter moved or a key edge came in the last 8 s; the always-running update and instance submit alone keep it quiet");
    }
    {   // the budget
        f135::Budget b;
        int allowed = 0;
        for (int i = 0; i < 150; ++i) allowed += b.allow(1000u + static_cast<uint64_t>(i), 120) ? 1 : 0;
        const uint32_t held = b.takeSuppressed();
        check(allowed == 120 && held == 30 && b.allow(62000u, 120), "F135: at most 120 edge lines a minute, the rest counted, and the minute rolls");
        check(std::string(f135::counterName(f135::kOwner)) == "owner transform" && std::string(f135::counterName(f135::kBridgePlace)) == "bridge placement" &&
                  std::string(f135::counterName(-3)) == "?",
              "F135: the counters' names are the ones the health rule reads");
    }
}

}  // namespace f135test
