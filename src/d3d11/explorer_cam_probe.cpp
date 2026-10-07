// advanced.explorer_cam_probe: the glue (explorer_cam_probe.h says what it is; explorer_cam_probe_core.h holds every decision
// and the text of every line, driven by tools\explorer_cam_probe_test).
//
// THREADS. The I3 hook runs on whichever thread the game's job system calls FreeCameraActivity's update from: it takes no lock,
// allocates nothing, writes no log line and calls nothing of the game's. It publishes through a seqlock and a small event ring
// (both in the core). I2's tee runs on whichever thread unmaps a 5376-byte scene block (a try-lock, never a wait). I1's tally
// runs on the render thread inside the census's observer. The consumer (explorerCamProbeFrameBoundary) is the render thread's
// Present boundary and is the only code that logs.
//
// KEY OFF. explorerCamProbeFrameBoundary reads the key, finds it off, says so once and returns: no hook is installed, the
// census's note pointer stays null, the tee flag stays false and nothing is allocated. Turning the key off while it runs closes
// the relay's gate (the game's call runs straight through to the original) and detaches I1 and I2.
#include "explorer_cam_probe.h"
#include "explorer_cam_probe_core.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/log.h"
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

#ifdef EDVR_EXPLORER_CAM_PROBE_TEST
uintptr_t g_testTarget = 0;        // the rig's synthetic function, installed in place of the game's
#endif

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
__declspec(noinline) bool sehCheckBytes(uintptr_t address, const uint8_t* expected, size_t n) noexcept {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(address), expected, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// PE TimeDateStamp + SizeOfImage (base+0x3C -> e_lfanew, +8 and +0x50), the pair every build-keyed hook here checks.
__declspec(noinline) bool checkIdentity(uintptr_t base, const char** why) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) { *why = "the PE header offset is implausible"; return false; }
        uint32_t timestamp = 0, imageSize = 0;
        std::memcpy(&timestamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(&imageSize, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
        if (timestamp != ecp::kExpectedTimestamp || imageSize != ecp::kExpectedImageSize) {
            *why = "the PE timestamp or image size is not build 332841's";
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "a read faulted while checking the PE header";
        return false;
    }
}

// ---- the relay: this DLL loads more than two gigabytes from the game, so a five-byte E9 cannot reach the replacement ----
// Copied from pose_reader_watch.cpp / object_record_writer_hook.cpp (grep kRelayBytes for the copies; if one changes, look at
// the others). mov rax,&gate; cmp qword ptr [rax],0; je original; jmp [callback]; original: jmp [trampoline]. RAX and the
// flags are volatile and the observed function does not read RAX on entry.
constexpr size_t kRelayBytes = 44, kOriginalLiteral = 36;

uint8_t* allocateRelay(uintptr_t target) noexcept {
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t floor = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t ceiling = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t distance = uintptr_t(INT32_MAX) - 0x10000u;
    uintptr_t at = target > distance ? target - distance : floor;
    if (at < floor) at = floor;
    const uintptr_t limit = target > ceiling - distance ? ceiling : target + distance;
    while (at < limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region))) break;
        const uintptr_t start = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > UINTPTR_MAX - start) break;
        const uintptr_t end = start + region.RegionSize;
        if (region.State == MEM_FREE) {
            uintptr_t candidate = at > start ? at : start;
            if (candidate > UINTPTR_MAX - (granularity - 1)) break;
            candidate = (candidate + granularity - 1) & ~(granularity - 1);
            if (candidate < limit && candidate < end && end - candidate >= 4096) {
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096,
                                                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    const uint8_t body[kRelayBytes] = {
        0x48,0xB8,0,0,0,0,0,0,0,0, 0x48,0x83,0x38,0,
        0x74,0x0E, 0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0,
        0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &gateAddress, 8);
    std::memcpy(code + 22, &callbackAddress, 8);
}

struct HookEntry {
    CodeHook hook;
    uint8_t* relay = nullptr;
    std::atomic<uintptr_t> forward{0};
    uintptr_t target = 0;
};
HookEntry g_hook;
// Open while the key is on, closed (the relay jumps straight to the trampoline) while it is off.
alignas(8) std::atomic<uintptr_t> g_relayGate{0};

bool prepareRelay(void* trampoline, void* context) noexcept {
    auto& entry = *static_cast<HookEntry*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(entry.relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), entry.relay, kRelayBytes)) return false;
    entry.forward.store(address, std::memory_order_release);
    return true;
}

// ---- the I3 hook ----------------------------------------------------------------------------------------------------------
// Ghidra shows one parameter (rcx = the activity), a return value in rax and no float-register parameter read, so four
// integer registers are forwarded and nothing else is declared: declaring a float parameter would read an xmm register the
// caller never set. The original runs FIRST; the snapshot is taken after it returns, so it describes the state the update
// left; the return value goes back unchanged.
using ForwardFn = uint64_t (__fastcall*)(void*, void*, void*, void*);

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

__declspec(noinline) uint64_t __fastcall freeCameraObserved(void* a, void* b, void* c, void* d) noexcept {
    const auto forward = reinterpret_cast<ForwardFn>(g_hook.forward.load(std::memory_order_acquire));
    if (!forward) return 0;   // unreachable: the relay exists only after the install published a trampoline
    const uint64_t result = forward(a, b, c, d);
    observeActivity(a);
    return result;
}

