#pragma once
#include <cmath>
#include <cstdint>

namespace edvr {

// Qualified Epic cluster layout: 120-pixel tiles, 8x8 lighting work groups.
// At pixel centres p=n+0.5, the shipped eight Halton phases keep p-j in the
// same integer pixel and therefore in the same cluster/worklist. Leave at
// least 1/16 pixel of float32 margin at either boundary; merely abs(j)<0.5
// would allow values that round onto a neighbouring tile at large extents.
// This establishes XY lookup invariance only, not camera/resource ownership.
inline bool flatLightingLookupInvariant(float jitterX, float jitterY,
                                        uint32_t width, uint32_t height,
                                        uint32_t tileWidth, uint32_t gridX,
                                        uint32_t gridY, uint32_t sampleCount) {
    constexpr float maxJitter = 7.0f / 16.0f;
    if (!width || !height || width > 16384 || height > 16384 ||
        tileWidth != 120 || sampleCount != 1 ||
        !std::isfinite(jitterX) || !std::isfinite(jitterY) ||
        std::fabs(jitterX) > maxJitter || std::fabs(jitterY) > maxJitter)
        return false;
    return gridX == (width + tileWidth - 1) / tileWidth &&
           gridY == (height + tileWidth - 1) / tileWidth;
}

} // namespace edvr
