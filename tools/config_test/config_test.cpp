// config_test -- runs the real parser over the real shipped edvr.ini.
//
// The file users edit is the one asserted here. Two properties of the parser
// that edvr.ini's own layout depends on are proven rather than assumed:
//
//   1. a section header may REPEAT -- edvr.ini opens with the settings most
//      people touch under [fix] and [hotkey], then returns to both further
//      down for the Explorer Cam block. If a repeat were a parse error, or
//      silently discarded everything after it, half the file would do
//      nothing while still looking perfectly valid in an editor.
//   2. a key set twice takes the LAST value, because parse() assigns into a
//      flat section.key map.
//
// Both were originally read out of config.cpp rather than observed, which is
// the wrong order for this project: every symptom of either being false
// appears in the game rather than here.
//
// The rest are regressions with history. The inline-comment and yes/no cases
// were real bugs, and the BOM case files every setting in the file under the
// wrong section while the file still looks fine.
//
// Usage: config_test.exe <dir containing edvr.ini> [scratch dir]
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/common/config.h"

using namespace edvr;

static int g_fails = 0;

static void ok(const char* what) { printf("  ok    %s\n", what); }

static void fail(const char* what, const std::string& detail) {
    printf("  FAIL  %s -- %s\n", what, detail.c_str());
    ++g_fails;
}

static void expectStr(const char* key, const char* want, const char* what) {
    const std::string got = Config::get().getString(key, "<unset>");
    if (got == want) ok(what);
    else fail(what, std::string(key) + " = \"" + got + "\", wanted \"" + want + "\"");
}

static void expectInt(const char* key, int want, const char* what) {
    const int got = Config::get().getInt(key, -999999);
    if (got == want) ok(what);
    else fail(what, std::string(key) + " = " + std::to_string(got) +
                        ", wanted " + std::to_string(want));
}

static void expectFloat(const char* key, float want, const char* what) {
    const float got = Config::get().getFloat(key, -999999.0f);
    const float d = got > want ? got - want : want - got;
    if (d < 0.0001f) ok(what);
    else fail(what, std::string(key) + " = " + std::to_string(got) +
                        ", wanted " + std::to_string(want));
}

static void expectBool(const char* key, bool want, const char* what) {
    // Both defaults, so a key that is absent entirely cannot pass by matching
    // the default that happens to be wanted.
    const bool a = Config::get().getBool(key, false);
    const bool b = Config::get().getBool(key, true);
    if (a == want && b == want) ok(what);
    else fail(what, std::string(key) + " = " + (a ? "true" : "false") + "/" +
                        (b ? "true" : "false") + ", wanted " +
                        (want ? "true" : "false") + " from both defaults");
}

// Deliberately not `std::wstring(std::string(p).begin(), std::string(p).end())`.
// That spells TWO temporaries and takes begin() from one and end() from the
// other, so the range is whatever the gap between two stack objects happens to
// be -- which is how the first run of this test died inside the constructor,
// before a single printf.
static std::wstring widen(const char* p) {
    std::wstring out;
    for (const char* c = p; *c; ++c) out.push_back(static_cast<wchar_t>(*c));
    return out;
}

// A scratch ini written from a literal, for the cases the real file cannot
// contain without being wrong.
static bool writeIni(const std::wstring& dir, const char* body) {
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\edvr.ini";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(f, body, static_cast<DWORD>(strlen(body)), &written, nullptr);
    CloseHandle(f);
    return true;
}

