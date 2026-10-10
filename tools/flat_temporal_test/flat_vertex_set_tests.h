#pragma once
// THE VERTICES A DRAW READS, EXACTLY (design doc section 104, the third and fourth builds): the parts that need no device.
//
// The history itself runs on WARP in tools\weapon_motion_test (flat_vertex_set_tests.h there). Here:
//   - historyVertexSetMeets, the one function that decides whether a write of bytes meets a vertex of a draw's set, as a pure function: a table of
//     writes against sets (the boundaries, the gaps, the routes to the same bytes, a write before the buffer's first vertex, the stride that
//     does not divide the write) and, for each way the function could be wrong, a mutant the table must fail;
//   - the log lines: every formatter of flat_history_report.h and the shadow's three parts, held to the logger's budget with twelve-digit counters,
//     printing every counter (a window of zeros prints zeros) and every key in exactly one line;
//   - the wiring pins: the history, the adapter and the runtime are sources the pure rig cannot drive end to end, so each decision is held to its
//     text, with a mutation control that fails it.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>
#include "../../src/d3d11/animated_history_ledger.h"
#include "../../src/d3d11/animated_history_writes.h"
#include "../../src/d3d11/flat_foreground_shadow.h"
#include "../../src/d3d11/flat_history_report.h"

namespace flat_vertex_set_detail {
using edvr::HistoryVertexSet;
// A set from runs {first, last, first, last, ...}; low and high are the first and last of them.
inline HistoryVertexSet makeSet(std::initializer_list<uint32_t> runs, bool exact = true) {
    HistoryVertexSet s;
    s.runs.assign(runs.begin(), runs.end());
    s.low = s.runs.front();
    s.high = s.runs.back();
    s.exact = exact;
    return s;
}
struct MeetsRow {
    const char* name;
    int set;                  // which set of the table
    int64_t vb0;
    uint64_t stride, first, end;
    bool meets, inSpan;
};
using MeetsFn = bool (*)(const HistoryVertexSet&, int64_t, uint64_t, uint64_t, uint64_t, bool&);

inline int64_t floorDiv(int64_t a, int64_t b) { int64_t q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
inline int64_t truncDiv(int64_t a, int64_t b) { return a / b; }

// The function under test, written once more with a seam for each mistake it could make. `mistake` 0 is the production's reading of the contract; the
// other values are the mutants the table has to fail. (The production function is also run against the table directly.)
template<int Mistake>
inline bool meetsMutant(const HistoryVertexSet& set, int64_t vb0, uint64_t stride, uint64_t first, uint64_t end, bool& inSpan) {
    inSpan = false;
    if (!stride || first >= end || set.runs.empty()) return false;
    int64_t s = static_cast<int64_t>(stride), base = vb0, endLess = 1;
    if constexpr (Mistake == 4) s = 1;          // a stride of one
    if constexpr (Mistake == 8) base = 0;       // the vertex buffer start ignored
    if constexpr (Mistake == 3) endLess = 0;    // the end taken as inclusive
    const auto div = [&](int64_t a, int64_t b) { if constexpr (Mistake == 1) return truncDiv(a, b); else return floorDiv(a, b); };
    const int64_t lo = div(static_cast<int64_t>(first) - base, s);
    const int64_t hi = div(static_cast<int64_t>(end) - endLess - base, s);
    const int64_t low = static_cast<int64_t>(set.low), high = static_cast<int64_t>(set.high);
    if constexpr (Mistake == 5) inSpan = true;   // mistake 5: a write outside the envelope is reported as inside it
    bool outside = hi < low || lo > high;
    if constexpr (Mistake == 9) outside = hi < low || lo >= high;      // the last vertex refused
    if constexpr (Mistake == 10) outside = hi <= low || lo > high;     // the first vertex refused
    if (outside) return false;
    inSpan = true;
    size_t a = 0, b = set.runs.size() / 2;
    if constexpr (Mistake == 7) b = 1;   // only the first run is ever searched
    while (a < b) {
        const size_t m = (a + b) / 2;
        bool before = static_cast<int64_t>(set.runs[m * 2 + 1]) < lo;
        if constexpr (Mistake == 2) before = static_cast<int64_t>(set.runs[m * 2 + 1]) <= lo;   // a run ending exactly at the write's first vertex is skipped
        if (before) a = m + 1; else b = m;
    }
    const bool found = a < set.runs.size() / 2 && static_cast<int64_t>(set.runs[a * 2]) <= hi;
    if constexpr (Mistake == 6) { if (!found) inSpan = false; }   // mistake 6: a gap is reported as outside the envelope
    return found;
}
}  // namespace flat_vertex_set_detail

inline int flatVertexSetTests() {
    using namespace edvr;
    using namespace flat_vertex_set_detail;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: vertex-set %s\n", name); ++failures; }
    };

