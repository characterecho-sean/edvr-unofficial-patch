#include "elite_binds.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/log.h"

namespace edvr {
namespace {

// Elite's Key_ names that are not simply their own EDVR name once the prefix
// is stripped. Everything else -- Key_F11, Key_A, Key_5 -- maps by stripping.
struct KeyMap { const char* elite; const char* ours; };
const KeyMap kKeyMap[] = {
    {"RightArrow", "RIGHT"},     {"LeftArrow", "LEFT"},
    {"UpArrow", "UP"},           {"DownArrow", "DOWN"},
    {"Space", "SPACE"},          {"Enter", "ENTER"},
    {"Return", "ENTER"},         {"Backspace", "BACKSPACE"},
    {"Tab", "TAB"},              {"Escape", "ESCAPE"},
    {"Insert", "INSERT"},        {"Home", "HOME"},
    {"End", "END"},              {"Delete", "DELETE"},
    {"PageUp", "PAGEUP"},        {"PageDown", "PAGEDOWN"},
    {"ScrollLock", "SCROLLLOCK"},{"Pause", "PAUSE"},
    {"NumLock", "NUMLOCK"},      {"CapsLock", "CAPSLOCK"},
    {"BackSlash", "BACKSLASH"},  {"Slash", "SLASH"},
    {"LeftBracket", "LEFTBRACKET"}, {"RightBracket", "RIGHTBRACKET"},
    {"SemiColon", "SEMICOLON"},  {"Apostrophe", "APOSTROPHE"},
    {"Comma", "COMMA"},          {"Period", "PERIOD"},
    {"Grave", "GRAVE"},          {"Minus", "MINUS"},
    {"Equals", "EQUALS"},        {"Hash", "HASH"},
    {"Numpad_0", "NUMPAD0"},     {"Numpad_1", "NUMPAD1"},
    {"Numpad_2", "NUMPAD2"},     {"Numpad_3", "NUMPAD3"},
    {"Numpad_4", "NUMPAD4"},     {"Numpad_5", "NUMPAD5"},
    {"Numpad_6", "NUMPAD6"},     {"Numpad_7", "NUMPAD7"},
    {"Numpad_8", "NUMPAD8"},     {"Numpad_9", "NUMPAD9"},
    {"Numpad_Multiply", "MULTIPLY"}, {"Numpad_Divide", "DIVIDE"},
    {"Numpad_Add", "ADD"},       {"Numpad_Subtract", "SUBTRACT"},
    {"Numpad_Decimal", "DECIMAL"},
    // Names Elite writes that this table could not express before, each one
    // silently reported as "not on a keyboard key" (issue #8).
    {"PrintScreen", "PRINTSCREEN"}, {"Apps", "APPS"},
    {"Menu", "APPS"},
    // Numpad Enter and the main Enter are ONE virtual key; the difference is
    // an extended-key flag that GetAsyncKeyState does not report. Watching
    // ENTER is the honest best we can do, and it does fire on the numpad one.
    {"Numpad_Enter", "ENTER"},
    // The extra key ISO keyboards have beside left shift (and on some
    // layouts beside Enter). No character name fits it across layouts, so it
    // goes through as the raw virtual-key code the ini syntax also accepts.
    {"Oem102", "0xE2"},
    // Elite names the modifier keys as keys; as a MAIN binding they are not
    // meaningful to EDVR and translate to nothing.
};

// Modifier Key_ names, for <Modifier> sub-elements.
const KeyMap kModMap[] = {
    {"LeftShift", "SHIFT"},   {"RightShift", "SHIFT"},
    {"LeftControl", "CTRL"},  {"RightControl", "CTRL"},
    {"LeftAlt", "ALT"},       {"RightAlt", "ALT"},
};

// The same six keys as MAIN keys, consulted only under
// kEliteKeyAllowModifierMain (the settings menu: Sean's UI_Back is
// Key_LeftControl). Raw virtual-key codes rather than names, because the
// hotkey parser has no word for a bare left Ctrl -- "CTRL" there is a
// modifier prefix, and a binding of just "CTRL" is the "no key" error. The
// left/right distinction is kept: GetAsyncKeyState(VK_LCONTROL) polls one
// physical key, which is what Elite bound.
const KeyMap kModMainMap[] = {
    {"LeftShift", "0xA0"},    {"RightShift", "0xA1"},
    {"LeftControl", "0xA2"},  {"RightControl", "0xA3"},
    {"LeftAlt", "0xA4"},      {"RightAlt", "0xA5"},
};

std::wstring bindingsDir() {
    wchar_t appdata[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return std::wstring(appdata) +
           L"\\Frontier Developments\\Elite Dangerous\\Options\\Bindings";
}

bool readWholeFile(const std::wstring& path, std::string* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(f, nullptr);
    if (size == INVALID_FILE_SIZE || size > (4u << 20)) {
        CloseHandle(f);
        return false;
    }
    out->resize(size);
    DWORD got = 0;
    const BOOL ok = ReadFile(f, &(*out)[0], size, &got, nullptr);
    CloseHandle(f);
    if (!ok) return false;
    out->resize(got);
    return true;
}

// Extract attribute="value" following `from` within the next `span` bytes.
bool attrAfter(const std::string& text, size_t from, size_t span,
               const char* attr, std::string* out) {
    const std::string needle = std::string(attr) + "=\"";
    const size_t at = text.find(needle, from);
    if (at == std::string::npos || at > from + span) return false;
    const size_t start = at + needle.size();
    const size_t end = text.find('"', start);
    if (end == std::string::npos) return false;
    *out = text.substr(start, end - start);
    return true;
}

// One candidate .binds file of the active preset.
struct Cand {
    std::wstring name;
    char utf8[MAX_PATH];
    FILETIME wt;
};

// The candidate files of the active preset, NEWEST-FIRST. Shared by every
// lookup below (camera keyboard, camera gamepad, menu slots) -- this walk
// was mirrored by hand in two functions with a "a selection-rule fix must
// land in both" warning, and a third consumer is where a hand mirror
// drifts. The camera fixtures in openvr_smoke pin its answers across the
// move.
//
// Preset names come from StartPreset.4.start (one per bind context in
// current builds) or the older StartPreset.start; no preset file means no
// candidates. A file is a candidate when its name is <preset> plus the
// version dot, so preset "Custom" does not also claim a preset named
// "Custom2"; an EMPTY preset file admits every .binds.
//
// Newest first because Elite keeps previous-format presets beside the live
// one -- a Custom.4.1.binds untouched since January 2025 sat beside the
// maintained Custom.4.2.binds and answered a live rebind with January's
// keys, because directory order put it first. The file the game maintains
// is the one it rewrites on every Apply, so recency picks the truth and
// self-heals across format bumps.
//
// Returns false when there is no preset file or the directory cannot be
// enumerated; true with an empty `out` is a preset naming files that are
// not there.
bool collectActiveBindsFiles(const std::wstring& dir, std::vector<Cand>* out) {
    out->clear();
    std::string presets;
    if (!readWholeFile(dir + L"\\StartPreset.4.start", &presets) &&
        !readWholeFile(dir + L"\\StartPreset.start", &presets)) {
        return false;
    }
    std::vector<std::string> names;
    {
        size_t start = 0;
        while (start < presets.size()) {
            size_t end = presets.find_first_of("\r\n", start);
            if (end == std::string::npos) end = presets.size();
            if (end > start) names.push_back(presets.substr(start, end - start));
            start = presets.find_first_not_of("\r\n", end);
            if (start == std::string::npos) break;
        }
    }
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*.binds").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Cand c;
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, c.utf8,
                            sizeof(c.utf8), nullptr, nullptr);
        bool active = names.empty();
        for (const std::string& n : names) {
            if (_strnicmp(c.utf8, n.c_str(), n.size()) == 0 &&
                c.utf8[n.size()] == '.') {
                active = true;
                break;
            }
        }
        if (!active) continue;
        c.name = fd.cFileName;
        c.wt = fd.ftLastWriteTime;
        out->push_back(c);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    std::sort(out->begin(), out->end(), [](const Cand& a, const Cand& b) {
        return CompareFileTime(&a.wt, &b.wt) > 0;
    });
    return true;
}

}  // namespace

bool eliteBindsTranslateKey(const char* eliteKey, char* out, size_t outLen) {
    if (!eliteKey || !out || outLen == 0) return false;
    const char* name = eliteKey;
    if (strncmp(name, "Key_", 4) == 0) name += 4;
    for (const KeyMap& m : kKeyMap) {
        if (_stricmp(name, m.elite) == 0) {
            snprintf(out, outLen, "%s", m.ours);
            return true;
        }
    }
    // Single letter or digit maps as itself; anything longer that the table
    // does not know is a name this build cannot express.
    if (name[0] != '\0' && name[1] == '\0' &&
        ((name[0] >= 'A' && name[0] <= 'Z') ||
         (name[0] >= 'a' && name[0] <= 'z') ||
         (name[0] >= '0' && name[0] <= '9'))) {
        snprintf(out, outLen, "%c", name[0]);
        return true;
    }
    // F-keys: F1..F24 pass through.
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '0' && name[1] <= '9') {
        snprintf(out, outLen, "%s", name);
        return true;
    }
    return false;
}

