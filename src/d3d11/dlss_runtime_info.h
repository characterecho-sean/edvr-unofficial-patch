#pragma once

#include <windows.h>

#include <string>
#include <cstddef>

namespace edvr::dlss_runtime_info {

// A snapshot of a module that is already loaded. This code never loads a
// module; callers must provide an HMODULE obtained from a real load or use
// inspectLoadedDlssModule(). The UTF-8 path is either complete or unavailable.
struct ModuleInfo final {
    HMODULE module = nullptr;
    bool loaded = false;
    bool pathAvailable = false;
    bool versionAvailable = false;
    std::string pathUtf8;
    std::string fileVersion;
};

// The caller keeps a supplied module alive during inspection. Version data
// comes from its mapped RT_VERSION/1 resource, never the file at its path.
// Missing/malformed resource data is unavailable; this is not a binary hash.
ModuleInfo inspectModule(HMODULE module) noexcept;
ModuleInfo inspectLoadedDlssModule() noexcept;

// Called only after NGX has successfully created a DLSS feature. Emits one
// log record when the observed loaded-module state changes. Repeated feature
// creations from the same loaded module are deduplicated. If Windows unloads
// and reloads a binary at the same HMODULE with identical path/version between
// these cold observations, that transition cannot be distinguished; no
// reference is retained and this diagnostic does not poll for reloads.
void reportAfterSuccessfulFeatureCreation() noexcept;

#if defined(EDVR_DLSS_RUNTIME_INFO_TEST)
bool versionResourceValidForTest(const void* data, std::size_t bytes) noexcept;
// Exercises the same emitter/deduplication with actual inspected modules.
void reportModuleForTest(HMODULE module) noexcept;
#endif

}  // namespace edvr::dlss_runtime_info
