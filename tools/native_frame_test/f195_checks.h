// F195 checks (native on-foot VR), run195 below. Header only, run by native_frame_test.
//
// F195 (native_on_foot_f195.h, native_on_foot.cpp; behaviour since F195b, the A/B key retired on the pilot's word): at a look-to-aim engage the aim keeps its pitch where the mouse left
// it; only the yaw (and any pitch offset beyond 15 deg) is centred. The pilot flew F194 on his Quest 3 (edvr_gfx_20261031_134656) and said the aim dropped: at four engages after a settled mouse aim F109's
// centred line gave the forward axis at +0.77, +4.46, +4.79 and +5.55 deg from the aim in pitch, and F194's ray (the Quest's picture centre, 13 deg down) moved the aim by -11.39, -7.70, -7.37 and -6.61 deg;
// before F194 the forward axis moved it up 4.5 to 5.6 deg. Either way the engage moved the aim off where he was looking. The rig runs the rule on those four, on a head pitched 26.4 deg up and one 20 deg down,
// on exactly +-15 and on offsets that are not numbers; the follower with a kept offset (no pitch ask, the yaw asked); the line; the sites, which hold that the key is gone.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f109.h"
#include "../../src/d3d11/native_on_foot_f195.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f195test {

namespace nf = edvr::nofoot;
namespace f109 = edvr::nofoot::f109;
namespace f195 = edvr::nofoot::f195;

