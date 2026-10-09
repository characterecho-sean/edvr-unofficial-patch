#include "perf_monitor.h"
#include "config_refresh_line.h"
#include "engine_motion_cpu.h"
#include "frame_ticks.h"
#include "gpu_frame_timing.h"

#include <windows.h>
#include <algorithm>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <psapi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../common/frame_flag.h"
#include "../common/freeze_book.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/perf_math.h"
#include "../common/slow_test.h"
#include "../common/timing.h"
#include "../common/vtable_hook.h"  // vtableWatchDumpRecent, the flip timeline
#include "device_hook.h"
#include "sharpen_pass.h"
#include "stall_watch.h"
#include "temporal_pass.h"
// fsr3_engine.h is deliberately NOT included: the EDVR PASSES tile reaches
// AMD's price through temporal_pass.h's temporalPassTrainedTotals, which
// answers for the engine fix.temporal_aa names right now (F6).
#include "native_menu.h"
#include "native_timing.h"
#include "native_perf_history.h"
#include "native_benchmark_collector.h"
#include "native_render_labels.h"
#include "../common/native_render_settings.h"
#include "../common/config.h"

namespace edvr {

// The draw clock's arm (perf_monitor.h). Out of State only so the header can
// answer without a call: DrawClock constructs it in all four draw thunks, so
// this is asked about 18k times a frame and says no on fifteen frames in
// sixteen. Set at the frame boundary, from the same expression as before.
namespace detail {
bool g_perfMonitorSampleDraws = false;
}  // namespace detail

namespace {

// Ten seconds at 90 Hz for the statistics; the graph shows the tail.
constexpr int kRing = 900;
// fpsVR prints the MEAN of each frametime over one overlay update, about
// 200 ms, and has since its version 1.24.1 (August 2022). Its pinned Q&A
// still describes the maximum, because that is what it did BEFORE that
// release and the Q&A was never edited -- do not take that page as the
// answer. This is the window the GPU TIME and CPU TIME tiles average over,
// so the two agree; the ten-second mean is on the sub-line, where it is
// the steadier number to tune against.
constexpr float kMatchWindowS = 0.2f;
constexpr uint64_t kSlowEveryMs = 1000;
constexpr uint64_t kGraceMs = 2000;
// One frame in sixteen samples the draw hooks.
constexpr uint32_t kDrawSampleEvery = 16;
// The drop log line: at most one every five seconds, sixty a session.
constexpr uint64_t kDropLogEveryMs = 5000;
constexpr uint32_t kDropLogMax = 60;

struct Frame {
    float    presentMs = 0.0f;   // Present to Present
    // Blocked time on the game's thread this frame, EDVR's own clock: the
    // period less this is the render thread's busy time (CPU TIME).
    float    presentWaitMs = 0.0f;
    uint8_t  native = 0;
    // EDVR's part of the frame.
    uint16_t events = 0;
    float    eventMs = 0.0f;     // the longest event's own duration (a compile, a reload)
    // cpuBoundaryMs is stamped by the frame boundary AFTER this frame's LONG FRAME
    // line is written (perfMonitorNoteCpu, at the end of the Present hook), so the
    // line cannot read it: it reads `ticks` instead, cut at this frame's own edge.
    float    cpuBoundaryMs = 0.0f;
    float    cpuDrawsMs = 0.0f;  // the running sampled figure
    bool     drawsFresh = false; // ...and whether it was measured in THIS frame
    FrameTickSummary ticks;      // EDVR's ticks in the Present hook, by name (frame_ticks.h)
    // Engine motion's CPU time on this thread in THIS frame (engine_motion_cpu.h),
    // every call clocked: exact for the frame, cut at the same edge as `ticks`.
    bool     emMeasured = false;
    float    emMs = 0.0f;
    uint32_t emCalls = 0;
    // The game's own creations in the frame (device_hook.h), for the
    // long-frame line: a busy frame that made a hundred textures was
    // streaming, whatever else it looked like.
    uint32_t createTextures = 0;
    uint32_t createBuffers = 0;
    uint32_t createShaders = 0;
    float    createMb = 0.0f;
};

// Engine motion's CPU instrument (engine_motion_cpu.h): cut once a frame at the
// Present hook's edge, folded and logged every 30 s. Static: its window holds a
// few hundred KB of per-frame samples.
emcpu::Recorder g_engineMotion;

// The 30 s report: four lines, each short of the log line's limit
// (tools\engine_motion_cpu_test holds the worst case): the scheme and the totals,
// the clock's floor and what the instrument costs, the render thread's parts
// (every call clocked), the other threads' parts (the sampled frames only).
void logEngineMotion(const emcpu::WindowReport& r) {
    char text[1400];
    emcpu::formatSummary(text, sizeof(text), r);
    Log::get().note("%s", text);
    emcpu::formatClock(text, sizeof(text), r);
    Log::get().note("%s", text);
    emcpu::formatRenderParts(text, sizeof(text), r);
    Log::get().note("%s", text);
    emcpu::formatOtherParts(text, sizeof(text), r);
    Log::get().note("%s", text);
}

// ---- NvAPI, the two entry points the page wants ---------------------------
typedef void* (*PFN_NvQueryInterface)(uint32_t id);
typedef int (*PFN_NvInitialize)();
typedef int (*PFN_NvEnumGpus)(void** handles, uint32_t* count);
typedef int (*PFN_NvDynamicPstates)(void* gpu, void* info);
typedef int (*PFN_NvThermal)(void* gpu, uint32_t target, void* settings);
constexpr uint32_t kIdInitialize = 0x0150E828;
constexpr uint32_t kIdEnumPhysicalGpus = 0xE5AC921F;
constexpr uint32_t kIdDynamicPstates = 0x60DED2ED;
constexpr uint32_t kIdThermalSettings = 0xE3640A56;

// NV_GPU_DYNAMIC_PSTATES_INFO_EX, version 1: utilization[0] is the GPU.
struct NvDynamicPstates {
    uint32_t version;
    uint32_t flags;
    struct {
        uint32_t present;   // bit 0
        uint32_t percentage;
    } utilization[8];
};
// NV_GPU_THERMAL_SETTINGS, version 2.
struct NvThermalSettings {
    uint32_t version;
    uint32_t count;
    struct {
        uint32_t controller;
        int32_t  defaultMin;
        int32_t  defaultMax;
        int32_t  current;
        uint32_t target;
    } sensor[3];
};
constexpr uint32_t kThermalTargetAll = 15;

struct State {
    NativePerfHistory nativeHistory;
    NativeBenchmarkCollector nativeBenchmark;
    NativeBenchmarkMetadata nativeBenchmarkMetadata{};
    uint64_t nativeBenchmarkMetadataMs = 0;
    uint64_t nativeBenchmarkGeneration = 0, nativeBenchmarkFirstSequence = 0;
    uint64_t nativeBenchmarkScope = 0;
    std::atomic<uint64_t> nativeBenchmarkSettingsEpoch{0};
    uint64_t nativeBenchmarkCpuCursor = 0;
    uint64_t nativeBenchmarkGpuCursor = 0;
    uint64_t nativeHistoryLogMs=0;
    unsigned nativeHistoryReports=0;
    Frame    ring[kRing];
    int      head = 0;          // next write
    int      count = 0;
    int64_t  lastQpc = 0;
    uint32_t frameNo = 0;

    // The frame in progress: events and their longest duration, the draw
    // sample, all cleared at the boundary.
    std::atomic<uint32_t> events{0};
    std::atomic<int32_t>  eventUs{0};
    int64_t  drawWholeTicks = 0;
    int64_t  drawRealTicks = 0;
    float    drawsMsRunning = 0.0f;
    bool     drawsSampled = false;
    double   drawWindowMs = 0.0;
    float    drawWindowMaxMs = 0.0f;
    uint32_t drawWindowSamples = 0;
    // The config refresh window (the line beside the draw hook's, every 1800 frames): what kEvReload (edvr.ini re-read and every module reconfigured, on the frame
    // thread, its duration the whole of it) and kEvIniWrite (a menu edit queued for the file) were noted since the last line. A held numeric row in the F8 menu is
    // one edit and, once the write lands, one refresh about every 83 ms; this prices it.
    std::atomic<uint32_t> cfgReloads{0}, cfgEdits{0}, cfgReloadMaxUs{0};
    std::atomic<uint64_t> cfgReloadUs{0};

    // The Present block noted by the swapchain hook, for the frame about
    // to be ringed.
    float    pendingPresentWaitMs = 0.0f;

    // The long-frame book (freeze_book.h): every Present gap over twice the period that reached the judge,
    // what it turned out to be, what was written, and the worst few. Written on the render thread, read at
    // the end of the session from whichever thread closes the runtime's timing context, so under freezeLock.
    FreezeBook freeze;
    std::atomic<uint32_t> freezeLock{0};
    uint64_t freezeCountsMs = 0;          // the last periodic counts line (0 until the first frame arms it)
    uint32_t freezeWorstPrinted = 0;      // the worst list's revision as last printed
    // The end-of-session lines as last written: the book's candidate count and worst revision then, so the
    // runtime's close and the DLL's own shutdown do not write the same set twice.
    bool     freezeFinalWritten = false;
    uint64_t freezeFinalCandidates = 0;
    uint32_t freezeFinalRevision = 0;
    // advanced.freeze_test_ms (freezeTestTick): read at the first frame, fired once.
    bool     freezeTestRead = false;
    bool     freezeTestDone = false;
    int      freezeTestMs = 0;
    uint64_t freezeTestArmedMs = 0;
    // advanced.slow_test_ms (slowTestTick): read at the first frame; the hold runs 40 s from 90 s in (slow_test.h).
    bool     slowTestRead = false;
    SlowTestSchedule slowTest;

    // The drop log's rate limit, and the last drop for the page.
    uint64_t dropLogMs = 0;
    uint32_t dropLogged = 0;
    uint64_t lastDropMs = 0;
    float    lastDropFrameMs = 0.0f;
    uint16_t lastDropEvents = 0;
    float    lastDropEventMs = 0.0f;

    bool     active = false;
    uint64_t activeUntilMs = 0;
    uint64_t slowMs = 0;

