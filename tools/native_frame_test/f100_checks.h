// F100 checks (native on-foot VR): the performance round's pure parts. Header only, run by native_frame_test (run100).
// The marker tee's prefilter is checked against a model of the tee's slow path: the same draws, through the slow path always and through the prefilter,
// must leave the same draw serial after every draw, the same decisions (frame, draw, serial), the same binds, the same world rows and the same counts.
#pragma once

#include "../../src/d3d11/native_on_foot_core.h"
#include "../../src/d3d11/native_on_foot_f100.h"

#include <cstdint>
#include <vector>

namespace f100test {

namespace f100 = edvr::nofoot::f100;
namespace nf = edvr::nofoot;

// A deterministic draw stream: kind and randomness from (frame, index), never from a sequence a skipped draw could shift.
inline uint32_t mix(uint32_t a, uint32_t b, uint32_t salt) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ salt * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}
enum Kind { kHud, kMarker, kWorld0, kWorld1, kOther };
inline Kind kindOf(uint32_t frame, uint32_t i) {
    const uint32_t r = mix(frame, i, 1) % 100u;
    return r < 2 ? kHud : r < 6 ? kMarker : r < 30 ? kWorld0 : r < 54 ? kWorld1 : kOther;
}
inline uint64_t vsOf(Kind k, uint32_t frame, uint32_t i) {
    if (k == kHud) return nf::kHudIssuerVs[mix(frame, i, 2) & 1u];
    if (k == kMarker) return nf::kMarkerCompositeVs;
    return 0x1000ull + mix(frame, i, 3);   // a world or other draw's VS: anything but the three above
}

struct Decision {
    uint32_t frame, draw;
    uint64_t serial;
    bool operator==(const Decision& o) const { return frame == o.frame && draw == o.draw && serial == o.serial; }
};
// The slow path's model: what nativeOnFootMarkerProbeDraw and f44::draw do with these draws, and the fast frame it leaves.
struct Model {
    f100::TeeState st;
    uint64_t serial = 0, decisionSerial = 0, notIssued = 0, lateRestores = 0, hud = 0, censusNotes = 0, slowCalls = 0, fastCalls = 0;
    std::vector<Decision> decisions, binds;
    std::vector<Decision> worldRows;
    bool censusFrame(uint32_t frame) const { return frame % 45u < 2u; }   // two census frames a "second"
    template <class Eligible>
    uint32_t slow(uint32_t frame, uint32_t i, Kind k, Eligible&& eligible) {
        ++slowCalls;
        ++serial;
        if (st.pending) ++notIssued;
        st.pending = false;
        if (st.bound) {
            ++lateRestores;
            st.bound = false;
        }
        if (frame != st.polledFrame) {
            st.polledFrame = frame;
            st.census = censusFrame(frame);
        }
        if (k == kHud) {
            ++hud;
        } else if (k == kMarker) {
            if (st.worldOk[0] == frame && st.worldOk[1] == frame && mix(frame, i, 4) % 3u != 0u) {
                st.pending = true;
                decisionSerial = serial;
                decisions.push_back({frame, i, serial});
            }
        } else if (!(!st.census && st.worldOk[0] == frame && st.worldOk[1] == frame)) {
            if (st.census) ++censusNotes;
            const int e = k == kWorld0 ? 0 : k == kWorld1 ? 1 : -1;
            if (e >= 0 && st.worldOk[e] != frame && mix(frame, i, 5) % 4u == 0u) {
                st.worldOk[e] = frame;
                worldRows.push_back({frame, i, static_cast<uint64_t>(e)});
            }
        }
        return eligible(st, frame);
    }
    // A layered draw's issue (the UI layer's bracket calls Begin for every draw it takes: a quarter of the marker composites are not the layer's, a few
    // other draws are): Begin takes a pending decision, binds it when the decision is this very draw's (its serial), else counts it stale; End
    // sometimes runs. As nativeOnFootMarkerFixBegin does.
    uint64_t stale = 0;
    void issue(uint32_t frame, uint32_t i, Kind k, uint32_t& fastFrame) {
        const bool layered = k == kMarker ? mix(frame, i, 8) % 4u != 0u : mix(frame, i, 9) % 20u == 0u;
        if (!layered || !st.pending) return;
        st.pending = false;
        if (decisionSerial != serial) {
            ++stale;
            return;
        }
        st.bound = true;
        binds.push_back({frame, i, serial});
        fastFrame = f100::kNoFrame;   // as Begin does
        if (mix(frame, i, 6) % 2u == 0u) st.bound = false;   // End ran
    }
};

