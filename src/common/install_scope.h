// The launch decision: which profile this process runs, from the installation's scope (edvr_profile.ini), the game's 3D mode
// (Settings.xml) and the command line. Pure and free of any Windows API, so tools\runtime_profile_test drives every rule here.
// runtime_profile.h reads the files and the command line and calls these functions; nothing in this header touches the disk.
// The rules are docs\design-unified-install-2026-10-12.md section 3.1.
#pragma once

#include <cstring>
#include <string>

#include "stereo_mode.h"

namespace edvr {

// The profile a process runs. Flat and Vr are the two profiles. Passthrough forwards only: the installation or the mode does not
// include this profile, and the next switch may leave it. Invalid forwards and stays off. LegacyVr is the value before the launch
// decision runs, and it reads as Vr.
enum class RuntimeProfile { LegacyVr, Vr, Flat, Invalid, Passthrough };

// What the user installed. Absent descriptor means Both (design Q3).
enum class InstallScope { Both, Vr, Flat, Invalid };

// schema is 0 when absent, 1 or 2 otherwise (0 with present true means the descriptor was malformed).
struct ScopeRead {
    InstallScope scope;
    int schema;
    bool present;
};

// The game's 3D mode: Flat for modes 0 and 1, Vr for 2 to 6.
enum class GameMode { Flat, Vr };

// known is false when the mode is unknown (the file was absent or unreadable, the tag was absent, or its value was out of range).
// Unknown reads as Vr (design Q2). fileRead says whether Settings.xml's text was read at all; it is false for an absent file.
struct ModeRead {
    bool known;
    int value;
    GameMode mode;
    bool fileRead;
};

// The command line's switch. When both appear, the last one wins.
enum class VrSwitch { None, NoVr, Vr };

struct LaunchDecision {
    ScopeRead scope;
    ModeRead mode;
    VrSwitch cmd;
    bool cmdApplied;      // the command line overrode the mode for this launch
    GameMode effective;   // the mode after the command line
    RuntimeProfile active;
};

inline std::string profileTrim(const std::string& value) {
    const size_t first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

inline ScopeRead absentDescriptor() { return ScopeRead{InstallScope::Both, 0, false}; }

// The descriptor's text, read as edvr_profile.ini. Duplicates and unknown fields are refused so two readers cannot disagree on scope.
// Schema 1 (profile = vr|flat) is what the installer wrote before this change; schema 2 (scope = both|vr|flat) is the unified
// install's. A DLL built before schema 2 reads it as Invalid, which is the failure wanted there: the fixes are off, chaining is kept.
inline ScopeRead parseInstallDescriptor(std::string text) {
    const ScopeRead invalid{InstallScope::Invalid, 0, true};
    if (text.size() > 4096 || text.find('\0') != std::string::npos) return invalid;
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
    bool section = false, schemaSeen = false, profileSeen = false, scopeSeen = false;
    int schema = 0;
    std::string profile, scope;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = profileTrim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line == "[install]") {
            if (section) return invalid;
            section = true;
            continue;
        }
        if (!section) return invalid;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return invalid;
        const auto key = profileTrim(line.substr(0, eq));
        const auto value = profileTrim(line.substr(eq + 1));
        if (key == "schema" && !schemaSeen) {
            schemaSeen = true;
            if (value == "1") schema = 1;
            else if (value == "2") schema = 2;
            else return invalid;
        } else if (key == "profile" && !profileSeen) {
            profileSeen = true;
            profile = value;
        } else if (key == "scope" && !scopeSeen) {
            scopeSeen = true;
            scope = value;
        } else return invalid;
    }
    if (!section || !schemaSeen) return invalid;
    if (schema == 1) {
        if (!profileSeen || scopeSeen) return invalid;
        if (profile == "vr") return ScopeRead{InstallScope::Both, 1, true};
        if (profile == "flat") return ScopeRead{InstallScope::Flat, 1, true};
        return invalid;
    }
    if (!scopeSeen || profileSeen) return invalid;
    if (scope == "both") return ScopeRead{InstallScope::Both, 2, true};
    if (scope == "vr") return ScopeRead{InstallScope::Vr, 2, true};
    if (scope == "flat") return ScopeRead{InstallScope::Flat, 2, true};
    return invalid;
}

// Settings.xml's text, or null when the file was absent, unreadable or too large. A text with no usable tag is a read file whose mode
// is unknown.
inline ModeRead readGameMode(const char* settingsXml) {
    if (!settingsXml) return ModeRead{false, -1, GameMode::Vr, false};
    int value = -1;
    if (!parseStereoscopicMode(settingsXml, &value)) return ModeRead{false, -1, GameMode::Vr, true};
    return ModeRead{true, value, value <= 1 ? GameMode::Flat : GameMode::Vr, true};
}

inline bool isLaunchSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// The switch in a command line. Tokens are whitespace-separated and compared case-insensitively; a quoted span is one token, so a
// "/novr" inside quotes (a path, say) does not count. The token keeps its quote characters, so only a bare /novr or /vr matches.
// The next token of a command line from position i, or empty at the end. A quoted span is one token and keeps its quote characters.
inline std::string nextLaunchToken(const std::string& s, size_t& i) {
    const size_t n = s.size();
    while (i < n && isLaunchSpace(s[i])) ++i;
    const size_t start = i;
    bool quoted = false;
    while (i < n && (quoted || !isLaunchSpace(s[i]))) {
        if (s[i] == '"') quoted = !quoted;
        ++i;
    }
    return s.substr(start, i - start);
}

