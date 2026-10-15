#include "plugin_manager.h"
#include "../common/log.h"
#include "../common/config.h"
#include "../d3d11/input_gate.h"
#include "../d3d11/menu.h"

#include <bcrypt.h>
#include <algorithm>
#include <iomanip>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")

namespace edvr::plugins {

namespace {

// C-style SEH wrappers avoiding C2712 (C++ destructor unwinding inside __try)
int callInitSafely(int (EDVR_API *fn)(const EdvrHostServices*), const EdvrHostServices* host, DWORD* outFault) {
    __try {
        return fn(host);
    } __except (*outFault = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int callRegisterSafely(EdvrPluginRegisterFunc fn, uint32_t apiVersion, EdvrPluginCallbacks* callbacks, DWORD* outFault) {
    __try {
        return fn(apiVersion, callbacks);
    } __except (*outFault = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void callShutdownSafely(void (EDVR_API *fn)(void), DWORD* outFault) {
    __try {
        fn();
    } __except (*outFault = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {}
}

void callUpdateSafely(void (EDVR_API *fn)(const EdvrPosef*, float), const EdvrPosef* pose, float dt, DWORD* outFault) {
    __try {
        fn(pose, dt);
    } __except (*outFault = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {}
}

void callRenderEyeSafely(void (EDVR_API *fn)(const EdvrEyeRenderContext*), const EdvrEyeRenderContext* ctx, DWORD* outFault) {
    __try {
        fn(ctx);
    } __except (*outFault = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {}
}

void callFilterInputSafely(void (EDVR_API *fn)(EdvrInputContext*), EdvrInputContext* ctx, DWORD* outFault) {
    __try {
        fn(ctx);
    } __except (*outFault = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {}
}

int EDVR_API hostRegisterSetting(const EdvrPluginSettingDef* setting) {
    return edvrRegisterPluginSetting(setting);
}

void EDVR_API hostLogNote(const char* msg) {
    if (msg) edvr::Log::get().note("addon: %s", msg);
}

std::string sanitizeStem(const std::wstring& ws) {
    std::string s;
    for (wchar_t wc : ws) {
        if (wc >= L'a' && wc <= L'z') s.push_back(static_cast<char>(wc));
        else if (wc >= L'A' && wc <= L'Z') s.push_back(static_cast<char>(tolower(wc)));
        else if (wc >= L'0' && wc <= L'9') s.push_back(static_cast<char>(wc));
        else s.push_back('_');
    }
    return s;
}

} // namespace

PluginManager& PluginManager::instance() {
    static PluginManager s_instance;
    return s_instance;
}

PluginManager::PluginManager() {
    QueryPerformanceFrequency(&m_qpcFreq);
    QueryPerformanceCounter(&m_qpcPrev);
}

PluginManager::~PluginManager() {
    shutdown();
}

std::string PluginManager::calculateSha256(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        CloseHandle(f);
        return "";
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> chunk(64 * 1024);
        bool ok = true;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(f, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)) {
                ok = false;
                break;
            }
            if (read == 0) break;
            if (BCryptHashData(hash, chunk.data(), read, 0) != 0) {
                ok = false;
                break;
            }
        }
        if (ok) {
            unsigned char digest[32]{};
            if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                char buf[65]{};
                for (int i = 0; i < 32; ++i) {
                    snprintf(buf + i * 2, 3, "%02x", digest[i]);
                }
                hex = buf;
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return hex;
}

bool PluginManager::initialize(const std::wstring& rootDir) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_initialized) return true;
    m_initialized = true;
    m_rootDir = rootDir;

    std::wstring pluginsDir = rootDir + L"\\plugins";
    std::wstring searchPattern = pluginsDir + L"\\*";

    WIN32_FIND_DATAW findData{};
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        Log::get().note("plugin_manager: no plugins directory at %ls", pluginsDir.c_str());
        return true;
    }

    do {
        if (findData.cFileName[0] == L'.') continue;

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring subDll = pluginsDir + L"\\" + findData.cFileName + L"\\plugin.dll";
            DWORD attrs = GetFileAttributesW(subDll.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                CandidatePlugin cand;
                cand.path = subDll;
                cand.stem = sanitizeStem(findData.cFileName);
                cand.sha256 = calculateSha256(subDll);
                m_candidates.push_back(cand);
            }
        } else {
            std::wstring filename = findData.cFileName;
            if (filename.size() > 4 && _wcsicmp(filename.c_str() + filename.size() - 4, L".dll") == 0) {
                std::wstring flatDll = pluginsDir + L"\\" + filename;
                std::wstring baseStem = filename.substr(0, filename.size() - 4);
                CandidatePlugin cand;
                cand.path = flatDll;
                cand.stem = sanitizeStem(baseStem);
                cand.sha256 = calculateSha256(flatDll);
                m_candidates.push_back(cand);
            }
        }
    } while (FindNextFileW(hFind, &findData));
    FindClose(hFind);

    // Evaluate approval against config [addons]
    for (auto& cand : m_candidates) {
        std::string approvedSha = Config::get().getString(("addons." + cand.stem).c_str(), "");
        std::string candShaLower = cand.sha256;
        for (char& c : candShaLower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        std::string approvedShaLower = approvedSha;
        for (char& c : approvedShaLower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

        if (!approvedShaLower.empty() && approvedShaLower == candShaLower) {
            cand.approved = true;
            loadPlugin(cand);
        } else {
            cand.approved = false;
            Log::get().note("plugin_manager: discovered unapproved addon '%s' (sha256=%s)",
                            cand.stem.c_str(), cand.sha256.c_str());
        }
    }

    Log::get().note("plugin_manager: initialized (%zu candidates, %zu active)",
                    m_candidates.size(), m_plugins.size());
    return true;
}

bool PluginManager::loadPlugin(const CandidatePlugin& cand) {
    HMODULE hMod = LoadLibraryW(cand.path.c_str());
    if (!hMod) {
        Log::get().note("plugin_manager: failed to load %ls (err=%lu)", cand.path.c_str(), GetLastError());
        return false;
    }

    auto regFunc = reinterpret_cast<EdvrPluginRegisterFunc>(GetProcAddress(hMod, "EdvrPluginRegister"));
    if (!regFunc) {
        Log::get().note("plugin_manager: %ls missing EdvrPluginRegister export", cand.path.c_str());
        FreeLibrary(hMod);
        return false;
    }

    LoadedPlugin lp;
    lp.path = cand.path;
    lp.stem = cand.stem;
    lp.sha256 = cand.sha256;
    lp.module = hMod;
    lp.callbacks.structSize = sizeof(EdvrPluginCallbacks);

    DWORD fault = 0;
    int regResult = callRegisterSafely(regFunc, EDVR_PLUGIN_API_VERSION, &lp.callbacks, &fault);
    if (fault != 0) {
        Log::get().note("plugin_manager: addon %s faulted in EdvrPluginRegister (0x%08X); rejected",
                        cand.stem.c_str(), fault);
        FreeLibrary(hMod);
        return false;
    }
    if (regResult != 0 || lp.callbacks.structSize < sizeof(EdvrPluginCallbacks)) {
        Log::get().note("plugin_manager: addon %s rejected API version %u or invalid structSize (res=%d)",
                        cand.stem.c_str(), EDVR_PLUGIN_API_VERSION, regResult);
        FreeLibrary(hMod);
        return false;
    }

    if (lp.callbacks.onInitialize) {
        EdvrHostServices host{};
        host.structSize = sizeof(EdvrHostServices);
        host.registerSetting = hostRegisterSetting;
        host.logNote = hostLogNote;

        int initResult = callInitSafely(lp.callbacks.onInitialize, &host, &fault);
        if (fault != 0 || initResult != 0) {
            Log::get().note("plugin_manager: addon %s onInitialize failed (code=%d fault=0x%08X)",
                            cand.stem.c_str(), initResult, fault);
            FreeLibrary(hMod);
            return false;
        }
    }

    const char* name = lp.callbacks.pluginName ? lp.callbacks.pluginName : cand.stem.c_str();
    const char* ver = lp.callbacks.pluginVersion ? lp.callbacks.pluginVersion : "unknown";
    Log::get().note("plugin_manager: loaded addon [%s v%s] from %ls", name, ver, cand.path.c_str());

    m_plugins.push_back(std::move(lp));
    return true;
}

void PluginManager::shutdown() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized) return;

    unloadAll();
    m_candidates.clear();
    m_initialized = false;
}

void PluginManager::unloadAll() {
    for (auto& p : m_plugins) {
        if (p.callbacks.onShutdown && !p.disabled) {
            DWORD fault = 0;
            callShutdownSafely(p.callbacks.onShutdown, &fault);
            if (fault != 0) {
                Log::get().note("plugin_manager: addon %s faulted during onShutdown (0x%08X)",
                                p.stem.c_str(), fault);
            }
        }
        if (p.module) {
            FreeLibrary(p.module);
            p.module = nullptr;
        }
    }
    m_plugins.clear();
}

bool PluginManager::hasActivePlugins() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (const auto& p : m_plugins) {
        if (!p.disabled) return true;
    }
    return false;
}

size_t PluginManager::activePluginCount() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    size_t count = 0;
    for (const auto& p : m_plugins) {
        if (!p.disabled) ++count;
    }
    return count;
}

const std::vector<CandidatePlugin>& PluginManager::candidates() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_candidates;
}

bool PluginManager::approvePlugin(const std::string& stem, const std::string& sha256) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (auto& cand : m_candidates) {
        if (cand.stem == stem && cand.sha256 == sha256) {
            cand.approved = true;
            return loadPlugin(cand);
        }
    }
    return false;
}

