#pragma once
// A VIEW WITH NO MOTION SOURCE, AND THE WAY BACK (design doc section 104, the training mission's turned-left view).
//
// The copy route's selector needs one draw whose shader pair the motion producer can substitute (engine_velocity_families.h) to name the
// scene's depth, camera and motion slots. A view made of ground and sky has none, and every frame of it is refused with
// no-supported-motion-source-pair. The 2026-10-07 training-mission log showed the camera injector in "warming" for the whole spell
// (injected=0, clean-closes=0): not a second fault, but the phase machine's right answer to frames that are never accepted, since a phase
// is chosen only after two clean accepted zero-phase frames (FlatLivePhase). This tracks the spell itself and the way out of it, so a
// flight can read, instead of infer, that the machine recovers and how long it takes:
//
//   spell     a run of consecutive frames refused for no source; the frame that ends it either is treated again (a recovery) or is refused
//             for another reason (abandoned: the view became a menu or a loading screen);
//   warm-up   the frames from the first treated frame after a spell until the first frame that carries a non-zero jitter phase.
//
// Plain counters; tools\flat_temporal_test runs the sequences of the log.
#include <cstdint>
#include <cstdio>
#include "flat_mono_frame.h"

namespace edvr {

struct FlatSourceSpellWindow {
    uint64_t frames = 0, noSourceFrames = 0, spells = 0, recoveries = 0, abandoned = 0, warmDone = 0, warmFrames = 0, warmMax = 0, warmAborted = 0;
    uint64_t longestSpell = 0;
    // Source-free frames (FlatMonoFrame::sourceFree): the scene held no pool draw and was treated from the camera term alone. Counted at
    // the Present, whether the resolver ran on them or not; the overlay count is the weapon glow passes admitted while no world was named.
    uint64_t sourceFreeFrames = 0, sourceFreeTreated = 0, overlaysUnnamed = 0;
    bool inSpell = false;
    uint64_t openSpell = 0;
};

class FlatSourceSpell {
public:
    // One frame that had a verdict (a final copy was seen). noSource: refused for no supported source pair. selected: the selector
    // selected it. nonzeroPhase: the frame carried a non-zero jitter phase.
    void frame(bool noSource, bool selected, bool nonzeroPhase) {
        ++w_.frames;
        if (noSource) {
            ++w_.noSourceFrames;
            if (!inSpell_) { inSpell_ = true; run_ = 0; ++w_.spells; }
            ++run_;
            if (run_ > w_.longestSpell) w_.longestSpell = run_;
            if (warming_) { ++w_.warmAborted; warming_ = false; }
            return;
        }
        if (inSpell_) {
            inSpell_ = false; run_ = 0;
            if (selected) { ++w_.recoveries; warming_ = true; warm_ = 0; }
            else ++w_.abandoned;
        }
        if (warming_) {
            if (nonzeroPhase) { ++w_.warmDone; w_.warmFrames += warm_; if (warm_ > w_.warmMax) w_.warmMax = warm_; warming_ = false; }
            else if (selected) ++warm_;
            else { ++w_.warmAborted; warming_ = false; }
        }
    }
    // A frame the selector took with no motion source (the view is the camera term alone), and whether the resolver treated it.
    void sourceFreeFrame(bool treated) { ++w_.sourceFreeFrames; if (treated) ++w_.sourceFreeTreated; }
    // A weapon glow pass admitted as an overlay although no draw had named the world: the HDR target's own first camera was the world's.
    void overlayUnnamed() { ++w_.overlaysUnnamed; }
    // The window since the last take (the state, an open spell and a warm-up in progress, carries over).
    FlatSourceSpellWindow take() {
        FlatSourceSpellWindow out = w_;
        out.inSpell = inSpell_; out.openSpell = inSpell_ ? run_ : 0;
        w_ = FlatSourceSpellWindow{};
        // A spell that is still open continues into the next window as its own run; its length so far is the next window's floor.
        if (inSpell_) w_.longestSpell = run_;
        return out;
    }
    void reset() { *this = FlatSourceSpell{}; }
private:
    FlatSourceSpellWindow w_;
    bool inSpell_ = false, warming_ = false;
    uint64_t run_ = 0, warm_ = 0;
};

// The pair's words for the line: whether the motion producer substitutes its vertex shader (a pool family with another pixel shader), and
// whether the flat projection recipes know the pair. The runtime fills them; the selector's summary cannot (it is plain data).
struct FlatSourcelessPairNote { bool familyVs = false, recipe = false; };

inline int flatSourceSpellLine(char* out, size_t n, const FlatSourceSpellWindow& w, const FlatMonoSourceless& last, uint64_t lastFrame,
                               const FlatSourcelessPairNote (&notes)[4]) {
    char pairs[1200];
    size_t at = 0;
    pairs[0] = 0;
    for (uint32_t i = 0; i < last.topCount; ++i) {
        const auto& p = last.top[i];
        const int k = std::snprintf(pairs + at, sizeof(pairs) - at,
            "%s[VS=%016llX PS=%016llX draws=%u records=%u same-camera=%u pool-kind=%u family-vs=%u recipe=%u]", i ? " " : "",
            static_cast<unsigned long long>(p.vs), static_cast<unsigned long long>(p.ps), p.draws, p.records, p.sameCamera ? 1u : 0u,
            p.pool ? 1u : 0u, notes[i].familyVs ? 1u : 0u, notes[i].recipe ? 1u : 0u);
        if (k < 0 || at + static_cast<size_t>(k) >= sizeof(pairs)) break;
        at += static_cast<size_t>(k);
    }
    if (!at) std::snprintf(pairs, sizeof(pairs), "none");
    return std::snprintf(out, n,
        "flat source 5s: frames=%llu no-source-frames=%llu spells=%llu longest-spell=%llu open-spell=%llu recoveries=%llu abandoned=%llu "
        "warm-ups-done=%llu warm-frames-total=%llu warm-frames-max=%llu warm-ups-aborted=%llu source-free-frames=%llu source-free-treated=%llu "
        "overlays-without-named-world=%llu; last no-source frame=%llu: "
        "records-on-scene-depth=%u draws=%u same-camera-draws=%u pool-kind-records=%u distinct-pairs=%u top: %s; a spell is a run of frames "
        "refused for no supported motion source pair (the view holds a draw of a pool family the motion producer leaves stock); "
        "source-free frames held no pool-family draw at all (open ground and sky) and take the camera term alone, treated counts those the resolver ran on; "
        "recoveries end a spell with a treated frame and warm-frames count the zero-phase frames after it until the first non-zero phase "
        "(two, by FlatLivePhase), abandoned ends one with another refusal (a menu or a loading screen); family-vs is a pool-family vertex "
        "shader drawn with an unkeyed pixel shader, recipe a pair the projection recipes know",
        static_cast<unsigned long long>(w.frames), static_cast<unsigned long long>(w.noSourceFrames),
        static_cast<unsigned long long>(w.spells), static_cast<unsigned long long>(w.longestSpell),
        static_cast<unsigned long long>(w.openSpell), static_cast<unsigned long long>(w.recoveries),
        static_cast<unsigned long long>(w.abandoned), static_cast<unsigned long long>(w.warmDone),
        static_cast<unsigned long long>(w.warmFrames), static_cast<unsigned long long>(w.warmMax),
        static_cast<unsigned long long>(w.warmAborted), static_cast<unsigned long long>(w.sourceFreeFrames),
        static_cast<unsigned long long>(w.sourceFreeTreated), static_cast<unsigned long long>(w.overlaysUnnamed),
        static_cast<unsigned long long>(lastFrame),
        last.records, last.draws, last.sameCameraDraws, last.poolRecords, last.distinctPairs, pairs);
}

}  // namespace edvr