    // Slow-sampled values.
    float    cpuSystemPct = -1.0f;
    float    cpuProcessPct = -1.0f;
    uint32_t cpuThreads = 0;
    uint64_t lastIdle = 0, lastKernel = 0, lastUser = 0;
    uint64_t lastProcKernel = 0, lastProcUser = 0, lastProcWall = 0;
    uint64_t ramTotal = 0, ramAvail = 0, ramProcess = 0;
    uint64_t vramUsed = 0, vramBudget = 0;
    bool     vramTried = false;
    IDXGIAdapter3* adapter3 = nullptr;
    int      gpuLoadPct = -1;
    int      gpuTempC = -1000;
    // NvAPI
    bool     nvTried = false;
    bool     nvOk = false;
    void*    nvGpu = nullptr;
    PFN_NvDynamicPstates nvPstates = nullptr;
    PFN_NvThermal nvThermal = nullptr;
    char     nvWhy[96] = {};
};
State g_s;

FaultBudget g_budget("perfMonitor", 4);

void ringPush(const Frame& f) {
    g_s.ring[g_s.head] = f;
    g_s.head = (g_s.head + 1) % kRing;
    if (g_s.count < kRing) ++g_s.count;
}

Frame& ringAt(int i) {   // 0 = oldest
    const int start = (g_s.head - g_s.count + kRing) % kRing;
    return g_s.ring[(start + i) % kRing];
}

Frame* ringLast() {
    if (!g_s.count) return nullptr;
    return &g_s.ring[(g_s.head - 1 + kRing) % kRing];
}

const char* eventName(uint32_t bit) {
    switch (bit) {
        case kEvReload: return "reload";
        case kEvIniWrite: return "ini write";
        case kEvCompile: return "shader compile";
        case kEvWithhold: return "withhold";
        case kEvResubmit: return "resubmit";
        case kEvCensus: return "census";
        case kEvRaster: return "raster upload";
        case kEvBinds: return "binds re-read";
        case kEvNgx: return "DLSS feature";
        case kEvMenu: return "menu open/close";
        case kEvFsr: return "FSR context";
        case kEvEyeDump: return "eye dump";
        default: return "?";
    }
}

// "reload (12 ms), shader compile (142 ms)" -- the duration goes on the
// events that have one, which is the longest of them.
void eventList(uint16_t events, float ms, char* buf, size_t n) {
    buf[0] = 0;
    if (!events) {
        snprintf(buf, n, "none");
        return;
    }
    size_t len = 0;
    for (uint32_t bit = 1; bit <= kEvEyeDump && len + 1 < n; bit <<= 1) {
        if (!(events & bit)) continue;
        const bool timed =
            ms > 0.0f && (bit == kEvCompile || bit == kEvReload || bit == kEvNgx || bit == kEvFsr);
        const int w = timed ? snprintf(buf + len, n - len, "%s%s (%.0f ms)", len ? ", " : "", eventName(bit),
                                       static_cast<double>(ms))
                            : snprintf(buf + len, n - len, "%s%s", len ? ", " : "", eventName(bit));
        if (w <= 0) break;
        len += static_cast<size_t>(w) < n - len ? static_cast<size_t>(w) : n - len - 1;
    }
    buf[n - 1] = 0;
}

void nvArm() {
    State& s = g_s;
    if (s.nvTried) return;
    s.nvTried = true;
    HMODULE lib = LoadLibraryW(L"nvapi64.dll");
    if (!lib) {
        snprintf(s.nvWhy, sizeof(s.nvWhy), "n/a (no NVIDIA driver)");
        return;
    }
    PFN_NvQueryInterface query =
        reinterpret_cast<PFN_NvQueryInterface>(GetProcAddress(lib, "nvapi_QueryInterface"));
    if (!query) {
        snprintf(s.nvWhy, sizeof(s.nvWhy), "n/a (nvapi64.dll has no QueryInterface)");
        return;
    }
    bool ok = false;
    guardedBudget(g_budget, [&] {
        PFN_NvInitialize init = reinterpret_cast<PFN_NvInitialize>(query(kIdInitialize));
        PFN_NvEnumGpus enumGpus = reinterpret_cast<PFN_NvEnumGpus>(query(kIdEnumPhysicalGpus));
        s.nvPstates = reinterpret_cast<PFN_NvDynamicPstates>(query(kIdDynamicPstates));
        s.nvThermal = reinterpret_cast<PFN_NvThermal>(query(kIdThermalSettings));
        if (!init || !enumGpus || !s.nvPstates) return;
        if (init() != 0) return;
        void* handles[64] = {};
        uint32_t n = 0;
        if (enumGpus(handles, &n) != 0 || n == 0) return;
        s.nvGpu = handles[0];
        ok = true;
    });
    s.nvOk = ok;
    if (!ok) snprintf(s.nvWhy, sizeof(s.nvWhy), "n/a (NvAPI refused)");
}

void slowSample(ID3D11Device* dev) {
    State& s = g_s;
    // CPU, system-wide and this process.
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        auto u64 = [](const FILETIME& f) {
            return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
        };
        const uint64_t i = u64(idle), k = u64(kernel), u = u64(user);
        if (s.lastKernel) {
            const uint64_t total = (k - s.lastKernel) + (u - s.lastUser);
            const uint64_t busy = total > (i - s.lastIdle) ? total - (i - s.lastIdle) : 0;
            if (total) s.cpuSystemPct = 100.0f * static_cast<float>(busy) / static_cast<float>(total);
        }
        s.lastIdle = i;
        s.lastKernel = k;
        s.lastUser = u;
        FILETIME pc{}, pe{}, pk{}, pu{};
        if (GetProcessTimes(GetCurrentProcess(), &pc, &pe, &pk, &pu)) {
            if (!s.cpuThreads) {
                SYSTEM_INFO si{};
                GetSystemInfo(&si);
                s.cpuThreads = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
            }
            const uint64_t wall = k + u;   // system time advances on every core
            const uint64_t proc = u64(pk) + u64(pu);
            if (s.lastProcWall && wall > s.lastProcWall) {
                s.cpuProcessPct = 100.0f * static_cast<float>(proc - (s.lastProcKernel + s.lastProcUser)) /
                                  static_cast<float>(wall - s.lastProcWall);
            }
            s.lastProcWall = wall;
            s.lastProcKernel = u64(pk);
            s.lastProcUser = u64(pu);
        }
    }
    // RAM.
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.ramTotal = ms.ullTotalPhys;
        s.ramAvail = ms.ullAvailPhys;
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        s.ramProcess = pmc.WorkingSetSize;
    }
    // VRAM, through the adapter the device sits on.
    if (!s.vramTried && dev) {
        s.vramTried = true;
        IDXGIDevice* dxgiDev = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) &&
            dxgiDev) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter) {
                adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&s.adapter3));
                adapter->Release();
            }
            dxgiDev->Release();
        }
    }
    if (s.adapter3) {
        DXGI_QUERY_VIDEO_MEMORY_INFO vmi{};
        if (SUCCEEDED(s.adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmi))) {
            s.vramUsed = vmi.CurrentUsage;
            s.vramBudget = vmi.Budget;
        }
    }
    // GPU load and temperature, NVIDIA only.
    nvArm();
    if (s.nvOk && s.nvGpu) {
        guardedBudget(g_budget, [&] {
            NvDynamicPstates p{};
            p.version = sizeof(p) | (1u << 16);
            if (s.nvPstates(s.nvGpu, &p) == 0 && (p.utilization[0].present & 1)) {
                s.gpuLoadPct = static_cast<int>(p.utilization[0].percentage);
            }
            if (s.nvThermal) {
                NvThermalSettings t{};
                t.version = sizeof(t) | (2u << 16);
                if (s.nvThermal(s.nvGpu, kThermalTargetAll, &t) == 0 && t.count > 0) {
                    s.gpuTempC = t.sensor[0].current;
                }
            }
        });
    }
}

void gb(char* buf, size_t n, uint64_t bytes) {
    snprintf(buf, n, "%.1f", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
}

// Whether the rate limit lets another ordinary long frame have its line. A frame of kFreezeAlwaysLogMs or
// more never asks (judgeLongFrame): the limiter is for a scene that drops a frame every few seconds.
bool dropLineAllowed() {
    return g_s.dropLogged < kDropLogMax && dueMs(g_s.dropLogMs, kDropLogEveryMs);
}

// The LONG FRAME line. `gapMs` is the frame's own length (f.presentMs is 0 for a frame of 5 s or more, which
// the ring keeps out of its statistics but the log has to say). `limited` is true for a line the rate limit
// let through, which is then charged to it; a freeze's line is not limited and not charged.
void dropLine(const Frame& f, float budgetMs, double gapMs, bool limited) {
    State& s = g_s;
    if (limited) {
        s.dropLogMs = stampMs();
        ++s.dropLogged;
    }
    char ev[200];
    eventList(f.events, f.eventMs, ev, sizeof(ev));
    const float busy = static_cast<float>(gapMs) - f.presentWaitMs;
    // WHICH FRAME THIS IS, in the ONE numbering the flip timeline stamps its
    // events with -- and until now neither side printed a frame number at all,
    // so the question issue #21 turns on (did the context's table change before
    // the hang or after it?) could not be answered from the log even when both
    // instruments were running.
    //
    // vtableWatchFrame() is the frame IN PROGRESS, published by device_hook's
    // frame path the instant this Present returned. Frame N is everything
    // between Present N-1 returning and Present N returning, so the frame THIS
    // LINE IS ABOUT is the one that just ended: one less. A flip stamped with
    // that number happened inside the long frame; one stamped lower did not.
    const uint64_t inProgress = vtableWatchFrame();
    const double   sinceArm = vtableWatchSecondsSinceArm();
    char stamp[220];
    if (sinceArm > 0.0) {
        snprintf(stamp, sizeof(stamp),
                 " This is frame %llu (the frame now in progress is %llu), "
                 "%.4f s after the flip timeline armed.",
                 static_cast<unsigned long long>(inProgress ? inProgress - 1 : 0),
                 static_cast<unsigned long long>(inProgress), sinceArm);
    } else {
        snprintf(stamp, sizeof(stamp),
                 " This is frame %llu; the flip timeline is not armed, so there "
                 "are no table changes to order against it.",
                 static_cast<unsigned long long>(inProgress ? inProgress - 1 : 0));
    }
    if (f.native) {
        char reference[80];
        if (budgetMs > 0.0f) snprintf(reference, sizeof(reference), "runtime predicted period %.1f ms", double(budgetMs));
        else snprintf(reference, sizeof(reference), "runtime predicted period unavailable");
        // The runtime's timing sequence and the game's work in its latest
        // cycle, from the EdvrNativeTimingFrame this half already receives:
        // "runtime sequence N" matches the OpenXR half's native_long_cycle
        // sequence=N, to within a frame.
        const NativeTimingSnapshot timing = nativeTimingSnapshot();
        char callerWork[48];
        if (timing.cpu.callerWorkValid) snprintf(callerWork, sizeof(callerWork), "%.2f ms", double(timing.cpu.callerWorkMs));
        else snprintf(callerWork, sizeof(callerWork), "unavailable");
        // EDVR's share of THIS frame, from the tick chain cut at its edge. The line
        // used to carry no share at all, and the boundary figure it might have
        // carried (cpuBoundaryMs) is written after the line is, so it read 0.00.
        char share[400];
        EngineMotionFrame motion;
        motion.measured = f.emMeasured;
        motion.renderMs = static_cast<double>(f.emMs);
        motion.calls = f.emCalls;
        formatEdvrShare(share, sizeof(share), gapMs, f.ticks,
                        static_cast<double>(f.cpuDrawsMs), f.drawsFresh, motion);
        NativeLongFrame line;
        line.frameMs = gapMs;
        line.reference = reference;
        line.textures = f.createTextures;
        line.buffers = f.createBuffers;
        line.shaders = f.createShaders;
        line.creationMb = static_cast<double>(f.createMb);
        line.share = share;
        line.events = ev;
        line.stamp = stamp;
        line.sequence = static_cast<unsigned long long>(timing.cpu.sequence);
        line.gameWork = callerWork;
        char text[1400];
        formatNativeLongFrame(text, sizeof(text), line);
        Log::get().note("%s", text);
        return;
    }
    char share[400];
    EngineMotionFrame motion;
    motion.measured = f.emMeasured;
    motion.renderMs = static_cast<double>(f.emMs);
    motion.calls = f.emCalls;
    formatEdvrShare(share, sizeof(share), gapMs, f.ticks,
                    static_cast<double>(f.cpuDrawsMs), f.drawsFresh, motion);
    Log::get().note(
        "monitor: LONG FRAME -- %.1f ms between Presents (budget %.1f), of which the thread waited "
        "%.1f in Present (busy %.1f); the game's creations in it: %u "
        "textures, %u buffers (%.1f MB together), %u shaders; %s "
        "EDVR events: %s.%s At most "
        "one of these lines every %u s, %u a session, except every frame over %.0f ms.",
        gapMs,
        static_cast<double>(budgetMs), static_cast<double>(f.presentWaitMs),
        static_cast<double>(busy > 0.0f ? busy : 0.0f),
        f.createTextures, f.createBuffers, static_cast<double>(f.createMb), f.createShaders,
        share, ev, stamp,
        static_cast<unsigned>(kDropLogEveryMs / 1000), kDropLogMax, kFreezeAlwaysLogMs);
}

// The page's "last drop": every Present gap over twice the period, the one-frame blips included -- to the
// person wearing the headset a blip is a missed frame, and this is the line they read. Unchanged by the
// freeze work except that it no longer shares a function with the log's decisions.
void pageDrop(const Frame& f) {
    State& s = g_s;
    s.lastDropMs = nowMs();
    s.lastDropFrameMs = f.presentMs;
    s.lastDropEvents = f.events;
    s.lastDropEventMs = f.eventMs;
}

// What the runtime's cycle says about the Present gap that just ended (nativeTimingWaitReturns).
//
// The runtime logs a native_long_cycle line for a cycle -- one pose-wait return to the next -- of more than
// twice its predicted period. A Present gap is a different window over the same game: it runs from one
// Present to the next, so it holds the tail of one cycle and the head of the next, and a game that Presents
// early in one cycle and late in the next stretches the gap by up to the whole cycle without any cycle being
// long. In the issue 63 flight that was 44 of the 59 lines the graphics log spent its cap on.
//
// A gap touches two cycles, and only the first is complete when its Present returns: the previous cycle
// whole (prevMs, wait return to wait return) and the current one so far (headMs, the last wait return to
// now, which is a floor of the cycle the runtime will log, because the tail can only add to it). The longer
// of the two is the figure the gap is judged by, against the same twice-the-period the runtime uses.
// `known` is false off the native path and before two waits have returned; the judge then trusts the gap.
struct CycleView {
    bool known = false;
    uint64_t sequence = 0;
    double prevMs = 0.0;
    double headMs = 0.0;
    double longestMs() const { return prevMs > headMs ? prevMs : headMs; }
};

CycleView cycleViewAt(int64_t qpc, bool native) {
    CycleView v;
    if (!native || qpcFrequency() <= 0) return v;
    const NativeWaitReturns w = nativeTimingWaitReturns();
    if (!w.valid || w.returnQpc > qpc) return v;
    const double perTick = 1000.0 / static_cast<double>(qpcFrequency());
    v.known = true;
    v.sequence = w.sequence;
    v.prevMs = static_cast<double>(w.returnQpc - w.previousQpc) * perTick;
    v.headMs = static_cast<double>(qpc - w.returnQpc) * perTick;
    return v;
}

// The graphics log's own clock (Log::note stamps local time, hours to milliseconds), for the worst list.
void localStamp(char* out, size_t n) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    snprintf(out, n, "%02u:%02u:%02u.%03u", static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
             static_cast<unsigned>(st.wSecond), static_cast<unsigned>(st.wMilliseconds));
}

