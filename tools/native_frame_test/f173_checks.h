// F173 checks (native on-foot VR), run173 below. Header only, run by native_frame_test.
//
// F173: the pilot's flown values are the defaults of the four experimental dials (they stay experimental and TEMPORARY): experimental.native_on_foot_crouch_ads_body_forward_cm 5 (was 0),
// native_on_foot_crouch_ads_body_left_cm 25 (was 15), native_on_foot_crouch_ads_square 0 (was 1) and native_on_foot_pistol_sprint_swing_phase 0.5 (was 0). A default lives in three places
// in native_on_foot.cpp and edvr.ini: the atomic's initial value, the cfg fallback and the shipped line; the rig pins all of them, one check per key (the menu's shipped column is read from
// the ini line and is checked in hotkey_capture_test, the mirror window's default in desktop_window_test). Every other rig in this file set the dial it means, so it still proves what it did.
//
// The new non-zero defaults must change nothing standing, at the hip or unarmed. They are gated by the crouched weight (F163) and the detach weight (the sights) for the body moves, by the
// armed pistol's release weight (F167) for the swing, and the torso square is 0; the rig runs the real functions at the SHIPPED values, read from edvr.ini, with each weight 0.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f168.h"
#include "../../src/d3d11/native_on_foot_f169.h"
#include "../../src/d3d11/native_on_foot_f170.h"
#include "../../src/d3d11/native_on_foot_f171.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace f173test {

namespace nf = edvr::nofoot;

