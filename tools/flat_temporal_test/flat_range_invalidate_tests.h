#pragma once
// RANGE-AWARE INVALIDATION OF THE FIRST-PERSON VERTEX HISTORY: the parts that need no device (design doc section 104, the 10-07 settlement walk).
//
// The history itself runs on WARP in tools\weapon_motion_test (flat_range_invalidate_tests.h there). Here:
//   - the overlap test and the key digest, which are plain functions, each held to a table and shown to fail for the mistakes a table must catch;
//   - the ledger's labels for a published key whose record is gone, and the order they take in the classifier;
//   - HistoryWriteStats::add summing every counter (the log lines and the vertex set are in flat_vertex_set_tests.h beside this);
//   - the wiring pins: the hooks, the adapter and the history are sources the rigs cannot drive end to end (the hooks live in the game's process),
//     so each decision is held to its text, with a mutation control that fails it.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <type_traits>
#include <utility>
#include "../../src/d3d11/animated_history_ledger.h"
#include "../../src/d3d11/animated_history_writes.h"
#include "../../src/d3d11/flat_history_report.h"
#include "../../src/d3d11/flat_no_candidate_report.h"

inline int flatRangeInvalidateTests() {
    using namespace edvr;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: range-invalidate %s\n", name); ++failures; }
    };

    // -- the overlap test: half-open byte ranges, an empty range overlaps nothing --
    {
        struct Row { uint64_t a0, a1, b0, b1; bool overlap; };
        const Row rows[] = {
            {0, 12, 12, 24, false},   // adjacent ranges share no byte
            {12, 24, 0, 12, false},   {0, 12, 11, 24, true},   {11, 24, 0, 12, true},    {0, 100, 10, 20, true},   {10, 20, 0, 100, true},
            {0, 1, 0, 1, true},       {0, 1, 1, 2, false},     {5, 5, 0, 100, false},    {0, 100, 5, 5, false},    {5, 5, 5, 5, false},
            {0, ~uint64_t(0), 5, 6, true}, {7, 3, 0, 100, false}   // an inverted range is empty, not wrapped
        };
        const auto holds = [&](bool (*overlap)(uint64_t, uint64_t, uint64_t, uint64_t)) {
            for (const Row& r : rows) if (overlap(r.a0, r.a1, r.b0, r.b1) != r.overlap) return false;
            return true;
        };
        expect(holds(historyRangesOverlap), "the overlap test is half-open, symmetric, and an empty or inverted range overlaps nothing");
        expect(!holds([](uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) { return a0 <= b1 && b0 <= a1; }),
               "mutation control: a closed-interval overlap (adjacent ranges share a byte) fails the table");
        expect(!holds([](uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) { return a0 < b1 && b0 < a1; }),
               "mutation control: an overlap with no empty-range guard fails the table");
        expect(!holds([](uint64_t a0, uint64_t, uint64_t, uint64_t b1) { return a0 < b1; }),
               "mutation control: a one-sided overlap fails the table");
        expect(!holds([](uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) { return a0 < a1 && b0 < b1 && a0 < b1 && b0 + 1 < a1; }),
               "mutation control: an overlap that misses a one-byte intersection at the end fails the table");
    }

    // -- the vocabulary: the names the line prints, once each --
    {
        bool unique = true, nonEmpty = true;
        for (unsigned a = 0; a < kHistoryWriteEntries; ++a) {
            const char* name = historyWriteEntryName(a);
            nonEmpty = nonEmpty && name && name[0] && (a + 1 == kHistoryWriteEntries || std::strcmp(name, "unknown") != 0);
            for (unsigned b = a + 1; b < kHistoryWriteEntries; ++b) unique = unique && std::strcmp(name, historyWriteEntryName(b)) != 0;
        }
        expect(unique && nonEmpty && kHistoryWriteEntries == 8 && !std::strcmp(historyWriteEntryName(0), "map") &&
                   !std::strcmp(historyWriteEntryName(1), "update") && !std::strcmp(historyWriteEntryName(2), "copy-region") &&
                   !std::strcmp(historyWriteEntryName(3), "copy-resource") && !std::strcmp(historyWriteEntryName(4), "clear") &&
                   !std::strcmp(historyWriteEntryName(5), "dispatch-uav") && !std::strcmp(historyWriteEntryName(6), "other") &&
                   !std::strcmp(historyWriteEntryName(7), "unknown") && !std::strcmp(historyWriteEntryName(8), "unknown"),
               "the eight write entries carry their names in enum order, and an index past the end is unknown");
        expect(kHistoryWriteTimings == 2 && !std::strcmp(historyWriteTimingName(0), "gap") && !std::strcmp(historyWriteTimingName(1), "window") &&
                   kHistoryWriteRoles == 2 && static_cast<unsigned>(HistoryWriteRole::Vertices) == 0 && static_cast<unsigned>(HistoryWriteRole::Indices) == 1 &&
                   static_cast<unsigned>(HistoryWriteTiming::Gap) == 0 && static_cast<unsigned>(HistoryWriteTiming::Window) == 1,
               "the line's gap/window and vertices/indices order is the enums' order");
        bool unique2 = true;
        for (unsigned a = 1; a < static_cast<unsigned>(HistoryErase::Count); ++a) {
            for (unsigned b = a + 1; b < static_cast<unsigned>(HistoryErase::Count); ++b) unique2 = unique2 && std::strcmp(historyEraseName(a), historyEraseName(b)) != 0;
            unique2 = unique2 && std::strcmp(historyEraseName(a), "unknown") != 0 && historyEraseName(a)[0];
        }
        expect(unique2 && static_cast<unsigned>(HistoryErase::Count) == 5 && !std::strcmp(historyEraseName(1), "advance-invalidated") &&
                   !std::strcmp(historyEraseName(2), "pressure-invalidated") && !std::strcmp(historyEraseName(3), "pressure-spent") &&
                   !std::strcmp(historyEraseName(4), "advance-aged"),
               "the four erase paths carry their names");
        // The three cases an example is filed under, in enum order, and how many a window keeps.
        expect(kHistoryWriteCases == 3 && kHistoryExamplesPerCase == 2 && static_cast<unsigned>(HistoryWriteCase::Unknown) == 0 &&
                   static_cast<unsigned>(HistoryWriteCase::InGap) == 1 && static_cast<unsigned>(HistoryWriteCase::Genuine) == 2 &&
                   !std::strcmp(historyWriteCaseName(0), "extent-unknown") && !std::strcmp(historyWriteCaseName(1), "in-gap") &&
                   !std::strcmp(historyWriteCaseName(2), "genuine") && !std::strcmp(historyWriteCaseName(3), "unknown"),
               "the three example cases carry their names in enum order, and two examples of each are kept");
        const HistoryWriteExtent whole = historyWholeWrite(HistoryWriteEntry::CopyResource);
        const HistoryWriteExtent ranged = historyRangedWrite(HistoryWriteEntry::Update, 4, 8);
        expect(!whole.ranged() && whole.entry == HistoryWriteEntry::CopyResource && whole.first == 0 && whole.end == ~uint64_t(0) &&
                   ranged.ranged() && ranged.entry == HistoryWriteEntry::Update && ranged.first == 4 && ranged.end == 8 && !HistoryWriteExtent{}.ranged(),
               "a write is whole unless it carries a range, and the default extent is whole");
    }

    // -- the key digest: every field of the key, and nothing else --
    {
        const auto base = [] {
            HistoryKey k;
            k.vs = reinterpret_cast<const void*>(0x10); k.layout = reinterpret_cast<const void*>(0x20);
            k.vertices = reinterpret_cast<const void*>(0x30); k.indices = reinterpret_cast<const void*>(0x40);
            k.count = 9; k.start = 3; k.offset = 64; k.stride = 32; k.indexOffset = 8; k.format = 57; k.base = 4;
            return k;
        };
        using Hash = uint64_t (*)(const HistoryKey&);
        const auto distinguishes = [&](Hash hash) {
            const uint64_t h0 = hash(base());
            if (hash(base()) != h0) return false;
            HistoryKey k[11] = {base(), base(), base(), base(), base(), base(), base(), base(), base(), base(), base()};
            k[0].vs = reinterpret_cast<const void*>(0x11); k[1].layout = reinterpret_cast<const void*>(0x21);
            k[2].vertices = reinterpret_cast<const void*>(0x31); k[3].indices = reinterpret_cast<const void*>(0x41);
            k[4].count = 12; k[5].start = 6; k[6].offset = 80; k[7].stride = 48; k[8].indexOffset = 12; k[9].format = 42; k[10].base = -4;
            for (const HistoryKey& other : k) if (hash(other) == h0) return false;
            return true;
        };
        expect(distinguishes(historyKeyHash), "the digest is stable and differs for a change of each of the key's eleven fields (a negative base included)");
        expect(!distinguishes([](const HistoryKey& k) { HistoryKey m = k; m.start = 0; return historyKeyHash(m); }),
               "mutation control: a digest that ignores the start fails the table");
        expect(!distinguishes([](const HistoryKey& k) { HistoryKey m = k; m.base = 0; return historyKeyHash(m); }),
               "mutation control: a digest that ignores the base fails the table");
        expect(!distinguishes([](const HistoryKey& k) { HistoryKey m = k; m.indexOffset = 0; return historyKeyHash(m); }),
               "mutation control: a digest that ignores the index buffer offset fails the table");
        expect(!distinguishes([](const HistoryKey& k) { HistoryKey m = k; m.indices = nullptr; return historyKeyHash(m); }),
               "mutation control: a digest that ignores the index buffer fails the table");
    }

    // -- the ledger's labels for a published key whose record is gone --
    {
        const auto key = [](unsigned count, unsigned start) {
            HistoryKey k;
            k.vs = reinterpret_cast<const void*>(0x10); k.layout = reinterpret_cast<const void*>(0x20);
            k.vertices = reinterpret_cast<const void*>(0x30); k.indices = reinterpret_cast<const void*>(0x40);
            k.count = count; k.start = start; k.stride = 32; k.format = 57;
            return k;
        };
        const auto published = [&](HistoryLedger& l, unsigned frame, const HistoryKey& k, bool publish = true) {
            const uint64_t token = l.note(frame, k, HistoryLedger::Captured);
            if (publish) l.markPublished(token);
        };
        const auto gap = [&](const HistoryLedger& l, const HistoryRecordFacts& f) { return l.classify(100, key(9, 0), f).gap; };
        HistoryLedger l;
        published(l, 99, key(9, 0));
        HistoryRecordFacts byAdvance, byPressure, unnamed, odd;
        byAdvance.goneBy = 1; byPressure.goneBy = 2; unnamed.goneBy = 0; odd.goneBy = 3;
        expect(gap(l, byAdvance) == HistoryGap::ReclaimedByAdvance, "published last frame, no record, taken at the frame boundary: reclaimed-by-advance");
        expect(gap(l, byPressure) == HistoryGap::ReclaimedByPressure, "published last frame, no record, taken for another key's budget: reclaimed-by-pressure");
        expect(gap(l, unnamed) == HistoryGap::RecordReclaimed, "published last frame, no record, and nothing remembers how: record-reclaimed");
        expect(gap(l, odd) == HistoryGap::RecordReclaimed, "a cause the ledger does not know is record-reclaimed, never one of the named labels");
        expect(!HistoryRecordFacts{}.goneBy, "the facts default to an unknown cause");
        // The new labels sit below what the classifier tests first, and only apply when no record of the exact key is present.
        HistoryRecordFacts present; present.exactPresent = true; present.goneBy = 2;
        expect(gap(l, present) == HistoryGap::RecordUnusable, "a record that is present is never reclaimed: the cause is ignored (the sentinel stays the sentinel)");
        HistoryRecordFacts invalidated = present; invalidated.exactInvalidated = true; invalidated.exactInvalidReasons = 4;
        expect(gap(l, invalidated) == HistoryGap::InvalidatedIndices, "a record found invalidated names the write, whatever the cause says");
        HistoryRecordFacts shifted = byPressure; shifted.shiftUsable = 1;
        expect(gap(l, shifted) == HistoryGap::ReclaimedByPressure, "evidence about the exact key (it was taken) outranks a record of the same mesh elsewhere");
        HistoryLedger unpublished;
        published(unpublished, 99, key(9, 0), false);
        expect(unpublished.classify(100, key(9, 0), byAdvance).gap == HistoryGap::UnpublishedLastFrame, "a key never published is not reclaimed, whatever the cause says");
        HistoryLedger refused;
        refused.note(99, key(9, 0), HistoryLedger::RefusedBudget);
        expect(refused.classify(100, key(9, 0), byPressure).gap == HistoryGap::RefusedBudget, "a key refused last frame is the refusal, whatever the cause says");
        HistoryLedger empty;
        expect(empty.classify(100, key(9, 0), byPressure).gap == HistoryGap::PreviousFrameEmpty, "with nothing offered the frame before, the cause is not asked");
        // The names the log reader greps.
        expect(!std::strcmp(historyGapName(HistoryGap::ReclaimedByAdvance), "reclaimed-by-advance") &&
                   !std::strcmp(historyGapName(HistoryGap::ReclaimedByPressure), "reclaimed-by-pressure") &&
                   !std::strcmp(historyGapName(HistoryGap::RecordReclaimed), "record-reclaimed"),
               "the two labels are named as the log reader greps them, and record-reclaimed keeps its name");
    }

    // -- HistoryWriteStats::add sums every counter (the struct is nothing but uint64_t, so every word is checked) --
    {
        static_assert(std::is_standard_layout<HistoryWriteStats>::value && sizeof(HistoryWriteStats) % sizeof(uint64_t) == 0,
                      "HistoryWriteStats must stay a plain run of uint64_t so the add() check below covers every field");
        static constexpr size_t words = sizeof(HistoryWriteStats) / sizeof(uint64_t);
        const auto fill = [](HistoryWriteStats& s, uint64_t seed) {
            uint64_t w[words];
            for (size_t i = 0; i < words; ++i) w[i] = seed + i * 3;
            std::memcpy(&s, w, sizeof(s));
        };
        const auto summed = [&](void (*add)(HistoryWriteStats&, const HistoryWriteStats&)) {
            HistoryWriteStats a, b;
            fill(a, 1000); fill(b, 777777);
            add(a, b);
            uint64_t w[words];
            std::memcpy(w, &a, sizeof(a));
            for (size_t i = 0; i < words; ++i) if (w[i] != 1000 + i * 3 + 777777 + i * 3) return false;
            return true;
        };
        expect(summed([](HistoryWriteStats& a, const HistoryWriteStats& b) { a.add(b); }), "add() sums every counter of the window's statistics");
        expect(!summed([](HistoryWriteStats& a, const HistoryWriteStats& b) {   // the mutation: one counter forgotten
                   HistoryWriteStats keep = a; a.add(b); a.deferredConservative = keep.deferredConservative; }),
               "mutation control: an add() that forgets the last counter fails the check");
        expect(!summed([](HistoryWriteStats& a, const HistoryWriteStats& b) {
                   HistoryWriteStats keep = a; a.add(b); a.extentFailed = keep.extentFailed; }),
               "mutation control: an add() that forgets a vertex-set counter fails the check");
        expect(!summed([](HistoryWriteStats& a, const HistoryWriteStats& b) {
                   HistoryWriteStats keep = a; a.add(b); a.setCancelled = keep.setCancelled; }),
               "mutation control: an add() that forgets the cancelled-read counter fails the check");
        expect(!summed([](HistoryWriteStats& a, const HistoryWriteStats& b) {
                   HistoryWriteStats keep = a; a.add(b); a.vertexInGap = keep.vertexInGap; }),
               "mutation control: an add() that forgets the in-gap counter fails the check");
        expect(!summed([](HistoryWriteStats& a, const HistoryWriteStats& b) {
                   HistoryWriteStats keep = a; a.add(b); a.vertexUnknown = keep.vertexUnknown; a.vertexOutside = keep.vertexOutside; a.vertexGenuine = keep.vertexGenuine; }),
               "mutation control: an add() that forgets the unknown, outside and genuine counters fails the check");
        expect(!summed([](HistoryWriteStats& a, const HistoryWriteStats& b) {
                   HistoryWriteStats keep = a; a.add(b); a.deferredInvalidated = keep.deferredInvalidated; a.setFromCache = keep.setFromCache; a.setPending = keep.setPending; }),
               "mutation control: an add() that forgets the deferred, from-cache or pending counters fails the check");
        expect(!summed([](HistoryWriteStats& a, const HistoryWriteStats& b) {
                   HistoryWriteStats keep = a; a.add(b); a.erased[1] = keep.erased[1]; }),
               "mutation control: an add() that forgets an erase path fails the check");
        expect(!summed([](HistoryWriteStats& a, const HistoryWriteStats& b) {
                   HistoryWriteStats keep = a; a.add(b); a.invalidating[7][1][1] = keep.invalidating[7][1][1]; }),
               "mutation control: an add() that forgets one cell of the last entry fails the check");
    }

    // -- the no-candidate line carries the history's gauges and allocations --
    {
        FlatNoCandidateWindow w;
        w.records = 97; w.bytes = 123456; w.peakRecords = 128; w.peakBytes = 7654321; w.allocations = 4242;
        char line[4096];
        // The line is printed in parts (kFlatNoCandidateLineParts): the head carries the gauges, the patterns follow; the parts are read together.
        const auto joined = [&](const FlatNoCandidateWindow& window) {
            std::string all;
            for (unsigned part = 0; part < kFlatNoCandidateLineParts; ++part) { flatNoCandidateLine(line, sizeof(line), window, part); all += line; all += '\n'; }
            return all;
        };
        const int n = flatNoCandidateLine(line, sizeof(line), w);
        const std::string text(line);
        expect(n > 0 && n < static_cast<int>(sizeof(line)) - 1 &&
                   text.find("records=97 bytes=123456 peak-records=128 peak-bytes=7654321 allocations=4242") != std::string::npos,
               "the no-candidate line carries records, bytes, the peaks and the allocations, in the order the reader parses them");
        flatNoCandidateLine(line, sizeof(line), FlatNoCandidateWindow{});
        expect(std::string(line).find(" records=0 bytes=0 peak-records=0 peak-bytes=0 allocations=0") != std::string::npos,
               "an empty window prints the gauges and the allocations at zero");
        FlatNoCandidateWindow patterns;
        patterns.missBy[static_cast<unsigned>(HistoryGap::ReclaimedByAdvance)] = 11;
        patterns.missBy[static_cast<unsigned>(HistoryGap::ReclaimedByPressure)] = 22;
        const std::string patternText = joined(patterns);
        expect(patternText.find(" reclaimed-by-advance=11") != std::string::npos && patternText.find(" reclaimed-by-pressure=22") != std::string::npos &&
                   patternText.find("same-key-misses=33") != std::string::npos,
               "the two new patterns are on the line with their counts and are part of the same-key misses");
    }
    return failures;
}