// A short spin lock around the book (freeze_book.h): the render thread writes it only when a frame is a
// candidate, and the end-of-session lines read it from whichever thread closes the runtime session.
struct FreezeLock {
    explicit FreezeLock(std::atomic<uint32_t>& flag) : flag_(flag) {
        while (flag_.exchange(1, std::memory_order_acquire) != 0) YieldProcessor();
    }
    ~FreezeLock() { flag_.store(0, std::memory_order_release); }
    FreezeLock(const FreezeLock&) = delete;
    FreezeLock& operator=(const FreezeLock&) = delete;
private:
    std::atomic<uint32_t>& flag_;
};

// The FREEZE line: one for every frame of kFreezeAlwaysLogMs or more, written after its LONG FRAME line, on
// the clock of the cycle the runtime logs a native_long_cycle line for. The LONG FRAME line itself cannot
// carry this -- at its worst it is 1154 characters of the 1160 the log keeps (native_perf_history_test) --
// and what this adds is what the person reading a freeze asks first: how long was the runtime's own cycle,
// and where in it did the time go.
void freezeLine(double gapMs, const CycleView& cv, uint64_t frame, uint64_t freezeNo, const char* sampler) {
    char cycle[200];
    if (cv.known) {
        snprintf(cycle, sizeof(cycle),
                 "%.1f ms (%.1f ms from the pose wait's return to this Present, the previous cycle %.1f ms)",
                 cv.longestMs(), cv.headMs, cv.prevMs);
    } else {
        snprintf(cycle, sizeof(cycle),
                 "unavailable (no two pose-wait returns: not the native path, or no open runtime session)");
    }
    Log::get().note(
        "monitor: FREEZE -- %.1f ms between Presents, ended now: frame %llu, runtime sequence %llu; runtime "
        "cycle %s; freeze %llu of this session; stall sampler %s. A frame of %.0f ms or more always gets this "
        "line and a LONG FRAME line, with no cap and no rate limit.",
        gapMs, static_cast<unsigned long long>(frame), static_cast<unsigned long long>(cv.sequence), cycle,
        static_cast<unsigned long long>(freezeNo), sampler, kFreezeAlwaysLogMs);
}

// What the stall sampler (stall_watch.h) found out about the stall that this freeze ends: written into `state`
// for the FREEZE line, and into `note` for the worst list ("" when it took no sample). The stall began after the
// previous Present, which is this frame's edge less its length, and the sampler's beat for it was taken a few
// hundred microseconds from that edge, so the match is made within 100 ms.
void samplerFor(int64_t qpc, double gapMs, char* state, size_t stateSize, char* note, size_t noteSize) {
    note[0] = 0;
    switch (stallWatchState()) {
        case StallWatchState::Off:
            snprintf(state, stateSize, "off (advanced.freeze_location = off)");
            return;
        case StallWatchState::NotStarted:
        case StallWatchState::Failed:
            snprintf(state, stateSize, "not running (the watchdog thread never started)");
            return;
        case StallWatchState::Running:
            break;
    }
    StallEpisode ep;
    const int64_t freq = qpcFrequency();
    const int64_t began = freq > 0 ? qpc - static_cast<int64_t>(gapMs * static_cast<double>(freq) / 1000.0) : 0;
    if (began > 0 && stallWatchEpisodeAround(began, 100.0, &ep)) {
        snprintf(state, stateSize, "took %u sample%s, the last in %s", ep.samples, ep.samples == 1 ? "" : "s", ep.owner);
        snprintf(note, noteSize, "stalled in %s (%u sample%s, longest at %u ms%s)", ep.owner, ep.samples,
                 ep.samples == 1 ? "" : "s", ep.maxAgeMs, ep.edvrOnStack ? ", EDVR code on the stack" : "");
    } else {
        snprintf(state, stateSize, "took no sample (the rate limit, a failed suspend, or the stall began before the sampler was armed)");
    }
}

// Decide what a Present gap over twice the period is, and write what it earns.
//
//   blip      the runtime's cycle around it was not long: counted by size, not written, no hang dump.
//   long      the cycle confirms it (or cannot be read): the hang dump, and a LONG FRAME line if the
//             rate limit allows; counted by size either way, so a line the limiter kept out is a number.
//   freeze    a gap of kFreezeAlwaysLogMs or more: long, and the line is not up to the limiter.
//
// `qpc` is the frame's edge (the clock reading the gap was cut on).
void judgeLongFrame(const Frame& f, float budgetMs, double gapMs, int64_t qpc) {
    State& s = g_s;
    if (f.presentMs > 0.0f && f.presentMs < 5000.0f) pageDrop(f);
    const CycleView cv = cycleViewAt(qpc, f.native != 0);
    const FreezeOutcome outcome =
        freezeDecide(gapMs, static_cast<double>(budgetMs), cv.known, cv.longestMs(), dropLineAllowed());
    if (outcome.verdict == FreezeVerdict::NotLong) return;
    if (outcome.verdict == FreezeVerdict::Blip) {
        FreezeLock lock(s.freezeLock);
        s.freeze.record(outcome, gapMs);
        return;
    }
    const bool freeze = outcome.verdict == FreezeVerdict::Freeze;
    const bool write = outcome.write;
    const uint64_t inProgressNow = vtableWatchFrame();
    const uint64_t frameNo = inProgressNow ? inProgressNow - 1 : 0;
    // The stall sampler can only have taken a sample of a stall that reached its first threshold (150 ms).
    char samplerState[200] = {};
    char samplerNote[120] = {};
    if (gapMs >= 150.0) samplerFor(qpc, gapMs, samplerState, sizeof(samplerState), samplerNote, sizeof(samplerNote));
    uint64_t freezeNo = 0;
    {
        FreezeLock lock(s.freezeLock);
        s.freeze.record(outcome, gapMs);
        FreezeWorst w;
        w.ms = gapMs;
        w.sequence = cv.sequence;
        w.frame = frameNo;
        w.cycleMs = cv.known ? cv.longestMs() : 0.0;
        localStamp(w.stamp, sizeof(w.stamp));
        freezeCopyText(w.note, sizeof(w.note), samplerNote);
        s.freeze.offerWorst(w);
        freezeNo = s.freeze.freezes;
    }
    // ABOVE dropLine, and OUTSIDE its rate limit, which is the whole point.
    //
    // A frame that took far too long is the shape a GPU hang makes on its way
    // out, and issue #21's question is whether the context's dispatch table
    // changed BEFORE that or after. The dump used to sit at the bottom of
    // dropLine, under a five-second-and-forty-lines-a-session gate meant for
    // the LOG's volume -- so a shader compile in the previous five seconds
    // (there are always several) suppressed the dump on the fatal frame, which
    // is the only frame it exists for. It has its own cap of sixteen a session
    // and writes the breadcrumb file, so it does not need that one.
    //
    // THE FRAME IT IS ABOUT IS THE ONE THAT JUST ENDED. This runs inside the
    // post-Present block for frame N, and the long frame it is reporting is
    // N-1; the flips recorded inside that frame carry N-1 too. The dump used to
    // print the frame in PROGRESS and tell the reader that a change stamped with
    // it had preceded the hang -- so the change that DID precede the hang, at
    // N-1, read as one that had not.
    //
    // Blips no longer reach it: it has a cap of sixteen a session too, and a one-frame blip is not the
    // shape of a hang.
    vtableWatchDumpRecent("monitor: LONG FRAME", frameNo);
    if (write) dropLine(f, budgetMs, gapMs, !freeze);
    if (freeze) freezeLine(gapMs, cv, frameNo, freezeNo, samplerState);
}