int main(int argc, char** argv) {
    // Unbuffered, so a crash does not take the output with it: the first run of
    // this test appeared to die before its first printf, which was only the
    // buffer being discarded.
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 2) {
        printf("usage: config_test.exe <dir containing edvr.ini> [scratch dir]\n");
        return 2;
    }
    const std::wstring dir = widen(argv[1]);

    Config::get().init(dir);
    if (Config::get().path().empty()) {
        printf("  FAIL  no edvr.ini found under %s\n", argv[1]);
        return 1;
    }
    printf("  read  %S\n", Config::get().path().c_str());

    // --- the shipped file, as the parser sees it ---------------------------
    //
    // The opening [fix] and [hotkey] blocks.
    expectBool("fix.share_exposure", true, "a key in the first [fix] reads");
    expectBool("fix.transition_flash", true, "...and another beside it");
    expectStr("hotkey.toggle_exposure", "SCROLLLOCK",
              "a key in the first [hotkey] reads");
    // The sensitivity pair under [advanced]: users are directed to these by
    // name in the log, so they stay ACTIVE rather than commented-out.
    expectFloat("advanced.transition_flash_units", 2000.0f,
                "the flash threshold reads from [advanced]");
    expectFloat("advanced.transition_flash_speed_factor", 8.0f,
                "...and its speed factor");

    // The Explorer Cam block, under a SECOND [fix] and a second [hotkey].
    // This is the claim that a repeated section header is not a parse error
    // and does not silently discard everything after it.
    expectBool("fix.head_offset_gate", true, "a key under a REPEATED [fix] reads");
    // The preset it applies to moved to [advanced] and ships commented out,
    // so the shipped file must NOT define it -- the compiled default is the
    // one in force. -999999 is this file's "the key is not there" sentinel.
    expectInt("advanced.head_offset_view", -999999,
              "...and the preset it applies to is an expert setting now, not shipped live");
    expectBool("hotkey.read_game_bindings", true,
               "a key under a repeated [hotkey] reads");

    // THE CAMERA KEYS MUST STAY GONE (0.7.1). They were removed because a
    // hand-kept copy of the game's own key configuration is a second copy
    // that drifts, and this one did -- for weeks, silently, agreeing with
    // Elite's ship camera binding while disagreeing with its on-foot one.
    // Documenting them again would resurrect that: the ini value would be
    // read as an override of the bindings file, by a build that no longer
    // reads it, so the setting would appear to work and do nothing.
    expectStr("hotkey.external_camera", "<unset>",
              "the retired external_camera key is still absent");
    expectStr("hotkey.external_camera_next", "<unset>",
              "...and external_camera_next");

    // --- regressions ------------------------------------------------------
    if (argc >= 3) {
        const std::wstring scratch = widen(argv[2]);
        static const char kIni[] =
            "\xEF\xBB\xBF"          // BOM: the byte that used to eat [fix]
            "[fix]\r\n"
            "black_void = 1  # trailing comment, not part of the value\r\n"
            "share_exposure = YES\r\n"
            "panel_distance = 2.5   ; semicolon comment\r\n"
            "[d3d11]\r\n"
            "inventory = no\r\n"
            "[fix]\r\n"             // repeated, as the shipped file does it
            "head_offset_view = 3\r\n"
            // A STRAY KEYSTROKE, which strtol used to read as a deliberate
            // setting. Found in a player ini as
            // transition_flash_max_consecutive = 3w: it parsed to 3, which
            // happens to equal the burst budget, so every excursion spent the
            // whole budget and opened a two-second window in which nothing
            // could be withheld. The flash the fix exists to hide came back,
            // and the cause was one invisible character.
            "head_offset_view_count = 4w\r\n"
            "panel_distance_index = 12 or 13\r\n"
            "black_void = 0\r\n";   // duplicate: the later one must win
        if (!writeIni(scratch, kIni)) {
            fail("scratch ini", "could not write it");
        } else {
            Config::get().init(scratch);
            expectBool("fix.share_exposure", true, "getBool accepts YES");
            expectBool("d3d11.inventory", false, "...and no");
            expectFloat("fix.panel_distance", 2.5f, "a ; comment is not part of the value");
            expectInt("fix.head_offset_view", 3,
                      "a BOM does not swallow the first section");
            expectInt("fix.head_offset_view_count", -999999,
                      "a trailing letter is refused, not silently truncated");
            expectInt("fix.panel_distance_index", -999999,
                      "...and so is a value with words after the number");
            expectBool("fix.black_void", false,
                       "a duplicate key takes the later value");
        }

        // --- the config audit: moved keys and case, over fixture tables ---
        //
        // The DLLs register generated tables (config_audit.cpp); here small
        // fixture tables prove the mechanics without depending on which keys
        // happen to be moved this release. The scenario is 2026-08-27's
        // field bug exactly: new DLLs hand-copied over an old-layout ini,
        // where the value the user pinned sat under a section nothing read
        // any more -- and the compiled default (inherit) moved the whole
        // scanner UI.
        static const char* kKnown[] = {"fix.alpha", "experimental.beta"};
        static const char* kMoved[][3] = {
            {"fix.beta", "experimental.beta", ""},
            // A move whose old key shipped a default: a user line still
            // carrying that default is an un-updated file, not a choice, and
            // must NOT follow the move -- the new key's own default rules.
            // 2026-08-28's field bug exactly: menu_backdrop = stock (the old
            // shipped default) suppressing intro_backdrop's new splash.
            {"fix.gamma", "experimental.gamma", "stock"},
        };
        Config::get().setAuditTables(kKnown, 2, kMoved, 2);
        static const char kAuditIni[] =
            "[fix]\r\n"
            "beta = 7\r\n"          // the old-layout line: must follow the move
            "AlPhA = 3\r\n"         // case-typo: used to be filed unfindably
            "mystery = 9\r\n";      // a key nothing reads: named, not eaten
        if (!writeIni(scratch, kAuditIni)) {
            fail("audit scratch ini", "could not write it");
        } else {
            Config::get().init(scratch);
            expectInt("experimental.beta", 7,
                      "an old-layout value is read through the moved-from map");
            expectInt("fix.alpha", 3, "keys are matched case-insensitively");
            expectInt("fix.mystery", 9,
                      "an unknown key still reads (it is named in the log)");
        }
        static const char kBothIni[] =
            "[fix]\r\n"
            "beta = 7\r\n"
            "[experimental]\r\n"
            "beta = 5\r\n";
        if (!writeIni(scratch, kBothIni)) {
            fail("audit both-set ini", "could not write it");
        } else {
            Config::get().init(scratch);
            expectInt("experimental.beta", 5,
                      "when old and new are both set, the new name wins");
        }
        static const char kStaleIni[] =
            "[fix]\r\n"
            "gamma = stock\r\n"      // the OLD key's shipped default: stale
            "beta = screen\r\n";     // a real choice on a move with no default
        if (!writeIni(scratch, kStaleIni)) {
            fail("audit stale-default ini", "could not write it");
        } else {
            Config::get().init(scratch);
            expectStr("experimental.gamma", "<unset>",
                      "an old line carrying its retired default does not "
                      "follow the move; the caller's own default rules");
            expectStr("experimental.beta", "screen",
                      "a real old-line choice still follows the move");
        }
        Config::get().setAuditTables(nullptr, 0, nullptr, 0);
    }

    if (g_fails) {
        printf("CONFIG TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("CONFIG TEST PASSED\n");
    return 0;
}
