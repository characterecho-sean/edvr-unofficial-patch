#pragma once
#include "../../src/d3d11/flat_lighting_contract.h"
#include "../../src/common/temporal_math.h"
#include <cstdio>
#include <limits>
#include <algorithm>
#include <cstring>

// Exact GPU bounds from Epic 88a04caa, frame 66884. Frame 67330 carries
// identical bounds while the world camera rotates. Record0 is coarse, record1
// fine; omit duplicate XY corners of the coarse 1x1 grid.
inline bool flatCapturedLightingBounds(bool clampLastTile) {
    struct Fixture { uint32_t fine, corner, bits[8]; };
    const Fixture fixtures[] = {
        {0,0,{0xBF9E7948u,0xBF59E6C5u,0x3CCCCCCCu,0x3F800000u,0x3F9E7948u,0x3F324871u,0x3FAAB0D5u,0x3F800000u}},
        {0,4,{0xC2F79D81u,0xC2AA3C4Au,0x42C7FFFEu,0x3F800000u,0x42F79D81u,0x428B4899u,0x43055A27u,0x3F800000u}},
        {0,8,{0xC611110Bu,0xC5C77770u,0x45EA5784u,0x3F800000u,0x4611110Bu,0x45A3332Cu,0x461C3FFFu,0x3F800000u}},
        {1,0,{0xBF9E7948u,0x3BEDAD56u,0x3CCCCCCCu,0x3F800000u,0xBC8E9B33u,0x3F324871u,0x3FAAB0D5u,0x3F800000u}},
        {1,1,{0x3C8E9B33u,0x3BEDAD56u,0x3CCCCCCCu,0x3F800000u,0x3F9E7948u,0x3F324871u,0x3FAAB0D5u,0x3F800000u}},
        {1,2,{0xBF9E7948u,0xBF59E6C5u,0x3CCCCCCCu,0x3F800000u,0xBC8E9B33u,0xBC265FBCu,0x3FAAB0D5u,0x3F800000u}},
        {1,3,{0x3C8E9B33u,0xBF59E6C5u,0x3CCCCCCCu,0x3F800000u,0x3F9E7948u,0xBC265FBCu,0x3FAAB0D5u,0x3F800000u}},
        {1,4,{0xC2F79D81u,0x41E81B45u,0x42C7FFFEu,0x3F800000u,0xC28B438Fu,0x428B4899u,0x43055A27u,0x3F800000u}},
        {1,5,{0x428B438Fu,0x41E81B45u,0x42C7FFFEu,0x3F800000u,0x42F79D81u,0x428B4899u,0x43055A27u,0x3F800000u}},
        {1,6,{0xC2F79D81u,0xC2AA3C4Au,0x42C7FFFEu,0x3F800000u,0xC28B438Fu,0xC222797Cu,0x43055A27u,0x3F800000u}},
        {1,7,{0x428B438Fu,0xC2AA3C4Au,0x42C7FFFEu,0x3F800000u,0x42F79D81u,0xC222797Cu,0x43055A27u,0x3F800000u}},
        {1,8,{0xC611110Bu,0x4507FB0Fu,0x45EA5784u,0x3F800000u,0xC5A32D44u,0x45A3332Cu,0x461C3FFFu,0x3F800000u}},
        {1,9,{0x45A32D44u,0x4507FB0Fu,0x45EA5784u,0x3F800000u,0x4611110Bu,0x45A3332Cu,0x461C3FFFu,0x3F800000u}},
        {1,10,{0xC611110Bu,0xC5C77770u,0x45EA5784u,0x3F800000u,0xC5A32D44u,0xC53E5F7Bu,0x461C3FFFu,0x3F800000u}},
        {1,11,{0x45A32D44u,0xC5C77770u,0x45EA5784u,0x3F800000u,0x4611110Bu,0xC53E5F7Bu,0x461C3FFFu,0x3F800000u}},
    };
    const uint32_t rayBits[] = {1072541013u,3211636053u,3213209984u,1057337728u};
    float ray[4]; std::memcpy(ray, rayBits, sizeof(ray));
    for (const auto& f : fixtures) {
        float captured[8]; std::memcpy(captured, f.bits, sizeof(captured));
        const uint32_t x = f.fine && (f.corner & 1) ? 7 : 0;
        const uint32_t y = f.fine && (f.corner & 2) ? 4 : 0;
        const double xPixel[] = {double(x*120), double(f.fine ? (x+1)*120 : 960)};
        const double yPixel[] = {double(y*120), double(f.fine ? (y+1)*120 : 600)};
        double minimum[2] = {1e30,1e30}, maximum[2] = {-1e30,-1e30};
        for (uint32_t iz = 0; iz < 2; ++iz)
            for (uint32_t iy = 0; iy < 2; ++iy)
                for (uint32_t ix = 0; ix < 2; ++ix) {
                    const double z = captured[iz ? 6 : 2];
                    const double coords[] = {
                        (ray[0]*xPixel[ix]/960.0 + ray[1])*z,
                        (ray[2]*(clampLastTile ? std::min(yPixel[iy],540.0) : yPixel[iy])/540.0 + ray[3])*z
                    };
                    for (uint32_t axis=0; axis<2; ++axis) {
                        minimum[axis]=std::min(minimum[axis],coords[axis]);
                        maximum[axis]=std::max(maximum[axis],coords[axis]);
                    }
                }
        for (uint32_t axis=0; axis<2; ++axis) {
            const double scale=std::max(1.0,std::max(std::fabs(minimum[axis]),std::fabs(maximum[axis])));
            if (std::fabs(minimum[axis]-captured[axis])>2e-7*scale ||
                std::fabs(maximum[axis]-captured[axis+4])>2e-7*scale) return false;
        }
    }
    return true;
}

