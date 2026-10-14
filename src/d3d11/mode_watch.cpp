// The mode watch's worker and its commit (mode_watch.h says what the API is; mode_watch_logic.h holds the rules). The worker owns the
// files, the wait and the log lines; the rules see none of it. The commit switches the profile live, at the frame boundary. Design:
// docs\design-unified-install-2026-10-12.md sections 3.2 to 3.4.
#include "mode_watch.h"

#include "device_hook.h"        // flatSwitchAvailable, deviceHookRequestConfigRefresh: the switch's gate and its forced configure sweep
#include "dlaa.h"               // dlaaReleaseForSwitch: DLSS features released, NGX kept
#include "engine_velocity.h"    // engineVelocityFlatAbandon: the flat teardown's velocity state
#include "flat_runtime.h"       // flatRuntimeResize: the flat teardown, run before a commit leaves flat
#include "fsr3_engine.h"        // fsr3ReleaseFeatures: FSR's features, released at every switch
#include "menu.h"               // menuProfileSwitch: the menu closes and forgets the outgoing profile's first-time block
#include "mode_watch_logic.h"
#include "perf_monitor.h"       // perfMonitorProfileSwitch: the frame interval restarts at a switch
#include "ui_layer.h"           // uiLayerProfileSwitch: the VR UI layers, torn down before a commit leaves VR
#include "xess_engine.h"        // xessReleaseFeatures: XeSS's features, released at every switch
#include "vr_ssaa_hold.h"       // vrSsaaHoldProfileChanged: the Supersampling hold follows the profile

#include "../common/config.h"
#include "../common/frame_flag.h"              // setVrProfileActive: the runtime's own frames stand down while the game is flat
#include "../common/ini_name.h"
#include "../common/install_scope.h"
#include "../common/log.h"
#include "../common/native_frame.h"           // nativeFrameLatchedFrames: the eye-submit count the watch samples
#include "../common/native_render_settings.h"  // edvrQueryNativeRenderSizing: the runtime's live VR session
#include "../common/runtime_profile.h"
#include "cross_profile_cost.h"   // the live-session query's cost while flat runs, once a frame (log only)
#include "../common/temporal_mode.h"          // temporalModeEnabled: the LOD bias default follows fix.temporal_aa

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

namespace edvr {
namespace {

using modewatch::Outcome;
using modewatch::Step;
using modewatch::Watch;

constexpr DWORD kMaxWaitMs = 1000;     // the stat backup and the openvr check run at this interval
constexpr uint64_t kSubmitPollMs = 250;  // while a live rise waits for the first eye submit, the wait ends this often
constexpr uint64_t kRetryMs = 100;    // a read that cannot open its file is retried this often
constexpr int kMaxRetries = 20;        // per write; then the watch waits for the next event
constexpr uint64_t kIgnoredLogMs = 1000;  // an unknown read repeats while the file stays invalid; its log line comes this often at most
constexpr size_t kMaxSettingsBytes = 1024 * 1024;

// The posted mode, packed so one exchange takes all of it: bit 0 posted, bit 1 Vr (clear is Flat), bit 2 from a live rise (clear is
// the file), and the time from bit 8 up.
constexpr uint64_t kPostedBit = 1, kVrBit = 2, kLiveBit = 4;
constexpr unsigned kTimeShift = 8;
std::atomic<uint64_t> g_posted{0};
std::atomic<uint32_t> g_hints{0};
std::atomic<HANDLE> g_hintEvent{nullptr};
std::atomic<bool> g_startCalled{false};
std::atomic<bool> g_started{false};

// The toast the menu shows once per commit. Written on the render thread, read by the menu's poll.
std::mutex g_noticeMutex;
std::string g_notice;
std::atomic<bool> g_noticeDue{false};

uint64_t nowMs() { return GetTickCount64(); }

const char* profileWord(RuntimeProfile p) {
    switch (p) {
    case RuntimeProfile::Vr: return "vr";
    case RuntimeProfile::Flat: return "flat";
    case RuntimeProfile::Passthrough: return "passthrough";
    default: return "invalid";
    }
}

void setNotice(const char* text) {
    std::lock_guard<std::mutex> lock(g_noticeMutex);
    g_notice = text;
    g_noticeDue.store(true, std::memory_order_release);
}

void postMode(uint64_t t, GameMode mode, bool live) {
    const uint64_t bits = kPostedBit | (mode == GameMode::Vr ? kVrBit : 0) | (live ? kLiveBit : 0);
    g_posted.store((t << kTimeShift) | bits, std::memory_order_release);
}

// Settings.xml's last write time, or 0 when the file is absent.
uint64_t writeTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

// The worker. One per process, pinned with the DLL, so it is never freed and has no shutdown path (module_pin.h).
class Worker {
public:
    Worker(GameMode launch, GameMode effective, std::wstring folder, std::wstring file, HANDLE hint)
        : watch_(launch, effective), folder_(std::move(folder)), file_(std::move(file)), hint_(hint) {}

