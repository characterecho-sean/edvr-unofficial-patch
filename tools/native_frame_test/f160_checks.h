// F160 checks (native on-foot VR), run160 below. Header only, run by native_frame_test.
//
// F160 (native_on_foot_f160.h): the neck reference's raise comes back with the sights letting go. The flight edvr_gfx_20261028_125106's release is replayed on the real parts
// (native_on_foot_core.h AdsRamp, native_on_foot_f95.h StanceSettle, native_on_foot_f94.h NeckRaise): the raise stood at +11.7 cm, the press took it to 0, and after the release it
// returned 0.63 s after the request went off (the 0.3 s ramp, a fresh 0.3 s settle window, the follower's 0.03 s); with the kept target published as (1 - w) times it, it returns
// with the release. The holster bridge's checks are in f99_checks.h (run160Holster).
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f94.h"
#include "../../src/d3d11/native_on_foot_f95.h"
#include "../../src/d3d11/native_on_foot_f160.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace f160test {

namespace nf = edvr::nofoot;
namespace f94 = edvr::nofoot::f94;
namespace f95 = edvr::nofoot::f95;
namespace f99 = edvr::nofoot::f99;
namespace f160 = edvr::nofoot::f160;

constexpr double kDt = 1.0 / 90.0;

struct Release {
    double beforeM = 0.0;    // the raise at the end of the press (the sights held), m
    double at45M = 0.0;      // the raise 0.45 s after the request went off
    double backS = 99.0;     // when the raise first reached 90% of the settled target after the request went off, s
    double finalM = 0.0;     // the raise 2 s after
    double maxStepM = 0.0;   // the largest change of the raise in one tick during the release
    uint32_t releases = 0, blended = 0, calls = 0;
};

// One press and release at 90 Hz. The stance stood at settledM with the item 7 in the standing class; blend: F160's publication (else the target 0 of the build before it); item and
// cls: those of the release's calls; storedM: the target kept (a NaN: none kept).
inline Release replay(bool blend, uint32_t item, int cls, double storedM, double settledM) {
    f160::AdsReturn ret;
    if (std::isfinite(storedM)) ret.store(storedM, 7u, f94::kClassStanding);
    nf::AdsRamp ramp;
    f95::StanceSettle settle;
    f94::NeckRaise raise;
    f160::Counts counts;
    double tS = 0.0, lastPub = 0.0, lastPubS = -9.0;
    bool ads = false;
    const auto tick = [&](bool storeSettled) {
        ramp.step(ads, kDt);
        tS += kDt;
        const double w = ramp.w;
        f95::SettleIn si;
        si.valid = !(w > 0.0);
        si.still = true;
        si.tS = tS;
        si.neckY = 1.543;
        si.camY = 1.58;
        const f95::SettleOut so = settle.step(si);
        bool publish = false;
        double target = 0.0;
        if (w > 0.0) {
            uint8_t why = f160::kPress;
            bool begins = false;
            if (blend) {
                const f160::AdsReturn::Out o = ret.step(w, ads, item, cls);
                target = o.r.targetM;
                why = o.r.why;
                begins = o.releaseBegins;
            }
            counts.add(why, begins);
            publish = true;
        } else {
            f160::AdsReturn::Done done;
            if (blend && ret.falling) done = ret.finish(item, cls);   // the release ran its course: the kept target whole
            if (done.publish) {
                target = done.targetM;
                publish = true;
            } else if (so.settled) {
                target = settledM;
                publish = true;
                if (storeSettled) ret.store(target, 7u, f94::kClassStanding);
            }
        }
        if (publish) {
            lastPub = target;
            lastPubS = tS;
        }
        f94::In in;
        in.scope = true;
        in.dt = kDt;
        in.settleS = 0.1;
        in.targetFresh = tS - lastPubS <= 0.25;
        in.targetM = lastPub;
        return raise.step(in).raiseM;
    };
    Release r;
    for (int i = 0; i < 180; ++i) tick(false);   // 2 s standing at the stance's raise (the kept target is the test's)
    ads = true;
    for (int i = 0; i < 90; ++i) r.beforeM = tick(false);   // the press, then the sights held for 1 s
    ads = false;
    double prev = r.beforeM, t = 0.0;
    bool got45 = false;
    for (int i = 0; i < 180; ++i) {   // the release: the request off at t = 0
        const double m = tick(false);
        t += kDt;
        if (t >= 0.45 && !got45) {
            r.at45M = m;
            got45 = true;
        }
        if (r.backS > 98.0 && m >= 0.9 * settledM) r.backS = t;
        r.maxStepM = std::fmax(r.maxStepM, std::fabs(m - prev));
        prev = m;
        r.finalM = m;
    }
    r.releases = counts.releases;
    r.blended = counts.blended;
    r.calls = counts.callsBlended;
    return r;
}

