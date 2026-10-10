#include "../../d3d11/transition_flash_eye_base.h"
#include "../../d3d11/transition_flash_eye_base_core.h"
#include "../../d3d11/glitch_frame.h"
#include "../../common/code_hook.h"
#include "../../common/config.h"
#include "../../common/log.h"

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

// The transition-flash ENGINE FIX (docs/design-transition-flash-engine-fix-
// 2026-09-23.md). Armed by fix.transition_flash; the old detector
// (glitch_frame.cpp) goes dormant while it is and is the fallback when
// identity or the hook fails. The mechanism, the evidence and everything that
// was tried and ruled out are in that document; the logic that needs no game
// is in transition_flash_eye_base_core.h.

namespace edvr {
namespace {

// --- Identity: build 332841, the same pair pose_reader_watch.cpp keys on (and
// the transition_flash_prevent.cpp removed 2026-09-29 did). Kept as this
// file's own copy rather than a shared constant -- the copy-culture rule -- so
// neither is touched by a change in the other.
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

// --- SEH-guarded reads/writes. Each is its own small function, mixing
// __try with this file's other locals (mutexes, std::string) being what
// CodeHook's own doc calls out as refused by the compiler --
// pose_reader_watch.cpp's sehReadBlock16 is the precedent, kept to one access
// each.
__declspec(noinline) bool sehReadU64(uintptr_t address, uint64_t& out) noexcept {
    __try { out = *reinterpret_cast<const uint64_t*>(address); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
__declspec(noinline) bool sehReadBlock64(uintptr_t address, float out[16]) noexcept {
    __try { std::memcpy(out, reinterpret_cast<const void*>(address), 16 * sizeof(float)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
__declspec(noinline) bool sehCheckBytes(uintptr_t address, const uint8_t* expected, size_t n) noexcept {
    __try { return std::memcmp(reinterpret_cast<const void*>(address), expected, n) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// PE TimeDateStamp + SizeOfImage, read the same way pose_reader_watch.cpp's
// checkIdentity does (base+0x3C -> e_lfanew, +8 TimeDateStamp, +0x50
// SizeOfImage).
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

// --- Relay machinery, mirrored from pose_reader_watch.cpp (itself mirrored
// from object_record_writer_hook.cpp / kinematic_eval_hook.cpp, by way of the
// transition_flash_prevent.cpp removed 2026-09-29, git 68bddaaa^). Kept as a
// copy rather than a shared unit so none of the flight-proven files are
// touched; if one changes, look at the others (grep kRelayBytes).
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
// atomic load), the same g_relayGate discipline pose_reader_watch.cpp uses --
// there is exactly one consumer of this one hook.
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
__declspec(noinline) void __fastcall consumerObserved(uintptr_t cameraObj, int32_t gameMode, uintptr_t ptr3) noexcept;

HookEntry g_consumerEntry{"transition-flash-eye-base-consumer", kConsumerRva,
                          reinterpret_cast<void*>(&consumerObserved)};

// --- Module state ------------------------------------------------------
std::atomic<bool> g_engineOn{false};    // the consumer hook is installed and the fix is armed
std::atomic<bool> g_armed{false};       // install has been attempted (once)
std::atomic<bool> g_offLogged{false};
std::atomic<uint32_t> g_frame{0};
std::atomic<uint32_t> g_ebConsecutiveUnrefilled{0};

// The armed event. One slot: a new ENTRY edge replaces an unfinished event.
// pending/pendingSkip mirror `active` for the render tap's lock-free
// early-out (the camera CB is mapped dozens of times a frame and an event is
// a few frames a minute). gapLastConsume: the last gap (un-refilled mode-2)
// consume of the event's run, so the tap knows whether ITS consume was still
// a gap. All under g_eventMutex except the atomics.
struct Event {
    bool active = false;
    uint32_t skipFrame = 0;
    bool windowOpen = false;   // glitchFrameEngineWindow(true) is standing
};
std::mutex g_eventMutex;
Event g_event;
std::atomic<bool> g_eventPending{false};
std::atomic<uint32_t> g_eventPendingSkip{0};
std::atomic<uint32_t> g_gapLastConsume{0};
std::atomic<bool> g_recordsOutstanding{false};
// A frame was held and the temporal pass's verdict is owed once its eyes have
// been handed to the compositor (the next frame boundary).
std::atomic<bool> g_verdictDue{false};
// The tap frame already held: later fills of it return before taking the lock.
std::atomic<uint32_t> g_lastHeldTap{0xFFFFFFFFu};

// One held frame (two slots per event, indexed by tapFrame - skipFrame - 1),
// written by the render tap under g_eventMutex and logged a frame later.
struct HeldRecord {
    bool used = false;
    bool logged = false;
    bool consumerPresent = true;
    uint32_t skipFrame = 0, tapFrame = 0;
};
HeldRecord g_records[tfeb::kEngineMaxFrames];

std::atomic<uint64_t> g_events{0}, g_framesHeld{0}, g_eventsNoHeadOnly{0}, g_notHonoured{0};
constexpr uint32_t kMaxHeldLines = 60;
constexpr uint32_t kMaxNoHeadOnlyLines = 8;
std::atomic<uint32_t> g_heldLines{0}, g_noHeadOnlyLines{0};

void logHeld(const HeldRecord& r) noexcept;

void armEvent(uint32_t frame) noexcept {
    HeldRecord orphaned[tfeb::kEngineMaxFrames];   // unlogged records of the event this replaces
    uint32_t orphanedCount = 0;
    {
        std::lock_guard<std::mutex> lock(g_eventMutex);
        for (const HeldRecord& r : g_records) {
            if (r.used && !r.logged) orphaned[orphanedCount++] = r;
        }
        g_event.active = true;
        g_event.skipFrame = frame;
        g_event.windowOpen = true;
        for (HeldRecord& r : g_records) r = HeldRecord{};
        g_gapLastConsume.store(frame, std::memory_order_relaxed);
        g_lastHeldTap.store(0xFFFFFFFFu, std::memory_order_relaxed);
        g_eventPendingSkip.store(frame, std::memory_order_release);
        g_eventPending.store(true, std::memory_order_release);
        g_recordsOutstanding.store(true, std::memory_order_release);
    }
    for (uint32_t i = 0; i < orphanedCount; ++i) logHeld(orphaned[i]);
    glitchFrameEngineWindow(true);
    g_events.fetch_add(1, std::memory_order_relaxed);
}

// =====================================================================
// The consumer hook: classifies each consume and arms an event at the ENTRY
// of a run of reset mailboxes. Nothing else -- no mailbox write, no base
// capture, no game call.
// =====================================================================

// The classifier proper, split from the hook so the glue rig can drive it
// with a synthetic mailbox (transitionFlashEyeBaseConsumeForTest).
void noteConsume(int32_t gameMode, const float m[16]) noexcept {
    const bool unrefilled = (gameMode != 1) && tfeb::isResetMailbox(m);
    // A CAS loop, not load-then-store: FUN_142868c30 and FUN_14287b030 can both
    // consume on one frame, so this is not single-writer. `cur` is the streak
    // as it stood BEFORE this call folded in.
    uint32_t cur = g_ebConsecutiveUnrefilled.load(std::memory_order_relaxed);
    tfeb::ModeSwitchEdge edge;
    uint32_t next;
    do {
        edge = tfeb::classifyModeSwitchEdge(gameMode, unrefilled, cur);
        next = tfeb::updateConsecutiveUnrefilled(cur, gameMode, unrefilled);
    } while (!g_ebConsecutiveUnrefilled.compare_exchange_weak(cur, next, std::memory_order_relaxed));
    if (unrefilled && gameMode == 2) {
        const uint32_t frame = g_frame.load(std::memory_order_relaxed);
        if (edge == tfeb::ModeSwitchEdge::Entry) {
            armEvent(frame);
        } else if (g_eventPending.load(std::memory_order_acquire)) {
            // A further gap consume of the armed event's run -- the render of
            // the NEXT frame was still drawn from a reset mailbox
            // (tfeb::engineActFrame reads this).
            g_gapLastConsume.store(frame, std::memory_order_relaxed);
        }
    }
}

void __fastcall consumerObserved(uintptr_t cameraObj, int32_t gameMode, uintptr_t ptr3) noexcept {
    const auto forward = reinterpret_cast<ConsumerFn>(g_consumerEntry.forward.load(std::memory_order_acquire));
    if (!forward) return;  // stood down at install; the relay is unreachable then
    if (g_engineOn.load(std::memory_order_relaxed)) {
        uint64_t ship = 0;
        float m[16];
        if (sehReadU64(cameraObj + 0x50, ship) && sehReadBlock64(ship + 0x3330, m)) noteConsume(gameMode, m);
    }
    forward(cameraObj, gameMode, ptr3);
}

// =====================================================================
// The render tap: glitchFrameObserve hands every camera-CB fill here while an
// event window is open. On a bad render (tfeb::engineActFrame: skip+1, and
// skip+2 while the gap lasted), at the frame's FIRST fill whose row 275 is
// head-only, the frame is withheld, once. Nothing is written to the buffer
// (flight 091951: correcting it in place still flashed -- the bad frame
// carries at least five other wrong structures). No head-only fill in the
// window: nothing happens, and serviceEvent says so.
// =====================================================================

void logHeld(const HeldRecord& r) noexcept {
    const uint32_t already = g_heldLines.fetch_add(1, std::memory_order_relaxed);
    if (already >= kMaxHeldLines) return;
    Log::get().note(
        "transition flash: frame %u held (event skip %u)%s%s",
        r.tapFrame, r.skipFrame,
        r.consumerPresent ? "" : " -- NOT held: no compositor was in a position to hold it",
        already + 1 == kMaxHeldLines ? " (line cap reached; counts continue)" : "");
}

void summaryLine(const char* prefix) noexcept {
    Log::get().note(
        "%s engine fix %s: events=%llu frames held=%llu events with no head-only fill=%llu "
        "hold not honoured=%llu.",
        prefix, g_engineOn.load(std::memory_order_relaxed) ? "ARMED" : "NOT armed (detector in charge)",
        (unsigned long long)g_events.load(std::memory_order_relaxed),
        (unsigned long long)g_framesHeld.load(std::memory_order_relaxed),
        (unsigned long long)g_eventsNoHeadOnly.load(std::memory_order_relaxed),
        (unsigned long long)g_notHonoured.load(std::memory_order_relaxed));
}

// Called once a frame from transitionFlashEyeBaseFrameBoundary: logs the
// finished records (their fills closed a frame ago), closes the camera-CB tap
// window once the second bad render is done, and retires the event once its
// window has passed -- noting one that held no head-only fill. Takes the mutex
// only while an event is outstanding.
void serviceEvent(uint32_t nowFrame) noexcept {
    if (!g_recordsOutstanding.load(std::memory_order_acquire)) return;
    HeldRecord done[tfeb::kEngineMaxFrames];
    uint32_t doneCount = 0;
    bool noFill = false;
    bool closeWindow = false;
    uint32_t noFillSkip = 0;
    {
        std::lock_guard<std::mutex> lock(g_eventMutex);
        for (HeldRecord& r : g_records) {
            if (r.used && !r.logged && nowFrame >= r.tapFrame + 2u) {
                r.logged = true;
                done[doneCount++] = r;
            }
        }
        // The tap window closes once the second bad render's fills are done
        // (tap frame skip+2 closes at boundary skip+3; one spare).
        if (g_event.windowOpen && nowFrame >= g_event.skipFrame + tfeb::kEngineMaxFrames + 2u) {
            g_event.windowOpen = false;
            closeWindow = true;
        }
        // The event is over once its window has passed; one that held no
        // frame never saw a head-only fill.
        if (g_event.active && nowFrame > g_event.skipFrame + tfeb::kEventWindowFrames + 2u) {
            bool any = false;
            for (const HeldRecord& r : g_records) any = any || r.used;
            if (!any) { noFill = true; noFillSkip = g_event.skipFrame; }
            g_event.active = false;
            g_eventPending.store(false, std::memory_order_release);
        }
        bool pendingRecord = false;
        for (const HeldRecord& r : g_records) pendingRecord = pendingRecord || (r.used && !r.logged);
        if (!g_event.active && !pendingRecord) g_recordsOutstanding.store(false, std::memory_order_release);
    }
    if (closeWindow) glitchFrameEngineWindow(false);
    for (uint32_t i = 0; i < doneCount; ++i) logHeld(done[i]);
    if (noFill) {
        g_eventsNoHeadOnly.fetch_add(1, std::memory_order_relaxed);
        if (g_noHeadOnlyLines.fetch_add(1, std::memory_order_relaxed) < kMaxNoHeadOnlyLines) {
            Log::get().note(
                "transition flash: event at frame %u saw no head-only fill in its window (frames %u..%u) -- "
                "nothing held.",
                noFillSkip, noFillSkip + 1, noFillSkip + tfeb::kEngineMaxFrames);
        }
    }
}

// --- Install ------------------------------------------------------------

// The handshake's one report, from install: Armed when identity matched and
// the consumer hook installed; otherwise Lost, with the reason the line
// quotes, and the detector keeps running exactly as before.
// What protection runs when the engine fix does not: the old detector, if it
// observes anything, otherwise none.
const char* fallbackNote() noexcept {
    return glitchFrameDetectorRuns()
        ? "The fallback detector is in charge."
        : "NO transition protection is active: the fallback detector needs the same camera buffer.";
}

void doInstall() noexcept {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const char* why = nullptr;
    if (!base || !checkIdentity(base, &why)) {
        Log::get().note(
            "transition flash: engine fix NOT armed -- fallback: detector (%s). Nothing installed, "
            "nothing changed. %s", why ? why : "module base unavailable", fallbackNote());
        g_armed.store(true, std::memory_order_release);
        glitchFrameNoteEngineFix(false);
        return;
    }
    g_relayGate.store(1, std::memory_order_relaxed);
    const bool consumerOk = installOne(g_consumerEntry, base, kConsumerBytes);
    g_armed.store(true, std::memory_order_release);
    // Armed needs the whole chain: identity, the consumer hook AND a camera-
    // buffer tap that can deliver (glitchFrameObserve feeds it; a camera
    // buffer the detector does not observe starves it silently).
    char tapWhy[256] = "";
    const bool tapOk = glitchFrameEngineTapAvailable(tapWhy, sizeof(tapWhy));
    if (consumerOk && !tapOk) {
        glitchFrameNoteEngineFix(false);
        Log::get().note(
            "transition flash: engine fix NOT armed -- the camera-buffer tap is unavailable (%s). %s",
            tapWhy, fallbackNote());
        return;
    }
    if (consumerOk) {
        g_engineOn.store(true, std::memory_order_release);
        glitchFrameNoteEngineFix(true);
        Log::get().note(
            "transition flash: engine fix armed (fix.transition_flash on; the detector is dormant). "
            "identity: build match (timestamp %u, image %u bytes) -- OK. consumer 0x28431D0: installed.",
            kExpectedTimestamp, kExpectedImageSize);
    } else {
        glitchFrameNoteEngineFix(false);
        Log::get().note(
            "transition flash: engine fix NOT armed -- fallback: detector (the consumer hook did not "
            "install). consumer 0x28431D0: %s. %s", g_consumerEntry.failReason, fallbackNote());
    }
}

// --- Periodic (~20s) reporting, only when something moved -----------------
uint64_t g_lastReportTickMs = 0;
uint64_t g_lastReportBits[2] = {};

void maybeReportPeriodic() noexcept {
    const uint64_t now = GetTickCount64();
    if (now - g_lastReportTickMs < 20000) return;
    g_lastReportTickMs = now;
    const uint64_t bits[2] = {g_events.load(std::memory_order_relaxed), g_framesHeld.load(std::memory_order_relaxed)};
    bool moved = false;
    for (uint32_t i = 0; i < 2; ++i) {
        if (bits[i] != g_lastReportBits[i]) { moved = true; g_lastReportBits[i] = bits[i]; }
    }
    if (moved) summaryLine("transition flash:");
}

}  // namespace

// --- Public API ------------------------------------------------------------

// fix.transition_flash arms the engine fix. Read once, at the first call (the
// detector reads the same key once at install, and a reload cannot swap one
// fix for the other mid-session); later calls do nothing.
void transitionFlashEyeBaseConfigure(Config& cfg) {
    if (g_armed.load(std::memory_order_acquire)) return;
    if (!cfg.getBool("fix.transition_flash", true)) {
        if (!g_offLogged.exchange(true, std::memory_order_relaxed)) {
            Log::get().note("transition flash: engine fix off (fix.transition_flash = 0).");
        }
        return;
    }
    doInstall();
}

void transitionFlashEyeBaseFrameBoundary(uint32_t frameNo) {
    g_frame.store(frameNo, std::memory_order_relaxed);
    if (!g_engineOn.load(std::memory_order_relaxed)) return;
    serviceEvent(frameNo);
    // A frame was held at the mark; its eyes were handed over before this
    // boundary (Submit, Submit, Present, then here), so the temporal pass has
    // latched its verdict word by now and a word published NOW reads as a
    // change: "the camera stayed", history kept (glitch_frame.h).
    if (g_verdictDue.exchange(false, std::memory_order_acq_rel)) glitchFrameEngineFixVerdict();
    maybeReportPeriodic();
}

// The camera-CB fill tap (glitch_frame.cpp's glitchFrameObserve, pre-Unmap).
// Each fill carries the detector's frame attribution (s->frameNo at Unmap
// time -- the PRE-advance counter, so a fill of the frame the boundary records
// as F carries F-1; the window tests compensate).
void transitionFlashEyeBaseNoteSceneCB(uint32_t frame, const void* mapped, size_t sizeBytes) {
    if (!g_engineOn.load(std::memory_order_relaxed)) return;
    if (sizeBytes != tfeb::kSceneCBBytes) return;
    if (!g_eventPending.load(std::memory_order_acquire)) return;
    const uint32_t skip = g_eventPendingSkip.load(std::memory_order_acquire);
    if (!tfeb::eventWindowCovers(frame + 1, skip)) return;
    if (!tfeb::engineActFrame(frame, skip, g_gapLastConsume.load(std::memory_order_relaxed))) return;
    if (frame == g_lastHeldTap.load(std::memory_order_relaxed)) return;   // this frame is already held

    const float* row = static_cast<const float*>(mapped) + tfeb::kSceneCBOriginFloat;
    const float origin[3] = {row[0], row[1], row[2]};
    if (!tfeb::isHeadOnlyEye(origin)) return;     // not the bad eye's fill: leave it

    std::lock_guard<std::mutex> lock(g_eventMutex);
    if (!g_event.active || g_event.skipFrame != skip) return;
    HeldRecord& rec = g_records[frame - skip - 1];
    if (rec.used) return;                         // this frame is already held
    rec.used = true;
    rec.skipFrame = skip;
    rec.tapFrame = frame;
    g_lastHeldTap.store(frame, std::memory_order_relaxed);
    rec.consumerPresent = glitchFrameEngineFixEvent(true);
    g_framesHeld.fetch_add(1, std::memory_order_relaxed);
    if (!rec.consumerPresent) g_notHonoured.fetch_add(1, std::memory_order_relaxed);
    // The temporal pass's verdict is owed once this frame's eyes have been
    // handed to the compositor: the next frame boundary.
    g_verdictDue.store(true, std::memory_order_release);
}

// FOR TESTS (tools\native_temporal_test's glue cases): arm the engine fix
// without a game module, and feed the consume classifier a synthetic mailbox.
void transitionFlashEyeBaseArmForTest() {
    g_armed.store(true, std::memory_order_release);
    g_engineOn.store(true, std::memory_order_release);
    glitchFrameNoteEngineFix(true);
}
void transitionFlashEyeBaseDisarmForTest() {
    g_engineOn.store(false, std::memory_order_release);
    glitchFrameNoteEngineFix(false);
}
void transitionFlashEyeBaseCountersForTest(uint64_t* events, uint64_t* framesHeld, uint64_t* noHeadOnly,
                                           uint64_t* notHonoured) {
    if (events) *events = g_events.load(std::memory_order_relaxed);
    if (framesHeld) *framesHeld = g_framesHeld.load(std::memory_order_relaxed);
    if (noHeadOnly) *noHeadOnly = g_eventsNoHeadOnly.load(std::memory_order_relaxed);
    if (notHonoured) *notHonoured = g_notHonoured.load(std::memory_order_relaxed);
}
void transitionFlashEyeBaseConsumeForTest(int32_t gameMode, const float mailbox[16]) {
    if (g_engineOn.load(std::memory_order_relaxed)) noteConsume(gameMode, mailbox);
}

void transitionFlashEyeBaseShutdown() {
    if (!g_armed.load(std::memory_order_relaxed)) return;
    serviceEvent(0xFFFFFFFFu);   // flush any unlogged record
    summaryLine("--- transition flash, session total:");
}

}  // namespace edvr