    static DWORD WINAPI threadMain(LPVOID p) { return static_cast<Worker*>(p)->run(); }

private:
    DWORD run() {
        HANDLE handles[2] = {hint_, INVALID_HANDLE_VALUE};
        DWORD count = 1;
        HANDLE change = FindFirstChangeNotificationW(folder_.c_str(), FALSE,
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);
        const bool haveFolder = change != INVALID_HANDLE_VALUE;
        if (haveFolder) {
            handles[count++] = change;
        } else {
            Log::get().note("mode watch: no folder notification (error %lu); polling once a second", GetLastError());
        }
        lastWrite_ = writeTime(file_);
        submitsAtSample_ = nativeFrameLatchedFrames();
        nextSampleMs_ = nowMs() + kMaxWaitMs;

        for (;;) {
            // A timed read (a confirm or a retry) runs as soon as it is due; the wait below ends no later than the next one, and no
            // later than the next eye-submit sample.
            const uint64_t at = dueReadAt();
            if (at && nowMs() >= at) readFile(retryAt_ != 0);
            uint64_t wake = nextSampleMs_;
            const uint64_t next = timedReadAt();
            if (next && next < wake) wake = next;
            const uint64_t now = nowMs();
            // While a live rise waits for the first eye submit the wait is short, so the flip follows the first submit closely.
            if (watch_.awaitingSubmits() && (wake == 0 || wake > now + kSubmitPollMs)) wake = now + kSubmitPollMs;
            DWORD timeout = kMaxWaitMs;
            if (wake) timeout = wake > now ? static_cast<DWORD>(std::min<uint64_t>(kMaxWaitMs, wake - now)) : 0;

            const DWORD r = WaitForMultipleObjects(count, handles, FALSE, timeout);
            if (r == WAIT_OBJECT_0) {
                handleHints();
            } else if (haveFolder && r == WAIT_OBJECT_0 + 1) {
                if (!FindNextChangeNotification(change))
                    Log::get().note("mode watch: folder notification re-arm failed (error %lu)", GetLastError());
                checkWrite(false);
            } else if (r == WAIT_TIMEOUT) {
                // A hint can land before the event exists or be missed while the worker is between waits; the bits are read here too.
                if (g_hints.load(std::memory_order_acquire)) handleHints();
                sampleSubmits(nowMs());
                checkAwaitedSubmits(nowMs());
                checkWrite(true);
                checkOpenVr();
            } else {
                Log::get().note("mode watch: wait failed (error %lu); the watch stopped", GetLastError());
                return 1;
            }
        }
    }

    // The time a timed read (a pending mode's confirm, or a retry) is due, or 0. It can be past: the loop runs an overdue read first.
    uint64_t dueReadAt() const {
        if (gaveUp_) return 0;
        uint64_t at = watch_.pending() ? watch_.confirmDueMs() : 0;
        if (retryAt_ > at) at = retryAt_;
        return at;
    }

    // The wake time for the timed read: a future time or 0, never a past one. An overdue read has run before this is asked; if the read
    // did not move its deadline, the wait is one retry interval, so the loop cannot spin on a zero wait.
    uint64_t timedReadAt() const {
        const uint64_t at = dueReadAt();
        const uint64_t now = nowMs();
        return at && at <= now ? now + kRetryMs : at;
    }

