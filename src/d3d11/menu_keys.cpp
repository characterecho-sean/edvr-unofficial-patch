#include "menu_keys.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../common/hotkey.h"

namespace edvr {

// Section 1 of the design (docs/settings-menu.md, "Your Elite keys"), in
// the order that decides a duplicate. MEASURED across all 30 stock schemes
// and the maintainer's Custom.4.2.binds: no UI_*/Cycle* keyboard slot
// carries a keyboard <Modifier>, so the chord refusal (R6) costs nobody
// today. UI_Toggle (stock '=') is deliberately absent until its label in
// Options > Controls > Interface Mode has been read (BELIEVED: it expands a
// nested list item and is not a tab or page key).
const MenuUiElement kMenuUiElements[kMenuUiElementCount] = {
    {"UI_Up", kNavUp, true},
    {"UI_Down", kNavDown, true},
    {"UI_Left", kNavLeft, true},
    {"UI_Right", kNavRight, true},
    {"UI_Select", kNavSelect, false},
    {"UI_Back", kNavBack, false},
    {"CycleNextPanel", kNavPageNext, false},
    {"CyclePreviousPanel", kNavPagePrev, false},
    {"CycleNextPage", kNavReadOn, true},
    {"CyclePreviousPage", kNavReadBack, true},
};

namespace {

// The fixed key's own name per nav, for the legend when no alias feeds it.
// PagePrev is Shift+Tab, which the legend spells "Tab" like its twin.
const char* const kFixedNavName[kNavCount] = {"Up",  "Down", "Left", "Right", "Enter", "Esc", "Tab",
                                              "Tab", "PgDn", "PgUp", "Home",  "End",   "R"};

// What the menu's key on a vk does, for R2's "the menu's <role> key keeps
// its meaning" -- by nav, so the text follows whatever fixed table the
// caller passed rather than a second copy of initKeys.
const char* fixedRole(MenuNav nav) {
    switch (nav) {
        case kNavUp: case kNavDown: return "highlight";
        case kNavLeft: case kNavRight: return "change";
        case kNavSelect: return "select";
        case kNavBack: return "close";
        case kNavPageNext: case kNavPagePrev: return "page";
        case kNavReadOn: case kNavReadBack: return "read";
        case kNavHome: return "first-row";
        case kNavEnd: return "last-row";
        case kNavReset: return "reset";
        default: return "own";
    }
}

// The groups the log and the Status row speak in, in the order they are
// spoken: a pair is "<prev>/<next>", the way the legend pairs them.
struct Group {
    const char* label;
    MenuNav     first;
    MenuNav     second;   // kNavCount for a single key
};
constexpr Group kGroups[] = {
    {"pick", kNavUp, kNavDown},
    {"change", kNavLeft, kNavRight},
    {"select", kNavSelect, kNavCount},
    {"page", kNavPagePrev, kNavPageNext},
    {"read on", kNavReadBack, kNavReadOn},
    {"back", kNavBack, kNavCount},
};
constexpr int kGroupCount = static_cast<int>(sizeof(kGroups) / sizeof(kGroups[0]));

const Group* groupOf(MenuNav nav) {
    for (const Group& g : kGroups) {
        if (g.first == nav || g.second == nav) return &g;
    }
    return nullptr;
}

void append(char* out, size_t n, const char* s) {
    if (!out || !n || !s) return;
    const size_t len = strlen(out);
    if (len + 1 >= n) return;
    snprintf(out + len, n - len, "%s", s);
}

void copyStr(char* dst, size_t n, const char* s) {
    if (!dst || !n) return;
    snprintf(dst, n, "%s", s ? s : "");
}

bool isCtrlVk(int vk) { return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL; }
bool isAltVk(int vk) { return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU; }
bool isShiftVk(int vk) { return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT; }

void recordSkip(MenuAliasTable* out, const MenuAliasInput& e, const char* name, MenuAliasSkip reason,
                int vk, MenuNav fixedNav, bool summonChord) {
    if (out->skippedCount >= kMenuAliasMax) return;
    MenuAliasSkipped& k = out->skipped[out->skippedCount++];
    copyStr(k.element, sizeof(k.element), e.element);
    copyStr(k.name, sizeof(k.name), name);
    k.nav = e.nav;
    k.reason = reason;
    k.vk = vk;
    k.fixedNav = fixedNav;
    k.summonChord = summonChord;
}

// Which navs an adopted or same-as-fixed slot has named: the groups worth
// speaking of in the log and on the Status row.
void navsFed(const MenuAliasTable& t, bool fed[kNavCount]) {
    for (int i = 0; i < kNavCount; ++i) fed[i] = false;
    for (int i = 0; i < t.count; ++i) fed[t.alias[i].nav] = true;
    for (int i = 0; i < t.skippedCount; ++i) {
        if (t.skipped[i].reason == kSkipSameAsFixed) fed[t.skipped[i].nav] = true;
    }
}

bool groupFed(const Group& g, const bool fed[kNavCount]) {
    return fed[g.first] || (g.second < kNavCount && fed[g.second]);
}

void groupName(const MenuAliasTable& t, const Group& g, char* out, size_t n) {
    if (g.second < kNavCount) snprintf(out, n, "%s/%s", t.navName[g.first], t.navName[g.second]);
    else snprintf(out, n, "%s", t.navName[g.first]);
}

}  // namespace

// ---------------------------------------------------------------------------
// The tracker

int keyRepeatStep(KeyRepeat& k, bool down, uint64_t now, bool act) {
    if (!down) {
        k.down = false;
        k.nextMs = 0;
        return 0;
    }
    if (!act) {
        // Tracked and swallowed: parked until the key is released. A press
        // made while the menu could not take it is not a press the menu
        // will take later.
        k.down = true;
        k.nextMs = kKeyRepeatParked;
        return 0;
    }
    if (!k.down) {
        k.down = true;
        k.nextMs = now + kKeyRepeatFirstMs;
        return 1;
    }
    if (k.nextMs == kKeyRepeatParked) return 0;   // held since a swallowed state: wait for release
    if (now >= k.nextMs) {
        k.nextMs = now + kKeyRepeatMs;
        return 1;
    }
    return 0;
}

void keyRepeatPrime(KeyRepeat& k, bool down) {
    k.down = down;
    k.nextMs = down ? kKeyRepeatParked : 0;
}

// ---------------------------------------------------------------------------
// The rules

void menuAliasResolve(const MenuAliasInput* in, int n, const MenuFixedKeys& fixed,
                      MenuAliasTable* out) {
    if (!out) return;
    // Zeroed whole, padding included: "read the same as before" is a memcmp.
    memset(out, 0, sizeof(*out));
    bool navNamed[kNavCount] = {};
    if (n > kMenuAliasMax / 2) n = kMenuAliasMax / 2;
    for (int i = 0; in && i < n; ++i) {
        const MenuAliasInput& e = in[i];
        if (e.nav >= kNavCount) continue;
        const int slots = e.slots < 0 ? 0 : e.slots > 2 ? 2 : e.slots;
        for (int s = 0; s < slots; ++s) {
            const char* binding = e.binding[s] ? e.binding[s] : "";
            const char* elite = e.eliteName[s] ? e.eliteName[s] : "";
            // R9: a gamepad or mouse slot is recorded so the log can say
            // where the key went, and never adopted.
            if (!e.keyboard[s]) {
                recordSkip(out, e, elite[0] ? elite : "?", kSkipNotKeyboard, 0, kNavCount, false);
                continue;
            }
            // R8: a keyboard key elite_binds could not name (issue #8's
            // distinction from "not on a keyboard key"), reported with
            // Elite's own spelling so a log can be acted on.
            if (!binding[0]) {
                recordSkip(out, e, elite[0] ? elite : "?", kSkipUnnamed, 0, kNavCount, false);
                continue;
            }
            // R6: the alias poll is one bare vk with no modifier
            // arithmetic, so a chorded slot is refused rather than fired
            // on its key alone.
            if (e.chorded[s]) {
                recordSkip(out, e, binding, kSkipChord, 0, kNavCount, false);
                continue;
            }
            uint32_t mods = 0;
            const int vk = virtualKeyFromName(binding, &mods);
            if (vk <= 0 || vk > 255) {
                recordSkip(out, e, elite[0] ? elite : binding, kSkipUnnamed, 0, kNavCount, false);
                continue;
            }
            // A binding string that carries a modifier is a chord whatever
            // the slot's flag says; the same refusal, from the other side.
            if (mods != 0) {
                recordSkip(out, e, binding, kSkipChord, vk, kNavCount, false);
                continue;
            }
            char name[12];
            menuKeyDisplayName(vk, name, sizeof(name));
            // R4: Shift is the menu's own modifier (Shift+Tab, Shift+arrow).
            if (isShiftVk(vk)) {
                recordSkip(out, e, name, kSkipShiftKey, vk, kNavCount, false);
                continue;
            }
            // R3: the summon key itself, or a modifier that is half of its
            // chord -- a Ctrl-as-Back with hotkey.menu = CTRL+F8 would close
            // the menu on the first half of the chord that opens it.
            if (fixed.summonVk != 0 && vk == fixed.summonVk) {
                recordSkip(out, e, name, kSkipSummonKey, vk, kNavCount, false);
                continue;
            }
            if ((isCtrlVk(vk) && (fixed.summonMods & kHotkeyCtrl)) ||
                (isAltVk(vk) && (fixed.summonMods & kHotkeyAlt))) {
                recordSkip(out, e, name, kSkipSummonKey, vk, kNavCount, true);
                continue;
            }
            // R1 / R2: one of the menu's own keys. The same meaning: the
            // fixed key already does it, and polling the vk twice is what
            // would fire two actions -- but the slot still names the legend
            // (stock UI_Select = Space reads "Space select"). Another
            // meaning: the fixed, documented one wins, and the log says so.
            int fixedNav = -1;
            for (int f = 0; f < fixed.count && f < 16; ++f) {
                if (fixed.vks[f] == vk) {
                    fixedNav = fixed.navs[f];
                    break;
                }
            }
            if (fixedNav < 0 && fixed.escapeVk != 0 && vk == fixed.escapeVk) fixedNav = kNavBack;
            if (fixedNav >= 0) {
                const bool same = fixedNav == e.nav;
                recordSkip(out, e, name, same ? kSkipSameAsFixed : kSkipFixedWins, vk,
                           static_cast<MenuNav>(fixedNav), false);
                if (same && !navNamed[e.nav]) {
                    navNamed[e.nav] = true;
                    copyStr(out->navName[e.nav], sizeof(out->navName[e.nav]), name);
                }
                continue;
            }
            // R5: an EDVR hotkey on the key is polled every frame by
            // device_hook whether or not the menu is open (Hotkey::pressed),
            // so a shared vk would fire in two places.
            bool registered = false;
            for (int r = 0; r < fixed.registeredCount && r < 16; ++r) {
                if (fixed.registered[r] == vk) {
                    registered = true;
                    break;
                }
            }
            if (registered) {
                recordSkip(out, e, name, kSkipEdvrHotkey, vk, kNavCount, false);
                continue;
            }
            // R7: a key an earlier slot took -- Primary == Secondary, or two
            // elements on one key. First in table order wins.
            bool taken = false;
            for (int a = 0; a < out->count; ++a) {
                if (out->alias[a].vk == vk) {
                    taken = true;
                    break;
                }
            }
            if (taken) {
                recordSkip(out, e, name, kSkipDuplicate, vk, kNavCount, false);
                continue;
            }
            if (out->count >= kMenuAliasMax) continue;   // ten elements x two slots: never
            MenuAlias& a = out->alias[out->count++];
            a.vk = vk;
            a.nav = e.nav;
            a.repeats = e.repeats;
            a.modifierMain = e.modifierMain[s] || isCtrlVk(vk) || isAltVk(vk);
            copyStr(a.name, sizeof(a.name), name);
            copyStr(a.element, sizeof(a.element), e.element);
            copyStr(a.eliteName, sizeof(a.eliteName), elite);
            if (!navNamed[e.nav]) {
                navNamed[e.nav] = true;
                copyStr(out->navName[e.nav], sizeof(out->navName[e.nav]), name);
            }
        }
    }
    for (int nav = 0; nav < kNavCount; ++nav) {
        if (!navNamed[nav]) copyStr(out->navName[nav], sizeof(out->navName[nav]), kFixedNavName[nav]);
    }
    out->adopted = out->count > 0;
}

bool menuAliasMayAct(bool gateHoldsGameKeyboard, bool editing, bool statusPage) {
    return gateHoldsGameKeyboard && !editing && !statusPage;
}

bool menuAliasFollowsTab(MenuNav nav) { return nav == kNavPageNext || nav == kNavPagePrev; }

bool menuAliasMayActNav(MenuNav nav, bool gateHoldsGameKeyboard, bool editing, bool statusPage) {
    // A page key is Tab: it acts wherever Tab does, which on the shared
    // pages means it reaches the ship as well, exactly as Tab does there.
    // Typing is the one state that holds it, since the arrows and Tab are
    // the editor's then.
    if (menuAliasFollowsTab(nav)) return !editing;
    return menuAliasMayAct(gateHoldsGameKeyboard, editing, statusPage);
}

// ---------------------------------------------------------------------------
// Names and text

void menuKeyDisplayName(int vk, char* out, size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        snprintf(out, n, "%c", static_cast<char>(vk));
        return;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(out, n, "F%d", vk - VK_F1 + 1);
        return;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(out, n, "Num %d", vk - VK_NUMPAD0);
        return;
    }
    struct Named { int vk; const char* name; };
    // Nine characters at most ("Backspace"): the legend pairs two of these
    // with a slash and has 25.7 em for five items.
    static const Named kNamed[] = {
        {VK_BACK, "Backspace"},  {VK_DELETE, "Del"},      {VK_INSERT, "Ins"},
        {VK_PRIOR, "PgUp"},      {VK_NEXT, "PgDn"},       {VK_HOME, "Home"},
        {VK_END, "End"},         {VK_TAB, "Tab"},         {VK_RETURN, "Enter"},
        {VK_SPACE, "Space"},     {VK_ESCAPE, "Esc"},      {VK_UP, "Up"},
        {VK_DOWN, "Down"},       {VK_LEFT, "Left"},       {VK_RIGHT, "Right"},
        {VK_LSHIFT, "L-Shift"},  {VK_RSHIFT, "R-Shift"},  {VK_LCONTROL, "L-Ctrl"},
        {VK_RCONTROL, "R-Ctrl"}, {VK_LMENU, "L-Alt"},     {VK_RMENU, "R-Alt"},
        {VK_SHIFT, "Shift"},     {VK_CONTROL, "Ctrl"},    {VK_MENU, "Alt"},
        {VK_DECIMAL, "Num ."},   {VK_SUBTRACT, "Num -"},  {VK_ADD, "Num +"},
        {VK_MULTIPLY, "Num *"},  {VK_DIVIDE, "Num /"},    {VK_PAUSE, "Pause"},
        {VK_SCROLL, "ScrLk"},    {VK_NUMLOCK, "NumLock"}, {VK_CAPITAL, "CapsLock"},
        {VK_SNAPSHOT, "PrtSc"},
    };
    for (const Named& e : kNamed) {
        if (e.vk == vk) {
            snprintf(out, n, "%s", e.name);
            return;
        }
    }
    // The punctuation row, as the character the active layout types for
    // it (MEASURED: 0xDB -> '[', 0xBB -> '='). The high bit marks a dead
    // key; the low word is the character.
    if (vk > 0 && vk < 256) {
        const UINT ch = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) & 0xFFFFu;
        if (ch > 0x20 && ch < 0x7F) {
            snprintf(out, n, "%c", static_cast<char>(ch));
            return;
        }
    }
    snprintf(out, n, "0x%02X", vk & 0xFF);
}

