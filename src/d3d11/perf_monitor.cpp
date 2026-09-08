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
#include "sharpen_pass.h"
#include "temporal_pass.h"

namespace edvr {
namespace {

// Ten seconds at 90 Hz for the statistics; the graph shows the tail.
constexpr int kRing = 900;
constexpr uint64_t kSlowEveryMs = 1000;
constexpr uint64_t kGraceMs = 2000;
// One frame in sixteen samples the draw hooks.
constexpr uint32_t kDrawSampleEvery = 16;
// The drop log line: at most one every five seconds, sixty a session.
constexpr uint64_t kDropLogEveryMs = 5000;
constexpr uint32_t kDropLogMax = 60;

struct Frame {
    float    presentMs = 0.0f;   // Present to Present
    float    appGpuMs = 0.0f;    // the compositor's word, when published
    float    compGpuMs = 0.0f;
    float    cpuFrameMs = 0.0f;
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
    Log::get().note(
        "monitor: %s -- %.1f ms between Presents (budget %.1f), the compositor's app GPU %.1f ms%s; "
        "EDVR this frame: boundary %.2f ms, door %.2f ms, draw hooks ~%.2f ms (sampled), door GPU "
        "%.2f ms; EDVR events: %s. At most one of these lines every %u s, %u a session.",
        f.dropped ? "DROPPED FRAME" : "LONG FRAME", static_cast<double>(f.presentMs),
        static_cast<double>(budgetMs), static_cast<double>(f.appGpuMs),
        f.dropped ? "" : " (the compositor reports no drop)", static_cast<double>(f.cpuBoundaryMs),
        static_cast<double>(f.cpuDoorMs), static_cast<double>(f.cpuDrawsMs),
        static_cast<double>(f.doorGpuMs), ev, static_cast<unsigned>(kDropLogEveryMs / 1000),
        kDropLogMax);
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
    FrameTimingSample sample{};
    uint32_t seq = 0;
    if (frameTimingSample(&sample, &seq)) {
        if (seq != s.lastSeq) {
            if (s.haveSample && sample.droppedTotal > s.lastSample.droppedTotal) {
                const uint32_t d = sample.droppedTotal - s.lastSample.droppedTotal;
                f.dropped = d > 255 ? 255 : static_cast<uint8_t>(d);
            }
            s.lastSeq = seq;
            s.lastSample = sample;
            s.haveSample = true;
        }
        f.appGpuMs = sample.appGpuMs;
        f.compGpuMs = sample.compGpuMs;
        f.cpuFrameMs = sample.cpuFrameMs;
        f.reproj = (sample.reprojFlags & 0x0Fu) ? 1 : 0;
        f.motion = (sample.reprojFlags & 0x08u) ? 1 : 0;
        f.haveComp = 1;
    }
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
    ringPush(f);

