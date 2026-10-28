// F154/F156 checks (native on-foot VR), run154 below. Header only, run by native_frame_test.
//
// F154 (native_on_foot_f154.h, a TEMPORARY instrument): the local body's eye draws (stencil ref 21 into an eye render target, any shader) are listed in the order they first appear,
// capped at 128 and never renumbered within a pass; the dial leaves one listed draw out of both eyes, and only a draw with stencil ref 21 that goes into an eye render target.
// F155: a pass begins when the dial is set from -1 to a number (the list emptied, numbered from 0 again), and each entry counts the frames it was drawn in each eye. F156: a draw is
// told apart by the vertex and pixel shaders, the index count and the instance count; the index buffer, start index and base vertex change from frame to frame for one mesh, so they
// are merged into the entry and counted as variants; the identities turned away once the list is full are counted too.
#pragma once

#include "../../src/d3d11/native_on_foot_f154.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace f154test {

namespace f154 = edvr::nofoot::f154;

inline f154::Identity id154(uint64_t vs, uint64_t ps, uint32_t count, uint32_t instances) {
    f154::Identity i;
    i.vs = vs;
    i.ps = ps;
    i.count = count;
    i.instances = instances;
    return i;
}

inline f154::Variant var154(uint64_t ib, uint32_t start, int32_t base) {
    f154::Variant v;
    v.ib = ib;
    v.start = start;
    v.base = base;
    return v;
}