    void handleHints() {
        const uint32_t bits = g_hints.exchange(0, std::memory_order_acq_rel);
        const uint64_t now = nowMs();
        if (bits & static_cast<uint32_t>(ModeHint::Setter))
            Log::get().note("mode watch: hint setter t=%llu", static_cast<unsigned long long>(now));
        if (bits & static_cast<uint32_t>(ModeHint::Window))
            Log::get().note("mode watch: hint window t=%llu", static_cast<unsigned long long>(now));
        if (bits & static_cast<uint32_t>(ModeHint::LiveRise)) {
            Log::get().note("mode watch: hint live-rise t=%llu", static_cast<unsigned long long>(now));
            // A live runtime alone does not post Vr: the runtime goes live for a cold flat start too (Elite loads openvr_api.dll with
            // no /novr). A live runtime with no file write (Epic's "Launch in VR") posts Vr once the game submits an eye frame
            // (checkAwaitedSubmits), a second or two later.
            const bool wasWaiting = watch_.awaitingSubmits();
            watch_.onLiveRise(now);
            if (watch_.awaitingSubmits()) {
                if (!wasWaiting) {
                    riseSubmits_ = submitsAtSample_;  // submits since the last sample count: Epic's can start before the hint lands
                    Log::get().note("mode watch: live session without eye submits -- staying flat until the game submits (eye submits last "
                                    "1 s: %llu)", static_cast<unsigned long long>(lastSecondSubmits_));
                }
            } else {
                Log::get().note("mode watch: live-rise t=%llu -> nothing", static_cast<unsigned long long>(now));
            }
        }
        readAfterWrite_ = false;  // a hint's read is not a write: a stale file does not count for it
        readFile(false);
    }

    // The folder's notification or the stat backup: a write is logged once per change of the file's write time.
    void checkWrite(bool fromStat) {
        const uint64_t wt = writeTime(file_);
        if (wt == lastWrite_) return;
        lastWrite_ = wt;
        char suffix[64];
        Log::get().note("mode watch: write t=%llu%s%s", static_cast<unsigned long long>(nowMs()), fromStat ? " (stat)" : "",
                        submitSuffix(suffix, sizeof(suffix)));
        readAfterWrite_ = true;  // a read of a stale file counts only after a write: the watch's rule (mode_watch_logic.h)
        readFile(false);
    }

    // The eye-submit rate: the frame pairs the game submitted in the last full second, sampled on the timeout path once a second. It is
    // file-independent, so it says whether the game still reaches the VR runtime when Settings.xml does not change. Logs only when the
    // state changes: "stopped" when a second with none follows one with some, "resumed" for the reverse.
    void sampleSubmits(uint64_t now) {
        if (now < nextSampleMs_) return;
        const uint64_t total = nativeFrameLatchedFrames();
        lastSecondSubmits_ = total - submitsAtSample_;
        submitsAtSample_ = total;
        nextSampleMs_ = now + kMaxWaitMs;
        const bool flowing = lastSecondSubmits_ > 0;
        if (flowing && !flowing_)
            Log::get().note("mode watch: eye submits resumed t=%llu (%llu/s)", static_cast<unsigned long long>(now),
                            static_cast<unsigned long long>(lastSecondSubmits_));
        else if (!flowing && flowing_)
            Log::get().note("mode watch: eye submits stopped t=%llu", static_cast<unsigned long long>(now));
        flowing_ = flowing;
    }

    // A live rise found the profile flat and is waiting for the game's first eye submit. The first submit past the rise's baseline posts
    // Vr (source "live-rise"); a flat start never submits one, so it stays flat.
    void checkAwaitedSubmits(uint64_t now) {
        if (!watch_.awaitingSubmits() || nativeFrameLatchedFrames() <= riseSubmits_) return;
        const Step s = watch_.onEyeSubmits(now);
        if (s.outcome != Outcome::Posted) return;
        postMode(now, GameMode::Vr, true);
        Log::get().note("mode watch: eye submits began after the live session; the game is a VR game");
        Log::get().note("mode watch: live-rise t=%llu -> posted vr", static_cast<unsigned long long>(now));  // edvr_log.py parses this line
    }

    // The suffix every read and write line carries: the last full second's eye submits (zero until the first sample completes).
    const char* submitSuffix(char* buf, size_t size) const {
        std::snprintf(buf, size, " (eye submits last 1 s: %llu)", static_cast<unsigned long long>(lastSecondSubmits_));
        return buf;
    }

