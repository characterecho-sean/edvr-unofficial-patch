// F119 checks (native on-foot VR, measure only): head containment. Header only, run by native_frame_test (run119). The head's offset from the rest point (the
// physical head from the pose and the alignment it carried), the second's percentiles and histogram by class, EDVR's own eye offsets kept apart, the depth grid
// decoded from words packed in the shader's layout (cells from the CPU procedure the shader runs, over a wall and over the player's own geometry), the baseline and the
// contact tallies, and the lines (each within the log's limit).
#pragma once

#include "../../src/d3d11/native_on_foot_f119.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace f119test {

namespace f119 = edvr::nofoot::f119;

inline uint32_t floatBits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}
inline bool has(const char* text, const char* needle) { return std::strstr(text, needle) != nullptr; }

// Packs the cells of a depth image the way head_containment_shader.h writes one eye's words.
inline std::vector<uint32_t> packEye(const std::vector<float>& depth, const std::vector<uint8_t>* stencil, uint32_t W, uint32_t H, double A, double B, uint32_t seq, uint32_t mask) {
    std::vector<uint32_t> w(f119::kEyeWords, 0u);
    w[0] = seq;
    w[1] = W;
    w[2] = H;
    w[3] = mask;
    for (uint32_t cy = 0; cy < f119::kGrid; ++cy)
        for (uint32_t cx = 0; cx < f119::kGrid; ++cx) {
            const f119::Cell c = f119::cellCpu(depth.data(), stencil ? stencil->data() : nullptr, W, H, A, B, cx, cy);
            const uint32_t k = cy * f119::kGrid + cx;
            w[4 + 3 * k] = floatBits(c.world);
            w[5 + 3 * k] = floatBits(c.all);
            w[6 + 3 * k] = c.marked;
        }
    return w;
}

