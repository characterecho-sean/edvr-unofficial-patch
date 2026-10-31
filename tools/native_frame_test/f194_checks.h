// F194 checks (native on-foot VR), run194 below. Header only, run by native_frame_test.
//
// F194 (native_on_foot_f194.h, native_on_foot.cpp, src/openxr/view_fov_note.h; TEMPORARY A/B key experimental.native_on_foot_look_to_aim_eye_height_as_mouse_look, default on, live). Part 1 (look to aim aiming at the
// middle of the picture, with its key) was removed on 2026-10-31 (F194b): the pilot's Quest flight refuted it (his gaze offset is about 5 deg against the picture's centre 12.2 deg below the axis), and the pitch,
// the frustum read, the ray and their rig cases are gone; the source pins below hold that nothing of it is left. Part 2: look to aim's own 10 deg settle gate (on the CARRIED pitch) is skipped, so F139's
// gate covers it as it covers mouse look. The rig: the gate's predicate (aim -13 / -15 / +12, armed and unarmed, against F139's rule; off: today's 10 deg), the 1 s line and its window, the height probe at an
// edge (an engage that moves nothing prints a small change, not nothing) and the source sites.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f109.h"
#include "../../src/d3d11/native_on_foot_f138.h"
#include "../../src/d3d11/native_on_foot_f171c.h"
#include "../../src/d3d11/native_on_foot_f194.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f194test {

