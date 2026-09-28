#include "plugin_manager.h"
#include "../common/log.h"
#include <algorithm>

namespace edvr::plugins {

PluginManager& PluginManager::instance() {
    static PluginManager s_instance;
    return s_instance;
}

PluginManager::~PluginManager() {
    shutdown();
}

bool PluginManager::initialize(const std::wstring& rootDir) {
    if (m_initialized) return true;
    m_initialized = true;

    std::vector<std::wstring> searchDirs = {
        rootDir + L"\\plugins",
        rootDir + L"\\edvr_plugins"
    };

    for (const auto& dir : searchDirs) {
        std::wstring pattern = dir + L"\\*.dll";
        WIN32_FIND_DATAW findData{};
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) continue;

        do {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::wstring dllPath = dir + L"\\" + findData.cFileName;
                HMODULE hMod = LoadLibraryW(dllPath.c_str());
                if (!hMod) {
                    Log::get().note("plugin_manager: failed to load %ls (err=%lu)\n", dllPath.c_str(), GetLastError());
                    continue;
                }

                auto regFunc = reinterpret_cast<EdvrPluginRegisterFunc>(GetProcAddress(hMod, "EdvrPluginRegister"));
                if (!regFunc) {
                    FreeLibrary(hMod);
                    continue; // Not an EDVR plugin DLL
                }

                LoadedPlugin plugin{};
                plugin.modulePath = dllPath;
                plugin.moduleHandle = hMod;
                plugin.callbacks.structSize = sizeof(EdvrPluginCallbacks);

                int regResult = regFunc(EDVR_PLUGIN_API_VERSION, &plugin.callbacks);
                if (regResult != 0) {
                    Log::get().note("plugin_manager: plugin %ls rejected API version %u (result=%d)\n",
                                    findData.cFileName, EDVR_PLUGIN_API_VERSION, regResult);
                    FreeLibrary(hMod);
                    continue;
                }

                if (plugin.callbacks.onInitialize) {
                    int initResult = plugin.callbacks.onInitialize(nullptr);
                    if (initResult != 0) {
                        Log::get().note("plugin_manager: plugin %ls onInitialize failed (code=%d)\n",
                                        findData.cFileName, initResult);
                        FreeLibrary(hMod);
                        continue;
                    }
                }

                const char* name = plugin.callbacks.pluginName ? plugin.callbacks.pluginName : "Unnamed Plugin";
                const char* ver = plugin.callbacks.pluginVersion ? plugin.callbacks.pluginVersion : "1.0";
                Log::get().note("plugin_manager: loaded plugin [%s v%s] from %ls\n", name, ver, findData.cFileName);
                m_plugins.push_back(std::move(plugin));
            }
        } while (FindNextFileW(hFind, &findData));

        FindClose(hFind);
    }

    Log::get().note("plugin_manager: initialized, %zu active plugin(s)\n", m_plugins.size());
    return true;
}

void PluginManager::shutdown() {
    for (auto& plugin : m_plugins) {
        if (plugin.callbacks.onShutdown) {
            plugin.callbacks.onShutdown();
        }
        if (plugin.moduleHandle) {
            FreeLibrary(plugin.moduleHandle);
            plugin.moduleHandle = nullptr;
        }
    }
    m_plugins.clear();
    m_initialized = false;
}

void PluginManager::onUpdate(const EdvrPosef& headPose, float dtSeconds) {
    for (auto& plugin : m_plugins) {
        if (plugin.callbacks.onUpdate) {
            plugin.callbacks.onUpdate(&headPose, dtSeconds);
        }
    }
}

void PluginManager::onRenderEye(const EdvrEyeRenderContext& eyeCtx) {
    for (auto& plugin : m_plugins) {
        if (plugin.callbacks.onRenderEye) {
            plugin.callbacks.onRenderEye(&eyeCtx);
        }
    }
}

bool PluginManager::onFilterInput(uint32_t deviceType, const void* rawInputData) {
    bool swallowed = false;
    for (auto& plugin : m_plugins) {
        if (plugin.callbacks.onFilterInput) {
            EdvrInputContext ctx{};
            ctx.structSize = sizeof(EdvrInputContext);
            ctx.deviceType = deviceType;
            ctx.rawInputData = rawInputData;
            ctx.swallowInput = 0;
            plugin.callbacks.onFilterInput(&ctx);
            if (ctx.swallowInput) {
                swallowed = true;
            }
        }
    }
    return swallowed;
}

} // namespace edvr::plugins
