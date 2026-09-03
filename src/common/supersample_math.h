// The supersample resolve's arithmetic, header-only and pure, shared by the
// openvr half (which decides), the d3d11 half (which filters) and the test
// that pins both (tools/supersample_test) -- the SubmitPairLatch precedent
// in frame_flag.h: one definition, every consumer, nothing to keep in step
// by hand.
//
// docs/anti-aliasing.md, Feature A. When the game submits an eye image
// larger than the runtime asked for (Elite's HMD Quality above 1.0 is the
// usual cause), EDVR filters it down to exactly the recommended size at
// submit with a kernel chosen for anti-aliasing, and the compositor samples
// the result one to one through its distortion pass instead of decimating a
// large texture on the fly. Three questions live here:
//
//   * IS the submitted size larger, by more than rounding?  supersampleExceeds
//   * WHICH pixels are one eye's?                            supersampleRegionFromBounds
//   * WHAT does each tap weigh?                              supersampleKernel
//
// plus the arm/disarm state machine both eyes are judged by, so the
// decision can be table-tested without a headset. The HLSL in
// src/d3d11/supersample_pass.cpp is a line-for-line transcription of
// supersampleKernel and supersampleTapRange; if one changes, both do.
#pragma once

#include <cmath>
#include <cstdint>

namespace edvr {

// The two kernels, by the names edvr.ini uses (fix.supersample_filter).
enum SupersampleFilter : int { kSupersampleCalm = 0, kSupersampleCrisp = 1 };

// The radius the ini accepts (fix.supersample_width), in output pixels.
constexpr float kSupersampleWidthMin = 0.5f;
constexpr float kSupersampleWidthMax = 3.0f;

// How far a submitted size may drift from the one the resolve armed on
// before it counts as a move. The cull guard's crop lands one pixel under
// the pre-guard size on the Quest 3 -- 3095 against 3096, 2026-09-03 --
// because canonical = submitted / factor rounds on its own, and an exact
// comparison re-adopted on it: one log line per go-live, for nothing. Two
// pixels is the same slack supersampleExceeds calls rounding.
constexpr uint32_t kSupersampleSettleSlackPx = 2;

// The width a kernel actually runs at. Crisp needs at least 1.0: its
// nearest tap can sit half an output pixel from the centre, and below 1.0
// that lands in Mitchell's negative lobe with no positive tap to outweigh
// it -- a zero or negative weight sum, which is no filter at all. Calm is
// positive everywhere and takes any width in range.
inline float supersampleEffectiveWidth(int filter, float width) {
    if (!(width >= kSupersampleWidthMin)) width = kSupersampleWidthMin;  // NaN too
    if (width > kSupersampleWidthMax) width = kSupersampleWidthMax;
    if (filter == kSupersampleCrisp && width < 1.0f) width = 1.0f;
    return width;
}

// The unnormalised weight of a tap `dOut` OUTPUT pixels from an output
// pixel's centre, for a kernel of radius `width` output pixels. Zero at and
// beyond the radius; the caller normalises over the taps it actually used.
//
//   calm   a Gaussian with sigma = width/2, so the support ends at two
//          sigma. Positive everywhere: it can never ring on the HUD's
//          hairlines, which is what "calm" buys.
//   crisp  Mitchell-Netravali with B = C = 1/3 -- the textbook compromise
//          between blur and ringing -- its unit support of 2 stretched (or
//          squeezed) to end at `width`. Crisp at 2.0 is the textbook filter.
inline float supersampleKernel(int filter, float dOut, float width) {
    const float t = fabsf(dOut);
    if (!(width > 0.0f) || t >= width) return 0.0f;
    if (filter == kSupersampleCrisp) {
        const float x = 2.0f * t / width;
        // Mitchell-Netravali, B = C = 1/3, the coefficients evaluated:
        //   |x| < 1:      ( 7 x^3 - 12 x^2 + 16/3) / 6
        //   1 <= |x| < 2: (-7/3 x^3 + 12 x^2 - 20 x + 32/3) / 6
        // k(0) = 8/9, k(1) = 1/18 from both pieces, k(2) = 0; the integer
        // shifts of it sum to one, which the test checks.
        if (x < 1.0f) {
            return (7.0f * x * x * x - 12.0f * x * x + 16.0f / 3.0f) / 6.0f;
        }
        return (-7.0f / 3.0f * x * x * x + 12.0f * x * x - 20.0f * x +
                32.0f / 3.0f) / 6.0f;
    }
    const float sigma = width * 0.5f;
    const float u = t / sigma;
    return expf(-0.5f * u * u);
}

// The input taps that reach one output pixel along one axis. `o` is the
// output index, `scale` the input pixels per output pixel (>= 1 here: this
// pass only ever shrinks), `width` the kernel radius in output pixels.
// Input coordinates are region-relative: tap 0 is the region's first pixel
// on that axis. Taps are the input pixels whose centres fall within the
// radius; the caller clamps each into the region (edge replication) and
// weighs it at supersampleKernel(((i + 0.5) - centre) / scale).
struct SupersampleTaps {
    int   first;   // inclusive
    int   last;    // inclusive; last >= first always (the nearest tap)
    float centre;  // the output pixel's centre in input pixels
};
inline SupersampleTaps supersampleTapRange(uint32_t o, float scale, float width) {
    SupersampleTaps t;
    t.centre = (static_cast<float>(o) + 0.5f) * scale;
    const float radius = width * scale;
    t.first = static_cast<int>(ceilf(t.centre - radius - 0.5f));
    t.last = static_cast<int>(floorf(t.centre + radius - 0.5f));
    // The nearest input pixel is always a tap, whatever the rounding did
    // at a tiny radius: an output pixel with no input is not a resolve.
    const int nearest = static_cast<int>(floorf(t.centre));
    if (t.first > nearest) t.first = nearest;
    if (t.last < nearest) t.last = nearest;
    return t;
}

// Larger than the recommendation by more than rounding.
//
// A pixel or two of disagreement is two roundings of the same size, not
// supersampling -- the render-scale work measured a Pimax vertical of 4285
// against 4284 between the runtime's answer and the game's target. One
// percent or two pixels, whichever is larger, on either axis; and neither
// axis SMALLER than recommended, which is the other direction entirely
// (performance.md's render scale) and not this pass's to touch.
inline bool supersampleExceeds(uint32_t recW, uint32_t recH, uint32_t subW,
                               uint32_t subH) {
    if (!recW || !recH || subW < recW || subH < recH) return false;
    auto slack = [](uint32_t n) {
        const uint32_t p = n / 100;
        return p > 2 ? p : 2;
    };
    return subW >= recW + slack(recW) || subH >= recH + slack(recH);
}

// The pixel box one eye's Submit bounds name inside a texW x texH texture,
// as {x0, y0, x1, y1} with exclusive maxima, plus which axes the bounds ran
// backwards along. Null bounds mean the whole texture.
//
// guardCropCopy's composition with no crop fractions: the box is taken from
// the min and max ends of each span, so a flipped span (vMin = 1, vMax = 0,
// the ordinary way to say the origin is at the bottom, and OpenVR permits
// it) names the same pixels as its unflipped twin, and only the direction
// is remembered for the outgoing full-span bounds. Elite names each eye of
// ONE double-wide texture this way (measured 2026-08-17: 2912x1560
// submitted, each eye u 0..0.5 or 0.5..1), so the region is where the
// other eye's half is kept out of the filter's reach.
//
// False for a box under 16 pixels on a side, which is not an eye image.
inline bool supersampleRegionFromBounds(uint32_t texW, uint32_t texH,
                                        const float* bounds, uint32_t out[4],
                                        bool* flipU, bool* flipV) {
    float uMin = 0.0f, vMin = 0.0f, uMax = 1.0f, vMax = 1.0f;
    if (bounds) {
        uMin = bounds[0];
        vMin = bounds[1];
        uMax = bounds[2];
        vMax = bounds[3];
    }
    if (!(uMin == uMin) || !(vMin == vMin) || !(uMax == uMax) ||
        !(vMax == vMax)) {
        return false;  // NaN in somebody else's struct
    }
    auto toPixel = [](float frac, uint32_t extent) -> long {
        long v = lroundf(frac * static_cast<float>(extent));
        if (v < 0) v = 0;
        if (v > static_cast<long>(extent)) v = static_cast<long>(extent);
        return v;
    };
    const long x0 = toPixel(uMin < uMax ? uMin : uMax, texW);
    const long x1 = toPixel(uMin < uMax ? uMax : uMin, texW);
    const long y0 = toPixel(vMin < vMax ? vMin : vMax, texH);
    const long y1 = toPixel(vMin < vMax ? vMax : vMin, texH);
    if (x1 - x0 < 16 || y1 - y0 < 16) return false;
    out[0] = static_cast<uint32_t>(x0);
    out[1] = static_cast<uint32_t>(y0);
    out[2] = static_cast<uint32_t>(x1);
    out[3] = static_cast<uint32_t>(y1);
    if (flipU) *flipU = uMax < uMin;
    if (flipV) *flipV = vMax < vMin;
    return true;
}

// The arm/disarm decision, judged once per frame at the boundary from what
// both eyes submitted during it -- the cull guard's changed-size adoption
// discipline, mirrored for a size nobody asked the game to change:
//
//   * arm only when BOTH eyes have submitted this frame, at the SAME size,
//     and that size exceeds the recommendation (supersampleExceeds);
//   * while armed, any eye arriving at another size, or the recommendation
//     itself moving, disarms -- and the same boundary re-adopts at once if
//     the new sizes qualify, so a resolution change costs no untreated
//     frame; a drop to native leaves it disarmed until both eyes agree on
//     something larger again;
//   * a frame that reported nothing (a withheld frame) changes nothing.
//
// The pass itself takes any input at least as large as its output, so the
// armed size is a decision about WHETHER to treat, never a promise about
// the exact input; a frame whose size moved mid-decision is still resolved
// correctly, and the boundary catches up one frame later.
struct SupersampleArmer {
    uint32_t eyeW[2] = {};
    uint32_t eyeH[2] = {};
    bool     eyeSeen[2] = {};   // reported since the last boundary
    bool     armed = false;
    uint32_t inW = 0, inH = 0;    // the per-eye size the arm was taken at
    uint32_t outW = 0, outH = 0;  // the recommendation it resolves to

