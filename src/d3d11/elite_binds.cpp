// GENERATED from src/d3d11/elite_binds.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 72949619fcf969ec]
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
    // Elite names the modifier keys as keys; as a MAIN binding they are not
    // meaningful to EDVR and translate to nothing.
};

// Modifier Key_ names, for <Modifier> sub-elements.
const KeyMap kModMap[] = {
    {"LeftShift", "SHIFT"},   {"RightShift", "SHIFT"},
    {"LeftControl", "CTRL"},  {"RightControl", "CTRL"},
    {"LeftAlt", "ALT"},       {"RightAlt", "ALT"},
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

// One element in one file's text: is the tag present, and does it carry a
// keyboard binding? The tag is matched CLOSED ("<Name>") -- a bare prefix
// also matched <PhotoCameraToggle_Humanoid> when asked for
// <PhotoCameraToggle>, and only file layout kept that from answering with
// the wrong element. The slot search is bounded by the element's own close
// tag, not a byte count: with a fixed span, an element missing its
// Secondary borrowed the NEXT element's, which is how a controller-bound
// on-foot camera read as the ship's F11.
bool parseElementIn(const std::string& text, const char* element, char* out,
                    size_t outLen, bool* present) {
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
        if (!eliteBindsTranslateKey(key.c_str(), keyName, sizeof(keyName)))
            continue;
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

bool eliteBindsLookupDir(const wchar_t* dirC, const char* element, char* out,
                         size_t outLen, const char* fallbackElement) {
    if (!dirC || !dirC[0] || !element || !out || outLen == 0) return false;
    const std::wstring dir(dirC);

    // The active preset names, one per bind context in current builds.
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

    // Candidate files NEWEST-FIRST. Elite keeps previous-format presets
    // beside the live one -- a Custom.4.1.binds untouched since January
    // 2025 sat beside the maintained Custom.4.2.binds and answered a live
    // rebind with January's keys, because directory order put it first.
    // The file the game maintains is the one it rewrites on every Apply,
    // so recency picks the truth and self-heals across format bumps.
    struct Cand {
        std::wstring name;
        char utf8[MAX_PATH];
        FILETIME wt;
    };
    std::vector<Cand> cands;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*.binds").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Cand c;
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, c.utf8,
                            sizeof(c.utf8), nullptr, nullptr);
        // Prefix plus the version dot, so preset "Custom" does not also
        // claim a preset named "Custom2".
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
        cands.push_back(c);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return CompareFileTime(&a.wt, &b.wt) > 0;
    });

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
            if (parseElementIn(text, wanted, out, outLen, &present)) {
                Log::get().note("bindings: %s read from %s: %s", wanted,
                                c.utf8, out);
                return true;
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
