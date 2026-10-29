// F172 checks (native on-foot VR), run172 below. Header only, run by native_frame_test.
//
// F172 (native_on_foot_f172.h): the neck raise published pre-turned by the mouse-look aim pitch. The flight edvr_gfx_20261029_054640 (raise carriage, mouse look): under 10 deg k_up +0.99,
// k_fwd +0.11, c -0.01; 40 deg and over k_up -0.09, k_fwd -1.04, c -1.09. The game turns the eye alignment with the aim's pitch: body_out = R(a) body_in, R(p) (f, u) = (f cos p - u sin p,
// f sin p + u cos p) (F137's convention), so a raise R up published as it stands lands at (forward -R sin a, up R cos a). The rig models the game's turn, the hook's vertical removal and the
// slopes the carriage line would read, and checks the published vector lands at up R, forward 0 at a = 30, 60, 85 and -60; that a = 0, look to aim and a stale aim leave the alignment
// bit for bit as before; that the readers of the published alignment cancel the new z share (F119's physical head, whole; F89's and F87h's y, as published); and by source pins that the serial
// record the hook's removal reads is still the unturned seatedY.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f119.h"
#include "../../src/d3d11/native_on_foot_f137.h"
#include "../../src/d3d11/native_on_foot_f171c.h"
#include "../../src/d3d11/native_on_foot_f172.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f172test {

namespace nf = edvr::nofoot;
namespace f172 = edvr::nofoot::f172;

inline std::string slurpFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