bool eliteBindsTranslateKeyEx(const char* eliteKey, unsigned flags, char* out,
                              size_t outLen, bool* modifierMain) {
    if (modifierMain) *modifierMain = false;
    // The plain answer first, so a caller with the flag gets byte-for-byte
    // what the camera path gets for every name the camera path can name.
    if (eliteBindsTranslateKey(eliteKey, out, outLen)) return true;
    if (!(flags & kEliteKeyAllowModifierMain)) return false;
    if (!eliteKey || !out || outLen == 0) return false;
    const char* name = eliteKey;
    if (strncmp(name, "Key_", 4) == 0) name += 4;
    for (const KeyMap& m : kModMainMap) {
        if (_stricmp(name, m.elite) == 0) {
            snprintf(out, outLen, "%s", m.ours);
            if (modifierMain) *modifierMain = true;
            return true;
        }
    }
    return false;
}

// One element in one file's text: is the tag present, and does it carry a
// keyboard binding? The tag is matched CLOSED ("<Name>") -- a bare prefix
// also matched <PhotoCameraToggle_Humanoid> when asked for
// <PhotoCameraToggle>, and only file layout kept that from answering with
// the wrong element. The slot search is bounded by the element's own close
// tag, not a byte count: with a fixed span, an element missing its
// Secondary borrowed the NEXT element's, which is how a controller-bound
// on-foot camera read as the ship's F11.
// rawKb receives the Elite key name of a KEYBOARD slot this build could not
// translate. That distinction is the whole point: "no keyboard binding" and
// "a keyboard binding EDVR cannot name" look identical from outside and want
// opposite things from the player -- bind a keyboard key, versus tell us the
// name so it can be added. Issue #8 was the second, reported as the first.
bool parseElementIn(const std::string& text, const char* element, char* out,
                    size_t outLen, bool* present, char* rawKb, size_t rawKbLen) {
    if (rawKb && rawKbLen) rawKb[0] = '\0';
    const size_t el = text.find(std::string("<") + element + ">");
    if (present) *present = el != std::string::npos;
    if (el == std::string::npos) return false;
    size_t end = text.find(std::string("</") + element + ">", el);
    if (end == std::string::npos) end = el + 600;   // damaged file: old bound
    for (const char* slot : {"<Primary ", "<Secondary "}) {
        const size_t s = text.find(slot, el);
        if (s == std::string::npos || s > end) continue;
        std::string device, key;
        if (!attrAfter(text, s, 120, "Device", &device)) continue;
        if (_stricmp(device.c_str(), "Keyboard") != 0) continue;
        if (!attrAfter(text, s, 160, "Key", &key)) continue;
        char keyName[32];
        if (!eliteBindsTranslateKey(key.c_str(), keyName, sizeof(keyName))) {
            // A keyboard slot we cannot name. Remember the first one so the
            // caller can say which key it was instead of claiming there is
            // none, and keep looking -- Secondary may still be expressible.
            if (rawKb && rawKbLen && rawKb[0] == '\0') {
                snprintf(rawKb, rawKbLen, "%s", key.c_str());
            }
            continue;
        }
        // Modifiers attached to this slot, before the element ends.
        std::string prefix;
        size_t m = s;
        for (int guard = 0; guard < 3; ++guard) {
            const size_t mod = text.find("<Modifier ", m);
            if (mod == std::string::npos || mod > end) break;
            std::string mdev, mkey;
            if (attrAfter(text, mod, 120, "Device", &mdev) &&
                _stricmp(mdev.c_str(), "Keyboard") == 0 &&
                attrAfter(text, mod, 160, "Key", &mkey)) {
                const char* mn = mkey.c_str();
                if (strncmp(mn, "Key_", 4) == 0) mn += 4;
                for (const KeyMap& mm : kModMap) {
                    if (_stricmp(mn, mm.elite) == 0) {
                        prefix += mm.ours;
                        prefix += "+";
                        break;
                    }
                }
            }
            m = mod + 10;
        }
        snprintf(out, outLen, "%s%s", prefix.c_str(), keyName);
        return true;
    }
    return false;
}

