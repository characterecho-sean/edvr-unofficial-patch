// S1/A1 checks (the scope lens), runS1 below. Header only, run by native_frame_test.
//
// A1 (src\d3d11\scope_zoom_s1.h): the pure parts of the lens. The table holds the game's ADS FOV per (item, scope) and M is tan(h / 2) / tan(ads / 2) against the reference FOV; the gate
// says when there is a lens and under which reason a frame has none; the ease takes M from 1 to its value over 0.25 s; the rays march from c over the depth for the hole of the scope's
// frame (the shader's text mirrors the reference march step for step, tools\scope_zoom_test runs the two against each other); the open-sight rule, r(theta), the mask, the source
// of a magnified pixel, the window and the box are the passes' arithmetic; the accumulators and the two lines are the DLL's own, which tools\edvr_log.py renders its --health
// fixtures from.
#pragma once

#include "../../src/d3d11/scope_zoom_s1.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace s1test {

namespace sz = edvr::scopezoom;

inline bool near1(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
inline double kNaNd() { return sz::kNaN; }

// A convex hexagonal frame of apothem a about (hx, hy): six half planes with the normals at 0, 60, ..., 300 degrees (rotated by rot). The frame is the band of width w outside it.
struct Hex {
    double hx, hy, a, w, rot;
    double metric(double x, double y) const {
        double m = -1.0e30;
        for (int i = 0; i < 6; ++i) {
            const double ang = rot + i * (sz::kPi / 3.0);
            m = std::fmax(m, (x - hx) * std::cos(ang) + (y - hy) * std::sin(ang));
        }
        return m;
    }
    bool frame(double x, double y) const { const double m = metric(x, y); return m >= a && m <= a + w; }
    // The distance along a ray from a point inside to the hexagon's inner edge.
    double cast(double cx, double cy, double dx, double dy) const {
        double t = 1.0e30;
        for (int i = 0; i < 6; ++i) {
            const double ang = rot + i * (sz::kPi / 3.0);
            const double nx = std::cos(ang), ny = std::sin(ang);
            const double along = nx * dx + ny * dy;
            if (along <= 1.0e-12) continue;
            t = std::fmin(t, (a - ((cx - hx) * nx + (cy - hy) * ny)) / along);
        }
        return t;
    }
};

template <class Check>
inline void runS1(Check&& check) {
    // ---- the table: the stem, the entries, M ----------------------------------------------------------------------------------------------------------------------------------
    {
        char stem[64];
        check(sz::lensStem("$wpn_m_sniper_plasma_charged_name;", stem, sizeof(stem)) && std::strcmp(stem, "m_sniper_plasma_charged") == 0,
              "S1 table: Status.json's name is reduced to its stem ($, wpn_, _name and ; taken off)");
        check(sz::lensStem("wpn_m_sniper_plasma_charged", stem, sizeof(stem)) && std::strcmp(stem, "m_sniper_plasma_charged") == 0, "S1 table: the journal's module name is the same stem");
        check(sz::lensStem("$WPN_M_Sniper_Plasma_Charged_NAME;", stem, sizeof(stem)) && std::strcmp(stem, "m_sniper_plasma_charged") == 0, "S1 table: the stem is read without case");
        check(!sz::lensStem(nullptr, stem, sizeof(stem)) && !sz::lensStem("", stem, sizeof(stem)) && !sz::lensStem("$wpn__name;", stem, sizeof(stem)) && !sz::lensStem("none", nullptr, 0),
              "S1 table: no name, an empty name and a name with no stem left have none");
        check(!sz::lensStem("$wpn_m_sniper_plasma_charged_name;", stem, 8), "S1 table: a stem that does not fit its buffer is refused, not cut");
        check(sz::lensTableEntries() == 7, "S1 table: seven (item, scope) entries: the Executioner both ways, the Oppressor and the Aphelion both ways, the scoped Eclipse");
        struct Want { const char* item; int scope; double deg; bool fromBare; };
        static const Want want[] = {
            {"$wpn_m_sniper_plasma_charged_name;", sz::kLensScopeYes, 15.0, false}, {"$wpn_m_sniper_plasma_charged_name;", sz::kLensScopeNo, 35.0, false},
            {"$wpn_m_assaultrifle_plasma_fauto_name;", sz::kLensScopeNo, 48.0, false}, {"$wpn_m_assaultrifle_plasma_fauto_name;", sz::kLensScopeYes, 48.0, true},
            {"$wpn_m_assaultrifle_laser_fauto_name;", sz::kLensScopeNo, 48.0, false}, {"$wpn_m_assaultrifle_laser_fauto_name;", sz::kLensScopeYes, 48.0, true},
            {"$wpn_m_submachinegun_laser_fauto_name;", sz::kLensScopeYes, 50.0, true},
        };
        bool all = true;
        for (const Want& w : want) {
            double deg = 0.0;
            bool fromBare = !w.fromBare;
            all = all && sz::lensAdsDeg(w.item, w.scope, &deg, &fromBare) && near1(deg, w.deg, 1e-12) && fromBare == w.fromBare;
        }
        check(all, "S1 table: the optical sights give their ADS FOV (15 and 35, 48, 50), and an unmeasured scoped copy takes its bare copy's FOV and says so");
        double deg = 0.0;
        check(sz::lensAdsDeg("wpn_m_sniper_plasma_charged", sz::kLensScopeYes, &deg) && near1(deg, 15.0, 1e-12), "S1 table: the journal's name finds the entry as Status.json's does");
        check(!sz::lensAdsDeg("$wpn_m_submachinegun_laser_fauto_name;", sz::kLensScopeNo, &deg) && std::isnan(deg), "S1 table: the unscoped Eclipse is not an optical sight: no entry, and the FOV reads NaN");
        {
            static const char* const kOpen[] = {"$wpn_m_assaultrifle_kinetic_fauto_name;", "$wpn_m_submachinegun_kinetic_fauto_name;", "$wpn_m_shotgun_plasma_doublebarrel_name;",
                                                "$wpn_s_pistol_plasma_charged_name;", "$wpn_s_pistol_kinetic_sauto_name;", "$wpn_m_launcher_rocket_sauto_name;", "$wpn_s_pistol_laser_sauto_name;"};
            bool none = true;
            for (const char* item : kOpen) none = none && !sz::lensAdsDeg(item, sz::kLensScopeNo, &deg) && !sz::lensAdsDeg(item, sz::kLensScopeYes, &deg);
            check(none, "S1 table: the weapons with an iron, reflex or holo sight (the Karma rifle and SMG, the Zenith, the pistols and the launcher) have no entry, scoped or not");
        }
        check(!sz::lensAdsDeg("$wpn_m_sniper_plasma_charged_name;", sz::kLensScopeUnknown, &deg) && !sz::lensAdsDeg("$wpn_m_assaultrifle_plasma_fauto_name;", sz::kLensScopeUnknown, &deg),
              "S1 table: an unknown scope state is not an optical sight, whatever the item");
        check(!sz::lensAdsDeg("$wpn_m_mining_laser_name;", sz::kLensScopeNo, &deg) && !sz::lensAdsDeg("none", sz::kLensScopeNo, &deg) && !sz::lensAdsDeg(nullptr, sz::kLensScopeNo, nullptr),
              "S1 table: an unmeasured item and 'none' have no lens");
        // M against this rig's two on-foot FOV options (section 7): 56.327 and 64.752 deg
        check(near1(sz::lensZoom(56.327, 15.0), 4.07, 0.01) && near1(sz::lensZoom(56.327, 35.0), 1.70, 0.01) && near1(sz::lensZoom(56.327, 52.0), 1.10, 0.01) &&
                  near1(sz::lensZoom(56.327, 48.0), 1.20, 0.01) && near1(sz::lensZoom(56.327, 50.0), 1.15, 0.01),
              "S1 M: from 56.327 deg the scoped Executioner is 4.07, the bare one 1.70, the unscoped sights 1.10 to 1.20 (section 7's table)");
        check(near1(sz::lensZoom(64.752, 15.0), 4.82, 0.01) && near1(sz::lensZoom(64.752, 35.0), 2.01, 0.01) && near1(sz::lensZoom(64.752, 52.0), 1.30, 0.01) &&
                  near1(sz::lensZoom(64.752, 48.0), 1.42, 0.01),
              "S1 M: from 64.752 deg the same weapons are 4.82, 2.01, 1.30 and 1.42");
        check(near1(sz::lensZoom(56.3, 56.3), 1.0, 1e-12) && sz::lensZoom(56.3, 90.0) < 1.0, "S1 M: an ADS FOV equal to the reference is 1 and a wider one is under 1");
        check(std::isnan(sz::lensZoom(kNaNd(), 15.0)) && std::isnan(sz::lensZoom(56.3, 0.0)) && std::isnan(sz::lensZoom(0.0, 15.0)) && std::isnan(sz::lensZoom(56.3, kNaNd())) &&
                  std::isnan(sz::lensZoom(180.0, 15.0)),
              "S1 M: an input that is not an angle (NaN, zero, a straight angle) is NaN");
        check(near1(sz::lensClampRefDeg(56.3), 56.3, 1e-12) && near1(sz::lensClampRefDeg(10.0), sz::kLensRefMinDeg, 1e-12) && near1(sz::lensClampRefDeg(200.0), sz::kLensRefMaxDeg, 1e-12) &&
                  near1(sz::lensClampRefDeg(kNaNd()), sz::kLensRefDefaultDeg, 1e-12),
              "S1 M: the reference FOV is held to 30..90 and a value that is no number reads as the default 56.3");
    }

    // ---- the gate and the ease --------------------------------------------------------------------------------------------------------------------------------------------------
    {
        sz::LensGateIn g;
        g.keyOn = g.onFoot = g.adsActive = g.wFull = g.optical = g.sFresh = g.cUsable = true;
        g.m = 4.07;
        check(sz::lensGate(g) == sz::LensVerdict::kQualifies, "S1 gate: the key on, on foot, the sights up at w 1.0, an optical sight with M 4.07, S fresh and c usable: the lens runs");
        auto without = [&](auto edit) {
            sz::LensGateIn x = g;
            edit(x);
            return sz::lensGate(x);
        };
        check(without([](sz::LensGateIn& x) { x.keyOn = false; }) == sz::LensVerdict::kIdle, "S1 gate: the key off asks nothing of the lens (idle, nothing counted)");
        check(without([](sz::LensGateIn& x) { x.onFoot = false; }) == sz::LensVerdict::kIdle && without([](sz::LensGateIn& x) { x.adsActive = false; }) == sz::LensVerdict::kIdle &&
                  without([](sz::LensGateIn& x) { x.wFull = false; }) == sz::LensVerdict::kIdle,
              "S1 gate: not on foot, the sights not up and w under 1.0 are idle");
        check(without([](sz::LensGateIn& x) { x.optical = false; }) == sz::LensVerdict::kNotOptical, "S1 gate: an item or scope that is not an optical sight is counted as not an optical sight");
        check(without([](sz::LensGateIn& x) { x.m = 1.04; }) == sz::LensVerdict::kNotOptical && without([](sz::LensGateIn& x) { x.m = 1.05; }) == sz::LensVerdict::kQualifies &&
                  without([](sz::LensGateIn& x) { x.m = kNaNd(); }) == sz::LensVerdict::kNotOptical,
              "S1 gate: an M under 1.05 (or no number) counts as not an optical sight; 1.05 is the line");
        check(without([](sz::LensGateIn& x) { x.sFresh = false; }) == sz::LensVerdict::kNoS && without([](sz::LensGateIn& x) { x.cUsable = false; }) == sz::LensVerdict::kNoS,
              "S1 gate: S stale or c not usable is counted as no S");
        check(without([](sz::LensGateIn& x) { x.optical = false; x.sFresh = false; }) == sz::LensVerdict::kNotOptical,
              "S1 gate: a weapon that is not an optical sight is not counted as a missing S (the table is asked first)");
        check(!sz::lensFresh(50001) && sz::lensFresh(50000) && sz::lensFresh(0), "S1 gate: the door takes a c no older than 50 ms");
        // F162: the lens is the right eye's, so the sights up with the left eye aiming (fix.native_on_foot_ads_sight_eye = left) is its own decline, whatever else holds
        check(without([](sz::LensGateIn& x) { x.leftAim = true; }) == sz::LensVerdict::kLeftAim && without([](sz::LensGateIn& x) { x.leftAim = true; x.optical = false; }) == sz::LensVerdict::kLeftAim &&
                  without([](sz::LensGateIn& x) { x.leftAim = true; x.sFresh = false; }) == sz::LensVerdict::kLeftAim,
              "S1 gate: the left eye aiming is counted as the left aiming eye, ahead of the optical sight and S tests (the lens is the right eye's)");
        check(without([](sz::LensGateIn& x) { x.leftAim = true; x.keyOn = false; }) == sz::LensVerdict::kIdle && without([](sz::LensGateIn& x) { x.leftAim = true; x.adsActive = false; }) == sz::LensVerdict::kIdle &&
                  without([](sz::LensGateIn& x) { x.leftAim = true; x.onFoot = false; }) == sz::LensVerdict::kIdle && without([](sz::LensGateIn& x) { x.leftAim = true; x.wFull = false; }) == sz::LensVerdict::kIdle,
              "S1 gate: the left eye aiming asks nothing of the lens while the key is off or the sights are not up (idle, nothing counted)");
        check(sz::LensVerdict::kLeftAim != sz::LensVerdict::kQualifies && static_cast<uint32_t>(sz::LensVerdict::kLeftAim) == 4, "S1 gate: the left aiming verdict is not a qualifying one (the gunsight mark's family is armed on kQualifies alone)");
        // A3: the S0 census probes a press of an optical sight and no other. lensOpticalSight is the table's verdict the census asks, and it is the gate's kNotOptical read the other way: for
        // every (item, scope) of the table and a sample of the weapons that are not in it, at two reference FOVs, the gate over the table's own numbers says kNotOptical exactly when the
        // census's question says no.
        static const char* const kItems[] = {"$wpn_m_sniper_plasma_charged_name;", "$wpn_m_assaultrifle_plasma_fauto_name;", "$wpn_m_assaultrifle_laser_fauto_name;", "$wpn_m_submachinegun_laser_fauto_name;",
                                             "$wpn_m_assaultrifle_kinetic_fauto_name;", "$wpn_m_shotgun_plasma_doublebarrel_name;", "$wpn_s_pistol_plasma_charged_name;", "$wpn_m_mining_laser_name;", "none", ""};
        static const int kScopes[] = {sz::kLensScopeUnknown, sz::kLensScopeNo, sz::kLensScopeYes};
        bool agrees = true;
        uint32_t optical = 0;
        for (const double ref : {56.3, 64.752}) {
            for (const char* item : kItems) {
                for (const int scope : kScopes) {
                    sz::LensGateIn x = g;
                    double ads = sz::kNaN;
                    x.optical = sz::lensAdsDeg(item, scope, &ads);
                    x.m = x.optical ? sz::lensZoom(ref, ads) : sz::kNaN;
                    const bool gateSaysOptical = sz::lensGate(x) != sz::LensVerdict::kNotOptical;
                    const bool census = sz::lensOpticalSight(item, scope, ref);
                    agrees = agrees && gateSaysOptical == census;
                    optical += census ? 1u : 0u;
                }
            }
        }
        check(agrees && optical == 14, "S1 gate: the census's question (lensOpticalSight) is the gate's optical-sight verdict, for the table's seven entries at two reference FOVs and for the weapons outside it");
        check(sz::lensOpticalSight("$wpn_m_sniper_plasma_charged_name;", sz::kLensScopeYes, 56.3) && sz::lensOpticalSight("$wpn_m_sniper_plasma_charged_name;", sz::kLensScopeNo, 56.3) &&
                  !sz::lensOpticalSight("$wpn_m_submachinegun_laser_fauto_name;", sz::kLensScopeNo, 56.3) && !sz::lensOpticalSight("$wpn_m_assaultrifle_kinetic_fauto_name;", sz::kLensScopeYes, 56.3) &&
                  !sz::lensOpticalSight("$wpn_m_sniper_plasma_charged_name;", sz::kLensScopeUnknown, 56.3) && !sz::lensOpticalSight(nullptr, sz::kLensScopeNo, 56.3),
              "S1 census: the Executioner is an optical sight both ways, the unscoped Eclipse, a Karma weapon, an unknown scope state and no item are not");
        check(!sz::lensOpticalSight("$wpn_m_assaultrifle_plasma_fauto_name;", sz::kLensScopeNo, 46.0) && sz::lensOpticalSight("$wpn_m_assaultrifle_plasma_fauto_name;", sz::kLensScopeNo, 56.3) &&
                  !sz::lensOpticalSight("$wpn_m_sniper_plasma_charged_name;", sz::kLensScopeYes, sz::kNaN),
              "S1 census: an entry whose M falls under 1.05 at the reference FOV, or at no reference, is not an optical sight, as the gate says");
    }
    {
        check(near1(sz::lensEase(4.07, 0.0), 1.0, 1e-12) && near1(sz::lensEase(4.07, -1.0), 1.0, 1e-12), "S1 ease: M is 1 at the first qualifying frame (and for a time before it)");
        check(near1(sz::lensEase(4.07, 0.25), 4.07, 1e-12) && near1(sz::lensEase(4.07, 3.0), 4.07, 1e-12), "S1 ease: M is its value at 0.25 s and after");
        double prev = 1.0;
        bool mono = true;
        for (int i = 1; i <= 25; ++i) {
            const double m = sz::lensEase(4.07, i * 0.01);
            mono = mono && m >= prev - 1e-12 && m <= 4.07 + 1e-12;
            prev = m;
        }
        check(mono, "S1 ease: M only grows from 1 to its value");
        const double half = sz::lensEase(4.0, 0.125);
        check(near1(half, 2.0, 1e-9), "S1 ease: halfway through it is the geometric middle (the zoom grows evenly in size, not in factor)");
        check(near1(sz::lensEase(1.0, 0.1), 1.0, 1e-12) && near1(sz::lensEase(sz::kNaN, 0.1), 1.0, 1e-12) && near1(sz::lensEase(0.5, 0.3), 0.5, 1e-12),
              "S1 ease: a target of 1, no number or under 1 has nothing to ease");
    }

    // ---- the rays: the reference march over a hexagonal frame ----------------------------------------------------------------------------------------------------------------
    {
        const int W = 2016, H = 2017;
        const float tan4[4] = {-1.17f, 0.95f, -1.01f, 1.07f};
        const double nearDepth = 0.0417;                              // the reverse-Z depth of 0.6 m (0.025 / 0.6)
        const Hex hex{1000.3, 1010.7, 100.0, 12.0, 0.2};         // the frame: a band 12 px wide, apothem 100 px
        const auto depth = [&](int x, int y) { return hex.frame(x + 0.5, y + 0.5) ? 0.14 : 0.00122; };
        const double cx = 1000.3 + 17.0, cy = 1010.7 - 9.0;     // c a little off the frame's centre
        sz::LensRay rays[sz::kLensRays];
        double worst = 0.0;
        uint32_t escapedN = 0;
        for (uint32_t k = 0; k < sz::kLensRays; ++k) {
            rays[k] = sz::lensMarchRef(depth, W, H, cx, cy, k, tan4, nearDepth);
            double dx, dy;
            sz::lensRayDir(k, &dx, &dy);
            worst = std::fmax(worst, std::fabs(rays[k].rPx - hex.cast(cx, cy, dx, dy)));
            escapedN += rays[k].escaped ? 1 : 0;
        }
        check(escapedN == 0 && worst <= 2.6, "S1 rays: all 128 rays stop at the frame's inner edge, within 2.6 render px of the exact hexagon (the run of 3 px reports the first)");
        float rpx[sz::kLensRays];
        sz::lensClampEscaped(rays, rpx);
        bool same = true;
        for (uint32_t k = 0; k < sz::kLensRays; ++k) same = same && rpx[k] == static_cast<float>(rays[k].rPx);
        check(same, "S1 rays: with no ray escaped the radii the mask uses are the rays' own");
        // the hole in degrees: the frame's apothem is about 100 px of 951 px a unit of tangent, a little under 6 degrees at its flats
        double rmin, rmed, rmax;
        sz::LensReadback rb;
        for (uint32_t k = 0; k < sz::kLensRays; ++k) { rb.rPx[k] = static_cast<float>(rays[k].rPx); rb.rDeg[k] = static_cast<float>(rays[k].rDeg); }
        rb.escaped = 0;
        check(sz::lensHoleStats(rb, &rmin, &rmed, &rmax) && rmin > 3.5 && rmax < 9.0 && rmin <= rmed && rmed <= rmax, "S1 rays: the hole is some 4 to 8 degrees across at the eye, min <= median <= max");
        // a ray whose first 2 px of frame are a speck (under the run of 3) goes through it
        const auto speck = [&](int x, int y) { return hex.frame(x + 0.5, y + 0.5) || (x >= 1060 && x < 1062 && y >= 1000 && y < 1020) ? 0.14 : 0.00122; };
        const sz::LensRay thru = sz::lensMarchRef(speck, W, H, 1000.3, 1010.7, 0, tan4, nearDepth);
        const sz::LensRay plain = sz::lensMarchRef(depth, W, H, 1000.3, 1010.7, 0, tan4, nearDepth);
        check(!thru.escaped && near1(thru.rPx, plain.rPx, 0.5), "S1 rays: a speck of frame 2 px wide is not the frame (the run is 3 px)");
        // no frame at all: every ray escapes at the angle (12 deg) and the sight is open
        const auto empty = [](int, int) { return 0.00122; };
        uint32_t esc = 0;
        double minDeg = 100.0;
        for (uint32_t k = 0; k < sz::kLensRays; ++k) {
            const sz::LensRay r = sz::lensMarchRef(empty, W, H, 1000.0, 1000.0, k, tan4, nearDepth);
            esc += r.escaped ? 1 : 0;
            minDeg = std::fmin(minDeg, r.rDeg);
        }
        check(esc == sz::kLensRays && near1(minDeg, 12.0, 1e-9) && sz::lensOpenSight(esc), "S1 rays: with no frame every ray escapes at 12 degrees and the sight is open");
        // c near the picture's edge: the rays that leave the image escape there
        const sz::LensRay edgeRay = sz::lensMarchRef(empty, W, H, 30.0, 1000.0, 64, tan4, nearDepth);   // ray 64 points to -x
        check(edgeRay.escaped && near1(edgeRay.rPx, 31.0, 1.001) && edgeRay.rDeg == sz::kLensMaxDeg, "S1 rays: a ray that leaves the image escapes at its edge and reads as the 12 degree sentinel, not the angle it had reached");
        {   // a hole with one ray off the image and one that took every step: neither is a hit in the statistics
            sz::LensReadback hr;
            for (uint32_t k = 0; k < sz::kLensRays; ++k) hr.rDeg[k] = 5.0f;
            hr.rDeg[7] = static_cast<float>(edgeRay.rDeg);
            const sz::LensRay capped = sz::lensMarchRef([](int, int) { return 0.00122; }, 100000, 100000, 50000.0, 50000.0, 0, tan4, nearDepth);   // 768 steps and the angle never reaches 12 degrees
            hr.rDeg[8] = static_cast<float>(capped.rDeg);
            double lo, mid, hi;
            check(capped.escaped && capped.rPx == sz::kLensStepCap && capped.rDeg == sz::kLensMaxDeg, "S1 rays: a ray that takes every step escapes at the step cap and reads as the sentinel");
            check(sz::lensHoleStats(hr, &lo, &mid, &hi) && near1(lo, 5.0, 1e-6) && near1(hi, 5.0, 1e-6), "S1 hole: rays that left the image or took every step are kept out of r min, median and max");
        }
        // the 12 degree stop is the angle at the eye in the tangent plane, not a pixel count
        const sz::LensRay right = sz::lensMarchRef(empty, W, H, 1000.0, 1000.0, 0, tan4, nearDepth), left = sz::lensMarchRef(empty, W, H, 1000.0, 1000.0, 64, tan4, nearDepth);
        check(right.escaped && left.escaped && right.rPx > 150.0 && left.rPx > 150.0 && std::fabs(right.rPx - left.rPx) > 3.0, "S1 rays: the 12 degree stop is the angle at the eye (an off-axis c reaches it at different px each way)");
        // the open rule and the clamp of an escaped ray
        check(!sz::lensOpenSight(0) && !sz::lensOpenSight(2) && sz::lensOpenSight(3), "S1 rays: more than 2 escaped rays is an open sight; 2 is a gap in the frame");
        sz::LensRay gap[sz::kLensRays];
        for (uint32_t k = 0; k < sz::kLensRays; ++k) { gap[k].rPx = 100.0 + k % 3; gap[k].escaped = false; }
        gap[10].escaped = true; gap[10].rPx = 280.0;
        gap[11].escaped = true; gap[11].rPx = 285.0;
        gap[40].escaped = true; gap[40].rPx = 290.0;
        float clamped[sz::kLensRays];
        sz::lensClampEscaped(gap, clamped);
        check(clamped[10] == static_cast<float>(gap[9].rPx) && clamped[11] == static_cast<float>(gap[12].rPx) && clamped[10] < 150.0f && clamped[40] == std::fmin(static_cast<float>(gap[39].rPx), static_cast<float>(gap[41].rPx)) &&
                  clamped[0] == static_cast<float>(gap[0].rPx),
              "S1 rays: an escaped ray takes the smaller radius of its nearest rays that did not escape (a run of two takes its two ends'), the others are as they are");
        sz::LensRay all[sz::kLensRays];
        for (uint32_t k = 0; k < sz::kLensRays; ++k) { all[k].rPx = 200.0 + k; all[k].escaped = true; }
        sz::lensClampEscaped(all, clamped);
        check(clamped[5] == 205.0f && clamped[127] == 327.0f, "S1 rays: with every ray escaped there is nothing to clamp to, and the radii stay as the rays ended");
    }

    // ---- r(theta), the mask, the source, Catmull-Rom ----------------------------------------------------------------------------------------------------------------------------
    {
        float r[sz::kLensRays];
        for (uint32_t k = 0; k < sz::kLensRays; ++k) r[k] = 100.0f + static_cast<float>(k);
        const double step = 2.0 * sz::kPi / sz::kLensRays;
        check(near1(sz::lensRadiusAt(r, 0.0), 100.0, 1e-9) && near1(sz::lensRadiusAt(r, 3 * step), 103.0, 1e-9), "S1 r(theta): at a ray's angle it is that ray's radius");
        check(near1(sz::lensRadiusAt(r, 3.5 * step), 103.5, 1e-9) && near1(sz::lensRadiusAt(r, 3.25 * step), 103.25, 1e-9), "S1 r(theta): between two rays it is interpolated");
        check(near1(sz::lensRadiusAt(r, 127.5 * step), (227.0 + 100.0) / 2.0, 1e-6), "S1 r(theta): past the last ray it wraps to the first");
        check(near1(sz::lensRadiusAt(r, -0.5 * step), (100.0 + 227.0) / 2.0, 1e-6) && near1(sz::lensRadiusAt(r, 2.0 * sz::kPi + 3 * step), 103.0, 1e-6),
              "S1 r(theta): an angle below zero or past a turn is the same angle (atan2 returns -pi..pi)");
        check(near1(sz::lensWeight(2.0), 1.0, 1e-12) && near1(sz::lensWeight(5.0), 1.0, 1e-12) && near1(sz::lensWeight(1.0), 0.5, 1e-12) && sz::lensWeight(0.0) == 0.0 && sz::lensWeight(-3.0) == 0.0,
              "S1 mask: the lens is whole 2 output px inside the hole's edge, half at 1 px, and gone at the edge and outside it");
        const double c[2] = {1000.0, 800.0}, p[2] = {1100.0, 760.0};
        double s[2];
        sz::lensSourceOf(c, 4.0, p, s);
        check(near1(s[0], 1025.0, 1e-12) && near1(s[1], 790.0, 1e-12), "S1 source: a pixel 100 px right and 40 px up of c is read from 25 px right and 10 px up of it at M 4");
        sz::lensSourceOf(c, 4.0, c, s);
        check(s[0] == c[0] && s[1] == c[1], "S1 source: c reads c");
        sz::lensSourceOf(c, 1.0, p, s);
        check(s[0] == p[0] && s[1] == p[1], "S1 source: M 1 reads the pixel itself");
        double w[4];
        sz::lensCatmullRom(0.0, w);
        check(near1(w[0], 0.0, 1e-12) && near1(w[1], 1.0, 1e-12) && near1(w[2], 0.0, 1e-12) && near1(w[3], 0.0, 1e-12), "S1 Catmull-Rom: at a texel centre it is that texel");
        sz::lensCatmullRom(1.0, w);
        check(near1(w[1], 0.0, 1e-12) && near1(w[2], 1.0, 1e-12), "S1 Catmull-Rom: one texel on it is the next texel");
        bool sums = true, linear = true;
        for (int i = 0; i <= 20; ++i) {
            const double f = i / 20.0;
            sz::lensCatmullRom(f, w);
            sums = sums && near1(w[0] + w[1] + w[2] + w[3], 1.0, 1e-12);
            linear = linear && near1(-1.0 * w[0] + 0.0 * w[1] + 1.0 * w[2] + 2.0 * w[3], f, 1e-12);   // it reproduces a straight line
        }
        check(sums && linear, "S1 Catmull-Rom: the four weights sum to 1 and reproduce a straight ramp");
    }

    // ---- the window, the box, the buffer ------------------------------------------------------------------------------------------------------------------------------------
    {
        const uint32_t region[4] = {0, 0, 4032, 4034};
        const sz::LensWindow w = sz::lensWindow(1642.0, 1990.0, -0.10, -0.01, 1894.0, 1894.0, region);
        check(!w.empty() && w.x0 >= 0 && w.y0 >= 0 && w.x1 <= 4032 && w.y1 <= 4034 && w.x0 < 1642 && w.x1 > 1642 && w.y0 < 1990 && w.y1 > 1990,
              "S1 window: the box about c inside the picture");
        check((w.x1 - w.x0) > 800 && (w.x1 - w.x0) < 1100 && (w.y1 - w.y0) > 800 && (w.y1 - w.y0) < 1100,
              "S1 window: 12 degrees at 1894 px a unit of tangent is a window some 900 px across, not the picture's 4032");
        const sz::LensWindow edge = sz::lensWindow(40.0, 30.0, -1.0, 0.9, 1894.0, 1894.0, region);
        check(!edge.empty() && edge.x0 == 0 && edge.y0 == 0 && edge.x1 > 40 && edge.y1 > 30, "S1 window: c near the picture's corner has the window cut to the picture");
        const uint32_t cut[4] = {100, 200, 900, 800};
        const sz::LensWindow inCut = sz::lensWindow(500.0, 500.0, 0.0, 0.0, 1894.0, 1894.0, cut);
        check(inCut.x0 == 100 && inCut.y0 == 200 && inCut.x1 == 900 && inCut.y1 == 800, "S1 window: a region that is not the whole texture cuts the window to itself");
        check(sz::lensWindow(-5000.0, 100.0, 0.0, 0.0, 1894.0, 1894.0, region).empty() && sz::lensWindow(sz::kNaN, 100.0, 0.0, 0.0, 1894.0, 1894.0, region).empty(),
              "S1 window: a c far outside the picture, or no number, is an empty window");
        const sz::LensWindow box = sz::lensBox(1642.0, 1990.0, 105.0, 2.0, 2.0, w);
        check(box.x0 >= w.x0 && box.y0 >= w.y0 && box.x1 <= w.x1 && box.y1 <= w.y1 && box.x1 - box.x0 >= 2 * 210 && box.x1 - box.x0 <= 2 * 215 + 4, "S1 box: the farthest ray (105 render px, 210 output px) plus 4 px each way, inside the window");
        const sz::LensWindow narrow = sz::lensBox(1642.0, 1990.0, 5000.0, 2.0, 2.0, w);
        check(narrow.x0 == w.x0 && narrow.x1 == w.x1 && narrow.y0 == w.y0 && narrow.y1 == w.y1, "S1 box: a ray longer than the window is cut to the window");
        // the buffer's layout: the shader and the readback must agree
        check(sz::kLensOffRadiusPx == 64 && sz::kLensOffRadiusDeg == 576 && sz::kLensBufferBytes == 1088 && sz::kLensOffBox == 12 && sz::kLensOffEscaped == 28 && sz::kLensOffDepthAtC == 32,
              "S1 buffer: the offsets the shaders store and load at (arguments 0, box 12, escaped 28, depth 32, radii 64, degrees 576, 1088 bytes)");
        std::vector<unsigned char> bytes(sz::kLensBufferBytes, 0);
        const uint32_t args[3] = {7, 9, 1}, boxv[4] = {10, 20, 224, 70};
        const uint32_t esc = 3;
        const float depthC = 0.00123f;
        std::memcpy(&bytes[sz::kLensOffArgs], args, sizeof(args));
        std::memcpy(&bytes[sz::kLensOffBox], boxv, sizeof(boxv));
        std::memcpy(&bytes[sz::kLensOffEscaped], &esc, sizeof(esc));
        std::memcpy(&bytes[sz::kLensOffDepthAtC], &depthC, sizeof(depthC));
        for (uint32_t k = 0; k < sz::kLensRays; ++k) {
            const float a = 100.0f + k, b = 5.0f + 0.01f * k;
            std::memcpy(&bytes[sz::kLensOffRadiusPx + 4 * k], &a, 4);
            std::memcpy(&bytes[sz::kLensOffRadiusDeg + 4 * k], &b, 4);
        }
        sz::LensReadback rb;
        check(sz::lensDecode(bytes.data(), bytes.size(), &rb) && rb.args[0] == 7 && rb.args[1] == 9 && rb.box[2] == 224 && rb.escaped == 3 && rb.depthAtC == depthC && rb.rPx[100] == 200.0f &&
                  near1(rb.rDeg[50], 5.5, 1e-6) && !sz::lensDecode(bytes.data(), bytes.size() - 4, &rb) && !sz::lensDecode(nullptr, 0, &rb),
              "S1 buffer: the readback decodes the arguments, the box, the escaped count, the depth at c and both radius arrays, and refuses a short buffer");
        // the hole's statistics skip the rays that escaped (12 degrees)
        sz::LensReadback h;
        for (uint32_t k = 0; k < sz::kLensRays; ++k) h.rDeg[k] = 5.0f;
        h.rDeg[0] = 12.0f;
        h.rDeg[1] = 12.0f;
        h.rDeg[2] = 3.0f;
        h.rDeg[3] = 8.0f;
        double lo, mid, hi;
        check(sz::lensHoleStats(h, &lo, &mid, &hi) && near1(lo, 3.0, 1e-6) && near1(mid, 5.0, 1e-6) && near1(hi, 8.0, 1e-6), "S1 hole: min, median and max over the rays that did not escape");
        for (uint32_t k = 0; k < sz::kLensRays; ++k) h.rDeg[k] = 12.0f;
        check(!sz::lensHoleStats(h, &lo, &mid, &hi), "S1 hole: every ray escaped has no hole to report");
    }

    // ---- the instrument: the accumulators and the lines -----------------------------------------------------------------------------------------------------------------------
    {
        sz::LensDoorStats s;
        sz::lensNoteC(&s, 100.0, 200.0);
        sz::lensNoteC(&s, 102.0, 200.0);
        sz::lensNoteC(&s, 100.0, 204.0);
        sz::lensNoteC(&s, sz::kNaN, 1.0);
        double mx, my, sp;
        sz::lensCStats(s, &mx, &my, &sp);
        check(s.cN == 3 && near1(mx, 100.6666667, 1e-6) && near1(my, 201.3333333, 1e-6) && near1(sp, std::sqrt(0.8888889 + 3.5555556), 1e-6), "S1 stats: c's mean and spread over the frames (a NaN is not counted)");
        sz::LensDoorStats none;
        sz::lensCStats(none, &mx, &my, &sp);
        check(std::isnan(mx) && std::isnan(my) && std::isnan(sp), "S1 stats: no frame has no mean and no spread");
        sz::lensNoteGpu(&s, 0.020, 0.040);
        sz::lensNoteGpu(&s, 0.030, 0.050);
        sz::lensNoteGpu(&s, sz::kNaN, 0.050);
        check(s.gpuN == 2 && near1(s.raysMsSum, 0.05, 1e-12) && near1(s.magMsMax, 0.05, 1e-12) && near1(s.raysMsMax, 0.03, 1e-12), "S1 stats: the GPU time of the two passes, the mean's sum and the maxima");
        // the press: 100 qualifying frames, 4 declined by the door, 3 of the rest found open, the first 2 at M 1
        sz::LensDoorStats p;
        p.noop = 2;
        p.dispatched = 94;
        p.staleC = 1;
        p.depthUnavailable = 2;
        p.formatRefused = 1;
        sz::LensReadback ok;
        for (uint32_t k = 0; k < sz::kLensRays; ++k) ok.rDeg[k] = 5.0f + 0.01f * k;
        ok.escaped = 0;
        ok.depthAtC = 0.00122f;
        sz::LensReadback open = ok;
        open.escaped = 128;
        for (uint32_t k = 0; k < sz::kLensRays; ++k) open.rDeg[k] = 12.0f;
        sz::lensNoteRays(&p, open, false);
        sz::lensNoteRays(&p, open, true);
        sz::lensNoteRays(&p, ok, false);
        check(p.openSight == 2 && p.raysRead == 3 && !p.holeKnown, "S1 stats: rays read back count open sights, and an open sight (every ray at 12 degrees) has no hole to take");
        sz::LensDoorStats q;
        sz::lensNoteRays(&q, ok, false);
        check(!q.holeKnown, "S1 stats: the hole is taken from a settled frame, not the ease's");
        sz::lensNoteRays(&q, ok, true);
        check(q.holeKnown && near1(q.rMinDeg, 5.0, 1e-6) && q.holeEscaped == 0 && near1(q.depthAtC, 0.00122, 1e-9) && q.raysRead == 2, "S1 stats: the first settled frame gives the hole and the depth at c");
        sz::LensReadback later = ok;
        for (uint32_t k = 0; k < sz::kLensRays; ++k) later.rDeg[k] = 9.0f;
        sz::lensNoteRays(&q, later, true);
        check(near1(q.rMinDeg, 5.0, 1e-6), "S1 stats: only one frame's hole a press");
        sz::lensFinish(&p);
        check(p.applied == 94 - 2 && p.qualifying == 94 + 2 + 1 + 2 + 1, "S1 stats: applied is the dispatched less the open; the M = 1 frames are qualifying and not applied, and the door's declines add to the qualifying");
        {   // a press the door declined on every frame but the first (depth null): the first frame is at M 1. It is not applied, so the press cannot read as one that applied.
            sz::LensDoorStats d;
            d.noop = 1;
            d.depthUnavailable = 200;
            sz::lensFinish(&d);
            check(d.applied == 0 && d.qualifying == 201, "S1 stats: a press with its depth missing on 200 frames and the first at M 1 is applied 0 of 201");
            sz::LensDoorStats d2;
            d2.noop = 1;
            sz::lensFinish(&d2);
            check(d2.applied == 0 && d2.qualifying == 1, "S1 stats: a press that is one frame at M 1 is applied 0 of 1");
        }
        {   // the frame thread's counts: c through S against c at the far point
            sz::LensTickStats t;
            sz::lensNoteFar(&t, 0.30);
            sz::lensNoteFar(&t, 0.50);
            sz::lensNoteFar(&t, sz::kNaN);
            check(t.farN == 2 && near1(t.farSumDeg / t.farN, 0.40, 1e-12) && near1(t.farMaxDeg, 0.50, 1e-12), "S1 tick stats: the angle between c through S and the far c, its mean and its max (a NaN is not counted)");
            const float tan4[4] = {-0.5f, 0.5f, -0.5f, 0.5f};
            const double a[2] = {0.0, 0.0}, b[2] = {0.0, 0.0}, c[2] = {0.2, 0.0};
            const double tc = 0.2 * 0.5;   // ndc 0.2 across a half-width of 0.5 is tangent 0.1
            check(near1(sz::lensNdcAngleDeg(a, b, tan4), 0.0, 1e-9) && near1(sz::lensNdcAngleDeg(a, c, tan4), std::atan(tc) * sz::kDeg, 1e-9) &&
                      std::isnan(sz::lensNdcAngleDeg(a, nullptr, tan4)) && std::isnan(sz::lensNdcAngleDeg(a, c, nullptr)),
                  "S1 tick stats: the angle at the eye between two NDC points is the angle of their rays (0.1 of tangent from the axis is 5.71 degrees), NaN with a point or the tangents missing");
            const double one[2] = {1.0, 1.0};
            check(sz::lensNdcAngleDeg(a, one, tan4) > sz::lensNdcAngleDeg(a, c, tan4), "S1 tick stats: the angle grows with the distance between the points");
        }
    }
    {
        char line[sz::kLensPressLineBytes];
        sz::formatLensAnnounce(line, sizeof(line), true, 56.3, sz::lensTableEntries());
        check(std::string(line).rfind("scope zoom (S1): on, reference FOV 56.3 deg, 7 table entries;", 0) == 0, "S1 lines: the announce says the key, the reference FOV and the table's size");
        sz::formatLensAnnounce(line, sizeof(line), false, 64.8, 7);
        check(std::string(line).rfind("scope zoom (S1): off, reference FOV 64.8 deg, 7 table entries;", 0) == 0, "S1 lines: the announce after the key went off");
        sz::LensPressLine p;
        p.press = 3;
        p.item = "$wpn_m_sniper_plasma_charged_name;";
        p.scope = "yes";
        p.keyOn = true;
        p.adsDeg = 15.0;
        p.m = 4.0670;
        p.refDeg = 56.3;
        p.t.qualified = 312;
        p.t.noS = 9;
        p.t.notOptical = 0;
        p.t.fovFromBare = 0;
        p.t.idle = 40;
        p.t.jitterNone = 2;
        p.t.jitterOther = 5;
        sz::lensNoteFar(&p.t, 0.25);
        sz::lensNoteFar(&p.t, 0.75);
        p.d.applied = 300;
        p.d.qualifying = 310;
        p.d.dispatched = 303;
        p.d.noop = 1;
        p.d.seqGap[0] = 1;
        p.d.seqGap[1] = 297;
        p.d.seqGap[2] = 4;
        p.d.seqGap[3] = 1;
        p.d.seqGap[4] = 0;
        p.d.markRedirected[0] = 296;
        p.d.markRedirected[1] = 300;
        p.d.markArmedFrames = 310;
        p.d.markNoDraw = 2;
        p.d.markPath[sz::kMarkPathSeeded] = 500;
        p.d.markPath[sz::kMarkPathSeededReadOnly] = 80;
        p.d.markPath[sz::kMarkPathTestsOff] = 16;
        p.d.markComposite[sz::kMarkCompositeMatched] = 590;
        p.d.markComposite[sz::kMarkCompositePlain] = 4;
        p.d.markComposite[sz::kMarkCompositeNone] = 2;
        p.d.staleC = 4;
        p.d.depthUnavailable = 3;
        p.d.formatRefused = 3;
        p.d.noS = 7;
        p.d.notOptical = 0;
        p.d.openSight = 0;
        p.d.raysRead = 296;
        p.d.holeKnown = true;
        p.d.rMinDeg = 4.01;
        p.d.rMedianDeg = 5.2;
        p.d.rMaxDeg = 6.42;
        p.d.holeEscaped = 1;
        p.d.depthAtC = 0.00122;
        p.d.depthNear = 0.0417;
        sz::lensNoteC(&p.d, 1642.0, 1990.0);
        sz::lensNoteC(&p.d, 1643.0, 1990.0);
        sz::lensNoteGpu(&p.d, 0.0123, 0.0456);
        const int n = sz::formatLensPress(line, sizeof(line), p);
        const std::string text(line);
        check(n == static_cast<int>(text.size()) && n < static_cast<int>(sz::kLensPressLineBytes) &&
                  text.rfind("scope zoom (S1): press 3 item $wpn_m_sniper_plasma_charged_name; scope yes key on ads 15.000 deg M 4.067 h 56.3 deg; frame thread: qualified 312, no S 9, "
                             "not an optical sight 0, left aiming eye 0, idle 40; FOV from the bare copy 0; jitter reading none 2, from another frame 5; c to the far point mean 0.500 max 0.750 deg; applied 300 of 310 qualifying frames (dispatched 303, at M 1 1); "
                             "declined: no S 7, stale c 4, open sight 0, not an optical sight 0, left aiming eye 0, depth unavailable 3, format refused 3; picture sequence less c's: -1 1, 0 297, +1 4, +2 1, other 0; "
                             "gunsight marks taken: L 296, R 300; armed frames 310, no mark draw 2; mark depth path: untested 0, seeded 500, seeded read-only 80, tests off 16; composites matched/plain/none 590/4/2; "
                             "hole r min 4.01 median 5.20 max 6.42 deg, 1 of 128 rays escaped (rays read on 296 frames); "
                             "depth at c 0.001220 (near 0.041700); c mean (1642.5, 1990.0) px spread 0.50 px; GPU rays 0.012 ms (max 0.012), magnify 0.046 ms (max 0.046)",
                             0) == 0,
              "S1 lines: the press line carries the item, scope, key, ADS FOV, M, h, the frame thread's counts, the applied and qualifying frames with the dispatched and M = 1 split, every decline, the hole, the depth, c's mean and spread and the GPU price");
        sz::LensPressLine empty;
        empty.refDeg = 56.3;
        sz::formatLensPress(line, sizeof(line), empty);
        const std::string e(line);
        check(e.find("press 0 item none scope unknown key off ads none deg M none h 56.3 deg; frame thread: qualified 0, no S 0, not an optical sight 0, left aiming eye 0, idle 0; FOV from the bare copy 0; "
                     "jitter reading none 0, from another frame 0; c to the far point mean none max none deg; applied 0 of 0 qualifying frames (dispatched 0, at M 1 0); declined: no S 0, stale c 0, open sight 0, "
                     "not an optical sight 0, left aiming eye 0, depth unavailable 0, format refused 0; picture sequence less c's: -1 0, 0 0, +1 0, +2 0, other 0; gunsight marks taken: L 0, R 0; "
                     "armed frames 0, no mark draw 0; mark depth path: untested 0, seeded 0, seeded read-only 0, tests off 0; composites matched/plain/none 0/0/0; ") != std::string::npos,
              "S1 lines: a press with nothing in it says none for what it does not have");
        check(e.find("hole r min none median none max none deg, 0 of 128 rays escaped (rays read on 0 frames); depth at c none (near none); c mean (none, none) px spread none px; "
                     "GPU rays none ms (max none), magnify none ms (max none)") != std::string::npos,
              "S1 lines: no hole, depth, c or GPU time is the word none, so a rule can read it");
        sz::formatLensPress(line, 40, p);
        check(std::strlen(line) < 40, "S1 lines: a buffer that is too small is cut, not overrun");
        check(sz::lensOtherFrames(p.d) == 6, "S1 lines: the A1 count of frames with c from another frame than the picture is the buckets but 0 (1 + 4 + 1 + 0)");
        // the worst case: the longest item name, every counter six digits (a press of ten minutes at 90 Hz is 54000 frames), every figure at its widest. It must fit the buffer the press
        // line is written from (kLensPressLineBytes), and the log's 1200, with its end (the GPU price) still on it.
        sz::LensPressLine w;
        w.press = 200;
        w.item = "$wpn_m_submachinegun_laser_fauto_name;";
        w.scope = "unknown";
        w.keyOn = true;
        w.adsDeg = 15.0;
        w.m = 4.0670;
        w.refDeg = 56.3;
        w.t.qualified = w.t.noS = w.t.notOptical = w.t.leftAim = w.t.idle = w.t.fovFromBare = w.t.jitterNone = w.t.jitterOther = 999999;
        sz::lensNoteFar(&w.t, 123.456);
        w.d.applied = w.d.qualifying = w.d.dispatched = w.d.noop = w.d.noS = w.d.staleC = w.d.openSight = w.d.notOptical = w.d.leftAim = w.d.depthUnavailable = w.d.formatRefused = 999999;
        for (uint32_t k = 0; k < sz::kLensGapBuckets; ++k) w.d.seqGap[k] = 999999;
        for (uint32_t k = 0; k < sz::kMarkPaths; ++k) w.d.markPath[k] = 999999;
        for (uint32_t k = 0; k < sz::kMarkComposites; ++k) w.d.markComposite[k] = 999999;
        w.d.markRedirected[0] = w.d.markRedirected[1] = w.d.markArmedFrames = w.d.markNoDraw = 999999;
        w.d.raysRead = 999999;
        w.d.holeKnown = true;
        w.d.rMinDeg = 11.99;
        w.d.rMedianDeg = 11.99;
        w.d.rMaxDeg = 11.99;
        w.d.holeEscaped = 128;
        w.d.depthAtC = 0.123456;
        w.d.depthNear = 0.123456;
        sz::lensNoteC(&w.d, 4031.5, 4031.5);
        sz::lensNoteGpu(&w.d, 12.345, 12.345);
        const int wn = sz::formatLensPress(line, sizeof(line), w);
        const std::string worst(line);
        check(wn == static_cast<int>(worst.size()) && wn < static_cast<int>(sz::kLensPressLineBytes) &&
                  worst.rfind("magnify 12.345 ms (max 12.345)") == worst.size() - std::strlen("magnify 12.345 ms (max 12.345)"),
              "S1 lines: the press line with every counter at six digits and every figure at its widest is under the press line's buffer (1168 bytes, the log's 1167 and a terminator) and keeps its end");
        std::printf("S1 lines: the worst-case press line is %d bytes (the buffer holds %u, the log keeps 1167)\n", wn, static_cast<unsigned>(sz::kLensPressLineBytes));
        check(worst.size() + 15 + 3 + 15 <= 1200, "S1 lines: the worst-case press line, with the log's stamp and truncation marker reserved, fits the log's 1200-byte line");
    }
}

}  // namespace s1test