template <class Check>
inline void run172(Check&& check) {
    const auto eq = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };
    char msg[512];
    const double kPi = 3.14159265358979323846;
    const auto rad = [&](double deg) { return deg * kPi / 180.0; };

    // ---- 1. the convention, against the flown slopes ---------------------------------------------------------------------------------------------------------------------------------
    {
        // seated <-> body: (right, up, forward) = (-x, -y, +z); a raise R up is seated y = -R
        const double seated[3] = {0.0, -0.30, 0.0};
        double fwd = 0.0, up = 0.0;
        f172::bodyOfSeated(seated, &fwd, &up);
        double viaCore[3];
        const double body[3] = {0.0, 0.30, 0.0};
        nf::seatedFromBody(body, 0.0, viaCore);
        check(fwd == 0.0 && eq(up, 0.30, 1e-15) && eq(viaCore[1], -0.30, 1e-15) && viaCore[0] == 0.0 && viaCore[2] == 0.0,
              "F172: a raise of 30 cm up is the seated y -0.30 (seatedFromBody), and bodyOfSeated reads it back as up 0.30, forward 0");
        // the game's turn of an UNTURNED raise: R(a) (0, R) = (-R sin a, R cos a). Per unit raise: k_up = cos a, k_fwd = -sin a, and the hook's vertical removal of R leaves G moving by cos a - 1
        const double r5 = rad(5.0), r85 = rad(85.0);
        double f5, u5, f85, u85;
        f172::gameTurn(r5, 0.0, 1.0, &f5, &u5);
        f172::gameTurn(r85, 0.0, 1.0, &f85, &u85);
        std::snprintf(msg, sizeof(msg),
                      "F172: the game's turn of an unturned raise reproduces the flown slopes: at 85 deg k_up %+.2f (flown -0.09), k_fwd %+.2f (flown -1.04), c %+.2f (flown -1.09); at 5 deg k_up %+.2f (flown +0.99), k_fwd %+.2f",
                      u85, f85, u85 - 1.0, u5, f5);
        check(eq(u85, std::cos(r85), 1e-15) && eq(f85, -std::sin(r85), 1e-15) && u85 < 0.1 && f85 < -0.99 && u85 - 1.0 < -0.9 && u5 > 0.99 && f5 > -0.1 && f5 < 0.0, msg);
        // at 40 deg, the top of the middle band: k_up 0.77, k_fwd -0.64 (the flown 10 to 40 band read +0.70 and -0.23, a mix of the pitches in it)
        double f40, u40;
        f172::gameTurn(rad(40.0), 0.0, 1.0, &f40, &u40);
        check(u40 > 0.7 && u40 < 0.8 && f40 < -0.6 && f40 > -0.7, "F172: and at 40 deg the model reads k_up 0.77, k_fwd -0.64");
    }

    // ---- 2. pre-turned, the game's turn lands it straight up ----------------------------------------------------------------------------------------------------------------------
    {
        bool landsUp = true, noForward = true, removalClear = true, formulas = true;
        const double aims[4] = {30.0, 60.0, 85.0, -60.0};
        const double raises[3] = {0.30, -0.45, 0.07};   // up (positive) and down (negative), in metres
        for (double aimDeg : aims) {
            for (double R : raises) {
                const double seatedY = -R;
                const f172::Raise p = f172::publish(seatedY, false, true, rad(aimDeg), true);
                const double seated[3] = {0.0, p.y, p.z};
                double bodyF = 0.0, bodyU = 0.0, outF = 0.0, outU = 0.0;
                f172::bodyOfSeated(seated, &bodyF, &bodyU);
                f172::gameTurn(rad(aimDeg), bodyF, bodyU, &outF, &outU);
                landsUp = landsUp && p.turned && eq(outU, R, 1e-12);
                noForward = noForward && eq(outF, 0.0, 1e-12);
                // the hook's removal is vertical and of R: the camera relative to the neck is then 0 and G does not move with the raise (c 0), k_up 1, k_fwd 0
                removalClear = removalClear && eq(outU - R, 0.0, 1e-12);
                formulas = formulas && p.y == seatedY * std::cos(rad(aimDeg)) && p.z == -seatedY * std::sin(rad(aimDeg)) && p.pitchRad == rad(aimDeg);
            }
        }
        check(landsUp && noForward && removalClear && formulas,
              "F172: published pre-turned (y = seatedY cos a, z = -seatedY sin a), the game's turn lands a raise of +30, -45 and +7 cm at up R, forward 0 at a = 30, 60, 85 and -60 deg, and the hook's vertical removal of R clears it");
        // the slopes the carriage would read over a run of raises: k_up = d(up)/dR = 1, k_fwd = 0, c = d(G)/dR = 0, at every pitch (the next flight's prediction)
        bool slopes = true;
        for (double aimDeg : aims) {
            double sxx = 0.0, sxyUp = 0.0, sxyFwd = 0.0, sxyG = 0.0, prevUp = 0.0, prevFwd = 0.0, prevG = 0.0, prevR = 0.0;
            bool have = false;
            for (int i = 0; i < 40; ++i) {
                const double R = -0.30 + 0.01 * i;
                const f172::Raise p = f172::publish(-R, false, true, rad(aimDeg), true);
                const double seated[3] = {0.0, p.y, p.z};
                double bf, bu, of, ou;
                f172::bodyOfSeated(seated, &bf, &bu);
                f172::gameTurn(rad(aimDeg), bf, bu, &of, &ou);
                const double g = ou - R;   // G with the hook's removal: the camera's up less the raise taken out
                if (have) {
                    const double dx = R - prevR;
                    sxx += dx * dx;
                    sxyUp += dx * (ou - prevUp);
                    sxyFwd += dx * (of - prevFwd);
                    sxyG += dx * (g - prevG);
                }
                have = true;
                prevR = R;
                prevUp = ou;
                prevFwd = of;
                prevG = g;
            }
            slopes = slopes && eq(sxyUp / sxx, 1.0, 1e-9) && eq(sxyFwd / sxx, 0.0, 1e-9) && eq(sxyG / sxx, 0.0, 1e-9);
        }
        check(slopes, "F172: over a run of raises pre-turned, the carriage's slopes come out k_up 1, k_fwd 0 and c 0 at 30, 60, 85 and -60 deg (the prediction for the next flight)");
        // and the same run UNTURNED reads the flown pattern: k_up = cos a, k_fwd = -sin a, c = cos a - 1
        const double a = rad(85.0);
        double of, ou;
        f172::gameTurn(a, 0.0, 0.2, &of, &ou);
        check(eq(ou / 0.2, std::cos(a), 1e-12) && eq(of / 0.2, -std::sin(a), 1e-12) && eq((ou - 0.2) / 0.2, std::cos(a) - 1.0, 1e-12), "F172: unturned the same raise reads k_up cos a, k_fwd -sin a, c cos a - 1");
        // the sign matters: with the z share's sign flipped the raise would land at forward -R sin 2a (a = 60 deg: -26 cm of a 30 cm raise)
        const double a60 = rad(60.0);
        const double sw[3] = {0.0, -0.30 * std::cos(a60), -0.30 * std::sin(a60)};   // the correct z is +0.30 sin a
        double bf, bu, wf, wu;
        f172::bodyOfSeated(sw, &bf, &bu);
        f172::gameTurn(a60, bf, bu, &wf, &wu);
        check(std::fabs(wf) > 0.25, "F172: the z share's sign is not free: with the wrong sign the game's turn leaves a forward part of 26 cm of a 30 cm raise at 60 deg");
    }

    // ---- 3. unturned: the alignment is bit for bit as before ----------------------------------------------------------------------------------------------------------------------
    {
        const double sy = -0.2345678;
        const auto same = [&](const f172::Raise& r) { return !r.turned && std::memcmp(&r.y, &sy, sizeof(double)) == 0 && r.z == 0.0 && !std::signbit(r.z) && r.pitchRad == 0.0; };
        check(same(f172::publish(sy, false, true, 0.0, true)), "F172: at a = 0 the raise is published as it was: y is seatedY to the bit, z is +0, not turned");
        check(same(f172::publish(sy, true, true, rad(60.0), true)), "F172: under look to aim (the aim follows the head: the game turns nothing) the raise is published as it was");
        check(same(f172::publish(sy, false, false, rad(60.0), true)), "F172: with a stale aim pitch the raise is published as it was");
        check(same(f172::publish(sy, false, true, rad(60.0), false)), "F172: with the camera external or off the on-foot camera the raise is published as it was");
        check(same(f172::publish(sy, false, true, std::nan(""), true)) && same(f172::publish(sy, false, true, std::numeric_limits<double>::infinity(), true)),
              "F172: an aim pitch that is not a number leaves it as it was");
        const f172::Raise none = f172::publish(0.0, false, true, rad(60.0), true);
        check(!none.turned && none.y == 0.0 && none.z == 0.0, "F172: no raise, nothing to turn");
        const f172::Raise nanRaise = f172::publish(std::nan(""), false, true, rad(60.0), true);
        check(!nanRaise.turned && std::isnan(nanRaise.y), "F172: a raise that is not a number is passed on as it was");
        const f172::Raise on = f172::publish(sy, false, true, rad(0.5), true);
        check(on.turned && eq(on.y, sy, 2e-5) && std::fabs(on.z) > 1e-3, "F172: a small pitch turns a little and continuously (0.5 deg: z 2 mm of a 23 cm raise)");
    }

    // ---- 4. the readers of the published alignment cancel the new share ---------------------------------------------------------------------------------------------------------------------
    {
        // F119's physical head: the pose's render plus the whole alignment that pose carried (render = physical - published, per axis), so the turned y and the new z share cancel as the
        // old y did, whatever the pitch; F89's and F87h's feeds take the published y as it went out (renderY + y)
        bool whole = true, yFeed = true;
        for (double aimDeg : {0.0, 30.0, 60.0, 85.0, -60.0}) {
            const f172::Raise p = f172::publish(-0.30, false, true, rad(aimDeg), true);
            const double published[3] = {0.0, p.y, p.z};
            const double physical[3] = {0.02, 1.60, -0.05};   // the head in the seated space
            float render[12] = {};   // the pose the runtime reports: the physical head less the alignment it carried, per axis (translation at 3, 7, 11)
            render[3] = static_cast<float>(physical[0] - published[0]);
            render[7] = static_cast<float>(physical[1] - published[1]);
            render[11] = static_cast<float>(physical[2] - published[2]);
            const nf::f119::Offset o = nf::f119::physicalOffset(render, published);
            whole = whole && eq(o.right, physical[0], 1e-6) && eq(o.up, physical[1], 1e-6) && eq(o.forward, -physical[2], 1e-6);
            yFeed = yFeed && eq(static_cast<double>(render[7]) + published[1], physical[1], 1e-6);
        }
        check(whole && yFeed, "F172: F119's physical head (whole vector) and F89's and F87h's feeds (the published y) read the same head with the raise turned as unturned, at 0, 30, 60, 85 and -60 deg");
    }

    // ---- 5. the sources: the record under the serial stays unturned, the readers read what they read ---------------------------------------------------------------------------------------
    {
        const std::string cpp = slurpFile("src/d3d11/native_on_foot.cpp");
        check(!cpp.empty(), "F172: the source pins read src/d3d11/native_on_foot.cpp (run the rig from the repository root)");
        const auto has = [&](const char* needle) { return cpp.find(needle) != std::string::npos; };
        check(has("g_f94NeckSeatedPub = camExternal || !onFoot ? 0.0 : neckRef.out.seatedY;"), "F172: the record kept under the serial for the hook's removal (g_f94NeckSeatedPub) is still the unturned seatedY");
        check(has("if (i == 1) eye[i] += neckRaise.y;") && has("if (i == 2 && neckRaise.turned) eye[i] += neckRaise.z;") && has("nofoot::f172::publish(neckRef.out.seatedY, g_ltaDrop.load(std::memory_order_relaxed), aimFresh, aimRad, onFoot && !camExternal)"),
              "F172: the eye alignment takes the raise's y and, when turned, its z share from f172::publish with F137's inputs (look to aim, the aim's freshness, the commander's first-person camera)");
        check(has("const uint64_t aimRootMs = g_f109RootMs.load(std::memory_order_acquire);") && has("now - aimRootMs <= nofoot::f137::kAimMaxAgeMs") &&
                  has("g_f109RootPitch.load(std::memory_order_relaxed)) * nofoot::kAdsDeg"),
              "F172: the aim pitch is F137's: the pin hook's FP root elevation (g_f109RootPitch), fresh within F137's kAimMaxAgeMs");
        check(has("g_f89Hist.record(serial, static_cast<double>(f[1]));") && has("const double whole[3] = {static_cast<double>(f[0]), static_cast<double>(f[1]), static_cast<double>(f[2])};") &&
                  has("g_f94Hist.record(serial, g_f94NeckSeatedPub);"),
              "F172: F89's history keeps the published y as it went out, F119's history the whole vector, and the hook's history the unturned seatedY");
        check(nf::f137::kAimMaxAgeMs == 300, "F172: F137's aim age limit, which the turn shares, is 300 ms");
    }

    // ---- 6. the counters and the clause -----------------------------------------------------------------------------------------------------------------------------------------------
    {
        f172::Counters c;
        for (int i = 0; i < 90; ++i) c.add(f172::publish(-0.30, false, true, rad(i < 60 ? 80.0 : 40.0), true));
        for (int i = 0; i < 10; ++i) c.add(f172::publish(-0.30, true, true, rad(80.0), true));   // look to aim: carried, not turned
        check(c.ticks == 100 && c.turned == 90 && eq(c.pitchDegSum / c.turned, (60 * 80.0 + 30 * 40.0) / 90.0, 1e-9), "F172: the counters keep the ticks with a raise, those turned, and the mean aim pitch over the turned ones");
        char line[600] = "native on foot raise carriage (F171) 1 s: x";
        const int len = f172::appendTurned(line, sizeof(line), c);
        check(len == static_cast<int>(std::strlen(line)) &&
                  std::strstr(line, "; turned by the aim pitch (F172): 90 of 100 ticks with a raise, mean aim pitch +66.7 deg, mean z share +26.1 cm") != nullptr,
              "F172: the clause reads the ticks turned, the mean aim pitch and the mean z share (a 30 cm raise: 0.30 sin a) as the log prints them");
        f172::Counters none;
        char empty[600] = "x";
        f172::appendTurned(empty, sizeof(empty), none);
        check(std::strstr(empty, "0 of 0 ticks with a raise, mean aim pitch +0.0 deg, mean z share +0.0 cm") != nullptr && std::strstr(empty, "nan") == nullptr, "F172: an empty second prints zeros");
        f172::Counters twice;
        twice.merge(c);
        twice.merge(c);
        check(twice.ticks == 200 && twice.turned == 180, "F172: two seconds add up");
        char tiny[40] = "native on foot raise carriage (F171)";
        f172::appendTurned(tiny, sizeof(tiny), c);
        check(std::strlen(tiny) < sizeof(tiny), "F172: a line buffer too small for the clause ends inside the buffer");
        // the whole carriage line with all six cells at what a second can hold and the clause at its widest, inside the log's 1166-byte message budget
        edvr::nofoot::f171c::Window all;
        for (int m = 0; m < 2; ++m)
            for (int b = 0; b < 3; ++b) {
                edvr::nofoot::f171c::Cell& cell = all.cell[m][b];
                cell.n = 89;
                cell.sxx = 1.0; cell.sxyUp = -9.99; cell.sxyFwd = 9.99; cell.sxyG = -9.99; cell.sxyEye = 9.99;
                cell.raiseMin = -0.50; cell.raiseMax = 0.50;
            }
        all.samples = 90;
        all.f137UpSum = -0.50 * 90.0;
        all.f137FwdSum = -0.50 * 90.0;
        f172::Counters widest;
        widest.ticks = 90;
        widest.turned = 90;
        widest.pitchDegSum = -90.0 * 89.9;
        widest.zCmSum = -90.0 * 50.0;
        char full[1300];
        edvr::nofoot::f171c::formatSummary(full, sizeof(full), 1, all);
        const int fullLen = f172::appendTurned(full, sizeof(full), widest);
        std::snprintf(msg, sizeof(msg), "F172: the raise carriage line with the turned clause is inside the log's 1166 bytes with all six cells at their widest (%d bytes)", fullLen);
        check(fullLen < 1166 && std::strstr(full, "90 of 90 ticks with a raise, mean aim pitch -89.9 deg, mean z share -50.0 cm") != nullptr, msg);
    }
}

}  // namespace f172test
