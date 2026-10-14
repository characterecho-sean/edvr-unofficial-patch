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
std::atomic<bool> g_keyOn{false};                  // fix.native_on_foot, as the frame thread last read it
std::atomic<bool> g_methodGate{true};              // fix.native_on_foot_method is gate (F2); the setter substitutes only when it is swap
std::atomic<bool> g_lastForwardedHmd{false};       // the most recent setter call forwarded an HMD mode (3 or 4): the gate method's flag reads it
uintptr_t g_base = 0;                             // the game image base; set by installOnce before the hook can run

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

// ---- F6: two read-only eye-index probes (TEMPORARY; they go when the arc closes) --------------------------------------------------------
// E1 (+0x2842FD0, the eye-index query; returns 0, 1 or 2) and E2 (+0x28540B0, the stereo leaf predicate; returns al). Each calls the
// original first, then reads game memory under SEH, and returns the original's result unchanged. One relay page and one gate per probe;
// the identity check, the prologue bytes and the instruction boundary of the stolen prefix are checked before any patch. Sixteen faults
// stop a probe's reads for the session. Aggregates are per 5 s window and per state; a key is announced once, when first seen. No game
// thread writes a line: first sightings are queued and written at the next frame boundary.
std::atomic_flag g_hudLock = ATOMIC_FLAG_INIT;   // a tiny spinlock: the keys, the window counters and the first-sighting queue
struct HudLockGuard {
    HudLockGuard() noexcept {
        while (g_hudLock.test_and_set(std::memory_order_acquire)) _mm_pause();
    }
    ~HudLockGuard() { g_hudLock.clear(std::memory_order_release); }
};

constexpr uint8_t kStateNative = 0, kStateCinema = 1, kStateOther = 2;
const char* const kStateName[3] = {"native", "cinema", "ship/other"};
// The state of one call: native stereo on foot, else cinema (on foot, key off), else the rest.
uint8_t probeState() noexcept {
    if (nativeOnFootActive()) return kStateNative;
    if (journalOnFootKnown() && journalOnFoot()) return kStateCinema;
    return kStateOther;
}

using EyeFn = uint64_t(__fastcall*)(void*, void*, uint64_t, uint64_t);

// One probe: its relay page, its gate, the trampoline (the original), and its CodeHook (never deleted, as g_hook).
struct EyeProbe {
    uint8_t* relay = nullptr;
    alignas(8) std::atomic<uintptr_t> gate{0};
    EyeFn original = nullptr;
    CodeHook* hook = nullptr;
};
EyeProbe g_eyeProbe[nofoot::kEyeCount];
std::atomic<bool> g_eyeStop[nofoot::kEyeCount]{};
std::atomic<uint32_t> g_eyeFaults[nofoot::kEyeCount]{};
std::atomic<uintptr_t> g_eyeBase{0};          // the image base, set before either probe is patched
bool g_eyeTried = false;                      // frame thread only
bool g_eyeArmed[nofoot::kEyeCount] = {};      // frame thread only
uint64_t g_eyeWindowMs = 0;                   // frame thread only

constexpr uint32_t kModeUnread = 0xFFFFFFFFu;  // mode not read
constexpr uint8_t kFlagUnread = 0xFF;          // [S+0x2AE0] not read (and always for E1)
constexpr uint8_t kClassSlot0 = 0, kClassSlot1 = 1, kClassOther = 2, kClassNone = 3;
constexpr size_t kEyeKeys = 64;
constexpr size_t kEyeRing = 32;

// A key: (probe, state, caller RVA, node class, result, mode, flag). `calls` is the session total; `windowCalls` is the window's.
struct EyeKey {
    bool used = false;
    uint8_t hook = 0, state = 0, cls = kClassNone, result = 0, flag = kFlagUnread;
    uint32_t caller = 0;   // RVA; 0 when the caller is outside the image
    uint32_t mode = kModeUnread;
    uint64_t calls = 0, windowCalls = 0;
};
EyeKey g_eyeKeys[kEyeKeys];                  // guarded by g_hudLock
uint64_t g_eyeWinCalls[nofoot::kEyeCount] = {};  // guarded by g_hudLock: calls this window, per probe
uint64_t g_eyeWinOverflow = 0;               // guarded by g_hudLock: calls with no free key this window
uint64_t g_eyeFrames = 0;                    // guarded by g_hudLock

