// F91 checks (native on-foot VR), parts B and C, run91bc below: the pure functions of src\d3d11\native_on_foot_f91.h. B: the ADS vibration instrument's per-frame fields
// (the "; view (F91)" suffix of the F89 trace, the cancel the pose carried, the view's step, the rifle in the view) and the fit that decides between H1 (the cancel lands d frames
// late), the sight lock and H3 (the readout pin's lag), on synthetic series built to each answer, still (tremor) and moving. C: the ADS item gate on the names both flights logged,
// and a tick sequence through the gate, adsAimTick's state edges, the head aim's weight, the left eye's darkening and the body detach. Part A's checks (the readout part's
// identity) are run90's and part A's own. Header only, run by native_frame_test.
#pragma once

#include "../../src/d3d11/journal_watch.h"
#include "../../src/d3d11/native_on_foot_core.h"   // AdsRamp, f89::darken
#include "../../src/d3d11/native_on_foot_f91.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace f91bctest {

namespace nf = edvr::nofoot;
namespace f89 = edvr::nofoot::f89;
namespace f91 = edvr::nofoot::f91;

// A deterministic uniform in [-1, 1).
struct Lcg {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    double next() noexcept {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>((s >> 11) & ((1ull << 53) - 1)) / static_cast<double>(1ull << 52) - 1.0;
    }
};
// Runs frames through a press's fit windows; every full window is fitted (as f87EyeLines does) and the results kept.
inline std::vector<f91::FitResult> fitAll(f91::FitWindow& w, const std::vector<f91::FitFrame>& frames) {
    std::vector<f91::FitResult> out;
    w.beginPress();
    for (const f91::FitFrame& f : frames)
        if (w.add(f)) {
            out.push_back(f91::fitWindow(w));
            f91::fitCarry(w, out.back());
        }
    return out;
}
inline bool has(const char* text, const char* what) { return std::strstr(text, what) != nullptr; }