    // ---- historyVertexSetMeets: the table ----------------------------------------------------------------------------------------------------
    // The sets: 0 two runs [0,2] and [6,8]; 1 one vertex [5,5]; 2 the envelope alone [2,10] (not exact); 3 four single vertices 0, 2, 4, 6; 4 the top of
    // the 32-bit range; 5 no runs; 6 three runs [0,0] [3,4] [9,12] (a run search with a middle).
    const HistoryVertexSet sets[] = {makeSet({0, 2, 6, 8}), makeSet({5, 5}), makeSet({2, 10}, false), makeSet({0, 0, 2, 2, 4, 4, 6, 6}),
                                     makeSet({0xFFFFFFF0u, 0xFFFFFFFFu}), HistoryVertexSet{}, makeSet({0, 0, 3, 4, 9, 12})};
    const MeetsRow rows[] = {
        // set 0 at stride 16, vertex buffer start 0: vertices 0..2 and 6..8 are bytes [0,48) and [96,144)
        {"first vertex", 0, 0, 16, 0, 16, true, true},
        {"first byte only", 0, 0, 16, 0, 1, true, true},
        {"the last byte of the last vertex of the first run", 0, 0, 16, 47, 48, true, true},
        {"the first byte of the first vertex of the gap", 0, 0, 16, 48, 49, false, true},
        {"the whole gap", 0, 0, 16, 48, 96, false, true},
        {"the last byte of the gap", 0, 0, 16, 95, 96, false, true},
        {"the first byte of the second run", 0, 0, 16, 96, 97, true, true},
        {"a write straddling the gap's start", 0, 0, 16, 47, 49, true, true},
        {"a write straddling the gap's end", 0, 0, 16, 95, 97, true, true},
        {"a write ending exactly at the gap's end (the second run's first byte is not written)", 0, 0, 16, 48, 96, false, true},
        {"a write starting exactly at the first run's end", 0, 0, 16, 48, 49, false, true},
        {"the middle vertex of the second run", 0, 0, 16, 112, 128, true, true},
        {"the last vertex", 0, 0, 16, 128, 144, true, true},
        {"the last byte of the last vertex", 0, 0, 16, 143, 144, true, true},
        {"the first byte past the envelope", 0, 0, 16, 144, 145, false, false},
        {"well past the envelope", 0, 0, 16, 1000, 1004, false, false},
        {"the whole envelope", 0, 0, 16, 0, 144, true, true},
        {"a write across everything", 0, 0, 16, 0, 100000, true, true},
        {"a write of 16 bytes that is not aligned and lands inside one vertex of the gap", 0, 0, 16, 50, 60, false, true},
        {"a write of 16 bytes that is not aligned and lands in two vertices, one of them the first run's last", 0, 0, 16, 40, 56, true, true},
        {"two bytes inside the gap, unaligned", 0, 0, 16, 49, 51, false, true},
        {"an empty write", 0, 0, 16, 5, 5, false, false},
        {"an inverted write", 0, 0, 16, 60, 10, false, false},
        // set 0 with the vertices placed at byte 32 (a buffer offset of 32, or a base of 2): everything moves by 32
        {"offset 32: the first vertex", 0, 32, 16, 32, 48, true, true},
        {"offset 32: the byte before the first vertex", 0, 32, 16, 31, 32, false, false},
        {"offset 32: a write that ends exactly at the first vertex (nothing of it)", 0, 32, 16, 16, 32, false, false},
        {"offset 32: the last byte before the first vertex and the first byte of it", 0, 32, 16, 31, 33, true, true},
        {"offset 32: sixteen bytes that end one byte inside the first vertex", 0, 32, 16, 17, 33, true, true},
        {"offset 32: the gap", 0, 32, 16, 80, 128, false, true},
        {"offset 32: the second run's first vertex", 0, 32, 16, 128, 144, true, true},
        {"offset 32: before the buffer's start entirely", 0, 32, 16, 0, 16, false, false},
        // a base before the buffer: vertex i is at -32 + 16 i, so index 2 is bytes [0,16)
        {"negative start: byte 0 is index 2, the last of the first run", 0, -32, 16, 0, 16, true, true},
        {"negative start: index 3 is the gap", 0, -32, 16, 16, 32, false, true},
        {"negative start: index 6 is the second run", 0, -32, 16, 64, 80, true, true},
        {"negative start: index 9 is past the envelope", 0, -32, 16, 112, 128, false, false},
        // a stride that does not divide the write
        {"stride 24: bytes across a vertex boundary", 0, 0, 24, 23, 25, true, true},
        {"stride 24: vertex 3 to 5 whole (the gap)", 0, 0, 24, 72, 144, false, true},
        {"stride 24: one byte more reaches vertex 6", 0, 0, 24, 72, 145, true, true},
        {"stride 24: vertex 2's last byte", 0, 0, 24, 71, 72, true, true},
        {"stride 1: byte i is vertex i", 0, 0, 1, 3, 6, false, true},
        {"stride 1: byte 6 is the second run", 0, 0, 1, 5, 7, true, true},
        {"stride 0 meets nothing", 0, 0, 0, 0, 100, false, false},
        // set 1, a single vertex
        {"single: the vertex", 1, 0, 16, 80, 96, true, true},
        {"single: the byte before", 1, 0, 16, 79, 80, false, false},
        {"single: one byte in", 1, 0, 16, 80, 81, true, true},
        {"single: the byte after", 1, 0, 16, 96, 97, false, false},
        {"single: straddling it", 1, 0, 16, 79, 81, true, true},
        // set 2, the envelope alone: every vertex in it is met, a gap included
        {"envelope: the lowest vertex", 2, 0, 16, 32, 48, true, true},
        {"envelope: the byte before it", 2, 0, 16, 31, 32, false, false},
        {"envelope: the middle", 2, 0, 16, 100, 101, true, true},
        {"envelope: the last vertex", 2, 0, 16, 160, 176, true, true},
        {"envelope: the byte past it", 2, 0, 16, 176, 177, false, false},
        // set 3, single vertices 0, 2, 4, 6 (a search that must land on each, and between)
        {"singles: vertex 0", 3, 0, 16, 0, 16, true, true},
        {"singles: vertex 1 is between", 3, 0, 16, 16, 32, false, true},
        {"singles: vertex 2", 3, 0, 16, 32, 48, true, true},
        {"singles: vertex 3 is between", 3, 0, 16, 48, 64, false, true},
        {"singles: vertex 4", 3, 0, 16, 64, 80, true, true},
        {"singles: vertex 5 is between", 3, 0, 16, 80, 96, false, true},
        {"singles: vertex 6", 3, 0, 16, 96, 112, true, true},
        {"singles: vertex 7 is past", 3, 0, 16, 112, 128, false, false},
        {"singles: vertices 1 and 2", 3, 0, 16, 16, 48, true, true},
        {"singles: vertices 5 and 6", 3, 0, 16, 80, 112, true, true},
        // set 4, the top of the 32-bit range, with 64-bit byte offsets
        {"top: the highest vertex", 4, 0, 16, 4294967295ull * 16, 4294967295ull * 16 + 16, true, true},
        {"top: the first of the run", 4, 0, 16, 4294967280ull * 16, 4294967280ull * 16 + 1, true, true},
        {"top: the vertex below the run", 4, 0, 16, 4294967279ull * 16, 4294967280ull * 16, false, false},
        // set 5, no runs: it meets nothing
        {"empty set", 5, 0, 16, 0, 100, false, false},
        // set 6, three runs [0,0] [3,4] [9,12]
        {"three runs: vertex 0", 6, 0, 16, 0, 16, true, true},
        {"three runs: vertices 1 and 2 are the first gap", 6, 0, 16, 16, 48, false, true},
        {"three runs: vertex 3", 6, 0, 16, 48, 64, true, true},
        {"three runs: vertex 4 (the middle run's last)", 6, 0, 16, 64, 80, true, true},
        {"three runs: vertices 5 to 8 are the second gap", 6, 0, 16, 80, 144, false, true},
        {"three runs: vertex 9", 6, 0, 16, 144, 160, true, true},
        {"three runs: vertex 12", 6, 0, 16, 192, 208, true, true},
        {"three runs: vertex 13 is past", 6, 0, 16, 208, 224, false, false},
        {"three runs: a write that spans the middle run from the gap before to the gap after", 6, 0, 16, 40, 90, true, true},
    };
    const auto holds = [&](MeetsFn fn, const char** firstFailure) {
        for (const MeetsRow& r : rows) {
            bool inSpan = false;
            const bool hit = fn(sets[r.set], r.vb0, r.stride, r.first, r.end, inSpan);
            if (hit != r.meets || inSpan != r.inSpan) { if (firstFailure && !*firstFailure) *firstFailure = r.name; return false; }
        }
        return true;
    };
    const char* firstFailure = nullptr;
    expect(holds(historyVertexSetMeets, &firstFailure), firstFailure ? firstFailure : "historyVertexSetMeets holds the table");
    expect(holds(meetsMutant<0>, nullptr), "the reference reading of the contract used for the mutants holds the table (so a mutant that fails it fails by its own mistake)");
    expect(sizeof(rows) / sizeof(rows[0]) >= 70, "the table has its seventy rows");
    const struct { MeetsFn fn; const char* what; } mutants[] = {
        {meetsMutant<1>, "a division that truncates toward zero (a write just before the first vertex is read as on it)"},
        {meetsMutant<2>, "a run search that skips a run whose last index is exactly the write's first vertex"},
        {meetsMutant<3>, "an end taken as inclusive (a write that ends at a vertex boundary is read as touching the next vertex)"},
        {meetsMutant<4>, "a stride of one"},
        {meetsMutant<5>, "an envelope test that always reports the write as inside it"},
        {meetsMutant<6>, "a gap reported as outside the envelope"},
        {meetsMutant<7>, "a search that reads only the first run"},
        {meetsMutant<8>, "a vertex buffer start (offset and base) that is ignored"},
        {meetsMutant<9>, "an envelope test that refuses a write on the last vertex"},
        {meetsMutant<10>, "an envelope test that refuses a write on the first vertex"},
    };
    for (const auto& m : mutants) {
        char name[256];
        std::snprintf(name, sizeof(name), "mutation control: %s fails the table", m.what);
        expect(!holds(m.fn, nullptr), name);
    }
    // The boundary table stays the same for every stride: a vertex at stride s occupies [i*s, (i+1)*s), and a write is on it iff it shares a byte.
    {
        bool allStrides = true;
        const HistoryVertexSet s = makeSet({0, 2, 6, 8});
        for (uint64_t stride : {1ull, 2ull, 4ull, 12ull, 16ull, 20ull, 32ull, 36ull, 64ull}) {
            for (int64_t vb0 : {int64_t(0), int64_t(40), int64_t(-7 * 64)}) {
                for (uint64_t first = 0; first < 700 && allStrides; first += 7)
                    for (uint64_t len : {1ull, 3ull, 16ull, 33ull}) {
                        // the oracle: any byte of the write inside any vertex of the set
                        bool oracle = false, spanOracle = false;
                        for (uint64_t b = first; b < first + len && !oracle; ++b) {
                            const int64_t rel = static_cast<int64_t>(b) - vb0;
                            if (rel < 0) continue;
                            const uint64_t idx = static_cast<uint64_t>(rel) / stride;
                            if (idx <= 2 || (idx >= 6 && idx <= 8)) oracle = true;
                        }
                        for (uint64_t b = first; b < first + len && !spanOracle; ++b) {
                            const int64_t rel = static_cast<int64_t>(b) - vb0;
                            if (rel >= 0 && static_cast<uint64_t>(rel) / stride <= 8) spanOracle = true;
                        }
                        bool inSpan = false;
                        const bool hit = historyVertexSetMeets(s, vb0, stride, first, first + len, inSpan);
                        if (hit != oracle || inSpan != spanOracle) allStrides = false;
                    }
            }
        }
        expect(allStrides, "against a byte-by-byte oracle, across nine strides, three vertex buffer starts (one before byte 0) and a sweep of writes of 1, 3, 16 and 33 bytes, the function agrees on hit and on envelope");
        // The same oracle over the mutants: each is wrong somewhere in the sweep, so the sweep is a second net under the table.
        const auto sweep = [&](MeetsFn fn) {
            for (uint64_t stride : {16ull, 24ull})
                for (int64_t vb0 : {int64_t(0), int64_t(32)})
                    for (uint64_t first = 0; first < 400; ++first)
                        for (uint64_t len : {1ull, 16ull}) {
                            bool oracle = false;
                            for (uint64_t b = first; b < first + len; ++b) {
                                const int64_t rel = static_cast<int64_t>(b) - vb0;
                                if (rel < 0) continue;
                                const uint64_t idx = static_cast<uint64_t>(rel) / stride;
                                if (idx <= 2 || (idx >= 6 && idx <= 8)) oracle = true;
                            }
                            bool inSpan = false;
                            if (fn(sets[0], vb0, stride, first, first + len, inSpan) != oracle) return false;
                        }
            return true;
        };
        expect(sweep(historyVertexSetMeets) && sweep(meetsMutant<0>), "the byte sweep holds the function and the reference reading");
        expect(!sweep(meetsMutant<1>) && !sweep(meetsMutant<2>) && !sweep(meetsMutant<3>) && !sweep(meetsMutant<4>) && !sweep(meetsMutant<8>),
               "mutation control: the byte sweep alone fails the truncating division, the skipped run, the inclusive end, the unit stride and the ignored start (the first-run-only search needs the table's three-run set)");
    }
    // The vocabulary: an empty set is exact and meets nothing; a set the history marks as an envelope meets by its runs like any other.
    {
        HistoryVertexSet fresh;
        bool inSpan = true;
        expect(fresh.exact && fresh.runs.empty() && fresh.low == 0 && fresh.high == 0 && !historyVertexSetMeets(fresh, 0, 16, 0, 1000, inSpan) && !inSpan,
               "a set that has not been read meets nothing and is not on the envelope");
        const HistoryVertexSet envelope = makeSet({3, 9}, false);
        inSpan = false;
        expect(historyVertexSetMeets(envelope, 0, 16, 5 * 16, 5 * 16 + 1, inSpan) && inSpan && !envelope.exact,
               "an envelope-only set is a single run: a write anywhere inside it is met (the gap it could not see is a hit)");
    }