// EVERY slot of one element, for the settings menu. Same closed-tag match
// and same close-tag bound as parseElementIn; the difference is that nothing
// returns early on the first translatable slot -- a gamepad Primary and a
// keyboard Secondary both come back, each saying what it is, so the menu
// can adopt the one and explain the other.
//
// The <Modifier> search is bounded by the NEXT slot's tag (parsePadIn's
// rule), not by the element's end as parseElementIn's is. parseElementIn
// is deliberately left as it is: for an element with a bare Primary and a
// chorded Secondary it answers "SHIFT+W", and that answer is the camera
// path's, pinned by the smoke fixtures. Here the same element reads as a
// bare W and a chorded E, which is what the file says.
//
// Fills present, count and slot[]; the caller owns filesSeen and file.
// Returns present. An unbound slot is not a slot: Elite writes
// Device="{NoDevice}" Key="" for every empty Primary and Secondary
// (MEASURED 2026-09-11: 430 of them in Sean's Custom.4.2.binds), and
// counting one as "not keyboard" would have the menu report a key that is
// on nothing as being on a controller.
bool parseElementSlotsIn(const std::string& text, const char* element,
                         unsigned flags, EliteKeySlots* out) {
    out->present = false;
    out->count = 0;
    memset(out->slot, 0, sizeof(out->slot));
    const size_t el = text.find(std::string("<") + element + ">");
    out->present = el != std::string::npos;
    if (el == std::string::npos) return false;
    size_t end = text.find(std::string("</") + element + ">", el);
    if (end == std::string::npos) end = el + 600;   // damaged file: old bound
    for (const char* slot : {"<Primary ", "<Secondary "}) {
        const size_t s = text.find(slot, el);
        if (s == std::string::npos || s > end) continue;
        std::string device, key;
        if (!attrAfter(text, s, 120, "Device", &device)) continue;
        if (!attrAfter(text, s, 160, "Key", &key)) continue;
        if (key.empty() || _stricmp(device.c_str(), "{NoDevice}") == 0) continue;
        EliteKeySlot& k = out->slot[out->count++];
        snprintf(k.eliteName, sizeof(k.eliteName), "%s", key.c_str());
        k.keyboard = _stricmp(device.c_str(), "Keyboard") == 0;
        // This slot's span: up to the next slot's tag or the element's end.
        size_t slotEnd = end;
        for (const char* other : {"<Primary ", "<Secondary "}) {
            const size_t o = text.find(other, s + 1);
            if (o != std::string::npos && o < slotEnd) slotEnd = o;
        }
        // Keyboard modifiers within the span, prefixed exactly as
        // parseElementIn builds them. A keyboard modifier this table has no
        // word for still marks the slot chorded: the menu adopts bare keys
        // only, and this slot is not one.
        std::string prefix;
        size_t m = s;
        for (int guard = 0; guard < 3; ++guard) {
            const size_t mod = text.find("<Modifier ", m);
            if (mod == std::string::npos || mod > slotEnd) break;
            std::string mdev, mkey;
            if (attrAfter(text, mod, 120, "Device", &mdev) &&
                _stricmp(mdev.c_str(), "Keyboard") == 0 &&
                attrAfter(text, mod, 160, "Key", &mkey)) {
                k.chorded = true;
                const char* mn = mkey.c_str();
                if (strncmp(mn, "Key_", 4) == 0) mn += 4;
                for (const KeyMap& mm : kModMap) {
                    if (_stricmp(mn, mm.elite) == 0) {
                        prefix += mm.ours;
                        prefix += "+";
                        break;
                    }
                }
            }
            m = mod + 10;
        }
        if (!k.keyboard) continue;   // eliteName says which pad/mouse key
        char keyName[32];
        if (!eliteBindsTranslateKeyEx(key.c_str(), flags, keyName,
                                      sizeof(keyName), &k.modifierMain)) {
            continue;   // binding stays empty: keyboard, but unnamed
        }
        snprintf(k.binding, sizeof(k.binding), "%s%s", prefix.c_str(), keyName);
    }
    return true;
}