inline double rad(double d) { return d * nf::kAdsDeg; }
inline double deg(double r) { return r / nf::kAdsDeg; }
inline bool eq(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

template <class Check>
void run195(Check&& check) {
    // ---- 1. the rule on the flown engages -------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        bool all = true;
        for (const double flown : {0.77, 4.46, 4.79, 5.55}) {
            const f195::Kept k = f195::keptPitch(rad(flown));
            all = all && k.centredRad == 0.0 && eq(deg(k.keptRad), flown, 1e-9);
        }
        check(all, "F195: the four flown engages (+0.77, +4.46, +4.79, +5.55 deg of the forward axis from the aim) keep all of it and centre 0 in pitch: the aim does not move");
        const f195::Kept up = f195::keptPitch(rad(26.4));
        check(eq(deg(up.keptRad), 15.0, 1e-9) && eq(deg(up.centredRad), 11.4, 1e-9), "F195: a head pitched 26.4 deg up at the first on-foot frame keeps 15 and centres 11.4");
        const f195::Kept down = f195::keptPitch(rad(-20.0));
        check(eq(deg(down.keptRad), -15.0, 1e-9) && eq(deg(down.centredRad), -5.0, 1e-9), "F195: 20 deg down keeps -15 and centres -5");
        const f195::Kept at = f195::keptPitch(rad(15.0)), atNeg = f195::keptPitch(rad(-15.0));
        check(at.keptRad == rad(15.0) && at.centredRad == 0.0 && atNeg.keptRad == rad(-15.0) && atNeg.centredRad == 0.0, "F195: exactly +15 and -15 are kept whole and centre exactly 0");
        const f195::Kept just = f195::keptPitch(rad(15.01));
        check(just.keptRad == rad(15.0) && eq(deg(just.centredRad), 0.01, 1e-9), "F195: 15.01 deg keeps 15 and centres the 0.01 beyond it");
        const f195::Kept zero = f195::keptPitch(0.0), tiny = f195::keptPitch(rad(-0.3));
        check(zero.keptRad == 0.0 && zero.centredRad == 0.0 && tiny.centredRad == 0.0 && eq(deg(tiny.keptRad), -0.3, 1e-12), "F195: a level head keeps and centres nothing; a small offset is kept whole");
        // kept + centred is always the offset
        bool sum = true;
        for (int i = -400; i <= 400; ++i) {
            const double op = rad(i * 0.2);
            const f195::Kept k = f195::keptPitch(op);
            sum = sum && eq(k.keptRad + k.centredRad, op, 1e-12) && std::fabs(k.keptRad) <= rad(15.0) + 1e-12 && (k.keptRad * k.centredRad >= 0.0);
        }
        check(sum, "F195: kept plus centred is the offset, the kept part never exceeds 15 deg, and both are on the offset's side, for every offset from -80 to +80 deg");
    }
    // ---- 2. the offsets that are not numbers: F109's full centring, as before ----------------------------------------------------------------------------------------------------------------------------------
    {
        const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
        const f195::Kept n = f195::keptPitch(nan), pi = f195::keptPitch(inf), ni = f195::keptPitch(-inf);
        check(n.keptRad == 0.0 && !std::isfinite(n.centredRad) && pi.keptRad == 0.0 && pi.centredRad == inf && ni.keptRad == 0.0 && ni.centredRad == -inf,
              "F195: an offset that is not a number keeps nothing and centres it as it came (the follower's addHead ignores what is not finite: the old behaviour)");
        f109::Follower fl;
        fl.addHead(0.0, n.centredRad);
        check(fl.target[1] == 0.0, "F195: ... and a follower given it takes nothing");
    }
    // ---- 3. the follower: a kept offset asks no pitch, the yaw is centred whole -------------------------------------------------------------------------------------------------------------------------------
    {
        const double oy = rad(30.0), op = rad(4.46);
        const f195::Kept k = f195::keptPitch(op);
        f109::Follower on, whole;
        on.addHead(oy, k.centredRad);
        whole.addHead(oy, op);   // F109's full centring, for the comparison
        double ay = 0, ap = 0, by = 0, bp = 0;
        on.ask(1.0 / 90.0, 0.0, 0.0, 0.0, &ay, &ap);
        whole.ask(1.0 / 90.0, 0.0, 0.0, 0.0, &by, &bp);
        check(ap == 0.0 && on.target[1] == 0.0 && on.target[0] == oy && by == ay && bp > 0.0 && eq(whole.target[1], op, 1e-12),
              "F195: a 4.46 deg pitch offset leaves the follower no pitch target and no pitch ask while the 30 deg yaw is asked as before; F109's full centring would have asked the pitch up");
        // the take threshold judges what is centred: a kept offset with the yaw level is already on it
        const double mag = std::sqrt(0.0 * 0.0 + k.centredRad * k.centredRad) / f109::kDeg;
        check(!(mag >= f109::kCentreMinDeg), "F195: a settled pitch offset with the yaw level is under the take threshold once it is kept (the centring is 'already on it')");
        const f195::Kept beyond = f195::keptPitch(rad(130.0));
        check(eq(deg(beyond.centredRad), 115.0, 1e-9) && eq(deg(beyond.keptRad), 15.0, 1e-9) && deg(beyond.centredRad) <= f109::kCentreMaxDeg && 130.0 > f109::kCentreMaxDeg,
              "F195: the over-120 deg rule judges the centred part (130 deg of offset centres 115 and is taken; the whole 130 would have been left alone)");
    }
    // ---- 4. the sites, the line, the retired key ---------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string cpp = slurp("src/d3d11/native_on_foot.cpp"), ini = slurp("edvr.ini"), h = slurp("src/d3d11/native_on_foot_f195.h");
        const auto has = [&](const std::string& needle) { return cpp.find(needle) != std::string::npos; };
        const auto at = [&](const std::string& needle) { return cpp.find(needle); };
        check(!cpp.empty() && !ini.empty() && !h.empty(), "F195: the sources the pins read are readable from the repo root");
        check(has("const nofoot::f195::Kept kept = nofoot::f195::keptPitch(op);") && has("if (take) g_lta.follow.addHead(oy, kept.centredRad);") &&
                  has("const double mag = std::sqrt(oy * oy + kept.centredRad * kept.centredRad) / f109::kDeg;") && !has("if (take) g_lta.follow.addHead(oy, op);"),
              "F195: the engage's centring puts the yaw and only the pitch beyond the kept offset into the follower, and judges the take threshold on that");
        const size_t keptAt = at("const nofoot::f195::Kept kept = nofoot::f195::keptPitch(op);"), addAt = at("if (take) g_lta.follow.addHead(oy, kept.centredRad);");
        check(keptAt != std::string::npos && addAt != std::string::npos && keptAt < addAt && addAt - keptAt < 700, "F195: the kept part is worked out right before the follower is given the rest");
        check(has("pitch kept %+.2f deg, centred %+.2f deg; %s\",") && has("kept.keptRad / f109::kDeg, kept.centredRad / f109::kDeg,\n") && !has("(F195:"),
              "F195: the centred line says the pitch kept and the pitch centred, and no longer whether a key was on");
        check(has("if (f109::centreOffset(np.render, &oy, &op)) {") && cpp.find("g_ltaCentre") == std::string::npos && cpp.find("picture-centre pitch") == std::string::npos,
              "F195: the engage's centring reads the reported head's offset from centreOffset (the forward axis) and keeps its pitch (F194b)");
        // F195b: the key is retired. The needles are stems, cut short on purpose, so that the tree holds no spelling of the retired name outside the docs.
        check(cpp.find("look_to_aim_keeps") == std::string::npos && cpp.find("g_ltaKeep") == std::string::npos && ini.find("look_to_aim_keeps") == std::string::npos && ini.find("keep the aim's pitch") == std::string::npos &&
                  ini.find("(F195, A/B)") == std::string::npos && h.find("keyOn") == std::string::npos && h.find("RETIRED on 2026-10-31") != std::string::npos,
              "F195b: the keep-the-aim's-pitch key is retired: nothing reads it, no flag holds it, keptPitch has no key argument, edvr.ini has neither its entry nor its menu row");
    }
}

}  // namespace f195test
