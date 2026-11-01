// F205 checks (native on-foot VR), run205 below. Header only, run by native_frame_test.
//
// F205 (native_on_foot_f205.h, view_circle.h, scope_zoom_s1.h adsFovDeg; native_on_foot.cpp; the runtime's renderer is openxr_stereo_test's): three things the pilot asked for. (1) The full-detail scope (B3)
// is on by default. (2) While it applies the press takes both eyes whatever fix.native_on_foot_ads_sight_eye says: one effective sight eye, decided once at the press's start. (3) Every ADS press shows each
// eye's view inside a circle, black outside, whose angular radius is (ADS FOV / hip FOV) x the headset's vertical half-FOV, as flat's narrowed view loses its periphery. The rig runs the effective sight eye over
// every key and both B3 states and the press latch; the ADS FOV table; the radius rule on the Pimax and the Quest 3 for the Executioner's two FOVs and the others, an unknown FOV, a frustum that is no eye, the key
// off and a ratio past 1; that no input of the circle looks at the Anti-aliasing mode (the same inputs give the same radius and request on and off); the request's wire form, sanitising, staleness and ramp; the
// press, runtime and override lines; and the sites.
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
    constexpr double kDeg = 180.0 / vc::kPi;
    const double nan = std::numeric_limits<double>::quiet_NaN();
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
    // ---- 2. the ADS FOV of every measured weapon ----------------------------------------------------------------------------------------------------------------------------------------------
    {
        struct Want { const char* item; int scope; double deg; bool bare; };
        const Want wants[] = {
            {"$wpn_m_sniper_plasma_charged_name;", sz::kLensScopeYes, 15.0, false}, {"$wpn_m_sniper_plasma_charged_name;", sz::kLensScopeNo, 35.0, false},
            {"$wpn_m_assaultrifle_plasma_fauto_name;", sz::kLensScopeNo, 48.0, false}, {"$wpn_m_assaultrifle_plasma_fauto_name;", sz::kLensScopeYes, 48.0, true},
            {"$wpn_m_assaultrifle_laser_fauto_name;", sz::kLensScopeNo, 48.0, false}, {"$wpn_m_assaultrifle_kinetic_fauto_name;", sz::kLensScopeNo, 48.0, false},
            {"$wpn_m_assaultrifle_kinetic_fauto_name;", sz::kLensScopeYes, 48.0, true}, {"$wpn_m_submachinegun_kinetic_fauto_name;", sz::kLensScopeNo, 50.0, false},
            {"$wpn_m_submachinegun_laser_fauto_name;", sz::kLensScopeNo, 50.0, false}, {"$wpn_m_submachinegun_laser_fauto_name;", sz::kLensScopeYes, 50.0, true},
            {"$wpn_m_shotgun_plasma_doublebarrel_name;", sz::kLensScopeNo, 52.0, false}, {"$wpn_s_pistol_plasma_charged_name;", sz::kLensScopeNo, 52.0, false},
            {"$wpn_s_pistol_kinetic_sauto_name;", sz::kLensScopeNo, 52.0, false}, {"$wpn_s_pistol_laser_sauto_name;", sz::kLensScopeNo, 52.0, false},
            {"$wpn_m_launcher_rocket_sauto_name;", sz::kLensScopeNo, 52.0, false}, {"wpn_s_pistol_laser_sauto", sz::kLensScopeNo, 52.0, false},
        };
        bool all = true;
        for (const Want& w : wants) {
            double deg = nan;
            bool bare = true;
            all = all && sz::adsFovDeg(w.item, w.scope, &deg, &bare) && eq(deg, w.deg, 1e-12) && bare == w.bare;
        }
        check(all, "F205: the ADS FOV of every weapon the flat flights measured (15 scoped and 35 bare Executioner, 48 assault rifles, 50 sub-machine guns, 52 shotgun, pistols and launcher) comes from the table, an unmeasured scoped copy taking its item's");
        double deg = 1.0;
        check(!sz::adsFovDeg("$wpn_m_unmeasured_thing_name;", sz::kLensScopeNo, &deg) && std::isnan(deg) && !sz::adsFovDeg(kExecutioner, sz::kLensScopeUnknown, &deg) &&
                  !sz::adsFovDeg("$wpn_s_pistol_laser_sauto_name;", sz::kLensScopeUnknown, &deg) && !sz::adsFovDeg("", sz::kLensScopeNo, &deg) && !sz::adsFovDeg(nullptr, sz::kLensScopeNo, &deg),
              "F205: a weapon no row names, an unknown scope state, an empty name: no ADS FOV (no circle, never a guess)");
    }
    // ---- 3. the radius ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        // the headsets' vertical half-FOV: the Pimax's tangents up 1.2648 and down 1.2648, the Quest 3's up 0.9657 and down 1.4281 (L and D negative: temporalPassEyeTan's order)
        const float pimax[4] = {-1.2543f, 0.8746f, -1.2648f, 1.2648f}, quest[4] = {-1.3498f, 0.8213f, -1.4281f, 0.9657f}, session[4] = {-1.2543f, 0.8746f, -1.065f, 1.065f};
        const double vPimax = vc::halfFovRad(pimax), vQuest = vc::halfFovRad(quest), vSession = vc::halfFovRad(session);
        check(eq(vPimax * kDeg, std::atan(1.2648) * kDeg, 1e-4) && eq(vPimax * kDeg, 51.67, 0.01) && eq(vQuest * kDeg, 0.5 * (std::atan(0.9657) + std::atan(1.4281)) * kDeg, 1e-4) && eq(vQuest * kDeg, 49.50, 0.01) &&
                  eq(vSession * kDeg, 46.80, 0.01),
              "F205: the half-FOV is the vertical half-angle of the eye's frustum: 51.67 deg on the Pimax (up and down 1.2648), 49.50 on the Quest 3 (up 0.9657, down 1.4281, not symmetric), 46.80 on the pilot's 04:12 session (1.065)");
        // the flat rule: the ADS FOV over the hip FOV (56.3) times the half-FOV
        struct Row { double ads; double pimaxDeg, questDeg; };
        const Row rows[] = {{15.0, 13.77, 13.19}, {35.0, 32.12, 30.77}, {48.0, 44.05, 42.20}, {50.0, 45.89, 43.96}, {52.0, 47.72, 45.72}};
        bool rule = true;
        for (const Row& r : rows) {
            const f205::Radius rp = f205::circleRadius(true, true, r.ads, 56.3, pimax), rq = f205::circleRadius(true, true, r.ads, 56.3, quest);
            rule = rule && rp.why == f205::Why::kOk && rq.why == f205::Why::kOk && eq(rp.radiusRad * kDeg, r.ads / 56.3 * vPimax * kDeg, 1e-9) && eq(rq.radiusRad * kDeg, r.ads / 56.3 * vQuest * kDeg, 1e-9) &&
                   eq(rp.radiusRad * kDeg, r.pimaxDeg, 0.02) && eq(rq.radiusRad * kDeg, r.questDeg, 0.02);
        }
        check(rule, "F205: the radius is ADS FOV / hip FOV x the half-FOV: the scoped Executioner 13.77 deg on the Pimax and 13.19 on the Quest 3, the bare one 32.12 and 30.77, the assault rifles 44.05 and 42.20, "
                    "the sub-machine guns 45.89 and 43.96, the pistols, shotgun and launcher 47.72 and 45.72");
        const f205::Radius off = f205::circleRadius(false, true, 15.0, 56.3, pimax), unknown = f205::circleRadius(true, false, nan, 56.3, pimax), noFov = f205::circleRadius(true, true, nan, 56.3, pimax),
                           noHip = f205::circleRadius(true, true, 15.0, nan, pimax), noEye = f205::circleRadius(true, true, 15.0, 56.3, nullptr);
        const float badEye[4] = {-1.f, 1.f, 1.f, -1.f};   // up below down: not an eye
        const f205::Radius flipped = f205::circleRadius(true, true, 15.0, 56.3, badEye);
        check(off.why == f205::Why::kScopeZoomOff && !std::isfinite(off.radiusRad) && unknown.why == f205::Why::kAdsFovUnknown && noFov.why == f205::Why::kAdsFovUnknown && noHip.why == f205::Why::kAdsFovUnknown &&
                  noEye.why == f205::Why::kNoFrustum && flipped.why == f205::Why::kNoFrustum && !std::isfinite(unknown.radiusRad) && !std::isfinite(noEye.radiusRad),
              "F205b: the scope zoom off gives no circle; an ADS FOV or hip FOV that is not known, and a frustum that is not an eye, give none with their reason, and never a size");
        // F205b: the circle's one switch is the scope zoom setting: on, every weapon with a measured ADS FOV has its circle; off, none, for every weapon, and the line says so
        bool follows = true;
        for (double ads : {15.0, 35.0, 48.0, 50.0, 52.0}) {
            const f205::Radius onR = f205::circleRadius(true, true, ads, 56.3, pimax), offR = f205::circleRadius(false, true, ads, 56.3, pimax);
            follows = follows && onR.why == f205::Why::kOk && std::isfinite(onR.radiusRad) && offR.why == f205::Why::kScopeZoomOff && !std::isfinite(offR.radiusRad);
        }
        char offLine[240];
        f205::formatLine(offLine, sizeof(offLine), 4, "off", f205::circleRadius(false, true, 15.0, 56.3, pimax), 15.0, 56.3, false, kExecutioner, "yes");
        check(follows && std::string(offLine) == "ADS view circle (F205): press 4: no circle (fix.native_on_foot_ads_scope_zoom is off)",
              "F205b: the circle follows the scope zoom setting on and off for every weapon, and with it off the press line says: press N: no circle (fix.native_on_foot_ads_scope_zoom is off)");
        const f205::Radius wide = f205::circleRadius(true, true, 80.0, 56.3, pimax);
        check(wide.why == f205::Why::kOk && eq(wide.radiusRad, vPimax, 1e-12), "F205: an ADS FOV wider than the hip is held to the whole half-FOV");
        // Anti-aliasing on and off: the rule takes no mode; the same inputs are the same radius, the same request and the same wire form
        const f205::Radius a = f205::circleRadius(true, true, 15.0, 56.3, pimax), b = f205::circleRadius(true, true, 15.0, 56.3, pimax);
        vc::Request ra, rb;
        ra.on = rb.on = true;
        ra.radiusRad = float(a.radiusRad);
        rb.radiusRad = float(b.radiusRad);
        ra.weight = rb.weight = 1.f;
        uint32_t wa[vc::kWords], wb[vc::kWords];
        vc::toWords(ra, wa);
        vc::toWords(rb, wb);
        check(a.radiusRad == b.radiusRad && std::memcmp(wa, wb, sizeof(wa)) == 0, "F205: the radius and the request are functions of the weapon, the hip FOV and the eye's frustum alone: the same inputs, the same words, with a temporal mode on or off");
        char la[420], lb[420];
        f205::formatLine(la, sizeof(la), 1, "on", a, 15.0, 56.3, false, kExecutioner, "yes");
        f205::formatLine(lb, sizeof(lb), 1, "off", b, 15.0, 56.3, false, kExecutioner, "yes");
        std::string sa = la, sb = lb;
        const size_t pa = sa.find("Anti-aliasing on"), pb = sb.find("Anti-aliasing off");
        check(pa != std::string::npos && pb != std::string::npos && sa.replace(pa, 16, "Anti-aliasing off") == sb,
              "F205: the press line differs between the two modes only in the word that states the mode, so one flight in each can be compared");
    }
    // ---- 4. the wire --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        vc::Request r;
        r.on = true;
        r.radiusRad = 0.2403f;
        r.weight = 0.75f;
        r.tc[0][0] = -0.0213f;
        r.tc[0][1] = 0.0342f;
        r.tc[1][0] = 0.0211f;
        r.tc[1][1] = 0.0339f;
        uint32_t w[vc::kWords];
        vc::toWords(r, w);
        const vc::Request back = vc::fromWords(w);
        check(back.on && back.radiusRad == r.radiusRad && back.weight == r.weight && std::memcmp(back.tc, r.tc, sizeof(r.tc)) == 0 && (w[0] & 0xFFFF0000u) == vc::kMagic && vc::kWordsAt + vc::kWords == 12,
              "F205: the request round-trips through its seven words (5..11 of the retired guard's), magic and flag first");
        vc::Request none;
        uint32_t z[vc::kWords];
        vc::toWords(none, z);
        bool zeros = true;
        for (uint32_t v : z) zeros = zeros && v == 0;
        check(zeros && !vc::fromWords(z).on, "F205: no circle is all zeros, which a reader of any age reads as none");
        vc::Request bad = r;
        bad.weight = 0.f;
        vc::Request nanR = r, big = r, outRange = r, off = r;
        nanR.radiusRad = std::numeric_limits<float>::quiet_NaN();
        big.radiusRad = 1.5f;
        outRange.tc[1][0] = 9.f;
        off.on = false;
        uint32_t t[vc::kWords];
        bool refused = true;
        for (const vc::Request* q : {&bad, &nanR, &big, &outRange, &off}) {
            vc::toWords(*q, t);
            refused = refused && !vc::fromWords(t).on;
        }
        uint32_t noMagic[vc::kWords];
        vc::toWords(r, noMagic);
        noMagic[0] &= 0x0000FFFFu;
        check(refused && !vc::fromWords(noMagic).on, "F205: a weight of 0, a radius that is no number or past 80 degrees, a centre out of range, an off request and a word set without the magic all read as none");
        // staleness: a request not republished decays over 200 ms after 200 ms of silence
        vc::publish(r, 1000);
        const vc::Request fresh = vc::read(1100), mid = vc::read(1300), gone = vc::read(1500);
        check(fresh.on && fresh.weight == 0.75f && mid.on && mid.weight > 0.f && mid.weight < 0.75f && !gone.on, "F205: a request not republished decays to none after 400 ms (200 held, 200 ramping)");
        vc::clear();
        check(!vc::read(1000).on, "F205: a forgotten request reads none");
        vc::Ramp ramp;
        ramp.step(true, 0.075);
        const double half = ramp.w;
        ramp.step(true, 0.2);
        const double full = ramp.w;
        ramp.step(false, 0.15);
        const double out = ramp.w;
        ramp.step(false, 1.0);
        check(eq(half, 0.5, 1e-9) && full == 1.0 && eq(out, 0.5, 1e-9) && ramp.w == 0.0, "F205: the weight comes in over 0.15 s and goes over 0.3 s, with the sight lock's times");
    }
    // ---- 5. the lines -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const float pimax[4] = {-1.2543f, 0.8746f, -1.2648f, 1.2648f};
        const f205::Radius r = f205::circleRadius(true, true, 15.0, 56.3, pimax);
        char line[480];
        const float tc[2][2] = {{-0.0213f, 0.0342f}, {0.0211f, 0.0339f}};
        f205::formatLine(line, sizeof(line), 7, "off", r, 15.0, 56.3, false, kExecutioner, "yes", tc);
        const std::string s = line;
        check(s.find("ADS view circle (F205): press 7, Anti-aliasing off: radius 13.77 deg (ADS FOV 15.00 / hip FOV 56.30 x the headset's half-FOV 51.67 deg, vertical)") == 0 &&
                  s.find("; item $wpn_m_sniper_plasma_charged_name;, scope yes; centre (tangents) left -0.0213/+0.0342 right +0.0211/+0.0339") != std::string::npos,
              "F205: the press line states the radius with its three figures, the Anti-aliasing mode, the item and scope and the centre in each eye");
        check(s.size() > 24 && s.compare(s.size() - 23, 23, " (the scope zoom is on)") == 0, "F205b: ... and ends with what switched it: (the scope zoom is on)");
        const f205::Radius none = f205::circleRadius(true, false, nan, 56.3, pimax);
        f205::formatLine(line, sizeof(line), 8, "on", none, nan, 56.3, false, "$wpn_m_unmeasured_thing_name;", "no");
        check(std::string(line) == "ADS view circle (F205): press 8, Anti-aliasing on: no circle: the ADS FOV of the held weapon is not known (item $wpn_m_unmeasured_thing_name;, scope no)",
              "F205: a weapon with no ADS FOV says why it has no circle");
        EdvrNativeViewCircleReport rep{};
        rep.size = sizeof(rep);
        rep.version = EDVR_NATIVE_VIEW_CIRCLE_REPORT_VERSION_1;
        rep.frames = 410;
        rep.cappedFrames = 0;
        rep.refusedFrames = 2;
        rep.sharedFrames = 400;
        rep.seconds = 4.2f;
        rep.radiusDeg = 13.77f;
        rep.requestedDeg = 13.77f;
        rep.weightPeak = 1.f;
        rep.centre[0] = 0.512f;
        rep.centre[1] = 0.488f;
        rep.centre[2] = 0.488f;
        rep.centre[3] = 0.488f;
        f205::formatRuntime(line, sizeof(line), rep);
        check(f205::reportReadable(&rep) && std::string(line).find("ADS view circle (F205) runtime: ADS view circle on 410 frames, radius 13.77 deg (the d3d11 half asked 13.77 deg), over 4.20 s; refused 2, with the share 400; weight peak 1.00;") == 0 &&
                  std::strlen(line) < f205::kRuntimeLineBytes,
              "F205: the runtime's line says the frames drawn and the radius, and fits its buffer");
        EdvrNativeViewCircleReport badRep = rep;
        badRep.radiusDeg = std::numeric_limits<float>::quiet_NaN();
        EdvrNativeViewCircleReport oldRep = rep;
        oldRep.version = 0;
        check(!f205::reportReadable(&badRep) && !f205::reportReadable(&oldRep) && !f205::reportReadable(nullptr), "F205: a report with a figure that is no number, from another version, or none, is not written");
    }
    // ---- 6. the sites -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), frame = slurp("src/d3d11/native_frame.cpp"), client = slurp("src/openxr/native_frame_client.h"), host = slurp("src/openxr/native_runtime_host.h"),
                          ini = slurp("edvr.ini"), build = slurp("build.bat");
        check(!cpp.empty() && !frame.empty() && !client.empty() && !host.empty() && !ini.empty() && !build.empty(), "F205: the sources the pins read are readable from the repo root");
        // the one point: the key is read in four places only (the config compare, its store, the latch's start and the effective function), everything else asks adsSightEyeMode
        check(countOf(cpp, "g_adsSightEyeKey.load(") == 3 && cpp.find("int adsSightEyeMode() noexcept {\n    return nofoot::f205::effectiveSightEye(g_adsSightEyeKey.load(std::memory_order_relaxed), g_adsSightForceBoth.load(std::memory_order_relaxed));") != std::string::npos &&
                  cpp.find("int adsAimEye() noexcept { return nofoot::f162::aimingEye(adsSightEyeMode()); }") != std::string::npos && cpp.find("const bool key = nofoot::f162::sharesView(adsSightEyeMode());") != std::string::npos &&
                  cpp.find("const bool shareMode = nofoot::f162::sharesView(adsSightEyeMode());") != std::string::npos,
              "F205: the sight eye choice has one reader: the F106 share, B3's gate and the aiming eye all ask adsSightEyeMode, which asks the latch");
        const size_t idle = cpp.find("if (g_adsState == kAdsIdle && g_f205Latch.forced) {"), tick = cpp.find("void adsAimTick(uint64_t now, bool session) noexcept {"), engage = cpp.find("g_f205Latch.begin(applies, g_adsSightEyeKey.load(std::memory_order_relaxed));"),
                     f176 = cpp.find("f176PressBegin(now);   // F176\n"), reengage = cpp.find("f176PressBegin(now);   // F176: an engage from the release is a press too");
        check(tick != std::string::npos && idle != std::string::npos && idle > tick && idle - tick < 400 && engage != std::string::npos && f176 != std::string::npos && engage > f176 && engage - f176 < 600 &&
                  reengage != std::string::npos && cpp.substr(reengage, 600).find("g_f205Latch") == std::string::npos,
              "F205: the latch starts at the engage from idle (and nowhere else: a re-engage from the release does not touch it) and ends at the top of the tick once the press has fully ended");
        check(cpp.find("if (!g_scopeZoomFullKey.load(std::memory_order_relaxed) || !g_scopeZoomKey.load(std::memory_order_relaxed) || !temporalPassWantsFssChrome()) return false;") != std::string::npos &&
                  cpp.find("edvr::scopezoom::lensRenderedEntry(it.name, it.scope), true);") != std::string::npos,
              "F205: B3 applies by its key, the scope zoom's, the held item's table row and a temporal mode (RC5 review: the very mode b3Tick gates the full detail on)");
        check(cpp.find("std::atomic<bool> g_scopeZoomFullKey{true};") != std::string::npos && cpp.find("cfg.getString(\"experimental.native_on_foot_ads_scope_zoom_full_detail\", \"on\") == \"on\"") != std::string::npos &&
                  ini.find("native_on_foot_ads_scope_zoom_full_detail = on\n") != std::string::npos && ini.find("# ui: ADS: scope zoom full detail (experimental) | choices on, off | recommended on | live\n") != std::string::npos,
              "F205: the full-detail scope is on by default in the code, the reader's fallback, the ini line and its menu row");
        // F205b: the circle's own key (fix.native_on_foot_ads_view_circle) is gone from the code, the ini and the menu's test, and the circle reads the lens's key by the lens's one reader
        const std::string menuTest = slurp("tools/hotkey_capture_test/hotkey_capture_test.cpp");
        check(!menuTest.empty() && cpp.find("ads_view_circle") == std::string::npos && cpp.find("g_adsViewCircleKey") == std::string::npos && ini.find("ads_view_circle") == std::string::npos &&
                  ini.find("ADS: view circle") == std::string::npos && menuTest.find("ads_view_circle") == std::string::npos && frame.find("ads_view_circle") == std::string::npos && host.find("ads_view_circle") == std::string::npos,
              "F205b: fix.native_on_foot_ads_view_circle is gone: no reader, atomic or log note in the code, no line or row in edvr.ini, no pin in the menu's test");
        check(countOf(cpp, "g_scopeZoomKey.store(") == 1 && countOf(cpp, "cfg.getString(\"fix.native_on_foot_ads_scope_zoom\", \"on\")") == 1 &&
                  cpp.find("    const bool key = g_scopeZoomKey.load(std::memory_order_relaxed);\n    const char* mode = temporalPassWantsFssChrome()") != std::string::npos &&
                  ini.find("# This setting also decides the ADS view circle: while it is on, every ADS press") != std::string::npos,
              "F205b: the circle reads g_scopeZoomKey, the one atomic the lens's one reader sets (fix.native_on_foot_ads_scope_zoom, live), and the key's row says it also decides the circle");
        // no gate on a temporal mode: the circle's tick names the mode once, in the string the press line states, and nowhere in its decisions
        const size_t tickAt = cpp.find("void circleTick(uint64_t nowMs) noexcept {"), tickEnd = cpp.find("\nvoid tick(uint64_t nowMs) noexcept {", tickAt);
        const std::string body = tickAt == std::string::npos || tickEnd == std::string::npos ? std::string() : cpp.substr(tickAt, tickEnd - tickAt);
        check(!body.empty() && countOf(body, "temporalPassWantsFssChrome") == 1 && body.find("const char* mode = temporalPassWantsFssChrome() ? \"on\" : \"off\";") != std::string::npos &&
                  body.find("doorFromPass") == std::string::npos && body.find("uiLayerLive") == std::string::npos && body.find("g_uiLayerLive") == std::string::npos && body.find("uiDepth") == std::string::npos &&
                  body.find("const bool want = key && up && r.why == nofoot::f205::Why::kOk && fresh;") != std::string::npos && body.find("temporalPassEyeTan(1, tan4)") != std::string::npos &&
                  body.find("s0::geometryNow(v, nowMs)") != std::string::npos,
              "F205: the circle's tick asks for no temporal mode, interface layer, door or ui depth: its inputs are S and the scene rows (F196), the weapon's table row, the scope zoom's hip FOV and the eye's frustum (F198), all of which exist with Anti-aliasing off");
        check(cpp.find("s1::circleTick(GetTickCount64());") != std::string::npos && cpp.find("extern \"C\" long __stdcall edvrNoteNativeViewCircle(const EdvrNativeViewCircleReport* r) {") != std::string::npos &&
                  build.find("--extra-export edvrNoteNativeViewCircle") != std::string::npos,
              "F205: the circle is published every tick after the lens tick, and the runtime's report has its export");
        check(frame.find("edvr::viewcircle::toWords(edvr::viewcircle::read(GetTickCount64()), circleWords);") != std::string::npos && frame.find("result.reservedTerrainGuard + edvr::viewcircle::kWordsAt") != std::string::npos &&
                  client.find("for(unsigned i=0;i<12;++i)out.reservedTerrainGuard[i]=0;") != std::string::npos &&
                  host.find("circleNow=carries?viewcircle::fromWords(featureFrame.reservedTerrainGuard+viewcircle::kWordsAt):viewcircle::Request{};") != std::string::npos &&
                  host.find("&frameOrigin,&circleArg);") != std::string::npos && host.find("&previousOrigin,&circleArg):") != std::string::npos && host.find("circlePress(r==XR_SUCCESS);") != std::string::npos,
              "F205: the request rides words 5..11 of a version 11 answer (older answers read none), the host reads it every frame, draws it on a fresh and on a replayed pair, and reports each run");
    }
}

}  // namespace f205test
