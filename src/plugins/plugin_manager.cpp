#include "plugin_manager.h"
#include "../common/log.h"
#include "../common/config.h"
#include "../common/runtime_profile.h"
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

// The addon whose onInitialize is running ON THIS THREAD. Settings may be registered only then, so every one has an
// owner that can be removed with it. It is per thread: a worker thread of an addon, or a host callback an addon kept
// and calls later, has no context on its own thread and is refused, however it overlaps another addon's initialise.
thread_local const char* t_registeringOwner = nullptr;

int EDVR_API hostRegisterSetting(const EdvrPluginSettingDef* setting) {
    if (!t_registeringOwner) {
        static std::atomic<bool> noted{false};
        if (!noted.exchange(true)) {
            Log::get().note("plugin_manager: an addon called registerSetting outside its own onInitialize (another thread, or after it "
                            "returned); refused. Further refusals are not logged.");
        }
        return -1;
    }
    return edvr::menuRegisterPluginSetting(t_registeringOwner, setting);
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

// The one predicate for whether addons may run: only the Vr and LegacyVr profiles. Flat, Passthrough (a VR-only install in a flat game
// mode) and anything else is dormant (docs/design-plugin-architecture-2026-09-30.md, section 11). Startup, dispatch, approval, loading and
// profile transitions all ask this and nothing else.
bool addonsDormantProfile() {
    return !runtimeVrProfile();
}

// True outside a VR session, where plugins are dormant. The line is the log's proof that the gate, not a missing plugins directory, is
// why nothing loaded.
bool vrOnlyBlocked(const char* what) {
    if (!addonsDormantProfile()) return false;
    Log::get().note("plugin_manager: %s skipped, plugins are VR-only and this is a %s session", what, runtimeProfileName());
    return true;
}

} // namespace

// Leaked on purpose. A function-static would be destroyed by the CRT while the DLL detaches, after the proxy's DllMain has run and
// so past its reserved != nullptr guard; the destructor's shutdown() locks m_mutex and calls addon code under the loader lock,
// which can hang an exiting process. The proxy calls shutdown() itself, in the one case where that is safe (a real FreeLibrary).
PluginManager& PluginManager::instance() {
    static PluginManager* const s_instance = new PluginManager();
    return *s_instance;
}

PluginManager::PluginManager() {
    QueryPerformanceFrequency(&m_qpcFreq);
    QueryPerformanceCounter(&m_qpcPrev);
}

// No shutdown work here: the instance is never destroyed (see instance()), and an owner that wants one calls shutdown().
PluginManager::~PluginManager() = default;

namespace {

// Hashes the whole file behind an open handle, from offset 0. The caller keeps the handle.
std::string sha256OfHandle(HANDLE f) {
    if (SetFilePointer(f, 0, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) return "";

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
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
    return hex;
}

std::string lowered(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::string PluginManager::calculateSha256(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    std::string hex = sha256OfHandle(f);
    CloseHandle(f);
    return hex;
}

bool PluginManager::dormantLocked() const {
    return m_dormant || addonsDormantProfile();
}

bool PluginManager::initialize(const std::wstring& rootDir) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_initialized) return true;
    m_initialized = true;
    m_discovered = false;
    m_rootDir = rootDir;
    // Plugins are VR-only: the only code that updates and draws them is the native OpenXR scene pass, which a flat or passthrough session does not
    // run, and the [addons] approvals are not even readable there (runtimeProfileAllowsKey). A session that begins in any non-VR profile therefore
    // discovers nothing, initialises nothing and registers no settings yet; onProfileSwitched does all of that on the first switch into VR. Elite
    // offers no flat-to-VR switch after a flat launch today, so in practice a flat launch loads no add-on at all; the deferral keeps the path
    // correct if that switch ever arrives (design doc section 11, "Flat to VR").
    if (addonsDormantProfile()) {
        m_dormant = true;
        Log::get().note("plugin_manager: %s session, no addon loaded (plugins are VR-only); discovery waits for a switch into VR",
                        runtimeProfileName());
        return true;
    }
    m_dormant = false;
    discoverAndLoad();
    return true;
}

void PluginManager::onProfileSwitched() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_initialized) return;
    const bool dormant = addonsDormantProfile();
    if (dormant == m_dormant) return;
    if (dormant) {
        m_dormant = true;
        // The normal release path, so the release tail applies: a key the addon was holding stays up until it is let go.
        clearAllInputHolds();
        inputGateSetPluginBlock(false);
        edvr::menuSetPluginRowsHidden(true);
        Log::get().note("plugin_manager: profile switched to %s, %zu addon(s) suspended (rows hidden, no callbacks, input released)",
                        runtimeProfileName(), m_plugins.size());
        return;
    }
    m_dormant = false;
    if (!m_discovered) {
        Log::get().note("plugin_manager: profile switched to VR, running the deferred addon discovery");
        discoverAndLoad();
    }
    edvr::menuSetPluginRowsHidden(false);
    size_t active = 0;
    for (const auto& p : m_plugins) {
        if (!p.disabled) ++active;
    }
    Log::get().note("plugin_manager: profile switched to VR, %zu addon(s) active", active);
}

