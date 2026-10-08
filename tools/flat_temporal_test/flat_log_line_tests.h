#pragma once
// THE LOGGER'S CUT, PINNED (design doc section 104; src/common/log.cpp). The game's logger keeps 1,167 characters of a line after its timestamp and
// ends a longer one with "...[truncated]". The 2026-10-08 Epic flight log had 118 truncated lines of four families (the SDK domain line, the sibling
// line, the no-candidate line, the engine movers line). They are split now, every part under kFlatLogLineBudget (1000, flat_history_report.h), and this
// file holds every periodic flat / engine-motion line to that, so the next long line fails the build instead of a flight:
//
//   1. the formatter functions, built at the worst digits (every u64 counter 2^64-1, every u32 4294967295, the longest names, four source pairs with
//      16-digit hex), at twelve digits, and all zero: each part under the budget, each part's prefix, each field on exactly one part, a zero window
//      printing every counter at zero, a buffer too small truncating rather than overrunning;
//   2. the flat cpu packer's lines (flat_cpu.h): every packed line under the budget, nothing dropped, no token split;
//   3. the SOURCE of every Log::get().note( call in the flat and engine-motion files, read by a small estimator (adjacent literals joined, each
//      conversion at its worst width, each %s at a cap): the lines this change split must stay under the budget at 20-digit counters; the lines left
//      whole under the cut at 20 digits or the budget at 12; and THE NET, every other call at 12-digit counters under the cut unless it is on an
//      allow-list that says why (a stale entry fails too);
//   4. the wiring: the runtime prints the parts in loops over the parts constants, the first part of each split line keeps its key.
// Mutation controls sit beside each: an over-long format, a re-merged line, a dropped group of patterns, a packer limit of 1090, a call with no cap.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include "../../src/d3d11/flat_foreground_shadow.h"
#include "../../src/d3d11/flat_foreground_sibling.h"
#include "../../src/d3d11/flat_history_report.h"
#include "../../src/d3d11/flat_no_candidate_report.h"
#include "../../src/d3d11/flat_source_spell.h"

namespace flat_log_lines {
using namespace edvr;

// src/common/log.cpp: a 1200-byte line, less the 15-character timestamp, 3, and the 15-character marker.
constexpr size_t kLoggerCut = 1167;

// ---- 1. the formatters --------------------------------------------------------------------------------------------------------------------------

enum Level : int { kZero = 0, kTwelve = 1, kWidest = 2 };
inline const char* levelName(int l) { return l == kZero ? "all-zero" : l == kTwelve ? "12-digit" : "20-digit"; }
inline uint64_t levelQ(int l) { return l == kZero ? 0ull : l == kTwelve ? 999999999999ull : ~0ull; }   // a u64 counter
inline unsigned levelD(int l) { return l == kZero ? 0u : ~0u; }                                          // a u32 counter

// A field of a line: the text before its value, and what the value is. kind: q a u64 counter, d a u32 counter, p four u64 counters joined by '/',
// x a value derived from others (checked at zero only), n no value check. `repeated`: it is printed once for each of the four source pairs.
struct Field {
    std::string needle;
    char kind;
    bool repeated;
};
inline std::string expectedValue(char kind, int l) {
    const std::string q = std::to_string(static_cast<unsigned long long>(levelQ(l)));
    if (kind == 'q') return q;
    if (kind == 'd') return std::to_string(levelD(l));
    if (kind == 'p') return q + "/" + q + "/" + q + "/" + q;
    if (kind == 'x') return l == kZero ? "0" : "";
    return "";
}
// The occurrences of a field: a name must start a word (after a space, a bracket or a line start), so `records=` is not `peak-records=`.
inline std::vector<size_t> fieldAt(const std::string& text, const std::string& needle) {
    std::vector<size_t> out;
    const bool word = !needle.empty() && std::isalnum(static_cast<unsigned char>(needle[0])) != 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        const char before = at ? text[at - 1] : ' ';
        if (!word || before == ' ' || before == '(' || before == '\n' || before == '[') out.push_back(at);
    }
    return out;
}
inline bool valueFollows(const std::string& text, size_t at, const std::string& needle, const std::string& value, bool lastOfGroup) {
    const size_t from = at + needle.size();
    if (text.compare(from, value.size(), value) != 0) return false;
    const char next = from + value.size() < text.size() ? text[from + value.size()] : ' ';
    return !std::isdigit(static_cast<unsigned char>(next)) && (!lastOfGroup || next != '/');
}
// Every number after a '=' or a '/' is a zero (the prefix is stripped first: it carries "5s" and "(1/3)"; "<=0.25=" is a bin's label, not a value).
inline bool allZeros(const std::string& text) {
    for (size_t i = 0; i + 1 < text.size(); ++i) {
        if ((text[i] != '=' && text[i] != '/') || !std::isdigit(static_cast<unsigned char>(text[i + 1]))) continue;
        if (text[i] == '=' && i && text[i - 1] == '<') continue;
        size_t j = i + 1;
        while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
        if (text.compare(i + 1, j - i - 1, "0") != 0) return false;
        if (j < text.size() && text[j] == '.' && j + 1 < text.size() && text[j + 1] != '0' && std::isdigit(static_cast<unsigned char>(text[j + 1]))) return false;
    }
    return true;
}

using Print = std::function<int(char*, size_t, unsigned, int)>;
struct Case {
    std::string name;
    unsigned parts;
    std::vector<std::string> prefixes;   // each part starts with this; the first keeps the original key
    std::vector<Field> fields;           // each on exactly one part (or four times, repeated)
    Print print;
    bool zeroable;                       // a window of zeros prints every counter at zero
};
struct Rendered {
    std::vector<std::string> parts;
    std::vector<int> returned;
};
inline Rendered render(const Case& c, int l) {
    Rendered r;
    for (unsigned p = 0; p < c.parts; ++p) {
        char buf[4096];
        std::memset(buf, 'x', sizeof(buf));
        const int n = c.print(buf, sizeof(buf), p, l);
        r.returned.push_back(n);
        r.parts.push_back(n > 0 && n < static_cast<int>(sizeof(buf)) ? std::string(buf) : std::string());
    }
    return r;
}
inline std::string joinParts(const std::vector<std::string>& parts) {
    std::string all;
    for (const std::string& p : parts) { all += p; all += '\n'; }
    return all;
}
// Each field on exactly one part (four times for the repeated ones), with the value the level puts in it. Returns the first thing wrong, or "".
inline std::string checkFields(const Case& c, const std::vector<std::string>& parts, int l) {
    const std::string all = joinParts(parts);
    for (const Field& f : c.fields) {
        const std::vector<size_t> at = fieldAt(all, f.needle);
        const size_t want = f.repeated ? (l == kZero ? 0 : 4) : 1;
        if (at.size() != want) return "field `" + f.needle + "` appears " + std::to_string(at.size()) + " times, not " + std::to_string(want);
        const std::string value = expectedValue(f.kind, l);
        if (f.kind == 'n' || value.empty()) continue;
        for (size_t one : at)
            if (!valueFollows(all, one, f.needle, value, f.kind != 'x')) return "field `" + f.needle + "` does not carry " + value + " at " + levelName(l);
    }
    return "";
}

inline void fillAll(HistoryWriteStats& s, uint64_t v) {
    for (unsigned e = 0; e < kHistoryWriteEntries; ++e) {
        s.observed[e] = s.ranged[e] = s.savedWrites[e] = s.recordsInvalidated[e] = v;
        for (unsigned r = 0; r < kHistoryWriteRoles; ++r)
            for (unsigned t = 0; t < kHistoryWriteTimings; ++t) s.touching[e][r][t] = s.invalidating[e][r][t] = v;
    }
    s.unknownInvalidating[0] = s.unknownInvalidating[1] = s.sparedRecords = v;
    for (auto& x : s.erased) x = v;
    s.allocations = s.allocationFailures = s.extentIssued = s.extentRead = s.extentFailed = s.setFromCache = s.setPending = s.setApproximate = s.setCancelled = v;
    s.vertexUnknown = s.vertexInGap = s.vertexOutside = s.vertexGenuine = s.deferredInvalidated = s.deferredConservative = v;
}
inline FlatHistoryWindow historyWindow(int l) {
    FlatHistoryWindow w;
    fillAll(w.writes, levelQ(l));
    w.records = w.bytes = w.peakRecords = w.peakBytes = levelD(l);
    if (l != kZero) {
        w.topCount = 3;
        for (auto& t : w.top) { t.resource = reinterpret_cast<const void*>(~uintptr_t(0)); t.touching = t.invalidating = t.saved = levelQ(l); }
    }
    return w;
}
inline HistoryKey widestKey() {
    HistoryKey k;
    k.vs = k.layout = k.vertices = k.indices = reinterpret_cast<const void*>(~uintptr_t(0));
    k.count = k.start = k.offset = k.stride = k.indexOffset = k.format = ~0u;
    k.base = -2147483647 - 1;
    return k;
}

