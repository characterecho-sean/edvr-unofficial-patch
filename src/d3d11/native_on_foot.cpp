#include "native_on_foot.h"

#include <windows.h>
#include <d3d11.h>   // F18: the body cull's rasterizer-state clones
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/native_frame.h"   // F22: nativeFrameSetLookYaw, the look-to-aim compensation total the runtime asks for each frame
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "binding_shadow.h"   // bindingShaderHash: the HUD issuer probe's pixel shader
#include "explorer_cam.h"   // localAvatarParts, avatarFadeModeRead, avatarCensusTake: the avatar census (F1, F3)
#include "journal_watch.h"
#include "load_sampler.h"   // F24: the load sampler's tick (read only, temporary)
#include "native_on_foot_core.h"

namespace edvr {

namespace {

using SetterFn = void(__fastcall*)(void*, uint32_t);

// Published before the hook is armed; neither changes after that.
// The CodeHook lives on the heap and is never freed: a static's destructor would uninstall the patch at unload, while the game may be
// inside the setter (the same choice skin_entity_hook makes). The patch lasts the process.
CodeHook* g_hook = nullptr;
SetterFn g_original = nullptr;                     // the trampoline: the setter as it was
std::atomic<uintptr_t> g_frozenSelector{0};        // the first selector whose owner points back at it
std::atomic<bool> g_panelsKeyOn{false};            // fix.native_on_foot_panels, as the frame thread last read it (F9)
std::atomic<bool> g_meshKeyOn{false};              // fix.native_on_foot_hud_mesh, as the frame thread last read it (F9)
std::atomic<float> g_hudSpread{0.0f};                           // fix.native_on_foot_hud_spread, live (F14)
float g_hudSpreadLast = -1.0f;                                  // frame thread only: as last logged (F14)
bool g_reticleKeyLast = false;                                  // frame thread only (F14)
std::atomic<bool> g_fpKeyOn{false};                             // fix.native_on_foot_hide_fp_body, as the frame thread last read it (F15, temporary)
bool g_fpKeyLast = false;                                       // frame thread only: as last logged (F15)
uint8_t g_bodyCullKeyLast = 0;                                  // frame thread only: fix.native_on_foot_body_cull as last logged (F18, temporary)
std::atomic<bool> g_unclipKeyOn{false};                         // fix.native_on_foot_body_unclip, as the frame thread last read it (F19, temporary)
bool g_unclipKeyLast = false;                                   // frame thread only: as last logged (F19)


std::atomic<float> g_hudSize{nofoot::kHudSizeDefault};          // fix.native_on_foot_hud_size, live (F13)
std::atomic<bool> g_followHead{true};                           // fix.native_on_foot_hud_follow is head (F13)
std::atomic<int> g_followSmoothMs{nofoot::kHudSmoothDefault};   // fix.native_on_foot_hud_follow_smoothing_ms, live (F13)
std::atomic<bool> g_visorUpSetting{true};                       // fix.native_on_foot_breathable_visor is up, as the frame thread last read it (F16)
float g_hudSizeLast = -1.0f;                                    // frame thread only: as last logged (F13)
bool g_followHeadLast = true, g_visorUpLast = true;   // frame thread only (F13, F16)
bool g_visorBreathLast = false;                                 // frame thread only: the breathable air state as last logged, on foot (F16)
std::atomic<bool> g_reticleHead{false};                         // fix.native_on_foot_reticle_follow is head (F16)
std::atomic<bool> g_reticleWriteOn{false};                      // the key 10 centring is wanted now: head, on foot, session (frame thread; the probe reads it, F16)
bool g_reticleHeadLast = false, g_reticleWriteLast = false;     // frame thread only (F16)
std::atomic<bool> g_reticleGame{false};                          // fix.native_on_foot_reticle_follow is game (F21): EDVR writes nothing to key 10
bool g_reticleGameLast = false;                                  // frame thread only: as last logged (F21)
std::atomic<bool> g_hybridKeyOn{false};                          // fix.native_on_foot_body_hybrid, as the frame thread last read it (F21, temporary)
bool g_hybridKeyLast = false;                                    // frame thread only: as last logged (F21)
std::atomic<bool> g_reticleLogWanted{false};                    // set by the frame thread on a switch to head; the probe consumes it once and queues the line (F17)
std::atomic<float> g_hudElemScale{0.0f};                        // fix.native_on_foot_hud_element_scale, live: 0 = the game's own (F17)
// ---- F22: look to aim (fix.native_on_foot_look_to_aim) and the read-only probes; the block is below the F9 probes ----
std::atomic<bool> g_lookWantRead{false};       // the P2 hook reads list B now (native stereo on foot, session): the look head and probe B
std::atomic<bool> g_lookKeyOn{false};          // fix.native_on_foot_look_to_aim is on, as the frame thread last read it
std::atomic<bool> g_lookEngaged{false};        // the controller may write its input now (frame thread; the update hook reads it)
std::atomic<float> g_lookInput{0.0f};          // our rotate-abs input for the next update (frame thread writes; zero = none)
std::atomic<uintptr_t> g_lookComp{0};          // the processor's comp, as the processor hook last saw it
std::atomic<uint32_t> g_lookGameFrame{0};      // processor calls so far: the game frame index the body turn is stamped with
std::atomic<int> g_lookSmoothMs{150};          // fix.native_on_foot_look_to_aim_smoothing_ms, live (0..1000)
std::atomic<int> g_lookSign{1};               // fix.native_on_foot_look_to_aim_yaw_sign (temporary, F24 default -1): 1 or -1, on the controller's input only
std::atomic<int> g_lookDelay{0};
bool g_nativeEdgeLast = false;                // F26: frame thread only
// F33 (TEMPORARY, live): the body align's key and its trims (right, up, forward, in cm; clamped to +-30 at read).
std::atomic<bool> g_alignKeyOn{false};
std::atomic<int> g_alignMode{0};   // F35: 0 off, 1 view (F33), 2 body (F35)
double g_alignTrimCm[3] = {};   // [0] right, [1] up, [2] forward (body axes)
std::atomic<int> g_worldMarkersMode{4};        // F27/F31/F37: fix.native_on_foot_world_markers: 0 fixed, 1 forward (C), 2 game, 3 forward_now (H_M), 4 forward_hist (the ring's matched pose, default), 5 forward_k2 (the ring's pose two frames back); live; key 10 is never touched
uint64_t g_worldLineMs = 0;                    // frame thread and game thread: the 5 s line (the post hook writes it, once per 5 s)
               // fix.native_on_foot_look_to_aim_comp_delay (temporary): 0 or 1 frames
bool g_lookOnLast = true;                      // frame thread only: the look keys as last logged (readKey)
int g_lookSignLast = 1, g_lookDelayLast = 0, g_lookCompSignLast = 1;   // frame thread only
// ---- F23 (after the F22 flight): the yaw convention, the rate cap, the runaway guard, the sign instrument, the runtime's sign ----
std::atomic<int> g_lookCompSign{1};            // fix.native_on_foot_look_to_aim_comp_sign (temporary): the sign the runtime's compensation takes
constexpr size_t kLookTurnRing = 1024;         // the processor's |our part| samples for the 3 s window (game thread writes, frame thread reads)
struct LookTurnSample { uint64_t ms; float absPart; };
LookTurnSample g_lookTurns[kLookTurnRing] = {};
uint64_t g_lookTurnsN = 0;                     // under g_f9Lock
std::atomic<uint32_t> g_lookSignMismatch{0};   // frames where sign(game yaw delta) != sign(old + ours) (game thread)
std::atomic<uint32_t> g_lookResetSerial{0};    // F25: moves each time the controller disengages or the guard trips; the runtime re-bases to zero
bool g_lookTripped = false;                    // frame thread: the runaway guard stopped the look; cleared by a key toggle or native on foot re-entering
bool g_lookKeyLast = false, g_lookFootLast = false, g_lookConvLogged = false, g_lookHavePrev = false;   // frame thread only
double g_lookPrevTotal = 0.0, g_lookPrevErr = 0.0;   // frame thread only: the total and the forward yaw at the last engaged tick
double g_lookCapErr = 0.0;                     // frame thread only: the error when the rate cap started
uint64_t g_lookCapStartMs = 0;                 // frame thread only: 0 = not at the cap
double g_lookResAbs = 0.0, g_lookResRatio = 0.0;   // frame thread only: the sign instrument's window sums (rad; ratio of residual to our part)
uint32_t g_lookResN = 0, g_lookRatioN = 0;     // frame thread only
uint32_t g_lookMismatchWinStart = 0;           // frame thread only: the mismatch count at the last 5 s line
float g_hudElemLast = -1.0f;                                    // frame thread only: as last logged (F17)
std::atomic<float> g_hudRoomSetting{1.0f};                      // fix.native_on_foot_hud_room, live, 1.00..2.50 (F18)
std::atomic<float> g_hudRoomEff{1.0f};                          // the room the mesh is built with now: 1 unless the room is applied (F18; game thread reads)
std::atomic<bool> g_elemOverrideOn{false};                      // the element-scale override is held now (frame thread publishes; F18)
std::atomic<float> g_sGame{0.0f};                               // the game's own layout scale, [HRC+0x4C4], last read while the override was off; 0 = not known (F18)
float g_roomLastR = -1.0f, g_roomLastK = -1.0f, g_roomLastS = -1.0f;   // frame thread only: as last logged (F18)
float g_hudOffsetLast[nofoot::kHudGroupCount * 2] = {};         // frame thread only: the group offsets as last read (F16)
float g_hudOffsetRead[nofoot::kHudGroupCount * 2] = {};         // written by the frame thread under g_f9Lock; the layout hook reads it (F16)
bool g_hudOffsetOn = false;                                     // under g_f9Lock: the offsets apply now, native stereo on foot (F16)
uintptr_t g_layoutHrc = 0;                                      // under g_f9Lock: the HRC the layout hook last ran for (F16)
std::atomic<float> g_hudDistance{nofoot::kHudDefaultDistance};   // fix.native_on_foot_hud_distance, live (F12)
float g_hudDistanceLast = -1.0f;                   // frame thread only: as last logged (F12)
std::atomic<bool> g_meshCreateLive{false};         // the creation patch is applied (set by the frame thread; read by the P2 hook, F12)

bool g_panelsKeyLast = false, g_meshKeyLast = false;   // frame thread only: as last logged
std::atomic<bool> g_hudKeyOn{false};               // fix.native_on_foot_hud, as the frame thread last read it (F8)
bool g_hudKeyLast = false;                         // frame thread only: the hud key as last logged
std::atomic<bool> g_keyOn{false};                  // fix.native_on_foot, as the frame thread last read it
std::atomic<bool> g_methodGate{true};              // fix.native_on_foot_method is gate (F2); the setter substitutes only when it is swap
std::atomic<bool> g_lastForwardedHmd{false};       // the most recent setter call forwarded an HMD mode (3 or 4): the gate method's flag reads it
uintptr_t g_base = 0;
// The spinlock the F9-F16 probes and the frame thread share (moved up from the F9 section in F16: readKey uses it first).
std::atomic_flag g_f9Lock = ATOMIC_FLAG_INIT;
struct F9Lock {
    F9Lock() noexcept {
        while (g_f9Lock.test_and_set(std::memory_order_acquire)) _mm_pause();
    }
    ~F9Lock() { g_f9Lock.clear(std::memory_order_release); }
};
std::atomic<uint64_t> g_setterCalls{0};            // every setter call (F7: the request census reads after one)                             // the game image base; set by installOnce before the hook can run

// Frame thread only.
enum class InstallState : uint8_t { NotTried, Armed, Refused };
InstallState g_install = InstallState::NotTried;
bool g_cfgRead = false;
uint32_t g_cfgGeneration = 0;
bool g_keyLast = false;
uint64_t g_lastGateLineMs = 0;
bool g_methodLast = true;          // the method as last read (and logged)
bool g_methodWasGate = true;       // the method on the previous frame boundary (a change clears the gate method's flag)
// The gate byte (F2, method gate): EDVR holds it at 0 while the key is on and a game session is live.
bool g_gateWritten = false;        // EDVR holds the byte at 0 and owes the game a 1 back
std::string g_gateRefusalText;     // the reason the last refusal gave; a refusal is logged once per reason
bool g_gateRestoreFailedNoted = false;

// One record per setter call, written by the game's thread and read at the frame boundary (the hook never logs).
struct SetterEvent {
    uint64_t tMs = 0;
    uint32_t incoming = 0, effective = 0, sourceAfter = 0;
    bool sourceReadOk = false, keyOn = false, admitted = false;
    nofoot::Reason reason = nofoot::Reason::NotCinema;
    uint8_t onFoot = 2, seated = 2;   // 0 no, 1 yes, 2 unknown
    uintptr_t selector = 0, vtRva = 0, owner = 0, caller = 0;
    bool vtReadOk = false, ownerReadOk = false, ownerBack = false;
};
constexpr size_t kEventRing = 64;
std::mutex g_eventMutex;
SetterEvent g_events[kEventRing];      // guarded by g_eventMutex
uint64_t g_eventsWritten = 0;          // guarded by g_eventMutex
uint64_t g_eventsRead = 0;             // guarded by g_eventMutex

// The only places game memory is read or compared. A fault returns false and nothing else.
__declspec(noinline) bool guardedRead(uintptr_t address, void* out, size_t bytes) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) bool bytesMatch(uintptr_t address, const uint8_t* expected, size_t n) noexcept {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(address), expected, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The one write the gate method makes: a single byte in the game's .data (writable, checked first, and SEH-guarded like every other access).
__declspec(noinline) bool guardedWriteByte(uintptr_t address, uint8_t value) noexcept {
    __try {
        *reinterpret_cast<volatile uint8_t*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool writableByte(uintptr_t address) noexcept {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) return false;
    if (region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return (region.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// A selector is a renderer's when its owner points back at it. A bad pointer anywhere along the way is simply not one.
// What the setter's selector looks like, read under SEH: its first qword (its vtable) and its owner (+0x58). The selector is embedded in
// the owner at +0x60, so owner+0x60 is compared by address and never read.
struct SelectorProbe {
    bool vtReadOk = false, ownerReadOk = false;
    uintptr_t vt = 0, owner = 0;
};
SelectorProbe probeSelector(uintptr_t selector) noexcept {
    SelectorProbe p;
    if (!selector) return p;
    p.vtReadOk = guardedRead(selector, &p.vt, sizeof(p.vt));
    p.ownerReadOk = guardedRead(selector + nofoot::kOwnerOffset, &p.owner, sizeof(p.owner));
    return p;
}
// Valid iff the selector's vtable is the setter's (image base + 0x52E9608), its owner is non-null, and owner+0x60 is the selector.
bool selectorValid(uintptr_t selector, const SelectorProbe& p) noexcept {
    return nofoot::selectorShapeOk(p.vtReadOk ? p.vt : 0, g_base + nofoot::kSetterVtableRva, p.ownerReadOk ? p.owner : 0, selector);
}

// The same PE check explorer_cam.cpp makes (its constants, in explorer_cam_core.h, are the ones shared here).
bool checkIdentity(uintptr_t base, const char** why) noexcept {
    uint32_t peOff = 0, timestamp = 0, imageSize = 0;
    if (!guardedRead(base + 0x3C, &peOff, 4)) { *why = "a read faulted while checking the PE header"; return false; }
    if (peOff > 0x1000) { *why = "the PE header offset is implausible"; return false; }
    if (!guardedRead(base + peOff + 8, &timestamp, 4) || !guardedRead(base + peOff + 0x50, &imageSize, 4)) {
        *why = "a read faulted while checking the PE header";
        return false;
    }
    if (!nofoot::identityMatches(timestamp, imageSize)) {
        *why = "the PE timestamp or image size is not build 332841's";
        return false;
    }
    return true;
}

// ---- the hook --------------------------------------------------------------------------------------------------------------
// F27: the eye order and the render-mode edge, at the setter's mode switch (the game's render mode, not 'native on foot active', which comes later).
// F31: the eye order is the cockpit rule everywhere (first target of a frame = left). The native flip is removed: the game renders
// the left eye first in native on foot too (the visor is correct with first = left; the F30 layer swap was persistent under first = right).
void logEyeRule(const char* why) {
    Log::get().note("ui quality: eye rule cockpit (first = left) in every mode: %s", why ? why : "");
}

// F22: the native session test. journalGameplay() (LoadGame seen), or Status.json says on foot now: a training scenario started from the main menu
// writes no LoadGame. Logs once, when the fallback is what admits (journal_watch.h journalSessionOrOnFoot).
bool nativeGameSession() {
    bool viaFoot = false;
    const bool session = journalSessionOrOnFoot(&viaFoot);
    if (viaFoot) {
        static std::atomic<bool> noted{false};
        if (!noted.exchange(true, std::memory_order_relaxed))
            Log::get().note("native on foot: game session from Status.json on foot (no LoadGame: a training scenario)");
    }
    return session;
}

void pushEvent(const SetterEvent& ev) {
    std::lock_guard<std::mutex> lock(g_eventMutex);
    g_events[g_eventsWritten % kEventRing] = ev;
    ++g_eventsWritten;
}

// The setter, as the game calls it (rcx = selector, edx = mode). The original runs exactly once, on this thread, with the effective mode.
// Nothing here waits on the frame thread; the journal and the key are atomic reads.
void __fastcall setterHooked(void* selectorPtr, uint32_t incoming) {
    if (!g_original) return;   // unreachable: the hook is armed only after the trampoline is set
    SetterEvent ev;
    ev.tMs = GetTickCount64();
    // The relay reaches this function by a jmp (buildCallbackRelay's `jmp qword ptr [rip+0]`), not a call, so the return address on entry is the game's caller.
    ev.caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uintptr_t selector = reinterpret_cast<uintptr_t>(selectorPtr);
    const SelectorProbe probe = probeSelector(selector);
    const bool valid = selectorValid(selector, probe);
    ev.selector = selector;
    ev.vtReadOk = probe.vtReadOk;
    ev.vtRva = probe.vt - g_base;
    ev.ownerReadOk = probe.ownerReadOk;
    ev.owner = probe.owner;
    ev.ownerBack = probe.ownerReadOk && probe.owner != 0 && probe.owner + nofoot::kOwnerSelectorOffset == selector;
    uintptr_t frozen = g_frozenSelector.load(std::memory_order_acquire);
    if (!frozen && valid) {
        uintptr_t expected = 0;
        if (g_frozenSelector.compare_exchange_strong(expected, selector, std::memory_order_acq_rel)) frozen = selector;
        else frozen = expected;
    }
    nofoot::Inputs in;
    in.keyOn = g_keyOn.load(std::memory_order_relaxed);
    in.incoming = incoming;
    in.selectorValid = valid;
    in.selectorFrozen = valid && frozen == selector;
    in.gameSession = nativeGameSession();
    ev.reason = nofoot::admit(in);
    ev.admitted = false;   // F34: nothing is substituted (the gate is the only method)
    ev.keyOn = in.keyOn;
    ev.incoming = incoming;
    ev.effective = nofoot::effectiveMode(ev.reason, incoming);
    // F30: every render-mode edge bumps the edge counter that the eye check re-arms on, not only the eye rule's flips.
    const uint32_t prevEffective = g_nativeLastEffective.exchange(ev.effective, std::memory_order_relaxed);
    if (prevEffective != ev.effective) g_nativeModeEdges.fetch_add(1, std::memory_order_relaxed);
    if (prevEffective != ev.effective) logEyeRule("the setter's render mode edge");
    ev.onFoot = journalOnFootKnown() ? (journalOnFoot() ? 1 : 0) : 2;
    ev.seated = journalSeatedKnown() ? (journalSeated() ? 1 : 0) : 2;
    // F34: under the gate the frame thread computes the native flag (nativeOnFootActive); the setter never sets it.
    g_original(selectorPtr, ev.effective);
    g_lastForwardedHmd.store(nofoot::isHmdMode(ev.effective), std::memory_order_relaxed);
    uint32_t after = 0;
    ev.sourceReadOk = guardedRead(selector + nofoot::kSourceModeOffset, &after, sizeof(after));
    ev.sourceAfter = after;
    pushEvent(ev);
}

// ---- the frame thread ---------------------------------------------------------------------------------------------------
void refuse(const char* why) {
    g_install = InstallState::Refused;
    Log::get().note("native on foot: NOT installed -- %s. The game was not touched; every mode request passes through as it always did.", why);
}

// The near relay. CodeHook's five-byte jump reaches only two gigabytes, and EDVR's DLL loads more than that from the game, so the
// replacement is a relay placed within reach of the setter (explorer_cam.cpp's and skin_entity_hook.cpp's pattern: ecm::buildCallbackRelay
// in explorer_cam_core.h, and allocateRelay below, copied from skin_entity_hook.cpp). The relay compares g_gate with zero: zero jumps to the
// original (the trampoline), anything else jumps to setterHooked.
uint8_t* g_relay = nullptr;
alignas(8) std::atomic<uintptr_t> g_gate{0};

// A page of executable memory within two gigabytes of `target`, or null. Same search as skin_entity_hook.cpp's allocateRelay.
uint8_t* allocateRelay(uintptr_t target) noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
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
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

// CodeHook's Prepare: runs after the trampoline exists and before the entry is patched. It puts the trampoline into the relay, makes the
// relay executable, and sets g_original, so the original is reachable before any call can reach the hook.
bool prepareRelay(void* trampoline, void*) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(g_relay + ecm::kCallbackRelayTrampolineAt, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(g_relay, 4096, PAGE_EXECUTE_READ, &oldProtect) || !FlushInstructionCache(GetCurrentProcess(), g_relay, ecm::kCallbackRelayBytes)) return false;
    g_original = reinterpret_cast<SetterFn>(address);
    return true;
}

void installOnce() {
    if (!runtimeVrProfile()) {
        refuse("the flat profile (native on foot is a VR fix; the flat build never patches the setter)");
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        refuse("the game module could not be resolved");
        return;
    }
    const char* peWhy = nullptr;
    if (!checkIdentity(base, &peWhy)) {
        char b[400];
        std::snprintf(b, sizeof(b), "identity mismatch (%s), so EliteDangerous64.exe+0x%llX was not touched", peWhy ? peWhy : "unknown",
                      static_cast<unsigned long long>(nofoot::kSetterRva));
        refuse(b);
        return;
    }
    // The vtable slot must hold the setter before anything is patched: the same function the build's vtable calls.
    uintptr_t slot = 0;
    if (!guardedRead(base + nofoot::kSetterVtableRva + nofoot::kSetterSlotOffset, &slot, sizeof(slot))) {
        refuse("slot mismatch: the vtable slot at EliteDangerous64.exe+0x52E9608 could not be read");
        return;
    }
    if (slot != base + nofoot::kSetterRva) {
        char b[360];
        std::snprintf(b, sizeof(b), "slot mismatch: slot 5 of the vtable at EliteDangerous64.exe+0x52E9608 holds 0x%llX, not the setter at +0x%llX",
                      static_cast<unsigned long long>(slot - base), static_cast<unsigned long long>(nofoot::kSetterRva));
        refuse(b);
        return;
    }
    if (!bytesMatch(base + nofoot::kSetterRva, nofoot::kPrologue, nofoot::kPrologueBytes)) {
        char b[256];
        std::snprintf(b, sizeof(b), "prologue mismatch: the %zu bytes at EliteDangerous64.exe+0x%llX are not build 332841's setter prologue",
                      nofoot::kPrologueBytes, static_cast<unsigned long long>(nofoot::kSetterRva));
        refuse(b);
        return;
    }
    // The relay first: within two gigabytes of the setter, so CodeHook's five-byte jump reaches it. The gate stays shut until the patch
    // is live, so a call that arrives in between goes straight to the original.
    const uintptr_t target = base + nofoot::kSetterRva;
    g_base = base;
    g_relay = allocateRelay(target);
    if (!g_relay) {
        refuse("no executable memory could be placed within two gigabytes of the setter, so the relay cannot reach it; nothing was patched");
        return;
    }
    ecm::buildCallbackRelay(g_relay, &g_gate, reinterpret_cast<const void*>(&setterHooked));
    g_hook = new CodeHook();   // deliberately never deleted (see g_hook)
    if (!g_hook->install(reinterpret_cast<void*>(target), g_relay, nullptr, "native-on-foot-setter", &prepareRelay, nullptr)) {
        VirtualFree(g_relay, 0, MEM_RELEASE);
        g_relay = nullptr;
        char b[480];
        const char* why = g_hook->refusal()[0] ? g_hook->refusal() : "CodeHook gave no reason";
        std::snprintf(b, sizeof(b), "CodeHook refused the install: %s", why);
        refuse(b);
        return;
    }
    g_gate.store(1, std::memory_order_seq_cst);
    g_install = InstallState::Armed;
    Log::get().note("native on foot: installed -- the render-mode setter at EliteDangerous64.exe+0x%llX (vtable +0x%llX slot 5, build 332841), "
                    "stolen=%zu bytes, prologue %zu bytes verified, relay at 0x%llX; every setter call is logged from here, whatever "
                    "fix.native_on_foot says.",
                    static_cast<unsigned long long>(nofoot::kSetterRva), static_cast<unsigned long long>(nofoot::kSetterVtableRva),
                    g_hook->stolenBytes(), nofoot::kPrologueBytes, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_relay)));
}

// F34: the keys removed in the cleanup. The shipping file carries them at their last values (silently); a value that differs is named once, here.
void retiredKeysNote(const Config& cfg, bool first) {
    if (!first) return;
    if (cfg.getString("fix.native_on_foot_method", "gate") != "gate") Log::get().note("native on foot: fix.native_on_foot_method is retired (F34): ignored");
    if (cfg.getString("fix.native_on_foot_hud", "off") != "off") Log::get().note("native on foot: fix.native_on_foot_hud is retired (F34): ignored (it follows fix.native_on_foot)");
    if (cfg.getString("fix.native_on_foot_panels", "off") != "off") Log::get().note("native on foot: fix.native_on_foot_panels is retired (F34): ignored (it follows fix.native_on_foot)");
    if (cfg.getString("fix.native_on_foot_hud_mesh", "off") != "off") Log::get().note("native on foot: fix.native_on_foot_hud_mesh is retired (F34): ignored (it follows fix.native_on_foot)");
    if (cfg.getString("fix.native_on_foot_hud_mesh_world", "off") != "off") Log::get().note("native on foot: fix.native_on_foot_hud_mesh_world is retired (F34): ignored");
    if (cfg.getString("fix.native_on_foot_hud_follow_order", "after") != "after") Log::get().note("native on foot: fix.native_on_foot_hud_follow_order is retired (F34): ignored (the order is after)");
    if (cfg.getString("fix.native_on_foot_reticle_probe", "off") != "off") Log::get().note("native on foot: fix.native_on_foot_reticle_probe is retired (F34): ignored");
    if (cfg.getString("fix.native_on_foot_body_cb_probe", "off") != "off") Log::get().note("native on foot: fix.native_on_foot_body_cb_probe is retired (F34): ignored");
    if (cfg.getString("fix.native_on_foot_view_probes", "off") != "off") Log::get().note("native on foot: fix.native_on_foot_view_probes is retired (F34): ignored");
    if (cfg.getString("fix.native_on_foot_visor_shift", "off") != "off") Log::get().note("native on foot: fix.native_on_foot_visor_shift is retired (F34): ignored");
    if (cfg.getString("fix.native_on_foot_sharp_hud", "off") != "off") Log::get().note("native on foot: fix.native_on_foot_sharp_hud is retired (F34): ignored (it is always on)");
}
// The key, re-read when the configuration changes (Config::generation()). Read by its literal name (tools/gen_settings_schema.py).
void readKey() {
    Config& cfg = Config::get();
    const uint32_t generation = cfg.generation();
    if (g_cfgRead && generation == g_cfgGeneration) return;
    const bool first = !g_cfgRead;
    g_cfgRead = true;
    g_cfgGeneration = generation;
    const std::string value = cfg.getString("fix.native_on_foot", "off");
    if (value != "on" && value != "off") {
        Log::get().note("native on foot: fix.native_on_foot = %s is not on or off; leaving it off.", value.c_str());
    }
    const bool on = value == "on";
    g_keyOn.store(on, std::memory_order_relaxed);
    if (first || on != g_keyLast) {
        Log::get().note("native on foot: fix.native_on_foot is %s%s", on ? "on" : "off",
                        on ? " (an on-foot Cinema request is answered as HMD stereo, by the method below, while a game session is live)"
                           : " (every natural mode request passes through unchanged)");
        g_keyLast = on;
    }
    // F34: the method is the gate (the swap path is gone). The HUD override, the panel and HRC patches follow the master key (F34: they are one
    // feature now, each write still verified). Keys removed in F34 that a settings file may still carry are named once, by retiredKeysNote below.
    g_methodGate.store(true, std::memory_order_relaxed);
    const bool hudOn = on;
    g_hudKeyOn.store(hudOn, std::memory_order_relaxed);
    if (first || hudOn != g_hudKeyLast) {
        Log::get().note("native on foot: the first-person HUD override is %s (it follows fix.native_on_foot)", hudOn ? "on" : "off");
        g_hudKeyLast = hudOn;
    }
    const bool panelsOn = on;
    const bool meshOn = on;
    g_panelsKeyOn.store(panelsOn, std::memory_order_relaxed);
    g_meshKeyOn.store(meshOn, std::memory_order_relaxed);
    if (first || panelsOn != g_panelsKeyLast) {
        Log::get().note("native on foot: the panel patch is %s (it follows fix.native_on_foot)", panelsOn ? "on" : "off");
        g_panelsKeyLast = panelsOn;
    }
    if (first || meshOn != g_meshKeyLast) {
        Log::get().note("native on foot: the HRC creation patch is %s (it follows fix.native_on_foot)", meshOn ? "on" : "off");
        g_meshKeyLast = meshOn;
    }
    retiredKeysNote(cfg, first);
    // F12: the HUD distance (a user setting, live), clamped to its range; logged once per change.
    float dist = cfg.getFloat("fix.native_on_foot_hud_distance", nofoot::kHudDistanceSettingDefault);
    if (dist < nofoot::kHudDistanceMin) dist = nofoot::kHudDistanceMin;
    if (dist > nofoot::kHudDistanceMax) dist = nofoot::kHudDistanceMax;
    g_hudDistance.store(dist, std::memory_order_relaxed);
    if (first || dist != g_hudDistanceLast) {
        Log::get().note("native on foot hud distance: %.3f m (scale %.3f)", static_cast<double>(dist),
                        static_cast<double>(dist / nofoot::kHudDefaultDistance));
        g_hudDistanceLast = dist;
    }
    // F13: the HUD size, the follow mode, the smoothing and the order (temporary), and the visor test key (temporary); live.
    float size = cfg.getFloat("fix.native_on_foot_hud_size", nofoot::kHudSizeDefault);
    if (size < nofoot::kHudSizeMin) size = nofoot::kHudSizeMin;
    if (size > nofoot::kHudSizeMax) size = nofoot::kHudSizeMax;
    g_hudSize.store(size, std::memory_order_relaxed);
    if (first || size != g_hudSizeLast) {
        Log::get().note("native on foot hud size: %.2f", static_cast<double>(size));
        g_hudSizeLast = size;
    }
    const std::string follow = cfg.getString("fix.native_on_foot_hud_follow", "head");
    const bool followHead = follow != "body";
    g_followHead.store(followHead, std::memory_order_relaxed);
    if (first || followHead != g_followHeadLast) {
        Log::get().note("native on foot hud follow: %s", followHead ? "head" : "body");
        g_followHeadLast = followHead;
    }
    int smooth = cfg.getInt("fix.native_on_foot_hud_follow_smoothing_ms", nofoot::kHudSmoothDefault);
    if (smooth < nofoot::kHudSmoothMin) smooth = nofoot::kHudSmoothMin;
    if (smooth > nofoot::kHudSmoothMax) smooth = nofoot::kHudSmoothMax;
    g_followSmoothMs.store(smooth, std::memory_order_relaxed);
    // F34: the follow order is "after" (the one behaviour; the A/B key is gone).
    // F16: the visor in breathable air (a user setting): up holds the visor override while Status.json says breathable air; game leaves it alone.
    const bool visorUp = cfg.getString("fix.native_on_foot_breathable_visor", "up") == "up";
    g_visorUpSetting.store(visorUp, std::memory_order_relaxed);
    if (first || visorUp != g_visorUpLast) {
        Log::get().note("native on foot visor in breathable air: %s", visorUp ? "up" : "game");
        g_visorUpLast = visorUp;
    }
    // F16: the reticle follows aim (the game's own position, the default) or head (key 10 placed along the head direction, F17).
    // F21: "game" leaves key 10 to the game entirely (no aim centring, no head placement; a head placement made before the switch is handed back once).
    const std::string reticleMode = cfg.getString("fix.native_on_foot_reticle_follow", "aim");
    const bool reticleHead = reticleMode == "head";
    const bool reticleGame = reticleMode == "game";
    g_reticleHead.store(reticleHead, std::memory_order_relaxed);
    g_reticleGame.store(reticleGame, std::memory_order_relaxed);
    if (first || reticleHead != g_reticleHeadLast || reticleGame != g_reticleGameLast) {
        Log::get().note("native on foot reticle follows: %s", reticleGame ? "game (EDVR writes nothing to the reticle)" : reticleHead ? "head" : "aim");
        g_reticleHeadLast = reticleHead;
        g_reticleGameLast = reticleGame;
    }
    // F25 (TEMPORARY): the world markers (the waypoint and the others, not key 10): fixed takes the head out of their placement, game leaves them as the game has them.
    {
        const std::string wm = cfg.getString("fix.native_on_foot_world_markers", "forward_hist");
        g_worldMarkersMode.store(wm == "forward" ? 1 : wm == "fixed" ? 0 : wm == "game" ? 2 : wm == "forward_now" ? 3 : wm == "forward_k2" ? 5 : 4, std::memory_order_relaxed);
    }
    // F33 (TEMPORARY, live): the body align and its trims (cm, clamped to +-30).
    {
        const std::string alignMode = cfg.getString("fix.native_on_foot_body_align", "off");
        const int mode = alignMode == "body" ? 2 : (alignMode == "view" || alignMode == "on") ? 1 : 0;   // the old "on" is view
        g_alignMode.store(mode, std::memory_order_relaxed);
        g_alignKeyOn.store(mode != 0, std::memory_order_relaxed);
        const std::string trims[3] = {cfg.getString("fix.native_on_foot_body_align_trim_right_cm", "0"),
                                      cfg.getString("fix.native_on_foot_body_align_trim_up_cm", "0"),
                                      cfg.getString("fix.native_on_foot_body_align_trim_forward_cm", "0")};
        for (size_t i = 0; i < 3; ++i) {
            double v = std::atof(trims[i].c_str());
            if (!(v == v)) v = 0.0;
            g_alignTrimCm[i] = std::max(-30.0, std::min(30.0, v));
        }
    }
    // F26 (TEMPORARY, live): the sharp on-foot HUD. Off by default; read on each frame through g_sharpHudKeyOn.
    g_nativeOnFootSharpHudKey.store(runtimeVrProfile(), std::memory_order_relaxed);   // F34: always on in native on foot (the key is gone)
    // F22: look to aim (user settings, live). The body turns to where the head looks while the reticle follows head; the runtime holds the world still.
    // The sign and the delay are TEMPORARY (the flight A/B); the smoothing is the setting. Each key is spelled out in its own call (the generator scans).
    const bool lookOn = cfg.getString("fix.native_on_foot_look_to_aim", "on") == "on";
    g_lookKeyOn.store(lookOn, std::memory_order_relaxed);
    int lookSmooth = cfg.getInt("fix.native_on_foot_look_to_aim_smoothing_ms", 150);
    if (lookSmooth < 0) lookSmooth = 0;
    if (lookSmooth > 1000) lookSmooth = 1000;
    g_lookSmoothMs.store(lookSmooth, std::memory_order_relaxed);
    const int lookSign = cfg.getInt("fix.native_on_foot_look_to_aim_yaw_sign", 1) < 0 ? -1 : 1;
    g_lookSign.store(lookSign, std::memory_order_relaxed);
    int lookDelay = cfg.getInt("fix.native_on_foot_look_to_aim_comp_delay", 0);
    if (lookDelay < 0) lookDelay = 0;
    if (lookDelay > 1) lookDelay = 1;
    g_lookDelay.store(lookDelay, std::memory_order_relaxed);
    // F23 (TEMPORARY): the sign the runtime's compensation takes. Read live; the published total carries it (lookPublishTick).
    const int lookCompSign = cfg.getInt("fix.native_on_foot_look_to_aim_comp_sign", 1) < 0 ? -1 : 1;
    g_lookCompSign.store(lookCompSign, std::memory_order_relaxed);
    if (first || lookOn != g_lookOnLast || lookSign != g_lookSignLast || lookDelay != g_lookDelayLast || lookCompSign != g_lookCompSignLast) {
        Log::get().note("native on foot look to aim: %s, smoothing %d ms, yaw sign %d (temporary), comp delay %d frame(s) (temporary), comp sign %d (temporary)",
                        lookOn ? "on" : "off", lookSmooth, lookSign, lookDelay, lookCompSign);
        g_lookOnLast = lookOn;
        g_lookSignLast = lookSign;
        g_lookDelayLast = lookDelay;
        g_lookCompSignLast = lookCompSign;
    }
    // F16: the per-group HUD offsets (user settings, live): x and y per group, a fraction of the HUD canvas, read under the layout hook's lock.
    {
        // Each key is spelled out in its own call: the settings generator scans the source for typed reads with a literal key.
        auto offsetClamp = [](float v) {
            if (v < nofoot::kHudOffsetMin) return nofoot::kHudOffsetMin;
            if (v > nofoot::kHudOffsetMax) return nofoot::kHudOffsetMax;
            return v;
        };
        float offs[nofoot::kHudGroupCount * 2] = {};
        // F19: the left group's defaults are Sean's tuning (0.18, 0.02) and the top group's y is -0.07; the other offsets stay at 0.
        offs[0] = offsetClamp(cfg.getFloat("fix.native_on_foot_hud_left_x", 0.0f));
        offs[1] = offsetClamp(cfg.getFloat("fix.native_on_foot_hud_left_y", 0.02f));
        offs[2] = offsetClamp(cfg.getFloat("fix.native_on_foot_hud_top_x", 0.0f));
        offs[3] = offsetClamp(cfg.getFloat("fix.native_on_foot_hud_top_y", -0.09f));
        offs[4] = offsetClamp(cfg.getFloat("fix.native_on_foot_hud_right_x", -0.07f));
        offs[5] = offsetClamp(cfg.getFloat("fix.native_on_foot_hud_right_y", 0.0f));
        offs[6] = offsetClamp(cfg.getFloat("fix.native_on_foot_hud_centre_x", 0.0f));
        offs[7] = offsetClamp(cfg.getFloat("fix.native_on_foot_hud_centre_y", 0.0f));
        {
            F9Lock lock;
            std::memcpy(g_hudOffsetRead, offs, sizeof(offs));
        }
        if (first || std::memcmp(offs, g_hudOffsetLast, sizeof(offs)) != 0) {
            Log::get().note("native on foot hud offsets (fraction of the canvas): left %.2f,%.2f top %.2f,%.2f right %.2f,%.2f centre %.2f,%.2f",
                            static_cast<double>(offs[0]), static_cast<double>(offs[1]), static_cast<double>(offs[2]), static_cast<double>(offs[3]),
                            static_cast<double>(offs[4]), static_cast<double>(offs[5]), static_cast<double>(offs[6]), static_cast<double>(offs[7]));
            std::memcpy(g_hudOffsetLast, offs, sizeof(offs));
        }
    }
    // F14: the HUD spread (a user setting, live) and the reticle probe key (temporary).
    float spread = cfg.getFloat("fix.native_on_foot_hud_spread", nofoot::kHudSpreadDefault);
    if (spread < 0.0f) spread = 0.0f;
    if (spread > 0.45f) spread = 0.45f;
    g_hudSpread.store(spread, std::memory_order_relaxed);
    if (first || spread != g_hudSpreadLast) {
        Log::get().note("native on foot hud spread: %.2f (pulls the elements toward the centre)", static_cast<double>(spread));
        g_hudSpreadLast = spread;
    }
    // F17: the HUD element scale (a user setting, live): 0 = the game's own scale; otherwise 0.30..2.00 (a value under 0.30 is 0.30).
    float elem = cfg.getFloat("fix.native_on_foot_hud_element_scale", 1.15f);   // F34: the shipping default (0 = the game's own)
    if (elem < 0.0f) elem = 0.0f;
    if (elem > 0.0f && elem < nofoot::kHudElemScaleMin) elem = nofoot::kHudElemScaleMin;
    if (elem > nofoot::kHudElemScaleMax) elem = nofoot::kHudElemScaleMax;
    g_hudElemScale.store(elem, std::memory_order_relaxed);
    if (first || elem != g_hudElemLast) {
        if (elem > 0.0f) Log::get().note("native on foot hud element scale setting: %.2f", static_cast<double>(elem));
        else Log::get().note("native on foot hud element scale setting: 0 (the game's own scale)");
        g_hudElemLast = elem;
    }
    // F18: the HUD room (a user setting, live): 1.00..2.50. Elements keep their angular size; the element scale is divided by the room and the
    // mesh is multiplied by it (the room is applied only when the game's scale is known; see hudRoomTick).
    float room = cfg.getFloat("fix.native_on_foot_hud_room", 1.25f);
    if (room < 1.0f) room = 1.0f;
    if (room > 2.5f) room = 2.5f;
    g_hudRoomSetting.store(room, std::memory_order_relaxed);
    // F34: the reticle stub is the feature's (key 10 while native stereo is on foot); the probe key is gone.
    // F15 (TEMPORARY): the first-person body hide (explorer_cam.cpp, explorerCamFpBody).
    const bool fpOn = cfg.getString("fix.native_on_foot_hide_fp_body", "off") == "on";
    g_fpKeyOn.store(fpOn, std::memory_order_relaxed);
    if (first || fpOn != g_fpKeyLast) {
        Log::get().note("native on foot: fix.native_on_foot_hide_fp_body is %s (temporary)", fpOn ? "on" : "off");
        g_fpKeyLast = fpOn;
    }
    // F18 (TEMPORARY): the third-person torso's cull mode (nativeOnFootBodyCullBegin, below). 0 game, 1 none, 2 front.
    const std::string bodyCull = cfg.getString("fix.native_on_foot_body_cull", "game");
    uint8_t bodyCullMode = 0;
    if (bodyCull == "none") {
        bodyCullMode = 1;
    } else if (bodyCull == "front") {
        bodyCullMode = 2;
    } else if (bodyCull != "game") {
        Log::get().note("native on foot: fix.native_on_foot_body_cull = %s is not game, none or front; leaving it game.", bodyCull.c_str());
    }
    g_nativeOnFootBodyCull.store(bodyCullMode, std::memory_order_relaxed);
    if (first || bodyCullMode != g_bodyCullKeyLast) {
        static const char* const kBodyCullNames[3] = {"game", "none", "front"};
        Log::get().note("native on foot: fix.native_on_foot_body_cull is %s (temporary)", kBodyCullNames[bodyCullMode]);
        g_bodyCullKeyLast = bodyCullMode;
    }
    // F19 (TEMPORARY): the body unclip, explorer_cam.cpp (explorerCamBodyClipTick, the fade hook's bodyClipStep). Written while native stereo is on foot.
    const bool unclipOn = cfg.getString("fix.native_on_foot_body_unclip", "off") == "on";
    g_unclipKeyOn.store(unclipOn, std::memory_order_relaxed);
    if (first || unclipOn != g_unclipKeyLast) {
        Log::get().note("native on foot: fix.native_on_foot_body_unclip is %s (temporary)", unclipOn ? "on" : "off");
        g_unclipKeyLast = unclipOn;
    }
    // F21 (TEMPORARY): the hybrid first-person body (explorer_cam.cpp, explorerCamHybridTick). While it is on and native stereo is on foot it
    // overrides fix.native_on_foot_hide_fp_body and fix.native_on_foot_body_unclip.
    const bool hybridOn = cfg.getString("fix.native_on_foot_body_hybrid", "off") == "on";
    g_hybridKeyOn.store(hybridOn, std::memory_order_relaxed);
    if (first || hybridOn != g_hybridKeyLast) {
        Log::get().note("native on foot: fix.native_on_foot_body_hybrid is %s (temporary)", hybridOn ? "on" : "off");
        g_hybridKeyLast = hybridOn;
    }
}

// The caller's address, image-relative; "+0x?" when it is not inside the game image.
void callerText(uintptr_t caller, char* out, size_t cap) {
    if (g_base && caller >= g_base && caller < g_base + ecm::kExpectedImageSize) {
        std::snprintf(out, cap, "+0x%llX", static_cast<unsigned long long>(caller - g_base));
    } else {
        std::snprintf(out, cap, "+0x?");
    }
}

// One setter call, in full.
void logSetterLine(const SetterEvent& ev) {
    char source[16];
    if (ev.sourceReadOk) std::snprintf(source, sizeof(source), "%u", ev.sourceAfter);
    else std::snprintf(source, sizeof(source), "unreadable");
    static const char* const kYesNoUnknown[3] = {"no", "yes", "unknown"};
    char vt[32];
    if (ev.vtReadOk) std::snprintf(vt, sizeof(vt), "+0x%llX", static_cast<unsigned long long>(ev.vtRva));
    else std::snprintf(vt, sizeof(vt), "unreadable");
    char owner[32];
    if (ev.ownerReadOk) std::snprintf(owner, sizeof(owner), "0x%llX", static_cast<unsigned long long>(ev.owner));
    else std::snprintf(owner, sizeof(owner), "unreadable");
    char caller[32];
    callerText(ev.caller, caller, sizeof(caller));
    Log::get().note("native on foot: incoming=%u effective=%u source_after=%s key=%s admitted=%s reason=%s onfoot=%s seated=%s t=%llu "
                    "sel=0x%llX vt=%s owner=%s owner_back=%s caller=%s",
                    ev.incoming, ev.effective, source, ev.keyOn ? "on" : "off", ev.admitted ? "yes" : "no",
                    nofoot::reasonName(ev.reason), kYesNoUnknown[ev.onFoot], kYesNoUnknown[ev.seated],
                    static_cast<unsigned long long>(ev.tMs), static_cast<unsigned long long>(ev.selector), vt, owner,
                    ev.ownerBack ? "yes" : "no", caller);
}

// ---- collapsing repeats (frame thread only) -------------------------------------------------------------------------------------
// The game asks for the same mode on nearly every frame. A call identical to the one before it (incoming, effective, reason,
// selector, caller) is counted, not logged. The first call of each kind is logged in full; the repeats are summarised every five
// seconds, and again when a different call arrives.
struct RunState {
    bool open = false;
    SetterEvent first;             // the kind being counted (its first call was logged in full)
    uint64_t repeats = 0;          // repeats not yet summarised
    uint64_t windowStartMs = 0;    // the start of the window the next summary covers
    uint64_t lastMs = 0;           // the newest repeat's time
};
RunState g_run;

bool sameKind(const SetterEvent& a, const SetterEvent& b) {
    return a.incoming == b.incoming && a.effective == b.effective && a.reason == b.reason && a.selector == b.selector && a.caller == b.caller;
}

void emitRepeats() {
    if (!g_run.repeats) return;
    char caller[32];
    callerText(g_run.first.caller, caller, sizeof(caller));
    Log::get().note("native on foot: repeated %llu x (incoming=%u effective=%u reason=%s caller=%s) over %llu ms",
                    static_cast<unsigned long long>(g_run.repeats), g_run.first.incoming, g_run.first.effective,
                    nofoot::reasonName(g_run.first.reason), caller,
                    static_cast<unsigned long long>(g_run.lastMs - g_run.windowStartMs));
    g_run.repeats = 0;
    g_run.windowStartMs = g_run.lastMs;
}

void noteSetterEvent(const SetterEvent& ev) {
    if (g_run.open && sameKind(ev, g_run.first)) {
        ++g_run.repeats;
        g_run.lastMs = ev.tMs;
        if (ev.tMs - g_run.windowStartMs >= 5000) emitRepeats();
        return;
    }
    emitRepeats();   // the previous kind's count, before the new kind
    logSetterLine(ev);
    g_run.open = true;
    g_run.first = ev;
    g_run.repeats = 0;
    g_run.windowStartMs = ev.tMs;
    g_run.lastMs = ev.tMs;
}

void drainEvents() {
    SetterEvent local[kEventRing];
    size_t count = 0;
    uint64_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(g_eventMutex);
        uint64_t from = g_eventsRead;
        if (g_eventsWritten - from > kEventRing) {
            dropped = g_eventsWritten - from - kEventRing;
            from = g_eventsWritten - kEventRing;
        }
        for (; from < g_eventsWritten; ++from) local[count++] = g_events[from % kEventRing];
        g_eventsRead = g_eventsWritten;
    }
    if (dropped) {
        Log::get().note("native on foot: %llu setter call(s) were not logged (more than %zu between two frames)",
                        static_cast<unsigned long long>(dropped), kEventRing);
    }
    for (size_t i = 0; i < count; ++i) noteSetterEvent(local[i]);
    if (g_run.open && g_run.repeats && GetTickCount64() - g_run.windowStartMs >= 5000) emitRepeats();
}

// ---- the gate byte (F2, method gate; frame thread) ---------------------------------------------------------------------------------
// The requester at 0x28873C0 keeps the cinema partner mode only while the byte at EliteDangerous64.exe+0x5E82B3E is non-zero (verified,
// build 332841; the design doc). At 0 it falls back to the base HMD mode, so the game requests HMD itself and nothing is substituted in
// the setter. The byte is written only after the identity check passes and only when it reads 1, and it is put back to 1 when the want
// ends. A refusal is logged once per reason.
constexpr uintptr_t kGateByteRva = 0x5E82B3E;

void refuseGate(const std::string& why) {
    if (why == g_gateRefusalText) return;   // the same reason: already said
    g_gateRefusalText = why;
    Log::get().note("native on foot: gate byte EliteDangerous64.exe+0x%llX NOT written -- %s. The requester is left as the game has it.",
                    static_cast<unsigned long long>(kGateByteRva), why.c_str());
}

// `wantByte`: the byte should read 0 now. `restoreReason` (a literal) says why it is being put back.
void gateByteTick(bool wantByte, const char* restoreReason) {
    const uintptr_t address = g_base + kGateByteRva;
    if (wantByte) {
        if (g_gateWritten || g_install != InstallState::Armed) return;
        const char* idWhy = nullptr;
        if (!checkIdentity(g_base, &idWhy)) {
            refuseGate(std::string("the PE identity check failed (") + idWhy + ")");
            return;
        }
        uint8_t current = 0;
        if (!guardedRead(address, &current, 1)) {
            refuseGate("the byte could not be read");
            return;
        }
        if (current != 1) {
            char b[128];
            std::snprintf(b, sizeof(b), "the byte reads %u, not 1, so it is someone else's and is left alone", static_cast<unsigned>(current));
            refuseGate(b);
            return;
        }
        if (!writableByte(address) || !guardedWriteByte(address, 0)) {
            refuseGate("the byte is not writable");
            return;
        }
        g_gateWritten = true;
        g_gateRefusalText.clear();
        Log::get().note("native on foot: gate byte EliteDangerous64.exe+0x5E82B3E 1 -> 0 (method gate)");
        return;
    }
    if (!g_gateWritten) {
        g_gateRefusalText.clear();
        return;
    }
    if (!writableByte(address) || !guardedWriteByte(address, 1)) {
        if (!g_gateRestoreFailedNoted) {
            g_gateRestoreFailedNoted = true;
            Log::get().note("native on foot: gate byte EliteDangerous64.exe+0x5E82B3E could not be put back to 1 (%s); retrying each frame.", restoreReason);
        }
        return;
    }
    g_gateWritten = false;
    g_gateRestoreFailedNoted = false;
    Log::get().note("native on foot: gate byte EliteDangerous64.exe+0x5E82B3E 0 -> 1 (restored: %s)", restoreReason);
}

// The avatar census (F1, log only): the local third-person avatar's parts and view masks, from Explorer Cam's head-hiding capture, read-only.
uint64_t g_lastAvatarMs = 0;   // frame thread only
void avatarLine(uint64_t nowMs) {
    if (!nativeOnFootActive()) return;
    if (nowMs - g_lastAvatarMs < 5000) return;
    g_lastAvatarMs = nowMs;
    int32_t fade = 0;
    char fadeText[24];
    if (avatarFadeModeRead(&fade)) std::snprintf(fadeText, sizeof(fadeText), "%d", fade);
    else std::snprintf(fadeText, sizeof(fadeText), "unreadable");
    static LocalAvatarPart parts[64];
    uint64_t amc = 0;
    const uint32_t count = localAvatarParts(parts, 64, &amc);
    if (!amc) {
        Log::get().note("native on foot avatar: no local third-person avatar latched yet; fade mode %s (-1 auto, 0 off)", fadeText);
        return;
    }
    Log::get().note("native on foot avatar: local AMC 0x%llX, %u parts, fade mode %s (-1 auto, 0 off)", static_cast<unsigned long long>(amc),
                    count, fadeText);
    for (uint32_t i = 0; i < count; ++i) {
        const LocalAvatarPart& p = parts[i];
        if (!p.ok) {
            Log::get().note("native on foot avatar: part=%s head=%s masks=unreadable", p.name, p.head ? "yes" : "no");
            continue;
        }
        Log::get().note("native on foot avatar: part=%s head=%s masks=0x%llX,0x%llX,0x%llX,0x%llX", p.name, p.head ? "yes" : "no",
                        static_cast<unsigned long long>(p.masks[0]), static_cast<unsigned long long>(p.masks[1]),
                        static_cast<unsigned long long>(p.masks[2]), static_cast<unsigned long long>(p.masks[3]));
    }
}

// The neck census (F1, log only): once a second, the head joint and the AMC origin the game's job thread sampled every 100 ms, and the
// min and max of d = head - amc over that second. The spaces differ (avatar-root model space, then world), so d is not a camera offset;
// the line names each space and says so.
uint64_t g_neckWindowMs = 0;        // frame thread only: the start of the current second
uint64_t g_neckStampSeen = 0;
NeckCensusReading g_neckLast;
bool g_neckHave = false;
uint32_t g_neckSamples = 0;         // valid (head and amc read) samples in this second
float g_neckDMin[3] = {}, g_neckDMax[3] = {};

void neckFormat(const float* v, bool ok, char* out, size_t cap) {
    if (ok) std::snprintf(out, cap, "(%.3f,%.3f,%.3f)", static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]));
    else std::snprintf(out, cap, "n/a");
}

void neckLine(uint64_t nowMs) {
    if (!nativeOnFootActive()) {
        g_neckSamples = 0;
        g_neckWindowMs = 0;
        return;
    }
    if (g_neckWindowMs == 0) g_neckWindowMs = nowMs;
    NeckCensusReading r;
    if (neckCensusRead(&r) && r.stamp != g_neckStampSeen) {
        g_neckStampSeen = r.stamp;
        g_neckLast = r;
        g_neckHave = true;
        if (r.state == 5 && r.amcState == 1) {
            const float d[3] = {r.head[0] - r.amc[0], r.head[1] - r.amc[1], r.head[2] - r.amc[2]};
            for (int i = 0; i < 3; ++i) {
                if (g_neckSamples == 0 || d[i] < g_neckDMin[i]) g_neckDMin[i] = d[i];
                if (g_neckSamples == 0 || d[i] > g_neckDMax[i]) g_neckDMax[i] = d[i];
            }
            ++g_neckSamples;
        }
    }
    if (nowMs - g_neckWindowMs < 1000) return;
    g_neckWindowMs = nowMs;
    if (!g_neckHave) return;
    const bool headOk = g_neckLast.state == 5, camOk = g_neckLast.amcState == 1 && headOk;
    char head[64], cam[64], d[64], dmin[64], dmax[64];
    neckFormat(g_neckLast.head, headOk, head, sizeof(head));
    neckFormat(g_neckLast.amc, camOk, cam, sizeof(cam));
    const float dNow[3] = {g_neckLast.head[0] - g_neckLast.amc[0], g_neckLast.head[1] - g_neckLast.amc[1], g_neckLast.head[2] - g_neckLast.amc[2]};
    neckFormat(dNow, camOk, d, sizeof(d));
    neckFormat(g_neckDMin, g_neckSamples > 0, dmin, sizeof(dmin));
    neckFormat(g_neckDMax, g_neckSamples > 0, dmax, sizeof(dmax));
    Log::get().note("native on foot neck: state=%u head=%s [avatar-root model space] cam=%s [AMC origin, world; the game camera is not read here] "
                    "d=%s [two spaces: not a camera offset] d_min=%s d_max=%s samples=%u",
                    g_neckLast.state, head, cam, d, dmin, dmax, g_neckSamples);
    g_neckSamples = 0;
}

void gateLine(uint64_t nowMs) {
    if (!nativeOnFootActive()) return;
    g_nativeOnFootGates[size_t(NativeOnFootGate::Frames)].fetch_add(1, std::memory_order_relaxed);
    if (nowMs - g_lastGateLineMs < 5000) return;
    g_lastGateLineMs = nowMs;
    uint64_t v[size_t(NativeOnFootGate::Count)];
    for (size_t i = 0; i < size_t(NativeOnFootGate::Count); ++i) v[i] = g_nativeOnFootGates[i].load(std::memory_order_relaxed);
    Log::get().note("native on foot gates: frames=%llu world_route=%llu ui_layer=%llu foot_split=%llu deferred_pacing=%llu panel_shape=%llu "
                    "transition_flash=%llu (cumulative since launch)",
                    static_cast<unsigned long long>(v[0]), static_cast<unsigned long long>(v[1]), static_cast<unsigned long long>(v[2]),
                    static_cast<unsigned long long>(v[3]), static_cast<unsigned long long>(v[4]), static_cast<unsigned long long>(v[5]),
                    static_cast<unsigned long long>(v[6]));
}

// ---- the HUD issuer stack probe (F3, log only) ---------------------------------------------------------------------------------------
// The capture runs on the draw thread (nativeOnFootHudCapture, below) for a matching vertex shader, at most once per shader per 10 s; the
// frame thread resolves the frames and logs them. The ring holds eight records; a full ring drops the newest and counts it.
constexpr size_t kHudRing = 8;
constexpr size_t kHudFrames = 32;
constexpr uint64_t kHudIntervalMs = 10000;
struct HudStackRecord {
    uint64_t vs = 0, ps = 0;
    uint32_t n = 0;
    uintptr_t frames[kHudFrames] = {};
};
std::mutex g_hudMutex;                                   // the ring and its counters; taken only by a capture the rate limit lets through
HudStackRecord g_hudRing[kHudRing];
uint64_t g_hudWritten = 0, g_hudRead = 0, g_hudDropped = 0;   // guarded by g_hudMutex
std::atomic<uint64_t> g_hudLastMs[2]{};                  // per table index; zero = never captured
bool g_hudArmedNoted = false;                            // frame thread only

// One frame as text: "+0xRVA" in EliteDangerous64.exe, "d3d11+0xRVA" in EDVR's proxy, "edvr+0xRVA" in openvr_api.dll (EDVR's), else "?".
void hudFrameText(uintptr_t address, char* out, size_t cap) {
    HMODULE mod = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        reinterpret_cast<LPCWSTR>(address), &mod) || !mod) {
        std::snprintf(out, cap, "?");
        return;
    }
    const uintptr_t rva = address - reinterpret_cast<uintptr_t>(mod);
    if (mod == GetModuleHandleW(nullptr)) {
        std::snprintf(out, cap, "+0x%llX", static_cast<unsigned long long>(rva));
        return;
    }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* leaf = std::wcsrchr(path, L'\\');
    leaf = leaf ? leaf + 1 : path;
    if (_wcsnicmp(leaf, L"d3d11", 5) == 0) std::snprintf(out, cap, "d3d11+0x%llX", static_cast<unsigned long long>(rva));
    else if (_wcsnicmp(leaf, L"openvr_api", 10) == 0) std::snprintf(out, cap, "edvr+0x%llX", static_cast<unsigned long long>(rva));
    else std::snprintf(out, cap, "?");
}

void hudStackDrain() {
    HudStackRecord local[kHudRing];
    size_t n = 0;
    uint64_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(g_hudMutex);
        while (g_hudRead < g_hudWritten && n < kHudRing) local[n++] = g_hudRing[(g_hudRead++) % kHudRing];
        dropped = g_hudDropped;
        g_hudDropped = 0;
    }
    if (dropped) {
        Log::get().note("native on foot hud stack: %llu capture(s) dropped (the ring of %zu was full)", static_cast<unsigned long long>(dropped), kHudRing);
    }
    for (size_t i = 0; i < n; ++i) {
        std::string text;
        for (uint32_t f = 0; f < local[i].n && f < kHudFrames; ++f) {
            char item[40];
            hudFrameText(local[i].frames[f], item, sizeof(item));
            if (!text.empty()) text += ' ';
            text += item;
        }
        Log::get().note("native on foot hud stack: vs=0x%016llX ps=0x%016llX frames: %s", static_cast<unsigned long long>(local[i].vs),
                        static_cast<unsigned long long>(local[i].ps), text.c_str());
    }
}

// ---- the avatar census (F3, log only) -----------------------------------------------------------------------------------------------
// Every 5 s while native stereo is on foot: the AMCs the fade hook collected in the window (explorer_cam.cpp), at most six, one line each
// (a second line for the parts that do not fit one).
constexpr uint32_t kFpRows = 6;
uint64_t g_lastFpMs = 0;   // frame thread only

void logAvatarRow(const AvatarCensusRow& r) {
    char dist[24];
    if (r.distOk) std::snprintf(dist, sizeof(dist), "%.2fm", static_cast<double>(r.dist));
    else std::snprintf(dist, sizeof(dist), "n/a");
    char head[160];
    std::snprintf(head, sizeof(head), "native on foot avatars: amc=0x%llX mode=%d parts=%u dist=%s",
                  static_cast<unsigned long long>(r.amc), r.mode, r.parts, dist);
    if (r.parts == 0) {
        Log::get().note("%s []", head);
        return;
    }
    uint32_t i = 0;
    bool first = true;
    while (i < r.parts) {
        std::string body;
        for (; i < r.parts; ++i) {
            const AvatarCensusPart& p = r.part[i];
            char item[160];
            std::snprintf(item, sizeof(item), "%s 0x%llX,0x%llX,0x%llX,0x%llX", p.name,
                          static_cast<unsigned long long>(p.masks[0]), static_cast<unsigned long long>(p.masks[1]),
                          static_cast<unsigned long long>(p.masks[2]), static_cast<unsigned long long>(p.masks[3]));
            if (!body.empty() && body.size() + 1 + std::strlen(item) > 800) break;   // the next line takes this part
            if (!body.empty()) body += ' ';
            body += item;
        }
        if (first) Log::get().note("%s [%s]", head, body.c_str());
        else Log::get().note("native on foot avatars: amc=0x%llX (cont.) [%s]", static_cast<unsigned long long>(r.amc), body.c_str());
        first = false;
    }
}

// F15 (TEMPORARY): the first-person body hide's zeroes as the hook left them, read straight after the write. The frame thread's own census read can
// land after the game's next-frame rewrite, so this line is the evidence that the zero happened.
void logFpBodyReadback() {
    const FpBodyReadback rb = explorerCamFpBodyReadback();
    if (rb.amc == 0) {
        Log::get().note("native on foot body: first-person masks not zeroed yet (parts found=%u, first-person AMC calls=%llu)", rb.parts,
                        static_cast<unsigned long long>(rb.mode2Seen));
        return;
    }
    std::string text;
    for (uint32_t i = 0; i < 3; ++i) {
        if (!rb.name[i]) continue;
        char item[160];
        std::snprintf(item, sizeof(item), "%s 0x%llX,0x%llX,0x%llX,0x%llX", rb.name[i], static_cast<unsigned long long>(rb.masks[i][0]),
                      static_cast<unsigned long long>(rb.masks[i][1]), static_cast<unsigned long long>(rb.masks[i][2]),
                      static_cast<unsigned long long>(rb.masks[i][3]));
        if (!text.empty()) text += ' ';
        text += item;
    }
    Log::get().note("native on foot body: first-person masks after the zero: amc=0x%llX first-person AMC calls=%llu [%s]",
                    static_cast<unsigned long long>(rb.amc), static_cast<unsigned long long>(rb.mode2Seen), text.c_str());
}

void avatarCensusLine(uint64_t nowMs) {
    if (nowMs - g_lastFpMs < 5000) return;
    g_lastFpMs = nowMs;
    static AvatarCensusRow rows[kFpRows];   // about 18 KB, frame thread only
    uint32_t seen = 0, dropped = 0;
    const uint32_t n = avatarCensusTake(rows, kFpRows, &seen, &dropped);   // clears the window even when the key is off
    if (!nativeOnFootActive()) return;
    if (g_fpKeyOn.load(std::memory_order_relaxed)) logFpBodyReadback();
    if (dropped) Log::get().note("native on foot avatars: %u AMC(s) not collected in the last 5 s (eight slots were full)", dropped);
    if (n == 0) {
        Log::get().note("native on foot avatars: none kept in the last 5 s (seen=%u)", seen);
        return;
    }
    for (uint32_t i = 0; i < n; ++i) logAvatarRow(rows[i]);
}

// ---- F8: the first-person HUD override (TEMPORARY; approved by Sean 2026-10-13) ----------------------------------------------------
// While native stereo is on foot, fix.native_on_foot_hud is on, and the identity check passes, the int32 at EliteDangerous64.exe+0x5E82B48
// is written from -1 (auto) to 1: the first-person HUD's enable input, which the requester at 0x2887A15 reads (the design doc). It is put
// back to -1 when any of those stops holding, at shutdown, or when a check fails. It is written only while it reads -1, and put back only
// while it reads our 1; any other value is someone else's and is left alone. The write is four bytes (guardedWriteDword).
bool g_hudOverrideHeld = false;                 // frame thread only: EDVR holds the override at 1 and owes the game a -1 back
std::string g_hudRefusalText;              // frame thread only
bool g_hudRestoreFailedNoted = false;      // frame thread only

__declspec(noinline) bool guardedWriteDword(uintptr_t address, int32_t value) noexcept {
    __try {
        *reinterpret_cast<volatile int32_t*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void refuseHud(const std::string& why) {
    if (why == g_hudRefusalText) return;   // the same reason: already said
    g_hudRefusalText = why;
    Log::get().note("native on foot hud: override EliteDangerous64.exe+0x%llX NOT written -- %s. The game's value is left as it is.",
                    static_cast<unsigned long long>(nofoot::kHudOverrideRva), why.c_str());
}

// `want`: the override should be at 1 now. `restoreReason` (a literal) says why it is being put back.
void hudOverrideTick(bool want, const char* restoreReason) {
    const uintptr_t address = g_base + nofoot::kHudOverrideRva;
    if (want) {
        if (g_hudOverrideHeld || g_install != InstallState::Armed) return;
        const char* idWhy = nullptr;
        if (!checkIdentity(g_base, &idWhy)) {
            refuseHud(std::string("the PE identity check failed (") + idWhy + ")");
            return;
        }
        int32_t current = 0;
        if (!guardedRead(address, &current, 4)) {
            refuseHud("the value could not be read");
            return;
        }
        if (current != -1) {
            char b[128];
            std::snprintf(b, sizeof(b), "the value reads %d, not -1, so it is someone else's and is left alone", static_cast<int>(current));
            refuseHud(b);
            return;
        }
        if (!writableByte(address) || !guardedWriteDword(address, 1)) {
            refuseHud("the value is not writable");
            return;
        }
        g_hudOverrideHeld = true;
        g_hudRefusalText.clear();
        Log::get().note("native on foot hud: override EliteDangerous64.exe+0x%llX -1 -> 1", static_cast<unsigned long long>(nofoot::kHudOverrideRva));
        return;
    }
    if (!g_hudOverrideHeld) {
        g_hudRefusalText.clear();
        return;
    }
    int32_t current = 0;
    if (!guardedRead(address, &current, 4)) return;   // retried next frame
    if (current == -1) {
        g_hudOverrideHeld = false;
        Log::get().note("native on foot hud: override EliteDangerous64.exe+0x%llX already -1 (restore: %s)",
                        static_cast<unsigned long long>(nofoot::kHudOverrideRva), restoreReason);
        return;
    }
    if (current != 1) {
        g_hudOverrideHeld = false;
        Log::get().note("native on foot hud: override EliteDangerous64.exe+0x%llX reads %d, not our 1, so it is left alone (restore: %s)",
                        static_cast<unsigned long long>(nofoot::kHudOverrideRva), static_cast<int>(current), restoreReason);
        return;
    }
    if (!writableByte(address) || !guardedWriteDword(address, -1)) {
        if (!g_hudRestoreFailedNoted) {
            g_hudRestoreFailedNoted = true;
            Log::get().note("native on foot hud: override EliteDangerous64.exe+0x%llX could not be put back to -1 (%s); retrying each frame.",
                            static_cast<unsigned long long>(nofoot::kHudOverrideRva), restoreReason);
        }
        return;
    }
    g_hudOverrideHeld = false;
    g_hudRestoreFailedNoted = false;
    Log::get().note("native on foot hud: override EliteDangerous64.exe+0x%llX 1 -> -1 (restored: %s)",
                    static_cast<unsigned long long>(nofoot::kHudOverrideRva), restoreReason);
}

// ---- F8: the first-person HUD census (read only; frame thread) ---------------------------------------------------------------------
// Each frame during a game session the state is read from the frozen selector; the line is written every 5 s, and at most every 200 ms
// when the text changes.
uint64_t g_hudCensusLogMs = 0;             // frame thread only: when the last line was written
std::string g_hudCensusLast;               // frame thread only

std::string censusByteText(bool ok, uint8_t v) {
    if (!ok) return "?";
    char b[8];
    std::snprintf(b, sizeof(b), "%02X", static_cast<unsigned>(v));
    return b;
}

// One slot (0 or 1) of the request's pass list: F70, fp_on and pg250.
std::string censusSlotText(const char* label, bool okW, uintptr_t slotAddr) {
    uint64_t slot = 0, F = 0, node = 0, pg = 0;
    uint8_t f70 = 0, fpOn = 0, pg250 = 0;
    const bool okSlot = okW && guardedRead(slotAddr, &slot, 8) && slot;
    const bool okF = okSlot && guardedRead(static_cast<uintptr_t>(slot) + nofoot::kCensusPassOff, &F, 8) && F;
    const bool okF70 = okF && guardedRead(static_cast<uintptr_t>(F) + nofoot::kCensusF70Off, &f70, 1);
    const bool okNode = okF && guardedRead(static_cast<uintptr_t>(F) + nofoot::kCensusNodeOff, &node, 8) && node;
    const bool okFpOn = okNode && guardedRead(static_cast<uintptr_t>(node) + nofoot::kCensusNodeOnOff, &fpOn, 1);
    const bool okPg = okF && guardedRead(static_cast<uintptr_t>(F) + nofoot::kCensusPgOff, &pg, 8) && pg;
    const bool okPg250 = okPg && guardedRead(static_cast<uintptr_t>(pg) + nofoot::kCensusPg250Off, &pg250, 1);
    return std::string(" ") + label + " F70=" + censusByteText(okF70, f70) + " fp_on=" + censusByteText(okFpOn, fpOn) +
           " pg250=" + censusByteText(okPg250, pg250);
}

void hudCensusTick(uint64_t nowMs, bool session) {
    if (!session) return;
    const uint64_t sel = g_frozenSelector.load(std::memory_order_acquire);
    std::string text = "native on foot hud census:";
    if (!sel) {
        text += " no selector frozen yet";
    } else {
        uint64_t owner = 0, settings = 0, rc = 0, w = 0;
        const bool okO = guardedRead(static_cast<uintptr_t>(sel) + nofoot::kCensusOwnerOff, &owner, 8) && owner;
        const bool okS = okO && guardedRead(static_cast<uintptr_t>(owner) + nofoot::kCensusSettingsOff, &settings, 8) && settings;
        const bool okRc = okS && guardedRead(static_cast<uintptr_t>(settings) + nofoot::kCensusRcOff, &rc, 8) && rc;
        float fov = 0.0f;
        const bool okFov = okRc && guardedRead(static_cast<uintptr_t>(rc) + nofoot::kCensusFovOff, &fov, 4);
        uint32_t mode = 0;
        const bool okMode = okS && guardedRead(static_cast<uintptr_t>(settings) + nofoot::kCensusModeOff, &mode, 4);
        uint32_t selMode = 0;
        const bool okSel = guardedRead(static_cast<uintptr_t>(sel) + nofoot::kCensusSelModeOff, &selMode, 4);
        int32_t ovr = 0;
        const bool okOvr = guardedRead(g_base + nofoot::kHudOverrideRva, &ovr, 4);
        const bool okW = okS && guardedRead(static_cast<uintptr_t>(settings) + nofoot::kCensusWOff, &w, 8) && w;
        char fovB[24], modeB[16], selB[16], ovrB[16];
        if (okFov) std::snprintf(fovB, sizeof(fovB), "%.3f", static_cast<double>(fov));
        else std::snprintf(fovB, sizeof(fovB), "?");
        if (okMode) std::snprintf(modeB, sizeof(modeB), "%u", static_cast<unsigned>(mode));
        else std::snprintf(modeB, sizeof(modeB), "?");
        if (okSel) std::snprintf(selB, sizeof(selB), "%u", static_cast<unsigned>(selMode));
        else std::snprintf(selB, sizeof(selB), "?");
        if (okOvr) std::snprintf(ovrB, sizeof(ovrB), "%d", static_cast<int>(ovr));
        else std::snprintf(ovrB, sizeof(ovrB), "?");
        text += std::string(" fov=") + fovB + " mode=" + modeB + " sel=" + selB + " ovr=" + ovrB;
        text += censusSlotText("slot0", okW, static_cast<uintptr_t>(w) + nofoot::kCensusSlotA);
        text += censusSlotText("slot1", okW, static_cast<uintptr_t>(w) + nofoot::kCensusSlotB);
    }
    const bool due5 = g_hudCensusLogMs == 0 || nowMs - g_hudCensusLogMs >= 5000;
    const bool changedNow = text != g_hudCensusLast && (g_hudCensusLogMs == 0 || nowMs - g_hudCensusLogMs >= 200);
    if (due5 || changedNow) {
        g_hudCensusLast = text;
        g_hudCensusLogMs = nowMs;
        Log::get().note("%s", text.c_str());
    }
}

// ---- F9: two code patches (TEMPORARY; approved by Sean 2026-10-14) -----------------------------------------------------------------
// Each patch is applied only while native stereo is on foot, its own key is on, and the first-person HUD override (F8) is held. The bytes
// are verified before the write (refused and logged otherwise), and restored to the exact original bytes when any of those stops holding,
// at shutdown, or when a check fails. A restore writes back only while the bytes still read as this patch left them.
struct CodePatch {
    const char* name;        // the log's name: "panels" or "hud mesh"
    uintptr_t rva;
    uint8_t from[2];         // the bytes the patch expects (both checked)
    uint8_t to[2];           // the bytes it leaves (both checked on a restore)
    size_t len;              // bytes written: 2, or 1 for the mesh patch
    const char* shape;       // the log's shape, for example "76 73 -> 90 90"
    bool applied = false;    // frame thread only
    std::string refusal;     // frame thread only
    bool restoreNoted = false;
};
CodePatch g_panelPatch{"panels", nofoot::kPanelPatchRva, {nofoot::kPanelPatchFrom[0], nofoot::kPanelPatchFrom[1]},
                       {nofoot::kPanelPatchTo[0], nofoot::kPanelPatchTo[1]}, nofoot::kPanelPatchLen, "76 73 -> 90 90"};

// A store of `len` bytes (2, or 1) at `address`, under VirtualProtect to PAGE_EXECUTE_READWRITE and back, with the instruction cache flushed.
__declspec(noinline) bool guardedStoreBytes(uintptr_t address, uint16_t value, size_t len) noexcept {
    __try {
        if (len == 2) *reinterpret_cast<volatile uint16_t*>(address) = value;
        else *reinterpret_cast<volatile uint8_t*>(address) = static_cast<uint8_t>(value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool storeCodeBytes(uintptr_t address, uint16_t value, size_t len) noexcept {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), len, PAGE_EXECUTE_READWRITE, &old)) return false;
    const bool ok = guardedStoreBytes(address, value, len);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(address), len, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), len);
    return ok;
}

void refusePatch(CodePatch& p, const std::string& why) {
    if (why == p.refusal) return;   // the same reason: already said
    p.refusal = why;
    Log::get().note("native on foot %s: patch EliteDangerous64.exe+0x%llX NOT patched -- %s. The game's bytes are left as they are.", p.name,
                    static_cast<unsigned long long>(p.rva), why.c_str());
}

// `want`: the patch should be applied now. `reason` (a literal) says why it is being put back.
void codePatchTick(CodePatch& p, bool want, const char* reason) {
    const uintptr_t address = g_base + p.rva;
    if (want) {
        if (p.applied || g_install != InstallState::Armed) return;
        const char* idWhy = nullptr;
        if (!checkIdentity(g_base, &idWhy)) {
            refusePatch(p, std::string("the PE identity check failed (") + idWhy + ")");
            return;
        }
        uint8_t cur[2] = {};
        if (!guardedRead(address, cur, 2)) {
            refusePatch(p, "the bytes could not be read");
            return;
        }
        if (cur[0] != p.from[0] || cur[1] != p.from[1]) {
            char b[160];
            std::snprintf(b, sizeof(b), "the bytes are %02X %02X, not %02X %02X, so they are someone else's and are left alone",
                          static_cast<unsigned>(cur[0]), static_cast<unsigned>(cur[1]), static_cast<unsigned>(p.from[0]),
                          static_cast<unsigned>(p.from[1]));
            refusePatch(p, b);
            return;
        }
        const uint16_t value = static_cast<uint16_t>(p.to[0] | (p.to[1] << 8));
        if (!storeCodeBytes(address, value, p.len)) {
            refusePatch(p, "the bytes are not writable");
            return;
        }
        p.applied = true;
        p.refusal.clear();
        Log::get().note("native on foot %s: patch EliteDangerous64.exe+0x%llX %s", p.name, static_cast<unsigned long long>(p.rva), p.shape);
        return;
    }
    if (!p.applied) {
        p.refusal.clear();
        return;
    }
    uint8_t cur[2] = {};
    if (!guardedRead(address, cur, 2)) return;   // retried next frame
    if (cur[0] != p.to[0] || cur[1] != p.to[1]) {
        p.applied = false;
        Log::get().note("native on foot %s: patch EliteDangerous64.exe+0x%llX NOT restored -- the bytes are %02X %02X, not ours (restore: %s)",
                        p.name, static_cast<unsigned long long>(p.rva), static_cast<unsigned>(cur[0]), static_cast<unsigned>(cur[1]), reason);
        return;
    }
    const uint16_t value = static_cast<uint16_t>(p.from[0] | (p.from[1] << 8));
    if (!storeCodeBytes(address, value, p.len)) {
        if (!p.restoreNoted) {
            p.restoreNoted = true;
            Log::get().note("native on foot %s: patch EliteDangerous64.exe+0x%llX could not be restored (%s); retrying each frame.", p.name,
                            static_cast<unsigned long long>(p.rva), reason);
        }
        return;
    }
    p.applied = false;
    p.restoreNoted = false;
    Log::get().note("native on foot %s: patch EliteDangerous64.exe+0x%llX restored (%s; %02X %02X -> %02X %02X)", p.name,
                    static_cast<unsigned long long>(p.rva), reason, static_cast<unsigned>(p.to[0]), static_cast<unsigned>(p.to[1]),
                    static_cast<unsigned>(p.from[0]), static_cast<unsigned>(p.from[1]));
}

// ---- F11: the two mesh patches (TEMPORARY; approved by Sean 2026-10-14) ------------------------------------------------------------
// fix.native_on_foot_hud_mesh: the HRC creation gate, 80 -> 78 at +0x1A149A8 (verify FF 90 80 00 00 00 at +0x1A149A6).
// F34: the world placement A/B (74 -> EB at +0x1A36453) is removed with its key.
// Each is applied only while native stereo is on foot and the first-person HUD override is held. Its bytes are verified before the write
// (refused and logged otherwise), and the original byte is restored only while the verified bytes still read as this patch left them.
struct MeshSite {
    const char* name;        // the log's name: "hud mesh" or "hud mesh world"
    uintptr_t verifyRva;     // where the expected bytes are read
    size_t verifyLen;        // 6 for the creation gate, 2 for the placement branch
    uint8_t before[6];       // the bytes before the write
    uint8_t after[6];        // the bytes after it (what a restore expects)
    uintptr_t writeRva;      // the one byte written
    uint8_t writeFrom;       // its original value
    uint8_t writeTo;         // its patched value
    const char* shape;       // the log's shape: "80 -> 78" or "74 -> EB"
    bool applied = false;    // frame thread only
    std::string refusal;     // frame thread only
    bool restoreNoted = false;
};
MeshSite g_meshCreate{"hud mesh", 0x1A149A6, 6, {0xFF, 0x90, 0x80, 0x00, 0x00, 0x00}, {0xFF, 0x90, 0x78, 0x00, 0x00, 0x00}, 0x1A149A8,
                      0x80, 0x78, "80 -> 78"};


// "FF 90 80 00 00 00" for the first `n` bytes.
std::string meshHex(const uint8_t* b, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        char t[4];
        std::snprintf(t, sizeof(t), i ? " %02X" : "%02X", static_cast<unsigned>(b[i]));
        s += t;
    }
    return s;
}

void refuseMeshSite(MeshSite& s, const std::string& why) {
    if (why == s.refusal) return;   // the same reason: already said
    s.refusal = why;
    Log::get().note("native on foot %s: patch EliteDangerous64.exe+0x%llX NOT patched -- %s. The game's bytes are left as they are.", s.name,
                    static_cast<unsigned long long>(s.writeRva), why.c_str());
}

// `want`: the patch should be applied now. `reason` (a literal) says why it is being put back.
void meshSiteTick(MeshSite& s, bool want, const char* reason) {
    const uintptr_t va = g_base + s.verifyRva;
    const uintptr_t wa = g_base + s.writeRva;
    if (want) {
        if (s.applied || g_install != InstallState::Armed) return;
        const char* idWhy = nullptr;
        if (!checkIdentity(g_base, &idWhy)) {
            refuseMeshSite(s, std::string("the PE identity check failed (") + idWhy + ")");
            return;
        }
        uint8_t cur[6] = {};
        if (!guardedRead(va, cur, s.verifyLen)) {
            refuseMeshSite(s, "the bytes could not be read");
            return;
        }
        for (size_t i = 0; i < s.verifyLen; ++i) {
            if (cur[i] != s.before[i]) {
                refuseMeshSite(s, "the bytes are " + meshHex(cur, s.verifyLen) + ", not " + meshHex(s.before, s.verifyLen) +
                                      ", so they are someone else's and are left alone");
                return;
            }
        }
        if (!storeCodeBytes(wa, s.writeTo, 1)) {
            refuseMeshSite(s, "the byte is not writable");
            return;
        }
        s.applied = true;
        s.refusal.clear();
        Log::get().note("native on foot %s: patch EliteDangerous64.exe+0x%llX %s", s.name, static_cast<unsigned long long>(s.writeRva), s.shape);
        return;
    }
    if (!s.applied) {
        s.refusal.clear();
        return;
    }
    uint8_t cur[6] = {};
    if (!guardedRead(va, cur, s.verifyLen)) return;   // retried next frame
    for (size_t i = 0; i < s.verifyLen; ++i) {
        if (cur[i] != s.after[i]) {
            s.applied = false;
            Log::get().note("native on foot %s: patch EliteDangerous64.exe+0x%llX NOT restored -- the bytes are %s, not ours (restore: %s)", s.name,
                            static_cast<unsigned long long>(s.writeRva), meshHex(cur, s.verifyLen).c_str(), reason);
            return;
        }
    }
    if (!storeCodeBytes(wa, s.writeFrom, 1)) {
        if (!s.restoreNoted) {
            s.restoreNoted = true;
            Log::get().note("native on foot %s: patch could not be restored (%s); retrying each frame.", s.name, reason);
        }
        return;
    }
    s.applied = false;
    s.restoreNoted = false;
    char shape[16];
    std::snprintf(shape, sizeof(shape), "%02X -> %02X", static_cast<unsigned>(s.writeTo), static_cast<unsigned>(s.writeFrom));
    Log::get().note("native on foot %s: patch EliteDangerous64.exe+0x%llX %s (restored: %s)", s.name, static_cast<unsigned long long>(s.writeRva), shape,
                    reason);
}

// ---- F9: three read-only probes (TEMPORARY) -----------------------------------------------------------------------------------------
// P1 (panel submit), P2 (the HRC submit; six arguments forwarded), P3 (the per-instance submit: the HRC's instances only). Each reads game
// memory under SEH and forwards the registers unchanged; a probe stands down after sixteen faults. Aggregates are per 5 s and per state;
// the HRC line is also written when its values change. No game thread writes a line.
const char* const g_f9StateName[3] = {"native", "cinema", "ship/other"};
uint8_t f9State() noexcept {
    if (nativeOnFootActive()) return 0;
    if (journalOnFootKnown() && journalOnFoot()) return 1;
    return 2;
}

struct F9Probe {
    uint8_t* relay = nullptr;
    alignas(8) std::atomic<uintptr_t> gate{0};
    void* original = nullptr;   // the trampoline; cast at the call (four or six arguments)
    CodeHook* hook = nullptr;
};
F9Probe g_f9Probe[nofoot::kF9ProbeCount];
std::atomic<bool> g_f9Stop[nofoot::kF9ProbeCount]{};
std::atomic<uint32_t> g_f9Faults[nofoot::kF9ProbeCount]{};
std::atomic<uintptr_t> g_hrcInst[nofoot::kF9Hrc]{};   // published by P2 each call, read by P3
bool g_f9Tried = false;                               // frame thread only
bool g_f9Armed[nofoot::kF9ProbeCount] = {};           // frame thread only
uint64_t g_f9LastMs = 0;                              // frame thread only

using F9Fn4 = uint64_t(__fastcall*)(void*, void*, uint64_t, uint64_t);
using F9Fn6 = uint64_t(__fastcall*)(void*, void*, uint64_t, uint64_t, uint64_t, uint64_t);

bool f9Read(size_t probe, uintptr_t address, void* out, size_t bytes) noexcept {
    if (guardedRead(address, out, bytes)) return true;
    if (g_f9Faults[probe].fetch_add(1, std::memory_order_relaxed) + 1 >= 16) g_f9Stop[probe].store(true, std::memory_order_relaxed);
    return false;
}
// An image RVA of a pointer value, 0 when it is outside the image.
uint32_t f9Rva(uint64_t value) noexcept {
    const uintptr_t base = g_base;
    if (!base || value < base || value >= base + ecm::kExpectedImageSize) return 0;
    return static_cast<uint32_t>(value - base);
}
std::string f9RvaText(uint32_t rva, bool ok) {
    if (!ok || !rva) return "?";
    char b[24];
    std::snprintf(b, sizeof(b), "+0x%X", static_cast<unsigned>(rva));
    return b;
}

// P1 aggregates: per (state, caller RVA).
struct F9CallerRow {
    bool used = false;
    uint8_t st = 0;
    uint32_t caller = 0;
    uint64_t calls = 0, window = 0;
};
constexpr size_t kF9Callers = 8;
F9CallerRow g_f9Callers[kF9Callers];   // guarded by g_f9Lock

// P2 aggregates: per state, the last values and the window's calls.
struct F9HrcState {
    bool seen = false;
    uint8_t g291 = 0, g470 = 0, g4C0 = 0;
    bool ok291 = false, ok470 = false, ok4C0 = false;
    uint64_t inst[nofoot::kF9Hrc] = {};
    bool instOk[nofoot::kF9Hrc] = {};
    uint64_t dep = 0;
    uint32_t depVt = 0;
    bool depOk = false, depVtOk = false;
    uint64_t window = 0;
    uint64_t hrc = 0;            // the HRC's address (the matrix line names it)
    uint64_t matMs = 0;          // when the matrix was last sampled (GetTickCount64)
    bool matOk = false;
    float mat[16] = {};          // the HRC's matrix at +0x430..0x46F
};
F9HrcState g_f9Hrc[3];   // guarded by g_f9Lock

// P3 aggregates: per (state, instance, view id), the last camera position.
struct F9InstRow {
    bool used = false;
    uint8_t st = 0, k = 0;
    uint32_t viewId = 0;
    bool viewOk = false;
    uint64_t calls = 0, window = 0;
    float cam[3] = {};
    float rot[12] = {};          // the view's floats at +0x20..+0x4C (camOk covers both)
    bool camOk = false;
};
constexpr size_t kF9Insts = 16;
F9InstRow g_f9Insts[kF9Insts];   // guarded by g_f9Lock

void f9RecordCaller(uint8_t st, uint32_t caller) noexcept {
    F9Lock lock;
    for (auto& r : g_f9Callers) {
        if (r.used && r.st == st && r.caller == caller) {
            ++r.calls;
            ++r.window;
            return;
        }
    }
    for (auto& r : g_f9Callers) {
        if (!r.used) {
            r.used = true;
            r.st = st;
            r.caller = caller;
            r.calls = r.window = 1;
            return;
        }
    }
}

void f9RecordHrc(uint8_t st, void* hrc) noexcept {
    const uintptr_t h = reinterpret_cast<uintptr_t>(hrc);
    uint8_t g291 = 0, g470 = 0, g4C0 = 0;
    const bool ok291 = f9Read(1, h + nofoot::kF9HrcG291Off, &g291, 1);
    const bool ok470 = f9Read(1, h + nofoot::kF9HrcG470Off, &g470, 1);
    const bool ok4C0 = f9Read(1, h + nofoot::kF9HrcG4C0Off, &g4C0, 1);
    uint64_t inst[nofoot::kF9Hrc] = {};
    bool instOk[nofoot::kF9Hrc] = {};
    for (size_t k = 0; k < nofoot::kF9Hrc; ++k) {
        instOk[k] = f9Read(1, h + nofoot::kF9HrcInst0Off + 8 * k, &inst[k], 8);
        g_hrcInst[k].store(instOk[k] ? static_cast<uintptr_t>(inst[k]) : 0, std::memory_order_relaxed);
    }
    uint64_t dep = 0, vt = 0;
    const bool depOk = f9Read(1, h + nofoot::kF9HrcDepOff, &dep, 8);
    const bool depVtOk = depOk && dep >= 0x10000 && f9Read(1, static_cast<uintptr_t>(dep), &vt, 8);
    const uint32_t depVt = depVtOk ? f9Rva(vt) : 0;
    // The HRC's world-to-view matrix (16 floats), at most once per 5 s per state.
    const uint64_t now = GetTickCount64();
    bool wantMat = false;
    {
        F9Lock lock;
        wantMat = g_f9Hrc[st].matMs == 0 || now - g_f9Hrc[st].matMs >= 5000;
    }
    float mat[16] = {};
    const bool matOk = wantMat && f9Read(1, h + nofoot::kF9HrcMatrixOff, mat, 64);
    F9Lock lock;
    F9HrcState& s = g_f9Hrc[st];
    s.seen = true;
    s.g291 = g291; s.g470 = g470; s.g4C0 = g4C0;
    s.ok291 = ok291; s.ok470 = ok470; s.ok4C0 = ok4C0;
    for (size_t k = 0; k < nofoot::kF9Hrc; ++k) {
        s.inst[k] = inst[k];
        s.instOk[k] = instOk[k];
    }
    s.dep = dep;
    s.depOk = depOk;
    s.depVt = depVt;
    s.depVtOk = depVtOk;
    s.hrc = h;
    if (wantMat) {
        s.matMs = now;
        s.matOk = matOk;
        if (matOk) std::memcpy(s.mat, mat, sizeof(mat));
    }
    ++s.window;
}

void f9RecordInst(uint8_t st, uint8_t k, void* view) noexcept {
    const uintptr_t v = reinterpret_cast<uintptr_t>(view);
    uint32_t vid = 0;
    const bool okId = v && f9Read(2, v + nofoot::kF9ViewIdOff, &vid, 4);
    // The view's floats from +0x20 to +0x5B: the rotation rows (+0x20..+0x4C, twelve floats) and the camera position (+0x50, +0x54, +0x58).
    float blk[15] = {};
    const bool okBlk = v && f9Read(2, v + nofoot::kF9ViewRotOff, blk, sizeof(blk));
    F9Lock lock;
    F9InstRow* row = nullptr;
    for (auto& r : g_f9Insts) {
        if (r.used && r.st == st && r.k == k && r.viewOk == okId && r.viewId == vid) {
            row = &r;
            break;
        }
    }
    if (!row) {
        for (auto& r : g_f9Insts) {
            if (!r.used) {
                row = &r;
                break;
            }
        }
        if (!row) return;
        row->used = true;
        row->st = st;
        row->k = k;
        row->viewId = vid;
        row->viewOk = okId;
    }
    ++row->calls;
    ++row->window;
    if (okBlk) {
        std::memcpy(row->rot, blk, sizeof(row->rot));
        row->cam[0] = blk[12];
        row->cam[1] = blk[13];
        row->cam[2] = blk[14];
        row->camOk = true;
    }
}

// P1: rcx = set, rdx = view, r8 = flag, r9. The caller is taken at entry (the relay jumps here, so it is the game's return address).
uint64_t __fastcall f9PanelSubmitHooked(void* set, void* view, uint64_t r8, uint64_t r9) {
    void* caller = _ReturnAddress();
    const F9Fn4 original = reinterpret_cast<F9Fn4>(g_f9Probe[0].original);
    if (!original) return 0;
    if (!g_f9Stop[0].load(std::memory_order_relaxed)) {
        const uintptr_t c = reinterpret_cast<uintptr_t>(caller);
        const uint32_t rva = f9Rva(c);
        f9RecordCaller(f9State(), rva);
    }
    return original(set, view, r8, r9);
}

// ---- F12 + F13: the HUD placement (user settings) -------------------------------------------------------------------------------------
// While native stereo is on foot and the creation patch is in, each HRC placement matrix (+0x430 and +0x480, 16 floats, row-major, row-vector
// convention p' = p . M, translation in row 3) is built from the game's fresh values (its base) in this order:
//   1. distance: s = distance / 0.192 scales x, y, z of every row (the basis and the translation); w kept.
//   2. size: the basis rows (0-2) x, y, z times size; the translation row is unchanged (it stays at the same depth).
//   3. follow = head: M' = M . C (or C . M with fix.native_on_foot_hud_follow_order = before), C = the head transform from the list-B views:
//      rows 0-2 of the first view, and row 3 = the mean position of the first two views; smoothed toward the raw C (slerp + lerp).
// No compounding: each matrix keeps its base and what was last written with which inputs. The game recomputes the matrices every frame; if
// the current values equal what was written and the inputs are the same, nothing is done; if the inputs changed, the final is rebuilt from the
// base. Neutral inputs (distance at its default, size 1, follow body) put the base back once. A fault stands the writes down for the session.
struct F13Inputs {
    float s = 1.0f;
    float size = 1.0f;
    bool head = false;
    float C[16] = {};
};
struct F12Matrix {
    bool have = false;           // a write of ours is in place for this HRC
    float base[16] = {};         // the game's values (before our last write)
    float written[16] = {};      // what we last wrote
    F13Inputs in;                // the inputs of that write
};
F12Matrix g_f12Mat[2];                              // game thread (the P2 hook) only
uintptr_t g_f12Hrc = 0;                             // game thread only: the HRC the state belongs to
std::atomic<bool> g_f12Stop{false};
std::atomic<const char*> g_f12StopWhy{nullptr};
bool g_f12StopNoted = false;                        // frame thread only

struct F13Head {
    bool valid = false;
    float C[16] = {};
    float viewY = 0.0f;   // F22: the list-B eye 0 view's row 2, column 1 (asin = its pitch); set by f13ReadHeadInto only
    float viewYaw = 0.0f; // F24: the list-B eye 0 view's forward yaw (atan2 of row 2), set by f13ReadHeadInto only
    float eyeDiff = 0.0f; // F29: the largest rotation difference between the two eyes' views in this sample
};
F13Head g_f13Raw;
F13Head g_worldC;                                  // F27: the list-B head sample from the P2 hook, for the world markers (any HUD follow); under g_f9Lock
uint64_t g_worldCMs = 0;                            // F27: GetTickCount64 of g_worldC's sample   // guarded by g_f9Lock: the head transform read by the P2 hook
uint64_t g_worldCSeq = 0;                           // F29: list-B sample number of g_worldC (the P2 hook's read)
// F30 part 2: one counter for every list-B head read (the P2 hook and the marker entry), so the two sources' sample numbers compare.
std::atomic<uint64_t> g_listBSeq{0};
uint64_t g_worldCQpc = 0;                           // F30: QueryPerformanceCounter of g_worldC's sample   // under g_f9Lock
uint64_t f30Qpc() noexcept {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}
uint64_t f30QpcUs(uint64_t ticks) noexcept {
    static const uint64_t freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? static_cast<uint64_t>(f.QuadPart) : uint64_t{1};
    }();
    return ticks * 1000000ull / freq;
}
// F30 part 2: the head C of THIS marker call. The marker pre-hook reads list B (r8) on the game thread; the post stub runs later in the same call
// on the same thread. Valid only for that call: the pre-hook clears it on entry.
struct F30SameCall {
    bool valid = false;
    bool haveEntry = false;
    F13Head head{};
    uint64_t entrySeq = 0;   // g_listBSeq at the marker entry, before its own read
    uint64_t seq = 0;        // the marker's own list-B sample number
    uint64_t qpc = 0;        // QueryPerformanceCounter at its read
    bool mOk = false;        // F37: M (the marker camera matrix) read at this entry, orthonormal
    float mMat[16] = {};
};
thread_local F30SameCall t_f30SameCall;
// The 5 s window's metrics (game thread: the post stub; reset on each 5 s line).
uint64_t g_f30PostSame = 0, g_f30PostFall = 0, g_f30PostNone = 0;
int64_t g_f30LagMax = 0;
uint64_t g_f30AgeMaxUs = 0, g_f30AgeSumUs = 0, g_f30AgeN = 0;
// F31: the last post's same-call C and the runtime's "now" head as a transform of C (H_M), with the answer and frame numbers; the window's counts.
float g_f31SameC[16] = {}, g_f31NowH[16] = {};
bool g_f31HaveNow = false;
double g_f31AngleDeg = 0.0;
uint32_t g_f31NowSeq = 0;
uint64_t g_f31FrameSeq = 0, g_f31NoNow = 0;
// F37: the ring match: the chosen k histogram, the residual (degrees, M against the chosen view), the fallbacks, and the QPC ages (us).
int g_f37K = -1;
uint64_t g_f37Hist[8] = {}, g_f37Fallback = 0, g_f37ResN = 0;
double g_f37ResSum = 0.0;
uint64_t g_f37AgePickMax = 0, g_f37AgeNewMax = 0;
bool g_f31FirstRead = false, g_f31FirstValid = false, g_f31LastNowValid = false;
uint32_t g_f31LastInputVersion = 0;
F13Head g_f13Smooth;                                // guarded by g_f9Lock: the smoothed one (the frame thread)
uint64_t g_f13LastQpc = 0;                          // frame thread only
uint64_t g_f13LogMs = 0;                            // frame thread only

bool f12Near(float a, float b) noexcept {
    const float d = a - b;
    return d < 1e-6f && d > -1e-6f;
}

void f12Stand(const char* why) noexcept {
    g_f12StopWhy.store(why, std::memory_order_relaxed);
    g_f12Stop.store(true, std::memory_order_relaxed);
}

// Writes the 16 floats at `address` under SEH.
__declspec(noinline) bool guardedWriteFloats16(uintptr_t address, const float* v) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void*>(address), v, 16 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// out = A . B, 4x4 row-major.
void f13Mul(const float* A, const float* B, float* out) noexcept {
    float t[16];
    for (size_t i = 0; i < 4; ++i) {
        for (size_t j = 0; j < 4; ++j) {
            float acc = 0.0f;
            for (size_t k = 0; k < 4; ++k) acc += A[i * 4 + k] * B[k * 4 + j];
            t[i * 4 + j] = acc;
        }
    }
    std::memcpy(out, t, sizeof(t));
}

// The quaternion (w, x, y, z) of the rotation whose rows are m[0..2], m[4..6], m[8..10] (a 4x4 row-major matrix).
void f13QuatFromRows(const float* m, float* q) noexcept {
    const float r00 = m[0], r01 = m[1], r02 = m[2];
    const float r10 = m[4], r11 = m[5], r12 = m[6];
    const float r20 = m[8], r21 = m[9], r22 = m[10];
    const float tr = r00 + r11 + r22;
    if (tr > 0.0f) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q[0] = 0.25f * s; q[1] = (r21 - r12) / s; q[2] = (r02 - r20) / s; q[3] = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        q[0] = (r21 - r12) / s; q[1] = 0.25f * s; q[2] = (r01 + r10) / s; q[3] = (r02 + r20) / s;
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        q[0] = (r02 - r20) / s; q[1] = (r01 + r10) / s; q[2] = 0.25f * s; q[3] = (r12 + r21) / s;
    } else {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        q[0] = (r10 - r01) / s; q[1] = (r02 + r20) / s; q[2] = (r12 + r21) / s; q[3] = 0.25f * s;
    }
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0.0f) for (size_t i = 0; i < 4; ++i) q[i] /= n;
}

// Writes the rotation of q into the rows of the 4x4 m (the w column and the translation row are left as they are).
void f13RowsFromQuat(const float* q, float* m) noexcept {
    const float w = q[0], x = q[1], y = q[2], z = q[3];
    m[0] = 1.0f - 2.0f * (y * y + z * z); m[1] = 2.0f * (x * y - w * z);        m[2] = 2.0f * (x * z + w * y);
    m[4] = 2.0f * (x * y + w * z);        m[5] = 1.0f - 2.0f * (x * x + z * z); m[6] = 2.0f * (y * z - w * x);
    m[8] = 2.0f * (x * z - w * y);        m[9] = 2.0f * (y * z + w * x);        m[10] = 1.0f - 2.0f * (x * x + y * y);
}

// Spherical interpolation of two unit quaternions, t in [0, 1].
void f13Slerp(const float* a, const float* b, float t, float* out) noexcept {
    float bb[4] = {b[0], b[1], b[2], b[3]};
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0f) {
        d = -d;
        for (size_t i = 0; i < 4; ++i) bb[i] = -bb[i];
    }
    float sa = 1.0f - t, sb = t;
    if (d < 0.9995f) {
        const float theta = std::acos(d);
        const float sinTheta = std::sin(theta);
        sa = std::sin((1.0f - t) * theta) / sinTheta;
        sb = std::sin(t * theta) / sinTheta;
    }
    for (size_t i = 0; i < 4; ++i) out[i] = sa * a[i] + sb * bb[i];
    const float n = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
    if (n > 0.0f) for (size_t i = 0; i < 4; ++i) out[i] /= n;
}

// The head transform from the list-B views (P2, game thread): a3 is list B's {begin, end}; its entries are view pointers. A read or a
// shape that is not as expected stands the writes down.
// F22: the pure reader. Returns false with `why` set on a read or shape failure, and writes nothing; the caller decides what a failure means.
bool f13ReadHeadInto(uintptr_t listB, F13Head& raw, const char** why) noexcept {
    uint64_t begin = 0, end = 0;
    if (!guardedRead(listB, &begin, 8) || !guardedRead(listB + 8, &end, 8)) {
        *why = "list B could not be read";
        return false;
    }
    if (begin < 0x10000 || end < begin || end - begin < 16) {
        *why = "list B does not hold two views";
        return false;
    }
    uint64_t v0 = 0, v1 = 0;
    if (!guardedRead(static_cast<uintptr_t>(begin), &v0, 8) || !guardedRead(static_cast<uintptr_t>(begin) + 8, &v1, 8)) {
        *why = "list B could not be read";
        return false;
    }
    auto plausible = [](uint64_t p) { return p >= 0x10000 && p < 0x00007FFF00000000ull; };
    if (!plausible(v0) || !plausible(v1)) {
        *why = "list B entries are not view pointers";
        return false;
    }
    float a[16] = {}, b[16] = {};
    if (!guardedRead(static_cast<uintptr_t>(v0) + nofoot::kF9ViewRotOff, a, sizeof(a)) ||
        !guardedRead(static_cast<uintptr_t>(v1) + nofoot::kF9ViewRotOff, b, sizeof(b))) {
        *why = "a list-B view could not be read";
        return false;
    }
    raw = F13Head{};
    raw.valid = true;
    for (size_t r = 0; r < 3; ++r) {
        for (size_t c = 0; c < 3; ++c) raw.C[r * 4 + c] = a[r * 4 + c];
        raw.C[r * 4 + 3] = 0.0f;
    }
    raw.C[12] = 0.5f * (a[12] + b[12]);
    raw.C[13] = 0.5f * (a[13] + b[13]);
    raw.C[14] = 0.5f * (a[14] + b[14]);
    raw.C[15] = 1.0f;
    raw.C[3] = raw.C[7] = raw.C[11] = 0.0f;
    raw.viewY = a[9];   // F22: eye 0's own row 2, column 1 (its pitch is asin of this; the probe reads it)
    {
        // F29: how far eye 0's and eye 1's rotations differ (the centre head is one frame's pose; a large value means the eyes disagree).
        float d = 0.0f;
        for (size_t k = 0; k < 9; ++k) d = std::max(d, std::fabs(a[k] - b[k]));
        raw.eyeDiff = d;
    }
    raw.viewYaw = std::atan2(a[8], a[10]);   // F24: eye 0's forward yaw (row 2, the same reading as C), for the passive sign check
    return true;
}

void f13ReadHead(uintptr_t listB) noexcept {
    F13Head raw;
    const char* why = "";
    if (!f13ReadHeadInto(listB, raw, &why)) {
        f12Stand(why);
        return;
    }
    F9Lock lock;
    g_f13Raw = raw;
}

// The inputs of a placement, for the no-compounding comparison.
bool f13SameInputs(const F13Inputs& a, const F13Inputs& b) noexcept {
    if (!f12Near(a.s, b.s) || !f12Near(a.size, b.size) || a.head != b.head) return false;
    return !a.head || std::memcmp(a.C, b.C, sizeof(a.C)) == 0;
}

// The final matrix from a base and the inputs: distance, then size (the basis only), then the head transform.
void f13Compose(const float* base, const F13Inputs& in, float* out) noexcept {
    float S[16];
    for (size_t r = 0; r < 4; ++r) {
        for (size_t c = 0; c < 4; ++c) {
            float v = base[r * 4 + c];
            if (c < 3) v *= in.s;
            if (r < 3 && c < 3) v *= in.size;
            S[r * 4 + c] = v;
        }
    }
    if (in.head) {
        float T[16];
        f13Mul(S, in.C, T);   // F34: the follow order is the one behaviour (after)
        std::memcpy(out, T, sizeof(T));
    } else {
        std::memcpy(out, S, sizeof(S));
    }
}

// Game thread, the P2 hook, before the original: builds the placement of the two matrices from the current settings.
void f13PlaceHud(void* hrcPtr, uintptr_t listB) noexcept {
    if (g_f12Stop.load(std::memory_order_relaxed)) return;
    if (!nativeOnFootActive() || !g_meshCreateLive.load(std::memory_order_relaxed)) return;
    const float d = g_hudDistance.load(std::memory_order_relaxed);
    const float s = d / nofoot::kHudDefaultDistance;
    // F18: the mesh is the size setting times the room that is applied now (1 unless the room is in effect).
    const float size = g_hudSize.load(std::memory_order_relaxed) * g_hudRoomEff.load(std::memory_order_relaxed);
    const bool wantHead = g_followHead.load(std::memory_order_relaxed);
    const uintptr_t h = reinterpret_cast<uintptr_t>(hrcPtr);
    if (!h) return;
    // F18: the game's own layout scale, read while our element-scale override is off (the field holds the S the layout used; RE, not re-checked).
    if (!g_elemOverrideOn.load(std::memory_order_relaxed)) {
        float sg = 0.0f;
        if (guardedRead(h + 0x4C4, &sg, 4) && sg > 0.0f && sg <= 8.0f) g_sGame.store(sg, std::memory_order_relaxed);
    }
    if (wantHead) {
        f13ReadHead(listB);
        if (g_f12Stop.load(std::memory_order_relaxed)) return;
    }
    F13Inputs in;
    in.s = s;
    in.size = size;
    {
        F9Lock lock;
        const F13Head& src = g_f13Smooth.valid ? g_f13Smooth : g_f13Raw;
        in.head = wantHead && src.valid;
        if (in.head) std::memcpy(in.C, src.C, sizeof(in.C));
    }
    const bool neutral = f12Near(s, 1.0f) && f12Near(size, 1.0f) && !in.head;
    if (h != g_f12Hrc) {
        g_f12Hrc = h;
        for (auto& m : g_f12Mat) m = F12Matrix{};
    }
    const uint32_t offs[2] = {nofoot::kF9HrcMatrixOff, nofoot::kHudMatrix2Off};
    for (size_t i = 0; i < 2; ++i) {
        float cur[16] = {};
        if (!guardedRead(h + offs[i], cur, sizeof(cur))) {
            f12Stand("a placement matrix could not be read");
            return;
        }
        F12Matrix& m = g_f12Mat[i];
        const bool ours = m.have && std::memcmp(cur, m.written, sizeof(cur)) == 0;
        if (ours && neutral) {
            // Back at the game's own placement: put the base back once, then stop claiming the matrix.
            if (!guardedWriteFloats16(h + offs[i], m.base)) {
                f12Stand("a placement matrix could not be written");
                return;
            }
            m.have = false;
            continue;
        }
        if (ours && f13SameInputs(m.in, in)) continue;   // already built from these inputs: the game has not recomputed
        if (!ours && neutral) continue;                    // the game's own values, and nothing to change
        if (!ours) std::memcpy(m.base, cur, sizeof(cur)); // a fresh recompute: these are the new base values
        float target[16] = {};
        f13Compose(m.base, in, target);
        if (!guardedWriteFloats16(h + offs[i], target)) {
            f12Stand("a placement matrix could not be written");
            return;
        }
        std::memcpy(m.written, target, sizeof(target));
        m.in = in;
        m.have = true;
    }
}

double f13YawDeg(const float* C) noexcept {
    return std::atan2(static_cast<double>(C[8]), static_cast<double>(C[10])) * 180.0 / 3.14159265358979323846;
}
double f13PitchDeg(const float* C) noexcept {
    double y = static_cast<double>(C[9]);
    if (y > 1.0) y = 1.0;
    if (y < -1.0) y = -1.0;
    return std::asin(y) * 180.0 / 3.14159265358979323846;
}

// Frame thread, every frame: smooths the head transform toward the raw one (alpha = 1 - exp(-dt / tau), tau = the setting in seconds; 0 =
// exact), and every 5 s while the head follow is on in native stereo, logs the head pose. Writes the stand-down once.
void f13FrameTick(uint64_t nowMs) {
    if (g_f12Stop.load(std::memory_order_relaxed) && !g_f12StopNoted) {
        g_f12StopNoted = true;
        const char* why = g_f12StopWhy.load(std::memory_order_relaxed);
        Log::get().note("native on foot hud placement: writes standing down -- %s", why ? why : "a fault");
    }
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double dt = 0.0;
    if (g_f13LastQpc != 0 && freq.QuadPart > 0) dt = static_cast<double>(now.QuadPart - static_cast<LONGLONG>(g_f13LastQpc)) / static_cast<double>(freq.QuadPart);
    g_f13LastQpc = static_cast<uint64_t>(now.QuadPart);
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;
    const int smoothMs = g_followSmoothMs.load(std::memory_order_relaxed);
    const double tau = smoothMs / 1000.0;
    const float alpha = tau <= 0.0 ? 1.0f : static_cast<float>(1.0 - std::exp(-dt / tau));
    F13Head raw, smooth;
    {
        F9Lock lock;
        raw = g_f13Raw;
        if (raw.valid) {
            if (!g_f13Smooth.valid) {
                g_f13Smooth = raw;
            } else {
                float qa[4], qb[4], qs[4];
                f13QuatFromRows(g_f13Smooth.C, qa);
                f13QuatFromRows(raw.C, qb);
                f13Slerp(qa, qb, alpha, qs);
                f13RowsFromQuat(qs, g_f13Smooth.C);
                for (size_t k = 12; k < 15; ++k) g_f13Smooth.C[k] += alpha * (raw.C[k] - g_f13Smooth.C[k]);
            }
        }
        smooth = g_f13Smooth;
    }
    if (!raw.valid || !smooth.valid) return;
    if (!g_followHead.load(std::memory_order_relaxed) || !nativeOnFootActive() || !g_meshCreateLive.load(std::memory_order_relaxed)) return;
    if (nowMs - g_f13LogMs < 5000 && g_f13LogMs != 0) return;
    g_f13LogMs = nowMs;
    Log::get().note("native on foot hud follow: head yaw=%.1f pitch=%.1f (smoothed yaw=%.1f pitch=%.1f) dist=%.3f",
                    f13YawDeg(raw.C), f13PitchDeg(raw.C), f13YawDeg(smooth.C), f13PitchDeg(smooth.C), static_cast<double>(g_hudDistance.load()));
}

// ---- F13: the visor override (TEMPORARY; the RemLok test; approved by Sean 2026-10-14) ------------------------------------------
// While native stereo is on foot, breathable air holds (fix.native_on_foot_breathable_visor is up, F16), and the identity check passes: both bytes must read 0; +0x5F741B8 is
// written 0 (the forced visor state, up) first, then +0x5F741B7 is written 1 (the override is active). The restore writes +0x5F741B7 back to
// 0 only while it still reads our 1; +0x5F741B8 stays 0. A refusal is logged once per reason (as the gate byte).
bool g_visorWritten = false;               // frame thread only: EDVR holds the override and owes the game a 0 back
std::string g_visorRefusalText;            // frame thread only
bool g_visorRestoreFailedNoted = false;    // frame thread only

void refuseVisor(const std::string& why) {
    if (why == g_visorRefusalText) return;
    g_visorRefusalText = why;
    Log::get().note("native on foot visor: override EliteDangerous64.exe+0x%llX NOT written -- %s. The game's visor is left as it is.",
                    static_cast<unsigned long long>(nofoot::kVisorOverrideRva), why.c_str());
}

// `want`: the override should be active now. `restoreReason` (a literal) says why it is being put back.
void visorTick(bool want, const char* restoreReason) {
    const uintptr_t a7 = g_base + nofoot::kVisorOverrideRva;
    const uintptr_t a8 = g_base + nofoot::kVisorDownRva;
    if (want) {
        if (g_visorWritten || g_install != InstallState::Armed) return;
        const char* idWhy = nullptr;
        if (!checkIdentity(g_base, &idWhy)) {
            refuseVisor(std::string("the PE identity check failed (") + idWhy + ")");
            return;
        }
        uint8_t c7 = 0, c8 = 0;
        if (!guardedRead(a7, &c7, 1) || !guardedRead(a8, &c8, 1)) {
            refuseVisor("the bytes could not be read");
            return;
        }
        if (c7 != 0 || c8 != 0) {
            char b[128];
            std::snprintf(b, sizeof(b), "the bytes read %u and %u, not 0 and 0, so they are someone else's and are left alone",
                          static_cast<unsigned>(c7), static_cast<unsigned>(c8));
            refuseVisor(b);
            return;
        }
        if (!writableByte(a7) || !writableByte(a8)) {
            refuseVisor("the bytes are not writable");
            return;
        }
        if (!guardedWriteByte(a8, 0) || !guardedWriteByte(a7, 1)) {
            refuseVisor("the write failed");
            return;
        }
        g_visorWritten = true;
        g_visorRefusalText.clear();
        Log::get().note("native on foot visor: override EliteDangerous64.exe+0x%llX 0 -> 1 (B8 = 0)",
                        static_cast<unsigned long long>(nofoot::kVisorOverrideRva));
        return;
    }
    if (!g_visorWritten) {
        g_visorRefusalText.clear();
        return;
    }
    uint8_t c7 = 0;
    if (!guardedRead(a7, &c7, 1)) return;   // retried next frame
    if (c7 != 1) {
        g_visorWritten = false;
        Log::get().note("native on foot visor: override EliteDangerous64.exe+0x%llX reads %u, not our 1, so it is left alone (restore: %s)",
                        static_cast<unsigned long long>(nofoot::kVisorOverrideRva), static_cast<unsigned>(c7), restoreReason);
        return;
    }
    if (!writableByte(a7) || !guardedWriteByte(a7, 0)) {
        if (!g_visorRestoreFailedNoted) {
            g_visorRestoreFailedNoted = true;
            Log::get().note("native on foot visor: override EliteDangerous64.exe+0x%llX could not be put back to 0 (%s); retrying each frame.",
                            static_cast<unsigned long long>(nofoot::kVisorOverrideRva), restoreReason);
        }
        return;
    }
    g_visorWritten = false;
    g_visorRestoreFailedNoted = false;
    Log::get().note("native on foot visor: override EliteDangerous64.exe+0x%llX 1 -> 0 (restored: %s)",
                    static_cast<unsigned long long>(nofoot::kVisorOverrideRva), restoreReason);
}

// ---- F14: the HUD spread (a user setting), the HUD layout probe (read only), and the reticle identification stub (TEMPORARY) --------------
// SPREAD. While native stereo is on foot, the creation patch is in, and spread > 0, the game's HUD margins at +0x5E7A754 (x, game 0.027) and
// +0x5E7A758 (y, game 0.06) are written 0.027 + spread and 0.06 + spread, clamped below 0.489. The game re-sends the HUD layout when the HRC's
// own copies differ from these globals, so nothing is forced. The current values must be the game's own or ours; otherwise nothing is written.
constexpr uintptr_t kMarginXRva = 0x5E7A754;
constexpr uintptr_t kMarginYRva = 0x5E7A758;
constexpr float kMarginXGame = 0.027f;
constexpr float kMarginYGame = 0.06f;
constexpr float kMarginCap = 0.489f;
bool g_spreadWritten = false;                 // frame thread only: we hold the margins at our values
float g_spreadWrittenX = 0.0f, g_spreadWrittenY = 0.0f;
std::string g_spreadRefusal;                  // frame thread only
bool g_spreadRestoreNoted = false;            // frame thread only

__declspec(noinline) bool guardedWriteFloat(uintptr_t address, float v) noexcept {
    __try {
        *reinterpret_cast<volatile float*>(address) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void refuseSpread(const std::string& why) {
    if (why == g_spreadRefusal) return;
    g_spreadRefusal = why;
    Log::get().note("native on foot hud spread: margins EliteDangerous64.exe+0x%llX, +0x%llX NOT written -- %s. The game's values are left as they are.",
                    static_cast<unsigned long long>(kMarginXRva), static_cast<unsigned long long>(kMarginYRva), why.c_str());
}

// `want`: the spread should be applied now (its value is `spread`). `reason` (a literal) says why it is being put back.
void spreadTick(bool want, float spread, const char* reason) {
    const uintptr_t ax = g_base + kMarginXRva;
    const uintptr_t ay = g_base + kMarginYRva;
    if (want) {
        const char* idWhy = nullptr;
        if (!checkIdentity(g_base, &idWhy)) {
            refuseSpread(std::string("the PE identity check failed (") + idWhy + ")");
            return;
        }
        float cx = 0.0f, cy = 0.0f;
        if (!guardedRead(ax, &cx, 4) || !guardedRead(ay, &cy, 4)) {
            refuseSpread("the margins could not be read");
            return;
        }
        const bool okX = f12Near(cx, kMarginXGame) || (g_spreadWritten && f12Near(cx, g_spreadWrittenX));
        const bool okY = f12Near(cy, kMarginYGame) || (g_spreadWritten && f12Near(cy, g_spreadWrittenY));
        if (!okX || !okY) {
            char b[160];
            std::snprintf(b, sizeof(b), "the margins read %.4f,%.4f, not the game's 0.027,0.060 or our last values, so they are left alone",
                          static_cast<double>(cx), static_cast<double>(cy));
            refuseSpread(b);
            return;
        }
        if (!writableByte(ax) || !writableByte(ay)) {
            refuseSpread("the margins are not writable");
            return;
        }
        float nx = kMarginXGame + spread;
        float ny = kMarginYGame + spread;
        if (nx > kMarginCap) nx = kMarginCap;
        if (ny > kMarginCap) ny = kMarginCap;
        if (g_spreadWritten && f12Near(nx, g_spreadWrittenX) && f12Near(ny, g_spreadWrittenY)) return;   // already there
        if (!guardedWriteFloat(ax, nx) || !guardedWriteFloat(ay, ny)) {
            refuseSpread("the write failed");
            return;
        }
        g_spreadWritten = true;
        g_spreadWrittenX = nx;
        g_spreadWrittenY = ny;
        g_spreadRefusal.clear();
        Log::get().note("native on foot hud spread: margins 0.027,0.060 -> %.3f,%.3f", static_cast<double>(nx), static_cast<double>(ny));
        return;
    }
    if (!g_spreadWritten) {
        g_spreadRefusal.clear();
        return;
    }
    float cx = 0.0f, cy = 0.0f;
    if (!guardedRead(ax, &cx, 4) || !guardedRead(ay, &cy, 4)) return;   // retried next frame
    if (!f12Near(cx, g_spreadWrittenX) || !f12Near(cy, g_spreadWrittenY)) {
        g_spreadWritten = false;
        Log::get().note("native on foot hud spread: margins NOT restored -- they read %.4f,%.4f, not ours (restore: %s)",
                        static_cast<double>(cx), static_cast<double>(cy), reason);
        return;
    }
    if (!writableByte(ax) || !writableByte(ay) || !guardedWriteFloat(ax, kMarginXGame) || !guardedWriteFloat(ay, kMarginYGame)) {
        if (!g_spreadRestoreNoted) {
            g_spreadRestoreNoted = true;
            Log::get().note("native on foot hud spread: margins could not be restored (%s); retrying each frame.", reason);
        }
        return;
    }
    g_spreadWritten = false;
    g_spreadRestoreNoted = false;
    Log::get().note("native on foot hud spread: margins -> 0.027,0.060 (restored: %s)", reason);
}

// ---- F17: the HUD element scale (a user setting, live) ----------------------------------------------------------------------------------
// RE (the layout routine 0x1AB32F0, branch at 0x1AB3351): when the byte at +0x5F741CA is non-zero, the layout scale S is the float at +0x5E7A760
// (1.0 in the file); when it is zero, S = min(1, aspect / 1.7222). S multiplies every part's scale and its spread around the centre. While the setting
// is 0.30..2.00 (0 leaves the game's own scale: nothing is written), native stereo is on foot and the HUD mesh patch is in, the float is written k
// first and the byte 1 second; the layout re-send (layoutResendTick) then applies it. Put back (the byte 0, then the float 1.0) when the setting goes
// to 0 or the wanted state ends. The values must be the game's own (0 and 1.0) or ours; otherwise nothing is written.
constexpr float kElemGame = 1.0f;
bool g_elemWritten = false;             // frame thread only: we hold the byte at 1 and the float at g_elemWrittenK
float g_elemWrittenK = 0.0f;            // frame thread only
std::string g_elemRefusal;              // frame thread only
bool g_elemRestoreFailedNoted = false;  // frame thread only

void refuseElem(const std::string& why) {
    if (why == g_elemRefusal) return;
    g_elemRefusal = why;
    Log::get().note("native on foot hud element scale: EliteDangerous64.exe+0x%llX and +0x%llX NOT written -- %s. The game's layout is left as it is.",
                    static_cast<unsigned long long>(nofoot::kElemScaleModeRva), static_cast<unsigned long long>(nofoot::kElemScaleRva), why.c_str());
}

// `want`: the layout scale should be k now (0.30..2.00). `reason` (a literal) says why it is being put back.
void elemScaleTick(bool want, float k, const char* reason) {
    const uintptr_t am = g_base + nofoot::kElemScaleModeRva;
    const uintptr_t af = g_base + nofoot::kElemScaleRva;
    if (want) {
        const char* idWhy = nullptr;
        if (!checkIdentity(g_base, &idWhy)) {
            refuseElem(std::string("the PE identity check failed (") + idWhy + ")");
            return;
        }
        uint8_t cm = 0;
        float cf = 0.0f;
        if (!guardedRead(am, &cm, 1) || !guardedRead(af, &cf, 4)) {
            refuseElem("the values could not be read");
            return;
        }
        const bool modeOk = cm == 0 || (g_elemWritten && cm == 1);
        const bool scaleOk = f12Near(cf, kElemGame) || (g_elemWritten && f12Near(cf, g_elemWrittenK));
        if (!modeOk || !scaleOk) {
            char b[200];
            std::snprintf(b, sizeof(b), "the byte reads %u and the float %.4f, not the game's 0 and 1.0 or our own, so they are left alone",
                          static_cast<unsigned>(cm), static_cast<double>(cf));
            refuseElem(b);
            return;
        }
        if (g_elemWritten && cm == 1 && f12Near(cf, k)) return;   // already there
        if (!writableByte(am) || !writableByte(af)) {
            refuseElem("the values are not writable");
            return;
        }
        if (!guardedWriteFloat(af, k)) {   // the float first: the byte then turns the layout onto it
            refuseElem("the write failed");
            return;
        }
        if (!guardedWriteByte(am, 1)) {
            guardedWriteFloat(af, kElemGame);   // the byte did not take: the float goes back to the game's own
            refuseElem("the write failed");
            return;
        }
        const bool first = !g_elemWritten;
        const float was = g_elemWrittenK;
        g_elemWritten = true;
        g_elemWrittenK = k;
        g_elemRefusal.clear();
        if (first) Log::get().note("native on foot hud element scale: %.2f (override 0 -> 1)", static_cast<double>(k));
        else Log::get().note("native on foot hud element scale: %.2f (was %.2f)", static_cast<double>(k), static_cast<double>(was));
        return;
    }
    if (!g_elemWritten) {
        g_elemRefusal.clear();
        return;
    }
    uint8_t cm = 0;
    float cf = 0.0f;
    if (!guardedRead(am, &cm, 1) || !guardedRead(af, &cf, 4)) return;   // retried next frame
    const bool modeOurs = cm == 1;
    const bool scaleOurs = f12Near(cf, g_elemWrittenK);
    const bool partial = cm == 0 && scaleOurs;   // a restore that stopped after the byte: the float is still ours
    if (!partial && (!modeOurs || !scaleOurs)) {
        g_elemWritten = false;
        Log::get().note("native on foot hud element scale: NOT restored -- the byte reads %u and the float %.4f, not ours, so they are left alone (restore: %s)",
                        static_cast<unsigned>(cm), static_cast<double>(cf), reason);
        return;
    }
    if (!writableByte(am) || !writableByte(af) || (modeOurs && !guardedWriteByte(am, 0)) || !guardedWriteFloat(af, kElemGame)) {
        if (!g_elemRestoreFailedNoted) {
            g_elemRestoreFailedNoted = true;
            Log::get().note("native on foot hud element scale: could not be put back to the game's (%s); retrying each frame.", reason);
        }
        return;
    }
    g_elemWritten = false;
    g_elemRestoreFailedNoted = false;
    Log::get().note("native on foot hud element scale: restored to the game's own (1.0, byte 0; %s)", reason);
}

// LAYOUT HOOK (F14 probe; F16 writer). EliteDangerous64.exe+0x1A0DCD0 (args rcx = HRC, rdx = float out[2], r8 = &rec; rec+0 = const char* part name), called
// once per HUD part per layout re-send. The prologue is verified (the first five bytes are mov [rsp+0x20],rbx: no rip-relative operand, no
// branch). The original runs first; then the fields are read under SEH and one line is queued for the frame thread. Six arguments are forwarded.
struct F14Line {
    char text[240];
};
constexpr size_t kF14Ring = 32;
F14Line g_f14Ring[kF14Ring];                  // guarded by g_f9Lock
uint64_t g_f14Written = 0, g_f14Read = 0, g_f14Dropped = 0;   // guarded by g_f9Lock
std::atomic<uint64_t> g_f14Calls{0};          // the layout hook's calls in this session
F9Probe g_f14Layout;                          // the layout probe's relay, gate, trampoline and CodeHook
std::atomic<bool> g_f14LayoutStop{false};
bool g_f14LayoutArmed = false;                // frame thread only
bool g_f14LayoutLogged = false;               // frame thread only

void f14Queue(const char* text) noexcept {
    F9Lock lock;
    if (g_f14Written - g_f14Read >= kF14Ring) {
        ++g_f14Dropped;
        return;
    }
    std::snprintf(g_f14Ring[g_f14Written % kF14Ring].text, sizeof(g_f14Ring[0].text), "%s", text);
    ++g_f14Written;
}

// F16: the layout lines are logged on change, per part name (under g_f9Lock). 0 = nothing new; 1 = this part's layout changed or is new; 2 = a name the
// group table does not know, logged once with its consequence (no offset).
struct F16SeenPart {
    char name[40];
    float out0, out1;
    bool used, unknownNoted;
};
constexpr size_t kF16Seen = 32;
F16SeenPart g_f16Seen[kF16Seen];   // guarded by g_f9Lock

bool sameBits(float a, float b) noexcept {
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

int noteSeenPart(const char* name, float out0, float out1, int group) noexcept {
    F9Lock lock;
    F16SeenPart* row = nullptr;
    for (auto& r : g_f16Seen) {
        if (r.used && std::strcmp(r.name, name) == 0) {
            row = &r;
            break;
        }
    }
    int result = 0;
    if (!row) {
        for (auto& r : g_f16Seen) {
            if (!r.used) {
                row = &r;
                break;
            }
        }
        if (!row) return 0;
        row->used = true;
        std::snprintf(row->name, sizeof(row->name), "%s", name);
        row->unknownNoted = false;
        row->out0 = out0;
        row->out1 = out1;
        result = 1;
    } else if (!sameBits(row->out0, out0) || !sameBits(row->out1, out1)) {
        row->out0 = out0;
        row->out1 = out1;
        result = 1;
    }
    if (group < 0 && !row->unknownNoted) {
        row->unknownNoted = true;
        result = 2;
    }
    return result;
}

// The part's name (rec+0 is a const char*), up to cap-1 characters; "?" when unreadable.
void readPartName(uintptr_t rec, char* name, size_t cap) noexcept {
    std::snprintf(name, cap, "?");
    uint64_t np = 0;
    if (!guardedRead(rec, &np, 8) || np < 0x10000) return;
    size_t i = 0;
    for (; i + 1 < cap; ++i) {
        char c = 0;
        if (!guardedRead(static_cast<uintptr_t>(np) + i, &c, 1)) break;
        name[i] = (c >= 32 && c < 127) ? c : '.';
        if (c == 0) break;
    }
    name[i] = 0;
    if (i == 0) std::snprintf(name, cap, "?");
}

void f14LogPart(void* hrc, void* out, void* rec, int group) noexcept {
    const uint64_t n = g_f14Calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const uintptr_t h = reinterpret_cast<uintptr_t>(hrc);
    const uintptr_t r = reinterpret_cast<uintptr_t>(rec);
    const uintptr_t o = reinterpret_cast<uintptr_t>(out);
    const float nanv = std::nanf("");
    // The name: copied up to 64 characters from *(const char**)rec; "?" when unreadable.
    char name[72] = "?";
    uint64_t np = 0;
    if (guardedRead(r, &np, 8) && np >= 0x10000) {
        size_t i = 0;
        for (; i < 64; ++i) {
            char c = 0;
            if (!guardedRead(static_cast<uintptr_t>(np) + i, &c, 1)) break;
            name[i] = (c >= 32 && c < 127) ? c : '.';
            if (c == 0) break;
        }
        name[i] = 0;
        if (i == 0) std::snprintf(name, sizeof(name), "?");
    }
    float rec8 = nanv, recC = nanv, rec10 = nanv, rec14 = nanv, rec1C = nanv;
    guardedRead(r + 0x8, &rec8, 4);
    guardedRead(r + 0xC, &recC, 4);
    guardedRead(r + 0x10, &rec10, 4);
    guardedRead(r + 0x14, &rec14, 4);
    guardedRead(r + 0x1C, &rec1C, 4);
    uint8_t b18 = 0, b19 = 0;
    const bool okB = guardedRead(r + 0x18, &b18, 1) && guardedRead(r + 0x19, &b19, 1);
    float hrc4C4 = nanv, hrc4C8 = nanv, hrc4CC = nanv;
    guardedRead(h + 0x4C4, &hrc4C4, 4);
    guardedRead(h + 0x4C8, &hrc4C8, 4);
    guardedRead(h + 0x4CC, &hrc4CC, 4);
    float canvasW = nanv, canvasH = nanv;
    uint64_t canvas = 0;
    if (guardedRead(h + 0x1B8, &canvas, 8) && canvas >= 0x10000) {
        guardedRead(static_cast<uintptr_t>(canvas) + 0x3E8, &canvasW, 4);
        guardedRead(static_cast<uintptr_t>(canvas) + 0x3EC, &canvasH, 4);
    }
    float out0 = nanv, out1 = nanv;
    guardedRead(o, &out0, 4);
    guardedRead(o + 4, &out1, 4);
    // F16: one line per change of a part's layout (the values after any group offset), and once for a name the group table does not know.
    const int seen = noteSeenPart(name, out0, out1, group);
    if (seen == 0) return;
    const char* groupNote = seen == 2 ? " (not in the HUD group table: no offset)" : "";
    char line[240];
    std::snprintf(line, sizeof(line),
                  "native on foot hud part: name=%s anchor=(%.3f,%.3f) off=(%.3f,%.3f) align=(%u,%u) scale=%.3f out=(%.3f,%.3f) canvas=%.0fx%.0f "
                  "hrc=(%.3f,%.3f,%.3f) calls=%llu%s",
                  name, static_cast<double>(rec8), static_cast<double>(recC), static_cast<double>(rec10), static_cast<double>(rec14),
                  okB ? static_cast<unsigned>(b18) : 0u, okB ? static_cast<unsigned>(b19) : 0u, static_cast<double>(rec1C),
                  static_cast<double>(out0), static_cast<double>(out1), static_cast<double>(canvasW), static_cast<double>(canvasH),
                  static_cast<double>(hrc4C4), static_cast<double>(hrc4C8), static_cast<double>(hrc4CC), static_cast<unsigned long long>(n), groupNote);
    f14Queue(line);
}

// F16: the group's offset (fraction of the canvas, x and y), added to the layout's own value after the original returns, while native stereo is on
// foot. The canvas is [[HRC+0x1B8]+0x3E8] by [+0x3EC]; the offset is applied to out[0] and out[1] only, and each call starts from the game's value,
// so nothing compounds. The HRC is kept for the layout re-send (layoutResendTick).
void applyHudOffset(uintptr_t h, uintptr_t o, int group) noexcept {
    float offX = 0.0f, offY = 0.0f;
    bool on = false;
    {
        F9Lock lock;
        g_layoutHrc = h;
        on = g_hudOffsetOn;
        if (group >= 0) {
            offX = g_hudOffsetRead[group * 2];
            offY = g_hudOffsetRead[group * 2 + 1];
        }
    }
    if (!on || group < 0 || !o || (offX == 0.0f && offY == 0.0f)) return;
    uint64_t canvas = 0;
    float cw = 0.0f, ch = 0.0f;
    if (!guardedRead(h + 0x1B8, &canvas, 8) || canvas < 0x10000) return;
    if (!guardedRead(static_cast<uintptr_t>(canvas) + 0x3E8, &cw, 4) || !guardedRead(static_cast<uintptr_t>(canvas) + 0x3EC, &ch, 4)) return;
    if (!(cw > 0.0f && ch > 0.0f && cw < 1.0e5f && ch < 1.0e5f)) return;
    float v0 = 0.0f, v1 = 0.0f;
    if (!guardedRead(o, &v0, 4) || !guardedRead(o + 4, &v1, 4)) return;
    guardedWriteFloat(o, v0 + offX * cw);
    guardedWriteFloat(o + 4, v1 + offY * ch);
}

uint64_t __fastcall layoutHooked(void* hrc, void* out, void* rec, uint64_t a4, uint64_t a5, uint64_t a6) {
    const F9Fn6 original = reinterpret_cast<F9Fn6>(g_f14Layout.original);
    if (!original) return 0;
    const uint64_t rax = original(hrc, out, reinterpret_cast<uint64_t>(rec), a4, a5, a6);
    char name[72] = "?";
    readPartName(reinterpret_cast<uintptr_t>(rec), name, sizeof(name));
    const int group = nofoot::hudGroupOf(name);
    applyHudOffset(reinterpret_cast<uintptr_t>(hrc), reinterpret_cast<uintptr_t>(out), group);
    if (!g_f14LayoutStop.load(std::memory_order_relaxed)) f14LogPart(hrc, out, rec, group);
    return rax;
}

// RETICLE INDICATOR PROBE (read only, except key 10 while native stereo is on foot, F16-F18). The stub below calls this with rcx = the instance and rdx = the node of the marker component's call at
// EliteDangerous64.exe+0x1A15644 (the call to 0x434ECC0, preceded by mov rcx,[rsi+0x60]). It writes nothing except key 10: reticle follows aim (F18, default) zeroes its translation x and y; head (F17) places it along the head.

struct F14Key {
    bool used = false;
    uint32_t key = 0;
    uint64_t n = 0;                 // calls in the window
    uint8_t f30 = 0, f31 = 0, f32 = 0;
    float fl34 = 0.0f, fl38 = 0.0f;  // the node's floats at +0x34 and +0x38 (last seen)
    float scale = 0.0f;             // the instance scale at +0x50 (last seen)
    float pos[3] = {};              // the instance's translation row at +0x40..+0x48 (last seen)
    bool posOk = false;
};
constexpr size_t kF14Keys = 16;
F14Key g_f14Keys[kF14Keys];         // guarded by g_f9Lock

// ---- F17: the reticle follows head (key 10 only; game thread, from the probe) --------------------------------------------------------
// The aim reticle is an instance matrix in the body camera space: basis rows 0-2 at +0x10, +0x20, +0x30 (xyz each; w is not written) and the
// translation at +0x40..+0x48. While `want`, with C = the smoothed head transform the HUD follows (g_f13Smooth, under g_f9Lock):
//   translation t' = d * row2(C) + row3(C), where d = |t| is the game's own distance (the point (0, 0, d) of head space, taken into body space);
//   each basis row b' = b . R(C), R the rotation of C.
// rec.base keeps the game's own values. A call that finds our values in place (the game has not recomputed the instance) composes from that base
// again, so nothing compounds. Without a head transform, or once `want` ends, the game's own values are put back.
struct F17Reticle {
    bool have = false;          // our values are in place in the instance at `inst`
    uintptr_t inst = 0;
    float base[12] = {};        // the game's values before our last write: basis rows 0-2 (xyz each), then the translation (xyz)
    float written[12] = {};     // what we last wrote, in the same order
};
F17Reticle g_f17Ret;                // game thread only
bool g_f17WriteFailNoted = false;   // game thread only

// Value k (0..11) of the instance: matrix rows 0-3 start at +0x10 and are 0x10 apart; the w of each row is not one of the twelve.
inline uintptr_t f17Addr(uintptr_t ia, size_t k) noexcept { return ia + 0x10 + (k / 3) * 0x10 + (k % 3) * 4; }

bool f17WriteAll(uintptr_t ia, const float* v) noexcept {
    for (size_t k = 0; k < 12; ++k)
        if (!guardedWriteFloat(f17Addr(ia, k), v[k])) return false;
    return true;
}

void reticleFollowHead(uintptr_t ia, bool want, bool announce) noexcept {
    F17Reticle& rec = g_f17Ret;
    float cur[12] = {};
    for (size_t k = 0; k < 12; ++k)
        if (!guardedRead(f17Addr(ia, k), &cur[k], 4)) return;
    const bool ours = rec.have && rec.inst == ia && std::memcmp(cur, rec.written, sizeof(cur)) == 0;
    if (!ours) {
        // The game's own values (its first call, or a fresh recompute of the instance): these are the base.
        std::memcpy(rec.base, cur, sizeof(cur));
        rec.have = false;
        rec.inst = ia;
    }
    float C[16] = {};
    bool pose = false;
    if (want) {
        F9Lock lock;
        if (g_followHead.load(std::memory_order_relaxed) && g_f13Smooth.valid) {
            std::memcpy(C, g_f13Smooth.C, sizeof(C));
            pose = true;
        }
    }
    if (!pose) {
        if (ours) {   // back to the game's own values, once
            if (!f17WriteAll(ia, rec.base) && !g_f17WriteFailNoted) {
                g_f17WriteFailNoted = true;
                f14Queue("native on foot reticle: the game's values could not be put back; key 10 may stay where it was placed");
            }
            rec.have = false;
        }
        if (want && announce) f14Queue("native on foot reticle: no head pose yet (the HUD follow is body, or the head views are not read); key 10 stays at the game's aim");
        return;
    }
    const float* rows[3] = {rec.base, rec.base + 3, rec.base + 6};
    const float* tb = rec.base + 9;
    const float d = std::sqrt(tb[0] * tb[0] + tb[1] * tb[1] + tb[2] * tb[2]);
    float target[12] = {};
    for (size_t r = 0; r < 3; ++r)
        for (size_t j = 0; j < 3; ++j) target[3 * r + j] = rows[r][0] * C[j] + rows[r][1] * C[4 + j] + rows[r][2] * C[8 + j];
    for (size_t j = 0; j < 3; ++j) target[9 + j] = d * C[8 + j] + C[12 + j];
    if (!f17WriteAll(ia, target)) {
        rec.have = false;
        if (!g_f17WriteFailNoted) {
            g_f17WriteFailNoted = true;
            f14Queue("native on foot reticle: NOT written -- the instance could not be written; key 10 stays at the game's aim");
        }
        return;
    }
    std::memcpy(rec.written, target, sizeof(target));
    rec.have = true;
    rec.inst = ia;
    if (announce) {
        char line[160];
        std::snprintf(line, sizeof(line), "native on foot reticle: follows head (placed along the head direction at d=%.2fm)", static_cast<double>(d));
        f14Queue(line);
    }
}

// F18: reticle follows aim (the default, key 10 only): the translation's x and y are zeroed, so the dot sits on the mouse-aim axis. That is the
// F16 centring, which the head mode used to be; z, w, the basis and the scale stay. Written only while g_reticleWriteOn (frame thread). Guarded.
bool g_f18AimFailNoted = false;     // game thread only
void reticleAimCentre(uintptr_t ia, bool announce) noexcept {
    const bool ok = guardedWriteFloat(ia + 0x40, 0.0f) && guardedWriteFloat(ia + 0x44, 0.0f);
    if (!ok && !g_f18AimFailNoted) {
        g_f18AimFailNoted = true;
        f14Queue("native on foot reticle: NOT centred -- the instance could not be written; key 10 stays at the game's aim");
    }
    if (ok && announce) f14Queue("native on foot reticle: follows aim (key 10 centred on the aim axis: x and y zeroed)");
}

void __fastcall reticleIndicatorProbe(void* instance, void* node) noexcept {
    const uintptr_t ia = reinterpret_cast<uintptr_t>(instance);
    const uintptr_t na = reinterpret_cast<uintptr_t>(node);
    uint32_t key = 0;
    uint8_t b30 = 0, b31 = 0, b32 = 0;
    float fl34 = 0.0f, fl38 = 0.0f, scale = 0.0f;
    float pos[3] = {};
    const bool okKey = guardedRead(na + 0x20, &key, 4);
    const bool okB = guardedRead(na + 0x30, &b30, 1) && guardedRead(na + 0x31, &b31, 1) && guardedRead(na + 0x32, &b32, 1);
    guardedRead(na + 0x34, &fl34, 4);
    guardedRead(na + 0x38, &fl38, 4);
    guardedRead(ia + 0x50, &scale, 4);
    const bool okPos = guardedRead(ia + 0x40, &pos[0], 4) && guardedRead(ia + 0x44, &pos[1], 4) && guardedRead(ia + 0x48, &pos[2], 4);
    if (!okKey || !okB) return;
    // Key 10 (the aim reticle), while g_reticleWriteOn (the frame thread's flag). F18: reticle follows head (F17) places it along the head; aim
    // (the default) zeroes its x and y. The head placement is handed back first when the mode is aim, so a switch leaves no residue.
    if (key == 10) {
        const bool writeOn = g_reticleWriteOn.load(std::memory_order_relaxed);
        const bool announce = g_reticleLogWanted.exchange(false, std::memory_order_relaxed);
        if (g_reticleGame.load(std::memory_order_relaxed)) {
            // F21: the game places key 10. A head placement made before the switch is handed back once (a no-op when there is none); nothing else is written.
            reticleFollowHead(ia, false, false);
            if (writeOn && announce) f14Queue("native on foot reticle: the game places key 10 (EDVR writes nothing to it)");
        } else if (g_reticleHead.load(std::memory_order_relaxed)) {
            reticleFollowHead(ia, writeOn, announce);
        } else {
            reticleFollowHead(ia, false, false);
            if (writeOn) reticleAimCentre(ia, announce);
        }
    }
    F9Lock lock;
    F14Key* row = nullptr;
    for (auto& r : g_f14Keys) {
        if (r.used && r.key == key) {
            row = &r;
            break;
        }
    }
    if (!row) {
        for (auto& r : g_f14Keys) {
            if (!r.used) {
                row = &r;
                break;
            }
        }
        if (!row) return;
        row->used = true;
        row->key = key;
    }
    ++row->n;
    row->f30 = b30;
    row->f31 = b31;
    row->f32 = b32;
    row->fl34 = fl34;
    row->fl38 = fl38;
    row->scale = scale;
    row->posOk = okPos;
    if (okPos) std::memcpy(row->pos, pos, sizeof(pos));
}

// F31: 4x4 helpers for the "now" head. The runtime's transforms are 3x4 row-major column-vector rigid matrices (HmdMatrix34_t: translation in
// column 3). The list-B view C is row-vector (p' = p . C, translation in row 3), so H_M = C . (N^-1 . R)^T: the view under the "now" head from
// the view under the render head. The body transform cancels in N^-1 . R, and with N = R, H_M = C exactly (the check the flight reads).
void f31Rigid(const float m[12], double out[4][4]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) out[i][j] = static_cast<double>(m[4 * i + j]);
    out[3][0] = out[3][1] = out[3][2] = 0.0;
    out[3][3] = 1.0;
}
void f31Mul(const double a[4][4], const double b[4][4], double out[4][4]) {
    double r[4][4];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double acc = 0.0;
            for (int k = 0; k < 4; ++k) acc += a[i][k] * b[k][j];
            r[i][j] = acc;
        }
    std::memcpy(out, r, sizeof(r));
}
void f31RigidInverse(const double m[4][4], double out[4][4]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[i][j] = m[j][i];
        out[i][3] = -(m[0][i] * m[0][3] + m[1][i] * m[1][3] + m[2][i] * m[2][3]);
    }
    out[3][0] = out[3][1] = out[3][2] = 0.0;
    out[3][3] = 1.0;
}
// H = C . (N^-1 . R)^T: C (the view under the render head R) carried to the "now" head N.
void f31NowMatrix(const float C[16], const float render[12], const float now[12], float H[16]) {
    double R[4][4], N[4][4], Ninv[4][4], Dinv[4][4], DinvT[4][4], Cd[4][4], Hd[4][4];
    f31Rigid(render, R);
    f31Rigid(now, N);
    f31RigidInverse(N, Ninv);
    f31Mul(Ninv, R, Dinv);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            DinvT[i][j] = Dinv[j][i];
            Cd[i][j] = static_cast<double>(C[4 * i + j]);
        }
    f31Mul(Cd, DinvT, Hd);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) H[4 * i + j] = static_cast<float>(Hd[i][j]);
}
// The angle between two transforms' rotations, in degrees: arccos((trace(Ra^T Rb) - 1) / 2).
double f31AngleDeg(const float a[16], const float b[16]) {
    double tr = 0.0;
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) tr += static_cast<double>(a[4 * k + i]) * static_cast<double>(b[4 * k + i]);
    double c = (tr - 1.0) * 0.5;
    if (c > 1.0) c = 1.0;
    if (c < -1.0) c = -1.0;
    return std::acos(c) * 180.0 / 3.14159265358979323846;
}
// The forward map of the world markers: t' = t . R + row3 (the reticle head mode's map), for a given C.
void f31ForwardMap(const float t[3], const float C[16], float out[3]) {
    for (int j = 0; j < 3; ++j) {
        float acc = 0.0f;
        for (int i = 0; i < 3; ++i) acc += t[i] * C[4 * i + j];
        out[j] = acc + C[12 + j];
    }
}

// ---- F37: the marker's camera M and the ring of render heads (waypoint lag, step 3). M is the game's own getter chain (the F24 probe 3's): P1 =
// [marker+0x1B0], x = P1's getter at vtable +0x58, M = x's getter at vtable +0x28. Every getter is the game's, called with rcx only, under SEH.
using F37GetterFn = float* (__fastcall*)(void*);
__declspec(noinline) bool f37CallGetter(uintptr_t obj, uint32_t slot, uintptr_t* out) noexcept {
    uintptr_t vt = 0, fn = 0;
    if (!obj || !guardedRead(obj, &vt, 8) || !vt || !guardedRead(vt + slot, &fn, 8) || !fn) return false;
    __try {
        const F37GetterFn f = reinterpret_cast<F37GetterFn>(fn);
        *out = reinterpret_cast<uintptr_t>(f(reinterpret_cast<void*>(obj)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Rows 0-2 of a 4x4 row-vector matrix: unit length and mutually orthogonal (within 0.02), else refused.
bool f37RotOrthonormal(const float* m) noexcept {
    for (int i = 0; i < 3; ++i) {
        const float n = m[4 * i] * m[4 * i] + m[4 * i + 1] * m[4 * i + 1] + m[4 * i + 2] * m[4 * i + 2];
        if (!(n > 0.98f && n < 1.02f)) return false;
    }
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b) {
            const float d = m[4 * a] * m[4 * b] + m[4 * a + 1] * m[4 * b + 1] + m[4 * a + 2] * m[4 * b + 2];
            if (!(d > -0.02f && d < 0.02f)) return false;
        }
    return true;
}
bool f37ReadM(uintptr_t marker, float out[16]) noexcept {
    uintptr_t p1 = 0, x = 0, mm = 0;
    if (!guardedRead(marker + 0x1B0, &p1, 8) || !p1) return false;
    if (!f37CallGetter(p1, 0x58, &x) || !x) return false;
    if (!f37CallGetter(x, 0x28, &mm) || !mm) return false;
    if (!guardedRead(mm, out, 16 * sizeof(float))) return false;
    return f37RotOrthonormal(out);
}
// The view under the ring's pose k back from the newest (0 = the newest), by the same mapping as H_M: f31NowMatrix(C, render, ring[k]).
void f37RingView(const float C[16], const EdvrNativeNowPose& rp, uint32_t k, float H[16]) noexcept {
    const uint32_t idx = (rp.ringNewest + 8u - k) % 8u;
    f31NowMatrix(C, rp.render, rp.ringHead[idx], H);
}
// F25 (TEMPORARY): the post-call of the marker stub, for every marker except key 10 (the reticle, which keeps its own placement). The game's call
// writes the instance's translation t (+0x40) from the marker's world position, which already includes the head (the probe-3 RE: its yaw is -C's).
// The list-B pipeline then applies C again, so the marker is head-locked. Writing t' = (t - row3(C)) . R^T, the exact inverse of that forward map
// (p . C for a row-vector C: the same map the reticle head mode writes, t' = d*row2 + row3), cancels the second application. Read the node's key
// and its instance (node+0x60) after the real call; nothing else is written.
void __fastcall reticleWorldPost(void* node) noexcept {
    const uintptr_t na = reinterpret_cast<uintptr_t>(node);
    uintptr_t ia = 0;
    uint32_t key = 0;
    if (!na || !guardedRead(na + 0x60, &ia, 8) || !ia || !guardedRead(na + 0x20, &key, 4)) return;
    if (key == 10) return;
    const int wmMode = g_worldMarkersMode.load(std::memory_order_relaxed);
    if (wmMode == 2 || !nativeOnFootActive()) return;
    float C[16] = {};
    bool havePose = false;
    const uint64_t now = GetTickCount64();
    const F30SameCall& sc = t_f30SameCall;
    bool sameCall = false;
    uint64_t usedSeq = 0, usedQpc = 0;
    if (sc.valid) {
        // F30 part 2: this marker call's own list-B head (the pre-hook read it on this thread, in this call): the same-frame pose.
        std::memcpy(C, sc.head.C, sizeof(C));
        havePose = true;
        sameCall = true;
        usedSeq = sc.seq;
        usedQpc = sc.qpc;
    } else {
        // Only when the call had no head read: the P2 sample (g_worldC) while it is fresh. (F30: the smoothed head is no longer used here.)
        F9Lock lock;
        if (g_worldC.valid && now - g_worldCMs < 500) {
            std::memcpy(C, g_worldC.C, sizeof(C));
            havePose = true;
            usedSeq = g_worldCSeq;
            usedQpc = g_worldCQpc;
        }
    }
    if (havePose) {
        // F30: the sample used against the marker entry's (zero for the same-call read), and the sample's age in microseconds.
        const uint64_t nowQ = f30Qpc();
        const uint64_t age = (usedQpc && nowQ >= usedQpc) ? f30QpcUs(nowQ - usedQpc) : 0;
        const int64_t lag = sameCall ? 0 : static_cast<int64_t>(usedSeq) - static_cast<int64_t>(sc.entrySeq);
        if (sameCall) ++g_f30PostSame; else ++g_f30PostFall;
        const int64_t absLag = lag < 0 ? -lag : lag;
        if (absLag > g_f30LagMax) g_f30LagMax = absLag;
        if (age > g_f30AgeMaxUs) g_f30AgeMaxUs = age;
        g_f30AgeSumUs += age;
        ++g_f30AgeN;
    } else {
        ++g_f30PostNone;
    }
    // F31/F32: the same-call C is kept for the comparison. The runtime's "now" head is read at every post, in every mode (F32: the flight never
    // read it under forward, which only read it in forward_now); H_M is formed and logged; forward_now (mode 3) uses it in place of C.
    if (havePose) std::memcpy(g_f31SameC, C, sizeof(g_f31SameC));
    g_f31HaveNow = false;
    if (havePose) {
        EdvrNativeNowPose np{};
        const bool haveFrame = nativeFrameNowPose(np);
        g_f31LastInputVersion = haveFrame ? np.inputVersion : 0;
        g_f31LastNowValid = haveFrame && np.nowValid;
        if (haveFrame && !g_f31FirstRead) {
            g_f31FirstRead = true;
            Log::get().note("now head: first read on the d3d11 side at a marker post: frame input version %u, renderValid %d, nowValid %d, answer #%u, "
                            "geometry #%llu, world markers mode %d", np.inputVersion, static_cast<int>(np.renderValid), static_cast<int>(np.nowValid),
                            static_cast<unsigned>(np.nowSequence), static_cast<unsigned long long>(np.frameSequence), wmMode);
        }
        if (haveFrame && np.renderValid && np.nowValid) {
            if (!g_f31FirstValid) {
                g_f31FirstValid = true;
                Log::get().note("now head: first valid now head in use (answer #%u; mode %d: %s)", static_cast<unsigned>(np.nowSequence), wmMode,
                                wmMode == 3 ? "it replaces C in the marker transform" : "it is logged against C only");
            }
            f31NowMatrix(C, np.render, np.now, g_f31NowH);
            g_f31HaveNow = true;
            g_f31NowSeq = np.nowSequence;
            g_f31FrameSeq = np.frameSequence;
            g_f31AngleDeg = f31AngleDeg(g_f31SameC, g_f31NowH);
            if (wmMode == 3) std::memcpy(C, g_f31NowH, sizeof(C));
        } else {
            ++g_f31NoNow;
        }
    }
    // F37 (waypoint step 3): forward_hist (4) takes the ring pose whose view best matches M's rotation (the k back from the newest, 0..7); forward_k2
    // (5), and forward_hist when M cannot be read, take the pose two frames back (k = 2). The view under the pose goes into C.
    g_f37K = -1;
    if (havePose && (wmMode == 4 || wmMode == 5)) {
        EdvrNativeNowPose rp{};
        if (nativeFrameNowPose(rp) && rp.renderValid && rp.ringCount > 0) {
            const uint32_t n = rp.ringCount;
            int pick = -1;
            double best = 1e9;
            if (wmMode == 4 && sc.valid && sc.mOk) {
                for (uint32_t k = 0; k < n; ++k) {
                    float H[16];
                    f37RingView(C, rp, k, H);
                    const double a = f31AngleDeg(sc.mMat, H);
                    if (a < best) {
                        best = a;
                        pick = static_cast<int>(k);
                    }
                }
                g_f37ResSum += best;
                ++g_f37ResN;
            }
            if (pick < 0) {
                pick = static_cast<int>(n > 2 ? 2 : n - 1);
                ++g_f37Fallback;
            }
            g_f37K = pick;
            ++g_f37Hist[pick & 7];
            float H[16];
            f37RingView(C, rp, static_cast<uint32_t>(pick), H);
            std::memcpy(C, H, sizeof(C));
            const uint64_t nowQ = f30Qpc();
            const uint32_t idxPick = (rp.ringNewest + 8u - static_cast<uint32_t>(pick)) % 8u;
            const uint64_t agePick = (rp.ringQpc[idxPick] && nowQ >= rp.ringQpc[idxPick]) ? f30QpcUs(nowQ - rp.ringQpc[idxPick]) : 0;
            const uint64_t ageNew = (rp.ringQpc[rp.ringNewest] && nowQ >= rp.ringQpc[rp.ringNewest]) ? f30QpcUs(nowQ - rp.ringQpc[rp.ringNewest]) : 0;
            g_f37AgePickMax = std::max(g_f37AgePickMax, agePick);
            g_f37AgeNewMax = std::max(g_f37AgeNewMax, ageNew);
        }
    }    if (!havePose) {
        if (now - g_worldLineMs >= 5000 || g_worldLineMs == 0) {
            g_worldLineMs = now;
            Log::get().note("native on foot world markers: fixed, but no fresh head pose (no list-B sample in 0.5 s); markers are left as the game places them");
        }
        return;
    }
    float t[3] = {};
    if (!guardedRead(ia + 0x40, t, sizeof(t))) return;
    // F27: fixed = the inverse, t' = (t - row3) . R^T (the flight ruled it out as the cure: it applied an extra -C yaw);
    // forward = t' = t . R + row3, the map the reticle head mode uses for its direction (the default).
    float tp[3] = {};
    for (size_t j = 0; j < 3; ++j) {
        float acc = 0.0f;
        for (size_t i = 0; i < 3; ++i)
            acc += wmMode == 0 ? (t[i] - C[12 + i]) * C[4 * j + i] : t[i] * C[4 * i + j];
        tp[j] = acc + (wmMode == 0 ? 0.0f : C[12 + j]);
    }
    if (!guardedWriteFloat(ia + 0x40, tp[0]) || !guardedWriteFloat(ia + 0x44, tp[1]) || !guardedWriteFloat(ia + 0x48, tp[2])) {
        if (now - g_worldLineMs >= 5000 || g_worldLineMs == 0) {
            g_worldLineMs = now;
            Log::get().note("native on foot world markers: key %u: the instance could not be written; left as the game placed it", static_cast<unsigned>(key));
        }
        return;
    }
    if (now - g_worldLineMs >= 5000 || g_worldLineMs == 0) {
        g_worldLineMs = now;
        const float yawC = static_cast<float>(std::atan2(static_cast<double>(C[8]), static_cast<double>(C[10])) * 180.0 / 3.14159265358979323846);
        Log::get().note("native on foot world markers: key %u: mode %s; t (game) (%.3f, %.3f, %.3f) -> t' (written) (%.3f, %.3f, %.3f); C yaw %.2f deg (row 2 x, z)",
                        static_cast<unsigned>(key), wmMode == 0 ? "fixed" : wmMode == 3 ? "forward_now" : "forward", static_cast<double>(t[0]), static_cast<double>(t[1]),
                        static_cast<double>(t[2]), static_cast<double>(tp[0]), static_cast<double>(tp[1]), static_cast<double>(tp[2]),
                        static_cast<double>(yawC));
        // F30/F31: where C came from since the last line, its lag and age (QPC); P = node+0x50 (three floats) and t' as written. F31: the
        // forward map under C (the same-call head) and under H_M (the runtime's "now" head), side by side, and the angle between them.
        float P[3] = {};
        const bool okP = guardedRead(na + 0x50, P, sizeof(P));
        float eyeDiff = 0.0f;
        {
            F9Lock lock;
            eyeDiff = g_worldC.eyeDiff;
        }
        float tpC[3] = {}, tpH[3] = {};
        f31ForwardMap(t, g_f31SameC, tpC);
        if (g_f31HaveNow) f31ForwardMap(t, g_f31NowH, tpH);
        Log::get().note("native on foot world markers: C source since the last line: same-call list B %llu, P2 fallback %llu, none %llu; "
                        "sample lag max %lld; C age max %llu us, mean %llu us; P (node+0x50) %s (%.3f, %.3f, %.3f); t' (written) (%.3f, %.3f, %.3f)",
                        static_cast<unsigned long long>(g_f30PostSame), static_cast<unsigned long long>(g_f30PostFall),
                        static_cast<unsigned long long>(g_f30PostNone), static_cast<long long>(g_f30LagMax),
                        static_cast<unsigned long long>(g_f30AgeMaxUs),
                        static_cast<unsigned long long>(g_f30AgeN ? g_f30AgeSumUs / g_f30AgeN : 0), okP ? "read" : "unread",
                        static_cast<double>(P[0]), static_cast<double>(P[1]), static_cast<double>(P[2]), static_cast<double>(tp[0]),
                        static_cast<double>(tp[1]), static_cast<double>(tp[2]));
        if (g_f31HaveNow)
            Log::get().note("native on foot world markers: H_M (now head) vs C: angle %.3f deg; now answer #%u, frame #%llu; forward under C t' "
                            "(%.3f, %.3f, %.3f), under H_M t' (%.3f, %.3f, %.3f); %llu posts with no now pose; P2 eye 0 vs eye 1 %.5f",
                            g_f31AngleDeg, static_cast<unsigned>(g_f31NowSeq), static_cast<unsigned long long>(g_f31FrameSeq),
                            static_cast<double>(tpC[0]), static_cast<double>(tpC[1]), static_cast<double>(tpC[2]), static_cast<double>(tpH[0]),
                            static_cast<double>(tpH[1]), static_cast<double>(tpH[2]), static_cast<unsigned long long>(g_f31NoNow),
                            static_cast<double>(eyeDiff));
        else
            Log::get().note("native on foot world markers: no runtime now head this window (%llu posts without one; frame input version %u, nowValid %d); "
                            "forward under C t' (%.3f, %.3f, %.3f); P2 eye 0 vs eye 1 %.5f", static_cast<unsigned long long>(g_f31NoNow),
                            static_cast<unsigned>(g_f31LastInputVersion), static_cast<int>(g_f31LastNowValid), static_cast<double>(tpC[0]),
                            static_cast<double>(tpC[1]), static_cast<double>(tpC[2]),
                            static_cast<double>(eyeDiff));
        Log::get().note("native on foot world markers: F37 ring: k histogram [%llu %llu %llu %llu %llu %llu %llu %llu], fallback k=2 %llu, mean residual %.3f deg over %llu posts, chosen-pose age max %llu us, newest render age max %llu us; mode %d",
                        static_cast<unsigned long long>(g_f37Hist[0]), static_cast<unsigned long long>(g_f37Hist[1]), static_cast<unsigned long long>(g_f37Hist[2]),
                        static_cast<unsigned long long>(g_f37Hist[3]), static_cast<unsigned long long>(g_f37Hist[4]), static_cast<unsigned long long>(g_f37Hist[5]),
                        static_cast<unsigned long long>(g_f37Hist[6]), static_cast<unsigned long long>(g_f37Hist[7]), static_cast<unsigned long long>(g_f37Fallback),
                        g_f37ResN ? g_f37ResSum / static_cast<double>(g_f37ResN) : 0.0, static_cast<unsigned long long>(g_f37ResN),
                        static_cast<unsigned long long>(g_f37AgePickMax), static_cast<unsigned long long>(g_f37AgeNewMax), wmMode);
        for (uint64_t & h : g_f37Hist) h = 0;
        g_f37Fallback = g_f37ResN = 0;
        g_f37ResSum = 0.0;
        g_f37AgePickMax = g_f37AgeNewMax = 0;
        g_f30PostSame = g_f30PostFall = g_f30PostNone = 0;
        g_f30LagMax = 0;
        g_f30AgeMaxUs = g_f30AgeSumUs = g_f30AgeN = 0;
        g_f31NoNow = 0;
    }
}

// The stub: push rdx; sub rsp,0x20; mov rdx,rsi; mov rax,probe; call rax; add rsp,0x20; pop rdx; mov rcx,[rsi+0x60]; call real; post; ret
// (F25: the real call is now a call, so the post-call can write the instance; rax is kept across both.)
// The probe gets rcx (the instance, as the game passed it) and rdx = rsi (the node); rdx is preserved for the real function.
constexpr uintptr_t kReticleCallRva = 0x1A15644;      // the 5-byte call E8 rel32
constexpr uintptr_t kReticleLoadRva = 0x1A15640;      // mov rcx,[rsi+0x60] (48 8B 4E 60), the 4 bytes before the call
constexpr uintptr_t kReticleFnRva = 0x434ECC0;        // the real function the call targets
const uint8_t kReticleExpect[9] = {0x48, 0x8B, 0x4E, 0x60, 0xE8, 0x77, 0x96, 0x93, 0x02};
uint8_t* g_reticleStub = nullptr;                     // frame thread only: the stub page (kept after a restore)
bool g_reticleInstalled = false;                      // frame thread only: the call points at the stub
int32_t g_reticleOrigRel = 0;                         // the original rel32 (0x02939677)
bool g_reticleRefusalNoted = false;                   // frame thread only

__declspec(noinline) bool guardedStoreDword(uintptr_t address, uint32_t value) noexcept {
    __try {
        *reinterpret_cast<volatile uint32_t*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// A single 4-byte store to the rel32 at call+1. The rel32 lies within one 64-byte cache line (its bytes are 0x1A15645..0x1A15648), so one
// store is single-copy atomic on x86; the instruction is not torn for a thread that is executing it.
bool storeRel32(uintptr_t address, int32_t rel) noexcept {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(address), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    const bool ok = guardedStoreDword(address, static_cast<uint32_t>(rel));
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(address), 4, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 4);
    return ok;
}

void refuseReticle(const std::string& why) {
    if (g_reticleRefusalNoted) return;
    g_reticleRefusalNoted = true;
    Log::get().note("native on foot indicator: NOT installed -- %s. The game's code is left as it is.", why.c_str());
}

// `want`: the stub should be in place. Installs the stub once (verified bytes, a stub within reach), and restores the original rel32 otherwise.
void reticleTick(bool want, const char* reason) {
    const uintptr_t call = g_base + kReticleCallRva;
    if (want) {
        if (g_reticleInstalled) return;
        const char* idWhy = nullptr;
        if (!checkIdentity(g_base, &idWhy)) {
            refuseReticle(std::string("the PE identity check failed (") + idWhy + ")");
            return;
        }
        uint8_t cur[9] = {};
        if (!guardedRead(g_base + kReticleLoadRva, cur, sizeof(cur))) {
            refuseReticle("the bytes could not be read");
            return;
        }
        if (std::memcmp(cur, kReticleExpect, sizeof(cur)) != 0) {
            refuseReticle("the bytes at +0x1A15640 are not mov rcx,[rsi+0x60] and call 0x434ECC0");
            return;
        }
        int32_t origRel = 0;
        std::memcpy(&origRel, cur + 5, 4);
        if (!g_reticleStub) {
            g_reticleStub = allocateRelay(call);
            if (!g_reticleStub) {
                refuseReticle("no executable memory within two gigabytes of the call");
                return;
            }
            uint8_t* p = g_reticleStub;
            size_t i = 0;
            const uint8_t head[] = {0x52, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xF2, 0x48, 0xB8};
            std::memcpy(p + i, head, sizeof(head));
            i += sizeof(head);
            const uint64_t probe = reinterpret_cast<uint64_t>(&reticleIndicatorProbe);
            std::memcpy(p + i, &probe, 8);
            i += 8;
            // F25: call probe; add rsp,0x20; pop rdx; mov rcx,[rsi+0x60]; sub rsp,0x28; mov rax,real; call rax; add rsp,0x28;
            // push rax; sub rsp,0x20; mov rcx,rsi; mov rax,post; call rax; add rsp,0x20; pop rax; ret.
            const uint8_t mid[] = {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20, 0x5A, 0x48, 0x8B, 0x4E, 0x60, 0x48, 0x83, 0xEC, 0x28, 0x48, 0xB8};
            std::memcpy(p + i, mid, sizeof(mid));
            i += sizeof(mid);
            const uint64_t real = g_base + kReticleFnRva;
            std::memcpy(p + i, &real, 8);
            i += 8;
            const uint8_t afterReal[] = {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0x50, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xF1, 0x48, 0xB8};
            std::memcpy(p + i, afterReal, sizeof(afterReal));
            i += sizeof(afterReal);
            const uint64_t post = reinterpret_cast<uint64_t>(&reticleWorldPost);
            std::memcpy(p + i, &post, 8);
            i += 8;
            const uint8_t tail[] = {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20, 0x58, 0xC3};
            std::memcpy(p + i, tail, sizeof(tail));
            i += sizeof(tail);
            DWORD old = 0;
            if (!VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old) || !FlushInstructionCache(GetCurrentProcess(), p, i)) {
                refuseReticle("the stub page could not be made executable");
                return;
            }
        }
        const int64_t rel = static_cast<int64_t>(reinterpret_cast<uintptr_t>(g_reticleStub)) - static_cast<int64_t>(call + 5);
        if (rel < INT32_MIN || rel > INT32_MAX) {
            refuseReticle("the stub is out of reach of the call");
            return;
        }
        if (!storeRel32(call + 1, static_cast<int32_t>(rel))) {
            refuseReticle("the call could not be written");
            return;
        }
        g_reticleOrigRel = origRel;
        g_reticleInstalled = true;
        g_reticleRefusalNoted = false;
        Log::get().note("native on foot indicator: call EliteDangerous64.exe+0x%llX -> stub (probe) for the marker component",
                        static_cast<unsigned long long>(kReticleCallRva));
        return;
    }
    if (!g_reticleInstalled) return;
    int32_t cur = 0;
    if (!guardedRead(call + 1, &cur, 4)) return;   // retried next frame
    if (cur != static_cast<int32_t>(reinterpret_cast<int64_t>(g_reticleStub) - static_cast<int64_t>(call + 5))) {
        g_reticleInstalled = false;
        Log::get().note("native on foot indicator: call NOT restored -- the rel32 reads %d, not ours (restore: %s)", static_cast<int>(cur), reason);
        return;
    }
    if (!storeRel32(call + 1, g_reticleOrigRel)) return;
    g_reticleInstalled = false;
    Log::get().note("native on foot indicator: call EliteDangerous64.exe+0x%llX -> the game's own (restored: %s)",
                    static_cast<unsigned long long>(kReticleCallRva), reason);
}

// F16: the game re-sends the HUD layout only when its own margins or [HRC+0x290] say so (RE: the byte is set to 1 again after the re-send, at
// 0x1AB385B). When the on-state, a group offset or the spread changes, one re-send is asked for by writing 0 to [HRC+0x290] (guarded). The hook above
// then applies the offsets, or their absence, to the new layout.
struct LayoutStamp {
    bool on = false;
    float off[nofoot::kHudGroupCount * 2] = {};
    float spread = 0.0f;
    float elem = 0.0f;     // F17: the element scale the layout is to use (0 = the game's own; the value written otherwise)
};
bool g_layoutStampValid = false;   // frame thread only
LayoutStamp g_layoutStampLast;     // frame thread only

bool sameStamp(const LayoutStamp& a, const LayoutStamp& b) noexcept {
    if (a.on != b.on || !sameBits(a.spread, b.spread) || !sameBits(a.elem, b.elem)) return false;
    for (int i = 0; i < nofoot::kHudGroupCount * 2; ++i)
        if (!sameBits(a.off[i], b.off[i])) return false;
    return true;
}

// `elem` is the element scale the layout is to use now (F17), 0 for the game's own.
void layoutResendTick(bool on, float elem) {
    LayoutStamp now;
    now.on = on;
    now.elem = elem;
    uintptr_t hrc = 0;
    {
        F9Lock lock;
        std::memcpy(now.off, g_hudOffsetRead, sizeof(now.off));
        hrc = g_layoutHrc;
    }
    now.spread = g_hudSpread.load(std::memory_order_relaxed);
    if (g_layoutStampValid && sameStamp(now, g_layoutStampLast)) return;
    g_layoutStampValid = true;
    g_layoutStampLast = now;
    if (!hrc) return;   // no layout has run yet: the first one the game sends applies the values
    if (!guardedWriteByte(hrc + 0x290, 0)) {
        Log::get().note("native on foot hud: layout re-send NOT asked -- HRC+0x290 could not be written; the layout keeps its last values");
        return;
    }
    Log::get().note("native on foot hud: layout re-send asked (HRC+0x290 = 0): on %s, spread %.2f, element scale %.2f", on ? "yes" : "no",
                    static_cast<double>(now.spread), static_cast<double>(elem));
}

// Frame thread: installs the layout probe once (VR only); drains the layout lines; logs the reticle indicator rows every 5 s.
struct F14Probes {
    static constexpr size_t kLayoutSlot = 0;
};
bool g_f14Tried = false;                       // frame thread only
uint64_t g_f14IndicatorMs = 0;                 // frame thread only

// Frame thread, every frame: the layout lines; every 5 s the reticle indicator rows.
void f14FrameTick(uint64_t nowMs) {
    F14Line lines[kF14Ring];
    size_t n = 0;
    uint64_t dropped = 0;
    {
        F9Lock lock;
        while (g_f14Read < g_f14Written && n < kF14Ring) lines[n++] = g_f14Ring[(g_f14Read++) % kF14Ring];
        dropped = g_f14Dropped;
        g_f14Dropped = 0;
    }
    for (size_t i = 0; i < n; ++i) Log::get().note("%s", lines[i].text);
    if (dropped) Log::get().note("native on foot hud part: %llu line(s) dropped (the queue of %zu was full)", static_cast<unsigned long long>(dropped), kF14Ring);
    if (g_f14IndicatorMs == 0) g_f14IndicatorMs = nowMs;
    if (nowMs - g_f14IndicatorMs < 5000) return;
    g_f14IndicatorMs = nowMs;
    F14Key rows[kF14Keys];
    {
        F9Lock lock;
        for (size_t i = 0; i < kF14Keys; ++i) rows[i] = g_f14Keys[i];
        for (auto& r : g_f14Keys) r.n = 0;
    }
    for (const F14Key& r : rows) {
        if (!r.used || r.n == 0) continue;
        char pos[64];
        if (r.posOk) std::snprintf(pos, sizeof(pos), "(%.3f,%.3f,%.3f)", static_cast<double>(r.pos[0]), static_cast<double>(r.pos[1]), static_cast<double>(r.pos[2]));
        else std::snprintf(pos, sizeof(pos), "?");
        Log::get().note("native on foot indicator: key=%u n=%llu f30=%u f31=%u f32=%u scale=%.3f pos=%s fl=(%.3f,%.3f)", static_cast<unsigned>(r.key),
                        static_cast<unsigned long long>(r.n), static_cast<unsigned>(r.f30), static_cast<unsigned>(r.f31), static_cast<unsigned>(r.f32),
                        static_cast<double>(r.scale), pos, static_cast<double>(r.fl34), static_cast<double>(r.fl38));
    }
}

// F22: defined with the look-to-aim block below. Reads list B into the look head when the frame thread wants it (never stands anything down).
void lookReadHeadP2(uintptr_t listB) noexcept;

// P2: rcx = HRC, and five more arguments, all forwarded (the later chunks were not scanned for stack arguments).
uint64_t __fastcall f9HrcSubmitHooked(void* hrc, void* a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    const F9Fn6 original = reinterpret_cast<F9Fn6>(g_f9Probe[1].original);
    if (!original) return 0;
    if (!g_f9Stop[1].load(std::memory_order_relaxed)) f9RecordHrc(f9State(), hrc);   // reads before the call
    if (g_lookWantRead.load(std::memory_order_relaxed)) lookReadHeadP2(static_cast<uintptr_t>(a3));   // F22: the look and probe B's head (read only)
    f13PlaceHud(hrc, static_cast<uintptr_t>(a3));   // F12/F13: the HUD placement (distance, size, head), before the original runs
    return original(hrc, a2, a3, a4, a5, a6);
}

// P3: rcx = instance, rdx = view, r8d. Compares rcx against the three HRC instances only.
uint64_t __fastcall f9InstSubmitHooked(void* inst, void* view, uint64_t r8, uint64_t r9) {
    const F9Fn4 original = reinterpret_cast<F9Fn4>(g_f9Probe[2].original);
    if (!original) return 0;
    const uintptr_t ia = reinterpret_cast<uintptr_t>(inst);
    int k = -1;
    for (size_t i = 0; i < nofoot::kF9Hrc; ++i) {
        if (ia && g_hrcInst[i].load(std::memory_order_relaxed) == ia) k = static_cast<int>(i);
    }
    if (k >= 0 && !g_f9Stop[2].load(std::memory_order_relaxed)) f9RecordInst(f9State(), static_cast<uint8_t>(k), view);
    return original(inst, view, r8, r9);
}

bool prepareF9Relay(void* trampoline, void* context) noexcept {
    F9Probe* probe = static_cast<F9Probe*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(probe->relay + ecm::kCallbackRelayTrampolineAt, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(probe->relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), probe->relay, ecm::kCallbackRelayBytes))
        return false;
    probe->original = trampoline;
    return true;
}

struct F9ProbeSpec {
    const char* name;
    uintptr_t rva;
    const uint8_t* prologue;
    size_t prologueBytes;
    size_t stolen;
    void* callback;
    const char* label;
};

bool installF9ProbeInto(F9Probe& pr, const F9ProbeSpec& s, uintptr_t base, size_t* stolenOut, std::string* whyOut) {
    const uintptr_t target = base + s.rva;
    if (!bytesMatch(target, s.prologue, s.prologueBytes)) {
        *whyOut = "the prologue bytes are not build 332841's";
        return false;
    }
    size_t covered = 0;
    while (covered < s.stolen) {
        size_t rip = 0;
        const size_t len = codeInstructionLength(reinterpret_cast<const uint8_t*>(target + covered), s.prologueBytes - covered, &rip);
        if (!len) {
            *whyOut = "a prologue instruction is one the decoder does not recognise";
            return false;
        }
        covered += len;
    }
    if (covered != s.stolen) {
        *whyOut = "the prologue does not end on an instruction boundary at the expected byte";
        return false;
    }
    pr.relay = allocateRelay(target);
    if (!pr.relay) {
        *whyOut = "no executable memory within two gigabytes of the target";
        return false;
    }
    ecm::buildCallbackRelay(pr.relay, &pr.gate, s.callback);
    pr.hook = new CodeHook();
    if (!pr.hook->install(reinterpret_cast<void*>(target), pr.relay, nullptr, s.name, &prepareF9Relay, &pr)) {
        const char* why = pr.hook->refusal();
        VirtualFree(pr.relay, 0, MEM_RELEASE);
        pr.relay = nullptr;
        *whyOut = why[0] ? why : "CodeHook gave no reason";
        return false;
    }
    pr.gate.store(1, std::memory_order_seq_cst);
    *stolenOut = pr.hook->stolenBytes();
    return true;
}

bool installF9Probe(size_t i, const F9ProbeSpec& s, uintptr_t base, size_t* stolenOut, std::string* whyOut) {
    return installF9ProbeInto(g_f9Probe[i], s, base, stolenOut, whyOut);
}

// Frame thread, once: VR only, behind the identity check. One line per probe, installed or refused.
void installF9ProbesOnce() {
    if (g_f9Tried) return;
    g_f9Tried = true;
    if (!runtimeVrProfile()) {
        Log::get().note("native on foot probes: not installed (the flat profile)");
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const char* peWhy = nullptr;
    if (!base || !checkIdentity(base, &peWhy)) {
        Log::get().note("native on foot probes: NOT installed -- identity mismatch (%s); the game was not touched.", peWhy ? peWhy : "no module");
        return;
    }
    if (!g_base) g_base = base;
    const F9ProbeSpec specs[nofoot::kF9ProbeCount] = {
        {"native-on-foot-panel-submit", nofoot::kPanelSubmitRva, nofoot::kPanelSubmitPrologue, nofoot::kPanelSubmitPrologueBytes,
         nofoot::kPanelSubmitStolen, reinterpret_cast<void*>(&f9PanelSubmitHooked), "P1 (panel submit)"},
        {"native-on-foot-hrc-submit", nofoot::kHrcSubmitRva, nofoot::kHrcSubmitPrologue, nofoot::kHrcSubmitPrologueBytes,
         nofoot::kHrcSubmitStolen, reinterpret_cast<void*>(&f9HrcSubmitHooked), "P2 (HRC submit)"},
        {"native-on-foot-inst-submit", nofoot::kInstSubmitRva, nofoot::kInstSubmitPrologue, nofoot::kInstSubmitPrologueBytes,
         nofoot::kInstSubmitStolen, reinterpret_cast<void*>(&f9InstSubmitHooked), "P3 (instance submit)"},
    };
    for (size_t i = 0; i < nofoot::kF9ProbeCount; ++i) {
        const F9ProbeSpec& s = specs[i];
        size_t stolen = 0;
        std::string why;
        if (installF9Probe(i, s, base, &stolen, &why)) {
            g_f9Armed[i] = true;
            Log::get().note("native on foot probe: installed %s at EliteDangerous64.exe+0x%llX, stolen=%zu bytes, prologue %zu bytes verified, "
                            "relay at 0x%llX (read only; temporary).",
                            s.label, static_cast<unsigned long long>(s.rva), stolen, s.prologueBytes,
                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_f9Probe[i].relay)));
        } else {
            Log::get().note("native on foot probe: %s NOT installed at EliteDangerous64.exe+0x%llX -- %s. The game was not touched by it.",
                            s.label, static_cast<unsigned long long>(s.rva), why.c_str());
        }
    }
}

// ---- F22: the training load wait (read only), camera rig probes A and B (read only), and look to aim -----------------------------------
// Every hook here follows the F9 probe pattern: the prologue bytes are verified before any write, a near relay carries the callback, and an
// install refusal is logged with its reason and leaves the game as it was. Addresses are build 332841's (Epic exe checked offline, 2026-10-14).
constexpr uintptr_t kLoadWaitRva = 0x260D6B0;   // the load pipeline's wait condition: vtable slot 3 of +0x5298390; false = waiting on ReadyToAllocIDs
const uint8_t kLoadWaitPrologue[10] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20};
constexpr uintptr_t kLookUpdateRva = 0x19973D0; // PlayerHumanoidControlActivity update: act in rcx; reads rotate-abs at [[act+0x190]+0x18]
const uint8_t kLookUpdatePrologue[32] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55,
                                         0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x08, 0xFF, 0xFF, 0xFF, 0x48};
constexpr uintptr_t kLookProcRva = 0x1A9C370;  // the processor: comp in rcx, dt in xmm1; its yaw delta is stored at comp+0x5F4
const uint8_t kLookProcPrologue[5] = {0x48, 0x8B, 0xC4, 0x55, 0x56};
constexpr uintptr_t kLookUpdPitchOff = 0x198, kLookUpdRotOff = 0x190, kLookActValueOff = 0x18;   // act+0x190 and +0x198 are the action pointers
constexpr uintptr_t kCompYawDeltaOff = 0x5F4, kCompBasisOff = 0x5B0, kCompGainLoOff = 0x7D8, kCompGainHiOff = 0x7DC;
constexpr float kLookMaxTurnRad = 0.1745f;     // the most the controller asks the body to turn in one frame (10 degrees)
constexpr uint64_t kLookHeadMaxAgeMs = 500;    // a head sample older than this does not steer

F9Probe g_f22LoadWait;   // read only
F9Probe g_f22Update;     // look to aim: writes rotate-abs for one update call while engaged, restores it after the original
F9Probe g_f22Proc;       // look to aim: read only (the comp pointer, the body turn after the processor)
bool g_f22ProbesTried = false;   // frame thread only
bool g_f22LookTried = false;     // frame thread only
bool g_lookArmed = false;        // frame thread only: both look hooks are in; the controller never writes without both
bool g_lookEngagedLast = false;  // frame thread only
uint64_t g_lookLastMs = 0;       // frame thread only: the controller's dt
uint64_t g_lookLineMs = 0;       // frame thread only: the 5 s line
double g_lookWinStartDeg = 0.0;  // frame thread only: the published total at the last line
F13Head g_lookHead;              // under g_f9Lock: the list-B head sample from the P2 hook (C and the eye 0 view row 2, column 1)
uint64_t g_lookHeadMs = 0;       // under g_f9Lock: when g_lookHead was read
bool g_lookPendingValid = false; // game thread only: the update hook wrote our input for the processor to account
bool g_lookPassPending = false;  // game thread only (F24): the update hook's mouse-only call is pending for the processor
float g_lookPassOld = 0.0f;      // game thread only (F24): the mouse's rotate-abs for that call
double g_lookPassSumD = 0.0;     // under g_f9Lock (F24): the mouse-only yaw deltas since the frame thread last took them
uint32_t g_lookPassAgree = 0, g_lookPassDisagree = 0, g_lookPassStill = 0;   // frame thread only (F24): this window's passive counts
double g_lookPassPrevView = 0.0;  // frame thread only (F24): the list-B eye 0 forward yaw at the last tick
bool g_lookPassHavePrev = false;  // frame thread only
uint64_t g_lookPassLineMs = 0;    // frame thread only
float g_lookPendOurs = 0.0f, g_lookPendSum = 0.0f;   // game thread only
double g_lookTotal = 0.0;        // under g_f9Lock: the body turn we caused so far, game yaw radians (the runtime's compensation total)
float g_lookLastD = 0.0f, g_lookLastPart = 0.0f;     // under g_f9Lock: the processor's yaw delta and our part of it, last call
std::atomic<float> g_lookAMeas{0.0f};   // the gain measured from the last write: D / (old + ours); 0 = none yet
std::atomic<float> g_lookLastOld{0.0f}; // the mouse's rotate-abs from the last write (the instrument's "old input")
std::atomic<float> g_lookLastInput{0.0f};
std::atomic<uint32_t> g_lookUpdCalls{0}, g_lookWrites{0}, g_lookRefusals{0}, g_lookProcCalls{0};
std::atomic<const char*> g_lookWhy{nullptr};   // the last refusal's reason (shown on the 5 s line)

// ---- F23 (Part E, TEMPORARY, read only): the load pipeline's stage timers ("load stages:") ---------------------------------------------
// The dispatcher (EliteDangerous64.exe+0x1CA6510, a virtual): rdx = pipeline node, obj = rdx - 0x20 (rdx 0 is skipped), rcx unused, xmm2 = dt,
// returns al. Pre-hook only: the observation reads the state the previous call left, then the original runs and its result is returned unchanged.
// The relay uses RAX only, so rdx and xmm2 reach the callback and the original intact (the same check as the processor hook).
constexpr uintptr_t kStageDispatchRva = 0x1CA6510;
const uint8_t kStageDispatchPrologue[16] = {0x40, 0x55, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x38, 0x45, 0x33, 0xE4, 0x0F, 0x29, 0x74, 0x24, 0x20};
constexpr uintptr_t kStageTableRva = 0x5E7B460;   // 21 entries, stride 0x30; entry+0 is a char* into .rdata (the stage name)
constexpr size_t kStageCount = 21, kStageEntryStride = 0x30, kStageNameMax = 48;
constexpr uintptr_t kStageRecOff = 0x2B0, kStageRecStride = 0x18, kStageClockOff = 0x4C4, kStageNetOff = 0x4B8, kStageNetGateOff = 0xC0;
constexpr uint32_t kStageGateOpen = 5;
F9Probe g_f23Stage;
using F23StageFn = bool(__fastcall*)(void*, void*, float);
struct StageSnap { int state = -1; float start = 0.0f, end = 0.0f; };   // state: 0 waiting, 1 running, 2 done, -1 unread
std::mutex g_stMutex;                  // the dispatcher may run on more than one thread
std::string g_stNames[kStageCount];    // read once at install
bool g_stInstalled = false, g_stNoCallNoted = false;   // frame thread and game thread
uint64_t g_stInstallMs = 0, g_stCalls = 0;   // g_stCalls under g_stMutex
uintptr_t g_stObj = 0;                 // the pipeline obj observed last
int g_stLast[kStageCount] = {};        // last seen state per stage, for g_stObj
StageSnap g_stSnap[kStageCount];       // the snapshot of g_stObj's records
bool g_stSumRunning = false;           // F24: the summary written when EnterLocationFinalStep starts running
bool g_stSumDone = false;              // the summary written when it is done (or when the obj changes first)
int g_stGate = 0;                      // 0 unknown, 1 open, 2 closed (dword < 5), 3 unread
uint64_t g_stGateSinceMs = 0;
uint32_t g_stGateValue = 0;
uint64_t g_stFirstCallMs = 0;

const char* stageStateName(int s) {
    switch (s) {
        case 0: return "waiting";
        case 1: return "running";
        case 2: return "done";
        default: return "unread";
    }
}

// The stage names, read once: each entry's char* must lie inside the module, and the string is capped at kStageNameMax.
void stageLoadNames(uintptr_t base) {
    uint32_t lfanew = 0, sizeOfImage = 0;
    if (!guardedRead(base + 0x3C, &lfanew, 4) || !guardedRead(base + lfanew + 0x50, &sizeOfImage, 4) || !sizeOfImage) return;
    for (size_t i = 0; i < kStageCount; ++i) {
        uintptr_t p = 0;
        char buf[kStageNameMax + 1] = {};
        std::string name = "?";
        if (guardedRead(base + kStageTableRva + i * kStageEntryStride, &p, 8) && p >= base && p < base + sizeOfImage &&
            guardedRead(p, buf, kStageNameMax)) {
            buf[kStageNameMax] = '\0';
            name = buf;
        }
        g_stNames[i] = name;
    }
}

// The summary of g_stObj's load: each stage that ran, in table order, with its duration (or running). One line.
void stageSummary(const char* why, float clock) {
    char line[1000] = {};
    size_t n = 0;
    unsigned ran = 0;
    for (size_t i = 0; i < kStageCount; ++i) {
        const StageSnap& s = g_stSnap[i];
        if (s.state < 1) continue;
        ++ran;
        char item[96];
        if (s.state == 2) std::snprintf(item, sizeof(item), " %s=%.3fs", g_stNames[i].c_str(), static_cast<double>(s.end - s.start));
        else std::snprintf(item, sizeof(item), " %s=running", g_stNames[i].c_str());
        const size_t len = std::strlen(item);
        if (n + len < sizeof(line) - 1) {
            std::memcpy(line + n, item, len);
            n += len;
            line[n] = '\0';
        }
    }
    Log::get().note("load stages: summary (%s) obj=0x%llX clock %.3f, %u stage(s) ran, dispatcher calls %llu:%s", why,
                    static_cast<unsigned long long>(g_stObj), static_cast<double>(clock), ran, static_cast<unsigned long long>(g_stCalls),
                    ran ? line : " none");
}

// Under g_stMutex: one observation of pipeline obj. Logs the changes against the last seen states, the gate's transitions, and the completion.
void stageObserve(uintptr_t obj) {
    if (obj != g_stObj) {
        if (g_stObj != 0 && !g_stSumDone) {
            float clock = 0.0f;
            guardedRead(g_stObj + kStageClockOff, &clock, 4);
            stageSummary("new pipeline obj", clock);
        }
        Log::get().note("load stages: new pipeline obj=0x%llX (previous 0x%llX); a new load starts here",
                        static_cast<unsigned long long>(obj), static_cast<unsigned long long>(g_stObj));
        SYSTEMTIME wall{};
        GetLocalTime(&wall);   // F24: the wall-clock time of this obj's first dispatcher call
        Log::get().note("load stages: first dispatcher call for obj=0x%llX at wall time %02u:%02u:%02u.%03u",
                        static_cast<unsigned long long>(obj), static_cast<unsigned>(wall.wHour), static_cast<unsigned>(wall.wMinute),
                        static_cast<unsigned>(wall.wSecond), static_cast<unsigned>(wall.wMilliseconds));
        g_stObj = obj;
        for (size_t i = 0; i < kStageCount; ++i) {
            g_stLast[i] = 0;
            g_stSnap[i] = StageSnap{};
        }
        g_stSumRunning = false;
        g_stSumDone = false;
        g_stGate = 0;
    }
    float clock = 0.0f;
    if (!guardedRead(obj + kStageClockOff, &clock, 4)) clock = 0.0f;
    for (size_t i = 0; i < kStageCount; ++i) {
        const uintptr_t rec = obj + kStageRecOff + i * kStageRecStride;
        uint32_t st = 0;
        float a = 0.0f, b = 0.0f;
        StageSnap s;
        if (guardedRead(rec, &st, 4) && guardedRead(rec + 4, &a, 4) && guardedRead(rec + 8, &b, 4) && st <= 2) {
            s.state = static_cast<int>(st);
            s.start = a;
            s.end = b;
        }
        if (s.state != g_stLast[i]) {
            if (s.state == 2)
                Log::get().note("load stages: %s %s -> %s at clock %.3f (start %.3f, end %.3f, duration %.3f s)", g_stNames[i].c_str(),
                                stageStateName(g_stLast[i]), stageStateName(s.state), static_cast<double>(clock), static_cast<double>(s.start),
                                static_cast<double>(s.end), static_cast<double>(s.end - s.start));
            else
                Log::get().note("load stages: %s %s -> %s at clock %.3f (start %.3f, end %.3f)", g_stNames[i].c_str(), stageStateName(g_stLast[i]),
                                stageStateName(s.state), static_cast<double>(clock), static_cast<double>(s.start), static_cast<double>(s.end));
            g_stLast[i] = s.state;
        }
        g_stSnap[i] = s;
    }
    // The net gate: no stage starts until the net object's dword at +0xC0 reaches 5.
    uintptr_t net = 0;
    uint32_t gv = 0;
    const bool okNet = guardedRead(obj + kStageNetOff, &net, 8) && net && guardedRead(net + kStageNetGateOff, &gv, 4);
    const int gs = !okNet ? 3 : (gv >= kStageGateOpen ? 1 : 2);
    if (gs != g_stGate) {
        const uint64_t now = GetTickCount64();
        if (gs == 2) {
            Log::get().note("load stages: net gate closed: dword +0xC0 = %u < %u at clock %.3f; no stage starts until it reaches %u", static_cast<unsigned>(gv),
                            static_cast<unsigned>(kStageGateOpen), static_cast<double>(clock), static_cast<unsigned>(kStageGateOpen));
            g_stGateSinceMs = now;
        } else if (gs == 1) {
            Log::get().note("load stages: net gate open (dword +0xC0 = %u) at clock %.3f, after %.2f s held", static_cast<unsigned>(gv),
                            static_cast<double>(clock), g_stGate == 2 ? static_cast<double>(now - g_stGateSinceMs) / 1000.0 : 0.0);
        } else if (gs == 3) {
            Log::get().note("load stages: net gate unread (the net object at +0x4B8 or its dword at +0xC0 is not readable) at clock %.3f",
                            static_cast<double>(clock));
        }
        g_stGate = gs;
        g_stGateValue = gv;
    }
    // The completion: EnterLocationFinalStep reaches done. Its index is found by name.
    // F24: the summary prints when the final step starts running (it may never be seen done), and again when it is done.
    for (size_t i = 0; i < kStageCount; ++i) {
        if (g_stNames[i] != "EnterLocationFinalStep") continue;
        if (g_stSnap[i].state >= 1 && !g_stSumRunning) {
            g_stSumRunning = true;
            stageSummary("final step running: EnterLocationFinalStep", clock);
        }
        if (g_stSnap[i].state == 2 && !g_stSumDone) {
            g_stSumDone = true;
            stageSummary("load complete: EnterLocationFinalStep done", clock);
        }
    }
}

// Pre-hook. The original runs with the same three arguments and its result is returned unchanged.
bool __fastcall stageDispatchHooked(void* rcx, void* rdx, float dt) {
    const F23StageFn original = reinterpret_cast<F23StageFn>(g_f23Stage.original);
    if (!original) return true;   // the trampoline is missing: let the game go on as it would without the hook
    if (rdx) {
        std::lock_guard<std::mutex> lock(g_stMutex);
        if (g_stCalls == 0) {
            g_stFirstCallMs = GetTickCount64();
            Log::get().note("load stages: dispatcher reached (first call %.1f s after install)",
                            static_cast<double>(g_stFirstCallMs - g_stInstallMs) / 1000.0);
        }
        ++g_stCalls;
        stageObserve(reinterpret_cast<uintptr_t>(rdx) - 0x20);
    }
    return original(rcx, rdx, dt);
}

// Frame thread, every frame: the one line that tells "the hook never ran" from "it ran and nothing changed": no call 30 s after install.
void loadStagesFrameTick(uint64_t nowMs) {
    if (!g_stInstalled || g_stNoCallNoted) return;
    std::lock_guard<std::mutex> lock(g_stMutex);
    if (g_stCalls == 0 && nowMs - g_stInstallMs >= 30000) {
        g_stNoCallNoted = true;
        Log::get().note("load stages: the dispatcher has not run in 30 s since install (no load has run this process, or the hook is not reached)");
    }
}

// ---- F28 (TEMPORARY, read only): the training connect probe (fix.training_connect_probe = on | off, read at launch) ---------------------------
// The ConnectToServerActivity (CTSA) and its state machine, probed at their entries. Every hook is a pre-hook: the original runs with the same
// arguments and its result is returned unchanged. Integer-argument targets forward rcx, rdx, r8, r9 and the four stack slots (entry+0x28 .. +0x48) as
// the composer fix needs. The two with dt in xmm1 (Run and SGMM) call through a shim that moves xmm2 to xmm1 and jumps to the trampoline: the same move
// the thunk makes, so the original sees what the game gave it. The shim's jump target is a slot written after the install (before the gate opens).
// Prologues verified before writing (F9Probe install). Lines: "training connect:".
namespace f28 {
constexpr uintptr_t kRunRva = 0x1D15300, kStartRva = 0x1D818D0, kStopRva = 0x1D4A710, kDtorRva = 0x1D0FBA0, kCtorRva = 0x1CFBBE0,
                    kStateRva = 0x1D7C8E0, kRirRva = 0x653C50, kDeactRva = 0x831A10, kSgmmRva = 0x1D1CDF0;
const uint8_t kRunB[8] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57};
const uint8_t kStartB[9] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9};
const uint8_t kStopB[9] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9};
const uint8_t kDtorB[10] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20};
const uint8_t kCtorB[16] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
const uint8_t kStateB[13] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00};
const uint8_t kRirB[8] = {0x48, 0x81, 0x79, 0x18, 0x00, 0x10, 0x00, 0x00};
const uint8_t kDeactB[9] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9};
const uint8_t kSgmmB[15] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20};
// v2: ActivitySet (AM) ClearAll and Remove. Prologues read from the Epic exe: whole instructions, no calls or RIP-relative operands in the stolen bytes.
constexpr uintptr_t kClearRva = 0x4C90C40, kRemoveRva = 0x4C8FAF0;
const uint8_t kClearB[20] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20};
const uint8_t kRemoveB[19] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xCA};
enum { kRun, kStart, kStop, kDtor, kCtor, kState, kRir, kDeact, kSgmm, kClear, kRemove, kCount };
const char* const kNames[kCount] = {"Run", "Start", "Stop", "Dtor", "Ctor", "State", "RIR", "Deact", "SGMM", "ClearAll", "Remove"};
}  // namespace f28

F9Probe g_f28[f28::kCount];
std::atomic<uint64_t> g_f28Calls[f28::kCount];
bool g_f28Installed = false;      // frame thread: the install ran (any probe)
uint64_t g_f28InstallMs = 0;      // frame thread
uint8_t* g_f28RunShim = nullptr;  // the shims (RWX pages, the jump slot at +9)
std::mutex g_f28Mutex;            // the CTSA list and the per-second accumulators
uint64_t g_f28Ctsa[4] = {};       // tracked CTSA objects (0 = free)
uint64_t g_f28RunSecCalls = 0, g_f28RunRcx = 0, g_f28RunRa = 0, g_f28RunTid = 0;   // per second (under the mutex)
float g_f28RunDt = 0.0f;
uint64_t g_f28RirSecCalls = 0, g_f28RirRcx = 0;
uint64_t g_f28RirRa[4] = {}, g_f28RirRaN[4] = {};   // the RA counts this second (the first four distinct)
uint64_t g_f28LastSecMs = 0, g_f28RunLastRcx = 0;
// v2: written by the game thread's hooks, read by the frame tick (atomics).
std::atomic<uint64_t> g_f28DeactMs{0}, g_f28DtorMs{0};
uint64_t g_f28ServLogged[8] = {};             // CTSA objects whose services were logged (game thread, Start)

std::string f28State(uint32_t s) { return s == 0 ? "0 (waiting)" : s == 1 ? "1 (running)" : s == 2 ? "2 (done)" : std::to_string(s); }

// The shim: movaps xmm1,xmm2; jmp qword ptr [rip+0] (the slot at +9 holds the trampoline).
uint8_t* f28MakeShim() {
    uint8_t* p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!p) return nullptr;
    const uint8_t code[9] = {0x0F, 0x28, 0xCA, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(p, code, sizeof(code));
    std::memset(p + 9, 0, 8);
    return p;
}

// One line for an entry-type probe: the object, its state, the caller and the thread. Every call.
void f28Entry(int k, uint64_t self, uint64_t ra) {
    uint32_t st = 0;
    const bool okSt = self && guardedRead(self + 0x30, &st, 4);
    char stText[24];
    if (okSt) std::snprintf(stText, sizeof(stText), "%u", static_cast<unsigned>(st));
    else std::snprintf(stText, sizeof(stText), "unread");
    Log::get().note("training connect: %s: object 0x%llX, [+0x30] %s, RA 0x%llX, tid %lu (%llu calls), at %llu ms", f28::kNames[k],
                    static_cast<unsigned long long>(self), stText, static_cast<unsigned long long>(ra), static_cast<unsigned long>(GetCurrentThreadId()),
                    static_cast<unsigned long long>(g_f28Calls[k].load()), static_cast<unsigned long long>(GetTickCount64()));
}

// v2: "0x%llX" for the log lines below.
std::string f28Hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

// v2, once per CTSA object (at Start): the owner AM at [act+8], and the service pointers at [act+0x40..+0x98]. The range holds twelve
// qwords at stride 8 (the brief said 14); they are logged as two lines.
void f28LogServices(uint64_t act) {
    for (uint64_t done : g_f28ServLogged) {
        if (done == act) return;
    }
    for (uint64_t& slot : g_f28ServLogged) {
        if (!slot) { slot = act; break; }
    }
    uint64_t owner = 0;
    const bool okO = act && guardedRead(static_cast<uintptr_t>(act) + 8, &owner, 8);
    Log::get().note("training connect: services of CTSA %s, owner AM [+8] %s", f28Hex(act).c_str(), okO ? f28Hex(owner).c_str() : "unread");
    for (int half = 0; half < 2; ++half) {
        std::string line = half == 0 ? "training connect: services [+0x40..+0x68]:" : "training connect: services [+0x70..+0x98]:";
        for (int i = 0; i < 6; ++i) {
            const uint32_t off = 0x40u + 8u * static_cast<uint32_t>(half * 6 + i);
            uint64_t v = 0;
            const bool ok = act && guardedRead(static_cast<uintptr_t>(act) + off, &v, 8);
            char b[48];
            std::snprintf(b, sizeof(b), " [+0x%02X] %s", static_cast<unsigned>(off), ok ? f28Hex(v).c_str() : "unread");
            line += b;
        }
        Log::get().note("%s", line.c_str());
    }
}

void f28Track(uint64_t act) {
    std::lock_guard<std::mutex> lock(g_f28Mutex);
    for (uint64_t& slot : g_f28Ctsa) {
        if (!slot) { slot = act; return; }
    }
}
void f28Untrack(uint64_t act) {
    std::lock_guard<std::mutex> lock(g_f28Mutex);
    for (uint64_t& slot : g_f28Ctsa) {
        if (slot == act) slot = 0;
    }
}

// ---- F30 part 1 (TEMPORARY): the Training connection stall. fix.training_connect_fix = on | off (read at launch, default on).
// A (prevent, f30HoldSgmm): SGMM in state 2 (the pending StartTutorial) with a CTSA alive in states 1-9 has its body skipped for up to 10 s.
//   The skipped body is the game's own not-ready exit (+0x1D1D1E2): for state 2 it returns al = (state == 0) = 0, which the hook returns.
// B (recover, f30RecoverTick, from the owner AM's Update after the original returns): after a CTSA is destroyed and a load is pending (a Deactivate
//   after the destroy), with the IDProvider link down, the owner AM gets a new CTSA the game's way (+0x4C8E500, +0x4C907A0, +0x4C8F960, +0x4C8EA60),
//   and StartPending (+0x4C90B90) once if no Start follows within 1 s. Two attempts per pending load. The AM owns the objects: nothing here frees one.
namespace f30 {
constexpr uint64_t kFirstAttemptMs = 8000, kRecoverQuietMs = 10000, kStartWaitMs = 1000;
constexpr uint32_t kAttempts = 2;
constexpr uintptr_t kUpdateRva = 0x4C8EEA0, kSetInitRva = 0x4C8E500, kSetInsertRva = 0x4C907A0, kInstantiateRva = 0x4C8F960,
                    kSetFreeRva = 0x4C8EA60, kStartPendingRva = 0x4C90B90, kTypeKeyRva = 0x5F7D7F8;
const uint8_t kUpdateB[11] = {0x41, 0x56, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x89, 0x5C, 0x24, 0x70};
const uint8_t kSetInitB[6] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
const uint8_t kSetInsertB[8] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74};
const uint8_t kInstantiateB[8] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74};
const uint8_t kSetFreeB[8] = {0x48, 0x8B, 0xC1, 0x48, 0x8B, 0x49, 0x20, 0x48};
const uint8_t kStartPendingB[8] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C};
using SetInitFn = void(__fastcall*)(void*, uint32_t);
using SetInsertFn = void(__fastcall*)(void*, uint64_t);
using InstantiateFn = void(__fastcall*)(void*, void*);
using SetFreeFn = void(__fastcall*)(void*);
using StartPendingFn = void(__fastcall*)(void*);
}  // namespace f30

bool g_f30FixOn = false;            // fix.training_connect_fix, read at install
uintptr_t g_f30Base = 0;            // EliteDangerous64.exe's base (install)
bool g_f30CalleesOk = false;        // the Update and the callees' entry bytes matched at install; recovery refuses otherwise
F9Probe g_f30Update;
uint8_t* g_f30UpdateShim = nullptr;
// Part B. Game thread only: the Ctor, Dtor, Remove, Start, Deactivate, SGMM and Update hooks all run there.
uint64_t g_f30Owner = 0;            // the owner AM of the last CTSA built ([act+8] after its ctor)
uint64_t g_f30NewAct = 0;           // the last CTSA built
bool g_f30Dead = false;             // a CTSA was destroyed (its dtor, or the owner's Remove) and none was built since
bool g_f30Pending = false;          // a Deactivate came after that destroy: a load is pending
bool g_f30GaveUp = false, g_f30RefuseLogged = false, g_f30StartPendingDone = false;
uint32_t g_f30Attempts = 0;
uint64_t g_f30AttemptMs = 0, g_f30AttemptCtor = 0, g_f30AttemptStart = 0, g_f30PendingCtor = 0;
uint32_t g_f30NewState0 = 0xFFFFFFFFu;
uint32_t g_f32NewStates = 0;   // F32: the states the new CTSA went through since the attempt (bit = state)
bool g_f32Shortcut = false;    // F32: the attempt went 1 -> 10 with none of states 2..8 between
std::atomic<uint64_t> g_f30CtorSeq{0}, g_f30StartSeq{0};

// The tracked CTSA objects (f28Track/f28Untrack) include this one.
bool f28Tracked(uint64_t act) {
    if (!act) return false;
    std::lock_guard<std::mutex> lock(g_f28Mutex);
    for (uint64_t slot : g_f28Ctsa) {
        if (slot == act) return true;
    }
    return false;
}

// Forwards the eight argument slots to the trampoline. Integer arguments only: the callee reads them as they came.
using F28Fn = uint64_t(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t f28Forward(int k, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    const F28Fn orig = reinterpret_cast<F28Fn>(g_f28[k].original);
    if (!orig) return 0;
    return orig(a1, a2, a3, a4, s5, s6, s7, s8);
}

uint64_t __fastcall f28StartHooked(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    g_f28Calls[f28::kStart].fetch_add(1);
    f28Entry(f28::kStart, a1, reinterpret_cast<uint64_t>(_ReturnAddress()));
    f28LogServices(a1);   // v2: once per object: would a retry instance have its services?
    g_f30StartSeq.fetch_add(1);
    return f28Forward(f28::kStart, a1, a2, a3, a4, s5, s6, s7, s8);
}
uint64_t __fastcall f28StopHooked(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    g_f28Calls[f28::kStop].fetch_add(1);
    f28Entry(f28::kStop, a1, reinterpret_cast<uint64_t>(_ReturnAddress()));
    return f28Forward(f28::kStop, a1, a2, a3, a4, s5, s6, s7, s8);
}
uint64_t __fastcall f28DtorHooked(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    g_f28Calls[f28::kDtor].fetch_add(1);
    f28Entry(f28::kDtor, a1, reinterpret_cast<uint64_t>(_ReturnAddress()));
    f28Untrack(a1);   // the object is freed by the dtor: stop reading it
    g_f28DtorMs.store(GetTickCount64());   // v2: the SGMM lines follow for 120 s after a destruction
    g_f30Dead = true;                      // F30 part B: destroyed, and no CTSA built since (until a ctor)
    return f28Forward(f28::kDtor, a1, a2, a3, a4, s5, s6, s7, s8);
}
uint64_t __fastcall f28CtorHooked(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    g_f28Calls[f28::kCtor].fetch_add(1);
    f28Entry(f28::kCtor, a1, reinterpret_cast<uint64_t>(_ReturnAddress()));
    const uint64_t r = f28Forward(f28::kCtor, a1, a2, a3, a4, s5, s6, s7, s8);
    f28Track(a1);   // the object exists once the constructor has returned
    uint64_t owner = 0;   // v2: the base ctor stores the owner AM at [act+8]; read it after the original returns
    const bool okO = guardedRead(static_cast<uintptr_t>(a1) + 8, &owner, 8);
    Log::get().note("training connect: CTSA ctor returned: act %s, owner AM [+8] %s (passed %s), key %s", f28Hex(a1).c_str(),
                    okO ? f28Hex(owner).c_str() : "unread", f28Hex(a2).c_str(), f28Hex(a3).c_str());
    g_f30Owner = okO ? owner : 0;   // F30 part B: the owner AM for the recovery's Update gate
    g_f30NewAct = a1;
    g_f30Dead = false;
    g_f30CtorSeq.fetch_add(1);
    return r;
}
uint64_t __fastcall f28DeactHooked(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    g_f28Calls[f28::kDeact].fetch_add(1);
    f28Entry(f28::kDeact, a1, reinterpret_cast<uint64_t>(_ReturnAddress()));   // IrcGameClient::Deactivate: the menu closes here
    g_f28DeactMs.store(GetTickCount64());   // v2: the IDProvider link line runs while an SGMM wait follows
    if (g_f30Dead) {                        // F30 part B: the menu closed after a CTSA was destroyed: a load is pending
        g_f30Pending = true;
        g_f30GaveUp = false;
        g_f30RefuseLogged = false;
        g_f30Attempts = 0;
        g_f30AttemptMs = 0;
        g_f30PendingCtor = g_f30CtorSeq.load();
        Log::get().note("training connect fix: Deactivate after a CTSA was destroyed (%llu s before): a load is pending; the recovery waits for the link to be down and 10 s",
                        static_cast<unsigned long long>((GetTickCount64() - g_f28DtorMs.load()) / 1000));
    }
    return f28Forward(f28::kDeact, a1, a2, a3, a4, s5, s6, s7, s8);
}
// State setter: (act, new state, RA, tid) on every call.
uint64_t __fastcall f28StateHooked(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    g_f28Calls[f28::kState].fetch_add(1);
    if (g_f30AttemptMs && a1 == g_f30NewAct) g_f32NewStates |= 1u << (static_cast<uint32_t>(a2) & 31u);   // F32: the new CTSA's states
    Log::get().note("training connect: state setter: act 0x%llX, new state %s, RA 0x%llX, tid %lu", static_cast<unsigned long long>(a1),
                    f28State(static_cast<uint32_t>(a2)).c_str(), static_cast<unsigned long long>(reinterpret_cast<uint64_t>(_ReturnAddress())),
                    static_cast<unsigned long>(GetCurrentThreadId()));
    return f28Forward(f28::kState, a1, a2, a3, a4, s5, s6, s7, s8);
}
// ReadyToAllocIDs: counted per second (the frame tick prints the count and the top callers).
uint64_t __fastcall f28RirHooked(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    g_f28Calls[f28::kRir].fetch_add(1);
    const uint64_t ra = reinterpret_cast<uint64_t>(_ReturnAddress());
    {
        std::lock_guard<std::mutex> lock(g_f28Mutex);
        ++g_f28RirSecCalls;
        g_f28RirRcx = a1;
        bool seen = false;
        for (int i = 0; i < 4 && !seen; ++i) {
            if (g_f28RirRa[i] == ra) { ++g_f28RirRaN[i]; seen = true; }
        }
        for (int i = 0; i < 4 && !seen; ++i) {
            if (!g_f28RirRa[i]) { g_f28RirRa[i] = ra; g_f28RirRaN[i] = 1; seen = true; }
        }
    }
    return f28Forward(f28::kRir, a1, a2, a3, a4, s5, s6, s7, s8);
}
// Run (dt in xmm1, via the shim): per second, the count, rcx, [rcx+0x30], dt, RA and tid; a new rcx is logged at once.
using F28ShimFn = uint64_t(__fastcall*)(void*, void*, float);
uint64_t __fastcall f28RunHooked(void* act, float dt) {
    g_f28Calls[f28::kRun].fetch_add(1);
    const uint64_t ra = reinterpret_cast<uint64_t>(_ReturnAddress());
    {
        std::lock_guard<std::mutex> lock(g_f28Mutex);
        ++g_f28RunSecCalls;
        g_f28RunRcx = reinterpret_cast<uint64_t>(act);
        g_f28RunRa = ra;
        g_f28RunTid = GetCurrentThreadId();
        g_f28RunDt = dt;
        if (g_f28RunRcx != g_f28RunLastRcx) {
            g_f28RunLastRcx = g_f28RunRcx;
            Log::get().note("training connect: Run: a new instance 0x%llX (RA 0x%llX, tid %lu)", static_cast<unsigned long long>(g_f28RunRcx),
                            static_cast<unsigned long long>(ra), static_cast<unsigned long>(g_f28RunTid));
        }
    }
    return reinterpret_cast<F28ShimFn>(g_f28RunShim)(act, static_cast<uint8_t*>(act) + 0x20, dt);   // rdx = act+0x20 (the thunk's value)
}
// ---- F32 (probe v4, read only; behind fix.training_connect_probe). SGMM Run (+0x1D1CDF0) is no longer hooked: the F31 flight never called it.
namespace f32 {
constexpr uintptr_t kStartedInsertRva = 0x4C8E020, kPhaseRva = 0x1D1D220;
// The started-array insert: the 18 bytes end on an instruction boundary (sub rsp,0x40).
const uint8_t kStartedInsertB[18] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40};
// The +0x1D1D220 function: 16 bytes, whole instructions. The brief's 15 would split `lea rbp,[rsp-0x27]`.
const uint8_t kPhaseB[16] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xD9};
}  // namespace f32

bool g_f32ProbeOn = false;   // set at install from fix.training_connect_probe
F9Probe g_f32Started, g_f32Phase;
uint8_t* g_f32PhaseShim = nullptr;
std::atomic<uint64_t> g_f32PhaseCalls{0}, g_f32PhaseTotal{0}, g_f32PhaseSelf{0};

// The activity's name, read without calling it. Vtable slot 1 is a stub `lea rax,[rip+rel32]; ret` (48 8D 05 rel32 C3) whose target is the
// string. "?" when the shape differs or a read faults; at most 64 printable characters.
std::string f32ActName(uint64_t act) {
    uint64_t vt = 0, stub = 0;
    if (!act || !guardedRead(static_cast<uintptr_t>(act), &vt, 8) || !vt) return "?";
    if (!guardedRead(static_cast<uintptr_t>(vt) + 8, &stub, 8) || !stub) return "?";
    uint8_t b[8] = {};
    if (!guardedRead(static_cast<uintptr_t>(stub), b, 8)) return "?";
    if (b[0] != 0x48 || b[1] != 0x8D || b[2] != 0x05 || b[7] != 0xC3) return "?";
    int32_t rel = 0;
    std::memcpy(&rel, b + 3, 4);
    const uint64_t str = stub + 7 + static_cast<int64_t>(rel);
    std::string out;
    for (uint64_t i = 0; i < 64; ++i) {
        char c = 0;
        if (!guardedRead(static_cast<uintptr_t>(str + i), &c, 1)) return "?";
        if (c == 0) break;
        if (c < 32 || c > 126) return "?";
        out.push_back(c);
    }
    return out.empty() ? "?" : out;
}

// The started-array insert (AM rcx, rdx, r8 = &{key, act}): the act is at r8+8 (the caller passes r8 = node+0x10 and the StartPending loop
// takes the act from node+0x18). Logged before the original runs, so a fault inside it still leaves the line.
using F32Fn8 = uint64_t(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t __fastcall f32StartedHooked(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    uint64_t key = 0, act = 0;
    const bool okK = a3 && guardedRead(static_cast<uintptr_t>(a3), &key, 8);
    const bool okA = a3 && guardedRead(static_cast<uintptr_t>(a3) + 8, &act, 8);
    Log::get().note("training connect: started insert: AM %s, act %s (name %s), key [r8] %s, RA 0x%llX, tid %lu, at %llu ms", f28Hex(a1).c_str(),
                    okA ? f28Hex(act).c_str() : "unread", okA ? f32ActName(act).c_str() : "?", okK ? f28Hex(key).c_str() : "unread",
                    static_cast<unsigned long long>(reinterpret_cast<uint64_t>(_ReturnAddress())), static_cast<unsigned long>(GetCurrentThreadId()),
                    static_cast<unsigned long long>(GetTickCount64()));
    const F32Fn8 original = reinterpret_cast<F32Fn8>(g_f32Started.original);
    return original ? original(a1, a2, a3, a4, s5, s6, s7, s8) : 0;
}

// +0x1D1D220 (dt in xmm1, via the shim, as the SGMM shim was): the calls and [this+0x138] are logged each second by f32Second.
uint64_t __fastcall f32PhaseHooked(void* self, float dt) {
    g_f32PhaseCalls.fetch_add(1);
    g_f32PhaseTotal.fetch_add(1);
    g_f32PhaseSelf.store(reinterpret_cast<uint64_t>(self));
    return reinterpret_cast<F28ShimFn>(g_f32PhaseShim)(self, nullptr, dt);
}

// The owner-of-Update counts: per AM, calls this second (16 slots; the rest counted as "more").
struct F32Am {
    uint64_t am = 0;
    uint64_t calls = 0;
};
F32Am g_f32Am[16];
uint64_t g_f32AmOther = 0;
std::mutex g_f32AmMutex;
void f32CountUpdate(void* am) {
    if (!g_f32ProbeOn) return;
    const uint64_t a = reinterpret_cast<uint64_t>(am);
    std::lock_guard<std::mutex> lock(g_f32AmMutex);
    for (F32Am& e : g_f32Am) {
        if (e.am == a) {
            ++e.calls;
            return;
        }
    }
    for (F32Am& e : g_f32Am) {
        if (!e.am) {
            e.am = a;
            e.calls = 1;
            return;
        }
    }
    ++g_f32AmOther;
}

// Frame thread, once a second: the +0x1D1D220 line, and for the 120 s after a Deactivate the top 8 AMs by Update calls this second.
void f32Second(uint64_t nowMs) {
    if (!g_f32ProbeOn) return;
    if (g_f32Phase.original) {
        const uint64_t calls = g_f32PhaseCalls.exchange(0);
        const uint64_t self = g_f32PhaseSelf.load();
        uint32_t st = 0;
        const bool okS = self && guardedRead(static_cast<uintptr_t>(self) + 0x138, &st, 4);
        Log::get().note("training connect: +0x1D1D220: %llu calls this second (%llu in all), this %s, [this+0x138] %s",
                        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(g_f32PhaseTotal.load()),
                        self ? f28Hex(self).c_str() : "none", okS ? f28State(st).c_str() : "unread");
    }
    F32Am snap[16];
    uint64_t other = 0;
    {
        std::lock_guard<std::mutex> lock(g_f32AmMutex);
        for (int i = 0; i < 16; ++i) {
            snap[i] = g_f32Am[i];
            g_f32Am[i].calls = 0;
        }
        other = g_f32AmOther;
        g_f32AmOther = 0;
    }
    const uint64_t deactMs = g_f28DeactMs.load();
    if (!deactMs || nowMs < deactMs || nowMs - deactMs > 120000) return;
    uint64_t total = 0;
    int used = 0;
    for (const F32Am& e : snap) {
        if (e.am && e.calls) {
            total += e.calls;
            ++used;
        }
    }
    if (!used && !other) return;
    bool taken[16] = {};
    std::string line;
    for (int k = 0; k < 8; ++k) {
        int best = -1;
        for (int i = 0; i < 16; ++i) {
            if (taken[i] || !snap[i].am || !snap[i].calls) continue;
            if (best < 0 || snap[i].calls > snap[best].calls) best = i;
        }
        if (best < 0) break;
        taken[best] = true;
        char b[64];
        std::snprintf(b, sizeof(b), "%s%s x%llu", line.empty() ? "" : ", ", f28Hex(snap[best].am).c_str(),
                      static_cast<unsigned long long>(snap[best].calls));
        line += b;
    }
    Log::get().note("training connect: AM Update calls this second: %llu in all across %d AMs (%llu more not tracked); top 8: %s",
                    static_cast<unsigned long long>(total), used, static_cast<unsigned long long>(other), line.empty() ? "none" : line.c_str());
}

// Install (F32): the started insert and the +0x1D1D220 function. Each refusal is one line; the game is not touched by a refused one.
void f32InstallProbes(uintptr_t base) {
    size_t got = 0;
    std::string why;
    const F9ProbeSpec s1{"native-on-foot-started-insert", f32::kStartedInsertRva, f32::kStartedInsertB, sizeof(f32::kStartedInsertB), 18,
                         reinterpret_cast<void*>(&f32StartedHooked), "training connect"};
    if (installF9ProbeInto(g_f32Started, s1, base, &got, &why))
        Log::get().note("training connect: started insert installed at EliteDangerous64.exe+0x%llX, stolen=%zu bytes, prologue 18 bytes verified (read only)",
                        static_cast<unsigned long long>(f32::kStartedInsertRva), got);
    else
        Log::get().note("training connect: started insert NOT installed at EliteDangerous64.exe+0x%llX -- %s. The game was not touched by it.",
                        static_cast<unsigned long long>(f32::kStartedInsertRva), why.c_str());
    g_f32PhaseShim = f28MakeShim();
    const F9ProbeSpec s2{"native-on-foot-phase", f32::kPhaseRva, f32::kPhaseB, sizeof(f32::kPhaseB), 16, reinterpret_cast<void*>(&f32PhaseHooked),
                         "training connect"};
    if (g_f32PhaseShim && installF9ProbeInto(g_f32Phase, s2, base, &got, &why)) {
        std::memcpy(g_f32PhaseShim + 9, &g_f32Phase.original, 8);
        Log::get().note("training connect: +0x1D1D220 installed at EliteDangerous64.exe+0x%llX, stolen=%zu bytes, prologue 16 bytes verified (read only)",
                        static_cast<unsigned long long>(f32::kPhaseRva), got);
    } else {
        Log::get().note("training connect: +0x1D1D220 NOT installed at EliteDangerous64.exe+0x%llX -- %s. The game was not touched by it.",
                        static_cast<unsigned long long>(f32::kPhaseRva), why.c_str());
    }
}


// v2: ActivitySet ClearAll (AM) and Remove (AM, act): read only, every call, with the caller and the time.
uint64_t __fastcall f28ClearHooked(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    g_f28Calls[f28::kClear].fetch_add(1);
    Log::get().note("training connect: ActivitySet ClearAll: AM %s, RA 0x%llX, tid %lu, at %llu ms", f28Hex(a1).c_str(),
                    static_cast<unsigned long long>(reinterpret_cast<uint64_t>(_ReturnAddress())), static_cast<unsigned long>(GetCurrentThreadId()),
                    static_cast<unsigned long long>(GetTickCount64()));
    return f28Forward(f28::kClear, a1, a2, a3, a4, s5, s6, s7, s8);
}
uint64_t __fastcall f28RemoveHooked(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    g_f28Calls[f28::kRemove].fetch_add(1);
    if (g_f30FixOn && f28Tracked(a2)) {   // F30 part B: the owner removed a tracked CTSA (the reconcile): dead for the recovery
        f28Untrack(a2);
        g_f28DtorMs.store(GetTickCount64());
        g_f30Dead = true;
        Log::get().note("training connect fix: the owner AM removed the tracked CTSA %s (AM %s): it is dead for the recovery", f28Hex(a2).c_str(),
                        f28Hex(a1).c_str());
    }
    uint32_t st = 0;
    const bool okSt = a2 && guardedRead(static_cast<uintptr_t>(a2) + 0x30, &st, 4);
    Log::get().note("training connect: ActivitySet Remove: AM %s, act %s, [act+0x30] %s, RA 0x%llX, tid %lu, at %llu ms", f28Hex(a1).c_str(),
                    f28Hex(a2).c_str(), okSt ? f28State(st).c_str() : "unread",
                    static_cast<unsigned long long>(reinterpret_cast<uint64_t>(_ReturnAddress())), static_cast<unsigned long>(GetCurrentThreadId()),
                    static_cast<unsigned long long>(GetTickCount64()));
    if (g_f32ProbeOn) Log::get().note("training connect: ActivitySet Remove: act %s name %s", f28Hex(a2).c_str(), f32ActName(a2).c_str());   // F32
    return f28Forward(f28::kRemove, a1, a2, a3, a4, s5, s6, s7, s8);
}

// ---- F30 part 1, part B: the recovery. Runs from the owner AM's Update after the original has returned.
// The link is IsConnected as the RE gives it: down when the pointer at idp+0x28 is null or [ptr+0x60] < 3, idp being ReadyToAllocIDs' rcx.
bool f30LinkDown(const char** why) {
    uint64_t idp = 0;
    {
        std::lock_guard<std::mutex> lock(g_f28Mutex);
        idp = g_f28RirRcx;
    }
    if (!idp) { *why = "no ReadyToAllocIDs call seen, so no IDProvider"; return false; }
    uint64_t p = 0;
    uint32_t v = 0;
    if (!guardedRead(static_cast<uintptr_t>(idp) + 0x28, &p, 8)) { *why = "the IDProvider could not be read: refused"; return false; }
    if (!p) { *why = "link down: the pointer at idp+0x28 is null"; return true; }
    if (!guardedRead(static_cast<uintptr_t>(p) + 0x60, &v, 4)) { *why = "the link object could not be read: refused"; return false; }
    if (v < 3) { *why = "link down: [link+0x60] is below 3"; return true; }
    *why = "link up";
    return false;
}

// The game's own sequence, in SEH: the set's init (count 1), the key insert, Instantiate(AM, set), the set's free. No C++ objects in this frame.
bool f30InstantiateSafe(uintptr_t base, void* am, uint8_t* actSet) {
    __try {
        reinterpret_cast<f30::SetInitFn>(base + f30::kSetInitRva)(actSet, 1u);
        reinterpret_cast<f30::SetInsertFn>(base + f30::kSetInsertRva)(actSet, static_cast<uint64_t>(base + f30::kTypeKeyRva));
        reinterpret_cast<f30::InstantiateFn>(base + f30::kInstantiateRva)(am, actSet);
        reinterpret_cast<f30::SetFreeFn>(base + f30::kSetFreeRva)(actSet);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool f30StartPendingSafe(uintptr_t base, void* am) {
    __try {
        reinterpret_cast<f30::StartPendingFn>(base + f30::kStartPendingRva)(am);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void f30Attempt(void* am, uint64_t ms) {
    ++g_f30Attempts;
    g_f30AttemptMs = ms;
    g_f30AttemptCtor = g_f30CtorSeq.load();
    g_f30AttemptStart = g_f30StartSeq.load();
    g_f30StartPendingDone = false;
    g_f30NewState0 = 0xFFFFFFFFu;
    g_f32NewStates = 0;
    g_f32Shortcut = false;
    alignas(16) uint8_t actSet[0x100];
    std::memset(actSet, 0, sizeof(actSet));
    const bool ok = f30InstantiateSafe(g_f30Base, am, actSet);
    Log::get().note("training connect fix: recovery attempt %u of %u: owner AM %s, set built (count 1, the CTSA type key), Instantiate %s",
                    static_cast<unsigned>(g_f30Attempts), static_cast<unsigned>(f30::kAttempts), f28Hex(reinterpret_cast<uint64_t>(am)).c_str(),
                    ok ? "returned" : "FAULTED (SEH): nothing further is called");
}

void f30RecoverTick(void* am, uint64_t ms) {
    if (!g_f30Pending || g_f30GaveUp || !g_f30Owner) return;
    if (reinterpret_cast<uint64_t>(am) != g_f30Owner) return;
    const uint64_t ctorNow = g_f30CtorSeq.load();
    const uint64_t dtorMs = g_f28DtorMs.load();
    if (g_f30AttemptMs == 0) {
        if (ctorNow != g_f30PendingCtor) {
            Log::get().note("training connect fix: the game built a CTSA on its own after the menu exit; no recovery is needed");
            g_f30Pending = false;
            return;
        }
        // F32: attempt 1 eight seconds after the destruction with a load pending, whatever the link: the link rule is attempt 2's only.
        if (!g_f30Dead || ms < dtorMs || ms - dtorMs < f30::kFirstAttemptMs) return;
        Log::get().note("training connect fix: the CTSA was destroyed %llu s ago with a load pending: attempt 1 (the link is not checked for it)",
                        static_cast<unsigned long long>((ms - dtorMs) / 1000));
        f30Attempt(am, ms);
        return;
    }
    // Progress is a session: states 7 or 8 on the new CTSA since the attempt (the F31 recovery reached them).
    if (g_f32NewStates & ((1u << 7) | (1u << 8))) {
        Log::get().note("training connect fix: progress: the new CTSA %s reached states 7-8 (sessions) after attempt %u; recovery done",
                        f28Hex(g_f30NewAct).c_str(), static_cast<unsigned>(g_f30Attempts));
        g_f30Pending = false;
        return;
    }
    // F32: the shortcut. Init went 1 -> 10 with none of states 2..8 between: the "already connected" path, so no sessions were made.
    if (!g_f32Shortcut && (g_f32NewStates & (1u << 10)) && !(g_f32NewStates & 0x1FCu)) {
        g_f32Shortcut = true;
        Log::get().note("training connect fix: shortcut: the new CTSA %s went 1 -> 10 with no states 2-8 between: no sessions were made (attempt %u)",
                        f28Hex(g_f30NewAct).c_str(), static_cast<unsigned>(g_f30Attempts));
    }
    if (!g_f30StartPendingDone && ms - g_f30AttemptMs >= f30::kStartWaitMs && g_f30StartSeq.load() == g_f30AttemptStart) {
        g_f30StartPendingDone = true;
        const bool ok = f30StartPendingSafe(g_f30Base, am);
        Log::get().note("training connect fix: no Start within 1 s of attempt %u: StartPending (+0x4C90B90) on the owner AM %s",
                        static_cast<unsigned>(g_f30Attempts), ok ? "returned" : "FAULTED (SEH)");
    }
    const bool timedOut = ms - g_f30AttemptMs >= f30::kRecoverQuietMs;
    if (!g_f32Shortcut && !timedOut) return;
    if (g_f30Attempts >= f30::kAttempts) {
        if (timedOut) {
            Log::get().note("training connect fix: gave up after %u attempts with no session in 10 s each; the load needs a restart",
                            static_cast<unsigned>(g_f30Attempts));
            g_f30GaveUp = true;
            g_f30Pending = false;
        }
        return;
    }
    // Attempt 2: the link must be down (the F30 rule, kept for this attempt only).
    const char* why = "";
    if (!f30LinkDown(&why)) {
        if (!g_f30RefuseLogged) {
            g_f30RefuseLogged = true;
            Log::get().note("training connect fix: attempt 2 held (%s after attempt %u): it needs the link down", why, static_cast<unsigned>(g_f30Attempts));
        }
        return;
    }
    Log::get().note("training connect fix: attempt 2 (%s): %s", g_f32Shortcut ? "the shortcut" : "no session in 10 s", why);
    f30Attempt(am, ms);
}

// The owner AM's Update: the original runs first with the same arguments (the shim moves dt to xmm1, as the SGMM shim did).
// F32: each call is counted per AM for the probe; the recovery needs the fix and the callee checks.
uint64_t __fastcall f30UpdateHooked(void* am, float dt) {
    const uint64_t r = reinterpret_cast<F28ShimFn>(g_f30UpdateShim)(am, nullptr, dt);
    f32CountUpdate(am);
    if (g_f30FixOn && g_f30CalleesOk && g_f30Pending) f30RecoverTick(am, GetTickCount64());
    return r;
}

// Install (F30 part B; F32 counts): the Update and the callees' entry bytes must match the RE's. Recovery needs the fix and the bytes; the
// per-AM counts need only the probe.
void f30InstallRecovery(uintptr_t base, bool probeOn) {
    g_f30Base = base;
    if (!g_f30FixOn && !probeOn) {
        Log::get().note("training connect fix: off (fix.training_connect_fix = off) and the probe off: no hold, no recovery, no counts");
        return;
    }
    struct Chk {
        const char* name;
        uintptr_t rva;
        const uint8_t* b;
        size_t n;
    };
    const Chk chks[] = {
        {"SetInit", f30::kSetInitRva, f30::kSetInitB, sizeof(f30::kSetInitB)},
        {"SetInsert", f30::kSetInsertRva, f30::kSetInsertB, sizeof(f30::kSetInsertB)},
        {"Instantiate", f30::kInstantiateRva, f30::kInstantiateB, sizeof(f30::kInstantiateB)},
        {"SetFree", f30::kSetFreeRva, f30::kSetFreeB, sizeof(f30::kSetFreeB)},
        {"StartPending", f30::kStartPendingRva, f30::kStartPendingB, sizeof(f30::kStartPendingB)},
    };
    bool ok = true;
    for (const Chk& c : chks) {
        uint8_t got[8] = {};
        const bool read = guardedRead(base + c.rva, got, c.n);
        if (!read || std::memcmp(got, c.b, c.n) != 0) {
            ok = false;
            if (g_f30FixOn)
                Log::get().note("training connect fix: %s at EliteDangerous64.exe+0x%llX does not match the RE's entry bytes: recovery refused (the hold is gone; the counts are unaffected)",
                                c.name, static_cast<unsigned long long>(c.rva));
        }
    }
    g_f30CalleesOk = ok;
    if (!probeOn && !(g_f30FixOn && ok)) return;
    g_f30UpdateShim = f28MakeShim();
    const F9ProbeSpec s{"AM Update", f30::kUpdateRva, f30::kUpdateB, sizeof(f30::kUpdateB), 11, reinterpret_cast<void*>(&f30UpdateHooked),
                        g_f30FixOn ? "training connect fix" : "training connect"};
    size_t got = 0;
    std::string why;
    if (g_f30UpdateShim && installF9ProbeInto(g_f30Update, s, base, &got, &why)) {
        std::memcpy(g_f30UpdateShim + 9, &g_f30Update.original, 8);
        Log::get().note("training connect: AM Update installed at EliteDangerous64.exe+0x%llX, stolen=%zu bytes (per-AM counts%s)",
                        static_cast<unsigned long long>(f30::kUpdateRva), got, g_f30FixOn && ok ? "; the recovery runs after the original returns" : "");
    } else {
        g_f30CalleesOk = false;
        Log::get().note("training connect: AM Update NOT installed (%s): the recovery and the per-AM counts are off", why.c_str());
    }
}

// Install: VR only (the probes are read at launch with the key; a refusal is one line each). The shims are built before their hooks, so the
// gate can open with the jump slot set.
void installF28ProbesOnce(uintptr_t base) {
    const bool probeOn = Config::get().getString("fix.training_connect_probe", "on") != "off";
    g_f30FixOn = Config::get().getString("fix.training_connect_fix", "on") != "off";
    if (!probeOn && !g_f30FixOn) {
        Log::get().note("training connect: probes off (fix.training_connect_probe = off) and the fix off (fix.training_connect_fix = off)");
        return;
    }
    if (!probeOn) {
        Log::get().note("training connect: fix.training_connect_probe = off, but the fix needs the tracking hooks (CTSA ctor, dtor, Remove, Start, Deactivate, SGMM): installing them");
    }
    g_f28RunShim = f28MakeShim();
    const auto install = [&](int k, uintptr_t rva, const uint8_t* prologue, size_t bytes, size_t stolen, void* cb) {
        const F9ProbeSpec s{f28::kNames[k], rva, prologue, bytes, stolen, cb, "training connect"};
        size_t got = 0;
        std::string why;
        if (installF9ProbeInto(g_f28[k], s, base, &got, &why)) {
            g_f28Installed = true;
            Log::get().note("training connect: %s installed at EliteDangerous64.exe+0x%llX, stolen=%zu bytes, prologue %zu bytes verified (read only)",
                            f28::kNames[k], static_cast<unsigned long long>(rva), got, bytes);
        } else {
            Log::get().note("training connect: %s NOT installed at EliteDangerous64.exe+0x%llX -- %s. The game was not touched by it.", f28::kNames[k],
                            static_cast<unsigned long long>(rva), why.c_str());
        }
    };
    install(f28::kRun, f28::kRunRva, f28::kRunB, sizeof(f28::kRunB), 8, reinterpret_cast<void*>(&f28RunHooked));
    install(f28::kStart, f28::kStartRva, f28::kStartB, sizeof(f28::kStartB), 9, reinterpret_cast<void*>(&f28StartHooked));
    install(f28::kStop, f28::kStopRva, f28::kStopB, sizeof(f28::kStopB), 9, reinterpret_cast<void*>(&f28StopHooked));
    install(f28::kDtor, f28::kDtorRva, f28::kDtorB, sizeof(f28::kDtorB), 10, reinterpret_cast<void*>(&f28DtorHooked));
    install(f28::kCtor, f28::kCtorRva, f28::kCtorB, sizeof(f28::kCtorB), 10, reinterpret_cast<void*>(&f28CtorHooked));
    install(f28::kState, f28::kStateRva, f28::kStateB, sizeof(f28::kStateB), 13, reinterpret_cast<void*>(&f28StateHooked));
    install(f28::kRir, f28::kRirRva, f28::kRirB, sizeof(f28::kRirB), 8, reinterpret_cast<void*>(&f28RirHooked));
    install(f28::kDeact, f28::kDeactRva, f28::kDeactB, sizeof(f28::kDeactB), 9, reinterpret_cast<void*>(&f28DeactHooked));
    install(f28::kClear, f28::kClearRva, f28::kClearB, sizeof(f28::kClearB), 20, reinterpret_cast<void*>(&f28ClearHooked));
    install(f28::kRemove, f28::kRemoveRva, f28::kRemoveB, sizeof(f28::kRemoveB), 19, reinterpret_cast<void*>(&f28RemoveHooked));
    // The shims' jump slots: the trampolines (installed above), written before the probes can run through them.
    if (g_f28RunShim && g_f28[f28::kRun].original) std::memcpy(g_f28RunShim + 9, &g_f28[f28::kRun].original, 8);
    g_f32ProbeOn = probeOn;
    f30InstallRecovery(base, probeOn);   // F30 part B and the F32 counts: the AM Update pre-hook and its callee checks
    if (probeOn) f32InstallProbes(base);   // F32: the started insert and the +0x1D1D220 function (read only)
    g_f28InstallMs = GetTickCount64();
}

// Frame thread, every frame: the one-second lines. Run and RIR print their per-second counts (every second while they have calls, else one
// zero line every 30 s so "never ran" and "ran with no calls" differ); the CTSA objects are read directly (SEH) every second.
void f28FrameTick(uint64_t nowMs) {
    if (!g_f28Installed) return;
    if (g_f28LastSecMs && nowMs - g_f28LastSecMs < 1000) return;
    g_f28LastSecMs = nowMs;
    f32Second(nowMs);   // F32: the +0x1D1D220 calls and the per-AM Update counts, each second
    uint64_t runN = 0, rirN = 0, ra[4] = {}, raN[4] = {}, runRcx = 0, runRa = 0, runTid = 0, rirRcx = 0;
    float runDt = 0.0f;
    uint64_t ctsa[4] = {};
    {
        std::lock_guard<std::mutex> lock(g_f28Mutex);
        runN = g_f28RunSecCalls; runRcx = g_f28RunRcx; runRa = g_f28RunRa; runTid = g_f28RunTid; runDt = g_f28RunDt;
        rirN = g_f28RirSecCalls; rirRcx = g_f28RirRcx;
        for (int i = 0; i < 4; ++i) { ra[i] = g_f28RirRa[i]; raN[i] = g_f28RirRaN[i]; ctsa[i] = g_f28Ctsa[i]; }
        g_f28RunSecCalls = 0; g_f28RirSecCalls = 0;
        for (int i = 0; i < 4; ++i) { g_f28RirRa[i] = 0; g_f28RirRaN[i] = 0; }
    }
    static uint64_t lastZeroMs = 0;
    if (runN || rirN || nowMs - lastZeroMs >= 30000) {
        if (!runN && !rirN) lastZeroMs = nowMs;
        uint32_t runSt = 0;
        uint64_t rir18 = 0, rir20 = 0;
        const bool okRunSt = runRcx && guardedRead(static_cast<uintptr_t>(runRcx) + 0x30, &runSt, 4);
        const bool okRir18 = rirRcx && guardedRead(static_cast<uintptr_t>(rirRcx) + 0x18, &rir18, 8);
        const bool okRir20 = rirRcx && guardedRead(static_cast<uintptr_t>(rirRcx) + 0x20, &rir20, 8);
        char rirA[24], rirB[24], runStText[24];
        if (okRir18) std::snprintf(rirA, sizeof(rirA), "0x%llX", static_cast<unsigned long long>(rir18)); else std::snprintf(rirA, sizeof(rirA), "unread");
        if (okRir20) std::snprintf(rirB, sizeof(rirB), "0x%llX", static_cast<unsigned long long>(rir20)); else std::snprintf(rirB, sizeof(rirB), "unread");
        if (okRunSt) std::snprintf(runStText, sizeof(runStText), "%u", static_cast<unsigned>(runSt)); else std::snprintf(runStText, sizeof(runStText), "unread");
        // ReadyToAllocIDs is ready when [idp+0x18] < 0x1000 or [idp+0x20] != 0 (the RE's rule, checked again in F30).
        const char* rdy = (okRir18 && okRir20) ? ((rir18 < 0x1000 || rir20 != 0) ? "yes" : "no") : "unknown";
        Log::get().note("training connect: per second: Run %llu calls (rcx 0x%llX, [rcx+0x30] %s, dt %.4f, RA 0x%llX, tid %lu); RIR %llu calls (rcx 0x%llX, "
                        "[rcx+0x18] %s, [rcx+0x20] %s, ready %s)",
                        static_cast<unsigned long long>(runN), static_cast<unsigned long long>(runRcx), runStText,
                        static_cast<double>(runDt), static_cast<unsigned long long>(runRa), static_cast<unsigned long>(runTid),
                        static_cast<unsigned long long>(rirN), static_cast<unsigned long long>(rirRcx), rirA, rirB, rdy);
        std::string top;
        for (int i = 0; i < 4; ++i) {
            if (!raN[i]) continue;
            char b[64];
            std::snprintf(b, sizeof(b), "%s0x%llX x%llu", top.empty() ? "" : ", ", static_cast<unsigned long long>(ra[i]),
                          static_cast<unsigned long long>(raN[i]));
            top += b;
        }
        if (!top.empty()) Log::get().note("training connect: RIR callers this second: %s", top.c_str());
    }
    // The CTSA objects, read directly every second (SEH-guarded; the dtor removed the freed ones).
    for (uint64_t act : ctsa) {
        if (!act) continue;
        uint32_t st = 0, fails = 0;
        uint64_t handle = 0;
        const bool okS = guardedRead(static_cast<uintptr_t>(act) + 0x30, &st, 4);
        const bool okF = guardedRead(static_cast<uintptr_t>(act) + 0xA8, &fails, 4);
        const bool okH = guardedRead(static_cast<uintptr_t>(act) + 0xA0, &handle, 8);
        Log::get().note("training connect: CTSA 0x%llX: state %s, failcount %s, handle %s",
                        static_cast<unsigned long long>(act), okS ? f28State(st).c_str() : "unread",
                        okF ? std::to_string(fails).c_str() : "unread", okH ? (std::to_string(handle)).c_str() : "unread");
    }
    // v2: the IDProvider link, every second while the SGMM wait polls ReadyToAllocIDs (this second's RIR calls) after a Deactivate. The RE rule:
    // IsConnected(idp+0x28) = the pointer is non-null and [ptr+0x60] >= 3. The idp is ReadyToAllocIDs' rcx.
    const uint64_t deactMs = g_f28DeactMs.load();
    if (rirN && rirRcx && deactMs && nowMs - deactMs <= 300000) {
        uint64_t p = 0;
        uint32_t v = 0;
        const bool okP = guardedRead(static_cast<uintptr_t>(rirRcx) + 0x28, &p, 8);
        const bool okV = okP && p && guardedRead(static_cast<uintptr_t>(p) + 0x60, &v, 4);
        const bool connected = okV && v >= 3;
        Log::get().note("training connect: IDProvider link (SGMM wait, %llu s after Deactivate): idp %s, [idp+0x28] %s, [ptr+0x60] %s, IsConnected %s",
                        static_cast<unsigned long long>((nowMs - deactMs) / 1000), f28Hex(rirRcx).c_str(), okP ? f28Hex(p).c_str() : "unread",
                        okV ? std::to_string(v).c_str() : (okP && !p ? "null pointer" : "unread"), okV ? (connected ? "yes" : "no") : "unknown");
    }
}

// The load wait (read only). The original is called first and its result is returned unchanged; the log says only what changed.
using F22BoolFn = bool(__fastcall*)(void*, void*, void*, void*);
bool g_lwInRun = false;          // the wait is in a false run (game thread, the load pipeline)
uint64_t g_lwStartMs = 0, g_lwLastMs = 0, g_lwCount = 0;
bool __fastcall loadWaitHooked(void* self, void* a2, void* a3, void* a4) {
    const F22BoolFn original = reinterpret_cast<F22BoolFn>(g_f22LoadWait.original);
    if (!original) return true;   // the trampoline is missing: let the game go on as it would without the hook
    const bool r = original(self, a2, a3, a4);
    const uint64_t now = GetTickCount64();
    if (!r) {
        if (!g_lwInRun) {
            g_lwInRun = true;
            g_lwStartMs = now;
            g_lwLastMs = now;
            g_lwCount = 0;
            Log::get().note("training load wait: returned false (waiting for ReadyToAllocIDs) first at %llu ms, this=0x%llX",
                            static_cast<unsigned long long>(now), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(self)));
        }
        ++g_lwCount;
        if (now - g_lwLastMs >= 5000) {
            g_lwLastMs = now;
            Log::get().note("training load wait: still false: %llu calls, %.1f s blocked, this=0x%llX", static_cast<unsigned long long>(g_lwCount),
                            static_cast<double>(now - g_lwStartMs) / 1000.0, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(self)));
        }
    } else if (g_lwInRun) {
        g_lwInRun = false;
        Log::get().note("training load wait: true after %.1f s blocked (%llu false calls), this=0x%llX",
                        static_cast<double>(now - g_lwStartMs) / 1000.0, static_cast<unsigned long long>(g_lwCount),
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(self)));
    }
    return r;
}

// Probe B (read only), called from the P1 callback for each AMC. The root getter is the game's own call at 0x3DD61AF (rcx = [AMC+0x208], vt+0x20).
using F22RootFn = float* (__fastcall*)(void*);
__declspec(noinline) bool callRootGetter(uintptr_t fn, uintptr_t obj, uintptr_t* outMatrix) noexcept {
    __try {
        const F22RootFn f = reinterpret_cast<F22RootFn>(fn);
        *outMatrix = reinterpret_cast<uintptr_t>(f(reinterpret_cast<void*>(obj)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint64_t g_rigBLastMs[4] = {};   // frame thread and game thread both reach it: the P1 callback only (game thread)
void rigProbeB(void* amc, uint64_t now) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(amc);
    uintptr_t p = 0;
    uint32_t mode = 0;
    if (!guardedRead(a + 0x50, &p, 8) || !p || !guardedRead(p + 0x14, &mode, 4)) return;
    if (mode != 2 && mode != 3) return;
    const size_t slot = mode - 2;
    if (g_rigBLastMs[slot] != 0 && now - g_rigBLastMs[slot] < 5000) return;
    g_rigBLastMs[slot] = now;
    uintptr_t obj = 0, vt = 0, fn = 0, mat = 0;
    const bool okObj = guardedRead(a + 0x208, &obj, 8) && obj && guardedRead(obj, &vt, 8) && vt && guardedRead(vt + 0x20, &fn, 8) && fn;
    float m[16] = {};
    const bool okRoot = okObj && callRootGetter(fn, obj, &mat) && mat && guardedRead(mat, m, sizeof(m));
    F13Head h;
    uint64_t headMs = 0;
    {
        F9Lock lock;
        h = g_lookHead;
        headMs = g_lookHeadMs;
    }
    const uintptr_t comp = g_lookComp.load(std::memory_order_relaxed);
    float basis[12] = {};
    const bool okBasis = comp && guardedRead(comp + kCompBasisOff, basis, sizeof(basis));
    char hy[24], by[24];
    if (h.valid && now - headMs < kLookHeadMaxAgeMs) std::snprintf(hy, sizeof(hy), "%.2f", f13YawDeg(h.C));
    else std::snprintf(hy, sizeof(hy), "unread");
    if (okBasis) std::snprintf(by, sizeof(by), "%.2f", static_cast<double>(std::atan2(basis[8], basis[10]) * 180.0f / 3.14159265f));
    else std::snprintf(by, sizeof(by), "unread");
    if (okRoot) {
        Log::get().note("native on foot rig probe B: AMC mode %u root row2 yaw %.2f pitch %.2f deg, origin (%.3f, %.3f, %.3f); headset (C) yaw %s deg; "
                        "look basis (comp+0x5B0) row2 yaw %s deg (read only)",
                        static_cast<unsigned>(mode), f13YawDeg(m), f13PitchDeg(m), static_cast<double>(m[12]), static_cast<double>(m[13]),
                        static_cast<double>(m[14]), hy, by);
    } else {
        Log::get().note("native on foot rig probe B: AMC mode %u root not read (getter %s); headset (C) yaw %s deg; look basis row2 yaw %s deg (read only)",
                        static_cast<unsigned>(mode), okObj ? "failed" : "pointers unread", hy, by);
    }
}

// ---- F24 Part F, F34: the marker entry (probes 1 to 3 are removed; the marker entry stays, always on in VR, for the same-call head read) ----
constexpr uintptr_t kMarkerRva = 0x1A14D80;     // the marker render entry: rcx = this, rdx = list A, r8 = list B
const uint8_t kMarkerPrologue[19] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                                     0x48, 0x81, 0xEC, 0xF0, 0x02, 0x00, 0x00};
F9Probe g_f24Marker;   // the marker entry's relay (the name is historical)
std::atomic<uint64_t> g_markerCalls{0};
uint64_t __fastcall markerHooked(void* self, void* listA, void* listB, uint64_t r9, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    using F24MarkerFn = uint64_t(__fastcall*)(void*, void*, void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    const F24MarkerFn original = reinterpret_cast<F24MarkerFn>(g_f24Marker.original);
    if (!original) return 0;
    const uint64_t n = g_markerCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) Log::get().note("native on foot marker entry: first call -- it ran (read only)");
    // F30 part 2: this call's head from list B (r8), for the post stub later in this call. Read only; under the same lock as the P2 read.
    F30SameCall& sc = t_f30SameCall;
    sc.valid = false;
    sc.haveEntry = true;
    sc.entrySeq = g_listBSeq.load(std::memory_order_relaxed);
    sc.mOk = nativeOnFootActive() && f37ReadM(reinterpret_cast<uintptr_t>(self), sc.mMat);   // F37: M at this entry
    if (listB && nativeOnFootActive()) {
        F13Head raw;
        const char* why = "";
        bool ok = false;
        {
            F9Lock lock;
            ok = f13ReadHeadInto(reinterpret_cast<uintptr_t>(listB), raw, &why);
        }
        if (ok) {
            sc.head = raw;
            sc.seq = g_listBSeq.fetch_add(1, std::memory_order_relaxed) + 1;
            sc.qpc = f30Qpc();
            sc.valid = true;
        }
    }
    return original(self, listA, listB, r9, s5, s6, s7, s8);
}
// F30 part 2: the marker entry, installed in every VR session (not only with the view probes): its pre-hook reads this call's head from list B for
// the post stub. Prologue-verified (the refuse is one line); the game is not written by it.
void installMarkerHookOnce(uintptr_t base) {
    const F9ProbeSpec s{"native-on-foot-marker-entry", kMarkerRva, kMarkerPrologue, sizeof(kMarkerPrologue), 12, reinterpret_cast<void*>(&markerHooked),
                        "native on foot marker entry"};
    size_t stolen = 0;
    std::string why;
    if (installF9ProbeInto(g_f24Marker, s, base, &stolen, &why))
        Log::get().note("native on foot marker entry: installed at EliteDangerous64.exe+0x%llX, stolen=%zu bytes, prologue %zu bytes verified (read only; "
                        "it reads the head in native on foot)", static_cast<unsigned long long>(kMarkerRva), stolen, sizeof(kMarkerPrologue));
    else
        Log::get().note("native on foot marker entry: NOT installed at EliteDangerous64.exe+0x%llX -- %s. The waypoints keep the P2 sample (fallback).",
                        static_cast<unsigned long long>(kMarkerRva), why.c_str());
}

// The list-B head sample for the look (P2 hook, game thread). Never stands the HUD writes down: a failed read only clears the sample.
void lookReadHeadP2(uintptr_t listB) noexcept {
    F13Head raw;
    const char* why = "";
    F9Lock lock;
    if (!f13ReadHeadInto(listB, raw, &why)) {
        g_lookHead.valid = false;
        return;
    }
    g_lookHead = raw;
    g_lookHeadMs = GetTickCount64();
    g_worldC = raw;   // F27: the same sample, for the world markers
    g_worldCMs = g_lookHeadMs;
    g_worldCSeq = ++g_listBSeq;   // F30: the same counter as the marker reads
    g_worldCQpc = f30Qpc();
}

// The update hook (game thread): while engaged with a non-zero input, writes rotate-abs = old + ours for this call only and restores old after it.
using F22UpdateFn = void(__fastcall*)(void*);
void __fastcall lookUpdateHooked(void* act) {
    const F22UpdateFn original = reinterpret_cast<F22UpdateFn>(g_f22Update.original);
    if (!original) return;
    g_lookUpdCalls.fetch_add(1, std::memory_order_relaxed);
    const float ours = g_lookEngaged.load(std::memory_order_relaxed) ? g_lookInput.load(std::memory_order_relaxed) : 0.0f;
    if (ours == 0.0f || !act) {
        g_lookPendingValid = false;
        // F24 (passive sign instrument): the mouse's rotate-abs, read only, for the processor's yaw delta on this call. Nothing is written.
        float passOld = 0.0f;
        uintptr_t rotP = 0;
        g_lookPassPending = act && guardedRead(reinterpret_cast<uintptr_t>(act) + kLookUpdRotOff, &rotP, 8) && rotP &&
                            guardedRead(rotP + kLookActValueOff, &passOld, 4) && std::isfinite(passOld);
        g_lookPassOld = passOld;
        original(act);
        return;
    }
    g_lookPassPending = false;
    const uintptr_t a = reinterpret_cast<uintptr_t>(act);
    uintptr_t rotAct = 0, pitchAct = 0;
    float old = 0.0f, scratch = 0.0f;
    const char* why = nullptr;
    if (!guardedRead(a + kLookUpdRotOff, &rotAct, 8) || !guardedRead(a + kLookUpdPitchOff, &pitchAct, 8)) why = "action pointers unreadable";
    else if (!rotAct || !pitchAct || rotAct == pitchAct) why = "action pointers null or equal";
    else if (!guardedRead(rotAct + kLookActValueOff, &old, 4) || !guardedRead(pitchAct + kLookActValueOff, &scratch, 4)) why = "rotate-abs or pitch-abs unreadable";
    else if (!std::isfinite(old)) why = "rotate-abs is not finite";
    if (why) {
        g_lookRefusals.fetch_add(1, std::memory_order_relaxed);
        g_lookWhy.store(why, std::memory_order_relaxed);
        g_lookPendingValid = false;
        original(act);
        return;
    }
    const float sum = old + ours;
    if (!guardedWriteFloat(rotAct + kLookActValueOff, sum)) {
        g_lookRefusals.fetch_add(1, std::memory_order_relaxed);
        g_lookWhy.store("rotate-abs write refused", std::memory_order_relaxed);
        g_lookPendingValid = false;
        original(act);
        return;
    }
    original(act);
    if (!guardedWriteFloat(rotAct + kLookActValueOff, old)) {
        // The sum is still in the field: stop engaging until the controller is told, and say so.
        g_lookEngaged.store(false, std::memory_order_relaxed);
        g_lookWhy.store("rotate-abs restore refused: look to aim stopped", std::memory_order_relaxed);
        g_lookRefusals.fetch_add(1, std::memory_order_relaxed);
    }
    g_lookWrites.fetch_add(1, std::memory_order_relaxed);
    g_lookLastOld.store(old, std::memory_order_relaxed);
    g_lookLastInput.store(ours, std::memory_order_relaxed);
    g_lookPendingValid = true;
    g_lookPendOurs = ours;
    g_lookPendSum = sum;
}

// The processor hook (game thread). Read only: it records comp and, after the original, the yaw delta it stored; our part of that delta is
// D * ours / (old + ours), which is linear, so it needs no value of the gain.
using F22ProcFn = uint64_t(__fastcall*)(void*, float);
uint64_t __fastcall lookProcHooked(void* comp, float dt) {
    const F22ProcFn original = reinterpret_cast<F22ProcFn>(g_f22Proc.original);
    if (!original) return 0;
    g_lookComp.store(reinterpret_cast<uintptr_t>(comp), std::memory_order_relaxed);
    g_lookProcCalls.fetch_add(1, std::memory_order_relaxed);
    g_lookGameFrame.fetch_add(1, std::memory_order_relaxed);
    original(comp, dt);
    if (!g_lookPendingValid && g_lookPassPending) {
        // F24: a mouse-only call (no write): the yaw delta it made, summed for the frame thread's passive check. Only when the mouse moved.
        g_lookPassPending = false;
        float d = 0.0f;
        if (std::fabs(g_lookPassOld) > 0.0005f && comp && guardedRead(reinterpret_cast<uintptr_t>(comp) + kCompYawDeltaOff, &d, 4) && std::isfinite(d)) {
            F9Lock lock;
            g_lookPassSumD += static_cast<double>(d);
        }
    }
    if (g_lookPendingValid) {
        g_lookPendingValid = false;
        float d = 0.0f;
        if (comp && guardedRead(reinterpret_cast<uintptr_t>(comp) + kCompYawDeltaOff, &d, 4) && std::isfinite(d)) {
            const float sum = g_lookPendSum;
            F9Lock lock;
            float part = 0.0f;
            if (std::fabs(sum) > 1e-9f) {
                part = d * g_lookPendOurs / sum;
                const float gain = d / sum;
                if (std::isfinite(gain) && gain > 0.0f) g_lookAMeas.store(gain, std::memory_order_relaxed);
            }
            g_lookLastD = d;
            g_lookLastPart = part;
            g_lookTotal += static_cast<double>(part);
            g_lookTurns[g_lookTurnsN % kLookTurnRing] = LookTurnSample{GetTickCount64(), std::fabs(part)};   // F23: the 3 s guard's window
            ++g_lookTurnsN;
            if (d != 0.0f && sum != 0.0f && ((d > 0.0f) != (sum > 0.0f))) g_lookSignMismatch.fetch_add(1, std::memory_order_relaxed);   // F23
        }
    }
    return 0;
}

// Installs the load wait and probe A (VR only, identity checked, once). Each refusal is one line; the game is untouched by a refused hook.
void installF22ProbesOnce() {
    if (g_f22ProbesTried) return;
    g_f22ProbesTried = true;
    if (!runtimeVrProfile()) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const char* peWhy = nullptr;
    if (!base || !checkIdentity(base, &peWhy)) {
        Log::get().note("native on foot rig probes: NOT installed -- identity mismatch (%s); the game was not touched.", peWhy ? peWhy : "no module");
        return;
    }
    if (!g_base) g_base = base;
    installF28ProbesOnce(base);   // F28: the training connect probe (fix.training_connect_probe), read only
    {
        const F9ProbeSpec s{"native-on-foot-load-wait", kLoadWaitRva, kLoadWaitPrologue, sizeof(kLoadWaitPrologue), 5,
                            reinterpret_cast<void*>(&loadWaitHooked), "F22 training load wait (read only)"};
        size_t stolen = 0;
        std::string why;
        if (installF9ProbeInto(g_f22LoadWait, s, base, &stolen, &why))
            Log::get().note("training load wait: installed at EliteDangerous64.exe+0x%llX, stolen=%zu bytes, prologue %zu bytes verified (read only; "
                            "temporary).", static_cast<unsigned long long>(kLoadWaitRva), stolen, sizeof(kLoadWaitPrologue));
        else
            Log::get().note("training load wait: NOT installed at EliteDangerous64.exe+0x%llX -- %s. The game was not touched by it.",
                            static_cast<unsigned long long>(kLoadWaitRva), why.c_str());
    }
    installMarkerHookOnce(base);  // F30 part 2: the marker entry, always (the same-call head for the waypoints)
    {
        const F9ProbeSpec s{"native-on-foot-load-stages", kStageDispatchRva, kStageDispatchPrologue, sizeof(kStageDispatchPrologue), 8,
                            reinterpret_cast<void*>(&stageDispatchHooked), "F23 load stages (read only)"};
        size_t stolen = 0;
        std::string why;
        if (installF9ProbeInto(g_f23Stage, s, base, &stolen, &why)) {
            stageLoadNames(base);
            g_stInstalled = true;
            g_stInstallMs = GetTickCount64();
            Log::get().note("load stages: installed at EliteDangerous64.exe+0x%llX, stolen=%zu bytes, prologue %zu bytes verified (read only; temporary); "
                            "stage names read: %s ... %s", static_cast<unsigned long long>(kStageDispatchRva), stolen, sizeof(kStageDispatchPrologue),
                            g_stNames[0].c_str(), g_stNames[kStageCount - 1].c_str());
        } else {
            Log::get().note("load stages: NOT installed at EliteDangerous64.exe+0x%llX -- %s. The game was not touched by it.",
                            static_cast<unsigned long long>(kStageDispatchRva), why.c_str());
        }
    }
}

// Installs the look's two hooks (VR only, once, when the key first reads on). Both must land: without the processor the body turn is not
// measured, so the controller would write input it could not compensate. The update hook alone writes nothing while not engaged.
void installLookHooksOnce() {
    if (g_f22LookTried) return;
    g_f22LookTried = true;
    if (!runtimeVrProfile()) {
        Log::get().note("native on foot look to aim: not installed (the flat profile)");
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const char* peWhy = nullptr;
    if (!base || !checkIdentity(base, &peWhy)) {
        Log::get().note("native on foot look to aim: NOT installed -- identity mismatch (%s); the game was not touched.", peWhy ? peWhy : "no module");
        return;
    }
    if (!g_base) g_base = base;
    size_t stolenUpd = 0, stolenProc = 0;
    std::string whyUpd, whyProc;
    const F9ProbeSpec su{"native-on-foot-look-update", kLookUpdateRva, kLookUpdatePrologue, sizeof(kLookUpdatePrologue), 7,
                         reinterpret_cast<void*>(&lookUpdateHooked), "F22 look update"};
    const F9ProbeSpec sp{"native-on-foot-look-proc", kLookProcRva, kLookProcPrologue, sizeof(kLookProcPrologue), 5,
                         reinterpret_cast<void*>(&lookProcHooked), "F22 look processor"};
    const bool okUpd = installF9ProbeInto(g_f22Update, su, base, &stolenUpd, &whyUpd);
    const bool okProc = installF9ProbeInto(g_f22Proc, sp, base, &stolenProc, &whyProc);
    g_lookArmed = okUpd && okProc;
    if (okUpd) Log::get().note("native on foot look to aim: update hook installed at EliteDangerous64.exe+0x%llX, stolen=%zu bytes, prologue %zu verified",
                               static_cast<unsigned long long>(kLookUpdateRva), stolenUpd, sizeof(kLookUpdatePrologue));
    else Log::get().note("native on foot look to aim: update hook NOT installed -- %s. Look to aim stays off.", whyUpd.c_str());
    if (okProc) Log::get().note("native on foot look to aim: processor hook installed at EliteDangerous64.exe+0x%llX, stolen=%zu bytes, prologue %zu verified "
                                "(read only)", static_cast<unsigned long long>(kLookProcRva), stolenProc, sizeof(kLookProcPrologue));
    else Log::get().note("native on foot look to aim: processor hook NOT installed -- %s. Look to aim stays off.", whyProc.c_str());
}

// F24: the head's yaw in list-B's convention, atan2(row 2 x, row 2 z) of C. F23 read it as minus row 2 and a centred head came out at 176 deg;
// F22's +row 2 read -3.6 deg with the head centred (the F23 flight, 2026-10-14). The F22 spin was the yaw sign (fix.native_on_foot_look_to_aim_yaw_sign).
inline double lookForwardYawRad(const float* C) noexcept {
    return std::atan2(static_cast<double>(C[8]), static_cast<double>(C[10]));
}
inline double lookWrapRad(double a) noexcept {
    const double pi = 3.14159265358979323846;
    while (a > pi) a -= 2.0 * pi;
    while (a < -pi) a += 2.0 * pi;
    return a;
}
constexpr double kLookPi = 3.14159265358979323846;
constexpr double kLookTurnWindowMs = 3000.0;                       // the runaway guard's window
constexpr double kLookTurnWindowMaxRad = 270.0 * kLookPi / 180.0;  // ...and the most body turn it allows in that window
constexpr double kLookTurnRateRad = 180.0 * kLookPi / 180.0;       // the turn-rate cap: 180 deg/s, times dt
constexpr uint64_t kLookCapStallMs = 1000;                         // at the cap this long with the error not falling: the guard trips

// Stops the look until the key toggles or native on foot re-enters. Zeroes the input before the update hook can see it.
void lookTrip(const char* reason) {
    g_lookTripped = true;
    g_lookResetSerial.fetch_add(1, std::memory_order_relaxed);   // F25: the compensation goes back to zero
    g_lookEngaged.store(false, std::memory_order_relaxed);
    g_lookInput.store(0.0f, std::memory_order_relaxed);
    g_lookEngagedLast = false;
    g_lookHavePrev = false;
    g_lookCapStartMs = 0;
    Log::get().note("native on foot look to aim: runaway guard tripped (%s)", reason);
}

// Frame thread, every frame: the controller. Writes g_lookInput (the update hook applies it on the next game update) and logs every 5 s while
// engaged, with the compensation sign check and the pitch probe (log only). Engagement needs the key, reticle_follow = head, native stereo on
// foot, the session and both hooks, and no runaway trip.
void lookToAimTick(uint64_t nowMs, bool session) {
    const bool keyOn = g_lookKeyOn.load(std::memory_order_relaxed);
    const bool headMode = g_reticleHead.load(std::memory_order_relaxed);
    const bool onFoot = nativeOnFootActive();
    // The guard clears only on a key toggle or on native on foot re-entering.
    if (keyOn != g_lookKeyLast || (onFoot && !g_lookFootLast)) {
        if (g_lookTripped) Log::get().note("native on foot look to aim: runaway guard cleared (the key was toggled or native on foot re-entered)");
        g_lookTripped = false;
    }
    g_lookKeyLast = keyOn;
    g_lookFootLast = onFoot;
    const bool engagedBase = keyOn && headMode && onFoot && session && g_lookArmed;
    const bool engagedWant = engagedBase && !g_lookTripped;
    g_lookWantRead.store(onFoot && session, std::memory_order_relaxed);
    const double dt = (g_lookLastMs != 0 && nowMs > g_lookLastMs) ? std::min(0.1, static_cast<double>(nowMs - g_lookLastMs) / 1000.0) : 0.0;
    g_lookLastMs = nowMs;
    if (engagedWant != g_lookEngagedLast) {
        if (engagedWant) {
            Log::get().note("native on foot look to aim: engaged (the body turns to the head; the world is held by the runtime's compensation)");
            g_lookHavePrev = false;
            g_lookCapStartMs = 0;
            g_lookConvLogged = false;
        } else {
            const char* offWhy = !keyOn ? "fix.native_on_foot_look_to_aim is off" : !headMode ? "fix.native_on_foot_reticle_follow is not head"
                                 : !onFoot ? "native stereo on foot ended" : !g_lookArmed ? "the hooks are not armed" : g_lookTripped ? "the runaway guard"
                                 : "the game session ended";
            Log::get().note("native on foot look to aim: stops (%s); input zero", offWhy);
        }
        if (!engagedWant) g_lookResetSerial.fetch_add(1, std::memory_order_relaxed);   // F25: disengage (key off or stopped) resets the compensation
        g_lookEngagedLast = engagedWant;
    }
    g_lookEngaged.store(engagedWant, std::memory_order_relaxed);
    if (!engagedWant) {
        g_lookInput.store(0.0f, std::memory_order_relaxed);
        return;
    }
    F13Head h;
    uint64_t headMs = 0;
    double totalNow = 0.0, turn3s = 0.0;
    {
        F9Lock lock;
        h = g_lookHead;
        headMs = g_lookHeadMs;
        totalNow = g_lookTotal;
        const size_t n = static_cast<size_t>(std::min<uint64_t>(g_lookTurnsN, kLookTurnRing));
        for (size_t i = 0; i < n; ++i) {
            if (static_cast<double>(nowMs) - static_cast<double>(g_lookTurns[i].ms) <= kLookTurnWindowMs) turn3s += static_cast<double>(g_lookTurns[i].absPart);
        }
    }
    const uintptr_t comp = g_lookComp.load(std::memory_order_relaxed);
    float aLo = 0.0f, aHi = 0.0f;
    const char* refuse = nullptr;
    if (!comp) refuse = "no processor call yet (comp unknown)";
    else if (!guardedRead(comp + kCompGainLoOff, &aLo, 4) || !guardedRead(comp + kCompGainHiOff, &aHi, 4)) refuse = "gain floats unreadable";
    const float aMeas = g_lookAMeas.load(std::memory_order_relaxed);
    const float aEst = aMeas > 0.0f ? aMeas : 0.5f * (aLo + aHi);
    if (!refuse && !(aEst > 0.0f && std::isfinite(aEst))) refuse = "gain is not positive";
    if (!refuse && !(h.valid && nowMs - headMs < kLookHeadMaxAgeMs)) refuse = "no fresh head sample (list B)";
    const double maxTurn = kLookTurnRateRad * dt;   // F23: the rate cap, per frame
    float input = 0.0f;
    float turn = 0.0f;
    double errRad = 0.0, errDeg = 0.0;
    if (!refuse) {
        if (!g_lookConvLogged) {
            g_lookConvLogged = true;
            Log::get().note("native on foot look to aim: yaw convention at engagement: raw C row 2 = (%.4f, %.4f, %.4f); yaw atan2(row 2 x, row 2 z) %.2f deg "
                            "(used, the F22 reading); minus row 2 %.2f deg (the F23 reading, refuted). A centred head reads about 0 under the first.",
                            static_cast<double>(h.C[8]), static_cast<double>(h.C[9]), static_cast<double>(h.C[10]),
                            lookForwardYawRad(h.C) * 180.0 / kLookPi, std::atan2(static_cast<double>(-h.C[8]), static_cast<double>(-h.C[10])) * 180.0 / kLookPi);
        }
        const float sign = g_lookSign.load(std::memory_order_relaxed) < 0 ? -1.0f : 1.0f;
        const double tauS = g_lookSmoothMs.load(std::memory_order_relaxed) / 1000.0;
        const double alpha = tauS <= 0.0 ? 1.0 : 1.0 - std::exp(-dt / tauS);
        errRad = lookForwardYawRad(h.C);
        errDeg = errRad * 180.0 / kLookPi;
        const double want = static_cast<double>(sign) * errRad * alpha;
        const double clamped = std::max(-maxTurn, std::min(maxTurn, want));
        turn = static_cast<float>(clamped);
        input = turn / aEst;
        if (!std::isfinite(input)) {
            refuse = "the input is not finite";
            input = 0.0f;
        }
    }
    if (refuse) {
        g_lookHavePrev = false;
    } else {
        // F23 runaway guard. Both checks use this frame's numbers; a trip zeroes the input and stops the look until the key toggles or on foot re-enters.
        if (turn3s > kLookTurnWindowMaxRad) {
            lookTrip("the body turned more than 270 deg in 3 s");
            return;
        }
        const bool atCap = maxTurn > 0.0 && std::fabs(static_cast<double>(turn)) >= 0.98 * maxTurn;
        if (atCap) {
            const double errAbs = std::fabs(errRad);
            if (g_lookCapStartMs == 0 || errAbs < g_lookCapErr - 0.005) {
                g_lookCapStartMs = nowMs;
                g_lookCapErr = errAbs;
            } else if (nowMs - g_lookCapStartMs >= kLookCapStallMs) {
                lookTrip("the output sat at the 180 deg/s rate cap for 1 s while the error did not fall");
                return;
            }
        } else {
            g_lookCapStartMs = 0;
        }
        // The sign instrument: after the body turned by our part since the last tick, the error should have fallen by that part.
        if (g_lookHavePrev) {
            const double part = totalNow - g_lookPrevTotal;
            const double res = lookWrapRad(errRad - (g_lookPrevErr - part));
            g_lookResAbs += std::fabs(res);
            ++g_lookResN;
            if (std::fabs(part) > 0.0005) {
                g_lookResRatio += res / part;
                ++g_lookRatioN;
            }
        }
        g_lookPrevTotal = totalNow;
        g_lookPrevErr = errRad;
        g_lookHavePrev = true;
    }
    g_lookInput.store(refuse ? 0.0f : input, std::memory_order_relaxed);
    g_lookWhy.store(refuse, std::memory_order_relaxed);
    if (nowMs - g_lookLineMs < 5000 && g_lookLineMs != 0) return;
    g_lookLineMs = nowMs;
    double totalDeg = 0.0, lastD = 0.0, lastPart = 0.0;
    {
        F9Lock lock;
        totalDeg = g_lookTotal * 180.0 / kLookPi;
        lastD = static_cast<double>(g_lookLastD) * 180.0 / kLookPi;
        lastPart = static_cast<double>(g_lookLastPart) * 180.0 / kLookPi;
    }
    const double windowDeg = totalDeg - g_lookWinStartDeg;
    g_lookWinStartDeg = totalDeg;
    const float aLoF = aLo, aHiF = aHi;
    const char* why = g_lookWhy.load(std::memory_order_relaxed);
    Log::get().note("native on foot look to aim: error yaw %.2f deg (sign %d), our input %.5f (turn %.3f deg, gain %.4f %s), old input %.5f, "
                    "game yaw delta %.3f deg, our part %.3f deg, compensation published %.3f deg this window (total %.3f deg, game frame %u), "
                    "A lo %.4f hi %.4f measured %.4f, comp 0x%llX, update calls %u writes %u refusals %u, turn in 3 s %.1f deg, %s",
                    refuse ? 0.0 : errDeg, g_lookSign.load() < 0 ? -1 : 1, static_cast<double>(refuse ? 0.0f : input),
                    static_cast<double>(turn) * 180.0 / kLookPi, static_cast<double>(aEst), aMeas > 0.0f ? "measured" : "midpoint",
                    static_cast<double>(g_lookLastOld.load()), lastD, lastPart, windowDeg, totalDeg,
                    static_cast<unsigned>(g_lookGameFrame.load()), static_cast<double>(aLoF), static_cast<double>(aHiF), static_cast<double>(aMeas),
                    static_cast<unsigned long long>(comp), static_cast<unsigned>(g_lookUpdCalls.load()), static_cast<unsigned>(g_lookWrites.load()),
                    static_cast<unsigned>(g_lookRefusals.load()), turn3s * 180.0 / kLookPi,
                    refuse ? (std::string("refused: ") + refuse).c_str() : (why ? why : "no refusals"));
    // F23: the sign instrument, one line per 5 s. About 0 mean residual: the compensation is right. About 2 x our part: its sign is wrong.
    {
        const uint32_t mismatchNow = g_lookSignMismatch.load(std::memory_order_relaxed);
        const uint32_t mismatchWin = mismatchNow - g_lookMismatchWinStart;
        g_lookMismatchWinStart = mismatchNow;
        Log::get().note("native on foot look to aim: compensation sign check: mean |residual| %.3f deg over %u frames, mean residual/our part %.3f over %u "
                        "frames (about 0 right, about 2 wrong), sign(game yaw delta) != sign(old+ours) %u frames this window (%u total), runtime sign key %d "
                        "(temporary)",
                        g_lookResN ? g_lookResAbs * 180.0 / kLookPi / static_cast<double>(g_lookResN) : 0.0, static_cast<unsigned>(g_lookResN),
                        g_lookRatioN ? g_lookResRatio / static_cast<double>(g_lookRatioN) : 0.0, static_cast<unsigned>(g_lookRatioN),
                        static_cast<unsigned>(mismatchWin), static_cast<unsigned>(mismatchNow), g_lookCompSign.load() < 0 ? -1 : 1);
        g_lookResAbs = 0.0;
        g_lookResRatio = 0.0;
        g_lookResN = 0;
        g_lookRatioN = 0;
    }
    // F23: the full look basis (comp+0x5B0, three rows of four floats), once per 5 s. Log only; nothing is written.
    float basisVals[12] = {};
    const bool okBasis = comp && guardedRead(comp + kCompBasisOff, basisVals, sizeof(basisVals));
    char basisText[200];
    if (okBasis) {
        std::snprintf(basisText, sizeof(basisText), "(%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f)",
                      static_cast<double>(basisVals[0]), static_cast<double>(basisVals[1]), static_cast<double>(basisVals[2]), static_cast<double>(basisVals[3]),
                      static_cast<double>(basisVals[4]), static_cast<double>(basisVals[5]), static_cast<double>(basisVals[6]), static_cast<double>(basisVals[7]),
                      static_cast<double>(basisVals[8]), static_cast<double>(basisVals[9]), static_cast<double>(basisVals[10]), static_cast<double>(basisVals[11]));
    } else {
        std::snprintf(basisText, sizeof(basisText), "unread");
    }
    char viewPitch[24], headPitch[24];
    if (h.valid) {
        std::snprintf(viewPitch, sizeof(viewPitch), "%.2f", static_cast<double>(std::asin(std::max(-1.0f, std::min(1.0f, h.viewY))) * 180.0f / 3.14159265f));
        std::snprintf(headPitch, sizeof(headPitch), "%.2f", f13PitchDeg(h.C));
    } else {
        std::snprintf(viewPitch, sizeof(viewPitch), "unread");
        std::snprintf(headPitch, sizeof(headPitch), "unread");
    }
    Log::get().note("native on foot look to aim: pitch probe (log only, nothing written): look basis %s, view pitch %s deg (list-B eye 0), "
                    "headset pitch %s deg (C)", basisText, viewPitch, headPitch);
}

// F24 (passive sign instrument, no injection): on frames where only the mouse turns the body, compare the sign of the game's yaw delta with the
// sign of the change in list-B eye 0's forward yaw. Same sign counts as agree. Implied yaw sign: +1 if agree >= disagree, else -1. Every 5 s.
void lookPassiveTick(uint64_t nowMs) {
    if (!g_lookArmed) return;
    double sumD = 0.0;
    {
        F9Lock lock;
        sumD = g_lookPassSumD;
        g_lookPassSumD = 0.0;
    }
    F13Head h;
    uint64_t headMs = 0;
    {
        F9Lock lock;
        h = g_lookHead;
        headMs = g_lookHeadMs;
    }
    const bool viewOk = h.valid && nowMs - headMs < kLookHeadMaxAgeMs;
    if (viewOk && g_lookPassHavePrev && std::fabs(sumD) > 0.0005) {
        const double dView = lookWrapRad(static_cast<double>(h.viewYaw) - g_lookPassPrevView);
        if (std::fabs(dView) <= 0.0005) ++g_lookPassStill;
        else if ((sumD > 0.0) == (dView > 0.0)) ++g_lookPassAgree;
        else ++g_lookPassDisagree;
    }
    if (viewOk) {
        g_lookPassPrevView = static_cast<double>(h.viewYaw);
        g_lookPassHavePrev = true;
    } else {
        g_lookPassHavePrev = false;
    }
    if (g_lookPassLineMs == 0) {
        g_lookPassLineMs = nowMs;
        return;
    }
    if (nowMs - g_lookPassLineMs < 5000) return;
    g_lookPassLineMs = nowMs;
    const int implied = g_lookPassAgree >= g_lookPassDisagree ? 1 : -1;
    Log::get().note("native on foot look to aim: passive sign check (mouse-only turns, nothing injected): agree %u, disagree %u, view still %u "
                    "(list-B eye 0 forward yaw did not move), implied yaw sign %d (agree = the game's yaw delta and the view yaw move the same way)",
                    static_cast<unsigned>(g_lookPassAgree), static_cast<unsigned>(g_lookPassDisagree), static_cast<unsigned>(g_lookPassStill), implied);
    g_lookPassAgree = g_lookPassDisagree = g_lookPassStill = 0;
}

// Frame thread, every frame: the total the runtime applies, published with the game frame it was taken at, delayed by fix.native_on_foot_
// look_to_aim_comp_delay frames (0 or 1). A two-slot ring: the delay picks the slot.
void lookPublishTick() {
    static double ring[2] = {};
    static uint32_t ringFrame[2] = {};
    static unsigned head = 0;
    double total = 0.0;
    uint32_t gameFrame = 0;
    {
        F9Lock lock;
        total = g_lookTotal;
        gameFrame = g_lookGameFrame.load(std::memory_order_relaxed);
    }
    ring[head & 1u] = total;
    ringFrame[head & 1u] = gameFrame;
    ++head;
    const unsigned delay = static_cast<unsigned>(g_lookDelay.load(std::memory_order_relaxed) > 0 ? 1 : 0);
    const unsigned slot = (head - 1u - delay) & 1u;
    // F23: the runtime's sign (temporary): the compensation is published as the body turn times this sign.
    const double sgn = g_lookCompSign.load(std::memory_order_relaxed) < 0 ? -1.0 : 1.0;
    nativeFrameSetLookYaw(static_cast<float>(sgn * ring[slot]), ringFrame[slot], g_lookResetSerial.load(std::memory_order_relaxed));
}

// The HRC line's values, without the window's calls (the change key).
std::string f9HrcValues(const F9HrcState& s) {
    char g[3][8];
    const bool oks[3] = {s.ok291, s.ok470, s.ok4C0};
    const uint8_t vals[3] = {s.g291, s.g470, s.g4C0};
    for (int i = 0; i < 3; ++i) {
        if (oks[i]) std::snprintf(g[i], sizeof(g[i]), "%02X", static_cast<unsigned>(vals[i]));
        else std::snprintf(g[i], sizeof(g[i]), "?");
    }
    std::string inst;
    for (size_t k = 0; k < nofoot::kF9Hrc; ++k) {
        char b[24];
        if (s.instOk[k]) std::snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(s.inst[k]));
        else std::snprintf(b, sizeof(b), "?");
        if (k) inst += ',';
        inst += b;
    }
    char dep[24];
    if (s.depOk) std::snprintf(dep, sizeof(dep), "0x%llX", static_cast<unsigned long long>(s.dep));
    else std::snprintf(dep, sizeof(dep), "?");
    return std::string("g291=") + g[0] + " g470=" + g[1] + " g4C0=" + g[2] + " inst=" + inst + " dep=" + dep +
           " dep.vt=" + f9RvaText(s.depVt, s.depVtOk);
}

// ---- F18 (TEMPORARY), probe P1: the eye against the body --------------------------------------------------------------------------------
// EliteDangerous64.exe+0x3DA2C80 is the avatar visibility job: rcx = the AMC, rdx = the job context, whose floats [rdx+0..8] are the camera position
// the visibility code subtracts from the AMC origin. Read-only. The callback samples the local third-person AMC (amcLocal(), the same match as the
// fade hook) once a second, under SEH, BEFORE it forwards to the game with its own arguments. The camera is read from [rdx]; the AMC's world matrix
// comes from its transform call (origin at +0x30, rows 0-2 the basis); the body coordinates are (p - origin) dotted with each basis row. The head is
// the neck census's head joint (avatar-root model space) taken through the same world matrix: that mapping is the assumption this probe tests.
using F9Fn2 = uint64_t(__fastcall*)(void*, void*);
constexpr uintptr_t kEyeProbeRva = 0x3DA2C80;
constexpr size_t kEyeProbeStolen = 5;   // mov [rsp+0x10],rdx (48 89 54 24 10); the next instructions are push, push, push, sub: no relative operand
const uint8_t kEyeProbePrologue[32] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00,
                                       0x80, 0xA1, 0x23, 0x03, 0x00, 0x00, 0xFB, 0x48, 0x8D, 0x81, 0xA8, 0x17, 0x00, 0x00, 0x48, 0x8B};
F9Probe g_eyeProbe;                      // the relay, gate, trampoline and CodeHook
std::atomic<bool> g_eyeStop{false};      // set at shutdown: the callback still forwards, but samples nothing
bool g_eyeTried = false;                 // frame thread only
uint64_t g_eyeLastMs = 0;                // game thread only: the last sample's time
struct EyeSample {
    bool valid = false;
    bool headOk = false;
    uint64_t stamp = 0;
    float cam[3] = {}, head[3] = {}, camBody[3] = {}, headBody[3] = {};
    // F21: the raw inputs of the derived values, read in the same callback: the AMC origin (world), its basis rows (world), the head joint in
    // avatar-root model space (the +0x58 translation, unconverted).
    float origin[3] = {}, rows[9] = {}, headModel[3] = {};
    // F33: the third-person pov in world space (the head joint plus the rest offset, through the AMC matrix), when the rest offset is known.
    bool povOk = false;
    bool povDerived = false;   // true: Explorer Cam's rest offset; false: the head joint itself (plus the trims)
    // F37: the AMC's root matrix origin ([AMC+0x208] through the root getter) and its own position ([AMC+0x2E0]), for the verification line.
    bool rootOk = false, amcPosOk = false;
    float root[3] = {}, amcPos[3] = {};
    float pov[3] = {};
};
std::mutex g_eyeMutex;                   // guards g_eyeSample (game thread writes, frame thread reads)
EyeSample g_eyeSample;
uint64_t g_eyeLoggedStamp = 0;           // frame thread only

// The AMC's world matrix (the transform call of sehAmcOrigin, the whole 4x4 this time). False when a pointer on the chain is implausible or a fault.
__declspec(noinline) bool sehAmcMatrix(const uint8_t* amc, float* out16) noexcept {
    __try {
        uint64_t obj = 0, vt = 0, fn = 0;
        std::memcpy(&obj, amc + ecm::kOffAmcTransform, 8);
        if (obj < 0x10000u || obj >= 0x00007FFF00000000ull) return false;
        std::memcpy(&vt, reinterpret_cast<const void*>(obj), 8);
        if (vt < 0x10000u || vt >= 0x00007FFF00000000ull) return false;
        std::memcpy(&fn, reinterpret_cast<const void*>(vt + ecm::kTransformMatrixSlot), 8);
        if (fn < 0x10000u || fn >= 0x00007FFF00000000ull) return false;
        const uint64_t m = reinterpret_cast<uint64_t(__fastcall*)(uint64_t)>(fn)(obj);
        if (m < 0x10000u || m >= 0x00007FFF00000000ull) return false;
        std::memcpy(out16, reinterpret_cast<const void*>(m), 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Game thread, from the callback, at most once a second, for the local third-person AMC only. Read-only.
void eyeSample(void* amcPtr, void* jobPtr, uint64_t now) noexcept {
    float cam[3] = {};
    if (!guardedRead(reinterpret_cast<uintptr_t>(jobPtr), cam, sizeof(cam))) return;
    float M[16] = {};
    if (!sehAmcMatrix(static_cast<const uint8_t*>(amcPtr), M)) return;
    EyeSample s;
    s.valid = true;
    s.stamp = now;
    std::memcpy(s.cam, cam, sizeof(cam));
    NeckCensusReading nk;
    s.headOk = neckCensusRead(&nk) && nk.state == 5;
    float head[3] = {};
    if (s.headOk) {
        // [h 1] times the world matrix (row vectors): the head joint in world space.
        for (size_t j = 0; j < 3; ++j) {
            head[j] = nk.head[0] * M[j] + nk.head[1] * M[4 + j] + nk.head[2] * M[8 + j] + M[12 + j];
        }
        std::memcpy(s.head, head, sizeof(head));
    }
    const float* o = M + 12;   // the origin (row 3)
    for (size_t i = 0; i < 3; ++i) {
        const float* b = M + 4 * i;   // basis row i
        float dc = 0.0f, dh = 0.0f;
        for (size_t j = 0; j < 3; ++j) {
            dc += (cam[j] - o[j]) * b[j];
            dh += (head[j] - o[j]) * b[j];
        }
        s.camBody[i] = dc;
        s.headBody[i] = dh;
    }
    for (size_t i = 0; i < 3; ++i) {
        s.origin[i] = o[i];
        for (size_t j = 0; j < 3; ++j) s.rows[3 * i + j] = M[4 * i + j];
    }
    if (s.headOk) std::memcpy(s.headModel, nk.head, sizeof(s.headModel));
    // F33: the third-person pov: the head joint plus the rest offset (model space), through the AMC matrix (row vectors, as the head above).
    // The rest offset is Explorer Cam's when its walk has derived one; otherwise the head joint itself (the pov source is logged).
    float rest[3] = {};
    s.povDerived = s.headOk && explorerCamRestPovDelta(rest);
    if (s.headOk) {
        if (!s.povDerived) rest[0] = rest[1] = rest[2] = 0.0f;
        const float pm[3] = {nk.head[0] + rest[0], nk.head[1] + rest[1], nk.head[2] + rest[2]};
        for (size_t j = 0; j < 3; ++j) s.pov[j] = pm[0] * M[j] + pm[1] * M[4 + j] + pm[2] * M[8 + j] + M[12 + j];
        s.povOk = true;
    }
    {
        const uintptr_t amc = reinterpret_cast<uintptr_t>(amcPtr);
        uintptr_t obj = 0, vt = 0, fn = 0, mat = 0;
        float rm[16] = {};
        if (guardedRead(amc + 0x208, &obj, 8) && obj && guardedRead(obj, &vt, 8) && vt && guardedRead(vt + 0x20, &fn, 8) && fn &&
            callRootGetter(fn, obj, &mat) && mat && guardedRead(mat, rm, sizeof(rm))) {
            s.rootOk = true;
            std::memcpy(s.root, rm + 12, sizeof(s.root));
        }
        s.amcPosOk = guardedRead(amc + 0x2E0, s.amcPos, sizeof(s.amcPos));
    }
    std::lock_guard<std::mutex> lock(g_eyeMutex);
    g_eyeSample = s;
}

// F37: the FP root (the mode-2 AMC's root matrix, the anchor the arms are drawn at), read through the game's own root getter as rigProbeB calls it.
// Read on each P1 call for a mode-2 AMC; under g_eyeMutex.
struct FpRootSnap {
    bool ok = false;
    float origin[3] = {};
    uint64_t stamp = 0;
};
FpRootSnap g_fpRoot;   // under g_eyeMutex
void fpRootTrack(void* amcPtr, uint64_t now) noexcept {
    const uintptr_t a = reinterpret_cast<uintptr_t>(amcPtr);
    uintptr_t p = 0;
    uint32_t mode = 0;
    if (!guardedRead(a + 0x50, &p, 8) || !p || !guardedRead(p + 0x14, &mode, 4) || mode != 2) return;
    uintptr_t obj = 0, vt = 0, fn = 0, mat = 0;
    float m[16] = {};
    if (!(guardedRead(a + 0x208, &obj, 8) && obj && guardedRead(obj, &vt, 8) && vt && guardedRead(vt + 0x20, &fn, 8) && fn &&
          callRootGetter(fn, obj, &mat) && mat && guardedRead(mat, m, sizeof(m))))
        return;
    std::lock_guard<std::mutex> lock(g_eyeMutex);
    g_fpRoot.ok = true;
    std::memcpy(g_fpRoot.origin, m + 12, sizeof(g_fpRoot.origin));
    g_fpRoot.stamp = now;
}
// F37 post-hook check: the next P1 call reads one shifted record and says whether the shift is still in place when the renderer reads it.
std::atomic<bool> g_f37CheckArm{false}, g_f37CheckPending{false};
uintptr_t g_f37CheckAddr = 0;
float g_f37CheckExpect[3] = {};
void f37CheckPoll() noexcept {
    if (!g_f37CheckPending.load(std::memory_order_acquire)) return;
    g_f37CheckPending.store(false, std::memory_order_relaxed);
    float xyz[3] = {};
    if (!guardedRead(g_f37CheckAddr, xyz, sizeof(xyz))) {
        Log::get().note("native on foot body shift: post-hook check: the record could not be read");
        return;
    }
    bool still = true;
    for (size_t i = 0; i < 3; ++i)
        if (std::fabs(xyz[i] - g_f37CheckExpect[i]) > 1e-3f) still = false;
    Log::get().note("native on foot body shift: post-hook check (the next P1 call): record xyz (%.3f,%.3f,%.3f), the shift wrote (%.3f,%.3f,%.3f): %s",
                    static_cast<double>(xyz[0]), static_cast<double>(xyz[1]), static_cast<double>(xyz[2]), static_cast<double>(g_f37CheckExpect[0]),
                    static_cast<double>(g_f37CheckExpect[1]), static_cast<double>(g_f37CheckExpect[2]),
                    still ? "still shifted when the next P1 call read it" : "already reset");
}

// The callback: the game jumps here with rcx, rdx intact (the relay's tail jump), so the original is called with the same two arguments.
uint64_t __fastcall eyeProbeHooked(void* amc, void* job) noexcept {
    const F9Fn2 original = reinterpret_cast<F9Fn2>(g_eyeProbe.original);
    if (!original) return 0;
    const uint64_t now = GetTickCount64();
    // F22 (read only): probe B, the FP and 3P roots against the head and the look basis, every 5 s per mode while native stereo is on foot.
    if (amc && !g_eyeStop.load(std::memory_order_relaxed) && nativeOnFootActive()) rigProbeB(amc, now);
    if (amc && nativeOnFootActive()) fpRootTrack(amc, now);   // F37
    f37CheckPoll();   // F37: the post-hook check armed by the body shift
    if (!g_eyeStop.load(std::memory_order_relaxed) && amc && job && nativeOnFootActive() && (g_alignKeyOn.load(std::memory_order_relaxed) || now - g_eyeLastMs >= 1000) &&
        reinterpret_cast<uint64_t>(amc) == localAmcPointer()) {
        g_eyeLastMs = now;
        eyeSample(amc, job, now);
    }
    return original(amc, job);
}

// Frame thread, every frame: logs the newest sample once (one per second at most). Stays silent while native stereo is off foot.
void eyeProbeLine() {
    if (!nativeOnFootActive()) return;
    EyeSample s;
    {
        std::lock_guard<std::mutex> lock(g_eyeMutex);
        s = g_eyeSample;
    }
    if (!s.valid || s.stamp == g_eyeLoggedStamp) return;
    g_eyeLoggedStamp = s.stamp;
    // F21: the raw inputs, for a hand check of the derived line below. |cam - origin| is the camera's distance from the AMC origin (about the eye
    // height if both are in one frame); the row lengths are 1 for a plain rotation (anything else is a scale); |head model| is the head's height in
    // avatar-root space when the root sits at the feet, and it must equal |head world - origin| when the basis is orthonormal.
    {
        const float co[3] = {s.cam[0] - s.origin[0], s.cam[1] - s.origin[1], s.cam[2] - s.origin[2]};
        const float rl[3] = {std::sqrt(s.rows[0] * s.rows[0] + s.rows[1] * s.rows[1] + s.rows[2] * s.rows[2]),
                             std::sqrt(s.rows[3] * s.rows[3] + s.rows[4] * s.rows[4] + s.rows[5] * s.rows[5]),
                             std::sqrt(s.rows[6] * s.rows[6] + s.rows[7] * s.rows[7] + s.rows[8] * s.rows[8])};
        const float hm = std::sqrt(s.headModel[0] * s.headModel[0] + s.headModel[1] * s.headModel[1] + s.headModel[2] * s.headModel[2]);
        const float ho[3] = {s.head[0] - s.origin[0], s.head[1] - s.origin[1], s.head[2] - s.origin[2]};
        const float hw = std::sqrt(ho[0] * ho[0] + ho[1] * ho[1] + ho[2] * ho[2]);
        Log::get().note("native on foot eye inputs: cam=(%.3f,%.3f,%.3f) origin=(%.3f,%.3f,%.3f) |cam-origin|=%.3f "
                        "row0=(%.4f,%.4f,%.4f) row1=(%.4f,%.4f,%.4f) row2=(%.4f,%.4f,%.4f) row_len=%.4f,%.4f,%.4f "
                        "head_model=%s(%.3f,%.3f,%.3f) |head_model|=%.3f head_world=%s(%.3f,%.3f,%.3f) |head_world-origin|=%.3f",
                        static_cast<double>(s.cam[0]), static_cast<double>(s.cam[1]), static_cast<double>(s.cam[2]),
                        static_cast<double>(s.origin[0]), static_cast<double>(s.origin[1]), static_cast<double>(s.origin[2]),
                        static_cast<double>(std::sqrt(co[0] * co[0] + co[1] * co[1] + co[2] * co[2])),
                        static_cast<double>(s.rows[0]), static_cast<double>(s.rows[1]), static_cast<double>(s.rows[2]),
                        static_cast<double>(s.rows[3]), static_cast<double>(s.rows[4]), static_cast<double>(s.rows[5]),
                        static_cast<double>(s.rows[6]), static_cast<double>(s.rows[7]), static_cast<double>(s.rows[8]),
                        static_cast<double>(rl[0]), static_cast<double>(rl[1]), static_cast<double>(rl[2]),
                        s.headOk ? "" : "unread ", static_cast<double>(s.headModel[0]), static_cast<double>(s.headModel[1]),
                        static_cast<double>(s.headModel[2]), static_cast<double>(hm), s.headOk ? "" : "unread ",
                        static_cast<double>(s.head[0]), static_cast<double>(s.head[1]), static_cast<double>(s.head[2]), static_cast<double>(hw));
    }
    const float dx = s.camBody[0] - s.headBody[0], dy = s.camBody[1] - s.headBody[1], dz = s.camBody[2] - s.headBody[2];
    if (s.headOk) {
        Log::get().note("native on foot eye vs body: cam_in_body=(%.3f,%.3f,%.3f) head_in_body=(%.3f,%.3f,%.3f) cam_minus_head=(%.3f,%.3f,%.3f)",
                        static_cast<double>(s.camBody[0]), static_cast<double>(s.camBody[1]), static_cast<double>(s.camBody[2]),
                        static_cast<double>(s.headBody[0]), static_cast<double>(s.headBody[1]), static_cast<double>(s.headBody[2]),
                        static_cast<double>(dx), static_cast<double>(dy), static_cast<double>(dz));
    } else {
        Log::get().note("native on foot eye vs body: cam_in_body=(%.3f,%.3f,%.3f) head_in_body=(unread: the neck census has no head sample) cam_minus_head=(unread)",
                        static_cast<double>(s.camBody[0]), static_cast<double>(s.camBody[1]), static_cast<double>(s.camBody[2]));
    }
}

// ---- F33/F35 (TEMPORARY, live): body align. fix.native_on_foot_body_align = off | view | body (the old "on" is view).
// view (F33): the seated origin moves, so the first-person camera sits at the third-person body's pov (through the runtime, output version 8).
// body (F35): the third-person body's render records move by the same offset, in world axes (the submit job's records, local AMC only).
// The error is what is left between the camera and the pov the render shows: (cam - pov) minus the trims, minus the offset already applied
// (the measured pov is the unshifted one, so the applied offset must be subtracted to close the loop). view also takes the HMD's place into
// account (F33). The integral takes 0.2 of the error per frame (5 cm at most), clamped to 0.6 m; a critically damped filter (tau 250 ms) follows
// it; the published value moves 5 cm per frame at most. The guard: when the error has not fallen to 90% 2 s after the offset started, the offset
// goes to zero for good until the key is toggled. Held while ADS, and while an input is missing or the AMC sample is over 100 ms old.
namespace align {
constexpr double kTauSec = 0.25, kGain = 0.2, kClampM = 0.6, kCapPerFrameM = 0.05, kGuardSec = 2.0, kStaleMs = 100.0, kErrOnM = 0.01;
}
struct AlignRun {
    bool keyWasOn = false;
    bool tripped = false;
    bool guardArmed = false;
    double guardErr = 0.0;
    uint64_t guardMs = 0;
    double pos[3] = {}, vel[3] = {}, intg[3] = {}, applied[3] = {};
    uint32_t serial = 0;
    uint64_t lastMs = 0, lineMs = 0, waitLineMs = 0;
    uint64_t holds = 0;
};
AlignRun g_align;       // view (F33): the seated offset, frame thread only
AlignRun g_alignBody;   // body (F35): the world offset of the body's render records, frame thread only

// F35 hand-off: the body offset (world metres) as a seqlock: the frame thread writes, the submit job's hook reads (it runs on the game's thread
// for each AMC, not the frame thread, so the reads must not block or tear).
std::atomic<uint32_t> g_bodySeq{0};
std::atomic<float> g_bodyDx{0.0f}, g_bodyDy{0.0f}, g_bodyDz{0.0f};
void bodyOffsetPublish(const double d[3]) noexcept {
    g_bodySeq.fetch_add(1, std::memory_order_acq_rel);   // odd: a write is in progress
    g_bodyDx.store(static_cast<float>(d[0]), std::memory_order_relaxed);
    g_bodyDy.store(static_cast<float>(d[1]), std::memory_order_relaxed);
    g_bodyDz.store(static_cast<float>(d[2]), std::memory_order_relaxed);
    g_bodySeq.fetch_add(1, std::memory_order_release);   // even again
}
void bodyOffsetRead(float d[3]) noexcept {
    for (;;) {
        const uint32_t s1 = g_bodySeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        d[0] = g_bodyDx.load(std::memory_order_relaxed);
        d[1] = g_bodyDy.load(std::memory_order_relaxed);
        d[2] = g_bodyDz.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_bodySeq.load(std::memory_order_relaxed) == s1) return;
    }
}
std::atomic<uint64_t> g_bodyShifted{0}, g_bodySkipped{0}, g_bodyCalls{0};   // this second (the frame thread reads and resets them)

// The third-person AMC's render records (F35, from the static RE of the submit job 0x3DA1DE0): 54 blocks of 0x680 bytes; each block holds four
// 0x150-byte records, the masks right after them. The offsets are the RE's; the flight checks them (the skipped count and the eye dump).
namespace bodyrec {
constexpr uintptr_t kRecBase = 0x1260;      // record k of block idx: AMC + kRecBase + idx*kStride + k*kRecSize
constexpr uintptr_t kInstBase = 0x11C0;     // instance pointer of (idx, k): AMC + kInstBase + idx*kStride + 8k
constexpr uintptr_t kMaskBase = 0x17A0;     // the mask of (idx, k): AMC + kMaskBase + idx*kStride + 8k (the masks follow the four records)
constexpr uintptr_t kStride = 0x680, kRecSize = 0x150;
constexpr uintptr_t kOrigin = 0x2E0;        // the AMC's world position (xyz); a record's xyz equals it when the part is unshifted
constexpr uintptr_t kRecPos = 0x10;         // the record's world xyz
constexpr uintptr_t kPrev = 0x124;          // the record's "previous" copy of xyz
constexpr uintptr_t kInstOff = 0x1A0;       // the instance's field that must be non-zero
constexpr uintptr_t kFlags = 0x323;         // the AMC's flags byte: bit 2 (0x4) gates the shift
constexpr int kBlocks = 54, kSlots = 4;
static_assert(kMaskBase - kRecBase == kSlots * kRecSize, "the masks start right after the four records of a block");
static_assert(kRecBase - kInstBase == 0xA0, "the instance pointers sit 0xA0 before the records of a block");
}  // namespace bodyrec

// The submit job's prologue (the post-hook's steal is its first 8 bytes: mov rax,rsp; mov [rax+8],rcx; push rbx). The whole 32 bytes are
// verified at install: a mismatch refuses the hook with a line.
constexpr uintptr_t kBodyShiftRva = 0x3DA1DE0;
const uint8_t kBodyShiftPrologue[31] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x53, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0xF6,
                                        0x81, 0x23, 0x03, 0x00, 0x00, 0x04, 0x48, 0x8B, 0xD9, 0x0F, 0x84, 0xCF, 0x04, 0x00, 0x00};
F9Probe g_bodyShift;

// The post-hook: the original runs first with the very arguments it was given (the wrapper forwards all eight, as the probes do), then the
// records of the local third-person AMC are shifted by the published body offset. Nothing else is written.
using F35Fn = uint64_t(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
std::atomic<bool> g_f37TableWanted{false};   // set on each entry to body mode: the per-part table prints on the next shift pass
struct F37First {
    bool valid = false;
    float pre[3] = {}, post[3] = {};
    uintptr_t addr = 0;
};
F37First g_f37First;   // the first shifted record of the latest pass (the 1 s line prints it)
void bodyShiftApply(uint64_t amc) noexcept {
    uint8_t flags = 0;
    if (!guardedRead(amc + bodyrec::kFlags, &flags, 1) || !(flags & 4)) return;
    float d[3] = {};
    bodyOffsetRead(d);
    if (d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f) return;
    const bool table = g_f37TableWanted.exchange(false, std::memory_order_relaxed);
    float origin[3] = {};
    if (!guardedRead(amc + bodyrec::kOrigin, origin, sizeof(origin))) return;
    uint64_t shifted = 0, skipped = 0;
    bool present[bodyrec::kBlocks][bodyrec::kSlots] = {}, movedAt[bodyrec::kBlocks][bodyrec::kSlots] = {};
    uint64_t maskAt[bodyrec::kBlocks][bodyrec::kSlots] = {};
    bool firstThisPass = true;
    for (int idx = 0; idx < bodyrec::kBlocks; ++idx) {
        for (int k = 0; k < bodyrec::kSlots; ++k) {
            uint64_t inst = 0, mask = 0, instField = 0;
            const uintptr_t slot = static_cast<uintptr_t>(idx) * bodyrec::kStride + static_cast<uintptr_t>(k) * 8u;
            if (!guardedRead(amc + bodyrec::kInstBase + slot, &inst, 8) || !inst) continue;
            if (!guardedRead(amc + bodyrec::kMaskBase + slot, &mask, 8) || !mask) continue;
            if (!guardedRead(static_cast<uintptr_t>(inst) + bodyrec::kInstOff, &instField, 8) || !instField) continue;
            present[idx][k] = true;
            maskAt[idx][k] = mask;
            const uintptr_t rec = amc + bodyrec::kRecBase + static_cast<uintptr_t>(idx) * bodyrec::kStride +
                                  static_cast<uintptr_t>(k) * bodyrec::kRecSize;
            float pos[3] = {};
            if (!guardedRead(rec + bodyrec::kRecPos, pos, sizeof(pos))) {
                ++skipped;
                continue;
            }
            if (std::memcmp(pos, origin, sizeof(pos)) != 0) {   // not at the AMC's origin: already placed, not ours to move
                ++skipped;
                continue;
            }
            float prev[3] = {};
            const bool okPrev = guardedRead(rec + bodyrec::kPrev, prev, sizeof(prev));
            for (int i = 0; i < 3; ++i) {
                guardedWriteFloat(rec + bodyrec::kRecPos + 4u * static_cast<uintptr_t>(i), pos[i] + d[i]);
                if (okPrev) guardedWriteFloat(rec + bodyrec::kPrev + 4u * static_cast<uintptr_t>(i), prev[i] + d[i]);
            }
            movedAt[idx][k] = true;
            if (firstThisPass) {
                firstThisPass = false;
                g_f37First.valid = true;
                g_f37First.addr = rec + bodyrec::kRecPos;
                for (int i = 0; i < 3; ++i) {
                    g_f37First.pre[i] = pos[i];
                    g_f37First.post[i] = pos[i] + d[i];
                }
                if (g_f37CheckArm.exchange(false, std::memory_order_relaxed)) {
                    g_f37CheckAddr = g_f37First.addr;
                    for (int i = 0; i < 3; ++i) g_f37CheckExpect[i] = g_f37First.post[i];
                    g_f37CheckPending.store(true, std::memory_order_release);
                }
            }
            ++shifted;
        }
    }
    g_bodyShifted.fetch_add(shifted, std::memory_order_relaxed);
    g_bodySkipped.fetch_add(skipped, std::memory_order_relaxed);
    if (table) {
        for (int idx = 0; idx < bodyrec::kBlocks; ++idx)
            for (int k = 0; k < bodyrec::kSlots; ++k) {
                if (!present[idx][k]) continue;
                Log::get().note("native on foot body shift part: block %d slot %d name %s mask %016llX shifted %s", idx, k,
                                idx < ecm::kPartCount ? ecm::kPartNames[idx] : "?", static_cast<unsigned long long>(maskAt[idx][k]),
                                movedAt[idx][k] ? "yes" : "no");
            }
    }
}
uint64_t __fastcall bodyShiftHooked(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    const F35Fn original = reinterpret_cast<F35Fn>(g_bodyShift.original);
    if (!original) return 0;
    const uint64_t r = original(a1, a2, a3, a4, s5, s6, s7, s8);
    g_bodyCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_alignMode.load(std::memory_order_relaxed) == 2 && a1 && a1 == localAmcPointer()) bodyShiftApply(a1);
    return r;
}

// One filter step for a run: the integral of the error, the guard, the critically damped follow, the clamp, the per-frame cap. `driving` false
// holds the integral and lets the filter settle onto it (a decay when `decay` is set, toward zero).
void alignFilter(AlignRun& a, const double err[3], bool driving, bool decay, double dt, uint64_t now, bool guardOn) noexcept {
    const double errMag = std::sqrt(err[0] * err[0] + err[1] * err[1] + err[2] * err[2]);
    if (driving) {
        double step[3] = {align::kGain * err[0], align::kGain * err[1], align::kGain * err[2]};
        const double sm = std::sqrt(step[0] * step[0] + step[1] * step[1] + step[2] * step[2]);
        const double capScale = sm > align::kCapPerFrameM ? align::kCapPerFrameM / sm : 1.0;
        for (size_t i = 0; i < 3; ++i) a.intg[i] += step[i] * capScale;
        const double im = std::sqrt(a.intg[0] * a.intg[0] + a.intg[1] * a.intg[1] + a.intg[2] * a.intg[2]);
        if (im > align::kClampM)
            for (size_t i = 0; i < 3; ++i) a.intg[i] *= align::kClampM / im;
        if (!guardOn) {
        } else if (!a.guardArmed) {
            if (errMag > align::kErrOnM) {
                a.guardArmed = true;
                a.guardMs = now;
                a.guardErr = errMag;
            }
        } else if (errMag < align::kErrOnM) {
            a.guardArmed = false;
        } else if (now - a.guardMs >= static_cast<uint64_t>(align::kGuardSec * 1000.0)) {
            if (errMag >= 0.9 * a.guardErr) {
                a.tripped = true;
                Log::get().note("native on foot body align: guard tripped: |error| has not fallen after 2 s (%.1f cm then, %.1f cm now): the offset goes to "
                                "zero (a sign or order error is the likely cause; toggle the key to re-arm)", a.guardErr * 100.0, errMag * 100.0);
            } else {
                a.guardMs = now;
                a.guardErr = errMag;
            }
        }
    }
    if (decay || a.tripped) {
        for (size_t i = 0; i < 3; ++i) a.intg[i] -= a.intg[i] * std::min(1.0, dt / align::kTauSec);
    }
    const double target[3] = {a.intg[0], a.intg[1], a.intg[2]};
    for (size_t i = 0; i < 3; ++i) {
        if (dt <= 0.0) break;
        const double acc = (target[i] - a.pos[i]) / (align::kTauSec * align::kTauSec) - 2.0 * a.vel[i] / align::kTauSec;
        a.vel[i] += acc * dt;
        double move = a.vel[i] * dt;
        if (move > align::kCapPerFrameM) move = align::kCapPerFrameM;
        if (move < -align::kCapPerFrameM) move = -align::kCapPerFrameM;
        a.pos[i] += move;
    }
    const double pm = std::sqrt(a.pos[0] * a.pos[0] + a.pos[1] * a.pos[1] + a.pos[2] * a.pos[2]);
    if (pm > align::kClampM)
        for (size_t i = 0; i < 3; ++i) a.pos[i] *= align::kClampM / pm;
}

// The frame thread's one call per frame. The view offset is seated metres (the runtime moves the seated origin by it); the body offset is world
// metres (the submit job's hook moves the records by it). The other run's offset decays to zero.
// F37: the view's head motion reference (tS0), the sign check's previous step, and the last values the 1 s lines print.
bool g_f37TS0Valid = false;
float g_f37TS0[3] = {};
uint32_t g_f37TS0Gen = 0;
double g_f37TSNow[3] = {};
struct F37Prev {
    bool valid = false;
    double camB[3] = {}, applied[3] = {};
};
F37Prev g_f37Prev;
double g_f37SignToward = 0.0, g_f37SignApplied = 0.0;   // metres: the cam's motion since the last step, toward the pov and along the applied step

void alignTick(uint64_t now) noexcept {
    static int lastMode = -1;
    const int mode = g_alignMode.load(std::memory_order_relaxed);   // 0 off, 1 view, 2 body
    AlignRun& a = mode == 2 ? g_alignBody : g_align;
    AlignRun& other = mode == 2 ? g_align : g_alignBody;
    const bool on = mode != 0;
    if (mode != lastMode) {
        static const char* const kNames[3] = {"off", "view", "body"};
        Log::get().note("native on foot body align: mode %s (was %s); trims right %.1f, up %.1f, forward %.1f cm; gain 0.2 per frame, tau 250 ms, "
                        "clamp 0.6 m, 5 cm per frame at most; guard 2 s", kNames[mode], lastMode < 0 ? "none" : kNames[lastMode], g_alignTrimCm[0],
                        g_alignTrimCm[1], g_alignTrimCm[2]);
        a.tripped = false;
        a.guardArmed = false;
        lastMode = mode;
        if (mode == 2) g_f37TableWanted.store(true, std::memory_order_relaxed);   // F37: the per-part table on entering body
        if (mode == 1) g_f37TS0Valid = false;                                      // F37: a new head reference on entering view
    }
    const double dt = (a.lastMs && now > a.lastMs) ? std::min(0.05, static_cast<double>(now - a.lastMs) / 1000.0) : 0.0;
    a.lastMs = now;
    other.lastMs = now;

    // The inputs: the AMC sample, the list-B view (view only), the render head (view only), ADS.
    EyeSample s;
    {
        std::lock_guard<std::mutex> lock(g_eyeMutex);
        s = g_eyeSample;
    }
    float C[16] = {};
    bool haveView = false;
    {
        F9Lock lock;
        if (g_worldC.valid && now - g_worldCMs < 500) {
            std::memcpy(C, g_worldC.C, sizeof(C));
            haveView = true;
        }
    }
    EdvrNativeNowPose np{};
    const bool haveFrame = nativeFrameNowPose(np) && np.renderValid;
    const bool ads = journalAds();
    const char* wait = nullptr;
    if (!on) wait = "key off";
    else if (!nativeOnFootActive()) wait = "not native stereo on foot";
    else if (!s.valid || now - s.stamp > static_cast<uint64_t>(align::kStaleMs)) wait = "the local third-person AMC sample is missing or over 100 ms old";
    else if (!s.headOk) wait = "no head joint (the neck census has no sample)";
    else if (!s.povOk) wait = "no pov (the neck census has no head sample)";
    else if (mode == 1 && !haveView) wait = "no list-B view (the P2 sample is missing or over 500 ms old)";
    else if (mode == 1 && !haveFrame) wait = "no render head from the runtime";
    else if (ads) wait = "held: aiming down sights";
    if (wait && on && now - a.waitLineMs >= 5000) {
        a.waitLineMs = now;
        Log::get().note("native on foot body align: %s: waiting (%s)", mode == 2 ? "body" : "view", wait);
    }

    // F37: the FP root (body mode's anchor), if seen within 100 ms.
    FpRootSnap fp{};
    {
        std::lock_guard<std::mutex> lock(g_eyeMutex);
        fp = g_fpRoot;
    }
    const bool fpFresh = fp.ok && now - fp.stamp < 100;
    // The error, in the frame of the run: seated for view, world for body. Both start from the same measurement: the camera minus the pov.
    double err[3] = {}, deltaB[3] = {}, hmdB[3] = {}, camB[3] = {}, povB[3] = {};
    if (!wait) {
        const auto body = [&](const float p[3], double out[3]) {
            for (size_t i = 0; i < 3; ++i) {
                double acc = 0.0;
                for (size_t j = 0; j < 3; ++j) acc += (static_cast<double>(p[j]) - static_cast<double>(s.origin[j])) * static_cast<double>(s.rows[3 * i + j]);
                out[i] = acc;
            }
        };
        const float* camUse = (mode == 2 && fpFresh) ? fp.origin : s.cam;   // F37: body mode anchors on the FP root
        body(camUse, camB);
        body(s.pov, povB);
        double trimB[3] = {g_alignTrimCm[0] / 100.0, g_alignTrimCm[1] / 100.0, g_alignTrimCm[2] / 100.0};   // right, up, forward
        if (mode == 2) {
            // body: the world error is the camera minus the pov, the trims in world axes, and the offset already applied (the pov the render
            // shows is the measured pov plus that offset).
            double tw[3] = {};
            for (size_t j = 0; j < 3; ++j)
                for (size_t i = 0; i < 3; ++i) tw[j] += trimB[i] * static_cast<double>(s.rows[3 * i + j]);
            for (size_t j = 0; j < 3; ++j)
                err[j] = (static_cast<double>(camUse[j]) - static_cast<double>(s.pov[j])) - tw[j] - a.pos[j];
            for (size_t i = 0; i < 3; ++i) deltaB[i] = camB[i] - povB[i] - trimB[i];
        } else {
            // F37: the head's motion only. tS is the render head's translation in seated metres, less tS0 (the first head of this reference
            // generation: captured when the view starts and at each recentre). Mapped to body axes: v = R^T tS, dw = C.rot v, hmd_i = row_i . dw
            // (a vector, no origin subtraction). The sign is the sign check's to prove (the 1 s line).
            if (!g_f37TS0Valid || np.referenceGeneration != g_f37TS0Gen) {
                g_f37TS0[0] = np.render[3];
                g_f37TS0[1] = np.render[7];
                g_f37TS0[2] = np.render[11];
                g_f37TS0Gen = np.referenceGeneration;
                g_f37TS0Valid = true;
                Log::get().note("native on foot body align: head reference tS0 captured at (%.3f, %.3f, %.3f) m seated (reference generation %u)",
                                static_cast<double>(g_f37TS0[0]), static_cast<double>(g_f37TS0[1]), static_cast<double>(g_f37TS0[2]),
                                static_cast<unsigned>(g_f37TS0Gen));
            }
            for (size_t i = 0; i < 3; ++i) g_f37TSNow[i] = static_cast<double>(np.render[4 * i + 3]) - static_cast<double>(g_f37TS0[i]);
            double vH[3] = {}, dwH[3] = {};
            for (size_t j = 0; j < 3; ++j)
                for (size_t i = 0; i < 3; ++i) vH[j] += static_cast<double>(np.render[4 * i + j]) * g_f37TSNow[i];
            for (size_t k = 0; k < 3; ++k)
                for (size_t j = 0; j < 3; ++j) dwH[k] += static_cast<double>(C[4 * k + j]) * vH[j];
            for (size_t i = 0; i < 3; ++i) {
                hmdB[i] = 0.0;
                for (size_t j = 0; j < 3; ++j) hmdB[i] += dwH[j] * static_cast<double>(s.rows[3 * i + j]);
            }
            double errB[3] = {};
            for (size_t i = 0; i < 3; ++i) {
                deltaB[i] = (camB[i] - hmdB[i]) - povB[i];
                errB[i] = deltaB[i] - trimB[i];
            }
            double dw[3] = {};
            for (size_t j = 0; j < 3; ++j)
                for (size_t i = 0; i < 3; ++i) dw[j] += errB[i] * static_cast<double>(s.rows[3 * i + j]);
            double tmp[3] = {};
            for (size_t j = 0; j < 3; ++j)
                for (size_t k = 0; k < 3; ++k) tmp[j] += static_cast<double>(C[4 * k + j]) * dw[k];
            for (size_t i = 0; i < 3; ++i)
                for (size_t j = 0; j < 3; ++j) err[i] += static_cast<double>(np.render[4 * i + j]) * tmp[j];
        }
    }
    const double errMag = std::sqrt(err[0] * err[0] + err[1] * err[1] + err[2] * err[2]);

    // F37 sign check (view): the cam's motion since the last step, against the step the applied offset took (mapped to body axes the way the
    // error is). "toward" is the component toward the pov; positive means the cam moved toward it. Along the applied step: positive means the cam
    // moved with the offset.
    if (mode == 1 && !wait) {
        if (g_f37Prev.valid) {
            double dCam[3] = {}, dS[3] = {}, tmpS[3] = {}, dwS[3] = {}, dB[3] = {}, toPov[3] = {};
            for (size_t i = 0; i < 3; ++i) {
                dCam[i] = camB[i] - g_f37Prev.camB[i];
                dS[i] = a.applied[i] - g_f37Prev.applied[i];
                toPov[i] = povB[i] - camB[i];
            }
            for (size_t j = 0; j < 3; ++j)
                for (size_t i = 0; i < 3; ++i) tmpS[j] += static_cast<double>(np.render[4 * i + j]) * dS[i];
            for (size_t k = 0; k < 3; ++k)
                for (size_t j = 0; j < 3; ++j) dwS[k] += static_cast<double>(C[4 * k + j]) * tmpS[j];
            for (size_t i = 0; i < 3; ++i)
                for (size_t j = 0; j < 3; ++j) dB[i] += dwS[j] * static_cast<double>(s.rows[3 * i + j]);
            const double nPov = std::sqrt(toPov[0] * toPov[0] + toPov[1] * toPov[1] + toPov[2] * toPov[2]);
            const double nB = std::sqrt(dB[0] * dB[0] + dB[1] * dB[1] + dB[2] * dB[2]);
            double tw2 = 0.0, aw2 = 0.0;
            for (size_t i = 0; i < 3; ++i) {
                if (nPov > 1e-9) tw2 += dCam[i] * toPov[i] / nPov;
                if (nB > 1e-9) aw2 += dCam[i] * dB[i] / nB;
            }
            g_f37SignToward = tw2;
            g_f37SignApplied = aw2;
        }
        g_f37Prev.valid = true;
        for (size_t i = 0; i < 3; ++i) {
            g_f37Prev.camB[i] = camB[i];
            g_f37Prev.applied[i] = a.applied[i];
        }
    }

    // The active run takes the filter step; the other decays to zero and publishes zero.
    alignFilter(a, err, on && !a.tripped && !wait, !on || a.tripped, dt, now, mode != 2);   // F37: the body run has no guard
    if (wait && on) ++a.holds;
    const double zero[3] = {0.0, 0.0, 0.0};
    alignFilter(other, zero, false, true, dt, now, mode == 2);

    double change = 0.0;
    for (size_t i = 0; i < 3; ++i) change = std::max(change, std::fabs(a.pos[i] - a.applied[i]));
    if (change > 1e-6) {
        for (size_t i = 0; i < 3; ++i) a.applied[i] = a.pos[i];
        if (mode == 2) {
            bodyOffsetPublish(a.applied);
        } else {
            const float f[3] = {static_cast<float>(a.applied[0]), static_cast<float>(a.applied[1]), static_cast<float>(a.applied[2])};
            nativeFrameSetEyeAlign(f, ++a.serial);
        }
    }
    // The other run's published value goes to zero (the view's eye offset when body is on, the body's offset when view is on).
    double otherChange = 0.0;
    for (size_t i = 0; i < 3; ++i) otherChange = std::max(otherChange, std::fabs(other.pos[i] - other.applied[i]));
    if (otherChange > 1e-6) {
        for (size_t i = 0; i < 3; ++i) other.applied[i] = other.pos[i];
        if (mode == 2) {
            const float f[3] = {static_cast<float>(other.applied[0]), static_cast<float>(other.applied[1]), static_cast<float>(other.applied[2])};
            nativeFrameSetEyeAlign(f, ++other.serial);
        } else {
            bodyOffsetPublish(other.applied);
        }
    }

    // The once-a-second line while the offset is live.
    if (on && !wait && now - a.lineMs >= 1000) {
        a.lineMs = now;
        g_f37CheckArm.store(true, std::memory_order_relaxed);   // F37: the post-hook check on the next shift
        if (mode == 2) {
            const uint64_t shifted = g_bodyShifted.exchange(0, std::memory_order_relaxed);
            const uint64_t skipped = g_bodySkipped.exchange(0, std::memory_order_relaxed);
            const F37First first = g_f37First;
            const double jf[3] = {fpFresh ? static_cast<double>(s.cam[0]) - fp.origin[0] : 0.0, fpFresh ? static_cast<double>(s.cam[1]) - fp.origin[1] : 0.0,
                                  fpFresh ? static_cast<double>(s.cam[2]) - fp.origin[2] : 0.0};
            const double am[3] = {s.amcPosOk && s.rootOk ? static_cast<double>(s.amcPos[0]) - s.root[0] : 0.0,
                                  s.amcPosOk && s.rootOk ? static_cast<double>(s.amcPos[1]) - s.root[1] : 0.0,
                                  s.amcPosOk && s.rootOk ? static_cast<double>(s.amcPos[2]) - s.root[2] : 0.0};
            Log::get().note("native on foot body align: mode body (F37), d=(%.1f,%.1f,%.1f) cm (world), records shifted %llu, skipped %llu this second, "
                            "guard off, ads %s, error %.1f cm; fp_root=(%.3f,%.3f,%.3f) %s; job_cam_minus_fp_root=(%.3f,%.3f,%.3f) m; "
                            "amc_pos_minus_root=(%.4f,%.4f,%.4f) m (expect 0); first shifted record %s: pos before (%.3f,%.3f,%.3f) after (%.3f,%.3f,%.3f)",
                            a.applied[0] * 100.0, a.applied[1] * 100.0, a.applied[2] * 100.0, static_cast<unsigned long long>(shifted),
                            static_cast<unsigned long long>(skipped), ads ? "yes" : "no", errMag * 100.0, static_cast<double>(fp.origin[0]),
                            static_cast<double>(fp.origin[1]), static_cast<double>(fp.origin[2]), fpFresh ? "fresh" : "stale", jf[0], jf[1], jf[2],
                            am[0], am[1], am[2], first.valid ? "yes" : "none", first.pre[0], first.pre[1], first.pre[2], first.post[0], first.post[1],
                            first.post[2]);
        } else {
            const double ap[3] = {a.applied[0] * 100.0, a.applied[1] * 100.0, a.applied[2] * 100.0};
            Log::get().note("native on foot body align: mode view (F37 head motion): tS=(%.3f,%.3f,%.3f) tS0=(%.3f,%.3f,%.3f) m (seated x,y,z); "
                            "cam_world=(%.3f,%.3f,%.3f) pov_world=(%.3f,%.3f,%.3f) m; cam_body=(%.3f,%.3f,%.3f) pov_body=(%.3f,%.3f,%.3f) hmd_body=(%.3f,%.3f,%.3f) m "
                            "(body right,up,forward); fp_root=(%.3f,%.3f,%.3f) %s; delta_body_cm=(%.1f,%.1f,%.1f) applied_seated_cm=(%.1f,%.1f,%.1f) "
                            "filt_ms=%.0f ads=%s guard=%s pov: %s; sign check: cam toward pov %+.3f cm, along the applied step %+.3f cm this step",
                            g_f37TSNow[0], g_f37TSNow[1], g_f37TSNow[2], static_cast<double>(g_f37TS0[0]), static_cast<double>(g_f37TS0[1]),
                            static_cast<double>(g_f37TS0[2]), static_cast<double>(s.cam[0]), static_cast<double>(s.cam[1]), static_cast<double>(s.cam[2]),
                            static_cast<double>(s.pov[0]), static_cast<double>(s.pov[1]), static_cast<double>(s.pov[2]), camB[0], camB[1], camB[2],
                            povB[0], povB[1], povB[2], hmdB[0], hmdB[1], hmdB[2], static_cast<double>(fp.origin[0]),
                            static_cast<double>(fp.origin[1]), static_cast<double>(fp.origin[2]), fpFresh ? "fresh" : "stale", deltaB[0] * 100.0,
                            deltaB[1] * 100.0, deltaB[2] * 100.0, ap[0], ap[1], ap[2], align::kTauSec * 1000.0, ads ? "yes" : "no",
                            a.tripped ? "tripped" : "ok", s.povDerived ? "derived" : "head joint + trims", g_f37SignToward * 100.0,
                            g_f37SignApplied * 100.0);
        }
    }
}

// The body hook's install (once, VR): refused with a line on any byte mismatch, the game untouched then.
void installBodyShiftOnce() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    if (!runtimeVrProfile()) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const char* peWhy = nullptr;
    if (!base || !checkIdentity(base, &peWhy)) {
        Log::get().note("native on foot body shift: NOT installed -- identity mismatch (%s); the game was not touched.", peWhy ? peWhy : "no module");
        return;
    }
    const F9ProbeSpec spec{"native-on-foot-body-shift", kBodyShiftRva, kBodyShiftPrologue, sizeof(kBodyShiftPrologue), 8,
                           reinterpret_cast<void*>(&bodyShiftHooked), "F35 body shift"};
    size_t stolen = 0;
    std::string why;
    if (installF9ProbeInto(g_bodyShift, spec, base, &stolen, &why))
        Log::get().note("native on foot body shift: installed at EliteDangerous64.exe+0x3DA1DE0, stolen=%zu bytes, prologue %zu bytes verified (post-hook: "
                        "the local third-person AMC's render records, only in body mode)", stolen, sizeof(kBodyShiftPrologue));
    else
        Log::get().note("native on foot body shift: NOT installed at EliteDangerous64.exe+0x3DA1DE0 -- %s. The body mode is off; the game was not touched.",
                        why.c_str());
}

void installEyeProbeOnce() {
    if (g_eyeTried) return;
    g_eyeTried = true;
    if (!runtimeVrProfile()) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const char* peWhy = nullptr;
    if (!base || !checkIdentity(base, &peWhy)) {
        Log::get().note("native on foot eye vs body: NOT installed -- identity mismatch (%s); the game was not touched.", peWhy ? peWhy : "no module");
        return;
    }
    if (!g_base) g_base = base;
    const F9ProbeSpec spec{"native-on-foot-eye-vs-body", kEyeProbeRva, kEyeProbePrologue, 32, kEyeProbeStolen,
                           reinterpret_cast<void*>(&eyeProbeHooked), "P1 (eye vs body)"};
    size_t stolen = 0;
    std::string why;
    if (installF9ProbeInto(g_eyeProbe, spec, base, &stolen, &why)) {
        Log::get().note("native on foot eye vs body: hook installed at EliteDangerous64.exe+0x3DA2C80, stolen=%zu bytes, prologue 32 bytes verified "
                        "(read only; temporary).", stolen);
    } else {
        Log::get().note("native on foot eye vs body: hook NOT installed -- %s. The game was not touched by it.", why.c_str());
    }
}

void installF14ProbesOnce() {
    if (g_f14Tried) return;
    g_f14Tried = true;
    if (!runtimeVrProfile()) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const char* peWhy = nullptr;
    if (!base || !checkIdentity(base, &peWhy)) {
        Log::get().note("native on foot hud layout: NOT installed -- identity mismatch (%s); the game was not touched.", peWhy ? peWhy : "no module");
        return;
    }
    if (!g_base) g_base = base;
    static const uint8_t kLayoutPrologue[32] = {0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x40, 0x48,
                                               0x8B, 0xF9, 0x0F, 0x29, 0x74, 0x24, 0x30, 0x48, 0x8B, 0x89, 0xB8, 0x01, 0x00, 0x00, 0x48, 0x8B};
    const F9ProbeSpec spec{"native-on-foot-hud-layout", 0x1A0DCD0, kLayoutPrologue, 32, 5, reinterpret_cast<void*>(&layoutHooked),
                           "P4 (HUD layout)"};
    size_t stolen = 0;
    std::string why;
    if (installF9ProbeInto(g_f14Layout, spec, base, &stolen, &why)) {
        g_f14LayoutArmed = true;
        Log::get().note("native on foot hud layout: hook installed at EliteDangerous64.exe+0x1A0DCD0, stolen=%zu bytes, prologue 32 bytes verified "
                        "(read only; temporary).", stolen);
    } else {
        Log::get().note("native on foot hud layout: hook NOT installed -- %s. The game was not touched by it.", why.c_str());
    }
}

// Frame thread, every frame: every 5 s the window's panel callers, the HRC matrices, and the HRC and instance rows; and the HRC line whenever
// its values change.
void f9ProbeTick(uint64_t nowMs) {
    if (!g_f9Armed[0] && !g_f9Armed[1] && !g_f9Armed[2]) return;
    const bool due = g_f9LastMs == 0 || nowMs - g_f9LastMs >= 5000;
    F9CallerRow callers[kF9Callers];
    F9HrcState hrc[3];
    F9InstRow insts[kF9Insts];
    {
        F9Lock lock;
        for (size_t i = 0; i < kF9Callers; ++i) callers[i] = g_f9Callers[i];
        for (size_t i = 0; i < 3; ++i) hrc[i] = g_f9Hrc[i];
        for (size_t i = 0; i < kF9Insts; ++i) insts[i] = g_f9Insts[i];
        if (due) {
            for (auto& r : g_f9Callers) r.window = 0;
            for (auto& s : g_f9Hrc) s.window = 0;
            for (auto& r : g_f9Insts) r.window = 0;
        }
    }
    static std::string lastKey[3];   // frame thread only: the HRC values last written, per state
    bool hrcWritten[3] = {false, false, false};
    for (uint8_t st = 0; st < 3; ++st) {
        if (!hrc[st].seen) continue;
        const std::string key = f9HrcValues(hrc[st]);
        if (key != lastKey[st]) {
            lastKey[st] = key;
            hrcWritten[st] = true;
            Log::get().note("native on foot hrc [%s]: %s calls=%llu", g_f9StateName[st], key.c_str(), static_cast<unsigned long long>(hrc[st].window));
        }
    }
    if (!due) return;
    g_f9LastMs = nowMs;
    for (const F9CallerRow& r : callers) {
        if (!r.used || r.window == 0) continue;
        char caller[24];
        if (r.caller) std::snprintf(caller, sizeof(caller), "+0x%X", static_cast<unsigned>(r.caller));
        else std::snprintf(caller, sizeof(caller), "?");
        Log::get().note("native on foot panel submit [%s]: caller=%s calls=%llu", g_f9StateName[r.st], caller, static_cast<unsigned long long>(r.window));
    }
    for (uint8_t st = 0; st < 3; ++st) {
        if (!hrc[st].seen || hrcWritten[st]) continue;
        Log::get().note("native on foot hrc [%s]: %s calls=%llu", g_f9StateName[st], f9HrcValues(hrc[st]).c_str(),
                        static_cast<unsigned long long>(hrc[st].window));
    }
    for (uint8_t st = 0; st < 3; ++st) {
        if (!hrc[st].matOk) continue;
        char m[480];
        int n = std::snprintf(m, sizeof(m), "native on foot hrc matrix [%s]: hrc=0x%llX m=(", g_f9StateName[st], static_cast<unsigned long long>(hrc[st].hrc));
        for (size_t i = 0; i < 16 && n > 0 && static_cast<size_t>(n) < sizeof(m) - 16; ++i) {
            n += std::snprintf(m + n, sizeof(m) - static_cast<size_t>(n), "%s%.3f", i ? "," : "", static_cast<double>(hrc[st].mat[i]));
        }
        std::snprintf(m + n, sizeof(m) - static_cast<size_t>(n), ")");
        Log::get().note("%s", m);
    }
    for (const F9InstRow& r : insts) {
        if (!r.used || r.window == 0) continue;
        char view[24], cam[64], rot[200];
        if (r.viewOk) std::snprintf(view, sizeof(view), "%u", static_cast<unsigned>(r.viewId));
        else std::snprintf(view, sizeof(view), "?");
        if (r.camOk) std::snprintf(cam, sizeof(cam), "(%.3f,%.3f,%.3f)", static_cast<double>(r.cam[0]), static_cast<double>(r.cam[1]), static_cast<double>(r.cam[2]));
        else std::snprintf(cam, sizeof(cam), "?");
        if (r.camOk) {
            int n = std::snprintf(rot, sizeof(rot), "(");
            for (size_t i = 0; i < 12 && n > 0; ++i) n += std::snprintf(rot + n, sizeof(rot) - static_cast<size_t>(n), "%s%.3f", i ? "," : "", static_cast<double>(r.rot[i]));
            std::snprintf(rot + n, sizeof(rot) - static_cast<size_t>(n), ")");
        } else {
            std::snprintf(rot, sizeof(rot), "?");
        }
        Log::get().note("native on foot hrc submit [%s]: inst=%u view=%s calls=%llu campos=%s rot=%s", g_f9StateName[r.st], static_cast<unsigned>(r.k),
                        view, static_cast<unsigned long long>(r.window), cam, rot);
    }
}

// ---- F18 (TEMPORARY): the third-person torso's cull mode (fix.native_on_foot_body_cull) ------------------------------------------------
// The match: DrawIndexedInstanced, one instance, one of two shader pairs at one of five index counts, stencil ref 21 (the local player's
// body; other people draw with the same meshes). The stencil ref is read with OMGetDepthStencilState only for a draw that already matched
// the hashes and the count -- a handful a frame -- because the binding shadow does not track it. The swap mirrors remlok_fix.cpp's
// remlokScissorBegin/End: the game's state is cloned with one field changed, bound for the draw, and the game's own put back after it.
constexpr uint64_t kBodyVsA = 0x9D800F45B12A6AACull, kBodyPsA = 0x194DD6A677084A74ull;   // the torso pieces at 13260, 30216, 23784, 7692
constexpr uint64_t kBodyVsB = 0x7B0DC42D383F694Cull, kBodyPsB = 0x0DF03E64DF9DBEF1ull;   // the piece at 37080
constexpr UINT kBodyStencilRef = 21;
constexpr size_t kBodyCounts = 5;                       // the index counts, in the log line's order
std::atomic<uint64_t> g_bodyMatched[kBodyCounts]{};     // draw thread adds, frame thread takes (exchange) every 5 s
std::atomic<uint64_t> g_bodyStencilMiss{0};             // draw thread, once: 1<<63 | count<<32 | ref of the first torso-shaped draw with another ref
bool g_bodyStencilMissNoted = false;                    // frame thread only
uint64_t g_lastBodyCullLineMs = 0;                      // frame thread only

// Draw thread only. A cache of clones keyed by the game's state and the mode; each entry holds a reference on the game's state so its
// address cannot be reused by another state while the clone is cached. A null source is the default state (no object to ask).
struct BodyCullClone {
    ID3D11RasterizerState* source = nullptr;
    ID3D11RasterizerState* clone = nullptr;
    uint8_t mode = 0;
};
constexpr size_t kBodyCullCache = 16;
BodyCullClone g_bodyCullCache[kBodyCullCache];
size_t g_bodyCullCached = 0;
bool g_bodyCullEngaged = false;                         // what Begin set, for End to put back (remlok's g_engaged contract)
ID3D11RasterizerState* g_bodyCullSavedRS = nullptr;     // the game's state, referenced by RSGetState, released by End
bool g_bodyCullCloneFailedNoted = false;
bool g_bodyCullCacheFullNoted = false;

int bodyCullIndex(uint32_t indexCount) noexcept {
    const uint64_t vs = bindingShaderHash(BindSlot::Vs);
    if (vs == kBodyVsA) {
        if (bindingShaderHash(BindSlot::Ps) != kBodyPsA) return -1;
        switch (indexCount) {
            case 13260: return 0;
            case 30216: return 1;
            case 23784: return 2;
            case 7692: return 3;
            default: return -1;
        }
    }
    if (vs == kBodyVsB && indexCount == 37080 && bindingShaderHash(BindSlot::Ps) == kBodyPsB) return 4;
    return -1;
}

ID3D11RasterizerState* bodyCullClone(ID3D11DeviceContext* ctx, ID3D11RasterizerState* source, uint8_t mode) {
    for (size_t i = 0; i < g_bodyCullCached; ++i) {
        if (g_bodyCullCache[i].source == source && g_bodyCullCache[i].mode == mode) return g_bodyCullCache[i].clone;
    }
    if (g_bodyCullCached >= kBodyCullCache) {
        if (!g_bodyCullCacheFullNoted) {
            g_bodyCullCacheFullNoted = true;
            Log::get().note("native on foot body cull: the clone cache is full (%zu game rasterizer states); further states draw untouched.",
                            kBodyCullCache);
        }
        return nullptr;
    }
    D3D11_RASTERIZER_DESC d{};
    if (source) {
        source->GetDesc(&d);
    } else {
        // The documented defaults of the null rasterizer state.
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_BACK;
        d.DepthClipEnable = TRUE;
    }
    d.CullMode = mode == 2 ? D3D11_CULL_FRONT : D3D11_CULL_NONE;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    ID3D11RasterizerState* clone = nullptr;
    const HRESULT hr = dev->CreateRasterizerState(&d, &clone);
    dev->Release();
    if (FAILED(hr) || !clone) {
        if (!g_bodyCullCloneFailedNoted) {
            g_bodyCullCloneFailedNoted = true;
            Log::get().note("native on foot body cull: could not create the culled rasterizer state (hr=0x%08X); the torso draws untouched.",
                            static_cast<unsigned>(hr));
        }
        return nullptr;
    }
    if (source) source->AddRef();
    g_bodyCullCache[g_bodyCullCached].source = source;
    g_bodyCullCache[g_bodyCullCached].clone = clone;
    g_bodyCullCache[g_bodyCullCached].mode = mode;
    ++g_bodyCullCached;
    return clone;
}

// Frame thread, every frame: the one-time stencil note, and the 5 s count line while the mode is not game and native stereo is on foot
// (written even at 0, so a flight can tell the code ran). The counts are those since the previous line.
void bodyCullLine(uint64_t nowMs) {
    const uint64_t miss = g_bodyStencilMiss.load(std::memory_order_relaxed);
    if (miss != 0 && !g_bodyStencilMissNoted) {
        g_bodyStencilMissNoted = true;
        Log::get().note("native on foot body cull: a torso-shaped draw (index count %u) had stencil ref %u, not %u, and was left alone "
                        "(logged once)",
                        static_cast<unsigned>((miss >> 32) & 0x7FFFFFFFu), static_cast<unsigned>(miss & 0xFFFFFFFFu),
                        static_cast<unsigned>(kBodyStencilRef));
    }
    const uint8_t mode = g_nativeOnFootBodyCull.load(std::memory_order_relaxed);
    if (mode == 0 || !nativeOnFootActive()) return;
    if (nowMs - g_lastBodyCullLineMs < 5000) return;
    g_lastBodyCullLineMs = nowMs;
    uint64_t n[kBodyCounts];
    uint64_t total = 0;
    for (size_t i = 0; i < kBodyCounts; ++i) {
        n[i] = g_bodyMatched[i].exchange(0, std::memory_order_relaxed);
        total += n[i];
    }
    Log::get().note("native on foot body cull: %s matched %llu draws (by index count: 13260=%llu, 30216=%llu, 23784=%llu, 7692=%llu, 37080=%llu)",
                    mode == 2 ? "front" : "none", static_cast<unsigned long long>(total), static_cast<unsigned long long>(n[0]),
                    static_cast<unsigned long long>(n[1]), static_cast<unsigned long long>(n[2]), static_cast<unsigned long long>(n[3]),
                    static_cast<unsigned long long>(n[4]));
}
}  // namespace

// F18 (declared in native_on_foot.h): the DrawIndexedInstanced thunk calls this only when nativeOnFootBodyCullWanted() and on the owner
// context. The same shape as remlokScissorBegin: read the game's state, bind the clone, keep the game's reference for End.
bool nativeOnFootBodyCullBegin(ID3D11DeviceContext* ctx, uint32_t indexCount, uint32_t instances) {
    g_bodyCullEngaged = false;
    const uint8_t mode = g_nativeOnFootBodyCull.load(std::memory_order_relaxed);
    if (mode == 0 || instances != 1) return false;
    const int index = bodyCullIndex(indexCount);
    if (index < 0) return false;
    ID3D11DepthStencilState* ds = nullptr;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&ds, &ref);
    if (ds) ds->Release();
    if (ref != kBodyStencilRef) {
        uint64_t none = 0;
        g_bodyStencilMiss.compare_exchange_strong(none, (1ull << 63) | (static_cast<uint64_t>(indexCount) << 32) | ref, std::memory_order_relaxed);
        return false;
    }
    ID3D11RasterizerState* current = nullptr;
    ctx->RSGetState(&current);   // AddRef'd when non-null
    ID3D11RasterizerState* culled = bodyCullClone(ctx, current, mode);
    if (!culled) {
        if (current) current->Release();
        return false;   // the draw runs untouched
    }
    g_bodyCullSavedRS = current;   // keep the reference until End restores it
    ctx->RSSetState(culled);
    g_bodyCullEngaged = true;
    g_bodyMatched[index].fetch_add(1, std::memory_order_relaxed);
    return true;
}

void nativeOnFootBodyCullEnd(ID3D11DeviceContext* ctx) {
    if (!g_bodyCullEngaged) return;
    g_bodyCullEngaged = false;
    ctx->RSSetState(g_bodyCullSavedRS);
    if (g_bodyCullSavedRS) {
        g_bodyCullSavedRS->Release();
        g_bodyCullSavedRS = nullptr;
    }
}

void nativeOnFootBodyCullShutdown() {
    for (size_t i = 0; i < g_bodyCullCached; ++i) {
        if (g_bodyCullCache[i].clone) g_bodyCullCache[i].clone->Release();
        if (g_bodyCullCache[i].source) g_bodyCullCache[i].source->Release();
        g_bodyCullCache[i] = BodyCullClone{};
    }
    g_bodyCullCached = 0;
    g_bodyCullEngaged = false;
    if (g_bodyCullSavedRS) {
        g_bodyCullSavedRS->Release();
        g_bodyCullSavedRS = nullptr;
    }
}

// The probe's capture (declared in native_on_foot.h, called from beginPanelOverride on the draw thread for the two HUD issuers). One load,
// one clock read and a CAS per match; the stack is captured outside the lock.
void nativeOnFootHudCapture(int index, uint64_t vs) {
    if (index < 0 || index >= 2) return;
    const uint64_t now = GetTickCount64();
    uint64_t last = g_hudLastMs[index].load(std::memory_order_relaxed);
    if (last != 0 && now - last < kHudIntervalMs) return;
    if (!g_hudLastMs[index].compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
    void* stack[kHudFrames] = {};
    const USHORT got = CaptureStackBackTrace(0, static_cast<ULONG>(kHudFrames), stack, nullptr);
    HudStackRecord rec;
    rec.vs = vs;
    rec.ps = bindingShaderHash(BindSlot::Ps);
    rec.n = got;
    for (uint32_t i = 0; i < got && i < kHudFrames; ++i) rec.frames[i] = reinterpret_cast<uintptr_t>(stack[i]);
    std::lock_guard<std::mutex> lock(g_hudMutex);
    if (g_hudWritten - g_hudRead >= kHudRing) {
        ++g_hudDropped;
        return;
    }
    g_hudRing[g_hudWritten % kHudRing] = rec;
    ++g_hudWritten;
}

// ---- F20 (TEMPORARY): the game's own third-person model mode (fix.native_on_foot_body_mode = game | third) ----------------------------
// HumanoidModelManager::SetMode (vtable slot 0x30 of the manager at EliteDangerous64.exe+0x5200F90, build 332841) sets the model mode: 0 =
// first-person models, 1 = third-person models (the first-person avatar hidden, the third-person body and its weapon shown). It does not touch
// the camera type, so the first-person view, the HUD gate and isFirstPerson stay as they are. Two hooks, both relays (ecm::buildCallbackRelay):
//   - a detour on SetMode: the game's requests for the local player are recorded and, while the body is wanted as third person, a request for
//     0 (first-person models) is forwarded as 1, or as 0 while aiming down sights. EDVR's own calls go through the trampoline, never the detour.
//   - an entry hook on the povCamera handler (EliteDangerous64.exe+0x19B1240, every humanoid, every frame): the original runs first, then the
//     local player's mode is held at the desired value through the SetMode trampoline.
// The hook threads never log: they queue events, and the frame boundary writes them.
namespace {
constexpr uintptr_t kModeSetRva = 0x1A112B0;                 // HumanoidModelManager::SetMode
constexpr uintptr_t kModeVtableRva = 0x5200F90;              // the manager's vtable; SetMode is its slot 0x30
constexpr uintptr_t kModeSetSlotOffset = 0x30;
constexpr size_t kModeSetPrologueBytes = 22;
constexpr uint8_t kModeSetPrologue[kModeSetPrologueBytes] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x39, 0x91,
                                                            0x30, 0x02, 0x00, 0x00, 0x75, 0x05, 0x45, 0x84, 0xC0, 0x74, 0x1F};
constexpr uintptr_t kPovRva = 0x19B1240;                     // the povCamera joint handler, entered every frame for every humanoid
constexpr size_t kPovPrologueBytes = 32;
constexpr uint8_t kPovPrologue[kPovPrologueBytes] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x85, 0xD2, 0x48, 0x89, 0x74, 0x24, 0x30,
                                                    0x48, 0x8D, 0xB2, 0xB0, 0x01, 0x00, 0x00, 0xB8, 0xA0, 0x02, 0x00, 0x00, 0x48, 0x0F,
                                                    0x44, 0xF0, 0x48, 0x8D};
constexpr uintptr_t kHmmIfaceOff = 0x1B0;                    // rdx + this = the manager pointer (rdx = the receiver)
constexpr uintptr_t kHmmLocalOff = 0x238;                    // non-zero only for the local player (the first-person avatar)
constexpr uintptr_t kHmmModeOff = 0x230;                     // the current model mode (dword)
constexpr uint64_t kModeUnseenFrames = 60;                   // frames the captured local manager may go unseen before it is dropped
constexpr size_t kModeEvents = 16;

using ModeSetFn = void(__fastcall*)(void*, uint32_t, uint8_t);
using PovFn = uint64_t(__fastcall*)(void*, void*);
ModeSetFn g_modeSetOriginal = nullptr;                       // the SetMode trampoline (the original bytes)
PovFn g_povOriginal = nullptr;                               // the povCamera trampoline
uint8_t* g_modeSetRelay = nullptr;
uint8_t* g_povRelay = nullptr;
alignas(8) std::atomic<uintptr_t> g_modeSetGate{0};          // zero: the relay runs the original
alignas(8) std::atomic<uintptr_t> g_povGate{0};
CodeHook* g_modeSetHook = nullptr;                           // never deleted (the same choice as g_hook)
CodeHook* g_povHook = nullptr;
bool g_modeInstallTried = false;                             // frame thread
bool g_modeCfgRead = false;                                  // frame thread
uint32_t g_modeCfgGen = 0;                                   // frame thread
bool g_modeThirdLast = false;                                // frame thread: the key as last logged
std::atomic<bool> g_modeArmed{false};                        // both hooks are live (set last, by the frame thread's install)
std::atomic<bool> g_modeThird{false};                        // fix.native_on_foot_body_mode is third (frame thread writes)
std::atomic<bool> g_modeWant{false};                         // third, native stereo on foot, session: the body is held (frame thread writes)
std::atomic<bool> g_modeAds{false};                          // aiming down sights now (journal Flags2 bit 5; frame thread writes)
std::atomic<uint32_t> g_modeWhy{0};                          // why the hold ends: 1 key game, 2 native ended, 3 session ended, 4 shutdown
std::atomic<uintptr_t> g_modeIface{0};                       // the captured local manager (written by the povCamera hook)
std::atomic<uint64_t> g_modeFrame{0};                        // frame boundaries (frame thread)
std::atomic<uint64_t> g_modeSeenFrame{0};                    // the frame the captured manager was last seen in
std::atomic<bool> g_modeOverride{false};                     // the hold is applied (hook threads)
std::atomic<bool> g_modeGameKnown{false};                    // the game's own mode is known (a request, or read before the first hold)
std::atomic<uint32_t> g_modeGame{0};                         // the game's own mode (last request, or read before the first hold)
std::atomic<uint32_t> g_modeLastReq{0xFFFFFFFFu};            // detour: last logged request, forward and thread
std::atomic<uint32_t> g_modeLastFwd{0xFFFFFFFFu};
std::atomic<uint32_t> g_modeLastThread{0};

// The events the hooks queue for the frame boundary to log.
enum class ModeEv : uint8_t { Asked, Set, Restore, Captured, Dropped };
struct ModeEvent {
    ModeEv kind = ModeEv::Asked;
    uint32_t a = 0, b = 0, thread = 0, why = 0;
    uint64_t iface = 0;
};
std::mutex g_modeEvMutex;
ModeEvent g_modeEv[kModeEvents];                             // guarded by g_modeEvMutex
size_t g_modeEvN = 0;                                        // guarded by g_modeEvMutex
uint32_t g_modeEvDropped = 0;                                // guarded by g_modeEvMutex

void modePush(const ModeEvent& e) {
    std::lock_guard<std::mutex> lock(g_modeEvMutex);
    if (g_modeEvN < kModeEvents) g_modeEv[g_modeEvN++] = e;
    else ++g_modeEvDropped;
}

bool prepareModeSetRelay(void* trampoline, void*) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(g_modeSetRelay + ecm::kCallbackRelayTrampolineAt, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(g_modeSetRelay, 4096, PAGE_EXECUTE_READ, &oldProtect) || !FlushInstructionCache(GetCurrentProcess(), g_modeSetRelay, ecm::kCallbackRelayBytes))
        return false;
    g_modeSetOriginal = reinterpret_cast<ModeSetFn>(address);
    return true;
}
bool preparePovRelay(void* trampoline, void*) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(g_povRelay + ecm::kCallbackRelayTrampolineAt, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(g_povRelay, 4096, PAGE_EXECUTE_READ, &oldProtect) || !FlushInstructionCache(GetCurrentProcess(), g_povRelay, ecm::kCallbackRelayBytes))
        return false;
    g_povOriginal = reinterpret_cast<PovFn>(address);
    return true;
}

// The SetMode detour. Its arguments are the game's: rcx = manager, edx = the requested mode, r8b = force.
void __fastcall modeSetCallback(void* iface, uint32_t mode, uint8_t force) noexcept {
    uint32_t forward = mode;
    const uintptr_t key = reinterpret_cast<uintptr_t>(iface);
    if (key != 0 && key == g_modeIface.load(std::memory_order_acquire)) {
        const uint32_t thread = static_cast<uint32_t>(GetCurrentThreadId());
        if (mode == 0 && g_modeWant.load(std::memory_order_acquire)) forward = g_modeAds.load(std::memory_order_relaxed) ? 0u : 1u;
        g_modeGame.store(mode, std::memory_order_relaxed);
        g_modeGameKnown.store(true, std::memory_order_relaxed);
        if (mode != g_modeLastReq.load(std::memory_order_relaxed) || forward != g_modeLastFwd.load(std::memory_order_relaxed) ||
            thread != g_modeLastThread.load(std::memory_order_relaxed)) {
            g_modeLastReq.store(mode, std::memory_order_relaxed);
            g_modeLastFwd.store(forward, std::memory_order_relaxed);
            g_modeLastThread.store(thread, std::memory_order_relaxed);
            ModeEvent e;
            e.kind = ModeEv::Asked;
            e.a = mode;
            e.b = forward;
            e.thread = thread;
            modePush(e);
        }
    }
    g_modeSetOriginal(iface, forward, force);
}

// The per-frame step for the local manager, on the povCamera hook's thread: hold the desired mode, or restore the game's once the hold ends.
void modeFrameStep(uintptr_t iface) noexcept {
    const uint32_t thread = static_cast<uint32_t>(GetCurrentThreadId());
    g_modeSeenFrame.store(g_modeFrame.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (g_modeIface.load(std::memory_order_acquire) != iface) {
        // A different local manager: the old one is not restored (its object may be gone); the hold starts again on this one.
        g_modeOverride.store(false, std::memory_order_relaxed);
        g_modeGameKnown.store(false, std::memory_order_relaxed);
        g_modeLastReq.store(0xFFFFFFFFu, std::memory_order_relaxed);
        g_modeLastFwd.store(0xFFFFFFFFu, std::memory_order_relaxed);
        g_modeIface.store(iface, std::memory_order_release);
        ModeEvent e;
        e.kind = ModeEv::Captured;
        e.iface = iface;
        e.thread = thread;
        modePush(e);
    }
    uint32_t cur = 0;
    if (!guardedRead(iface + kHmmModeOff, &cur, sizeof(cur))) return;
    if (g_modeWant.load(std::memory_order_acquire)) {
        const bool ads = g_modeAds.load(std::memory_order_relaxed);
        const uint32_t desired = ads ? 0u : 1u;
        if (!g_modeOverride.load(std::memory_order_relaxed)) {
            if (!g_modeGameKnown.load(std::memory_order_relaxed)) {
                g_modeGame.store(cur, std::memory_order_relaxed);   // no request seen yet: the game's mode is what it is now
                g_modeGameKnown.store(true, std::memory_order_relaxed);
            }
            g_modeOverride.store(true, std::memory_order_relaxed);
        }
        if (cur != desired) {
            g_modeSetOriginal(reinterpret_cast<void*>(iface), desired, 1);   // the trampoline: the detour never sees this call
            ModeEvent e;
            e.kind = ModeEv::Set;
            e.a = desired;
            e.b = ads ? 1u : 0u;
            e.thread = thread;
            modePush(e);
        }
    } else if (g_modeOverride.load(std::memory_order_relaxed)) {
        const uint32_t back = g_modeGame.load(std::memory_order_relaxed);
        g_modeSetOriginal(reinterpret_cast<void*>(iface), back, 1);
        g_modeOverride.store(false, std::memory_order_relaxed);
        ModeEvent e;
        e.kind = ModeEv::Restore;
        e.a = back;
        e.why = g_modeWhy.load(std::memory_order_relaxed);
        e.thread = thread;
        modePush(e);
    }
}

// The povCamera hook. rcx = the event context (unused), rdx = the humanoid (the receiver). The original runs first; its result is returned.
uint64_t __fastcall povCallback(void* ctx, void* rcv) noexcept {
    uintptr_t iface = 0;
    bool local = false;
    if (rcv && guardedRead(reinterpret_cast<uintptr_t>(rcv) + kHmmIfaceOff, &iface, sizeof(iface)) && iface) {
        uintptr_t vt = 0, avatar = 0;
        if (guardedRead(iface, &vt, sizeof(vt)) && vt == g_base + kModeVtableRva && guardedRead(iface + kHmmLocalOff, &avatar, sizeof(avatar)) &&
            avatar != 0)
            local = true;
    }
    const uint64_t result = g_povOriginal(ctx, rcv);
    if (local && g_modeArmed.load(std::memory_order_acquire)) modeFrameStep(iface);
    return result;
}

void modeRefuse(const char* why) {
    Log::get().note("native on foot body mode: NOT installed -- %s. The game was not touched; the model mode stays the game's.", why);
}

// Frame thread, once, when the key first reads third. Identity, the vtable slot and both prologues are checked before anything is patched.
void installModeHooks() {
    g_modeInstallTried = true;
    if (!runtimeVrProfile()) {
        modeRefuse("the flat profile (the model mode is a VR fix)");
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        modeRefuse("the game module could not be resolved");
        return;
    }
    const char* peWhy = nullptr;
    if (!checkIdentity(base, &peWhy)) {
        char b[400];
        std::snprintf(b, sizeof(b), "identity mismatch (%s)", peWhy ? peWhy : "unknown");
        modeRefuse(b);
        return;
    }
    uintptr_t slot = 0;
    if (!guardedRead(base + kModeVtableRva + kModeSetSlotOffset, &slot, sizeof(slot)) || slot != base + kModeSetRva) {
        modeRefuse("vtable slot 0x30 of the manager at EliteDangerous64.exe+0x5200F90 is not SetMode at +0x1A112B0");
        return;
    }
    if (!bytesMatch(base + kModeSetRva, kModeSetPrologue, kModeSetPrologueBytes)) {
        modeRefuse("the 22-byte prologue at EliteDangerous64.exe+0x1A112B0 is not build 332841's SetMode");
        return;
    }
    if (!bytesMatch(base + kPovRva, kPovPrologue, kPovPrologueBytes)) {
        modeRefuse("the 32-byte prologue at EliteDangerous64.exe+0x19B1240 is not build 332841's povCamera handler");
        return;
    }
    g_base = base;
    g_modeSetRelay = allocateRelay(base + kModeSetRva);
    g_povRelay = allocateRelay(base + kPovRva);
    if (!g_modeSetRelay || !g_povRelay) {
        modeRefuse("no executable memory within two gigabytes of the targets for the relays");
        return;
    }
    ecm::buildCallbackRelay(g_modeSetRelay, &g_modeSetGate, reinterpret_cast<const void*>(&modeSetCallback));
    ecm::buildCallbackRelay(g_povRelay, &g_povGate, reinterpret_cast<const void*>(&povCallback));
    g_modeSetHook = new CodeHook();
    if (!g_modeSetHook->install(reinterpret_cast<void*>(base + kModeSetRva), g_modeSetRelay, nullptr, "native-on-foot-body-mode-set",
                                &prepareModeSetRelay, nullptr)) {
        const char* why = g_modeSetHook->refusal()[0] ? g_modeSetHook->refusal() : "CodeHook gave no reason";
        char b[480];
        std::snprintf(b, sizeof(b), "CodeHook refused SetMode: %s", why);
        modeRefuse(b);
        return;
    }
    Log::get().note("native on foot body mode: installed -- SetMode at EliteDangerous64.exe+0x1A112B0 (vtable slot 0x30 of the manager at +0x5200F90), "
                    "stolen=%zu bytes, prologue %zu bytes verified, relay at 0x%llX (thread %u)",
                    g_modeSetHook->stolenBytes(), kModeSetPrologueBytes, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_modeSetRelay)),
                    static_cast<unsigned>(GetCurrentThreadId()));
    g_povHook = new CodeHook();
    if (!g_povHook->install(reinterpret_cast<void*>(base + kPovRva), g_povRelay, nullptr, "native-on-foot-body-mode-pov", &preparePovRelay, nullptr)) {
        // SetMode is patched but its gate is still zero: the detour passes every call through. Nothing is held.
        const char* why = g_povHook->refusal()[0] ? g_povHook->refusal() : "CodeHook gave no reason";
        char b[480];
        std::snprintf(b, sizeof(b), "CodeHook refused the povCamera hook: %s (SetMode passes through; no hold)", why);
        modeRefuse(b);
        return;
    }
    Log::get().note("native on foot body mode: installed -- the per-frame hook at EliteDangerous64.exe+0x19B1240 (the povCamera handler), "
                    "stolen=%zu bytes, prologue %zu bytes verified, relay at 0x%llX (thread %u)",
                    g_povHook->stolenBytes(), kPovPrologueBytes, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_povRelay)),
                    static_cast<unsigned>(GetCurrentThreadId()));
    g_modeSetGate.store(1, std::memory_order_seq_cst);
    g_povGate.store(1, std::memory_order_seq_cst);
    g_modeArmed.store(true, std::memory_order_release);
}

const char* modeReasonText(uint32_t why) {
    switch (why) {
        case 1: return "fix.native_on_foot_body_mode is game";
        case 2: return "native stereo on foot ended";
        case 3: return "the game session ended";
        case 4: return "the game is shutting down";
        default: return "unknown";
    }
}

// Frame thread, every frame: the key, the install, the wants for the hooks, the drop of a manager that has gone unseen, and the queued lines.
void bodyModeTick(uint64_t now, bool session) {
    (void)now;
    Config& cfg = Config::get();
    const uint32_t gen = cfg.generation();
    if (!g_modeCfgRead || gen != g_modeCfgGen) {
        g_modeCfgRead = true;
        g_modeCfgGen = gen;
        const bool third = cfg.getString("fix.native_on_foot_body_mode", "game") == "third";
        g_modeThird.store(third, std::memory_order_relaxed);
        if (third != g_modeThirdLast) {
            Log::get().note("native on foot: fix.native_on_foot_body_mode is %s (temporary)", third ? "third" : "game");
            g_modeThirdLast = third;
        }
    }
    const bool third = g_modeThird.load(std::memory_order_relaxed);
    const uint64_t frame = g_modeFrame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (third && !g_modeInstallTried) installModeHooks();
    const bool native = nativeOnFootActive();
    const uint32_t why = !third ? 1u : !native ? 2u : !session ? 3u : 0u;
    g_modeWhy.store(why, std::memory_order_relaxed);
    g_modeWant.store(third && native && session && g_modeArmed.load(std::memory_order_acquire), std::memory_order_release);
    g_modeAds.store(journalAdsKnown() && journalAds(), std::memory_order_relaxed);
    const uintptr_t cap = g_modeIface.load(std::memory_order_acquire);
    if (cap != 0 && frame - g_modeSeenFrame.load(std::memory_order_relaxed) > kModeUnseenFrames) {
        // Not restored: the manager has not been called for 60 frames, so it may be gone.
        g_modeIface.store(0, std::memory_order_release);
        g_modeOverride.store(false, std::memory_order_relaxed);
        g_modeGameKnown.store(false, std::memory_order_relaxed);
        ModeEvent e;
        e.kind = ModeEv::Dropped;
        e.iface = cap;
        e.thread = static_cast<uint32_t>(GetCurrentThreadId());
        modePush(e);
    }
    // Drain.
    ModeEvent ev[kModeEvents];
    size_t n = 0;
    uint32_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(g_modeEvMutex);
        n = g_modeEvN;
        for (size_t i = 0; i < n; ++i) ev[i] = g_modeEv[i];
        g_modeEvN = 0;
        dropped = g_modeEvDropped;
        g_modeEvDropped = 0;
    }
    for (size_t i = 0; i < n; ++i) {
        const ModeEvent& e = ev[i];
        switch (e.kind) {
            case ModeEv::Asked:
                Log::get().note("native on foot body mode: game asked %u (thread %u) -> forwarded %u", e.a, e.thread, e.b);
                break;
            case ModeEv::Set:
                Log::get().note("native on foot body mode: set %u (ADS %u, frame thread %u)", e.a, e.b, e.thread);
                break;
            case ModeEv::Restore:
                Log::get().note("native on foot body mode: restored to %u (%s, frame thread %u)", e.a, modeReasonText(e.why), e.thread);
                break;
            case ModeEv::Captured:
                Log::get().note("native on foot body mode: local third-person humanoid seen at 0x%llX (thread %u)",
                                static_cast<unsigned long long>(e.iface), e.thread);
                break;
            case ModeEv::Dropped:
                Log::get().note("native on foot body mode: the local humanoid 0x%llX was not seen for %llu frames; dropped without a restore (thread %u)",
                                static_cast<unsigned long long>(e.iface), static_cast<unsigned long long>(kModeUnseenFrames), e.thread);
                break;
        }
    }
    if (dropped != 0) Log::get().note("native on foot body mode: %u events dropped (the queue was full)", static_cast<unsigned>(dropped));
}

// Shutdown: the hold ends at the manager's next call (the restore is the hook's, once).
void bodyModeShutdown() {
    g_modeWhy.store(4, std::memory_order_relaxed);
    g_modeWant.store(false, std::memory_order_release);
}
}  // namespace

void nativeOnFootFrameBoundary(uint32_t frameNo) {
    (void)frameNo;
    if (g_install == InstallState::NotTried) installOnce();
    if (g_install == InstallState::Armed) readKey();
    drainEvents();
    if (!g_hudArmedNoted) {
        g_hudArmedNoted = true;
        Log::get().note("native on foot hud stack: probe armed on the VR draw path for vs=0x%016llX and vs=0x%016llX (one capture per shader per "
                        "10 s; the flat profile is not probed)",
                        static_cast<unsigned long long>(nofoot::kHudIssuerVs[0]), static_cast<unsigned long long>(nofoot::kHudIssuerVs[1]));
    }
    hudStackDrain();
    const uint64_t now = GetTickCount64();
    const bool keyOn = g_keyOn.load(std::memory_order_relaxed);
    const bool gate = g_methodGate.load(std::memory_order_relaxed);
    const bool session = nativeGameSession();
    // The native flag. Under the gate method it is computed here (native_on_foot.h, THE FLAG): the byte is ours, the game's last forwarded
    // mode was HMD, and the journal says on foot. Under swap the setter sets and clears it, as F0 did.
    if (gate) {
        g_nativeOnFootActive.store(g_gateWritten && g_lastForwardedHmd.load(std::memory_order_relaxed) && journalOnFootKnown() && journalOnFoot(),
                                   std::memory_order_relaxed);
    } else if (g_methodWasGate) {
        g_nativeOnFootActive.store(false, std::memory_order_relaxed);
    }
    g_methodWasGate = gate;
    // F26: a native on-foot enter or leave is an edge; the UI layer's eye check re-arms on it.
    if (nativeOnFootActive() != g_nativeEdgeLast) {
        g_nativeEdgeLast = nativeOnFootActive();
        g_nativeOnFootEdges.fetch_add(1, std::memory_order_relaxed);
    }
    const char* byteWhy = !keyOn ? "fix.native_on_foot is off" : !gate ? "fix.native_on_foot_method is swap" : "the game session ended";
    gateByteTick(keyOn && gate && session, byteWhy);
    // F8 (TEMPORARY): the first-person HUD override, while native stereo is on foot and the key is on (hudOverrideTick restores otherwise).
    const bool hudKeyOn = g_hudKeyOn.load(std::memory_order_relaxed);
    const char* hudWhy = !hudKeyOn ? "fix.native_on_foot_hud is off" : !nativeOnFootActive() ? "native stereo on foot ended" : "the game session ended";
    hudOverrideTick(hudKeyOn && nativeOnFootActive() && session, hudWhy);
    // The body (F2): Explorer Cam's fade and head-part code runs while native stereo is on foot. Its head hooks are wanted from launch while the key is on.
    const char* bodyWhy = !keyOn ? "fix.native_on_foot is off" : !session ? "the game session ended" : "native stereo ended (boarded or seated, or the game asked for its own mode)";
    explorerCamNativeBody(keyOn, nativeOnFootActive(), bodyWhy);
    gateLine(now);
    avatarLine(now);
    avatarCensusLine(now);
    hudCensusTick(now, session);
    // F9 (TEMPORARY): the two code patches, while native stereo is on foot and the first-person HUD override (F8) is held; and the probes.
    installF9ProbesOnce();
    const bool f9Active = nativeOnFootActive() && g_hudOverrideHeld && session;
    const char* f9Why = !g_hudOverrideHeld ? "the first-person HUD override is off" : !nativeOnFootActive() ? "native stereo on foot ended" : "the game session ended";
    codePatchTick(g_panelPatch, g_panelsKeyOn.load(std::memory_order_relaxed) && f9Active,
                  g_panelsKeyOn.load(std::memory_order_relaxed) ? f9Why : "fix.native_on_foot_panels is off");
    meshSiteTick(g_meshCreate, g_meshKeyOn.load(std::memory_order_relaxed) && f9Active,
                 g_meshKeyOn.load(std::memory_order_relaxed) ? f9Why : "fix.native_on_foot_hud_mesh is off");
    g_meshCreateLive.store(g_meshCreate.applied, std::memory_order_relaxed);   // F12: the HUD distance writes only while the creation patch is in

    f9ProbeTick(now);
    f13FrameTick(now);
    // F22: the training load wait and probe A (read only, VR, once); the look's two hooks (once, when its key first reads on); the look controller;
    // and the total the runtime reads for its compensation (every frame).
    installF22ProbesOnce();
    loadStagesFrameTick(now);   // F23 (Part E): the no-call check for the stage hook
    f28FrameTick(now);   // F28: the training connect probe's one-second lines

    // F24 (TEMPORARY, read only): the load sampler runs in a menu (no Flags2 in Status.json) and while a load pipeline is running, until its summary.
    {
        bool pipeline = false;
        {
            std::lock_guard<std::mutex> lock(g_stMutex);
            pipeline = g_stObj != 0 && !g_stSumDone;
        }
        const bool menuLike = !journalOnFootKnown();
        const bool want = menuLike || pipeline;
        loadSamplerTick(now, want, want ? (pipeline ? "a load pipeline is running (its summary has not printed)"
                                                    : "no Flags2 in Status.json: a menu or a loading screen")
                                       : "in gameplay: Status.json has Flags2 and no load pipeline is running");
    }
    if (g_lookKeyOn.load(std::memory_order_relaxed)) installLookHooksOnce();
    lookToAimTick(now, session);
    lookPassiveTick(now);   // F24: the passive sign check (mouse-only turns; no injection)
    lookPublishTick();
    // F14: the layout probe, its lines, the HUD spread, and the reticle probe (temporary).
    installF14ProbesOnce();
    installEyeProbeOnce();   // F18 (TEMPORARY): probe P1, read only
    installBodyShiftOnce();  // F35 (TEMPORARY): the body shift's post-hook on the submit job (body mode only)
    f14FrameTick(now);
    {
        const float spreadNow = g_hudSpread.load(std::memory_order_relaxed);
        const bool spreadWant = spreadNow > 0.0f && nativeOnFootActive() && g_meshCreateLive.load(std::memory_order_relaxed) && session;
        spreadTick(spreadWant, spreadNow,
                   !(spreadNow > 0.0f) ? "the HUD spread is 0" : !nativeOnFootActive() ? "native stereo on foot ended"
                   : !g_meshCreateLive.load(std::memory_order_relaxed) ? "the HUD mesh is off" : "the game session ended");
        // F17: the HUD element scale (the user setting), while native stereo is on foot and the HUD mesh patch is in. Before the layout re-send below.
        // F18: the HUD room. The base scale is the setting, or the game's own S when the setting is 0 (and S has been read). The room divides the base
        // (k_eff = base / room) and multiplies the mesh (g_hudRoomEff). With the room at 1 and the setting at 0 nothing is written, as before.
        const float elemSet = g_hudElemScale.load(std::memory_order_relaxed);
        const float roomSet = g_hudRoomSetting.load(std::memory_order_relaxed);
        const bool onHud = nativeOnFootActive() && g_meshCreateLive.load(std::memory_order_relaxed) && session;
        const float sGame = g_sGame.load(std::memory_order_relaxed);
        const float kBase = elemSet > 0.0f ? elemSet : sGame;
        const float roomEff = (roomSet > 1.0f && kBase > 0.0f && onHud) ? roomSet : 1.0f;
        const float elemK = std::min(2.0f, std::max(nofoot::kHudElemScaleMin, kBase / roomEff));
        const bool elemWant = onHud && kBase > 0.0f && (elemSet > 0.0f || roomEff > 1.0f);
        elemScaleTick(elemWant, elemK,
                      !elemWant ? (!nativeOnFootActive() ? "native stereo on foot ended" : !g_meshCreateLive.load(std::memory_order_relaxed) ? "the HUD mesh is off"
                                   : !session ? "the game session ended" : "the setting is 0 and the room is 1")
                      : "the game session ended");
        g_elemOverrideOn.store(g_elemWritten, std::memory_order_relaxed);
        g_hudRoomEff.store(onHud ? roomEff : 1.0f, std::memory_order_relaxed);
        if (onHud && (roomEff != g_roomLastR || elemK != g_roomLastK || g_hudSize.load(std::memory_order_relaxed) * roomEff != g_roomLastS)) {
            // Logged on change only; the room's own line (and its reason when the room waits for S).
            const float sizeEff = g_hudSize.load(std::memory_order_relaxed) * roomEff;
            if (roomSet > 1.0f && kBase <= 0.0f)
                Log::get().note("native on foot hud room: %.2f waits for the game's own element scale (not read yet); the room is not applied", static_cast<double>(roomSet));
            else
                Log::get().note("native on foot hud room: %.2f (element scale %.2f, size %.2f)", static_cast<double>(roomEff), static_cast<double>(elemWant ? elemK : 0.0f), static_cast<double>(sizeEff));
            g_roomLastR = roomEff;
            g_roomLastK = elemK;
            g_roomLastS = sizeEff;
        }
        // F18: the reticle stub is in place whenever native stereo is on foot (both modes write key 10), or while the probe key is on.
        const bool reticleWant = nativeOnFootActive() && session;
        reticleTick(reticleWant, !nativeOnFootActive() ? "native stereo on foot ended" : "the game session ended");
        const bool writeNow = nativeOnFootActive() && session;
        g_reticleWriteOn.store(writeNow, std::memory_order_relaxed);
        if (writeNow != g_reticleWriteLast) {
            if (writeNow) g_reticleLogWanted.store(true, std::memory_order_relaxed);   // the probe queues the line on its next call (aim or head)
            else
                Log::get().note("native on foot reticle: writes stop (%s)",
                                !nativeOnFootActive() ? "native stereo on foot ended" : "the game session ended");
            g_reticleWriteLast = writeNow;
        }
        // F16: the HUD group offsets are applied by the layout hook while native stereo is on foot; any change of that state, of an offset or of the
        // spread asks for one layout re-send.
        const bool layoutOn = nativeOnFootActive() && session;
        {
            F9Lock lock;
            g_hudOffsetOn = layoutOn;
        }
        layoutResendTick(layoutOn, g_elemWritten ? g_elemWrittenK : 0.0f);
    }
    // F16: the visor override in breathable air (replaces the F13 test key). Held while the setting is up, native stereo is on foot, and Status.json's
    // Flags2 says breathable air. The override itself is the F13 code path (visorTick: B8 = 0, then B7 = 1; restored when this stops).
    const bool visorUp = g_visorUpSetting.load(std::memory_order_relaxed);
    const bool breathable = journalBreathableKnown() && journalBreathable();
    if (nativeOnFootActive() && breathable != g_visorBreathLast) {
        Log::get().note("native on foot visor: breathable air %s (Flags2 0x%08X)", breathable ? "on" : "off", static_cast<unsigned>(journalFlags2()));
        g_visorBreathLast = breathable;
    }
    const bool visorWant = visorUp && breathable && nativeOnFootActive() && session;
    visorTick(visorWant, !visorUp ? "the visor in breathable air is game" : !breathable ? "breathable air off" : !nativeOnFootActive() ? "native stereo on foot ended" : "the game session ended");
    // F15 (TEMPORARY): the first-person body hide (explorer_cam.cpp), while native stereo is on foot and its key is on.
    // F21 (TEMPORARY): the hybrid body (explorer_cam.cpp, explorerCamHybridTick). While on, it overrides the two body keys below.
    const bool hybridKey = g_hybridKeyOn.load(std::memory_order_relaxed);
    const bool hybridWant = hybridKey && nativeOnFootActive();
    explorerCamHybridTick(now, hybridWant, !hybridKey ? "fix.native_on_foot_body_hybrid is off" : "native stereo on foot ended");
    const bool fpKey = g_fpKeyOn.load(std::memory_order_relaxed);
    explorerCamFpBody((fpKey || hybridWant) && nativeOnFootActive(),
                      !(fpKey || hybridWant) ? "fix.native_on_foot_hide_fp_body is off" : "native stereo on foot ended");
    // F19 (TEMPORARY): the body's clip modifiers (unclip, written while native stereo is on foot) and their 5 s probe (explorer_cam.cpp).
    const bool unclipKey = g_unclipKeyOn.load(std::memory_order_relaxed);
    explorerCamBodyClipTick(now, unclipKey && !hybridWant && nativeOnFootActive(), nativeOnFootActive(),
                            !(unclipKey && !hybridWant) ? "fix.native_on_foot_body_unclip is off or the hybrid body is on" : "native stereo on foot ended");
    // F19 (TEMPORARY): the constant-buffer probe's flag (on foot, native or Explorer Cam) and the drain of its readings.
    bodyModeTick(now, session);   // F20 (TEMPORARY): the model mode (fix.native_on_foot_body_mode), the hooks' wants and their lines
    bodyCullLine(now);   // F18 (TEMPORARY): the torso cull's 5 s count line (the swap itself is on the draw thread, nativeOnFootBodyCullBegin)
    neckLine(now);
    eyeProbeLine();   // F18 (TEMPORARY): P1's one-a-second line
    alignTick(GetTickCount64());   // F33: the body align (TEMPORARY, live)
}

void nativeOnFootShutdown() {
    gateByteTick(false, "the game is shutting down");
    hudOverrideTick(false, "the game is shutting down");
    codePatchTick(g_panelPatch, false, "the game is shutting down");
    meshSiteTick(g_meshCreate, false, "the game is shutting down");

    visorTick(false, "the game is shutting down");
    spreadTick(false, 0.0f, "the game is shutting down");
    elemScaleTick(false, 0.0f, "the game is shutting down");
    g_eyeStop.store(true, std::memory_order_relaxed);          // F18: P1 samples nothing from here
    g_elemOverrideOn.store(false, std::memory_order_relaxed);   // F18: the room's state is per session
    g_hudRoomEff.store(1.0f, std::memory_order_relaxed);
    g_sGame.store(0.0f, std::memory_order_relaxed);
    reticleTick(false, "the game is shutting down");
    g_reticleWriteOn.store(false, std::memory_order_relaxed);   // F16: no more centring writes
    {
        F9Lock lock;
        g_hudOffsetOn = false;                                  // F16: the layout hook applies no offsets from here
    }
    explorerCamFpBody(false, "the game is shutting down");
    explorerCamHybridTick(GetTickCount64(), false, "the game is shutting down");            // F21
    explorerCamBodyClipTick(GetTickCount64(), false, false, "the game is shutting down");   // F19
    bodyModeShutdown();                                                                    // F20
    g_lookEngaged.store(false, std::memory_order_relaxed);                                 // F22: no more input; the hooks pass through
    g_lookInput.store(0.0f, std::memory_order_relaxed);
    g_lookWantRead.store(false, std::memory_order_relaxed);
}


}  // namespace edvr
