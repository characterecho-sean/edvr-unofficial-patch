// F212 checks (native on-foot VR): the conversation look. Header only, run by native_frame_test (run212). The pure half only: the leaf's bytes and offsets against the RE note, the
// callback's decision (gated stores 0, passed stores the inputs bit for bit), the game thread's counters, the look processor's reads of the springs, the runs and the 10 s windows
// (native_on_foot_f212.h Tracker), the lines, and the wiring read as text. The real glue (the hook over a synthetic copy of the 17-byte leaf at its real RVA, the callback called
// with xmm1 and xmm2, the frame thread's lines, a wrong byte, a build that differs) is run by native_on_foot_hooks_test.
//
// The figures are the flight's (Epic log edvr_gfx_20261102_183150): "Mission Giver" at 18:35:32, the aim pitch -3.6 to -12.8 deg in about 1.2 s, then held at exactly -12.8 for 5 s.
#pragma once

#include "../../src/d3d11/native_on_foot_f212.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace f212test {

namespace f212 = edvr::nofoot::f212;

inline std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());   // a checkout with CRLF reads as LF
    return s;
}
inline size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}
inline bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

// The facade interface the way the leaf sees it: rcx, with the two targets at +0x730 and +0x74C.
struct Facade {
    uint8_t b[0x800] = {};
    uint32_t yaw() const { uint32_t v; std::memcpy(&v, b + f212::kOffYawTarget, 4); return v; }
    uint32_t pitch() const { uint32_t v; std::memcpy(&v, b + f212::kOffPitchTarget, 4); return v; }
    void set(uint32_t y, uint32_t p) {
        std::memcpy(b + f212::kOffYawTarget, &y, 4);
        std::memcpy(b + f212::kOffPitchTarget, &p, 4);
    }
};
// What the 17-byte leaf does: the two movss stores.
inline void leaf(Facade& f, float yaw, float pitch) { f.set(f212::bitsOf(yaw), f212::bitsOf(pitch)); }
// What the replacement does.
inline void replacement(Facade& f, bool active, float yaw, float pitch) {
    const f212::Stores s = f212::decide(active, yaw, pitch);
    f.set(s.yawBits, s.pitchBits);
}

// A springs block as the look processor hook reads it from comp+0x780.
struct Springs {
    uint8_t b[f212::kCompSpringsBytes] = {};
    void put(uint32_t off, float v) { std::memcpy(b + off, &v, 4); }
};

