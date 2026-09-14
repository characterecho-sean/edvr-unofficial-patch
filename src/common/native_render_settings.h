#pragma once

#include <windows.h>
#include <cmath>
#include <stdint.h>
#include <stddef.h>
#include <algorithm>

#ifdef __cplusplus
extern "C" {
#endif

#define EDVR_NATIVE_RENDER_SETTINGS_VERSION_1 1u
#define EDVR_NATIVE_RENDER_SCALE_DEFAULT 1.0f
#define EDVR_NATIVE_RENDER_SCALE_MINIMUM 0.25f
#define EDVR_NATIVE_RENDER_SCALE_MAXIMUM 2.0f

// CPU-only settings returned by the paired graphics provider. The exact
// size/version handshake lets the runtime reject an ABI drift before it uses
// the value. The reserved word keeps this POD extensible without changing
// version 1's layout.
typedef struct EdvrNativeRenderSettings {
    uint32_t size;
    uint32_t version;
    float    openxrRenderScale;
    uint32_t reserved;
} EdvrNativeRenderSettings;

#define EDVR_NATIVE_RENDER_SIZING_VERSION_1 1u

typedef struct EdvrNativeRenderViewBounds {
    uint32_t originalWidth;
    uint32_t originalHeight;
    uint32_t maxWidth;
    uint32_t maxHeight;
} EdvrNativeRenderViewBounds;

typedef struct EdvrNativeRenderSizing {
    uint32_t size;
    uint32_t version;
    uint64_t generation;
    EdvrNativeRenderViewBounds eyes[2];
    uint32_t activeWidth[2];
    uint32_t activeHeight[2];
    float requestedScale;
    float effectiveScale;
    uint32_t valid;
    uint32_t reserved;
} EdvrNativeRenderSizing;

typedef BOOL(WINAPI *EdvrQueryNativeRenderSettings)(
    uint32_t version, uint32_t size, void* output);
typedef BOOL(WINAPI *EdvrPublishNativeRenderSizing)(
    uint32_t version, uint32_t size, const void* input);
typedef BOOL(WINAPI *EdvrQueryNativeRenderSizing)(
    uint32_t version, uint32_t size, void* output);

BOOL WINAPI edvrPublishNativeRenderSizing(uint32_t version, uint32_t size,
                                          const void* input);
BOOL WINAPI edvrQueryNativeRenderSizing(uint32_t version, uint32_t size,
                                        void* output);
BOOL WINAPI edvrQueryNativeRenderSettings(uint32_t version, uint32_t size,
                                          void* output);

#ifdef __cplusplus
}

static_assert(sizeof(EdvrNativeRenderSettings) == 16,
              "native render settings ABI");
static_assert(sizeof(EdvrNativeRenderSizing) == 80,
              "native render sizing ABI");

namespace edvr::native_render {

inline float clampScale(float value) noexcept {
    if (!std::isfinite(value)) return EDVR_NATIVE_RENDER_SCALE_DEFAULT;
    if (value < EDVR_NATIVE_RENDER_SCALE_MINIMUM)
        return EDVR_NATIVE_RENDER_SCALE_MINIMUM;
    if (value > EDVR_NATIVE_RENDER_SCALE_MAXIMUM) return EDVR_NATIVE_RENDER_SCALE_MAXIMUM;
    return value;
}

inline uint32_t scaledDimension(uint32_t original, uint32_t maximum,
                               float scale) noexcept {
    if (!original || !maximum) return 0;
    const float factor = clampScale(scale);
    const double scaled = static_cast<double>(original) * static_cast<double>(factor);
    if (!(scaled == scaled) || scaled < 1.0) return 1;
    const double rounded = scaled + 0.5;
    if (rounded >= static_cast<double>(maximum)) return maximum;
    return static_cast<uint32_t>(rounded);
}

inline float effectiveScale(float requested,
                            const EdvrNativeRenderViewBounds* views,
                            size_t count) noexcept {
    float result = clampScale(requested);
    if (!views || !count) return result;
    for (size_t i = 0; i < count; ++i) {
        if (!views[i].originalWidth || !views[i].originalHeight ||
            !views[i].maxWidth || !views[i].maxHeight) return EDVR_NATIVE_RENDER_SCALE_DEFAULT;
        result = (std::min)(result, static_cast<float>(views[i].maxWidth) /
                                      static_cast<float>(views[i].originalWidth));
        result = (std::min)(result, static_cast<float>(views[i].maxHeight) /
                                      static_cast<float>(views[i].originalHeight));
    }
    return result;
}

}  // namespace edvr::native_render
#endif
