// F205/F206 checks (native on-foot VR), run205 below. Header only, run by native_frame_test.
//
// F205 (native_on_foot_f205.h): (1) The full-detail scope (B3) is on by default. (2) While it applies the press takes both eyes whatever fix.native_on_foot_ads_sight_eye says: one effective sight eye,
// decided once at the press's start. The rig runs the effective sight eye over every key and both B3 states and the press latch.
//
// F206 (the pilot's word: "back off on the stencil entirely for all weapons except the sniper with scope mod so that it hides the black vertical bars and then have the widest radius possible that
// does"): the ADS view circle is the full-detail scope's alone. The rig runs which presses have one (B3's latch only: the scoped Executioner with a temporal mode, whatever the key says; no other
// weapon, no B3-less scoped press, no Anti-aliasing-off press, and nothing depends on a sight eye of its own), the request (a centre alone: no radius, no weight, no flat rule, no FOV table), its
// wire form, sanitising and staleness, the lines (the run's, the one for a B3 press that got no circle, health's), and the sites. The radius itself is the runtime's (scope_circle.h coverageRadius,
// run by openxr_stereo_test on known windows) and the weight is the mirror strip's, not the ADS weight.
#pragma once

#include "../../src/common/view_circle.h"
#include "../../src/d3d11/native_on_foot_f205.h"
#include "../../src/d3d11/scope_zoom_s1.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f205test {

namespace nf = edvr::nofoot;
namespace f162 = edvr::nofoot::f162;
namespace f205 = edvr::nofoot::f205;
namespace vc = edvr::viewcircle;
namespace sz = edvr::scopezoom;

inline bool eq(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
inline size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}

