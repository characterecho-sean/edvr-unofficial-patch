// The in-headset menu's row table, as data (docs/settings-menu.md).
//
// Generated into menu_schema.inc by tools/gen_settings_schema.py from the
// two places every fact already lives -- the accessor call in the code (the
// type, the range, the default) and the comment block above the key in
// edvr.ini (the label, the choices, the one-line hint, and WHEN A CHANGE
// APPLIES) -- so a row here can never say something the ini does not. The
// installer's settings window is built from the same pass.
#pragma once

namespace edvr {

enum class MenuKind : unsigned char {
    Toggle,   // getBool: on / off, drawn as a switch
    Number,   // getInt / getFloat, stepped within its bounds, or typed
    Choice,   // an enumerated string: cycled
    Text,     // a free string: typed
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
    int         applies;   // 0 not documented, 1 live, 2 needs a game restart
    MenuTier    tier;
    const char* page;      // "performance" | "fixes" | the section for developer rows
    const char* group;     // the ini's heading above it
};

}  // namespace edvr
