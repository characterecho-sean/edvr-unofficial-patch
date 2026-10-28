// F154 checks (native on-foot VR), run154 below. Header only, run by native_frame_test.
//
// F154 (native_on_foot_f154.h, a TEMPORARY instrument): the local body's draws into the eyes are listed by their identity (the shaders, the index buffer, the start index, the index
// count, the base vertex) in the order they first appear, capped at 64 and never renumbered; the dial leaves one listed draw out of both eyes, and only a draw that is the local
// body's (stencil ref 21) and goes into an eye render target.
#pragma once

#include "../../src/d3d11/native_on_foot_f154.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace f154test {

namespace f154 = edvr::nofoot::f154;

inline f154::Identity id154(uint64_t vs, uint64_t ps, uint64_t ib, uint32_t start, uint32_t count, int32_t base) {
    f154::Identity i;
    i.vs = vs;
    i.ps = ps;
    i.ib = ib;
    i.start = start;
    i.count = count;
    i.base = base;
    return i;
}

template <class Check>
inline void run154(Check&& check) {
    const f154::Identity a = id154(0x9D800F45B12A6AACull, 0x194DD6A677084A74ull, 0x1000, 0, 13260, 0);
    // ---- 1. an identity is the six fields, each of them ---------------------------------------------------------------------------------------------------------------------
    {
        check(f154::sameIdentity(a, a) && f154::identityHash(a) == f154::identityHash(a), "F154: an identity equals itself and hashes the same");
        f154::Identity b = a;
        b.vs ^= 1;
        f154::Identity c = a;
        c.ps ^= 1;
        f154::Identity d = a;
        d.ib += 16;
        f154::Identity e = a;
        e.start += 1;
        f154::Identity f = a;
        f.count += 1;
        f154::Identity g = a;
        g.base -= 1;
        check(!f154::sameIdentity(a, b) && !f154::sameIdentity(a, c) && !f154::sameIdentity(a, d) && !f154::sameIdentity(a, e) && !f154::sameIdentity(a, f) && !f154::sameIdentity(a, g),
              "F154: the vertex shader, the pixel shader, the index buffer, the start index, the index count and the base vertex each tell two draws apart");
        check(f154::identityHash(a) != f154::identityHash(b) && f154::identityHash(a) != f154::identityHash(c) && f154::identityHash(a) != f154::identityHash(d) &&
                  f154::identityHash(a) != f154::identityHash(e) && f154::identityHash(a) != f154::identityHash(f) && f154::identityHash(a) != f154::identityHash(g),
              "F154: ... and each changes the hash");
    }
    // ---- 2. the numbers: first appearance, kept ------------------------------------------------------------------------------------------------------------------------------
    {
        static f154::DrawList list;   // (about 7 KB)
        const f154::Identity b = id154(a.vs, a.ps, a.ib, 13260, 30216, 0);
        const f154::Identity c = id154(0x7B0DC42D383F694Cull, 0x0DF03E64DF9DBEF1ull, 0x2000, 0, 37080, 5);
        check(list.count() == 0, "F154: a new list is empty");
        f154::DrawList::Note n = list.note(a, 1, 0, 100);
        check(n.index == 0 && n.added && list.count() == 1, "F154: the first draw seen is number 0");
        n = list.note(b, 1, 0, 100);
        check(n.index == 1 && n.added, "F154: the second is 1");
        n = list.note(c, 2, 1, 101);
        check(n.index == 2 && n.added && list.count() == 3, "F154: the third is 2");
        n = list.note(b, 1, 1, 102);
        check(n.index == 1 && !n.added && list.count() == 3, "F154: a draw seen again keeps its number and is not added again");
        n = list.note(a, 1, 0, 103);
        check(n.index == 0 && !n.added, "F154: ... in any order: number 0 stays 0");
        check(list.at(0).firstFrame == 100 && list.at(2).firstFrame == 101 && list.at(2).instances == 2 && f154::sameIdentity(list.at(2).id, c),
              "F154: an entry keeps the frame it was first seen in, its instance count at first sight and its identity");
        // the eyes a draw was seen in
        check(std::string(f154::eyeText(list.at(0).eyes.load())) == "0", "F154: a draw seen in eye 0 only reads eye 0");
        check(std::string(f154::eyeText(list.at(1).eyes.load())) == "both", "F154: a draw seen in eye 0 and then eye 1 reads both");
        check(std::string(f154::eyeText(list.at(2).eyes.load())) == "1", "F154: a draw seen in eye 1 only reads eye 1");
        list.note(c, 2, -1, 104);
        check(std::string(f154::eyeText(list.at(2).eyes.load())) == "1" && (list.at(2).eyes.load() & f154::kEyeUnknown), "F154: an eye the table could not name is kept apart and does not make a draw 'both'");
        const f154::Identity u = id154(5, 6, 7, 8, 9, 10);
        list.note(u, 1, -1, 105);
        check(list.count() == 4 && std::string(f154::eyeText(list.at(3).eyes.load())) == "unknown", "F154: a draw seen only in an unnamed eye reads unknown (and is still listed)");
    }
    // ---- 3. the table: colliding slots, 64 draws, the cap, no renumbering --------------------------------------------------------------------------------------------------------
    {
        // identities that land in one slot of the table, so the probe has to walk
        std::vector<f154::Identity> same;
        const uint32_t want = static_cast<uint32_t>(f154::identityHash(id154(1, 1, 1, 1, 1, 1))) & (f154::kSlots - 1);
        for (uint32_t i = 2; same.size() < 6 && i < 1000000; ++i) {
            const f154::Identity t = id154(i, 1, 1, 1, 1, 1);
            if ((static_cast<uint32_t>(f154::identityHash(t)) & (f154::kSlots - 1)) == want) same.push_back(t);
        }
        same.insert(same.begin(), id154(1, 1, 1, 1, 1, 1));
        check(same.size() == 7, "F154: found seven draws that hash to one slot");
        static f154::DrawList list;
        bool ordered = true;
        for (size_t i = 0; i < same.size(); ++i) {
            const f154::DrawList::Note n = list.note(same[i], 1, 0, 1);
            ordered = ordered && n.index == static_cast<int>(i) && n.added;
        }
        bool found = true;
        for (size_t i = 0; i < same.size(); ++i) {
            const f154::DrawList::Note n = list.note(same[i], 1, 1, 2);
            found = found && n.index == static_cast<int>(i) && !n.added;
        }
        check(ordered && found && list.count() == 7, "F154: seven draws in one slot are numbered 0..6 in order and each is found again by its own number");

        static f154::DrawList full;
        bool all = true;
        for (int i = 0; i < f154::kCap; ++i) {
            const f154::DrawList::Note n = full.note(id154(0x1000 + static_cast<uint64_t>(i), 7, 3, static_cast<uint32_t>(i * 3), 100 + static_cast<uint32_t>(i), i - 5), 1, i & 1, 9);
            all = all && n.index == i && n.added;
        }
        check(all && full.count() == f154::kCap && full.refused() == 0, "F154: 64 different draws are numbered 0..63 and listed");
        const f154::DrawList::Note over = full.note(id154(0xFFFF, 7, 3, 1, 1, 1), 1, 0, 10);
        check(over.index == -1 && !over.added && full.count() == f154::kCap && full.refused() == 1, "F154: the 65th is neither numbered nor listed, and is counted");
        const f154::DrawList::Note over2 = full.note(id154(0xFFFF, 7, 3, 1, 1, 1), 1, 0, 11);
        check(over2.index == -1 && full.refused() == 2, "F154: ... again, still turned away (counted per draw)");
        bool same63 = true;
        for (int i = 0; i < f154::kCap; ++i) {
            const f154::DrawList::Note n = full.note(id154(0x1000 + static_cast<uint64_t>(i), 7, 3, static_cast<uint32_t>(i * 3), 100 + static_cast<uint32_t>(i), i - 5), 1, 0, 12);
            same63 = same63 && n.index == i && !n.added;
        }
        check(same63 && full.count() == f154::kCap, "F154: with the list full every listed draw keeps its number (nothing is renumbered)");
    }
    // ---- 4. the hide: the number, the stencil and the eye target are all required ----------------------------------------------------------------------------------------------
    {
        check(f154::hides(5, 5, 21, true), "F154: hide: the dial's draw, stencil 21, into an eye: left out");
        check(!f154::hides(5, 4, 21, true) && !f154::hides(5, 6, 21, true), "F154: hide: another number is drawn");
        check(!f154::hides(5, 5, 20, true) && !f154::hides(5, 5, 0, true) && !f154::hides(5, 5, 22, true) && !f154::hides(5, 5, 0xFFFFFFFFu, true),
              "F154: hide: the right number with another stencil ref (another person's body, the world) is drawn");
        check(!f154::hides(5, 5, 21, false), "F154: hide: the right number and stencil into something that is not an eye render target is drawn");
        check(!f154::hides(5, 5, 20, false), "F154: hide: neither the stencil nor the eye target: drawn");
        check(!f154::hides(-1, -1, 21, true) && !f154::hides(-1, 0, 21, true) && !f154::hides(-1, 5, 21, true), "F154: hide: the dial at -1 hides nothing");
        check(!f154::hides(5, -1, 21, true), "F154: hide: a draw that was not listed (the list full) is never hidden");
        check(f154::hides(0, 0, 21, true) && f154::hides(63, 63, 21, true), "F154: hide: the ends of the dial, 0 and 63, work");
        check(!f154::hides(64, 64, 21, true) && !f154::hides(100, 100, 21, true), "F154: hide: a dial past 63 hides nothing");
        check(f154::listable(21, true) && !f154::listable(21, false) && !f154::listable(20, true) && !f154::listable(0, false), "F154: a draw is listed only with stencil 21 and an eye target");
        check(f154::inList(0, 1) && f154::inList(3, 4) && !f154::inList(4, 4) && !f154::inList(-1, 4) && !f154::inList(0, 0) && !f154::inList(64, 100),
              "F154: the dial is in the list when it is a number below the draws listed");
    }
    // ---- 5. the counts of what was left out ---------------------------------------------------------------------------------------------------------------------------------
    {
        static f154::DrawList list;
        const int n = list.note(a, 1, 0, 1).index;
        list.countSkip(n, 0, 50);
        list.countSkip(n, 1, 50);
        list.countSkip(n, 0, 50);
        list.countSkip(n, 1, 51);
        list.countSkip(n, -1, 51);
        const f154::Entry& e = list.at(n);
        check(e.skipped[0].load() == 2 && e.skipped[1].load() == 2 && e.skipped[2].load() == 1, "F154: a left-out draw is counted in its own eye, and an unnamed eye apart");
        check(e.skipFrames.load() == 2, "F154: ... and its frame once, however many of its draws the frame left out (5 draws in 2 frames)");
        check(f154::eyeSlot(0) == 0 && f154::eyeSlot(1) == 1 && f154::eyeSlot(-1) == 2 && f154::eyeSlot(2) == 2, "F154: the eye slots are 0, 1 and unknown");
    }
    // ---- 6. a reader never sees an entry before it is whole ----------------------------------------------------------------------------------------------------------------------
    {
        static f154::DrawList list;
        std::atomic<bool> torn{false};
        std::atomic<bool> done{false};
        std::thread reader([&] {
            while (!done.load()) {
                const int n = list.count();
                for (int i = 0; i < n; ++i) {
                    const f154::Entry& e = list.at(i);
                    if (e.id.vs != 5000u + static_cast<uint64_t>(i) || e.id.count != 700u + static_cast<uint32_t>(i) || e.firstFrame != 40u + static_cast<uint32_t>(i)) torn.store(true);
                }
            }
        });
        for (int i = 0; i < f154::kCap; ++i) list.note(id154(5000u + static_cast<uint64_t>(i), 1, 1, 1, 700u + static_cast<uint32_t>(i), 0), 1, 0, 40u + static_cast<uint32_t>(i));
        done.store(true);
        reader.join();
        check(!torn.load() && list.count() == f154::kCap, "F154: a reader that takes count() sees every entry below it whole (the writer publishes after the fields)");
    }
}

}  // namespace f154test
