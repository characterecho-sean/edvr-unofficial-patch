// advanced.explorer_cam_probe: the glue (explorer_cam_probe.h says what it is; explorer_cam_probe_core.h holds every decision
// and the text of every line, driven by tools\explorer_cam_probe_test).
//
// THREADS. The I3 observer runs on whichever thread the game's job system calls FreeCameraActivity's update from, after the original
// has returned: it takes no lock, allocates nothing, writes no log line and calls nothing of the game's. It publishes through a
// seqlock and a small event ring (both in the core). The hook itself is explorer_cam.cpp's (one target gets one CodeHook, and
// Explorer Cam's placement runs in the same hook); the probe attaches its observer to it. I2's tee runs on whichever thread unmaps
// a 5376-byte scene block (a try-lock, never a wait). I1's tally runs on the render thread inside the census's observer. The
// consumer (explorerCamProbeFrameBoundary) is the render thread's Present boundary and is the only code that logs.
//
// KEY OFF. explorerCamProbeFrameBoundary reads the key, finds it off, says so once and returns: the probe asks for no hook, the
// census's note pointer stays null, the tee flag stays false and nothing is allocated. Turning the key off while it runs detaches
// the observer (the hook stays in place; its relay gate closes unless Explorer Cam placement is using it) and detaches I1 and I2.
#include "explorer_cam_probe.h"
#include "explorer_cam_probe_core.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "../common/config.h"
#include "../common/log.h"
#include "explorer_cam.h"
#include "flat_camera_inject.h"
#include "vr_camera_census.h"

