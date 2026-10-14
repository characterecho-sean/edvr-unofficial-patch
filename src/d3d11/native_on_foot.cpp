#include "native_on_foot.h"

#include <windows.h>
#include <intrin.h>

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
bool g_panelsKeyLast = false, g_meshKeyLast = false;   // frame thread only: as last logged
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

// ---- F10: the mesh unit (TEMPORARY; approved by Sean 2026-10-14) ----------------------------------------------------------------------
// Two sites, applied and restored together: site 1 (+0x1A36453) 74 2A -> EB 2A (the first byte); site 2 (+0x1A149A8) the byte 80 -> 78 in the
// six bytes FF 90 80 00 00 00 at +0x1A149A6. Both are verified before either is written; if the second write fails, the first is put back.
// A restore writes back only when both read as this unit left them. Scope as the panels patch.
bool g_meshApplied = false;               // frame thread only
std::string g_meshRefusal;                // frame thread only
bool g_meshRestoreNoted = false;          // frame thread only

void refuseMesh(const std::string& why) {
    if (why == g_meshRefusal) return;
    g_meshRefusal = why;
    Log::get().note("native on foot hud mesh: patch EliteDangerous64.exe+0x%llX, +0x%llX NOT patched -- %s. The game's bytes are left as they are.",
                    static_cast<unsigned long long>(nofoot::kMeshPatchRva), static_cast<unsigned long long>(nofoot::kMeshSite2Rva + nofoot::kMeshSite2ByteOff),
                    why.c_str());
}

// `want`: the unit should be applied now. `reason` (a literal) says why it is being put back.
void meshUnitTick(bool want, const char* reason) {
    const uintptr_t a1 = g_base + nofoot::kMeshPatchRva;
    const uintptr_t a2 = g_base + nofoot::kMeshSite2Rva;
    const uintptr_t b2 = a2 + nofoot::kMeshSite2ByteOff;
    if (want) {
        if (g_meshApplied || g_install != InstallState::Armed) return;
        const char* idWhy = nullptr;
        if (!checkIdentity(g_base, &idWhy)) {
            refuseMesh(std::string("the PE identity check failed (") + idWhy + ")");
            return;
        }
        uint8_t c1[2] = {};
        if (!guardedRead(a1, c1, 2)) {
            refuseMesh("site 1 could not be read");
            return;
        }
        if (c1[0] != nofoot::kMeshPatchFrom[0] || c1[1] != nofoot::kMeshPatchFrom[1]) {
            char b[160];
            std::snprintf(b, sizeof(b), "site 1 reads %02X %02X, not 74 2A, so it is left alone", static_cast<unsigned>(c1[0]), static_cast<unsigned>(c1[1]));
            refuseMesh(b);
            return;
        }
        uint8_t c2[6] = {};
        if (!guardedRead(a2, c2, 6)) {
            refuseMesh("site 2 could not be read");
            return;
        }
        for (size_t i = 0; i < 6; ++i) {
            if (c2[i] != nofoot::kMeshSite2Expect[i]) {
                char b[200];
                std::snprintf(b, sizeof(b), "site 2 reads %02X %02X %02X %02X %02X %02X, not FF 90 80 00 00 00, so it is left alone",
                              static_cast<unsigned>(c2[0]), static_cast<unsigned>(c2[1]), static_cast<unsigned>(c2[2]), static_cast<unsigned>(c2[3]),
                              static_cast<unsigned>(c2[4]), static_cast<unsigned>(c2[5]));
                refuseMesh(b);
                return;
            }
        }
        if (!storeCodeBytes(a1, 0xEB, 1)) {
            refuseMesh("site 1 is not writable");
            return;
        }
        if (!storeCodeBytes(b2, nofoot::kMeshSite2Patched[nofoot::kMeshSite2ByteOff], 1)) {
            storeCodeBytes(a1, nofoot::kMeshPatchFrom[0], 1);   // put site 1 back: the unit is all or nothing
            refuseMesh("site 2 is not writable (site 1 put back)");
            return;
        }
        g_meshApplied = true;
        g_meshRefusal.clear();
        Log::get().note("native on foot hud mesh: patch EliteDangerous64.exe+0x%llX 74 -> EB, +0x%llX 80 -> 78",
                        static_cast<unsigned long long>(nofoot::kMeshPatchRva), static_cast<unsigned long long>(b2));
        return;
    }
    if (!g_meshApplied) {
        g_meshRefusal.clear();
        return;
    }
    uint8_t c1[2] = {};
    uint8_t c2[6] = {};
    if (!guardedRead(a1, c1, 2) || !guardedRead(a2, c2, 6)) return;   // retried next frame
    bool site2Ours = true;
    for (size_t i = 0; i < 6; ++i) site2Ours = site2Ours && c2[i] == nofoot::kMeshSite2Patched[i];
    if (!(c1[0] == nofoot::kMeshPatchTo[0] && c1[1] == nofoot::kMeshPatchTo[1]) || !site2Ours) {
        g_meshApplied = false;
        Log::get().note("native on foot hud mesh: patch NOT restored -- the bytes are not this unit's (site 1 %02X %02X, site 2 %02X %02X %02X %02X %02X %02X; restore: %s)",
                        static_cast<unsigned>(c1[0]), static_cast<unsigned>(c1[1]), static_cast<unsigned>(c2[0]), static_cast<unsigned>(c2[1]),
                        static_cast<unsigned>(c2[2]), static_cast<unsigned>(c2[3]), static_cast<unsigned>(c2[4]), static_cast<unsigned>(c2[5]), reason);
        return;
    }
    if (!storeCodeBytes(b2, nofoot::kMeshSite2Expect[nofoot::kMeshSite2ByteOff], 1) || !storeCodeBytes(a1, nofoot::kMeshPatchFrom[0], 1)) {
        if (!g_meshRestoreNoted) {
            g_meshRestoreNoted = true;
            Log::get().note("native on foot hud mesh: patch could not be restored (%s); retrying each frame.", reason);
        }
        return;
    }
    g_meshApplied = false;
    g_meshRestoreNoted = false;
    Log::get().note("native on foot hud mesh: patch EliteDangerous64.exe+0x%llX EB -> 74, +0x%llX 78 -> 80 (restored: %s)",
                    static_cast<unsigned long long>(nofoot::kMeshPatchRva), static_cast<unsigned long long>(b2), reason);
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
    ++s.window;
}

