#pragma once
#include "../../src/d3d11/native_on_foot_f125containment.h"
#include "../../src/d3d11/native_on_foot_f123.h"
#include <cstring>
namespace f125containmenttest {
template <class Check> inline void run(Check&& check) {
    namespace h = edvr::nofoot::f119;
    h::HeadWindow w;
    for (int second = 0; second < 10; ++second) {
        h::OffsetSecond off;
        h::OwnSecond own;
        h::DepthSecond depth;
        off.frames = 180;
        for (int cls = 0; cls < h::kClasses; ++cls) {
            if (cls == 0 && second >= 5) { off.noPose += 10; continue; }
            h::Offset sample;
            sample.right = 0.01 * (cls + 1);
            for (int n = 0; n < 10; ++n) off.add(cls, sample);
        }
        depth.eye[0].read = second < 7 ? 90 : 0;
        depth.eye[1].read = (second & 1) ? 0 : 90;
        depth.baseFrames = 80;
        depth.baseCells20 = second == 3 ? 2 : 0;
        w.fold(off, own, depth, 180.0, 180);
    }
    check(w.classes() == 18 && w.seconds == 10 && w.frames == 1800 && w.cls[0].n == 50 && w.classSeconds[0] == 5 && w.cls[1].n == 100,
          "F125 containment: ten folds retain all 18 observed classes, beyond the old 512-sample list, with each class's own duration");
    uint32_t total = 0, hist = 0;
    for (const auto& c : w.cls) { total += c.n; for (uint32_t n : c.hist) hist += n; }
    check(total == 1750 && hist == total && w.noPose == 50,
          "F125 containment: every sample is present exactly once in its class and the full 17-bin histograms");
    check(w.depthSeconds[0] == 7 && w.depthSeconds[1] == 5 && w.depthAny == 8 && w.depth.baseFrames == 800 && w.depth.baseCells20 == 2,
          "F125 containment: depth-readable seconds are flags per fold, while frame counts sum and coverage takes its maximum");
    char line[1600];
    int count = h::formatHeadWindow(line, sizeof(line), w, 0, 7, 0, 10.0, 1, 1, false);
    check(count > 0 && count <= h::kHeadWindowLineLimit && std::strstr(line, "class s 5; window 1; epoch 1; steady") && std::strstr(line, "hist "),
          "F125 containment: each observed class line carries its full histogram, duration and common-window id within 1090 bytes");
    h::HeadWindow stress = w;
    for (auto& c : stress.cls) {
        c.n = 5120; c.max = 1000.0;
        c.rMin = c.fMin = c.uMin = -1000.0; c.rMax = c.fMax = c.uMax = 1000.0;
        for (int bin = 0; bin < h::kBins; ++bin) c.hist[bin] = 301;
        c.hist[0] += 3;
    }
    stress.own.n = 5120; stress.own.hMax = 10.0; stress.own.upMin = -10.0; stress.own.upMax = 10.0;
    stress.own.lockH = stress.own.weaponH = stress.own.viewH = 10.0; stress.own.neckUp = stress.own.holdUp = 10.0;
    for (auto& e : stress.depth.eye) { e.read = 5120; e.nearM = 0.12; e.nearestWorld = e.nearestAll = 10.0; for (auto& n : e.within) n = 64; e.all20 = 64; }
    for (auto& n : stress.depth.skipped) n = 5120;
    count = h::formatHeadWindow(line, sizeof(line), stress, 0, 1000000, 1000000, 1000000.0, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, true);
    std::printf("F125 containment dense line: %d bytes\n", count);
    check(count > 0 && count <= h::kHeadWindowLineLimit && static_cast<size_t>(count) == std::strlen(line) && std::strstr(line, "not a depth fit 5120"),
          "F125 containment: a dense ten-second stress line with full histograms and large finite ranges stays whole under the 1090-byte limit");
    w.clear();
    check(w.seconds == 0 && w.classes() == 0 && w.depthAny == 0, "F125 containment: close/exit/recentre clears only the measurement window");
    h::OffsetSecond one; h::Offset pose; pose.right = 0.2; one.frames = 1; one.add(0, pose);
    h::OwnSecond own; h::DepthSecond depth;
    w.fold(one, own, depth, 10.0, 1);
    h::formatHeadWindow(line, sizeof(line), w, 0, 8, 1, 0.1, 2, 2, false);
    check(w.seconds == 1 && w.cls[0].n == 1 && std::strstr(line, "class s 1; window 2"),
          "F125 containment: a short flushed new-generation window starts fresh and uses the caller's next monotonically increasing id");
    edvr::nofoot::logwin::PanelWindow panel;
    panel.state(false, true, false);
    panel.take(3, 3, 0, false);
    check(panel.changed(true, true, false) && !panel.key && panel.seconds == 1 && panel.seen == 3 && panel.hidden == 3,
          "F125 panel: a change before the first fold drains partial counters under the old off setting");
    panel.clear(); panel.state(true, true, false);
    for (int i = 0; i < 10; ++i) panel.take(i < 3 ? 90 : 0, 0, 0, true);
    check(panel.key && panel.seconds == 10 && panel.active == 3 && panel.seen == 270,
          "F125 panel: ten functional folds retain active seconds instead of treating every summary row as one active second");
    panel.clear();
    check(panel.known && panel.key && panel.changed(false, true, false),
          "F125 panel: resetting reporting sums keeps the primed key so an edge before the next fold cannot relabel old counts");
    h::RestContext rest;
    rest.observe(7, 1);
    rest.pending(2);
    h::formatHeadWindow(line, sizeof(line), w, 0, rest.reference, 0, 0.1, 3, rest.epoch, rest.mixed);
    check(rest.reference == 7 && rest.epoch == 2 && rest.mixed && std::strstr(line, "rest 7 age") && std::strstr(line, "epoch 2; mixed rest"),
          "F125 containment: an ask starts an explicitly mixed-rest cohort under the unchanged real reference generation");
    rest.settled(3);
    check(rest.reference == 7 && rest.epoch == 3 && !rest.mixed,
          "F125 containment: the geometry-based done result starts a clean report epoch without inventing a runtime generation change");
    rest.pending(4);
    rest.settled(5);
    check(rest.reference == 7 && rest.epoch == 5 && !rest.mixed,
          "F125 containment: a failed/retried ask also ends its mixed cohort without claiming that the runtime changed reference");
    rest.pending(6);
    edvr::nofoot::f123::Machine machine;
    edvr::nofoot::f123::In input;
    input.active = input.haveOffset = true; input.hCm = edvr::nofoot::f123::kHardCm + 1.0; input.dt = 0.01;
    bool asked = false;
    for (int i = 0; i < 200 && !asked; ++i) asked = machine.step(input).asked;
    machine.gate(false);
    input.active = false;
    const auto cancelled = machine.step(input);
    check(asked && !cancelled.done && !cancelled.failed && rest.needsCancellation(input.active),
          "F125 containment: the actual F123 camera gate withdraws a pending ask without a done/failed result, so reporting must cancel independently");
    check(!rest.needsCancellation(true) && rest.needsCancellation(false),
          "F125 containment: a pending ask cancelled by the camera gate must close its mixed cohort even without a done/failed output");
    if (rest.needsCancellation(false)) rest.settled(7);
    check(!rest.mixed && !rest.needsCancellation(true) && rest.reference == 7 && rest.epoch == 7,
          "F125 containment: returning to first person after cancellation starts a clean reporting epoch under the actual reference");
    // The feature may forget a stale sample; reporting must remember the cohort through that gap until it is flushed.
    bool featureHasPose = false;
    check(!featureHasPose && rest.known && rest.different(8) && rest.reference == 7,
          "F125 containment: a no-pose gap cannot erase the old reference before a new reference's first valid sample is separated");
    const uint32_t oldReference = rest.reference;
    rest.observe(8, 6);
    check(oldReference == 7 && rest.reference == 8 && rest.epoch == 6,
          "F125 containment: after the old cohort is closed the new reference is recorded independently of feature validity flags");
}
}
