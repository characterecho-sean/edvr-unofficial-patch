// F95c checks (native on-foot VR), run95c below. Header only, run by native_frame_test.
//
// F95c: the head parts take the game's shadow-only class (2) for the length of the local avatar's visibility call (native_on_foot_f95c.h). These checks run the
// visibility job's admission as read on the 334925 exe (0x3DA5D75..0x3DA5E18: a part's mask is the OR of the bits of the views its class admits) over every
// flags value of the view's low 20 bits, and over a context shaped like the F3 census's (two eye views, shadow views, a shadow-ish view class 2 does not take):
// the head never reaches a main view, the guard the fade hook applies removes nothing from a swapped mask unless two views share a bit, and the fade hook's
// action follows the ticket.
#pragma once

#include "../../src/d3d11/native_on_foot_f95c.h"

#include <cstdint>

namespace f95ctest {

namespace f95c = edvr::nofoot::f95c;

// The visibility job's per-part loop for one instance: the OR of the bits of the views the class admits.
inline uint64_t jobMask(int32_t cls, const uint32_t* flags, const uint64_t* bits, uint32_t n) {
    uint64_t m = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (f95c::admits(cls, flags[i])) m |= bits[i];
    return m;
}

template <class Check>
inline void run95c(Check&& check) {
    // ---- the classes ------------------------------------------------------------------------------------------------------------------------------------------------
    check(f95c::shadowClass(1) == 2 && f95c::shadowClass(3) == 0 && f95c::shadowClass(0) == 0 && f95c::shadowClass(2) == 2 && f95c::shadowClass(7) == 7,
          "F95c: every view becomes the shadow views, main only becomes none, none and shadow-only stay, an unknown class is left alone");
    check(f95c::restoredClass(2, 2, 1) == 1 && f95c::restoredClass(0, 2, 1) == 0 && f95c::restoredClass(1, 0, 3) == 1,
          "F95c: the game's class goes back after the call, unless the game changed it during the call (its value stands)");

    // ---- admission over every flags value: the head never reaches a main view ----------------------------------------------------------------------------------
    bool mainNever = true, shadowExact = true, classOne = true, classThree = true, mainOnlyNone = true;
    for (uint32_t lo = 0; lo <= 0xFFFFu; ++lo) {
        for (uint32_t hi = 0; hi < 2; ++hi) {
            const uint32_t f = lo | (hi << 19);   // bit 19: the view-group bit the job tests at the AMC level (0x3DA5A0C)
            const f95c::ViewKind k = f95c::viewKind(f);
            for (int32_t c = 0; c <= 7; ++c)
                if (f95c::admits(f95c::shadowClass(c), f) && k == f95c::ViewKind::Main) mainNever = false;
            if (f95c::admits(f95c::shadowClass(f95c::kClassAll), f) != (k == f95c::ViewKind::Shadow)) shadowExact = false;
            if (!f95c::admits(f95c::kClassAll, f)) classOne = false;
            if (f95c::admits(f95c::kClassMain, f) != (k == f95c::ViewKind::Main)) classThree = false;
            if (f95c::admits(f95c::shadowClass(f95c::kClassMain), f)) mainOnlyNone = false;
        }
    }
    check(mainNever, "F95c: no class the swap writes admits a main view (131072 flags values, classes 0-7)");
    check(shadowExact, "F95c: a visible head part through the call takes exactly the views F95c counts as shadow views");
    check(classOne && classThree, "F95c: class 1 takes every view; class 3 takes exactly the main views (the job's !0x2856680)");
    check(mainOnlyNone, "F95c: a main-only head part takes no view through the call");
    check(f95c::viewKind(0) == f95c::ViewKind::Main && f95c::viewKind(0x80000) == f95c::ViewKind::Main && f95c::viewKind(0x10) == f95c::ViewKind::Shadow &&
              f95c::viewKind(0x20) == f95c::ViewKind::Shadow && f95c::viewKind(0x1000) == f95c::ViewKind::Shadow && f95c::viewKind(0x40) == f95c::ViewKind::Other &&
              f95c::viewKind(0x4000) == f95c::ViewKind::Other && f95c::viewKind(0x8000) == f95c::ViewKind::Main,
          "F95c: the view kinds: no bit of 0x7FF0 is main; 0x10, 0x20, 0x1000 shadow; 0x40, 0x4000 other; 0x8000 is outside 0x7FF0 (main)");

    // ---- a context like the F3 census's: eye A bit 1, eye B bit 22, shadow views bits 31, 32 and 40, a shadow-ish view class 2 does not take, bit 41 -----------
    {
        const uint32_t flags[6] = {0x0, 0x80000, 0x10, 0x10, 0x1000, 0x40};
        const uint64_t bits[6] = {1ull << 1, 1ull << 22, 1ull << 31, 1ull << 32, 1ull << 40, 1ull << 41};
        const f95c::Views v = f95c::splitViews(flags, bits, 6);
        check(v.n == 6 && v.main == 2 && v.shadow == 3 && v.other == 1 && v.shared == 0, "F95c context: 2 main, 3 shadow, 1 other view, no shared bit");
        check(v.deny == ((1ull << 1) | (1ull << 22) | (1ull << 41)) && v.shadowBits == ((1ull << 31) | (1ull << 32) | (1ull << 40)),
              "F95c context: deny is the eyes' and the other view's bits; the shadow bits are the three shadow views'");
        const uint64_t before = jobMask(f95c::kClassAll, flags, bits, 6);
        const uint64_t through = jobMask(f95c::shadowClass(f95c::kClassAll), flags, bits, 6);
        check(before == 0x30180400002ull && through == v.shadowBits, "F95c context: a visible head part's mask: every view before F95c, the shadow views through the call");
        check(f95c::guarded(through, v.deny) == through && f95c::popcount64(through) == 3,
              "F95c context: the fade hook's guard removes nothing from a swapped mask (3 shadow passes keep the head)");
        check(f95c::guarded(before, v.deny) == v.shadowBits && (f95c::guarded(before, v.deny) & ((1ull << 1) | (1ull << 22))) == 0,
              "F95c context: the guard alone takes every eye bit off an unswapped mask");
        check(jobMask(f95c::shadowClass(f95c::kClassMain), flags, bits, 6) == 0 && jobMask(f95c::shadowClass(0), flags, bits, 6) == 0,
              "F95c context: a main-only or never-visible head part stays out of every view");
    }
    // ---- a shadow view sharing the eye's bit: the guard removes it ----------------------------------------------------------------------------------------------------
    {
        const uint32_t flags[3] = {0x0, 0x10, 0x10};
        const uint64_t bits[3] = {1ull << 1, 1ull << 1, 1ull << 31};
        const f95c::Views v = f95c::splitViews(flags, bits, 3);
        const uint64_t through = jobMask(f95c::shadowClass(f95c::kClassAll), flags, bits, 3);
        check(v.shared == 1 && (through & (1ull << 1)) != 0 && f95c::guarded(through, v.deny) == (1ull << 31),
              "F95c shared bit: a shadow view carrying eye A's bit puts that bit in the head mask; the guard takes it off (the eye never draws the head)");
    }
    // ---- no shadow view in the context: the head is in no view, as zeroing left it ------------------------------------------------------------------------------------
    {
        const uint32_t flags[2] = {0x0, 0x0};
        const uint64_t bits[2] = {1ull << 1, 1ull << 22};
        const f95c::Views v = f95c::splitViews(flags, bits, 2);
        check(v.shadow == 0 && jobMask(f95c::shadowClass(f95c::kClassAll), flags, bits, 2) == 0, "F95c: with no shadow view the head is in no view");
        const f95c::Views none = f95c::splitViews(flags, bits, 0);
        check(none.n == 0 && none.deny == 0 && none.shadowBits == 0, "F95c: an empty context has no bits");
    }
    // ---- the fade hook's action ---------------------------------------------------------------------------------------------------------------------------------------------
    check(f95c::fadeAction(false, true) == f95c::Action::Hold && f95c::fadeAction(false, false) == f95c::Action::Hold,
          "F95c fade: the third-person camera writes nothing (F93d), ticket or not");
    check(f95c::fadeAction(true, true) == f95c::Action::Keep && f95c::fadeAction(true, false) == f95c::Action::Zero,
          "F95c fade: a ticket keeps the shadow masks; no ticket zeroes as before F95c");
    check(f95c::popcount64(0) == 0 && f95c::popcount64(~0ull) == 64 && f95c::popcount64(0x8000000000000001ull) == 2, "F95c: popcount64");
}

}  // namespace f95ctest
