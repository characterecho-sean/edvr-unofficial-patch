#pragma once
// THE PISTOL'S NO-CANDIDATE BURSTS AND THE TRAINING MISSION'S TURNED-LEFT VIEW (design doc section 104).
//
// Two things here that need no device:
//   - the ledger's classifier (animated_history_ledger.h): for a draw that found no record of its exact geometry key, which of the twenty
//     patterns it is, in the order the classifier tests them, with the facts only the history can read supplied by the test; the identity
//     verdicts of the sampled readback; the lines that carry both; and the source-spell tracker with the sequence the log showed.
//   - the wiring pins: the history, the adapter and the runtime are sources the rigs cannot drive end to end (the offset-shift rescue is
//     run on a device by tools\weapon_motion_test), so each decision is held to its text here, with a mutation control that fails it.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include "../../src/d3d11/animated_history_ledger.h"
#include "../../src/d3d11/flat_foreground_identity_verdict.h"
#include "../../src/d3d11/flat_no_candidate_report.h"
#include "../../src/d3d11/flat_source_spell.h"
#include "../../src/d3d11/flat_camera_phase.h"
#include "../../src/d3d11/flat_live_phase.h"

inline int flatNoCandidateTests() {
    using namespace edvr;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: no-candidate %s\n", name); ++failures; }
    };
    const auto key = [](unsigned count, unsigned start, int base = 0, unsigned offset = 0, unsigned indexOffset = 0) {
        HistoryKey k;
        k.vs = reinterpret_cast<const void*>(0x10); k.layout = reinterpret_cast<const void*>(0x20);
        k.vertices = reinterpret_cast<const void*>(0x30); k.indices = reinterpret_cast<const void*>(0x40);
        k.count = count; k.start = start; k.base = base; k.offset = offset; k.stride = 32; k.indexOffset = indexOffset; k.format = 57;
        return k;
    };
    const auto drawn = [](HistoryLedger& l, unsigned frame, const HistoryKey& k, bool publish = true) {
        const uint64_t token = l.note(frame, k, HistoryLedger::Captured);
        if (publish) l.markPublished(token);
        return token;
    };
    const HistoryRecordFacts none;
    const auto gapIs = [&](const HistoryLedger& l, unsigned frame, const HistoryKey& k, const HistoryRecordFacts& facts, HistoryGap want) {
        return l.classify(frame, k, facts).gap == want;
    };

    // -- the first two: the frame before gave the ledger nothing to remember --
    {
        HistoryLedger l;
        expect(gapIs(l, 100, key(9, 0), none, HistoryGap::PreviousFrameEmpty), "an empty ledger is previous-frame-empty");
        drawn(l, 98, key(9, 0));
        expect(gapIs(l, 100, key(9, 0), none, HistoryGap::PreviousFrameEmpty),
               "a ledger with only older frames is previous-frame-empty, not absent: nothing was offered the frame before");
        l.noteUnkeyed(99);
        expect(gapIs(l, 100, key(9, 0), none, HistoryGap::PreviousFrameNotCaptured),
               "a frame before that offered only draws turned away before a key is previous-frame-not-captured");
        drawn(l, 99, key(9, 3));
        expect(!gapIs(l, 100, key(9, 0), none, HistoryGap::PreviousFrameNotCaptured), "one keyed capture the frame before ends that");
    }
    // -- the same key was drawn the frame before --
    {
        HistoryLedger l;
        l.note(99, key(9, 0), HistoryLedger::RefusedOccurrence);
        expect(gapIs(l, 100, key(9, 0), none, HistoryGap::RefusedLastFrame), "refused for the occurrence cap last frame");
        HistoryLedger b; b.note(99, key(9, 0), HistoryLedger::RefusedBudget);
        expect(gapIs(b, 100, key(9, 0), none, HistoryGap::RefusedBudget), "refused for the history budget last frame");
        HistoryLedger o; o.note(99, key(9, 0), HistoryLedger::RefusedOther);
        expect(gapIs(o, 100, key(9, 0), none, HistoryGap::RefusedOther), "refused for any other cause last frame");
        HistoryLedger u; drawn(u, 99, key(9, 0), false);
        expect(gapIs(u, 100, key(9, 0), none, HistoryGap::UnpublishedLastFrame), "captured but never published last frame");
        HistoryLedger r; drawn(r, 99, key(9, 0));
        expect(gapIs(r, 100, key(9, 0), none, HistoryGap::RecordReclaimed), "published last frame and no record now: reclaimed");
        HistoryRecordFacts present; present.exactPresent = true;
        expect(gapIs(r, 100, key(9, 0), present, HistoryGap::RecordUnusable),
               "published, present and not invalidated, yet not a prior: record-unusable (the sentinel that must stay zero)");
        HistoryRecordFacts unknown = present; unknown.exactInvalidated = true; unknown.exactInvalidReasons = 1;
        HistoryRecordFacts vertices = present; vertices.exactInvalidated = true; vertices.exactInvalidReasons = 2;
        HistoryRecordFacts indices = present; indices.exactInvalidated = true; indices.exactInvalidReasons = 4;
        HistoryRecordFacts all = present; all.exactInvalidated = true; all.exactInvalidReasons = 7;
        HistoryRecordFacts both = present; both.exactInvalidated = true; both.exactInvalidReasons = 6;
        expect(gapIs(r, 100, key(9, 0), unknown, HistoryGap::InvalidatedUnknown), "invalidated by a write nothing could name");
        expect(gapIs(r, 100, key(9, 0), vertices, HistoryGap::InvalidatedVertices), "invalidated by a vertex buffer write");
        expect(gapIs(r, 100, key(9, 0), indices, HistoryGap::InvalidatedIndices), "invalidated by an index buffer write");
        expect(gapIs(r, 100, key(9, 0), all, HistoryGap::InvalidatedUnknown), "the unknown write outranks the named ones");
        expect(gapIs(r, 100, key(9, 0), both, HistoryGap::InvalidatedVertices), "a vertex buffer write outranks an index buffer write");
        // The exact key outranks everything below it.
        HistoryRecordFacts shifted = present; shifted.shiftUsable = 1;
        expect(gapIs(r, 100, key(9, 0), shifted, HistoryGap::RecordUnusable), "evidence about the exact key outranks a record at another place");
        const HistoryClass c = r.classify(100, key(9, 0), none);
        expect(c.hasNearest && c.diff == 0, "the nearest entry is the exact key's own");
    }
    // -- the same mesh at another place in the same buffers (H1) --
    {
        struct Case { const char* name; HistoryKey previous; unsigned diff; };
        const Case cases[] = {
            {"start", key(9, 3), kDiffStart}, {"base", key(9, 0, 4), kDiffBase}, {"vertex buffer offset", key(9, 0, 0, 64), kDiffOffset},
            {"index buffer offset", key(9, 0, 0, 0, 12), kDiffIndexOffset}, {"start and base", key(9, 3, 4), kDiffStart | kDiffBase},
            {"all four", key(9, 3, 4, 64, 12), kDiffStart | kDiffBase | kDiffOffset | kDiffIndexOffset}};
        for (const Case& t : cases) {
            HistoryLedger l; drawn(l, 99, t.previous);
            HistoryRecordFacts one; one.shiftUsable = 1;
            const HistoryClass c = l.classify(100, key(9, 0), one);
            char what[160];
            std::snprintf(what, sizeof(what), "the same mesh moved in %s is offset-shift and names exactly that field", t.name);
            // The key under test is (9, 0, 0, 0, 0); the previous differs by what the case names.
            expect(c.gap == HistoryGap::OffsetShift && c.diff == t.diff && c.hasNearest, what);
        }
        HistoryLedger l; drawn(l, 99, key(9, 3));
        HistoryRecordFacts two; two.shiftUsable = 2;
        HistoryRecordFacts claimed; claimed.shiftUsable = 1; claimed.shiftClaimed = 1;
        HistoryRecordFacts gone;
        expect(gapIs(l, 100, key(9, 0), two, HistoryGap::OffsetShiftAmbiguous), "two records at other places are ambiguous: refused");
        expect(gapIs(l, 100, key(9, 0), claimed, HistoryGap::OffsetShiftAmbiguous), "a record another draw already took is ambiguous: refused");
        expect(gapIs(l, 100, key(9, 0), gone, HistoryGap::OffsetShiftUnusable), "the ledger saw the mesh elsewhere and no record is usable: refused");
        HistoryRecordFacts recordsOnly; recordsOnly.shiftUsable = 1;
        HistoryLedger rolled;   // the ledger lost the entry (older than its ring): the records still decide
        expect(rolled.classify(100, key(9, 0), recordsOnly).gap == HistoryGap::PreviousFrameEmpty,
               "an empty ledger classifies by the frame before first, whatever the records hold");
        // The count must match for a shift: a different count is the next pattern, not this one.
        HistoryLedger c; drawn(c, 99, key(12, 3));
        expect(gapIs(c, 100, key(9, 0), none, HistoryGap::CountChange), "another vertex count is count-change");
        expect(c.classify(100, key(9, 0), none).diff == (kDiffCount | kDiffStart), "count-change names the count and the start");
        HistoryLedger countAndShift; drawn(countAndShift, 99, key(12, 0)); drawn(countAndShift, 99, key(9, 3));
        expect(gapIs(countAndShift, 100, key(9, 0), none, HistoryGap::OffsetShiftUnusable),
               "a count change and a shift in the frame before: the shift is the nearer, and with no usable record it is refused");
    }
    // -- the other nearest keys --
    {
        HistoryLedger f; HistoryKey other = key(9, 0); other.stride = 48;
        drawn(f, 99, other);
        expect(gapIs(f, 100, key(9, 0), none, HistoryGap::FormatChange), "another stride is format-change");
        HistoryLedger g; HistoryKey layout = key(9, 0); layout.layout = reinterpret_cast<const void*>(0x99);
        drawn(g, 99, layout);
        expect(gapIs(g, 100, key(9, 0), none, HistoryGap::FormatChange), "another input layout is format-change");
        HistoryLedger h; HistoryKey indexFormat = key(9, 0); indexFormat.format = 42;
        drawn(h, 99, indexFormat);
        expect(gapIs(h, 100, key(9, 0), none, HistoryGap::FormatChange), "another index format is format-change");
        HistoryLedger b; HistoryKey buffers = key(9, 0); buffers.vertices = reinterpret_cast<const void*>(0x31);
        drawn(b, 99, buffers);
        expect(gapIs(b, 100, key(9, 0), none, HistoryGap::BufferChange), "the same shader from another vertex buffer is buffer-change");
        HistoryLedger bi; HistoryKey indices = key(9, 0); indices.indices = reinterpret_cast<const void*>(0x41);
        drawn(bi, 99, indices);
        expect(gapIs(bi, 100, key(9, 0), none, HistoryGap::BufferChange), "the same shader from another index buffer is buffer-change");
        HistoryLedger v; HistoryKey shader = key(9, 0); shader.vs = reinterpret_cast<const void*>(0x11);
        drawn(v, 99, shader);
        expect(gapIs(v, 100, key(9, 0), none, HistoryGap::NewKey), "another vertex shader is nothing like it: new-key");
        HistoryLedger refused; refused.note(99, key(9, 3), HistoryLedger::RefusedBudget);
        expect(gapIs(refused, 100, key(9, 0), none, HistoryGap::NewKey),
               "a refused draw of another place left no record to be a prior: it is not an offset-shift");
        HistoryLedger unpublished; drawn(unpublished, 99, key(9, 3), false);
        expect(gapIs(unpublished, 100, key(9, 0), none, HistoryGap::NewKey), "an unpublished capture elsewhere is not a prior either");
    }
    // -- how long ago --
    {
        HistoryKey unrelated = key(5, 0); unrelated.vs = reinterpret_cast<const void*>(0x99);   // keeps the frame before non-empty, related to nothing
        HistoryLedger l; drawn(l, 97, key(9, 0)); drawn(l, 99, unrelated);
        HistoryClass c = l.classify(100, key(9, 0), none);
        expect(c.gap == HistoryGap::AbsentShort && c.age == 3, "an exact key seen three frames ago is absent-short with its age");
        HistoryLedger longAgo; drawn(longAgo, 80, key(9, 0)); drawn(longAgo, 99, unrelated);
        c = longAgo.classify(100, key(9, 0), none);
        expect(c.gap == HistoryGap::AbsentLong && c.age == 20, "an exact key seen twenty frames ago is absent-long");
        HistoryLedger twoBack; drawn(twoBack, 98, key(9, 0)); drawn(twoBack, 99, unrelated);
        expect(twoBack.classify(100, key(9, 0), none).age == 2, "the frame before last is age two");
        HistoryLedger pingpong; drawn(pingpong, 98, key(9, 0)); drawn(pingpong, 99, key(9, 3));
        HistoryRecordFacts one; one.shiftUsable = 1;
        expect(gapIs(pingpong, 100, key(9, 0), one, HistoryGap::OffsetShift),
               "a mesh that alternates between two places is an offset-shift against the frame before, not absent-short against the frame before that");
        HistoryLedger never; drawn(never, 99, key(5, 0)); HistoryKey otherShader = key(9, 0); otherShader.vs = reinterpret_cast<const void*>(0x77);
        expect(gapIs(never, 100, otherShader, none, HistoryGap::NewKey), "nothing like it: new-key");
    }
    // -- the ring --
    {
        HistoryKey elsewhere = key(5, 1); elsewhere.vs = reinterpret_cast<const void*>(0x99);
        HistoryLedger within; drawn(within, 10, key(9, 0));
        for (unsigned i = 0; i < 10; ++i) { elsewhere.start = i + 1; drawn(within, 11, elsewhere); }
        const HistoryClass kept = within.classify(12, key(9, 0), none);
        expect(kept.gap == HistoryGap::AbsentShort && kept.age == 2, "an exact key still in the ring is absent-short with its age");
        HistoryLedger rolled; drawn(rolled, 10, key(9, 0));
        for (unsigned i = 0; i < HistoryLedger::capacity + 8; ++i) { elsewhere.start = i + 1; drawn(rolled, 11, elsewhere); }
        expect(gapIs(rolled, 12, key(9, 0), none, HistoryGap::NewKey), "an entry older than the ring is forgotten, never invented");
        HistoryLedger stale; const uint64_t first = drawn(stale, 50, key(9, 0), false);
        for (unsigned i = 0; i < HistoryLedger::capacity; ++i) stale.note(51, key(3, i), HistoryLedger::Captured);
        stale.markPublished(first);   // the token's entry was overwritten by the last note: it must not publish what took its place
        expect(stale.entries() == 1 + HistoryLedger::capacity, "the ledger counts every note");
        expect(gapIs(stale, 52, key(3, HistoryLedger::capacity - 1), none, HistoryGap::UnpublishedLastFrame),
               "a stale token does not publish the entry that overwrote it");
        stale.markPublished(0);
        expect(gapIs(stale, 52, key(3, HistoryLedger::capacity - 1), none, HistoryGap::UnpublishedLastFrame), "token zero (the default policy's) publishes nothing");
    }
    // -- every pattern has a name, once --
    {
        bool unique = true, nonEmpty = true;
        for (unsigned a = 0; a < kHistoryGapCount; ++a) {
            const char* name = historyGapName(static_cast<HistoryGap>(a));
            nonEmpty = nonEmpty && name && name[0] && std::strcmp(name, "unknown") != 0;
            for (unsigned b = a + 1; b < kHistoryGapCount; ++b) unique = unique && std::strcmp(name, historyGapName(static_cast<HistoryGap>(b))) != 0;
        }
        expect(unique && nonEmpty && kHistoryGapCount == 20, "the twenty patterns each carry one name");
        expect(!std::strcmp(historyGapName(HistoryGap::OffsetShift), "offset-shift") && !std::strcmp(historyGapName(HistoryGap::NewKey), "new-key") &&
               !std::strcmp(historyGapName(HistoryGap::RefusedLastFrame), "refused-last-frame-cap"),
               "the names a log reader greps for are the ones the line prints");
    }
    // -- the lines --
    {
        FlatNoCandidateWindow w;
        w.submitted = 6346; w.noCandidate = 700; w.acrossOffset = 99; w.rescueCancelled = 4; w.frames = 450; w.framesMissing = 120; w.framesAllMissing = 7;
        w.longestRun = 30; w.resetFrames = 2; w.nearChanges = 1;
        for (unsigned i = 0; i < kHistoryGapCount; ++i) w.missBy[i] = i == static_cast<unsigned>(HistoryGap::OffsetShift) ? 99 : i + 1;
        char line[4096];
        const int n = flatNoCandidateLine(line, sizeof(line), w);
        const std::string text(line);
        expect(n > 0 && n < static_cast<int>(sizeof(line)) - 1, "the no-candidate line fits its buffer");
        expect(text.rfind("flat foreground no-candidate 5s: ", 0) == 0, "the no-candidate line carries the key the log is read by");
        uint64_t sum = 0; bool all = true;
        for (unsigned i = 0; i < kHistoryGapCount; ++i) {
            sum += w.missBy[i];
            char want[96];
            std::snprintf(want, sizeof(want), " %s=%llu", historyGapName(static_cast<HistoryGap>(i)), static_cast<unsigned long long>(w.missBy[i]));
            all = all && text.find(want) != std::string::npos;
        }
        expect(all, "every pattern is on the line with its count");
        char want[96];
        std::snprintf(want, sizeof(want), "same-key-misses=%llu", static_cast<unsigned long long>(sum));
        expect(text.find(want) != std::string::npos && w.misses() == sum, "same-key-misses is the sum of the patterns");
        expect(text.find("rescued-offset-shift=99 rescue-cancelled=4") != std::string::npos && text.find("still-no-candidate=700") != std::string::npos &&
               text.find("frames-all-missed=7") != std::string::npos && text.find("longest-miss-run=30") != std::string::npos &&
               text.find("reset-frames=2") != std::string::npos && text.find("near-changes=1") != std::string::npos,
               "the line carries the rescued count, what stayed without, the frame counters and the reset counters");
        FlatNoCandidateWindow zero;
        flatNoCandidateLine(line, sizeof(line), zero);
        const std::string zeros(line);
        bool allZero = true;
        for (unsigned i = 0; i < kHistoryGapCount; ++i) {
            char z[96];
            std::snprintf(z, sizeof(z), " %s=0", historyGapName(static_cast<HistoryGap>(i)));
            allZero = allZero && zeros.find(z) != std::string::npos;
        }
        expect(allZero && zeros.find("same-key-misses=0") != std::string::npos, "an empty window prints every pattern at zero: the line's absence means the code did not run");

        FlatNoCandidateWindow id;
        id.identitySamples = 40; id.identitySkipped = 3; id.identityUnread = 1;
        for (unsigned i = 0; i < kIdentityVerdictCount; ++i) id.identityBy[i] = i + 2;
        flatIdentityLine(line, sizeof(line), id);
        const std::string idText(line);
        bool allVerdicts = idText.rfind("flat foreground identity 5s: ", 0) == 0;
        for (unsigned i = 0; i < kIdentityVerdictCount; ++i) {
            char v[96];
            std::snprintf(v, sizeof(v), " %s=%u", identityVerdictName(static_cast<IdentityVerdict>(i)), i + 2);
            allVerdicts = allVerdicts && idText.find(v) != std::string::npos;
        }
        expect(allVerdicts && idText.find("sampled=40") != std::string::npos && idText.find("skipped-total=3") != std::string::npos &&
               idText.find("unread-total=1") != std::string::npos, "the identity line carries every verdict, zeros included, and the sample accounting");
        flatIdentityLine(line, sizeof(line), FlatNoCandidateWindow{});
        expect(std::string(line).find(" match=0 current-unauthentic=0 priors-unreadable=0 x-differs=0 parameter-differs=0 both-differ=0") != std::string::npos,
               "an empty identity window prints every verdict at zero");

        HistoryClass miss; miss.gap = HistoryGap::OffsetShift; miss.diff = kDiffStart | kDiffBase; miss.hasNearest = true; miss.nearest = key(9, 3, 4); miss.age = 0;
        flatNoCandidateExampleLine(line, sizeof(line), 1234, miss, key(9, 0), true, 0xDE545DC8EE4FBB87ull, 0xE46E3E4832B2FDB0ull);
        const std::string ex(line);
        expect(ex.rfind("flat foreground no-candidate example: frame=1234 pattern=offset-shift rescued=1 ", 0) == 0 &&
               ex.find("VS=DE545DC8EE4FBB87 PS=E46E3E4832B2FDB0") != std::string::npos && ex.find("count=9 start=0 base=0 vb-offset=0 stride=32 ib-offset=0 format=57") != std::string::npos &&
               ex.find("nearest the frame before:") != std::string::npos && ex.find("count=9 start=3 base=4") != std::string::npos &&
               ex.find("differs: start,base") != std::string::npos,
               "the example line carries the pattern, the draw's shaders, its whole key, the nearest entry's key and what differs");
        HistoryClass bare; bare.gap = HistoryGap::NewKey;
        flatNoCandidateExampleLine(line, sizeof(line), 5, bare, key(9, 0), false, 1, 2);
        expect(std::string(line).find("nearest the frame before: none; differs: none") != std::string::npos, "an example with no nearest entry says none");
    }
    // -- the identity verdicts: the map's own two tests --
    {
        const IdentityWords cur{100, 0x00AB1234u, 1, 0};
        IdentityWords same = cur; same.y ^= 0x00FF0000u;   // byte 30 is a per-instance parameter and is not part of the identity
        IdentityWords otherX = cur; otherX.x = 101;
        IdentityWords otherSig = cur; otherSig.y ^= 0x00000001u;
        IdentityWords otherBoth = otherX; otherBoth.y ^= 0x00000001u;
        IdentityWords unreadable{100, 0x00AB1234u, 0, 0};
        expect(classifyIdentity(cur, &same, 1) == IdentityVerdict::Match, "a candidate that differs only in byte 30 matches");
        expect(classifyIdentity(cur, &otherX, 1) == IdentityVerdict::XDiffers, "a different first word with the same signature is x-differs");
        expect(classifyIdentity(cur, &otherSig, 1) == IdentityVerdict::ParameterDiffers, "the same first word with another signature is parameter-differs");
        expect(classifyIdentity(cur, &otherBoth, 1) == IdentityVerdict::BothDiffer, "neither word shared is both-differ");
        expect(classifyIdentity(cur, &unreadable, 1) == IdentityVerdict::PriorsUnreadable, "a candidate with no valid mark cannot be read");
        IdentityWords noMark = cur; noMark.z = 0;
        expect(classifyIdentity(noMark, &same, 1) == IdentityVerdict::CurrentUnauthentic, "the draw's own identity unwritten is current-unauthentic");
        const IdentityWords several[3] = {otherX, unreadable, same};
        expect(classifyIdentity(cur, several, 3) == IdentityVerdict::Match, "any one of several candidates may match");
        const IdentityWords noneMatch[3] = {otherX, unreadable, otherSig};
        expect(classifyIdentity(cur, noneMatch, 3) == IdentityVerdict::ParameterDiffers, "an unreadable candidate is skipped and the others decide");
        expect(classifyIdentity(cur, nullptr, 0) == IdentityVerdict::PriorsUnreadable, "no candidates cannot match");
        IdentityWords y = cur; y.y ^= 0xFF000000u;
        expect(classifyIdentity(cur, &y, 1) == IdentityVerdict::ParameterDiffers, "byte 31 is part of the signature");
        bool unique = true;
        for (unsigned a = 0; a < kIdentityVerdictCount; ++a) for (unsigned b = a + 1; b < kIdentityVerdictCount; ++b)
            unique = unique && std::strcmp(identityVerdictName(static_cast<IdentityVerdict>(a)), identityVerdictName(static_cast<IdentityVerdict>(b))) != 0;
        expect(unique && kIdentityVerdictCount == 6, "the six verdicts each carry one name");
        char line[1024];
        const IdentityWords priors[2] = {otherX, otherSig};
        flatIdentityExampleLine(line, sizeof(line), 77, IdentityVerdict::BothDiffer, cur, priors, 2, key(9, 0), 3, 4);
        const std::string ex(line);
        expect(ex.rfind("flat foreground identity example: frame=77 verdict=both-differ priors=2 ", 0) == 0 &&
               ex.find("current=(00000064,00AB1234,00000001)") != std::string::npos && ex.find("prior0=(00000065,00AB1234,00000001)") != std::string::npos &&
               ex.find("prior1=(00000064,00AB1235,00000001)") != std::string::npos, "the identity example carries the words of the draw and of each candidate");
    }
    return failures;
}

