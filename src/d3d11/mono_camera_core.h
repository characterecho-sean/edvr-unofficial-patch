#pragma once

// The terrain-culling arc's MONO CAMERA probe, the pure half (docs\terrain-culling.md, round 5). TEMPORARY: it goes with
// advanced.cull_probe when the arc closes.
//
// WHAT IT TESTS. The eye cameras build their frustum from the kind-5 matrix, which the projection lies cannot reach (round 3 and 4
// ruled the getters, the caller census and the viewport out), so the culler's input has to be somewhere else. The one camera left that
// the game builds on its own is the MONO camera (a kind 0/3 camera, EliteDangerous64.exe+2871D30): its aspect is read from the camera at
// +70h by a one-instruction getter, and it is written only by FUN_28634E0. If the squares come and go with the mono camera's frustum,
// widening that frustum by lying about its aspect should admit more planet-terrain tiles at the periphery, which the cycle's draw
// counter sees (cull_cycle.h).
//
//   THE LIE. The getter at +2841190 is `movss xmm0,[rcx+70h]` and a return. The mono filler calls it at +2871D86
//   (`call qword ptr [rax+40h]`), so the address it returns to is +2871D89. The detour calls the original and, ONLY when the game's
//   return address is that one AND a mono window is active, returns the aspect times 1.30. Every other caller, and every call while no
//   window is active, gets the original's value in xmm0 untouched.
//
//   THE OBSERVATION. Two things are logged whenever the hook is live, so one flight reads more than the squares: the first sight of each
//   distinct return address that calls the getter (up to 16), and one line per call of the writer FUN_28634E0 (up to 50: its width and
//   height, the minimum aspect, what *out held before and after, and where it was called from). The writer's hook is observe-only: it
//   calls the original with the arguments it was given and returns what the original returned. Both are written at the frame boundary,
//   never from inside the detours.
//
// Build 332841 only. The gate is the executable's PE stamp and image size plus the bytes of both functions: on any mismatch the hooks
// are not installed and the log says so once.
//
// Pure bookkeeping, no windows.h: mono_camera_hook.cpp wires it to the process, tools\native_frame_test drives every case.
#include "cull_cycle.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::monocam {

constexpr uint32_t kGetterRva = 0x2841190;       // movss xmm0,[rcx+70h] ; ret
constexpr uint32_t kFillerCallRva = 0x2871D86;   // call qword ptr [rax+40h] in the mono filler (FF 50 40)
constexpr uint32_t kFillerReturnRva = 0x2871D89; // ...and the address it returns to
constexpr uint32_t kWriterRva = 0x28634E0;       // FUN_28634E0, the only writer of the camera's aspect (+80h on its owner)
constexpr float kFactor = cullcycle::kMonoFactor;
constexpr unsigned kCallerCap = 16, kWriterCap = 50;

inline constexpr uint8_t kGetterBytes[6] = {0xF3, 0x0F, 0x10, 0x41, 0x70, 0xC3};
inline constexpr uint8_t kWriterPrologue[24] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29, 0x74, 0x24, 0x40, 0x48,
                                                0x8B, 0xD9, 0x44, 0x0F, 0x29, 0x44, 0x24, 0x20, 0x44, 0x0F, 0x28, 0xC3};

// The gate: null when the hooks may go in, else why not. `getter` and `writer` are the bytes read from the executable at the two RVAs
// (null when they could not be read).
inline const char* gateReason(bool headersRead, uint32_t stamp, uint32_t imageSize, const uint8_t* getter, const uint8_t* writer) {
    if (!headersRead) return "the executable's headers could not be read";
    if (stamp != cullcycle::kBuildStamp || imageSize != cullcycle::kBuildImageSize) return "not build 332841";
    if (!getter || std::memcmp(getter, kGetterBytes, sizeof(kGetterBytes)) != 0) return "the aspect getter's bytes differ";
    if (!writer || std::memcmp(writer, kWriterPrologue, sizeof(kWriterPrologue)) != 0) return "the aspect writer's prologue differs";
    return nullptr;
}

// The detour's decision: the original's value, untouched, unless the caller is the mono filler and a mono window is active.
inline float decide(float original, uintptr_t returnAddress, uintptr_t fillerReturn, bool windowActive) {
    if (!windowActive || fillerReturn == 0 || returnAddress != fillerReturn) return original;
    return original * kFactor;
}

