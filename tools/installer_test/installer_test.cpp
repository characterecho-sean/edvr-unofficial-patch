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
#include <string>
#include <vector>

#include "../../src/installer/apply.h"
#include "../../src/installer/plan.h"

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
    s.gameRunning = false;
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

    // A user file made from the real one, with the settings a real report
    // would name.
    std::string user = shipped;
    const size_t damp = user.find("exposure_damping = 0");
    if (damp != std::string::npos)
        user.replace(damp, strlen("exposure_damping = 0"), "exposure_damping = 0.7");

    MergeReport real;
    const std::string merged =
        mergeIni(shipped, user, &shipped, {{"advanced.real_dll", "d3d11_edhm.dll"}}, &real);
    expectEq(iniValue(merged, "fix.exposure_damping"), "0.7",
             "a real tuned value survives a real merge");
    expectEq(iniValue(merged, "advanced.real_dll"), "d3d11_edhm.dll",
             "chaining is written into the real file");
    check(merged.size() > shipped.size() - 64,
          "the merged file is still the whole documented ini, not a stripped one");
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

    {   // The game is running.
        Survey s = baseSurvey(dir);
        s.gameRunning = true;
        const Plan plan = planInstall(s, options, payload);
        check(plan.blocked && plan.steps.empty(), "nothing is planned while the game is running");
    }

    {   // Only one half asked for.
        Survey s = baseSurvey(dir);
        Options half = options;
        half.installOpenvr = false;
        const Plan plan = planInstall(s, half, payload);
        check(!hasStep(plan, Action::Rename, L"openvr_api.dll", L"openvr_api_orig.dll"),
              "the VR half is left alone when it is not wanted");
        check(hasStep(plan, Action::WritePayload, nullptr, L"d3d11.dll"),
              "and the fixes still install");
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

static Survey scratchSurvey(const std::wstring& gameDir) {
    Survey s = baseSurvey(gameDir);
    s.d3d11 = fakeDll(DllKind::Absent, joinPath(gameDir, L"d3d11.dll"), "");
    s.openvrCurrent =
        fakeDll(DllKind::OpenVrRuntime, joinPath(s.game.openvrDir, L"openvr_api.dll"), "game-vr");
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

    {   // Install, then uninstall, and the folder should be as it started.
        layOutScratchGame(gameDir);
        Survey s = scratchSurvey(gameDir);
        check(applyPlan(planInstall(s, options, payload), provider(false)).ok, "installed");

        s.d3d11 = fakeDll(DllKind::Edvr, joinPath(gameDir, L"d3d11.dll"), payload.d3d11Sha);
        s.openvrCurrent = fakeDll(DllKind::Edvr, joinPath(s.game.openvrDir, L"openvr_api.dll"),
                                  payload.openvrSha);
        s.openvrOrig = fakeDll(DllKind::OpenVrRuntime,
                               joinPath(s.game.openvrDir, L"openvr_api_orig.dll"), "game-vr");
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
        // was written -- and that blocks a plan outright. This case is about
        // the ini merge over a folder a previous run really wrote.
        again.gameRunning = false;
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
    testProbe(scratch);
    testState();

    printf("\n%s\n", g_fails == 0 ? "installer_test: all good" : "installer_test: FAILURES");
    return g_fails == 0 ? 0 : 1;
}