inline int flatSourceSpellTests() {
    using namespace edvr;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: source spell %s\n", name); ++failures; }
    };
    // -- the sequence of the training-mission log: treated, a view with no source, treated again after two zero-phase frames --
    {
        FlatSourceSpell t;
        for (int i = 0; i < 100; ++i) t.frame(false, true, true);
        for (int i = 0; i < 203; ++i) t.frame(true, false, false);
        t.frame(false, true, false); t.frame(false, true, false); t.frame(false, true, true);
        for (int i = 0; i < 50; ++i) t.frame(false, true, true);
        const FlatSourceSpellWindow w = t.take();
        expect(w.frames == 356 && w.noSourceFrames == 203 && w.spells == 1 && w.longestSpell == 203 && w.recoveries == 1 && w.abandoned == 0,
               "one spell of 203 frames ends in a recovery");
        expect(w.warmDone == 1 && w.warmFrames == 2 && w.warmMax == 2 && w.warmAborted == 0 && !w.inSpell && w.openSpell == 0,
               "the warm-up after the spell is the two zero-phase frames FlatLivePhase asks for, then the first non-zero phase");
    }
    // -- the second turn: the spell is still open when the window ends, and ends in a menu --
    {
        FlatSourceSpell t;
        for (int i = 0; i < 30; ++i) t.frame(true, false, false);
        FlatSourceSpellWindow w = t.take();
        expect(w.inSpell && w.openSpell == 30 && w.longestSpell == 30 && w.spells == 1 && w.recoveries == 0, "a spell still open at the window's end says so");
        for (int i = 0; i < 20; ++i) t.frame(true, false, false);
        w = t.take();
        expect(w.inSpell && w.openSpell == 50 && w.spells == 0 && w.longestSpell == 50, "the next window continues the open spell, and does not count a second");
        t.frame(false, false, false);   // no-3d-scene: the view became a menu
        w = t.take();
        expect(!w.inSpell && w.abandoned == 1 && w.recoveries == 0 && w.warmDone == 0, "a spell that ends in another refusal is abandoned, not recovered");
    }
    // -- a warm-up cut short by another spell --
    {
        FlatSourceSpell t;
        t.frame(true, false, false);
        t.frame(false, true, false);
        t.frame(true, false, false);
        const FlatSourceSpellWindow w = t.take();
        expect(w.spells == 2 && w.recoveries == 1 && w.warmAborted == 1 && w.warmDone == 0, "a spell that returns during the warm-up aborts it");
        FlatSourceSpell u;
        u.frame(true, false, false); u.frame(false, true, false); u.frame(false, false, false);
        expect(u.take().warmAborted == 1, "a warm-up that meets a refusal aborts");
        FlatSourceSpell v;
        for (int i = 0; i < 5; ++i) v.frame(false, true, true);
        const FlatSourceSpellWindow steady = v.take();
        expect(steady.spells == 0 && steady.noSourceFrames == 0 && steady.recoveries == 0 && steady.warmDone == 0 && steady.frames == 5,
               "frames that never lacked a source open no spell");
    }
    // -- the camera set changes and the phase machine is not stuck --
    {
        // The frame loop the runtime runs, with the real phase machine and the real ownership core: a kind-3 camera refresh asks the
        // admission whether to inject. Warming is the admission's answer while the phase is zero, and the phase is chosen only after
        // two clean accepted zero-phase frames: a view with no motion source refuses every frame, so the machine warms for as long as
        // the view lasts, and is not stuck once it ends. A camera leaving the set changes none of it.
        FlatLivePhase phase;
        FlatCameraFrameCore core;
        unsigned injected = 0, warming = 0, cleanCloses = 0, closes = 0, cameras = 2;
        const auto run = [&](unsigned frames, bool accepted) {
            injected = warming = cleanCloses = closes = 0;
            for (unsigned f = 0; f < frames; ++f) {
                core.begin(false);
                phase.beginFrame(true, true, 2880, 1620);
                const bool nonzero = phase.currentX != 0.0f || phase.currentY != 0.0f;
                for (unsigned c = 0; c < cameras; ++c) {
                    FlatCameraAdmitInput in;
                    in.readable = true; in.kind = 3; in.gate = FlatCameraGateVerdict::Admit; in.upstreamOwns = true; in.phaseNonzero = nonzero;
                    const FlatCameraAdmit admit = flatCameraAdmit(in);
                    if (admit == FlatCameraAdmit::Inject) { ++injected; phase.noteApplied(); }
                    else if (admit == FlatCameraAdmit::Warming) ++warming;
                }
                phase.finish(accepted, true);
                if (core.close(nonzero, phase.applied != 0, phase.previousAcceptedValid, true, true)) {
                    ++closes;
                    if (phase.previousAcceptedValid) ++cleanCloses;
                }
            }
        };
        run(40, true);
        expect(injected > 0 && warming > 0 && cleanCloses == 40, "a steady view warms for two frames and then injects every frame it is accepted");
        cameras = 1;   // the pool pass's camera is no longer refreshed: the view holds no pool draws, and the selector refuses every frame
        run(1, false);   // the frame the view turns: it began with the phase the steady view had, and the refusal is its end
        run(300, false);
        expect(injected == 0 && warming == 300 * cameras && closes == 300 && cleanCloses == 0,
               "a spell of refused frames: every kind-3 refresh warms, nothing injects, no close is clean (the log's injected=0 clean-closes=0)");
        cameras = 2;   // the view turns back: a pool draw returns, and the frames are accepted again
        unsigned sequence[6] = {};
        for (unsigned f = 0; f < 6; ++f) {
            core.begin(false);
            phase.beginFrame(true, true, 2880, 1620);
            sequence[f] = (phase.currentX != 0.0f || phase.currentY != 0.0f) ? 1u : 0u;
            if (sequence[f]) phase.noteApplied();
            phase.finish(true, true);
            core.close(sequence[f] != 0, phase.applied != 0, phase.previousAcceptedValid, true, true);
        }
        expect(sequence[0] == 0 && sequence[1] == 0 && sequence[2] == 1 && sequence[3] == 1 && sequence[5] == 1,
               "two accepted zero-phase frames after the spell and the phase is chosen again: warming is not a latch");
    }
    return failures;
}

