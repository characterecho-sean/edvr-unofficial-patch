#include "../common/native_render_settings.h"
#include "native_render_labels.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/log.h"
#include "../common/openxr_resolution_entries.h"

namespace {
std::mutex g_sizingMutex;
EdvrNativeRenderSizing g_sizing{};
// The last v2 query's names, under the sizing's mutex and cleared with it:
// the settings input carries no generation of its own (the query runs before
// the sizing publish mints one), so the labels borrow the sizing's.
NativeRenderLabels g_labels{};

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

// The v2 settings buffer is in/out. MSVC refuses __try in a function that
// needs object unwinding (C2712), and the query body holds std::string, so
// the guarded copies live here and the parse sits in plain C++ between them.
bool copySettingsInput(const void* input, EdvrNativeRenderSettings& output) noexcept {
    __try {
        output=*static_cast<const EdvrNativeRenderSettings*>(input);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeSettingsOutput(void* output, const EdvrNativeRenderSettings& value) noexcept {
    __try {
        *static_cast<EdvrNativeRenderSettings*>(output)=value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool terminated(const char* field, size_t bytes) noexcept {
    return std::memchr(field, 0, bytes) != nullptr;
}

std::string dimensions(uint32_t w, uint32_t h) {
    char text[32];
    snprintf(text, sizeof(text), "%ux%u", w, h);
    return text;
}

std::string number(uint32_t value) {
    char text[16];
    snprintf(text, sizeof(text), "%u", value);
    return text;
}
}

bool nativeRenderLabels(NativeRenderLabels* out) {
    if (!out) return false;
    std::lock_guard<std::mutex> lock(g_sizingMutex);
    *out = g_labels;
    return out->valid != 0;
}

std::string formatOpenxrResolutionReport(const OpenxrResolutionReport& report) {
    using namespace edvr::native_render;
    const std::string key = headsetKey(report.runtimeToken, report.systemToken);
    EdvrNativeRenderViewBounds eyes[2]{};
    if (report.eyes) { eyes[0] = report.eyes[0]; eyes[1] = report.eyes[1]; }
    const uint32_t recW = eyes[0].originalWidth, recH = eyes[0].originalHeight;
    const uint32_t width = report.width ? report.width : recW;
    const float asked = widthToScale(width, recW);
    const float requested = clampScale(asked);
    const float effective = effectiveScale(requested, eyes, 2);
    const uint32_t w = scaledDimension(recW, eyes[0].maxWidth, effective);
    const uint32_t h = scaledDimension(recH, eyes[0].maxHeight, effective);
    char mp[32];
    snprintf(mp, sizeof(mp), "%.1f MP", megapixels(w, h));
    const std::string runtimeName = report.runtimeName ? report.runtimeName : "";
    const std::string systemName = report.systemName ? report.systemName : "";
    const std::string who = report.systemToken.empty()
        ? "(" + runtimeName + ", runtime reported no system name)"
        : "(" + runtimeName + ", \"" + systemName + "\")";
    const std::string saved = report.entryCount
        ? formatResolutionEntries(report.entries, report.entryCount) : "none";
    std::string line = "openxr resolution: ";
    double bare = 0.0;
    if (!report.matched && !report.entryCount && parseBareNumber(report.value.c_str(), &bare)) {
        // A user who copied an old-style value into the new key: say what
        // it would have meant here, or that it means nothing.
        bool aboveCap = false;
        const uint32_t offered = bareNumberToWidth(report.value.c_str(), eyes, &aboveCap);
        line += "fix.openxr_resolution = \"" + report.value + "\" is a bare number and names no headset; this headset (" +
                key + ", " + dimensions(recW, recH) + ") runs at 100% = " + dimensions(w, h) + " per eye (" + mp +
                "). Set it in F8 > Performance with this headset on";
        if (!offered && aboveCap) {
            // Within a quarter to twice, so name the bound that refused it.
            line += "; \"" + report.value + "\" is more than this runtime can build (" +
                    number(effectiveWidthCap(eyes)) + " wide at most for this headset).";
        } else if (!offered) {
            line += "; \"" + report.value + "\" is out of range for this headset (a quarter to twice " + number(recW) + " wide).";
        } else if (offered == recW) {
            line += ", or add \"" + key + ":<width>\" to fix.openxr_resolution (" + number(recW) + " is 100%).";
        } else {
            line += ", or replace the value with \"" + key + ":" + number(offered) + "\" for " + number(offered) + " wide (" +
                    formatPercent(widthToScale(offered, recW) * 100.f) + "%).";
        }
        return line;
    }
    line += "this headset is " + key + " " + who + "; ";
    if (!report.matched) {
        line += "edvr.ini has no entry for it, so it runs at 100% = " + dimensions(w, h) + " per eye (" + mp + "). Saved: " + saved +
                ". Set it in F8 > Performance with this headset on, or add \"" + key + ":<width>\" to fix.openxr_resolution (" +
                number(recW) + " is 100%).";
        return line;
    }
    line += "edvr.ini sets it to " + number(width) + " wide ";
    if (report.matched == 2) line += "from the runtime-only entry " + report.runtimeToken + ":" + number(width) + " ";
    line += "= " + dimensions(w, h) + " per eye (" + mp + ", " + formatPercent(effective * 100.f) + "% of the runtime's " +
            dimensions(recW, recH);
    // The runtime's cap is named first, as the menu names it (resolutionView):
    // it binds whatever the 2x clamp did, and the percent beside it is the
    // cap's, not 200.
    if (effective + 0.0005f < requested) line += ", runtime cap " + formatPercent(effective * 100.f) + "%";
    else if (asked > EDVR_NATIVE_RENDER_SCALE_MAXIMUM) line += ", capped at 200%";
    else if (asked < EDVR_NATIVE_RENDER_SCALE_MINIMUM) line += ", floored at 25%";
    line += "). Saved: " + saved + ". Elite's HMD Quality multiplies this.";
    return line;
}

// This is deliberately a CPU-only query. NativeRuntimeHost dispatches it on
// the graphics producer after the game device has initialized Config and
// before it creates OpenXR swapchains. It must never initialize D3D itself.
// The buffer is in/out: the host's bounds and names come in, one scale for
// this headset goes out with the inputs echoed, and the host compares them.
extern "C" BOOL WINAPI edvrQueryNativeRenderSettings(uint32_t version,
                                                       uint32_t size,
                                                       void* output) {
    if (version != EDVR_NATIVE_RENDER_SETTINGS_VERSION_2 ||
        size != sizeof(EdvrNativeRenderSettings) || !output) return FALSE;
    EdvrNativeRenderSettings in{};
    if (!copySettingsInput(output, in) || in.size != sizeof(in) ||
        in.version != EDVR_NATIVE_RENDER_SETTINGS_VERSION_2 || in.reserved != 0 ||
        !terminated(in.runtimeName, sizeof(in.runtimeName)) ||
        !terminated(in.systemName, sizeof(in.systemName))) return FALSE;
    for (unsigned eye = 0; eye < 2; ++eye) {
        const EdvrNativeRenderViewBounds& bounds = in.eyes[eye];
        if (!bounds.originalWidth || !bounds.originalHeight || !bounds.maxWidth ||
            !bounds.maxHeight || bounds.originalWidth > bounds.maxWidth ||
            bounds.originalHeight > bounds.maxHeight) return FALSE;
    }
    using namespace edvr::native_render;
    const std::string rt = headsetToken(in.runtimeName, sizeof(in.runtimeName));
    const std::string sys = headsetToken(in.systemName, sizeof(in.systemName));
    const std::string value = edvr::Config::get().getString("fix.openxr_resolution", "");
    ResolutionEntry entries[kResolutionEntryMax];
    std::vector<std::string> skipped;
    const size_t count = parseResolutionEntries(value.c_str(), entries, &skipped);
    uint32_t matched = 0;
    const uint32_t resolved = resolveResolutionWidth(entries, count, rt, sys, &matched);
    const uint32_t width = resolved ? resolved : in.eyes[0].originalWidth;
    EdvrNativeRenderSettings answer = in;
    answer.openxrRenderScale = clampScale(widthToScale(width, in.eyes[0].originalWidth));
    answer.matchedEntry = matched;
    answer.entryCount = static_cast<uint32_t>(count);
    answer.reserved = 0;
    if (!writeSettingsOutput(output, answer)) return FALSE;
    {
        std::lock_guard<std::mutex> lock(g_sizingMutex);
        g_labels = {};
        std::memcpy(g_labels.runtimeName, in.runtimeName, sizeof(g_labels.runtimeName));
        std::memcpy(g_labels.systemName, in.systemName, sizeof(g_labels.systemName));
        std::memcpy(g_labels.runtimeToken, rt.c_str(), (std::min)(rt.size(), sizeof(g_labels.runtimeToken) - 1));
        std::memcpy(g_labels.systemToken, sys.c_str(), (std::min)(sys.size(), sizeof(g_labels.systemToken) - 1));
        g_labels.valid = 1;
    }
    for (const std::string& token : skipped) {
        edvr::Log::get().note("openxr resolution: ignored \"%s\" in fix.openxr_resolution "
                              "(entries look like oculus/meta-quest-3:3283)", token.c_str());
    }
    OpenxrResolutionReport report;
    report.runtimeName = in.runtimeName;
    report.systemName = in.systemName;
    report.runtimeToken = rt;
    report.systemToken = sys;
    report.value = value;
    report.entries = entries;
    report.entryCount = count;
    report.matched = matched;
    report.width = resolved;
    report.eyes = in.eyes;
    const std::string line = formatOpenxrResolutionReport(report);
    edvr::Log::get().note("%s", line.c_str());
    return TRUE;
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
        if(!g_sizing.valid || candidate.generation==g_sizing.generation) { g_sizing={}; g_labels={}; }
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
