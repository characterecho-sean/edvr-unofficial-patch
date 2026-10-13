// The settings keys the flat profile reads. One list, shared by the runtime's allow-list (runtime_profile.h's
// runtimeProfileAllowsKey) and the installer's Flat view (settings.cpp), so the window never offers a row the flat runtime
// would ignore. Pure and free of any Windows API, so both can include it.
#pragma once

#include <cstring>

namespace edvr {

// True for the keys the flat profile reads from its settings file: every log.* key, advanced.real_dll, and the listed flat keys.
// Keys are dotted "section.key". A null key is never read.
inline bool flatProfileReadsKey(const char* key) {
    if (!key) return false;
    if (std::strncmp(key, "log.", 4) == 0 || std::strcmp(key, "advanced.real_dll") == 0) return true;
    return std::strcmp(key, "advanced.d3d11_fixes") == 0 ||
        std::strcmp(key, "advanced.input_gate") == 0 ||
        std::strcmp(key, "advanced.flat_camera_producer_probe") == 0 ||
        std::strcmp(key, "hotkey.menu") == 0 ||
        std::strcmp(key, "fix.temporal_aa_model") == 0 ||
        std::strcmp(key, "hotkey.dump_draws") == 0 ||
        std::strcmp(key, "fix.render_sharpness") == 0 ||
        // The interface quality (flat panel row; ui_panel_scale.cpp's flat factor and the flat UI layer). The VR layer reads it
        // too, but stays inert in flat: it is live only with fix.temporal_aa on through this gate, which refuses that key.
        std::strcmp(key, "fix.ui_quality") == 0 ||
        // The flat jitter cycle's length (flat_runtime.cpp, FlatLivePhase::phaseCount; temporal_math.h). Developer tier, no flat panel
        // row. Unlisted, getInt answers 0 here whatever the file says, which the reader takes for out of range and reads as 8.
        std::strcmp(key, "advanced.temporal_aa_jitter_phases") == 0 ||
        // The journal watcher, for the System Map's GuiFocus (journal_watch.h: journalFlatGuiFocus). Its other accessors answer
        // flat with the no-journal values, so enabling the watcher here changes no other flat behaviour. journal_dir names the
        // Saved Games folder when the default is wrong; unlisted, it reads "off" and the watcher looks in a folder named "off".
        std::strcmp(key, "d3d11.journal_watch") == 0 ||
        std::strcmp(key, "d3d11.journal_dir") == 0;
}

}  // namespace edvr
