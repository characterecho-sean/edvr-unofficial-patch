#pragma once

// The flat F8 panel's settings warning (src\d3d11\flat_elite_settings.h): Elite's Settings.xml and
// Custom.<major>.<minor>.fxcfg parsed from fixtures of the shapes users have (plain tags, one per
// line inside <Root>), the file chosen by the highest version, the words for the three reference
// users (AA on, bloom and DoF on, everything off), a non-Custom preset, missing files, the
// wrapping, and the watcher's re-read on change. The files are real files in a temporary
// directory, so the folder reading is the code the DLL runs; the panel's wiring is pinned by the
// source scan in flat_temporal_test.cpp. The hold on a changed cause (FlatWarnHold) is driven here
// too: the flight it was made for, replayed at the log's own stamps, and the controls that make
// the replay a test (a wait of 0 ms, 1500 ms or none, and four defective copies of the rule).

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../../src/d3d11/flat_elite_settings.h"
#include "../../src/d3d11/flat_hdr_route.h"   // flatHdrSupersamplingAdvice: the key and sizes that decide the extra paragraph
#include "../../src/d3d11/flat_standdown.h"   // kFlatStandDownProbeMs: the hold is measured against the probe cadence

namespace elite_settings_test {

// A temporary Options\Graphics stand-in; the destructor removes what the test wrote.
struct Folder {
    std::wstring path;
    std::vector<std::wstring> written;
    Folder() {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        wchar_t leaf[64];
        swprintf_s(leaf, L"edvr_fxcfg_test_%lu_%llu", GetCurrentProcessId(),
                   static_cast<unsigned long long>(GetTickCount64()));
        path = std::wstring(temp) + leaf;
        CreateDirectoryW(path.c_str(), nullptr);
    }
    ~Folder() {
        for (const auto& f : written) DeleteFileW(f.c_str());
        RemoveDirectoryW(path.c_str());
    }
    // Write `text` as `name`, last written `seconds` after an arbitrary fixed epoch.
    void write(const char* name, const std::string& text, uint64_t seconds) {
        const std::wstring full = path + L"\\" + std::wstring(name, name + std::strlen(name));
        HANDLE h = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD wrote = 0;
        WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
        ULARGE_INTEGER t;
        t.QuadPart = 134000000000000000ull + seconds * 10000000ull;
        FILETIME ft;
        ft.dwLowDateTime = t.LowPart;
        ft.dwHighDateTime = t.HighPart;
        SetFileTime(h, nullptr, nullptr, &ft);
        CloseHandle(h);
        bool known = false;
        for (const auto& f : written) known |= f == full;
        if (!known) written.push_back(full);
    }
};

inline std::string settingsXml(const char* preset) {
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\r\n<Root>\r\n  <PresetName>") + preset +
           "</PresetName>\r\n  <Fullscreen>1</Fullscreen>\r\n</Root>\r\n";
}
// One tag per line inside <Root>, as Elite writes them.
inline std::string fxcfg(int aa, int bloom, int dof) {
    char text[512];
    std::snprintf(text, sizeof(text),
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\r\n<Root>\r\n  <BlurEnabled>false</BlurEnabled>\r\n"
        "  <AAMode>%d</AAMode>\r\n  <AOQuality>3</AOQuality>\r\n  <BloomQuality>%d</BloomQuality>\r\n"
        "  <DOFEnabled>%d</DOFEnabled>\r\n  <HMDRenderTargetMultiplier>1.0</HMDRenderTargetMultiplier>\r\n</Root>\r\n",
        aa, bloom, dof);
    return text;
}
// The panel's ruler, a fixed 20 px a character.
inline int ruler(const char* text, void*) { return static_cast<int>(std::strlen(text)) * 20; }
constexpr int kPanelWidthPx = 806;   // the flat card less its padding, at the default size

}  // namespace elite_settings_test

