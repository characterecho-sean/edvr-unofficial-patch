// Elite's terrain checkerboard rendering in VR, said in the headset (design doc docs/design-flat-temporal-aa-2026-09-23.md, section 84):
// the words and the pure picks (src/common/terrain_checkerboard_notice.h), the reader on fixture folders and the monitor's rule for a
// half-written file (src/d3d11/terrain_checkerboard_reader.h), the worker thread on a real log (src/d3d11/terrain_checkerboard.cpp), and the
// wiring in menu.cpp as source pins.
//
// The pure half is the very code the DLL runs: this file includes the two headers and calls them, and links the worker's own source.
// Nothing here fakes a decision. tools\terrain_checkerboard_test\mutants.py breaks each rule on purpose (the headers and the worker by a
// textual edit compiled in, the wiring pins by editing copies of the sources they read) and requires the rule's own checks to fail.
//
// WHAT IS PINNED, by rule (a check's label starts with its rule's id: the mutation tool names the rule each edit must trip):
//   T1  the words: the toast (55 characters at most) and the Status hint (78), the sentence, the log lines (ON, OFF, unknown, the worker's
//       own four, the menu's queued note), the constants
//   T2  the pure picks: the published word (state and version in one atomic word), the toast's latch (once per RAISE, re-armed by Off or
//       Unknown), the Status page's one hint slot (the one that applies; alternating every 6 s when both do), the log's bound
//   T3  the reader on fixture folders: Custom true and false, the highest Custom version, the lookalike files that are never read, no Custom
//       file, a missing or empty Settings.xml, an absent tag, the value spellings, CRLF/BOM/indent, Settings.xml's own tag (never the
//       toggle), comments, the stock presets' OptionDefaults files, a stock preset whose file is missing, user-made presets, names that
//       could be paths, case, and that a read changes nothing
//   T4  the monitor's rule: the first read is published, a rewrite flips the state at the next poll, a known state stands against one bad
//       read (a half-written file) and goes to Unknown on the second, a transient does not accumulate, a change of preset is a change
//   T5  the worker thread on a real log: the flat profile starts nothing and logs nothing; the VR profile starts once, publishes the
//       state and a version at each change, says it in the log (start, each change), is stopped by its flag, and tools\edvr_log.py
//       --terrain-checkerboard reads what it wrote
//   T6  the wiring, as source pins: the toast and the Status hint in menu.cpp (the toast's latch set before menu.toasts is tested, no
//       line added to any page, the hint after the supersampling one), the render thread never reads a file, the flat profile never
//       touches it, the worker's lifetime (a detached thread in a try block, stopped by a flag and an event, not from DllMain), the
//       reader asks Settings.xml for the preset's name and for nothing else, the words header stays free of Windows and the config
//
// Usage: --self-test [<repo root>] [--only T1,T3,...] [--root <dir>] | --dry-run (does nothing). Run from the repo root (the pins read src\
// and T5 runs python tools\edvr_log.py). Exit 0 and "PASS" only when every check holds; the first failing label is printed as `FAIL: <label>`.
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// The mutation tool builds the rig against edited copies of the two headers (a temp tree of the sources, outside the repo).
#ifndef TCB_NOTICE_HEADER
#define TCB_NOTICE_HEADER "../../src/common/terrain_checkerboard_notice.h"
#endif
#include TCB_NOTICE_HEADER
#ifndef TCB_READER_HEADER
#define TCB_READER_HEADER "../../src/d3d11/terrain_checkerboard_reader.h"
#endif
#include TCB_READER_HEADER

// The worker's own source, config, log and guard are linked in the control build and in the worker mutants; a header mutant is built without them
// (TCB_MUTANT), and T5 is left out of it.
#ifndef TCB_MUTANT
#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/d3d11/terrain_checkerboard.h"
#endif

namespace tcn = edvr::tcn;

namespace {

// ---- the harness ---------------------------------------------------------------------------------------------------------
unsigned g_checks = 0;
std::string g_failure;   // the first failing label of the running case
std::string g_root = ".";

void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok && g_failure.empty()) g_failure = label;
}
void checkf(bool ok, const char* fmt, ...) {
    ++g_checks;
    if (ok || !g_failure.empty()) return;
    char buf[600];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_failure = buf;
}
bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

std::string readFile(const std::string& relative) {
    std::string path = g_root;
    if (!path.empty() && path.back() != '\\' && path.back() != '/') path += '\\';
    path += relative;
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out;
    out.reserve(text.size());
    for (char c : text) if (c != '\r') out += c;
    return out;
}
size_t count(const std::string& s, const std::string& needle) {
    size_t n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + needle.size())) ++n;
    return n;
}
// The text of the function whose head is `head` (from the head through its matching brace).
std::string functionBody(const std::string& s, const std::string& head) {
    const size_t at = s.find(head);
    if (at == std::string::npos) return std::string();
    const size_t open = s.find('{', at);
    if (open == std::string::npos) return std::string();
    int depth = 0;
    for (size_t i = open; i < s.size(); ++i) {
        if (s[i] == '{') ++depth;
        else if (s[i] == '}' && --depth == 0) return s.substr(at, i - at + 1);
    }
    return std::string();
}
bool before(const std::string& s, const std::string& a, const std::string& b) {
    const size_t x = s.find(a), y = s.find(b);
    return x != std::string::npos && y != std::string::npos && x < y;
}

// ---- the fixture folders (a temp directory the rig makes and removes) -----------------------------------------------------------
std::wstring g_tmp;
int g_seq = 0;

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // the fixture names are ASCII

