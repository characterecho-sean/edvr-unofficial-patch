// The rig for the freeze logging (docs/freeze-diagnostics-2026-10-01.md, commit 1): the rules both halves
// share (src\common\freeze_book.h) and the lines the runtime writes (src\openxr\long_cycle_line.h).
//
// What it holds, case by case. Every check's label starts with its case id (F1..F9), which is how
// tools\freeze_log_test\mutants.py tells a mutation that tripped the right check from one that tripped
// something else:
//
//   F1  the size buckets: where each starts, and the fixed words their keys are written with
//   F2  the judge: a Present gap over twice the period is a blip when the runtime's cycle around it is not
//       long, long when the cycle is (or cannot be read), and a freeze from 250 ms whatever the cycle says
//   F3  the write rule: a freeze always has its line, a long frame only when the limiter allows, a blip never
//   F4  the book's counts, and the field flight in miniature: 44 blips and the cap used up, and the 1858 ms
//       freeze is written and counted, with over_250ms_unwritten still 0
//   F5  the worst list: five, longest first, the earlier of two equals ahead, a revision that moves on change
//   F6  the counts text, both separators, and a buffer too small for it
//   F7  the runtime's rate limit (4 a second, 400 a session) and that a freeze is neither held to it nor
//       charged to it
//   F8  the runtime's lines: native_long_cycle unchanged, the worst line, the summary with its three
//       original fields in place, and the widest of each fitting its buffer
//   F9  the glue, by source text: perf_monitor.cpp, native_runtime_host.h and native_timing.cpp each still
//       call what this rig tests (an instrument whose glue is gone looks like a quiet session)
//
//   freeze_log_test.exe --dry-run          touches nothing
//   freeze_log_test.exe --self-test [root]  root is the repository, for the F9 source pins (skipped without it)
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "freeze_book.h"
#include "long_cycle_line.h"

using namespace edvr;
using namespace edvr::openxr;

namespace {
unsigned g_checks = 0, g_failures = 0;

void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", label);
    }
}

bool contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

// ---- F1 ----------------------------------------------------------------------------------------------------
void bucketCases() {
    check(freezeBucketOf(0.0) == kFzUnder50 && freezeBucketOf(22.3) == kFzUnder50 && freezeBucketOf(49.999) == kFzUnder50,
          "F1.buckets: under 50 ms is the first bucket, a long frame's own start (22 ms at 90 Hz) included");
    check(freezeBucketOf(50.0) == kFz50To100 && freezeBucketOf(99.999) == kFz50To100,
          "F1.buckets: 50 ms starts the second bucket, 100 ms ends it");
    check(freezeBucketOf(100.0) == kFz100To250 && freezeBucketOf(249.999) == kFz100To250,
          "F1.buckets: 100 ms starts the third, 250 ms ends it");
    check(freezeBucketOf(250.0) == kFz250To1000 && freezeBucketOf(999.999) == kFz250To1000,
          "F1.buckets: 250 ms starts the fourth, the first bucket whose frames are always written");
    check(freezeBucketOf(1000.0) == kFzOver1000 && freezeBucketOf(1858.0) == kFzOver1000 && freezeBucketOf(7.2e6) == kFzOver1000,
          "F1.buckets: a second and more is the last bucket, whatever it is");
    check(std::string(freezeBucketKey(kFzUnder50)) == "lt50" && std::string(freezeBucketKey(kFz50To100)) == "50_100" &&
              std::string(freezeBucketKey(kFz100To250)) == "100_250" && std::string(freezeBucketKey(kFz250To1000)) == "250_1000" &&
              std::string(freezeBucketKey(kFzOver1000)) == "ge1000",
          "F1.keys: the bucket words are fixed: lt50 50_100 100_250 250_1000 ge1000");
    check(kFreezeAlwaysLogMs == 250.0 && kFreezeCountsEveryMs == 300000ull,
          "F1.constants: every frame of 250 ms or more is written, and the counts come every five minutes");
}

