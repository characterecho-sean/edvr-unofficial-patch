// Elite's terrain checkerboard rendering, read from the game's ACTIVE graphics settings (design doc
// docs/design-flat-temporal-aa-2026-09-23.md, section 84; the words and what they are for are in
// src/common/terrain_checkerboard_notice.h).
//
// WHICH FILE IS THE TOGGLE. The game's options screen fills the option from the ACTIVE PRESET's file, so that is what is read:
//   Settings.xml  (%LOCALAPPDATA%\Frontier Developments\Elite Dangerous\Options\Graphics, src/common/elite_graphics_folder.h)
//                 names the preset in <PresetName>. It carries a <TerrainCheckerboardRenderingEnabled> of its own, which reads
//                 true on every machine seen (the one where the option is OFF in the game too) and is NOT the toggle: this
//                 file is asked for the preset's name and for nothing else.
//   Custom        the highest Custom.<major>.<minor>.fxcfg in the same folder (older versions linger beside it: 4.0, 4.1, 4.3
//                 next to 4.4). Only names that are exactly that shape count: -Custom.4.0.fxcfg, Custom.4.0.fxcfg-backup,
//                 Custom.4.4.fxcfg.baseline-bak-20260921 and TomCatT.4.0.fxcfg0 are not read. flatPickCustomFxcfg's highest
//                 version is what the flat F8 panel flew with and is kept here; matching the game's own version exactly would need
//                 the game's version, which nothing here has.
//   a stock preset  <game folder>\OptionDefaults\<Preset>.fxcfg (the game folder is the exe's, which is where this DLL sits). The
//                 stock names are the file stems: Low, Mid, High, Ultra, VRLow, VRMedium, VRHigh, VRUltra, and every VR one has the
//                 option ON. A name that could not be a file stem here, or has no file there (a user-made preset has its own
//                 files in the options folder, not read), is Unknown.
// The tag is a plain `<TerrainCheckerboardRenderingEnabled>true</...>` and a tag search does the reading (the same way
// flat_elite_settings.h reads AAMode): the folder listing, the highest-version pick, the whole-file read and the tag text are
// that header's own helpers, called here and not copied. Commentary (<!-- -->) is cut out first, so a commented-out line is
// never read, and the value must be true, false, 1 or 0 (any case) or the answer is Unknown.
//
// EVERYTHING FAILS SOFT, to Unknown, with the reason in words: no folder, no Settings.xml, no PresetName, no Custom file, a file
// that cannot be read, a preset that is not Custom or a stock one, a tag that is not there. Nothing is guessed: the game's own
// default for an absent tag is inferred, never seen, and an inference raises no notice.
//
// THE GAME REWRITES THE FILES IN PLACE at Apply (Settings.xml, then the Custom preset, no temporary file), so a read can catch one
// half written. Monitor below reads again at every poll and leaves a known state for Unknown only when two reads in a row say
// Unknown (kUnknownReads); a read that is a known state is believed at once (the tag is early in the file, and the new value is
// written in order).
//
// Header-only: the code here is what src/d3d11/terrain_checkerboard.cpp runs on its worker thread, and tools\terrain_checkerboard_test
// drives it on fixture folders. It reads files and never writes one; no Elite process state, no config key.
#pragma once

#include "../common/terrain_checkerboard_notice.h"
#include "flat_elite_settings.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace edvr {
namespace tcn {

// The tag, as the files spell it.
inline constexpr char kTag[] = "TerrainCheckerboardRenderingEnabled";

// What one read found. The strings are for the log only (one line each, control bytes replaced); the render thread never sees them.
struct Reading {
    State state = State::Unknown;
    char preset[48] = {};    // the preset Settings.xml names, "" when it names none
    char source[96] = {};    // the file the answer came from: Custom.4.4.fxcfg or OptionDefaults\VRHigh.fxcfg, "" when none was chosen
    char value[24] = {};     // the tag's text as written (true, False, 1, ...), "" when none
    char reason[220] = {};   // for Unknown: why, in words
    bool operator==(const Reading& o) const {
        return state == o.state && !std::strcmp(preset, o.preset) && !std::strcmp(source, o.source) && !std::strcmp(value, o.value) &&
               !std::strcmp(reason, o.reason);
    }
    bool operator!=(const Reading& o) const { return !(*this == o); }
};

// ---- small pure pieces -----------------------------------------------------------------------------------------------------
// A field for the log: at most cap-1 bytes, one line, a control byte shown as '?' whatever the file holds.
inline void readerCopy(char* dst, size_t cap, const std::string& src) {
    if (!cap) return;
    const size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        dst[i] = (c < 0x20 || c == 0x7F) ? '?' : static_cast<char>(c);
    }
    dst[n] = 0;
}

// The text with every <!-- ... --> cut out; an unterminated comment takes the rest of the text with it.
inline std::string stripXmlComments(const std::string& xml) {
    std::string out;
    out.reserve(xml.size());
    size_t i = 0;
    while (i < xml.size()) {
        if (xml.compare(i, 4, "<!--") == 0) {
            const size_t end = xml.find("-->", i + 4);
            if (end == std::string::npos) break;
            i = end + 3;
            continue;
        }
        out += xml[i++];
    }
    return out;
}

