#pragma once
// A DRAW WITH NO HISTORY OF ITS OWN TAKES ITS SIBLINGS' MOTION (design doc section 104, the pistol's LOD swap).
//
// The first-person map gives a draw the motion of the previous frame's draw of the same geometry (flat_foreground_motion_shader.h). A draw
// the history has no record of, because the mesh was swapped for another level of detail (count-change), came back after some frames
// (absent-short, absent-long) or was never seen (new-key), or because its prior is another record (identity-differs), was refused: its pixels
// showed the raw current frame, which at the pistol's scale is a white flash. The object it belongs to did not stop moving. Pieces of one
// object are drawn under one pool identity, and those that matched their history this frame say how the object moved.
//
// The pass (flat_foreground_motion.h; shaders in flat_foreground_motion_shader.h) takes the draws of the frame that matched, groups them by
// identity, and gives a draw without history their uniform motion, or the view's own when it has no sibling. This header is the part that
// needs no device: the pattern and outcome numbering the 5 s line prints, the CPU form of the decision the second shader makes (the rig
// holds the shader to it, vertex for vertex, and the policy is pinned here), and the line.
//
// WHAT IS GIVEN, AND WHY IT IS SOUND. Motion is the vertex's own, in pixels, as the map would have written it: a donor is a vertex whose
// prior was read and matched. The mean over the donors is the object's motion only if the object's pieces agree: a held object that rotates
// moves its pieces differently, and a uniform translation would be wrong by the difference. So the donors' spread per axis (the largest
// motion less the smallest, over every donor vertex) must be at most kFlatSiblingSpreadPixels, one pixel: the resampling of the history is
// bilinear, and an error under a pixel is inside what it already blurs. Past it the draw is refused as it was (disagree). With no donor,
// the motion is none: a first-person draw is attached to the view (its camera is the first person's, near 0.0675 against the world's 0.025),
// so its position against the previous frame's view does not change unless the object itself moves, which the draw's own siblings would show;
// with none the object is new or all of it lost its history together, and nothing in the frame says it moved. Its history is kept.
//
// WHAT STAYS REFUSED. A current draw that is not valid (reason 1), a pool row that is not authentic (2), prior positions that cannot be read
// or are ambiguous (4, 7): nothing says what the draw's motion would be. And a draw whose own identity cannot be read (mode 4), which could
// not be matched to siblings by identity.
#include <cstdint>
#include <cstdio>
#include "animated_history_ledger.h"

