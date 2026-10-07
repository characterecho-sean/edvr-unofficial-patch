#pragma once
// The lines that name why a first-person draw had no same-key prior, and why one with priors was not matched (design doc section 104, the
// pistol's no-candidate bursts). Plain text from plain counters: tools\flat_temporal_test builds the lines from known counts and holds
// their keys to the names the patterns carry, so a log reader and the code that writes the line cannot drift apart.
//
// `flat foreground no-candidate 5s:` carries, for the 5 s window, every pattern of animated_history_ledger.h (HistoryGap), zeros included:
// the patterns are exclusive and sum to same-key-misses. `flat foreground identity 5s:` carries every verdict of
// flat_foreground_identity_verdict.h, zeros included. The example lines follow, the first draw of each pattern with its whole key.
#include <cstdint>
#include <cstdio>
#include "animated_history_ledger.h"
#include "flat_foreground_identity_verdict.h"

namespace edvr {

struct FlatNoCandidateWindow {
    uint64_t submitted = 0, noCandidate = 0, acrossOffset = 0, frames = 0, framesMissing = 0, framesAllMissing = 0, resetFrames = 0, nearChanges = 0;
    uint64_t rescueCancelled = 0;
    uint64_t missBy[kHistoryGapCount] = {};
    unsigned longestRun = 0;
    uint64_t identitySamples = 0, identitySkipped = 0, identityUnread = 0;
    uint64_t identityBy[kIdentityVerdictCount] = {};
    uint64_t misses() const { uint64_t n = 0; for (uint64_t v : missBy) n += v; return n; }
};

// The key fields in `diff`, comma-separated, or "none".
inline int flatHistoryDiffText(unsigned diff, char* out, size_t n) {
    static const struct { unsigned bit; const char* name; } fields[] = {
        {kDiffVs, "shader"}, {kDiffLayout, "layout"}, {kDiffVertices, "vertices"}, {kDiffIndices, "indices"}, {kDiffCount, "count"},
        {kDiffStart, "start"}, {kDiffBase, "base"}, {kDiffOffset, "vb-offset"}, {kDiffStride, "stride"}, {kDiffIndexOffset, "ib-offset"},
        {kDiffFormat, "format"}};
    size_t at = 0;
    if (n) out[0] = 0;
    for (const auto& f : fields) {
        if (!(diff & f.bit)) continue;
        const int w = std::snprintf(out + at, n > at ? n - at : 0, "%s%s", at ? "," : "", f.name);
        if (w < 0) break;
        at += static_cast<size_t>(w);
    }
    if (!at) return std::snprintf(out, n, "none");
    return static_cast<int>(at);
}

inline int flatNoCandidateLine(char* out, size_t n, const FlatNoCandidateWindow& w) {
    char head[1024];
    std::snprintf(head, sizeof(head),
        "flat foreground no-candidate 5s: submitted=%llu same-key-misses=%llu rescued-offset-shift=%llu rescue-cancelled=%llu still-no-candidate=%llu "
        "frames=%llu frames-with-misses=%llu frames-all-missed=%llu longest-miss-run=%u reset-frames=%llu near-changes=%llu",
        static_cast<unsigned long long>(w.submitted), static_cast<unsigned long long>(w.misses()),
        static_cast<unsigned long long>(w.acrossOffset), static_cast<unsigned long long>(w.rescueCancelled),
        static_cast<unsigned long long>(w.noCandidate),
        static_cast<unsigned long long>(w.frames), static_cast<unsigned long long>(w.framesMissing),
        static_cast<unsigned long long>(w.framesAllMissing), w.longestRun,
        static_cast<unsigned long long>(w.resetFrames), static_cast<unsigned long long>(w.nearChanges));
    char patterns[1536];
    size_t at = 0;
    patterns[0] = 0;
    for (unsigned i = 0; i < kHistoryGapCount; ++i) {
        const int k = std::snprintf(patterns + at, sizeof(patterns) - at, " %s=%llu", historyGapName(static_cast<HistoryGap>(i)),
                                    static_cast<unsigned long long>(w.missBy[i]));
        if (k < 0 || at + static_cast<size_t>(k) >= sizeof(patterns)) break;
        at += static_cast<size_t>(k);
    }
    return std::snprintf(out, n,
        "%s; by pattern:%s; a miss is a submitted draw with no record of its exact geometry key that the frame before used; the patterns are "
        "exclusive and sum to same-key-misses; offset-shift (the same mesh at another place in the same buffers, one record) is the one pattern "
        "the history acts on and rescued-offset-shift must equal it; rescue-cancelled counts rescues withdrawn at the end of the frame because "
        "another draw used the donor record (two parts of one object, not a move); record-unusable must be 0; still-no-candidate is what stayed without a "
        "prior; frames-all-missed counts frames in which every submitted draw missed (a whole-set cause), longest-miss-run the consecutive "
        "frames with a miss; reset-frames are frames H asked the backend to reset history for, near-changes those caused by the common near plane",
        head, patterns);
}

inline int flatIdentityLine(char* out, size_t n, const FlatNoCandidateWindow& w) {
    char verdicts[512];
    size_t at = 0;
    verdicts[0] = 0;
    for (unsigned i = 0; i < kIdentityVerdictCount; ++i) {
        const int k = std::snprintf(verdicts + at, sizeof(verdicts) - at, " %s=%llu", identityVerdictName(static_cast<IdentityVerdict>(i)),
                                    static_cast<unsigned long long>(w.identityBy[i]));
        if (k < 0 || at + static_cast<size_t>(k) >= sizeof(verdicts)) break;
        at += static_cast<size_t>(k);
    }
    return std::snprintf(out, n,
        "flat foreground identity 5s: sampled=%llu by verdict:%s skipped-total=%llu unread-total=%llu; one draw in thirteen that the map matches by "
        "identity has its identity words and its priors' read back a few frames later and classified by the map's own two tests: match means the "
        "map takes the history, x-differs / parameter-differs / both-differ are the reason-5 (identity-differs) causes (the first word, the "
        "signature word without byte 30, or both), current-unauthentic and priors-unreadable are reasons 2 and 6",
        static_cast<unsigned long long>(w.identitySamples), verdicts, static_cast<unsigned long long>(w.identitySkipped),
        static_cast<unsigned long long>(w.identityUnread));
}

inline int flatHistoryKeyText(char* out, size_t n, const HistoryKey& k) {
    return std::snprintf(out, n, "shader=%p layout=%p vertices=%p indices=%p count=%u start=%u base=%d vb-offset=%u stride=%u ib-offset=%u format=%u",
        k.vs, k.layout, k.vertices, k.indices, k.count, k.start, k.base, k.offset, k.stride, k.indexOffset, k.format);
}

inline int flatNoCandidateExampleLine(char* out, size_t n, unsigned long long frame, const HistoryClass& miss, const HistoryKey& key,
                                      bool rescued, uint64_t vs, uint64_t ps) {
    char own[320], nearest[400] = "none", diff[160];
    flatHistoryKeyText(own, sizeof(own), key);
    flatHistoryDiffText(miss.diff, diff, sizeof(diff));
    if (miss.hasNearest) {
        char text[320];
        flatHistoryKeyText(text, sizeof(text), miss.nearest);
        std::snprintf(nearest, sizeof(nearest), "%s", text);
    }
    return std::snprintf(out, n,
        "flat foreground no-candidate example: frame=%llu pattern=%s rescued=%u age=%u VS=%016llX PS=%016llX key: %s; nearest the frame before: %s; "
        "differs: %s",
        frame, historyGapName(miss.gap), rescued ? 1u : 0u, miss.age, static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps),
        own, nearest, diff);
}

inline int flatIdentityExampleLine(char* out, size_t n, unsigned long long frame, IdentityVerdict verdict, const IdentityWords& current,
                                   const IdentityWords* priors, unsigned count, const HistoryKey& key, uint64_t vs, uint64_t ps) {
    char own[320];
    flatHistoryKeyText(own, sizeof(own), key);
    char words[512];
    size_t at = 0;
    words[0] = 0;
    for (unsigned i = 0; i < count && i < 4; ++i) {
        const int k = std::snprintf(words + at, sizeof(words) - at, " prior%u=(%08X,%08X,%08X)", i, priors[i].x, priors[i].y, priors[i].z);
        if (k < 0 || at + static_cast<size_t>(k) >= sizeof(words)) break;
        at += static_cast<size_t>(k);
    }
    return std::snprintf(out, n,
        "flat foreground identity example: frame=%llu verdict=%s priors=%u VS=%016llX PS=%016llX current=(%08X,%08X,%08X)%s key: %s",
        frame, identityVerdictName(verdict), count, static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps),
        current.x, current.y, current.z, words, own);
}

}  // namespace edvr
