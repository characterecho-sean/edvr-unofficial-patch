#pragma once

#include "../../include/edvr_plugin_api.h"
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

namespace edvr::plugins {

struct CandidatePlugin {
    uint32_t id = 0;        // stable for the manager's life; a menu row holds this, never a pointer
    std::wstring path;
    std::string stem;       // e.g. "edvr_mfd"
    std::string sha256;
    bool approved = false;
};

struct LoadedPlugin {
    std::wstring path;
    std::string stem;
    std::string sha256;
    HMODULE module = nullptr;
    EdvrPluginCallbacks callbacks{};
    bool disabled = false;
    DWORD faultCode = 0;
};

struct ApprovalResult {
    bool applied = false;   // the candidate existed, was not yet approved, and is now (false: stale id, already approved, or the file changed)
    bool loaded = false;    // ...and the addon is running
    std::string stem;
    std::string sha256;     // the hash that was approved; what belongs in [addons]
};

class PluginManager {
public:
    static PluginManager& instance();

    PluginManager();
    ~PluginManager();

    // Discovers plugins in <rootDir>\plugins\ without loading unapproved DLLs.
    // Loads only approved plugins whose SHA-256 matches [addons] in config.
    bool initialize(const std::wstring& rootDir);
    void shutdown();

    bool hasActivePlugins();
    size_t activePluginCount();

    // Candidacy and approval (Trust-On-First-Use)
    // A copy of the candidates taken under the manager's lock. It can be held after the lock is released,
    // and lets the menu take the manager lock before its own (see m_mutex).
    std::vector<CandidatePlugin> candidatesSnapshot();
    bool approvePlugin(const std::string& stem, const std::string& sha256);
    // Approves by the id a menu row holds. A stale id, or a candidate already approved, does nothing.
    ApprovalResult approveCandidate(uint32_t id);
    // A copy of the candidate with this id; false when there is none.
    bool candidateById(uint32_t id, CandidatePlugin* out);

    // Dispatch lifecycle hooks wrapped in SEH fault containment
    void onUpdate(const EdvrPosef& headPose, float dtSeconds);
    void onRenderEye(const EdvrEyeRenderContext& eyeCtx);
    bool onFilterInput(uint32_t deviceType, const void* rawInputData);

    // Calculate file SHA-256 without loading
    static std::string calculateSha256(const std::wstring& path);

private:
    // Loads only if the file on disk still hashes to approvedSha; otherwise resurfaces cand under its new hash.
    bool loadPlugin(CandidatePlugin& cand, const std::string& approvedSha);
    void unloadAll();

    // Lock order, global: this mutex is taken BEFORE the menu's plugin-settings mutex, never after it.
    // loadPlugin (registration during onInitialize) and the fault paths (menuRemovePluginSettings) take the
    // settings mutex while holding this one; so the menu must not call into the manager while it holds
    // the settings mutex (it snapshots candidates first, and rebuilds pages after releasing it).
    std::recursive_mutex m_mutex;
    bool m_initialized = false;
    std::wstring m_rootDir;
    std::vector<CandidatePlugin> m_candidates;
    uint32_t m_nextCandidateId = 1;   // never reset, so an id from before a re-initialise cannot name a new candidate
    std::vector<LoadedPlugin> m_plugins;

    LARGE_INTEGER m_qpcPrev{};
    LARGE_INTEGER m_qpcFreq{};
};

} // namespace edvr::plugins