inline std::vector<Case> buildCases() {
    std::vector<Case> cases;

    {   // the sibling line: the head and the 24 patterns in three groups of eight
        Case c{"sibling", kFlatSiblingLineParts,
               {"flat foreground sibling 5s: ", "flat foreground sibling 5s (2/4): by pattern (sibling/view-attached/disagree/identity-unreadable):",
                "flat foreground sibling 5s (3/4): by pattern:", "flat foreground sibling 5s (4/4): by pattern:"},
               {{"engaged-frames=", 'q', false}, {"dispatches=", 'q', false}, {"draws-read=", 'x', false}, {"read=", 'q', false}, {"unread=", 'q', false},
                {"failed=", 'q', false}, {"sibling=", 'x', false}, {"view-attached=", 'x', false}, {"disagree=", 'x', false},
                {"identity-unreadable=", 'x', false}},
               [](char* out, size_t n, unsigned part, int l) {
                   FlatSiblingWindow w;
                   const uint64_t v = levelQ(l);
                   w.engagedFrames = w.dispatches = w.readbacks = w.notReady = w.failed = v;
                   for (auto& row : w.by) for (auto& cell : row) cell = v;
                   return flatSiblingLine(out, n, w, part);
               },
               true};
        for (unsigned p = 0; p < kSiblingPatterns; ++p) c.fields.push_back({std::string(siblingPatternName(p)) + "=", 'p', false});
        cases.push_back(c);
    }
    {   // the no-candidate line: the head and the 22 patterns in two groups of eleven
        Case c{"no-candidate", kFlatNoCandidateLineParts,
               {"flat foreground no-candidate 5s: ", "flat foreground no-candidate 5s (2/3): by pattern:", "flat foreground no-candidate 5s (3/3): by pattern:"},
               {{"submitted=", 'q', false}, {"same-key-misses=", 'x', false}, {"rescued-offset-shift=", 'q', false}, {"rescue-cancelled=", 'q', false},
                {"still-no-candidate=", 'q', false}, {"frames=", 'q', false}, {"frames-with-misses=", 'q', false}, {"frames-all-missed=", 'q', false},
                {"longest-miss-run=", 'd', false}, {"reset-frames=", 'q', false}, {"near-changes=", 'q', false}, {"records=", 'd', false},
                {"bytes=", 'd', false}, {"peak-records=", 'd', false}, {"peak-bytes=", 'd', false}, {"allocations=", 'q', false}},
               [](char* out, size_t n, unsigned part, int l) {
                   FlatNoCandidateWindow w;
                   const uint64_t v = levelQ(l);
                   w.submitted = w.noCandidate = w.acrossOffset = w.frames = w.framesMissing = w.framesAllMissing = w.resetFrames = w.nearChanges = v;
                   w.rescueCancelled = w.allocations = v;
                   w.records = w.bytes = w.peakRecords = w.peakBytes = w.longestRun = levelD(l);
                   for (auto& m : w.missBy) m = v;
                   return flatNoCandidateLine(out, n, w, part);
               },
               true};
        for (unsigned i = 0; i < kHistoryGapCount; ++i) c.fields.push_back({std::string(historyGapName(static_cast<HistoryGap>(i))) + "=", 'q', false});
        cases.push_back(c);
    }
    {   // the identity line: one line, its legend moved to a comment
        Case c{"identity", 1, {"flat foreground identity 5s: "},
               {{"sampled=", 'q', false}, {"skipped-total=", 'q', false}, {"unread-total=", 'q', false}},
               [](char* out, size_t n, unsigned part, int l) {
                   if (part) return 0;
                   FlatNoCandidateWindow w;
                   const uint64_t v = levelQ(l);
                   w.identitySamples = w.identitySkipped = w.identityUnread = v;
                   for (auto& b : w.identityBy) b = v;
                   return flatIdentityLine(out, n, w);
               },
               true};
        for (unsigned i = 0; i < kIdentityVerdictCount; ++i) c.fields.push_back({std::string(identityVerdictName(static_cast<IdentityVerdict>(i))) + "=", 'q', false});
        cases.push_back(c);
    }
    {   // the source-spell line: the fields, then the top pairs
        Case c{"source-spell", kFlatSourceLineParts, {"flat source 5s: frames=", "flat source 5s (2/2): top: "},
               {{"frames=", 'q', false}, {"no-source-frames=", 'q', false}, {"spells=", 'q', false}, {"longest-spell=", 'q', false}, {"open-spell=", 'q', false},
                {"recoveries=", 'q', false}, {"abandoned=", 'q', false}, {"warm-ups-done=", 'q', false}, {"warm-frames-total=", 'q', false},
                {"warm-frames-max=", 'q', false}, {"warm-ups-aborted=", 'q', false}, {"source-free-frames=", 'q', false},
                {"source-free-treated=", 'q', false}, {"overlays-without-named-world=", 'q', false}, {"last no-source frame=", 'q', false},
                {"records-on-scene-depth=", 'd', false}, {"same-camera-draws=", 'd', false}, {"pool-kind-records=", 'd', false},
                {"distinct-pairs=", 'd', false}, {"[VS=", 'n', true}, {" PS=", 'n', true}, {"same-camera=", 'n', true}, {"pool-kind=", 'n', true},
                {"family-vs=", 'n', true}, {"recipe=", 'n', true}},
               [](char* out, size_t n, unsigned part, int l) {
                   FlatSourceSpellWindow w;
                   const uint64_t v = levelQ(l);
                   w.frames = w.noSourceFrames = w.spells = w.recoveries = w.abandoned = w.warmDone = w.warmFrames = w.warmMax = w.warmAborted = v;
                   w.longestSpell = w.openSpell = w.sourceFreeFrames = w.sourceFreeTreated = w.overlaysUnnamed = v;
                   FlatMonoSourceless busy;
                   FlatSourcelessPairNote notes[4];
                   if (l != kZero) {
                       busy.records = busy.draws = busy.sameCameraDraws = busy.poolRecords = busy.distinctPairs = ~0u;
                       busy.topCount = 4;
                       for (auto& pair : busy.top) { pair.vs = pair.ps = ~0ull; pair.draws = pair.records = ~0u; pair.sameCamera = pair.pool = true; }
                       for (auto& note : notes) note.familyVs = note.recipe = true;
                   }
                   return flatSourceSpellLine(out, n, w, busy, v, notes, part);
               },
               true};
        cases.push_back(c);
    }
    {   // the history lines (already split by the build before): the summary, three writes lines, the vertex line, an example
        cases.push_back({"history", 1, {"flat foreground history 5s: "},
                         {{"records=", 'd', false}, {"bytes=", 'd', false}, {"peak-records=", 'd', false}, {"peak-bytes=", 'd', false},
                          {"allocations=", 'q', false}, {"allocation-failures=", 'q', false}, {"advance-invalidated=", 'q', false},
                          {"pressure-invalidated=", 'q', false}, {"pressure-spent=", 'q', false}, {"advance-aged=", 'q', false},
                          {"requested=", 'q', false}, {"read=", 'q', false}, {"failed=", 'q', false}, {"cancelled=", 'q', false},
                          {"from-cache=", 'q', false}, {"already-pending=", 'q', false}, {"approximate=", 'q', false}},
                         [](char* out, size_t n, unsigned part, int l) { return part ? 0 : flatHistoryLine(out, n, historyWindow(l)); }, true});
        Case writes{"history-writes", kFlatHistoryWriteLines,
                    {"flat foreground history writes 5s (1/3):", "flat foreground history writes 5s (2/3):", "flat foreground history writes 5s (3/3):"},
                    {},
                    [](char* out, size_t n, unsigned part, int l) { return flatHistoryWritesLine(out, n, historyWindow(l), part); }, true};
        for (unsigned e = 0; e < kHistoryWriteEntries; ++e) writes.fields.push_back({std::string(" ") + historyWriteEntryName(e) + ": observed=", 'q', false});
        cases.push_back(writes);
        cases.push_back({"history-vertex", 1, {"flat foreground history vertex writes 5s: "},
                         {{"unknown-invalidating=", 'x', false}, {"spared-records=", 'q', false}, {"extent-unknown=", 'q', false}, {" in-gap=", 'q', false},
                          {"outside-span=", 'q', false}, {"genuine=", 'q', false}, {"deferred-invalidated=", 'q', false},
                          {"deferred-conservative=", 'q', false}},
                         [](char* out, size_t n, unsigned part, int l) { return part ? 0 : flatHistoryVertexLine(out, n, historyWindow(l)); }, true});
        cases.push_back({"history-example", 1, {"flat foreground history write example: "},
                         {{"case=", 'n', false}, {"entry=", 'n', false}, {"resource=", 'n', false}, {"write=", 'n', false}, {"record-age=", 'n', false},
                          {"shader=", 'n', false}, {"layout=", 'n', false}, {"vertices=", 'n', false}, {"indices=", 'n', false}, {"count=", 'n', false},
                          {"start=", 'n', false}, {"base=", 'n', false}, {"vb-offset=", 'n', false}, {"stride=", 'n', false}, {"ib-offset=", 'n', false},
                          {"format=", 'n', false}},
                         [](char* out, size_t n, unsigned part, int l) {
                             if (part) return 0;
                             const uint64_t v = levelQ(l);
                             HistoryWriteExample x;
                             x.kind = HistoryWriteCase::Genuine; x.entry = HistoryWriteEntry::CopyResource;
                             x.resource = reinterpret_cast<const void*>(~uintptr_t(0));
                             x.first = v; x.end = v; x.known = true; x.spanFirst = v - 5; x.spanEnd = v; x.exact = false; x.runs = ~0u; x.age = ~0u;
                             x.key = widestKey();
                             return flatHistoryExampleLine(out, n, x);
                         },
                         false});
    }
    {   // the shadow line (three parts, the build before)
        cases.push_back({"shadow", kFlatShadowLineParts,
                         {"flat foreground shadow 5s (1/3): ", "flat foreground shadow 5s (2/3): ", "flat foreground shadow 5s (3/3): "},
                         {{"sampled-frames=", 'q', false}, {"draws-read=", 'q', false}, {"unread=", 'q', false}, {"failed=", 'q', false},
                          {"matched-with-donors=", 'q', false}, {"matched-alone=", 'q', false}, {"receiver-with-donors=", 'q', false},
                          {"receiver-alone=", 'q', false}, {"identity-unreadable=", 'q', false}, {"current=", 'q', false}, {"affine=", 'q', false},
                          {"both=", 'q', false}, {"neither=", 'q', false}, {"fit-vertices=", 'q', false}, {"conditioning=", 'q', false},
                          {"residual=", 'q', false}, {"hull=", 'q', false}, {"depth=", 'q', false}, {"draws=", 'x', false}, {"vertices=", 'x', false},
                          {"current-accepts=", 'q', false}, {"current-refuses(disagree)=", 'q', false}, {"affine-would-accept-of-refused=", 'q', false},
                          {"affine-would-accept-of-accepted=", 'q', false}, {"fires=", 'q', false}, {"while-matched-draws-moved-over-1px=", 'q', false},
                          {"frames with such motion=", 'q', false}},
                         [](char* out, size_t n, unsigned part, int l) {
                             const uint64_t v = levelQ(l);
                             ShadowStats st;
                             st.sampledFrames = st.drawsRead = st.notReady = st.failed = v;
                             for (auto& b : st.byKind) b = v;
                             for (unsigned i = 0; i < kFlatShadowBins; ++i) st.meanAll[i] = st.meanAccepted[i] = st.affineAll[i] = st.affineAccepted[i] = st.residualBins[i] = v;
                             st.currentAccepts = st.affineAccepts = st.bothAccept = st.neitherAccept = v;
                             for (auto& g : st.gateFailed) g = v;
                             st.donorDrawsSum = st.donorVerticesSum = v;
                             st.receiverCurrentAccepts = st.receiverCurrentRefuses = st.receiverRefusedAffineAccepts = st.receiverAcceptedAffineAccepts = v;
                             st.modeTwoFires = st.modeTwoWhileMoving = st.movingFrames = v;
                             return flatShadowLine(out, n, st, part);
                         },
                         true});
    }
    {   // the example lines of the no-candidate and identity reports (one each)
        cases.push_back({"no-candidate-example", 1, {"flat foreground no-candidate example: frame="}, {},
                         [](char* out, size_t n, unsigned part, int l) {
                             if (part) return 0;
                             HistoryClass miss;
                             miss.gap = HistoryGap::PreviousFrameNotCaptured;
                             miss.diff = kDiffVs | kDiffLayout | kDiffVertices | kDiffIndices | kDiffCount | kDiffStart | kDiffBase | kDiffOffset | kDiffStride |
                                         kDiffIndexOffset | kDiffFormat;
                             miss.hasNearest = true;
                             miss.nearest = widestKey();
                             miss.age = ~0u;
                             return flatNoCandidateExampleLine(out, n, levelQ(l), miss, widestKey(), true, ~0ull, ~0ull);
                         },
                         false});
        cases.push_back({"identity-example", 1, {"flat foreground identity example: frame="}, {},
                         [](char* out, size_t n, unsigned part, int l) {
                             if (part) return 0;
                             IdentityWords w;
                             w.x = w.y = w.z = w.w = ~0u;
                             IdentityWords priors[4] = {w, w, w, w};
                             return flatIdentityExampleLine(out, n, levelQ(l), IdentityVerdict::ParameterDiffers, w, priors, 4, widestKey(), ~0ull, ~0ull);
                         },
                         false});
    }
    return cases;
}

