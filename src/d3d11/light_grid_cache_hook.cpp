// The light grid cache's glue (light_grid_cache_hook.h): finds the cluster builder, patches its entry through a relay (the same copy the other engine hooks carry), reads the
// call's inputs under SEH and hands them to light_grid_cache_core.h. Everything that decides is in the core, which the rig drives; this file only touches the game.
#include "light_grid_cache_hook.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "../common/code_hook.h"
#include "../common/log.h"
#include "light_grid_cache_core.h"

namespace edvr {
namespace {

namespace lg = lgc;

constexpr size_t kRelayBytes = 44, kOriginalLiteral = 36;

// rcx the cluster context, rdx/r8/r9 pointers, then the target's width and height (32-bit stack arguments; the callee reads 32 bits). The return value is not read by the caller.
using BuilderFn = uintptr_t(__fastcall*)(void* self, void* a2, void* a3, void* a4, uint32_t width, uint32_t height);
using RefreshFn = void(__fastcall*)(void* view);   // 0x4F2FF0: recompute the view block's four rows and clear its dirty bit

__declspec(noinline) uintptr_t __fastcall builderDetour(void* self, void* a2, void* a3, void* a4, uint32_t width, uint32_t height) noexcept;

alignas(8) std::atomic<uintptr_t> g_gate{0};
static_assert(decltype(g_gate)::is_always_lock_free, "The relay reads the aligned atomic cell directly.");

struct Detour {
    std::atomic<uintptr_t> forward{0};
    CodeHook hook;
    uint8_t* relay = nullptr;
    std::atomic<bool> ready{false};
    std::atomic<bool> attempted{false};
    std::atomic<const char*> status{"not installed yet"};
};
Detour g_detour;

uintptr_t g_base = 0;
std::atomic<uintptr_t> g_refresh{0};      // 0x4F2FF0 when its prologue is the pinned one; 0: a dirty view block is forwarded whole
std::atomic<uintptr_t> g_copy{0};         // 0x57A460 when its prologue is the pinned one; 0: the override path's rows cannot be derived (the call goes to the game whole)
lg::Cache g_cache;                        // the render thread's alone
std::atomic<uint32_t> g_ownerThread{0};   // the first thread to call
std::atomic<uint64_t> g_otherThreadCalls{0};
std::atomic<uint64_t> g_frame{0};
std::atomic<uint64_t> g_callsEver{0};
std::atomic<uint64_t> g_installedMs{0};
std::atomic<bool> g_silenceSaid{false};
uint64_t g_windowFrame0 = 0;              // the render thread's
bool g_windowOpen = false;
double g_windowStartMs = 0.0;
double g_lastMissMs = 0.285;              // the measured miss cost kept across windows (seeded from the probe flight)

// EDVR's jitter, published by the temporal pass (a seqlock; the probe's lines are the only reader).
std::atomic<uint32_t> g_jitSeq{0};
std::atomic<uint64_t> g_jitFrame{0};
std::atomic<uint32_t> g_jitBits[6]{};

int64_t ticks() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double nowMs() noexcept {
    static const double perMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    return static_cast<double>(ticks()) / perMs;
}

// ---- SEH-guarded memory for the core ---------------------------------------------------------------------------------------------------------------------------------
struct GuardedMem {
    static bool read(void* dst, const void* src, size_t n) noexcept {
        __try {
            std::memcpy(dst, src, n);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    static bool write(void* dst, const void* src, size_t n) noexcept {
        __try {
            std::memcpy(dst, src, n);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
};

// ---- the relay: mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback]; original: jmp [trampoline] --------------------------------------------------------------
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

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    // The builder's first instruction is mov rax,rsp, which overwrites RAX, and it takes nothing in RAX or the flags; no stack adjustment, no nonvolatile register touched, so
    // the detour sees the arguments exactly as the caller left them.
    const uint8_t body[kRelayBytes] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x83, 0x38, 0, 0x74, 0x0E, 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                       0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &gateAddress, 8);
    std::memcpy(code + 22, &callbackAddress, 8);
}

bool prepareRelay(void* trampoline, void* context) noexcept {
    auto& d = *static_cast<Detour*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(d.relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(d.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) || !FlushInstructionCache(GetCurrentProcess(), d.relay, kRelayBytes)) return false;
    d.forward.store(address, std::memory_order_release);
    return true;
}

bool readIdentity(uintptr_t base, uint32_t* stamp, uint32_t* size) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) return false;
        std::memcpy(stamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(size, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The call's inputs, every one the build reads that is not in the context. False when any read faulted.
bool readInputs(const void* a2, const void* a4, uint32_t width, uint32_t height, lg::CallInputs* in) noexcept {
    __try {
        const auto* view = static_cast<const uint8_t*>(a4);
        if (view == nullptr) return false;
        std::memcpy(in->rows, view + lg::kViewRows, lg::kRowsBytes);
        std::memcpy(&in->nearBits, view + lg::kViewNear, 4);
        std::memcpy(&in->farBits, view + lg::kViewFar, 4);
        in->extraNearBits = in->nearBits;
        in->viewFlags = view[lg::kViewFlags];
        in->overridePath = *reinterpret_cast<const volatile uint8_t*>(g_base + lg::kOverridePathRva);
        in->forceRebuild = *reinterpret_cast<const volatile uint8_t*>(g_base + lg::kForceRebuildRva);
        in->tile = *reinterpret_cast<const volatile uint32_t*>(g_base + lg::kTileRva);
        in->slices = *reinterpret_cast<const volatile uint32_t*>(g_base + lg::kSlicesRva);
        in->width = width;
        in->height = height;
        in->renderer = a2 ? *static_cast<const volatile uint64_t*>(a2) : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The override path (kOverrideNearRva's comment in the core): the rows, near and far the build keys on, got the way the build gets them, on a local copy. False when a pinned function is
// missing or anything faulted; the call then goes to the game whole.
bool deriveOverride(const void* view, lg::CallInputs* in) noexcept {
    using CopyFn = void*(__fastcall*)(void* dst, const void* src);
    const auto copy = reinterpret_cast<CopyFn>(g_copy.load(std::memory_order_acquire));
    const auto refresh = reinterpret_cast<RefreshFn>(g_refresh.load(std::memory_order_acquire));
    if (!copy || !refresh || view == nullptr) return false;
    alignas(16) uint8_t local[lg::kViewObjectBytes + 16];
    __try {
        copy(local, view);
        uint32_t flags = 0;
        std::memcpy(&flags, local + lg::kViewFlags, 4);
        flags |= lg::kOverrideFlagBits;
        std::memcpy(local + lg::kViewFlags, &flags, 4);
        std::memcpy(local + lg::kViewNear, reinterpret_cast<const void*>(g_base + lg::kOverrideNearRva), 4);
        std::memcpy(local + lg::kViewFar, reinterpret_cast<const void*>(g_base + lg::kOverrideFarRva), 4);
        refresh(local);
        std::memcpy(in->rows, local + lg::kViewRowsOut, lg::kRowsBytes);
        std::memcpy(&in->nearBits, local + lg::kViewNear, 4);
        std::memcpy(&in->farBits, local + lg::kViewFar, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The game's own refresh of a dirty view block, run ahead of reading the rows (the builder would run it first thing; see kRefreshRva). False when it faulted.
bool refreshView(void* view) noexcept {
    const auto fn = reinterpret_cast<RefreshFn>(g_refresh.load(std::memory_order_acquire));
    if (!fn) return false;
    __try {
        fn(view);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The self-check's second derivation (CallInputs::rederive): the inputs of this call, read again as the view block is now.
struct Redo {
    const void* a2;
    const void* a4;
    uint32_t width, height;
};
bool redoInputs(const void* user, lg::CallInputs* out) noexcept {
    const Redo& r = *static_cast<const Redo*>(user);
    if (!readInputs(r.a2, r.a4, r.width, r.height, out)) return false;
    return out->overridePath == 0 || deriveOverride(r.a4, out);
}

lg::JitterSnap readJitter() noexcept {
    lg::JitterSnap j;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t s0 = g_jitSeq.load(std::memory_order_acquire);
        if (s0 == 0 || (s0 & 1u)) continue;
        uint32_t bits[6];
        for (int i = 0; i < 6; ++i) bits[i] = g_jitBits[i].load(std::memory_order_relaxed);
        const uint64_t frame = g_jitFrame.load(std::memory_order_relaxed);
        if (g_jitSeq.load(std::memory_order_acquire) != s0) continue;
        float f[6];
        std::memcpy(f, bits, sizeof(f));
        j.frame = frame;
        j.jx = f[0];
        j.jy = f[1];
        j.shift[0][0] = f[2];
        j.shift[0][1] = f[3];
        j.shift[1][0] = f[4];
        j.shift[1][1] = f[5];
        j.known = true;
        break;
    }
    return j;
}

void say(const char* line) noexcept { Log::get().note("%s", line); }

// The window's lines (the render thread's), then a fresh window.
void reportWindow(double now) noexcept {
    lg::Stats& s = g_cache.stats;
    const uint64_t seconds = static_cast<uint64_t>((now - g_windowStartMs) / 1000.0 + 0.5);
    const uint64_t frames = g_frame.load(std::memory_order_relaxed) - g_windowFrame0;
    if (s.misses > 0) g_lastMissMs = s.missMs / static_cast<double>(s.misses);
    const uint64_t other = g_otherThreadCalls.exchange(0, std::memory_order_relaxed);
    s.calls += other;
    s.forwarded += other;
    s.byReason[lg::kOtherThread] += other;
    char line[2400];
    lg::formatMainLine(line, sizeof(line), s, seconds, frames, g_lastMissMs, static_cast<unsigned>(g_cache.slotsInUse()), static_cast<unsigned>(g_cache.contexts()));
    say(line);
    if (s.jNeighbour > 0) {
        lg::formatJitterLine(line, sizeof(line), s, seconds);
        say(line);
        for (int i = 0; i < s.nSamples; ++i) {
            lg::formatSampleLine(line, sizeof(line), s.samples[i]);
            say(line);
        }
    }
    s = lg::Stats{};
    g_windowStartMs = now;
    g_windowFrame0 = g_frame.load(std::memory_order_relaxed);
}

// The first call of each context (at most four): where it is and whether the cache could take its template.
void sayFirstSight(void* self, void* view, uint32_t width, uint32_t height, const lg::CallInputs& in) noexcept {
    static uintptr_t seen[lg::kMaxContexts] = {};
    for (uintptr_t s : seen) {
        if (s == reinterpret_cast<uintptr_t>(self)) return;
    }
    for (uintptr_t& s : seen) {
        if (s != 0) continue;
        s = reinterpret_cast<uintptr_t>(self);
        const int gi = g_cache.groupFor(reinterpret_cast<uintptr_t>(self));
        const bool templ = gi >= 0 && g_cache.group(gi).haveTemplate;
        char line[480];
        std::snprintf(line, sizeof(line), "light grid cache: new context %p (view block %p, %ux%u target, tile %u, %u slices); first seen with %s; %s.", self, view, width, height,
                      in.tile, in.slices, gi >= 0 ? g_cache.group(gi).firstSeen : "no group", templ ? "empty template made" : "no template: the cache stands down for it");
        say(line);
        return;
    }
}

void turnOff(const char* why) noexcept {
    g_gate.store(0, std::memory_order_release);
    g_detour.status.store("off for this session (the log says why)", std::memory_order_release);
    char line[1300];
    std::snprintf(line, sizeof(line), "light grid cache: OFF for this session: %s. Every call now goes to the game.", why);
    say(line);
}

__declspec(noinline) uintptr_t __fastcall builderDetour(void* self, void* a2, void* a3, void* a4, uint32_t width, uint32_t height) noexcept {
    const auto forward = reinterpret_cast<BuilderFn>(g_detour.forward.load(std::memory_order_acquire));
    if (!forward) return 0;
    uintptr_t result = 0;
    auto fwd = [&]() noexcept { result = forward(self, a2, a3, a4, width, height); };
    g_callsEver.fetch_add(1, std::memory_order_relaxed);
    // The builder's caller is the render thread; any other thread's call goes to the game whole (counted apart: the cache's own numbers belong to the render thread).
    if (self == nullptr || !lg::threadMayServe(g_ownerThread, GetCurrentThreadId())) {
        g_otherThreadCalls.fetch_add(1, std::memory_order_relaxed);
        fwd();
        return result;
    }
    if (g_cache.disabled) {
        fwd();
        return result;
    }
    lg::CallInputs in;
    if (!readInputs(a2, a4, width, height, &in)) {
        ++g_cache.stats.calls;
        ++g_cache.stats.forwarded;
        ++g_cache.stats.byReason[lg::kUnreadable];
        fwd();
        return result;
    }
    if (in.overridePath != 0) {
        // The build's usual path: its rows come from a copy of the block with the override near and far. Do the same, on a copy of ours.
        const double r0 = nowMs();
        in.rowsOk = deriveOverride(a4, &in);
        if (in.rowsOk) {
            ++g_cache.stats.derived;
            g_cache.stats.deriveMs += nowMs() - r0;
        }
    } else if ((in.viewFlags & lg::kViewDirtyBit) != 0 && g_refresh.load(std::memory_order_acquire) != 0) {
        // The plain path: a dirty block has its rows made by the game's refresh first thing; make them now (the original then finds the bit clear and skips it).
        const double r0 = nowMs();
        if (refreshView(a4) && readInputs(a2, a4, width, height, &in)) {
            ++g_cache.stats.refreshes;
            g_cache.stats.refreshMs += nowMs() - r0;
        }
    }
    in.jitter = readJitter();
    const Redo redo{a2, a4, width, height};
    in.rederive = &redoInputs;
    in.rederiveUser = &redo;
    const double now = nowMs();
    if (!g_windowOpen) {
        g_windowOpen = true;
        g_windowStartMs = now;
        g_windowFrame0 = g_frame.load(std::memory_order_relaxed);
    }
    const lg::Result r = lg::serve<GuardedMem>(g_cache, static_cast<uint8_t*>(self), in, fwd, nowMs);
    sayFirstSight(self, a4, width, height, in);
    if (r.disableNow || g_cache.disabled) turnOff(g_cache.disabledWhy);
    if (now - g_windowStartMs >= lg::kWindowMs) reportWindow(now);
    return result;
}

void installOnce() noexcept {
    if (g_detour.attempted.exchange(true, std::memory_order_acq_rel)) return;
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_base) {
        g_detour.status.store("the module base is unknown", std::memory_order_release);
        return;
    }
    uint32_t stamp = 0, size = 0;
    const bool identityRead = readIdentity(g_base, &stamp, &size);
    uint8_t got[lg::kPrologueBytes] = {};
    bool readOk = false;
    __try {
        std::memcpy(got, reinterpret_cast<const void*>(g_base + lg::kBuilderRva), sizeof(got));
        readOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        readOk = false;
    }
    const lg::Install verdict = lg::installDecision(identityRead, stamp, size, readOk ? got : nullptr, sizeof(got));
    if (verdict != lg::Install::kInstall) {
        if (verdict == lg::Install::kWrongIdentity) {
            g_detour.status.store("not build 332841 (PE timestamp or image size)", std::memory_order_release);
            Log::get().note("light grid cache: refused: not build 332841 (PE timestamp %u, image size %u); nothing patched.", static_cast<unsigned>(stamp),
                            static_cast<unsigned>(size));
        } else if (verdict == lg::Install::kUnreadable) {
            g_detour.status.store("the executable's PE header or the builder's prologue is unreadable", std::memory_order_release);
            Log::get().note("light grid cache: refused: the PE header or the cluster builder's prologue is unreadable; nothing patched.");
        } else {
            g_detour.status.store("the builder's prologue does not match build 332841", std::memory_order_release);
            Log::get().note("light grid cache: refused: the cluster builder's sixteen prologue bytes are not build 332841's; nothing patched.");
        }
        return;
    }
    uint8_t refreshGot[lg::kPrologueBytes] = {};
    bool refreshRead = false;
    __try {
        std::memcpy(refreshGot, reinterpret_cast<const void*>(g_base + lg::kRefreshRva), sizeof(refreshGot));
        refreshRead = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        refreshRead = false;
    }
    if (refreshRead && lg::refreshPinMatches(refreshGot, sizeof(refreshGot))) {
        g_refresh.store(g_base + lg::kRefreshRva, std::memory_order_release);
    } else {
        Log::get().note("light grid cache: the view-block refresh (0x4F2FF0) is not the pinned function; a dirty view block and the override path go to the game whole.");
    }
    uint8_t copyGot[lg::kPrologueBytes] = {};
    bool copyRead = false;
    __try {
        std::memcpy(copyGot, reinterpret_cast<const void*>(g_base + lg::kCopyRva), sizeof(copyGot));
        copyRead = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        copyRead = false;
    }
    if (copyRead && lg::copyPinMatches(copyGot, sizeof(copyGot))) {
        g_copy.store(g_base + lg::kCopyRva, std::memory_order_release);
    } else {
        Log::get().note("light grid cache: the view-block copy constructor (0x57A460) is not the pinned function; the override path goes to the game whole.");
    }
    g_detour.relay = allocateRelay(g_base + lg::kBuilderRva);
    if (!g_detour.relay) {
        g_detour.status.store("no relay within range", std::memory_order_release);
        Log::get().note("light grid cache: refused: no memory for the relay within range of the game; nothing patched.");
        return;
    }
    buildRelay(g_detour.relay, &g_gate, reinterpret_cast<void*>(&builderDetour));
    if (!g_detour.hook.install(reinterpret_cast<void*>(g_base + lg::kBuilderRva), g_detour.relay, nullptr, "light-grid-cache", &prepareRelay, &g_detour)) {
        VirtualFree(g_detour.relay, 0, MEM_RELEASE);
        g_detour.relay = nullptr;
        g_detour.status.store("CodeHook refused the builder (its own line says why)", std::memory_order_release);
        return;
    }
    g_detour.ready.store(true, std::memory_order_release);
    g_detour.status.store("installed", std::memory_order_release);
    g_installedMs.store(static_cast<uint64_t>(nowMs()), std::memory_order_release);
    g_gate.store(1, std::memory_order_release);
    Log::get().note("light grid cache: the fragment-culling cluster builder (0x58B150) is detoured. Exact: a call whose inputs match a kept state bit for bit gets that state "
                    "swapped in; one hit in %llu is rebuilt by the game and compared byte for byte, and any difference turns the cache off. Up to %d states per context.",
                    static_cast<unsigned long long>(lg::kVerifyEvery), lg::kSlotsPerContext);
}

}  // namespace

void lightGridCacheInstall() { installOnce(); }

void lightGridCachePoll(uint64_t nowMsStamp) {
    (void)nowMsStamp;
    if (!g_detour.ready.load(std::memory_order_acquire) || g_silenceSaid.load(std::memory_order_relaxed)) return;
    if (g_callsEver.load(std::memory_order_relaxed) != 0) {
        g_silenceSaid.store(true, std::memory_order_relaxed);
        return;
    }
    const uint64_t since = static_cast<uint64_t>(nowMs()) - g_installedMs.load(std::memory_order_acquire);
    if (since >= 180000 && !g_silenceSaid.exchange(true, std::memory_order_relaxed)) {
        Log::get().note("light grid cache: no call of the cluster builder in the first %llu s after the install (the menus do not run it).", static_cast<unsigned long long>(since / 1000));
    }
}

void lightGridCacheNoteFrame(uint64_t frame) { g_frame.store(frame, std::memory_order_relaxed); }

void lightGridCacheNoteJitter(uint64_t frame, float jx, float jy, const float shift[2][2]) {
    const float f[6] = {jx, jy, shift[0][0], shift[0][1], shift[1][0], shift[1][1]};
    uint32_t bits[6];
    std::memcpy(bits, f, sizeof(bits));
    const uint32_t s = g_jitSeq.load(std::memory_order_relaxed);
    g_jitSeq.store((s | 1u) + 0u, std::memory_order_release);   // odd: being written
    for (int i = 0; i < 6; ++i) g_jitBits[i].store(bits[i], std::memory_order_relaxed);
    g_jitFrame.store(frame, std::memory_order_relaxed);
    g_jitSeq.store(((s | 1u) + 1u), std::memory_order_release);  // even again, and never zero
}

const char* lightGridCacheStatus() { return g_detour.status.load(std::memory_order_acquire); }

}  // namespace edvr
