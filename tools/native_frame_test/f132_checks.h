// F132 checks (native on-foot VR): the G frame pairing re-seeds its phase after a far-apart refusal. Header only, run by native_frame_test (run132).
// The flight (edvr_gfx_20261027_064839, from 07:03:13 after a new avatar was created; also 182525 on rc.1-27): the game's order is the local 3P AMC's P1 call then the FP
// AMC's, the machine counted an FP call first, paired FP(n) with 3P(n+1), 11 ms apart, and refused 6927 of 7351 frames for good.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"

#include <cstdint>

namespace f132test {

namespace nf = edvr::nofoot;

// The two callers as native_on_foot.cpp runs them (fpRootTrack and ikPublishG under g_eyeMutex), with the true game frame of each call as a label so the pairs can be
// judged: a pair is right when both halves carry the same label.
struct Driver {
    nf::P1FramePairing p;
    uint64_t freq = 1000000;   // QPC ticks a second: 1 tick a microsecond
    bool fpOk = false, tpHave = false;
    uint32_t fpFrame = 0, tpFrame = 0;
    uint64_t fpQ = 0, tpQ = 0;
    int fpLabel = -1, tpLabel = -1;
    int pairsRight = 0, pairsWrong = 0, calls = 0;
    bool reseedOn = true;   // false: the F68c rule as it was (take() without the caller's kind)
    void fp(uint64_t usec, int label) {
        ++calls;
        fpFrame = p.on(0);
        fpQ = usec;
        fpLabel = label;
        fpOk = true;
        uint32_t after = fpFrame;
        const bool pair = reseedOn ? p.take(true, fpFrame, fpQ, tpHave, tpFrame, tpQ, freq, 0, &after) : p.take(true, fpFrame, fpQ, tpHave, tpFrame, tpQ, freq);
        fpFrame = after;
        if (pair) (fpLabel == tpLabel ? pairsRight : pairsWrong)++;
    }
    void tp(uint64_t usec, int label) {
        ++calls;
        const uint32_t frame = p.on(1);
        tpHave = true;
        tpFrame = frame;
        tpQ = usec;
        tpLabel = label;
        uint32_t after = frame;
        const bool pair = reseedOn ? p.take(fpOk, fpFrame, fpQ, true, frame, usec, freq, 1, &after) : p.take(fpOk, fpFrame, fpQ, true, frame, usec, freq);
        tpFrame = after;
        if (pair) (fpLabel == tpLabel ? pairsRight : pairsWrong)++;
    }
};

template <class Check>
inline void run132(Check&& check) {
    const uint64_t period = 11100;   // 90 Hz
    {   // the game's order is 3P then FP; the machine's first call is an FP call (a new avatar was created in the middle of a frame)
        Driver d;
        d.fp(500, 0);   // FP(0): its 3P call was before the machine counted
        for (int n = 1; n <= 200; ++n) {
            d.tp(static_cast<uint64_t>(n) * period, n);
            d.fp(static_cast<uint64_t>(n) * period + 500, n);
        }
        check(d.p.reseeds == 1 && d.p.farApart == 1 && d.pairsRight == 200 && d.pairsWrong == 0 && d.p.pairs == 200,
              "F132: order 3P,FP with the machine started on an FP call: one pair refused as far apart (10.6 ms), one reseed, and every frame after pairs (200 of 200), none wrong");
        check(d.p.farFpFirst == 1 && d.p.farTpFirst == 0 && d.p.farGapMinMs > 10.0 && d.p.farGapMaxMs < 11.0 && d.p.farGapMinMs == d.p.farGapMaxMs,
              "F132: the refused pair is counted with its gap (10.6 ms) and with the FP call first");
        Driver old;
        old.reseedOn = false;
        old.fp(500, 0);
        for (int n = 1; n <= 200; ++n) {
            old.tp(static_cast<uint64_t>(n) * period, n);
            old.fp(static_cast<uint64_t>(n) * period + 500, n);
        }
        check(old.p.pairs == 0 && old.p.farApart >= 199 && old.pairsRight == 0 && old.p.reseeds == 0,
              "F132: the same calls without the re-seed (the F68c rule): refused on every frame for good, as in the flight (6927 of 7351)");
    }
    {   // the game's order is FP then 3P; the machine's first call is a 3P call
        Driver d;
        d.tp(500, 0);
        for (int n = 1; n <= 200; ++n) {
            d.fp(static_cast<uint64_t>(n) * period, n);
            d.tp(static_cast<uint64_t>(n) * period + 500, n);
        }
        check(d.p.reseeds == 1 && d.pairsRight == 200 && d.pairsWrong == 0 && d.p.farTpFirst == 1,
              "F132: order FP,3P with the machine started on a 3P call: one reseed, then every frame pairs");
    }
    {   // the right order from the start: nothing is refused, nothing is re-seeded
        Driver a, b;
        for (int n = 0; n < 200; ++n) {
            a.tp(static_cast<uint64_t>(n) * period, n);
            a.fp(static_cast<uint64_t>(n) * period + 500, n);
            b.fp(static_cast<uint64_t>(n) * period, n);
            b.tp(static_cast<uint64_t>(n) * period + 500, n);
        }
        check(a.p.reseeds == 0 && a.p.farApart == 0 && a.pairsRight == 200 && b.p.reseeds == 0 && b.p.farApart == 0 && b.pairsRight == 200 && a.pairsWrong == 0 && b.pairsWrong == 0,
              "F132: either true order started on the right call pairs every frame with no refusal and no reseed");
    }
    {   // one 3P call missing mid-run: that frame is lost, the next one pairs again (no shifted phase for good)
        Driver d;
        for (int n = 0; n < 300; ++n) {
            if (n != 100) d.tp(static_cast<uint64_t>(n) * period, n);
            d.fp(static_cast<uint64_t>(n) * period + 500, n);
        }
        check(d.pairsWrong == 0 && d.pairsRight >= 297 && d.p.reseeds <= 1 && d.p.farApart <= 1,
              "F132: one missing 3P call mid-run loses at most the frame it was missing from and the next, never pairs wrong, and the phase is back by the third");
        Driver e;   // and one missing FP call, the other order
        for (int n = 0; n < 300; ++n) {
            e.fp(static_cast<uint64_t>(n) * period, n);
            if (n != 100) e.tp(static_cast<uint64_t>(n) * period + 500, n);
        }
        check(e.pairsWrong == 0 && e.pairsRight >= 297 && e.p.reseeds <= 1, "F132: one missing 3P call with the order FP,3P: the same");
        Driver g;   // three missing calls in a row
        for (int n = 0; n < 300; ++n) {
            if (n < 100 || n > 102) g.tp(static_cast<uint64_t>(n) * period, n);
            g.fp(static_cast<uint64_t>(n) * period + 500, n);
        }
        check(g.pairsWrong == 0 && g.pairsRight >= 294, "F132: a run of missing 3P calls is a run of unpaired frames, and pairing resumes after it");
    }
    {   // a genuinely large gap on every frame: refused every time, each refusal a reseed, no runaway
        Driver d;
        const uint64_t slow = 40000;   // 25 Hz: neither alignment is under 10 ms
        for (int n = 0; n < 500; ++n) {
            d.fp(static_cast<uint64_t>(n) * slow, n);
            d.tp(static_cast<uint64_t>(n) * slow + 15000, n);
        }
        check(d.p.pairs == 0 && d.pairsRight == 0 && d.pairsWrong == 0 && d.p.farApart > 400 && d.p.reseeds == d.p.farApart && d.p.frame <= static_cast<uint32_t>(d.calls) + 2 &&
                  d.p.farGapMinMs >= 10.0,
              "F132: a gap over 10 ms on every frame is refused every time, each refusal re-seeds (reseeds == pairs refused), and the frame index grows with the calls only");
        check(d.p.farGapMaxMs >= 24.0 && d.p.farGapMaxMs <= 26.0 && d.p.farGapMinMs >= 14.0 && d.p.farFpFirst > 0 && d.p.farTpFirst > 0,
              "F132: the refused gaps are counted (min 15 ms, max 25 ms here) with which call was first");
        d.p.clearSecond();
        check(d.p.reseeds == 0 && d.p.farApart == 0 && d.p.farGapMaxMs == 0.0 && d.p.farFpFirst == 0 && d.p.frames == 0,
              "F132: the second's counters clear together (the line takes and clears them)");
    }
    {   // the refused call's frame index comes back to the caller; the legacy call (no kind) is unchanged
        nf::P1FramePairing p;
        p.on(0);
        const uint32_t tf = p.on(1);
        uint32_t nfm = tf;
        const bool paired = p.take(true, tf, 0, true, tf, 300000, 1000000, 1, &nfm);
        check(!paired && nfm == tf + 1 && p.frame == nfm && p.seen[1] && !p.seen[0] && p.firstKind == 1 && !p.published && p.reseeds == 1 && p.noPair == 1,
              "F132: the refused 3P call opens the next frame as its first call and its new index is handed back");
        const uint32_t ff = p.on(0);
        check(ff == nfm && p.fpFirst + p.tpFirst >= 2, "F132: the next FP call joins that frame as its second");
        nf::P1FramePairing legacy;
        legacy.on(0);
        const uint32_t lf = legacy.on(1);
        uint32_t same = lf;
        legacy.take(true, lf, 0, true, lf, 300000, 1000000, -1, &same);
        check(same == lf && legacy.reseeds == 0 && legacy.farApart == 1, "F132: without the caller's kind nothing is re-seeded (the old rule)");
        nf::P1FramePairing missing;   // a missing half is not a far-apart refusal
        missing.on(0);
        const uint32_t mf = missing.on(1);
        uint32_t keep = mf;
        const bool no = missing.take(true, mf, 0, false, mf, 0, 1000000, 1, &keep);
        check(!no && keep == mf && missing.reseeds == 0, "F132: a pair with a half missing is no refusal for distance and re-seeds nothing");
    }
}

}  // namespace f132test