// Whether a preset's name could be a file stem in OptionDefaults: letters, digits, underscore and hyphen, 32 at most. The name comes
// from a file anybody can edit and ends up in a path, so nothing else (no separator, no dot, no drive) is ever put in one.
inline bool plausibleStockPreset(const std::string& name) {
    if (name.empty() || name.size() > 32) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

inline void setReason(Reading* r, const char* fmt, ...) {
    r->state = State::Unknown;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(r->reason, sizeof(r->reason), fmt, ap);
    va_end(ap);
}

// ---- the read --------------------------------------------------------------------------------------------------------------
// One read of the active preset's toggle. `folder` is Elite's Options\Graphics folder, `gameDir` the folder of the game's exe.
inline Reading readTerrainCheckerboard(const std::wstring& folder, const std::wstring& gameDir) {
    Reading r;
    const EliteFolderListing listing = flatListEliteGraphics(folder);
    if (!listing.folder) {
        setReason(&r, "no Elite graphics settings folder was found at Frontier Developments\\Elite Dangerous\\Options\\Graphics under LocalAppData");
        return r;
    }
    std::string settings;
    if (!listing.settings || !flatReadSmallFile(folder + L"\\Settings.xml", &settings)) {
        setReason(&r, "Settings.xml is missing or unreadable, so the active preset is not known");
        return r;
    }
    // Settings.xml is asked for the preset's name and nothing else: its own tag of the same name is not the toggle.
    std::string preset;
    if (!flatXmlText(stripXmlComments(settings), "PresetName", &preset) || preset.empty()) {
        setReason(&r, "Settings.xml names no PresetName, so the active preset is not known");
        return r;
    }
    readerCopy(r.preset, sizeof(r.preset), preset);

    std::string xml;
    if (_stricmp(preset.c_str(), "Custom") == 0) {
        // Only names of exactly the Custom.<major>.<minor>.fxcfg shape are the Custom preset's: flatPickCustomFxcfg then takes the
        // highest version. Its fallback for a name with no version (the newest file starting with Custom) would pick a lookalike
        // the game never reads, so it is never reached: nothing unversioned is handed to it.
        std::vector<EliteFxcfg> versioned;
        for (const EliteFxcfg& f : listing.fxcfg) {
            unsigned major = 0, minor = 0;
            if (flatCustomFxcfgVersion(f.name, &major, &minor)) versioned.push_back(f);
        }
        const int pick = flatPickCustomFxcfg(versioned);
        if (pick < 0) {
            setReason(&r, "preset Custom but the folder has no Custom.<major>.<minor>.fxcfg");
            return r;
        }
        const std::string& name = versioned[static_cast<size_t>(pick)].name;
        readerCopy(r.source, sizeof(r.source), name);
        if (!flatReadSmallFile(folder + L"\\" + std::wstring(name.begin(), name.end()), &xml)) {
            setReason(&r, "preset Custom but %s could not be read", r.source);
            return r;
        }
    } else {
        if (!plausibleStockPreset(preset)) {
            setReason(&r, "preset %s is not Custom or a stock preset, so no file is read for it", r.preset);
            return r;
        }
        readerCopy(r.source, sizeof(r.source), "OptionDefaults\\" + preset + ".fxcfg");
        if (gameDir.empty() ||
            !flatReadSmallFile(gameDir + L"\\OptionDefaults\\" + std::wstring(preset.begin(), preset.end()) + L".fxcfg", &xml)) {
            setReason(&r, "preset %s has no readable %s beside the game, so it is not a stock preset Elite ships", r.preset, r.source);
            return r;
        }
    }

    std::string text;
    if (!flatXmlText(stripXmlComments(xml), kTag, &text)) {
        setReason(&r, "%s has no %s", r.source, kTag);
        return r;
    }
    readerCopy(r.value, sizeof(r.value), text);
    if (_stricmp(text.c_str(), "true") == 0 || text == "1") r.state = State::On;
    else if (_stricmp(text.c_str(), "false") == 0 || text == "0") r.state = State::Off;
    else setReason(&r, "%s holds %s=%s, which is neither true nor false", r.source, kTag, r.value);
    return r;
}

// ---- the poll --------------------------------------------------------------------------------------------------------------
// The worker's whole decision, with no thread in it so a rig can drive it: read, apply the rule for a read that disagrees with the
// last good answer, say whether anything changed. Owned by one thread.
class Monitor {
public:
    void configure(const std::wstring& folder, const std::wstring& gameDir) {
        folder_ = folder;
        gameDir_ = gameDir;
        pub_ = Reading{};
        have_ = false;
        strikes_ = 0;
    }
    // One poll. True when what is published changed in any way the log shows (the first read always is one); `*stateMoved` is
    // whether the STATE changed (a new version to publish: the first read counts, whatever it says).
    bool poll(bool* stateMoved) {
        if (stateMoved) *stateMoved = false;
        const Reading cur = readTerrainCheckerboard(folder_, gameDir_);
        if (!have_) {
            pub_ = cur;
            have_ = true;
            strikes_ = 0;
            if (stateMoved) *stateMoved = true;
            return true;
        }
        // A known state stands against Unknown reads until kUnknownReads of them agree: the files can be caught mid-write.
        if (cur.state == State::Unknown && pub_.state != State::Unknown && ++strikes_ < kUnknownReads) return false;
        strikes_ = 0;
        if (cur == pub_) return false;
        const bool moved = cur.state != pub_.state;
        pub_ = cur;
        if (stateMoved) *stateMoved = moved;
        return true;
    }
    const Reading& published() const { return pub_; }
    bool havePublished() const { return have_; }

private:
    std::wstring folder_, gameDir_;
    Reading pub_;
    bool have_ = false;
    int strikes_ = 0;
};

}  // namespace tcn
}  // namespace edvr