// ---- F2 ----------------------------------------------------------------------------------------------------
void judgeCases() {
    const double period = 11.1;   // 90 Hz
    using V = FreezeVerdict;
    check(freezeJudge(20.0, period, true, 40.0) == V::NotLong && freezeJudge(22.2, period, false, 0.0) == V::NotLong,
          "F2.judge: a gap of twice the period or less is not long, whatever the cycle says; the boundary is strict");
    check(freezeJudge(30.0, 0.0, true, 40.0) == V::NotLong && freezeJudge(30.0, -1.0, false, 0.0) == V::NotLong,
          "F2.judge: with no period to compare with nothing is long");
    check(freezeJudge(28.0, period, true, 15.0) == V::Blip,
          "F2.blip: a 28 ms gap while the runtime's cycle was 15 ms is a blip (the field flight's 44 of 59 lines)");
    check(freezeJudge(58.0, period, true, 22.2) == V::Blip,
          "F2.blip: a cycle of exactly twice the period is not long (the runtime's own rule is strict)");
    check(freezeJudge(30.0, period, true, 22.3) == V::Long && freezeJudge(30.0, period, true, 29.0) == V::Long,
          "F2.long: the same gap with the cycle over twice the period is long");
    check(freezeJudge(28.0, period, false, 0.0) == V::Long,
          "F2.long: a gap whose cycle cannot be read is trusted, not hidden (the flat profile, a runtime with fewer than two waits)");
    check(freezeJudge(249.9, period, true, 5.0) == V::Blip && freezeJudge(249.9, period, true, 40.0) == V::Long,
          "F2.freeze: just under 250 ms is still judged by the cycle");
    check(freezeJudge(250.0, period, true, 5.0) == V::Freeze && freezeJudge(250.0, period, false, 0.0) == V::Freeze,
          "F2.freeze: from 250 ms a gap is a freeze whatever the cycle says, or whether it can be read");
    check(freezeJudge(1858.0, period, true, 1858.0) == V::Freeze && freezeJudge(30000.0, period, true, 30000.0) == V::Freeze,
          "F2.freeze: the 1858 ms of the field flight, and a half-minute one, are freezes");
}

// ---- F3 ----------------------------------------------------------------------------------------------------
void writeRuleCases() {
    using V = FreezeVerdict;
    check(freezeWritesLine(V::Freeze, false) && freezeWritesLine(V::Freeze, true),
          "F3.write: a freeze is written with the limiter shut");
    check(freezeWritesLine(V::Long, true) && !freezeWritesLine(V::Long, false),
          "F3.write: a long frame is written when the limiter allows and only then");
    check(!freezeWritesLine(V::Blip, true) && !freezeWritesLine(V::Blip, false) && !freezeWritesLine(V::NotLong, true),
          "F3.write: a blip and a frame that is not long are never written");
    const FreezeOutcome a = freezeDecide(1858.0, 11.1, true, 1858.0, false);
    check(a.verdict == V::Freeze && a.write, "F3.decide: the 1858 ms freeze with the limiter shut is a freeze and is written");
    const FreezeOutcome b = freezeDecide(28.0, 11.1, true, 15.0, true);
    check(b.verdict == V::Blip && !b.write, "F3.decide: a blip with the limiter open is a blip and is not written");
    const FreezeOutcome c = freezeDecide(60.0, 11.1, true, 60.0, false);
    check(c.verdict == V::Long && !c.write, "F3.decide: a long frame with the limiter shut is long and is not written");
}

// ---- F4 ----------------------------------------------------------------------------------------------------
// The graphics half's flow for one Present gap, as perf_monitor.cpp's judgeLongFrame applies it: decide,
// then record. `limiterOpen` stands for the rate limit's answer.
struct Flow {
    FreezeBook book;
    void gap(double ms, double budget, bool cycleKnown, double cycle, bool limiterOpen) {
        const FreezeOutcome o = freezeDecide(ms, budget, cycleKnown, cycle, limiterOpen);
        book.record(o, ms);
    }
};