inline std::string slurpIni(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
// The value of `key = value` on a line of its own in the ini text; NaN when the key is not there exactly once.
inline double iniValue(const std::string& ini, const std::string& key) {
    const std::string needle = "\n" + key + " = ";
    const size_t at = ini.find(needle);
    if (at == std::string::npos || ini.find(needle, at + 1) != std::string::npos) return std::nan("");
    return std::strtod(ini.c_str() + at + needle.size(), nullptr);
}

template <class Check>
inline void run173(Check&& check) {
    const std::string ini = slurpIni("edvr.ini"), cpp = slurpIni("src/d3d11/native_on_foot.cpp");
    check(!ini.empty() && !cpp.empty(), "F173: the default pins read edvr.ini and src/d3d11/native_on_foot.cpp (run the rig from the repository root)");
    const auto has = [&](const char* needle) { return cpp.find(needle) != std::string::npos; };

    // ---- 1. one check per key: the atomic's initial value, the cfg fallback and the shipped line are all the new default --------------------------------------------------------------------
    check(has("std::atomic<float> g_crouchAdsBodyCm{5.0f};") && has("cfg.getFloat(\"experimental.native_on_foot_crouch_ads_body_forward_cm\", 5.0f)") &&
              iniValue(ini, "native_on_foot_crouch_ads_body_forward_cm") == 5.0,
          "F173: experimental.native_on_foot_crouch_ads_body_forward_cm defaults to 5 (the atomic, the fallback and the shipped line)");
    check(has("std::atomic<float> g_crouchAdsBodyLeftCm{25.0f};") && has("cfg.getFloat(\"experimental.native_on_foot_crouch_ads_body_left_cm\", 25.0f)") &&
              iniValue(ini, "native_on_foot_crouch_ads_body_left_cm") == 25.0,
          "F173: experimental.native_on_foot_crouch_ads_body_left_cm defaults to 25 (the atomic, the fallback and the shipped line)");
    check(has("std::atomic<float> g_crouchAdsSquare{0.0f};") && has("cfg.getFloat(\"experimental.native_on_foot_crouch_ads_square\", 0.0f)") &&
              iniValue(ini, "native_on_foot_crouch_ads_square") == 0.0,
          "F173: experimental.native_on_foot_crouch_ads_square defaults to 0 (the atomic, the fallback and the shipped line)");
    check(has("std::atomic<float> g_pistolSprintPhase{0.5f};") && has("cfg.getFloat(\"experimental.native_on_foot_pistol_sprint_swing_phase\", 0.5f)") &&
              iniValue(ini, "native_on_foot_pistol_sprint_swing_phase") == 0.5,
          "F173: experimental.native_on_foot_pistol_sprint_swing_phase defaults to 0.5 (the atomic, the fallback and the shipped line)");
    // the keys stay where they are, as they are: experimental, TEMPORARY, each with its ui: line
    check(ini.find("# TEMPORARY (F168): Crouched ADS body forward / back") != std::string::npos && ini.find("# TEMPORARY (F171): Crouched ADS body left / right") != std::string::npos &&
              ini.find("# TEMPORARY (F170): Crouched ADS torso square") != std::string::npos && ini.find("# TEMPORARY (F169): Pistol sprint arm swing phase") != std::string::npos &&
              ini.find("# ui: Crouched ADS body forward / back | range -20..20 |") != std::string::npos && ini.find("# ui: Crouched ADS body left / right | range -30..30 |") != std::string::npos &&
              ini.find("# ui: Crouched ADS torso square | range 0..1 |") != std::string::npos && ini.find("# ui: Pistol sprint arm swing phase | range 0..1 |") != std::string::npos,
          "F173: the four keys stay TEMPORARY, with their ui: lines and ranges, only the defaults changed");

    // ---- 2. the shipped values change nothing standing, at the hip or unarmed ---------------------------------------------------------------------------------------------------------------
    const double forwardCm = iniValue(ini, "native_on_foot_crouch_ads_body_forward_cm"), leftCm = iniValue(ini, "native_on_foot_crouch_ads_body_left_cm"),
                 square = iniValue(ini, "native_on_foot_crouch_ads_square"), phase = iniValue(ini, "native_on_foot_pistol_sprint_swing_phase");
    const double aim[3] = {0.6, 0.3, 0.8};
    {
        bool zero = true, made = false;
        const double postures[3] = {0.0, 0.0, 1.0}, details[3] = {1.0, 0.0, 0.0};   // standing in the sights, crouched at the hip, standing at the hip
        for (int i = 0; i < 3; ++i) {
            double a[3] = {9.0, 9.0, 9.0}, b[3] = {9.0, 9.0, 9.0};
            made = nf::f168::shiftVector(forwardCm, postures[i], details[i], aim, a) || made;
            made = nf::f171::shiftVector(leftCm, postures[i], details[i], aim, b) || made;
            zero = zero && a[0] == 0.0 && a[1] == 0.0 && a[2] == 0.0 && b[0] == 0.0 && b[1] == 0.0 && b[2] == 0.0;
        }
        // the crouched-in-the-sights case does move at the shipped values (5 cm forward, 25 cm left), so the zeros above are the weights', not a dead path
        double fwdMove[3], leftMove[3];
        const bool inSights = nf::f168::shiftVector(forwardCm, 1.0, 1.0, aim, fwdMove) && nf::f171::shiftVector(leftCm, 1.0, 1.0, aim, leftMove);
        const double fwdLen = std::sqrt(fwdMove[0] * fwdMove[0] + fwdMove[2] * fwdMove[2]), leftLen = std::sqrt(leftMove[0] * leftMove[0] + leftMove[2] * leftMove[2]);
        check(zero && !made && inSights && std::fabs(fwdLen - 0.05) < 1e-12 && std::fabs(leftLen - 0.25) < 1e-12,
              "F173: at the shipped 5 cm and 25 cm the body moves are exactly zero standing in the sights, crouched at the hip and standing at the hip (the posture and detach weights gate them), and 5 cm and 25 cm crouched in the sights");
        double a[3], b[3];
        const bool zeroForward = nf::f168::shiftVector(0.0, 1.0, 1.0, aim, a), zeroLeft = nf::f171::shiftVector(0.0, 1.0, 1.0, aim, b);
        check(!zeroForward && !zeroLeft && a[0] == 0.0 && a[2] == 0.0 && b[0] == 0.0 && b[2] == 0.0,
              "F173: a dial set to 0 explicitly is still exactly zero, bit for bit, crouched in the sights (the guarantee at dial 0 holds)");
    }
    {
        // the torso square at its shipped 0: no share whatever the weights; the old default of 1 still turns when set (F170's rig sets it)
        check(nf::f170::share(square, 1.0, 1.0) == 0.0 && nf::f170::share(square, 0.0, 1.0) == 0.0 && nf::f170::share(1.0, 1.0, 1.0) == 1.0 && nf::f170::turnRad(nf::f170::share(square, 1.0, 1.0), true, 0.5) == 0.0,
              "F173: the torso square ships at 0: no share and no turn crouched in the sights, and it still turns when the dial is set to 1");
        // the swing at its shipped 0.5: nothing is applied unarmed, with a pistol not released (walking, standing, aiming) or released with the dial at 0; the delay needs the release weight
        namespace f169 = nf::f169;
        const auto run = [&](bool pistolArmed, double release, double dial, bool* everApplied) {
            f169::Controller c;
            bool applied = false;
            for (int i = 0; i < 400; ++i) {   // 4 s at 100 Hz, a stride of 0.6 s on the signal
                f169::In in;
                in.us = 1000000ull + static_cast<uint64_t>(i) * 10000ull;
                in.pistolArmed = pistolArmed;
                in.sprinting = true;
                in.release = release;
                in.phase = dial;
                in.strideSignal = 0.2 * std::sin(2.0 * 3.14159265358979323846 * (static_cast<double>(i) * 0.01) / 0.6);
                in.curValid = true;
                const f169::Out out = c.step(in);
                applied = applied || out.apply;
            }
            *everApplied = applied;
        };
        bool unarmed = true, notReleased = true, dialZero = true, released = false;
        run(false, 1.0, phase, &unarmed);
        run(true, 0.0, phase, &notReleased);
        run(true, 1.0, 0.0, &dialZero);
        run(true, 1.0, phase, &released);
        check(phase == 0.5 && !unarmed && !notReleased && !dialZero && released,
              "F173: the swing ships at 0.5 and applies nothing unarmed or with the release weight 0 (standing, walking, the sights), nothing with the dial at 0, and does apply in a released pistol sprint");
    }
}

}  // namespace f173test
