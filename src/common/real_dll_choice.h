// Which other mod's d3d11 EDVR chains through, decided from the settings files (2026-10-14).
//
// advanced.real_dll names the mod (EDHM, ReShade) that was renamed aside so EDVR could take the
// d3d11.dll name. It is ONE decision about the install, not one per profile, and until now it was
// read from the launch profile's own settings file alone: a flat start read edvr-flat.ini whole, a
// VR start edvr.ini whole. Set in one and not the other, EDHM chained in one profile and silently
// not in the other (a user's install ended up with it only in edvr-flat.ini, so the VR profile
// never chained it). Nobody should set it twice.
//
// So the value lives once, in the shared file (edvr.ini), and applies to both profiles:
//
//   VR     the shared file's value.
//   flat   the flat file's own value when it holds one, else the shared file's.
//
// The flat file can differ from the shared one for exactly one reason: to say `none`, "this
// profile runs without the chain". A flat file that names a mod while the shared file names
// none is the layout from before this rule (the installer moves it to the shared file on its next
// run); it is honoured and logged as such, never dropped.
//
// This header is the whole decision as a pure function of the two values, so the rig can hold
// every case without a file system. Reading the files is the caller's (d3d11_proxy.cpp), and the
// installer applies the same rule to what it writes (src/installer/plan.cpp). It spells no file
// name: the caller passes them (ini_name.h), so a message here can never name the wrong file.
#pragma once

#include <string>

namespace edvr {

enum class RealDllSource {
    NoneSet,           // neither file names a mod: nothing to chain, nothing to say
    Shared,            // the shared file's value, for either profile
    FlatOverride,      // flat only: the flat file's value, with the shared file naming one too (or `none`)
    FlatLegacy,        // flat only: the flat file's value and the shared file has none (the old layout)
    DisabledByFlat,    // flat only: the flat file says `none`
    DisabledByShared,  // the shared file says `none`
};

struct RealDllChoice {
    RealDllSource source = RealDllSource::NoneSet;
    std::string   value;   // the file name to chain through; empty unless a mod is named
    bool chains() const { return !value.empty(); }
};

// The value as the ini reader hands it, with the blanks around it gone.
inline std::string realDllTrimmed(const std::string& v) {
    size_t a = 0, b = v.size();
    while (a < b && (v[a] == ' ' || v[a] == '\t' || v[a] == '\r' || v[a] == '\n')) ++a;
    while (b > a && (v[b - 1] == ' ' || v[b - 1] == '\t' || v[b - 1] == '\r' || v[b - 1] == '\n')) --b;
    return v.substr(a, b - a);
}

// `none`, in any case: the value that turns the chain off where a name would turn it on.
inline bool realDllIsNone(const std::string& v) {
    const std::string t = realDllTrimmed(v);
    if (t.size() != 4) return false;
    const char* want = "none";
    for (size_t i = 0; i < 4; ++i) {
        const char c = t[i] >= 'A' && t[i] <= 'Z' ? static_cast<char>(t[i] - 'A' + 'a') : t[i];
        if (c != want[i]) return false;
    }
    return true;
}

// flatProfile: the launch profile. flatValue: advanced.real_dll in the flat file ("" when the file
// is absent or the key empty). sharedValue: the same key in the shared file. A VR launch ignores
// flatValue by construction: the flat file is not its file.
inline RealDllChoice chooseRealDll(bool flatProfile, const std::string& flatValue,
                                   const std::string& sharedValue) {
    RealDllChoice out;
    const std::string shared = realDllTrimmed(sharedValue);
    if (flatProfile) {
        const std::string own = realDllTrimmed(flatValue);
        if (realDllIsNone(own)) {
            out.source = RealDllSource::DisabledByFlat;
            return out;
        }
        if (!own.empty()) {
            out.source = shared.empty() ? RealDllSource::FlatLegacy : RealDllSource::FlatOverride;
            out.value = own;
            return out;
        }
    }
    if (realDllIsNone(shared)) {
        out.source = RealDllSource::DisabledByShared;
    } else if (!shared.empty()) {
        out.source = RealDllSource::Shared;
        out.value = shared;
    }
    return out;
}

// The one log line that names the value and the file that supplied it; empty for NoneSet.
// sharedName and flatName are the settings files' names (ini_name.h) as the caller has them.
inline std::string realDllLogLine(const RealDllChoice& c, bool flatProfile,
                                  const char* sharedName, const char* flatName) {
    const std::string shared = sharedName ? sharedName : "";
    const std::string flat = flatName ? flatName : "";
    const std::string profile = flatProfile ? "flat" : "VR";
    switch (c.source) {
        case RealDllSource::Shared:
            return "real dll: " + c.value + " (from " + shared + ", shared)";
        case RealDllSource::FlatOverride:
            return "real dll: " + c.value + " (from " + flat + ", the flat override)";
        case RealDllSource::FlatLegacy:
            return "real dll: " + c.value + " (from " + flat + "; the installer will move it to " +
                   shared + ")";
        case RealDllSource::DisabledByFlat:
            return "real dll: none: chain disabled for the " + profile + " profile by " + flat;
        case RealDllSource::DisabledByShared:
            return "real dll: none: chain disabled for the " + profile + " profile by " + shared;
        case RealDllSource::NoneSet:
            break;
    }
    return std::string();
}

}  // namespace edvr