// The GamePad slots of one element: the raw Elite key name, no
// translation (the xinput watcher owns that table).
//
// A slot with a Modifier chord is skipped -- UNLESS the caller passes
// modOut, in which case the chord is reported instead of hidden. Two
// different questions share this walk: "which gamepad key fires this" wants
// the unchorded slot only, and "does this element chord on a key somebody
// else watches" wants exactly the ones the first question drops.
bool parsePadIn(const std::string& text, const char* element, char* out,
                size_t outLen, bool* present, char* modOut = nullptr,
                size_t modLen = 0) {
    if (modOut && modLen) modOut[0] = '\0';
    const size_t el = text.find(std::string("<") + element + ">");
    if (present) *present = el != std::string::npos;
    if (el == std::string::npos) return false;
    size_t end = text.find(std::string("</") + element + ">", el);
    if (end == std::string::npos) end = el + 600;
    for (const char* slot : {"<Primary ", "<Secondary "}) {
        const size_t s = text.find(slot, el);
        if (s == std::string::npos || s > end) continue;
        std::string device, key;
        if (!attrAfter(text, s, 120, "Device", &device)) continue;
        if (_stricmp(device.c_str(), "GamePad") != 0) continue;
        if (!attrAfter(text, s, 160, "Key", &key)) continue;
        // A chord on this slot: the Modifier sits between this slot's tag
        // and the next slot (or the element's end).
        size_t slotEnd = end;
        for (const char* other : {"<Primary ", "<Secondary "}) {
            const size_t o = text.find(other, s + 1);
            if (o != std::string::npos && o < slotEnd) slotEnd = o;
        }
        const size_t mod = text.find("<Modifier ", s);
        const bool chorded = mod != std::string::npos && mod < slotEnd;
        if (chorded && !modOut) continue;
        if (chorded) {
            std::string modKey;
            if (!attrAfter(text, mod, 160, "Key", &modKey)) continue;
            snprintf(modOut, modLen, "%s", modKey.c_str());
        } else if (modOut) {
            // Asked for a chord and this slot has none: not an answer.
            continue;
        }
        snprintf(out, outLen, "%s", key.c_str());
        return true;
    }
    return false;
}

