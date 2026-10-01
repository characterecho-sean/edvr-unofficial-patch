// The flat panel's graphics-wrapper note (2026-09-30).
//
// A field user (docs/design-flat-temporal-aa-2026-09-23.md, section 80, the fourth record) lost a third of their
// frame rate to a temporal mode and got it back by moving ReShade aside. ReShade installs a wrapper over the game's
// immediate context: every method EDVR calls on it, from a shader set to a render-target read, goes through the
// wrapper first, and a treated frame makes thousands of them. Nothing on screen said so. The hook-mode probe
// (d3d11_proxy.cpp, contextHookModeFor) already knows whose code backs the context's methods; this is what it tells
// the person, in the F8 panel, while a temporal mode is selected and only then.
//
// Two decisions, both pure so the rig can drive them:
//   flatWrapperFile     whether a wrapper handles the context, and the file it lives in. Only when the probe found the
//                       methods outside Windows' d3d11.dll (InPlace) AND the session hooks in place: a live copy is the
//                       runtime's own code (EDHM and 3Dmigoto sit at 96 of 96 there and cost nothing), a forced mode is
//                       the person's own experiment.
//   flatComposeWrapperNote  the words, wrapped to the panel's note width by the same ruler the settings warning uses.
#pragma once

#include <cstdio>

#include "../common/vtable_hook.h"   // HookMode
#include "flat_elite_settings.h"     // FlatSettingsWarning, FlatMeasureFn, flatWrapWarning

namespace edvr {

// `owner`: the file name (no path) of the module that backs most of the context's methods when Windows' d3d11.dll
// does not, or empty when none was named. Null means no wrapper to speak of.
inline const char* flatWrapperFile(HookMode mode, HookMode probed, const char* owner) {
    if (mode != HookMode::InPlace || probed != HookMode::InPlace) return nullptr;
    return owner && *owner ? owner : nullptr;
}

// The note as wrapped lines: none unless a temporal mode is selected and a wrapper was named.
inline void flatComposeWrapperNote(bool temporalSelected, const char* file, int widthPx, FlatMeasureFn measure,
                                   void* context, FlatSettingsWarning* out) {
    *out = FlatSettingsWarning{};
    if (!temporalSelected || !file || !*file) return;
    char text[192];
    std::snprintf(text, sizeof(text), "%s handles every graphics call (likely ReShade): anti-aliasing costs more frame "
                  "time with it.", file);
    flatWrapWarning(text, widthPx, measure, context, out);
}

}  // namespace edvr