inline int flatNoCandidateWiringTests() {
    using namespace edvr;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: no-candidate wiring %s\n", name); ++failures; }
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
    const auto body = [](const std::string& text, const char* signature) {
        const size_t at = text.find(signature);
        if (at == std::string::npos) return std::string();
        const size_t end = text.find("\n}\n", at);
        return text.substr(at, end == std::string::npos ? std::string::npos : end + 3 - at);
    };
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
    const std::string motionSource = slurp("src/d3d11/flat_foreground_motion.h");
    const std::string runtimeSource = slurp("src/d3d11/flat_runtime.cpp");
    const std::string selectorSource = slurp("src/d3d11/flat_mono_frame.h");
    expect(!historySource.empty() && !motionSource.empty() && !runtimeSource.empty() && !selectorSource.empty(), "the sources are readable from the repo root");

    // -- the history: the rescue is the offset-shift pattern, extended policy only, through one unclaimed record --
    const std::string prepare = compact(member(historySource, "bool prepareCapture("));
    const auto prepareValid = [&](const std::string& text) {
        return ordered(text, {
            "if(occurrences>=limit)return refuseKeyed(\"occurrence-cap\",HistoryLedger::RefusedOccurrence);",
            "if(extended&&priorCount==0){", "out.missed=true;", "out.miss=ledger_.classify(frame,ledgerKey,facts);",
            "if(out.miss.gap==HistoryGap::OffsetShift&&shiftIndex<records_.size()){", "prior[priorCount++]=shiftIndex;",
            "out.priorRecords=priorCount;", "out.acrossOffset=true;", "records_[shiftIndex].claimFrame=frame;", "}", "}",
            "const unsigned take=(std::min)(priorCount,4u);"}) &&
            count(text, "prior[priorCount++]=") == 2 && count(text, "ledger_.classify(") == 1 && count(text, "out.acrossOffset=true;") == 1;
    };
    expect(prepareValid(prepare), "the history classifies a draw with no exact prior, and rescues the one offset-shift pattern through one record");
    expect(!prepareValid(without(prepare, "records_[shiftIndex].claimFrame=frame;")), "mutation control: a rescue that never claims its record fails the wiring");
    expect(!prepareValid(replaced(prepare, "if(out.miss.gap==HistoryGap::OffsetShift&&", "if(out.miss.gap!=HistoryGap::NewKey&&")),
           "mutation control: a rescue taken for any pattern but new-key fails the wiring");
    expect(!prepareValid(replaced(prepare, "if(out.miss.gap==HistoryGap::OffsetShift&&", "if(out.miss.gap==HistoryGap::CountChange&&")),
           "mutation control: a rescue taken for a count change fails the wiring");
    expect(!prepareValid(replaced(prepare, "if(extended&&priorCount==0){", "if(priorCount==0){")),
           "mutation control: a rescue the default policy (VR) can reach fails the wiring");
    expect(!prepareValid(without(prepare, "out.missed=true;")), "mutation control: a miss that is never recorded fails the wiring");
    expect(!prepareValid(replaced(prepare, "if(extended&&priorCount==0){", "if(extended){")),
           "mutation control: a classification of draws that have a prior fails the wiring");
    expect(prepare.find(compact("if(extended&&sameMeshElsewhere(r.geometry,key)&&usableAsPrior(r,frame,previous)){")) != std::string::npos &&
               prepare.find(compact("if(r.frame[next]==frame||r.claimFrame==frame)++facts.shiftClaimed;")) != std::string::npos &&
               count(prepare, "ledger_.note(") == 2 && count(prepare, "ledger_.noteUnkeyed(") == 1,
           "the scan counts only usable records of the same mesh elsewhere, and a record used or claimed this frame is claimed");
    expect(prepare.find(compact("refuseKeyed(\"history-budget\",HistoryLedger::RefusedBudget)")) != std::string::npos &&
               prepare.find(compact("refuseKeyed(\"resource-creation\",HistoryLedger::RefusedOther)")) != std::string::npos &&
               prepare.find(compact("if(extended)out.ledgerToken=ledger_.note(frame,ledgerKey,HistoryLedger::Captured);")) != std::string::npos,
           "every refusal after the key and every capture is noted with its key, the refusals under their cause");
    const std::string mesh = compact(member(historySource, "static bool sameMeshElsewhere("));
    const auto meshValid = [&](const std::string& text) {
        return ordered(text, {"a.original==b.original", "a.layout==b.layout", "a.vertices==b.vertices", "a.indices==b.indices", "a.count==b.count",
                              "a.stride==b.stride", "a.format==b.format", "(a.start!=b.start||a.base!=b.base||a.offset!=b.offset||a.indexOffset!=b.indexOffset)"});
    };
    expect(meshValid(mesh), "the same mesh elsewhere is the same shader, layout, buffers, count, stride and format with the placement different");
    expect(!meshValid(without(mesh, "a.count==b.count&&")), "mutation control: matching across a change of count fails the wiring");
    expect(!meshValid(without(mesh, "a.vertices==b.vertices&&")), "mutation control: matching across a change of vertex buffer fails the wiring");
    expect(!meshValid(without(mesh, "a.format==b.format&&")), "mutation control: matching across a change of index format fails the wiring");
    expect(!meshValid(without(mesh, "a.original==b.original&&")), "mutation control: matching across a change of shader fails the wiring");
    expect(!meshValid(replaced(mesh, "(a.start!=b.start||a.base!=b.base||a.offset!=b.offset||a.indexOffset!=b.indexOffset)", "true")),
           "mutation control: an exact match counted as another place fails the wiring");
    const std::string usable = compact(member(historySource, "static bool usableAsPrior("));
    expect(usable.find(compact("return!r.invalidated&&r.frame[previous]!=~0u&&r.frame[previous]+1==frame;")) != std::string::npos,
           "a record is a prior only if nothing invalidated it and the frame before used it");
    expect(usable.find(compact("return r.frame[previous]!=~0u&&r.frame[previous]+1==frame;")) == std::string::npos,
           "mutation control: a prior that ignores invalidation is not what the text says");
    const std::string publish = compact(member(historySource, "void restorePositions("));
    expect(count(publish, "ledger_.markPublished(out.ledgerToken);") == 2, "both publishing paths mark the ledger entry published");
    const std::string written = compact(member(historySource, "unsigned resourceWritten("));
    expect(written.find(compact("r.invalidated=true;r.invalidReasons|=why;")) != std::string::npos, "an invalidation keeps the write's reason for the ledger");

    // -- the withdrawal: a rescue whose donor another draw used this frame was a sibling part, not a move --
    const std::string holds = compact(member(historySource, "bool rescueHolds("));
    const auto holdsValid = [&](const std::string& text) {
        return ordered(text, {"if(!c.acrossOffset)returntrue;", "for(constauto&r:records_)",
                              "if(r.views[previous]==c.previousPositions[0]&&r.frame[next]==frame)returnfalse;", "returntrue;"});
    };
    expect(holdsValid(holds), "a rescue holds unless a record whose previous positions are the donor's was used this frame");
    expect(!holdsValid(without(holds, "&&r.frame[next]==frame")), "mutation control: a donor that is withdrawn whether or not it was used fails the wiring");
    expect(!holdsValid(without(holds, "if(!c.acrossOffset)returntrue;")), "mutation control: a draw that was not rescued being judged fails the wiring");
    expect(!holdsValid(replaced(holds, "returnfalse;", "returntrue;")), "mutation control: a rescue that is never withdrawn fails the wiring");
    const std::string forH = compact(member(motionSource, "bool prepareH("));
    const auto withdrawValid = [&](const std::string& text) {
        return ordered(text, {"out.coveredDraws=covered_;", "for(auto&d:current_)",
                              "if(d.capture.acrossOffset&&d.priorCount&&!history_.rescueHolds(d.capture,frame)){d.priorCount=0;++stats_.rescueCancelled;}",
                              "if(!ctx||!owners||!rawDepth"});
    };
    expect(withdrawValid(forH), "H withdraws a rescued prior whose donor another draw used, after the frame's refusal and before it draws");
    expect(!withdrawValid(without(forH, "d.priorCount=0;")), "mutation control: a withdrawal that keeps the prior fails the wiring");
    expect(!withdrawValid(replaced(forH, "!history_.rescueHolds(d.capture,frame)", "history_.rescueHolds(d.capture,frame)")),
           "mutation control: a withdrawal of the rescues that hold fails the wiring");
    expect(!withdrawValid(without(forH, "++stats_.rescueCancelled;")), "mutation control: a withdrawal that is not counted fails the wiring");

    // -- the adapter: the pattern counts, the frame counters, the sample, and the preflight's notes --
    const std::string capture = compact(member(motionSource, "bool capture("));
    expect(capture.find(compact("auto reject=[&](const char* reason){++stats_.preflightRefused;history_.noteNotOffered(frame);fail(reason);return false;};")) != std::string::npos &&
               capture.find(compact("auto defer=[&](const char* reason){++stats_.preflightRefused;history_.noteNotOffered(frame);drawRefusal_=reason;return false;};")) != std::string::npos,
           "a draw turned away before the history was asked is noted, so the frame before is not read as empty");
    expect(ordered(capture, {"tally(d);", "++frameSubmitted_;", "if(d.capture.missed){", "++frameMissed_;", "sampler_.consider(ctx,frame,"}) &&
               capture.find(compact("if(inputs.gpuIdentity&&d.priorCount&&d.capture.currentIdentity){")) != std::string::npos,
           "the adapter counts the frame's draws and misses, keeps an example of each pattern, and samples the identity of a draw the map will match");
    const std::string tally = compact(member(motionSource, "void tally("));
    const auto tallyValid = [&](const std::string& text) {
        return ordered(text, {"if(d.capture.missed){", "++stats_.missBy[unsigned(d.capture.miss.gap)];",
                              "if(d.capture.acrossOffset)++stats_.acrossOffset;", "}", "if(d.capture.candidateCount==0){++stats_.noCandidate;return;}"});
    };
    expect(tallyValid(tally), "every same-key miss is filed under its pattern before the draw takes its other filing");
    expect(!tallyValid(without(tally, "++stats_.missBy[unsigned(d.capture.miss.gap)];")), "mutation control: a miss that is never filed fails the wiring");
    expect(!tallyValid(without(tally, "if(d.capture.acrossOffset)++stats_.acrossOffset;")), "mutation control: a rescue that is never counted fails the wiring");
    const std::string beginFrame = compact(member(motionSource, "void beginFrame("));
    expect(ordered(beginFrame, {"if(frameSubmitted_){", "++stats_.frames;", "if(frameMissed_){", "++stats_.framesMissing;", "if(missRun_>longestRun_)longestRun_=missRun_;",
                                "if(frameMissed_==frameSubmitted_)++stats_.framesAllMissing;", "frameSubmitted_=frameMissed_=0;"}),
           "the frame that ends is counted before the next begins: the draws it submitted, those that missed, the run, and a frame where all did");
    const std::string prepareH = compact(member(motionSource, "bool prepareH("));
    expect(prepareH.find(compact("if(previousNear_&&previousNear_!=commonNear){out.resetRequired=true;++stats_.nearChanges;}")) != std::string::npos &&
               prepareH.find(compact("if(out.resetRequired)++stats_.resetFrames;")) != std::string::npos,
           "a reset H asks for, and the near plane change behind it, are counted");

    // -- the runtime: the shaders ride the capture, and the lines are written --
    const std::string scope = compact(body(runtimeSource, "void FlatRuntimeDrawScope::beginActualDraw("));
    expect(scope.find(compact("inputs.vs=domainVs;inputs.ps=domainPs;")) != std::string::npos,
           "the capture is handed the draw's shader pair for the example lines");
    const std::string report = compact(body(runtimeSource, "static void reportForegroundNoCandidate("));
    const auto reportValid = [&](const std::string& text) {
        return ordered(text, {"s.foregroundMissReported=captures;", "flatNoCandidateLine(line,sizeof(line),w);Log::get().note(\"%s\",line);",
                              "flatIdentityLine(line,sizeof(line),w);Log::get().note(\"%s\",line);", "flatNoCandidateExampleLine(", "flatIdentityExampleLine("}) &&
            text.find(compact("w.longestRun=(std::max)(w.longestRun,motion.takeLongestMissRun());")) != std::string::npos &&
            text.find(compact("delta(captures.missBy[i],was.missBy[i])")) != std::string::npos;
    };
    expect(reportValid(report), "the window is the difference of the cumulative counters, printed as the two lines and the examples");
    expect(!reportValid(without(report, "flatNoCandidateLine(line,sizeof(line),w);Log::get().note(\"%s\",line);")), "mutation control: a window that is never printed fails the wiring");
    expect(compact(runtimeSource).find(compact("w.rescueCancelled=delta(captures.rescueCancelled,was.rescueCancelled);")) != std::string::npos,
           "the window carries the rescues withdrawn");
    const std::string domain = compact(body(runtimeSource, "static void reportForegroundDomain("));
    expect(ordered(domain, {"for(const auto& candidate:s.foregroundCandidates)captures.add(candidate.motion.stats());", "reportForegroundNoCandidate(s,captures);"}),
           "the domain report prints the no-candidate window from the same cumulative counters");

    // -- the selector and the spell --
    const std::string selector = compact(body(selectorSource, "inline FlatMonoFrame flatSelectMonoFrame("));
    const auto selectorValid = [&](const std::string& text) {
        return ordered(text, {"if(!out.supportedDraws){", "summarizeSourceless(in,count,hdr->key.depth,*hdrCamera,out.sourceless);",
                              "if(in.unsupportedFamilyDraws||out.unsupportedDraws)returnrefuse(FlatMonoReason::NoSupportedSource);",
                              "out.sourceFree=true;", "}"});
    };
    expect(selectorValid(selector), "a scene with no supported source says what it held on the HDR's depth before it is refused, or taken source-free");
    expect(!selectorValid(without(selector, "summarizeSourceless(in,count,hdr->key.depth,*hdrCamera,out.sourceless);")), "mutation control: a refusal that names nothing fails the wiring");
    const std::string present = compact(runtimeSource);
    const auto spellValid = [&](const std::string& text) {
        return ordered(text, {"const FlatFrameSeenendedSeen=s.frameSeen;", "const FlatMonoReasonendedReason=s.frameReason;", "standDownFrame(s,frame);",
                              "flatCameraInjectClose(s.frameHadPhase,", "if(endedSeen!=FlatFrameSeen::None)",
                              "s.sourceSpell.frame(endedReason==FlatMonoReason::NoSupportedSource,endedSeen==FlatFrameSeen::Treatable,s.frameHadPhase);"}) &&
            text.find(compact("if(selected.reason==FlatMonoReason::NoSupportedSource){s.sourcelessLast=selected.sourceless;s.sourcelessLastFrame=s.prefix.frame;}")) != std::string::npos &&
            ordered(text, {"reportForegroundDomain(s);", "reportSourceSpell(s);", "flatHDRimagecontinuation"}) ;
    };
    expect(spellValid(present), "the verdict of the frame that ends is taken before the stand-down clears it, feeds the spell tracker, and the spell line follows the domain line");
    expect(!spellValid(without(present, "s.sourceSpell.frame(endedReason==FlatMonoReason::NoSupportedSource,endedSeen==FlatFrameSeen::Treatable,s.frameHadPhase);")),
           "mutation control: a spell tracker that is never fed fails the wiring");
    expect(!spellValid(without(present, "reportSourceSpell(s);")), "mutation control: a spell line that is never written fails the wiring");
    expect(!spellValid(replaced(present, "const FlatFrameSeenendedSeen=s.frameSeen;", "const FlatFrameSeenendedSeen=FlatFrameSeen::None;")),
           "mutation control: a verdict read after the stand-down cleared it fails the wiring");
    return failures;
}
