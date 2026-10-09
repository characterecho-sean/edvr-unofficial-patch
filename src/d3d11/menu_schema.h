// The in-headset menu's row table, as data (docs/settings-menu.md).
//
// Generated into menu_schema.inc by tools/gen_settings_schema.py from the
// two places every fact already lives -- the accessor call in the code (the
// type, the range, the default) and the comment block above the key in
// edvr.ini (the label, the choices, the one-line hint, and WHEN A CHANGE
// APPLIES) -- so a row here can never say something the ini does not. The
// installer's settings window is built from the same pass.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace edvr {

enum class MenuKind : unsigned char {
    Toggle,   // getBool: on / off, drawn as a switch
    Number,   // getInt / getFloat, stepped within its bounds, or typed
    Choice,   // an enumerated string: cycled
    Text,     // a free string: typed
    Hotkey,   // a hotkey.* value: the next key, pad button or HOTAS button pressed
              // is the value (hotkey_capture.h); never typed
};

enum class MenuTier : unsigned char {
    Fix,           // [fix], tagged `menu` on its ui: line
    Advanced,      // [advanced], the developer tier
    Experimental,  // [experimental], the developer tier
};

struct MenuRowDef {
    const char* section;
    const char* key;
    const char* label;     // the ui: label, or the key for a developer row
    const char* hint;      // the comment block's first sentence
    const char* detail;    // the whole comment block, for the row's tooltip
    MenuKind    kind;
    const char* shipped;   // the value edvr.ini ships with
    const char* lo;        // bounds, when known; empty otherwise
    const char* hi;
    int         precision; // decimals for a number
    const char* choices;   // "a|b=Label|c", empty for other kinds
    bool        percent;   // shown as a percentage, stored as a fraction
    // A per-headset list (`# ui: ... | headset`): the value is
    // `runtime/system:value` entries, of which the row shows and edits only
    // the worn headset's, and `lo`/`hi` bound one entry's value rather than
    // the whole string. fix.openxr_resolution. (The field-of-view trims were
    // the other such rows until 2026-09-29; they are ini-only now.)
    bool        headset;
    int         applies;   // 0 not documented, 1 live, 2 needs a game restart
    MenuTier    tier;
    const char* page;      // "performance" | "fixes" | "explorer_cam" | "hotkeys" | the section for developer rows
    const char* group;     // the ini's heading above it
    // How a NUMBER row is stepped and shown (the ui: line's `step`, `unit`, `zero`, `signed`; empty / false for every other row).
    const char* step;      // the arrow keys' step, in place of the range's twentieth
    const char* unit;      // the text after the number: "m", "ms"
    const char* zero;      // the word in brackets after a zero: "exact" reads "0 ms (exact)"
    bool        showSign;  // an explicit + on a positive number
};

// The arrow keys' step for a number row: the row's own `step` when it has one, else the range in about twenty steps, rounded to 1, 2 or 5 times a power
// of ten, else one (a tenth for a decimal). Pure, so a rig can hold the Explorer Cam page's 0.01 m and 10 ms to what the ini says.
inline double menuStepOf(const MenuRowDef& d) {
    if (d.step && d.step[0]) {
        const double s = std::atof(d.step);
        if (s > 0.0) return s;
    }
    const bool haveBounds = d.lo[0] && d.hi[0];
    if (!haveBounds) return d.precision > 0 ? 0.1 : 1.0;
    const double range = std::atof(d.hi) - std::atof(d.lo);
    if (!(range > 0.0)) return d.precision > 0 ? 0.1 : 1.0;
    const double raw = range / 20.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    double best = mag;
    for (double m : {1.0, 2.0, 5.0, 10.0}) {
        if (std::fabs(m * mag - raw) < std::fabs(best - raw)) best = m * mag;
    }
    if (d.precision == 0 && best < 1.0) best = 1.0;
    return best;
}

// What one press of Left or Right does to a number: a step (times `mult`: Shift is five) from where it is, snapped to the step grid so 0.15 + 0.01 is 0.16 and
// not 0.16000001 after a few presses, and held to the row's bounds. Pure.
inline double menuSteppedNumber(const MenuRowDef& d, double cur, int dir, int mult) {
    const double step = menuStepOf(d) * mult;
    double v = cur + dir * step;
    v = std::floor(v / step + 0.5) * step;
    if (d.lo[0] && v < std::atof(d.lo)) v = std::atof(d.lo);
    if (d.hi[0] && v > std::atof(d.hi)) v = std::atof(d.hi);
    return v;
}

// A number as the FILE holds it: `precision` decimals with the trailing zeros past the first trimmed (0.30 is 0.3, 1.00 is 1.0), or a whole number. What a
// step or a typed value writes; menuFormatNumber below is what the row SHOWS.
inline std::string menuFileNumber(double v, int precision) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f", precision > 0 ? precision : 0, v);
    std::string s(buf);
    if (precision > 0) {
        const size_t dot = s.find('.');
        if (dot != std::string::npos) {
            while (s.size() > dot + 2 && s.back() == '0') s.pop_back();
        }
    }
    return s;
}

// The text of a number row's value. A row with a unit shows its decimals as they are (+0.10 m, not +0.1 m, so a column of them lines up), a positive one
// with `signed` wears its +, and a zero with a `zero` word is followed by it in brackets (0 ms (exact)). A row with neither keeps the older rule: trailing
// zeros past the first decimal trimmed (0.30 reads 0.3). `percent` rows scale by a hundred.
inline std::string menuFormatNumber(const MenuRowDef& d, double v) {
    const bool shaped = (d.unit && d.unit[0]) || (d.zero && d.zero[0]) || d.showSign;
    if (d.percent) v *= 100.0;
    const int precision = d.percent ? 0 : (d.precision > 0 ? d.precision : 0);
    char buf[48];
    if (!shaped) {
        const std::string s = menuFileNumber(v, precision);
        return d.percent ? s + "%" : s;
    }
    // Round first, so -0.004 at two decimals is a zero and not "-0.00".
    const double scale = std::pow(10.0, precision);
    const double rounded = std::floor(std::fabs(v) * scale + 0.5) / scale;
    const bool isZero = rounded == 0.0;
    const bool negative = v < 0.0 && !isZero;
    std::snprintf(buf, sizeof(buf), "%s%.*f", negative ? "-" : (d.showSign && !isZero ? "+" : ""), precision, rounded);
    std::string s(buf);
    if (d.unit && d.unit[0]) s += std::string(" ") + d.unit;
    if (isZero && d.zero && d.zero[0]) s += std::string(" (") + d.zero + ")";
    return s;
}

}  // namespace edvr