void bookCases() {
    FreezeBook b;
    b.noteBlip(30.0);
    b.noteBlip(60.0);
    b.noteLong(40.0, true);
    b.noteLong(300.0, true);
    b.noteLong(60.0, false);
    b.noteLong(1858.0, true);
    check(b.candidates == 6 && b.blips == 2 && b.longCount == 4 && b.written == 3 && b.unwritten == 1,
          "F4.counts: candidates are blips plus long frames, and the long ones split into written and not");
    check(b.blipBucket[kFzUnder50] == 1 && b.blipBucket[kFz50To100] == 1 && b.longBucket[kFzUnder50] == 1 &&
              b.longBucket[kFz50To100] == 1 && b.longBucket[kFz250To1000] == 1 && b.longBucket[kFzOver1000] == 1,
          "F4.counts: each is counted in the bucket of its own size");
    check(b.unwrittenBucket[kFz50To100] == 1 && b.unwrittenBucket[kFzUnder50] == 0 && b.unwrittenBucket[kFz100To250] == 0,
          "F4.unwritten: a long frame the limiter kept out is counted in its bucket");
    check(b.freezes == 2 && b.freezesUnwritten == 0, "F4.freezes: two freezes (300, 1858), none of them unwritten");
    b.noteLong(400.0, false);
    check(b.freezes == 3 && b.freezesUnwritten == 1,
          "F4.freezes: a freeze that was NOT written (a bug upstream) shows as over_250ms_unwritten=1, not as nothing");
    // The field flight in miniature (issue 63): 44 blips, the cap used up by ordinary lines, then the 1858 ms
    // freeze, with the limiter shut. The old graphics half counted every one of the 44 as a long frame, spent
    // its sixty lines on them and lost the freeze to the five-second limiter.
    Flow f;
    for (int i = 0; i < 44; ++i) f.gap(22.5 + i * 0.8, 11.1, true, 14.0, true);     // blips: Present gaps 22-58 ms, cycles short
    for (int i = 0; i < 15; ++i) f.gap(60.0 + i * 10.0, 11.1, true, 60.0 + i * 10.0, i < 10);   // real: ten written, five kept out
    f.gap(1858.0, 11.1, true, 1858.0, false);                                        // the freeze, limiter shut
    check(f.book.blips == 44 && f.book.longCount == 16 && f.book.written == 11 && f.book.unwritten == 5,
          "F4.flight: 44 blips counted and not written; 16 long, 11 written (ten under the limit and the freeze), 5 kept out and counted");
    check(f.book.freezes == 1 && f.book.freezesUnwritten == 0 && f.book.longBucket[kFzOver1000] == 1,
          "F4.flight: the 1858 ms freeze is written with the limiter shut and counted in the last bucket");
    check(f.book.unwrittenBucket[kFz50To100] + f.book.unwrittenBucket[kFz100To250] == 5,
          "F4.flight: the five kept out are in the 50-250 ms buckets");
    Flow g;
    g.gap(15.0, 11.1, true, 40.0, true);   // not long: 15 ms is under twice the period
    g.gap(22.2, 11.1, false, 0.0, true);   // exactly twice: not long
    check(g.book.candidates == 0, "F4.counts: a frame that is not long is not a candidate");
}

// ---- F5 ----------------------------------------------------------------------------------------------------
FreezeWorst worstOf(double ms, unsigned long long sequence, const char* stamp) {
    FreezeWorst w;
    w.ms = ms;
    w.sequence = sequence;
    w.cycleMs = ms;
    freezeCopyText(w.stamp, sizeof(w.stamp), stamp);
    return w;
}