// The hold on a changed cause (FlatWarnHold): the panel's side of it, the real helper under the waits the controls vary, four
// copies of the rule with one defect each, and the scenarios that must pass the real helper and fail every one of those.
namespace warn_hold_test {

using namespace edvr;

// A log stamp as milliseconds of the day. The last argument is decimal: write 50, not 050 (octal).
constexpr uint64_t stamp(int h, int m, int s, int ms) {
    return ((static_cast<uint64_t>(h) * 60 + static_cast<uint64_t>(m)) * 60 + static_cast<uint64_t>(s)) * 1000 +
           static_cast<uint64_t>(ms);
}

// What flatWarningTick keeps and does with the hold, in its order: the hold is asked first, every tick; then the comparison; then
// the switch (shown, changed, hidden: the three lines the log's reader counts). `held` counts the ticks on which a hold began.
template <class Hold>
struct Panel {
    Hold hold;
    bool active = false;
    std::string key;
    int shown = 0, changed = 0, hidden = 0, held = 0;
    std::vector<uint64_t> changedAt;
    void tick(bool refusing, const std::string& computed, uint64_t now) {
        if (!hold.admit(refusing, active, key, computed, now)) {
            if (hold.began()) ++held;
            return;
        }
        if (refusing == active && computed == key) return;
        if (!active) ++shown;
        else if (!refusing) ++hidden;
        else { ++changed; changedAt.push_back(now); }
        active = refusing;
        key = computed;
    }
};

// The real helper, called as menu.cpp calls it: five arguments, the constant's wait.
struct ProductionHold {
    FlatWarnHold h;
    bool admit(bool refusing, bool active, const std::string& shown, const std::string& computed, uint64_t now) {
        return h.admit(refusing, active, shown, computed, now);
    }
    bool began() const { return h.began(); }
    bool pending() const { return h.pending(); }
    const std::string& pendingKey() const { return h.pendingKey(); }
    uint64_t sinceMs() const { return h.sinceMs(); }
};
// The same helper with another wait, for the controls: 0 ms is the panel as it was before the hold, 1500 ms is one stand-down
// probe interval, UINT64_MAX is a hold that never expires.
template <uint64_t Ms>
struct WaitHold : ProductionHold {
    bool admit(bool refusing, bool active, const std::string& shown, const std::string& computed, uint64_t now) {
        return h.admit(refusing, active, shown, computed, now, Ms);
    }
};

// The rule written out again with one defect at a time (this rig has no mutants.py, so its controls live in it, as the source
// pins' do): StaleAfterReturn leaves a wait standing when the cause comes back to the one on show, ThirdKeyKeepsClock lets a third
// cause inherit the second one's clock, ShowAndHideHeld makes a show and a hide wait like any change, and ShowAndHideKeepPending
// leaves a waiting change standing across a show or a hide.
enum class Defect { StaleAfterReturn, ThirdKeyKeepsClock, ShowAndHideHeld, ShowAndHideKeepPending };
template <Defect D>
struct DefectiveHold {
    bool pending_ = false, began_ = false;
    std::string pendingKey_;
    uint64_t since_ = 0;
    bool admit(bool refusing, bool active, const std::string& shown, const std::string& computed, uint64_t now) {
        began_ = false;
        if constexpr (D != Defect::ShowAndHideHeld) {
            if (!refusing || !active) {
                if constexpr (D != Defect::ShowAndHideKeepPending) pending_ = false;
                return true;
            }
        }
        if (computed == shown) {
            if constexpr (D != Defect::StaleAfterReturn) pending_ = false;
            return true;
        }
        if (!pending_ || computed != pendingKey_) {
            if constexpr (D == Defect::ThirdKeyKeepsClock) {
                if (!pending_) since_ = now;
            } else {
                since_ = now;
            }
            pending_ = true;
            pendingKey_ = computed;
            began_ = true;
        }
        if (now - since_ < kFlatWarnHoldMs) return false;
        pending_ = false;
        return true;
    }
    bool began() const { return began_; }
    bool pending() const { return pending_; }
    const std::string& pendingKey() const { return pendingKey_; }
    uint64_t sinceMs() const { return since_; }
};

// ---- the flight (edvr_gfx_20261001_103559.log: Epic, DLSS, the route's key auto, a 3840x2160 screen) --------------------------
// The computed cause across the stand-down that entered at 10:38:16.516 with Elite rendering 1440x810: the loading screen's
// 256x256 that the stand-down's probes saw twice (it stood for 1521 ms and 1509 ms, one probe interval each), the resume at
// 10:38:36.172, and the panel's hide a millisecond later. The stamps are the log's: its four `changed` lines are the four flips of
// the computed cause, with no hold at all.
constexpr uint64_t kEntered = stamp(10, 38, 16, 516);   // the stand-down enters and the warning is shown
constexpr uint64_t kLow1 = stamp(10, 38, 19, 529);      // the computed cause moves to 256x256 ...
constexpr uint64_t kUp1 = stamp(10, 38, 21, 50);        // ... and back to 1440x810 (1521 ms later)
constexpr uint64_t kLow2 = stamp(10, 38, 24, 85);       // ... to 256x256 again ...
constexpr uint64_t kUp2 = stamp(10, 38, 25, 594);       // ... and back (1509 ms later)
constexpr uint64_t kResumed = stamp(10, 38, 36, 172);   // the stand-down resumes; the panel hides at kResumed + 1

struct Replay {
    int shown = 0, changed = 0, hidden = 0, held = 0;
    std::vector<uint64_t> changedAt;
    bool stayed = true;   // the key on show was the 1440x810 one at every tick
};

// The panel ticking every `step` ms over the flight (1 ms is the finest the log's stamps allow; 16 ms is a 60 fps frame).
template <class Hold>
Replay replayFlight(const std::string& k1440, const std::string& k256, uint64_t step) {
    Panel<Hold> p;
    Replay r;
    for (uint64_t t = kEntered; t <= kResumed; t += step) {
        const bool low = (t >= kLow1 && t < kUp1) || (t >= kLow2 && t < kUp2);
        p.tick(true, low ? k256 : k1440, t);
        if (p.active && p.key != k1440) r.stayed = false;
    }
    p.tick(false, std::string(), kResumed + 1);
    r.shown = p.shown;
    r.changed = p.changed;
    r.hidden = p.hidden;
    r.held = p.held;
    r.changedAt = p.changedAt;
    return r;
}

// Each scenario returns nullptr when the rule holds, else the first thing that did not. S is on show; X, Y are other causes.

// A change that persists is adopted at 2000 ms and not before: 1999 ms holds, 2000 ms adopts.
template <class Hold>
const char* persists(const std::string& S, const std::string& X) {
    Panel<Hold> p;
    const uint64_t T = 10000;
    p.tick(true, S, T - 1);
    for (uint64_t t = T; t < T + 2000; ++t) {
        p.tick(true, X, t);
        if (p.key != S || p.changed) return "a change replaced the one on show before it had persisted 2000 ms";
    }
    p.tick(true, X, T + 2000);
    if (p.key != X || p.changed != 1 || p.changedAt[0] != T + 2000) return "a change that persisted 2000 ms was not adopted at 2000 ms";
    return nullptr;
}

// A change that flips back before 2000 ms is dropped, and the next flip starts a new wait.
template <class Hold>
const char* flipBack(const std::string& S, const std::string& X) {
    Panel<Hold> p;
    const uint64_t T = 70000;
    p.tick(true, S, T - 1);
    for (uint64_t t = T; t < T + 1500; ++t) p.tick(true, X, t);
    if (!p.hold.pending() || p.key != S) return "a change was not waiting after 1500 ms";
    for (uint64_t t = T + 1500; t < T + 1600; ++t) p.tick(true, S, t);
    if (p.hold.pending() || p.changed) return "a change that came back to the one on show before its wait was up was not dropped";
    for (uint64_t t = T + 1600; t < T + 3600; ++t) {
        p.tick(true, X, t);
        if (p.key != S || p.changed) return "a change that flipped back and came again was adopted before 2000 ms of its new wait";
    }
    p.tick(true, X, T + 3600);
    if (p.key != X || p.changed != 1 || p.changedAt[0] != T + 3600) return "the restarted wait was not 2000 ms";
    return nullptr;
}

// A third cause starts its own 2000 ms, from the tick it appears; the second one is never adopted.
template <class Hold>
const char* thirdKey(const std::string& S, const std::string& X, const std::string& Y) {
    Panel<Hold> p;
    const uint64_t T = 90000;
    p.tick(true, S, T - 1);
    for (uint64_t t = T; t < T + 1500; ++t) p.tick(true, X, t);
    for (uint64_t t = T + 1500; t < T + 3500; ++t) {
        p.tick(true, Y, t);
        if (p.key != S || p.changed) return "a third cause was adopted before 2000 ms of its own wait";
        if (t == T + 1500 && (p.hold.pendingKey() != Y || p.hold.sinceMs() != T + 1500))
            return "the wait did not restart, from the tick it appeared, on a third cause";
    }
    p.tick(true, Y, T + 3500);
    if (p.key != Y || p.changed != 1 || p.changedAt[0] != T + 3500 || p.held != 2)
        return "the third cause was not adopted at its own 2000 ms as the only change (two waits begun)";
    return nullptr;
}

// A show and a hide are immediate, and either clears a waiting change.
template <class Hold>
const char* showHide(const std::string& S, const std::string& X) {
    Panel<Hold> p;
    const uint64_t T = 50000;
    p.tick(true, S, T);
    if (!p.active || p.shown != 1 || p.key != S) return "a show did not take effect on the tick that wanted it";
    p.tick(false, std::string(), T + 40);
    if (p.active || p.hidden != 1) return "a hide did not take effect on the tick that wanted it";
    p.tick(true, S, T + 80);
    if (!p.active || p.shown != 2) return "a second show was not immediate";
    p.tick(true, X, T + 100);
    p.tick(false, std::string(), T + 1500);
    if (p.active || p.hidden != 2 || p.hold.pending()) return "a hide did not take effect at once, or left a change waiting";
    p.tick(true, S, T + 1600);
    for (uint64_t t = T + 1700; t < T + 3700; ++t) {
        p.tick(true, X, t);
        if (p.key != S || p.changed) return "a change that was waiting before a hide was adopted early: its clock was not cleared";
    }
    p.tick(true, X, T + 3700);
    if (p.key != X || p.changed != 1) return "the change after a hide and a show was not adopted at 2000 ms";
    return nullptr;
}

}  // namespace warn_hold_test