template <class Check>
inline void run212(Check&& check) {
    // ---- 1. the constants: build 334925's address, bytes and offsets (the RE note's, read from the exe) ---------------------------------------------------------------------------
    {
        check(f212::kLeafRva == 0x1A8DF40 && f212::kLeafBytes == 17, "F212: the leaf is build 334925's 0x1A8DF40, 17 bytes");
        const uint8_t want[17] = {0xF3, 0x0F, 0x11, 0x89, 0x30, 0x07, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0x91, 0x4C, 0x07, 0x00, 0x00, 0xC3};
        check(std::memcmp(f212::kLeaf, want, 17) == 0, "F212: its bytes are `f3 0f 11 89 30 07 00 00 f3 0f 11 91 4c 07 00 00 c3` (movss [rcx+730h],xmm1; movss [rcx+74Ch],xmm2; ret)");
        uint32_t d1 = 0, d2 = 0;
        std::memcpy(&d1, f212::kLeaf + 4, 4);
        std::memcpy(&d2, f212::kLeaf + 12, 4);
        check(d1 == f212::kOffYawTarget && d2 == f212::kOffPitchTarget && f212::kOffYawTarget == 0x730 && f212::kOffPitchTarget == 0x74C,
              "F212: the offsets in the code are the displacements in the bytes (0x730 and 0x74C)");
        check(f212::kOffYawTarget + 0x58 == 0x788 && f212::kOffPitchTarget + 0x58 == 0x7A4,
              "F212: from the facade (comp + 0x58) they are the component's yaw target comp+0x788 and pitch target comp+0x7A4");
        check(f212::kCompSpringsOff == 0x780 && f212::kCompSpringsOff + f212::kSpringYawVel == 0x780 && f212::kCompSpringsOff + f212::kSpringYawTarget == 0x788 &&
                  f212::kCompSpringsOff + f212::kSpringPitchVel == 0x79C && f212::kCompSpringsOff + f212::kSpringPitchTarget == 0x7A4 &&
                  f212::kSpringPitchTarget + 4 <= f212::kCompSpringsBytes,
              "F212: the processor's read of the springs covers the velocities (comp+0x780, +0x79C) and the targets (+0x788, +0x7A4) in one block");
        check(f212::kRunGapMs == 1000 && f212::kWindowMs == 10000, "F212: a run ends after 1 s without a gated call; the summary's window is 10 s");
        char hex[80];
        f212::hexBytes(hex, sizeof(hex), f212::kLeaf, f212::kLeafBytes);
        check(std::strcmp(hex, "f3 0f 11 89 30 07 00 00 f3 0f 11 91 4c 07 00 00 c3") == 0, "F212: the install line spells the 17 bytes");
        char tiny[10];
        f212::hexBytes(tiny, sizeof(tiny), f212::kLeaf, f212::kLeafBytes);
        check(std::strlen(tiny) < sizeof(tiny) && std::strncmp(tiny, "f3 0f 11", 8) == 0, "F212: the spelling into a small buffer is cut and terminated");
    }
    // ---- 2. the decision ---------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        // Gated: native on foot active. Both stores are +0.0f whatever the game asked.
        const float asks[][2] = {{0.25f, -0.2231f}, {-3.6f, -12.8f}, {0.0f, 0.0f}, {nan, nan}, {inf, -inf}, {1e30f, -1e-30f}, {-0.0f, 0.0f}};
        bool gatedOk = true;
        for (const auto& a : asks) {
            const f212::Stores s = f212::decide(true, a[0], a[1]);
            gatedOk = gatedOk && s.gated && s.yawBits == 0u && s.pitchBits == 0u;
        }
        check(gatedOk, "F212: native on foot active: both stores are +0.0 (bits 0) whatever the game asked, NaN, infinity and -0.0 included, and the call is gated");
        // Passed: the inputs, bit for bit.
        const uint32_t patterns[] = {0x3E800000u, 0xBE64E3A4u, 0x00000000u, 0x80000000u, 0x00000001u, 0x007FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7FC00001u, 0x7F800001u, 0xFFFFFFFFu,
                                     0xFFC12345u, 0x7F7FFFFFu};
        bool passOk = true;
        for (const uint32_t y : patterns)
            for (const uint32_t p : patterns) {
                const f212::Stores s = f212::decide(false, f212::floatOf(y), f212::floatOf(p));
                passOk = passOk && !s.gated && s.yawBits == y && s.pitchBits == p;
            }
        check(passOk, "F212: native on foot not active: both stores are the inputs bit for bit (every pair of 14 patterns: -0.0, denormals, infinities, quiet and signalling NaNs with payloads)");
        const f212::Stores nanPass = f212::decide(false, nan, -nan);
        check(!nanPass.gated && std::isnan(f212::floatOf(nanPass.yawBits)) && std::isnan(f212::floatOf(nanPass.pitchBits)) && nanPass.yawBits == f212::bitsOf(nan) &&
                  nanPass.pitchBits == f212::bitsOf(-nan),
              "F212: NaN inputs pass through unchanged when not gated");
        // The leaf and the replacement, on a facade: not active, identical; active, zero.
        Facade a, b;
        bool same = true;
        for (const uint32_t y : patterns)
            for (const uint32_t p : patterns) {
                leaf(a, f212::floatOf(y), f212::floatOf(p));
                replacement(b, false, f212::floatOf(y), f212::floatOf(p));
                same = same && std::memcmp(a.b, b.b, sizeof(a.b)) == 0;
            }
        check(same, "F212: not active, the replacement leaves the facade byte for byte as the leaf would (every pair of the patterns; nothing else in the object is touched)");
        Facade c;
        std::memset(c.b, 0xA5, sizeof(c.b));
        replacement(c, true, 0.5f, -0.5f);
        bool others = true;
        for (size_t i = 0; i < sizeof(c.b); ++i)
            if ((i < f212::kOffYawTarget || i >= f212::kOffYawTarget + 4) && (i < f212::kOffPitchTarget || i >= f212::kOffPitchTarget + 4)) others = others && c.b[i] == 0xA5;
        check(c.yaw() == 0u && c.pitch() == 0u && others, "F212: active, the replacement writes exactly the two targets, as zeros, and no other byte of the object");
        // Storing zero rather than skipping: a target left non-zero when native on foot engages is cleared by the next call.
        Facade d;
        replacement(d, false, 0.3f, -0.1f);
        const bool left = d.yaw() == f212::bitsOf(0.3f) && d.pitch() == f212::bitsOf(-0.1f);
        replacement(d, true, 0.2f, -0.05f);
        check(left && d.yaw() == 0u && d.pitch() == 0u, "F212: a target left non-zero by a passed call is zeroed by the first gated call (a skip would keep the spring turning)");
        replacement(d, false, 0.1f, 0.2f);
        check(d.yaw() == f212::bitsOf(0.1f) && d.pitch() == f212::bitsOf(0.2f), "F212: and when native on foot ends the next call stores what the game asks again");
    }
    // ---- 3. the game thread's counters -----------------------------------------------------------------------------------------------------------------------------------------
    {
        f212::Shared sh;
        f212::noteCall(sh, f212::decide(true, 0.2f, -0.1f), 0.2f, -0.1f, true, 5000);
        f212::noteCall(sh, f212::decide(true, -0.6f, 0.05f), -0.6f, 0.05f, true, 5016);
        f212::noteCall(sh, f212::decide(false, 9.0f, 9.0f), 9.0f, 9.0f, true, 5032);
        f212::noteCall(sh, f212::decide(true, 0.1f, -0.9f), 0.1f, -0.9f, false, 5048);
        check(sh.gated.load() == 3 && sh.passed.load() == 1 && sh.faults.load() == 1 && sh.lastGatedMs.load() == 5048, "F212: three gated calls, one passed, one store fault (the call still counts), the newest gated call's time");
        check(f212::floatOf(sh.askYaw.load()) == 0.6f && f212::floatOf(sh.askPitch.load()) == 0.9f,
              "F212: the largest |yaw| and |pitch| the game asked are kept from gated calls only (a passed call's 9.0 is not one), as magnitudes");
        f212::Shared nn;
        const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        f212::noteCall(nn, f212::decide(true, nan, inf), nan, inf, true, 100);
        f212::noteCall(nn, f212::decide(true, 0.3f, -inf), 0.3f, -inf, true, 116);
        check(nn.gated.load() == 2 && f212::floatOf(nn.askYaw.load()) == 0.3f && nn.askPitch.load() == 0u, "F212: a NaN or an infinity asked is counted as a call but never becomes the maximum");
        const f212::Snapshot s1 = f212::take(sh);
        check(s1.gated == 3 && s1.passed == 1 && s1.faults == 1 && s1.lastGatedMs == 5048 && s1.askYaw == 0.6f && s1.askPitch == 0.9f, "F212: the frame thread's take reads the counters and the maxima");
        const f212::Snapshot s2 = f212::take(sh);
        check(s2.gated == 3 && s2.askYaw == 0.0f && s2.askPitch == 0.0f && s2.reads == 0, "F212: the maxima are taken (reset); the counters are cumulative");
        // the look processor's reads of the springs
        Springs sp;
        sp.put(f212::kSpringYawVel, -0.75f);
        sp.put(f212::kSpringYawTarget, 0.4f);
        sp.put(f212::kSpringPitchVel, 0.125f);
        sp.put(f212::kSpringPitchTarget, -0.2f);
        f212::noteSprings(sh, sp.b);
        Springs sp2;
        sp2.put(f212::kSpringYawVel, 0.5f);
        sp2.put(f212::kSpringPitchTarget, std::numeric_limits<float>::quiet_NaN());
        f212::noteSprings(sh, sp2.b);
        const f212::Snapshot s3 = f212::take(sh);
        check(s3.reads == 2 && s3.velYaw == 0.75f && s3.velPitch == 0.125f && s3.tgtYaw == 0.4f && s3.tgtPitch == 0.2f,
              "F212: the processor's reads keep the largest |velocity| and |target| of both springs (the magnitude of -0.75, a NaN ignored), and count the reads");
        Springs zero;
        f212::noteSprings(sh, zero.b);
        const f212::Snapshot s4 = f212::take(sh);
        check(s4.reads == 1 && s4.velYaw == 0.0f && s4.velPitch == 0.0f && s4.tgtYaw == 0.0f && s4.tgtPitch == 0.0f, "F212: springs at rest read exactly 0.0 (what the gate leaves)");
        sh.reset();
        check(sh.gated.load() == 0 && sh.passed.load() == 0 && sh.lastGatedMs.load() == 0 && sh.reads.load() == 0, "F212: reset clears the counters");
    }
    // ---- 4. the runs ---------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        // A conversation as the flight had it: gated calls every 11 ms for 6 s, then none.
        f212::Tracker t;
        f212::Shared sh;
        int began = 0, ended = 0, summaries = 0;
        uint64_t lastCall = 0;
        f212::Out endedOut;
        auto frame = [&](uint64_t at, bool call, float y = 0.0f, float p = 0.0f) {
            if (call) {
                f212::noteCall(sh, f212::decide(true, y, p), y, p, true, at);
                lastCall = at;
            }
            const f212::Out o = t.step(at, f212::take(sh));
            if (o.began) ++began;
            if (o.ended) {
                ++ended;
                endedOut = o;
            }
            if (o.summary) ++summaries;
            return o;
        };
        frame(100000, false);   // the tracker's first frame, nothing yet
        check(!t.open() && t.runs() == 0 && began == 0, "F212: no gated call, no run");
        for (int i = 0; i < 5; ++i) frame(100000 + 11u * static_cast<uint64_t>(i + 1), false);
        check(began == 0 && ended == 0 && summaries == 0, "F212: frames with no call say nothing (not a line in a quiet window)");
        const f212::Out first = frame(100100, true, 0.05f, -0.0646f);
        check(first.began && !first.ended && t.open() && first.run == 1 && first.runCalls == 1 && first.runAskYaw == 0.05f && first.runAskPitch == 0.0646f,
              "F212: the first gated call begins run 1 at the next frame, with the largest asked so far");
        for (uint64_t at = 100111; at <= 106000; at += 11) frame(at, true, 0.05f * (1.0f - static_cast<float>(at - 100100) / 6000.0f), -0.0646f);
        check(began == 1 && ended == 0 && t.open(), "F212: 6 s of calls every 11 ms are one run, with no further line");
        // the calls stop; the run ends the first frame more than 1 s after the newest gated call
        uint64_t endedAt = 0;
        for (uint64_t at = lastCall + 11; at <= lastCall + 1100; at += 11) {
            const f212::Out o = frame(at, false);
            if (o.ended && !endedAt) endedAt = at;
        }
        check(ended == 1 && !t.open() && endedAt != 0, "F212: the run ends once");
        check(endedAt > lastCall + f212::kRunGapMs && endedAt <= lastCall + f212::kRunGapMs + 11, "F212: ...at the first frame more than 1 s after the newest gated call, not before");
        check(endedOut.run == 1 && endedOut.runS > 5.8 && endedOut.runS < 6.0 && endedOut.runCalls > 500 && endedOut.runCalls < 560 && endedOut.runAskYaw == 0.05f && endedOut.runAskPitch == 0.0646f,
              "F212: its line carries the run's duration (about 5.9 s), the calls (one in each of about 540 frames) and the largest |yaw| and |pitch| the game asked");
        // a second conversation
        const f212::Out again = frame(120000, true, 0.4f, 0.1f);
        check(again.began && again.run == 2 && t.open(), "F212: the next conversation is run 2");
        // a gap shorter than the limit does not split a run
        f212::Tracker u;
        f212::Shared sh2;
        auto frame2 = [&](uint64_t at, bool call) {
            if (call) f212::noteCall(sh2, f212::decide(true, 0.1f, 0.1f), 0.1f, 0.1f, true, at);
            return u.step(at, f212::take(sh2));
        };
        frame2(1000, true);
        frame2(1500, false);
        frame2(1990, false);
        const f212::Out resumed = frame2(2000, true);
        check(u.open() && u.runs() == 1 && !resumed.began && !resumed.ended, "F212: a gap under 1 s inside a conversation is one run");
        frame2(2500, false);
        const f212::Out later = frame2(3000, false);
        check(!later.ended && u.open(), "F212: exactly 1 s after the newest call does not end the run");
        const f212::Out over = frame2(3001, false);
        check(over.ended && over.runCalls == 2 && over.runS > 0.99 && over.runS < 1.01, "F212: more than 1 s does: 2 calls over 1.0 s");
        // a frame thread that stalled past the gap sees a whole run at once: began and ended in the one step, the duration from the earliest call
        f212::Tracker w;
        f212::Shared sh4;
        w.step(4000, f212::take(sh4));
        f212::noteCall(sh4, f212::decide(true, 0.2f, 0.2f), 0.2f, 0.2f, true, 5000);
        f212::noteCall(sh4, f212::decide(true, 0.3f, 0.1f), 0.3f, 0.1f, true, 5100);
        const f212::Out both = w.step(9000, f212::take(sh4));
        check(both.began && both.ended && both.run == 1 && both.runCalls == 2 && both.runS > 0.09 && both.runS < 0.11 && !w.open(),
              "F212: a frame thread that stalled past the gap sees a whole run at once: began and ended in the one step, 2 calls over 0.1 s");
        // native on foot stops: the calls become passed calls; the run ends 1 s later and the passed calls are not gated
        f212::Tracker x;
        f212::Shared sh5;
        auto frame3 = [&](uint64_t at, bool active) {
            f212::noteCall(sh5, f212::decide(active, 0.2f, 0.1f), 0.2f, 0.1f, true, at);
            return x.step(at, f212::take(sh5));
        };
        frame3(1000, true);
        uint64_t passedEnd = 0;
        for (uint64_t at = 1011; at < 3000; at += 11) {
            const f212::Out o = frame3(at, false);
            if (o.ended && !passedEnd) passedEnd = at;
        }
        check(passedEnd > 2000 && passedEnd <= 2011 && sh5.passed.load() > 150 && sh5.gated.load() == 1, "F212: calls that pass (native on foot no longer active) end the run 1 s after the last gated one");
    }
    // ---- 5. the windows --------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f212::Tracker t;
        f212::Shared sh;
        auto step = [&](uint64_t at) { return t.step(at, f212::take(sh)); };
        step(1000);
        check(!step(5000).summary && !step(10999).summary, "F212: a quiet window writes no summary");
        const f212::Out quiet = step(11000);
        check(!quiet.summary, "F212: ...not even at 10 s");
        // a window with gated calls and springs read
        Springs rest;
        for (uint64_t at = 11000; at < 21000; at += 100) {
            f212::noteCall(sh, f212::decide(true, 0.2f, -0.1f), 0.2f, -0.1f, true, at);
            f212::noteSprings(sh, rest.b);
            step(at + 1);
        }
        const f212::Out w = step(21000);
        check(w.summary && w.winGated == 100 && w.winPassed == 0 && w.sessGated == 100 && w.sessPassed == 0 && w.sessFaults == 0 && w.winReads == 100 && w.winAskYaw == 0.2f && w.winAskPitch == 0.1f &&
                  w.winVelYaw == 0.0f && w.winTgtPitch == 0.0f && w.winS > 9.9 && w.winS < 10.1,
              "F212: a window with 100 gated calls and springs read at rest: the summary says 100 gated, 0 passed, 100 reads, springs exactly 0, 10 s");
        check(!step(21001).summary, "F212: and the next window starts from nothing");
        // springs above zero without any call (another writer, or the hook missing): the summary says so
        Springs moving;
        moving.put(f212::kSpringYawVel, 0.3f);
        moving.put(f212::kSpringPitchTarget, -0.12f);
        f212::noteSprings(sh, moving.b);
        step(25000);
        const f212::Out s2 = step(31001);
        check(s2.summary && s2.winGated == 0 && s2.winPassed == 0 && s2.winVelYaw == 0.3f && s2.winTgtPitch == 0.12f && s2.winReads == 1 && s2.sessGated == 100,
              "F212: a window with no call but a spring above zero is written: the springs moved and the hook did not fire (the evidence the look turned by another route)");
        // passed calls are a window's activity, and faults are the session's
        f212::noteCall(sh, f212::decide(false, 0.2f, 0.2f), 0.2f, 0.2f, false, 32000);
        const f212::Out s3 = step(41002);
        check(s3.summary && s3.winGated == 0 && s3.winPassed == 1 && s3.sessPassed == 1 && s3.sessFaults == 1, "F212: a passed call makes a window, and the session's faults are in its line");
        // a clock that went back is due once
        f212::noteCall(sh, f212::decide(true, 0.2f, 0.2f), 0.2f, 0.2f, true, 100);
        const f212::Out s4 = step(100);
        check(s4.summary && s4.winGated == 1, "F212: a clock that went back closes the window once");
    }
    // ---- 6. the lines ----------------------------------------------------------------------------------------------------------------------------------------------------------------
    {
        f212::Out o;
        o.run = 3;
        o.runCalls = 541;
        o.runS = 5.93;
        o.runAskYaw = 0.0123f;
        o.runAskPitch = 0.2236f;
        o.winS = 10.0;
        o.winGated = 449;
        o.winPassed = 2;
        o.sessGated = 1030;
        o.sessPassed = 9;
        o.sessFaults = 0;
        o.winAskYaw = 0.0123f;
        o.winAskPitch = 0.2236f;
        o.winReads = 450;
        o.winVelYaw = 0.0f;
        o.winVelPitch = 0.0f;
        o.winTgtYaw = 0.0f;
        o.winTgtPitch = 0.0f;
        char b[640], e[640], s[800];
        f212::formatBegan(b, sizeof(b), o);
        f212::formatEnded(e, sizeof(e), o);
        f212::formatSummary(s, sizeof(s), o);
        const std::string began = b, ended = e, summary = s;
        check(began.rfind("conversation look (F212): run 3 began:", 0) == 0 && has(began, "both stores are 0.0") && has(began, "|yaw| 0.01230") && has(began, "|pitch| 0.22360"),
              "F212: the began line names the run, says both stores are 0.0, and gives the largest asked so far");
        check(ended.rfind("conversation look (F212): run 3 ended after 5.93 s: 541 gated call(s)", 0) == 0 && has(ended, "up to |yaw| 0.01230 and |pitch| 0.22360") && has(ended, "(raw floats, unit unknown)") &&
                  has(ended, "no gated call for 1000 ms"),
              "F212: the ended line gives the duration, the calls, the largest |yaw| and |pitch| (raw floats, unit unknown) and the gap");
        check(summary.rfind("conversation look (F212): 10.0 s: 449 call(s) gated, 2 passed (session 1030 gated, 9 passed, 0 store fault(s))", 0) == 0 && has(summary, "(450 read(s))") &&
                  has(summary, "largest |velocity| yaw 0.00000 pitch 0.00000, largest |target| yaw 0.00000 pitch 0.00000"),
              "F212: the summary gives the window's gated and passed calls, the session's, the reads and the springs' largest velocity and target");
        check(std::strlen(b) < 400 && std::strlen(e) < 400 && std::strlen(s) < 560, "F212: the lines are short");
        // distinct lines: none repeats another's start
        check(began.substr(0, 40) != ended.substr(0, 40) && ended.substr(0, 40) != summary.substr(0, 40) && began.substr(0, 40) != summary.substr(0, 40), "F212: the three lines start differently");
    }
    // ---- 7. the wiring, read as text -------------------------------------------------------------------------------------------------------------------------------------------------
    {
        const std::string hooks = slurp("src/d3d11/native_on_foot_hooks.cpp"), cpp = slurp("src/d3d11/native_on_foot.cpp"), hs = slurp("tools/hook_signatures.py");
        check(countOf(hooks, "nofoot::f212::kLeafRva") == 1 && countOf(hooks, "nofoot::f212::kLeaf, nofoot::f212::kLeafBytes}") == 1 &&
                  countOf(hooks, "case kHkLook: return reinterpret_cast<const void*>(&conversationLookHooked)") == 1,
              "F212: the hook is in the hook table with the leaf's address, its 17 bytes and its callback");
        check(hooks.find("void __fastcall conversationLookHooked(void* iface, float yaw, float pitch) noexcept") != std::string::npos,
              "F212: the callback has the leaf's arguments under the x64 ABI: rcx = the facade, xmm1 = yaw, xmm2 = pitch");
        const size_t at = hooks.find("// ---- F212: the conversation look (native_on_foot_f212.h");
        const size_t end = hooks.find("// ---- F209: the Pioneer Supplies buy screens", at);
        const std::string block = at != std::string::npos && end != std::string::npos ? hooks.substr(at, end - at) : std::string();
        check(!block.empty() && block.find("g_forward") == std::string::npos && block.find("forward(") == std::string::npos && block.find("ForwardFn") == std::string::npos &&
                  block.find("VirtualProtect") == std::string::npos && block.find("WriteProcessMemory") == std::string::npos && block.find("Log::get()") == std::string::npos &&
                  block.find("= new ") == std::string::npos && block.find("std::vector") == std::string::npos && block.find("std::string") == std::string::npos &&
                  block.find("malloc(") == std::string::npos && block.find("lock_guard") == std::string::npos && block.find("std::mutex") == std::string::npos &&
                  countOf(block, "nativeOnFootActive()") == 1 && countOf(block, "f212::decide(") == 1 && countOf(block, "f212::noteCall(") == 1,
              "F212: the callback never calls the trampoline (it does the leaf's work itself), takes no lock, allocates nothing, logs nothing, and decides by nativeOnFootActive() once");
        check(hooks.find("convLookTick(sink);") != std::string::npos && hooks.find("convLookTick(sink);") < hooks.find("    updateGates();\n}\navh::Sink g_frameSink") &&
                  hooks.find("g_gate[kHkLook].store(armed(kHkLook) && capture ? 1u : 0u") != std::string::npos && hooks.find("tryHook(kHkLook, sink)") != std::string::npos,
              "F212: the frame boundary tries the hook while the native key is on, writes the run and summary lines every frame, and opens the gate with the key");
        check(cpp.find("if (nativeOnFootActive()) f212SampleSprings(c);") != std::string::npos && cpp.find("if (nativeOnFootActive()) f212SampleSprings(c);") > cpp.find("if (co.local) {\n        g_f129Ctr.add(cl.path"),
              "F212: the look processor hook reads the springs for the local component only (inside F129's co.local), only while native on foot is active");
        check(hs.find("(\"d3d11/native_on_foot_f212.h\"") != std::string::npos, "F212: the header joins tools\\hook_signatures.py's exact-build audit list (the maintainer reviews it before a merge)");
        // no config key: the behaviour is not a setting
        const std::string ini = slurp("edvr.ini");
        check(!ini.empty() && ini.find("conversation_look") == std::string::npos && ini.find("f212") == std::string::npos && ini.find("F212") == std::string::npos,
              "F212: there is no config key for it");
    }
}

}  // namespace f212test