// ---- 2. the flat cpu packer --------------------------------------------------------------------------------------------------------------------

inline bool linesWithin(const flatcpu::Lines& lines, size_t limit) {
    for (int i = 0; i < lines.count; ++i) if (std::strlen(lines.line[i]) > limit) return false;
    return true;
}
// A copy of flatcpu::packLines with the limit as an argument: the mutant a packer of 1090 would be, which the oracle above must catch.
inline void packAt(const flatcpu::Tokens& tk, flatcpu::Lines* out, size_t limit) {
    out->count = 0;
    out->truncated = false;
    size_t used = 0;
    for (int i = 0; i < tk.n; ++i) {
        const size_t textLen = std::strlen(tk.t[i].text);
        const size_t sepLen = out->count > 0 ? std::strlen(tk.t[i].sep) : 0;
        if (out->count > 0 && used + sepLen + textLen <= limit) {
            std::snprintf(out->line[out->count - 1] + used, sizeof(out->line[0]) - used, "%s%s", tk.t[i].sep, tk.t[i].text);
            used += sepLen + textLen;
            continue;
        }
        if (out->count >= flatcpu::kMaxLines) { out->truncated = true; break; }
        char* dst = out->line[out->count];
        const int w = std::snprintf(dst, sizeof(out->line[0]), "%s%s", out->count == 0 ? "" : "flat cpu 5s (cont.): ", tk.t[i].text);
        used = w > 0 ? static_cast<size_t>(w) : 0;
        ++out->count;
    }
}
inline bool parenthesesBalanced(const char* s) {
    int depth = 0;
    for (; *s; ++s) { if (*s == '(') ++depth; if (*s == ')' && --depth < 0) return false; }
    return depth == 0;
}

// ---- 3. the source scan ------------------------------------------------------------------------------------------------------------------------

// Comments blanked (newlines kept, so offsets and line numbers are the file's); `live[i]` is false inside a comment, a string or a character literal.
inline void blankComments(const std::string& raw, std::string& code, std::vector<char>& live) {
    code = raw;
    live.assign(raw.size(), 1);
    const size_t n = raw.size();
    const auto isIdent = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
    const auto blank = [&](size_t a, size_t b) {
        for (size_t k = a; k < b; ++k) { if (code[k] != '\n') code[k] = ' '; live[k] = 0; }
    };
    const auto dead = [&](size_t a, size_t b) { for (size_t k = a; k < b && k < n; ++k) live[k] = 0; };
    size_t i = 0;
    while (i < n) {
        const char c = raw[i];
        if (c == '/' && i + 1 < n && raw[i + 1] == '/') {
            size_t j = raw.find('\n', i);
            if (j == std::string::npos) j = n;
            blank(i, j);
            i = j;
        } else if (c == '/' && i + 1 < n && raw[i + 1] == '*') {
            size_t j = raw.find("*/", i + 2);
            j = j == std::string::npos ? n : j + 2;
            blank(i, j);
            i = j;
        } else if (c == 'R' && i + 1 < n && raw[i + 1] == '"' && (i == 0 || !isIdent(raw[i - 1]))) {
            const size_t open = raw.find('(', i + 2);
            if (open == std::string::npos) { ++i; continue; }
            const std::string close = ")" + raw.substr(i + 2, open - i - 2) + "\"";
            size_t end = raw.find(close, open + 1);
            end = end == std::string::npos ? n : end + close.size();
            dead(i, end);
            i = end;
        } else if (c == '"') {
            size_t j = i + 1;
            while (j < n && raw[j] != '"') { if (raw[j] == '\\') ++j; ++j; }
            j = (std::min)(j + 1, n);
            dead(i, j);
            i = j;
        } else if (c == '\'' && !(i > 0 && std::isalnum(static_cast<unsigned char>(raw[i - 1])))) {
            size_t j = i + 1;
            while (j < n && raw[j] != '\'') { if (raw[j] == '\\') ++j; ++j; }
            j = (std::min)(j + 1, n);
            dead(i, j);
            i = j;
        } else {
            ++i;
        }
    }
}

struct NoteCall {
    std::string file;
    size_t line = 0;
    std::string format;                  // the adjacent literals joined, escapes still in
    bool literal = false;                // the first argument is a literal
    std::vector<std::string> args;       // after the format, as written
    std::string argText;                 // the first argument as written
    std::string context;                 // the code before the call
    std::string before;                  // the code before the call, further back (for a buffer's declaration)
};

