// The worn headset's names, as the last v2 render-settings query saw them.
// SystemRead is unreachable from src/d3d11 (the host keeps it), so this is
// the one source the F8 menu has for "this headset is oculus/meta-quest-3".
// Kept beside the published sizing in native_render_settings.cpp, under the
// same mutex, and cleared on exactly the condition that clears the sizing.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>

#include "../common/native_render_settings.h"
#include "../common/openxr_resolution_entries.h"

struct NativeRenderLabels {
    char runtimeName[64];   // raw XrInstanceProperties.runtimeName, truncated
    char systemName[64];    // raw XrSystemProperties.systemName, truncated
    char runtimeToken[edvr::native_render::kHeadsetTokenMax + 1];  // headsetToken of runtimeName
    char systemToken[edvr::native_render::kHeadsetTokenMax + 1];   // headsetToken of systemName; empty when none
    uint32_t valid;         // 0 before the first query and after close
};
// The token buffers follow the sanitiser's limit: sized by hand they would
// silently cut the menu's key when the constant grows, and the entry the
// menu wrote would never match the DLL's resolver.
static_assert(sizeof(NativeRenderLabels::runtimeToken) == edvr::native_render::kHeadsetTokenMax + 1 &&
              sizeof(NativeRenderLabels::systemToken) == edvr::native_render::kHeadsetTokenMax + 1,
              "NativeRenderLabels token buffers must hold a maximal headsetToken plus NUL");
static_assert(sizeof(NativeRenderLabels::runtimeName) == sizeof(EdvrNativeRenderSettings::runtimeName) &&
              sizeof(NativeRenderLabels::systemName) == sizeof(EdvrNativeRenderSettings::systemName),
              "NativeRenderLabels raw names must be the ABI's 64-byte fields");

// Copies the record; returns valid != 0. Before the first query, and after
// the host's valid=0 sizing publish, it reads valid = 0 and the menu shows
// `unknown`, which no runtime could produce as a token.
bool nativeRenderLabels(NativeRenderLabels* out);

// The graphics-log line edvrQueryNativeRenderSettings prints, formatted
// without Log so the self-test can bound its length for eight maximal
// entries (docs/openxr-resolution-per-headset-2026-09-14.md, "Log lines").
struct OpenxrResolutionReport {
    const char* runtimeName = nullptr;   // raw 64-byte field
    const char* systemName = nullptr;    // raw 64-byte field
    std::string runtimeToken, systemToken;
    std::string value;                   // fix.openxr_resolution as read
    const edvr::native_render::ResolutionEntry* entries = nullptr;
    size_t entryCount = 0;
    uint32_t matched = 0;                // 0 none, 1 runtime/system, 2 runtime-only
    uint32_t width = 0;                  // resolved width, 0 when no entry matched
    const EdvrNativeRenderViewBounds* eyes = nullptr;  // [2]
};
std::string formatOpenxrResolutionReport(const OpenxrResolutionReport& report);
