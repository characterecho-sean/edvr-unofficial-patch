#include "transition_flash_eye_base.h"
#include "transition_flash_eye_base_core.h"
#include "transition_flash_prevent_core.h"
#include "pose_reader_watch_core.h"
#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/log.h"

#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

namespace edvr {
namespace {

// --- Identity: build 332841, transition_flash_prevent.cpp's own pair. Kept
// as this file's own copy rather than a shared constant -- the copy-culture
// rule that file states for its own identity check, so neither is touched by
// a change in the other.
constexpr uint32_t kExpectedTimestamp = 1788384820u;
constexpr uint32_t kExpectedImageSize = 104894464u;

// The consumer, FUN_1428431d0 (design doc, "Static round 6"). First 16 bytes
// verified two ways: analysis\decomp\flash\r6\follow_28431D0.txt's own
// disassembly dump, and independently by walking analysis\
// EliteDangerous64.exe's own PE section table (RVA -> file offset via
// PointerToRawData) and reading the raw bytes -- see the task report, not
// committed here. `40 55 53 56 48 8D AC 24 50 E8 FF FF` is PUSH RBP; PUSH
// RBX; PUSH RSI; LEA RBP,[RSP-0x17B0] -- CodeHook steals all four
// instructions (12 bytes) to cover its 5-byte patch.
constexpr uintptr_t kConsumerRva = 0x28431D0u;
constexpr uint8_t kConsumerBytes[16] = {
    0x40, 0x55, 0x53, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0x50, 0xE8, 0xFF, 0xFF, 0xB8, 0xB0, 0x18, 0x00};

// CHANGE 6 (static round 7): the camera controller tick, FUN_1410730a0. Read-
// only counter hook -- it just names which frames the loop that calls the
// writer even ran. First 16 bytes verified the same two ways as the
// consumer's above: analysis\decomp\flash\r7\resolved_10730A0.txt's own
// disassembly dump, and independently by walking analysis\
// EliteDangerous64.exe's PE section table. `48 89 5C 24 10 48 89 74 24 18 55
// 57 41 56 48 8D` is MOV [RSP+0x10],RBX; MOV [RSP+0x18],RSI; PUSH RBP; PUSH
// RDI; PUSH R14; LEA RBP,[...] -- a clean, CodeHook-relocatable prologue
// (unlike the writer below). Ghidra's own signature recovery gives it one
// argument, FUN_1410730a0(longlong param_1) -- RCX only, no RDX/R8/R9 spill
// in its prologue beyond saving the non-volatile RBX/RSI into the caller's
// shadow space.
constexpr uintptr_t kControllerRva = 0x10730A0u;
constexpr uint8_t kControllerBytes[16] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8D};

// CHANGE 5 (static round 7): the writer, FUN_142874b20 (true entry; 239
// bytes, chained pdata). Verified the same two ways, against analysis\
// decomp\flash\r7\writer_true_2874B20.txt and the exe's own section table.
// Never CodeHooked -- `48 85 D2 0F 84 E5 00 00 00` is TEST RDX,RDX; JZ
// rel32, a pattern CodeHook's decoder refuses -- only ever addressed as a
// DR1 EXECUTE breakpoint target, so these bytes gate ARMING it, the same
// belt-and-suspenders role kConsumerBytes plays for its own CodeHook even
// though checkIdentity has already matched the whole module. Signature
// (Ghidra, writer_true_2874B20.txt): void FUN_142874b20(longlong param_1,
// longlong *param_2, undefined8 *param_3) -- RCX/RDX/R8, all three still
// exactly as the caller passed them at this, the function's very first
// instruction.
constexpr uintptr_t kWriterRva = 0x2874B20u;
constexpr uint8_t kWriterBytes[16] = {
    0x48, 0x85, 0xD2, 0x0F, 0x84, 0xE5, 0x00, 0x00, 0x00, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48};

constexpr uint32_t kSessionActCap = 60;
constexpr uint32_t kDumpWindowFrames = 60;
constexpr uint32_t kMaxDumpsPerSession = 12;
constexpr uint32_t kMaxShipPointerChangeLines = 16;
constexpr uint32_t kMaxReArms = 8;
// CHANGE 9 (2026-09-24): the mode-switch entry/exit log lines, the same
// session-cap idiom as kMaxShipPointerChangeLines above.
constexpr uint32_t kMaxModeSwitchEdgeLines = 40;
// CHANGE 8 (static round 7): raised from 180s/100,000 hits -- the previous
// bound, sized for round 6's discovery phase. A longer flight with the
// writer now identified wants more headroom before the watch self-disarms.
constexpr uint64_t kBoundArmedMs = 300000;     // 300s
constexpr uint64_t kBoundGameHits = 150000;
constexpr uint64_t kRearmSweepMs = 2000;       // ~2s, pose_reader_watch.cpp's own bound

const char* modeName(tfp::Mode m) noexcept {
    switch (m) {
    case tfp::Mode::Off: return "off";
    case tfp::Mode::Watch: return "watch";
    case tfp::Mode::On: return "on";
    case tfp::Mode::Alternate: return "alternate";
    }
    return "?";
}

// --- SEH-guarded reads/writes. Each is its own small function, mixing
// __try with this file's other locals (mutexes, std::string) being what
// CodeHook's own doc calls out as refused by the compiler --
// transition_flash_prevent.cpp's sehReadU64 and pose_reader_watch.cpp's
// sehReadBlock16 are the precedent, kept to one access each.
__declspec(noinline) bool sehReadU64(uintptr_t address, uint64_t& out) noexcept {
    __try { out = *reinterpret_cast<const uint64_t*>(address); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
__declspec(noinline) bool sehReadBlock64(uintptr_t address, float out[16]) noexcept {
    __try { std::memcpy(out, reinterpret_cast<const void*>(address), 16 * sizeof(float)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The act path's own write: F's 16 floats into the mailbox, before the
// original runs. A fault here just means the write did not happen -- acted
// stays false, nothing else is disturbed.
__declspec(noinline) bool sehWriteBlock64(uintptr_t address, const float in[16]) noexcept {
    __try { std::memcpy(reinterpret_cast<void*>(address), in, 16 * sizeof(float)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
__declspec(noinline) bool sehCheckBytes(uintptr_t address, const uint8_t* expected, size_t n) noexcept {
    __try { return std::memcmp(reinterpret_cast<const void*>(address), expected, n) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// PE TimeDateStamp + SizeOfImage, read the same way transition_flash_
// prevent.cpp's / pose_reader_watch.cpp's checkIdentity does (base+0x3C ->
// e_lfanew, +8 TimeDateStamp, +0x50 SizeOfImage).
__declspec(noinline) bool checkIdentity(uintptr_t base, const char** why) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) { *why = "PE header offset implausible"; return false; }
        uint32_t timestamp = 0, imageSize = 0;
        std::memcpy(&timestamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(&imageSize, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
        if (timestamp != kExpectedTimestamp || imageSize != kExpectedImageSize) {
            *why = "not build 332841 (PE timestamp/size mismatch)";
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "a read faulted while checking the PE header";
        return false;
    }
}

// --- Relay machinery, mirrored from transition_flash_prevent.cpp (itself
// mirrored from object_record_writer_hook.cpp / kinematic_eval_hook.cpp /
// pose_reader_watch.cpp). Kept as a copy rather than a shared unit so none of
// the flight-proven files are touched; if one changes, change all five.
// Needed because the target is in the GAME's module and this DLL loads more
// than two gigabytes away -- a plain five-byte E9 patch cannot reach a
// replacement here directly.
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
        if (end <= at) break; at = end;
    }
    return nullptr;
}

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    // mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback];
    // original: jmp [trampoline]. RAX/flags are volatile and the consumer
    // does not consume RAX on entry.
    const uint8_t body[kRelayBytes] = {
        0x48,0xB8,0,0,0,0,0,0,0,0, 0x48,0x83,0x38,0,
        0x74,0x0E, 0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0,
        0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &gateAddress, 8); std::memcpy(code + 22, &callbackAddress, 8);
}

struct HookEntry {
    const char* name;
    uintptr_t rva;
    void* callback;
    std::atomic<uintptr_t> forward{0};
    CodeHook hook;
    uint8_t* relay = nullptr;
    std::atomic<bool> ready{false};
    const char* failReason = "not attempted";
};

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

// Held open unconditionally once installation starts: the off/watch/on/
// alternate distinction is made inside the observed callback itself (one
// atomic load), matching transition_flash_prevent.cpp's g_relayGate
// discipline -- there is exactly one consumer of this one hook.
std::atomic<uintptr_t> g_relayGate{0};

bool installOne(HookEntry& entry, uintptr_t base, const uint8_t* expectedBytes) noexcept {
    if (!sehCheckBytes(base + entry.rva, expectedBytes, 16)) {
        entry.failReason = "prologue mismatch at this RVA (not build 332841's shape)";
        return false;
    }
    entry.relay = allocateRelay(base + entry.rva);
    if (!entry.relay) {
        entry.failReason = "relay allocation failed (no free memory within 2GB)";
        return false;
    }
    buildRelay(entry.relay, &g_relayGate, entry.callback);
    if (!entry.hook.install(reinterpret_cast<void*>(base + entry.rva), entry.relay, nullptr,
                            entry.name, &prepareRelay, &entry)) {
        VirtualFree(entry.relay, 0, MEM_RELEASE);
        entry.relay = nullptr;
        entry.failReason = "CodeHook refused it (see its own line above, tagged with this hook's name)";
        return false;
    }
    entry.ready.store(true, std::memory_order_release);
    return true;
    // Process-lifetime storage, same rule as every CodeHook user here: do not
    // free a relay or trampoline an in-flight call may already be executing.
}

// --- The detour. Verified argument count two ways: the decompiler's own
// signature recovery (void FUN_1428431d0(longlong,int,longlong), from
// analysing the callee body directly -- no R9/stack reference anywhere in
// it) and both direct callers' actual call sites: FUN_142868c30 passes
// exactly (this+0x1188, 2, *(this+0x11C0)); FUN_14287b030 passes exactly
// (rsi, r8+1, ...) with RCX/EDX freshly loaded right before the CALL and R8
// unchanged from whatever it already held -- an R9D zero immediately before
// that second call site is unrelated to it (the callee never reads R9; it is
// scheduled early for something after the call returns, ordinary MSVC
// instruction scheduling). No stack argument either way. Return value: void
// in the decompile, and neither caller reads RAX after the call.
using ConsumerFn = void (__fastcall*)(uintptr_t, int32_t, uintptr_t);
__declspec(noinline) void __fastcall consumerObserved(uintptr_t cameraObj, int32_t gameMode,
                                                       uintptr_t ptr3) noexcept;

HookEntry g_consumerEntry{"transition-flash-eye-base-consumer", kConsumerRva,
                          reinterpret_cast<void*>(&consumerObserved)};

// CHANGE 6: the controller counter. Read-only -- passes param_1 through
// unchanged and returns the original's own return value unchanged too
// (Ghidra's decompile of the return path is ambiguous about whether any
// caller reads it, unlike the consumer's -- safer to preserve it than to
// assume void).
using ControllerFn = uint64_t (__fastcall*)(uintptr_t);
__declspec(noinline) uint64_t __fastcall controllerObserved(uintptr_t param1) noexcept;

HookEntry g_controllerEntry{"transition-flash-eye-base-controller", kControllerRva,
                            reinterpret_cast<void*>(&controllerObserved)};

// --- Module state ------------------------------------------------------
std::atomic<uint8_t> g_fixMode{uint8_t(tfp::Mode::Off)};
std::atomic<bool> g_armed{false};       // install has been attempted (once)
std::atomic<bool> g_offLogged{false};
std::atomic<uintptr_t> g_base{0};
std::atomic<uint32_t> g_frame{0};
std::atomic<uint64_t> g_sequence{0};    // bumped once per consumer call; writer-table entries stamp it

// Validation / event state, shared under one mutex -- every unrefilled or
// validation-fold call touches at least the first two together, the same
// rule transition_flash_prevent.cpp's g_guardMutex states for its own set.
std::mutex g_guardMutex;
tfeb::EyeBaseValidation g_validation;
tfp::EventTracker g_eventTracker;
uint32_t g_lastActedFrame = 0;
bool g_hasActedFrame = false;

// Session counters. Monotonic; the periodic line and the shutdown summary
// both read a snapshot rather than resetting anything.
std::atomic<uint64_t> g_totalCalls{0};
std::atomic<uint64_t> g_totalUnrefilled{0};
std::atomic<uint64_t> g_eventsWatched{0}, g_eventsActed{0};
std::atomic<uint64_t> g_actedFrames{0};
std::atomic<uint64_t> g_shipPointerChangesTotal{0};
std::atomic<uint32_t> g_shipPointerChangeLinesLogged{0};
// CHANGE 9: the mode-switch ENTRY/EXIT log lines' own session cap counter,
// same idiom as g_shipPointerChangeLinesLogged just above.
std::atomic<uint32_t> g_modeSwitchEdgeLinesLogged{0};
std::atomic<uint64_t> g_modeHistogram[9] = {};  // 0-7, 8 = "other"; session total
// CHANGE 4: the same bucketing, but per real frame -- read-and-reset by
// transitionFlashEyeBaseNoteSceneCamera into the frame ring below, so the
// dump can show whether two callers (FUN_142868c30 mode 2, FUN_14287b030 its
// own mode) both consumed on one frame.
std::atomic<uint16_t> g_modeCountsThisFrame[9] = {};

void noteGameMode(int32_t gameMode) noexcept {
    const uint32_t slot = (gameMode >= 0 && gameMode <= 7) ? uint32_t(gameMode) : 8u;
    g_modeHistogram[slot].fetch_add(1, std::memory_order_relaxed);
    g_modeCountsThisFrame[slot].fetch_add(1, std::memory_order_relaxed);
}

// The ship pointer (*(cameraObj+0x50)), published every successful read so
// the frame boundary's writer-watch stability gate can observe it without
// touching game memory itself, and so the ring snapshot can report a change.
std::atomic<uint64_t> g_lastShip{0};
std::atomic<uint64_t> g_previousShipForChangeTest{0};

// CHANGE 1: the held base. The last mailbox M cached from a REFILLED mode!=1
// call -- the substitute an un-refilled call's act guard judges (transition_
// flash_eye_base_core.h's heldBaseRefusal), and what the per-call ring entry
// reports regardless of treatment, so a dump shows the candidate even for a
// watched or refused call.
std::mutex g_heldBaseMutex;
float g_heldBaseM[16] = {};
uint32_t g_heldBaseFrame = 0;
uint64_t g_heldBaseShip = 0;
bool g_haveHeldBase = false;

// CHANGE 11 (2026-09-24, the render-time patch sim's NEW-base candidate):
// the last REFILLED mode==2 mailbox, kept as the full 4x4 the consumer read,
// with the frame/ship/sequence it came from. Mode==2 only (the eye path,
// FUN_142868c30's caller), where the held base above deliberately takes any
// mode!=1: at a low-wake drop-in the writer's first post-skip refill -- the
// NEW base the scene at R wants -- lands on a mode==2 consume, and a
// non-eye mode's refill must not overwrite the candidate between the skip
// and the render. Shares g_heldBaseMutex: updated in the same refilled-call
// block, read from transitionFlashEyeBaseNoteSceneCamera's render-path sim.
float g_lastRefilledM[16] = {};
uint32_t g_lastRefilledFrame = 0;
uint64_t g_lastRefilledShip = 0;
uint64_t g_lastRefilledSequence = 0;
bool g_haveLastRefilled = false;

// CHANGE 12 (2026-09-24, "render-time patch simulation (watch-only)"): the
// pending sim, armed on a mode==2 mode-switch ENTRY edge in consumerObserved
// and fired from transitionFlashEyeBaseNoteSceneCamera on the render frames
// N+1..N+3. One slot: a new entry edge replaces an unfired pending sim and
// says so in the log. Purely passive -- nothing here ever writes game
// memory; the sim computes what the ACTING build (after this validation
// flies) would premultiply into the bad frame's eye and logs it, once per
// covered render frame, never per consume.
struct PatchSimPending {
    bool active = false;
    uint32_t skipFrame = 0;   // N: the un-refilled ENTRY-edge consume's frame
    float heldM[16] = {};     // the held base copied at arm time
    bool haveHeld = false;    // a held base existed at arm time
    // CHANGE 13: the ship pointer this skip happened on, stashed at arm
    // time from the arming call's own read. The tap-time live-mailbox probe
    // reads stashedShip+0x3330; stashing (rather than reading g_lastShip at
    // the tap) pins the same ship object the skip was judged on, immune to
    // ship-change races between arm and tap. 0 = unknown at arm time, probe
    // disabled.
    uint64_t ship = 0;
    // CHANGE 14: the locator's cross-frame state -- the most recent covered
    // fill whose row 275 looked head-only (the bad frame's signature)
    // produced this corrected origin/view-translation; the NEXT covered
    // frame's fills residual against it (the engine's own correct frame).
    float prevCorrOrigin[3] = {NAN, NAN, NAN};
    float prevCorrViewT[3] = {NAN, NAN, NAN};
    bool havePrevCorr = false;
};
std::mutex g_patchSimMutex;
PatchSimPending g_patchSim;
std::atomic<uint64_t> g_patchSimFired{0};      // sim frames logged (session)
std::atomic<uint64_t> g_patchSimReplaced{0};   // pending sims an entry edge replaced
std::atomic<uint64_t> g_patchSimExpired{0};    // windows that passed with no covered scene frame
// CHANGE 14: cheap mirrors of the pending state for the per-fill CB tap's
// early-out (60+ fills a frame must not take the mutex when nothing is
// armed). Written only under g_patchSimMutex, at arm and at retire/expire.
std::atomic<bool> g_patchSimPending{false};
std::atomic<uint32_t> g_patchSimPendingSkip{0};

// CHANGE 14: the buffer-row locator's per-tap-frame accumulation, consumed
// by the boundary sim block (which logs it one line per covered frame).
// Guarded by g_patchSimMutex. `tapFrame` is the PRE-advance counter the
// fills carried (s->frameNo at Unmap): the boundary that closes that frame
// records it one higher, so the consumption key is tapFrame+1.
struct PatchSimCBFillDetail {
    float row275[3] = {NAN, NAN, NAN};
    int32_t viewRow = -1;         // the located current view's float4 row
    float originMatch = NAN;      // its |t + origin.R|
    uint8_t liveState = 0;        // PatchSimLiveState at this fill
};
constexpr uint32_t kCBAccumRow275Max = 64;    // fills' row 275 kept for the boundary's P-match
constexpr uint32_t kCBAccumDetailMax = 4;     // per-fill dump detail lines
struct PatchSimCBAccum {
    uint32_t tapFrame = 0xFFFFFFFFu;
    uint32_t fillsSeen = 0;
    uint32_t stored = 0;                       // row275 entries actually stored (<= kCBAccumRow275Max)
    float row275[kCBAccumRow275Max][3] = {};   // first kCBAccumRow275Max fills' eye origins
    uint32_t detailCount = 0;
    PatchSimCBFillDetail detail[kCBAccumDetailMax] = {};
    // The LAST analyzed fill's results (analysis needs a usable live base):
    bool analyzed = false;
    uint8_t lastLiveState = 0;                 // PatchSimLiveState of the most recent fill
    tfeb::SceneCBViewFind find = {};
    float correctedOrigin[3] = {NAN, NAN, NAN};
    float correctedViewT[3] = {NAN, NAN, NAN};
    bool haveCorrection = false;
    // The cross-frame residual vs the previous covered frame's correction:
    bool crossDone = false;
    float crossOrigin = NAN;
    float crossView = NAN;
};
PatchSimCBAccum g_cbAccum;

// CHANGE 14: the locator's tolerances. The view's t = -R.origin holds to
// float rounding (~1e-4) for the CURRENT view; a previous frame's view is
// metres to kilometres off at a transition, and |row275| under a metre is
// the head-only signature of the bad frame (the same 1 m head volume
// glitch_scene.h's CameraReset test uses).
constexpr float kCBViewOrthoTol = 0.01f;
constexpr float kCBViewOriginTol = 0.5f;
constexpr float kCBMatchedPTol = 0.5f;
constexpr float kCBHeadOnlyRadius2 = 1.0f;   // |row275|^2 under this = head-only

// CHANGE 13: the tap-time live-mailbox probe's outcome, printed as the
// line's live= token and stored in the dump's sim row. Unavailable covers
// every non-usable case alike (null stashed ship, SEH read fault, failed
// plausibility gate) -- the probe is a bonus, and one token is enough to
// say it gave nothing; RESET is its own case because it is the answer to
// the open question (does the tap land inside the consumer's reset
// window?).
enum PatchSimLiveState : uint8_t { LiveUnavailable = 0, LiveOk = 1, LiveReset = 2 };

const char* patchSimLiveStateText(uint8_t s) noexcept {
    return s == LiveOk ? "ok" : s == LiveReset ? "RESET" : "n/a";
}

void armPatchSim(uint32_t frame, const float heldM[16], bool haveHeldBase, uint64_t ship) noexcept {
    uint32_t replacedSkip = 0;
    bool replaced = false;
    {
        std::lock_guard<std::mutex> lock(g_patchSimMutex);
        replaced = g_patchSim.active;
        if (replaced) replacedSkip = g_patchSim.skipFrame;
        g_patchSim.active = true;
        g_patchSim.skipFrame = frame;
        g_patchSim.haveHeld = haveHeldBase;
        if (haveHeldBase) std::memcpy(g_patchSim.heldM, heldM, sizeof(g_patchSim.heldM));
        g_patchSim.ship = ship;
        // CHANGE 14: a fresh sim starts a fresh cross-frame chain and a
        // fresh CB accumulation; the mirrors publish the armed state for
        // the per-fill tap's lock-free early-out.
        g_patchSim.havePrevCorr = false;
        g_cbAccum = PatchSimCBAccum{};
        g_patchSimPending.store(true, std::memory_order_release);
        g_patchSimPendingSkip.store(frame, std::memory_order_release);
    }
    if (replaced) {
        g_patchSimReplaced.fetch_add(1, std::memory_order_relaxed);
        Log::get().note(
            "transition flash eye base: patch sim armed at frame %u, replacing an unfired pending "
            "sim (skip %u).",
            frame, replacedSkip);
    }
}

// CHANGE 2: the writer-watch gate's own half of "flight, with the writer
// active" -- the consumer's count of consecutive refilled mode!=1 calls,
// read by frameBoundaryWriterWatch beside the existing ship-pointer
// stability gate. Updated with a CAS loop, not a plain load/store: the
// design doc's own two callers (FUN_142868c30, FUN_14287b030) can both
// consume on one frame, so this is not single-writer.
std::atomic<uint32_t> g_wwConsecutiveRefilled{0};

// CHANGE 9: the mirror of g_wwConsecutiveRefilled above -- the run of
// consecutive UN-REFILLED mode!=1 calls, which classifyModeSwitchEdge reads
// to tell a mode-switch ENTRY/EXIT from an ordinary call inside the run. Same
// CAS-loop, not-single-writer rationale as g_wwConsecutiveRefilled states for
// itself.
std::atomic<uint32_t> g_ebConsecutiveUnrefilled{0};

// Per-frame accumulators for glitch_frame.cpp's RingEntry (read-and-reset,
// poseReaderWatchFrameSnapshot's own convention). Touched from whichever
// thread calls the consumer (a scheduler job thread, the design doc says,
// not necessarily the render thread); read and cleared only from
// transitionFlashEyeBaseFrameSnapshot, called once per real frame from
// glitch_frame.cpp ahead of transitionFlashEyeBaseFrameBoundary in
// vscreen.cpp's own call order -- transition_flash_prevent.cpp's H3Accum is
// the same finalise-then-reset shape.
std::atomic<uint16_t> g_callsThisFrame{0};
std::atomic<uint16_t> g_unrefilledThisFrame{0};
std::atomic<uint8_t> g_treatmentThisFrame{0};
std::atomic<bool> g_writerSinceLastConsumeThisFrame{false};
std::atomic<bool> g_shipChangedThisFrame{false};
std::mutex g_lastCallMutex;
float g_lastCallM[3] = {0, 0, 0};
float g_lastCallF[3] = {0, 0, 0};
bool g_haveLastCall = false;

// Read-and-reset by the consumer hook itself: set by the writer watch's VEH
// handler on any WRITER hit (not a consumer-reset hit) since the previous
// consumer call.
std::atomic<bool> g_writerHitSincePriorConsume{false};

// CHANGE 5/7: the two round-7 "since previous consume" flags the substitute
// policy reads, read-and-reset by consumerObserved the same way as
// g_writerHitSincePriorConsume above. Entered: the DR1 handler saw the
// writer's own entry with ship_w == our ship. Wrote: a DR0 hit landed inside
// the writer's own body extent (tfeb::rvaInsideWriterExtent) -- its name
// gate let the copy through.
std::atomic<bool> g_writerEnteredSincePriorConsume{false};
std::atomic<bool> g_writerWroteSincePriorConsume{false};

// CHANGE 6/8: per-frame and session counters for the controller and the
// writer's own DR1/DR0 activity -- read-and-reset into the frame ring by
// transitionFlashEyeBaseNoteSceneCamera, peeked (not reset) by
// consumerObserved's per-event log line, and (the totals) printed once per
// dump. "ThisFrame" counters are not single-writer (two callers can consume
// on one frame; the controller and writer can each fire more than once
// too), so all six are atomics.
std::atomic<uint32_t> g_controllerCallsThisFrame{0};
std::atomic<uint64_t> g_controllerCallsTotal{0};
std::atomic<uint32_t> g_writerEnteredThisFrame{0};
std::atomic<uint64_t> g_writerEnteredTotal{0};
std::atomic<uint32_t> g_writerWroteThisFrame{0};
std::atomic<uint64_t> g_writerWroteTotal{0};

// CHANGE 5: the offered-matrix tear-free slot. A seqlock: an odd sequence
// means a publish is in progress; readOffered retries rather than blocking
// -- nothing in a VEH may take a lock the excepted thread could already
// hold, and nothing here allocates. Written only from writerWatchVeh (DR1);
// read from consumerObserved and transitionFlashEyeBaseNoteSceneCamera, both
// well outside the VEH.
struct OfferedSlot {
    std::atomic<uint32_t> seq{0};
    float m[16] = {};
    uint32_t frame = 0;
    uint64_t sequence = 0;
};
OfferedSlot g_offeredSlot;

// __declspec(noinline): kept small and out of the VEH's own inlined shape,
// the same rule this file's SEH helpers state for themselves.
__declspec(noinline) void publishOffered(const float m[16], uint32_t frame, uint64_t sequence) noexcept {
    const uint32_t s = g_offeredSlot.seq.load(std::memory_order_relaxed);
    g_offeredSlot.seq.store(s + 1, std::memory_order_release);
    std::memcpy(g_offeredSlot.m, m, sizeof(g_offeredSlot.m));
    g_offeredSlot.frame = frame;
    g_offeredSlot.sequence = sequence;
    g_offeredSlot.seq.store(s + 2, std::memory_order_release);
}

// Up to 4 attempts: a VEH-side publish in flight is a handful of stores, not
// a stall, so a torn read here means "try again", not "give up". Returns
// false when nothing has ever been published (seq still 0) or every attempt
// raced a publish.
__declspec(noinline) bool readOffered(float outM[16], uint32_t& outFrame, uint64_t& outSequence) noexcept {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t s1 = g_offeredSlot.seq.load(std::memory_order_acquire);
        if (s1 == 0) return false;
        if (s1 & 1u) continue;
        float m[16];
        std::memcpy(m, g_offeredSlot.m, sizeof(m));
        const uint32_t frame = g_offeredSlot.frame;
        const uint64_t sequence = g_offeredSlot.sequence;
        const uint32_t s2 = g_offeredSlot.seq.load(std::memory_order_acquire);
        if (s1 == s2) {
            std::memcpy(outM, m, sizeof(m));
            outFrame = frame;
            outSequence = sequence;
            return true;
        }
    }
    return false;
}

// --- The ring: one entry per consumer call, fixed capacity. A concurrent
// dump can read a slot mid-write (this file's hot-path writer is not
// serialised against the dump), the same accepted risk transition_flash_
// prevent.cpp's own ring states for the same reason.
// CHANGE 8 (static round 7): 8192 -> 65536. Flight 073114's event #2 dump
// was written a minute later than its trigger and its window had already
// wrapped past 8192 entries of ordinary refilled-call traffic; 65536 (96
// bytes/entry, ~6 MiB total -- see the static_assert below) gives far more
// headroom against exactly that.
constexpr uint32_t kEyeBaseRingCapacity = 65536;

struct EyeBaseRingEntry {
    uint32_t frame = 0;
    uint64_t sequence = 0;
    uint32_t threadId = 0;
    int32_t gameMode = 0;
    bool unrefilled = false;
    uint8_t treatment = 0;       // 0 none, 1 watched, 2 acted
    bool writerSinceLastConsume = false;
    bool shipChanged = false;
    uint64_t ship = 0;
    float m[3] = {0, 0, 0};
    float f[3] = {0, 0, 0};      // F is read and logged for the record only (CHANGE 1)
    float cachedM[3] = {0, 0, 0};    // the held base at the time of this call
    int32_t cachedAgeFrames = -1;    // frames since cached; -1 = no cache yet
    double dt = 0.0, dr = 0.0;   // only meaningful when a validation fold ran this call
};

static_assert(sizeof(EyeBaseRingEntry) * uint64_t(kEyeBaseRingCapacity) <= 8ull * 1024 * 1024,
             "transition flash eye base: the ring must stay small");

std::atomic<EyeBaseRingEntry*> g_ring{nullptr};
std::atomic<uint64_t> g_ringHead{0};

void pushRing(const EyeBaseRingEntry& e) noexcept {
    EyeBaseRingEntry* const ring = g_ring.load(std::memory_order_acquire);
    if (!ring) return;
    const uint64_t slot = g_ringHead.fetch_add(1, std::memory_order_relaxed);
    ring[slot % kEyeBaseRingCapacity] = e;
}

// --- The frame ring: one entry per REAL frame (not per call), pushed from
// transitionFlashEyeBaseNoteSceneCamera -- CHANGE 3/4's per-frame dump rows
// (the detector's scene camera, the writer hits, and the mode breakdown that
// frame). Same concurrent-read-during-write acceptance as the call ring
// above; there is only ever one writer (glitch_frame.cpp's own per-frame
// tap), so unlike the call ring this one needs no fetch_add race tolerance
// beyond that already-accepted read.
constexpr uint32_t kEyeBaseFrameRingCapacity = 4096;

struct EyeBaseFrameRecord {
    uint32_t frame = 0;
    bool sceneValid = false;
    float scenePos[3] = {NAN, NAN, NAN};
    uint32_t writerHits = 0;
    uint16_t calls = 0;
    uint16_t modeCounts[9] = {};
    // CHANGE 8: static round 7's per-frame classification -- the controller's
    // own call count, how many of those entries were the writer for OUR ship
    // (DR1), how many of those actually wrote (DR0 inside the writer's own
    // body), and the last offered matrix's translation THIS frame (NAN when
    // nothing was offered this frame -- see transitionFlashEyeBaseNoteScene-
    // Camera's own read).
    uint32_t controllerCalls = 0;
    uint32_t writerEntered = 0;
    uint32_t writerWrote = 0;
    float offeredTranslation[3] = {NAN, NAN, NAN};
    // CHANGE 9 (2026-09-24): the detector's own per-frame geometry
    // (glitch_scene.h's GlitchSceneGeometry -- matched/predicted point
    // counts, the camera's and the object pool's own step, and how far the
    // two disagree) and the decision it fed, folded in beside the scene
    // camera above so the object side and the camera side of one frame read
    // on one line. `geometryFresh` is false on a frame the pool was not
    // sampled on -- distinct from a real, freshly-measured all-zero
    // geometry (recordScenePosition's own freshness gate, glitch_frame.cpp).
    GlitchSceneGeometry geometry;
    bool geometryFresh = false;
    GlitchSceneDecision decision = GlitchSceneDecision::Unknown;
    // CHANGE 12: this frame's patch-sim outcome, when a pending sim's window
    // covered it and the scene position was valid. Folded in beside the
    // geometry the choice was computed from, so the dump's sim section can
    // replay exactly what the log line said. NAN defaults mean "no sim this
    // frame".
    bool patchSim = false;
    uint32_t patchSimSkipFrame = 0;
    float patchSimP[3] = {NAN, NAN, NAN};
    float patchSimHeld[3] = {NAN, NAN, NAN};
    float patchSimNew[3] = {NAN, NAN, NAN};
    // CHANGE 13: the tap-time live-mailbox probe's outcome -- the patched
    // origin when the live base was usable, the live mailbox's own raw
    // translation when the read itself succeeded (RESET zeros included, so a
    // reset-window tap still shows on the record), and the probe state as a
    // PatchSimLiveState (0 = unavailable, 1 = ok, 2 = RESET).
    float patchSimLive[3] = {NAN, NAN, NAN};
    float patchSimLiveT[3] = {NAN, NAN, NAN};
    uint8_t patchSimLiveState = 0;
    // CHANGE 14: the buffer-row locator's boundary-consumed outcome for this
    // frame (NAN/-1 defaults = no CB tap landed on it).
    uint32_t patchSimCBFills = 0;
    uint32_t patchSimCBMatched = 0;
    int32_t patchSimCBTapFrame = -1;
    int32_t patchSimCBViewRow = -1;
    float patchSimCBCorrO[3] = {NAN, NAN, NAN};
    float patchSimCBCrossO = NAN;
    float patchSimCBCrossV = NAN;
    uint8_t patchSimCBLiveState = 0;
    uint32_t patchSimCBDetailCount = 0;
    PatchSimCBFillDetail patchSimCBDetail[kCBAccumDetailMax] = {};
    float patchSimCamStep = 0.0f;
    float patchSimPoolStep = 0.0f;
    uint8_t patchSimChoice = 0;   // tfeb::SceneChoice (held as uint8_t; 0 = Old)
    int32_t patchSimRefilledAge = -1;
    bool patchSimHaveHeld = false;
    bool patchSimHaveNew = false;
};

// CHANGE 14 raised the bound 1 MiB -> 2 MiB: the record gained the locator's
// boundary-consumed outcome plus four per-fill details (~90 bytes), and
// 4096 x that is ~1.4 MiB -- still small next to the call ring's 6 MiB, and
// still bounded here rather than silently growing.
static_assert(sizeof(EyeBaseFrameRecord) * uint64_t(kEyeBaseFrameRingCapacity) <= 2ull * 1024 * 1024,
             "transition flash eye base: the frame ring must stay small");

std::atomic<EyeBaseFrameRecord*> g_frameRing{nullptr};
std::atomic<uint64_t> g_frameRingHead{0};

void pushFrameRing(const EyeBaseFrameRecord& r) noexcept {
    EyeBaseFrameRecord* const ring = g_frameRing.load(std::memory_order_acquire);
    if (!ring) return;
    const uint64_t slot = g_frameRingHead.fetch_add(1, std::memory_order_relaxed);
    ring[slot % kEyeBaseFrameRingCapacity] = r;
}

// --- Dumps: one pending slot, serviced from the frame boundary (never from
// inside the game hook). Mirrors transition_flash_prevent.cpp's PendingDump/
// requestDump/performDump/serviceDump shape -- its own copy, its own file
// family (edvr_logs\flash\eyebase_HHMMSS_fN.txt), and its own ±60-frame
// window and 12-dump cap, both wider/narrower than that file's own 30/16.
struct PendingDump {
    tfp::PendingDumpWindow window;
    static constexpr uint32_t kMaxReasons = 4;
    const char* reasons[kMaxReasons] = {};
    uint32_t reasonCount = 0;
};
std::mutex g_dumpMutex;
PendingDump g_pendingDump;
std::atomic<uint32_t> g_dumpsThisSession{0};

void addReason(PendingDump& d, const char* reason) noexcept {
    if (!reason) return;
    for (uint32_t i = 0; i < d.reasonCount; ++i) {
        if (std::strcmp(d.reasons[i], reason) == 0) return;
    }
    if (d.reasonCount < PendingDump::kMaxReasons) d.reasons[d.reasonCount++] = reason;
}

void requestDump(const char* reason, uint32_t triggerFrame) noexcept {
    std::lock_guard<std::mutex> lock(g_dumpMutex);
    const bool creatingNew = !g_pendingDump.window.active;
    if (creatingNew && g_dumpsThisSession.load(std::memory_order_relaxed) >= kMaxDumpsPerSession) return;
    g_pendingDump.window = tfp::foldDumpTrigger(g_pendingDump.window, triggerFrame, kDumpWindowFrames);
    addReason(g_pendingDump, reason);
}

void appendLine(std::string& out, const char* fmt, ...) noexcept {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) out.append(buf, static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n) : sizeof(buf) - 1);
    out += "\r\n";
}

// --- Part B forward declarations (defined below; performDump prints the
// writer table and mode histogram they own). ---------------------------
uint32_t writerTableCount() noexcept;
struct WriterTableEntry {
    uint64_t rip = 0;
    uint32_t count = 0;
    uint32_t firstFrame = 0;
    uint32_t lastFrame = 0;
    uint64_t lastSequence = 0;
    uint32_t unwindRvas[prw::kMaxUnwindFrames] = {};
    uint32_t unwindCount = 0;
};
WriterTableEntry writerTableEntry(uint32_t index) noexcept;

const char* treatmentText(uint8_t t) noexcept {
    return t == 2 ? "ACTED" : t == 1 ? "watched" : "none";
}

void performDump(const PendingDump& due) noexcept {
    const uint32_t dumpIndex = g_dumpsThisSession.fetch_add(1, std::memory_order_relaxed) + 1;

    std::string reasonsText;
    for (uint32_t i = 0; i < due.reasonCount; ++i) {
        if (i) reasonsText += ", ";
        reasonsText += due.reasons[i];
    }
    if (reasonsText.empty()) reasonsText = "unknown";

    std::string text;
    appendLine(text, "transition flash eye base: dump, reasons=(%s), requested trigger frames %u..%u",
              reasonsText.c_str(), due.window.lowTriggerFrame, due.window.dueFrame);

    // The writer table, stacks first (point 4 of the design doc's own
    // dump-reading convention, as eye_origin_trace's dump states it).
    const uint32_t writers = writerTableCount();
    appendLine(text, "writer table: %u of %u unique writer(s)", writers, prw::kMaxReaders);
    for (uint32_t i = 0; i < writers; ++i) {
        const WriterTableEntry w = writerTableEntry(i);
        std::string callers;
        for (uint32_t u = 0; u < w.unwindCount; ++u) {
            char piece[16];
            std::snprintf(piece, sizeof(piece), "%s0x%X", callers.empty() ? "" : "/", w.unwindRvas[u]);
            callers += piece;
        }
        if (callers.empty()) callers = "(none)";
        appendLine(text, "  #%u rip=0x%llX count=%u firstFrame=%u lastFrame=%u seq=%llu callers=%s",
                  i, (unsigned long long)w.rip, w.count, w.firstFrame, w.lastFrame,
                  (unsigned long long)w.lastSequence, callers.c_str());
    }

    // The mode histogram.
    appendLine(text, "mode histogram (game's own EDX, not this fix's mode):");
    for (uint32_t i = 0; i < 8; ++i) {
        const uint64_t c = g_modeHistogram[i].load(std::memory_order_relaxed);
        if (c) appendLine(text, "  mode=%u: %llu", i, (unsigned long long)c);
    }
    const uint64_t other = g_modeHistogram[8].load(std::memory_order_relaxed);
    if (other) appendLine(text, "  mode=other: %llu", (unsigned long long)other);

    // CHANGE 6/8: static round 7's session totals -- whether the controller
    // and the writer's own DR1/DR0 counters ever engaged at all this
    // session, before wading into the per-call/per-frame rows below.
    appendLine(text, "static round 7: controller_calls=%llu writer_entered=%llu writer_wrote=%llu (session totals)",
              (unsigned long long)g_controllerCallsTotal.load(std::memory_order_relaxed),
              (unsigned long long)g_writerEnteredTotal.load(std::memory_order_relaxed),
              (unsigned long long)g_writerWroteTotal.load(std::memory_order_relaxed));
    // CHANGE 12: the passive patch sim's own session totals, beside round 7's
    // above -- printed only once the sim has ever done anything, so dumps
    // from sessions without a covered transition stay byte-identical to the
    // pre-sim format.
    const uint64_t simFired = g_patchSimFired.load(std::memory_order_relaxed);
    const uint64_t simReplaced = g_patchSimReplaced.load(std::memory_order_relaxed);
    const uint64_t simExpired = g_patchSimExpired.load(std::memory_order_relaxed);
    if (simFired || simReplaced || simExpired) {
        appendLine(text, "patch sim (passive): fired=%llu replaced_pending=%llu expired=%llu (session totals)",
                  (unsigned long long)simFired, (unsigned long long)simReplaced,
                  (unsigned long long)simExpired);
    }

    EyeBaseRingEntry* const ring = g_ring.load(std::memory_order_acquire);
    const uint64_t head = g_ringHead.load(std::memory_order_relaxed);
    const uint64_t have = ring ? (head < kEyeBaseRingCapacity ? head : kEyeBaseRingCapacity) : 0;
    const uint64_t first = head - have;

    uint32_t printed = 0, lo = 0, hi = 0;
    bool any = false;
    for (uint64_t i = first; i < head; ++i) {
        const EyeBaseRingEntry e = ring[i % kEyeBaseRingCapacity];
        if (!tfp::frameInDumpWindow(e.frame, due.window.lowTriggerFrame, due.window.dueFrame,
                                    kDumpWindowFrames)) continue;
        if (!any) { lo = e.frame; hi = e.frame; any = true; }
        else { if (e.frame < lo) lo = e.frame; if (e.frame > hi) hi = e.frame; }
        ++printed;
        appendLine(text,
            "  f%-7u seq=%-8llu t%-6u mode=%-3d %-10s treat=%-7s ship=0x%llX "
            "M=(%+.3f %+.3f %+.3f) cachedM=(%+.3f %+.3f %+.3f) age=%-4d F=(%+.3f %+.3f %+.3f) "
            "dt=%.4f dr=%.6f wsince=%s shipchg=%s",
            e.frame, (unsigned long long)e.sequence, e.threadId, e.gameMode,
            e.unrefilled ? "UNREFILLED" : "refilled", treatmentText(e.treatment),
            (unsigned long long)e.ship, e.m[0], e.m[1], e.m[2], e.cachedM[0], e.cachedM[1], e.cachedM[2],
            e.cachedAgeFrames, e.f[0], e.f[1], e.f[2], e.dt, e.dr,
            e.writerSinceLastConsume ? "yes" : "no", e.shipChanged ? "yes" : "no");
    }
    if (!any) { lo = due.window.lowTriggerFrame; hi = due.window.dueFrame; }
    appendLine(text, "transition flash eye base: dump done, %u entries", printed);

    // CHANGE 3/4: the per-frame rows -- the detector's own scene camera
    // (cb1[275], independent of advanced.eye_origin_trace), the writer hits
    // that frame, and the mode breakdown (so the dump shows whether two
    // callers both consumed on one frame).
    appendLine(text, "per-frame rows:");
    EyeBaseFrameRecord* const frameRing = g_frameRing.load(std::memory_order_acquire);
    const uint64_t frameHead = g_frameRingHead.load(std::memory_order_relaxed);
    const uint64_t frameHave =
        frameRing ? (frameHead < kEyeBaseFrameRingCapacity ? frameHead : kEyeBaseFrameRingCapacity) : 0;
    const uint64_t frameFirst = frameHead - frameHave;
    uint32_t framePrinted = 0;
    uint32_t simPrinted = 0;
    std::string simText;
    for (uint64_t i = frameFirst; i < frameHead; ++i) {
        const EyeBaseFrameRecord r = frameRing[i % kEyeBaseFrameRingCapacity];
        if (!tfp::frameInDumpWindow(r.frame, due.window.lowTriggerFrame, due.window.dueFrame,
                                    kDumpWindowFrames)) continue;
        std::string modes;
        for (uint32_t md = 0; md < 9; ++md) {
            if (!r.modeCounts[md]) continue;
            char piece[24];
            if (md == 8) std::snprintf(piece, sizeof(piece), "%smode-other=%u", modes.empty() ? "" : " ", r.modeCounts[md]);
            else std::snprintf(piece, sizeof(piece), "%smode%u=%u", modes.empty() ? "" : " ", md, r.modeCounts[md]);
            modes += piece;
        }
        if (modes.empty()) modes = "(none)";
        ++framePrinted;
        // CHANGE 9: the detector's own per-frame geometry and decision,
        // beside the scene camera above -- the object side and the camera
        // side of this frame on one line. geom=STALE-(...) mirrors scene=
        // STALE-'s own prefix: the pool was not sampled this frame, so every
        // number in it is the zeroed default, not a measurement.
        appendLine(text,
            "  f%-7u calls=%-3u %-24s scene=%s(%+.2f %+.2f %+.2f) writerHits=%u "
            "controllerCalls=%u writerEntered=%u writerWrote=%u offered=(%+.3f %+.3f %+.3f) "
            "geom=%s(matched=%u predicted=%u cam=%.3f pool=%.3f relMed=%.3f relP90=%.3f predP90=%.3f) "
            "decision=%s",
            r.frame, r.calls, modes.c_str(), r.sceneValid ? "" : "STALE-",
            r.scenePos[0], r.scenePos[1], r.scenePos[2], r.writerHits,
            r.controllerCalls, r.writerEntered, r.writerWrote,
            r.offeredTranslation[0], r.offeredTranslation[1], r.offeredTranslation[2],
            glitchSceneGeometryFreshText(r.geometryFresh), r.geometry.matched, r.geometry.predicted,
            r.geometry.cameraStep, r.geometry.poolStep, r.geometry.relativeMedian, r.geometry.relativeP90,
            r.geometry.predictionP90, glitchSceneDecisionText(r.decision));
        // CHANGE 12: this frame's patch-sim outcome, printed into the dump's
        // own separate section (below "per-frame rows done") rather than
        // appended to the per-frame row -- the row format above is parsed
        // offline (tools\flash_patch_residual.py) and must not grow.
        if (r.patchSim) {
            ++simPrinted;
            appendLine(simText,
                "  patch-sim f%u (skip %u): P=(%+.3f %+.3f %+.3f) held->(%s%+.3f %+.3f %+.3f) "
                "new->(%s%+.3f %+.3f %+.3f) live->(%s%+.3f %+.3f %+.3f) liveT=(%+.3f %+.3f %+.3f) "
                "cam=%.3f pool=%.3f choice=%s->%s (refilled age=%d, live=%s)",
                r.frame, r.patchSimSkipFrame, r.patchSimP[0], r.patchSimP[1], r.patchSimP[2],
                r.patchSimHaveHeld ? "" : "n/a ", r.patchSimHeld[0], r.patchSimHeld[1], r.patchSimHeld[2],
                r.patchSimHaveNew ? "" : "n/a ", r.patchSimNew[0], r.patchSimNew[1], r.patchSimNew[2],
                r.patchSimLiveState == LiveOk ? "" : "n/a ",
                r.patchSimLive[0], r.patchSimLive[1], r.patchSimLive[2],
                r.patchSimLiveT[0], r.patchSimLiveT[1], r.patchSimLiveT[2],
                r.patchSimCamStep, r.patchSimPoolStep,
                tfeb::sceneChoiceText(static_cast<tfeb::SceneChoice>(r.patchSimChoice)),
                static_cast<tfeb::SceneChoice>(r.patchSimChoice) == tfeb::SceneChoice::New ? "new" : "held",
                r.patchSimRefilledAge, patchSimLiveStateText(r.patchSimLiveState));
            // CHANGE 14: the buffer-row locator's boundary-consumed outcome,
            // then the per-fill detail (the "patch-sim-cb" prefix keeps the
            // offline parser's row regex from ever matching these lines).
            if (r.patchSimCBFills) {
                char crossOText[24], crossVText[24];
                if (std::isnan(r.patchSimCBCrossO)) std::snprintf(crossOText, sizeof(crossOText), "n/a");
                else std::snprintf(crossOText, sizeof(crossOText), "%.3f", r.patchSimCBCrossO);
                if (std::isnan(r.patchSimCBCrossV)) std::snprintf(crossVText, sizeof(crossVText), "n/a");
                else std::snprintf(crossVText, sizeof(crossVText), "%.3f", r.patchSimCBCrossV);
                appendLine(simText,
                    "  patch-sim-cb f%u summary: fills=%u match275=%u tapf=%d view=%d "
                    "corrO=(%+.3f %+.3f %+.3f) crossO=%s crossV=%s live=%s",
                    r.frame, r.patchSimCBFills, r.patchSimCBMatched, r.patchSimCBTapFrame,
                    r.patchSimCBViewRow,
                    r.patchSimCBCorrO[0], r.patchSimCBCorrO[1], r.patchSimCBCorrO[2],
                    crossOText, crossVText,
                    patchSimLiveStateText(r.patchSimCBLiveState));
                for (uint32_t i = 0; i < r.patchSimCBDetailCount && i < kCBAccumDetailMax; ++i) {
                    const PatchSimCBFillDetail& d = r.patchSimCBDetail[i];
                    appendLine(simText,
                        "  patch-sim-cb f%u fill%u: row275=(%+.3f %+.3f %+.3f) view=%d "
                        "originMatch=%s%.4f live=%s",
                        r.frame, i, d.row275[0], d.row275[1], d.row275[2], d.viewRow,
                        d.viewRow < 0 ? "n/a " : "", d.originMatch,
                        patchSimLiveStateText(d.liveState));
                }
            }
        }
    }
    appendLine(text, "per-frame rows done, %u entries", framePrinted);
    // CHANGE 12: the sim section -- only when this dump's window saw any.
    // The leading "patch-sim" prefix keeps these lines from ever matching
    // the offline parser's per-row pattern.
    if (simPrinted) {
        appendLine(text, "patch sim rows:");
        text += simText;
        appendLine(text, "patch sim rows done, %u entries", simPrinted);
    }

    const std::wstring logDir = Log::get().dir();
    std::wstring path;
    bool wrote = false;
    if (!logDir.empty()) {
        const std::wstring dir = logDir + L"\\flash";
        CreateDirectoryW(dir.c_str(), nullptr);
        SYSTEMTIME stm{};
        GetLocalTime(&stm);
        wchar_t filename[64];
        _snwprintf_s(filename, _TRUNCATE, L"eyebase_%02u%02u%02u_f%u.txt",
                     static_cast<unsigned>(stm.wHour), static_cast<unsigned>(stm.wMinute),
                     static_cast<unsigned>(stm.wSecond), due.window.lowTriggerFrame);
        path = dir + L"\\" + filename;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            wrote = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0;
            CloseHandle(f);
        }
    }
    Log::get().note(
        "transition flash eye base: dump %u/%u (%s) frames %u..%u, %u call(s) %u frame(s) -> %ls",
        dumpIndex, kMaxDumpsPerSession, reasonsText.c_str(), lo, hi, printed, framePrinted,
        wrote ? path.c_str() : L"(could not write the file; see the log directory)");
}

void serviceDump(uint32_t nowFrame) noexcept {
    PendingDump due;
    {
        std::lock_guard<std::mutex> lock(g_dumpMutex);
        if (!g_pendingDump.window.active || nowFrame < g_pendingDump.window.dueFrame) return;
        due = g_pendingDump;
        g_pendingDump = PendingDump{};
    }
    performDump(due);
}

// =====================================================================
// Part B: the writer watch. A hardware WRITE watchpoint on the armed ship's
// +0x3360 (4 bytes), Dr7 RW0=01b/LEN0=11b/L0=1. Arming machinery mirrors
// pose_reader_watch.cpp's Part A2 (VEH, all-thread arming including the
// current thread via a helper, the ~2s re-arm sweep, RtlVirtualUnwind-based
// unwind, a deduped table) -- refactored here, and in pose_reader_watch.cpp
// (pose_reader_watch_core.h's armSlot0Dr7), to take the watch address and
// the Dr7 RW mode as explicit parameters instead of a single hardcoded
// global/constant, so this copy can ask for WRITE-only while that file's own
// copy keeps asking for READ-OR-WRITE, unchanged. Kept as a copy rather than
// a shared unit for the same reason the relay machinery above is: neither
// flight-proven file is touched by a change in the other.
// =====================================================================

std::atomic<uintptr_t> g_gameBase{0};
std::atomic<uintptr_t> g_gameSize{0};

// this identity check already ran (doInstallConsumerHook); reuse its result
// rather than re-parsing the PE headers a second time.
void publishGameModule(uintptr_t base) noexcept {
    g_gameBase.store(base, std::memory_order_relaxed);
    g_gameSize.store(kExpectedImageSize, std::memory_order_release);
}

constexpr uint32_t kMaxArmedThreads = 512;
struct ArmedThreadSet {
    DWORD ids[kMaxArmedThreads]{};
    uint32_t count = 0;
    bool contains(DWORD tid) const noexcept {
        for (uint32_t i = 0; i < count; ++i) if (ids[i] == tid) return true;
        return false;
    }
    bool add(DWORD tid) noexcept {
        if (contains(tid)) return true;
        if (count >= kMaxArmedThreads) return false;
        ids[count++] = tid;
        return true;
    }
    void clear() noexcept { count = 0; }
};
// Touched only from transitionFlashEyeBaseFrameBoundary's own thread (arm,
// the re-arm sweep, and disarm all run from there) -- no lock needed.
ArmedThreadSet g_wwArmedThreads;
uint64_t g_wwWatchAddress = 0;
// CHANGE 5: the writer's own entry, base + kWriterRva -- constant for the
// session once the module base is known, unlike g_wwWatchAddress above
// (ship-relative, re-armed on a ship pointer change). Set once by
// armWriterWatch. Read cross-thread by the helper thread the same way
// g_wwWatchAddress already is (setCurrentThreadDr/helperDrThreadProc) --
// this file's existing, accepted pattern for "set once before arming, never
// mutated concurrently with an armed read".
uint64_t g_wwWriterExecAddress = 0;
// Whether DR1 participates at all this session -- false when the writer's
// bytes did not verify at install (doInstall), in which case DR0's write
// watch still arms exactly as round 6 flew it and DR1 is simply never
// touched. Set once, before any arming; same cross-thread-read acceptance
// as g_wwWriterExecAddress.
bool g_wwArmSlot1 = false;

// DR0/Dr7 on one thread, by handle. `arm` true sets DR0=watchAddress and
// arms slot 0 in WRITE mode (prw::kDr7RwWrite); false clears just L0
// (prw::disarmSlot0Dr7). Suspend/GetContext/SetContext/Resume, exactly as
// pose_reader_watch.cpp's own setThreadDr, parameterized by rw mode. CHANGE
// 5: when g_wwArmSlot1 is set, slot 1 (DR1, EXECUTE, the writer's entry)
// arms and disarms in this exact same call, right alongside slot 0 -- the
// task's own rule, "both arm and disarm together". Slot 0's own bits and
// DR0 are untouched by this addition either way.
bool setThreadDr(DWORD tid, uint64_t watchAddress, uint32_t rwBits, bool arm) noexcept {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    if (!h) return false;
    bool ok = false;
    if (SuspendThread(h) != static_cast<DWORD>(-1)) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &ctx)) {
            const uint32_t lowDr7 = static_cast<uint32_t>(ctx.Dr7 & 0xFFFFFFFFull);
            const uint32_t highDr7 = static_cast<uint32_t>((ctx.Dr7 >> 32) & 0xFFFFFFFFull);
            uint32_t newLowDr7 = arm ? prw::armSlot0Dr7(lowDr7, rwBits) : prw::disarmSlot0Dr7(lowDr7);
            if (g_wwArmSlot1) {
                newLowDr7 = arm ? tfeb::armSlot1ExecuteDr7(newLowDr7) : tfeb::disarmSlot1Dr7(newLowDr7);
            }
            ctx.Dr7 = (static_cast<DWORD64>(highDr7) << 32) | newLowDr7;
            if (arm) {
                ctx.Dr0 = static_cast<DWORD64>(watchAddress);
                if (g_wwArmSlot1) ctx.Dr1 = static_cast<DWORD64>(g_wwWriterExecAddress);
            }
            ok = SetThreadContext(h, &ctx) != FALSE;
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    return ok;
}

// The current thread cannot usefully suspend itself; a short-lived helper
// thread does it from the outside, exactly as pose_reader_watch.cpp's own
// setCurrentThreadDr/helperDrThreadProc.
struct HelperDrArgs { DWORD targetTid; uint64_t watchAddress; uint32_t rwBits; bool arm; };

DWORD WINAPI helperDrThreadProc(LPVOID param) {
    std::unique_ptr<HelperDrArgs> args(static_cast<HelperDrArgs*>(param));
    setThreadDr(args->targetTid, args->watchAddress, args->rwBits, args->arm);
    return 0;
}

bool setCurrentThreadDr(uint64_t watchAddress, uint32_t rwBits, bool arm) noexcept {
    auto* args = new (std::nothrow) HelperDrArgs{GetCurrentThreadId(), watchAddress, rwBits, arm};
    if (!args) return false;
    HANDLE h = CreateThread(nullptr, 0, &helperDrThreadProc, args, 0, nullptr);
    if (!h) { delete args; return false; }
    WaitForSingleObject(h, 2000);
    CloseHandle(h);
    return true;
}

void sweepArmThreads() noexcept {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    const DWORD selfTid = GetCurrentThreadId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (g_wwArmedThreads.contains(te.th32ThreadID)) continue;
            const bool ok = (te.th32ThreadID == selfTid)
                ? setCurrentThreadDr(g_wwWatchAddress, prw::kDr7RwWrite, true)
                : setThreadDr(te.th32ThreadID, g_wwWatchAddress, prw::kDr7RwWrite, true);
            if (ok) g_wwArmedThreads.add(te.th32ThreadID);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

void disarmAllThreads() noexcept {
    const DWORD selfTid = GetCurrentThreadId();
    for (uint32_t i = 0; i < g_wwArmedThreads.count; ++i) {
        const DWORD tid = g_wwArmedThreads.ids[i];
        if (tid == selfTid) setCurrentThreadDr(g_wwWatchAddress, prw::kDr7RwWrite, false);
        else setThreadDr(tid, g_wwWatchAddress, prw::kDr7RwWrite, false);
    }
    g_wwArmedThreads.clear();
}

PVOID g_wwVehHandle = nullptr;

// --- The writer table. Preallocated, guarded by a plain spin, exactly
// pose_reader_watch.cpp's g_readerTable discipline: "the handler may not
// log, take a lock or resolve a module" (vtable_hook.cpp's writeWatch rule).
struct WriterEntry {
    prw::ReaderKey key;
    uint32_t count = 0;
    uint32_t firstFrame = 0;
    uint32_t lastFrame = 0;
    uint64_t lastSequence = 0;
    uint32_t unwindRvas[prw::kMaxUnwindFrames] = {};
    uint32_t unwindCount = 0;
};
WriterEntry g_writerTable[prw::kMaxReaders];
volatile LONG g_writerTableUsed = 0;
volatile LONG g_writerTableLock = 0;
std::atomic<uint32_t> g_frameWriterMask{0};
std::atomic<uint64_t> g_wwGameHitsTotal{0};
std::atomic<uint64_t> g_consumerResetHitsTotal{0};
std::atomic<uint64_t> g_writerHitsTotal{0};
// CHANGE 3/4: writer hits THIS frame (not the consumer's own reset writes --
// same exclusion as g_writerHitsTotal above), read-and-reset by
// transitionFlashEyeBaseNoteSceneCamera into the frame ring's row.
std::atomic<uint32_t> g_writerHitsThisFrame{0};

// RtlLookupFunctionEntry + RtlVirtualUnwind from a copy of the faulting
// context -- crash_context.h's emitUnwind shape, SEH-guarded, POD locals
// only, safe from a VEH handler. Self-contained (no coupling to the pose
// path beyond the shape), so this is pose_reader_watch.cpp's
// unwindGameFrames verbatim, its own copy for the same reason as the relay
// machinery above.
__declspec(noinline) uint32_t unwindGameFrames(const CONTEXT& startCtx, uint64_t gameBase, uint64_t gameSize,
                                               uint32_t* outRvas, uint32_t cap) noexcept {
    CONTEXT c = startCtx;
    uint32_t found = 0;
    for (uint32_t step = 0; step < prw::kMaxUnwindFrames && found < cap; ++step) {
        if (!c.Rip) break;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = nullptr;
        __try {
            function = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        const DWORD64 oldRip = c.Rip, oldRsp = c.Rsp;
        if (function) {
            DWORD64 establisher = 0;
            PVOID handlerData = nullptr;
            __try {
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, function, &c, &handlerData,
                                 &establisher, nullptr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
        } else {
            uintptr_t next = 0;
            bool readOk = false;
            __try {
                if (c.Rsp <= UINTPTR_MAX - sizeof(uintptr_t)) {
                    next = *reinterpret_cast<const uintptr_t*>(c.Rsp);
                    readOk = true;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                readOk = false;
            }
            if (!readOk) break;
            c.Rip = next;
            c.Rsp += sizeof(uintptr_t);
        }
        if (c.Rsp <= oldRsp || c.Rip == oldRip) break;
        if (c.Rip >= gameBase && c.Rip < gameBase + gameSize) {
            outRvas[found++] = static_cast<uint32_t>(c.Rip - gameBase);
        }
    }
    return found;
}

void recordWriterHit(const CONTEXT& originalCtx, uint64_t hitAddress) noexcept {
    const uint64_t base = g_gameBase.load(std::memory_order_relaxed);
    const uint64_t size = g_gameSize.load(std::memory_order_relaxed);
    const uint32_t frameNo = g_frame.load(std::memory_order_relaxed);
    g_wwGameHitsTotal.fetch_add(1, std::memory_order_relaxed);

    const uint64_t rva = hitAddress - base;
    if (tfeb::rvaInsideConsumerExtent(rva)) {
        // The consumer's own reset write (ship+0x3360 is part of the same
        // qword store that zeroes the mailbox's translation): count only.
        g_consumerResetHitsTotal.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // CHANGE 5: a write inside the KNOWN writer's own body (static round 7)
    // is the writer WRITING, not merely "some outside writer" -- tracked
    // specifically, in ADDITION to (not instead of) the generic writer table
    // below, which still names caller stacks for any writer this build has
    // not keyed by address.
    if (tfeb::rvaInsideWriterExtent(rva)) {
        g_writerWroteThisFrame.fetch_add(1, std::memory_order_relaxed);
        g_writerWroteTotal.fetch_add(1, std::memory_order_relaxed);
        g_writerWroteSincePriorConsume.store(true, std::memory_order_relaxed);
    }

    prw::ReaderKey key{};
    key.rip = rva;
    uint32_t rvas[prw::kMaxUnwindFrames] = {};
    const uint32_t n = unwindGameFrames(originalCtx, base, size, rvas, prw::kMaxUnwindFrames);
    key.caller1 = n > 0 ? rvas[0] : 0;
    key.caller2 = n > 1 ? rvas[1] : 0;

    while (InterlockedCompareExchange(&g_writerTableLock, 1, 0) != 0) { /* short spin, no OS call */ }
    const uint32_t used = static_cast<uint32_t>(g_writerTableUsed);
    prw::ReaderKey keys[prw::kMaxReaders];
    for (uint32_t i = 0; i < used; ++i) keys[i] = g_writerTable[i].key;
    uint32_t idx = prw::findReaderSlot(keys, used, key);
    if (idx == prw::kMaxReaders && !prw::readerTableFull(used)) {
        idx = used;
        WriterEntry& e = g_writerTable[idx];
        e.key = key;
        e.count = 0;
        e.firstFrame = frameNo;
        e.lastFrame = frameNo;
        e.unwindCount = n;
        for (uint32_t i = 0; i < n; ++i) e.unwindRvas[i] = rvas[i];
        g_writerTableUsed = static_cast<LONG>(used + 1);
    }
    if (idx < prw::kMaxReaders) {
        WriterEntry& e = g_writerTable[idx];
        ++e.count;
        e.lastFrame = frameNo;
        e.lastSequence = g_sequence.load(std::memory_order_relaxed);
        g_frameWriterMask.fetch_or(1u << idx, std::memory_order_relaxed);
    }
    InterlockedExchange(&g_writerTableLock, 0);

    g_writerHitsTotal.fetch_add(1, std::memory_order_relaxed);
    g_writerHitsThisFrame.fetch_add(1, std::memory_order_relaxed);
    g_writerHitSincePriorConsume.store(true, std::memory_order_relaxed);
}

// Not noexcept: AddVectoredExceptionHandler's PVECTORED_EXCEPTION_HANDLER has
// no exception specification, matching pose_reader_watch.cpp's own handler.
LONG CALLBACK writerWatchVeh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t dr6 = static_cast<uint32_t>(ep->ContextRecord->Dr6 & 0xFFFFFFFFull);
    const bool slot0Hit = prw::dr6HasSlot0Hit(dr6);
    const bool slot1Hit = tfeb::dr6HasSlot1Hit(dr6);
    if (!slot0Hit && !slot1Hit) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Dr6 = 0;  // sticky; clear before anything else, same rule pose_reader_watch.cpp states

    const uint64_t base = g_gameBase.load(std::memory_order_relaxed);
    const uint64_t size = g_gameSize.load(std::memory_order_relaxed);
    const auto addr = reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress);

    if (slot0Hit && base && addr >= base && addr < base + size) {
        recordWriterHit(*ep->ContextRecord, addr);
    }
    // A slot-0 hit outside the game module (EDVR's own code touching the
    // same watched bytes some other way) is not a writer being hunted;
    // ignored, same as pose_reader_watch.cpp's g_edvrHitsIgnored path, just
    // without a dedicated counter -- Part B's bounded-logging list has no
    // line for it.

    if (slot1Hit) {
        // CHANGE 5: the writer's own entry. An EXECUTE breakpoint is a
        // FAULT, not a trap -- ExceptionAddress IS the writer's first
        // instruction, and RCX/RDX/R8 still hold param_1/param_2/param_3
        // exactly as the caller passed them; nothing has executed yet.
        // SEH-guarded: this is game memory, read from inside a VEH, and a
        // fault here must not crash the exception path itself.
        g_writerEnteredThisFrame.fetch_add(1, std::memory_order_relaxed);
        g_writerEnteredTotal.fetch_add(1, std::memory_order_relaxed);
        const uint64_t param1 = ep->ContextRecord->Rcx;
        const uint64_t param3 = ep->ContextRecord->R8;
        uint64_t shipW = 0;
        if (sehReadU64(param1 + 0x38, shipW) && shipW != 0 &&
            shipW == g_lastShip.load(std::memory_order_relaxed)) {
            g_writerEnteredSincePriorConsume.store(true, std::memory_order_relaxed);
            float offered[16];
            if (sehReadBlock64(param3, offered)) {
                publishOffered(offered, g_frame.load(std::memory_order_relaxed),
                               g_sequence.load(std::memory_order_relaxed));
            }
        }
        // RF (Resume Flag, bit 16 / 0x10000): without it, the CPU would
        // re-trap on this same instruction the instant execution resumes --
        // an execute breakpoint re-fires on its own address unless RF
        // suppresses that one re-trigger. DR0 needs no such flag: a data
        // (write) breakpoint is reported as a TRAP, after the faulting
        // instruction has already retired, so simply resuming already lands
        // on the next instruction -- exactly what the slot-0 path above has
        // always done, untouched here.
        ep->ContextRecord->EFlags |= 0x10000u;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void installVeh() noexcept {
    if (g_wwVehHandle) return;
    g_wwVehHandle = AddVectoredExceptionHandler(1, &writerWatchVeh);
}
void removeVeh() noexcept {
    if (!g_wwVehHandle) return;
    RemoveVectoredExceptionHandler(g_wwVehHandle);
    g_wwVehHandle = nullptr;
}

bool g_wwHwArmed = false;
bool g_wwHwDisarmedForSession = false;
uint64_t g_wwHwArmedAtMs = 0;
uint64_t g_wwLastRearmSweepMs = 0;
uint64_t g_wwArmedShip = 0;
uint32_t g_wwReArmCount = 0;
prw::StabilityState g_wwStability;

// CHANGE 2: `stableFrames`/`consecutiveRefilled` are the gate counts at the
// moment of this call, logged so the arm line names what satisfied it --
// not necessarily the exact gate-crossing values on a re-arm, which calls
// this again with whatever the counts are at that later moment.
void armWriterWatch(uint64_t shipPointer, uint32_t stableFrames, uint32_t consecutiveRefilled) noexcept {
    g_wwArmedShip = shipPointer;
    g_wwWatchAddress = shipPointer + 0x3360;
    // CHANGE 5: constant for the session (module base, not ship-relative) --
    // recomputed on every (re)arm for simplicity; cheap, and always correct
    // even across a ship-pointer re-arm.
    g_wwWriterExecAddress = g_gameBase.load(std::memory_order_relaxed) + kWriterRva;
    installVeh();
    sweepArmThreads();
    g_wwHwArmed = true;
    g_wwHwArmedAtMs = GetTickCount64();
    g_wwLastRearmSweepMs = g_wwHwArmedAtMs;
    Log::get().note(
        "transition flash eye base: writer watch armed at 0x%llX (ship 0x%llX + 0x3360), %u thread(s); "
        "DR1 %s at 0x%llX (writer entry); "
        "gate: ship pointer stable %u consecutive frame(s), %u consecutive refilled call(s).",
        (unsigned long long)g_wwWatchAddress, (unsigned long long)shipPointer, g_wwArmedThreads.count,
        g_wwArmSlot1 ? "armed" : "NOT armed (writer bytes did not verify)",
        (unsigned long long)g_wwWriterExecAddress, stableFrames, consecutiveRefilled);
}

void disarmWriterWatch(const char* reason) noexcept {
    disarmAllThreads();
    removeVeh();
    g_wwHwArmed = false;
    g_wwHwDisarmedForSession = true;
    Log::get().note("transition flash eye base: writer watch disarmed -- %s.", reason);
}

uint32_t writerTableCount() noexcept {
    while (InterlockedCompareExchange(&g_writerTableLock, 1, 0) != 0) { /* short spin */ }
    const uint32_t used = static_cast<uint32_t>(g_writerTableUsed);
    InterlockedExchange(&g_writerTableLock, 0);
    return used;
}

WriterTableEntry writerTableEntry(uint32_t index) noexcept {
    WriterTableEntry out{};
    while (InterlockedCompareExchange(&g_writerTableLock, 1, 0) != 0) { /* short spin */ }
    const uint32_t used = static_cast<uint32_t>(g_writerTableUsed);
    if (index < used) {
        const WriterEntry& e = g_writerTable[index];
        out.rip = e.key.rip;
        out.count = e.count;
        out.firstFrame = e.firstFrame;
        out.lastFrame = e.lastFrame;
        out.lastSequence = e.lastSequence;
        out.unwindCount = e.unwindCount;
        for (uint32_t i = 0; i < e.unwindCount && i < prw::kMaxUnwindFrames; ++i) out.unwindRvas[i] = e.unwindRvas[i];
    }
    InterlockedExchange(&g_writerTableLock, 0);
    return out;
}

// Called once a frame (transitionFlashEyeBaseFrameBoundary). Owns the whole
// writer-watch lifecycle: the self-contained flight gate (CHANGE 2; the
// 60-consecutive-frame ship-pointer stability gate AND 300 consecutive
// refilled consumer calls) before the first arm, the ~2s re-arm sweep for
// new threads, an immediate disarm/re-arm (capped at 8, logged) on a
// ship-pointer change, and the 180s/100,000-hit bound.
void frameBoundaryWriterWatch(uint32_t frameNo) noexcept {
    (void)frameNo;
    const tfp::Mode fixMode = static_cast<tfp::Mode>(g_fixMode.load(std::memory_order_relaxed));
    if (fixMode == tfp::Mode::Off) {
        if (g_wwHwArmed) disarmWriterWatch("advanced.transition_flash_eye_base turned off");
        return;
    }
    if (g_wwHwDisarmedForSession) return;

    const uint64_t currentShip = g_lastShip.load(std::memory_order_relaxed);
    g_wwStability = prw::observeAddress(g_wwStability, currentShip);

    const uint64_t now = GetTickCount64();

    if (g_wwHwArmed) {
        const uint64_t hits = g_wwGameHitsTotal.load(std::memory_order_relaxed);
        char reason[96];
        if (now - g_wwHwArmedAtMs >= kBoundArmedMs) {
            std::snprintf(reason, sizeof(reason), "%llus armed (the bound)",
                          static_cast<unsigned long long>((now - g_wwHwArmedAtMs) / 1000));
            disarmWriterWatch(reason);
        } else if (hits >= kBoundGameHits) {
            std::snprintf(reason, sizeof(reason), "%llu game hits (the bound)",
                          static_cast<unsigned long long>(hits));
            disarmWriterWatch(reason);
        } else if (currentShip != 0 && currentShip != g_wwArmedShip) {
            if (g_wwReArmCount < kMaxReArms) {
                const uint64_t oldShip = g_wwArmedShip;
                disarmAllThreads();
                ++g_wwReArmCount;
                Log::get().note(
                    "transition flash eye base: writer watch re-armed on a new ship pointer "
                    "(0x%llX -> 0x%llX), re-arm %u/%u.",
                    (unsigned long long)oldShip, (unsigned long long)currentShip, g_wwReArmCount, kMaxReArms);
                armWriterWatch(currentShip, g_wwStability.consecutive,
                               g_wwConsecutiveRefilled.load(std::memory_order_relaxed));
            } else {
                disarmWriterWatch("ship pointer changed and the re-arm cap (8) is reached");
            }
        } else if (now - g_wwLastRearmSweepMs >= kRearmSweepMs) {
            g_wwLastRearmSweepMs = now;
            sweepArmThreads();
        }
        return;
    }

    // CHANGE 2: self-contained -- no glitchFrameCameraValidated (pose_reader_
    // watch.cpp keeps its own dependency on it). The ship pointer's own
    // 60-consecutive-frame stability plus 300 consecutive refilled mode!=1
    // consumer calls together ARE "in flight, with the writer active",
    // whatever fix.transition_flash says.
    const uint32_t consecutiveRefilled = g_wwConsecutiveRefilled.load(std::memory_order_relaxed);
    if (tfeb::writerWatchGateSatisfied(prw::isStable(g_wwStability), consecutiveRefilled) && currentShip != 0) {
        armWriterWatch(currentShip, g_wwStability.consecutive, consecutiveRefilled);
    }
}

// Reads and resets the per-frame writer-hit mask, for glitch_frame.cpp's
// RingEntry (transitionFlashEyeBaseFrameSnapshot).
uint32_t takeFrameWriterMask() noexcept {
    return g_frameWriterMask.exchange(0, std::memory_order_relaxed);
}

// =====================================================================
// Part C: the controller counter (CHANGE 6). Read-only -- no substitute, no
// write, no dump trigger; it exists only so a frame can be classified
// (tfeb::classifyFrameWriter) instead of leaving "the writer never entered"
// ambiguous between "the loop didn't run" and "it ran but skip candidate 1
// fired".
// =====================================================================

uint64_t __fastcall controllerObserved(uintptr_t param1) noexcept {
    const auto forward = reinterpret_cast<ControllerFn>(g_controllerEntry.forward.load(std::memory_order_acquire));
    if (!forward) return 0;  // stood down at install; the relay is unreachable then
    if (static_cast<tfp::Mode>(g_fixMode.load(std::memory_order_relaxed)) != tfp::Mode::Off) {
        g_controllerCallsTotal.fetch_add(1, std::memory_order_relaxed);
        g_controllerCallsThisFrame.fetch_add(1, std::memory_order_relaxed);
    }
    return forward(param1);
}

// =====================================================================
// Part A: the consumer hook.
// =====================================================================

void __fastcall consumerObserved(uintptr_t cameraObj, int32_t gameMode, uintptr_t ptr3) noexcept {
    const auto forward = reinterpret_cast<ConsumerFn>(g_consumerEntry.forward.load(std::memory_order_acquire));
    if (!forward) return;  // stood down at install; the relay is unreachable then
    const tfp::Mode fixMode = static_cast<tfp::Mode>(g_fixMode.load(std::memory_order_relaxed));
    if (fixMode == tfp::Mode::Off) { forward(cameraObj, gameMode, ptr3); return; }

    const uint32_t frame = g_frame.load(std::memory_order_relaxed);
    const uint64_t seq = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    g_totalCalls.fetch_add(1, std::memory_order_relaxed);
    g_callsThisFrame.fetch_add(1, std::memory_order_relaxed);
    noteGameMode(gameMode);

    const bool writerSinceLast = g_writerHitSincePriorConsume.exchange(false, std::memory_order_acq_rel);
    g_writerSinceLastConsumeThisFrame.store(writerSinceLast, std::memory_order_relaxed);
    // CHANGE 7: the substitute policy's own two "since previous consume"
    // inputs -- see their declarations for what sets each.
    const bool writerEnteredSinceLast = g_writerEnteredSincePriorConsume.exchange(false, std::memory_order_acq_rel);
    const bool writerWroteSinceLast = g_writerWroteSincePriorConsume.exchange(false, std::memory_order_acq_rel);

    uint64_t ship = 0;
    const bool haveShip = sehReadU64(cameraObj + 0x50, ship);
    bool shipChanged = false;
    if (haveShip) {
        g_lastShip.store(ship, std::memory_order_relaxed);
        const uint64_t previous = g_previousShipForChangeTest.exchange(ship, std::memory_order_acq_rel);
        if (previous != 0 && previous != ship) {
            shipChanged = true;
            g_shipPointerChangesTotal.fetch_add(1, std::memory_order_relaxed);
            const uint32_t already = g_shipPointerChangeLinesLogged.fetch_add(1, std::memory_order_relaxed);
            if (already < kMaxShipPointerChangeLines) {
                Log::get().note(
                    "transition flash eye base: ship pointer changed at frame %u: 0x%llX -> 0x%llX (%u/%u logged).",
                    frame, (unsigned long long)previous, (unsigned long long)ship, already + 1,
                    kMaxShipPointerChangeLines);
            }
        }
    }
    g_shipChangedThisFrame.store(shipChanged, std::memory_order_relaxed);

    float m[16] = {}, f[16] = {};
    const bool haveM = haveShip && sehReadBlock64(ship + 0x3330, m);
    // CHANGE 1: F (ship+0x130) is read and logged for the record only -- it
    // no longer gates or supplies the act-path write.
    const bool haveF = haveShip && sehReadBlock64(ship + 0x130, f);

    bool unrefilled = false;
    uint8_t treatmentOutcome = 0;  // 0 none, 1 watched, 2 acted
    double dt = 0.0, dr = 0.0;
    // CHANGE 12: the mode-switch edge this call crossed, if any (filled by
    // the un-refilled CAS loop inside the haveM block below; the patch sim
    // arms on it after the held-base read, which lives outside that block).
    tfeb::ModeSwitchEdge modeSwitchEdge = tfeb::ModeSwitchEdge::None;

    if (haveM) {
        unrefilled = (gameMode != 1) && tfeb::isResetMailbox(m);

        // CHANGE 2: the writer-watch gate's consecutive-refilled count. A CAS
        // loop, not load-then-store: FUN_142868c30 and FUN_14287b030 can both
        // consume on one frame, so this is not single-writer.
        {
            uint32_t cur = g_wwConsecutiveRefilled.load(std::memory_order_relaxed);
            uint32_t next;
            do {
                next = tfeb::updateConsecutiveRefilled(cur, gameMode, unrefilled);
            } while (!g_wwConsecutiveRefilled.compare_exchange_weak(cur, next, std::memory_order_relaxed));
        }

        // CHANGE 9: the mirror CAS loop, tracking the run of consecutive
        // un-refilled mode!=1 calls instead, and firing the mode-switch
        // ENTRY/EXIT dump trigger and log line on the one call that crosses
        // the edge -- see tfeb::classifyModeSwitchEdge's own comment.
        // `cur` after the loop is the streak's value from the call that WON
        // the compare-exchange (compare_exchange_weak leaves it unchanged on
        // success), the same value edge/next were computed from -- exactly
        // "how many un-refilled consumes" for the EXIT log line below.
        // CHANGE 12: `modeSwitchEdge` is a function-scope local (declared
        // with the other per-call locals above) because the patch sim arms
        // on it below, after the held-base read outside this block.
        {
            uint32_t cur = g_ebConsecutiveUnrefilled.load(std::memory_order_relaxed);
            uint32_t next;
            tfeb::ModeSwitchEdge edge;
            do {
                edge = tfeb::classifyModeSwitchEdge(gameMode, unrefilled, cur);
                next = tfeb::updateConsecutiveUnrefilled(cur, gameMode, unrefilled);
            } while (!g_ebConsecutiveUnrefilled.compare_exchange_weak(cur, next, std::memory_order_relaxed));
            modeSwitchEdge = edge;
            if (edge == tfeb::ModeSwitchEdge::Entry) {
                requestDump("a mode-switch entry", frame);
                const uint32_t already = g_modeSwitchEdgeLinesLogged.fetch_add(1, std::memory_order_relaxed);
                if (already < kMaxModeSwitchEdgeLines) {
                    Log::get().note("transition flash eye base: mode switch ENTRY at frame %u", frame);
                }
            } else if (edge == tfeb::ModeSwitchEdge::Exit) {
                requestDump("a mode-switch exit", frame);
                const uint32_t already = g_modeSwitchEdgeLinesLogged.fetch_add(1, std::memory_order_relaxed);
                if (already < kMaxModeSwitchEdgeLines) {
                    Log::get().note(
                        "transition flash eye base: mode switch EXIT at frame %u after %u un-refilled consumes",
                        frame, cur);
                }
            }
        }

        // CHANGE 1: cache every refilled mode!=1 mailbox -- the held base an
        // un-refilled call's act guard judges below.
        if (gameMode != 1 && !unrefilled) {
            std::lock_guard<std::mutex> lock(g_heldBaseMutex);
            std::memcpy(g_heldBaseM, m, sizeof(g_heldBaseM));
            g_heldBaseFrame = frame;
            g_heldBaseShip = ship;
            g_haveHeldBase = true;
            // CHANGE 11: the render-time patch sim's NEW-base candidate --
            // mode==2 only, so a non-eye refilled consume cannot overwrite
            // the post-skip refill the scene at R wants before the render
            // reads it (see the globals' own comment).
            if (gameMode == 2) {
                std::memcpy(g_lastRefilledM, m, sizeof(g_lastRefilledM));
                g_lastRefilledFrame = frame;
                g_lastRefilledShip = ship;
                g_lastRefilledSequence = seq;
                g_haveLastRefilled = true;
            }
        }
    }

    // The held base as it now stands. For an un-refilled call the cache
    // above was untouched this call, so this is exactly the candidate the
    // act guard judges; for a refilled call it is simply M again, read back
    // only for the ring entry below.
    float heldM[16] = {};
    uint32_t heldFrame = 0;
    uint64_t heldShip = 0;
    bool haveHeldBase = false;
    {
        std::lock_guard<std::mutex> lock(g_heldBaseMutex);
        haveHeldBase = g_haveHeldBase;
        std::memcpy(heldM, g_heldBaseM, sizeof(heldM));
        heldFrame = g_heldBaseFrame;
        heldShip = g_heldBaseShip;
    }
    const int32_t heldAgeFrames =
        haveHeldBase ? (frame >= heldFrame ? static_cast<int32_t>(frame - heldFrame) : 0) : -1;

    // CHANGE 12: arm the render-time patch sim on a mode==2 ENTRY edge. The
    // entry classification ran in the un-refilled CAS loop above; heldM here
    // is exactly the candidate the act guard judged (this un-refilled call
    // left the cache untouched), which is the sim's "held" candidate. The
    // condition implies haveM: `unrefilled` is only ever set under haveM,
    // and ENTRY requires mode!=1. Runs in every non-off mode -- watch, on
    // and alternate alike -- and never writes game memory. CHANGE 13: the
    // ship local (0 when the +0x50 read faulted) rides along for the tap's
    // live-mailbox probe.
    if (unrefilled && gameMode == 2 && modeSwitchEdge == tfeb::ModeSwitchEdge::Entry) {
        armPatchSim(frame, heldM, haveHeldBase, ship);
    }

    if (haveM && unrefilled) {
        g_totalUnrefilled.fetch_add(1, std::memory_order_relaxed);
        g_unrefilledThisFrame.fetch_add(1, std::memory_order_relaxed);
        // CHANGE 9 replaced CHANGE 3's own per-call trigger here (every
        // un-refilled call dumped -- flight 091726's low wake alone was
        // 5,041 of them) with the mode-switch edge fired above, inside the
        // `if (haveM)` block. The detector's scene-judged verdict, this
        // predicate's other half, is untouched
        // (transitionFlashEyeBaseNoteDetectorVerdict below).

        bool isNewEvent = false;
        uint32_t eventNumber = 0;
        tfp::Treatment treatment = tfp::Treatment::Watch;
        {
            std::lock_guard<std::mutex> lock(g_guardMutex);
            const tfp::EventTracker::Note note = g_eventTracker.note(frame);
            isNewEvent = note.isNewEvent;
            eventNumber = note.eventNumber;
            if (isNewEvent) {
                treatment = tfp::modeAllowsActing(fixMode, eventNumber) ? tfp::Treatment::Act
                                                                        : tfp::Treatment::Watch;
                g_eventTracker.latch(treatment);
                if (treatment == tfp::Treatment::Act) g_eventsActed.fetch_add(1, std::memory_order_relaxed);
                else g_eventsWatched.fetch_add(1, std::memory_order_relaxed);
            } else {
                treatment = g_eventTracker.treatment();
            }
        }

        // CHANGE 7: the offered matrix -- what the writer was about to copy
        // in (param_3, captured by the DR1 handler at its entry), computed
        // unconditionally for every unrefilled call, watched or acted, the
        // same "show the candidate regardless of treatment" rule the held
        // base already follows below.
        float offeredM[16] = {};
        uint32_t offeredFrame = 0;
        uint64_t offeredSeq = 0;
        const bool haveOffered = readOffered(offeredM, offeredFrame, offeredSeq);
        const int32_t offeredAgeFrames =
            haveOffered ? (frame >= offeredFrame ? static_cast<int32_t>(frame - offeredFrame) : -1) : -1;
        const bool offeredUsable = tfeb::offeredSubstituteUsable(
            writerEnteredSinceLast, writerWroteSinceLast, haveOffered, frame, offeredFrame, offeredM);

        const uint64_t actedFrames = g_actedFrames.load(std::memory_order_relaxed);
        const bool capReached = tfp::sessionCapReached(actedFrames, kSessionActCap);
        const tfeb::HeldBaseRefusal heldRefusal = tfeb::heldBaseRefusal(
            treatment, haveHeldBase, frame, heldFrame, ship, heldShip, heldM, capReached);
        const bool mayActHeld = tfeb::heldBaseMayAct(heldRefusal);
        // The session act cap bounds both substitutes alike -- applied here
        // rather than inside offeredSubstituteUsable (its own five
        // conditions are exactly what the design doc's substitute policy
        // lists; the cap is CHANGE 1's own orthogonal safety valve).
        const bool mayActOffered = treatment == tfp::Treatment::Act && !capReached && offeredUsable;

        bool acted = false;
        const char* substituteUsed = "none";
        int32_t substituteAgeFrames = -1;
        const float* chosenM = nullptr;
        if (mayActOffered) {
            chosenM = offeredM;
            substituteUsed = "offered";
            substituteAgeFrames = offeredAgeFrames;
        } else if (mayActHeld) {
            chosenM = heldM;
            substituteUsed = "held";
            substituteAgeFrames = heldAgeFrames;
        }

        if (chosenM) {
            // Write the chosen substitute into the mailbox BEFORE the
            // original runs, exactly where F used to be written -- so the
            // engine snapshots it as the base and resets as usual.
            acted = sehWriteBlock64(ship + 0x3330, chosenM);
            if (acted) {
                std::lock_guard<std::mutex> lock(g_guardMutex);
                if (tfp::isNewActedFrame(frame, g_lastActedFrame, g_hasActedFrame)) {
                    g_actedFrames.fetch_add(1, std::memory_order_relaxed);
                    g_lastActedFrame = frame;
                    g_hasActedFrame = true;
                }
            }
        }
        treatmentOutcome = acted ? 2 : 1;

        if (isNewEvent) {
            // heldRefusal can be None here with acted==false only if the SEH
            // write itself faulted -- every guard passed but the store did
            // not happen. Named off heldRefusal even when it was the offered
            // matrix that was tried and faulted: the two substitutes write
            // through the same sehWriteBlock64 call, so a fault there is a
            // fault either way, and heldRefusal's own chain already covers
            // "watch slot"/"cap" for both.
            const char* reason = acted ? substituteUsed
                                : heldRefusal != tfeb::HeldBaseRefusal::None ? tfeb::heldBaseRefusalText(heldRefusal)
                                                                              : "write faulted";
            char heldText[64];
            if (haveHeldBase) {
                std::snprintf(heldText, sizeof(heldText), "(%+.3f %+.3f %+.3f) age=%d",
                              heldM[12], heldM[13], heldM[14], heldAgeFrames);
            } else {
                std::snprintf(heldText, sizeof(heldText), "none cached");
            }
            char offeredText[96];
            if (haveOffered) {
                std::snprintf(offeredText, sizeof(offeredText), "(%+.3f %+.3f %+.3f) age=%d%s",
                              offeredM[12], offeredM[13], offeredM[14], offeredAgeFrames,
                              offeredUsable ? "" : " (refused)");
            } else {
                std::snprintf(offeredText, sizeof(offeredText), "none captured");
            }
            char substText[96];
            if (chosenM) {
                std::snprintf(substText, sizeof(substText), "%s (%+.3f %+.3f %+.3f) age=%d",
                              substituteUsed, chosenM[12], chosenM[13], chosenM[14], substituteAgeFrames);
            } else {
                std::snprintf(substText, sizeof(substText), "none");
            }
            const uint32_t controllerCallsNow = g_controllerCallsThisFrame.load(std::memory_order_relaxed);
            const uint32_t writerEnteredNow = g_writerEnteredThisFrame.load(std::memory_order_relaxed);
            const uint32_t writerWroteNow = g_writerWroteThisFrame.load(std::memory_order_relaxed);
            Log::get().note(
                "transition flash eye base: event #%u frame %u %s -- %s (mode=%s). "
                "substitute=%s. held=%s. offered=%s. M=(%+.3f %+.3f %+.3f) F=(%+.3f %+.3f %+.3f). "
                "this frame: controller_calls=%u writer_entered=%u writer_wrote=%u.",
                eventNumber, frame, acted ? "ACTED" : "WATCHED", reason, modeName(fixMode),
                substText, heldText, offeredText,
                m[12], m[13], m[14], haveF ? f[12] : NAN, haveF ? f[13] : NAN, haveF ? f[14] : NAN,
                controllerCallsNow, writerEnteredNow, writerWroteNow);
        }
    } else if (haveM && haveF && gameMode != 1) {
        const tfeb::EyeBaseDelta d = tfeb::compareEyeBase(m, f);
        dt = d.dt; dr = d.dr;
        std::lock_guard<std::mutex> lock(g_guardMutex);
        if (tfeb::eyeBaseAgrees(d)) ++g_validation.agree; else ++g_validation.disagree;
    }

    g_treatmentThisFrame.store(treatmentOutcome, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_lastCallMutex);
        g_lastCallM[0] = m[12]; g_lastCallM[1] = m[13]; g_lastCallM[2] = m[14];
        g_lastCallF[0] = f[12]; g_lastCallF[1] = f[13]; g_lastCallF[2] = f[14];
        g_haveLastCall = haveM || haveF;
    }

    EyeBaseRingEntry e;
    e.frame = frame;
    e.sequence = seq;
    e.threadId = GetCurrentThreadId();
    e.gameMode = gameMode;
    e.unrefilled = unrefilled;
    e.treatment = treatmentOutcome;
    e.writerSinceLastConsume = writerSinceLast;
    e.shipChanged = shipChanged;
    e.ship = ship;
    e.m[0] = m[12]; e.m[1] = m[13]; e.m[2] = m[14];
    e.f[0] = f[12]; e.f[1] = f[13]; e.f[2] = f[14];
    e.cachedM[0] = heldM[12]; e.cachedM[1] = heldM[13]; e.cachedM[2] = heldM[14];
    e.cachedAgeFrames = heldAgeFrames;
    e.dt = dt; e.dr = dr;
    pushRing(e);

    forward(cameraObj, gameMode, ptr3);
}

// --- Install ------------------------------------------------------------

void doInstall(tfp::Mode mode) noexcept {
    if (!g_ring.load(std::memory_order_acquire)) {
        auto* ring = new (std::nothrow) EyeBaseRingEntry[kEyeBaseRingCapacity];
        if (ring) g_ring.store(ring, std::memory_order_release);
    }
    if (!g_frameRing.load(std::memory_order_acquire)) {
        auto* ring = new (std::nothrow) EyeBaseFrameRecord[kEyeBaseFrameRingCapacity];
        if (ring) g_frameRing.store(ring, std::memory_order_release);
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const char* why = nullptr;
    if (!base || !checkIdentity(base, &why)) {
        Log::get().note(
            "transition flash eye base: standing down entirely -- %s. Nothing installed, "
            "nothing changed.", why ? why : "module base unavailable");
        g_armed.store(true, std::memory_order_release);
        return;
    }
    g_base.store(base, std::memory_order_relaxed);
    g_relayGate.store(1, std::memory_order_relaxed);
    publishGameModule(base);

    const bool consumerOk = installOne(g_consumerEntry, base, kConsumerBytes);
    // CHANGE 6: the controller counter -- read-only, shares g_relayGate with
    // the consumer above (one master on/off switch for every hook this
    // module installs).
    const bool controllerOk = installOne(g_controllerEntry, base, kControllerBytes);
    // CHANGE 5: the writer is never CodeHooked (see kWriterBytes' own
    // comment), but its bytes are still verified before DR1 is ever allowed
    // to arm at its address -- the same belt-and-suspenders role installOne's
    // own sehCheckBytes plays for the consumer and controller even though
    // checkIdentity above already matched the whole module. A mismatch here
    // degrades gracefully: DR0's write watch (round 6, flight-proven) still
    // arms on its own; only DR1 stays off for the session.
    const bool writerBytesOk = sehCheckBytes(base + kWriterRva, kWriterBytes, sizeof(kWriterBytes));
    g_wwArmSlot1 = writerBytesOk;
    g_armed.store(true, std::memory_order_release);

    Log::get().note(
        "transition flash eye base: armed, mode=%s. identity: build match (timestamp %u, image "
        "%u bytes) -- OK. consumer 0x28431D0: %s. controller 0x10730A0: %s. writer 0x2874B20 "
        "(DR1 target) bytes: %s. Ring %u entries, session act cap %u frames.",
        modeName(mode), kExpectedTimestamp, kExpectedImageSize,
        consumerOk ? "installed" : g_consumerEntry.failReason,
        controllerOk ? "installed" : g_controllerEntry.failReason,
        writerBytesOk ? "verified" : "MISMATCH -- DR1 will not arm", kEyeBaseRingCapacity, kSessionActCap);
}

// --- Periodic (~20s) reporting, only when something moved -----------------
void reportLine(const char* prefix) noexcept {
    tfeb::EyeBaseValidation validationSnapshot;
    {
        std::lock_guard<std::mutex> lock(g_guardMutex);
        validationSnapshot = g_validation;
    }
    const tfp::Mode fixMode = static_cast<tfp::Mode>(g_fixMode.load(std::memory_order_relaxed));
    Log::get().note(
        "%s mode=%s calls=%llu unrefilled=%llu agree/disagree=%llu/%llu validated=%s "
        "events(watched/acted)=%llu/%llu writer_hits=%llu writers_found=%u consumer_reset_hits=%llu "
        "ship_pointer_changes=%llu writer_watch_armed=%s",
        prefix, modeName(fixMode),
        (unsigned long long)g_totalCalls.load(std::memory_order_relaxed),
        (unsigned long long)g_totalUnrefilled.load(std::memory_order_relaxed),
        (unsigned long long)validationSnapshot.agree, (unsigned long long)validationSnapshot.disagree,
        tfeb::isEyeBaseValidated(validationSnapshot) ? "yes" : "no",
        (unsigned long long)g_eventsWatched.load(std::memory_order_relaxed),
        (unsigned long long)g_eventsActed.load(std::memory_order_relaxed),
        (unsigned long long)g_writerHitsTotal.load(std::memory_order_relaxed),
        writerTableCount(),
        (unsigned long long)g_consumerResetHitsTotal.load(std::memory_order_relaxed),
        (unsigned long long)g_shipPointerChangesTotal.load(std::memory_order_relaxed),
        g_wwHwArmed ? "yes" : "no");
}

uint64_t g_lastReportTickMs = 0;
uint64_t g_lastReportBits[8] = {};

uint64_t reportBit(uint32_t i) noexcept {
    switch (i) {
    case 0: return g_totalCalls.load(std::memory_order_relaxed);
    case 1: return g_totalUnrefilled.load(std::memory_order_relaxed);
    case 2: return g_eventsWatched.load(std::memory_order_relaxed);
    case 3: return g_eventsActed.load(std::memory_order_relaxed);
    case 4: return g_writerHitsTotal.load(std::memory_order_relaxed);
    case 5: return g_consumerResetHitsTotal.load(std::memory_order_relaxed);
    case 6: return g_shipPointerChangesTotal.load(std::memory_order_relaxed);
    default: return g_wwHwArmed ? 1 : 0;
    }
}

void maybeReportPeriodic() noexcept {
    const uint64_t now = GetTickCount64();
    if (now - g_lastReportTickMs < 20000) return;
    g_lastReportTickMs = now;
    bool moved = false;
    for (uint32_t i = 0; i < 8; ++i) {
        const uint64_t v = reportBit(i);
        if (v != g_lastReportBits[i]) { moved = true; g_lastReportBits[i] = v; }
    }
    if (!moved) return;
    reportLine("transition flash eye base:");
}

}  // namespace

// --- Public API ------------------------------------------------------------

void transitionFlashEyeBaseConfigure(Config& cfg) {
    bool recognized = true;
    const std::string text = cfg.getString("advanced.transition_flash_eye_base", "off");
    const tfp::Mode mode = tfp::parseMode(text.c_str(), &recognized);
    if (!recognized) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed)) {
            Log::get().note(
                "transition flash eye base: advanced.transition_flash_eye_base = \"%s\" is not "
                "off, watch, on or alternate -- treated as off.", text.c_str());
        }
    }

    if (mode == tfp::Mode::Off) {
        g_fixMode.store(uint8_t(tfp::Mode::Off), std::memory_order_relaxed);
        if (!g_offLogged.exchange(true, std::memory_order_relaxed)) {
            Log::get().note("transition flash eye base: off (advanced.transition_flash_eye_base = off).");
        }
        return;
    }

    g_fixMode.store(uint8_t(mode), std::memory_order_relaxed);
    if (!g_armed.load(std::memory_order_acquire)) doInstall(mode);
}

void transitionFlashEyeBaseFrameBoundary(uint32_t frameNo) {
    g_frame.store(frameNo, std::memory_order_relaxed);
    if (!g_armed.load(std::memory_order_relaxed)) return;
    frameBoundaryWriterWatch(frameNo);
    serviceDump(frameNo);
    maybeReportPeriodic();
}

void transitionFlashEyeBaseNoteDetectorVerdict(uint32_t frame, bool sceneResetVerdict) {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    // CHANGE 3: the scene-reset-verdict trigger, expressed with the same
    // predicate as the unrefilled-call trigger in consumerObserved -- see
    // tfeb::eyeBaseDumpTrigger's own comment for why it takes no eye-trace
    // flag.
    if (!tfeb::eyeBaseDumpTrigger(/*unrefilledCall=*/false, sceneResetVerdict)) return;
    requestDump("the transition-flash detector's scene-judged eye-reset verdict", frame);
}

// CHANGE 3/4: the per-frame record -- glitch_frame.cpp's own scene-camera
// tap (computed whether or not advanced.eye_origin_trace is on; see this
// function's declaration), folded together with this module's own per-frame
// writer-hit count and mode breakdown, read-and-reset the same way
// transitionFlashEyeBaseFrameSnapshot reads its per-call counters. Pushed to
// its own ring for the next dump, whether or not one is pending -- cheap
// (one memcpy-sized struct) and it is what lets a dump requested a few
// frames from now still show frames already past.
//
// CHANGE 9: geometry/geometryFresh/decision are the detector's own per-frame
// pool-vs-camera measurement and verdict (glitch_scene.h), folded in beside
// the scene camera above.
void transitionFlashEyeBaseNoteSceneCamera(uint32_t frame, const float pos[3], bool valid,
                                            const GlitchSceneGeometry& geometry, bool geometryFresh,
                                            GlitchSceneDecision decision) {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    EyeBaseFrameRecord r;
    r.frame = frame;
    r.sceneValid = valid;
    r.scenePos[0] = pos[0]; r.scenePos[1] = pos[1]; r.scenePos[2] = pos[2];
    // CHANGE 9: folded in unmodified -- the caller (glitch_frame.cpp's
    // recordScenePosition) already zeroed geometry itself when the pool was
    // not sampled this frame; geometryFresh is what tells that apart from a
    // real measured zero.
    r.geometry = geometry;
    r.geometryFresh = geometryFresh;
    r.decision = decision;
    r.writerHits = g_writerHitsThisFrame.exchange(0, std::memory_order_relaxed);
    // CHANGE 6/8: the controller's own call count and the writer's DR1/DR0
    // counts, read-and-reset the same way as writerHits above.
    r.controllerCalls = g_controllerCallsThisFrame.exchange(0, std::memory_order_relaxed);
    r.writerEntered = g_writerEnteredThisFrame.exchange(0, std::memory_order_relaxed);
    r.writerWrote = g_writerWroteThisFrame.exchange(0, std::memory_order_relaxed);
    // The offered matrix's translation THIS frame: a non-destructive peek
    // (readOffered never resets the slot -- consumerObserved's own peeks and
    // this one are both just readers), shown only when the slot's own stamp
    // says it was captured on exactly this frame; left at the struct's NAN
    // default otherwise.
    {
        float offeredM[16];
        uint32_t offeredFrame = 0;
        uint64_t offeredSeq = 0;
        if (readOffered(offeredM, offeredFrame, offeredSeq) && offeredFrame == frame) {
            r.offeredTranslation[0] = offeredM[12];
            r.offeredTranslation[1] = offeredM[13];
            r.offeredTranslation[2] = offeredM[14];
        }
    }
    uint16_t total = 0;
    for (uint32_t i = 0; i < 9; ++i) {
        const uint16_t c = g_modeCountsThisFrame[i].exchange(0, std::memory_order_relaxed);
        r.modeCounts[i] = c;
        total = static_cast<uint16_t>(total + c);
    }
    r.calls = total;

    // CHANGE 12: the passive render-time patch sim. A pending sim armed at
    // the skip consume's frame N covers the render frames N+1..N+3 (the bad
    // render is N+2; one frame of consume/render skew slack either side),
    // and fires only on a covered frame whose scene position is valid. The
    // work is a few memcpys, one SEH-guarded read-only probe of game memory
    // (CHANGE 13 below), and floats under two short-held mutexes, on a
    // render-path thread -- no allocation, nothing written. The camera step
    // is the geometry's OWN cameraStep: glitch_frame.cpp fills the pool
    // sample's camera with this same sceneDrawPos, so that field already is
    // |pos - previous frame's pos| and is only meaningful when the geometry
    // is fresh (patchSceneChoice reads it regardless and the freshness flag
    // is what keeps a stale zero from deciding).
    if (static_cast<tfp::Mode>(g_fixMode.load(std::memory_order_relaxed)) != tfp::Mode::Off) {
        std::lock_guard<std::mutex> lock(g_patchSimMutex);
        if (g_patchSim.active) {
            if (tfeb::patchSimWindowCovers(frame, g_patchSim.skipFrame)) {
                if (valid) {
                    const float P[3] = {pos[0], pos[1], pos[2]};
                    float patchHeld[3] = {NAN, NAN, NAN};
                    float patchNew[3] = {NAN, NAN, NAN};
                    float patchLive[3] = {NAN, NAN, NAN};
                    float liveM[16] = {};
                    uint8_t liveState = LiveUnavailable;
                    // CHANGE 13: the tap-time probe of the LIVE mailbox.
                    // Flight N+1 showed the consume-indexed NEW-base
                    // candidate is structurally stale at the bad render (the
                    // refill consume N+1 runs AFTER the bad render's tap),
                    // but that same refill leaves the needed base sitting
                    // live in ship+0x3330 during the tap's wall-clock window
                    // -- so a read here sees it. The stashed ship pointer
                    // (not g_lastShip) pins the object the skip happened on.
                    // RESET (the bit-exact identity, i.e. the tap landed
                    // inside the consumer's reset window) and any failed
                    // gate mark the probe unusable; the read itself is
                    // SEH-guarded and cannot escape the tap.
                    bool liveReadOk = false;
                    if (g_patchSim.ship != 0 &&
                        sehReadBlock64(static_cast<uintptr_t>(g_patchSim.ship) + 0x3330, liveM)) {
                        liveReadOk = true;
                        if (tfeb::isResetMailbox(liveM)) {
                            liveState = LiveReset;
                        } else if (tfeb::mailboxPlausible(liveM)) {
                            liveState = LiveOk;
                            tfeb::patchEyeOrigin(liveM, P, patchLive);
                        }
                    }
                    float newM[16] = {};
                    uint32_t newFrame = 0;
                    bool haveNew = false;
                    {
                        // The only lock nesting anywhere in this module is
                        // this one, g_patchSimMutex -> g_heldBaseMutex (the
                        // two are always taken in exactly this order, and
                        // g_heldBaseMutex is never held while anything else
                        // is acquired -- armPatchSim, the only other
                        // g_patchSimMutex user, is always called with no
                        // locks held), so this cannot deadlock.
                        std::lock_guard<std::mutex> heldLock(g_heldBaseMutex);
                        haveNew = g_haveLastRefilled;
                        if (haveNew) {
                            std::memcpy(newM, g_lastRefilledM, sizeof(newM));
                            newFrame = g_lastRefilledFrame;
                        }
                    }
                    const bool haveHeld = g_patchSim.haveHeld;
                    if (haveHeld) tfeb::patchEyeOrigin(g_patchSim.heldM, P, patchHeld);
                    if (haveNew) tfeb::patchEyeOrigin(newM, P, patchNew);
                    const tfeb::SceneChoice choice =
                        tfeb::patchSceneChoice(geometry.cameraStep, geometry.poolStep, geometryFresh);
                    const int32_t refilledAge =
                        haveNew ? (frame >= newFrame ? static_cast<int32_t>(frame - newFrame) : -1) : -1;
                    // CHANGE 14: consume the buffer-row locator's
                    // accumulation for this frame. The fills of the frame
                    // the boundary just closed carried the PRE-advance
                    // counter (tapFrame == this frame - 1); accept the
                    // no-skew case too and prefer the -1. Invalidating the
                    // slot afterwards keeps the next frame's fills from
                    // appending to an already-logged accumulation.
                    uint32_t cbFills = 0, cbMatched = 0;
                    int32_t cbTapFrame = -1, cbViewRow = -1;
                    float cbCorrO[3] = {NAN, NAN, NAN};
                    float cbCrossO = NAN, cbCrossV = NAN;
                    uint8_t cbLive = 0;
                    if ((g_cbAccum.tapFrame + 1 == frame || g_cbAccum.tapFrame == frame) &&
                        g_cbAccum.fillsSeen) {
                        PatchSimCBAccum& a = g_cbAccum;
                        cbFills = a.fillsSeen;
                        cbTapFrame = static_cast<int32_t>(a.tapFrame);
                        cbLive = a.lastLiveState;
                        for (uint32_t i = 0; i < a.stored; ++i) {
                            const float dx = a.row275[i][0] - P[0];
                            const float dy = a.row275[i][1] - P[1];
                            const float dz = a.row275[i][2] - P[2];
                            if (dx * dx + dy * dy + dz * dz <= kCBMatchedPTol * kCBMatchedPTol) ++cbMatched;
                        }
                        if (a.analyzed) {
                            cbViewRow = a.find.startRow;
                            cbCorrO[0] = a.correctedOrigin[0];
                            cbCorrO[1] = a.correctedOrigin[1];
                            cbCorrO[2] = a.correctedOrigin[2];
                        }
                        if (a.crossDone) {
                            cbCrossO = a.crossOrigin;
                            cbCrossV = a.crossView;
                        }
                        r.patchSimCBFills = cbFills;
                        r.patchSimCBMatched = cbMatched;
                        r.patchSimCBTapFrame = cbTapFrame;
                        r.patchSimCBViewRow = cbViewRow;
                        r.patchSimCBCorrO[0] = cbCorrO[0];
                        r.patchSimCBCorrO[1] = cbCorrO[1];
                        r.patchSimCBCorrO[2] = cbCorrO[2];
                        r.patchSimCBCrossO = cbCrossO;
                        r.patchSimCBCrossV = cbCrossV;
                        r.patchSimCBLiveState = cbLive;
                        r.patchSimCBDetailCount = a.detailCount;
                        for (uint32_t i = 0; i < a.detailCount && i < kCBAccumDetailMax; ++i) {
                            r.patchSimCBDetail[i] = a.detail[i];
                        }
                        a.tapFrame = 0xFFFFFFFFu;   // consumed
                    }
                    char cbCrossOText[24], cbCrossVText[24];
                    if (std::isnan(cbCrossO)) std::snprintf(cbCrossOText, sizeof(cbCrossOText), "n/a");
                    else std::snprintf(cbCrossOText, sizeof(cbCrossOText), "%.3f", cbCrossO);
                    if (std::isnan(cbCrossV)) std::snprintf(cbCrossVText, sizeof(cbCrossVText), "n/a");
                    else std::snprintf(cbCrossVText, sizeof(cbCrossVText), "%.3f", cbCrossV);
                    g_patchSimFired.fetch_add(1, std::memory_order_relaxed);
                    // One line per covered render frame (at most three per
                    // event), never per consume -- per-consume spam filled
                    // the log once before (CHANGE 9's own history). Both
                    // record candidates and the live probe print regardless
                    // of the choice; the arrow names which the acting build
                    // would write (Unclear defaults to held, the
                    // proven-safe side). live= answers the flight question:
                    // ok (a usable live base sat in the mailbox at the tap),
                    // RESET (the tap landed inside the consumer's reset
                    // window), or n/a. The cb=[...] cluster is the buffer-
                    // row locator's outcome for this frame: fills seen
                    // pre-Unmap, how many carried this P at row 275, the
                    // located current view's float4 row, the correction the
                    // acting build would write, and the cross-frame residual
                    // against the previous covered frame's correction (the
                    // engine's own correct frame).
                    Log::get().note(
                        "transition flash eye base: patch sim frame %u (skip %u): P=(%+.3f %+.3f %+.3f) "
                        "held->(%s%+.3f %+.3f %+.3f) new->(%s%+.3f %+.3f %+.3f) "
                        "live->(%s%+.3f %+.3f %+.3f) cam=%.3f pool=%.3f "
                        "choice=%s->%s (refilled age=%d, live=%s) cb=[fills=%u match275=%u tapf=%d "
                        "view=%d corrO=(%+.3f %+.3f %+.3f) crossO=%s crossV=%s live=%s]",
                        frame, g_patchSim.skipFrame, P[0], P[1], P[2],
                        haveHeld ? "" : "n/a ", patchHeld[0], patchHeld[1], patchHeld[2],
                        haveNew ? "" : "n/a ", patchNew[0], patchNew[1], patchNew[2],
                        liveState == LiveOk ? "" : "n/a ", patchLive[0], patchLive[1], patchLive[2],
                        geometry.cameraStep, geometry.poolStep,
                        tfeb::sceneChoiceText(choice),
                        choice == tfeb::SceneChoice::New ? "new" : "held",
                        refilledAge, patchSimLiveStateText(liveState),
                        cbFills, cbMatched, cbTapFrame, cbViewRow,
                        cbCorrO[0], cbCorrO[1], cbCorrO[2],
                        cbCrossOText, cbCrossVText, patchSimLiveStateText(cbLive));
                    r.patchSim = true;
                    r.patchSimSkipFrame = g_patchSim.skipFrame;
                    r.patchSimP[0] = P[0]; r.patchSimP[1] = P[1]; r.patchSimP[2] = P[2];
                    r.patchSimHeld[0] = patchHeld[0]; r.patchSimHeld[1] = patchHeld[1]; r.patchSimHeld[2] = patchHeld[2];
                    r.patchSimNew[0] = patchNew[0]; r.patchSimNew[1] = patchNew[1]; r.patchSimNew[2] = patchNew[2];
                    r.patchSimLive[0] = patchLive[0]; r.patchSimLive[1] = patchLive[1]; r.patchSimLive[2] = patchLive[2];
                    r.patchSimLiveState = liveState;
                    if (liveReadOk) {
                        // The probe read SOMETHING: keep the raw translation
                        // too, so the dump shows what the mailbox held at
                        // the tap even when it was the RESET zeros or a
                        // value the plausibility gate refused.
                        r.patchSimLiveT[0] = liveM[12]; r.patchSimLiveT[1] = liveM[13]; r.patchSimLiveT[2] = liveM[14];
                    }
                    r.patchSimCamStep = geometry.cameraStep;
                    r.patchSimPoolStep = geometry.poolStep;
                    r.patchSimChoice = static_cast<uint8_t>(choice);
                    r.patchSimRefilledAge = refilledAge;
                    r.patchSimHaveHeld = haveHeld;
                    r.patchSimHaveNew = haveNew;
                    if (frame - g_patchSim.skipFrame >= tfeb::kPatchSimWindowFrames) {
                        // Fired on the window's last frame: retire the sim
                        // here so the next frame's expiry branch does not
                        // count a completed run as expired.
                        g_patchSim.active = false;
                        g_patchSimPending.store(false, std::memory_order_release);
                    }
                }
            } else if (frame - g_patchSim.skipFrame > tfeb::kPatchSimWindowFrames) {
                // The window passed with no covered scene frame at all (a
                // covered frame with an invalid scene= neither logs nor
                // clears -- the next covered frame may still be valid).
                g_patchSim.active = false;
                g_patchSimPending.store(false, std::memory_order_release);
                g_patchSimExpired.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    pushFrameRing(r);
}

// CHANGE 14: the buffer-row locator's fill tap -- glitchFrameObserve's
// pre-Unmap read of the 5376-byte scene CB (one call per fill, ~60 a frame
// on the nominated camera buffer). Everything gates on the module's own
// pending-sim state so the idle cost is one atomic load per fill; the
// analysis itself runs only on the covered frames and writes nothing.
void transitionFlashEyeBaseNoteSceneCB(uint32_t frame, const void* mapped, size_t sizeBytes) {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    if (static_cast<tfp::Mode>(g_fixMode.load(std::memory_order_relaxed)) == tfp::Mode::Off) return;
    if (sizeBytes != tfeb::kSceneCBSimBytes) return;
    if (!g_patchSimPending.load(std::memory_order_acquire)) return;
    const uint32_t skip = g_patchSimPendingSkip.load(std::memory_order_acquire);
    // The fills of boundary frame F carried s->frameNo == F-1 (glitchFrame-
    // Boundary advances the counter before it records the frame it closes;
    // the sim's own frame argument is that advanced number), so the tap
    // frame's +1 must land in the window: tap frames are N..N+2 for the
    // covered boundary frames N+1..N+3.
    if (!tfeb::patchSimWindowCovers(frame + 1, skip)) return;

    const float* cb = static_cast<const float*>(mapped);
    std::lock_guard<std::mutex> lock(g_patchSimMutex);
    if (!g_patchSim.active || !tfeb::patchSimWindowCovers(frame + 1, g_patchSim.skipFrame)) return;

    PatchSimCBAccum& a = g_cbAccum;
    if (a.tapFrame != frame) {
        // First fill of a new covered tap-frame: the previous slot was
        // consumed by its boundary (or held an unconsumed non-covered
        // frame -- either way it is disposable; the cross-frame chain lives
        // in g_patchSim, not here).
        a = PatchSimCBAccum{};
        a.tapFrame = frame;
    }
    ++a.fillsSeen;
    const float* originRow = cb + tfeb::kSceneCBSimOriginFloat;
    const float origin[3] = {originRow[0], originRow[1], originRow[2]};
    if (a.stored < kCBAccumRow275Max) {
        a.row275[a.stored][0] = origin[0];
        a.row275[a.stored][1] = origin[1];
        a.row275[a.stored][2] = origin[2];
        ++a.stored;
    }
    PatchSimCBFillDetail d;
    d.row275[0] = origin[0]; d.row275[1] = origin[1]; d.row275[2] = origin[2];

    // CHANGE 13's path, per fill: the locator is only meaningful while the
    // live base is usable -- on this fill, at this moment.
    float liveM[16] = {};
    uint8_t liveState = LiveUnavailable;
    if (g_patchSim.ship != 0 &&
        sehReadBlock64(static_cast<uintptr_t>(g_patchSim.ship) + 0x3330, liveM)) {
        if (tfeb::isResetMailbox(liveM)) {
            liveState = LiveReset;
        } else if (tfeb::mailboxPlausible(liveM)) {
            liveState = LiveOk;
        }
    }
    a.lastLiveState = liveState;
    d.liveState = liveState;

    if (liveState == LiveOk) {
        // Locate THIS frame's current view structurally: orthonormal 3x3
        // plus a translation locked to this fill's row-275 origin (the
        // previous frame's view is orthonormal too, but its translation
        // belongs to the previous frame's eye -- rejected by the origin
        // test).
        a.find = tfeb::locateSceneCBView(cb, tfeb::kSceneCBSimFloat4Rows, origin,
                                         kCBViewOrthoTol, kCBViewOriginTol);
        a.analyzed = true;
        d.viewRow = a.find.startRow;
        d.originMatch = a.find.startRow >= 0 ? a.find.originMatch : NAN;
        // The correction the acting build would write: the premultiplied
        // origin, and the stored view POSTmultiplied by liveM^-1 (corrected
        // eye = liveM x P, so view' = P^-1 x liveM^-1 = V_bad x liveM^-1).
        tfeb::patchEyeOrigin(liveM, origin, a.correctedOrigin);
        if (a.find.startRow >= 0) {
            float vBad[16], liveInv[16], vCorr[16];
            std::memcpy(vBad, cb + a.find.startRow * 4, sizeof(vBad));
            tfeb::affineInverse4x4(liveM, liveInv);
            tfeb::postmul4x4(vBad, liveInv, vCorr);
            a.correctedViewT[0] = vCorr[12];
            a.correctedViewT[1] = vCorr[13];
            a.correctedViewT[2] = vCorr[14];
        } else {
            a.correctedViewT[0] = NAN; a.correctedViewT[1] = NAN; a.correctedViewT[2] = NAN;
        }
        a.haveCorrection = true;
        // Cross-frame validation: the PREVIOUS covered head-only fill's
        // correction against THIS fill's actual rows. On the bad frame
        // itself the chain is empty (nothing to compare against); the
        // residual appears on the next covered frame -- the engine's own
        // correct frame.
        if (g_patchSim.havePrevCorr) {
            const float dox = g_patchSim.prevCorrOrigin[0] - origin[0];
            const float doy = g_patchSim.prevCorrOrigin[1] - origin[1];
            const float doz = g_patchSim.prevCorrOrigin[2] - origin[2];
            a.crossOrigin = std::sqrt(dox * dox + doy * doy + doz * doz);
            if (a.find.startRow >= 0) {
                const float* actualT = cb + a.find.startRow * 4 + 12;
                const float dvx = g_patchSim.prevCorrViewT[0] - actualT[0];
                const float dvy = g_patchSim.prevCorrViewT[1] - actualT[1];
                const float dvz = g_patchSim.prevCorrViewT[2] - actualT[2];
                a.crossView = std::sqrt(dvx * dvx + dvy * dvy + dvz * dvz);
            } else {
                a.crossView = NAN;
            }
            a.crossDone = true;
        }
        // Chain forward -- but only from a head-only row 275 (|origin| < 1
        // m, the same head volume glitch_scene.h's CameraReset test uses):
        // the premultiply expects P in head space, and on the control fills
        // (N+1/N+3's ordinary eye) the "correction" is not meaningful.
        const float o2 = origin[0] * origin[0] + origin[1] * origin[1] + origin[2] * origin[2];
        if (o2 < kCBHeadOnlyRadius2) {
            g_patchSim.prevCorrOrigin[0] = a.correctedOrigin[0];
            g_patchSim.prevCorrOrigin[1] = a.correctedOrigin[1];
            g_patchSim.prevCorrOrigin[2] = a.correctedOrigin[2];
            g_patchSim.prevCorrViewT[0] = a.correctedViewT[0];
            g_patchSim.prevCorrViewT[1] = a.correctedViewT[1];
            g_patchSim.prevCorrViewT[2] = a.correctedViewT[2];
            g_patchSim.havePrevCorr = true;
        }
    } else {
        a.analyzed = false;
    }
    if (a.detailCount < kCBAccumDetailMax) a.detail[a.detailCount++] = d;
}

EyeBaseFrameSnapshot transitionFlashEyeBaseFrameSnapshot() {
    EyeBaseFrameSnapshot snap;
    snap.calls = g_callsThisFrame.exchange(0, std::memory_order_relaxed);
    snap.unrefilledCalls = g_unrefilledThisFrame.exchange(0, std::memory_order_relaxed);
    snap.treatment = g_treatmentThisFrame.exchange(0, std::memory_order_relaxed);
    snap.writerMask = takeFrameWriterMask();
    snap.writerSinceLastConsume = g_writerSinceLastConsumeThisFrame.exchange(false, std::memory_order_relaxed);
    snap.shipChanged = g_shipChangedThisFrame.exchange(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_lastCallMutex);
        if (g_haveLastCall) {
            snap.mTranslation[0] = g_lastCallM[0]; snap.mTranslation[1] = g_lastCallM[1]; snap.mTranslation[2] = g_lastCallM[2];
            snap.fTranslation[0] = g_lastCallF[0]; snap.fTranslation[1] = g_lastCallF[1]; snap.fTranslation[2] = g_lastCallF[2];
        }
        g_haveLastCall = false;
    }
    return snap;
}

void transitionFlashEyeBaseShutdown() {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    reportLine("--- transition flash eye base, session total:");
}

}  // namespace edvr
