#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>

#include "../../include/edvr_plugin_api.h"
#include "../../src/plugins/plugin_manager.h"
#include "../../src/plugins/plugin_setting_restore.h"
#include "../../src/plugins/plugin_row_registry.h"
#include "../../src/common/config.h"
#include "../../src/common/log.h"
#include "../../src/common/runtime_profile.h"
#include "../rig_graphics_mode.h"

// Stubs for d3d11 and input gate functions required by plugin_manager
// The F8 menu's settings registry, reduced to what the manager's ownership contract needs.
struct FakeSetting { std::string owner; std::string key; };
static std::vector<FakeSetting> g_fakeSettings;
static int g_fakeRegisteredEver = 0;

// What the input gate is told: the last answer, and how many times it was told to clear.
static bool g_gateBlocked = false;
static int g_gateClears = 0;
static int g_gateInstalls = 0;   // times a loading addon asked for the user32 doors
static bool g_fakeRowsHidden = false;   // what the menu was last told about the addons' rows

namespace edvr {
int menuRegisterPluginSetting(const char* ownerId, const EdvrPluginSettingDef* setting) {
    if (!setting) return -1;
    g_fakeSettings.push_back({ ownerId ? ownerId : "", setting->key ? setting->key : "" });
    ++g_fakeRegisteredEver;
    return 0;
}

void menuRemovePluginSettings(const char* ownerId) {
    std::string o = ownerId ? ownerId : "";
    for (size_t i = g_fakeSettings.size(); i-- > 0;) {
        if (g_fakeSettings[i].owner == o) g_fakeSettings.erase(g_fakeSettings.begin() + static_cast<ptrdiff_t>(i));
    }
}

void menuSetPluginRowsHidden(bool hidden) {
    g_fakeRowsHidden = hidden;
}

void inputGateInstallForPlugin() {
    ++g_gateInstalls;
}

void inputGateSetPluginBlock(bool block) {
    if (g_gateBlocked && !block) ++g_gateClears;
    g_gateBlocked = block;
}

void breadcrumb(const char* stage) {
    (void)stage;
}
}


namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", msg);
    if (!cond) ++g_failures;
}

// "" is the healthy default fixture; "b", "regfault", "initfail", "initfault", "settings" are its variants.
std::wstring getFixtureDllPath(const wchar_t* variant = L"") {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path p(exePath);
    std::wstring name = L"test_addon_fixture";
    if (variant && variant[0]) name += std::wstring(L"_") + variant;
    return (p.parent_path() / (name + L".dll")).wstring();
}

// A fresh sandbox with an empty plugins directory and a bare edvr.ini.
std::filesystem::path makeSandbox(const wchar_t* tag) {
    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    std::filesystem::path sb = std::filesystem::path(tempPath) /
        (std::wstring(L"edvr_addon_test_") + tag + L"_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(sb, ec);
    std::filesystem::create_directories(sb / L"plugins");
    std::ofstream iniOut(sb / L"edvr.ini");
    iniOut << "[general]\nmode = vr\n";
    return sb;
}

void dropFixture(const std::filesystem::path& sandbox, const wchar_t* variant, const wchar_t* fileName) {
    std::filesystem::copy_file(getFixtureDllPath(variant), sandbox / L"plugins" / fileName,
                               std::filesystem::copy_options::overwrite_existing);
}

size_t fakeSettingsOwnedBy(const char* owner) {
    size_t n = 0;
    for (const auto& s : g_fakeSettings) {
        if (s.owner == owner) ++n;
    }
    return n;
}

// A copy of the named candidate as of this call; re-call it to see later changes.
std::optional<edvr::plugins::CandidatePlugin> findCandidate(edvr::plugins::PluginManager& pm, const char* stem) {
    for (const auto& c : pm.candidatesSnapshot()) {
        if (c.stem == stem) return c;
    }
    return std::nullopt;
}

} // namespace

// PR #92 review: the manager is never destroyed by the CRT. Its destructor ran shutdown() (a lock and addon callbacks) during DLL process
// detach, under the loader lock, bypassing the proxy's reserved check. This runs at exit after main (atexit handlers run in reverse
// order of registration, and main registers this before first touching the singleton): the addon left loaded must not have been shut
// down by anything at all.
static std::string g_shutdownMark;
static void checkNoShutdownAtExit() {
    const bool ran = GetFileAttributesA(g_shutdownMark.c_str()) != INVALID_FILE_ATTRIBUTES;
    printf("  %s  singleton: nothing shut the manager's addons down at process exit\n", ran ? "FAIL" : "ok  ");
    DeleteFileA(g_shutdownMark.c_str());
    if (ran) {
        printf("ADDON MANAGER TEST FAILED (static destruction ran the manager's shutdown)\n");
        fflush(stdout);
        _exit(1);
    }
}

