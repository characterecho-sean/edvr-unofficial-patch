#pragma once

#include "../../include/edvr_plugin_api.h"
#include <vector>
#include <string>
#include <windows.h>

namespace edvr::plugins {

struct LoadedPlugin {
    std::wstring modulePath;
    HMODULE moduleHandle = nullptr;
    EdvrPluginCallbacks callbacks{};
};

class PluginManager {
public:
    static PluginManager& instance();

    PluginManager() = default;
    ~PluginManager();

    // Scan directory and load all valid EDVR plugin DLLs
    bool initialize(const std::wstring& rootDir);
    void shutdown();

    bool hasPlugins() const { return !m_plugins.empty(); }
    size_t pluginCount() const { return m_plugins.size(); }

    // Dispatch lifecycle hooks
    void onUpdate(const EdvrPosef& headPose, float dtSeconds);
    void onRenderEye(const EdvrEyeRenderContext& eyeCtx);
    bool onFilterInput(uint32_t deviceType, const void* rawInputData);

private:
    bool m_initialized = false;
    std::vector<LoadedPlugin> m_plugins;
};

} // namespace edvr::plugins
