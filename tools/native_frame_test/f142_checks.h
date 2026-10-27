// F142 checks (native on-foot VR), run142 below. Header only, run by native_frame_test.
//
// F142 (native_on_foot_f142.h and native_on_foot_f141.h): (1) the icon mesh table's slot reuse: a configuration still drawing keeps its slot, a new one takes a free slot, else
// the slot not seen for the longest and for at least 60 frames, never one with a copy in flight or one a record or the staging ring names; (2) the full outfit A/B's promotion
// (a class-2 part that is not a head part and not the first-person skeleton is class 1 for the visibility call, the game's class back after, a class the game changed
// stands); (3) the outfit census's signature (never the masks) and the parts that changed; (4) F141's checks on the lanes used (rows 0..2 xyz and the position: a NaN in
// padding lane 3 passes, in lane 12 fails), every rejection reason counted, the raw floats' text, the two candidate positions of E.
#pragma once

#include "../../src/d3d11/avatar_hooks_core.h"
#include "../../src/d3d11/native_on_foot_f142.h"
#include "f141_checks.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace f142test {

namespace f142 = edvr::nofoot::f142;
namespace f141 = edvr::nofoot::f141;
namespace f95c = edvr::nofoot::f95c;

// A model of the icon table: the same lookup, free-slot and victim choice iconCapture makes (native_on_foot.cpp), with a configuration as an integer.
struct IconTable {
    int state[f142::kIconMeshes];
    uint32_t seen[f142::kIconMeshes];
    int cfg[f142::kIconMeshes];
    bool pinned[f142::kIconMeshes];   // the staging ring's owner or a live record
    uint32_t reused = 0, full = 0;
    IconTable() {
        for (int i = 0; i < f142::kIconMeshes; ++i) {
            state[i] = f142::kIconFree;
            seen[i] = 0;
            cfg[i] = -1;
            pinned[i] = false;
        }
    }
    // The slot the configuration reads from at this frame, or -1 (the table is full and every slot is in use or seen in the last minAge frames).
    int draw(int config, uint32_t frame, int newState = f142::kIconDecoded) {
        int freeSlot = -1;
        for (int i = 0; i < f142::kIconMeshes; ++i) {
            if (state[i] == f142::kIconFree) {
                if (freeSlot < 0) freeSlot = i;
                continue;
            }
            if (cfg[i] == config) {
                seen[i] = frame;
                return i;
            }
        }
        int slot = freeSlot;
        if (slot < 0) {
            slot = f142::iconVictim(state, seen, f142::kIconMeshes, frame, f142::kIconReuseMinAgeFrames, [&](int i) { return pinned[i]; });
            if (slot < 0) {
                ++full;
                return -1;
            }
            ++reused;
        }
        state[slot] = newState;
        cfg[slot] = config;
        seen[slot] = frame;
        return slot;
    }
};

