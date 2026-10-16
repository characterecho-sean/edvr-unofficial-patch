#include "native_on_foot.h"

#include <windows.h>
#include <d3d11.h>   // F18: the body cull's rasterizer-state clones
#include <d3d11_1.h>   // F44: VSGetConstantBuffers1 (a ranged constant buffer's first constant)
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>   // F45: the marker probe's line builder
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>   // F46: the marker probe's per-frame draw records (no cap)

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/native_frame.h"   // F22: nativeFrameSetLookYaw, the look-to-aim compensation total the runtime asks for each frame
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "binding_shadow.h"   // bindingShaderHash: the HUD issuer probe's pixel shader
#include "depth_probe.h"   // F44: depthProbeCurrentSceneEyeOf, the eye of a draw by its depth view
#include "draw_record_tag.h"   // F44: the marker probe's tag, find and match (after d3d11.h: its staging ring)
#include "engine_velocity.h"   // F45: engineVelocityShadowSceneRows, the CB shadow's rows 270..275 (the world view's row 275)
#include "engine_velocity_families.h"   // F44: the pool families' vertex shaders (the eye's world view)
#include "explorer_cam.h"   // localAvatarParts, avatarFadeModeRead, avatarCensusTake: the avatar census (F1, F3)
#include "journal_watch.h"
#include "load_sampler.h"   // F24: the load sampler's tick (read only, temporary)
#include "native_on_foot_core.h"
#include "temporal_pass.h"   // F52: temporalPassEyeRows, the temporal pass's camera rows for an eye (the marker correction's reference)
#include "ui_depth.h"  // F46: uiDepthEyeOfTargetReadOnly, ui depth's frame table (the marker probe's eyes; read only)
#include "ui_layer.h"   // F46: uiLayerEyeOfGameTarget, the UI layer's redirect mapping (the marker probe's eyes; read only)
#include "vscreen.h"    // F46: vScreenIsEyeSized, the world capture's eye-sized targets

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
std::atomic<int> g_alignAnchorMode{0};   // F48: fix.native_on_foot_body_align_anchor: 0 neck (F53: the default, rest geometry), 1 clavicles (F43/F45), 2 pov
std::atomic<bool> g_alignVertical{false};   // F50 (TEMPORARY): fix.native_on_foot_body_align_vertical: off (the default) locks the body's height
std::atomic<bool> g_raiseFpKey{false};      // F51 (TEMPORARY): fix.native_on_foot_body_raise_fp: F53 default off; on raises the FP rig and the view in body mode
std::atomic<double> g_raiseCm{nofoot::kRaiseDefaultCm};   // F53 (TEMPORARY): fix.native_on_foot_body_raise_cm, the constant raise while raise_fp is on
std::atomic<double> g_dropCm{0.0};   // F55 (TEMPORARY): fix.native_on_foot_fp_shoulder_drop_cm, 0..20 (0, the default, drops nothing); body mode only
uint64_t g_f40MarkerLogMs = 0;   // F40: the marker angle line's last time
std::atomic<double> g_worldLeadFrames{2.5};   // F38: fix.native_on_foot_world_markers_lead, 0 to 5 frames (forward_lead); live
std::atomic<int> g_worldMarkersMode{6};        // F27/F31/F37: fix.native_on_foot_world_markers: 0 fixed, 1 forward (C), 2 game, 3 forward_now (H_M), 4 forward_hist (the ring's matched pose), 5 forward_k2 (the ring's pose two frames back), 6 forward_lead (the newest ring pose carried ahead, default); live; key 10 is never touched
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
        const std::string wm = cfg.getString("fix.native_on_foot_world_markers", "forward_lead");
        // F40 (TEMPORARY, live A/B): the runtime answers Elite's "now" request one display period ahead (next, today) or at the instant the
        // frame draws at (render). Carried in the native frame output; the runtime logs each change.
        {
            const std::string np = cfg.getString("fix.now_pose", "next");
            nativeFrameSetNowPose(np == "render_locked" ? 2u : np == "render" ? 1u : 0u);
        }
        g_worldMarkersMode.store(wm == "forward" ? 1 : wm == "fixed" ? 0 : wm == "game" ? 2 : wm == "forward_now" ? 3 : wm == "forward_k2" ? 5 : wm == "forward_hist" ? 4 : 6, std::memory_order_relaxed);
        // F38: the waypoint lead in frames (forward_lead), clamped to 0..5; a value that does not parse reads as 2.5.
        double lead = std::atof(cfg.getString("fix.native_on_foot_world_markers_lead", "2.5").c_str());
        if (!(lead == lead)) lead = 2.5;
        g_worldLeadFrames.store(std::max(0.0, std::min(5.0, lead)), std::memory_order_relaxed);
    }
    // F44 (TEMPORARY instrument, live): the marker probe, off by default. Measure only (native_on_foot.h; draw_record_tag.h).
    {
        // F47 (TEMPORARY, live): the marker composite's per-eye NDC correction, off by default; on turns the probe's machinery on with it
        // (the correction needs its tags, finds and read-backs).
        const bool fix = runtimeVrProfile() && cfg.getString("fix.native_on_foot_marker_fix", "off") == "on";
        const bool probe = runtimeVrProfile() && (cfg.getString("fix.native_on_foot_marker_probe", "off") == "on" || fix);
        if (first || probe != g_nativeOnFootMarkerProbeKey.load(std::memory_order_relaxed))
            Log::get().note("native on foot marker probe (F44/F46/F47, TEMPORARY): %s%s", probe ? "on -- world markers are tagged in t' (low mantissa "
                            "bits), found in the HUD's t33 pool and matched to the marker composite draw (VS B10B032BDFD46700); per eye (the UI "
                            "layer's), its NDC against P through the eye's world rows (b1 270..273); 1 s lines while on foot; measure only" : "off",
                            probe && fix ? " (on because fix.native_on_foot_marker_fix is on)" : "");
        if (first || fix != g_nativeOnFootMarkerFixKey.load(std::memory_order_relaxed))
            Log::get().note("native on foot marker fix (F47/F53, TEMPORARY): %s", fix ? "on -- the marker composite's b0 rows 4 and 5 are moved by "
                            "(dx, dy) . row 7 on a private copy, per eye, when both eyes' marker draws are matched; F53: (dx, dy) = -(m - e), m = X0 "
                            "through the marker's b0 minus P through the eye's main-scene rows, e the fixed icon offset (36.3 px up); only on frames "
                            "whose scene rows agree with the temporal pass under 1 px; 1 s lines while on foot" : "off");
        g_nativeOnFootMarkerFixKey.store(fix, std::memory_order_relaxed);
        g_nativeOnFootMarkerProbeKey.store(probe, std::memory_order_relaxed);
        // F52 (TEMPORARY, live): the reference the correction measures against: temporal (the default) or world_first.
        const int ref = drawtag::parseRefMode(cfg.getString("fix.native_on_foot_marker_ref", "temporal").c_str());
        if (first || ref != g_nativeOnFootMarkerRefKey.load(std::memory_order_relaxed))
            Log::get().note("native on foot marker fix (F52, TEMPORARY): reference %s", ref == drawtag::kRefWorldFirst ? "world_first -- each eye's "
                            "first world pool draw's b1 rows 270..273" : "temporal -- the temporal pass's camera rows for the eye (at the world rows' "
                            "projection and eye) while it has them, else world_first");
        g_nativeOnFootMarkerRefKey.store(ref, std::memory_order_relaxed);
    }
    // F33 (TEMPORARY, live): the body align and its trims (cm, clamped to +-30).
    {
        const std::string alignMode = cfg.getString("fix.native_on_foot_body_align", "off");
        const int mode = alignMode == "body" ? 2 : alignMode == "calibrate" ? 3 : (alignMode == "view" || alignMode == "on") ? 1 : 0;   // the old "on" is view
        g_alignMode.store(mode, std::memory_order_relaxed);
        g_alignKeyOn.store(mode != 0, std::memory_order_relaxed);
        // F48 (live): the body anchor: neck (F53: the default, with the rest geometry: the 3P neck at the FP side plus (0, -0.155, -0.161) m in
        // body axes), pov (F48: the 3P povCamera joint at the FP root) or clavicles (F45; the old value "shoulders" reads as clavicles).
        // Anything else is neck.
        const std::string anchor = cfg.getString("fix.native_on_foot_body_align_anchor", "neck");
        g_alignAnchorMode.store(anchor == "pov" ? 2 : (anchor == "clavicles" || anchor == "shoulders") ? 1 : 0, std::memory_order_relaxed);
        // F50 (TEMPORARY, live): the body's vertical. off (the default): in body mode d moves the body only in its horizontal plane (the feet
        // stay on the ground); on: F48's behaviour, d in all three axes.
        const bool vertical = cfg.getString("fix.native_on_foot_body_align_vertical", "off") == "on";
        if (first || vertical != g_alignVertical.load(std::memory_order_relaxed))
            Log::get().note("native on foot body align (F50, TEMPORARY): fix.native_on_foot_body_align_vertical is %s", vertical ? "on -- d moves "
                            "the body in all three axes (F48)" : "off -- in body mode d moves the body only along its right and forward axes; its "
                            "up component (and the up trim) is dropped and logged");
        g_alignVertical.store(vertical, std::memory_order_relaxed);
        // F51 (TEMPORARY, live): the FP rig and view raise in body mode. F53: off by default; on: the view rises by a constant (a seated y
        // offset) and the FP AMC's records by the same along the body's up axis. The constant is fix.native_on_foot_body_raise_cm (default 10,
        // clamped 0..50): F51's live dh = (3P pov height - FP side height) bobbed 0 to 12 cm with the pov joint's animation, so it is now logged
        // only. Body mode only.
        const bool raiseFp = cfg.getString("fix.native_on_foot_body_raise_fp", "off") == "on";
        double raiseCm = static_cast<double>(cfg.getFloat("fix.native_on_foot_body_raise_cm", static_cast<float>(nofoot::kRaiseDefaultCm)));
        if (!(raiseCm == raiseCm)) raiseCm = nofoot::kRaiseDefaultCm;
        raiseCm = std::max(0.0, std::min(nofoot::RaiseDh::kMaxM * 100.0, raiseCm));
        if (first || raiseFp != g_raiseFpKey.load(std::memory_order_relaxed) || raiseCm != g_raiseCm.load(std::memory_order_relaxed))
            Log::get().note("native on foot body raise (F51/F53, TEMPORARY): fix.native_on_foot_body_raise_fp is %s; fix.native_on_foot_body_raise_cm "
                            "%.1f", raiseFp ? "on -- in body mode the view and the first-person AMC's records rise by the constant raise_cm (F53; "
                            "the live dh is logged, not applied)" : "off -- nothing is raised (the F53 default)", raiseCm);
        g_raiseFpKey.store(raiseFp, std::memory_order_relaxed);
        g_raiseCm.store(raiseCm, std::memory_order_relaxed);
        // F55 (TEMPORARY, live): experiment (c), the FP shoulder drop: the FP clavicles 19 and 20 and their subtree lower by this many cm in the
        // Animate output, body mode only, blended out over 0.2 s while ADS. 0 (the default) drops nothing; clamped to 0..20.
        const double dropCm = nofoot::dropClampCm(static_cast<double>(cfg.getFloat("fix.native_on_foot_fp_shoulder_drop_cm", 0.0f)));
        if (first || dropCm != g_dropCm.load(std::memory_order_relaxed))
            Log::get().note("native on foot shoulder drop (F55, TEMPORARY): fix.native_on_foot_fp_shoulder_drop_cm %.1f -- %s", dropCm,
                            dropCm > 0.0 ? "in body mode the FP clavicles 19 and 20 and the arm chain below them drop by this much (not while ADS)"
                                         : "nothing is dropped (the default)");
        g_dropCm.store(dropCm, std::memory_order_relaxed);
        pfz::g_dropWanted.store(dropCm > 0.0, std::memory_order_relaxed);
        // F49 (TEMPORARY, live): the pose freeze: off, probe (the default: the Animate post-hook measures and writes nothing) or on.
        const std::string freeze = cfg.getString("fix.native_on_foot_pose_freeze", "probe");
        pfz::g_mode.store(freeze == "on" ? 2 : freeze == "off" ? 0 : 1, std::memory_order_relaxed);
        // F56 (TEMPORARY, live): the 3P arm IK: off, probe (the default: the hook measures and writes nothing), nudge (the left hand group up
        // 25 cm: does the skin read the hook's writes?) or on (the two-bone solve onto the FP handAttach joints). Body mode only.
        const std::string ik = cfg.getString("fix.native_on_foot_arm_ik", "probe");
        const int ikMode = ik == "on" ? 3 : ik == "nudge" ? 2 : ik == "off" ? 0 : 1;
        if (first || ikMode != pfz::g_ikMode.load(std::memory_order_relaxed)) {
            static const char* const kIkModeText[4] = {"off -- nothing is hooked or measured for it",
                                                       "probe -- the Animate hook measures the arm chains against the FP handAttach targets every 5 s; "
                                                       "nothing is written (the default)",
                                                       "nudge -- in body mode the 3P left hand group rises 25 cm (model y), blended over 0.2 s: proves "
                                                       "whether the skin reads the hook's writes",
                                                       "on -- in body mode the 3P arms are solved onto the FP handAttach joints (two bones, pole hint, "
                                                       "swivel clamp), blended over 0.2 s, faded per arm between 0.35 and 0.8 m"};
            Log::get().note("native on foot arm IK (F56, TEMPORARY): fix.native_on_foot_arm_ik = %s", kIkModeText[ikMode]);
        }
        pfz::g_ikMode.store(ikMode, std::memory_order_relaxed);
        pfz::g_ikWanted.store(ikMode != 0, std::memory_order_relaxed);
        // F46 part A: the trims are numbers (the menu's sliders): right/left, height and forward/back, in cm, clamped to +-30. F48: default 0.
        const float trims[3] = {cfg.getFloat("fix.native_on_foot_body_align_trim_right_cm", 0.0f),
                                cfg.getFloat("fix.native_on_foot_body_align_trim_up_cm", 0.0f),
                                cfg.getFloat("fix.native_on_foot_body_align_trim_forward_cm", 0.0f)};
        for (size_t i = 0; i < 3; ++i) {
            double v = static_cast<double>(trims[i]);
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
uint64_t g_f38LeadLineMs = 0;   // F38: the forward_lead line's last time
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
// ---- F44 (TEMPORARY instrument, fix.native_on_foot_marker_probe): the tag side, on the game's thread. Each tagged marker's sentinel, t' as
// written (the tag included), P (node+0x50) and tag frame go into the cell of its key under a sequence count: the render thread takes a cell only
// between two equal even counts. g_f44Frame is the render frame (the boundary's frameNo), whose low 16 bits the sentinel carries.
struct F44TagCell {
    std::atomic<uint32_t> seq;
    std::atomic<uint32_t> sentinel, key, frame, pOk;
    std::atomic<uint32_t> tp[3], p[3];   // float bits
};
// F47: a key's first post of a frame and its second post (its key carries bit 7) in separate cells, so the second does not overwrite the first
// before the render thread reads it. F52: one cell per 8-bit tag key (drawtag::tagCellOf): F47's 32 cells shared a cell between keys 16 apart.
constexpr size_t kF44Cells = drawtag::kTagCells;
F44TagCell g_f44Cells[kF44Cells];        // zero-initialised (static storage)
std::atomic<uint32_t> g_f44Frame{0};
std::atomic<uint64_t> g_f44Tagged{0}, g_f44Untaggable{0}, g_f44CellBusy{0};
// F47: the post stub's calls per key and frame (game thread): the last frame (+1) each key (mod 128) was posted in, and the second posts.
constexpr uint32_t kF47SecondPostBit = 0x80u;
std::atomic<uint32_t> g_f47PostFrame[128];   // zero-initialised (static storage)
std::atomic<uint64_t> g_f47SecondPosts{0};
struct F44Tag {
    uint32_t sentinel = 0, key = 0, frame = 0;
    float tp[3] = {}, p[3] = {};
    bool pOk = false;
};
size_t f44CellOf(uint32_t key) noexcept {
    return drawtag::tagCellOf(key);
}
void f44Publish(const F44Tag& t) noexcept {
    F44TagCell& c = g_f44Cells[f44CellOf(t.key)];
    uint32_t s = c.seq.load(std::memory_order_relaxed);
    if ((s & 1u) || !c.seq.compare_exchange_strong(s, s + 1, std::memory_order_acq_rel)) {
        g_f44CellBusy.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::atomic_thread_fence(std::memory_order_release);
    c.sentinel.store(t.sentinel, std::memory_order_relaxed);
    c.key.store(t.key, std::memory_order_relaxed);
    c.frame.store(t.frame, std::memory_order_relaxed);
    c.pOk.store(t.pOk ? 1u : 0u, std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) {
        c.tp[i].store(drawtag::bitsOf(t.tp[i]), std::memory_order_relaxed);
        c.p[i].store(drawtag::bitsOf(t.p[i]), std::memory_order_relaxed);
    }
    c.seq.store(s + 2, std::memory_order_release);
}
bool f44ReadCell(size_t i, F44Tag& out) noexcept {
    const F44TagCell& c = g_f44Cells[i];
    const uint32_t s1 = c.seq.load(std::memory_order_acquire);
    if (s1 == 0 || (s1 & 1u)) return false;
    out.sentinel = c.sentinel.load(std::memory_order_relaxed);
    out.key = c.key.load(std::memory_order_relaxed);
    out.frame = c.frame.load(std::memory_order_relaxed);
    out.pOk = c.pOk.load(std::memory_order_relaxed) != 0;
    for (int k = 0; k < 3; ++k) {
        out.tp[k] = drawtag::floatOf(c.tp[k].load(std::memory_order_relaxed));
        out.p[k] = drawtag::floatOf(c.p[k].load(std::memory_order_relaxed));
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    return c.seq.load(std::memory_order_relaxed) == s1;
}
// F44/F47: a post's tag key and sentinel for this frame (a second post of the same key in one frame carries bit 7).
void f44Assign(uint32_t key, F44Tag& t) noexcept {
    t.frame = g_f44Frame.load(std::memory_order_relaxed) & 0xFFFFu;
    const uint32_t was = g_f47PostFrame[key & 0x7Fu].exchange(t.frame + 1u, std::memory_order_relaxed);
    const bool second = was == t.frame + 1u;
    if (second) g_f47SecondPosts.fetch_add(1, std::memory_order_relaxed);
    t.key = second ? ((key & 0x7Fu) | kF47SecondPostBit) : key;
    t.sentinel = drawtag::sentinel(t.key, t.frame);
}
// F53: the post stub's key census (game thread; cumulative since launch, while the probe is on and native stereo is on foot): each distinct key
// (full 32-bit value) and its posts and tagged posts, so the 1 s line can say whether every key is tagged and whether every key's sentinel is
// distinct (drawtag::keysDistinct). No lock, no allocation: a slot is claimed by a CAS of key + 1; keys past the table are counted.
constexpr uint32_t kF53KeyCensus = 32;
struct F53KeySlot {
    std::atomic<uint32_t> keyPlus1{0};
    std::atomic<uint64_t> posts{0}, tagged{0};
};
F53KeySlot g_f53Keys[kF53KeyCensus];
std::atomic<uint64_t> g_f53KeyOverflow{0};
F53KeySlot* f53KeySlot(uint32_t key) noexcept {
    const uint32_t want = key + 1u;
    if (want == 0) return nullptr;
    for (uint32_t i = 0; i < kF53KeyCensus; ++i) {
        F53KeySlot& s = g_f53Keys[(key + i) % kF53KeyCensus];
        uint32_t cur = s.keyPlus1.load(std::memory_order_relaxed);
        if (cur == 0 && s.keyPlus1.compare_exchange_strong(cur, want, std::memory_order_relaxed)) cur = want;
        if (cur == want) return &s;
    }
    g_f53KeyOverflow.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}
void f53KeyPost(uint32_t key) noexcept {
    if (F53KeySlot* s = f53KeySlot(key)) s->posts.fetch_add(1, std::memory_order_relaxed);
}
void f53KeyTagged(uint32_t key) noexcept {
    if (F53KeySlot* s = f53KeySlot(key)) s->tagged.fetch_add(1, std::memory_order_relaxed);
}
// F52: the marker posts the placement leaves alone (key 10, the reticle; world markers mode "game"; no fresh head pose) are tagged in place
// while the probe is on: the game's own t at +0x40 with the sentinel in its low mantissa bits (at most 255 ulps a component), so every post-stub
// marker node is findable. No lock, no log; the reads and writes are guarded.
std::atomic<uint64_t> g_f52TagOnly{0};
void f52TagInPlace(uintptr_t na, uintptr_t ia, uint32_t key) noexcept {
    if (!g_nativeOnFootMarkerProbeKey.load(std::memory_order_relaxed) || !nativeOnFootActive()) return;
    float t[3] = {};
    if (!guardedRead(ia + 0x40, t, sizeof(t))) return;
    F44Tag f44{};
    f44Assign(key, f44);
    if (!drawtag::encode(t, f44.sentinel)) {
        g_f44Untaggable.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!guardedWriteFloat(ia + 0x40, t[0]) || !guardedWriteFloat(ia + 0x44, t[1]) || !guardedWriteFloat(ia + 0x48, t[2])) return;
    std::memcpy(f44.tp, t, sizeof(t));
    f44.pOk = guardedRead(na + 0x50, f44.p, sizeof(f44.p));
    f44Publish(f44);
    g_f44Tagged.fetch_add(1, std::memory_order_relaxed);
    g_f52TagOnly.fetch_add(1, std::memory_order_relaxed);
    f53KeyTagged(key);
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
    if (g_nativeOnFootMarkerProbeKey.load(std::memory_order_relaxed) && nativeOnFootActive()) f53KeyPost(key);   // F53: the key census
    if (key == 10) {
        f52TagInPlace(na, ia, key);   // F52: tagged (the probe on), placement left alone
        return;
    }
    const int wmMode = g_worldMarkersMode.load(std::memory_order_relaxed);
    if (!nativeOnFootActive()) return;
    if (wmMode == 2) {
        f52TagInPlace(na, ia, key);
        return;
    }
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
    }
    // F38 (waypoint lead, mode 6, the default): the newest ring pose carried L frames on along its own motion; the one before it sets the rate.
    // The view under the lead pose goes into C, as the ring pick does.
    if (havePose && wmMode == 6) {
        EdvrNativeNowPose rp{};
        if (nativeFrameNowPose(rp) && rp.renderValid && rp.ringCount > 0) {
            const uint32_t i0 = rp.ringNewest % 8u;
            const uint32_t i1 = (rp.ringNewest + 7u) % 8u;
            const double L = g_worldLeadFrames.load(std::memory_order_relaxed);
            float lead[12] = {};
            if (rp.ringCount >= 2) nofoot::leadPose(rp.ringHead[i0], rp.ringHead[i1], L, lead);
            else std::memcpy(lead, rp.ringHead[i0], sizeof(lead));
            float H[16] = {};
            f31NowMatrix(C, rp.render, lead, H);
            std::memcpy(C, H, sizeof(C));
            if (now - g_f38LeadLineMs >= 5000 || g_f38LeadLineMs == 0) {
                g_f38LeadLineMs = now;
                const double perFrame = rp.ringCount >= 2 ? nofoot::rotDeltaDeg(rp.ringHead[i0], rp.ringHead[i1]) : 0.0;
                const double leadDeg = nofoot::rotDeltaDeg(rp.ringHead[i0], lead);
                Log::get().note("native on foot world markers: forward_lead: L %.2f frames, rotation %.3f deg per frame, the lead is %.3f deg "
                                "from the newest pose (ring %u)", L, perFrame, leadDeg, static_cast<unsigned>(rp.ringCount));
            }
        }
    }
    // F40: M (the marker camera read at entry, F37) against this call's list-B view (C) and against the runtime's now answer, every 5 s.
    if (sc.valid && sc.mOk && now - g_f40MarkerLogMs >= 5000) {
        EdvrNativeNowPose q{};
        if (nativeFrameNowPose(q) && q.nowValid) {
            float nowM[16] = {};
            for (size_t i = 0; i < 3; ++i)
                for (size_t j = 0; j < 4; ++j) nowM[4 * i + j] = q.now[4 * i + j];
            nowM[15] = 1.0f;
            g_f40MarkerLogMs = now;
            Log::get().note("native on foot world markers: M against C (this call's list-B view) %.3f deg, M against the now answer %.3f deg "
                            "(mode %d)", f31AngleDeg(sc.mMat, sc.head.C), f31AngleDeg(sc.mMat, nowM), wmMode);
        }
    }
    if (!havePose) {
        if (now - g_worldLineMs >= 5000 || g_worldLineMs == 0) {
            g_worldLineMs = now;
            Log::get().note("native on foot world markers: fixed, but no fresh head pose (no list-B sample in 0.5 s); markers are left as the game places them");
        }
        f52TagInPlace(na, ia, key);
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
    // F44 (TEMPORARY instrument, fix.native_on_foot_marker_probe): t' is written with the sentinel in its low mantissa bits (at most 255 ulps a
    // component, under 3e-5 relative; draw_record_tag.h). A component the tag cannot take leaves t' as computed (counted).
    F44Tag f44{};
    float tw[3] = {tp[0], tp[1], tp[2]};
    bool f44Tagged = false;
    if (g_nativeOnFootMarkerProbeKey.load(std::memory_order_relaxed)) {
        // F47: a second (or later) post of the same key in one frame gets its own low-bit magic (the key with bit 7 set), so a second marker
        // record, if the game makes one, is told apart from the first in the pool and in the log.
        f44Assign(key, f44);
        f44Tagged = drawtag::encode(tw, f44.sentinel);
        if (!f44Tagged) g_f44Untaggable.fetch_add(1, std::memory_order_relaxed);
    }
    if (!guardedWriteFloat(ia + 0x40, tw[0]) || !guardedWriteFloat(ia + 0x44, tw[1]) || !guardedWriteFloat(ia + 0x48, tw[2])) {
        if (now - g_worldLineMs >= 5000 || g_worldLineMs == 0) {
            g_worldLineMs = now;
            Log::get().note("native on foot world markers: key %u: the instance could not be written; left as the game placed it", static_cast<unsigned>(key));
        }
        return;
    }
    if (f44Tagged) {
        std::memcpy(f44.tp, tw, sizeof(tw));
        f44.pOk = guardedRead(na + 0x50, f44.p, sizeof(f44.p));
        f44Publish(f44);
        g_f44Tagged.fetch_add(1, std::memory_order_relaxed);
        f53KeyTagged(key);
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
    // F39: the neck joint (found by name on the local third-person skeleton, census) in world space, and the name it was found under.
    bool neckOk = false;
    float neck[3] = {};
    const char* neckName = nullptr;
    // F43: the 3P shoulders in world (L then R), the FP shoulders in FP model space, and the neck plus the rest pov offset in world.
    bool shOk3P = false, shOkFp = false, extOk3P = false, extOkFp = false;
    float sh3PW[6] = {}, shFpM[6] = {}, ext3PW[12] = {}, extFpM[12] = {};
    // F48: the 3P def_c_povCamera_joint in world (the census's model-space read through this AMC matrix, as the neck), and its index.
    bool povJOk = false;
    float povJ[3] = {};
    uint32_t povJIdx = 0xFFFFu;
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
    // F39: the neck joint from the census, through the same matrix as the head.
    s.neckOk = s.headOk && nk.neckState == 1;
    if (s.neckOk) {
        s.neckName = nk.neckName;
        for (size_t j = 0; j < 3; ++j) s.neck[j] = nk.neck[0] * M[j] + nk.neck[1] * M[4 + j] + nk.neck[2] * M[8 + j] + M[12 + j];
    }
    // F43: the 3P shoulders through this AMC matrix (the head's transform), the FP shoulders as the census read them (model space).
    s.shOk3P = s.headOk && (nk.shState & 1u) != 0;
    if (s.shOk3P)
        for (size_t k = 0; k < 2; ++k)
            for (size_t j = 0; j < 3; ++j)
                s.sh3PW[3 * k + j] = nk.sh3P[3 * k + 0] * M[j] + nk.sh3P[3 * k + 1] * M[4 + j] + nk.sh3P[3 * k + 2] * M[8 + j] + M[12 + j];
    s.shOkFp = (nk.shState & 2u) != 0;
    if (s.shOkFp) std::memcpy(s.shFpM, nk.shFp, sizeof(s.shFpM));
    // F45: the hands and the attach points: 3P through this AMC matrix (as the head), FP in FP model space.
    s.extOk3P = s.headOk && (nk.shState & 4u) != 0;
    if (s.extOk3P)
        for (size_t k = 0; k < 4; ++k)
            for (size_t j = 0; j < 3; ++j)
                s.ext3PW[3 * k + j] = nk.ext3P[3 * k + 0] * M[j] + nk.ext3P[3 * k + 1] * M[4 + j] + nk.ext3P[3 * k + 2] * M[8 + j] + M[12 + j];
    s.extOkFp = (nk.shState & 8u) != 0;
    if (s.extOkFp) std::memcpy(s.extFpM, nk.extFp, sizeof(s.extFpM));
    // F48: the 3P povCamera joint through this AMC matrix (as the neck).
    s.povJOk = s.headOk && (nk.shState & 0x10u) != 0;
    if (s.povJOk) {
        s.povJIdx = nk.povIndex;
        for (size_t j = 0; j < 3; ++j) s.povJ[j] = nk.pov3P[0] * M[j] + nk.pov3P[1] * M[4 + j] + nk.pov3P[2] * M[8 + j] + M[12 + j];
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
    float rows[9] = {};   // F43: the root matrix's rotation rows (rows[3r + c])
    uint64_t stamp = 0;
};
FpRootSnap g_fpRoot;   // under g_eyeMutex
std::atomic<uint64_t> g_fpAmcLocal{0};   // F51: the mode-2 (FP) AMC fpRootTrack last saw (the body-shift post-hook's FP match; it re-reads the mode)
// F48 (census, read only): every populated part entry of a mode-2 (FP) AMC, once per AMC: each entry whose four instance pointers, four view
// masks or flag byte are not all zero, with all four of each, the flag byte, state, pending and current. The F1/F3 census lines keep skipping
// parts without an instance; this dump does not. Game thread (the P1 callback); every read guarded.
uintptr_t g_f48PartsAmc = 0;   // game thread only
void fpPartTableDump(uintptr_t a) noexcept {
    if (a == g_f48PartsAmc) return;
    g_f48PartsAmc = a;
    uint32_t populated = 0, unread = 0;
    for (uint32_t idx = 0; idx < ecm::kPartCount; ++idx) {
        const uintptr_t at = a + static_cast<uintptr_t>(idx) * ecm::kPartStride;
        uint64_t inst[ecm::kPartVariants] = {}, mask[ecm::kPartVariants] = {};
        uint8_t flags = 0;
        int32_t sp[3] = {};   // state, pending, current
        if (!guardedRead(at + ecm::kOffAmcInstances, inst, sizeof(inst)) || !guardedRead(at + ecm::kOffAmcMasks, mask, sizeof(mask)) ||
            !guardedRead(at + ecm::kOffAmcPartFlags, &flags, 1) || !guardedRead(at + ecm::kOffAmcPartState, &sp[0], 4) ||
            !guardedRead(at + ecm::kOffAmcPartPending, &sp[1], 4) || !guardedRead(at + ecm::kOffAmcPartCurrent, &sp[2], 4)) {
            ++unread;
            continue;
        }
        bool any = flags != 0;
        for (uint32_t k = 0; k < ecm::kPartVariants; ++k) any = any || inst[k] != 0 || mask[k] != 0;
        if (!any) continue;
        ++populated;
        Log::get().note("native on foot FP part table (AMC 0x%llX): part %u %s: inst 0x%llX 0x%llX 0x%llX 0x%llX; mask 0x%llX 0x%llX 0x%llX 0x%llX; "
                        "flags 0x%02X state %d pending %d current %d",
                        static_cast<unsigned long long>(a), static_cast<unsigned>(idx), ecm::kPartNames[idx],
                        static_cast<unsigned long long>(inst[0]), static_cast<unsigned long long>(inst[1]), static_cast<unsigned long long>(inst[2]),
                        static_cast<unsigned long long>(inst[3]), static_cast<unsigned long long>(mask[0]), static_cast<unsigned long long>(mask[1]),
                        static_cast<unsigned long long>(mask[2]), static_cast<unsigned long long>(mask[3]), static_cast<unsigned>(flags), sp[0], sp[1], sp[2]);
    }
    Log::get().note("native on foot FP part table (AMC 0x%llX): %u of %u entries populated (an instance, a mask or the flag byte non-zero), %u not readable",
                    static_cast<unsigned long long>(a), populated, static_cast<unsigned>(ecm::kPartCount), unread);
}
void fpRootTrack(void* amcPtr, uint64_t now) noexcept {
    const uintptr_t a = reinterpret_cast<uintptr_t>(amcPtr);
    uintptr_t p = 0;
    uint32_t mode = 0;
    if (!guardedRead(a + 0x50, &p, 8) || !p || !guardedRead(p + 0x14, &mode, 4) || mode != 2) return;
    g_fpAmcLocal.store(static_cast<uint64_t>(a), std::memory_order_relaxed);   // F51
    fpPartTableDump(a);   // F48 (census): once per FP AMC
    uintptr_t obj = 0, vt = 0, fn = 0, mat = 0;
    float m[16] = {};
    if (!(guardedRead(a + 0x208, &obj, 8) && obj && guardedRead(obj, &vt, 8) && vt && guardedRead(vt + 0x20, &fn, 8) && fn &&
          callRootGetter(fn, obj, &mat) && mat && guardedRead(mat, m, sizeof(m))))
        return;
    std::lock_guard<std::mutex> lock(g_eyeMutex);
    g_fpRoot.ok = true;
    std::memcpy(g_fpRoot.origin, m + 12, sizeof(g_fpRoot.origin));
    for (size_t r = 0; r < 3; ++r)
        for (size_t c = 0; c < 3; ++c) g_fpRoot.rows[3 * r + c] = m[4 * r + c];
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

// ---- F56 (TEMPORARY): the 3P arm IK's hand-offs ----------------------------------------------------------------------------------
// The FP side: the FP X's model matrices of joints 51 (R handAttach), 52 (L handAttach), 37 and 44 (the hands), copied by the FP Animate post-hook
// (fdAfterAnimate) BEFORE any shoulder-drop write, under a sequence counter (one writer: g_fdBusy). G: the FP-model to 3P-model transform
// G = F_root x Tr(r - d) x inv(M3P), published by the game thread's P1 callback for the local 3P AMC (every call while the IK key is not off),
// with its inputs' sanity. The 3P Animate post-hook reads both (two tries, never waits) and refuses either older than 100 ms.
struct IkFpSnap {
    std::atomic<uint32_t> seq{0};
    float m[4][16] = {};   // [0] 51 R attach, [1] 52 L attach, [2] 37 hand, [3] 44 hand
    uint64_t ms = 0;
};
IkFpSnap g_ikFp;
struct IkGData {
    float g[16] = {};
    float m3p[16] = {}, froot[16] = {};
    float r[3] = {}, d[3] = {};
    float m3pRowErr = 0.0f, m3pDet = 0.0f, fRowErr = 0.0f, fDet = 0.0f;   // max |row length - 1| and the 3x3 determinant
    uint64_t ms = 0, frootMs = 0;
};
std::atomic<uint32_t> g_ikGSeq{0};
IkGData g_ikG;
std::atomic<uint64_t> g_ikGPublishes{0}, g_ikGRefusals{0};
bool ikFpRead(float m[4][16], uint64_t* ms) noexcept {
    for (int tries = 0; tries < 2; ++tries) {
        const uint32_t s1 = g_ikFp.seq.load(std::memory_order_acquire);
        if (s1 & 1u || s1 == 0) continue;
        std::memcpy(m, g_ikFp.m, sizeof(g_ikFp.m));
        *ms = g_ikFp.ms;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_ikFp.seq.load(std::memory_order_relaxed) == s1) return true;
    }
    return false;
}
bool ikGRead(IkGData* out) noexcept {
    for (int tries = 0; tries < 2; ++tries) {
        const uint32_t s1 = g_ikGSeq.load(std::memory_order_acquire);
        if (s1 & 1u || s1 == 0) continue;
        std::memcpy(out, &g_ikG, sizeof(*out));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_ikGSeq.load(std::memory_order_relaxed) == s1) return true;
    }
    return false;
}
void ikPublishG(void* amcPtr, uint64_t now) noexcept;   // defined with the FP raise's hand-off below (it reads d and r)

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
    // F56: G for the arm IK, every call for the local 3P AMC while the key is not off (the FP root was tracked above, for the FP AMC's call).
    if (!g_eyeStop.load(std::memory_order_relaxed) && amc && nativeOnFootActive() && pfz::g_ikWanted.load(std::memory_order_relaxed) &&
        reinterpret_cast<uint64_t>(amc) == localAmcPointer())
        ikPublishG(amc, now);
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
// F41: the view loop is closed (the camera follows the applied offset 250 ms late), so its integral runs at 0.02 a frame into a 100 ms filter;
// the body is open loop, so it has no integral: the camera and the pov low-passed (0.1 s, 0.4 s), one critically damped filter (0.1 s).
constexpr double kViewGain = 0.02, kViewTauSec = 0.1, kBodyTauSec = 0.1, kBodyCamTauSec = 0.1, kBodyPovTauSec = 0.4;
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
    const bool dz = d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f;
    // F45: the table prints at the first body pass after entry: shifted parts when d is not zero, else the present parts (nothing is written).
    if (dz && !g_f37TableWanted.load(std::memory_order_relaxed)) return;
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
            if (dz) continue;
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
        uint32_t partsPresent = 0, partsShifted = 0;
        for (int idx = 0; idx < bodyrec::kBlocks; ++idx)
            for (int k = 0; k < bodyrec::kSlots; ++k) {
                if (present[idx][k]) ++partsPresent;
                if (movedAt[idx][k]) ++partsShifted;
                if (!present[idx][k]) continue;
                Log::get().note("native on foot body shift part: block %d slot %d name %s mask %016llX %s", idx, k,
                                idx < ecm::kPartCount ? ecm::kPartNames[idx] : "?", static_cast<unsigned long long>(maskAt[idx][k]),
                                movedAt[idx][k] ? "shifted" : (dz ? "present (d zero: not shifted)" : "present, not shifted"));
            }
        Log::get().note("native on foot body shift: %u parts present, %u shifted this pass (%llu records shifted, %llu skipped; d %s)", partsPresent,
                        partsShifted, static_cast<unsigned long long>(shifted), static_cast<unsigned long long>(skipped), dz ? "zero" : "not zero");
    }
}
// ---- F51 (TEMPORARY): the FP rig raise, the hook's half ------------------------------------------------------------------------------
// The frame thread publishes the FP offset (world metres: dh along the body's up row) through a seqlock, and g_f51FpOn (body mode and the key on).
// The post-hook moves the FP (mode 2) AMC's records by it under the 3P rule (a record moves only while it equals the AMC origin, so a second pass
// leaves it alone). Inside the hook: no allocation, logging or locks; every read and write under SEH; counters go to the frame thread.
std::atomic<bool> g_f51FpOn{false};
std::atomic<uint32_t> g_fpRaiseSeq{0};
std::atomic<float> g_fpRaiseD[3]{};
void fpRaisePublish(const double d[3]) noexcept {
    g_fpRaiseSeq.fetch_add(1, std::memory_order_acq_rel);
    for (size_t i = 0; i < 3; ++i) g_fpRaiseD[i].store(static_cast<float>(d[i]), std::memory_order_relaxed);
    g_fpRaiseSeq.fetch_add(1, std::memory_order_release);
}
bool fpRaiseRead(float d[3]) noexcept {
    for (int tries = 0; tries < 4; ++tries) {
        const uint32_t s1 = g_fpRaiseSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        for (size_t i = 0; i < 3; ++i) d[i] = g_fpRaiseD[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_fpRaiseSeq.load(std::memory_order_relaxed) == s1) return true;
    }
    return false;   // being written: this pass skips (the next one moves the records)
}
// F56: G = F_root x Tr(r - d) x inv(M3P), the FP model space to the 3P model space (row vectors: FP world = model_fp x F_root, raised by r; 3P
// world = model_3p x M3P, shifted by d). Game thread (the P1 callback, the local 3P AMC). Refused (not published) when the AMC matrix cannot be
// read, the FP root is older than 100 ms, or M3P is singular. The FP root is the whole matrix (rows and origin); probe 5 compares it with the
// yaw-only map the F45/F55 code uses.
void ikPublishG(void* amcPtr, uint64_t now) noexcept {
    IkGData s;
    FpRootSnap fp{};
    {
        std::lock_guard<std::mutex> lock(g_eyeMutex);
        fp = g_fpRoot;
    }
    float inv[16] = {};
    if (!sehAmcMatrix(static_cast<const uint8_t*>(amcPtr), s.m3p) || !fp.ok || (now > fp.stamp ? now - fp.stamp : 0) > nofoot::kIkFreshMs ||
        !nofoot::pfzAffineInverse(s.m3p, inv)) {
        g_ikGRefusals.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) s.froot[r * 4 + c] = fp.rows[r * 3 + c];
        s.froot[r * 4 + 3] = 0.0f;
    }
    if (g_f51FpOn.load(std::memory_order_relaxed)) {
        if (!fpRaiseRead(s.r)) s.r[0] = s.r[1] = s.r[2] = 0.0f;
    }
    bodyOffsetRead(s.d);
    for (int c = 0; c < 3; ++c) s.froot[12 + c] = fp.origin[c];
    s.froot[15] = 1.0f;
    float shifted[16];
    std::memcpy(shifted, s.froot, sizeof(shifted));
    for (int c = 0; c < 3; ++c) shifted[12 + c] += s.r[c] - s.d[c];
    nofoot::pfzMul(shifted, inv, s.g);
    const auto rowErr = [](const float* m) noexcept {
        float e = 0.0f;
        for (int r = 0; r < 3; ++r) {
            const float l = std::sqrt(m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2]);
            e = std::max(e, std::fabs(l - 1.0f));
        }
        return e;
    };
    const auto det3 = [](const float* m) noexcept {
        return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
    };
    s.m3pRowErr = rowErr(s.m3p);
    s.m3pDet = det3(s.m3p);
    s.fRowErr = rowErr(s.froot);
    s.fDet = det3(s.froot);
    s.ms = now;
    s.frootMs = fp.stamp;
    static std::atomic<bool> busy{false};   // one writer at a time (a second caller skips; never waits)
    if (busy.exchange(true, std::memory_order_acquire)) return;
    g_ikGSeq.fetch_add(1, std::memory_order_acq_rel);
    std::memcpy(&g_ikG, &s, sizeof(s));
    g_ikGSeq.fetch_add(1, std::memory_order_release);
    busy.store(false, std::memory_order_release);
    g_ikGPublishes.fetch_add(1, std::memory_order_relaxed);
}
std::atomic<uint64_t> g_fpShifted{0}, g_fpSkipped{0}, g_fpPasses{0}, g_fpPartsMask{0}, g_fpNotMode2{0};
struct F51First {
    std::atomic<uint32_t> seq{0};
    float pre[3] = {}, post[3] = {};
    uint32_t block = 0;
};
F51First g_f51First;   // the first FP record shifted in the latest pass (the 1 s line)
void fpRaiseApply(uint64_t amc) noexcept {
    uint64_t params = 0;
    uint32_t mode = 0;
    if (!guardedRead(amc + 0x50, &params, 8) || !params || !guardedRead(params + 0x14, &mode, 4) || mode != 2) {
        g_fpNotMode2.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint8_t flags = 0;
    if (!guardedRead(amc + bodyrec::kFlags, &flags, 1) || !(flags & 4)) return;
    float d[3] = {};
    if (!fpRaiseRead(d) || (d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f)) return;
    float origin[3] = {};
    if (!guardedRead(amc + bodyrec::kOrigin, origin, sizeof(origin))) return;
    g_fpPasses.fetch_add(1, std::memory_order_relaxed);
    uint64_t shifted = 0, skipped = 0, parts = 0;
    bool first = true;
    for (int idx = 0; idx < bodyrec::kBlocks; ++idx) {
        for (int k = 0; k < bodyrec::kSlots; ++k) {
            uint64_t inst = 0, mask = 0, instField = 0;
            const uintptr_t slot = static_cast<uintptr_t>(idx) * bodyrec::kStride + static_cast<uintptr_t>(k) * 8u;
            if (!guardedRead(amc + bodyrec::kInstBase + slot, &inst, 8) || !inst) continue;
            if (!guardedRead(amc + bodyrec::kMaskBase + slot, &mask, 8) || !mask) continue;
            if (!guardedRead(static_cast<uintptr_t>(inst) + bodyrec::kInstOff, &instField, 8) || !instField) continue;
            const uintptr_t rec = amc + bodyrec::kRecBase + static_cast<uintptr_t>(idx) * bodyrec::kStride +
                                  static_cast<uintptr_t>(k) * bodyrec::kRecSize;
            float pos[3] = {}, pre[3] = {};
            if (!guardedRead(rec + bodyrec::kRecPos, pos, sizeof(pos))) {
                ++skipped;
                continue;
            }
            std::memcpy(pre, pos, sizeof(pre));
            if (!nofoot::shiftIfAtOrigin(pos, origin, d)) {   // already placed (or moved by an earlier pass): not ours to move
                ++skipped;
                continue;
            }
            float prev[3] = {};
            const bool okPrev = guardedRead(rec + bodyrec::kPrev, prev, sizeof(prev));
            for (int i = 0; i < 3; ++i) {
                guardedWriteFloat(rec + bodyrec::kRecPos + 4u * static_cast<uintptr_t>(i), pos[i]);
                if (okPrev) guardedWriteFloat(rec + bodyrec::kPrev + 4u * static_cast<uintptr_t>(i), prev[i] + d[i]);
            }
            parts |= 1ull << idx;
            if (first) {
                first = false;
                g_f51First.seq.fetch_add(1, std::memory_order_acq_rel);
                std::memcpy(g_f51First.pre, pre, sizeof(pre));
                std::memcpy(g_f51First.post, pos, sizeof(pos));
                g_f51First.block = static_cast<uint32_t>(idx);
                g_f51First.seq.fetch_add(1, std::memory_order_release);
            }
            ++shifted;
        }
    }
    g_fpShifted.fetch_add(shifted, std::memory_order_relaxed);
    g_fpSkipped.fetch_add(skipped, std::memory_order_relaxed);
    if (parts) g_fpPartsMask.fetch_or(parts, std::memory_order_relaxed);
}
static_assert(bodyrec::kBlocks <= 64, "the FP parts mask holds one bit per block");

uint64_t __fastcall bodyShiftHooked(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    const F35Fn original = reinterpret_cast<F35Fn>(g_bodyShift.original);
    if (!original) return 0;
    const uint64_t r = original(a1, a2, a3, a4, s5, s6, s7, s8);
    g_bodyCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_alignMode.load(std::memory_order_relaxed) == 2 && a1) {
        if (a1 == localAmcPointer()) bodyShiftApply(a1);
        else if (g_f51FpOn.load(std::memory_order_relaxed) && a1 == g_fpAmcLocal.load(std::memory_order_relaxed)) fpRaiseApply(a1);   // F51
    }
    return r;
}

// One filter step for a run: the integral of the error, the guard, the critically damped follow, the clamp, the per-frame cap. `driving` false
// holds the integral and lets the filter settle onto it (a decay when `decay` is set, toward zero).
void alignFilter(AlignRun& a, const double err[3], bool driving, bool decay, double dt, uint64_t now, bool guardOn) noexcept {
    const double errMag = std::sqrt(err[0] * err[0] + err[1] * err[1] + err[2] * err[2]);
    if (driving) {
        double step[3] = {align::kViewGain * err[0], align::kViewGain * err[1], align::kViewGain * err[2]};
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
        for (size_t i = 0; i < 3; ++i) a.intg[i] -= a.intg[i] * std::min(1.0, dt / align::kViewTauSec);
    }
    const double target[3] = {a.intg[0], a.intg[1], a.intg[2]};
    for (size_t i = 0; i < 3; ++i) {
        nofoot::cdStep(a.pos[i], a.vel[i], target[i], align::kViewTauSec, dt, align::kCapPerFrameM);
    }
    const double pm = std::sqrt(a.pos[0] * a.pos[0] + a.pos[1] * a.pos[1] + a.pos[2] * a.pos[2]);
    if (pm > align::kClampM)
        for (size_t i = 0; i < 3; ++i) a.pos[i] *= align::kClampM / pm;
}

// F41: the body's one filter: the target (world metres) through one critically damped filter, tau 0.1 s, at most 5 cm a frame, clamped to 0.6 m.
// No integral: the body is open loop.
void bodyFilter(AlignRun& a, const double target[3], double dt) noexcept {
    for (size_t i = 0; i < 3; ++i) nofoot::cdStep(a.pos[i], a.vel[i], target[i], align::kBodyTauSec, dt, align::kCapPerFrameM);
    const double pm = std::sqrt(a.pos[0] * a.pos[0] + a.pos[1] * a.pos[1] + a.pos[2] * a.pos[2]);
    if (pm > align::kClampM)
        for (size_t i = 0; i < 3; ++i) a.pos[i] *= align::kClampM / pm;
}
// The frame thread's one call per frame. The view offset is seated metres (the runtime moves the seated origin by it); the body offset is world
// metres (the submit job's hook moves the records by it). The other run's offset decays to zero.
// F37: the view's head motion reference (tS0), the sign check's previous step, and the last values the 1 s lines print.
// F38: the pov target's state (frame thread). Reset on entering a mode; the first second of an entry sets the neck offset and the
// camera-above-FP mean; the head is low-passed in body axes (tau 1 s) when the neck is not known.
// F41: the body's low-passes (body axes) and the logged target, frame thread only; reset on entering body.
bool g_f41LpValid = false;
double g_f41CamLp[3] = {}, g_f41PovLp[3] = {}, g_f41TgtB[3] = {};
uint64_t g_f41BodyLogMs = 0;
// F48: the body's standing height (frame thread; reset on entering body), the pov anchor's source (-1 not yet, 0 the povCamera joint, 1 the
// head plus (0, 0.038, 0.112)), and this frame's 3P side, FP side and live height for the 1 s line.
nofoot::StandingHeight g_f48Stand;
int g_f48PovSrc = -1;
bool g_f48Have = false;
double g_f48SrcB[3] = {}, g_f48CamB[3] = {}, g_f48LiveUp = 0.0;
bool g_f38Valid = false, g_f38LpValid = false;
uint64_t g_f38StartMs = 0, g_f38LineMs = 0;
double g_f38R0[3] = {}, g_f38CamFp[3] = {}, g_f38Lp[3] = {};
int g_f38R0N = 0, g_f38CamFpN = 0;
double g_f38CamNeck[3] = {};   // F39: mean of (camera minus neck) in body axes over the first second (logged)
int g_f38CamNeckN = 0;
bool g_f37TS0Valid = false;
double g_f37TS0[3] = {};
uint32_t g_f37TS0Gen = 0;
double g_f37TSNow[3] = {};
struct F37Prev {
    bool valid = false;
    double camB[3] = {}, applied[3] = {};
};
F37Prev g_f37Prev;
double g_f37SignToward = 0.0, g_f37SignApplied = 0.0;   // metres: the cam's motion since the last step, toward the pov and along the applied step

// F40: the calibration schedule (one flight, mode calibrate): six 1 s steps, +5 cm seated X, rest, +5 cm Y, rest, +5 cm Z, rest, repeating. The
// offsets are scheduled, not driven by the error: the loop and the guard are off. At each step's start the step that just ended is measured:
// the camera's body position and tS, the mean over its last 0.5 s less the mean over the 0.5 s before that step began.
struct CalSample {
    uint64_t t = 0;
    double cam[3] = {}, ts[3] = {};
};
constexpr uint32_t kCalRing = 256;
CalSample g_calRing[kCalRing];
uint32_t g_calHead = 0, g_calCount = 0;
uint64_t g_calT0 = 0;
int g_calStep = -1;
uint64_t g_f40ViewLogMs = 0;
double g_f40Psi = 0.0;
const double kCalOffsets[6][3] = {{0.05, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.05, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.05}, {0.0, 0.0, 0.0}};
const char* const kCalNames[6] = {"+X 5 cm", "rest", "+Y 5 cm", "rest", "+Z 5 cm", "rest"};

void calibrateReset() noexcept {
    g_calT0 = 0;
    g_calStep = -1;
    g_calHead = 0;
    g_calCount = 0;
}

// The mean over the samples taken in [from, to), and how many there were.
uint32_t calWindow(uint64_t from, uint64_t to, double cam[3], double ts[3]) noexcept {
    uint32_t n = 0;
    for (size_t i = 0; i < 3; ++i) cam[i] = ts[i] = 0.0;
    for (uint32_t k = 0; k < g_calCount; ++k) {
        const CalSample& c = g_calRing[(g_calHead + kCalRing - 1 - k) % kCalRing];
        if (c.t < from || c.t >= to) continue;
        ++n;
        for (size_t i = 0; i < 3; ++i) {
            cam[i] += c.cam[i];
            ts[i] += c.ts[i];
        }
    }
    if (n > 0)
        for (size_t i = 0; i < 3; ++i) {
            cam[i] /= n;
            ts[i] /= n;
        }
    return n;
}

void calibrateStep(uint64_t now, const double cam[3], const double ts[3], bool haveSample, AlignRun& a) noexcept {
    if (g_calT0 == 0) g_calT0 = now;
    const int step = static_cast<int>(((now - g_calT0) / 1000u) % 6u);
    if (haveSample) {
        CalSample& c = g_calRing[g_calHead];
        c.t = now;
        for (size_t i = 0; i < 3; ++i) {
            c.cam[i] = cam[i];
            c.ts[i] = ts[i];
        }
        g_calHead = (g_calHead + 1) % kCalRing;
        if (g_calCount < kCalRing) ++g_calCount;
    }
    if (step != g_calStep) {
        if (g_calStep >= 0) {
            // The step that just ended began 1000 ms ago: its last 0.5 s, and the 0.5 s before it began.
            double aCam[3] = {}, aTs[3] = {}, bCam[3] = {}, bTs[3] = {};
            const uint32_t na = calWindow(now - 500, now, aCam, aTs);
            const uint32_t nb = calWindow(now - 1500, now - 1000, bCam, bTs);
            if (na < 5 || nb < 5) {
                Log::get().note("native on foot calibrate: step %s: too few samples (%u and %u); not measured", kCalNames[g_calStep], na, nb);
            } else {
                Log::get().note("native on foot calibrate: step %s (seated offset %.3f %.3f %.3f m, psi %.1f deg): camera body delta %.4f %.4f %.4f m, "
                                "tS delta %.4f %.4f %.4f m (%u and %u samples)",
                                kCalNames[g_calStep], kCalOffsets[g_calStep][0], kCalOffsets[g_calStep][1], kCalOffsets[g_calStep][2],
                                g_f40Psi * 180.0 / 3.14159265358979323846, aCam[0] - bCam[0], aCam[1] - bCam[1], aCam[2] - bCam[2],
                                aTs[0] - bTs[0], aTs[1] - bTs[1], aTs[2] - bTs[2], na, nb);
            }
        }
        g_calStep = step;
    }
    for (size_t i = 0; i < 3; ++i) a.pos[i] = haveSample ? kCalOffsets[step][i] : 0.0;
}

// F51: the raise's frame-thread state: dh's filter, the applied raise (metres; the view's seated y is minus it, the FP records move by it along
// the body's up row), the last published eye align (the view run plus the raise) and FP offset, the body's last up row, and the 1 s line's time.
nofoot::RaiseDh g_f51Dh;
double g_f51Applied = 0.0;
double g_f51EyePub[3] = {}, g_f51FpPub[3] = {};
double g_f51Up[3] = {0.0, 1.0, 0.0};
double g_f51PovUp = 0.0, g_f51FpUp = 0.0;
int g_f51Src = -1;   // 0 the 3P povCamera joint, 1 the head plus 0.038 m
uint64_t g_f51LogMs = 0;

// F55 (TEMPORARY): the walking shift's frame-thread state. The locomotion gate (the F49 gate's rule, fed from the body sample), the rest point's
// kind this frame and the kind the capture holds (1 the neck, 2 the low-passed head, 3 the 3P clavicles, 4 the povCamera joint, 5 the head plus
// the pov offset), this frame's live source, its offset from the rest point, and whether the captured rest was used (all body axes).
nofoot::PfzLocoRest g_f55Loco;
uint64_t g_f55LocoStamp = 0;
int g_f55KindNow = 0, g_f55KindPrev = 0, g_f55RestKind = 0;
double g_f55Live[3] = {}, g_f55Raw[3] = {}, g_f55Off[3] = {};
bool g_f55Used = false;
const char* const kF55KindNames[6] = {"none", "the neck", "the low-passed head (no neck)", "the 3P clavicles", "the povCamera joint",
                                      "the head + (0, 0.038, 0.112)"};
// F55: the FP shoulder drop: the blend (0.2 s), the applied drop (metres, FP model -y), published to the Animate hook.
nofoot::DropBlend g_f55DropBlend;
double g_f55DropApplied = 0.0;
std::atomic<float> g_f55DropM{0.0f};
// F55: the hook's snapshot of the FP X's joints 19, 20, 37 and 44 (model space, before and after the drop) under a sequence counter (one
// writer: the hook under g_fdBusy), and its counters and failure bits (the frame thread names each failure once).
struct F55Snap {
    std::atomic<uint32_t> seq{0};
    float before[4][3] = {};
    float after[4][3] = {};
    float drop = 0.0f;
    uint64_t ms = 0;
};
F55Snap g_f55Snap;
std::atomic<uint64_t> g_fdCalls{0}, g_fdWrites{0}, g_fdBusySkips{0};
std::atomic<uint32_t> g_fdFailBits{0};
std::atomic<bool> g_fdWriteDown{false};
uint64_t g_f55CallsLast = 0, g_f55WritesLast = 0;
uint32_t g_f55FailNamed = 0;
constexpr uint32_t kFdFailKinds = 7;
const char* const kFdFailText[kFdFailKinds] = {
    "the plan was being rewritten (skipped this call)", "the plan does not belong to this X or is empty",
    "a flag of X (+0x210) or P = [X+8] or its joint count was not readable", "the joint count differs from the plan's (the skeleton changed)",
    "no output span: [job+0x28] or [X+0x1F0] is null, or count-1 is not below [Y+0xE0]", "a joint position in the output span was not readable",
    "a drop write faulted: the drop stands down for the session"};
float pfzWeightNow() noexcept;   // the pose freeze's blend weight (defined with the Animate hook below)
// F55: the hook's snapshot, read under its sequence counter (false: torn or never written).
bool f55SnapRead(float before[4][3], float after[4][3], float* drop, uint64_t* ms) noexcept {
    for (int tries = 0; tries < 4; ++tries) {
        const uint32_t s1 = g_f55Snap.seq.load(std::memory_order_acquire);
        if (s1 & 1u || s1 == 0) continue;
        std::memcpy(before, g_f55Snap.before, sizeof(g_f55Snap.before));
        std::memcpy(after, g_f55Snap.after, sizeof(g_f55Snap.after));
        *drop = g_f55Snap.drop;
        *ms = g_f55Snap.ms;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_f55Snap.seq.load(std::memory_order_relaxed) == s1) return true;
    }
    return false;
}

// F51: the once-a-second line in body mode: dh, the view offset, the FP records shifted (count and part names), the weapon's status.
void f51Line(const EyeSample& s, const FpRootSnap& fp, bool fpFresh, bool raiseOn) noexcept {
    const uint64_t shifted = g_fpShifted.exchange(0, std::memory_order_relaxed), skipped = g_fpSkipped.exchange(0, std::memory_order_relaxed),
                   passes = g_fpPasses.exchange(0, std::memory_order_relaxed), parts = g_fpPartsMask.exchange(0, std::memory_order_relaxed),
                   notMode2 = g_fpNotMode2.exchange(0, std::memory_order_relaxed);
    char names[700] = {};
    size_t at = 0;
    for (uint32_t idx = 0; idx < ecm::kPartCount && idx < 64; ++idx) {
        if (!(parts & (1ull << idx))) continue;
        const int w = std::snprintf(names + at, sizeof(names) - at, "%s%s", at ? " " : "", ecm::kPartNames[idx]);
        if (w <= 0 || static_cast<size_t>(w) >= sizeof(names) - at) break;
        at += static_cast<size_t>(w);
    }
    float pre[3] = {}, post[3] = {};
    uint32_t block = 0;
    bool firstOk = false;
    for (int tries = 0; tries < 4 && !firstOk; ++tries) {
        const uint32_t s1 = g_f51First.seq.load(std::memory_order_acquire);
        if (s1 & 1u || s1 == 0) break;
        std::memcpy(pre, g_f51First.pre, sizeof(pre));
        std::memcpy(post, g_f51First.post, sizeof(post));
        block = g_f51First.block;
        std::atomic_thread_fence(std::memory_order_acquire);
        firstOk = g_f51First.seq.load(std::memory_order_relaxed) == s1;
    }
    double raiseS[3] = {};
    nofoot::raiseSeated(g_f51Applied, raiseS);
    Log::get().note("native on foot body raise (F51/F53, key %s): dh measured %.3f m (3P pov up %.3f m from %s, FP side up %.3f m, body axes), low-passed "
                    "%.3f m (tau 1 s, 0..0.5 m; F53: logged only, the target is the constant %.2f m), applied %.3f m; view: seated offset (0, %.3f, 0) m, eye align published (%.3f, %.3f, %.3f) m; FP "
                    "records (AMC %016llX): %llu shifted, %llu skipped over %llu passes this second%s, along the up row (%.3f, %.3f, %.3f); parts: %s; "
                    "first shifted %s: block %u %s, before (%.3f, %.3f, %.3f) after (%.3f, %.3f, %.3f)",
                    raiseOn ? "on" : g_raiseFpKey.load(std::memory_order_relaxed) ? "on, not active" : "off", g_f51Dh.raw, g_f51PovUp,
                    g_f51Src == 0 ? "the povCamera joint" : g_f51Src == 1 ? "the head + 0.038 m" : "nothing yet", g_f51FpUp, g_f51Dh.lp,
                    nofoot::raiseConstM(true, g_raiseCm.load(std::memory_order_relaxed)), g_f51Applied,
                    raiseS[1], g_f51EyePub[0], g_f51EyePub[1], g_f51EyePub[2], static_cast<unsigned long long>(g_fpAmcLocal.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(shifted), static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(passes),
                    notMode2 ? " (the latched FP AMC was not mode 2 on some calls)" : "", g_f51Up[0], g_f51Up[1], g_f51Up[2], at ? names : "none",
                    firstOk ? "record" : "(none yet)", block, block < ecm::kPartCount ? ecm::kPartNames[block] : "?", static_cast<double>(pre[0]),
                    static_cast<double>(pre[1]), static_cast<double>(pre[2]), static_cast<double>(post[0]), static_cast<double>(post[1]),
                    static_cast<double>(post[2]));
    // The weapon: the FP handAttach joints (FP 52 L, 51 R) in world through the FP root's yaw (as F45), as the skeleton places them (where a weapon
    // attached to the joint draws) and where the shifted arms put them (+ the applied raise along up).
    if (s.extOkFp && fpFresh) {
        const double rn = std::sqrt(static_cast<double>(fp.rows[6]) * fp.rows[6] + static_cast<double>(fp.rows[8]) * fp.rows[8]);
        const double cy = rn > 1e-6 ? fp.rows[8] / rn : 1.0, sy = rn > 1e-6 ? fp.rows[6] / rn : 0.0;
        double w[2][3] = {};
        for (size_t side = 0; side < 2; ++side) {
            const float* m = &s.extFpM[3 * (2 + side)];
            w[side][0] = static_cast<double>(fp.origin[0]) + m[0] * cy + m[2] * sy;
            w[side][1] = static_cast<double>(fp.origin[1]) + m[1];
            w[side][2] = static_cast<double>(fp.origin[2]) - m[0] * sy + m[2] * cy;
        }
        const double r = g_f51Applied;
        Log::get().note("native on foot body raise (F51): hand attach (world) L: skeleton (%.3f, %.3f, %.3f), with the arms' shift (%.3f, %.3f, %.3f); R: "
                        "skeleton (%.3f, %.3f, %.3f), with the arms' shift (%.3f, %.3f, %.3f); weapon: not an AMC part -- F54 shifts it as an MGMC "
                        "record when it matches (see the F54 lines); unshifted it would draw %.3f m below the hands while the raise is applied",
                        w[0][0], w[0][1], w[0][2], w[0][0] + r * g_f51Up[0], w[0][1] + r * g_f51Up[1], w[0][2] + r * g_f51Up[2], w[1][0], w[1][1],
                        w[1][2], w[1][0] + r * g_f51Up[0], w[1][1] + r * g_f51Up[1], w[1][2] + r * g_f51Up[2], r);
    } else {
        Log::get().note("native on foot body raise (F51): hand attach: not read (%s); weapon: not an AMC part (F54: "
                        "the MGMC shift needs these joints)",!s.extOkFp ? "the FP attach joints are not in the census" : "the FP root is not fresh");
    }
}

void alignTick(uint64_t now) noexcept {
    static int lastMode = -1;
    const int mode = g_alignMode.load(std::memory_order_relaxed);   // 0 off, 1 view, 2 body
    AlignRun& a = mode == 2 ? g_alignBody : g_align;
    AlignRun& other = mode == 2 ? g_align : g_alignBody;
    const bool on = mode != 0;
    if (mode != lastMode) {
        static const char* const kNames[4] = {"off", "view", "body", "calibrate"};
        Log::get().note("native on foot body align: mode %s (was %s); trims right %.1f, up %.1f, forward %.1f cm; view gain 0.02 per frame, tau 100 ms; body no integral, tau 100 ms (camera 100 ms, pov 400 ms); "
                        "clamp 0.6 m, 5 cm per frame at most; guard 2 s", kNames[mode], lastMode < 0 ? "none" : kNames[lastMode], g_alignTrimCm[0],
                        g_alignTrimCm[1], g_alignTrimCm[2]);
        a.tripped = false;
        a.guardArmed = false;
        lastMode = mode;
        if (mode == 2) {
            g_f37TableWanted.store(true, std::memory_order_relaxed);   // the per-part table prints on the next shifted pass of this entry
            Log::get().note("native on foot body shift table: requested on entering body (printed on the next shifted pass)");
        }   // F37: the per-part table on entering body
        g_f38Valid = false;
        g_f38LpValid = false;
        g_f38LineMs = 0;
        g_f41LpValid = false;
        g_f48Stand.reset();   // F48: the standing height is captured again at key-on
        g_f48PovSrc = -1;
        g_f48Have = false;
        g_f55KindPrev = g_f55RestKind = 0;   // F55: the walking shift's rest is captured again at idle
        g_f55Used = false;
        if (mode == 3) calibrateReset();
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
    // F55: the walking shift's locomotion gate: the body's horizontal speed under 0.35 m/s for 0.3 s (the F49 rule), fed each new body sample.
    if (s.valid && s.stamp != g_f55LocoStamp) {
        g_f55LocoStamp = s.stamp;
        const double lpos[3] = {s.origin[0], s.origin[1], s.origin[2]}, lup[3] = {s.rows[3], s.rows[4], s.rows[5]};
        g_f55Loco.step(s.stamp, lpos, lup, false);
    }
    if (!s.valid || (now >= s.stamp && now - s.stamp > 1500)) g_f55Loco = nofoot::PfzLocoRest{};
    const bool f55Moving = !g_f55Loco.resting;
    const char* wait = nullptr;
    if (!on) wait = "key off";
    else if (!nativeOnFootActive()) wait = "not native stereo on foot";
    else if (!s.valid || now - s.stamp > static_cast<uint64_t>(align::kStaleMs)) wait = "the local third-person AMC sample is missing or over 100 ms old";
    else if (!s.headOk) wait = "no head joint (the neck census has no sample)";
    else if (!s.povOk) wait = "no pov (the neck census has no head sample)";
    else if ((mode == 1 || mode == 3) && !haveView) wait = "no list-B view (the P2 sample is missing or over 500 ms old)";
    else if ((mode == 1 || mode == 3) && !haveFrame) wait = "no render head from the runtime";
    else if (ads) wait = "held: aiming down sights";
    if (wait && on && now - a.waitLineMs >= 5000) {
        a.waitLineMs = now;
        Log::get().note("native on foot body align: %s: waiting (%s)", mode == 2 ? "body" : "view", wait);
    }
    if (mode == 2 && ads) g_f48Stand.step(now, 0.0, 0.0, true);   // F48: ADS is never a rest; the standing window starts again after it
    const int anchorMode = g_alignAnchorMode.load(std::memory_order_relaxed);   // F48: 0 neck, 1 clavicles, 2 pov
    const bool vertical = g_alignVertical.load(std::memory_order_relaxed);      // F50: off locks the body's height in body mode
    static int lastVerticalNote = -1;   // F50: the vertical-off note, once per entry to body (and on a key change while in body)
    if (mode != 2) lastVerticalNote = -1;
    else if (lastVerticalNote != (vertical ? 1 : 0)) {
        lastVerticalNote = vertical ? 1 : 0;
        // F55: at each body entry (and on a live change): the active value, prominently.
        Log::get().note("native on foot body (F55): ===== body vertical: %s (fix.native_on_foot_body_align_vertical; the code default is off) =====",
                        vertical ? "ON (d moves the body in all three axes)" : "OFF (feet on the ground)");
        if (!vertical)
            Log::get().note("native on foot body align (F50): body mode with vertical off: d moves the body along its right and forward axes only; "
                            "the up trim (%.1f cm) is ignored; the standing height is logged, not applied; the up component d would have had is "
                            "logged once a second", g_alignTrimCm[1]);
    }
    static int lastAnchorMode = -1;
    if (anchorMode != lastAnchorMode) {   // F48: a live anchor change measures a different point: capture its standing height afresh
        if (lastAnchorMode >= 0) {
            static const char* const kAnchorNames[3] = {"neck", "clavicles", "pov"};
            Log::get().note("native on foot body align: anchor %s (was %s); the standing height is captured again",
                            kAnchorNames[anchorMode], kAnchorNames[lastAnchorMode]);
        }
        lastAnchorMode = anchorMode;
        g_f48Stand.reset();
        g_f48PovSrc = -1;
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
    // F45: a point in body axes, relative to the AMC origin (the rows of this frame's sample).
    const auto bodyD = [&](const double q[3], double out[3]) {
        for (size_t i = 0; i < 3; ++i) {
            double acc = 0.0;
            for (size_t j = 0; j < 3; ++j) acc += (q[j] - static_cast<double>(s.origin[j])) * static_cast<double>(s.rows[3 * i + j]);
            out[i] = acc;
        }
    };
    // F43: the shoulder anchor's state for this frame (set in the !wait block; zero when the anchor is not the shoulders).
    bool shoulderAnchor = false;
    double fpShW[3] = {}, tpShW[3] = {};
    bool f51Have = false;   // F51: this frame measured dh (body mode, inputs fresh, the FP root fresh)
    double f51Raw = 0.0;
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
        // F38: the pov target, in body axes. With the neck, the neck plus the head-to-neck offset (pov minus neck) captured over the first
        // second of this entry; otherwise the pov low-passed in body axes (tau 1 s), so the head's own swing drops out and the body's turns
        // do not lag. The means are frozen after the first second. The camera minus the FP root is logged, not applied.
        double neckB[3] = {}, jobB[3] = {}, fpB[3] = {};
        if (s.neckOk) body(s.neck, neckB);
        body(s.cam, jobB);
        if (fpFresh) body(fp.origin, fpB);
        if (!g_f38Valid) {
            g_f38Valid = true;
            g_f38StartMs = now;
            g_f38R0N = 0;
            g_f38CamFpN = 0;
            g_f38CamNeckN = 0;
            for (size_t i = 0; i < 3; ++i) g_f38R0[i] = g_f38CamFp[i] = g_f38CamNeck[i] = 0.0;
        }
        const bool inWin = now - g_f38StartMs < 1000;
        if (!g_f38LpValid) {
            for (size_t i = 0; i < 3; ++i) g_f38Lp[i] = povB[i];
            g_f38LpValid = true;
        }
        const double alpha = dt > 0.0 ? 1.0 - std::exp(-dt / 1.0) : 0.0;   // tau 1 s
        for (size_t i = 0; i < 3; ++i) g_f38Lp[i] += alpha * (povB[i] - g_f38Lp[i]);
        if (inWin && s.neckOk) {
            ++g_f38R0N;
            for (size_t i = 0; i < 3; ++i) g_f38R0[i] += ((povB[i] - neckB[i]) - g_f38R0[i]) / static_cast<double>(g_f38R0N);
        }
        if (inWin && s.neckOk) {
            ++g_f38CamNeckN;
            for (size_t i = 0; i < 3; ++i) g_f38CamNeck[i] += ((jobB[i] - neckB[i]) - g_f38CamNeck[i]) / static_cast<double>(g_f38CamNeckN);
        }
        if (inWin && fpFresh) {
            ++g_f38CamFpN;
            for (size_t i = 0; i < 3; ++i) g_f38CamFp[i] += ((jobB[i] - fpB[i]) - g_f38CamFp[i]) / static_cast<double>(g_f38CamFpN);
        }
        const bool useNeck = s.neckOk && g_f38R0N > 0;
        double povEqB[3] = {}, povEqW[3] = {};
        // F42/F45: the neck plus the pov-minus-neck offset captured over the first second of the entry (F38); without it, the low-passed head.
        for (size_t i = 0; i < 3; ++i) povEqB[i] = useNeck ? neckB[i] + g_f38R0[i] : g_f38Lp[i];
        for (size_t j = 0; j < 3; ++j) {
            povEqW[j] = static_cast<double>(s.origin[j]);
            for (size_t i = 0; i < 3; ++i) povEqW[j] += povEqB[i] * static_cast<double>(s.rows[3 * i + j]);
        }
        // F43: the shoulder means in world. The 3P shoulders through the AMC matrix; the FP shoulders through the FP root's yaw only (the
        // torso does not slide with the aim pitch), and the FP root's origin. Both need a fresh FP root and both skeletons' shoulders.
        shoulderAnchor = mode == 2 && anchorMode == 1 && s.shOk3P && s.shOkFp && fpFresh;
        if (s.shOk3P && s.shOkFp && fpFresh) {
            const double rn = std::sqrt(static_cast<double>(fp.rows[6]) * fp.rows[6] + static_cast<double>(fp.rows[8]) * fp.rows[8]);
            const double cy = rn > 1e-6 ? fp.rows[8] / rn : 1.0, sy = rn > 1e-6 ? fp.rows[6] / rn : 0.0;
            const double Y[3][3] = {{cy, 0.0, -sy}, {0.0, 1.0, 0.0}, {sy, 0.0, cy}};   // the yaw-only basis, row 2 forward
            for (size_t k = 0; k < 2; ++k)
                for (size_t j = 0; j < 3; ++j) {
                    double w = static_cast<double>(fp.origin[j]);
                    for (size_t mI = 0; mI < 3; ++mI) w += static_cast<double>(s.shFpM[3 * k + mI]) * Y[mI][j];
                    fpShW[j] += 0.5 * w;
                    tpShW[j] += 0.5 * static_cast<double>(s.sh3PW[3 * k + j]);
                }
        }
        if (now - g_f38LineMs >= 5000 || g_f38LineMs == 0) {
            g_f38LineMs = now;
            double off = 0.0;
            for (size_t i = 0; i < 3; ++i) off += (povEqB[i] - povB[i]) * (povEqB[i] - povB[i]);
            Log::get().note("native on foot body align: pov target from %s: pov minus neck (body) %.3f %.3f %.3f m, target minus pov %.3f m; "
                            "camera minus neck (body) %.3f %.3f %.3f m; camera minus fp root (body) %.3f %.3f %.3f m%s",
                            useNeck ? "the neck plus the captured offset" : "the low-passed head",
                            g_f38R0[0], g_f38R0[1], g_f38R0[2],
                            std::sqrt(off), g_f38CamNeck[0], g_f38CamNeck[1], g_f38CamNeck[2], g_f38CamFp[0], g_f38CamFp[1], g_f38CamFp[2],
                            fpFresh ? "" : " (fp root not fresh)");
        }
        double trimB[3] = {g_alignTrimCm[0] / 100.0, g_alignTrimCm[1] / 100.0, g_alignTrimCm[2] / 100.0};   // right, up, forward
        if (mode == 2) {
            // F41: body is open loop (the render records move; the camera, the neck and the pov do not), so no integral. The camera and the pov
            // are low-passed in body axes (0.1 s and 0.4 s: the sway and the camera's jitter out). The target is (camera - pov) - trims, in body
            // axes; bodyFilter takes it to the offset through one critically damped filter. The target goes to the filter in world axes.
            // F43: the anchor's two sides in body axes: the FP shoulders (the anchor, the camera's place) and the 3P shoulders (the pov's place) when
            // the shoulder anchor holds; otherwise the camera against the neck anchor (F41).
            // F48: the pov anchor (the default): the 3P povCamera joint (body axes) placed at the FP root (camB); without the joint, the head
            // plus (0, 0.038, 0.112) in body axes (the joint's rest offset from the head on the 3P skeleton). The source is logged on each change.
            double anchorB[3] = {}, srcB[3] = {};
            if (shoulderAnchor) {
                bodyD(fpShW, anchorB);
                bodyD(tpShW, srcB);
            } else if (anchorMode == 2) {
                for (size_t i = 0; i < 3; ++i) anchorB[i] = camB[i];
                int src = 0;
                if (s.povJOk) {
                    body(s.povJ, srcB);
                } else {
                    src = 1;
                    body(s.head, srcB);
                    srcB[1] += 0.038;
                    srcB[2] += 0.112;
                }
                if (src != g_f48PovSrc) {
                    g_f48PovSrc = src;
                    if (src == 0)
                        Log::get().note("native on foot body align: pov anchor from the 3P def_c_povCamera_joint (index %u), placed at the FP root%s",
                                        static_cast<unsigned>(s.povJIdx), fpFresh ? "" : " (fp root not fresh: the job camera)");
                    else
                        Log::get().note("native on foot body align: pov anchor from the 3P head plus (0, 0.038, 0.112) m in body axes (the povCamera joint "
                                        "did not resolve), placed at the FP root%s", fpFresh ? "" : " (fp root not fresh: the job camera)");
                }
            } else {
                // F53: the neck with the rest geometry (nofoot::neckRestSource): the neck placed where the pov is at rest, so d converges on
                // the neck at the FP side plus (0, -0.155, -0.161) m. F42/F46's offset captured over the entry's first second carried the
                // stance and the twist; it is logged, not applied. Without the neck: the low-passed head (as before).
                for (size_t i = 0; i < 3; ++i) anchorB[i] = camB[i];
                if (s.neckOk) nofoot::neckRestSource(neckB, srcB);
                else
                    for (size_t i = 0; i < 3; ++i) srcB[i] = povEqB[i];
            }
            // F48: the standing height. The 3P side's live height is tracked on every anchor (logged); the pov anchor alone uses the captured
            // value in place of it (neck and clavicles keep F46's and F45's behaviour). x and z stay live.
            g_f48LiveUp = srcB[1];
            // F55: the rest point for the walking shift: the neck itself on the neck anchor (srcB = neck - kNeckRestOffset, so the offset is
            // srcB - neck), else the anchor's own source (offset 0). A kind change restarts the window, so a capture never mixes two points.
            {
                const int kind = shoulderAnchor ? 3 : anchorMode == 2 ? (g_f48PovSrc == 0 ? 4 : 5) : (s.neckOk ? 1 : 2);
                for (size_t i = 0; i < 3; ++i) {
                    g_f55Raw[i] = kind == 1 ? neckB[i] : srcB[i];
                    g_f55Off[i] = srcB[i] - g_f55Raw[i];
                }
                if (kind != g_f55KindPrev) {
                    g_f55KindPrev = kind;
                    g_f48Stand.n = 0;
                }
                g_f55KindNow = kind;
            }
            const uint32_t f55CapBefore = g_f48Stand.hCaptures;
            g_f48Stand.step(now, static_cast<double>(s.headBody[1]), srcB[1], false, f55Moving, g_f55Raw[0], g_f55Raw[2]);
            if (anchorMode == 2 && !shoulderAnchor && g_f48Stand.valid && vertical) {   // F50: vertical off: logged only
                const double live[3] = {srcB[0], srcB[1], srcB[2]};
                nofoot::standingApply(live, g_f48Stand.value, srcB);
            }
            // F55: the walking shift. The source's horizontal is the rest captured at idle plus the offset, so d_h = FP side_h - (rest_h +
            // offset_h) stays constant in the body frame while the neck moves with locomotion and aim. Live until the first idle capture.
            for (size_t i = 0; i < 3; ++i) g_f55Live[i] = srcB[i];
            if (g_f48Stand.hCaptures != f55CapBefore) {
                g_f55RestKind = g_f55KindNow;
                Log::get().note("native on foot walking shift (F55): rest captured (#%u, 1 s idle: speed %.2f m/s, no ADS, head steady): %s x %.3f "
                                "z %.3f m (body axes); source rest (plus the offset %.3f, %.3f) x %.3f z %.3f m; live source x %.3f z %.3f m; standing "
                                "height %.3f m", static_cast<unsigned>(g_f48Stand.hCaptures), g_f55Loco.speed, kF55KindNames[g_f55RestKind],
                                g_f48Stand.hx, g_f48Stand.hz, g_f55Off[0], g_f55Off[2], g_f48Stand.hx + g_f55Off[0], g_f48Stand.hz + g_f55Off[2],
                                g_f55Live[0], g_f55Live[2], g_f48Stand.value);
            }
            g_f55Used = g_f48Stand.hValid && g_f55RestKind == g_f55KindNow;
            if (g_f55Used) {
                const double live[3] = {srcB[0], srcB[1], srcB[2]};
                nofoot::walkRestSource(live, g_f55Off, g_f48Stand.hx, g_f48Stand.hz, srcB);
            }
            g_f48Have = true;
            for (size_t i = 0; i < 3; ++i) {
                g_f48SrcB[i] = srcB[i];
                g_f48CamB[i] = anchorB[i];
            }
            // F51: dh's measurement, whatever the anchor: the 3P povCamera joint's height (the head plus 0.038 m without it) less the FP side's
            // (the FP root), body axes up, both live (the vertical lock and the standing height do not enter it).
            if (fpFresh) {
                double pj[3] = {};
                if (s.povJOk) {
                    body(s.povJ, pj);
                } else {
                    body(s.head, pj);
                    pj[1] += 0.038;
                }
                g_f51PovUp = pj[1];
                g_f51FpUp = camB[1];
                g_f51Src = s.povJOk ? 0 : 1;
                f51Raw = g_f51PovUp - g_f51FpUp;
                f51Have = true;
            }
            if (!g_f41LpValid) {
                for (size_t i = 0; i < 3; ++i) {
                    g_f41CamLp[i] = anchorB[i];
                    g_f41PovLp[i] = srcB[i];
                }
                g_f41LpValid = true;
            }
            const double aCam = 1.0 - std::exp(-dt / align::kBodyCamTauSec);
            const double aPov = 1.0 - std::exp(-dt / align::kBodyPovTauSec);
            if (!vertical) trimB[1] = 0.0;   // F50: the up trim is ignored with the body's height locked
            for (size_t i = 0; i < 3; ++i) {
                g_f41CamLp[i] += aCam * (anchorB[i] - g_f41CamLp[i]);
                g_f41PovLp[i] += aPov * (srcB[i] - g_f41PovLp[i]);
                g_f41TgtB[i] = (g_f41CamLp[i] - g_f41PovLp[i]) - trimB[i];
            }
            for (size_t j = 0; j < 3; ++j) {
                err[j] = 0.0;
                for (size_t i = 0; i < 3; ++i) err[j] += g_f41TgtB[i] * static_cast<double>(s.rows[3 * i + j]);
            }
        } else {
            // F37: the head's motion only. tS is the render head's translation in seated metres, less tS0 (the first head of this reference
            // generation: captured when the view starts and at each recentre). Mapped to body axes: v = R^T tS, dw = C.rot v, hmd_i = row_i . dw
            // (a vector, no origin subtraction). The sign is the sign check's to prove (the 1 s line).
            double headTrue[3] = {};
            nofoot::alignHeadTrue(np.render, a.applied, headTrue);   // F38: the head is the render translation plus the applied offset
            if (!g_f37TS0Valid || np.referenceGeneration != g_f37TS0Gen) {
                for (size_t i = 0; i < 3; ++i) g_f37TS0[i] = headTrue[i];
                g_f37TS0Gen = np.referenceGeneration;
                g_f37TS0Valid = true;
                Log::get().note("native on foot body align: head reference tS0 captured at (%.3f, %.3f, %.3f) m seated (reference generation %u)",
                                static_cast<double>(g_f37TS0[0]), static_cast<double>(g_f37TS0[1]), static_cast<double>(g_f37TS0[2]),
                                static_cast<unsigned>(g_f37TS0Gen));
            }
            for (size_t i = 0; i < 3; ++i) g_f37TSNow[i] = headTrue[i] - g_f37TS0[i];
            // F40: the body-to-seated map, seated = D . Ry(psi) . body, D = diag(-1, -1, +1). psi is the seated head yaw less the headset's yaw in the
            // body (viewPsi). The head's motion in body axes is Ry(psi)^-1 . diag(1, 1, -1) . tS. The error is the target minus the camera (the
            // trims sit in the target), so the integral's step, through the map, closes it. The C-chain of F37 is gone.
            const double psi = 0.0;   // F41: psi is zero, the measured map; look_to_aim's seated yaw is not supplied yet (TODO)
            g_f40Psi = psi;
            nofoot::hmdInBody(g_f37TSNow, psi, hmdB);
            double errBody[3] = {};
            for (size_t i = 0; i < 3; ++i) {
                deltaB[i] = (camB[i] - hmdB[i]) - povEqB[i];
                errBody[i] = -(deltaB[i] - trimB[i]);
            }
            nofoot::seatedFromBody(errBody, psi, err);
            if (now - g_f40ViewLogMs >= 5000 || g_f40ViewLogMs == 0) {
                g_f40ViewLogMs = now;
                Log::get().note("native on foot body align: view map: psi %.1f deg; target minus camera (body) %.3f %.3f %.3f m; seated offset %.3f %.3f %.3f m",
                                psi * 180.0 / 3.14159265358979323846, errBody[0], errBody[1], errBody[2], err[0], err[1], err[2]);
            }
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
    if (mode == 3) {
        calibrateStep(now, camB, g_f37TSNow, !wait, a);   // F40: scheduled, no loop, no guard
    } else if (mode == 2) {
        bodyFilter(a, err, dt);
        // F50: with vertical off, d keeps only its horizontal part: the body run's position and velocity lose their component along the body's
        // up axis (AMC row 1, world). What d's up component would have been is logged once a second: the target's up (body axes, the up trim
        // excluded), which d converges to with vertical on, and this frame's filter step along up that was dropped.
        static uint64_t vertN = 0, vertLineMs = 0;
        static double vertSum = 0.0, vertMin = 0.0, vertMax = 0.0, dropMax = 0.0;
        if (!vertical) {
            const double up[3] = {static_cast<double>(s.rows[3]), static_cast<double>(s.rows[4]), static_cast<double>(s.rows[5])};
            const double dropped = nofoot::dropAlong(a.pos, up);
            nofoot::dropAlong(a.vel, up);
            if (!wait) {
                const double wouldUp = g_f41TgtB[1];
                if (!vertN) vertMin = vertMax = wouldUp;
                ++vertN;
                vertSum += wouldUp;
                vertMin = std::min(vertMin, wouldUp);
                vertMax = std::max(vertMax, wouldUp);
                dropMax = std::max(dropMax, std::fabs(dropped));
            }
            if (now - vertLineMs >= 1000 && vertN) {
                vertLineMs = now;
                Log::get().note("native on foot body align (F50): vertical off: the up component d would have had (target, body axes, up trim "
                                "excluded) %.3f m mean, %.3f..%.3f m over %llu frames; dropped per frame up to %.4f m (the filter's step along up); "
                                "standing height %.3f m (%s), live 3P height %.3f m",
                                vertSum / static_cast<double>(vertN), vertMin, vertMax, static_cast<unsigned long long>(vertN), dropMax,
                                g_f48Stand.value, g_f48Stand.valid ? "captured, logged only" : "not captured", g_f48LiveUp);
                vertN = 0;
                vertSum = dropMax = 0.0;
            }
        } else {
            vertN = 0;
            vertSum = dropMax = 0.0;
        }
        if (now - g_f41BodyLogMs >= 1000 || g_f41BodyLogMs == 0) {
            g_f41BodyLogMs = now;
            double dB[3] = {};
            for (size_t i = 0; i < 3; ++i)
                for (size_t j = 0; j < 3; ++j) dB[i] += a.pos[j] * static_cast<double>(s.rows[3 * i + j]);
            double before = 0.0, after = 0.0;
            for (size_t j = 0; j < 3; ++j) {
                const double dd = fpShW[j] - tpShW[j], da = fpShW[j] - (tpShW[j] + a.pos[j]);
                before += dd * dd;
                after += da * da;
            }
            double fpClB[3] = {}, tpClB[3] = {}, hdB[3] = {}, ndB[3] = {};
            bodyD(fpShW, fpClB);
            bodyD(tpShW, tpClB);
            const double hq[3] = {static_cast<double>(s.head[0]), static_cast<double>(s.head[1]), static_cast<double>(s.head[2])};
            const double nq[3] = {static_cast<double>(s.neck[0]), static_cast<double>(s.neck[1]), static_cast<double>(s.neck[2])};
            bodyD(hq, hdB);
            bodyD(nq, ndB);
            // F45: the FP root's forward row against the body's horizontal: the forward row's dot with the body's up row (AMC row 1).
            const double fwdUp = static_cast<double>(fp.rows[6]) * s.rows[3] + static_cast<double>(fp.rows[7]) * s.rows[4] +
                                 static_cast<double>(fp.rows[8]) * s.rows[5];
            const double pitchDeg = std::asin(std::max(-1.0, std::min(1.0, fwdUp))) * 180.0 / 3.14159265358979323846;
            Log::get().note("native on foot body align: clavicles in body axes: FP %.3f %.3f %.3f m, 3P %.3f %.3f %.3f m; distance %.3f m before d, "
                            "%.3f m after; d (right, up, forward) %.3f %.3f %.3f m; target %.3f %.3f %.3f m; head in body %.3f %.3f %.3f m; "
                            "neck in body %.3f %.3f %.3f m; anchor %s (FP clavicles %s, 3P %s); FP root pitch vs the body's horizontal %.1f deg%s",
                            fpClB[0], fpClB[1], fpClB[2], tpClB[0], tpClB[1], tpClB[2], std::sqrt(before), std::sqrt(after), dB[0], dB[1], dB[2],
                            g_f41TgtB[0], g_f41TgtB[1], g_f41TgtB[2], hdB[0], hdB[1], hdB[2], ndB[0], ndB[1], ndB[2],
                            shoulderAnchor ? "clavicles" : anchorMode == 2 ? "pov" : "neck", (s.shOkFp && fpFresh) ? "yes" : "no", s.shOk3P ? "yes" : "no",
                            pitchDeg, anchorMode == 1 && !shoulderAnchor ? " (clavicles not resolved: neck anchor)" : "");
            // F48: the pov anchor's two sides, the standing height (captured and live) and where the torso's neck lands against the FP root
            // after d (expected about (0, -0.156, -0.161) m when the FP root is the eye).
            if (g_f48Have) {
                double neckAfter[3] = {};
                for (size_t i = 0; i < 3; ++i) neckAfter[i] = ndB[i] + dB[i] - g_f48CamB[i];
                Log::get().note("native on foot body align: anchor %s: 3P side (body) %.3f %.3f %.3f m (source %s), FP side %.3f %.3f %.3f m; "
                                "standing height %.3f m (%s, %u captures), live %.3f m; neck after d minus the FP side (body) %.3f %.3f %.3f m%s",
                                shoulderAnchor ? "clavicles" : anchorMode == 2 ? "pov" : "neck", g_f48SrcB[0], g_f48SrcB[1], g_f48SrcB[2],
                                anchorMode == 2 && !shoulderAnchor ? (g_f48PovSrc == 0 ? "povCamera joint" : "head + (0, 0.038, 0.112)")
                                                                   : (shoulderAnchor ? "3P clavicles" : s.neckOk ? "neck + rest (0, 0.155, 0.161) (F53)"
                                                                                                                  : "the low-passed head (no neck)"),
                                g_f48CamB[0], g_f48CamB[1], g_f48CamB[2], g_f48Stand.value,
                                !g_f48Stand.valid ? "not captured"
                                : (anchorMode == 2 && !shoulderAnchor) ? (vertical ? "applied" : "logged, not applied: vertical off")
                                                                       : "logged, not applied",
                                static_cast<unsigned>(g_f48Stand.captures), g_f48LiveUp, neckAfter[0], neckAfter[1], neckAfter[2],
                                s.neckOk ? "" : " (no neck: the neck figure is not meaningful)");
            }
            // F55: the walking shift: the rest it holds, whether it is used, the live source, d's horizontal target.
            Log::get().note("native on foot walking shift (F55): %s; rest %s x %.3f z %.3f m (%u idle captures), offset %.3f %.3f, source x %.3f z %.3f m; "
                            "live %s x %.3f z %.3f m (live source x %.3f z %.3f m); target d_h (right, forward) %.3f %.3f m; speed %.2f m/s (%s)",
                            g_f55Used ? "the captured rest is used" : !g_f48Stand.hValid ? "live (no idle capture yet)" : "live (the rest point's kind changed)",
                            kF55KindNames[g_f55RestKind], g_f48Stand.hx, g_f48Stand.hz, static_cast<unsigned>(g_f48Stand.hCaptures), g_f55Off[0],
                            g_f55Off[2], g_f48SrcB[0], g_f48SrcB[2], kF55KindNames[g_f55KindNow], g_f55Raw[0], g_f55Raw[2], g_f55Live[0], g_f55Live[2],
                            g_f41TgtB[0], g_f41TgtB[2], g_f55Loco.speed, f55Moving ? "moving: no capture" : "idle");
            // F55: per side, body axes after d: the FP clavicles and hands (the hook's span snapshot of joints 19/20 and 37/44 after any drop when
            // fresh, else the census by name; FP model to world through the FP root's yaw, as F45), the 3P clavicles and hands moved by d; the
            // freeze weight; d's up component (zero with vertical off).
            {
                float sb[4][3] = {}, sa[4][3] = {}, sDrop = 0.0f;
                uint64_t sMs = 0;
                const bool snapFresh = f55SnapRead(sb, sa, &sDrop, &sMs) && sMs != 0 && now >= sMs && now - sMs < 500;
                const double rnY = std::sqrt(static_cast<double>(fp.rows[6]) * fp.rows[6] + static_cast<double>(fp.rows[8]) * fp.rows[8]);
                const double cyY = rnY > 1e-6 ? fp.rows[8] / rnY : 1.0, syY = rnY > 1e-6 ? fp.rows[6] / rnY : 0.0;
                const auto fpBody = [&](const float* m, double out[3]) {
                    const double w[3] = {static_cast<double>(fp.origin[0]) + m[0] * cyY + m[2] * syY, static_cast<double>(fp.origin[1]) + m[1],
                                         static_cast<double>(fp.origin[2]) - m[0] * syY + m[2] * cyY};
                    bodyD(w, out);
                };
                const auto tpBody = [&](const float* w, double out[3]) {
                    const double q[3] = {w[0] + a.pos[0], w[1] + a.pos[1], w[2] + a.pos[2]};
                    bodyD(q, out);
                };
                double fC[2][3] = {}, fH[2][3] = {}, tC[2][3] = {}, tH[2][3] = {};
                const bool fpOk = fpFresh && (snapFresh || (s.shOkFp && s.extOkFp));
                const bool tpOk = s.shOk3P && s.extOk3P;
                for (size_t side = 0; side < 2; ++side) {
                    if (fpOk) {
                        fpBody(snapFresh ? sa[side] : &s.shFpM[3 * side], fC[side]);
                        fpBody(snapFresh ? sa[2 + side] : &s.extFpM[3 * side], fH[side]);
                    }
                    if (tpOk) {
                        tpBody(&s.sh3PW[3 * side], tC[side]);
                        tpBody(&s.ext3PW[3 * side], tH[side]);
                    }
                }
                Log::get().note("native on foot body (F55): body axes after d (right, up, forward), L: FP clavicle %.3f %.3f %.3f hand %.3f %.3f %.3f, 3P "
                                "clavicle %.3f %.3f %.3f hand %.3f %.3f %.3f; R: FP clavicle %.3f %.3f %.3f hand %.3f %.3f %.3f, 3P clavicle %.3f %.3f %.3f "
                                "hand %.3f %.3f %.3f; FP from %s, 3P from %s; freeze weight %.2f; d up applied %.3f m (vertical %s); d (body) %.3f %.3f "
                                "%.3f m; FP raise %.3f m; shoulder drop %.3f m",
                                fC[0][0], fC[0][1], fC[0][2], fH[0][0], fH[0][1], fH[0][2], tC[0][0], tC[0][1], tC[0][2], tH[0][0], tH[0][1], tH[0][2],
                                fC[1][0], fC[1][1], fC[1][2], fH[1][0], fH[1][1], fH[1][2], tC[1][0], tC[1][1], tC[1][2], tH[1][0], tH[1][1], tH[1][2],
                                !fpOk ? "NOTHING (zeros: no fresh FP root or joints)" : snapFresh ? "the Animate span (19/37 L, 20/44 R, after the drop)"
                                                                                                  : "the census by name (L, R)",
                                tpOk ? "the census by name (L, R) + d" : "NOTHING (zeros)", static_cast<double>(pfzWeightNow()), dB[1],
                                vertical ? "ON" : "OFF", dB[0], dB[1], dB[2], g_f51Applied, g_f55DropApplied);
                // F55: the shoulder drop, before and after, world (FP root yaw), joints 19, 20, 37, 44; the hook's counters and failures.
                const uint64_t calls = g_fdCalls.load(std::memory_order_relaxed), writes = g_fdWrites.load(std::memory_order_relaxed);
                const uint64_t dCalls = calls - g_f55CallsLast, dWrites = writes - g_f55WritesLast;
                g_f55CallsLast = calls;
                g_f55WritesLast = writes;
                const uint32_t bits = g_fdFailBits.load(std::memory_order_relaxed) & ~g_f55FailNamed;
                for (uint32_t i = 0; i < kFdFailKinds; ++i) {
                    if (!(bits & (1u << i))) continue;
                    g_f55FailNamed |= 1u << i;
                    Log::get().note("native on foot shoulder drop (F55): the hook refused a call: %s (said once)", kFdFailText[i]);
                }
                if (snapFresh && fpFresh) {
                    double wb[4][3] = {}, wa[4][3] = {};
                    for (size_t k = 0; k < 4; ++k) {
                        const float* m0 = sb[k];
                        const float* m1 = sa[k];
                        wb[k][0] = static_cast<double>(fp.origin[0]) + m0[0] * cyY + m0[2] * syY;
                        wb[k][1] = static_cast<double>(fp.origin[1]) + m0[1];
                        wb[k][2] = static_cast<double>(fp.origin[2]) - m0[0] * syY + m0[2] * cyY;
                        wa[k][0] = static_cast<double>(fp.origin[0]) + m1[0] * cyY + m1[2] * syY;
                        wa[k][1] = static_cast<double>(fp.origin[1]) + m1[1];
                        wa[k][2] = static_cast<double>(fp.origin[2]) - m1[0] * syY + m1[2] * cyY;
                    }
                    Log::get().note("native on foot shoulder drop (F55): key %.1f cm, weight %.2f, applied %.3f m (hook's last %.3f m); FP X calls %llu, "
                                    "dropped %llu this second; world before -> after: clavicle 19 (%.3f,%.3f,%.3f) -> (%.3f,%.3f,%.3f), 20 (%.3f,%.3f,%.3f) -> "
                                    "(%.3f,%.3f,%.3f); hand 37 (%.3f,%.3f,%.3f) -> (%.3f,%.3f,%.3f), 44 (%.3f,%.3f,%.3f) -> (%.3f,%.3f,%.3f)%s",
                                    g_dropCm.load(std::memory_order_relaxed), g_f55DropBlend.w, g_f55DropApplied, static_cast<double>(sDrop),
                                    static_cast<unsigned long long>(dCalls), static_cast<unsigned long long>(dWrites), wb[0][0], wb[0][1], wb[0][2],
                                    wa[0][0], wa[0][1], wa[0][2], wb[1][0], wb[1][1], wb[1][2], wa[1][0], wa[1][1], wa[1][2], wb[2][0], wb[2][1], wb[2][2],
                                    wa[2][0], wa[2][1], wa[2][2], wb[3][0], wb[3][1], wb[3][2], wa[3][0], wa[3][1], wa[3][2],
                                    ads ? " (ADS: blending out)" : "");
                } else if (g_dropCm.load(std::memory_order_relaxed) > 0.0) {
                    Log::get().note("native on foot shoulder drop (F55): key %.1f cm, weight %.2f, applied %.3f m; no fresh FP snapshot (%s; FP X calls %llu "
                                    "this second, FP X %016llX)", g_dropCm.load(std::memory_order_relaxed), g_f55DropBlend.w, g_f55DropApplied,
                                    !fpFresh ? "the FP root is not fresh" : "the hook has not seen the FP X in 500 ms", static_cast<unsigned long long>(dCalls),
                                    static_cast<unsigned long long>(pfz::g_fdX.load(std::memory_order_relaxed)));
                }
            }
            // F45: the hands and the attach points, world, on both skeletons: FP through the FP root's yaw, 3P through the AMC matrix.
            if (s.extOk3P && s.extOkFp && fpFresh) {
                const double rnY = std::sqrt(static_cast<double>(fp.rows[6]) * fp.rows[6] + static_cast<double>(fp.rows[8]) * fp.rows[8]);
                const double cyY = rnY > 1e-6 ? fp.rows[8] / rnY : 1.0, syY = rnY > 1e-6 ? fp.rows[6] / rnY : 0.0;
                const auto fpWorld = [&](const float* m, double out[3]) {
                    out[0] = static_cast<double>(fp.origin[0]) + m[0] * cyY + m[2] * syY;
                    out[1] = static_cast<double>(fp.origin[1]) + m[1];
                    out[2] = static_cast<double>(fp.origin[2]) - m[0] * syY + m[2] * cyY;
                };
                const auto dist = [](const double x[3], const double y[3]) {
                    return std::sqrt((x[0] - y[0]) * (x[0] - y[0]) + (x[1] - y[1]) * (x[1] - y[1]) + (x[2] - y[2]) * (x[2] - y[2]));
                };
                double fH[2][3] = {}, tH[2][3] = {}, fA[2][3] = {}, tA[2][3] = {};
                for (size_t side = 0; side < 2; ++side) {
                    fpWorld(&s.extFpM[3 * side], fH[side]);
                    fpWorld(&s.extFpM[3 * (2 + side)], fA[side]);
                    for (size_t j = 0; j < 3; ++j) {
                        tH[side][j] = static_cast<double>(s.ext3PW[3 * side + j]);
                        tA[side][j] = static_cast<double>(s.ext3PW[3 * (2 + side) + j]);
                    }
                }
                Log::get().note("native on foot body align: hands (world) L: FP %.3f %.3f %.3f, 3P %.3f %.3f %.3f, distance %.3f m; R: FP %.3f %.3f %.3f, "
                                "3P %.3f %.3f %.3f, distance %.3f m",
                                fH[0][0], fH[0][1], fH[0][2], tH[0][0], tH[0][1], tH[0][2], dist(fH[0], tH[0]), fH[1][0], fH[1][1], fH[1][2],
                                tH[1][0], tH[1][1], tH[1][2], dist(fH[1], tH[1]));
                Log::get().note("native on foot body align: attach points (world) L: FP %.3f %.3f %.3f, 3P %.3f %.3f %.3f, distance %.3f m; R: FP %.3f %.3f %.3f, "
                                "3P %.3f %.3f %.3f, distance %.3f m",
                                fA[0][0], fA[0][1], fA[0][2], tA[0][0], tA[0][1], tA[0][2], dist(fA[0], tA[0]), fA[1][0], fA[1][1], fA[1][2],
                                tA[1][0], tA[1][1], tA[1][2], dist(fA[1], tA[1]));
            }
        }
    } else {
        alignFilter(a, err, on && !a.tripped && !wait, !on || a.tripped, dt, now, mode != 2);   // F37: the view run
    }
    if (wait && on) ++a.holds;
    const double zero[3] = {0.0, 0.0, 0.0};
    alignFilter(other, zero, false, true, dt, now, mode == 2);

    double change = 0.0;
    for (size_t i = 0; i < 3; ++i) change = std::max(change, std::fabs(a.pos[i] - a.applied[i]));
    if (change > 1e-6) {
        for (size_t i = 0; i < 3; ++i) a.applied[i] = a.pos[i];
        if (mode == 2) bodyOffsetPublish(a.applied);   // the view run's value goes out below, with the F51 raise
    }
    // The other run's published value goes to zero (the view's eye offset when body is on, the body's offset when view is on).
    double otherChange = 0.0;
    for (size_t i = 0; i < 3; ++i) otherChange = std::max(otherChange, std::fabs(other.pos[i] - other.applied[i]));
    if (otherChange > 1e-6) {
        for (size_t i = 0; i < 3; ++i) other.applied[i] = other.pos[i];
        if (mode != 2) bodyOffsetPublish(other.applied);
    }

    // F51: the raise. Body mode with the key on and native stereo on foot: dh through its filter (held on a frame without a measurement); the
    // applied raise follows it at 5 cm a frame at most. Anywhere else the filter resets and the applied raise returns to zero at the same cap (the
    // undo of body mode's raise; nothing is raised outside body mode). The view's seated y is minus the applied raise; the FP records move by it
    // along the body's up row (AMC row 1, world), the axis dh is measured on.
    // F53: the target is the constant raise (fix.native_on_foot_body_raise_cm); dh still runs through its filter for the 1 s line (logged only).
    const bool raiseOn = mode == 2 && g_raiseFpKey.load(std::memory_order_relaxed) && nativeOnFootActive();
    if (mode == 2) g_f51Dh.step(f51Raw, f51Have, dt);
    else g_f51Dh.reset();
    g_f51Applied = nofoot::capToward(g_f51Applied, nofoot::raiseConstM(raiseOn, g_raiseCm.load(std::memory_order_relaxed)), align::kCapPerFrameM);
    if (!raiseOn && std::fabs(g_f51Applied) < 1e-9) g_f51Applied = 0.0;
    g_f51FpOn.store(raiseOn, std::memory_order_relaxed);
    // F55: the FP shoulder drop. Body mode, native stereo on foot, the key above zero, not ADS: the weight ramps to 1 over 0.2 s; otherwise
    // (ADS starting, body mode ending, the key at 0) to 0 over 0.2 s. The applied drop follows at 5 cm a frame at most; the hook reads it.
    {
        const double dropCm = g_dropCm.load(std::memory_order_relaxed);
        const bool dropOn = mode == 2 && dropCm > 0.0 && !ads && nativeOnFootActive();
        g_f55DropBlend.step(dropOn, dt);
        g_f55DropApplied = nofoot::capToward(g_f55DropApplied, nofoot::dropM(g_f55DropBlend.w, dropCm), align::kCapPerFrameM);
        if (std::fabs(g_f55DropApplied) < 1e-9) g_f55DropApplied = 0.0;
        g_f55DropM.store(static_cast<float>(g_f55DropApplied), std::memory_order_relaxed);
    }
    if (s.valid) {
        const double un = std::sqrt(static_cast<double>(s.rows[3]) * s.rows[3] + static_cast<double>(s.rows[4]) * s.rows[4] +
                                    static_cast<double>(s.rows[5]) * s.rows[5]);
        if (un > 1e-6)
            for (size_t i = 0; i < 3; ++i) g_f51Up[i] = static_cast<double>(s.rows[3 + i]) / un;
    }
    {
        double fpD[3] = {};
        for (size_t i = 0; i < 3; ++i) fpD[i] = raiseOn ? g_f51Applied * g_f51Up[i] : 0.0;
        double fpChange = 0.0;
        for (size_t i = 0; i < 3; ++i) fpChange = std::max(fpChange, std::fabs(fpD[i] - g_f51FpPub[i]));
        if (fpChange > 1e-6) {
            for (size_t i = 0; i < 3; ++i) g_f51FpPub[i] = fpD[i];
            fpRaisePublish(fpD);
        }
    }
    // The eye align: the view run (g_align: the active run in view and calibrate, decaying to zero in body) plus the raise's seated term (zero
    // outside body mode once the raise has returned). One serial, g_align's, as before.
    {
        double raiseS[3] = {};
        nofoot::raiseSeated(g_f51Applied, raiseS);
        double eye[3] = {}, eyeChange = 0.0;
        for (size_t i = 0; i < 3; ++i) {
            eye[i] = g_align.applied[i] + raiseS[i];
            eyeChange = std::max(eyeChange, std::fabs(eye[i] - g_f51EyePub[i]));
        }
        if (eyeChange > 1e-6) {
            for (size_t i = 0; i < 3; ++i) g_f51EyePub[i] = eye[i];
            const float f[3] = {static_cast<float>(eye[0]), static_cast<float>(eye[1]), static_cast<float>(eye[2])};
            nativeFrameSetEyeAlign(f, ++g_align.serial);
        }
    }
    if (mode == 2 && (now - g_f51LogMs >= 1000 || g_f51LogMs == 0)) {
        g_f51LogMs = now;
        f51Line(s, fp, fpFresh, raiseOn);
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

// ---- F54 (TEMPORARY): the held weapon, the MotionGraphModelComponent records ----------------------------------------------------------
// The static RE (build 332841): the held weapon is most likely a MotionGraphModelComponent (MGMC). Its submit job 0x43ED820 (reached only
// through the thunk 0x43B796A; rcx = the MGMC) skips an object whose view mask [obj+0x380] is zero, else writes ONE 0x150-byte record at
// obj+0x230, its position at obj+0x240..0x248 and a copy at obj+0x354..0x35C, rewritten from the instance's matrix every run (an add after
// the job is not cumulative), and hands the record's pointer to 0x4C837B0, which the renderer reads later (deferred, as the AMC's 0x3DA1DE0).
// The viewmodel byte is obj+0x1C8. MEASURE (always on in VR, read only; lines once a second while native stereo is on foot): a post-hook on
// the job tables each MGMC with a non-zero mask (pointer, the byte, the mask, the record position, its distance to the FP and 3P handAttach
// joints); a pre-hook on 0x4C837B0 tallies its callers by return address (the AMC's excluded) with the record nearest an FP handAttach joint.
// SHIFT (fix.native_on_foot_body_raise_fp on, body mode, exactly when the FP arms rise): an MGMC that is the local FP weapon (mask non-zero,
// the byte set, the record within 0.3 m of FP handAttach 51 or 52 before the shift) moves by the FP raise (both copies) after the job returns.
// Inside both hooks: no allocation, logging or locks (one try-flag that skips, never waits); every read and write under SEH. The frame thread
// publishes the joints (seqlock) and drains the counters and tables once a second.
namespace f54 {
constexpr uintptr_t kMgmcRva = 0x43ED820;
const uint8_t kMgmcPrologue[35] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x70, 0x20, 0x57, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00, 0x48, 0x8D, 0xB1,
                                   0x80, 0x03, 0x00, 0x00, 0x48, 0x8B, 0xF9, 0x48, 0x83, 0x3E, 0x00, 0x0F, 0x84, 0xE9, 0x03, 0x00, 0x00};
constexpr size_t kMgmcStolen = 8;   // mov rax,rsp; mov [rax+0x20],rsi; push rdi (no RIP-relative operand; the relay enters before they run)
constexpr uintptr_t kRecRva = 0x4C837B0;
const uint8_t kRecPrologue[24] = {0x4C, 0x8B, 0xDC, 0x4D, 0x89, 0x4B, 0x20, 0x4D, 0x89, 0x43, 0x18, 0x55,
                                  0x56, 0x57, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x68};
constexpr size_t kRecStolen = 7;    // mov r11,rsp; mov [r11+0x20],r9
constexpr uintptr_t kOffViewFlag = 0x1C8, kOffPos = 0x240, kOffPosCopy = 0x354, kOffMask = 0x380;
constexpr uintptr_t kRecPosInRecord = 0x10;   // a record's position (every 0x4C837B0 caller's records share the AMC layout)
constexpr uint32_t kRetAmc = 0x3DA2242;
constexpr uint32_t kRecScanCap = 256;           // records scanned per 0x4C837B0 call for the nearest to the FP hand
constexpr uint32_t kNoBest = 0xFFFFFFFFu;
struct KnownCaller {
    uint32_t ret;
    const char* name;
};
// The 0x4C837B0 call sites the RE found (E8 calls: the return address is the call plus 5).
const KnownCaller kKnown[] = {{0x3DA2242, "AMC"},           {0x43EDC24, "MGMC"},           {0x434D8D5, "GUI mesh"},
                              {0x23C72E9, "root 0x23C7100"}, {0x31EC020, "root 0x31EBE90"}, {0x372F491, "root 0x372EF80"},
                              {0x3C9BF53, "root 0x3C9B760"}, {0x3E75F6D, "root 0x3E75DA0"}};
const char* knownName(uint32_t rva) noexcept {
    for (const KnownCaller& k : kKnown)
        if (k.ret == rva) return k.name;
    return "unknown";
}

F9Probe g_mgmcHook, g_recHook;
bool g_tried = false, g_armedMgmc = false, g_armedRec = false;   // frame thread only
std::atomic<uintptr_t> g_base{0};
std::atomic<bool> g_live{false};   // native stereo on foot (frame thread): the hooks table and scan only while it holds

// The joints, world (frame thread publishes each frame; the hooks read): [0] L, [1] R. FP: handAttach 52 L, 51 R through the FP root's yaw (as
// F45); 3P: handAttach 67 L, 66 R through the AMC matrix. Unshifted (the skeleton's own).
struct Joints {
    float fp[2][3] = {}, tp[2][3] = {};
    bool fpOk = false, tpOk = false;
};
std::atomic<uint32_t> g_jSeq{0};
Joints g_j;
void jointsPublish(const Joints& j) noexcept {
    g_jSeq.fetch_add(1, std::memory_order_acq_rel);
    std::memcpy(&g_j, &j, sizeof(j));
    g_jSeq.fetch_add(1, std::memory_order_release);
}
bool jointsRead(Joints& out) noexcept {
    for (int tries = 0; tries < 4; ++tries) {
        const uint32_t s1 = g_jSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        std::memcpy(&out, &g_j, sizeof(out));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_jSeq.load(std::memory_order_relaxed) == s1) return true;
    }
    return false;
}

// The MGMC table: one slot per object with a non-zero mask this second. The tag is (second's generation << 48) | the object pointer; a slot of an
// older generation is free to claim. The data is written under the slot's sequence; the frame thread reads the previous generation's slots.
constexpr uint32_t kSlots = 128, kProbe = 16;
struct MgmcSlot {
    std::atomic<uint64_t> tag{0};
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> calls{0}, shifted{0};
    uint8_t flag = 0;
    uint64_t mask = 0;
    float pos[3] = {};   // the record position before any shift
};
MgmcSlot g_slots[kSlots];
std::atomic<uint32_t> g_gen{1};
std::atomic<uint64_t> g_mgmcCalls{0}, g_mgmcMask0{0}, g_mgmcUnread{0}, g_mgmcFull{0}, g_mgmcShifted{0}, g_mgmcMatched{0};
MgmcSlot* slotFor(uint64_t obj, uint32_t gen) noexcept {
    const uint64_t want = (static_cast<uint64_t>(gen & 0xFFFFu) << 48) | (obj & 0x0000FFFFFFFFFFFFull);
    const uint32_t h = static_cast<uint32_t>(((obj >> 4) * 0x9E3779B97F4A7C15ull) >> 57);
    for (uint32_t i = 0; i < kProbe; ++i) {
        MgmcSlot& s = g_slots[(h + i) % kSlots];
        uint64_t t = s.tag.load(std::memory_order_acquire);
        if (t == want) return &s;
        if ((t >> 48) == (want >> 48)) continue;   // this second's slot of another object
        if (s.tag.compare_exchange_strong(t, want, std::memory_order_acq_rel)) {
            s.calls.store(0, std::memory_order_relaxed);
            s.shifted.store(0, std::memory_order_relaxed);
            return &s;
        }
        if (t == want) return &s;   // another thread claimed it for this object
    }
    return nullptr;
}

// The MGMC post-hook's work (job threads), after the original returned.
void mgmcAfterSubmit(uint64_t obj) noexcept {
    g_mgmcCalls.fetch_add(1, std::memory_order_relaxed);
    uint64_t mask = 0;
    if (!guardedRead(obj + kOffMask, &mask, 8)) {
        g_mgmcUnread.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!mask) {   // the job skipped it: never tabled, never shifted
        g_mgmcMask0.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool live = g_live.load(std::memory_order_relaxed);
    const bool shiftOn = g_f51FpOn.load(std::memory_order_relaxed);
    if (!live && !shiftOn) return;
    uint8_t flag = 0;
    float pos[3] = {};
    if (!guardedRead(obj + kOffViewFlag, &flag, 1) || !guardedRead(obj + kOffPos, pos, sizeof(pos))) {
        g_mgmcUnread.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    bool shifted = false;
    Joints j;
    if (shiftOn && jointsRead(j) && nofoot::weaponIsLocalFp(mask, flag, pos, j.fp, j.fpOk)) {
        g_mgmcMatched.fetch_add(1, std::memory_order_relaxed);
        float d[3] = {}, copy[3] = {};
        if (fpRaiseRead(d) && !(d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f) && guardedRead(obj + kOffPosCopy, copy, sizeof(copy))) {
            float np[3] = {pos[0], pos[1], pos[2]};
            nofoot::weaponShift(np, copy, d);
            bool ok = true;
            for (uintptr_t i = 0; i < 3; ++i) {
                ok = guardedWriteFloat(obj + kOffPos + 4u * i, np[i]) && ok;
                ok = guardedWriteFloat(obj + kOffPosCopy + 4u * i, copy[i]) && ok;
            }
            shifted = ok;
        }
    }
    if (shifted) g_mgmcShifted.fetch_add(1, std::memory_order_relaxed);
    if (!live) return;
    MgmcSlot* s = slotFor(obj, g_gen.load(std::memory_order_relaxed));
    if (!s) {
        g_mgmcFull.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    s->seq.fetch_add(1, std::memory_order_acq_rel);
    s->flag = flag;
    s->mask = mask;
    std::memcpy(s->pos, pos, sizeof(pos));
    s->seq.fetch_add(1, std::memory_order_release);
    s->calls.fetch_add(1, std::memory_order_relaxed);
    if (shifted) s->shifted.fetch_add(1, std::memory_order_relaxed);
}

// The 0x4C837B0 callers, by return address (image RVA; 0xFFFFFFFF outside the image). The nearest record's details are written under a try-flag.
constexpr uint32_t kCallers = 32;
struct CallerSlot {
    std::atomic<uint32_t> rva{0};   // 0 = free
    std::atomic<uint64_t> calls{0}, records{0};
    std::atomic<uint32_t> bestMm{kNoBest};
    std::atomic<bool> busy{false};
    uint32_t detMm = kNoBest, detIdx = 0, detCount = 0;   // under busy
    uint64_t detRec = 0;
    float detPos[3] = {};
};
CallerSlot g_callers[kCallers];
std::atomic<uint64_t> g_recCalls{0}, g_amcCalls{0}, g_callerFull{0};
CallerSlot* callerFor(uint32_t rva) noexcept {
    for (uint32_t i = 0; i < kCallers; ++i) {
        CallerSlot& c = g_callers[(rva * 2654435761u + i) % kCallers];
        uint32_t r = c.rva.load(std::memory_order_acquire);
        if (r == rva) return &c;
        if (r != 0) continue;
        if (c.rva.compare_exchange_strong(r, rva, std::memory_order_acq_rel) || r == rva) return &c;
    }
    return nullptr;
}
void recSubmitNote(uintptr_t ret, uint64_t recs, uint64_t stride, uint64_t count) noexcept {
    g_recCalls.fetch_add(1, std::memory_order_relaxed);
    const uintptr_t base = g_base.load(std::memory_order_relaxed);
    const uint32_t rva = (base && ret >= base && ret - base < 0x7FFFFFFFu) ? static_cast<uint32_t>(ret - base) : 0xFFFFFFFFu;
    if (rva == kRetAmc) {
        g_amcCalls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!g_live.load(std::memory_order_relaxed)) return;
    CallerSlot* c = callerFor(rva ? rva : 0xFFFFFFFFu);
    if (!c) {
        g_callerFull.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t n32 = static_cast<uint32_t>(count);
    c->calls.fetch_add(1, std::memory_order_relaxed);
    c->records.fetch_add(n32, std::memory_order_relaxed);
    Joints j;
    if (!recs || stride < 0x20 || stride > 0x4000 || !jointsRead(j) || !j.fpOk) return;
    const uint32_t n = std::min(n32, kRecScanCap);
    float best = 1e30f, bestPos[3] = {};
    uint32_t bestIdx = 0;
    for (uint32_t k = 0; k < n; ++k) {
        float p[3] = {};
        if (!guardedRead(static_cast<uintptr_t>(recs + k * stride + kRecPosInRecord), p, sizeof(p))) break;
        const float dk = std::min(nofoot::dist3f(p, j.fp[0]), nofoot::dist3f(p, j.fp[1]));
        if (dk < best) {   // a NaN never wins
            best = dk;
            bestIdx = k;
            std::memcpy(bestPos, p, sizeof(p));
        }
    }
    if (!(best < 1e6f)) return;
    const uint32_t mm = static_cast<uint32_t>(best * 1000.0f);
    uint32_t cur = c->bestMm.load(std::memory_order_relaxed);
    bool won = false;
    while (mm < cur) {
        if (c->bestMm.compare_exchange_weak(cur, mm, std::memory_order_relaxed)) {
            won = true;
            break;
        }
    }
    if (!won || c->busy.exchange(true, std::memory_order_acquire)) return;
    c->detMm = mm;
    c->detIdx = bestIdx;
    c->detCount = n32;
    c->detRec = recs + bestIdx * stride;
    std::memcpy(c->detPos, bestPos, sizeof(bestPos));
    c->busy.store(false, std::memory_order_release);
}

uint64_t __fastcall mgmcSubmitHooked(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    const F35Fn original = reinterpret_cast<F35Fn>(g_mgmcHook.original);
    if (!original) return 0;
    const uint64_t r = original(a1, a2, a3, a4, s5, s6, s7, s8);   // the original first, with its arguments; its return value forwarded unchanged
    if (a1) mgmcAfterSubmit(a1);
    return r;
}
// The pre-hook: reached by the relay's jmp, so _ReturnAddress is the game's caller. Ten arguments per the RE (r9 = records, [+0x28] stride,
// [+0x30] count, ... [+0x50]); twelve are forwarded (the two extra are read from the caller's frame and never used by the callee).
using F54Fn12 = uint64_t(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                      uint64_t);
uint64_t __fastcall recSubmitHooked(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8,
                                    uint64_t s9, uint64_t s10, uint64_t s11, uint64_t s12) {
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const F54Fn12 original = reinterpret_cast<F54Fn12>(g_recHook.original);
    if (!original) return 0;
    recSubmitNote(ret, a4, s5, s6);
    return original(a1, a2, a3, a4, s5, s6, s7, s8, s9, s10, s11, s12);
}

// Frame thread, once (VR): both prologues verified against the loaded image; a mismatch refuses that hook with a line, the game untouched.
void installOnce() noexcept {
    if (g_tried) return;
    g_tried = true;
    if (!runtimeVrProfile()) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const char* peWhy = nullptr;
    if (!base || !checkIdentity(base, &peWhy)) {
        Log::get().note("native on foot weapon (F54): NOT installed -- identity mismatch (%s); the game was not touched.", peWhy ? peWhy : "no module");
        return;
    }
    g_base.store(base, std::memory_order_relaxed);
    size_t stolen = 0;
    std::string why;
    const F9ProbeSpec mspec{"native-on-foot-weapon-mgmc", kMgmcRva, kMgmcPrologue, sizeof(kMgmcPrologue), kMgmcStolen,
                            reinterpret_cast<void*>(&mgmcSubmitHooked), "F54 MGMC submit"};
    if (installF9ProbeInto(g_mgmcHook, mspec, base, &stolen, &why)) {
        g_armedMgmc = true;
        Log::get().note("native on foot weapon (F54): MGMC submit post-hook installed at EliteDangerous64.exe+0x43ED820, stolen=%zu bytes, prologue %zu "
                        "bytes verified (measures every MGMC with a non-zero mask; shifts the local FP weapon only with "
                        "fix.native_on_foot_body_raise_fp on, in body mode)", stolen, sizeof(kMgmcPrologue));
    } else {
        Log::get().note("native on foot weapon (F54): MGMC submit post-hook NOT installed at EliteDangerous64.exe+0x43ED820 -- %s. The weapon is "
                        "neither measured nor shifted; the game was not touched.", why.c_str());
    }
    why.clear();
    stolen = 0;
    const F9ProbeSpec rspec{"native-on-foot-weapon-records", kRecRva, kRecPrologue, sizeof(kRecPrologue), kRecStolen,
                            reinterpret_cast<void*>(&recSubmitHooked), "F54 record submit"};
    if (installF9ProbeInto(g_recHook, rspec, base, &stolen, &why)) {
        g_armedRec = true;
        Log::get().note("native on foot weapon (F54): record submit pre-hook installed at EliteDangerous64.exe+0x4C837B0, stolen=%zu bytes, prologue %zu "
                        "bytes verified (read only: its callers by return address, the AMC's +0x3DA2242 excluded)", stolen, sizeof(kRecPrologue));
    } else {
        Log::get().note("native on foot weapon (F54): record submit pre-hook NOT installed at EliteDangerous64.exe+0x4C837B0 -- %s. The game was not "
                        "touched.", why.c_str());
    }
}

struct MgmcRow {
    uint64_t obj = 0, mask = 0;
    uint8_t flag = 0;
    float pos[3] = {};
    uint32_t calls = 0, shifted = 0;
    float key = 0.0f;   // the nearest joint distance (sort key)
};
uint64_t g_lineMs = 0;   // frame thread only

// F56 probe 5 (frame thread, once a second with the F54 lines while the arm IK key is not off): the FP R handAttach (joint 51, the arm IK's
// snapshot) in world through the FP root's yaw only (as F45/F55) and through the whole root matrix (as G), their difference, and the nearest MGMC
// record to each; then that record's instance matrix ([obj+0x1C0] + 0x10, four rows) against the FP attach's world rotation. Neither within about
// 3 cm of the weapon: the target is wrong. Reads under SEH only.
void ikMappingLine(const MgmcRow* rows, uint32_t nRows, const FpRootSnap& fp, bool fpFresh) noexcept {
    static uint64_t lastMs = 0;   // every 5 s, with the arm IK's other probe lines (frame thread only)
    const uint64_t nowMs = GetTickCount64();
    if (lastMs && nowMs - lastMs < 5000) return;
    lastMs = nowMs;
    float m[4][16];
    uint64_t ms = 0;
    if (!ikFpRead(m, &ms) || !fpFresh) {
        Log::get().note("native on foot arm IK (F56, probe 5): no mapping check: %s", !fpFresh ? "the FP root is not fresh" : "no FP snapshot");
        return;
    }
    const float* a = m[0];   // 51, R handAttach, FP model space
    float full[3], yaw[3];
    for (int c = 0; c < 3; ++c) full[c] = a[12] * fp.rows[c] + a[13] * fp.rows[3 + c] + a[14] * fp.rows[6 + c] + fp.origin[c];
    const double rn = std::sqrt(static_cast<double>(fp.rows[6]) * fp.rows[6] + static_cast<double>(fp.rows[8]) * fp.rows[8]);
    const double cy = rn > 1e-6 ? fp.rows[8] / rn : 1.0, sy = rn > 1e-6 ? fp.rows[6] / rn : 0.0;
    yaw[0] = static_cast<float>(fp.origin[0] + a[12] * cy + a[14] * sy);
    yaw[1] = static_cast<float>(fp.origin[1] + a[13]);
    yaw[2] = static_cast<float>(fp.origin[2] - a[12] * sy + a[14] * cy);
    const float diffCm = nofoot::dist3f(full, yaw) * 100.0f;
    int bestFull = -1, bestYaw = -1;
    float dFull = 1e30f, dYaw = 1e30f;
    for (uint32_t i = 0; i < nRows; ++i) {
        const float df = nofoot::dist3f(rows[i].pos, full), dy = nofoot::dist3f(rows[i].pos, yaw);
        if (df < dFull) dFull = df, bestFull = static_cast<int>(i);
        if (dy < dYaw) dYaw = dy, bestYaw = static_cast<int>(i);
    }
    if (bestFull < 0) {
        Log::get().note("native on foot arm IK (F56, probe 5): R51 world full (%.3f, %.3f, %.3f), yaw-only (%.3f, %.3f, %.3f), %.1f cm apart; no MGMC "
                        "record with a mask this second", full[0], full[1], full[2], yaw[0], yaw[1], yaw[2], diffCm);
        return;
    }
    const MgmcRow& r = rows[bestFull];
    // The FP attach's world rotation (A's rows x the root's rows), and the record's instance matrix.
    float aw[16] = {};
    for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 3; ++c) aw[i * 4 + c] = a[i * 4] * fp.rows[c] + a[i * 4 + 1] * fp.rows[3 + c] + a[i * 4 + 2] * fp.rows[6 + c];
    uint64_t inst = 0;
    float wm[16] = {};
    const bool instOk = guardedRead(static_cast<uintptr_t>(r.obj) + 0x1C0u, &inst, 8) && inst && guardedRead(static_cast<uintptr_t>(inst) + 0x10u, wm, sizeof(wm));
    char tail[400] = "the instance matrix was not readable";
    if (instOk)
        std::snprintf(tail, sizeof(tail), "instance %016llX rows (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f) pos (%.3f, %.3f, %.3f); its rotation vs "
                      "the FP attach in world %.1f deg", static_cast<unsigned long long>(inst), wm[0], wm[1], wm[2], wm[4], wm[5], wm[6], wm[8], wm[9],
                      wm[10], wm[12], wm[13], wm[14], nofoot::ikRotAngleDeg(wm, aw));
    Log::get().note("native on foot arm IK (F56, probe 5): R51 world full root (%.3f, %.3f, %.3f), yaw-only (%.3f, %.3f, %.3f), %.1f cm apart; nearest "
                    "MGMC to full: %016llX at %.1f cm (byte 0x%02X); nearest to yaw-only: %016llX at %.1f cm; %s",
                    full[0], full[1], full[2], yaw[0], yaw[1], yaw[2], diffCm, static_cast<unsigned long long>(r.obj), dFull * 100.0f,
                    static_cast<unsigned>(r.flag), static_cast<unsigned long long>(rows[bestYaw].obj), dYaw * 100.0f, tail);
}

// Frame thread, every frame: the install, the joints, and once a second the lines (while native stereo is on foot).
void tick(uint64_t now) noexcept {
    installOnce();
    if (!g_armedMgmc && !g_armedRec) return;
    const bool live = nativeOnFootActive();
    g_live.store(live, std::memory_order_relaxed);
    EyeSample s;
    FpRootSnap fp{};
    {
        std::lock_guard<std::mutex> lock(g_eyeMutex);
        s = g_eyeSample;
        fp = g_fpRoot;
    }
    const bool sFresh = s.valid && now - s.stamp < 100;
    const bool fpFresh = fp.ok && now - fp.stamp < 100;
    Joints j;
    j.fpOk = live && sFresh && s.extOkFp && fpFresh;
    j.tpOk = live && sFresh && s.extOk3P;
    if (j.fpOk) {
        const double rn = std::sqrt(static_cast<double>(fp.rows[6]) * fp.rows[6] + static_cast<double>(fp.rows[8]) * fp.rows[8]);
        const double cy = rn > 1e-6 ? fp.rows[8] / rn : 1.0, sy = rn > 1e-6 ? fp.rows[6] / rn : 0.0;
        for (size_t side = 0; side < 2; ++side) {
            const float* m = &s.extFpM[3 * (2 + side)];
            j.fp[side][0] = static_cast<float>(static_cast<double>(fp.origin[0]) + m[0] * cy + m[2] * sy);
            j.fp[side][1] = static_cast<float>(static_cast<double>(fp.origin[1]) + m[1]);
            j.fp[side][2] = static_cast<float>(static_cast<double>(fp.origin[2]) - m[0] * sy + m[2] * cy);
        }
    }
    if (j.tpOk)
        for (size_t side = 0; side < 2; ++side)
            for (size_t k = 0; k < 3; ++k) j.tp[side][k] = s.ext3PW[3 * (2 + side) + k];
    jointsPublish(j);
    if (now - g_lineMs < 1000) return;
    g_lineMs = now;
    // The second's boundary: a new generation (never 0 in the tag's 16 bits); the previous one's slots are read below.
    const uint32_t prevGen = g_gen.load(std::memory_order_relaxed);
    uint32_t nextGen = prevGen + 1;
    if ((nextGen & 0xFFFFu) == 0) ++nextGen;
    g_gen.store(nextGen, std::memory_order_relaxed);
    const uint64_t calls = g_mgmcCalls.exchange(0, std::memory_order_relaxed), mask0 = g_mgmcMask0.exchange(0, std::memory_order_relaxed),
                   unread = g_mgmcUnread.exchange(0, std::memory_order_relaxed), full = g_mgmcFull.exchange(0, std::memory_order_relaxed),
                   shifted = g_mgmcShifted.exchange(0, std::memory_order_relaxed), matched = g_mgmcMatched.exchange(0, std::memory_order_relaxed);
    const uint64_t recCalls = g_recCalls.exchange(0, std::memory_order_relaxed), amcCalls = g_amcCalls.exchange(0, std::memory_order_relaxed),
                   callerFull = g_callerFull.exchange(0, std::memory_order_relaxed);
    MgmcRow rows[kSlots];
    uint32_t nRows = 0;
    for (uint32_t i = 0; i < kSlots; ++i) {
        MgmcSlot& sl = g_slots[i];
        const uint64_t t1 = sl.tag.load(std::memory_order_acquire);
        if (!t1 || (t1 >> 48) != (prevGen & 0xFFFFu)) continue;
        const uint32_t q1 = sl.seq.load(std::memory_order_acquire);
        if (q1 & 1u) continue;
        MgmcRow r;
        r.obj = t1 & 0x0000FFFFFFFFFFFFull;
        r.flag = sl.flag;
        r.mask = sl.mask;
        std::memcpy(r.pos, sl.pos, sizeof(r.pos));
        r.calls = sl.calls.load(std::memory_order_relaxed);
        r.shifted = sl.shifted.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (sl.seq.load(std::memory_order_relaxed) != q1 || sl.tag.load(std::memory_order_relaxed) != t1) continue;
        float key = 1e30f;
        if (j.fpOk) key = std::min(key, std::min(nofoot::dist3f(r.pos, j.fp[0]), nofoot::dist3f(r.pos, j.fp[1])));
        if (j.tpOk) key = std::min(key, std::min(nofoot::dist3f(r.pos, j.tp[0]), nofoot::dist3f(r.pos, j.tp[1])));
        r.key = key == key ? key : 1e30f;
        rows[nRows++] = r;
    }
    struct CallerRow {
        uint32_t rva = 0, detMm = kNoBest, detIdx = 0, detCount = 0;
        uint64_t calls = 0, records = 0, detRec = 0;
        float detPos[3] = {};
        bool detBusy = false;
    };
    CallerRow crow[kCallers];
    uint32_t nCallers = 0;
    for (uint32_t i = 0; i < kCallers; ++i) {
        CallerSlot& c = g_callers[i];
        const uint32_t rva = c.rva.load(std::memory_order_acquire);
        if (!rva) continue;
        CallerRow cr;
        cr.rva = rva;
        cr.calls = c.calls.exchange(0, std::memory_order_relaxed);
        cr.records = c.records.exchange(0, std::memory_order_relaxed);
        c.bestMm.store(kNoBest, std::memory_order_relaxed);
        if (!c.busy.exchange(true, std::memory_order_acquire)) {
            cr.detMm = c.detMm;
            cr.detIdx = c.detIdx;
            cr.detCount = c.detCount;
            cr.detRec = c.detRec;
            std::memcpy(cr.detPos, c.detPos, sizeof(cr.detPos));
            c.detMm = kNoBest;
            c.busy.store(false, std::memory_order_release);
        } else {
            cr.detBusy = true;
        }
        if (cr.calls) crow[nCallers++] = cr;
    }
    if (!live) return;
    float d[3] = {};
    const bool dOk = fpRaiseRead(d);
    Log::get().note("native on foot weapon (F54): MGMC submits %llu this second (%llu mask 0, %llu unreadable), %u tabled with mask != 0 (%llu "
                    "calls lost: table full); local FP weapon: %llu calls matched, %llu shifted (raise %s, d (%.3f, %.3f, %.3f) m); handAttach (world) "
                    "FP R51 (%.3f, %.3f, %.3f) L52 (%.3f, %.3f, %.3f) %s; 3P R66 (%.3f, %.3f, %.3f) L67 (%.3f, %.3f, %.3f) %s",
                    static_cast<unsigned long long>(calls), static_cast<unsigned long long>(mask0), static_cast<unsigned long long>(unread), nRows,
                    static_cast<unsigned long long>(full), static_cast<unsigned long long>(matched), static_cast<unsigned long long>(shifted),
                    g_f51FpOn.load(std::memory_order_relaxed) ? "on" : "off", dOk ? d[0] : 0.0f, dOk ? d[1] : 0.0f, dOk ? d[2] : 0.0f,
                    j.fp[1][0], j.fp[1][1], j.fp[1][2], j.fp[0][0], j.fp[0][1], j.fp[0][2], j.fpOk ? "fresh" : "not known",
                    j.tp[1][0], j.tp[1][1], j.tp[1][2], j.tp[0][0], j.tp[0][1], j.tp[0][2], j.tpOk ? "fresh" : "not known");
    std::sort(rows, rows + nRows, [](const MgmcRow& x, const MgmcRow& y) { return x.key < y.key; });
    if (pfz::g_ikWanted.load(std::memory_order_relaxed)) ikMappingLine(rows, nRows, fp, fpFresh);   // F56 probe 5
    constexpr uint32_t kRowLines = 8;
    for (uint32_t i = 0; i < nRows && i < kRowLines; ++i) {
        const MgmcRow& r = rows[i];
        const auto dj = [&](const float jp[3], bool ok) { return ok ? static_cast<double>(nofoot::dist3f(r.pos, jp)) : -1.0; };
        Log::get().note("native on foot weapon (F54): MGMC %016llX byte+0x1C8 0x%02X mask %016llX pos (%.3f, %.3f, %.3f); distance FP R51 %.3f L52 "
                        "%.3f, 3P R66 %.3f L67 %.3f m (-1 not known); %u calls, %u shifted%s",
                        static_cast<unsigned long long>(r.obj), static_cast<unsigned>(r.flag), static_cast<unsigned long long>(r.mask),
                        static_cast<double>(r.pos[0]), static_cast<double>(r.pos[1]), static_cast<double>(r.pos[2]), dj(j.fp[1], j.fpOk),
                        dj(j.fp[0], j.fpOk), dj(j.tp[1], j.tpOk), dj(j.tp[0], j.tpOk), r.calls, r.shifted,
                        r.flag && j.fpOk && r.key <= nofoot::kWeaponMatchM ? " <- within 0.3 m of a hand joint with the byte set" : "");
    }
    if (nRows > kRowLines)
        Log::get().note("native on foot weapon (F54): %u more MGMCs with mask != 0 not listed (farther from the hands)", nRows - kRowLines);
    Log::get().note("native on foot weapon (F54): 0x4C837B0 calls %llu this second: AMC (+0x3DA2242, excluded) %llu, %u other callers%s",
                    static_cast<unsigned long long>(recCalls), static_cast<unsigned long long>(amcCalls), nCallers,
                    callerFull ? " (some calls lost: the caller table is full)" : "");
    for (uint32_t i = 0; i < nCallers; ++i) {
        const CallerRow& c = crow[i];
        if (c.detMm != kNoBest)
            Log::get().note("native on foot weapon (F54): caller +0x%X (%s): %llu calls, %llu records; nearest the FP hand: record %u of %u at %016llX, "
                            "pos (%.3f, %.3f, %.3f), %.3f m",
                            c.rva, knownName(c.rva), static_cast<unsigned long long>(c.calls), static_cast<unsigned long long>(c.records), c.detIdx,
                            c.detCount, static_cast<unsigned long long>(c.detRec), static_cast<double>(c.detPos[0]), static_cast<double>(c.detPos[1]),
                            static_cast<double>(c.detPos[2]), c.detMm / 1000.0);
        else
            Log::get().note("native on foot weapon (F54): caller +0x%X (%s): %llu calls, %llu records; nearest the FP hand: %s", c.rva, knownName(c.rva),
                            static_cast<unsigned long long>(c.calls), static_cast<unsigned long long>(c.records),
                            c.detBusy ? "not read (busy)" : j.fpOk ? "none (no readable record or stride)" : "not measured (FP joints not known)");
    }
}
}  // namespace f54

// ---- F49 (TEMPORARY): the pose freeze: the frAnim Animate post-hook---------------------------------------------------------------
// The Animate handler 0x741520 (vtable 0x510E370 slot +8; rcx unused, rdx = the job; job[0] = the evaluator X) runs on frAnim worker threads.
// After the original returns, for the local avatar's X only (pfz::g_localX, the census's plan): the output span ([job+0x28] when [X+0x210] is 0,
// else [[X+0x1F0]+0xA0] when count-1 < [[X+0x1F0]+0xE0]; 0x43FB740 is never called), the frozen set's rest chain anchored at the hips' animated
// matrix, the probe's statistics and the span snapshot; with the key on and the frame thread's weight above 0, the frozen set's matrices are
// written (blended toward the rest chain). Inside the hook: no allocation, no logging, no locks (one try-flag that skips, never waits), every read
// and write under SEH. The frame thread names each failure once and prints the statistics every 5 s.
constexpr uintptr_t kPfzAnimateRva = 0x741520;
const uint8_t kPfzAnimatePrologue[15] = {0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0xD2, 0x48, 0x83, 0xC2, 0x08, 0x49, 0x8B, 0x42, 0x38};
constexpr size_t kPfzAnimateStolen = 7;   // sub rsp,0x38; mov r10,rdx (no RIP-relative operand)
F9Probe g_pfzHook;
bool g_pfzTried = false, g_pfzArmed = false;   // frame thread only
std::atomic<float> g_pfzWeight{0.0f};           // the frame thread's blend weight (0 unless the key is on and the gate holds)
std::atomic<bool> g_pfzWriteDown{false};        // a failed write stands the writes down for the session
std::atomic<uint32_t> g_pfzFailBits{0};         // failures the hook met; the frame thread names each once
std::atomic<uint64_t> g_pfzCalls{0}, g_pfzLocalCalls{0}, g_pfzBusySkips{0};
std::atomic<bool> g_pfzBusy{false};             // the hook's scratch is in use (a second local call skips; never waits)
std::atomic<float> g_pfzHeadUp{0.0f};           // the local avatar's animated head (joint 44) height, model space, before any write
std::atomic<uint64_t> g_pfzHeadMs{0};
enum PfzFail : uint32_t {
    kPfzFailJob = 1u << 0, kPfzFailPlanTorn = 1u << 1, kPfzFailPlan = 1u << 2, kPfzFailFlags = 1u << 3, kPfzFailPose = 1u << 4,
    kPfzFailCount = 1u << 5, kPfzFailCap = 1u << 6, kPfzFailBuffer = 1u << 7, kPfzFailRead = 1u << 8, kPfzFailInverse = 1u << 9,
    kPfzFailWrite = 1u << 10, kPfzFailKinds = 11
};
const char* const kPfzFailText[kPfzFailKinds] = {
    "job[0] was not readable",
    "the plan was being rewritten (skipped this call)",
    "the plan does not belong to this X or is empty",
    "a flag of X (+0x210, +0x212, +0x12C, +0x128) was not readable",
    "P = [X+8] or its joint count was not readable",
    "the joint count differs from the plan's (the skeleton changed)",
    "the joint count is past 0xC0",
    "no output span: [job+0x28] or [X+0x1F0] is null, or count-1 is not below [Y+0xE0]",
    "the output span was not readable",
    "an animated parent matrix is singular (no blend this call)",
    "a frozen matrix write faulted: the writes stand down for the session"};
void pfzFail(uint32_t bit) noexcept { g_pfzFailBits.fetch_or(bit, std::memory_order_relaxed); }

// The hook's statistics (one writer at a time: g_pfzBusy), read by the frame thread under the sequence counter; reset on request.
struct PfzStats {
    uint64_t calls = 0;
    uint32_t tid = 0, tidChanges = 0;
    uint8_t f210 = 0, f212 = 0, f12C = 0;
    uint32_t f128 = 0, count = 0;
    uint64_t perBuffer[2] = {};
    uint32_t n[2] = {};                   // joints 34 and 44: samples
    double sumMm[2] = {}, minMm[2] = {}, maxMm[2] = {}, sumV[2][3] = {};
    uint64_t written = 0;
    float wLast = 0.0f;
};
std::atomic<uint32_t> g_pfzStatSeq{0};
PfzStats g_pfzStats;
std::atomic<bool> g_pfzStatReset{false};
// The hook's scratch (under g_pfzBusy).
pfz::Plan g_pfzPlanCopy;
alignas(16) float g_pfzAnim[nofoot::kPfzMaxJoints][16];
alignas(16) float g_pfzChain[nofoot::kPfzMaxF][16];

// F56: the shared part of the local X's post-hook (the pose freeze and the arm IK use one path): X's flags, the joint count, the output span
// ([job+0x28] when [X+0x210] is 0, else [[X+0x1F0]+0xA0] when count-1 < [[X+0x1F0]+0xE0]) and its PRE-write animated copy in g_pfzAnim.
struct PfzSpan {
    uint64_t buf = 0;
    uint32_t which = 0, count = 0;
    uint8_t f210 = 0, f212 = 0, f12C = 0;
    uint32_t f128 = 0;
};
bool pfzSpanRead(uint64_t x, uint64_t job, PfzSpan* sp) noexcept {
    if (!guardedRead(x + 0x210, &sp->f210, 1) || !guardedRead(x + 0x212, &sp->f212, 1) || !guardedRead(x + 0x12C, &sp->f12C, 1) ||
        !guardedRead(x + 0x128, &sp->f128, 4)) {
        pfzFail(kPfzFailFlags);
        return false;
    }
    uint64_t pose = 0;
    uint16_t count = 0;
    if (!guardedRead(x + 8, &pose, 8) || !pose || !guardedRead(pose, &count, 2) || count == 0) {
        pfzFail(kPfzFailPose);
        return false;
    }
    if (count > nofoot::kPfzMaxJoints) {
        pfzFail(kPfzFailCap);
        return false;
    }
    sp->count = count;
    uint64_t buf = 0;
    if (sp->f210 == 0) {
        if (!guardedRead(job + 0x28, &buf, 8)) buf = 0;
    } else {
        sp->which = 1;
        uint64_t y = 0;
        uint32_t cap = 0;
        if (guardedRead(x + 0x1F0, &y, 8) && y && guardedRead(y + 0xE0, &cap, 4) && static_cast<uint32_t>(count) - 1u < cap) {
            if (!guardedRead(y + 0xA0, &buf, 8)) buf = 0;
        }
    }
    if (!buf) {
        pfzFail(kPfzFailBuffer);
        return false;
    }
    if (!guardedRead(buf, g_pfzAnim, static_cast<size_t>(count) * 64u)) {
        pfzFail(kPfzFailRead);
        return false;
    }
    sp->buf = buf;
    return true;
}

// The pose freeze on the shared span. True when it wrote (g_pfzChain then holds the written matrices, g_pfzPlanCopy the plan); its refusals
// return false and never skip the arm IK that follows.
bool pfzFreezeStep(uint64_t x, const PfzSpan& sp) noexcept {
    bool planOk = false;
    for (int tries = 0; tries < 2 && !planOk; ++tries) {
        const uint32_t s1 = pfz::g_planSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        std::memcpy(&g_pfzPlanCopy, &pfz::g_plan, sizeof(g_pfzPlanCopy));
        std::atomic_thread_fence(std::memory_order_acquire);
        planOk = pfz::g_planSeq.load(std::memory_order_relaxed) == s1;
    }
    if (!planOk) return pfzFail(kPfzFailPlanTorn), false;
    const pfz::Plan& pl = g_pfzPlanCopy;
    if (pl.x != x || pl.count == 0 || pl.count > nofoot::kPfzMaxF) return pfzFail(kPfzFailPlan), false;
    const uint8_t f210 = sp.f210, f212 = sp.f212, f12C = sp.f12C;
    const uint32_t f128 = sp.f128, count = sp.count, which = sp.which;
    const uint64_t buf = sp.buf;
    if (count != pl.joints || pl.anchor >= count) return pfzFail(kPfzFailCount), false;
    // The rest chain (w = 1), anchored at the hips' animated matrix.
    const float* anchorM = g_pfzAnim[pl.anchor];
    for (uint32_t k = 0; k < pl.count; ++k) {
        float local[16];
        nofoot::pfzLocalMatrix(pl.rest[k], local);
        const float* parentM = pl.parentSlot[k] < 0 ? anchorM : g_pfzChain[pl.parentSlot[k]];
        nofoot::pfzMul(local, parentM, g_pfzChain[k]);
    }
    const uint32_t tid = GetCurrentThreadId();
    const uint64_t nowMs = GetTickCount64();
    if (count > 44) {
        g_pfzHeadUp.store(g_pfzAnim[44][13], std::memory_order_relaxed);
        g_pfzHeadMs.store(nowMs, std::memory_order_relaxed);
    }
    // The span snapshot of joint 34 (as animated); the written copy follows below when the freeze writes.
    pfz::g_spanSeq.fetch_add(1, std::memory_order_acq_rel);
    if (count > 34) std::memcpy(pfz::g_span34, g_pfzAnim[34], sizeof(pfz::g_span34));
    pfz::g_span34WasWritten = false;
    pfz::g_span34Ms = count > 34 ? nowMs : 0;
    pfz::g_span34Buffer = which;
    pfz::g_spanSeq.fetch_add(1, std::memory_order_release);
    // The probe: joints 34 and 44, animated minus the rest chain (both relative to the anchor's animated matrix, so the anchor cancels), in mm.
    g_pfzStatSeq.fetch_add(1, std::memory_order_acq_rel);
    if (g_pfzStatReset.exchange(false, std::memory_order_relaxed)) g_pfzStats = PfzStats{};
    PfzStats& st = g_pfzStats;
    ++st.calls;
    if (st.tid != 0 && st.tid != tid) ++st.tidChanges;
    st.tid = tid;
    st.f210 = f210;
    st.f212 = f212;
    st.f12C = f12C;
    st.f128 = f128;
    st.count = count;
    ++st.perBuffer[which];
    const int32_t slots[2] = {pl.slot34, pl.slot44};
    const uint32_t ids[2] = {34, 44};
    for (int i = 0; i < 2; ++i) {
        if (slots[i] < 0 || ids[i] >= count) continue;
        const float* a = g_pfzAnim[ids[i]];
        const float* r = g_pfzChain[slots[i]];
        const double d[3] = {(static_cast<double>(a[12]) - r[12]) * 1000.0, (static_cast<double>(a[13]) - r[13]) * 1000.0,
                             (static_cast<double>(a[14]) - r[14]) * 1000.0};
        const double mm = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!std::isfinite(mm)) continue;
        if (st.n[i] == 0 || mm < st.minMm[i]) st.minMm[i] = mm;
        if (st.n[i] == 0 || mm > st.maxMm[i]) st.maxMm[i] = mm;
        st.sumMm[i] += mm;
        for (int c = 0; c < 3; ++c) st.sumV[i][c] += d[c];
        ++st.n[i];
    }
    g_pfzStatSeq.fetch_add(1, std::memory_order_release);
    // ON: the frozen set, blended toward the rest chain by the frame thread's weight. Every matrix is computed before the first write.
    const float w = g_pfzWeight.load(std::memory_order_relaxed);
    if (pfz::g_mode.load(std::memory_order_relaxed) != 2 || !(w > 0.0f) || g_pfzWriteDown.load(std::memory_order_relaxed)) return false;
    if (w < 1.0f) {
        for (uint32_t k = 0; k < pl.count; ++k) {
            const float* parentAnim = pl.parentSlot[k] < 0 ? anchorM : g_pfzAnim[pl.member[pl.parentSlot[k]]];
            float inv[16], animLocal[16], rec[8], local[16];
            if (!nofoot::pfzAffineInverse(parentAnim, inv)) return pfzFail(kPfzFailInverse), false;
            nofoot::pfzMul(g_pfzAnim[pl.member[k]], inv, animLocal);
            nofoot::pfzBlendRecord(animLocal, pl.rest[k], w, rec);
            nofoot::pfzLocalMatrix(rec, local);
            const float* parentNew = pl.parentSlot[k] < 0 ? anchorM : g_pfzChain[pl.parentSlot[k]];
            nofoot::pfzMul(local, parentNew, g_pfzChain[k]);   // parent first: the parent's slot already holds its blended matrix
        }
    }
    for (uint32_t k = 0; k < pl.count; ++k) {
        if (!guardedWriteFloats16(buf + static_cast<uint64_t>(pl.member[k]) * 64u, g_pfzChain[k])) {
            g_pfzWriteDown.store(true, std::memory_order_relaxed);
            return pfzFail(kPfzFailWrite), false;
        }
    }
    pfz::g_spanSeq.fetch_add(1, std::memory_order_acq_rel);
    if (pl.slot34 >= 0) {
        std::memcpy(pfz::g_span34Written, g_pfzChain[pl.slot34], sizeof(pfz::g_span34Written));
        pfz::g_span34WasWritten = true;
    }
    pfz::g_spanSeq.fetch_add(1, std::memory_order_release);
    g_pfzStatSeq.fetch_add(1, std::memory_order_acq_rel);
    ++g_pfzStats.written;
    g_pfzStats.wLast = w;
    g_pfzStatSeq.fetch_add(1, std::memory_order_release);
    return true;
}

// ---- F56 (TEMPORARY): the 3P arm IK, the hook's half -----------------------------------------------------------------------------
// After the pose freeze on the same span (frAnim worker, under g_pfzBusy), for the arm plan's X only. Per arm: M0 = the animated matrices carried
// by the clavicle's freeze delta D_C (identity when the freeze did not write); the target T_A = A_fp x G (the FP handAttach in the 3P model space),
// the hand target T_H = inv(L_A) x T_A with L_A = A x inv(H) from this span; the elbow on the circle of the two-bone solve, its swivel from the
// animated elbow toward 0.5 (animated elbow) + 0.5 (hint), clamped (elbow out 10 cm, not above the shoulder) and scaled by the weight; swing-only
// rotations of U about S and F about E; the hand rotation nlerp'd toward the target's; the forearm and upper-arm helpers rolled by t x the twist.
// probe solves at weight 1 and writes nothing; nudge writes the left hand group 25 cm up (model y) with gU and gF as animated; on writes the
// solve at the per-arm weight (the base weight times the distance gate). Every matrix is computed before the first write; the applied hand
// offset moves at most 5 cm a call. No allocation, logging or locks (the scratch is under g_pfzBusy); every read and write under SEH.
std::atomic<float> g_ikWeight{0.0f};        // the frame thread's base weight (0 unless the key is nudge or on and the gate holds)
std::atomic<bool> g_ikWriteDown{false};     // a failed write stands the IK's writes down for the session
std::atomic<uint32_t> g_ikFailBits{0};      // refusals the hook met; the frame thread names each once
enum IkRefusal : uint32_t {
    kIkRefBuffer = 0, kIkRefPlanTorn, kIkRefPlan, kIkRefFpTorn, kIkRefFpStale, kIkRefGTorn, kIkRefGStale, kIkRefSide, kIkRefSingular, kIkRefWrite,
    kIkRefKinds
};
const char* const kIkRefText[kIkRefKinds] = {
    "no output span (the shared span read failed: flags, pose, count or buffer)",
    "the arm plan was being rewritten (skipped this call)",
    "the arm plan does not belong to this X or its joint count differs (the skeleton changed)",
    "the FP snapshot was being written or never was (no FP X, or its Animate call has not run)",
    "the FP snapshot is older than 100 ms",
    "G was being written or never was (no local 3P AMC call, or the FP root is older than 100 ms there)",
    "G is older than 100 ms",
    "an arm's plan was refused by the census (its side is not solved)",
    "a singular or non-finite matrix (the side is skipped this call)",
    "an arm write faulted: the IK's writes stand down for the session"};
std::atomic<uint64_t> g_ikRefused[kIkRefKinds];
std::atomic<uint64_t> g_ikCalls{0};
void ikRefuse(uint32_t k) noexcept {
    g_ikRefused[k].fetch_add(1, std::memory_order_relaxed);
    g_ikFailBits.fetch_or(1u << k, std::memory_order_relaxed);
}
// The hook's statistics (one writer: g_pfzBusy), read by the frame thread under the sequence counter; reset on request.
struct IkSideStats {
    uint32_t n = 0, clamped = 0, errN = 0;
    double distSum = 0.0, distMin = 0.0, distMax = 0.0;      // |T_A - the animated 3P attach|, cm
    double angSum = 0.0, angMin = 0.0, angMax = 0.0;         // the rotation between them, degrees
    double reachSum = 0.0, reachMin = 0.0, reachMax = 0.0;   // |p_T - S| / (l1 + l2)
    double shortMax = 0.0;                                   // cm past reach
    double bendSum = 0.0;                                    // the animated elbow's interior angle, degrees
    double errMax = 0.0;                                     // the solved chain's hand position against its target, cm (reach < 1 only)
    double wSum = 0.0;                                       // the per-arm weight (on only)
    double gateSum = 0.0;                                    // the distance gate (every mode)
    float l1 = 0.0f, l2 = 0.0f, E[3] = {}, S[3] = {};
    uint64_t written = 0;
};
struct IkStats {
    uint64_t calls = 0, solved = 0;
    double fpAgeLast = 0.0, fpAgeMax = 0.0, gAgeLast = 0.0, gAgeMax = 0.0;
    IkSideStats side[2];
};
std::atomic<uint32_t> g_ikStatSeq{0};
IkStats g_ikStats;
std::atomic<bool> g_ikStatReset{false};
// The hook's scratch (under g_pfzBusy).
pfz::ArmPlan g_ikPlanCopy;
alignas(16) float g_ikM0[nofoot::kPfzMaxJoints][16];
alignas(16) float g_ikOut[nofoot::kPfzMaxJoints][16];
uint16_t g_ikList[nofoot::kPfzMaxJoints];
float g_ikApplied[2][3];       // the applied hand offset per side (model space, from the animated hand)
uint64_t g_ikAppliedMs[2];     // when it was last applied (older than 200 ms: start again from the animated hand)

bool ikMatrixSane(const float m[16]) noexcept {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1000.0f) return false;
    return true;
}
// The elbow's swivel for a frame built on the animated elbow (its swivel 0): the desired pole 0.5 unit(animated elbow - S) + 0.5 hint, hint =
// 0.8 down + 0.6 out + 0.1 back (model space: y up, z forward), the hint alone when the pole's part across dir is under 0.2; then clamped.
double ikSwivel(const nofoot::IkTwoBone& tb, const double elbowDir[3], double out, bool* clamped) noexcept {
    const double hint[3] = {0.6 * out, -0.8, -0.1};
    double pole[3], pp[3];
    for (int i = 0; i < 3; ++i) pole[i] = 0.5 * elbowDir[i] + 0.5 * hint[i];
    double pd = nofoot::ikDot(pole, tb.dir);
    for (int i = 0; i < 3; ++i) pp[i] = pole[i] - tb.dir[i] * pd;
    if (nofoot::ikLen(pp) < 0.2) {
        pd = nofoot::ikDot(hint, tb.dir);
        for (int i = 0; i < 3; ++i) pp[i] = hint[i] - tb.dir[i] * pd;
    }
    return nofoot::ikSwivelClamp(tb, tb.angleOf(pp), out, clamped);
}
inline void ikPos(const float m[16], double p[3]) noexcept {
    p[0] = m[12];
    p[1] = m[13];
    p[2] = m[14];
}

void armIkAfterAnimate(uint64_t x, const PfzSpan& sp, bool froze) noexcept {
    const int mode = pfz::g_ikMode.load(std::memory_order_relaxed);
    if (mode == 0) return;
    g_ikCalls.fetch_add(1, std::memory_order_relaxed);
    bool planOk = false;
    for (int tries = 0; tries < 2 && !planOk; ++tries) {
        const uint32_t s1 = pfz::g_armSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        std::memcpy(&g_ikPlanCopy, &pfz::g_armPlan, sizeof(g_ikPlanCopy));
        std::atomic_thread_fence(std::memory_order_acquire);
        planOk = pfz::g_armSeq.load(std::memory_order_relaxed) == s1;
    }
    if (!planOk) return ikRefuse(kIkRefPlanTorn);
    const pfz::ArmPlan& pl = g_ikPlanCopy;
    if (pl.x != x || pl.joints != sp.count) return ikRefuse(kIkRefPlan);
    float fpm[4][16];
    uint64_t fpMs = 0;
    if (!ikFpRead(fpm, &fpMs)) return ikRefuse(kIkRefFpTorn);
    const uint64_t now = GetTickCount64();
    const uint64_t fpAge = now > fpMs ? now - fpMs : 0;
    if (fpAge > nofoot::kIkFreshMs) return ikRefuse(kIkRefFpStale);
    IkGData G;
    if (!ikGRead(&G)) return ikRefuse(kIkRefGTorn);
    const uint64_t gAge = now > G.ms ? now - G.ms : 0;
    if (gAge > nofoot::kIkFreshMs) return ikRefuse(kIkRefGStale);
    const float wBase = g_ikWeight.load(std::memory_order_relaxed);
    const bool writeOk = (mode == 2 || mode == 3) && wBase > 0.0f && !g_ikWriteDown.load(std::memory_order_relaxed);
    struct SideOut {
        bool solved = false, clamped = false, wrote = false;
        double dist = 0.0, ang = 0.0, reach = 0.0, shortfall = 0.0, bend = 0.0, err = -1.0, wArm = 0.0, gate = 0.0;
        float l1 = 0.0f, l2 = 0.0f, E[3] = {}, S[3] = {};
    } so[2];
    uint32_t nList = 0;
    for (int side = 0; side < 2; ++side) {
        if (!pl.ok[side]) {
            ikRefuse(kIkRefSide);
            continue;
        }
        const nofoot::IkArm& arm = pl.side[side];
        const uint32_t C = arm.C, U = arm.U, F = arm.F, H = arm.H, A = arm.A;
        // D_C: the freeze's move of the clavicle (identity when it did not write it).
        float DC[16];
        bool dcIdentity = true;
        if (froze) {
            const pfz::Plan& fpl = g_pfzPlanCopy;
            for (uint32_t k = 0; k < fpl.count; ++k) {
                if (fpl.member[k] != C) continue;
                float inv[16];
                if (!nofoot::pfzAffineInverse(g_pfzAnim[C], inv)) break;
                nofoot::pfzMul(inv, g_pfzChain[k], DC);
                dcIdentity = false;
                break;
            }
        }
        const uint16_t* lists[3] = {arm.gU, arm.gF, arm.gH};
        const uint16_t counts[3] = {arm.nU, arm.nF, arm.nH};
        for (int g = 0; g < 3; ++g)
            for (uint32_t k = 0; k < counts[g]; ++k) {
                const uint32_t j = lists[g][k];
                if (dcIdentity) std::memcpy(g_ikM0[j], g_pfzAnim[j], 64);
                else nofoot::pfzMul(g_pfzAnim[j], DC, g_ikM0[j]);
            }
        // The targets: T_A = A_fp x G; T_H = inv(L_A) x T_A, L_A = M0[A] x inv(M0[H]).
        const float* fpA = side == 0 ? fpm[1] : fpm[0];   // L: 52, R: 51
        float TA[16], invH[16], LA[16], invLA[16], TH[16];
        nofoot::pfzMul(fpA, G.g, TA);
        if (!nofoot::pfzAffineInverse(g_ikM0[H], invH)) {
            ikRefuse(kIkRefSingular);
            continue;
        }
        nofoot::pfzMul(g_ikM0[A], invH, LA);
        if (!nofoot::pfzAffineInverse(LA, invLA)) {
            ikRefuse(kIkRefSingular);
            continue;
        }
        nofoot::pfzMul(invLA, TA, TH);
        if (!ikMatrixSane(TA) || !ikMatrixSane(TH) || !nofoot::ikOrthonormalise(TH)) {
            ikRefuse(kIkRefSingular);
            continue;
        }
        double S[3], F0[3], H0[3], pT[3], aA[3], tA[3];
        ikPos(g_ikM0[U], S);
        ikPos(g_ikM0[F], F0);
        ikPos(g_ikM0[H], H0);
        ikPos(TH, pT);
        ikPos(g_ikM0[A], aA);
        ikPos(TA, tA);
        const double uf[3] = {F0[0] - S[0], F0[1] - S[1], F0[2] - S[2]}, fh[3] = {H0[0] - F0[0], H0[1] - F0[1], H0[2] - F0[2]};
        const double l1 = nofoot::ikLen(uf), l2 = nofoot::ikLen(fh);
        const double outSign = g_pfzAnim[C][12] >= 0.0f ? 1.0 : -1.0;
        SideOut& o = so[side];
        const double dA[3] = {tA[0] - aA[0], tA[1] - aA[1], tA[2] - aA[2]};
        o.dist = nofoot::ikLen(dA) * 100.0;
        o.ang = nofoot::ikRotAngleDeg(TA, g_ikM0[A]);
        o.l1 = static_cast<float>(l1 * 100.0);
        o.l2 = static_cast<float>(l2 * 100.0);
        {
            const double fs[3] = {-uf[0], -uf[1], -uf[2]};
            const double c = l1 > 1e-9 && l2 > 1e-9 ? nofoot::ikDot(fs, fh) / (l1 * l2) : 1.0;
            o.bend = std::acos(c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c) * 180.0 / nofoot::kIkPi;   // 180: straight
        }
        const double dH[3] = {pT[0] - H0[0], pT[1] - H0[1], pT[2] - H0[2]};
        const float gate = nofoot::ikDistanceGate(static_cast<float>(nofoot::ikLen(dH)));
        // The weight this side solves at: on and writing, the per-arm weight; otherwise 1 (probe: the full solve, for the statistics only).
        const double wArm = static_cast<double>(wBase) * gate;
        const bool doSolveWrite = mode == 3 && writeOk && wArm > 0.0;
        o.wArm = mode == 3 ? wArm : 0.0;
        o.gate = gate;
        const double wSolve = doSolveWrite ? wArm : 1.0;
        // The hand target: the offset from the animated hand, capped at 5 cm a call while writing (from zero after 200 ms without a write).
        double target[3];
        if (doSolveWrite) {
            if (now - g_ikAppliedMs[side] > 200) g_ikApplied[side][0] = g_ikApplied[side][1] = g_ikApplied[side][2] = 0.0f;
            const float want[3] = {static_cast<float>(dH[0] * wSolve), static_cast<float>(dH[1] * wSolve), static_cast<float>(dH[2] * wSolve)};
            nofoot::ikCapToward(g_ikApplied[side], want, nofoot::kIkCapPerCallM);
            g_ikAppliedMs[side] = now;
            for (int i = 0; i < 3; ++i) target[i] = H0[i] + g_ikApplied[side][i];
        } else {
            for (int i = 0; i < 3; ++i) target[i] = pT[i];
            if (!(mode == 2 && side == 0)) g_ikApplied[side][0] = g_ikApplied[side][1] = g_ikApplied[side][2] = 0.0f;
        }
        nofoot::IkTwoBone tb = nofoot::ikTwoBoneFrame(S, target, l1, l2, uf);
        {
            const nofoot::IkTwoBone full = nofoot::ikTwoBoneFrame(S, pT, l1, l2, uf);   // the reach of the target itself
            o.reach = full.ok ? full.reach : 0.0;
            o.shortfall = full.ok ? full.shortfall * 100.0 : 0.0;
        }
        double elbowDir[3];
        if (!tb.ok || !nofoot::ikUnit(uf, elbowDir)) {
            ikRefuse(kIkRefSingular);
            continue;
        }
        const double theta = wSolve * nofoot::ikWrapPi(ikSwivel(tb, elbowDir, outSign, &o.clamped));
        double E[3];
        tb.elbowAt(theta, E);
        // Swing 1: U's subtree about S, from the animated elbow's direction to the solved one.
        double R1[9], R2[9], ue[3];
        const double se[3] = {E[0] - S[0], E[1] - S[1], E[2] - S[2]};
        if (!nofoot::ikUnit(se, ue)) {
            ikRefuse(kIkRefSingular);
            continue;
        }
        nofoot::ikRotFromTo(elbowDir, ue, R1);
        for (int g = 0; g < 3; ++g)
            for (uint32_t k = 0; k < counts[g]; ++k) nofoot::ikPivot(g_ikM0[lists[g][k]], R1, S, g_ikOut[lists[g][k]]);
        // Swing 2: F's subtree about E, from the hand's direction after swing 1 to the solved hand.
        double H1[3], eh[3], eT[3];
        ikPos(g_ikOut[H], H1);
        const double h1[3] = {H1[0] - E[0], H1[1] - E[1], H1[2] - E[2]}, ht[3] = {tb.hand[0] - E[0], tb.hand[1] - E[1], tb.hand[2] - E[2]};
        if (!nofoot::ikUnit(h1, eh) || !nofoot::ikUnit(ht, eT)) {
            ikRefuse(kIkRefSingular);
            continue;
        }
        nofoot::ikRotFromTo(eh, eT, R2);
        for (int g = 1; g < 3; ++g)
            for (uint32_t k = 0; k < counts[g]; ++k) {
                float tmp[16];
                std::memcpy(tmp, g_ikOut[lists[g][k]], sizeof(tmp));
                nofoot::ikPivot(tmp, R2, E, g_ikOut[lists[g][k]]);
            }
        // The hand: the rotation nlerp'd from the animated one toward the target's, at the chain's position; gH follows by D_H.
        float MH[16];
        {
            float q0[4], qt[4], q[4], rows[9];
            nofoot::pfzQuatFromMatrix(g_ikM0[H], q0);
            nofoot::pfzQuatFromMatrix(TH, qt);
            const float dq = q0[0] * qt[0] + q0[1] * qt[1] + q0[2] * qt[2] + q0[3] * qt[3];
            const float sg = dq < 0.0f ? -1.0f : 1.0f, wf = static_cast<float>(wSolve);
            float n = 0.0f;
            for (int i = 0; i < 4; ++i) {
                q[i] = (1.0f - wf) * q0[i] + wf * sg * qt[i];
                n += q[i] * q[i];
            }
            n = n > 1e-12f ? 1.0f / std::sqrt(n) : 1.0f;
            for (int i = 0; i < 4; ++i) q[i] *= n;
            ecm::quatRows(q, rows);
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) MH[r * 4 + c] = rows[r * 3 + c];
                MH[r * 4 + 3] = 0.0f;
            }
            for (int c = 0; c < 3; ++c) MH[12 + c] = g_ikOut[H][12 + c];
            MH[15] = 1.0f;
        }
        float invH2[16], DH[16];
        if (!nofoot::pfzAffineInverse(g_ikOut[H], invH2)) {
            ikRefuse(kIkRefSingular);
            continue;
        }
        // The hand's roll beyond the forearm (model space: M' = M2 x X), its twist about the forearm axis, before H is overridden.
        double X[9];
        {
            double RH2[9], RHt[9];
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) {
                    RH2[r * 3 + c] = invH2[r * 4 + c];
                    RHt[r * 3 + c] = MH[r * 4 + c];
                }
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) X[r * 3 + c] = RH2[r * 3] * RHt[c] + RH2[r * 3 + 1] * RHt[3 + c] + RH2[r * 3 + 2] * RHt[6 + c];
        }
        nofoot::pfzMul(invH2, MH, DH);
        for (uint32_t k = 0; k < arm.nH; ++k) {
            float tmp[16];
            std::memcpy(tmp, g_ikOut[arm.gH[k]], sizeof(tmp));
            nofoot::pfzMul(tmp, DH, g_ikOut[arm.gH[k]]);
        }
        std::memcpy(g_ikOut[H], MH, sizeof(MH));
        // The forearm helpers (gF less F) roll about the forearm axis by t x the hand's twist; the upper-arm helpers (gU off the U->F path) about
        // the upper-arm axis by t x the forearm's twist (swing 2's), t their projection along the bone.
        {
            const double thetaF = nofoot::ikTwistAbout(X, eT), thetaU = nofoot::ikTwistAbout(R2, ue);
            const double lF2 = nofoot::ikDot(ht, ht), lU2 = nofoot::ikDot(se, se);
            for (uint32_t k = 0; k < arm.nF; ++k) {
                const uint32_t j = arm.gF[k];
                if (j == F || !(lF2 > 1e-12)) continue;
                double pj[3];
                ikPos(g_ikOut[j], pj);
                const double rel[3] = {pj[0] - E[0], pj[1] - E[1], pj[2] - E[2]};
                double t = nofoot::ikDot(rel, ht) / lF2;
                t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
                double R[9];
                float tmp[16];
                nofoot::ikRotAxis(eT, t * thetaF, R);
                std::memcpy(tmp, g_ikOut[j], sizeof(tmp));
                nofoot::ikPivot(tmp, R, E, g_ikOut[j]);
            }
            for (uint32_t k = 0; k < arm.nU; ++k) {
                const uint32_t j = arm.gU[k];
                if (arm.chainU[k] || !(lU2 > 1e-12)) continue;
                double pj[3];
                ikPos(g_ikOut[j], pj);
                const double rel[3] = {pj[0] - S[0], pj[1] - S[1], pj[2] - S[2]};
                double t = nofoot::ikDot(rel, se) / lU2;
                t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
                double R[9];
                float tmp[16];
                nofoot::ikRotAxis(ue, t * thetaU, R);
                std::memcpy(tmp, g_ikOut[j], sizeof(tmp));
                nofoot::ikPivot(tmp, R, S, g_ikOut[j]);
            }
        }
        // The math check: the chain's hand against its target (about 0 while the target is in reach).
        {
            double hp[3];
            ikPos(g_ikOut[H], hp);
            const double e[3] = {hp[0] - target[0], hp[1] - target[1], hp[2] - target[2]};
            if (tb.reach < 1.0) o.err = nofoot::ikLen(e) * 100.0;
        }
        for (int i = 0; i < 3; ++i) {
            o.E[i] = static_cast<float>(E[i]);
            o.S[i] = static_cast<float>(S[i]);
        }
        o.solved = true;
        // NUDGE (the left arm only): gU and gF as animated (D_C), gH up by the applied offset (kIkNudgeM x the base weight, 5 cm a call).
        bool listIt = doSolveWrite;
        if (mode == 2 && side == 0 && writeOk) {
            if (now - g_ikAppliedMs[side] > 200) g_ikApplied[side][0] = g_ikApplied[side][1] = g_ikApplied[side][2] = 0.0f;
            const float want[3] = {0.0f, nofoot::kIkNudgeM * wBase, 0.0f};
            nofoot::ikCapToward(g_ikApplied[side], want, nofoot::kIkCapPerCallM);
            g_ikAppliedMs[side] = now;
            for (int g = 0; g < 3; ++g)
                for (uint32_t k = 0; k < counts[g]; ++k) {
                    const uint32_t j = lists[g][k];
                    std::memcpy(g_ikOut[j], g_ikM0[j], 64);
                    if (g == 2)
                        for (int i = 0; i < 3; ++i) g_ikOut[j][12 + i] += g_ikApplied[side][i];
                }
            listIt = true;
        } else if (mode == 2 && side == 0) {
            g_ikApplied[side][0] = g_ikApplied[side][1] = g_ikApplied[side][2] = 0.0f;
        }
        if (!listIt) continue;
        bool sane = true;
        for (int g = 0; g < 3 && sane; ++g)
            for (uint32_t k = 0; k < counts[g] && sane; ++k) sane = ikMatrixSane(g_ikOut[lists[g][k]]);
        if (!sane) {
            ikRefuse(kIkRefSingular);
            continue;
        }
        for (int g = 0; g < 3; ++g)
            for (uint32_t k = 0; k < counts[g] && nList < nofoot::kPfzMaxJoints; ++k) g_ikList[nList++] = lists[g][k];
        o.wrote = true;
    }
    // The statistics (before the writes, so a write fault still leaves the measurement).
    g_ikStatSeq.fetch_add(1, std::memory_order_acq_rel);
    if (g_ikStatReset.exchange(false, std::memory_order_relaxed)) g_ikStats = IkStats{};
    IkStats& st = g_ikStats;
    ++st.calls;
    st.fpAgeLast = static_cast<double>(fpAge);
    st.gAgeLast = static_cast<double>(gAge);
    st.fpAgeMax = std::max(st.fpAgeMax, st.fpAgeLast);
    st.gAgeMax = std::max(st.gAgeMax, st.gAgeLast);
    for (int side = 0; side < 2; ++side) {
        const SideOut& o = so[side];
        if (!o.solved) continue;
        ++st.solved;
        IkSideStats& s = st.side[side];
        const bool first = s.n == 0;
        s.distMin = first || o.dist < s.distMin ? o.dist : s.distMin;
        s.distMax = first || o.dist > s.distMax ? o.dist : s.distMax;
        s.angMin = first || o.ang < s.angMin ? o.ang : s.angMin;
        s.angMax = first || o.ang > s.angMax ? o.ang : s.angMax;
        s.reachMin = first || o.reach < s.reachMin ? o.reach : s.reachMin;
        s.reachMax = first || o.reach > s.reachMax ? o.reach : s.reachMax;
        s.distSum += o.dist;
        s.angSum += o.ang;
        s.reachSum += o.reach;
        s.shortMax = std::max(s.shortMax, o.shortfall);
        s.bendSum += o.bend;
        if (o.err >= 0.0) {
            s.errMax = std::max(s.errMax, o.err);
            ++s.errN;
        }
        s.wSum += o.wArm;
        s.gateSum += o.gate;
        if (o.clamped) ++s.clamped;
        s.l1 = o.l1;
        s.l2 = o.l2;
        std::memcpy(s.E, o.E, sizeof(s.E));
        std::memcpy(s.S, o.S, sizeof(s.S));
        if (o.wrote) ++s.written;
        ++s.n;
    }
    g_ikStatSeq.fetch_add(1, std::memory_order_release);
    // The writes, all matrices computed above.
    for (uint32_t k = 0; k < nList; ++k) {
        if (!guardedWriteFloats16(sp.buf + static_cast<uint64_t>(g_ikList[k]) * 64u, g_ikOut[g_ikList[k]])) {
            g_ikWriteDown.store(true, std::memory_order_relaxed);
            return ikRefuse(kIkRefWrite);
        }
    }
}

// The local X's post-hook: the shared span, then the pose freeze (its X and key), then the arm IK (its X and key) on the same span.
void pfzAfterAnimate(uint64_t x, uint64_t job, bool freeze, bool ik) noexcept {
    PfzSpan sp;
    if (!pfzSpanRead(x, job, &sp)) {
        if (ik) ikRefuse(kIkRefBuffer);
        return;
    }
    const bool froze = freeze && pfzFreezeStep(x, sp);
    if (ik) armIkAfterAnimate(x, sp, froze);
}

float pfzWeightNow() noexcept { return g_pfzWeight.load(std::memory_order_relaxed); }

// F55 (TEMPORARY): the FP shoulder drop, after the original Animate returns, for the FP X of the census's plan only (frAnim worker threads). The
// output span is chosen as the 3P path chooses it. Joints 19, 20, 37 and 44 are snapshotted (model space) before and after; with the frame
// thread's drop above zero, every member of the subtree of 19 and 20 has its model matrix's position row moved down by the drop (row 3's y, the
// FP model's up), so the arm chain follows. Not cumulative: Animate rewrites the span each call. No allocation, logging or locks (one try-flag
// that skips, never waits); every read and write under SEH.
std::atomic<bool> g_fdBusy{false};
pfz::FpDropPlan g_fdPlanCopy;   // the hook's scratch (under g_fdBusy)
void fdFail(uint32_t bit) noexcept { g_fdFailBits.fetch_or(bit, std::memory_order_relaxed); }
void fdAfterAnimate(uint64_t x, uint64_t job) noexcept {
    bool planOk = false;
    for (int tries = 0; tries < 2 && !planOk; ++tries) {
        const uint32_t s1 = pfz::g_fdSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        std::memcpy(&g_fdPlanCopy, &pfz::g_fdPlan, sizeof(g_fdPlanCopy));
        std::atomic_thread_fence(std::memory_order_acquire);
        planOk = pfz::g_fdSeq.load(std::memory_order_relaxed) == s1;
    }
    if (!planOk) return fdFail(1u << 0);
    const pfz::FpDropPlan& pl = g_fdPlanCopy;
    if (pl.x != x || pl.count == 0 || pl.count > nofoot::kPfzMaxF) return fdFail(1u << 1);
    uint8_t f210 = 0;
    uint64_t pose = 0;
    uint16_t count = 0;
    if (!guardedRead(x + 0x210, &f210, 1) || !guardedRead(x + 8, &pose, 8) || !pose || !guardedRead(pose, &count, 2)) return fdFail(1u << 2);
    if (count != pl.joints || count == 0 || count > nofoot::kPfzMaxJoints || count <= nofoot::kDropHandR) return fdFail(1u << 3);
    uint64_t buf = 0;
    if (f210 == 0) {
        if (!guardedRead(job + 0x28, &buf, 8)) buf = 0;
    } else {
        uint64_t y = 0;
        uint32_t cap = 0;
        if (guardedRead(x + 0x1F0, &y, 8) && y && guardedRead(y + 0xE0, &cap, 4) && static_cast<uint32_t>(count) - 1u < cap) {
            if (!guardedRead(y + 0xA0, &buf, 8)) buf = 0;
        }
    }
    if (!buf) return fdFail(1u << 4);
    constexpr uint32_t kIds[4] = {nofoot::kDropClavL, nofoot::kDropClavR, nofoot::kDropHandL, nofoot::kDropHandR};
    float before[4][3] = {}, after[4][3] = {};
    for (int k = 0; k < 4; ++k)
        if (!guardedRead(buf + static_cast<uint64_t>(kIds[k]) * 64u + 48u, before[k], sizeof(before[k]))) return fdFail(1u << 5);
    // F56: the arm IK's FP targets: joints 51, 52 (the handAttach joints) and 37, 44 (the hands), whole matrices, BEFORE any drop write.
    if (pfz::g_ikWanted.load(std::memory_order_relaxed) && count > 52) {
        constexpr uint32_t kIkIds[4] = {51, 52, nofoot::kDropHandL, nofoot::kDropHandR};
        float m[4][16];
        bool ok = true;
        for (int k = 0; k < 4 && ok; ++k) ok = guardedRead(buf + static_cast<uint64_t>(kIkIds[k]) * 64u, m[k], sizeof(m[k]));
        if (ok) {
            g_ikFp.seq.fetch_add(1, std::memory_order_acq_rel);
            std::memcpy(g_ikFp.m, m, sizeof(m));
            g_ikFp.ms = GetTickCount64();
            g_ikFp.seq.fetch_add(1, std::memory_order_release);
        }
    }
    const float drop = g_f55DropM.load(std::memory_order_relaxed);
    bool wrote = false;
    if (drop > 0.0f && !g_fdWriteDown.load(std::memory_order_relaxed)) {
        for (uint32_t k = 0; k < pl.count; ++k) {
            const uint32_t j = pl.member[k];
            if (j >= count) continue;
            const uintptr_t ya = static_cast<uintptr_t>(buf + static_cast<uint64_t>(j) * 64u + 52u);
            float yv = 0.0f;
            if (!guardedRead(ya, &yv, 4)) return fdFail(1u << 5);
            if (!guardedWriteFloat(ya, yv - drop)) {
                g_fdWriteDown.store(true, std::memory_order_relaxed);
                return fdFail(1u << 6);
            }
        }
        wrote = true;
    }
    for (int k = 0; k < 4; ++k)
        if (!guardedRead(buf + static_cast<uint64_t>(kIds[k]) * 64u + 48u, after[k], sizeof(after[k]))) return fdFail(1u << 5);
    g_f55Snap.seq.fetch_add(1, std::memory_order_acq_rel);
    std::memcpy(g_f55Snap.before, before, sizeof(before));
    std::memcpy(g_f55Snap.after, after, sizeof(after));
    g_f55Snap.drop = wrote ? drop : 0.0f;
    g_f55Snap.ms = GetTickCount64();
    g_f55Snap.seq.fetch_add(1, std::memory_order_release);
    if (wrote) g_fdWrites.fetch_add(1, std::memory_order_relaxed);
}

uint64_t __fastcall pfzAnimateHooked(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8) {
    const F35Fn original = reinterpret_cast<F35Fn>(g_pfzHook.original);
    if (!original) return 0;
    const uint64_t r = original(a1, a2, a3, a4, s5, s6, s7, s8);   // the original first, with its arguments
    g_pfzCalls.fetch_add(1, std::memory_order_relaxed);
    const int freezeMode = pfz::g_mode.load(std::memory_order_relaxed);
    const bool ikWanted = pfz::g_ikWanted.load(std::memory_order_relaxed);   // F56: the arm IK needs the hook (and the FP snapshot) too
    if (!a2 || (freezeMode == 0 && !ikWanted && !pfz::g_dropWanted.load(std::memory_order_relaxed))) return r;
    uint64_t x = 0;
    if (!guardedRead(a2, &x, 8)) {
        pfzFail(kPfzFailJob);
        return r;
    }
    pfz::xNote(x);   // F51: every evaluator Animate sees, by call count (the census matches the local one against it)
    // F55: the FP X (the census's shoulder drop plan): snapshot, and the drop when the frame thread's value is above zero.
    const uint64_t fx = pfz::g_fdX.load(std::memory_order_acquire);
    if (fx && x == fx) {
        g_fdCalls.fetch_add(1, std::memory_order_relaxed);
        if (g_fdBusy.exchange(true, std::memory_order_acquire)) {
            g_fdBusySkips.fetch_add(1, std::memory_order_relaxed);
            return r;
        }
        fdAfterAnimate(x, a2);
        g_fdBusy.store(false, std::memory_order_release);
        return r;
    }
    if (freezeMode == 0 && !ikWanted) return r;
    const uint64_t lx = pfz::g_localX.load(std::memory_order_acquire), ax = pfz::g_armX.load(std::memory_order_acquire);
    const bool freeze = freezeMode != 0 && lx && x == lx, ik = ikWanted && ax && x == ax;
    if (!freeze && !ik) return r;   // only the local avatar's evaluator
    g_pfzLocalCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_pfzBusy.exchange(true, std::memory_order_acquire)) {
        g_pfzBusySkips.fetch_add(1, std::memory_order_relaxed);
        return r;
    }
    pfzAfterAnimate(x, a2, freeze, ik);
    g_pfzBusy.store(false, std::memory_order_release);
    return r;
}

// Frame thread, once (VR): the prologue bytes verified against the loaded image; a mismatch refuses with a line and installs nothing.
void pfzInstallOnce() {
    if (g_pfzTried) return;
    g_pfzTried = true;
    if (!runtimeVrProfile()) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const char* peWhy = nullptr;
    if (!base || !checkIdentity(base, &peWhy)) {
        Log::get().note("native on foot pose freeze: NOT installed -- identity mismatch (%s); the game was not touched.", peWhy ? peWhy : "no module");
        return;
    }
    const F9ProbeSpec spec{"native-on-foot-pose-freeze", kPfzAnimateRva, kPfzAnimatePrologue, sizeof(kPfzAnimatePrologue), kPfzAnimateStolen,
                           reinterpret_cast<void*>(&pfzAnimateHooked), "F49 pose freeze"};
    size_t stolen = 0;
    std::string why;
    if (installF9ProbeInto(g_pfzHook, spec, base, &stolen, &why)) {
        g_pfzArmed = true;
        Log::get().note("native on foot pose freeze: Animate post-hook installed at EliteDangerous64.exe+0x741520, stolen=%zu bytes, prologue %zu bytes "
                        "verified (the local avatar's X only; writes only with fix.native_on_foot_pose_freeze = on; F55: the FP X's clavicle subtree only "
                        "with fix.native_on_foot_fp_shoulder_drop_cm above 0, in body mode)", stolen, sizeof(kPfzAnimatePrologue));
    } else {
        Log::get().note("native on foot pose freeze: NOT installed at EliteDangerous64.exe+0x741520 -- %s. The game was not touched.", why.c_str());
    }
}

nofoot::PfzHeadRest g_pfzRest;   // frame thread only
nofoot::PfzLocoRest g_pfzLoco;   // F51: the no-locomotion gate (frame thread only)
uint64_t g_pfzLocoStamp = 0;     // F51: the body sample the gate last took
nofoot::PfzBlend g_pfzBlend;
uint64_t g_pfzLastMs = 0, g_pfzLineMs = 0;
uint32_t g_pfzFailNamed = 0;
const char* g_pfzWhyLast = "(none)";
uint64_t g_pfzCallsLast = 0, g_pfzLocalLast = 0, g_pfzBusyLast = 0;
uint64_t g_pfzOverflowLast = 0;

// F51 (frame thread, every 5 s with the statistics): the local X's identification. The X table's banks turn over (the bank about to be active is
// cleared first, so the one left behind keeps the last full 5 s for the census); the left bank's top 8 X by Animate calls, each with P = [X+8] and
// what it matches; then each latched skeleton's candidates as the census published them; then the freeze gate's inputs.
void pfzIdentLine(int mode, bool ads, bool headFresh) noexcept {
    static const char* const kModeNames[3] = {"off", "probe", "on"};
    const uint32_t left = pfz::g_xBank.load(std::memory_order_relaxed) & 1u, next = left ^ 1u;
    for (uint32_t i = 0; i < pfz::kXSlots; ++i) {
        pfz::g_xTable[next][i].n.store(0, std::memory_order_relaxed);
        pfz::g_xTable[next][i].x.store(0, std::memory_order_relaxed);
    }
    pfz::g_xBank.store(next, std::memory_order_relaxed);
    uint64_t xs[pfz::kXSlots] = {}, ns[pfz::kXSlots] = {};
    uint32_t used = 0;
    for (uint32_t i = 0; i < pfz::kXSlots; ++i) {
        const uint64_t x = pfz::g_xTable[left][i].x.load(std::memory_order_relaxed);
        if (!x) continue;
        xs[used] = x;
        ns[used] = pfz::g_xTable[left][i].n.load(std::memory_order_relaxed);
        ++used;
    }
    const uint64_t gp3 = pfz::g_cand[0].gp.load(std::memory_order_relaxed), gpFp = pfz::g_cand[1].gp.load(std::memory_order_relaxed);
    const uint64_t lx = pfz::g_localX.load(std::memory_order_relaxed);
    const uint64_t c0a = pfz::g_cand[0].x2B8.load(std::memory_order_relaxed), c0b = pfz::g_cand[0].x120.load(std::memory_order_relaxed);
    const uint64_t c1a = pfz::g_cand[1].x2B8.load(std::memory_order_relaxed), c1b = pfz::g_cand[1].x120.load(std::memory_order_relaxed);
    char top[1400] = {};
    size_t at = 0;
    uint32_t sameP3 = 0;
    for (uint32_t i = 0; i < used; ++i) {
        uint64_t p = 0;
        if (gp3 && guardedRead(static_cast<uintptr_t>(xs[i]) + 8u, &p, 8) && p == gp3) ++sameP3;
    }
    for (uint32_t rank = 0; rank < 8 && rank < used; ++rank) {
        uint32_t best = rank;
        for (uint32_t j = rank + 1; j < used; ++j)
            if (ns[j] > ns[best]) best = j;
        std::swap(xs[rank], xs[best]);
        std::swap(ns[rank], ns[best]);
        uint64_t p = 0;
        const bool pOk = guardedRead(static_cast<uintptr_t>(xs[rank]) + 8u, &p, 8);
        const uint64_t x = xs[rank];
        const int w = std::snprintf(top + at, sizeof(top) - at, "%s%016llX x%llu P=%s%016llX%s%s%s%s", rank ? "; " : "", static_cast<unsigned long long>(x),
                                    static_cast<unsigned long long>(ns[rank]), pOk ? "" : "unread ", static_cast<unsigned long long>(p),
                                    pOk && gp3 && p == gp3 ? " (= 3P GetPoseData)" : "", pOk && gpFp && p == gpFp ? " (= FP GetPoseData)" : "",
                                    x == lx ? " [the local X]" : "",
                                    x == c0a || x == c0b ? " [a 3P iface X]" : x == c1a || x == c1b ? " [an FP iface X]" : "");
        if (w <= 0 || static_cast<size_t>(w) >= sizeof(top) - at) break;
        at += static_cast<size_t>(w);
    }
    const uint64_t overflow = pfz::g_xOverflow.load(std::memory_order_relaxed);
    Log::get().note("native on foot pose freeze (F51, %s): Animate X table, the last 5 s: %u distinct X (%llu not counted: the table was full); "
                    "%u with [X+8] = the 3P GetPoseData; top by calls: %s", kModeNames[mode], used,
                    static_cast<unsigned long long>(overflow - g_pfzOverflowLast), sameP3, at ? top : "none");
    g_pfzOverflowLast = overflow;
    static const char* const kHow[5] = {"none", "the kind's X (seen in Animate)", "matched by P = [X+8] (the one X seen with it)",
                                        "the kind's X, NOT seen in Animate (no unique P match)", "the kind's X; P is AMBIGUOUS (several X seen with it)"};
    for (uint32_t slot = 0; slot < 2; ++slot) {
        const pfz::Candidates& c = pfz::g_cand[slot];
        const uint64_t stamp = c.stamp.load(std::memory_order_relaxed);
        if (!stamp) {
            Log::get().note("native on foot pose freeze (F51): %s skeleton: no census sample yet", slot ? "FP" : "3P (local)");
            continue;
        }
        const uint64_t gp = c.gp.load(std::memory_order_relaxed), xa = c.x2B8.load(std::memory_order_relaxed),
                       xb = c.x120.load(std::memory_order_relaxed), pa = c.p2B8.load(std::memory_order_relaxed),
                       pb = c.p120.load(std::memory_order_relaxed), how = c.how.load(std::memory_order_relaxed);
        const uint64_t nowMs = GetTickCount64();
        Log::get().note("native on foot pose freeze (F51): %s skeleton iface %016llX (%s, %llu ms ago): GetPoseData %016llX; X=*(iface+0x2B8) %016llX "
                        "(P %016llX%s, %llu Animate calls in 5-10 s), X=*(iface+0x120) %016llX (P %016llX%s, %llu calls); [iface+0x2C0]=%016llX "
                        "[iface+0xF8]=%016llX; the census takes %016llX: %s (%llu X seen with P = GetPoseData)",
                        slot ? "FP" : "3P (local, g_skelIface[0])", static_cast<unsigned long long>(c.iface.load(std::memory_order_relaxed)),
                        ecm::hkindText(static_cast<uint32_t>(c.kind.load(std::memory_order_relaxed))),
                        static_cast<unsigned long long>(nowMs >= stamp ? nowMs - stamp : 0), static_cast<unsigned long long>(gp),
                        static_cast<unsigned long long>(xa), static_cast<unsigned long long>(pa), gp && pa == gp ? " = GetPoseData" : "",
                        static_cast<unsigned long long>(pfz::xCount(xa)), static_cast<unsigned long long>(xb), static_cast<unsigned long long>(pb),
                        gp && pb == gp ? " = GetPoseData" : "", static_cast<unsigned long long>(pfz::xCount(xb)),
                        static_cast<unsigned long long>(c.f2C0.load(std::memory_order_relaxed)),
                        static_cast<unsigned long long>(c.fF8.load(std::memory_order_relaxed)),
                        static_cast<unsigned long long>(c.chosen.load(std::memory_order_relaxed)), how < 5 ? kHow[how] : "?",
                        static_cast<unsigned long long>(c.pMatches.load(std::memory_order_relaxed)));
    }
    Log::get().note("native on foot pose freeze (F51): gate: body horizontal speed %.2f m/s (moving above %.2f), %s; ADS %s; animated head %s (F49's "
                    "steady test, logged only: %s)", g_pfzLoco.speed, nofoot::PfzLocoRest::kMoveMps,
                    g_pfzLoco.resting ? "no locomotion (freeze allowed)" : "locomotion or within 0.3 s of it", ads ? "yes" : "no",
                    headFresh ? "fresh" : "stale", g_pfzRest.resting ? "steady" : "not steady");
}

void pfzTick(uint64_t now) {
    const int mode = pfz::g_mode.load(std::memory_order_relaxed);
    static int lastMode = -1;
    static const char* const kModeNames[3] = {"off", "probe", "on"};
    if (mode != lastMode) {
        Log::get().note("native on foot pose freeze: fix.native_on_foot_pose_freeze = %s (was %s)%s", kModeNames[mode],
                        lastMode < 0 ? "none" : kModeNames[lastMode],
                        mode == 2 ? "; writes while not walking or running (F51: a weapon out freezes too; not ADS), blended over 0.3 s"
                                  : "; nothing is written");
        lastMode = mode;
    }
    // F55: the shoulder drop needs the hook too; F56: and the arm IK
    const bool hookWanted = mode != 0 || pfz::g_dropWanted.load(std::memory_order_relaxed) || pfz::g_ikWanted.load(std::memory_order_relaxed);
    if (hookWanted) pfzInstallOnce();
    if (g_pfzArmed) g_pfzHook.gate.store(hookWanted ? 1u : 0u, std::memory_order_release);
    if (!nativeOnFootActive()) {   // the census sets them again (it runs only while active)
        pfz::g_localX.store(0, std::memory_order_release);
        pfz::g_armX.store(0, std::memory_order_release);
    }
    // The gate and the blend (the weight is 0 unless the key is on).
    const double dt = g_pfzLastMs && now > g_pfzLastMs ? std::min(0.1, static_cast<double>(now - g_pfzLastMs) / 1000.0) : 0.0;
    g_pfzLastMs = now;
    const bool ads = journalAds();
    const uint64_t headMs = g_pfzHeadMs.load(std::memory_order_relaxed);
    const bool headFresh = headMs != 0 && now >= headMs && now - headMs < 500;
    if (headFresh) g_pfzRest.step(now, static_cast<double>(g_pfzHeadUp.load(std::memory_order_relaxed)), ads);   // F49's head test: logged only
    else g_pfzRest = nofoot::PfzHeadRest{};
    // F51: the gate is no locomotion (the body's horizontal speed, from the P1 body sample), so a weapon out freezes too; ADS stays excluded.
    bool bodyFresh = false;
    {
        EyeSample es;
        {
            std::lock_guard<std::mutex> lock(g_eyeMutex);
            es = g_eyeSample;
        }
        bodyFresh = es.valid && now >= es.stamp && now - es.stamp < 1500;
        if (bodyFresh && es.stamp != g_pfzLocoStamp) {
            g_pfzLocoStamp = es.stamp;
            const double pos[3] = {es.origin[0], es.origin[1], es.origin[2]}, up[3] = {es.rows[3], es.rows[4], es.rows[5]};
            g_pfzLoco.step(es.stamp, pos, up, ads);
        }
        if (!bodyFresh) g_pfzLoco = nofoot::PfzLocoRest{};
    }
    const bool resting = bodyFresh && g_pfzLoco.resting;
    const char* why = mode != 2 ? "the key is not on" : !nativeOnFootActive() ? "not native stereo on foot"
                    : g_pfzWriteDown.load(std::memory_order_relaxed) ? "the writes stood down" : ads ? "aiming down sights"
                    : !headFresh ? "no animated head from the hook in 500 ms" : !bodyFresh ? "no body sample in 1.5 s"
                    : !resting ? "walking or running (or still within 0.3 s of it)" : nullptr;
    g_pfzBlend.step(why == nullptr, dt);
    if (mode != 2) g_pfzBlend = nofoot::PfzBlend{};   // every write path is behind the key: off or probe writes nothing at once
    g_pfzWeight.store(g_pfzBlend.w, std::memory_order_relaxed);
    if (mode == 2) {
        const char* whyNow = why ? why : "freezing";
        if (std::strcmp(whyNow, g_pfzWhyLast) != 0) {
            Log::get().note("native on foot pose freeze: %s%s (weight %.2f)", why ? "held: " : "freezing: no locomotion", why ? why : "",
                            static_cast<double>(g_pfzBlend.w));
            g_pfzWhyLast = whyNow;
        }
    } else {
        g_pfzWhyLast = "(none)";
    }
    // Each failure the hook met, named once.
    const uint32_t bits = g_pfzFailBits.load(std::memory_order_relaxed) & ~g_pfzFailNamed;
    for (uint32_t i = 0; i < kPfzFailKinds; ++i) {
        if (!(bits & (1u << i))) continue;
        g_pfzFailNamed |= 1u << i;
        Log::get().note("native on foot pose freeze: the hook refused a call: %s (said once)", kPfzFailText[i]);
    }
    // Every 5 s while the hook is in and native stereo is on foot: the statistics.
    if (!g_pfzArmed || mode == 0 || !nativeOnFootActive()) return;
    if (g_pfzLineMs == 0) g_pfzLineMs = now;
    if (now - g_pfzLineMs < 5000) return;
    g_pfzLineMs = now;
    PfzStats st;
    bool got = false;
    for (int tries = 0; tries < 8 && !got; ++tries) {
        const uint32_t s1 = g_pfzStatSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        st = g_pfzStats;
        std::atomic_thread_fence(std::memory_order_acquire);
        got = g_pfzStatSeq.load(std::memory_order_relaxed) == s1;
    }
    g_pfzStatReset.store(true, std::memory_order_relaxed);
    const uint64_t calls = g_pfzCalls.load(std::memory_order_relaxed), local = g_pfzLocalCalls.load(std::memory_order_relaxed),
                   busy = g_pfzBusySkips.load(std::memory_order_relaxed);
    const uint64_t dCalls = calls - g_pfzCallsLast, dLocal = local - g_pfzLocalLast, dBusy = busy - g_pfzBusyLast;
    g_pfzCallsLast = calls;
    g_pfzLocalLast = local;
    g_pfzBusyLast = busy;
    pfzIdentLine(mode, ads, headFresh);   // F51: the local X's identification (candidates, the Animate X table), the gate's state
    if (!got || st.calls == 0) {
        Log::get().note("native on foot pose freeze (F49, %s): %llu Animate calls in 5 s, %llu for the local X %016llX (%llu skipped busy); %s",
                        kModeNames[mode], static_cast<unsigned long long>(dCalls), static_cast<unsigned long long>(dLocal),
                        static_cast<unsigned long long>(pfz::g_localX.load(std::memory_order_relaxed)), static_cast<unsigned long long>(dBusy),
                        !got ? "the statistics were being written (retried next time)" : "no measured call for the local X");
        return;
    }
    char joint[2][200] = {};
    for (int i = 0; i < 2; ++i) {
        if (st.n[i] == 0) {
            std::snprintf(joint[i], sizeof(joint[i]), "joint %d: not measured (not in F or past the count)", i ? 44 : 34);
            continue;
        }
        const double n = static_cast<double>(st.n[i]);
        std::snprintf(joint[i], sizeof(joint[i]), "joint %d minus the rest chain: mean %.1f mm (min %.1f, max %.1f; mean vector %.1f,%.1f,%.1f mm)",
                      i ? 44 : 34, st.sumMm[i] / n, st.minMm[i], st.maxMm[i], st.sumV[i][0] / n, st.sumV[i][1] / n, st.sumV[i][2] / n);
    }
    Log::get().note("native on foot pose freeze (F49, %s): %llu Animate calls in 5 s, %llu for the local X %016llX (%llu measured, %llu skipped busy); "
                    "thread %u (%u thread changes); [X+0x210]=%u [X+0x12C]=%u [X+0x128]=0x%08X [X+0x212]=%u; output [job+0x28] %llu calls, "
                    "[[X+0x1F0]+0xA0] %llu; count %u; %s; %s; weight %.2f, frames written %llu (last weight %.2f)",
                    kModeNames[mode], static_cast<unsigned long long>(dCalls), static_cast<unsigned long long>(dLocal),
                    static_cast<unsigned long long>(pfz::g_localX.load(std::memory_order_relaxed)), static_cast<unsigned long long>(st.calls),
                    static_cast<unsigned long long>(dBusy), st.tid, st.tidChanges, static_cast<unsigned>(st.f210), static_cast<unsigned>(st.f12C),
                    st.f128, static_cast<unsigned>(st.f212), static_cast<unsigned long long>(st.perBuffer[0]),
                    static_cast<unsigned long long>(st.perBuffer[1]), st.count, joint[0], joint[1], static_cast<double>(g_pfzBlend.w),
                    static_cast<unsigned long long>(st.written), static_cast<double>(st.wLast));
}

// F56 (TEMPORARY): the arm IK's frame-thread half. The base weight ramps to 1 over 0.2 s while the key is nudge or on and every input holds
// (native stereo on foot, body mode, hide_fp_body on, body_unclip on, an arm plan for the local X, the FP snapshot and G under 100 ms, no
// write stand-down), and to 0 over 0.2 s otherwise; off and probe hold it at 0 (nothing is written). Each refusal the hook met is named once;
// every 5 s while native stereo is on foot and the key is not off: the gate, the data ages, G's inputs and the refusals, then a line per arm.
nofoot::IkBlend g_ikBlend;               // frame thread only
uint64_t g_ikLastMs = 0, g_ikLineMs = 0;
const char* g_ikWhyLast = "(none)";
uint32_t g_ikFailNamed = 0;
uint64_t g_ikCallsLast = 0, g_ikGPubLast = 0, g_ikGRefLast = 0;
uint64_t g_ikRefusedLast[kIkRefKinds] = {};
void armIkTick(uint64_t now) noexcept {
    const int mode = pfz::g_ikMode.load(std::memory_order_relaxed);
    const double dt = g_ikLastMs && now > g_ikLastMs ? std::min(0.1, static_cast<double>(now - g_ikLastMs) / 1000.0) : 0.0;
    g_ikLastMs = now;
    // The newest FP snapshot and G stamps (a read that meets a write keeps the last stamp, so a torn read never drops the weight).
    static uint64_t fpMsLast = 0;
    static IkGData gLast;
    static bool gHave = false;
    float fpm[4][16];
    uint64_t fpRead = 0;
    if (ikFpRead(fpm, &fpRead)) fpMsLast = fpRead;
    const uint64_t fpMs = fpMsLast;
    const bool fpFresh = fpMs != 0 && (now > fpMs ? now - fpMs : 0) <= nofoot::kIkFreshMs;
    IkGData gNow;
    if (ikGRead(&gNow)) {
        gLast = gNow;
        gHave = true;
    }
    const IkGData& G = gLast;
    const bool gOk = gHave;
    const bool gFresh = gOk && (now > G.ms ? now - G.ms : 0) <= nofoot::kIkFreshMs;
    const bool active = nativeOnFootActive();
    const char* gate = !active ? "not native stereo on foot" : g_alignMode.load(std::memory_order_relaxed) != 2 ? "not body mode (body align = body)"
                     : !g_fpKeyOn.load(std::memory_order_relaxed) ? "fix.native_on_foot_hide_fp_body is off"
                     : !g_unclipKeyOn.load(std::memory_order_relaxed) ? "fix.native_on_foot_body_unclip is off"
                     : !pfz::g_armX.load(std::memory_order_relaxed) ? "no arm plan for the local X"
                     : !fpFresh ? "the FP snapshot is older than 100 ms (or none)" : !gFresh ? "G is older than 100 ms (or none)"
                     : g_ikWriteDown.load(std::memory_order_relaxed) ? "the writes stood down" : nullptr;
    const bool writes = mode == 2 || mode == 3;
    const char* why = mode == 0 ? "the key is off" : mode == 1 ? "probe: nothing is written" : gate;
    g_ikBlend.step(why == nullptr, dt);
    if (!writes) g_ikBlend = nofoot::IkBlend{};   // every write path is behind nudge or on: off and probe write nothing at once
    g_ikWeight.store(g_ikBlend.w, std::memory_order_relaxed);
    if (writes) {
        const char* whyNow = why ? why : "writing";
        if (std::strcmp(whyNow, g_ikWhyLast) != 0) {
            Log::get().note("native on foot arm IK (F56, %s): %s%s (base weight %.2f)", mode == 3 ? "on" : "nudge", why ? "held: " : "writing: every gate holds",
                            why ? why : "", static_cast<double>(g_ikBlend.w));
            g_ikWhyLast = whyNow;
        }
    } else {
        g_ikWhyLast = "(none)";
    }
    const uint32_t bits = g_ikFailBits.load(std::memory_order_relaxed) & ~g_ikFailNamed;
    for (uint32_t i = 0; i < kIkRefKinds; ++i) {
        if (!(bits & (1u << i))) continue;
        g_ikFailNamed |= 1u << i;
        Log::get().note("native on foot arm IK (F56): the hook refused a call: %s (said once)", kIkRefText[i]);
    }
    if (mode == 0 || !active || !g_pfzArmed) return;
    if (g_ikLineMs == 0) g_ikLineMs = now;
    if (now - g_ikLineMs < 5000) return;
    g_ikLineMs = now;
    IkStats st;
    bool got = false;
    for (int tries = 0; tries < 8 && !got; ++tries) {
        const uint32_t s1 = g_ikStatSeq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        st = g_ikStats;
        std::atomic_thread_fence(std::memory_order_acquire);
        got = g_ikStatSeq.load(std::memory_order_relaxed) == s1;
    }
    g_ikStatReset.store(true, std::memory_order_relaxed);
    static const char* const kModeNames[4] = {"off", "probe", "nudge", "on"};
    const uint64_t calls = g_ikCalls.load(std::memory_order_relaxed), gPub = g_ikGPublishes.load(std::memory_order_relaxed),
                   gRef = g_ikGRefusals.load(std::memory_order_relaxed);
    char refusals[700] = {};
    size_t at = 0;
    for (uint32_t i = 0; i < kIkRefKinds; ++i) {
        const uint64_t n = g_ikRefused[i].load(std::memory_order_relaxed);
        const uint64_t dn = n - g_ikRefusedLast[i];
        g_ikRefusedLast[i] = n;
        static const char* const kShort[kIkRefKinds] = {"buffer", "plan torn", "plan", "FP torn", "FP stale", "G torn", "G stale", "side", "singular",
                                                        "write"};
        const int w = std::snprintf(refusals + at, sizeof(refusals) - at, "%s%s %llu", i ? ", " : "", kShort[i], static_cast<unsigned long long>(dn));
        if (w > 0 && static_cast<size_t>(w) < sizeof(refusals) - at) at += static_cast<size_t>(w);
    }
    const uint64_t fpAgeNow = fpMs && now > fpMs ? now - fpMs : 0, gAgeNow = gOk && now > G.ms ? now - G.ms : 0;
    Log::get().note("native on foot arm IK (F56, %s): %llu IK calls in 5 s (%llu measured, %llu arm solves); base weight %.2f; gate: %s; refusals in "
                    "5 s: %s; ages now: FP snapshot %s%llu ms, G %s%llu ms; in the hook: FP last %.0f max %.0f ms, G last %.0f max %.0f ms; G published "
                    "%llu, refused %llu in 5 s",
                    kModeNames[mode], static_cast<unsigned long long>(calls - g_ikCallsLast), static_cast<unsigned long long>(got ? st.calls : 0),
                    static_cast<unsigned long long>(got ? st.solved : 0), static_cast<double>(g_ikBlend.w), gate ? gate : "holds", refusals,
                    fpMs ? "" : "(none) ", static_cast<unsigned long long>(fpAgeNow), gOk ? "" : "(none) ", static_cast<unsigned long long>(gAgeNow),
                    got ? st.fpAgeLast : -1.0, got ? st.fpAgeMax : -1.0, got ? st.gAgeLast : -1.0, got ? st.gAgeMax : -1.0,
                    static_cast<unsigned long long>(gPub - g_ikGPubLast), static_cast<unsigned long long>(gRef - g_ikGRefLast));
    g_ikCallsLast = calls;
    g_ikGPubLast = gPub;
    g_ikGRefLast = gRef;
    if (gOk)
        Log::get().note("native on foot arm IK (F56): G inputs: M3P |row length - 1| max %.4f det %.4f; F_root |row length - 1| max %.4f det %.4f "
                        "(root %llu ms older than G); r (%.3f, %.3f, %.3f) m (raise %s); d (%.3f, %.3f, %.3f) m; G rows (%.3f %.3f %.3f) (%.3f %.3f %.3f) "
                        "(%.3f %.3f %.3f) pos (%.3f, %.3f, %.3f)",
                        static_cast<double>(G.m3pRowErr), static_cast<double>(G.m3pDet), static_cast<double>(G.fRowErr), static_cast<double>(G.fDet),
                        static_cast<unsigned long long>(G.ms >= G.frootMs ? G.ms - G.frootMs : 0), static_cast<double>(G.r[0]),
                        static_cast<double>(G.r[1]), static_cast<double>(G.r[2]), g_f51FpOn.load(std::memory_order_relaxed) ? "on" : "off",
                        static_cast<double>(G.d[0]), static_cast<double>(G.d[1]), static_cast<double>(G.d[2]), static_cast<double>(G.g[0]),
                        static_cast<double>(G.g[1]), static_cast<double>(G.g[2]), static_cast<double>(G.g[4]), static_cast<double>(G.g[5]),
                        static_cast<double>(G.g[6]), static_cast<double>(G.g[8]), static_cast<double>(G.g[9]), static_cast<double>(G.g[10]),
                        static_cast<double>(G.g[12]), static_cast<double>(G.g[13]), static_cast<double>(G.g[14]));
    if (!got) return;
    for (int side = 0; side < 2; ++side) {
        const IkSideStats& s = st.side[side];
        if (s.n == 0) {
            Log::get().note("native on foot arm IK (F56): arm %s: no solve in 5 s (see the refusals)", side ? "R" : "L");
            continue;
        }
        const double n = static_cast<double>(s.n);
        char err[96] = "no sample in reach";
        if (s.errN) std::snprintf(err, sizeof(err), "max %.3f cm over %u samples in reach", s.errMax, s.errN);
        Log::get().note("native on foot arm IK (F56): arm %s: %u solves; T_A vs the animated 3P attach: distance min %.1f mean %.1f max %.1f cm, "
                        "rotation min %.1f mean %.1f max %.1f deg; reach min %.2f mean %.2f max %.2f (shortfall max %.1f cm); l1 %.1f cm l2 %.1f cm; "
                        "animated elbow angle mean %.1f deg (180 straight); last solved elbow (%.3f, %.3f, %.3f) shoulder (%.3f, %.3f, %.3f) model; swivel "
                        "clamped %u of %u; chain hand error %s; distance gate mean %.2f; per-arm weight mean %.2f (on only); frames written %llu",
                        side ? "R" : "L", s.n, s.distMin, s.distSum / n, s.distMax, s.angMin, s.angSum / n, s.angMax, s.reachMin, s.reachSum / n,
                        s.reachMax, s.shortMax, static_cast<double>(s.l1), static_cast<double>(s.l2), s.bendSum / n, static_cast<double>(s.E[0]),
                        static_cast<double>(s.E[1]), static_cast<double>(s.E[2]), static_cast<double>(s.S[0]), static_cast<double>(s.S[1]),
                        static_cast<double>(s.S[2]), s.clamped, s.n, err, s.gateSum / n, s.wSum / n, static_cast<unsigned long long>(s.written));
    }
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

// ---- F44 (TEMPORARY instrument, fix.native_on_foot_marker_probe): the find, the draw-time match and the 1 s log. Render thread only (the owner
// context's Map, Unmap and draw tees, and the frame boundary): nothing below is shared but the tag cells. Measure only: the Unmap tee reads the
// game's mapped pool before the real Unmap, the draw tee copies what the draw binds into EDVR's own staging buffers, and no binding, constant
// buffer or game resource is written.
//
// THE FIND. A pool is watched once an on-foot HUD draw binds it at t33 (a structured buffer of stride 336). At each Unmap of a DISCARD or
// NO_OVERWRITE map of it, the sentinels in flight (the tags of the last kF44TagAgeMax frames) are looked for at +16 of its records: first at the
// index each key was last found at, then, when a key is still missing, over the whole mapped range (streaming loads; a scan over 2 ms backs off
// for 30 frames). Each find keeps (pool, record index, X0 = the stored position).
// THE MATCH (F45; draw_record_tag.h holds the stream match, the NDC pair and the frame staging). F46 (part B) looks past the vertex shader hash
// at three kinds of draw only: the on-foot HUD draws (VS 95D01BA609BF7500 or 46546443FD3C3F88: they name the pool, learnPool), the marker
// composite (VS B10B032BDFD46700, F45's reader of the tagged record) and the world pool families (engine_velocity_family::familyOfVs: the eye's
// view). A marker composite bound to a watched pool, in a frame with a find in that pool (this frame or the last), copies its whole instance
// stream (the single stride-8 vertex buffer from StartInstanceLocation, up to kEntriesMax entries), its b0 rows 4..7 and its b1 row 275 (o_hud;
// from engine motion's CB shadow when a watch follows that b1 from constant 0) into the frame's staging buffer. A world draw into an eye-sized
// target copies b1 rows 270..275 (the CB shadow's when it holds them, else one 96-byte GPU copy), once per eye a frame; cb0 is not read. BOTH
// EYES ARE THE UI LAYER'S: a target's eye is the layer's own redirect mapping (uiLayerEyeOfGameTarget), else ui depth's frame table (read only,
// never registering), else an earlier frame's answer from either for the same target; a world target with none of these takes the table's own
// rule over the frame's eye-sized world targets (the first of a shape is eye 0), and its depth view's eye is kept beside it as a check. Read
// back two or more frames later without a wait: a marker composite reads a find when FirstElement + x of an entry is its index (y counted
// apart). Per eye, ndc_marker = (r4 . p, r5 . p) / |r7 . p| with p = (X0 - o_hud, 1) is set against ndc_P = P through that eye's world rows
// at o_w, in pixels of the marker's target and in degrees. Measure only: no constant buffer is patched and nothing is written back.
namespace f44 {
constexpr uint32_t kPools = 4;
constexpr uint32_t kHits = 32;              // finds kept for a frame (this frame's, and the one before's); F52: 32 (every key's record)
constexpr uint32_t kHistory = 96;           // tags taken from the cells: P and t' for a find, by sentinel (F52: 96, every key in flight)
constexpr uint32_t kTagAgeMax = 8;          // frames a sentinel stays in flight
constexpr uint32_t kScansPerFrame = 4;
constexpr uint64_t kScanBackoffUs = 2000;
// F46: the marker composite's b0 rows 4..7 (row 6 rides along unused) and b1 row 275; the world draws' b1 rows 270..275 (row 274 rides along).
constexpr UINT kMarkerRowsOffset = 4 * 16, kMarkerRowsBytes = 64, kOriginOffset = 275 * 16, kOriginBytes = 16;
constexpr UINT kWorldOffset = 270 * 16, kWorldBytes = 6 * 16;
// F45: the frame's staging buffer and its draw records (F46: no record cap; a frame's records grow as needed).
constexpr uint32_t kFrameSlots = 6, kFrameBytes = 1u << 20;   // 1 MB a frame: 131072 stream entries
constexpr uint32_t kEntriesMax = 16384;                       // stream entries read per draw (more are counted as cut)
constexpr uint32_t kSlotFinds = 32;                           // a frame's finds, kept for its read-back (F52: 32)
constexpr uint32_t kTuples = 12;                              // distinct readers of a find (VS, PS, eye, call) logged a second
constexpr uint32_t kWorldViews = 32;                          // (frame, eye) world views kept: more than kFrameSlots frames of both eyes
constexpr uint32_t kTargets = 8;                              // the frame's world targets; the remembered target eyes
constexpr uint32_t kWorldTriesPerEye = 8;                     // world draws a frame that may try an eye's copy
constexpr uint32_t kSrvCache = 8;
constexpr double kDefaultTargetSize = 2016.0;                 // the marker target's size when its view did not resolve
// F47: the correction and its instruments.
constexpr uint32_t kMatchFresh = 8;      // frames a read-back's match stays an eye's prediction (read-backs land 2..3 frames late)
constexpr double kGateNdc = 0.05;        // F50: |dx|, |dy| must be under this (NDC) after the baseline (F47: 0.1 on P's own delta)
constexpr double kLagStdMaxPx = 0.5;     // F50: no patch until the eye's baseline is seeded and its standard deviation is under this
constexpr double kLagPitchSpreadPx = 2.0;   // F50: the honesty check: c by head pitch / roll differing by more than this is "not constant"
constexpr uint32_t kLagBinMinN = 30;     // F50: a pitch or roll bin counts toward the honesty check from this many slow-head frames
constexpr uint32_t kCbShadows = 4;       // marker composite b0 buffers shadowed on the CPU
constexpr UINT kCb0ShadowBytes = 128;    // b0 rows 0..7
constexpr uint32_t kBurstFrames = 8;     // per-frame marker composite lines: the first 8 read-back frames of each second
constexpr uint32_t kCensusEvery = 30;    // a full pool scan every 30 frames even with every key found (a second copy of a record)
constexpr UINT kPrivateMax = 65536;      // the private b0's size limit (the game's b0 ByteWidth, made once)
// F52: the per-key correction and the reference.
constexpr uint32_t kKeySlots = 12;       // tag keys tracked (six marker composite draws an eye were seen; second posts take their own)
constexpr uint32_t kRefFreshFrames = 90; // the temporal rows count as available while the eye had them within this many frames
constexpr uint32_t kRefNotes = 8;        // reference switches said in the log (counted after)
// F53: the scene reference's census of cb1 rows 270..273, the trust check, the untagged marker composite draws' records.
constexpr uint32_t kRowSets = 8;              // distinct (vs, ps, rows) kept per eye in a census frame
constexpr uint32_t kRowCensusFrames = 2;      // census frames a second (each logged per eye)
constexpr uint32_t kRowCensusDraws = 4096;    // world draws a census frame looks at (more are counted as capped)
constexpr uint32_t kUntags = 16;              // untagged marker composite draws (eye, place, record) kept a second
constexpr uint32_t kAxisNotes = 6;            // axis mismatches of the temporal rows described in the log (counted after)

enum Refuse : uint32_t { kNoT33, kStride, kNoStream, kConstants, kNoSlot, kBufferFull, kNoTag, kPoolChanged, kScanFault, kRefuseCount };
const char* const kRefuseName[kRefuseCount] = {
    "an on-foot HUD draw has no t33 view",
    "an on-foot HUD draw's t33 is not a structured buffer of stride 336 (the record layout the HUD VS reads)",
    "a marker composite draw bound to the pool has no single instance stream of stride 8, or its range is outside the buffer",
    "a marker composite draw's constant buffer is missing or too small for b0 rows 4..7 / b1 row 275",
    "no free staging slot for the frame (read-backs late)",
    "the frame's staging buffer is full",
    "a found record's tag has no P (node+0x50 unread)",
    "a watched pool address is no longer a buffer of stride 336",
    "the pool scan faulted"};
const char* const kRefuseShort[kRefuseCount] = {"no t33", "stride", "no stream", "constants", "no slot", "buffer full", "no tag", "pool changed",
                                                "scan fault"};

struct Pool {
    ID3D11Resource* res = nullptr;   // identity only, no reference held; verified at each Map (the game's live resource there)
    uint32_t bytes = 0;
    void* mapped = nullptr;
    int mapType = 0;
    uint32_t lastFrame = 0;
};
struct Find {
    const void* pool = nullptr;
    uint32_t index = 0, sentinel = 0, key = 0, tagFrame = 0, frame = 0;
    int mapType = 0;
    float x0[3] = {}, tp[3] = {}, p[3] = {};
    bool pOk = false, haveTag = false;
};
enum RecFlag : uint8_t { kRecWorld = 1, kRecMarker = 2, kRecShadowO = 4, kRecCut = 8 };
// One recorded draw: offsets into its frame's staging slot (-1: not copied).
struct Rec {
    uint64_t vs = 0, ps = 0;
    const void* pool = nullptr;
    uint32_t first = 0, instances = 0, entries = 0;
    int32_t streamAt = -1, rowsAt = -1, originAt = -1, worldAt = -1;   // stream; b0 rows 4..7; b1 row 275; b1 rows 270..275
    float origin[4] = {};    // b1 row 275 from the CB shadow (kRecShadowO)
    uint32_t tw = 0, th = 0; // the marker composite's target size (0: its view did not resolve)
    int8_t eye = -1;         // the UI layer's eye of the draw's target (eyeSrc says how); -1 unknown
    char eyeSrc = 0;         // 'L' the layer's redirect mapping, 'T' ui depth's frame table, 'C' remembered, 'O' the order rule
    uint8_t flags = 0;
    char kind = 0;           // the draw call (nativeOnFootMarkerProbeDraw)
    // F47: every marker composite draw is recorded, its stream copied or not.
    uint32_t start = 0;                  // StartInstanceLocation
    uint16_t ordEye = 0, ordAll = 0;     // its place among the frame's marker composite draws of its eye, and of the frame
    const void* t33 = nullptr;           // its VS t33 resource (identity; any)
    bool watched = false, findsNow = false;   // bound to a watched pool; that pool had a find this frame or the last
    bool cpuRows = false;                // the CPU shadow of b0 rows 4..7 at the draw
    float cpu[16] = {};
    bool fixApplied = false, fixHeld = false;
    float fixDx = 0.0f, fixDy = 0.0f;    // the applied delta (NDC)
    float fixCx = 0.0f, fixCy = 0.0f;    // F50: the baseline c the delta was taken against (NDC)
    float fixHead = -1.0f;               // F50: the head's speed at the decision (deg/s; -1 unknown)
    int32_t patchedAt = -1;              // the private b0's rows 4..7 after the patch (staged)
    uint8_t fixRef = 0;                  // F52: the reference the patch measured against (drawtag::RefMode)
};
// F46: an eye's world view as a world draw read it: b1 rows 270..273 and row 275, by frame and eye.
struct WorldView {
    bool valid = false;
    uint32_t frame = 0;
    int eye = -1;
    float rows[16] = {};
    float o[4] = {};
};
// F46: a target whose eye the UI layer or ui depth's table named (kept across frames: the targets are the same textures frame to frame).
struct TargetEye {
    const void* res = nullptr;
    int eye = -1;
};
// F46: a world draw's target this frame (by its view), and the eye it was given.
struct WorldTarget {
    const void* view = nullptr;
    const void* res = nullptr;
    uint32_t w = 0, h = 0, fmt = 0;
    int eye = -1;
    char src = 0;
    bool eyeSized = false, dup = false;   // dup: another view of a resource listed before it
    uint32_t tries = 0;
};
// F46: one eye's pairs this second.
struct EyeStats {
    uint64_t pairs = 0, markerReads = 0, worldViews = 0, noWorld = 0, behind = 0, wZero = 0, wNeg = 0, noP = 0, degN = 0, otherN = 0;
    double pxSum = 0.0, pxMax = 0.0, degSum = 0.0, degMax = 0.0, otherPxSum = 0.0, pwSum = 0.0, xoSum = 0.0;
    double dx = 0.0, dy = 0.0, ndcM[2] = {}, ndcP[2] = {}, pw = 0.0, xo = 0.0, tw = 0.0, th = 0.0;
    // F47 item 1, at read-back: the delta of the same P through the world rows and the marker rows (unpatched), in pixels and degrees.
    uint64_t dN = 0, dDegN = 0;
    double dxSum = 0.0, dySum = 0.0, dxMax = 0.0, dyMax = 0.0, dDegSum = 0.0, dDegMax = 0.0;
    // F47 item 4: the same through the patched rows (the private b0, staged after its write).
    uint64_t rN = 0;
    double rxSum = 0.0, rySum = 0.0, rxMax = 0.0, ryMax = 0.0;
    // F47 at the draw: applied, held, and why not.
    uint64_t fPatched = 0, fHeld = 0, fUnmatched[2] = {}, fOtherDraw = 0, fRanged = 0, fNoRows = 0, fNoOrigin = 0, fNoWorld = 0, fNoP = 0,
             fBehind = 0, fGate = 0, fBind = 0;
    // F47 at read-back: the patched draw read the record (the prediction held) or not; the CPU b0 rows against the GPU copy.
    uint64_t predHit = 0, predMiss = 0, cpuAgree = 0, cpuDiffer = 0;
    double cpuDiffMax = 0.0;
    // F50 at the draw: |m - c| (px) by the head's speed (bins < 3, 3..20, 20..60, >= 60 deg/s), the frames the baseline took, its resets,
    // F53: c is the fixed icon offset e; the decisions refused because the eye's scene rows were not trusted this frame. At read-back:
    // |marker - (P + e)| through the patched rows (px) by the same bins.
    uint64_t lagN[4] = {}, lagTaken = 0, lagResets = 0, fUntrusted = 0, resN[4] = {};
    double lagSum[4] = {}, lagMax[4] = {}, resSum[4] = {}, resMax[4] = {};
};
struct SrvInfo {
    const void* srv = nullptr;
    ID3D11Resource* res = nullptr;   // identity only
    uint32_t first = 0, bytes = 0;
    bool pool = false;               // a structured buffer of stride 336 behind a buffer view
};
struct Tuple {
    uint64_t vs = 0, ps = 0, draws = 0;
    int eye = -1;
    char kind = 0;
    uint32_t instMin = 0, instMax = 0, entry = 0, record = 0, xHits = 0, yHits = 0;
    bool viaY = false;
};
// The eye sources, as counted: the layer's redirect mapping, ui depth's frame table, remembered, the order rule.
int srcIndex(char s) {
    return s == 'L' ? 0 : s == 'T' ? 1 : s == 'C' ? 2 : 3;
}
// F52: one tag key's second, per eye: the record it was found at, the marker composite draws that read it (their places in the eye, as a
// mask), |m - c| at the decision and the residual after the patch by the head's speed (bins < 3, 3..20, 20..60, >= 60 deg/s), and the patch.
struct KeyWin {
    uint32_t record = 0;
    uint64_t reads[2] = {};
    uint32_t ordMask[2] = {};
    uint64_t lagN[2][4] = {}, resN[2][4] = {};
    double lagSum[2][4] = {}, lagMax[2][4] = {}, resSum[2][4] = {}, resMax[2][4] = {};
    uint64_t decided[2] = {}, patched[2] = {}, held[2] = {}, untrusted[2] = {}, gate[2] = {}, resets[2] = {}, otherUnmatched[2] = {};
};
// F53: one distinct cb1 rows 270..273 content a census eye-frame saw, by the vertex and pixel shader that read it, and whether that draw
// passed the scene reference's test (drawtag::sceneRefWhy).
struct RowSet {
    uint64_t vs = 0, ps = 0, n = 0;
    bool scene = false;
    float rows[16] = {};
};
struct RowCensus {
    uint32_t frame = ~0u;
    bool active = false;
    uint64_t draws = 0, noRows = 0, capped = 0, full[2] = {};
    RowSet sets[2][kRowSets];
    uint32_t n[2] = {};
};
// F53: an untagged marker composite draw (its stream read back, no found record read by x): the record its first entry names, and that
// record's position (+16) and low bits as the next Unmap of its pool shows them.
struct Untag {
    const void* pool = nullptr;
    uint32_t record = 0, instances = 0;
    int eye = -1;
    uint16_t ord = 0;
    uint64_t n = 0, ps = 0;
    bool havePos = false, taggable = false;
    float pos[3] = {};
    uint32_t bits = 0;   // the 24 low bits as a sentinel would carry them
    int tagKey = -1;     // a tag in flight they match (the key), -1 none
    uint32_t tagAge = 0;
};
// F52: an eye's world rows' source this second: the first world pool draw that gave the eye's view (its shaders, target, depth view, viewport,
// b1), the last one and how often the vertex shader changed between eye-frames.
struct WorldSrc {
    uint64_t n = 0, vs = 0, ps = 0, vsChanges = 0;
    uint32_t w = 0, h = 0;
    const void* dsv = nullptr;
    const void* cb1 = nullptr;
    bool vpOk = false;
    float vp[6] = {};   // TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth
};
// F52: an eye's world rows against the temporal pass's camera rows for the eye, this second.
struct RefCmp {
    uint64_t ticks = 0, n = 0, noTemporal = 0, noWorld = 0, degenerate = 0, convention = 0, sameBuf = 0, otherBuf = 0, switches = 0, pxN = 0;
    uint64_t trusted = 0, overPx = 0, noP = 0;   // F53: frames trusted (agree under 1 px), compared but over 1 px, compared with no P
    double degSum = 0.0, degMax = 0.0, pxSum = 0.0, pxMax = 0.0;
    drawtag::BasisDiff last;
    int sign = 0;
    uint64_t tvs = 0;
    uint64_t decided[2] = {};   // decisions measured against world_first, temporal
};
struct Window {
    uint64_t mapDiscard = 0, mapNoOverwrite = 0, mapOther = 0;
    uint64_t unmapsTagged = 0, unmapsNoFind = 0, finds = 0, sameIndex = 0, changedIndex = 0, exact = 0, differs = 0, hintFinds = 0;
    uint64_t scans = 0, scanUsMax = 0, scanUsSum = 0, backoffs = 0;
    // F46: the draw side's cost: the tee's calls, the draws looked at past their VS hash (by kind), the time in them, records and bytes staged.
    uint64_t hookCalls = 0, scanned = 0, scanHud = 0, scanMarker = 0, scanWorld = 0, drawTicks = 0, recs = 0, stagedBytes = 0;
    // The marker composite draws and their streams.
    uint64_t hudDraws = 0, markerPool = 0, markerFinds = 0, indirect = 0, streamDraws = 0, streamEntries = 0, streamCut = 0, framesRead = 0,
             lost = 0;
    uint64_t streamRead = 0, readsX = 0, readsYOnly = 0, markerNoEye = 0, markerSrc[4] = {};
    Tuple tuples[kTuples];
    uint32_t nTuples = 0;
    uint64_t tuplesDropped = 0;
    uint64_t refused[kRefuseCount] = {};
    bool haveFind = false;           // the window's last find
    uint32_t index = 0, key = 0, age = 0;
    int mapType = 0;
    const void* pool = nullptr;
    float x0[3] = {}, tp[3] = {};
    // F46: the world views: draws looked at, eye-frames captured (CB shadow or staged), the eye's source, the depth-view check, the failures.
    uint64_t worldDraws = 0, worldShadow = 0, worldStaged = 0, worldRead = 0, worldSrc[4] = {}, dsvAgree = 0, dsvDisagree = 0, dsvUnknown = 0;
    uint64_t worldNoTarget = 0, worldNotEyeSized = 0, worldNoEye = 0, worldNoRows = 0, worldStageFail = 0, worldTriesCapped = 0;
    UINT worldCb1Bytes = 0, worldF1 = 0;
    bool worldCb1Bound = false;
    EyeStats eye[2];
    // The head, from the runtime ring, once a frame.
    uint64_t headN = 0;
    double headSum = 0.0, headMax = 0.0;
    // F47: every marker composite draw (any pool), the pools learned from them, a second copy of a record in one unmap, the census scans,
    // the CPU b0 shadow's writes, and the fix's draws with no eye or a stale decision.
    uint64_t b10bDraws = 0, b10bNoT33 = 0, b10bNotPool = 0, markerLearned = 0, dupFinds = 0, censusScans = 0, cbMapped = 0, cbUpdated = 0,
             fixNoEye = 0, fixStale = 0, fixLateRestore = 0, fixNotIssued = 0;
    // F52: per key (by key slot), the B10B bookkeeping per eye at read-back (and the draws with no eye), the world rows' source and the
    // reference comparison per eye, the key slots reclaimed.
    KeyWin keys[kKeySlots];
    drawtag::B10bTally tally[2];
    uint64_t tallyNoEye = 0, keyReclaims = 0;
    WorldSrc wsrc[2];
    RefCmp ref[2];
    // F53: the scene reference's refusals (by drawtag::SceneRefWhy), the cb1 census (frames, the most distinct contents in an eye-frame, the
    // widest forward angle between two of them), the untagged marker composite draws and their records.
    uint64_t sceneRefused[4] = {}, rowCensusFrames = 0, rowDistinctMax[2] = {};
    double rowFwdMax[2] = {};
    Untag untag[kUntags];
    uint32_t nUntag = 0;
    uint64_t untagDropped = 0, untagPosRead = 0, untagPosFault = 0;
};

Pool g_pools[kPools];
Find g_finds[2][kHits];              // [0] the frame g_findFrame, [1] the frame before it
uint32_t g_findN[2] = {};
uint32_t g_findFrame = ~0u;
F44Tag g_hist[kHistory];
uint32_t g_histNext = 0;
uint32_t g_lastIndex[256] = {};
bool g_haveLast[256] = {};
uint32_t g_backoffUntil = 0, g_scanFrame = ~0u, g_scansThisFrame = 0;
drawtag::FrameStaging<kFrameSlots, kFrameBytes> g_staging;
std::vector<Rec> g_recs[kFrameSlots];   // F46: no cap (the scan is limited to the marker composite and the world capture instead)
uint32_t g_recFrame[kFrameSlots] = {};
Find g_slotFinds[kFrameSlots][kSlotFinds];
uint32_t g_slotFindN[kFrameSlots] = {};
WorldView g_worldViews[kWorldViews];
uint32_t g_worldViewNext = 0;
TargetEye g_targetEyes[kTargets];
uint32_t g_targetEyeNext = 0;
WorldTarget g_wt[kTargets];
uint32_t g_wtN = 0, g_wtFrame = ~0u;
SrvInfo g_srvCache[kSrvCache];
uint32_t g_srvCacheN = 0, g_srvCacheFrame = ~0u;
const void* g_eyeDsv = nullptr;
uint32_t g_eyeDsvFrame = ~0u;
int g_eyeDsvEye = -1;
uint32_t g_worldOk[2] = {~0u, ~0u};
uint32_t g_polledFrame = ~0u;
Find g_lastFind;                     // the newest find (any window)
bool g_haveLastFind = false;
Window g_win;
uint64_t g_winMs = 0;
bool g_refuseNoted[kRefuseCount] = {};
bool g_worldRowsNoted = false;
uint64_t g_taggedSeen = 0, g_untaggableSeen = 0, g_busySeen = 0;
uint32_t g_poolNotes = 0;

// ---- F47: the correction's state (render thread only).
// The CPU shadow of a marker composite's b0 (rows 0..7): learned at a B10B draw that binds it from constant 0, written by the Map/Unmap tees
// (copied before the real Unmap) and the UpdateSubresource tee. Checked against the GPU copy at read-back.
struct CbShadow {
    const void* res = nullptr;   // identity only
    uint32_t bytes = 0;
    void* mapped = nullptr;
    bool valid = false;
    float rows[8][4] = {};
};
CbShadow g_cbShadow[kCbShadows];
uint32_t g_cbShadowNext = 0;
// An eye's world view this frame from engine motion's CB shadow (the CPU's: a staged one is not known until its read-back).
struct CpuWorld {
    uint32_t frame = ~0u;
    float rows[16] = {};
    float o[4] = {};
};
CpuWorld g_cpuWorld[2];
// F52: an eye's temporal reference this frame: the world rows with the temporal pass's camera rotation for the eye (drawtag::swapRotation),
// at the world rows' o.
CpuWorld g_tempRef[2];
// An eye's latest read-back match: the frame, the matching draw's place among the eye's marker composite draws, and the find's key.
// F52: one per key per eye (KeyState).
struct EyeMatch {
    uint32_t frame = ~0u;
    uint16_t ord = 0;
    uint32_t key = 0;
};
// An eye's last good delta (held one frame on a miss). F52: one per key per eye.
struct Good {
    uint32_t frame = ~0u;
    double dx = 0.0, dy = 0.0;
};
uint32_t g_ordFrame = ~0u;
uint16_t g_ordEye[2] = {}, g_ordAll = 0;
// The decision the draw tee made for this draw, and the binding Begin made for it.
struct Fix {
    bool pending = false, bound = false, held = false;
    uint64_t serial = 0;
    ID3D11DeviceContext* ctx = nullptr;   // identity only
    const void* cb0 = nullptr;            // identity only: the b0 the tee saw
    int eye = -1;
    uint32_t frame = 0;
    int slot = -1;
    size_t rec = 0;
    double dx = 0.0, dy = 0.0;
    double cx = 0.0, cy = 0.0, head = -1.0;   // F50: the baseline the delta was taken against, the head's speed at the decision
    int keySlot = -1;                     // F52: the key slot the decision was made for
    int ref = 0;                          // F52: the reference it measured against (drawtag::RefMode)
    float rows45[8] = {};                 // rows 4 and 5, moved
    ID3D11Buffer* saved = nullptr;        // the game's b0 while ours is bound (a reference held)
};
Fix g_fix;
// F50: the head this frame (headSample): its angular speed (deg/s, -1 unknown), pitch and roll (deg, from the newest render head).
double g_headDps = -1.0, g_headPitch = 0.0, g_headRoll = 0.0;
bool g_headPoseOk = false;
// F50: an eye's lag baseline (drawtag::LagBaseline), the frustum it was taken under (b0 row 5 and row 7 scales, the marker target's size),
// the eye's matched draw place (the eye assignment), and the honesty check's bins since the last reset: m on slow-head frames by head pitch
// (< -15, -15..15, > 15 deg) and roll (|roll| < 10, >= 10 deg).
struct LagEye {
    drawtag::LagBaseline b;
    bool keyed = false;
    double r5n = 0.0, r7n = 0.0;
    uint32_t tw = 0, th = 0;
    uint16_t ord = 0;
    uint32_t refEpoch = 0;   // F52: the eye's reference epoch the baseline was taken under (a reference change resets it)
    uint32_t lastFrame = ~0u;
    uint32_t resetNotes = 0;
    const char* lastReset = "none";
    uint64_t pitchN[3] = {}, rollN[2] = {};
    double pitchSum[3][2] = {}, rollSum[2][2] = {};
    void clearBins() {
        for (int i = 0; i < 3; ++i) pitchN[i] = 0, pitchSum[i][0] = pitchSum[i][1] = 0.0;
        for (int i = 0; i < 2; ++i) rollN[i] = 0, rollSum[i][0] = rollSum[i][1] = 0.0;
    }
};
// F52: a tag key's state per eye: its read-back match, its baseline, its last good delta; and the record it was last found at.
struct KeyEye {
    EyeMatch match;
    LagEye lag;
    Good good;
};
struct KeyState {
    KeyEye eye[2];
    uint32_t record = 0;
};
drawtag::KeyIndex<kKeySlots> g_keyIdx;
KeyState g_keys[kKeySlots];
// F52: the reference: the frame each eye's comparison ran, the last frame the temporal rows were usable for the eye, the reference in use
// per eye (-1 not yet) and its epoch (a change resets every baseline of the eye), the eye's world b1 (identity) for the same-buffer check,
// the switches said.
uint32_t g_refFrame[2] = {~0u, ~0u}, g_tempSeen[2] = {~0u, ~0u}, g_refEpoch[2] = {}, g_refNotes = 0;
int g_refEff[2] = {-1, -1};
const void* g_worldCb1[2] = {};
uint64_t g_lastWorldVs[2] = {};
WorldView g_refViews[kWorldViews];   // the temporal reference by (frame, eye), for the residual at read-back
uint32_t g_refViewNext = 0;
uint64_t g_tagOnlySeen = 0;
// F53: the cb1 census (render thread), the census frames left this second, the frame each eye's scene rows were last trusted, the axis notes.
RowCensus g_rowCensus;
uint32_t g_rowCensusLeft = kRowCensusFrames;
uint32_t g_sceneTrusted[2] = {~0u, ~0u};
uint32_t g_axisNotes = 0;
uint64_t g_drawSerial = 0;
ID3D11Buffer* g_privCb = nullptr;         // the private b0 (DEFAULT, constant buffer), made once
ID3D11Buffer* g_workBuf = nullptr;        // the private b0 built here first (DEFAULT, not a constant buffer): the game's b0 copied in,
                                          // rows 4 and 5 written by a boxed UpdateSubresource, then copied whole into the private b0
UINT g_privBytes = 0;
bool g_privFailed = false, g_fixAppliedNoted = false, g_unmatchedNoted[2] = {}, g_bindNoted = false;
uint32_t g_burstLeft = kBurstFrames;
uint32_t g_lastCensus = 0;
uint64_t g_secondPostsSeen = 0;

void refuse(Refuse r) {
    ++g_win.refused[r];
    if (!g_refuseNoted[r]) {
        g_refuseNoted[r] = true;
        Log::get().note("native on foot marker probe: refused (%s); counted in the 1 s lines, said once", kRefuseName[r]);
    }
}

void rollFrame(uint32_t frame) {
    if (g_findFrame == frame) return;
    if (g_findFrame + 1u == frame) {
        for (uint32_t i = 0; i < g_findN[0]; ++i) g_finds[1][i] = g_finds[0][i];
        g_findN[1] = g_findN[0];
    } else {
        g_findN[1] = 0;
    }
    g_findN[0] = 0;
    g_findFrame = frame;
}

void remember(const F44Tag& t) {
    for (F44Tag& h : g_hist)
        if (h.sentinel == t.sentinel && h.key == t.key && h.frame == t.frame) {
            h = t;
            return;
        }
    g_hist[g_histNext] = t;
    g_histNext = (g_histNext + 1) % kHistory;
}
const F44Tag* tagOf(uint32_t s) {
    for (const F44Tag& h : g_hist)
        if (h.sentinel == s && (h.key & 0xFFu) == drawtag::sentinelKey(s) && h.frame == drawtag::sentinelFrame(s)) return &h;
    return nullptr;
}

// The hint records, then (full) every record; under SEH: a mapping that is not what the pool's size says faults here, not in the game.
__declspec(noinline) uint32_t scanGuarded(const uint8_t* base, uint32_t bytes, const uint32_t* wants, uint32_t nWants, const uint32_t* hints,
                                          uint32_t nHints, bool full, drawtag::Hit* hits, uint32_t maxHits, bool* fault) noexcept {
    uint32_t n = 0;
    __try {
        if (full) {
            n = drawtag::scanRecords(base, bytes, wants, nWants, hits, maxHits, drawtag::streamLoad16);
        } else {
            for (uint32_t i = 0; i < nHints && n < maxHits; ++i)
                if (drawtag::checkRecord(base, bytes, hints[i], wants, nWants, hits[n], drawtag::streamLoad16)) ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
        return 0;
    }
    return n;
}

// F53: one record's 16 bytes at +16 (streaming load), under SEH; false when the record is past the mapped range or the read faults.
__declspec(noinline) bool readRecordGuarded(const uint8_t* base, uint32_t bytes, uint32_t index, uint32_t out[4]) noexcept {
    const uint64_t at = static_cast<uint64_t>(index) * drawtag::kRecordStride + drawtag::kPositionOffset;
    if (!base || at + 16 > bytes) return false;
    __try {
        drawtag::streamLoad16(base + at, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void addFind(const Pool& p, const drawtag::Hit& h, int mapType, uint32_t frame) {
    for (uint32_t i = 0; i < g_findN[0]; ++i)
        if (g_finds[0][i].pool == p.res && g_finds[0][i].index == h.index && g_finds[0][i].sentinel == h.sentinel) return;   // found again
    if (g_findN[0] >= kHits) return;
    Find& f = g_finds[0][g_findN[0]++];
    f = Find{};
    f.pool = p.res;
    f.index = h.index;
    f.sentinel = h.sentinel;
    f.key = drawtag::sentinelKey(h.sentinel);
    f.tagFrame = drawtag::sentinelFrame(h.sentinel);
    f.frame = frame;
    f.mapType = mapType;
    std::memcpy(f.x0, h.pos, sizeof(f.x0));
    if (const F44Tag* t = tagOf(h.sentinel)) {
        f.haveTag = true;
        f.pOk = t->pOk;
        std::memcpy(f.tp, t->tp, sizeof(f.tp));
        std::memcpy(f.p, t->p, sizeof(f.p));
        const bool exact = std::memcmp(f.tp, f.x0, sizeof(f.x0)) == 0;
        if (exact) ++g_win.exact; else ++g_win.differs;
    }
    ++g_win.finds;
    if (g_haveLast[f.key]) {
        if (g_lastIndex[f.key] == f.index) ++g_win.sameIndex; else ++g_win.changedIndex;
    }
    g_haveLast[f.key] = true;
    g_lastIndex[f.key] = f.index;
    g_lastFind = f;
    g_haveLastFind = true;
    g_win.haveFind = true;
    g_win.index = f.index;
    g_win.key = f.key;
    g_win.age = (frame - f.tagFrame) & 0xFFFFu;
    g_win.mapType = mapType;
    g_win.pool = p.res;
    std::memcpy(g_win.x0, f.x0, sizeof(g_win.x0));
    std::memcpy(g_win.tp, f.tp, sizeof(g_win.tp));
}

void unmapPool(Pool& p) {
    const uint8_t* base = static_cast<const uint8_t*>(p.mapped);
    const int mapType = p.mapType;
    p.mapped = nullptr;
    if (!base || (mapType != D3D11_MAP_WRITE_DISCARD && mapType != D3D11_MAP_WRITE_NO_OVERWRITE)) return;
    const uint32_t frame = g_f44Frame.load(std::memory_order_relaxed);
    for (size_t i = 0; i < kF44Cells; ++i) {
        F44Tag t;
        if (f44ReadCell(i, t)) remember(t);
    }
    // F53: the untagged marker composite draws' records in this pool (read back this second): each one's position and low bits as the game
    // wrote them for this frame, and the tag in flight they carry if any (a marker piece the match missed carries one).
    for (uint32_t i = 0; i < g_win.nUntag; ++i) {
        Untag& u = g_win.untag[i];
        if (u.pool != p.res) continue;
        uint32_t wv[4] = {};
        if (!readRecordGuarded(base, p.bytes, u.record, wv)) {
            ++g_win.untagPosFault;
            continue;
        }
        ++g_win.untagPosRead;
        u.havePos = true;
        for (int k = 0; k < 3; ++k) u.pos[k] = drawtag::floatOf(wv[k]);
        u.taggable = drawtag::taggableBits(wv[0]) && drawtag::taggableBits(wv[1]) && drawtag::taggableBits(wv[2]);
        u.bits = drawtag::decodeWords(wv);
        u.tagKey = -1;
        u.tagAge = 0;
        if (u.taggable)
            if (const F44Tag* t = tagOf(u.bits)) {
                u.tagKey = static_cast<int>(t->key);
                u.tagAge = (frame - t->frame) & 0xFFFFu;
            }
    }
    uint32_t wants[kHistory];
    uint32_t keys[kHistory];
    uint32_t nWants = 0;
    for (const F44Tag& h : g_hist) {
        if (!h.sentinel || ((frame - h.frame) & 0xFFFFu) > kTagAgeMax) continue;
        wants[nWants] = h.sentinel;
        keys[nWants] = h.key & 0xFFu;
        ++nWants;
    }
    if (!nWants) return;
    rollFrame(frame);
    ++g_win.unmapsTagged;
    uint32_t hints[kHistory + 2 * kHits];
    uint32_t nHints = 0;
    auto addHint = [&](uint32_t index) {
        for (uint32_t j = 0; j < nHints; ++j)
            if (hints[j] == index) return;
        if (nHints < kHistory + 2 * kHits) hints[nHints++] = index;
    };
    for (uint32_t k = 0; k < nWants; ++k)
        if (g_haveLast[keys[k]]) addHint(g_lastIndex[keys[k]]);
    // F47: every index this pool's finds had this frame and the last (a second copy of a record is looked for where it was).
    for (int s = 0; s < 2; ++s)
        for (uint32_t i = 0; i < g_findN[s]; ++i)
            if (g_finds[s][i].pool == p.res) addHint(g_finds[s][i].index);
    drawtag::Hit hits[kHits];
    bool fault = false;
    uint32_t n = nHints ? scanGuarded(base, p.bytes, wants, nWants, hints, nHints, false, hits, kHits, &fault) : 0;
    g_win.hintFinds += n;
    // A key whose newest tag in flight has no hit (an older tag at the last index is a stale record): the whole mapped range, within the
    // per-frame cap and outside a backoff.
    auto ageOf = [frame](uint32_t s) { return (frame - drawtag::sentinelFrame(s)) & 0xFFFFu; };
    bool missing = false;
    for (uint32_t k = 0; k < nWants && !missing; ++k) {
        bool newest = true;
        for (uint32_t j = 0; j < nWants && newest; ++j) newest = keys[j] != keys[k] || ageOf(wants[j]) >= ageOf(wants[k]);
        if (!newest) continue;
        bool got = false;
        for (uint32_t i = 0; i < n && !got; ++i) got = hits[i].sentinel == wants[k];
        missing = !got;
    }
    if (g_scanFrame != frame) {
        g_scanFrame = frame;
        g_scansThisFrame = 0;
    }
    // F47: a census scan every kCensusEvery frames even when every key was found, so a second copy of a record at another index shows up.
    const bool census = !missing && frame - g_lastCensus >= kCensusEvery;
    if (!fault && (missing || census) && g_scansThisFrame < kScansPerFrame && static_cast<int32_t>(frame - g_backoffUntil) >= 0) {
        ++g_scansThisFrame;
        if (census) {
            g_lastCensus = frame;
            ++g_win.censusScans;
        }
        const uint64_t t0 = f30Qpc();
        n = scanGuarded(base, p.bytes, wants, nWants, nullptr, 0, true, hits, kHits, &fault);
        const uint64_t us = f30QpcUs(f30Qpc() - t0);
        ++g_win.scans;
        g_win.scanUsSum += us;
        if (us > g_win.scanUsMax) g_win.scanUsMax = us;
        if (us > kScanBackoffUs) {
            g_backoffUntil = frame + 30;
            ++g_win.backoffs;
        }
    }
    if (fault) {
        refuse(kScanFault);
        return;
    }
    if (!n) ++g_win.unmapsNoFind;
    // Per key, only the newest tag found (a DISCARD map's renamed memory can still hold an older frame's record). F47: every record that carries
    // that newest tag is a find (a second copy of the record at another index is kept, and counted).
    for (uint32_t i = 0; i < n; ++i) {
        bool newest = true;
        for (uint32_t j = 0; j < n && newest; ++j)
            newest = j == i || drawtag::sentinelKey(hits[j].sentinel) != drawtag::sentinelKey(hits[i].sentinel) ||
                     ageOf(hits[j].sentinel) >= ageOf(hits[i].sentinel);
        if (!newest) continue;
        for (uint32_t j = 0; j < i; ++j)
            if (hits[j].sentinel == hits[i].sentinel && hits[j].index != hits[i].index) {
                ++g_win.dupFinds;
                break;
            }
        addFind(p, hits[i], mapType, frame);
    }
}

void learnPool(ID3D11Resource* res, uint32_t bytes, uint32_t frame) {
    Pool* empty = nullptr;
    Pool* oldest = &g_pools[0];
    for (Pool& p : g_pools) {
        if (p.res == res) {
            p.lastFrame = frame;
            if (!p.mapped) p.bytes = bytes;
            return;
        }
        if (!p.res && !empty) empty = &p;
        if (p.lastFrame < oldest->lastFrame) oldest = &p;
    }
    Pool* slot = empty ? empty : oldest;
    *slot = Pool{};
    slot->res = res;
    slot->bytes = bytes;
    slot->lastFrame = frame;
    if (g_poolNotes < 8) {
        ++g_poolNotes;
        Log::get().note("native on foot marker probe: watching the on-foot HUD's t33 pool %p (%u bytes, %u records of 336)", static_cast<void*>(res),
                        bytes, bytes / drawtag::kRecordStride);
    }
}

// A bound VS constant buffer and its first constant (a ranged bind's offset, in 16-byte constants; 0 otherwise).
bool vsConstants(ID3D11DeviceContext* ctx, UINT slot, ID3D11Buffer** buf, UINT* first) {
    *buf = nullptr;
    *first = 0;
    ID3D11DeviceContext1* c1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) && c1) {
        UINT f = 0, num = 0;
        c1->VSGetConstantBuffers1(slot, 1, buf, &f, &num);
        c1->Release();
        *first = f;
    } else {
        ctx->VSGetConstantBuffers(slot, 1, buf);
    }
    return *buf != nullptr;
}
bool covers(ID3D11Buffer* b, UINT offset, UINT bytes) {
    if (!b) return false;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    return static_cast<uint64_t>(offset) + bytes <= d.ByteWidth;
}

// The head's angular speed between the runtime ring's two newest samples, once a frame (F45: counted unconditionally for the 1 s line).
void headSample() {
    g_headDps = -1.0;   // F50: unknown until this frame's sample lands
    g_headPoseOk = false;
    EdvrNativeNowPose np{};
    if (!nativeFrameNowPose(np) || np.ringCount < 2) return;
    const uint32_t i0 = np.ringNewest % 8u, i1 = (np.ringNewest + 7u) % 8u;
    if (np.ringQpc[i0] <= np.ringQpc[i1]) return;
    const double dt = static_cast<double>(f30QpcUs(np.ringQpc[i0] - np.ringQpc[i1])) * 1e-6;
    if (!(dt > 0.0)) return;
    const double dps = nofoot::rotDeltaDeg(np.ringHead[i0], np.ringHead[i1]) / dt;
    // F50: the head's pitch and roll (seated space, y up, -z forward, rotation row-major in [4i+j]): forward's y is -R[1][2], right's y R[1][0].
    const float* R = np.ringHead[i0];
    const double kDeg = 180.0 / 3.14159265358979323846;
    g_headPitch = std::asin(std::max(-1.0, std::min(1.0, -static_cast<double>(R[6])))) * kDeg;
    g_headRoll = std::asin(std::max(-1.0, std::min(1.0, static_cast<double>(R[4])))) * kDeg;
    g_headPoseOk = true;
    g_headDps = dps;
    ++g_win.headN;
    g_win.headSum += dps;
    if (dps > g_win.headMax) g_win.headMax = dps;
}

bool poolWatched(const void* res) {
    for (const Pool& p : g_pools)
        if (p.res && p.res == res) return true;
    return false;
}
// The pool has a find this frame or the last.
bool poolHasFinds(const void* res, uint32_t frame) {
    for (int s = 0; s < 2; ++s)
        for (uint32_t i = 0; i < g_findN[s]; ++i)
            if (g_finds[s][i].pool == res && frame - g_finds[s][i].frame <= 1u) return true;
    return false;
}

// The draw's VS t33 view: the resource (identity), FirstElement and whether it is a stride-336 pool. Asked of the context (the binding
// shadow's t33 is not kept while the AA is off); a view seen before this frame is not described again.
bool t33Of(ID3D11DeviceContext* ctx, uint32_t frame, SrvInfo& out) {
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->VSGetShaderResources(33, 1, &srv);
    if (!srv) return false;
    if (g_srvCacheFrame != frame) {
        g_srvCacheFrame = frame;
        g_srvCacheN = 0;
    }
    for (uint32_t i = 0; i < g_srvCacheN; ++i)
        if (g_srvCache[i].srv == srv) {
            out = g_srvCache[i];
            srv->Release();
            return true;
        }
    SrvInfo info;
    info.srv = srv;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    srv->GetDesc(&vd);
    bool bufferView = true;
    if (vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER) info.first = vd.Buffer.FirstElement;
    else if (vd.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) info.first = vd.BufferEx.FirstElement;
    else bufferView = false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    srv->Release();
    ID3D11Buffer* buf = nullptr;
    if (res) res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buf));
    D3D11_BUFFER_DESC pd{};
    if (buf) {
        buf->GetDesc(&pd);
        buf->Release();
    }
    if (res) res->Release();   // the identity is kept, never a reference
    info.res = res;
    info.bytes = pd.ByteWidth;
    info.pool = buf && bufferView && pd.StructureByteStride == drawtag::kRecordStride && (pd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED);
    if (g_srvCacheN < kSrvCache) g_srvCache[g_srvCacheN++] = info;
    out = info;
    return true;
}

// The draw's eye by its depth view (0 or 1), -1 when the view is not a scene eye's; asked once per depth view a frame.
int eyeOfDraw(uint32_t frame) {
    auto* dsv = static_cast<ID3D11DepthStencilView*>(bindingGet(BindSlot::Dsv0));
    if (!dsv) return -1;
    if (dsv == g_eyeDsv && g_eyeDsvFrame == frame) return g_eyeDsvEye;
    int eye = -1, target = -1;
    if (!depthProbeCurrentSceneEyeOf(dsv, &eye, &target) || (eye != 0 && eye != 1)) eye = -1;
    g_eyeDsv = dsv;
    g_eyeDsvFrame = frame;
    g_eyeDsvEye = eye;
    return eye;
}

// F46: the world view for (frame, eye), or null; an eye-frame's first view stands.
const WorldView* worldGet(uint32_t frame, int eye) {
    for (const WorldView& v : g_worldViews)
        if (v.valid && v.frame == frame && v.eye == eye) return &v;
    return nullptr;
}
void worldPut(const WorldView& v) {
    if (worldGet(v.frame, v.eye)) return;
    g_worldViews[g_worldViewNext] = v;
    g_worldViews[g_worldViewNext].valid = true;
    g_worldViewNext = (g_worldViewNext + 1) % kWorldViews;
}

// F46: a game target's eye, the UI layer's: its redirect mapping ('L'), else ui depth's frame table, read only ('T'), else the last answer
// either gave for the same target ('C'); -1 when none has one. Nothing here registers a target or changes what the layer decides.
int targetEye(const void* res, char* src) {
    *src = 0;
    if (!res) return -1;
    char s = 'L';
    int eye = uiLayerEyeOfGameTarget(res);
    if (eye != 0 && eye != 1) {
        s = 'T';
        eye = uiDepthEyeOfTargetReadOnly(res);
    }
    if (eye == 0 || eye == 1) {
        TargetEye* slot = nullptr;
        for (TargetEye& t : g_targetEyes)
            if (t.res == res) slot = &t;
        if (!slot) {
            slot = &g_targetEyes[g_targetEyeNext];
            g_targetEyeNext = (g_targetEyeNext + 1) % kTargets;
        }
        slot->res = res;
        slot->eye = eye;
        *src = s;
        return eye;
    }
    for (const TargetEye& t : g_targetEyes)
        if (t.res == res) {
            *src = 'C';
            return t.eye;
        }
    return -1;
}

// The frame's record list for an open staging slot (emptied at the slot's first record of a frame).
std::vector<Rec>& recsOf(int slot, uint32_t frame) {
    if (g_recFrame[slot] != frame) {
        g_recFrame[slot] = frame;
        g_recs[slot].clear();
    }
    return g_recs[slot];
}

// An on-foot HUD draw names the pool (the record layout the HUD VS reads); nothing is copied.
void hudDraw(ID3D11DeviceContext* ctx, uint32_t frame) {
    ++g_win.scanHud;
    ++g_win.hudDraws;
    SrvInfo t33;
    if (!t33Of(ctx, frame, t33)) refuse(kNoT33);
    else if (!t33.pool) refuse(kStride);
    else learnPool(t33.res, t33.bytes, frame);
}

// F53: a census frame's world draw: its cb1 rows 270..273 from engine motion's CB shadow (a whole-buffer bind only), kept per eye by (vs, ps,
// rows) with a draw count. No GPU copy, no allocation.
void appendf(char* buf, size_t cap, size_t& len, const char* fmt, ...);   // below
void rowCensusNote(ID3D11DeviceContext* ctx, int e, uint64_t vs, uint64_t ps, bool scene) {
    RowCensus& c = g_rowCensus;
    if (++c.draws > kRowCensusDraws) {
        ++c.capped;
        return;
    }
    ID3D11Buffer* cb1 = nullptr;
    UINT f1 = 0;
    vsConstants(ctx, 1, &cb1, &f1);
    float shadow[6][4] = {};
    const bool ok = cb1 && f1 == 0 && engineVelocityShadowSceneRows(cb1, shadow);
    if (cb1) cb1->Release();
    if (!ok) {
        ++c.noRows;
        return;
    }
    for (uint32_t i = 0; i < c.n[e]; ++i) {
        RowSet& s = c.sets[e][i];
        if (s.vs == vs && s.ps == ps && std::memcmp(s.rows, shadow, sizeof(s.rows)) == 0) {
            ++s.n;
            return;
        }
    }
    if (c.n[e] >= kRowSets) {
        ++c.full[e];
        return;
    }
    RowSet& s = c.sets[e][c.n[e]++];
    s.vs = vs;
    s.ps = ps;
    s.n = 1;
    s.scene = scene;
    std::memcpy(s.rows, shadow, sizeof(s.rows));
}
// F53: a census frame's lines, one per eye that saw a world draw: each distinct (vs, ps, rows) with its draws, the content it carries (sets
// with the same rows share one), and its forward and rotation angles against the rows the eye's scene reference chose that frame.
void rowCensusFlush() {
    RowCensus& c = g_rowCensus;
    using ull = unsigned long long;
    ++g_win.rowCensusFrames;
    for (int e = 0; e < 2; ++e) {
        if (!c.n[e]) continue;
        const CpuWorld& ref = g_cpuWorld[e];
        const bool haveRef = ref.frame == c.frame;
        int contentOf[kRowSets];
        uint32_t distinct = 0;
        for (uint32_t i = 0; i < c.n[e]; ++i) {
            contentOf[i] = -1;
            for (uint32_t j = 0; j < i && contentOf[i] < 0; ++j)
                if (std::memcmp(c.sets[e][i].rows, c.sets[e][j].rows, sizeof(c.sets[e][i].rows)) == 0) contentOf[i] = contentOf[j];
            if (contentOf[i] < 0) contentOf[i] = static_cast<int>(distinct++);
        }
        double fwdMax = 0.0;
        for (uint32_t i = 0; i < c.n[e]; ++i)
            for (uint32_t j = i + 1; j < c.n[e]; ++j) {
                if (contentOf[i] == contentOf[j]) continue;
                double fwd = -1.0, rot = -1.0;
                drawtag::rowsBasisAngles(c.sets[e][i].rows, c.sets[e][j].rows, &fwd, &rot);
                if (fwd > fwdMax) fwdMax = fwd;
            }
        if (distinct > g_win.rowDistinctMax[e]) g_win.rowDistinctMax[e] = distinct;
        if (fwdMax > g_win.rowFwdMax[e]) g_win.rowFwdMax[e] = fwdMax;
        char buf[3072];
        size_t len = 0;
        appendf(buf, sizeof(buf), len, "native on foot marker probe (F53): frame %u eye %d cb1 rows 270..273 census: %u distinct contents over %u (vs, ps) "
                "sets, widest forward angle between two contents %.3f deg; the eye's chosen scene rows: %s:", c.frame, e, distinct, c.n[e], fwdMax,
                haveRef ? "this frame's" : "none this frame (no main-scene draw gave them from the CB shadow)");
        for (uint32_t i = 0; i < c.n[e]; ++i) {
            const RowSet& s = c.sets[e][i];
            appendf(buf, sizeof(buf), len, " [content %d: vs %016llX ps %016llX%s, %llu draws", contentOf[i], ull(s.vs), ull(s.ps),
                    s.scene ? " (scene reference eligible)" : "", ull(s.n));
            if (haveRef) {
                double fwd = -1.0, rot = -1.0;
                drawtag::rowsBasisAngles(s.rows, ref.rows, &fwd, &rot);
                const bool chosen = std::memcmp(s.rows, ref.rows, sizeof(s.rows)) == 0;
                appendf(buf, sizeof(buf), len, "; %sagainst the chosen rows: forward %.3f deg, rotation %.3f deg", chosen ? "CHOSEN; " : "", fwd, rot);
            }
            appendf(buf, sizeof(buf), len, "]");
        }
        appendf(buf, sizeof(buf), len, "; world draws looked at %llu (capped at %u: %llu), no CPU rows (a ranged b1 or no CB shadow) %llu, sets past "
                "the table %llu", ull(c.draws), kRowCensusDraws, ull(c.capped), ull(c.noRows), ull(c.full[e]));
        Log::get().note("%s", buf);
    }
}
// F53: at each frame's turn: the census frame that ended is logged; a new census frame starts while this second has census frames left.
void rowCensusTurn(uint32_t frame) {
    RowCensus& c = g_rowCensus;
    if (c.frame == frame) return;
    if (c.active) rowCensusFlush();
    if (g_rowCensusLeft > 0) {
        --g_rowCensusLeft;
        c = RowCensus{};
        c.active = true;
    } else {
        c.active = false;
    }
    c.frame = frame;
}

// F46: a world pool draw (render thread, the game's bindings): its target's eye (the UI layer's; else ui depth's rule over the frame's
// eye-sized world targets), then b1 rows 270..275 once per eye a frame -- the CB shadow's when it holds them, else one 96-byte GPU copy.
// F53: only a main-scene pass gives the eye's rows (drawtag::sceneRefWhy: not the first-person body's PS, the scene depth view, a scene
// geometry VS); a census frame also notes every world draw's cb1 rows (rowCensusNote).
void worldDraw(ID3D11DeviceContext* ctx, uint32_t frame, uint64_t vs, bool census) {
    ++g_win.scanWorld;
    ++g_win.worldDraws;
    void* view = bindingGet(BindSlot::Rtv0);
    if (!view) {
        ++g_win.worldNoTarget;
        return;
    }
    if (g_wtFrame != frame) {
        g_wtFrame = frame;
        g_wtN = 0;
    }
    WorldTarget* t = nullptr;
    for (uint32_t i = 0; i < g_wtN && !t; ++i)
        if (g_wt[i].view == view) t = &g_wt[i];
    if (!t) {
        if (g_wtN >= kTargets) {
            ++g_win.worldNoEye;
            return;
        }
        t = &g_wt[g_wtN++];
        *t = WorldTarget{};
        t->view = view;
        ResourceInfo ri;
        if (bindingResolveProbe(view, &ri) && ri.isTexture2D && ri.resource) {
            t->res = ri.resource;
            t->w = ri.a;
            t->h = ri.b;
            t->fmt = ri.fmt;
            t->eyeSized = vScreenIsEyeSized(ri.a, ri.b);
        }
        if (t->eyeSized) {
            for (uint32_t i = 0; i + 1 < g_wtN && !t->dup; ++i)
                if (g_wt[i].res == t->res) {
                    t->dup = true;
                    t->eye = g_wt[i].eye;
                    t->src = g_wt[i].src;
                }
            if (!t->dup) {
                t->eye = targetEye(t->res, &t->src);
                if (t->eye < 0) {
                    // ui depth's own rule (eyeIndexFor) over the frame's eye-sized world targets: the first of a shape is eye 0, the second 1.
                    uint32_t ofShape = 0;
                    for (uint32_t i = 0; i + 1 < g_wtN; ++i) {
                        const WorldTarget& o = g_wt[i];
                        if (o.eyeSized && !o.dup && o.w == t->w && o.h == t->h && o.fmt == t->fmt) ++ofShape;
                    }
                    if (ofShape < 2) {
                        t->eye = static_cast<int>(ofShape);
                        t->src = 'O';
                    }
                }
            }
        }
    }
    if (!t->eyeSized) {
        ++g_win.worldNotEyeSized;
        return;
    }
    if (t->eye != 0 && t->eye != 1) {
        ++g_win.worldNoEye;
        return;
    }
    const int e = t->eye;
    const uint64_t ps = bindingShaderHash(BindSlot::Ps);
    const int sceneWhy = drawtag::sceneRefWhy(vs, ps, eyeOfDraw(frame) >= 0);
    if (census) rowCensusNote(ctx, e, vs, ps, sceneWhy == drawtag::kSceneOk);
    if (g_worldOk[e] == frame) return;
    if (sceneWhy != drawtag::kSceneOk) {
        ++g_win.sceneRefused[sceneWhy];
        return;
    }
    if (t->tries >= kWorldTriesPerEye) {
        ++g_win.worldTriesCapped;
        return;
    }
    ++t->tries;
    ID3D11Buffer* cb1 = nullptr;
    UINT f1 = 0;
    vsConstants(ctx, 1, &cb1, &f1);
    float shadow[6][4] = {};
    bool ok = false;
    if (cb1 && f1 == 0 && engineVelocityShadowSceneRows(cb1, shadow)) {
        WorldView v;
        v.frame = frame;
        v.eye = e;
        std::memcpy(v.rows, shadow, sizeof(v.rows));
        std::memcpy(v.o, shadow[5], sizeof(v.o));
        worldPut(v);
        // F47: the eye's world view is known on the CPU this frame (the correction's input at the marker composite's draw).
        g_cpuWorld[e].frame = frame;
        std::memcpy(g_cpuWorld[e].rows, v.rows, sizeof(v.rows));
        std::memcpy(g_cpuWorld[e].o, v.o, sizeof(v.o));
        ++g_win.worldShadow;
        ok = true;
    } else if (!covers(cb1, f1 * 16 + kWorldOffset, kWorldBytes)) {
        ++g_win.worldNoRows;
        D3D11_BUFFER_DESC d{};
        if (cb1) cb1->GetDesc(&d);
        g_win.worldCb1Bound = cb1 != nullptr;
        g_win.worldCb1Bytes = d.ByteWidth;
        g_win.worldF1 = f1;
        if (!g_worldRowsNoted) {
            g_worldRowsNoted = true;
            Log::get().note("native on foot marker probe: a world draw's b1 does not cover rows 270..275 (vs %016llX: b1 %s, ByteWidth %u, first "
                            "constant %u; the rows need bytes %u..%u past the first constant). Said once; counted in the 1 s lines",
                            static_cast<unsigned long long>(vs), cb1 ? "bound" : "NOT bound", d.ByteWidth, f1, kWorldOffset,
                            kWorldOffset + kWorldBytes);
        }
    } else {
        const int slot = g_staging.open(ctx, frame);
        const int64_t at = slot < 0 ? -1 : g_staging.append(ctx, cb1, f1 * 16 + kWorldOffset, kWorldBytes);
        if (slot < 0) refuse(kNoSlot);
        else if (at < 0) refuse(kBufferFull);
        if (at < 0) {
            ++g_win.worldStageFail;
        } else {
            Rec r;
            r.vs = vs;
            r.ps = bindingShaderHash(BindSlot::Ps);
            r.worldAt = static_cast<int32_t>(at);
            r.eye = static_cast<int8_t>(e);
            r.eyeSrc = t->src;
            r.flags = kRecWorld;
            recsOf(slot, frame).push_back(r);
            ++g_win.recs;
            g_win.stagedBytes += kWorldBytes;
            ++g_win.worldStaged;
            ok = true;
        }
    }
    if (cb1) cb1->Release();   // identity only from here
    if (!ok) return;
    g_worldOk[e] = frame;
    // F52 item A: which draw gave the eye's world rows: its shaders, target, depth view, viewport and b1 (once per eye a frame).
    {
        WorldSrc& ws = g_win.wsrc[e];
        ++ws.n;
        if (g_lastWorldVs[e] && g_lastWorldVs[e] != vs) ++ws.vsChanges;
        g_lastWorldVs[e] = vs;
        ws.vs = vs;
        ws.ps = bindingShaderHash(BindSlot::Ps);
        ws.w = t->w;
        ws.h = t->h;
        ws.dsv = bindingGet(BindSlot::Dsv0);
        ws.cb1 = cb1;
        g_worldCb1[e] = cb1;
        D3D11_VIEWPORT vp{};
        UINT nvp = 1;
        ctx->RSGetViewports(&nvp, &vp);
        ws.vpOk = nvp >= 1 && vp.Width > 0.0f;
        const float v6[6] = {vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth};
        std::memcpy(ws.vp, v6, sizeof(v6));
    }
    ++g_win.eye[e].worldViews;
    ++g_win.worldSrc[srcIndex(t->src)];
    const int d = eyeOfDraw(frame);   // the check only: the scene depth view's eye beside the UI layer's
    if (d < 0) ++g_win.dsvUnknown;
    else if (d == e) ++g_win.dsvAgree;
    else ++g_win.dsvDisagree;
}

// F47: the CPU shadow of the b0 `res` (learning it when `learn`), or null.
CbShadow* shadowFor(const void* res, bool learn, uint32_t bytes) {
    if (!res) return nullptr;
    for (CbShadow& s : g_cbShadow)
        if (s.res == res) return &s;
    if (!learn) return nullptr;
    CbShadow& s = g_cbShadow[g_cbShadowNext];
    g_cbShadowNext = (g_cbShadowNext + 1) % kCbShadows;
    s = CbShadow{};
    s.res = res;
    s.bytes = bytes;
    return &s;
}

// F47: the bound b0 is a whole-buffer bind from constant 0 (a ranged bind is not patched: its restore would need VSSetConstantBuffers1).
bool vsB0Plain(ID3D11DeviceContext* ctx, ID3D11Buffer** buf) {
    *buf = nullptr;
    UINT first = 0, num = 0;
    ID3D11DeviceContext1* c1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) && c1) {
        c1->VSGetConstantBuffers1(0, 1, buf, &first, &num);
        c1->Release();
    } else {
        ctx->VSGetConstantBuffers(0, 1, buf);
        return *buf != nullptr;
    }
    if (!*buf) return false;
    D3D11_BUFFER_DESC d{};
    (*buf)->GetDesc(&d);
    // A plain bind reads back as constant 0 with all 4096 constants (or the buffer's own count).
    if (first == 0 && (num == 0 || num >= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT || num * 16u >= d.ByteWidth)) return true;
    (*buf)->Release();
    *buf = nullptr;
    return false;
}

// F47: the newest find of `key` with P, this frame or the last.
const Find* findForKey(uint32_t key, uint32_t frame) {
    const Find* best = nullptr;
    for (int s = 0; s < 2; ++s)
        for (uint32_t i = 0; i < g_findN[s]; ++i) {
            const Find& f = g_finds[s][i];
            if (f.key != key || !f.haveTag || !f.pOk || frame - f.frame > 1u) continue;
            if (!best || f.frame > best->frame) best = &f;
        }
    return best;
}
bool matchFresh(const EyeMatch& m, uint32_t frame) {
    return m.frame != ~0u && frame - m.frame <= kMatchFresh;
}

// F52: the temporal reference by (frame, eye) for the read-back's residual; an eye-frame's first stands.
const WorldView* refGet(uint32_t frame, int eye) {
    for (const WorldView& v : g_refViews)
        if (v.valid && v.frame == frame && v.eye == eye) return &v;
    return nullptr;
}
void refPut(uint32_t frame, int eye, const float rows[16], const float o[4]) {
    if (refGet(frame, eye)) return;
    WorldView& v = g_refViews[g_refViewNext];
    g_refViewNext = (g_refViewNext + 1) % kWorldViews;
    v.valid = true;
    v.frame = frame;
    v.eye = eye;
    std::memcpy(v.rows, rows, sizeof(v.rows));
    std::memcpy(v.o, o, sizeof(v.o));
}
// F52: a key slot for `key` (acquired at read-back; a reclaimed slot starts clean).
int keySlotAcquire(uint32_t key, uint32_t frame) {
    bool fresh = false;
    const int i = g_keyIdx.acquire(key, frame, &fresh);
    if (fresh) {
        g_keys[i] = KeyState{};
        g_win.keys[i] = KeyWin{};
        ++g_win.keyReclaims;
    }
    return i;
}

// F52 item A: once per eye a frame (at its first marker composite draw, after the scene): the eye's world rows (the CPU's, g_cpuWorld)
// against the temporal pass's camera rows for the eye, in degrees and in pixels at the newest P; the temporal reference built from them
// (the world rows' projection and eye, the temporal rotation); and the reference in use, a change of which starts a new epoch for the eye
// (every key's baseline of the eye resets on it, as at an eye-assignment change). W, H: the marker target's size.
void refTick(int e, uint32_t frame, double W, double H) {
    if (g_refFrame[e] == frame) return;
    g_refFrame[e] = frame;
    RefCmp& rc = g_win.ref[e];
    ++rc.ticks;
    float cam[12];
    const void* res = nullptr;
    uint64_t tvs = 0;
    const bool haveT = temporalPassEyeRows(e, cam, &res, &tvs);
    const CpuWorld& w = g_cpuWorld[e];
    bool usable = false;
    if (!haveT) {
        ++rc.noTemporal;
    } else if (w.frame != frame) {
        ++rc.noWorld;
    } else {
        rc.tvs = tvs;
        if (res && res == g_worldCb1[e]) ++rc.sameBuf;
        else ++rc.otherBuf;
        double rW[3], uW[3], fW[3], rT[3], uT[3], fT[3];
        int sign = 0;
        float swapped[16];
        if (!drawtag::worldBasis(w.rows, rW, uW, fW) || !drawtag::cameraBasis(cam, fW, rT, uT, fT, &sign) ||
            !drawtag::swapRotation(w.rows, rW, uW, fW, rT, uT, fT, swapped)) {
            ++rc.degenerate;
        } else {
            const drawtag::BasisDiff d = drawtag::basisDiff(rW, uW, fW, rT, uT, fT);
            rc.last = d;
            rc.sign = sign;
            if (!d.sameHand || !(d.right < 90.0) || !(d.up < 90.0)) {
                ++rc.convention;   // the axes do not correspond (a mirrored or swapped convention): not usable as a reference; F53: untrusted
                if (g_axisNotes < kAxisNotes) {
                    ++g_axisNotes;
                    Log::get().note("native on foot marker probe (F53): eye %d frame %u: the temporal rows' axes do not correspond to the scene rows' "
                                    "(this frame is untrusted): scene right (%.3f, %.3f, %.3f) up (%.3f, %.3f, %.3f) forward (%.3f, %.3f, %.3f); "
                                    "temporal right (%.3f, %.3f, %.3f) up (%.3f, %.3f, %.3f) forward (%.3f, %.3f, %.3f), forward sign %d; angles right "
                                    "%.1f, up %.1f, forward %.1f deg, same handedness %s; temporal vs %016llX, its b1 %s the scene draw's",
                                    e, frame, rW[0], rW[1], rW[2], uW[0], uW[1], uW[2], fW[0], fW[1], fW[2], rT[0], rT[1], rT[2], uT[0], uT[1], uT[2],
                                    fT[0], fT[1], fT[2], sign, d.right, d.up, d.fwd, d.sameHand ? "yes" : "no", static_cast<unsigned long long>(tvs),
                                    res && res == g_worldCb1[e] ? "is" : "is NOT");
                }
            } else {
                usable = true;
                ++rc.n;
                rc.degSum += d.total;
                if (d.total > rc.degMax) rc.degMax = d.total;
                g_tempRef[e].frame = frame;
                std::memcpy(g_tempRef[e].rows, swapped, sizeof(swapped));
                std::memcpy(g_tempRef[e].o, w.o, sizeof(w.o));
                refPut(frame, e, swapped, w.o);
                // F53: the trust check: P through the scene rows and through the temporal reference must agree under 1 px.
                bool compared = false;
                double px = -1.0;
                if (g_haveLastFind && g_lastFind.haveTag && g_lastFind.pOk) {
                    const double P[3] = {g_lastFind.p[0], g_lastFind.p[1], g_lastFind.p[2]}, ow[3] = {w.o[0], w.o[1], w.o[2]};
                    double a[4], b[4];
                    drawtag::worldClip(w.rows, P, ow, a);
                    drawtag::worldClip(swapped, P, ow, b);
                    if (a[3] > 1e-9 && b[3] > 1e-9) {
                        const double dx = (b[0] / b[3] - a[0] / a[3]) * W * 0.5, dy = (b[1] / b[3] - a[1] / a[3]) * H * 0.5;
                        px = std::sqrt(dx * dx + dy * dy);
                        compared = true;
                        ++rc.pxN;
                        rc.pxSum += px;
                        if (px > rc.pxMax) rc.pxMax = px;
                    }
                } else {
                    ++rc.noP;
                }
                if (drawtag::sceneRowsTrusted(compared, px)) {
                    ++rc.trusted;
                    g_sceneTrusted[e] = frame;
                } else if (compared) {
                    ++rc.overPx;
                }
            }
        }
    }
    // F53: the eye dump's marker crop centre: P through the eye's scene rows this frame (relaxed atomics; the temporal pass reads them).
    if (w.frame == frame && g_haveLastFind && g_lastFind.haveTag && g_lastFind.pOk) {
        const double P[3] = {g_lastFind.p[0], g_lastFind.p[1], g_lastFind.p[2]}, ow[3] = {w.o[0], w.o[1], w.o[2]};
        double a[4];
        drawtag::worldClip(w.rows, P, ow, a);
        if (a[3] > 1e-9) {
            g_markerCropNdc[e].p[0].store(static_cast<float>(a[0] / a[3]), std::memory_order_relaxed);
            g_markerCropNdc[e].p[1].store(static_cast<float>(a[1] / a[3]), std::memory_order_relaxed);
            g_markerCropNdc[e].pMs.store(GetTickCount64(), std::memory_order_relaxed);
        }
    }
    if (usable) g_tempSeen[e] = frame;
    const bool recent = g_tempSeen[e] != ~0u && frame - g_tempSeen[e] <= kRefFreshFrames;
    const int eff = drawtag::chooseReference(g_nativeOnFootMarkerRefKey.load(std::memory_order_relaxed), recent);
    if (eff != g_refEff[e]) {
        if (g_refEff[e] >= 0) {
            ++g_refEpoch[e];
            ++rc.switches;
        }
        if (g_refNotes < kRefNotes) {
            ++g_refNotes;
            Log::get().note("native on foot marker fix (F52): eye %d measures against %s (asked %s; the temporal rows %s)", e,
                            eff == drawtag::kRefTemporal ? "temporal (the temporal pass's camera rotation at the world rows' projection)" :
                                                           "world_first (the eye's first world pool draw's b1 rows 270..273)",
                            g_nativeOnFootMarkerRefKey.load(std::memory_order_relaxed) == drawtag::kRefWorldFirst ? "world_first" : "temporal",
                            recent ? "are available" : "are not available (the temporal pass is off, saw no scene draw for the eye, or its axes do not correspond)");
        }
        g_refEff[e] = eff;
    }
}

// F47: the correction's decision at a marker composite draw (the tee, the game's bindings): sets g_fix.pending for this draw, or counts why
// not. Nothing is bound here; Begin binds. F52: per key: the draw is the one whose place in the eye a key's fresh read-back match names; that
// key must be matched in both eyes; its own baseline, last good delta and find are used; the eye's reference is world_first or temporal.
void fixDecide(ID3D11DeviceContext* ctx, uint32_t frame, const Rec& r, int slot, size_t recIndex, ID3D11Buffer* b0Plain, const float* oHud,
               const CbShadow* sh) {
    if (r.eye != 0 && r.eye != 1) {
        ++g_win.fixNoEye;
        return;
    }
    const int e = r.eye;
    EyeStats& s = g_win.eye[e];
    int ks = -1;
    bool anyFresh = false;
    for (uint32_t i = 0; i < kKeySlots && ks < 0; ++i) {
        if (!g_keyIdx.used[i] || !matchFresh(g_keys[i].eye[e].match, frame)) continue;
        anyFresh = true;
        if (g_keys[i].eye[e].match.ord == r.ordEye) ks = static_cast<int>(i);
    }
    if (ks < 0 || !matchFresh(g_keys[ks].eye[1 - e].match, frame)) {
        const int u = ks < 0 ? e : 1 - e;
        if (ks < 0 && anyFresh) {
            ++s.fOtherDraw;   // no key's match names this draw (an untagged marker composite draw)
            return;
        }
        if (ks >= 0) ++g_win.keys[ks].otherUnmatched[e];
        ++s.fUnmatched[u];
        if (!g_unmatchedNoted[u]) {
            g_unmatchedNoted[u] = true;
            Log::get().note("native on foot marker fix: eye %d unmatched: not applied (no marker composite draw of eye %d read the marker "
                            "record in the last %u frames' read-backs; both eyes must match, per key). Said once; counted in the 1 s lines", u, u,
                            kMatchFresh);
        }
        return;
    }
    KeyState& K = g_keys[ks];
    KeyWin& kw = g_win.keys[ks];
    const EyeMatch& match = K.eye[e].match;
    ++kw.decided[e];
    if (!b0Plain) {
        ++s.fRanged;
        return;
    }
    if (!sh || !sh->valid) {
        ++s.fNoRows;
        return;
    }
    if (!oHud) {
        ++s.fNoOrigin;
        return;
    }
    double dx = 0.0, dy = 0.0;
    bool have = false, held = false;
    // F52: the eye's reference (refTick ran at this eye's first marker composite draw of the frame): world_first or temporal.
    const int ref = g_refEff[e] == drawtag::kRefTemporal ? drawtag::kRefTemporal : drawtag::kRefWorldFirst;
    const CpuWorld& w = ref == drawtag::kRefTemporal ? g_tempRef[e] : g_cpuWorld[e];
    ++g_win.ref[e].decided[ref];
    const Find* f = findForKey(match.key, frame);
    int why = -1;
    // F50: the eye's baseline, reset when the frustum it was taken under changes (b0 row 5 or row 7 scale, the target's size) or the eye's
    // matched draw moves to another place (the eye assignment). F52: one per key per eye; also reset when the eye's reference changes.
    LagEye& L = K.eye[e].lag;
    Good& good = K.eye[e].good;
    const double W = r.tw ? static_cast<double>(r.tw) : kDefaultTargetSize, H = r.th ? static_cast<double>(r.th) : kDefaultTargetSize;
    {
        const auto norm3 = [](const float* v) {
            return std::sqrt(static_cast<double>(v[0]) * v[0] + static_cast<double>(v[1]) * v[1] + static_cast<double>(v[2]) * v[2]);
        };
        const double r5n = norm3(sh->rows[5]), r7n = norm3(sh->rows[7]);
        const auto moved = [](double a, double b) { return std::fabs(a - b) > 1e-3 * std::max(std::fabs(a), std::fabs(b)); };
        const char* why0 = nullptr;
        if (L.keyed) {
            if (moved(r5n, L.r5n) || moved(r7n, L.r7n)) why0 = "the marker's b0 row 5 or row 7 scale changed (a frustum change)";
            else if (r.tw != L.tw || r.th != L.th) why0 = "the marker target's size changed (a render-size change)";
            else if (match.ord != L.ord) why0 = "the eye's matched marker composite draw moved (an eye-assignment change)";
            else if (g_refEpoch[e] != L.refEpoch) why0 = "the eye's reference changed (world_first <-> temporal; an eye-assignment-style reset)";
        }
        if (why0) {
            L.b.reset();
            L.clearBins();
            L.lastReset = why0;
            ++s.lagResets;
            ++kw.resets[e];
            if (L.resetNotes < 8) {
                ++L.resetNotes;
                Log::get().note("native on foot marker fix (F50/F52): key %u eye %d baseline reset: %s (b0 row 5 %.5f -> %.5f, row 7 %.5f -> %.5f, "
                                "target %ux%u -> %ux%u, draw place %u -> %u, reference epoch %u -> %u)", match.key, e, why0, L.r5n, r5n, L.r7n, r7n,
                                L.tw, L.th, r.tw, r.th, static_cast<unsigned>(L.ord), static_cast<unsigned>(match.ord), L.refEpoch, g_refEpoch[e]);
            }
        }
        L.keyed = true;
        L.r5n = r5n;
        L.r7n = r7n;
        L.tw = r.tw;
        L.th = r.th;
        L.ord = match.ord;
        L.refEpoch = g_refEpoch[e];
    }
    if (w.frame != frame) {
        ++s.fNoWorld;
    } else if (!f) {
        ++s.fNoP;
    } else {
        const double ow[3] = {w.o[0], w.o[1], w.o[2]}, oh[3] = {oHud[0], oHud[1], oHud[2]}, P[3] = {f->p[0], f->p[1], f->p[2]};
        const double X0[3] = {f->x0[0], f->x0[1], f->x0[2]};
        // F53: the target is ndc(P through the eye's scene rows) + e, e the fixed icon offset (drawtag::iconOffsetNdc); the learned baseline
        // (F50) is dropped from the decision and kept, updated below, for the log only.
        double c[2];
        drawtag::iconOffsetNdc(H, c);
        double lag[2] = {};
        why = drawtag::ndcLagDelta(w.rows, ow, sh->rows[4], sh->rows[5], sh->rows[7], oh, X0, P, c, kGateNdc, lag, &dx, &dy);
        if ((why == drawtag::kDeltaOk || why == drawtag::kDeltaGate) && L.lastFrame != frame) {
            L.lastFrame = frame;
            // |m - c| by the head's speed (against the c the delta used: F53 e), the baseline's update (logged only), the honesty bins (every
            // slow-head frame; reported against e).
            const int bin = g_headDps < 0.0 ? -1 : g_headDps < 3.0 ? 0 : g_headDps < 20.0 ? 1 : g_headDps < 60.0 ? 2 : 3;
            if (bin >= 0) {
                const double ex = (lag[0] - c[0]) * W * 0.5, ey = (lag[1] - c[1]) * H * 0.5, px = std::sqrt(ex * ex + ey * ey);
                ++s.lagN[bin];
                s.lagSum[bin] += px;
                if (px > s.lagMax[bin]) s.lagMax[bin] = px;
                ++kw.lagN[e][bin];
                kw.lagSum[e][bin] += px;
                if (px > kw.lagMax[e][bin]) kw.lagMax[e][bin] = px;
            }
            bool restarted = false;
            if (L.b.update(lag, g_headDps, &restarted)) ++s.lagTaken;
            if (restarted) {
                L.clearBins();
                L.lastReset = "90 slow-head frames in a row were over 0.01 NDC from c (the seed starts again)";
                ++s.lagResets;
                ++kw.resets[e];
            }
            if (bin == 0 && g_headPoseOk) {
                const int pb = g_headPitch < -15.0 ? 0 : g_headPitch <= 15.0 ? 1 : 2, rb = std::fabs(g_headRoll) < 10.0 ? 0 : 1;
                ++L.pitchN[pb];
                ++L.rollN[rb];
                for (int k = 0; k < 2; ++k) {
                    L.pitchSum[pb][k] += lag[k];
                    L.rollSum[rb][k] += lag[k];
                }
            }
        }
        if (g_sceneTrusted[e] != frame) {
            ++s.fUntrusted;
            ++kw.untrusted[e];
            return;   // F53: no patch and no hold unless the eye's scene rows agree with the temporal pass under 1 px this frame
        }
        if (why == drawtag::kDeltaOk) have = true;
        else if (why == drawtag::kDeltaGate) {
            ++s.fGate;
            ++kw.gate[e];
        } else {
            ++s.fBehind;
        }
    }
    if (have) {
        good.frame = frame;
        good.dx = dx;
        good.dy = dy;
    } else if (good.frame != ~0u && good.frame + 1u == frame && g_sceneTrusted[e] == frame) {
        dx = good.dx;   // the last good delta, held for one frame (F53: only while the eye's scene rows are trusted)
        dy = good.dy;
        have = held = true;
    }
    if (!have) return;
    float r4[4], r5[4];
    std::memcpy(r4, sh->rows[4], sizeof(r4));
    std::memcpy(r5, sh->rows[5], sizeof(r5));
    drawtag::ndcOffsetRows(r4, r5, sh->rows[7], dx, dy);
    double eOff[2];
    drawtag::iconOffsetNdc(H, eOff);   // F53: the c the delta was taken against is e
    g_fix.cx = eOff[0];
    g_fix.cy = eOff[1];
    g_fix.head = g_headDps;
    g_fix.keySlot = ks;
    g_fix.ref = ref;
    g_fix.pending = true;
    g_fix.held = held;
    g_fix.serial = g_drawSerial;
    g_fix.ctx = ctx;
    g_fix.cb0 = b0Plain;
    g_fix.eye = e;
    g_fix.frame = frame;
    g_fix.slot = slot;
    g_fix.rec = recIndex;
    g_fix.dx = dx;
    g_fix.dy = dy;
    std::memcpy(g_fix.rows45, r4, sizeof(r4));
    std::memcpy(g_fix.rows45 + 4, r5, sizeof(r5));
}

// F46: a marker composite draw (VS B10B032BDFD46700) bound to a watched pool, in a frame with a find in it: its instance stream, b0 rows 4..7
// and b1 row 275 are copied into the frame's staging slot, with the UI layer's eye of its target and the target's size. Nothing is written.
// F47: EVERY marker composite draw is recorded (its eye, its place in the frame, its t33 pool, watched or not) for the per-frame lines; a
// stride-336 pool it binds that is not watched yet is learned (the Map/Unmap tees then look for the tags in it too); the CPU shadow of its b0
// is learned and read; and, with fix.native_on_foot_marker_fix on, the correction is decided for this draw (fixDecide).
void markerDraw(ID3D11DeviceContext* ctx, uint32_t frame, uint64_t vs, uint32_t instances, uint32_t startInstance, char kind) {
    ++g_win.scanMarker;
    ++g_win.b10bDraws;
    if (g_ordFrame != frame) {
        g_ordFrame = frame;
        g_ordEye[0] = g_ordEye[1] = 0;
        g_ordAll = 0;
    }
    Rec r;
    r.vs = vs;
    r.ps = bindingShaderHash(BindSlot::Ps);
    r.instances = instances;
    r.start = startInstance;
    r.kind = kind;
    r.flags = kRecMarker;
    r.ordAll = g_ordAll++;
    if (void* view = bindingGet(BindSlot::Rtv0)) {
        ResourceInfo ri;
        if (bindingResolveProbe(view, &ri) && ri.isTexture2D && ri.resource) {
            r.tw = ri.a;
            r.th = ri.b;
            r.eye = static_cast<int8_t>(targetEye(ri.resource, &r.eyeSrc));
        }
    }
    if (r.eye == 0 || r.eye == 1) r.ordEye = g_ordEye[r.eye]++;
    SrvInfo t33;
    const bool haveT33 = t33Of(ctx, frame, t33);
    if (!haveT33) ++g_win.b10bNoT33;
    else if (!t33.pool) ++g_win.b10bNotPool;
    if (haveT33) {
        r.t33 = t33.res;
        r.first = t33.first;
    }
    if (haveT33 && t33.pool && !poolWatched(t33.res)) {
        learnPool(t33.res, t33.bytes, frame);
        ++g_win.markerLearned;
    }
    r.watched = haveT33 && t33.pool && poolWatched(t33.res);
    if (r.watched) {
        r.pool = t33.res;
        ++g_win.markerPool;
    }
    bool copy = r.watched;
    if (copy && (kind == 'Y' || kind == 'Z')) {   // indirect: the instance range is in a GPU argument buffer
        ++g_win.indirect;
        copy = false;
    }
    if (copy && !poolHasFinds(t33.res, frame)) copy = false;
    if (copy) ++g_win.markerFinds;
    r.findsNow = copy;
    ID3D11Buffer* cb0 = nullptr;
    ID3D11Buffer* cb1 = nullptr;
    UINT f0 = 0, f1 = 0;
    vsConstants(ctx, 0, &cb0, &f0);
    vsConstants(ctx, 1, &cb1, &f1);
    float shadow[6][4] = {};
    const bool shadowO = cb1 && f1 == 0 && engineVelocityShadowSceneRows(cb1, shadow);
    // F47: the CPU shadow of b0, for a bind from constant 0 (learned here; the tees write it).
    ID3D11Buffer* b0Plain = nullptr;
    CbShadow* sh = nullptr;
    if (vsB0Plain(ctx, &b0Plain)) {
        D3D11_BUFFER_DESC bd{};
        b0Plain->GetDesc(&bd);
        if (bd.ByteWidth >= kCb0ShadowBytes) sh = shadowFor(b0Plain, true, bd.ByteWidth);
    }
    if (sh && sh->valid) {
        r.cpuRows = true;
        std::memcpy(r.cpu, sh->rows[4], sizeof(r.cpu));
    }
    ID3D11Buffer* vbs[8] = {};
    UINT strides[8] = {}, offs[8] = {};
    const int slot = g_staging.open(ctx, frame);
    if (slot < 0) refuse(kNoSlot);
    bool fine = copy && slot >= 0;
    if (fine && (!covers(cb0, f0 * 16 + kMarkerRowsOffset, kMarkerRowsBytes) || (!shadowO && !covers(cb1, f1 * 16 + kOriginOffset, kOriginBytes)))) {
        refuse(kConstants);
        fine = false;
    }
    int pick = -1;
    uint64_t at = 0;
    uint32_t entries = 0;
    if (fine) {
        // The instance stream: the one vertex buffer of stride 8 (INSTANCEANDMODELDATAINDEX); every entry the draw reads.
        ctx->IAGetVertexBuffers(0, 8, vbs, strides, offs);
        unsigned eights = 0;
        for (int i = 0; i < 8; ++i)
            if (vbs[i] && strides[i] == drawtag::kStreamStride) {
                ++eights;
                if (pick < 0) pick = i;
            }
        D3D11_BUFFER_DESC sd{};
        if (pick >= 0) vbs[pick]->GetDesc(&sd);
        at = pick >= 0 ? static_cast<uint64_t>(offs[pick]) + static_cast<uint64_t>(startInstance) * drawtag::kStreamStride : 0;
        entries = instances < kEntriesMax ? instances : kEntriesMax;
        if (pick >= 0 && at < sd.ByteWidth && entries > (sd.ByteWidth - at) / drawtag::kStreamStride)
            entries = static_cast<uint32_t>((sd.ByteWidth - at) / drawtag::kStreamStride);
        if (eights != 1 || at >= sd.ByteWidth || !entries) {
            refuse(kNoStream);
            fine = false;
        }
    }
    if (fine) {
        const int64_t sAt = g_staging.append(ctx, vbs[pick], static_cast<UINT>(at), entries * drawtag::kStreamStride);
        const int64_t rAt = sAt < 0 ? -1 : g_staging.append(ctx, cb0, f0 * 16 + kMarkerRowsOffset, kMarkerRowsBytes);
        const int64_t oAt = rAt < 0 || shadowO ? -1 : g_staging.append(ctx, cb1, f1 * 16 + kOriginOffset, kOriginBytes);
        if (sAt < 0 || rAt < 0 || (!shadowO && oAt < 0)) {
            refuse(kBufferFull);
        } else {
            if (entries < instances) {
                r.flags |= kRecCut;
                ++g_win.streamCut;
            }
            r.streamAt = static_cast<int32_t>(sAt);
            r.entries = entries;
            r.rowsAt = static_cast<int32_t>(rAt);
            r.originAt = static_cast<int32_t>(oAt);
            g_win.stagedBytes += static_cast<uint64_t>(entries) * drawtag::kStreamStride + kMarkerRowsBytes + (shadowO ? 0u : kOriginBytes);
            ++g_win.streamDraws;
            g_win.streamEntries += entries;
        }
    }
    if (shadowO) {
        std::memcpy(r.origin, shadow[5], sizeof(r.origin));
        r.flags |= kRecShadowO;
    }
    // F52 item A: the eye's world rows against the temporal pass's (once per eye a frame), and the eye's reference.
    if (r.eye == 0 || r.eye == 1)
        refTick(r.eye, frame, r.tw ? static_cast<double>(r.tw) : kDefaultTargetSize, r.th ? static_cast<double>(r.th) : kDefaultTargetSize);
    if (slot >= 0) {
        std::vector<Rec>& list = recsOf(slot, frame);
        list.push_back(r);
        ++g_win.recs;
        // F47: the correction's decision for this draw (the key on only).
        if (g_nativeOnFootMarkerFixKey.load(std::memory_order_relaxed))
            fixDecide(ctx, frame, list.back(), slot, list.size() - 1, b0Plain, shadowO ? shadow[5] : nullptr, sh);
    }
    for (ID3D11Buffer* b : vbs)
        if (b) b->Release();
    if (b0Plain) b0Plain->Release();   // identity only from here (g_fix.cb0)
    if (cb0) cb0->Release();
    if (cb1) cb1->Release();
}

// The draw tee's work (render thread, the game's bindings). F46: only the on-foot HUD draws, the marker composite and the world pool draws
// (until both eyes' world views are in for the frame) are looked at past the vertex shader hash; the time spent in them is counted.
void draw(ID3D11DeviceContext* ctx, uint32_t frame, uint32_t instances, uint32_t startInstance, char kind) {
    ++g_win.hookCalls;
    const uint64_t vs = bindingShaderHash(BindSlot::Vs);
    const bool hud = nofoot::hudIssuerIndex(vs) >= 0;
    const bool marker = vs == nofoot::kMarkerCompositeVs;
    const bool census = g_rowCensus.active && g_rowCensus.frame == frame;   // F53: a census frame looks at every world draw
    if (!hud && !marker) {
        if (!census && g_worldOk[0] == frame && g_worldOk[1] == frame) return;
        if (engine_velocity_family::familyOfVs(vs) < 0) return;
    }
    ++g_win.scanned;
    const uint64_t t0 = f30Qpc();
    if (hud) hudDraw(ctx, frame);
    else if (marker) markerDraw(ctx, frame, vs, instances, startInstance, kind);
    else worldDraw(ctx, frame, vs, census);
    g_win.drawTicks += f30Qpc() - t0;
}

void addTuple(const Rec& r, const drawtag::StreamMatch& m) {
    Window& w = g_win;
    Tuple* t = nullptr;
    for (uint32_t i = 0; i < w.nTuples && !t; ++i)
        if (w.tuples[i].vs == r.vs && w.tuples[i].ps == r.ps && w.tuples[i].eye == r.eye && w.tuples[i].kind == r.kind) t = &w.tuples[i];
    if (!t) {
        if (w.nTuples >= kTuples) {
            ++w.tuplesDropped;
            return;
        }
        t = &w.tuples[w.nTuples++];
        t->vs = r.vs;
        t->ps = r.ps;
        t->eye = r.eye;
        t->kind = r.kind;
        t->instMin = t->instMax = r.instances;
    }
    ++t->draws;
    if (r.instances < t->instMin) t->instMin = r.instances;
    if (r.instances > t->instMax) t->instMax = r.instances;
    t->entry = m.entry;
    t->record = m.record;
    t->xHits = m.xHits;
    t->yHits = m.yHits;
    t->viaY = m.viaY;
}

// F46: a marker composite that read a find: ndc_marker = (r4 . p, r5 . p) / |r7 . p|, p = (X0 - o_hud, 1), against ndc_P = P through the
// world rows of the draw's eye (the UI layer's) at o_w; in pixels of the marker's target and in degrees, and against the other eye's world
// view beside it (a wrong eye shows as the smaller of the two).
// F47: also the delta of P itself through the world rows and through the marker rows (item 1), and, for a patched draw, the same through the
// private b0's rows read back after their write (item 4: about zero when the patch took).
void f47Measure(EyeStats& s, const WorldView& w, const float rows[16], const double oh[3], const double P[3], double W, double H) {
    const double ow[3] = {w.o[0], w.o[1], w.o[2]};
    double dx = 0.0, dy = 0.0;
    const int why = drawtag::ndcDelta(w.rows, ow, rows, rows + 4, rows + 12, oh, P, 1e30, &dx, &dy);
    if (why != drawtag::kDeltaOk) return;
    double wc[4], mc[3];
    drawtag::worldClip(w.rows, P, ow, wc);
    drawtag::markerClip(rows, rows + 4, rows + 12, P, oh, mc);
    const double a[2] = {wc[0] / wc[3], wc[1] / wc[3]}, b[2] = {mc[0] / mc[2], mc[1] / mc[2]};
    const double px = dx * W * 0.5, py = dy * H * 0.5;
    ++s.dN;
    s.dxSum += px;
    s.dySum += py;
    if (std::fabs(px) > s.dxMax) s.dxMax = std::fabs(px);
    if (std::fabs(py) > s.dyMax) s.dyMax = std::fabs(py);
    const double deg = drawtag::ndcAngleDeg(w.rows, a, b);
    if (deg >= 0.0) {
        ++s.dDegN;
        s.dDegSum += deg;
        if (deg > s.dDegMax) s.dDegMax = deg;
    }
}
// F50: the residual after the patch: X0 through the patched rows (read back) minus (P through the world rows + the c the patch used), in
// pixels; about zero at every head speed when the patch took (a held frame carries the frame before's delta, so it is not zero).
// F52: also per key (kw, eye e), against the reference the patch measured against (the caller's w).
void f50Residual(EyeStats& s, const WorldView& w, const float patched[16], const double oh[3], const double X0[3], const double P[3], const Rec& r,
                 double W, double H, KeyWin* kw, int e) {
    const double ow[3] = {w.o[0], w.o[1], w.o[2]}, c[2] = {r.fixCx, r.fixCy};
    double dx = 0.0, dy = 0.0, m[2] = {};
    if (drawtag::ndcLagDelta(w.rows, ow, patched, patched + 4, patched + 12, oh, X0, P, c, 1e30, m, &dx, &dy) != drawtag::kDeltaOk) return;
    const double px = -dx * W * 0.5, py = -dy * H * 0.5;   // (m - c): where the marker landed against P + c
    ++s.rN;
    s.rxSum += px;
    s.rySum += py;
    if (std::fabs(px) > s.rxMax) s.rxMax = std::fabs(px);
    if (std::fabs(py) > s.ryMax) s.ryMax = std::fabs(py);
    const double hd = r.fixHead;
    const int bin = hd < 0.0 ? -1 : hd < 3.0 ? 0 : hd < 20.0 ? 1 : hd < 60.0 ? 2 : 3;
    if (bin >= 0) {
        const double d = std::sqrt(px * px + py * py);
        ++s.resN[bin];
        s.resSum[bin] += d;
        if (d > s.resMax[bin]) s.resMax[bin] = d;
        if (kw && (e == 0 || e == 1)) {
            ++kw->resN[e][bin];
            kw->resSum[e][bin] += d;
            if (d > kw->resMax[e][bin]) kw->resMax[e][bin] = d;
        }
    }
}

void pairMarker(const Rec& r, const Find& f, const float rows[16], const float oH[4], uint32_t frame, const float* patched, KeyWin* kw) {
    if (r.eye != 0 && r.eye != 1) {
        ++g_win.markerNoEye;
        return;
    }
    const int e = r.eye;
    EyeStats& s = g_win.eye[e];
    ++s.markerReads;
    ++g_win.markerSrc[srcIndex(r.eyeSrc)];
    if (!f.haveTag || !f.pOk) {
        ++s.noP;
        refuse(kNoTag);
        return;
    }
    const double x0[3] = {f.x0[0], f.x0[1], f.x0[2]}, oh[3] = {oH[0], oH[1], oH[2]};
    double mc[3];
    drawtag::markerClip(rows, rows + 4, rows + 12, x0, oh, mc);
    const double aw = std::fabs(mc[2]);
    if (!(aw > 1e-9)) {
        ++s.wZero;
        return;
    }
    if (mc[2] < 0.0) ++s.wNeg;
    const double ndcM[2] = {mc[0] / aw, mc[1] / aw};
    const double xo[3] = {x0[0] - oh[0], x0[1] - oh[1], x0[2] - oh[2]};
    s.ndcM[0] = ndcM[0];
    s.ndcM[1] = ndcM[1];
    g_markerCropNdc[e].drawn[0].store(static_cast<float>(ndcM[0]), std::memory_order_relaxed);   // F53: the eye dump's marker crop
    g_markerCropNdc[e].drawn[1].store(static_cast<float>(ndcM[1]), std::memory_order_relaxed);
    g_markerCropNdc[e].drawnMs.store(GetTickCount64(), std::memory_order_relaxed);
    s.xo = drawtag::length3(xo);
    const WorldView* w = worldGet(frame, e);
    if (!w) {
        ++s.noWorld;
        return;
    }
    const double P[3] = {f.p[0], f.p[1], f.p[2]}, ow[3] = {w->o[0], w->o[1], w->o[2]};
    double wc[4];
    drawtag::worldClip(w->rows, P, ow, wc);
    if (!(wc[3] > 1e-9)) {
        ++s.behind;
        return;
    }
    const double ndcP[2] = {wc[0] / wc[3], wc[1] / wc[3]};
    const double W = r.tw ? static_cast<double>(r.tw) : kDefaultTargetSize, H = r.th ? static_cast<double>(r.th) : kDefaultTargetSize;
    const double dx = (ndcM[0] - ndcP[0]) * W * 0.5, dy = (ndcM[1] - ndcP[1]) * H * 0.5;
    const double px = std::sqrt(dx * dx + dy * dy);
    const double deg = drawtag::ndcAngleDeg(w->rows, ndcM, ndcP);
    const double pw[3] = {P[0] - ow[0], P[1] - ow[1], P[2] - ow[2]};
    ++s.pairs;
    s.pxSum += px;
    if (px > s.pxMax) s.pxMax = px;
    if (deg >= 0.0) {
        ++s.degN;
        s.degSum += deg;
        if (deg > s.degMax) s.degMax = deg;
    }
    s.pw = drawtag::length3(pw);
    s.pwSum += s.pw;
    s.xoSum += s.xo;
    s.dx = dx;
    s.dy = dy;
    s.ndcP[0] = ndcP[0];
    s.ndcP[1] = ndcP[1];
    s.tw = W;
    s.th = H;
    f47Measure(s, *w, rows, oh, P, W, H);
    if (patched) {
        // F52: the residual against the reference the patch measured against (the temporal one, kept by frame and eye, or the world rows).
        const WorldView* rv = r.fixRef == drawtag::kRefTemporal ? refGet(frame, e) : w;
        if (rv) f50Residual(s, *rv, patched, oh, x0, P, r, W, H, kw, e);
    }
    if (const WorldView* o = worldGet(frame, 1 - e)) {
        const double oo[3] = {o->o[0], o->o[1], o->o[2]};
        double oc[4];
        drawtag::worldClip(o->rows, P, oo, oc);
        if (oc[3] > 1e-9) {
            const double ox = (ndcM[0] - oc[0] / oc[3]) * W * 0.5, oy = (ndcM[1] - oc[1] / oc[3]) * H * 0.5;
            s.otherPxSum += std::sqrt(ox * ox + oy * oy);
            ++s.otherN;
        }
    }
}

// One frame's staging slot, read back: its staged world views into the cache first (a marker composite may come before its eye's world
// draw), then every marker composite's stream against the frame's finds, and the pair for each one that read a find by x.
void appendf(char* buf, size_t cap, size_t& len, const char* fmt, ...);   // below
void readFrame(int slot, uint32_t frame, const uint8_t* d, uint32_t used) {
    ++g_win.framesRead;
    if (g_recFrame[slot] != frame) return;
    const std::vector<Rec>& recs = g_recs[slot];
    auto inside = [used](int32_t at, uint64_t bytes) { return at >= 0 && static_cast<uint64_t>(at) + bytes <= used; };
    for (const Rec& r : recs) {
        if (!(r.flags & kRecWorld) || !inside(r.worldAt, kWorldBytes)) continue;
        WorldView v;
        v.frame = frame;
        v.eye = r.eye;
        std::memcpy(v.rows, d + r.worldAt, sizeof(v.rows));
        std::memcpy(v.o, d + r.worldAt + 5 * 16, sizeof(v.o));
        worldPut(v);
        ++g_win.worldRead;
    }
    const Find* finds = g_slotFinds[slot];
    const uint32_t nFinds = g_slotFindN[slot] <= kSlotFinds ? g_slotFindN[slot] : 0;
    // F47 item 2: one line for this frame listing EVERY marker composite draw (the first kBurstFrames read-back frames of each second).
    const bool burst = g_burstLeft > 0;
    char line[4096];
    size_t len = 0;
    uint32_t listed = 0;
    if (burst)
        appendf(line, sizeof(line), len, "native on foot marker probe: frame %u marker composite draws (#, eye (source), place in the eye, call, instances "
                "@ start, t33, record read by x / y or -):", frame);
    for (const Rec& r : recs) {
        if (!(r.flags & kRecMarker)) continue;
        // F47: the CPU shadow of b0 rows 4..7 at the draw against the GPU copy of the same rows.
        if (r.cpuRows && inside(r.rowsAt, kMarkerRowsBytes)) {
            float g[16];
            std::memcpy(g, d + r.rowsAt, sizeof(g));
            double diff = 0.0;
            for (int i = 0; i < 16; ++i) diff = std::max(diff, std::fabs(static_cast<double>(g[i]) - r.cpu[i]));
            if (r.eye == 0 || r.eye == 1) {
                EyeStats& s = g_win.eye[r.eye];
                if (diff == 0.0) ++s.cpuAgree;
                else ++s.cpuDiffer;
                if (diff > s.cpuDiffMax) s.cpuDiffMax = diff;
            }
        }
        drawtag::StreamMatch m;
        bool matched = false;
        uint32_t key = 0;
        int ks = -1;   // F52: the key slot of the record this draw read (by x)
        if (inside(r.streamAt, static_cast<uint64_t>(r.entries) * drawtag::kStreamStride)) {
            ++g_win.streamRead;
            uint32_t wants[kSlotFinds];
            uint32_t which[kSlotFinds];
            uint32_t nw = 0;
            for (uint32_t k = 0; k < nFinds; ++k)
                if (finds[k].pool == r.pool) {
                    wants[nw] = finds[k].index;
                    which[nw] = k;
                    ++nw;
                }
            if (nw && drawtag::matchStream(d + r.streamAt, r.entries, r.first, wants, nw, m)) {
                matched = true;
                key = finds[which[m.want]].key;
                if (m.xHits) ++g_win.readsX;
                else ++g_win.readsYOnly;
                addTuple(r, m);
                // F47: the eye's prediction for the correction: this draw's place among the eye's marker composite draws, and the find's key.
                // F52: per key: every key's own match (its record, the draws that read it).
                if (m.xHits && (r.eye == 0 || r.eye == 1)) {
                    ks = keySlotAcquire(key, frame);
                    KeyState& K = g_keys[ks];
                    EyeMatch& em = K.eye[r.eye].match;
                    if (em.frame == ~0u || static_cast<int32_t>(frame - em.frame) >= 0) {
                        em.frame = frame;
                        em.ord = r.ordEye;
                        em.key = key;
                    }
                    K.record = m.record;
                    KeyWin& kw = g_win.keys[ks];
                    kw.record = m.record;
                    ++kw.reads[r.eye];
                    kw.ordMask[r.eye] |= 1u << (r.ordEye < 31 ? r.ordEye : 31);
                }
                if (m.xHits && inside(r.rowsAt, kMarkerRowsBytes)) {
                    float rows[16], oH[4];
                    std::memcpy(rows, d + r.rowsAt, sizeof(rows));
                    bool haveO = true;
                    if (r.flags & kRecShadowO) std::memcpy(oH, r.origin, sizeof(oH));
                    else if (inside(r.originAt, kOriginBytes)) std::memcpy(oH, d + r.originAt, sizeof(oH));
                    else haveO = false;
                    float patched[16];
                    const bool havePatched = r.fixApplied && inside(r.patchedAt, kMarkerRowsBytes);
                    if (havePatched) std::memcpy(patched, d + r.patchedAt, sizeof(patched));
                    if (haveO) pairMarker(r, finds[which[m.want]], rows, oH, frame, havePatched ? patched : nullptr, ks >= 0 ? &g_win.keys[ks] : nullptr);
                }
            }
        }
        // F52 item C: the bookkeeping: each marker composite draw per eye, tagged (read a found record by x), untagged or not copied.
        const bool streamIn = r.streamAt >= 0 && inside(r.streamAt, static_cast<uint64_t>(r.entries) * drawtag::kStreamStride);
        if (r.eye == 0 || r.eye == 1) g_win.tally[r.eye].note(streamIn, matched && m.xHits);
        else ++g_win.tallyNoEye;
        // F53: an untagged draw (its stream read, no found record read by x): the record its first entry names, kept with its place in the eye
        // (its position is read at the pool's next Unmap, unmapPool).
        if ((r.eye == 0 || r.eye == 1) && streamIn && r.entries > 0 && r.pool && !(matched && m.xHits)) {
            uint32_t e0[2];
            std::memcpy(e0, d + r.streamAt, sizeof(e0));
            const uint32_t rec = r.first + e0[0];
            Untag* u = nullptr;
            for (uint32_t i = 0; i < g_win.nUntag && !u; ++i) {
                Untag& c = g_win.untag[i];
                if (c.eye == r.eye && c.ord == r.ordEye && c.record == rec && c.pool == r.pool) u = &c;
            }
            if (!u && g_win.nUntag < kUntags) {
                u = &g_win.untag[g_win.nUntag++];
                *u = Untag{};
                u->pool = r.pool;
                u->record = rec;
                u->eye = r.eye;
                u->ord = r.ordEye;
                u->ps = r.ps;
            }
            if (u) {
                ++u->n;
                u->instances = r.instances;
            } else {
                ++g_win.untagDropped;
            }
        }
        // F47: did the patched draw read the record (the read-back's verdict on the draw-time prediction)?
        if (r.fixApplied && (r.eye == 0 || r.eye == 1)) {
            if (matched && m.xHits) ++g_win.eye[r.eye].predHit;
            else ++g_win.eye[r.eye].predMiss;
        }
        if (burst) {
            ++listed;
            appendf(line, sizeof(line), len, " [#%u eye %d (%c) %u %c %u@%u t33 %p%s: ", r.ordAll, r.eye, r.eyeSrc ? r.eyeSrc : '-', r.ordEye,
                    r.kind ? r.kind : '?', r.instances, r.start, r.t33, r.watched ? (r.findsNow ? " watched" : " watched, no find") :
                    " NOT watched");
            if (matched) appendf(line, sizeof(line), len, "record %u key %u%s %s, entry %u, x %u y %u", m.record, key,
                                 (key & kF47SecondPostBit) ? " (second post)" : "", m.viaY ? "by y" : "by x", m.entry, m.xHits, m.yHits);
            else if (r.streamAt >= 0) appendf(line, sizeof(line), len, "- (stream read, no record of a find)");
            else appendf(line, sizeof(line), len, "- (stream not copied)");
            if (r.fixApplied) appendf(line, sizeof(line), len, "; PATCHED dx %+.5f dy %+.5f NDC%s", double(r.fixDx), double(r.fixDy),
                                      r.fixHeld ? " (held)" : "");
            appendf(line, sizeof(line), len, "]");
        }
    }
    if (burst && listed) {
        --g_burstLeft;
        appendf(line, sizeof(line), len, " (finds in the frame: %u)", nFinds);
        Log::get().note("%s", line);
    }
}

// The first draw of a new frame: the last frame's slot keeps its finds and waits for its read-back, the head is sampled, and the slots two or
// more frames old are read.
void frameTurn(ID3D11DeviceContext* ctx, uint32_t frame) {
    const int open = g_staging.openSlot();
    if (open >= 0) {
        const uint32_t f = g_staging.slotFrame(open);
        uint32_t n = 0;
        for (int s = 0; s < 2; ++s)
            for (uint32_t i = 0; i < g_findN[s] && n < kSlotFinds; ++i)
                if (f - g_finds[s][i].frame <= 1u) g_slotFinds[open][n++] = g_finds[s][i];
        g_slotFindN[open] = n;
        g_staging.commit();
    }
    headSample();
    rowCensusTurn(frame);   // F53: the census frame that ended is logged; a new one may start
    g_staging.poll(ctx, frame, 2, 12, [](int slot, uint32_t f, const uint8_t* d, uint32_t used) { readFrame(slot, f, d, used); }, &g_win.lost);
}

const char* mapName(int t) {
    return t == D3D11_MAP_WRITE_DISCARD ? "DISCARD" : t == D3D11_MAP_WRITE_NO_OVERWRITE ? "NO_OVERWRITE" : "other";
}

// Appends printf output to a fixed buffer (truncates silently at its end).
void appendf(char* buf, size_t cap, size_t& len, const char* fmt, ...) {
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int k = std::vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
    if (k > 0) len = len + static_cast<size_t>(k) < cap ? len + static_cast<size_t>(k) : cap - 1;
}

// F46: an eye's marker side for the 1 s line: present (with what kept its reads from pairing), or MISSING and the first reason that holds.
void markerSide(const Window& w, int e, char* out, size_t cap) {
    using ull = unsigned long long;
    const EyeStats& s = w.eye[e];
    size_t len = 0;
    out[0] = 0;
    if (s.markerReads) {
        appendf(out, cap, len, "present (%llu reads of the record", ull(s.markerReads));
        if (s.noP) appendf(out, cap, len, "; %llu with no P (node+0x50 unread)", ull(s.noP));
        if (s.wZero) appendf(out, cap, len, "; %llu with r7 . p zero", ull(s.wZero));
        if (s.noWorld) appendf(out, cap, len, "; %llu in a frame with no world view for this eye", ull(s.noWorld));
        if (s.behind) appendf(out, cap, len, "; %llu with P behind this eye's world view", ull(s.behind));
        appendf(out, cap, len, ")");
    } else if (!w.haveFind) {
        appendf(out, cap, len, "MISSING: no marker record was found this second (the sentinel was not seen in a watched pool)");
    } else if (!w.markerPool) {
        appendf(out, cap, len, "MISSING: no marker composite draw (VS %016llX) was bound to the watched pool", ull(nofoot::kMarkerCompositeVs));
    } else if (!w.markerFinds) {
        appendf(out, cap, len, "MISSING: the marker composite draws bound to the pool (%llu, indirect %llu) came only in frames with no find",
                ull(w.markerPool), ull(w.indirect));
    } else if (!w.streamDraws) {
        appendf(out, cap, len, "MISSING: no marker composite draw's stream and rows were copied (%llu tried; the refusals are in the cost line)",
                ull(w.markerFinds));
    } else if (!w.streamRead) {
        appendf(out, cap, len, "MISSING: no copied marker composite stream was read back (%llu copied, %llu read-backs lost)", ull(w.streamDraws),
                ull(w.lost));
    } else if (!w.readsX) {
        appendf(out, cap, len, "MISSING: no marker composite draw read the record by x (%llu streams read, by y only %llu)", ull(w.streamRead),
                ull(w.readsYOnly));
    } else if (w.markerNoEye) {
        appendf(out, cap, len, "MISSING: the %llu draws that read the record had no eye (neither the UI layer's mapping nor ui depth's table "
                "named their target, and no earlier answer was kept)", ull(w.markerNoEye));
    } else {
        appendf(out, cap, len, "MISSING: the record was read for the other eye only (%llu reads)", ull(w.eye[1 - e].markerReads));
    }
}

// F46: an eye's world side for the 1 s line: present, or MISSING and the first reason that holds.
void worldSide(const Window& w, int e, char* out, size_t cap) {
    using ull = unsigned long long;
    const EyeStats& s = w.eye[e];
    size_t len = 0;
    out[0] = 0;
    if (s.worldViews) {
        appendf(out, cap, len, "present (%llu eye-frames)", ull(s.worldViews));
    } else if (!w.worldDraws) {
        appendf(out, cap, len, "MISSING: no world pool draw this second");
    } else if (w.worldNoEye) {
        appendf(out, cap, len, "MISSING: %llu world draws had no eye (neither the UI layer's mapping nor ui depth's table named their target, "
                "and the order rule found a third target of its shape or a full table)", ull(w.worldNoEye));
    } else if (w.worldNoRows) {
        appendf(out, cap, len, "MISSING: b1 does not cover rows 270..275 (%llu draws; last: b1 %s, ByteWidth %u, first constant %u)",
                ull(w.worldNoRows), w.worldCb1Bound ? "bound" : "NOT bound", w.worldCb1Bytes, w.worldF1);
    } else if (w.worldStageFail) {
        appendf(out, cap, len, "MISSING: the rows' copy failed (%llu; the refusals are in the cost line)", ull(w.worldStageFail));
    } else if (w.eye[1 - e].worldViews) {
        appendf(out, cap, len, "MISSING: every eye-sized world target this second was given the other eye (%llu eye-frames there)",
                ull(w.eye[1 - e].worldViews));
    } else {
        appendf(out, cap, len, "MISSING: no world draw into an eye-sized target (%llu looked at: no target %llu, not eye-sized %llu, tries capped "
                "%llu)", ull(w.worldDraws), ull(w.worldNoTarget), ull(w.worldNotEyeSized), ull(w.worldTriesCapped));
    }
}

void report(uint64_t nowMs) {
    if (g_winMs == 0) {
        g_winMs = nowMs;
        return;
    }
    if (nowMs - g_winMs < 1000) return;
    g_winMs = nowMs;
    Window& w = g_win;
    using ull = unsigned long long;
    const uint64_t taggedAll = g_f44Tagged.load(std::memory_order_relaxed);
    const uint64_t untaggableAll = g_f44Untaggable.load(std::memory_order_relaxed);
    const uint64_t busyAll = g_f44CellBusy.load(std::memory_order_relaxed);
    const uint64_t tagged = taggedAll - g_taggedSeen, untaggable = untaggableAll - g_untaggableSeen, busy = busyAll - g_busySeen;
    g_taggedSeen = taggedAll;
    g_untaggableSeen = untaggableAll;
    g_busySeen = busyAll;
    const ull scanMean = w.scans ? w.scanUsSum / w.scans : 0;
    if (w.haveFind) {
        Log::get().note("native on foot marker probe: marker record found at index %u (key %u, tag age %u frames, map %s, pool %p); this second: "
                        "%llu finds (%llu at the last index), index same %llu, changed %llu; X0 == t' bit-exact %llu, differs %llu (last X0 (%.4f, %.4f, "
                        "%.4f), t' (%.4f, %.4f, %.4f)); full scans %llu (max %llu us, mean %llu us), backoffs %llu; unmaps with tags in flight %llu, "
                        "with no find %llu; maps DISCARD %llu, NO_OVERWRITE %llu, other %llu; tagged %llu, untaggable %llu, cell busy %llu",
                        w.index, w.key, w.age, mapName(w.mapType), w.pool, ull(w.finds), ull(w.hintFinds), ull(w.sameIndex), ull(w.changedIndex),
                        ull(w.exact), ull(w.differs), double(w.x0[0]), double(w.x0[1]), double(w.x0[2]), double(w.tp[0]), double(w.tp[1]),
                        double(w.tp[2]), ull(w.scans), ull(w.scanUsMax), scanMean, ull(w.backoffs), ull(w.unmapsTagged), ull(w.unmapsNoFind),
                        ull(w.mapDiscard), ull(w.mapNoOverwrite), ull(w.mapOther), ull(tagged), ull(untaggable), ull(busy));
    } else if (tagged || w.unmapsTagged) {
        Log::get().note("native on foot marker probe: sentinel NOT found this second: tagged %llu, untaggable %llu, cell busy %llu; unmaps with "
                        "tags in flight %llu; full scans %llu (max %llu us, mean %llu us), backoffs %llu; maps DISCARD %llu, NO_OVERWRITE %llu, other "
                        "%llu; pools watched by a HUD draw: %s",
                        ull(tagged), ull(untaggable), ull(busy), ull(w.unmapsTagged), ull(w.scans), ull(w.scanUsMax), scanMean, ull(w.backoffs),
                        ull(w.mapDiscard), ull(w.mapNoOverwrite), ull(w.mapOther), g_pools[0].res ? "yes" : "none yet");
    }
    // F45 item 1, F46: who reads the tagged record, among the marker composite draws (the only draws whose streams are copied now).
    if (w.haveFind || w.markerPool || w.streamRead) {
        char buf[3072];
        size_t len = 0;
        if (w.readsX || w.readsYOnly) {
            appendf(buf, sizeof(buf), len, "native on foot marker probe: record %u is read by %llu marker composite draws this second (by x %llu, by "
                    "y only %llu):", w.index, ull(w.readsX + w.readsYOnly), ull(w.readsX), ull(w.readsYOnly));
            for (uint32_t i = 0; i < w.nTuples; ++i) {
                const Tuple& t = w.tuples[i];
                appendf(buf, sizeof(buf), len, " [vs %016llX ps %016llX eye %d (the UI layer's) call %c: %llu draws, instances %u..%u, entry %u -> "
                        "record %u%s, x hits %u, y hits %u]", ull(t.vs), ull(t.ps), t.eye, t.kind ? t.kind : '?', ull(t.draws), t.instMin,
                        t.instMax, t.entry, t.record, t.viaY ? " (by y)" : "", t.xHits, t.yHits);
            }
            if (w.tuplesDropped) appendf(buf, sizeof(buf), len, " (+%llu draws of other pairs not listed)", ull(w.tuplesDropped));
        } else if (w.haveFind) {
            appendf(buf, sizeof(buf), len, "native on foot marker probe: record %u is read by no marker composite draw this second", w.index);
        } else {
            appendf(buf, sizeof(buf), len, "native on foot marker probe: no record found this second, nothing to match");
        }
        appendf(buf, sizeof(buf), len, "; marker composite draws bound to the pool %llu (indirect, range unreadable, %llu), in a frame with a find "
                "%llu, streams copied %llu (%llu entries, %llu cut at %u or the buffer end), read %llu; their eye by the UI layer's mapping %llu, ui "
                "depth's table %llu, remembered %llu, none %llu; frames read %llu, lost %llu",
                ull(w.markerPool), ull(w.indirect), ull(w.markerFinds), ull(w.streamDraws), ull(w.streamEntries), ull(w.streamCut), kEntriesMax,
                ull(w.streamRead), ull(w.markerSrc[0]), ull(w.markerSrc[1]), ull(w.markerSrc[2]), ull(w.markerNoEye), ull(w.framesRead),
                ull(w.lost));
        Log::get().note("%s", buf);
    }
    // F46: the world views: eye-frames by eye, how they were copied, whose eye they took, the depth view's check, and what failed.
    if (w.worldDraws) {
        Log::get().note("native on foot marker probe: world views (b1 rows 270..273, origin row 275): eye-frames eye 0 %llu, eye 1 %llu (from the "
                        "CB shadow %llu, staged %llu, read back %llu); eye by the UI layer's mapping %llu, ui depth's table %llu, remembered %llu, the "
                        "order rule %llu; the scene depth view's eye agrees %llu, DISAGREES %llu, unknown %llu; world draws looked at %llu: no "
                        "target %llu, not eye-sized %llu, no eye %llu, b1 rows not covered %llu, copy failed %llu, tries capped %llu",
                        ull(w.eye[0].worldViews), ull(w.eye[1].worldViews), ull(w.worldShadow), ull(w.worldStaged), ull(w.worldRead),
                        ull(w.worldSrc[0]), ull(w.worldSrc[1]), ull(w.worldSrc[2]), ull(w.worldSrc[3]), ull(w.dsvAgree), ull(w.dsvDisagree),
                        ull(w.dsvUnknown), ull(w.worldDraws), ull(w.worldNoTarget), ull(w.worldNotEyeSized), ull(w.worldNoEye), ull(w.worldNoRows),
                        ull(w.worldStageFail), ull(w.worldTriesCapped));
    }
    // F46 item 3, every second on foot: per eye, |ndc_marker - ndc_P| in pixels of the marker's target and in degrees beside the head's
    // angular speed, and |P - o_w| beside |X0 - o_hud|; a missing side says which, and why.
    if (nativeOnFootActive() || w.scanned) {
        char head[96];
        if (w.headN) std::snprintf(head, sizeof(head), "%.1f deg/s mean, %.1f max (%llu frames)", w.headSum / double(w.headN), w.headMax, ull(w.headN));
        else std::snprintf(head, sizeof(head), "n/a (no runtime ring sample)");
        for (int e = 0; e < 2; ++e) {
            const EyeStats& s = w.eye[e];
            if (s.pairs) {
                char other[96];
                if (s.otherN) std::snprintf(other, sizeof(other), "%.2f px mean", s.otherPxSum / double(s.otherN));
                else std::snprintf(other, sizeof(other), "n/a (no other-eye view in the pairs' frames)");
                Log::get().note("native on foot marker probe: eye %d: |ndc_marker - ndc_P| %.2f px mean, %.2f px max, %.4f deg mean, %.4f deg max "
                                "over %llu pairs (last dx %+.2f, dy %+.2f px at %.0fx%.0f); against the other eye's world view %s; head %s; |P - o_w| "
                                "%.3f m, |X0 - o_hud| %.3f m (means; last %.3f, %.3f); last ndc_marker (%.5f, %.5f), ndc_P (%.5f, %.5f); r7 . p < 0 "
                                "%llu; unpaired reads: no world view %llu, P behind %llu, no P %llu",
                                e, s.pxSum / double(s.pairs), s.pxMax, s.degN ? s.degSum / double(s.degN) : -1.0, s.degMax, ull(s.pairs), s.dx, s.dy,
                                s.tw, s.th, other, head, s.pwSum / double(s.pairs), s.xoSum / double(s.pairs), s.pw, s.xo, s.ndcM[0], s.ndcM[1],
                                s.ndcP[0], s.ndcP[1], ull(s.wNeg), ull(s.noWorld), ull(s.behind), ull(s.noP));
            } else {
                char mk[512], wd[384];
                markerSide(w, e, mk, sizeof(mk));
                worldSide(w, e, wd, sizeof(wd));
                Log::get().note("native on foot marker probe: eye %d: no pair this second: marker side %s; world side %s; head %s", e, mk, wd, head);
            }
        }
        // F47 items 1, 3 and 4, per eye: the delta of P itself through the world rows and the marker rows (unpatched), what the correction did
        // at the draw and why not, the delta through the patched rows read back, the prediction's verdict, the CPU b0 against the GPU copy.
        const bool fixOn = g_nativeOnFootMarkerFixKey.load(std::memory_order_relaxed);
        for (int e = 0; e < 2; ++e) {
            const EyeStats& s = w.eye[e];
            char meas[320], res[200];
            if (s.dN)
                std::snprintf(meas, sizeof(meas), "dx mean %+.2f px, max |dx| %.2f px; dy mean %+.2f px, max |dy| %.2f px; %.4f deg mean, %.4f deg "
                              "max; over %llu pairs", s.dxSum / double(s.dN), s.dxMax, s.dySum / double(s.dN), s.dyMax,
                              s.dDegN ? s.dDegSum / double(s.dDegN) : -1.0, s.dDegMax, ull(s.dN));
            else std::snprintf(meas, sizeof(meas), "no pair this second");
            if (s.rN)
                std::snprintf(res, sizeof(res), "dx mean %+.2f px, max |dx| %.2f px; dy mean %+.2f px, max |dy| %.2f px; over %llu pairs",
                              s.rxSum / double(s.rN), s.rxMax, s.rySum / double(s.rN), s.ryMax, ull(s.rN));
            else std::snprintf(res, sizeof(res), "no patched pair this second");
            Log::get().note("native on foot marker fix (F47, key %s): eye %d: P through the world rows minus P through the marker rows (unpatched): "
                            "%s; head %s; patched %llu draws (held %llu); after the patch (F53: X0 through the patched rows minus (P + e)): %s; not "
                            "applied: eye 0 unmatched %llu, eye 1 unmatched %llu, another draw of the eye %llu, ranged b0 %llu, no CPU b0 rows %llu, no "
                            "CPU o_hud %llu, no CPU world view %llu, no P %llu, scene rows untrusted %llu, w <= 0 %llu, gate (|d| >= %.2f NDC) %llu, bind "
                            "refused %llu; the patched draw read the record at read-back %llu, did not %llu; CPU b0 rows 4..7 against the GPU copy: "
                            "agree %llu, differ %llu (max %.3g)",
                            fixOn ? "on" : "off", e, meas, head, ull(s.fPatched), ull(s.fHeld), res, ull(s.fUnmatched[0]), ull(s.fUnmatched[1]),
                            ull(s.fOtherDraw), ull(s.fRanged), ull(s.fNoRows), ull(s.fNoOrigin), ull(s.fNoWorld), ull(s.fNoP), ull(s.fUntrusted),
                            ull(s.fBehind), kGateNdc, ull(s.fGate), ull(s.fBind), ull(s.predHit), ull(s.predMiss), ull(s.cpuAgree), ull(s.cpuDiffer),
                            s.cpuDiffMax);
        }
        // F50, per key and eye (F52: each key has its own baseline): the baseline c and its spread, |m - c| against the head's speed, the
        // patched draws, the residual after the patch by the head's speed, and the honesty check (c by head pitch and roll on slow-head frames,
        // since the last reset). A key-eye with no read and no decision this second is not printed.
        static const char* const kBins[4] = {"< 3", "3..20", "20..60", ">= 60"};
        for (uint32_t ki = 0; ki < kKeySlots; ++ki) {
            if (!g_keyIdx.used[ki]) continue;
            const KeyWin& kw = w.keys[ki];
            for (int e = 0; e < 2; ++e) {
                if (!kw.reads[e] && !kw.decided[e]) continue;
                const LagEye& L = g_keys[ki].eye[e].lag;
                const double W = L.tw ? static_cast<double>(L.tw) : kDefaultTargetSize, H = L.th ? static_cast<double>(L.th) : kDefaultTargetSize;
                char lag[512], resid[512], bins[512];
                size_t ll = 0, rl = 0, bl = 0;
                lag[0] = resid[0] = bins[0] = 0;
                for (int b = 0; b < 4; ++b) {
                    if (kw.lagN[e][b])
                        appendf(lag, sizeof(lag), ll, "%s%s deg/s %.2f px mean, %.2f max (%llu)", ll ? "; " : "", kBins[b],
                                kw.lagSum[e][b] / double(kw.lagN[e][b]), kw.lagMax[e][b], ull(kw.lagN[e][b]));
                    if (kw.resN[e][b])
                        appendf(resid, sizeof(resid), rl, "%s%s deg/s %.2f px mean, %.2f max (%llu)", rl ? "; " : "", kBins[b],
                                kw.resSum[e][b] / double(kw.resN[e][b]), kw.resMax[e][b], ull(kw.resN[e][b]));
                }
                if (!ll) appendf(lag, sizeof(lag), ll, "none this second");
                if (!rl) appendf(resid, sizeof(resid), rl, "none this second");
                // The honesty check: the bins' means (px) and the largest difference between two bins that have kLagBinMinN frames. F53: each
                // bin is m - e (the fixed icon offset), so a constant e shows as bins near zero.
                double eOff[2];
                drawtag::iconOffsetNdc(H, eOff);
                double binMean[5][2] = {};
                bool binOk[5] = {};
                static const char* const kBinNames[5] = {"pitch < -15", "pitch -15..15", "pitch > 15", "|roll| < 10", "|roll| >= 10"};
                for (int b = 0; b < 5; ++b) {
                    const uint64_t n = b < 3 ? L.pitchN[b] : L.rollN[b - 3];
                    const double* sum = b < 3 ? L.pitchSum[b] : L.rollSum[b - 3];
                    if (!n) continue;
                    binMean[b][0] = sum[0] / double(n) - eOff[0];
                    binMean[b][1] = sum[1] / double(n) - eOff[1];
                    binOk[b] = n >= kLagBinMinN;
                    appendf(bins, sizeof(bins), bl, "%s%s (%+.2f, %+.2f) px (%llu)", bl ? ", " : "", kBinNames[b], binMean[b][0] * W * 0.5,
                            binMean[b][1] * H * 0.5, ull(n));
                }
                if (!bl) appendf(bins, sizeof(bins), bl, "no slow-head frames yet");
                double spread = 0.0;
                int compared = 0;
                for (int g = 0; g < 2; ++g) {   // pitch bins against each other, roll bins against each other
                    const int lo = g == 0 ? 0 : 3, hi = g == 0 ? 3 : 5;
                    int inGroup = 0;
                    for (int a = lo; a < hi; ++a) {
                        if (!binOk[a]) continue;
                        ++inGroup;
                        for (int b = a + 1; b < hi; ++b) {
                            if (!binOk[b]) continue;
                            spread = std::max(spread, std::fabs(binMean[a][0] - binMean[b][0]) * W * 0.5);
                            spread = std::max(spread, std::fabs(binMean[a][1] - binMean[b][1]) * H * 0.5);
                        }
                    }
                    if (inGroup >= 2) ++compared;
                }
                const bool masking = compared > 0 && spread > kLagPitchSpreadPx;
                const char* verdict = masking ? "NOT CONSTANT" : compared ? "constant so far" : "not yet testable (fewer than two bins with enough frames)";
                Log::get().note("native on foot marker fix (F53, key %s): tag key %u eye %d (record %u, read by %llu draws, places mask 0x%X; "
                                "reference %s): target = P through the scene rows + e, e (fixed icon offset, %.1f px up) (%+.5f, %+.5f) NDC = (%+.2f, "
                                "%+.2f) px at %.0fx%.0f; the learned baseline (F50, logged only, not applied) c (%+.5f, %+.5f) NDC = (%+.2f, %+.2f) px, "
                                "std %.2f px (%s, %u frames taken; resets this second %llu, last: %s); |m - e| by head speed: %s; decided %llu, patched "
                                "%llu draws (held %llu), scene rows untrusted %llu, gate (|d| >= %.2f NDC) %llu, other eye unmatched %llu; after the "
                                "patch |marker - (P + e)| by head speed: %s; honesty check (m - e by head pitch / roll, slow head, since the last "
                                "reset): %s; spread %.2f px (limit %.1f): %s",
                                fixOn ? "on" : "off", g_keyIdx.key[ki], e, kw.record, ull(kw.reads[e]), kw.ordMask[e],
                                g_refEff[e] == drawtag::kRefTemporal ? "temporal" : "world_first", drawtag::kIconPx, eOff[0], eOff[1],
                                eOff[0] * W * 0.5, eOff[1] * H * 0.5, W, H, L.b.c[0], L.b.c[1], L.b.c[0] * W * 0.5, L.b.c[1] * H * 0.5,
                                L.b.stdPx(W, H), L.b.ready(W, H, kLagStdMaxPx) ? "steady" : L.b.seeded ? "seeded, std too high" : "seeding", L.b.n,
                                ull(kw.resets[e]), L.lastReset, lag, ull(kw.decided[e]), ull(kw.patched[e]), ull(kw.held[e]), ull(kw.untrusted[e]),
                                kGateNdc, ull(kw.gate[e]), ull(kw.otherUnmatched[e]), resid, bins, spread, kLagPitchSpreadPx, verdict);
                if (masking)
                    Log::get().note("native on foot marker fix (F53): tag key %u eye %d: m - e is not constant: e masks a moving error (the bins "
                                    "differ by %.2f px with head pitch or roll: %s). Do not claim the fix", g_keyIdx.key[ki], e, spread, bins);
            }
        }
        // F52 item B: key -> record, and which marker composite draws (their places in each eye) read it, every key seen this second.
        {
            char buf[2048];
            size_t len = 0;
            appendf(buf, sizeof(buf), len, "native on foot marker probe (F52): keys -> records this second:");
            uint32_t listed = 0;
            for (uint32_t ki = 0; ki < kKeySlots; ++ki) {
                const KeyWin& kw = w.keys[ki];
                if (!g_keyIdx.used[ki] || (!kw.reads[0] && !kw.reads[1])) continue;
                ++listed;
                appendf(buf, sizeof(buf), len, " [tag key %u%s: record %u; eye 0 read %llu (places 0x%X), eye 1 read %llu (places 0x%X); patched eye 0 "
                        "%llu, eye 1 %llu]", g_keyIdx.key[ki], (g_keyIdx.key[ki] & kF47SecondPostBit) ? " (second post)" : "", kw.record,
                        ull(kw.reads[0]), kw.ordMask[0], ull(kw.reads[1]), kw.ordMask[1], ull(kw.patched[0]), ull(kw.patched[1]));
            }
            if (!listed) appendf(buf, sizeof(buf), len, " none");
            const uint64_t tagOnlyAll = g_f52TagOnly.load(std::memory_order_relaxed);
            appendf(buf, sizeof(buf), len, "; marker composite draws at read-back: eye 0 %llu (tagged %llu, NOT tagged %llu, not copied %llu), eye 1 "
                    "%llu (tagged %llu, NOT tagged %llu, not copied %llu), no eye %llu; posts tagged in place (key 10, mode game, no head pose) %llu; "
                    "key slots reclaimed %llu", ull(w.tally[0].draws), ull(w.tally[0].tagged), ull(w.tally[0].untagged()), ull(w.tally[0].notCopied),
                    ull(w.tally[1].draws), ull(w.tally[1].tagged), ull(w.tally[1].untagged()), ull(w.tally[1].notCopied), ull(w.tallyNoEye),
                    ull(tagOnlyAll - g_tagOnlySeen), ull(w.keyReclaims));
            g_tagOnlySeen = tagOnlyAll;
            Log::get().note("%s", buf);
        }
        // F52 item A, per eye: which draw gave the world rows, and the world rows against the temporal pass's camera rows for the eye.
        for (int e = 0; e < 2; ++e) {
            const WorldSrc& ws = w.wsrc[e];
            const RefCmp& rc = w.ref[e];
            char src[384], cmp[640];
            if (ws.n)
                std::snprintf(src, sizeof(src), "first main-scene pass (F53) vs %016llX ps %016llX, target %ux%u, dsv %p, viewport %s (%.0f, %.0f, %.0f x %.0f, "
                              "depth %.2f..%.2f), b1 %p; %llu eye-frames, vs changed %llu times", ull(ws.vs), ull(ws.ps), ws.w, ws.h, ws.dsv,
                              ws.vpOk ? "" : "NONE", double(ws.vp[0]), double(ws.vp[1]), double(ws.vp[2]), double(ws.vp[3]), double(ws.vp[4]),
                              double(ws.vp[5]), ws.cb1, ull(ws.n), ull(ws.vsChanges));
            else std::snprintf(src, sizeof(src), "no world rows captured this second");
            if (rc.n)
                std::snprintf(cmp, sizeof(cmp), "%.4f deg mean, %.4f deg max over %llu frames (last: right %.4f, up %.4f, forward %.4f deg; temporal "
                              "forward = %s column 2); at P %.2f px mean, %.2f px max (%llu); the temporal rows' b1 is the world draw's %llu, another %llu "
                              "(temporal vs %016llX)", rc.degSum / double(rc.n), rc.degMax, ull(rc.n), rc.last.right, rc.last.up, rc.last.fwd,
                              rc.sign < 0 ? "minus" : "plus", rc.pxN ? rc.pxSum / double(rc.pxN) : -1.0, rc.pxMax, ull(rc.pxN), ull(rc.sameBuf),
                              ull(rc.otherBuf), ull(rc.tvs));
            else std::snprintf(cmp, sizeof(cmp), "no comparison this second");
            Log::get().note("native on foot marker probe (F52): eye %d world rows: %s; against the temporal pass's camera rows: %s; frames %llu: no "
                            "temporal rows %llu, no world rows %llu, degenerate %llu, axes do not correspond %llu (last: right %.1f, up %.1f deg, same "
                            "handedness %s); reference asked %s, in use %s (switches %llu; decisions world_first %llu, temporal %llu); F53 trust "
                            "(P through the scene rows and the temporal reference under %.1f px): trusted %llu frames, UNTRUSTED %llu (over %.1f px "
                            "%llu, axes do not correspond %llu, no temporal rows %llu, no scene rows %llu, degenerate %llu, no P %llu)",
                            e, src, cmp, ull(rc.ticks), ull(rc.noTemporal), ull(rc.noWorld), ull(rc.degenerate), ull(rc.convention), rc.last.right,
                            rc.last.up, rc.last.sameHand ? "yes" : "no",
                            g_nativeOnFootMarkerRefKey.load(std::memory_order_relaxed) == drawtag::kRefWorldFirst ? "world_first" : "temporal",
                            g_refEff[e] == drawtag::kRefTemporal ? "temporal" : g_refEff[e] == drawtag::kRefWorldFirst ? "world_first" : "none yet",
                            ull(rc.switches), ull(rc.decided[0]), ull(rc.decided[1]), drawtag::kSceneTrustPx, ull(rc.trusted),
                            ull(rc.ticks - rc.trusted), drawtag::kSceneTrustPx, ull(rc.overPx), ull(rc.convention), ull(rc.noTemporal),
                            ull(rc.noWorld), ull(rc.degenerate), ull(rc.noP));
        }
        // F53 item 4: the scene reference's refusals and the cb1 census's summary.
        Log::get().note("native on foot marker probe (F53): scene reference: world draws refused as the eye's rows: the first-person body's PS "
                        "%016llX %llu, not the scene depth view %llu, not a scene geometry VS (%016llX, %016llX, %016llX) %llu; cb1 census frames "
                        "%llu: most distinct rows 270..273 contents in an eye-frame eye 0 %llu, eye 1 %llu; widest forward angle between two "
                        "contents eye 0 %.3f deg, eye 1 %.3f deg",
                        ull(drawtag::kFpBodyPs), ull(w.sceneRefused[drawtag::kSceneFpBodyPs]), ull(w.sceneRefused[drawtag::kSceneNotSceneDepth]),
                        ull(drawtag::kSceneVs[0]), ull(drawtag::kSceneVs[1]), ull(drawtag::kSceneVs[2]), ull(w.sceneRefused[drawtag::kSceneNotSceneVs]),
                        ull(w.rowCensusFrames), ull(w.rowDistinctMax[0]), ull(w.rowDistinctMax[1]), w.rowFwdMax[0], w.rowFwdMax[1]);
        // F53 item 6: the untagged marker composite draws per eye, with the record each one's first entry names and that record's position.
        for (int e = 0; e < 2; ++e) {
            char buf[3072];
            size_t len = 0;
            uint32_t listed = 0;
            appendf(buf, sizeof(buf), len, "native on foot marker probe (F53): eye %d untagged marker composite draws (place in the eye: record, "
                    "draws, instances, position at +16, |pos - X0| of the newest find, the tag it carries):", e);
            for (uint32_t i = 0; i < w.nUntag; ++i) {
                const Untag& u = w.untag[i];
                if (u.eye != e) continue;
                ++listed;
                appendf(buf, sizeof(buf), len, " [place %u: record %u (pool %p), %llu draws, %u instances, ps %016llX", static_cast<unsigned>(u.ord),
                        u.record, u.pool, ull(u.n), u.instances, ull(u.ps));
                if (u.havePos) {
                    const double dx = double(u.pos[0]) - g_lastFind.x0[0], dy = double(u.pos[1]) - g_lastFind.x0[1],
                                 dz = double(u.pos[2]) - g_lastFind.x0[2];
                    appendf(buf, sizeof(buf), len, ", pos (%.3f, %.3f, %.3f)", double(u.pos[0]), double(u.pos[1]), double(u.pos[2]));
                    if (g_haveLastFind) appendf(buf, sizeof(buf), len, ", |pos - X0| %.3f m", std::sqrt(dx * dx + dy * dy + dz * dz));
                    if (u.tagKey >= 0) appendf(buf, sizeof(buf), len, ", CARRIES tag key %d (age %u frames): a marker piece the match missed",
                                               u.tagKey, u.tagAge);
                    else appendf(buf, sizeof(buf), len, ", no tag in flight (low bits %06X%s)", u.bits, u.taggable ? "" : ", a component not taggable");
                } else {
                    appendf(buf, sizeof(buf), len, ", position not read (no Unmap of its pool after the read-back this second)");
                }
                appendf(buf, sizeof(buf), len, "]");
            }
            if (!listed) appendf(buf, sizeof(buf), len, " none");
            appendf(buf, sizeof(buf), len, "; positions read %llu, faulted or past the range %llu; untagged draws past the table %llu",
                    ull(w.untagPosRead), ull(w.untagPosFault), ull(w.untagDropped));
            if (listed || w.tally[e].draws) Log::get().note("%s", buf);
        }
        // F53 item 6: the post stub's key census (cumulative since launch): every key posted while the probe was on, its posts and tagged posts,
        // and whether every key (first and second posts) has its own sentinel.
        {
            char buf[2048];
            size_t len = 0;
            uint32_t keys[kF53KeyCensus];
            uint32_t nk = 0;
            appendf(buf, sizeof(buf), len, "native on foot marker probe (F53): post stub keys (since launch; posts, tagged):");
            for (const F53KeySlot& s : g_f53Keys) {
                const uint32_t kp = s.keyPlus1.load(std::memory_order_relaxed);
                if (!kp) continue;
                const uint64_t posts = s.posts.load(std::memory_order_relaxed), tg = s.tagged.load(std::memory_order_relaxed);
                keys[nk++] = kp - 1u;
                appendf(buf, sizeof(buf), len, " [key %u: %llu, %llu%s]", kp - 1u, ull(posts), ull(tg), tg < posts ? " -- NOT ALL TAGGED" : "");
            }
            if (!nk) appendf(buf, sizeof(buf), len, " none yet");
            uint32_t ka = 0, kb = 0;
            const bool distinct = drawtag::keysDistinct(keys, nk, &ka, &kb);
            if (distinct) appendf(buf, sizeof(buf), len, "; every key's sentinel is distinct (first and second posts)");
            else appendf(buf, sizeof(buf), len, "; SENTINELS COLLIDE: key %u and key %u share a tag byte", ka, kb);
            appendf(buf, sizeof(buf), len, "; keys past the census table %llu", ull(g_f53KeyOverflow.load(std::memory_order_relaxed)));
            if (nk) Log::get().note("%s", buf);
        }
        const uint64_t secondAll = g_f47SecondPosts.load(std::memory_order_relaxed);
        const uint64_t second = secondAll - g_secondPostsSeen;
        g_secondPostsSeen = secondAll;
        Log::get().note("native on foot marker probe (F47): marker composite draws %llu (no t33 %llu, t33 not a stride-336 pool %llu), pools "
                        "learned from them %llu; second posts of a key in one frame %llu (tagged with bit 7 in the key); a second copy of a record "
                        "in one unmap %llu; census scans %llu; b0 CPU shadow writes: mapped %llu, updated %llu; fix: draws with no eye %llu, stale "
                        "decisions %llu, decisions with no layered issue %llu, late restores %llu; F52: marker composite draws NOT tagged (no record "
                        "match at read-back) eye 0 %llu, eye 1 %llu (not copied: eye 0 %llu, eye 1 %llu)",
                        ull(w.b10bDraws), ull(w.b10bNoT33), ull(w.b10bNotPool), ull(w.markerLearned), ull(second), ull(w.dupFinds),
                        ull(w.censusScans), ull(w.cbMapped), ull(w.cbUpdated), ull(w.fixNoEye), ull(w.fixStale), ull(w.fixNotIssued), ull(w.fixLateRestore),
                        ull(w.tally[0].untagged()), ull(w.tally[1].untagged()), ull(w.tally[0].notCopied), ull(w.tally[1].notCopied));
    }
    g_burstLeft = kBurstFrames;
    g_rowCensusLeft = kRowCensusFrames;   // F53: the next second's census frames
    // F46 item 4: the cost, every second the draw tee ran: its calls, the draws looked at past the VS hash, the time in them, what was staged.
    if (w.hookCalls || w.hudDraws) {
        uint64_t refusedAll = 0;
        for (uint64_t r : w.refused) refusedAll += r;
        char buf[1024];
        size_t len = 0;
        appendf(buf, sizeof(buf), len, "native on foot marker probe: cost: draw tee calls %llu, draws scanned past the VS hash %llu (on-foot HUD %llu, "
                "marker composite %llu, world %llu), %llu us in them; records %llu, staged %llu bytes; refused %llu:", ull(w.hookCalls),
                ull(w.scanned), ull(w.scanHud), ull(w.scanMarker), ull(w.scanWorld), ull(f30QpcUs(w.drawTicks)), ull(w.recs),
                ull(w.stagedBytes), ull(refusedAll));
        for (uint32_t r = 0; r < kRefuseCount; ++r) appendf(buf, sizeof(buf), len, "%s %s %llu", r ? "," : "", kRefuseShort[r], ull(w.refused[r]));
        appendf(buf, sizeof(buf), len, "; read-backs lost %llu", ull(w.lost));
        Log::get().note("%s", buf);
    }
    w = Window{};
}
}  // namespace f44
}  // namespace

namespace {
namespace f44 {
// F47: the b0 CPU shadow's copy of a mapped write, before the real Unmap; under SEH (the mapping is the game's).
__declspec(noinline) bool shadowCopyGuarded(void* dst, const void* src, size_t bytes) noexcept {
    __try {
        std::memcpy(dst, src, bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}
}  // namespace f44
}  // namespace

void nativeOnFootMarkerProbeMap(ID3D11Resource* res, void* data, int mapType) {
    if (!res || !data) return;
    // F47: a shadowed marker composite b0: the mapped pointer (the copy is taken before the real Unmap). Still a buffer of its size?
    for (f44::CbShadow& s : f44::g_cbShadow) {
        if (s.res != res) continue;
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        D3D11_BUFFER_DESC d{};
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
        if (dim != D3D11_RESOURCE_DIMENSION_BUFFER || d.ByteWidth < f44::kCb0ShadowBytes) {
            s = f44::CbShadow{};
            break;
        }
        s.bytes = d.ByteWidth;
        s.mapped = data;
        s.valid = false;
        break;
    }
    for (f44::Pool& p : f44::g_pools) {
        if (p.res != res) continue;
        // The game's live resource here: still a structured buffer of stride 336? (The address may have been re-used since it was learned.)
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        D3D11_BUFFER_DESC d{};
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
        if (dim != D3D11_RESOURCE_DIMENSION_BUFFER || d.StructureByteStride != drawtag::kRecordStride || !d.ByteWidth) {
            p = f44::Pool{};
            f44::refuse(f44::kPoolChanged);
            return;
        }
        p.bytes = d.ByteWidth;
        p.mapped = data;
        p.mapType = mapType;
        if (mapType == D3D11_MAP_WRITE_DISCARD) ++f44::g_win.mapDiscard;
        else if (mapType == D3D11_MAP_WRITE_NO_OVERWRITE) ++f44::g_win.mapNoOverwrite;
        else ++f44::g_win.mapOther;
        return;
    }
}

void nativeOnFootMarkerProbeUnmap(const ID3D11Resource* res) {
    if (!res) return;
    for (f44::CbShadow& s : f44::g_cbShadow) {
        if (s.res != res || !s.mapped) continue;
        s.valid = f44::shadowCopyGuarded(s.rows, s.mapped, f44::kCb0ShadowBytes);
        s.mapped = nullptr;
        if (s.valid) ++f44::g_win.cbMapped;
        break;
    }
    for (f44::Pool& p : f44::g_pools)
        if (p.res == res) {
            if (p.mapped) f44::unmapPool(p);
            return;
        }
}

void nativeOnFootMarkerProbeDraw(ID3D11DeviceContext* ctx, uint32_t instances, uint32_t startInstance, char kind) {
    if (!ctx) return;
    ++f44::g_drawSerial;   // F47: a decision is for the draw that made it (Begin checks the serial)
    if (f44::g_fix.pending) ++f44::g_win.fixNotIssued;   // decided, but Begin never ran for it (the draw was not the layer's, or skipped)
    f44::g_fix.pending = false;
    // F47: a private b0 still bound from an earlier draw (End did not run): put the game's back only while ours is still the one bound (a
    // later bind of the game's own stands), and drop the reference either way.
    if (f44::g_fix.bound) {
        ID3D11Buffer* now = nullptr;
        ctx->VSGetConstantBuffers(0, 1, &now);
        if (now && now == f44::g_privCb) ctx->VSSetConstantBuffers(0, 1, &f44::g_fix.saved);
        if (now) now->Release();
        if (f44::g_fix.saved) f44::g_fix.saved->Release();
        f44::g_fix.saved = nullptr;
        f44::g_fix.bound = false;
        ++f44::g_win.fixLateRestore;
    }
    const uint32_t frame = g_f44Frame.load(std::memory_order_relaxed);
    if (frame != f44::g_polledFrame) {
        f44::g_polledFrame = frame;
        f44::frameTurn(ctx, frame);
    }
    f44::draw(ctx, frame, instances, startInstance, kind);
}

void nativeOnFootMarkerProbeUpdate(const ID3D11Resource* res, const void* data, bool boxed, uint32_t left, uint32_t right) {
    if (!res || !data) return;
    for (f44::CbShadow& s : f44::g_cbShadow) {
        if (s.res != res) continue;
        if (!boxed) {
            s.valid = f44::shadowCopyGuarded(s.rows, data, f44::kCb0ShadowBytes);
        } else if (left < f44::kCb0ShadowBytes && right > left) {
            // A ranged write: the part of it in rows 0..7 lands in place; the rest of the shadow keeps what it had.
            const uint32_t end = right < f44::kCb0ShadowBytes ? right : f44::kCb0ShadowBytes;
            uint8_t* rows = reinterpret_cast<uint8_t*>(s.rows);
            if (!f44::shadowCopyGuarded(rows + left, data, end - left)) s.valid = false;
        }
        if (s.valid) ++f44::g_win.cbUpdated;
        break;
    }
}

namespace {
namespace f44 {
// F47: the private b0 and its work buffer (both the game's b0 size, made once); false (and said once) when they cannot be made, or when the
// game's b0 is a different size from the one they were made for.
bool ensurePrivate(ID3D11DeviceContext* ctx, ID3D11Buffer* cb0) {
    D3D11_BUFFER_DESC d{};
    cb0->GetDesc(&d);
    if (g_privCb && g_workBuf) return d.ByteWidth == g_privBytes;
    if (g_privFailed) return false;
    g_privFailed = true;   // until both are made
    if (d.ByteWidth < kCb0ShadowBytes || d.ByteWidth > kPrivateMax || (d.ByteWidth & 15u)) {
        Log::get().note("native on foot marker fix: the marker composite's b0 is %u bytes (not a multiple of 16 in 128..%u); no private copy is "
                        "made, nothing is patched this session", d.ByteWidth, kPrivateMax);
        return false;
    }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_BUFFER_DESC p{};
    p.ByteWidth = d.ByteWidth;
    p.Usage = D3D11_USAGE_DEFAULT;
    p.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_BUFFER_DESC q{};
    q.ByteWidth = d.ByteWidth;
    q.Usage = D3D11_USAGE_DEFAULT;
    q.BindFlags = D3D11_BIND_VERTEX_BUFFER;   // never bound: a boxed UpdateSubresource may write it (a constant buffer's box needs D3D11.1)
    const HRESULT h1 = dev->CreateBuffer(&p, nullptr, &g_privCb);
    const HRESULT h2 = SUCCEEDED(h1) ? dev->CreateBuffer(&q, nullptr, &g_workBuf) : E_FAIL;
    dev->Release();
    if (FAILED(h1) || FAILED(h2) || !g_privCb || !g_workBuf) {
        if (g_privCb) g_privCb->Release();
        if (g_workBuf) g_workBuf->Release();
        g_privCb = g_workBuf = nullptr;
        Log::get().note("native on foot marker fix: the private b0 (%u bytes) or its work buffer could not be made (0x%08lX, 0x%08lX); nothing is "
                        "patched this session", d.ByteWidth, static_cast<unsigned long>(h1), static_cast<unsigned long>(h2));
        return false;
    }
    g_privBytes = d.ByteWidth;
    g_privFailed = false;
    return true;
}
}  // namespace f44
}  // namespace

bool nativeOnFootMarkerFixBegin(ID3D11DeviceContext* ctx) {
    using namespace f44;
    if (!ctx || !g_fix.pending) return false;
    g_fix.pending = false;
    if (g_fix.serial != g_drawSerial || g_fix.ctx != ctx || g_fix.bound || (g_fix.eye != 0 && g_fix.eye != 1)) {
        ++g_win.fixStale;
        return false;
    }
    EyeStats& s = g_win.eye[g_fix.eye];
    auto refuseBind = [&s](const char* why) {
        ++s.fBind;
        if (!g_bindNoted) {
            g_bindNoted = true;
            Log::get().note("native on foot marker fix: a decided draw was drawn unpatched: %s. Said once; counted in the 1 s lines", why);
        }
        return false;
    };
    if (bindingShaderHash(BindSlot::Vs) != nofoot::kMarkerCompositeVs) return refuseBind("the vertex shader bound at the issue is not the marker composite's");
    ID3D11Buffer* cb0 = nullptr;
    if (!vsB0Plain(ctx, &cb0)) return refuseBind("b0 at the issue is not a whole-buffer bind from constant 0");
    if (static_cast<const void*>(cb0) != g_fix.cb0) {
        cb0->Release();
        return refuseBind("b0 at the issue is not the buffer the decision read");
    }
    if (!ensurePrivate(ctx, cb0)) {
        cb0->Release();
        return refuseBind("no private b0 of the game's b0 size");
    }
    // The game's whole b0 into the work buffer, rows 4 and 5 (moved) written over it, the work buffer copied whole into the private b0 (every
    // copy into the constant buffer is whole), then the private b0 bound. GPU order makes each step see the one before.
    const D3D11_BOX all{0, 0, 0, g_privBytes, 1, 1};
    ctx->CopySubresourceRegion(g_workBuf, 0, 0, 0, 0, cb0, 0, &all);
    const D3D11_BOX rows{4 * 16, 0, 0, 6 * 16, 1, 1};
    vScreenUpdateSubresourceRaw(ctx, g_workBuf, 0, &rows, g_fix.rows45, 0, 0);
    ctx->CopySubresourceRegion(g_privCb, 0, 0, 0, 0, g_workBuf, 0, &all);
    ctx->VSSetConstantBuffers(0, 1, &g_privCb);
    g_fix.saved = cb0;   // the reference vsB0Plain took, held until End
    g_fix.bound = true;
    ++s.fPatched;
    if (g_fix.held) ++s.fHeld;
    if (g_fix.keySlot >= 0 && g_fix.keySlot < static_cast<int>(kKeySlots)) {   // F52: per key
        KeyWin& kw = g_win.keys[g_fix.keySlot];
        ++kw.patched[g_fix.eye];
        if (g_fix.held) ++kw.held[g_fix.eye];
    }
    // Item 4: the private b0's rows 4..7 as written, staged for the re-measure; the record notes what was applied.
    if (g_fix.slot >= 0 && g_fix.slot == g_staging.openSlot() && g_recFrame[g_fix.slot] == g_fix.frame && g_fix.rec < g_recs[g_fix.slot].size()) {
        Rec& r = g_recs[g_fix.slot][g_fix.rec];
        const int64_t at = g_staging.append(ctx, g_privCb, kMarkerRowsOffset, kMarkerRowsBytes);
        r.fixApplied = true;
        r.fixHeld = g_fix.held;
        r.fixDx = static_cast<float>(g_fix.dx);
        r.fixDy = static_cast<float>(g_fix.dy);
        r.fixCx = static_cast<float>(g_fix.cx);
        r.fixCy = static_cast<float>(g_fix.cy);
        r.fixHead = static_cast<float>(g_fix.head);
        r.patchedAt = static_cast<int32_t>(at);
        r.fixRef = static_cast<uint8_t>(g_fix.ref);
    }
    if (!g_fixAppliedNoted) {
        g_fixAppliedNoted = true;
        Log::get().note("native on foot marker fix: first patched marker composite draw (eye %d, frame %u, key slot %d, reference %s): dx %+.5f, "
                        "dy %+.5f NDC%s", g_fix.eye, g_fix.frame, g_fix.keySlot, g_fix.ref == drawtag::kRefTemporal ? "temporal" : "world_first",
                        g_fix.dx, g_fix.dy, g_fix.held ? " (held)" : "");
    }
    return true;
}

void nativeOnFootMarkerFixEnd(ID3D11DeviceContext* ctx) {
    using namespace f44;
    if (!ctx || !g_fix.bound) return;
    ctx->VSSetConstantBuffers(0, 1, &g_fix.saved);
    if (g_fix.saved) g_fix.saved->Release();
    g_fix.saved = nullptr;
    g_fix.bound = false;
}

void nativeOnFootMarkerProbeShutdown() {
    f44::g_staging.release();
    if (f44::g_fix.saved) f44::g_fix.saved->Release();
    f44::g_fix = f44::Fix{};
    for (f44::KeyState& k : f44::g_keys) k = f44::KeyState{};   // F50/F52: every key's baselines
    f44::g_keyIdx = drawtag::KeyIndex<f44::kKeySlots>{};
    if (f44::g_privCb) f44::g_privCb->Release();
    if (f44::g_workBuf) f44::g_workBuf->Release();
    f44::g_privCb = f44::g_workBuf = nullptr;
}

void nativeOnFootFrameBoundary(uint32_t frameNo) {
    g_f44Frame.store(frameNo, std::memory_order_relaxed);   // F44: the marker probe's frame (the tag's frame bits, the captures' frame)
    if (g_install == InstallState::NotTried) installOnce();
    if (g_install == InstallState::Armed) readKey();
    drainEvents();
    if (g_nativeOnFootMarkerProbeKey.load(std::memory_order_relaxed)) f44::report(GetTickCount64());
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
    f54::tick(now);          // F54 (TEMPORARY): the held weapon's MGMC and record-submit hooks (measure always; shift with the FP raise), its lines
    pfzTick(now);            // F49 (TEMPORARY): the pose freeze's Animate post-hook (installed unless the key is off), its gate, blend and lines
    armIkTick(now);          // F56 (TEMPORARY): the arm IK's base weight (nudge and on only), its refusals and its 5 s lines
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