void menuAliasSummary(const MenuAliasTable& t, char* out, size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    bool fed[kNavCount];
    navsFed(t, fed);
    bool first = true;
    for (int g = 0; g < kGroupCount; ++g) {
        const Group& grp = kGroups[g];
        if (!groupFed(grp, fed)) continue;
        char item[160];
        char name[32];
        groupName(t, grp, name, sizeof(name));
        snprintf(item, sizeof(item), "%s%s %s", first ? "" : ", ", grp.label, name);
        first = false;
        // A modifier key used as a key is named as such where it is the
        // group's own name, so the log says what "L-Ctrl" means.
        for (int i = 0; i < t.count; ++i) {
            const MenuAlias& a = t.alias[i];
            if (!a.modifierMain) continue;
            if (a.nav != grp.first && a.nav != grp.second) continue;
            if (strcmp(a.name, t.navName[a.nav]) != 0) continue;
            char note[96];
            snprintf(note, sizeof(note), " (%s, a modifier key used as a key; the menu only)",
                     a.eliteName[0] ? a.eliteName : a.name);
            append(item, sizeof(item), note);
            break;
        }
        // The extra adopted keys for the group, in table order.
        bool anyAlso = false;
        for (int i = 0; i < t.count; ++i) {
            const MenuAlias& a = t.alias[i];
            if (a.nav != grp.first && a.nav != grp.second) continue;
            if (strcmp(a.name, t.navName[a.nav]) == 0) continue;
            append(item, sizeof(item), anyAlso ? ", " : " (also ");
            append(item, sizeof(item), a.name);
            anyAlso = true;
        }
        if (anyAlso) append(item, sizeof(item), ")");
        append(out, n, item);
    }
}