    // ---- the lines -------------------------------------------------------------------------------------------------------------------------------
    expect(kFlatLogLineBudget == 1000, "the logger cuts a line at about 1167 characters; every report line is built to stay under 1000");
    const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    const auto countOf = [](const std::string& text, const std::string& needle) {
        size_t n = 0, at = 0;
        while ((at = text.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
        return n;
    };
    const auto fillStats = [&](HistoryWriteStats& s, uint64_t& next) {
        const auto take = [&] { next += 7919; return next; };
        for (unsigned e = 0; e < kHistoryWriteEntries; ++e) {
            s.observed[e] = take(); s.ranged[e] = take(); s.savedWrites[e] = take(); s.recordsInvalidated[e] = take();
            for (unsigned r = 0; r < kHistoryWriteRoles; ++r)
                for (unsigned t = 0; t < kHistoryWriteTimings; ++t) { s.touching[e][r][t] = take(); s.invalidating[e][r][t] = take(); }
        }
        s.unknownInvalidating[0] = take(); s.unknownInvalidating[1] = take();
        s.sparedRecords = take();
        for (unsigned i = 0; i < static_cast<unsigned>(HistoryErase::Count); ++i) s.erased[i] = take();
        s.allocations = take(); s.allocationFailures = take();
        s.extentIssued = take(); s.extentRead = take(); s.extentFailed = take(); s.setFromCache = take(); s.setPending = take(); s.setApproximate = take(); s.setCancelled = take();
        s.vertexUnknown = take(); s.vertexInGap = take(); s.vertexOutside = take(); s.vertexGenuine = take(); s.deferredInvalidated = take(); s.deferredConservative = take();
    };
    const auto fillAll = [&](HistoryWriteStats& s, uint64_t v) {
        for (unsigned e = 0; e < kHistoryWriteEntries; ++e) {
            s.observed[e] = s.ranged[e] = s.savedWrites[e] = s.recordsInvalidated[e] = v;
            for (unsigned r = 0; r < kHistoryWriteRoles; ++r)
                for (unsigned t = 0; t < kHistoryWriteTimings; ++t) s.touching[e][r][t] = s.invalidating[e][r][t] = v;
        }
        s.unknownInvalidating[0] = s.unknownInvalidating[1] = s.sparedRecords = v;
        for (auto& x : s.erased) x = v;
        s.allocations = s.allocationFailures = s.extentIssued = s.extentRead = s.extentFailed = s.setFromCache = s.setPending = s.setApproximate = s.setCancelled = v;
        s.vertexUnknown = s.vertexInGap = s.vertexOutside = s.vertexGenuine = s.deferredInvalidated = s.deferredConservative = v;
    };
    {
        FlatHistoryWindow w;
        uint64_t next = 1000003;
        fillStats(w.writes, next);
        const HistoryWriteStats& s = w.writes;
        w.records = 111; w.bytes = 222222; w.peakRecords = 128; w.peakBytes = 33333333;
        w.topCount = 3;
        for (unsigned i = 0; i < 3; ++i) {
            w.top[i].resource = reinterpret_cast<const void*>(uintptr_t(0x1000) * (i + 1));
            w.top[i].touching = next += 7919; w.top[i].invalidating = next += 7919; w.top[i].saved = next += 7919;
        }
        char want[512];

        // -- the summary line --
        char line[4096];
        const int n = flatHistoryLine(line, sizeof(line), w);
        const std::string summary(line);
        expect(n > 0 && static_cast<size_t>(n) == summary.size() && static_cast<size_t>(n) <= kFlatLogLineBudget, "the summary line is terminated and under the logger's budget");
        expect(summary.rfind("flat foreground history 5s: records=111 bytes=222222 peak-records=128 peak-bytes=33333333 allocations=", 0) == 0,
               "the summary line leads with the key the log is read by and the size gauges, in the order the reader parses them");
        std::snprintf(want, sizeof(want), " allocations=%llu allocation-failures=%llu; erased: advance-invalidated=%llu pressure-invalidated=%llu pressure-spent=%llu advance-aged=%llu; "
                      "vertex sets: requested=%llu read=%llu failed=%llu cancelled=%llu from-cache=%llu already-pending=%llu approximate=%llu",
                      u(s.allocations), u(s.allocationFailures), u(s.erased[1]), u(s.erased[2]), u(s.erased[3]), u(s.erased[4]), u(s.extentIssued), u(s.extentRead),
                      u(s.extentFailed), u(s.setCancelled), u(s.setFromCache), u(s.setPending), u(s.setApproximate));
        expect(summary.find(want) != std::string::npos && summary.size() - summary.find(want) == std::strlen(want),
               "the summary line carries the allocations, every erase path and the seven vertex-set counters, each under its own name, and ends with the last");

        // -- the writes lines --
        std::string parts[kFlatHistoryWriteLines];
        for (unsigned p = 0; p < kFlatHistoryWriteLines; ++p) {
            const int k = flatHistoryWritesLine(line, sizeof(line), w, p);
            parts[p] = line;
            expect(k > 0 && static_cast<size_t>(k) == parts[p].size() && static_cast<size_t>(k) <= kFlatLogLineBudget, "each writes line is terminated and under the logger's budget");
            std::snprintf(want, sizeof(want), "flat foreground history writes 5s (%u/%u):", p + 1, kFlatHistoryWriteLines);
            expect(parts[p].rfind(want, 0) == 0, "each writes line names its position among the three, so a line cut out of the log stands alone");
        }
        expect(kFlatHistoryWriteLines == 3 && flatHistoryWritesLine(line, sizeof(line), w, 3) == 0 && flatHistoryWritesLine(line, sizeof(line), w, 77) == 0,
               "there are three writes lines, and a fourth prints nothing");
        const unsigned firsts[4] = {0, 3, 6, kHistoryWriteEntries};
        bool entries = true, once = true;
        for (unsigned e = 0; e < kHistoryWriteEntries; ++e) {
            std::snprintf(want, sizeof(want),
                          " %s: observed=%llu touching(v-gap/v-window/i-gap/i-window)=%llu/%llu/%llu/%llu invalidating=%llu/%llu/%llu/%llu ranged=%llu saved-writes=%llu records-invalidated=%llu;",
                          historyWriteEntryName(e), u(s.observed[e]), u(s.touching[e][0][0]), u(s.touching[e][0][1]), u(s.touching[e][1][0]), u(s.touching[e][1][1]),
                          u(s.invalidating[e][0][0]), u(s.invalidating[e][0][1]), u(s.invalidating[e][1][0]), u(s.invalidating[e][1][1]), u(s.ranged[e]), u(s.savedWrites[e]),
                          u(s.recordsInvalidated[e]));
            unsigned where = 0;
            for (unsigned p = 0; p < kFlatHistoryWriteLines; ++p) where += static_cast<unsigned>(countOf(parts[p], want));
            once = once && where == 1;
            for (unsigned p = 0; p < kFlatHistoryWriteLines; ++p) {
                const bool belongs = e >= firsts[p] && e < firsts[p + 1];
                entries = entries && (parts[p].find(want) != std::string::npos) == belongs;
            }
        }
        expect(once, "each of the eight entries is on exactly one writes line, with its observed count, the four touching and four invalidating counts in order, ranged, saved-writes and records-invalidated");
        expect(entries, "entries 0-2 are on the first line, 3-5 on the second, 6-7 on the third, and no entry is on a line that is not its");
        {
            // The mutation controls for the order: two counts swapped inside one entry change the line that carries it, and only that one.
            FlatHistoryWindow swapped = w;
            std::swap(swapped.writes.touching[4][0][1], swapped.writes.touching[4][1][0]);
            char other[4096];
            flatHistoryWritesLine(other, sizeof(other), swapped, 1);
            const std::string changed(other);
            flatHistoryWritesLine(other, sizeof(other), swapped, 0);
            const std::string untouched(other);
            expect(changed != parts[1] && untouched == parts[0],
                   "mutation control: swapping a vertices-window and an indices-gap count of an entry changes its line (the legend's order is real) and no other");
            FlatHistoryWindow saved = w;
            saved.writes.savedWrites[7] += 1;
            flatHistoryWritesLine(other, sizeof(other), saved, 2);
            expect(std::string(other) != parts[2] && std::string(other).find(std::to_string(static_cast<unsigned long long>(s.savedWrites[7] + 1))) != std::string::npos,
                   "mutation control: a changed counter of the last entry changes the last line (the value is printed, not a constant)");
        }

        // -- the vertex line --
        const int vn = flatHistoryVertexLine(line, sizeof(line), w);
        const std::string vertex(line);
        expect(vn > 0 && static_cast<size_t>(vn) == vertex.size() && static_cast<size_t>(vn) <= kFlatLogLineBudget, "the vertex line is terminated and under the logger's budget");
        std::snprintf(want, sizeof(want),
                      "flat foreground history vertex writes 5s: unknown-invalidating=%llu/%llu (gap/window) spared-records=%llu; ranged writes meeting a live record's vertex buffer: "
                      "extent-unknown=%llu in-gap=%llu outside-span=%llu genuine=%llu; deferred-invalidated=%llu deferred-conservative=%llu; top resources written (touching/invalidating/saved):",
                      u(s.unknownInvalidating[0]), u(s.unknownInvalidating[1]), u(s.sparedRecords), u(s.vertexUnknown), u(s.vertexInGap), u(s.vertexOutside), u(s.vertexGenuine),
                      u(s.deferredInvalidated), u(s.deferredConservative));
        expect(vertex.rfind(want, 0) == 0, "the vertex line carries unknown-invalidating, spared-records, the four ranged-write cases and the two deferred counts, each under its own name, in order");
        std::snprintf(want, sizeof(want), " %p=%llu/%llu/%llu %p=%llu/%llu/%llu %p=%llu/%llu/%llu", w.top[0].resource, u(w.top[0].touching), u(w.top[0].invalidating), u(w.top[0].saved),
                      w.top[1].resource, u(w.top[1].touching), u(w.top[1].invalidating), u(w.top[1].saved), w.top[2].resource, u(w.top[2].touching), u(w.top[2].invalidating), u(w.top[2].saved));
        expect(vertex.size() > std::strlen(want) && vertex.compare(vertex.size() - std::strlen(want), std::strlen(want), want) == 0,
               "and ends with the resources written most, most first, each with touching, invalidating and saved");
        {
            FlatHistoryWindow few = w;
            few.topCount = 1;
            char other[4096], first[200], second[200];
            std::snprintf(first, sizeof(first), " %p=%llu/%llu/%llu", w.top[0].resource, u(w.top[0].touching), u(w.top[0].invalidating), u(w.top[0].saved));
            std::snprintf(second, sizeof(second), " %p=%llu/%llu/%llu", w.top[1].resource, u(w.top[1].touching), u(w.top[1].invalidating), u(w.top[1].saved));
            flatHistoryVertexLine(other, sizeof(other), few);
            expect(std::string(other).find(" none") == std::string::npos && std::string(other).find(first) != std::string::npos && std::string(other).find(second) == std::string::npos,
                   "a window with one resource listed prints one resource");
            FlatHistoryWindow swapped = w;
            std::swap(swapped.writes.vertexInGap, swapped.writes.vertexOutside);
            flatHistoryVertexLine(other, sizeof(other), swapped);
            expect(std::string(other) != vertex, "mutation control: swapping the in-gap and outside-span counts changes the line");
        }

        // -- every counter, once --
        bool every = true;
        std::string all = summary;
        for (const std::string& p : parts) all += "\n" + p;
        all += "\n" + vertex;
        const auto hasNumber = [&](uint64_t v) {
            const std::string digits = std::to_string(static_cast<unsigned long long>(v));
            size_t at = 0;
            while ((at = all.find(digits, at)) != std::string::npos) {
                const bool before = at == 0 || !std::isdigit(static_cast<unsigned char>(all[at - 1]));
                const bool after = at + digits.size() >= all.size() || !std::isdigit(static_cast<unsigned char>(all[at + digits.size()]));
                if (before && after) return true;
                at += digits.size();
            }
            return false;
        };
        for (unsigned e = 0; e < kHistoryWriteEntries; ++e) {
            every = every && hasNumber(s.observed[e]) && hasNumber(s.ranged[e]) && hasNumber(s.savedWrites[e]) && hasNumber(s.recordsInvalidated[e]);
            for (unsigned r = 0; r < kHistoryWriteRoles; ++r)
                for (unsigned t = 0; t < kHistoryWriteTimings; ++t) every = every && hasNumber(s.touching[e][r][t]) && hasNumber(s.invalidating[e][r][t]);
        }
        every = every && hasNumber(s.unknownInvalidating[0]) && hasNumber(s.unknownInvalidating[1]) && hasNumber(s.sparedRecords) && hasNumber(s.allocations) &&
                hasNumber(s.allocationFailures) && hasNumber(s.extentIssued) && hasNumber(s.extentRead) && hasNumber(s.extentFailed) && hasNumber(s.setFromCache) &&
                hasNumber(s.setPending) && hasNumber(s.setApproximate) && hasNumber(s.setCancelled) && hasNumber(s.vertexUnknown) && hasNumber(s.vertexInGap) &&
                hasNumber(s.vertexOutside) && hasNumber(s.vertexGenuine) && hasNumber(s.deferredInvalidated) && hasNumber(s.deferredConservative);
        for (unsigned i = 1; i < static_cast<unsigned>(HistoryErase::Count); ++i) every = every && hasNumber(s.erased[i]);
        expect(every, "every counter of the statistics appears in the five lines: nothing the window measured is dropped");
        // A counter that is not printed: the control zeroes the line's copy and the number goes missing.
        {
            FlatHistoryWindow mine = w;
            mine.writes.deferredConservative = 424242424242ull;
            char other[4096];
            flatHistoryVertexLine(other, sizeof(other), mine);
            expect(std::string(other).find("deferred-conservative=424242424242;") != std::string::npos, "a distinct value of the last counter reaches the line under its name");
            mine = w;
            mine.writes.setApproximate = 525252525252ull;
            flatHistoryLine(other, sizeof(other), mine);
            expect(std::string(other).find("approximate=525252525252") != std::string::npos, "a distinct value of the summary's last counter reaches the line under its name");
        }

        // -- the empty window prints every counter at zero: the line's absence must mean the code did not run, never that nothing happened --
        FlatHistoryWindow empty;
        flatHistoryLine(line, sizeof(line), empty);
        expect(std::string(line) ==
                   "flat foreground history 5s: records=0 bytes=0 peak-records=0 peak-bytes=0 allocations=0 allocation-failures=0; erased: advance-invalidated=0 "
                   "pressure-invalidated=0 pressure-spent=0 advance-aged=0; vertex sets: requested=0 read=0 failed=0 cancelled=0 from-cache=0 already-pending=0 approximate=0",
               "an empty window prints the gauges, the allocation counters, every erase path and every vertex-set counter at zero");
        bool allZero = true;
        std::string zeros;
        for (unsigned p = 0; p < kFlatHistoryWriteLines; ++p) {
            flatHistoryWritesLine(line, sizeof(line), empty, p);
            zeros += line;
        }
        for (unsigned e = 0; e < kHistoryWriteEntries; ++e) {
            std::snprintf(want, sizeof(want), " %s: observed=0 touching(v-gap/v-window/i-gap/i-window)=0/0/0/0 invalidating=0/0/0/0 ranged=0 saved-writes=0 records-invalidated=0;",
                          historyWriteEntryName(e));
            allZero = allZero && countOf(zeros, want) == 1;
        }
        expect(allZero, "an empty window prints every entry on the three writes lines with all of its counters at zero");
        flatHistoryVertexLine(line, sizeof(line), empty);
        expect(std::string(line) ==
                   "flat foreground history vertex writes 5s: unknown-invalidating=0/0 (gap/window) spared-records=0; ranged writes meeting a live record's vertex buffer: "
                   "extent-unknown=0 in-gap=0 outside-span=0 genuine=0; deferred-invalidated=0 deferred-conservative=0; top resources written (touching/invalidating/saved): none",
               "an empty window prints the vertex line's counters at zero and says none for the resources written");

        // -- the budget, at twelve digits in every field, and at the widest a counter can be --
        for (const uint64_t v : {uint64_t(999999999999ull), ~uint64_t(0)}) {
            FlatHistoryWindow wide;
            fillAll(wide.writes, v);
            wide.records = wide.bytes = wide.peakRecords = wide.peakBytes = ~0u;
            wide.topCount = 3;
            for (auto& t : wide.top) { t.resource = reinterpret_cast<const void*>(~uintptr_t(0)); t.touching = t.invalidating = t.saved = v; }
            const size_t limit = v == 999999999999ull ? kFlatLogLineBudget : size_t(1167);
            const char* const how = v == 999999999999ull ? "with twelve digits in every field" : "with every counter at 2^64-1";
            char scratch[4096];
            std::memset(scratch, 'x', sizeof(scratch));
            int k = flatHistoryLine(scratch, sizeof(scratch), wide);
            char name[256];
            std::snprintf(name, sizeof(name), "%s the summary line is terminated and within %zu characters", how, limit);
            expect(k > 0 && scratch[k] == 0 && static_cast<size_t>(k) <= limit, name);
            for (unsigned p = 0; p < kFlatHistoryWriteLines; ++p) {
                std::memset(scratch, 'x', sizeof(scratch));
                k = flatHistoryWritesLine(scratch, sizeof(scratch), wide, p);
                std::snprintf(name, sizeof(name), "%s writes line %u is terminated and within %zu characters", how, p + 1, limit);
                expect(k > 0 && scratch[k] == 0 && static_cast<size_t>(k) <= limit, name);
                if (v == 999999999999ull) {
                    // at twelve digits nothing is cut: every entry of the line is whole
                    bool whole = true;
                    for (unsigned e = firsts[p]; e < firsts[p + 1]; ++e) {
                        std::snprintf(name, sizeof(name), " %s: observed=999999999999 touching(v-gap/v-window/i-gap/i-window)=", historyWriteEntryName(e));
                        whole = whole && std::string(scratch).find(name) != std::string::npos;
                    }
                    whole = whole && std::string(scratch).size() > 3 && std::string(scratch).back() == ';';
                    expect(whole, "at twelve digits every entry of every writes line is whole: the last ends with its closing semicolon");
                }
            }
            std::memset(scratch, 'x', sizeof(scratch));
            k = flatHistoryVertexLine(scratch, sizeof(scratch), wide);
            std::snprintf(name, sizeof(name), "%s the vertex line is terminated and within %zu characters", how, limit);
            expect(k > 0 && scratch[k] == 0 && static_cast<size_t>(k) <= limit, name);
            HistoryWriteExample x;
            x.kind = HistoryWriteCase::Genuine; x.entry = HistoryWriteEntry::CopyResource; x.resource = reinterpret_cast<const void*>(~uintptr_t(0));
            x.first = v; x.end = v; x.known = true; x.spanFirst = v - 5; x.spanEnd = v; x.exact = false; x.runs = ~0u; x.age = ~0u;
            x.key.vs = x.key.layout = x.key.vertices = x.key.indices = reinterpret_cast<const void*>(~uintptr_t(0));
            x.key.count = x.key.start = x.key.offset = x.key.stride = x.key.indexOffset = x.key.format = ~0u;
            x.key.base = -2147483647 - 1;
            std::memset(scratch, 'x', sizeof(scratch));
            k = flatHistoryExampleLine(scratch, sizeof(scratch), x);
            std::snprintf(name, sizeof(name), "%s the example line is terminated and within %zu characters", how, limit);
            expect(k > 0 && scratch[k] == 0 && static_cast<size_t>(k) <= limit, name);
            for (unsigned p = 0; p < kFlatShadowLineParts; ++p) {
                ShadowStats st;
                st.sampledFrames = st.drawsRead = st.notReady = st.failed = v;
                for (auto& b : st.byKind) b = v;
                for (unsigned i = 0; i < kFlatShadowBins; ++i) st.meanAll[i] = st.meanAccepted[i] = st.affineAll[i] = st.affineAccepted[i] = st.residualBins[i] = v;
                st.currentAccepts = st.affineAccepts = st.bothAccept = st.neitherAccept = v;
                for (auto& g : st.gateFailed) g = v;
                st.donorDrawsSum = st.donorVerticesSum = v;
                st.receiverCurrentAccepts = st.receiverCurrentRefuses = st.receiverRefusedAffineAccepts = st.receiverAcceptedAffineAccepts = v;
                st.modeTwoFires = st.modeTwoWhileMoving = st.movingFrames = v;
                std::memset(scratch, 'x', sizeof(scratch));
                k = flatShadowLine(scratch, sizeof(scratch), st, p);
                std::snprintf(name, sizeof(name), "%s shadow line %u is terminated and within %zu characters", how, p + 1, limit);
                expect(k > 0 && scratch[k] == 0 && static_cast<size_t>(k) <= limit, name);
            }
        }
        // A buffer the size of the logger's cut is filled to its last byte, never past it.
        {
            FlatHistoryWindow wide;
            fillAll(wide.writes, ~uint64_t(0));
            char tight[200];
            std::memset(tight, 'x', sizeof(tight));
            flatHistoryWritesLine(tight, sizeof(tight), wide, 0);
            expect(std::strlen(tight) == sizeof(tight) - 1, "a buffer too small for a writes line is truncated at its last byte, not overrun");
            std::memset(tight, 'x', sizeof(tight));
            flatHistoryVertexLine(tight, sizeof(tight), wide);
            expect(std::strlen(tight) == sizeof(tight) - 1, "and for the vertex line");
            std::memset(tight, 'x', sizeof(tight));
            flatHistoryLine(tight, sizeof(tight), wide);
            expect(std::strlen(tight) == sizeof(tight) - 1, "and for the summary line");
        }
    }

    // -- the example line --
    {
        HistoryWriteExample x;
        x.kind = HistoryWriteCase::InGap; x.entry = HistoryWriteEntry::Update; x.resource = reinterpret_cast<const void*>(0x7000);
        x.first = 48; x.end = 96; x.known = true; x.spanFirst = 0; x.spanEnd = 144; x.exact = true; x.runs = 2; x.age = 7;
        x.key.vs = reinterpret_cast<const void*>(0x10); x.key.layout = reinterpret_cast<const void*>(0x20); x.key.vertices = reinterpret_cast<const void*>(0x7000);
        x.key.indices = reinterpret_cast<const void*>(0x40); x.key.count = 6; x.key.start = 3; x.key.base = -2; x.key.offset = 64; x.key.stride = 16;
        x.key.indexOffset = 8; x.key.format = 57;
        char line[4096];
        const int n = flatHistoryExampleLine(line, sizeof(line), x);
        const std::string text(line);
        expect(n > 0 && static_cast<size_t>(n) == text.size() && static_cast<size_t>(n) <= kFlatLogLineBudget, "the example line is terminated and under the logger's budget");
        char want[512];
        std::snprintf(want, sizeof(want), "flat foreground history write example: case=in-gap entry=update resource=%p write=[48,96) set=[0,144) exact runs=2 write-starts=48 bytes into it (33.3%% of the span) "
                      "record-age=7 frames key: ", x.resource);
        expect(text.rfind(want, 0) == 0, "the example line names the case, the entry, the resource, the write, the record's set and where in it the write began, and the record's age");
        std::snprintf(want, sizeof(want), "key: shader=%p layout=%p vertices=%p indices=%p count=6 start=3 base=-2 vb-offset=64 stride=16 ib-offset=8 format=57", x.key.vs, x.key.layout,
                      x.key.vertices, x.key.indices);
        expect(text.find(want) != std::string::npos && text.size() - text.find(want) == std::strlen(want), "and ends with the whole key, base included with its sign");
        x.kind = HistoryWriteCase::Genuine; x.exact = false; x.runs = 1;
        flatHistoryExampleLine(line, sizeof(line), x);
        expect(std::string(line).find("case=genuine") != std::string::npos && std::string(line).find("envelope-only runs=1") != std::string::npos,
               "a genuine write on an envelope-only set says so");
        x.kind = HistoryWriteCase::Unknown; x.known = false;
        flatHistoryExampleLine(line, sizeof(line), x);
        expect(std::string(line).find("case=extent-unknown") != std::string::npos && std::string(line).find("set=unknown record-age=7") != std::string::npos &&
                   std::string(line).find("write-starts") == std::string::npos,
               "a write met with an unknown set says set=unknown and has no position in it");
        x.kind = HistoryWriteCase::InGap; x.known = true; x.exact = true; x.spanFirst = 100; x.spanEnd = 100;
        flatHistoryExampleLine(line, sizeof(line), x);
        expect(std::string(line).find("set=unknown") != std::string::npos, "a set with an empty span is not divided by: it prints as unknown");
        x.spanFirst = 200; x.spanEnd = 400; x.first = 150;
        flatHistoryExampleLine(line, sizeof(line), x);
        expect(std::string(line).find("write-starts=-50 bytes into it (-25.0% of the span)") != std::string::npos, "a write that starts before the set's span has a negative position");
    }
    return failures;
}

// ---- the wiring: the history, the adapter and the runtime are sources the pure rig cannot drive end to end, so each decision is held to its text,
// with a mutation control that fails it (tools\weapon_motion_test runs the history itself on a device) ----
inline int flatVertexSetWiringTests() {
    using namespace edvr;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: vertex-set wiring %s\n", name); ++failures; }
    };
    const auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const auto compact = [](const std::string& in) {
        std::string out; out.reserve(in.size());
        for (char c : in) if (c != ' ' && c != '\r' && c != '\n' && c != '\t') out += c;
        return out;
    };
    // A top-level function ends at the first closing brace in column 0; a member of a class whose members are indented four spaces ends at the first
    // line that is four spaces and a brace.
    const auto body = [](const std::string& text, const char* signature) {
        const size_t at = text.find(signature);
        if (at == std::string::npos) return std::string();
        const size_t end = text.find("\n}\n", at);
        return text.substr(at, end == std::string::npos ? std::string::npos : end + 3 - at);
    };
    const auto member = [](const std::string& text, const char* signature) {
        const size_t at = text.find(signature);
        if (at == std::string::npos) return std::string();
        const size_t end = text.find("\n    }\n", at);
        return text.substr(at, end == std::string::npos ? std::string::npos : end + 7 - at);
    };
    const auto count = [](const std::string& text, const std::string& needle) {
        size_t n = 0, at = 0;
        while ((at = text.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
        return n;
    };
    const auto ordered = [&](const std::string& compacted, std::initializer_list<std::string> needles) {
        size_t pos = 0;
        bool ok = !compacted.empty();
        for (const std::string& needle : needles) {
            const size_t at = compacted.find(compact(needle), pos);
            if (at == std::string::npos) { ok = false; break; }
            pos = at + compact(needle).size();
        }
        return ok;
    };
    // A mutation must change the text, or its control proves nothing: a needle that is not found is a failure of the test itself.
    const auto without = [&](std::string text, const char* needle) {
        const std::string n = compact(needle);
        const size_t at = text.find(n);
        if (at == std::string::npos) { std::printf("FAIL: vertex-set wiring mutation needle not found: %s\n", needle); ++failures; return text; }
        text.erase(at, n.size());
        return text;
    };
    const auto replaced = [&](std::string text, const char* from, const char* to) {
        const std::string f = compact(from);
        const size_t at = text.find(f);
        if (at == std::string::npos) { std::printf("FAIL: vertex-set wiring mutation needle not found: %s\n", from); ++failures; return text; }
        text.replace(at, f.size(), compact(to));
        return text;
    };

    const std::string historySource = slurp("src/d3d11/animated_vertex_history.h");
    const std::string writesSource = slurp("src/d3d11/animated_history_writes.h");
    const std::string motionSource = slurp("src/d3d11/flat_foreground_motion.h");
    const std::string runtimeSource = slurp("src/d3d11/flat_runtime.cpp");
    expect(!historySource.empty() && !writesSource.empty() && !motionSource.empty() && !runtimeSource.empty(), "the sources are readable from the repo root");
    const std::string historyCompact = compact(historySource);

    // -- the pure function the whole decision rests on --
    const std::string meetsFn = compact(body(writesSource, "inline bool historyVertexSetMeets("));
    const auto meetsFnValid = [&](const std::string& text) {
        return ordered(text, {"inSpan=false;", "if(!stride||first>=end||set.runs.empty())returnfalse;",
                              "constint64_tlo=floorDiv(static_cast<int64_t>(first)-vb0,s);", "constint64_thi=floorDiv(static_cast<int64_t>(end)-1-vb0,s);",
                              "if(hi<static_cast<int64_t>(set.low)||lo>static_cast<int64_t>(set.high))returnfalse;", "inSpan=true;",
                              "if(static_cast<int64_t>(set.runs[m*2+1])<lo)a=m+1;elseb=m;", "returna<set.runs.size()/2&&static_cast<int64_t>(set.runs[a*2])<=hi;"});
    };
    expect(meetsFnValid(meetsFn), "the set test floors both ends of the write to vertices (the end less one byte), is outside when the vertices do not reach the envelope, and searches the runs for one that reaches them");
    expect(!meetsFnValid(replaced(meetsFn, "static_cast<int64_t>(end)-1-vb0", "static_cast<int64_t>(end)-vb0")) &&
               !meetsFnValid(replaced(meetsFn, "if(static_cast<int64_t>(set.runs[m*2+1])<lo)", "if(static_cast<int64_t>(set.runs[m*2+1])<=lo)")) &&
               !meetsFnValid(replaced(meetsFn, "if(hi<static_cast<int64_t>(set.low)||lo>static_cast<int64_t>(set.high))", "if(hi<static_cast<int64_t>(set.low)||lo>=static_cast<int64_t>(set.high))")) &&
               !meetsFnValid(replaced(meetsFn, "inSpan=true;", "inSpan=false;")) && !meetsFnValid(without(meetsFn, "||set.runs.empty()")),
           "mutation control: an inclusive end, a skipped run, an envelope that refuses its last vertex, an envelope that is never reported, or a set with no runs that is searched fails the pin");
    expect(count(compact(writesSource), "floorDiv=[](int64_ta,int64_tb){int64_tq=a/b;return(a%b!=0&&((a<0)!=(b<0)))?q-1:q;}") == 1,
           "the division is a floor (a write before the first vertex is before it, not on it)");
    expect(count(replaced(compact(writesSource), "?q-1:q", ":q"), "?q-1:q") == 0, "mutation control: a truncating division is not what the text says");

    // -- the history --
    const std::string meets = compact(member(historySource, "static bool meets("));
    const auto meetsValid = [&](const std::string& text) {
        return ordered(text, {"vertexCase=0;", "constboolisV=resource==g.vertices.Get(),isI=resource==g.indices.Get();", "if(!isV&&!isI)returnfalse;", "if(end==~uint64_t(0))returntrue;",
                              "if(isI){uint64_ta=0,b=0;indexRange(g,a,b);if(historyRangesOverlap(first,end,a,b))hit=true;}", "if(isV){", "if(!set)vertexCase=1;",
                              "elseif(set==wholeVertexSet().get()){vertexCase=5;hit=true;}", "constint64_tvb0=int64_t(g.offset)+int64_t(g.base)*int64_t(g.stride);",
                              "if(historyVertexSetMeets(*set,vb0,g.stride,first,end,inSpan)){vertexCase=4;hit=true;}", "elsevertexCase=inSpan?2:3;", "returnhit;"});
    };
    expect(meetsValid(meets), "a write meets a record by its index range exactly, by its vertex set once known (placed by the buffer offset and the signed base times the stride), "
                              "as nothing but a spare while the set is unknown, and as everything when its indices could not be read");
    expect(!meetsValid(replaced(meets, "if(!set)vertexCase=1;", "if(!set){vertexCase=1;hit=true;}")),
           "mutation control: an unknown set treated as a hit (the old rule: the whole buffer) fails the pin");
    expect(!meetsValid(replaced(meets, "elsevertexCase=inSpan?2:3;", "else{vertexCase=inSpan?2:3;hit=inSpan;}")),
           "mutation control: a gap in the set treated as a hit (the envelope) fails the pin");
    expect(!meetsValid(replaced(meets, "else if(set==wholeVertexSet().get()){vertexCase=5;hit=true;}", "else if(set==wholeVertexSet().get()){vertexCase=5;}")),
           "mutation control: a record whose indices could not be read that spares every write fails the pin");
    expect(!meetsValid(replaced(meets, "int64_t(g.offset)+int64_t(g.base)*int64_t(g.stride)", "int64_t(g.offset)+int64_t(unsigned(g.base))*int64_t(g.stride)")) &&
               !meetsValid(replaced(meets, "int64_t(g.offset)+int64_t(g.base)*int64_t(g.stride)", "int64_t(g.base)*int64_t(g.stride)")) &&
               !meetsValid(replaced(meets, "int64_t(g.offset)+int64_t(g.base)*int64_t(g.stride)", "int64_t(g.offset)+int64_t(g.base)")),
           "mutation control: a base read as unsigned (a negative base that still names valid bytes), no buffer offset, or a base not times the stride fails the pin");
    expect(!meetsValid(without(meets, "if(isI){uint64_ta=0,b=0;indexRange(g,a,b);if(historyRangesOverlap(first,end,a,b))hit=true;}")),
           "mutation control: a record that does not read its index bytes fails the pin");
    const std::string captureHit = compact(member(historySource, "bool captureHitBy("));
    const auto captureHitValid = [&](const std::string& text) {
        return ordered(text, {"if(!resource)returntrue;", "constHistoryVertexSet*set=nullptr;",
                              "for(constauto&r:records_)if(r.vset&&matches(r.geometry,c.geometry)){set=r.vset.get();break;}", "unsignedvertexCase=0;",
                              "returnmeets(c.geometry,set,resource,first,end,vertexCase);"});
    };
    expect(captureHitValid(captureHit), "the adapter's lists are judged by the same function as the history's records, against the set the record of the draw's geometry holds (none: unknown)");
    expect(!captureHitValid(replaced(captureHit, "returnmeets(c.geometry,set,resource,first,end,vertexCase);", "returntrue;")),
           "mutation control: a list that drops every draw that reads the written buffer fails the pin");
    expect(!captureHitValid(replaced(captureHit, "if(r.vset&&matches(r.geometry,c.geometry))", "if(matches(r.geometry,c.geometry))")),
           "mutation control: a draw judged by the first record of its geometry whether or not that one has its set fails the pin");

    const std::string poll = compact(member(historySource, "void pollExtents("));
    const auto pollValid = [&](const std::string& text) {
        return ordered(text, {"unsignedbudget=wait?kExtentSlots:3;", "if(!s.pending)continue;", "if(!wait&&frame-s.frame<1)continue;",
                              "if(s.cancelled){s.pending=false;s.stage.Reset();++writeStats_.setCancelled;continue;}",
                              "ctx->Map(s.stage.Get(),0,D3D11_MAP_READ,wait?0u:D3D11_MAP_FLAG_DO_NOT_WAIT,&m)", "buildSet(m.pData,size_t(s.bytes/(wide?4:2)),wide);",
                              "if(!set){failSet(s,frame);continue;}", "++writeStats_.extentRead;if(!set->exact)++writeStats_.setApproximate;",
                              "storeSet(key,s.indices,s.ibFirst,s.ibEnd,set);", "s.pending=false;s.stage.Reset();",
                              "for(auto&r:records_)if((!r.vset||r.vset==wholeVertexSet())&&setKeyOf(r.geometry,r.ibFirst,r.ibEnd)==key)adoptSet(r,set);"});
    };
    expect(pollValid(poll), "a read in flight that a write cancelled is dropped and counted unread; one that is ready is built into a set, cached by its index range, and given to every record of the key that lacks it");
    expect(!pollValid(without(poll, "if(s.cancelled){s.pending=false;s.stage.Reset();++writeStats_.setCancelled;continue;}")),
           "mutation control: a read that a write to the indices cancelled but that is adopted anyway fails the pin");
    expect(!pollValid(without(poll, "storeSet(key,s.indices,s.ibFirst,s.ibEnd,set);")), "mutation control: a set that is not cached beyond the record fails the pin");
    expect(!pollValid(replaced(poll, "adoptSet(r,set);", "r.vset=set;")), "mutation control: a set handed to a record without the deferred check fails the pin");
    expect(!pollValid(replaced(poll, "if(!wait&&frame-s.frame<1)continue;", "if(!wait)continue;")), "mutation control: a poll that never reads without waiting fails the pin");
    expect(!pollValid(without(poll, "if(!set->exact)++writeStats_.setApproximate;")), "mutation control: an envelope-only set that is not counted fails the pin");

    const std::string adopt = compact(member(historySource, "void adoptSet("));
    const auto adoptValid = [&](const std::string& text) {
        return ordered(text, {"r.vset=std::move(set);", "unsignedsince=0;boolpublished=false;",
                              "for(unsignedp=0;p<2;++p)if(r.frame[p]!=~0u){published=true;since=(std::max)(since,r.frame[p]);}", "if(!published||r.invalidated)return;",
                              "constsize_tkept=(std::min<size_t>)(deferredNext_,deferred_.size());", "constint64_tvb0=int64_t(r.geometry.offset)+int64_t(r.geometry.base)*int64_t(r.geometry.stride);",
                              "if(d.resource==r.geometry.vertices.Get()&&d.frame>=since&&historyVertexSetMeets(*r.vset,vb0,r.geometry.stride,d.first,d.end,inSpan))overlap=true;",
                              "if(!overlap&&deferredNext_>deferred_.size()){", "for(constDeferredWrite&d:deferred_)oldest=(std::min)(oldest,d.frame);", "conservative=oldest>=since;",
                              "if(!overlap&&!conservative)return;", "r.frame[0]=r.frame[1]=~0u;r.invalidated=true;r.invalidReasons|=2;++r.mutationEpoch;r.invalidatedAt=lastFrame_;",
                              "++(overlap?writeStats_.deferredInvalidated:writeStats_.deferredConservative);"});
    };
    expect(adoptValid(adopt), "a record given its set takes it, and one that was published while it was unknown is invalidated if a write logged since its last publish meets the set, "
                              "or if the log has wrapped past its publish; either way as a vertex write, counted by which");
    expect(!adoptValid(without(adopt, "if(!published||r.invalidated)return;")), "mutation control: a record that was never published, or is down already, that is invalidated again fails the pin");
    expect(!adoptValid(replaced(adopt, "d.frame>=since", "d.frame>since")) && !adoptValid(replaced(adopt, "d.frame>=since", "true")),
           "mutation control: a log read from after the publish, or not read by frame at all, fails the pin");
    expect(!adoptValid(without(adopt, "if(!overlap&&deferredNext_>deferred_.size()){")) && !adoptValid(replaced(adopt, "if(!overlap&&!conservative)return;", "if(!overlap)return;")),
           "mutation control: a log that has wrapped and is trusted anyway fails the pin");
    expect(!adoptValid(replaced(adopt, "conservative=oldest>=since;", "conservative=oldest>since;")),
           "mutation control: a log that has lost writes of the frame of the record's own stamp and trusts it anyway (the comparison one short) fails the pin");
    expect(!adoptValid(replaced(adopt, "r.invalidReasons|=2;", "r.invalidReasons|=4;")) && !adoptValid(without(adopt, "++r.mutationEpoch;")),
           "mutation control: a deferred invalidation filed as an index write, or that leaves the epoch a capture of the record would still publish under, fails the pin");
    expect(!adoptValid(replaced(adopt, "++(overlap?writeStats_.deferredInvalidated:writeStats_.deferredConservative);", "++writeStats_.deferredInvalidated;")),
           "mutation control: a conservative invalidation counted as a met write fails the pin");
    const std::string logDefer = compact(member(historySource, "void logDeferred("));
    expect(ordered(logDefer, {"if(deferred_.empty())deferred_.resize(kDeferredWrites);", "DeferredWrite&d=deferred_[deferredNext_++%kDeferredWrites];",
                              "d.resource=resource;d.first=first;d.end=end;d.frame=lastFrame_;"}),
           "a spared write is kept in a ring of kDeferredWrites with the resource, its bytes and the history's frame");
    expect(!ordered(replaced(logDefer, "deferredNext_++%kDeferredWrites", "deferredNext_%kDeferredWrites"), {"DeferredWrite&d=deferred_[deferredNext_++%kDeferredWrites];"}) &&
               !ordered(replaced(logDefer, "d.frame=lastFrame_;", "d.frame=0;"), {"d.resource=resource;d.first=first;d.end=end;d.frame=lastFrame_;"}),
           "mutation control: a ring that never advances, or entries with no frame, are not what the text says");

    const std::string ensure = compact(member(historySource, "void ensureVertexSet("));
    const auto ensureValid = [&](const std::string& text) {
        return ordered(text, {"if((r.vset&&r.vset!=wholeVertexSet())||r.invalidated||!r.geometry.indices||out.frame<r.setRetryAt)return;",
                              "constuint64_tkey=setKeyOf(r.geometry,r.ibFirst,r.ibEnd);",
                              "if(auto cached=findSet(key)){r.vset=std::move(cached);++writeStats_.setFromCache;return;}",
                              "if(extentSlots_[i].pending&&!extentSlots_[i].cancelled&&extentSlots_[i].key==key){++writeStats_.setPending;return;}",
                              "if(!bytes||r.ibEnd>ib.ByteWidth||bytes>uint64_t(maxVertices)*4)", "constD3D11_BOXbox{UINT(r.ibFirst),0,0,UINT(r.ibEnd),1,1};",
                              "ctx->CopySubresourceRegion(slot.stage.Get(),0,0,0,0,r.geometry.indices.Get(),0,&box);", "slot.pending=true;slot.cancelled=false;",
                              "noteIndexBuffer(slot.indices);++writeStats_.extentIssued;"});
    };
    expect(ensureValid(ensure), "a record without a set takes it from the cache when its index range has been read, waits for the read in flight for the range, or asks the device to copy "
                                "exactly its index bytes; a record that has one asks for nothing");
    expect(!ensureValid(without(ensure, "if(auto cached=findSet(key)){r.vset=std::move(cached);++writeStats_.setFromCache;return;}")),
           "mutation control: a cache that is never consulted at capture (every re-made record asks the device again) fails the pin");
    expect(!ensureValid(replaced(ensure, "if(auto cached=findSet(key)){r.vset=std::move(cached);", "if(auto cached=findSet(key)){adoptSet(r,std::move(cached));")),
           "mutation control: a set taken from the cache at the end of the record's own capture that runs the deferred check (and takes the record at birth) fails the pin");
    // Every road to the whole-buffer set runs the deferred check on the way: the failed read, the range past the buffer, the buffer that would not be made.
    const std::string failSetText = compact(member(historySource, "void failSet("));
    expect(ordered(failSetText, {"s.pending=false;s.stage.Reset();++writeStats_.extentFailed;",
                                 "if((!r.vset||r.vset==wholeVertexSet())&&setKeyOf(r.geometry,r.ibFirst,r.ibEnd)==s.key){adoptSet(r,wholeVertexSet());r.setRetryAt=frame+30;}"}),
           "a failed read gives every record of the key the whole-buffer set through the deferred check, and the retry is thirty frames on");
    expect(!ordered(replaced(failSetText, "{adoptSet(r,wholeVertexSet());r.setRetryAt=frame+30;}", "{r.vset=wholeVertexSet();r.setRetryAt=frame+30;}"), {"adoptSet(r,wholeVertexSet());r.setRetryAt=frame+30;"}) &&
               !ordered(replaced(failSetText, "adoptSet(r,wholeVertexSet());r.setRetryAt=frame+30;", "adoptSet(r,wholeVertexSet());"), {"r.setRetryAt=frame+30;"}),
           "mutation control: a failed read that assigns the whole-buffer set without the check, or retries never, is not what the text says");
    expect(count(ensure, "adoptSet(r,wholeVertexSet());r.setRetryAt=out.frame+30;return;}") == 2 && count(historyCompact, "r.vset=wholeVertexSet()") == 0,
           "the two early roads to the whole-buffer set in the capture (a range past the buffer, a staging buffer that would not be made) run the deferred check and keep the retry, and nothing assigns it bare");
    expect(count(replaced(ensure, "if(!bytes||r.ibEnd>ib.ByteWidth||bytes>uint64_t(maxVertices)*4){adoptSet(r,wholeVertexSet());", "if(!bytes||r.ibEnd>ib.ByteWidth||bytes>uint64_t(maxVertices)*4){r.vset=wholeVertexSet();"),
                 "adoptSet(r,wholeVertexSet());r.setRetryAt=out.frame+30;return;}") == 1,
           "mutation control: an early road that assigns the whole-buffer set bare is not what the text says");
    expect(!ensureValid(replaced(ensure, "if((r.vset&&r.vset!=wholeVertexSet())||", "if(")) && !ensureValid(replaced(ensure, "||r.invalidated", "")),
           "mutation control: a record that has its set but asks again, or an invalidated record that asks, fails the pin");
    expect(!ensureValid(replaced(ensure, "D3D11_BOXbox{UINT(r.ibFirst)", "D3D11_BOXbox{UINT(0)")) &&
               !ensureValid(without(ensure, "if(extentSlots_[i].pending&&!extentSlots_[i].cancelled&&extentSlots_[i].key==key){++writeStats_.setPending;return;}")),
           "mutation control: a copy that begins at byte 0 of the buffer, or a key whose read is in flight that copies again every frame, fails the pin");
    expect(!ensureValid(without(ensure, "noteIndexBuffer(slot.indices);")), "mutation control: a read in flight whose index buffer is not registered (no write could cancel it) fails the pin");
    const std::string keyOf = compact(member(historySource, "static uint64_t setKeyOf("));
    expect(ordered(keyOf, {"mix(bufferId(g.indices.Get()));mix(ibFirst);mix(ibEnd);mix(uint64_t(g.format));"}) && count(keyOf, "uintptr_t") == 0,
           "the cache is keyed by the index buffer's identity (a number kept with it), the byte range and the format: the three things the set depends on, and not by the buffer's address");
    expect(!ordered(replaced(keyOf, "mix(bufferId(g.indices.Get()));", "mix(reinterpret_cast<uintptr_t>(g.indices.Get()));"), {"mix(bufferId(g.indices.Get()));"}) &&
               count(replaced(keyOf, "mix(bufferId(g.indices.Get()));", "mix(reinterpret_cast<uintptr_t>(g.indices.Get()));"), "uintptr_t") == 1,
           "mutation control: a key by the buffer's address (which a buffer made after another was released can reuse) is not what the text says");
    const std::string idFn = compact(member(historySource, "static uint64_t bufferId("));
    const auto idValid = [&](const std::string& text) {
        return ordered(text, {"static const GUID kId={0x5d1c1f0a,0x91c3,0x4b7e,{0x8f,0x54,0x2a,0x73,0x0e,0x66,0xa1,0x7d}};", "static uint64_t next=0;", "if(!buffer)return 0;",
                              "if(SUCCEEDED(buffer->GetPrivateData(kId,&size,&id))&&size==sizeof(id)&&id)return id;", "id=++next;", "buffer->SetPrivateData(kId,sizeof(id),&id);", "return id;"});
    };
    expect(idValid(idFn), "a buffer's identity is read from its private data, and given once, from a counter that never repeats, when it has none");
    expect(!idValid(without(idFn, "buffer->SetPrivateData(kId,sizeof(id),&id);")) && !idValid(replaced(idFn, "id=++next;", "id=1;")) &&
               !idValid(without(idFn, "if(SUCCEEDED(buffer->GetPrivateData(kId,&size,&id))&&size==sizeof(id)&&id)return id;")),
           "mutation control: an identity that is never kept with the buffer, that is the same number for every buffer, or that is made again at every ask, fails the pin");
    expect(!ordered(without(keyOf, "mix(uint64_t(g.format));"), {"mix(uint64_t(g.format));"}) && !ordered(without(keyOf, "mix(ibEnd);"), {"mix(ibEnd);"}) &&
               !ordered(without(keyOf, "mix(ibFirst);"), {"mix(ibFirst);"}),
           "mutation control: a key without the format, or without either end of the range, is not what the text says");
    expect(count(keyOf, "g.vertices") == 0 && count(keyOf, "g.base") == 0 && count(keyOf, "g.offset") == 0 && count(keyOf, "g.stride") == 0,
           "and without the vertex buffer, the base, the offset or the stride: the set depends on the index bytes only, so a record of another vertex buffer shares it");
    const std::string drop = compact(member(historySource, "void dropSets("));
    const auto dropValid = [&](const std::string& text) {
        return ordered(text, {"if(!resource){sets_.clear();for(auto&s:extentSlots_)if(s.pending)s.cancelled=true;return;}", "constboolwhole=end==~uint64_t(0);",
                              "if(sets_[i].indices==resource&&(whole||historyRangesOverlap(first,end,sets_[i].ibFirst,sets_[i].ibEnd)))sets_.erase(sets_.begin()+i);",
                              "if(s.pending&&s.indices==resource&&(whole||historyRangesOverlap(first,end,s.ibFirst,s.ibEnd)))s.cancelled=true;"});
    };
    expect(dropValid(drop), "a write to the indices ends the cached sets and cancels the reads in flight that overlap its bytes (all of them for a whole-buffer write, everything for a write nothing could name)");
    expect(!dropValid(replaced(drop, "(whole||historyRangesOverlap(first,end,sets_[i].ibFirst,sets_[i].ibEnd))", "true")),
           "mutation control: a write that ends the sets of a range it did not touch fails the pin");
    expect(!dropValid(replaced(drop, "(whole||historyRangesOverlap(first,end,s.ibFirst,s.ibEnd))", "false")), "mutation control: a write that leaves a read in flight over its bytes standing fails the pin");
    expect(!dropValid(without(drop, "sets_.clear();")) && !dropValid(replaced(drop, "sets_[i].indices==resource&&", "")),
           "mutation control: a null write that keeps the sets, or a write that ends the sets of other buffers, fails the pin");
    const std::string store = compact(member(historySource, "void storeSet("));
    expect(ordered(store, {"for(auto&e:sets_)if(e.key==key){e.set=std::move(set);e.lastUsed=lastFrame_;return;}", "if(sets_.size()>=kMaxSets){", "size_toldest=0;",
                           "if(sets_[i].lastUsed<sets_[oldest].lastUsed)oldest=i;", "sets_.erase(sets_.begin()+oldest);", "noteIndexBuffer(indices);", "sets_.push_back(std::move(e));"}),
           "a new set at the bound takes the place of the one used longest ago, and its index buffer is registered so a write can end it");
    expect(!ordered(replaced(store, "sets_[i].lastUsed<sets_[oldest].lastUsed", "sets_[i].lastUsed>sets_[oldest].lastUsed"), {"if(sets_[i].lastUsed<sets_[oldest].lastUsed)oldest=i;"}) &&
               !ordered(without(store, "noteIndexBuffer(indices);"), {"noteIndexBuffer(indices);"}),
           "mutation control: the newest-used set replaced instead of the oldest, or a set whose buffer is not registered, is not what the text says");
    const std::string find = compact(member(historySource, "std::shared_ptr<const HistoryVertexSet> findSet("));
    expect(ordered(find, {"for(auto&e:sets_)if(e.key==key){e.lastUsed=lastFrame_;return e.set;}"}),
           "a set found is marked used now (so the one dropped at the bound is the one nobody asked for)");
    const std::string registry = compact(member(historySource, "void noteIndexBuffer("));
    expect(ordered(registry, {"if(isIndexBuffer(resource))return;", "if(indexBuffers_.size()>=256){", "indexBuffers_.clear();sets_.clear();",
                              "for(auto&s:extentSlots_)if(s.pending)s.cancelled=true;", "for(auto&r:records_)r.vset.reset();", "indexBuffers_.push_back(resource);"}),
           "an index buffer past the registry's cap forgets everything held (sets, reads in flight, the records' sets) before it is registered: a set no write could end is never kept");
    expect(!ordered(without(registry, "for(auto&r:records_)r.vset.reset();"), {"for(auto&r:records_)r.vset.reset();"}) &&
               !ordered(without(registry, "indexBuffers_.clear();sets_.clear();"), {"indexBuffers_.clear();sets_.clear();"}),
           "mutation control: a reset that leaves the records' sets (or the cache) standing is not what the text says");
    const std::string build = compact(member(historySource, "static std::shared_ptr<const HistoryVertexSet> buildSet("));
    const auto buildValid = [&](const std::string& text) {
        return ordered(text, {"if(!n)returnnullptr;", "constuint32_tsentinel=wide?0xFFFFFFFFu:0xFFFFu;", "if(v==sentinel)returnnullptr;", "set->low=lo;set->high=hi;",
                              "constuint64_tspan=uint64_t(hi)-lo+1;", "if(span>kMaxSetSpan){envelope();returnset;}", "set->runs.push_back(uint32_t(lo+start));set->runs.push_back(uint32_t(lo+i-1));",
                              "if(set->runs.size()>size_t(kMaxRuns)*2){envelope();returnset;}"}) &&
               ordered(text, {"constautoenvelope=[&]{set->runs={lo,hi};set->exact=false;};"});
    };
    expect(buildValid(build), "a set is the runs of the vertices the indices name; the sentinel surviving means the copy never happened; a span past kMaxSetSpan or runs past kMaxRuns "
                              "make it the envelope alone, marked inexact");
    expect(!buildValid(replaced(build, "if(span>kMaxSetSpan)", "if(span>=kMaxSetSpan)")) && !buildValid(replaced(build, "set->runs.size()>size_t(kMaxRuns)*2", "set->runs.size()>=size_t(kMaxRuns)*2")),
           "mutation control: bounds that fall one short fail the pin");
    expect(!buildValid(without(build, "if(v==sentinel)returnnullptr;")) && !buildValid(replaced(build, "set->exact=false;", "set->exact=true;")),
           "mutation control: a sentinel taken for an index, or an envelope marked exact, fails the pin");
    expect(count(historyCompact, "kMaxSets=256,kDeferredWrites=128,kMaxRuns=4096;") == 1 && count(historyCompact, "kMaxSetSpan=4u*1024*1024;") == 1 &&
               count(historyCompact, "kInvalidKeepFrames=3,kExtentSlots=24,kTombstones=128;") == 1,
           "the bounds: 256 sets, a ring of 128 spared writes, 4096 runs, a span of four million vertices, 24 reads in flight");
    expect(count(replaced(historyCompact, "kMaxSets=256", "kMaxSets=255"), "kMaxSets=256") == 0 && count(replaced(historyCompact, "kDeferredWrites=128", "kDeferredWrites=127"), "kDeferredWrites=128") == 0,
           "mutation control: a changed bound is not what the text says");
    // The capture asks for the set after the draw is submitted, under the extended policy only; VR pays nothing.
    const std::string captureFn = compact(member(historySource, "bool capture(ID3D11DeviceContext* ctx,PanelCurveDrawFn draw,unsigned count"));
    expect(ordered(captureFn, {"submitIdentity(ctx,out);submitPositions(ctx,draw,startInstance,out);", "if(retainIndex&&!retainInstanceIndex(ctx,out)){out.refusal=\"index-snapshot-creation\";returnfalse;}",
                               "if(extended)ensureVertexSet(ctx,out);"}) && count(historyCompact, "ensureVertexSet(") == 2,
           "the set is asked for once, after the draw is submitted, and only under the extended policy");
    expect(count(replaced(captureFn, "if(extended)ensureVertexSet(ctx,out);", "ensureVertexSet(ctx,out);"), "if(extended)ensureVertexSet(") == 0 &&
               count(without(captureFn, "if(extended)ensureVertexSet(ctx,out);"), "ensureVertexSet(") == 0,
           "mutation control: a capture that never asks for the set, or asks under the default policy too, is not what the text says");
    const std::string example = compact(member(historySource, "void noteExample("));
    expect(ordered(example, {"unsigned&n=exampleCount_[unsigned(kind)];", "if(n>=kHistoryExamplesPerCase)return;", "HistoryWriteExample&x=examples_[unsigned(kind)][n++];",
                             "x.known=r.vset&&r.vset!=wholeVertexSet();", "x.spanFirst=a<0?0:uint64_t(a);x.spanEnd=b<0?0:uint64_t(b);x.exact=r.vset->exact;x.runs=unsigned(r.vset->runs.size()/2);",
                             "x.age=lastFrame_-r.bornFrame;x.key=historyKeyOf(r.geometry);"}),
           "an example is kept per case up to kHistoryExamplesPerCase, with the write, the set's envelope in bytes of the vertex buffer, exact and runs, the record's age and its key");
    // Examples and the three-way counters are only for counted writes; the log of spared writes is for all.
    const std::string written = compact(member(historySource, "unsigned resourceWritten("));
    expect(ordered(written, {"if(vcase==1&&!deferredLogged){logDeferred(resource,first,end);deferredLogged=true;}", "if(tally&&vcase){",
                             "if(vcase==1)++writeStats_.vertexUnknown;", "elseif(vcase==2)++writeStats_.vertexInGap;", "else++writeStats_.vertexOutside;",
                             "if(vcase<=2)noteExample(vcase==1?HistoryWriteCase::Unknown:HistoryWriteCase::InGap,entry,resource,first,end,r);",
                             "if(tally&&ranged&&vcase==4){++writeStats_.vertexGenuine;noteExample(HistoryWriteCase::Genuine,entry,resource,first,end,r);}"}),
           "the three-way counters and the examples are for counted writes only, an outside write has no example, and the log of writes spared on an unknown set is kept whether counted or not");
    expect(!ordered(replaced(written, "if(tally&&vcase){", "if(vcase){"), {"if(tally&&vcase){"}) && !ordered(replaced(written, "if(vcase<=2)noteExample(", "noteExample("), {"if(vcase<=2)noteExample("}) &&
               !ordered(replaced(written, "if(tally&&ranged&&vcase==4){", "if(ranged&&vcase==4){"), {"if(tally&&ranged&&vcase==4){"}),
           "mutation control: examples collected for an uncounted notification, for an outside write, or a genuine one counted twice, are not what the text says");

    // -- the adapter forwards the examples, the runtime prints them --
    expect(count(compact(motionSource), "unsignedtakeWriteExamples(HistoryWriteExample*out,unsignedcapacity){returnhistory_.takeWriteExamples(out,capacity);}") == 1,
           "the adapter hands the history's examples to the runtime");
    const std::string report = compact(body(runtimeSource, "static void reportForegroundNoCandidate("));
    const std::string printHistory = "flatHistoryLine(line,sizeof(line),history);Log::get().note(\"%s\",line);";
    const std::string printWrites = "for(unsignedpart=0;part<kFlatHistoryWriteLines;++part){flatHistoryWritesLine(line,sizeof(line),history,part);Log::get().note(\"%s\",line);}";
    const std::string printVertex = "flatHistoryVertexLine(line,sizeof(line),history);Log::get().note(\"%s\",line);";
    const std::string printExamples = "for(unsignedi=0;i<exampleCount&&s.historyExampleLines<96;++i,++s.historyExampleLines){flatHistoryExampleLine(line,sizeof(line),examples[i]);Log::get().note(\"%s\",line);}";
    const auto reportValid = [&](const std::string& text) {
        return ordered(text, {"HistoryWriteExampleexamples[kHistoryWriteCases*kHistoryExamplesPerCase*State::kDomainCandidateCap]{};unsignedexampleCount=0;",
                              "exampleCount+=motion.takeWriteExamples(examples+exampleCount,kHistoryWriteCases*kHistoryExamplesPerCase);", printHistory, printWrites, printVertex, printExamples,
                              "if(shadowWindow.sampledFrames"});
    };
    expect(reportValid(report), "the 5 s report prints the summary, the three writes lines, the vertex line and the examples (up to 96 a session), in that order, ahead of the shadow");
    expect(!reportValid(without(report, printHistory.c_str())) && !reportValid(without(report, printWrites.c_str())) && !reportValid(without(report, printVertex.c_str())) &&
               !reportValid(without(report, printExamples.c_str())),
           "mutation control: a report that never prints the summary, the writes lines, the vertex line or the examples fails the pin");
    expect(!reportValid(replaced(report, "part<kFlatHistoryWriteLines;", "part<kFlatHistoryWriteLines-1;")) && !reportValid(replaced(report, "s.historyExampleLines<96", "s.historyExampleLines<960")) &&
               !reportValid(replaced(report, "flatHistoryWritesLine(line,sizeof(line),history,part)", "flatHistoryWritesLine(line,sizeof(line),history,0)")),
           "mutation control: a report that drops the last writes line, prints without a cap, or prints the first writes line three times, fails the pin");
    expect(!reportValid(without(report, "exampleCount+=motion.takeWriteExamples(examples+exampleCount,kHistoryWriteCases*kHistoryExamplesPerCase);")),
           "mutation control: examples that are never taken from the candidates fail the pin");
    const std::string delta = compact(body(runtimeSource, "static HistoryWriteStats historyWindowDelta("));
    expect(count(delta, "w.deferredInvalidated=d(now.deferredInvalidated,was.deferredInvalidated);w.deferredConservative=d(now.deferredConservative,was.deferredConservative);") == 1 &&
               count(delta, "w.vertexUnknown=d(now.vertexUnknown,was.vertexUnknown);w.vertexInGap=d(now.vertexInGap,was.vertexInGap);") == 1 &&
               count(delta, "w.setFromCache=d(now.setFromCache,was.setFromCache);w.setPending=d(now.setPending,was.setPending);") == 1,
           "the window's difference carries the three-way, deferred and vertex-set counters under their own names");
    return failures;
}