void makeDirs(const std::wstring& path) {
    for (size_t at = path.find(L'\\', 3); ; at = path.find(L'\\', at + 1)) {
        CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
        if (at == std::wstring::npos) break;
    }
}
bool writeFile(const std::wstring& path, const std::string& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = bytes.empty() || (WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size());
    CloseHandle(h);
    return ok;
}
// A rewrite the reader sees whole or not at all (a temporary file moved over the old one): for the worker's test, where the file is read at
// any moment. The monitor's test writes in place on purpose, which is what the game does.
bool replaceFile(const std::wstring& path, const std::string& bytes) {
    const std::wstring temp = path + L".tmp";
    if (!writeFile(temp, bytes)) return false;
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
std::string slurp(const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void removeTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring path = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                removeTree(path);
            } else {
                SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

struct Fx {
    std::wstring root, folder, game;   // folder is Options\Graphics, game the folder of the game's exe
    Fx() {
        root = g_tmp + L"\\fx" + std::to_wstring(++g_seq);
        folder = root + L"\\Options\\Graphics";
        game = root + L"\\Game";
        makeDirs(folder);
        makeDirs(game);
    }
    void put(const std::string& name, const std::string& text) { writeFile(folder + L"\\" + widen(name), text); }
    void putGame(const std::string& relative, const std::string& text) {
        const std::wstring path = game + L"\\" + widen(relative);
        makeDirs(path.substr(0, path.rfind(L'\\')));
        writeFile(path, text);
    }
    tcn::Reading read() const { return tcn::readTerrainCheckerboard(folder, game); }
};

// The files as the game writes them (the shape of the ones on the rig that found this, with fewer tags).
std::string customXml(const char* value, const char* eol = "\r\n", const char* indent = "\t", bool withTag = true) {
    const std::string e = eol, i = indent;
    std::string out = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>") + e + "<Root PresetName=\"Custom\" MajorVersion=\"4\" MinorVersion=\"4\">" + e;
    out += i + "<BlurEnabled>false</BlurEnabled>" + e;
    if (withTag) out += i + "<TerrainCheckerboardRenderingEnabled>" + value + "</TerrainCheckerboardRenderingEnabled>" + e;
    out += i + "<DirectionalShadowQuality>1</DirectionalShadowQuality>" + e + i + "<AAMode>0</AAMode>" + e + "</Root>" + e;
    return out;
}
std::string stockXml(const std::string& preset, const char* value, bool withTag = true) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\r\n<RenderOptions PresetName=\"" + preset + "\">\r\n\t<FOV>60.000000</FOV>\r\n\t<AAMode>1</AAMode>\r\n";
    if (withTag) out += std::string("\t<TerrainCheckerboardRenderingEnabled>") + value + "</TerrainCheckerboardRenderingEnabled>\r\n";
    out += "</RenderOptions>\r\n";
    return out;
}
// Settings.xml carries a tag of its own that reads true on every machine seen: it is never the toggle.
std::string settingsXml(const std::string& preset, const char* ownTag = "true", const char* eol = "\r\n") {
    const std::string e = eol;
    std::string out = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\" ?>") + e + "<GraphicsOptions>" + e + "\t<Version>1</Version>" + e;
    out += "\t<PresetName>" + preset + "</PresetName>" + e + "\t<FOV>55.150700</FOV>" + e;
    if (ownTag) out += std::string("\t<TerrainCheckerboardRenderingEnabled>") + ownTag + "</TerrainCheckerboardRenderingEnabled>" + e;
    out += "</GraphicsOptions>" + e;
    return out;
}
bool eq(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

// ---- T1: the words ------------------------------------------------------------------------------------------------------------
void caseT1() {
    check(tcn::kPollMs == 3000 && tcn::kUnknownReads == 2 && tcn::kHintAlternateMs == 6000 && tcn::kLogMax == 16 && tcn::kToastMax == 55 && tcn::kHintMax == 78,
          "T1a: the constants: a 3 s poll, two Unknown reads to leave a known state, a 6 s turn, 16 log lines, a 55-character toast, a 78-character hint");
    char toast[200], hint[200], sentence[300], line[1400];
    tcn::formatToast(toast, sizeof(toast));
    tcn::formatStatusHint(hint, sizeof(hint));
    tcn::formatSentence(sentence, sizeof(sentence));
    check(std::string(toast) == "Distant terrain shimmers: turn off terrain checkerboard" && std::strlen(toast) <= tcn::kToastMax,
          "T1b: the toast says what the player sees and what to turn off, in 55 characters or fewer");
    check(std::string(hint) == "Turn off terrain checkerboard rendering in Elite's graphics options." && std::strlen(hint) <= tcn::kHintMax,
          "T1c: the Status hint says what to turn off and where, in 78 characters or fewer (two lines of the hint box)");
    check(std::string(sentence) ==
              "Elite's terrain checkerboard rendering makes distant terrain shimmer with DLSS. Turn it off in Elite's graphics options." &&
              !std::strchr(sentence, '(') && !std::strchr(sentence, ')'),
          "T1d: the full sentence says what it does and what to set, and has no parenthesis (the log's reader ends the detail at the last one)");
    check(has(toast, "terrain checkerboard") && has(hint, "terrain checkerboard rendering") && has(sentence, "terrain checkerboard rendering") &&
              !has(toast, "Supersampling") && !has(hint, "Supersampling") && !has(sentence, "Supersampling") && !has(hint, "HMD Image Quality"),
          "T1d: the words name the option the way the tag and the game's pass do, and nothing of the supersampling notice");

    tcn::formatLog(line, sizeof(line), tcn::State::On, "Custom", "Custom.4.4.fxcfg", "true", "");
    check(std::string(line) == "vr terrain checkerboard: ON (preset Custom, Custom.4.4.fxcfg: TerrainCheckerboardRenderingEnabled=true): "
                               "Elite's terrain checkerboard rendering makes distant terrain shimmer with DLSS. Turn it off in Elite's graphics options.",
          "T1e: the ON line names the preset, the file and the value it rests on, then the full sentence");
    tcn::formatLog(line, sizeof(line), tcn::State::Off, "VRHigh", "OptionDefaults\\VRHigh.fxcfg", "false", "");
    check(std::string(line) == "vr terrain checkerboard: OFF (preset VRHigh, OptionDefaults\\VRHigh.fxcfg: TerrainCheckerboardRenderingEnabled=false): no notice.",
          "T1e: the OFF line says what it rests on and that there is no notice");
    tcn::formatLog(line, sizeof(line), tcn::State::Unknown, "", "", "", "Settings.xml is missing or unreadable, so the active preset is not known");
    check(std::string(line) == "vr terrain checkerboard: unknown (Settings.xml is missing or unreadable, so the active preset is not known): no notice.",
          "T1e: the unknown line carries its reason and says there is no notice");
    {
        const std::string on = [&] { char b[1400]; tcn::formatLog(b, sizeof(b), tcn::State::On, "VRHigh", "OptionDefaults\\VRHigh.fxcfg", "true", ""); return std::string(b); }();
        check(on.find('\n') == std::string::npos && on.size() < 1000 && on.compare(0, 25, "vr terrain checkerboard: ") == 0,
              "T1e: a line is one line, well inside the log's 1200-byte buffer, and starts with the prefix the reader of the log looks for");
    }

    tcn::formatStartLine(line, sizeof(line), 4242, 3000);
    check(std::string(line) == "vr terrain checkerboard: reading Elite's graphics settings on its own thread (4242), every 3 s, off the render thread.",
          "T1f: the start line says the worker started, on which thread, and how often it reads");
    tcn::formatStartLine(line, sizeof(line), 7, 1000);
    check(has(line, "every 1 s,"), "T1f: the start line's period follows the poll");
    tcn::formatStartFailedLine(line, sizeof(line));
    check(std::string(line) == "vr terrain checkerboard: could not start the reader thread, so Elite's terrain checkerboard rendering is not being read: no notice.",
          "T1f: a thread that could not start says so");
    tcn::formatFaultLine(line, sizeof(line));
    check(std::string(line) == "vr terrain checkerboard: the reader faulted repeatedly and has stopped; the notice keeps the last state it published.",
          "T1f: a reader that faulted for good says so");
    tcn::formatLimitLine(line, sizeof(line));
    check(std::string(line) == "vr terrain checkerboard: 16 lines logged; further changes are followed by the notice but no longer logged.",
          "T1f: the bound's line says how many were logged and that the notice still follows");
    tcn::formatQueuedLog(line, sizeof(line), true, toast);
    check(std::string(line) == "vr terrain checkerboard: the headset notice is queued as a toast (\"Distant terrain shimmers: turn off terrain checkerboard\"); "
                               "the Status page shows the advice as its hint while the menu is open.",
          "T1f: the menu's note names the toast it queued");
    tcn::formatQueuedLog(line, sizeof(line), false, toast);
    check(std::string(line) == "vr terrain checkerboard: menu.toasts is off, so no toast; the Status page shows the advice as its hint while the menu is open.",
          "T1f: with menu.toasts off the note says there is no toast and the hint still shows");
}

// ---- T2: the pure picks -------------------------------------------------------------------------------------------------------
void caseT2() {
    // the published word
    {
        const uint32_t w = tcn::pack(tcn::State::On, 5);
        check(tcn::unpackState(w) == tcn::State::On && tcn::unpackVersion(w) == 5 && tcn::unpackState(tcn::pack(tcn::State::Off, 9)) == tcn::State::Off &&
                  tcn::unpackVersion(tcn::pack(tcn::State::Off, 9)) == 9 && tcn::unpackState(tcn::pack(tcn::State::Unknown, 2)) == tcn::State::Unknown,
              "T2a: the state and the version share one word and read back (a consistent pair)");
        check(tcn::unpackState(0) == tcn::State::Unknown && tcn::unpackVersion(0) == 0 && tcn::unpackState(0xFFu) == tcn::State::Unknown,
              "T2a: the empty word is Unknown at version 0, and a byte that is no state is Unknown");
        check(tcn::nextVersion(0) == 1 && tcn::nextVersion(tcn::pack(tcn::State::On, 1)) == 2 && tcn::nextVersion(tcn::pack(tcn::State::On, 0xFFFFFE)) == 0xFFFFFF &&
                  tcn::nextVersion(tcn::pack(tcn::State::On, 0xFFFFFF)) == 1,
              "T2a: the version counts up from 1 and never comes back to 0 (a latch that starts at 0 has never said anything)");
    }
    // the toast's latch: once per raise, re-armed by Off or by Unknown
    {
        uint32_t latch = 0;
        check(!tcn::toastOnRaise(&latch, tcn::State::Unknown, 0) && !tcn::toastOnRaise(&latch, tcn::State::Unknown, 1) && latch == 0,
              "T2b: nothing read, or read as Unknown, toasts nothing");
        check(tcn::toastOnRaise(&latch, tcn::State::On, 2) && latch == 2, "T2b: the first raise toasts, and sets the latch itself (before the caller tests menu.toasts)");
        check(!tcn::toastOnRaise(&latch, tcn::State::On, 2) && !tcn::toastOnRaise(&latch, tcn::State::On, 2),
              "T2b: an option that stays on toasts once, however many frames look at it");
        check(!tcn::toastOnRaise(&latch, tcn::State::Off, 3), "T2b: Off toasts nothing");
        check(tcn::toastOnRaise(&latch, tcn::State::On, 4) && latch == 4, "T2b: turned off and on again, it toasts again (re-armed by Off)");
        check(!tcn::toastOnRaise(&latch, tcn::State::Unknown, 5) && tcn::toastOnRaise(&latch, tcn::State::On, 6),
              "T2b: Unknown between two raises re-arms it too");
        check(tcn::toastOnRaise(&latch, tcn::State::On, 8) && !tcn::toastOnRaise(&latch, tcn::State::On, 8),
              "T2b: two changes between two frames (Off then On) are still one new raise, because the latch is the version");
    }
    // the Status page's one hint slot
    {
        using tcn::Hint;
        check(tcn::pickStatusHint(false, false, 0) == Hint::None && tcn::pickStatusHint(false, false, 7000) == Hint::None,
              "T2c: neither applies: the page keeps its own hint");
        check(tcn::pickStatusHint(true, false, 0) == Hint::Supersampling && tcn::pickStatusHint(true, false, 7000) == Hint::Supersampling && tcn::pickStatusHint(true, false, 123456789) == Hint::Supersampling,
              "T2c: only the supersampling one applies: it shows, at any time");
        check(tcn::pickStatusHint(false, true, 0) == Hint::Checkerboard && tcn::pickStatusHint(false, true, 7000) == Hint::Checkerboard && tcn::pickStatusHint(false, true, 123456789) == Hint::Checkerboard,
              "T2c: only the checkerboard one applies: it shows, at any time");
        check(tcn::pickStatusHint(true, true, 0) == Hint::Supersampling && tcn::pickStatusHint(true, true, 5999) == Hint::Supersampling,
              "T2c: both apply: the supersampling hint has the first 6 s");
        check(tcn::pickStatusHint(true, true, 6000) == Hint::Checkerboard && tcn::pickStatusHint(true, true, 11999) == Hint::Checkerboard,
              "T2c: ...then the checkerboard hint has 6 s");
        check(tcn::pickStatusHint(true, true, 12000) == Hint::Supersampling && tcn::pickStatusHint(true, true, 18000) == Hint::Checkerboard &&
                  tcn::pickStatusHint(true, true, 6000ull * 1000001ull) == Hint::Checkerboard && tcn::pickStatusHint(true, true, 6000ull * 1000000ull) == Hint::Supersampling,
              "T2c: ...and they keep taking turns");
    }
    // the log's bound
    {
        int logged = 0;
        bool lines = true;
        for (int i = 0; i < tcn::kLogMax; ++i) lines = lines && tcn::logDue(&logged) == tcn::LogDue::Line;
        check(lines && logged == tcn::kLogMax, "T2d: the first 16 reads and changes are logged");
        check(tcn::logDue(&logged) == tcn::LogDue::Limit, "T2d: the next one says the bound was reached, once");
        check(tcn::logDue(&logged) == tcn::LogDue::Nothing && tcn::logDue(&logged) == tcn::LogDue::Nothing && tcn::logDue(&logged) == tcn::LogDue::Nothing,
              "T2d: and then nothing, however many changes follow");
    }
}

// ---- T3: the reader on fixture folders ------------------------------------------------------------------------------------------
void expect(const tcn::Reading& r, tcn::State want, const char* label) {
    checkf(r.state == want, "%s (read %s, wanted %s: %s)", label, tcn::stateWord(r.state), tcn::stateWord(want), r.reason);
}

void caseT3() {
    using tcn::State;
    // a. Custom true and false, with what the answer rests on
    {
        Fx fx;
        fx.put("Settings.xml", settingsXml("Custom"));
        fx.put("Custom.4.4.fxcfg", customXml("true"));
        const tcn::Reading on = fx.read();
        check(on.state == State::On && eq(on.preset, "Custom") && eq(on.source, "Custom.4.4.fxcfg") && eq(on.value, "true"),
              "T3a: Custom with the tag true is ON, and says the preset, the file and the value");
        fx.put("Custom.4.4.fxcfg", customXml("false"));
        const tcn::Reading off = fx.read();
        check(off.state == State::Off && eq(off.value, "false") && eq(off.source, "Custom.4.4.fxcfg"), "T3a: the tag false is OFF");
    }
    // b. the value's spellings
    {
        static const struct { const char* text; State want; } kValues[] = {
            {"true", State::On}, {"True", State::On}, {"TRUE", State::On}, {"1", State::On}, {"  true \t", State::On},
            {"false", State::Off}, {"False", State::Off}, {"FALSE", State::Off}, {"0", State::Off},
            {"yes", State::Unknown}, {"no", State::Unknown}, {"on", State::Unknown}, {"2", State::Unknown}, {"-1", State::Unknown}, {"01", State::Unknown},
            {"", State::Unknown}, {"truee", State::Unknown}, {"tru", State::Unknown}, {"0.0", State::Unknown}};
        for (const auto& v : kValues) {
            Fx fx;
            fx.put("Settings.xml", settingsXml("Custom", "false"));
            fx.put("Custom.4.4.fxcfg", customXml(v.text));
            expect(fx.read(), v.want, "T3b: a value spelled true, True, TRUE, 1 is on; false, False, FALSE, 0 is off; anything else (yes, on, 2, 01, empty) is Unknown");
        }
    }
    // c. the highest Custom version is the one the game reads (older ones linger beside it)
    {
        Fx fx;
        fx.put("Settings.xml", settingsXml("Custom"));
        fx.put("Custom.4.0.fxcfg", customXml("true"));
        fx.put("Custom.4.1.fxcfg", customXml("true"));
        fx.put("Custom.4.3.fxcfg", customXml("true"));
        fx.put("Custom.4.4.fxcfg", customXml("false"));
        const tcn::Reading r = fx.read();
        check(r.state == State::Off && eq(r.source, "Custom.4.4.fxcfg"), "T3c: 4.0, 4.1, 4.3 true and 4.4 false: the highest version, 4.4, is read: OFF");
        Fx fy;
        fy.put("Settings.xml", settingsXml("Custom"));
        fy.put("Custom.4.3.fxcfg", customXml("false"));
        fy.put("Custom.4.4.fxcfg", customXml("true"));
        check(fy.read().state == State::On, "T3c: and the other way round: 4.3 false, 4.4 true: ON");
        Fx fz;
        fz.put("Settings.xml", settingsXml("Custom"));
        fz.put("Custom.4.9.fxcfg", customXml("false"));
        fz.put("Custom.4.10.fxcfg", customXml("true"));
        const tcn::Reading z = fz.read();
        check(z.state == State::On && eq(z.source, "Custom.4.10.fxcfg"), "T3c: 4.10 is above 4.9 (the numbers compare as numbers, not as text)");
        Fx fm;
        fm.put("Settings.xml", settingsXml("Custom"));
        fm.put("Custom.3.9.fxcfg", customXml("true"));
        fm.put("Custom.4.0.fxcfg", customXml("false"));
        check(fm.read().state == State::Off, "T3c: the major version counts before the minor");
        Fx fv;
        fv.put("Settings.xml", settingsXml("Custom"));
        fv.put("Custom.fxcfg", customXml("false"));
        fv.put("Custom.4.0.fxcfg", customXml("true"));
        check(fv.read().state == State::On, "T3c: a versioned Custom file is above the unversioned Custom.fxcfg");
    }
    // d. the lookalikes the game never reads
    {
        Fx fx;
        fx.put("Settings.xml", settingsXml("Custom"));
        fx.put("Custom.4.4.fxcfg", customXml("false"));
        fx.put("-Custom.4.0.fxcfg", customXml("true"));
        fx.put("Custom.4.0.fxcfg-backup", customXml("true"));
        fx.put("Custom.4.4.fxcfg.baseline-bak-20260921", customXml("true"));
        fx.put("Custom.4.4.fxcfg.pre-crisp2-20260903.bak", customXml("true"));
        fx.put("TomCatT.4.0.fxcfg0", customXml("true"));
        fx.put("Settings - Copy.xml0", settingsXml("TomCatT"));
        fx.put("Custom Copy.fxcfg", customXml("true"));
        fx.put("CustomX.4.9.fxcfg", customXml("true"));
        fx.put("Custom.4.9.fxcfg.txt", customXml("true"));
        fx.put("Custom.4.x.fxcfg", customXml("true"));
        fx.put("Custom.4.5.6.fxcfg", customXml("true"));
        const tcn::Reading r = fx.read();
        check(r.state == State::Off && eq(r.source, "Custom.4.4.fxcfg"), "T3d: lookalike files beside the real one (backups, copies, other presets, odd versions) are never read: OFF from Custom.4.4.fxcfg");
        Fx fy;
        fy.put("Settings.xml", settingsXml("Custom"));
        fy.put("-Custom.4.0.fxcfg", customXml("true"));
        fy.put("Custom.4.0.fxcfg-backup", customXml("true"));
        fy.put("Custom Copy.fxcfg", customXml("true"));
        fy.put("CustomX.4.9.fxcfg", customXml("true"));
        fy.put("TomCatT.4.0.fxcfg", customXml("true"));
        expect(fy.read(), State::Unknown, "T3d: with only lookalikes and no Custom.<major>.<minor>.fxcfg the answer is Unknown, never On from a file the game does not read");
    }
    // e. Custom with no Custom file
    {
        Fx fx;
        fx.put("Settings.xml", settingsXml("Custom"));
        const tcn::Reading r = fx.read();
        check(r.state == State::Unknown && has(r.reason, "Custom.<major>.<minor>.fxcfg") && has(r.reason, "no"), "T3e: preset Custom and no Custom file in the folder: Unknown, and the reason says so");
        check(!has(r.reason, "\n") && r.preset[0] == 'C', "T3e: the reason is one line and the preset is still said");
    }
    // f. the files that are not there, or say nothing
    {
        Fx fx;   // no Settings.xml at all
        fx.put("Custom.4.4.fxcfg", customXml("true"));
        const tcn::Reading r = fx.read();
        check(r.state == State::Unknown && has(r.reason, "Settings.xml"), "T3f: no Settings.xml: Unknown (the active preset is not known), whatever Custom files lie there");
        Fx fy;
        fy.put("Settings.xml", "");
        fy.put("Custom.4.4.fxcfg", customXml("true"));
        expect(fy.read(), State::Unknown, "T3f: an empty Settings.xml: Unknown");
        Fx fz;
        fz.put("Settings.xml", "<GraphicsOptions><Version>1</Version></GraphicsOptions>");
        fz.put("Custom.4.4.fxcfg", customXml("true"));
        expect(fz.read(), State::Unknown, "T3f: a Settings.xml with no PresetName: Unknown");
        Fx fw;
        fw.put("Settings.xml", "<GraphicsOptions><PresetName>   </PresetName></GraphicsOptions>");
        fw.put("Custom.4.4.fxcfg", customXml("true"));
        expect(fw.read(), State::Unknown, "T3f: a blank PresetName: Unknown");
        const tcn::Reading gone = tcn::readTerrainCheckerboard(g_tmp + L"\\no\\such\\folder", g_tmp);
        check(gone.state == State::Unknown && has(gone.reason, "no Elite graphics settings folder"), "T3f: no Options\\Graphics folder: Unknown, and the reason names the folder");
        check(tcn::readTerrainCheckerboard(L"", L"").state == State::Unknown, "T3f: no folder at all (LocalAppData not set): Unknown");
    }
    // g. the tag absent: never a guess
    {
        Fx fx;
        fx.put("Settings.xml", settingsXml("Custom"));
        fx.put("Custom.4.4.fxcfg", customXml("", "\r\n", "\t", false));
        const tcn::Reading r = fx.read();
        check(r.state == State::Unknown && has(r.reason, "has no TerrainCheckerboardRenderingEnabled") && has(r.reason, "Custom.4.4.fxcfg"),
              "T3g: the tag absent from the Custom file (an older file): Unknown, never On or Off by inference, and the reason names the file");
        Fx fy;
        fy.put("Settings.xml", settingsXml("VRHigh"));
        fy.putGame("OptionDefaults\\VRHigh.fxcfg", stockXml("VRHigh", "true", false));
        expect(fy.read(), State::Unknown, "T3g: the tag absent from a stock preset's file: Unknown");
    }
    // h. encodings and shapes
    {
        struct Shape { const char* name; std::string text; };
        const std::string crlf = customXml("true", "\r\n", "\t"), lf = customXml("true", "\n", "\t"), two = customXml("true", "\r\n", "  "), none = customXml("true", "", "");
        const Shape shapes[] = {{"CRLF and tabs", crlf}, {"LF only", lf}, {"two-space indent", two}, {"a single line", none},
                                {"a UTF-8 BOM", std::string("\xEF\xBB\xBF") + crlf}, {"a BOM and LF", std::string("\xEF\xBB\xBF") + lf},
                                {"the value on its own lines", "<Root>\r\n<TerrainCheckerboardRenderingEnabled>\r\n\ttrue\r\n</TerrainCheckerboardRenderingEnabled>\r\n</Root>"}};
        for (const Shape& s : shapes) {
            Fx fx;
            fx.put("Settings.xml", settingsXml("Custom"));
            fx.put("Custom.4.4.fxcfg", s.text);
            const tcn::Reading r = fx.read();
            checkf(r.state == State::On && eq(r.value, "true"), "T3h: the tag is read through %s (read %s)", s.name, tcn::stateWord(r.state));
        }
        Fx fx;
        fx.put("Settings.xml", std::string("\xEF\xBB\xBF") + settingsXml("Custom", "true", "\n"));
        fx.put("Custom.4.4.fxcfg", lf);
        expect(fx.read(), State::On, "T3h: Settings.xml with a BOM and LF is read too");
        Fx fy;
        fy.put("Settings.xml", "<GraphicsOptions>\r\n\t<PresetName>\r\n\t\tCustom\r\n\t</PresetName>\r\n</GraphicsOptions>");
        fy.put("Custom.4.4.fxcfg", crlf);
        expect(fy.read(), State::On, "T3h: a PresetName on its own lines is trimmed");
    }
    // i. Settings.xml's own tag is never the toggle
    {
        Fx fx;
        fx.put("Settings.xml", settingsXml("Custom", "true"));
        fx.put("Custom.4.4.fxcfg", customXml("false"));
        expect(fx.read(), State::Off, "T3i: Settings.xml says true and the Custom file says false: OFF (the toggle is the preset's file)");
        Fx fy;
        fy.put("Settings.xml", settingsXml("Custom", "false"));
        fy.put("Custom.4.4.fxcfg", customXml("true"));
        expect(fy.read(), State::On, "T3i: Settings.xml says false and the Custom file says true: ON");
        Fx fz;
        fz.put("Settings.xml", settingsXml("Custom", "true"));
        fz.put("Custom.4.4.fxcfg", customXml("", "\r\n", "\t", false));
        expect(fz.read(), State::Unknown, "T3i: Settings.xml says true and the Custom file has no tag: Unknown, not On from Settings.xml");
        Fx fw;
        fw.put("Settings.xml", settingsXml("VRHigh", "false"));
        fw.putGame("OptionDefaults\\VRHigh.fxcfg", stockXml("VRHigh", "true"));
        expect(fw.read(), State::On, "T3i: nor for a stock preset: Settings.xml says false, the preset's file true: ON");
    }
    // j. commentary
    {
        const std::string head = "<Root>\r\n";
        Fx fx;
        fx.put("Settings.xml", settingsXml("Custom"));
        fx.put("Custom.4.4.fxcfg", head + "<!-- <TerrainCheckerboardRenderingEnabled>true</TerrainCheckerboardRenderingEnabled> -->\r\n</Root>");
        expect(fx.read(), State::Unknown, "T3j: a tag inside a comment is not there: Unknown");
        Fx fy;
        fy.put("Settings.xml", settingsXml("Custom"));
        fy.put("Custom.4.4.fxcfg", head + "<!-- <TerrainCheckerboardRenderingEnabled>true</TerrainCheckerboardRenderingEnabled> -->\r\n<TerrainCheckerboardRenderingEnabled>false</TerrainCheckerboardRenderingEnabled>\r\n</Root>");
        expect(fy.read(), State::Off, "T3j: a commented-out old value before the real one: the real one is read");
        Fx fz;
        fz.put("Settings.xml", settingsXml("Custom"));
        fz.put("Custom.4.4.fxcfg", head + "<!-- note\r\n<TerrainCheckerboardRenderingEnabled>true</TerrainCheckerboardRenderingEnabled>\r\n</Root>");
        expect(fz.read(), State::Unknown, "T3j: a comment that never ends takes the rest of the file with it: Unknown");
        Fx fw;
        fw.put("Settings.xml", "<GraphicsOptions><!-- <PresetName>Custom</PresetName> --><PresetName>VRHigh</PresetName></GraphicsOptions>");
        fw.putGame("OptionDefaults\\VRHigh.fxcfg", stockXml("VRHigh", "true"));
        const tcn::Reading r = fw.read();
        check(r.state == State::On && eq(r.preset, "VRHigh"), "T3j: a commented-out PresetName is not the preset's name");
    }
    // k. the stock presets, with the values the game ships (OptionDefaults\*.fxcfg of the install)
    {
        static const struct { const char* name; const char* value; State want; } kStock[] = {
            {"Low", "true", State::On}, {"Mid", "true", State::On}, {"High", "false", State::Off}, {"Ultra", "false", State::Off},
            {"VRHigh", "true", State::On}, {"VRLow", "true", State::On}, {"VRMedium", "true", State::On}, {"VRUltra", "true", State::On}};
        for (const auto& s : kStock) {
            Fx fx;
            fx.put("Settings.xml", settingsXml(s.name));
            fx.putGame(std::string("OptionDefaults\\") + s.name + ".fxcfg", stockXml(s.name, s.value));
            const tcn::Reading r = fx.read();
            const std::string source = std::string("OptionDefaults\\") + s.name + ".fxcfg";
            checkf(r.state == s.want && eq(r.preset, s.name) && source == r.source,
                   "T3k: stock preset %s reads %s from OptionDefaults (read %s, source %s): every stock VR preset has it ON, High and Ultra OFF", s.name,
                   s.value, tcn::stateWord(r.state), r.source);
        }
        // a Custom file in the folder does not decide a stock preset
        Fx fx;
        fx.put("Settings.xml", settingsXml("VRHigh"));
        fx.put("Custom.4.4.fxcfg", customXml("false"));
        fx.putGame("OptionDefaults\\VRHigh.fxcfg", stockXml("VRHigh", "true"));
        expect(fx.read(), State::On, "T3k: a stock preset is read from the game's OptionDefaults, not from a Custom file beside Settings.xml");
        Fx fy;
        fy.put("Settings.xml", settingsXml("Custom"));
        fy.put("Custom.4.4.fxcfg", customXml("false"));
        fy.putGame("OptionDefaults\\VRHigh.fxcfg", stockXml("VRHigh", "true"));
        expect(fy.read(), State::Off, "T3k: and Custom is read from the folder, not from a stock file");
    }
    // l. stock presets and names the reader does not understand
    {
        Fx fx;
        fx.put("Settings.xml", settingsXml("VRHigh"));
        const tcn::Reading r = fx.read();
        check(r.state == State::Unknown && has(r.reason, "OptionDefaults\\VRHigh.fxcfg"), "T3l: a stock preset whose OptionDefaults file is missing: Unknown, and the reason names the file");
        Fx fy;
        fy.put("Settings.xml", settingsXml("VRHigh"));
        fy.putGame("OptionDefaults\\VRHigh.fxcfg", stockXml("VRHigh", "true"));
        check(tcn::readTerrainCheckerboard(fy.folder, L"").state == State::Unknown, "T3l: no game folder: Unknown");
        Fx fz;   // a user-made preset has its own files in the options folder, which are not read
        fz.put("Settings.xml", settingsXml("TomCatT"));
        fz.put("TomCatT.4.0.fxcfg", customXml("true"));
        fz.put("TomCatT.fxcfg", customXml("true"));
        const tcn::Reading u = fz.read();
        check(u.state == State::Unknown && has(u.reason, "TomCatT"), "T3l: a user-made preset (its own TomCatT.<version>.fxcfg files, none in OptionDefaults): Unknown, never read");
        // a name that could be a path is never put in one: each of these would reach a file that says true if it were
        const char* names[] = {"..\\..\\Options\\Graphics\\evil", "../evil", "A/B", "..", "C:evil", "evil.fxcfg", "a b", "\\\\server\\share\\x",
                               "VRHigh\x01", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"};
        for (const char* n : names) {
            Fx fp;
            fp.put("Settings.xml", settingsXml(n));
            fp.put("evil.fxcfg", stockXml("evil", "true"));
            fp.putGame("evil.fxcfg", stockXml("evil", "true"));
            fp.putGame("OptionDefaults\\evil.fxcfg", stockXml("evil", "true"));
            const tcn::Reading p = fp.read();
            checkf(p.state == State::Unknown && std::strchr(p.reason, '\n') == nullptr && std::strchr(p.preset, '\x01') == nullptr,
                   "T3l: the preset name \"%s\" could be a path or is no file stem: Unknown, with no file read and no control byte in the log's field", n);
        }
    }
    // m. case
    {
        Fx fx;
        fx.put("Settings.xml", settingsXml("custom"));
        fx.put("custom.4.4.fxcfg", customXml("true"));
        expect(fx.read(), State::On, "T3m: the preset name and the Custom file's name are compared without regard to case (the files are on NTFS)");
    }
    // n. a read changes nothing
    {
        Fx fx;
        fx.put("Settings.xml", settingsXml("Custom"));
        fx.put("Custom.4.4.fxcfg", customXml("true"));
        const std::wstring custom = fx.folder + L"\\Custom.4.4.fxcfg";
        SetFileAttributesW(custom.c_str(), FILE_ATTRIBUTE_READONLY);
        auto stamp = [&](const std::wstring& path) {
            WIN32_FILE_ATTRIBUTE_DATA d{};
            GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d);
            return (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32 | d.ftLastWriteTime.dwLowDateTime) ^ (static_cast<uint64_t>(d.nFileSizeLow) << 1);
        };
        const uint64_t a = stamp(custom), b = stamp(fx.folder + L"\\Settings.xml");
        const tcn::Reading r = fx.read();
        const size_t files = [&] { size_t n = 0; WIN32_FIND_DATAW fd{}; HANDLE h = FindFirstFileW((fx.folder + L"\\*").c_str(), &fd);
                                    if (h != INVALID_HANDLE_VALUE) { do { ++n; } while (FindNextFileW(h, &fd)); FindClose(h); } return n; }();
        check(r.state == State::On && stamp(custom) == a && stamp(fx.folder + L"\\Settings.xml") == b && files == 4,
              "T3n: a read-only Custom file reads fine, and a read writes nothing (the same write times and sizes, no file added: the folder holds . .. and the two files)");
        SetFileAttributesW(custom.c_str(), FILE_ATTRIBUTE_NORMAL);
    }
    // o. the log's field is one line whatever the file holds
    {
        Fx fx;
        fx.put("Settings.xml", "<GraphicsOptions><PresetName>Hi\r\ngh\tx</PresetName></GraphicsOptions>");
        const tcn::Reading r = fx.read();
        bool control = false;
        for (const char* p = r.preset; *p; ++p) control = control || static_cast<unsigned char>(*p) < 0x20;
        for (const char* p = r.reason; *p; ++p) control = control || static_cast<unsigned char>(*p) < 0x20;
        check(r.state == State::Unknown && !control && std::strchr(r.preset, '?') != nullptr, "T3o: a control byte in a name from a file is shown as ? in the log's fields");
    }
}

// ---- T4: the monitor's rule ---------------------------------------------------------------------------------------------------------
void caseT4() {
    using tcn::State;
    bool moved = false;
    Fx fx;
    fx.put("Settings.xml", settingsXml("Custom"));
    fx.put("Custom.4.4.fxcfg", customXml("true"));
    tcn::Monitor m;
    m.configure(fx.folder, fx.game);
    check(!m.havePublished() && m.published().state == State::Unknown, "T4a: nothing is published before the first poll");
    check(m.poll(&moved) && moved && m.published().state == State::On && m.havePublished(), "T4a: the first poll publishes what it read, as a change of the state (version 1)");
    moved = true;
    check(!m.poll(&moved) && !moved, "T4a: a poll that finds the same thing changes nothing");

    // a rewrite is seen at the next poll (the game rewrites in place, so the write time may not even differ: the content is read every time)
    fx.put("Custom.4.4.fxcfg", customXml("false"));
    check(m.poll(&moved) && moved && m.published().state == State::Off && eq(m.published().value, "false"), "T4b: rewriting the file flips the published state at the next poll");
    fx.put("Custom.4.4.fxcfg", customXml("true"));
    check(m.poll(&moved) && moved && m.published().state == State::On, "T4b: and back");

    // one bad read of a good file (the game truncates and rewrites in place): the last state stands; the second read agrees: Unknown
    fx.put("Custom.4.4.fxcfg", "");
    moved = true;
    check(!m.poll(&moved) && !moved && m.published().state == State::On, "T4c: one empty read of a file that read fine keeps ON (and is retried at the next poll)");
    check(m.poll(&moved) && moved && m.published().state == State::Unknown && has(m.published().reason, "has no TerrainCheckerboardRenderingEnabled"),
          "T4c: the second read agrees: Unknown, with the reason");
    fx.put("Custom.4.4.fxcfg", customXml("false"));
    check(m.poll(&moved) && moved && m.published().state == State::Off, "T4d: Unknown to a known state is at once: one good read is believed");

    // a transient does not accumulate: bad, good, bad is still no change
    fx.put("Custom.4.4.fxcfg", "");
    check(!m.poll(&moved) && m.published().state == State::Off, "T4e: bad read: keeps OFF");
    fx.put("Custom.4.4.fxcfg", customXml("false"));
    check(!m.poll(&moved) && !moved && m.published().state == State::Off, "T4e: good read of the same thing: no change");
    fx.put("Custom.4.4.fxcfg", "");
    check(!m.poll(&moved) && m.published().state == State::Off, "T4e: a bad read after a good one is the FIRST bad read again: still OFF (the count was reset)");
    fx.put("Custom.4.4.fxcfg", customXml("true"));
    check(m.poll(&moved) && moved && m.published().state == State::On, "T4e: a good read that differs is believed at once");

    // a file caught after the tag was written: the tag is early in the file, so the new value is there and is believed; caught inside the value it
    // says nothing yet, and that is a bad read like any other
    {
        const std::string whole = customXml("false"), close = "</TerrainCheckerboardRenderingEnabled>", open = "<TerrainCheckerboardRenderingEnabled>";
        fx.put("Custom.4.4.fxcfg", whole.substr(0, whole.find(close) + close.size()));
        check(m.poll(&moved) && moved && m.published().state == State::Off, "T4f: a file cut after its tag (half written) already says its new value: believed at once");
        fx.put("Custom.4.4.fxcfg", customXml("true").substr(0, customXml("true").find(open) + open.size() + 2));
        check(!m.poll(&moved) && !moved && m.published().state == State::Off, "T4f: a file cut inside the value (tr) says nothing yet: the last state stands");
        fx.put("Custom.4.4.fxcfg", customXml("true"));
        check(m.poll(&moved) && moved && m.published().state == State::On, "T4f: and the finished file is believed");
    }

    // Settings.xml gone for two reads
    check(!m.poll(&moved) && m.published().state == State::On, "T4g: ON, and nothing changed");
    DeleteFileW((fx.folder + L"\\Settings.xml").c_str());
    check(!m.poll(&moved) && m.published().state == State::On, "T4g: Settings.xml missing for one read: keeps ON");
    check(m.poll(&moved) && moved && m.published().state == State::Unknown && has(m.published().reason, "Settings.xml"), "T4g: missing for two reads: Unknown");

    // a change that is not a change of the state still goes to the log
    {
        Fx fy;
        fy.put("Settings.xml", settingsXml("Custom"));
        fy.put("Custom.4.4.fxcfg", customXml("true"));
        fy.putGame("OptionDefaults\\VRHigh.fxcfg", stockXml("VRHigh", "true"));
        tcn::Monitor n;
        n.configure(fy.folder, fy.game);
        n.poll(&moved);
        fy.put("Settings.xml", settingsXml("VRHigh"));
        check(n.poll(&moved) && !moved && n.published().state == State::On && eq(n.published().source, "OptionDefaults\\VRHigh.fxcfg"),
              "T4h: Custom ON to a stock preset that is also ON: a change to log (a different source), not a change of the state (no new version, so no second toast)");
        fy.put("Settings.xml", settingsXml("TomCatT"));
        check(!n.poll(&moved) && n.published().state == State::On, "T4h: a preset the reader does not understand is Unknown, and one such read does not leave ON");
        check(n.poll(&moved) && moved && n.published().state == State::Unknown, "T4h: the second one does");
        fy.put("Settings.xml", settingsXml("TomCat2"));
        check(n.poll(&moved) && !moved && n.published().state == State::Unknown && has(n.published().reason, "TomCat2"),
              "T4h: Unknown for another reason is a change to log, and not of the state");
        fy.put("Settings.xml", settingsXml("TomCat2"));
        check(!n.poll(&moved), "T4h: and the same reason again is nothing");
    }
    // the first read is published whatever it says
    {
        Fx fy;
        tcn::Monitor n;
        n.configure(fy.folder, fy.game);
        check(n.poll(&moved) && moved && n.havePublished() && n.published().state == State::Unknown, "T4i: a first read that finds nothing is published too: Unknown at version 1, and it is logged");
    }
}

// ---- T5: the worker thread on a real log ---------------------------------------------------------------------------------------------
#ifndef TCB_MUTANT
// Waits for a condition the worker should bring about. Once a check of this case has failed nothing is waited for any more (the state is
// broken, and ten seconds per remaining wait would only make a broken worker slow to report).
template <class F>
bool waitFor(F cond, unsigned timeoutMs = 10000) {
    if (!g_failure.empty()) return cond();
    for (unsigned waited = 0; waited <= timeoutMs; waited += 5) {
        if (cond()) return true;
        Sleep(5);
    }
    return cond();
}
std::string runReader(const std::wstring& log, int* rc) {
    std::string narrow;
    for (wchar_t c : log) narrow.push_back(static_cast<char>(c));
    std::string script = g_root;
    if (!script.empty() && script.back() != '\\' && script.back() != '/') script += '\\';
    script += "tools\\edvr_log.py";
    const std::string cmd = "python \"" + script + "\" --file \"" + narrow + "\" --terrain-checkerboard 2>&1";
    std::string out;
    *rc = -1;
    if (FILE* p = _popen(cmd.c_str(), "r")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
        *rc = _pclose(p);
    }
    return out;
}
std::wstring newestLog(const std::wstring& dir, const wchar_t* tag) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\edvr_" + tag + L"_*.log").c_str(), &fd);
    std::wstring best;
    if (h != INVALID_HANDLE_VALUE) {
        do { const std::wstring n = dir + L"\\" + fd.cFileName; if (n > best) best = n; } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return best;
}
std::vector<std::string> linesWith(const std::string& log, const char* needle) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < log.size()) {
        size_t eol = log.find('\n', at);
        if (eol == std::string::npos) eol = log.size();
        const std::string one = log.substr(at, eol - at);
        if (one.find(needle) != std::string::npos) out.push_back(one);
        at = eol + 1;
    }
    return out;
}

