// F129 checks (native on-foot VR): look to aim reads its turn from the player's own look processor. Header only, run by native_frame_test (run129).
// The figures are build 334925's as the RE reads them (analysis\decomp\verify_20261027_onfoot_lift_look.txt): the processor has three exits, runs once per humanoid
// component, and only the player's own component's sink is the update's sink.
#pragma once

#include "../../src/d3d11/native_on_foot_f109.h"
#include "../../src/d3d11/native_on_foot_f129.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace f129test {

namespace f109 = edvr::nofoot::f109;
namespace f129 = edvr::nofoot::f129;

// The gate as the hooks drive it, one frame of the bug: an update that asks, then the processor calls in the order given (comp keys; 0 = unread).
struct Frame {
    f129::Gate* g;
    f129::UpdateEntryOut entry;
    // An update: entry, then the original, then the exit with a readable sink.
    void update(bool asked, uint64_t sink, bool sinkOk = true) {
        entry = g->updateEntry();
        g->updateEnd(true, asked, sinkOk, sink);
    }
};

template <class Check>
inline void run129(Check&& check) {
    const uint32_t S = f129::kSentinelBits;
    {   // the classification: the sentinel intact is a SKIP, A != 0 with O == 0 is path A, anything else is path B
        auto cls = [&](bool pre, bool sent, bool after, uint32_t yaw, uint32_t pitch, uint8_t a, uint8_t o) {
            f129::ClassIn in;
            in.preOk = pre;
            in.sentinelOk = sent;
            in.afterOk = after;
            in.yawBits = yaw;
            in.pitchBits = pitch;
            in.a = a;
            in.o = o;
            return f129::classify(in);
        };
        const float w = 0.4f, nonzero = 0.0123f;
        uint32_t wBits = 0, nBits = 0;
        std::memcpy(&wBits, &w, 4);
        std::memcpy(&nBits, &nonzero, 4);
        const f129::ClassOut skip = cls(true, true, true, S, S, 0, 0);
        const f129::ClassOut skipA = cls(true, true, true, S, S, 1, 0);   // the bytes do not matter when nothing was stored
        const f129::ClassOut pathA = cls(true, true, true, wBits, 0u, 1, 0);
        const f129::ClassOut pathB = cls(true, true, true, nBits, nBits, 1, 1);
        const f129::ClassOut pathB0 = cls(true, true, true, wBits, nBits, 0, 0);
        const f129::ClassOut bPitch0 = cls(true, true, true, nBits, 0u, 1, 1);   // path B can store pitch bits 0: classified by the bytes read BEFORE the call
        const f129::ClassOut partialYaw = cls(true, true, true, S, nBits, 1, 0);
        const f129::ClassOut partialPitch = cls(true, true, true, nBits, S, 0, 0);
        const f129::ClassOut noPre = cls(false, true, true, nBits, nBits, 1, 0);
        const f129::ClassOut noSentinel = cls(true, false, true, S, S, 1, 0);
        const f129::ClassOut noAfter = cls(true, true, false, 0u, 0u, 1, 0);
        check(skip.path == f129::kSkip && skip.restoreYaw && skip.restorePitch && !skip.partial && skipA.path == f129::kSkip && skipA.restoreYaw && skipA.restorePitch,
              "F129 classify: both words still the sentinel is a SKIP (whatever the bytes) and both are put back");
        check(pathA.path == f129::kPathA && !pathA.restoreYaw && !pathA.restorePitch && pathB.path == f129::kPathB && pathB0.path == f129::kPathB && bPitch0.path == f129::kPathB,
              "F129 classify: A != 0 with O == 0 and a stored pair is path A; A != 0 with O != 0, or A == 0, is path B, and a path B pair with pitch bits 0 is not taken for path A");
        check(partialYaw.path == f129::kSkip && partialYaw.partial && partialYaw.restoreYaw && !partialYaw.restorePitch && partialPitch.path == f129::kSkip &&
                  partialPitch.partial && !partialPitch.restoreYaw && partialPitch.restorePitch,
              "F129 classify: one word still the sentinel is a SKIP, counted partial, and only that word is put back");
        check(noPre.path == f129::kUnknown && noSentinel.path == f129::kUnknown && noAfter.path == f129::kUnknown && !noPre.restoreYaw && !noSentinel.restoreYaw &&
                  !noAfter.restoreYaw && f129::pathCarriesAsk(f129::kUnknown) && f129::pathCarriesAsk(f129::kPathB) && !f129::pathCarriesAsk(f129::kPathA) &&
                  !f129::pathCarriesAsk(f129::kSkip),
              "F129 classify: a pre-read, the sentinel write or the read after that failed is UNKNOWN, which carries the ask as path B did before F129, and writes nothing back");
        check(f129::decide(false, f129::kPathB) == f129::kActNone && f129::decide(true, f129::kPathB) == f129::kActSplit && f129::decide(true, f129::kUnknown) == f129::kActSplit &&
                  f129::decide(true, f129::kPathA) == f129::kActNoSplit && f129::decide(true, f129::kSkip) == f129::kActNoSplit,
              "F129 decide: a call that takes nothing does nothing, path B and UNKNOWN run the split, path A and SKIP are blocked calls without one");
    }
    {   // the bug: another comp's call first with a pair of 0, then the player's call that applied the ask
        f129::Gate g;
        Frame f{&g, {}};
        const uint64_t local = 0x7000'1000, remote = 0x7000'2000;
        f.update(true, local);
        const f129::CallOut other = g.call(remote);
        const f129::CallOut mine = g.call(local);
        check(!other.local && other.first && !other.takes && other.firstNotLocalAsk && mine.local && !mine.first && mine.takes && !mine.firstNotLocalAsk && !g.pendValid,
              "F129 gate: a remote commander's call first after the update takes nothing (and is the bug's own signature: the first-call rule would have given our ask to it); "
              "the player's call after it takes the pending ask");
        uint32_t y = 0;
        check(g.firstOursOf(&y) == 0 && y == 1, "F129 gate: the first call after the update is not ours: 0 of 1 updates");
        f.update(true, local);
        const f129::CallOut mine2 = g.call(local);
        const f129::CallOut other2 = g.call(remote);
        check(mine2.local && mine2.first && mine2.takes && !mine2.firstNotLocalAsk && !other2.takes && g.firstOursOf(&y) == 1 && y == 2 && f.entry.otherCalls == 1,
              "F129 gate: the player's call first takes the ask, the remote call after it takes nothing; ours in 1 of 2; the previous update saw one other comp's call");
        const f129::UpdateEntryOut e3 = g.updateEntry();
        check(!e3.unconsumed && e3.processorRan && e3.calls == 2 && e3.otherCalls == 1, "F129 gate: an ask the player's call took is not unconsumed at the next update");
    }
    {   // an update with no local call: the next update counts the ask unconsumed; a call before any update takes nothing; an update that asked nothing leaves nothing
        f129::Gate g;
        const uint64_t local = 0x7000'1000, remote = 0x7000'2000;
        g.updateEntry();
        g.updateEnd(true, true, true, local);
        g.call(remote);
        const f129::UpdateEntryOut e = g.updateEntry();
        check(e.unconsumed && e.processorRan && e.otherCalls == 1 && !g.pendValid, "F129 gate: an update followed by no local call: the next update counts its ask unconsumed (and blocked)");
        g.updateEnd(true, false, true, local);
        const f129::UpdateEntryOut e2 = g.updateEntry();
        check(!e2.unconsumed, "F129 gate: an update that asked nothing is not an unconsumed ask");
        g.updateEnd(true, true, true, local);
        const f129::UpdateEntryOut e3 = g.updateEntry();
        check(e3.unconsumed && !e3.processorRan, "F129 gate: an ask with no processor call at all is unconsumed too (the game's input never reached the processor)");
        const f129::CallOut early = g.call(local);   // a call before the first update of the session: nothing pending
        check(early.local && !early.takes, "F129 gate: a local call with nothing pending takes nothing");
        f129::Gate g2;
        g2.updateEnd(false, false, true, local);   // a refused update: no record, the sink still read
        const f129::CallOut refused = g2.call(local);
        check(refused.local && !refused.takes && !g2.fallback, "F129 gate: a refused update leaves no record, and the player's call is still known by the sink");
    }
    {   // the fallback: 120 updates with processor calls and none of them the player's, then the first call takes the ask as before F129; the player's call re-arms it
        f129::Gate g;
        const uint64_t local = 0x7000'1000, wrong = 0x7000'3000;
        int entered = 0;
        for (int i = 0; i < 120; ++i) {   // 119 finished intervals (the 120th is still open)
            const f129::UpdateEntryOut e = g.updateEntry();
            entered += e.fallbackEntered;
            g.updateEnd(true, true, true, local);
            g.call(wrong);
        }
        check(!g.fallback && entered == 0 && g.noLocalUpdates == 119, "F129 fallback: 119 updates with processor calls and none of them the player's are not yet a fallback");
        const f129::UpdateEntryOut e120 = g.updateEntry();
        check(e120.fallbackEntered && e120.why == f129::kWhyNoLocal && g.fallback && g.fallbackWhy == f129::kWhyNoLocal, "F129 fallback: the 120th update with processor calls and no local one falls back, naming why");
        g.updateEnd(true, true, true, local);
        const f129::CallOut first = g.call(wrong);
        const f129::CallOut second = g.call(wrong);
        check(first.local && first.takes && !second.local && !second.takes && g.fallback,
              "F129 fallback: the first call after the update is the player's and takes the ask, as before F129 (look to aim never has no consuming call)");
        g.updateEntry();
        g.updateEnd(true, true, true, local);
        g.call(wrong);
        const f129::CallOut back = g.call(local);
        check(back.keyMatch && back.local && back.leftFallback && !g.fallback && g.noLocalUpdates == 0 && !back.takes,
              "F129 fallback: a call whose key matches the update's sink re-arms the rule (the first call had taken this update's ask)");
        g.updateEntry();
        g.updateEnd(true, true, true, local);
        g.call(wrong);
        const f129::CallOut gone = g.call(local);
        check(gone.local && gone.takes && !gone.leftFallback, "F129 fallback: re-armed, the player's call takes the ask and another's does not");
        f129::Gate n;
        const f129::UpdateEntryOut s = n.updateEnd(true, true, false, 0);
        const f129::CallOut c = n.call(local);
        check(s.fallbackEntered && s.why == f129::kWhyNoSink && n.fallback && c.local && c.first && c.takes && !c.keyMatch,
              "F129 fallback: an unreadable sink pointer falls back at once, the first call after the update takes the ask");
        uint32_t y = 0;
        check(n.firstOursOf(&y) == 0 && y == 0, "F129 fallback: with the sink unread the first-call history is not fed");
        f129::Gate z;
        z.updateEnd(true, true, true, local);
        const f129::CallOut unreadKey = z.call(0);   // the call's own key could not be read
        check(!unreadKey.local && !unreadKey.takes && z.pendValid, "F129 gate: a call whose key could not be read is not the player's and takes nothing");
        f129::Gate calm;   // updates with no processor call at all never fall back
        for (int i = 0; i < 300; ++i) {
            calm.updateEntry();
            calm.updateEnd(true, false, true, local);
        }
        check(!calm.fallback, "F129 fallback: updates with no processor calls at all are not a key that does not match");
    }
    {   // the first-call history: ours in X of the last Y updates, the streak
        f129::Gate g;
        const uint64_t local = 0x7000'1000, remote = 0x7000'2000;
        for (int i = 0; i < 40; ++i) {
            g.updateEntry();
            g.updateEnd(true, true, true, local);
            if (i % 4 == 0) g.call(remote);
            g.call(local);
        }
        uint32_t y = 0;
        const uint32_t x = g.firstOursOf(&y);
        check(y == 32 && x == 24, "F129 gate: ours in 24 of the last 32 updates when another comp was ahead of it every fourth update");
        check(f129::oursOf(0b1011u, 4, &y) == 3 && y == 4 && f129::oursOf(0xFFFFFFFFu, 99, &y) == 32 && y == 32, "F129 oursOf: counts the newest n bits");
    }
    {   // the order check and the sink's outcome
        check(f129::orderCheck(true, 0.25f, 0.0f, 0.25f, 0.0f) == f129::kOrderOk && f129::orderCheck(true, 0.0f, 0.0f, 0.25f, 0.0f) == f129::kOrderFlipped &&
                  f129::orderCheck(true, 0.2f, 0.1f, 0.25f, 0.1f) == f129::kOrderFlipped && f129::orderCheck(false, 0.25f, 0.0f, 0.25f, 0.0f) == f129::kOrderUnknown,
              "F129 order: the sink's mouse fields before the player's call equal to what the update left: the update ran first; old or zero: flipped; unread: unknown");
        check(f129::sinkAxis(0.001, true, 0.0105f, 0.0105f) == f129::kSinkReached && f129::sinkAxis(0.001, true, 0.0105f, 0.0f) == f129::kSinkZeroed &&
                  f129::sinkAxis(0.001, true, 0.0105f, 0.0042f) == f129::kSinkAltered && f129::sinkAxis(0.0, true, 0.0105f, 0.0f) == f129::kSinkNone &&
                  f129::sinkAxis(0.001, false, 0.0105f, 0.0f) == f129::kSinkNone,
              "F129 sink: an ask that reached the sink (equal), was zeroed there (the game's look block), came out changed, or was not asked or not read");
        check(f129::sinkOutcome(f129::kSinkReached, f129::kSinkZeroed) == f129::kSinkZeroed && f129::sinkOutcome(f129::kSinkNone, f129::kSinkAltered) == f129::kSinkAltered &&
                  f129::sinkOutcome(f129::kSinkReached, f129::kSinkNone) == f129::kSinkReached && f129::sinkOutcome(f129::kSinkNone, f129::kSinkNone) == f129::kSinkNone,
              "F129 sink: a zeroed axis makes the update's outcome zeroed, a changed one altered, else reached when an ask reached it");
    }
    {   // the episodes: three in a row of one kind
        f129::Episode e;
        int edges = 0;
        edges += e.observe(f129::kEvUnconsumed, 1000);
        edges += e.observe(f129::kEvUnconsumed, 1010);
        const bool before = e.active;
        const int third = e.observe(f129::kEvUnconsumed, 1020);
        check(!before && third == +1 && e.active && e.beginMs == 1020 && e.reasons == f129::kRsUnconsumed && edges == 0 && e.unconsumed == 3,
              "F129 episode: three asks in a row that no local call took open an episode, with the three counted");
        e.observe(f129::kEvUnconsumed, 1030);
        const int end = e.observe(f129::kEvConsumed, 1040);
        check(end == -1 && !e.active && e.unconsumed == 4, "F129 episode: an ask the local call took closes it");
        f129::Episode m;   // mixed kinds are not a run of three
        m.observe(f129::kEvUnconsumed, 1);
        m.observe(f129::kEvAorSkip, 2);
        m.observe(f129::kEvUnconsumed, 3);
        m.observe(f129::kEvAorSkip, 4);
        check(!m.active, "F129 episode: alternating kinds are no run of three");
        m.observe(f129::kEvAorSkip, 5);
        const int third2 = m.observe(f129::kEvAorSkip, 6);
        check(third2 == +1 && m.reasons == f129::kRsAorSkip, "F129 episode: three asks in a row that met a local path A or SKIP call open one");
        f129::Episode fn;
        fn.observe(f129::kEvFirstNot, 1);
        fn.observe(f129::kEvFirstNot, 2);
        fn.observe(f129::kEvFirstOurs, 3);
        fn.observe(f129::kEvFirstNot, 4);
        fn.observe(f129::kEvFirstNot, 5);
        const int third3 = fn.observe(f129::kEvFirstNot, 6);
        check(third3 == +1 && fn.reasons == f129::kRsFirstNot && fn.firstNot == 3, "F129 episode: the first call after the update not the player's for three updates in a row opens one; an update with it first resets the run");
        const int end3 = fn.observe(f129::kEvFirstOurs, 7);
        check(end3 == -1, "F129 episode: ... and the player's call being first closes it");
        char buf[300];
        f129::reasonText(f129::kRsUnconsumed | f129::kRsFirstNot, buf, sizeof(buf));
        check(std::string(buf).find("no local call took") != std::string::npos && std::string(buf).find("not the local comp's") != std::string::npos, "F129 episode: the reasons read as text");
    }
    {   // the event lines' budget
        f129::RateCap c;
        int written = 0;
        for (int i = 0; i < 10; ++i) written += c.allow(1000u + static_cast<uint64_t>(i) * 100u, 6) ? 1 : 0;
        const uint32_t held = c.takeSuppressed();
        check(written == 6 && held == 4 && c.takeSuppressed() == 0, "F129 lines: at most 6 event lines a minute, the rest counted and said once");
        check(c.allow(61000u, 6), "F129 lines: the minute rolls");
        f129::RateCap d;
        int comps = 0;
        for (int i = 0; i < 6; ++i) comps += d.allow(1000u + static_cast<uint64_t>(i), 4) ? 1 : 0;
        check(comps == 4 && d.allow(1010u, 6) && d.allow(1011u, 6) && !d.allow(1012u, 6), "F129 lines: the components' line stops at 4 a minute, leaving room under the cap of 6 for an episode");
    }
    {   // the components seen
        f129::CompTable t;
        for (uint64_t i = 1; i <= 3; ++i) t.touch(0x1000u * i, 1000);
        t.touch(0x1000u, 1500);
        check(t.live(1500, 1000) == 3 && t.live(2300, 1000) == 1 && t.live(20000, 10000) == 0, "F129 comps: the components called in the last second, and in the last ten");
        f129::CompTable full;
        for (uint64_t i = 1; i <= 20; ++i) full.touch(0x100u * i, 100u * i);
        check(full.live(2000, 100000) == f129::kCompTableSize, "F129 comps: a full table keeps 16 and replaces the longest unseen");
        full.touch(0x100u * 20, 2100);
        check(full.live(2100, 100000) == f129::kCompTableSize, "F129 comps: a component already in the table is not added twice");
    }
    {   // the ring and its text
        f129::Ring r;
        for (uint32_t i = 1; i <= 40; ++i) {
            f129::Rec x;
            x.kind = (i % 5 == 0) ? f129::kKindUpdate : f129::kKindProc;
            x.seq = i;
            x.id = 0x1000u + i;
            x.sink = 0xAB00u;
            x.path = static_cast<uint8_t>(i % 4);
            x.flags = static_cast<uint8_t>((i % 2) ? f129::kFlLocal | f129::kFlFirst | f129::kFlTook : 0);
            x.A = 1;
            x.O = 0;
            x.D = 0;
            x.M = 1;
            x.a = 0.25f;
            x.b = -0.01f;
            x.sa = 0.0123f;
            x.sb = 0.0f;
            x.sinkState = f129::kSinkReached;
            r.push(x);
        }
        f129::Rec out[16];
        const size_t n = r.read(out, 16);
        check(n == 16 && out[0].seq == 25 && out[15].seq == 40, "F129 ring: the newest 16 of 40 records, oldest first");
        f129::Rec own[8];
        check(r.readOwn(own, 8) == 8 && own[7].seq == 40 && own[0].seq == 33, "F129 ring: the writer's own read agrees");
        f129::Ring few;
        f129::Rec one;
        one.kind = f129::kKindProc;
        one.seq = 7;
        few.push(one);
        check(few.read(out, 16) == 1 && out[0].seq == 7, "F129 ring: fewer records than asked");
        char text[1000];
        const int len = f129::formatRing(text, sizeof(text), out, 0, 16);
        check(len == 0 && text[0] == 0, "F129 ring: no records, no text");
        const size_t k = r.read(out, 16);
        const int len2 = f129::formatRing(text, sizeof(text), out, k, 16);
        const std::string s(text);
        check(len2 > 0 && static_cast<size_t>(len2) < sizeof(text) && s.find("U40") != std::string::npos && s.find("P39") != std::string::npos && s.find(" | ") != std::string::npos &&
                  s.find("1st") != std::string::npos && s.find("TOOK") != std::string::npos,
              "F129 ring: the newest records as text, newest last, in the 1000 bytes the incident line has (older ones are dropped first, with a note)");
        char tiny[200];
        const int len3 = f129::formatRing(tiny, sizeof(tiny), out, k, 16);
        const std::string t3(tiny);
        check(len3 > 0 && len3 < static_cast<int>(sizeof(tiny)) && t3.find("older not shown") != std::string::npos && t3.find("U40") != std::string::npos,
              "F129 ring: a buffer too small for all of them drops the oldest, says how many, and stops before it overflows");
        f129::Rec up;
        up.kind = f129::kKindUpdate;
        up.seq = 5;
        up.id = 0xC0DE;
        up.a = 0.25f;
        up.sa = 0.0f;
        up.sinkState = f129::kSinkZeroed;
        char one160[160];
        f129::formatRec(one160, sizeof(one160), up);
        check(std::string(one160).find("ZEROED") != std::string::npos && std::string(one160).find("ask +0.250") != std::string::npos, "F129 ring: an update's record names the ask and what its sink held");
    }
    {   // the counters
        f129::Counters c;
        c.add(f129::kAsks, 3);
        c.add(f129::kAsks);
        c.maxOther(5);
        c.maxOther(2);
        check(c.get(f129::kAsks) == 4 && c.otherPerUpdateMax.load() == 5 && c.get(f129::kCallsAll) == 0 && c.otherPerUpdateMax.exchange(0) == 5 && c.otherPerUpdateMax.load() == 0,
              "F129 counters: cumulative, the most other comps' calls between two updates is taken with exchange");
        const uint32_t p = f129::packBytes(1, 0, 7, 255);
        check((p & 0xFF) == 1 && ((p >> 8) & 0xFF) == 0 && ((p >> 16) & 0xFF) == 7 && ((p >> 24) & 0xFF) == 255, "F129 counters: the last bytes A O D M pack one each");
    }
    {   // the zero-sum probes: +p, then -p; leaving blocked restarts; the radius is 0.25 deg
        f109::ProbePair pp;
        double ay = 0.0, ap = 0.0, by = 0.0, bp = 0.0;
        pp.next(2.0 * f109::kDeg, -1.0 * f109::kDeg, &ay, &ap);
        pp.next(5.0 * f109::kDeg, 5.0 * f109::kDeg, &by, &bp);   // what is left meanwhile does not change the undo
        check(std::fabs(f109::kProbeRad - 0.25 * f109::kDeg) < 1e-15 && std::fabs(ay - 0.25 * f109::kDeg) < 1e-12 && std::fabs(ap + 0.25 * f109::kDeg) < 1e-12 &&
                  ay + by == 0.0 && ap + bp == 0.0,
              "F129 probes: an odd probe asks what is left up to 0.25 deg each axis, the next takes it back exactly (two probes sum to zero)");
        double cy = 0.0, cp = 0.0, dy = 0.0, dp = 0.0;
        pp.next(0.1 * f109::kDeg, 0.0, &cy, &cp);
        pp.reset();   // the game's look came back, or went away: the phase restarts
        pp.next(0.1 * f109::kDeg, 0.0, &dy, &dp);
        check(std::fabs(cy - 0.1 * f109::kDeg) < 1e-12 && std::fabs(dy - 0.1 * f109::kDeg) < 1e-12 && pp.undo, "F129 probes: leaving the blocked state restarts at an odd probe, never an undo of nothing");
        double sumY = 0.0, sumP = 0.0;
        f109::ProbePair q;
        for (int i = 0; i < 20; ++i) {
            double y = 0.0, p = 0.0;
            q.next(0.7 * f109::kDeg * ((i % 3) - 1), 0.3 * f109::kDeg, &y, &p);
            sumY += y;
            sumP += p;
        }
        check(std::fabs(sumY) < 1e-12 && std::fabs(sumP) < 1e-12, "F129 probes: any even number of probes moves the aim by nothing, whatever was left in between");
        double zy = 1.0, zp = 1.0;
        f109::ProbePair z;
        z.next(0.0, 0.0, &zy, &zp);
        check(zy == 0.0 && zp == 0.0 && !std::signbit(zy), "F129 probes: nothing left to ask is a probe of exactly zero");
    }
}

}  // namespace f129test