// The same file selection as eliteBindsLookupDir below (one walk,
// collectActiveBindsFiles); the first file containing the element answers
// alone.
bool eliteBindsLookupPadDir(const wchar_t* dirC, const char* element,
                            char* out, size_t outLen) {
    if (!dirC || !dirC[0] || !element || !out || outLen == 0) return false;
    const std::wstring dir(dirC);
    std::vector<Cand> cands;
    if (!collectActiveBindsFiles(dir, &cands)) return false;
    for (const Cand& c : cands) {
        std::string text;
        if (!readWholeFile(dir + L"\\" + c.name, &text)) continue;
        bool present = false;
        if (parsePadIn(text, element, out, outLen, &present)) {
            Log::get().note("bindings: %s gamepad slot read from %s: %s",
                            element, c.utf8, out);
            return true;
        }
        if (present) return false;   // element answered: no pad slot
    }
    return false;
}

bool eliteBindsLookupPad(const char* element, char* out, size_t outLen) {
    return eliteBindsLookupPadDir(bindingsDir().c_str(), element, out,
                                  outLen);
}

bool eliteBindsLookupPadMod(const char* element, char* out, size_t outLen) {
    if (!element || !out || outLen == 0) return false;
    out[0] = '\0';
    const std::wstring dir = bindingsDir();
    if (dir.empty()) return false;
    // The same file walk as the lookups above, kept short because it wants
    // only the newest maintained preset -- a stale file naming a chord that
    // is no longer bound would veto a press for no reason.
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*.binds").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return false;
    std::wstring best;
    FILETIME bestTime{};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (best.empty() || CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) {
            best = fd.cFileName;
            bestTime = fd.ftLastWriteTime;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (best.empty()) return false;
    std::string text;
    if (!readWholeFile(dir + L"\\" + best, &text)) return false;
    char key[48];
    bool present = false;
    return parsePadIn(text, element, key, sizeof(key), &present, out, outLen);
}

bool eliteBindsLookupDir(const wchar_t* dirC, const char* element, char* out,
                         size_t outLen, const char* fallbackElement) {
    if (!dirC || !dirC[0] || !element || !out || outLen == 0) return false;
    const std::wstring dir(dirC);

    // The active preset's files, newest first (the walk and its reasons
    // are with collectActiveBindsFiles).
    std::vector<Cand> cands;
    if (!collectActiveBindsFiles(dir, &cands)) return false;

    for (const Cand& c : cands) {
        std::string text;
        if (!readWholeFile(dir + L"\\" + c.name, &text)) continue;
        // The first file that CONTAINS the element answers ALONE -- falling
        // through to an older file would resurrect exactly the stale keys
        // recency exists to bury. The fallback element is consulted only
        // where the primary is absent from this file: on foot the game
        // acts on the _Humanoid element exclusively, so a Humanoid entry
        // bound to a controller must not inherit the ship element's
        // keyboard key -- that key does nothing on foot.
        for (const char* wanted : {element, fallbackElement}) {
            if (!wanted) break;
            bool present = false;
            char rawKb[48] = {};
            if (parseElementIn(text, wanted, out, outLen, &present, rawKb,
                               sizeof(rawKb))) {
                Log::get().note("bindings: %s read from %s: %s", wanted,
                                c.utf8, out);
                return true;
            }
            if (present && rawKb[0] != '\0') {
                Log::get().note(
                    "bindings: %s in %s IS on a keyboard key -- Elite calls it "
                    "%s -- but this build has no name for that key, so it "
                    "cannot be watched. Please report this line; meanwhile "
                    "binding the action to an ordinary letter, F-key or arrow "
                    "in Elite will work.",
                    wanted, c.utf8, rawKb);
                return false;
            }
            if (present) {
                Log::get().note(
                    "bindings: %s in %s is not on a keyboard key, so there "
                    "is nothing for EDVR to watch for it.",
                    wanted, c.utf8);
                return false;
            }
        }
    }
    return false;
}

bool eliteBindsLookup(const char* element, char* out, size_t outLen,
                      const char* fallbackElement) {
    return eliteBindsLookupDir(bindingsDir().c_str(), element, out, outLen,
                               fallbackElement);
}

bool eliteBindsLookupSlotsDir(const wchar_t* dirC, const char* element,
                              unsigned flags, EliteKeySlots* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!dirC || !dirC[0] || !element) return false;
    const std::wstring dir(dirC);
    std::vector<Cand> cands;
    if (!collectActiveBindsFiles(dir, &cands)) return false;
    for (const Cand& c : cands) {
        std::string text;
        if (!readWholeFile(dir + L"\\" + c.name, &text)) continue;
        ++out->filesSeen;
        // The first file that CONTAINS the element answers ALONE, whatever
        // its slots hold -- the camera lookup's rule, for the same reason:
        // falling through to an older file would resurrect the stale keys
        // recency exists to bury. No fallback element: MEASURED 2026-09-11,
        // none of the ten UI elements has a _Humanoid twin in Sean's file.
        if (parseElementSlotsIn(text, element, flags, out)) {
            snprintf(out->file, sizeof(out->file), "%s", c.utf8);
            return true;
        }
    }
    return false;
}