inline int flatRangeInvalidateWiringTests() {
    using namespace edvr;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: range-invalidate wiring %s\n", name); ++failures; }
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
    // A top-level function ends at the first closing brace in column 0.
    const auto body = [](const std::string& text, const char* signature) {
        const size_t at = text.find(signature);
        if (at == std::string::npos) return std::string();
        const size_t lf = text.find("\n}\n", at), crlf = text.find("\n}\r\n", at);
        const size_t end = (std::min)(lf, crlf);
        return end == std::string::npos ? std::string() :
            text.substr(at, end + (end == crlf ? 4 : 3) - at);
    };
    expect(compact(body("void hook(){\nnotify();\n}\nvoid later(){\nother();\n}\n", "void hook(")) ==
               "voidhook(){notify();}" &&
           compact(body("void hook(){\r\nnotify();\r\n}\r\nvoid later(){\r\nother();\r\n}\r\n", "void hook(")) ==
               "voidhook(){notify();}" &&
           body("void hook(){\nnotify();\n", "void hook(").empty(),
           "hook extraction accepts LF and CRLF and refuses an unterminated body");
    // A member function of a class whose members are indented four spaces ends at the first line that is four spaces and a brace.
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
    const auto ordered = [&](const std::string& compacted, std::initializer_list<const char*> needles) {
        size_t pos = 0;
        bool ok = !compacted.empty();
        for (const char* needle : needles) {
            const size_t at = compacted.find(compact(needle), pos);
            if (at == std::string::npos) { ok = false; break; }
            pos = at + compact(needle).size();
        }
        return ok;
    };
    const auto has = [&](const std::string& compacted, const char* needle) { return !compacted.empty() && compacted.find(compact(needle)) != std::string::npos; };
    const auto without = [&](std::string text, const char* needle) {
        const std::string n = compact(needle);
        const size_t at = text.find(n);
        if (at != std::string::npos) text.erase(at, n.size());
        return text;
    };
    const auto replaced = [&](std::string text, const char* from, const char* to) {
        const std::string f = compact(from);
        const size_t at = text.find(f);
        if (at != std::string::npos) text.replace(at, f.size(), compact(to));
        return text;
    };

    const std::string historySource = slurp("src/d3d11/animated_vertex_history.h");
    const std::string writesSource = slurp("src/d3d11/animated_history_writes.h");
    const std::string motionSource = slurp("src/d3d11/flat_foreground_motion.h");
    const std::string runtimeSource = slurp("src/d3d11/flat_runtime.cpp");
    const std::string vscreenSource = slurp("src/d3d11/vscreen.cpp");
    expect(!historySource.empty() && !writesSource.empty() && !motionSource.empty() && !runtimeSource.empty() && !vscreenSource.empty(),
           "the sources are readable from the repo root");

    // -- the history: a write with a range invalidates only the records it touched, and keeps its reasons for the ledger --
    const std::string written = compact(member(historySource, "unsigned resourceWritten("));
    const auto writtenValid = [&](const std::string& text) {
        return ordered(text, {"const bool ranged=resource&&end!=~uint64_t(0);", "if(tally)++writeStats_.observed[e];",
                              "if(!resource||isIndexBuffer(resource))dropSets(resource,first,end);",
                              "for(auto&r:records_){",
                              "if(ranged)hit=meets(r.geometry,r.vset.get(),resource,first,end,vcase);",
                              "if(r.vset&&(!resource||(isI&&(!ranged||historyRangesOverlap(first,end,r.ibFirst,r.ibEnd)))))r.vset.reset();",
                              "if(!hit){", "if(live){", "++spared;",
                              "if(vcase==1&&!deferredLogged){logDeferred(resource,first,end);deferredLogged=true;}",
                              "const unsigned why=!resource?1:isV?2:4;",
                              "if(live){++hitRecords;hitV+=isV;hitI+=isI&&!isV;r.invalidatedAt=lastFrame_;",
                              "r.invalidated=true;r.invalidReasons|=why;", "if(!tally)return reasons;", "++writeStats_.touching[e]"});
    };
    expect(writtenValid(written), "a ranged write that misses a record spares it before anything is invalidated; a spare on an unknown set is logged; a hit keeps its reason and stamps its frame");
    expect(!writtenValid(without(written, "if(ranged)hit=meets(r.geometry,r.vset.get(),resource,first,end,vcase);")),
           "mutation control: a write that never consults the range fails the wiring");
    expect(!writtenValid(replaced(written, "const bool ranged=resource&&end!=~uint64_t(0);", "const bool ranged=end!=~uint64_t(0);")),
           "mutation control: a range on a null resource (which can spare nothing) fails the wiring");
    expect(!writtenValid(replaced(written, "r.vset.get(),resource,first,end,vcase", "nullptr,resource,first,end,vcase")),
           "mutation control: a write judged against no set (every vertex write spared as unknown, or the old whole-buffer rule) fails the wiring");
    expect(!writtenValid(without(written, "r.invalidated=true;r.invalidReasons|=why;")), "mutation control: an invalidation that drops the write's reason fails the wiring");
    expect(!writtenValid(without(written, "r.invalidatedAt=lastFrame_;")), "mutation control: an invalidation that does not stamp its frame (the keep window's start) fails the wiring");
    expect(!writtenValid(without(written, "if(!tally)return reasons;")), "mutation control: a second notification of one write that is counted again fails the wiring");
    expect(!writtenValid(without(written, "if(tally)++writeStats_.observed[e];")), "mutation control: a write that is never observed fails the wiring");
    expect(!writtenValid(without(written, "if(!resource||isIndexBuffer(resource))dropSets(resource,first,end);")),
           "mutation control: a write to the indices that does not end the sets read from them fails the wiring");
    expect(!writtenValid(replaced(written, "if(!resource||isIndexBuffer(resource))dropSets(", "if(isIndexBuffer(resource))dropSets(")),
           "mutation control: the write nothing could name (a null resource) that leaves the sets standing fails the wiring");
    expect(!writtenValid(without(written, "if(r.vset&&(!resource||(isI&&(!ranged||historyRangesOverlap(first,end,r.ibFirst,r.ibEnd)))))r.vset.reset();")),
           "mutation control: a record that keeps its set across a write to its indices fails the wiring");
    expect(!writtenValid(replaced(written, "if(r.vset&&(!resource||(isI&&(!ranged||historyRangesOverlap(first,end,r.ibFirst,r.ibEnd)))))r.vset.reset();",
                                  "if(r.vset&&(!resource||isV||isI))r.vset.reset();")),
           "mutation control: a record that forgets its set on any write to either buffer (a vertex write included) fails the wiring");
    expect(!writtenValid(replaced(written, "if(r.vset&&(!resource||(isI&&(!ranged||historyRangesOverlap(first,end,r.ibFirst,r.ibEnd)))))r.vset.reset();",
                                  "if(r.vset&&(!resource||isI))r.vset.reset();")),
           "mutation control: a record that forgets its set on a write to the indices that did not touch its range fails the wiring");
    expect(!writtenValid(without(written, "if(vcase==1&&!deferredLogged){logDeferred(resource,first,end);deferredLogged=true;}")),
           "mutation control: a write spared on an unknown set that is not kept for the check fails the wiring");
    expect(!writtenValid(replaced(written, "if(vcase==1&&!deferredLogged)", "if(vcase&&!deferredLogged)")),
           "mutation control: a write kept for the check whatever it met (gap and outside writes fill the log) fails the wiring");
    // -- the frame boundary keeps an invalidated record for its key, under the extended policy only --
    const std::string advance = compact(member(historySource, "void advance(unsigned frame)"));
    const auto advanceValid = [&](const std::string& text) {
        return ordered(text, {"for(auto it=records_.begin();it!=records_.end();)",
                              "if((it->frame[0]==~0u||frame-it->frame[0]>2)&&(it->frame[1]==~0u||frame-it->frame[1]>2)&&"
                              "(!retainInvalid_||!it->invalidated||frame-it->invalidatedAt>kInvalidKeepFrames)){",
                              "noteErased(*it,it->invalidated?HistoryErase::AdvanceInvalidated:HistoryErase::AdvanceAged,frame);",
                              "bytes_-=it->geometry.count*32;it=records_.erase(it);"});
    };
    expect(advanceValid(advance), "the boundary erases a record that has aged out, or an invalidated one only when the policy does not retain it or the keep window has passed");
    expect(!advanceValid(replaced(advance, "&&(!retainInvalid_||!it->invalidated||frame-it->invalidatedAt>kInvalidKeepFrames)){", "){")),
           "mutation control: a boundary that erases an invalidated record at once under every policy fails the wiring");
    expect(!advanceValid(replaced(advance, "(!retainInvalid_||!it->invalidated||", "(!it->invalidated||")),
           "mutation control: a boundary that keeps invalidated records under the default policy too fails the wiring");
    expect(!advanceValid(replaced(advance, "(!retainInvalid_||!it->invalidated||frame-it->invalidatedAt>kInvalidKeepFrames)", "(retainInvalid_||!it->invalidated||frame-it->invalidatedAt>kInvalidKeepFrames)")),
           "mutation control: a boundary that keeps invalidated records only under the default policy fails the wiring");
    expect(!advanceValid(without(advance, "noteErased(*it,it->invalidated?HistoryErase::AdvanceInvalidated:HistoryErase::AdvanceAged,frame);")),
           "mutation control: a boundary erase that is neither counted nor remembered fails the wiring");
    const std::string historyCompact = compact(historySource);
    expect(count(historyCompact, compact("kInvalidKeepFrames=3,")) == 1 && count(historyCompact, compact("if(extended)retainInvalid_=true;")) == 1 &&
               count(historyCompact, compact("retainInvalid_=true")) == 1,
           "the keep window is three frames and only an extended capture turns retention on");
    expect(count(replaced(historyCompact, "kInvalidKeepFrames=3,", "kInvalidKeepFrames=0,"), compact("kInvalidKeepFrames=3,")) == 0 &&
               count(replaced(historyCompact, "if(extended)retainInvalid_=true;", "retainInvalid_=true;"), compact("if(extended)retainInvalid_=true;")) == 0,
           "mutation control: a changed keep window, or retention turned on for every policy, is not what the text says");
    expect(has(historyCompact, "bool retainIndex=false,bool extended=false)") && has(historyCompact, "if(!prepareCapture(ctx,count,instances,start,base,startInstance,frame,out,extended))return false;"),
           "the default capture is the default policy (VR), and the policy reaches prepareCapture");

    // -- every erase path is counted and remembered; the ledger asks how a key's record went --
    expect(count(historyCompact, "records_.erase(") == 2 && count(historyCompact, "noteErased(") == 3,
           "the history erases records in two places (the boundary and the reclaim) and notes both, so no erase is unlabelled");
    expect(count(historyCompact + compact("records_.erase(records_.begin());"), "records_.erase(") == 3,
           "mutation control: a third erase path is what the count would see");
    const std::string erasedNote = compact(member(historySource, "void noteErased("));
    expect(has(erasedNote, "++writeStats_.erased[unsigned(why)];") && has(erasedNote, "t.key=historyKeyHash(historyKeyOf(r.geometry));t.frame=frame;") &&
               has(erasedNote, "t.cause=(why==HistoryErase::AdvanceInvalidated||why==HistoryErase::AdvanceAged)?1:2;"),
           "an erase is counted by path and remembered by the key's digest, the boundary as cause 1 and pressure as cause 2");
    expect(!has(replaced(erasedNote, "?1:2;", "?2:1;"), "t.cause=(why==HistoryErase::AdvanceInvalidated||why==HistoryErase::AdvanceAged)?1:2;"),
           "mutation control: swapped causes are not what the text says");
    const std::string gone = compact(member(historySource, "unsigned goneBy("));
    expect(has(gone, "constuint64_thash=historyKeyHash(key);") && has(gone, "if(frame-t.frame>2)continue;") && has(gone, "if(t.key==hash)return t.cause;"),
           "the cause is asked by the draw's own key digest, for erases of the last two frames");
    const std::string prepare = compact(member(historySource, "bool prepareCapture("));
    const auto prepareValid = [&](const std::string& text) {
        return ordered(text, {"if(extended&&priorCount==0){", "out.missed=true;", "if(!facts.exactPresent)facts.goneBy=goneBy(ledgerKey,frame);",
                              "out.miss=ledger_.classify(frame,ledgerKey,facts);"}) &&
            ordered(text, {"noteErased(*it,it->invalidated?HistoryErase::PressureInvalidated:HistoryErase::PressureSpent,frame);", "it=records_.erase(it);"}) &&
            ordered(text, {"found->invalidated=false;found->invalidReasons=0;"}) && count(text, "vset") == 0 && count(text, "extentState") == 0;
    };
    expect(prepareValid(prepare), "a draw that missed asks how its record went before it is classified, the reclaim notes what it takes, and a revived record is not touched but for its flag: it keeps its set");
    expect(!prepareValid(without(prepare, "if(!facts.exactPresent)facts.goneBy=goneBy(ledgerKey,frame);")), "mutation control: a classification that never asks how the record went fails the wiring");
    expect(!prepareValid(without(prepare, "noteErased(*it,it->invalidated?HistoryErase::PressureInvalidated:HistoryErase::PressureSpent,frame);")),
           "mutation control: a reclaim that takes records without noting them fails the wiring");
    expect(!prepareValid(replaced(prepare, "found->invalidated=false;found->invalidReasons=0;", "if(found->invalidated)found->vset.reset();found->invalidated=false;found->invalidReasons=0;")),
           "mutation control: a revived record that forgets its set (and asks the device for it again, every frame the loop repeats) fails the wiring");
    expect(has(prepare, "indexRange(key,record.ibFirst,record.ibEnd);record.bornFrame=frame;") && has(prepare, "if(extended)out.ledgerToken=ledger_.note(frame,ledgerKey,HistoryLedger::Captured);"),
           "a new record is given the exact index range of its key and the frame it was made in (the age an example names)");
    expect(!has(without(prepare, "record.bornFrame=frame;"), "indexRange(key,record.ibFirst,record.ibEnd);record.bornFrame=frame;"),
           "mutation control: a record that is not given its birth frame is not what the text says");

    // -- the index range the index role is judged by --
    const std::string rangeOf = compact(member(historySource, "static void indexRange("));
    expect(has(rangeOf, "constuint64_tsize=g.format==DXGI_FORMAT_R16_UINT?2u:4u;") && has(rangeOf, "first=uint64_t(g.indexOffset)+uint64_t(g.start)*size;end=first+uint64_t(g.count)*size;"),
           "the index range is in bytes from the buffer offset, in two- or four-byte indices");
    expect(!has(replaced(rangeOf, "uint64_t(g.indexOffset)+", ""), "first=uint64_t(g.indexOffset)+uint64_t(g.start)*size;") && !has(replaced(rangeOf, "?2u:4u", "?4u:4u"), "?2u:4u"),
           "mutation control: an index range with no buffer offset, or in four-byte units for every format, is not what the text says");

    // -- the adapter: the draws its lists keep are the ones the write did not touch; the write is filed by where it falls --
    const std::string adapter = compact(member(motionSource, "void resourceWritten("));
    const auto adapterValid = [&](const std::string& text) {
        return ordered(text, {"const HistoryWriteTiming timing=(!current_.empty()&&hFrame_!=frame_)?HistoryWriteTiming::Window:HistoryWriteTiming::Gap;",
                              "history_.resourceWritten(resource,extent.first,extent.end,extent.entry,timing,tally);",
                              "if(!kind)return;", "return history_.captureHitBy(d.capture,resource,extent.first,extent.end);",
                              "erase(previous_);", "erase(current_);", "if(kind&1)fail(\"foreground-unknown-resource-write\");",
                              "else if(before!=current_.size())fail(\"foreground-captured-geometry-written\");"});
    };
    expect(adapterValid(adapter), "the adapter files a write by the frame's window, hands the history its range and tally, and drops only the draws it touched");
    expect(!adapterValid(replaced(adapter, "(!current_.empty()&&hFrame_!=frame_)", "(false)")), "mutation control: a write that is never in the window fails the wiring");
    expect(!adapterValid(replaced(adapter, "extent.entry,timing,tally);", "extent.entry,timing,true);")), "mutation control: a second notification counted again fails the wiring");
    expect(!adapterValid(replaced(adapter, "return history_.captureHitBy(d.capture,resource,extent.first,extent.end);",
                                  "return !resource||d.capture.geometry.vertices.Get()==resource||d.capture.geometry.indices.Get()==resource;")),
           "mutation control: dropping every draw that reads the written buffer, range or not, fails the wiring");
    expect(!adapterValid(without(adapter, "else if(before!=current_.size())fail(\"foreground-captured-geometry-written\");")),
           "mutation control: a current draw touched that does not fail the frame fails the wiring");
    const std::string prepareH = compact(member(motionSource, "bool prepareH("));
    expect(has(prepareH, "out=Output{};out.frame=frame;hFrame_=frame;") && !has(replaced(prepareH, "hFrame_=frame;", ""), "out=Output{};out.frame=frame;hFrame_=frame;"),
           "H marks its frame first, before any refusal, so a write after a refused H is in the gap");
    const std::string captureM = compact(member(motionSource, "bool capture("));
    expect(has(captureM, "history_.pollExtents(ctx,frame);") && has(captureM, "history_.capture(ctx,draw,count,instances,start,base,startInstance,frame,d.capture,true,true)"),
           "each frame's first capture reads the vertex extents that are ready, and every capture asks the history for the extended policy and the retained index");
    expect(!has(replaced(captureM, "history_.pollExtents(ctx,frame);", ""), "history_.pollExtents(ctx,frame);"), "mutation control: a capture that never polls extents is not what the text says");
    const std::string motionCompact = compact(motionSource);
    expect(has(motionCompact, "mutable CaptureStats stats_{};") && has(motionCompact, "const CaptureStats& stats() const{stats_.history=history_.writeStats();"),
           "the adapter's statistics carry the history's write instrument");
    expect(!has(without(motionCompact, "stats_.history=history_.writeStats();"), "const CaptureStats& stats() const{stats_.history=history_.writeStats();"),
           "mutation control: statistics that never copy the history's counters are not what the text says");

    // -- the hooks: the extents the game's calls carry reach the adapter, and every write is counted once --
    const std::string overlay = compact(body(runtimeSource, "void flatRuntimeOverlayResourceMutation("));
    expect(count(overlay, compact("domainResourceWritten(state(),resource,\"foreground-depth-or-unknown-mutation\",mutationExtent(op,details),true);")) == 1,
           "the API-level mutation report counts the write, with the bytes the call carried");
    expect(count(overlay, compact("mutationExtent(op,details),true")) == 1 && count(replaced(overlay, "mutationExtent(op,details),true", "mutationExtent(op,details),false"), compact("mutationExtent(op,details),true")) == 0,
           "mutation control: a report that stops counting is not what the text says");
    const std::string prefix = compact(body(runtimeSource, "static void resourceWritten(State& s, ID3D11Resource* res,const char* entry,"));
    expect(count(prefix, compact("domainResourceWritten(s,res,\"foreground-depth-or-unknown-mutation\",extent,false);")) == 1,
           "the prefix model's own notification invalidates with the same extent and does not count it again");
    expect(count(replaced(prefix, "extent,false);", "extent,true);"), compact("extent,false);")) == 0, "mutation control: a notification that counts again is not what the text says");
    const std::string domain = compact(body(runtimeSource, "static void domainResourceWritten("));
    expect(has(domain, "candidate.motion.resourceWritten(resource,extent,tally);") && has(domain, "const HistoryWriteExtent& extent=HistoryWriteExtent{},bool tally=true)"),
           "the domain forwards the extent and the tally to every candidate's adapter, and counts by default");
    const std::string mutation = compact(body(runtimeSource, "static HistoryWriteExtent mutationExtent("));
    const auto mutationValid = [&](const std::string& text) {
        return ordered(text, {"caseFlatOverlayMutationOp::Map:caseFlatOverlayMutationOp::Unmap:returnhistoryWholeWrite(HistoryWriteEntry::Map);",
                              "caseFlatOverlayMutationOp::ClearRtv:caseFlatOverlayMutationOp::ClearDsv:caseFlatOverlayMutationOp::ClearUav:returnhistoryWholeWrite(HistoryWriteEntry::Clear);",
                              "caseFlatOverlayMutationOp::CopyResource:returnhistoryWholeWrite(HistoryWriteEntry::CopyResource);",
                              "caseFlatOverlayMutationOp::CopyRegion:returnflatRuntimeCopyExtent(d.dstSub,d.dstX,d.source,d.hasBox?&d.box:nullptr);",
                              "caseFlatOverlayMutationOp::UpdateSubresource:returnflatRuntimeUpdateExtent(d.dstSub,d.hasBox?&d.box:nullptr);",
                              "default:returnhistoryWholeWrite(HistoryWriteEntry::Other);"});
    };
    expect(mutationValid(mutation), "the mutation report maps Map, Clear and CopyResource to whole writes, a region copy and an update to their ranges, and anything else to whole");
    expect(!mutationValid(replaced(mutation, "returnflatRuntimeUpdateExtent(d.dstSub,d.hasBox?&d.box:nullptr);", "returnhistoryWholeWrite(HistoryWriteEntry::Update);")),
           "mutation control: an update reported as a whole write (no range ever reaches the history) fails the wiring");
    expect(!mutationValid(replaced(mutation, "returnflatRuntimeCopyExtent(d.dstSub,d.dstX,d.source,d.hasBox?&d.box:nullptr);", "returnhistoryWholeWrite(HistoryWriteEntry::CopyRegion);")),
           "mutation control: a region copy reported as a whole write fails the wiring");
    expect(!mutationValid(replaced(mutation, "default:returnhistoryWholeWrite(HistoryWriteEntry::Other);", "default:returnhistoryWholeWrite(HistoryWriteEntry::Clear);")),
           "mutation control: an unnamed write filed under clear fails the wiring");
    const std::string copyExtent = compact(body(runtimeSource, "HistoryWriteExtent flatRuntimeCopyExtent("));
    const auto copyValid = [&](const std::string& text) {
        return ordered(text, {"if(dstSub!=0)returnhistoryWholeWrite(HistoryWriteEntry::CopyRegion);", "if(box){",
                              "if(box->right<=box->left)returnhistoryWholeWrite(HistoryWriteEntry::CopyRegion);",
                              "returnhistoryRangedWrite(HistoryWriteEntry::CopyRegion,dstX,uint64_t(dstX)+(box->right-box->left));}",
                              "constuint64_tbytes=flatBufferBytes(src);",
                              "returnbytes?historyRangedWrite(HistoryWriteEntry::CopyRegion,dstX,uint64_t(dstX)+bytes):historyWholeWrite(HistoryWriteEntry::CopyRegion);"});
    };
    expect(copyValid(copyExtent), "a region copy writes [dstX, dstX + box width) of a buffer, the source's whole size with no box, and the whole resource for another subresource");
    expect(!copyValid(replaced(copyExtent, "historyRangedWrite(HistoryWriteEntry::CopyRegion,dstX,uint64_t(dstX)+(box->right-box->left))", "historyRangedWrite(HistoryWriteEntry::CopyRegion,box->left,box->right)")),
           "mutation control: a copy range taken from the source box instead of the destination offset fails the wiring");
    expect(!copyValid(without(copyExtent, "if(dstSub!=0)returnhistoryWholeWrite(HistoryWriteEntry::CopyRegion);")),
           "mutation control: a copy into a subresource other than zero given a range fails the wiring");
    expect(!copyValid(replaced(copyExtent, "returnbytes?historyRangedWrite(", "returnhistoryRangedWrite(")),
           "mutation control: a boxless copy from a source with no buffer size given a range fails the wiring");
    const std::string updateExtent = compact(body(runtimeSource, "HistoryWriteExtent flatRuntimeUpdateExtent("));
    const auto updateValid = [&](const std::string& text) {
        return ordered(text, {"if(dstSub!=0||!box||box->right<=box->left)returnhistoryWholeWrite(HistoryWriteEntry::Update);",
                              "returnhistoryRangedWrite(HistoryWriteEntry::Update,box->left,box->right);"});
    };
    expect(updateValid(updateExtent), "an update with a box writes [left, right) of a buffer; with no box, an empty one or another subresource, the whole resource");
    expect(!updateValid(without(updateExtent, "||!box")) && !updateValid(replaced(updateExtent, "box->right<=box->left", "box->right<box->left")),
           "mutation control: an update with no box (or an empty one) given a range fails the wiring");
    const std::string bufferBytes = compact(body(runtimeSource, "static uint64_t flatBufferBytes("));
    expect(has(bufferBytes, "if(dim!=D3D11_RESOURCE_DIMENSION_BUFFER)return0;") && has(bufferBytes, "returnd.ByteWidth;"),
           "a copy's source size is a buffer's byte width and nothing for a texture");
    const std::string updateHook = compact(body(runtimeSource, "void flatRuntimeUpdate("));
    expect(count(updateHook, compact("resourceWritten(state(), res,\"flatRuntimeUpdate\",FlatOverlayMutationOp::Written,flatRuntimeUpdateExtent(0,box));")) == 1,
           "UpdateSubresource's box reaches the history through flatRuntimeUpdateExtent");
    expect(count(replaced(updateHook, "flatRuntimeUpdateExtent(0,box)", "historyWholeWrite(HistoryWriteEntry::Update)"), compact("flatRuntimeUpdateExtent(0,box)")) == 0,
           "mutation control: an update that drops its box on the way to the history is not what the text says");
    expect(has(compact(body(runtimeSource, "void flatRuntimeMap(")), "resourceWritten(state(), res,\"flatRuntimeMap\",FlatOverlayMutationOp::Written,historyWholeWrite(HistoryWriteEntry::Map));"),
           "a map is a whole-resource write");
    expect(has(compact(body(runtimeSource, "void flatRuntimeWrittenExtent(")), "resourceWritten(state(), res,\"flatRuntimeWritten\",FlatOverlayMutationOp::Written,extent);") &&
               has(compact(body(runtimeSource, "void flatRuntimeWritten(ID3D11Resource* res,FlatOverlayMutationOp provenance)")), "historyWholeWrite(clear?HistoryWriteEntry::Clear:HistoryWriteEntry::Other)"),
           "a write with a known extent is passed on as it is, one without is a whole clear or an unnamed whole write");
    expect(has(compact(runtimeSource), "domainResourceWritten(s,u.Get(),\"foreground-dispatch-depth-write\",historyWholeWrite(HistoryWriteEntry::Dispatch));"),
           "a compute dispatch's UAV is a whole-resource write, counted at the dispatch (no API-level report covers it)");

    // -- the hooks in the game's calls --
    const std::string copyResource = compact(body(vscreenSource, "void STDMETHODCALLTYPE hookedCopyResource("));
    const std::string copyRegion = compact(body(vscreenSource, "void STDMETHODCALLTYPE hookedCopySubresourceRegion("));
    expect(count(copyResource, compact("flatRuntimeWrittenExtent(dst,historyWholeWrite(HistoryWriteEntry::CopyResource));")) == 1 && !has(copyResource, "flatRuntimeWritten(dst);"),
           "CopyResource tells the flat runtime it wrote the whole destination");
    expect(count(copyRegion, compact("flatRuntimeWrittenExtent(dst,flatRuntimeCopyExtent(dstSub,dstX,src,box));")) == 1 && !has(copyRegion, "flatRuntimeWritten(dst);"),
           "CopySubresourceRegion tells the flat runtime the bytes its destination offset and box name");
    expect(count(replaced(copyRegion, "flatRuntimeCopyExtent(dstSub,dstX,src,box)", "historyWholeWrite(HistoryWriteEntry::CopyRegion)"), compact("flatRuntimeCopyExtent(dstSub,dstX,src,box)")) == 0,
           "mutation control: a region copy that drops its extent on the way to the runtime is not what the text says");
    const std::string update = compact(body(vscreenSource, "void STDMETHODCALLTYPE hookedUpdateSubresource("));
    expect(ordered(update, {"flatRuntimeOverlayResourceMutation(dst,FlatOverlayMutationOp::UpdateSubresource,detail);", "flatRuntimeUpdate(dst,data,box);"}) &&
               has(update, "FlatMutationDetails::transfer(FlatOverlayMutationOp::UpdateSubresource,\"UpdateSubresource\",nullptr,0,dstSub,box);"),
           "UpdateSubresource reports its box to the mutation report and to the runtime");

    // -- the log: the line is written next to the others, from the window's difference --
    const std::string report = compact(body(runtimeSource, "static void reportForegroundNoCandidate("));
    const auto reportValid = [&](const std::string& text) {
        return ordered(text, {"history.writes=historyWindowDelta(captures.history,was.history);", "takeHistoryPeaks(peakRecords,peakBytes);",
                              "takeWriteTop(tops+topCount,kHistoryTopResources);", "w.records=history.records;w.bytes=history.bytes;w.peakRecords=history.peakRecords;w.peakBytes=history.peakBytes;",
                              "s.foregroundMissReported=captures;",
                              "for(unsigned part=0;part<kFlatNoCandidateLineParts;++part){flatNoCandidateLine(line,sizeof(line),w,part);Log::get().note(\"%s\",line);}",
                              "for(unsigned part=0;part<kFlatSiblingLineParts;++part){flatSiblingLine(line,sizeof(line),sibling,part);Log::get().note(\"%s\",line);}",
                              "flatHistoryLine(line,sizeof(line),history);Log::get().note(\"%s\",line);"}) &&
            has(text, "w.allocations=delta(captures.history.allocations,was.history.allocations);");
    };
    expect(reportValid(report), "the window's history line is printed with the others from the difference of the cumulative counters, and the no-candidate line gets the gauges and allocations");
    expect(!reportValid(without(report, "flatHistoryLine(line,sizeof(line),history);Log::get().note(\"%s\",line);")), "mutation control: a history line that is never printed fails the wiring");
    expect(!reportValid(without(report, "history.writes=historyWindowDelta(captures.history,was.history);")), "mutation control: a window that is the cumulative counters, not their difference, fails the wiring");
    expect(!reportValid(without(report, "w.allocations=delta(captures.history.allocations,was.history.allocations);")), "mutation control: a no-candidate line without its allocations fails the wiring");
    // historyWindowDelta must carry every counter the struct has: a field added to HistoryWriteStats and forgotten there fails here, not in a log.
    const std::string delta = compact(body(runtimeSource, "static HistoryWriteStats historyWindowDelta("));
    std::string fields;
    {
        const size_t at = writesSource.find("struct HistoryWriteStats {");
        const size_t end = writesSource.find("void add(", at);
        const std::string text = at == std::string::npos || end == std::string::npos ? std::string() : writesSource.substr(at, end - at);
        size_t pos = 0;
        while ((pos = text.find("uint64_t ", pos)) != std::string::npos) {
            pos += 9;
            const size_t semi = text.find(';', pos);
            std::string decl = text.substr(pos, semi - pos);
            size_t start = 0;
            while (start <= decl.size()) {
                size_t comma = decl.find(',', start);
                if (comma == std::string::npos) comma = decl.size();
                std::string item = decl.substr(start, comma - start);
                size_t b = item.find_first_not_of(" \t\r\n");
                size_t e = item.find_first_of("[= \t\r\n", b == std::string::npos ? 0 : b);
                if (b != std::string::npos) fields += item.substr(b, e == std::string::npos ? std::string::npos : e - b) + ",";
                start = comma + 1;
            }
            pos = semi == std::string::npos ? text.size() : semi;
        }
    }
    const std::string addText = compact(member(writesSource, "void add("));
    // The token as a whole identifier path: "w.extentFailed" is not inside "now.extentFailed".
    const auto hasToken = [](const std::string& text, const std::string& token) {
        const auto identifier = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        for (size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + 1)) {
            const size_t after = at + token.size();
            if ((at == 0 || !identifier(text[at - 1])) && (after >= text.size() || !identifier(text[after]))) return true;
        }
        return false;
    };
    const auto carries = [&](const std::string& text, const char* prefix) {
        if (fields.empty() || text.empty()) return false;
        for (size_t start = 0; start < fields.size();) {
            const size_t comma = fields.find(',', start);
            const std::string name = fields.substr(start, comma - start);
            start = comma + 1;
            if (!hasToken(text, prefix + name)) return false;
        }
        return true;
    };
    unsigned fieldCount = 0;
    for (char c : fields) fieldCount += c == ',';
    expect(fieldCount == 24 && carries(delta, "w.") && carries(addText, "o."),
           "the window delta and add() each carry all twenty-four counters of the statistics struct, by name");
    expect(!carries(replaced(delta, "w.extentFailed", "w.nothing"), "w.") && !carries(replaced(addText, "o.extentRead", "o.nothing"), "o.") &&
               !carries(replaced(delta, "w.allocationFailures", "w.allocations"), "w."),
           "mutation control: a delta or an add() that forgets one counter fails the check");
    expect(!carries(replaced(delta, "w.deferredConservative", "w.nothing"), "w.") && !carries(replaced(addText, "o.setCancelled", "o.nothing"), "o.") &&
               !carries(replaced(delta, "w.vertexInGap", "w.nothing"), "w.") && !carries(replaced(addText, "o.vertexGenuine", "o.nothing"), "o.") &&
               !carries(replaced(delta, "w.setFromCache", "w.nothing"), "w.") && !carries(replaced(delta, "w.deferredInvalidated", "w.deferredConservative"), "w."),
           "mutation control: a delta or an add() that forgets one of the vertex-set or three-way counters fails the check");
    return failures;
}
