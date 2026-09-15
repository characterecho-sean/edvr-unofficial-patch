#pragma once

#include <windows.h>
#include <cmath>
#include <cstring>
#include <stdint.h>
#include <stddef.h>
#include <algorithm>

#ifdef __cplusplus
extern "C" {
#endif

#define EDVR_NATIVE_RENDER_SETTINGS_VERSION_2 2u
#define EDVR_NATIVE_RENDER_SCALE_DEFAULT 1.0f
#define EDVR_NATIVE_RENDER_SCALE_MINIMUM 0.25f
#define EDVR_NATIVE_RENDER_SCALE_MAXIMUM 2.0f

typedef struct EdvrNativeRenderViewBounds {
    uint32_t originalWidth;
    uint32_t originalHeight;
    uint32_t maxWidth;
    uint32_t maxHeight;
} EdvrNativeRenderViewBounds;

// CPU-only settings query, one in/out buffer. The host fills the input half
// (both eyes' bounds and the runtime and system names, truncated to 63 bytes
// plus NUL) and the paired graphics provider resolves fix.openxr_resolution
// for that headset into one scale. The provider writes the whole struct back
// with the inputs echoed unchanged, and the host compares them byte for byte
// (validateRenderSettingsAnswer below), so a field-order slip fails startup
// rather than flying a wrong size. Version 1 (16 bytes, a global scale) was
// replaced in place; both DLLs ship together and nothing native is public.
typedef struct EdvrNativeRenderSettings {
    uint32_t size;                       // in/out: sizeof == 184
    uint32_t version;                    // in/out: 2
    EdvrNativeRenderViewBounds eyes[2];  // in: host renderBounds[] (original
                                         //     and D3D-capped max per eye)
    char     runtimeName[64];            // in: XrInstanceProperties
                                         //     .runtimeName, truncated, NUL
    char     systemName[64];             // in: XrSystemProperties.systemName,
                                         //     truncated, NUL-terminated
    float    openxrRenderScale;          // out: width / eyes[0].originalWidth
                                         //     via clampScale; 1.0 when no
                                         //     entry matches
    uint32_t matchedEntry;               // out: 0 none, 1 runtime/system,
                                         //     2 runtime-only
    uint32_t entryCount;                 // out: well-formed entries parsed
    uint32_t reserved;                   // 0 in and out
} EdvrNativeRenderSettings;

#define EDVR_NATIVE_RENDER_SIZING_VERSION_1 1u

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

static_assert(sizeof(EdvrNativeRenderSettings) == 184,
              "native render settings ABI");
static_assert(sizeof(EdvrNativeRenderSizing) == 80,
              "native render sizing ABI");

namespace edvr::native_render {

// The v2 request, built by the host before the paired query. The whole
// struct is zeroed first: the echo check below compares the full 64 bytes of
// each name, and a name shorter than 63 bytes would otherwise leave stack
// bytes after the NUL that the provider's copy-back reproduces or not by
// accident. Shared by the host, the graphics DLL's test and the host test.
inline EdvrNativeRenderSettings buildRenderSettingsRequest(
    const EdvrNativeRenderViewBounds renderBounds[2], const char* runtimeLabel,
    const char* systemLabel) noexcept {
    EdvrNativeRenderSettings request;
    std::memset(&request, 0, sizeof(request));
    request.size = sizeof(request);
    request.version = EDVR_NATIVE_RENDER_SETTINGS_VERSION_2;
    if (renderBounds) {
        request.eyes[0] = renderBounds[0];
        request.eyes[1] = renderBounds[1];
    }
    for (size_t i = 0; runtimeLabel && i + 1 < sizeof(request.runtimeName) && runtimeLabel[i]; ++i)
        request.runtimeName[i] = runtimeLabel[i];
    for (size_t i = 0; systemLabel && i + 1 < sizeof(request.systemName) && systemLabel[i]; ++i)
        request.systemName[i] = systemLabel[i];
    return request;
}

// The host's check of the provider's answer: exact size and version, a zero
// reserved word, a finite scale, matchedEntry in 0..2, entryCount at most 8,
// and every input field (both eyes' bounds, both 64-byte names) echoed
// byte-equal to what was sent.
inline bool validateRenderSettingsAnswer(const EdvrNativeRenderSettings& sent,
                                         const EdvrNativeRenderSettings& answer) noexcept {
    if (answer.size != sizeof(answer) ||
        answer.version != EDVR_NATIVE_RENDER_SETTINGS_VERSION_2 ||
        answer.reserved != 0 || !std::isfinite(answer.openxrRenderScale) ||
        answer.matchedEntry > 2 || answer.entryCount > 8) return false;
    if (std::memcmp(answer.eyes, sent.eyes, sizeof(answer.eyes)) != 0) return false;
    if (std::memcmp(answer.runtimeName, sent.runtimeName, sizeof(answer.runtimeName)) != 0) return false;
    if (std::memcmp(answer.systemName, sent.systemName, sizeof(answer.systemName)) != 0) return false;
    return true;
}

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
