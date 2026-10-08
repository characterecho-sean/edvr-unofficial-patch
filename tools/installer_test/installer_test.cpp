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
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#include "../../src/common/iniedit.h"
#include "../../src/installer/apply.h"
#include "../../src/installer/plan.h"
#include "../../src/installer/probe.h"
#include "../../src/installer/logbundle.h"
#include "native_contract_cases.h"
#include "native_apply_cases.h"
#include "../../src/installer/mirror.h"
#include "../../src/installer/settings.h"

using namespace edvr::installer;
// The ini merge lives in common now (src/common/iniedit.h, shared with the
// in-headset menu), in the plain edvr namespace.
using namespace edvr;

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
    // Somebody else having the file open for a moment is a sharing violation here,
    // or an access denial where they had just deleted it: the real-time scanner and
    // the search indexer look at every file that was just written, and a helper
    // that gave up on the first one left the OLD bytes in the file for the case
    // that followed to trip over ("the original is exactly as it was -- got ''",
    // three runs in twenty under a stand-in scanner). Tried again for up to two
    // seconds; any other failure -- no such folder, a read-only file -- is final.
    HANDLE f = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 400; ++i) {
        f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) break;
        const DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED &&
            error != ERROR_LOCK_VIOLATION)
            break;
        Sleep(5);
    }
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
    p.descriptorText = "[install]\r\nschema = 1\r\nprofile = vr\r\n";
    p.descriptorSha = sha256Bytes(p.descriptorText.data(), p.descriptorText.size());
    p.haveD3d11 = true;
    p.d3d11Sha = "aaaa-new-d3d11";
    p.haveOpenvr = true;
    p.openvrSha = "bbbb-new-openvr";
    p.nativePairValid = true;
    p.haveOpenxrLoader = true;
    p.openxrLoaderSha = "cccc-loader";
    p.haveOpenxrLicense = true;
    p.openxrLicenseSha = "dddd-license";
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
    s.eliteKind = EliteExeKind::OdysseyQualified;
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
        // The field-of-view trims (2026-09-29) were [fix] keys with menu rows
        // and are ini-only [experimental] keys now: a per-headset list tuned
        // for one headset and one for another, and one left as shipped (empty).
        "fov_trim_vertical = pimax-openxr/pimax-crystal-super:10, virtualdesktopxr/meta-quest-3:5\r\n"
        "fov_trim_outer = oculus/meta-quest-3:7\r\n"
        "fov_trim_nasal =\r\n"
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

    // The trims, against the same real file: the tuned lists arrive whole under
    // [experimental] (commas, slashes, colons and all), the one left empty
    // stays empty there, and nothing is left under [fix] to shadow them.
    const std::string tunedTrims =
        "pimax-openxr/pimax-crystal-super:10, virtualdesktopxr/meta-quest-3:5";
    expectEq(iniValue(migrated, "experimental.fov_trim_vertical", "<absent>"), tunedTrims,
             "a tuned per-headset trim list follows the section move whole");
    expectEq(iniValue(migrated, "experimental.fov_trim_outer", "<absent>"),
             "oculus/meta-quest-3:7", "and so does a single entry");
    expectEq(iniValue(migrated, "experimental.fov_trim_nasal", "<absent>"), "",
             "a trim left as shipped (empty) stays empty under its new name");
    for (const char* key : {"fix.fov_trim_vertical", "fix.fov_trim_outer", "fix.fov_trim_nasal"}) {
        expectEq(iniValue(migrated, key, "<absent>"), "<absent>",
                 (std::string("nothing is left under ") + key + " for the reader to shadow").c_str());
    }

    // CONTROL: the same merge over the shipped file with its three moved-from
    // annotations taken out (what a careless edit of the block would leave). The
    // tuned list is NOT under [experimental]; it is carried under [fix], with a
    // note, where nothing reads it -- which is what the assertions above would
    // report if the annotations were lost.
    std::string unannotated = shipped;
    for (const char* key : {"vertical", "outer", "nasal"}) {
        const std::string line = std::string("# moved-from: fix.fov_trim_") + key;
        const size_t at = unannotated.find(line);
        if (at != std::string::npos) unannotated.replace(at, line.size(), "# (annotation removed)");
    }
    check(unannotated != shipped && unannotated.find("moved-from: fix.fov_trim_") == std::string::npos,
          "the control's ini really lost its three annotations");
    MergeReport strandedRep;
    const std::string stranded = mergeIni(unannotated, oldLayout, nullptr, {}, &strandedRep);
    check(iniValue(stranded, "experimental.fov_trim_vertical", "<absent>") != tunedTrims,
          "control: without the annotation the tuned trim list does NOT arrive under [experimental]");
    expectEq(iniValue(stranded, "fix.fov_trim_vertical", "<absent>"), tunedTrims,
             "control: it is carried under the old name instead, where nothing reads it");

    // 2026-09-29: the flat camera path became always-on and its two switches,
    // fix.temporal_aa_camera and fix.temporal_aa_camera_trace, were removed. A
    // rig that flew the experiment has both lines, one or both "on". Neither may
    // be adopted (the shipped file no longer documents either, so nothing reads
    // them), neither may be eaten (a line somebody put there is how a support
    // thread starts), and neither may disturb the settings around them. The
    // base is the shipped file plus the two blocks the previous version carried
    // right after temporal_aa_model.
    check(shipped.find("temporal_aa_camera") == std::string::npos,
          "the shipped ini documents neither retired camera-path switch");
    // The checkout's line endings are whatever git gave it (LF here, CRLF on a
    // machine with autocrlf), so the fixture takes them from the file.
    const std::string eol = shipped.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::string anchor = "temporal_aa_model = k" + eol;
    const size_t at = shipped.find(anchor);
    check(at != std::string::npos, "the shipped ini still has the line the fixture anchors on");
    if (at != std::string::npos) {
        const size_t after = at + anchor.size();
        const std::string previous = shipped.substr(0, after) + eol +
            "# ui: Camera-path jitter (experimental) | choices off, on | live | menu performance" + eol +
            "temporal_aa_camera = off" + eol + eol +
            "# ui: Camera-path jitter trace logging (experimental) | choices off, on | live | menu performance" + eol +
            "temporal_aa_camera_trace = off" + eol +
            shipped.substr(after);
        std::string flown = previous;
        flown.replace(flown.find("temporal_aa_camera = off"), strlen("temporal_aa_camera = off"),
                      "temporal_aa_camera = on");
        flown.replace(flown.find("temporal_aa_camera_trace = off"),
                      strlen("temporal_aa_camera_trace = off"), "temporal_aa_camera_trace = on");
        const size_t modelAt = flown.find("temporal_aa_model = k");
        flown.replace(modelAt, strlen("temporal_aa_model = k"), "temporal_aa_model = l");

        MergeReport camRep;
        const std::string camMerged = mergeIni(shipped, flown, &previous, {}, &camRep);
        expectEq(iniValue(camMerged, "fix.temporal_aa_model"), "l",
                 "the setting next to the retired pair keeps its tuned value");
        expectEq(iniValue(camMerged, "fix.temporal_aa_camera"), "on",
                 "a retired camera-path switch is carried with its value, not eaten");
        expectEq(iniValue(camMerged, "fix.temporal_aa_camera_trace"), "on",
                 "...and so is the trace switch");
        check(camRep.retired.size() == 2 && camRep.carried.empty(),
              "both are reported as retired settings, neither as an unknown key");
        check(camMerged.find("temporal_aa_camera = on") != std::string::npos &&
                  camMerged.find("# carried over from your edvr.ini; this version no longer uses it") !=
                      std::string::npos,
              "each carried line says this version no longer uses it");
        check(camMerged.find("# ui: Camera-path jitter") == std::string::npos,
              "the retired settings' menu rows are not resurrected");

        // The note names the file the user's text came from. The flat edition merges its own
        // edvr-flat.ini (2026-09-30: a flat install read edvr-flat.ini while every message said
        // edvr.ini), so the caller says which, and the default stays the VR file's name.
        const std::string flatMerged = mergeIni(shipped, flown, &previous, {}, nullptr, "edvr-flat.ini");
        check(flatMerged.find("# carried over from your edvr-flat.ini; this version no longer uses it") !=
                      std::string::npos &&
                  flatMerged.find("carried over from your edvr.ini") == std::string::npos,
              "a merge of edvr-flat.ini says the retired line was carried over from edvr-flat.ini");
        const std::string flatBare = mergeIni(shipped, flown, nullptr, {}, nullptr, "edvr-flat.ini");
        check(flatBare.find("# carried over from your edvr-flat.ini; not an EDVR setting this version knows") !=
                  std::string::npos,
              "...and so does the note for a key this version never shipped");
        check(mergeIni(shipped, flown, nullptr, {}, nullptr, nullptr)
                      .find("# carried over from your edvr.ini; not an EDVR setting this version knows") !=
                  std::string::npos,
              "no file named: the VR file's name, as before");

        // Hand-installed, no base copy: the same two lines are still carried and
        // still inert, only the note differs (the merge cannot know they once
        // shipped).
        MergeReport bareRep;
        const std::string bare = mergeIni(shipped, flown, nullptr, {}, &bareRep);
        expectEq(iniValue(bare, "fix.temporal_aa_camera"), "on",
                 "with no base copy a retired switch is still carried");
        check(bareRep.carried.size() == 2,
              "and reported as a key this version never shipped");

        // The merge is idempotent on the carried lines: merging again with the
        // result as the user's file must not duplicate them.
        MergeReport again;
        const std::string twice = mergeIni(shipped, camMerged, &shipped, {}, &again);
        size_t seen = 0, from = 0;
        while ((from = twice.find("temporal_aa_camera = on", from)) != std::string::npos) {
            ++seen;
            from += 1;
        }
        check(seen == 1, "a second merge does not duplicate a carried line",
              std::to_string(seen) + " copies");
    }

    // 2026-10-01: advanced.terrain_motion retired (terrain takes the camera's motion; the per-patch hook is gone). A rig that
    // flew the lever's A/B has `terrain_motion = off` live under [advanced]. It is carried with its value, not eaten and
    // not adopted; it is reported as retired; the setting beside it keeps its tuned value; the lever's documentation block
    // is not resurrected. A rig that never touched it (the line still commented) carries nothing at all. At run time the
    // config audit names a carried line in the log (config.cpp: "name settings this build does not read ... a retired setting").
    check(shipped.find("terrain_motion") == std::string::npos,
          "the shipped ini no longer documents the terrain motion lever");
    {
        const std::string terrainAnchor = "#temporal_aa_diagnostics = 0" + eol;
        const size_t terrainAt = shipped.find(terrainAnchor);
        check(terrainAt != std::string::npos, "the shipped ini still has the line the terrain-motion fixture anchors on");
        if (terrainAt != std::string::npos) {
            const size_t terrainAfter = terrainAt + terrainAnchor.size();
            // The previous version's file: the shipped one with the lever's block put back where it sat.
            const std::string previous = shipped.substr(0, terrainAfter) + eol +
                "# The motion DLSS, TAA and FSR are given for planetary terrain (every terrain" + eol +
                "# patch's own transform, recorded at its draw). Off hands those pixels the" + eol +
                "# camera's motion instead, which ghosts and smears on terrain that moves" + eol +
                "# against the camera -- a lever for pricing the hook against the frame time" + eol +
                "# over terrain, not a setting to fly with. Live." + eol +
                "#terrain_motion = on" + eol +
                shipped.substr(terrainAfter);
            std::string flown = previous;
            flown.replace(flown.find("#terrain_motion = on"), strlen("#terrain_motion = on"), "terrain_motion = off");
            flown.replace(flown.find("#temporal_aa_diagnostics = 0"), strlen("#temporal_aa_diagnostics = 0"),
                          "temporal_aa_diagnostics = 1");

            MergeReport terrainRep;
            const std::string terrainMerged = mergeIni(shipped, flown, &previous, {}, &terrainRep);
            expectEq(iniValue(terrainMerged, "advanced.temporal_aa_diagnostics"), "1",
                     "the setting next to the retired lever keeps its tuned value");
            expectEq(iniValue(terrainMerged, "advanced.terrain_motion"), "off",
                     "a retired terrain motion lever is carried with its value, not eaten");
            check(terrainRep.retired.size() == 1 && terrainRep.carried.empty(),
                  "it is reported as a retired setting, not as a key this version never shipped");
            check(terrainMerged.find("# carried over from your edvr.ini; this version no longer uses it\n" +
                                     std::string("terrain_motion = off")) != std::string::npos ||
                      terrainMerged.find("# carried over from your edvr.ini; this version no longer uses it\r\n" +
                                         std::string("terrain_motion = off")) != std::string::npos,
                  "the carried line follows a note saying this version no longer uses it");
            check(terrainMerged.find("planetary terrain (every terrain") == std::string::npos,
                  "the retired lever's documentation block is not resurrected");

            // Hand-installed, no base copy: still carried, still inert; only the note differs.
            MergeReport bareTerrain;
            const std::string bareMerged = mergeIni(shipped, flown, nullptr, {}, &bareTerrain);
            expectEq(iniValue(bareMerged, "advanced.terrain_motion"), "off", "with no base copy the lever is still carried");
            check(bareTerrain.carried.size() == 1, "and reported as a key this version never shipped");

            // The merge is idempotent on the carried line.
            MergeReport againTerrain;
            const std::string twiceTerrain = mergeIni(shipped, terrainMerged, &shipped, {}, &againTerrain);
            size_t copies = 0, from = 0;
            while ((from = twiceTerrain.find("terrain_motion = off", from)) != std::string::npos) {
                ++copies;
                from += 1;
            }
            check(copies == 1, "a second merge does not duplicate the carried lever", std::to_string(copies) + " copies");

            // A rig that never touched the lever has its line commented: nothing is carried and the new file stands.
            MergeReport untouchedTerrain;
            const std::string untouchedMerged = mergeIni(shipped, previous, &previous, {}, &untouchedTerrain);
            check(untouchedMerged.find("terrain_motion") == std::string::npos && untouchedTerrain.retired.empty(),
                  "a rig that left the lever commented carries nothing: the new file stands as shipped");
        }
    }

    // 2026-10-01: three experimental switches retired, each fixed at the behaviour it chose. The VR world route always jitters the
    // world it owns (only the global experimental.temporal_aa_jitter off stops it): experimental.temporal_aa_on_foot_world_jitter.
    // The depth-checked steady detail is always on, in the VR world route and in flat: experimental.temporal_aa_on_foot_world_steady_detail.
    // The jitter phases always follow the upscale ratio: experimental.temporal_aa_jitter_follows_upscale. All three shipped as LIVE
    // lines under [experimental], so every install of the previous version has them written out, and a rig that flew an A/B has one
    // of them on the other value. Each line is carried with its value (a line somebody put there is how a support thread starts),
    // reported as retired and not as a key this version never shipped, and not adopted (the shipped file documents none of the three);
    // the setting beside each keeps its tuned value; no documentation block is resurrected; a second merge adds no copy.
    check(shipped.find("temporal_aa_on_foot_world_jitter") == std::string::npos &&
              shipped.find("temporal_aa_on_foot_world_steady_detail") == std::string::npos &&
              shipped.find("temporal_aa_jitter_follows_upscale") == std::string::npos,
          "the shipped ini documents none of the three retired experimental switches");
    {
        // The previous version's file: the shipped one with the three blocks put back where each sat, right after its neighbour.
        const std::string afterJitter = "\ntemporal_aa_jitter = on" + eol;          // the jitter-phase switch followed it
        const std::string afterRoute = "\ntemporal_aa_on_foot_world = auto" + eol;  // the world jitter followed the route's key
        const std::string afterMaps = "\non_foot_maps_sharp = on" + eol;             // the steady detail followed the maps gate
        const size_t atJitter = shipped.find(afterJitter);
        const size_t atRoute = shipped.find(afterRoute);
        const size_t atMaps = shipped.find(afterMaps);
        const bool anchored = atJitter != std::string::npos && atRoute != std::string::npos && atMaps != std::string::npos &&
                              atJitter < atRoute && atRoute < atMaps;
        check(anchored, "the shipped ini still has the three lines the retired-switch fixture anchors on, in this order");
        if (anchored) {
            std::string previous = shipped;
            // Back to front, so the earlier offsets stay valid.
            previous.insert(atMaps + afterMaps.size(),
                eol + "# On foot: steadier fine detail on objects the game draws over (design doc" + eol +
                "# section 82, the depth-validated steady detail). on, the default, gives those" + eol +
                "# pixels the camera's motion instead, but only where last frame's depth confirms it." + eol +
                "# dev: choices on, off" + eol +
                "temporal_aa_on_foot_world_steady_detail = on" + eol);
            previous.insert(atRoute + afterRoute.size(),
                eol + "# VR on foot: jitter the world's own cameras while the world route owns the" + eol +
                "# world (design doc section 82, stage 2). on, the default, jitters the world" + eol +
                "# while the route owns it; off keeps the route and resolves an unjittered world." + eol +
                "# dev: choices on, off" + eol +
                "temporal_aa_on_foot_world_jitter = on" + eol);
            previous.insert(atJitter + afterJitter.size(),
                eol + "# TEMPORARY A/B SWITCH (2026-10-01): how many sub-pixel jitter phases the" + eol +
                "# jitter runs through before it repeats. off, the default, is the fixed eight." + eol +
                "temporal_aa_jitter_follows_upscale = off" + eol);

            // A user's file: the previous version's, with the setting beside each switch tuned and the switches at the given values.
            const auto setLine = [&](std::string text, const std::string& oldLine, const std::string& newLine) {
                const size_t p = text.find("\n" + oldLine + eol);
                if (p != std::string::npos) text.replace(p + 1, oldLine.size(), newLine);
                return text;
            };
            const auto userFile = [&](const char* jitter, const char* steady, const char* follows) {
                std::string text = setLine(previous, "temporal_aa_jitter = on", "temporal_aa_jitter = off");
                // Both keys ship on now (2026-10-01): the user who opted out of the route and the maps gate has them off.
                text = setLine(text, "temporal_aa_on_foot_world = auto", "temporal_aa_on_foot_world = off");
                text = setLine(text, "on_foot_maps_sharp = on", "on_foot_maps_sharp = off");
                text = setLine(text, "temporal_aa_on_foot_world_jitter = on",
                               std::string("temporal_aa_on_foot_world_jitter = ") + jitter);
                text = setLine(text, "temporal_aa_on_foot_world_steady_detail = on",
                               std::string("temporal_aa_on_foot_world_steady_detail = ") + steady);
                text = setLine(text, "temporal_aa_jitter_follows_upscale = off",
                               std::string("temporal_aa_jitter_follows_upscale = ") + follows);
                return text;
            };
            const auto copies = [](const std::string& text, const std::string& needle) {
                size_t n = 0, from = 0;
                while ((from = text.find(needle, from)) != std::string::npos) {
                    ++n;
                    from += 1;
                }
                return n;
            };
            const std::string carriedNote = "# carried over from your edvr.ini; this version no longer uses it";
            const auto carriedAs = [&](const std::string& merged, const std::string& line) {
                return merged.find(carriedNote + "\n" + line) != std::string::npos ||
                       merged.find(carriedNote + "\r\n" + line) != std::string::npos;
            };

            struct Variant { const char* what; const char* jitter; const char* steady; const char* follows; };
            const Variant variants[] = {
                {"the previous version's own values", "on", "on", "off"},
                {"a rig that flew the world jitter A/B and the jitter phases", "off", "on", "on"},
                {"a rig that flew the steady-detail A/B", "on", "off", "off"},
            };
            for (const Variant& v : variants) {
                const std::string tag = std::string(v.what) + ": ";
                const std::string jitterLine = std::string("temporal_aa_on_foot_world_jitter = ") + v.jitter;
                const std::string steadyLine = std::string("temporal_aa_on_foot_world_steady_detail = ") + v.steady;
                const std::string followsLine = std::string("temporal_aa_jitter_follows_upscale = ") + v.follows;
                MergeReport rep;
                const std::string switchMerged = mergeIni(shipped, userFile(v.jitter, v.steady, v.follows), &previous, {}, &rep);
                expectEq(iniValue(switchMerged, "experimental.temporal_aa_on_foot_world_jitter"), v.jitter,
                         (tag + "the retired world jitter switch is carried with its value, not eaten").c_str());
                expectEq(iniValue(switchMerged, "experimental.temporal_aa_on_foot_world_steady_detail"), v.steady,
                         (tag + "...and so is the steady-detail switch").c_str());
                expectEq(iniValue(switchMerged, "experimental.temporal_aa_jitter_follows_upscale"), v.follows,
                         (tag + "...and the jitter-phase switch").c_str());
                check(rep.retired.size() == 3 && rep.carried.empty(),
                      (tag + "all three are reported as retired settings, none as a key this version never shipped").c_str(),
                      std::to_string(rep.retired.size()) + " retired, " + std::to_string(rep.carried.size()) + " carried");
                check(carriedAs(switchMerged, jitterLine) && carriedAs(switchMerged, steadyLine) && carriedAs(switchMerged, followsLine),
                      (tag + "each carried line follows a note saying this version no longer uses it").c_str());
                expectEq(iniValue(switchMerged, "experimental.temporal_aa_on_foot_world"), "off",
                         (tag + "the route key beside the world jitter keeps its tuned value (off: opted out of the new default)").c_str());
                expectEq(iniValue(switchMerged, "experimental.on_foot_maps_sharp"), "off",
                         (tag + "the maps gate beside the steady detail keeps its tuned value (off: opted out of the new default)").c_str());
                expectEq(iniValue(switchMerged, "experimental.temporal_aa_jitter"), "off",
                         (tag + "the global jitter key beside the jitter-phase switch keeps its tuned value").c_str());
                check(switchMerged.find("steadier fine detail on objects") == std::string::npos &&
                          switchMerged.find("jitter the world's own cameras while the world route owns") == std::string::npos &&
                          switchMerged.find("TEMPORARY A/B SWITCH") == std::string::npos,
                      (tag + "the retired switches' documentation blocks are not resurrected").c_str());

                // The merge is idempotent on the carried lines: merging again with the result as the user's file must not duplicate them.
                MergeReport again;
                const std::string twice = mergeIni(shipped, switchMerged, &shipped, {}, &again);
                check(copies(twice, jitterLine) == 1 && copies(twice, steadyLine) == 1 && copies(twice, followsLine) == 1,
                      (tag + "a second merge does not duplicate a carried line").c_str(),
                      std::to_string(copies(twice, jitterLine)) + "/" + std::to_string(copies(twice, steadyLine)) + "/" +
                          std::to_string(copies(twice, followsLine)) + " copies");
            }

            // Hand-installed, no base copy: the same three lines are still carried and still inert, only the note differs.
            MergeReport bareRep;
            const std::string bare = mergeIni(shipped, userFile("off", "off", "on"), nullptr, {}, &bareRep);
            check(iniValue(bare, "experimental.temporal_aa_on_foot_world_jitter") == "off" &&
                      iniValue(bare, "experimental.temporal_aa_on_foot_world_steady_detail") == "off" &&
                      iniValue(bare, "experimental.temporal_aa_jitter_follows_upscale") == "on",
                  "with no base copy the three retired switches are still carried with their values");
            check(bareRep.carried.size() == 3 && bareRep.retired.empty(),
                  "and reported as keys this version never shipped (the merge cannot know they once did)");

            // A file that has none of the lines (installed from this version, or the lines deleted) carries nothing: the new file stands.
            MergeReport freshRep;
            const std::string fresh = mergeIni(shipped, shipped, &previous, {}, &freshRep);
            check(fresh == shipped && freshRep.retired.empty() && freshRep.carried.empty(),
                  "a file without the three lines carries nothing: the new file stands as shipped");
        }
    }

    // 2026-10-08: the settlement LOD governor and the static prop gate removed, four keys: fix.settlement_detail (game | auto |
    // reduced; shipped LIVE as `game` in [fix], so every install of the previous version has the line written out),
    // advanced.settlement_detail_max and advanced.settlement_detail_observe (commented templates in [advanced]) and
    // fix.static_prop_updates (a commented template in [fix]). The game's own detail and its own prop updates are what run now. A
    // line somebody set is carried with its value, reported as retired and not as a key this version never shipped, and not
    // adopted; the setting beside each keeps its tuned value; no documentation block is resurrected; a second merge adds no copy;
    // a rig that left the templates commented carries only the live settlement_detail line. At run time the config audit names a
    // carried line in the log (config.cpp: "name settings this build does not read ... a retired setting").
    check(shipped.find("settlement_detail") == std::string::npos && shipped.find("static_prop_updates") == std::string::npos,
          "the shipped ini documents none of the four removed settlement detail and static prop keys");
    {
        // The previous version's file: the shipped one with the two [fix] blocks put back after wake_pulse and the two [advanced]
        // templates after the pixel probe, where each sat.
        const std::string afterWake = "\nwake_pulse = off" + eol;
        const std::string afterProbe = "\n#pixel_probe =" + eol;
        const size_t atWake = shipped.find(afterWake);
        const size_t atProbe = shipped.find(afterProbe);
        const bool anchored = atWake != std::string::npos && atProbe != std::string::npos && atWake < atProbe;
        check(anchored, "the shipped ini still has the two lines the removed-settlement-detail fixture anchors on, in this order");
        if (anchored) {
            std::string previous = shipped;
            // Back to front, so the earlier offset stays valid.
            previous.insert(atProbe + afterProbe.size(),
                eol + "# How far fix.settlement_detail may move the game's level-of-detail" + eol +
                "# distance, as a factor on the LOD scale the game holds: 1 changes nothing. Live." + eol +
                "#settlement_detail_max = 6.0" + eol +
                eol + "# Measurement mode for fix.settlement_detail (auto or reduced): 1 works out" + eol +
                "# the factor and logs what it would drop. 0, the default, lets the setting act. Live." + eol +
                "#settlement_detail_observe = 0" + eol);
            previous.insert(atWake + afterWake.size(),
                eol + "# Stop the engine re-composing settlement structures and props that have not" + eol +
                "# changed. 1 on, 0 off (the shipped state). Live." + eol +
                "#static_prop_updates = 0" + eol +
                eol + "# Settlement detail: a higher frame rate at busy settlements." + eol +
                "#   game    -- the default: the game's own detail, untouched." + eol +
                "# ui: Settlement detail | choices game, auto=Auto, reduced | live | menu performance" + eol +
                "settlement_detail = game" + eol);

            const auto setLine = [&](std::string text, const std::string& oldLine, const std::string& newLine) {
                const size_t p = text.find("\n" + oldLine + eol);
                if (p != std::string::npos) text.replace(p + 1, oldLine.size(), newLine);
                return text;
            };
            const auto copies = [](const std::string& text, const std::string& needle) {
                size_t n = 0, from = 0;
                while ((from = text.find(needle, from)) != std::string::npos) {
                    ++n;
                    from += 1;
                }
                return n;
            };
            const std::string carriedNote = "# carried over from your edvr.ini; this version no longer uses it";
            const auto carriedAs = [&](const std::string& merged, const std::string& line) {
                return merged.find(carriedNote + "\n" + line) != std::string::npos ||
                       merged.find(carriedNote + "\r\n" + line) != std::string::npos;
            };
            // The settings beside the blocks, tuned, to show the merge leaves them alone.
            const auto tuned = [&](std::string text) {
                text = setLine(text, "wake_pulse = off", "wake_pulse = stock");
                text = setLine(text, "#pixel_probe =", "pixel_probe = 0.5,0.5");
                return text;
            };

            // 1. The previous version's own file: only the live settlement_detail line is there to carry.
            {
                MergeReport rep;
                const std::string merged = mergeIni(shipped, tuned(previous), &previous, {}, &rep);
                expectEq(iniValue(merged, "fix.settlement_detail"), "game",
                         "the previous version's own settlement detail line is carried, not eaten");
                check(rep.retired.size() == 1 && rep.carried.empty(),
                      "...reported as a retired setting, not as a key this version never shipped",
                      std::to_string(rep.retired.size()) + " retired, " + std::to_string(rep.carried.size()) + " carried");
                check(carriedAs(merged, "settlement_detail = game"),
                      "...after a note saying this version no longer uses it");
                expectEq(iniValue(merged, "fix.wake_pulse"), "stock", "the setting before the removed blocks keeps its tuned value");
                expectEq(iniValue(merged, "advanced.pixel_probe"), "0.5,0.5", "...and so does the one after them");
                check(iniValue(merged, "advanced.settlement_detail_max", "<unset>") == "<unset>" &&
                          iniValue(merged, "advanced.settlement_detail_observe", "<unset>") == "<unset>" &&
                          iniValue(merged, "fix.static_prop_updates", "<unset>") == "<unset>",
                      "the three commented templates carry nothing");
            }

            // 2. A rig that flew all four: every line is carried with its value.
            {
                std::string flown = tuned(previous);
                flown = setLine(flown, "settlement_detail = game", "settlement_detail = auto");
                flown = setLine(flown, "#static_prop_updates = 0", "static_prop_updates = 1");
                flown = setLine(flown, "#settlement_detail_max = 6.0", "settlement_detail_max = 4.5");
                flown = setLine(flown, "#settlement_detail_observe = 0", "settlement_detail_observe = 1");
                MergeReport rep;
                const std::string merged = mergeIni(shipped, flown, &previous, {}, &rep);
                expectEq(iniValue(merged, "fix.settlement_detail"), "auto", "a flown settlement detail setting is carried with its value");
                expectEq(iniValue(merged, "fix.static_prop_updates"), "1", "...and so is the static prop gate's");
                expectEq(iniValue(merged, "advanced.settlement_detail_max"), "4.5", "...and the ceiling");
                expectEq(iniValue(merged, "advanced.settlement_detail_observe"), "1", "...and the observe switch");
                check(rep.retired.size() == 4 && rep.carried.empty(),
                      "all four are reported as retired settings, none as a key this version never shipped",
                      std::to_string(rep.retired.size()) + " retired, " + std::to_string(rep.carried.size()) + " carried");
                check(carriedAs(merged, "settlement_detail = auto") && carriedAs(merged, "static_prop_updates = 1") &&
                          carriedAs(merged, "settlement_detail_max = 4.5") && carriedAs(merged, "settlement_detail_observe = 1"),
                      "each carried line follows a note saying this version no longer uses it");
                expectEq(iniValue(merged, "fix.wake_pulse"), "stock", "the setting before the removed blocks keeps its tuned value");
                expectEq(iniValue(merged, "advanced.pixel_probe"), "0.5,0.5", "...and so does the one after them");
                check(merged.find("Stop the engine re-composing") == std::string::npos &&
                          merged.find("a higher frame rate at busy settlements") == std::string::npos &&
                          merged.find("# ui: Settlement detail") == std::string::npos &&
                          merged.find("How far fix.settlement_detail may move") == std::string::npos &&
                          merged.find("Measurement mode for fix.settlement_detail") == std::string::npos,
                      "the removed settings' documentation blocks and menu row are not resurrected");

                // The merge is idempotent on the carried lines.
                MergeReport again;
                const std::string twice = mergeIni(shipped, merged, &shipped, {}, &again);
                check(copies(twice, "settlement_detail = auto") == 1 && copies(twice, "static_prop_updates = 1") == 1 &&
                          copies(twice, "settlement_detail_max = 4.5") == 1 && copies(twice, "settlement_detail_observe = 1") == 1,
                      "a second merge does not duplicate a carried line");

                // Hand-installed, no base copy: the same four lines are still carried and still inert, only the note differs.
                MergeReport bareRep;
                const std::string bare = mergeIni(shipped, flown, nullptr, {}, &bareRep);
                check(iniValue(bare, "fix.settlement_detail") == "auto" && iniValue(bare, "fix.static_prop_updates") == "1" &&
                          iniValue(bare, "advanced.settlement_detail_max") == "4.5" &&
                          iniValue(bare, "advanced.settlement_detail_observe") == "1",
                      "with no base copy the four removed settings are still carried with their values");
                check(bareRep.carried.size() == 4 && bareRep.retired.empty(),
                      "and reported as keys this version never shipped (the merge cannot know they once did)");
            }

            // A file with none of the lines (installed from this version, or the lines deleted) carries nothing.
            MergeReport freshRep;
            const std::string fresh = mergeIni(shipped, shipped, &previous, {}, &freshRep);
            check(fresh == shipped && freshRep.retired.empty() && freshRep.carried.empty(),
                  "a file without the four lines carries nothing: the new file stands as shipped");
        }
    }
}

