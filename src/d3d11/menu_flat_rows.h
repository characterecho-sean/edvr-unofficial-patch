// The rows of the flat profile's F8 panel, in three tables: the Performance tab (kFlatPageRows, the panel's settings), the Visual tab
// (kFlatVisualRows, the [visual] options) and the Debug tab (kFlatDebugRows, two toggles every flat user may reach, labelled debug).
//
// menu.cpp builds the pages from them, and tools\flat_sharpen_test reads them to
// fail the build for any row whose key the flat profile's gate refuses
// (runtime_profile.h: runtimeProfileAllowsKey). A refused key raises no error:
// its getter answers 0 or "off", so the row shows a value that does nothing and
// the log says nothing. That is the failure this table exists to make loud.
//
// `read` says how the row's value reaches the panel and the code that acts on it:
//   Allowlist      through Config's getters, so the key must pass the gate;
//   RequestedMode  through Config::requestedTemporalMode(), which is the one
//                  read that bypasses it -- fix.temporal_aa, the mode itself.
//
// Order is the panel's: menu.cpp walks the generated schema (edvr.ini order) and
// keeps the rows that are named here, so a row is added by adding it here and
// giving it a `# ui:` line in edvr.ini.
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace edvr {

enum class FlatRowRead { Allowlist, RequestedMode };

struct FlatPageRow {
    const char* section;
    const char* key;
    FlatRowRead read;
};

inline constexpr FlatPageRow kFlatPageRows[] = {
    {"fix", "temporal_aa",       FlatRowRead::RequestedMode},
    {"fix", "temporal_aa_model", FlatRowRead::Allowlist},
    {"fix", "render_sharpness",  FlatRowRead::Allowlist},
    {"fix", "ui_quality",        FlatRowRead::Allowlist},
    {"fix", "sharp_weapons",     FlatRowRead::Allowlist},   // directly under UI quality, as in VR (2026-10-16)
    {"fix", "optimized_stations", FlatRowRead::Allowlist},
};
inline constexpr size_t kFlatPageRowCount = sizeof(kFlatPageRows) / sizeof(kFlatPageRows[0]);

// The Visual tab (2026-10-22): the [visual] options, for every flat user (Fix tier). Both are choices, both live:
//   intro_video  Intro movie  skip, or the game's own movie (flat, 2026-10-14). On the Performance tab until 2026-10-22, as fix.intro_video.
//                             Its `menu` token is the bare one, so VR's menu, which has no Visual page, still lists it on the Fixes page.
//   star_glow    Star glow    100 to 200 percent of the halo the game's own bloom draws around a star; its `menu visual` token is the
//                             page only this panel builds, as `menu debug` is.
// Order is the tab's: menu.cpp walks the generated schema (edvr.ini order) and keeps the rows named here, whatever page their token names.
inline constexpr FlatPageRow kFlatVisualRows[] = {
    {"visual", "intro_video", FlatRowRead::Allowlist},
    {"visual", "star_glow",   FlatRowRead::Allowlist},
};
inline constexpr size_t kFlatVisualRowCount = sizeof(kFlatVisualRows) / sizeof(kFlatVisualRows[0]);

// The Debug tab (2026-10-16). Both are [advanced] keys with `menu debug` on their ui: line in edvr.ini (a per-key exception of
// tools\gen_settings_schema.py, MENU_ADVANCED_EXCEPTIONS), both toggles, both live, both off by default:
//   flat_cpu_timing   CPU timing in the log   the census's clocks (flat_cpu.h Census::setClocks, flat_runtime.cpp)
//   flat_refusal_view Motion source view      the prep's per-pixel class painted over the resolved image
// Order is the tab's: menu.cpp walks the generated schema (edvr.ini order) and keeps the `debug` rows named here.
inline constexpr FlatPageRow kFlatDebugRows[] = {
    {"advanced", "flat_cpu_timing",   FlatRowRead::Allowlist},
    {"advanced", "flat_refusal_view", FlatRowRead::Allowlist},
};
inline constexpr size_t kFlatDebugRowCount = sizeof(kFlatDebugRows) / sizeof(kFlatDebugRows[0]);
// The generated page name the Debug rows carry (the `menu debug` token).
inline constexpr const char* kFlatDebugPage = "debug";
// The tabs, in order: Performance (the existing page), Visual, then Debug. Debug is always last.
inline constexpr const char* kFlatTabNames[] = {"Performance", "Visual", "Debug"};
inline constexpr size_t kFlatTabCount = sizeof(kFlatTabNames) / sizeof(kFlatTabNames[0]);

inline bool flatDebugHasRow(const char* section, const char* key) {
    for (const FlatPageRow& r : kFlatDebugRows) {
        if (std::strcmp(r.section, section) == 0 && std::strcmp(r.key, key) == 0) return true;
    }
    return false;
}
inline bool flatVisualHasRow(const char* section, const char* key) {
    for (const FlatPageRow& r : kFlatVisualRows) {
        if (std::strcmp(r.section, section) == 0 && std::strcmp(r.key, key) == 0) return true;
    }
    return false;
}
inline bool flatPageHasRow(const char* section, const char* key) {
    for (const FlatPageRow& r : kFlatPageRows) {
        if (std::strcmp(r.section, section) == 0 && std::strcmp(r.key, key) == 0) return true;
    }
    return false;
}

// Whether the Sharpening row is dimmed (and ignores steps and typing). In the flat
// profile the sharpening acts on the temporal pass's output, so with anti-aliasing
// off there is nothing for it to act on: the row stays on the panel and dims, the
// way the DLSS preset row does under a mode that does not read it. VR sharpens
// whatever it submits, so there the row never dims. One function so a rig can read
// the whole truth table; menu.cpp asks it and nothing else.
inline bool flatSharpenRowDim(bool flatProfile, bool antiAliasingOn) {
    return flatProfile && !antiAliasingOn;
}

// Whether the Sharp weapons row is dimmed (and ignores steps and typing): it lays its layer over the temporal pass's
// output, so with anti-aliasing off there is nothing for them to act on. The same in both profiles (the caller passes the profile's own reading of
// fix.temporal_aa). One function so a rig can read the whole truth table; menu.cpp asks it and nothing else.
inline bool weaponsRowDim(bool antiAliasingOn) {
    return !antiAliasingOn;
}

// The first Allowlist row whose dotted key `allows` refuses, or null. `allows` is
// runtimeProfileAllowsKey in the product and a deliberately broken copy in the rig
// that proves this check can fail.
template <class Allows>
const FlatPageRow* flatPageFirstRefused(Allows allows) {
    for (const FlatPageRow& r : kFlatPageRows) {
        if (r.read != FlatRowRead::Allowlist) continue;
        char dotted[96];
        std::snprintf(dotted, sizeof(dotted), "%s.%s", r.section, r.key);
        if (!allows(static_cast<const char*>(dotted))) return &r;
    }
    for (const FlatPageRow& r : kFlatVisualRows) {
        if (r.read != FlatRowRead::Allowlist) continue;
        char dotted[96];
        std::snprintf(dotted, sizeof(dotted), "%s.%s", r.section, r.key);
        if (!allows(static_cast<const char*>(dotted))) return &r;
    }
    for (const FlatPageRow& r : kFlatDebugRows) {
        if (r.read != FlatRowRead::Allowlist) continue;
        char dotted[96];
        std::snprintf(dotted, sizeof(dotted), "%s.%s", r.section, r.key);
        if (!allows(static_cast<const char*>(dotted))) return &r;
    }
    return nullptr;
}

}  // namespace edvr
