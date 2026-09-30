#include "flat_camera_inject.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <intrin.h>

#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"
#include "flat_camera_ownership.h"
#include "flat_camera_stubs.h"
#include "flat_runtime.h"

extern "C" DWORD _tls_index; // CRT-provided once a __declspec(thread) exists

namespace edvr {
namespace {

// The generated stubs live in flat_camera_stubs.h so the rig can run them.
using camera_stubs::buildStubA;
using camera_stubs::buildStubB;
using camera_stubs::kStubAOffset;
using camera_stubs::kStubBOffset;

// The upstream camera injector (docs/design-flat-camera-integration.md, C3
// wiring plan addendum). One CodeHook on the game's view-constant refresh
// (FUN_1405921f0): the detour applies the temporal phase to the camera's
// bound pair transiently -- mutate on the entry values, run the original,
// restore immediately (the restore-after-call protocol: no accumulation,
// nothing left behind on disable, mid-frame setters compose naturally).
// Bits 4 and 8 are raised so the refresh's own finalizers re-derive the
// projection and the cached VP from the mutated parameters in the same
// call; bits 2/1 stay untouched (a projection-only jitter leaves view rows
// and ray snapshots legitimately unchanged).
//
// THE MECHANISM, rewritten after five .rdata crashes. The hook site is
// mid-function: the relay JUMPS to the detour with rsp = S, the game's
// live stack -- no return address is pushed, and [S]/[S+8]/[S+0x10] hold
// the game's saved r14, saved r13 and the function's own return address.
// A compiled C detour is incompatible with that twice over: its prologue
// spills through [rsp+8/10h/18h] onto exactly those slots, and a
// call-forward parks the game body 0xE0 bytes below its own frame, so the
// body's epilogue (add rsp,0xE0; pop r15/r14/r13; ret) reads the detour's
// frame -- in the 19:14 dump r14 held the call's own return address
// (d3d11+0x6FCA5) and the body's ret jumped to a spilled log-string
// pointer, which is the RIP-in-.rdata signature of all five crashes.
//
// So the detour is generated code, built at install:
//   stubA (the relay's callback): push all fifteen GPRs (landing
//     16-aligned), call refreshPre with the ABI's 32-byte home area
//     reserved (sub/add rsp,0x20 around the call), pop every register --
//     r15 included, because the trampoline's stolen "push r15" must save
//     the GAME's r15 -- and jmp to the trampoline with rsp = S exactly.
//     The body then sees every register, and every stack byte at and
//     above S, as the unhooked call left them (the stack below S is
//     scratch, as it is for any callee). The home area is the lesson of
//     the 20:09/03:50/04:23 crashes: the first stubs called C with none
//     reserved, refreshPre's MSVC prologue homed rcx and rdx onto stubA's
//     own saved r15 and r14, and the pops handed the game r15 = R0
//     (design-flat-camera-integration.md, 2026-09-29). The stubs are in
//     flat_camera_stubs.h; tools/flat_camera_stub_test runs them against
//     callees that spend their home slots.
//   refreshPre: the pre-forward half (admission, the phase, the dirty
//     bits) plus the redirection that gives the post-half control: the
//     body's ret pops whatever sits at [R0], so stubB's address goes
//     there and the real return address is kept in TLS. A refusal is
//     simply "don't redirect": stubA joins the trampoline with the stack
//     exactly as found and the body returns straight to its caller.
//   stubB: the body's ret lands here with rsp = R0+8 and the game's
//     return state in every register. It preserves rax/xmm0 and the
//     scratch set, calls refreshPost with its own 32-byte home area
//     reserved (restore-after-call, counters, the ray CB observation),
//     then jumps to the real return address from TLS. r11 alone carries
//     the TLS walk: it has no ABI role across a return.
// Single-level per thread: the refresh is not recursive (the C2 lineage
// and the decompile agree), so one TLS slot per thread suffices. A body
// that unwound instead of returning would leave the armed flag set; the
// next call clears it, and nothing else ever reads it.
//
// Safety discipline is the producer probe's, learned from its two crashed
// flights and the probe rebuild: prologue-verified single site, gate-first
// relay that preserves EVERY register (it costs one push and pop; 905bddd0
// made it so on the theory that r11 was load-bearing in the 20:09/03:50
// crashes, and the 04:23 flight crashed identically -- the cause was the
// missing home area above), forward through the trampoline, and hold-open
// for process lifetime -- disable closes the gate and nothing more (no
// uninstall, no free, ever).

constexpr uintptr_t kRefreshRva = 0x592200;
// Two site constraints, learned from two refused installs: the function's
// second instruction is a conditional jump CodeHook will not move, and the
// patch site must be eight-byte aligned for the atomic store. +0x592200 is
// the first site satisfying both (push r15; sub rsp,0xE0; register moves
// after that replay inside the trampoline). The detour's arguments arrive
// in rcx/rdx/r8 untouched either way, and rax (the frame anchor the
// prologue set with mov rax,rsp) must reach the body intact.
constexpr uint8_t kRefreshPrologue[16] = {0x41, 0x57, 0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00,
                                         0x00, 0x4D, 0x8B, 0xF8, 0x4C, 0x8B, 0xEA, 0x4C};
constexpr size_t kRelayBytes = 46;
constexpr uint32_t kCallbackLiteral = 24;
constexpr uint32_t kOriginalLiteral = 38;
// The stubs' page offsets (kStubAOffset, kStubBOffset) come from
// flat_camera_stubs.h with their sizes; the rig checks both against the
// emitters.

// The camera struct's fields (camera-relative, the typed table).
constexpr uint32_t kCamKind = 0x264;
constexpr uint32_t kCamBoundX = 0x28C;
constexpr uint32_t kCamBoundY = 0x290;
constexpr uint32_t kCamFlags = 0x250;
constexpr uint32_t kFlagProj = 4, kFlagVP = 8;

// Per-thread forward state. While the body runs with its return address
// redirected to stubB, everything the post-half needs travels here --
// nothing may live on the stack, because the stack belongs to the game.
struct RefreshTls {
    uint64_t realRet = 0; // MUST STAY FIRST: stubB's disp32 targets this field
    uintptr_t ctx = 0;
    uintptr_t camera = 0;
    uint64_t callNo = 0;
    float entryX = 0, entryY = 0;
    uint32_t flags = 0;
    uint32_t haveFlags = 0;
    uint32_t armed = 0;
    uint32_t pad = 0;
};
__declspec(thread) RefreshTls g_refreshTls;
static_assert(offsetof(RefreshTls, realRet) == 0, "stubB reads realRet at the struct base");

struct InjectState {
    std::atomic<bool> installed{false};
    CodeHook hook;
    uint8_t* relay = nullptr;
    const char* failReason = "not attempted";
    // The per-frame ownership protocol (flat_camera_phase.h): the ownership
    // machine for the main view-group (auxiliary cameras are named unsupported
    // this increment, per group reporting), this frame's decision, and the
    // Legacy fallback hysteresis. Written and read on the Present thread; the
    // detour reads the decision only after the gate has proved it is on that
    // thread.
    FlatCameraFrameCore core;
    bool wanted = false;       // cached each frame: the profile is flat and a temporal mode is selected
    FlatCameraGate gate;       // the frame window the detour may inject in
    FlatCameraInjectedSet injected; // cameras this session injected (flush candidates); owner thread only
    FlatCameraCensus census;   // owner thread only; reset every window
    std::atomic<bool> kind3Seen{false}; // a kind-3 camera reached the detour this frame
    uintptr_t gameBase = 0;    // EliteDangerous64.exe, for the census's call-site offsets
    uint64_t frame = 0;
    // No call-path I/O, ever: the 2026-09-28 20:09 crash (a null-read downstream
    // in the camera pipeline, sentinel-caught) came with the detour doing
    // NOTHING but pass-through plus three note() writes on the game thread
    // inside the view-constant refresh. Its per-call breadcrumbs (the old trace
    // setting, retired 2026-09-29) were deleted with it; everything they said
    // rides out on the fields below and the 5s tick, census and rows lines.
    // Counters, reported on the cadence tick.
    std::atomic<uint64_t> refreshCalls{0};
    std::atomic<uint64_t> injectedCalls{0};
    std::atomic<uint64_t> kindRefusals{0};
    std::atomic<uint64_t> unsupportedCameras{0};
    std::atomic<uint64_t> warmingCalls{0};
    // The wiring's counters (window, exchanged to zero by the tick). Every one
    // is the count of calls or frames that took the named path, so a path that
    // never runs prints zero on a line that is present -- the line's absence
    // is what "the code never ran" looks like.
    std::atomic<uint64_t> staleCalls{0};       // kind-3 calls with the frame window closed or lapsed
    std::atomic<uint64_t> offThreadCalls{0};   // calls on a thread other than Present's
    std::atomic<uint64_t> notUpstreamCalls{0}; // kind-3 calls while another route owns the frame
    std::atomic<uint64_t> flushed{0};          // dirty bits raised on a camera injected earlier
    std::atomic<uint64_t> flushFailed{0};      // the same, refused by memory protection
    std::atomic<uint64_t> writeFailures{0};    // mutation or flush writes that failed (8 in a window stands the hook down)
    std::atomic<uint64_t> closes{0};           // frames closed through flatCameraInjectClose
    std::atomic<uint64_t> cleanCloses{0};      // ... of which the phase machine called clean
    std::atomic<uint64_t> historyResets{0};    // owner switches that reset the runtime's history
    uint64_t windowFrames = 0;                 // frames begun this window (census denominator)
    uint32_t ownerNotes = 0;                   // owner-transition notes written (capped)
    FlatCameraRoute lastRoute = FlatCameraRoute::Off;
    bool lastFallback = false;
    std::atomic<uint64_t> rayCbLogged{0};
    // Last-call triage for the tick (no call-path I/O).
    std::atomic<uint64_t> lastCallNo{0};
    std::atomic<uintptr_t> lastCtx{0};
    std::atomic<uintptr_t> lastP2{0};
    std::atomic<uintptr_t> lastCamera{0};
    std::atomic<uint32_t> lastKind{0};
    std::atomic<uint64_t> raySeq{0};
    uint64_t lastRaySeqReported = 0;
    uintptr_t lastRaySlot = 0;
    uint64_t lastLogMs = 0;
    float lastRay[4] = {};
};
InjectState g_inject;
std::atomic<uintptr_t> g_gate{0};
std::atomic<uintptr_t> g_refreshForward{0};
// The incoming r11 of the most recent refresh call, captured by stubA's
// moffs store BEFORE anything can clobber it. It was added when the r11
// convention was suspected in the 20:09/03:50 crashes (an inherited frame
// base the unhooked refresh preserves); that suspicion is retired (the cause
// was stubA's home area), and the tick still prints the value, one store
// per call.
std::atomic<uintptr_t> g_lastIncomingR11{0};
uintptr_t g_stubB = 0;
uint32_t g_stubATrampOfs = 0;

bool sehCheck(uintptr_t at, const uint8_t* expected, size_t bytes) noexcept {
    __try { return std::memcmp(reinterpret_cast<const void*>(at), expected, bytes) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehReadU32(uintptr_t at, uint32_t* out) noexcept {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehReadF32(uintptr_t at, float* out) noexcept {
    __try {
        *out = *reinterpret_cast<const float*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehReadU64(uintptr_t at, uint64_t* out) noexcept {
    __try {
        *out = *reinterpret_cast<const uint64_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehWriteF32(uintptr_t at, float value) noexcept {
    __try {
        *reinterpret_cast<float*>(at) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehWriteU32(uintptr_t at, uint32_t value) noexcept {
    __try {
        *reinterpret_cast<uint32_t*>(at) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool sehWriteU64(uintptr_t at, uint64_t value) noexcept {
    __try {
        *reinterpret_cast<uint64_t*>(at) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uint8_t* allocateRelay(uintptr_t target) noexcept {
    const uintptr_t granularity = 64 * 1024, span = 0x7FFF0000ull;
    const uintptr_t begin = (target - span) & ~(granularity - 1);
    for (uintptr_t candidate = target; candidate >= begin; candidate -= granularity) {
        auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096,
                                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (p) return p;
    }
    return nullptr;
}

void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    // push rax; mov rax,&gate; cmp qword ptr[rax],0; pop rax; jz original;
    // jmp [callback]; original: jmp [trampoline].
    //
    // NO register is left clobbered. The first form carried the gate in r11:
    // rax was the known constraint (this target's frame anchor is mov rax,rsp
    // BEFORE the patch site, so rax must survive), and 905bddd0 also kept r11
    // on the theory that this pipeline family passes a frame base in it
    // (FUN_140594dc8 spills through [r11+0x10] at entry). That theory was not
    // the crash: the 04:23 flight crashed identically with this relay, and
    // the cause was stubA's missing home area (see the header comment). The
    // relay stays as it is because it costs one push and pop and leaves the
    // caller nothing to depend on. The cmp clobbers flags, which is safe
    // because the trampoline's stolen sub rsp,0xE0 re-sets them before the
    // body can read them.
    const uint8_t body[kRelayBytes] = {
        0x50, 0x48,0xB8,0,0,0,0,0,0,0,0, 0x48,0x83,0x38,0,
        0x58, 0x74,0x0E, 0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0,
        0xFF,0x25,0,0,0,0, 0,0,0,0,0,0,0,0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 3, &gateAddress, 8);
    std::memcpy(code + kCallbackLiteral, &callbackAddress, 8);
}

bool prepareRelay(void* trampoline, void*) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(g_inject.relay + kOriginalLiteral, &address, 8);
    std::memcpy(g_inject.relay + kStubAOffset + g_stubATrampOfs, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(g_inject.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) ||
        !FlushInstructionCache(GetCurrentProcess(), g_inject.relay, 4096)) return false;
    g_refreshForward.store(address, std::memory_order_release);
    return true;
}

// The ray CB observation: after the original refresh returns, read the
// slot record at lVar4+0x78 (lVar4 = *(ctx+0x28) per the refresh's own
// layout) and record the composition's first floats when they materially
// change. This is the follow-up that pins the consumer: the composed
// values cross-check the rig's composeRayCb against the live game, and
// the slot record is the handle for naming the binding shader next. The
// reads stay on the call path (plain memory, no I/O); the note is emitted
// from the 5s tick.
void observeRayCb(uintptr_t ctx) {
    uint64_t lVar4 = 0;
    if (!sehReadU64(ctx + 0x28, &lVar4) || !lVar4) return;
    uint64_t staging = 0;
    if (!sehReadU64(static_cast<uintptr_t>(lVar4) + 0x78, &staging) || !staging) return;
    float row[4] = {};
    bool ok = true;
    for (uint32_t i = 0; i < 4; ++i) ok &= sehReadF32(static_cast<uintptr_t>(staging) + 4 * i, &row[i]);
    if (!ok) return;
    const float drift = std::fabs(row[0] - g_inject.lastRay[0]) +
                        std::fabs(row[1] - g_inject.lastRay[1]) +
                        std::fabs(row[2] - g_inject.lastRay[2]) +
                        std::fabs(row[3] - g_inject.lastRay[3]);
    if (g_inject.rayCbLogged.load(std::memory_order_relaxed) != 0 && drift < 1e-3f) return;
    for (int i = 0; i < 4; ++i) g_inject.lastRay[i] = row[i];
    g_inject.lastRaySlot = static_cast<uintptr_t>(staging);
    const uint64_t n = g_inject.rayCbLogged.fetch_add(1, std::memory_order_relaxed) + 1;
    g_inject.raySeq.store(n, std::memory_order_relaxed); // the tick reports the anchor
}

// The flush: raise the dirty bits (projection and cached VP) on a camera this
// session injected and is not injecting now, so the game's own refresh
// re-derives it from the pristine bound pair the restore left behind. One
// SEH-guarded write of the flag word the injection itself writes; the body
// clears the bits as it consumes them, so nothing is restored afterwards.
// Counted either way, and a failed write counts toward the stand-down.
void flushCamera(uintptr_t camera) noexcept {
    uint32_t flags = 0;
    if (sehReadU32(camera + kCamFlags, &flags) && sehWriteU32(camera + kCamFlags, flags | kFlagProj | kFlagVP)) {
        g_inject.flushed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_inject.flushFailed.fetch_add(1, std::memory_order_relaxed);
    g_inject.writeFailures.fetch_add(1, std::memory_order_relaxed);
}

// The pre-forward half, called by stubA with R0 (the game's return-
// address slot) and the live argument registers. Everything the old
// detour did before forward() -- admission, the phase, the dirty bits --
// plus the return redirection. Any refusal leaves the camera and [R0]
// untouched, which is the whole of "forward unmodified" now: stubA joins
// the trampoline regardless and the body returns straight to its caller.
void refreshPre(uintptr_t r0, uintptr_t ctx, uintptr_t p2, uintptr_t camera) noexcept {
    g_refreshTls.armed = 0; // a previous body that unwound never disarmed
    const uint64_t callNo = g_inject.refreshCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    // The triage fields ride out on the 5s tick (the lastcall line); no I/O here.
    g_inject.lastCallNo.store(callNo, std::memory_order_relaxed);
    g_inject.lastCtx.store(ctx, std::memory_order_relaxed);
    g_inject.lastP2.store(p2, std::memory_order_relaxed);
    g_inject.lastCamera.store(camera, std::memory_order_relaxed);

    // The thread first. The phase machine, the decision, the census and the
    // set of injected cameras belong to the thread that runs Present; a call
    // on any other thread touches none of them (counted, passed through).
    const FlatCameraGateVerdict gate = g_inject.gate.check(GetCurrentThreadId(), GetTickCount64());
    if (gate == FlatCameraGateVerdict::OffThread) {
        g_inject.offThreadCalls.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Admission for THIS call (flat_camera_phase.h, flatCameraAdmit): the
    // injector owns only a kind-3 camera, in an open frame window, on the owner
    // thread, whose frame ownership is Upstream and whose phase is non-zero.
    // Anything else passes through untouched and is counted by the reason
    // (unsupported cameras are named, not silently jittered).
    uint32_t kind = 0;
    const bool readable = camera && sehReadU32(camera + kCamKind, &kind);
    g_inject.lastKind.store(readable ? kind : 0xffffffffu, std::memory_order_relaxed);
    g_inject.census.noteKind(readable, kind);
    {
        // The call site, for the census: [R0] is the return address, and its
        // offset from the module names which of the refresh's callers this is.
        uint64_t ret = 0, rva = 0;
        if (g_inject.gameBase && sehReadU64(r0, &ret) && ret > g_inject.gameBase) rva = ret - g_inject.gameBase;
        g_inject.census.noteCaller(rva);
    }
    // The phase, in RENDER pixels from the validated resolve plan (R5).
    float jx = 0, jy = 0;
    uint32_t rw = 0, rh = 0, applied = 0;
    FlatCameraAdmitInput admission;
    admission.readable = readable;
    admission.kind = kind;
    admission.gate = gate;
    if (readable && kind == 3) {
        g_inject.kind3Seen.store(true, std::memory_order_relaxed);
        flatRuntimePhaseState(&jx, &jy, &rw, &rh, &applied);
        admission.upstreamOwns = g_inject.core.valid() && g_inject.core.route() == FlatCameraRoute::Upstream;
        admission.phaseNonzero = rw && rh && (jx != 0.0f || jy != 0.0f);
    }
    const FlatCameraAdmit admit = flatCameraAdmit(admission);
    switch (admit) {
        case FlatCameraAdmit::Unsupported:
            g_inject.unsupportedCameras.fetch_add(1, std::memory_order_relaxed);
            [[fallthrough]];
        case FlatCameraAdmit::OtherKind:
        case FlatCameraAdmit::Unreadable:
            g_inject.kindRefusals.fetch_add(1, std::memory_order_relaxed);
            break;
        case FlatCameraAdmit::Warming: g_inject.warmingCalls.fetch_add(1, std::memory_order_relaxed); break;
        case FlatCameraAdmit::NotUpstream: g_inject.notUpstreamCalls.fetch_add(1, std::memory_order_relaxed); break;
        case FlatCameraAdmit::GateClosed: g_inject.staleCalls.fetch_add(1, std::memory_order_relaxed); break;
        case FlatCameraAdmit::Inject:
        case FlatCameraAdmit::OffThread: break;
    }
    // A camera this session injected, now not: its derived blocks still hold
    // the last phase, and the game re-derives only what its dirty bits name.
    // One write, once per such edge, never on a camera never injected.
    if (flatCameraFlushDecision(g_inject.injected, camera, admit)) flushCamera(camera);
    if (admit != FlatCameraAdmit::Inject) {
        // A camera that is not the proven branch is named by the census (kinds, top cameras),
        // counted by the tick (kind-refusals, unsupported) and left untouched.
        if (camera) g_inject.census.note(camera, kind, false);
        return;
    }

    float entryX = 0, entryY = 0;
    if (!sehReadF32(camera + kCamBoundX, &entryX) || !sehReadF32(camera + kCamBoundY, &entryY)) {
        g_inject.writeFailures.fetch_add(1, std::memory_order_relaxed);
        g_inject.census.note(camera, kind, false);
        return;
    }
    // The D3D sign convention W1 proved: content right by jx needs
    // boundX += jx/R_w; content down by jy needs boundY += -jy/R_h.
    const float jitX = entryX + jx / static_cast<float>(rw);
    const float jitY = entryY - jy / static_cast<float>(rh);
    uint32_t flags = 0;
    const bool haveFlags = sehReadU32(camera + kCamFlags, &flags);
    const bool wroteX = sehWriteF32(camera + kCamBoundX, jitX);
    const bool wroteY = wroteX && sehWriteF32(camera + kCamBoundY, jitY);
    const bool wroteAll = wroteY && (!haveFlags || sehWriteU32(camera + kCamFlags, flags | kFlagProj | kFlagVP));
    if (!wroteAll) { // roll back whatever landed; the body runs pristine
        if (wroteX) sehWriteF32(camera + kCamBoundX, entryX);
        if (wroteY) sehWriteF32(camera + kCamBoundY, entryY);
        g_inject.writeFailures.fetch_add(1, std::memory_order_relaxed);
        g_inject.census.note(camera, kind, false);
        return;
    }
    // Redirect the body's return to stubB. This must be the LAST step: if
    // it fails the mutation is undone and the body returns to its caller.
    uint64_t realRet = 0;
    if (!sehReadU64(r0, &realRet) || !sehWriteU64(r0, static_cast<uint64_t>(g_stubB))) {
        sehWriteF32(camera + kCamBoundX, entryX);
        sehWriteF32(camera + kCamBoundY, entryY);
        if (haveFlags) sehWriteU32(camera + kCamFlags, flags);
        g_inject.writeFailures.fetch_add(1, std::memory_order_relaxed);
        g_inject.census.note(camera, kind, false);
        return;
    }
    g_refreshTls.realRet = realRet;
    g_refreshTls.ctx = ctx;
    g_refreshTls.camera = camera;
    g_refreshTls.callNo = callNo;
    g_refreshTls.entryX = entryX;
    g_refreshTls.entryY = entryY;
    g_refreshTls.flags = flags;
    g_refreshTls.haveFlags = haveFlags ? 1u : 0u;
    g_refreshTls.armed = 1u;
    // Committed: this camera now carries a phase the game will keep after the
    // bound pair is restored, so it is a flush candidate from here on.
    g_inject.injected.noteInjected(camera);
    g_inject.census.note(camera, kind, true);
}

// The post-forward half, called by stubB after the body's ret. The entry
// values ARE the authoritative originals (restore-after-call, unchanged):
// the derived blocks keep this call's jitter (the committed phase); the
// sources return pristine for the next derivation.
void refreshPost() noexcept {
    if (!g_refreshTls.armed) return; // defensive: stubB only fires after a redirect
    g_refreshTls.armed = 0;
    const uintptr_t camera = g_refreshTls.camera;
    sehWriteF32(camera + kCamBoundX, g_refreshTls.entryX);
    sehWriteF32(camera + kCamBoundY, g_refreshTls.entryY);
    if (g_refreshTls.haveFlags) sehWriteU32(camera + kCamFlags, g_refreshTls.flags);
    g_inject.injectedCalls.fetch_add(1, std::memory_order_relaxed);
    flatRuntimeNoteCameraApplied();
    observeRayCb(g_refreshTls.ctx);
}

void standDown(const char* why) {
    if (g_gate.exchange(0, std::memory_order_acq_rel) != 0) {
        Log::get().note("flat camera inject: %s; the refresh hook stays in place as an inert "
                        "pass-through for process lifetime", why);
    }
}

} // namespace

bool flatCameraInjectUpstreamOwns() {
    return g_inject.wanted && g_inject.core.valid() && g_inject.core.route() == FlatCameraRoute::Upstream;
}

bool flatCameraInjectBypassRefusal(const char* reason) {
    if (!flatCameraInjectUpstreamOwns() || !reason) return false;
    // The legacy-only classes: reasons that can only arise from the legacy
    // route's own preparation and binding, which Upstream ownership suppresses
    // -- an unknown recipe on a certified camera lineage, a private
    // preparation the frame does not use, a binding it never makes. None of
    // them may veto a route that does not consume them. True violations (a
    // scene depth that changed, a render extent that moved, a foreign write)
    // keep refusing in flat_runtime.
    return std::strcmp(reason, "unknown-scene-projection-recipe") == 0 ||
           std::strcmp(reason, "projection-preparation-refused") == 0 ||
           std::strcmp(reason, "draw-binding-refused") == 0;
}

FlatCameraRoute flatCameraInjectRoute() {
    return g_inject.wanted ? g_inject.core.route() : FlatCameraRoute::Off;
}

bool flatCameraInjectTakeHistoryReset() { return g_inject.wanted && g_inject.core.takeHistoryReset(); }

// The window opens once the frame's phase is chosen; every Present edge closes
// it, on the thread that runs Present.
void flatCameraInjectArm() {
    if (!g_inject.wanted || !g_inject.core.valid()) return;
    g_inject.gate.arm(GetTickCount64());
}
void flatCameraInjectDisarm() { g_inject.gate.disarm(GetCurrentThreadId()); }

void flatCameraInjectClose(bool phaseNonzero, bool applied, bool clean, bool sceneNamed) {
    if (!g_inject.wanted) return;
    const bool kind3 = g_inject.kind3Seen.exchange(false, std::memory_order_relaxed);
    if (!g_inject.core.close(phaseNonzero, applied, clean, sceneNamed, kind3)) return;
    g_inject.closes.fetch_add(1, std::memory_order_relaxed);
    if (clean) g_inject.cleanCloses.fetch_add(1, std::memory_order_relaxed);
}

void flatCameraInjectReset() {
    g_inject.core.reset();
    g_inject.gate.disarm(g_inject.gate.ownerThread());
    g_inject.census.reset();
    g_inject.windowFrames = 0;
    g_inject.lastRoute = FlatCameraRoute::Off;
    g_inject.lastFallback = false;
    // g_inject.injected is kept on purpose: the cameras this session injected
    // still hold the last phase, and their first un-injected call flushes them.
}

void flatCameraInjectFrame(uint64_t frame, bool temporalModeEnabled) {
    // The camera path is on whenever the flat profile has a temporal mode selected; there is no
    // key. Not wanted (another profile, or the mode is off) installs nothing, writes nothing and
    // logs nothing -- the flat runtime returns before it gets here with the mode off, and this
    // says so a second time. It does NOT stand the hook down: an installed hook keeps its gate (the
    // frame window lapses by itself with no Present arming it), so selecting a mode again needs
    // no re-arming. The stand-downs below are for the four things that cannot recover.
    g_inject.wanted = flatCameraPathWanted(runtimeFlatProfile(), temporalModeEnabled);
    if (!g_inject.wanted) {
        g_inject.core.invalidate();
        return;
    }
    if (frame != g_inject.frame) {
        g_inject.frame = frame;
        // The upstream route is certified per call (the detour re-verifies kind
        // 3); the fallback hysteresis feeds the policy's unsupported input.
        const FlatCameraOwnershipDecision& decision = g_inject.core.begin(flatRuntimeLegacyPlanExists());
        ++g_inject.windowFrames;
        if (decision.historyReset) g_inject.historyResets.fetch_add(1, std::memory_order_relaxed);
        const FlatCameraRoute route = g_inject.core.route();
        const bool fallback = g_inject.core.fallback().active();
        if ((route != g_inject.lastRoute || fallback != g_inject.lastFallback) && g_inject.ownerNotes < 24) {
            ++g_inject.ownerNotes;
            char text[256];
            flatCameraFormatOwner(text, sizeof(text), frame, flatCameraRouteName(g_inject.lastRoute),
                                  flatCameraRouteName(route), decision.reason, decision.historyReset, fallback);
            Log::get().note("%s", text);
        }
        g_inject.lastRoute = route;
        g_inject.lastFallback = fallback;
    }
    if (!g_inject.installed.load(std::memory_order_acquire)) {
        if (g_inject.relay) return; // a failed install is final for the session
        const HMODULE game = GetModuleHandleW(L"EliteDangerous64.exe");
        if (!game) {
            if (!g_inject.failReason[0] || std::strcmp(g_inject.failReason, "not attempted") == 0) {
                g_inject.failReason = "EliteDangerous64.exe is not loaded in this process";
                Log::get().note("flat camera inject: wanted but %s; standing down", g_inject.failReason);
            }
            return;
        }
        const uintptr_t base = reinterpret_cast<uintptr_t>(game);
        g_inject.gameBase = base; // before the hook exists: the detour reads it
        if (!sehCheck(base + kRefreshRva, kRefreshPrologue, sizeof(kRefreshPrologue))) {
            g_inject.failReason = "refresh prologue mismatch at this build (not the Ghidra-verified shape)";
            Log::get().note("flat camera inject: %s; standing down", g_inject.failReason);
            g_inject.relay = reinterpret_cast<uint8_t*>(1); // do not retry
            return;
        }
        g_inject.relay = allocateRelay(base + kRefreshRva);
        if (!g_inject.relay) { g_inject.failReason = "relay allocation failed (no free memory within 2 GB)"; return; }
        // TLS placement for stubB's walk: _tls_index is process-constant
        // after CRT init, and the struct's offset from this thread's TLS
        // base is the same on every thread.
        const auto tlsArray = reinterpret_cast<const uintptr_t*>(__readgsqword(0x58));
        const uintptr_t tlsBase = tlsArray ? tlsArray[_tls_index] : 0;
        const uintptr_t tlsStruct = reinterpret_cast<uintptr_t>(&g_refreshTls);
        if (!tlsBase || tlsStruct < tlsBase || tlsStruct - tlsBase > 0xFFFFFFFFull ||
            _tls_index == 0) {
            static char tlsDetail[160];
            std::snprintf(tlsDetail, sizeof(tlsDetail),
                "the thread-local the return stub needs is outside its reach "
                "(tlsBase=%p tlsStruct=%p tlsIndex=%u)",
                reinterpret_cast<void*>(tlsBase), reinterpret_cast<void*>(tlsStruct), _tls_index);
            g_inject.failReason = tlsDetail;
            Log::get().note("flat camera inject: %s; standing down", g_inject.failReason);
            VirtualFree(g_inject.relay, 0, MEM_RELEASE);
            g_inject.relay = reinterpret_cast<uint8_t*>(1);
            return;
        }
        g_stubB = reinterpret_cast<uintptr_t>(g_inject.relay) + kStubBOffset;
        buildRelay(g_inject.relay, &g_gate, g_inject.relay + kStubAOffset);
        g_stubATrampOfs = buildStubA(g_inject.relay + kStubAOffset, &refreshPre,
                                     &g_lastIncomingR11);
        buildStubB(g_inject.relay + kStubBOffset, &refreshPost, _tls_index,
                   static_cast<uint32_t>(tlsStruct - tlsBase));
        if (!g_inject.hook.install(reinterpret_cast<void*>(base + kRefreshRva), g_inject.relay, nullptr,
                                   "camera-inject-refresh", &prepareRelay, &g_inject)) {
            VirtualFree(g_inject.relay, 0, MEM_RELEASE); g_inject.relay = reinterpret_cast<uint8_t*>(1);
            g_inject.failReason = "CodeHook refused it (its own line above names why)";
            Log::get().note("flat camera inject: %s; standing down", g_inject.failReason);
            return;
        }
        g_inject.installed.store(true, std::memory_order_release);
        g_gate.store(1, std::memory_order_release);
        Log::get().note("flat camera inject: refresh hook installed at EliteDangerous64.exe+0x%llX; "
                        "kind-3 cameras now get the temporal phase applied transiently at the source",
                        static_cast<unsigned long long>(kRefreshRva));
        // Readback verification: the relay stub, its callback literal, the
        // trampoline's and stubA's first bytes, so a later crash can be
        // compared against what was actually built.
        uint8_t relayBytes[46] = {};
        uint64_t callbackLiteral = 0, fwd = g_refreshForward.load(std::memory_order_relaxed);
        uint8_t trampBytes[16] = {};
        uint8_t stubABytes[16] = {};
        __try {
            std::memcpy(relayBytes, g_inject.relay, sizeof(relayBytes));
            std::memcpy(&callbackLiteral, g_inject.relay + kCallbackLiteral, 8);
            std::memcpy(trampBytes, reinterpret_cast<const void*>(fwd), sizeof(trampBytes));
            std::memcpy(stubABytes, g_inject.relay + kStubAOffset, sizeof(stubABytes));
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        char hex1[192]{}, hex2[96]{}, hex3[96]{};
        for (int i = 0; i < 46; ++i) std::snprintf(hex1 + i * 3, sizeof(hex1) - i * 3, "%02X ", relayBytes[i]);
        for (int i = 0; i < 16; ++i) std::snprintf(hex2 + i * 3, sizeof(hex2) - i * 3, "%02X ", trampBytes[i]);
        for (int i = 0; i < 16; ++i) std::snprintf(hex3 + i * 3, sizeof(hex3) - i * 3, "%02X ", stubABytes[i]);
        Log::get().note("flat camera inject: relay[0..45]=%s| stubA=%p trampoline=%p tramp[0..15]=%s stubA[0..15]=%s",
                        hex1, reinterpret_cast<void*>(callbackLiteral), reinterpret_cast<void*>(fwd), hex2, hex3);
    }
    const uint64_t now = GetTickCount64();
    if (now - g_inject.lastLogMs >= 5000) {
        g_inject.lastLogMs = now;
        // The tick, its fields and their order in flat_camera_phase.h (the
        // rig prints the same text). history= is the ownership machine's
        // verdict on the last CLOSED frame; before the wiring nothing closed a
        // frame and it read "invalid" forever, which is also what it reads if
        // flatCameraInjectClose never runs.
        FlatCameraTickFields tick;
        tick.refreshCalls = g_inject.refreshCalls.exchange(0);
        tick.injected = g_inject.injectedCalls.exchange(0);
        tick.warming = g_inject.warmingCalls.exchange(0);
        tick.kindRefusals = g_inject.kindRefusals.exchange(0);
        tick.unsupported = g_inject.unsupportedCameras.exchange(0);
        tick.owner = g_inject.core.valid() && g_inject.core.route() == FlatCameraRoute::Upstream
            ? "upstream" : "legacy/none";
        tick.history = g_inject.core.historyValid() ? "valid" : "invalid";
        tick.closes = g_inject.closes.exchange(0);
        tick.cleanCloses = g_inject.cleanCloses.exchange(0);
        tick.stale = g_inject.staleCalls.exchange(0);
        tick.offThread = g_inject.offThreadCalls.exchange(0);
        tick.notUpstream = g_inject.notUpstreamCalls.exchange(0);
        tick.flushed = g_inject.flushed.exchange(0);
        tick.flushFailed = g_inject.flushFailed.exchange(0);
        tick.writeFailures = g_inject.writeFailures.exchange(0);
        tick.historyResets = g_inject.historyResets.exchange(0);
        tick.fallbackActive = g_inject.core.fallback().active();
        tick.fallbacks = g_inject.core.fallback().engagements();
        tick.setEvicted = g_inject.injected.evicted();
        char text[640];
        flatCameraFormatTick(text, sizeof(text), tick);
        Log::get().note("%s", text);
        // The census, every window while the hook is installed, INCLUDING an
        // empty one (cameras=0): an absent line is what "never ran" looks like.
        char census[1100];
        flatCameraFormatCensus(census, sizeof(census), g_inject.census, g_inject.windowFrames, tick.injected);
        Log::get().note("%s", census);
        g_inject.census.reset();
        g_inject.windowFrames = 0;
        // kFlatCameraWriteFailureLimit failed writes in one window (a camera the game
        // unmapped under us, a page protection that changed): the hook stands down by name
        // and stays inert for the session. The fallback then hands frames to Legacy.
        if (flatCameraWriteFailureStandDown(tick.writeFailures)) {
            char why[96];
            std::snprintf(why, sizeof(why), "%llu or more camera writes failed in one 5s window",
                          (unsigned long long)kFlatCameraWriteFailureLimit);
            standDown(why);
        }
        // The call triage that used to ride the per-call notes, reported
        // here instead: no I/O on the game's refresh path (the 20:09
        // crash hypothesis).
        const uint64_t lastNo = g_inject.lastCallNo.load(std::memory_order_relaxed);
        if (lastNo) {
            Log::get().note("flat camera inject lastcall: #%llu ctx=%p p2=%p camera=%p kind=%u r11-in=%p",
                (unsigned long long)lastNo,
                reinterpret_cast<void*>(g_inject.lastCtx.load(std::memory_order_relaxed)),
                reinterpret_cast<void*>(g_inject.lastP2.load(std::memory_order_relaxed)),
                reinterpret_cast<void*>(g_inject.lastCamera.load(std::memory_order_relaxed)),
                g_inject.lastKind.load(std::memory_order_relaxed),
                reinterpret_cast<void*>(g_lastIncomingR11.load(std::memory_order_relaxed)));
        }
        const uint64_t raySeq = g_inject.raySeq.load(std::memory_order_relaxed);
        if (raySeq != g_inject.lastRaySeqReported) {
            g_inject.lastRaySeqReported = raySeq;
            Log::get().note("flat camera inject ray CB: slot %p anchors (%llux): [%.5f %.5f %.5f %.5f]",
                reinterpret_cast<void*>(g_inject.lastRaySlot), (unsigned long long)raySeq,
                g_inject.lastRay[0], g_inject.lastRay[1], g_inject.lastRay[2], g_inject.lastRay[3]);
        }
    }
}

} // namespace edvr