void PluginManager::onUpdate(const EdvrPosef& headPose, float dtSeconds) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    // Compute accurate real delta time if dtSeconds is defaulted or unmeasured
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (m_qpcPrev.QuadPart != 0 && m_qpcFreq.QuadPart != 0) {
        float measuredDt = static_cast<float>(static_cast<double>(now.QuadPart - m_qpcPrev.QuadPart) / m_qpcFreq.QuadPart);
        if (measuredDt >= 0.0001f && measuredDt <= 0.5f) {
            dtSeconds = measuredDt;
        }
    }
    m_qpcPrev = now;

    for (auto& p : m_plugins) {
        if (p.disabled || !p.callbacks.onUpdate) continue;
        DWORD fault = 0;
        callUpdateSafely(p.callbacks.onUpdate, &headPose, dtSeconds, &fault);
        if (fault != 0) {
            p.disabled = true;
            p.faultCode = fault;
            Log::get().note("plugin_manager: addon '%s' faulted onUpdate (0x%08X); disabled for session",
                            p.callbacks.pluginName ? p.callbacks.pluginName : p.stem.c_str(), fault);
        }
    }
}

void PluginManager::onRenderEye(const EdvrEyeRenderContext& eyeCtx) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (auto& p : m_plugins) {
        if (p.disabled || !p.callbacks.onRenderEye) continue;
        DWORD fault = 0;
        callRenderEyeSafely(p.callbacks.onRenderEye, &eyeCtx, &fault);
        if (fault != 0) {
            p.disabled = true;
            p.faultCode = fault;
            Log::get().note("plugin_manager: addon '%s' faulted onRenderEye (0x%08X); disabled for session",
                            p.callbacks.pluginName ? p.callbacks.pluginName : p.stem.c_str(), fault);
        }
    }
}