inline VrSwitch parseVrSwitch(const std::string& commandLine) {
    VrSwitch result = VrSwitch::None;
    size_t i = 0;
    for (std::string token = nextLaunchToken(commandLine, i); !token.empty(); token = nextLaunchToken(commandLine, i)) {
        for (char& c : token) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (token == "/novr") result = VrSwitch::NoVr;
        else if (token == "/vr") result = VrSwitch::Vr;
    }
    return result;
}

// A bare switch: '/' or '-', then 1 to 32 characters of [A-Za-z0-9_] and nothing else. No '=', no quotes, no spaces.
inline bool isBareSwitch(const std::string& token) {
    if (token.size() < 2 || token.size() > 33) return false;
    if (token[0] != '/' && token[0] != '-') return false;
    for (size_t k = 1; k < token.size(); ++k) {
        const char c = token[k];
        const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!word) return false;
    }
    return true;
}

// The command line as the log may carry it, since logs are posted publicly: the program's file name only, bare switches kept, and
// every other token (a server token, an account, a password) replaced by <redacted>. Tokens are joined with single spaces.
inline std::string redactCommandLine(const std::string& commandLine) {
    size_t i = 0;
    std::string exe = nextLaunchToken(commandLine, i);
    if (exe.empty()) return std::string();
    std::string bare;
    for (char c : exe) {
        if (c != '"') bare += c;
    }
    const size_t slash = bare.find_last_of("\\/");
    if (slash != std::string::npos) bare.erase(0, slash + 1);
    std::string out = bare.empty() ? std::string("<redacted>") : bare;
    for (std::string token = nextLaunchToken(commandLine, i); !token.empty(); token = nextLaunchToken(commandLine, i)) {
        out += ' ';
        out += isBareSwitch(token) ? token : std::string("<redacted>");
    }
    return out;
}

inline const char* scopeName(InstallScope scope) {
    switch (scope) {
    case InstallScope::Both: return "both";
    case InstallScope::Vr: return "vr";
    case InstallScope::Flat: return "flat";
    default: return "invalid";
    }
}

inline const char* gameModeName(GameMode mode) { return mode == GameMode::Flat ? "flat" : "vr"; }

// The profile for a scope and a mode: the mode's profile when the scope includes it, Passthrough when it does not, Invalid for an
// invalid scope.
inline RuntimeProfile activeProfileFor(InstallScope scope, GameMode mode) {
    if (scope == InstallScope::Invalid) return RuntimeProfile::Invalid;
    if (mode == GameMode::Flat && (scope == InstallScope::Both || scope == InstallScope::Flat)) return RuntimeProfile::Flat;
    if (mode == GameMode::Vr && (scope == InstallScope::Both || scope == InstallScope::Vr)) return RuntimeProfile::Vr;
    return RuntimeProfile::Passthrough;
}

// Each switch applies only when its own flag is set. Production applies both: each overrides Settings.xml in the game (flights
// 2026-10-12: /steam /vr over a file at 0 went into VR; /steam /novr over a file at 4 stayed flat, with no openvr_api.dll load).
inline LaunchDecision decideLaunch(const ScopeRead& scope, const ModeRead& mode, VrSwitch cmd, bool applyVr, bool applyNoVr) {
    LaunchDecision d{};
    d.scope = scope;
    d.mode = mode;
    d.cmd = cmd;
    d.effective = mode.mode;
    if (cmd == VrSwitch::NoVr && applyNoVr) {
        d.effective = GameMode::Flat;
        d.cmdApplied = true;
    } else if (cmd == VrSwitch::Vr && applyVr) {
        d.effective = GameMode::Vr;
        d.cmdApplied = true;
    }
    d.active = activeProfileFor(scope.scope, d.effective);
    return d;
}

// The launch's one log line, without a trailing newline.
inline std::string formatProfileLine(const LaunchDecision& d, const char* activeName, const std::string& settingsPath) {
    if (d.scope.scope == InstallScope::Invalid)
        return "profile: invalid (edvr_profile.ini malformed; fixes disabled, proxy chaining retained; repair the installation)";
    std::string out = "profile: ";
    out += activeName;
    out += " (scope ";
    out += scopeName(d.scope.scope);
    if (d.scope.schema == 1) out += " from schema-1 descriptor";
    else if (!d.scope.present) out += " with no descriptor";
    out += "; 3D mode ";
    out += d.mode.known ? std::to_string(d.mode.value) : std::string("unknown");
    out += " from ";
    out += settingsPath;
    if (!d.mode.fileRead) out += " (file absent)";
    out += "; command line ";
    switch (d.cmd) {
    case VrSwitch::NoVr: out += "/novr"; break;
    case VrSwitch::Vr: out += "/vr"; break;
    default: out += "none"; break;
    }
    if (d.cmd != VrSwitch::None) {
        if (d.cmdApplied) out += " applied";
        else out += " not applied";
    }
    out += ")";
    return out;
}

}  // namespace edvr
