#include "perf_monitor.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <psapi.h>

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

struct Frame {
    float   presentMs = 0.0f;   // Present to Present
    float   appGpuMs = 0.0f;    // the compositor's word, when published
    float   compGpuMs = 0.0f;
    float   cpuFrameMs = 0.0f;
    uint8_t reproj = 0;         // any reprojection reason
    uint8_t motion = 0;         // motion smoothing
    uint8_t dropped = 0;        // frames dropped at this sample
    uint8_t haveComp = 0;
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

struct State {
    Frame    ring[kRing];
    int      head = 0;          // next write
    int      count = 0;
    int64_t  lastQpc = 0;
    uint32_t lastSeq = 0;
    FrameTimingSample lastSample{};
    bool     haveSample = false;
    uint32_t droppedAtRingStart = 0;

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

const Frame& ringAt(int i) {   // 0 = oldest
    const int start = (g_s.head - g_s.count + kRing) % kRing;
    return g_s.ring[(start + i) % kRing];
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
                // The system total counts all cores; the process share of
                // it is the share of all cores.
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

}  // namespace

void perfMonitorFrame(ID3D11Device* dev) {
    State& s = g_s;
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
    ringPush(f);

    if (s.active || (s.activeUntilMs && nowMs() < s.activeUntilMs)) {
        if (dueMs(s.slowMs, kSlowEveryMs)) {
            s.slowMs = stampMs();
            guardedBudget(g_budget, [&] { slowSample(dev); });
        }
    }
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
    char buf[160];

    // The interval ring, summarised.
    float present[kRing], appGpu[kRing], compGpu[kRing], cpuFrame[kRing];
    int cnt = 0, compCnt = 0, reproj = 0, motion = 0, dropped = 0;
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
    }
    const PerfStats ps = perfStatsOf(present, cnt);
    const float hz = s.haveSample && s.lastSample.displayHz > 0.0f ? s.lastSample.displayHz : 0.0f;
    const float budget = hz > 0.0f ? 1000.0f / hz : 11.1f;

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
        snprintf(buf, sizeof(buf), "%d in %.0f s (%u since launch), %.0f%% reprojected, %.0f%% smoothed",
                 dropped, ps.count ? ps.count * ps.avgMs / 1000.0f : 0.0f,
                 s.haveSample ? s.lastSample.droppedTotal : 0u,
                 100.0f * static_cast<float>(reproj) / static_cast<float>(compCnt),
                 100.0f * static_cast<float>(motion) / static_cast<float>(compCnt));
        line("Dropped frames", buf);
    } else {
        line("App GPU", glitchConsumerPresent() ? "compositor timing not available" : "no openvr half");
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
    if (budgetMs) {
        const float hz = s.haveSample && s.lastSample.displayHz > 0.0f ? s.lastSample.displayHz : 0.0f;
        *budgetMs = hz > 0.0f ? 1000.0f / hz : 11.1f;
    }
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
    if (g_s.adapter3) {
        g_s.adapter3->Release();
        g_s.adapter3 = nullptr;
    }
}

}  // namespace edvr