void menuAliasSkippedText(const MenuAliasTable& t, char* out, size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    bool first = true;
    for (int i = 0; i < t.skippedCount; ++i) {
        const MenuAliasSkipped& k = t.skipped[i];
        // An unnamed key is the log's own "please report" line, not a skip.
        if (k.reason == kSkipUnnamed) continue;
        const Group* grp = groupOf(k.nav);
        char why[96];
        switch (k.reason) {
            case kSkipSameAsFixed:
                snprintf(why, sizeof(why), "already the menu's key");
                break;
            case kSkipFixedWins:
                snprintf(why, sizeof(why), "the menu's %s key keeps its meaning", fixedRole(k.fixedNav));
                break;
            case kSkipSummonKey:
                snprintf(why, sizeof(why), "%s", k.summonChord ? "half of your menu chord (hotkey.menu)"
                                                              : "it is your menu key (hotkey.menu)");
                break;
            case kSkipShiftKey:
                snprintf(why, sizeof(why), "Shift is the menu's own modifier");
                break;
            case kSkipEdvrHotkey:
                snprintf(why, sizeof(why), "an EDVR hotkey is on that key");
                break;
            case kSkipChord:
                snprintf(why, sizeof(why), "a chord -- the menu adopts bare keys only");
                break;
            case kSkipDuplicate: {
                const char* winner = "another element";
                for (int a = 0; a < t.count; ++a) {
                    if (t.alias[a].vk == k.vk) {
                        winner = t.alias[a].element;
                        break;
                    }
                }
                snprintf(why, sizeof(why), "already adopted for %s", winner);
                break;
            }
            case kSkipNotKeyboard:
            default:
                snprintf(why, sizeof(why), "%s",
                         strncmp(k.name, "GamePad", 7) == 0 ? "on your gamepad"
                         : strncmp(k.name, "Mouse", 5) == 0 ? "on your mouse"
                                                            : "on a controller");
                break;
        }
        char item[192];
        snprintf(item, sizeof(item), "%s%s (%s; %s)", first ? "" : ", ", k.name,
                 grp ? grp->label : "menu", why);
        // An item that does not fit is not cut mid-word and silently
        // followed by nothing: the list says it stopped, so a log cannot
        // read a missing key as "not skipped".
        const size_t len = strlen(out);
        if (len + strlen(item) + 1 > n) {
            const char* mark = first ? "..." : ", ...";
            if (len + strlen(mark) + 1 <= n) append(out, n, mark);
            return;
        }
        first = false;
        append(out, n, item);
    }
}