// The long-frame counts, and the worst few, as log lines (freeze_book.h).
//
//   periodic   every kFreezeCountsEveryMs from the frame boundary: the counts always (a quiet session's
//              zeros are the proof that the instrument ran), the worst list only when it changed since the
//              last time it was printed, so a quiet session repeats nothing;
//   final      the end of the session: the counts, and the whole worst list. Written from the one place the
//              graphics half can know the session is over -- the runtime closing its timing context
//              (perfMonitorSessionEnd), or the DLL's own FreeLibrary teardown (perfMonitorShutdown). It is
//              NOT written at process exit: DllMain's DLL_PROCESS_DETACH runs with the other threads already
//              dead and the loader lock held, and the log itself is only detached there
//              (d3d11_proxy.cpp). That is why the periodic lines exist.
void writeFreezeSummary(const char* reason, bool final) {
    State& s = g_s;
    FreezeBook copy;
    uint32_t worstPrinted = 0;
    {
        FreezeLock lock(s.freezeLock);
        if (final) {
            // One set per change: the runtime closing and the DLL shutting down both ask, and a session
            // reopened after a close asks again with new counts.
            if (s.freezeFinalCandidates == s.freeze.candidates && s.freezeFinalRevision == s.freeze.worstRevision &&
                s.freezeFinalWritten)
                return;
            s.freezeFinalWritten = true;
            s.freezeFinalCandidates = s.freeze.candidates;
            s.freezeFinalRevision = s.freeze.worstRevision;
        }
        copy = s.freeze;
        worstPrinted = s.freezeWorstPrinted;
        s.freezeWorstPrinted = copy.worstRevision;
    }
    char counts[700];
    copy.formatCounts(counts, sizeof(counts), ' ', true);
    Log::get().note(
        "monitor: long frame counts reason=%s %s; candidates are Present gaps over twice the runtime's "
        "predicted period; a candidate the runtime's cycle did not confirm is a blip, counted and not "
        "written; the others are long, written one every %u s up to %u a session; every frame of %.0f ms or "
        "more is written whatever the limit says, and over_250ms_unwritten counts any that was not.",
        reason, counts, static_cast<unsigned>(kDropLogEveryMs / 1000), kDropLogMax, kFreezeAlwaysLogMs);
    // The stall sampler's own counts, beside the long-frame counts they explain (silent when it is off).
    stallWatchWriteCounts(reason);
    if (!copy.worstCount() || (!final && copy.worstRevision == worstPrinted)) return;
    for (unsigned i = 0; i < copy.worstCount(); ++i) {
        const FreezeWorst& w = copy.worstAt(i);
        char cycle[40];
        if (w.cycleMs > 0.0) snprintf(cycle, sizeof(cycle), "%.1f ms", w.cycleMs);
        else snprintf(cycle, sizeof(cycle), "unavailable");
        Log::get().note(
            "monitor: worst long frame %u of %u: %.1f ms between Presents, ended %s (frame %llu, runtime "
            "sequence %llu, runtime cycle %s)%s%s",
            i + 1, copy.worstCount(), w.ms, w.stamp, static_cast<unsigned long long>(w.frame),
            static_cast<unsigned long long>(w.sequence), cycle, w.note[0] ? "; " : "", w.note);
    }
}

// A TEST-ONLY TRIGGER, advanced.freeze_test_ms (0 = off, the default): sixty seconds into the session the render
// thread sleeps that many milliseconds, once, so a flight can show the whole chain -- the LONG FRAME and FREEZE
// lines, the stall sampler's lines naming where the thread was (here: an ntdll sleep, called from this DLL, so
// "EDVR code on the stack: yes" is the expected answer), the counts and the worst list -- without waiting for a
// real freeze. It stops the game on purpose; it is not a feature, and a value above 0 is logged when it is read.
void freezeTestTick() {
    State& s = g_s;
    if (s.freezeTestDone) return;
    if (!s.freezeTestRead) {
        s.freezeTestRead = true;
        s.freezeTestMs = Config::get().getIntInRange("advanced.freeze_test_ms", 0, 0, 5000);
        s.freezeTestArmedMs = stampMs();
        if (s.freezeTestMs > 0) {
            Log::get().note("freeze test: advanced.freeze_test_ms = %d. Sixty seconds from now the render thread "
                            "sleeps %d ms, once, to test the freeze lines and the stall sampler. Set it back to 0.",
                            s.freezeTestMs, s.freezeTestMs);
        } else {
            s.freezeTestDone = true;
        }
        return;
    }
    if (!elapsedMs(s.freezeTestArmedMs, 60000)) return;
    s.freezeTestDone = true;
    Log::get().note("freeze test: the render thread sleeps %d ms now.", s.freezeTestMs);
    Sleep(static_cast<DWORD>(s.freezeTestMs));
}

// A TEST-ONLY TRIGGER, advanced.slow_test_ms (0 = off, the default): ninety seconds into the session the runtime is
// asked (frame_flag's requestEndFrameHold) to hold every xrEndFrame it makes that many milliseconds longer, inside the
// call's timed region, and forty seconds later the request is withdrawn. From outside that is a vendor runtime that
// stalls in xrEndFrame: the runtime's log shows native_end_frame_episode lines (the first call of the stall, then the
// episode's end) and the slow regime's SLOW, still_slow and end lines naming the vendor's xrEndFrame as the owner
// (slow_regime.h), and `edvr_log.py --freezes` reads them. It slows the game on purpose; it is not a feature, and a
// value above 0 is logged when it is read. The schedule is slow_test.h's; the runtime half only obeys the request.
void slowTestTick() {
    State& s = g_s;
    if (s.slowTest.done()) return;
    if (!s.slowTestRead) {
        s.slowTestRead = true;
        // The bounds are literals: gen_settings_schema reads them for the menu (slow_test.h's kSlowTestMaxMs is the same 500, pinned by tools\slow_regime_test).
        const int ms = Config::get().getIntInRange("advanced.slow_test_ms", 0, 0, 500);
        s.slowTest.arm(stampMs(), ms);
        if (s.slowTest.on()) {
            Log::get().note("slow test: advanced.slow_test_ms = %d. %llu s from now the runtime holds every xrEndFrame it makes %d ms longer, "
                            "for %llu s, to test the end-frame episode and slow-regime lines. Set it back to 0.",
                            ms, static_cast<unsigned long long>(kSlowTestStartMs / 1000), ms, static_cast<unsigned long long>(kSlowTestForMs / 1000));
        }
        return;
    }
    switch (s.slowTest.tick(nowMs())) {
        case SlowTestSchedule::Step::Began:
            requestEndFrameHold(s.slowTest.holdNowMs());
            Log::get().note("slow test: the hold begins now: every xrEndFrame is held %u ms longer for %llu s.", s.slowTest.holdNowMs(),
                            static_cast<unsigned long long>(kSlowTestForMs / 1000));
            break;
        case SlowTestSchedule::Step::Ended:
            requestEndFrameHold(0);
            Log::get().note("slow test: the hold ended. The runtime's frames are as they were.");
            break;
        case SlowTestSchedule::Step::None:
            break;
    }
}

float budgetNow() {
    const State& s = g_s;
    if (nativeMenuActive()) return static_cast<float>(s.nativeHistory.predictedPeriod(GetTickCount64()));
    // Off the native path nothing reports the display's rate (it crossed
    // from the retired openvr half), so the budget is the unknown-rate one.
    return 11.1f;
}

#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unknown"
#endif

uint64_t benchmarkHashBytes(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t benchmarkScope(const NativeTimingSnapshot& timing,
                        const NativeBenchmarkMetadata& metadata,
                        uint64_t settingsEpoch) {
    uint64_t hash = 1469598103934665603ull;
    hash = benchmarkHashBytes(hash, &timing.generation, sizeof(timing.generation));
    hash = benchmarkHashBytes(hash, &timing.firstSequence, sizeof(timing.firstSequence));
    hash = benchmarkHashBytes(hash, metadata.inputWidth, sizeof(metadata.inputWidth));
    hash = benchmarkHashBytes(hash, metadata.inputHeight, sizeof(metadata.inputHeight));
    hash = benchmarkHashBytes(hash, metadata.outputWidth, sizeof(metadata.outputWidth));
    hash = benchmarkHashBytes(hash, metadata.outputHeight, sizeof(metadata.outputHeight));
    hash = benchmarkHashBytes(hash, &metadata.refreshMilliHz, sizeof(metadata.refreshMilliHz));
    hash = benchmarkHashBytes(hash, metadata.gameFov, sizeof(metadata.gameFov));
    hash = benchmarkHashBytes(hash, metadata.treatments, sizeof(metadata.treatments));
    hash = benchmarkHashBytes(hash, &metadata.featureEpoch, sizeof(metadata.featureEpoch));
    hash = benchmarkHashBytes(hash, metadata.runtime, sizeof(metadata.runtime));
    hash = benchmarkHashBytes(hash, metadata.headset, sizeof(metadata.headset));
    hash = benchmarkHashBytes(hash, metadata.aaMode, sizeof(metadata.aaMode));
    hash = benchmarkHashBytes(hash, metadata.dlssMode, sizeof(metadata.dlssMode));
    hash = benchmarkHashBytes(hash, metadata.build, sizeof(metadata.build));
    hash = benchmarkHashBytes(hash, &settingsEpoch, sizeof(settingsEpoch));
    return hash ? hash : 1;
}

// Defined with the other native readiness helpers below; the benchmark uses
// the same freshness rules as the monitor's native tiles.
bool nativeCpuReady(const NativeTimingSnapshot& timing);
bool nativeGpuReady(const NativeTimingSnapshot& timing, const GpuFrameSnapshot& gpu);
bool nativeApplicationCpuReady(const NativeTimingSnapshot& timing);
bool nativeApplicationGpuReady(const NativeTimingSnapshot& timing, const GpuFrameSnapshot& gpu);

void benchmarkCopy(char* out, size_t size, const char* value) {
    if (!out || !size) return;
    std::strncpy(out, value ? value : "", size - 1);
    out[size - 1] = 0;
}

bool updateBenchmarkMetadata(State& s, const NativeTimingSnapshot& timing,
                             uint64_t nowMs) {
    if (!timing.active || !timing.generation || !timing.firstSequence) return false;
    if (s.nativeBenchmarkGeneration != timing.generation ||
        s.nativeBenchmarkFirstSequence != timing.firstSequence) {
        s.nativeBenchmarkGeneration=timing.generation;
        s.nativeBenchmarkFirstSequence=timing.firstSequence;
        s.nativeBenchmarkMetadata={};
        s.nativeBenchmarkMetadataMs=0;
    }
    const bool refreshMetadata = !s.nativeBenchmarkMetadataMs ||
        nowMs < s.nativeBenchmarkMetadataMs || nowMs - s.nativeBenchmarkMetadataMs >= 500;
    NativeBenchmarkMetadata metadata=s.nativeBenchmarkMetadata;
    if (timing.haveCpu) {
        for (unsigned eye = 0; eye < 2; ++eye) {
            if (!timing.cpu.inputWidth[eye] || !timing.cpu.inputHeight[eye] ||
                !timing.cpu.outputWidth[eye] || !timing.cpu.outputHeight[eye]) return false;
            metadata.inputWidth[eye]=timing.cpu.inputWidth[eye];
            metadata.inputHeight[eye]=timing.cpu.inputHeight[eye];
            metadata.outputWidth[eye]=timing.cpu.outputWidth[eye];
            metadata.outputHeight[eye]=timing.cpu.outputHeight[eye];
        }
        std::memcpy(metadata.gameFov,timing.cpu.gameFov,sizeof(metadata.gameFov));
        std::memcpy(metadata.treatments,timing.cpu.treatments,sizeof(metadata.treatments));
        metadata.featureEpoch=timing.cpu.featureEpoch;
    }
    if (!metadata.inputWidth[0]) return false;
    if (refreshMetadata) {
        NativeRenderLabels labels{};
        if (nativeRenderLabels(&labels) && labels.valid) {
            benchmarkCopy(metadata.runtime, sizeof(metadata.runtime), labels.runtimeName);
            benchmarkCopy(metadata.headset, sizeof(metadata.headset), labels.systemName);
        }
        const std::string aa = Config::get().getString("fix.temporal_aa", "off");
        const std::string dlss = Config::get().getString("fix.temporal_aa_model", "k");
        benchmarkCopy(metadata.aaMode, sizeof(metadata.aaMode), aa.c_str());
        benchmarkCopy(metadata.dlssMode, sizeof(metadata.dlssMode), dlss.c_str());
        benchmarkCopy(metadata.build, sizeof(metadata.build), EDVR_VERSION_STRING);
        s.nativeBenchmarkMetadataMs = nowMs;
    }
    if (std::isfinite(timing.predictedPeriodMs) && timing.predictedPeriodMs > 0.0) {
        // Quantize to 0.1 Hz so normal prediction jitter cannot restart a
        // benchmark scope. This is context, not the GPU conversion factor.
        const double hz = 1000.0 / timing.predictedPeriodMs;
        metadata.refreshMilliHz = static_cast<uint32_t>(std::llround(hz * 10.0) * 100.0);
    }
    const uint64_t scope = benchmarkScope(timing, metadata,
                                          s.nativeBenchmarkSettingsEpoch.load(std::memory_order_relaxed));
    if (scope != s.nativeBenchmarkScope) {
        s.nativeBenchmarkScope = scope;
        s.nativeBenchmarkMetadata = metadata;
    }
    return true;
}

const char* benchmarkAbortName(uint32_t reason) {
    switch (reason) {
    case kNativeBenchmarkScopeChanged: return "scope-changed";
    case kNativeBenchmarkOverflow: return "overflow";
    case kNativeBenchmarkClockReversed: return "clock-reversed";
    case kNativeBenchmarkTransportLoss: return "transport-loss";
    default: return "completed";
    }
}

void logNativeBenchmark(State& s, const NativeBenchmarkReport& report) {
    const auto& m = report.metadata;
    char cpuPercentiles[96] = "--/--/--";
    char gpuPercentiles[96] = "--/--/--";
    if (report.cpu.available)
        snprintf(cpuPercentiles, sizeof(cpuPercentiles), "%.3f/%.3f/%.3f",
                 report.cpu.p50, report.cpu.p95, report.cpu.p99);
    if (report.gpu.available)
        snprintf(gpuPercentiles, sizeof(gpuPercentiles), "%.3f/%.3f/%.3f",
                 report.gpu.p50, report.gpu.p95, report.gpu.p99);
    const uint64_t sampleDuration = report.sampleEndedAtMs >= report.startedAtMs
        ? report.sampleEndedAtMs - report.startedAtMs : 0;
    Log::get().note("native benchmark: window %llu, scope %llu, status %s, "
                    "sample %llu ms [%llu..%llu], drain %llu ms (finished %llu), cpu p50/p95/p99 %s ms valid %llu invalid %llu missing %llu, "
                    "gpu p50/p95/p99 %s ms valid %llu invalid %llu missing %llu, "
                    "input %ux%u/%ux%u output %ux%u/%ux%u refresh %u mHz, runtime=\"%s\" headset=\"%s\" aa=\"%s\" dlss=\"%s\" build=\"%s\"; elapsed windows are independent CPU/GPU samples.",
                    static_cast<unsigned long long>(report.window),
                    static_cast<unsigned long long>(report.scope), benchmarkAbortName(report.abortReason),
                    static_cast<unsigned long long>(sampleDuration),
                    static_cast<unsigned long long>(report.startedAtMs),
                    static_cast<unsigned long long>(report.sampleEndedAtMs),
                    static_cast<unsigned long long>(report.drainMs),
                    static_cast<unsigned long long>(report.endedAtMs), cpuPercentiles,
                    static_cast<unsigned long long>(report.cpu.valid),
                    static_cast<unsigned long long>(report.cpu.invalid),
                    static_cast<unsigned long long>(report.cpu.missing),
                    gpuPercentiles,
                    static_cast<unsigned long long>(report.gpu.valid),
                    static_cast<unsigned long long>(report.gpu.invalid),
                    static_cast<unsigned long long>(report.gpu.missing),
                    m.inputWidth[0], m.inputHeight[0], m.inputWidth[1], m.inputHeight[1],
                    m.outputWidth[0], m.outputHeight[0], m.outputWidth[1], m.outputHeight[1],
                    m.refreshMilliHz, m.runtime, m.headset, m.aaMode, m.dlssMode, m.build);
    (void)s;
    Log::get().note("native benchmark workload: window %llu, feature epoch %llu, treatments %u/%u, game FOV radians L %.5f/%.5f/%.5f/%.5f R %.5f/%.5f/%.5f/%.5f; order left/right/up/down; input is submitted ROI, output is active XR target.",
        static_cast<unsigned long long>(report.window),static_cast<unsigned long long>(m.featureEpoch),
        m.treatments[0],m.treatments[1],m.gameFov[0][0],m.gameFov[0][1],m.gameFov[0][2],m.gameFov[0][3],
        m.gameFov[1][0],m.gameFov[1][1],m.gameFov[1][2],m.gameFov[1][3]);
}

}  // namespace