    void checkOpenVr() {
        const bool loaded = GetModuleHandleW(L"openvr_api.dll") != nullptr;
        if (loaded == openvrLoaded_) return;
        openvrLoaded_ = loaded;
        Log::get().note("mode watch: openvr_api.dll %s t=%llu", loaded ? "loaded" : "unloaded",
                        static_cast<unsigned long long>(nowMs()));
    }

    // Reads Settings.xml and feeds the watch. A retry is a read the timer asked for; any other read starts a new attempt count.
    void readFile(bool retry) {
        const uint64_t now = nowMs();
        if (!retry) {
            retries_ = 0;
            gaveUp_ = false;
        }
        ModeRead r{};
        DWORD error = 0;
        char suffix[64];
        if (!readSettings(&r, &error)) {
            retryAt_ = 0;
            if (retries_ < kMaxRetries) {
                ++retries_;
                retryAt_ = now + kRetryMs;
                Log::get().note("mode watch: read failed t=%llu (error %lu); retry in 100 ms%s",
                                static_cast<unsigned long long>(now), error, submitSuffix(suffix, sizeof(suffix)));
            } else {
                gaveUp_ = true;
                Log::get().note("mode watch: read failed t=%llu (error %lu); giving up on this write after %d retries%s",
                                static_cast<unsigned long long>(now), error, kMaxRetries, submitSuffix(suffix, sizeof(suffix)));
            }
            return;
        }
        retryAt_ = 0;
        const Step s = watch_.onRead(now, r, readAfterWrite_);
        // An unknown read repeats while the file stays invalid, so its line is logged once a second at most; the next line printed says
        // how many were skipped.
        const bool unknownIgnored = s.outcome == Outcome::Ignored && !r.known;
        if (unknownIgnored && now < nextIgnoredLogMs_) {
            ++skippedIgnoredLogs_;
            return;
        }
        if (unknownIgnored) nextIgnoredLogMs_ = now + kIgnoredLogMs;

        char mode[16];
        if (r.known) std::snprintf(mode, sizeof(mode), "%d", r.value);
        else std::snprintf(mode, sizeof(mode), "unknown");
        char result[96];
        switch (s.outcome) {
        case Outcome::Ignored:
            std::snprintf(result, sizeof(result), "%s", s.staleFile ? "ignored (stale file)" : "ignored");
            break;
        case Outcome::Same: std::snprintf(result, sizeof(result), "same"); break;
        case Outcome::Pending:
            std::snprintf(result, sizeof(result), "pending %s confirm at t=%llu", gameModeName(s.mode),
                          static_cast<unsigned long long>(watch_.confirmDueMs()));
            break;
        case Outcome::Dropped: std::snprintf(result, sizeof(result), "dropped"); break;
        case Outcome::Posted:
            std::snprintf(result, sizeof(result), "posted %s", gameModeName(s.mode));
            break;
        default: std::snprintf(result, sizeof(result), "nothing"); break;
        }
        char retryNote[64] = "";
        if (unknownIgnored && watch_.pending())
            std::snprintf(retryNote, sizeof(retryNote), " (confirm retry %u of %u)", watch_.unknownRetries(), Watch::kMaxUnknownRetries);
        char skipped[64] = "";
        if (skippedIgnoredLogs_)
            std::snprintf(skipped, sizeof(skipped), " (%u unknown reads not logged)", skippedIgnoredLogs_);
        skippedIgnoredLogs_ = 0;
        Log::get().note("mode watch: read t=%llu mode %s -> %s%s%s%s", static_cast<unsigned long long>(now), mode, result, retryNote,
                        submitSuffix(suffix, sizeof(suffix)), skipped);
        if (s.outcome == Outcome::Posted) postMode(now, s.mode, false);
    }

    // False when the file could not be opened or read; the error is then the Windows error. An absent file reads as no mode, and so
    // does one too large to be Settings.xml.
    bool readSettings(ModeRead* out, DWORD* error) {
        HANDLE file = CreateFileW(file_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
                *out = readGameMode(nullptr);
                return true;
            }
            *error = e;
            return false;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size)) {
            *error = GetLastError();
            CloseHandle(file);
            return false;
        }
        if (size.QuadPart < 0 || size.QuadPart > static_cast<LONGLONG>(kMaxSettingsBytes)) {
            CloseHandle(file);
            *out = readGameMode("");
            return true;
        }
        std::string text(static_cast<size_t>(size.QuadPart), '\0');
        DWORD got = 0;
        const bool ok = text.empty() || (ReadFile(file, &text[0], static_cast<DWORD>(text.size()), &got, nullptr) && got == text.size());
        const DWORD e = GetLastError();
        CloseHandle(file);
        if (!ok) {
            *error = e;
            return false;
        }
        *out = readGameMode(text.c_str());
        return true;
    }