void caseT5() {
    using edvr::Log;
    using edvr::RuntimeProfile;
    const std::wstring logDir = g_tmp + L"\\logs";
    makeDirs(logDir);

    // ---- the flat profile: nothing starts, nothing is written ----
    {
        Fx fx;
        fx.put("Settings.xml", settingsXml("Custom"));
        fx.put("Custom.4.4.fxcfg", customXml("true"));
        edvr::g_runtimeProfile = RuntimeProfile::Flat;
        edvr::terrainCheckerboardTestSetPaths(fx.folder.c_str(), fx.game.c_str(), 15);
        check(Log::get().open(logDir, L"tcbflat"), "T5a: the log opens in the temp directory");
        for (int i = 0; i < 6; ++i) edvr::terrainCheckerboardTick();
        Sleep(200);
        tcn::State s = tcn::State::On;
        uint32_t v = 99;
        edvr::terrainCheckerboardPublished(&s, &v);
        check(!edvr::terrainCheckerboardTestWorkerRunning() && s == tcn::State::Unknown && v == 0 && !edvr::terrainCheckerboardOn(),
              "T5a: the flat profile starts no worker, publishes nothing and is never ON");
        Log::get().close();
        check(!has(slurp(newestLog(logDir, L"tcbflat")), "vr terrain checkerboard"), "T5a: the flat profile writes no line at all");
        edvr::terrainCheckerboardTestReset();
    }

    // ---- stopped before the first tick: a later tick starts nothing ----
    {
        Fx fx;
        fx.put("Settings.xml", settingsXml("Custom"));
        fx.put("Custom.4.4.fxcfg", customXml("true"));
        edvr::g_runtimeProfile = RuntimeProfile::Vr;
        edvr::terrainCheckerboardTestSetPaths(fx.folder.c_str(), fx.game.c_str(), 15);
        check(Log::get().open(logDir, L"tcbstop"), "T5f: a log opens for the stopped session");
        edvr::terrainCheckerboardShutdown();
        for (int i = 0; i < 5; ++i) edvr::terrainCheckerboardTick();
        Sleep(150);
        tcn::State s = tcn::State::On;
        uint32_t v = 99;
        edvr::terrainCheckerboardPublished(&s, &v);
        check(!edvr::terrainCheckerboardTestWorkerRunning() && s == tcn::State::Unknown && v == 0, "T5f: a tick after the shutdown starts no worker, even in the VR profile");
        Log::get().close();
        check(!has(slurp(newestLog(logDir, L"tcbstop")), "vr terrain checkerboard"), "T5f: and writes no line");
        edvr::terrainCheckerboardTestReset();
    }

    // ---- the VR profile with no settings folder: a worker that read and found nothing says so (not silence) ----
    {
        edvr::g_runtimeProfile = RuntimeProfile::Vr;
        edvr::terrainCheckerboardTestSetPaths((g_tmp + L"\\nothing\\here").c_str(), g_tmp.c_str(), 15);
        check(Log::get().open(logDir, L"tcbnone"), "T5b: a second log opens");
        edvr::terrainCheckerboardTick();
        tcn::State s = tcn::State::On;
        uint32_t v = 0;
        const bool read = waitFor([&] { edvr::terrainCheckerboardPublished(&s, &v); return v != 0; });
        check(read && s == tcn::State::Unknown && v == 1 && !edvr::terrainCheckerboardOn(), "T5b: a worker that found no folder publishes Unknown at version 1: read, nothing there");
        edvr::terrainCheckerboardShutdown();
        waitFor([] { return !edvr::terrainCheckerboardTestWorkerRunning(); });
        Log::get().close();
        const std::string log = slurp(newestLog(logDir, L"tcbnone"));
        const auto reads = linesWith(log, "vr terrain checkerboard: unknown (");
        check(count(log, "reading Elite's graphics settings on its own thread") == 1 && reads.size() == 1 && has(reads[0], "no Elite graphics settings folder"),
              "T5b: the log says the worker started and then that it found no folder: one start line and one unknown line, which is not 'never ran'");
        edvr::terrainCheckerboardTestReset();
    }

    // ---- the VR profile: start once, publish at each change, log each, stop on the flag ----
    {
        Fx fx;
        fx.put("Settings.xml", settingsXml("Custom"));
        fx.put("Custom.4.4.fxcfg", customXml("true"));
        edvr::g_runtimeProfile = RuntimeProfile::Vr;
        edvr::terrainCheckerboardTestSetPaths(fx.folder.c_str(), fx.game.c_str(), 15);
        check(Log::get().open(logDir, L"tcbvr"), "T5c: the session's log opens");
        tcn::State s = tcn::State::Unknown;
        uint32_t v = 0, latch = 0;
        int toasts = 0;
        auto observe = [&] { edvr::terrainCheckerboardPublished(&s, &v); if (tcn::toastOnRaise(&latch, s, v)) ++toasts; };
        observe();
        check(s == tcn::State::Unknown && v == 0 && toasts == 0 && !edvr::terrainCheckerboardTestWorkerRunning(), "T5c: before the first tick nothing runs and nothing is published");
        for (int i = 0; i < 5; ++i) edvr::terrainCheckerboardTick();
        check(edvr::terrainCheckerboardTestWorkerRunning(), "T5c: the first tick starts the worker (a lazy start, from the tick)");
        check(waitFor([&] { observe(); return v == 1; }) && s == tcn::State::On && edvr::terrainCheckerboardOn() && toasts == 1,
              "T5c: the first read publishes ON at version 1, and that is one raise");
        edvr::g_runtimeProfile = RuntimeProfile::Flat;
        check(!edvr::terrainCheckerboardOn(), "T5c: the live predicate is false in the flat profile even with ON published (flat never shows it)");
        edvr::g_runtimeProfile = RuntimeProfile::Vr;
        const std::wstring custom = fx.folder + L"\\Custom.4.4.fxcfg";
        replaceFile(custom, customXml("false"));
        check(waitFor([&] { observe(); return v == 2; }) && s == tcn::State::Off && !edvr::terrainCheckerboardOn() && toasts == 1,
              "T5c: the file says false: OFF at version 2, the notice ends, no toast");
        replaceFile(custom, customXml("true"));
        check(waitFor([&] { observe(); return v == 3; }) && s == tcn::State::On && toasts == 2, "T5c: the file says true again: ON at version 3, and the toast is said again (a second raise)");
        replaceFile(custom, "");
        check(waitFor([&] { observe(); return v == 4; }) && s == tcn::State::Unknown && toasts == 2, "T5c: an unreadable preset: Unknown at version 4 after two reads, no toast");
        replaceFile(custom, customXml("true"));
        check(waitFor([&] { observe(); return v == 5; }) && s == tcn::State::On && toasts == 3, "T5c: readable again: ON at version 5, a third raise");
        for (int i = 0; i < 5; ++i) edvr::terrainCheckerboardTick();
        edvr::terrainCheckerboardTick();
        edvr::terrainCheckerboardShutdown();
        check(waitFor([] { return !edvr::terrainCheckerboardTestWorkerRunning(); }, 5000), "T5c: the stop flag and the wake event end the worker");
        replaceFile(custom, customXml("false"));
        Sleep(150);
        observe();
        check(s == tcn::State::On && v == 5 && toasts == 3, "T5c: a stopped worker publishes nothing more");
        edvr::terrainCheckerboardTick();
        check(!edvr::terrainCheckerboardTestWorkerRunning(), "T5c: and a tick after a worker ran and was stopped starts no second one");
        Log::get().close();
        const std::wstring logPath = newestLog(logDir, L"tcbvr");
        const std::string log = slurp(logPath);
        const auto all = linesWith(log, "vr terrain checkerboard: ");
        checkf(count(log, "reading Elite's graphics settings on its own thread") == 1, "T5d: ticks every frame start ONE worker: %zu start lines", count(log, "reading Elite's graphics settings on its own thread"));
        const auto on = linesWith(log, "vr terrain checkerboard: ON (");
        const auto off = linesWith(log, "vr terrain checkerboard: OFF (");
        const auto unknown = linesWith(log, "vr terrain checkerboard: unknown (");
        check(all.size() == 6 && on.size() == 3 && off.size() == 1 && unknown.size() == 1, "T5d: the log has the start line and one line for each of the five changes: ON, OFF, ON, unknown, ON");
        check(!on.empty() && has(on[0], "ON (preset Custom, Custom.4.4.fxcfg: TerrainCheckerboardRenderingEnabled=true): Elite's terrain checkerboard rendering makes distant terrain shimmer with DLSS."),
              "T5d: the first read's line is the ON line of the header, with the file it read");
        check(all.size() == 6 && has(all[0], "reading Elite's graphics settings on its own thread") && has(all[1], " ON (") && has(all[2], " OFF (") && has(all[3], " ON (") && has(all[4], " unknown (") && has(all[5], " ON ("),
              "T5d: in the order they happened, the start line first");
        check(!unknown.empty() && has(unknown[0], "has no TerrainCheckerboardRenderingEnabled"), "T5d: the unknown line carries its reason");
        check(!has(log, "headset notice is queued") && !has(log, "no toast"), "T5d: the worker writes no toast line (the menu's tick does, once per raise)");
        int rc = -1;
        const std::string out = runReader(logPath, &rc);
        check(rc == 0 && has(out, "vr terrain checkerboard verdict:") && has(out, "PASS (READER) the worker started") && has(out, "5 read line(s) in the order they were logged") &&
                  has(out, "read ") && has(out, ": ON in preset Custom (Custom.4.4.fxcfg), TerrainCheckerboardRenderingEnabled=true") && has(out, "WARN (HEADSET)"),
              "T5e: tools\\edvr_log.py --terrain-checkerboard reads the lines the real worker wrote: the worker, the five reads, and no menu line (the rig has no menu)");
        edvr::terrainCheckerboardTestReset();
    }
    edvr::g_runtimeProfile = RuntimeProfile::Vr;
}
#else
void caseT5() {}
#endif

