// The game-code trampolines (vr_window_trampolines.h says what and why): the mode request's window rewrite, the DisplayConfig writer's hold,
// and the settings copy's hold.
#include "vr_window_trampolines.h"

#include "device_hook.h"     // deviceHookPanelSettings: DisplaySettings.xml's window size
#include "vr_display_observer.h"  // vrDisplayReadGameWindow: the window the request produced
#include "vr_ssaa_gate.h"    // vrSsaaGateFrame: the frame number the log carries
#include "vr_ssaa_hold.h"    // vrSsaaHoldVrLive, vrSsaaHoldAfterCopy, vrSsaaHoldReadSettings
#include "vr_window_gate.h"  // vrWindowRewriteAllowed: the rewrite's profile gate (VR launch stays rewritten in Passthrough)

#include "../common/code_hook.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"  // runtimeScopeIncludesVr, runtimeProfileValue: the per-call rewrite gate
#include "cross_profile_cost.h"         // the mode request's and the settings copy's cost while flat runs (log only)

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace {

// The mode request (0x7E9E50). Its first eleven bytes, read from build 334925: mov rax,rsp; mov [rax+0x10],rbx; mov [rax+0x18],rsi.
// The request is rdx on entry. The function reads these request fields: [+0x08] (a table selector), [+0x10], [+0x18] (a table index),
// [+0x20] (the kind, a dword), and [+0x24], [+0x25] (flags). [+0x00] is a locked compare-exchange target and is never written here.
constexpr uint32_t kRequestRva = 0x7E9E50;
constexpr uint8_t kRequestBytes[11] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18};
constexpr uintptr_t kReqBlockOff = 0x08;   // the remembered block: request +0x08 .. +0x27 (four qwords)
constexpr uintptr_t kReqKindOff = 0x20;

// The settings copy W (0x281E650): the block copy into the render context at ctx+0x3428 (ctx = the first argument's +0x1180). Its first
// eleven bytes: mov r11,rsp; push rbx; sub rsp,0x4D0.
constexpr uint32_t kCopyRva = 0x281E650;
constexpr uint8_t kCopyBytes[11] = {0x4C, 0x8B, 0xDC, 0x53, 0x48, 0x81, 0xEC, 0xD0, 0x04, 0x00, 0x00};

constexpr uint32_t kRewriteCap = 64;

using Fn8 = uint64_t (*)(void*, void*, void*, void*, void*, void*, void*, void*);
Fn8 g_requestOrig = nullptr;
Fn8 g_copyOrig = nullptr;
CodeHook g_requestHook;
CodeHook g_copyHook;

// The last non-windowed request accepted while VR was inactive: the whole request block (+0x08 .. +0x27) and its kind.
std::atomic<bool> g_remembered{false};
std::atomic<uint64_t> g_block[4] = {};
std::atomic<uint32_t> g_rememberedKind{0};
std::atomic<uint32_t> g_rewrites{0};
std::atomic<bool> g_noneLogged{false};
// The mode request trace: every call is counted, the first kModeRequestTraceCap are logged, in every profile.
constexpr uint32_t kModeRequestTraceCap = 32;
std::atomic<uint32_t> g_modeRequests{0};
std::atomic<uint64_t> g_installTickMs{0};
std::atomic<uint8_t> g_lastLoggedAction{0xFF};
// The user's size: DisplaySettings.xml's ScreenWidth and ScreenHeight, kept when the file is not the 1280x768 windowed signature.
std::atomic<uint32_t> g_userW{0}, g_userH{0};

