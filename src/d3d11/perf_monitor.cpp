#include "perf_monitor.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <psapi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/perf_math.h"
#include "../common/timing.h"
#include "device_hook.h"
#include "sharpen_pass.h"
#include "temporal_pass.h"

namespace edvr {
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
    // The compositor's word, filled in kFrameTimingLag frames later, when
    // the record for this frame has settled.
    float    appGpuMs = 0.0f;
    float    compGpuMs = 0.0f;
    float    compCpuMs = 0.0f;
    float    cpuFrameMs = 0.0f;
    float    appCpuMs = 0.0f;    // the app's busy time, poses ready to second submit
    float    posesReadyMs = 0.0f;
    float    frameReadyMs = 0.0f;
    float    totalGpuMs = 0.0f;  // the GPU frame: previous present to the end of compositor work
    // Blocked time on the game's thread this frame, EDVR's own clocks: the
    // period less these is the render thread's busy time (CPU TIME).
    float    presentWaitMs = 0.0f;
    float    posesWaitMs = 0.0f;
    uint8_t  reproj = 0;         // any reprojection reason
    uint8_t  motion = 0;         // motion smoothing
    uint8_t  dropped = 0;        // frames dropped at this sample
    uint8_t  haveComp = 0;
    // EDVR's part of the frame.
    uint16_t events = 0;
    float    eventMs = 0.0f;     // the longest event's own duration (a compile, a reload)
    float    cpuBoundaryMs = 0.0f;
    float    cpuDoorMs = 0.0f;
    float    cpuDrawsMs = 0.0f;  // the running sampled figure
    float    doorGpuMs = 0.0f;   // the last completed pair, both eyes
    // The game's own creations in the frame (device_hook.h), for the
    // long-frame line: a busy frame that made a hundred textures was
    // streaming, whatever else it looked like.
    uint32_t createTextures = 0;
    uint32_t createBuffers = 0;
    uint32_t createShaders = 0;
    float    createMb = 0.0f;
};

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

// The door's GPU bracket: a query ring per eye, the sharpen pass's shape.
struct QuerySlot {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* begin = nullptr;
    ID3D11Query* end = nullptr;
    bool         inUse = false;
    bool         begun = false;
};
constexpr int kQueryRing = 6;

struct State {
    Frame    ring[kRing];
    int      head = 0;          // next write
    int      count = 0;
    int64_t  lastQpc = 0;
    uint32_t lastSeq = 0;
    FrameTimingSample lastSample{};
    bool     haveSample = false;
    uint32_t frameNo = 0;

    // The frame in progress: events and their longest duration, the draw
    // sample, all cleared at the boundary.
    std::atomic<uint32_t> events{0};
    std::atomic<int32_t>  eventUs{0};
    bool     sampleDraws = false;
    int64_t  drawWholeTicks = 0;
    int64_t  drawRealTicks = 0;
    float    drawsMsRunning = 0.0f;
    bool     drawsSampled = false;

    // The door's GPU pairs.
    QuerySlot doorQ[2][kQueryRing];
    int       doorOpen[2] = {-1, -1};   // the slot begun and not yet ended
    float     doorGpuMs[2] = {0.0f, 0.0f};
    uint32_t  doorGpuSamples = 0;

