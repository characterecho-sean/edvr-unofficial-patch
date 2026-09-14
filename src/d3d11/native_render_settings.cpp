#include "../common/native_render_settings.h"

#include <cmath>
#include <mutex>

#include "../common/config.h"

namespace {
std::mutex g_sizingMutex;
EdvrNativeRenderSizing g_sizing{};

bool copySizingInput(const void* input, EdvrNativeRenderSizing& output) noexcept {
    __try {
        output=*static_cast<const EdvrNativeRenderSizing*>(input);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeSizingOutput(void* output, const EdvrNativeRenderSizing& value) noexcept {
    __try {
        *static_cast<EdvrNativeRenderSizing*>(output)=value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}

// This is deliberately a CPU-only query. NativeRuntimeHost dispatches it on
// the graphics producer after the game device has initialized Config and
// before it creates OpenXR swapchains. It must never initialize D3D itself.
extern "C" BOOL WINAPI edvrQueryNativeRenderSettings(uint32_t version,
                                                       uint32_t size,
                                                       void* output) {
    if (version != EDVR_NATIVE_RENDER_SETTINGS_VERSION_1 ||
        size != sizeof(EdvrNativeRenderSettings) || !output) return FALSE;
    __try {
        EdvrNativeRenderSettings result{
            sizeof(EdvrNativeRenderSettings),
            EDVR_NATIVE_RENDER_SETTINGS_VERSION_1,
            edvr::native_render::clampScale(
                edvr::Config::get().getFloat("fix.openxr_render_scale",
                                               EDVR_NATIVE_RENDER_SCALE_DEFAULT)),
            0};
        *static_cast<EdvrNativeRenderSettings*>(output) = result;
        return TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }
}

extern "C" BOOL WINAPI edvrPublishNativeRenderSizing(uint32_t version,
                                                        uint32_t size,
                                                        const void* input) {
    if (version != EDVR_NATIVE_RENDER_SIZING_VERSION_1 ||
        size != sizeof(EdvrNativeRenderSizing) || !input) return FALSE;
    EdvrNativeRenderSizing candidate{};
    if(!copySizingInput(input,candidate) || candidate.size!=sizeof(candidate) ||
       candidate.version!=version || candidate.reserved!=0 || candidate.valid>1)
        return FALSE;
    if(!candidate.valid) {
        std::lock_guard<std::mutex> lock(g_sizingMutex);
        if(!g_sizing.valid || candidate.generation==g_sizing.generation) g_sizing={};
        return TRUE;
    }
    if(!candidate.generation || !std::isfinite(candidate.requestedScale) ||
       !std::isfinite(candidate.effectiveScale)) return FALSE;
    for(unsigned eye=0;eye<2;++eye) {
        const auto& bounds=candidate.eyes[eye];
        if(!bounds.originalWidth || !bounds.originalHeight || !bounds.maxWidth ||
           !bounds.maxHeight || bounds.originalWidth>bounds.maxWidth ||
           bounds.originalHeight>bounds.maxHeight || !candidate.activeWidth[eye] ||
           !candidate.activeHeight[eye]) return FALSE;
    }
    const float expected=edvr::native_render::effectiveScale(
        candidate.requestedScale,candidate.eyes,2);
    if(std::fabs(expected-candidate.effectiveScale)>0.0001f) return FALSE;
    for(unsigned eye=0;eye<2;++eye) {
        if(candidate.activeWidth[eye]!=edvr::native_render::scaledDimension(
               candidate.eyes[eye].originalWidth,candidate.eyes[eye].maxWidth,candidate.effectiveScale) ||
           candidate.activeHeight[eye]!=edvr::native_render::scaledDimension(
               candidate.eyes[eye].originalHeight,candidate.eyes[eye].maxHeight,candidate.effectiveScale))
            return FALSE;
    }
    std::lock_guard<std::mutex> lock(g_sizingMutex);
    if(g_sizing.valid && candidate.generation<g_sizing.generation) return FALSE;
    g_sizing=candidate;
    return TRUE;
}

extern "C" BOOL WINAPI edvrQueryNativeRenderSizing(uint32_t version,
                                                      uint32_t size,
                                                      void* output) {
    if (version != EDVR_NATIVE_RENDER_SIZING_VERSION_1 ||
        size != sizeof(EdvrNativeRenderSizing) || !output) return FALSE;
    EdvrNativeRenderSizing snapshot{};
    {
        std::lock_guard<std::mutex> lock(g_sizingMutex);
        snapshot=g_sizing;
    }
    return writeSizingOutput(output,snapshot) ? TRUE : FALSE;
}
