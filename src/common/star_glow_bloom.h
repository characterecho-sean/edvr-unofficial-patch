// What the menus say when visual.star_glow cannot do anything: Elite's own Bloom graphics option is off.
//
// The star glow strengthens the halo that Elite's bloom draws around a star (src/common/star_glow.h). With Bloom off in the game's graphics
// options there is no halo to strengthen and the setting changes nothing, and nothing told the player. The state is read from Elite's own
// graphics files (BloomQuality, 0 = off; src/d3d11/star_glow_bloom_reader.h), never from the draw path: the draw path's bloom-off count only
// moves with a star in view and also counts frames where a depth-of-field pass came first.
//
// This header is the pure half: the tri-state, the one condition, where each menu says it, and the log lines. It names no Windows type and
// reads no file, so tools\flat_sharpen_test and tools\terrain_checkerboard_test drive the very functions menu.cpp calls.
//
//   state    on, off or unknown (no folder, no file, an unreadable file, a user-made preset, a tag that is not there or holds no number)
//   hint     the setting is above 100 AND Bloom is known to be off. Unknown and on say nothing: a hint that may be wrong is worse than none.
//   where    flat F8 panel: a note under the Visual tab's rows. VR menu: the hint line while the Star glow row is highlighted. Never both,
//            and the row itself stays as it is: a dimmed row stops responding.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace edvr {
namespace sgb {

// The stock percentage: nothing runs at it, and nothing is read or said (menu.cpp static_asserts it equals kStarGlowStock).
inline constexpr int kStockPercent = 100;

enum class Bloom : unsigned char { Unknown = 0, Off = 1, On = 2 };

// What one read found. The strings are for the log only (one line each).
struct Reading {
    Bloom state = Bloom::Unknown;
    char preset[48] = {};    // the preset Settings.xml names, "" when it names none
    char source[96] = {};    // the file the answer came from: Custom.4.4.fxcfg or OptionDefaults\VRHigh.fxcfg, "" when none was chosen
    char value[24] = {};     // BloomQuality as written, "" when none
    char reason[220] = {};   // for Unknown: why, in words
};

// BloomQuality's text: a whole number (0 off, above 0 on), also in the true/false dialect (flat_elite_settings.h flatXmlInt reads the same).
// A negative number, a fraction, a blank and anything else is Unknown: nothing is guessed.
inline Bloom bloomOfText(const char* text) {
    if (!text || !*text) return Bloom::Unknown;
    if (_stricmp(text, "true") == 0) return Bloom::On;
    if (_stricmp(text, "false") == 0) return Bloom::Off;
    char* end = nullptr;
    const long number = std::strtol(text, &end, 10);
    if (end == text || *end != 0) return Bloom::Unknown;
    if (number == 0) return Bloom::Off;
    return number > 0 ? Bloom::On : Bloom::Unknown;
}

// ---- the hint --------------------------------------------------------------------------------------------------------------
inline constexpr char kHint[] =
    "Bloom is off in Elite's graphics options. Star glow strengthens the game's bloom, so it changes nothing until Bloom is on.";

// The one condition. Null when there is nothing to say.
inline const char* hint(int percent, Bloom state) {
    return percent > kStockPercent && state == Bloom::Off ? kHint : nullptr;
}
// The flat F8 panel's note, under the Visual tab's rows and on no other tab. Not in VR.
inline const char* tabNote(bool flatProfile, bool visualTab, int percent, Bloom state) {
    return flatProfile && visualTab ? hint(percent, state) : nullptr;
}
// VR's hint line, while the Star glow row is the highlighted one. Not in the flat panel, whose note already says it.
inline const char* rowHint(bool flatProfile, bool starGlowRowHighlighted, int percent, Bloom state) {
    return !flatProfile && starGlowRowHighlighted ? hint(percent, state) : nullptr;
}

// ---- the log ---------------------------------------------------------------------------------------------------------------
// One line per outcome, each starting "star glow: Elite's Bloom": no line at all means the code never ran (the setting stayed at 100, or the menu
// was never configured), and each of the three outcomes reads differently from the others.
inline int formatLog(char* out, size_t size, const Reading& r) {
    switch (r.state) {
    case Bloom::Off:
        return std::snprintf(out, size, "star glow: Elite's Bloom is off (preset %s, BloomQuality %s): the glow changes nothing until Bloom is on",
                             r.preset, r.value);
    case Bloom::On:
        return std::snprintf(out, size, "star glow: Elite's Bloom is on (preset %s, BloomQuality %s)", r.preset, r.value);
    case Bloom::Unknown:
    default:
        return std::snprintf(out, size, "star glow: Elite's Bloom setting is unknown (%s)", r.reason[0] ? r.reason : "no reason was recorded");
    }
}

}  // namespace sgb
}  // namespace edvr