bool PluginManager::onFilterInput(uint32_t deviceType, const void* rawInputData) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    bool swallowed = false;
    for (auto& p : m_plugins) {
        if (p.disabled || !p.callbacks.onFilterInput) continue;
        EdvrInputContext ctx{};
        ctx.structSize = sizeof(EdvrInputContext);
        ctx.deviceType = deviceType;
        ctx.rawInputData = rawInputData;
        ctx.swallowInput = 0;

        DWORD fault = 0;
        callFilterInputSafely(p.callbacks.onFilterInput, &ctx, &fault);
        if (fault != 0) {
            p.disabled = true;
            p.faultCode = fault;
            Log::get().note("plugin_manager: addon '%s' faulted onFilterInput (0x%08X); disabled for session",
                            p.callbacks.pluginName ? p.callbacks.pluginName : p.stem.c_str(), fault);
        } else if (ctx.swallowInput) {
            swallowed = true;
        }
    }
    inputGateSetPluginBlock(swallowed);
    return swallowed;
}

} // namespace edvr::plugins

// Bridge exports for OpenXR runtime / test rigs
extern "C" {

__declspec(dllexport) void WINAPI edvrPluginDispatchUpdate(const EdvrPosef* headPose, float dtSeconds) {
    if (headPose) {
        edvr::plugins::PluginManager::instance().onUpdate(*headPose, dtSeconds);
    }
}

__declspec(dllexport) void WINAPI edvrPluginDispatchRenderEye(const EdvrEyeRenderContext* eyeCtx) {
    if (eyeCtx) {
        edvr::plugins::PluginManager::instance().onRenderEye(*eyeCtx);
    }
}

__declspec(dllexport) int WINAPI edvrPluginDispatchFilterInput(uint32_t deviceType, const void* rawInputData) {
    return edvr::plugins::PluginManager::instance().onFilterInput(deviceType, rawInputData) ? 1 : 0;
}

__declspec(dllexport) int WINAPI edvrPluginHasActivePlugins() {
    return edvr::plugins::PluginManager::instance().hasActivePlugins() ? 1 : 0;
}

} // extern "C"
