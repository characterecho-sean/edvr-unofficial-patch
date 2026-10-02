// Elite's own graphics settings, read for the flat F8 panel's warning.
//
// WHY. Two rc.4 users got no temporal AA at all: the game's anti-aliasing (user 1)
// or bloom (user 2) put passes between the tone pass and the final copy, the
// selector refused every frame, and nothing told either of them why. When the
// runtime is refusing frames for the shape of the post chain (flat_standdown.h)
// the panel says so and names the game settings that can cause it. The warning is
// gated on that refusal, never on the settings alone: settings-only rules would
// fire on combinations that work (the all-maxed DoF and bloom test is treated).
//
// WHAT IS READ. %LOCALAPPDATA%\Frontier Developments\Elite Dangerous\Options\
// Graphics\ (src/common/elite_graphics_folder.h): Settings.xml names the preset
// (<PresetName>); for "Custom" the Custom preset's file, Custom.<major>.<minor>.fxcfg,
// carries <AAMode>, <BloomQuality> and <DOFEnabled>, each 0 for off. Older versions'
// files linger beside the current one, so the file read is the HIGHEST <major>.<minor>
// present -- the one the game reads -- and the newest write time only where no name
// carries a version. For any other preset the warning names the preset and says it
// may turn the effects on; there is no preset parser.
//
// The tags are plain (`<AAMode>4</AAMode>`, one per line inside <Root>), so a
// tag search does the reading, the same way device_hook.cpp reads
// <HMDRenderTargetMultiplier>. Everything here fails soft: no folder, no file, no
// field leaves the value unknown, and the warning then asks for the logs.
//
// Header-only: the parsing and the text are pure (tools\flat_temporal_test drives
// them on fixtures); the folder reading needs kernel32 only.
#pragma once

#include "../common/elite_graphics_folder.h"
#include "flat_mono_frame.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace edvr {

// ---- what is read ------------------------------------------------------------------
struct EliteGraphics {
    bool folderFound = false;   // Options\Graphics exists
    bool presetKnown = false;   // Settings.xml gave a <PresetName>
    bool custom = false;        // ... and it is Custom
    char preset[48] = {};       // the preset as written
    bool fileRead = false;      // a Custom.<major>.<minor>.fxcfg was read
    char file[80] = {};         // its name
    // -1: the field was not in the file (or the file was not read). 0 is off.
    int aaMode = -1, bloomQuality = -1, dofEnabled = -1;
    bool aaOn() const { return aaMode > 0; }
    bool bloomOn() const { return bloomQuality > 0; }
    bool dofOn() const { return dofEnabled > 0; }
    bool anyOn() const { return aaOn() || bloomOn() || dofOn(); }
};

// ---- the tags ------------------------------------------------------------------------
// The text of <tag>...</tag>, trimmed. A missing close tag ends the text at the next '<'.
inline bool flatXmlText(const std::string& xml, const char* tag, std::string* out) {
    const std::string open = std::string("<") + tag + ">";
    size_t at = xml.find(open);
    if (at == std::string::npos) return false;
    at += open.size();
    size_t end = xml.find('<', at);
    if (end == std::string::npos) end = xml.size();
    size_t a = at, b = end;
    while (a < b && (xml[a] == ' ' || xml[a] == '\t' || xml[a] == '\r' || xml[a] == '\n')) ++a;
    while (b > a && (xml[b - 1] == ' ' || xml[b - 1] == '\t' || xml[b - 1] == '\r' || xml[b - 1] == '\n')) --b;
    if (out) *out = xml.substr(a, b - a);
    return true;
}
// A whole number, also in the true/false dialect some fields are written in.
inline bool flatXmlInt(const std::string& xml, const char* tag, int* out) {
    std::string text;
    if (!flatXmlText(xml, tag, &text) || text.empty()) return false;
    if (_stricmp(text.c_str(), "true") == 0) { if (out) *out = 1; return true; }
    if (_stricmp(text.c_str(), "false") == 0) { if (out) *out = 0; return true; }
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str()) return false;
    if (out) *out = static_cast<int>(value);
    return true;
}

