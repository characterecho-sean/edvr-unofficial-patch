// The Full System Scanner's reveal fix: which draws it recognises and when it acts. Pure, so the rig can run every decision on synthetic input.
//
// THE CAUSE (docs/fss-scanner.md, flight d7769063): the zoomed body's ring is revealed by a composite pass (VS 953C8123 / PS A473A867) that shows each cell's coarse input, t1, until the cell
// is revealed. t1 is a 63 x 63 downsample of the ring layer (VS DEF19B03 / PS 831DF02E). The left eye's pass draws that downsample BETWEEN the ring's two draw groups, so the left t1 lacks the
// front arc and the unrevealed cells show black; a second downsample, from the finished layer, runs just before the right composite, so the right eye shows a blurry ring instead. Stock Elite
// does this in every eye order it has, and flat has one eye, so there the black cells are the game's own look.
//
// THE FIX: at the first reveal composite of a frame, run the frame's first downsample again from the now finished layer, with the state it was issued with (fss_reveal_replay.h), so the left t1
// includes the second group too. Both eyes then show the right eye's look: blurry to sharp, no black squares.
//
// PS 831DF02E is a generic colour-matrix copy, so the hashes alone never decide: a downsample is replayed only when its render target is the texture the reveal composite binds as t1 in the
// same frame. RevealReplay is that decision and its tallies; the hook only feeds it.
#pragma once

#include <cstdint>

namespace edvr {
namespace fssreveal {

constexpr uint64_t kCompVs = 0x953C8123AD8DC13Bull, kCompPs = 0xA473A867BDDBC342ull;   // the reveal composite
constexpr uint64_t kDownVs = 0xDEF19B035D5EDEDCull, kDownPs = 0x831DF02EBA8AE814ull;   // the downsample that makes its t1

struct RevealReplay {
    enum Decision : uint8_t { Replay = 0, DeclFlat, DeclNoRecord, DeclOtherTarget, DeclT1, DeclDone, kDecisions };
    static constexpr uint32_t kRec = 4;
    // this frame
    const void* recRt[kRec] = {};
    uint32_t    recN = 0;
    bool        compSeen = false;
    bool        fAny = false, fReplayed = false, fDecl[kDecisions] = {};
    // the totals since the last resetTotals (a scanner session, in the log)
    uint32_t    frames = 0, framesWithComposite = 0, framesReplayed = 0, framesDeclined[kDecisions] = {};
    uint32_t    records = 0, recordDrops = 0, recordFailures = 0;
    uint32_t    replays = 0, replayFailures = 0, verifiedOk = 0, verifiedBad = 0, t1Unresolved = 0;

    // A downsample was issued into `rt` (hash match of both shaders). Returns the slot to store its state in, or -1 when the frame's table is full.
    int record(const void* rt) {
        fAny = true;
        if (recN >= kRec) { ++recordDrops; return -1; }
        recRt[recN] = rt;
        ++records;
        return static_cast<int>(recN++);
    }
    void recordFailed(int idx) {   // its state could not be captured: it can never be replayed
        if (idx >= 0 && idx < static_cast<int>(kRec)) recRt[idx] = nullptr;
        ++recordFailures;
    }
    // A reveal composite is about to draw. Only the first one of a frame decides; the rest are DeclDone (not tallied). On Replay, *idx is the record to replay.
    Decision composite(bool flat, const void* t1Res, int* idx) {
        fAny = true;
        if (compSeen) return DeclDone;
        compSeen = true;
        Decision d;
        if (flat) d = DeclFlat;
        else if (!t1Res) { d = DeclT1; ++t1Unresolved; }
        else {
            d = recN ? DeclOtherTarget : DeclNoRecord;
            for (uint32_t i = 0; i < recN; ++i) {
                if (recRt[i] && recRt[i] == t1Res) { d = Replay; *idx = static_cast<int>(i); break; }
            }
        }
        if (d == Replay) fReplayed = true;
        else fDecl[d] = true;
        return d;
    }
    // Once a frame: tally what this frame did and start the next one clean.
    void endFrame() {
        if (fAny) {
            ++frames;
            if (compSeen) ++framesWithComposite;
            if (fReplayed) ++framesReplayed;
            for (uint32_t i = 1; i < kDecisions; ++i) if (fDecl[i]) ++framesDeclined[i];
        }
        recN = 0;
        for (uint32_t i = 0; i < kRec; ++i) recRt[i] = nullptr;
        compSeen = false;
        fAny = fReplayed = false;
        for (bool& d : fDecl) d = false;
    }
    void resetTotals() {
        frames = framesWithComposite = framesReplayed = 0;
        for (uint32_t& x : framesDeclined) x = 0;
        records = recordDrops = recordFailures = replays = replayFailures = verifiedOk = verifiedBad = t1Unresolved = 0;
    }
};

inline const char* decisionText(RevealReplay::Decision d) {
    switch (d) {
        case RevealReplay::Replay:          return "replayed";
        case RevealReplay::DeclFlat:        return "declined: the flat profile never replays";
        case RevealReplay::DeclNoRecord:    return "declined: no downsample was recorded this frame before the composite";
        case RevealReplay::DeclOtherTarget: return "declined: the frame's recorded downsamples went to another texture than the composite's t1 (a hash-only match)";
        case RevealReplay::DeclT1:          return "declined: the composite's t1 did not resolve to a texture";
        default:                            return "?";
    }
}

// What a scanner session's tallies say, in one phrase. A run where reveal composites drew and the replay never fired must read differently from success and from "nothing to do".
inline const char* sessionVerdict(const RevealReplay& r, uint32_t drawsChecked) {
    if (!drawsChecked) return "NO DrawInstanced call was checked in this scanner session: the reveal fix's hook did not run";
    if (!r.framesWithComposite) return "no reveal composite drew in this scanner session, so the replay had nothing to do";
    if (r.framesReplayed) return r.verifiedBad ? "the replay fired, but the context state read back after a replay DIFFERED from the state before: check this run" : "the replay fired";
    return "THE REPLAY NEVER FIRED although reveal composites drew: this run did not test the fix";
}

}  // namespace fssreveal
}  // namespace edvr
