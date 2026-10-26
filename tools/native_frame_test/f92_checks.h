// F92b checks (native on-foot VR), run92 below: the ADS head aim's cancel published ahead (AdsAimMap::yawPublished / pitchPublished, native_on_foot_core.h) on a plant
// with the F91 flight's timing, and the head aim as a live toggle within a press (native_on_foot_f92.h) composed with the press's state edges, its weight, the left eye's
// share (F106; the darkening until then) and the same plant. Header only, run by native_frame_test.
//
// The plant (one axis, degrees in the seat convention): the head h(F) at frame F; tick F steps the real map with the head's step and the turn the game applied, which is
// the ask of tick F - 1 (the F91 flight: applied(N) = asked(N - 1) to 0.0028 deg rms); the game's aim Look(F) is the sum of what it applied; the pose of frame F carries
// the value published at tick F - 1 (the F91 flight: the view less the head equal to the game's last applied step to 0.0005 deg rms). The view less the head is then
// r(F) = Look(F) - P(F - 1), and the weapon against the eye is P(F - 1) - h(F).
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"   // AdsAimMap, AdsRamp, f106::share
#include "../../src/d3d11/native_on_foot_f92.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace f92test {

namespace nf = edvr::nofoot;
namespace f89 = edvr::nofoot::f89;
namespace f91 = edvr::nofoot::f91;
namespace f92 = edvr::nofoot::f92;

struct Lcg {
    uint64_t s = 0x2545F4914F6CDD1Dull;
    double next() noexcept {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>((s >> 11) & ((1ull << 53) - 1)) / static_cast<double>(1ull << 52) - 1.0;
    }
};

// One press through the plant. key(i) is the toggle at frame i, ads(i) the resolved request; refuse(i) makes the game apply nothing at frame i. published: true for
// F92b's published cancel, false for the cancel as published before it (what the game applied only).
struct Frame {
    double head = 0.0, look = 0.0, pose = 0.0, r = 0.0, weaponEye = 0.0, ask = 0.0, w = 0.0, share = 0.0;
    int state = 0, mode = 0;
};
template <class Key, class Ads, class Head, class Refuse>
inline std::vector<Frame> runPlant(int n, Key key, Ads ads, Head headAt, Refuse refuse, bool published) {
    const double dt = 1.0 / 90.0, kD = nf::kAdsDeg;
    nf::AdsAimMap map;
    nf::AdsRamp ramp;
    int state = f91::kStateIdle, mode = f92::kAimOff;
    double lastAsk = 0.0, look = 0.0, pubPrev = 0.0, headPrev = headAt(0);
    std::vector<Frame> out;
    for (int i = 1; i <= n; ++i) {
        Frame f;
        f.head = headAt(i);
        const double applied = refuse(i) ? 0.0 : lastAsk;   // the game applies last tick's ask in this frame's update
        look += applied;
        // adsAimTick's order: the press's edge, the weight, the head aim's edge, the map, then the head aim's unwind end and the release's end
        const int next = f91::adsStateAfter(state, ads(i));
        const bool entered = next == f91::kStateEngaged && state != f91::kStateEngaged;
        if (entered) map.beginPress();
        if (state == f91::kStateEngaged && next == f91::kStateReleasing) map.beginRelease();
        state = next;
        const double step = (f.head - headPrev) * kD;
        headPrev = f.head;
        if (state == f91::kStateIdle) {   // adsAimTick returns before the map
            mode = f92::kAimOff;
            lastAsk = 0.0;
        } else {
            ramp.step(state == f91::kStateEngaged, dt);
            const f92::AimEdge e = f92::aimEdge(mode, state, entered, key(i));
            if (e.engage && !entered) map.beginPress();
            if (e.unwindBegin) map.beginRelease();
            mode = e.mode;
            const nf::AdsAimMap::Out mo = map.step(f92::mapEngaged(state, mode), f92::mapReleasing(state, mode), dt, ramp.w, true, step, 0.0, applied * kD, 0.0, true, true);
            lastAsk = mo.injYaw / kD;
            if (f92::aimUnwound(state, mode, map.releaseDone())) {   // the remainder dropped and the queued input cleared, as adsAimTick does
                map.dropSeated();
                mode = f92::kAimOff;
                lastAsk = 0.0;
            }
            if (state == f91::kStateReleasing && ramp.w <= 0.0 && map.releaseDone()) {
                map.dropSeated();
                state = f91::kStateIdle;
                mode = f92::kAimOff;
                ramp = nf::AdsRamp{};
                lastAsk = 0.0;
            }
        }
        f.look = look;
        f.pose = pubPrev;   // the pose of this frame carries what was published last tick
        f.r = look - pubPrev;
        f.weaponEye = pubPrev - f.head;
        f.ask = lastAsk;
        f.w = state == f91::kStateIdle ? 0.0 : ramp.w;
        f.share = edvr::nofoot::f106::share(true, true, true, f.w).weight;
        f.state = state;
        f.mode = mode;
        pubPrev = (published ? map.yawPublished() : map.yawSeat) / kD;
        out.push_back(f);
    }
    return out;
}
inline double rmsOf(const std::vector<double>& x) {
    double s = 0.0;
    for (double v : x) s += v * v;
    return x.empty() ? 0.0 : std::sqrt(s / x.size());
}