template <class Check>
inline void run160(Check&& check) {
    constexpr double kStored = 0.117;
    f160::Stored s;
    s.have = true;
    s.targetM = kStored;
    s.item = 7u;
    s.cls = f94::kClassStanding;
    // ---- 1. the decision --------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const f160::Result press = f160::adsReturnTarget(0.6, true, s, 7u, f94::kClassStanding);
        const f160::Result held = f160::adsReturnTarget(1.0, true, s, 7u, f94::kClassStanding);
        check(press.targetM == 0.0 && press.why == f160::kPress && held.targetM == 0.0 && held.why == f160::kPress,
              "F160: the press and the hold publish 0 as before, whatever is kept");
        const f160::Result a = f160::adsReturnTarget(1.0, false, s, 7u, f94::kClassStanding), b = f160::adsReturnTarget(0.5, false, s, 7u, f94::kClassStanding),
                           c = f160::adsReturnTarget(0.0625, false, s, 7u, f94::kClassStanding), d = f160::adsReturnTarget(1.7, false, s, 7u, f94::kClassStanding);
        check(a.why == f160::kBlend && a.targetM == 0.0 && b.why == f160::kBlend && std::fabs(b.targetM - 0.0585) < 1e-12 && c.why == f160::kBlend &&
                  std::fabs(c.targetM - 0.9375 * kStored) < 1e-12 && d.why == f160::kBlend && d.targetM == 0.0,
              "F160: the release with the item and the stance class of the kept target publishes (1 - w) times it (0 at w = 1, half at 0.5, a weight over 1 held at 1)");
        const f160::Result item = f160::adsReturnTarget(0.5, false, s, 8u, f94::kClassStanding), cls = f160::adsReturnTarget(0.5, false, s, 7u, f94::kClassCrouched),
                           unk = f160::adsReturnTarget(0.5, false, s, 7u, f94::kClassUnknown);
        check(item.why == f160::kItem && item.targetM == 0.0 && cls.why == f160::kStance && cls.targetM == 0.0 && unk.why == f160::kStance && unk.targetM == 0.0,
              "F160: the release with another item, or another stance class, than the kept target's publishes 0 as before");
        f160::Stored none;
        f160::Stored nan = s;
        nan.targetM = std::nan("");
        const f160::Result n1 = f160::adsReturnTarget(0.5, false, none, 7u, f94::kClassStanding), n2 = f160::adsReturnTarget(0.5, false, nan, 7u, f94::kClassStanding);
        check(n1.why == f160::kNoStored && n1.targetM == 0.0 && n2.why == f160::kNoStored && n2.targetM == 0.0,
              "F160: the release with no target kept (or one that is not a number) publishes 0 as before");
        const f160::Result off = f160::adsReturnTarget(0.0, false, s, 7u, f94::kClassStanding), bad = f160::adsReturnTarget(std::nan(""), false, s, 7u, f94::kClassStanding),
                           neg = f160::adsReturnTarget(-0.2, false, s, 7u, f94::kClassStanding);
        check(off.why == f160::kPress && off.targetM == 0.0 && bad.why == f160::kPress && bad.targetM == 0.0 && neg.why == f160::kPress && neg.targetM == 0.0,
              "F160: a weight of 0, below 0 or not a number is not the sights: nothing to blend");
        f160::Stored same = s;
        same.cls = f94::kClassUnknown;
        const f160::Result unknown = f160::adsReturnTarget(0.5, false, same, 7u, f94::kClassUnknown);
        check(unknown.why == f160::kBlend, "F160: a stance class not known when kept and not known at the release is the same class");
    }
    // ---- 2. the state: what is kept, when a release begins ----------------------------------------------------------------------------------------------------------------------
    {
        f160::AdsReturn r;
        const f160::AdsReturn::Out none = r.step(0.5, false, 7u, f94::kClassStanding);
        r.store(std::nan(""), 7u, f94::kClassStanding);
        check(none.r.why == f160::kNoStored && !r.stored.have, "F160: nothing is kept before a settled stance, and a target that is not a number is never kept");
        r.store(0.117, 7u, f94::kClassStanding);
        r.store(0.121, 7u, f94::kClassStanding);
        check(r.stored.have && r.stored.targetM == 0.121 && r.stored.item == 7u && r.stored.cls == f94::kClassStanding, "F160: the newest settled target is the one kept");
        const f160::AdsReturn::Out p1 = r.step(0.4, true, 7u, f94::kClassStanding), p2 = r.step(1.0, true, 7u, f94::kClassStanding);
        const f160::AdsReturn::Out r1 = r.step(0.9, false, 7u, f94::kClassStanding), r2 = r.step(0.6, false, 7u, f94::kClassStanding), r3 = r.step(0.3, false, 7u, f94::kClassStanding);
        check(!p1.releaseBegins && !p2.releaseBegins && r1.releaseBegins && !r2.releaseBegins && !r3.releaseBegins && r1.r.why == f160::kBlend,
              "F160: a release begins at its first call, once; the press and the hold are not releases");
        const f160::AdsReturn::Done end = r.finish(7u, f94::kClassStanding), twice = r.finish(7u, f94::kClassStanding);
        check(end.publish && end.targetM == 0.121 && !twice.publish && !r.falling,
              "F160: the first call after a release ran its course publishes the kept target whole, once");
        r.step(0.3, false, 8u, f94::kClassStanding);
        const f160::AdsReturn::Done otherItem = r.finish(8u, f94::kClassStanding);
        r.step(0.3, false, 7u, f94::kClassStanding);
        const f160::AdsReturn::Done otherStance = r.finish(7u, f94::kClassCrouched);
        r.step(0.3, false, 7u, f94::kClassStanding);
        r.step(0.1, true, 7u, f94::kClassStanding);
        const f160::AdsReturn::Done pressed = r.finish(7u, f94::kClassStanding);
        r.step(0.3, false, 7u, f94::kClassStanding);
        r.leave();
        const f160::AdsReturn::Done away = r.finish(7u, f94::kClassStanding);
        check(!otherItem.publish && !otherStance.publish && !pressed.publish && !away.publish,
              "F160: nothing is published at the end of a release of another item or stance class, one a press interrupted, or one the camera leaving the commander cut off");
        const f160::AdsReturn::Out again = r.step(0.9, false, 7u, f94::kClassStanding);
        r.step(0.5, false, 7u, f94::kClassStanding);
        const f160::AdsReturn::Out quick = r.step(0.5, true, 7u, f94::kClassStanding);   // the sights asked for again half way through the release
        const f160::AdsReturn::Out second = r.step(0.4, false, 7u, f94::kClassStanding);
        check(again.releaseBegins && !quick.releaseBegins && quick.r.targetM == 0.0 && quick.r.why == f160::kPress && second.releaseBegins &&
                  std::fabs(second.r.targetM - 0.6 * 0.121) < 1e-12,
              "F160: leaving the sights ends a release; a press half way through one publishes 0, and the release after it begins again at its weight");
        r.reset();
        check(!r.stored.have && !r.falling, "F160: a reset forgets the kept target");
    }
    // ---- 3. the counts ----------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f160::Counts c;
        c.add(f160::kPress, false);
        c.add(f160::kBlend, true);
        c.add(f160::kBlend, false);
        c.add(f160::kBlend, false);
        c.add(f160::kItem, true);
        c.add(f160::kItem, false);
        c.add(f160::kNoStored, true);
        c.add(f160::kStance, true);
        c.add(f160::kNone, false);
        check(c.releases == 4 && c.blended == 1 && c.callsBlended == 3 && c.notBlended[0] == 1 && c.notBlended[1] == 1 && c.notBlended[2] == 1 && c.any(),
              "F160: the releases are counted at their first call and classed by it; the calls that published a blend are counted apart");
        f160::Counts m;
        m.merge(c);
        m.merge(c);
        f160::Counts empty;
        check(m.releases == 8 && m.blended == 2 && m.callsBlended == 6 && m.notBlended[0] == 2 && m.notBlended[1] == 2 && m.notBlended[2] == 2 && !empty.any(),
              "F160: the second's counts merge into the window's by sum; a window with none has nothing to say");
    }
    // ---- 4. the flight's release on the real parts ------------------------------------------------------------------------------------------------------------------------------
    {
        const Release before = replay(false, 7u, f94::kClassStanding, kStored, 0.118);
        const Release after = replay(true, 7u, f94::kClassStanding, kStored, 0.118);
        check(std::fabs(before.beforeM) < 0.0015 && std::fabs(after.beforeM) < 0.0015,
              "F160 replay: the press takes the raise to 0 in both builds (the sights hold it at 0, as flown)");
        check(before.backS > 0.58 && before.backS < 0.75 && before.at45M < 0.01,
              "F160 replay (the build before): the raise stays near 0 until the fresh settle window is full and returns 0.63 s after the request went off");
        check(after.backS < 0.40 && std::fabs(after.at45M - kStored) < 0.003,
              "F160 replay: with the kept target blended through the release the raise is back by 0.4 s (90% of it), at +11.7 cm at 0.45 s: no plateau");
        check(std::fabs(before.finalM - 0.118) < 0.0011 && std::fabs(after.finalM - 0.118) < 0.0011,
              "F160 replay: the first settled sample after the release corrects the small difference: the raise ends at the settled target (+11.8 cm) in both builds");
        check(before.maxStepM > 0.02 && after.maxStepM < 0.008,
              "F160 replay: the raise's largest step in a tick, 2+ cm before (the late jump), under 0.8 cm with the blend");
        check(after.releases == 1 && after.blended == 1 && after.calls > 20 && after.calls < 30 && before.releases == 0,
              "F160 replay: one release, blended, over the 0.3 s ramp's 27 calls");
        const Release item = replay(true, 8u, f94::kClassStanding, kStored, 0.118);
        const Release cls = replay(true, 7u, f94::kClassCrouched, kStored, 0.118);
        const Release none = replay(true, 7u, f94::kClassStanding, std::nan(""), 0.118);
        check(item.backS > 0.58 && cls.backS > 0.58 && none.backS > 0.58 && item.releases == 1 && item.blended == 0 && cls.blended == 0 && none.blended == 0,
              "F160 replay: another item, another stance class, or nothing kept falls back to the 0 target and the 0.63 s return, not blended");
        const Release low = replay(true, 7u, f94::kClassStanding, kStored, 0.095);
        check(std::fabs(low.finalM - 0.095) < 0.0011 && low.backS < 0.40,
              "F160 replay: a stance that settled 2.2 cm lower after the release ends at its own target (the settle window corrects the kept one)");
    }
    // ---- 5. the holster bridge's line ------------------------------------------------------------------------------------------------------------------------------------------
    {
        f160::HolsterLine h;
        std::snprintf(h.item, sizeof(h.item), "%s", "$wpn_m_assaultrifle_laser_fauto_name;");
        h.fromId = 0x12u;
        h.end = f99::kHolsterStatus;
        h.ranMs = 1349.6;
        char buf[320];
        const int n = f160::formatHolster(buf, sizeof(buf), h);
        check(n > 0 && n < 320 && std::strstr(buf, "native on foot holster bridge (F160): item $wpn_m_assaultrifle_laser_fauto_name;") == buf &&
                  std::strstr(buf, "after id 00000012") && std::strstr(buf, "the bridge ran 1350 ms and ended: Status.json named no item") &&
                  !std::strstr(buf, "not logged"),
              "F160: the holster line names the item, the id the game had selected, how long the bridge ran and how it ended");
        h.end = f99::kHolsterDrawn;
        h.skipped = 3;
        h.item[0] = '\0';
        f160::formatHolster(buf, sizeof(buf), h);
        check(std::strstr(buf, "item none;") && std::strstr(buf, "ended: the game selected an item again; 3 earlier bridge(s) not logged (one line in 0.5 s)"),
              "F160: a drawn end, a bridge that began with no item name, and the count of bridges the rate limit kept out");
        h.end = f99::kHolsterTimeout;
        h.skipped = 0;
        f160::formatHolster(buf, sizeof(buf), h);
        check(std::strstr(buf, "ended: the 4 s limit") != nullptr, "F160: a timeout is said as the 4 s limit");
        f160::LineGate g;
        uint32_t skipped = 99;
        const bool first = g.take(1000, &skipped);
        const bool held1 = g.take(1100, &skipped), held2 = g.take(1400, &skipped);
        uint32_t carried = 99;
        const bool next = g.take(1500, &carried);
        uint32_t after = 99;
        const bool gap = !g.take(1700, &after);
        check(first && !held1 && !held2 && next && carried == 2 && gap,
              "F160: the holster line passes once in 0.5 s, and the next that passes says how many bridges were kept out");
    }
}

}  // namespace f160test