// First sights and the writer's calls, recorded from any thread and written by the frame thread.
class Observer {
public:
    void reset(uintptr_t base, uintptr_t fillerReturn) {
        base_ = base;
        fillerReturn_ = fillerReturn;
        callers_.store(0, std::memory_order_relaxed);
        callerOverflow_.store(false, std::memory_order_relaxed);
        callersDrained_ = 0;
        callerCapSaid_ = false;
        writers_.store(0, std::memory_order_relaxed);
        writerOverflow_.store(false, std::memory_order_relaxed);
        writersDrained_ = 0;
        writerCapSaid_ = false;
        for (unsigned i = 0; i < kWriterCap; ++i) ready_[i].store(false, std::memory_order_relaxed);
    }

    // The getter was called from `returnAddress`: the first sight of each distinct address is kept, up to kCallerCap.
    void noteGetterCaller(uintptr_t returnAddress) noexcept {
        unsigned n = callers_.load(std::memory_order_acquire);
        for (unsigned i = 0; i < n; ++i)
            if (caller_[i].load(std::memory_order_relaxed) == returnAddress) return;
        while (lock_.test_and_set(std::memory_order_acquire)) {}
        n = callers_.load(std::memory_order_relaxed);
        bool seen = false;
        for (unsigned i = 0; i < n && !seen; ++i) seen = caller_[i].load(std::memory_order_relaxed) == returnAddress;
        if (!seen) {
            if (n >= kCallerCap) {
                callerOverflow_.store(true, std::memory_order_relaxed);
            } else {
                caller_[n].store(returnAddress, std::memory_order_relaxed);
                callers_.store(n + 1, std::memory_order_release);
            }
        }
        lock_.clear(std::memory_order_release);
    }

    // The writer was called and has returned. `outOk` says *out could be read; before and after are its value at each end.
    void noteWriter(uint32_t width, uint32_t height, float minAspect, bool outOk, float before, float after, uintptr_t returnAddress) noexcept {
        const uint32_t index = writers_.fetch_add(1, std::memory_order_relaxed);
        if (index >= kWriterCap) {
            writerOverflow_.store(true, std::memory_order_relaxed);
            return;
        }
        WriterRecord& r = writer_[index];
        r.width = width;
        r.height = height;
        r.minAspect = minAspect;
        r.outOk = outOk;
        r.before = before;
        r.after = after;
        r.returnAddress = returnAddress;
        ready_[index].store(true, std::memory_order_release);
    }

    // Write what has been recorded since the last drain, in order. Frame thread only.
    template <class Sink>
    void drain(Sink&& sink) {
        char line[256];
        const unsigned n = callers_.load(std::memory_order_acquire);
        while (callersDrained_ < n) {
            const uintptr_t at = caller_[callersDrained_].load(std::memory_order_relaxed);
            std::snprintf(line, sizeof(line), "mono camera: aspect getter called from exe+0x%llX (caller %u)%s",
                          static_cast<unsigned long long>(at - base_), callersDrained_ + 1,
                          at == fillerReturn_ ? " -- the mono filler" : "");
            sink(line);
            ++callersDrained_;
        }
        if (callerOverflow_.load(std::memory_order_relaxed) && !callerCapSaid_) {
            std::snprintf(line, sizeof(line), "mono camera: more than %u distinct aspect-getter callers; the rest are not logged", kCallerCap);
            sink(line);
            callerCapSaid_ = true;
        }
        while (writersDrained_ < kWriterCap && ready_[writersDrained_].load(std::memory_order_acquire)) {
            const WriterRecord& r = writer_[writersDrained_];
            char out[80];
            if (r.outOk) std::snprintf(out, sizeof(out), "%.6f -> %.6f", static_cast<double>(r.before), static_cast<double>(r.after));
            else std::snprintf(out, sizeof(out), "unreadable");
            std::snprintf(line, sizeof(line),
                          "mono camera: aspect writer call %u: width %u, height %u, min aspect %.6f, out %s, return exe+0x%llX",
                          writersDrained_ + 1, r.width, r.height, static_cast<double>(r.minAspect), out,
                          static_cast<unsigned long long>(r.returnAddress - base_));
            sink(line);
            ++writersDrained_;
        }
        if (writerOverflow_.load(std::memory_order_relaxed) && writersDrained_ == kWriterCap && !writerCapSaid_) {
            std::snprintf(line, sizeof(line), "mono camera: aspect writer: %u calls logged, the rest are not logged", kWriterCap);
            sink(line);
            writerCapSaid_ = true;
        }
    }