void menuAliasStatusValue(const MenuAliasTable& t, char* out, size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    bool fed[kNavCount];
    navsFed(t, fed);
    bool first = true;
    for (int g = 0; g < kGroupCount; ++g) {
        const Group& grp = kGroups[g];
        if (!groupFed(grp, fed)) continue;
        char name[32];
        groupName(t, grp, name, sizeof(name));
        if (!first) append(out, n, " ");
        append(out, n, name);
        first = false;
    }
}

void menuComposeTipAction(const char* const navName[kNavCount], bool typed, char* out, size_t n) {
    if (!out || !n) return;
    auto nameOf = [&](MenuNav nav) {
        return navName && navName[nav] && navName[nav][0] ? navName[nav] : kFixedNavName[nav];
    };
    char change[32];
    snprintf(change, sizeof(change), "%s/%s", nameOf(kNavLeft), nameOf(kNavRight));
    if (typed) snprintf(out, n, "%s types a value; %s steps it.", nameOf(kNavSelect), change);
    else snprintf(out, n, "%s or %s changes it.", nameOf(kNavSelect), change);
}

// ---------------------------------------------------------------------------
// The footer

namespace {

struct FooterItem {
    char text[48];
    int  dropRank;   // 0 never dropped; otherwise dropped lowest rank first
    bool dropped;
};

void joinItems(const FooterItem* items, int count, char* out, size_t n) {
    out[0] = 0;
    bool first = true;
    for (int i = 0; i < count; ++i) {
        if (items[i].dropped) continue;
        if (!first) append(out, n, "  ");
        append(out, n, items[i].text);
        first = false;
    }
}

// Drop items lowest rank first until the line measures within the width.
// Rank-0 items survive whatever the width: a legend with no "page" and no
// "close" would be the clipped footer this replaces.
void fitLine(FooterItem* items, int count, int widthPx, MenuMeasureFn measure, void* ctx, char* out,
             size_t n) {
    joinItems(items, count, out, n);
    if (!measure || widthPx <= 0) return;
    while (measure(out, ctx) > widthPx) {
        int pick = -1;
        for (int i = 0; i < count; ++i) {
            if (items[i].dropped || items[i].dropRank <= 0) continue;
            if (pick < 0 || items[i].dropRank < items[pick].dropRank) pick = i;
        }
        if (pick < 0) return;
        items[pick].dropped = true;
        joinItems(items, count, out, n);
    }
}

// The pair the shared-keyboard line says is off: pick when it is adopted,
// else change. Never the page pair -- it follows Tab and is never off. A
// pair whose both names are the fixed key's own is not adopted (a
// same-as-fixed slot names the fixed key itself), and Select is never used
// because a same-as-fixed Space would read as an adopted key.
bool offPair(const MenuFooterInput& in, char* out, size_t n) {
    auto nameOf = [&](MenuNav nav) {
        return in.navName[nav] && in.navName[nav][0] ? in.navName[nav] : kFixedNavName[nav];
    };
    const MenuNav pairs[2][2] = {{kNavUp, kNavDown}, {kNavLeft, kNavRight}};
    for (const auto& p : pairs) {
        const char* a = nameOf(p[0]);
        const char* b = nameOf(p[1]);
        if (strcmp(a, kFixedNavName[p[0]]) == 0 && strcmp(b, kFixedNavName[p[1]]) == 0) continue;
        snprintf(out, n, "%s/%s", a, b);
        return true;
    }
    return false;
}

// The legend's page item: the adopted pair wherever any of it is adopted
// (a page key follows Tab, so it is live on every page and in every
// keyboard mode), else the fixed key's own "Tab page". Half a pair reads
// "Tab/E page": Tab stands for Shift+Tab as it does in the fixed legend.
void pageItem(const MenuFooterInput& in, char* out, size_t n) {
    auto nameOf = [&](MenuNav nav) {
        return in.navName[nav] && in.navName[nav][0] ? in.navName[nav] : kFixedNavName[nav];
    };
    const char* a = nameOf(kNavPagePrev);
    const char* b = nameOf(kNavPageNext);
    if (strcmp(a, kFixedNavName[kNavPagePrev]) == 0 && strcmp(b, kFixedNavName[kNavPageNext]) == 0) {
        snprintf(out, n, "Tab page");
    } else {
        snprintf(out, n, "%s/%s page", a, b);
    }
}

}  // namespace