void worstCases() {
    FreezeBook b;
    check(b.worstCount() == 0 && b.worstRevision == 0 && b.wouldKeep(1.0), "F5.worst: an empty list keeps anything and has no revision");
    const double series[] = {300.0, 1858.0, 700.0, 250.0, 500.0};
    for (double ms : series) b.offerWorst(worstOf(ms, static_cast<unsigned long long>(ms), "t"));
    check(b.worstCount() == 5 && b.worstAt(0).ms == 1858.0 && b.worstAt(1).ms == 700.0 && b.worstAt(2).ms == 500.0 &&
              b.worstAt(3).ms == 300.0 && b.worstAt(4).ms == 250.0,
          "F5.worst: five are kept, longest first");
    const uint32_t rev = b.worstRevision;
    check(!b.wouldKeep(250.0) && !b.wouldKeep(100.0) && b.wouldKeep(251.0),
          "F5.worst: a full list keeps what beats its fifth and not what only equals it");
    b.offerWorst(worstOf(100.0, 1, "x"));
    check(b.worstCount() == 5 && b.worstRevision == rev && b.worstAt(4).ms == 250.0,
          "F5.worst: a frame shorter than the fifth changes nothing, and the revision does not move");
    b.offerWorst(worstOf(900.0, 900, "y"));
    check(b.worstCount() == 5 && b.worstAt(0).ms == 1858.0 && b.worstAt(1).ms == 900.0 && b.worstAt(2).ms == 700.0 &&
              b.worstAt(4).ms == 300.0 && b.worstRevision != rev,
          "F5.worst: 900 ms goes in second and the 250 ms one falls off the end; the revision moves");
    FreezeBook t;
    t.offerWorst(worstOf(400.0, 1, "first"));
    t.offerWorst(worstOf(400.0, 2, "second"));
    check(t.worstCount() == 2 && t.worstAt(0).sequence == 1 && t.worstAt(1).sequence == 2,
          "F5.worst: of two equal frames the earlier stays ahead");
    check(std::string(b.worstAt(0).stamp) == "t" && b.worstAt(0).sequence == 1858,
          "F5.worst: an entry keeps its own time and sequence");
    char tiny[8] = {'#', '#', '#', '#', '#', '#', '#', '#'};   // not zeroed: only a terminator written by the copy ends the text
    freezeCopyText(tiny, 4, "abcdefgh");
    check(std::string(tiny, 8) == std::string("abc\0####", 8), "F5.text: text copied into a field is cut at the field and always terminated");
}

// ---- F6 ----------------------------------------------------------------------------------------------------
void countsTextCases() {
    FreezeBook b;
    b.noteBlip(30.0);
    b.noteBlip(60.0);
    b.noteLong(40.0, true);
    b.noteLong(300.0, true);
    b.noteLong(60.0, false);
    b.noteLong(1858.0, true);
    char text[1000];
    const size_t n = b.formatCounts(text, sizeof(text), ' ', true);
    check(std::string(text) ==
              "candidates=6 long=4 blips=2 written=3 unwritten=1 over_250ms=2 over_250ms_unwritten=0 "
              "long_lt50=1 long_50_100=1 long_100_250=0 long_250_1000=1 long_ge1000=1 "
              "blip_lt50=1 blip_50_100=1 blip_100_250=0 blip_250_1000=0 blip_ge1000=0 "
              "unwritten_lt50=0 unwritten_50_100=1 unwritten_100_250=0" &&
              n == std::strlen(text),
          "F6.text: the counts, key=value, space-joined, blips included; the length returned is the length written");
    b.formatCounts(text, sizeof(text), ',', false);
    check(std::string(text) ==
              "candidates=6,long=4,written=3,unwritten=1,over_250ms=2,over_250ms_unwritten=0,"
              "long_lt50=1,long_50_100=1,long_100_250=0,long_250_1000=1,long_ge1000=1,"
              "unwritten_lt50=0,unwritten_50_100=1,unwritten_100_250=0",
          "F6.text: comma-joined and without blips (the runtime's form): no blip keys, and nothing but the first three buckets can be unwritten");
    char small[48];
    const size_t m = b.formatCounts(small, sizeof(small), ' ', true);
    check(m < sizeof(small) && m == std::strlen(small) && std::strncmp(small, "candidates=6 long=4", 19) == 0,
          "F6.text: a buffer too small cuts the text, terminates it and never overruns");
    check(b.formatCounts(nullptr, 10, ' ', true) == 0 && b.formatCounts(small, 0, ' ', true) == 0,
          "F6.text: no buffer, no text");
    FreezeBook empty;
    empty.formatCounts(text, sizeof(text), ' ', true);
    check(std::string(text).rfind("candidates=0 long=0 blips=0 written=0 unwritten=0 over_250ms=0 over_250ms_unwritten=0 ", 0) == 0,
          "F6.text: a session with no long frame prints zeros, which is the proof the instrument ran");
}

