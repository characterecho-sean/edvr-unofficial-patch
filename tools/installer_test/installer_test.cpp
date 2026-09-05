// installer_test -- the installer's decisions, over folders nobody can arrange
// on demand.
//
// Every interesting case this installer exists for is a state of somebody
// else's machine: EDHM already holding the d3d11.dll name, another mod's
// installer having overwritten ours, a game update that put the stock
// openvr_api.dll back over ours, an original OpenVR runtime lost to a double
// rename, an edvr.ini full of values tuned in a headset that an update must not
// throw away. None of them can be produced to order on the machine doing the
// build, and every one of them is a folder somebody would have to repair by
// hand if the installer got it wrong.
//
// So the planner is a pure function of a Survey, and this builds the Surveys.
// The apply engine is exercised for real, in a scratch folder, including the
// case that matters most: a failure AFTER the game's openvr_api.dll has been
// renamed out of the way must put it back, because the alternative is a folder
// with no openvr_api.dll at all.
//
// Usage: installer_test.exe <repo root> <scratch dir>
#include <windows.h>

#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

#include "../../src/installer/apply.h"
#include "../../src/installer/plan.h"
#include "../../src/installer/probe.h"
#include "../../src/installer/logbundle.h"
#include "../../src/installer/mirror.h"
#include "../../src/installer/settings.h"

using namespace edvr::installer;

static int g_fails = 0;

static void ok(const char* what) { printf("  ok    %s\n", what); }

static void fail(const char* what, const std::string& detail) {
    printf("  FAIL  %s -- %s\n", what, detail.c_str());
    ++g_fails;
}

static void check(bool condition, const char* what, const std::string& detail = std::string()) {
    if (condition)
        ok(what);
    else
        fail(what, detail.empty() ? "condition was false" : detail);
}

static void expectEq(const std::string& got, const std::string& want, const char* what) {
    if (got == want)
        ok(what);
    else
        fail(what, "got \"" + got + "\", wanted \"" + want + "\"");
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static std::string readAll(const std::wstring& path) { return readTextFile(path); }

static bool writeAll(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL good = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(f);
    return good != FALSE;
}

static void makeTree(const std::wstring& path) {
    if (path.empty() || dirExists(path)) return;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) makeTree(path.substr(0, slash));
    CreateDirectoryW(path.c_str(), nullptr);
}