// A first sighting, queued by the game's thread and written by the frame thread.
struct EyeFirst {
    EyeKey key;
};
EyeFirst g_eyeRing[kEyeRing];                // guarded by g_hudLock
uint64_t g_eyeWritten = 0, g_eyeRead = 0, g_eyeDropped = 0;

uint32_t eyeCallerRva(void* caller) noexcept {
    const uintptr_t base = g_eyeBase.load(std::memory_order_relaxed);
    const uintptr_t c = reinterpret_cast<uintptr_t>(caller);
    if (!base || c < base || c >= base + ecm::kExpectedImageSize) return 0;
    return static_cast<uint32_t>(c - base);
}

// A read under SEH. A fault counts against the probe; sixteen stop that probe's reads.
bool eyeRead(size_t probe, uintptr_t address, void* out, size_t bytes) noexcept {
    if (guardedRead(address, out, bytes)) return true;
    if (g_eyeFaults[probe].fetch_add(1, std::memory_order_relaxed) + 1 >= 16) g_eyeStop[probe].store(true, std::memory_order_relaxed);
    return false;
}

// Counts one call under the key (hook, state, caller, class, result, mode, flag). A new key is queued for its first-sighting line.
void eyeKeyNote(uint8_t hook, uint8_t st, uint32_t caller, uint8_t cls, uint8_t result, uint32_t mode, uint8_t flag) noexcept {
    HudLockGuard lock;
    ++g_eyeWinCalls[hook];
    EyeKey* key = nullptr;
    for (size_t i = 0; i < kEyeKeys; ++i) {
        const EyeKey& k = g_eyeKeys[i];
        if (k.used && k.hook == hook && k.state == st && k.caller == caller && k.cls == cls && k.result == result && k.mode == mode && k.flag == flag) {
            key = &g_eyeKeys[i];
            break;
        }
    }
    if (!key) {
        for (size_t i = 0; i < kEyeKeys && !key; ++i) {
            if (!g_eyeKeys[i].used) key = &g_eyeKeys[i];
        }
        if (!key) {
            ++g_eyeWinOverflow;
            return;
        }
        key->used = true;
        key->hook = hook;
        key->state = st;
        key->caller = caller;
        key->cls = cls;
        key->result = result;
        key->mode = mode;
        key->flag = flag;
        if (g_eyeWritten - g_eyeRead < kEyeRing) {
            g_eyeRing[g_eyeWritten % kEyeRing].key = *key;
            ++g_eyeWritten;
        } else {
            ++g_eyeDropped;
        }
    }
    ++key->calls;
    ++key->windowCalls;
}

// E1: rcx = this, rdx = node. The original runs first; then the scene, the mode, and the two slot nodes are read.
void eyeQueryRecord(uint8_t st, void* self, void* node, void* caller, uint64_t rax) noexcept {
    const uintptr_t sa = reinterpret_cast<uintptr_t>(self);
    uint64_t scene = 0, w = 0, n0 = 0, n1 = 0;
    uint32_t mode = kModeUnread;
    bool okScene = eyeRead(0, sa + nofoot::kEyeSceneOff, &scene, 8) && scene >= 0x10000;
    if (okScene && !eyeRead(0, static_cast<uintptr_t>(scene) + nofoot::kEyeModeOff, &mode, 4)) mode = kModeUnread;
    bool okN0 = false, okN1 = false;
    if (okScene && eyeRead(0, static_cast<uintptr_t>(scene) + nofoot::kEyeWOff, &w, 8) && w >= 0x10000) {
        uint64_t s0 = 0, s1 = 0;
        if (eyeRead(0, static_cast<uintptr_t>(w) + nofoot::kEyeSlot0Off, &s0, 8) && s0 >= 0x10000)
            okN0 = eyeRead(0, static_cast<uintptr_t>(s0) + nofoot::kEyeNodeOff, &n0, 8);
        if (eyeRead(0, static_cast<uintptr_t>(w) + nofoot::kEyeSlot1Off, &s1, 8) && s1 >= 0x10000)
            okN1 = eyeRead(0, static_cast<uintptr_t>(s1) + nofoot::kEyeNodeOff, &n1, 8);
    }
    const uint64_t nodeV = reinterpret_cast<uint64_t>(node);
    uint8_t cls = kClassOther;
    if (okN0 && nodeV == n0) cls = kClassSlot0;
    else if (okN1 && nodeV == n1) cls = kClassSlot1;
    eyeKeyNote(0, st, eyeCallerRva(caller), cls, static_cast<uint8_t>(rax & 0xFF), mode, kFlagUnread);
}