namespace edvr {

inline constexpr float kFlatSiblingSpreadPixels = 1.0f;      // the donors' spread per axis (the map's pixel units)
inline constexpr unsigned kFlatSiblingMinVertices = 3;       // fewer donor vertices than one triangle is no sibling
inline constexpr uint32_t kFlatSiblingIdentityMask = 0xFF00FFFFu;   // identity.y with byte 30 (a per-instance parameter) left out

// The fit table's mode for a draw (flat_foreground_motion_shader.h).
enum class SiblingOutcome : uint8_t {
    Matched = 0,        // the draw matched its own history: it needs no sibling (not an outcome of the pass)
    Sibling,            // the mean motion of its donors
    ViewAttached,       // no donor: the view's own motion (none), history kept
    Disagree,           // donors that do not agree to within the limit: refused as before
    IdentityUnreadable, // the draw's own pool identity cannot be read: refused as before
    Count
};
inline constexpr unsigned kSiblingOutcomes = 4;   // the four a no-history draw can take: Sibling .. IdentityUnreadable
inline const char* siblingOutcomeName(unsigned i) {
    static const char* const names[kSiblingOutcomes] = {"sibling", "view-attached", "disagree", "identity-unreadable"};
    return i < kSiblingOutcomes ? names[i] : "unknown";
}

// What a draw without history is filed under: the pattern the history gave (HistoryGap, every one), a draw whose exact key had a record that
// no prior came of (the pool or near differs, a rescue withdrawn), and a draw that had priors and none matched by identity.
inline constexpr unsigned kSiblingPriorFiltered = kHistoryGapCount;
inline constexpr unsigned kSiblingIdentityDiffers = kHistoryGapCount + 1;
inline constexpr unsigned kSiblingPatterns = kHistoryGapCount + 2;
inline const char* siblingPatternName(unsigned i) {
    if (i < kHistoryGapCount) return historyGapName(static_cast<HistoryGap>(i));
    if (i == kSiblingPriorFiltered) return "prior-filtered";
    if (i == kSiblingIdentityDiffers) return "identity-differs";
    return "unknown";
}

// One donor record as the first shader writes it: three uint4 a draw, the second and third holding float bit patterns.
struct SiblingDonor {
    bool matched = false;            // the draw's prior matched by identity
    uint32_t vertices = 0;           // the vertices whose motion was taken
    uint32_t identityX = 0, identityY = 0;   // identity.x, identity.y with byte 30 left out
    float mean[2] = {0, 0};
    float lo[2] = {0, 0}, hi[2] = {0, 0};
};

struct SiblingFit {
    SiblingOutcome outcome = SiblingOutcome::Matched;
    float motion[2] = {0, 0};
    float spread = 0;
    unsigned donors = 0;
    float vertices = 0;
};

// The second shader's decision for draw `self`, whose own identity words are `id` (x, y, z; z zero is unreadable). The donors are every draw's
// record, `self`'s included (it is never a donor of itself: it did not match).
inline SiblingFit siblingDecide(const SiblingDonor* draws, unsigned count, unsigned self, const uint32_t (&id)[4],
                                float spreadLimit = kFlatSiblingSpreadPixels, unsigned minVertices = kFlatSiblingMinVertices) {
    SiblingFit out;
    if (self >= count || draws[self].matched) return out;
    if (id[2] == 0) { out.outcome = SiblingOutcome::IdentityUnreadable; return out; }
    float n = 0;
    float sum[2] = {0, 0}, lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (unsigned e = 0; e < count; ++e) {
        const SiblingDonor& d = draws[e];
        if (!d.matched || d.vertices == 0 || d.identityX != id[0] || d.identityY != (id[1] & kFlatSiblingIdentityMask)) continue;
        const float c = static_cast<float>(d.vertices);
        for (int a = 0; a < 2; ++a) {
            sum[a] += d.mean[a] * c;
            lo[a] = d.lo[a] < lo[a] ? d.lo[a] : lo[a];
            hi[a] = d.hi[a] > hi[a] ? d.hi[a] : hi[a];
        }
        n += c;
        ++out.donors;
    }
    out.vertices = n;
    if (out.donors == 0 || n < static_cast<float>(minVertices)) { out.outcome = SiblingOutcome::ViewAttached; return out; }
    const float sx = hi[0] - lo[0], sy = hi[1] - lo[1];
    out.spread = sx > sy ? sx : sy;
    if (out.spread <= spreadLimit) {
        out.outcome = SiblingOutcome::Sibling;
        out.motion[0] = sum[0] / n;
        out.motion[1] = sum[1] / n;
    } else {
        out.outcome = SiblingOutcome::Disagree;
    }
    return out;
}

// The counters of a window: the passes made, the draws read back, and every no-history draw by pattern and outcome. Cumulative in the
// capture stats; the runtime prints the difference from the last line.
struct FlatSiblingWindow {
    uint64_t engagedFrames = 0, dispatches = 0, readbacks = 0, notReady = 0, failed = 0;
    uint64_t by[kSiblingPatterns][kSiblingOutcomes] = {};
    uint64_t total(unsigned outcome) const { uint64_t n = 0; for (unsigned p = 0; p < kSiblingPatterns; ++p) n += by[p][outcome]; return n; }
    uint64_t draws() const { uint64_t n = 0; for (unsigned o = 0; o < kSiblingOutcomes; ++o) n += total(o); return n; }
};

inline int flatSiblingLine(char* out, size_t n, const FlatSiblingWindow& w) {
    char patterns[2048];
    size_t at = 0;
    patterns[0] = 0;
    for (unsigned p = 0; p < kSiblingPatterns; ++p) {
        const int k = std::snprintf(patterns + at, sizeof(patterns) - at, " %s=%llu/%llu/%llu/%llu", siblingPatternName(p),
            static_cast<unsigned long long>(w.by[p][0]), static_cast<unsigned long long>(w.by[p][1]),
            static_cast<unsigned long long>(w.by[p][2]), static_cast<unsigned long long>(w.by[p][3]));
        if (k < 0 || at + static_cast<size_t>(k) >= sizeof(patterns)) break;
        at += static_cast<size_t>(k);
    }
    return std::snprintf(out, n,
        "flat foreground sibling 5s: engaged-frames=%llu dispatches=%llu draws-read=%llu (frames read=%llu unread=%llu failed=%llu) sibling=%llu "
        "view-attached=%llu disagree=%llu identity-unreadable=%llu; by pattern (sibling/view-attached/disagree/identity-unreadable):%s; a draw "
        "the history has no record for (the pattern) takes the mean motion of this frame's draws of the same pool identity that matched theirs "
        "(sibling), when those agree to within one pixel per axis; with none it is attached to the view and its history is kept "
        "(view-attached); donors that disagree (disagree) and an identity the pool cannot read (identity-unreadable) stay refused, as the "
        "reasons 3, 5 and 6 of the refusal census; the counts are of draws, read back from the GPU a few frames late",
        static_cast<unsigned long long>(w.engagedFrames), static_cast<unsigned long long>(w.dispatches),
        static_cast<unsigned long long>(w.draws()), static_cast<unsigned long long>(w.readbacks),
        static_cast<unsigned long long>(w.notReady), static_cast<unsigned long long>(w.failed),
        static_cast<unsigned long long>(w.total(0)), static_cast<unsigned long long>(w.total(1)),
        static_cast<unsigned long long>(w.total(2)), static_cast<unsigned long long>(w.total(3)), patterns);
}

}  // namespace edvr
