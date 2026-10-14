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

namespace edvr {
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

std::wstring getFixtureDllPath() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path p(exePath);
    return (p.parent_path() / L"test_addon_fixture.dll").wstring();
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

    // Cleanup sandbox
    std::error_code ec;
    std::filesystem::remove_all(sandbox, ec);

    printf("\nADDON MANAGER TEST %s\n", g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
