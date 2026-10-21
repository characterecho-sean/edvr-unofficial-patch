// The LoD view probe's glue (lod_view_hook.h says what and why; lod_view_core.h holds the logic and the evidence).
#include "lod_view_hook.h"

#include "flat_runtime.h"   // flatRuntimeSceneSizes: the flat runtime's own render and output sizes, any thread
#include "lod_view_core.h"

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"  // runtimeFlatProfile: the per-call gate

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

namespace edvr {
namespace {

namespace lv = lodview;

constexpr const char* kKey = "advanced.detail_at_output_size";
constexpr uint64_t kSilenceMs = 60000;

using Fn8 = uint64_t (*)(void*, void*, void*, void*, void*, void*, void*, void*);
Fn8 g_orig = nullptr;
CodeHook g_hook;
uintptr_t g_base = 0;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_keyOn{false};
std::atomic<uint64_t> g_calls{0};

// The view table: the hook observes into it on the game's thread, the poll takes the due lines out on the config tick.
SRWLOCK g_tableLock = SRWLOCK_INIT;
lv::ViewTable g_table;

// The poll's own state (the config tick's thread only).
bool g_keyKnown = false;
bool g_keyLast = false;
uint64_t g_firstPollMs = 0;
bool g_silenceSaid = false;
bool g_overflowSaid = false;

// One call's reads: the count the builder left, the descriptor's size and primary byte, and the written slot's A and B. Guarded: a fault in the
// game's memory must not become the game's crash. No objects inside the guards.
struct CallRead {
    uint32_t count = 0, width = 0, height = 0, slot = 0;
    uint8_t primary = 0;
    float a = 0.0f, b = 0.0f;
};
bool readCall(uintptr_t views, uintptr_t desc, CallRead* r) noexcept {
    __try {
        r->count = *reinterpret_cast<const volatile uint32_t*>(views + lv::kCountOffset);
        r->slot = lv::writtenSlot(r->count);
        if (r->slot == lv::kSlotCount) return false;
        r->width = *reinterpret_cast<const volatile uint32_t*>(desc + lv::kDescWidth);
        r->height = *reinterpret_cast<const volatile uint32_t*>(desc + lv::kDescHeight);
        r->primary = *reinterpret_cast<const volatile uint8_t*>(desc + lv::kDescPrimary);
        const uintptr_t at = views + uintptr_t(r->slot) * lv::kSlotStride;
        r->a = *reinterpret_cast<const volatile float*>(at + lv::kSlotA);
        r->b = *reinterpret_cast<const volatile float*>(at + lv::kSlotB);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool writePixelSize(uintptr_t views, uint32_t slot, float a, float b) noexcept {
    __try {
        const uintptr_t at = views + uintptr_t(slot) * lv::kSlotStride;
        *reinterpret_cast<volatile float*>(at + lv::kSlotA) = a;
        *reinterpret_cast<volatile float*>(at + lv::kSlotB) = b;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool readGuardedBytes(const uint8_t* at, uint8_t* out, size_t n) noexcept {
    __try {
        for (size_t i = 0; i < n; ++i) out[i] = at[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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

// The builder's replacement: the original builds the slot; then, in the flat profile, the slot's size and pixel size are read, the key's factor
// applied when it holds, and the call recorded for the poll's lines. The caller is the return address the game's call pushed (the relay and
// CodeHook's jump leave the stack as the call made it).
uint64_t builderReplacement(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uint64_t r = g_orig(a, b, c, d, e, f, g, h);
    if (!runtimeFlatProfile()) return r;
    g_calls.fetch_add(1, std::memory_order_relaxed);
    CallRead rd;
    const uintptr_t views = reinterpret_cast<uintptr_t>(a);
    if (!readCall(views, reinterpret_cast<uintptr_t>(b), &rd)) return r;
    lv::Observation o;
    o.callerRva = ret >= g_base && ret - g_base < lv::kExpectedImageSize ? static_cast<uint32_t>(ret - g_base) : 0;
    o.width = rd.width;
    o.height = rd.height;
    o.slot = rd.slot;
    o.a = rd.a;
    o.b = rd.b;
    o.primary = rd.primary;
    if (g_keyOn.load(std::memory_order_relaxed)) {
        uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
        if (flatRuntimeSceneSizes(&rw, &rh, &ow, &oh)) {
            const float k = lv::outputFactor(rd.width, rd.height, rw, rh, ow, oh);
            if (k != 1.0f && writePixelSize(views, rd.slot, rd.a * k, rd.b * k)) o.factor = k;
        }
    }
    AcquireSRWLockExclusive(&g_tableLock);
    g_table.observe(o);
    ReleaseSRWLockExclusive(&g_tableLock);
    return r;
}

// A relay within +/-2 GB of the target: the five-byte jump CodeHook writes can reach it, and the relay jumps to the replacement, which may be
// anywhere (vr_window_trampolines.cpp's allocateRelayNear, copied rather than shared, as that file does with kinematic_eval_hook.cpp's).
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

// CodeHook's Prepare: the original-function forward is published before the entry patch becomes visible. The replacement calls it
// unconditionally, so it must be set before any call can arrive.
bool prepareForward(void* trampoline, void* context) noexcept {
    if (!trampoline || !context) return false;
    *static_cast<void**>(context) = trampoline;
    return true;
}

}  // namespace

void lodViewInstall() {
    static bool done = false;
    if (done) return;
    done = true;
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_base) {
        Log::get().note("lod view: refused: the game module could not be found; nothing patched.");
        return;
    }
    uint32_t stamp = 0, size = 0;
    const bool identityRead = readIdentity(g_base, &stamp, &size);
    uint8_t got[lv::kPrologueBytes] = {};
    const bool readOk = readGuardedBytes(reinterpret_cast<const uint8_t*>(g_base + lv::kBuilderRva), got, sizeof(got));
    const lv::Install verdict = lv::installDecision(identityRead, stamp, size, readOk ? got : nullptr, sizeof(got));
    if (verdict == lv::Install::kWrongIdentity) {
        Log::get().note("lod view: refused: not build 334473 (PE timestamp %u, image size %u); nothing patched.", static_cast<unsigned>(stamp),
                        static_cast<unsigned>(size));
        return;
    }
    if (verdict == lv::Install::kUnreadable) {
        Log::get().note("lod view: refused: the PE header or the scene-view builder's prologue is unreadable; nothing patched.");
        return;
    }
    if (verdict == lv::Install::kWrongPrologue) {
        Log::get().note("lod view: refused: the scene-view builder's sixteen prologue bytes (0x2811E40) are not build 334473's; nothing patched.");
        return;
    }
    const uintptr_t target = g_base + lv::kBuilderRva;
    uint8_t* relay = buildRelayNear(target, reinterpret_cast<void*>(&builderReplacement));
    if (!relay) {
        Log::get().note("lod view: refused: no page within +/-2 GB of the scene-view builder for its relay; nothing patched.");
        return;
    }
    if (!g_hook.install(reinterpret_cast<void*>(target), relay, reinterpret_cast<void**>(&g_orig), "lod-view", &prepareForward,
                        reinterpret_cast<void**>(&g_orig))) {
        g_orig = nullptr;  // CodeHook's rule: a prepared forward is valid only if the install succeeds
        Log::get().note("lod view: refused by CodeHook (the prologue cannot be relocated; the reason is in the line above); nothing patched.");
        return;
    }
    g_installed.store(true, std::memory_order_release);
    Log::get().note("lod view: the scene-view builder (0x2811E40) is hooked, flat profile only (TEMPORARY instrument, "
                    "docs\\elite-ss-hangar-door-2026-10-21.md). A 'lod view:' line per (caller, size) when first seen and every 5 s while "
                    "called gives the game's own pixel size A and B; %s scales A and B by render height / output height for views drawn at "
                    "the render size. No 'lod view:' view line within 60 s of the first config tick means no scene view was built.",
                    kKey);
}

void lodViewPoll(uint64_t nowMs) {
    if (!g_installed.load(std::memory_order_acquire)) return;
    if (!g_firstPollMs) g_firstPollMs = nowMs ? nowMs : 1;

    const bool on = Config::get().getBool("advanced.detail_at_output_size", false);
    if (!g_keyKnown || on != g_keyLast) {
        uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
        const bool sizes = flatRuntimeSceneSizes(&rw, &rh, &ow, &oh);
        Log::get().note("lod view: %s %s%s (scene %ux%u output %ux%u, factor now %.4f; flat profile only, views drawn at the render size, "
                        "render below output on both axes)",
                        kKey, on ? "on" : "off", g_keyKnown ? "" : " at start", sizes ? rw : 0, sizes ? rh : 0, sizes ? ow : 0,
                        sizes ? oh : 0, static_cast<double>(sizes ? lv::outputFactor(rw, rh, rw, rh, ow, oh) : 1.0f));
        g_keyKnown = true;
        g_keyLast = on;
        g_keyOn.store(on, std::memory_order_relaxed);
    }

    if (!g_silenceSaid && g_calls.load(std::memory_order_relaxed) == 0 && nowMs - g_firstPollMs >= kSilenceMs) {
        g_silenceSaid = true;
        Log::get().note("lod view: no scene view built in the flat profile in the 60 s since the first config tick; the hook is installed but "
                        "the builder has not run through it (or the profile is not flat).");
    }

    lv::Due due[lv::kEntries];
    size_t n = 0;
    uint64_t overflow = 0;
    AcquireSRWLockExclusive(&g_tableLock);
    n = g_table.takeDue(nowMs, due, lv::kEntries);
    overflow = g_table.overflow();
    ReleaseSRWLockExclusive(&g_tableLock);
    if (!n && !(overflow && !g_overflowSaid)) return;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    if (!flatRuntimeSceneSizes(&rw, &rh, &ow, &oh)) rw = rh = ow = oh = 0;
    const bool keyOn = g_keyOn.load(std::memory_order_relaxed);
    char text[400];
    for (size_t i = 0; i < n; ++i) {
        lv::formatView(text, sizeof(text), due[i], rw, rh, ow, oh, keyOn);
        Log::get().note("%s", text);
    }
    if (overflow && !g_overflowSaid) {
        g_overflowSaid = true;
        Log::get().note("lod view: more than %u distinct (caller, size) views; calls to further ones are counted, not reported (%llu so far).",
                        static_cast<unsigned>(lv::kEntries), static_cast<unsigned long long>(overflow));
    }
}

}  // namespace edvr
