// The one eye-region rule at the door: given a texture size and the
// Submit bounds, which pixels are one eye's -- the whole texture, or
// either half of a double-wide, with either axis flipped. Header-only
// and pure, shared by src/d3d11/temporal_pass.cpp, sharpen_pass.cpp,
// native_sharpen.cpp, native_temporal.cpp and menu_panel.cpp, and pinned
// by tools/supersample_test.
#pragma once

#include <cmath>
#include <cstdint>

namespace edvr {

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

}  // namespace edvr