    Watch watch_;
    std::wstring folder_;
    std::wstring file_;
    HANDLE hint_;
    uint64_t lastWrite_ = 0;
    bool readAfterWrite_ = false;  // the current read follows a write of the file (or is a retry of one)
    uint64_t retryAt_ = 0;
    int retries_ = 0;
    bool gaveUp_ = false;
    uint64_t nextIgnoredLogMs_ = 0;    // the earliest time the next unknown read's log line is written
    unsigned skippedIgnoredLogs_ = 0;  // unknown reads not logged since the last line, reported on the next one
    bool openvrLoaded_ = false;
    // The eye-submit sampler: the counter at the last sample, the next sample's time, the last full second's count, and whether that
    // second had any. flowing_ starts false, so the first second with submits logs "resumed".
    uint64_t submitsAtSample_ = 0;
    uint64_t nextSampleMs_ = 0;
    uint64_t lastSecondSubmits_ = 0;
    bool flowing_ = false;
    uint64_t riseSubmits_ = 0;  // the eye-submit counter a live rise's wait starts from
};

// The settings patched into the game or the device at creation (design section 3.4, Q7). They keep the launch's values until the next
// launch, so the switch line names each one whose value in the file now differs from the value in effect.
constexpr size_t kPatchedCount = 3;
const char* const kPatchedKeys[kPatchedCount] = {"advanced.texture_anisotropy", "advanced.texture_lod_bias", "fix.vscreen_res_width"};
// The values in effect, read once in modeWatchStart (after Config::init, before the device exists). Never updated: a switch changes the
// file's values, not the game's.
std::string g_inEffect[kPatchedCount];

// The effective value of each patched setting under the active profile, with the defaults of the sites that read them
// (device_hook.cpp's texture sampler, vscreen_res.cpp's width).
void readPatched(std::string* out) {
    const Config& cfg = Config::get();
    const bool temporal = temporalModeEnabled(cfg.getString("fix.temporal_aa", "off"));
    out[0] = cfg.getString(kPatchedKeys[0], "0");
    out[1] = cfg.getString(kPatchedKeys[1], temporal ? "auto" : "0");
    out[2] = cfg.getString(kPatchedKeys[2], "auto");
}

// The rig's view of the switch (tools\profile_switch_test): real switches committed and teardowns run. The skip is the rig's control
// and is never set in production (edvr_selftest_profile_skip_teardown).
std::atomic<uint32_t> g_switches{0};
std::atomic<uint32_t> g_teardowns{0};
std::atomic<bool> g_skipTeardown{false};