namespace edvr {
namespace {

ecp::Shared g_shared;
ecp::SkinTee g_skin;
ecp::CamTee g_cam;
ecp::Consumer g_consumer;

std::atomic<bool> g_i2On{false};   // the tee flag vscreen.cpp's Map hook reads
bool g_on = false;                 // the consumer's state: armed (render thread only)
bool g_offLogged = false;
bool g_announced = false;          // the armed lines are printed once per session; a re-arm prints a short one
enum class I3State { NotTried, Armed, StoodDown };
I3State g_i3 = I3State::NotTried;

// ---- guarded reads of the game's memory (nothing with a destructor lives in a function that has a __try) --------------
__declspec(noinline) bool sehCopyRaw(const uint8_t* a, ecp::Raw* out) noexcept {
    __try {
        std::memcpy(out->world, a + ecp::kOffWorldPose, sizeof(out->world));
        std::memcpy(out->local, a + ecp::kOffLocalPose, sizeof(out->local));
        std::memcpy(&out->target, a + ecp::kOffTarget, sizeof(out->target));   // the raw qword; never dereferenced
        out->relative = a[ecp::kOffRelative];
        out->rotationLock = a[ecp::kOffRotationLock];
        out->presetPending = a[ecp::kOffPresetPending];
        out->state = a[ecp::kOffState];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// ---- the I3 observer ------------------------------------------------------------------------------------------------------
// explorer_cam.cpp's hook calls this after the original returns (and after the lock press is restored), with rcx = the activity.
// The snapshot is taken after the original, so it describes the state the update left.
__declspec(noinline) void observeActivity(void* a) noexcept {
    if (!a) {
        g_shared.totalCalls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ecp::Raw raw;
    const bool read = g_shared.faults.load(std::memory_order_relaxed) < ecp::kMaxFaults &&
                      sehCopyRaw(static_cast<const uint8_t*>(a), &raw);
    ecp::noteActivityCall(g_shared, reinterpret_cast<uintptr_t>(a), GetCurrentThreadId(), read ? &raw : nullptr);
}

// ---- I1's feed from the census ---------------------------------------------------------------------------------------------
void censusNote(uint64_t frame, uint64_t camera, uint64_t callerRva, const uint8_t* snap) noexcept {
    g_cam.note(frame, static_cast<uint32_t>(callerRva), camera, snap);
}

void logSink(void*, const char* line) { Log::get().note("%s", line); }

const char* i3StatusName() {
    return g_i3 == I3State::Armed ? "armed" : g_i3 == I3State::StoodDown ? "stood down" : "not tried";
}

void say(const ecp::Sink& sink, const char* fmt, ...) {
    char line[ecp::kLineBytes];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    sink(line);
}

void arm(uint64_t nowMs, const ecp::Sink& sink) {
    g_on = true;
    g_offLogged = false;
    const bool first = !g_announced;
    g_announced = true;
    // The hook is explorer_cam.cpp's (one target gets one CodeHook); the probe attaches its observer to it. Attaching is idempotent,
    // and a hook that stood down stays down.
    const ExplorerCamHookStatus hook = explorerCamProbeAttach(true, &observeActivity);
    if (g_i3 == I3State::NotTried) {
        if (hook.state == ExplorerCamHookStatus::Armed) {
            g_i3 = I3State::Armed;
            say(sink, "%s EliteDangerous64.exe+0x%llX (FreeCameraActivity update, build 332841) hooked at 0x%llX, stolen=%zu bytes, "
                      "prologue %zu/%zu bytes verified, relay at 0x%llX; the original runs first, then rcx's activity is copied (pose "
                      "+0x70 and +0x3B0, bytes +0x470 +0x471 +0x473 +0x48C, the raw qword +0x2C8) through a seqlock; four integer "
                      "registers are forwarded and the return value is unchanged",
                ecp::prefixI3Armed(), static_cast<unsigned long long>(ecp::kTargetRva), static_cast<unsigned long long>(hook.target), hook.stolen,
                ecp::kPrologueBytes, ecp::kPrologueBytes, static_cast<unsigned long long>(hook.relay));
        } else {
            g_i3 = I3State::StoodDown;
            say(sink, "%s %s. I3 will not run; I1 and I2 do, but their detail lines are gated on I3 and will not print.",
                ecp::prefixI3Down(), hook.why);
        }
    }
    g_i2On.store(true, std::memory_order_release);
    detail::g_vrCensusCameraNote = &censusNote;
    g_consumer.start(nowMs, g_shared.totalCalls.load(std::memory_order_relaxed));
    if (first) {
        say(sink, "%s (advanced.explorer_cam_probe): a temporary, log-only instrument for the Explorer Cam redesign (Phase 0b, "
                  "flight F0), removed when the arc closes; it writes nothing to the game. Lines: 'explorer cam probe I3|I1|I2 "
                  "armed/stood down' once, '... heartbeat:' every 5 s, 'I3 change:' at once on a change of +0x48C/+0x470/+0x471/+0x473, "
                  "and 'I3 pose:' 'I1 cam:' 'I2 root:' at 1 Hz while +0x48C != 0 and for 2 s after.",
            ecp::prefixOn());
        say(sink, "%s the VR camera census is the source (advanced.vr_camera_census is %s; this probe switches the census on "
                  "implicitly while it is on). Per (camera kind +0x264, call site): calls per frame, origin +0x50..+0x58, axes rows "
                  "+0x20..+0x4C, as the census snapshots them after the game's body; read only.",
            ecp::prefixI1Armed(), Config::get().getString("advanced.vr_camera_census", "off") == "on" ? "on explicitly" : "off");
        say(sink, "%s the 5376-byte scene blocks at Unmap (a separate read of the camera tee: the temporal pass's own feed is "
                  "untouched). Skinned-object fingerprint: floats 536..538 equal floats 935/939/943 within 0.05, the rows at floats "
                  "932/936/940 each have squared length 0.81..1.21, |translation| under 50 m. Reports the nearest root in view space.",
            ecp::prefixI2Armed());
    } else {
        say(sink, "%s again (advanced.explorer_cam_probe turned back on); I3 is %s.", ecp::prefixOn(), i3StatusName());
    }
}

void disarm(const ecp::Sink& sink) {
    g_on = false;
    explorerCamProbeAttach(false, nullptr);   // the observer detaches; the hook stays in place and its gate closes unless placement uses it
    g_i2On.store(false, std::memory_order_release);
    detail::g_vrCensusCameraNote = nullptr;
    say(sink, "%s (advanced.explorer_cam_probe turned off while running): the I3 observer is detached (the hook stays in place, "
              "inert for the probe), I1 and I2 are detached.", ecp::prefixOff());
}

void boundaryAt(uint32_t frame, uint64_t nowMs, bool want, const ecp::Sink& sink) {
    if (!want) {
        if (g_on) disarm(sink);
        else if (!g_offLogged) {
            g_offLogged = true;
            say(sink, "%s (advanced.explorer_cam_probe = off): no hook is installed and nothing is read.", ecp::prefixOff());
        }
        return;
    }
    if (!g_on) arm(nowMs, sink);
    ecp::TickIn in;
    in.nowMs = nowMs;
    in.frame = frame;
    in.i3Armed = g_i3 == I3State::Armed;
    in.i3Status = i3StatusName();
    in.censusWanted = vrCameraCensusWanted();
    in.injectStatus = flatCameraInjectObserveStatus();
    in.i2Armed = g_i2On.load(std::memory_order_acquire);
    g_consumer.tick(g_shared, g_skin, g_cam, in, sink);
}

}  // namespace

void explorerCamProbeFrameBoundary(uint32_t frameNo) {
    const bool want = Config::get().getBool("advanced.explorer_cam_probe", false);
    boundaryAt(frameNo, GetTickCount64(), want, ecp::Sink{&logSink, nullptr});
}

bool explorerCamProbeWantsSceneBlocks() { return g_i2On.load(std::memory_order_acquire); }

void explorerCamProbeNoteSceneBlock(const void* resource, const void* data, uint32_t bytes) {
    if (!g_i2On.load(std::memory_order_acquire) || !data || bytes < ecp::kFingerprintFloats * 4) return;
    g_skin.note(reinterpret_cast<uintptr_t>(resource), static_cast<const float*>(data), bytes / 4);
}

#ifdef EDVR_EXPLORER_CAM_PROBE_TEST
// The rig's seam (tools\explorer_cam_probe_test): the same boundary with a scripted clock and key, the shared state, and a
// synthetic target in place of the game's function (installed through explorer_cam.cpp's own seam).
namespace explorercamprobetest {
void setTarget(uintptr_t target) { explorercamtest::setTargets(target, 0); }
void boundary(uint32_t frame, uint64_t nowMs, bool want, ecp::SinkFn fn, void* ctx) { boundaryAt(frame, nowMs, want, ecp::Sink{fn, ctx}); }
ecp::Shared& shared() { return g_shared; }
ecp::SkinTee& skin() { return g_skin; }
ecp::CamTee& cam() { return g_cam; }
size_t stolenBytes() { return explorercamtest::freeStolen(); }
bool gateOpen() { return explorercamtest::gateOpen(); }
// Back to a session that has not tried the hook: the CodeHook is uninstalled (the original bytes return), the one-shot latches clear.
void reset() {
    explorercamtest::reset();
    g_i3 = I3State::NotTried;
    g_on = false;
    g_offLogged = false;
    g_announced = false;
    g_i2On.store(false);
    detail::g_vrCensusCameraNote = nullptr;
}
void observe(void* a) { observeActivity(a); }
}  // namespace explorercamprobetest
#endif

}  // namespace edvr