// A shipped default that CHANGED, against the real edvr.ini: fix.ui_quality went
// from off to 100 on 2026-09-29. What an existing install does with it is a
// property of the merge, not of this key, and it is pinned here so a change of that
// behaviour is a decision. The previous version's file is the shipped one with the
// old default written back.
static void testChangedDefault(const std::wstring& root) {
    printf("\na shipped default that changed (fix.ui_quality: off -> 100), against the real edvr.ini\n");
    const std::string shipped = readAll(joinPath(root, L"edvr.ini"));
    if (shipped.empty()) {
        fail("read the repository's edvr.ini", "not found next to the repo root");
        return;
    }
    expectEq(iniValue(shipped, "fix.ui_quality", "<absent>"), "100", "the shipped default is 100");
    std::string previous = shipped;
    const size_t at = previous.find("\nui_quality = 100");
    check(at != std::string::npos, "the shipped ini has the line this case reverts");
    if (at == std::string::npos) return;
    previous.replace(at, strlen("\nui_quality = 100"), "\nui_quality = off");
    expectEq(iniValue(previous, "fix.ui_quality", "<absent>"), "off", "the previous version's file ships off");

    // An install that never touched it, with the base copy the installer keeps: the
    // new default is adopted, and the report says so. Somebody who typed `off` on
    // purpose is the same bytes and gets the same answer: the merge cannot tell.
    MergeReport untouched;
    const std::string adopted = mergeIni(shipped, previous, &previous, {}, &untouched);
    expectEq(iniValue(adopted, "fix.ui_quality", "<absent>"), "100",
             "an install that never touched it (base copy kept) is moved to the new default");
    bool reported = false;
    for (const std::string& line : untouched.adopted) reported |= line.find("fix.ui_quality = 100") == 0;
    check(reported, "and the report says the default moved");

    // With no base copy (a hand-installed rig): compared against the NEW defaults, so
    // the old default reads as a choice and is kept. The merge over-preserves.
    MergeReport handInstalled;
    const std::string kept = mergeIni(shipped, previous, nullptr, {}, &handInstalled);
    expectEq(iniValue(kept, "fix.ui_quality", "<absent>"), "off",
             "with no base copy the same file keeps its off");
    check(handInstalled.twoWay, "and the report says it compared against the new defaults");

    // A value they chose is theirs either way.
    std::string chose125 = previous;
    chose125.replace(chose125.find("\nui_quality = off"), strlen("\nui_quality = off"), "\nui_quality = 125");
    MergeReport chosen;
    expectEq(iniValue(mergeIni(shipped, chose125, &previous, {}, &chosen), "fix.ui_quality", "<absent>"),
             "125", "a value somebody chose (125) survives");

    // A line they deleted stays deleted (commented out): the runtime then uses the
    // code's fallback, which is the shipped default (tools/config_test checks it).
    std::string deleted = previous;
    deleted.replace(deleted.find("\nui_quality = off"), strlen("\nui_quality = off"), "\n#ui_quality = off");
    MergeReport removed;
    expectEq(iniValue(mergeIni(shipped, deleted, &previous, {}, &removed), "fix.ui_quality", "<absent>"),
             "<absent>", "a line they commented out stays commented out");
}

