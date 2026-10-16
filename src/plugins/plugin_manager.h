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
    // Another file in plugins\ has the same canonical identity (the sanitized stem, so plugins\foo.dll, plugins\foo\plugin.dll and
    // foo-bar.dll / foo_bar.dll all collide). Every file of such a group is refused: none loads and none can be approved.
    bool conflicted = false;
    // Why the last attempt to load this candidate failed, short enough for a menu hint; empty when none failed. Cleared by a load that succeeds.
    std::string loadNote;
};

struct LoadedPlugin {
    std::wstring path;
    std::string stem;
    std::string sha256;
    HMODULE module = nullptr;
    EdvrPluginCallbacks callbacks{};
    bool disabled = false;
    bool holdingInput = false;   // this addon's swallowInput from its last onFilterInput call; cleared when it faults, is disabled or the profile suspends it
    DWORD faultCode = 0;
};

struct ApprovalResult {
    // The approval took effect: the candidate existed, was not yet approved, and the addon loaded and initialised. Only then does the
    // caller record the approval in [addons]. False for a stale id, an already approved candidate, a changed file (it resurfaces under
    // its new hash) and any failed load (the candidate stays unapproved so the approval can be retried).
    bool applied = false;
    bool loaded = false;    // ...and the addon is running
    std::string stem;
    std::string sha256;     // the hash that was approved; what belongs in [addons]
    std::string reason;     // short, for the menu: why an approval did not take effect; empty when it did or the id named nothing
};

class PluginManager {
public:
    // Never destroyed: the object is leaked, so nothing runs at process exit or DLL detach. Call shutdown() for a clean teardown
    // (the proxy does, only from a real FreeLibrary).
    static PluginManager& instance();

    PluginManager();
    ~PluginManager();

    // Discovers plugins in <rootDir>\plugins\ without loading unapproved DLLs.
    // Loads only approved plugins whose SHA-256 matches [addons] in config.
    // Plugins are VR-only: they run in the Vr and LegacyVr profiles, and every other profile (Flat, Passthrough) is
    // dormant. Launched in a dormant profile this only remembers rootDir: discovery, approval and loading wait for
    // the first switch into VR (onProfileSwitched).
    bool initialize(const std::wstring& rootDir);
    void shutdown();

    // The runtime profile just changed (called from the mode watch once the new profile is in effect). Switching to
    // a dormant profile (Flat, Passthrough) suspends the addons: no callback is made, their input ownership is
    // released and their F8 rows are hidden. Switching to VR resumes them, running the deferred discovery first when
    // the session began dormant. Nothing is unloaded at a switch, so an addon's state and settings survive the trip.
    void onProfileSwitched();

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
    // Finds the candidates, evaluates approval and loads the approved ones. Once per initialize, in a VR profile.
    void discoverAndLoad();
    // True when no callback may be made and nothing may load: suspended, or the profile has just left VR.
    bool dormantLocked() const;
    // After an addon faults: drops its hold, and releases the input block only when no remaining enabled addon holds input.
    void releaseInputAfterFault(LoadedPlugin& faulted);
    void clearAllInputHolds();

    // Lock order, global: this mutex is taken BEFORE the menu's plugin-settings mutex, never after it.
    // loadPlugin (registration during onInitialize) and the fault paths (menuRemovePluginSettings) take the
    // settings mutex while holding this one; so the menu must not call into the manager while it holds
    // the settings mutex (it snapshots candidates first, and rebuilds pages after releasing it).
    std::recursive_mutex m_mutex;
    bool m_initialized = false;
    bool m_discovered = false;   // discoverAndLoad has run since initialize
    bool m_dormant = false;      // suspended outside VR (Flat, Passthrough): loaded addons stay loaded, but nothing calls them
    std::wstring m_rootDir;
    std::vector<CandidatePlugin> m_candidates;
    uint32_t m_nextCandidateId = 1;   // never reset, so an id from before a re-initialise cannot name a new candidate
    std::vector<LoadedPlugin> m_plugins;

    LARGE_INTEGER m_qpcPrev{};
    LARGE_INTEGER m_qpcFreq{};
};

} // namespace edvr::plugins
