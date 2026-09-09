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

// COMPOSITOR_FRAMETIMING COMES IN TWO LAYOUTS, AND THE SIZE FIELD DOES NOT
// SAY WHICH. SteamVR fills the struct belonging to the compositor interface
// the process BOUND, not the size the caller asked for -- Elite ships an
// openvr_api.dll that exports IVRCompositor_014 (openvr v0.9.20), so it is
// served the record of that era whatever we pass, and our m_nSize comes
// back echoed and ignored. Decoding that with the modern struct displaced
// eleven fields: the GPU frame time read as the compositor's idle CPU time,
// the dropped-frame count and the reprojection flags read as the two halves
// of the record's own clock. Both were wrong in the field, and the clock is
// what proved it: those two words, read together as a double, advanced by
// exactly the wall-clock seconds between two probes.
//
// So the fields are read by BYTE OFFSET from a raw buffer, through a table
// chosen once by measurement (armLayout below), and never by casting.
struct Layout {
    const char* name;
    uint32_t size;        // what to ask for
    int systemTime;       // the double; the discriminator
    int presents, dropped, flags;
    int sceneGpu;         // the app's own GPU work
    int totalGpu;         // the GPU frame: fpsVR's GPU frametime
    int compGpu, compCpu, idleCpu, presentCpu;
    int wgpCalled, posesReady, frameReady;   // ms from the frame's vsync
    int pose;             // the HmdMatrix34_t, for the confirming test
};

// openvr v0.9.20 / IVRCompositor_014 (identical in v1.0.0's _015): four
// uint32, the double at 16, fifteen floats, the pose at 84, the fidelity
// level at 164 and the reprojection flags LAST at 168.
constexpr Layout kLegacy = {"IVRCompositor_014 (openvr 0.9.20)", 176,
                            /*systemTime*/ 16,
                            /*presents*/ 8, /*dropped*/ 12, /*flags*/ 168,
                            /*sceneGpu*/ 24, /*totalGpu*/ 28, /*compGpu*/ 32,
                            /*compCpu*/ 36, /*idleCpu*/ 40, /*presentCpu*/ 48,
                            /*wgp*/ 60, /*poses*/ 64, /*ready*/ 68, /*pose*/ 84};

// openvr.h as it stands: six uint32, the double at 24, sixteen floats, the
// pose at 96. m_flPreSubmitGpuMs takes the place of the scene's GPU time.
constexpr Layout kModern = {"the current openvr.h", 184,
                            /*systemTime*/ 24,
                            /*presents*/ 8, /*dropped*/ 16, /*flags*/ 20,
                            /*sceneGpu*/ 32, /*totalGpu*/ 40, /*compGpu*/ 44,
                            /*compCpu*/ 48, /*idleCpu*/ 52, /*presentCpu*/ 60,
                            /*wgp*/ 72, /*poses*/ 76, /*ready*/ 80, /*pose*/ 96};

constexpr size_t kBufBytes = 256;   // room for either, with slack

float f32At(const uint8_t* b, int off) {
    float v = 0.0f;
    memcpy(&v, b + off, sizeof(v));
    return v;
}
uint32_t u32At(const uint8_t* b, int off) {
    uint32_t v = 0;
    memcpy(&v, b + off, sizeof(v));
    return v;
}
double f64At(const uint8_t* b, int off) {
    double v = 0.0;
    memcpy(&v, b + off, sizeof(v));
    return v;
}

struct State {
    bool     on = true;
    bool     configured = false;
    bool     standDown = false;
    bool     armed = false;         // the layout chosen and validated
    const Layout* layout = nullptr;
    uint32_t sizeUsed = 0;          // the size the runtime accepted
    uint32_t lastIndex = 0;
    uint32_t droppedTotal = 0;
    uint32_t published = 0;
    uint32_t refusals = 0;
    uint32_t faults = 0;
    // The arming measurement: one earlier sample of each candidate's clock.
    double   armClock[2] = {0.0, 0.0};
    uint32_t armIndex = 0;
    int      armSeen = 0;
    float    hz = 0.0f;
    uint64_t hzMs = 0;
};
State g_s;

FaultBudget g_budget("frameTiming", 4);

// The probe: twice in a session, twenty and sixty seconds after arming,
// three frames each -- every field of the four most recent records under
// the layout in use, and then the newest record's RAW WORDS in hex, so a
// layout this build decodes wrongly can be read off any flight's log
// without another build. That hex is what identified the layout on
// 2026-09-08; keep it.
constexpr uint32_t kProbeAt[2] = {1800, 5400};
constexpr uint32_t kProbeFrames = 3;
constexpr uint32_t kProbeDepth = 4;

