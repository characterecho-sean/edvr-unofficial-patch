// Pure-function rig for the DLSS quality-mode selection in src\d3d11\dlaa.h
// (DlssModeRange / dlssModeByRatio / dlssChooseMode) -- no NGX SDK, no
// device, no D3D11: dlaa.h only forward-declares those types and this
// rig never dereferences them. See docs/anti-aliasing.md, "DLSS mode
// selection hardened (2026-09-23)".
//
// Fixture ranges below follow the field's own rule (that doc's entry and
// the flight log it cites, edvr_gfx_20260923_092848.log line 658, plus
// Sean's report of an exact-half feature at 3070x3032): a mode's minimum
// input size is its own optimal render size, and its maximum is the
// output -- so the four modes' ranges stack, the lowest reaching
// furthest down. Quality and Balanced's minimums are not evidenced in
// either flight; buildLadder's fractions for them are plausible filler
// only, so a complete ladder exists to select among -- no CHECK below
// depends on their exact numbers, only on Performance's and Ultra
// Performance's, which match the field exactly (see the CHECKs just
// after buildLadder).
#include "../../src/d3d11/dlaa.h"
#include "../../src/d3d11/dlss_floor.h"  // the served floor's rule, SDK-free the same way

#include <cstdio>
#include <cstring>

using namespace edvr;