// ---- F7 ----------------------------------------------------------------------------------------------------
void limiterCases() {
    FreezeSecondLimiter l;
    check(l.perSecond == 4 && l.perSession == 400, "F7.limiter: four a second and four hundred a session, as before");
    unsigned allowed = 0;
    for (int i = 0; i < 10; ++i) {
        if (l.allows(1000)) {
            l.charge();
            ++allowed;
        }
    }
    check(allowed == 4, "F7.limiter: four lines in one second, then it is shut");
    check(l.allows(1001), "F7.limiter: the next second opens it again");
    // A freeze never consults the limiter's answer and never charges it; the host's flow, written out.
    FreezeSecondLimiter h;
    unsigned written = 0, charged = 0;
    for (int i = 0; i < 12; ++i) {
        const bool freeze = i % 3 == 0;   // 4 freezes among 12 cycles in one second
        const bool allows = h.allows(2000);
        const bool write = freezeWritesLine(freeze ? FreezeVerdict::Freeze : FreezeVerdict::Long, allows);
        if (write && !freeze) { h.charge(); ++charged; }
        if (write) ++written;
    }
    check(charged == 4 && written == 8 + 0 && h.charged == 4,
          "F7.limiter: 12 cycles in one second, 4 of them freezes: the 4 freezes and 4 ordinary lines are written, the freezes charge nothing");
    FreezeSecondLimiter s;
    s.perSession = 400;
    unsigned lines = 0;
    for (uint64_t sec = 0; sec < 1000; ++sec)
        for (int i = 0; i < 4; ++i)
            if (s.allows(sec)) { s.charge(); ++lines; }
    check(lines == 400, "F7.limiter: the session cap is 400 ordinary lines");
    check(!s.allows(5000), "F7.limiter: once the cap is reached no second opens it again");
}

// ---- F8 ----------------------------------------------------------------------------------------------------
FrameCycleStats::Completed sampleCycle() {
    FrameCycleStats::Completed c;
    c.sequence = 44415;
    c.cycleMs = 1858.0;
    c.beforeFirstMs = 3.1;
    c.firstSubmitMs = 0.9;
    c.betweenEyesMs = 0.4;
    c.secondSubmitMs = 0.8;
    c.afterSecondMs = 1850.7;
    c.nextWaitMs = 1.2;
    c.waitOwnerMs = 0.3;
    c.submitOwnerMs[0] = 0.2;
    c.submitOwnerMs[1] = 0.2;
    c.renderParkMs[0] = 0.1;
    c.renderParkMs[1] = 0.1;
    c.presentSplit = false;
    c.postValid = false;
    c.postUnavailable = FrameCycleStats::ProviderMissing;
    return c;
}