inline std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
inline std::vector<NoteCall> scanNotes(const std::string& raw, const std::string& file) {
    std::vector<NoteCall> out;
    std::string code;
    std::vector<char> live;
    blankComments(raw, code, live);
    static const std::string callee = "Log::get().note(";
    const size_t n = code.size();
    for (size_t at = code.find(callee); at != std::string::npos; at = code.find(callee, at + 1)) {
        if (!live[at]) continue;
        NoteCall call;
        call.file = file;
        call.line = 1 + static_cast<size_t>(std::count(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
        std::vector<std::string> args;
        std::string cur;
        int depth = 0;
        size_t i = at + callee.size();
        for (; i < n; ++i) {
            const char c = code[i];
            if (c == '"' || (c == '\'' && !(i > 0 && std::isalnum(static_cast<unsigned char>(code[i - 1]))))) {
                size_t j = i + 1;
                while (j < n && code[j] != c) { if (code[j] == '\\') ++j; ++j; }
                j = (std::min)(j, n - 1);
                cur.append(code, i, j + 1 - i);
                i = j;
                continue;
            }
            if (c == '(' || c == '[' || c == '{') ++depth;
            else if (c == ')' || c == ']' || c == '}') { if (depth == 0) break; --depth; }
            else if (c == ',' && depth == 0) { args.push_back(trimmed(cur)); cur.clear(); continue; }
            cur += c;
        }
        args.push_back(trimmed(cur));
        call.argText = args[0];
        call.args.assign(args.begin() + 1, args.end());
        // the format: the literals of the first argument, joined
        size_t p = 0;
        const std::string& first = args[0];
        bool pureLiterals = !first.empty() && first[0] == '"';
        while (p < first.size()) {
            while (p < first.size() && std::isspace(static_cast<unsigned char>(first[p]))) ++p;
            if (p >= first.size()) break;
            if (first[p] != '"') { pureLiterals = false; break; }
            size_t j = p + 1;
            while (j < first.size() && first[j] != '"') { if (first[j] == '\\') ++j; ++j; }
            call.format.append(first, p + 1, j - p - 1);
            p = j + 1;
        }
        call.literal = pureLiterals;
        const size_t back = at > 420 ? at - 420 : 0;
        call.context = code.substr(back, at - back);
        const size_t wayBack = at > 1800 ? at - 1800 : 0;
        call.before = code.substr(wayBack, at - wayBack);
        out.push_back(call);
    }
    return out;
}

// One conversion of a format, in the order it consumes its argument.
struct Conversion {
    char conv = 0;
    bool wide64 = false;     // ll, z, j, t, I64: a 64-bit argument
    bool wideString = false; // %ls
    int width = 0, precision = -1;
    size_t arg = 0;          // index among the call's arguments after the format
};
inline std::vector<Conversion> parseFormat(const std::string& fmt, size_t& literalChars) {
    std::vector<Conversion> out;
    literalChars = 0;
    size_t i = 0, arg = 0;
    const size_t n = fmt.size();
    while (i < n) {
        const char c = fmt[i];
        if (c == '\\') {
            ++i;
            if (i < n && fmt[i] == 'x') { ++i; while (i < n && std::isxdigit(static_cast<unsigned char>(fmt[i]))) ++i; }
            else if (i < n && fmt[i] >= '0' && fmt[i] <= '7') { for (int k = 0; k < 3 && i < n && fmt[i] >= '0' && fmt[i] <= '7'; ++k) ++i; }
            else ++i;
            ++literalChars;
            continue;
        }
        if (c != '%') { ++literalChars; ++i; continue; }
        ++i;
        if (i < n && fmt[i] == '%') { ++literalChars; ++i; continue; }
        Conversion cv;
        while (i < n && std::strchr("-+ #0", fmt[i])) ++i;
        if (i < n && fmt[i] == '*') { ++arg; ++i; }
        else while (i < n && std::isdigit(static_cast<unsigned char>(fmt[i]))) cv.width = cv.width * 10 + (fmt[i++] - '0');
        if (i < n && fmt[i] == '.') {
            ++i;
            cv.precision = 0;
            if (i < n && fmt[i] == '*') { ++arg; ++i; }
            else while (i < n && std::isdigit(static_cast<unsigned char>(fmt[i]))) cv.precision = cv.precision * 10 + (fmt[i++] - '0');
        }
        bool isLong = false;
        for (bool again = true; again && i < n;) {
            again = true;
            if (fmt[i] == 'l' && i + 1 < n && fmt[i + 1] == 'l') { cv.wide64 = true; i += 2; }
            else if (fmt[i] == 'l') { isLong = true; ++i; }
            else if (fmt[i] == 'z' || fmt[i] == 'j' || fmt[i] == 't') { cv.wide64 = true; ++i; }
            else if (fmt[i] == 'h') { ++i; }
            else if (fmt.compare(i, 3, "I64") == 0) { cv.wide64 = true; i += 3; }
            else if (fmt.compare(i, 3, "I32") == 0) { i += 3; }
            else again = false;
        }
        if (i >= n) break;
        cv.conv = fmt[i++];
        cv.wideString = isLong && cv.conv == 's';
        cv.arg = arg++;
        out.push_back(cv);
    }
    return out;
}
// The worst length of a format at `digits`-digit counters (20: 2^64-1, 12: a twelve-digit count). strCap(cv) gives the cap of a %s.
inline size_t estimateFormat(const std::string& fmt, unsigned digits, const std::function<size_t(const Conversion&)>& strCap, unsigned* unknown = nullptr) {
    size_t lit = 0;
    const std::vector<Conversion> conversions = parseFormat(fmt, lit);
    size_t total = lit;
    for (const Conversion& cv : conversions) {
        size_t natural = 0;
        const size_t prec = cv.precision < 0 ? 6u : static_cast<size_t>((std::min)(cv.precision, 24));
        switch (cv.conv) {
        case 'd': case 'i': natural = cv.wide64 ? (digits >= 20 ? 20 : digits + 1) : 11; break;
        case 'u': natural = cv.wide64 ? digits : 10; break;
        case 'x': case 'X': natural = cv.wide64 ? (cv.width >= 16 ? 16 : digits) : 8; break;
        case 'o': natural = cv.wide64 ? 22 : 11; break;
        case 'p': natural = 18; break;
        case 'c': natural = 1; break;
        case 'f': case 'F': natural = 21 + prec; break;
        case 'e': case 'E': case 'g': case 'G': natural = prec + 8; break;
        case 's': natural = strCap(cv); if (cv.precision >= 0) natural = (std::min)(natural, static_cast<size_t>(cv.precision)); break;
        default: if (unknown) ++*unknown; natural = 0; break;
        }
        if (cv.conv != 's' && cv.precision > 0 && (cv.conv == 'd' || cv.conv == 'i' || cv.conv == 'u' || cv.conv == 'x' || cv.conv == 'X'))
            natural = (std::max)(natural, static_cast<size_t>(cv.precision));
        total += (std::max)(natural, static_cast<size_t>(cv.width));
    }
    return total;
}

// ---- the table and the net ---------------------------------------------------------------------------------------------------------------------

constexpr size_t kDefaultStr = 32;       // an enum name, a reason, a mode name
constexpr size_t kDefaultCStr = 96;      // a std::string built by the code (c_str())
constexpr size_t kDefaultWideStr = 260;  // a path

enum Kind { kPart, kWhole20, kCut12 };
struct Row {
    const char* lead;     // the call's format begins with this
    size_t strCap;        // the cap of each %s of the line (0: the default of the net)
    Kind kind;            // kPart: held to the budget at 20 digits; kWhole20: to the cut at 20 digits; kCut12: to the cut at 12 digits (20 is out of reach)
    const char* why;
};
inline const std::vector<Row>& rows() {
    static const std::vector<Row> table = {
        {"flat foreground SDK domain: configured=", 32, kPart, "part 1 of 3 of the split; %s is the mode name"},
        {"flat foreground SDK domain (2/3):", 32, kPart, "part 2 of 3"},
        {"flat foreground SDK domain (3/3):", 64, kPart, "part 3 of 3; the only %s is the last H refusal's reason name"},
        {"engine motion: movers joined", 32, kPart, "part 1 of 4 of the split"},
        {"engine motion: movers (2/4):", 700, kPart, "part 2 of 4; the one %s is the invalidation list (checked against the source: 11 names)"},
        {"engine motion: movers (3/4):", 350, kPart, "part 3 of 4; the one %s is the MRT6 refusal list (checked against the source: 5 names)"},
        {"engine motion: movers (4/4):", 32, kPart, "part 4 of 4; the %s are a ' (' reason ')' triple"},
        {"engine motion: on foot: source frames", 470, kPart, "part 1 of the on-foot split; the one %s is the dropped-frames list (the 11 invalidation names, checked against the source)"},
        {"engine motion: on foot (2/3):", 220, kPart, "part 2 of the on-foot split; the one %s is the declined list (5 names, checked against the source)"},
        {"engine motion: on foot, another camera:", 768, kPart, "the on-foot line printed only when another camera moved the rows; the one %s is the `other` buffer (768 bytes)"},
        {"engine motion: on foot (3/3):", 720, kPart, "part 3 of the on-foot split; the one %s is the panel's pixel counts, the `pixels` buffer (720 bytes)"},
        {"flat map bounce 5s:", 8, kCut12, "left whole: tools\\edvr_log.py --map-bounce parses it. At 20 digits it is out of reach (a 5 s window of maps and bytes is not 10^19); at 12 it is held to the cut, not the budget"},
        {"flat foreground ownership: frame=", 40, kWhole20, "left whole on the cut: a one-time diagnostic, not periodic (flat_foreground_probe.h reports at most kLimit = 2 times, then releases its GPU resources); the %s are a status, measured/unavailable, a failure reason"},
        {"flat overlay admission refused:", 24, kWhole20, "left whole on the cut: a capped diagnostic, not periodic (flat_runtime.cpp prints at most 6 of them a session, 10 s apart); the %s is a route name"},
        {"flat runtime conflict: frame=", 32, kWhole20, "left whole: a sample with a rate limit; the %s is a cause name"},
        {"flat untrusted camera coverage summary:", 32, kWhole20, "left whole: once in 300 frames; the %s are mode and reason names"},
        {"engine motion: emit (", 24, kWhole20, "left whole: the emit's 5 s census; the %s is a mode name"},
        {"engine motion: history gaps", 32, kWhole20, "left whole: the emit's 5 s census"},
    };
    return table;
}
inline const Row* rowFor(const std::string& format) {
    for (const Row& r : rows()) if (format.compare(0, std::strlen(r.lead), r.lead) == 0) return &r;
    return nullptr;
}

// What was measured of the formatters: a pass-through note("%s", line) after one of these calls is as long as the formatter's worst part.
struct Measured { const char* callee; const char* caseName; size_t worst = 0; };

struct Allow {
    const char* file;      // the source file's name
    const char* lead;      // the call's format begins with this ("" for any)
    const char* context;   // and the code before the call contains this ("" for any)
    const char* reason;
    mutable bool used;
};
inline std::vector<Allow> allowList() {
    // What the net cannot estimate or finds over the cut at 12-digit counters, each with the reason it is allowed. Every entry must match a call.
    return {
        {"flat_runtime.cpp", "%s", "flatQueryDepth(", "flat_query_cut.h's fallback notice, passed through a lambda: written once per state for the session (a state that falls back asks the context for the rest of it), by flatQueryFallbackLine", false},
        {"flat_runtime.cpp", "%s", "flatQueryShaders(", "the same fallback notice for the shader question; a one-time diagnostic", false},
        {"flat_runtime.cpp", "%s", "flatStandDownFormatEntered(", "an event line (entered the stand-down), built into 1200 bytes by flat_standdown.h, whose rig pins it under 1100 characters", false},
        {"flat_runtime.cpp", "%s", "flatStandDownFormatResumed(", "an event line (resumed), the same formatter family and bound", false},
        {"flat_runtime.cpp", "%s", "flatStandDownFormatStill(", "a reminder while stood down, rate-limited by reportDue(); the same formatter family and bound", false},
        {"flat_runtime.cpp", "%s", "flatCopyFormatWindow(", "flat_copy_structure.h's 5 s window line, built into 1200 bytes; outside the four families the 2026-10-08 log showed truncated (it was whole there) and not measured at 20 digits here", false},
    };
}
struct Verdict {
    std::vector<std::string> failures;
    std::vector<std::string> report;
    int calls = 0, inScope = 0, literalCalls = 0, passThrough = 0, formatterFed = 0, bufferBound = 0, allowed = 0;
};

inline bool inScopeFile(const std::string& path) {
    const std::filesystem::path p(path);
    const std::string dir = p.parent_path().filename().string(), name = p.filename().string();
    return dir == "d3d11" && (name.rfind("flat_", 0) == 0 || name.rfind("engine_velocity", 0) == 0);
}
inline bool leadIsFlat(const std::string& f) { return f.rfind("flat ", 0) == 0 || f.rfind("engine motion:", 0) == 0; }

// A %s whose argument is a plain identifier: the size of the char array it was declared as, a few hundred characters before; -1 if none was found or
// it is a parameter.
inline long bufferSizeOf(const std::string& name, const std::string& before) {
    const std::string array = "char " + name + "[";
    const size_t decl = before.rfind(array);
    const size_t param = before.rfind("const char* " + name);
    if (param != std::string::npos && (decl == std::string::npos || param > decl)) return -1;
    if (decl == std::string::npos) return -1;
    const size_t digits = decl + array.size();
    size_t j = digits;
    while (j < before.size() && std::isdigit(static_cast<unsigned char>(before[j]))) ++j;
    if (j == digits || j >= before.size() || before[j] != ']') return -1;
    return std::atol(before.substr(digits, j - digits).c_str());
}
inline bool isIdentifier(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}
inline bool isLiteralArg(const std::string& s) {
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') return false;
    for (size_t i = 1; i + 1 < s.size(); ++i) { if (s[i] == '\\') ++i; else if (s[i] == '"') return false; }
    return true;
}

// The estimate of one call at `digits`-digit counters: its length, and how it was bounded.
struct Estimate {
    size_t length = 0;
    std::string how;            // "" for an ordinary literal format
    bool unclassified = false;  // a %s of a buffer of unknown size, with no measured formatter before it
};
inline Estimate estimateCall(const NoteCall& call, unsigned digits, const Row* row, const std::vector<Measured>& measured, size_t cpuLimit) {
    Estimate e;
    if (!call.literal) { e.unclassified = true; e.how = "the format is not a literal"; return e; }
    const bool passThrough = call.format == "%s" && call.args.size() == 1;
    if (passThrough) {
        const std::string& arg = call.args[0];
        if (arg.find("lines.line[") == 0) { e.length = cpuLimit; e.how = "the flat cpu packer's line (kLineLimit)"; return e; }
        long size = isIdentifier(arg) ? bufferSizeOf(arg, call.before) : -1;
        if (size > 0 && static_cast<size_t>(size) - 1 <= kLoggerCut) { e.length = static_cast<size_t>(size) - 1; e.how = "a " + std::to_string(size) + "-byte buffer"; return e; }
        size_t best = 0;
        std::string name;
        for (const Measured& m : measured)
            if (call.context.find(m.callee) != std::string::npos && m.worst >= best) { best = m.worst; name = m.callee; }
        if (best) { e.length = best; e.how = "the measured worst part of " + name + ")"; return e; }
        e.unclassified = true;
        e.how = size > 0 ? "a buffer of " + std::to_string(size) + " bytes with no measured formatter before it" : "a pass-through of a buffer of unknown size";
        return e;
    }
    const auto cap = [&](const Conversion& cv) -> size_t {
        if (row && row->strCap) return row->strCap;
        const std::string arg = cv.arg < call.args.size() ? call.args[cv.arg] : std::string();
        if (isLiteralArg(arg)) return arg.size() - 2;
        if (isIdentifier(arg)) {
            const long size = bufferSizeOf(arg, call.before);
            if (size > 0 && static_cast<size_t>(size) - 1 <= kLoggerCut) return static_cast<size_t>(size) - 1;
        }
        if (cv.wideString) return kDefaultWideStr;
        if (arg.find("c_str()") != std::string::npos) return kDefaultCStr;
        return kDefaultStr;
    };
    e.length = estimateFormat(call.format, digits, cap);
    return e;
}

inline std::vector<Measured> measuredFormatters(const std::vector<Case>& cases) {
    struct Map { const char* callee; const char* caseName; };
    static const Map map[] = {{"flatNoCandidateLine(", "no-candidate"}, {"flatIdentityLine(", "identity"}, {"flatSiblingLine(", "sibling"},
                              {"flatSourceSpellLine(", "source-spell"}, {"flatHistoryLine(", "history"}, {"flatHistoryWritesLine(", "history-writes"},
                              {"flatHistoryVertexLine(", "history-vertex"}, {"flatHistoryExampleLine(", "history-example"}, {"flatShadowLine(", "shadow"},
                              {"flatNoCandidateExampleLine(", "no-candidate-example"}, {"flatIdentityExampleLine(", "identity-example"}};
    std::vector<Measured> out;
    for (const Map& m : map) {
        Measured one{m.callee, m.caseName, 0};
        for (const Case& c : cases) {
            if (c.name != m.caseName) continue;
            const Rendered r = render(c, kWidest);
            for (const std::string& part : r.parts) one.worst = (std::max)(one.worst, part.size());
        }
        out.push_back(one);
    }
    return out;
}

// Checks every call against the table and the net. `checkRows`: false skips the demand that each table row match exactly one call (a synthetic source).
inline void checkNotes(const std::vector<NoteCall>& all, const std::vector<Measured>& measured, size_t cpuLimit, const std::vector<Allow>& allow, Verdict& v,
                       bool verbose, bool checkRows = true) {
    for (const Allow& a : allow) a.used = false;
    std::map<std::string, int> found;
    for (const NoteCall& call : all) {
        ++v.calls;
        if (!(inScopeFile(call.file) || (call.literal && leadIsFlat(call.format)))) continue;
        ++v.inScope;
        const Row* row = call.literal ? rowFor(call.format) : nullptr;
        if (row) ++found[row->lead];
        const std::string where = call.file + ":" + std::to_string(call.line);
        const std::string lead = call.literal ? call.format.substr(0, 52) : call.argText.substr(0, 40);
        if (call.literal) ++v.literalCalls;
        if (call.literal && call.format == "%s" && call.args.size() == 1) ++v.passThrough;
        const Estimate net = estimateCall(call, 12, row, measured, cpuLimit);
        if (net.how.rfind("the measured worst part", 0) == 0) ++v.formatterFed;
        else if (net.how.find("-byte buffer") != std::string::npos) ++v.bufferBound;
        const bool overCut = net.length > kLoggerCut && !(row && row->kind != kCut12);   // the rows judge themselves below
        if (net.unclassified || overCut) {
            // the allow-list, consulted only for a call the net would fail. Of the entries that fit, the one named closest before the call.
            const Allow* chosen = nullptr;
            size_t chosenAt = 0;
            for (const Allow& a : allow) {
                if (call.file.size() < std::strlen(a.file) || call.file.compare(call.file.size() - std::strlen(a.file), std::string::npos, a.file) != 0) continue;
                if (a.lead[0] && call.format.compare(0, std::strlen(a.lead), a.lead) != 0) continue;
                size_t at = 0;
                if (a.context[0]) {
                    at = call.context.rfind(a.context);
                    if (at == std::string::npos) continue;
                }
                if (!chosen || at >= chosenAt) { chosen = &a; chosenAt = at; }
            }
            if (chosen) {
                chosen->used = true;
                ++v.allowed;
                v.report.push_back("allow-listed " + where + " `" + lead + "` (" + (net.unclassified ? net.how : std::to_string(net.length) + " characters at 12 digits") + "): " + chosen->reason);
            } else if (net.unclassified) {
                v.failures.push_back(where + " `" + lead + "`: " + net.how + "; add the formatter to the measured list or the call to the allow-list with a reason");
            } else {
                v.failures.push_back(where + " `" + lead + "` can reach " + std::to_string(net.length) + " characters at 12-digit counters: the logger keeps " +
                                     std::to_string(kLoggerCut) + ". Split it (flat_history_report.h's budget is " + std::to_string(kFlatLogLineBudget) + ") or allow-list it with a reason");
            }
            continue;
        }        if (!row) continue;
        // a line of the table is held to its own limit, at the digits its row names
        const Estimate w20 = estimateCall(call, 20, row, measured, cpuLimit), w12 = net;
        const size_t limit = row->kind == kPart ? kFlatLogLineBudget : kLoggerCut;
        const Estimate& used = row->kind == kCut12 ? w12 : w20;
        if (verbose)
            std::printf("  log lines: %-44s %4zu at 20 digits, %4zu at 12 (%s; limit %zu at %s digits)%s%s\n", call.format.substr(0, 44).c_str(), w20.length, w12.length,
                        row->kind == kPart ? "split part" : row->kind == kWhole20 ? "left whole" : "left whole, 20 digits out of reach", limit, row->kind == kCut12 ? "12" : "20",
                        row->kind == kCut12 ? ": " : "", row->kind == kCut12 ? row->why : "");
        if (used.length > limit)
            v.failures.push_back(where + " `" + lead + "` can reach " + std::to_string(used.length) + " characters at " + (row->kind == kCut12 ? "12" : "20") +
                                 "-digit counters: its limit is " + std::to_string(limit));
    }
    if (checkRows)
        for (const Row& r : rows()) {
            const int n = found.count(r.lead) ? found[r.lead] : 0;
            if (n != 1) v.failures.push_back(std::string("the table row `") + r.lead + "` matches " + std::to_string(n) + " calls, not one: the line was renamed, merged or duplicated");
        }
    for (const Allow& a : allow)
        if (checkRows && !a.used) v.failures.push_back(std::string("the allow-list entry for ") + a.file + " `" + a.lead + "` / `" + a.context + "` matched no call: remove it");
}

inline std::vector<NoteCall> scanTree(const std::string& root, int* files) {
    std::vector<NoteCall> out;
    std::error_code ec;
    std::vector<std::string> paths;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path& p = it->path();
        const std::string ext = p.extension().string();
        if (it->is_regular_file(ec) && (ext == ".cpp" || ext == ".h")) paths.push_back(p.generic_string());
    }
    std::sort(paths.begin(), paths.end());
    for (const std::string& path : paths) {
        std::ifstream in(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.find("Log::get().note(") == std::string::npos) continue;
        if (files) ++*files;
        const std::vector<NoteCall> calls = scanNotes(text, path);
        out.insert(out.end(), calls.begin(), calls.end());
    }
    return out;
}

// ---- the tests ---------------------------------------------------------------------------------------------------------------------------------

inline std::string compactText(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) if (c != ' ' && c != '\r' && c != '\n' && c != '\t') out += c;
    return out;
}
inline std::string withoutText(std::string text, const std::string& needle) {
    const std::string n = compactText(needle);
    const size_t at = text.find(n);
    if (at != std::string::npos) text.erase(at, n.size());
    return text;
}
inline std::string replacedText(std::string text, const std::string& from, const std::string& to) {
    const std::string f = compactText(from);
    const size_t at = text.find(f);
    if (at != std::string::npos) text.replace(at, f.size(), compactText(to));
    return text;
}
// A plain (not whitespace-blind) replacement in raw source text, for mutating the scanner's input.
inline std::string replacedRaw(std::string text, const std::string& from, const std::string& to) {
    const size_t at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), to);
    return text;
}
inline std::string slurpFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
inline size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}
// The quoted strings of a source region (names in a table or the returns of a switch), and the longest list they make with 20-digit counters.
inline std::vector<std::string> quotedStrings(const std::string& region) {
    std::vector<std::string> out;
    for (size_t at = region.find('"'); at != std::string::npos;) {
        size_t j = at + 1;
        while (j < region.size() && region[j] != '"') { if (region[j] == '\\') ++j; ++j; }
        out.push_back(region.substr(at + 1, j - at - 1));
        at = region.find('"', j + 1);
    }
    return out;
}