template <class Eligible>
inline Model runStream(uint32_t frames, bool prefilter, std::vector<uint64_t>* serials, Eligible&& eligible) {
    Model m;
    uint32_t fastFrame = f100::kNoFrame;
    for (uint32_t f = 1; f <= frames; ++f) {
        const uint32_t draws = 300u + mix(f, 0, 7) % 200u;
        for (uint32_t i = 0; i < draws; ++i) {
            const Kind k = kindOf(f, i);
            const uint64_t vs = vsOf(k, f, i);
            if (prefilter) {
                f100::teeStep(fastFrame, f, vs, m.serial, m.fastCalls, [&](uint64_t) { return m.slow(f, i, k, eligible); });
            } else {
                m.slow(f, i, k, eligible);
            }
            m.issue(f, i, k, fastFrame);
            if (serials) serials->push_back(m.serial);
        }
    }
    return m;
}

inline bool sameOutcome(const Model& a, const Model& b, const std::vector<uint64_t>& sa, const std::vector<uint64_t>& sb) {
    return sa == sb && a.decisions == b.decisions && a.binds == b.binds && a.worldRows == b.worldRows && a.notIssued == b.notIssued &&
           a.lateRestores == b.lateRestores && a.hud == b.hud && a.censusNotes == b.censusNotes && a.stale == b.stale;
}