void settleProbe(void* iface, PFN_GetFrameTiming fn, uint32_t published) {
    bool due = false;
    for (uint32_t at : kProbeAt) {
        if (published >= at && published < at + kProbeFrames) due = true;
    }
    if (!due || !g_s.layout) return;
    const Layout& L = *g_s.layout;
    char line[900];
    size_t len = 0;
    alignas(8) uint8_t buf[kBufBytes];
    for (uint32_t ago = 0; ago < kProbeDepth && len + 1 < sizeof(line); ++ago) {
        memset(buf, 0, sizeof(buf));
        *reinterpret_cast<uint32_t*>(buf) = g_s.sizeUsed;
        bool ok = false;
        guarded("frameTiming/probe", [&] { ok = fn(iface, buf, ago); });
        int w = 0;
        if (!ok) {
            w = snprintf(line + len, sizeof(line) - len, "%sago %u: refused", ago ? " | " : "", ago);
        } else {
            w = snprintf(line + len, sizeof(line) - len,
                         "%sago %u: idx %u t %.4f scene %.2f total %.2f cgpu %.2f ccpu %.3f idle %.2f "
                         "pres_cpu %.3f wgp %.2f poses %.2f ready %.2f drop %u pres %u flags 0x%x",
                         ago ? " | " : "", ago, u32At(buf, 4), f64At(buf, L.systemTime),
                         static_cast<double>(f32At(buf, L.sceneGpu)),
                         static_cast<double>(f32At(buf, L.totalGpu)),
                         static_cast<double>(f32At(buf, L.compGpu)),
                         static_cast<double>(f32At(buf, L.compCpu)),
                         static_cast<double>(f32At(buf, L.idleCpu)),
                         static_cast<double>(f32At(buf, L.presentCpu)),
                         static_cast<double>(f32At(buf, L.wgpCalled)),
                         static_cast<double>(f32At(buf, L.posesReady)),
                         static_cast<double>(f32At(buf, L.frameReady)), u32At(buf, L.dropped),
                         u32At(buf, L.presents), u32At(buf, L.flags));
        }
        if (w <= 0) break;
        len += static_cast<size_t>(w) < sizeof(line) - len ? static_cast<size_t>(w) : sizeof(line) - len - 1;
    }
    Log::get().note("compositor timing probe (%s; ms; wgp/poses/ready are from the frame's vsync): %s",
                    L.name, line);
    {
        memset(buf, 0, sizeof(buf));
        *reinterpret_cast<uint32_t*>(buf) = g_s.sizeUsed;
        bool ok = false;
        guarded("frameTiming/probeRaw", [&] { ok = fn(iface, buf, 0); });
        if (ok) {
            const size_t n = 48;   // 192 bytes, past either layout
            char hex[48 * 9 + 8];
            size_t hl = 0;
            for (size_t i = 0; i < n && hl + 10 < sizeof(hex); ++i) {
                hl += static_cast<size_t>(
                    snprintf(hex + hl, sizeof(hex) - hl, "%s%08x", i ? " " : "",
                             u32At(buf, static_cast<int>(i * 4))));
            }
            Log::get().note("compositor timing probe raw (ago 0, %u words of the record as written, "
                            "little-endian): %s",
                            static_cast<unsigned>(n), hex);
        }
    }
}

// WHICH LAYOUT THE RUNTIME IS FILLING, by measurement rather than by the
// size field, which this runtime echoes back unchanged.
//
// The test is the record's own clock. Only one of the two candidate
// offsets holds m_flSystemTimeInSeconds, and it is unmistakable: it is a
// plausible uptime, and it advances by about one frame period between
// consecutive frame indices. At the other offset the modern layout has two
// small counts (which read as zero or a denormal) and the legacy one has
// two GPU floats (which read as hundreds of thousands of seconds and do
// not advance monotonically). The pose confirms it: an HmdMatrix34_t whose
// rows are unit vectors sits at 84 in one layout and 96 in the other.
bool clockPlausible(double t) { return t > 1.0 && t < 1.0e7; }

bool poseLooksRight(const uint8_t* buf, int off) {
    for (int r = 0; r < 3; ++r) {
        const float x = f32At(buf, off + (r * 4 + 0) * 4);
        const float y = f32At(buf, off + (r * 4 + 1) * 4);
        const float z = f32At(buf, off + (r * 4 + 2) * 4);
        const float n = x * x + y * y + z * z;
        if (!(n > 0.98f && n < 1.02f)) return false;
    }
    return true;
}