// 2026-10-01: two experimental defaults flipped on, the VR world route (experimental.temporal_aa_on_foot_world: off -> auto) and the
// on-foot maps gate (experimental.on_foot_maps_sharp: off -> on), the keys kept for one release candidate as the way back. What an
// existing install does with them is the merge's property, as for fix.ui_quality above, and it is pinned for both keys so that what an
// upgrading user sees is on record: a line that still says what the previous version shipped (the installer's base copy kept) moves to
// the new default and the report says so; a value somebody chose, a line they deleted and a hand-installed file with no base copy keep
// what they have. The previous version's file is the shipped one with the two old defaults written back.
static void testChangedDefaultsOn(const std::wstring& root) {
    printf("\nshipped defaults that flipped on (the VR world route off -> auto, the on-foot maps gate off -> on), against the real edvr.ini\n");
    const std::string shipped = readAll(joinPath(root, L"edvr.ini"));
    if (shipped.empty()) {
        fail("read the repository's edvr.ini", "not found next to the repo root");
        return;
    }
    struct Flip {
        const char* line;      // the key's line in the ini, as the previous version spelled its value
        const char* dotted;    // the key as the merge names it
        const char* newValue;  // what this version ships
    };
    const Flip flips[] = {
        {"temporal_aa_on_foot_world", "experimental.temporal_aa_on_foot_world", "auto"},
        {"on_foot_maps_sharp", "experimental.on_foot_maps_sharp", "on"},
    };
    std::string previous = shipped;
    bool anchored = true;
    for (const Flip& f : flips) {
        expectEq(iniValue(shipped, f.dotted, "<absent>"), f.newValue,
                 (std::string("the shipped default of ") + f.dotted + " is " + f.newValue).c_str());
        const std::string now = std::string("\n") + f.line + " = " + f.newValue;
        const size_t at = previous.find(now);
        if (at == std::string::npos) { anchored = false; continue; }
        previous.replace(at, now.size(), std::string("\n") + f.line + " = off");
    }
    check(anchored, "the shipped ini has the two lines this case reverts");
    if (!anchored) return;

    // An install that never touched them, with the base copy the installer keeps: the new defaults are adopted, and the report
    // says so. Somebody who typed `off` on purpose is the same bytes and gets the same answer: the merge cannot tell.
    MergeReport untouched;
    const std::string adopted = mergeIni(shipped, previous, &previous, {}, &untouched);
    for (const Flip& f : flips) {
        expectEq(iniValue(adopted, f.dotted, "<absent>"), f.newValue,
                 (std::string("an install that never touched ") + f.dotted + " (base copy kept) is moved to the new default").c_str());
        bool reported = false;
        for (const std::string& line : untouched.adopted)
            reported |= line.find(std::string(f.dotted) + " = " + f.newValue) == 0;
        check(reported, (std::string("and the report says the default of ") + f.dotted + " moved").c_str());
    }

    // With no base copy (a hand-installed rig): compared against the NEW defaults, so the old default reads as a choice and is
    // kept. The merge over-preserves; the key is the way back, and this is a rig that had written the old default out.
    MergeReport handInstalled;
    const std::string kept = mergeIni(shipped, previous, nullptr, {}, &handInstalled);
    for (const Flip& f : flips)
        expectEq(iniValue(kept, f.dotted, "<absent>"), "off",
                 (std::string("with no base copy the same file keeps its off for ") + f.dotted).c_str());
    check(handInstalled.twoWay, "and the report says it compared against the new defaults");

    // A line they deleted stays deleted (commented out), and the runtime then uses the code's fallback, which is the shipped
    // default (tools/config_test checks it); the other key, left alone, still moves.
    std::string deletedRoute = previous;
    deletedRoute.replace(deletedRoute.find("\ntemporal_aa_on_foot_world = off"), strlen("\ntemporal_aa_on_foot_world = off"),
                         "\n#temporal_aa_on_foot_world = off");
    MergeReport partial;
    const std::string mixed = mergeIni(shipped, deletedRoute, &previous, {}, &partial);
    expectEq(iniValue(mixed, "experimental.temporal_aa_on_foot_world", "<absent>"), "<absent>",
             "a route line they commented out stays commented out (the runtime then uses the code's fallback, auto)");
    expectEq(iniValue(mixed, "experimental.on_foot_maps_sharp", "<absent>"), "on",
             "...and the maps gate they left alone still moves to the new default");
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
        s.descriptorPresent = true;
        s.descriptorSha = payload.descriptorSha;
        s.state.descriptorSha = payload.descriptorSha;
        s.openxrLoader=fakeDll(DllKind::Foreign,joinPath(s.game.openvrDir,L"openxr_loader.dll"),payload.openxrLoaderSha);
        s.openxrLicense=fakeDll(DllKind::Foreign,joinPath(s.game.openvrDir,L"OPENXR-LOADER-LICENSE.txt"),payload.openxrLicenseSha);
        const auto first=planInstall(s,options,payload);
        s.nativeConfig=fakeDll(DllKind::Foreign,joinPath(s.game.openvrDir,L"edvr_openxr.ini"),first.nextState.nativeConfigSha);
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
        check(notesMention(plan, "newer original"), "the report explains it");
    }

    {   // The worst case: ours is installed and the game's original is gone.
        Survey s = baseSurvey(dir);
        s.openvrCurrent =
            fakeDll(DllKind::Edvr, joinPath(s.game.openvrDir, L"openvr_api.dll"), "old-edvr");
        s.openvrOrig = fakeDll(DllKind::Absent,
                               joinPath(s.game.openvrDir, L"openvr_api_orig.dll"), "");
        const Plan plan = planInstall(s, options, payload);
        check(!plan.blocked && hasStep(plan, Action::WritePayload, nullptr, L"openvr_api.dll"),
              "native update proceeds without an original runtime dependency");
        check(!hasStep(plan,Action::Rename,L"openvr_api.dll",L"openvr_api_orig.dll"),
              "an old EDVR DLL is never preserved as a stock original");
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
        check(!hasStep(plan, Action::Backup, L"openvr_api.dll", L"openvr_api_orig.dll"),
              "native startup does not copy a legacy runtime back into use");
        check(hasStep(plan, Action::WritePayload, nullptr, L"openvr_api.dll"),
              "and EDVR is reinstalled in front of it");
    }

    {   // No Openvr folder: create it and install the complete native package.
        Survey s = baseSurvey(dir);
        s.haveOpenvrDir = false;
        s.game.openvrDir.clear();
        s.openvrCurrent=DllInfo{};
        const Plan plan = planInstall(s, options, payload);
        check(hasStep(plan, Action::WritePayload, nullptr, L"d3d11.dll"),
              "graphics installs in a fresh folder");
        check(hasStep(plan,Action::WritePayload,nullptr,L"openvr_api.dll") &&
              hasStep(plan,Action::WritePayload,nullptr,L"openxr_loader.dll"),
              "the native runtime and loader always accompany graphics");
    }


    {   // openvrOrigName can be anything a caller puts on the Survey struct.
        // A value with a path in it would move the game's runtime out of the
        // folder and over another mod, while the report said "the game's own
        // copy is renamed openvr_api_orig.dll". Anything that is not a plain
        // filename is refused back to the default.
        Survey nested = baseSurvey(dir);
        nested.openvrOrigName = L"sub\\somewhere.dll";
        const Plan nestedPlan = planInstall(nested, options, payload);
        check(hasStep(nestedPlan, Action::Rename, L"openvr_api.dll", L"openvr_api_orig.dll"),
              "a name with a folder in it is refused back to the default name");

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

    {   // The process list could not be read. That is not "the game is stopped":
        // it used to be, and the installer went on into a folder the game may
        // have had open. Refused, in words of its own -- closing the game does
        // not cure a check that could not run.
        Survey s = baseSurvey(dir);
        s.gameRunStateUnknown = true;
        const Plan plan = planInstall(s, options, payload);
        check(plan.blocked && plan.steps.empty(),
              "nothing is planned when it cannot be told whether the game is running");
        check(notesMention(plan, "Could not tell whether Elite Dangerous is running"),
              "and the report says the check failed, not that the game is running");
        check(!notesMention(plan, "Elite Dangerous is running. Close it first"),
              "which is a different message from the one for a game that is");
        const Plan out = planUninstall(s, options);
        check(out.blocked && out.steps.empty(), "and nothing is taken back out either");
        check(notesMention(out, "Could not tell whether Elite Dangerous is running"),
              "with the same words on the way out");
        Options repair = options;
        repair.repair = true;
        check(planInstall(s, repair, payload).blocked, "a repair is refused the same way");
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
        check(plan.blocked && plan.steps.empty(), "a partial package cannot change any files");
    }

    {   // The elite gate, per case: the message names the game the folder
        // really holds, and only the pinned Odyssey revision gets through.
        Survey s = baseSurvey(dir);
        s.eliteKind = EliteExeKind::Legacy;
        Plan plan = planInstall(s, options, payload);
        check(plan.blocked && plan.steps.empty(), "legacy Elite Dangerous is refused");
        check(notesMention(plan, "Elite Dangerous (Horizons)"), "and named as Horizons, not Odyssey");
        check(notesMention(plan, "no EDVR build supports this game"),
              "with no pointer at some other build");

        s.eliteKind = EliteExeKind::OdysseyUnknown;
        s.eliteFileVersion = L"999999";
        plan = planInstall(s, options, payload);
        check(plan.blocked && plan.steps.empty(), "an unknown Odyssey revision is refused");
        check(notesMention(plan, "not one this EDVR build is qualified for"),
              "and the refusal says what is wrong");
        check(notesMention(plan, "999999"), "and names the revision the game reports");

        s.eliteKind = EliteExeKind::Unreadable;
        plan = planInstall(s, options, payload);
        check(plan.blocked && plan.steps.empty(), "an unreadable executable is refused");
        check(notesMention(plan, "could not be read"),
              "and the refusal says the executable could not be read");
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

static void testNativePlanner() {
    printf("\nnative planner\n");
    const std::wstring dir = L"C:\\Games\\ED\\Products\\elite-dangerous-odyssey-64";
    Options o = testOptions();
    PayloadInfo p; p.version="native-test"; p.iniText="[fix]\r\nblack_void = 1\r\n";
    p.descriptorText = "[install]\r\nschema = 1\r\nprofile = vr\r\n";
    p.descriptorSha = sha256Bytes(p.descriptorText.data(), p.descriptorText.size());
    p.nativePairValid=true; p.haveD3d11=true; p.haveOpenvr=true; p.haveOpenxrLoader=true; p.d3d11Sha="graphics";
    p.openvrSha="runtime"; p.openxrLoaderSha="loader";
    p.haveOpenxrLicense=true; p.openxrLicenseSha="license";
    Survey s=baseSurvey(dir); s.openxrLoader=fakeDll(DllKind::Absent,joinPath(s.game.openvrDir,L"openxr_loader.dll"),"");
    s.eliteKind=EliteExeKind::OdysseyQualified;
    Plan fresh=planInstall(s,o,p);
    check(!fresh.blocked,"complete native payload plans");
    check(hasStep(fresh,Action::WritePayload,nullptr,L"d3d11.dll"),"native graphics is installed beside Elite");
    check(hasStep(fresh,Action::WritePayload,nullptr,L"openvr_api.dll"),"native runtime is installed in Openvr");
    check(hasStep(fresh,Action::WritePayload,nullptr,L"openxr_loader.dll"),"bundled Khronos loader is installed");
    check(hasStep(fresh,Action::WriteText,nullptr,L"edvr_openxr.ini"),"native startup config is written");
    bool systemConfig=false; for(const Step& st:fresh.steps) if(st.action==Action::WriteText && leafOf(st.to)==L"edvr_openxr.ini") systemConfig=st.text.find("runtime=system")!=std::string::npos;
    check(systemConfig,"config selects the system runtime");
    PayloadInfo half=p; half.nativePairValid=false;
    check(planInstall(s,o,half).blocked,"missing one native payload rejects the whole plan");
    s.openvrCurrent=fakeDll(DllKind::Foreign,joinPath(s.game.openvrDir,L"openvr_api.dll"),"old-opencomposite",L"OpenComposite");
    Plan upgrade=planInstall(s,o,p);
    check(!upgrade.blocked && hasStep(upgrade,Action::Backup,L"openvr_api.dll",L"openvr_api.dll"),"upgrade preserves a stock or OpenComposite runtime");
    s.state.nativeInstalled=true; s.state.nativeRuntimeSha=p.openvrSha; s.state.nativeOriginalSha="old-opencomposite";
    s.openxrLoader=fakeDll(DllKind::Edvr,joinPath(s.game.openvrDir,L"openxr_loader.dll"),p.openxrLoaderSha);
    s.state.openxrLoaderSha=p.openxrLoaderSha; s.state.openxrLicenseSha=p.openxrLicenseSha;
    s.openvrCurrent=fakeDll(DllKind::Edvr,joinPath(s.game.openvrDir,L"openvr_api.dll"),p.openvrSha);
    s.openvrOrig=fakeDll(DllKind::OpenVrRuntime,joinPath(s.game.openvrDir,L"openvr_api_orig.dll"),"old-opencomposite");
    s.d3d11=fakeDll(DllKind::Edvr,joinPath(dir,L"d3d11.dll"),p.d3d11Sha);
    Plan out=planUninstall(s,o);
    check(hasStep(out,Action::Delete,L"openxr_loader.dll",nullptr),"uninstall removes the bundled loader");
    check(hasStep(out,Action::Rename,L"openvr_api_orig.dll",L"openvr_api.dll"),"uninstall restores the original runtime");
}

static void testFlatPlanner() {
    printf("\nflat profile planner\n");
    const std::wstring dir = L"C:\\Games\\ED\\Products\\elite-dangerous-odyssey-64";
    PayloadInfo flat = testPayload("[fix]\r\ntemporal_aa = off\r\n[advanced]\r\nreal_dll =\r\n");
    flat.profile = "flat";
    flat.descriptorText = "[install]\r\nschema = 1\r\nprofile = flat\r\n";
    flat.descriptorSha = sha256Bytes(flat.descriptorText.data(), flat.descriptorText.size());
    flat.nativeGraphicsValid = true;
    flat.haveOpenvr = false; flat.haveOpenxrLoader = false; flat.haveOpenxrLicense = false;
    flat.nativePairValid = false;
    Survey s = baseSurvey(dir);
    s.haveOpenvrDir = false;
    Options o = testOptions();
    Plan fresh = planInstall(s, o, flat);
    check(!fresh.blocked, "flat accepts graphics without the native runtime pair");
    check(hasStep(fresh, Action::WritePayload, nullptr, L"d3d11.dll"), "flat installs graphics");
    check(hasStep(fresh, Action::WritePayload, nullptr, L"edvr_profile.ini"), "flat installs its descriptor");
    check(!hasStep(fresh, Action::WritePayload, nullptr, L"openvr_api.dll"), "flat omits the VR runtime");
    bool makesOpenvr = false;
    for (const Step& step : fresh.steps)
        if (step.action == Action::MakeDir && leafOf(step.to) == L"win64") makesOpenvr = true;
    check(!makesOpenvr, "fresh flat does not create Openvr\\win64");

    Survey markerOnly = s;
    markerOnly.descriptorPresent = true;
    markerOnly.descriptorSha = flat.descriptorSha;
    check(installedProfile(markerOnly) == "flat", "canonical flat marker identifies edition without GUI state");
    PayloadInfo vr = testPayload("[fix]\r\ntemporal_aa = off\r\n");
    check(planInstall(markerOnly, o, vr).blocked,
          "VR installer cannot silently widen a descriptor-only flat install");
    Options explicitVr = o; explicitVr.convertProfile = true;
    Plan widened = planInstall(markerOnly, explicitVr, vr);
    check(!widened.blocked && hasStep(widened, Action::WritePayload, nullptr, L"openvr_api.dll"),
          "explicit descriptor-only flat to VR conversion installs the native runtime");
    check(widened.nextState.profile == "vr", "conversion records the VR edition");
    markerOnly.descriptorSha = "unrecognized-marker";
    check(planInstall(markerOnly, explicitVr, vr).blocked,
          "unknown descriptor is never treated as permission to install VR");

    s.d3d11 = fakeDll(DllKind::D3d11Provider, joinPath(dir, L"d3d11.dll"), "edhm", L"3Dmigoto");
    Plan chained = planInstall(s, o, flat);
    check(!chained.blocked && hasStep(chained, Action::Rename, L"d3d11.dll", L"d3d11_edhm.dll"),
          "flat uses the existing EDHM chain planner");

    s = baseSurvey(dir);
    s.state.present = true; s.state.profile = "vr";
    s.state.openvrInstalled = true; s.state.openvrSha = "installed-vr";
    s.state.openvrOrigSha = "game-vr";
    s.openvrCurrent = fakeDll(DllKind::Edvr, joinPath(s.game.openvrDir, L"openvr_api.dll"), "installed-vr");
    s.openvrOrig = fakeDll(DllKind::OpenVrRuntime, joinPath(s.game.openvrDir, L"openvr_api_orig.dll"), "game-vr");
    check(planInstall(s, o, flat).blocked, "edition switch needs explicit conversion");
    o.convertProfile = true;
    Plan conversion = planInstall(s, o, flat);
    check(!conversion.blocked, "owned VR to flat conversion plans");
    check(hasStep(conversion, Action::Rename, L"openvr_api_orig.dll", L"openvr_api.dll"),
          "conversion restores the game's original runtime");
    s.openvrOrig = fakeDll(DllKind::Absent, joinPath(s.game.openvrDir, L"openvr_api_orig.dll"), "");
    check(planInstall(s, o, flat).blocked, "missing original blocks VR to flat conversion");
}

// ---------------------------------------------------------------------------
// the flat edition's own settings file (edvr-flat.ini)
//
// The flat runtime reads edvr-flat.ini first and falls back to edvr.ini only
// while there is none (config.cpp). The installer used to merge into and edit
// edvr.ini for a flat install: it reported success while the game went on
// reading the old flat file, and where there was no flat file it changed the VR
// profile's tuning. Every case here holds one rule: an operation of an edition
// changes the file THAT edition's runtime reads, and the other edition's file
// is left byte for byte as it was.
// ---------------------------------------------------------------------------

// What each shipped edition's defaults look like, in miniature, and the two files
// a folder can hold: the VR profile's edvr.ini and the flat profile's own, chosen
// to differ from each other in every way a wrong-file merge could show.
static const char* kFlatTemplate =
    "# EDVR flat settings.\r\n"
    "\r\n"
    "[fix]\r\n"
    "temporal_aa = off\r\n"
    "temporal_aa_model = k\r\n"
    "\r\n"
    "[advanced]\r\n"
    "real_dll =\r\n";

// The same file a version later: one default moved and one setting arrived.
static const char* kFlatTemplateNewer =
    "# EDVR flat settings.\r\n"
    "\r\n"
    "[fix]\r\n"
    "temporal_aa = off\r\n"
    "temporal_aa_model = l\r\n"
    "render_sharpness = 0\r\n"
    "\r\n"
    "[advanced]\r\n"
    "real_dll =\r\n";

// The VR profile's edvr.ini -- or, in a folder from before the profiles had files
// of their own, the one file both read. It carries a section and a value that no
// flat file above has.
static const char* kSharedIni =
    "[fix]\r\n"
    "temporal_aa = dlaa\r\n"
    "temporal_aa_model = k\r\n"
    "black_void = 0\r\n"
    "\r\n"
    "[hotkey]\r\n"
    "menu = F5\r\n"
    "\r\n"
    "[advanced]\r\n"
    "real_dll =\r\n";

// The flat profile's own edvr-flat.ini, tuned differently.
static const char* kFlatOwnIni =
    "# EDVR flat settings.\r\n"
    "\r\n"
    "[fix]\r\n"
    "temporal_aa = fsr\r\n"
    "temporal_aa_model = k\r\n"
    "\r\n"
    "[advanced]\r\n"
    "real_dll =\r\n";

static const char kFlatGraphicsBytes[] = "TEST-FLAT-D3D11-PAYLOAD";

static PayloadInfo flatPayloadFor(const std::string& iniText) {
    PayloadInfo p = testPayload(iniText);
    p.profile = "flat";
    p.descriptorText = "[install]\r\nschema = 1\r\nprofile = flat\r\n";
    p.descriptorSha = sha256Bytes(p.descriptorText.data(), p.descriptorText.size());
    p.nativeGraphicsValid = true;
    p.haveOpenvr = false;
    p.haveOpenxrLoader = false;
    p.haveOpenxrLicense = false;
    p.nativePairValid = false;
    p.d3d11Sha = sha256Bytes(kFlatGraphicsBytes, sizeof(kFlatGraphicsBytes) - 1);
    return p;
}

// The payload the flat installer carries, for an apply on real files.
static PayloadProvider flatProvider() {
    return [](const std::string& item, const void** data, size_t* size) {
        static const char kProfile[] = "[install]\r\nschema = 1\r\nprofile = flat\r\n";
        if (item == "d3d11") {
            *data = kFlatGraphicsBytes;
            *size = sizeof(kFlatGraphicsBytes) - 1;
            return true;
        }
        if (item == "profile") {
            *data = kProfile;
            *size = sizeof(kProfile) - 1;
            return true;
        }
        return false;
    };
}

// The live file `leaf` in `dir` named by any step -- read, replaced, moved,
// removed. A backup's copy under edvr_backup\ is a different path and does not
// count: what is asked is whether the plan reaches the file the game reads.
static bool touchesLive(const Plan& plan, const std::wstring& dir, const wchar_t* leaf) {
    const std::wstring live = joinPath(dir, leaf);
    for (const Step& step : plan.steps) {
        if (_wcsicmp(step.from.c_str(), live.c_str()) == 0 ||
            _wcsicmp(step.to.c_str(), live.c_str()) == 0)
            return true;
    }
    return false;
}

// The text a plan would write to the live file `leaf`.
static std::string plannedText(const Plan& plan, const wchar_t* leaf) {
    for (const Step& step : plan.steps) {
        if (step.action == Action::WriteText && _wcsicmp(leafOf(step.to).c_str(), leaf) == 0)
            return step.text;
    }
    return std::string();
}

// A flat edition installed here already, a version ago: its d3d11.dll, its
// descriptor, its record, and the shipped defaults it was installed with as the
// base of the next merge.
static Survey installedFlatSurvey(const std::wstring& dir, const PayloadInfo& installedWith) {
    Survey s = baseSurvey(dir);
    s.haveOpenvrDir = false;
    s.d3d11 = fakeDll(DllKind::Edvr, joinPath(dir, L"d3d11.dll"), "an-older-flat-d3d11");
    s.descriptorPresent = true;
    s.descriptorSha = installedWith.descriptorSha;
    s.state.present = true;
    s.state.profile = "flat";
    s.state.descriptorSha = installedWith.descriptorSha;
    s.state.d3d11Installed = true;
    s.state.d3d11Sha = "an-older-flat-d3d11";
    s.baseIniText = installedWith.iniText;
    return s;
}

static void testFlatSettingsPlanner() {
    printf("\nthe flat edition's settings file: planning\n");
    const std::wstring dir = L"C:\\Games\\ED\\Products\\elite-dangerous-odyssey-64";
    const PayloadInfo flatOld = flatPayloadFor(kFlatTemplate);
    const PayloadInfo flatNew = flatPayloadFor(kFlatTemplateNewer);
    const Options options = testOptions();

    {   // The legacy shared-INI install: a flat install from before edvr-flat.ini
        // existed has only edvr.ini. The flat runtime has been reading it, so its
        // settings carry over into a file of the flat edition's own -- and edvr.ini
        // itself is not touched by anything the plan does.
        Survey s = installedFlatSurvey(dir, flatOld);
        s.iniPresent = true;
        s.iniText = kSharedIni;
        const Plan plan = planInstall(s, options, flatNew);
        check(!plan.blocked, "a flat update over a legacy shared edvr.ini plans");
        expectEq(plan.settingsFile, "edvr-flat.ini", "and says the file it works on is edvr-flat.ini");
        check(hasStep(plan, Action::WriteText, nullptr, L"edvr-flat.ini"),
              "it writes the flat edition's own edvr-flat.ini");
        check(!touchesLive(plan, dir, L"edvr.ini"),
              "and nothing in the plan reads, backs up, replaces or removes edvr.ini");
        const std::string seeded = plannedText(plan, L"edvr-flat.ini");
        expectEq(iniValue(seeded, "fix.temporal_aa"), "dlaa",
                 "the setting the flat runtime was reading carries over");
        expectEq(iniValue(seeded, "fix.render_sharpness"), "0",
                 "and the setting that arrived in this version is adopted beside it");
        check(!hasStep(plan, Action::Backup, L"edvr-flat.ini", nullptr),
              "a file that is new has nothing to back up");
        check(notesMention(plan, "Creating edvr-flat.ini from your edvr.ini"),
              "the report says where the new file came from");
        check(plan.nextState.iniSha == sha256Bytes(seeded.data(), seeded.size()),
              "the record names the hash of the flat file it writes");
        check(!plan.nothingToDo, "and a run that has only the seeding to do is not 'nothing to do'");
    }

    {   // The same, asked for fresh defaults: the shared file is not read into the
        // new one, and is still not touched.
        Survey s = installedFlatSurvey(dir, flatOld);
        s.iniPresent = true;
        s.iniText = kSharedIni;
        Options fresh = options;
        fresh.keepSettings = false;
        const Plan plan = planInstall(s, fresh, flatNew);
        const std::string written = plannedText(plan, L"edvr-flat.ini");
        expectEq(iniValue(written, "fix.temporal_aa"), "off",
                 "--replace-settings gives the flat file the shipped defaults, not edvr.ini's");
        check(!touchesLive(plan, dir, L"edvr.ini"), "and still leaves edvr.ini alone");
        check(notesMention(plan, "every setting at its default"), "and says so");
    }

    {   // Two deliberately different files. The flat update merges the FLAT one
        // and the VR profile's tuning is not read into it, let alone written.
        Survey s = installedFlatSurvey(dir, flatOld);
        s.iniPresent = true;
        s.iniText = kSharedIni;
        s.flatIniPresent = true;
        s.flatIniText = kFlatOwnIni;
        const Plan plan = planInstall(s, options, flatNew);
        check(!plan.blocked, "a flat update over two different files plans");
        const std::string merged = plannedText(plan, L"edvr-flat.ini");
        expectEq(iniValue(merged, "fix.temporal_aa"), "fsr", "the flat file's own setting is kept");
        expectEq(iniValue(merged, "fix.temporal_aa_model"), "l",
                 "a default the flat file had not changed moves with the new version");
        expectEq(iniValue(merged, "fix.render_sharpness"), "0", "and the new setting arrives");
        expectEq(iniValue(merged, "hotkey.menu", "<absent>"), "<absent>",
                 "nothing of the VR profile's tuning is merged into it");
        expectEq(iniValue(merged, "fix.black_void", "<absent>"), "<absent>",
                 "not a single one of its values");
        check(hasStep(plan, Action::Backup, L"edvr-flat.ini", L"edvr-flat.ini"),
              "the flat file is backed up before it is changed");
        check(!touchesLive(plan, dir, L"edvr.ini"), "and edvr.ini is not in the plan at all");
    }

    {   // A setting this version does not know is carried to the end of its section under a note
        // that names the file it came from. The flat edition's own file is edvr-flat.ini, and a
        // note saying "your edvr.ini" (the flight of 2026-09-30 read edvr-flat.ini while every
        // message said edvr.ini) points at a file the flat edition never reads. Seeded from the
        // shared file, naming edvr.ini is right: that is where the lines were.
        Survey s = installedFlatSurvey(dir, flatOld);
        s.iniPresent = true;
        s.iniText = kSharedIni;
        s.flatIniPresent = true;
        s.flatIniText = std::string(kFlatOwnIni) + "stale_line = 1\r\n";
        const std::string merged = plannedText(planInstall(s, options, flatNew), L"edvr-flat.ini");
        check(merged.find("stale_line = 1") != std::string::npos &&
                  merged.find("# carried over from your edvr-flat.ini; not an EDVR setting this version knows") !=
                      std::string::npos,
              "a flat update carries an unknown line under a note naming edvr-flat.ini");
        check(merged.find("carried over from your edvr.ini") == std::string::npos,
              "and never one naming edvr.ini, which it did not come from");

        Survey seed = installedFlatSurvey(dir, flatOld);
        seed.iniPresent = true;
        seed.iniText = kSharedIni;
        const std::string seeded = plannedText(planInstall(seed, options, flatNew), L"edvr-flat.ini");
        check(seeded.find("# carried over from your edvr.ini; not an EDVR setting this version knows") !=
                  std::string::npos,
              "a seed from the shared file names edvr.ini for the lines it carries over");
    }

    {   // Asked for fresh defaults with two files: the flat one is replaced, after
        // a backup of it, and only that one.
        Survey s = installedFlatSurvey(dir, flatOld);
        s.iniPresent = true;
        s.iniText = kSharedIni;
        s.flatIniPresent = true;
        s.flatIniText = kFlatOwnIni;
        Options fresh = options;
        fresh.keepSettings = false;
        const Plan plan = planInstall(s, fresh, flatNew);
        expectEq(iniValue(plannedText(plan, L"edvr-flat.ini"), "fix.temporal_aa"), "off",
                 "the flat file is replaced by the shipped defaults");
        check(hasStep(plan, Action::Backup, L"edvr-flat.ini", L"edvr-flat.ini"),
              "after a copy of it goes to the backup folder");
        check(!touchesLive(plan, dir, L"edvr.ini"), "and edvr.ini is not touched");
    }

    {   // Nothing installed and no settings at all: the defaults go to the flat
        // file, and edvr.ini is not created.
        Survey s = baseSurvey(dir);
        s.haveOpenvrDir = false;
        const Plan plan = planInstall(s, options, flatOld);
        check(hasStep(plan, Action::WriteText, nullptr, L"edvr-flat.ini"),
              "a fresh flat install writes edvr-flat.ini");
        check(!touchesLive(plan, dir, L"edvr.ini"), "and does not create edvr.ini");
        expectEq(iniValue(plannedText(plan, L"edvr-flat.ini"), "fix.temporal_aa_model"), "k",
                 "with the shipped defaults in it");
    }

    {   // A flat file that already says what it should: left alone, and said once.
        Survey s = installedFlatSurvey(dir, flatOld);
        s.d3d11.sha256 = flatOld.d3d11Sha;
        s.state.d3d11Sha = flatOld.d3d11Sha;
        s.flatIniPresent = true;
        s.flatIniText = kFlatTemplate;
        s.iniPresent = true;
        s.iniText = kSharedIni;
        const Plan plan = planInstall(s, options, flatOld);
        check(plan.nothingToDo, "an up-to-date flat install has nothing to do");
        check(!hasStep(plan, Action::WriteText, nullptr, L"edvr-flat.ini"),
              "it does not rewrite edvr-flat.ini");
        check(notesMention(plan, "edvr-flat.ini already says what it should"),
              "and names the file that needed nothing");
    }

    {   // A graphics mod in the d3d11.dll slot: the chain decision forces
        // advanced.real_dll into the file the flat runtime reads. In edvr.ini it
        // would change nothing the flat edition does.
        Survey s = baseSurvey(dir);
        s.haveOpenvrDir = false;
        s.d3d11 = fakeDll(DllKind::D3d11Provider, joinPath(dir, L"d3d11.dll"), "edhm", L"3Dmigoto");
        s.iniPresent = true;
        s.iniText = kSharedIni;
        const Plan plan = planInstall(s, options, flatOld);
        check(hasStep(plan, Action::Rename, L"d3d11.dll", L"d3d11_edhm.dll"),
              "a flat install chains to the mod already in the slot");
        expectEq(iniValue(plannedText(plan, L"edvr-flat.ini"), "advanced.real_dll"),
                 "d3d11_edhm.dll", "and the chain target is in the file the flat runtime reads");
        check(!touchesLive(plan, dir, L"edvr.ini"), "not in edvr.ini, which is left alone");
    }

    {   // The mirror image: the VR edition works on edvr.ini and leaves the flat
        // profile's file exactly as it is.
        const PayloadInfo vr = testPayload(kNextIni);
        Survey s = baseSurvey(dir);
        s.iniPresent = true;
        s.iniText = kSharedIni;
        s.flatIniPresent = true;
        s.flatIniText = kFlatOwnIni;
        const Plan plan = planInstall(s, options, vr);
        check(!plan.blocked, "a VR install beside a flat profile's file plans");
        expectEq(plan.settingsFile, "edvr.ini", "and says its file is edvr.ini");
        check(hasStep(plan, Action::WriteText, nullptr, L"edvr.ini"), "it writes edvr.ini");
        check(!touchesLive(plan, dir, L"edvr-flat.ini"),
              "and the flat profile's edvr-flat.ini is not in the plan at all");
    }

    {   // A base is the defaults of the file the record's edition wrote. Merging
        // into the OTHER edition's own file it is no base at all: here the record
        // says flat while the VR file has black_void = 0, which the flat defaults
        // in the base happen to say too. Read as a base, that would call the
        // person's choice untouched and hand back the VR default of 1.
        const PayloadInfo vr = testPayload("[fix]\r\nblack_void = 1\r\n");
        Survey s = baseSurvey(dir);
        s.state.present = true;
        s.state.profile = "flat";
        s.baseIniText = "[fix]\r\nblack_void = 0\r\n";
        s.iniPresent = true;
        s.iniText = "[fix]\r\nblack_void = 0\r\n";
        Options convert = options;
        convert.convertProfile = true;
        const Plan plan = planInstall(s, convert, vr);
        check(!plan.blocked, "a flat to VR conversion plans");
        // Kept means the plan either leaves the file alone or writes it with the
        // person's value; what it must not do is write the VR default over it.
        const std::string written = plannedText(plan, L"edvr.ini");
        expectEq(iniValue(written.empty() ? s.iniText : written, "fix.black_void"), "0",
                 "the VR file's value is kept, not read against the flat edition's defaults");
        check(plan.merge.twoWay, "the merge says it had no base to go on");
    }

    {   // ...but the shared file a flat install seeds from IS described by the base
        // whichever edition the record names: it is the one file every edition
        // before this wrote. Here the record says VR, the base holds the VR
        // defaults, and the person's one real choice (black_void) is the only
        // thing that carries over from it.
        const PayloadInfo flat = flatPayloadFor("[fix]\r\nblack_void = 5\r\ntemporal_aa = off\r\n");
        Survey s = baseSurvey(dir);
        s.state.present = true;
        s.state.profile = "vr";
        s.baseIniText = "[fix]\r\nblack_void = 1\r\ntemporal_aa = dlaa\r\n";
        s.iniPresent = true;
        s.iniText = "[fix]\r\nblack_void = 0\r\ntemporal_aa = dlaa\r\n";   // black_void is theirs; temporal_aa the VR default
        Options convert = options;
        convert.convertProfile = true;
        s.state.openvrInstalled = true;
        s.state.openvrSha = "installed-vr";
        s.state.openvrOrigSha = "game-vr";
        s.openvrCurrent = fakeDll(DllKind::Edvr, joinPath(s.game.openvrDir, L"openvr_api.dll"), "installed-vr");
        s.openvrOrig = fakeDll(DllKind::OpenVrRuntime, joinPath(s.game.openvrDir, L"openvr_api_orig.dll"), "game-vr");
        const Plan plan = planInstall(s, convert, flat);
        check(!plan.blocked, "a VR to flat conversion over a shared edvr.ini plans",
              plan.problems.empty() ? std::string() : plan.problems.front());
        const std::string seeded = plannedText(plan, L"edvr-flat.ini");
        expectEq(iniValue(seeded, "fix.black_void"), "0", "what the person chose in edvr.ini carries into edvr-flat.ini");
        expectEq(iniValue(seeded, "fix.temporal_aa"), "off",
                 "and what they never chose is the flat edition's default, not the VR one");
        check(!touchesLive(plan, dir, L"edvr.ini"), "with edvr.ini itself untouched");
    }

    // ---- uninstall: each edition removes its own file and no other ----
    auto installedFlat = [&](bool sharedToo, bool flatToo) {
        Survey s = baseSurvey(dir);
        s.haveOpenvrDir = false;
        s.d3d11 = fakeDll(DllKind::Edvr, joinPath(dir, L"d3d11.dll"), flatOld.d3d11Sha);
        s.descriptorPresent = true;
        s.descriptorSha = flatOld.descriptorSha;
        s.state.present = true;
        s.state.profile = "flat";
        s.state.descriptorSha = flatOld.descriptorSha;
        s.iniPresent = sharedToo;
        s.iniText = sharedToo ? kSharedIni : "";
        s.flatIniPresent = flatToo;
        s.flatIniText = flatToo ? kFlatOwnIni : "";
        return s;
    };
    Options withSettings = options;
    withSettings.removeSettings = true;

    {
        const Survey s = installedFlat(true, true);
        const Plan kept = planUninstall(s, options);
        check(!hasStep(kept, Action::Delete, L"edvr-flat.ini", nullptr) &&
                  !hasStep(kept, Action::Delete, L"edvr.ini", nullptr),
              "a flat uninstall removes neither settings file by default");
        check(notesMention(kept, "Leaving edvr-flat.ini in place"), "and names the flat one it left");

        const Plan removed = planUninstall(s, withSettings);
        check(hasStep(removed, Action::Delete, L"edvr-flat.ini", nullptr),
              "asked to remove settings, a flat uninstall removes edvr-flat.ini");
        check(hasStep(removed, Action::Backup, L"edvr-flat.ini", L"edvr-flat.ini"),
              "after a copy of it goes to the backup folder");
        check(!touchesLive(removed, dir, L"edvr.ini"),
              "and never touches the VR profile's edvr.ini");
    }

    {   // A flat install that never got a file of its own was reading the shared
        // edvr.ini, which the VR profile uses too: not the flat edition's to remove.
        const Survey s = installedFlat(true, false);
        const Plan plan = planUninstall(s, withSettings);
        check(!hasStep(plan, Action::Delete, L"edvr.ini", nullptr),
              "a legacy flat install's shared edvr.ini is not removed with it");
        check(notesMention(plan, "edvr.ini was left in place"), "and the report says why");
        check(hasStep(plan, Action::Delete, L"d3d11.dll", nullptr), "the rest of the uninstall still happens");
    }

    {   // The VR edition's uninstall removes edvr.ini and leaves the flat file.
        Survey s = baseSurvey(dir);
        s.d3d11 = fakeDll(DllKind::Edvr, joinPath(dir, L"d3d11.dll"), "vr-graphics");
        s.openvrCurrent = fakeDll(DllKind::Edvr, joinPath(s.game.openvrDir, L"openvr_api.dll"), "vr-runtime");
        s.openvrOrig = fakeDll(DllKind::OpenVrRuntime, joinPath(s.game.openvrDir, L"openvr_api_orig.dll"), "game-vr");
        s.state.present = true;
        s.iniPresent = true;
        s.iniText = kSharedIni;
        s.flatIniPresent = true;
        s.flatIniText = kFlatOwnIni;
        const Plan plan = planUninstall(s, withSettings);
        check(hasStep(plan, Action::Delete, L"edvr.ini", nullptr),
              "a VR uninstall asked to remove settings removes edvr.ini");
        check(!touchesLive(plan, dir, L"edvr-flat.ini"),
              "and never touches the flat profile's edvr-flat.ini");
    }

    {   // Without a record of the chain, a flat uninstall finds the mod it moved
        // aside by reading the file the flat runtime reads.
        Survey s = installedFlat(true, true);
        s.iniText = kSharedIni;   // real_dll is empty here
        s.flatIniText = "[advanced]\r\nreal_dll = d3d11_edhm.dll\r\n";
        s.otherD3d11.push_back(fakeDll(DllKind::D3d11Provider, joinPath(dir, L"d3d11_edhm.dll"),
                                       "edhm-sha", L"3Dmigoto"));
        const Plan plan = planUninstall(s, options);
        check(hasStep(plan, Action::Rename, L"d3d11_edhm.dll", L"d3d11.dll"),
              "the mod the flat file names is put back under its own name");
        Survey vr = baseSurvey(dir);
        vr.d3d11 = fakeDll(DllKind::Edvr, joinPath(dir, L"d3d11.dll"), "vr-graphics");
        vr.iniPresent = true;
        vr.iniText = "[advanced]\r\nreal_dll = d3d11_edhm.dll\r\n";
        vr.flatIniPresent = true;
        vr.flatIniText = kFlatOwnIni;   // real_dll empty in the flat file
        vr.otherD3d11.push_back(s.otherD3d11.front());
        check(hasStep(planUninstall(vr, options), Action::Rename, L"d3d11_edhm.dll", L"d3d11.dll"),
              "a VR uninstall reads edvr.ini for it, whatever the flat file says");
    }
}

// ---------------------------------------------------------------------------
// apply, for real, in a scratch folder
// ---------------------------------------------------------------------------

static PayloadProvider provider(bool failOpenvr) {
    return [failOpenvr](const std::string& item, const void** data, size_t* size) {
        static const char kD3d11[] = "TEST-D3D11-PAYLOAD";
        static const char kOpenvr[] = "TEST-OPENVR-PAYLOAD";
        static const char kLoader[] = "TEST-OPENXR-LOADER";
        static const char kLicense[] = "Khronos OpenXR Loader license";
        static const char kProfile[] = "[install]\r\nschema = 1\r\nprofile = vr\r\n";
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
        if (item == "openxr_loader") { *data=kLoader; *size=sizeof(kLoader)-1; return true; }
        if (item == "openxr_license") { *data=kLicense; *size=sizeof(kLicense)-1; return true; }
        if (item == "profile") { *data=kProfile; *size=sizeof(kProfile)-1; return true; }
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

// Whether the run got as far as writing a file: a step that completed is the run's
// own account of it, and `overwrote` is what that account must agree with.
static bool wroteAFile(const ApplyResult& result) {
    for (const std::string& line : result.done)
        if (line.rfind("wrote ", 0) == 0) return true;
    return false;
}

// The rig's stand-ins for the renames are defined with the writer's cases, further
// down (see "stand-ins for the renames"); the case here that needs them is earlier.
static unsigned long placeFile(const wchar_t* from, const wchar_t* to);
static void realRenames();

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

        // This case is about what the result says once a file HAS been replaced,
        // so it needs the run to get that far and to fail at the record and not
        // before. The replaces are therefore stood in for by renames that a
        // scanner cannot refuse (placeFile waits one out): with the real ones, a
        // real-time scanner that had d3d11.dll open at the instant of the first
        // replace made the classic rename fail with "access denied", the run
        // stopped there with nothing replaced -- `overwrote` false, correctly --
        // and this check failed 2 runs in 80. That the engine waits such a refusal
        // out is testApplyPatience's, scripted and on the real files; this case
        // must not depend on it.
        replaceHooksForTest(placeFile, placeFile);
        const ApplyResult result = applyPlan(planInstall(again, second, newer), provider(false));
        realRenames();
        check(!result.ok, "a run that cannot finish fails");
        check(fileExists(joinPath(gameDir, L"edvr_backup\\20260827-121500\\d3d11.dll")),
              "the backups it took are still there afterwards");
        check(wroteAFile(result), "having replaced files before it did", result.error);
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
        again.eliteKind=EliteExeKind::OdysseyQualified;
        again.d3d11.kind=DllKind::Edvr;
        again.openvrCurrent.kind=DllKind::Edvr;
        again.openvrOrig.kind=DllKind::OpenVrRuntime;
        again.openxrLoader.kind=DllKind::Foreign;
        // The build machine may well have Elite running -- it did the day this
        // was written. It cannot be running from this scratch folder, so the
        // survey reads it as somebody else's and plans anyway; all three flags
        // are cleared regardless (the third is a process list Windows would not
        // give), because this case is about the ini merge over a folder a
        // previous run really wrote and nothing else.
        again.gameRunningHere = false;
        again.gameRunningElsewhere = false;
        again.gameRunStateUnknown = false;
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
// the one writer of a live file (iniedit.h: writeFileAtomic)
// ---------------------------------------------------------------------------

// The files in `dir`, by leaf name, sorted and comma-joined: what a write left
// beside its target.
static std::string listing(const std::wstring& dir) {
    std::vector<std::string> names;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(joinPath(dir, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            names.push_back(toUtf8(fd.cFileName));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end());
    std::string out;
    for (const std::string& n : names) {
        if (!out.empty()) out += ", ";
        out += n;
    }
    return out;
}

// `path` held open by a handle of this test's own, as a reader would. With
// `shareDelete` it is how Config's read opens edvr.ini now: the POSIX-semantics
// replace goes through under it (the classic one is refused while ANY handle is
// open, share mode or not -- measured on Windows 11 build 26200). Without, it is
// how that read used to open it, and how an editor holding a file mid-save does:
// both kinds of rename are refused for as long as it is open.
static HANDLE holdOpen(const std::wstring& path, bool shareDelete) {
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | (shareDelete ? FILE_SHARE_DELETE : 0);
    return CreateFileW(path.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

// What the file behind an open handle says, from its start: the file the reader
// opened, whatever has been done to its name since.
static std::string readThrough(HANDLE h) {
    std::string out;
    LARGE_INTEGER zero{};
    if (h == INVALID_HANDLE_VALUE || !SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) return out;
    char buffer[256];
    DWORD got = 0;
    while (ReadFile(h, buffer, sizeof(buffer), &got, nullptr) && got) out.append(buffer, got);
    return out;
}

// ---------------------------------------------------------------------------
// stand-ins for the renames (iniedit.h, replaceHooksForTest)
//
// A rig that counts renames must not let the real file system into the count.
// The real-time scanner and the search indexer have every file that was just
// written open for a few milliseconds, and the classic rename is refused with a
// real "access denied" for as long as ANY handle to its target is open: the
// writer tries again, as it should, and an exact total of two comes out as three.
// That failed a full build once (2026-09-29), and it reproduces on demand with a
// process that opens the rig's files and lets go (17 runs in 20). So the cases
// that count script the answers, and the cases that use the real renames assert
// what holds however many times a scanner made the writer try.

// What a stand-in does when it is to put the file in place: the real move, tried
// again until nothing else has the file open, so that a scanner looking at it in
// the middle of a case costs a few milliseconds and not a count.
static unsigned long placeFile(const wchar_t* from, const wchar_t* to) {
    DWORD last = ERROR_SUCCESS;
    for (int i = 0; i < 400; ++i) {   // two seconds at the most
        if (MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return ERROR_SUCCESS;
        last = GetLastError();
        Sleep(5);
    }
    return last;
}

// A stand-in that answers from a script: each call takes the next reply -- 0 puts
// the file in place, anything else is the Windows error it refuses with -- and
// once the script is spent every call puts the file in place.
struct Replies {
    std::vector<unsigned long> script;
    size_t                     next = 0;
};
static Replies g_posixReplies;
static Replies g_classicReplies;

static unsigned long reply(Replies& r, const wchar_t* from, const wchar_t* to) {
    const unsigned long code = r.next < r.script.size() ? r.script[r.next++] : 0;
    return code != 0 ? code : placeFile(from, to);
}
static unsigned long posixReply(const wchar_t* from, const wchar_t* to) {
    return reply(g_posixReplies, from, to);
}
static unsigned long classicReply(const wchar_t* from, const wchar_t* to) {
    return reply(g_classicReplies, from, to);
}

// Installs the two scripts, and clears whatever an earlier case left in the counts
// or in a remembered refusal.
static void script(const std::vector<unsigned long>& posix,
                   const std::vector<unsigned long>& classic) {
    g_posixReplies = Replies();
    g_posixReplies.script = posix;
    g_classicReplies = Replies();
    g_classicReplies.script = classic;
    replaceHooksForTest(posixReply, classicReply);
}

// The stand-ins off and the counts cleared: the real renames.
static void realRenames() { replaceHooksForTest(nullptr, nullptr); }

// `code`, `n` times: a rename that stays refused.
static std::vector<unsigned long> repeated(unsigned long code, size_t n) {
    return std::vector<unsigned long>(n, code);
}

// Two stand-ins for the real-file-system cases that need one of the two renames
// to answer in a fixed way and the other to be real.
static unsigned long posixUnsupported(const wchar_t*, const wchar_t*) {
    return ERROR_INVALID_PARAMETER;
}
// A busy answer on every other call: a case where each replace meets one.
static int g_calls = 0;
static unsigned long posixEveryOtherBusy(const wchar_t* from, const wchar_t* to) {
    return (++g_calls % 2) ? ERROR_SHARING_VIOLATION : placeFile(from, to);
}

// One write, with the numbers a scripted case is asked about. The backoff is a
// millisecond: nothing scripted waits on a real reader.
struct Wrote {
    bool         ok = false;
    int          tries = -1;
    std::wstring why;
};
static Wrote writeWith(const std::wstring& target, const std::string& bytes, int retries = 5) {
    AtomicWriteOptions options;
    options.retries = retries;
    options.backoffMs = 1;
    Wrote w;
    w.ok = writeFileAtomic(target, bytes, &w.why, options, &w.tries);
    return w;
}

static std::string counts(int tries) {
    return std::to_string(tries) + " tries, " + std::to_string(posixReplaceAttempts()) +
           " POSIX calls, " + std::to_string(classicReplaceAttempts()) + " classic calls, " +
           (posixReplaceRefused() ? "refusal remembered" : "no refusal remembered");
}

// The write's tries and the counts since the scripts went in, against what the
// case says they must be. One line, so a failing case prints all four numbers.
static void expectCounts(const char* what, const Wrote& w, int tries, int posix, int classic,
                         bool refused) {
    const bool good = w.tries == tries && posixReplaceAttempts() == posix &&
                      classicReplaceAttempts() == classic && posixReplaceRefused() == refused;
    check(good, what,
          "got " + counts(w.tries) + "; wanted " + std::to_string(tries) + " tries, " +
              std::to_string(posix) + " POSIX calls, " + std::to_string(classic) +
              " classic calls, " + (refused ? "refusal remembered" : "no refusal remembered"));
}

static void testAtomicWrite(const std::wstring& scratch) {
    printf("\nwriting a live file whole\n");

    const std::wstring dir = joinPath(scratch, L"atomic");
    removeTree(dir);
    makeTree(dir);
    const std::wstring target = joinPath(dir, L"edvr.ini");
    realRenames();   // no stand-in, nothing remembered, nothing counted

    // What follows uses the REAL renames, and so asserts what holds however many
    // times a scanner made the writer try: the outcome, the temp file's absence,
    // and that every attempt asked the POSIX-semantics rename first. The exact
    // attempt counts of the retry logic are testReplaceScripts', scripted.

    {   // The plain cases: the bytes given, none of the old ones, nothing left over.
        std::wstring why;
        int tries = -1;
        check(writeFileAtomic(target, "first\r\n", &why, AtomicWriteOptions(), &tries),
              "a new file is written", toUtf8(why));
        expectEq(readAll(target), "first\r\n", "with exactly the bytes given");
        check(tries >= 1, "by a replace that was actually made", std::to_string(tries) + " tries");
        expectEq(listing(dir), "edvr.ini", "and nothing else is left beside it");

        check(writeFileAtomic(target, "second, and longer than the first\r\n", &why),
              "an existing file is replaced", toUtf8(why));
        expectEq(readAll(target), "second, and longer than the first\r\n",
                 "with the new bytes");
        check(writeFileAtomic(target, "3\r\n", &why), "a shorter file replaces a longer one",
              toUtf8(why));
        expectEq(readAll(target), "3\r\n", "and leaves no tail of the longer one");

        std::string bytes(2500000, 'x');   // past the one-megabyte write chunk
        for (size_t i = 0; i < bytes.size(); i += 4093) bytes[i] = static_cast<char>('a' + i % 26);
        check(writeFileAtomic(target, bytes, &why), "a file of several megabytes is written",
              toUtf8(why));
        check(readAll(target) == bytes, "and reads back byte for byte");

        check(writeFileAtomic(target, std::string(), &why), "an empty file can be written",
              toUtf8(why));
        std::string got = "not empty";
        check(readFileBytes(target, &got) && got.empty(), "and reads back as empty");
        expectEq(listing(dir), "edvr.ini", "with nothing left beside it after all of that");
    }

    {   // A reader that holds the target open and does NOT share DELETE refuses
        // the replace -- either kind of rename -- for as long as it holds on. The
        // writer tries again as it was told to, gives up, and leaves both the
        // original and the folder as it found them. The reader never lets go, so
        // every attempt fails and the count of them is fixed: no scanner can
        // change it.
        writeAll(target, "original\r\n");
        HANDLE reader = holdOpen(target, false);
        check(reader != INVALID_HANDLE_VALUE, "a reader not sharing DELETE can hold the target open");
        AtomicWriteOptions quick;
        quick.retries = 3;
        quick.backoffMs = 1;
        std::wstring why;
        int tries = 0;
        realRenames();
        const bool wrote = writeFileAtomic(target, "never lands\r\n", &why, quick, &tries);
        printf("  info  reader not sharing DELETE: wrote=%d after %d tries; the message: %s\n",
               wrote ? 1 : 0, tries, toUtf8(why).c_str());
        check(!wrote, "a replace that reader refuses fails when the retries are spent");
        check(tries == 4, "the first try and the three it was allowed were spent",
              std::to_string(tries) + " tries");
        check(!why.empty(), "and the failure says why", "no message");
        expectEq(readAll(target), "original\r\n", "the original is exactly as it was");
        expectEq(listing(dir), "edvr.ini", "and no temporary file is left");
        check(posixReplaceAttempts() == tries, "every attempt asked the POSIX-semantics rename, once",
              std::to_string(posixReplaceAttempts()) + " calls in " + std::to_string(tries) + " tries");
        check(!posixReplaceRefused(),
              "the refusal was of this rename, not of POSIX-semantics renames: nothing is remembered");
        if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
        check(writeFileAtomic(target, "lands now\r\n", &why),
              "once the reader lets go, the same write goes through", toUtf8(why));
        expectEq(readAll(target), "lands now\r\n", "with the new bytes");
    }

    {   // A reader that holds the target open and DOES share DELETE -- which is how
        // Config's read opens edvr.ini -- is replaced under by the POSIX-semantics
        // rename, and goes on reading the file it opened. (The classic rename is
        // refused here: measured with cmd's move /Y on Windows 11 build 26200,
        // which is why this rename exists.)
        //
        // The reader never lets go, so a write that lands has landed under it,
        // and no number of retries could have done that for a rename the reader
        // refuses. How many attempts it took is therefore not what this asserts (a
        // scanner that has the temp file or the target open for a moment costs
        // one, and did fail a build); that every attempt asked the POSIX-semantics
        // rename, and that none needed the classic one, is.
        writeAll(target, "held open\r\n");
        HANDLE reader = holdOpen(target, true);
        check(reader != INVALID_HANDLE_VALUE, "a reader sharing DELETE can hold the target open");
        AtomicWriteOptions patient;
        patient.retries = 100;   // half a second, for a scanner; the reader itself costs none
        patient.backoffMs = 5;
        std::wstring why;
        int tries = 0;
        realRenames();
        const bool wrote =
            writeFileAtomic(target, "replaced under the reader\r\n", &why, patient, &tries);
        printf("  info  reader sharing DELETE: wrote=%d after %d tries; POSIX calls %d, classic %d; "
               "the message: %s\n",
               wrote ? 1 : 0, tries, posixReplaceAttempts(), classicReplaceAttempts(),
               toUtf8(why).c_str());
        check(wrote, "the replace succeeds while a reader that shares DELETE holds the file",
              toUtf8(why));
        check(posixReplaceAttempts() == tries, "every attempt asked the POSIX-semantics rename, once",
              std::to_string(posixReplaceAttempts()) + " calls in " + std::to_string(tries) + " tries");
        check(classicReplaceAttempts() == 0,
              "and none fell back to the classic rename, which cannot replace under this reader",
              std::to_string(classicReplaceAttempts()) + " classic calls");
        check(!posixReplaceRefused(),
              "which this volume took: nothing was remembered as unsupported");
        expectEq(readAll(target), "replaced under the reader\r\n", "and the path holds the new bytes");
        expectEq(readThrough(reader), "held open\r\n", "while the reader still sees the file it opened");
        if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
        expectEq(listing(dir), "edvr.ini", "with nothing left beside it");
    }

    {   // The default policy is the one that was asked for: five tries after the
        // first, and it is what a caller who says nothing gets.
        const AtomicWriteOptions defaults{};
        check(defaults.retries == 5 && defaults.backoffMs == 20,
              "the default is five retries, twenty milliseconds apart");
    }

    {   // A read-only target is somebody's decision, not a transient: it is
        // refused, the retries are spent on it (access denied is one of the
        // codes that can pass), and nothing changes. It never reaches the
        // POSIX-semantics rename: the classic one is what has been measured to
        // refuse it, and the attribute is not something to test that one on.
        // Every attempt is refused whatever a scanner does, so the counts are
        // fixed.
        writeAll(target, "protected\r\n");
        SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_READONLY);
        AtomicWriteOptions quick;
        quick.retries = 2;
        quick.backoffMs = 1;
        std::wstring why;
        int tries = 0;
        realRenames();
        const bool wrote = writeFileAtomic(target, "overwritten\r\n", &why, quick, &tries);
        check(!wrote, "a read-only file is not replaced");
        check(tries == 3, "the retries were spent on it", std::to_string(tries) + " tries");
        check(posixReplaceAttempts() == 0,
              "without the POSIX-semantics rename having been asked to",
              std::to_string(posixReplaceAttempts()) + " calls");
        check(classicReplaceAttempts() == tries, "every attempt going straight to the classic one",
              std::to_string(classicReplaceAttempts()) + " calls in " + std::to_string(tries) + " tries");
        expectEq(readAll(target), "protected\r\n", "the file is untouched");
        expectEq(listing(dir), "edvr.ini", "and no temporary file is left");
        SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    {   // A reader that does not share DELETE and lets go while the retries are
        // running: the write lands. This is what the retry is for.
        writeAll(target, "before\r\n");
        HANDLE reader = holdOpen(target, false);
        std::thread letGo([reader] {
            Sleep(60);
            if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
        });
        AtomicWriteOptions patient;
        patient.retries = 200;   // up to a second: this is not about the default policy
        patient.backoffMs = 5;
        std::wstring why;
        int tries = 0;
        realRenames();
        const bool wrote = writeFileAtomic(target, "after\r\n", &why, patient, &tries);
        letGo.join();
        check(wrote, "a reader that lets go while the retries run does not fail the write",
              toUtf8(why));
        expectEq(readAll(target), "after\r\n", "and the write landed");
        check(posixReplaceAttempts() == tries, "every attempt asking the POSIX-semantics rename, once",
              std::to_string(posixReplaceAttempts()) + " calls in " + std::to_string(tries) + " tries");
        expectEq(listing(dir), "edvr.ini", "with nothing left beside it");
        printf("  info  it landed on try %d\n", tries);
    }

    {   // Where the POSIX-semantics rename is refused as unsupported the classic
        // rename does the replace, for real. Only that rename's own answer is
        // stood in for (the refusal); the replace itself is the operating
        // system's.
        writeAll(target, "before the fallback\r\n");
        AtomicWriteOptions patient;
        patient.retries = 100;
        patient.backoffMs = 5;
        std::wstring why;
        int tries = 0;
        replaceHooksForTest(posixUnsupported, nullptr);
        check(writeFileAtomic(target, "by the classic rename\r\n", &why, patient, &tries),
              "a write whose POSIX-semantics rename is refused as unsupported still lands",
              toUtf8(why));
        expectEq(readAll(target), "by the classic rename\r\n", "with the new bytes");
        check(posixReplaceAttempts() == 1 && posixReplaceRefused(),
              "the POSIX-semantics rename was asked once, and the refusal is remembered",
              counts(tries));
        check(classicReplaceAttempts() == tries, "every attempt being the classic rename's",
              counts(tries));

        int again = 0;
        check(writeFileAtomic(target, "and again\r\n", &why, patient, &again),
              "the next write lands too", toUtf8(why));
        expectEq(readAll(target), "and again\r\n", "with its bytes");
        check(posixReplaceAttempts() == 1,
              "without the POSIX-semantics rename being asked again: a refusal is not retried on every write",
              std::to_string(posixReplaceAttempts()) + " calls");
        check(classicReplaceAttempts() == tries + again, "the classic one having done both",
              counts(again));

        // The classic rename is what runs now, so a reader that shares DELETE
        // holds it off, as it always did. Where an operating system has taught
        // MoveFileExW POSIX semantics that is not so, and either answer is fine;
        // what matters is that the POSIX-semantics call is not made.
        HANDLE reader = holdOpen(target, true);
        AtomicWriteOptions quick;
        quick.retries = 3;
        quick.backoffMs = 1;
        int heldTries = 0;
        const bool wrote = writeFileAtomic(target, "under the reader\r\n", &why, quick, &heldTries);
        printf("  info  classic rename under a reader sharing DELETE: wrote=%d after %d tries\n",
               wrote ? 1 : 0, heldTries);
        if (!wrote) {
            check(heldTries == 4, "the classic rename spent the first try and the three it was allowed",
                  std::to_string(heldTries) + " tries");
            expectEq(readAll(target), "and again\r\n", "and left the file as it was");
        }
        check(posixReplaceAttempts() == 1, "and the POSIX-semantics rename was still not asked for");
        if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
        expectEq(listing(dir), "edvr.ini", "with no temporary file left");
        realRenames();
    }

    {   // A folder that is not there is not a folder to create, and not a failure
        // to retry: it fails without reaching the replace.
        const std::wstring missing = joinPath(joinPath(dir, L"no-such-folder"), L"edvr.ini");
        std::wstring why;
        int tries = -1;
        check(!writeFileAtomic(missing, "x", &why, AtomicWriteOptions(), &tries),
              "a target in a folder that is not there fails");
        check(tries == 0, "before it reaches the replace", std::to_string(tries) + " tries");
        check(!why.empty(), "and says why", "no message");
        check(!dirExists(joinPath(dir, L"no-such-folder")), "and does not make the folder");
    }
    realRenames();
}

// ---------------------------------------------------------------------------
// the retry logic, scripted
//
// Every case here stands in for BOTH renames (replaceHooksForTest), so the real
// file system is in none of the counts. Each states exactly what the writer must
// do -- how many tries, how many times each rename is asked, whether a refusal is
// remembered -- and each is a case a mutation of the writer must break: retrying
// on any error, skipping the POSIX-semantics rename, not asking it again on a
// retry, remembering what it should not, forgetting what it should not.

static void testReplaceScripts(const std::wstring& scratch) {
    printf("\nthe writer's retries, scripted\n");

    const std::wstring dir = joinPath(scratch, L"scripted");
    removeTree(dir);
    makeTree(dir);
    const std::wstring target = joinPath(dir, L"edvr.ini");
    const unsigned long kBadPath = ERROR_BAD_PATHNAME;   // an answer nobody recognises

    {   // A clean write asks the POSIX-semantics rename once and never the classic one.
        script({}, {});
        writeAll(target, "before\r\n");
        const Wrote w = writeWith(target, "clean\r\n");
        check(w.ok, "a clean write lands", toUtf8(w.why));
        expectCounts("and asked the POSIX-semantics rename once and the classic one never", w, 1, 1,
                     0, false);
        expectEq(readAll(target), "clean\r\n", "with the new bytes");
        expectEq(listing(dir), "edvr.ini", "and no temporary file is left");
    }

    // Every answer that means "this OS or volume does not do POSIX-semantics
    // renames": the classic rename does that attempt, the refusal is remembered,
    // and the next write does not ask again.
    for (const unsigned long code : {ERROR_INVALID_PARAMETER, ERROR_NOT_SUPPORTED,
                                     ERROR_INVALID_FUNCTION, ERROR_CALL_NOT_IMPLEMENTED,
                                     ERROR_INVALID_LEVEL}) {
        const std::string which = "error " + std::to_string(code) + ": ";
        script({code}, {});
        writeAll(target, "before\r\n");
        const Wrote first = writeWith(target, "by the classic rename\r\n");
        check(first.ok, (which + "the write lands").c_str(), toUtf8(first.why));
        expectCounts((which + "refused as unsupported: the classic rename does that attempt, "
                              "and the refusal is remembered").c_str(),
                     first, 1, 1, 1, true);
        expectEq(readAll(target), "by the classic rename\r\n", "with the new bytes");
        const Wrote second = writeWith(target, "and again\r\n");
        check(second.ok, "the next write lands", toUtf8(second.why));
        expectCounts((which + "and the next write does not ask the POSIX-semantics rename again")
                         .c_str(),
                     second, 1, 1, 2, true);
    }

    {   // Refused as unsupported, and then the classic rename is busy once: the
        // retry goes straight to the classic rename.
        script({ERROR_INVALID_PARAMETER}, {ERROR_ACCESS_DENIED});
        writeAll(target, "before\r\n");
        const Wrote w = writeWith(target, "after one busy answer\r\n");
        check(w.ok, "a write whose classic rename is busy once lands", toUtf8(w.why));
        expectCounts("on the second try, without asking the POSIX-semantics rename again", w, 2, 1, 2,
                     true);
        expectEq(readAll(target), "after one busy answer\r\n", "with the new bytes");
    }

    // The documented transient answers, from the POSIX-semantics rename: exactly
    // one retry, through that same rename, and never the classic one.
    for (const unsigned long code :
         {ERROR_SHARING_VIOLATION, ERROR_ACCESS_DENIED, ERROR_LOCK_VIOLATION}) {
        const std::string which = "error " + std::to_string(code) + ": ";
        script({code}, {});
        writeAll(target, "before\r\n");
        const Wrote w = writeWith(target, "after one busy answer\r\n");
        check(w.ok, (which + "a busy answer does not fail the write").c_str(), toUtf8(w.why));
        expectCounts((which + "exactly one retry, through the POSIX-semantics rename, and no "
                              "classic rename at all").c_str(),
                     w, 2, 2, 0, false);
        expectEq(readAll(target), "after one busy answer\r\n", "with the new bytes");
        expectEq(listing(dir), "edvr.ini", "and no temporary file is left");
    }

    {   // An answer the writer does not recognise: the classic rename decides that
        // attempt, and the next write asks again -- it is not a refusal.
        script({kBadPath, kBadPath}, {});
        writeAll(target, "before\r\n");
        const Wrote first = writeWith(target, "by the classic rename\r\n");
        check(first.ok, "a failure the writer does not recognise still lets the write land",
              toUtf8(first.why));
        expectCounts("in one attempt, and not remembered as a refusal", first, 1, 1, 1, false);
        const Wrote second = writeWith(target, "and again\r\n");
        check(second.ok, "the next write lands", toUtf8(second.why));
        expectCounts("having asked the POSIX-semantics rename each time", second, 1, 2, 2, false);
        expectEq(readAll(target), "and again\r\n", "with the new bytes");
    }

    {   // The same answer, and the classic rename busy once: the retry asks the
        // POSIX-semantics rename AGAIN, because nothing was remembered.
        script({kBadPath, kBadPath}, {ERROR_ACCESS_DENIED});
        writeAll(target, "before\r\n");
        const Wrote w = writeWith(target, "after one busy answer\r\n");
        check(w.ok, "an unrecognised answer and a busy classic rename still land", toUtf8(w.why));
        expectCounts("the retry asking the POSIX-semantics rename again", w, 2, 2, 2, false);
        expectEq(readAll(target), "after one busy answer\r\n", "with the new bytes");
    }

    // Only the documented transient answers are retried. Every other answer from
    // the classic rename -- there being nothing to replace, no such folder, a full
    // disk, a file mapped into a process -- fails the write on the spot.
    for (const unsigned long code : {ERROR_SHARING_VIOLATION, ERROR_ACCESS_DENIED,
                                     ERROR_LOCK_VIOLATION}) {
        const std::string which = "classic error " + std::to_string(code) + ": ";
        script({ERROR_INVALID_PARAMETER}, {code});
        writeAll(target, "before\r\n");
        const Wrote w = writeWith(target, "after one busy answer\r\n");
        check(w.ok, (which + "a transient answer is retried and the write lands").c_str(),
              toUtf8(w.why));
        expectCounts((which + "exactly one retry").c_str(), w, 2, 1, 2, true);
    }
    for (const unsigned long code : {ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, ERROR_DISK_FULL,
                                     ERROR_USER_MAPPED_FILE, ERROR_INVALID_PARAMETER,
                                     ERROR_NOT_SUPPORTED}) {
        const std::string which = "classic error " + std::to_string(code) + ": ";
        script({ERROR_INVALID_PARAMETER}, {code});
        writeAll(target, "before\r\n");
        const Wrote w = writeWith(target, "never lands\r\n");
        check(!w.ok, (which + "an answer that does not pass fails the write").c_str());
        expectCounts((which + "at once: one try, no retry").c_str(), w, 1, 1, 1, true);
        check(!w.why.empty(), "and says why", "no message");
        expectEq(readAll(target), "before\r\n", "the original is exactly as it was");
        expectEq(listing(dir), "edvr.ini", "and no temporary file is left");
    }

    {   // The POSIX-semantics rename refused for a reason that passes, every time:
        // the retries are spent on it, and the classic rename is never asked.
        script(repeated(ERROR_SHARING_VIOLATION, 4), {});
        writeAll(target, "before\r\n");
        const Wrote w = writeWith(target, "never lands\r\n", 3);
        check(!w.ok, "a POSIX-semantics rename that stays busy fails the write");
        expectCounts("after the first try and the three it was allowed, never asking the classic "
                     "rename", w, 4, 4, 0, false);
        expectEq(readAll(target), "before\r\n", "the original is exactly as it was");
        expectEq(listing(dir), "edvr.ini", "and no temporary file is left");
    }

    {   // The classic rename refused for a reason that passes, every time.
        script({ERROR_INVALID_PARAMETER}, repeated(ERROR_ACCESS_DENIED, 4));
        writeAll(target, "before\r\n");
        const Wrote w = writeWith(target, "never lands\r\n", 3);
        check(!w.ok, "a classic rename that stays busy fails the write");
        expectCounts("after the first try and the three it was allowed, the POSIX-semantics rename "
                     "asked only once", w, 4, 1, 4, true);
        expectEq(readAll(target), "before\r\n", "the original is exactly as it was");
        expectEq(listing(dir), "edvr.ini", "and no temporary file is left");
    }

    {   // A read-only target never reaches the POSIX-semantics rename, whatever it
        // would have said.
        script({}, repeated(ERROR_ACCESS_DENIED, 3));
        writeAll(target, "protected\r\n");
        SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_READONLY);
        const Wrote w = writeWith(target, "overwritten\r\n", 2);
        check(!w.ok, "a read-only file is not replaced");
        expectCounts("every attempt going straight to the classic rename", w, 3, 0, 3, false);
        expectEq(readAll(target), "protected\r\n", "the file is untouched");
        SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    {   // The replace on its own, for a caller that staged its file (the apply
        // engine): it renames and nothing else, reports what the writer reports --
        // the tries, and the Windows error of the last refusal -- and on failure
        // leaves the staged file where it was for its owner to delete.
        const std::wstring staged = joinPath(dir, L"staged.tmp");
        AtomicWriteOptions quick;
        quick.retries = 3;
        quick.backoffMs = 1;
        int tries = -1;
        unsigned long code = 99;

        writeAll(target, "before\r\n");
        writeAll(staged, "staged\r\n");
        script({ERROR_ACCESS_DENIED}, {});
        check(replaceFileAtomic(staged, target, quick, &tries, &code),
              "a staged file is put in place over the target after one busy answer");
        check(tries == 2 && code == ERROR_SUCCESS, "having made two tries, and reporting no error",
              std::to_string(tries) + " tries, error " + std::to_string(code));
        expectCounts("asking the POSIX-semantics rename twice and the classic one never",
                     Wrote{true, tries, {}}, 2, 2, 0, false);
        expectEq(readAll(target), "staged\r\n", "the target holds the staged bytes");
        check(!fileExists(staged), "and the staged file is gone: it was renamed, not copied");

        writeAll(target, "before\r\n");
        writeAll(staged, "staged\r\n");
        script(repeated(ERROR_SHARING_VIOLATION, 4), {});
        tries = -1;
        code = ERROR_SUCCESS;
        check(!replaceFileAtomic(staged, target, quick, &tries, &code),
              "a target that stays busy is not replaced");
        check(tries == 4 && code == ERROR_SHARING_VIOLATION,
              "after the first try and the three it was allowed, reporting the last error",
              std::to_string(tries) + " tries, error " + std::to_string(code));
        expectEq(readAll(target), "before\r\n", "the target is exactly as it was");
        expectEq(readAll(staged), "staged\r\n", "and the staged file is where it was");

        script({ERROR_INVALID_PARAMETER}, {ERROR_FILE_NOT_FOUND});
        tries = -1;
        code = ERROR_SUCCESS;
        check(!replaceFileAtomic(staged, target, quick, &tries, &code),
              "an answer that is not a hold fails at once");
        check(tries == 1 && code == ERROR_FILE_NOT_FOUND,
              "in one try, reporting the classic rename's error",
              std::to_string(tries) + " tries, error " + std::to_string(code));

        script({}, {});
        check(replaceFileAtomic(staged, target, quick), "the tries and the error are optional");
        expectEq(readAll(target), "staged\r\n", "and the file still lands");
        expectEq(listing(dir), "edvr.ini", "with nothing left beside it");
    }

    {   // A rotation of the mirror's generations under retries: every replace it
        // makes meets one busy answer first, and the generations are still right.
        // Nine replaces -- 1 for the first copy, 2 for the second, 3 for each of
        // the third and the fourth -- each asked twice.
        const std::wstring gdir = joinPath(scratch, L"scripted-generations");
        removeTree(gdir);
        makeTree(gdir);
        const std::wstring name = L"edvr.ini";
        std::wstring why;
        g_calls = 0;
        replaceHooksForTest(posixEveryOtherBusy, nullptr);
        check(writeGenerations(gdir, name, "A\r\n", true, &why), "a first copy is written",
              toUtf8(why));
        check(writeGenerations(gdir, name, "B\r\n", true, &why), "a second copy rotates in",
              toUtf8(why));
        check(writeGenerations(gdir, name, "C\r\n", true, &why), "a third copy rotates in",
              toUtf8(why));
        check(writeGenerations(gdir, name, "D\r\n", true, &why), "a fourth copy rotates in",
              toUtf8(why));
        check(posixReplaceAttempts() == 18 && classicReplaceAttempts() == 0,
              "each of the nine replaces asked the POSIX-semantics rename twice, the classic one never",
              std::to_string(posixReplaceAttempts()) + " POSIX calls, " +
                  std::to_string(classicReplaceAttempts()) + " classic calls");
        expectEq(readAll(generationPath(gdir, name, 0)), "D\r\n", "the newest is the fourth");
        expectEq(readAll(generationPath(gdir, name, 1)), "C\r\n", ".1 is the third");
        expectEq(readAll(generationPath(gdir, name, 2)), "B\r\n", ".2 is the second");
        check(!fileExists(generationPath(gdir, name, 3)), "and the first is dropped");
        expectEq(listing(gdir), "edvr.ini, edvr.ini.1, edvr.ini.2", "with no temporary file left");
    }
    realRenames();
}

// ---------------------------------------------------------------------------
// the apply engine's replaces (apply.h: replacePatienceForTest)
//
// The engine writes a DLL by staging it beside the target and replacing the
// target, and it moves the game's runtime aside with a rename over whatever is at
// the new name. Both are iniedit's replace, so a file that something else has open
// for a moment is waited out and does not fail the run. The classic rename is
// refused with "access denied" while ANY handle to its target is open, and the
// engine used to give up on the first refusal, roll the run back and report that
// it could not finish: under a stand-in for a real-time scanner it lost the first
// replace of the pair in 2 runs in 80 (2026-09-29), and a person's antivirus
// holds a file it has just looked at in the same way.
//
// The scripted cases stand in for BOTH renames, so the real file system is in none
// of their counts, and each is one that a mutation of the engine must break:
// replacing by the classic rename, not waiting, waiting on an answer that is not a
// hold, reporting the wrong file, forgetting a file that was replaced. The two at
// the end use the real renames on the real files and assert what holds however
// often a scanner made the engine try.

// The plan a repair makes of the pair: the graphics DLL and the runtime, each
// written over the one that is there.
static Plan pairPlan(const std::wstring& dir) {
    Plan plan;
    plan.backupDir = joinPath(dir, L"edvr_backup");
    Step graphics;
    graphics.action = Action::WritePayload;
    graphics.item = "d3d11";
    graphics.to = joinPath(dir, L"d3d11.dll");
    plan.steps.push_back(graphics);
    Step runtime;
    runtime.action = Action::WritePayload;
    runtime.item = "openvr";
    runtime.to = joinPath(dir, L"openvr_api.dll");
    plan.steps.push_back(runtime);
    return plan;
}

// An install's first move: the game's runtime renamed aside, and, with `thenOurs`,
// ours written where it was.
static Plan asidePlan(const std::wstring& dir, bool thenOurs) {
    Plan plan;
    plan.backupDir = joinPath(dir, L"edvr_backup");
    Step aside;
    aside.action = Action::Rename;
    aside.from = joinPath(dir, L"openvr_api.dll");
    aside.to = joinPath(dir, L"openvr_api_orig.dll");
    aside.required = true;
    plan.steps.push_back(aside);
    if (thenOurs) {
        Step ours;
        ours.action = Action::WritePayload;
        ours.item = "openvr";
        ours.to = joinPath(dir, L"openvr_api.dll");
        plan.steps.push_back(ours);
    }
    return plan;
}

// An uninstall's move: the game's runtime renamed back over ours, which is a file
// that is there.
static Plan restorePlan(const std::wstring& dir) {
    Plan plan;
    plan.backupDir = joinPath(dir, L"edvr_backup");
    Step back;
    back.action = Action::Rename;
    back.from = joinPath(dir, L"openvr_api_orig.dll");
    back.to = joinPath(dir, L"openvr_api.dll");
    back.required = true;
    plan.steps.push_back(back);
    return plan;
}

// The pair an install would replace; `withOrig` adds the game's own runtime under
// the name it is kept by, as it stands when there is something to put back.
static void layOutPair(const std::wstring& dir, bool withOrig = false) {
    removeTree(dir);
    makeTree(dir);
    writeAll(joinPath(dir, L"d3d11.dll"), "OLD-GRAPHICS");
    writeAll(joinPath(dir, L"openvr_api.dll"), "OLD-RUNTIME");
    if (withOrig) writeAll(joinPath(dir, L"openvr_api_orig.dll"), "THE-GAMES-OWN-RUNTIME");
}

// How often each rename was asked since the scripts went in, against what the case
// says. One line, so a failing case prints all three numbers.
static void expectAttempts(const std::string& what, int posix, int classic, bool refused) {
    const auto shown = [](int p, int c, bool r) {
        return std::to_string(p) + " POSIX calls, " + std::to_string(c) + " classic calls, " +
               (r ? "refusal remembered" : "no refusal remembered");
    };
    check(posixReplaceAttempts() == posix && classicReplaceAttempts() == classic &&
              posixReplaceRefused() == refused,
          what.c_str(),
          "got " + shown(posixReplaceAttempts(), classicReplaceAttempts(), posixReplaceRefused()) +
              "; wanted " + shown(posix, classic, refused));
}

static bool mentions(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

// The operating system's words for `code`, which is what the engine's failure says
// when it says why. Asked of the same API in the same way, so that a language other
// than English changes both sides of the comparison and not the answer.
static std::string windowsText(unsigned long code) {
    char* message = nullptr;
    const DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<char*>(&message),
        0, nullptr);
    std::string out;
    if (n && message) out.assign(message, n);
    if (message) LocalFree(message);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}

static void testApplyPatience(const std::wstring& scratch) {
    printf("\nthe apply engine's replaces\n");

    const std::wstring dir = joinPath(scratch, L"apply-patience");
    const std::wstring d3d11 = joinPath(dir, L"d3d11.dll");
    const std::wstring runtime = joinPath(dir, L"openvr_api.dll");
    const std::wstring orig = joinPath(dir, L"openvr_api_orig.dll");
    const std::string newGraphics = "TEST-D3D11-PAYLOAD";
    const std::string newRuntime = "TEST-OPENVR-PAYLOAD";
    const std::string pairListing = "d3d11.dll, openvr_api.dll";

    // A refusal that is never lifted must not cost the rig two seconds a case:
    // three tries after the first, a millisecond apart. (The product's own numbers
    // are what the two real-file cases at the end run with.)
    replacePatienceForTest(3, 1);

    {   // Nothing in the way: each of the two writes asks the POSIX-semantics rename
        // once, and the classic one is never asked.
        layOutPair(dir);
        script({}, {});
        const ApplyResult r = applyPlan(pairPlan(dir), provider(false));
        check(r.ok, "a pair that nothing holds is replaced", r.error);
        expectEq(readAll(d3d11), newGraphics, "with the new graphics DLL");
        expectEq(readAll(runtime), newRuntime, "and the new runtime");
        expectAttempts("each write asked the POSIX-semantics rename once and the classic one never",
                       2, 0, false);
        expectEq(listing(dir), pairListing, "and no staged file is left beside them");
    }

    // The answers that mean "held for a moment", on the first replace: it is asked
    // again, through the same rename, and the run lands. "Access denied" is the
    // one the real rename gives under a scanner.
    for (const unsigned long code :
         {ERROR_ACCESS_DENIED, ERROR_SHARING_VIOLATION, ERROR_LOCK_VIOLATION}) {
        const std::string which = "error " + std::to_string(code) + ": ";
        layOutPair(dir);
        script({code}, {});
        const ApplyResult r = applyPlan(pairPlan(dir), provider(false));
        check(r.ok, (which + "a file that is busy once does not fail the run").c_str(), r.error);
        expectAttempts(which + "one retry of the first replace, through the POSIX-semantics "
                               "rename, and no classic rename at all",
                       3, 0, false);
        expectEq(readAll(d3d11), newGraphics, "the new graphics DLL is in place");
        expectEq(readAll(runtime), newRuntime, "and so is the new runtime");
        expectEq(listing(dir), pairListing, "with no staged file left");
    }

    {   // Where the volume does not do POSIX-semantics renames the classic rename
        // does them, and its busy answers are waited out in the same way.
        layOutPair(dir);
        script({ERROR_INVALID_PARAMETER}, {ERROR_ACCESS_DENIED});
        const ApplyResult r = applyPlan(pairPlan(dir), provider(false));
        check(r.ok, "without POSIX-semantics renames a busy classic rename is waited out", r.error);
        expectAttempts("the POSIX-semantics rename asked once and remembered as unsupported; the "
                       "classic one refused once, then twice more for the two writes",
                       1, 3, true);
        expectEq(readAll(d3d11), newGraphics, "the new graphics DLL is in place");
        expectEq(readAll(runtime), newRuntime, "and so is the new runtime");
    }

    {   // The first file held for good: three tries after the first are all the rig
        // allows. The run fails, names the file, is rolled back, never gets to the
        // second, and does not claim a file was replaced -- because none was.
        layOutPair(dir);
        script(repeated(ERROR_ACCESS_DENIED, 8), {});
        const ApplyResult r = applyPlan(pairPlan(dir), provider(false));
        check(!r.ok, "a file that stays held fails the run");
        check(r.rolledBack, "which is rolled back", r.error);
        check(!r.overwrote, "and does not claim that a file had been replaced: none had");
        check(mentions(r.error, "d3d11.dll"), "the failure names the file", r.error);
        check(mentions(r.error, windowsText(ERROR_ACCESS_DENIED).c_str()),
              "and says what the operating system said", r.error);
        expectAttempts("the first try and the three it was allowed, and the second file never reached",
                       4, 0, false);
        expectEq(readAll(d3d11), "OLD-GRAPHICS", "the graphics DLL is exactly as it was");
        expectEq(readAll(runtime), "OLD-RUNTIME", "and so is the runtime");
        expectEq(listing(dir), pairListing, "with no staged file left");
    }

    {   // An answer that is not a hold -- the disk is full -- fails the run at once:
        // waiting on it would only be slow.
        layOutPair(dir);
        script({ERROR_DISK_FULL}, {ERROR_DISK_FULL});
        const ApplyResult r = applyPlan(pairPlan(dir), provider(false));
        check(!r.ok, "a replace refused for a reason that does not pass fails the run");
        check(r.rolledBack, "which is rolled back", r.error);
        check(!r.overwrote, "and does not claim that a file had been replaced");
        check(mentions(r.error, windowsText(ERROR_DISK_FULL).c_str()),
              "saying what the operating system said", r.error);
        expectAttempts("after one try of each rename, without waiting", 1, 1, false);
        expectEq(readAll(d3d11), "OLD-GRAPHICS", "the graphics DLL is exactly as it was");
        expectEq(listing(dir), pairListing, "with no staged file left");
    }

    {   // The second file held for good, after the first was replaced: the first is
        // put back, and the result says a file had already been replaced.
        layOutPair(dir);
        script({0, ERROR_SHARING_VIOLATION, ERROR_SHARING_VIOLATION, ERROR_SHARING_VIOLATION,
                ERROR_SHARING_VIOLATION},
               {});
        const ApplyResult r = applyPlan(pairPlan(dir), provider(false));
        check(!r.ok, "a second file that stays held fails the run");
        check(r.rolledBack, "which is rolled back", r.error);
        check(r.overwrote, "and admits that the first file had already been replaced");
        check(wroteAFile(r), "which the steps it completed agree with");
        check(mentions(r.error, "openvr_api.dll"), "the failure names the second file", r.error);
        check(mentions(r.error, windowsText(ERROR_SHARING_VIOLATION).c_str()),
              "and says what the operating system said", r.error);
        expectAttempts("one try for the first file, and the first and the three allowed for the second",
                       5, 0, false);
        expectEq(readAll(d3d11), "OLD-GRAPHICS", "the graphics DLL is back to what it was");
        expectEq(readAll(runtime), "OLD-RUNTIME", "and the runtime never left");
        expectEq(listing(dir), pairListing, "with no staged file left");
    }

    {   // The rename that moves the game's runtime aside waits in the same way.
        layOutPair(dir);
        script({ERROR_ACCESS_DENIED}, {});
        const ApplyResult r = applyPlan(asidePlan(dir, true), provider(false));
        check(r.ok, "a rename that is busy once does not fail the run", r.error);
        expectAttempts("one retry of the rename, and one try for the write after it", 3, 0, false);
        expectEq(readAll(orig), "OLD-RUNTIME", "the game's runtime is under its new name");
        expectEq(readAll(runtime), newRuntime, "and ours is in its place");
        expectEq(listing(dir), "d3d11.dll, openvr_api.dll, openvr_api_orig.dll",
                 "with no staged file left");
    }

    {   // The same rename refused for good, and nothing after it: nothing moved, and
        // nothing is claimed.
        layOutPair(dir);
        script(repeated(ERROR_SHARING_VIOLATION, 8), {});
        const ApplyResult r = applyPlan(asidePlan(dir, false), provider(false));
        check(!r.ok, "a rename that stays refused fails the run");
        check(r.rolledBack, "which is rolled back", r.error);
        check(!r.overwrote, "and does not claim that a file had been replaced");
        check(mentions(r.error, "openvr_api.dll"), "the failure names the file it could not move",
              r.error);
        check(mentions(r.error, windowsText(ERROR_SHARING_VIOLATION).c_str()),
              "and says what the operating system said", r.error);
        expectAttempts("the first try and the three it was allowed", 4, 0, false);
        expectEq(readAll(runtime), "OLD-RUNTIME", "the game's runtime is where it was");
        check(!fileExists(orig), "with nothing under the new name");
    }

    {   // An uninstall's rename puts the game's runtime back over ours, which is a
        // file that is there: a replace, and it waits in the same way.
        layOutPair(dir, true);
        script({ERROR_ACCESS_DENIED}, {});
        const ApplyResult r = applyPlan(restorePlan(dir), provider(false));
        check(r.ok, "a rename over an existing file that is busy once does not fail the run",
              r.error);
        expectAttempts("one retry, through the POSIX-semantics rename", 2, 0, false);
        expectEq(readAll(runtime), "THE-GAMES-OWN-RUNTIME", "the game's runtime is back");
        check(!fileExists(orig), "and is no longer under the other name");
    }

    // The product's own patience from here on, for the real files.
    replacePatienceForTest(-1, 0);

    {   // A reader that shares DELETE has the graphics DLL open for the whole run,
        // as a real-time scanner or the indexer does. The classic rename is refused
        // for as long as it holds on (measured: Windows 11 build 26200), so the
        // engine used to fail here at once; the POSIX-semantics rename goes through
        // under it, and the reader goes on seeing the file it opened. The reader
        // never lets go, so this asserts the outcome and not how often the engine
        // tried.
        layOutPair(dir);
        realRenames();
        HANDLE reader = holdOpen(d3d11, true);
        check(reader != INVALID_HANDLE_VALUE, "a reader sharing DELETE can hold the graphics DLL open");
        const ApplyResult r = applyPlan(pairPlan(dir), provider(false));
        check(r.ok, "the pair is replaced under a reader that shares DELETE", r.error);
        expectEq(readAll(d3d11), newGraphics, "the path holds the new graphics DLL");
        expectEq(readThrough(reader), "OLD-GRAPHICS", "while the reader still sees the file it opened");
        expectEq(readAll(runtime), newRuntime, "and the runtime is replaced");
        check(!posixReplaceRefused(), "which this volume took: nothing was remembered as unsupported");
        if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
        expectEq(listing(dir), pairListing, "with no staged file left");
    }

    {   // The same hold on the other kind of replace: the uninstall's rename of the
        // game's runtime back over ours, while a reader that shares DELETE has ours
        // open.
        layOutPair(dir, true);
        realRenames();
        HANDLE reader = holdOpen(runtime, true);
        check(reader != INVALID_HANDLE_VALUE, "a reader sharing DELETE can hold our runtime open");
        const ApplyResult r = applyPlan(restorePlan(dir), provider(false));
        check(r.ok, "the game's runtime is renamed back over ours under such a reader", r.error);
        expectEq(readAll(runtime), "THE-GAMES-OWN-RUNTIME", "the path holds the game's runtime");
        expectEq(readThrough(reader), "OLD-RUNTIME", "while the reader still sees the file it opened");
        check(!fileExists(orig), "and it is no longer under the other name");
        if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
    }

    {   // A reader that does NOT share DELETE refuses both kinds of rename until it
        // lets go, and here it lets go while the engine is waiting: the run lands.
        // That is what the wait is for. The reader is released once the engine has
        // asked a second time, so the case waits on the engine and not on a clock
        // (the bound is only so that an engine that never asks again fails this
        // case and does not hang it), and the engine runs with the product's own
        // patience.
        layOutPair(dir);
        realRenames();
        HANDLE reader = holdOpen(d3d11, false);
        check(reader != INVALID_HANDLE_VALUE, "a reader not sharing DELETE can hold the graphics DLL open");
        std::thread letGo([reader] {
            for (int i = 0; i < 3000 && posixReplaceAttempts() < 2; ++i) Sleep(1);
            Sleep(20);
            if (reader != INVALID_HANDLE_VALUE) CloseHandle(reader);
        });
        const ApplyResult r = applyPlan(pairPlan(dir), provider(false));
        letGo.join();
        check(r.ok, "a reader that lets go while the engine waits does not fail the run", r.error);
        check(posixReplaceAttempts() >= 3,
              "after the first replace was refused at least once, and the second one asked",
              std::to_string(posixReplaceAttempts()) + " calls");
        expectEq(readAll(d3d11), newGraphics, "the graphics DLL is replaced");
        expectEq(readAll(runtime), newRuntime, "and so is the runtime");
        expectEq(listing(dir), pairListing, "with no staged file left");
    }

    replacePatienceForTest(-1, 0);
    realRenames();
}

// ---------------------------------------------------------------------------
// the mirror's generations
// ---------------------------------------------------------------------------

static void testGenerations(const std::wstring& scratch) {
    printf("\nthe mirror's generations\n");

    const std::wstring dir = joinPath(scratch, L"generations");
    removeTree(dir);
    makeTree(dir);
    const std::wstring name = L"edvr.ini";
    auto gen = [&](int g) { return readAll(generationPath(dir, name, g)); };
    auto leafOfNewest = [&]() { return toUtf8(leafOf(newestGeneration(dir, name))); };
    std::wstring why;

    check(writeGenerations(dir, name, "A\r\n", true, &why), "a first copy is written", toUtf8(why));
    expectEq(gen(0), "A\r\n", "as the newest");
    check(!fileExists(generationPath(dir, name, 1)), "with nothing behind it yet");

    check(writeGenerations(dir, name, "B\r\n", true, &why), "a second copy rotates in",
          toUtf8(why));
    expectEq(gen(0), "B\r\n", "the newest is the second");
    expectEq(gen(1), "A\r\n", "and the first is kept as .1");

    check(writeGenerations(dir, name, "C\r\n", true, &why), "a third copy rotates in",
          toUtf8(why));
    expectEq(gen(0), "C\r\n", "the newest is the third");
    expectEq(gen(1), "B\r\n", "the second moved to .1");
    expectEq(gen(2), "A\r\n", "and the first to .2");

    check(writeGenerations(dir, name, "D\r\n", true, &why), "a fourth copy rotates in",
          toUtf8(why));
    expectEq(gen(0), "D\r\n", "the newest is the fourth");
    expectEq(gen(1), "C\r\n", "the third is .1");
    expectEq(gen(2), "B\r\n", "the second is .2");
    check(!fileExists(generationPath(dir, name, 3)),
          "and there is no .3: three generations, the oldest dropped");
    expectEq(listing(dir), "edvr.ini, edvr.ini.1, edvr.ini.2",
             "nothing else is in the folder, no temporary file among it");

    // An install that changed nothing must not age the history out.
    check(writeGenerations(dir, name, "D\r\n", true, &why), "the same copy again succeeds",
          toUtf8(why));
    expectEq(gen(1), "C\r\n", "and pushes nothing down");
    expectEq(gen(2), "B\r\n", "so the oldest is still there");

    // A change made a moment ago replaces the newest and leaves the history.
    check(writeGenerations(dir, name, "E\r\n", false, &why), "a change replaces the newest in place",
          toUtf8(why));
    expectEq(gen(0), "E\r\n", "the newest is the change");
    expectEq(gen(1), "C\r\n", "and .1 is what it was");
    expectEq(gen(2), "B\r\n", "and .2");
    check(writeGenerations(dir, name, "F\r\n", false, &why), "and another",
          toUtf8(why));
    check(gen(0) == "F\r\n" && gen(1) == "C\r\n" && gen(2) == "B\r\n",
          "still one newest copy and the same two behind it: a generation per tweak would lose them");

    // Which generation a restore reads.
    expectEq(leafOfNewest(), "edvr.ini", "the newest generation is the newest copy");
    DeleteFileW(generationPath(dir, name, 0).c_str());
    expectEq(leafOfNewest(), "edvr.ini.1",
             "with the newest gone -- a crash between two writes -- the one behind it is read");
    writeAll(generationPath(dir, name, 0), "");
    expectEq(leafOfNewest(), "edvr.ini.1", "an empty newest is passed over too");
    DeleteFileW(generationPath(dir, name, 1).c_str());
    expectEq(leafOfNewest(), "edvr.ini.2", "and so is a missing .1");
    DeleteFileW(generationPath(dir, name, 2).c_str());
    check(newestGeneration(dir, name).empty(), "with nothing but an empty file there is nothing to read");
    check(generationPath(dir, name, 0) == joinPath(dir, name) &&
              generationPath(dir, name, 2) == joinPath(dir, name) + L".2",
          "and the paths are <name>, <name>.1, <name>.2");
    check(generationPath(dir + L"\\", name, 1) == joinPath(dir, name) + L".1",
          "whether or not the folder ends in a separator");

    // A rotation that cannot finish changes nothing. .2 is made read-only, so the
    // move of .1 up onto it is refused: the new copy has been written by then,
    // and it must not be in the way of a single generation.
    removeTree(dir);
    makeTree(dir);
    writeGenerations(dir, name, "P\r\n", true);
    writeGenerations(dir, name, "Q\r\n", true);
    writeGenerations(dir, name, "R\r\n", true);
    SetFileAttributesW(generationPath(dir, name, 2).c_str(), FILE_ATTRIBUTE_READONLY);
    check(!writeGenerations(dir, name, "S\r\n", true, &why),
          "a rotation that cannot move an older copy fails");
    check(gen(0) == "R\r\n" && gen(1) == "Q\r\n" && gen(2) == "P\r\n",
          "with every generation exactly as it was");
    expectEq(listing(dir), "edvr.ini, edvr.ini.1, edvr.ini.2", "and the staged copy is gone");
    SetFileAttributesW(generationPath(dir, name, 2).c_str(), FILE_ATTRIBUTE_NORMAL);
    check(writeGenerations(dir, name, "S\r\n", true, &why),
          "and once the older copy can be moved the same write goes through", toUtf8(why));
    check(gen(0) == "S\r\n" && gen(1) == "R\r\n" && gen(2) == "Q\r\n", "as three generations");

    // The LAST step of a rotation being refused -- the newest is read-only, so the
    // new copy cannot take its place -- has already kept the copy being replaced
    // as .1. The same write tried again must not push another real copy out to
    // make room for that duplicate.
    removeTree(dir);
    makeTree(dir);
    writeGenerations(dir, name, "P\r\n", true);
    writeGenerations(dir, name, "Q\r\n", true);
    writeGenerations(dir, name, "R\r\n", true);
    SetFileAttributesW(generationPath(dir, name, 0).c_str(), FILE_ATTRIBUTE_READONLY);
    check(!writeGenerations(dir, name, "S\r\n", true, &why),
          "a write whose last step is refused fails");
    check(gen(0) == "R\r\n" && gen(1) == "R\r\n" && gen(2) == "Q\r\n",
          "leaving the newest intact, with the copy it kept and the one behind that");
    SetFileAttributesW(generationPath(dir, name, 0).c_str(), FILE_ATTRIBUTE_NORMAL);
    check(writeGenerations(dir, name, "S\r\n", true, &why),
          "and once the newest can be replaced the same write goes through", toUtf8(why));
    check(gen(0) == "S\r\n" && gen(1) == "R\r\n" && gen(2) == "Q\r\n",
          "without pushing a real copy out for the duplicate");
    expectEq(listing(dir), "edvr.ini, edvr.ini.1, edvr.ini.2", "and no temporary file is left");

    // A folder that is not there fails cleanly.
    check(!writeGenerations(joinPath(dir, L"no-such-folder"), name, "x", true, &why),
          "a mirror folder that is not there is a failure");
}

static void testMirrorGenerations(const std::wstring& scratch) {
    printf("\nthe mirror survives what overwrote it\n");

    const std::wstring gameDir = joinPath(scratch, L"mirrorgen-game");
    const std::wstring root = joinPath(scratch, L"mirrorgen-root");
    const std::wstring mirrorDir = joinPath(root, L"mirrorgen-test");
    removeTree(gameDir);
    removeTree(root);
    makeTree(gameDir);
    const std::wstring liveIni = joinPath(gameDir, L"edvr.ini");
    auto mirrored = [&](int g) { return readAll(generationPath(mirrorDir, L"edvr.ini", g)); };

    const std::string tuned = "[fix]\r\nshare_exposure = 0\r\nblack_void = 0\r\n";
    const std::string defaults = "[fix]\r\nshare_exposure = 1\r\nblack_void = 1\r\n";

    // An earlier install mirrored the settings somebody tuned.
    writeAll(liveIni, tuned);
    check(updateMirror(gameDir, L"", mirrorDir).ok, "the tuned settings are mirrored");
    expectEq(mirrored(0), tuned, "as the newest copy");

    // A game update wipes the folder. The person declines the restore -- or it
    // fails -- and the install writes a fresh ini, which the mirror then takes.
    // This is the sequence that used to erase the only saved copy.
    DeleteFileW(liveIni.c_str());
    writeAll(liveIni, defaults);
    const MirrorResult afterInstall = updateMirror(gameDir, L"", mirrorDir);
    check(afterInstall.ok && !afterInstall.saved.empty(), "the fresh install is mirrored");
    expectEq(mirrored(0), defaults, "the newest copy is what the fresh install wrote");
    expectEq(mirrored(1), tuned, "and the settings it replaced are still there, as edvr.ini.1");

    // The same install run again, changing nothing, ages nothing.
    check(updateMirror(gameDir, L"", mirrorDir).ok, "an install that changes nothing is mirrored");
    expectEq(mirrored(1), tuned, "without pushing the kept copy down");

    // A setting changed afterwards -- the settings window, the in-game menu --
    // replaces the newest and leaves the kept copy where it is.
    const std::string touched = defaults + "\r\n[hotkey]\r\nmenu = F8\r\n";
    writeAll(liveIni, touched);
    check(updateMirrorIni(gameDir, mirrorDir).ok, "a settings change is mirrored");
    expectEq(mirrored(0), touched, "as the newest copy");
    expectEq(mirrored(1), tuned, "and the tuned settings are still edvr.ini.1");
    check(!fileExists(generationPath(mirrorDir, L"edvr.ini", 2)),
          "with no generation made for the tweak");

    // The restore reads the newest.
    {
        const std::wstring wiped = joinPath(scratch, L"mirrorgen-wiped");
        removeTree(wiped);
        makeTree(wiped);
        const MirrorInfo info = readMirror(mirrorDir);
        check(info.hasIni, "the mirror offers a restore");
        std::vector<std::string> notes;
        check(restoreFromMirror(wiped, info, &notes), "and it restores");
        expectEq(readAll(joinPath(wiped, L"edvr.ini")), touched, "the newest copy comes back");
    }

    // Two more installs that change the ini: three generations, the oldest gone.
    writeAll(liveIni, "[fix]\r\nshare_exposure = 2\r\n");
    check(updateMirror(gameDir, L"", mirrorDir).ok, "a later install is mirrored");
    writeAll(liveIni, "[fix]\r\nshare_exposure = 3\r\n");
    check(updateMirror(gameDir, L"", mirrorDir).ok, "and another");
    expectEq(mirrored(0), "[fix]\r\nshare_exposure = 3\r\n", "the newest is the last");
    expectEq(mirrored(1), "[fix]\r\nshare_exposure = 2\r\n", ".1 is the one before it");
    expectEq(mirrored(2), touched, ".2 is the copy from before that");
    check(!fileExists(generationPath(mirrorDir, L"edvr.ini", 3)),
          "and the copy before those is dropped: three generations");

    // A restore after a crash between two writes, or a half-written newest, reads
    // the copy behind it rather than offering nothing.
    {
        DeleteFileW(generationPath(mirrorDir, L"edvr.ini", 0).c_str());
        const MirrorInfo info = readMirror(mirrorDir);
        check(info.hasIni, "with the newest missing the mirror still offers a restore");
        const std::wstring wiped = joinPath(scratch, L"mirrorgen-wiped2");
        removeTree(wiped);
        makeTree(wiped);
        std::vector<std::string> notes;
        check(restoreFromMirror(wiped, info, &notes), "and restores");
        expectEq(readAll(joinPath(wiped, L"edvr.ini")), "[fix]\r\nshare_exposure = 2\r\n",
                 "from the newest copy that is there");
        writeAll(generationPath(mirrorDir, L"edvr.ini", 0), "");
        check(readMirror(mirrorDir).hasIni, "an empty newest does not hide the ones behind it");
    }

    // The flat profile's settings file is kept the same way.
    {
        const std::wstring flat = joinPath(gameDir, L"edvr-flat.ini");
        writeAll(flat, "[fix]\r\nflat = 1\r\n");
        check(updateMirror(gameDir, L"", mirrorDir).ok, "a flat ini is mirrored");
        writeAll(flat, "[fix]\r\nflat = 2\r\n");
        check(updateMirror(gameDir, L"", mirrorDir).ok, "and again");
        expectEq(readAll(generationPath(mirrorDir, L"edvr-flat.ini", 0)), "[fix]\r\nflat = 2\r\n",
                 "the flat ini's newest copy");
        expectEq(readAll(generationPath(mirrorDir, L"edvr-flat.ini", 1)), "[fix]\r\nflat = 1\r\n",
                 "and the one it replaced");
    }

    // Read-only on a mirrored copy -- a launcher's verification does that to
    // files it thinks are its own -- is cleared, as the copy always did.
    {
        SetFileAttributesW(generationPath(mirrorDir, L"edvr.ini", 0).c_str(),
                           FILE_ATTRIBUTE_READONLY);
        SetFileAttributesW(generationPath(mirrorDir, L"edvr.ini", 2).c_str(),
                           FILE_ATTRIBUTE_READONLY);
        writeAll(liveIni, "[fix]\r\nshare_exposure = 4\r\n");
        check(updateMirror(gameDir, L"", mirrorDir).ok, "a mirror with read-only copies is updated");
        expectEq(mirrored(0), "[fix]\r\nshare_exposure = 4\r\n", "with the new newest copy");
    }

    // No temporary file is left in the mirror by any of that.
    {
        const std::string files = listing(mirrorDir);
        check(files.find("edvr-tmp") == std::string::npos, "the mirror holds no temporary file",
              files);
    }
}

// ---------------------------------------------------------------------------
// the demoted field-of-view trims, through the mirror and an install
// ---------------------------------------------------------------------------

// A value somebody tuned under the OLD name has to survive the whole road an
// update walks: mirrored outside the game folder, restored after a game update
// wiped it, and merged into the NEW shipped file by the install that follows
// (which is the only thing that moves it, edvr.ini's `# moved-from:`). Run end to
// end against the real shipped file, with the shipped file's annotations taken out
// as the control.
static void testDemotedTrims(const std::wstring& root, const std::wstring& scratch) {
    printf("\nthe demoted field-of-view trims through the mirror, a restore and an install\n");

    const std::string shipped = readAll(joinPath(root, L"edvr.ini"));
    if (shipped.empty()) {
        fail("read the repository's edvr.ini", "not found next to the repo root");
        return;
    }
    const std::string tuned =
        "pimax-openxr/pimax-crystal-super:10, virtualdesktopxr/meta-quest-3:5";
    // Yesterday's install, as the previous version wrote it: the trims under
    // [fix], the base copy holding the shipped (empty) defaults, and the tuning
    // somebody did on top of them.
    const std::string oldBase =
        "[fix]\r\nfov_trim_vertical =\r\nfov_trim_outer =\r\nfov_trim_nasal =\r\n"
        "black_void = 1\r\n";
    const std::string oldUser =
        "[fix]\r\nfov_trim_vertical = " + tuned + "\r\n"
        "fov_trim_outer = oculus/meta-quest-3:7\r\nfov_trim_nasal =\r\n"
        "black_void = 0\r\n";

    const std::wstring gameDir = joinPath(scratch, L"trims-game");
    const std::wstring mirrorDir = joinPath(scratch, L"trims-mirror\\trims-test");
    removeTree(joinPath(scratch, L"trims-mirror"));
    layOutScratchGame(gameDir);
    makeTree(joinPath(gameDir, L"edvr_install"));
    writeAll(joinPath(gameDir, L"edvr.ini"), oldUser);
    writeAll(baseIniPath(gameDir), oldBase);
    check(updateMirror(gameDir, L"", mirrorDir).ok, "the old-layout settings are mirrored");
    expectEq(readAll(joinPath(mirrorDir, L"edvr.ini")), oldUser,
             "the mirror keeps them verbatim, under their old names");

    // Ran twice: once with the shipped file, once with its annotations removed.
    std::string unannotated = shipped;
    for (const char* key : {"vertical", "outer", "nasal"}) {
        const std::string line = std::string("# moved-from: fix.fov_trim_") + key;
        const size_t at = unannotated.find(line);
        if (at != std::string::npos) unannotated.replace(at, line.size(), "# (annotation removed)");
    }
    check(unannotated != shipped && unannotated.find("moved-from: fix.fov_trim_") == std::string::npos,
          "the control's ini really lost its three annotations");

    struct Road { const char* name; const std::string* nextIni; bool annotated; };
    const Road roads[] = {{"shipped edvr.ini", &shipped, true},
                          {"control, annotations removed", &unannotated, false}};
    for (const Road& road : roads) {
        const std::string label = std::string(road.annotated ? "" : "control: ") +
                                  (road.annotated ? "" : "without the annotation, ");
        // A game update wipes the folder; only the mirror is left. The restore
        // puts the old ini and its base back, and the install that follows
        // merges them into the new file (a plan, and then the real apply).
        const std::wstring wiped = joinPath(scratch, road.annotated ? L"trims-wiped" : L"trims-wiped-control");
        layOutScratchGame(wiped);
        const MirrorInfo info = readMirror(mirrorDir);
        std::vector<std::string> notes;
        check(restoreFromMirror(wiped, info, &notes), (label + "the restore succeeds").c_str());
        expectEq(readAll(joinPath(wiped, L"edvr.ini")), oldUser,
                 (label + "the restored ini is the old layout, verbatim").c_str());
        expectEq(readAll(baseIniPath(wiped)), oldBase,
                 (label + "and its base copy comes back beside it").c_str());

        Survey s = scratchSurvey(wiped);
        s.iniPresent = true;
        s.iniText = readAll(joinPath(wiped, L"edvr.ini"));
        s.baseIniText = readAll(baseIniPath(wiped));
        const PayloadInfo payload = testPayload(*road.nextIni);
        const Plan plan = planInstall(s, testOptions(), payload);
        const std::string planned = plannedIni(plan);
        check(!planned.empty(), (label + "the install plans to write an ini").c_str());
        const ApplyResult applied = applyPlan(plan, provider(false));
        check(applied.ok, (label + "the install applies").c_str(), applied.error);
        const std::string installed = readAll(joinPath(wiped, L"edvr.ini"));
        expectEq(installed, planned, (label + "and writes what it planned").c_str());

        expectEq(iniValue(installed, "fix.black_void"), "0",
                 (label + "another tuned value lands too (the merge ran)").c_str());
        if (road.annotated) {
            expectEq(iniValue(installed, "experimental.fov_trim_vertical", "<absent>"), tuned,
                     "the tuned trim list is under [experimental] after the install");
            expectEq(iniValue(installed, "experimental.fov_trim_outer", "<absent>"),
                     "oculus/meta-quest-3:7", "and so is the single entry");
            expectEq(iniValue(installed, "experimental.fov_trim_nasal", "<absent>"), "",
                     "the one left empty is still empty there");
            for (const char* key : {"fix.fov_trim_vertical", "fix.fov_trim_outer", "fix.fov_trim_nasal"}) {
                expectEq(iniValue(installed, key, "<absent>"), "<absent>",
                         (std::string("nothing is left under ") + key).c_str());
            }
            check(plan.merge.followed.size() >= 3, "the install's report says the trims followed");
        } else {
            check(iniValue(installed, "experimental.fov_trim_vertical", "<absent>") != tuned,
                  "control: the tuned trim list does NOT reach [experimental] without the annotation");
            expectEq(iniValue(installed, "fix.fov_trim_vertical", "<absent>"), tuned,
                     "control: it is carried under the old name, where nothing reads it");
        }
    }
}

// ---------------------------------------------------------------------------
// the flat edition's settings file, on real files
// ---------------------------------------------------------------------------

static void layOutFlatGame(const std::wstring& dir) {
    removeTree(dir);
    makeTree(dir);
    writeAll(joinPath(dir, L"EliteDangerous64.exe"), "not really the game");
}

// The file the flat runtime reads (config.cpp): edvr-flat.ini first, edvr.ini only
// while there is none.
static std::wstring flatRuntimeReads(const std::wstring& dir) {
    const std::wstring flat = joinPath(dir, L"edvr-flat.ini");
    return fileExists(flat) ? flat : joinPath(dir, L"edvr.ini");
}

// A survey of a scratch folder as a later run would take it -- what is on disk --
// with the facts that depend on the machine (is Elite running, which revision the
// executable is) fixed by the case.
static Survey diskSurvey(const std::wstring& dir) {
    Survey s = baseSurvey(dir);
    s.haveOpenvrDir = false;
    const std::wstring graphics = joinPath(dir, L"d3d11.dll");
    s.d3d11 = fileExists(graphics) ? fakeDll(DllKind::Edvr, graphics, sha256File(graphics))
                                   : fakeDll(DllKind::Absent, graphics, "");
    const std::wstring ini = joinPath(dir, L"edvr.ini");
    const std::wstring flatIni = joinPath(dir, L"edvr-flat.ini");
    s.iniPresent = fileExists(ini);
    if (s.iniPresent) s.iniText = readAll(ini);
    s.flatIniPresent = fileExists(flatIni);
    if (s.flatIniPresent) s.flatIniText = readAll(flatIni);
    s.baseIniText = readAll(baseIniPath(dir));
    s.state = readState(dir);
    const std::wstring descriptor = joinPath(dir, L"edvr_profile.ini");
    s.descriptorPresent = fileExists(descriptor);
    if (s.descriptorPresent) s.descriptorSha = sha256File(descriptor);
    return s;
}

static void testFlatSettingsFiles(const std::wstring& scratch) {
    printf("\nthe flat edition's settings file, on real files\n");
    const PayloadInfo flatOld = flatPayloadFor(kFlatTemplate);
    const PayloadInfo flatNew = flatPayloadFor(kFlatTemplateNewer);
    const Options options = testOptions();
    auto gameAt = [](const std::wstring& dir) {
        GameInstall game;
        game.dir = dir;
        game.source = L"Test";
        game.product = L"elite-dangerous-odyssey-64";
        game.odyssey = true;
        return game;
    };

    {   // The survey reads both files, each as itself.
        const std::wstring dir = joinPath(scratch, L"flatset-survey");
        layOutFlatGame(dir);
        writeAll(joinPath(dir, L"edvr.ini"), kSharedIni);
        writeAll(joinPath(dir, L"edvr-flat.ini"), kFlatOwnIni);
        const Survey both = surveyTarget(gameAt(dir));
        check(both.iniPresent && both.iniText == kSharedIni, "the survey reads edvr.ini as itself");
        check(both.flatIniPresent && both.flatIniText == kFlatOwnIni,
              "and edvr-flat.ini as itself");
        check(hasSettingsFor(both, "vr") && hasSettingsFor(both, "flat"),
              "each edition finds its settings");

        DeleteFileW(joinPath(dir, L"edvr-flat.ini").c_str());
        const Survey legacy = surveyTarget(gameAt(dir));
        check(legacy.iniPresent && !legacy.flatIniPresent && legacy.flatIniText.empty(),
              "a folder from before edvr-flat.ini has only the shared file");
        check(hasSettingsFor(legacy, "vr") && hasSettingsFor(legacy, "flat"),
              "which both editions read");

        DeleteFileW(joinPath(dir, L"edvr.ini").c_str());
        writeAll(joinPath(dir, L"edvr-flat.ini"), kFlatOwnIni);
        const Survey flatOnly = surveyTarget(gameAt(dir));
        check(!flatOnly.iniPresent && flatOnly.flatIniPresent, "a flat-only folder is read as one");
        check(!hasSettingsFor(flatOnly, "vr") && hasSettingsFor(flatOnly, "flat"),
              "and has settings for the flat edition alone");

        DeleteFileW(joinPath(dir, L"edvr-flat.ini").c_str());
        const Survey empty = surveyTarget(gameAt(dir));
        check(!hasSettingsFor(empty, "vr") && !hasSettingsFor(empty, "flat"),
              "an empty folder has settings for neither");
    }

    {   // A legacy shared-INI flat install, end to end: survey, plan, apply, and a
        // later update and uninstall. The flat runtime read edvr.ini until now;
        // from here on it reads edvr-flat.ini, and edvr.ini -- the VR profile's --
        // is byte for byte what it was throughout.
        const std::wstring dir = joinPath(scratch, L"flatset-legacy");
        layOutFlatGame(dir);
        writeAll(joinPath(dir, L"edvr.ini"), kSharedIni);
        Survey s = surveyTarget(gameAt(dir));
        s.eliteKind = EliteExeKind::OdysseyQualified;
        s.gameRunningHere = s.gameRunningElsewhere = s.gameRunStateUnknown = false;
        const Plan plan = planInstall(s, options, flatOld);
        check(!plan.blocked, "a flat install over a legacy shared edvr.ini plans",
              plan.problems.empty() ? std::string() : plan.problems.front());
        const ApplyResult result = applyPlan(plan, flatProvider());
        check(result.ok, "and applies", result.error);
        expectEq(readAll(joinPath(dir, L"edvr.ini")), kSharedIni,
                 "edvr.ini is exactly as the VR profile left it");
        const std::string flatText = readAll(joinPath(dir, L"edvr-flat.ini"));
        expectEq(iniValue(flatText, "fix.temporal_aa"), "dlaa",
                 "edvr-flat.ini carries the setting the flat runtime was reading");
        check(flatRuntimeReads(dir) == joinPath(dir, L"edvr-flat.ini"),
              "and it is the file the flat runtime reads from now on");
        const InstallState state = readState(dir);
        check(state.present && state.profile == "flat", "the record says flat");
        check(state.iniSha == sha256Bytes(flatText.data(), flatText.size()),
              "and holds the hash of edvr-flat.ini, the file that was written");
        expectEq(readAll(baseIniPath(dir)), kFlatTemplate,
                 "the kept base is the flat edition's shipped file");

        // A later update, with defaults that moved: the flat file merges.
        Options second = options;
        second.backupStamp = L"20260827-121500";
        const Plan update = planInstall(diskSurvey(dir), second, flatNew);
        check(!update.blocked && !touchesLive(update, dir, L"edvr.ini"),
              "a later flat update plans without touching edvr.ini");
        check(applyPlan(update, flatProvider()).ok, "and applies");
        expectEq(readAll(joinPath(dir, L"edvr.ini")), kSharedIni, "edvr.ini is still exactly as it was");
        const std::string updated = readAll(joinPath(dir, L"edvr-flat.ini"));
        expectEq(iniValue(updated, "fix.render_sharpness"), "0", "the flat file took the new setting");
        expectEq(iniValue(updated, "fix.temporal_aa"), "dlaa", "and kept the one it carried over");
        expectEq(readAll(joinPath(dir, L"edvr_backup\\20260827-121500\\edvr-flat.ini")), flatText,
                 "with the flat file as it was in the backup folder");
        check(!fileExists(joinPath(dir, L"edvr_backup\\20260827-121500\\edvr.ini")),
              "and no copy of edvr.ini, which nothing replaced");

        // The uninstall, asked to remove settings, removes the flat edition's.
        Options gone = options;
        gone.backupStamp = L"20260827-123000";
        gone.removeSettings = true;
        const ApplyResult removed = applyPlan(planUninstall(diskSurvey(dir), gone), flatProvider());
        check(removed.ok, "the flat uninstall applies", removed.error);
        check(!fileExists(joinPath(dir, L"edvr-flat.ini")), "edvr-flat.ini goes with the flat edition");
        expectEq(readAll(joinPath(dir, L"edvr_backup\\20260827-123000\\edvr-flat.ini")), updated,
                 "after a copy of it went to the backup folder");
        expectEq(readAll(joinPath(dir, L"edvr.ini")), kSharedIni,
                 "and edvr.ini, which is the VR profile's, is left exactly as it was");
        check(!fileExists(joinPath(dir, L"d3d11.dll")), "the rest of the flat edition is gone too");
    }

    {   // Two deliberately different files, end to end: the flat install merges the
        // flat file, and replacing settings replaces that one and only that one.
        const std::wstring dir = joinPath(scratch, L"flatset-two");
        layOutFlatGame(dir);
        writeAll(joinPath(dir, L"edvr.ini"), kSharedIni);
        writeAll(joinPath(dir, L"edvr-flat.ini"), kFlatOwnIni);
        const Plan plan = planInstall(diskSurvey(dir), options, flatOld);
        check(!plan.blocked && applyPlan(plan, flatProvider()).ok,
              "a flat install over two different files applies");
        expectEq(readAll(joinPath(dir, L"edvr.ini")), kSharedIni, "edvr.ini is byte for byte the same");
        const std::string merged = readAll(joinPath(dir, L"edvr-flat.ini"));
        expectEq(iniValue(merged, "fix.temporal_aa"), "fsr", "the flat file keeps its own setting");
        expectEq(iniValue(merged, "hotkey.menu", "<absent>"), "<absent>",
                 "and takes nothing from edvr.ini");

        Options fresh = options;
        fresh.backupStamp = L"20260827-124500";
        fresh.keepSettings = false;
        check(applyPlan(planInstall(diskSurvey(dir), fresh, flatOld), flatProvider()).ok,
              "asked for fresh defaults, the run applies");
        expectEq(iniValue(readAll(joinPath(dir, L"edvr-flat.ini")), "fix.temporal_aa"), "off",
                 "the flat file has the shipped defaults");
        expectEq(readAll(joinPath(dir, L"edvr_backup\\20260827-124500\\edvr-flat.ini")), merged,
                 "with what it held in the backup folder");
        expectEq(readAll(joinPath(dir, L"edvr.ini")), kSharedIni, "and edvr.ini is still untouched");
    }

    {   // The VR edition beside a flat profile's file: it updates edvr.ini and the
        // flat file is exactly what it was.
        const std::wstring dir = joinPath(scratch, L"flatset-vr");
        layOutScratchGame(dir);
        writeAll(joinPath(dir, L"edvr.ini"), kBaseIni);
        writeAll(joinPath(dir, L"edvr-flat.ini"), kFlatOwnIni);
        Survey s = scratchSurvey(dir);
        s.iniPresent = true;
        s.iniText = readAll(joinPath(dir, L"edvr.ini"));
        s.flatIniPresent = true;
        s.flatIniText = readAll(joinPath(dir, L"edvr-flat.ini"));
        const ApplyResult result = applyPlan(planInstall(s, options, testPayload(kNextIni)), provider(false));
        check(result.ok, "a VR install beside a flat profile's file applies", result.error);
        expectEq(iniValue(readAll(joinPath(dir, L"edvr.ini")), "fix.sun_glare"), "vivid",
                 "the VR file took the new setting");
        expectEq(readAll(joinPath(dir, L"edvr-flat.ini")), kFlatOwnIni,
                 "and the flat file is byte for byte the same");
    }
}

// The settings window: it reads and writes the file its edition's runtime reads.
static void testFlatSettingsWindow(const std::wstring& root, const std::wstring& scratch) {
    printf("\nthe settings window and the flat edition's file\n");
    const std::string shipped = readAll(joinPath(root, L"edvr.ini"));
    if (shipped.empty()) {
        fail("read the repository edvr.ini", "not found");
        return;
    }
    auto rowFor = [](const SettingsModel& model, const char* key) {
        for (size_t i = 0; i < model.rows().size(); ++i)
            if (std::string(model.rows()[i].def->key) == key) return i;
        return static_cast<size_t>(-1);
    };
    const std::string flatOwn = "[fix]\r\nblack_void = 0\r\n";

    {   // Two files: the flat model shows the flat file's value and writes only
        // there; the VR model beside it does the reverse.
        const std::wstring dir = joinPath(scratch, L"flatset-window-two");
        removeTree(dir);
        makeTree(dir);
        writeAll(joinPath(dir, L"edvr.ini"), shipped);
        writeAll(joinPath(dir, L"edvr-flat.ini"), flatOwn);

        SettingsModel flat;
        flat.load(dir, "flat");
        const size_t toggle = rowFor(flat, "black_void");
        if (toggle == static_cast<size_t>(-1)) {
            fail("find the setting to exercise", "black_void is not exposed");
            return;
        }
        check(flat.iniPath() == joinPath(dir, L"edvr-flat.ini"), "the flat window's file is edvr-flat.ini");
        expectEq(flat.rows()[toggle].value, "0", "it shows the flat file's value, not edvr.ini's");
        check(flat.set(toggle, "1"), "a change in the flat window is written", flat.lastError());
        expectEq(iniValue(readAll(joinPath(dir, L"edvr-flat.ini")), "fix.black_void"), "1",
                 "to edvr-flat.ini");
        expectEq(readAll(joinPath(dir, L"edvr.ini")), shipped, "and edvr.ini is byte for byte the same");
        const std::wstring backups = joinPath(dir, L"edvr_backup");
        const std::wstring stamp = firstSubdirLike(backups, L"settings-*");
        check(!stamp.empty() && readAll(joinPath(joinPath(backups, stamp), L"edvr-flat.ini")) == flatOwn,
              "with the flat file as it was in the settings backup");

        SettingsModel vr;
        vr.load(dir);
        check(vr.iniPath() == joinPath(dir, L"edvr.ini"), "the VR window's file is edvr.ini");
        const std::string flatAfter = readAll(joinPath(dir, L"edvr-flat.ini"));
        check(vr.set(rowFor(vr, "black_void"), "0"), "a change in the VR window is written", vr.lastError());
        expectEq(iniValue(readAll(joinPath(dir, L"edvr.ini")), "fix.black_void"), "0", "to edvr.ini");
        expectEq(readAll(joinPath(dir, L"edvr-flat.ini")), flatAfter,
                 "and edvr-flat.ini is exactly what the flat window left");
    }

    {   // A legacy folder: only the shared edvr.ini. The flat window shows what the
        // flat runtime is reading, and the first change starts edvr-flat.ini from
        // it, leaving edvr.ini as it is.
        const std::wstring dir = joinPath(scratch, L"flatset-window-legacy");
        removeTree(dir);
        makeTree(dir);
        writeAll(joinPath(dir, L"edvr.ini"), shipped);
        SettingsModel flat;
        flat.load(dir, "flat");
        const size_t toggle = rowFor(flat, "black_void");
        if (toggle == static_cast<size_t>(-1)) {
            fail("find the setting to exercise", "black_void is not exposed");
            return;
        }
        expectEq(flat.rows()[toggle].value, iniValue(shipped, "fix.black_void"),
                 "with no edvr-flat.ini yet the window shows the shared file's value");
        check(flat.set(toggle, "0"), "the first change succeeds", flat.lastError());
        check(fileExists(joinPath(dir, L"edvr-flat.ini")), "and starts edvr-flat.ini");
        const std::string started = readAll(joinPath(dir, L"edvr-flat.ini"));
        expectEq(iniValue(started, "fix.black_void"), "0", "with the change in it");
        check(started.size() == shipped.size(),
              "and every other setting carried over from the shared file: one character differs");
        expectEq(readAll(joinPath(dir, L"edvr.ini")), shipped, "while edvr.ini is exactly as it was");
        check(firstSubdirLike(joinPath(dir, L"edvr_backup"), L"settings-*").empty(),
              "a file that did not exist has nothing to back up");
        check(flat.set(toggle, "1"), "the next change succeeds too", flat.lastError());
        expectEq(iniValue(readAll(joinPath(dir, L"edvr-flat.ini")), "fix.black_void"), "1",
                 "and lands in the flat file");
        expectEq(readAll(joinPath(dir, L"edvr.ini")), shipped, "with edvr.ini untouched again");
    }

    {   // Nothing at all: the window says which file is missing.
        const std::wstring dir = joinPath(scratch, L"flatset-window-none");
        removeTree(dir);
        makeTree(dir);
        SettingsModel flat;
        flat.load(dir, "flat");
        const size_t toggle = rowFor(flat, "black_void");
        if (toggle == static_cast<size_t>(-1)) return;
        check(!flat.set(toggle, "0"), "a change with no settings file anywhere fails");
        check(flat.lastError().find("edvr-flat.ini is not there yet") != std::string::npos,
              "and names edvr-flat.ini, the file that is missing", flat.lastError());
        check(!fileExists(joinPath(dir, L"edvr-flat.ini")) && !fileExists(joinPath(dir, L"edvr.ini")),
              "without creating either");
    }
}

// ---------------------------------------------------------------------------
// the mirror, when only the flat edition's settings were saved
//
// A flat install (or a menu edit under one) mirrors edvr-flat.ini alone, and
// after a game update wipes the folder that file is all there is to recover. The
// offer, and the copy back, used to look for edvr.ini and pass by a mirror that
// held only this one; a flat copy that failed was ignored, and the restore could
// report success after putting back only the shared file.
// ---------------------------------------------------------------------------

static bool noteHas(const std::vector<std::string>& notes, const char* needle) {
    for (const std::string& note : notes)
        if (note.find(needle) != std::string::npos) return true;
    return false;
}

static void testFlatMirror(const std::wstring& scratch) {
    printf("\nthe mirror, when only the flat edition's settings were saved\n");

    // A fresh flat install on real files, the one this mirrors: edvr-flat.ini, the
    // record and the base, and no edvr.ini anywhere.
    const std::wstring dir = joinPath(scratch, L"flatmirror-game");
    layOutFlatGame(dir);
    const PayloadInfo flat = flatPayloadFor(kFlatTemplate);
    const Plan plan = planInstall(diskSurvey(dir), testOptions(), flat);
    check(!plan.blocked && applyPlan(plan, flatProvider()).ok,
          "a fresh flat install, the one this test mirrors, applies");
    check(fileExists(joinPath(dir, L"edvr-flat.ini")) && !fileExists(joinPath(dir, L"edvr.ini")),
          "it leaves edvr-flat.ini and no edvr.ini");

    const std::wstring root = joinPath(scratch, L"flatmirror-root");
    removeTree(root);
    const std::wstring mirrorDir = joinPath(root, L"flatmirror-test");
    const MirrorResult saved = updateMirror(dir, plan.backupDir, mirrorDir);
    check(saved.ok && !saved.saved.empty() && saved.saved.front() == "edvr-flat.ini",
          "updateMirror saves the flat settings");
    check(!fileExists(joinPath(mirrorDir, L"edvr.ini")), "and there is no edvr.ini in the mirror");
    const std::string mirroredFlat = readAll(joinPath(mirrorDir, L"edvr-flat.ini"));
    expectEq(mirroredFlat, readAll(joinPath(dir, L"edvr-flat.ini")), "the mirrored copy matches");

    const MirrorInfo info = readMirror(mirrorDir);
    check(!info.hasIni && info.hasFlatIni, "readMirror sees the flat settings alone");
    check(info.holdsSettings(), "which is a mirror with settings in it");
    check(info.holdsSettingsFor("flat") && !info.holdsSettingsFor("vr"),
          "settings the flat edition can use and the VR edition cannot");
    check(!info.savedUtc.empty(), "and it says when they were saved");
    check(info.hasState && info.hasBaseIni, "with the record and the base beside them");

    {   // The offer, asked as both installers ask it.
        const std::wstring wipedDir = joinPath(scratch, L"flatmirror-wiped");
        layOutFlatGame(wipedDir);
        const Survey wiped = diskSurvey(wipedDir);
        check(offerRestore(hasSettingsFor(wiped, "flat"), info, "flat"),
              "a wiped flat folder is offered a mirror that holds only edvr-flat.ini");
        check(!offerRestore(hasSettingsFor(wiped, "vr"), info, "vr"),
              "a VR install of it is not: the VR edition cannot use that file");
        check(!offerRestore(hasSettingsFor(diskSurvey(dir), "flat"), info, "flat"),
              "a folder that still has its settings is offered nothing");

        writeAll(joinPath(wipedDir, L"edvr.ini"), kSharedIni);
        check(!offerRestore(hasSettingsFor(diskSurvey(wipedDir), "flat"), info, "flat"),
              "nor is one that has only the shared edvr.ini the flat runtime falls back to");

        MirrorInfo none;
        check(!offerRestore(false, none, "flat") && !offerRestore(false, none, "vr"),
              "an empty mirror is offered to nobody");
        MirrorInfo vrOnly;
        vrOnly.hasIni = true;
        check(offerRestore(false, vrOnly, "vr") && offerRestore(false, vrOnly, "flat"),
              "a mirror of edvr.ini is offered to a VR install and, as before, to a flat one");
    }

    {   // The copy, with the flat file alone in the mirror.
        const std::wstring wipedDir = joinPath(scratch, L"flatmirror-restore");
        removeTree(wipedDir);
        makeTree(wipedDir);
        std::vector<std::string> notes;
        check(restoreFromMirror(wipedDir, info, &notes), "a flat-only mirror is restored");
        expectEq(readAll(joinPath(wipedDir, L"edvr-flat.ini")), mirroredFlat,
                 "edvr-flat.ini comes back byte for byte");
        check(!fileExists(joinPath(wipedDir, L"edvr.ini")), "and no edvr.ini is made up");
        check(noteHas(notes, "Restored edvr-flat.ini"), "the notes say what was restored");
        check(fileExists(joinPath(wipedDir, L"edvr_install\\state.ini")) &&
                  fileExists(joinPath(wipedDir, L"edvr_install\\edvr.ini.base")),
              "with the record and the base");
    }

    {   // A flat copy that fails is a restore that failed: with a directory where the
        // file has to go, nothing can replace it.
        const std::wstring blocked = joinPath(scratch, L"flatmirror-blocked");
        removeTree(blocked);
        makeTree(joinPath(blocked, L"edvr-flat.ini"));
        std::vector<std::string> notes;
        check(!restoreFromMirror(blocked, info, &notes), "a failed flat copy fails a flat-only restore");
        check(noteHas(notes, "Could not restore edvr-flat.ini"), "and the notes say which file it was");
        check(!fileExists(joinPath(blocked, L"edvr_install\\state.ini")),
              "nothing else of a mirror whose settings did not come back is put into the folder");
    }

    // A mirror holding both files, made by hand: the shared file and the flat one.
    const std::wstring bothDir = joinPath(root, L"both-mirror");
    makeTree(bothDir);
    writeAll(joinPath(bothDir, L"edvr.ini"), kSharedIni);
    writeAll(joinPath(bothDir, L"edvr-flat.ini"), kFlatOwnIni);
    writeAll(joinPath(bothDir, L"state.ini"), "[edvr]\r\nversion = 1\r\n");
    const MirrorInfo both = readMirror(bothDir);
    check(both.hasIni && both.hasFlatIni, "a mirror can hold both files");

    {
        const std::wstring wipedDir = joinPath(scratch, L"flatmirror-both");
        removeTree(wipedDir);
        makeTree(wipedDir);
        std::vector<std::string> notes;
        check(restoreFromMirror(wipedDir, both, &notes), "a mirror of both files restores");
        expectEq(readAll(joinPath(wipedDir, L"edvr.ini")), kSharedIni, "edvr.ini comes back");
        expectEq(readAll(joinPath(wipedDir, L"edvr-flat.ini")), kFlatOwnIni, "and edvr-flat.ini");
    }

    {   // The shared file's success must not hide the flat file's failure.
        const std::wstring blocked = joinPath(scratch, L"flatmirror-flatblocked");
        removeTree(blocked);
        makeTree(joinPath(blocked, L"edvr-flat.ini"));
        std::vector<std::string> notes;
        check(!restoreFromMirror(blocked, both, &notes),
              "edvr.ini restored and edvr-flat.ini not is not a successful restore");
        expectEq(readAll(joinPath(blocked, L"edvr.ini")), kSharedIni, "the shared file did come back");
        check(noteHas(notes, "Restored edvr.ini") && noteHas(notes, "Could not restore edvr-flat.ini"),
              "and the notes name both the one that did and the one that did not");
        check(fileExists(joinPath(blocked, L"edvr_install\\state.ini")),
              "the record is restored, since some settings came back");
    }

    {   // And the other way round.
        const std::wstring blocked = joinPath(scratch, L"flatmirror-sharedblocked");
        removeTree(blocked);
        makeTree(joinPath(blocked, L"edvr.ini"));
        std::vector<std::string> notes;
        check(!restoreFromMirror(blocked, both, &notes),
              "edvr-flat.ini restored and edvr.ini not is not a successful restore either");
        expectEq(readAll(joinPath(blocked, L"edvr-flat.ini")), kFlatOwnIni, "the flat file did come back");
        check(noteHas(notes, "Could not restore edvr.ini"), "and the notes say edvr.ini did not");
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

    // A process list that could not be taken is not "nobody is running". It
    // returned NotRunning for years: a failed CreateToolhelp32Snapshot read as
    // a machine with no game on it. It cannot be made to fail on demand, so the
    // snapshot is handed in.
    check(runStateOfSnapshot(INVALID_HANDLE_VALUE, name.c_str(), dir) == GameRunState::Unknown,
          "a snapshot that could not be taken is not provably stopped");
    check(runStateOfSnapshot(nullptr, name.c_str(), dir) == GameRunState::Unknown,
          "nor is a null one");
    check(runStateOfSnapshot(INVALID_HANDLE_VALUE, L"a-name-nothing-on-this-machine-has.exe",
                             dir) == GameRunState::Unknown,
          "whatever name was asked about: with no list there is no answer");
    check(runStateOfSnapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0), name.c_str(), dir) ==
              GameRunState::ThisFolder,
          "and a snapshot that was taken answers exactly as runStateOf does");
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

    // A percentage row round-trips what is typed into it. Sharpening shipped
    // as a TEXT row for three releases: the schema typed it off the menu's
    // getString echo of the file rather than the pass's getFloat, and a text
    // row hands typing to the file verbatim while the display multiplies a
    // percent row by 100 whatever its kind -- so "10%" was shown, "20" was
    // typed, 20 was written and "2000%" came back (issue 35). The curve row
    // beside it, read only as a float, always worked. Both are held to the
    // same round trip here, against the table the build just generated.
    int textPercent = 0;
    for (const SettingRow& row : model.rows()) {
        if (row.def->percent && row.def->kind != SettingKind::Number) ++textPercent;
    }
    check(textPercent == 0, "every percentage row is a number row");

    for (const char* key : {"render_sharpness", "panel_curvature"}) {
        size_t at = SIZE_MAX;
        for (size_t i = 0; i < model.rows().size(); ++i) {
            if (std::string(model.rows()[i].def->key) == key) at = i;
        }
        if (at == SIZE_MAX) {
            fail("find the percentage row to exercise", key);
            continue;
        }
        const SettingRow& row = model.rows()[at];
        std::string label = std::string(key) + ": ";
        check(row.def->percent && row.def->kind == SettingKind::Number,
              (label + "is a number shown as a percentage").c_str());
        std::string file;
        check(row.parseTyped(L"20", &file) && file == "0.2",
              (label + "typing 20 means twenty percent, 0.2 in the file").c_str(), file);
        check(row.parseTyped(L"20%", &file) && file == "0.2",
              (label + "and so does typing 20%").c_str(), file);
        check(row.parseTyped(L"0.2", &file) && file == "0.2",
              (label + "a fraction typed as the file holds it is left alone").c_str(), file);
        check(!row.parseTyped(L"lots", &file), (label + "a word is refused").c_str());
        check(model.set(at, "0.2"), (label + "writing the parsed value succeeds").c_str(),
              model.lastError());
        expectEq(toUtf8(model.rows()[at].shown()), "20%",
                 (label + "and the row shows it as 20%").c_str());
    }

    // The on-foot width's bounds come from edvr.ini's own `| range` annotation
    // now: fix.vscreen_res_width is "auto" or a width, read with getString by
    // every consumer (the panel patch and the intro upscaler both resolve it
    // through resolveVScreenTargetResolution), so no typed, bounded read is
    // left for the generator to detect on its own -- see apply_annotation's
    // fallback in gen_settings_schema.py.
    for (const SettingRow& row : model.rows()) {
        if (std::string(row.def->key) != "vscreen_res_width") continue;
        expectEq(std::string(row.def->lo) + ".." + row.def->hi, "640..8192",
                 "vscreen_res_width: carries the range the code clamps to");
    }
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

// Independent stored-ZIP reader checks the local payload and its CRC, rather
// than accepting a central directory full of names as evidence of a valid ZIP.
static bool zipPayloadEquals(const std::wstring& path,const std::string& wanted,const std::string& expected) {
    const auto data=readAll(path);
    auto u16=[&](size_t at){return unsigned(static_cast<unsigned char>(data[at])) |
        (unsigned(static_cast<unsigned char>(data[at+1]))<<8);};
    auto u32=[&](size_t at){return u16(at)|(static_cast<unsigned long>(u16(at+2))<<16);};
    size_t at=0;
    while(at+30<=data.size() && u32(at)==0x04034b50) {
        const auto size=u32(at+18);const auto length=u16(at+26);const auto extra=u16(at+28);
        const size_t start=at+30+length+extra;
        if(start>data.size()||size>data.size()-start||u16(at+8)!=0)return false;
        const auto body=data.substr(start,size);
        unsigned long crc=0xffffffffu;
        for(unsigned char byte:body){crc^=byte;for(unsigned bit=0;bit<8;++bit)crc=(crc&1)?(crc>>1)^0xedb88320u:crc>>1;}
        if((crc^0xffffffffu)!=u32(at+14))return false;
        if(data.substr(at+30,length)==wanted)return body==expected;
        at=start+size;
    }
    return false;
}

static void bundleStamp(const std::wstring& path,WORD day,WORD minute,WORD second=0) {
    SYSTEMTIME st{};st.wYear=2026;st.wMonth=8;st.wDay=day;st.wHour=14;st.wMinute=minute;st.wSecond=second;
    FILETIME stamp{};SystemTimeToFileTime(&st,&stamp);
    HANDLE file=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    check(file!=INVALID_HANDLE_VALUE,"capture fixture timestamp file opens");
    if(file!=INVALID_HANDLE_VALUE){check(SetFileTime(file,nullptr,nullptr,&stamp)!=0,"capture fixture timestamp is set");CloseHandle(file);}
}

static bool bundleSparse(const std::wstring& path,unsigned long long size) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    DWORD returned=0;
    // FSCTL_SET_SPARSE, kept numeric to avoid changing the rig's include set.
    const bool sparse=DeviceIoControl(file,0x000900c4,nullptr,0,nullptr,0,&returned,nullptr)!=0;
    LARGE_INTEGER end{};end.QuadPart=static_cast<LONGLONG>(size);
    const bool ok=sparse&&SetFilePointerEx(file,end,nullptr,FILE_BEGIN)&&SetEndOfFile(file);
    CloseHandle(file);return ok;
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
    writeAll(joinPath(logs, L"edvr_openxr_20260827_140004_123_4567.log"), "the native session that matters");
    // A second gfx log opened in the same second as another takes the native
    // trace's _mmm_pid suffix. Its stamp must still be read off its name: by
    // write time it would be the newest log by weeks, alone in its session,
    // and the three above would be left out.
    writeAll(joinPath(logs, L"edvr_gfx_20260827_140000_500_9.log"), "a second process, same second");

    for(const auto* name:{L"edvr_gfx_20260827_140000.log",L"edvr_vr_20260827_140003.log",
                          L"edvr_openxr_20260827_140004_123_4567.log",L"edvr_gfx_20260827_140000_500_9.log"})
        bundleStamp(joinPath(logs,name),27,12);
    const auto packetRoot=joinPath(logs,L"flat_draw_packets");
    auto packetFixture=[&](const wchar_t* folder,WORD minute,bool manifest=true){
        const auto group=joinPath(packetRoot,folder);makeTree(group);
        writeAll(joinPath(group,L"payload.bin"),std::string((1u<<20)+37,'p'));
        bundleStamp(joinPath(group,L"payload.bin"),27,minute);
        if(manifest){writeAll(joinPath(group,L"manifest.json"),"{\"complete\":false}");bundleStamp(joinPath(group,L"manifest.json"),27,minute);}
        return group;
    };
    const auto late=packetFixture(L"late",10);
    const auto resolveRoot=joinPath(logs,L"flat_pixels");
    const auto resolveLate=joinPath(resolveRoot,L"resolve_inputs_late");makeTree(resolveLate);
    writeAll(joinPath(resolveLate,L"manifest.json"),"{\"stage\":\"prebackend\",\"backend_status\":\"not-run\"}");
    writeAll(joinPath(resolveLate,L"inputs_frame_1.json"),"{\"complete\":true}");
    writeAll(joinPath(resolveLate,L"overlay.bin"),"full-plane-mask");
    bundleStamp(joinPath(resolveLate,L"manifest.json"),27,10);
    bundleStamp(joinPath(resolveLate,L"inputs_frame_1.json"),27,10);
    bundleStamp(joinPath(resolveLate,L"overlay.bin"),27,2);
    const auto resolveMissing=joinPath(resolveRoot,L"resolve_inputs_missing");makeTree(resolveMissing);
    writeAll(joinPath(resolveMissing,L"overlay.bin"),"orphan");bundleStamp(joinPath(resolveMissing,L"overlay.bin"),27,10);
    // Member straddles the old three-minute boundary while the manifest is
    // ten minutes into a session. The selected folder must retain both.
    bundleStamp(joinPath(late,L"payload.bin"),27,2);
    packetFixture(L"missing_manifest",11,false);
    packetFixture(L"within_grace",14);
    packetFixture(L"after_grace",16);
    const auto oversized=packetFixture(L"oversized_member",11);
    const auto oversizedFile=joinPath(oversized,L"oversized.bin");
    check(bundleSparse(oversizedFile,(64ull<<20)+1),"per-file capture limit fixture is sparse");
    bundleStamp(oversizedFile,27,11);
    const auto prior=packetFixture(L"previous",0);bundleStamp(joinPath(prior,L"manifest.json"),26,0);
    const auto budget=packetFixture(L"over_budget",11);
    for(unsigned i=0;i<25;++i){const auto path=joinPath(budget,L"chunk_"+std::to_wstring(i)+L".bin");
        check(bundleSparse(path,64ull<<20),"atomic-budget fixture is sparse");bundleStamp(path,27,11);}

    const LogBundle bundle = collectLogs(dir, scratch);
    check(bundle.ok, "the bundle is written", bundle.error);
    if (!bundle.ok) return;
    check(fileExists(bundle.zipPath), "the zip is on disk");

    const std::vector<std::string> names = zipEntryNames(bundle.zipPath);
    check(!names.empty(), "the zip has a readable central directory");
    check(bundleHas(names, "edvr_gfx_20260827_140000.log"), "the newest session's gfx log is in");
    check(bundleHas(names, "edvr_vr_20260827_140003.log"), "and its vr log");
    check(bundleHas(names, "edvr_openxr_20260827_140004_123_4567.log"), "and its native OpenXR log");
    check(bundleHas(names, "edvr_gfx_20260827_140000_500_9.log"),
          "and the same-second gfx log, dated by its name not its write time");
    check(!bundleHas(names, "edvr_gfx_20260101_100000.log"),
          "the previous session is left out");
    check(bundleHas(names, "edvr_breadcrumbs.txt"), "the breadcrumbs are in");
    check(bundleHas(names, "edvr_FATAL.txt"), "the fatal note is in");
    check(bundleHas(names, "edvr.ini"), "the settings file is in");
    check(bundleHas(names,"flat_draw_packets/late/manifest.json")&&bundleHas(names,"flat_draw_packets/late/payload.bin"),
          "packet capture ten minutes into a flight retains its whole folder across member timestamp boundaries");
    check(bundleHas(names,"flat_pixels/resolve_inputs_late/manifest.json") &&
          bundleHas(names,"flat_pixels/resolve_inputs_late/inputs_frame_1.json") &&
          zipPayloadEquals(bundle.zipPath,"flat_pixels/resolve_inputs_late/overlay.bin","full-plane-mask"),
          "prebackend input capture remains atomic ten minutes into a flight with intact mask CRC");
    check(!bundleHas(names,"flat_pixels/resolve_inputs_missing/overlay.bin"),"prebackend input folder without manifest is omitted atomically");
    check(!bundleHas(names,"flat_draw_packets/missing_manifest/payload.bin"),"packet folder without manifest is omitted atomically");
    check(!bundleHas(names,"flat_draw_packets/over_budget/manifest.json")&&!bundleHas(names,"flat_draw_packets/over_budget/payload.bin"),"over-budget packet group retains no misleading manifest or partial payload");
    check(!bundleHas(names,"flat_draw_packets/previous/manifest.json"),"packet from earlier session is omitted");
    check(bundleHas(names,"flat_draw_packets/within_grace/manifest.json")&&!bundleHas(names,"flat_draw_packets/after_grace/manifest.json"),
          "packet folder selection permits three minutes after actual log end and rejects later evidence");
    check(!bundleHas(names,"flat_draw_packets/oversized_member/manifest.json"),"one oversized member omits the entire packet capture");
    check(zipPayloadEquals(bundle.zipPath,"flat_draw_packets/late/payload.bin",std::string((1u<<20)+37,'p')),
          "streamed ZIP payload crossing one MiB extracts exactly and has an independently verified CRC");
    bool missingNote=false,budgetNote=false;
    for(const auto& note:bundle.notes){missingNote|=note.find("manifest missing")!=std::string::npos;budgetNote|=note.find("bundle budget")!=std::string::npos;}
    check(missingNote&&budgetNote,"atomic capture omissions state manifest and budget reasons");
    {
        const auto huge=joinPath(scratch,L"zip32-sparse.bin");const auto out=joinPath(scratch,L"zip32-rejected.zip");
        check(bundleSparse(huge,0xF0000001ull),"ZIP32 boundary fixture uses sparse disk storage");
        std::string error;std::vector<std::wstring> skipped;
        check(!writeZip(out,{huge},{L"huge.bin"},&error,&skipped)&&error.find("ZIP32")!=std::string::npos,
              "ZIP32 bound fails before reading or allocating a multi-GiB payload");
        check(!fileExists(out),"failed ZIP32 write deletes its partial output");
        const auto missing=joinPath(scratch,L"absent-packet.bin");
        check(!writeZip(out,{joinPath(late,L"manifest.json"),missing},
              {L"flat_draw_packets/late/manifest.json",L"flat_draw_packets/late/payload.bin"},&error,&skipped),
              "packet member disappearing during bundling fails the whole ZIP");
        check(!fileExists(out),"packet read failure removes ZIP after an earlier member was written");
    }

    {   // A flat install's logs are found where ITS settings file says: edvr-flat.ini
        // (edvr.ini only while it has none). The VR profile's log.dir, which the
        // flat runtime never reads, does not send the bundle to the wrong folder.
        const std::wstring flat = joinPath(scratch, L"flatlogs");
        const std::wstring flatLogs = joinPath(flat, L"flat_logs_here");
        removeTree(flat);
        makeTree(flatLogs);
        writeAll(joinPath(flat, L"EliteDangerous64.exe"), "not really the game");
        writeAll(joinPath(flat, L"edvr_profile.ini"), "[install]\r\nschema = 1\r\nprofile = flat\r\n");
        writeAll(joinPath(flat, L"edvr.ini"), "[log]\r\ndir = " + toUtf8(joinPath(flat, L"vr_logs_nowhere")) + "\r\n");
        writeAll(joinPath(flat, L"edvr-flat.ini"), "[log]\r\ndir = " + toUtf8(flatLogs) + "\r\n");
        writeAll(joinPath(flatLogs, L"edvr_gfx_20260827_140000.log"), "the flat session");
        const LogBundle flatBundle = collectLogs(flat, scratch);
        check(flatBundle.ok, "a flat install's bundle is written", flatBundle.error);
        const std::vector<std::string> flatNames = zipEntryNames(flatBundle.zipPath);
        check(bundleHas(flatNames, "edvr_gfx_20260827_140000.log"),
              "with the log from the folder edvr-flat.ini names");
        check(bundleHas(flatNames, "edvr-flat.ini"), "and the flat settings file");

        // Without a flat file yet, the flat runtime is reading edvr.ini, so that is
        // where log.dir comes from.
        DeleteFileW(joinPath(flat, L"edvr-flat.ini").c_str());
        writeAll(joinPath(flat, L"edvr.ini"), "[log]\r\ndir = " + toUtf8(flatLogs) + "\r\n");
        const LogBundle legacyBundle = collectLogs(flat, scratch);
        check(legacyBundle.ok, "a legacy flat install's bundle is written", legacyBundle.error);
        check(bundleHas(zipEntryNames(legacyBundle.zipPath), "edvr_gfx_20260827_140000.log"),
              "with the log from the folder the shared edvr.ini names");
    }

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
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const std::wstring root = argc > 1 ? argv[1] : L".";
    const std::wstring scratch = argc > 2 ? argv[2] : joinPath(root, L"build\\insttest_scratch");
    makeTree(scratch);
    edvr::installer::test::nativeContractCases([](bool ok, const char* what) { check(ok, what); });
    edvr::installer::test::nativeBuiltContractCases([](bool ok,const char* what){check(ok,what);},root);
    edvr::installer::native_apply_cases::run([](bool ok, const char* what) { check(ok, what); }, joinPath(scratch, L"native_apply"), placeFile);

    printf("installer_test: root %ls\n", root.c_str());

    testMerge();
    testShippedIni(root);
    testChangedDefault(root);
    testChangedDefaultsOn(root);
    testPlanner();
    testNativePlanner();
    testFlatPlanner();
    testFlatSettingsPlanner();
    testApply(scratch);
    testMirror(scratch);
    testAtomicWrite(scratch);
    testReplaceScripts(scratch);
    testApplyPatience(scratch);
    testGenerations(scratch);
    testMirrorGenerations(scratch);
    testDemotedTrims(root, scratch);
    testFlatSettingsFiles(scratch);
    testFlatSettingsWindow(root, scratch);
    testFlatMirror(scratch);
    testProbe(scratch);
    testRunState();
    testSettings(root, scratch);
    testLogBundle(scratch);
    testState();

    printf("\n%s\n", g_fails == 0 ? "installer_test: all good" : "installer_test: FAILURES");
    return g_fails == 0 ? 0 : 1;
}
