// supersample_test -- table tests for the supersample resolve's arithmetic
// (src/common/supersample_math.h), without a headset, a game, a device, or
// either DLL: everything under test is header-only, so this links nothing
// from src/ at all.
//
// Three things the field cannot check cheaply and a build can:
//
//   * the ARM/DISARM verdict -- from per-eye sizes and the runtime's
//     recommendation, including the double-wide texture Elite submits
//     (each eye named by bounds) and the flipped bounds OpenVR permits;
//   * the eye REGION those bounds name, which is where the other eye's
//     half is kept out of the filter's reach;
//   * both KERNELS' weights -- symmetric, positive where they must be, the
//     textbook Mitchell values at the textbook width, summing to one over
//     the taps every output pixel actually uses, and never reaching past
//     the region (a CPU transcription of the shader's own loop, run over a
//     two-half image, must come back as the one half alone).
//
// The HLSL in src/d3d11/supersample_pass.cpp transcribes the same two
// functions this file pins; tools/smoke exercises the GPU side.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../src/common/supersample_math.h"

namespace {

int g_fails = 0;

void ok(const char* what) { printf("  ok    %s\n", what); }

void check(bool got, bool want, const char* what) {
    if (got == want) {
        ok(what);
        return;
    }
    printf("  FAIL  %s -- got %s, wanted %s\n", what, got ? "true" : "false",
           want ? "true" : "false");
    ++g_fails;
}

void checkNear(float got, float want, float tol, const char* what) {
    if (fabsf(got - want) <= tol) {
        ok(what);
        return;
    }
    printf("  FAIL  %s -- got %g, wanted %g (tolerance %g)\n", what, got, want,
           tol);
    ++g_fails;
}

void checkEq(uint32_t got, uint32_t want, const char* what) {
    if (got == want) return;
    printf("  FAIL  %s -- got %u, wanted %u\n", what, got, want);
    ++g_fails;
}

// One axis of the resolve, transcribed from the shader's loop: output o in
// [0, outN) from region-relative input [0, inN), taps clamped INTO the
// region, weights normalised over the taps used. Single channel.
void resolveAxis(int filter, float width, const float* in, int inN, float* out,
                 int outN) {
    const float scale = static_cast<float>(inN) / static_cast<float>(outN);
    for (int o = 0; o < outN; ++o) {
        const edvr::SupersampleTaps t =
            edvr::supersampleTapRange(static_cast<uint32_t>(o), scale, width);
        float acc = 0.0f, wsum = 0.0f;
        for (int i = t.first; i <= t.last; ++i) {
            const float w = edvr::supersampleKernel(
                filter, (static_cast<float>(i) + 0.5f - t.centre) / scale, width);
            int ic = i;
            if (ic < 0) ic = 0;
            if (ic > inN - 1) ic = inN - 1;
            acc += in[ic] * w;
            wsum += w;
        }
        if (wsum <= 1e-6f) {
            printf("  FAIL  zero weight sum at output %d (filter %d width %g "
                   "scale %g)\n", o, filter, width, scale);
            ++g_fails;
            out[o] = 0.0f;
            continue;
        }
        out[o] = acc / wsum;
    }
}

// The whole separable resolve over one region of a wider image: rows of
// the region first (x0..x1 of each row), then columns of the intermediate.
void resolveRegion(int filter, float width, const std::vector<float>& img,
                   int imgW, int x0, int y0, int x1, int y1, int outW, int outH,
                   std::vector<float>* out) {
    const int rW = x1 - x0, rH = y1 - y0;
    std::vector<float> mid(static_cast<size_t>(outW) * rH);
    std::vector<float> row(rW), rowOut(outW);
    for (int y = 0; y < rH; ++y) {
        for (int x = 0; x < rW; ++x) row[x] = img[(y0 + y) * imgW + x0 + x];
        resolveAxis(filter, width, row.data(), rW, rowOut.data(), outW);
        for (int x = 0; x < outW; ++x) mid[static_cast<size_t>(y) * outW + x] = rowOut[x];
    }
    std::vector<float> col(rH), colOut(outH);
    out->assign(static_cast<size_t>(outW) * outH, 0.0f);
    for (int x = 0; x < outW; ++x) {
        for (int y = 0; y < rH; ++y) col[y] = mid[static_cast<size_t>(y) * outW + x];
        resolveAxis(filter, width, col.data(), rH, colOut.data(), outH);
        for (int y = 0; y < outH; ++y) (*out)[static_cast<size_t>(y) * outW + x] = colOut[y];
    }
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("edvr supersample test\n\n");

    // ---- supersampleExceeds: larger than the recommendation by more than
    // rounding, on the Quest 3's measured sizes (1456x1560 per eye). ------
    {
        using edvr::supersampleExceeds;
        check(supersampleExceeds(1456, 1560, 1456, 1560), false,
              "exceeds: the recommended size itself is not supersampling");
        check(supersampleExceeds(1456, 1560, 1457, 1561), false,
              "exceeds: a pixel of rounding is not supersampling");
        check(supersampleExceeds(1456, 1560, 1469, 1574), false,
              "exceeds: under one percent on both axes is rounding");
        check(supersampleExceeds(1456, 1560, 1470, 1560), true,
              "exceeds: one percent on one axis is enough");
        check(supersampleExceeds(1456, 1560, 1820, 1950), true,
              "exceeds: HMD Quality 1.25 (1820x1950) qualifies");
        check(supersampleExceeds(1456, 1560, 1300, 1950), false,
              "exceeds: smaller on either axis is never supersampling");
        // The first flight's numbers (Pimax, 2026-09-02), as the gate the
        // treat applies to the region it is handed: the guard's stage-1
        // margin looks like supersampling to this test (7% is over 1%), the
        // crop's landing at the recommended size does not -- which is why
        // stage-1 reports are withheld from the armer and a non-exceeding
        // region forwards untouched instead of standing anything down.
        check(supersampleExceeds(5424, 5356, 5792, 5356), true,
              "exceeds: the guard's 7% stage-1 margin reads as supersampling");
        check(supersampleExceeds(5424, 5356, 5424, 5356), false,
              "exceeds: the crop landing at the recommended size does not");
        check(supersampleExceeds(5424, 5356, 6780, 6695), true,
              "exceeds: HMD Quality 1.25 on the Pimax (6780x6695) qualifies");
        check(supersampleExceeds(0, 0, 1820, 1950), false,
              "exceeds: no recommendation, no verdict");
        // Small sizes: the two-pixel floor governs below 200 px.
        check(supersampleExceeds(100, 100, 101, 101), false,
              "exceeds: one pixel at 100 is rounding");
        check(supersampleExceeds(100, 100, 102, 100), true,
              "exceeds: two pixels at 100 is the floor");
    }

    // ---- supersampleRegionFromBounds: the double-wide texture Elite
    // submits (2912x1560, measured 2026-08-17), and flipped bounds. -------
    {
        using edvr::supersampleRegionFromBounds;
        uint32_t r[4] = {};
        bool fu = true, fv = true;
        check(supersampleRegionFromBounds(2912, 1560, nullptr, r, &fu, &fv), true,
              "region: null bounds name the whole texture");
        checkEq(r[0], 0, "region null x0"); checkEq(r[1], 0, "region null y0");
        checkEq(r[2], 2912, "region null x1"); checkEq(r[3], 1560, "region null y1");
        check(!fu && !fv, true, "region: null bounds run forwards");

        const float left[4] = {0.0f, 0.0f, 0.5f, 1.0f};
        check(supersampleRegionFromBounds(2912, 1560, left, r, &fu, &fv), true,
              "region: the left eye of a double-wide texture");
        checkEq(r[0], 0, "left x0"); checkEq(r[2], 1456, "left x1");
        checkEq(r[1], 0, "left y0"); checkEq(r[3], 1560, "left y1");

        const float right[4] = {0.5f, 0.0f, 1.0f, 1.0f};
        check(supersampleRegionFromBounds(2912, 1560, right, r, &fu, &fv), true,
              "region: the right eye of a double-wide texture");
        checkEq(r[0], 1456, "right x0"); checkEq(r[2], 2912, "right x1");
        check(!fu && !fv, true, "region: the right eye runs forwards");

        // Flipped v: the ordinary way to say the origin is at the bottom.
        // Same pixels as the left eye, direction remembered.
        const float flippedV[4] = {0.0f, 1.0f, 0.5f, 0.0f};
        check(supersampleRegionFromBounds(2912, 1560, flippedV, r, &fu, &fv), true,
              "region: flipped v names the same pixels");
        checkEq(r[0], 0, "flipped v x0"); checkEq(r[2], 1456, "flipped v x1");
        checkEq(r[1], 0, "flipped v y0"); checkEq(r[3], 1560, "flipped v y1");
        check(fv && !fu, true, "region: flipped v is remembered, u is not");

        // Backwards u on the right eye: u 1.0 -> 0.5.
        const float backU[4] = {1.0f, 0.0f, 0.5f, 1.0f};
        check(supersampleRegionFromBounds(2912, 1560, backU, r, &fu, &fv), true,
              "region: backwards u names the right eye's pixels");
        checkEq(r[0], 1456, "backwards u x0"); checkEq(r[2], 2912, "backwards u x1");
        check(fu && !fv, true, "region: backwards u is remembered, v is not");

        // One texture per eye (the Pimax), null bounds, plainly.
        check(supersampleRegionFromBounds(5424, 5356, nullptr, r, &fu, &fv), true,
              "region: a per-eye texture with null bounds");
        checkEq(r[2], 5424, "per-eye x1"); checkEq(r[3], 5356, "per-eye y1");

        const float tiny[4] = {0.0f, 0.0f, 0.001f, 1.0f};
        check(supersampleRegionFromBounds(2912, 1560, tiny, r, &fu, &fv), false,
              "region: a sliver is not an eye image");
        const float nan[4] = {0.0f, 0.0f, NAN, 1.0f};
        check(supersampleRegionFromBounds(2912, 1560, nan, r, &fu, &fv), false,
              "region: NaN bounds are refused");
    }

    // ---- SupersampleArmer: the verdict, frame by frame. -------------------
    {
        using edvr::SupersampleArmer;
        SupersampleArmer a;
        // One eye alone never arms.
        a.note(0, 1820, 1950);
        check(a.boundary(1456, 1560) == SupersampleArmer::kNone, true,
              "armer: one eye reported is not enough");
        check(a.armed, false, "armer: still off after one eye");
        // Both eyes, agreeing, larger: armed, with the sizes latched.
        a.note(0, 1820, 1950);
        a.note(1, 1820, 1950);
        check(a.boundary(1456, 1560) == SupersampleArmer::kArmed, true,
              "armer: both eyes at HMD Quality 1.25 arm it");
        checkEq(a.inW, 1820, "armer inW"); checkEq(a.inH, 1950, "armer inH");
        checkEq(a.outW, 1456, "armer outW"); checkEq(a.outH, 1560, "armer outH");
        // Steady frames change nothing; a frame with no reports (a
        // withheld frame) changes nothing either.
        a.note(0, 1820, 1950);
        a.note(1, 1820, 1950);
        check(a.boundary(1456, 1560) == SupersampleArmer::kNone, true,
              "armer: a steady frame is no event");
        check(a.boundary(1456, 1560) == SupersampleArmer::kNone && a.armed, true,
              "armer: a frame with no reports keeps the state");
        // Eyes disagreeing while armed: disarmed, and not re-armed until
        // they agree again.
        a.note(0, 1820, 1950);
        a.note(1, 1456, 1560);
        check(a.boundary(1456, 1560) == SupersampleArmer::kDisarmed, true,
              "armer: an eye at another size disarms");
        check(a.armed, false, "armer: off after the disagreement");
        // Both at native: stays off, no event.
        a.note(0, 1456, 1560);
        a.note(1, 1456, 1560);
        check(a.boundary(1456, 1560) == SupersampleArmer::kNone && !a.armed, true,
              "armer: both eyes at native stay off");
        // Both at a new larger size: armed again.
        a.note(0, 2184, 2340);
        a.note(1, 2184, 2340);
        check(a.boundary(1456, 1560) == SupersampleArmer::kArmed, true,
              "armer: both eyes at HMD Quality 1.5 arm it again");
        // A resolution change while armed, both eyes together: re-adopted
        // at the same boundary, no untreated frame.
        a.note(0, 1820, 1950);
        a.note(1, 1820, 1950);
        check(a.boundary(1456, 1560) == SupersampleArmer::kReadopted, true,
              "armer: a size change both eyes agree on re-adopts at once");
        checkEq(a.inW, 1820, "armer re-adopted inW");
        // Rounding is not a move: the guard's crop landing a pixel under
        // the armed size (the Quest 3's 3095 against 3096) neither disarms
        // nor re-adopts, and the armed size is kept; three pixels is a move.
        a.note(0, 1819, 1949);
        a.note(1, 1819, 1949);
        check(a.boundary(1456, 1560) == SupersampleArmer::kNone && a.armed &&
                  a.inW == 1820 && a.inH == 1950,
              true, "armer: a pixel of rounding neither disarms nor re-adopts");
        a.note(0, 1823, 1950);
        a.note(1, 1823, 1950);
        check(a.boundary(1456, 1560) == SupersampleArmer::kReadopted && a.inW == 1823,
              true, "armer: three pixels is a move, and re-adopts");
        // The recommendation moving up to meet the submission: disarmed.
        a.note(0, 1820, 1950);
        a.note(1, 1820, 1950);
        check(a.boundary(1820, 1950) == SupersampleArmer::kDisarmed, true,
              "armer: the recommendation catching up disarms");
        // A recommendation of zero (no system hook) never arms.
        a.note(0, 1820, 1950);
        a.note(1, 1820, 1950);
        check(a.boundary(0, 0) == SupersampleArmer::kNone && !a.armed, true,
              "armer: no recommendation, no arm");
    }

    // ---- supersampleEffectiveWidth: the ini's range and crisp's floor. ---
    {
        using edvr::supersampleEffectiveWidth;
        checkNear(supersampleEffectiveWidth(edvr::kSupersampleCalm, 0.5f), 0.5f, 0.0f,
                  "width: calm takes 0.5");
        checkNear(supersampleEffectiveWidth(edvr::kSupersampleCrisp, 0.5f), 1.0f, 0.0f,
                  "width: crisp floors at 1.0");
        checkNear(supersampleEffectiveWidth(edvr::kSupersampleCalm, 5.0f), 3.0f, 0.0f,
                  "width: 5 clamps to 3");
        checkNear(supersampleEffectiveWidth(edvr::kSupersampleCalm, NAN), 0.5f, 0.0f,
                  "width: NaN reads as the minimum");
    }

    // ---- The kernels themselves. -----------------------------------------
    {
        using edvr::supersampleKernel;
        const int calm = edvr::kSupersampleCalm, crisp = edvr::kSupersampleCrisp;
        // Mitchell-Netravali (B = C = 1/3) at the textbook width of 2: the
        // values every reference prints.
        checkNear(supersampleKernel(crisp, 0.0f, 2.0f), 8.0f / 9.0f, 1e-6f,
                  "mitchell: k(0) = 8/9");
        checkNear(supersampleKernel(crisp, 0.5f, 2.0f), 0.534722f, 1e-5f,
                  "mitchell: k(0.5) = 0.5347");
        checkNear(supersampleKernel(crisp, 1.0f, 2.0f), 1.0f / 18.0f, 1e-6f,
                  "mitchell: k(1) = 1/18, continuous across the pieces");
        checkNear(supersampleKernel(crisp, 1.5f, 2.0f), -0.034722f, 1e-5f,
                  "mitchell: k(1.5) = -0.0347, the negative lobe");
        checkNear(supersampleKernel(crisp, 2.0f, 2.0f), 0.0f, 0.0f,
                  "mitchell: k(2) = 0, the support's end");
        // Its integer shifts sum to one, at any phase (the partition-of-
        // unity property a resampling kernel must have).
        bool partition = true;
        for (float t = 0.0f; t < 1.0f; t += 0.125f) {
            float sum = 0.0f;
            for (int k = -3; k <= 3; ++k) {
                sum += supersampleKernel(crisp, t - static_cast<float>(k), 2.0f);
            }
            if (fabsf(sum - 1.0f) > 1e-5f) partition = false;
        }
        check(partition, true, "mitchell: integer shifts sum to one at every phase");
        // Squeezed to width 1, the same shape ends at 1.
        checkNear(supersampleKernel(crisp, 0.5f, 1.0f), 1.0f / 18.0f, 1e-6f,
                  "mitchell at width 1: k(0.5) is the textbook k(1)");
        checkNear(supersampleKernel(crisp, 1.0f, 1.0f), 0.0f, 0.0f,
                  "mitchell at width 1: the support ends at 1");
        // The Gaussian: sigma = width/2, positive inside, zero at the edge.
        checkNear(supersampleKernel(calm, 0.0f, 1.0f), 1.0f, 0.0f,
                  "gaussian: k(0) = 1");
        checkNear(supersampleKernel(calm, 0.5f, 1.0f), expf(-0.5f), 1e-6f,
                  "gaussian at width 1: k(sigma) = e^-1/2");
        checkNear(supersampleKernel(calm, 1.0f, 1.0f), 0.0f, 0.0f,
                  "gaussian at width 1: the support ends at 1");
        checkNear(supersampleKernel(calm, 1.0f, 2.0f), expf(-0.5f), 1e-6f,
                  "gaussian at width 2: k(1) = k(sigma)");
        bool symmetric = true, positive = true;
        for (float d = 0.0f; d < 3.0f; d += 0.05f) {
            for (float w = 0.5f; w <= 3.0f; w += 0.5f) {
                if (supersampleKernel(calm, d, w) != supersampleKernel(calm, -d, w) ||
                    supersampleKernel(crisp, d, w) != supersampleKernel(crisp, -d, w)) {
                    symmetric = false;
                }
                if (d < w && supersampleKernel(calm, d, w) <= 0.0f) positive = false;
            }
        }
        check(symmetric, true, "both kernels are symmetric");
        check(positive, true, "the gaussian is positive everywhere inside its support");
    }

    // ---- The taps: every output pixel has some, the nearest always. -----
    {
        bool sane = true;
        const float scales[] = {1.0f, 1.2f, 1.25f, 1.4f, 1.5f, 2.0f, 3.0f};
        const float widths[] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f};
        for (float scale : scales) {
            for (float width : widths) {
                for (uint32_t o = 0; o < 64; ++o) {
                    const edvr::SupersampleTaps t = edvr::supersampleTapRange(o, scale, width);
                    const int nearest = static_cast<int>(floorf(t.centre));
                    if (t.last < t.first || t.first > nearest || t.last < nearest) sane = false;
                    // The support: no tap further than the radius plus one
                    // input pixel, none closer than it should be missed.
                    const float radius = width * scale;
                    if (static_cast<float>(t.first) + 0.5f < t.centre - radius - 1.0f) sane = false;
                    if (static_cast<float>(t.last) + 0.5f > t.centre + radius + 1.0f) sane = false;
                }
            }
        }
        check(sane, true, "tap ranges: nonempty, include the nearest pixel, stay within the radius");
    }