// Guarded reads and writes: a fault in the game's memory must not become the game's crash. No objects inside the guards.
bool readU32(uintptr_t at, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uint32_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool readU64(uintptr_t at, uint64_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uint64_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool writeU32(uintptr_t at, uint32_t v) {
    __try {
        *reinterpret_cast<volatile uint32_t*>(at) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool writeU64(uintptr_t at, uint64_t v) {
    __try {
        *reinterpret_cast<volatile uint64_t*>(at) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool readGuardedBytes(const uint8_t* at, uint8_t* out, size_t n) {
    __try {
        for (size_t i = 0; i < n; ++i) out[i] = at[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// DisplaySettings.xml's window size, kept as the user's unless it is the 1280x768 windowed signature (the value a windowed apply saved).
void refreshUserSize() {
    float hmd = 0.0f, ssaa = 0.0f;
    uint32_t w = 0, h = 0;
    if (!deviceHookPanelSettings(&hmd, &ssaa, &w, &h) || !w || !h) return;
    if (w == 1280 && h == 768) return;
    g_userW.store(w, std::memory_order_release);
    g_userH.store(h, std::memory_order_release);
}

// What requestReplacement did with a request, named exactly: the one log line says it after the decision, not before.
enum class ModeAction : uint8_t {
    PassNotAllowed,        // the profile gate (vrWindowRewriteAllowed): the request goes through untouched, the saved mode is not read
    RememberedGateClosed,  // a non-windowed request with VR not live: its block is kept, then the gate does not allow a rewrite and it passes unchanged
    Remembered,            // VR inactive, a non-windowed request: its block is kept, the request passes unchanged
    Rewritten,             // VR on, a windowed request: the remembered block was written into the request
    LeftVrInactive,        // VR inactive and the request is windowed: nothing to remember, nothing changed
    LeftNothingRemembered, // VR on, a windowed request, and no non-windowed request was remembered: left as the game made it
    LeftNonWindowed,       // VR on and the request is not windowed: left as the game made it
    LeftUnreadable         // the kind or block could not be read or written: left as the game made it
};
const char* modeActionName(ModeAction a) {
    switch (a) {
        case ModeAction::PassNotAllowed: return "passthrough_not_allowed";
        case ModeAction::RememberedGateClosed: return "remembered_gate_closed";
        case ModeAction::Remembered: return "remembered";
        case ModeAction::Rewritten: return "rewritten";
        case ModeAction::LeftVrInactive: return "left_as_is_vr_inactive";
        case ModeAction::LeftNothingRemembered: return "left_as_is_nothing_remembered";
        case ModeAction::LeftNonWindowed: return "left_as_is_vr_on_non_windowed";
        default: return "left_as_is_unreadable";
    }
}

// One mode request as the game made it (read at entry, before any rewrite) and what was done with it.
struct ModeRequestRecord {
    uint32_t n = 0, frame = 0, thread = 0;
    uint64_t sinceInstallMs = 0;
    bool haveKind = false, haveFlags = false, haveBlock = false, live = false, rememberedBefore = false;
    uint32_t kind = 0, flagsDword = 0;   // flagsDword is the dword at +0x24: byte 0 is +0x24, byte 1 is +0x25
    uint64_t q[4] = {};
    const char* profile = "";
    ModeAction action = ModeAction::PassNotAllowed;
};

// vr window: mode request #N: frame F, +T ms since hook install, thread TID, kind K, flags +0x24 XX +0x25 YY, block +0x08 A +0x10 B +0x18 C
//   +0x20 D, profile P, vr_live 0|1, remembered_before 0|1, action NAME
// A field that could not be read prints ?. Returns the length written, or 0 when the buffer is too small.
int formatModeRequestLine(const ModeRequestRecord& r, char* buf, size_t cap) {
    char kind[16], flags[24], block[96];
    if (r.haveKind) std::snprintf(kind, sizeof(kind), "%u", r.kind); else std::snprintf(kind, sizeof(kind), "?");
    if (r.haveFlags) std::snprintf(flags, sizeof(flags), "%02X +0x25 %02X", r.flagsDword & 0xFFu, (r.flagsDword >> 8) & 0xFFu);
    else std::snprintf(flags, sizeof(flags), "? +0x25 ?");
    if (r.haveBlock)
        std::snprintf(block, sizeof(block), "%llX +0x10 %llX +0x18 %llX +0x20 %llX", static_cast<unsigned long long>(r.q[0]),
                      static_cast<unsigned long long>(r.q[1]), static_cast<unsigned long long>(r.q[2]), static_cast<unsigned long long>(r.q[3]));
    else std::snprintf(block, sizeof(block), "? +0x10 ? +0x18 ? +0x20 ?");
    const int n = std::snprintf(buf, cap,
        "vr window: mode request #%u: frame %u, +%llu ms since hook install, thread %u, kind %s, flags +0x24 %s, block +0x08 %s, profile %s, "
        "vr_live %u, remembered_before %u, action %s",
        r.n, r.frame, static_cast<unsigned long long>(r.sinceInstallMs), r.thread, kind, flags, block, r.profile, unsigned(r.live),
        unsigned(r.rememberedBefore), modeActionName(r.action));
    return n > 0 && size_t(n) < cap ? n : 0;
}
// vr window: mode request #N returned: game window style 0xS client WxH, frame F, +T ms since hook install   (or: game window not hooked yet)
int formatModeReturnLine(const ModeRequestRecord& r, bool haveWindow, unsigned long long style, long cw, long ch, uint32_t frame,
                         uint64_t sinceInstallMs, char* buf, size_t cap) {
    int n;
    if (haveWindow)
        n = std::snprintf(buf, cap, "vr window: mode request #%u returned: game window style 0x%llX client %ldx%ld, frame %u, +%llu ms since hook install",
                          r.n, style, cw, ch, frame, static_cast<unsigned long long>(sinceInstallMs));
    else
        n = std::snprintf(buf, cap, "vr window: mode request #%u returned: game window not hooked yet, frame %u, +%llu ms since hook install",
                          r.n, frame, static_cast<unsigned long long>(sinceInstallMs));
    return n > 0 && size_t(n) < cap ? n : 0;
}

uint64_t msSinceInstall() {
    const uint64_t at = g_installTickMs.load(std::memory_order_acquire);
    const uint64_t now = GetTickCount64();
    return at && now >= at ? now - at : 0;
}

// The request at entry: what the game made, read before the decision and before any rewrite. Only the first kModeRequestTraceCap calls.
void fillModeRequestRecord(ModeRequestRecord* r, uint32_t n, uintptr_t req) {
    r->n = n;
    r->frame = vrSsaaGateFrame();
    r->sinceInstallMs = msSinceInstall();
    r->thread = GetCurrentThreadId();
    r->haveKind = readU32(req + kReqKindOff, &r->kind);
    r->haveFlags = readU32(req + kReqKindOff + 4u, &r->flagsDword);
    r->haveBlock = true;
    for (int i = 0; i < 4 && r->haveBlock; ++i) r->haveBlock = readU64(req + kReqBlockOff + 8u * i, &r->q[i]);
    r->profile = runtimeProfileName();
    r->live = vrSsaaHoldVrLive();
    r->rememberedBefore = g_remembered.load(std::memory_order_acquire);
}
void logModeRequest(ModeRequestRecord* r, ModeAction action) {
    r->action = action;
    g_lastLoggedAction.store(static_cast<uint8_t>(action), std::memory_order_relaxed);   // read by vr_ssaa_hold_test only
    char line[640];
    if (formatModeRequestLine(*r, line, sizeof(line))) Log::get().note("%s", line);
}
// After the original returned: the window's style and client size, as the Present window observer reads them.
void logModeRequestReturn(const ModeRequestRecord& r) {
    unsigned long long style = 0;
    long cw = 0, ch = 0;
    const bool have = vrDisplayReadGameWindow(&style, &cw, &ch);
    char line[320];
    if (formatModeReturnLine(r, have, style, cw, ch, vrSsaaGateFrame(), msSinceInstall(), line, sizeof(line))) Log::get().note("%s", line);
}

// The mode request's window rewrite (H6; the maintainer's decision: keep the user's display mode when 3D is on). A non-windowed request (kind
// != 0) made while VR is not live is remembered whole (its block) in every profile, before the gate: a flat launch's own mode request is the
// mode the first live switch must keep, and the gate decides the rewrite only. The rewrite runs in the VR profile, or flat and passthrough with
// VR in scope and live (vr_window_gate.h); otherwise the request passes byte for byte and the saved mode is not read. VR active (or the settings
// file's mode is on at a windowed request): a windowed request (kind 0) gets the remembered block restored before the original runs, so the
// apply builds the remembered mode. A request made while VR is live is never remembered. Nothing in a request is changed while VR is inactive.
uint64_t requestReplacement(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    // Every call is counted and the first kModeRequestTraceCap are logged in every profile (a flat launch included): the entry is read here,
    // the one line is written once the decision is made, and a second line after the original returns. Logging changes nothing below.
    const uint32_t callNumber = g_modeRequests.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool trace = callNumber <= kModeRequestTraceCap;
    ModeRequestRecord record;
    if (trace) fillModeRequestRecord(&record, callNumber, reinterpret_cast<uintptr_t>(b));
    // Timed while the flat profile is active (cross_profile_cost.h inactiveFor): from entry to the forward call, the rewrite included.
    cross_profile::Probe probe = cross_profile::enterSite(cross_profile::Site::VrModeRequest,
        cross_profile::inactiveFor(cross_profile::Owner::Vr, runtimeFlatProfile(), runtimeVrProfile()));
    const bool live = vrSsaaHoldVrLive();
    const uintptr_t req = reinterpret_cast<uintptr_t>(b);
    uint32_t kind = 0;
    const bool haveKind = readU32(req + kReqKindOff, &kind);
    // Remembered before the gate and before any read of the saved mode, in every profile: what the game asked for while VR was not live is the
    // mode a later live switch keeps. Only the guarded reads of the request and stores into the remembered block.
    bool remembered = false, unreadable = false;
    if (haveKind && kind != 0 && !live) {
        uint64_t q[4] = {};
        bool ok = true;
        for (int i = 0; i < 4 && ok; ++i) ok = readU64(req + kReqBlockOff + 8u * i, &q[i]);
        if (ok) {
            for (int i = 0; i < 4; ++i) g_block[i].store(q[i], std::memory_order_release);
            g_rememberedKind.store(kind, std::memory_order_release);
            g_remembered.store(true, std::memory_order_release);
            remembered = true;
        } else {
            unreadable = true;
        }
    }
    // Flat and Passthrough rewrite only with VR in scope and a live session (vr_window_gate.h).
    if (!vrWindowRewriteAllowed(runtimeProfileValue(), runtimeScopeIncludesVr(), live)) {
        probe.stop();
        if (trace) logModeRequest(&record, remembered ? ModeAction::RememberedGateClosed : ModeAction::PassNotAllowed);
        const uint64_t passed = g_requestOrig(a, b, c, d, e, f, g, h);
        if (trace) logModeRequestReturn(record);
        return passed;
    }
    ModeAction action = ModeAction::LeftVrInactive;
    bool vrOn = live;
    if (!vrOn && haveKind && kind == 0) {
        int mode = 0;
        char raw[24] = {};
        vrOn = vrSsaaHoldReadSettings(&mode, raw, sizeof(raw)) && mode != 0;  // the request at the switch, before the live session is up
    }
    if (!vrOn) {
        if (haveKind && kind != 0) {
            action = remembered ? ModeAction::Remembered : unreadable ? ModeAction::LeftUnreadable : ModeAction::LeftVrInactive;
        } else if (!haveKind) {
            action = ModeAction::LeftUnreadable;
        }
    } else if (haveKind && kind == 0) {
        refreshUserSize();
        const uint32_t dw = g_userW.load(std::memory_order_acquire), dh = g_userH.load(std::memory_order_acquire);
        if (g_remembered.load(std::memory_order_acquire)) {
            // Before: the request's block as the game made it; after: the remembered block, written whole (verified fields only).
            uint64_t before[4] = {}, after[4] = {};
            bool ok = true;
            for (int i = 0; i < 4 && ok; ++i) ok = readU64(req + kReqBlockOff + 8u * i, &before[i]);
            for (int i = 0; i < 4; ++i) after[i] = g_block[i].load(std::memory_order_acquire);
            for (int i = 0; ok && i < 4; ++i) ok = writeU64(req + kReqBlockOff + 8u * i, after[i]);
            action = ok ? ModeAction::Rewritten : ModeAction::LeftUnreadable;
            if (ok && g_rewrites.fetch_add(1, std::memory_order_relaxed) < kRewriteCap)
                Log::get().note("vr window: 3D on: the game asked for kind 0 (windowed); the request block was set to the one remembered "
                                "while VR was inactive: kind 0 -> %u; +0x08 %llX -> %llX; +0x10 %llX -> %llX; +0x18 %llX -> %llX; "
                                "+0x20 %llX -> %llX; DisplaySettings ScreenWidth %u ScreenHeight %u (the save is not held); frame %u",
                                g_rememberedKind.load(std::memory_order_acquire),
                                static_cast<unsigned long long>(before[0]), static_cast<unsigned long long>(after[0]),
                                static_cast<unsigned long long>(before[1]), static_cast<unsigned long long>(after[1]),
                                static_cast<unsigned long long>(before[2]), static_cast<unsigned long long>(after[2]),
                                static_cast<unsigned long long>(before[3]), static_cast<unsigned long long>(after[3]), dw, dh,
                                vrSsaaGateFrame());
        } else {
            action = ModeAction::LeftNothingRemembered;
            if (!g_noneLogged.exchange(true, std::memory_order_acq_rel))
                Log::get().note("vr window: 3D on asked for kind 0 and no non-windowed request was remembered since start; the request is "
                                "left as the game made it (DisplaySettings %ux%u; a size alone does not name a mode, so it is not used)",
                                dw, dh);
        }
    } else {
        action = haveKind ? ModeAction::LeftNonWindowed : ModeAction::LeftUnreadable;
    }
    probe.stop();
    if (trace) logModeRequest(&record, action);
    const uint64_t result = g_requestOrig(a, b, c, d, e, f, g, h);
    if (trace) logModeRequestReturn(record);
    return result;
}

// The settings copy (W): the original copies the settings block into the render context; then the hold (vrSsaaHoldAfterCopy) reads
// the copied value and, while VR is live, sets the field to 1.0. ctx is the first argument's +0x1180, read before the copy.
uint64_t copyReplacement(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    // Timed while the flat profile is active (cross_profile_cost.h inactiveFor): the read before the copy and the hold after it, not the copy.
    cross_profile::Probe probe = cross_profile::enterSite(cross_profile::Site::VrSettingsCopy,
        cross_profile::inactiveFor(cross_profile::Owner::Vr, runtimeFlatProfile(), runtimeVrProfile()));
    uint64_t ctx = 0;
    readU64(reinterpret_cast<uintptr_t>(a) + 0x1180, &ctx);
    probe.stop();
    const uint64_t r = g_copyOrig(a, b, c, d, e, f, g, h);
    probe.restart();
    if (ctx) vrSsaaHoldAfterCopy(static_cast<uintptr_t>(ctx));
    probe.stop();
    return r;
}

// A relay within +/-2 GB of the target: the five-byte jump CodeHook writes can reach it, and the relay jumps to the replacement, which
// may be anywhere (kinematic_eval_hook.cpp's allocateRelay, copied rather than shared, as that file does with its writer hook).
// Layout: jmp qword ptr [rip+0] (FF 25 00 00 00 00), then the replacement's address. Registers and the stack pass through unchanged.
constexpr size_t kRelayBytes = 14;
uint8_t* allocateRelayNear(uintptr_t target) {
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
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_RESERVE | MEM_COMMIT,
                                                             PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

uint8_t* buildRelayNear(uintptr_t target, void* replacement) {
    uint8_t* relay = allocateRelayNear(target);
    if (!relay) return nullptr;
    const uint8_t body[kRelayBytes] = {0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(relay, body, sizeof(body));
    const uintptr_t to = reinterpret_cast<uintptr_t>(replacement);
    std::memcpy(relay + 6, &to, 8);
    DWORD old = 0;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old) || !FlushInstructionCache(GetCurrentProcess(), relay, kRelayBytes)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return nullptr;
    }
    return relay;  // kept for the process lifetime: the installed patch jumps here
}

// CodeHook's Prepare: the replacement's original-function forward (the context) is published before the entry patch becomes visible.
// The replacements call the forward unconditionally, so it must be set before any call can arrive.
bool prepareForward(void* trampoline, void* context) noexcept {
    if (!trampoline || !context) return false;
    *static_cast<void**>(context) = trampoline;
    return true;
}

// Pins the function's first bytes, then installs the entry through a near relay. False (and no patch) when the bytes differ, no
// relay can be placed, or CodeHook refuses. The forward is published through Prepare, and cleared again when CodeHook refuses.
bool installOne(const uint8_t* base, uint32_t rva, const uint8_t* want, size_t len, void* replacement, void** origOut,
                CodeHook* hook, const char* name) {
    uint8_t got[11] = {};
    if (!readGuardedBytes(base + rva, got, len) || std::memcmp(got, want, len) != 0) {
        Log::get().note("vr window: trampoline %s refused: the bytes at 0x%X are not build 334925's; nothing patched", name, rva);
        return false;
    }
    const uintptr_t target = reinterpret_cast<uintptr_t>(base) + rva;
    uint8_t* relay = buildRelayNear(target, replacement);
    if (!relay) {
        Log::get().note("vr window: trampoline %s refused: no page within +/-2 GB of the target for its relay; nothing patched", name);
        return false;
    }
    if (!hook->install(reinterpret_cast<void*>(target), relay, origOut, name, &prepareForward, origOut)) {
        *origOut = nullptr;  // CodeHook's rule: a prepared forward is valid only if the install succeeds
        Log::get().note("vr window: trampoline %s refused by CodeHook (the prologue cannot be relocated; the reason is in the line "
                        "above); nothing patched", name);
        return false;
    }
    return true;
}

}  // namespace

void vrWindowTrampolinesInstall() {
    static bool done = false;
    if (done) return;
    done = true;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) {
        Log::get().note("vr window: trampolines off: the game module could not be found");
        return;
    }
    g_installTickMs.store(GetTickCount64(), std::memory_order_release);
    refreshUserSize();
    const bool req = installOne(base, kRequestRva, kRequestBytes, sizeof(kRequestBytes), reinterpret_cast<void*>(&requestReplacement),
                                reinterpret_cast<void**>(&g_requestOrig), &g_requestHook, "0x7E9E50 (mode request)");
    const bool copy = installOne(base, kCopyRva, kCopyBytes, sizeof(kCopyBytes), reinterpret_cast<void*>(&copyReplacement),
                                 reinterpret_cast<void**>(&g_copyOrig), &g_copyHook, "0x281E650 (settings copy)");
    Log::get().note("vr window: trampolines: 0x7E9E50 (mode request, window rewrite) %s; 0x281E650 (settings copy, hold) %s; DisplaySettings %ux%u; the display config save is not held (its field layout is not proven)",
                    req ? "installed" : "not installed", copy ? "installed" : "not installed", g_userW.load(), g_userH.load());
}

}  // namespace edvr
