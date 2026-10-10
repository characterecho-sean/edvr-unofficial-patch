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
#include <fstream>
#include <iterator>
#include <string>

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
// A check with a label, for the cases tools\dlaa_mode_test\mutants.py names: "C<n>.<case>.<what>". The mutation proof requires a mutant to fail a
// check of the case it belongs to (a FAIL label starting "C<n>."), so every check of the ceiling section carries one.
#define CASE(id, value) \
    do { \
        ++checks; \
        if (!(value)) { \
            ++failures; \
            std::printf("FAIL: %s: %s\n", id, #value); \
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

    // --- The upscaler slots (dlaa.h, kUpscalerSlots; docs/design-flat-temporal-aa-2026-09-23.md section 82). The eyes own slots 0 and 1,
    // the VR world route owns slot 2; every slot has the full frame. ---
    static_assert(kUpscalerSlots == 3 && kUpscalerEyeSlots == 2 && kUpscalerEyeSlots < kUpscalerSlots,
                  "two eyes' slots and the VR world route's third");
    for (int slot = -3; slot < 7; ++slot) {
        const bool eye = slot == 0 || slot == 1, world = slot == 2;
        CHECK(upscalerSlotHasFullFrame(slot) == (eye || world));
    }
    CHECK(!upscalerSlotHasFullFrame(3) && !upscalerSlotHasFullFrame(-1) && upscalerSlotHasFullFrame(2));
    // The eyes' log lines are the lines they always were ("for eye 0"); the world's names its slot.
    CHECK(!std::strcmp(upscalerSlotLabel(0), "eye 0") && !std::strcmp(upscalerSlotLabel(1), "eye 1"));
    CHECK(std::strstr(upscalerSlotLabel(2), "slot 2") != nullptr && std::strstr(upscalerSlotLabel(2), "world") != nullptr);
    CHECK(std::strstr(upscalerSlotLabel(3), "unknown") != nullptr && std::strstr(upscalerSlotLabel(-1), "unknown") != nullptr);

    // --- dlaa.cpp's slot wiring, read as source: NGX needs an NVIDIA GPU, so no rig can run it (tools\fsr3_engine_test runs the FSR
    // half for real). The full-frame features are one per slot; every entry gate takes the rules
    // above; the loading-screen warm-up makes the eyes' two and not the world's (its feature is made lazily, on its first evaluation);
    // and the creation lines name the slot through upscalerSlotLabel, which keeps the eyes' words. The rig runs from the repo root. ---
    {
        std::ifstream in("src/d3d11/dlaa.cpp", std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto count = [&](const char* needle) {
            size_t n = 0, at = 0; const std::string s(needle);
            while ((at = src.find(s, at)) != std::string::npos) { ++n; at += s.size(); }
            return n;
        };
        CHECK(!src.empty());
        CHECK(count("EyeFeature g_feature[kUpscalerSlots];") == 1);
        CHECK(count("DlaaRoleStats g_fullStats[kUpscalerSlots];") == 1);
        CHECK(count("if (!upscalerSlotHasFullFrame(eye)) {") == 2);        // ensureFeature, the one place that indexes g_feature, and dlaaEvaluate
        CHECK(count("for (int eye = 0; eye < 2; ++eye) {") == 1);          // dlaaWarm: the eyes' two only
        CHECK(count("eye > 1") == 0 && count("eye == 1") == 0);             // no hard-coded two-eye bound left
        CHECK(count("the feature is created for %s at") == 1 && count("the feature is created for %s, %ux%u in") == 1 &&
              count("the feature for %s was created for the flat HDR route") == 1 && count("upscalerSlotLabel(eye)") == 3);
        CHECK(count("g_fullStats[q.eye]") == 1 && count("upscalerSlotHasFullFrame(q.eye)") == 1);
    }

    // --- The ceiling (dlss_floor.h, 2026-10-09). NGX answers a usable ladder for any output with max(width, height) <= 8192 and, past that, ok = true
    // with every size ZERO for all four modes (the Pimax flight at 8268x3948 printed "quality 0x0 (0x0..0x0)" four times; an NGX probe on an RTX 5090,
    // driver 617.42, measured 8192x8192 usable, 8193 on either axis and 16384x1000 not -- an axis limit, not a pixel count). So `ok` proves nothing; a mode is
    // ANSWERED only when its sizes are there, and an output with no answered mode is cut to the largest one that has one: 8268x3948 -> 8192x3910. Every check
    // carries a label "C<n>.<case>.<what>" (the cases: C1 zero_ladder, C2 answered_ladder, C3 single_point, C4 partial, C5 ceiling_8268, C6 ceiling_tall,
    // C7 ceiling_square, C8 ceiling_odd, C9 ceiling_none, C10 ceiling_verified, C11 ceiling_calls, C12 ceiling_pixel_model, C13 window_mode); the numbers are
    // there because tools\rig_mutants_lib.py reads a case id as a prefix plus digits. ---
    {
        // The answer past the limit: ok, every size zero, on all four modes.
        const auto zeroLadder = [](DlssModeRange ms[kDlssModeCount]) {
            for (int k = 0; k < kDlssModeCount; ++k) {
                ms[k] = DlssModeRange{};
                ms[k].ok = true;
            }
        };
        // NGX as measured: a usable ladder when both axes are within 8192, the zero ladder otherwise.
        const auto perAxis = [&](unsigned qw, unsigned qh, DlssModeRange* ms) {
            if (qw <= 8192 && qh <= 8192) buildLadder(qw, qh, ms); else zeroLadder(ms);
        };
        // NOT NGX: a pixel-count limit, to prove the per-axis model above is what the search followed.
        const auto perPixel = [&](unsigned qw, unsigned qh, DlssModeRange* ms) {
            if (uint64_t(qw) * qh <= 32000000ull) buildLadder(qw, qh, ms); else zeroLadder(ms);
        };
        const auto answeredBy = [](const auto& q, unsigned qw, unsigned qh) {
            DlssModeRange ms[kDlssModeCount];
            q(qw, qh, ms);
            return dlssRangesAnswered(ms);
        };
        // The oracle: the largest even width at or under the door's whose size the model answers, every width tried, no bisection; the height is the
        // door's aspect, rounded down and made even, as the search sizes it.
        const auto bruteCeiling = [](unsigned dw, unsigned dh, const auto& q, unsigned* bw, unsigned* bh) {
            *bw = *bh = 0;
            for (unsigned half = 1; half <= dw / 2; ++half) {
                const unsigned qw = 2 * half;
                const unsigned qh = static_cast<unsigned>((uint64_t(dh) * qw / dw) & ~uint64_t(1));
                if (qh < 2) continue;
                DlssModeRange ms[kDlssModeCount];
                q(qw, qh, ms);
                if (dlssRangesAnswered(ms)) { *bw = qw; *bh = qh; }
            }
            return *bw != 0;
        };

        // C1: the flight's zero ladder is no answer and no floor, and serves nothing.
        DlssModeRange zl[kDlssModeCount];
        zeroLadder(zl);
        uint32_t zfw = 0, zfh = 0, zow = 0, zoh = 0;
        CASE("C1.zero_ladder.not_answered", !dlssRangesAnswered(zl));
        CASE("C1.zero_ladder.no_mode_answered", !dlssModeAnswered(zl[0]) && !dlssModeAnswered(zl[1]) && !dlssModeAnswered(zl[2]) && !dlssModeAnswered(zl[3]));
        CASE("C1.zero_ladder.no_floor", !dlssRangeFloor(zl, &zfw, &zfh) && zfw == 0 && zfh == 0);
        CASE("C1.zero_ladder.no_floor_output", !dlssFloorOutput(zl, 8268, 3948, 4134, 1974, &zow, &zoh));
        CASE("C1.zero_ladder.door_kept", zow == 8268 && zoh == 3948);
        CASE("C1.zero_ladder.serves_nothing", !dlssRangesServe(zl, 4134, 1974) && !dlssRangesServe(zl, 1, 1));

        // C2: a complete ladder is answered, on every mode and as a whole.
        DlssModeRange al[kDlssModeCount];
        buildLadder(4134, 1974, al);
        CASE("C2.answered_ladder.mode_0", dlssModeAnswered(al[0]));
        CASE("C2.answered_ladder.mode_1", dlssModeAnswered(al[1]));
        CASE("C2.answered_ladder.mode_2", dlssModeAnswered(al[2]));
        CASE("C2.answered_ladder.mode_3", dlssModeAnswered(al[3]));
        CASE("C2.answered_ladder.ranges_answered", dlssRangesAnswered(al));

        // C3: one mode answered as a point (ultra performance on the flights) is an answer, and is no floor.
        DlssModeRange sp[kDlssModeCount] = {};
        sp[3].ok = true;
        sp[3].optW = sp[3].minW = sp[3].maxW = 1378;
        sp[3].optH = sp[3].minH = sp[3].maxH = 658;
        uint32_t spw = 0, sph = 0;
        CASE("C3.single_point.mode_answered", dlssModeAnswered(sp[3]));
        CASE("C3.single_point.ranges_answered", dlssRangesAnswered(sp));
        CASE("C3.single_point.is_no_range", !dlssRangeIsRange(sp[3]));
        CASE("C3.single_point.no_floor", !dlssRangeFloor(sp, &spw, &sph));
        CASE("C3.single_point.serves_itself", dlssRangesServe(sp, 1378, 658) && !dlssRangesServe(sp, 1379, 658));

        // C4: a mode that is missing a part of its answer is not answered -- the three shapes the spec names, then each field of a complete mode on its own.
        DlssModeRange pt[kDlssModeCount] = {};
        pt[0].ok = true;  // optimal set, minimum zero
        pt[0].optW = 2756; pt[0].optH = 1316;
        pt[0].maxW = 4134; pt[0].maxH = 1974;
        pt[1].ok = true;  // minimum above maximum
        pt[1].optW = 2067; pt[1].optH = 987;
        pt[1].minW = 4134; pt[1].minH = 1974;
        pt[1].maxW = 2067; pt[1].maxH = 987;
        pt[2] = al[2];    // every size there, the call failed
        pt[2].ok = false;
        pt[3].ok = true;  // optimal zero, minimum and maximum set
        pt[3].minW = 1378; pt[3].minH = 658;
        pt[3].maxW = 4134; pt[3].maxH = 1974;
        CASE("C4.partial.min_zero", !dlssModeAnswered(pt[0]));
        CASE("C4.partial.min_above_max", !dlssModeAnswered(pt[1]));
        CASE("C4.partial.not_ok", !dlssModeAnswered(pt[2]));
        CASE("C4.partial.opt_zero", !dlssModeAnswered(pt[3]));
        CASE("C4.partial.none_answered", !dlssRangesAnswered(pt));
        const DlssModeRange full = al[2];
        DlssModeRange one;
        CASE("C4.partial.control", dlssModeAnswered(full));
        one = full; one.optW = 0;  CASE("C4.partial.opt_w_zero", !dlssModeAnswered(one));
        one = full; one.optH = 0;  CASE("C4.partial.opt_h_zero", !dlssModeAnswered(one));
        one = full; one.minW = 0;  CASE("C4.partial.min_w_zero", !dlssModeAnswered(one));
        one = full; one.minH = 0;  CASE("C4.partial.min_h_zero", !dlssModeAnswered(one));
        one = full; one.maxW = 0;  CASE("C4.partial.max_w_zero", !dlssModeAnswered(one));
        one = full; one.maxH = 0;  CASE("C4.partial.max_h_zero", !dlssModeAnswered(one));
        one = full; one.minW = full.maxW + 1;  CASE("C4.partial.min_w_above_max", !dlssModeAnswered(one));
        one = full; one.minH = full.maxH + 1;  CASE("C4.partial.min_h_above_max", !dlssModeAnswered(one));
        one = full; one.minW = full.maxW;      CASE("C4.partial.min_w_equals_max", dlssModeAnswered(one));

        // C5: the flight. 8268x3948 has the zero ladder; the ceiling is 8192 wide, and 3948 * 8192 / 8268 = 3911.7 floors to 3911 and is made even.
        uint32_t cow = 77, coh = 77;
        CASE("C5.ceiling_8268.model_door_zero", !answeredBy(perAxis, 8268, 3948));
        CASE("C5.ceiling_8268.model_cut_answered", answeredBy(perAxis, 8192, 3910));
        CASE("C5.ceiling_8268.model_next_pair_zero", !answeredBy(perAxis, 8194, 3912));  // the model itself: one pair of pixels past the limit
        CASE("C5.ceiling_8268.returns", dlssCeilingOutput(8268, 3948, perAxis, &cow, &coh));
        CASE("C5.ceiling_8268.width", cow == 8192);
        CASE("C5.ceiling_8268.height", coh == 3910);
        unsigned bw = 0, bh = 0;
        CASE("C5.ceiling_8268.oracle", bruteCeiling(8268, 3948, perAxis, &bw, &bh) && bw == cow && bh == coh);

        // C6: a portrait door, the limit on the height: 4134x8268 -> 4096x8192.
        cow = coh = 77;
        CASE("C6.ceiling_tall.returns", dlssCeilingOutput(4134, 8268, perAxis, &cow, &coh));
        CASE("C6.ceiling_tall.size", cow == 4096 && coh == 8192);
        CASE("C6.ceiling_tall.next_pair_zero", !answeredBy(perAxis, 4098, 8196));

        // C7: a door at the limit is answered as it stands; a door under it is cut only to even.
        cow = coh = 77;
        CASE("C7.ceiling_square.returns", dlssCeilingOutput(8192, 8192, perAxis, &cow, &coh));
        CASE("C7.ceiling_square.size", cow == 8192 && coh == 8192);
        cow = coh = 77;
        CASE("C7.ceiling_square.under_limit_returns", dlssCeilingOutput(3071, 3033, perAxis, &cow, &coh));
        CASE("C7.ceiling_square.under_limit_size", cow == 3070 && coh == 3032);
        cow = coh = 77;
        CASE("C7.ceiling_square.exact_door_returns", dlssCeilingOutput(3070, 3032, perAxis, &cow, &coh));
        CASE("C7.ceiling_square.exact_door_size", cow == 3070 && coh == 3032);

        // C8: an odd door. 3949 * 8192 / 8269 = 3912.2 -> 3912; 3950 * 8192 / 8269 = 3913.2 -> 3913 -> made even, 3912.
        cow = coh = 77;
        CASE("C8.ceiling_odd.returns", dlssCeilingOutput(8269, 3949, perAxis, &cow, &coh));
        CASE("C8.ceiling_odd.even", (cow & 1u) == 0 && (coh & 1u) == 0);
        CASE("C8.ceiling_odd.size", cow == 8192 && coh == 3912);
        cow = coh = 77;
        CASE("C8.ceiling_odd.odd_height_returns", dlssCeilingOutput(8269, 3950, perAxis, &cow, &coh));
        CASE("C8.ceiling_odd.odd_height_even", (cow & 1u) == 0 && (coh & 1u) == 0);
        CASE("C8.ceiling_odd.odd_height_size", cow == 8192 && coh == 3912);

        // C9: nothing at or under the door answers -> false, and the outputs say 0, not what they held.
        const auto zeroAlways = [&](unsigned, unsigned, DlssModeRange* ms) { zeroLadder(ms); };
        const auto notOk = [](unsigned, unsigned, DlssModeRange* ms) {
            for (int k = 0; k < kDlssModeCount; ++k) ms[k] = DlssModeRange{};  // ok false: NGX unavailable
        };
        cow = coh = 77;
        CASE("C9.ceiling_none.zero_ladder_false", !dlssCeilingOutput(8268, 3948, zeroAlways, &cow, &coh));
        CASE("C9.ceiling_none.zero_ladder_outputs_zero", cow == 0 && coh == 0);
        cow = coh = 77;
        CASE("C9.ceiling_none.not_ok_false", !dlssCeilingOutput(8268, 3948, notOk, &cow, &coh));
        CASE("C9.ceiling_none.not_ok_outputs_zero", cow == 0 && coh == 0);
        cow = coh = 77;
        CASE("C9.ceiling_none.no_door_false", !dlssCeilingOutput(0, 3948, perAxis, &cow, &coh) && cow == 0 && coh == 0);
        cow = coh = 77;
        CASE("C9.ceiling_none.no_door_height_false", !dlssCeilingOutput(8268, 0, perAxis, &cow, &coh) && cow == 0 && coh == 0);

        // C10: only a size seen answered is returned, however the answers fall. A hole at the natural ceiling, at 6000, an island, a hole at the search's
        // second probe: the size returned answers when it is asked again.
        const auto holes = [&](unsigned qw, unsigned qh, DlssModeRange* ms) {
            if (qw == 6000 || qw == 8192) zeroLadder(ms); else perAxis(qw, qh, ms);
        };
        cow = coh = 77;
        CASE("C10.ceiling_verified.holes_returns", dlssCeilingOutput(8268, 3948, holes, &cow, &coh));
        CASE("C10.ceiling_verified.holes_answered", answeredBy(holes, cow, coh));
        CASE("C10.ceiling_verified.holes_not_the_holes", cow < 8192 && cow != 6000 && coh <= 3910);
        const auto island = [&](unsigned qw, unsigned qh, DlssModeRange* ms) {
            if (qw >= 4000 && qw <= 4200) perAxis(qw, qh, ms); else zeroLadder(ms);
        };
        cow = coh = 77;
        CASE("C10.ceiling_verified.island_returns", dlssCeilingOutput(8268, 3948, island, &cow, &coh));
        CASE("C10.ceiling_verified.island_answered", answeredBy(island, cow, coh) && cow >= 4000 && cow <= 4200);
        const auto probeHole = [&](unsigned qw, unsigned qh, DlssModeRange* ms) {
            if (qw == 6202) zeroLadder(ms); else perAxis(qw, qh, ms);
        };
        cow = coh = 77;
        CASE("C10.ceiling_verified.probe_hole_returns", dlssCeilingOutput(8268, 3948, probeHole, &cow, &coh));
        CASE("C10.ceiling_verified.probe_hole_answered", answeredBy(probeHole, cow, coh) && cow <= 8192);

        // C11: a bisection -- a dozen queries for 8268x3948 (4134 widths), never a scan.
        unsigned calls = 0;
        const auto counting = [&](unsigned qw, unsigned qh, DlssModeRange* ms) { ++calls; perAxis(qw, qh, ms); };
        cow = coh = 77;
        CASE("C11.ceiling_calls.returns", dlssCeilingOutput(8268, 3948, counting, &cow, &coh) && cow == 8192 && coh == 3910);
        CASE("C11.ceiling_calls.at_most_16", calls >= 1 && calls <= 16);

        // C12: the search follows the query. Under a pixel-count limit (32 Mpx; not NGX's) the ceiling of the same door is not 8192x3910 -- it is what the
        // oracle finds under that limit -- so a search that hard-codes NGX's 8192 cannot pass both models.
        cow = coh = 77;
        CASE("C12.ceiling_pixel_model.returns", dlssCeilingOutput(8268, 3948, perPixel, &cow, &coh));
        CASE("C12.ceiling_pixel_model.differs", !(cow == 8192 && coh == 3910));
        CASE("C12.ceiling_pixel_model.under_limit", uint64_t(cow) * coh <= 32000000ull && answeredBy(perPixel, cow, coh));
        CASE("C12.ceiling_pixel_model.oracle", bruteCeiling(8268, 3948, perPixel, &bw, &bh) && bw == cow && bh == coh);
        CASE("C12.ceiling_pixel_model.models_differ", !answeredBy(perPixel, 8192, 3910) && answeredBy(perAxis, 8192, 3910));

        // C13: the cut output serves a 2x input. At 8192x3910 the three upper modes' floor is half the output, 4096x1955 (buildLadder stacks its modes,
        // so a floor on the flights' own shape is checked as well), and the flight's 4134x1974 input stands on it with the door unchanged.
        DlssModeRange cut[kDlssModeCount];
        buildLadder(8192, 3910, cut);
        CASE("C13.window_mode.stacked_serves_half", dlssRangesServe(cut, 4096, 1955));
        DlssModeRange fcut[kDlssModeCount];
        flightLadder(8192, 3910, fcut);
        CASE("C13.window_mode.answered", dlssRangesAnswered(fcut));
        CASE("C13.window_mode.floor_served", dlssRangesServe(fcut, 4096, 1955));
        CASE("C13.window_mode.under_floor_w_refused", !dlssRangesServe(fcut, 4095, 1955));
        CASE("C13.window_mode.under_floor_h_refused", !dlssRangesServe(fcut, 4096, 1954));
        CASE("C13.window_mode.max_served", dlssRangesServe(fcut, 8192, 3910));
        CASE("C13.window_mode.over_max_w_refused", !dlssRangesServe(fcut, 8194, 3910));
        CASE("C13.window_mode.over_max_h_refused", !dlssRangesServe(fcut, 8192, 3912));
        uint32_t wow = 0, woh = 0;
        int wmode = -2;
        CASE("C13.window_mode.flight_input_stands", dlssFloorOutput(fcut, 8192, 3910, 4134, 1974, &wow, &woh, &wmode) && wow == 8192 && woh == 3910 && wmode == -1);
    }

    std::printf("dlaa_mode_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