// E2: rcx = this. The original runs first; then the scene, the mode and the flag byte are read.
void stereoPredRecord(uint8_t st, void* self, void* caller, uint64_t rax) noexcept {
    const uintptr_t sa = reinterpret_cast<uintptr_t>(self);
    uint64_t scene = 0;
    uint32_t mode = kModeUnread;
    uint8_t flag = kFlagUnread;
    if (eyeRead(1, sa + nofoot::kStereoSceneOff, &scene, 8) && scene >= 0x10000) {
        if (!eyeRead(1, static_cast<uintptr_t>(scene) + nofoot::kEyeModeOff, &mode, 4)) mode = kModeUnread;
        uint8_t f = 0;
        if (eyeRead(1, static_cast<uintptr_t>(scene) + nofoot::kStereoFlagOff, &f, 1)) flag = f;
    }
    eyeKeyNote(1, st, eyeCallerRva(caller), kClassNone, static_cast<uint8_t>(rax & 0xFF), mode, flag);
}

uint64_t __fastcall eyeQueryHooked(void* self, void* node, uint64_t a3, uint64_t a4) {
    const EyeFn original = g_eyeProbe[0].original;
    if (!original) return 0;
    void* caller = _ReturnAddress();
    const uint8_t st = probeState();
    const uint64_t rax = original(self, node, a3, a4);
    if (!g_eyeStop[0].load(std::memory_order_relaxed)) eyeQueryRecord(st, self, node, caller, rax);
    return rax;
}

uint64_t __fastcall stereoPredHooked(void* self, void* a2, uint64_t a3, uint64_t a4) {
    const EyeFn original = g_eyeProbe[1].original;
    if (!original) return 0;
    void* caller = _ReturnAddress();
    const uint8_t st = probeState();
    const uint64_t rax = original(self, a2, a3, a4);
    if (!g_eyeStop[1].load(std::memory_order_relaxed)) stereoPredRecord(st, self, caller, rax);
    return rax;
}

// The relay's trampoline and its executable state (CodeHook's Prepare, with the probe as its context).
bool prepareEyeRelay(void* trampoline, void* context) noexcept {
    EyeProbe* probe = static_cast<EyeProbe*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(probe->relay + ecm::kCallbackRelayTrampolineAt, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(probe->relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), probe->relay, ecm::kCallbackRelayBytes))
        return false;
    probe->original = reinterpret_cast<EyeFn>(trampoline);
    return true;
}

struct EyeProbeSpec {
    const char* name;
    uintptr_t rva;
    const uint8_t* prologue;
    size_t prologueBytes;
    size_t stolen;
    void* callback;
    const char* label;
};