// The render thread's commit (design section 3.3): the posted mode becomes the profile it selects. The outgoing profile is torn down while
// its predicates still answer true, the profile flips, the settings file is re-picked, and the configure sweep is forced for later in
// this same frame boundary.
void commitPosted(uint64_t packed) {
    const GameMode mode = (packed & kVrBit) ? GameMode::Vr : GameMode::Flat;
    const bool live = (packed & kLiveBit) != 0;
    const unsigned long long t = static_cast<unsigned long long>(packed >> kTimeShift);
    const char* cls = gameModeName(mode);
    const char* from = live ? "live-rise" : "file";
    RuntimeProfile cur = runtimeProfileValue();
    if (cur == RuntimeProfile::LegacyVr) cur = RuntimeProfile::Vr;
    RuntimeProfile target = activeProfileFor(runtimeInstallScope(), mode);
    // Without the ResizeBuffers hook the flat profile cannot run this session (device_hook.cpp hookSwapChain): the switch runs as passthrough.
    const bool flatRefused = target == RuntimeProfile::Flat && !flatSwitchAvailable();
    if (flatRefused) {
        target = RuntimeProfile::Passthrough;
        // The "profile switch: " prefix makes --switches show it as a note row beside the switch line (tools\edvr_log.py).
        Log::get().note("profile switch: flat refused, ResizeBuffers was not hooked this session (posted %s t=%llu from %s); the switch runs "
                        "as passthrough",
                        cls, t, from);
    }

    char line[640];
    if (target == cur) {
        std::snprintf(line, sizeof(line), "profile switch: none, stays %s (posted %s t=%llu from %s)", profileWord(cur), cls, t, from);
        Log::get().note("%s", line);
        return;
    }

    // Teardown, while the outgoing profile's predicates still answer true (design section 3.3). Every switch releases the DLSS and FSR
    // features and closes the menu; the flat and VR parts are the outgoing profile's own.
    if (!g_skipTeardown.load(std::memory_order_relaxed)) {
        if (cur == RuntimeProfile::Flat) {
            flatRuntimeResize();
            engineVelocityFlatAbandon();
        } else if (cur == RuntimeProfile::Vr) {
            uiLayerProfileSwitch();
        }
        dlaaReleaseForSwitch();
        fsr3ReleaseFeatures();
        xessReleaseFeatures();
        menuProfileSwitch();
        g_teardowns.fetch_add(1, std::memory_order_relaxed);
    }
    g_switches.fetch_add(1, std::memory_order_relaxed);

    runtimeProfileSet(target);
    setVrProfileActive(target == RuntimeProfile::Vr || target == RuntimeProfile::LegacyVr);
    Config::get().switchProfile();
    vrSsaaHoldProfileChanged();
    deviceHookRequestConfigRefresh();  // the forced configure sweep runs later in this presentFrameBoundary
    perfMonitorProfileSwitch();        // the monitor's frame interval restarts; the other profile's stretch is no long frame

    std::string now[kPatchedCount];
    readPatched(now);
    std::string kept;
    for (size_t i = 0; i < kPatchedCount; ++i) {
        if (now[i] == g_inEffect[i]) continue;
        if (!kept.empty()) kept += ", ";
        kept += kPatchedKeys[i];
        kept += "=" + g_inEffect[i] + " (new: " + now[i] + ")";
    }
    // A missing file reads as defaults (Config::switchProfile), which is not what the user's settings say: the line says so.
    const bool found = GetFileAttributesW(Config::get().path().c_str()) != INVALID_FILE_ATTRIBUTES;
    std::snprintf(line, sizeof(line), "profile switch: %s -> %s (posted %s t=%llu from %s; settings %s%s; kept until restart: %s)",
                  profileWord(cur), profileWord(target), cls, t, from, Config::get().iniName(), found ? "" : " (not found: defaults)",
                  kept.empty() ? "none" : kept.c_str());
    Log::get().note("%s", line);

    if (target == RuntimeProfile::Vr) {
        setNotice("EDVR switched to its VR fixes");
    } else if (target == RuntimeProfile::Flat) {
        setNotice("EDVR switched to its flat fixes");
    } else if (flatRefused) {
        setNotice("EDVR is paused: its flat fixes are unavailable this session");
    } else {
        char notice[96];
        std::snprintf(notice, sizeof(notice), "EDVR is paused: this install does not include %s", mode == GameMode::Vr ? "vr" : "flat");
        setNotice(notice);
    }
}

}  // namespace

void modeWatchStart() {
    if (runtimeProfileValue() == RuntimeProfile::Invalid) return;
    if (g_startCalled.exchange(true)) return;
    readPatched(g_inEffect);  // the launch's values: what device creation reads, before any switch can change the file's

    const ModeRead launch = runtimeLaunchRead();
    const std::wstring& path = runtimeSettingsPathWide();
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        Log::get().note("mode watch: not started (no Settings.xml path; LOCALAPPDATA is not set)");
        return;
    }
    const std::wstring folder = path.substr(0, slash);
    HANDLE hint = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hint) {
        Log::get().note("mode watch: not started (event error %lu)", GetLastError());
        return;
    }
    g_hintEvent.store(hint, std::memory_order_release);

    char launchMode[16];
    if (launch.known) std::snprintf(launchMode, sizeof(launchMode), "%d", launch.value);
    else std::snprintf(launchMode, sizeof(launchMode), "unknown");
    const GameMode effective = runtimeLaunchEffective();
    Log::get().note("mode watch: started on %S; launch file mode %s (%s); launch mode %s; scope %s", folder.c_str(), launchMode,
                    gameModeName(launch.mode), gameModeName(effective), scopeName(runtimeInstallScope()));

    Worker* worker = new Worker(launch.mode, effective, folder, path, hint);
    const HANDLE thread = CreateThread(nullptr, 0, Worker::threadMain, worker, 0, nullptr);
    if (!thread) {
        Log::get().note("mode watch: worker thread not started (error %lu)", GetLastError());
        return;
    }
    CloseHandle(thread);
    g_started.store(true, std::memory_order_release);
}

