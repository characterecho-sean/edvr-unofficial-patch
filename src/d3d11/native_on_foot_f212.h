// F212 (native on-foot VR): the conversation look. Header only, no Windows and no game, so native_frame_test runs the very functions the hook uses
// (tools\native_frame_test\f212_checks.h) and native_on_foot_hooks_test runs them against the REAL glue (native_on_foot_hooks.cpp) over a synthetic copy of the leaf.
// Journal: docs\design-native-onfoot-vr-2026-10-13.md, 2026-11-03: F212.
//
// WHAT THE PILOT SAW. At a Mission Giver the commander's look turned toward the NPC by itself (Epic log edvr_gfx_20261102_183150: "Mission Giver" at 18:35:32, the aim
// pitch -3.6 to -12.8 deg in about 1.2 s, then exactly -12.8 for 5 s with the head moving, still held after "OnFoot" until the mouse moved), and the view turned with it.
//
// WHAT TURNS IT (analysis\decomp\verify_20261103_npc_conversation_look.txt, build 334925). The look component comp (constructor 0x19DCC00) keeps two critically damped springs
// (yaw at comp+0x770, pitch at comp+0x78C; velocity +0x10, current +0x14, target +0x18 within each). The look processor 0x1A9EB50 adds dt * the springs' velocities to the
// look delta like a mouse turn. The only writer of the two targets is slot +0xE8 of the component's facade interface (comp+0x58, vtable 0x5206AA8): the 17-byte leaf 0x1A8DF40,
// no .pdata entry, which stores xmm1 at [rcx+0x730] (comp+0x788, the yaw target) and xmm2 at [rcx+0x74C] (comp+0x7A4, the pitch target). ConversationCameraComponent's job
// 0x11FA4A0 calls it at 0x11FA9E5 on every frame of a conversation with the two angle errors toward the NPC (sign and unit unknown). The springs start with target 0, which is
// the game's own "no turn" state, and the processor clears them once they settle on their targets.
//
// WHAT F212 DOES. While EDVR runs, the leaf's first instruction is replaced with a jump (CodeHook; the build gate is the PE identity and all 17 bytes). The callback has the
// leaf's own arguments, void(iface = rcx, yaw = xmm1, pitch = xmm2), and never returns into the leaf: while native on foot is active it stores 0.0f in both targets (not "skips":
// a target left non-zero at the moment native on foot engages would keep the spring turning); otherwise it stores what the game asked, exactly as the leaf did. The look then
// stays wherever the player's head, mouse or stick put it. No config key: it is behaviour. Flat and VR in the ship are unchanged (the callback stores what the game asked).
//
// THE LINES (every one tagged "(F212)"; the frame thread writes them, the game's thread only counts):
//   - the hook's armed or stood-down line (native_on_foot_hooks.cpp, the avatar hooks' own format) at install;
//   - a "run N began" line when gated calls start and a "run N ended" line when none arrived for kRunGapMs, with the run's calls, duration and the largest |yaw| and |pitch|
//     the game asked for (raw floats, unit unknown);
//   - a summary every kWindowMs of a window that had any call or any spring reading above zero: calls gated and passed, and, read in the look processor hook for the
//     LOCAL component only while native on foot is active, the largest |velocity| (comp+0x780, +0x79C) and |target| (comp+0x788, +0x7A4) of the springs.
// If the hook never installs: the stood-down line, or no armed line. If it installs and never fires: no run line and no summary, and --health reads a conversation state
// (F209's SuitUIStateMachine "Mission Giver", "Service Giver" or "StartConversation") with no gated call as NEVER APPLIED.
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace nofoot {
namespace f212 {

// ---- build 334925's address, bytes and offsets ------------------------------------------------------------------------------------------------------------------------
constexpr uintptr_t kLeafRva = 0x1A8DF40;
constexpr size_t kLeafBytes = 17;
// movss [rcx+730h],xmm1 ; movss [rcx+74Ch],xmm2 ; ret. CodeHook steals the first instruction (eight bytes: no rip-relative operand), so the patch is one aligned eight-byte store.
inline constexpr uint8_t kLeaf[kLeafBytes] = {0xF3, 0x0F, 0x11, 0x89, 0x30, 0x07, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0x91, 0x4C, 0x07, 0x00, 0x00, 0xC3};
constexpr uint32_t kOffYawTarget = 0x730, kOffPitchTarget = 0x74C;   // from the facade interface (rcx = comp + 0x58): comp+0x788 and comp+0x7A4
// The springs' fields in the component, read as one block from comp+0x780: yaw velocity +0x780, current +0x784, target +0x788; pitch velocity +0x79C, current +0x7A0, target +0x7A4.
constexpr uint32_t kCompSpringsOff = 0x780, kCompSpringsBytes = 0x28;
constexpr uint32_t kSpringYawVel = 0x00, kSpringYawTarget = 0x08, kSpringPitchVel = 0x1C, kSpringPitchTarget = 0x24;   // within that block
constexpr uint64_t kRunGapMs = 1000;    // a run ends when no gated call has come for this long
constexpr uint64_t kWindowMs = 10000;   // the summary's window

inline const char* tag() noexcept { return "conversation look (F212):"; }

// "f3 0f 11 89 ..." for the install lines (the bytes the hook verified, or the bytes a refusal found). Always terminated; a buffer too small keeps what fits.
inline void hexBytes(char* buf, size_t cap, const uint8_t* p, size_t n) noexcept {
    if (!buf || cap == 0) return;
    size_t at = 0;
    buf[0] = 0;
    for (size_t i = 0; i < n && at + 3 < cap; ++i) at += static_cast<size_t>(std::snprintf(buf + at, cap - at, i ? " %02x" : "%02x", static_cast<unsigned>(p[i])));
}

// ---- the decision ------------------------------------------------------------------------------------------------------------------------------------------------------
// What the callback stores. Bits, not floats, so a NaN (any payload) or a signed zero passes through exactly as the leaf's movss would have stored it.
struct Stores {
    uint32_t yawBits = 0, pitchBits = 0;
    bool gated = false;   // native on foot was active: 0.0f in both
};
inline uint32_t bitsOf(float f) noexcept {
    uint32_t b = 0;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}
inline float floatOf(uint32_t b) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}
inline Stores decide(bool nativeOnFootActive, float yaw, float pitch) noexcept {
    Stores s;
    if (nativeOnFootActive) {
        s.yawBits = bitsOf(0.0f);   // the springs' own "no turn" state; storing it (not skipping) also zeroes a target left from before native on foot engaged
        s.pitchBits = bitsOf(0.0f);
        s.gated = true;
        return s;
    }
    s.yawBits = bitsOf(yaw);
    s.pitchBits = bitsOf(pitch);
    return s;
}

// ---- what the game's thread leaves for the frame thread --------------------------------------------------------------------------------------------------------
// All relaxed except the gated count, which is released after the values it covers so a frame thread that sees the count sees them. Values are the largest |x| (finite only) since
// the frame thread last took them, as the float's bits (non-negative IEEE floats order as their bits).
struct Shared {
    std::atomic<uint64_t> gated{0}, passed{0}, faults{0}, lastGatedMs{0};
    std::atomic<uint64_t> firstGatedMs{0};                                         // the time of the earliest gated call since the frame thread last took (0: none)
    std::atomic<uint32_t> askYaw{0}, askPitch{0};                                  // what the game asked (the leaf's xmm1, xmm2) in gated calls
    std::atomic<uint32_t> velYaw{0}, velPitch{0}, tgtYaw{0}, tgtPitch{0};          // the springs, as the look processor's local call read them while native on foot was active
    std::atomic<uint32_t> reads{0};                                                // ... and how many such reads there were
    void reset() noexcept {
        gated.store(0);
        passed.store(0);
        faults.store(0);
        lastGatedMs.store(0);
        firstGatedMs.store(0);
        askYaw.store(0);
        askPitch.store(0);
        velYaw.store(0);
        velPitch.store(0);
        tgtYaw.store(0);
        tgtPitch.store(0);
        reads.store(0);
    }
};
inline Shared g_shared;

inline void noteMax(std::atomic<uint32_t>& slot, float v) noexcept {
    if (!std::isfinite(v)) return;
    const uint32_t b = bitsOf(std::fabs(v));
    uint32_t cur = slot.load(std::memory_order_relaxed);
    while (b > cur && !slot.compare_exchange_weak(cur, b, std::memory_order_relaxed)) {
    }
}
// The callback's bookkeeping after its store: `storeOk` false counts a fault (the call still counts).
inline void noteCall(Shared& s, const Stores& st, float yaw, float pitch, bool storeOk, uint64_t nowMs) noexcept {
    if (!storeOk) s.faults.fetch_add(1, std::memory_order_relaxed);
    if (st.gated) {
        noteMax(s.askYaw, yaw);
        noteMax(s.askPitch, pitch);
        s.lastGatedMs.store(nowMs, std::memory_order_relaxed);
        uint64_t none = 0;
        if (s.firstGatedMs.load(std::memory_order_relaxed) == 0) s.firstGatedMs.compare_exchange_strong(none, nowMs, std::memory_order_relaxed);
        s.gated.fetch_add(1, std::memory_order_release);
    } else {
        s.passed.fetch_add(1, std::memory_order_relaxed);
    }
}
// The look processor hook's read of the springs (the local component only, while native on foot is active): `block` is kCompSpringsBytes bytes from comp+0x780.
inline void noteSprings(Shared& s, const uint8_t* block) noexcept {
    auto at = [&](uint32_t off) {
        float f = 0.0f;
        std::memcpy(&f, block + off, sizeof(f));
        return f;
    };
    noteMax(s.velYaw, at(kSpringYawVel));
    noteMax(s.velPitch, at(kSpringPitchVel));
    noteMax(s.tgtYaw, at(kSpringYawTarget));
    noteMax(s.tgtPitch, at(kSpringPitchTarget));
    s.reads.fetch_add(1, std::memory_order_relaxed);
}

// ---- the frame thread's reading ---------------------------------------------------------------------------------------------------------------------------------------
struct Snapshot {
    uint64_t gated = 0, passed = 0, faults = 0, lastGatedMs = 0;   // cumulative
    uint64_t firstGatedMs = 0;                                     // the earliest gated call since the previous take (0: none)
    float askYaw = 0.0f, askPitch = 0.0f;                          // the largest |x| since the previous take
    float velYaw = 0.0f, velPitch = 0.0f, tgtYaw = 0.0f, tgtPitch = 0.0f;
    uint32_t reads = 0;
};
inline Snapshot take(Shared& s) noexcept {
    Snapshot o;
    o.gated = s.gated.load(std::memory_order_acquire);
    o.passed = s.passed.load(std::memory_order_relaxed);
    o.faults = s.faults.load(std::memory_order_relaxed);
    o.lastGatedMs = s.lastGatedMs.load(std::memory_order_relaxed);
    o.firstGatedMs = s.firstGatedMs.exchange(0, std::memory_order_relaxed);
    o.askYaw = floatOf(s.askYaw.exchange(0, std::memory_order_relaxed));
    o.askPitch = floatOf(s.askPitch.exchange(0, std::memory_order_relaxed));
    o.velYaw = floatOf(s.velYaw.exchange(0, std::memory_order_relaxed));
    o.velPitch = floatOf(s.velPitch.exchange(0, std::memory_order_relaxed));
    o.tgtYaw = floatOf(s.tgtYaw.exchange(0, std::memory_order_relaxed));
    o.tgtPitch = floatOf(s.tgtPitch.exchange(0, std::memory_order_relaxed));
    o.reads = s.reads.exchange(0, std::memory_order_relaxed);
    return o;
}
inline float maxOf(float a, float b) noexcept { return b > a ? b : a; }

// What one step has to say.
struct Out {
    bool began = false, ended = false, summary = false;
    // began and ended
    uint32_t run = 0;
    uint64_t runCalls = 0;
    double runS = 0.0;
    float runAskYaw = 0.0f, runAskPitch = 0.0f;
    // summary
    double winS = 0.0;
    uint64_t winGated = 0, winPassed = 0, sessGated = 0, sessPassed = 0, sessFaults = 0;
    float winAskYaw = 0.0f, winAskPitch = 0.0f, winVelYaw = 0.0f, winVelPitch = 0.0f, winTgtYaw = 0.0f, winTgtPitch = 0.0f;
    uint32_t winReads = 0;
};

// The runs and the 10 s windows. The frame thread's, once a frame, with a snapshot of the game's thread's counters. A run is the stretch of gated calls with no gap of kRunGapMs
// in it: it begins at the first step that sees the count rise (its start is the earliest gated call since the previous step) and ends at the first step that finds the newest
// gated call older than the gap. A window is kWindowMs from the previous one; its summary is written when it saw any call or any spring above zero.
class Tracker {
public:
    Out step(uint64_t nowMs, const Snapshot& s) noexcept {
        Out o;
        if (!haveWindow_) {
            haveWindow_ = true;
            winStartMs_ = nowMs;
        }
        winAskYaw_ = maxOf(winAskYaw_, s.askYaw);
        winAskPitch_ = maxOf(winAskPitch_, s.askPitch);
        winVelYaw_ = maxOf(winVelYaw_, s.velYaw);
        winVelPitch_ = maxOf(winVelPitch_, s.velPitch);
        winTgtYaw_ = maxOf(winTgtYaw_, s.tgtYaw);
        winTgtPitch_ = maxOf(winTgtPitch_, s.tgtPitch);
        winReads_ += s.reads;
        if (open_) {
            runAskYaw_ = maxOf(runAskYaw_, s.askYaw);
            runAskPitch_ = maxOf(runAskPitch_, s.askPitch);
        }
        if (!open_ && s.gated > seenGated_) {
            open_ = true;
            ++runs_;
            runBase_ = seenGated_;
            startMs_ = s.firstGatedMs != 0 ? s.firstGatedMs : s.lastGatedMs;
            runAskYaw_ = s.askYaw;
            runAskPitch_ = s.askPitch;
            o.began = true;
            o.run = runs_;
            o.runCalls = s.gated - runBase_;
            o.runAskYaw = runAskYaw_;
            o.runAskPitch = runAskPitch_;
        }
        seenGated_ = s.gated;
        if (open_ && nowMs > s.lastGatedMs && nowMs - s.lastGatedMs > kRunGapMs) {
            open_ = false;
            o.ended = true;
            o.run = runs_;
            o.runCalls = s.gated - runBase_;
            o.runS = s.lastGatedMs > startMs_ ? static_cast<double>(s.lastGatedMs - startMs_) * 1e-3 : 0.0;
            o.runAskYaw = runAskYaw_;
            o.runAskPitch = runAskPitch_;
        }
        if (nowMs - winStartMs_ >= kWindowMs) {   // a clock that went back is due once
            const uint64_t g = s.gated - winGated0_, p = s.passed - winPassed0_;
            const bool springs = winVelYaw_ > 0.0f || winVelPitch_ > 0.0f || winTgtYaw_ > 0.0f || winTgtPitch_ > 0.0f;
            if (g + p > 0 || springs) {
                o.summary = true;
                o.winS = nowMs >= winStartMs_ ? static_cast<double>(nowMs - winStartMs_) * 1e-3 : 0.0;
                o.winGated = g;
                o.winPassed = p;
                o.sessGated = s.gated;
                o.sessPassed = s.passed;
                o.sessFaults = s.faults;
                o.winAskYaw = winAskYaw_;
                o.winAskPitch = winAskPitch_;
                o.winVelYaw = winVelYaw_;
                o.winVelPitch = winVelPitch_;
                o.winTgtYaw = winTgtYaw_;
                o.winTgtPitch = winTgtPitch_;
                o.winReads = winReads_;
            }
            winStartMs_ = nowMs;
            winGated0_ = s.gated;
            winPassed0_ = s.passed;
            winAskYaw_ = winAskPitch_ = winVelYaw_ = winVelPitch_ = winTgtYaw_ = winTgtPitch_ = 0.0f;
            winReads_ = 0;
        }
        return o;
    }
    bool open() const noexcept { return open_; }
    uint32_t runs() const noexcept { return runs_; }

private:
    bool open_ = false, haveWindow_ = false;
    uint32_t runs_ = 0, winReads_ = 0;
    uint64_t seenGated_ = 0, runBase_ = 0, startMs_ = 0, winStartMs_ = 0, winGated0_ = 0, winPassed0_ = 0;
    float runAskYaw_ = 0.0f, runAskPitch_ = 0.0f;
    float winAskYaw_ = 0.0f, winAskPitch_ = 0.0f, winVelYaw_ = 0.0f, winVelPitch_ = 0.0f, winTgtYaw_ = 0.0f, winTgtPitch_ = 0.0f;
};

// ---- the lines -------------------------------------------------------------------------------------------------------------------------------------------------------------
inline int formatBegan(char* buf, size_t cap, const Out& o) noexcept {
    return std::snprintf(buf, cap,
                         "%s run %u began: the game is setting the look springs' targets and native on foot is active, so both stores are 0.0 (the largest |yaw| %.5f and |pitch| %.5f "
                         "asked in its first frames; raw floats, unit unknown)",
                         tag(), o.run, static_cast<double>(o.runAskYaw), static_cast<double>(o.runAskPitch));
}
inline int formatEnded(char* buf, size_t cap, const Out& o) noexcept {
    return std::snprintf(buf, cap,
                         "%s run %u ended after %.2f s: %llu gated call(s), the game asked for up to |yaw| %.5f and |pitch| %.5f (raw floats, unit unknown); no gated call for %llu ms",
                         tag(), o.run, o.runS, static_cast<unsigned long long>(o.runCalls), static_cast<double>(o.runAskYaw), static_cast<double>(o.runAskPitch),
                         static_cast<unsigned long long>(kRunGapMs));
}
inline int formatSummary(char* buf, size_t cap, const Out& o) noexcept {
    return std::snprintf(buf, cap,
                         "%s %.1f s: %llu call(s) gated, %llu passed (session %llu gated, %llu passed, %llu store fault(s)); the game asked for up to |yaw| %.5f and |pitch| %.5f; "
                         "look processor, local component, native on foot active (%u read(s)): largest |velocity| yaw %.5f pitch %.5f, largest |target| yaw %.5f pitch %.5f",
                         tag(), o.winS, static_cast<unsigned long long>(o.winGated), static_cast<unsigned long long>(o.winPassed), static_cast<unsigned long long>(o.sessGated),
                         static_cast<unsigned long long>(o.sessPassed), static_cast<unsigned long long>(o.sessFaults), static_cast<double>(o.winAskYaw),
                         static_cast<double>(o.winAskPitch), o.winReads, static_cast<double>(o.winVelYaw), static_cast<double>(o.winVelPitch), static_cast<double>(o.winTgtYaw),
                         static_cast<double>(o.winTgtPitch));
}

}  // namespace f212
}  // namespace nofoot
}  // namespace edvr
