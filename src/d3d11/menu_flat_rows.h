// The rows of the flat profile's F8 panel ("Flat graphics"), in one table.
//
// menu.cpp builds the page from it, and tools\flat_sharpen_test reads it to
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
};
inline constexpr size_t kFlatPageRowCount = sizeof(kFlatPageRows) / sizeof(kFlatPageRows[0]);

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
    return nullptr;
}

}  // namespace edvr
