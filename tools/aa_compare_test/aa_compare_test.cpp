// The AA comparison key (hotkey.compare_aa): the toggle's state machine, the notice words, the key-clash rule, the Config runtime
// override it rides on, and the source pins that wire it. Nothing is opened: no device, no window, no game.
//
//   aa_compare_test <repo root>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/common/aa_compare.h"
#include "../../src/common/config.h"
#include "../../src/common/hotkey.h"
#include "../../src/common/log.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/d3d11/flat_notice_queue.h"
#include "../rig_graphics_mode.h"

using namespace edvr;
using namespace edvr::aacmp;

static unsigned g_checks = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        std::exit(1);
    }
}

static std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool writeFile(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    CloseHandle(h);
    return ok && n == text.size();
}

// A write time that is certainly different from the last one, whatever the file system's resolution.
static void touchForward(const std::wstring& path, int seconds) {
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += static_cast<unsigned long long>(seconds) * 10000000ull;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

static void machineCells() {
    // The ordinary run: off, then restored.
    {
        Machine m;
        check(m.press("dlss") == Press::WentOff && m.off && m.saved == "dlss", "a press with AA on goes off and remembers the mode");
        check(m.press("dlss") == Press::Restored && !m.off && m.saved.empty(), "the next press restores");
        check(m.press("dlss") == Press::WentOff, "and the one after goes off again");
    }
    // Every mode EDVR has goes off; off itself has nothing to compare.
    for (const char* mode : {"on", "dlaa", "dlss", "fsr", "xess", "DLSS", "Xess"}) {
        Machine m;
        check(m.press(mode) == Press::WentOff, "every temporal mode can be compared");
    }
    for (const char* mode : {"off", "", "banana", "0"}) {
        Machine m;
        check(m.press(mode) == Press::NothingToCompare && !m.off, "a configured mode that is not a temporal mode has nothing to compare");
        check(m.press(mode) == Press::NothingToCompare, "and it stays that way press after press");
    }
    // Restoring returns whatever the settings say at that moment, not what was remembered.
    {
        Machine m;
        m.press("dlss");
        check(m.press("fsr") == Press::Restored && !m.off, "a press while off restores, whatever the file says now");
    }
    // The player changes AA while it is off: their choice wins.
    {
        Machine m;
        m.press("dlss");
        check(m.observe("dlss", true, 10) == Seen::Nothing && m.off, "nothing changed: the comparison stands");
        check(m.observe("DLSS", true, 20) == Seen::Nothing && m.off, "the mode compares without case");
        check(m.observe("fsr", true, 30) == Seen::EndedByChange && !m.off, "the file's AA changed: the comparison ends");
        check(m.observe("fsr", true, 40) == Seen::Nothing, "and ends once");
        check(m.press("fsr") == Press::WentOff && m.saved == "fsr", "the next press is a fresh off from the new mode");
    }
    {
        Machine m;
        m.press("dlss");
        check(m.observe("off", true, 5) == Seen::EndedByChange && !m.off, "a hand edit to off ends it too (AA is off for real)");
        check(m.press("off") == Press::NothingToCompare, "...and then there is nothing to compare");
    }
    // An F8 pick that lands on the remembered mode: ended once the write has had time to land.
    {
        Machine m;
        m.press("dlss");
        m.noteMenuEdit(1000);
        check(m.observe("dlss", true, 1000 + kMenuEditSettleMs - 1) == Seen::Nothing && m.off, "a pick still being written does not end it early");
        check(m.observe("dlss", true, 1000 + kMenuEditSettleMs) == Seen::EndedBySameValue && !m.off, "a pick that landed on the same mode ends it");
    }
    {
        Machine m;
        m.press("dlss");
        m.noteMenuEdit(1000);
        check(m.observe("xess", true, 1100) == Seen::EndedByChange, "a pick that changed the file ends it at once, before the settle time");
    }
    {
        Machine m;
        m.noteMenuEdit(1);   // a pick with no comparison running is nobody's business
        check(!m.menuEdit && m.observe("dlss", true, 99999) == Seen::Nothing, "a menu edit with the comparison on ends nothing");
        m.press("dlss");
        check(!m.menuEdit && m.observe("dlss", true, 99999) == Seen::Nothing, "and does not leak into the next comparison");
    }
    // The override went away under it (a profile switch): the comparison is over, the machine is clean.
    {
        Machine m;
        m.press("dlss");
        check(m.observe("dlss", false, 1) == Seen::EndedByProfile && !m.off && m.saved.empty(), "a lost override ends the comparison");
        check(m.press("on") == Press::WentOff, "and the key works at once in the new profile");
    }
    {
        Machine m;
        check(m.observe("dlss", false, 1) == Seen::Nothing, "a lost override with no comparison running is nothing");
    }
}

static void wordsCells() {
    check(noticeText(Press::WentOff, "dlss", "F7") == "AA off (comparison), press F7 to restore", "the off notice, word for word");
    check(noticeText(Press::WentOff, "dlss", "CTRL+F7") == "AA off (comparison), press CTRL+F7 to restore", "the off notice names the key as bound");
    check(noticeText(Press::Restored, "dlss", "F7") == "AA restored: DLSS", "the restore notice names the mode");
    check(noticeText(Press::Restored, "on", "F7") == "AA restored: TAA" && noticeText(Press::Restored, "dlaa", "F7") == "AA restored: DLAA" &&
          noticeText(Press::Restored, "fsr", "F7") == "AA restored: FSR" && noticeText(Press::Restored, "xess", "F7") == "AA restored: XeSS",
          "every mode has its name");
    check(!noticeText(Press::NothingToCompare, "off", "F7").empty(), "the nothing-to-compare notice exists");
    check(modeLabel("") == "off" && modeLabel("mystery") == "mystery", "an unknown mode is shown as written");
}

static void clashCells() {
    check(keysClash(0x76, 0, 0x76, 0), "the same bare key clashes");
    check(!keysClash(0x76, 0, 0x77, 0), "another key does not");
    check(keysClash(0x76, 0, 0x76, kHotkeyCtrl), "a bare key and the same key with Ctrl overlap (the bare one also fires with Ctrl held)");
    check(keysClash(0x76, kHotkeyCtrl, 0x76, 0), "and the other way round");
    check(!keysClash(0x76, kHotkeyCtrl, 0x76, kHotkeyShift), "Ctrl+F7 and Shift+F7 are different presses");
    check(keysClash(0x76, kHotkeyCtrl, 0x76, kHotkeyCtrl), "the same chord clashes");
    check(!keysClash(0, 0, 0, 0), "an unbound key clashes with nothing");
}

// Config::init decides the runtime profile from the 3D mode in Elite's Settings.xml under %LOCALAPPDATA% (runtime_profile.h), and mirrors the settings under
// %LOCALAPPDATA%\EDVR. The rig never reads the machine's own: it points LOCALAPPDATA at a fixture tree holding a Settings.xml of the mode it wants
// (tools\rig_graphics_mode.h; mode 0 is flat, 4 is VR). Before this, a machine whose Settings.xml said flat failed "the file's AA is read".
static bool pinGraphicsMode(const std::wstring& root, int mode) { return edvr::rig::pinRigGraphicsMode(root, mode); }

static void removeTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(full);
            else DeleteFileW(full.c_str());
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

// The same file under a flat Settings.xml (3D mode 0): the profile is flat, the allow-list gates generic reads, and the AA mode is still answered
// around it by requestedTemporalMode, which is what the flat adapter calls. The override rides the same way.
static void flatProfileCells(const std::wstring& dir) {
    const std::wstring ini = dir + L"\\edvr.ini";
    check(writeFile(ini, "[fix]\ntemporal_aa = dlss\nui_quality = 100\n"), "flat: the fixture ini is written");
    check(pinGraphicsMode(dir + L"\\flat_localappdata", 0), "flat: the fixture Settings.xml (3D mode 0) is in place");
    Config& cfg = Config::get();
    cfg.init(dir);
    check(runtimeFlatProfile() && !runtimeVrProfile(), "flat: the fixture's mode 0 makes the profile flat, whatever the machine's own Settings.xml says");
    check(cfg.requestedTemporalMode() == "dlss" && cfg.configuredTemporalMode() == "dlss", "flat: the file's AA is read through the mode reader");
    check(cfg.getString("fix.black_void", "x") == "off", "flat: the allow-list refuses what it refuses");
    check(cfg.getString("hotkey.compare_aa", "F7") == "F7", "flat: the comparison key passes the gate");
    cfg.setRuntimeOverride("fix.temporal_aa", "off");
    check(cfg.requestedTemporalMode() == "off" && cfg.configuredTemporalMode() == "dlss", "flat: the override is what the flat adapter reads, the file's mode untouched");
    cfg.clearRuntimeOverride("fix.temporal_aa");
    check(cfg.requestedTemporalMode() == "dlss", "flat: clearing returns to the file's mode");
}

static void configCells(const std::wstring& dir) {
    check(pinGraphicsMode(dir + L"\\vr_localappdata", 4), "the fixture Settings.xml (3D mode 4) is in place");
    const std::wstring ini = dir + L"\\edvr.ini";
    const std::string original = "[fix]\ntemporal_aa = dlss\nui_quality = 100\n";
    check(writeFile(ini, original), "the fixture ini is written");
    Config& cfg = Config::get();
    cfg.init(dir);
    check(runtimeVrProfile() && !runtimeFlatProfile(), "the fixture's mode 4 makes the profile VR, whatever the machine's own Settings.xml says");
    check(cfg.getString("fix.temporal_aa", "off") == "dlss" && cfg.requestedTemporalMode() == "dlss", "the file's AA is read");
    check(cfg.configuredTemporalMode() == "dlss" && !cfg.hasRuntimeOverride("fix.temporal_aa"), "no override to begin with");
    check(!cfg.reloadIfChanged(), "an untouched file reports no change");

    const uint32_t gen0 = cfg.generation();
    cfg.setRuntimeOverride("fix.temporal_aa", "off");
    check(cfg.generation() != gen0, "an override moves the generation (modules that keep parsed settings re-read)");
    check(cfg.getString("fix.temporal_aa", "x") == "off" && cfg.requestedTemporalMode() == "off", "the override is what both readers answer");
    check(cfg.configuredTemporalMode() == "dlss", "the configured mode ignores it");
    check(cfg.hasRuntimeOverride("fix.temporal_aa") && cfg.hasRuntimeOverride("FIX.TEMPORAL_AA") == false, "the key is the dotted lowercase name");
    check(cfg.getString("fix.ui_quality", "x") == "100", "other keys are untouched");
    check(cfg.reloadIfChanged(), "the next reload poll reports a change, so the configure sweep runs");
    check(!cfg.reloadIfChanged(), "and only once");

    // The file is never written: byte for byte what it was, and a reload of it leaves the override in place.
    check(slurp(std::string(ini.begin(), ini.end())) == original, "no ini was written");
    check(writeFile(ini, "[fix]\ntemporal_aa = fsr\nui_quality = 125\n"), "the file is edited by hand");
    touchForward(ini, 5);
    check(cfg.reloadIfChanged(), "the edit is picked up");
    check(cfg.getString("fix.temporal_aa", "x") == "off", "a reload leaves the override in force");
    check(cfg.configuredTemporalMode() == "fsr" && cfg.getString("fix.ui_quality", "x") == "125", "while the file's new values are read");

    cfg.clearRuntimeOverride("fix.temporal_aa");
    check(cfg.getString("fix.temporal_aa", "x") == "fsr" && cfg.requestedTemporalMode() == "fsr", "clearing returns to the file's mode");
    check(!cfg.hasRuntimeOverride("fix.temporal_aa") && cfg.reloadIfChanged() && !cfg.reloadIfChanged(), "a clear reports one change");
    const uint32_t gen1 = cfg.generation();
    cfg.clearRuntimeOverride("fix.temporal_aa");
    check(cfg.generation() == gen1 && !cfg.reloadIfChanged(), "clearing what is not set changes nothing");

    // The flat gate reads the mode around the allow-list; the override must follow it there.
    const RuntimeProfile saved = g_runtimeProfile;
    g_runtimeProfile = RuntimeProfile::Flat;
    cfg.setRuntimeOverride("fix.temporal_aa", "off");
    check(cfg.requestedTemporalMode() == "off" && cfg.configuredTemporalMode() == "fsr", "flat: requestedTemporalMode answers the override");
    check(cfg.getString("hotkey.compare_aa", "F7") == "F7", "flat: the comparison key passes the gate (the fixture has none, so the default F7)");
    check(cfg.getString("fix.black_void", "x") == "off", "flat: the gate still refuses what it refused");
    g_runtimeProfile = saved;

    // A profile switch drops it.
    check(cfg.hasRuntimeOverride("fix.temporal_aa"), "the override is set going into the switch");
    cfg.switchProfile();
    check(!cfg.hasRuntimeOverride("fix.temporal_aa") && cfg.requestedTemporalMode() == "fsr", "a profile switch clears every override");
    check(cfg.reloadIfChanged(), "and reports it");
    check(slurp(std::string(ini.begin(), ini.end())).find("temporal_aa = fsr") != std::string::npos, "the file still says what the player wrote");
}

// The flat F8 panel is the notice's surface, so a notice raised with the menu open (F7 pressed while F8 is up) waits for it to close
// (review of main 9fe5b257, finding 4: menuTick cleared the due flag every tick and started the notice only with the menu closed).
// FlatNoticeQueue is the production decision menuTick calls; the cells drive it with the real notice words.
static void flatNoticeCells() {
    // F7 with the menu open: AA goes off (the comparison), the notice is queued; menuTick runs a few times with the menu open.
    {
        FlatNoticeQueue q;
        q.show(noticeText(Press::WentOff, "dlss", "F7").c_str());
        check(q.due(), "the off notice is queued");
        check(q.take(true) == nullptr && q.take(true) == nullptr && q.take(true) == nullptr,
              "ticks with the menu open do not start the notice");
        check(q.due(), "...and do not discard it");
        const std::string* got = q.take(false);
        check(got && *got == "AA off (comparison), press F7 to restore", "the menu closes: the off notice starts, word for word");
        check(!q.due() && q.take(false) == nullptr, "...once");
    }
    // Restoring AA with the menu open: same.
    {
        FlatNoticeQueue q;
        q.show(noticeText(Press::Restored, "dlss", "F7").c_str());
        check(q.take(true) == nullptr && q.due(), "the restore notice is kept while the menu is open");
        const std::string* got = q.take(false);
        check(got && *got == "AA restored: DLSS", "...and starts when the menu closes");
    }
    // Off then restore while the menu stays open: the newer notice replaces the older, the player sees where AA stands now.
    {
        FlatNoticeQueue q;
        q.show(noticeText(Press::WentOff, "dlss", "F7").c_str());
        check(q.take(true) == nullptr, "off, menu open");
        q.show(noticeText(Press::Restored, "dlss", "F7").c_str());
        check(q.take(true) == nullptr, "restored, menu open");
        const std::string* got = q.take(false);
        check(got && *got == "AA restored: DLSS" && q.take(false) == nullptr, "only the newest starts when the menu closes");
    }
    // A profile switch with a notice pending (review follow-up of main 4bd06f12, F3): F7 with the menu open queues "AA off", the mode watcher then commits a
    // switch to VR, which closes the menu and clears the AA override. Back in flat the old notice must not appear.
    {
        FlatNoticeQueue q;
        q.show(noticeText(Press::WentOff, "dlss", "F7").c_str());
        check(q.take(true) == nullptr && q.due(), "profile switch: the AA-off notice is waiting behind the open menu");
        q.clear();   // menuProfileSwitch
        check(!q.due() && q.take(false) == nullptr && q.take(true) == nullptr, "profile switch: the cleared queue starts nothing when the menu is closed, and keeps nothing");
        q.show(noticeText(Press::Restored, "dlss", "F7").c_str());
        const std::string* got = q.take(false);
        check(got && *got == "AA restored: DLSS", "profile switch: a notice raised after the switch is shown, and is not mixed with the old text");
    }
    // Menu closed when the notice is raised: it starts on the next tick, as before.
    {
        FlatNoticeQueue q;
        q.show("AA off (comparison), press F7 to restore");
        check(q.take(false) != nullptr && !q.due(), "with the menu closed the notice starts at once");
        q.show("");
        q.show(nullptr);
        check(!q.due() && q.take(false) == nullptr, "an empty notice queues nothing");
    }
}

static bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

static void sourcePins(const std::string& root) {
    const std::string hook = slurp(root + "\\src\\d3d11\\device_hook.cpp");
    const std::string menu = slurp(root + "\\src\\d3d11\\menu.cpp");
    const std::string ini = slurp(root + "\\edvr.ini");
    const std::string flatKeys = slurp(root + "\\src\\common\\flat_profile_keys.h");
    const std::string gen = slurp(root + "\\tools\\gen_installer_rc.py");
    const std::string readme = slurp(root + "\\release\\README.txt");
    const std::string mod = slurp(root + "\\src\\d3d11\\aa_compare.cpp");
    if (hook.empty() || menu.empty() || ini.empty()) {
        std::printf("FAIL: the repo root \"%s\" does not hold the sources\n", root.c_str());
        std::exit(1);
    }
    check(has(hook, "tkAaCompare.run([] { aaCompareTick(); });"), "device_hook runs the comparison tick every owned Present");
    check(has(menu, "void menuRequestConfigPoll() { g_s.pollRequest = true; }") && has(mod, "menuRequestConfigPoll();"),
          "the toggle asks for the config poll through the menu's own request, which the frame boundary already honours");
    check(has(menu, "aaCompareNoteMenuEdit();") && has(menu, "\"fix.temporal_aa\""), "an F8 write of fix.temporal_aa tells the comparison");
    check(has(menu, "void menuShowNotice(") && has(menu, "void menuNoteOwnChange("), "the menu has the notice and the own-change calls");
    check(has(menu, "s.flatNotice.show(text);") && has(menu, "s.flatNotice.take(s.open)"),
          "the flat menu raises and starts its notice through the queue, which holds it while the menu is open");
    {
        const size_t at = menu.find("void menuProfileSwitch() {");
        const size_t end = at == std::string::npos ? at : menu.find("\n}\n", at);
        const std::string body = end == std::string::npos ? std::string() : menu.substr(at, end - at);
        check(!body.empty() && has(body.c_str(), "s.flatNotice.clear();") && has(body.c_str(), "s.flatNoticeUp = false;") && has(body.c_str(), "s.flatNoticeShown = false;"),
              "a profile switch clears the outgoing profile's queued and displayed flat notice (menuProfileSwitch)");
    }
    check(has(ini, "compare_aa = F7"), "edvr.ini ships the key bound to F7");
    check(std::string(kDefaultKey) == "F7", "and the code's default is the same F7");
    check(has(ini, "# ui: AA comparison key (empty = off) | hotkey | live"), "the F8 Hotkeys page has the row");
    check(has(flatKeys, "\"hotkey.compare_aa\""), "the flat profile reads the key");
    check(has(gen, "compare_aa = F7"), "and the flat ini template carries it");
    check(has(readme, "F7"), "release\\README.txt lists the key");
    check(has(mod, "setRuntimeOverride(kKey, \"off\")") && !has(mod, "iniedit") && !has(mod, "writeFileAtomic"),
          "the module overrides in memory and includes no ini writer");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: aa_compare_test <repo root>");
        return 2;
    }
    wchar_t temp[MAX_PATH]{}, name[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    GetTempFileNameW(temp, L"aac", 0, name);
    DeleteFileW(name);
    CreateDirectoryW(name, nullptr);
    const std::wstring dir = name;

    machineCells();
    wordsCells();
    clashCells();
    flatNoticeCells();
    const std::wstring previous = edvr::rig::currentRigLocalAppData();
    configCells(dir);
    flatProfileCells(dir);
    edvr::rig::restoreRigLocalAppData(previous);
    sourcePins(argv[1]);

    removeTree(dir);
    std::printf("aa_compare_test: %u checks, 0 failures\n", g_checks);
    return 0;
}