void PluginManager::discoverAndLoad() {
    m_discovered = true;
    std::wstring pluginsDir = m_rootDir + L"\\plugins";
    std::wstring searchPattern = pluginsDir + L"\\*";

    WIN32_FIND_DATAW findData{};
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        Log::get().note("plugin_manager: no plugins directory at %ls", pluginsDir.c_str());
        return;
    }

    do {
        if (findData.cFileName[0] == L'.') continue;

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring subDll = pluginsDir + L"\\" + findData.cFileName + L"\\plugin.dll";
            DWORD attrs = GetFileAttributesW(subDll.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                CandidatePlugin cand;
                cand.id = m_nextCandidateId++;
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
                cand.id = m_nextCandidateId++;
                cand.path = flatDll;
                cand.stem = sanitizeStem(baseStem);
                cand.sha256 = calculateSha256(flatDll);
                m_candidates.push_back(cand);
            }
        }
    } while (FindNextFileW(hFind, &findData));
    FindClose(hFind);

    // An addon's identity is its sanitized stem: it keys the approval in [addons], tags the addon's settings and names its saved keys. Two files
    // with one identity cannot be told apart by any of those, so before anything loads every file of such a group is refused, approved hash or not.
    for (auto& cand : m_candidates) {
        size_t sameIdentity = 0;
        for (const auto& other : m_candidates) {
            if (other.stem == cand.stem) ++sameIdentity;
        }
        cand.conflicted = sameIdentity > 1;
    }
    for (const auto& cand : m_candidates) {
        if (!cand.conflicted) continue;
        bool firstOfGroup = true;
        std::wstring paths;
        size_t count = 0;
        for (const auto& other : m_candidates) {
            if (other.stem != cand.stem) continue;
            if (other.id < cand.id) firstOfGroup = false;
            if (!paths.empty()) paths += L", ";
            paths += other.path;
            ++count;
        }
        if (firstOfGroup) {   // once per identity
            Log::get().note("plugin_manager: addon identity '%s' is claimed by %zu files (%ls); none loaded. Rename or remove all but one.",
                            cand.stem.c_str(), count, paths.c_str());
        }
    }

    // Evaluate approval against config [addons]
    for (auto& cand : m_candidates) {
        if (cand.conflicted) {
            cand.approved = false;
            continue;
        }
        std::string approvedSha = Config::get().getString(("addons." + cand.stem).c_str(), "");
        const std::string candShaLower = lowered(cand.sha256);
        const std::string approvedShaLower = lowered(approvedSha);

        if (!approvedShaLower.empty() && approvedShaLower == candShaLower) {
            cand.approved = true;
            loadPlugin(cand, cand.sha256);
        } else {
            cand.approved = false;
            Log::get().note("plugin_manager: discovered unapproved addon '%s' (sha256=%s)",
                            cand.stem.c_str(), cand.sha256.c_str());
        }
    }

    Log::get().note("plugin_manager: initialized (%zu candidates, %zu active)",
                    m_candidates.size(), m_plugins.size());
}