template <class Check>
inline void run119(Check&& check) {
    {   // the physical head: the pose plus the alignment it carried, in the seated space (right +x, up +y, forward -z)
        const float render[12] = {1, 0, 0, 0.10f, 0, 1, 0, 0.02f, 0, 0, 1, -0.30f};
        const double applied[3] = {0.01, -0.02, 0.05};
        const f119::Offset o = f119::physicalOffset(render, applied);
        check(std::fabs(o.right - 0.11) < 1e-6 && std::fabs(o.up - 0.0) < 1e-6 && std::fabs(o.forward - 0.25) < 1e-6 &&
                  std::fabs(o.horizontal() - std::sqrt(0.11 * 0.11 + 0.25 * 0.25)) < 1e-6,
              "F119: the physical head is the pose plus its alignment: right +0.11, up 0.00, forward +0.25 (z -0.30 + 0.05 negated), 27.3 cm horizontal");
        const double none[3] = {};
        const f119::Offset z = f119::physicalOffset(render, none);
        check(std::fabs(z.forward - 0.30) < 1e-6 && std::fabs(z.up - 0.02) < 1e-6, "F119: with no alignment the pose is the head: a head 30 cm forward of the origin has z -0.30");
    }
    {   // the alignment history: all three axes by serial; the word is serial + 1; 0 is a runtime that reports none
        f119::AlignHistory3 h;
        const double a[3] = {0.01, 0.17, -0.02}, b[3] = {-0.03, 0.15, 0.04};
        h.record(5, a);
        h.record(6, b);
        const f119::AlignHistory3::Found f6 = h.find(7), f5 = h.find(6), f0 = h.find(1), none = h.find(0), gone = h.find(99);
        check(f6.matched && f6.v[0] == -0.03 && f6.v[1] == 0.15 && f6.v[2] == 0.04 && f5.matched && f5.v[1] == 0.17 && f0.matched && f0.v[0] == 0.0 && f0.v[1] == 0.0 &&
                  !none.reported && !none.matched && gone.reported && !gone.matched,
              "F119: the history finds an alignment whole by the pose's word (serial + 1); serial 0 is the zero alignment; word 0 reports none; a serial it lacks is reported, not matched");
        h.record(5 + f119::kHistN, b);   // the slot is reused
        check(!h.find(6).matched && h.find(5 + f119::kHistN + 1).matched, "F119: a slot reused by a later serial no longer answers for the old one");
    }
    {   // the classes
        check(f119::classIndex(f119::kStanding, f119::kUnarmed, false) == 0 && f119::classIndex(f119::kCrouched, f119::kWeapon, true) == (1 * 3 + 1) * 2 + 1 &&
                  f119::classIndex(f119::kStanceUnknown, f119::kTool, true) == f119::kClasses - 1,
              "F119: 18 classes (stance x item x ADS), indexed in order");
        char n[48];
        f119::className(f119::classIndex(f119::kCrouched, f119::kWeapon, true), n, sizeof(n));
        char m[48];
        f119::className(f119::classIndex(f119::kStanceUnknown, f119::kTool, false), m, sizeof(m));
        check(std::string(n) == "crouched/weapon/ADS" && std::string(m) == "unknown/tool/hip", "F119: class names read stance/item/ADS or hip");
        check(f119::stanceOf(1, 1000, 1500, 1000) == f119::kCrouched && f119::stanceOf(0, 1000, 1500, 1000) == f119::kStanding && f119::stanceOf(2, 1000, 1500, 1000) == f119::kStanceUnknown &&
                  f119::stanceOf(1, 1000, 2001, 1000) == f119::kStanceUnknown && f119::stanceOf(1, 0, 1500, 1000) == f119::kStanceUnknown && f119::stanceOf(1, 2000, 1500, 1000) == f119::kStanceUnknown,
              "F119: the stance is the pin's while it is under a second old; older, unpublished or from the future it is not known");
        check(f119::itemOf(true, true) == f119::kWeapon && f119::itemOf(false, true) == f119::kTool && f119::itemOf(false, false) == f119::kUnarmed && f119::itemOf(true, false) == f119::kWeapon,
              "F119: a weapon is a weapon, anything else drawn a tool, nothing drawn unarmed");
    }
    {   // a second's percentiles by nearest rank, and its histogram: 100 samples at 1..100 cm in one class
        f119::OffsetSecond s;
        const int c = f119::classIndex(f119::kStanding, f119::kUnarmed, false);
        for (int i = 1; i <= 100; ++i) {
            f119::Offset o;
            o.right = i * 0.01 * 0.6;
            o.forward = i * 0.01 * 0.8;   // a 3-4-5 split: the horizontal distance is i cm
            o.up = -0.01 * i;
            s.add(c, o);
        }
        f119::ClassOut out;
        const bool got = s.summarize(c, &out);
        uint32_t total = 0;
        for (int i = 0; i < f119::kBins; ++i) total += out.hist[i];
        // bins: <1 none; [1,2) holds 1; ... [5,6) 5; [6,8) 6 7; [8,10) 8 9; [10,12) 10 11; [12,15) 12 13 14; [15,20) 5 values; [20,25) 5; [25,30) 5; [30,40) 10; [40,60) 20; [60,100) 40; 100+ one
        check(got && out.n == 100 && std::fabs(out.p50 - 50.0) < 1e-4 && std::fabs(out.p95 - 95.0) < 1e-4 && std::fabs(out.max - 100.0) < 1e-4 && total == 100 && out.hist[0] == 0 &&
                  out.hist[1] == 1 && out.hist[5] == 1 && out.hist[6] == 2 && out.hist[7] == 2 && out.hist[8] == 2 && out.hist[9] == 3 && out.hist[10] == 5 && out.hist[13] == 10 &&
                  out.hist[14] == 20 && out.hist[15] == 40 && out.hist[16] == 1,
              "F119: 100 samples at 1..100 cm: p50 50, p95 95, max 100 (nearest rank), the histogram's bins [1,2) 1 ... [60,100) 40, 100 and over 1");
        check(std::fabs(out.rMax - 60.0) < 1e-3 && std::fabs(out.rMin - 0.6) < 1e-3 && std::fabs(out.fMax - 80.0) < 1e-3 && std::fabs(out.uMin + 100.0) < 1e-3 && std::fabs(out.uMax + 1.0) < 1e-3,
              "F119: the components' ranges in cm: right 0.6..60, forward 0.8..80, up -100..-1");
        check(!s.summarize(f119::classIndex(f119::kCrouched, f119::kTool, true), &out) && std::fabs(s.maxHorizontalCm() - 100.0) < 1e-4, "F119: a class with no samples has no summary; the second's largest offset is 100 cm");
        float v[5] = {5, 1, 4, 2, 3};
        check(f119::nearestRank(v, 5, 0.5) == 3.0 && f119::nearestRank(v, 5, 0.95) == 5.0 && f119::nearestRank(v, 5, 0.0) == 1.0 && f119::nearestRank(v, 0, 0.5) == 0.0,
              "F119: nearest rank of five values: 0.5 gives the third, 0.95 the fifth, 0 the first; no values give 0");
    }
    {   // two classes in a second: the most samples first; the cap counts what it drops
        f119::OffsetSecond s;
        const int a = f119::classIndex(f119::kStanding, f119::kWeapon, false), b = f119::classIndex(f119::kStanding, f119::kWeapon, true), d = f119::classIndex(f119::kCrouched, f119::kUnarmed, false);
        f119::Offset o;
        o.right = 0.04;
        for (int i = 0; i < 5; ++i) s.add(b, o);
        for (int i = 0; i < 70; ++i) s.add(a, o);
        for (int i = 0; i < 3; ++i) s.add(d, o);
        int top[2];
        const int classes = s.top(2, top);
        check(classes == 3 && top[0] == a && top[1] == b, "F119: three classes in a second; the two with the most samples come first (70 hip, 5 ADS; 3 crouched left out)");
        f119::OffsetSecond big;
        for (uint32_t i = 0; i < f119::kMaxSamples + 7; ++i) big.add(a, o);
        check(big.n == f119::kMaxSamples && big.dropped == 7, "F119: past the sample limit a frame is counted dropped, not kept");
    }
    {   // EDVR's own eye offsets, kept apart: the total's horizontal and up range, the terms' largest magnitude
        f119::OwnSecond w;
        const double total1[3] = {0.03, 0.17, 0.04}, lock1[3] = {0.01, 0.0, 0.02}, weapon1[3] = {0.0, 0.0, -0.10}, view1[3] = {0.02, 0.0, 0.0};
        const double total2[3] = {0.0, 0.15, 0.0}, none[3] = {};
        w.add(total1, lock1, weapon1, view1, 0.16, 0.01);
        w.add(total2, none, none, none, 0.12, -0.03);
        check(w.n == 2 && std::fabs(w.hMax - 0.05) < 1e-12 && std::fabs(w.upMin - 0.15) < 1e-12 && std::fabs(w.upMax - 0.17) < 1e-12 && std::fabs(w.lockH - std::sqrt(0.0005)) < 1e-12 &&
                  std::fabs(w.weaponH - 0.10) < 1e-12 && std::fabs(w.viewH - 0.02) < 1e-12 && w.neckUp == 0.16 && w.holdUp == -0.03,
              "F119: EDVR's own offsets: horizontal max 5 cm, up 15..17 cm, ADS lock 2.2 cm, weapon eye 10 cm, view 2 cm horizontal, the neck raise +16 and the hold -3 (largest magnitude)");
    }
    {   // the depth grid: a wall 15 cm away over the lower left quarter, the player's weapon at 18 cm (stencil bit set) over the lower right quarter, the rest sky
        constexpr uint32_t W = 160, H = 128;
        const double A = 0.0, B = 0.10;   // reversed-Z, infinite far: depth = B / w; the near plane 10 cm
        std::vector<float> depth(W * H, 0.0f);
        std::vector<uint8_t> stencil(W * H, 0);
        for (uint32_t y = H / 2; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                if (x < W / 2) depth[y * W + x] = static_cast<float>(B / 0.15);
                else {
                    depth[y * W + x] = static_cast<float>(B / 0.18);
                    stencil[y * W + x] = 0x10;
                }
            }
        const std::vector<uint32_t> words = packEye(depth, &stencil, W, H, A, B, 77, f119::kStencilMask);
        f119::EyeGrid g;
        const bool ok = f119::decodeEye(words.data(), 77, W, H, &g);
        const f119::Coverage c = f119::coverage(g, B);
        check(ok && g.mask == 0x10 && std::fabs(c.nearestWorld - 0.15) < 1e-5 && std::fabs(c.nearestAll - 0.15) < 1e-5 && c.within[0] == 0 && c.within[1] == 0 && c.within[2] == 16 &&
                  c.within[3] == 0 && c.all20 == 32 && c.markedCells == 16,   // the weapon's cells have no world tap: the world depth there is the sky
              "F119: a wall at 15 cm over 16 cells: 16 cells within 20 cm, none within 10 or 5, none within 3 cm of a 10 cm near plane; the weapon's 16 cells appear only with the player's own (32 within 20 cm)");
        f119::EyeGrid g2;
        const std::vector<uint32_t> wall12 = packEye(std::vector<float>(W * H, static_cast<float>(B / 0.12)), nullptr, W, H, A, B, 3, 0);
        const bool ok2 = f119::decodeEye(wall12.data(), 3, W, H, &g2);
        const f119::Coverage c2 = f119::coverage(g2, B);
        check(ok2 && g2.mask == 0 && c2.within[2] == 64 && c2.within[3] == 64 && c2.within[1] == 0 && c2.markedCells == 0 && std::fabs(c2.nearestWorld - 0.12) < 1e-5,
              "F119: a wall at 12 cm filling the view with no stencil view: all 64 cells within 20 cm and within 3 cm of the 10 cm near plane, none within 10 cm");
        const std::vector<uint32_t> cut = packEye(std::vector<float>(W * H, static_cast<float>(B / 0.10)), nullptr, W, H, A, B, 4, 0);
        f119::EyeGrid g3;
        f119::decodeEye(cut.data(), 4, W, H, &g3);
        const f119::Coverage c3 = f119::coverage(g3, B);
        check(c3.within[3] == 64 && c3.within[2] == 64 && c3.within[1] == 0 && c3.within[0] == 0 && std::fabs(c3.nearestWorld - 0.10) < 1e-5,
              "F119: the near plane cutting a wall reads at the near plane (10 cm): within 20 cm and within 3 cm of it, and not within 10 or 5 cm of the camera");
        f119::EyeGrid bad;
        check(!f119::decodeEye(words.data(), 78, W, H, &bad) && !f119::decodeEye(words.data(), 77, W + 1, H, &bad) && !f119::decodeEye(words.data(), 77, W, H + 1, &bad),
              "F119: words with another sequence or another texture size are refused");
        const std::vector<uint32_t> sky = packEye(std::vector<float>(W * H, 0.0f), nullptr, W, H, A, B, 5, 0);
        f119::EyeGrid g4;
        f119::decodeEye(sky.data(), 5, W, H, &g4);
        const f119::Coverage c4 = f119::coverage(g4, B);
        check(c4.nearestWorld > 1e29 && c4.within[2] == 0 && c4.all20 == 0, "F119: the sky (depth 0) is infinitely far: no coverage, no nearest depth");
        uint32_t px, py;
        f119::tapPixel(0, 0, 0, 0, 2016, 2017, &px, &py);
        uint32_t qx, qy;
        f119::tapPixel(7, 7, 7, 7, 2016, 2017, &qx, &qy);
        check(px == 15 && py == 15 && qx == 2000 && qy == 2001, "F119: the first tap of a 2016 x 2017 eye reads pixel (15, 15), the last (2000, 2001): taps at the centres of 64 x 64 sub-cells");
    }
    {   // a second's coverage, the baseline and the contact tallies
        f119::DepthSecond d;
        f119::Coverage c[2];
        c[0].within[2] = 0;
        c[1].within[2] = 2;
        c[1].all20 = 5;
        bool have[2] = {true, true};
        d.addFrame(c, have, true, 1.5);    // under 3 cm: the baseline, 2 cells within 20 cm (with the own 5)
        c[0].within[3] = 0;
        c[1].within[3] = 0;
        d.addFrame(c, have, true, 20.0);   // away: not the baseline
        c[1].within[3] = 3;
        d.addFrame(c, have, true, 30.0);   // 3 cells at the near plane: contact (1 cell) at 30 cm
        c[0].within[3] = 9;
        d.addFrame(c, have, true, 34.0);   // 9: contact (8 cells) at 34 cm
        d.addFrame(c, have, false, 2.0);   // no offset known: counted nowhere
        bool none[2] = {false, false};
        d.addFrame(c, none, true, 0.5);    // no eye read: counted nowhere
        check(d.baseFrames == 1 && d.baseCells20 == 2 && d.baseOwn20 == 5 && std::fabs(d.contactMin1 - 30.0) < 1e-9 && std::fabs(d.contactMin8 - 34.0) < 1e-9 &&
                  d.contactFrames == 0 && d.contactMin32 > 1e8,
              "F119: one frame at a head offset under 3 cm (the baseline: 2 cells within 20 cm, 5 with the player's own); contact first at 30 cm for 1 cell and 34 cm for 8; no frame has 32 cells");
        // F123's proof of a peek: the frames with 32 cells or more at the near plane, the weakest of them, the far cells seen on them
        c[1].within[3] = 40;
        c[1].far1m = 3;
        d.addFrame(c, have, true, 47.0);
        c[1].within[3] = 33;
        c[1].far1m = 9;
        c[0].within[3] = 2;
        c[0].far1m = 20;
        d.addFrame(c, have, true, 46.0);
        c[1].within[3] = 5;   // under 32: not a contact frame, its far cells not counted
        c[1].far1m = 50;
        c[0].within[3] = 5;
        c[0].far1m = 50;
        d.addFrame(c, have, true, 45.0);
        check(d.contactFrames == 2 && d.contactWeakest == 33 && d.contactFar == 20 && std::fabs(d.contactMin32 - 46.0) < 1e-9,
              "F123: two contact frames (40 and 33 cells), the weakest 33, the most far cells on them 20, first at 46 cm; a frame under 32 cells is none");
        f119::EyeGrid sky2;
        for (uint32_t i = 0; i < f119::kCells; ++i) sky2.world[i] = i < 16 ? 0.05f : i < 24 ? 1.5f : 1e30f;
        check(f119::coverage(sky2, 0.1).far1m == 48, "F123: far cells are the world's cells past 1 m, the sky included (16 cells at 5 cm, 8 at 1.5 m, 40 in the sky: 48 far)");
        f119::EyeSecond e;
        f119::Coverage k;
        k.nearestWorld = 0.4;
        k.nearestAll = 0.3;
        k.within[2] = 3;
        e.add(k, 0.1);
        k.nearestWorld = 0.2;
        k.within[2] = 1;
        e.add(k, 0.1);
        check(e.read == 2 && std::fabs(e.nearestWorld - 0.2) < 1e-12 && std::fabs(e.nearestAll - 0.3) < 1e-12 && e.within[2] == 3 && std::fabs(e.nearM - 0.1) < 1e-12,
              "F119: an eye's second keeps the nearest depths and the largest coverage");
    }
    {   // the lines: the shape, the log's limit, the fallbacks
        f119::OffsetSecond off;
        f119::OwnSecond own;
        f119::DepthSecond dep;
        const int a = f119::classIndex(f119::kStanding, f119::kWeapon, false), b = f119::classIndex(f119::kCrouched, f119::kUnarmed, true);
        for (int i = 0; i < 90; ++i) {
            f119::Offset o;
            o.right = 0.0123 * (i % 7);
            o.forward = 0.0231 * (i % 5);
            o.up = -0.0111 * (i % 3);
            off.add(i < 60 ? a : b, o);
        }
        off.frames = 90;
        const double t[3] = {0.011, 0.171, 0.022}, z[3] = {};
        own.add(t, z, z, z, 0.171, 0.0);
        f119::Coverage c[2];
        c[0].nearestWorld = 0.633;
        c[0].nearestAll = 0.21;
        c[1].nearestWorld = 0.65;
        c[1].nearestAll = 0.22;
        dep.eye[0].add(c[0], 0.12);
        dep.eye[1].add(c[1], 0.12);
        f119::LineIn li;
        li.frames = 90;
        li.noPose = 2;
        li.generation = 7;
        li.restAgeS = 12.4;
        li.recentres = 1;
        li.off = &off;
        li.own = &own;
        li.depth = &dep;
        li.costUs = 41.0;
        char line[1600];
        const int n = f119::formatLine(line, sizeof(line), li);
        check(n > 0 && n <= f119::kLineLimit && has(line, "native on foot head containment (F119) 1 s: frames 90 (no pose 2, over limit 0); rest point: generation 7, 12.4 s old, recentres 1;") &&
                  has(line, "[standing/weapon/hip n 60 h p50 ") && has(line, "[crouched/unarmed/ADS n 30 h p50 ") && has(line, ", hist ") && has(line, "L near 12.0, nearest 63.3 (body 21.0), cells 0/0/0/0") &&
                  has(line, "R near 12.0, nearest 65.0 (body 22.0)") && has(line, "skipped none;") && has(line, "smallest head offset: 1 cell none, 8 cells none, 32 cells none; contact frames (32 cells or more) 0, weakest 0 cells, far (over 1 m) up to 0 cells;") &&
                  has(line, "at head offset under 3 cm: frames 0, cells within 20 cm max 0 (body 0); cost 41 us a frame"),
              "F119: the 1 s line holds the frames, the rest point, both classes with histograms, EDVR's eye offsets, both eyes' depth, the skips, the contact and its proof of a peek, the baseline and the cost");
        check(n <= f119::kLineLimit, "F119: a usual second's line stays within the line limit with both classes' histograms (the log's note holds 1166 characters after its stamp)");
        // the worst case: three classes' worth of samples, every skip counted, large figures: the line stays within the log's limit, by dropping the histograms and then a class
        for (int i = 0; i < 40; ++i) {
            f119::Offset o;
            o.right = 0.5 + 0.0137 * i;
            o.forward = -0.4 - 0.0211 * i;
            o.up = -1.2 + 0.0345 * i;
            off.add(f119::classIndex(f119::kStanceUnknown, f119::kTool, true), o);
        }
        for (int s = 0; s < f119::kSkips; ++s) dep.skipped[s] = 12345;
        dep.contactMin1 = 123.4;
        dep.contactMin8 = 234.5;
        dep.baseFrames = 4000000000u;
        dep.baseCells20 = 64;
        dep.baseOwn20 = 64;
        dep.eye[0].within[0] = dep.eye[0].within[1] = dep.eye[0].within[2] = dep.eye[0].within[3] = 64;
        dep.eye[1].within[0] = dep.eye[1].within[1] = dep.eye[1].within[2] = dep.eye[1].within[3] = 64;
        li.frames = 123456u;
        li.noPose = 123456u;
        li.dropped = 123456u;
        li.generation = 123456u;
        li.recentres = 123456u;
        li.restAgeS = 99999.9;
        li.costUs = 123456.0;
        dep.baseFrames = 123456u;
        dep.contactFrames = 123456u;
        dep.contactWeakest = 64;
        dep.contactFar = 64;
        dep.contactMin32 = 345.6;
        char big[1600];
        const int nb = f119::formatLine(big, sizeof(big), li);
        check(nb > 0 && has(big, "native on foot head containment (F119) 1 s: frames 123456 (no pose 123456") && has(big, " classes (n "),
              "F119: a crowded second (a third class, every skip, six-digit figures) still gives one line naming the classes left out");
        check(nb <= 1166, "F119: the crowded line stays under the log's 1200-byte line with its stamp and mark");
    }
    {   // the once-per-session line and a move of the rest point
        char b[800];
        f119::formatSession(b, sizeof(b));
        check(has(b, "native on foot head containment (F119): measure only, nothing drawn changes; the commander's collision radius: not read;") && has(b, "stencil bit 0x10") && std::strlen(b) < 760,
              "F119: the session line says measure only, the collision radius not read, the grid and the stencil bit");
        char b2[800];
        f119::formatSession(b2, sizeof(b2), 0.35);
        check(has(b2, "the commander's collision radius: 0.350 m;"), "F119: a radius that was read is logged in metres");
        f119::Offset was;
        was.right = 0.12;
        was.forward = -0.09;
        was.up = -0.03;
        char r[700];
        f119::formatRecentre(r, sizeof(r), 7, 8, 2, true, was, 41.5, f119::kStanding, f119::kWeapon, false);
        check(has(r, "the rest point moved (reference generation 7 to 8; 2 on foot this session): the head was 15.0 cm horizontal and -3.0 cm up from the old rest point (right +12.0, forward -9.0), 41.5 s after the last move; standing, weapon, hip"),
              "F119: a move of the rest point names the generations, the head's offset from the old one and the class");
        char r2[700];
        f119::formatRecentre(r2, sizeof(r2), 7, 8, 1, false, was, 3.0, f119::kCrouched, f119::kTool, true);
        check(has(r2, "no head offset was sampled before it") && has(r2, "crouched, tool, ADS"), "F119: a move with no sample before it says so");
    }
}

}  // namespace f119test