void runtimeLineCases() {
    const FrameCycleStats::Completed c = sampleCycle();
    char line[1024];
    formatLongCycleLine(line, sizeof(line), 44415, 11.1111, c);
    const std::string base = line;
    check(base.rfind("native_long_cycle,sequence=44415,cycle_ms=1858.0000,period_ms=11.1111,game_before_first_submit=3.1000,", 0) == 0 &&
              contains(base, ",present_split=provider_missing,next_wait_roundtrip=1.2000,next_wait_owner_body=0.3000,units=wall_ms"),
          "F8.line: native_long_cycle keeps its head and its fields, in the order the readers know");
    char fields[1024];
    size_t len = 0;
    fields[0] = 0;
    formatLongCycleFields(fields, sizeof(fields), len, 44415, 11.1111, c);
    check(base == std::string("native_long_cycle,") + fields && len == std::strlen(fields),
          "F8.fields: the fields alone are the line without its head, byte for byte");
    char worst[1280];
    const size_t wn = formatWorstCycleLine(worst, sizeof(worst), 1, 3, "2026-10-01T15:24:19.790Z", fields);
    check(std::string(worst) == std::string("native_long_cycle_worst,rank=1,of=3,utc=2026-10-01T15:24:19.790Z,") + fields &&
              wn == std::strlen(worst),
          "F8.worst: the worst line is its rank, its time (UTC) and the cycle's own fields");
    formatWorstCycleLine(worst, sizeof(worst), 2, 2, "", nullptr);
    check(std::string(worst) == "native_long_cycle_worst,rank=2,of=2,utc=unknown,",
          "F8.worst: a missing time or fields is said, not left to read as a short line");

    FreezeBook b;
    for (int i = 0; i < 40; ++i) b.noteLong(30.0, true);
    for (int i = 0; i < 6; ++i) b.noteLong(70.0, i < 2);
    b.noteLong(1858.0, true);
    char counts[1100];
    formatLongCycleCountsLine(counts, sizeof(counts), "native_long_cycle_summary", 47, 43, nullptr, b);
    check(std::string(counts).rfind("native_long_cycle_summary,count=47,logged=43,threshold=2x_period,candidates=47,long=47,written=43,", 0) == 0,
          "F8.summary: the summary keeps count, logged and threshold at its head, in place, and adds the buckets after them");
    check(contains(counts, ",long_lt50=40,long_50_100=6,long_100_250=0,long_250_1000=0,long_ge1000=1,unwritten_lt50=0,unwritten_50_100=4,unwritten_100_250=0") &&
              !contains(counts, "blip") && !contains(counts, "reason="),
          "F8.summary: counts by bucket, the unwritten ones named, no blip keys, no reason on the closing summary");
    formatLongCycleCountsLine(counts, sizeof(counts), "native_long_cycle_counts", 47, 43, "periodic", b);
    check(std::string(counts).rfind("native_long_cycle_counts,reason=periodic,count=47,logged=43,threshold=2x_period,candidates=47,", 0) == 0,
          "F8.counts: the periodic line says what it is and why it was written");

    // The widest of each, in the buffers the host gives them (native_runtime_host.h: 1100 and 1280).
    FrameCycleStats::Completed widest = c;
    widest.cycleMs = widest.beforeFirstMs = widest.firstSubmitMs = widest.betweenEyesMs = widest.secondSubmitMs =
        widest.afterSecondMs = widest.nextWaitMs = widest.waitOwnerMs = widest.submitOwnerMs[0] = widest.submitOwnerMs[1] =
            widest.renderParkMs[0] = widest.renderParkMs[1] = 123456789.1234;
    widest.presentSplit = true;
    widest.prePresentMs = widest.presentHookMs = widest.hookBeforeRealMs = widest.hookRealMs = widest.hookAfterRealMs =
        widest.hookCallbackMs = widest.postPresentMs = 123456789.1234;
    char wf[1024];
    size_t wl = 0;
    wf[0] = 0;
    formatLongCycleFields(wf, sizeof(wf), wl, 18446744073709551615ull, 123456789.1234, widest);
    check(wl > 0 && wl < sizeof(wf) - 1 && contains(wf, "units=wall_ms"),
          "F8.fit: the widest cycle's fields fit the 1024 bytes a worst-list entry keeps, tail intact");
    char wline[1280];
    const size_t wlen = formatWorstCycleLine(wline, sizeof(wline), 5, 5, "2026-10-01T15:24:19.790Z", wf);
    check(wlen > 0 && wlen < sizeof(wline) - 1 && contains(wline, "units=wall_ms"),
          "F8.fit: and the worst line made from them fits its 1280");
    FreezeBook big;
    for (unsigned k = 0; k < kFzBuckets; ++k) {
        big.longBucket[k] = 18446744073709551615ull;
        big.blipBucket[k] = 18446744073709551615ull;
        big.unwrittenBucket[k] = 18446744073709551615ull;
    }
    big.candidates = big.longCount = big.blips = big.written = big.unwritten = big.freezes = big.freezesUnwritten = 18446744073709551615ull;
    char bigLine[1100];
    const size_t bl = formatLongCycleCountsLine(bigLine, sizeof(bigLine), "native_long_cycle_summary", 18446744073709551615ull,
                                                 18446744073709551615ull, nullptr, big);
    check(bl > 0 && bl < sizeof(bigLine) - 1 && contains(bigLine, ",unwritten_100_250=18446744073709551615"),
          "F8.fit: the counts line at 2^64-1 everywhere still fits 1100, its last key intact");
    char gfx[1000];
    const size_t gl = big.formatCounts(gfx, sizeof(gfx), ' ', true);
    check(gl > 0 && gl < 700 && contains(gfx, " unwritten_100_250=18446744073709551615"),
          "F8.fit: and the graphics half's, with blips, is under the 700 bytes perf_monitor.cpp gives it");
}

// ---- F9 ----------------------------------------------------------------------------------------------------
std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

size_t at(const std::string& text, const char* needle, size_t from = 0) { return text.find(needle, from); }