// ---- which fxcfg ---------------------------------------------------------------------
struct EliteFxcfg {
    std::string name;
    uint64_t mtime = 0;   // last write, FILETIME ticks
};
inline bool flatEndsWithNoCase(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && _strnicmp(s.c_str() + s.size() - n, suffix, n) == 0;
}
inline bool flatStartsWithNoCase(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && _strnicmp(s.c_str(), prefix, n) == 0;
}
// Custom.<major>.<minor>.fxcfg -> the two numbers. Custom.fxcfg is (0,0), Custom.4.fxcfg is (4,0).
// Anything else (another preset, a backup suffix, letters where numbers go) is not a version.
inline bool flatCustomFxcfgVersion(const std::string& name, unsigned* major, unsigned* minor) {
    if (!flatEndsWithNoCase(name, ".fxcfg") || !flatStartsWithNoCase(name, "Custom.")) return false;
    if (_stricmp(name.c_str(), "Custom.fxcfg") == 0) {
        if (major) *major = 0;
        if (minor) *minor = 0;
        return true;
    }
    if (name.size() <= 7 + 6) return false;
    const std::string middle = name.substr(7, name.size() - 7 - 6);
    unsigned parts[2] = {0, 0};
    unsigned count = 0;
    size_t i = 0;
    while (i < middle.size()) {
        if (count == 2) return false;
        size_t j = i;
        unsigned long value = 0;
        while (j < middle.size() && middle[j] >= '0' && middle[j] <= '9') {
            value = value * 10 + static_cast<unsigned long>(middle[j] - '0');
            if (value > 1000000ul) return false;
            ++j;
        }
        if (j == i) return false;
        parts[count++] = static_cast<unsigned>(value);
        if (j < middle.size()) {
            if (middle[j] != '.') return false;
            ++j;
            if (j == middle.size()) return false;
        }
        i = j;
    }
    if (count == 0) return false;
    if (major) *major = parts[0];
    if (minor) *minor = parts[1];
    return true;
}
// The Custom preset's file: the highest <major>.<minor> present, which is what the
// game reads, with the newest write time breaking a tie. Where no Custom name carries
// a version the newest write time stands in. -1 when there is no Custom file at all.
inline int flatPickCustomFxcfg(const std::vector<EliteFxcfg>& files) {
    int best = -1;
    unsigned bestMajor = 0, bestMinor = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        unsigned major = 0, minor = 0;
        if (!flatCustomFxcfgVersion(files[i].name, &major, &minor)) continue;
        const bool better = best < 0 || major > bestMajor || (major == bestMajor && minor > bestMinor) ||
            (major == bestMajor && minor == bestMinor && files[i].mtime > files[best].mtime);
        if (better) { best = static_cast<int>(i); bestMajor = major; bestMinor = minor; }
    }
    if (best >= 0) return best;
    for (size_t i = 0; i < files.size(); ++i) {
        if (!flatStartsWithNoCase(files[i].name, "Custom")) continue;
        if (best < 0 || files[i].mtime > files[best].mtime) best = static_cast<int>(i);
    }
    return best;
}