template <class Check>
inline void run91bc(Check&& check) {
    const double dt = 1.0 / 90.0;

    // ---- C: the item gate ------------------------------------------------------------------------------------------------------------------------------
    {
        const char* weapons[] = {"$wpn_m_assaultrifle_laser_fauto_name;", "$wpn_s_pistol_kinetic_sauto_name;", "$WPN_M_ASSAULTRIFLE_LASER_FAUTO_NAME;", "$Wpn_s_pistol_kinetic_sauto_name;"};
        bool ok = true;
        for (const char* n : weapons) {
            const f91::ItemGate g = f91::adsItemGate(false, edvr::kHeldItem, n);
            ok = ok && g.allow && std::strcmp(g.reason, "weapon") == 0;
        }
        check(ok, "F91 C: the weapons both flights held ($wpn_m_assaultrifle_laser_fauto_name;, $wpn_s_pistol_kinetic_sauto_name;) are allowed by name, $WPN_ in upper case too");
        const char* tools[] = {"$humanoid_companalyser_name;", "$humanoid_repairtool_name;", "$humanoid_rechargetool_name;", "$humanoid_anyothertool_name;", "$wpn", "$wp", "wpn_x"};
        ok = true;
        for (const char* n : tools) {
            const f91::ItemGate g = f91::adsItemGate(false, edvr::kHeldItem, n);
            ok = ok && !g.allow && std::strcmp(g.reason, "tool") == 0;
        }
        check(ok, "F91 C: the tools both flights held (the Profile Analyser, the Arc Cutter, the Energylink), any other $humanoid_ item and names short of \"$wpn_\" are refused as a tool");
        // the fists, through Status.json's own parse
        static const char kFists[] = "{\"timestamp\":\"x\",\"SelectedWeapon\":\"$humanoid_fists_name;\",\"SelectedWeapon_Localised\":\"Unarmed\"}";
        char name[64];
        const edvr::JournalHeldItem fk = edvr::journalParseSelectedWeapon(kFists, sizeof(kFists) - 1, name, sizeof(name));
        const f91::ItemGate fg = f91::adsItemGate(false, fk, name);
        static const char kRifle[] = "{\"SelectedWeapon\":\"$wpn_m_assaultrifle_laser_fauto_name;\"}";
        const edvr::JournalHeldItem rk = edvr::journalParseSelectedWeapon(kRifle, sizeof(kRifle) - 1, name, sizeof(name));
        const f91::ItemGate rg = f91::adsItemGate(false, rk, name);
        check(fk == edvr::kHeldNone && !fg.allow && std::strcmp(fg.reason, "fists") == 0 && rk == edvr::kHeldItem && rg.allow,
              "F91 C: the fists (Status.json's \"$humanoid_fists_name;\", parsed) are refused as fists; the rifle parsed from Status.json is allowed");
        const f91::ItemGate u1 = f91::adsItemGate(false, edvr::kHeldUnknown, "");
        const f91::ItemGate u2 = f91::adsItemGate(false, edvr::kHeldItem, "");
        const f91::ItemGate u3 = f91::adsItemGate(false, edvr::kHeldItem, nullptr);
        const f91::ItemGate p1 = f91::adsItemGate(true, edvr::kHeldUnknown, "");
        const f91::ItemGate p2 = f91::adsItemGate(true, edvr::kHeldItem, "$humanoid_companalyser_name;");
        const f91::ItemGate p3 = f91::adsItemGate(true, edvr::kHeldNone, "$humanoid_fists_name;");
        check(!u1.allow && !u2.allow && !u3.allow && std::strcmp(u1.reason, "unknown") == 0 && std::strcmp(u2.reason, "unknown") == 0 && std::strcmp(u3.reason, "unknown") == 0 &&
                  p1.allow && p2.allow && p3.allow && std::strcmp(p1.reason, "native primary") == 0,
              "F91 C: an empty or unknown name is refused unless F83 says primary; with F83's primary a tool name or the fists are allowed (Status.json lags a switch by up to 100 ms)");
    }
    // ---- C: a tick sequence through the gate, the state edges, the weight, the left eye and the body detach --------------------------------------------
    {
        struct Sim {
            f91::GateEdges gate;
            int state = f91::kStateIdle;
            nf::AdsRamp ramp;
            double w = 0.0, darken = 0.0;
            bool bodyDetach = false;
            std::vector<int> events;
            int maxState = 0;
            double maxW = 0.0, maxDarken = 0.0;
            bool anyDetach = false;
            void tick(bool asked, edvr::JournalHeldItem kind, const char* name, bool primary, double dt) {
                const f91::ItemGate g = f91::adsItemGate(primary, kind, name);
                const f91::GateEdges::Out o = gate.step(asked, g.allow);
                if (o.event != f91::kGateNone) events.push_back(o.event);
                bodyDetach = o.ads;   // F73: the body detach follows the gated ADS
                state = f91::adsStateAfter(state, o.ads);
                if (state == f91::kStateIdle) {
                    w = 0.0;
                } else {
                    ramp.step(state == f91::kStateEngaged, dt);
                    w = ramp.w;
                    if (state == f91::kStateReleasing && w <= 0.0) state = f91::kStateIdle;   // the unwind is done when the weight reaches 0 (the map's chase aside)
                }
                const bool armed = kind == edvr::kHeldItem || primary;
                darken = f89::darken(true, true, armed, w).level;
                maxState = std::max(maxState, state);
                maxW = std::max(maxW, w);
                maxDarken = std::max(maxDarken, darken);
                anyDetach = anyDetach || bodyDetach;
            }
        };
        const char* analyser = "$humanoid_companalyser_name;";
        const char* rifle = "$wpn_m_assaultrifle_laser_fauto_name;";
        const char* pistol = "$wpn_s_pistol_kinetic_sauto_name;";
        // two tool presses of 1.5 s (the F90 flight's: the veto came after 1.5 s), the key on
        Sim tool;
        for (int i = 0; i < 30; ++i) tool.tick(false, edvr::kHeldItem, analyser, false, dt);
        for (int p = 0; p < 2; ++p) {
            for (int i = 0; i < 135; ++i) tool.tick(true, edvr::kHeldItem, analyser, false, dt);
            for (int i = 0; i < 30; ++i) tool.tick(false, edvr::kHeldItem, analyser, false, dt);
        }
        check(tool.maxState == f91::kStateIdle && tool.maxW == 0.0 && tool.maxDarken == 0.0 && !tool.anyDetach && tool.events.size() == 2 &&
                  tool.events[0] == f91::kGateRefused && tool.events[1] == f91::kGateRefused,
              "F91 C: a Profile Analyser press leaves the head aim idle, its weight 0, the left eye's darkening 0 and the body detach off; each press logs one refusal");
        Sim fists;
        for (int i = 0; i < 90; ++i) fists.tick(true, edvr::kHeldNone, "$humanoid_fists_name;", false, dt);
        check(fists.maxState == f91::kStateIdle && fists.maxDarken == 0.0 && !fists.anyDetach && fists.events.size() == 1 && fists.events[0] == f91::kGateRefused,
              "F91 C: the fists with HumanoidZoom held never engage");
        // a rifle press engages; switched to the Arc Cutter mid-press it releases through the unwind, the body detach at once
        Sim sw;
        for (int i = 0; i < 90; ++i) sw.tick(true, edvr::kHeldItem, rifle, false, dt);
        const bool engaged = sw.state == f91::kStateEngaged && sw.w == 1.0 && sw.darken == 1.0 && sw.bodyDetach && sw.events.empty();
        sw.tick(true, edvr::kHeldItem, "$humanoid_repairtool_name;", false, dt);
        const bool releasing = sw.state == f91::kStateReleasing && !sw.bodyDetach && sw.w < 1.0 && sw.w > 0.0 && sw.events.size() == 1 && sw.events[0] == f91::kGateStopped;
        for (int i = 0; i < 40; ++i) sw.tick(true, edvr::kHeldItem, "$humanoid_repairtool_name;", false, dt);
        check(engaged && releasing && sw.state == f91::kStateIdle && sw.w == 0.0 && sw.darken == 0.0 && sw.events.size() == 1,
              "F91 C: a rifle press engages (weight 1, the left eye closed, the body detached); switched to a tool mid-press it logs one stop and releases over 0.3 s to idle");
        // a pistol press while Status.json still says the fists: refused, then allowed when the name arrives (100 ms), and engaged
        Sim pis;
        for (int i = 0; i < 9; ++i) pis.tick(true, edvr::kHeldNone, "$humanoid_fists_name;", false, dt);
        const bool waited = pis.state == f91::kStateIdle && pis.events.size() == 1 && pis.events[0] == f91::kGateRefused;
        for (int i = 0; i < 30; ++i) pis.tick(true, edvr::kHeldItem, pistol, false, dt);
        check(waited && pis.state == f91::kStateEngaged && pis.w == 1.0 && pis.events.size() == 2 && pis.events[1] == f91::kGateAllowed,
              "F91 C: a pistol press before Status.json names it is refused, then allowed (one line) when its name arrives, and engages");
        // F83's primary engages with Status.json still naming a tool
        Sim prim;
        for (int i = 0; i < 30; ++i) prim.tick(true, edvr::kHeldItem, analyser, true, dt);
        check(prim.state == f91::kStateEngaged && prim.events.empty(), "F91 C: F83's native primary engages whatever Status.json names");
        // the edges: adsStateAfter is adsAimTick's
        check(f91::adsStateAfter(0, true) == 1 && f91::adsStateAfter(0, false) == 0 && f91::adsStateAfter(1, false) == 2 && f91::adsStateAfter(1, true) == 1 &&
                  f91::adsStateAfter(2, true) == 1 && f91::adsStateAfter(2, false) == 2,
              "F91 C: the state edges: engage on ADS, release when it ends, re-engage from the release (the release's end is the caller's)");
    }

    // ---- B: the trace's fields -------------------------------------------------------------------------------------------------------------------------
    {
        f91::ViewFields v;
        v.rShown = true;
        v.r[0] = 0.123;
        v.r[1] = -0.045;
        v.rowsKnown = true;
        v.rowsFrame = 1234;
        v.headSeq = 5678;
        v.applied[0] = 0.010;
        v.applied[1] = -0.020;
        v.head[0] = 0.030;
        v.head[1] = 0.004;
        v.viewOk = true;
        v.viewStep = 0.031;
        v.cancelTotal[0] = 1.5;
        v.cancelTotal[1] = -0.25;
        v.sightOk = true;
        v.along = 0.12;
        v.perp = 0.34;
        v.asked[0] = 0.030;
        v.asked[1] = 0.004;
        v.cancelNow[0] = 0.010;
        v.cancelNow[1] = -0.020;
        v.poseOk = true;
        v.cancelPose[0] = 1.48;
        v.cancelPose[1] = -0.23;
        v.poseAge = 2;
        v.serialLag = 1;
        v.rifleOk = true;
        v.rifle[0] = 0.5;
        v.rifle[1] = -1.25;
        v.rifleStepOk = true;
        v.rifleStep[0] = 0.002;
        v.rifleStep[1] = -0.001;
        char buf[640];
        f91::formatView(buf, sizeof(buf), v);
        const char* want =
            "; view (F91): r yaw +0.123 pitch -0.045 deg (rows frame 1234, head seq 5678), applied yaw +0.010 pitch -0.020, head step yaw +0.030 pitch +0.004, view step 0.031 deg, "
            "cancel total yaw +1.500 pitch -0.250 deg; sight along +0.12 perp 0.34 mm; asked yaw +0.030 pitch +0.004, cancel this frame yaw +0.010 pitch -0.020, cancel in the pose "
            "yaw +1.480 pitch -0.230 deg (2 frames old, alignment serial lag 1); rifle in view yaw +0.500 pitch -1.250 deg, step yaw +0.002 pitch -0.001";
        const bool exact = std::strcmp(buf, want) == 0;
        if (!exact) std::printf("F91 formatView got:  %s\nF91 formatView want: %s\n", buf, want);
        check(exact, "F91 B: a synthetic tick's fields print as the design gives them (r, rows frame, head seq, applied, head step, view step, cancel total, sight) and the additions");
        f91::ViewFields idle = v;
        idle.rShown = false;
        idle.viewOk = idle.sightOk = idle.poseOk = idle.rifleOk = false;
        f91::formatView(buf, sizeof(buf), idle);
        static const char kIdleHead[] = "; view (F91): r - (rows frame 1234, head seq 5678), applied yaw";
        const bool dash = std::strncmp(buf, kIdleHead, sizeof(kIdleHead) - 1) == 0 && has(buf, "view step -, cancel total") &&
                          has(buf, "sight -; asked") && has(buf, "cancel in the pose - (") && has(buf, "rifle in view -") && !has(buf, "nan");
        idle.rowsKnown = false;
        f91::formatView(buf, sizeof(buf), idle);
        check(dash && has(buf, "r - (rows frame none, head seq 5678)"),
              "F91 B: not engaged the line reads r - (the stamps still printed), and a figure not measured this frame reads - (never a stale value as if fresh)");
        char tiny[24];
        const size_t n = f91::formatView(tiny, sizeof(tiny), v);
        check(n < sizeof(tiny) && std::strlen(tiny) == n && std::strncmp(tiny, "; view (F91): ", 14) == 0, "F91 B: the suffix is cut, never overrun, in a short buffer");
    }
    // ---- B: the cancel the pose carried --------------------------------------------------------------------------------------------------------------
    {
        f91::CancelHistory h;
        for (uint32_t s = 1; s <= 5; ++s) h.record(s, 100 + s, 0.1 * s, -0.01 * s);
        const f91::CancelHistory::Found a = h.find(3 + 1, 107);
        const f91::CancelHistory::Found none = h.find(0, 107), absent = h.find(9 + 1, 107);
        f91::CancelHistory g;
        for (uint32_t s = 1; s <= 300; ++s) g.record(s, s, 0.0, 0.0);
        const f91::CancelHistory::Found old = g.find(10 + 1, 300), recent = g.find(299 + 1, 301);
        check(a.ok && a.age == 4 && a.serialLag == 2 && std::fabs(a.yaw - 0.3) < 1e-12 && std::fabs(a.pitch + 0.03) < 1e-12 && !none.ok && !absent.ok && !old.ok && recent.ok &&
                  recent.age == 2,
              "F91 B: the pose's alignment serial names the cancel it carried and its age in ticks; not reported, never published or overwritten (256 back) reads not found");
    }
    // ---- B: the view's step and the rifle in the view -----------------------------------------------------------------------------------------------
    {
        const auto rot = [](const double axis[3], double deg, const double v[3], double out[3]) {
            const double a = deg * f91::kPi / 180.0, c = std::cos(a), s = std::sin(a);
            const double k = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
            const double x[3] = {axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0]};
            for (int i = 0; i < 3; ++i) out[i] = v[i] * c + x[i] * s + axis[i] * k * (1.0 - c);
        };
        const double r0[3] = {1, 0, 0}, u0[3] = {0, 1, 0}, f0[3] = {0, 0, 1};
        const double ax[3] = {0.48, 0.8, 0.36};   // unit
        bool ok = true;
        for (const double deg : {0.0, 0.01, 0.02, 0.5, 30.0}) {
            double r1[3], u1[3], f1[3];
            rot(ax, deg, r0, r1);
            rot(ax, deg, u0, u1);
            rot(ax, deg, f0, f1);
            ok = ok && std::fabs(f91::viewStepDeg(r0, u0, f0, r1, u1, f1) - deg) < 1e-9;
        }
        // from float rows: a 0.01 deg step is still read to a thousandth
        double r1[3], u1[3], f1[3], rf[3], uf[3], ff[3], rf1[3], uf1[3], ff1[3];
        rot(ax, 0.01, r0, r1);
        rot(ax, 0.01, u0, u1);
        rot(ax, 0.01, f0, f1);
        for (int i = 0; i < 3; ++i) {
            rf[i] = static_cast<float>(r0[i]), uf[i] = static_cast<float>(u0[i]), ff[i] = static_cast<float>(f0[i]);
            rf1[i] = static_cast<float>(r1[i]), uf1[i] = static_cast<float>(u1[i]), ff1[i] = static_cast<float>(f1[i]);
        }
        ok = ok && std::fabs(f91::viewStepDeg(rf, uf, ff, rf1, uf1, ff1) - 0.01) < 1e-3;
        check(ok, "F91 B: the view's step is the angle between the two axes sets (0, 0.01, 0.02, 0.5 and 30 deg to 1e-9; a 0.01 deg step from float axes to a thousandth)");
        double y = 0.0, p = 0.0, y2 = 0.0, p2 = 0.0;
        const double dir[3] = {std::sin(1.0 * f91::kPi / 180.0), 0.0, std::cos(1.0 * f91::kPi / 180.0)};
        const double dirUp[3] = {0.0, std::sin(-2.0 * f91::kPi / 180.0), std::cos(-2.0 * f91::kPi / 180.0)};
        const double zero[3] = {0.0, 0.0, 0.0};
        check(f91::rifleInView(dir, r0, u0, f0, &y, &p) && std::fabs(y - 1.0) < 1e-9 && std::fabs(p) < 1e-9 && f91::rifleInView(dirUp, r0, u0, f0, &y2, &p2) &&
                  std::fabs(p2 + 2.0) < 1e-9 && std::fabs(y2) < 1e-9 && !f91::rifleInView(zero, r0, u0, f0, &y, &p),
              "F91 B: the rifle in the view: a sight line 1 deg right reads yaw 1, 2 deg down reads pitch -2, a degenerate one is refused");
    }
    // ---- B: the fit ----------------------------------------------------------------------------------------------------------------------------------
    f91::FitWindow* fw = new f91::FitWindow;
    {   // H1, moving: the head turns 9-22 deg/s with tremor, the game applies each ask a frame later, r is the last 2 applied steps (yaw +, pitch -) plus the base's offset
        Lcg rng;
        std::vector<f91::FitFrame> fr;
        double hPrev[2] = {}, aPrev[2] = {};
        for (int i = 0; i < 360; ++i) {
            f91::FitFrame f;
            f.dt = dt;
            f.t = i * dt;
            f.headOk = true;
            f.head[0] = 0.17 + 0.08 * std::sin(2.0 * f91::kPi * i / 30.0) + 0.02 * rng.next();
            f.head[1] = 0.04 * std::sin(2.0 * f91::kPi * i / 23.0) + 0.02 * rng.next();
            f.applied[0] = hPrev[0];
            f.applied[1] = hPrev[1];
            f.rOk = true;
            f.r[0] = 0.4 + (f.applied[0] + aPrev[0]) + 0.002 * rng.next();
            f.r[1] = -0.1 - (f.applied[1] + aPrev[1]) + 0.002 * rng.next();
            f.sightOk = true;
            f.along = 0.5 + 0.0005 * i;
            f.perp = 0.3;
            for (int k = 0; k < 2; ++k) aPrev[k] = f.applied[k], hPrev[k] = f.head[k];
            fr.push_back(f);
        }
        const std::vector<f91::FitResult> R = fitAll(*fw, fr);
        bool ok = R.size() >= 7 && R.size() <= 8;
        for (size_t i = 1; ok && i < R.size(); ++i)   // the first window carries the press's first frames (no history yet)
            ok = !R[i].still && R[i].h1 && R[i].bestD == 2 && R[i].signY == 1 && R[i].signP == -1 && R[i].bestRms < f91::kH1FitDeg && R[i].rmsD[0] > 0.02 &&
                 R[i].rmsD[2] > 0.02 && !R[i].lock && !R[i].h3 && R[i].index == i;
        char text[1400];
        if (!R.empty()) f91::formatFit(text, sizeof(text), R.back());
        check(ok && !R.empty() && has(text, "head MOVING") && has(text, "verdict: H1 (the cancel lands 2 frames late)") && !has(text, "nan"),
              "F91 B: H1 moving: r equal to the last 2 applied steps (yaw +, pitch -, an offset from the base) fits d 2 under 0.01 deg, d 1 and d 3 do not, the window is moving");
    }
    {   // H1 at tremor scale, still: the head's tremor (zero mean: the steps of a random position, so the net turn of a window is at most 0.14 deg), the game applies each ask a
        // frame later, r is this frame's applied step (d 1)
        Lcg rng;
        std::vector<f91::FitFrame> fr;
        double hPrev[2] = {}, uPrev[2] = {rng.next(), rng.next()};
        for (int i = 0; i < 180; ++i) {
            f91::FitFrame f;
            f.dt = dt;
            f.t = i * dt;
            f.headOk = true;
            const double u[2] = {rng.next(), rng.next()};
            f.head[0] = 0.07 * (u[0] - uPrev[0]);
            f.head[1] = 0.05 * (u[1] - uPrev[1]);
            uPrev[0] = u[0];
            uPrev[1] = u[1];
            f.applied[0] = hPrev[0];
            f.applied[1] = hPrev[1];
            f.rOk = true;
            f.r[0] = f.applied[0];
            f.r[1] = f.applied[1];
            f.missingOk = true;   // the pose carried the cancel up to the last tick: the missing part is this tick's applied
            f.missing[0] = f.applied[0];
            f.missing[1] = f.applied[1];
            hPrev[0] = f.head[0];
            hPrev[1] = f.head[1];
            fr.push_back(f);
        }
        const std::vector<f91::FitResult> R = fitAll(*fw, fr);
        bool ok = R.size() >= 3;
        for (size_t i = 1; ok && i < R.size(); ++i)
            ok = R[i].still && R[i].netRate < 0.5 && R[i].h1 && R[i].bestD == 1 && R[i].rStepRms2 >= f91::kH1StepDeg && R[i].missRms < 1e-9 && R[i].xr[0][1] > 0.6 &&
                 R[i].xr[1][1] > 0.6 && R[i].headRms[0] > 0.03;
        check(ok, "F91 B: H1 at tremor scale: a still window (net turn under 2 deg/s) whose r is the head's tremor a frame late fits d 1, matches the cancel missing from the pose, "
                  "and r's step follows the head's step one frame later (correlation over 0.6 at lag 1)");
    }
    {   // the rifle in the view jitters with the head's step a frame later; the correlation names the lag
        Lcg rng;
        std::vector<f91::FitFrame> fr;
        double hPrev[2] = {}, rifle[2] = {0.3, -0.2};
        for (int i = 0; i < 100; ++i) {   // two windows whether one holds 45 frames or 46
            f91::FitFrame f;
            f.dt = dt;
            f.t = i * dt;
            f.headOk = true;
            f.head[0] = 0.06 * rng.next();
            f.head[1] = 0.06 * rng.next();
            rifle[0] += hPrev[0];
            rifle[1] -= hPrev[1];
            f.rifleOk = true;
            f.rifle[0] = rifle[0];
            f.rifle[1] = rifle[1];
            f.rOk = true;
            hPrev[0] = f.head[0];
            hPrev[1] = f.head[1];
            fr.push_back(f);
        }
        const std::vector<f91::FitResult> R = fitAll(*fw, fr);
        const bool ok = R.size() == 2 && R[1].xf[0][1] > 0.95 && R[1].xf[1][1] < -0.95 && std::fabs(R[1].xf[0][0]) < 0.5 && std::fabs(R[1].xf[0][2]) < 0.5 &&
                        R[1].rifleRms[0] > 0.02 && std::fabs(R[1].rifleRms[0] / R[1].headRms[0] - 1.0) < 0.15 && R[1].xfN[1] > 30 && std::isnan(R[1].xr[0][0]);
        check(ok, "F91 B: the rifle's step in the view against the head's: lag 1 correlates (+1 yaw, -1 pitch), lags 0 and 2 do not, its jitter rms equals the tremor's; a flat r "
                  "has no correlation (printed -)");
    }
    {   // the sight lock oscillating: r flat, the sight error's step reversing by 0.3 mm a frame
        std::vector<f91::FitFrame> fr;
        for (int i = 0; i < 46; ++i) {
            f91::FitFrame f;
            f.dt = dt;
            f.t = i * dt;
            f.headOk = true;
            f.rOk = true;
            f.r[0] = 0.1;
            f.sightOk = true;
            f.along = 1.0 + ((i & 1) ? 0.15 : -0.15);
            f.perp = 0.4;
            f.viewOk = true;
            fr.push_back(f);
        }
        const std::vector<f91::FitResult> R = fitAll(*fw, fr);
        char text[1400];
        if (!R.empty()) f91::formatFit(text, sizeof(text), R[0]);
        check(R.size() == 1 && R[0].lock && R[0].revAlong > 40 && R[0].revPerp == 0 && !R[0].h1 && !R[0].h3 && R[0].still && has(text, "verdict: LOCK"),
              "F91 B: the lock: the sight error stepping back and forth by 0.3 mm a frame with r flat reads LOCK, not H1 or H3");
    }
    {   // the lock overshooting after the head stops: a moving window carries the error forward, the still one after it brings it back 0.36 mm without a reversal
        std::vector<f91::FitFrame> fr;
        double along = 0.0;
        for (int i = 0; i < 92; ++i) {
            f91::FitFrame f;
            f.dt = dt;
            f.t = i * dt;
            const bool moving = i < 46;
            f.headOk = true;
            f.head[0] = moving ? 0.1 : 0.0;
            along += moving ? 0.02 : -0.008;
            f.rOk = true;
            f.sightOk = true;
            f.along = along;
            f.perp = 0.2;
            fr.push_back(f);
        }
        const std::vector<f91::FitResult> R = fitAll(*fw, fr);
        check(R.size() == 2 && !R[0].still && !R[0].lock && R[1].still && R[1].overshoot && R[1].lock && R[1].revAlong == 0,
              "F91 B: the lock's overshoot: the error moving back 0.36 mm in the still window after a moving one reads LOCK (overshoot), the moving window does not");
    }
    {   // H3: r flat, the view's step the head's, the sight still: the readout pin's lag is the suspect
        Lcg rng;
        std::vector<f91::FitFrame> fr;
        for (int i = 0; i < 46; ++i) {
            f91::FitFrame f;
            f.dt = dt;
            f.t = i * dt;
            f.headOk = true;
            f.head[0] = 0.1 + 0.05 * std::sin(i * 0.4);
            f.head[1] = 0.03 * std::cos(i * 0.3);
            f.applied[0] = f.head[0];
            f.applied[1] = f.head[1];
            f.rOk = true;
            f.r[0] = 0.05 + 0.0001 * rng.next();
            f.r[1] = -0.02 + 0.0001 * rng.next();
            f.viewOk = true;
            f.viewStep = std::sqrt(f.head[0] * f.head[0] + f.head[1] * f.head[1]);
            f.sightOk = true;
            f.along = 0.3 + 0.01 * (i & 1);
            f.perp = 0.2;
            fr.push_back(f);
        }
        const std::vector<f91::FitResult> R = fitAll(*fw, fr);
        char text[1400];
        if (!R.empty()) f91::formatFit(text, sizeof(text), R[0]);
        check(R.size() == 1 && R[0].h3 && !R[0].h1 && !R[0].lock && R[0].viewHeadRms < 1e-9 && has(text, "verdict: H3"),
              "F91 B: H3: r flat, the view's step equal to the head's and the sight still read H3 (the readout pin's lag), not H1 or LOCK");
    }
    {   // undecided: r moves but not with the applied steps; and a window without r has no verdict
        Lcg rng;
        std::vector<f91::FitFrame> fr, noR;
        for (int i = 0; i < 46; ++i) {
            f91::FitFrame f;
            f.dt = dt;
            f.t = i * dt;
            f.headOk = true;
            f.head[0] = 0.05 * rng.next();
            f.applied[0] = 0.05 * rng.next();
            f.rOk = true;
            f.r[0] = 0.1 * rng.next();
            f.r[1] = 0.1 * rng.next();
            f.sightOk = true;
            f.along = 0.3;
            f.perp = 0.2;
            fr.push_back(f);
            f.rOk = false;
            noR.push_back(f);
        }
        std::vector<f91::FitResult> R = fitAll(*fw, fr);
        char text[1400];
        if (!R.empty()) f91::formatFit(text, sizeof(text), R[0]);
        const bool undecided = R.size() == 1 && !R[0].h1 && !R[0].lock && !R[0].h3 && R[0].bestRms > 0.03 && has(text, "verdict: none of H1, LOCK or H3");
        R = fitAll(*fw, noR);
        if (!R.empty()) f91::formatFit(text, sizeof(text), R[0]);
        check(undecided && R.size() == 1 && !R[0].enough && !R[0].h1 && !R[0].h3 && has(text, "too few r frames") && has(text, "verdict: none (too few r frames)"),
              "F91 B: r moving unlike the applied steps reads none of H1, LOCK or H3; a window without r frames gives no verdict and says why");
    }
    {   // a new residual base mid-window moves r by a constant: the fit takes the offset out per base (and would fail without it)
        Lcg rng;
        std::vector<f91::FitFrame> fr, oneBase;
        double hPrev = 0.0, aPrev = 0.0;
        for (int i = 0; i < 92; ++i) {
            f91::FitFrame f;
            f.dt = dt;
            f.t = i * dt;
            f.headOk = true;
            f.head[0] = 0.15 + 0.1 * std::sin(2.0 * f91::kPi * i / 20.0);
            f.applied[0] = hPrev;
            f.rOk = true;
            f.rBase = i < 66 ? 3u : 4u;
            f.r[0] = (i < 66 ? 0.0 : 2.5) + f.applied[0] + aPrev + 0.001 * rng.next();
            aPrev = f.applied[0];
            hPrev = f.head[0];
            fr.push_back(f);
            f.rBase = 3u;
            oneBase.push_back(f);
        }
        const std::vector<f91::FitResult> R = fitAll(*fw, fr), Q = fitAll(*fw, oneBase);
        check(R.size() == 2 && R[1].h1 && R[1].bestD == 2 && Q.size() == 2 && !Q[1].h1 && Q[1].bestRms > 0.5,
              "F91 B: a new residual base mid-window (r moved 2.5 deg) still fits H1 d 2: the offset is taken out per base (on one base the same series does not fit)");
    }
    delete fw;
}

}  // namespace f91bctest