bool eliteBindsLookupSlots(const char* element, unsigned flags,
                           EliteKeySlots* out) {
    return eliteBindsLookupSlotsDir(bindingsDir().c_str(), element, flags,
                                    out);
}

unsigned long long eliteBindsFingerprintDir(const wchar_t* dir) {
    if (!dir || !dir[0]) return 0;
    const std::wstring glob = std::wstring(dir) + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(glob.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    // FNV-1a over each file's name, size and write time. Enumeration order
    // is filesystem-defined but stable between consecutive calls on the same
    // volume, which is all a change detector needs.
    unsigned long long fp = 1469598103934665603ull;
    const auto fold = [&fp](const void* p, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) {
            fp ^= b[i];
            fp *= 1099511628211ull;
        }
    };
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        fold(fd.cFileName, wcslen(fd.cFileName) * sizeof(wchar_t));
        fold(&fd.nFileSizeLow, sizeof(fd.nFileSizeLow));
        fold(&fd.nFileSizeHigh, sizeof(fd.nFileSizeHigh));
        fold(&fd.ftLastWriteTime, sizeof(fd.ftLastWriteTime));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return fp ? fp : 1;   // 0 stays reserved for "could not enumerate"
}

unsigned long long eliteBindsFingerprint() {
    return eliteBindsFingerprintDir(bindingsDir().c_str());
}

}  // namespace edvr