void perfMonitorFrame(ID3D11Device* dev) {
    State& s = g_s;
    ++s.frameNo;
    Frame f;
    const int64_t q = qpcNow();
    // The gap as it really was. f.presentMs below is 0 for a gap of 5 s or more, which keeps a loading
    // screen or a suspend out of the ring's statistics; the freeze log needs the real length.
    double gapMs = 0.0;
    if (s.lastQpc && qpcFrequency() > 0) {
        const double ms = static_cast<double>(q - s.lastQpc) * 1000.0 / static_cast<double>(qpcFrequency());
        f.presentMs = ms > 0.0 && ms < 5000.0 ? static_cast<float>(ms) : 0.0f;
        if (ms > 0.0 && std::isfinite(ms)) gapMs = ms;
    }
    s.lastQpc = q;
    // THE FRAME'S EDGE IS ALSO THE TICK CHAIN'S CUT, on the same clock reading:
    // the ticks handed over are exactly those measured since the previous frame's
    // edge, so the frame's own length, EDVR's ticks in it, the real Present and
    // the rest add up (frame_ticks.h). The stretch since the last mark is the
    // menu tick's own head; the remainder of this function is marked at its end.
    f.ticks = g_frameTicks.cut("menu_tick", q, qpcFrequency());
    // Engine motion's hooks, cut at the same edge (engine_motion_cpu.h): this
    // thread is the render thread, the one that calls Present. The figures are
    // this frame's own, every call clocked -- what the LONG FRAME line reads.
    {
        const emcpu::Figures em = g_engineMotion.onFrame(qpcFrequency(), nowMs());
        f.emMeasured = em.measured;
        f.emMs = static_cast<float>(em.renderMs);
        f.emCalls = em.renderCalls > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(em.renderCalls);
        emcpu::WindowReport report;
        if (g_engineMotion.takeReport(report)) logEngineMotion(report);
    }
    // The frame's waits: Present's, noted by the swapchain hook a moment
    // ago; WaitGetPoses's, over the channel.
    f.presentWaitMs = s.pendingPresentWaitMs;
    s.pendingPresentWaitMs = 0.0f;
    f.native = nativeMenuActive() ? 1 : 0;
    // Independent completion histories: an asynchronous GPU result is not
    // assigned to whichever Present happened to observe it.
    if (f.native) {
        const uint64_t historyNow=GetTickCount64();
        const NativeTimingSnapshot timing = nativeTimingSnapshot();
        const GpuFrameSnapshot gpu = gpuFrameSnapshot();
        s.nativeHistory.observe(true, timing, gpu, historyNow);
        if (!updateBenchmarkMetadata(s, timing, historyNow)) {
            s.nativeBenchmark.reset();
            s.nativeBenchmarkScope = 0;
        }
        NativeBenchmarkObservation tick{};
        tick.scope = s.nativeBenchmarkScope;
        tick.metadata = &s.nativeBenchmarkMetadata;
        // Consume completion queues rather than the latest-value snapshots:
        // a single Present can retire several delayed timestamp results.
        NativeTimingSnapshot cpuCompletions[32]{};
        uint64_t cpuDropped = 0;
        const unsigned cpuCount = nativeTimingReadCompletions(
            s.nativeBenchmarkCpuCursor, cpuCompletions, _countof(cpuCompletions), cpuDropped);
        s.nativeBenchmark.noteDropped(true, cpuDropped);
        for (unsigned i = 0; i < cpuCount; ++i) {
            const NativeTimingSnapshot& completion = cpuCompletions[i];
            if (completion.generation != timing.generation ||
                completion.firstSequence != timing.firstSequence || !completion.sequence) continue;
            if (!updateBenchmarkMetadata(s, completion, historyNow)) continue;
            NativeBenchmarkObservation benchmark{};
            benchmark.scope = s.nativeBenchmarkScope;
            benchmark.metadata = &s.nativeBenchmarkMetadata;
            benchmark.cpuSequence = completion.sequence;
            benchmark.cpuAtMs = completion.capturedAtMs;
            benchmark.cpuMs = completion.applicationMs;
            benchmark.cpuValid = completion.haveCpu && !completion.invalid &&
                                 completion.applicationValid;
            s.nativeBenchmark.observe(benchmark, historyNow);
        }
        GpuFrameSnapshot gpuCompletions[32]{};
        uint64_t gpuDropped = 0;
        const unsigned gpuCount = gpuFrameReadCompletions(
            s.nativeBenchmarkGpuCursor, gpuCompletions, _countof(gpuCompletions), gpuDropped);
        s.nativeBenchmark.noteDropped(false, gpuDropped);
        for (unsigned i = 0; i < gpuCount; ++i) {
            const GpuFrameSnapshot& completion = gpuCompletions[i];
            if (!completion.haveResult || !completion.result.sequence ||
                completion.result.sequence < timing.firstSequence ||
                completion.result.source != GpuSpanSource::ApplicationRender) continue;
            NativeBenchmarkObservation benchmark{};
            benchmark.scope = s.nativeBenchmarkScope;
            benchmark.metadata = &s.nativeBenchmarkMetadata;
            benchmark.gpuSequence = completion.result.sequence;
            benchmark.gpuAtMs = completion.result.completedAtMs;
            benchmark.gpuMs = completion.result.outerMs;
            benchmark.gpuValid = completion.result.reason == GpuSpanReason::Valid;
            s.nativeBenchmark.observe(benchmark, historyNow);
        }
        // Drain observations before the clock tick can finalize the window.
        // A completion already queued at the deadline still belongs to it.
        updateBenchmarkMetadata(s, timing, historyNow);
        tick.scope=s.nativeBenchmarkScope;
        tick.metadata=&s.nativeBenchmarkMetadata;
        s.nativeBenchmark.observe(tick, historyNow);
        NativeBenchmarkReport report{};
        if (s.nativeBenchmark.takeReport(&report)) {
            logNativeBenchmark(s, report);
        }
        if (s.nativeHistoryReports<60 && (!s.nativeHistoryLogMs || historyNow-s.nativeHistoryLogMs>=5000)) {
            s.nativeHistoryLogMs=historyNow;++s.nativeHistoryReports;
            const auto cpu=s.nativeHistory.submit(historyNow,200), historyGpu=s.nativeHistory.producer(historyNow,200);
            const auto copy=s.nativeHistory.transfer(historyNow,200), compose=s.nativeHistory.compose(historyNow,200);
            Log::get().note("native metrics history: 200ms window, submit %.3f ms (%u samples), producer %.3f ms (%u samples), XR copy %.3f ms (%u samples), XR compose %.3f ms (%u samples), predicted period %.3f ms; zero samples means unavailable; independent sources.",
                cpu.meanMs,cpu.count,historyGpu.meanMs,historyGpu.count,copy.meanMs,copy.count,compose.meanMs,compose.count,s.nativeHistory.predictedPeriod(historyNow));
        }
    } else {
        s.nativeHistory.clear();
        s.nativeBenchmark.reset();
        s.nativeBenchmarkScope = 0;
        s.nativeBenchmarkMetadataMs = 0;
        s.nativeBenchmarkMetadata = {};
    }
    // EDVR's part: the events of the frame just ending.
    const uint32_t ev = s.events.exchange(0);
    f.events = static_cast<uint16_t>(ev & 0xFFFFu);
    f.eventMs = static_cast<float>(s.eventUs.exchange(0)) / 1000.0f;
    if (detail::g_perfMonitorSampleDraws && qpcFrequency() > 0) {
        // Scaled: only every kPerfMonitorDrawTimeStride-th draw of a sampled
        // frame is clocked (perf_monitor.h says why), so the frame's figure is
        // the clocked draws' own time times the stride -- an estimate of the
        // same quantity the line has always reported, not a new one.
        const int64_t own = (s.drawWholeTicks - s.drawRealTicks) *
                            static_cast<int64_t>(kPerfMonitorDrawTimeStride);
        s.drawsMsRunning = own > 0 ? static_cast<float>(static_cast<double>(own) * 1000.0 /
                                                        static_cast<double>(qpcFrequency()))
                                   : 0.0f;
        s.drawsSampled = true;
        f.drawsFresh = true;
        s.drawWindowMs += s.drawsMsRunning;
        s.drawWindowMaxMs = std::max(s.drawWindowMaxMs, s.drawsMsRunning);
        ++s.drawWindowSamples;
    }
    // Reuse the existing sampled clocks; do not time every draw just to
    // explain a settlement's many thousands of hook invocations. Average
    // only measured frames, never the held value copied into the ring.
    if (s.frameNo % 1800 == 0) {
        Log::get().note("draw hook CPU: 1800-frame window ending %u; %.3f ms/sampled frame mean, %.3f ms max, %u sampled frames (one in %u); excludes forwarded game draw time, includes EDVR reissues; zero samples means unavailable. Each sampled frame's figure is estimated from every %uth draw, scaled by %u.",
            s.frameNo, s.drawWindowSamples ? s.drawWindowMs / s.drawWindowSamples : 0.0,
            double(s.drawWindowMaxMs), s.drawWindowSamples, unsigned(kDrawSampleEvery),
            unsigned(kPerfMonitorDrawTimeStride), unsigned(kPerfMonitorDrawTimeStride));
        s.drawWindowMs = 0.0; s.drawWindowMaxMs = 0.0f; s.drawWindowSamples = 0;
        // Written every window, quiet or not, so a window with no refresh in it (all zeros) can be told from a build that does not have this line.
        const uint32_t reloads = s.cfgReloads.exchange(0, std::memory_order_relaxed);
        const uint32_t edits = s.cfgEdits.exchange(0, std::memory_order_relaxed);
        const double reloadMs = static_cast<double>(s.cfgReloadUs.exchange(0, std::memory_order_relaxed)) / 1000.0;
        const double reloadMaxMs = static_cast<double>(s.cfgReloadMaxUs.exchange(0, std::memory_order_relaxed)) / 1000.0;
        char refreshLine[320];
        formatConfigRefreshWindow(refreshLine, sizeof(refreshLine), s.frameNo, reloads, reloadMs, reloadMaxMs, edits);
        Log::get().note("%s", refreshLine);
    }
    f.cpuDrawsMs = s.drawsMsRunning;
    s.drawWholeTicks = s.drawRealTicks = 0;
    detail::g_perfMonitorSampleDraws = (s.frameNo % kDrawSampleEvery) == 0;
    const DeviceCreates made = deviceCreatesTake();
    f.createTextures = made.textures;
    f.createBuffers = made.buffers;
    f.createShaders = made.shaders;
    f.createMb = static_cast<float>(static_cast<double>(made.textureBytes + made.bufferBytes) / 1048576.0);
    ringPush(f);

    // A frame our own clock calls long: the page's "last drop", and the log's decisions about it
    // (judgeLongFrame: blip, long, or freeze). A gap of 5 s or more used to be skipped here altogether, so a
    // multi-second freeze left no line; it is judged now, by its real length.
    const float budget = budgetNow();
    if (budget > 0.0f && gapMs > 2.0 * static_cast<double>(budget)) {
        judgeLongFrame(*ringLast(), budget, gapMs, q);
    }
    // The long-frame counts every few minutes, so a session that ends without a clean exit left them in
    // the log. The first frame arms the clock and says nothing.
    if (!s.freezeCountsMs) {
        s.freezeCountsMs = stampMs();
    } else if (elapsedMs(s.freezeCountsMs, kFreezeCountsEveryMs)) {
        s.freezeCountsMs = stampMs();
        writeFreezeSummary("periodic", false);
    }
    freezeTestTick();
    slowTestTick();

    if (s.active || (s.activeUntilMs && nowMs() < s.activeUntilMs)) {
        if (dueMs(s.slowMs, kSlowEveryMs)) {
            s.slowMs = stampMs();
            guardedBudget(g_budget, [&] { slowSample(dev); });
        }
    }
    // Everything above ran after the cut, so it belongs to the NEXT frame's ticks:
    // the ring push, the benchmark bookkeeping, the LONG FRAME line itself.
    frameTick("perf_monitor");
}

