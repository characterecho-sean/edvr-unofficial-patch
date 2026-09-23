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

    std::printf("dlaa_mode_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