inline int flatLightingTests() {
    using namespace edvr;
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::printf("FAIL: flat lighting %s\n", what); ++failures; }
    };
    check(flatCapturedLightingBounds(false), "GPU bounds match padded view-space frustum cells");
    check(!flatCapturedLightingBounds(true), "clamping bottom tiles to scene height contradicts GPU bounds");
    // Include measured native/scaled extents and the largest D3D11 texture.
    const uint32_t sizes[][2] = {{960,540}, {1280,720}, {1,1}, {16384,16384}};
    for (const auto& size : sizes) {
        for (uint32_t phase = 0; phase < kTemporalJitterCount; ++phase) {
            float jitter[2]; temporalJitter(phase, &jitter[0], &jitter[1]);
            check(flatLightingLookupInvariant(jitter[0], jitter[1], size[0], size[1],
                120, (size[0]+119)/120, (size[1]+119)/120, 1), "real Halton phase admitted");
            for (uint32_t axis = 0; axis < 2; ++axis) {
                bool same = true;
                for (uint32_t pixel = 0; pixel < size[axis]; ++pixel) {
                    // Float operations match the shader; also test the CS
                    // work-group index rather than just comparing two floors.
                    const float sample = static_cast<float>(pixel) + 0.5f - jitter[axis];
                    const uint32_t tile = static_cast<uint32_t>(std::floor(sample / 120.0f));
                    same = same && tile == pixel / 120 && tile == (pixel / 8) / 15;
                }
                check(same, "all pixel centres preserve raster and compute worklist tiles");
            }
        }
    }
    check(flatLightingLookupInvariant(-0.4375f, 0.4375f, 960, 540, 120, 8, 5, 1),
          "finite margin includes measured sequence extremum");
    check(!flatLightingLookupInvariant(0.5f, 0, 960, 540, 120, 8, 5, 1) &&
          !flatLightingLookupInvariant(-0.5f, 0, 960, 540, 120, 8, 5, 1) &&
          !flatLightingLookupInvariant(std::nextafter(0.5f, 0.0f), 0, 960, 540, 120, 8, 5, 1),
          "near-half boundary needs a separate floating-point proof");
    check(!flatLightingLookupInvariant(std::numeric_limits<float>::quiet_NaN(), 0, 960, 540, 120, 8, 5, 1) &&
          !flatLightingLookupInvariant(0, std::numeric_limits<float>::infinity(), 960, 540, 120, 8, 5, 1),
          "nonfinite offsets refused");
    check(!flatLightingLookupInvariant(0, 0, 960, 540, 120, 8, 4, 1) &&
          !flatLightingLookupInvariant(0, 0, 960, 540, 128, 8, 5, 1) &&
          !flatLightingLookupInvariant(0, 0, 960, 540, 120, 8, 5, 4) &&
          !flatLightingLookupInvariant(0, 0, 0, 540, 120, 8, 5, 1) &&
          !flatLightingLookupInvariant(0, 0, 16385, 540, 120, 137, 5, 1),
          "unqualified layout or sampling refused");
    // A full pixel really does cross the next tile: rejection is material.
    check(std::floor((119.5f + 1.0f) / 120.0f) != 119 / 120,
          "larger jitter changes light worklist ownership");
    return failures;
}