template <class Check>
inline void run142(Check&& check) {
    // ---- 1. the icon mesh table's reuse choice ---------------------------------------------------------------------------------------------------------------------------
    check(f142::kIconMeshes == 32 && f142::kIconMeshes <= 64 && f142::kIconReuseMinAgeFrames == 60,
          "F142: the table has 32 slots (the victim search's skip set is 64 bits) and a slot is reusable after 60 frames unseen");
    {
        int st[f142::kIconMeshes];
        uint32_t seen[f142::kIconMeshes];
        for (int i = 0; i < f142::kIconMeshes; ++i) {
            st[i] = f142::kIconDecoded;
            seen[i] = 1000;
        }
        const auto none = [](int) { return false; };
        check(f142::iconVictim(st, seen, f142::kIconMeshes, 1059, 60, none) == -1, "F142: every slot seen within 59 frames: no victim (the table is genuinely busy)");
        check(f142::iconVictim(st, seen, f142::kIconMeshes, 1060, 60, none) == 0, "F142: 60 frames unseen is old enough; a tie goes to the lowest slot");
        seen[5] = 900;
        seen[9] = 900;
        seen[20] = 800;
        check(f142::iconVictim(st, seen, f142::kIconMeshes, 1100, 60, none) == 20, "F142: the slot not seen for the longest is the victim");
        st[20] = f142::kIconInFlight;
        check(f142::iconVictim(st, seen, f142::kIconMeshes, 1100, 60, none) == 5, "F142: a slot with a copy in flight is never the victim (the next oldest, the lower of a tie)");
        st[20] = f142::kIconRefused;
        check(f142::iconVictim(st, seen, f142::kIconMeshes, 1100, 60, none) == 20, "F142: a refused slot that has aged out is reusable like a decoded one");
        seen[20] = 1090;
        check(f142::iconVictim(st, seen, f142::kIconMeshes, 1100, 60, none) == 5, "F142: a refused slot still drawing (seen 10 frames ago) keeps its slot");
        // pinned: asked of a candidate only, oldest first
        int asked = 0, order[f142::kIconMeshes] = {};
        const auto pin5 = [&](int i) {
            order[asked++] = i;
            return i == 5;
        };
        check(f142::iconVictim(st, seen, f142::kIconMeshes, 1100, 60, pin5) == 9 && asked == 2 && order[0] == 5 && order[1] == 9,
              "F142: a slot a record or the staging ring names is skipped (the next oldest is asked after it); only candidates are asked");
        const auto all = [](int) { return true; };
        check(f142::iconVictim(st, seen, f142::kIconMeshes, 1100, 60, all) == -1, "F142: every aged slot pinned: no victim");
        {
            int st2[f142::kIconMeshes];
            uint32_t seen2[f142::kIconMeshes];
            for (int i = 0; i < f142::kIconMeshes; ++i) {
                st2[i] = f142::kIconDecoded;
                seen2[i] = 1000;
            }
            int asked2 = 0;
            const auto spy = [&](int) {
                ++asked2;
                return false;
            };
            check(f142::iconVictim(st2, seen2, f142::kIconMeshes, 1030, 60, spy) == -1 && asked2 == 0,
                  "F142: with no slot aged out the pinned test is never asked (a full table does not scan the records)");
        }
        // frames
        seen[7] = 5000;   // a stamp from the future (the counter went back): age 0, never a huge age
        check(f142::iconAge(100, 5000) == 0 && f142::iconAge(5000, 100) == 4900 && f142::iconAge(100, 100) == 0 &&
                  f142::iconVictim(st, seen, f142::kIconMeshes, 1100, 60, none) == 5,
              "F142: a stamp from the future is age 0: the slot is not freed");
        int freeAll[f142::kIconMeshes] = {};
        check(f142::iconVictim(freeAll, seen, f142::kIconMeshes, 1100, 60, none) == -1 && f142::iconVictim(nullptr, seen, 4, 1100, 60, none) == -1 &&
                  f142::iconVictim(st, seen, 0, 1100, 60, none) == -1 && f142::iconVictim(st, seen, 65, 1100, 60, none) == -1,
              "F142: free slots are the caller's own first choice (never a victim), and a bad table is no victim");
        const int cs[4] = {f142::kIconFree, f142::kIconInFlight, f142::kIconDecoded, f142::kIconRefused};
        const uint32_t cseen[4] = {0, 90, 80, 50};
        const f142::IconCounts c = f142::iconCount(cs, cseen, 4, 100);
        check(c.free == 1 && c.inFlight == 1 && c.decoded == 1 && c.refused == 1 && c.oldestIdle == 50, "F142: the slots by state and the longest idle age (a free slot has none)");
        check(f142::powerOfTwo(1) && f142::powerOfTwo(2) && f142::powerOfTwo(64) && !f142::powerOfTwo(0) && !f142::powerOfTwo(3) && !f142::powerOfTwo(48),
              "F142: the table-full line is written at 1, 2, 4, 8, ... draws");
    }
    {   // the flight's order: the label configurations and the discs came first, then the weapon readout parts
        IconTable t;
        for (int k = 0; k < 6; ++k) check(t.draw(k, 1000 + k, k == 1 || k == 2 || k == 3 ? f142::kIconRefused : f142::kIconDecoded) == k, "F142: the first six configurations take slots 0 to 5");
        // keep drawing those six every frame, as the game does; the weapon's readout quad appears at 16:07:37
        for (uint32_t f = 1010; f < 1100; ++f)
            for (int k = 0; k < 6; ++k) t.draw(k, f);
        const int quad = t.draw(100, 1100);
        check(quad == 6 && t.reused == 0 && t.full == 0, "F142: with six slots taken the readout quad takes a free one (the old table of six would have returned -1)");
        check(t.draw(100, 1101) == 6 && t.draw(101, 1101) == 7 && t.draw(102, 1101) == 8, "F142: the quad keeps its slot, and the next parts take the next free ones");
    }
    {   // many other configurations first, however many: a readout still gets a slot once the earlier ones have aged out
        IconTable t;
        for (int k = 0; k < 100; ++k) t.draw(k, 10 + static_cast<uint32_t>(k));   // 100 configurations, each drawn once: the table fills and then reuses aged slots only
        check(t.reused > 0, "F142: a hundred one-off configurations reuse slots (each had aged 60 frames when its slot was taken)");
        // the 32 most recent are the live ones; at frame 500 every slot is old
        const int quad = t.draw(1000, 500);
        check(quad >= 0 && t.cfg[quad] == 1000, "F142: the weapon readout quad gets a slot after any number of earlier configurations");
        // a config that stops being drawn and returns re-reads (a new slot), which is correct: nothing points at the old one
        IconTable busy;
        for (int k = 0; k < f142::kIconMeshes; ++k) busy.draw(k, 100);
        check(busy.draw(500, 130) == -1 && busy.full == 1, "F142: 32 configurations all drawn in the last 60 frames: no slot (the bound the size is chosen for)");
        for (int k = 0; k < f142::kIconMeshes; ++k) busy.draw(k, 150);   // still drawing
        check(busy.draw(500, 170) == -1 && busy.full == 2, "F142: configurations that keep drawing keep their slots");
        check(busy.draw(500, 211) == 0 && busy.reused == 1, "F142: 61 frames after the last draw the oldest slot (the lowest of a tie) is the quad's");
        busy.pinned[1] = true;
        check(busy.draw(501, 212) == 2 && busy.cfg[1] == 1 && busy.reused == 2, "F142: a pinned slot (a record or the staging ring names it) is skipped: the next oldest is the victim");
        // a copy in flight is not reusable until iconOnData has set its state
        IconTable fl;
        fl.draw(1, 10, f142::kIconInFlight);
        for (int k = 2; k < f142::kIconMeshes + 1; ++k) fl.draw(k, 10);
        check(fl.draw(99, 500) != 0, "F142: the slot with a copy in flight is not taken even when it is the oldest");
    }

    // ---- 2. the full outfit promotion -------------------------------------------------------------------------------------------------------------------------------------
    check(f142::kPartCount == edvr::avh::kPartCount, "F142: the swap record holds every part of the avatar's table");
    {
        check(f142::promotedClass(2, false, false) == 1, "F142: a shadow-views-only part (state 2) is every view for the call");
        check(f142::promotedClass(0, false, false) == 0 && f142::promotedClass(1, false, false) == 1 && f142::promotedClass(3, false, false) == 3 && f142::promotedClass(9, false, false) == 9,
              "F142: never, every view, main views only and an unknown class are left alone");
        check(f142::promotedClass(2, true, false) == 2 && f142::promotedClass(3, true, false) == 3 && f142::promotedClass(1, true, false) == 1,
              "F142: a head part is never promoted (F95c's swap owns it)");
        check(f142::promotedClass(2, false, true) == 2 && f142::promotedClass(1, false, true) == 1, "F142: the first-person skeleton is never promoted");
        const uint32_t fps = static_cast<uint32_t>(edvr::avh::partIndexByName("FirstPersonSkeleton"));
        const uint32_t gearBack = static_cast<uint32_t>(edvr::avh::partIndexByName("EVASuit_Gear_Back"));
        const uint32_t lowerArmR = static_cast<uint32_t>(edvr::avh::partIndexByName("EVASuit_Gear_LowerArmR"));
        const uint32_t upperArms = static_cast<uint32_t>(edvr::avh::partIndexByName("EVASuit_Gear_UpperArms"));
        const uint32_t suitArms = static_cast<uint32_t>(edvr::avh::partIndexByName("EVASuit_Arms"));
        const uint32_t helmet = static_cast<uint32_t>(edvr::avh::partIndexByName("EVASuit_Helmet"));
        check(fps < f142::kPartCount && gearBack < f142::kPartCount && lowerArmR < f142::kPartCount && upperArms < f142::kPartCount && suitArms < f142::kPartCount &&
                  helmet < f142::kPartCount && !edvr::avh::isHeadPart(fps) && !edvr::avh::isHeadPart(gearBack) && !edvr::avh::isHeadPart(lowerArmR) &&
                  edvr::avh::isHeadPart(helmet) && edvr::avh::isHeadPart(0) && lowerArmR < gearBack,
              "F142: the parts the A/B names are in the part table; the helmet and the head are head parts, the skeleton and the coat's Gear_Back are not");
        // the avatar of the coat outfit: the game gave class 2 to Gear_Back, the skeleton and the head parts; 1 to some, 3 to some
        int32_t cls[f142::kPartCount];
        for (uint32_t i = 0; i < f142::kPartCount; ++i) cls[i] = 1;
        cls[gearBack] = 2;
        cls[fps] = 2;
        cls[0] = 2;             // Head
        cls[helmet] = 2;
        cls[upperArms] = 3;     // main views only
        cls[suitArms] = 0;      // never
        cls[lowerArmR] = 2;
        f142::OutfitSwap s;
        const auto read = [&](uint32_t i) { return cls[i]; };
        const auto cas = [&](uint32_t i, int32_t desired, int32_t expected) {
            if (cls[i] != expected) return false;
            cls[i] = desired;
            return true;
        };
        f142::promoteClasses(s, fps, &edvr::avh::isHeadPart, read, cas);
        check(s.n == 2 && s.part[0] == lowerArmR && s.part[1] == gearBack && s.was[0] == 2 && s.set[0] == 1 && cls[gearBack] == 1 && cls[lowerArmR] == 1,
              "F142: of the class-2 parts only the non-head, non-skeleton ones are promoted (Gear_Back, LowerArmR), recorded in order");
        check(cls[fps] == 2 && cls[0] == 2 && cls[helmet] == 2 && cls[upperArms] == 3 && cls[suitArms] == 0, "F142: the skeleton, the head parts, state 3 and state 0 are untouched");
        // the game changes the class of Gear_Back to 3 during its call: that value stands, the other goes back
        cls[gearBack] = 3;
        const uint32_t lost = f142::restoreClasses(s, cas);
        check(lost == 1 && cls[gearBack] == 3 && cls[lowerArmR] == 2, "F142: restoring puts the game's class back, and leaves a class the game changed during the call (counted)");
        // the next call promotes the same parts again (the swap is per call and leaves nothing)
        f142::OutfitSwap s2;
        cls[gearBack] = 2;
        f142::promoteClasses(s2, fps, &edvr::avh::isHeadPart, read, cas);
        const uint32_t lost2 = f142::restoreClasses(s2, cas);
        check(s2.n == 2 && lost2 == 0 && cls[gearBack] == 2 && cls[lowerArmR] == 2 && cls[fps] == 2, "F142: a clean call restores every class");
        // the game writes a class between our read and our compare-exchange: refused, left alone, not recorded
        f142::OutfitSwap s3;
        const auto racing = [&](uint32_t i, int32_t desired, int32_t expected) {
            if (i == gearBack) cls[gearBack] = 3;   // the game's write lands first
            if (cls[i] != expected) return false;
            cls[i] = desired;
            return true;
        };
        f142::promoteClasses(s3, fps, &edvr::avh::isHeadPart, read, racing);
        check(s3.n == 1 && s3.part[0] == lowerArmR && s3.refused == 1 && cls[gearBack] == 3, "F142: a class the game changed between our read and write is left alone and counted as refused");
        // an outfit whose only shadow-only parts are the head and the skeleton: nothing promoted
        int32_t plain[f142::kPartCount];
        for (uint32_t i = 0; i < f142::kPartCount; ++i) plain[i] = 1;
        for (uint32_t h : edvr::avh::kHeadParts) plain[h] = 2;
        plain[fps] = 2;
        f142::OutfitSwap s4;
        f142::promoteClasses(s4, fps, &edvr::avh::isHeadPart, [&](uint32_t i) { return plain[i]; },
                             [&](uint32_t i, int32_t d, int32_t e) {
                                 if (plain[i] != e) return false;
                                 plain[i] = d;
                                 return true;
                             });
        check(s4.n == 0 && s4.refused == 0, "F142: an avatar whose only shadow-only parts are the head and the skeleton promotes nothing");
        // every non-head part in state 2 is promoted: 54 - 12 head - 1 skeleton
        int32_t every[f142::kPartCount];
        for (uint32_t i = 0; i < f142::kPartCount; ++i) every[i] = 2;
        f142::OutfitSwap s5;
        f142::promoteClasses(s5, fps, &edvr::avh::isHeadPart, [&](uint32_t i) { return every[i]; },
                             [&](uint32_t i, int32_t d, int32_t e) {
                                 if (every[i] != e) return false;
                                 every[i] = d;
                                 return true;
                             });
        check(s5.n == f142::kPartCount - edvr::avh::kHeadPartCount - 1, "F142: every other part of an all-shadow avatar is promoted (41 of 54)");
    }

    // ---- 3. the outfit census ---------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(std::strcmp(f142::classText(0), "never") == 0 && std::strcmp(f142::classText(1), "every view") == 0 && std::strcmp(f142::classText(2), "shadow views only") == 0 &&
                  std::strcmp(f142::classText(3), "main views only") == 0 && std::strcmp(f142::classText(7), "unknown") == 0,
              "F142: the class as the log says it");
        f142::Census a;
        a.amc = 0x1000;
        a.n = 3;
        a.row[0].idx = 27; a.row[0].inst = 1; a.row[0].state = 1; a.row[0].flags = 5; a.row[0].current = 11; a.row[0].pending = -1;
        a.row[1].idx = 47; a.row[1].inst = 3; a.row[1].state = 2; a.row[1].flags = 5; a.row[1].current = 12; a.row[1].pending = -1; a.row[1].masks[0] = 0x180000000ull;
        a.row[2].idx = 28; a.row[2].inst = 1; a.row[2].state = 1; a.row[2].flags = 5; a.row[2].current = 13; a.row[2].pending = -1;
        f142::Census b = a;
        b.row[1].masks[0] = 0;
        b.row[1].masks[1] = 0xFFull;
        check(f142::censusSig(a) == f142::censusSig(b), "F142: the masks are not part of the census's signature (they come and go with the scene's views)");
        uint8_t ch[8];
        check(f142::changedParts(a, b, ch, 8) == 0, "F142: no part changed when only the masks did");
        b.row[1].state = 1;
        check(f142::censusSig(a) != f142::censusSig(b) && f142::changedParts(a, b, ch, 8) == 1 && ch[0] == 47, "F142: a part's class changing is a change, and names the part");
        b = a;
        b.row[2].current = 99;
        check(f142::censusSig(a) != f142::censusSig(b), "F142: the current item changing is a change");
        b = a;
        b.row[0].pending = 4;
        check(f142::censusSig(a) != f142::censusSig(b), "F142: the pending item changing is a change");
        b = a;
        b.row[0].flags = 0x15;
        check(f142::censusSig(a) != f142::censusSig(b), "F142: the flags changing is a change");
        b = a;
        b.row[1].inst = 1;
        check(f142::censusSig(a) != f142::censusSig(b), "F142: an instance appearing or going is a change");
        b = a;
        b.amc = 0x2000;
        check(f142::censusSig(a) != f142::censusSig(b), "F142: another avatar is a change");
        b = a;
        b.row[2].idx = 31;   // EVASuit_Arms (28) has no instance any more, EVASuit_Feet (31) has one
        const uint32_t n = f142::changedParts(a, b, ch, 8);
        check(n == 2 && ((ch[0] == 31 && ch[1] == 28) || (ch[0] == 28 && ch[1] == 31)), "F142: a part that vanished and one that appeared are both named");
        uint8_t one[1];
        check(f142::changedParts(a, b, one, 1) == 2, "F142: the count is the full count when the list is short");
    }

    // ---- 4. F141: the lanes used ------------------------------------------------------------------------------------------------------------------------------------------
    {
        float m[16];
        const double pos[3] = {1.0, 2.0, 3.0};
        f141test::rigid(10.0, 5.0, pos, m);
        check(f141::finiteUsed(m) && f141::finitePad(m), "F142: a rigid matrix is finite in the lanes used and in the padding");
        const float nan = std::nanf("");
        for (int lane : {3, 7, 11, 15}) {
            float x[16];
            std::memcpy(x, m, sizeof(x));
            x[lane] = nan;
            check(f141::finiteUsed(x) && !f141::finitePad(x), "F142: a NaN in a padding lane (3, 7, 11, 15) passes the used-lane test and is seen by the padding test");
        }
        for (int lane : {0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14}) {
            float x[16];
            std::memcpy(x, m, sizeof(x));
            x[lane] = nan;
            float y[16];
            std::memcpy(y, m, sizeof(y));
            y[lane] = INFINITY;
            check(!f141::finiteUsed(x) && f141::finitePad(x) && !f141::finiteUsed(y), "F142: a NaN or an infinity in a lane used (the axes, the position at 12..14) fails the used-lane test");
        }
        // the raw floats' text: %.6g, a NaN as nan, row by row
        char text[400];
        float id[16] = {};
        id[0] = id[5] = id[10] = id[15] = 1.0f;
        id[3] = nan;
        id[12] = 0.1f;
        id[13] = -2.5f;
        id[14] = 123456789.0f;
        id[7] = INFINITY;
        id[11] = -INFINITY;
        f141::formatMat16(id, text, sizeof(text));
        check(std::strcmp(text, "[1 0 0 nan | 0 1 0 inf | 0 0 1 -inf | 0.1 -2.5 1.23457e+08 1]") == 0, "F142: the raw 16 floats as %.6g, row by row, a NaN as nan");
        char tiny[30];
        f141::formatMat16(id, tiny, sizeof(tiny));
        check(std::strlen(tiny) < sizeof(tiny) && tiny[0] == '[', "F142: the raw floats' text never overruns a short buffer");
        char one[16];
        check(f141::formatFloat(0.25f, one, sizeof(one)) == 4 && std::strcmp(one, "0.25") == 0 && f141::formatFloat(nan, one, sizeof(one)) == 3 && std::strcmp(one, "nan") == 0,
              "F142: one float: 0.25 and nan");
    }
    {   // the bend: a NaN in padding lane 3 of E is used; in lane 12 it is rejected, and every reason is its own
        f141::BendIn in;
        const double eyePos[3] = {0.0, 1.7, 0.0};
        f141test::rigid(0.0, 0.0, eyePos, in.E);
        in.eOk = in.sOk = in.wOk = in.okRead = in.ok = in.aimInOk = in.aimOutOk = true;
        in.w = 1.0f;
        in.S[0] = 0.0f; in.S[1] = 1.711f; in.S[2] = 0.35f;
        in.aimIn[2] = 1.0f;
        in.aimOut[1] = 0.031f; in.aimOut[2] = 0.9995f;
        const float nan = std::nanf("");
        f141::BendOut ok = f141::bend(in);
        check(ok.geo && ok.why == f141::kWhyOk && !ok.ePad, "F142: a clean sample is usable, no padding NaN");
        f141::BendIn x = in;
        x.E[3] = nan;
        f141::BendOut b = f141::bend(x);
        check(b.geo && b.applied && b.why == f141::kWhyOk && b.ePad && f141test::close141(b.eyeUp, 0.011, 1e-5),
              "F142: E with a NaN in lane 3 is used (geometry, the bend applied) and counted as padding-only non-finite");
        x = in;
        x.E[7] = nan; x.E[11] = nan; x.E[15] = nan;
        check(f141::bend(x).geo && f141::bend(x).ePad, "F142: lanes 7, 11 and 15 as well");
        x = in;
        x.E[12] = nan;
        b = f141::bend(x);
        check(!b.geo && b.why == f141::kWhyENonFinite && !b.ePad, "F142: E with a NaN in lane 12 (the position) is rejected, as non-finite in a used lane, not as a padding lane");
        x = in;
        x.E[5] = nan;
        check(!f141::bend(x).geo && f141::bend(x).why == f141::kWhyENonFinite, "F142: ... or in a row of the axes");
        x = in;
        x.eOk = false;
        check(!f141::bend(x).geo && f141::bend(x).why == f141::kWhyENotRead, "F142: E not read");
        x = in;
        x.sOk = false;
        check(!f141::bend(x).geo && f141::bend(x).why == f141::kWhySNotRead, "F142: S not read (the call or its reads failed)");
        x = in;
        x.sOk = false;
        x.ok = false;
        check(f141::bend(x).why == f141::kWhyHandle, "F142: S not asked for because the game's sight handle is not valid: its own reason, not a rejection");
        x = in;
        x.S[1] = nan;
        check(!f141::bend(x).geo && f141::bend(x).why == f141::kWhySNonFinite, "F142: S non-finite");
        x = in;
        for (int i = 0; i < 4; ++i) x.E[4 + i] = 0.0f;   // the y row has no length
        check(!f141::bend(x).geo && f141::bend(x).why == f141::kWhyRow, "F142: a degenerate row of E");
        x = in;
        x.S[0] = static_cast<float>(eyePos[0]); x.S[1] = static_cast<float>(eyePos[1]) + 5e-7f; x.S[2] = static_cast<float>(eyePos[2]);
        check(!f141::bend(x).geo && f141::bend(x).why == f141::kWhySOnE, "F142: |S - E| under 1e-6 m has no ray");
        x.S[1] = static_cast<float>(eyePos[1]) + 3e-6f;
        check(f141::bend(x).geo && f141::bend(x).why == f141::kWhyOk, "F142: ... and 3e-6 m has");
        x = in;
        x.eOk = false;
        x.sOk = false;
        check(f141::bend(x).why == f141::kWhyENotRead, "F142: both unread: E's reason comes first");
        check(std::strcmp(f141::whyText(f141::kWhyOk), "usable") == 0 && std::strcmp(f141::whyText(f141::kWhySOnE), "|S - E| under 1 um") == 0 && std::strcmp(f141::whyText(99), "?") == 0,
              "F142: the reasons' text");
    }
    {   // the window: ADS-on samples by reason, the rejected sum without the game's invalid handle, and the two candidate positions
        f141::BendIn in;
        const double eyePos[3] = {0.0, 1.7, 0.0};
        f141test::rigid(0.0, 0.0, eyePos, in.E);
        in.eOk = in.sOk = in.wOk = in.okRead = in.ok = true;
        in.w = 1.0f;
        in.S[1] = 1.711f; in.S[2] = 0.35f;
        const float nan = std::nanf("");
        f141::Acc a;
        f141::P14Off none;
        const auto add = [&](bool ads, const f141::BendIn& i) { a.add(ads, i, f141::bend(i), false, none, 0); };
        add(true, in);
        f141::BendIn pad = in;
        pad.E[3] = nan;
        add(true, pad);
        add(true, pad);
        f141::BendIn used = in;
        used.E[12] = nan;
        add(true, used);
        f141::BendIn ne = in;
        ne.eOk = false;
        add(true, ne);
        add(true, ne);
        f141::BendIn ns = in;
        ns.sOk = false;
        add(true, ns);
        f141::BendIn nh = in;
        nh.sOk = false;
        nh.ok = false;
        add(true, nh);
        add(false, ne);   // ADS handling off: counted apart, not by reason
        check(a.samples == 9 && a.adsOff == 1 && a.why[f141::kWhyOk] == 3 && a.why[f141::kWhyENotRead] == 2 && a.why[f141::kWhyENonFinite] == 1 && a.why[f141::kWhySNotRead] == 1 &&
                  a.why[f141::kWhyHandle] == 1 && a.why[f141::kWhySNonFinite] == 0 && a.ePadN == 2,
              "F142: the window counts the ADS-on samples by reason (an ADS-off call apart) and the samples whose E has a padding NaN");
        check(a.rejected() == 4, "F142: rejected = every reason but usable and the game's invalid handle: 2 + 1 + 1 = 4 of 8");
        a.reset();
        check(a.why[f141::kWhyOk] == 0 && a.ePadN == 0 && a.rejected() == 0 && a.posPairs == 0, "F142: the window resets the counts");

        // E's position as row 3 (used) or column 3: with P14w at (10.2, 1.65, 5.1)
        float froot[16];
        const double rootPos[3] = {10.0, 0.0, 5.0};
        f141test::rigid(0.0, 0.0, rootPos, froot);
        const double p14w[3] = {10.2, 1.65, 5.1};
        float S[3] = {10.2f, 1.7f, 5.4f};
        float E[16];
        f141test::rigid(0.0, 0.0, p14w, E);   // position in row 3, near P14
        f141::P14Off p = f141::p14Offset(S, E, p14w, froot);
        check(p.ok && f141test::close141(p.rowDist, 0.0, 1e-6) && p.colDist >= 0.0 && p.colDist > 0.5, "F142: E's position in row 3 is near P14; the column 3 reading (0, 0, 0) is far");
        f141::Acc b;
        f141::BendIn ein = in;
        std::memcpy(ein.E, E, sizeof(E));
        b.add(true, ein, f141::bend(ein), true, p, 0);
        check(b.posPairs == 1 && b.posRowNear == 1 && b.posColNear == 0, "F142: the window counts which candidate position lies within 50 cm of P14w: row 3");
        float Ec[16] = {};
        Ec[0] = Ec[5] = Ec[10] = 1.0f;
        Ec[3] = 10.2f; Ec[7] = 1.65f; Ec[11] = 5.1f;   // the position in column 3, row 3 empty
        Ec[12] = Ec[13] = Ec[14] = 0.0f;
        f141::P14Off pc = f141::p14Offset(S, Ec, p14w, froot);
        f141::BendIn cin = in;
        std::memcpy(cin.E, Ec, sizeof(Ec));
        b.add(true, cin, f141::bend(cin), true, pc, 0);
        check(pc.ok && b.posPairs == 2 && b.posRowNear == 1 && b.posColNear == 1, "F142: ... and column 3 when the position is there");
        float En[16];
        std::memcpy(En, E, sizeof(En));
        En[3] = nan; En[7] = nan; En[11] = nan;
        f141::P14Off pn = f141::p14Offset(S, En, p14w, froot);
        check(pn.ok && pn.rowDist >= 0.0 && pn.colDist < 0.0, "F142: column 3 NaN: the column reading is not a number (-1), the row reading stands");
        E[13] = nan;
        check(!f141::p14Offset(S, E, p14w, froot).ok, "F142: p14Offset needs E's used lanes too");
    }
    {   // the readout's rel: U's used lanes, the frame's rows, and the window's counts
        float W[16], U[16];
        const double wPos[3] = {4.0, 1.3, -2.0}, uPos[3] = {4.05, 1.33, -1.88};
        f141test::rigid(35.0, -12.0, wPos, W);
        f141test::rigid(35.0, -12.0, uPos, U);
        double Wd[16], Pd[16];
        for (int i = 0; i < 16; ++i) Wd[i] = W[i];
        for (int i = 0; i < 16; ++i) Pd[i] = W[i];
        const float nan = std::nanf("");
        f141::Rel r = f141::relTo(U, Wd);
        check(r.ok && r.why == f141::kRelOk && !r.padNonFinite, "F142: a clean U makes a rel");
        float Up[16];
        std::memcpy(Up, U, sizeof(Up));
        Up[3] = nan; Up[7] = nan; Up[11] = nan; Up[15] = nan;
        r = f141::relTo(Up, Wd);
        check(r.ok && r.padNonFinite && f141test::close141(r.t[0], f141::relTo(U, Wd).t[0], 1e-9), "F142: U with NaN in every padding lane makes the same rel and is counted");
        Up[12] = nan;
        r = f141::relTo(Up, Wd);
        check(!r.ok && r.why == f141::kRelUNonFinite && !r.padNonFinite, "F142: U with a NaN in lane 12 is rejected as non-finite in a used lane");
        float Ud[16];
        std::memcpy(Ud, U, sizeof(Ud));
        for (int i = 4; i < 8; ++i) Ud[i] = 0.0f;
        check(!f141::relTo(Ud, Wd).ok && f141::relTo(Ud, Wd).why == f141::kRelUDegenerate, "F142: a U row with no length is a degenerate U row");
        double Wz[16];
        std::memcpy(Wz, Wd, sizeof(Wz));
        for (int i = 8; i < 12; ++i) Wz[i] = 0.0;
        check(!f141::relTo(U, Wz).ok && f141::relTo(U, Wz).why == f141::kRelFDegenerate, "F142: a frame row with no length is the frame's, not U's");
        f141::RelCounts c;
        const f141::Rel good = f141::relTo(U, Wd);
        c.note(good, good);
        c.note(f141::relTo(Up, Wd), f141::relTo(Up, Pd));
        c.note(f141::relTo(Ud, Wd), f141::relTo(Ud, Pd));
        c.note(f141::relTo(U, Wz), good);
        f141::Rel padOnly = f141::relTo(U, Wd);
        padOnly.padNonFinite = true;
        c.note(padOnly, padOnly);
        check(c.rows == 5 && c.usable == 2 && c.rejected() == 3 && c.uNonFinite == 1 && c.uDegenerate == 1 && c.weaponFrame == 1 && c.p14Frame == 0 && c.padNonFinite == 1,
              "F142: the readout window counts rows, usable rels, and each reason");
        c.reset();
        check(c.rows == 0 && c.rejected() == 0, "F142: the readout counts reset");
    }
}

}  // namespace f142test