// Verify the build, verify the prologue, place the relay and hook. On failure `why` says why in one sentence, nothing was patched.
bool installHook(char* why, size_t whyCap, size_t* stolen, uintptr_t* targetOut) {
    uintptr_t target = 0;
#ifdef EDVR_EXPLORER_CAM_PROBE_TEST
    if (g_testTarget) target = g_testTarget;
#endif
    if (!target) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!base) {
            std::snprintf(why, whyCap, "the game module could not be resolved");
            return false;
        }
        const char* peWhy = nullptr;
        if (!checkIdentity(base, &peWhy)) {
            std::snprintf(why, whyCap, "the game build differs (%s); EliteDangerous64.exe+0x%llX was not touched",
                          peWhy, static_cast<unsigned long long>(ecp::kTargetRva));
            return false;
        }
        target = base + ecp::kTargetRva;
    }
    if (!sehCheckBytes(target, ecp::kPrologue, ecp::kPrologueBytes)) {
        std::snprintf(why, whyCap,
                      "the game build differs (the %zu bytes at EliteDangerous64.exe+0x%llX are not build 332841's FreeCameraActivity "
                      "update prologue); nothing was patched",
                      ecp::kPrologueBytes, static_cast<unsigned long long>(ecp::kTargetRva));
        return false;
    }
    g_hook.target = target;
    g_hook.relay = allocateRelay(target);
    if (!g_hook.relay) {
        std::snprintf(why, whyCap, "no executable memory could be placed within two gigabytes of the target; nothing was patched");
        return false;
    }
    buildRelay(g_hook.relay, &g_relayGate, reinterpret_cast<void*>(&freeCameraObserved));
    if (!g_hook.hook.install(reinterpret_cast<void*>(target), g_hook.relay, nullptr, "explorer-cam-free-camera-update",
                             &prepareRelay, &g_hook)) {
        VirtualFree(g_hook.relay, 0, MEM_RELEASE);
        g_hook.relay = nullptr;
        std::snprintf(why, whyCap, "CodeHook refused it (its own line above, tagged explorer-cam-free-camera-update, names why); nothing was patched");
        return false;
    }
    *stolen = g_hook.hook.stolenBytes();
    *targetOut = target;
    return true;
    // Process-lifetime storage: the relay and trampoline are never freed, so a call already inside them can finish.
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
    if (g_i3 == I3State::NotTried) {
        char why[400] = {};
        size_t stolen = 0;
        uintptr_t target = 0;
        if (installHook(why, sizeof(why), &stolen, &target)) {
            g_i3 = I3State::Armed;
            say(sink, "%s EliteDangerous64.exe+0x%llX (FreeCameraActivity update, build 332841) hooked at 0x%llX, stolen=%zu bytes, "
                      "prologue %zu/%zu bytes verified, relay at 0x%llX; the original runs first, then rcx's activity is copied (pose "
                      "+0x70 and +0x3B0, bytes +0x470 +0x471 +0x473 +0x48C, the raw qword +0x2C8) through a seqlock; four integer "
                      "registers are forwarded and the return value is unchanged",
                ecp::prefixI3Armed(), static_cast<unsigned long long>(ecp::kTargetRva), static_cast<unsigned long long>(target), stolen,
                ecp::kPrologueBytes, ecp::kPrologueBytes, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_hook.relay)));
        } else {
            g_i3 = I3State::StoodDown;
            say(sink, "%s %s. I3 will not run; I1 and I2 do, but their detail lines are gated on I3 and will not print.",
                ecp::prefixI3Down(), why);
        }
    }
    g_relayGate.store(g_i3 == I3State::Armed ? 1u : 0u, std::memory_order_release);
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
    g_relayGate.store(0, std::memory_order_release);   // the game's call runs straight through to the original
    g_i2On.store(false, std::memory_order_release);
    detail::g_vrCensusCameraNote = nullptr;
    say(sink, "%s (advanced.explorer_cam_probe turned off while running): the I3 relay's gate is closed (the hook stays in place, "
              "inert), I1 and I2 are detached.", ecp::prefixOff());
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
// synthetic target in place of the game's function.
namespace explorercamprobetest {
void setTarget(uintptr_t target) { g_testTarget = target; }
void boundary(uint32_t frame, uint64_t nowMs, bool want, ecp::SinkFn fn, void* ctx) { boundaryAt(frame, nowMs, want, ecp::Sink{fn, ctx}); }
ecp::Shared& shared() { return g_shared; }
ecp::SkinTee& skin() { return g_skin; }
ecp::CamTee& cam() { return g_cam; }
size_t stolenBytes() { return g_hook.hook.stolenBytes(); }
bool gateOpen() { return g_relayGate.load() != 0; }
// Back to a session that has not tried the hook: the CodeHook is uninstalled (the original bytes return), the one-shot latches clear.
void reset() {
    g_hook.hook.uninstall();
    g_hook.relay = nullptr;   // process-lifetime storage by design: the rig leaks the page
    g_hook.forward.store(0);
    g_hook.target = 0;
    g_i3 = I3State::NotTried;
    g_on = false;
    g_offLogged = false;
    g_announced = false;
    g_relayGate.store(0);
    g_i2On.store(false);
    detail::g_vrCensusCameraNote = nullptr;
}
void observe(void* a) { observeActivity(a); }
}  // namespace explorercamprobetest
#endif

}  // namespace edvr