    // The Present block noted by the swapchain hook, for the frame about
    // to be ringed.
    float    pendingPresentWaitMs = 0.0f;

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
FaultBudget g_doorBudget("perfMonitor.door", 4);

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
    for (uint32_t bit = 1; bit <= kEvMenu && len + 1 < n; bit <<= 1) {
        if (!(events & bit)) continue;
        const bool timed = ms > 0.0f && (bit == kEvCompile || bit == kEvReload || bit == kEvNgx);
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

// The door's query ring, polled at each Begin so nothing is ever awaited.
void pollDoor(ID3D11DeviceContext* ctx, int eye) {
    State& s = g_s;
    for (QuerySlot& q : s.doorQ[eye]) {
        if (!q.inUse) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (ctx->GetData(q.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 t0 = 0, t1 = 0;
        const HRESULT h0 = ctx->GetData(q.begin, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        const HRESULT h1 = ctx->GetData(q.end, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        q.inUse = false;
        if (dj.Disjoint || h0 != S_OK || h1 != S_OK || dj.Frequency == 0 || t1 < t0) continue;
        const double ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
        if (ms >= 0.0 && ms < 100.0) {
            s.doorGpuMs[eye] = static_cast<float>(ms);
            ++s.doorGpuSamples;
        }
    }
}

int acquireDoorSlot(ID3D11Device* dev, int eye) {
    for (int i = 0; i < kQueryRing; ++i) {
        QuerySlot& q = g_s.doorQ[eye][i];
        if (q.inUse) continue;
        if (!q.disjoint) {
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            D3D11_QUERY_DESC qt{};
            qt.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(dev->CreateQuery(&qd, &q.disjoint)) || FAILED(dev->CreateQuery(&qt, &q.begin)) ||
                FAILED(dev->CreateQuery(&qt, &q.end))) {
                if (q.disjoint) { q.disjoint->Release(); q.disjoint = nullptr; }
                if (q.begin) { q.begin->Release(); q.begin = nullptr; }
                if (q.end) { q.end->Release(); q.end = nullptr; }
                return -1;
            }
        }
        return i;
    }
    return -1;
}

bool deviceOf(void* tex, ID3D11Device** dev, ID3D11DeviceContext** ctx) {
    ID3D11Texture2D* t = nullptr;
    static_cast<IUnknown*>(tex)->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t));
    if (!t) return false;
    t->GetDevice(dev);
    t->Release();
    if (!*dev) return false;
    (*dev)->GetImmediateContext(ctx);
    if (!*ctx) {
        (*dev)->Release();
        *dev = nullptr;
        return false;
    }
    return true;
}

void dropLine(const Frame& f, float budgetMs) {
    State& s = g_s;
    if (s.dropLogged >= kDropLogMax || !dueMs(s.dropLogMs, kDropLogEveryMs)) return;
    s.dropLogMs = stampMs();
    ++s.dropLogged;
    char ev[200];
    eventList(f.events, f.eventMs, ev, sizeof(ev));
    char comp[260] = "the compositor's record has not settled yet";
    if (f.haveComp) {
        snprintf(comp, sizeof(comp),
                 "the compositor's record: GPU frame %.1f ms (scene %.1f, compositor %.1f), "
                 "poses at %.1f and submit at %.1f ms from vsync%s",
                 static_cast<double>(f.totalGpuMs), static_cast<double>(f.appGpuMs),
                 static_cast<double>(f.compGpuMs), static_cast<double>(f.posesReadyMs),
                 static_cast<double>(f.frameReadyMs), f.dropped ? "" : ", no drop reported");
    }
    const float busy = f.presentMs - f.presentWaitMs - f.posesWaitMs;
    Log::get().note(
        "monitor: %s -- %.1f ms between Presents (budget %.1f), of which the thread waited %.1f in "
        "Present and %.1f in WaitGetPoses (busy %.1f); %s; the game's creations in it: %u "
        "textures, %u buffers (%.1f MB together), %u shaders; EDVR this frame: boundary %.2f ms, "
        "door %.2f ms, draw hooks ~%.2f ms (sampled), door GPU %.2f ms; EDVR events: %s. At most "
        "one of these lines every %u s, %u a session.",
        f.dropped ? "DROPPED FRAME" : "LONG FRAME", static_cast<double>(f.presentMs),
        static_cast<double>(budgetMs), static_cast<double>(f.presentWaitMs),
        static_cast<double>(f.posesWaitMs), static_cast<double>(busy > 0.0f ? busy : 0.0f), comp,
        f.createTextures, f.createBuffers, static_cast<double>(f.createMb), f.createShaders,
        static_cast<double>(f.cpuBoundaryMs), static_cast<double>(f.cpuDoorMs),
        static_cast<double>(f.cpuDrawsMs), static_cast<double>(f.doorGpuMs), ev,
        static_cast<unsigned>(kDropLogEveryMs / 1000), kDropLogMax);
}

void noteDrop(const Frame& f, float budgetMs) {
    State& s = g_s;
    s.lastDropMs = nowMs();
    s.lastDropFrameMs = f.presentMs;
    s.lastDropEvents = f.events;
    s.lastDropEventMs = f.eventMs;
    dropLine(f, budgetMs);
}

float budgetNow() {
    const State& s = g_s;
    const float hz = s.haveSample && s.lastSample.displayHz > 0.0f ? s.lastSample.displayHz : 0.0f;
    return hz > 0.0f ? 1000.0f / hz : 11.1f;
}

}  // namespace

void perfMonitorFrame(ID3D11Device* dev) {
    State& s = g_s;
    ++s.frameNo;
    Frame f;
    const int64_t q = qpcNow();
    if (s.lastQpc && qpcFrequency() > 0) {
        const double ms = static_cast<double>(q - s.lastQpc) * 1000.0 / static_cast<double>(qpcFrequency());
        f.presentMs = ms > 0.0 && ms < 5000.0 ? static_cast<float>(ms) : 0.0f;
    }
    s.lastQpc = q;
    // The frame's waits: Present's, noted by the swapchain hook a moment
    // ago; WaitGetPoses's, over the channel.
    f.presentWaitMs = s.pendingPresentWaitMs;
    s.pendingPresentWaitMs = 0.0f;
    f.posesWaitMs = static_cast<float>(takeWaitCpuUs()) / 1000.0f;
    // EDVR's part: the events of the frame just ending, from both halves.
    const uint32_t ev = s.events.exchange(0) | takeEdvrEvents();
    f.events = static_cast<uint16_t>(ev & 0xFFFFu);
    f.eventMs = static_cast<float>(s.eventUs.exchange(0)) / 1000.0f;
    f.cpuDoorMs = static_cast<float>(takeDoorCpuUs()) / 1000.0f;
    if (s.sampleDraws && qpcFrequency() > 0) {
        const int64_t own = s.drawWholeTicks - s.drawRealTicks;
        s.drawsMsRunning = own > 0 ? static_cast<float>(static_cast<double>(own) * 1000.0 /
                                                        static_cast<double>(qpcFrequency()))
                                   : 0.0f;
        s.drawsSampled = true;
    }
    f.cpuDrawsMs = s.drawsMsRunning;
    s.drawWholeTicks = s.drawRealTicks = 0;
    s.sampleDraws = (s.frameNo % kDrawSampleEvery) == 0;
    f.doorGpuMs = s.doorGpuMs[0] + s.doorGpuMs[1];
    const DeviceCreates made = deviceCreatesTake();
    f.createTextures = made.textures;
    f.createBuffers = made.buffers;
    f.createShaders = made.shaders;
    f.createMb = static_cast<float>(static_cast<double>(made.textureBytes + made.bufferBytes) / 1048576.0);
    ringPush(f);

    // The compositor's word describes the frame kFrameTimingLag frames
    // back -- the settled record -- so it is written into THAT entry, next
    // to the EDVR events of the same frame. The first build wrote it into
    // the newest entry, which is why drops landed on whatever EDVR happened
    // to be doing two frames later (the Monitor page's own raster upload,
    // four times a second, took the blame for a steady share).
    Frame* settled = nullptr;
    FrameTimingSample sample{};
    uint32_t seq = 0;
    if (frameTimingSample(&sample, &seq) && seq != s.lastSeq) {
        uint8_t dropped = 0;
        if (s.haveSample && sample.droppedTotal > s.lastSample.droppedTotal) {
            const uint32_t d = sample.droppedTotal - s.lastSample.droppedTotal;
            dropped = d > 255 ? 255 : static_cast<uint8_t>(d);
        }
        s.lastSeq = seq;
        s.lastSample = sample;
        s.haveSample = true;
        if (s.count > static_cast<int>(kFrameTimingLag)) {
            settled = &ringAt(s.count - 1 - static_cast<int>(kFrameTimingLag));
            settled->appGpuMs = sample.appGpuMs;
            settled->compGpuMs = sample.compGpuMs;
            settled->totalGpuMs = sample.totalGpuMs;
            settled->compCpuMs = sample.compCpuMs;
            settled->cpuFrameMs = sample.cpuFrameMs;
            settled->appCpuMs = sample.appCpuMs;
            settled->posesReadyMs = sample.posesReadyMs;
            settled->frameReadyMs = sample.frameReadyMs;
            settled->reproj = (sample.reprojFlags & 0x0Fu) ? 1 : 0;
            settled->motion = (sample.reprojFlags & 0x08u) ? 1 : 0;
            settled->dropped = dropped;
            settled->haveComp = 1;
        }
    }

    // A drop the compositor reports for the settled frame, or a frame our
    // own clock calls long: the page's "last drop" and the rate-limited
    // log line.
    const float budget = budgetNow();
    if (settled && settled->dropped) {
        noteDrop(*settled, budget);
    } else if (f.presentMs > 2.0f * budget && f.presentMs < 5000.0f) {
        noteDrop(*ringLast(), budget);
    }

    if (s.active || (s.activeUntilMs && nowMs() < s.activeUntilMs)) {
        if (dueMs(s.slowMs, kSlowEveryMs)) {
            s.slowMs = stampMs();
            guardedBudget(g_budget, [&] { slowSample(dev); });
        }
    }
}

void perfMonitorNoteEvent(uint32_t bits, double ms) {
    State& s = g_s;
    s.events.fetch_or(bits, std::memory_order_relaxed);
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
    else if (which == kCpuDoor) f->cpuDoorMs += static_cast<float>(ms);
}

void perfMonitorNotePresentWait(double ms) {
    if (ms >= 0.0 && ms < 5000.0) g_s.pendingPresentWaitMs = static_cast<float>(ms);
}

bool perfMonitorSampleDraws() { return g_s.sampleDraws; }

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
    float present[kRing], appGpu[kRing], compGpu[kRing], gpuFrame[kRing], busy[kRing], waits[kRing];
    int cnt = 0, compCnt = 0, reproj = 0, dropped = 0;
    // The mean over fpsVR's own update window, so the two numbers can be
    // read side by side. Averaging the whole ten-second ring instead read
    // about a millisecond under it (flown 2026-09-07).
    double matchGpu = 0.0, matchCpu = 0.0, matchApp = 0.0;
    int matchGpuN = 0, matchCpuN = 0, matchAppN = 0;
    float matchSecs = 0.0f;
    for (int i = s.count - 1; i >= 0 && matchSecs < kMatchWindowS; --i) {
        const Frame& f = ringAt(i);
        if (f.haveComp) {
            matchGpu += f.totalGpuMs;
            ++matchGpuN;
            if (f.appCpuMs > 0.0f) {
                matchApp += f.appCpuMs;
                ++matchAppN;
            }
        }
        const float b = f.presentMs - f.presentWaitMs - f.posesWaitMs;
        if (b > 0.0f) {
            matchCpu += b;
            ++matchCpuN;
        }
        matchSecs += f.presentMs / 1000.0f;
    }
    const float recentGpu = matchGpuN ? static_cast<float>(matchGpu / matchGpuN) : 0.0f;
    const float recentCpu = matchCpuN ? static_cast<float>(matchCpu / matchCpuN) : 0.0f;
    const float recentAppCpu = matchAppN ? static_cast<float>(matchApp / matchAppN) : 0.0f;
    int dropsWithEdvr = 0, dropsClean = 0;
    uint32_t dropEventBits = 0;
    double edvrBoundary = 0.0, edvrDoor = 0.0, doorGpu = 0.0;
    int doorGpuN = 0;
    for (int i = 0; i < s.count; ++i) {
        const Frame& f = ringAt(i);
        present[cnt] = f.presentMs;
        const float b = f.presentMs - f.presentWaitMs - f.posesWaitMs;
        busy[cnt] = b > 0.0f ? b : 0.0f;
        waits[cnt] = f.presentWaitMs + f.posesWaitMs;
        ++cnt;
        if (f.haveComp) {
            // The GPU frame is the record's total, and the app's share is the
            // scene's own GPU work, which the record reports directly.
            appGpu[compCnt] = f.appGpuMs;
            compGpu[compCnt] = f.compGpuMs;
            gpuFrame[compCnt] = f.totalGpuMs;
            ++compCnt;
            reproj += f.reproj;
        }
        dropped += f.dropped;
        edvrBoundary += f.cpuBoundaryMs;
        edvrDoor += f.cpuDoorMs;
        if (f.doorGpuMs > 0.0f) {
            doorGpu += f.doorGpuMs;
            ++doorGpuN;
        }
        if (f.dropped) {
            if (f.events) {
                ++dropsWithEdvr;
                dropEventBits |= f.events;
            } else {
                ++dropsClean;
            }
        }
    }
    const PerfStats ps = perfStatsOf(present, cnt);
    const float hz = s.haveSample && s.lastSample.displayHz > 0.0f ? s.lastSample.displayHz : 0.0f;
    const float budget = hz > 0.0f ? 1000.0f / hz : 11.1f;
    const float windowS = ps.count ? ps.count * ps.avgMs / 1000.0f : 0.0f;

    // Row 1: the frame, as fpsVR reports it. Both come from Valve's own
    // worked example on the Compositor_FrameTiming page: the GPU frame is
    // the record's total, and the app's CPU frame is the poses-to-submit
    // window plus the compositor's own submit cost. EDVR's own measure of
    // the render thread -- the period less the time blocked in
    // WaitGetPoses and in Present -- goes on the CPU tile's sub-line,
    // because it is the larger number and the useful one: it includes the
    // work after the submit, which fpsVR's window does not.
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
    if (compCnt) {
        const PerfStats gf = perfStatsOf(gpuFrame, compCnt);
        snprintf(v, sizeof(v), "%.1f", recentGpu > 0.0f ? recentGpu : gf.avgMs);
        snprintf(sub, sizeof(sub), "ms now; %.1f over 10 s", gf.avgMs);
        tile("GPU TIME", v, sub);
    } else {
        tile("GPU TIME", "--", glitchConsumerPresent() ? "no compositor timing" : "no openvr half");
    }
    if (ps.count) {
        const PerfStats bs = perfStatsOf(busy, cnt);
        if (recentAppCpu > 0.0f) {
            snprintf(v, sizeof(v), "%.1f", recentAppCpu);
            snprintf(sub, sizeof(sub), "ms app; %.1f thread", bs.avgMs);
        } else {
            // No usable stamps: fall back to our own, and say which it is.
            snprintf(v, sizeof(v), "%.1f", recentCpu > 0.0f ? recentCpu : bs.avgMs);
            snprintf(sub, sizeof(sub), "ms thread; no app stamps");
        }
        tile("CPU TIME", v, sub);
    } else {
        tile("CPU TIME", "--", "");
    }

    // Row 2: the app's GPU share, and the drops.
    if (compCnt) {
        const PerfStats ag = perfStatsOf(appGpu, compCnt);
        const PerfStats cg = perfStatsOf(compGpu, compCnt);
        snprintf(v, sizeof(v), "%.1f", ag.avgMs);
        snprintf(sub, sizeof(sub), "ms scene; %.1f compositor", cg.avgMs);
        tile("APP GPU", v, sub);
    } else {
        tile("APP GPU", "--", "");
    }
    if (compCnt) {
        snprintf(v, sizeof(v), "%d", dropped);
        snprintf(sub, sizeof(sub), "in %.0f s, %u total", windowS, s.haveSample ? s.lastSample.droppedTotal : 0u);
        tile("DROPPED", v, sub);
        if (dropped) {
            char ev[24];
            eventList(static_cast<uint16_t>(dropEventBits), 0.0f, ev, sizeof(ev));
            snprintf(v, sizeof(v), "%d / %d", dropsWithEdvr, dropsClean);
            snprintf(sub, sizeof(sub), "EDVR / clean; %s", dropsWithEdvr ? ev : "none");
            tile("BY CAUSE", v, sub);
        } else {
            tile("BY CAUSE", "--", "no drops");
        }
        snprintf(v, sizeof(v), "%.0f%%", 100.0f * static_cast<float>(reproj) / static_cast<float>(compCnt));
        tile("REPROJECTED", v, "of frames");
    } else {
        tile("DROPPED", "--", "no compositor timing");
        tile("BY CAUSE", "--", "");
        tile("REPROJECTED", "--", "");
    }

    // Row 3: the display and the machine.
    {
        uint32_t ew = 0, eh = 0;
        const bool haveEye = eyeTextureSize(&ew, &eh);
        if (hz > 0.0f) {
            snprintf(v, sizeof(v), "%.0f Hz", hz);
            if (haveEye) snprintf(sub, sizeof(sub), "%.1f ms budget, %ux%u", budget, ew, eh);
            else snprintf(sub, sizeof(sub), "%.1f ms budget", budget);
        } else if (haveEye) {
            snprintf(v, sizeof(v), "--");
            snprintf(sub, sizeof(sub), "%ux%u per eye", ew, eh);
        } else {
            snprintf(v, sizeof(v), "--");
            snprintf(sub, sizeof(sub), "not published yet");
        }
        tile("DISPLAY", v, sub);
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
        if (doorGpuN) {
            snprintf(v, sizeof(v), "%.2f", doorGpu / doorGpuN);
            snprintf(sub, sizeof(sub), "ms/frame at the door");
        } else {
            snprintf(v, sizeof(v), "--");
            snprintf(sub, sizeof(sub), glitchConsumerPresent() ? "no pair yet" : "no openvr half");
        }
        tile("EDVR GPU", v, sub);
        const double total = edvrBoundary / frames + edvrDoor / frames +
                             (s.drawsSampled ? static_cast<double>(s.drawsMsRunning) : 0.0);
        snprintf(v, sizeof(v), "%.2f", total);
        if (s.drawsSampled) {
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
        if (temporalPassDlaaTotals(&t, &avg, &mx, &resets) && t) {
            temporal = avg;
            trained = true;
        } else if (temporalPassTotals(&t, &avg, &mx, &rej, &clip) && t) {
            temporal = avg;
        }
        if (sharpenPassTotals(&t, &avg, &mx) && t) sharpen = avg;
        if (temporal >= 0.0 && sharpen >= 0.0) {
            snprintf(v, sizeof(v), "%.1f+%.1f", temporal, sharpen);
            snprintf(sub, sizeof(sub), "ms/eye %s + sharpen", trained ? "NVIDIA" : "temporal");
        } else if (temporal >= 0.0) {
            snprintf(v, sizeof(v), "%.2f", temporal);
            snprintf(sub, sizeof(sub), "ms/eye, %s pass", trained ? "NVIDIA's" : "temporal");
        } else if (sharpen >= 0.0) {
            snprintf(v, sizeof(v), "%.2f", sharpen);
            snprintf(sub, sizeof(sub), "ms/eye, sharpen");
        } else {
            snprintf(v, sizeof(v), "--");
            snprintf(sub, sizeof(sub), "no pass running");
        }
        tile("EDVR PASSES", v, sub);
    }
    return n;
}
void perfMonitorLastDropLine(char* buf, size_t bufLen) {
    State& s = g_s;
    if (!buf || !bufLen) return;
    if (!s.lastDropMs) {
        snprintf(buf, bufLen, "No dropped or long frame yet this session.");
        return;
    }
    char ev[120];
    eventList(s.lastDropEvents, s.lastDropEventMs, ev, sizeof(ev));
    const uint64_t ago = nowMs() - s.lastDropMs;
    snprintf(buf, bufLen, "Last drop %.0f s ago: a %.1f ms frame. EDVR events in it: %s.",
             static_cast<double>(ago) / 1000.0, static_cast<double>(s.lastDropFrameMs), ev);
    buf[bufLen - 1] = 0;
}
int perfMonitorGraph(int which, float* out, int max, float* budgetMs) {
    State& s = g_s;
    if (budgetMs) *budgetMs = budgetNow();
    if (!out || max <= 0) return 0;
    const int n = s.count < max ? s.count : max;
    for (int i = 0; i < n; ++i) {
        const Frame& f = ringAt(s.count - n + i);
        if (which == kGraphGpu) {
            // A frame whose record has not settled plots as a gap, not as
            // a zero: two frames at the young end always lack one.
            out[i] = f.haveComp ? f.totalGpuMs : 0.0f;
        } else if (which == kGraphCpu) {
            const float b = f.presentMs - f.presentWaitMs - f.posesWaitMs;
            out[i] = b > 0.0f ? b : 0.0f;
        } else {
            out[i] = f.presentMs;
        }
    }
    return n;
}

void perfMonitorOverlayLine(char* buf, size_t bufLen) {
    State& s = g_s;
    if (!buf || !bufLen) return;
    // The last second of intervals, the last ten of drops; the GPU frame
    // (the compositor's total, fpsVR's GPU frametime) from the settled
    // records in that second, and the render thread's busy time.
    float present[kRing], gpuFrame[kRing], busy[kRing];
    int n = 0, gpuN = 0, dropped = 0;
    float secs = 0.0f;
    for (int i = s.count - 1; i >= 0; --i) {
        const Frame& f = ringAt(i);
        dropped += f.dropped;
        if (secs < 1.0f) {
            present[n] = f.presentMs;
            const float b = f.presentMs - f.presentWaitMs - f.posesWaitMs;
            busy[n] = b > 0.0f ? b : 0.0f;
            ++n;
            if (f.haveComp) gpuFrame[gpuN++] = f.totalGpuMs;
            secs += f.presentMs / 1000.0f;
        }
    }
    const PerfStats ps = perfStatsOf(present, n);
    if (!ps.count) {
        snprintf(buf, bufLen, "measuring");
        return;
    }
    char times[80] = "";
    if (gpuN) {
        snprintf(times, sizeof(times), "   gpu %.1f   cpu %.1f", perfStatsOf(gpuFrame, gpuN).avgMs,
                 perfStatsOf(busy, n).avgMs);
    } else {
        snprintf(times, sizeof(times), "   %.1f ms   cpu %.1f", ps.avgMs, perfStatsOf(busy, n).avgMs);
    }
    char drop[40] = "";
    if (dropped) snprintf(drop, sizeof(drop), "   %d dropped", dropped);
    snprintf(buf, bufLen, "%.0f fps%s%s", perfFpsOf(ps.avgMs), times, drop);
    buf[bufLen - 1] = 0;
}

void perfMonitorShutdown() {
    State& s = g_s;
    if (s.adapter3) {
        s.adapter3->Release();
        s.adapter3 = nullptr;
    }
    for (int e = 0; e < 2; ++e) {
        for (QuerySlot& q : s.doorQ[e]) {
            if (q.disjoint) { q.disjoint->Release(); q.disjoint = nullptr; }
            if (q.begin) { q.begin->Release(); q.begin = nullptr; }
            if (q.end) { q.end->Release(); q.end = nullptr; }
            q.inUse = false;
        }
    }
    if (s.dropLogged) {
        Log::get().note("monitor: %u dropped or long frames were logged this session (of %u at most).",
                        s.dropLogged, kDropLogMax);
    }
}

}  // namespace edvr

extern "C" __declspec(dllexport) void edvrDoorGpuBegin(void* tex, int eye) {
    using namespace edvr;
    if (!tex || eye < 0 || eye > 1) return;
    State& s = g_s;
    if (s.doorOpen[eye] >= 0) return;   // a pair already open: the End never came; leave it
    guardedBudget(g_doorBudget, [&] {
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        if (!deviceOf(tex, &dev, &ctx)) return;
        pollDoor(ctx, eye);
        const int slot = acquireDoorSlot(dev, eye);
        if (slot >= 0) {
            QuerySlot& q = s.doorQ[eye][slot];
            ctx->Begin(q.disjoint);
            ctx->End(q.begin);
            q.begun = true;
            s.doorOpen[eye] = slot;
        }
        ctx->Release();
        dev->Release();
    });
}

extern "C" __declspec(dllexport) void edvrDoorGpuEnd(void* tex, int eye) {
    using namespace edvr;
    if (!tex || eye < 0 || eye > 1) return;
    State& s = g_s;
    const int slot = s.doorOpen[eye];
    if (slot < 0) return;
    s.doorOpen[eye] = -1;
    guardedBudget(g_doorBudget, [&] {
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        if (!deviceOf(tex, &dev, &ctx)) return;
        QuerySlot& q = s.doorQ[eye][slot];
        if (q.begun) {
            ctx->End(q.end);
            ctx->End(q.disjoint);
            q.begun = false;
            q.inUse = true;
        }
        ctx->Release();
        dev->Release();
    });
}
