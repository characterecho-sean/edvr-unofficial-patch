#include "frame_timing.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/timing.h"
#include "openvr_min.h"
#include "system_hook.h"

namespace edvr {
namespace {

constexpr size_t kSlotGetFrameTiming = 8;
constexpr size_t kSysSlotFloatProp = 22;
constexpr int32_t kPropDisplayFrequency = 2002;

typedef bool (*PFN_GetFrameTiming)(void* self, void* timing, uint32_t framesAgo);
typedef float (*PFN_FloatProp)(void* self, uint32_t device, int32_t prop, int32_t* error);

// Compositor_FrameTiming as openvr.h laid it out in the 1.0 era (176
// bytes), with the two vsync counts the later layout appends (184). The
// fields read are the ones both share.
struct FrameTimingRaw {
    uint32_t size;
    uint32_t frameIndex;
    uint32_t numFramePresents;
    uint32_t numMisPresented;
    uint32_t numDroppedFrames;
    uint32_t reprojectionFlags;
    double   systemTimeInSeconds;
    float    preSubmitGpuMs;
    float    postSubmitGpuMs;
    float    totalRenderGpuMs;
    float    compositorRenderGpuMs;
    float    compositorRenderCpuMs;
    float    compositorIdleCpuMs;
    float    clientFrameIntervalMs;
    float    presentCallCpuMs;
    float    waitForPresentCpuMs;
    float    submitFrameMs;
    float    waitGetPosesCalledMs;
    float    newPosesReadyMs;
    float    newFrameReadyMs;
    float    compositorUpdateStartMs;
    float    compositorUpdateEndMs;
    float    compositorRenderStartMs;
    vr::TrackedDevicePose_t hmdPose;
    uint32_t numVSyncsReadyForUse;
    uint32_t numVSyncsToFirstView;
};
static_assert(offsetof(FrameTimingRaw, numVSyncsReadyForUse) == 176,
              "the 1.0-era Compositor_FrameTiming is 176 bytes");
static_assert(sizeof(FrameTimingRaw) == 184, "the later layout is 184 bytes");

struct State {
    bool     on = true;
    bool     configured = false;
    bool     standDown = false;
    bool     armed = false;         // the first answer validated
    uint32_t sizeUsed = 0;          // 176 or 184, once one worked
    uint32_t lastIndex = 0;
    uint32_t droppedTotal = 0;
    uint32_t published = 0;
    uint32_t refusals = 0;
    uint32_t faults = 0;
    float    hz = 0.0f;
    uint64_t hzMs = 0;
};
State g_s;

FaultBudget g_budget("frameTiming", 4);

// The settle probe: twice in a session, twenty and sixty seconds after
// arming, three frames each, the four most recent records side by side in
// the log -- so a flight shows which record's GPU fields have resolved
// and every raw stamp fpsVR could be built from. Flown 2026-09-07 with the
// most recent record: the app's GPU time read 0.2 ms for a frame whose
// door pass alone was 5 ms, and fpsVR showed a steady 9.6.
constexpr uint32_t kProbeAt[2] = {1800, 5400};
constexpr uint32_t kProbeFrames = 3;
constexpr uint32_t kProbeDepth = 4;

void settleProbe(void* iface, PFN_GetFrameTiming fn, uint32_t published) {
    bool due = false;
    for (uint32_t at : kProbeAt) {
        if (published >= at && published < at + kProbeFrames) due = true;
    }
    if (!due) return;
    char line[900];
    size_t len = 0;
    for (uint32_t ago = 0; ago < kProbeDepth && len + 1 < sizeof(line); ++ago) {
        FrameTimingRaw r{};
        r.size = g_s.sizeUsed;
        bool ok = false;
        guarded("frameTiming/probe", [&] { ok = fn(iface, &r, ago); });
        int w = 0;
        if (!ok) {
            w = snprintf(line + len, sizeof(line) - len, "%sago %u: refused", ago ? " | " : "", ago);
        } else {
            w = snprintf(line + len, sizeof(line) - len,
                         "%sago %u: idx %u pre %.2f post %.2f total %.2f comp %.2f ccpu %.2f idle %.2f "
                         "interval %.2f wgp %.2f poses %.2f ready %.2f upd %.2f-%.2f rstart %.2f "
                         "drop %u pres %u flags 0x%x",
                         ago ? " | " : "", ago, r.frameIndex, static_cast<double>(r.preSubmitGpuMs),
                         static_cast<double>(r.postSubmitGpuMs), static_cast<double>(r.totalRenderGpuMs),
                         static_cast<double>(r.compositorRenderGpuMs),
                         static_cast<double>(r.compositorRenderCpuMs),
                         static_cast<double>(r.compositorIdleCpuMs),
                         static_cast<double>(r.clientFrameIntervalMs),
                         static_cast<double>(r.waitGetPosesCalledMs), static_cast<double>(r.newPosesReadyMs),
                         static_cast<double>(r.newFrameReadyMs), static_cast<double>(r.compositorUpdateStartMs),
                         static_cast<double>(r.compositorUpdateEndMs),
                         static_cast<double>(r.compositorRenderStartMs), r.numDroppedFrames,
                         r.numFramePresents, r.reprojectionFlags);
        }
        if (w <= 0) break;
        len += static_cast<size_t>(w) < sizeof(line) - len ? static_cast<size_t>(w) : sizeof(line) - len - 1;
    }
    Log::get().note("compositor timing probe (ms; poses/ready/upd/rstart are from the frame's vsync): %s",
                    line);
}

void standDown(const char* why) {
    if (g_s.standDown) return;
    g_s.standDown = true;
    Log::get().note("compositor timing: STANDING DOWN -- %s. The Monitor page shows what it can "
                    "measure itself (frame intervals) and leaves the compositor's columns blank.",
                    why);
}

bool finite(float v) { return v == v && v <= 1.0e6f && v >= -1.0e6f; }

// The headset's refresh, once a second through the system interface's
// float-property slot (range-checked as predictDisplayPose checks it).
void refreshHz() {
    if (!dueMs(g_s.hzMs, 1000)) return;
    g_s.hzMs = stampMs();
    void* sys = systemInterfaceV012();
    if (!sys) return;
    if (systemInterfacePrefixV012() <= kSysSlotFloatProp) return;
    void** vt = *reinterpret_cast<void***>(sys);
    if (!vt || !vt[kSysSlotFloatProp]) return;
    guarded("frameTiming/hz", [&] {
        int32_t err = 0;
        const float hz = reinterpret_cast<PFN_FloatProp>(vt[kSysSlotFloatProp])(
            sys, vr::k_unTrackedDeviceIndex_Hmd, kPropDisplayFrequency, &err);
        if (finite(hz) && hz >= 20.0f && hz <= 500.0f) g_s.hz = hz;
    });
}

}  // namespace

void frameTimingConfigure() {
    State& s = g_s;
    const std::string v = Config::get().getString("advanced.compositor_timing", "on");
    const bool on = !(v == "off" || v == "0" || v == "false" || v == "no");
    if (s.configured && on != s.on) {
        Log::get().note("compositor timing: %s.", on ? "on" : "off");
    }
    s.on = on;
    s.configured = true;
}

void frameTimingBoundary(void* iface, size_t prefix) {
    State& s = g_s;
    if (!s.on || s.standDown || !iface) return;
    if (prefix <= kSlotGetFrameTiming) {
        standDown("the compositor's table has too few entries for GetFrameTiming");
        return;
    }
    void** vt = *reinterpret_cast<void***>(iface);
    if (!vt || !vt[kSlotGetFrameTiming]) {
        standDown("the GetFrameTiming slot is empty");
        return;
    }
    refreshHz();

    FrameTimingRaw raw{};
    bool got = false;
    PFN_GetFrameTiming fn = reinterpret_cast<PFN_GetFrameTiming>(vt[kSlotGetFrameTiming]);
    const bool survived = guardedBudget(g_budget, [&] {
        // The size the runtime expects for the generation it serves: the
        // 1.0-era layout first, then the later one, remembered once one works.
        // The record asked for is the SETTLED one, kFrameTimingLag frames
        // back: the most recent is still in flight at this boundary and its
        // GPU stamps have not resolved (flown 2026-09-07: 0.2 ms for a frame
        // whose door pass alone was 5 ms).
        const uint32_t sizes[2] = {s.sizeUsed ? s.sizeUsed : 176u, s.sizeUsed ? s.sizeUsed : 184u};
        for (int i = 0; i < 2 && !got; ++i) {
            memset(&raw, 0, sizeof(raw));
            raw.size = sizes[i];
            if (fn(iface, &raw, kFrameTimingLag)) {
                got = true;
                s.sizeUsed = sizes[i];
            }
        }
    });
    if (!survived) {
        if (++s.faults >= 3) standDown("GetFrameTiming faulted repeatedly");
        return;
    }
    if (!got) {
        // Refused: the runtime has no timing yet (the first frames), or
        // this runtime does not implement it (OpenComposite). Give it a few
        // seconds of frames before concluding the second.
        if (++s.refusals == 600) {
            standDown("the runtime answered false to GetFrameTiming for 600 frames -- "
                      "OpenComposite does not implement it, and SteamVR answers within "
                      "a few frames");
        }
        return;
    }
    // Validate the first answer before believing any: the echoed size, a
    // finite interval in a frame's range, an index that moves.
    if (!s.armed) {
        const bool sane = raw.size == s.sizeUsed && finite(raw.clientFrameIntervalMs) &&
                          finite(raw.preSubmitGpuMs) && finite(raw.totalRenderGpuMs) &&
                          raw.clientFrameIntervalMs >= 0.0f && raw.clientFrameIntervalMs < 5000.0f;
        if (!sane) {
            standDown("the first GetFrameTiming answer did not read as a frame timing (size "
                      "not echoed, or an interval out of range) -- the slot or the layout is "
                      "not this build's; please report this log");
            return;
        }
        if (s.lastIndex == 0) {
            s.lastIndex = raw.frameIndex;
            return;   // one more frame, to see the index move
        }
        if (raw.frameIndex == s.lastIndex) return;
        s.armed = true;
        Log::get().note(
            "compositor timing: armed -- IVRCompositor::GetFrameTiming answers (layout %u "
            "bytes), one read per frame at the boundary for the menu's Monitor page, of the "
            "record %u frames back (the settled one): the app's GPU time, the compositor's, "
            "dropped and reprojected frames, the CPU frame interval and the app's busy time. "
            "advanced.compositor_timing = off turns it off, live.",
            s.sizeUsed, kFrameTimingLag);
    }
    FrameTimingSample out{};
    if (raw.frameIndex != s.lastIndex) {
        // A new compositor frame: its dropped count is new information.
        s.droppedTotal += raw.numDroppedFrames;
        s.lastIndex = raw.frameIndex;
    }
    out.frameIndex = raw.frameIndex;
    out.presents = raw.numFramePresents;
    out.droppedTotal = s.droppedTotal;
    out.reprojFlags = raw.reprojectionFlags;
    out.appGpuMs = raw.preSubmitGpuMs + raw.postSubmitGpuMs;
    out.totalGpuMs = raw.totalRenderGpuMs;
    out.compGpuMs = raw.compositorRenderGpuMs;
    out.compCpuMs = raw.compositorRenderCpuMs;
    out.cpuFrameMs = raw.clientFrameIntervalMs;
    // The app's busy time: from the poses arriving to the second eye's
    // submit. Both stamps are milliseconds from the frame's vsync (running
    // start puts the first a few ms before it); a pair that reads backwards
    // or absurd is reported as unknown rather than as a number.
    {
        const float busy = raw.newFrameReadyMs - raw.newPosesReadyMs;
        out.appCpuMs = (finite(busy) && busy >= 0.0f && busy < 1000.0f) ? busy : 0.0f;
        out.posesReadyMs = finite(raw.newPosesReadyMs) ? raw.newPosesReadyMs : 0.0f;
        out.frameReadyMs = finite(raw.newFrameReadyMs) ? raw.newFrameReadyMs : 0.0f;
    }
    out.presentCpuMs = raw.presentCallCpuMs;
    out.idleCpuMs = raw.compositorIdleCpuMs;
    out.displayHz = s.hz;
    publishFrameTiming(out);
    ++s.published;
    settleProbe(iface, fn, s.published);
}

namespace {
typedef void (*PFN_DoorGpu)(void*, int);
PFN_DoorGpu g_doorBegin = nullptr;
PFN_DoorGpu g_doorEnd = nullptr;
bool        g_doorTried = false;
int64_t     g_doorQpc[2] = {};

void resolveDoorGpu() {
    if (g_doorTried) return;
    g_doorTried = true;
    HMODULE m = GetModuleHandleW(L"d3d11.dll");
    if (!m) return;
    g_doorBegin = reinterpret_cast<PFN_DoorGpu>(GetProcAddress(m, "edvrDoorGpuBegin"));
    g_doorEnd = reinterpret_cast<PFN_DoorGpu>(GetProcAddress(m, "edvrDoorGpuEnd"));
    if (!g_doorBegin || !g_doorEnd) {
        g_doorBegin = g_doorEnd = nullptr;
        Log::get().note("compositor timing: d3d11.dll exports no door GPU bracket (an older "
                        "pair); the Monitor page's door GPU figure stays blank.");
    }
}
}  // namespace

void frameTimingDoorBegin(void* handle, int eye) {
    if (eye < 0 || eye > 1) return;
    g_doorQpc[eye] = qpcNow();
    if (handle) {
        resolveDoorGpu();
        if (g_doorBegin) g_doorBegin(handle, eye);
    }
}

void frameTimingDoorEnd(void* handle, int eye) {
    if (eye < 0 || eye > 1) return;
    if (handle && g_doorEnd) g_doorEnd(handle, eye);
    if (g_doorQpc[eye] && qpcFrequency() > 0) {
        const double us = static_cast<double>(qpcNow() - g_doorQpc[eye]) * 1.0e6 /
                          static_cast<double>(qpcFrequency());
        if (us > 0.0 && us < 1.0e6) addDoorCpuUs(static_cast<uint32_t>(us));
    }
    g_doorQpc[eye] = 0;
}

void frameTimingShutdown() {
    if (g_s.published) {
        Log::get().note("compositor timing: %u samples published, %u frames dropped by the "
                        "compositor this session.",
                        g_s.published, g_s.droppedTotal);
    }
}

}  // namespace edvr
