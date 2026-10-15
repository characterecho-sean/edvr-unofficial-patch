#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>

#include "../../include/edvr_plugin_api.h"
#include "../../src/plugins/plugin_manager.h"
#include "../../src/common/config.h"
#include "../../src/common/log.h"

// Stubs for d3d11 and input gate functions required by plugin_manager
extern "C" __declspec(dllexport) int WINAPI edvrRegisterPluginSetting(const EdvrPluginSettingDef* setting) {
    (void)setting;
    return 0;
}

// The F8 menu's settings registry, reduced to what the manager's ownership contract needs.
struct FakeSetting { std::string owner; std::string key; };
static std::vector<FakeSetting> g_fakeSettings;
static int g_fakeRegisteredEver = 0;

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

void inputGateSetPluginBlock(bool block) {
    (void)block;
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

const edvr::plugins::CandidatePlugin* findCandidate(edvr::plugins::PluginManager& pm, const char* stem) {
    for (const auto& c : pm.candidates()) {
        if (c.stem == stem) return &c;
    }
    return nullptr;
}

} // namespace

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

    auto& pm = edvr::plugins::PluginManager::instance();
    bool initOk = pm.initialize(sandbox.wstring());
    check(initOk, "PluginManager::initialize succeeds on sandbox");

    const auto& cands = pm.candidates();
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
        auto* bad = findCandidate(pm, "reg_fault");
        auto* good = findCandidate(pm, "healthy");
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
        auto* cand = findCandidate(pm, "swap");
        check(cand != nullptr, "swap: candidate discovered");
        std::string shaA = cand ? cand->sha256 : "";
        // Replace the file after discovery, before the user approves the hash they were shown.
        dropFixture(sb, L"b", L"swap.dll");
        std::string shaB = edvr::plugins::PluginManager::calculateSha256((sb / L"plugins" / L"swap.dll").wstring());
        check(!shaA.empty() && !shaB.empty() && shaA != shaB, "swap: the replacement has a different hash");

        bool loaded = pm.approvePlugin("swap", shaA);
        check(!loaded, "swap: approval of the old hash does not load the replaced file");
        check(GetModuleHandleW(L"swap.dll") == nullptr, "swap: the replacement DLL was never loaded");
        check(pm.activePluginCount() == 0, "swap: nothing active after the refused load");
        auto* after = findCandidate(pm, "swap");
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
        auto* keeper = findCandidate(pm, "keeper");
        auto* fails = findCandidate(pm, "fails");
        auto* faults = findCandidate(pm, "faults");
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

    // Cleanup sandbox
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);

    printf("\nADDON MANAGER TEST %s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