template <class Check>
void run205(Check&& check) {
    const char* kExecutioner = "$wpn_m_sniper_plasma_charged_name;";
    // ---- 1. the effective sight eye -----------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        bool all = true;
        for (int key : {int(f162::kBoth), int(f162::kRight), int(f162::kLeft)}) {
            all = all && f205::effectiveSightEye(key, true) == f162::kBoth && f205::effectiveSightEye(key, false) == key;
            const int eff = f205::effectiveSightEye(key, true);
            all = all && f162::aimingEye(eff) == 1 && f162::sharesView(eff);   // forced: the right eye is the aiming eye and the share runs
        }
        check(all, "F205: B3 applying forces both for the key right, left and both (the right eye aims, the share runs); B3 not applying leaves the key as it is");
        check(f205::b3Applies(true, true, true, true) && !f205::b3Applies(false, true, true, true) && !f205::b3Applies(true, false, true, true) && !f205::b3Applies(true, true, false, true) &&
                  !f205::b3Applies(true, true, true, false),
              "F205: B3 applies when its key is on, the scope zoom is on, the held item is its entry and a temporal mode runs, and not otherwise (RC5 review: not with Anti-aliasing off)");
        {   // the RC5 review: with the mode off the latch forces nothing for any key value; with it on it is as before
            bool offKeeps = true, onForces = true;
            for (int key : {int(f162::kBoth), int(f162::kRight), int(f162::kLeft)}) {
                f205::PressLatch off, on;
                off.begin(f205::b3Applies(true, true, true, false), key);
                on.begin(f205::b3Applies(true, true, true, true), key);
                offKeeps = offKeeps && !off.forced && !off.overrode && f205::effectiveSightEye(key, off.forced) == key;
                onForces = onForces && on.forced && on.overrode == (key != f162::kBoth) && f205::effectiveSightEye(key, on.forced) == f162::kBoth;
            }
            check(offKeeps && onForces, "RC5 review: with Anti-aliasing off the scoped Executioner's press forces nothing for the key both, right or left (no override, no line); with a temporal mode it forces both as before");
        }
        check(sz::lensRenderedEntry(kExecutioner, sz::kLensScopeYes) && !sz::lensRenderedEntry(kExecutioner, sz::kLensScopeNo) && !sz::lensRenderedEntry(kExecutioner, sz::kLensScopeUnknown) &&
                  !sz::lensRenderedEntry("$wpn_m_assaultrifle_plasma_fauto_name;", sz::kLensScopeYes),
              "F205: B3's entry is the scoped Executioner alone (the bare one, an unknown scope and the assault rifle are not)");
        // the press latch: decided at the start, kept to the end, a re-engage from the release keeps it, and the player's key is back when the press has fully ended
        f205::PressLatch l;
        l.begin(true, f162::kRight);
        check(l.forced && l.overrode && l.keyAtStart == f162::kRight, "F205: a press that starts with B3 applying and the key right is forced to both, and says so");
        l.reengage();
        check(l.forced && l.overrode, "F205: a press re-engaged from its own release keeps the latch (the lock is mid-ramp and must not change eye)");
        l.end();
        check(!l.forced && !l.overrode, "F205: the press's end gives the player's setting back");
        l.begin(true, f162::kBoth);
        check(l.forced && !l.overrode, "F205: with the key already both the press is forced but nothing is overridden (no line)");
        l.end();
        l.begin(false, f162::kLeft);
        check(!l.forced && !l.overrode && f205::effectiveSightEye(f162::kLeft, l.forced) == f162::kLeft, "F205: a press that starts without B3 applying keeps the key (left aims with the left eye), and a later change does not flip it: the latch is not asked again");
        char line[256];
        f205::formatOverride(line, sizeof(line), f162::kRight);
        const std::string right = line;
        f205::formatOverride(line, sizeof(line), f162::kLeft);
        check(right == "scope zoom (B3): the sight eye is both for this press (fix.native_on_foot_ads_sight_eye is right): full detail needs both eyes" &&
                  std::string(line) == "scope zoom (B3): the sight eye is both for this press (fix.native_on_foot_ads_sight_eye is left): full detail needs both eyes",
              "F205: the press's one line when it overrides names the key's value");
    }
    // ---- 2. which presses have a circle -------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        // the circle is B3's: a press has one exactly when its latch forced both. The scoped Executioner with B3 on, the scope zoom on and a temporal mode is the one such press.
        bool b3Only = true, everyKey = true;
        for (int key : {int(f162::kBoth), int(f162::kRight), int(f162::kLeft)}) {
            f205::PressLatch on, off;
            on.begin(f205::b3Applies(true, true, true, true), key);
            off.begin(f205::b3Applies(true, true, true, false), key);
            everyKey = everyKey && f205::pressHasCircle(on) && !f205::pressHasCircle(off) && f205::effectiveSightEye(key, on.forced) == f162::kBoth;
            // a weapon that is not B3's entry (the bare Executioner, the assault rifle, a pistol), whatever the key says: no circle, no sight-eye condition of its own
            for (const char* item : {"$wpn_m_assaultrifle_plasma_fauto_name;", "$wpn_s_pistol_laser_sauto_name;", "$wpn_m_shotgun_plasma_doublebarrel_name;"}) {
                f205::PressLatch other;
                other.begin(f205::b3Applies(true, true, sz::lensRenderedEntry(item, sz::kLensScopeYes), true), key);
                b3Only = b3Only && !f205::pressHasCircle(other);
            }
            f205::PressLatch bare;
            bare.begin(f205::b3Applies(true, true, sz::lensRenderedEntry(kExecutioner, sz::kLensScopeNo), true), key);
            b3Only = b3Only && !f205::pressHasCircle(bare);
            f205::PressLatch unknown;
            unknown.begin(f205::b3Applies(true, true, sz::lensRenderedEntry(kExecutioner, sz::kLensScopeUnknown), true), key);
            b3Only = b3Only && !f205::pressHasCircle(unknown);
        }
        check(everyKey, "F206: the scoped Executioner's press with B3 on and a temporal mode has the circle for the key both, right and left alike (the latch forces both), and with Anti-aliasing off (no temporal mode) has none");
        check(b3Only, "F206: no other weapon (the assault rifle, a pistol, the shotgun), no bare Executioner and no press of an unknown scope has a circle, for any key");
        bool keysOff = true;
        for (int key : {int(f162::kBoth), int(f162::kRight), int(f162::kLeft)}) {
            f205::PressLatch noScopeZoom, noFull;
            noScopeZoom.begin(f205::b3Applies(true, false, true, true), key);
            noFull.begin(f205::b3Applies(false, true, true, true), key);
            keysOff = keysOff && !f205::pressHasCircle(noScopeZoom) && !f205::pressHasCircle(noFull);
        }
        check(keysOff, "F206: with the scope zoom off, or the full-detail view off, the press has no circle (the circle is part of B3's full-detail view and has no setting of its own)");
        // decided once, with the press's other state: a setting changed in the middle of a press changes nothing, a re-engage from the release keeps it, the press's end clears it
        f205::PressLatch l;
        l.begin(true, f162::kRight);
        const bool began = f205::pressHasCircle(l);
        l.reengage();
        const bool kept = f205::pressHasCircle(l);
        l.end();
        const bool ended = !f205::pressHasCircle(l);
        f205::PressLatch noB3;
        noB3.begin(false, f162::kBoth);
        check(began && kept && ended && !f205::pressHasCircle(noB3), "F206: the press's circle is decided at its engage, kept through a re-engage from the release, and cleared when the press has fully ended; a press without B3 and the key both has none");
        // the request: the centre alone, once it is known, in a press that has a circle
        check(f205::requestOn(true, true) && !f205::requestOn(true, false) && !f205::requestOn(false, true) && !f205::requestOn(false, false),
              "F206: the circle's centre is asked for in a press that has a circle once the sight's far point is known in both eyes, and in no other");
    }
    // ---- 3. nothing of the flat rule is left --------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string s1 = slurp("src/d3d11/scope_zoom_s1.h"), f205h = slurp("src/d3d11/native_on_foot_f205.h"), vch = slurp("src/common/view_circle.h"), cpp = slurp("src/d3d11/native_on_foot.cpp");
        check(!s1.empty() && !f205h.empty() && !vch.empty() && !cpp.empty() && s1.find("adsFovDeg") == std::string::npos && s1.find("kAdsOnlyTable") == std::string::npos && cpp.find("adsFovDeg") == std::string::npos,
              "F206: the circle's ADS FOV table (adsFovDeg, the weapons-only rows) is gone; the lens table stays");
        check(f205h.find("circleRadius") == std::string::npos && f205h.find("halfFovRad") == std::string::npos && vch.find("halfFovRad") == std::string::npos && vch.find("radiusRad") == std::string::npos &&
                  vch.find("struct Ramp") == std::string::npos && vch.find("float weight =") == std::string::npos,
              "F206: the flat rule (ADS FOV / hip FOV x the headset's half-FOV), its helpers, the request's radius and weight and the sight lock's ramp are gone");
        check(sz::lensAdsDeg(kExecutioner, sz::kLensScopeYes, nullptr) && sz::lensAdsDeg(kExecutioner, sz::kLensScopeNo, nullptr) && sz::lensAdsDeg("$wpn_m_assaultrifle_plasma_fauto_name;", sz::kLensScopeNo, nullptr),
              "F206: the lens table the scope zoom magnifies by is untouched (the Executioner's two rows, the Oppressor)");
    }
    // ---- 4. the wire --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        vc::Request r;
        r.on = true;
        r.tc[0][0] = -0.0213f;
        r.tc[0][1] = 0.0342f;
        r.tc[1][0] = 0.0211f;
        r.tc[1][1] = 0.0339f;
        uint32_t w[vc::kWords];
        vc::toWords(r, w);
        const vc::Request back = vc::fromWords(w);
        check(back.on && std::memcmp(back.tc, r.tc, sizeof(r.tc)) == 0 && (w[0] & 0xFFFF0000u) == vc::kMagic && vc::kWordsAt + vc::kWords == 10,
              "F206: the request round-trips through its five words (5..9 of the retired guard's: the flag and the centre in each eye), magic and flag first");
        vc::Request none;
        uint32_t z[vc::kWords];
        vc::toWords(none, z);
        bool zeros = true;
        for (uint32_t v : z) zeros = zeros && v == 0;
        check(zeros && !vc::fromWords(z).on, "F205: no circle is all zeros, which a reader of any age reads as none");
        vc::Request nanR = r, outRange = r, off = r;
        nanR.tc[0][1] = std::numeric_limits<float>::quiet_NaN();
        outRange.tc[1][0] = 9.f;
        off.on = false;
        uint32_t t[vc::kWords];
        bool refused = true;
        for (const vc::Request* q : {&nanR, &outRange, &off}) {
            vc::toWords(*q, t);
            refused = refused && !vc::fromWords(t).on;
        }
        uint32_t noMagic[vc::kWords];
        vc::toWords(r, noMagic);
        noMagic[0] &= 0x0000FFFFu;
        check(refused && !vc::fromWords(noMagic).on, "F206: a centre that is no number or out of range, an off request and a word set without the magic all read as none");
        // staleness: a request not republished is read as published for 400 ms (B3's strip decays in that span), then none
        vc::publish(r, 1000);
        const vc::Request fresh = vc::read(1100), held = vc::read(1399), gone = vc::read(1401);
        check(fresh.on && held.on && std::memcmp(held.tc, r.tc, sizeof(r.tc)) == 0 && !gone.on, "F206: a request not republished is read as published for 400 ms (200 held and 200 for the strip's decay), then as none");
        vc::clear();
        check(!vc::read(1000).on, "F205: a forgotten request reads none");
    }
    // ---- 5. the lines -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        char line[480];
        EdvrNativeViewCircleReport rep{};
        rep.size = sizeof(rep);
        rep.version = EDVR_NATIVE_VIEW_CIRCLE_REPORT_VERSION_2;
        rep.frames = 410;
        rep.refusedFrames = 2;
        rep.sharedFrames = 400;
        rep.seconds = 4.2f;
        rep.radiusDeg = 39.42f;
        rep.radiusMinDeg = 38.9f;
        rep.radiusMaxDeg = 39.6f;
        rep.edgeDeg = 41.17f;
        rep.weightPeak = 1.f;
        rep.centre[0] = 0.512f;
        rep.centre[1] = 0.488f;
        rep.centre[2] = 0.488f;
        rep.centre[3] = 0.488f;
        f205::formatRun(line, sizeof(line), 7, rep);
        const std::string run = line;
        check(f205::reportReadable(&rep) &&
                  run.find("ADS view circle (F206): press 7: radius 39.42 deg (the widest that keeps the full-detail view's side bars in the black: nearest edge 41.17 deg less the band 1.75 deg); over the run 38.90 to 39.60 deg; "
                           "drawn on 410 frames over 4.20 s; refused 2, with the share 400; weight peak 1.00; centre (u, v) left 0.512/0.488 right 0.488/0.488") == 0 &&
                  run.size() < f205::kRunLineBytes,
              "F206: the press's line states the radius, the nearest edge and the band it is less than (the margin and half the 3 deg soft edge: 1.75 deg), the run's range, the frames and the centre, and fits its buffer");
        EdvrNativeViewCircleReport badRep = rep;
        badRep.radiusDeg = std::numeric_limits<float>::quiet_NaN();
        EdvrNativeViewCircleReport oldRep = rep;
        oldRep.version = 1;
        check(!f205::reportReadable(&badRep) && !f205::reportReadable(&oldRep) && !f205::reportReadable(nullptr), "F206: a report with a figure that is no number, from the F205 layout (version 1), or none, is not written");
        // a press that had a circle and a window and got no circle says why, once, after the window has gone
        f205::PressEnd pe;
        pe.hasCircle = pe.windowSeen = true;
        pe.haveCentre = false;
        const bool due = f205::noCircleDue(pe, f205::kNoCircleWaitMs) && !f205::noCircleDue(pe, f205::kNoCircleWaitMs - 1) && f205::noCircleWhy(pe) == f205::NoCircle::kNoCentre;
        pe.haveCentre = true;
        const bool why2 = f205::noCircleWhy(pe) == f205::NoCircle::kNoFrame;
        f205::PressEnd reported = pe;
        reported.runReported = true;
        f205::PressEnd noWindow = pe;
        noWindow.windowSeen = false;
        f205::PressEnd noCircle = pe;
        noCircle.hasCircle = false;
        check(due && why2 && !f205::noCircleDue(reported, 5000) && !f205::noCircleDue(noWindow, 5000) && !f205::noCircleDue(noCircle, 5000),
              "F206: a press that had a circle and a window and no report says why once the window has been gone a second; a press with a run reported, with no window (B3 never stood) or without a circle writes nothing");
        f205::formatNoCircle(line, sizeof(line), 8, f205::NoCircle::kNoCentre, kExecutioner, "yes");
        const std::string nc = line;
        check(nc == "ADS view circle (F206): press 8: no circle: the sight's far point was not known in both eyes while the full-detail view stood (item $wpn_m_sniper_plasma_charged_name;, scope yes)",
              "F206: the line for a B3 press that got no circle names the press, the reason, the item and the scope");
    }
    // ---- 6. the sites -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), frame = slurp("src/d3d11/native_frame.cpp"), client = slurp("src/openxr/native_frame_client.h"), host = slurp("src/openxr/native_runtime_host.h"),
                          ini = slurp("edvr.ini"), build = slurp("build.bat"), stereo = slurp("src/openxr/d3d11_stereo.cpp");
        check(!cpp.empty() && !frame.empty() && !client.empty() && !host.empty() && !ini.empty() && !build.empty() && !stereo.empty(), "F205: the sources the pins read are readable from the repo root");
        // the one point: the key is read in four places only (the config compare, its store, the latch's start and the effective function), everything else asks adsSightEyeMode
        check(countOf(cpp, "g_adsSightEyeKey.load(") == 3 && cpp.find("int adsSightEyeMode() noexcept {\n    return nofoot::f205::effectiveSightEye(g_adsSightEyeKey.load(std::memory_order_relaxed), g_adsSightForceBoth.load(std::memory_order_relaxed));") != std::string::npos &&
                  cpp.find("int adsAimEye() noexcept { return nofoot::f162::aimingEye(adsSightEyeMode()); }") != std::string::npos && cpp.find("const bool key = nofoot::f162::sharesView(adsSightEyeMode());") != std::string::npos &&
                  cpp.find("const bool shareMode = nofoot::f162::sharesView(adsSightEyeMode());") != std::string::npos,
              "F205: the sight eye choice has one reader: the F106 share, B3's gate and the aiming eye all ask adsSightEyeMode, which asks the latch");
        const size_t idle = cpp.find("if (g_adsState == kAdsIdle && g_f205Latch.forced) {"), tick = cpp.find("void adsAimTick(uint64_t now, bool session) noexcept {"),
                     engage = cpp.find("g_f205Latch.begin(applies, g_adsSightEyeKey.load(std::memory_order_relaxed));"), f176 = cpp.find("f176PressBegin(now);   // F176\n"),
                     reengage = cpp.find("f176PressBegin(now);   // F176: an engage from the release is a press too");
        check(tick != std::string::npos && idle != std::string::npos && idle > tick && idle - tick < 400 && engage != std::string::npos && f176 != std::string::npos && engage > f176 && engage - f176 < 600 &&
                  reengage != std::string::npos && cpp.substr(reengage, 600).find("g_f205Latch") == std::string::npos,
              "F205: the latch starts at the engage from idle (and nowhere else: a re-engage from the release does not touch it) and ends at the top of the tick once the press has fully ended");
        check(cpp.find("if (!g_scopeZoomFullKey.load(std::memory_order_relaxed) || !g_scopeZoomKey.load(std::memory_order_relaxed) || !temporalPassWantsFssChrome()) return false;") != std::string::npos &&
                  cpp.find("edvr::scopezoom::lensRenderedEntry(it.name, it.scope), true);") != std::string::npos,
              "F205: B3 applies by its key, the scope zoom's, the held item's table row and a temporal mode (RC5 review: the very mode b3Tick gates the full detail on)");
        check(cpp.find("std::atomic<bool> g_scopeZoomFullKey{true};") != std::string::npos && cpp.find("cfg.getString(\"experimental.native_on_foot_ads_scope_zoom_full_detail\", \"on\") == \"on\"") != std::string::npos &&
                  ini.find("native_on_foot_ads_scope_zoom_full_detail = on\n") != std::string::npos && ini.find("# ui: ADS: scope zoom full detail (experimental) | choices on, off | recommended on | live\n") != std::string::npos,
              "F205: the full-detail scope is on by default in the code, the reader's fallback, the ini line and its menu row");
        // the circle has no key of its own (F205b) and no flat rule (F206)
        const std::string menuTest = slurp("tools/hotkey_capture_test/hotkey_capture_test.cpp");
        check(!menuTest.empty() && cpp.find("ads_view_circle") == std::string::npos && cpp.find("g_adsViewCircleKey") == std::string::npos && ini.find("ads_view_circle") == std::string::npos &&
                  ini.find("ADS: view circle") == std::string::npos && menuTest.find("ads_view_circle") == std::string::npos && frame.find("ads_view_circle") == std::string::npos && host.find("ads_view_circle") == std::string::npos,
              "F205b: the circle has no setting of its own: no reader, atomic or log note in the code, no line or row in edvr.ini, no pin in the menu's test");
        check(ini.find("# The scoped Executioner's full-detail view (ADS: scope zoom full detail, below) is part of this setting: its two black side bars are hidden by a circle") != std::string::npos &&
                  ini.find("a circle, black outside, about the sight in each eye,\n# the widest that still hides those black bars, comes and goes with them (it is part of this view and has no setting of its own).") != std::string::npos &&
                  ini.find("shows each eye's view inside a circle") == std::string::npos,
              "F206: the scope zoom's ini row and the full-detail row say the circle is the full-detail view's: the scoped Executioner's, the widest that hides its bars, with no setting of its own");
        // the circle's tick: the centre alone, from the press's latch; no flat rule, no weight, no ramp, no ADS weight, no temporal mode, no sight eye or key of its own
        const size_t tickAt = cpp.find("void circleTick(uint64_t nowMs) noexcept {"), tickEnd = cpp.find("\nvoid tick(uint64_t nowMs) noexcept {", tickAt);
        const std::string body = tickAt == std::string::npos || tickEnd == std::string::npos ? std::string() : cpp.substr(tickAt, tickEnd - tickAt);
        check(!body.empty() && body.find("c.hasCircle = nofoot::f205::pressHasCircle(g_f205Latch);") != std::string::npos && countOf(body, "pressHasCircle(") == 1 &&
                  body.find("req.on = nofoot::f205::requestOn(c.hasCircle, c.haveCentre);") != std::string::npos && body.find("radius") == std::string::npos && body.find("weight") == std::string::npos &&
                  body.find("ramp") == std::string::npos && body.find("g_adsW") == std::string::npos && body.find("adsFov") == std::string::npos && body.find("g_scopeZoomKey") == std::string::npos &&
                  body.find("temporalPass") == std::string::npos && body.find("adsSightEyeMode") == std::string::npos && body.find("g_adsSightEyeKey") == std::string::npos &&
                  body.find("s0::geometryNow(v, nowMs)") != std::string::npos && body.find("scopewin::read(nowMs)") != std::string::npos,
              "F206: the circle's tick decides the press's circle once from the F205 latch and asks for the centre alone (from S and the scene rows) in such a press: no radius, weight, ramp, ADS weight, FOV table, "
              "temporal mode, sight eye or setting of its own");
        check(cpp.find("s1::circleTick(GetTickCount64());") != std::string::npos && cpp.find("extern \"C\" long __stdcall edvrNoteNativeViewCircle(const EdvrNativeViewCircleReport* r) {") != std::string::npos &&
                  cpp.find("edvr::nofoot::f205::formatRun(line, sizeof(line), press, *r);") != std::string::npos && build.find("--extra-export edvrNoteNativeViewCircle") != std::string::npos,
              "F205: the circle is published every tick after the lens tick, and the runtime's report has its export (it writes the press's line)");
        check(frame.find("edvr::viewcircle::toWords(edvr::viewcircle::read(GetTickCount64()), circleWords);") != std::string::npos && frame.find("result.reservedTerrainGuard + edvr::viewcircle::kWordsAt") != std::string::npos &&
                  client.find("for(unsigned i=0;i<12;++i)out.reservedTerrainGuard[i]=0;") != std::string::npos &&
                  host.find("circleNow=carries?viewcircle::fromWords(featureFrame.reservedTerrainGuard+viewcircle::kWordsAt):viewcircle::Request{};") != std::string::npos &&
                  host.find("&frameOrigin,&circleArg);") != std::string::npos && host.find("&previousOrigin,&circleArg):") != std::string::npos && host.find("circlePress(r==XR_SUCCESS);") != std::string::npos,
              "F206: the request rides words 5..9 of a version 11 answer (older answers read none), the host reads it every frame, draws it on a fresh and on a replayed pair, and reports each run");
        // the runtime draws the circle with the mirror, by its weight, at the radius that hides the bars: the three pins of the one point
        const size_t ringAt = stereo.find("if(circle&&circle->on&&mirrorOn&&mirrorWeight>0.f&&fade<1.f) {"), ringEnd = stereo.find("auto failed=[&](XrResult r)", ringAt);
        const std::string ring = ringAt == std::string::npos || ringEnd == std::string::npos ? std::string() : stereo.substr(ringAt, ringEnd - ringAt);
        check(!ring.empty() && ring.find("radius=scopecircle::coverageRadius(shareMap,mirrorMap,circle->tc[0],circle->tc[1]);") != std::string::npos && ring.find("k.band[2]=mirrorWeight;") != std::string::npos &&
                  ring.find("radiusRad") == std::string::npos && ring.find("circle->weight") == std::string::npos && stereo.find("mirrorOn=true;mirrorMap=mm;mirrorWeight=weight;") != std::string::npos &&
                  stereo.find("const float weight=leftZoomed?1.f:leftStripBlack;") != std::string::npos,
              "F206: the renderer draws the circle only while the mirror is, by the mirror's own weight (the strip's, full on a narrowed frame; never the ADS weight), at the radius coverageRadius works out from the actual centre each frame");
    }
}

}  // namespace f205test
