// visual.star_glow: how strong the halo around a star is, as a percentage of the halo the game's own bloom draws.
//
//   visual.star_glow = 100 | 125 | 150 | 175 | 200
//     100  the game as it ships (the default); nothing runs
//     125 .. 200  a stronger halo, the same strength for every star in this version
//
// Flat only for now (flat_profile_keys.h lists the key; in VR nothing acts on it). This header is the key's reader and nothing else:
// the mechanism that draws the stronger halo is built apart from it. The key is live, so a consumer reads it again at each
// configure sweep.
//
// Exact text, as every choice key is read (ui_layer_math.h: uiQualityParse): "125" is a choice; "125.0", "1.25" and "125%" are not.
// An unrecognised value is the STOCK 100 with *recognised = false for the caller's one-time note -- a typo must not strengthen the
// picture (getBool's philosophy), and 100 runs nothing.
#pragma once

#include <cstring>
#include <string>

#include "config.h"

namespace edvr {

inline constexpr int kStarGlowStock = 100;

// The percentage a settings value names: 100, 125, 150, 175 or 200; anything else is kStarGlowStock.
inline int starGlowParse(const char* text, bool* recognised = nullptr) {
    if (recognised) *recognised = true;
    if (text) {
        static const char* const kChoices[] = {"100", "125", "150", "175", "200"};
        static const int kPercent[] = {100, 125, 150, 175, 200};
        for (size_t i = 0; i < sizeof(kChoices) / sizeof(kChoices[0]); ++i) {
            if (std::strcmp(text, kChoices[i]) == 0) return kPercent[i];
        }
    }
    if (recognised) *recognised = false;
    return kStarGlowStock;
}

// The percentage the settings file asks for.
inline int starGlowPercent(const Config& cfg, bool* recognised = nullptr) {
    return starGlowParse(cfg.getString("visual.star_glow", "100").c_str(), recognised);
}

}  // namespace edvr
