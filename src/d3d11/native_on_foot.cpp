#include "native_on_foot.h"

#include <windows.h>
#include <intrin.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "binding_shadow.h"   // bindingShaderHash: the HUD issuer probe's pixel shader
#include "explorer_cam.h"   // localAvatarParts, avatarFadeModeRead, avatarCensusTake: the avatar census (F1, F3)
#include "journal_watch.h"
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
std::atomic<bool> g_reticleKeyOn{false};                        // fix.native_on_foot_reticle_probe, live (F14, temporary)
bool g_reticleKeyLast = false;                                  // frame thread only (F14)
std::atomic<float> g_hudSize{nofoot::kHudSizeDefault};          // fix.native_on_foot_hud_size, live (F13)
std::atomic<bool> g_followHead{true};                           // fix.native_on_foot_hud_follow is head (F13)
std::atomic<bool> g_followBefore{false};                        // fix.native_on_foot_hud_follow_order is before (F13, temporary)
std::atomic<int> g_followSmoothMs{nofoot::kHudSmoothDefault};   // fix.native_on_foot_hud_follow_smoothing_ms, live (F13)
std::atomic<bool> g_visorKeyOn{false};                          // fix.native_on_foot_visor_up, as the frame thread last read it (F13, temporary)
float g_hudSizeLast = -1.0f;                                    // frame thread only: as last logged (F13)
bool g_followHeadLast = true, g_followBeforeLast = false, g_visorKeyLast = false;   // frame thread only (F13)
std::atomic<float> g_hudDistance{nofoot::kHudDefaultDistance};   // fix.native_on_foot_hud_distance, live (F12)
float g_hudDistanceLast = -1.0f;                   // frame thread only: as last logged (F12)
std::atomic<bool> g_meshCreateLive{false};         // the creation patch is applied (set by the frame thread; read by the P2 hook, F12)
std::atomic<bool> g_meshWorldKeyOn{false};         // fix.native_on_foot_hud_mesh_world, as the frame thread last read it (F11)
bool g_panelsKeyLast = false, g_meshKeyLast = false, g_meshWorldKeyLast = false;   // frame thread only: as last logged
std::atomic<bool> g_hudKeyOn{false};               // fix.native_on_foot_hud, as the frame thread last read it (F8)
bool g_hudKeyLast = false;                         // frame thread only: the hud key as last logged
std::atomic<bool> g_keyOn{false};                  // fix.native_on_foot, as the frame thread last read it
std::atomic<bool> g_methodGate{true};              // fix.native_on_foot_method is gate (F2); the setter substitutes only when it is swap
std::atomic<bool> g_lastForwardedHmd{false};       // the most recent setter call forwarded an HMD mode (3 or 4): the gate method's flag reads it
uintptr_t g_base = 0;
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
    in.gateMethod = g_methodGate.load(std::memory_order_relaxed);
    in.incoming = incoming;
    in.selectorValid = valid;
    in.selectorFrozen = valid && frozen == selector;
    in.gameSession = journalGameplay();
    ev.reason = nofoot::admit(in);
    ev.admitted = ev.reason == nofoot::Reason::Admitted;
    ev.keyOn = in.keyOn;
    ev.incoming = incoming;
    ev.effective = nofoot::effectiveMode(ev.reason, incoming);
    ev.onFoot = journalOnFootKnown() ? (journalOnFoot() ? 1 : 0) : 2;
    ev.seated = journalSeatedKnown() ? (journalSeated() ? 1 : 0) : 2;
    // The flag is set before the call, so a frame boundary that runs while the original is inside it sees the session.
    // A call that is not substituted ends the flag: an HMD mode (the game's own stereo), or a cinema mode the game asks for while
    // the key is off or the call was refused (the game is back on the flat screen).
    // Under the gate method the flag is the frame thread's to compute (nativeOnFootActive, native_on_foot.h): the setter never sets it there.
    if (!in.gateMethod) {
        if (ev.admitted) g_nativeOnFootActive.store(true, std::memory_order_relaxed);
        else if (nofoot::isHmdMode(incoming) || nofoot::isCinemaMode(incoming)) g_nativeOnFootActive.store(false, std::memory_order_relaxed);
    }
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
    // TEMPORARY A/B (F2): swap substitutes in the setter (F0's behaviour); gate writes one byte and lets the game's own requester ask for HMD.
    const std::string method = cfg.getString("fix.native_on_foot_method", "gate");
    bool gate = true;
    if (method == "swap") {
        gate = false;
    } else if (method != "gate") {
        Log::get().note("native on foot: fix.native_on_foot_method = %s is not swap or gate; using gate.", method.c_str());
    }
    g_methodGate.store(gate, std::memory_order_relaxed);
    if (first || gate != g_methodLast) {
        Log::get().note("native on foot: fix.native_on_foot_method is %s (%s)", gate ? "gate" : "swap",
                        gate ? "the setter never substitutes; one byte at EliteDangerous64.exe+0x5E82B3E is held at 0 while the key is on and a game session is live"
                             : "the setter substitutes an on-foot Cinema request as HMD stereo, as F0 did");
        g_methodLast = gate;
    }
    // F8 (TEMPORARY): the first-person HUD override, on or off, live. hudOverrideTick reads the key each frame.
    const bool hudOn = cfg.getString("fix.native_on_foot_hud", "off") == "on";
    g_hudKeyOn.store(hudOn, std::memory_order_relaxed);
    if (first || hudOn != g_hudKeyLast) {
        Log::get().note("native on foot: fix.native_on_foot_hud is %s", hudOn ? "on" : "off");
        g_hudKeyLast = hudOn;
    }
    // F9 (TEMPORARY): the two code patches' keys, live.
    const bool panelsOn = cfg.getString("fix.native_on_foot_panels", "off") == "on";
    const bool meshOn = cfg.getString("fix.native_on_foot_hud_mesh", "off") == "on";
    g_panelsKeyOn.store(panelsOn, std::memory_order_relaxed);
    g_meshKeyOn.store(meshOn, std::memory_order_relaxed);
    if (first || panelsOn != g_panelsKeyLast) {
        Log::get().note("native on foot: fix.native_on_foot_panels is %s", panelsOn ? "on" : "off");
        g_panelsKeyLast = panelsOn;
    }
    if (first || meshOn != g_meshKeyLast) {
        Log::get().note("native on foot: fix.native_on_foot_hud_mesh is %s", meshOn ? "on" : "off");
        g_meshKeyLast = meshOn;
    }
    // F11 (TEMPORARY): the world placement A/B, live.
    const bool worldOn = cfg.getString("fix.native_on_foot_hud_mesh_world", "off") == "on";
    g_meshWorldKeyOn.store(worldOn, std::memory_order_relaxed);
    if (first || worldOn != g_meshWorldKeyLast) {
        Log::get().note("native on foot: fix.native_on_foot_hud_mesh_world is %s", worldOn ? "on" : "off");
        g_meshWorldKeyLast = worldOn;
    }
    // F12: the HUD distance (a user setting, live), clamped to its range; logged once per change.
    float dist = cfg.getFloat("fix.native_on_foot_hud_distance", nofoot::kHudDefaultDistance);
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
    const bool followBefore = cfg.getString("fix.native_on_foot_hud_follow_order", "after") == "before";
    g_followBefore.store(followBefore, std::memory_order_relaxed);
    if (first || followBefore != g_followBeforeLast) {
        Log::get().note("native on foot hud follow order: %s (temporary)", followBefore ? "before" : "after");
        g_followBeforeLast = followBefore;
    }
    const bool visorOn = cfg.getString("fix.native_on_foot_visor_up", "off") == "on";
    g_visorKeyOn.store(visorOn, std::memory_order_relaxed);
    if (first || visorOn != g_visorKeyLast) {
        Log::get().note("native on foot: fix.native_on_foot_visor_up is %s", visorOn ? "on" : "off");
        g_visorKeyLast = visorOn;
    }
    // F14: the HUD spread (a user setting, live) and the reticle probe key (temporary).
    float spread = cfg.getFloat("fix.native_on_foot_hud_spread", 0.0f);
    if (spread < 0.0f) spread = 0.0f;
    if (spread > 0.45f) spread = 0.45f;
    g_hudSpread.store(spread, std::memory_order_relaxed);
    if (first || spread != g_hudSpreadLast) {
        Log::get().note("native on foot hud spread: %.2f (pulls the elements toward the centre)", static_cast<double>(spread));
        g_hudSpreadLast = spread;
    }
    const bool reticleOn = cfg.getString("fix.native_on_foot_reticle_probe", "off") == "on";
    g_reticleKeyOn.store(reticleOn, std::memory_order_relaxed);
    if (first || reticleOn != g_reticleKeyLast) {
        Log::get().note("native on foot: fix.native_on_foot_reticle_probe is %s", reticleOn ? "on" : "off");
        g_reticleKeyLast = reticleOn;
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

void avatarCensusLine(uint64_t nowMs) {
    if (nowMs - g_lastFpMs < 5000) return;
    g_lastFpMs = nowMs;
    static AvatarCensusRow rows[kFpRows];   // about 18 KB, frame thread only
    uint32_t seen = 0, dropped = 0;
    const uint32_t n = avatarCensusTake(rows, kFpRows, &seen, &dropped);   // clears the window even when the key is off
    if (!nativeOnFootActive()) return;
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
// fix.native_on_foot_hud_mesh_world (A/B): the placement branch, 74 -> EB at +0x1A36453 (verify 74 2A).
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
MeshSite g_meshWorld{"hud mesh world", 0x1A36453, 2, {0x74, 0x2A, 0, 0, 0, 0}, {0xEB, 0x2A, 0, 0, 0, 0}, 0x1A36453, 0x74, 0xEB, "74 -> EB"};

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
std::atomic_flag g_f9Lock = ATOMIC_FLAG_INIT;
struct F9Lock {
    F9Lock() noexcept {
        while (g_f9Lock.test_and_set(std::memory_order_acquire)) _mm_pause();
    }
    ~F9Lock() { g_f9Lock.clear(std::memory_order_release); }
};
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
    bool before = false;
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
};
F13Head g_f13Raw;                                   // guarded by g_f9Lock: the head transform read by the P2 hook
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
void f13ReadHead(uintptr_t listB) noexcept {
    uint64_t begin = 0, end = 0;
    if (!guardedRead(listB, &begin, 8) || !guardedRead(listB + 8, &end, 8)) {
        f12Stand("list B could not be read");
        return;
    }
    if (begin < 0x10000 || end < begin || end - begin < 16) {
        f12Stand("list B does not hold two views");
        return;
    }
    uint64_t v0 = 0, v1 = 0;
    if (!guardedRead(static_cast<uintptr_t>(begin), &v0, 8) || !guardedRead(static_cast<uintptr_t>(begin) + 8, &v1, 8)) {
        f12Stand("list B could not be read");
        return;
    }
    auto plausible = [](uint64_t p) { return p >= 0x10000 && p < 0x00007FFF00000000ull; };
    if (!plausible(v0) || !plausible(v1)) {
        f12Stand("list B entries are not view pointers");
        return;
    }
    float a[16] = {}, b[16] = {};
    if (!guardedRead(static_cast<uintptr_t>(v0) + nofoot::kF9ViewRotOff, a, sizeof(a)) ||
        !guardedRead(static_cast<uintptr_t>(v1) + nofoot::kF9ViewRotOff, b, sizeof(b))) {
        f12Stand("a list-B view could not be read");
        return;
    }
    F13Head raw;
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
    F9Lock lock;
    g_f13Raw = raw;
}

// The inputs of a placement, for the no-compounding comparison.
bool f13SameInputs(const F13Inputs& a, const F13Inputs& b) noexcept {
    if (!f12Near(a.s, b.s) || !f12Near(a.size, b.size) || a.head != b.head || a.before != b.before) return false;
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
        if (in.before) f13Mul(in.C, S, T);
        else f13Mul(S, in.C, T);
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
    const float size = g_hudSize.load(std::memory_order_relaxed);
    const bool wantHead = g_followHead.load(std::memory_order_relaxed);
    const bool before = g_followBefore.load(std::memory_order_relaxed);
    const uintptr_t h = reinterpret_cast<uintptr_t>(hrcPtr);
    if (!h) return;
    if (wantHead) {
        f13ReadHead(listB);
        if (g_f12Stop.load(std::memory_order_relaxed)) return;
    }
    F13Inputs in;
    in.s = s;
    in.size = size;
    in.before = before;
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
// While native stereo is on foot, fix.native_on_foot_visor_up is on, and the identity check passes: both bytes must read 0; +0x5F741B8 is
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

// LAYOUT PROBE (read only). EliteDangerous64.exe+0x1A0DCD0 (args rcx = HRC, rdx = float out[2], r8 = &rec; rec+0 = const char* part name), called
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

void f14LogPart(void* hrc, void* out, void* rec) noexcept {
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
    char line[240];
    std::snprintf(line, sizeof(line),
                  "native on foot hud part: name=%s anchor=(%.3f,%.3f) off=(%.3f,%.3f) align=(%u,%u) scale=%.3f out=(%.3f,%.3f) canvas=%.0fx%.0f "
                  "hrc=(%.3f,%.3f,%.3f) calls=%llu",
                  name, static_cast<double>(rec8), static_cast<double>(recC), static_cast<double>(rec10), static_cast<double>(rec14),
                  okB ? static_cast<unsigned>(b18) : 0u, okB ? static_cast<unsigned>(b19) : 0u, static_cast<double>(rec1C),
                  static_cast<double>(out0), static_cast<double>(out1), static_cast<double>(canvasW), static_cast<double>(canvasH),
                  static_cast<double>(hrc4C4), static_cast<double>(hrc4C8), static_cast<double>(hrc4CC), static_cast<unsigned long long>(n));
    f14Queue(line);
}

uint64_t __fastcall layoutHooked(void* hrc, void* out, void* rec, uint64_t a4, uint64_t a5, uint64_t a6) {
    const F9Fn6 original = reinterpret_cast<F9Fn6>(g_f14Layout.original);
    if (!original) return 0;
    const uint64_t rax = original(hrc, out, reinterpret_cast<uint64_t>(rec), a4, a5, a6);
    if (!g_f14LayoutStop.load(std::memory_order_relaxed)) f14LogPart(hrc, out, rec);
    return rax;
}

// RETICLE INDICATOR PROBE (read only). The stub below calls this with rcx = the instance and rdx = the node of the marker component's call at
// EliteDangerous64.exe+0x1A15644 (the call to 0x434ECC0, preceded by mov rcx,[rsi+0x60]). Nothing is written to the instance.
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

// The stub: push rdx; sub rsp,0x20; mov rdx,rsi; mov rax,probe; call rax; add rsp,0x20; pop rdx; mov rcx,[rsi+0x60]; mov rax,real; jmp rax.
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
            const uint8_t mid[] = {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20, 0x5A, 0x48, 0x8B, 0x4E, 0x60, 0x48, 0xB8};
            std::memcpy(p + i, mid, sizeof(mid));
            i += sizeof(mid);
            const uint64_t real = g_base + kReticleFnRva;
            std::memcpy(p + i, &real, 8);
            i += 8;
            const uint8_t tail[] = {0xFF, 0xE0};
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

// P2: rcx = HRC, and five more arguments, all forwarded (the later chunks were not scanned for stack arguments).
uint64_t __fastcall f9HrcSubmitHooked(void* hrc, void* a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    const F9Fn6 original = reinterpret_cast<F9Fn6>(g_f9Probe[1].original);
    if (!original) return 0;
    if (!g_f9Stop[1].load(std::memory_order_relaxed)) f9RecordHrc(f9State(), hrc);   // reads before the call
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
}  // namespace

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
    const bool session = journalGameplay();
    // The native flag. Under the gate method it is computed here (native_on_foot.h, THE FLAG): the byte is ours, the game's last forwarded
    // mode was HMD, and the journal says on foot. Under swap the setter sets and clears it, as F0 did.
    if (gate) {
        g_nativeOnFootActive.store(g_gateWritten && g_lastForwardedHmd.load(std::memory_order_relaxed) && journalOnFootKnown() && journalOnFoot(),
                                   std::memory_order_relaxed);
    } else if (g_methodWasGate) {
        g_nativeOnFootActive.store(false, std::memory_order_relaxed);
    }
    g_methodWasGate = gate;
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
    meshSiteTick(g_meshWorld, g_meshWorldKeyOn.load(std::memory_order_relaxed) && f9Active,
                 g_meshWorldKeyOn.load(std::memory_order_relaxed) ? f9Why : "fix.native_on_foot_hud_mesh_world is off");
    f9ProbeTick(now);
    f13FrameTick(now);
    // F14: the layout probe, its lines, the HUD spread, and the reticle probe (temporary).
    installF14ProbesOnce();
    f14FrameTick(now);
    {
        const float spreadNow = g_hudSpread.load(std::memory_order_relaxed);
        const bool spreadWant = spreadNow > 0.0f && nativeOnFootActive() && g_meshCreateLive.load(std::memory_order_relaxed) && session;
        spreadTick(spreadWant, spreadNow,
                   !(spreadNow > 0.0f) ? "the HUD spread is 0" : !nativeOnFootActive() ? "native stereo on foot ended"
                   : !g_meshCreateLive.load(std::memory_order_relaxed) ? "the HUD mesh is off" : "the game session ended");
        const bool reticleKey = g_reticleKeyOn.load(std::memory_order_relaxed);
        const bool reticleWant = reticleKey && nativeOnFootActive() && session;
        reticleTick(reticleWant, !reticleKey ? "fix.native_on_foot_reticle_probe is off" : !nativeOnFootActive() ? "native stereo on foot ended" : "the game session ended");
    }
    // F13 (TEMPORARY): the visor override, while native stereo is on foot and its test key is on.
    const bool visorKeyOn = g_visorKeyOn.load(std::memory_order_relaxed);
    const bool visorWant = visorKeyOn && nativeOnFootActive() && session;
    visorTick(visorWant, !visorKeyOn ? "fix.native_on_foot_visor_up is off" : !nativeOnFootActive() ? "native stereo on foot ended" : "the game session ended");
    neckLine(now);
}

void nativeOnFootShutdown() {
    gateByteTick(false, "the game is shutting down");
    hudOverrideTick(false, "the game is shutting down");
    codePatchTick(g_panelPatch, false, "the game is shutting down");
    meshSiteTick(g_meshCreate, false, "the game is shutting down");
    meshSiteTick(g_meshWorld, false, "the game is shutting down");
    visorTick(false, "the game is shutting down");
    spreadTick(false, 0.0f, "the game is shutting down");
    reticleTick(false, "the game is shutting down");
}

}  // namespace edvr
