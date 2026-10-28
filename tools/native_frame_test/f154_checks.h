// F154/F155 checks (native on-foot VR), run154 below. Header only, run by native_frame_test.
//
// F154 (native_on_foot_f154.h, a TEMPORARY instrument): the eye draws that use the clip pixel shader (the body parts with the vertex clip modifier) are listed by their identity (the
// shaders, the index buffer, the start index, the index count, the base vertex) in the order they first appear, capped at 128 and never renumbered within a pass; the dial leaves one
// listed draw out of both eyes, and only a draw that uses the clip pixel shader and goes into an eye render target. F155: any vertex shader and any stencil ref (the ref is
// recorded), a pass begins when the dial is set from -1 to a number (the list emptied, numbered from 0 again), and each entry counts the frames it was drawn in each eye.
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
    const uint64_t kClip = 0x0DF03E64DF9DBEF1ull;
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
        static f154::DrawList list;   // (about 15 KB)
        const f154::Identity b = id154(a.vs, a.ps, a.ib, 13260, 30216, 0);
        const f154::Identity c = id154(0x7B0DC42D383F694Cull, kClip, 0x2000, 0, 37080, 5);
        check(list.count() == 0, "F154: a new list is empty");
        f154::DrawList::Note n = list.note(a, 1, 0, 100, 21);
        check(n.index == 0 && n.added && list.count() == 1, "F154: the first draw seen is number 0");
        n = list.note(b, 1, 0, 100, 21);
        check(n.index == 1 && n.added, "F154: the second is 1");
        n = list.note(c, 2, 1, 101, 21);
        check(n.index == 2 && n.added && list.count() == 3, "F154: the third is 2");
        n = list.note(b, 1, 1, 102, 21);
        check(n.index == 1 && !n.added && list.count() == 3, "F154: a draw seen again keeps its number and is not added again");
        n = list.note(a, 1, 0, 103, 21);
        check(n.index == 0 && !n.added, "F154: ... in any order: number 0 stays 0");
        check(list.at(0).firstFrame == 100 && list.at(2).firstFrame == 101 && list.at(2).instances == 2 && f154::sameIdentity(list.at(2).id, c),
              "F154: an entry keeps the frame it was first seen in, its instance count at first sight and its identity");
        // the eyes a draw was seen in
        check(std::string(f154::eyeText(list.at(0).eyes.load())) == "0", "F154: a draw seen in eye 0 only reads eye 0");
        check(std::string(f154::eyeText(list.at(1).eyes.load())) == "both", "F154: a draw seen in eye 0 and then eye 1 reads both");
        check(std::string(f154::eyeText(list.at(2).eyes.load())) == "1", "F154: a draw seen in eye 1 only reads eye 1");
        list.note(c, 2, -1, 104, 21);
        check(std::string(f154::eyeText(list.at(2).eyes.load())) == "1" && (list.at(2).eyes.load() & f154::kEyeUnknown), "F154: an eye the table could not name is kept apart and does not make a draw 'both'");
        const f154::Identity u = id154(5, 6, 7, 8, 9, 10);
        list.note(u, 1, -1, 105, 21);
        check(list.count() == 4 && std::string(f154::eyeText(list.at(3).eyes.load())) == "unknown", "F154: a draw seen only in an unnamed eye reads unknown (and is still listed)");
    }
    // ---- 3. the table: colliding slots, 128 draws, the cap, no renumbering --------------------------------------------------------------------------------------------------------
    {
        check(f154::kCap == 128 && f154::kOff == -1 && f154::kSlots >= 2u * f154::kCap, "F155: the list holds 128 draws (the dial -1..127) in a table twice its size");
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
            const f154::DrawList::Note n = list.note(same[i], 1, 0, 1, 21);
            ordered = ordered && n.index == static_cast<int>(i) && n.added;
        }
        bool found = true;
        for (size_t i = 0; i < same.size(); ++i) {
            const f154::DrawList::Note n = list.note(same[i], 1, 1, 2, 21);
            found = found && n.index == static_cast<int>(i) && !n.added;
        }
        check(ordered && found && list.count() == 7, "F154: seven draws in one slot are numbered 0..6 in order and each is found again by its own number");

        static f154::DrawList full;
        bool all = true;
        for (int i = 0; i < f154::kCap; ++i) {
            const f154::DrawList::Note n = full.note(id154(0x1000 + static_cast<uint64_t>(i), 7, 3, static_cast<uint32_t>(i * 3), 100 + static_cast<uint32_t>(i), i - 5), 1, i & 1, 9, 21);
            all = all && n.index == i && n.added;
        }
        check(all && full.count() == f154::kCap && full.refused() == 0, "F155: 128 different draws are numbered 0..127 and listed");
        const f154::DrawList::Note over = full.note(id154(0xFFFF, 7, 3, 1, 1, 1), 1, 0, 10, 21);
        check(over.index == -1 && !over.added && full.count() == f154::kCap && full.refused() == 1, "F155: the 129th is neither numbered nor listed, and is counted");
        const f154::DrawList::Note over2 = full.note(id154(0xFFFF, 7, 3, 1, 1, 1), 1, 0, 11, 21);
        check(over2.index == -1 && full.refused() == 2, "F154: ... again, still turned away (counted per draw)");
        bool sameAll = true;
        for (int i = 0; i < f154::kCap; ++i) {
            const f154::DrawList::Note n = full.note(id154(0x1000 + static_cast<uint64_t>(i), 7, 3, static_cast<uint32_t>(i * 3), 100 + static_cast<uint32_t>(i), i - 5), 1, 0, 12, 21);
            sameAll = sameAll && n.index == i && !n.added;
        }
        check(sameAll && full.count() == f154::kCap, "F154: with the list full every listed draw keeps its number (nothing is renumbered within the pass)");
    }
    // ---- 4. the shader filter and the hide: the number, the clip pixel shader and the eye target are all required -------------------------------------------------------------------
    {
        check(kClip == f154::kClipPs, "F155: the listed pixel shader is 0DF03E64DF9DBEF1, the clip shader of the parts that carry the vertex clip modifier");
        check(f154::listable(kClip, true), "F155: a draw with the clip pixel shader into an eye is listed");
        check(!f154::listable(kClip, false), "F155: ... but not into something that is not an eye render target");
        check(!f154::listable(0x194DD6A677084A74ull, true) && !f154::listable(0, true) && !f154::listable(kClip ^ 1ull, true) && !f154::listable(~0ull, true),
              "F155: a draw with another pixel shader (the body's other draws, the world) is not listed");
        check(f154::hides(5, 5, kClip, true), "F154: hide: the dial's draw, the clip shader, into an eye: left out");
        check(!f154::hides(5, 4, kClip, true) && !f154::hides(5, 6, kClip, true), "F154: hide: another number is drawn");
        check(!f154::hides(5, 5, 0x194DD6A677084A74ull, true) && !f154::hides(5, 5, 0, true) && !f154::hides(5, 5, ~0ull, true),
              "F155: hide: the right number with another pixel shader is drawn");
        check(!f154::hides(5, 5, kClip, false), "F154: hide: the right number and shader into something that is not an eye render target is drawn");
        check(!f154::hides(5, 5, 0, false), "F154: hide: neither the shader nor the eye target: drawn");
        check(!f154::hides(-1, -1, kClip, true) && !f154::hides(-1, 0, kClip, true) && !f154::hides(-1, 5, kClip, true), "F154: hide: the dial at -1 hides nothing");
        check(!f154::hides(5, -1, kClip, true), "F154: hide: a draw that was not listed (the list full) is never hidden");
        check(f154::hides(0, 0, kClip, true) && f154::hides(127, 127, kClip, true), "F155: hide: the ends of the dial, 0 and 127, work");
        check(!f154::hides(128, 128, kClip, true) && !f154::hides(200, 200, kClip, true), "F155: hide: a dial past 127 hides nothing");
        check(f154::inList(0, 1) && f154::inList(3, 4) && !f154::inList(4, 4) && !f154::inList(-1, 4) && !f154::inList(0, 0) && !f154::inList(128, 200) && f154::inList(127, 128),
              "F154: the dial is in the list when it is a number below the draws listed");
    }
    // ---- 5. the counts of what was left out ---------------------------------------------------------------------------------------------------------------------------------
    {
        static f154::DrawList list;
        const int n = list.note(a, 1, 0, 1, 21).index;
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
                    if (e.id.vs != 5000u + static_cast<uint64_t>(i) || e.id.count != 700u + static_cast<uint32_t>(i) || e.firstFrame != 40u + static_cast<uint32_t>(i) ||
                        e.stencil != 21u + static_cast<uint32_t>(i))
                        torn.store(true);
                }
            }
        });
        for (int i = 0; i < f154::kCap; ++i)
            list.note(id154(5000u + static_cast<uint64_t>(i), 1, 1, 1, 700u + static_cast<uint32_t>(i), 0), 1, 0, 40u + static_cast<uint32_t>(i), 21u + static_cast<uint32_t>(i));
        done.store(true);
        reader.join();
        check(!torn.load() && list.count() == f154::kCap, "F154: a reader that takes count() sees every entry below it whole (the writer publishes after the fields)");
    }
    // ---- 7. F155: the stencil ref is recorded, any stencil lists ------------------------------------------------------------------------------------------------------------------
    {
        static f154::DrawList list;
        const f154::Identity x = id154(0x7B0DC42D383F694Cull, kClip, 0x3000, 0, 714, 0);
        const f154::Identity y = id154(0x8B589D25B2A0ADDCull, kClip, 0x3000, 714, 900, 0);   // another vertex shader
        check(list.note(x, 1, 0, 10, 21).index == 0, "F155: a clip-shader draw with stencil ref 21 is listed");
        check(list.note(y, 1, 0, 10, 7).index == 1, "F155: ... and one with stencil ref 7 and another vertex shader is listed too (any stencil, any vertex shader)");
        check(list.at(0).stencil == 21 && list.at(1).stencil == 7, "F155: each entry records the stencil ref of its first draw");
        check(!list.at(0).stencilVaries.load() && !list.at(1).stencilVaries.load(), "F155: a draw seen again with the same ref does not mark the ref as varying");
        list.note(x, 1, 1, 11, 21);
        check(!list.at(0).stencilVaries.load(), "F155: ... in the other eye either");
        list.note(x, 1, 0, 12, 22);
        check(list.at(0).stencilVaries.load() && list.at(0).stencil == 21 && !list.at(1).stencilVaries.load(),
              "F155: the same identity drawn with another ref marks only its own entry as varying, and keeps the first ref");
        check(list.count() == 2, "F155: ... without listing it twice");
    }
    // ---- 8. F155: the frames an entry was drawn in, per eye -------------------------------------------------------------------------------------------------------------------------
    {
        static f154::DrawList list;
        const f154::Identity x = id154(1, kClip, 2, 3, 4, 5);
        list.note(x, 1, 0, 100, 21);
        list.note(x, 1, 0, 100, 21);
        list.note(x, 1, 1, 100, 21);
        list.note(x, 1, 0, 101, 21);
        list.note(x, 1, 1, 101, 21);
        list.note(x, 1, 1, 101, 21);
        list.note(x, 1, 0, 103, 21);
        list.note(x, 1, -1, 103, 21);
        const f154::Entry& e = list.at(0);
        check(e.drawnFrames[0].load() == 3, "F155: eye 0: three frames (100, 101, 103), the second draw of frame 100 not counted again");
        check(e.drawnFrames[1].load() == 2, "F155: eye 1: two frames (100, 101), the second draw of frame 101 not counted again");
        check(e.drawnFrames[2].load() == 1, "F155: an eye the table could not name has its own count");
        const f154::Identity z = id154(9, kClip, 9, 9, 9, 9);
        list.note(z, 1, 1, 0, 21);
        list.note(z, 1, 1, 0, 21);
        check(list.at(1).drawnFrames[1].load() == 1 && list.at(1).drawnFrames[0].load() == 0, "F155: a draw first seen in frame 0 counts that frame once, in its own eye only");
    }
    // ---- 9. F155: a pass begins when the dial leaves -1; the list is emptied and numbered from 0 again ------------------------------------------------------------------------------
    {
        check(f154::beginsPass(-1, 0) && f154::beginsPass(-1, 127) && f154::beginsPass(-1, 5), "F155: a pass begins when the dial is set from -1 to a number");
        check(!f154::beginsPass(5, 6) && !f154::beginsPass(0, 127) && !f154::beginsPass(127, 0) && !f154::beginsPass(5, -1) && !f154::beginsPass(-1, -1),
              "F155: ... and not when it moves between numbers, goes back to -1 or stays at -1");
        static f154::DrawList list;
        const f154::Identity x = id154(1, kClip, 2, 3, 4, 5);
        const f154::Identity y = id154(6, kClip, 7, 8, 9, 10);
        list.note(x, 1, 0, 100, 21);
        list.note(y, 1, 1, 100, 21);
        list.countSkip(1, 1, 100);
        check(list.count() == 2 && list.at(1).skipped[1].load() == 1 && list.at(0).drawnFrames[0].load() == 1, "F155: the first pass lists x as 0 and y as 1, with counts");
        list.reset();
        check(list.count() == 0 && list.refused() == 0, "F155: reset empties the list");
        const f154::DrawList::Note ny = list.note(y, 1, 1, 200, 21);
        const f154::DrawList::Note nx = list.note(x, 1, 0, 200, 21);
        check(ny.index == 0 && ny.added && nx.index == 1 && nx.added && list.count() == 2, "F155: the new pass numbers from 0 again, in the order the draws appear now (y first)");
        check(list.at(0).firstFrame == 200 && list.at(0).skipped[1].load() == 0 && list.at(0).skipFrames.load() == 0 && list.at(0).drawnFrames[1].load() == 1 &&
                  list.at(0).drawnFrames[0].load() == 0 && list.at(1).drawnFrames[0].load() == 1 && list.at(1).drawnFrames[1].load() == 0,
              "F155: the new pass's entries start with fresh counts and first frames (nothing carried over)");
        check(list.note(y, 1, 0, 201, 21).index == 0 && list.note(x, 1, 0, 201, 21).index == 1 && list.count() == 2, "F155: within the new pass nothing is renumbered");
        // a full list is emptied and takes draws again; the old identities find no stale slot
        static f154::DrawList full;
        for (int i = 0; i < f154::kCap; ++i) full.note(id154(0x2000 + static_cast<uint64_t>(i), kClip, 1, 1, 1, 1), 1, 0, 5, 21);
        full.note(id154(0xBEEF, kClip, 1, 1, 1, 1), 1, 0, 5, 21);
        check(full.count() == f154::kCap && full.refused() == 1, "F155: a full list turned a draw away");
        full.reset();
        check(full.count() == 0 && full.refused() == 0, "F155: reset empties a full list and its refused count");
        bool again = true;
        for (int i = f154::kCap - 1; i >= 0; --i) {
            const f154::DrawList::Note n = full.note(id154(0x2000 + static_cast<uint64_t>(i), kClip, 1, 1, 1, 1), 1, 1, 6, 21);
            again = again && n.added && n.index == f154::kCap - 1 - i;
        }
        check(again && full.count() == f154::kCap && full.at(0).id.vs == static_cast<uint64_t>(0x2000 + f154::kCap - 1), "F155: the same 128 draws in the opposite order take the numbers in the new order");
        const f154::DrawList::Note beef = full.note(id154(0xBEEF, kClip, 1, 1, 1, 1), 1, 0, 6, 21);
        check(beef.index == -1 && full.refused() == 1, "F155: the list fills again to the cap in the new pass and turns the next draw away");
    }
}

}  // namespace f154test