inline int flatLogLineTests() {
    int failures = 0;
    const auto expect = [&](bool ok, const std::string& name) {
        if (!ok) { std::printf("FAIL: log lines %s\n", name.c_str()); ++failures; }
    };

    // ===== 1. the formatters at the worst digits ===============================================================================================
    const std::vector<Case> cases = buildCases();
    expect(kFlatLogLineBudget == 1000, "the budget is 1000 (the logger keeps 1167)");
    expect(kFlatSiblingLineParts == 4 && kFlatNoCandidateLineParts == 3 && kFlatSourceLineParts == 2 && kFlatHistoryWriteLines == 3 && kFlatShadowLineParts == 3,
           "the parts of the split lines: sibling 4, no-candidate 3, source 2, history writes 3, shadow 3");
    size_t worstOverall = 0;
    for (const Case& c : cases) {
        std::string lengths;
        for (int l : {kTwelve, kWidest}) {
            const Rendered r = render(c, l);
            const std::string label = c.name + " at " + levelName(l);
            bool terminated = true;
            for (unsigned p = 0; p < c.parts; ++p) {
                const std::string& text = r.parts[p];
                const int n = r.returned[p];
                terminated = terminated && n > 0 && static_cast<size_t>(n) == text.size();
                expect(n > 0 && static_cast<size_t>(n) == text.size(), label + " part " + std::to_string(p + 1) + " is terminated and fits 4096 bytes untruncated");
                expect(text.rfind(c.prefixes[p], 0) == 0, label + " part " + std::to_string(p + 1) + " starts with `" + c.prefixes[p] + "`" + (p == 0 ? " (the key the log is read by)" : ""));
                const size_t limit = kFlatLogLineBudget;   // every formatter part, at 12 digits and at 2^64-1
                expect(text.size() <= limit, label + " part " + std::to_string(p + 1) + " is " + std::to_string(text.size()) + " characters, over " + std::to_string(limit));
                worstOverall = (std::max)(worstOverall, text.size());
                char piece[48];
                std::snprintf(piece, sizeof(piece), " %zu", text.size());
                lengths += piece;
            }
            if (terminated) {
                const std::string wrong = checkFields(c, r.parts, l);
                expect(wrong.empty(), label + ": " + wrong);
            }
            lengths += l == kTwelve ? " |" : "";
        }
        std::printf("  log lines: %-22s parts (chars at 12 digits | at 20 digits):%s\n", c.name.c_str(), lengths.c_str());
        // an all-zero window prints every counter, at zero
        if (c.zeroable) {
            const Rendered z = render(c, kZero);
            bool zeroOk = true;
            for (unsigned p = 0; p < c.parts; ++p) {
                std::string text = z.parts[p];
                if (text.rfind(c.prefixes[p], 0) == 0) text.erase(0, c.prefixes[p].size());
                zeroOk = zeroOk && !z.parts[p].empty() && allZeros(text);
            }
            expect(zeroOk, c.name + " an all-zero window prints every counter at zero (and every part prints)");
            const std::string wrong = checkFields(c, z.parts, kZero);
            expect(wrong.empty(), c.name + " all-zero: " + wrong);
        }
        // a buffer too small truncates; a part past the last prints nothing
        const Case& cc = c;
        for (unsigned p = 0; p < c.parts; ++p) {
            char guard[200];
            std::memset(guard, 'g', sizeof(guard));
            cc.print(guard, 100, p, kWidest);
            bool cutHere = std::strlen(guard) == 99;
            for (size_t k = 100; k < sizeof(guard); ++k) cutHere = cutHere && guard[k] == 'g';
            expect(cutHere, c.name + " part " + std::to_string(p + 1) + " into a 100-byte buffer is cut at its last byte and writes nothing past it");
        }
        char beyond[64];
        expect(c.print(beyond, sizeof(beyond), c.parts, kWidest) == 0, c.name + " a part past the last prints nothing");
    }
    std::printf("  log lines: the longest formatter part measured is %zu characters (the budget is %zu, the logger keeps %zu)\n", worstOverall, kFlatLogLineBudget, kLoggerCut);

    // mutation controls for the field check
    {
        const Case& sib = cases[0];
        const Rendered r = render(sib, kTwelve);
        expect(checkFields(sib, r.parts, kTwelve).empty(), "mutation control baseline: the sibling line's twelve-digit parts pass the field check");
        std::vector<std::string> dropped = r.parts;
        dropped.erase(dropped.begin() + 2);
        expect(!checkFields(sib, dropped, kTwelve).empty(), "mutation control: the sibling line with one group of its patterns dropped fails the every-field-once check");
        std::vector<std::string> doubled = r.parts;
        doubled.push_back(r.parts[1]);
        expect(!checkFields(sib, doubled, kTwelve).empty(), "mutation control: a group of patterns printed twice fails the every-field-once check");
        std::vector<std::string> nohead = r.parts;
        nohead.erase(nohead.begin());
        expect(!checkFields(sib, nohead, kTwelve).empty(), "mutation control: the sibling line without its head fails the check");
        const Case& nc = cases[1];
        const Rendered nr = render(nc, kTwelve);
        std::vector<std::string> ncDropped = nr.parts;
        ncDropped.pop_back();
        expect(!checkFields(nc, ncDropped, kTwelve).empty(), "mutation control: the no-candidate line with its second group of patterns dropped fails the check");
        std::vector<std::string> wrongValue = r.parts;
        const size_t at = wrongValue[0].find("dispatches=999999999999");
        if (at != std::string::npos) wrongValue[0].replace(at, 23, "dispatches=999999999990");
        expect(at != std::string::npos && !checkFields(sib, wrongValue, kTwelve).empty(), "mutation control: a counter that prints another value fails the value check");
        const Rendered zr = render(sib, kZero);
        std::string nonZero = zr.parts[1].substr(sib.prefixes[1].size());
        expect(allZeros(nonZero), "mutation control baseline: the all-zero sibling part passes the all-zero check");
        const size_t eq = nonZero.find("=0/");
        if (eq != std::string::npos) nonZero[eq + 1] = '7';
        expect(eq != std::string::npos && !allZeros(nonZero), "mutation control: a window with a counter that is not zero fails the all-zero check");
    }

    // ===== 2. the flat cpu packer ===============================================================================================================
    {
        expect(flatcpu::kLineLimit == kFlatLogLineBudget, "the cpu packer's line limit is the repo's budget, not the truncation limit less a margin");
        expect(flatcpu::kMaxLines == 5, "the cpu packer holds five lines (a window is typically three)");
        flatcpu::WindowReport r;
        r.valid = true;
        r.frames = 5000;
        r.pausedFrames = 4999;
        r.sampledFrames = 156;
        r.renderClockedFrames = 313;
        r.standingDown = true;
        r.presentP50Ms = r.presentP95Ms = 99999.99;
        for (unsigned f = 0; f < flatcpu::kFamilies; ++f) {
            r.renderMs[f] = r.otherMs[f] = 9.9e9;
            r.renderCalls[f] = r.otherCalls[f] = 999999999999ull;
        }
        for (unsigned p = 0; p < emcpu::kParts; ++p) {
            r.emRenderMs[p] = r.emOtherMs[p] = 9.9e9;
            r.emRenderCalls[p] = r.emOtherCalls[p] = 999999999999ull;
        }
        r.stateCalls = r.substitutedDraws = 99999999999999ull;
        r.gpuFrameSamples = r.gpuResolveSamples = 2048;
        r.gpuFrameP50 = r.gpuFrameP95 = r.gpuResolveP50 = r.gpuResolveP95 = 99999.99;
        r.gpuSkipped = r.gpuInvalid = 999999999999ull;
        r.floor.measured = r.emFloor.measured = true;
        r.floor.costNs = r.floor.recordedNs = 99999;
        r.emFloor.plainCostNs = r.emFloor.pausedCostNs = r.emFloor.plainRecordedNs = r.emFloor.pausedRecordedNs = 99999;
        for (unsigned i = 0; i < kFlatQueryCount; ++i) {
            r.queries.served[i] = r.queries.sampled[i] = r.queries.mismatched[i] = 999999999999ull;
            r.queries.asked[i] = 5000000ull;   // 1,000 a frame over these 5,000 frames: heavy. (At 999999999999 the token`s 200-byte name list is cut mid-entry; see the report)
            r.queries.fellBack |= 1u << i;   // every state asks the context again: the "ASKS THE CONTEXT AGAIN" token at its longest
        }
        flatcpu::Lines lines;
        flatcpu::formatWindow(r, &lines);
        std::string lengths;
        bool balanced = true, noDangling = true;
        for (int i = 0; i < lines.count; ++i) {
            lengths += " " + std::to_string(std::strlen(lines.line[i]));
            balanced = balanced && parenthesesBalanced(lines.line[i]);
            const std::string s = lines.line[i];
            for (const char* sep : {" + ", " = ", "; ", ", ", ": "})
                noDangling = noDangling && !(s.size() >= std::strlen(sep) && s.compare(s.size() - std::strlen(sep), std::strlen(sep), sep) == 0);
        }
        std::printf("  log lines: flat cpu packer, the worst plausible window (every family and span, 12-digit figures): %d lines of%s characters (limit %zu, room for %d)\n",
                    lines.count, lengths.c_str(), flatcpu::kLineLimit, flatcpu::kMaxLines);
        expect(lines.count >= 2 && lines.count <= flatcpu::kMaxLines && !lines.truncated, "the worst window packs into at most kMaxLines lines and nothing is dropped");
        expect(linesWithin(lines, kFlatLogLineBudget), "every packed line is within the budget (1000)");
        expect(std::strncmp(lines.line[0], "flat cpu 5s: ", 13) == 0, "the first packed line starts `flat cpu 5s:`");
        bool continued = true;
        for (int i = 1; i < lines.count; ++i) continued = continued && std::strncmp(lines.line[i], "flat cpu 5s (cont.): ", 21) == 0;
        expect(continued, "every other packed line starts `flat cpu 5s (cont.):`");
        if (!balanced || !noDangling)
            for (int i = 0; i < lines.count; ++i) {
                const char* ask = std::strstr(lines.line[i], "ASKS THE CONTEXT");
                std::printf("  log lines: packed line %d balanced=%d: ...%s | starts %.40s | %s\n", i + 1, parenthesesBalanced(lines.line[i]) ? 1 : 0,
                            std::strlen(lines.line[i]) > 60 ? lines.line[i] + std::strlen(lines.line[i]) - 60 : lines.line[i], lines.line[i], ask ? ask : "");
            }
        expect(balanced && noDangling, "no token is split across lines: every line's brackets close and none ends on a separator");
        std::string packed;
        for (int i = 0; i < lines.count; ++i) { packed += lines.line[i]; packed += '\n'; }
        expect(packed.find("ASKS THE CONTEXT AGAIN: ") != std::string::npos && packed.find("of the per-clocked-frame total above into it") != std::string::npos &&
                   packed.find("every other family ") != std::string::npos,
               "and the longest tokens (the context fallback, the clock price, the families) are on the lines whole");

        // The oracle must notice a packer of 1090: three tokens of 319 characters and a fourth of 40. At 1000 the fourth goes to a second line; at 1090 it
        // makes the first 1,005 characters.
        flatcpu::Tokens tk;
        const std::string a(319, 'A'), b(319, 'B'), c3(319, 'C'), d(40, 'D');
        tk.add("", "%s", a.c_str());
        tk.add(" + ", "%s", b.c_str());
        tk.add(" + ", "%s", c3.c_str());
        tk.add("; ", "%s", d.c_str());
        flatcpu::Lines real, mutant;
        flatcpu::packLines(tk, &real);
        packAt(tk, &mutant, 1090);
        expect(real.count == 2 && !real.truncated && linesWithin(real, kFlatLogLineBudget) && std::string(real.line[0]).find(a) != std::string::npos &&
                   std::string(real.line[0]).find(c3) != std::string::npos && std::string(real.line[1]).find(d) != std::string::npos,
               "the packer puts a token that would pass 1000 on the next line, whole");
        expect(mutant.count == 1 && !linesWithin(mutant, kFlatLogLineBudget),
               "mutation control: the same tokens packed at a limit of 1090 make a first line over the budget, which the check refuses");
        const std::string cpuHeader = slurpFile("src/d3d11/flat_cpu.h");
        expect(cpuHeader.find("constexpr size_t kLineLimit = 1000;") != std::string::npos && cpuHeader.find("constexpr int kMaxLines = 5;") != std::string::npos,
               "flat_cpu.h declares a line limit of 1000 and five lines");
        const std::string cpuCompact = compactText(cpuHeader);
        const auto limitValid = [&](const std::string& text) { return text.find(compactText("constexpr size_t kLineLimit = 1000;")) != std::string::npos; };
        expect(limitValid(cpuCompact), "the pin on the packer's limit holds on the source");
        expect(!limitValid(replacedText(cpuCompact, "constexpr size_t kLineLimit = 1000;", "constexpr size_t kLineLimit = 1090;")),
               "mutation control: a packer limit raised to 1090 fails the pin");
    }

    // ===== 3. the source scan ===================================================================================================================
    const std::vector<Measured> measured = measuredFormatters(cases);
    const size_t cpuLimit = flatcpu::kLineLimit;
    {
        // the estimator on formats of known width
        const auto est = [](const char* f, unsigned digits, size_t cap = 32) {
            return estimateFormat(f, digits, [cap](const Conversion&) { return cap; });
        };
        expect(est("%llu", 20) == 20 && est("%llu", 12) == 12 && est("%zu", 20) == 20 && est("%llX", 20) == 20 && est("%llX", 12) == 12,
               "estimator: a 64-bit conversion is the counter's digits");
        expect(est("%u", 20) == 10 && est("%d", 20) == 11 && est("%i", 12) == 11 && est("%X", 20) == 8 && est("%08X", 20) == 8 && est("%x", 12) == 8 &&
                   est("%lu", 20) == 10,
               "estimator: a 32-bit conversion is 10, 11 signed, 8 in hex");
        expect(est("%016llX", 20) == 16 && est("%p", 20) == 18 && est("%c", 20) == 1, "estimator: a padded 64-bit hex is 16, a pointer 18, a character 1");
        expect(est("%.1f", 20) == 22 && est("%.3f", 20) == 24 && est("%.0f", 20) == 21 && est("%.9g", 20) == 17, "estimator: a float is 21 + the precision, a %g the precision + 8");
        expect(est("%lld", 20) == 20 && est("%lld", 12) == 13, "estimator: a signed 64-bit conversion has room for its sign");
        expect(est("%%", 20) == 1 && est("ab\\n\\\"", 20) == 4 && est("a%sb", 20, 10) == 12 && est("%.5s", 20, 100) == 5 && est("%s %s", 20, 7) == 15,
               "estimator: %% is one, an escape is one, a %s is its cap (cut by a precision)");
        // an over-long format fails the net; a short one does not
        std::string longFormat = "flat synthetic line:";
        for (int i = 0; i < 60; ++i) longFormat += " counter-" + std::to_string(i) + "=%llu";
        const std::string synthetic = "void f() {\n    Log::get().note(\"" + longFormat + "\",\n        0);\n    Log::get().note(\"flat synthetic short: a=%llu\", 1);\n}\n";
        const std::vector<NoteCall> syn = scanNotes(synthetic, "src/d3d11/flat_synthetic.cpp");
        expect(syn.size() == 2 && syn[0].literal && syn[0].format == longFormat, "the scanner reads a call's literal format");
        Verdict sv;
        checkNotes(syn, measured, cpuLimit, {}, sv, false, false);
        expect(estimateFormat(longFormat, 12, [](const Conversion&) { return kDefaultStr; }) > kLoggerCut && sv.failures.size() == 1 &&
                   sv.failures[0].find("flat synthetic line:") != std::string::npos,
               "mutation control: a synthetic over-long periodic line fails the net, and only it");
        // comments, strings and other receivers are not calls; adjacent literals and raw strings are read
        const std::string tricky =
            "// Log::get().note(\"flat commented: a=%llu\", 1);\n"
            "/* Log::get().note(\"flat blocked: a=%llu\", 1); */\n"
            "const char* s = \"Log::get().note(\\\"flat quoted: a=%llu\\\", 1);\";\n"
            "const char* r = R\"x(Log::get().note(\"flat raw: a=%llu\", 1);)x\";\n"
            "ledger.note(frame, key, 3);\n"
            "Log::get().note(\"flat joined: a=%llu \"\n    \"b=%u\", x, y);\n";
        const std::vector<NoteCall> trick = scanNotes(tricky, "src/d3d11/flat_synthetic.cpp");
        expect(trick.size() == 1 && trick[0].format == "flat joined: a=%llu b=%u" && trick[0].args.size() == 2 && trick[0].line == 6,
               "the scanner skips comments, strings, raw strings and other receivers, joins adjacent literals and reports the call's line");

        // the real tree
        int files = 0;
        const std::vector<NoteCall> all = scanTree("src", &files);
        expect(files > 10 && all.size() > 300, "the source tree is readable from the repo root and holds the note calls (" + std::to_string(all.size()) + " in " + std::to_string(files) + " files)");
        Verdict v;
        const std::vector<Allow> allow = allowList();
        checkNotes(all, measured, cpuLimit, allow, v, true);
        for (const std::string& f : v.failures) expect(false, f);
        std::printf("  log lines: the net read %d Log::get().note( calls in %d files; %d in the flat / engine-motion scope, %d with a literal format; %d pass-throughs "
                    "(%d of a buffer under the cut, %d of a measured formatter's line); %d allow-listed; %zu failures\n",
                    v.calls, files, v.inScope, v.literalCalls, v.passThrough, v.bufferBound, v.formatterFed, v.allowed, v.failures.size());
        for (const std::string& line : v.report) std::printf("  log lines: %s\n", line.c_str());

        // the list sizes the table's caps stand on, read from the source
        const std::string engineSource = slurpFile("src/d3d11/engine_velocity.cpp");
        const size_t ia = engineSource.find("const char* const kInvalidNames[kInvalidCount] = {");
        const std::vector<std::string> invalidNames = ia == std::string::npos ? std::vector<std::string>() : quotedStrings(engineSource.substr(ia, engineSource.find("};", ia) - ia));
        size_t invalidList = 0;
        for (const std::string& name : invalidNames) invalidList += name.size() + 1 + 20 + 2;
        const std::string stateSource = slurpFile("src/d3d11/engine_velocity_state.h");
        const size_t ra = stateSource.find("inline const char* engineVelocityBindRefusalName(");
        std::vector<std::string> refusalNames;
        if (ra != std::string::npos)
            for (const std::string& name : quotedStrings(stateSource.substr(ra, stateSource.find("\n}\n", ra) - ra))) if (name != "none") refusalNames.push_back(name);
        size_t refusalList = 0;
        for (const std::string& name : refusalNames) refusalList += name.size() + 1 + 20 + 2;
        std::printf("  log lines: the movers' lists at 20-digit counters: invalidation list %zu characters of 700 (%zu names), MRT6 refusal list %zu of 350 (%zu names)\n",
                    invalidList, invalidNames.size(), refusalList, refusalNames.size());
        expect(!invalidNames.empty() && invalidList <= 700, "the movers' invalidation list stays within the 700 characters the table gives it");
        expect(!refusalNames.empty() && refusalList <= 350, "the movers' MRT6 refusal list stays within the 350 characters the table gives it");
        // the on-foot line's two lists: the dropped-frames list is the invalidation names again; the declined list is kSourceDeclineNames
        const size_t da = engineSource.find("const char* const kSourceDeclineNames[kSourceDeclineCount] = {");
        const std::vector<std::string> declineNames = da == std::string::npos ? std::vector<std::string>() : quotedStrings(engineSource.substr(da, engineSource.find("};", da) - da));
        size_t declineList = 0;
        for (const std::string& name : declineNames) declineList += name.size() + 1 + 20 + 2;
        std::printf("  log lines: the on-foot lists at 20-digit counters: dropped-frames list %zu characters of 470 (%zu names), declined list %zu of 220 (%zu names)\n",
                    invalidList, invalidNames.size(), declineList, declineNames.size());
        expect(!invalidNames.empty() && invalidList <= 470, "the on-foot dropped-frames list stays within the 470 characters the table gives it");
        expect(!declineNames.empty() && declineList <= 220, "the on-foot declined list stays within the 220 characters the table gives it");

        // mutation controls on the real source: the lines re-merged, a row renamed
        const auto rescanned = [&](const char* fileName, const std::string& text) {
            std::vector<NoteCall> mutated;
            for (const NoteCall& call : all) if (call.file.size() < std::strlen(fileName) || call.file.compare(call.file.size() - std::strlen(fileName), std::string::npos, fileName) != 0) mutated.push_back(call);
            const std::vector<NoteCall> again = scanNotes(text, std::string("src/d3d11/") + fileName);
            mutated.insert(mutated.end(), again.begin(), again.end());
            return mutated;
        };
        const auto merge = [](std::string text, const std::string& endsWith, const std::string& nextLead) {
            const size_t a = text.find(endsWith);
            if (a == std::string::npos) return std::string();
            const size_t bAt = text.find(nextLead, a);
            if (bAt == std::string::npos) return std::string();
            text.replace(a + endsWith.size(), bAt + nextLead.size() - (a + endsWith.size()), " ");
            return text;
        };
        const auto fails = [&](const std::vector<NoteCall>& calls, const char* mention) {
            Verdict mv;
            checkNotes(calls, measured, cpuLimit, allow, mv, false);
            for (const std::string& f : mv.failures) if (f.find(mention) != std::string::npos) return true;
            return false;
        };
        std::string runtimeText = slurpFile("src/d3d11/flat_runtime.cpp");
        std::string remerged = merge(runtimeText, "repeated-geometry=%llu", "flat foreground SDK domain (2/3):");
        remerged = merge(remerged, "H-attempts=%llu H-qualified=%llu", "flat foreground SDK domain (3/3):");
        expect(!remerged.empty() && fails(rescanned("flat_runtime.cpp", remerged), "SDK domain: configured="),
               "mutation control: the SDK domain line re-merged into one string fails (its three parts are gone and the whole is over the budget)");
        expect(!fails(rescanned("flat_runtime.cpp", runtimeText), "SDK domain"), "mutation control baseline: the unmutated source passes");
        std::string moversMerged = merge(engineSource, "under the old order %llu)", "engine motion: movers (2/4):");
        moversMerged = merge(moversMerged, "pool appended and refreshed %llu", "engine motion: movers (3/4):");
        moversMerged = merge(moversMerged, "slot target create failed %llu", "engine motion: movers (4/4):");
        expect(!moversMerged.empty() && fails(rescanned("engine_velocity.cpp", moversMerged), "movers joined"),
               "mutation control: the engine movers line re-merged into one string fails");
        std::string onFootMerged = merge(engineSource, "unwritten %llu, no previous scene constants %llu", "engine motion: on foot (2/3):");
        onFootMerged = onFootMerged.empty() ? onFootMerged : merge(onFootMerged, "declined %llu in %llu frames (%s)", "engine motion: on foot, another camera:");
        onFootMerged = onFootMerged.empty() ? onFootMerged : merge(onFootMerged, "frames (%s)  %s", "engine motion: on foot (3/3):");   // (the second merge left two spaces before the `other` %s)
        expect(!onFootMerged.empty() && fails(rescanned("engine_velocity.cpp", onFootMerged), "on foot"),
               "mutation control: the three on-foot notes re-merged into one string fails");
        expect(!fails(rescanned("engine_velocity.cpp", engineSource), "on foot"), "mutation control baseline: the unmutated on-foot notes pass");
        const std::string renamed = replacedRaw(runtimeText, "flat foreground SDK domain (3/3):", "flat foreground SDK domain (3 of 3):");
        expect(fails(rescanned("flat_runtime.cpp", renamed), "SDK domain (3/3):"), "mutation control: a split line whose part was renamed fails the table (a row matches no call)");
        // the net: a new long line in a flat file, a pass-through of an unmeasured buffer, a stale allow-list entry
        std::vector<NoteCall> plus = all;
        plus.insert(plus.end(), syn.begin(), syn.end());
        expect(fails(plus, "flat synthetic line:"), "mutation control: a new over-long periodic line anywhere in the flat files fails the net");
        const std::string unmeasured =
            "void g() {\n    char line[4096];\n    makeSomeLine(line, sizeof(line));\n    Log::get().note(\"%s\", line);\n}\n"
            "void h() {\n    char line[4096];\n    flatSiblingLine(line, sizeof(line), w, 0);\n    Log::get().note(\"%s\", line);\n}\n"
            "void i() {\n    char text[512];\n    makeSomeLine(text, sizeof(text));\n    Log::get().note(\"%s\", text);\n}\n";
        const std::vector<NoteCall> pass = scanNotes(unmeasured, "src/d3d11/flat_synthetic.cpp");
        Verdict pv;
        checkNotes(pass, measured, cpuLimit, {}, pv, false, false);
        expect(pass.size() == 3 && pv.failures.size() == 1 && pv.failures[0].find("flat_synthetic.cpp:4") != std::string::npos,
               "mutation control: a pass-through of a 4096-byte buffer with no measured formatter before it fails the net; one after a measured formatter, or of a 512-byte buffer, does not");
        std::vector<Allow> stale = allowList();
        stale.push_back({"flat_runtime.cpp", "flat nothing like this:", "", "an entry for a line that no longer exists", false});
        Verdict stv;
        checkNotes(all, measured, cpuLimit, stale, stv, false);
        bool staleNamed = false;
        for (const std::string& f : stv.failures) staleNamed = staleNamed || f.find("flat nothing like this:") != std::string::npos;
        expect(staleNamed, "mutation control: an allow-list entry that matches no call fails (the list cannot rot)");
        Verdict nv;
        checkNotes(all, measured, cpuLimit, {}, nv, false);
        expect(nv.failures.size() == v.failures.size() + v.allowed, "mutation control: without its allow-list the net fails on exactly the calls the list names (" + std::to_string(v.allowed) + ")");
    }

    // ===== 4. the wiring ========================================================================================================================
    {
        const std::string runtime = compactText(slurpFile("src/d3d11/flat_runtime.cpp"));
        const std::string engine = compactText(slurpFile("src/d3d11/engine_velocity.cpp"));
        expect(!runtime.empty() && !engine.empty(), "the runtime and the engine motion sources are readable from the repo root");
        const auto ordered = [&](const std::string& text, std::initializer_list<const char*> needles) {
            size_t pos = 0;
            bool ok = !text.empty();
            for (const char* needle : needles) {
                const std::string n = compactText(needle);
                const size_t at = text.find(n, pos);
                if (at == std::string::npos) { ok = false; break; }
                pos = at + n.size();
            }
            return ok;
        };
        const char* const siblingLoop = "for(unsigned part=0;part<kFlatSiblingLineParts;++part){flatSiblingLine(line,sizeof(line),sibling,part);Log::get().note(\"%s\",line);}";
        const char* const candidateLoop = "for(unsigned part=0;part<kFlatNoCandidateLineParts;++part){flatNoCandidateLine(line,sizeof(line),w,part);Log::get().note(\"%s\",line);}";
        const char* const sourceLoop = "for(unsigned part=0;part<kFlatSourceLineParts;++part){flatSourceSpellLine(line,sizeof(line),w,s.sourcelessLast,s.sourcelessLastFrame,notes,part);Log::get().note(\"%s\",line);}";
        const auto loopsValid = [&](const std::string& text) {
            return countOf(text, compactText(siblingLoop)) == 1 && countOf(text, compactText(candidateLoop)) == 1 && countOf(text, compactText(sourceLoop)) == 1 &&
                   ordered(text, {"flatIdentityLine(line,sizeof(line),w);Log::get().note(\"%s\",line);", siblingLoop}) && ordered(text, {candidateLoop, "flatIdentityLine("});
        };
        expect(loopsValid(runtime), "the runtime prints the no-candidate and sibling lines and the source-spell line in loops over their parts constants, the identity line once");
        expect(!loopsValid(withoutText(runtime, siblingLoop)), "mutation control: a sibling line that is never printed fails the wiring");
        expect(!loopsValid(replacedText(runtime, "part<kFlatSiblingLineParts", "part<1")), "mutation control: a sibling line printed as its head alone fails the wiring");
        expect(!loopsValid(replacedText(runtime, "part<kFlatNoCandidateLineParts", "part<2")), "mutation control: a no-candidate line missing its last group fails the wiring");
        expect(!loopsValid(replacedText(runtime, "part<kFlatSourceLineParts", "part<1")), "mutation control: a source-spell line without its pairs fails the wiring");
        expect(!loopsValid(replacedText(runtime, "flatSourceSpellLine(line,sizeof(line),w,s.sourcelessLast,s.sourcelessLastFrame,notes,part)",
                                        "flatSourceSpellLine(line,sizeof(line),w,s.sourcelessLast,s.sourcelessLastFrame,notes)")),
               "mutation control: a source-spell loop that asks for the same part every time fails the wiring");
        const auto domainValid = [&](const std::string& text) {
            return ordered(text, {"reportForegroundNoCandidate(s,captures);", "Log::get().note(\"flat foreground SDK domain: configured=%s foreign-seen=%llu",
                                  "Log::get().note(\"flat foreground SDK domain (2/3): world-markers=%llu", "Log::get().note(\"flat foreground SDK domain (3/3): H-qualified-with-per-pixel-refusals=%llu",
                                  "s.foregroundHRefusalWindow=nullptr;"}) &&
                   countOf(text, compactText("Log::get().note(\"flat foreground SDK domain")) == 3;
        };
        expect(domainValid(runtime), "the SDK domain line is three notes, the first still beginning `flat foreground SDK domain: configured=`");
        expect(!domainValid(replacedText(runtime, "Log::get().note(\"flat foreground SDK domain (2/3): world-markers=%llu", "Log::get().note(\"flat foreground SDK domain (2 of 3): world-markers=%llu")),
               "mutation control: a second part under another key fails the wiring");
        expect(!domainValid(replacedText(runtime, "Log::get().note(\"flat foreground SDK domain: configured=%s foreign-seen=%llu", "Log::get().note(\"flat foreground SDK domain (1/3): configured=%s foreign-seen=%llu")),
               "mutation control: a first part that lost the original key fails the wiring");
        expect(!domainValid(withoutText(runtime, "Log::get().note(\"flat foreground SDK domain (3/3): H-qualified-with-per-pixel-refusals=%llu")),
               "mutation control: a missing third part fails the wiring");
        const auto moversValid = [&](const std::string& text) {
            return ordered(text, {"Log::get().note(\"engine motion: movers joined %.1f records/frame", "Log::get().note(\"engine motion: movers (2/4): invalidated %llu (%s)",
                                  "Log::get().note(\"engine motion: movers (3/4): MRT6 refused:", "Log::get().note(\"engine motion: movers (4/4): blend:"}) &&
                   countOf(text, compactText("Log::get().note(\"engine motion: movers")) == 4;
        };
        expect(moversValid(engine), "the engine movers line is four notes, the first still beginning `engine motion: movers joined`");
        expect(!moversValid(replacedText(engine, "Log::get().note(\"engine motion: movers (3/4): MRT6 refused:", "Log::get().note(\"engine motion: refusals: MRT6 refused:")),
               "mutation control: a movers part under another key fails the wiring");
        expect(!moversValid(replacedText(engine, "Log::get().note(\"engine motion: movers joined %.1f records/frame", "Log::get().note(\"engine motion: movers (1/4): %.1f records/frame")),
               "mutation control: a first movers part that lost the original key fails the wiring");
        expect(!moversValid(withoutText(engine, "Log::get().note(\"engine motion: movers (4/4): blend:")), "mutation control: a missing fourth movers part fails the wiring");
        const auto onFootValid = [&](const std::string& text) {
            return ordered(text, {"Log::get().note(\"engine motion: on foot: source frames %llu", "Log::get().note(\"engine motion: on foot (2/3): camera rule: namings %llu",
                                  "if(other[0])Log::get().note(\"engine motion: on foot, another camera: %s\",other);", "Log::get().note(\"engine motion: on foot (3/3): %s.\",pixels);"}) &&
                   countOf(text, compactText("Log::get().note(\"engine motion: on foot")) == 4;
        };
        expect(onFootValid(engine), "the engine on-foot summary is three notes and a conditional fourth, the first still beginning `engine motion: on foot:`");
        expect(!onFootValid(replacedText(engine, "Log::get().note(\"engine motion: on foot (2/3): camera rule", "Log::get().note(\"engine motion: on foot camera rule")),
               "mutation control: an on-foot part under another key fails the wiring");
        expect(!onFootValid(withoutText(engine, "if(other[0])")), "mutation control: an other-camera line printed whether or not another camera moved the rows fails the wiring");
        expect(!onFootValid(withoutText(engine, "Log::get().note(\"engine motion: on foot (3/3): %s.\",pixels);")), "mutation control: a missing pixels part fails the wiring");
    }

    return failures;
}

}  // namespace flat_log_lines

inline int flatLogLineTests() { return flat_log_lines::flatLogLineTests(); }
