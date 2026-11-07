// F218 checks (native on-foot VR), run218 below. Header only, run by native_frame_test.
//
// F218 (native_on_foot_f218.h, native_on_foot.cpp): with the laser rifle at the hip a refusal of F82's rest solution holds it back (RestHold: 1 s, extended to 0.3 s ahead while the body moves), and a
// switch between the rest solution and the legacy one slides (Slide: the difference of the last move and this one becomes a horizontal offset that falls at 0.5 m/s, none over 20 cm). The rig runs the
// hold's timing, the slide's continuity, decay, jump limit, first call and dt clamp, the window's counts and line, and pins the source sites.
#pragma once

#include "../../src/d3d11/native_on_foot_f218.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f218test {

namespace f218 = edvr::nofoot::f218;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

template <class Check>
inline void run218(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    constexpr double kDt = 1.0 / 90.0;

    // ---- 1. the hold ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f218::RestHold h;
        check(!h.step(100, false) && !h.step(5000, true), "F218: with no refusal the rest solution is never held, standing or moving");
        h.refused(1000);
        bool still = true;
        for (uint64_t t = 1000; t < 2000; t += 11) still = still && h.step(t, false);
        check(still && !h.step(2000, false) && !h.step(2500, false), "F218: a refusal standing still holds the rest solution for 1 s and no longer");
        h.refused(10000);
        bool moving = true;
        for (uint64_t t = 10000; t < 14000; t += 11) moving = moving && h.step(t, true);   // 4 s on the move
        check(moving, "F218: while the body moves the hold keeps being extended, a run stays on the legacy solution");
        // stops at 14000 (the last step in the loop was at 13993 or so): the hold runs on to 0.3 s after the last moving call
        const uint64_t lastMoving = 10000 + (3999 / 11) * 11;
        check(h.step(lastMoving + 200, false) && !h.step(lastMoving + f218::kHoldWhileMovingMs, false), "F218: it comes back 0.3 s after the last moving call, the body having stopped");
        h.refused(20000);
        check(h.step(20500, true) && h.step(20999, false) && !h.step(21000, false), "F218: moving early in the hold changes nothing: 0.3 s ahead is inside the second");
        h.refused(22000);
        check(h.step(22800, true) && h.step(23099, false) && !h.step(23100, false), "F218: moving late in the hold extends it to 0.3 s ahead of the last moving call");        h.refused(30000);
        h.reset();
        check(!h.step(30001, false) && !h.step(30001, true), "F218: reset forgets the hold");
        f218::RestHold k;
        k.refused(1000);
        check(k.step(1999, false) && !k.step(2000, false) && !f218::RestHold{}.step(0, true), "F218: the hold ends at exactly 1000 ms after the refusal and a fresh one holds nothing");
    }

    // ---- 2. the slide -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        double out[2];
        f218::Slide s;
        s.step(false, 0.10, 0.02, kDt, out);
        check(out[0] == 0.0 && out[1] == 0.0 && !s.switched && s.offsetM == 0.0, "F218: the first call only remembers");
        s.step(false, 0.11, 0.02, kDt, out);
        check(out[0] == 0.0 && !s.switched, "F218: calls on the same solution make no offset whatever the moves");
        // a switch: the rest solution moves the body 8 cm back
        const double lastX = 0.11, lastZ = 0.02, newX = 0.03, newZ = 0.02;
        s.step(true, newX, newZ, kDt, out);
        check(s.switched && eq(newX + out[0], lastX, 1e-12) && eq(newZ + out[1], lastZ, 1e-12) && eq(s.offsetM, 0.08, 1e-12),
              "F218: a switch makes no jump: the new move plus the offset is the last move");
        // the offset falls at 0.5 m/s
        double prev = s.offsetM;
        bool falls = true;
        for (int i = 0; i < 10; ++i) {
            s.step(true, newX, newZ, kDt, out);
            falls = falls && eq(prev - s.offsetM, 0.5 * kDt, 1e-12) && !s.switched;
            prev = s.offsetM;
        }
        check(falls && out[0] > 0.0 && eq(out[0], 0.08 - 10 * 0.5 * kDt, 1e-12), "F218: the offset falls at 0.5 m/s, keeping its direction");
        bool gone = false;
        for (int i = 0; i < 100 && !gone; ++i) {
            s.step(true, newX, newZ, kDt, out);
            gone = out[0] == 0.0 && out[1] == 0.0;
        }
        check(gone && s.offsetM == 0.0, "F218: and it reaches zero, not past it");
        // a switch back: the body goes back, the offset is the negative of the step
        s.step(false, 0.11, 0.02, kDt, out);
        check(s.switched && eq(0.11 + out[0], newX, 1e-12) && out[0] < 0.0, "F218: a switch the other way slides the other way");
        // direction kept when the move has a sideways part
        f218::Slide d;
        d.step(false, 0.0, 0.0, kDt, out);
        d.step(true, -0.06, -0.08, kDt, out);   // a difference of (0.06, 0.08): 10 cm
        const double ratio = out[0] / out[1];
        d.step(true, -0.06, -0.08, kDt, out);
        check(eq(ratio, 0.75, 1e-9) && eq(out[0] / out[1], 0.75, 1e-9) && eq(d.offsetM, 0.10 - 0.5 * kDt, 1e-12), "F218: the offset keeps its direction as its length falls");
        // a jump over 20 cm makes no offset
        f218::Slide j;
        j.step(false, 0.0, 0.0, kDt, out);
        j.step(true, 0.19, 0.0, kDt, out);
        check(j.switched && eq(out[0], -0.19, 1e-12), "F218: a difference of 19 cm is taken up");
        f218::Slide j2;
        j2.step(false, 0.0, 0.0, kDt, out);
        j2.step(true, 0.21, 0.0, kDt, out);
        check(j2.switched && out[0] == 0.0 && out[1] == 0.0 && j2.offsetM == 0.0, "F218: a difference of 21 cm is something else moving the body: no offset");
        // dt: clamped to 0.05 s (a long gap lets 2.5 cm go, not more), a zero or negative or not a number dt lets nothing go
        f218::Slide t;
        t.step(false, 0.0, 0.0, kDt, out);
        t.step(true, -0.10, 0.0, 0.0, out);
        t.step(true, -0.10, 0.0, 10.0, out);
        check(eq(out[0], 0.10 - 0.5 * f218::kMaxDtS, 1e-12), "F218: a long gap lets go at most 0.5 m/s of 0.05 s");
        f218::Slide z;
        z.step(false, 0.0, 0.0, kDt, out);
        z.step(true, -0.10, 0.0, -1.0, out);
        z.step(true, -0.10, 0.0, std::nan(""), out);
        check(eq(out[0], 0.10, 1e-12), "F218: a negative or not-a-number dt lets nothing go");
        // reset
        s.step(true, 0.0, 0.0, kDt, out);
        s.reset();
        s.step(true, 0.5, 0.5, kDt, out);
        check(!s.switched && out[0] == 0.0 && s.offsetM == 0.0, "F218: reset forgets the offset and the last move: the next call only remembers");
    }

    // ---- 3. the counts and the line ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f218::Stats st;
        check(!st.any(), "F218: a window with no rifle call has nothing to say");
        for (int i = 0; i < 100; ++i) {
            f218::Call c;
            c.applied = i < 60;
            c.refused = i >= 60 && i < 70;
            c.held = i >= 70;
            c.switched = i == 60 || i == 70;
            c.offsetM = i == 60 ? 0.08 : i == 70 ? 0.045 : 0.0;
            st.add(c);
        }
        check(st.calls == 100 && st.applied == 60 && st.refused == 10 && st.held == 30 && st.switches == 2 && eq(st.offsetMaxM, 0.08, 1e-12), "F218: the window counts calls, applied, refused, held, switches and the largest offset");
        char line[400];
        const int n = f218::formatLine(line, sizeof(line), 10, st);
        check(n > 0 && std::string(line) == "native on foot rifle torso (F218) 10 s: rifle calls 100, rest applied 60, refused 10, held 30; switches 2; largest slide 8.0 cm", line);
        st.clear();
        check(!st.any() && st.calls == 0 && st.offsetMaxM == 0.0, "F218: clear empties the window");
    }

    // ---- 4. the sites -----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), rec = slurp("src/common/onfoot_recorder_core.h");
        const auto has = [&](const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; };
        check(has(cpp, "const bool restHeld = armed && heldRifle && !(pc.detachW > 0.0) && g_f218Hold.step(now, pc.f95Moving);") &&
                  has(cpp, "neckYawEligible = armed && heldRifle && pc.detachW < 1.0 && solveWrite && !restHeld;") && has(cpp, "pc.f218Held = restHeld;"),
              "F218: a held rest solution makes the call not eligible, which is the legacy path byte for byte");
        const size_t elig = cpp.find("neckYawEligible = armed && heldRifle"), reset = cpp.find("if (!armed || !heldRifle) {\n            g_f218Hold.reset();\n            g_f218Slide.reset();");
        check(elig != std::string::npos && reset != std::string::npos && reset < elig, "F218: no rifle in hand resets the hold and the slide before the eligibility line");
        check(has(cpp, "g_f218Hold.reset();   // F218: and no hold on the rest solution") && has(cpp, "g_f218Slide.reset();\n            }"), "F218: an edge of the on-foot camera resets the hold and the slide");
        check(has(cpp, "if (pc.neckRestRefused && !(pc.detachW > 0.0)) g_f218Hold.refused(now);"), "F218: a refusal at the hip starts the hold");
        const size_t applied = cpp.find("pc.neckRestApplied = true;"), refusedAt = cpp.find("g_f218Hold.refused(now);"), slide = cpp.find("g_f218Slide.step(restApplied, mv.th[0], mv.th[2], dt218, off218);"),
                     write = cpp.find("g_f215WriteY = pc.f215BodyY;");
        check(applied != std::string::npos && refusedAt != std::string::npos && slide != std::string::npos && write != std::string::npos && slide < write && applied > write && refusedAt > applied,
              "F218: the slide is stepped before the whole-pose write, the refusal is noted after the last place that can refuse");
        check(has(cpp, "const bool restApplied = pc.neckRestRequested && pc.neckRestCandidateValid && pc.neckYawScope && pc.neckYawFeasible && legsSolved && !pc.neckRestRefused;") &&
                  has(cpp, "if (armed && heldRifle) {\n            const bool restApplied"),
              "F218: the slide runs with a rifle held and takes the decision pc.neckRestApplied is made from");
        check(has(cpp, "g_pfzAnim[j][12] = static_cast<float>(static_cast<double>(g_pfzAnim[j][12]) + off218[0]);") && has(cpp, "g_pfzChain[k][14] = static_cast<float>(static_cast<double>(g_pfzChain[k][14]) + off218[1]);"),
              "F218: the offset goes on every joint and on the freeze's chain");
        check(has(cpp, "nofoot::f218::formatLine(torsoLine") && has(cpp, "g_f218Box.write(") && has(cpp, "g_f218Box.clearPending.store(true"), "F218: the window's line is printed from the hook's window and cleared at an edge");
        check(has(cpp, "r.f218_held = pc.f218Held;") && has(cpp, "r.f218_applied = pc.neckRestApplied;") && has(cpp, "r.f218_refused = pc.neckRestRefused;") && has(cpp, "r.f218_offset_m = static_cast<float>(pc.f218OffsetM);") &&
                  has(rec, "S(f32, f218_offset_m)") && has(rec, "S(u8, f218_held) S(u8, f218_applied) S(u8, f218_refused)"),
              "F218: the pin record carries the hold, the solution, the refusal and the offset");
    }
}

}  // namespace f218test