bool PluginManager::loadPlugin(CandidatePlugin& cand, const std::string& approvedSha) {
    cand.loadNote.clear();
    if (vrOnlyBlocked("load")) { cand.loadNote = "load failed: addons run only in VR"; return false; }   // approval must never load an addon that nothing would run
    if (m_dormant) { cand.loadNote = "load failed: addons run only in VR"; return false; }
    // Approval names a hash, so the bytes that run must be the bytes that hash. Hold the file with
    // write and delete sharing denied, hash it through that handle, and load it while the handle is
    // still open: nothing can swap the file between the check and the load.
    HANDLE hold = CreateFileW(cand.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hold == INVALID_HANDLE_VALUE) {
        Log::get().note("plugin_manager: cannot hold %ls for verification (err=%lu)", cand.path.c_str(), GetLastError());
        cand.loadNote = "load failed: file locked or unreadable";
        return false;
    }
    std::string actualSha = sha256OfHandle(hold);
    if (actualSha.empty() || lowered(actualSha) != lowered(approvedSha)) {
        Log::get().note("plugin_manager: addon %s changed since it was approved (approved %s, now %s); "
                        "not loaded, needs approval again",
                        cand.stem.c_str(), approvedSha.c_str(), actualSha.c_str());
        CloseHandle(hold);
        if (!actualSha.empty()) {
            cand.sha256 = actualSha;
            cand.approved = false;
            cand.loadNote = "file changed: review and approve the new version";
        } else {
            cand.loadNote = "load failed: file could not be read";
        }
        return false;
    }

    HMODULE hMod = LoadLibraryW(cand.path.c_str());
    CloseHandle(hold);
    if (!hMod) {
        Log::get().note("plugin_manager: failed to load %ls (err=%lu)", cand.path.c_str(), GetLastError());
        cand.loadNote = "load failed: not a loadable DLL";
        return false;
    }

    auto regFunc = reinterpret_cast<EdvrPluginRegisterFunc>(GetProcAddress(hMod, "EdvrPluginRegister"));
    if (!regFunc) {
        Log::get().note("plugin_manager: %ls missing EdvrPluginRegister export", cand.path.c_str());
        FreeLibrary(hMod);
        cand.loadNote = "load failed: addon refused";
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
        cand.loadNote = "load failed: addon refused";
        return false;
    }
    if (regResult != 0 || lp.callbacks.structSize < sizeof(EdvrPluginCallbacks)) {
        Log::get().note("plugin_manager: addon %s rejected API version %u or invalid structSize (res=%d)",
                        cand.stem.c_str(), EDVR_PLUGIN_API_VERSION, regResult);
        FreeLibrary(hMod);
        cand.loadNote = "load failed: addon refused";
        return false;
    }

    if (lp.callbacks.onInitialize) {
        EdvrHostServices host{};
        host.structSize = sizeof(EdvrHostServices);
        host.registerSetting = hostRegisterSetting;
        host.logNote = hostLogNote;

        const char* const outerOwner = t_registeringOwner;
        t_registeringOwner = cand.stem.c_str();
        int initResult = callInitSafely(lp.callbacks.onInitialize, &host, &fault);
        t_registeringOwner = outerOwner;
        if (fault != 0 || initResult != 0) {
            Log::get().note("plugin_manager: addon %s onInitialize failed (code=%d fault=0x%08X)",
                            cand.stem.c_str(), initResult, fault);
            edvr::menuRemovePluginSettings(cand.stem.c_str());   // its getters and setters point into the DLL about to go
            FreeLibrary(hMod);
            cand.loadNote = "load failed: addon could not start";
            return false;
        }
    }

    // An addon that filters input needs the user32 doors whether or not the player ever opens F8.
    if (lp.callbacks.onFilterInput) edvr::inputGateInstallForPlugin();

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
    if (m_dormant) edvr::menuSetPluginRowsHidden(false);
    m_initialized = false;
    m_discovered = false;
    m_dormant = false;
}

void PluginManager::clearAllInputHolds() {
    for (auto& p : m_plugins) p.holdingInput = false;
}

void PluginManager::releaseInputAfterFault(LoadedPlugin& faulted) {
    faulted.holdingInput = false;
    for (const auto& p : m_plugins) {
        if (!p.disabled && p.holdingInput) return;   // another addon still owns input: its hold stands
    }
    inputGateSetPluginBlock(false);   // the faulted addon was the last holder: the normal release path, tail included
}

