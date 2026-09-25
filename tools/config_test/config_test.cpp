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
#include <cwctype>
#include <string>

#include "../../src/common/config.h"
#include "../../src/common/runtime_profile.h"
#include "../../src/common/temporal_mode.h"
#include "../../src/common/log.h"

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
    expectStr("fix.temporal_aa_model", "k", "DLSS preset ships as K on the Performance page");
    expectStr("experimental.supersample_resolve", "<unset>",
              "the retired supersample_resolve key is still absent");
    expectStr("experimental.supersample_filter", "<unset>",
              "the retired supersample_filter key is still absent");
    expectBool("fix.night_vision_stability", true, "night vision pulse stability remains a standard enabled fix");
    expectBool("experimental.night_vision_realistic", false, "Realistic nightvision ships disabled under Experimental");
    expectFloat("experimental.night_vision_brightness", 8.0f, "night vision appearance brightness lives under Experimental");
    expectStr("fix.ui_depth", "<unset>", "UI depth is bundled with temporal AA");
    expectStr("fix.temporal_aa_objects", "<unset>", "station motion is bundled with temporal AA");
    expectStr("fix.temporal_aa_smoke", "<unset>", "smoke depth is bundled with temporal AA");
    expectStr("fix.engine_motion", "<unset>", "engine motion is bundled with temporal AA: its own key is retired");
    // The estimation paths the engine records superseded (2026-09-23): their
    // keys are retired, not merely off.
    expectStr("advanced.temporal_aa_estimated_objects", "<unset>", "the retired estimated station/ship paths' key is absent");
    expectStr("advanced.mesh_motion", "<unset>", "the retired mesh record pairing's key is absent");
    expectStr("advanced.temporal_aa_objects_reach", "<unset>", "the retired station path's reach is absent");
    expectStr("advanced.temporal_aa_objects_ships_metres", "<unset>", "the retired ship path's range is absent");
    // The particle facing measurement (dead since 2026-08-23) retired 2026-09-23.
    expectStr("advanced.particle_face_emitter", "<unset>", "the retired particle facing key is absent");
    // The foveation's eye-tracked centre went with its gaze source (2026-09-23);
    // its key retired 2026-09-24, and advanced.foveation_distance defaults to 0.
    expectStr("experimental.foveation_centre", "<unset>", "the retired foveation centre key is absent");
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
    // The eye-run depth instrument ships as a commented template like the
    // other developer instruments: the compiled default is what a user gets.
    if (Config::get().getBool("advanced.eye_depth_capture", false) == false &&
        Config::get().getBool("advanced.eye_depth_capture", true) == true)
        ok("eye depth capture is documented but not live under [advanced]");
    else
        fail("eye depth capture is documented but not live under [advanced]",
             "the shipped file defines it live");
    // The settlement LOD governor is a shipped fix: fix.settlement_detail is
    // live under the first [fix] and reads game (off), the compiled default too.
    // Its two [advanced] tuning keys ship as commented templates, so the
    // compiled defaults -- a ceiling of 6.0 (held to 1..8) and observe 0 --
    // are what every user runs until they choose otherwise.
    expectStr("fix.settlement_detail", "game",
              "settlement detail ships live in [fix] and reads game (off)");
    expectStr("advanced.settlement_detail_max", "<unset>",
              "...its ceiling ships commented out: the compiled 6.0 is in force");
    expectStr("advanced.settlement_detail_observe", "<unset>",
              "...and so does its observe-only switch");
    // ui_quality (docs/ui-layer-2026-09-23.md) is one key for both halves:
    // the interface panels made at the target's size, and the game's
    // post-tonemap UI drawn into a per-eye layer after the upscale. Values
    // off | 100 | 125 (percent of HMD Quality 1.0; ui_layer_math.h parses
    // them, and the first spellings 1.0 / 1.25, for one release). Ships live
    // in [fix], default off (the interface is drawn as today).
    // fix.hud_quality, the surfaces' own key for one day, is gone: absorbed.
    expectStr("fix.ui_quality", "off",
              "ui quality ships live in [fix] and defaults off");
    expectStr("fix.hud_quality", "<unset>",
              "...and the separate HUD key it absorbed is gone");

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

        // --- the log directory, and the environment's say over it ----------
        //
        // build.bat's test runner gives each rig's proxies their own log
        // directory through EDVR_LOG_DIR, scoped by EDVR_LOG_DIR_FOR to the
        // exes in build\, so two rigs' proxies never share crash sentinels.
        // The scope matters: rigs that stage children in private directories
        // read those children's exe-relative logs, and the runner's variables
        // reach the children too. This process runs under those variables
        // itself, so each case sets both and the originals are put back.
        {
            const std::wstring exeDir = executableDirectory();
            const std::wstring exeDefault = exeDir + L"\\edvr_logs";
            const std::wstring moved = scratch + L"\\moved_logs";
            wchar_t savedDir[MAX_PATH]{}, savedFor[MAX_PATH]{};
            const bool hadDir = GetEnvironmentVariableW(L"EDVR_LOG_DIR", savedDir, MAX_PATH) != 0;
            const bool hadFor = GetEnvironmentVariableW(L"EDVR_LOG_DIR_FOR", savedFor, MAX_PATH) != 0;
            auto expectLogDir = [&](const wchar_t* dir, const wchar_t* onlyFor,
                                    const std::wstring& want, const char* what) {
                SetEnvironmentVariableW(L"EDVR_LOG_DIR", dir);
                SetEnvironmentVariableW(L"EDVR_LOG_DIR_FOR", onlyFor);
                Config::get().init(scratch);
                const std::wstring& got = Config::get().logDir();
                if (_wcsicmp(got.c_str(), want.c_str()) == 0) { ok(what); return; }
                std::string detail = "log dir is ";
                for (wchar_t c : got) detail.push_back(static_cast<char>(c));
                fail(what, detail);
            };
            std::wstring upperExeDir = exeDir;
            for (wchar_t& c : upperExeDir) c = static_cast<wchar_t>(towupper(c));
            if (!writeIni(scratch, "[fix]\r\nblack_void = 1\r\n")) {
                fail("log dir scratch ini", "could not write it");
            } else {
                expectLogDir(nullptr, nullptr, exeDefault, "without log.dir the log goes beside the exe");
                expectLogDir(moved.c_str(), nullptr, moved, "EDVR_LOG_DIR alone moves the default anywhere");
                expectLogDir(moved.c_str(), scratch.c_str(), exeDefault,
                             "EDVR_LOG_DIR_FOR naming another directory leaves this exe's default alone");
                expectLogDir(moved.c_str(), upperExeDir.c_str(), moved,
                             "EDVR_LOG_DIR_FOR naming the exe's directory, any case, moves it");
                expectLogDir(L"", upperExeDir.c_str(), exeDefault, "an empty EDVR_LOG_DIR moves nothing");
            }
            if (!writeIni(scratch, "[log]\r\ndir = C:\\elsewhere\\logs\r\n")) {
                fail("log.dir scratch ini", "could not write it");
            } else {
                expectLogDir(moved.c_str(), nullptr, L"C:\\elsewhere\\logs",
                             "an explicit log.dir wins over the environment");
            }
            SetEnvironmentVariableW(L"EDVR_LOG_DIR", hadDir ? savedDir : nullptr);
            SetEnvironmentVariableW(L"EDVR_LOG_DIR_FOR", hadFor ? savedFor : nullptr);
        }
    }

    // --- the log's buffer, and that a lost line SAYS it was lost -----------
    //
    // WHY THIS IS HERE (2026-09-07). Log::append drops a line when more text
    // queues between two flusher passes than the buffer holds, counts it in
    // m_dropped -- and until this date NOTHING EVER PRINTED THAT COUNT. Six
    // draw censuses across two flights were read as complete when every one
    // had lost its tail at the old 1 MB cap, taking the intern table that
    // says what each @N refers to. The diagnosis went to draw_census.cpp
    // twice before it came to the logger.
    //
    // So the property under test is not "the buffer is big enough" -- that is
    // a number in edvr.ini -- but "a log that lost lines is not silent about
    // it". Driven at buffer_mb = 1, the floor, so the test stays fast.
    if (argc >= 3) {
        const std::wstring scratch = widen(argv[2]);
        static const char kLogIni[] =
            "[log]\r\n"
            "enabled = 1\r\n"
            "buffer_mb = 1\r\n"
            "max_mb = 64\r\n";
        if (!writeIni(scratch, kLogIni)) {
            fail("log scratch ini", "could not write it");
        } else {
            Config::get().init(scratch);
            Log::get().close();          // in case anything above opened one

            // Clear previous runs first. Each leaves about a megabyte, so
            // without this every build grows the scratch directory forever --
            // and it also makes "the newest match" below provably THIS run's
            // rather than whichever name sorted last.
            {
                WIN32_FIND_DATAW old{};
                HANDLE oh = FindFirstFileW((scratch + L"\\edvr_buftest_*.log").c_str(),
                                           &old);
                if (oh != INVALID_HANDLE_VALUE) {
                    do {
                        DeleteFileW((scratch + L"\\" + old.cFileName).c_str());
                    } while (FindNextFileW(oh, &old));
                    FindClose(oh);
                }
            }

            if (!Log::get().open(scratch, L"buftest")) {
                fail("log buffer", "the log would not open in the scratch dir");
            } else {
                // Well past 1 MB, and fast: the flusher sleeps 250 ms before
                // its first pass, so this whole burst lands in one buffer --
                // which is exactly the shape of a draw census.
                std::string filler(800, 'x');
                for (int i = 0; i < 4000; ++i) {
                    Log::get().note("burst %d %s", i, filler.c_str());
                }
                const uint64_t lost = Log::get().dropped();
                Log::get().close();      // joins the flusher, flushes both

                if (lost == 0) {
                    fail("log buffer", "4000 x ~800 bytes did not overrun a "
                                       "1 MB buffer; the cap is not being read");
                } else {
                    ok("a burst past the buffer drops lines and counts them");
                }

                // And the count reaches the FILE, which is the half that was
                // missing: a reader of the log must see the gap.
                WIN32_FIND_DATAW fd{};
                const std::wstring pat = scratch + L"\\edvr_buftest_*.log";
                HANDLE h = FindFirstFileW(pat.c_str(), &fd);
                std::string body;
                if (h != INVALID_HANDLE_VALUE) {
                    std::wstring newest = fd.cFileName;
                    while (FindNextFileW(h, &fd)) newest = fd.cFileName;
                    FindClose(h);
                    HANDLE f = CreateFileW((scratch + L"\\" + newest).c_str(),
                                           GENERIC_READ, FILE_SHARE_READ, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                           nullptr);
                    if (f != INVALID_HANDLE_VALUE) {
                        char chunk[65536];
                        DWORD got = 0;
                        while (ReadFile(f, chunk, sizeof(chunk), &got, nullptr) &&
                               got) {
                            body.append(chunk, got);
                        }
                        CloseHandle(f);
                    }
                }
                if (body.empty()) {
                    fail("log buffer", "could not read the log back");
                } else if (body.find("were DROPPED here") == std::string::npos) {
                    fail("log buffer",
                         "lines were dropped and the log does not say so -- "
                         "the exact silence this test exists to prevent");
                } else {
                    ok("the log says in its own text that lines were dropped");
                }
            }
        }
    }

    // An old/full INI cannot widen a flat installation, even through numeric
    // getters whose ordinary fallback or lower bound would turn a fix on.
    const RuntimeProfile savedProfile = g_runtimeProfile;
    const char* descriptor = "[install]\r\nschema = 1\r\nprofile = flat\r\n";
    if (parseRuntimeProfile(descriptor) != RuntimeProfile::Flat ||
        parseRuntimeProfile("[install]\nschema=1\nprofile=vr\n") != RuntimeProfile::Vr ||
        parseRuntimeProfile("[install]\nschema=2\nprofile=flat\n") != RuntimeProfile::Invalid ||
        parseRuntimeProfile("[install]\nschema=1\nprofile=flat\nprofile=vr\n") != RuntimeProfile::Invalid ||
        parseRuntimeProfile("[install]\nprofile=flat\n") != RuntimeProfile::Invalid ||
        parseRuntimeProfile(std::string(4097, 'x')) != RuntimeProfile::Invalid)
        fail("profile descriptor", "invalid schema/duplicate/oversize admitted");
    else ok("profile descriptor refuses ambiguous or unsupported scope");
    g_runtimeProfile = RuntimeProfile::Flat;
    // The flat adapter reads this through getString with an on default. A
    // refused key turns that default into off, so exercise the actual getter.
    if (argc >= 3) {
        const std::wstring scratch = widen(argv[2]);
        if (!writeIni(scratch, "[fix]\r\nblack_void = on\r\n"))
            fail("flat jitter scratch ini", "could not write it");
        else {
            Config::get().init(scratch);
            g_runtimeProfile = RuntimeProfile::Flat;
            if (Config::get().getString("experimental.temporal_aa_jitter", "on") == "on")
                ok("flat jitter uses on default when absent");
            else fail("flat jitter default", "missing key was not on");
        }
    }
    Config::get().set("experimental.temporal_aa_jitter", "on");
    if (Config::get().getString("experimental.temporal_aa_jitter", "off") == "on")
        ok("flat jitter reads explicit on");
    else fail("flat jitter enabled", "explicit on was suppressed");
    Config::get().set("experimental.temporal_aa_jitter", "off");
    if (Config::get().getString("experimental.temporal_aa_jitter", "on") == "off")
        ok("flat jitter preserves explicit off");
    else fail("flat jitter override", "explicit off was not read");
    Config::get().set("fix.temporal_aa", "dlss");
    Config::get().set("fix.black_void", "on");
    Config::get().set("fix.head_offset_forward", "12");
    Config::get().set("experimental.night_vision_realistic", "on");
    Config::get().set("advanced.real_dll", "d3d11_edhm.dll");
    expectStr("fix.temporal_aa", "off", "flat discovery cannot activate stereo temporal AA");
    if (Config::get().requestedTemporalMode() != "dlss") fail("flat request", "intent lost");
    else ok("flat diagnostic retains requested temporal mode");
    expectBool("fix.black_void", false, "flat profile suppresses restored unrelated fix");
    expectBool("experimental.night_vision_realistic", false,
               "flat jitter exception leaves unrelated experimental settings suppressed");
    expectInt("fix.head_offset_forward", 0, "flat profile suppresses numeric fix");
    expectFloat("fix.head_offset_forward", 0.0f, "flat profile suppresses float fix");
    if (Config::get().getIntInRange("fix.head_offset_forward", 12, 1, 100) != 0)
        fail("flat bounded getter", "minimum reactivated disabled feature");
    else ok("flat scope precedes bounded numeric defaults");
    expectStr("advanced.real_dll", "d3d11_edhm.dll", "flat scope preserves mod chaining");
    Config::get().set("hotkey.menu", "F8");
    expectStr("hotkey.menu", "F8", "flat scope permits the temporal menu hotkey");
    Config::get().set("fix.temporal_aa_model", "m");
    expectStr("fix.temporal_aa_model", "m", "flat scope permits the DLSS model");
    expectStr("hotkey.toggle_exposure", "", "flat menu exception leaves unrelated hotkeys suppressed");
    const struct { const char* name; unsigned full, fovea; bool known; } presets[] = {
        {"auto",0,0,true},{"default",0,0,true},{"j",10,10,true},
        {"K",11,11,true},{"l",12,12,true},{"m",13,13,true},
        {"quality",11,11,true},{"steady",11,12,true},{"responsive",10,10,true},
        {"removed-preset",11,11,false}
    };
    for (const auto& p : presets) {
        const auto actual=temporalPresetFor(p.name);
        if (actual.full!=p.full || actual.fovea!=p.fovea || actual.known!=p.known)
            fail("shared temporal preset mapping",p.name);
    }
    Config::get().set("fix.black_void", "on");
    expectBool("fix.black_void", false, "live setting change cannot widen scope");
    g_runtimeProfile = RuntimeProfile::Invalid;
    expectBool("advanced.d3d11_fixes", false, "bad descriptor disables graphics hooks");
    expectStr("hotkey.menu", "", "invalid profile cannot open temporal menu");
    expectStr("fix.temporal_aa_model", "off", "invalid profile cannot select a DLSS model");
    expectStr("advanced.real_dll", "d3d11_edhm.dll", "bad descriptor preserves mod chaining");
    if (Config::get().getString("experimental.temporal_aa_jitter", "on") == "off")
        ok("invalid profile suppresses flat jitter");
    else fail("invalid profile jitter", "flat key widened invalid scope");
    g_runtimeProfile = RuntimeProfile::LegacyVr;
    expectBool("fix.black_void", true, "legacy profile retains original behavior");
    if (Config::get().getString("experimental.temporal_aa_jitter", "on") == "off")
        ok("legacy profile retains explicit jitter setting");
    else fail("legacy profile jitter", "flat exception changed VR read");
    g_runtimeProfile = RuntimeProfile::Vr;
    Config::get().set("experimental.temporal_aa_jitter", "on");
    if (Config::get().getString("experimental.temporal_aa_jitter", "off") == "on")
        ok("VR profile reads explicit jitter setting");
    else fail("VR profile jitter", "flat exception changed VR scope");
    g_runtimeProfile = savedProfile;

    if (g_fails) {
        printf("CONFIG TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("CONFIG TEST PASSED\n");
    return 0;
}