// Installs probe `i`: the prologue bytes, the stolen length on an instruction boundary, a relay within reach, then CodeHook. Returns false
// with `whyOut` set, having changed nothing that remains.
bool installEyeProbe(size_t i, const EyeProbeSpec& s, uintptr_t base, size_t* stolenOut, std::string* whyOut) {
    EyeProbe& pr = g_eyeProbe[i];
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
    if (!pr.hook->install(reinterpret_cast<void*>(target), pr.relay, nullptr, s.name, &prepareEyeRelay, &pr)) {
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

// Frame thread, once: the VR profile only, behind the identity check. One line per probe, installed or refused.
void installEyeProbesOnce() {
    if (g_eyeTried) return;
    g_eyeTried = true;
    if (!runtimeVrProfile()) {
        Log::get().note("native on foot eye probes: not installed (the flat profile)");
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const char* peWhy = nullptr;
    if (!base || !checkIdentity(base, &peWhy)) {
        Log::get().note("native on foot eye probes: NOT installed -- identity mismatch (%s); the game was not touched.", peWhy ? peWhy : "no module");
        return;
    }
    g_eyeBase.store(base, std::memory_order_relaxed);
    const EyeProbeSpec specs[nofoot::kEyeCount] = {
        {"native-on-foot-eye-query", nofoot::kEyeQueryRva, nofoot::kEyeQueryPrologue, nofoot::kEyeQueryPrologueBytes, nofoot::kEyeQueryStolen,
         reinterpret_cast<void*>(&eyeQueryHooked), "E1 (eye-index query)"},
        {"native-on-foot-stereo-pred", nofoot::kStereoPredRva, nofoot::kStereoPredPrologue, nofoot::kStereoPredPrologueBytes,
         nofoot::kStereoPredStolen, reinterpret_cast<void*>(&stereoPredHooked), "E2 (stereo leaf predicate)"},
    };
    for (size_t i = 0; i < nofoot::kEyeCount; ++i) {
        const EyeProbeSpec& s = specs[i];
        size_t stolen = 0;
        std::string why;
        if (installEyeProbe(i, s, base, &stolen, &why)) {
            g_eyeArmed[i] = true;
            Log::get().note("native on foot eye probe: installed %s at EliteDangerous64.exe+0x%llX, stolen=%zu bytes, prologue %zu bytes "
                            "verified, relay at 0x%llX (read only; temporary).",
                            s.label, static_cast<unsigned long long>(s.rva), stolen, s.prologueBytes,
                            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_eyeProbe[i].relay)));
        } else {
            Log::get().note("native on foot eye probe: %s NOT installed at EliteDangerous64.exe+0x%llX -- %s. The game was not touched by it.",
                            s.label, static_cast<unsigned long long>(s.rva), why.c_str());
        }
    }
}

// The text of an eye key's caller, class, result, mode and flag.
std::string eyeCallerText(uint32_t caller) {
    if (!caller) return "?";
    char b[24];
    std::snprintf(b, sizeof(b), "+0x%llX", static_cast<unsigned long long>(caller));
    return b;
}
std::string eyeModeText(uint32_t mode) {
    if (mode == kModeUnread) return "?";
    return std::to_string(mode);
}
const char* eyeClassText(uint8_t cls) {
    return cls == kClassSlot0 ? "slot0" : cls == kClassSlot1 ? "slot1" : cls == kClassOther ? "other" : "-";
}

void eyeLogKey(const EyeKey& k, bool first) {
    const char* st = kStateName[k.state < 3 ? k.state : 2];
    if (k.hook == 0) {
        if (first) {
            Log::get().note("native on foot eye query [%s]: first sighting caller=%s node=%s result=%u mode=%s", st,
                            eyeCallerText(k.caller).c_str(), eyeClassText(k.cls), static_cast<unsigned>(k.result), eyeModeText(k.mode).c_str());
        } else {
            Log::get().note("native on foot eye query [%s]: caller=%s node=%s result=%u mode=%s calls=%llu", st, eyeCallerText(k.caller).c_str(),
                            eyeClassText(k.cls), static_cast<unsigned>(k.result), eyeModeText(k.mode).c_str(),
                            static_cast<unsigned long long>(k.windowCalls));
        }
    } else {
        char flag[8];
        if (k.flag == kFlagUnread) std::snprintf(flag, sizeof(flag), "?");
        else std::snprintf(flag, sizeof(flag), "%u", static_cast<unsigned>(k.flag));
        if (first) {
            Log::get().note("native on foot stereo pred [%s]: first sighting caller=%s result=%u mode=%s flag2AE0=%s", st,
                            eyeCallerText(k.caller).c_str(), static_cast<unsigned>(k.result), eyeModeText(k.mode).c_str(), flag);
        } else {
            Log::get().note("native on foot stereo pred [%s]: caller=%s result=%u mode=%s flag2AE0=%s calls=%llu", st,
                            eyeCallerText(k.caller).c_str(), static_cast<unsigned>(k.result), eyeModeText(k.mode).c_str(), flag,
                            static_cast<unsigned long long>(k.windowCalls));
        }
    }
}

// Frame thread, every frame: writes the queued first sightings at once, and every 5 s the window's keys (by state), the totals, then resets.
void eyeProbeTick(uint64_t nowMs) {
    if (!g_eyeArmed[0] && !g_eyeArmed[1]) return;
    if (g_eyeWindowMs == 0) g_eyeWindowMs = nowMs;
    EyeFirst first[kEyeRing];
    size_t nFirst = 0;
    uint64_t dropped = 0, frames = 0, calls[nofoot::kEyeCount] = {}, overflow = 0;
    bool due = false;
    EyeKey snap[kEyeKeys];
    size_t nSnap = 0;
    {
        HudLockGuard lock;
        while (g_eyeRead < g_eyeWritten && nFirst < kEyeRing) first[nFirst++] = g_eyeRing[(g_eyeRead++) % kEyeRing];
        dropped = g_eyeDropped;
        g_eyeDropped = 0;
        ++g_eyeFrames;
        due = nowMs - g_eyeWindowMs >= 5000;
        if (due) {
            for (size_t i = 0; i < kEyeKeys; ++i) {
                if (g_eyeKeys[i].used && g_eyeKeys[i].windowCalls) snap[nSnap++] = g_eyeKeys[i];
                g_eyeKeys[i].windowCalls = 0;
            }
            for (size_t p = 0; p < nofoot::kEyeCount; ++p) {
                calls[p] = g_eyeWinCalls[p];
                g_eyeWinCalls[p] = 0;
            }
            overflow = g_eyeWinOverflow;
            g_eyeWinOverflow = 0;
            frames = g_eyeFrames;
            g_eyeFrames = 0;
        }
    }
    for (size_t i = 0; i < nFirst; ++i) eyeLogKey(first[i].key, true);
    if (dropped) Log::get().note("native on foot eye: %llu first sighting(s) dropped (the queue of %zu was full)", static_cast<unsigned long long>(dropped), kEyeRing);
    if (!due) return;
    g_eyeWindowMs = nowMs;
    const double f = frames ? static_cast<double>(frames) : 1.0;
    Log::get().note("native on foot eye totals: frames=%llu eye_calls=%llu (%.2f per frame) stereo_calls=%llu (%.2f per frame) keys=%zu overflow=%llu",
                    static_cast<unsigned long long>(frames), static_cast<unsigned long long>(calls[0]), static_cast<double>(calls[0]) / f,
                    static_cast<unsigned long long>(calls[1]), static_cast<double>(calls[1]) / f, nSnap, static_cast<unsigned long long>(overflow));
    if (nSnap == 0) Log::get().note("native on foot eye: no calls in the window");
    for (size_t i = 0; i < nSnap; ++i) eyeLogKey(snap[i], false);
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
    installEyeProbesOnce();   // F6: the two read-only eye probes (VR profile; once)
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
    // The body (F2): Explorer Cam's fade and head-part code runs while native stereo is on foot. Its head hooks are wanted from launch while the key is on.
    const char* bodyWhy = !keyOn ? "fix.native_on_foot is off" : !session ? "the game session ended" : "native stereo ended (boarded or seated, or the game asked for its own mode)";
    explorerCamNativeBody(keyOn, nativeOnFootActive(), bodyWhy);
    gateLine(now);
    avatarLine(now);
    avatarCensusLine(now);
    eyeProbeTick(now);
    neckLine(now);
}

void nativeOnFootShutdown() {
    gateByteTick(false, "the game is shutting down");
}

}  // namespace edvr