void PluginManager::unloadAll() {
    clearAllInputHolds();
    inputGateSetPluginBlock(false);   // no addon outlives its input ownership
    for (auto& p : m_plugins) {
        if (p.callbacks.onShutdown && !p.disabled) {
            DWORD fault = 0;
            callShutdownSafely(p.callbacks.onShutdown, &fault);
            if (fault != 0) {
                Log::get().note("plugin_manager: addon %s faulted during onShutdown (0x%08X)",
                                p.stem.c_str(), fault);
            }
        }
        edvr::menuRemovePluginSettings(p.stem.c_str());
        if (p.module) {
            FreeLibrary(p.module);
            p.module = nullptr;
        }
    }
    m_plugins.clear();
}

bool PluginManager::hasActivePlugins() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (dormantLocked()) return false;   // suspended: nothing will be called, so the scene pass need not try
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

std::vector<CandidatePlugin> PluginManager::candidatesSnapshot() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_candidates;
}

bool PluginManager::approvePlugin(const std::string& stem, const std::string& sha256) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (auto& cand : m_candidates) {
        if (cand.stem == stem && cand.sha256 == sha256) {
            return approveCandidate(cand.id).loaded;
        }
    }
    return false;
}

ApprovalResult PluginManager::approveCandidate(uint32_t id) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    ApprovalResult r;
    if (dormantLocked()) return r;   // a flat session: there is nothing to run an approved addon, and its row is hidden
    for (auto& cand : m_candidates) {
        if (cand.id != id) continue;
        if (cand.approved) return r;   // already approved: a second activation of a stale row
        if (cand.conflicted) return r; // another file has this addon's identity: nothing can be approved until all but one is gone
        r.stem = cand.stem;
        if (cand.sha256.empty()) {     // the file could not be hashed at discovery: there is no hash to approve
            cand.loadNote = "cannot approve: file could not be read";
            r.reason = cand.loadNote;
            return r;
        }
        const std::string approvedSha = cand.sha256;
        cand.approved = true;
        r.loaded = loadPlugin(cand, approvedSha);
        // The approval stands only if the addon is running. A changed file already resurfaced itself under its new hash; any other
        // failure leaves the same candidate unapproved so it can be tried again, and the caller saves nothing.
        if (!r.loaded) {
            cand.approved = false;
            r.reason = cand.loadNote;
            return r;
        }
        r.applied = true;
        r.sha256 = approvedSha;
        return r;
    }
    return r;
}

bool PluginManager::candidateById(uint32_t id, CandidatePlugin* out) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (const auto& cand : m_candidates) {
        if (cand.id == id) {
            if (out) *out = cand;
            return true;
        }
    }
    return false;
}

void PluginManager::onUpdate(const EdvrPosef& headPose, float dtSeconds) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (dormantLocked()) return;

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
            edvr::menuRemovePluginSettings(p.stem.c_str());   // a disabled addon's rows would still call into its DLL
            releaseInputAfterFault(p);
        }
    }
}

void PluginManager::onRenderEye(const EdvrEyeRenderContext& eyeCtx) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (dormantLocked()) return;
    for (auto& p : m_plugins) {
        if (p.disabled || !p.callbacks.onRenderEye) continue;
        DWORD fault = 0;
        callRenderEyeSafely(p.callbacks.onRenderEye, &eyeCtx, &fault);
        if (fault != 0) {
            p.disabled = true;
            p.faultCode = fault;
            Log::get().note("plugin_manager: addon '%s' faulted onRenderEye (0x%08X); disabled for session",
                            p.callbacks.pluginName ? p.callbacks.pluginName : p.stem.c_str(), fault);
            edvr::menuRemovePluginSettings(p.stem.c_str());   // a disabled addon's rows would still call into its DLL
            releaseInputAfterFault(p);
        }
    }
}

bool PluginManager::onFilterInput(uint32_t deviceType, const void* rawInputData) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (dormantLocked()) {   // suspended (or the profile has just turned flat): a dormant addon never holds input
        clearAllInputHolds();
        inputGateSetPluginBlock(false);
        return false;
    }
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
            edvr::menuRemovePluginSettings(p.stem.c_str());   // a disabled addon's rows would still call into its DLL
            p.holdingInput = false;   // no release here: the aggregate below is applied once, so another addon's hold is never dropped mid-loop
        } else {
            p.holdingInput = ctx.swallowInput != 0;
            if (p.holdingInput) swallowed = true;
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