static void removeTree(const std::wstring& path) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(joinPath(path, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring child = joinPath(path, name);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                removeTree(child);
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(path.c_str());
}

static bool hasStep(const Plan& plan, Action action, const wchar_t* leafFrom,
                    const wchar_t* leafTo) {
    for (const Step& s : plan.steps) {
        if (s.action != action) continue;
        if (leafFrom && _wcsicmp(leafOf(s.from).c_str(), leafFrom) != 0) continue;
        if (leafTo && _wcsicmp(leafOf(s.to).c_str(), leafTo) != 0) continue;
        return true;
    }
    return false;
}

static bool notesMention(const Plan& plan, const char* needle) {
    for (const std::string& n : plan.notes) {
        if (n.find(needle) != std::string::npos) return true;
    }
    for (const std::string& p : plan.problems) {
        if (p.find(needle) != std::string::npos) return true;
    }
    return false;
}

// The edvr.ini text a plan would write, pulled back out of its steps.
static std::string plannedIni(const Plan& plan) {
    for (const Step& s : plan.steps) {
        if (s.action == Action::WriteText && _wcsicmp(leafOf(s.to).c_str(), L"edvr.ini") == 0)
            return s.text;
    }
    return std::string();
}

static DllInfo fakeDll(DllKind kind, const std::wstring& path, const std::string& sha,
                       const wchar_t* product = nullptr) {
    DllInfo d;
    d.kind = kind;
    d.path = path;
    d.sha256 = sha;
    d.is64 = true;
    d.size = 1024 * 1024;
    if (product) d.product = product;
    return d;
}

static PayloadInfo testPayload(const std::string& iniText) {
    PayloadInfo p;
    p.version = "v9.9.9-test";
    p.haveD3d11 = true;
    p.d3d11Sha = "aaaa-new-d3d11";
    p.haveOpenvr = true;
    p.openvrSha = "bbbb-new-openvr";
    p.iniText = iniText;
    return p;
}

static Survey baseSurvey(const std::wstring& gameDir) {
    Survey s;
    s.game.dir = gameDir;
    s.game.openvrDir = joinPath(gameDir, L"Openvr\\win64");
    s.game.source = L"Test";
    s.game.product = L"elite-dangerous-odyssey-64";
    s.game.odyssey = true;
    s.haveOpenvrDir = true;
    s.gameRunningHere = false;
    s.d3d11 = fakeDll(DllKind::Absent, joinPath(gameDir, L"d3d11.dll"), "");
    s.openvrCurrent =
        fakeDll(DllKind::OpenVrRuntime, joinPath(s.game.openvrDir, L"openvr_api.dll"), "game-vr");
    s.openvrOrig =
        fakeDll(DllKind::Absent, joinPath(s.game.openvrDir, L"openvr_api_orig.dll"), "");
    return s;
}

static Options testOptions() {
    Options o;
    o.backupStamp = L"20260827-120000";
    o.nowUtc = "2026-08-27T12:00:00Z";
    return o;
}

// ---------------------------------------------------------------------------
// the shipped ini, in miniature: enough shape to exercise every merge rule
// ---------------------------------------------------------------------------

static const char* kBaseIni =
    "# EDVR settings.\r\n"
    "\r\n"
    "[fix]\r\n"
    "share_exposure = 1\r\n"
    "exposure_damping = 0\r\n"
    "black_void = 1\r\n"
    "# An expert setting, shown at its default.\r\n"
    "#fss_res = 0\r\n"
    "retired_thing = 3\r\n"
    "\r\n"
    "[advanced]\r\n"
    "real_dll =\r\n"
    "exposure_shader =            ; empty means find it\r\n";

static const char* kNextIni =
    "# EDVR settings.\r\n"
    "\r\n"
    "[fix]\r\n"
    "share_exposure = 1\r\n"
    "exposure_damping = 0\r\n"
    "black_void = 0\r\n"       // the default MOVED in this version
    "# An expert setting, shown at its default.\r\n"
    "#fss_res = 0\r\n"
    "sun_glare = vivid\r\n"    // and a new setting arrived
    "\r\n"
    "[advanced]\r\n"
    "real_dll =\r\n"
    "exposure_shader =            ; empty means find it\r\n";

static void testMerge() {
    printf("\nedvr.ini merge\n");

    const std::string base = kBaseIni;
    const std::string next = kNextIni;

    // A user who has been in the headset: one value changed, one expert setting
    // enabled, one line deleted outright, one setting from a support thread
    // that this build has never heard of.
    std::string user = kBaseIni;
    user.replace(user.find("exposure_damping = 0"), strlen("exposure_damping = 0"),
                 "exposure_damping = 0.7");
    user.replace(user.find("#fss_res = 0"), strlen("#fss_res = 0"), "fss_res = 1");
    user.replace(user.find("share_exposure = 1\r\n"), strlen("share_exposure = 1\r\n"), "");
    user += "hand_added = 42\r\n";

    MergeReport report;
    const std::string merged = mergeIni(next, user, &base, {}, &report);

    expectEq(iniValue(merged, "fix.exposure_damping"), "0.7", "a changed value is kept");
    expectEq(iniValue(merged, "fix.fss_res"), "1", "an expert setting the user enabled stays on");
    expectEq(iniValue(merged, "fix.black_void"), "0",
             "a default that moved is adopted where the user had not touched it");
    expectEq(iniValue(merged, "fix.sun_glare"), "vivid", "a new setting arrives at its default");
    expectEq(iniValue(merged, "fix.share_exposure", "<absent>"), "<absent>",
             "a line the user deleted stays deleted");
    expectEq(iniValue(merged, "advanced.real_dll", "<absent>"), "",
             "a key shipped with an empty value is present and empty, not absent");
    expectEq(iniValue(merged, "fix.retired_thing"), "3",
             "a value for a setting this version dropped is carried, not discarded");
    // It went in at the end of the file, so it belongs to [advanced].
    expectEq(iniValue(merged, "advanced.hand_added"), "42", "a hand-added key is carried");
    check(!report.kept.empty(), "the report names what was kept");
    check(report.retired.size() == 1, "the retired setting is reported as retired");
    check(report.removed.size() == 1, "the deleted line is reported");
    check(!report.twoWay, "with a base copy this is a three-way merge");
    check(merged.find("# An expert setting, shown at its default.") != std::string::npos,
          "the new file's explanations survive");
    check(merged.find("\r\n") != std::string::npos && merged.find("\n\n") == std::string::npos,
          "line endings stay CRLF");

    // No edvr.ini at all -- a first install. There are no opinions to preserve,
    // and inferring "deleted" from "absent" here once commented out the entire
    // shipped file: a settings file with nothing set, which the game reads
    // happily and which looks almost right in an editor.
    MergeReport fresh;
    expectEq(mergeIni(next, std::string(), nullptr, {}, &fresh), next,
             "with no existing ini the shipped file is written as it ships");
    check(fresh.removed.empty(), "and nothing is reported as removed");

    // A hand-installed rig whose ini predates several settings. Those must
    // arrive live, not commented out -- there is no base to prove they were
    // ever there to delete.
    MergeReport older;
    const std::string oldUser =
        "[fix]\r\n"
        "share_exposure = 1\r\n"
        "exposure_damping = 0.5\r\n";
    const std::string caughtUp = mergeIni(next, oldUser, nullptr, {}, &older);
    expectEq(iniValue(caughtUp, "fix.sun_glare"), "vivid",
             "a setting their old file never had arrives at its default");
    expectEq(iniValue(caughtUp, "fix.exposure_damping"), "0.5", "and their value is still kept");


    // A setting that moved section. The section is part of the key, so a move
    // renames it -- and a tuned value would be stranded under a name nothing
    // reads. The new file says where it came from, and the value follows.
    MergeReport moveReport;
    const std::string movedNext =
        "[fix]\r\n"
        "share_exposure = 1\r\n"
        "\r\n"
        "[experimental]\r\n"
        "# moved-from: fix.exposure_damping\r\n"
        "#exposure_damping = 0\r\n";
    const std::string movedUser =
        "[fix]\r\n"
        "share_exposure = 1\r\n"
        "exposure_damping = 0.7\r\n";
    const std::string movedOut = mergeIni(movedNext, movedUser, nullptr, {}, &moveReport);
    expectEq(iniValue(movedOut, "experimental.exposure_damping"), "0.7",
             "a tuned value follows its setting to a new section");
    expectEq(iniValue(movedOut, "fix.exposure_damping", "<absent>"), "<absent>",
             "and is not left behind under the old name");
    check(moveReport.followed.size() == 1, "the move is reported");
    check(moveReport.retired.empty(), "and not also reported as retired");

    // Somebody who already set the NEW key wins over the old one.
    MergeReport bothReport;
    const std::string bothUser =
        "[fix]\r\n"
        "exposure_damping = 0.7\r\n"
        "\r\n"
        "[experimental]\r\n"
        "exposure_damping = 0.4\r\n";
    const std::string bothOut = mergeIni(movedNext, bothUser, nullptr, {}, &bothReport);
    expectEq(iniValue(bothOut, "experimental.exposure_damping"), "0.4",
             "a value already under the new name is not overwritten by the old one");


    // A comment can never unset a live value. config.cpp skips any line
    // starting with # before it looks for a key, so this file reads
    // black_void = 0 -- and a merge that let the commented copy below it win
    // decided the user had DELETED the setting, commented the live line out,
    // and handed them the compiled default instead. Their value, reversed.
    MergeReport shadowReport;
    const std::string shadowNext =
        "[fix]\r\n"
        "black_void = 1\r\n";
    const std::string shadowUser =
        "[fix]\r\n"
        "black_void = 0\r\n"
        "#black_void = 1\r\n";
    const std::string shadowOut = mergeIni(shadowNext, shadowUser, &shadowNext, {}, &shadowReport);
    expectEq(iniValue(shadowOut, "fix.black_void"), "0",
             "a commented copy below a live line does not unset it");
    check(shadowReport.removed.empty(), "and is not reported as a deletion");

    // A key config.cpp would read must survive the merge even if it is not
    // spelled the way this file's own keys are. Dropping it lost a setting the
    // game was using, with nothing in the report to say so.
    MergeReport oddReport;
    const std::string oddUser =
        "[fix]\r\n"
        "black_void = 1\r\n"
        "my+key = 7\r\n";
    const std::string oddOut = mergeIni(shadowNext, oddUser, &shadowNext, {}, &oddReport);
    expectEq(iniValue(oddOut, "fix.my+key"), "7", "an unusual key is carried, not dropped");
    check(!oddReport.carried.empty(), "and the report says it was carried");

    // Prose is still prose: the test above must not have made every comment
    // containing '=' into a setting.
    MergeReport proseGuard;
    const std::string proseNext2 =
        "[fix]\r\n"
        "# 0.3, paired with panel_distance = 0.7, is a comfortable pairing\r\n"
        "panel_curvature = 0\r\n";
    expectEq(mergeIni(proseNext2, proseNext2, &proseNext2, {}, &proseGuard), proseNext2,
             "a comment containing = is still left as prose");

    // The identity that makes the whole thing trustworthy: merging a file with
    // itself changes nothing at all.
    MergeReport quiet;
    expectEq(mergeIni(next, next, &next, {}, &quiet), next, "merging a file with itself is a no-op");

    // No base copy: a hand-installed rig meeting this installer for the first
    // time. Anything differing from the new defaults is treated as the user's.
    MergeReport twoWay;
    const std::string mergedTwo = mergeIni(next, user, nullptr, {}, &twoWay);
    check(twoWay.twoWay, "without a base copy the report says so");
    expectEq(iniValue(mergedTwo, "fix.exposure_damping"), "0.7", "two-way keeps a changed value");
    expectEq(iniValue(mergedTwo, "fix.black_void"), "1",
             "two-way keeps the OLD default, because it cannot tell it from an edit");

    // A forced value beats whatever the user had -- this is how chaining is
    // written, and it must not be reported as one of their settings.
    MergeReport forcedReport;
    std::string userWithChain = base;
    userWithChain.replace(userWithChain.find("real_dll ="), strlen("real_dll ="),
                          "real_dll = stale.dll");
    const std::string chained =
        mergeIni(next, userWithChain, &base, {{"advanced.real_dll", "d3d11_edhm.dll"}},
                 &forcedReport);
    expectEq(iniValue(chained, "advanced.real_dll"), "d3d11_edhm.dll",
             "the installer's chain setting wins");
    check(forcedReport.forced.size() == 1, "the forced value is reported as the installer's");

    // The inline comment on exposure_shader is part of the shipped file's
    // documentation; rewriting a neighbouring value must not eat it.
    check(chained.find("; empty means find it") != std::string::npos,
          "inline comments are preserved");

    // Notepad writes a BOM. The game's reader skips it; so must this, or every
    // setting in the file is filed under the wrong section.
    MergeReport bomReport;
    const std::string bomUser = std::string("\xEF\xBB\xBF") + user;
    const std::string mergedBom = mergeIni(next, bomUser, &base, {}, &bomReport);
    expectEq(iniValue(mergedBom, "fix.exposure_damping"), "0.7",
             "a BOM at the front of the user's file changes nothing");

    // Prose that happens to contain '=' is not a setting.
    MergeReport proseReport;
    const std::string proseNext =
        "[fix]\r\n"
        "# 0.3, paired with panel_distance = 0.7, is a comfortable pairing\r\n"
        "panel_curvature = 0\r\n";
    const std::string proseMerged = mergeIni(proseNext, proseNext, &proseNext, {}, &proseReport);
    expectEq(proseMerged, proseNext, "a comment line containing = is left as prose");
}

static void testShippedIni(const std::wstring& root) {
    printf("\nthe shipped edvr.ini\n");
    const std::string shipped = readAll(joinPath(root, L"edvr.ini"));
    if (shipped.empty()) {
        fail("read the repository's edvr.ini", "not found next to the repo root");
        return;
    }
    MergeReport report;
    expectEq(mergeIni(shipped, shipped, &shipped, {}, &report), shipped,
             "the real edvr.ini merges with itself unchanged");

    // A user file made from the real one, tuned the way somebody in a headset
    // tunes it: the on-foot screen pulled closer, which is the pairing the
    // README documents with a 0.3 curve.
    std::string user = shipped;
    const size_t distance = user.find("panel_distance = 1.0");
    check(distance != std::string::npos, "the shipped ini still has the setting this tunes");
    if (distance != std::string::npos)
        user.replace(distance, strlen("panel_distance = 1.0"), "panel_distance = 0.7");

    MergeReport real;
    const std::string merged =
        mergeIni(shipped, user, &shipped, {{"advanced.real_dll", "d3d11_edhm.dll"}}, &real);
    expectEq(iniValue(merged, "fix.panel_distance"), "0.7",
             "a real tuned value survives a real merge");
    expectEq(iniValue(merged, "advanced.real_dll"), "d3d11_edhm.dll",
             "chaining is written into the real file");
    check(merged.size() > shipped.size() - 64,
          "the merged file is still the whole documented ini, not a stripped one");

    // The 2026-08-27 field scenario, against the REAL shipped file: a rig
    // whose ini predates the [fix] -> [experimental] move, with the two
    // values whose loss actually regressed in a headset -- the scanner
    // panel's stock-distance pin and the full-resolution body. Hand-installed
    // (no base copy), which is exactly the rig the move strands.
    const std::string oldLayout =
        "[fix]\r\n"
        "fss_panel_distance = 1.0\r\n"
        "fss_res = 1\r\n"
        "fss_eye_heal = 1\r\n"        // the pre-0.11 pair, both of which
        "fss_reveal_sync = on\r\n"    // now merge into ONE new key
        "fss_eye_glue = stock\r\n"   // a key that never existed: carried, not eaten
        "[advanced]\r\n"
        "cull_guard_percent = 20.0\r\n";
    MergeReport moveRep;
    const std::string migrated = mergeIni(shipped, oldLayout, nullptr, {}, &moveRep);
    expectEq(iniValue(migrated, "experimental.fss_panel_distance"), "1.0",
             "the scanner panel's stock pin follows the section move");
    expectEq(iniValue(migrated, "experimental.fss_res"), "1",
             "the full-res body follows the section move");
    expectEq(iniValue(migrated, "fix.fss_panel_distance"), "",
             "nothing is left under the old name for the reader to shadow");
    expectEq(iniValue(migrated, "fix.fss_eye_sync"), "on",
             "the retired pair of keys lands merged on the one new key");
    expectEq(iniValue(migrated, "advanced.cull_guard_percent"), "20.0",
             "an unmoved tuned value still lands in its own section");
    check(iniValue(migrated, "fix.fss_eye_glue") == "stock",
          "a key this version never shipped is carried, with its note");
    check(moveRep.followed.size() >= 4,
          "the report says the values followed their settings");
}

// ---------------------------------------------------------------------------
// the planner
// ---------------------------------------------------------------------------

static void testPlanner() {
    printf("\nthe planner\n");
    const std::wstring dir = L"C:\\Games\\ED\\Products\\elite-dangerous-odyssey-64";
    const PayloadInfo payload = testPayload(kNextIni);
    const Options options = testOptions();

    {   // A clean machine: nothing installed, the game's own runtime in place.
        const Survey s = baseSurvey(dir);
        const Plan plan = planInstall(s, options, payload);
        check(!plan.blocked, "a fresh install is not blocked");
        check(hasStep(plan, Action::WritePayload, nullptr, L"d3d11.dll"), "it writes d3d11.dll");
        check(hasStep(plan, Action::Rename, L"openvr_api.dll", L"openvr_api_orig.dll"),
              "it renames the game's runtime rather than overwriting it");
        check(hasStep(plan, Action::WritePayload, nullptr, L"openvr_api.dll"),
              "it writes our openvr_api.dll");
        check(hasStep(plan, Action::Backup, L"openvr_api.dll", L"openvr_api.dll"),
              "it backs the game's runtime up first");
        expectEq(iniValue(plannedIni(plan), "advanced.real_dll"), "",
                 "with no other mod there is nothing to chain to");
        check(hasStep(plan, Action::WriteText, nullptr, L"state.ini"), "it records what it did");
        check(hasStep(plan, Action::WriteText, nullptr, L"edvr.ini.base"),
              "it keeps this version's default ini for the next merge");
    }

    {   // NVIDIA's DLSS runtime: placed only where an NVIDIA card is, and only
        // when the slot is empty or holds the copy we placed.
        PayloadInfo withNgx = payload;
        withNgx.haveNgx = true;
        withNgx.ngxSha = "cccc-new-ngx";
        Survey s = baseSurvey(dir);
        s.nvidiaAdapter = true;
        Plan plan = planInstall(s, options, withNgx);
        check(hasStep(plan, Action::WritePayload, nullptr, L"nvngx_dlss.dll"),
              "with an NVIDIA card the DLSS runtime is placed");
        check(plan.nextState.ngxInstalled && plan.nextState.ngxSha == "cccc-new-ngx",
              "and recorded as ours");
        s.nvidiaAdapter = false;
        plan = planInstall(s, options, withNgx);
        check(!hasStep(plan, Action::WritePayload, nullptr, L"nvngx_dlss.dll"),
              "without one it is not");
        check(notesMention(plan, "No NVIDIA"), "and the report says so");
        s.nvidiaAdapter = true;
        s.ngx = fakeDll(DllKind::Foreign, joinPath(dir, L"nvngx_dlss.dll"), "their-ngx");
        plan = planInstall(s, options, withNgx);
        check(!hasStep(plan, Action::WritePayload, nullptr, L"nvngx_dlss.dll"),
              "a copy that is not ours is left alone");
        check(!plan.nextState.ngxInstalled, "and not claimed");
        s.state.present = true;
        s.state.ngxInstalled = true;
        s.state.ngxSha = "their-ngx";
        plan = planInstall(s, options, withNgx);
        check(hasStep(plan, Action::WritePayload, nullptr, L"nvngx_dlss.dll"),
              "an older copy of ours is updated");
        check(hasStep(plan, Action::Backup, L"nvngx_dlss.dll", L"nvngx_dlss.dll"),
              "and backed up first");
        s.ngx = fakeDll(DllKind::Foreign, joinPath(dir, L"nvngx_dlss.dll"), "cccc-new-ngx");
        plan = planInstall(s, options, withNgx);
        check(!hasStep(plan, Action::WritePayload, nullptr, L"nvngx_dlss.dll"),
              "this build's copy is left alone");
        check(plan.nextState.ngxInstalled, "and still recorded");
        s.ngx = fakeDll(DllKind::Foreign, joinPath(dir, L"nvngx_dlss.dll"), "their-ngx");
        const Plan un = planUninstall(s, options);
        check(hasStep(un, Action::Delete, L"nvngx_dlss.dll", nullptr),
              "uninstall removes the copy EDVR placed");
        s.state.ngxSha = "some-other";
        const Plan un2 = planUninstall(s, options);
        check(!hasStep(un2, Action::Delete, L"nvngx_dlss.dll", nullptr),
              "but not one it did not place");
        const Plan none = planInstall(baseSurvey(dir), options, payload);
        check(!hasStep(none, Action::WritePayload, nullptr, L"nvngx_dlss.dll"),
              "an installer without the runtime never writes one");
    }

    {   // EDHM is already installed as d3d11.dll.
        Survey s = baseSurvey(dir);
        s.d3d11 = fakeDll(DllKind::D3d11Provider, joinPath(dir, L"d3d11.dll"), "edhm-sha",
                          L"3Dmigoto");
        const Plan plan = planInstall(s, options, payload);
        check(hasStep(plan, Action::Rename, L"d3d11.dll", L"d3d11_edhm.dll"),
              "EDHM is renamed aside, not overwritten");
        check(hasStep(plan, Action::Backup, L"d3d11.dll", L"d3d11.dll"), "and backed up first");
        expectEq(iniValue(plannedIni(plan), "advanced.real_dll"), "d3d11_edhm.dll",
                 "EDVR is pointed at it, so both mods run");
        check(notesMention(plan, "EDHM"), "the report names the mod it found");
    }

    {   // Another mod's installer has run since ours and taken the name back.
        Survey s = baseSurvey(dir);
        s.d3d11 = fakeDll(DllKind::D3d11Provider, joinPath(dir, L"d3d11.dll"), "reshade-sha",
                          L"ReShade");
        s.state.present = true;
        s.state.d3d11Installed = true;
        s.state.d3d11Sha = "old-edvr-sha";
        const Plan plan = planInstall(s, options, payload);
        check(hasStep(plan, Action::Rename, L"d3d11.dll", L"d3d11_reshade.dll"),
              "the intruder is kept, under its own name");
        check(hasStep(plan, Action::WritePayload, nullptr, L"d3d11.dll"), "and EDVR goes back");
        expectEq(iniValue(plannedIni(plan), "advanced.real_dll"), "d3d11_reshade.dll",
                 "with the chain pointed at it");
        check(notesMention(plan, "replaced EDVR"), "the report says what happened");
    }

    {   // The same mod reinstalled itself over us twice: its older copy is
        // already parked under the name we want to use.
        Survey s = baseSurvey(dir);
        s.d3d11 = fakeDll(DllKind::D3d11Provider, joinPath(dir, L"d3d11.dll"), "edhm-new",
                          L"3Dmigoto");
        s.otherD3d11.push_back(
            fakeDll(DllKind::D3d11Provider, joinPath(dir, L"d3d11_edhm.dll"), "edhm-old",
                    L"3Dmigoto"));
        s.state.present = true;
        s.state.d3d11Installed = true;
        s.state.chainTarget = L"d3d11_edhm.dll";
        s.state.chainMod = L"EDHM";
        const Plan plan = planInstall(s, options, payload);
        check(hasStep(plan, Action::Backup, L"d3d11_edhm.dll", L"d3d11_edhm.dll"),
              "the older parked copy is backed up before being replaced");
        check(hasStep(plan, Action::Rename, L"d3d11.dll", L"d3d11_edhm.dll"),
              "and the newer one takes its place in the chain");
    }

    {   // An update where nothing has changed at all.
        Survey s = baseSurvey(dir);
        s.d3d11 = fakeDll(DllKind::Edvr, joinPath(dir, L"d3d11.dll"), payload.d3d11Sha);
        s.openvrCurrent = fakeDll(DllKind::Edvr, joinPath(s.game.openvrDir, L"openvr_api.dll"),
                                  payload.openvrSha);
        s.openvrOrig = fakeDll(DllKind::OpenVrRuntime,
                               joinPath(s.game.openvrDir, L"openvr_api_orig.dll"), "game-vr");
        s.iniPresent = true;
        s.iniText = kNextIni;
        s.baseIniText = kNextIni;
        s.state.present = true;
        s.state.d3d11Installed = true;
        s.state.d3d11Sha = payload.d3d11Sha;
        const Plan plan = planInstall(s, options, payload);
        check(plan.nothingToDo, "an install that would change nothing says so");
        check(plan.steps.empty(), "and does nothing");
    }

    {   // The same folder, but the user asked for a repair.
        Survey s = baseSurvey(dir);
        s.d3d11 = fakeDll(DllKind::Edvr, joinPath(dir, L"d3d11.dll"), payload.d3d11Sha);
        s.openvrCurrent = fakeDll(DllKind::Edvr, joinPath(s.game.openvrDir, L"openvr_api.dll"),
                                  payload.openvrSha);
        s.openvrOrig = fakeDll(DllKind::OpenVrRuntime,
                               joinPath(s.game.openvrDir, L"openvr_api_orig.dll"), "game-vr");
        s.iniPresent = true;
        s.iniText = kNextIni;
        s.baseIniText = kNextIni;
        Options repair = options;
        repair.repair = true;
        const Plan plan = planInstall(s, repair, payload);
        check(!plan.nothingToDo, "a repair writes even when everything looks right");
        check(hasStep(plan, Action::WritePayload, nullptr, L"d3d11.dll"),
              "a repair rewrites d3d11.dll");
        check(hasStep(plan, Action::WritePayload, nullptr, L"openvr_api.dll"),
              "a repair rewrites openvr_api.dll");
    }

    {   // EDHM's uninstaller ran `del d3d11.dll`, which after our install is
        // OUR file. The chain target is still on disk.
        Survey s = baseSurvey(dir);
        s.d3d11 = fakeDll(DllKind::Absent, joinPath(dir, L"d3d11.dll"), "");
        s.otherD3d11.push_back(fakeDll(DllKind::D3d11Provider, joinPath(dir, L"d3d11_edhm.dll"),
                                       "edhm-sha", L"3Dmigoto"));
        s.state.present = true;
        s.state.d3d11Installed = true;
        s.state.chainTarget = L"d3d11_edhm.dll";
        s.state.chainMod = L"EDHM";
        const Plan plan = planInstall(s, options, payload);
        check(hasStep(plan, Action::WritePayload, nullptr, L"d3d11.dll"), "EDVR is put back");
        check(notesMention(plan, "Keeping the chain"), "and the chain to EDHM is kept");
    }

    {   // The mod EDVR was chaining to has been uninstalled properly.
        Survey s = baseSurvey(dir);
        s.d3d11 = fakeDll(DllKind::Edvr, joinPath(dir, L"d3d11.dll"), "old-edvr");
        s.state.present = true;
        s.state.d3d11Installed = true;
        s.state.chainTarget = L"d3d11_edhm.dll";
        s.state.chainMod = L"EDHM";
        s.iniPresent = true;
        s.iniText = std::string(kNextIni) + "\r\n[advanced]\r\nreal_dll = d3d11_edhm.dll\r\n";
        s.baseIniText = kNextIni;
        const Plan plan = planInstall(s, options, payload);
        expectEq(iniValue(plannedIni(plan), "advanced.real_dll", "<absent>"), "",
                 "a chain target that is gone is cleared rather than left dangling");
        check(notesMention(plan, "no longer there"), "and the report says why");
    }

    {   // The game restored its own openvr_api.dll over ours -- an update, or a
        // file verification in the launcher.
        Survey s = baseSurvey(dir);
        s.openvrCurrent = fakeDll(DllKind::OpenVrRuntime,
                                  joinPath(s.game.openvrDir, L"openvr_api.dll"), "game-vr-new");
        s.openvrOrig = fakeDll(DllKind::OpenVrRuntime,
                               joinPath(s.game.openvrDir, L"openvr_api_orig.dll"), "game-vr-old");
        const Plan plan = planInstall(s, options, payload);
        check(hasStep(plan, Action::Rename, L"openvr_api.dll", L"openvr_api_orig.dll"),
              "the current runtime becomes the original");
        check(hasStep(plan, Action::Backup, L"openvr_api_orig.dll", L"openvr_api_orig.dll"),
              "and the superseded one is kept in the backup folder");
        check(notesMention(plan, "restored its own"), "the report explains it");
    }

    {   // The worst case: ours is installed and the game's original is gone.
        Survey s = baseSurvey(dir);
        s.openvrCurrent =
            fakeDll(DllKind::Edvr, joinPath(s.game.openvrDir, L"openvr_api.dll"), "old-edvr");
        s.openvrOrig = fakeDll(DllKind::Absent,
                               joinPath(s.game.openvrDir, L"openvr_api_orig.dll"), "");
        const Plan plan = planInstall(s, options, payload);
        check(!hasStep(plan, Action::WritePayload, nullptr, L"openvr_api.dll"),
              "nothing is written over a broken VR install");
        check(notesMention(plan, "verify"), "the report says how to fix it");
    }

    {   // The same, with one of our own backups to hand.
        Survey s = baseSurvey(dir);
        s.openvrCurrent =
            fakeDll(DllKind::Edvr, joinPath(s.game.openvrDir, L"openvr_api.dll"), "old-edvr");
        s.openvrOrig = fakeDll(DllKind::Absent,
                               joinPath(s.game.openvrDir, L"openvr_api_orig.dll"), "");
        s.openvrOrigInBackups.push_back(
            joinPath(dir, L"edvr_backup\\20260101-000000\\openvr_api.dll"));
        const Plan plan = planInstall(s, options, payload);
        check(hasStep(plan, Action::Backup, L"openvr_api.dll", L"openvr_api_orig.dll"),
              "the original is restored from the backup");
        check(hasStep(plan, Action::WritePayload, nullptr, L"openvr_api.dll"),
              "and EDVR is reinstalled in front of it");
    }

    {   // No Openvr folder at all: the other half still installs.
        Survey s = baseSurvey(dir);
        s.haveOpenvrDir = false;
        s.game.openvrDir.clear();
        const Plan plan = planInstall(s, options, payload);
        check(hasStep(plan, Action::WritePayload, nullptr, L"d3d11.dll"),
              "the fixes install without the VR half");
        check(!plan.problems.empty(), "and the missing half is reported");
    }


    {   // advanced.real_openvr_dll is a NAME, and a hand-edited edvr.ini can
        // put anything in it. A value with a path in it would move the game's
        // runtime out of the folder and over another mod, while the report
        // said "the game's own copy is renamed openvr_api_orig.dll".
        Survey s = baseSurvey(dir);
        s.openvrOrigName = L"..\\..\\d3d11.dll";   // out of the folder, onto another mod
        const Plan plan = planInstall(s, options, payload);
        check(hasStep(plan, Action::Rename, L"openvr_api.dll", L"openvr_api_orig.dll"),
              "a path in real_openvr_dll is refused back to the default name");
        for (const Step& step : plan.steps) {
            check(step.to.find(L"..") == std::wstring::npos,
                  "and no step points outside the game folder");
            break;
        }

        // A separator on its own, with no "..", is refused for the same reason.
        Survey nested = baseSurvey(dir);
        nested.openvrOrigName = L"sub\\somewhere.dll";
        const Plan nestedPlan = planInstall(nested, options, payload);
        check(hasStep(nestedPlan, Action::Rename, L"openvr_api.dll", L"openvr_api_orig.dll"),
              "and so is a name with a folder in it");

        Survey self = baseSurvey(dir);
        self.openvrOrigName = L"openvr_api.dll";   // the file it stands in for
        const Plan selfPlan = planInstall(self, options, payload);
        check(hasStep(selfPlan, Action::Rename, L"openvr_api.dll", L"openvr_api_orig.dll"),
              "and so is naming the file it is supposed to replace");

        Survey custom = baseSurvey(dir);
        custom.openvrOrigName = L"openvr_api_stock.dll";  // a legitimate hand install
        const Plan customPlan = planInstall(custom, options, payload);
        check(hasStep(customPlan, Action::Rename, L"openvr_api.dll", L"openvr_api_stock.dll"),
              "a plain filename is honoured, which is the point of the setting");
    }


    {   // Uninstalling when the game's original is gone must not delete the
        // only openvr_api.dll in the folder -- that leaves VR unable to start,
        // in the name of removing the thing that was making it work.
        Survey s = baseSurvey(dir);
        s.d3d11 = fakeDll(DllKind::Edvr, joinPath(dir, L"d3d11.dll"), payload.d3d11Sha);
        s.openvrCurrent = fakeDll(DllKind::Edvr, joinPath(s.game.openvrDir, L"openvr_api.dll"),
                                  payload.openvrSha);
        s.openvrOrig = fakeDll(DllKind::Absent,
                               joinPath(s.game.openvrDir, L"openvr_api_orig.dll"), "");
        const Plan plan = planUninstall(s, options);
        check(!hasStep(plan, Action::Delete, L"openvr_api.dll", nullptr),
              "uninstall leaves ours in place when there is nothing to put back");
        check(notesMention(plan, "no openvr_api.dll at all"), "and says why");

        // ...unless one of our own backups has the original, which is exactly
        // what that folder is for.
        Survey withBackup = s;
        withBackup.openvrOrigInBackups.push_back(
            joinPath(dir, L"edvr_backup\\20260101-000000\\openvr_api.dll"));
        const Plan recovered = planUninstall(withBackup, options);
        // Copied OVER ours rather than deleting first: the folder is never
        // without an openvr_api.dll, not even for the moment between two steps.
        check(hasStep(recovered, Action::Backup, L"openvr_api.dll", L"openvr_api.dll"),
              "with a backup to hand, the game's own file is restored from it");
        check(!hasStep(recovered, Action::Delete, L"openvr_api.dll", nullptr),
              "and ours is replaced in place, never deleted first");
    }

    {   // A second mod taking the d3d11.dll slot means EDVR chains to the new
        // one -- and the old one is still on disk, loaded by nothing. Saying
        // nothing about that is how somebody's EDHM quietly stops working.
        Survey s = baseSurvey(dir);
        s.d3d11 = fakeDll(DllKind::D3d11Provider, joinPath(dir, L"d3d11.dll"), "reshade-sha",
                          L"ReShade");
        s.otherD3d11.push_back(fakeDll(DllKind::D3d11Provider, joinPath(dir, L"d3d11_edhm.dll"),
                                       "edhm-sha", L"3Dmigoto"));
        s.state.present = true;
        s.state.d3d11Installed = true;
        s.state.chainTarget = L"d3d11_edhm.dll";
        s.state.chainMod = L"EDHM";
        const Plan plan = planInstall(s, options, payload);
        check(notesMention(plan, "can only chain to one"),
              "dropping a previous chain target is reported, not silent");
        check(notesMention(plan, "d3d11_edhm.dll"), "and the report names the one left behind");
    }

    {   // The game is running, out of this very folder.
        Survey s = baseSurvey(dir);
        s.gameRunningHere = true;
        const Plan plan = planInstall(s, options, payload);
        check(plan.blocked && plan.steps.empty(), "nothing is planned while the game is running");
        check(planUninstall(s, options).blocked, "and nothing is taken back out either");
    }

    {   // The game is running out of the OTHER install. A machine with two of
        // them is somebody's actual setup, and a refusal that went by the
        // executable's name alone stopped the folder nobody was playing from.
        Survey s = baseSurvey(dir);
        s.gameRunningElsewhere = true;
        const Plan plan = planInstall(s, options, payload);
        check(!plan.blocked && !plan.steps.empty(),
              "a game running from a different folder does not stop this one");
        check(notesMention(plan, "different folder"),
              "and the report says so rather than leaving it unexplained");
        const Plan out = planUninstall(s, options);
        check(!out.blocked, "the same on the way back out");
        check(notesMention(out, "different folder"), "and it says so there too");
    }

    {   // A build made without EDVR's openvr_api.dll. Not a choice anybody can
        // make from the window any more -- both files are the patch -- so it
        // can only be a developer build, and it has to say so rather than
        // quietly installing half of one. package.bat refuses to ship it.
        Survey s = baseSurvey(dir);
        PayloadInfo halfBuild = payload;
        halfBuild.haveOpenvr = false;
        const Plan plan = planInstall(s, options, halfBuild);
        check(!hasStep(plan, Action::Rename, L"openvr_api.dll", L"openvr_api_orig.dll"),
              "a build without the VR half does not touch the game's runtime");
        check(hasStep(plan, Action::WritePayload, nullptr, L"d3d11.dll"),
              "the fixes still install");
        check(notesMention(plan, "development build"), "and the report says the build is partial");
    }

    {   // Uninstall, with a chained mod and the original runtime in place.
        Survey s = baseSurvey(dir);
        s.d3d11 = fakeDll(DllKind::Edvr, joinPath(dir, L"d3d11.dll"), payload.d3d11Sha);
        s.otherD3d11.push_back(fakeDll(DllKind::D3d11Provider, joinPath(dir, L"d3d11_edhm.dll"),
                                       "edhm-sha", L"3Dmigoto"));
        s.openvrCurrent = fakeDll(DllKind::Edvr, joinPath(s.game.openvrDir, L"openvr_api.dll"),
                                  payload.openvrSha);
        s.openvrOrig = fakeDll(DllKind::OpenVrRuntime,
                               joinPath(s.game.openvrDir, L"openvr_api_orig.dll"), "game-vr");
        s.iniPresent = true;
        s.iniText = kNextIni;
        s.state.present = true;
        s.state.chainTarget = L"d3d11_edhm.dll";
        s.state.chainMod = L"EDHM";

        const Plan plan = planUninstall(s, options);
        check(hasStep(plan, Action::Delete, L"d3d11.dll", nullptr), "EDVR's d3d11.dll is removed");
        check(hasStep(plan, Action::Rename, L"d3d11_edhm.dll", L"d3d11.dll"),
              "and EDHM gets its name back -- otherwise uninstalling EDVR silently uninstalls it");
        check(hasStep(plan, Action::Rename, L"openvr_api_orig.dll", L"openvr_api.dll"),
              "the game's runtime is put back");
        check(!hasStep(plan, Action::Delete, L"edvr.ini", nullptr),
              "settings are left in place by default");

        Options withSettings = options;
        withSettings.removeSettings = true;
        const Plan plan2 = planUninstall(s, withSettings);
        check(hasStep(plan2, Action::Delete, L"edvr.ini", nullptr),
              "and removed when that is asked for");
        check(hasStep(plan2, Action::Backup, L"edvr.ini", L"edvr.ini"),
              "after a copy goes to the backup folder");
    }
}

// ---------------------------------------------------------------------------
// apply, for real, in a scratch folder
// ---------------------------------------------------------------------------

static PayloadProvider provider(bool failOpenvr) {
    return [failOpenvr](const std::string& item, const void** data, size_t* size) {
        static const char kD3d11[] = "TEST-D3D11-PAYLOAD";
        static const char kOpenvr[] = "TEST-OPENVR-PAYLOAD";
        if (item == "d3d11") {
            *data = kD3d11;
            *size = sizeof(kD3d11) - 1;
            return true;
        }
        if (item == "openvr" && !failOpenvr) {
            *data = kOpenvr;
            *size = sizeof(kOpenvr) - 1;
            return true;
        }
        return false;
    };
}

// The hashes here are the REAL ones, taken off the files just written.
//
// applyPlan checks, before it touches anything, that the files are still the
// ones the plan was made for -- so a survey carrying invented hashes describes
// a folder that does not exist, and the run is refused. Which is the check
// doing its job; this is how a test says "and the folder really is like that".
static Survey scratchSurvey(const std::wstring& gameDir) {
    Survey s = baseSurvey(gameDir);
    s.d3d11 = fakeDll(DllKind::Absent, joinPath(gameDir, L"d3d11.dll"), "");
    const std::wstring runtime = joinPath(s.game.openvrDir, L"openvr_api.dll");
    s.openvrCurrent = fakeDll(DllKind::OpenVrRuntime, runtime, sha256File(runtime));
    return s;
}

static void layOutScratchGame(const std::wstring& gameDir) {
    removeTree(gameDir);
    makeTree(joinPath(gameDir, L"Openvr\\win64"));
    writeAll(joinPath(gameDir, L"EliteDangerous64.exe"), "not really the game");
    writeAll(joinPath(gameDir, L"Openvr\\win64\\openvr_api.dll"), "THE-GAMES-OWN-RUNTIME");
}

static void testApply(const std::wstring& scratch) {
    printf("\napplying a plan\n");
    const std::wstring gameDir = joinPath(scratch, L"game");
    const PayloadInfo payload = testPayload(kNextIni);
    const Options options = testOptions();

    {   // The whole thing, on real files.
        layOutScratchGame(gameDir);
        const Survey s = scratchSurvey(gameDir);
        const Plan plan = planInstall(s, options, payload);
        const ApplyResult result = applyPlan(plan, provider(false));
        check(result.ok, "the plan applies", result.error);

        expectEq(readAll(joinPath(gameDir, L"d3d11.dll")), "TEST-D3D11-PAYLOAD",
                 "d3d11.dll is ours afterwards");
        expectEq(readAll(joinPath(gameDir, L"Openvr\\win64\\openvr_api_orig.dll")),
                 "THE-GAMES-OWN-RUNTIME", "the game's runtime survived under its new name");
        expectEq(readAll(joinPath(gameDir, L"Openvr\\win64\\openvr_api.dll")),
                 "TEST-OPENVR-PAYLOAD", "and ours is in its place");
        check(fileExists(joinPath(gameDir, L"edvr.ini")), "edvr.ini is written");
        check(fileExists(joinPath(gameDir, L"edvr_install\\state.ini")), "the record is written");
        check(fileExists(joinPath(gameDir, L"edvr_install\\edvr.ini.base")),
              "and this version's default ini is kept for the next merge");
        check(fileExists(joinPath(gameDir,
                                  L"edvr_backup\\20260827-120000\\openvr_api.dll")),
              "the game's runtime is also in the backup folder");

        // Read the folder back the way a second run would.
        const InstallState state = readState(gameDir);
        check(state.present, "the record parses back");
        expectEq(state.edvrVersion, payload.version, "and names the version installed");
    }

    {   // The failure that matters: the rename has happened, then the write
        // fails. The folder must not be left without an openvr_api.dll.
        layOutScratchGame(gameDir);
        const Survey s = scratchSurvey(gameDir);
        const Plan plan = planInstall(s, options, payload);
        const ApplyResult result = applyPlan(plan, provider(true));
        check(!result.ok, "a failing payload fails the run");
        check(result.rolledBack, "and the run is rolled back");
        expectEq(readAll(joinPath(gameDir, L"Openvr\\win64\\openvr_api.dll")),
                 "THE-GAMES-OWN-RUNTIME",
                 "the game's own openvr_api.dll is back under its own name");
        check(!fileExists(joinPath(gameDir, L"Openvr\\win64\\openvr_api_orig.dll")),
              "with no half-renamed leftover");
        check(!fileExists(joinPath(gameDir, L"d3d11.dll")),
              "and the file this run had already written is gone again");
    }


    {   // The folder changing between the plan and the yes. Another installer
        // window, a game update, a second copy of this one -- executing the
        // stale plan is how the game's original gets renamed onto itself.
        layOutScratchGame(gameDir);
        const Survey s = scratchSurvey(gameDir);
        const Plan plan = planInstall(s, options, payload);

        // Somebody else replaces the runtime after the plan was made.
        writeAll(joinPath(gameDir, L"Openvr\\win64\\openvr_api.dll"), "SOMEBODY-ELSES-FILE");

        const ApplyResult stale = applyPlan(plan, provider(false));
        check(!stale.ok, "a plan made for a folder that has since changed is refused");
        check(stale.done.empty(), "and nothing at all was done");
        expectEq(readAll(joinPath(gameDir, L"Openvr\\win64\\openvr_api.dll")),
                 "SOMEBODY-ELSES-FILE", "the file that changed is left exactly as it was");
        check(!fileExists(joinPath(gameDir, L"d3d11.dll")),
              "and no half-install was left behind");
    }

    {   // A failure after a file has been replaced. The backups are the only
        // way back, so the rollback must not take them with it -- and the
        // result must not claim the folder is as it was.
        layOutScratchGame(gameDir);
        Survey s = scratchSurvey(gameDir);
        check(applyPlan(planInstall(s, options, payload), provider(false)).ok, "installed once");

        // A file where the record directory has to go: every step succeeds
        // until the record is written.
        removeTree(joinPath(gameDir, L"edvr_install"));
        writeAll(joinPath(gameDir, L"edvr_install"), "not a directory");

        PayloadInfo newer = payload;
        newer.d3d11Sha = "dddd-newer";
        Survey again = scratchSurvey(gameDir);
        const std::wstring ours = joinPath(gameDir, L"d3d11.dll");
        again.d3d11 = fakeDll(DllKind::Edvr, ours, sha256File(ours));
        const std::wstring vr = joinPath(gameDir, L"Openvr\\win64\\openvr_api.dll");
        const std::wstring orig = joinPath(gameDir, L"Openvr\\win64\\openvr_api_orig.dll");
        again.openvrCurrent = fakeDll(DllKind::Edvr, vr, sha256File(vr));
        again.openvrOrig = fakeDll(DllKind::OpenVrRuntime, orig, sha256File(orig));
        again.iniPresent = true;
        again.iniText = readAll(joinPath(gameDir, L"edvr.ini"));

        Options second = options;
        second.backupStamp = L"20260827-121500";
        second.repair = true;   // force the writes even though little changed
        const ApplyResult result = applyPlan(planInstall(again, second, newer), provider(false));
        check(!result.ok, "a run that cannot finish fails");
        check(fileExists(joinPath(gameDir, L"edvr_backup\\20260827-121500\\d3d11.dll")),
              "the backups it took are still there afterwards");
        check(result.overwrote, "and it admits a file had already been replaced");
    }

    {   // Install, then uninstall, and the folder should be as it started.
        layOutScratchGame(gameDir);
        Survey s = scratchSurvey(gameDir);
        check(applyPlan(planInstall(s, options, payload), provider(false)).ok, "installed");

        const std::wstring ourD3d11 = joinPath(gameDir, L"d3d11.dll");
        const std::wstring ourVr = joinPath(s.game.openvrDir, L"openvr_api.dll");
        const std::wstring theirVr = joinPath(s.game.openvrDir, L"openvr_api_orig.dll");
        s.d3d11 = fakeDll(DllKind::Edvr, ourD3d11, sha256File(ourD3d11));
        s.openvrCurrent = fakeDll(DllKind::Edvr, ourVr, sha256File(ourVr));
        s.openvrOrig = fakeDll(DllKind::OpenVrRuntime, theirVr, sha256File(theirVr));
        s.iniPresent = true;
        s.iniText = readAll(joinPath(gameDir, L"edvr.ini"));
        s.state = readState(gameDir);

        Options removeAll = options;
        removeAll.removeSettings = true;
        removeAll.backupStamp = L"20260827-120100";
        const ApplyResult result = applyPlan(planUninstall(s, removeAll), provider(false));
        check(result.ok, "uninstalled", result.error);
        check(!fileExists(joinPath(gameDir, L"d3d11.dll")), "d3d11.dll is gone");
        check(!fileExists(joinPath(gameDir, L"edvr.ini")), "edvr.ini is gone");
        expectEq(readAll(joinPath(gameDir, L"Openvr\\win64\\openvr_api.dll")),
                 "THE-GAMES-OWN-RUNTIME", "and the game's runtime is back under its own name");
        check(!fileExists(joinPath(gameDir, L"Openvr\\win64\\openvr_api_orig.dll")),
              "with nothing renamed left behind");
    }

    {   // A second install must keep what the user changed in between.
        layOutScratchGame(gameDir);
        Survey s = scratchSurvey(gameDir);
        check(applyPlan(planInstall(s, options, payload), provider(false)).ok, "installed once");

        std::string tuned = readAll(joinPath(gameDir, L"edvr.ini"));
        const size_t at = tuned.find("black_void = 0");
        check(at != std::string::npos, "the installed ini has the setting to tune");
        if (at != std::string::npos) tuned.replace(at, strlen("black_void = 0"), "black_void = 1");
        writeAll(joinPath(gameDir, L"edvr.ini"), tuned);

        // A new version arrives with a different d3d11.dll and a new default.
        PayloadInfo newer = payload;
        newer.version = "v9.9.10-test";
        newer.d3d11Sha = "cccc-newer-d3d11";
        std::string newerIni = kNextIni;
        newerIni += "\r\n[fix]\r\nbrand_new = 7\r\n";
        newer.iniText = newerIni;

        Survey again = surveyTarget(s.game);
        // The build machine may well have Elite running -- it did the day this
        // was written. It cannot be running from this scratch folder, so the
        // survey reads it as somebody else's and plans anyway; both flags are
        // cleared regardless, because this case is about the ini merge over a
        // folder a previous run really wrote and nothing else.
        again.gameRunningHere = false;
        again.gameRunningElsewhere = false;
        Options second = options;
        second.backupStamp = L"20260827-120200";
        const Plan plan = planInstall(again, second, newer);
        // surveyTarget reads the real folder, where our payload is not a real
        // PE -- so the planner sees "not ours" and would chain. What is being
        // checked here is the ini, which is read from the same real folder.
        const std::string merged = plannedIni(plan);
        expectEq(iniValue(merged, "fix.black_void"), "1", "the tuned value survives the update");
        expectEq(iniValue(merged, "fix.brand_new"), "7", "and the new setting arrives");
    }
}

// ---------------------------------------------------------------------------
// the mirror kept outside the game folder
// ---------------------------------------------------------------------------

// The leaf of a matching subfolder under `dir`, or empty. Used only to find
// the restore-<stamp> folder restoreFromMirror creates, whose exact name is
// the clock at the moment the test runs.
static std::wstring firstSubdirLike(const std::wstring& dir, const std::wstring& pattern) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(joinPath(dir, pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    std::wstring name;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            name = fd.cFileName;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return name;
}

static void testMirror(const std::wstring& scratch) {
    printf("\nthe mirror kept outside the game folder\n");

    // A real install, laid out and applied exactly like testApply does --
    // the mirror is worth testing against real files, not hand-placed ones.
    const std::wstring gameDir = joinPath(scratch, L"mirrorgame");
    layOutScratchGame(gameDir);
    const PayloadInfo payload = testPayload(kNextIni);
    const Options options = testOptions();
    const Survey s = scratchSurvey(gameDir);
    const Plan plan = planInstall(s, options, payload);
    const ApplyResult applied = applyPlan(plan, provider(false));
    check(applied.ok, "the scratch install this test mirrors applies", applied.error);

    const std::wstring mirrorDir = joinPath(scratch, L"mirrorroot\\mirrorgame-test");
    removeTree(joinPath(scratch, L"mirrorroot"));

    {
        const MirrorResult m = updateMirror(gameDir, plan.backupDir, mirrorDir);
        check(m.ok, "updateMirror succeeds against a real install");
        check(fileExists(joinPath(mirrorDir, L"edvr.ini")), "edvr.ini is mirrored");
        check(fileExists(joinPath(mirrorDir, L"edvr.ini.base")), "edvr.ini.base is mirrored");
        check(fileExists(joinPath(mirrorDir, L"state.ini")), "state.ini is mirrored");
        check(fileExists(joinPath(mirrorDir, L"backup\\openvr_api.dll")),
              "the backed-up openvr_api.dll is mirrored -- the game had no d3d11.dll of its own "
              "to back up, so only this half of the pair exists here, same as in edvr_backup\\");
        expectEq(readAll(joinPath(mirrorDir, L"edvr.ini")), readAll(joinPath(gameDir, L"edvr.ini")),
                 "the mirrored edvr.ini matches the live one");
    }

    {
        const MirrorInfo info = readMirror(mirrorDir);
        check(info.hasIni, "readMirror sees the ini");
        check(info.hasBaseIni, "and the base ini");
        check(info.hasState, "and the state");
        check(info.hasBackupPair, "and the backup pair");
        check(!info.savedUtc.empty(), "and a saved-at time for it");
    }

    {   // A settings-window save only ever touches edvr.ini -- re-copying the
        // record and the DLLs on every toggle would be pointless disk I/O for
        // files that provably did not change.
        writeAll(joinPath(gameDir, L"edvr.ini"), "[fix]\r\nshare_exposure = 0\r\n");
        const MirrorResult m = updateMirrorIni(gameDir, mirrorDir);
        check(m.ok, "updateMirrorIni succeeds");
        check(m.saved.size() == 1 && m.saved[0] == "edvr.ini",
              "and reports only the ini as saved");
        expectEq(readAll(joinPath(mirrorDir, L"edvr.ini")), "[fix]\r\nshare_exposure = 0\r\n",
                 "the mirrored ini picks up the settings-only change");
        check(fileExists(joinPath(mirrorDir, L"backup\\openvr_api.dll")),
              "the backup pair mirrored earlier is untouched");
    }

    {   // The disaster this exists for: the game folder is wiped -- by a game
        // update, same as 2026-09-02 -- and the mirror outside it is all that
        // is left.
        const std::wstring wiped = joinPath(scratch, L"mirrorgame-wiped");
        removeTree(wiped);
        makeTree(wiped);
        const MirrorInfo info = readMirror(mirrorDir);
        std::vector<std::string> notes;
        check(restoreFromMirror(wiped, info, &notes), "restoreFromMirror succeeds");
        check(fileExists(joinPath(wiped, L"edvr.ini")), "edvr.ini comes back");
        check(fileExists(joinPath(wiped, L"edvr_install\\edvr.ini.base")),
              "so does edvr.ini.base");
        check(fileExists(joinPath(wiped, L"edvr_install\\state.ini")), "so does state.ini");
        check(!notes.empty(), "and it says what it did");

        const std::wstring backupRoot = joinPath(wiped, L"edvr_backup");
        const std::wstring stamp = firstSubdirLike(backupRoot, L"restored-*");
        check(!stamp.empty(), "the backup pair lands in its own restored-<stamp> folder");
        check(fileExists(joinPath(joinPath(backupRoot, stamp), L"openvr_api.dll")),
              "which is exactly where the installer's own recovery scan for a genuine original "
              "openvr_api.dll already looks");
    }

    {   // Nothing to restore is not an error; it is the ordinary case of a
        // folder that was never mirrored at all.
        MirrorInfo empty;
        std::vector<std::string> notes;
        check(!restoreFromMirror(joinPath(scratch, L"nowhere"), empty, &notes),
              "restoreFromMirror refuses a mirror with no edvr.ini");
        check(notes.empty(), "and adds no notes when it does");
    }

    {   // Frontier's own Products\ folder name is identical whether the game
        // came from the Frontier launcher or Steam, so the leaf alone cannot
        // be the mirror's name on a machine -- like this one -- with both.
        GameInstall steam;
        steam.dir = L"C:\\Games\\steamapps\\common\\Elite Dangerous\\Products\\"
                    L"elite-dangerous-odyssey-64";
        steam.source = L"Steam";
        GameInstall frontier = steam;
        frontier.source = L"Frontier launcher";
        const std::wstring root = L"C:\\fake\\EDVR";

        const std::wstring steamDir = mirrorDirFor(steam, root);
        const std::wstring frontierDir = mirrorDirFor(frontier, root);
        check(!steamDir.empty() && !frontierDir.empty(), "both installs resolve to a folder");
        check(steamDir != frontierDir,
              "two storefronts sharing a leaf folder name get two different mirrors");
        check(mirrorDirFor(steam, root) == steamDir,
              "and the same install resolves to the same mirror every time");
    }

    {   // No root at all -- LOCALAPPDATA unreadable, in practice never -- must
        // be a quiet no-op everywhere, not a crash or a write into "".
        GameInstall g;
        g.dir = L"C:\\Games\\ed";
        g.source = L"Steam";
        check(mirrorDirFor(g, L"").empty(), "no root means no mirror path");
        const MirrorResult m = updateMirror(gameDir, plan.backupDir, L"");
        check(!m.ok, "updateMirror with no mirror directory is a safe no-op");
        const MirrorResult mi = updateMirrorIni(gameDir, L"");
        check(!mi.ok, "so is updateMirrorIni");
    }
}

// ---------------------------------------------------------------------------
// reading a DLL to find out whose it is
// ---------------------------------------------------------------------------

static void testProbe(const std::wstring& scratch) {
    printf("\nreading DLLs\n");

    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const DllInfo d3d11 = probeDll(joinPath(system, L"d3d11.dll"));
    check(d3d11.kind == DllKind::D3d11Provider,
          "Windows' own d3d11.dll reads as a d3d11 provider");
    check(d3d11.is64, "and as 64-bit");
    check(!d3d11.hasEdvrExports, "and not as ours");
    check(d3d11.sha256.size() == 64, "its hash is computed");

    const DllInfo missing = probeDll(joinPath(scratch, L"nothing-here.dll"));
    check(missing.kind == DllKind::Absent, "a file that is not there reads as absent");

    const std::wstring junk = joinPath(scratch, L"junk.dll");
    writeAll(junk, "this is not a PE file at all");
    const DllInfo notPe = probeDll(junk);
    check(notPe.kind == DllKind::Unreadable, "a file that is not a PE reads as unreadable");
    DeleteFileW(junk.c_str());

    // Naming a mod from its version information is what turns "another
    // d3d11.dll" into "EDHM" in the report, and picks the name it is renamed to.
    DllInfo edhm = fakeDll(DllKind::D3d11Provider, L"C:\\x\\d3d11.dll", "x", L"3Dmigoto");
    expectEq(toUtf8(modNameOf(edhm)), "EDHM", "3Dmigoto is recognised as EDHM");
    expectEq(toUtf8(chainNameFor(edhm)), "d3d11_edhm.dll", "and gets a name that says so");
    DllInfo unknown = fakeDll(DllKind::D3d11Provider, L"C:\\x\\d3d11.dll", "x", L"Something Else");
    expectEq(toUtf8(chainNameFor(unknown)), "d3d11_other.dll",
             "an unrecognised mod still gets a name of its own");
}

// ---------------------------------------------------------------------------
// which install is running
// ---------------------------------------------------------------------------

static void testRunState() {
    printf("\nwho is running\n");

    // Elite is not running on a build machine, and no test can make it. This
    // process is running, and it is the same shape: a name that matches, and a
    // folder that either is or is not the one being asked about, which is the
    // whole of the question.
    wchar_t self[1024]{};
    const DWORD n = GetModuleFileNameW(nullptr, self, 1024);
    check(n > 0 && n < 1024, "the test can find its own executable");
    const std::wstring path(self, n);
    const size_t slash = path.find_last_of(L"\\/");
    check(slash != std::wstring::npos, "and that path has a folder in it");
    if (slash == std::wstring::npos) return;
    const std::wstring dir = path.substr(0, slash);
    const std::wstring name = path.substr(slash + 1);

    check(runStateOf(name.c_str(), dir) == GameRunState::ThisFolder,
          "a process running from the folder asked about is that folder's");
    check(runStateOf(name.c_str(), joinPath(dir, L"somewhere-else")) == GameRunState::OtherFolder,
          "the same name from another folder is not -- the two-install refusal");
    check(runStateOf(L"a-name-nothing-on-this-machine-has.exe", dir) == GameRunState::NotRunning,
          "a name nobody is running is not running");
    check(runStateOf(name.c_str(), std::wstring()) == GameRunState::ThisFolder,
          "with no folder to compare against, a match is still a refusal");

    // Spellings Windows treats as one folder have to read as one folder here,
    // or the refusal misses the install it exists for.
    std::wstring shouty = dir;
    for (wchar_t& c : shouty) c = static_cast<wchar_t>(towupper(c));
    check(runStateOf(name.c_str(), shouty + L"\\") == GameRunState::ThisFolder,
          "case and a trailing separator are not a different folder");
    check(runStateOf(name.c_str(), joinPath(joinPath(dir, L"down"), L"..")) ==
              GameRunState::ThisFolder,
          "nor is a path that goes down and comes back up");
}

// ---------------------------------------------------------------------------
// the settings window's write path
// ---------------------------------------------------------------------------

static void testSettings(const std::wstring& root, const std::wstring& scratch) {
    printf("\nsettings\n");

    const std::wstring dir = joinPath(scratch, L"settings");
    removeTree(dir);
    makeTree(dir);
    const std::string shipped = readAll(joinPath(root, L"edvr.ini"));
    if (shipped.empty()) {
        fail("read the repository edvr.ini", "not found");
        return;
    }
    writeAll(joinPath(dir, L"edvr.ini"), shipped);

    SettingsModel model;
    model.load(dir);
    check(!model.rows().empty(), "the generated schema has settings in it");

    size_t toggle = SIZE_MAX;
    size_t choice = SIZE_MAX;
    for (size_t i = 0; i < model.rows().size(); ++i) {
        const std::string key = model.rows()[i].def->key;
        if (key == "black_void") toggle = i;
        if (key == "sun_glare") choice = i;
    }
    if (toggle == SIZE_MAX || choice == SIZE_MAX) {
        fail("find the settings to exercise", "black_void or sun_glare is not exposed");
        return;
    }
    expectEq(model.rows()[toggle].value, "1", "a toggle reads its shipped value");
    check(model.rows()[toggle].isRecommended, "and starts at the recommended value");

    check(model.set(toggle, "0"), "writing a toggle succeeds", model.lastError());
    const std::string after = readAll(joinPath(dir, L"edvr.ini"));
    expectEq(iniValue(after, "fix.black_void"), "0", "the new value is in the file");
    check(!model.rows()[toggle].isRecommended,
          "and the row now says it is away from the recommended value");

    // The property that matters: writing one setting moves nothing else. This
    // file is somebody's tuning, and a settings window that rewrites values it
    // was not asked about is worse than no settings window.
    int drifted = 0;
    for (const SettingRow& row : model.rows()) {
        const std::string dotted = std::string(row.def->section) + "." + row.def->key;
        if (dotted == "fix.black_void") continue;
        if (iniValue(after, dotted, "<absent>") != iniValue(shipped, dotted, "<absent>")) ++drifted;
    }
    check(drifted == 0, "no other setting changed");
    check(after.size() == shipped.size(),
          "the file is the same size: one character replaced, no reformatting");
    check(after.find("# Make the space around the on-foot screen pure black") != std::string::npos,
          "the comments are still there");

    check(model.set(choice, "realistic"), "writing a choice succeeds", model.lastError());
    expectEq(iniValue(readAll(joinPath(dir, L"edvr.ini")), "fix.sun_glare"), "realistic",
             "the choice landed");

    // Every exposed setting must be readable from the shipped file, or the
    // window would show "(not set)" for something the game is using.
    int unreadable = 0;
    for (const SettingRow& row : model.rows()) {
        if (row.value.empty() && std::string(row.def->shipped) != "") ++unreadable;
    }
    check(unreadable == 0, "every exposed setting has a value to show");
}

// ---------------------------------------------------------------------------
// the log bundle
// ---------------------------------------------------------------------------

// The entry names in a zip, read back out of its central directory. Enough of
// a reader to prove the writer produced something a reader can open.
static std::vector<std::string> zipEntryNames(const std::wstring& path) {
    std::vector<std::string> names;
    const std::string data = readAll(path);
    if (data.size() < 22) return names;

    // "PK" then two bytes: 05 06 ends the central directory, 01 02 heads an
    // entry in it. Compared byte by byte rather than against a string literal,
    // because those two bytes are not printable characters.
    auto signature = [&](size_t at, unsigned char third, unsigned char fourth) {
        return at + 4 <= data.size() && data[at] == 0x50 && data[at + 1] == 0x4B &&
               static_cast<unsigned char>(data[at + 2]) == third &&
               static_cast<unsigned char>(data[at + 3]) == fourth;
    };

    size_t eocd = std::string::npos;
    for (size_t i = data.size() - 22; i + 1 > 0; --i) {
        if (signature(i, 5, 6)) {
            eocd = i;
            break;
        }
        if (i == 0) break;
    }
    if (eocd == std::string::npos) return names;

    auto read16 = [&](size_t at) {
        return static_cast<unsigned>(static_cast<unsigned char>(data[at])) |
               (static_cast<unsigned>(static_cast<unsigned char>(data[at + 1])) << 8);
    };
    auto read32 = [&](size_t at) {
        return static_cast<unsigned long>(read16(at)) |
               (static_cast<unsigned long>(read16(at + 2)) << 16);
    };

    const unsigned count = read16(eocd + 10);
    size_t at = static_cast<size_t>(read32(eocd + 16));
    for (unsigned i = 0; i < count && at + 46 <= data.size(); ++i) {
        if (!signature(at, 1, 2)) break;
        const unsigned nameLength = read16(at + 28);
        const unsigned extra = read16(at + 30);
        const unsigned comment = read16(at + 32);
        names.push_back(data.substr(at + 46, nameLength));
        at += 46 + nameLength + extra + comment;
    }
    return names;
}

static bool bundleHas(const std::vector<std::string>& names, const char* wanted) {
    for (const std::string& name : names) {
        if (name == wanted) return true;
    }
    return false;
}

static void testLogBundle(const std::wstring& scratch) {
    printf("\nthe log bundle\n");

    const std::wstring dir = joinPath(scratch, L"logs");
    removeTree(dir);
    makeTree(joinPath(dir, L"edvr_logs"));
    writeAll(joinPath(dir, L"EliteDangerous64.exe"), "not really the game");
    writeAll(joinPath(dir, L"edvr.ini"), "[fix]\r\nshare_exposure = 1\r\n");
    writeAll(joinPath(dir, L"edvr_breadcrumbs.txt"), "d3d11 attached");
    writeAll(joinPath(dir, L"edvr_FATAL.txt"), "could not load the real openvr");

    // Two sessions: an old pair and a new pair. Only the new pair belongs in
    // the zip -- an attachment with six launches in it is harder to read, not
    // more informative.
    const std::wstring logs = joinPath(dir, L"edvr_logs");
    writeAll(joinPath(logs, L"edvr_gfx_20260101_100000.log"), "an old session");
    writeAll(joinPath(logs, L"edvr_vr_20260101_100003.log"), "an old session");
    Sleep(1100);  // the newest-file test is by write time, so they must differ
    writeAll(joinPath(logs, L"edvr_gfx_20260827_140000.log"), "the session that matters");
    writeAll(joinPath(logs, L"edvr_vr_20260827_140003.log"), "the session that matters");

    const LogBundle bundle = collectLogs(dir, scratch);
    check(bundle.ok, "the bundle is written", bundle.error);
    if (!bundle.ok) return;
    check(fileExists(bundle.zipPath), "the zip is on disk");

    const std::vector<std::string> names = zipEntryNames(bundle.zipPath);
    check(!names.empty(), "the zip has a readable central directory");
    check(bundleHas(names, "edvr_gfx_20260827_140000.log"), "the newest session's gfx log is in");
    check(bundleHas(names, "edvr_vr_20260827_140003.log"), "and its vr log");
    check(!bundleHas(names, "edvr_gfx_20260101_100000.log"),
          "the previous session is left out");
    check(bundleHas(names, "edvr_breadcrumbs.txt"), "the breadcrumbs are in");
    check(bundleHas(names, "edvr_FATAL.txt"), "the fatal note is in");
    check(bundleHas(names, "edvr.ini"), "the settings file is in");

    // A folder with nothing to collect says so rather than writing an empty zip.
    const std::wstring bare = joinPath(scratch, L"barelogs");
    removeTree(bare);
    makeTree(bare);
    const LogBundle nothing = collectLogs(bare, scratch);
    check(!nothing.ok, "a folder with no logs and no settings collects nothing");
}

static void testState() {
    printf("\nthe install record\n");
    InstallState state;
    state.edvrVersion = "v0.10.1";
    state.installedUtc = "2026-08-27T12:00:00Z";
    state.openvrDir = L"Openvr\\win64";
    state.d3d11Installed = true;
    state.d3d11Sha = "1234";
    state.chainTarget = L"d3d11_edhm.dll";
    state.chainMod = L"EDHM";
    state.openvrInstalled = true;
    state.openvrSha = "5678";
    state.openvrOrigName = L"openvr_api_orig.dll";
    state.openvrOrigSha = "9abc";
    state.iniSha = "def0";

    const InstallState back = parseState(serializeState(state));
    check(back.present, "a written record reads back as present");
    expectEq(back.edvrVersion, state.edvrVersion, "the version survives");
    expectEq(toUtf8(back.chainTarget), "d3d11_edhm.dll", "the chain target survives");
    expectEq(toUtf8(back.openvrDir), "Openvr\\win64", "the openvr folder survives");
    expectEq(back.openvrOrigSha, "9abc", "the original runtime's hash survives");
    check(!parseState("").present, "an empty record is not a record");
    check(!parseState("[edvr]\r\nsomething = else\r\n").present,
          "a file that merely parses is not a record either");
}

int wmain(int argc, wchar_t** argv) {
    const std::wstring root = argc > 1 ? argv[1] : L".";
    const std::wstring scratch = argc > 2 ? argv[2] : joinPath(root, L"build\\insttest_scratch");
    makeTree(scratch);

    printf("installer_test: root %ls\n", root.c_str());

    testMerge();
    testShippedIni(root);
    testPlanner();
    testApply(scratch);
    testMirror(scratch);
    testProbe(scratch);
    testRunState();
    testSettings(root, scratch);
    testLogBundle(scratch);
    testState();

    printf("\n%s\n", g_fails == 0 ? "installer_test: all good" : "installer_test: FAILURES");
    return g_fails == 0 ? 0 : 1;
}