// ---- the folder ----------------------------------------------------------------------
struct EliteFolderListing {
    bool folder = false;
    bool settings = false;          // Settings.xml is there
    uint64_t settingsTime = 0;
    std::vector<EliteFxcfg> fxcfg;  // every *.fxcfg
};
inline uint64_t flatFileTimeTicks(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
inline std::string flatNarrow(const wchar_t* w) {
    if (!w || !*w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    return out;
}
// One pass over the folder: Settings.xml's write time and every *.fxcfg's name and write time.
inline EliteFolderListing flatListEliteGraphics(const std::wstring& folder) {
    EliteFolderListing listing;
    if (folder.empty()) return listing;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return listing;
    listing.folder = true;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string name = flatNarrow(fd.cFileName);
        if (_stricmp(name.c_str(), "Settings.xml") == 0) {
            listing.settings = true;
            listing.settingsTime = flatFileTimeTicks(fd.ftLastWriteTime);
        } else if (flatEndsWithNoCase(name, ".fxcfg")) {
            EliteFxcfg f;
            f.name = name;
            f.mtime = flatFileTimeTicks(fd.ftLastWriteTime);
            listing.fxcfg.push_back(f);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return listing;
}
// A small text file, whole; empty on any failure. These are KB-sized XML.
inline bool flatReadSmallFile(const std::wstring& path, std::string* text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(f, nullptr);
    bool ok = false;
    if (size != INVALID_FILE_SIZE && size <= (1u << 20)) {
        text->assign(size, '\0');
        DWORD got = 0;
        ok = size == 0 || (ReadFile(f, &(*text)[0], size, &got, nullptr) && got == size);
        if (!ok) text->clear();
    }
    CloseHandle(f);
    return ok;
}
inline void flatCopyName(char* dst, size_t cap, const std::string& src) {
    if (!cap) return;
    const size_t n = std::min(src.size(), cap - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = 0;
}
// What the listing says, read: the preset, and for Custom the chosen file's three fields.
inline EliteGraphics flatReadEliteGraphics(const std::wstring& folder, const EliteFolderListing& listing) {
    EliteGraphics g;
    g.folderFound = listing.folder;
    if (!listing.folder) return g;
    std::string xml;
    if (listing.settings && flatReadSmallFile(folder + L"\\Settings.xml", &xml)) {
        std::string preset;
        if (flatXmlText(xml, "PresetName", &preset) && !preset.empty()) {
            g.presetKnown = true;
            flatCopyName(g.preset, sizeof(g.preset), preset);
            g.custom = _stricmp(preset.c_str(), "Custom") == 0;
        }
    }
    if (g.custom) {
        const int pick = flatPickCustomFxcfg(listing.fxcfg);
        std::string text;
        if (pick >= 0 && flatReadSmallFile(folder + L"\\" + std::wstring(listing.fxcfg[pick].name.begin(),
                                                                          listing.fxcfg[pick].name.end()), &text)) {
            g.fileRead = true;
            flatCopyName(g.file, sizeof(g.file), listing.fxcfg[pick].name);
            flatXmlInt(text, "AAMode", &g.aaMode);
            flatXmlInt(text, "BloomQuality", &g.bloomQuality);
            flatXmlInt(text, "DOFEnabled", &g.dofEnabled);
        }
    }
    return g;
}
// %LOCALAPPDATA% the way device_hook.cpp's eliteHmdMultiplier resolves it: the DLL links no
// shell library.
inline std::wstring flatEliteGraphicsFolder() {
    wchar_t appdata[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return eliteGraphicsFolderUnder(appdata);
}

// ---- the watcher -----------------------------------------------------------------------
// Re-reads when the files change. The game rewrites them when settings are applied, so
// the folder is listed at most every kCheckMs, or at once when the caller forces it (the
// menu opening), and the two files are read again only when a write time, the chosen
// file's name or the file set changed.
class FlatSettingsWatcher {
public:
    static constexpr uint64_t kCheckMs = 2000;
    void setFolder(const std::wstring& folder) { folder_ = folder; checked_ = false; sig_ = Signature{}; }
    // True when the settings were (re)read on this call.
    bool poll(uint64_t nowMs, bool force) {
        if (!force && checked_ && nowMs - lastCheckMs_ < kCheckMs) return false;
        lastCheckMs_ = nowMs;
        const EliteFolderListing listing = flatListEliteGraphics(folder_);
        Signature sig;
        sig.folder = listing.folder;
        sig.settings = listing.settings;
        sig.settingsTime = listing.settingsTime;
        const int pick = flatPickCustomFxcfg(listing.fxcfg);
        if (pick >= 0) {
            sig.file = listing.fxcfg[pick].name;
            sig.fileTime = listing.fxcfg[pick].mtime;
        }
        sig.count = listing.fxcfg.size();
        if (checked_ && sig == sig_) return false;
        sig_ = sig;
        checked_ = true;
        g_ = flatReadEliteGraphics(folder_, listing);
        ++reads_;
        return true;
    }
    const EliteGraphics& settings() const { return g_; }
    bool everRead() const { return checked_; }
    uint64_t reads() const { return reads_; }

private:
    struct Signature {
        bool folder = false, settings = false;
        uint64_t settingsTime = 0, fileTime = 0;
        std::string file;
        size_t count = 0;
        bool operator==(const Signature& o) const {
            return folder == o.folder && settings == o.settings && settingsTime == o.settingsTime &&
                   fileTime == o.fileTime && file == o.file && count == o.count;
        }
    };
    std::wstring folder_;
    bool checked_ = false;
    uint64_t lastCheckMs_ = 0, reads_ = 0;
    Signature sig_;
    EliteGraphics g_;
};

// ---- the words -------------------------------------------------------------------------
// Twelve lines: the flat page has three rows, so a blank line and twelve of these are the card's sixteen (menu_panel.h's
// kMenuMaxLines; menu.cpp static_asserts it). The two paragraphs take up to six at the panel's width (40 characters a
// line in the rig's ruler), the third (EDVR's TAA above the output) four more: ten at worst, and the two spare are for a
// ruler wider than the rig's rather than a cut-off sentence.
struct FlatSettingsWarning {
    static constexpr int kMaxLines = 12;
    char line[kMaxLines][160] = {};
    int count = 0;
};
// The extra paragraph for EDVR's own TAA with the scene rendering above the output (design section 83): neither route
// resolves a post chain the whitelist does not know there. The HDR route evaluates at the render size and TAA at the output's
// (flatHdrRouteEvaluatesAtRender), and the copy's admission by structure leaves a chain with a filter after the tone pass. DLSS
// and FSR resolve before the post chain at any render size from the output's up, so they are the way out, and so is rendering
// at or below the output.
inline constexpr char kFlatTaaAboveWords[] =
    "Above 1.0 supersampling, EDVR's TAA works only on a post chain it knows. Set Elite's supersampling to 1.0 or "
    "lower, or choose DLSS or FSR.";
static_assert(sizeof(kFlatTaaAboveWords) <= sizeof(FlatSettingsWarning{}.line[0]),
              "the TAA paragraph fits one unwrapped log line");
// The panel's ruler: the width in pixels of one line of note text (menuPanelMeasureLine at the
// note face's em). A rig gives it a fixed pitch.
using FlatMeasureFn = int (*)(const char* utf8, void* context);

inline void flatWrapWarning(const std::string& paragraph, int widthPx, FlatMeasureFn measure, void* context,
                            FlatSettingsWarning* out) {
    std::string current;
    size_t i = 0;
    auto flush = [&] {
        if (current.empty() || out->count >= FlatSettingsWarning::kMaxLines) { current.clear(); return; }
        flatCopyName(out->line[out->count], sizeof(out->line[0]), current);
        ++out->count;
        current.clear();
    };
    while (i < paragraph.size()) {
        while (i < paragraph.size() && paragraph[i] == ' ') ++i;
        size_t j = i;
        while (j < paragraph.size() && paragraph[j] != ' ') ++j;
        if (j == i) break;
        const std::string word = paragraph.substr(i, j - i);
        const std::string trial = current.empty() ? word : current + " " + word;
        if (!current.empty() && measure && measure(trial.c_str(), context) > widthPx) {
            flush();
            current = word;
        } else {
            current = trial;
        }
        i = j;
    }
    flush();
}

// What the warning knows beyond Elite's files, all from the runtime's own measurements (design section 83), never from the
// settings: whether the route's key is auto (the game's final copy is then admitted by its structure, so Bloom and Depth of
// field never cause a refusal and their advice is dropped; the Anti-aliasing advice stays, because a filter after the tone
// pass is what keeps a frame refused), whether the refusal is the render size's (the scene's size is not a uniform scale of
// the output between half and twice: Elite's resolution is not the screen's shape), and whether it is EDVR's TAA with the
// scene above the output. The sizes are the scene's (the R11G11B10F target it is drawn into) and the swap chain's.
struct FlatWarningCause {
    bool structureAdmission = false;
    bool renderSize = false;
    bool taaAbove = false;
    uint32_t renderW = 0, renderH = 0, outputW = 0, outputH = 0;
};

// The warning, as wrapped note lines. Called only while the runtime is refusing frames for the shape of the post chain, or
// for a render size that does not fit. `modeLabel` is the selected mode as the panel names it (DLSS, DLAA, TAA, FSR3).
// A render-size refusal says what it is, in the user's terms, and nothing else: "Elite renders 2176x1224 on a 2560x1600
// screen", then what to set (the resolution to the screen's where the shape is off, the supersampling where the size is
// out of the half-to-twice band). Any other refusal is the post chain's: "Elite's post-processing is not recognised", then,
// from Elite's own files, the settings that can cause it (Bloom and Depth of field only with the route's key off), and a
// third paragraph for EDVR's TAA above the output.
inline void flatComposeSettingsWarning(const char* modeLabel, const EliteGraphics& g, int widthPx,
                                       FlatMeasureFn measure, void* context, FlatSettingsWarning* out,
                                       const FlatWarningCause& cause = FlatWarningCause{}) {
    *out = FlatSettingsWarning{};
    const char* label = modeLabel && *modeLabel ? modeLabel : "Anti-aliasing";
    char first[200];
    if (cause.renderSize && cause.renderW && cause.renderH && cause.outputW && cause.outputH) {
        std::snprintf(first, sizeof(first), "%s is not active: Elite renders %ux%u on a %ux%u screen.", label, cause.renderW,
                      cause.renderH, cause.outputW, cause.outputH);
        flatWrapWarning(first, widthPx, measure, context, out);
        char second[200];
        if (!flatUniformScale(cause.renderW, cause.renderH, cause.outputW, cause.outputH))
            std::snprintf(second, sizeof(second),
                          "Set Elite's resolution to your screen's, %ux%u, and change the render size with its supersampling.",
                          cause.outputW, cause.outputH);
        else if (uint64_t(cause.renderW) * 2 < cause.outputW || uint64_t(cause.renderH) * 2 < cause.outputH)
            std::snprintf(second, sizeof(second), "That is under half the screen's size. Raise Elite's supersampling.");
        else
            std::snprintf(second, sizeof(second), "That is over twice the screen's size. Lower Elite's supersampling.");
        flatWrapWarning(second, widthPx, measure, context, out);
        return;
    }
    std::snprintf(first, sizeof(first), "%s is not active: Elite's post-processing is not recognised.", label);
    flatWrapWarning(first, widthPx, measure, context, out);
    std::string second;
    if (g.presetKnown && !g.custom) {
        second = cause.structureAdmission
            ? std::string("Elite's ") + g.preset + " graphics preset may turn on Anti-aliasing. Turn it off in "
              "Elite's graphics options."
            : std::string("Elite's ") + g.preset + " graphics preset may turn on Anti-aliasing, Bloom or "
              "Depth of field. Turn them off in Elite's graphics options.";
    } else if (g.custom && (cause.structureAdmission ? g.aaOn() : g.anyOn())) {
        std::string list;
        auto add = [&](bool on, const char* name) {
            if (!on) return;
            if (!list.empty()) list += ", ";
            list += name;
        };
        add(g.aaOn(), "Anti-aliasing");
        if (!cause.structureAdmission) {
            add(g.bloomOn(), "Bloom");
            add(g.dofOn(), "Depth of field");
        }
        second = "Turn off in Elite's graphics options: " + list;
    } else {
        second = "Please send your logs (F10 in the cockpit, then the installer's log bundle).";
    }
    flatWrapWarning(second, widthPx, measure, context, out);
    if (cause.taaAbove) flatWrapWarning(kFlatTaaAboveWords, widthPx, measure, context, out);
}

// A short key for the composed warning: it changes when the words would, so the panel rebuilds (and the log speaks) exactly
// then. Every condition and, where the words name them, the sizes are part of it: the text appears, goes and changes live as
// Elite's resolution or supersampling does.
inline std::string flatSettingsWarningKey(const char* modeLabel, const EliteGraphics& g,
                                          const FlatWarningCause& cause = FlatWarningCause{}) {
    char key[260];
    const bool sizes = cause.renderSize || cause.taaAbove;
    std::snprintf(key, sizeof(key), "%s|%d|%d|%s|%d|%d|%d|%d|%d|%d|%u|%u|%u|%u", modeLabel ? modeLabel : "",
                  g.presetKnown ? 1 : 0, g.custom ? 1 : 0, g.presetKnown ? g.preset : "",
                  g.aaOn() ? 1 : 0, g.bloomOn() ? 1 : 0, g.dofOn() ? 1 : 0, cause.structureAdmission ? 1 : 0,
                  cause.renderSize ? 1 : 0, cause.taaAbove ? 1 : 0, sizes ? cause.renderW : 0u, sizes ? cause.renderH : 0u,
                  sizes ? cause.outputW : 0u, sizes ? cause.outputH : 0u);
    return key;
}

// Which of the warning's conditions hold, from what the runtime publishes. The warning exists only while frames are refused
// (`refusing`); the key being auto drops the Bloom and Depth of field advice; a render-size refusal needs the runtime's
// measured sizes; EDVR's TAA above the output is the post-chain refusal's third paragraph (never alongside a render-size one).
// One function, so the panel, the log and the rig read the same truth table.
inline FlatWarningCause flatWarningCause(bool refusing, bool structureAdmission, bool taaAbove, bool reasonIsRenderSize,
                                         bool sizesKnown, uint32_t renderW, uint32_t renderH, uint32_t outputW,
                                         uint32_t outputH) {
    FlatWarningCause c;
    if (!refusing) return c;
    c.structureAdmission = structureAdmission;
    c.renderSize = reasonIsRenderSize && sizesKnown;
    c.taaAbove = !c.renderSize && structureAdmission && taaAbove && sizesKnown;
    if (sizesKnown) { c.renderW = renderW; c.renderH = renderH; c.outputW = outputW; c.outputH = outputH; }
    return c;
}

// A warning on show keeps the cause it shows until a different one has persisted. WHY. The scene's size, which the render-size
// words name, moves only when a final copy is evaluated: every frame while the work is treated, but only at the stand-down's
// probe frames (kFlatStandDownProbeMs, 1500 ms) while it is stood down. A loading screen's 256x256 seen by one probe is then the
// computed cause for exactly one probe interval, and in the flight of 2026-10-01 (10:38:16 to 10:38:36, Elite rendering 1440x810
// on a 3840x2160 screen) the words flipped five times in nine seconds, the transients lasting 1.509 s and 1.521 s. No real state
// that carries a message lasted under 3.0 s (the stand-down has already waited five seconds before the warning shows), and two
// agreeing probes are 2000 ms: that is the hold. It is a hold and not a size filter, because a transient size dropped as "no
// scene" would HIDE the warning, which is a different flicker. A show and a hide stay immediate. The hold is the panel's: the
// stand-down's gate (FlatStandDown::warningActive) keeps no clock and still keeps none, and nothing here reads one either, the
// caller hands it the time.
constexpr uint64_t kFlatWarnHoldMs = 2000;

struct FlatWarnHold {
    // True when the computed state may be acted on this tick; false while a computed cause that differs from the one on show
    // waits out its hold, and the caller then leaves what is on show alone. `refusing` is whether a warning is wanted now,
    // `active` whether one is on show, `shownKey` the key on show and `computedKey` this tick's. Asked every tick and before the
    // caller's own comparison: a computed key that comes back to the shown one drops the change that was waiting, and one that
    // changes to a third key starts that key's own wait. A show (refusing, nothing on show) and a hide (on show, no longer
    // refusing) pass at once and clear any wait. `holdMs` is the constant everywhere but the rig's controls.
    bool admit(bool refusing, bool active, const std::string& shownKey, const std::string& computedKey, uint64_t nowMs,
               uint64_t holdMs = kFlatWarnHoldMs) {
        began_ = false;
        if (!refusing || !active || computedKey == shownKey) {
            pending_ = false;
            return true;
        }
        if (!pending_ || computedKey != pendingKey_) {
            pending_ = true;
            pendingKey_ = computedKey;
            sinceMs_ = nowMs;
            began_ = true;
        } else if (nowMs < sinceMs_) {
            sinceMs_ = nowMs;   // a clock that stepped back restarts the wait; it never stretches it
        }
        if (nowMs - sinceMs_ < holdMs) return false;
        pending_ = false;
        return true;
    }
    // True after the call that started holding a cause (a first one or a third), so the caller says so once.
    bool began() const { return began_; }
    bool pending() const { return pending_; }
    const std::string& pendingKey() const { return pendingKey_; }
    uint64_t sinceMs() const { return sinceMs_; }

private:
    bool pending_ = false, began_ = false;
    std::string pendingKey_;
    uint64_t sinceMs_ = 0;
};

// The log line for a shown or changed warning: the conditions in the brackets, then the composed paragraphs as the panel would
// say them (unwrapped, one per line, joined with " | " so the Custom list, which has no final period, does not run into the
// next), so the log carries every paragraph the panel does.
inline int flatFormatSettingsWarningLog(char* out, size_t size, bool changed, const char* modeLabel, const char* reason,
                                        bool standing, const FlatWarningCause& cause, const FlatSettingsWarning& words) {
    char extra[160] = "";
    if (cause.renderSize)
        std::snprintf(extra, sizeof(extra), ", render %ux%u on output %ux%u", cause.renderW, cause.renderH, cause.outputW,
                      cause.outputH);
    else if (cause.taaAbove)
        std::snprintf(extra, sizeof(extra), ", TAA above the output (render %ux%u, output %ux%u)", cause.renderW,
                      cause.renderH, cause.outputW, cause.outputH);
    int n = std::snprintf(out, size, "flat settings warning: %s (mode=%s, frames refused for %s%s%s%s): ",
                          changed ? "changed" : "shown", modeLabel ? modeLabel : "", reason ? reason : "",
                          standing ? ", work stood down" : "", cause.structureAdmission ? ", structure admission on" : "",
                          extra);
    for (int i = 0; i < words.count && n >= 0 && static_cast<size_t>(n) < size; ++i) {
        const int more = std::snprintf(out + n, size - static_cast<size_t>(n), "%s%s", i ? " | " : "", words.line[i]);
        if (more > 0) n += more;
    }
    return n;
}

// How a cause names itself when a held change is logged: by the sizes the key carries (the warning's own words), else only as the
// post chain's refusal.
inline void flatDescribeWarnCause(char* out, size_t size, const FlatWarningCause& c) {
    if (c.renderSize)
        std::snprintf(out, size, "render %ux%u on output %ux%u", c.renderW, c.renderH, c.outputW, c.outputH);
    else if (c.taaAbove)
        std::snprintf(out, size, "TAA above the output, render %ux%u on output %ux%u", c.renderW, c.renderH, c.outputW,
                      c.outputH);
    else
        std::snprintf(out, size, "the post chain, no sizes");
}

// The log line for a change that has started to be held (FlatWarnHold): what is on show and what is waiting. It does not begin
// with shown, changed or hidden, the three words tools\edvr_log.py's F8 reader parses after "flat settings warning: ", so it is
// neither counted as a warning nor mistaken for one. A `changed` line about two seconds after it is that cause being adopted;
// none is the change coming back before the hold ran out.
inline int flatFormatWarnHeldLog(char* out, size_t size, const FlatWarningCause& onShow, const FlatWarningCause& computed) {
    char before[128], after[128];
    flatDescribeWarnCause(before, sizeof(before), onShow);
    flatDescribeWarnCause(after, sizeof(after), computed);
    // The key also carries the mode, Elite's settings and the route's key: when the sizes read the same, one of those moved.
    const char* tail = std::strcmp(before, after) == 0 ? "; the mode, an Elite setting or the route's key differs" : "";
    return std::snprintf(out, size,
                         "flat settings warning: a change of cause is held for %llu ms before it replaces the one on show "
                         "(on show: %s; computed now: %s%s)",
                         static_cast<unsigned long long>(kFlatWarnHoldMs), before, after, tail);
}

// The log line for what was read.
inline int flatFormatEliteSettings(char* out, size_t size, const EliteGraphics& g) {
    auto value = [](int v, char* buf, size_t cap) {
        if (v < 0) std::snprintf(buf, cap, "unset");
        else std::snprintf(buf, cap, "%d", v);
    };
    if (!g.folderFound)
        return std::snprintf(out, size,
            "flat settings: no Elite graphics settings folder found under this user's LocalAppData "
            "(Frontier Developments\\Elite Dangerous\\Options\\Graphics); the warning cannot name settings");
    if (!g.presetKnown)
        return std::snprintf(out, size,
            "flat settings: Elite graphics preset unknown (no Settings.xml or no <PresetName>); nothing read");
    if (!g.custom)
        return std::snprintf(out, size,
            "flat settings: Elite graphics preset=%s (not Custom, so no .fxcfg is read); the preset may turn "
            "on anti-aliasing, bloom or depth of field", g.preset);
    if (!g.fileRead)
        return std::snprintf(out, size,
            "flat settings: Elite graphics preset=Custom but no Custom.<major>.<minor>.fxcfg could be read");
    char aa[16], bloom[16], dof[16];
    value(g.aaMode, aa, sizeof(aa));
    value(g.bloomQuality, bloom, sizeof(bloom));
    value(g.dofEnabled, dof, sizeof(dof));
    return std::snprintf(out, size,
        "flat settings: Elite graphics preset=Custom file=%s AAMode=%s BloomQuality=%s DOFEnabled=%s",
        g.file, aa, bloom, dof);
}

}  // namespace edvr