void sourcePins(const std::string& root) {
    const std::string perf = slurp(root + "\\src\\d3d11\\perf_monitor.cpp");
    const std::string host = slurp(root + "\\src\\openxr\\native_runtime_host.h");
    const std::string timing = slurp(root + "\\src\\d3d11\\native_timing.cpp");
    check(!perf.empty() && !host.empty() && !timing.empty(), "F9.read: the three sources the glue lives in were read");
    if (perf.empty() || host.empty() || timing.empty()) return;
    // perf_monitor.cpp: the judge is asked of every gap over twice the period, the freeze line is written, the
    // periodic counts and the end-of-session lines are written.
    const size_t judge = at(perf, "void judgeLongFrame(");
    const size_t decide = at(perf, "freezeDecide(gapMs", judge);
    const size_t record = at(perf, "s.freeze.record(outcome, gapMs)", judge);
    const size_t dump = at(perf, "vtableWatchDumpRecent(\"monitor: LONG FRAME\", frameNo)", judge);
    const size_t line = at(perf, "if (write) dropLine(f, budgetMs, gapMs, !freeze)", judge);
    const size_t freeze = at(perf, "if (freeze) freezeLine(gapMs, cv, frameNo, freezeNo", judge);
    const size_t none = std::string::npos;
    check(judge != none && decide != none && record != none && dump != none && line != none && freeze != none &&
              decide < record && record < dump && dump < line && line < freeze,
          "F9.perf: judgeLongFrame decides, records, dumps the flip timeline, writes the LONG FRAME line, then the FREEZE line");
    check(at(perf, "judgeLongFrame(*ringLast(), budget, gapMs, q)") != std::string::npos &&
              at(perf, "if (budget > 0.0f && gapMs > 2.0 * static_cast<double>(budget))") != std::string::npos,
          "F9.perf: perfMonitorFrame asks the judge about every gap over twice the period, by its real length (not the ring's, which is 0 from 5 s)");
    check(at(perf, "writeFreezeSummary(\"periodic\", false)") != std::string::npos &&
              at(perf, "writeFreezeSummary(\"session_close\", true)") != std::string::npos &&
              at(perf, "writeFreezeSummary(\"shutdown\", true)") != std::string::npos,
          "F9.perf: the counts are written periodically, when the runtime closes its session, and at the DLL's shutdown");
    check(at(perf, "g_nativeTimingCloseObserver = &perfMonitorSessionEnd") != std::string::npos,
          "F9.perf: the runtime's close is wired to the end-of-session lines");
    // The page's last drop still takes every candidate, blips included.
    const size_t pageAt = at(perf, "pageDrop(f);", judge);
    check(pageAt != std::string::npos && pageAt < decide,
          "F9.perf: the page's last drop is updated before the judge, so a blip is still a drop on the Monitor page");
    // native_runtime_host.h: the rule is the shared one, and the close writes the summary.
    check(at(host, "freezeWritesLine(freeze?FreezeVerdict::Freeze:FreezeVerdict::Long,limiterAllows)") != std::string::npos &&
              at(host, "if(write&&!freeze)cycleLimiter.charge();") != std::string::npos &&
              at(host, "if(tracing)writeLongCycleSummary(nullptr,true);") != std::string::npos &&
              at(host, "maybeLongCycleCounts();") != std::string::npos,
          "F9.host: noteLongCycle applies the shared write rule and charges the limiter only for ordinary lines; the close writes the summary; the counts are periodic");
    // native_timing.cpp: the wait-return stamps and the close observer.
    check(at(timing, "c->prevReturnQpc = c->lastReturnQpc;") != std::string::npos &&
              at(timing, "c->lastReturnQpc = end;") != std::string::npos &&
              at(timing, "edvr::g_nativeTimingCloseObserver()") != std::string::npos,
          "F9.timing: every wait return is stamped and the close calls the observer");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::puts("freeze_log_test: dry-run (no files, no log, no clock)");
        return 0;
    }
    if (argc < 2 || std::strcmp(argv[1], "--self-test") != 0) return 2;
    bucketCases();
    judgeCases();
    writeRuleCases();
    bookCases();
    worstCases();
    countsTextCases();
    limiterCases();
    runtimeLineCases();
    if (argc >= 3) sourcePins(argv[2]);
    else std::puts("freeze_log_test: source pins skipped (no repository root given)");
    if (g_failures) {
        std::printf("FAIL: freeze log: %u of %u checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %u freeze log checks\n", g_checks);
    return 0;
}