    // A drop, or a frame our own clock calls long: the page's "last drop"
    // and the rate-limited log line.
    const float budget = budgetNow();
    const bool longFrame = f.presentMs > 2.0f * budget && f.presentMs < 5000.0f;
    if (f.dropped || longFrame) {
        s.lastDropMs = nowMs();
        s.lastDropFrameMs = f.presentMs;
        s.lastDropEvents = f.events;
        s.lastDropEventMs = f.eventMs;
        dropLine(f, budget);
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

int perfMonitorLines(PerfLine* out, int max) {
    State& s = g_s;
    if (!out || max <= 0) return 0;
    int n = 0;
    auto line = [&](const char* l, const char* r) {
        if (n >= max) return;
        strncpy(out[n].left, l, sizeof(out[n].left) - 1);
        out[n].left[sizeof(out[n].left) - 1] = 0;
        strncpy(out[n].right, r, sizeof(out[n].right) - 1);
        out[n].right[sizeof(out[n].right) - 1] = 0;
        ++n;
    };
    char buf[200];

    // The interval ring, summarised, and the drops attributed.
    float present[kRing], appGpu[kRing], compGpu[kRing], cpuFrame[kRing];
    int cnt = 0, compCnt = 0, reproj = 0, motion = 0, dropped = 0;
    int dropsWithEdvr = 0, dropsClean = 0;
    uint32_t dropEventBits = 0;
    double edvrBoundary = 0.0, edvrDoor = 0.0, doorGpu = 0.0;
    int doorGpuN = 0;
    for (int i = 0; i < s.count; ++i) {
        const Frame& f = ringAt(i);
        present[cnt++] = f.presentMs;
        if (f.haveComp) {
            appGpu[compCnt] = f.appGpuMs;
            compGpu[compCnt] = f.compGpuMs;
            cpuFrame[compCnt] = f.cpuFrameMs;
            ++compCnt;
            reproj += f.reproj;
            motion += f.motion;
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

    if (ps.count) {
        snprintf(buf, sizeof(buf), "%.1f fps, %.1f ms  (1%% low %.0f fps, max %.1f ms)",
                 perfFpsOf(ps.avgMs), ps.avgMs, perfFpsOf(ps.p99Ms), ps.maxMs);
    } else {
        snprintf(buf, sizeof(buf), "measuring");
    }
    line("Frame rate", buf);

    if (compCnt) {
        const PerfStats ag = perfStatsOf(appGpu, compCnt);
        const PerfStats cg = perfStatsOf(compGpu, compCnt);
        snprintf(buf, sizeof(buf), "%.1f ms avg, max %.1f  (compositor %.1f ms)", ag.avgMs, ag.maxMs,
                 cg.avgMs);
        line("App GPU", buf);
        const PerfStats cf = perfStatsOf(cpuFrame, compCnt);
        snprintf(buf, sizeof(buf), "%.1f ms between poses, present wait %.1f ms", cf.avgMs,
                 s.lastSample.presentCpuMs);
        line("App CPU", buf);
        char attr[120] = "";
        if (dropped) {
            char ev[80];
            eventList(static_cast<uint16_t>(dropEventBits), 0.0f, ev, sizeof(ev));
            snprintf(attr, sizeof(attr), " -- %d during EDVR activity (%s), %d clean", dropsWithEdvr,
                     dropsWithEdvr ? ev : "none", dropsClean);
        }
        snprintf(buf, sizeof(buf), "%d in %.0f s (%u since launch), %.0f%% reprojected%s", dropped,
                 windowS, s.haveSample ? s.lastSample.droppedTotal : 0u,
                 100.0f * static_cast<float>(reproj) / static_cast<float>(compCnt), attr);
        line("Dropped frames", buf);
        (void)motion;
    } else {
        line("App GPU", glitchConsumerPresent() ? "compositor timing not available" : "no openvr half");
    }

    // The last drop, or long frame, with what EDVR was doing.
    if (s.lastDropMs) {
        char ev[120];
        eventList(s.lastDropEvents, s.lastDropEventMs, ev, sizeof(ev));
        const uint64_t ago = nowMs() - s.lastDropMs;
        snprintf(buf, sizeof(buf), "%.0f s ago: %.1f ms frame, EDVR events: %s",
                 static_cast<double>(ago) / 1000.0, static_cast<double>(s.lastDropFrameMs), ev);
    } else {
        snprintf(buf, sizeof(buf), "none yet");
    }
    line("Last drop", buf);

    // EDVR's own cost per frame.
    {
        const double frames = cnt > 0 ? static_cast<double>(cnt) : 1.0;
        char draws[48];
        if (s.drawsSampled) snprintf(draws, sizeof(draws), ", draw hooks ~%.2f ms (sampled)",
                                     static_cast<double>(s.drawsMsRunning));
        else snprintf(draws, sizeof(draws), ", draw hooks not sampled yet");
        snprintf(buf, sizeof(buf), "boundary %.2f ms, door %.2f ms%s", edvrBoundary / frames,
                 edvrDoor / frames, draws);
        line("EDVR CPU", buf);
        if (doorGpuN) {
            snprintf(buf, sizeof(buf), "door %.2f ms/frame (both eyes, %u pairs measured)",
                     doorGpu / doorGpuN, s.doorGpuSamples);
        } else {
            snprintf(buf, sizeof(buf), glitchConsumerPresent() ? "door: no pair measured yet"
                                                               : "door: no openvr half");
        }
        line("EDVR GPU", buf);
    }

    uint32_t ew = 0, eh = 0;
    if (hz > 0.0f && eyeTextureSize(&ew, &eh)) {
        snprintf(buf, sizeof(buf), "%.0f Hz, budget %.1f ms, %ux%u per eye", hz, budget, ew, eh);
    } else if (eyeTextureSize(&ew, &eh)) {
        snprintf(buf, sizeof(buf), "%ux%u per eye", ew, eh);
    } else {
        snprintf(buf, sizeof(buf), "not published yet");
    }
    line("Display", buf);

    if (s.cpuSystemPct >= 0.0f) {
        snprintf(buf, sizeof(buf), "system %.0f%%, Elite %.0f%% of %u threads", s.cpuSystemPct,
                 s.cpuProcessPct >= 0.0f ? s.cpuProcessPct : 0.0f, s.cpuThreads);
    } else {
        snprintf(buf, sizeof(buf), "sampling");
    }
    line("CPU", buf);

    {
        char vu[24] = "?", vb[24] = "?";
        if (s.vramBudget) {
            gb(vu, sizeof(vu), s.vramUsed);
            gb(vb, sizeof(vb), s.vramBudget);
        }
        if (s.nvOk) {
            if (s.gpuTempC > -1000) {
                snprintf(buf, sizeof(buf), "%d%% load, %d C, VRAM %s of %s GB", s.gpuLoadPct, s.gpuTempC, vu, vb);
            } else {
                snprintf(buf, sizeof(buf), "%d%% load, VRAM %s of %s GB", s.gpuLoadPct, vu, vb);
            }
        } else {
            snprintf(buf, sizeof(buf), "load %s, VRAM %s of %s GB", s.nvTried ? s.nvWhy : "sampling", vu, vb);
        }
        line("GPU", buf);
    }
    {
        char rp[24] = "?", ra[24] = "?", rt[24] = "?";
        if (s.ramTotal) {
            gb(rp, sizeof(rp), s.ramProcess);
            gb(ra, sizeof(ra), s.ramAvail);
            gb(rt, sizeof(rt), s.ramTotal);
        }
        snprintf(buf, sizeof(buf), "Elite %s GB, %s GB free of %s", rp, ra, rt);
        line("RAM", buf);
    }
    {
        uint32_t t = 0, resets = 0;
        double avg = 0.0, mx = 0.0, rej = 0.0, clip = 0.0;
        std::string passes;
        char part[64];
        if (temporalPassDlaaTotals(&t, &avg, &mx, &resets) && t) {
            snprintf(part, sizeof(part), "NVIDIA %.2f ms/eye", avg);
            passes += part;
        } else if (temporalPassTotals(&t, &avg, &mx, &rej, &clip) && t) {
            snprintf(part, sizeof(part), "temporal %.2f ms/eye", avg);
            passes += part;
        }
        if (sharpenPassTotals(&t, &avg, &mx) && t) {
            snprintf(part, sizeof(part), "%ssharpen %.2f ms/eye", passes.empty() ? "" : ", ", avg);
            passes += part;
        }
        line("EDVR passes", passes.empty() ? "none running" : passes.c_str());
    }
    return n;
}

int perfMonitorGraph(float* out, int max, float* budgetMs) {
    State& s = g_s;
    if (budgetMs) *budgetMs = budgetNow();
    if (!out || max <= 0) return 0;
    const int n = s.count < max ? s.count : max;
    for (int i = 0; i < n; ++i) out[i] = ringAt(s.count - n + i).presentMs;
    return n;
}

void perfMonitorOverlayLine(char* buf, size_t bufLen) {
    State& s = g_s;
    if (!buf || !bufLen) return;
    // The last second of intervals, the last ten of drops.
    float present[kRing];
    float appGpu[kRing];
    int n = 0, gpuN = 0, dropped = 0;
    float secs = 0.0f;
    for (int i = s.count - 1; i >= 0; --i) {
        const Frame& f = ringAt(i);
        dropped += f.dropped;
        if (secs < 1.0f) {
            present[n++] = f.presentMs;
            if (f.haveComp) appGpu[gpuN++] = f.appGpuMs;
            secs += f.presentMs / 1000.0f;
        }
    }
    const PerfStats ps = perfStatsOf(present, n);
    if (!ps.count) {
        snprintf(buf, bufLen, "measuring");
        return;
    }
    char gpu[40] = "";
    if (gpuN) {
        const PerfStats ag = perfStatsOf(appGpu, gpuN);
        snprintf(gpu, sizeof(gpu), "   gpu %.1f", ag.avgMs);
    }
    char drop[40] = "";
    if (dropped) snprintf(drop, sizeof(drop), "   %d dropped", dropped);
    snprintf(buf, bufLen, "%.0f fps   %.1f ms%s%s", perfFpsOf(ps.avgMs), ps.avgMs, gpu, drop);
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
