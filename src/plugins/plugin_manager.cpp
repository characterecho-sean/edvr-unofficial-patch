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

    std::vector<std::wstring> baseDirs = {
        rootDir + L"\\plugins",
        rootDir + L"\\edvr_plugins"
    };

    auto tryLoadPlugin = [this](const std::wstring& dllPath, const std::wstring& displayName) {
        HMODULE hMod = LoadLibraryW(dllPath.c_str());
        if (!hMod) {
            Log::get().note("plugin_manager: failed to load %ls (err=%lu)\n", dllPath.c_str(), GetLastError());
            return;
        }

        auto regFunc = reinterpret_cast<EdvrPluginRegisterFunc>(GetProcAddress(hMod, "EdvrPluginRegister"));
        if (!regFunc) {
            FreeLibrary(hMod);
            return; // Not an EDVR plugin DLL
        }

        LoadedPlugin plugin{};
        plugin.modulePath = dllPath;
        plugin.moduleHandle = hMod;
        plugin.callbacks.structSize = sizeof(EdvrPluginCallbacks);

        int regResult = regFunc(EDVR_PLUGIN_API_VERSION, &plugin.callbacks);
        if (regResult != 0) {
            // Fallback for version 1 plugins
            regResult = regFunc(1, &plugin.callbacks);
        }
        if (regResult != 0) {
            Log::get().note("plugin_manager: plugin %ls rejected API version %u (result=%d)\n",
                            displayName.c_str(), EDVR_PLUGIN_API_VERSION, regResult);
            FreeLibrary(hMod);
            return;
        }

        if (plugin.callbacks.onInitialize) {
            EdvrHostServices hostServices{};
            hostServices.structSize = sizeof(EdvrHostServices);
            hostServices.registerSetting = [](const EdvrPluginSettingDef* setting) -> int {
                typedef int (WINAPI *PFN_edvrRegisterPluginSetting)(const EdvrPluginSettingDef*);
                HMODULE hD3D11 = GetModuleHandleW(L"d3d11.dll");
                if (hD3D11) {
                    auto reg = reinterpret_cast<PFN_edvrRegisterPluginSetting>(
                        GetProcAddress(hD3D11, "edvrRegisterPluginSetting"));
                    if (reg) return reg(setting);
                }
                return -1;
            };
            hostServices.logNote = [](const char* msg) {
                if (msg) Log::get().note("%s", msg);
            };

            int initResult = plugin.callbacks.onInitialize(&hostServices);
            if (initResult != 0) {
                Log::get().note("plugin_manager: plugin %ls onInitialize failed (code=%d)\n",
                                displayName.c_str(), initResult);
                FreeLibrary(hMod);
                return;
            }
        }

        const char* name = plugin.callbacks.pluginName ? plugin.callbacks.pluginName : "Unnamed Plugin";
        const char* ver = plugin.callbacks.pluginVersion ? plugin.callbacks.pluginVersion : "1.0";
        Log::get().note("plugin_manager: loaded plugin [%s v%s] from %ls\n", name, ver, dllPath.c_str());
        m_plugins.push_back(std::move(plugin));
    };

    for (const auto& baseDir : baseDirs) {
        // 1. Scan subdirectories: <baseDir>/<plugin_name>/plugin.dll (and <baseDir>/<plugin_name>/*.dll)
        std::wstring dirPattern = baseDir + L"\\*";
        WIN32_FIND_DATAW subFind{};
        HANDLE hSub = FindFirstFileW(dirPattern.c_str(), &subFind);
        if (hSub != INVALID_HANDLE_VALUE) {
            do {
                if ((subFind.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    wcscmp(subFind.cFileName, L".") != 0 &&
                    wcscmp(subFind.cFileName, L"..") != 0) {
                    
                    std::wstring subDir = baseDir + L"\\" + subFind.cFileName;
                    std::wstring pluginDll = subDir + L"\\plugin.dll";
                    if (GetFileAttributesW(pluginDll.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        tryLoadPlugin(pluginDll, std::wstring(subFind.cFileName) + L"\\plugin.dll");
                    } else {
                        // Scan for any .dll in subfolder
                        std::wstring subDllPattern = subDir + L"\\*.dll";
                        WIN32_FIND_DATAW dllFind{};
                        HANDLE hDll = FindFirstFileW(subDllPattern.c_str(), &dllFind);
                        if (hDll != INVALID_HANDLE_VALUE) {
                            do {
                                if (!(dllFind.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                                    tryLoadPlugin(subDir + L"\\" + dllFind.cFileName,
                                                  std::wstring(subFind.cFileName) + L"\\" + dllFind.cFileName);
                                }
                            } while (FindNextFileW(hDll, &dllFind));
                            FindClose(hDll);
                        }
                    }
                }
            } while (FindNextFileW(hSub, &subFind));
            FindClose(hSub);
        }

        // 2. Flat fallback: <baseDir>/*.dll
        std::wstring flatPattern = baseDir + L"\\*.dll";
        WIN32_FIND_DATAW flatFind{};
        HANDLE hFlat = FindFirstFileW(flatPattern.c_str(), &flatFind);
        if (hFlat != INVALID_HANDLE_VALUE) {
            do {
                if (!(flatFind.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::wstring dllPath = baseDir + L"\\" + flatFind.cFileName;
                    bool alreadyLoaded = false;
                    for (const auto& p : m_plugins) {
                        if (_wcsicmp(p.modulePath.c_str(), dllPath.c_str()) == 0) {
                            alreadyLoaded = true;
                            break;
                        }
                    }
                    if (!alreadyLoaded) {
                        tryLoadPlugin(dllPath, flatFind.cFileName);
                    }
                }
            } while (FindNextFileW(hFlat, &flatFind));
            FindClose(hFlat);
        }
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
