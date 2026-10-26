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
        std::strcmp(key, "diag.flat_origin_capture") == 0 ||
        // The refusal view (flat_runtime.cpp): paints the prep's per-pixel class over the resolved image. A flat panel row on the Debug tab.
        // Unlisted, getBool answers false here whatever the file says.
        std::strcmp(key, "advanced.flat_refusal_view") == 0 ||
        // The CPU timing in the log (flat_runtime.cpp, flat_cpu.h Census::setClocks): the census's clocks, off unless this is on. A flat panel row
        // on the Debug tab. Unlisted, getBool answers false here whatever the file says, and the timing could never be turned on from the panel.
        std::strcmp(key, "advanced.flat_cpu_timing") == 0 ||
        std::strcmp(key, "hotkey.menu") == 0 ||
        std::strcmp(key, "fix.temporal_aa_model") == 0 ||
        std::strcmp(key, "hotkey.dump_draws") == 0 ||
        // TEMPORARY (GPU pass profile, 2026-10-12): the flat profile reads the same key. Remove with the arc.
        std::strcmp(key, "hotkey.gpu_pass_profile") == 0 ||
        std::strcmp(key, "fix.render_sharpness") == 0 ||
        // The launch movie (intro_skip.cpp), flat's one value being `skip`: the game's open of the ident file is answered "not found". Every
        // other value plays the movie (intro_mode.h: introVideoParseFor), so the VR panel and resample stay inert in flat. Unlisted, getString
        // answers "off" here and the skip never arms.
        std::strcmp(key, "visual.intro_video") == 0 ||
        // The star glow (visual.star_glow, star_glow.h): a percentage of the halo the game's own bloom draws around a star, flat only for now.
        // The Visual tab's row. Unlisted, getString answers "off" here, which the reader takes for the stock 100.
        std::strcmp(key, "visual.star_glow") == 0 ||
        // The AA comparison key (aa_compare.cpp): turns EDVR's temporal AA off for a side-by-side look and back, runtime only. Unlisted, a hotkey
        // key reads empty in flat, which is unbound.
        std::strcmp(key, "hotkey.compare_aa") == 0 ||
        // The interface quality (flat panel row; ui_panel_scale.cpp's flat factor and the flat UI layer). The VR layer reads it
        // too, but stays inert in flat: it is live only with fix.temporal_aa on through this gate, which refuses that key.
        std::strcmp(key, "fix.ui_quality") == 0 ||
        // The weapons fire and the shield ripple after the upscale (ui_layer.cpp's weapons and ripple halves, flat_ui_layer.cpp's adapter), on in flat as in VR:
        // on | plain | off. Unlisted, getString answers "off" here and neither is ever taken.
        std::strcmp(key, "fix.sharp_weapons") == 0 ||
        // The optimized station (optimized_station.cpp), on in flat as in VR (built v14.34). Unlisted, getString answers "off" here and the station never starts.
        std::strcmp(key, "fix.optimized_stations") == 0 ||
        // The flat jitter cycle's length (flat_runtime.cpp, FlatLivePhase::phaseCount; temporal_math.h). Developer tier, no flat panel
        // row. Unlisted, getInt answers 0 here whatever the file says, which the reader takes for out of range and reads as 8.
        std::strcmp(key, "advanced.temporal_aa_jitter_phases") == 0 ||
        // The shader dump (device_hook.cpp): every VS, PS and CS the game creates, to edvr_logs\shaders by hash. Flat's post
        // chain (bloom, tone, copy) is in no effects archive, so a running game is the only place its bytecode can be read.
        // Developer tier, no flat panel row. Unlisted, getBool answers false here whatever the file says.
        std::strcmp(key, "advanced.glare_shader_dump") == 0 ||
        // The HUD layer's edge smoothing (flat_ui_layer.cpp, ui_layer_smaa.h): on | off, off by default, read live a frame at a time. The HUD Anti-Aliasing row
        // of the flat panel's Performance tab, directly under UI quality. Unlisted, getBool answers false here whatever the file says, which reads as off, so the
        // row could never turn it on, and the 30 s "flat ui layer smaa:" line shows key=off.
        std::strcmp(key, "fix.hud_antialiasing") == 0 ||
        // The journal watcher, for the System Map's GuiFocus (journal_watch.h: journalFlatGuiFocus). Its other accessors answer
        // flat with the no-journal values, so enabling the watcher here changes no other flat behaviour. journal_dir names the
        // Saved Games folder when the default is wrong; unlisted, it reads "off" and the watcher looks in a folder named "off".
        std::strcmp(key, "d3d11.journal_watch") == 0 ||
        std::strcmp(key, "d3d11.journal_dir") == 0;
}

}  // namespace edvr