// ---- T6: the wiring, as source pins -------------------------------------------------------------------------------------------------
void caseT6() {
    const std::string menu = readFile("src\\d3d11\\menu.cpp");
    const std::string worker = readFile("src\\d3d11\\terrain_checkerboard.cpp");
    const std::string whead = readFile("src\\d3d11\\terrain_checkerboard.h");
    const std::string reader = readFile("src\\d3d11\\terrain_checkerboard_reader.h");
    const std::string notice = readFile("src\\common\\terrain_checkerboard_notice.h");
    const std::string flat = readFile("src\\d3d11\\flat_runtime.cpp");
    const std::string flatSettings = readFile("src\\d3d11\\flat_elite_settings.h");
    const std::string proxy = readFile("src\\d3d11\\d3d11_proxy.cpp");
    const std::string hook = readFile("src\\d3d11\\device_hook.cpp");
    check(!menu.empty() && !worker.empty() && !whead.empty() && !reader.empty() && !notice.empty() && !flat.empty() && !flatSettings.empty() && !proxy.empty() && !hook.empty(),
          "T6a: the sources the pins read are readable from the repo root");
    if (g_failure.size()) return;

    // the menu: one include, the latch's field, the shutdown, and no read of a file on the render thread
    check(count(menu, "#include \"terrain_checkerboard.h\"") == 1 && !has(menu, "terrain_checkerboard_reader") && !has(menu, "readTerrainCheckerboard") && !has(menu, "tcn::Monitor"),
          "T6a: menu.cpp includes the worker's small header and never the reader: no file is read on the render thread");
    check(has(menu, "uint32_t    terrainCheckerboardToasted = 0;"), "T6a: the toast's latch is a field of the menu's state, the published version that last toasted");
    check(has(functionBody(menu, "void menuShutdown() {"), "terrainCheckerboardShutdown();"), "T6a: the menu's shutdown stops the worker (its stop flag and wake event)");

    // the toast: the VR branch only, the latch set before menu.toasts is tested, logged either way
    const std::string tick = functionBody(menu, "void menuTick(ID3D11Device* dev) {");
    const size_t vrBranch = tick.find("guardedBudget(g_budget, [&] {\n        static uint64_t lastNativeRevision = 0;");
    const size_t call = tick.find("terrainCheckerboardTick();");
    const size_t published = tick.find("terrainCheckerboardPublished(&cbState, &cbVersion);");
    const size_t latch = tick.find("if (tcn::toastOnRaise(&s.terrainCheckerboardToasted, cbState, cbVersion)) {");
    const size_t toastsTest = tick.find("if (s.toasts) s.toastQueue.push_back(terrainToast);");
    const size_t note = tick.find("Log::get().note(\"%s\", terrainNote);");
    check(vrBranch != std::string::npos && call != std::string::npos && vrBranch < call && count(tick, "terrainCheckerboardTick();") == 1 && !has(tick.substr(0, vrBranch), "terrainCheckerboard"),
          "T6b: the worker is started from the menu tick's VR branch, once in the source, after the flat profile's branch returned: flat never reaches it");
    check(published != std::string::npos && latch != std::string::npos && toastsTest != std::string::npos && note != std::string::npos && call < published && published < latch &&
              latch < toastsTest && toastsTest < note,
          "T6b: the published pair is loaded, then the latch is set (toastOnRaise), then menu.toasts is tested, then the note is logged");
    check(has(tick, "tcn::formatQueuedLog(terrainNote, sizeof(terrainNote), s.toasts, terrainToast);") && count(menu, "tcn::toastOnRaise(") == 1 && count(menu, "tcn::formatToast(") == 1 &&
              count(menu, "tcn::formatQueuedLog(") == 1,
          "T6b: the note is logged whether the toast was queued or menu.toasts is off, and each of the words is asked for once");

    // the Status page's hint: after the supersampling one, live, and no line added
    const std::string build = functionBody(menu, "void buildContent(MenuContent& c) {");
    const size_t statusAt = build.find("buildStatus(c);");
    const size_t ss = build.find("vrss::formatStatusHint(c.hint, sizeof(c.hint));");
    const size_t pick = build.find("if (tcn::pickStatusHint(vrss::below(rw, rh, ew, eh), terrainCheckerboardOn(), s.tickMs) == tcn::Hint::Checkerboard)");
    const size_t write = build.find("tcn::formatStatusHint(c.hint, sizeof(c.hint));");
    check(statusAt != std::string::npos && ss != std::string::npos && pick != std::string::npos && write != std::string::npos && statusAt < ss && ss < pick && pick < write &&
              build.rfind("vrss::formatStatusHint(c.hint") == ss && count(menu, "tcn::formatStatusHint(") == 1 && count(menu, "tcn::pickStatusHint(") == 1,
          "T6c: the Status page's hint is picked after the supersampling hint was written, by the page's one slot, and nothing writes the supersampling hint after it");
    check(has(menu, "terrainCheckerboardOn(), s.tickMs)") && !has(menu, "terrainCheckerboardToasted != 0") && !has(menu, "statusLine(c, \"Terrain checkerboard") && !has(menu, "statusLine(c, \"Checkerboard") &&
              !has(menu, "formatNote"),
          "T6c: the hint follows the live predicate (it ends when the option does), the latch is not the predicate, and no line or note is added to any page");

    // flat never touches it
    check(!has(flat, "terrainCheckerboard") && !has(flat, "terrain_checkerboard") && !has(flat, "tcn::") && !has(flatSettings, "Checkerboard") && !has(flatSettings, "TerrainCheckerboard"),
          "T6d: the flat runtime and the flat panel's Elite settings never read, show or name it");
    check(!has(proxy, "terrainCheckerboard") && !has(hook, "terrainCheckerboard") && !has(worker, "WINAPI DllMain") && !has(worker, "DLL_PROCESS_ATTACH"),
          "T6d: the worker is started from the menu's tick only: never from DllMain (d3d11_proxy.cpp) or the device hook");

    // the worker's lifetime
    const std::string tickBody = functionBody(worker, "void terrainCheckerboardTick() {");
    check(has(worker, "try {\n        std::thread(workerMain, &s).detach();\n        return true;\n    } catch (...) {\n        return false;\n    }") && count(worker, ".detach();") == 1,
          "T6e: the worker is a detached std::thread started inside a try block, like the journal worker's");
    check(before(tickBody, "if (g_startTried.load(std::memory_order_relaxed)) return;", "if (!runtimeVrProfile() || g_stopped.load(std::memory_order_relaxed)) return;") &&
              before(tickBody, "if (!runtimeVrProfile() || g_stopped.load(std::memory_order_relaxed)) return;", "g_startTried.store(true, std::memory_order_relaxed);") &&
              before(tickBody, "g_startTried.store(true, std::memory_order_relaxed);", "startWorker(*s)") && count(worker, "startWorker(*s)") == 1,
          "T6e: the tick returns at once when it has started, starts nothing in the flat profile or after the stop, and starts the worker once");
    const std::string shutdown = functionBody(worker, "void terrainCheckerboardShutdown() {");
    check(has(shutdown, "g_stopped.store(true, std::memory_order_relaxed);") && has(shutdown, "s->stop.store(true, std::memory_order_release);") && has(shutdown, "SetEvent(s->wake);") &&
              has(worker, "WaitForSingleObject(s.wake, s.pollMs);"),
          "T6e: the worker waits on an event for its poll and the shutdown sets the stop flag and the event");
    check(has(functionBody(worker, "void workerMain(Session* sp) {"), "catch (...)") && has(worker, "guardedBudget(s.budget, [&] { pass(monitor, &logged); })") &&
              has(worker, "FaultBudget budget{\"terrain_checkerboard.worker\", 3};"),
          "T6e: no exception leaves the thread, and a pass runs inside the fault budget");
    const std::string pass = functionBody(worker, "void pass(tcn::Monitor& monitor, int* logged) {");
    check(has(pass, "switch (tcn::logDue(logged)) {") && has(pass, "tcn::formatLog(line, sizeof(line), r.state, r.preset, r.source, r.value, r.reason);") && has(pass, "if (stateMoved) {") &&
              pass.find("g_published.store(") != std::string::npos && pass.find("g_published.store(") > pass.rfind("Log::get().note("),
          "T6e: a change is logged (bounded) BEFORE the state is published, and only a change of the state is published");
    check(has(worker, "tcn::Monitor monitor;") && has(worker, "tcn::formatStartLine(line, sizeof(line), GetCurrentThreadId(), s.pollMs);") && !has(worker, "Config::") && !has(worker, "getBool(") &&
              !has(worker, "getString(") && !has(whead, "Config"),
          "T6e: the worker says it started, polls through the monitor, and reads no config key (the feature has none)");

    // the reader's rules
    check(count(reader, "flatXmlText(") == 2 && has(reader, "flatXmlText(stripXmlComments(settings), \"PresetName\", &preset)") && has(reader, "flatXmlText(stripXmlComments(xml), kTag, &text)") &&
              !has(reader, "stripXmlComments(settings), kTag") && has(reader, "inline constexpr char kTag[] = \"TerrainCheckerboardRenderingEnabled\";"),
          "T6f: Settings.xml is asked for the preset's name and for nothing else; the toggle is read from the preset's own file");
    check(has(reader, "flatListEliteGraphics(folder)") && has(reader, "flatPickCustomFxcfg(versioned)") && has(reader, "flatCustomFxcfgVersion(f.name, &major, &minor)") && has(reader, "flatReadSmallFile(") &&
              !has(reader, "FindFirstFile") && !has(reader, "CreateFileW") && !has(reader, "ReadFile(") && !has(reader, "WriteFile") && !has(reader, "DeleteFile") && !has(reader, "MoveFile"),
          "T6f: the folder listing, the highest-version pick and the file read are flat_elite_settings.h's own helpers (called, not copied), and the reader writes nothing");

    // the words header stays pure
    check(!has(notice, "windows.h") && !has(notice, "Config") && !has(notice, "Log::") && !has(notice, "std::string") && count(notice, "#include") == 3,
          "T6g: the words header includes three standard headers and nothing else: no Windows, no config, no log");
}