template <class Check>
inline void run100(Check&& check) {
    const auto eligible = [](const f100::TeeState& s, uint32_t frame) { return f100::teeFastFrame(s, frame); };
    {   // the prefilter against the slow path: 600 frames of 300-500 draws
        std::vector<uint64_t> slowSerials, fastSerials;
        const Model ref = runStream(600, false, &slowSerials, eligible);
        const Model pre = runStream(600, true, &fastSerials, eligible);
        check(sameOutcome(ref, pre, slowSerials, fastSerials),
              "F100: through the prefilter the draw serial after every draw, the decisions, the binds, the world rows and the counts equal the slow path's");
        check(pre.fastCalls > 0 && pre.fastCalls + pre.slowCalls == ref.slowCalls && pre.fastCalls * 2 > ref.slowCalls,
              "F100: the prefilter takes most draws (fast + slow calls = every draw) and the decisions still happen");
        check(!ref.decisions.empty() && !ref.binds.empty() && ref.censusNotes > 0 && ref.lateRestores > 0,
              "F100: the stream exercises decisions, binds, late restores and census frames");
    }
    {   // the check has teeth: a prefilter that ignored the census or a pending decision would change the outcome
        const auto noCensus = [](const f100::TeeState& s, uint32_t frame) {
            f100::TeeState t = s;
            t.census = false;
            return f100::teeFastFrame(t, frame);
        };
        const auto noPending = [](const f100::TeeState& s, uint32_t frame) {
            f100::TeeState t = s;
            t.pending = false;
            return f100::teeFastFrame(t, frame);
        };
        std::vector<uint64_t> a, b, c;
        const Model ref = runStream(300, false, &a, eligible);
        const Model nc = runStream(300, true, &b, noCensus);
        const Model np = runStream(300, true, &c, noPending);
        check(!sameOutcome(ref, nc, a, b), "F100: a prefilter that skipped a census frame's world draws would not match the slow path (the comparison sees it)");
        check(!sameOutcome(ref, np, a, c) && ref.stale == 0 && np.stale > 0,
              "F100: a prefilter that skipped draws while a decision is pending would let a later layered draw find it stale (the comparison sees it)");
    }
    {   // each condition alone keeps the slow path
        f100::TeeState s;
        s.polledFrame = 7;
        s.worldOk[0] = s.worldOk[1] = 7;
        const bool ok = f100::teeFastFrame(s, 7) == 7u;
        f100::TeeState m = s, p = s, b = s, c = s, w = s, t = s;
        m.measure = true;
        p.pending = true;
        b.bound = true;
        c.census = true;
        w.worldOk[1] = 6;
        t.polledFrame = 6;
        const bool each = f100::teeFastFrame(m, 7) == f100::kNoFrame && f100::teeFastFrame(p, 7) == f100::kNoFrame && f100::teeFastFrame(b, 7) == f100::kNoFrame &&
                          f100::teeFastFrame(c, 7) == f100::kNoFrame && f100::teeFastFrame(w, 7) == f100::kNoFrame && f100::teeFastFrame(t, 7) == f100::kNoFrame;
        const bool vs = !f100::teeFast(7, 7, nf::kMarkerCompositeVs) && !f100::teeFast(7, 7, nf::kHudIssuerVs[0]) && !f100::teeFast(7, 7, nf::kHudIssuerVs[1]) &&
                        f100::teeFast(7, 7, 0x1234ull) && !f100::teeFast(7, 8, 0x1234ull) && !f100::teeFast(f100::kNoFrame, f100::kNoFrame, 0x1234ull);
        check(ok && each && vs, "F100: the fast frame needs the frame turned, nothing pending or bound, no census, both eyes' rows and no measurement; HUD and marker draws always go slow");
    }
    {   // the tee clock's arithmetic: 10 MHz QPC, 64 calls clocked at 20 ticks (2 us) each, 2,000,000 calls over 90 frames
        const double ns = f100::teeNsPerCall(64 * 20, 64, 10000000);
        const double ms = f100::teeMsPerFrame(64 * 20, 64, 2000000, 90, 10000000);
        check(ns > 1999.0 && ns < 2001.0 && ms > 44.44 && ms < 44.45 && f100::teeMsPerFrame(0, 0, 1, 1, 1) < 0.0,
              "F100: the tee clock: the mean clocked call, times the second's calls over its frames (2 us x 22,222 calls a frame = 44.4 ms)");
    }
    {   // the load sampler's want: F98's case (a pipeline whose summary printed when its final step ran, never seen done, Flags2 present) is off
        const bool f98 = !f100::loadSamplerWanted(false, true, true, false);
        const bool menu = f100::loadSamplerWanted(true, false, false, false) && f100::loadSamplerWanted(true, true, true, true);
        const bool loading = f100::loadSamplerWanted(false, true, false, false) && !f100::loadSamplerWanted(false, true, false, true) &&
                             !f100::loadSamplerWanted(false, false, false, false);
        check(f98 && menu && loading, "F100: the load sampler ends at the pipeline's first summary (F98's never-done final step held it on all session), runs in a menu");
    }
    {   // F68d: 360 lines in slices of 24, in order, then the summary once
        f100::PacedLines p;
        p.begin(360);
        std::vector<uint32_t> printed;
        uint32_t ticks = 0, summaries = 0, maxSlice = 0;
        for (int t = 0; t < 40; ++t) {
            uint32_t first = 0;
            const uint32_t n = p.take(&first);
            if (n) ++ticks;
            maxSlice = n > maxSlice ? n : maxSlice;
            for (uint32_t k = first; k < first + n; ++k) printed.push_back(k);
            if (p.finish()) ++summaries;
        }
        bool inOrder = printed.size() == 360;
        for (uint32_t k = 0; k < printed.size() && inOrder; ++k) inOrder = printed[k] == k;
        f100::PacedLines z;
        z.begin(0);
        uint32_t zf = 0;
        const bool empty = z.take(&zf) == 0 && z.finish() && !z.finish() && !z.active;
        check(inOrder && ticks == 15 && maxSlice == f100::kOnsetLinesPerTick && summaries == 1 && !p.active && empty,
              "F100: the F68d window prints all 360 lines in order, 24 a tick over 15 ticks, then its summary once (an empty window: the summary only)");
    }
    {   // the eye probe's lines: a sample every frame (11 ms) for 10 s prints about once a second
        uint64_t last = 0;
        uint32_t lines = 0;
        for (uint64_t ms = 1000; ms < 11000; ms += 11) {
            if (f100::eyeLineDue(ms, last)) {
                last = ms;
                ++lines;
            }
        }
        check(lines >= 9 && lines <= 11 && !f100::eyeLineDue(5000, 5000) && !f100::eyeLineDue(0, 0),
              "F100: the eye probe's lines: one a second from a sample every frame (F98: two lines every frame), never twice for one sample");
    }
    {   // F70b: the change lines restart on the eligibility's edges only
        int sourceLast = -1, compareLast = -1, oldSourceLast = -1, oldCompareLast = -1;
        bool eligibleLast = false;
        uint32_t lines = 0, oldLines = 0;
        for (int t = 0; t < 2000; ++t) {
            const bool elig = t >= 1000;
            const int source = elig ? 1 : 0, compare = elig ? 2 : 0;
            if (f100::adsChangeLinesRestart(elig, eligibleLast)) sourceLast = compareLast = -1;
            eligibleLast = elig;
            if (source != sourceLast) { sourceLast = source; ++lines; }
            if (compare != compareLast) { compareLast = compare; ++lines; }
            if (!elig) oldSourceLast = oldCompareLast = -1;   // the old rule: reset on every ineligible tick
            if (source != oldSourceLast) { oldSourceLast = source; ++oldLines; }
            if (compare != oldCompareLast) { oldCompareLast = compare; ++oldLines; }
        }
        check(lines == 4 && oldLines == 2002, "F100: the F70b change lines print once at each edge (4 over 2,000 ticks); the old rule printed two every ineligible tick");
    }
}

}  // namespace f100test