void menuComposeFooter(const MenuFooterInput& in, MenuMeasureFn measure, void* ctx, char* out,
                       size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    auto nameOf = [&](MenuNav nav) {
        return in.navName[nav] && in.navName[nav][0] ? in.navName[nav] : kFixedNavName[nav];
    };
    auto fits = [&](const char* s) { return !measure || in.widthPx <= 0 || measure(s, ctx) <= in.widthPx; };

    // Line 1, the legend.
    FooterItem items[5] = {};
    int count = 0;
    auto item = [&](const char* text, int rank) {
        FooterItem& it = items[count++];
        snprintf(it.text, sizeof(it.text), "%s", text);
        it.dropRank = rank;
        it.dropped = false;
    };
    // The page item follows Tab (menuAliasFollowsTab): the adopted pair is
    // named on every page and in every keyboard mode, because it acts
    // there; the other items follow the live predicate.
    char page[48];
    pageItem(in, page, sizeof(page));
    if (in.editing) {
        item("Type a value", 1);
        item("Backspace deletes", 2);
        item("Enter writes", 0);
        item("Esc cancels", 0);
    } else if (in.statusPage) {
        item(page, 0);
        item("Esc close", 0);
    } else if (in.aliasesLive) {
        char b[48];
        snprintf(b, sizeof(b), "%s/%s pick", nameOf(kNavUp), nameOf(kNavDown));
        item(b, 2);
        snprintf(b, sizeof(b), "%s/%s change", nameOf(kNavLeft), nameOf(kNavRight));
        item(b, 1);
        snprintf(b, sizeof(b), "%s select", nameOf(kNavSelect));
        item(b, 3);
        item(page, 0);
        // Always Esc: a back alias is on the Status row and in the log
        // (MEASURED: "L-Ctrl or Esc close" pushes the line to 861 px in 770).
        item("Esc close", 0);
    } else {
        item("Arrows pick/change", 1);
        item("Enter select", 3);
        item(page, 0);
        item("Esc close", 0);
    }
    char line1[160];
    fitLine(items, count, in.widthPx, measure, ctx, line1, sizeof(line1));

    // Line 2, at most one item, by precedence. The pending count yields to
    // the shared warnings because it is also a row badge and on the Status
    // page; a warning is shown nowhere else. The fault line wins in every
    // state, typing included: a stalled draw drops the gate while a value
    // is typed, and the keys being pressed then are W/A/S/D/Space -- the
    // ship's thrust and boost -- so that is the moment the warning is for.
    char line2[160] = "";
    char pair[32];
    if (in.privateWanted && !in.statusPage && !in.gatePrivate) {
        snprintf(line2, sizeof(line2), "KEYS SHARED WITH THE GAME");
    } else if (in.statusPage) {
        // No "off" note here: the legend above names only the page pair,
        // and that pair is on.
        snprintf(line2, sizeof(line2), "keys shared (%s)", in.pageName ? in.pageName : "");
    } else if (!in.privateWanted) {
        snprintf(line2, sizeof(line2), "keys shared (menu.keyboard)");
        if (in.adopted && offPair(in, pair, sizeof(pair))) {
            append(line2, sizeof(line2), "  ");
            append(line2, sizeof(line2), pair);
            append(line2, sizeof(line2), " off");
        }
    } else if (in.pendingN > 0) {
        snprintf(line2, sizeof(line2), "%d change%s at next launch", in.pendingN,
                 in.pendingN == 1 ? "" : "s");
    } else if (in.aliasesLive) {
        snprintf(line2, sizeof(line2), "Tab, arrows and Enter work too");
    }
    if (line2[0] && !fits(line2)) line2[0] = 0;

    // MenuContent::footer is 160 bytes: at most 159 and one '\n'.
    const size_t cap = n < 160 ? n : 160;
    if (line2[0]) snprintf(out, cap, "%s\n%s", line1, line2);
    else snprintf(out, cap, "%s", line1);
}

}  // namespace edvr