void modeWatchHint(ModeHint h) {
    g_hints.fetch_or(static_cast<uint32_t>(h), std::memory_order_acq_rel);
    const HANDLE e = g_hintEvent.load(std::memory_order_acquire);
    if (e) SetEvent(e);
}

void modeWatchFrameBoundary() {
    if (!g_started.load(std::memory_order_acquire)) return;
    const uint64_t posted = g_posted.exchange(0, std::memory_order_acq_rel);
    if (posted) commitPosted(posted);

    // The runtime's live flag, sampled once a frame; a rise asks the worker for a re-read (a VR start may write no file).
    static bool lastLive = false;
    EdvrNativeRenderSizing sizing{};
    // The VR-side query, timed while VR is not active (cross_profile_cost.h).
    cross_profile::Probe probe = cross_profile::enterSite(cross_profile::Site::VrModeWatchFrame, !runtimeVrProfile());
    const bool live = edvrQueryNativeRenderSizing(EDVR_NATIVE_RENDER_SIZING_VERSION_1, sizeof(sizing), &sizing) && sizing.valid == 1;
    probe.stop();
    if (live && !lastLive) modeWatchHint(ModeHint::LiveRise);
    lastLive = live;
}

bool modeWatchNoticeDue(char* out, size_t size) {
    if (!out || size == 0 || !g_noticeDue.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(g_noticeMutex);
    if (!g_noticeDue.load(std::memory_order_acquire)) return false;
    std::snprintf(out, size, "%s", g_notice.c_str());
    g_noticeDue.store(false, std::memory_order_release);
    return true;
}

}  // namespace edvr

// Test-only exports, read by tools\profile_switch_test (build.bat's rig). Production never calls them. The rig's own header says what
// it covers: the switch's mechanics on WARP, not the flat or VR runtimes, which key on Elite's shaders and never engage here.

// Posts a mode exactly as the watch posts one from the file (source "file"); the commit happens at the next frame boundary.
extern "C" void edvr_selftest_profile_post(unsigned int vr) {
    edvr::postMode(edvr::nowMs(), vr ? edvr::GameMode::Vr : edvr::GameMode::Flat, false);
}

// out[0] the active profile (RuntimeProfile), [1] 1 when Config's file is edvr-flat.ini (0: edvr.ini), [2] Config's generation (low 32
// bits), [3] real switches committed, [4] the menu configured, [5] the Supersampling hold held, [6] the flat runtime live, [7] teardowns
// run. Returns how many it wrote.
extern "C" unsigned int edvr_selftest_profile_state(uint32_t* out, unsigned int count) {
    if (!out) return 0;
    const edvr::Config& cfg = edvr::Config::get();
    const uint32_t values[8] = {
        static_cast<uint32_t>(edvr::runtimeProfileValue()),
        std::strcmp(cfg.iniName(), edvr::kIniNameFlat) == 0 ? 1u : 0u,
        cfg.generation(),
        edvr::g_switches.load(std::memory_order_relaxed),
        edvr::menuIsConfigured() ? 1u : 0u,
        edvr::vrSsaaHoldActive() ? 1u : 0u,
        edvr::g_flatRuntimeLive.load(std::memory_order_relaxed) ? 1u : 0u,
        edvr::g_teardowns.load(std::memory_order_relaxed),
    };
    const unsigned int n = std::min(count, 8u);
    for (unsigned int i = 0; i < n; ++i) out[i] = values[i];
    return n;
}

// The control: when on, a commit skips its teardown. The rig's second process sets it; production never does.
extern "C" void edvr_selftest_profile_skip_teardown(unsigned int on) {
    edvr::g_skipTeardown.store(on != 0, std::memory_order_relaxed);
}