    enum Event { kNone = 0, kArmed = 1, kDisarmed = 2, kReadopted = 3 };

    void note(int eye, uint32_t w, uint32_t h) {
        if (eye < 0 || eye > 1) return;
        eyeW[eye] = w;
        eyeH[eye] = h;
        eyeSeen[eye] = true;
    }

    // A submitted size within the settle slack of the armed one has not
    // moved. The recommendation is compared exactly: it is the runtime's
    // own number and changes only when somebody moves a slider. (Not
    // named near: windef.h defines that to nothing, a 16-bit keyword kept
    // for compatibility, and the close-out build found out.)
    static bool withinSlack(uint32_t a, uint32_t b) {
        return (a > b ? a - b : b - a) <= kSupersampleSettleSlackPx;
    }

    Event boundary(uint32_t recW, uint32_t recH) {
        const bool wasArmed = armed;
        if (armed) {
            bool moved = recW != outW || recH != outH;
            for (int e = 0; e < 2; ++e) {
                if (eyeSeen[e] && (!withinSlack(eyeW[e], inW) || !withinSlack(eyeH[e], inH))) {
                    moved = true;
                }
            }
            if (moved) armed = false;
        }
        Event ev = kNone;
        if (!armed) {
            const bool agree = eyeSeen[0] && eyeSeen[1] && eyeW[0] == eyeW[1] &&
                               eyeH[0] == eyeH[1];
            if (agree && supersampleExceeds(recW, recH, eyeW[0], eyeH[0])) {
                armed = true;
                inW = eyeW[0];
                inH = eyeH[0];
                outW = recW;
                outH = recH;
                ev = wasArmed ? kReadopted : kArmed;
            } else if (wasArmed) {
                ev = kDisarmed;
            }
        }
        eyeSeen[0] = eyeSeen[1] = false;
        return ev;
    }
};

}  // namespace edvr