unsigned checks = 0, failures = 0;
#define CHECK(value) \
    do { \
        ++checks; \
        if (!(value)) { \
            ++failures; \
            std::printf("FAIL: line %d: %s\n", __LINE__, #value); \
        } \
    } while (false)

namespace {

// A complete, self-consistent 4-mode ladder for one output size: min =
// optimal, max = the output, on every mode -- exactly the shape the
// evidence describes.
void buildLadder(unsigned outW, unsigned outH, DlssModeRange modes[kDlssModeCount]) {
    const float fractions[kDlssModeCount] = {0.667f, 0.58f, 0.5f, 1.0f / 3.0f};
    for (int k = 0; k < kDlssModeCount; ++k) {
        DlssModeRange& m = modes[k];
        m = DlssModeRange{};
        m.ok = true;
        m.optW = m.minW = static_cast<unsigned>(outW * fractions[k]);
        m.optH = m.minH = static_cast<unsigned>(outH * fractions[k]);
        m.maxW = outW;
        m.maxH = outH;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (!std::strcmp(argv[1], "--dry-run")) {
        std::puts("dlaa_mode_test: dry-run (no device or files)");
        return 0;
    }
    if (std::strcmp(argv[1], "--self-test")) return 2;

    // --- dlssModeByRatio: build point 4 -- the one ratio rule, shared by
    // ensureFeature's range-unknown fallback and evaluateCrop. ---
    CHECK(dlssModeByRatio(665, 1000) == DlssMode::Quality);         // 0.665 + eps = 0.667 exactly
    CHECK(dlssModeByRatio(664, 1000) == DlssMode::Balanced);        // one unit lower stays under 0.667
    CHECK(dlssModeByRatio(580, 1000) == DlssMode::Balanced);        // 0.58 + eps clears the balanced floor
    CHECK(dlssModeByRatio(500, 1000) == DlssMode::Performance);     // exact half -> Performance, not
                                                                      // Ultra Performance (the 2026-09-05
                                                                      // review, F1)
    CHECK(dlssModeByRatio(497, 1000) == DlssMode::UltraPerformance);
    CHECK(dlssModeByRatio(1, 1000) == DlssMode::UltraPerformance);
    CHECK(dlssModeByRatio(500, 0) == DlssMode::UltraPerformance);   // no output: no divide by zero

    // --- The two outputs the flight log and Sean's report both named. ---
    DlssModeRange out3070[kDlssModeCount];
    buildLadder(3070, 3032, out3070);
    CHECK(out3070[2].minW == 1535 && out3070[2].minH == 1516);  // performance, Sean's report exactly
    CHECK(out3070[3].minW == 1023 && out3070[3].minH == 1010);  // ultra performance ("one third")

    DlssModeRange out2458[kDlssModeCount];
    buildLadder(2458, 2824, out2458);
    CHECK(out2458[2].minW == 1229 && out2458[2].minH == 1412);  // performance, the flight's own number

    // --- Build point 3: exact-at-minimum stays in that mode; one pixel
    // short on either axis drops to the next mode down rather than
    // refusing (the ranges overlap: every mode's max is the output); the
    // true floor -- below even Ultra Performance -- is still a genuine
    // refusal. Six sizes, both outputs, as specified. ---
    DlssMode picked;
    bool fromRange;
    DlssModeRange range;

    // 3070x3032 output.
    CHECK(dlssChooseMode(out3070, 1535, 1516, 3070, &picked, &fromRange, &range) &&
          picked == DlssMode::Performance && fromRange);                     // at the minimum
    CHECK(dlssChooseMode(out3070, 1534, 1516, 3070, &picked, &fromRange, &range) &&
          picked == DlssMode::UltraPerformance && fromRange);                // 1px short on w only
    CHECK(dlssChooseMode(out3070, 1229, 1412, 3070, &picked, &fromRange, &range) &&
          picked == DlssMode::UltraPerformance && fromRange);
    // ^ the flight's own refused input: ultra performance SHOULD hold it
    // at this output (comment in the evidence), and with the fixed
    // selection it does.
    CHECK(dlssChooseMode(out3070, 1228, 1411, 3070, &picked, &fromRange, &range) &&
          picked == DlssMode::UltraPerformance && fromRange);
    CHECK(dlssChooseMode(out3070, 1023, 1010, 3070, &picked, &fromRange, &range) &&
          picked == DlssMode::UltraPerformance && fromRange);                // at the floor's own minimum
    CHECK(!dlssChooseMode(out3070, 1022, 1010, 3070, &picked, &fromRange, &range));
    // ^ 1px short of the LOWEST mode's minimum, every query "succeeded":
    // a real refusal, not a fall-through (build point 2's second case).

    // 2458x2824 output (the flight's second frame, 54 ms later).
    CHECK(dlssChooseMode(out2458, 1535, 1516, 2458, &picked, &fromRange, &range) &&
          picked == DlssMode::Performance && fromRange);
    CHECK(dlssChooseMode(out2458, 1534, 1516, 2458, &picked, &fromRange, &range) &&
          picked == DlssMode::Performance && fromRange);
    CHECK(dlssChooseMode(out2458, 1229, 1412, 2458, &picked, &fromRange, &range) &&
          picked == DlssMode::Performance && fromRange);                     // exact half; Sean's report
    CHECK(dlssChooseMode(out2458, 1228, 1411, 2458, &picked, &fromRange, &range) &&
          picked == DlssMode::UltraPerformance && fromRange);
    // ^ 1px short on BOTH axes at once (this output's minimum is 1229x1412
    // squarely) -> next mode down, not a refusal.
    CHECK(dlssChooseMode(out2458, 1023, 1010, 2458, &picked, &fromRange, &range) &&
          picked == DlssMode::UltraPerformance && fromRange);
    CHECK(dlssChooseMode(out2458, 1022, 1010, 2458, &picked, &fromRange, &range) &&
          picked == DlssMode::UltraPerformance && fromRange);

    // --- Build points 1/2: a hole in the middle of the ladder (one
    // query failed) must not hide a mode further down that would have
    // held the input -- the flight's own hypothesis for why 1229x1412
    // against 3070x3032 was refused when ultra performance should have
    // held it. ensureFeature's query loop no longer breaks on a failure
    // (it always gathers all four); this is the selection's side of that
    // fix, on data with a hole already in it, which is what the walk now
    // hands the selection. ---
    DlssModeRange holed[kDlssModeCount];
    buildLadder(3070, 3032, holed);
    holed[1].ok = false;  // balanced's query failed; performance and ultra performance still answered
    CHECK(dlssChooseMode(holed, 1229, 1412, 3070, &picked, &fromRange, &range) &&
          picked == DlssMode::UltraPerformance && fromRange);
    holed[0].ok = false;  // quality fails too -- still reaches ultra performance
    CHECK(dlssChooseMode(holed, 1229, 1412, 3070, &picked, &fromRange, &range) &&
          picked == DlssMode::UltraPerformance && fromRange);

    // --- Build point 2's middle case: no mode's range holds the input,
    // but a query failed -- the ladder is incomplete, not a real
    // refusal. Guess by ratio (the same helper as above) and let NGX's
    // own create call decide; do not fabricate a range for the guess. ---
    DlssModeRange allFailed[kDlssModeCount] = {};  // every .ok defaults to false
    CHECK(dlssChooseMode(allFailed, 500, 500, 1000, &picked, &fromRange, &range) &&
          !fromRange && picked == DlssMode::Performance && picked == dlssModeByRatio(500, 1000));
    CHECK(!range.ok && range.minW == 0 && range.maxW == 0);  // no fabricated range on a guess

    // A mix: one mode answered but the input undercuts even that one,
    // and a different mode's query failed -- still a ratio guess, not a
    // refusal, because the picture is incomplete.
    DlssModeRange partial[kDlssModeCount];
    buildLadder(1000, 1000, partial);
    partial[3].ok = false;  // ultra performance's own query failed
    CHECK(dlssChooseMode(partial, 100, 100, 1000, &picked, &fromRange, &range) &&
          !fromRange && picked == dlssModeByRatio(100, 1000));

    // --- Every query succeeds and every mode is held over the input:
    // dlssChooseMode still picks the NEAREST optimal among the modes that
    // hold it, not just the first. ---
    DlssModeRange out1000[kDlssModeCount];
    buildLadder(1000, 1000, out1000);
    CHECK(dlssChooseMode(out1000, 1000, 1000, 1000, &picked, &fromRange, &range) &&
          picked == DlssMode::Quality && fromRange);  // held by all four; quality's optimal is nearest

    // --- The served floor (dlss_floor.h, 2026-09-23). The first modes line
    // a flight printed (edvr_gfx_20260923_153446.log line 435) is not the
    // stacked ladder above: the three upper modes share a floor at half the
    // output, and ultra performance's range is a single point at a third.
    //   dlss: modes for 4074x4076: quality 2716x2717 (2037x2038..4074x4076),
    //   balanced 2363x2364 (2037x2038..4074x4076), performance 2037x2038
    //   (2037x2038..4074x4076), ultra performance 1358x1359
    //   (1358x1359..1358x1359)
    // Every input between a third and a half, and under a third, is served
    // by no mode; the rule cuts the output to twice the input instead. ---
    const auto flightLadder = [](unsigned outW, unsigned outH, DlssModeRange m[kDlssModeCount]) {
        const unsigned hw = (outW + 1) / 2, hh = (outH + 1) / 2;
        const unsigned optW[3] = {(outW * 2 + 1) / 3, (outW * 58 + 50) / 100, hw};
        const unsigned optH[3] = {(outH * 2 + 1) / 3, (outH * 58 + 50) / 100, hh};
        for (int k = 0; k < 3; ++k) {
            m[k] = DlssModeRange{};
            m[k].ok = true;
            m[k].optW = optW[k]; m[k].optH = optH[k];
            m[k].minW = hw; m[k].minH = hh;
            m[k].maxW = outW; m[k].maxH = outH;
        }
        m[3] = DlssModeRange{};
        m[3].ok = true;
        m[3].optW = m[3].minW = m[3].maxW = (outW + 1) / 3;
        m[3].optH = m[3].minH = m[3].maxH = (outH + 1) / 3;
    };
    DlssModeRange pimax[kDlssModeCount];
    flightLadder(4074, 4076, pimax);
    // The fixture IS the logged line, number for number.
    CHECK(pimax[0].optW == 2716 && pimax[0].optH == 2717 && pimax[1].optW == 2363 &&
          pimax[1].optH == 2364 && pimax[2].optW == 2037 && pimax[2].optH == 2038);
    CHECK(pimax[0].minW == 2037 && pimax[0].minH == 2038 && pimax[1].minW == 2037 &&
          pimax[2].maxW == 4074 && pimax[2].maxH == 4076);
    CHECK(pimax[3].minW == 1358 && pimax[3].minH == 1359 && pimax[3].maxW == 1358 &&
          pimax[3].maxH == 1359);
    uint32_t fw = 0, fh = 0;
    CHECK(dlssRangeFloor(pimax, &fw, &fh) && fw == 2037 && fh == 2038);  // the point is no floor
    CHECK(!dlssRangeIsRange(pimax[3]) && dlssRangeIsRange(pimax[2]));
    uint32_t ow = 0, oh = 0;
    int m = -2;
    const auto rule = [&](const DlssModeRange* modes, uint32_t dw, uint32_t dh, uint32_t w,
                          uint32_t h) { return dlssFloorOutput(modes, dw, dh, w, h, &ow, &oh, &m); };
    // HMD Quality 0.5 sits exactly on the floor (line 436 created there).
    CHECK(rule(pimax, 4074, 4076, 2037, 2038) && ow == 4074 && oh == 4076 && m == -1);
    CHECK(dlssChooseMode(pimax, 2037, 2038, 4074, &picked, &fromRange, &range) &&
          picked == DlssMode::Performance && fromRange);
    // One pixel under, on both axes: the selection refuses, the rule halves.
    CHECK(!dlssChooseMode(pimax, 2036, 2037, 4074, &picked, &fromRange, &range));
    CHECK(rule(pimax, 4074, 4076, 2036, 2037) && ow == 4072 && oh == 4074 && m >= 0 && m <= 2);
    // HMD Quality 0.45 (1833x1834): 3666x3668.
    CHECK(!dlssRangesServe(pimax, 1833, 1834));
    CHECK(rule(pimax, 4074, 4076, 1833, 1834) && ow == 3666 && oh == 3668);
    // Ultra performance's own point is served as it stands; a pixel off it
    // is not, and is cut like any other input under the floor.
    CHECK(rule(pimax, 4074, 4076, 1358, 1359) && ow == 4074 && oh == 4076 && m == -1);
    CHECK(dlssChooseMode(pimax, 1358, 1359, 4074, &picked, &fromRange, &range) &&
          picked == DlssMode::UltraPerformance);
    CHECK(rule(pimax, 4074, 4076, 1358, 1358) && ow == 2716 && oh == 2716);
    CHECK(rule(pimax, 4074, 4076, 1300, 1300) && ow == 2600 && oh == 2600);
    // Each cut output is served by NGX's own rule at that output.
    {
        const unsigned cuts[4][4] = {{4072, 4074, 2036, 2037}, {3666, 3668, 1833, 1834},
                                     {2716, 2716, 1358, 1358}, {2600, 2600, 1300, 1300}};
        for (const auto& c : cuts) {
            DlssModeRange at[kDlssModeCount];
            flightLadder(c[0], c[1], at);
            CHECK(dlssRangesServe(at, c[2], c[3]) &&
                  dlssChooseMode(at, c[2], c[3], c[0], &picked, &fromRange, &range) &&
                  picked == DlssMode::Performance && fromRange);
        }
    }
    // The FOV-trim session's untrimmed 3070x3032 (edvr_gfx_20260923_092848:
    // performance 1535x1516..3070x3032), and the trim's two-step adoption:
    // 1229x1412 against the still-untrimmed door, refused at 09:29:10, cut
    // to 2458x2824 -- exactly the trimmed door the promotion then hands.
    DlssModeRange trimDoor[kDlssModeCount];
    flightLadder(3070, 3032, trimDoor);
    CHECK(trimDoor[2].minW == 1535 && trimDoor[2].minH == 1516);
    CHECK(rule(trimDoor, 3070, 3032, 1535, 1516) && ow == 3070 && oh == 3032 && m == -1);
    CHECK(rule(trimDoor, 3070, 3032, 1534, 1515) && ow == 3068 && oh == 3030);
    CHECK(rule(trimDoor, 3070, 3032, 1229, 1412) && ow == 2458 && oh == 2824);
    CHECK(rule(trimDoor, 3070, 3032, 1229, 1213) && ow == 2458 && oh == 2426);
    DlssModeRange trimmed[kDlssModeCount];
    flightLadder(2458, 2824, trimmed);
    CHECK(rule(trimmed, 2458, 2824, 1229, 1412) && ow == 2458 && oh == 2824 && m == -1);
    // Even rounding: an odd door is cut to an even output; an axis the input
    // already serves keeps the door's own size, odd or not.
    DlssModeRange odd[kDlssModeCount];
    flightLadder(4075, 4077, odd);
    CHECK(odd[2].minW == 2038 && odd[2].minH == 2039);
    CHECK(rule(odd, 4075, 4077, 2036, 2037) && ow == 4070 && oh == 4072 &&
          (ow & 1u) == 0 && (oh & 1u) == 0);
    CHECK(rule(odd, 4075, 4077, 2040, 2037) && ow == 4075 && oh == 4072);
    CHECK(dlssFloorAxis(4075, 2038, 2036) == 4070 && dlssFloorAxis(3, 2, 1) == 0 &&
          dlssFloorAxis(4074, 2037, 1) == 2 && dlssFloorAxis(4074, 2037, 2037) == 4074 &&
          dlssFloorAxis(0, 2037, 1000) == 0 && dlssFloorAxis(4074, 2037, 0) == 0);
    // Nothing known to stand on: no answered query, or only the point --
    // the door's output stands and the selection decides, as before.
    DlssModeRange none[kDlssModeCount];
    CHECK(!rule(none, 4074, 4076, 1833, 1834) && ow == 4074 && oh == 4076);
    DlssModeRange pointOnly[kDlssModeCount];
    flightLadder(4074, 4076, pointOnly);
    pointOnly[0].ok = pointOnly[1].ok = pointOnly[2].ok = false;
    CHECK(!dlssRangeFloor(pointOnly, &fw, &fh));
    CHECK(!rule(pointOnly, 4074, 4076, 1833, 1834) && ow == 4074 && oh == 4076);
    CHECK(!rule(pimax, 0, 4076, 1833, 1834) && !rule(pimax, 4074, 4076, 0, 1834));

    std::printf("dlaa_mode_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