template <class Check>
inline void run154(Check&& check) {
    const f154::Identity a = id154(0x9D800F45B12A6AACull, 0x194DD6A677084A74ull, 13260, 1);
    const f154::Variant va = var154(0x1000, 0, 0);
    const uint64_t kClip = 0x0DF03E64DF9DBEF1ull;
    // ---- 1. an identity is the four fields; the index buffer, start index and base vertex are not part of it -----------------------------------------------------------------------
    {
        check(f154::sameIdentity(a, a) && f154::identityHash(a) == f154::identityHash(a), "F154: an identity equals itself and hashes the same");
        f154::Identity b = a;
        b.vs ^= 1;
        f154::Identity c = a;
        c.ps ^= 1;
        f154::Identity d = a;
        d.count += 1;
        f154::Identity e = a;
        e.instances += 1;
        check(!f154::sameIdentity(a, b) && !f154::sameIdentity(a, c) && !f154::sameIdentity(a, d) && !f154::sameIdentity(a, e),
              "F156: the vertex shader, the pixel shader, the index count and the instance count each tell two draws apart");
        check(f154::identityHash(a) != f154::identityHash(b) && f154::identityHash(a) != f154::identityHash(c) && f154::identityHash(a) != f154::identityHash(d) &&
                  f154::identityHash(a) != f154::identityHash(e),
              "F156: ... and each changes the hash");
        check(f154::identityFingerprint(a) != 0 && f154::identityFingerprint(id154(0, 0, 0, 0)) != 0 && f154::identityFingerprint(a) == f154::identityFingerprint(a),
              "F156: an identity's fingerprint is never 0 (0 marks an empty slot)");
        f154::Variant v2 = va, v3 = va, v4 = va;
        v2.ib += 16;
        v3.start += 1;
        v4.base -= 1;
        check(f154::variantFingerprint(va) != 0 && f154::variantFingerprint(var154(0, 0, 0)) != 0 && f154::variantFingerprint(va) == f154::variantFingerprint(va),
              "F156: a variant's fingerprint is never 0 and is the same for the same variant");
        check(f154::variantFingerprint(va) != f154::variantFingerprint(v2) && f154::variantFingerprint(va) != f154::variantFingerprint(v3) &&
                  f154::variantFingerprint(va) != f154::variantFingerprint(v4),
              "F156: the index buffer, the start index and the base vertex each change a variant's fingerprint");
    }
    // ---- 2. the numbers: first appearance, kept ------------------------------------------------------------------------------------------------------------------------------
    {
        static f154::DrawList list;
        const f154::Identity b = id154(a.vs, a.ps, 30216, 1);
        const f154::Identity c = id154(0x7B0DC42D383F694Cull, kClip, 37080, 2);
        check(list.count() == 0 && list.refused() == 0 && list.turnedIdentities() == 0, "F154: a new list is empty");
        f154::DrawList::Note n = list.note(a, va, 0, 100);
        check(n.index == 0 && n.added && n.newVariant && list.count() == 1, "F154: the first draw seen is number 0");
        n = list.note(b, va, 0, 100);
        check(n.index == 1 && n.added, "F154: the second is 1");
        n = list.note(c, va, 1, 101);
        check(n.index == 2 && n.added && list.count() == 3, "F154: the third is 2");
        n = list.note(b, va, 1, 102);
        check(n.index == 1 && !n.added && !n.newVariant && list.count() == 3, "F154: a draw seen again keeps its number and is not added again");
        n = list.note(a, va, 0, 103);
        check(n.index == 0 && !n.added, "F154: ... in any order: number 0 stays 0");
        check(list.at(0).firstFrame == 100 && list.at(2).firstFrame == 101 && list.at(2).id.instances == 2 && f154::sameIdentity(list.at(2).id, c),
              "F154: an entry keeps the frame it was first seen in, its instance count and its identity");
        // the eyes a draw was seen in
        check(std::string(f154::eyeText(list.at(0).eyes.load())) == "0", "F154: a draw seen in eye 0 only reads eye 0");
        check(std::string(f154::eyeText(list.at(1).eyes.load())) == "both", "F154: a draw seen in eye 0 and then eye 1 reads both");
        check(std::string(f154::eyeText(list.at(2).eyes.load())) == "1", "F154: a draw seen in eye 1 only reads eye 1");
        list.note(c, va, -1, 104);
        check(std::string(f154::eyeText(list.at(2).eyes.load())) == "1" && (list.at(2).eyes.load() & f154::kEyeUnknown), "F154: an eye the table could not name is kept apart and does not make a draw 'both'");
        const f154::Identity u = id154(5, 6, 9, 10);
        list.note(u, va, -1, 105);
        check(list.count() == 4 && std::string(f154::eyeText(list.at(3).eyes.load())) == "unknown", "F154: a draw seen only in an unnamed eye reads unknown (and is still listed)");
    }
    // ---- 3. the table: colliding slots, 128 draws, the cap, no renumbering, the identities turned away ----------------------------------------------------------------------------
    {
        check(f154::kCap == 128 && f154::kOff == -1 && f154::kSlots >= 2u * f154::kCap, "F155: the list holds 128 draws (the dial -1..127) in a table twice its size");
        // identities that land in one slot of the table, so the probe has to walk
        std::vector<f154::Identity> same;
        const uint32_t want = static_cast<uint32_t>(f154::identityHash(id154(1, 1, 1, 1))) & (f154::kSlots - 1);
        for (uint32_t i = 2; same.size() < 6 && i < 1000000; ++i) {
            const f154::Identity t = id154(i, 1, 1, 1);
            if ((static_cast<uint32_t>(f154::identityHash(t)) & (f154::kSlots - 1)) == want) same.push_back(t);
        }
        same.insert(same.begin(), id154(1, 1, 1, 1));
        check(same.size() == 7, "F154: found seven draws that hash to one slot");
        static f154::DrawList list;
        bool ordered = true;
        for (size_t i = 0; i < same.size(); ++i) {
            const f154::DrawList::Note n = list.note(same[i], va, 0, 1);
            ordered = ordered && n.index == static_cast<int>(i) && n.added;
        }
        bool found = true;
        for (size_t i = 0; i < same.size(); ++i) {
            const f154::DrawList::Note n = list.note(same[i], va, 1, 2);
            found = found && n.index == static_cast<int>(i) && !n.added;
        }
        check(ordered && found && list.count() == 7, "F154: seven draws in one slot are numbered 0..6 in order and each is found again by its own number");

        static f154::DrawList full;
        bool all = true;
        for (int i = 0; i < f154::kCap; ++i) {
            const f154::DrawList::Note n = full.note(id154(0x1000 + static_cast<uint64_t>(i), 7, 100 + static_cast<uint32_t>(i), 1), var154(3, static_cast<uint32_t>(i * 3), i - 5), i & 1, 9);
            all = all && n.index == i && n.added;
        }
        check(all && full.count() == f154::kCap && full.refused() == 0 && full.turnedIdentities() == 0, "F155: 128 different draws are numbered 0..127 and listed");
        const f154::Identity over = id154(0xFFFF, 7, 1, 1);
        const f154::DrawList::Note n1 = full.note(over, va, 0, 10);
        check(n1.index == -1 && !n1.added && !n1.newVariant && full.count() == f154::kCap && full.refused() == 1 && full.turnedIdentities() == 1,
              "F156: the 129th is neither numbered nor listed, and is counted (one draw, one identity)");
        const f154::DrawList::Note n2 = full.note(over, va, 0, 11);
        check(n2.index == -1 && full.refused() == 2 && full.turnedIdentities() == 1, "F156: ... again, still turned away: two draws of one identity");
        full.note(id154(0xFFFE, 7, 1, 1), va, 0, 11);
        full.note(id154(0xFFFE, 7, 1, 2), va, 1, 11);   // the same shaders and count with another instance count is another identity
        full.note(over, var154(99, 99, 99), 0, 12);       // the same identity at another variant is not another identity
        check(full.refused() == 5 && full.turnedIdentities() == 3 && full.turnedLost() == 0, "F156: five draws turned away were three identities (a variant does not make a new one)");
        bool sameAll = true;
        for (int i = 0; i < f154::kCap; ++i) {
            const f154::DrawList::Note n = full.note(id154(0x1000 + static_cast<uint64_t>(i), 7, 100 + static_cast<uint32_t>(i), 1), va, 0, 12);
            sameAll = sameAll && n.index == i && !n.added;
        }
        check(sameAll && full.count() == f154::kCap, "F154: with the list full every listed draw keeps its number (nothing is renumbered within the pass)");
        // more identities than the turned-away table can tell apart: counted up to its limit, the rest as draws it could not tell
        const uint32_t before = full.turnedIdentities();
        for (uint32_t i = 0; i < f154::kTurnedCap + 100; ++i) full.note(id154(0x50000 + i, 7, 5, 1), va, 0, 13);
        check(full.turnedIdentities() == f154::kTurnedCap && full.turnedLost() == before + 100 && full.count() == f154::kCap,
              "F156: past 768 identities turned away the count stops there and says so (turnedLost), and the list is unchanged");
        // and one already counted is still found, not lost
        const uint32_t lost = full.turnedLost();
        full.note(id154(0x50000, 7, 5, 1), va, 0, 14);
        check(full.turnedLost() == lost && full.turnedIdentities() == f154::kTurnedCap, "F156: an identity that was counted is found again with the table full, not counted as lost");
    }
    // ---- 4. the filter and the hide: the number, stencil ref 21 and the eye target are all required; the shader is not -----------------------------------------------------------
    {
        check(f154::kStencilRef == 21, "F156: the listed stencil ref is 21, the local player's body");
        check(f154::listable(21, true), "F156: a draw with stencil ref 21 into an eye is listed");
        check(!f154::listable(21, false), "F156: ... but not into something that is not an eye render target");
        check(!f154::listable(0, true) && !f154::listable(20, true) && !f154::listable(22, true) && !f154::listable(255, true) && !f154::listable(~0u, true),
              "F156: a draw with another stencil ref (another person's body, the world) is not listed");
        check(f154::hides(5, 5, 21, true), "F154: hide: the dial's draw, stencil ref 21, into an eye: left out");
        check(!f154::hides(5, 4, 21, true) && !f154::hides(5, 6, 21, true), "F154: hide: another number is drawn");
        check(!f154::hides(5, 5, 0, true) && !f154::hides(5, 5, 22, true) && !f154::hides(5, 5, ~0u, true), "F156: hide: the right number with another stencil ref is drawn");
        check(!f154::hides(5, 5, 21, false), "F154: hide: the right number and ref into something that is not an eye render target is drawn");
        check(!f154::hides(5, 5, 0, false), "F154: hide: neither the ref nor the eye target: drawn");
        check(!f154::hides(-1, -1, 21, true) && !f154::hides(-1, 0, 21, true) && !f154::hides(-1, 5, 21, true), "F154: hide: the dial at -1 hides nothing");
        check(!f154::hides(5, -1, 21, true), "F154: hide: a draw that was not listed (the list full) is never hidden");
        check(f154::hides(0, 0, 21, true) && f154::hides(127, 127, 21, true), "F155: hide: the ends of the dial, 0 and 127, work");
        check(!f154::hides(128, 128, 21, true) && !f154::hides(200, 200, 21, true), "F155: hide: a dial past 127 hides nothing");
        check(f154::inList(0, 1) && f154::inList(3, 4) && !f154::inList(4, 4) && !f154::inList(-1, 4) && !f154::inList(0, 0) && !f154::inList(128, 200) && f154::inList(127, 128),
              "F154: the dial is in the list when it is a number below the draws listed");
    }
    // ---- 5. the counts of what was left out ---------------------------------------------------------------------------------------------------------------------------------
    {
        static f154::DrawList list;
        const int n = list.note(a, va, 0, 1).index;
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
                        e.first.start != 9000u + static_cast<uint32_t>(i) || e.first.ib != 0x2000u + static_cast<uint64_t>(i) || e.variants.load() < 1u)
                        torn.store(true);
                }
            }
        });
        for (int i = 0; i < f154::kCap; ++i)
            list.note(id154(5000u + static_cast<uint64_t>(i), 1, 700u + static_cast<uint32_t>(i), 1), var154(0x2000u + static_cast<uint64_t>(i), 9000u + static_cast<uint32_t>(i), 0), 0, 40u + static_cast<uint32_t>(i));
        done.store(true);
        reader.join();
        check(!torn.load() && list.count() == f154::kCap, "F154: a reader that takes count() sees every entry below it whole (the writer publishes after the fields)");
    }
    // ---- 7. F156: one mesh at a changing start index and base vertex keeps one number; the variants are counted -------------------------------------------------------------------
    {
        static f154::DrawList list;
        const f154::Identity m = id154(0x7B0DC42D383F694Cull, 0x7268762D11A610F2ull, 37080, 1);
        const uint64_t ib = 0x219FFAE9A60ull;
        // 37080 indices at the start indexes and base vertices the F155 flight saw, then more of them
        const uint32_t starts[] = {3271096, 3252904, 3252816, 3260000, 3261000, 3262000};
        const int32_t bases[] = {1278997, 437344, 437344, 1000, 2000, 3000};
        bool one = true, fresh = true;
        for (int i = 0; i < 6; ++i) {
            const f154::DrawList::Note n = list.note(m, var154(ib, starts[i], bases[i]), i & 1, 100 + static_cast<uint32_t>(i));
            one = one && n.index == 0 && n.added == (i == 0);
            fresh = fresh && n.newVariant;
        }
        check(one && list.count() == 1, "F156: one mesh drawn at six different start indexes and base vertices keeps one number (0) and is listed once");
        check(fresh && list.at(0).variants.load() == 6 && list.at(0).variantsLost.load() == 0, "F156: ... each of the six is a new variant, counted: 6 variants, none lost");
        check(list.at(0).first.ib == ib && list.at(0).first.start == 3271096 && list.at(0).first.base == 1278997, "F156: the entry keeps the first draw's index buffer, start index and base vertex");
        f154::DrawList::Note again = list.note(m, var154(ib, 3252904, 437344), 0, 110);
        check(again.index == 0 && !again.added && !again.newVariant && list.at(0).variants.load() == 6, "F156: a variant seen again is not counted again");
        again = list.note(m, var154(ib, 3252816, 437344), 1, 110);
        check(!again.newVariant && list.at(0).variants.load() == 6, "F156: ... in either eye (the same start with the same base)");
        // each of the three values alone makes a new variant
        check(list.note(m, var154(ib + 64, 3271096, 1278997), 0, 111).newVariant && list.at(0).variants.load() == 7, "F156: another index buffer alone is a new variant");
        check(list.note(m, var154(ib, 3271097, 1278997), 0, 111).newVariant && list.at(0).variants.load() == 8, "F156: another start index alone is a new variant");
        check(list.note(m, var154(ib, 3271096, 1278998), 0, 111).newVariant && list.at(0).variants.load() == 9, "F156: another base vertex alone is a new variant");
        check(list.count() == 1, "F156: ... and the list still holds the one entry");
        // two meshes with the same shaders and different index counts are two numbers
        const f154::Identity m2 = id154(m.vs, m.ps, 30216, 1);
        const f154::DrawList::Note n2 = list.note(m2, var154(ib, 3271096, 1278997), 0, 112);
        check(n2.index == 1 && n2.added && list.count() == 2, "F156: two meshes with the same shaders and different index counts are two numbers");
        // the same count with another instance count is another number
        const f154::Identity m3 = id154(m.vs, m.ps, 37080, 2);
        const f154::DrawList::Note n3 = list.note(m3, var154(ib, 3271096, 1278997), 0, 112);
        check(n3.index == 2 && n3.added && list.count() == 3, "F156: the same count with another instance count gives another number");
        // the same shaders and count under another vertex shader or pixel shader are other numbers
        check(list.note(id154(m.vs ^ 1, m.ps, 37080, 1), va, 0, 113).index == 3 && list.note(id154(m.vs, m.ps ^ 1, 37080, 1), va, 0, 113).index == 4, "F156: ... and so are the same counts under another vertex shader or pixel shader");
        // variants are counted per entry: the same (ib, start, base) on two entries counts on both
        check(list.at(1).variants.load() == 1 && list.at(2).variants.load() == 1 && list.at(0).variants.load() == 9, "F156: the same variant on two entries counts once on each");
        list.note(m2, var154(ib, 3271096, 1278997), 1, 113);
        check(list.at(1).variants.load() == 1, "F156: ... and a repeat on one entry does not count again");
    }
    // ---- 8. F156: the variants past the cap are told apart no longer, and say so ---------------------------------------------------------------------------------------------------
    {
        static f154::DrawList list;
        const f154::Identity m = id154(1, kClip, 100, 1);
        check(f154::kVariantCap == 64 && f154::kVariantSlots == 128, "F156: an entry tells 64 variants apart, in a row of 128 slots");
        bool added = true;
        for (uint32_t i = 0; i < 200; ++i) {
            const f154::DrawList::Note n = list.note(m, var154(0x500, i, static_cast<int32_t>(i) * 7), 0, 300 + i);
            added = added && n.index == 0 && n.added == (i == 0) && n.newVariant == (i < f154::kVariantCap);
        }
        check(added && list.count() == 1, "F156: 200 different variants of one mesh keep one number; the first 64 are new, the rest are not counted");
        check(list.at(0).variants.load() == f154::kVariantCap && list.at(0).variantsLost.load() == 200 - f154::kVariantCap, "F156: the entry says 64 variants and 136 draws it could not tell");
        const f154::DrawList::Note kept = list.note(m, var154(0x500, 5, 35), 0, 600);
        check(!kept.newVariant && list.at(0).variantsLost.load() == 200 - f154::kVariantCap, "F156: a variant counted before the cap is found again with the row full, not counted as lost");
        const f154::DrawList::Note late = list.note(m, var154(0x500, 5000, 1), 0, 600);
        check(!late.newVariant && late.index == 0 && list.at(0).variantsLost.load() == 201 - f154::kVariantCap, "F156: a new variant past the cap is lost, and the draw keeps its number");
        // another entry is not held back by a full row
        const f154::DrawList::Note other = list.note(id154(2, kClip, 100, 1), var154(0x500, 0, 0), 0, 601);
        check(other.index == 1 && other.newVariant && list.at(1).variants.load() == 1 && list.at(1).variantsLost.load() == 0, "F156: another entry has its own row");
    }
    // ---- 9. F155: the stencil ref and the shader do not decide what the list holds (the caller asks listable); a draw is listed whatever its shaders ------------------------------------
    {
        static f154::DrawList list;
        const f154::Identity x = id154(0x7B0DC42D383F694Cull, kClip, 714, 1);
        const f154::Identity y = id154(0x8B589D25B2A0ADDCull, 0x7268762D11A610F2ull, 900, 1);   // another vertex shader and another pixel shader
        check(list.note(x, va, 0, 10).index == 0, "F156: a draw with the clip pixel shader is listed");
        check(list.note(y, va, 0, 10).index == 1, "F156: ... and so is one with other shaders (any shader)");
        list.note(x, va, 1, 11);
        check(list.count() == 2 && list.at(0).eyes.load() == (f154::kEye0 | f154::kEye1), "F156: ... without listing the first twice");
    }
    // ---- 10. F155: the frames an entry was drawn in, per eye -------------------------------------------------------------------------------------------------------------------------
    {
        static f154::DrawList list;
        const f154::Identity x = id154(1, kClip, 4, 5);
        list.note(x, va, 0, 100);
        list.note(x, va, 0, 100);
        list.note(x, va, 1, 100);
        list.note(x, va, 0, 101);
        list.note(x, va, 1, 101);
        list.note(x, va, 1, 101);
        list.note(x, va, 0, 103);
        list.note(x, va, -1, 103);
        const f154::Entry& e = list.at(0);
        check(e.drawnFrames[0].load() == 3, "F155: eye 0: three frames (100, 101, 103), the second draw of frame 100 not counted again");
        check(e.drawnFrames[1].load() == 2, "F155: eye 1: two frames (100, 101), the second draw of frame 101 not counted again");
        check(e.drawnFrames[2].load() == 1, "F155: an eye the table could not name has its own count");
        const f154::Identity z = id154(9, kClip, 9, 9);
        list.note(z, va, 1, 0);
        list.note(z, va, 1, 0);
        check(list.at(1).drawnFrames[1].load() == 1 && list.at(1).drawnFrames[0].load() == 0, "F155: a draw first seen in frame 0 counts that frame once, in its own eye only");
    }
    // ---- 11. F155: a pass begins when the dial leaves -1; the list is emptied and numbered from 0 again ------------------------------------------------------------------------------
    {
        check(f154::beginsPass(-1, 0) && f154::beginsPass(-1, 127) && f154::beginsPass(-1, 5), "F155: a pass begins when the dial is set from -1 to a number");
        check(!f154::beginsPass(5, 6) && !f154::beginsPass(0, 127) && !f154::beginsPass(127, 0) && !f154::beginsPass(5, -1) && !f154::beginsPass(-1, -1),
              "F155: ... and not when it moves between numbers, goes back to -1 or stays at -1");
        static f154::DrawList list;
        const f154::Identity x = id154(1, kClip, 4, 5);
        const f154::Identity y = id154(6, kClip, 9, 10);
        list.note(x, va, 0, 100);
        list.note(x, var154(0x1000, 8, 8), 0, 100);
        list.note(y, va, 1, 100);
        list.countSkip(1, 1, 100);
        check(list.count() == 2 && list.at(1).skipped[1].load() == 1 && list.at(0).drawnFrames[0].load() == 1 && list.at(0).variants.load() == 2,
              "F155: the first pass lists x as 0 and y as 1, with counts (x at two variants)");
        list.reset();
        check(list.count() == 0 && list.refused() == 0 && list.turnedIdentities() == 0 && list.turnedLost() == 0, "F155: reset empties the list");
        const f154::DrawList::Note ny = list.note(y, va, 1, 200);
        const f154::DrawList::Note nx = list.note(x, va, 0, 200);
        check(ny.index == 0 && ny.added && nx.index == 1 && nx.added && list.count() == 2, "F155: the new pass numbers from 0 again, in the order the draws appear now (y first)");
        check(list.at(0).firstFrame == 200 && list.at(0).skipped[1].load() == 0 && list.at(0).skipFrames.load() == 0 && list.at(0).drawnFrames[1].load() == 1 &&
                  list.at(0).drawnFrames[0].load() == 0 && list.at(1).drawnFrames[0].load() == 1 && list.at(1).drawnFrames[1].load() == 0,
              "F155: the new pass's entries start with fresh counts and first frames (nothing carried over)");
        check(list.at(1).variants.load() == 1 && list.at(1).variantsLost.load() == 0, "F156: ... and with a fresh variant count (x's two variants of the last pass are not carried over)");
        check(list.note(x, var154(0x1000, 8, 8), 0, 201).newVariant && list.at(1).variants.load() == 2, "F156: a variant of the last pass is new again in this one");
        check(list.note(y, va, 0, 201).index == 0 && list.note(x, va, 0, 201).index == 1 && list.count() == 2, "F155: within the new pass nothing is renumbered");
        // a full list is emptied and takes draws again; the old identities find no stale slot
        static f154::DrawList full;
        for (int i = 0; i < f154::kCap; ++i) full.note(id154(0x2000 + static_cast<uint64_t>(i), kClip, 1, 1), va, 0, 5);
        full.note(id154(0xBEEF, kClip, 1, 1), va, 0, 5);
        check(full.count() == f154::kCap && full.refused() == 1 && full.turnedIdentities() == 1, "F155: a full list turned a draw away");
        full.reset();
        check(full.count() == 0 && full.refused() == 0 && full.turnedIdentities() == 0, "F155: reset empties a full list, its refused count and its turned-away identities");
        bool again = true;
        for (int i = f154::kCap - 1; i >= 0; --i) {
            const f154::DrawList::Note n = full.note(id154(0x2000 + static_cast<uint64_t>(i), kClip, 1, 1), va, 1, 6);
            again = again && n.added && n.index == f154::kCap - 1 - i;
        }
        check(again && full.count() == f154::kCap && full.at(0).id.vs == static_cast<uint64_t>(0x2000 + f154::kCap - 1), "F155: the same 128 draws in the opposite order take the numbers in the new order");
        const f154::DrawList::Note beef = full.note(id154(0xBEEF, kClip, 1, 1), va, 0, 6);
        check(beef.index == -1 && full.refused() == 1 && full.turnedIdentities() == 1, "F155: the list fills again to the cap in the new pass and turns the next draw away (counted anew)");
    }
}

}  // namespace f154test