inline int flatEliteSettingsTests() {
    using namespace edvr;
    using elite_settings_test::Folder;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: elite settings %s\n", name); ++failures; }
    };
    auto words = [&](const char* mode, const EliteGraphics& g, int width, FlatSettingsWarning* out) {
        flatComposeSettingsWarning(mode, g, width, &elite_settings_test::ruler, nullptr, out);
    };

    // ---- the tags ----------------------------------------------------------------
    {
        const std::string text = elite_settings_test::fxcfg(4, 3, 2);
        int v = -9;
        expect(flatXmlInt(text, "AAMode", &v) && v == 4 && flatXmlInt(text, "BloomQuality", &v) && v == 3 &&
               flatXmlInt(text, "DOFEnabled", &v) && v == 2, "plain one-per-line tags parse to their numbers");
        expect(!flatXmlInt(text, "NotThere", &v) && !flatXmlInt("<AAMode></AAMode>", "AAMode", &v) &&
               !flatXmlInt("<AAMode>x</AAMode>", "AAMode", &v), "a missing, empty or non-numeric tag is not a value");
        expect(flatXmlInt("<A> true </A>", "A", &v) && v == 1 && flatXmlInt("<A>false</A>", "A", &v) && v == 0 &&
               flatXmlInt("<A>\r\n  7\r\n</A>", "A", &v) && v == 7, "true/false dialect and whitespace");
        std::string preset;
        expect(flatXmlText(elite_settings_test::settingsXml("Custom"), "PresetName", &preset) && preset == "Custom",
               "Settings.xml's PresetName");
        // <AAModeX> must not answer for <AAMode>.
        expect(!flatXmlInt("<AAModeX>4</AAModeX>", "AAMode", &v), "a longer tag name is another tag");
    }

    // ---- the file names --------------------------------------------------------------
    {
        unsigned a = 9, b = 9;
        expect(flatCustomFxcfgVersion("Custom.4.4.fxcfg", &a, &b) && a == 4 && b == 4 &&
               flatCustomFxcfgVersion("Custom.10.2.fxcfg", &a, &b) && a == 10 && b == 2 &&
               flatCustomFxcfgVersion("custom.4.0.FXCFG", &a, &b) && a == 4 && b == 0 &&
               flatCustomFxcfgVersion("Custom.fxcfg", &a, &b) && a == 0 && b == 0 &&
               flatCustomFxcfgVersion("Custom.4.fxcfg", &a, &b) && a == 4 && b == 0,
               "Custom.<major>.<minor>.fxcfg parses, case-insensitively, with the unversioned forms");
        expect(!flatCustomFxcfgVersion("High.4.4.fxcfg", &a, &b) && !flatCustomFxcfgVersion("Custom.4.4.fxcfg.bak", &a, &b) &&
               !flatCustomFxcfgVersion("Custom.a.b.fxcfg", &a, &b) && !flatCustomFxcfgVersion("Custom..fxcfg", &a, &b) &&
               !flatCustomFxcfgVersion("Custom.4.4.4.fxcfg", &a, &b) && !flatCustomFxcfgVersion("Custom.4..fxcfg", &a, &b) &&
               !flatCustomFxcfgVersion("Settings.xml", &a, &b),
               "another preset, a backup suffix, letters or a third number are not versions");
        using F = EliteFxcfg;
        // The highest version wins whatever the write times say (user 3 had 4.0 to 4.4 side by side).
        std::vector<F> files = {{"Custom.4.0.fxcfg", 900}, {"Custom.4.4.fxcfg", 100}, {"Custom.4.2.fxcfg", 500},
                                {"High.4.4.fxcfg", 9999}, {"Custom.4.1.fxcfg", 800}};
        expect(flatPickCustomFxcfg(files) == 1, "the highest <major>.<minor> is the one the game reads, not the newest file");
        files = {{"Custom.9.9.fxcfg", 1}, {"Custom.10.0.fxcfg", 2}};
        expect(flatPickCustomFxcfg(files) == 1, "versions compare as numbers: 10.0 is above 9.9");
        files = {{"Custom.4.4.fxcfg", 10}, {"Custom.4.4.fxcfg", 30}};
        expect(flatPickCustomFxcfg(files) == 1, "the same version twice: the newest write breaks the tie");
        files = {{"Custom.beta.fxcfg", 10}, {"CustomOld.fxcfg", 30}, {"High.fxcfg", 99}};
        expect(flatPickCustomFxcfg(files) == 1, "no name carries a version: the newest Custom file by write time");
        files = {{"Custom.beta.fxcfg", 10}, {"Custom.3.1.fxcfg", 5}};
        expect(flatPickCustomFxcfg(files) == 1, "a versioned name beats an unparsed newer one");
        files = {{"High.4.4.fxcfg", 5}, {"Ultra.4.4.fxcfg", 6}};
        expect(flatPickCustomFxcfg(files) == -1 && flatPickCustomFxcfg({}) == -1, "no Custom file: none");
    }

    // ---- the words for the three reference users ------------------------------------------
    {
        EliteGraphics user1, user2, sean;
        for (EliteGraphics* g : {&user1, &user2, &sean}) {
            g->folderFound = g->presetKnown = g->custom = g->fileRead = true;
            std::strcpy(g->preset, "Custom");
            std::strcpy(g->file, "Custom.4.4.fxcfg");
        }
        user1.aaMode = 4; user1.bloomQuality = 0; user1.dofEnabled = 0;
        user2.aaMode = 0; user2.bloomQuality = 3; user2.dofEnabled = 2;
        sean.aaMode = 0; sean.bloomQuality = 0; sean.dofEnabled = 0;
        const int wide = 100000;   // no wrapping: one line per paragraph, for the exact words
        FlatSettingsWarning w;
        words("DLSS", user1, wide, &w);
        expect(w.count == 2 &&
               std::strcmp(w.line[0], "DLSS is not active: Elite's post-processing is not recognised.") == 0 &&
               std::strcmp(w.line[1], "Turn off in Elite's graphics options: Anti-aliasing") == 0,
               "user 1 (AAMode 4, bloom and DoF off): names only Anti-aliasing");
        words("DLAA", user2, wide, &w);
        expect(w.count == 2 &&
               std::strcmp(w.line[0], "DLAA is not active: Elite's post-processing is not recognised.") == 0 &&
               std::strcmp(w.line[1], "Turn off in Elite's graphics options: Bloom, Depth of field") == 0,
               "user 2 (AA off, BloomQuality 3, DOFEnabled 2): names Bloom and Depth of field");
        words("FSR3", sean, wide, &w);
        expect(w.count == 2 &&
               std::strcmp(w.line[0], "FSR3 is not active: Elite's post-processing is not recognised.") == 0 &&
               std::strcmp(w.line[1], "Please send your logs (F10 in the cockpit, then the installer's log bundle).") == 0,
               "Sean (all off): asks for the logs and names no setting");
        EliteGraphics all = user1;
        all.bloomQuality = 1; all.dofEnabled = 1;
        words("TAA", all, wide, &w);
        expect(w.count == 2 &&
               std::strcmp(w.line[1], "Turn off in Elite's graphics options: Anti-aliasing, Bloom, Depth of field") == 0,
               "all three on: all three named, in that order");

        EliteGraphics preset = sean;
        preset.custom = false;
        std::strcpy(preset.preset, "Ultra");
        words("DLSS", preset, wide, &w);
        expect(w.count == 2 && std::strstr(w.line[1], "Ultra graphics preset may turn on") != nullptr &&
               std::strstr(w.line[1], "Anti-aliasing, Bloom or Depth of field") != nullptr &&
               std::strstr(w.line[1], "Please send your logs") == nullptr,
               "a preset other than Custom is named and said to be able to turn the effects on");

        EliteGraphics unknown;   // nothing read at all
        words("DLSS", unknown, wide, &w);
        expect(w.count == 2 && std::strstr(w.line[1], "Please send your logs") != nullptr,
               "settings that could not be read ask for the logs");
        EliteGraphics customNoFile = sean;
        customNoFile.fileRead = false; customNoFile.aaMode = customNoFile.bloomQuality = customNoFile.dofEnabled = -1;
        words("DLSS", customNoFile, wide, &w);
        expect(w.count == 2 && std::strstr(w.line[1], "Please send your logs") != nullptr,
               "Custom with no readable .fxcfg asks for the logs");

        // The panel's width: 806 px at 20 px a character is 40 characters a line.
        words("DLSS", user2, elite_settings_test::kPanelWidthPx, &w);
        bool fits = w.count >= 3 && w.count <= FlatSettingsWarning::kMaxLines;
        std::string joined0, joined1;
        for (int i = 0; i < w.count; ++i) {
            if (elite_settings_test::ruler(w.line[i], nullptr) > elite_settings_test::kPanelWidthPx) fits = false;
        }
        expect(fits, "wrapped to the panel's width, every line fits");
        std::string all2;
        for (int i = 0; i < w.count; ++i) all2 += (i ? " " : "") + std::string(w.line[i]);
        expect(all2.find("DLAA") == std::string::npos && all2.find("DLSS is not active: Elite's post-processing is not "
               "recognised. Turn off in Elite's graphics options: Bloom, Depth of field") != std::string::npos,
               "the wrapped lines read as the two sentences, words unchanged");
        // A change of mode, preset or any of the three fields changes the key (and so the raster and the log).
        expect(flatSettingsWarningKey("DLSS", user1) != flatSettingsWarningKey("DLAA", user1) &&
               flatSettingsWarningKey("DLSS", user1) != flatSettingsWarningKey("DLSS", user2) &&
               flatSettingsWarningKey("DLSS", sean) != flatSettingsWarningKey("DLSS", preset) &&
               flatSettingsWarningKey("DLSS", user1) == flatSettingsWarningKey("DLSS", user1),
               "the warning key moves with the mode, the preset and the three fields");
    }

    // ---- the route's key is auto (sections 81 and 83): Bloom and Depth of field are not what a refusal is about -----------
    // The route resolves before both and the copy's admission by structure does not care about either, so their advice goes;
    // the Anti-aliasing advice stays (a filter after the tone pass is what keeps a frame refused, and a game TAA's jitter
    // fights EDVR's). Every other word is unchanged, which the key-off rows above pin.
    {
        EliteGraphics user1, user2, all, preset;
        for (EliteGraphics* g : {&user1, &user2, &all, &preset}) {
            g->folderFound = g->presetKnown = g->custom = g->fileRead = true;
            std::strcpy(g->preset, "Custom");
            std::strcpy(g->file, "Custom.4.4.fxcfg");
        }
        user1.aaMode = 4; user1.bloomQuality = 0; user1.dofEnabled = 0;
        user2.aaMode = 0; user2.bloomQuality = 3; user2.dofEnabled = 2;
        all.aaMode = 4; all.bloomQuality = 1; all.dofEnabled = 1;
        preset.aaMode = preset.bloomQuality = preset.dofEnabled = 0;
        preset.custom = false; std::strcpy(preset.preset, "Ultra");
        const int wide = 100000;
        FlatSettingsWarning w;
        FlatWarningCause admission;
        admission.structureAdmission = true;
        auto admitWords = [&](const char* mode, const EliteGraphics& g) {
            flatComposeSettingsWarning(mode, g, wide, &elite_settings_test::ruler, nullptr, &w, admission);
        };
        admitWords("DLSS", user1);
        expect(w.count == 2 && std::strcmp(w.line[1], "Turn off in Elite's graphics options: Anti-aliasing") == 0,
               "key auto, user 1 (AA on): Anti-aliasing is still named");
        admitWords("DLAA", user2);
        expect(w.count == 2 && std::strstr(w.line[1], "Bloom") == nullptr && std::strstr(w.line[1], "Depth of field") == nullptr &&
               std::strstr(w.line[1], "Please send your logs") != nullptr,
               "key auto, user 2 (only bloom and DoF on): neither is named, the logs are asked for");
        admitWords("TAA", all);
        expect(w.count == 2 && std::strcmp(w.line[1], "Turn off in Elite's graphics options: Anti-aliasing") == 0,
               "key auto, all three on: only Anti-aliasing is named");
        admitWords("DLSS", preset);
        expect(w.count == 2 && std::strstr(w.line[1], "Ultra graphics preset may turn on Anti-aliasing.") != nullptr &&
               std::strstr(w.line[1], "Bloom") == nullptr && std::strstr(w.line[1], "Depth of field") == nullptr,
               "key auto, another preset: only Anti-aliasing is said to be turned on");
        words("TAA", all, wide, &w);
        expect(w.count == 2 && std::strcmp(w.line[1], "Turn off in Elite's graphics options: Anti-aliasing, Bloom, Depth of field") == 0,
               "key off: the words are exactly what they were (all three named)");
        FlatWarningCause none;
        expect(flatSettingsWarningKey("DLSS", user1, none) != flatSettingsWarningKey("DLSS", user1, admission) &&
               flatSettingsWarningKey("DLSS", user1) == flatSettingsWarningKey("DLSS", user1, none),
               "the warning key moves with the key, so the panel rebuilds when it flips");
    }

    // ---- the render size (section 83): what the refusal is, in the user's terms ----------------------------------------
    // The scene's size is not a uniform scale of the output between half and twice (Elite's resolution is not the screen's
    // shape): the warning says "Elite renders 2176x1224 on a 2560x1600 screen" and what to set, and nothing about the post
    // chain. EDVR's TAA above the output is the post-chain refusal's third paragraph. The runtime's measured sizes decide
    // both, never Elite's settings file.
    {
        EliteGraphics rc5, sean, preset;
        for (EliteGraphics* g : {&rc5, &sean, &preset}) {
            g->folderFound = g->presetKnown = g->custom = g->fileRead = true;
            std::strcpy(g->preset, "Custom");
            std::strcpy(g->file, "Custom.4.4.fxcfg");
        }
        rc5.aaMode = 0; rc5.bloomQuality = 3; rc5.dofEnabled = 2;
        sean.aaMode = 0; sean.bloomQuality = 0; sean.dofEnabled = 0;
        preset.aaMode = preset.bloomQuality = preset.dofEnabled = 0;
        preset.custom = false; std::strcpy(preset.preset, "Ultra");
        const int wide = 100000;
        FlatSettingsWarning w;
        const EliteGraphics* users[] = {&rc5, &sean, &preset};

        // The rc.5 user's rig: Elite's resolution 2560x1440 on a 2560x1600 screen at supersampling 0.85, so R = 2176x1224.
        FlatWarningCause shape = flatWarningCause(true, true, false, true, true, 2176, 1224, 2560, 1600);
        expect(shape.renderSize && shape.structureAdmission && !shape.taaAbove && shape.renderW == 2176 && shape.outputH == 1600,
               "a render-size refusal with measured sizes is a render-size cause");
        for (const EliteGraphics* g : users) {
            flatComposeSettingsWarning("DLSS", *g, wide, &elite_settings_test::ruler, nullptr, &w, shape);
            expect(w.count == 2 &&
                   std::strcmp(w.line[0], "DLSS is not active: Elite renders 2176x1224 on a 2560x1600 screen.") == 0 &&
                   std::strcmp(w.line[1], "Set Elite's resolution to your screen's, 2560x1600, and change the render size with its "
                                          "supersampling.") == 0,
                   "the shape is off: the render size, the screen's, and what to set; the same for every Elite setting");
        }
        // The size is out of the half-to-twice band but the shape is right: say which way.
        flatComposeSettingsWarning("DLSS", sean, wide, &elite_settings_test::ruler, nullptr, &w,
                                   flatWarningCause(true, true, false, true, true, 1200, 750, 2560, 1600));
        expect(w.count == 2 && std::strcmp(w.line[0], "DLSS is not active: Elite renders 1200x750 on a 2560x1600 screen.") == 0 &&
               std::strcmp(w.line[1], "That is under half the screen's size. Raise Elite's supersampling.") == 0,
               "uniform but under half the screen's size: raise the supersampling");
        flatComposeSettingsWarning("TAA", sean, wide, &elite_settings_test::ruler, nullptr, &w,
                                   flatWarningCause(true, true, false, true, true, 5760, 3600, 2560, 1600));
        expect(w.count == 2 && std::strcmp(w.line[0], "TAA is not active: Elite renders 5760x3600 on a 2560x1600 screen.") == 0 &&
               std::strcmp(w.line[1], "That is over twice the screen's size. Lower Elite's supersampling.") == 0,
               "uniform but over twice the screen's size: lower the supersampling");
        // Sizes unknown (nothing measured yet): the refusal is not called a render-size one, the words are the post chain's.
        const FlatWarningCause unknownSizes = flatWarningCause(true, true, false, true, false, 0, 0, 0, 0);
        expect(!unknownSizes.renderSize, "a render-size reason without measured sizes says nothing about sizes");

        // EDVR's TAA above the output: a third paragraph after the two the post-chain warning always has.
        const FlatWarningCause taa = flatWarningCause(true, true, true, false, true, 3840, 2160, 2560, 1440);
        expect(taa.taaAbove && !taa.renderSize && taa.structureAdmission, "TAA above the output with the key auto is a TAA cause");
        flatComposeSettingsWarning("TAA", sean, wide, &elite_settings_test::ruler, nullptr, &w, taa);
        expect(w.count == 3 && std::strcmp(w.line[2], kFlatTaaAboveWords) == 0 &&
               std::strcmp(w.line[0], "TAA is not active: Elite's post-processing is not recognised.") == 0,
               "the TAA paragraph is the third line, after the post chain's two");
        expect(std::string(kFlatTaaAboveWords) == "Above 1.0 supersampling, EDVR's TAA works only on a post chain it knows. Set "
                                                  "Elite's supersampling to 1.0 or lower, or choose DLSS or FSR.",
               "the TAA paragraph's words");
        // Only with the key auto (the structure is what DLSS and FSR do not depend on), never alongside a render-size refusal, never
        // for another mode's label (the runtime publishes the mode), never when frames are not refused.
        expect(!flatWarningCause(true, false, true, false, true, 3840, 2160, 2560, 1440).taaAbove &&
               !flatWarningCause(true, true, true, true, true, 3840, 2160, 2560, 1440).taaAbove &&
               !flatWarningCause(false, true, true, false, true, 3840, 2160, 2560, 1440).taaAbove &&
               !flatWarningCause(true, true, false, false, true, 3840, 2160, 2560, 1440).taaAbove,
               "the TAA paragraph needs key auto, a post-chain refusal, TAA above the output and measured sizes");
        expect(!flatWarningCause(false, true, true, true, true, 2176, 1224, 2560, 1600).renderSize &&
               !flatWarningCause(false, true, true, true, true, 2176, 1224, 2560, 1600).structureAdmission,
               "frames not refused: no condition holds, whatever the runtime published");

        // At the panel's width it wraps onto the card: every variant fits the lines it has (twelve allowed), each line fits the width, and
        // the flat page's three rows and a blank line still leave the card room (menu.cpp static_asserts the same sum).
        bool fits = true;
        const FlatWarningCause causes[] = {shape, taa, flatWarningCause(true, true, false, true, true, 1200, 750, 2560, 1600),
                                           flatWarningCause(true, false, false, false, true, 0, 0, 0, 0)};
        for (const EliteGraphics* g : users)
            for (const FlatWarningCause& c : causes) {
                flatComposeSettingsWarning("TAA", *g, elite_settings_test::kPanelWidthPx, &elite_settings_test::ruler, nullptr, &w, c);
                fits = fits && w.count >= 2 && w.count <= FlatSettingsWarning::kMaxLines;
                for (int i = 0; i < w.count; ++i)
                    if (elite_settings_test::ruler(w.line[i], nullptr) > elite_settings_test::kPanelWidthPx) fits = false;
            }
        expect(fits, "wrapped to the panel's width every variant fits the lines it has");
        expect(3 + 1 + FlatSettingsWarning::kMaxLines <= 16, "the flat page's rows, a blank line and a full warning fit the card's 16 lines");

        // The key moves with every condition and with the sizes the words name, so the panel and the log update live as Elite's
        // resolution or supersampling changes; with no cause it is what it was.
        expect(flatSettingsWarningKey("DLSS", sean, shape) != flatSettingsWarningKey("DLSS", sean, FlatWarningCause{}) &&
               flatSettingsWarningKey("DLSS", sean, shape) !=
                   flatSettingsWarningKey("DLSS", sean, flatWarningCause(true, true, false, true, true, 2176, 1224, 2560, 1440)) &&
               flatSettingsWarningKey("DLSS", sean, taa) != flatSettingsWarningKey("DLSS", sean, FlatWarningCause{true, false, false}) &&
               flatSettingsWarningKey("DLSS", sean, FlatWarningCause{true, false, false, 3840, 2160, 2560, 1440}) ==
                   flatSettingsWarningKey("DLSS", sean, FlatWarningCause{true, false, false}),
               "the warning key moves with the sizes the words name and with nothing else it does not show");

        // The log line carries every paragraph the panel does (joined, so a list with no final period does not run into the next),
        // names the measured sizes where the words do, and is otherwise exactly what it was.
        char line[900];
        FlatSettingsWarning logged;
        flatComposeSettingsWarning("DLSS", rc5, 0, nullptr, nullptr, &logged, shape);
        flatFormatSettingsWarningLog(line, sizeof(line), false, "DLSS", "render-size-does-not-fit-output", true, shape, logged);
        expect(std::string(line) == "flat settings warning: shown (mode=DLSS, frames refused for render-size-does-not-fit-output, work "
                                    "stood down, structure admission on, render 2176x1224 on output 2560x1600): DLSS is not active: "
                                    "Elite renders 2176x1224 on a 2560x1600 screen. | Set Elite's resolution to your screen's, "
                                    "2560x1600, and change the render size with its supersampling.",
               "the log line for a render-size refusal: the conditions, the sizes, both paragraphs");
        flatComposeSettingsWarning("TAA", rc5, 0, nullptr, nullptr, &logged, taa);
        flatFormatSettingsWarningLog(line, sizeof(line), true, "TAA", "no-known-tone-pass", true, taa, logged);
        expect(std::string(line) == "flat settings warning: changed (mode=TAA, frames refused for no-known-tone-pass, work stood down, "
                                    "structure admission on, TAA above the output (render 3840x2160, output 2560x1440)): TAA is not "
                                    "active: Elite's post-processing is not recognised. | Please send your logs (F10 in the cockpit, "
                                    "then the installer's log bundle). | " + std::string(kFlatTaaAboveWords),
               "the log line for TAA above the output: the conditions, the sizes, all three paragraphs");
        FlatWarningCause off;
        flatComposeSettingsWarning("DLSS", rc5, 0, nullptr, nullptr, &logged, off);
        flatFormatSettingsWarningLog(line, sizeof(line), true, "DLSS", "no-known-tone-pass", true, off, logged);
        expect(std::string(line) == "flat settings warning: changed (mode=DLSS, frames refused for no-known-tone-pass, work stood "
                                    "down): DLSS is not active: Elite's post-processing is not recognised. | Turn off in Elite's "
                                    "graphics options: Bloom, Depth of field",
               "with the key off the log line is what it always was, but for the separator between paragraphs");
    }

    // ---- the hold on a changed cause (FlatWarnHold) ------------------------------------------------------------------------
    // The flat F8 warning flipped five times in nine seconds in the flight of 2026-10-01 (10:38:16.516 to 10:38:25.594): the computed
    // cause moved between "Elite renders 1440x810 on a 3840x2160 screen" and a loading screen's 256x256 at the stand-down's probe
    // frames, one probe interval (1521 ms, 1509 ms) apart. A cause that differs from the one on show is adopted only after it has
    // been the computed one on every tick for 2000 ms; a show and a hide stay immediate. The replay is the log's own sequence at
    // the log's own stamps. Each scenario below passes the real helper and fails the controls that break the rule it pins.
    {
        using namespace warn_hold_test;
        EliteGraphics sean;
        sean.folderFound = sean.presetKnown = sean.custom = sean.fileRead = true;
        std::strcpy(sean.preset, "Custom");
        std::strcpy(sean.file, "Custom.4.4.fxcfg");
        sean.aaMode = 0; sean.bloomQuality = 0; sean.dofEnabled = 0;
        // The keys are the panel's own: the cause the runtime publishes for a render-size refusal (route's key auto), DLSS selected.
        auto keyOf = [&](uint32_t w, uint32_t h) {
            return flatSettingsWarningKey("DLSS", sean, flatWarningCause(true, true, false, true, true, w, h, 3840, 2160));
        };
        const std::string k1440 = keyOf(1440, 810), k256 = keyOf(256, 256), k1024 = keyOf(1024, 576);
        expect(k1440 != k256 && k256 != k1024 && k1440 != k1024, "the three causes the hold is driven with are three different keys");
        expect(kFlatWarnHoldMs == 2000 && kFlatWarnHoldMs > kFlatStandDownProbeMs && kFlatWarnHoldMs < 2 * kFlatStandDownProbeMs,
               "the hold is 2000 ms: more than one stand-down probe interval (a transient is one), under two (a second probe confirms)");
        auto passes = [&](const char* failed, const char* what) {
            expect(failed == nullptr, (std::string(what) + (failed ? std::string(" -- ") + failed : std::string())).c_str());
        };
        auto catches = [&](const char* failed, const char* what) {
            expect(failed != nullptr, (std::string("(control) ") + what).c_str());
        };
        const auto clean = [](const Replay& r) { return r.shown == 1 && r.changed == 0 && r.hidden == 1 && r.stayed; };

        // (a) The replay: zero adoptions, whatever the frame time, and the key on show never leaves 1440x810.
        for (const uint64_t step : {1ull, 7ull, 16ull, 33ull}) {
            const Replay r = replayFlight<ProductionHold>(k1440, k256, step);
            const std::string at = " (ticks every " + std::to_string(step) + " ms)";
            expect(clean(r), ("the flight replayed: shown once, zero changes, the key on show never leaves 1440x810, hidden once" + at).c_str());
            expect(r.held == 2, ("the flight replayed: a hold begins at each 256x256 the probes saw, and no other" + at).c_str());
        }
        // The controls. With no hold the replay IS the log: one shown, the four changes at the log's own stamps, one hidden. A hold of
        // 0 ms or of one probe interval lets the real transients through, so the replay discriminates the 2000 ms.
        const Replay none = replayFlight<WaitHold<0>>(k1440, k256, 1);
        expect(none.shown == 1 && none.hidden == 1 && none.changed == 4 && !none.stayed &&
                   none.changedAt == std::vector<uint64_t>({kLow1, kUp1, kLow2, kUp2}),
               "(control) with no hold the replay reproduces the log: one shown, four changes at 19.529, 21.050, 24.085 and 25.594, one hidden");
        const Replay probe = replayFlight<WaitHold<1500>>(k1440, k256, 1);
        expect(!clean(probe) && probe.changed == 4, "(control) a hold of 1500 ms, one probe interval, lets both transients through");
        expect(!clean(replayFlight<DefectiveHold<Defect::StaleAfterReturn>>(k1440, k256, 1)),
               "(control) a wait left standing when the cause returns is adopted by the next transient: the flight catches it");
        expect(!clean(replayFlight<DefectiveHold<Defect::ShowAndHideHeld>>(k1440, k256, 1)),
               "(control) a hold that also delays the hide is caught by the flight");

        // (b) A change that persists is adopted at 2000 ms, not before; the controls that move the wait are caught.
        passes(persists<ProductionHold>(k1440, k256), "a change that persists 1999 ms holds and at 2000 ms is adopted");
        catches(persists<WaitHold<0>>(k1440, k256), "no hold at all is caught: 1999 ms must hold");
        catches(persists<WaitHold<1500>>(k1440, k256), "a hold of 1500 ms is caught: 1999 ms must hold");
        catches(persists<WaitHold<1999>>(k1440, k256), "a hold of 1999 ms is caught: 1999 ms must hold");
        catches(persists<WaitHold<2001>>(k1440, k256), "a hold of 2001 ms is caught: 2000 ms must adopt");
        catches(persists<WaitHold<UINT64_MAX>>(k1440, k256), "a hold that never expires is caught: 2000 ms must adopt");
        // (c) A change that flips back before 2000 ms is dropped; the next flip starts a new clock.
        passes(flipBack<ProductionHold>(k1440, k256), "a change that returns to the one on show before 2000 ms is dropped and the next flip restarts the clock");
        catches(flipBack<DefectiveHold<Defect::StaleAfterReturn>>(k1440, k256), "a wait left standing when the cause returns is caught");
        catches(flipBack<WaitHold<1500>>(k1440, k256), "a hold of 1500 ms is caught by the restarted clock");
        // (d) A third cause restarts the clock.
        passes(thirdKey<ProductionHold>(k1440, k256, k1024), "a third cause starts its own 2000 ms from the tick it appears, and the second is never adopted");
        catches(thirdKey<DefectiveHold<Defect::ThirdKeyKeepsClock>>(k1440, k256, k1024), "a third cause that inherits the second's clock is caught");
        // (e) Show and hide are immediate and clear a waiting change.
        passes(showHide<ProductionHold>(k1440, k256), "a show and a hide are immediate and a hide clears the waiting change");
        catches(showHide<DefectiveHold<Defect::ShowAndHideHeld>>(k1440, k256), "a show or a hide that waits is caught");
        catches(showHide<DefectiveHold<Defect::ShowAndHideKeepPending>>(k1440, k256), "a hide that leaves a change waiting is caught");

        // The held line (menu.cpp says it once when a hold begins): what is on show and what is waiting, in the sizes the key carries.
        // It never begins with shown, changed or hidden, the three words tools\edvr_log.py's F8 reader parses after the prefix.
        const FlatWarningCause c1440 = flatWarningCause(true, true, false, true, true, 1440, 810, 3840, 2160);
        const FlatWarningCause c256 = flatWarningCause(true, true, false, true, true, 256, 256, 3840, 2160);
        const FlatWarningCause taa = flatWarningCause(true, true, true, false, true, 3840, 2160, 2560, 1440);
        const FlatWarningCause chain = flatWarningCause(true, true, false, false, true, 0, 0, 0, 0);
        char held[360];   // menu.cpp's buffer for it
        const int n = flatFormatWarnHeldLog(held, sizeof(held), c1440, c256);
        expect(n > 0 && std::string(held) ==
                   "flat settings warning: a change of cause is held for 2000 ms before it replaces the one on show (on show: render "
                   "1440x810 on output 3840x2160; computed now: render 256x256 on output 3840x2160)",
               "the held line names the cause on show and the one waiting, with the sizes the key carries");
        const std::string heldLine(held);
        const char* parsed[] = {"flat settings warning: shown", "flat settings warning: changed", "flat settings warning: hidden"};
        bool clear = true;
        for (const char* prefix : parsed) clear = clear && heldLine.rfind(prefix, 0) != 0;
        expect(clear && heldLine.rfind("flat settings warning: ", 0) == 0,
               "the held line is a flat settings warning line that is none of shown, changed or hidden: the log's reader does not count it");
        expect(flatFormatWarnHeldLog(held, sizeof(held), c1440, taa) > 0 &&
                   std::string(held).find("(on show: render 1440x810 on output 3840x2160; computed now: TAA above the output, render "
                                          "3840x2160 on output 2560x1440)") != std::string::npos,
               "the held line names a TAA-above-the-output cause by its sizes");
        expect(flatFormatWarnHeldLog(held, sizeof(held), chain, chain) > 0 &&
                   std::string(held).find("(on show: the post chain, no sizes; computed now: the post chain, no sizes; the mode, an "
                                          "Elite setting or the route's key differs)") != std::string::npos,
               "when the sizes read the same the held line says the mode, a setting or the key is what moved");
        const int longest = flatFormatWarnHeldLog(held, sizeof(held), taa, taa);
        expect(longest > 0 && longest < static_cast<int>(sizeof(held)), "the longest held line fits menu.cpp's buffer untruncated");
    }

    // ---- real files ------------------------------------------------------------------------
    {
        Folder dir;
        dir.write("Settings.xml", elite_settings_test::settingsXml("Custom"), 10);
        // Custom 4.0 through 4.4 side by side, the OLD one written last (user 3): the game reads 4.4.
        dir.write("Custom.4.4.fxcfg", elite_settings_test::fxcfg(0, 0, 0), 20);
        dir.write("Custom.4.3.fxcfg", elite_settings_test::fxcfg(2, 2, 2), 30);
        dir.write("Custom.4.0.fxcfg", elite_settings_test::fxcfg(4, 4, 4), 90);
        dir.write("Custom.4.4.fxcfg.baseline-bak-20260921", elite_settings_test::fxcfg(4, 4, 4), 95);
        dir.write("High.4.4.fxcfg", elite_settings_test::fxcfg(4, 4, 4), 99);
        const EliteFolderListing listing = flatListEliteGraphics(dir.path);
        expect(listing.folder && listing.settings && listing.fxcfg.size() == 4,
               "the listing sees Settings.xml and the four .fxcfg files, not the backup");
        const EliteGraphics g = flatReadEliteGraphics(dir.path, listing);
        expect(g.folderFound && g.presetKnown && g.custom && g.fileRead &&
               std::strcmp(g.preset, "Custom") == 0 && std::strcmp(g.file, "Custom.4.4.fxcfg") == 0 &&
               g.aaMode == 0 && g.bloomQuality == 0 && g.dofEnabled == 0 && !g.anyOn(),
               "the highest Custom version is read: Custom.4.4.fxcfg, all three off");
        char line[512];
        flatFormatEliteSettings(line, sizeof(line), g);
        expect(std::string(line) == "flat settings: Elite graphics preset=Custom file=Custom.4.4.fxcfg AAMode=0 "
               "BloomQuality=0 DOFEnabled=0", "the log line for what was read");

        // The watcher: reads once, then only when a file changes, checked at most every 2 s.
        FlatSettingsWatcher watcher;
        watcher.setFolder(dir.path);
        expect(!watcher.everRead() && watcher.poll(1000, false) && watcher.everRead() && watcher.reads() == 1,
               "the first poll reads");
        expect(!watcher.poll(1500, false) && watcher.reads() == 1, "a poll inside 2 s does not look at all");
        expect(!watcher.poll(3500, false) && watcher.reads() == 1, "a poll after 2 s finds nothing changed and reads nothing");
        expect(!watcher.poll(3600, true) && watcher.reads() == 1, "a forced poll (the menu opening) still reads only on change");
        // The game rewrites the current file when settings are applied.
        dir.write("Custom.4.4.fxcfg", elite_settings_test::fxcfg(4, 3, 2), 200);
        expect(!watcher.poll(3700, false), "inside the 2 s window even a change waits");
        expect(watcher.poll(5700, false) && watcher.reads() == 2 &&
               watcher.settings().aaMode == 4 && watcher.settings().bloomQuality == 3 &&
               watcher.settings().dofEnabled == 2 && watcher.settings().anyOn(),
               "a rewritten file is read again at the next check");
        // A newer game version writes a higher file: it becomes the one read.
        dir.write("Custom.4.5.fxcfg", elite_settings_test::fxcfg(0, 0, 0), 210);
        expect(watcher.poll(6000, true) && watcher.reads() == 3 &&
               std::strcmp(watcher.settings().file, "Custom.4.5.fxcfg") == 0 && !watcher.settings().anyOn(),
               "a new higher version is picked up, forced");
        // The preset changes to one that is not Custom.
        dir.write("Settings.xml", elite_settings_test::settingsXml("High"), 220);
        expect(watcher.poll(9000, false) && watcher.reads() == 4 && watcher.settings().presetKnown &&
               !watcher.settings().custom && !watcher.settings().fileRead &&
               std::strcmp(watcher.settings().preset, "High") == 0,
               "a preset other than Custom is read as such and no .fxcfg is read for it");
        flatFormatEliteSettings(line, sizeof(line), watcher.settings());
        expect(std::string(line).find("preset=High (not Custom") != std::string::npos, "and the log line says so");
    }
    {   // No folder, no Settings.xml, an empty folder.
        Folder dir;
        FlatSettingsWatcher watcher;
        watcher.setFolder(dir.path);
        expect(watcher.poll(0, false) && watcher.settings().folderFound && !watcher.settings().presetKnown &&
               !watcher.settings().fileRead, "an empty Options\\Graphics folder: found, nothing in it");
        char line[512];
        flatFormatEliteSettings(line, sizeof(line), watcher.settings());
        expect(std::string(line).find("preset unknown") != std::string::npos, "and the log line says the preset is unknown");
        FlatSettingsWatcher missing;
        missing.setFolder(dir.path + L"\\not_here");
        expect(missing.poll(0, false) && !missing.settings().folderFound, "a folder that does not exist");
        flatFormatEliteSettings(line, sizeof(line), missing.settings());
        expect(std::string(line).find("no Elite graphics settings folder") != std::string::npos,
               "and the log line says there is none");
        FlatSettingsWarning w;
        flatComposeSettingsWarning("DLSS", missing.settings(), 100000, &elite_settings_test::ruler, nullptr, &w);
        expect(w.count == 2 && std::strstr(w.line[1], "Please send your logs") != nullptr,
               "with no settings folder the warning asks for the logs");
        FlatSettingsWatcher blank;   // LOCALAPPDATA unset: an empty folder path
        blank.setFolder(std::wstring());
        expect(blank.poll(0, false) && !blank.settings().folderFound, "an empty folder path is no folder");
    }
    // The folder composition the installer's log bundler shares.
    expect(eliteGraphicsFolderUnder(L"C:\\Users\\a\\AppData\\Local") ==
               L"C:\\Users\\a\\AppData\\Local\\Frontier Developments\\Elite Dangerous\\Options\\Graphics" &&
           eliteGraphicsFolderUnder(L"C:\\Users\\a\\AppData\\Local\\") ==
               L"C:\\Users\\a\\AppData\\Local\\Frontier Developments\\Elite Dangerous\\Options\\Graphics" &&
           eliteGraphicsFolderUnder(L"").empty(),
           "the shared folder composition: with or without a trailing separator, and empty in, empty out");
    return failures;
}