void perfMonitorNoteEvent(uint32_t bits, double ms) {
    State& s = g_s;
    s.events.fetch_or(bits, std::memory_order_relaxed);
    // These events can change the render treatment or native menu state in
    // the middle of a benchmark window. Advance a cheap epoch immediately;
    // the next monitor tick hashes it into the scope and aborts the window.
    constexpr uint32_t kBenchmarkScopeEvents = kEvReload | kEvIniWrite |
        kEvBinds | kEvNgx | kEvFsr | kEvMenu | kEvCensus | kEvEyeDump;
    if (bits & kBenchmarkScopeEvents)
        s.nativeBenchmarkSettingsEpoch.fetch_add(1, std::memory_order_relaxed);
    // The config refresh window's books (the line at the 1800-frame mark): every reload with its own duration, every queued menu edit.
    if (bits & kEvReload) {
        const uint32_t us = ms > 0.0 ? (ms < 60000.0 ? static_cast<uint32_t>(ms * 1000.0) : 60000000u) : 0u;
        s.cfgReloads.fetch_add(1, std::memory_order_relaxed);
        s.cfgReloadUs.fetch_add(us, std::memory_order_relaxed);
        uint32_t cur = s.cfgReloadMaxUs.load(std::memory_order_relaxed);
        while (us > cur && !s.cfgReloadMaxUs.compare_exchange_weak(cur, us, std::memory_order_relaxed)) {
        }
    }
    if (bits & kEvIniWrite) s.cfgEdits.fetch_add(1, std::memory_order_relaxed);
    if (ms > 0.0) {
        const int32_t us = ms < 60000.0 ? static_cast<int32_t>(ms * 1000.0) : 60000000;
        // The longest event of the frame is the one worth naming.
        int32_t cur = s.eventUs.load(std::memory_order_relaxed);
        while (us > cur && !s.eventUs.compare_exchange_weak(cur, us)) {
        }
    }
}

void perfMonitorNoteCpu(int which, double ms) {
    Frame* f = ringLast();
    if (!f || !(ms >= 0.0)) return;
    if (which == kCpuBoundary) f->cpuBoundaryMs = static_cast<float>(ms);
}

void perfMonitorNotePresentWait(double ms) {
    if (ms >= 0.0 && ms < 5000.0) g_s.pendingPresentWaitMs = static_cast<float>(ms);
}

// perfMonitorSampleDraws is inline in the header now (perf_monitor.h).

void perfMonitorDrawTicks(int64_t wholeTicks, int64_t realTicks) {
    if (wholeTicks > 0) g_s.drawWholeTicks += wholeTicks;
    if (realTicks > 0) g_s.drawRealTicks += realTicks;
}

void perfMonitorSetActive(bool active) {
    State& s = g_s;
    if (active && !s.active) s.slowMs = 0;   // the first sample at once
    s.active = active;
    if (active) s.activeUntilMs = nowMs() + kGraceMs;
}

namespace {
PerfRecentTimes recentTimes() {
    PerfRecentTimes times;
    float seconds = 0;
    for (int i = g_s.count - 1; i >= 0 && seconds < kMatchWindowS; --i) {
        const Frame& f = ringAt(i);
        if (!f.native) times.add(f.presentMs, f.presentWaitMs);
        seconds += f.presentMs / 1000.f;
    }
    return times;
}

uint64_t nativeTimingAge(uint64_t capturedAtMs) {
    const uint64_t now = GetTickCount64();
    return capturedAtMs && now >= capturedAtMs ? now - capturedAtMs : UINT64_MAX;
}

bool nativeCpuReady(const NativeTimingSnapshot& timing) {
    return timing.active && timing.haveCpu && !timing.invalid && timing.firstSequence &&
           timing.cpu.sequence >= timing.firstSequence && nativeTimingAge(timing.capturedAtMs) <= 2000;
}

// The newest frame carries the CPU figure the history averages: the caller
// work per cycle (timing v5), or an older runtime's pre-submit application
// time (NativePerfHistory::cpuFigure).
bool nativeApplicationCpuReady(const NativeTimingSnapshot& timing) {
    return nativeCpuReady(timing) && NativePerfHistory::cpuFigure(timing).valid;
}

// The CPU figure's label: plain for the caller work per cycle, named as the
// pre-submit phase when an older runtime sends no caller work.
bool nativeCpuPreSubmit(const NativePerfHistory& history) {
    return history.cpuSource() == NativeCpuSource::PreSubmit;
}

bool nativeGpuReady(const NativeTimingSnapshot& timing, const GpuFrameSnapshot& gpu) {
    const uint64_t observedAge = nativeTimingAge(gpu.capturedAtMs);
    return timing.active && !timing.invalid && timing.firstSequence && gpu.enabled && gpu.haveResult &&
           gpu.result.reason == GpuSpanReason::Valid && gpu.result.sequence >= timing.firstSequence &&
           observedAge <= 2000 && gpu.result.ageMs <= 2000 - observedAge;
}

bool nativeApplicationGpuReady(const NativeTimingSnapshot& timing, const GpuFrameSnapshot& gpu) {
    return nativeGpuReady(timing, gpu) &&
           gpu.result.source == GpuSpanSource::ApplicationRender;
}

uint64_t nativeGpuAge(const GpuFrameSnapshot& gpu) {
    const uint64_t age = nativeTimingAge(gpu.capturedAtMs);
    return gpu.result.ageMs > UINT64_MAX - age ? UINT64_MAX : gpu.result.ageMs + age;
}
}