    // ---- The resolve as a whole: weights sum to one (a flat image stays
    // flat, exactly), and taps never leave the region. ---------------------
    {
        const int filters[] = {edvr::kSupersampleCalm, edvr::kSupersampleCrisp};
        const float widths[] = {0.5f, 1.0f, 1.5f, 2.0f, 3.0f};
        const int cases[][4] = {   // inW, inH, outW, outH
            {40, 32, 32, 26},      // 1.25 x 1.23
            {56, 44, 40, 32},      // 1.4 x 1.375
            {60, 48, 40, 32},      // 1.5
            {64, 64, 32, 32},      // 2.0
            {33, 31, 32, 30},      // barely larger: one pixel, still fine
        };
        bool flat = true, contained = true;
        for (int filter : filters) {
            for (float widthIn : widths) {
                const float width = edvr::supersampleEffectiveWidth(filter, widthIn);
                for (const int* c : cases) {
                    const int inW = c[0], inH = c[1], outW = c[2], outH = c[3];
                    // A flat 0.37 everywhere: every output must read 0.37.
                    std::vector<float> img(static_cast<size_t>(inW) * inH, 0.37f);
                    std::vector<float> out;
                    resolveRegion(filter, width, img, inW, 0, 0, inW, inH, outW, outH, &out);
                    for (float v : out) {
                        if (fabsf(v - 0.37f) > 1e-5f) flat = false;
                    }
                    // A double-wide image: the left half 0.1, the right 0.9.
                    // Resolving the RIGHT half must come back 0.9 everywhere
                    // -- its leftmost column included, where a tap that
                    // strayed one pixel left would read 0.1.
                    std::vector<float> wide(static_cast<size_t>(inW) * 2 * inH);
                    for (int y = 0; y < inH; ++y) {
                        for (int x = 0; x < inW * 2; ++x) {
                            wide[static_cast<size_t>(y) * inW * 2 + x] = x < inW ? 0.1f : 0.9f;
                        }
                    }
                    resolveRegion(filter, width, wide, inW * 2, inW, 0, inW * 2, inH, outW, outH, &out);
                    for (float v : out) {
                        if (fabsf(v - 0.9f) > 1e-5f) contained = false;
                    }
                    // ...and the LEFT half comes back 0.1, its rightmost
                    // column included.
                    resolveRegion(filter, width, wide, inW * 2, 0, 0, inW, inH, outW, outH, &out);
                    for (float v : out) {
                        if (fabsf(v - 0.1f) > 1e-5f) contained = false;
                    }
                }
            }
        }
        check(flat, true, "resolve: a flat image stays flat for every kernel, width and ratio");
        check(contained, true, "resolve: taps never cross out of the eye's region");

        // And it does filter: a one-pixel checkerboard at ratio 2 resolves
        // to its mean under calm, and crisp keeps more of the contrast than
        // calm does at the same width -- "crisp" means what it says.
        {
            const int inN = 64, outN = 32;
            std::vector<float> in(inN), outCalm(outN), outCrisp(outN);
            for (int i = 0; i < inN; ++i) in[i] = (i & 1) ? 1.0f : 0.0f;
            resolveAxis(edvr::kSupersampleCalm, 1.0f, in.data(), inN, outCalm.data(), outN);
            resolveAxis(edvr::kSupersampleCrisp, 1.0f, in.data(), inN, outCrisp.data(), outN);
            float devCalm = 0.0f, devCrisp = 0.0f;
            for (int o = 4; o < outN - 4; ++o) {
                devCalm = fmaxf(devCalm, fabsf(outCalm[o] - 0.5f));
                devCrisp = fmaxf(devCrisp, fabsf(outCrisp[o] - 0.5f));
            }
            check(devCalm < 0.15f, true,
                  "resolve: calm at width 1 flattens a Nyquist checkerboard toward its mean");
            check(devCrisp >= devCalm, true,
                  "resolve: crisp keeps at least as much contrast as calm");
            // A smooth ramp passes through both kernels within a pixel's
            // slope: neither blurs a gradient into a different gradient.
            std::vector<float> ramp(inN), outR(outN);
            for (int i = 0; i < inN; ++i) ramp[i] = static_cast<float>(i) / (inN - 1);
            resolveAxis(edvr::kSupersampleCrisp, 2.0f, ramp.data(), inN, outR.data(), outN);
            bool rampOk = true;
            for (int o = 2; o < outN - 2; ++o) {
                const float want = ((static_cast<float>(o) + 0.5f) * 2.0f - 0.5f) / (inN - 1);
                if (fabsf(outR[o] - want) > 0.01f) rampOk = false;
            }
            check(rampOk, true, "resolve: crisp at width 2 reproduces a linear ramp");
        }
    }

    if (g_fails) {
        printf("\nSUPERSAMPLE TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("\nSUPERSAMPLE TEST PASSED\n");
    return 0;
}