void f9RecordInst(uint8_t st, uint8_t k, void* view) noexcept {
    const uintptr_t v = reinterpret_cast<uintptr_t>(view);
    uint32_t vid = 0;
    const bool okId = v && f9Read(2, v + nofoot::kF9ViewIdOff, &vid, 4);
    float cam[3] = {};
    const bool okCam = v && f9Read(2, v + nofoot::kF9ViewCamOff, cam, 12);
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
    if (okCam) {
        row->cam[0] = cam[0];
        row->cam[1] = cam[1];
        row->cam[2] = cam[2];
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

// P2: rcx = HRC, and five more arguments, all forwarded (the later chunks were not scanned for stack arguments).
uint64_t __fastcall f9HrcSubmitHooked(void* hrc, void* a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    const F9Fn6 original = reinterpret_cast<F9Fn6>(g_f9Probe[1].original);
    if (!original) return 0;
    if (!g_f9Stop[1].load(std::memory_order_relaxed)) f9RecordHrc(f9State(), hrc);   // reads before the call
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

bool installF9Probe(size_t i, const F9ProbeSpec& s, uintptr_t base, size_t* stolenOut, std::string* whyOut) {
    F9Probe& pr = g_f9Probe[i];
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

// Frame thread, every frame: every 5 s the window's panel callers, HRC and instance rows; and the HRC line whenever its values change.
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
    for (const F9InstRow& r : insts) {
        if (!r.used || r.window == 0) continue;
        char view[24], cam[64];
        if (r.viewOk) std::snprintf(view, sizeof(view), "%u", static_cast<unsigned>(r.viewId));
        else std::snprintf(view, sizeof(view), "?");
        if (r.camOk) std::snprintf(cam, sizeof(cam), "(%.3f,%.3f,%.3f)", static_cast<double>(r.cam[0]), static_cast<double>(r.cam[1]), static_cast<double>(r.cam[2]));
        else std::snprintf(cam, sizeof(cam), "?");
        Log::get().note("native on foot hrc submit [%s]: inst=%u view=%s calls=%llu campos=%s", g_f9StateName[r.st], static_cast<unsigned>(r.k), view,
                        static_cast<unsigned long long>(r.window), cam);
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
    meshUnitTick(g_meshKeyOn.load(std::memory_order_relaxed) && f9Active,
                  g_meshKeyOn.load(std::memory_order_relaxed) ? f9Why : "fix.native_on_foot_hud_mesh is off");
    f9ProbeTick(now);
    neckLine(now);
}

void nativeOnFootShutdown() {
    gateByteTick(false, "the game is shutting down");
    hudOverrideTick(false, "the game is shutting down");
    codePatchTick(g_panelPatch, false, "the game is shutting down");
    meshUnitTick(false, "the game is shutting down");
}

}  // namespace edvr