int perfMonitorTiles(PerfTile* out, int max) {
    State& s = g_s;
    if (!out || max <= 0) return 0;
    int n = 0;
    auto tile = [&](const char* caption, const char* value, const char* sub) {
        if (n >= max) return;
        strncpy(out[n].caption, caption, sizeof(out[n].caption) - 1);
        out[n].caption[sizeof(out[n].caption) - 1] = 0;
        strncpy(out[n].value, value, sizeof(out[n].value) - 1);
        out[n].value[sizeof(out[n].value) - 1] = 0;
        strncpy(out[n].sub, sub ? sub : "", sizeof(out[n].sub) - 1);
        out[n].sub[sizeof(out[n].sub) - 1] = 0;
        ++n;
    };
    // Sub-lines are at most about thirty characters: a four-across tile
    // fits sixteen a line in its face, over two lines.
    char v[32], sub[40];

    // The interval ring, summarised, and the drops attributed.
    float present[kRing], busy[kRing];
    int cnt = 0;
    const bool native = nativeMenuActive();
    const NativeTimingSnapshot nativeTiming = nativeTimingSnapshot();
    const bool haveNativePeriod = native && nativeCpuReady(nativeTiming);
    const uint64_t historyNow = GetTickCount64();
    const auto nativeApplicationCpu = s.nativeHistory.applicationCpu(historyNow, 200);
    const auto nativeApplicationGpu = s.nativeHistory.applicationGpu(historyNow, 200);
    const auto nativeSubmit = s.nativeHistory.submit(historyNow, 200);
    const auto nativeWait = s.nativeHistory.wait(historyNow, 200);
    // The mean over fpsVR's own update window, so the two numbers can be
    // read side by side. Averaging the whole ten-second ring instead read
    // about a millisecond under it (flown 2026-09-07).
    const float recentCpu = recentTimes().threadMs();
    double edvrBoundary = 0.0;
    for (int i = 0; i < s.count; ++i) {
        const Frame& f = ringAt(i);
        present[cnt] = f.presentMs;
        const float b = f.presentMs - f.presentWaitMs;
        busy[cnt] = f.native ? 0.0f : (b > 0.0f ? b : 0.0f);
        ++cnt;
        edvrBoundary += f.cpuBoundaryMs;
    }
    const PerfStats ps = perfStatsOf(present, cnt);
    const char* noTiming = "native OpenXR timing unavailable";

    // Row 1: the frame, as fpsVR reports it. On the native path the GPU
    // and CPU figures are the runtime's own (NativePerfHistory); off it,
    // the only one left is EDVR's measure of the render thread, the period
    // less the time blocked in Present. The rows the compositor's frame
    // timing filled -- app GPU, dropped, by cause, reprojected -- went with
    // the openvr half that published it (frame_flag v34, 2026-09-23).
    if (ps.count) {
        snprintf(v, sizeof(v), "%.1f", perfFpsOf(ps.avgMs));
        snprintf(sub, sizeof(sub), "fps, period %.1f ms", ps.avgMs);
        tile("FRAME RATE", v, sub);
        snprintf(v, sizeof(v), "%.0f", perfFpsOf(ps.p99Ms));
        snprintf(sub, sizeof(sub), "fps, worst 1%% %.1f ms", ps.p99Ms);
        tile("1% LOW", v, sub);
    } else {
        tile("FRAME RATE", "--", "measuring");
        tile("1% LOW", "--", "");
    }
    if (native && nativeApplicationGpu.count) {
        snprintf(v, sizeof(v), "%.1f", nativeApplicationGpu.meanMs);
        tile("GPU TIME", v, "ms application render; elapsed");
    } else if (native) {
        tile("GPU TIME", "--", noTiming);
    }
    if (native && nativeApplicationCpu.count) {
        // The caller work per cycle: the game's thread from one pose wait's
        // return to the next, submits included. An older runtime sends none,
        // and its pre-submit time is named as such.
        const bool preSubmit = nativeCpuPreSubmit(s.nativeHistory);
        snprintf(v, sizeof(v), "%.1f", nativeApplicationCpu.meanMs);
        snprintf(sub, sizeof(sub), preSubmit ? "ms; the runtime DLL is older" : "ms game thread per frame");
        tile(preSubmit ? "CPU PRE-SUBMIT" : "CPU TIME", v, sub);
    } else if (ps.count && !native) {
        const PerfStats bs = perfStatsOf(busy, cnt);
        // EDVR's own clock, and the tile says which figure it is.
        snprintf(v, sizeof(v), "%.1f", recentCpu > 0.0f ? recentCpu : bs.avgMs);
        snprintf(sub, sizeof(sub), "ms thread; no app stamps");
        tile("CPU TIME", v, sub);
    } else {
        tile("CPU TIME", "--", native ? "application timing unavailable" : "");
    }

    // Row 2: the submit.
    if (native && nativeSubmit.count) {
        snprintf(v, sizeof(v), "%.1f", nativeSubmit.meanMs);
        snprintf(sub, sizeof(sub), "ms elapsed; pose wait %.1f", nativeWait.meanMs);
        tile("SUBMIT WALL", v, sub);
    } else if (native) {
        tile("SUBMIT WALL", "--", noTiming);
    }

    // Row 3: the display and the machine.
    {
        uint32_t ew = 0, eh = 0;
        const bool haveEye = eyeTextureSize(&ew, &eh);
        const double predicted = s.nativeHistory.predictedPeriod(historyNow);
        if (native && haveNativePeriod && predicted > 0.0) {
            snprintf(v, sizeof(v), "%.1f ms", predicted);
            if (haveEye) snprintf(sub, sizeof(sub), "runtime prediction; %ux%u", ew, eh);
            else snprintf(sub, sizeof(sub), "runtime prediction");
        } else if (haveEye) {
            snprintf(v, sizeof(v), "--");
            snprintf(sub, sizeof(sub), "%ux%u per eye", ew, eh);
        } else {
            snprintf(v, sizeof(v), "--");
            snprintf(sub, sizeof(sub), "not published yet");
        }
        tile(native ? "XR PERIOD" : "DISPLAY", v, sub);
    }
    if (s.cpuSystemPct >= 0.0f) {
        snprintf(v, sizeof(v), "%.0f%%", s.cpuSystemPct);
        snprintf(sub, sizeof(sub), "Elite %.0f%% of %u", s.cpuProcessPct >= 0.0f ? s.cpuProcessPct : 0.0f,
                 s.cpuThreads);
        tile("CPU", v, sub);
    } else {
        tile("CPU", "--", "sampling");
    }
    if (s.nvOk) {
        snprintf(v, sizeof(v), "%d%%", s.gpuLoadPct);
        if (s.gpuTempC > -1000) snprintf(sub, sizeof(sub), "load, %d C", s.gpuTempC);
        else snprintf(sub, sizeof(sub), "load");
        tile("GPU", v, sub);
    } else {
        tile("GPU", "--", s.nvTried ? s.nvWhy : "sampling");
    }
    {
        char vu[24] = "--", vb[24] = "?";
        if (s.vramBudget) {
            gb(vu, sizeof(vu), s.vramUsed);
            gb(vb, sizeof(vb), s.vramBudget);
        }
        snprintf(sub, sizeof(sub), "of %s GB", vb);
        tile("VRAM", vu, s.vramBudget ? sub : "sampling");
    }

    // Row 4: memory, and EDVR's own price.
    {
        char rp[24] = "--", ra[24] = "?", rt[24] = "?";
        if (s.ramTotal) {
            gb(rp, sizeof(rp), s.ramProcess);
            gb(ra, sizeof(ra), s.ramAvail);
            gb(rt, sizeof(rt), s.ramTotal);
        }
        snprintf(sub, sizeof(sub), "GB, %s free of %s", ra, rt);
        tile("RAM", rp, s.ramTotal ? sub : "sampling");
    }
    {
        const double frames = cnt > 0 ? static_cast<double>(cnt) : 1.0;
        if (native) {
            char xr[32] = "--";
            const char* xrSub = "native timing unavailable";
            if (nativeTiming.active && !nativeTiming.invalid && nativeTiming.firstSequence) {
                if (!nativeTiming.haveDeviceGpu) {
                    xrSub = "pending";
                } else {
                    const auto& sample = nativeTiming.deviceGpu;
                    const uint64_t age = nativeTimingAge(sample.completedAtMs);
                    if (sample.status == EdvrNativeGpuDisabled) {
                        snprintf(sub, sizeof(sub), "disabled; age %llums",
                                 static_cast<unsigned long long>(age));
                        xrSub = sub;
                    } else if (sample.status == EdvrNativeGpuPending) {
                        snprintf(sub, sizeof(sub), "pending; age %llums",
                                 static_cast<unsigned long long>(age));
                        xrSub = sub;
                    } else if (age > 2000 || sample.status == EdvrNativeGpuStale) {
                        snprintf(sub, sizeof(sub), "stale; age %llums",
                                 static_cast<unsigned long long>(age));
                        xrSub = sub;
                    } else if (sample.status == EdvrNativeGpuNotSeparate) {
                        xrSub = "N/A; not separate";
                    } else if (sample.status == EdvrNativeGpuValid) {
                        const auto copy = s.nativeHistory.transfer(historyNow, 200);
                        const auto compose = s.nativeHistory.compose(historyNow, 200);
                        if (copy.count && compose.count) {
                            snprintf(xr, sizeof(xr), "%.2f / %.2f", copy.meanMs, compose.meanMs);
                            snprintf(sub, sizeof(sub), "ms, 200ms mean; XR device only");
                            xrSub = sub;
                        } else xrSub = "no recent samples";
                    } else {
                        xrSub = "unavailable";
                    }
                }
            }
            tile("XR COPY/COMPOSE", xr, xrSub);
        } else {
            snprintf(v, sizeof(v), "--");
            snprintf(sub, sizeof(sub), glitchConsumerPresent() ? "no pair yet" : "no openvr half");
        }
        if (!native) tile("EDVR GPU", v, sub);
        const double total = edvrBoundary / frames +
                             (s.drawsSampled ? static_cast<double>(s.drawsMsRunning) : 0.0);
        snprintf(v, sizeof(v), "%.2f", total);
        if (native) {
            snprintf(sub, sizeof(sub), "ms/frame; hooks/boundary only");
        } else if (s.drawsSampled) {
            snprintf(sub, sizeof(sub), "ms/frame; hooks %.2f", static_cast<double>(s.drawsMsRunning));
        } else {
            snprintf(sub, sizeof(sub), "ms/frame; hooks not yet");
        }
        tile("EDVR CPU", v, sub);
    }
    {
        uint32_t t = 0, resets = 0;
        double avg = 0.0, mx = 0.0, rej = 0.0, clip = 0.0;
        double temporal = -1.0, sharpen = -1.0;
        bool trained = false;
        // The engine fix.temporal_aa names RIGHT NOW, and its own word for
        // the tile. Trying NVIDIA's totals first and falling through to
        // AMD's read the engine that ran FIRST: neither total is cleared on
        // a live switch, so after a dlss -> fsr A/B this tile kept printing
        // "ms NVIDIA/call" beside a price line that said fsr (the review of
        // 2026-09-16, F6). temporalPassTrainedTotals answers for the current
        // engine and writes the word whatever it answers.
        const char* engineWord = "NVIDIA";
        if (temporalPassTrainedTotals(&t, &avg, &mx, &resets, &engineWord, nullptr) && t) {
            temporal = avg;
            trained = true;
        } else if (temporalPassTotals(&t, &avg, &mx, &rej, &clip) && t) {
            temporal = avg;
        }
        if (sharpenPassTotals(&t, &avg, &mx) && t) sharpen = avg;
        if (temporal >= 0.0 && sharpen >= 0.0) {
            snprintf(v, sizeof(v), "%.1f+%.1f", temporal, sharpen);
            // The trained-engine figure here is a pooled average over every
            // call (any role, either eye), not one eye's price -- say so
            // rather than mislabel it "ms/eye" alongside the sharpen pass,
            // which genuinely is per eye. trained's format has one %s; the
            // untrained one has none and simply ignores the extra argument.
            snprintf(sub, sizeof(sub),
                     trained ? "ms %s/call + sharpen/eye" : "ms/eye temporal + sharpen",
                     engineWord);
        } else if (temporal >= 0.0) {
            snprintf(v, sizeof(v), "%.2f", temporal);
            snprintf(sub, sizeof(sub), trained ? "ms/call, %s's pass" : "ms/eye, temporal pass",
                     engineWord);
        } else if (sharpen >= 0.0) {
            snprintf(v, sizeof(v), "%.2f", sharpen);
            snprintf(sub, sizeof(sub), "ms/eye, sharpen");
        } else {
            snprintf(v, sizeof(v), "--");
            snprintf(sub, sizeof(sub), "no pass running");
        }
        tile("EDVR PASSES", v, sub);
    }
    // The display side, which no producer tile can show: the runtime's
    // predicted period is the display cadence it advertises -- the same ring
    // value the periodic native-metrics log line prints -- and against the
    // base rate, a prediction past 1.5x base means the headset is delivering
    // a throttled display rate (e.g. SteamVR halving 90 Hz to 45 under the
    // wall) no matter what FRAME RATE's producer cadence reads. No runtime
    // accepted/completed-frame counter with display timestamps crosses the
    // ABI, so there is deliberately no measured display-cadence figure here.
    if (native) {
        const double predicted = s.nativeHistory.predictedPeriod(historyNow);
        const double baseMs = s.nativeHistory.basePeriodMs(historyNow);
        if (predicted > 0.0) {
            snprintf(v, sizeof(v), "%.1f", 1000.0 / predicted);
            if (baseMs > 0.0) {
                snprintf(sub, sizeof(sub), "Hz display (base %.1f)%s", 1000.0 / baseMs,
                         NativePerfHistory::displayThrottled(predicted, baseMs) ? "; THROTTLED" : "");
            } else {
                snprintf(sub, sizeof(sub), "Hz display; base unknown");
            }
        } else {
            snprintf(v, sizeof(v), "--");
            snprintf(sub, sizeof(sub), "runtime prediction unavailable");
        }
        tile("DISPLAY", v, sub);
    }
    return n;
}
void perfMonitorLastDropLine(char* buf, size_t bufLen) {
    State& s = g_s;
    if (!buf || !bufLen) return;
    if (!s.lastDropMs) {
        snprintf(buf, bufLen, "No long frame yet this session.");
        return;
    }
    char ev[120];
    eventList(s.lastDropEvents, s.lastDropEventMs, ev, sizeof(ev));
    const uint64_t ago = nowMs() - s.lastDropMs;
    snprintf(buf, bufLen, "Last long frame %.0f s ago: %.1f ms between Presents. EDVR events: %s.",
             static_cast<double>(ago) / 1000.0, static_cast<double>(s.lastDropFrameMs), ev);
    buf[bufLen - 1] = 0;
}
void perfMonitorLocalGpuLine(char* buf, size_t bufLen) {
    if (!buf || !bufLen) return;
    if (nativeMenuActive()) {
        const NativeTimingSnapshot timing = nativeTimingSnapshot();
        const GpuFrameSnapshot snap = gpuFrameSnapshot();
        const char* label = snap.haveResult && snap.result.source == GpuSpanSource::ApplicationRender
                                ? "Application render" : "Render to submit";
        if (!timing.active || !timing.firstSequence) { snprintf(buf, bufLen, "%s: native timing unavailable", label); return; }
        if (!snap.enabled) { snprintf(buf, bufLen, "%s: disabled", label); return; }
        if (!snap.haveResult) { snprintf(buf, bufLen, "%s: pending", label); return; }
        if (snap.result.reason == GpuSpanReason::Valid && snap.result.sequence < timing.firstSequence) {
            snprintf(buf, bufLen, "%s: pending (new native session)", label); return;
        }
        if (snap.result.reason != GpuSpanReason::Valid) {
            snprintf(buf, bufLen, "%s: unavailable (%s)", label, gpuFrameReason(snap.result.reason)); return;
        }
        const uint64_t age = nativeGpuAge(snap);
        if (age > 2000) { snprintf(buf, bufLen, "%s: stale", label); return; }
        snprintf(buf, bufLen, "%s: %.2f ms (elapsed; frame %llu; age %llu ms)", label,
                 snap.result.outerMs, static_cast<unsigned long long>(snap.result.sourceFrame),
                 static_cast<unsigned long long>(age));
        buf[bufLen - 1] = 0;
        return;
    }
    const GpuFrameSnapshot snap = gpuFrameSnapshot();
    if (!snap.enabled) { snprintf(buf, bufLen, "Render to submit: disabled"); return; }
    if (!snap.haveResult) { snprintf(buf, bufLen, "Render to submit: pending"); return; }
    if (snap.result.reason != GpuSpanReason::Valid) {
        snprintf(buf, bufLen, "Render to submit: unavailable (%s; D3D11)",
                 gpuFrameReason(snap.result.reason)); return;
    }
    const uint64_t elapsed = GetTickCount64() >= snap.capturedAtMs
                                 ? GetTickCount64() - snap.capturedAtMs : 0;
    const uint64_t age = snap.result.ageMs + elapsed;
    if (age > 2000) { snprintf(buf, bufLen, "Render to submit: stale"); return; }
    snprintf(buf, bufLen, "Render to submit: %.2f ms (D3D11; frame %llu; age %llu ms)",
             snap.result.outerMs, static_cast<unsigned long long>(snap.result.sourceFrame),
             static_cast<unsigned long long>(age));
    buf[bufLen - 1] = 0;
}
void perfMonitorNativeTimingLine(char* buf, size_t bufLen) {
    if (!buf || !bufLen) return;
    const NativeTimingSnapshot timing = nativeTimingSnapshot();
    if (!nativeMenuActive()) { buf[0] = 0; return; }
    if (!timing.active || !timing.firstSequence) {
        snprintf(buf, bufLen, "Native wall timing: unavailable");
        return;
    }
    if (timing.invalid) {
        snprintf(buf, bufLen, "Native wall timing: invalid");
        return;
    }
    if (!timing.haveCpu) {
        snprintf(buf, bufLen, "Native wall timing: pending");
        return;
    }
    if (nativeTimingAge(timing.capturedAtMs) > 2000) {
        snprintf(buf, bufLen, "Native wall timing: stale");
        return;
    }
    const auto& f = timing.cpu;
    snprintf(buf, bufLen, "Wall ms: wait %.1f; temporal %.1f menu %.1f transfer %.1f compose %.1f",
             timing.waitMs, f.temporalMs[0] + f.temporalMs[1], f.menuMs[0] + f.menuMs[1],
             f.transferMs[0] + f.transferMs[1], f.composeMs);
    buf[bufLen - 1] = 0;
}
int perfMonitorGraph(int which, float* out, int max, float* budgetMs) {
    State& s = g_s;
    if (budgetMs) *budgetMs = budgetNow();
    if (!out || max <= 0) return 0;
    if (nativeMenuActive() && (which == kGraphGpu || which == kGraphCpu))
        return s.nativeHistory.graph(which == kGraphGpu, out, max, GetTickCount64());
    // Off the native path there is no GPU frame to plot: it was the
    // compositor's record, which crossed from the retired openvr half.
    if (which == kGraphGpu) return 0;
    const int n = s.count < max ? s.count : max;
    for (int i = 0; i < n; ++i) {
        const Frame& f = ringAt(s.count - n + i);
        if (which == kGraphCpu) {
            const float b = f.presentMs - f.presentWaitMs;
            out[i] = f.native ? 0.0f : (b > 0 ? b : 0);
        } else {
            out[i] = f.presentMs;
        }
    }
    return n;
}

