// The F2 instruments: the glue (explorer_cam_f2.h says what they are; explorer_cam_f2_core.h holds the decisions, the commander-frame
// math and the text of every line, driven by tools\explorer_cam_probe_test).
//
// THREADS. The free-camera and controller observers run on whichever thread the game's job system calls those updates from, after
// the original has returned and every press is restored: each takes a try-flag (a second concurrent caller skips), allocates nothing,
// writes no log line and calls nothing of the game's, and publishes through a small ring or a seqlock. The neck's replacement getter
// runs on ANY thread that reads the eye matrix, for every humanoid: it takes nothing, allocates nothing, logs nothing, and returns
// exactly what the original returns. The tick (explorerCamF2Tick) is the probe's frame boundary and is the only code that logs.
#include "explorer_cam_f2.h"
#include "explorer_cam_f2_core.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "explorer_cam.h"

namespace edvr {
namespace {

// ---- shared state ----------------------------------------------------------------------------------------------------------
std::atomic<bool> g_armed{false};

// I3 pressed (free-camera observer)
std::atomic<bool> g_busy3{false};
f2::Pressed3 g_last3;
bool g_have3 = false;
std::atomic<uint64_t> g_freeCalls{0};
ecm::Ring<f2::F2Event, 64> g_ringFree;

// I4 (controller observer)
std::atomic<bool> g_busyCtl{false};
f2::ControllerSnap g_lastCtl;
bool g_haveCtl = false;
std::atomic<uint64_t> g_ctlCalls{0};
std::atomic<uint32_t> g_ctlMode{0};
std::atomic<uint64_t> g_ctlLast{0};
ecm::Ring<f2::F2Event, 64> g_ringCtl;

std::atomic<uint64_t> g_f2Seq{0};

// The neck.
f2::NeckTargets g_neck;                          // written before the swap, read-only afterwards (the replacement reads it)
std::atomic<uint64_t> g_neckCalls{0}, g_neckLocalCalls{0};
std::atomic<uintptr_t> g_localEye{0};            // the local commander's eye interface, or 0
ecp::KeyTable<32> g_neckSeen;                    // the distinct interface pointers (capped)
std::atomic<uint32_t> g_neckSeenOverflow{0};
std::atomic<uint32_t> g_staleCount{0};
std::atomic<uintptr_t> g_staleVptr{0};
ecp::SeqSlot<f2::NeckSample> g_sample;
bool g_neckTried = false;
bool g_neckInstalled = false;
uintptr_t g_neckOriginal = 0;                    // what the slot held

// The arm lines say what happened once per session.
const char* g_i4Status = "not tried";
const char* g_neckStatus = "not tried";
bool g_announced = false;

#ifdef EDVR_EXPLORER_CAM_TEST
explorercamf2test::NeckSeam g_testNeck;
#endif

// ---- guarded access --------------------------------------------------------------------------------------------------------
// The pressed int of the action object at object+handleOffset, or kUnreadable (NULL, not an object pointer, or a fault).
__declspec(noinline) int32_t sehPressedAt(const uint8_t* object, uint32_t handleOffset) noexcept {
    __try {
        uint64_t p = 0;
        std::memcpy(&p, object + handleOffset, 8);
        if (p < 0x10000u || p >= 0x00007FFF00000000ull || (p & 7u) != 0) return f2::kUnreadable;
        int32_t v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(p + ecm::kOffActionPressed), 4);
        return v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return f2::kUnreadable;
    }
}
__declspec(noinline) bool sehReadController(const uint8_t* c, f2::ControllerSnap* s) noexcept {
    __try {
        s->mode = c[ecm::kOffCtlMode];
        std::memcpy(&s->presetKind, c + ecm::kOffCtlPresetKind, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The paired sample: the eye interface's matrix and the activity's two poses, in one go. `vptrBad` says the interface is stale.
__declspec(noinline) bool sehReadNeckSample(const uint8_t* activity, uintptr_t iface, const f2::NeckTargets& t, f2::NeckSample* s, bool* vptrBad,
                                            uint64_t* badVptr) noexcept {
    __try {
        uint64_t vptr = 0;
        std::memcpy(&vptr, reinterpret_cast<const void*>(iface), 8);
        if (!f2::neckInterfaceOk(vptr, t)) {
            *vptrBad = true;
            *badVptr = vptr;
            return false;
        }
        std::memcpy(s->eye, reinterpret_cast<const void*>(iface + f2::kOffEyeMatrix), 64);
        std::memcpy(s->world, activity + 0x70, 64);
        std::memcpy(s->local, activity + ecm::kOffLocalPose, 64);
        s->state = activity[ecm::kOffState];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct EyeRead {
    float a[16], b[16], c[16], d[16], v[4];
};
__declspec(noinline) bool sehReadEyeInterface(uintptr_t iface, const f2::NeckTargets& t, EyeRead* r, bool* vptrBad, uint64_t* badVptr) noexcept {
    __try {
        uint64_t vptr = 0;
        std::memcpy(&vptr, reinterpret_cast<const void*>(iface), 8);
        if (!f2::neckInterfaceOk(vptr, t)) {
            *vptrBad = true;
            *badVptr = vptr;
            return false;
        }
        std::memcpy(r->a, reinterpret_cast<const void*>(iface + f2::kOffEyeMatrix), 64);
        std::memcpy(r->b, reinterpret_cast<const void*>(iface + f2::kOffEyeB), 64);
        std::memcpy(r->c, reinterpret_cast<const void*>(iface + f2::kOffEyeC), 64);
        std::memcpy(r->d, reinterpret_cast<const void*>(iface + f2::kOffEyeD), 64);
        std::memcpy(r->v, reinterpret_cast<const void*>(iface + f2::kOffEyeVec), 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehReadQword(uintptr_t address, uint64_t* out) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehBytesEqual(uintptr_t address, const uint8_t* expected, size_t n) noexcept {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(address), expected, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
__declspec(noinline) bool sehStoreQword(uintptr_t address, uintptr_t value) noexcept {
    __try {
        *reinterpret_cast<volatile uintptr_t*>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- the neck's replacement getter ---------------------------------------------------------------------------------------------
// Slot +0x20 of the eye interface's vtable: `lea rax,[rcx+268h]; ret`. This does what it does and a little more: it counts, notes the
// interface pointer, and keeps the pointer when the caller is the first-person camera activity's own read, which only ever reads
// the LOCAL commander's eye. No lock, no log, no allocation; it can be called from several threads and for every humanoid.
__declspec(noinline) void* __fastcall eyeGetterObserved(void* self) noexcept {
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    g_neckCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_neckSeen.findOrInsert(reinterpret_cast<uintptr_t>(self)) < 0) g_neckSeenOverflow.fetch_add(1, std::memory_order_relaxed);
    if (f2::isLocalEyeSite(ret, g_neck)) {
        g_neckLocalCalls.fetch_add(1, std::memory_order_relaxed);
        g_localEye.store(reinterpret_cast<uintptr_t>(self), std::memory_order_release);
    }
    return static_cast<uint8_t*>(self) + f2::kOffEyeMatrix;
}

// Replace one 8-byte slot: only those 8 bytes change protection, one aligned store, the protection back.
bool storeSlot(uintptr_t slot, uintptr_t value) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot), 8, PAGE_READWRITE, &old)) return false;
    const bool ok = sehStoreQword(slot, value);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), 8, old, &ignored);
    return ok;
}

bool installNeck(const f2::NeckTargets& t, char* why, size_t cap) {
    uint64_t current = 0;
    if (!sehReadQword(t.slot, &current)) {
        std::snprintf(why, cap, "the read of the eye vtable's slot at 0x%llX faulted; nothing was patched", static_cast<unsigned long long>(t.slot));
        return false;
    }
    if (!f2::neckSlotOk(current, t)) {
        std::snprintf(why, cap,
                      "the game build differs (slot +0x%X of the eye interface's vtable at 0x%llX holds 0x%llX, not build 332841's accessor at 0x%llX); "
                      "nothing was patched",
                      8u * f2::kEyeGetterSlot, static_cast<unsigned long long>(t.vtable), static_cast<unsigned long long>(current),
                      static_cast<unsigned long long>(t.getter));
        return false;
    }
    if (!sehBytesEqual(t.getter, f2::kEyeGetterCode, f2::kEyeGetterBytes)) {
        std::snprintf(why, cap, "the game build differs (the 8 bytes at 0x%llX are not `lea rax,[rcx+268h]; ret`); nothing was patched",
                      static_cast<unsigned long long>(t.getter));
        return false;
    }
    g_neck = t;
    if (!storeSlot(t.slot, reinterpret_cast<uintptr_t>(&eyeGetterObserved))) {
        g_neck = f2::NeckTargets();
        std::snprintf(why, cap, "the slot could not be written; nothing was patched");
        return false;
    }
    g_neckOriginal = static_cast<uintptr_t>(current);
    g_neckInstalled = true;
    return true;
}

// Put the original back, but only if the slot is still ours: another tool that replaced it after us owns it now.
void uninstallNeck() {
    if (!g_neckInstalled) return;
    uint64_t now = 0;
    if (sehReadQword(g_neck.slot, &now) && now == reinterpret_cast<uintptr_t>(&eyeGetterObserved)) storeSlot(g_neck.slot, g_neckOriginal);
    g_neckInstalled = false;
}
struct NeckGuard {
    ~NeckGuard() { uninstallNeck(); }
} g_neckGuard;

// ---- text and ticks --------------------------------------------------------------------------------------------------------------
void say(const ecm::Sink& sink, const char* fmt, ...) {
    char line[ecm::kLineBytes];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    sink(line);
}

struct F2Frame {
    bool started = false;
    ecp::Cadence sec, beat;
    uint64_t lastBeatMs = 0;
    uint64_t beatCtlCalls = 0, beatNeckCalls = 0, beatLocalCalls = 0;
    uint64_t seenLocalCalls = 0;
    int64_t lastLocalMs = -1;
    uint32_t seenPublishes = 0;
    int64_t lastSampleMs = -1;
    uint32_t staleSaid = 0;
    bool noPairNoted = false;
};
F2Frame g_f2;

}  // namespace

void explorerCamF2Arm(const ecm::Sink& sink) {
    if (g_armed.load(std::memory_order_acquire)) return;
    g_armed.store(true, std::memory_order_release);
    const bool first = !g_announced;
    g_announced = true;

    // I4: the controller observer rides explorer_cam.cpp's hook.
    const ExplorerCamHookStatus ctl = explorerCamObserve(ExplorerCamHook::Controller, &explorerCamF2Controller, true);
    if (ctl.state == ExplorerCamHookStatus::Armed) {
        g_i4Status = "armed";
        if (first)
            say(sink, "%s EliteDangerous64.exe+0x%llX (VesselCameraMountControl update, build 332841) hooked at 0x%llX, stolen=%zu bytes, relay at 0x%llX; "
                      "observed after the original and after every press is restored: mode +0x3E0 on every transition, pressed ints +0x310 +0x328 "
                      "+0x340 and preset kind +0x2E8 on change, and a 5 s heartbeat of the call count; read only",
                f2::prefixI4Armed(), static_cast<unsigned long long>(ecm::kControllerRva), static_cast<unsigned long long>(ctl.target), ctl.stolen,
                static_cast<unsigned long long>(ctl.relay));
    } else {
        g_i4Status = "stood down";
        if (first) say(sink, "%s %s. I4 will not run.", f2::prefixI4Down(), ctl.why);
    }

    // N: the neck's slot swap, once per session.
    if (!g_neckTried) {
        g_neckTried = true;
        char why[400] = {};
        f2::NeckTargets t;
        bool known = false;
#ifdef EDVR_EXPLORER_CAM_TEST
        if (g_testNeck.vtable) {
            t.vtable = g_testNeck.vtable;
            t.slot = g_testNeck.slot;
            t.getter = g_testNeck.getter;
            t.localSite = g_testNeck.localSite;
            known = true;
        }
#endif
        if (!known) {
            uintptr_t base = 0;
            if (explorerCamBuildKnown(&base, why, sizeof(why))) {
                t = f2::neckTargetsFromBase(base);
                known = true;
            }
        }
        if (known && installNeck(t, why, sizeof(why))) {
            g_neckStatus = "armed";
            say(sink, "%s the eye interface's vtable at EliteDangerous64.exe+0x%llX (build 332841): slot +0x%X (0x%llX, `lea rax,[rcx+268h]; ret`, shared by six "
                      "vtables, so the SLOT is replaced and the code is not hooked) now holds a getter that counts, notes the interface pointer and returns "
                      "exactly what the original returns. A call returning to 0x%llX is the first-person camera activity's own read, which only ever reads "
                      "the LOCAL commander's eye. Read from the free-camera hook's post-call and at 1 Hz; nothing is written to the game but that one slot",
                f2::prefixNArmed(), static_cast<unsigned long long>(f2::kEyeVtableRva), 8u * f2::kEyeGetterSlot, static_cast<unsigned long long>(t.getter),
                static_cast<unsigned long long>(t.localSite));
        } else {
            g_neckStatus = "stood down";
            say(sink, "%s %s. N will not run.", f2::prefixNDown(), why[0] ? why : "the build is unknown");
        }
    }
    g_f2.started = false;
}

void explorerCamF2Disarm() {
    if (!g_armed.exchange(false, std::memory_order_acq_rel)) return;
    explorerCamObserve(ExplorerCamHook::Controller, &explorerCamF2Controller, false);
    g_f2.started = false;
}

void explorerCamF2FreeCamera(void* activity) noexcept {
    if (!activity || !g_armed.load(std::memory_order_acquire)) return;
    if (g_busy3.exchange(true, std::memory_order_acquire)) return;
    const uint8_t* const a = static_cast<const uint8_t*>(activity);
    const uint64_t call = g_freeCalls.fetch_add(1, std::memory_order_relaxed) + 1;

    f2::Pressed3 p;
    p.a = sehPressedAt(a, ecm::kOffToggleRotationAction);
    p.b = sehPressedAt(a, ecm::kOffWorldFixAction);
    p.c = sehPressedAt(a, ecm::kOffLockAction);
    if (!g_have3 || !(p == g_last3)) {
        f2::F2Event e;
        e.seq = g_f2Seq.fetch_add(1, std::memory_order_relaxed);
        e.kind = static_cast<uint32_t>(f2::F2Kind::Pressed3);
        e.object = reinterpret_cast<uint64_t>(activity);
        e.call = call;
        e.threadId = GetCurrentThreadId();
        e.a = p.a; e.b = p.b; e.c = p.c;
        e.pa = g_last3.a; e.pb = g_last3.b; e.pc = g_last3.c;
        e.first = g_have3 ? 0u : 1u;
        g_ringFree.push(e);
        g_last3 = p;
        g_have3 = true;
    }

    // The neck, paired with this update's poses: only once the local eye interface is known.
    const uintptr_t iface = g_localEye.load(std::memory_order_acquire);
    if (iface) {
        f2::NeckSample s;
        s.activity = reinterpret_cast<uint64_t>(activity);
        s.iface = iface;
        s.localSiteCalls = g_neckLocalCalls.load(std::memory_order_relaxed);
        bool bad = false;
        uint64_t badVptr = 0;
        if (sehReadNeckSample(a, iface, g_neck, &s, &bad, &badVptr)) {
            g_sample.tryPublish(s);
        } else if (bad) {
            uintptr_t expected = iface;
            if (g_localEye.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
                g_staleVptr.store(badVptr, std::memory_order_relaxed);
                g_staleCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    g_busy3.store(false, std::memory_order_release);
}

void explorerCamF2Controller(void* controller) noexcept {
    if (!controller || !g_armed.load(std::memory_order_acquire)) return;
    const uint64_t call = g_ctlCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    g_ctlLast.store(reinterpret_cast<uint64_t>(controller), std::memory_order_relaxed);
    if (g_busyCtl.exchange(true, std::memory_order_acquire)) return;
    const uint8_t* const c = static_cast<const uint8_t*>(controller);
    f2::ControllerSnap s;
    s.controller = reinterpret_cast<uint64_t>(controller);
    s.call = call;
    if (sehReadController(c, &s)) {
        s.photo = sehPressedAt(c, ecm::kOffCtlPhotoAction);
        s.free = sehPressedAt(c, ecm::kOffCtlFreeAction);
        s.quit = sehPressedAt(c, ecm::kOffCtlQuitAction);
        g_ctlMode.store(s.mode, std::memory_order_relaxed);
        if (!g_haveCtl || !s.sameAs(g_lastCtl)) {
            f2::F2Event e;
            e.seq = g_f2Seq.fetch_add(1, std::memory_order_relaxed);
            e.kind = static_cast<uint32_t>(f2::F2Kind::Controller);
            e.object = s.controller;
            e.call = call;
            e.threadId = GetCurrentThreadId();
            e.a = s.photo; e.b = s.free; e.c = s.quit;
            e.mode = s.mode;
            e.prevMode = g_haveCtl ? g_lastCtl.mode : s.mode;
            e.presetKind = s.presetKind;
            e.first = g_haveCtl ? 0u : 1u;
            g_ringCtl.push(e);
            g_lastCtl = s;
            g_haveCtl = true;
        }
    }
    g_busyCtl.store(false, std::memory_order_release);
}

void explorerCamF2Tick(uint32_t frame, uint64_t nowMs, const ecm::Sink& sink) {
    if (!g_armed.load(std::memory_order_acquire)) return;
    F2Frame& fs = g_f2;
    char line[ecm::kLineBytes];
    if (!fs.started) {
        fs.started = true;
        fs.sec.periodMs = 1000;
        fs.sec.reset(nowMs);
        fs.beat.periodMs = 5000;
        fs.beat.reset(nowMs);
        fs.lastBeatMs = nowMs;
        fs.beatCtlCalls = g_ctlCalls.load(std::memory_order_relaxed);
        fs.beatNeckCalls = g_neckCalls.load(std::memory_order_relaxed);
        fs.beatLocalCalls = g_neckLocalCalls.load(std::memory_order_relaxed);
        fs.seenLocalCalls = fs.beatLocalCalls;
        fs.seenPublishes = g_sample.publishes();
    }

    // 1. What the observers saw change, in order.
    {
        f2::F2Event events[2 * 64];
        size_t count = 0;
        for (auto* ring : {&g_ringFree, &g_ringCtl})
            while (count < sizeof(events) / sizeof(events[0]) && ring->take(&events[count])) ++count;
        std::sort(events, events + count, [](const f2::F2Event& x, const f2::F2Event& y) { return x.seq < y.seq; });
        for (size_t i = 0; i < count; ++i) {
            f2::formatF2Event(line, sizeof(line), events[i], frame);
            sink(line);
        }
    }

    // 2. A stale interface pointer, said once each time it happens.
    const uint32_t stale = g_staleCount.load(std::memory_order_relaxed);
    if (stale != fs.staleSaid) {
        fs.staleSaid = stale;
        say(sink, "%s the local eye interface pointer's vtable is 0x%llX, not the eye vtable's 0x%llX: it was stale, so it is cleared and re-learned the "
                  "next time the first-person camera activity reads the eye (stale count %u)",
            f2::prefixNStale(), static_cast<unsigned long long>(g_staleVptr.load(std::memory_order_relaxed)), static_cast<unsigned long long>(g_neck.vtable),
            stale);
    }

    // 3. When the local site was last called, and when the paired sample last moved.
    const uint64_t localCalls = g_neckLocalCalls.load(std::memory_order_relaxed);
    if (localCalls != fs.seenLocalCalls) {
        fs.seenLocalCalls = localCalls;
        fs.lastLocalMs = static_cast<int64_t>(nowMs);
    }
    const uint32_t publishes = g_sample.publishes();
    if (publishes != fs.seenPublishes) {
        fs.seenPublishes = publishes;
        fs.lastSampleMs = static_cast<int64_t>(nowMs);
    }

    // 4. 1 Hz, while the free camera activity exists or the local site was called in the last second.
    if (fs.sec.due(nowMs) && std::strcmp(g_neckStatus, "armed") == 0) {
        const bool activity = fs.lastSampleMs >= 0 && static_cast<int64_t>(nowMs) - fs.lastSampleMs <= 1500;
        const bool local = fs.lastLocalMs >= 0 && static_cast<int64_t>(nowMs) - fs.lastLocalMs <= 1000;
        const uintptr_t iface = g_localEye.load(std::memory_order_acquire);
        if ((activity || local) && iface) {
            const char* phase = activity ? "free-camera" : "local-site";
            EyeRead r;
            bool bad = false;
            uint64_t badVptr = 0;
            if (sehReadEyeInterface(iface, g_neck, &r, &bad, &badVptr)) {
                f2::formatNeckMatrix(line, sizeof(line), "+0x268(eye)", iface, r.a, phase);
                sink(line);
                f2::formatNeckMatrix(line, sizeof(line), "+0x1A8", iface, r.b, phase);
                sink(line);
                f2::formatNeckMatrix(line, sizeof(line), "+0x1E8", iface, r.c, phase);
                sink(line);
                f2::formatNeckMatrix(line, sizeof(line), "+0x228", iface, r.d, phase);
                sink(line);
                f2::formatNeckVec(line, sizeof(line), iface, r.v, phase);
                sink(line);
            } else if (bad) {
                uintptr_t expected = iface;
                if (g_localEye.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
                    g_staleVptr.store(badVptr, std::memory_order_relaxed);
                    g_staleCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if (activity) {
            f2::NeckSample s;
            if (g_sample.read(s)) {
                const f2::CommanderFrame cf = f2::commanderFrame(s.local, s.world);
                float eyeLocal[3] = {0, 0, 0};
                if (cf.valid) f2::worldToCommanderLocal(cf, s.eye + 12, eyeLocal);
                f2::formatNeckLocal(line, sizeof(line), s, cf, eyeLocal, static_cast<int64_t>(nowMs) - fs.lastSampleMs);
                sink(line);
            }
        } else if (local && fs.lastSampleMs < 0 && !fs.noPairNoted) {
            fs.noPairNoted = true;
            say(sink, "%s the first-person camera activity reads the eye, but no free-camera update has paired a sample with it yet, so the "
                      "commander-local figure waits for the free camera (TAB, or F5)",
                f2::prefixNLocal());
        }
    }

    // 5. The 5 s heartbeats: one per instrument, always, so a zero is distinguishable from not running.
    if (fs.beat.due(nowMs)) {
        const double seconds = static_cast<double>(nowMs - fs.lastBeatMs) / 1000.0;
        f2::I4HeartbeatIn h4;
        h4.windowSeconds = seconds;
        h4.hook = g_i4Status;
        h4.callsTotal = g_ctlCalls.load(std::memory_order_relaxed);
        h4.callsWindow = h4.callsTotal - fs.beatCtlCalls;
        h4.mode = g_ctlMode.load(std::memory_order_relaxed);
        h4.lastController = g_ctlLast.load(std::memory_order_relaxed);
        h4.lost = g_ringCtl.lost();
        f2::formatI4Heartbeat(line, sizeof(line), h4);
        sink(line);
        fs.beatCtlCalls = h4.callsTotal;

        f2::NeckHeartbeatIn hn;
        hn.windowSeconds = seconds;
        hn.hook = g_neckStatus;
        hn.calls = g_neckCalls.load(std::memory_order_relaxed);
        hn.callsWindow = hn.calls - fs.beatNeckCalls;
        hn.localSiteCalls = g_neckLocalCalls.load(std::memory_order_relaxed);
        hn.localSiteWindow = hn.localSiteCalls - fs.beatLocalCalls;
        size_t distinct = 0;
        for (size_t i = 0; i < 32; ++i) distinct += g_neckSeen.key(i) != 0 ? 1 : 0;
        hn.distinctInterfaces = distinct;
        hn.distinctOverflow = g_neckSeenOverflow.load(std::memory_order_relaxed);
        hn.localEye = g_localEye.load(std::memory_order_relaxed);
        hn.localEyeSet = hn.localEye != 0;
        hn.localSiteAgeMs = fs.lastLocalMs < 0 ? -1 : static_cast<int64_t>(nowMs) - fs.lastLocalMs;
        f2::formatNeckHeartbeat(line, sizeof(line), hn);
        sink(line);
        fs.beatNeckCalls = hn.calls;
        fs.beatLocalCalls = hn.localSiteCalls;
        fs.lastBeatMs = nowMs;
    }
}

#ifdef EDVR_EXPLORER_CAM_TEST
namespace explorercamf2test {
void setNeckTargets(const NeckSeam& t) { g_testNeck = t; }
uint64_t neckCalls() { return g_neckCalls.load(); }
uint64_t neckLocalSiteCalls() { return g_neckLocalCalls.load(); }
uintptr_t neckLocalEye() { return g_localEye.load(); }
size_t neckDistinct() {
    size_t n = 0;
    for (size_t i = 0; i < 32; ++i) n += g_neckSeen.key(i) != 0 ? 1 : 0;
    return n;
}
bool neckInstalled() { return g_neckInstalled; }
uintptr_t neckReplacement() { return reinterpret_cast<uintptr_t>(&eyeGetterObserved); }
void reset() {
    uninstallNeck();
    g_armed.store(false);
    g_busy3.store(false);
    g_last3 = f2::Pressed3();
    g_have3 = false;
    g_freeCalls.store(0);
    g_busyCtl.store(false);
    g_lastCtl = f2::ControllerSnap();
    g_haveCtl = false;
    g_ctlCalls.store(0);
    g_ctlMode.store(0);
    g_ctlLast.store(0);
    g_f2Seq.store(0);
    g_neck = f2::NeckTargets();
    g_neckCalls.store(0);
    g_neckLocalCalls.store(0);
    g_localEye.store(0);
    g_neckSeenOverflow.store(0);
    g_staleCount.store(0);
    g_staleVptr.store(0);
    g_neckTried = false;
    g_neckOriginal = 0;
    g_i4Status = "not tried";
    g_neckStatus = "not tried";
    g_announced = false;
    g_testNeck = NeckSeam();
    g_f2 = F2Frame();
    g_neckSeen.clear();
    g_sample.clear();
    f2::F2Event drop;
    while (g_ringFree.take(&drop)) {}
    while (g_ringCtl.take(&drop)) {}
}
}  // namespace explorercamf2test
#endif

}  // namespace edvr
