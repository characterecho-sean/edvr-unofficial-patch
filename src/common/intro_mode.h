// The intro's THREE keys -- fix.intro_video, fix.intro_backdrop,
// fix.loading_dim -- each one function, parsed here for every module that
// consumes a slice of one.
//
// The intro work shipped as seven user-facing keys because it was built as
// seven campaign artifacts: the movie's size, lock and upscale; the menu
// backdrop; the loading wash, the loading panel and the splash dim. No
// user wants half of any of these, so the surface is now the function
// (per the fss_eye_sync precedent, src/common/eye_sync.h):
//
//   fix.intro_video = screen | stock
//     screen  the movie plays on the splash's own screen -- world-anchored,
//             splash-sized, FSR-resampled to the vscreen resolution (the
//             default)
//     stock   the game's own small head-locked rectangle
//     dev values: head (screen without the world anchor), sharp (screen
//     with the Catmull-Rom resample instead of FSR). Legacy spellings from
//     the retired keys parse silently: splash/world/fsr and any numeric
//     size mean screen; head/1/1.0 alone meant the game's own lock or
//     size. "head" is the dev value above, not a stock alias.
//
//   fix.intro_backdrop = splash | stock
//     splash  the menu keeps its nicer first picture, debanded (the
//             default)
//     stock   the game's own
//     dev value: smooth (the deband without keeping the big picture --
//     the retired menu_backdrop's middle option). splash supersedes it.
//
//   fix.loading_dim = screen | stock
//     screen  the loading dialogs' dimming happens on the SPLASH SCREEN
//             instead of across your whole view: the frosted wash is
//             removed, the full-view scrim is withheld, and the screen
//             itself steps back by the scrim's own tint (the default)
//     stock   the game's own full-view wash and scrim
//     dev values: wash (only the frosted-wash removal -- the key's old
//     "off"), panel (only the scrim withhold, no splash dim). Legacy
//     spellings: off means screen (its author wanted the dimming gone
//     from the view; screen is that, completed), fit (the retired
//     loading_panel's value) means screen.
//
// An unrecognised value is the DEFAULT with recognised=false -- the caller
// notes it once -- per getBool's philosophy: a typo must not switch a fix
// off. Header-only because six modules parse these three values.
#pragma once

#include <cstdlib>
#include <string>

namespace edvr {

namespace intro_detail {
inline std::string lowered(std::string v) {
    for (char& c : v) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return v;
}
}  // namespace intro_detail

struct IntroVideoMode {
    bool screen = true;      // splash-sized panel, derived from the rig
    bool worldLock = true;   // anchored to the game's forward
    int  upscale = 2;        // 0 stock | 1 sharp (Catmull-Rom) | 2 fsr
    bool recognised = true;
};

inline IntroVideoMode introVideoParse(std::string raw) {
    const std::string v = intro_detail::lowered(raw);
    IntroVideoMode m;
    if (v == "stock" || v == "off" || v == "0" || v == "1" || v == "1.0") {
        m.screen = false;
        m.worldLock = false;
        m.upscale = 0;
        return m;
    }
    if (v == "sharp") {
        m.upscale = 1;
        return m;
    }
    // The documented dev value: the splash-sized, resampled panel with the
    // world anchor OFF. It was listed in the stock branch, so it produced
    // full stock instead -- and there was no way at all to run the panel
    // without the world lock, which is the one A/B that isolates the
    // geometry from the rest of the intro fix.
    if (v == "head") {
        m.worldLock = false;
        return m;
    }
    if (v == "screen" || v == "on" || v == "splash" || v == "world" ||
        v == "fsr" || v.empty()) {
        return m;
    }
    // A legacy numeric size (intro_video_size = 2.0) meant "bigger than
    // stock": the intent survives as the full fix.
    const double n = atof(v.c_str());
    if (n > 1.0) return m;
    m.recognised = false;
    return m;
}

struct IntroBackdropMode {
    bool on = true;       // deband runs
    bool splash = true;   // and the nicer first picture is kept
    bool recognised = true;
};

inline IntroBackdropMode introBackdropParse(std::string raw) {
    const std::string v = intro_detail::lowered(raw);
    IntroBackdropMode m;
    if (v == "stock" || v == "off" || v == "0") {
        m.on = false;
        m.splash = false;
        return m;
    }
    if (v == "smooth") {
        m.splash = false;
        return m;
    }
    if (!(v == "splash" || v == "on" || v == "1" || v.empty())) {
        m.recognised = false;
    }
    return m;
}

struct LoadingDimMode {
    bool washOff = true;    // the frosted wash removed (scrim_fix)
    bool withhold = true;   // the full-view scrim withheld (loader_panel)
    bool splashDim = true;  // the screen steps back instead (splash_dim)
    bool recognised = true;
};

inline LoadingDimMode loadingDimParse(std::string raw) {
    const std::string v = intro_detail::lowered(raw);
    LoadingDimMode m;
    if (v == "stock" || v == "0") {
        m.washOff = false;
        m.withhold = false;
        m.splashDim = false;
        return m;
    }
    if (v == "wash") {
        m.withhold = false;
        m.splashDim = false;
        return m;
    }
    if (v == "panel") {
        m.splashDim = false;
        return m;
    }
    if (!(v == "screen" || v == "on" || v == "off" || v == "fit" ||
          v == "1" || v.empty())) {
        m.recognised = false;
    }
    return m;
}

}  // namespace edvr