namespace nf = edvr::nofoot;
namespace f194 = edvr::nofoot::f194;
namespace f138 = edvr::nofoot::f138;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
inline bool eq(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
inline size_t countOf(const std::string& hay, const std::string& needle) {
    size_t n = 0, at = 0;
    while ((at = hay.find(needle, at)) != std::string::npos) {
        ++n;
        at += needle.size();
    }
    return n;
}

template <class Check>
void run194(Check&& check) {
    // ---- 1. part 2: look to aim's settle gate ----------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        check(!f194::ltaGateWouldRefuse(9.99) && f194::ltaGateWouldRefuse(10.0) && f194::ltaGateWouldRefuse(45.0) && !f194::ltaGateWouldRefuse(std::nan("")) && !f194::ltaGateWouldRefuse(0.0),
              "F194: look to aim's own gate would refuse from 10 deg of carried pitch (not under it, not for a number that is not one)");
        check(f194::ltaGateRefuses(false, 13.0) && !f194::ltaGateRefuses(true, 13.0) && !f194::ltaGateRefuses(false, 9.9) && !f194::ltaGateRefuses(true, 9.9),
              "F194: with the key off the gate refuses from 10 deg (today's); with it on it refuses nothing");
        // on: the carried pitch is the engage's account; F139 reads the FP root's pitch (the game's aim), 20 deg, nothing drawn
        const struct {
            double carried, root;
            bool item;
            bool refusedOn, refusedOff;
        } cases[] = {
            {13.0, -13.0, false, false, true},    // aim -13 (an engage that carried 13): settles with the key on, refused by the 10 deg gate with it off
            {13.0, -13.0, true, false, true},
            {15.0, -15.0, false, false, true},
            {15.0, -15.0, true, false, true},
            {12.0, 12.0, false, false, true},     // the earlier Pimax engage at +11.9: refused for 80+90+37 calls, the view 10 cm low until the head levelled
            {12.0, 12.0, true, false, true},
            {5.0, -25.0, false, true, true},      // the game's aim 25 deg down, nothing drawn: F139 refuses in both
            {5.0, -25.0, true, false, false},     // ... with an item drawn it does not
            {5.0, 21.0, false, true, true},
            {9.9, -13.0, false, false, false},    // under the 10 deg carried and under F139's 20: both settle
            {0.0, 0.0, false, false, false},
        };
        bool all = true, tens = true, matchesF139 = true;
        for (const auto& c : cases) {
            const f194::SettleVerdict on = f194::settleVerdict(true, c.carried, c.root, true, c.item), off = f194::settleVerdict(false, c.carried, c.root, true, c.item);
            all = all && on.refused() == c.refusedOn && off.refused() == c.refusedOff;
            tens = tens && !on.tenRefused && on.tenWould == (c.carried >= 10.0) && off.tenWould == (c.carried >= 10.0) && off.tenRefused == (c.carried >= 10.0);
            matchesF139 = matchesF139 && on.f139Refused == f138::settleRefused(c.root, f138::kSettleGateDeg, c.item);
        }
        check(all, "F194: the settle's verdict: aim -13 / -15 / +12 (carried 13 / 15 / 12) settles with the key on, armed and unarmed; the 10 deg gate refused all of them with it off; F139 refuses 25 deg down unarmed in both");
        check(tens && matchesF139, "F194: with the key on the only refusal is F139's rule on the game's aim (f138::settleRefused, 20 deg, nothing drawn), exactly as mouse look; the 10 deg gate is counted as would-have in both");
        const f194::SettleVerdict unknown = f194::settleVerdict(true, 30.0, 50.0, false, false);
        check(unknown.tenWould && !unknown.tenRefused && !unknown.f139Refused && !unknown.refused(), "F194: a game aim that was not read is not F139's to refuse; the 10 deg gate is only counted");
        const f194::SettleVerdict offFirst = f194::settleVerdict(false, 13.0, -25.0, true, false);
        check(offFirst.tenRefused && !offFirst.f139Refused, "F194: off, the 10 deg gate refuses first and F139's rule is not asked, as the hook does (it counts the calls it refuses itself)");
    }
    // ---- 2. the 1 s line ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        char buf[1400];
        f194::Second s;
        f194::Window w;
        for (int i = 0; i < 90; ++i) {
            s.add(true, 2.0, -3.0, true, -16.0, true, 4.5);
            f194::Call c;
            c.eyeOverNeckM = 0.19;
            c.raiseM = -0.05;
            c.tenWould = true;
            c.taken = i == 30;
            if (i < 3) c.f139Refused = true;
            w.add(c);
        }
        f194::formatSecond(buf, sizeof(buf), s, w, true);
        check(std::strlen(buf) < 1100 && std::strstr(buf, "look to aim height (F194) 1 s: the headset's pitch (LOCAL) mean +2.0 deg;") != nullptr &&
                  std::strstr(buf, "carried aim pitch mean -3.0 deg (the game's aim -16.0 deg);") != nullptr && std::strstr(buf, "the reported head's pitch from the aim mean +4.5 deg;") != nullptr &&
                  std::strstr(buf, "look to aim's 10 deg gate would have refused (skipped) 90 of 90 pin calls, F138/F139's 20 deg gate refused 3, taken 1;") != nullptr &&
                  std::strstr(buf, "the published raise mean -5.0 cm;") != nullptr && std::strstr(buf, "the drawn eye over the animated neck mean +19.0 cm") != nullptr &&
                  std::strstr(buf, "picture") == nullptr && std::strstr(buf, "ray in force") == nullptr && std::strstr(buf, "forward axis") == nullptr,
              "F194: the 1 s line says the headset's, the carried and the game's aim pitch, the reported head's pitch from the aim, the settle (the 10 deg gate would have refused, skipped; F139's; taken), the raise and the drawn eye over the neck, and nothing of the picture's centre or a ray");
        f194::Window off;
        for (int i = 0; i < 12; ++i) {
            f194::Call c;
            c.tenWould = c.tenRefused = true;
            off.add(c);
        }
        f194::formatSecond(buf, sizeof(buf), s, off, false);
        check(std::strstr(buf, "look to aim's 10 deg gate refused 12 of 12 pin calls,") != nullptr, "F194: with the key off the 10 deg gate line says it refused (not would have)");
        f194::formatSecond(buf, sizeof(buf), s, f194::Window{}, true);
        check(std::strstr(buf, "of 0 pin calls") != nullptr && std::strstr(buf, "taken 0;") != nullptr,
              "F194: a second in which the hook wrote nothing says 0 pin calls: the line cannot read as success when the hook never ran");
        f194::Window w2;
        f194::Call missing;   // eye over the neck and the raise not numbers: not counted, not zero
        w2.add(missing);
        check(w2.any() && w2.eyeN == 0 && w2.raiseN == 0 && w2.calls == 1, "F194: a call with no eye reading or no raise yet counts as a call and adds nothing to the means");
        f194::Second ss;
        ss.add(false, 0.0, 5.0, false, 0.0, false, 0.0);
        check(ss.any() && ss.localN == 0 && ss.offN == 0 && ss.mean(ss.localSum, ss.localN) == 0.0 && ss.mean(ss.carriedSum, ss.ticks) == 5.0, "F194: a tick whose angles were not read adds nothing to their means");
        // the window goes through the mailbox the other 1 s lines use
        nf::f171c::Mailbox<f194::Window> box;
        f194::Call c1;
        c1.taken = true;
        box.write([&](f194::Window& win) noexcept { win.add(c1); });
        f194::Window got;
        check(box.take(&got) && got.calls == 1 && got.taken == 1, "F194: the hook's window goes through the mailbox the other 1 s lines use");
        f194::Window again;
        check(box.take(&again) && !again.any(), "F194: ... and a second take with no write in between reads an empty window");
    }
    // ---- 3. the view's height at an edge --------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        char buf[900];
        // an engage that moves nothing prints a small change, not nothing
        f194::HeightProbe flat;
        flat.begin(true, 10000, true, 1.610, 0.0);
        bool done = false;
        for (uint64_t t = 10000; t <= 13600 && !done; t += 11) done = flat.tick(t, true, 1.6101, -2.0);
        flat.format(buf, sizeof(buf));
        check(done && std::strlen(buf) < 760 && std::strstr(buf, "look to aim height (F194) engage: the view's height") != nullptr && std::strstr(buf, "before 1.610 m, carried aim pitch +0.0 deg;") != nullptr &&
                  std::strstr(buf, "after 0.5 s +0.0 cm (1.610 m), carried aim pitch -2.0 deg;") != nullptr && std::strstr(buf, "after 1 s +0.0 cm") != nullptr && std::strstr(buf, "after 3 s +0.0 cm") != nullptr &&
                  std::strstr(buf, "not read") == nullptr,
              "F194: an engage that moves nothing prints +0.0 cm at 0.5 s, 1 s and 3 s, with the heights and the carried aim pitch (a small change, not nothing)");
        // the pilot's drop: 10 cm in the first half second, then more at 3 s
        f194::HeightProbe drop;
        drop.begin(true, 20000, true, 1.610, 0.0);
        done = false;
        for (uint64_t t = 20000; t <= 23600 && !done; t += 11) done = drop.tick(t, true, t < 20500 ? 1.610 : t < 21000 ? 1.510 : t < 23000 ? 1.505 : 1.500, 13.0);
        drop.format(buf, sizeof(buf));
        check(done && std::strstr(buf, "after 0.5 s -10.0 cm (1.510 m)") != nullptr && std::strstr(buf, "after 1 s -10.5 cm (1.505 m)") != nullptr && std::strstr(buf, "after 3 s -11.0 cm (1.500 m), carried aim pitch +13.0 deg") != nullptr,
              "F194: the pilot's drop reads as -10.0 cm at 0.5 s, -10.5 at 1 s, -11.0 at 3 s with the carried aim pitch at each");
        // the disengage is named so
        f194::HeightProbe dis;
        dis.begin(false, 1000, true, 1.5, 3.0);
        dis.format(buf, sizeof(buf));
        check(std::strstr(buf, "look to aim height (F194) disengage: the view's height") != nullptr, "F194: a disengage's probe says disengage");
        // no frame before: the afters are heights with no change
        f194::HeightProbe noBefore;
        noBefore.begin(true, 30000, false, 1.6, 0.0);
        done = false;
        for (uint64_t t = 30000; t <= 33600 && !done; t += 11) done = noBefore.tick(t, true, 1.55, 1.0);
        noBefore.format(buf, sizeof(buf));
        check(done && std::strstr(buf, "before not read;") != nullptr && std::strstr(buf, "after 0.5 s 1.550 m (no frame before to change from)") != nullptr && std::strstr(buf, " cm (") == nullptr,
              "F194: with no frame before, the line says so and gives the heights, no change");
        // no fresh frame at all: the three moments read not read, and the probe still ends
        f194::HeightProbe stale;
        stale.begin(true, 40000, true, 1.6, 0.0);
        done = false;
        for (uint64_t t = 40000; t <= 44000 && !done; t += 11) done = stale.tick(t, false, 0.0, 0.0);
        stale.format(buf, sizeof(buf));
        check(done && std::strstr(buf, "after 0.5 s not read; after 1 s not read; after 3 s not read") != nullptr, "F194: with no fresh frame the moments read not read (a probe that read nothing is not a probe that read 0 cm)");
        // a moment is served by a frame read at most 400 ms after it: a stall past that is not read, the later ones still are
        f194::HeightProbe gap;
        gap.begin(true, 50000, true, 1.6, 0.0);
        done = false;
        for (uint64_t t = 50000; t <= 54000 && !done; t += 11) {
            const bool fresh = !(t >= 50400 && t < 51000);   // no frame from 0.4 s to 1.0 s
            done = gap.tick(t, fresh, 1.58, 0.0);
        }
        gap.format(buf, sizeof(buf));
        check(done && std::strstr(buf, "after 0.5 s not read; after 1 s -2.0 cm") != nullptr && std::strstr(buf, "after 3 s -2.0 cm") != nullptr,
              "F194: a stall of frames over the 0.5 s moment (and 400 ms of grace) reads it as not read; the 1 s and 3 s moments are served afterwards");
        // a frame inside the grace serves the moment
        f194::HeightProbe late;
        late.begin(true, 60000, true, 1.6, 0.0);
        done = false;
        for (uint64_t t = 60000; t <= 64000 && !done; t += 11) done = late.tick(t, t >= 60700, 1.59, 0.0);
        late.format(buf, sizeof(buf));
        check(done && std::strstr(buf, "after 0.5 s -1.0 cm") != nullptr, "F194: a frame 200 ms after the 0.5 s moment (inside the grace) serves it");
        // a next edge inside the 3 s cuts it short and says so
        f194::HeightProbe cut;
        cut.begin(true, 70000, true, 1.6, 0.0);
        cut.tick(70500, true, 1.59, 0.0);
        cut.cutShort();
        cut.format(buf, sizeof(buf));
        check(std::strstr(buf, "after 0.5 s -1.0 cm") != nullptr && std::strstr(buf, "after 1 s not read (cut short by the next edge)") != nullptr && std::strstr(buf, "after 3 s not read (cut short by the next edge)") != nullptr,
              "F194: an edge inside the 3 s cuts the probe short: what it read stays, the rest reads not read (cut short by the next edge)");
        f194::HeightProbe idle;
        check(!idle.tick(1000, true, 1.0, 0.0) && !idle.active, "F194: a probe that was never begun does nothing");
        f194::HeightProbe nan;
        nan.begin(true, 80000, true, std::nan(""), 0.0);
        check(!nan.haveBefore, "F194: a frame before that is not a number is no frame before");
    }
    // ---- 4. the source sites ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini"), f109h = slurp("src/d3d11/native_on_foot_f109.h"), f194h = slurp("src/d3d11/native_on_foot_f194.h");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const auto at = [&](const std::string& needle) { return cpp.find(needle); };
        check(!cpp.empty() && !ini.empty() && !f109h.empty() && !f194h.empty(), "F194: the sources the pins read are readable from the repo root");
        // part 1 is removed (F194b): nothing reads its key, no flag or call of it is left, and centreOffset is adsHeadAngles again. The needles are stems, cut short on purpose, so that the tree holds no spelling of the
        // removed names outside the docs.
        check(cpp.find("look_to_aim_aims_at") == std::string::npos && cpp.find("g_ltaAimsAt") == std::string::npos && cpp.find("g_ltaCentre") == std::string::npos && cpp.find("g_f194Frustum") == std::string::npos &&
                  cpp.find("g_f194Centre") == std::string::npos && cpp.find("f194::Frustum") == std::string::npos && cpp.find("f194::rayAng") == std::string::npos && cpp.find("f194::formatFrus") == std::string::npos &&
                  cpp.find("picture-centre pitch") == std::string::npos && ini.find("look_to_aim_aims_at") == std::string::npos && ini.find("aim at the middle of the picture") == std::string::npos &&
                  ini.find("(F194, A/B): Look to aim: aim at") == std::string::npos,
              "F194b: part 1 is gone: nothing reads its key, no flag, frustum or centre pitch is left in native_on_foot.cpp, edvr.ini has neither its entry nor its menu row");
        check(f194h.find("rayAng") == std::string::npos && f194h.find("pictureCentrePitch") == std::string::npos && f194h.find("frustumFromChannel") == std::string::npos && f194h.find("pitchInForce") == std::string::npos &&
                  f194h.find("sameFrustum") == std::string::npos && f194h.find("formatFrustum") == std::string::npos && f194h.find("struct Frustum") == std::string::npos &&
                  f194h.find("REMOVED on 2026-10-31 (F194b)") != std::string::npos,
              "F194b: native_on_foot_f194.h has no pitch helper, channel read, ray or frustum line, and says part 1 was removed");
        check(f109h.find("inline bool centreOffset(const float render[12], double* yawRad, double* pitchRad) noexcept {") != std::string::npos && f109h.find("if (!adsHeadAngles(render, &y, &p)) return false;") != std::string::npos &&
                  f109h.find("native_on_foot_f194.h") == std::string::npos,
              "F194b: centreOffset is adsHeadAngles again (no picture parameter)");
        check(countOf(cpp, "centreOffset(np.render, &oy, &op)") == 3 && countOf(cpp, "centreOffset(") == 3,
              "F194b: the three callers of centreOffset (the engage's centring, F133's yaw re-centre, the 1 s line's offset) call it as they did before F194");
        // the key: spelled out in its own call, default on, live; edvr.ini has it with a menu row
        check(has("cfg.getString(\"experimental.native_on_foot_look_to_aim_eye_height_as_mouse_look\", \"on\") != \"off\"") && has("std::atomic<bool> g_ltaEyeHeightAsMouseLook{true};"),
              "F194: the eye height key is read with its own literal, default on");
        check(ini.find("\nnative_on_foot_look_to_aim_eye_height_as_mouse_look = on\n") != std::string::npos &&
                  ini.find("# ui: Look to aim: eye height as mouse look (test) | choices on, off | recommended on | live\n") != std::string::npos &&
                  ini.find("# TEMPORARY (F194, A/B): Look to aim: eye height as mouse look") != std::string::npos &&
                  ini.find("\nnative_on_foot_pistol_sprint_swing_phase = 0.5\n\n# TEMPORARY (F194, A/B): Look to aim: eye height as mouse look") != std::string::npos,
              "F194: edvr.ini has the eye height key, default on, marked TEMPORARY, with a menu row, right after the pistol sprint swing row");
        // part 2: the 10 deg gate is the predicate's, skipped by the key, counted either way; F139's gate after it is unchanged; the old constant is gone
        const size_t wouldAt = at("if (si.valid && nofoot::f194::ltaGateWouldRefuse(f194CarriedAbs)) {"), countAt = at("g_f109SettleGated.fetch_add(1, std::memory_order_relaxed);"),
                     refuseAt = at("if (nofoot::f194::ltaGateRefuses(f194AsMouse, f194CarriedAbs)) {"),
                     f139At = at("if (si.valid && f138AimKnown && nofoot::f138::settleRefused(pc.f86AimPitch, nofoot::f138::kSettleGateDeg, f138Item)) {");
        check(wouldAt != std::string::npos && countAt != std::string::npos && refuseAt != std::string::npos && f139At != std::string::npos && wouldAt < countAt && countAt < refuseAt && refuseAt < f139At &&
                  has("const bool f194AsMouse = g_ltaEyeHeightAsMouseLook.load(std::memory_order_relaxed);") && has("const double f194CarriedAbs = static_cast<double>(g_f109AimPitchAbsDeg.load(std::memory_order_relaxed));") &&
                  !has("kF109SettlePitchDeg"),
              "F194: the 10 deg gate counts the call it would refuse, then refuses only with the key off, and runs before F139's gate, which is the same line as before; the old constant is gone");
        check(has("g_f109AimPitchAbsDeg.store(g_lta.engaged ? static_cast<float>(std::fabs(g_adsMap.pitchSeat) / nofoot::kAdsDeg) : 0.0f, std::memory_order_relaxed);"),
              "F194: the gate's input is the carried pitch (AdsAimMap::pitchSeat), unchanged");
        // the sites that read the head's forward axis and were never an aim: F134's seat yaw and F175's lead (measure only)
        check(has("nofoot::adsHeadAngles(np.local, &yawLocal, &pitchLocal) && nofoot::adsHeadAngles(np.render, &yawSeated, &pitchSeated)") &&
                  has("nofoot::adsHeadAngles(in.render, &leadYaw, &leadPitch)"),
              "F194: F134's seat yaw (the difference of one axis's headings in two spaces) and F175's lead (measure only) read the forward axis");
        // the instruments: the hook's window, the view's height, the probe's edges, the 1 s line
        check(has("g_f194Box.write([&](nofoot::f194::Window& win) noexcept { win.add(c194); });") && has("c194.tenWould = f194TenWould;") && has("c194.tenRefused = f194TenRefused;") &&
                  has("c194.f139Refused = f138SettleRefused;") && has("c194.taken = so.settled;") && has("if (pc.f97EyeDrawnOk) c194.eyeOverNeckM = pc.f97EyeDrawn[1] - N0[1];") &&
                  has("g_f194ViewY.store(pc.f97EyeDrawn[1], std::memory_order_relaxed);") && has("if (freshIn && pc.f97EyeDrawnOk && refusal != kPinRefCameraAway && !(pc.detachW > 0.0)) {"),
              "F194: the pin hook writes the settle's fate, the drawn eye over the neck and the raise to the window, and the view's height (a fresh hip frame, the commander's camera) for the probe");
        check(has("f194Edge(now, true);   // F194: the view's height before the engage") && has("f194Edge(now, false);   // F194: the view's height before the disengage") &&
                  has("if (g_lta.engaged) f194Edge(GetTickCount64(), false);") && has("f194Tick(now);") && has("nofoot::f194::formatSecond(heightLine, sizeof(heightLine), g_f194Sec, hw, g_ltaEyeHeightAsMouseLook.load(std::memory_order_relaxed));") &&
                  has("if (g_f194Box.take(&hw) && g_f194Sec.any()) {"),
              "F194: the probe is begun at the engage, the disengage and a stop, ticked every frame, and the 1 s line is taken from the hook's window while engaged");
        // F109's centred line: the picture-centre and forward-axis fields are gone, F195's tail stays
        check(has("pitch kept %+.2f deg, centred %+.2f deg (F195: %s); %s\",") && !has("the forward axis itself") && !has("(F194), "),
              "F194b: the F109 centred line is the reported head's offset, F195's kept and centred pitch and the outcome: no picture-centre pitch, no second set of angles");
    }
}

}  // namespace f194test
