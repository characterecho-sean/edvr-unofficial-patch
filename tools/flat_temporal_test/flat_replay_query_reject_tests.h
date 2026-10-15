#pragma once

// The flat foreground replay-safety instrument (src\d3d11\flat_replay_query_reject.h; wiring in flat_runtime.cpp). Log only: when
// replayQueriesSafe(ctx) refuses a foreground draw (stage=state reason=foreground-active-query, the 09:34:54-09:37:34 on-foot refusals of
// 2026-10-14), the log names the open query kind(s), the draw's VS/PS, the target size, the frame and the AA mode: the first 8 one by one, then a
// 5 s window line counted per (query kind, VS, PS) that is printed every window, so "never ran" (no line), "never reached" (checked=0) and "no
// rejection" (rejected=0, checked>0) read differently.
//
//   1. the words: code names, predicate marking, the event line character for character, the window line, both under the line budget at the worst digits;
//   2. the counting: the first eight ordinals, the window's per-key counts and order, the overflow, the reset, the session totals;
//   3. the wiring: source pins in flat_runtime.cpp (the decision is the same expression it was; the check is counted ahead of the rejection;
//      the window line is printed from the foreground report), each with a control that removes the line.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include "../../src/d3d11/flat_replay_query_reject.h"

inline int flatReplayQueryRejectTests() {
    using namespace edvr;
    int failures = 0;
    auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: replay query reject %s\n", name); ++failures; }
    };
    auto has = [](const char* text, const char* needle) { return std::strstr(text, needle) != nullptr; };

    // ---- 1. the words -------------------------------------------------------------------------------------------
    expect(std::strcmp(flatQueryCodeName(1), "OCCLUSION") == 0 && std::strcmp(flatQueryCodeName(5), "OCCLUSION_PREDICATE") == 0 &&
           std::strcmp(flatQueryCodeName(4), "PIPELINE_STATISTICS") == 0 && std::strcmp(flatQueryCodeName(15), "SO_OVERFLOW_PREDICATE_STREAM3") == 0,
           "the names are D3D11_QUERY's (1 OCCLUSION, 4 PIPELINE_STATISTICS, 5 OCCLUSION_PREDICATE, 15 SO_OVERFLOW_PREDICATE_STREAM3)");
    expect(std::strcmp(flatQueryCodeName(kFlatQueryUncertain), "UNCERTAIN_BRACKET") == 0 && std::strcmp(flatQueryCodeName(kFlatQueryTableFull), "TABLE_FULL") == 0 &&
           std::strcmp(flatQueryCodeName(kFlatQueryNotImmediate), "NOT_IMMEDIATE_CONTEXT") == 0 &&
           std::strcmp(flatQueryCodeName(kFlatQueryUnheld), "UNREADABLE_QUERY") == 0 && std::strcmp(flatQueryCodeName(77), "UNKNOWN_QUERY") == 0,
           "the tracker's own reasons have names, and an unknown code says so");
    {
        bool predicate[16];
        for (uint32_t c = 0; c < 16; ++c) predicate[c] = flatQueryCodeIsPredicate(c);
        expect(predicate[5] && predicate[7] && predicate[9] && predicate[11] && predicate[13] && predicate[15] &&
               !predicate[0] && !predicate[1] && !predicate[2] && !predicate[3] && !predicate[4] && !predicate[6] && !predicate[8] && !predicate[10] &&
               !predicate[12] && !predicate[14] && !flatQueryCodeIsPredicate(kFlatQueryUncertain),
               "predicate: OCCLUSION_PREDICATE and the SO overflow predicates only");
    }
    {
        FlatQueryReject r;
        r.codes[0] = 5; r.codes[1] = 4; r.count = 2; r.active = 3;
        char text[640];
        flatQueryRejectFormatEvent(text, sizeof(text), 3, 104910, "dlss", 0x7B0DC42D383F694Cull, 0x0DF03E64DF9DBEF1ull, 3840, 2160, r);
        expect(std::strcmp(text, "flat replay query rejected 3/8: frame=104910 mode=dlss VS=7B0DC42D383F694C PS=0DF03E64DF9DBEF1 target=3840x2160 "
                                 "queries=OCCLUSION_PREDICATE#5(predicate)+PIPELINE_STATISTICS#4 open-brackets=3") == 0,
               "the event line: ordinal of 8, frame, mode, VS, PS, target size, the query kinds (predicate marked) and the open brackets");
        FlatQueryReject lone;
        lone.codes[0] = kFlatQueryUncertain; lone.count = 1;
        flatQueryRejectFormatEvent(text, sizeof(text), 1, 7, nullptr, 1, 2, 0, 0, lone);
        expect(has(text, "mode=? ") && has(text, "queries=UNCERTAIN_BRACKET#100 ") && has(text, "target=0x0"),
               "an uncertain tracker and a missing mode name still print a full line");
        FlatQueryReject many;
        for (uint32_t i = 0; i < kFlatQueryRejectCodes; ++i) many.codes[i] = 15 - i;
        many.count = kFlatQueryRejectCodes; many.extra = 3; many.active = 9;
        flatQueryRejectFormatEvent(text, sizeof(text), 8, ~0ull, "xess", ~0ull, ~0ull, ~0u, ~0u, many);
        expect(std::strlen(text) < 640 && has(text, "+3 more open-brackets=9"), "more codes than the line keeps are counted (+N more) and the line stays under its buffer");
        expect(std::strlen(text) < 1000, "the event line at the worst digits is under the flat line budget");
    }

    // ---- 2. the counting ----------------------------------------------------------------------------------------
    {
        FlatQueryRejectStats stats;
        FlatQueryReject occ; occ.codes[0] = 5; occ.count = 1; occ.active = 1;
        FlatQueryReject stat; stat.codes[0] = 4; stat.count = 1; stat.active = 1;
        char line[1000];
        stats.formatWindow(line, sizeof(line), "dlss");
        expect(has(line, "flat replay query rejections 5s: checked=0 rejected=0 distinct=0 ") && has(line, "top=[none]") && has(line, "logged=0/8"),
               "an idle window prints checked=0 rejected=0 top=[none]: the line is there even when nothing reached the test");
        for (int i = 0; i < 40; ++i) stats.noteChecked();
        stats.formatWindow(line, sizeof(line), "dlss");
        expect(has(line, "checked=40 rejected=0 distinct=0 ") && has(line, "total-checked=40 total-rejected=0 "),
               "checked>0 with rejected=0 reads differently from checked=0, and the session totals carry over");
        uint32_t ordinals[12];
        for (int i = 0; i < 12; ++i) { stats.noteChecked(); ordinals[i] = stats.noteRejected(occ, 0xAAull, 0xBBull); }
        bool firstEight = true;
        for (uint32_t i = 0; i < kFlatQueryRejectFirst; ++i) if (ordinals[i] != i + 1) firstEight = false;
        expect(firstEight && ordinals[8] == 0 && ordinals[11] == 0 && stats.logged() == kFlatQueryRejectFirst,
               "the first 8 rejections of the session get ordinals 1..8, the rest 0 (counted, not logged)");
        for (int i = 0; i < 5; ++i) { stats.noteChecked(); stats.noteRejected(stat, 0xCCull, 0xDDull); }
        for (int i = 0; i < 3; ++i) { stats.noteChecked(); stats.noteRejected(occ, 0xAAull, 0xEEull); }   // same query, another pixel shader: its own key
        stats.formatWindow(line, sizeof(line), "fsr");
        expect(has(line, "checked=20 rejected=20 distinct=3 ") && has(line, "total-checked=60 total-rejected=20 ") && has(line, "mode=fsr"),
               "the window counts its checks, rejections and distinct (query, VS, PS) keys; totals accumulate");
        const char* a = std::strstr(line, "OCCLUSION_PREDICATE#5(predicate) VS=00000000000000AA PS=00000000000000BB n=12");
        const char* b = std::strstr(line, "PIPELINE_STATISTICS#4 VS=00000000000000CC PS=00000000000000DD n=5");
        const char* c = std::strstr(line, "OCCLUSION_PREDICATE#5(predicate) VS=00000000000000AA PS=00000000000000EE n=3");
        expect(a && b && c && a < b && b < c, "top lists each key with its count, the most frequent first");
        stats.formatWindow(line, sizeof(line), "fsr");
        expect(has(line, "checked=0 rejected=0 distinct=0 ") && has(line, "total-rejected=20 ") && has(line, "top=[none]") && stats.logged() == kFlatQueryRejectFirst,
               "the window resets after its line; the session totals and the ordinal count stay");
    }
    {
        FlatQueryRejectStats stats;
        FlatQueryReject r; r.codes[0] = 1; r.count = 1;
        for (uint32_t i = 0; i < kFlatQueryRejectTable + 5; ++i) stats.noteRejected(r, i, i);
        char line[1000];
        stats.formatWindow(line, sizeof(line), "taa");
        expect(stats.distinct() == 0 && has(line, "distinct=16 overflow=5 "), "a 17th distinct key is counted in overflow, never lost silently, and the table clears with the window");
        // A long window: six distinct keys of the longest query name (four are listed), a long mode name, 40000 checks.
        FlatQueryRejectStats wide;
        FlatQueryReject w; w.codes[0] = 15; w.count = 1;
        for (uint32_t i = 0; i < 6; ++i) for (int k = 0; k < 3; ++k) wide.noteRejected(w, ~0ull - i, ~0ull - i);
        for (int i = 0; i < 40000; ++i) wide.noteChecked();
        const int len = wide.formatWindow(line, sizeof(line), "experimental-mode");
        expect(len > 0 && len < 1000 && std::strlen(line) == static_cast<size_t>(len), "the window line with four listed keys of the longest name is under the flat line budget (1000)");
        char tiny[40];
        FlatQueryRejectStats cramped;
        cramped.noteRejected(w, 1, 2);
        cramped.formatWindow(tiny, sizeof(tiny), "dlss");
        expect(std::strlen(tiny) < sizeof(tiny), "a buffer too small truncates, it never overruns");
    }

    // ---- 3. the wiring ------------------------------------------------------------------------------------------
    {
        auto slurp = [](const char* path) {
            std::ifstream in(path, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        auto count = [](const std::string& text, const std::string& needle) {
            unsigned n = 0;
            for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
            return n;
        };
        const std::string runtimeCpp = slurp("src/d3d11/flat_runtime.cpp");
        const std::string rejectH = slurp("src/d3d11/flat_replay_query_reject.h");
        expect(!runtimeCpp.empty() && !rejectH.empty(), "the runtime and the instrument header are readable from the repo root");
        struct Pin { const std::string* text; const char* needle; unsigned times; const char* what; };
        const Pin pins[] = {
            {&runtimeCpp, "const bool queriesChecked=opaque && bytes;", 1, "the safety test is reached under the same condition it always was (opaque and the original bytes)"},
            {&runtimeCpp, "const bool queriesUnsafe=queriesChecked && !replayQueriesSafe(ctx);", 1,
             "the rejection decision is the old expression: opaque && bytes && !replayQueriesSafe(ctx)"},
            {&runtimeCpp, "if(queriesChecked)noteReplayQueryCheck(s,ctx,queriesUnsafe,domainVs,domainPs,domainWidth,domainHeight);", 1,
             "every foreground draw that reaches the test is counted, with its shaders and target size"},
            {&runtimeCpp, "failDomain(\"state\",!opaque?rasterRefusal:!bytes?\n                \"foreground-original-shader-unavailable\":indirectArgs?\"foreground-indirect-writer\":\"foreground-active-query\");", 1,
             "the refusal reasons are unchanged (foreground-active-query still names this rejection)"},
            {&runtimeCpp, "const uint32_t ordinal=s.queryReject.noteRejected(why,vs,ps);", 1, "a rejection is counted by (query, VS, PS) and may be logged one by one"},
            {&runtimeCpp, "flatQueryRejectFormatEvent(text,sizeof(text),ordinal,s.prefix.frame,flatMonoResolveModeName(s.engine),vs,ps,width,height,why);", 1,
             "the event line carries the frame and the AA mode"},
            {&runtimeCpp, "s.queryReject.formatWindow(rejectLine,sizeof(rejectLine),flatMonoResolveModeName(s.engine));", 1,
             "the window line is printed from the foreground report, every window"},
            {&runtimeCpp, "D3D11_QUERY_DESC desc{};query->GetDesc(&desc);code=static_cast<uint32_t>(desc.Query);", 1,
             "the query kind is read back from the object the tracker is holding"},
            {&rejectH, "\"flat replay query rejected %u/%u: frame=%llu", 1, "the event line's key is in the header"},
            {&rejectH, "\"flat replay query rejections 5s: checked=%llu", 1, "the window line's key is in the header"},
        };
        for (const Pin& pin : pins) {
            expect(count(*pin.text, pin.needle) == pin.times, pin.what);
            std::string without = *pin.text;
            for (size_t at = without.find(pin.needle); at != std::string::npos; at = without.find(pin.needle)) without.erase(at, std::strlen(pin.needle));
            expect(count(without, pin.needle) == 0, "replay query reject wiring control: a source with the line removed no longer contains it");
        }
        const size_t check = runtimeCpp.find("const bool queriesChecked=opaque && bytes;");
        const size_t unplannable = runtimeCpp.find("const bool unplannable=!opaque || !bytes || queriesUnsafe || indirectArgs;", check);
        const size_t note = runtimeCpp.find("noteReplayQueryCheck(s,ctx,queriesUnsafe,", check);
        expect(check != std::string::npos && note != std::string::npos && unplannable != std::string::npos && check < note && note < unplannable,
               "the check is counted before the draw's plan is decided, so the count cannot depend on the refusal");
    }
    return failures;
}