    unsigned callersSeen() const { return callers_.load(std::memory_order_acquire); }
    uint32_t writerCalls() const { return writers_.load(std::memory_order_relaxed); }

private:
    struct WriterRecord {
        uint32_t width = 0, height = 0;
        float minAspect = 0.0f, before = 0.0f, after = 0.0f;
        bool outOk = false;
        uintptr_t returnAddress = 0;
    };
    uintptr_t base_ = 0, fillerReturn_ = 0;
    std::atomic<uintptr_t> caller_[kCallerCap]{};
    std::atomic<unsigned> callers_{0};
    std::atomic<bool> callerOverflow_{false};
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    unsigned callersDrained_ = 0;
    bool callerCapSaid_ = false;
    WriterRecord writer_[kWriterCap];
    std::atomic<bool> ready_[kWriterCap]{};
    std::atomic<uint32_t> writers_{0};
    std::atomic<bool> writerOverflow_{false};
    unsigned writersDrained_ = 0;
    bool writerCapSaid_ = false;
};

struct InstallResult {
    const char* getter = nullptr;   // null: the getter hook is in; else why not
    const char* writer = nullptr;   // the same for the observe-only writer hook (only read when the getter's is in)
};

// The lazy install: nothing is touched until the first frame on which a mono-using key is set (cycle, measure, mono), and then once,
// pass or fail. `gate` returns null or a reason, `install` returns an InstallResult; one line says how it went.
class Lazy {
public:
    template <class Gate, class Install, class Sink>
    void frame(bool wanted, Gate&& gate, Install&& install, Sink&& sink) {
        if (!wanted || attempted_) return;
        attempted_ = true;
        char line[256];
        const char* reason = gate();
        InstallResult result;
        if (!reason) {
            result = install();
            reason = result.getter;
        }
        live_ = reason == nullptr;
        reason_ = reason;
        if (live_) {
            std::snprintf(line, sizeof(line), "cull probe: mono windows multiply the mono camera's aspect by %.2f (hook live)", static_cast<double>(kFactor));
        } else {
            std::snprintf(line, sizeof(line), "cull probe: mono windows multiply the mono camera's aspect by %.2f (hook inert: %s)",
                          static_cast<double>(kFactor), reason);
        }
        sink(line);
        if (live_) {
            writerLive_ = result.writer == nullptr;
            if (writerLive_) std::snprintf(line, sizeof(line), "mono camera: aspect writer observe live (up to %u calls logged)", kWriterCap);
            else std::snprintf(line, sizeof(line), "mono camera: aspect writer observe inert: %s", result.writer);
            sink(line);
        }
    }
    void reset() { *this = Lazy(); }
    bool attempted() const { return attempted_; }
    bool live() const { return live_; }
    bool writerLive() const { return writerLive_; }
    const char* reason() const { return reason_; }

private:
    bool attempted_ = false, live_ = false, writerLive_ = false;
    const char* reason_ = nullptr;
};

// ---- the process-wide state the detours read -------------------------------------------------------------------------------------
inline std::atomic<bool> g_lie{false};              // a mono window is active (native_frame.cpp sets it each frame)
inline std::atomic<uintptr_t> g_fillerReturn{0};    // base + kFillerReturnRva once the hooks are in
inline Observer g_observer;
inline Lazy g_lazy;

// What the getter's detour does with the value the original returned.
inline float onAspectGetter(float original, uintptr_t returnAddress) noexcept {
    g_observer.noteGetterCaller(returnAddress);
    const uintptr_t filler = g_fillerReturn.load(std::memory_order_relaxed);
    if (filler != 0 && returnAddress == filler) cullcycle::noteMonoRead();
    return decide(original, returnAddress, filler, g_lie.load(std::memory_order_relaxed));
}

}  // namespace edvr::monocam