// Two samples of one candidate: does its clock advance like a frame clock?
bool clockAdvances(double before, double now, uint32_t frames) {
    if (!clockPlausible(before) || !clockPlausible(now)) return false;
    if (frames == 0 || frames > 600) return false;
    const double perFrame = (now - before) * 1000.0 / static_cast<double>(frames);
    return perFrame > 1.0 && perFrame < 120.0;   // 8 Hz to 1000 Hz
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

    alignas(8) uint8_t buf[kBufBytes];
    bool got = false;
    PFN_GetFrameTiming fn = reinterpret_cast<PFN_GetFrameTiming>(vt[kSlotGetFrameTiming]);
    const bool survived = guardedBudget(g_budget, [&] {
        // The size to ask for. It selects nothing -- this runtime echoes it
        // back untouched and fills the layout belonging to the compositor
        // interface the game bound -- so ask for the larger and let the
        // detection below decide what came back. The record read is the one
        // kFrameTimingLag frames back, the settled one.
        const uint32_t sizes[2] = {s.sizeUsed ? s.sizeUsed : 184u, s.sizeUsed ? s.sizeUsed : 176u};
        for (int i = 0; i < 2 && !got; ++i) {
            memset(buf, 0, sizeof(buf));
            *reinterpret_cast<uint32_t*>(buf) = sizes[i];
            if (fn(iface, buf, kFrameTimingLag)) {
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

    // ARM: choose the layout by measuring the record, never by its size
    // field. Two consecutive answers are needed, because the test is that
    // the clock ADVANCES like a frame clock.
    if (!s.armed) {
        const Layout* candidates[2] = {&kLegacy, &kModern};
        const uint32_t idx = u32At(buf, 4);
        const double now[2] = {f64At(buf, kLegacy.systemTime), f64At(buf, kModern.systemTime)};
        if (s.armSeen == 0) {
            s.armClock[0] = now[0];
            s.armClock[1] = now[1];
            s.armIndex = idx;
            s.armSeen = 1;
            return;
        }
        if (idx == s.armIndex) return;   // the same record; wait for a new one
        const uint32_t frames = idx > s.armIndex ? idx - s.armIndex : 0;
        int chosen = -1;
        for (int i = 0; i < 2; ++i) {
            if (!clockAdvances(s.armClock[i], now[i], frames)) continue;
            if (!poseLooksRight(buf, candidates[i]->pose)) continue;
            chosen = i;
        }
        if (chosen < 0) {
            // Try again from scratch for a while: the first records after a
            // scene change can be odd. Give up loudly rather than guess.
            s.armClock[0] = now[0];
            s.armClock[1] = now[1];
            s.armIndex = idx;
            if (++s.refusals > 900) {
                standDown("no candidate layout's clock advanced like a frame clock, and no "
                          "candidate's pose read as a rotation -- this runtime's "
                          "Compositor_FrameTiming is neither layout this build knows. The "
                          "probe's raw words in this log say what it is; please report it");
            }
            return;
        }
        s.layout = candidates[chosen];
        s.armed = true;
        s.lastIndex = idx;
        Log::get().note(
            "compositor timing: armed -- IVRCompositor::GetFrameTiming answers, and the record "
            "it fills is %s, chosen by measurement: its clock at byte %d advanced one frame "
            "period per frame index and its pose at byte %d reads as a rotation. One read per "
            "frame at the boundary, of the record %u frames back (the settled one). "
            "advanced.compositor_timing = off turns it off, live.",
            s.layout->name, s.layout->systemTime, s.layout->pose, kFrameTimingLag);
        return;
    }
    const Layout& L = *s.layout;

    FrameTimingSample out{};
    const uint32_t idx = u32At(buf, 4);
    if (idx != s.lastIndex) {
        // A new compositor frame: its dropped count is new information.
        const uint32_t d = u32At(buf, L.dropped);
        if (d < 1000u) s.droppedTotal += d;
        s.lastIndex = idx;
    }
    out.layout = L.size;
    out.frameIndex = idx;
    out.presents = u32At(buf, L.presents);
    out.droppedTotal = s.droppedTotal;
    out.reprojFlags = u32At(buf, L.flags);
    // THE GPU FRAME TIME is the record's own total: Valve's example on the
    // Compositor_FrameTiming page adds exactly this field, and it is what
    // fpsVR shows. The app's share is the scene's own GPU work beside it.
    out.totalGpuMs = f32At(buf, L.totalGpu);
    out.appGpuMs = f32At(buf, L.sceneGpu);
    out.compGpuMs = f32At(buf, L.compGpu);
    out.compCpuMs = f32At(buf, L.compCpu);
    out.idleCpuMs = f32At(buf, L.idleCpu);
    out.presentCpuMs = f32At(buf, L.presentCpu);
    // THE APP'S CPU FRAME TIME, the same page's example: from the poses
    // arriving to the second eye's submit, plus the compositor's own submit
    // cost. The stamps are milliseconds from the frame's vsync, and running
    // start puts the first of them before it, so either may be negative;
    // only their difference is a duration, and a pair that reads backwards
    // is reported as unknown rather than as a number.
    {
        const float poses = f32At(buf, L.posesReady);
        const float ready = f32At(buf, L.frameReady);
        const float busy = ready - poses + f32At(buf, L.compCpu);
        out.appCpuMs = (finite(busy) && busy >= 0.0f && busy < 1000.0f) ? busy : 0.0f;
        out.posesReadyMs = finite(poses) ? poses : 0.0f;
        out.frameReadyMs = finite(ready) ? ready : 0.0f;
    }
    // The record's own frame period, from its clock: the field named for it
    // in the legacy layout is not one (it reads about 3 ms at 90 Hz).
    out.cpuFrameMs = 0.0f;    out.displayHz = s.hz;
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