void perfMonitorOverlayLine(char* buf, size_t bufLen) {
    State& s = g_s;
    if (!buf || !bufLen) return;
    // FPS keeps its one-second window; CPU/GPU match the menu's recent
    // compositor window. Thread time is explicitly named when unavailable.
    float present[kRing];
    int n = 0;
    float secs = 0;
    for (int i = s.count - 1; i >= 0; --i) {
        const Frame& f = ringAt(i);
        if (secs < 1.f) {
            present[n++] = f.presentMs;
            secs += f.presentMs / 1000.f;
        }
    }
    const PerfStats ps = perfStatsOf(present, n);
    if (!ps.count) { snprintf(buf, bufLen, "measuring"); return; }
    const PerfRecentTimes recent = recentTimes();
    if (nativeMenuActive()) {
        const NativeTimingSnapshot timing = nativeTimingSnapshot();
        const GpuFrameSnapshot gpu = gpuFrameSnapshot();
        const uint64_t now = GetTickCount64();
        const auto render = s.nativeHistory.applicationGpu(now, 200);
        const auto submit = s.nativeHistory.applicationCpu(now, 200);
        const bool haveGpu = nativeApplicationGpuReady(timing, gpu) && render.count;
        const bool haveCpu = nativeApplicationCpuReady(timing) && submit.count;
        char gpuValue[24] = "--", cpuValue[24] = "--";
        if (haveGpu) snprintf(gpuValue, sizeof(gpuValue), "%.1f", render.meanMs);
        if (haveCpu) snprintf(cpuValue, sizeof(cpuValue), "%.1f", submit.meanMs);
        // "cpu" is the caller work per cycle, the line's width unchanged; an
        // older runtime's stand-in says what it is.
        snprintf(buf, bufLen, "%.0f fps   gpu %s ms   %s %s ms",
            perfFpsOf(ps.avgMs), gpuValue, nativeCpuPreSubmit(s.nativeHistory) ? "cpu (pre-submit)" : "cpu",
            cpuValue);
        return;
    }
    char times[120] = "";
    {
        const GpuFrameSnapshot snap = gpuFrameSnapshot();
        const uint64_t now = GetTickCount64();
        const uint64_t age = snap.capturedAtMs && now >= snap.capturedAtMs
                                 ? snap.result.ageMs + now - snap.capturedAtMs : UINT64_MAX;
        if (snap.enabled && snap.haveResult && snap.result.reason == GpuSpanReason::Valid && age <= 2000)
            snprintf(times, sizeof(times), "   submit gpu %.1f   thread %.1f", snap.result.outerMs, recent.threadMs());
        else
            snprintf(times, sizeof(times), "   %.1f ms   thread %.1f", ps.avgMs, recent.threadMs());
    }
    snprintf(buf, bufLen, "%.0f fps%s", perfFpsOf(ps.avgMs), times);
    buf[bufLen - 1] = 0;
}

// The runtime closed its timing context (native_timing.cpp's close, through g_nativeTimingCloseObserver):
// the session is over, and this is the one moment before the process exits that the graphics half knows
// it. Not called from DllMain's process-exit path (see writeFreezeSummary).
void perfMonitorSessionEnd() {
    guarded("perfMonitorSessionEnd", [] { writeFreezeSummary("session_close", true); });
}

namespace {
// The observer is a plain pointer store at load time, which is safe under the loader lock.
struct CloseObserverRegistration {
    CloseObserverRegistration() { g_nativeTimingCloseObserver = &perfMonitorSessionEnd; }
};
const CloseObserverRegistration g_closeObserverRegistration;
}  // namespace

void perfMonitorShutdown() {
    State& s = g_s;
    writeFreezeSummary("shutdown", true);
    stallWatchShutdown();
    s.nativeHistory.clear();
    if (s.adapter3) {
        s.adapter3->Release();
        s.adapter3 = nullptr;
    }
    if (s.dropLogged) {
        Log::get().note("monitor: %u dropped or long frames were logged this session (of %u at most).",
                        s.dropLogged, kDropLogMax);
    }
}

}  // namespace edvr