struct Case { const char* id; void (*run)(); };
const Case kCases[] = {{"T1", caseT1}, {"T2", caseT2}, {"T3", caseT3}, {"T4", caseT4},
#ifndef TCB_MUTANT
                       {"T5", caseT5},
#endif
                       {"T6", caseT6}};

bool selected(const std::string& only, const char* id) {
    if (only.empty()) return true;
    const std::string wanted = "," + only + ",";
    return wanted.find(std::string(",") + id + ",") != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
    std::string only;
    bool selfTest = false, dry = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test")) selfTest = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!std::strcmp(argv[i], "--root") && i + 1 < argc) g_root = argv[++i];
        else if (argv[i][0] != '-') g_root = argv[i];
        else { std::fputs("usage: --self-test [<repo root>] [--only T1,T3,...] | --dry-run\n", stderr); return 2; }
    }
    if (dry) {
        std::puts("terrain_checkerboard_test: --dry-run: nothing run, nothing written");
        return 0;
    }
    if (!selfTest) {
        std::fputs("usage: --self-test [<repo root>] [--only T1,T3,...] | --dry-run\n", stderr);
        return 2;
    }
    if (!only.empty()) {
        for (const char* id = only.c_str(); *id;) {
            const char* comma = std::strchr(id, ',');
            const std::string one = comma ? std::string(id, comma) : std::string(id);
            bool known = false;
            for (const Case& c : kCases) known = known || one == c.id;
            if (!known) { std::fprintf(stderr, "FAIL: --only names a case that does not exist: %s\n", one.c_str()); return 1; }
            id = comma ? comma + 1 : id + one.size();
        }
    }
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    g_tmp = std::wstring(temp) + L"edvr_tcb_" + std::to_wstring(GetCurrentProcessId());
    if (g_tmp.back() == L'\\') g_tmp.pop_back();
    removeTree(g_tmp);
    makeDirs(g_tmp);

    std::string firstFailure;
    unsigned ran = 0;
    for (const Case& c : kCases) {
        if (!selected(only, c.id)) continue;
        g_failure.clear();
        c.run();
        ++ran;
        std::printf("case %s: %s\n", c.id, g_failure.empty() ? "ok" : "FAILED");
        if (!g_failure.empty() && firstFailure.empty()) firstFailure = g_failure;
    }
    if (std::getenv("TCB_KEEP")) std::printf("kept: %ls\n", g_tmp.c_str());   // the fixtures and the logs T5 wrote, for a look
    else removeTree(g_tmp);
    if (!ran) { std::fputs("FAIL: --only selected no case\n", stderr); return 1; }
    if (!firstFailure.empty()) {
        std::fprintf(stderr, "FAIL: %s\n", firstFailure.c_str());
        return 1;
    }
    std::printf("terrain_checkerboard_test: PASS (%u checks over %u cases)\n", g_checks, ran);
    return 0;
}