template <class Check>
inline void run92(Check&& check) {
    // ---- the published cancel: tremor with the head still ----
    {
        std::vector<double> tremor(400, 0.0);
        Lcg rng;
        double u = rng.next();
        for (size_t i = 1; i < tremor.size(); ++i) {   // a still head's tremor: about 0.01 deg a frame, zero mean
            const double v = rng.next();
            tremor[i] = tremor[i - 1] + 0.012 * (v - u);
            u = v;
        }
        const auto headAt = [&](int i) { return tremor[static_cast<size_t>(i < 399 ? i : 399)]; };
        const auto on = [](int) { return true; };
        const auto never = [](int) { return false; };
        const std::vector<Frame> before = runPlant(390, on, on, headAt, never, false), after = runPlant(390, on, on, headAt, never, true);
        std::vector<double> rB, rA;
        for (size_t i = 30; i < before.size(); ++i) {   // past the press's first frames (the weight's 0.15 s ramp)
            rB.push_back(before[i].r);
            rA.push_back(after[i].r);
        }
        // before F92b: r is the game's last step (the head's step a frame back) and the weapon is off the eye by the last two frames of the head's motion; after: r is 0
        // and the weapon is off by the last frame's (the game's own latency). The weapon's offsets are taken against the first compared frame's (the ramp's deficit).
        bool rIsLastStep = true;
        for (size_t i = 30; i < before.size(); ++i)
            rIsLastStep = rIsLastStep && std::fabs(before[i].r - (before[i - 1].head - before[i - 2].head)) < 1e-9;
        const double c0 = before[30].weaponEye + (before[30].head - before[28].head), c1 = after[30].weaponEye + (after[30].head - after[29].head);
        double eyeB = 0.0, eyeA = 0.0;
        for (size_t i = 30; i < before.size(); ++i) {
            eyeB += std::pow(before[i].weaponEye + (before[i].head - before[i - 2].head) - c0, 2.0);   // -(h(F) + h(F-1))
            eyeA += std::pow(after[i].weaponEye + (after[i].head - after[i - 1].head) - c1, 2.0);      // -h(F)
        }
        check(rIsLastStep && rmsOf(rB) > 0.005 && rmsOf(rA) < 1e-9 && eyeB < 1e-12 && eyeA < 1e-12,
              "F92b: with the head still, the cancel published once the game's turn is seen leaves the view off the head by the game's last step (the F91 flight's "
              "r = applied a frame back) and the weapon two frames of tremor off the eye; published with the ask the view is the head's exactly and the weapon one frame off");
    }
    // ---- deliberate turns still followed 1:1, a refused ask shows for one frame only ----
    {
        const auto headAt = [](int i) { return i < 60 ? 0.0 : 0.15 * (i - 60) + 0.05 * std::sin(i * 0.3); };   // a 13.5 deg/s turn from frame 60
        const auto on = [](int) { return true; };
        const auto never = [](int) { return false; };
        const std::vector<Frame> a = runPlant(200, on, on, headAt, never, true), b = runPlant(200, on, on, headAt, never, false);
        bool follows = true, same = true, flat = true;
        for (size_t i = 20; i < a.size(); ++i) {
            follows = follows && std::fabs((a[i].look - a[19].look) - (a[i - 1].head - a[18].head)) < 1e-9;   // the aim is the head's, a frame of the game's latency
            same = same && std::fabs(a[i].look - b[i].look) < 1e-12 && std::fabs(a[i].ask - b[i].ask) < 1e-12;   // the asks and the aim did not change
            flat = flat && std::fabs(a[i].r) < 1e-9;
        }
        const auto refuse = [](int i) { return i == 120; };
        const std::vector<Frame> c = runPlant(200, on, on, headAt, refuse, true);
        bool once = true;
        for (size_t i = 20; i < c.size(); ++i) {
            const int frame = static_cast<int>(i) + 1;
            const bool expect = frame == 120;   // the frame whose ask was refused is off by that ask, and only it
            once = once && (expect ? std::fabs(c[i].r + c[i - 1].ask) < 1e-9 && std::fabs(c[i].r) > 0.1 : std::fabs(c[i].r) < 1e-9);
        }
        check(follows && same && flat && once,
              "F92b: a deliberate 13.5 deg/s turn is still followed 1:1 (the same asks and aim as before, a frame of the game's latency), the view stays the head's, and an ask "
              "the game refuses offsets the view for that one frame only");
    }
    // ---- the toggle within a press ----
    {
        Lcg rng;
        std::vector<double> head(800, 0.0);
        for (size_t i = 1; i < head.size(); ++i) head[i] = head[i - 1] + (i > 50 && i < 200 ? 0.12 : 0.0) + 0.01 * rng.next();   // a turn, then still
        const auto headAt = [&](int i) { return head[static_cast<size_t>(i)]; };
        const auto ads = [](int i) { return i >= 10 && i < 700; };
        const auto key = [](int i) { return !(i >= 300 && i < 500); };   // off at frame 300, on again at 500
        const auto never = [](int) { return false; };
        const auto onAlways = [](int) { return true; };
        const auto offAlways = [](int) { return false; };
        const std::vector<Frame> t = runPlant(790, key, ads, headAt, never, true);
        const std::vector<Frame> on = runPlant(790, onAlways, ads, headAt, never, true), off = runPlant(790, offAlways, ads, headAt, never, true);
        // off mid-press: the press stays engaged with its weight and the left eye; the view stays the head's; what was carried unwinds to 0 and the mode reaches off
        bool engaged = true, viewHead = true, eyeSame = true;
        int offAt = -1;
        double carriedAt300 = 0.0;
        for (size_t i = 0; i < t.size(); ++i) {
            const int frame = static_cast<int>(i) + 1;
            if (frame >= 15 && frame < 700) engaged = engaged && t[i].state == f91::kStateEngaged;
            viewHead = viewHead && std::fabs(t[i].r) < 1e-9;
            eyeSame = eyeSame && t[i].w == on[i].w && t[i].share == on[i].share;
            if (frame == 299) carriedAt300 = t[i].look;
            if (offAt < 0 && frame > 300 && t[i].mode == f92::kAimOff) offAt = frame;
        }
        const size_t i450 = 449, i499 = 498;
        const bool unwound = offAt > 300 && offAt < 300 + 60 && std::fabs(carriedAt300) > 5.0 && std::fabs(t[i450].pose) < 1e-6 && std::fabs(t[i499].ask) < 1e-12;
        // on again at 500: a fresh head aim from the current head: the aim follows the head's steps from there, the view still the head's
        bool fresh = t[499].mode == f92::kAimOn;
        for (size_t i = 510; i < 690; ++i) fresh = fresh && std::fabs((t[i].look - t[i - 1].look) - (t[i - 1].head - t[i - 2].head)) < 1e-9;
        // the press's end unwinds the rest and goes idle
        const bool idle = t.back().state == f91::kStateIdle && std::fabs(t.back().pose) < 1e-3;
        check(engaged && viewHead && eyeSame && unwound && fresh && idle,
              "F92b: the head aim turned off mid-press unwinds what it carried (to 0 within 0.67 s) with the view the head's throughout and the press, its weight and the "
              "left eye as with it on; turned on again it follows the head from there as a fresh press; the release unwinds the rest");
        // off for the whole press: nothing asked, nothing published, the press's weight and the left eye exactly as with it on
        bool silent = true;
        for (size_t i = 0; i < off.size(); ++i) {
            silent = silent && off[i].ask == 0.0 && off[i].pose == 0.0 && off[i].look == 0.0 && off[i].w == on[i].w && off[i].share == on[i].share;
            if (i < 700) silent = silent && off[i].state == on[i].state;   // (the release's end waits for the unwind with it on: a frame later)
        }
        check(silent, "F92b: with the head aim off the press asks the game nothing and publishes nothing, and its weight, the left eye and its state are those of a press with it on");
    }
    // ---- the head aim's edges ----
    {
        using namespace f92;
        const int I = f91::kStateIdle, E = f91::kStateEngaged, R = f91::kStateReleasing;
        const AimEdge a = aimEdge(kAimOff, E, true, true), b = aimEdge(kAimOff, E, true, false), c = aimEdge(kAimOn, E, false, false), d = aimEdge(kAimUnwinding, E, false, true),
                      e = aimEdge(kAimOff, E, false, true), f = aimEdge(kAimOn, R, false, true), g = aimEdge(kAimUnwinding, I, false, false), h = aimEdge(kAimOn, E, false, true);
        check(a.mode == kAimOn && a.engage && !a.unwindBegin && b.mode == kAimOff && !b.engage && c.mode == kAimUnwinding && c.unwindBegin && d.mode == kAimOn && d.engage &&
                  e.mode == kAimOn && e.engage && f.mode == kAimOff && !f.engage && g.mode == kAimOff && h.mode == kAimOn && !h.engage && !h.unwindBegin &&
                  mapEngaged(E, kAimOn) && !mapEngaged(E, kAimUnwinding) && mapReleasing(E, kAimUnwinding) && mapReleasing(R, kAimOff) && !mapReleasing(E, kAimOff) &&
                  aimUnwound(E, kAimUnwinding, true) && !aimUnwound(E, kAimUnwinding, false) && !aimUnwound(R, kAimUnwinding, true),
              "F92b: the head aim's edges: it engages with a press when on, unwinds when turned off engaged, engages fresh when turned on (off or unwinding), and the release owns "
              "the rest");
    }
}

}  // namespace f92test