int main(int argc, char** argv) {
    bool dryRun = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dry-run") == 0) dryRun = true;
    }

    if (dryRun) {
        printf("addon_manager_test: dry run passed\n");
        return 0;
    }

    printf("[addon_manager_test] running test cases...\n");

    // Config::init takes the profile from the machine's Elite Settings.xml and mirrors settings under %LOCALAPPDATA%\EDVR. Run by hand on
    // a machine last flown flat, this rig went dormant and wrote the real mirror: pin a VR fixture first (tools\rig_graphics_mode.h).
    static const std::wstring s_fixtureRoot = edvr::rig::rigFixtureRoot(L"addon_manager");
    if (!edvr::rig::pinRigGraphicsMode(s_fixtureRoot, 4)) {
        printf("  FAIL  could not pin the rig's graphics mode fixture\n");
        return 1;
    }
    atexit([] { edvr::rig::removeRigTree(s_fixtureRoot); });

    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    std::filesystem::path sandbox = std::filesystem::path(tempPath) / (L"edvr_addon_test_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(sandbox / L"plugins");

    // Case 1: SHA-256 calculation
    {
        std::filesystem::path shaTestFile = sandbox / L"test_sha.txt";
        {
            std::ofstream out(shaTestFile, std::ios::binary);
            out << "Antigravity EDVR Plugin Test\n";
        }
        std::string calculated = edvr::plugins::PluginManager::calculateSha256(shaTestFile.wstring());
        check(!calculated.empty() && calculated.length() == 64, "SHA-256 calculation produces 64-char hex digest");
    }

    // Case 2: Trust-On-First-Use: Unapproved DLL rejection without LoadLibraryW
    std::wstring fixturePath = getFixtureDllPath();
    std::filesystem::path targetPlugin = sandbox / L"plugins" / L"test_addon.dll";
    std::filesystem::copy_file(fixturePath, targetPlugin, std::filesystem::copy_options::overwrite_existing);

    // Initial config without [addons] entry
    {
        std::ofstream iniOut(sandbox / L"edvr.ini");
        iniOut << "[general]\nmode = vr\n";
    }

    // Registered before the singleton is first touched: atexit handlers and function-static destructors run in reverse order of
    // registration, so this one runs after a static singleton would already have been destroyed.
    {
        char tmp[MAX_PATH]{};
        GetTempPathA(MAX_PATH, tmp);
        g_shutdownMark = std::string(tmp) + "edvr_addon_shutdown_mark_" + std::to_string(GetCurrentProcessId()) + ".txt";
        DeleteFileA(g_shutdownMark.c_str());
        SetEnvironmentVariableA("EDVR_FIXTURE_SHUTDOWN_MARK", g_shutdownMark.c_str());
        atexit(&checkNoShutdownAtExit);
    }
    auto& pm = edvr::plugins::PluginManager::instance();
    bool initOk = pm.initialize(sandbox.wstring());
    check(initOk, "PluginManager::initialize succeeds on sandbox");

    const auto cands = pm.candidatesSnapshot();
    check(cands.size() == 1, "PluginManager discovers candidate DLL in plugins/ directory");
    if (!cands.empty()) {
        check(cands[0].stem == "test_addon", "Candidate stem matches file name");
        check(!cands[0].approved, "Candidate is unapproved by default");
    }
    check(!pm.hasActivePlugins(), "Unapproved plugin is NOT active");
    check(pm.activePluginCount() == 0, "Active plugin count is 0 for unapproved plugins");
    check(GetModuleHandleW(L"test_addon.dll") == nullptr, "Unapproved DLL is never loaded via LoadLibraryW");

    // Case 3: Approval and Version Negotiation
    std::string sha = cands.empty() ? "" : cands[0].sha256;
    check(!sha.empty(), "Candidate SHA-256 is non-empty before approval");

    bool approved = pm.approvePlugin("test_addon", sha);
    check(approved, "approvePlugin succeeds and loads plugin");
    check(pm.hasActivePlugins(), "Approved plugin becomes active");
    check(pm.activePluginCount() == 1, "Active plugin count is 1 after approval");

    HMODULE hAddon = GetModuleHandleW(L"test_addon.dll");
    check(hAddon != nullptr, "Approved plugin module is loaded in process");

    // Case 4: Dispatch Lifecycle Hooks
    typedef void (WINAPI *PFN_SetCrashMode)(int);
    typedef int (WINAPI *PFN_GetCount)();

    auto setCrashMode = reinterpret_cast<PFN_SetCrashMode>(GetProcAddress(hAddon, "fixtureSetCrashMode"));
    auto getUpdateCount = reinterpret_cast<PFN_GetCount>(GetProcAddress(hAddon, "fixtureGetUpdateCount"));
    auto getRenderCount = reinterpret_cast<PFN_GetCount>(GetProcAddress(hAddon, "fixtureGetRenderCount"));

    check(setCrashMode && getUpdateCount && getRenderCount, "Fixture helper exports resolved");

    EdvrPosef headPose{};
    pm.onUpdate(headPose, 0.016f);
    if (getUpdateCount) {
        check(getUpdateCount() == 1, "onUpdate dispatched cleanly to addon");
    }

    EdvrEyeRenderContext eyeCtx{};
    eyeCtx.structSize = sizeof(EdvrEyeRenderContext);
    pm.onRenderEye(eyeCtx);
    if (getRenderCount) {
        check(getRenderCount() == 1, "onRenderEye dispatched cleanly to addon");
    }

    // DirectInput filter hook
    bool swallowed = pm.onFilterInput(1, nullptr);
    check(swallowed, "onFilterInput correctly intercepts and swallows input when requested");

    // Case 5: SEH Fault Containment
    if (setCrashMode) {
        setCrashMode(2); // crash onRenderEye
        pm.onRenderEye(eyeCtx);
        check(true, "SEH caught deliberate access violation in onRenderEye without crashing process");
        check(!pm.hasActivePlugins(), "Faulting plugin was automatically disabled for session");
    }

    // Case 6: Shutdown
    pm.shutdown();
    check(!pm.hasActivePlugins(), "PluginManager::shutdown successfully unloads all plugins");

    // Case 7 (PR #92 review): a fault inside EdvrPluginRegister is contained like any other lifecycle fault.
    {
        auto sb = makeSandbox(L"regfault");
        dropFixture(sb, L"regfault", L"reg_fault.dll");
        dropFixture(sb, L"", L"healthy.dll");
        pm.initialize(sb.wstring());
        auto bad = findCandidate(pm, "reg_fault");
        auto good =findCandidate(pm, "healthy");
        check(bad && good, "regfault: both candidates discovered");
        if (bad && good) {
            bool badLoaded = pm.approvePlugin("reg_fault", bad->sha256);
            check(!badLoaded, "regfault: candidate whose registration faults is rejected");
            check(GetModuleHandleW(L"reg_fault.dll") == nullptr, "regfault: faulting candidate is unloaded");
            check(pm.activePluginCount() == 0, "regfault: nothing active after the rejected candidate");
            bool goodLoaded = pm.approvePlugin("healthy", good->sha256);
            check(goodLoaded, "regfault: a healthy candidate still loads afterwards");
            check(pm.activePluginCount() == 1, "regfault: exactly the healthy candidate is active");
        }
        pm.shutdown();
        std::error_code ec7;
        std::filesystem::remove_all(sb, ec7);
    }

    // Case 8 (PR #92 review): approval binds to the bytes loaded, not the bytes seen at discovery.
    {
        auto sb = makeSandbox(L"swap");
        dropFixture(sb, L"", L"swap.dll");
        pm.initialize(sb.wstring());
        auto cand = findCandidate(pm, "swap");
        check(cand.has_value(), "swap: candidate discovered");
        std::string shaA = cand ? cand->sha256 : "";
        // Replace the file after discovery, before the user approves the hash they were shown.
        dropFixture(sb, L"b", L"swap.dll");
        std::string shaB = edvr::plugins::PluginManager::calculateSha256((sb / L"plugins" / L"swap.dll").wstring());
        check(!shaA.empty() && !shaB.empty() && shaA != shaB, "swap: the replacement has a different hash");

        bool loaded = pm.approvePlugin("swap", shaA);
        check(!loaded, "swap: approval of the old hash does not load the replaced file");
        check(GetModuleHandleW(L"swap.dll") == nullptr, "swap: the replacement DLL was never loaded");
        check(pm.activePluginCount() == 0, "swap: nothing active after the refused load");
        auto after =findCandidate(pm, "swap");
        check(after && after->sha256 == shaB && !after->approved,
              "swap: the changed file is resurfaced as an unapproved candidate under its new hash");

        bool reloaded = pm.approvePlugin("swap", shaB);
        check(reloaded, "swap: approving the new hash loads the file");
        HMODULE hB = GetModuleHandleW(L"swap.dll");
        typedef int (WINAPI *PFN_GetVariant)();
        auto getVariant = hB ? reinterpret_cast<PFN_GetVariant>(GetProcAddress(hB, "fixtureGetVariant")) : nullptr;
        check(getVariant && getVariant() == 2, "swap: the code that loaded is the approved replacement");
        pm.shutdown();
        std::error_code ec8;
        std::filesystem::remove_all(sb, ec8);
    }

    // Case 9 (PR #92 review): a plugin's settings die with its initialisation or its life.
    {
        auto sb = makeSandbox(L"settings");
        dropFixture(sb, L"settings", L"keeper.dll");
        dropFixture(sb, L"initfail", L"fails.dll");
        dropFixture(sb, L"initfault", L"faults.dll");
        pm.initialize(sb.wstring());
        g_fakeSettings.clear();
        g_fakeRegisteredEver = 0;
        auto keeper = findCandidate(pm, "keeper");
        auto fails = findCandidate(pm, "fails");
        auto faults =findCandidate(pm, "faults");
        check(keeper && fails && faults, "settings: all three candidates discovered");
        if (keeper && fails && faults) {
            check(pm.approvePlugin("keeper", keeper->sha256), "settings: the healthy settings addon loads");
            check(fakeSettingsOwnedBy("keeper") == 2, "settings: the healthy addon's two settings are registered");

            check(!pm.approvePlugin("fails", fails->sha256), "settings: init that returns an error is rejected");
            check(!pm.approvePlugin("faults", faults->sha256), "settings: init that faults is rejected");
            check(g_fakeRegisteredEver == 6, "settings: the failing addons really did register settings first");
            check(fakeSettingsOwnedBy("fails") == 0, "settings: none remain after an init error");
            check(fakeSettingsOwnedBy("faults") == 0, "settings: none remain after an init fault");
            check(fakeSettingsOwnedBy("keeper") == 2, "settings: the other addon's settings are untouched");
        }
        pm.shutdown();
        check(g_fakeSettings.empty(), "settings: shutdown removes the retired addon's settings");
        std::error_code ec9;
        std::filesystem::remove_all(sb, ec9);
    }

    // Case 9b (PR #92 review follow-up): an addon that faults at run time retires its settings like one that fails init.
    {
        auto sb = makeSandbox(L"renderfault");
        dropFixture(sb, L"settings", L"keeper.dll");
        dropFixture(sb, L"renderfault", L"rfaults.dll");
        pm.initialize(sb.wstring());
        g_fakeSettings.clear();
        auto keeper = findCandidate(pm, "keeper");
        auto rfaults =findCandidate(pm, "rfaults");
        check(keeper && rfaults, "runtimefault: both candidates discovered");
        if (keeper && rfaults) {
            check(pm.approvePlugin("keeper", keeper->sha256), "runtimefault: the healthy settings addon loads");
            check(pm.approvePlugin("rfaults", rfaults->sha256), "runtimefault: the render-faulting addon loads (init is clean)");
            check(fakeSettingsOwnedBy("rfaults") == 2, "runtimefault: its settings are registered before it faults");
            EdvrEyeRenderContext rctx{};
            rctx.structSize = sizeof(rctx);
            pm.onRenderEye(rctx);
            check(fakeSettingsOwnedBy("rfaults") == 0, "runtimefault: its settings are gone after it faults in onRenderEye");
            check(fakeSettingsOwnedBy("keeper") == 2, "runtimefault: the healthy addon's settings survive");
        }
        pm.shutdown();
        std::error_code ec9b;
        std::filesystem::remove_all(sb, ec9b);
    }

    // Case 9c (PR #92 review follow-up): the menu's candidate list is a copy, so it can be held and walked
    // after the manager's lock is released (the lock-order fix depends on that); later approvals do not change it.
    {
        auto sb = makeSandbox(L"snapshot");
        dropFixture(sb, L"", L"snap.dll");
        pm.initialize(sb.wstring());
        std::vector<edvr::plugins::CandidatePlugin> snap = pm.candidatesSnapshot();
        check(snap.size() == 1 && !snap[0].approved, "snapshot: one unapproved candidate is copied out");
        if (snap.size() == 1) {
            pm.approveCandidate(snap[0].id);
            check(!snap[0].approved, "snapshot: the copy does not change when the candidate is approved afterwards");
            check(pm.candidatesSnapshot().size() == 1 && pm.candidatesSnapshot()[0].approved, "snapshot: a fresh copy shows the approval");
        }
        pm.shutdown();
        std::error_code ec9c;
        std::filesystem::remove_all(sb, ec9c);
    }

    // Case 10 (PR #92 review): approving the same candidate twice is a no-op the second time.
    {
        auto sb = makeSandbox(L"twice");
        dropFixture(sb, L"", L"plain.dll");
        pm.initialize(sb.wstring());
        auto cand = findCandidate(pm, "plain");
        check(cand.has_value(), "twice: candidate discovered");
        std::string sha = cand ? cand->sha256 : "";
        check(pm.approvePlugin("plain", sha), "twice: first approval loads the addon");
        pm.approvePlugin("plain", sha);
        check(pm.activePluginCount() == 1, "twice: a second approval of the same candidate loads nothing more");
        // The menu row holds a candidate id and nothing else.
        uint32_t id = cand ? cand->id : 0;
        check(id != 0, "twice: the candidate has a stable id");
        edvr::plugins::ApprovalResult again = pm.approveCandidate(id);
        check(!again.applied && !again.loaded, "twice: activating the approved candidate's row again is a no-op");
        edvr::plugins::ApprovalResult stale = pm.approveCandidate(987654u);
        check(!stale.applied, "twice: an id that names no candidate is a no-op");
        check(pm.activePluginCount() == 1, "twice: still exactly one active addon after the repeated activations");
        pm.shutdown();
        std::error_code ec10;
        std::filesystem::remove_all(sb, ec10);
    }

    // Case 11: by-id approval, the entry point the menu row calls.
    {
        auto sb = makeSandbox(L"byid");
        dropFixture(sb, L"", L"plain.dll");
        pm.initialize(sb.wstring());
        auto cand = findCandidate(pm, "plain");
        uint32_t id = cand ? cand->id : 0;
        std::string sha = cand ? cand->sha256 : "";
        edvr::plugins::ApprovalResult first = pm.approveCandidate(id);
        check(first.applied && first.loaded && first.stem == "plain" && first.sha256 == sha,
              "byid: the first activation approves and loads, and reports the stem and hash to record");
        edvr::plugins::CandidatePlugin copy;
        check(pm.candidateById(id, &copy) && copy.approved, "byid: the candidate now reads approved (its row is gone on rebuild)");
        check(!pm.candidateById(987654u, &copy), "byid: an unknown id is not found");
        check(pm.activePluginCount() == 1, "byid: the addon is active exactly once");
        pm.shutdown();
        std::error_code ec11;
        std::filesystem::remove_all(sb, ec11);
    }

    // Case 12 (PR #92 review): a saved float is applied when its setting registers.
    {
        auto sb = makeSandbox(L"float");
        {
            std::ofstream iniOut(sb / L"edvr.ini");
            iniOut << "[addon.float_owner.float_header]\nsaved = 0.75\n";
        }
        edvr::Config::get().init(sb.wstring());

        auto encode = [](float f) { int64_t v = 0; memcpy(&v, &f, sizeof(float)); return v; };
        auto decode = [](int64_t v) { float f = 0.0f; memcpy(&f, &v, sizeof(float)); return f; };

        EdvrPluginSettingDef def{};
        def.structSize = sizeof(def);
        def.header = "Float Header";
        def.type = EDVR_PLUGIN_SETTING_FLOAT;
        def.defaultValue = encode(0.25f);

        def.key = "saved";
        std::string savedKey = edvr::plugins::pluginSettingIniKey("float_owner", def.header, def.key);
        check(savedKey == "addon.float_owner.float_header.saved", "float: the ini key is addon.<stem>.<header>.<key>");
        check(decode(edvr::plugins::restoredPluginSettingValue(def, savedKey)) == 0.75f,
              "float: the saved value (0.75) is applied over the default (0.25)");

        def.key = "unsaved";
        std::string unsavedKey = edvr::plugins::pluginSettingIniKey("float_owner", def.header, def.key);
        check(decode(edvr::plugins::restoredPluginSettingValue(def, unsavedKey)) == 0.25f,
              "float: with nothing saved the default is applied");

        std::error_code ec12;
        std::filesystem::remove_all(sb, ec12);
    }

    // Case 13 (PR #92 review): the last addon faulting somewhere other than onFilterInput must not strand the input block.
    {
        auto sb = makeSandbox(L"inputfault");
        dropFixture(sb, L"", L"owner.dll");
        pm.initialize(sb.wstring());
        auto cand = findCandidate(pm, "owner");
        check(cand && pm.approvePlugin("owner", cand->sha256), "inputfault: the owning addon loads");
        HMODULE h = GetModuleHandleW(L"owner.dll");
        auto crash = h ? reinterpret_cast<PFN_SetCrashMode>(GetProcAddress(h, "fixtureSetCrashMode")) : nullptr;
        check(crash != nullptr, "inputfault: fixture helper resolved");
        g_gateBlocked = false;
        check(pm.onFilterInput(1, nullptr) && g_gateBlocked, "inputfault: the addon takes input");
        if (crash) crash(2);
        EdvrEyeRenderContext ctx{};
        ctx.structSize = sizeof(ctx);
        pm.onRenderEye(ctx);
        check(!pm.hasActivePlugins(), "inputfault: the addon is disabled by its render fault");
        check(!g_gateBlocked, "inputfault: the gate is clear after the owning addon faults in onRenderEye");
        pm.shutdown();
        std::error_code ec13;
        std::filesystem::remove_all(sb, ec13);
    }

    // Case 14 (PR #92 review): shutdown clears an input block still held by a healthy addon.
    {
        auto sb = makeSandbox(L"inputshutdown");
        dropFixture(sb, L"", L"owner.dll");
        pm.initialize(sb.wstring());
        auto cand = findCandidate(pm, "owner");
        check(cand && pm.approvePlugin("owner", cand->sha256), "inputshutdown: the owning addon loads");
        check(pm.onFilterInput(1, nullptr) && g_gateBlocked, "inputshutdown: the addon takes input");
        pm.shutdown();
        check(!g_gateBlocked, "inputshutdown: the gate is clear after shutdown");
        std::error_code ec14;
        std::filesystem::remove_all(sb, ec14);
    }

    // Case 15 (PR #92 review): loading an addon with an input filter installs the user32 doors. (Every fixture filters
    // input, so the side that installs nothing is the discovered-but-unapproved candidate.)
    {
        auto sb = makeSandbox(L"installdoors");
        dropFixture(sb, L"", L"filtering.dll");
        pm.initialize(sb.wstring());
        const int before = g_gateInstalls;
        auto cand = findCandidate(pm, "filtering");
        check(g_gateInstalls == before, "installdoors: an unapproved candidate installs nothing");
        check(cand && pm.approvePlugin("filtering", cand->sha256), "installdoors: the filtering addon loads");
        check(g_gateInstalls == before + 1, "installdoors: loading an addon with an input filter installs the user32 doors");
        pm.shutdown();
        std::error_code ec15;
        std::filesystem::remove_all(sb, ec15);
    }

    // Case 16 (PR #92 review): plugins are VR-only. A flat session discovers, initialises and dispatches nothing and registers no settings;
    // the same install in a VR session behaves as before.
    {
        auto sb = makeSandbox(L"flatscope");
        dropFixture(sb, L"settings", L"flatkeeper.dll");
        const std::string sha = edvr::plugins::PluginManager::calculateSha256((sb / L"plugins" / L"flatkeeper.dll").wstring());
        const edvr::RuntimeProfile savedProfile = edvr::runtimeProfileValue();
        g_fakeSettings.clear();
        g_fakeRegisteredEver = 0;
        edvr::runtimeProfileSet(edvr::RuntimeProfile::Flat);
        pm.initialize(sb.wstring());
        check(pm.candidatesSnapshot().empty(), "flatscope: a flat session discovers no candidates");
        check(!pm.approvePlugin("flatkeeper", sha), "flatscope: nothing can be approved into a flat session");
        check(GetModuleHandleW(L"flatkeeper.dll") == nullptr, "flatscope: no addon DLL is loaded in a flat session");
        check(!pm.hasActivePlugins(), "flatscope: no addon is active in a flat session");
        check(g_fakeRegisteredEver == 0 && g_fakeSettings.empty(), "flatscope: a flat session registers no plugin settings");
        EdvrPosef head{};
        pm.onUpdate(head, 0.016f);
        check(!pm.onFilterInput(1, nullptr), "flatscope: a flat session dispatches no input filter");
        pm.shutdown();

        edvr::runtimeProfileSet(edvr::RuntimeProfile::Vr);
        pm.initialize(sb.wstring());
        check(findCandidate(pm, "flatkeeper").has_value(), "flatscope: the same install discovers its addon in a VR session");
        check(pm.approvePlugin("flatkeeper", sha) && pm.hasActivePlugins(), "flatscope: a VR session loads the approved addon");
        check(fakeSettingsOwnedBy("flatkeeper") == 2, "flatscope: a VR session registers the addon's settings");
        pm.shutdown();
        edvr::runtimeProfileSet(savedProfile);
        std::error_code ec16;
        std::filesystem::remove_all(sb, ec16);
    }

    // Case 18 (PR #92 review): the profile switches live. A session that launches flat defers everything to the first switch into VR; a switch
    // to flat suspends the loaded addon (no callback, input released, rows hidden, nothing unloaded) and a switch back resumes it.
    {
        auto sb = makeSandbox(L"dormant");
        dropFixture(sb, L"settings", L"dormant.dll");
        const std::string dsha = edvr::plugins::PluginManager::calculateSha256((sb / L"plugins" / L"dormant.dll").wstring());
        {
            std::ofstream iniOut(sb / L"edvr.ini");
            iniOut << "[general]\nmode = vr\n[addons]\ndormant = " << dsha << "\n";
        }
        edvr::Config::get().init(sb.wstring());
        const edvr::RuntimeProfile savedProfile = edvr::runtimeProfileValue();
        g_fakeSettings.clear();
        g_fakeRegisteredEver = 0;
        g_fakeRowsHidden = false;

        edvr::runtimeProfileSet(edvr::RuntimeProfile::Flat);
        pm.initialize(sb.wstring());
        check(pm.candidatesSnapshot().empty() && !pm.hasActivePlugins() && g_fakeRegisteredEver == 0 &&
              GetModuleHandleW(L"dormant.dll") == nullptr,
              "dormant: launched in flat, initialize discovers and loads nothing");
        pm.onProfileSwitched();
        check(g_fakeRegisteredEver == 0 && GetModuleHandleW(L"dormant.dll") == nullptr,
              "dormant: a profile notification that changes nothing (still flat) loads nothing");

        edvr::runtimeProfileSet(edvr::RuntimeProfile::Vr);
        pm.onProfileSwitched();
        check(findCandidate(pm, "dormant").has_value(), "dormant: the first switch into VR discovers the addon");
        check(pm.hasActivePlugins() && pm.activePluginCount() == 1, "dormant: ...loads the approved addon (lazy load)");
        check(fakeSettingsOwnedBy("dormant") == 2 && g_fakeRegisteredEver == 2, "dormant: ...and registers its two settings");
        check(!g_fakeRowsHidden, "dormant: the rows are shown in VR");

        HMODULE hd = GetModuleHandleW(L"dormant.dll");
        auto updates = hd ? reinterpret_cast<PFN_GetCount>(GetProcAddress(hd, "fixtureGetUpdateCount")) : nullptr;
        auto renders = hd ? reinterpret_cast<PFN_GetCount>(GetProcAddress(hd, "fixtureGetRenderCount")) : nullptr;
        check(updates && renders, "dormant: fixture counters resolved");
        EdvrPosef dhead{};
        EdvrEyeRenderContext dctx{};
        dctx.structSize = sizeof(dctx);
        pm.onUpdate(dhead, 0.016f);
        pm.onRenderEye(dctx);
        const int u1 = updates ? updates() : -1;
        const int r1 = renders ? renders() : -1;
        check(pm.onFilterInput(1, nullptr) && g_gateBlocked, "dormant: in VR the addon takes input");

        edvr::runtimeProfileSet(edvr::RuntimeProfile::Flat);
        pm.onProfileSwitched();
        check(!g_gateBlocked, "dormant: the switch to flat releases the addon's input ownership");
        check(g_fakeRowsHidden, "dormant: the switch to flat hides the addon's rows");
        check(fakeSettingsOwnedBy("dormant") == 2, "dormant: ...but does not delete them (they must reappear on resume)");
        check(GetModuleHandleW(L"dormant.dll") == hd && pm.activePluginCount() == 1, "dormant: the addon stays loaded");
        check(!pm.hasActivePlugins(), "dormant: the scene pass is told nothing is active");
        pm.onUpdate(dhead, 0.016f);
        pm.onRenderEye(dctx);
        check(!pm.onFilterInput(1, nullptr) && !g_gateBlocked, "dormant: no input filter is called while suspended");
        check((updates ? updates() : -2) == u1 && (renders ? renders() : -2) == r1, "dormant: no update or render callback is made while suspended");

        edvr::runtimeProfileSet(edvr::RuntimeProfile::Vr);
        pm.onProfileSwitched();
        check(!g_fakeRowsHidden, "dormant: the switch back to VR shows the rows again");
        check(pm.hasActivePlugins(), "dormant: ...and the addon is active again");
        pm.onUpdate(dhead, 0.016f);
        pm.onRenderEye(dctx);
        check((updates ? updates() : -2) == u1 + 1 && (renders ? renders() : -2) == r1 + 1, "dormant: callbacks run again after resume");
        check(g_fakeRegisteredEver == 2, "dormant: resume does not register the settings a second time");
        check(pm.onFilterInput(1, nullptr) && g_gateBlocked, "dormant: the resumed addon can take input");
        pm.shutdown();
        edvr::runtimeProfileSet(savedProfile);
        std::error_code ec18;
        std::filesystem::remove_all(sb, ec18);
    }

    // Case 17 (PR #92 review): a fault retires an addon's rows from a thread that is not the menu's. The rows go inert at
    // once but are not erased, and no page is rebuilt, until the menu thread drains the request: it may be mid-tick,
    // holding Page and Entry values that index into the rows and heading text that borrows from them.
    {
        edvr::plugins::PluginRowRegistry reg;
        auto addRow = [&](const char* owner, const char* header, const char* key) {
            std::lock_guard<std::recursive_mutex> lock(reg.mutex());
            auto& r = reg.upsertLocked(owner, header, key);
            r.getter = reinterpret_cast<EdvrPluginSettingGetter>(&GetCurrentThreadId);   // any non-null pointer: only its nullness matters
            r.setter = reinterpret_cast<EdvrPluginSettingSetter>(&GetCurrentThreadId);
            r.userData = &reg;
        };
        addRow("alpha", "Alpha", "one");
        addRow("alpha", "Alpha", "two");
        addRow("beta", "Beta", "one");
        check(!reg.drain(), "retire: adding rows to the registry alone posts nothing");
        reg.postChanged();
        check(reg.drain(), "retire: a posted change makes the menu thread rebuild");
        check(!reg.drain(), "retire: a drained queue asks for nothing the second time");

        const edvr::plugins::PluginSettingRow* held = nullptr;
        const void* heldHeading = nullptr;
        {
            std::lock_guard<std::recursive_mutex> lock(reg.mutex());
            held = &reg.rows()[0];                              // what a Page entry's index resolves to
            heldHeading = reg.internHeading(reg.rows()[0].header);   // what a Heading entry borrows
        }
        std::thread faulter([&] { reg.retire("alpha"); });       // the OpenXR owner thread's fault path
        faulter.join();
        {
            std::lock_guard<std::recursive_mutex> lock(reg.mutex());
            check(reg.rows().size() == 3 && &reg.rows()[0] == held, "retire: no row is erased by the faulting thread (storage stays put)");
            check(reg.rows()[0].retired && reg.rows()[1].retired && !reg.rows()[2].retired, "retire: only the faulting owner's rows are marked");
            check(!held->getter && !held->setter && !held->userData, "retire: the marked row's callbacks are inert at once");
            check(reg.rows()[2].getter && reg.rows()[2].setter, "retire: another addon's callbacks are untouched");
            check(std::string(static_cast<const char*>(heldHeading)) == "Alpha", "retire: a heading taken before the fault still reads");
        }
        for (int i = 0; i < 64; ++i) addRow("gamma", "Gamma", ("k" + std::to_string(i)).c_str());   // the vector reallocates
        check(std::string(static_cast<const char*>(heldHeading)) == "Alpha", "retire: heading text survives the row vector growing");
        check(reg.drain(), "retire: the menu thread is told to rebuild");
        {
            std::lock_guard<std::recursive_mutex> lock(reg.mutex());
            size_t alpha = 0, beta = 0;
            for (const auto& r : reg.rows()) { if (r.owner == "alpha") ++alpha; if (r.owner == "beta") ++beta; }
            check(alpha == 0 && beta == 1, "retire: the drain erases the retired rows and nothing else");
        }
        check(!reg.retire("alpha"), "retire: retiring an owner with no rows is a no-op");
        check(!reg.drain(), "retire: ...and posts nothing");
        check(!reg.hidden(), "hide: rows start shown");
        reg.postHidden(true);
        check(reg.drain() && reg.hidden(), "hide: the menu thread hides on request and rebuilds");
        reg.postHidden(true);
        check(!reg.drain() && reg.hidden(), "hide: hiding again changes nothing");
        {
            std::lock_guard<std::recursive_mutex> lock(reg.mutex());
            check(reg.rows().size() == 65, "hide: hidden rows are kept, not deleted");
        }
        reg.postHidden(false);
        check(reg.drain() && !reg.hidden(), "hide: the menu thread shows them again");
    }

    // Case 19 (PR #92 review): addon identity is the DLL file stem. Two addons registering the same header and key get separate
    // rows and separate saved values, and one failing takes only its own row.
    {
        edvr::plugins::PluginRowRegistry reg;
        auto addRow = [&](const char* owner, int64_t value) {
            std::lock_guard<std::recursive_mutex> lock(reg.mutex());
            auto& r = reg.upsertLocked(owner, "Tools", "enabled");
            r.cachedValue = value;
            r.getter = reinterpret_cast<EdvrPluginSettingGetter>(&GetCurrentThreadId);   // only its nullness matters
        };
        addRow("tool_a", 11);
        addRow("tool_b", 22);
        addRow("tool_a", 33);   // the same addon registering again refreshes its own row
        {
            std::lock_guard<std::recursive_mutex> lock(reg.mutex());
            check(reg.rows().size() == 2, "identity: two owners with the same header and key have separate rows");
            check(reg.rows()[0].cachedValue == 33 && reg.rows()[1].cachedValue == 22,
                  "identity: re-registering overwrites only the owner's own row");
        }
        check(edvr::plugins::pluginSettingIniKey("tool_a", "Tools", "enabled") == "addon.tool_a.tools.enabled" &&
              edvr::plugins::pluginSettingIniKey("tool_b", "Tools", "enabled") == "addon.tool_b.tools.enabled",
              "identity: the saved key is addon.<stem>.<header>.<key>, one per owner");
        reg.retire("tool_b");   // addon B fails
        reg.drain();
        std::lock_guard<std::recursive_mutex> lock(reg.mutex());
        check(reg.rows().size() == 1 && reg.rows()[0].owner == "tool_a" && reg.rows()[0].cachedValue == 33 && reg.rows()[0].getter,
              "identity: B failing removes B's row; A's row and its value survive");
    }
    {
        // Persisted values do not cross owners either.
        auto sb = makeSandbox(L"identity");
        {
            std::ofstream iniOut(sb / L"edvr.ini");
            iniOut << "[addon.tool_a.tools]" << std::endl << "enabled = on" << std::endl;
        }
        edvr::Config::get().init(sb.wstring());
        EdvrPluginSettingDef def{};
        def.structSize = sizeof(def);
        def.header = "Tools";
        def.key = "enabled";
        def.type = EDVR_PLUGIN_SETTING_BOOL;
        check(edvr::plugins::restoredPluginSettingValue(def, edvr::plugins::pluginSettingIniKey("tool_a", def.header, def.key)) == 1,
              "identity: the owner's saved value is applied");
        check(edvr::plugins::restoredPluginSettingValue(def, edvr::plugins::pluginSettingIniKey("tool_b", def.header, def.key)) == 0,
              "identity: another owner's same header and key reads its own default, not A's saved value");
        std::error_code ec19;
        std::filesystem::remove_all(sb, ec19);
    }

    // Case 20 (PR #92 review): registration context belongs to the initialising thread. An addon's worker thread calling the host's
    // registerSetting during that addon's own onInitialize, and a callback the addon kept and calls once initialisation is over,
    // are refused and enter no registry. (Per-thread, so they are refused however they overlap another addon's initialise.)
    {
        auto sb = makeSandbox(L"threadreg");
        dropFixture(sb, L"threadreg", L"worker_a.dll");
        dropFixture(sb, L"settings", L"keeper_b.dll");
        pm.initialize(sb.wstring());
        g_fakeSettings.clear();
        g_fakeRegisteredEver = 0;
        auto wa = findCandidate(pm, "worker_a");
        auto kb = findCandidate(pm, "keeper_b");
        check(wa && kb, "threadreg: both candidates discovered");
        if (wa && kb) {
            check(pm.approvePlugin("worker_a", wa->sha256), "threadreg: the addon whose worker registers loads");
            HMODULE hw = GetModuleHandleW(L"worker_a.dll");
            typedef int (WINAPI *PFN_Int)();
            auto fromWorker = hw ? reinterpret_cast<PFN_Int>(GetProcAddress(hw, "fixtureGetThreadRegResult")) : nullptr;
            auto callLater = hw ? reinterpret_cast<PFN_Int>(GetProcAddress(hw, "fixtureCallRegister")) : nullptr;
            check(fromWorker && callLater, "threadreg: fixture helpers resolved");
            check(fromWorker && fromWorker() == -1, "threadreg: registerSetting from the addon's worker thread during its onInitialize returns -1");
            check(g_fakeRegisteredEver == 0 && fakeSettingsOwnedBy("worker_a") == 0, "threadreg: ...and nothing entered the registry");
            check(callLater && callLater() == -1, "threadreg: a kept registerSetting called later returns -1");
            int fromOtherThread = 12345;
            if (callLater) {
                std::thread t([&] { fromOtherThread = callLater(); });
                t.join();
            }
            check(fromOtherThread == -1 && g_fakeRegisteredEver == 0, "threadreg: ...from another thread too, and still nothing registered");
            check(pm.approvePlugin("keeper_b", kb->sha256) && fakeSettingsOwnedBy("keeper_b") == 2 && fakeSettingsOwnedBy("worker_a") == 0,
                  "threadreg: another addon's own initialise still registers, tagged with itself");
        }
        pm.shutdown();
        std::error_code ec20;
        std::filesystem::remove_all(sb, ec20);
    }

    // Case 22 (PR #92 review): addons are active only in the Vr and LegacyVr profiles. Passthrough (a VR-only install whose game is in flat mode) is
    // as dormant as Flat: VR to Passthrough suspends and releases input, and a launch in Passthrough defers discovery (the [addons] approvals
    // are unreadable there) so the later switch into VR loads the previously approved addon.
    {
        auto sb = makeSandbox(L"passthrough");
        dropFixture(sb, L"settings", L"pass.dll");
        const std::string psha = edvr::plugins::PluginManager::calculateSha256((sb / L"plugins" / L"pass.dll").wstring());
        {
            std::ofstream iniOut(sb / L"edvr.ini");
            iniOut << "[general]\nmode = vr\n[addons]\npass = " << psha << "\n";
        }
        edvr::Config::get().init(sb.wstring());
        const edvr::RuntimeProfile savedProfile = edvr::runtimeProfileValue();
        g_fakeSettings.clear();
        g_fakeRegisteredEver = 0;
        g_fakeRowsHidden = false;

        // (b) start in Passthrough
        edvr::runtimeProfileSet(edvr::RuntimeProfile::Passthrough);
        pm.initialize(sb.wstring());
        check(pm.candidatesSnapshot().empty() && !pm.hasActivePlugins() && g_fakeRegisteredEver == 0 &&
              GetModuleHandleW(L"pass.dll") == nullptr,
              "passthrough: launched in Passthrough, initialize discovers and loads nothing");
        edvr::runtimeProfileSet(edvr::RuntimeProfile::Vr);
        pm.onProfileSwitched();
        auto pc = findCandidate(pm, "pass");
        check(pc.has_value() && pc->approved, "passthrough: the switch into VR discovers the addon and reads its approval");
        check(pm.hasActivePlugins() && pm.activePluginCount() == 1 && GetModuleHandleW(L"pass.dll") != nullptr,
              "passthrough: ...and loads the previously approved addon automatically");

        // (a) VR to Passthrough
        check(pm.onFilterInput(1, nullptr) && g_gateBlocked, "passthrough: in VR the addon takes input");
        edvr::runtimeProfileSet(edvr::RuntimeProfile::Passthrough);
        pm.onProfileSwitched();
        check(!g_gateBlocked, "passthrough: the switch to Passthrough releases the addon's input ownership");
        check(g_fakeRowsHidden && !pm.hasActivePlugins(), "passthrough: ...hides its rows and suspends it");
        check(!pm.onFilterInput(1, nullptr), "passthrough: no input filter is called while suspended");
        edvr::runtimeProfileSet(edvr::RuntimeProfile::LegacyVr);
        pm.onProfileSwitched();
        check(!g_fakeRowsHidden && pm.hasActivePlugins(), "passthrough: LegacyVr counts as VR and resumes the addon");
        pm.shutdown();
        edvr::runtimeProfileSet(savedProfile);
        std::error_code ec22;
        std::filesystem::remove_all(sb, ec22);
    }

    // Case 23 (PR #92 review): the persisted name is folded to lowercase, as Config::parse folds every stored name, so a key an addon spells
    // "Enabled" is restored after a save. The menu writes the dotted name into [section] key = value; the ini is then parsed afresh.
    {
        auto sb = makeSandbox(L"keycase");
        EdvrPluginSettingDef def{};
        def.structSize = sizeof(def);
        def.header = "Tools";
        def.key = "Enabled";
        def.type = EDVR_PLUGIN_SETTING_BOOL;
        def.defaultValue = 0;
        const std::string dotted = edvr::plugins::pluginSettingIniKey("tool_a", def.header, def.key);
        check(dotted == "addon.tool_a.tools.enabled", "keycase: the persisted key is lowercase though the addon spells it Enabled");
        check(std::string(def.key) == "Enabled", "keycase: the addon's own key string is untouched");
        const size_t lastDot = dotted.find_last_of('.');
        {
            std::ofstream iniOut(sb / L"edvr.ini");   // what the menu's write leaves: the section and key of the dotted name
            iniOut << "[general]\nmode = vr\n[" << dotted.substr(0, lastDot) << "]\n" << dotted.substr(lastDot + 1) << " = on\n";
        }
        edvr::Config::get().init(sb.wstring());   // the next initialisation parses the saved file
        check(edvr::plugins::restoredPluginSettingValue(def, dotted) == 1,
              "keycase: the non-default value saved for 'Enabled' is restored on the next initialisation");
        std::error_code ec23;
        std::filesystem::remove_all(sb, ec23);
    }

    // Case 24 (PR #92 review): an addon's identity is its sanitized stem. plugins\foo.dll and plugins\foo\plugin.dll both say 'foo', and
    // foo-bar.dll / foo_bar.dll both say 'foo_bar': every file of such a group is refused before anything loads, approved hash or not, and
    // none can be approved this session. A lone foo.dll still loads.
    {
        // (a) the two layouts, the same bytes, the hash approved
        auto sb = makeSandbox(L"dupid");
        dropFixture(sb, L"", L"foo.dll");
        std::filesystem::create_directories(sb / L"plugins" / L"foo");
        dropFixture(sb, L"", L"foo\\plugin.dll");
        const std::string fsha = edvr::plugins::PluginManager::calculateSha256((sb / L"plugins" / L"foo.dll").wstring());
        {
            std::ofstream iniOut(sb / L"edvr.ini");
            iniOut << "[general]\nmode = vr\n[addons]\nfoo = " << fsha << "\n";
        }
        edvr::Config::get().init(sb.wstring());
        g_fakeSettings.clear();
        g_fakeRegisteredEver = 0;
        pm.initialize(sb.wstring());
        size_t onFoo = 0;
        bool anyApproved = false, allConflicted = true;
        uint32_t firstId = 0;
        for (const auto& c : pm.candidatesSnapshot()) {
            if (c.stem != "foo") continue;
            ++onFoo;
            anyApproved = anyApproved || c.approved;
            allConflicted = allConflicted && c.conflicted;
            if (!firstId) firstId = c.id;
        }
        check(onFoo == 2 && allConflicted, "dupid: foo.dll and foo\\plugin.dll are both discovered and both marked as conflicting");
        check(!anyApproved && pm.activePluginCount() == 0 && !pm.hasActivePlugins() && GetModuleHandleW(L"foo.dll") == nullptr &&
              GetModuleHandleW(L"plugin.dll") == nullptr && g_fakeRegisteredEver == 0,
              "dupid: neither copy loads although their shared hash is approved");
        check(!pm.approveCandidate(firstId).applied && !pm.approvePlugin("foo", fsha) && pm.activePluginCount() == 0,
              "dupid: approval of a conflicted candidate is refused");
        pm.shutdown();

        // (b) a name collision through sanitizeStem, different binaries
        auto sb2 = makeSandbox(L"dupstem");
        dropFixture(sb2, L"", L"foo-bar.dll");
        dropFixture(sb2, L"b", L"foo_bar.dll");
        pm.initialize(sb2.wstring());
        size_t onBar = 0;
        bool barConflicted = true;
        for (const auto& c : pm.candidatesSnapshot()) {
            if (c.stem != "foo_bar") continue;
            ++onBar;
            barConflicted = barConflicted && c.conflicted && !pm.approveCandidate(c.id).applied && !pm.approvePlugin(c.stem, c.sha256);
        }
        check(onBar == 2 && barConflicted && pm.activePluginCount() == 0,
              "dupstem: foo-bar.dll and foo_bar.dll collide; neither loads and approval of either is refused");
        pm.shutdown();

        // (c) one file keeps working
        auto sb3 = makeSandbox(L"dupone");
        dropFixture(sb3, L"", L"foo.dll");
        {
            std::ofstream iniOut(sb3 / L"edvr.ini");
            iniOut << "[general]\nmode = vr\n[addons]\nfoo = " << fsha << "\n";
        }
        edvr::Config::get().init(sb3.wstring());
        pm.initialize(sb3.wstring());
        auto one = findCandidate(pm, "foo");
        check(one && !one->conflicted && one->approved && pm.activePluginCount() == 1, "dupone: a single foo.dll is not a conflict and loads on its approved hash");
        pm.shutdown();
        std::error_code ecd;
        std::filesystem::remove_all(sb, ecd);
        std::filesystem::remove_all(sb2, ecd);
        std::filesystem::remove_all(sb3, ecd);
    }

    // Case 25 (PR #92 review, Argon): a faulting addon releases input only when no other addon holds it. Two addons: "a_holder" (default
    // fixture, swallows input) loads first, "b_other" (variant b, a separate DLL with its own crash mode) is the one that faults. The
    // host runs onFilterInput before onRenderEye each frame, so a fault in B must not drop A's hold for the rest of the frame.
    {
        typedef void (WINAPI *PFN_SetCrashMode)(int);
        auto loadPair = [&](const wchar_t* tag, PFN_SetCrashMode* crashB, PFN_SetCrashMode* crashA) {
            auto sb = makeSandbox(tag);
            dropFixture(sb, L"", L"a_holder.dll");
            dropFixture(sb, L"b", L"b_other.dll");
            pm.initialize(sb.wstring());
            auto ca = findCandidate(pm, "a_holder");
            auto cb = findCandidate(pm, "b_other");
            check(ca && cb && pm.approvePlugin("a_holder", ca->sha256) && pm.approvePlugin("b_other", cb->sha256) &&
                  pm.activePluginCount() == 2, "multihold: both addons load");
            HMODULE ha = GetModuleHandleW(L"a_holder.dll");
            HMODULE hb = GetModuleHandleW(L"b_other.dll");
            *crashA = ha ? reinterpret_cast<PFN_SetCrashMode>(GetProcAddress(ha, "fixtureSetCrashMode")) : nullptr;
            *crashB = hb ? reinterpret_cast<PFN_SetCrashMode>(GetProcAddress(hb, "fixtureSetCrashMode")) : nullptr;
            check(*crashA && *crashB, "multihold: fixture helpers resolved");
            return sb;
        };
        EdvrEyeRenderContext eye{};
        eye.structSize = sizeof(eye);
        EdvrPosef pose{};
        PFN_SetCrashMode crashA = nullptr, crashB = nullptr;

        // (1) B faults in onRenderEye while A holds input.
        {
            auto sb = loadPair(L"multihold_render", &crashB, &crashA);
            g_gateBlocked = false;
            check(pm.onFilterInput(1, nullptr) && g_gateBlocked, "multihold render: the addons take input");
            const int clears = g_gateClears;
            if (crashB) crashB(2);
            pm.onRenderEye(eye);
            check(pm.activePluginCount() == 1, "multihold render: B is disabled by its render fault");
            check(g_gateBlocked && g_gateClears == clears, "multihold render: A's hold survives B's onRenderEye fault");
            pm.shutdown();
            std::error_code ec; std::filesystem::remove_all(sb, ec);
        }
        // (2) B faults in onUpdate while A holds input.
        {
            auto sb = loadPair(L"multihold_update", &crashB, &crashA);
            g_gateBlocked = false;
            check(pm.onFilterInput(1, nullptr) && g_gateBlocked, "multihold update: the addons take input");
            const int clears = g_gateClears;
            if (crashB) crashB(1);
            pm.onUpdate(pose, 0.011f);
            check(pm.activePluginCount() == 1, "multihold update: B is disabled by its update fault");
            check(g_gateBlocked && g_gateClears == clears, "multihold update: A's hold survives B's onUpdate fault");
            pm.shutdown();
            std::error_code ec; std::filesystem::remove_all(sb, ec);
        }
        // (3) B faults in onFilterInput after A swallowed earlier in the same loop: no intermediate release.
        {
            auto sb = loadPair(L"multihold_filter", &crashB, &crashA);
            g_gateBlocked = false;
            check(pm.onFilterInput(1, nullptr) && g_gateBlocked, "multihold filter: the addons take input");
            const int clears = g_gateClears;
            if (crashB) crashB(3);
            check(pm.onFilterInput(1, nullptr), "multihold filter: A still swallows after B faults");
            check(pm.activePluginCount() == 1, "multihold filter: B is disabled by its filter fault");
            check(g_gateBlocked && g_gateClears == clears, "multihold filter: no release, even momentarily, while A holds");
            pm.shutdown();
            std::error_code ec; std::filesystem::remove_all(sb, ec);
        }
        // (4) The only holder faults: the block is released. A filters input but never swallows; B is the sole holder.
        {
            auto sb = loadPair(L"multihold_sole", &crashB, &crashA);
            if (crashA) crashA(4);
            g_gateBlocked = false;
            check(pm.onFilterInput(1, nullptr) && g_gateBlocked, "multihold sole: B takes input");
            if (crashB) crashB(2);
            pm.onRenderEye(eye);
            check(pm.activePluginCount() == 1, "multihold sole: B is disabled by its render fault");
            check(!g_gateBlocked, "multihold sole: the gate is released when the only holder faults");
            pm.shutdown();
            std::error_code ec; std::filesystem::remove_all(sb, ec);
        }
    }

    // Case 26 (PR #92 review, ticket 19): an approval counts, and is saved by the menu, only when the addon loads. A failed load leaves the
    // candidate unapproved with a reason, so the row stays and the approval can be retried in the same session.
    {
        // (1) The file is locked at approval; releasing it and approving again loads and applies.
        {
            auto sb = makeSandbox(L"retry_locked");
            dropFixture(sb, L"", L"locked.dll");
            pm.initialize(sb.wstring());
            auto cand = findCandidate(pm, "locked");
            check(cand.has_value() && !cand->sha256.empty(), "retry locked: candidate discovered with a hash");
            const uint32_t id = cand ? cand->id : 0;
            const std::wstring path = (sb / L"plugins" / L"locked.dll").wstring();
            HANDLE lockH = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            check(lockH != INVALID_HANDLE_VALUE, "retry locked: the test holds the file exclusively");
            edvr::plugins::ApprovalResult r = pm.approveCandidate(id);
            check(!r.applied && !r.loaded, "retry locked: a locked file is not applied (nothing for the menu to save)");
            check(!r.reason.empty(), "retry locked: the result carries a reason");
            edvr::plugins::CandidatePlugin after;
            check(pm.candidateById(id, &after) && !after.approved && !after.loadNote.empty(),
                  "retry locked: the candidate stays listed unapproved, with a reason on it");
            check(pm.activePluginCount() == 0, "retry locked: nothing loaded");
            if (lockH != INVALID_HANDLE_VALUE) CloseHandle(lockH);
            edvr::plugins::ApprovalResult retry = pm.approveCandidate(id);
            check(retry.applied && retry.loaded && retry.stem == "locked" && retry.sha256 == (cand ? cand->sha256 : ""),
                  "retry locked: approving again once the file is free loads it and reports the hash to save");
            check(pm.candidateById(id, &after) && after.approved && after.loadNote.empty(), "retry locked: the loaded candidate is approved with no failure note");
            pm.shutdown();
            std::error_code ec; std::filesystem::remove_all(sb, ec);
        }
        // (2) An addon whose onInitialize fails (or whose registration faults) is not applied and can be tried again.
        for (const wchar_t* variant : { L"initfail", L"initfault", L"regfault" }) {
            const std::wstring tag = std::wstring(L"retry_") + variant;
            auto sb = makeSandbox(tag.c_str());
            dropFixture(sb, variant, L"broken.dll");
            pm.initialize(sb.wstring());
            auto cand = findCandidate(pm, "broken");
            const uint32_t id = cand ? cand->id : 0;
            edvr::plugins::ApprovalResult r = pm.approveCandidate(id);
            const std::string label = "retry " + std::string(variant, variant + wcslen(variant));
            check(cand && !r.applied && !r.loaded && !r.reason.empty(), (label + ": not applied, with a reason").c_str());
            edvr::plugins::CandidatePlugin after;
            check(pm.candidateById(id, &after) && !after.approved && !after.loadNote.empty() && after.sha256 == cand->sha256,
                  (label + ": the candidate stays listed unapproved with a reason").c_str());
            pm.shutdown();
            std::error_code ec; std::filesystem::remove_all(sb, ec);
        }
        // (3) A candidate whose file could not be hashed at discovery has no hash to approve: refused, with a reason.
        {
            auto sb = makeSandbox(L"retry_nohash");
            dropFixture(sb, L"", L"nohash.dll");
            const std::wstring path = (sb / L"plugins" / L"nohash.dll").wstring();
            HANDLE lockH = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            pm.initialize(sb.wstring());
            if (lockH != INVALID_HANDLE_VALUE) CloseHandle(lockH);
            auto cand = findCandidate(pm, "nohash");
            check(cand && cand->sha256.empty(), "nohash: a file locked at discovery has an empty hash");
            edvr::plugins::ApprovalResult r = pm.approveCandidate(cand ? cand->id : 0);
            check(!r.applied && !r.loaded && !r.reason.empty(), "nohash: approval is refused with a reason");
            edvr::plugins::CandidatePlugin after;
            check(cand && pm.candidateById(cand->id, &after) && !after.approved && !after.loadNote.empty() && pm.activePluginCount() == 0,
                  "nohash: the candidate stays listed unapproved with a reason, nothing loaded");
            pm.shutdown();
            std::error_code ec; std::filesystem::remove_all(sb, ec);
        }
        // (4) A file swapped after discovery is still not applied; it resurfaces under its new hash and that hash can be approved.
        {
            auto sb = makeSandbox(L"retry_swap");
            dropFixture(sb, L"", L"swapped.dll");
            pm.initialize(sb.wstring());
            auto cand = findCandidate(pm, "swapped");
            const uint32_t id = cand ? cand->id : 0;
            dropFixture(sb, L"b", L"swapped.dll");
            const std::string shaB = edvr::plugins::PluginManager::calculateSha256((sb / L"plugins" / L"swapped.dll").wstring());
            edvr::plugins::ApprovalResult r = pm.approveCandidate(id);
            check(cand && !r.applied && !r.loaded, "retry swap: the old hash is not applied to the replaced file");
            edvr::plugins::CandidatePlugin after;
            check(pm.candidateById(id, &after) && !after.approved && after.sha256 == shaB && shaB != cand->sha256,
                  "retry swap: the candidate resurfaces unapproved under the new hash");
            edvr::plugins::ApprovalResult again = pm.approveCandidate(id);
            check(again.applied && again.loaded && again.sha256 == shaB, "retry swap: the new hash approves and loads");
            pm.shutdown();
            std::error_code ec; std::filesystem::remove_all(sb, ec);
        }
    }

    // Case 21 (PR #92 review): the singleton is leaked on purpose. Leave a healthy addon loaded and initialised, with no shutdown(),
    // and let checkNoShutdownAtExit look for its onShutdown after the CRT has run its exit-time destructors.
    {
        auto sb = makeSandbox(L"leaked");
        dropFixture(sb, L"markshutdown", L"leaky.dll");
        pm.initialize(sb.wstring());
        auto cand = findCandidate(pm, "leaky");
        check(cand && pm.approvePlugin("leaky", cand->sha256) && pm.hasActivePlugins(), "singleton: an addon is left loaded without shutdown()");
        check(GetFileAttributesA(g_shutdownMark.c_str()) == INVALID_FILE_ATTRIBUTES, "singleton: its onShutdown has not run yet");
        // The sandbox is left for the OS temp cleaner: the addon DLL is still loaded from it.
    }

    // Cleanup sandbox
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);

    printf("\nADDON MANAGER TEST %s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
