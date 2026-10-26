// The spatial recovery at the final copy of a frame with no 3D scene (docs/flat-loading-hologram-ghost-2026-10-09.md).
//
// WHY. Below the output the game stretches its render-size image to the display with a bilinear tap, and a frame with no scene
// (a loading screen's hologram, startup, a 2D menu, a loading panel) is left to it: there is no scene to resolve temporally, and the
// hologram has no motion source, so a temporal resolve of it ghosts (the 2026-10-09 fix excludes it, and it stays excluded). The
// Galaxy Map, the System Map, the Orrery and the Full System Scanner already get a spatial recovery in the same position: SMAA 1x
// on the render-size image, then AMD's EASU up to the display (flatMonoResolveSpatialFallback with easu set). This admits the same
// recovery, and nothing else, at the output copy of a no-scene frame.
//
// WHAT IT IS. The copy's source is the only thing it reads and the copy's source is the only thing it replaces. No jitter was
// asked for the frame and none is resampled away, no history is read, written or kept (the resolver is told its history is invalid,
// as it is after a map frame and after every refused frame), and the first scene frame after it starts or keeps history exactly as it
// does after a refused one. The interface is drawn after the copy and is never touched.
//
// WHEN. Every condition, on the actual context at the copy draw:
//   - the temporal engine is on (fix.temporal_aa selects one and the F7 comparison has not turned it off); there is no key of its own
//   - the frame is no-scene: the copy route's verdict this frame (flatCopyAdmit's NoScene), or, on a frame the stand-down paused,
//     the verdict the standing stand-down found
//   - the draw is the game's known output copy (flat_mono_frame.h kCopyVs / kCopyPs) into the output, at the full output viewport
//   - its source is an R8G8B8A8 image (view format UNORM or UNORM_SRGB), the LDR the game's own copy reads
//   - the source is below the output on at least one axis, and its size is a uniform scale of the output between half and twice
//     (the band every other route in this runtime calls the render size)
//   - no draw of the frame was jittered (a first no-scene frame after a scene can be: the early spatial recovery that already
//     serves a refused jittered frame (FlatRuntimeDrawScope::recover) resamples the jitter away and keeps it)
// Anything else is declined, counted by reason, and the frame is what it was.
//
// THE STAND-DOWN. A no-scene frame is a structural refusal and stands the work down after 5 s, as it always has: nothing is
// watched while there is nothing to treat. A stood-down frame is not watched, so the draw scope's one extra question per draw there
// is flatNoScenePausedCopyDraw (a flag in the runtime state, then three integers and two shader hashes already in the binding shadow);
// only the output copy of a frame that passes it asks the context anything. The flag is raised by a watched no-scene frame the
// recovery treated, and lowered by any watched frame that was not, so it is never up for a stood-down reason other than no-scene.
//
// Pure, like the rest of the copy stage: tools\flat_temporal_test drives it, and the runtime calls the same functions.
#pragma once
#include "flat_copy_structure.h"
#include <cstdio>

namespace edvr {

enum class FlatNoSceneSpatialWhy : uint8_t {
    Treat,             // every condition holds
    TemporalOff,       // no temporal engine is running (unreachable in the runtime, whose draw scope does not run then)
    NotNoScene,        // the frame is not no-scene
    UnknownCopyPair,   // the draw is not the game's known output copy
    NotToOutput,       // ...or it does not write the whole output
    SourceNotLdr,      // the copy's source view is not R8G8B8A8
    NotBelowOutput,    // the source is at least the output on both axes: nothing to recover
    SourceUnfit,       // the source is below the output, but not a uniform scale of it between half and twice
    Jittered,          // a draw of the frame was jittered
    Count
};
inline const char* flatNoSceneSpatialWhyName(FlatNoSceneSpatialWhy why) {
    switch (why) {
    case FlatNoSceneSpatialWhy::Treat: return "treated";
    case FlatNoSceneSpatialWhy::TemporalOff: return "temporal-off";
    case FlatNoSceneSpatialWhy::NotNoScene: return "not-no-scene";
    case FlatNoSceneSpatialWhy::UnknownCopyPair: return "unknown-copy-pair";
    case FlatNoSceneSpatialWhy::NotToOutput: return "not-to-output";
    case FlatNoSceneSpatialWhy::SourceNotLdr: return "source-not-ldr";
    case FlatNoSceneSpatialWhy::NotBelowOutput: return "not-below-output";
    case FlatNoSceneSpatialWhy::SourceUnfit: return "source-unfit";
    case FlatNoSceneSpatialWhy::Jittered: return "jittered";
    default: return "unknown";
    }
}

// What the context shows at the copy draw. Everything is a value.
struct FlatNoSceneSpatialFacts {
    bool temporalOn = false;
    bool noScene = false;
    uint64_t copyVs = 0, copyPs = 0;       // the bound pair, by content hash
    bool toOutput = false;                 // colour is the output, no depth bound, one viewport covering all of it
    uint32_t outputWidth = 0, outputHeight = 0;
    uint32_t srcViewFormat = 0;            // DXGI_FORMAT of the shader resource view at slot 0
    uint32_t srcWidth = 0, srcHeight = 0;  // its texture
    bool jittered = false;
};
// DXGI_FORMAT_R8G8B8A8_UNORM and _UNORM_SRGB: the views the resolver's colour input accepts (flat_mono_resolve.cpp inputTexture).
inline bool flatNoSceneLdrView(uint32_t viewFormat) { return viewFormat == 28 || viewFormat == 29; }

inline FlatNoSceneSpatialWhy flatNoSceneSpatialAdmit(const FlatNoSceneSpatialFacts& f) {
    using W = FlatNoSceneSpatialWhy;
    if (!f.temporalOn) return W::TemporalOff;
    if (!f.noScene) return W::NotNoScene;
    if (f.copyVs != flat_mono_detail::kCopyVs || f.copyPs != flat_mono_detail::kCopyPs) return W::UnknownCopyPair;
    if (!f.toOutput || !f.outputWidth || !f.outputHeight) return W::NotToOutput;
    if (!flatNoSceneLdrView(f.srcViewFormat)) return W::SourceNotLdr;
    if (!f.srcWidth || !f.srcHeight || (f.srcWidth >= f.outputWidth && f.srcHeight >= f.outputHeight)) return W::NotBelowOutput;
    if (!flatRenderFitsOutput(f.srcWidth, f.srcHeight, f.outputWidth, f.outputHeight)) return W::SourceUnfit;
    if (f.jittered) return W::Jittered;
    return W::Treat;
}

// The draw scope's question on a frame the stand-down paused: whether this draw could be the output copy, from what the scope was
// called with and the binding shadow's two shader hashes. A fullscreen triangle (three vertices, one instance) with the copy's pair.
inline bool flatNoScenePausedCopyDraw(char kind, uint32_t count, uint32_t instances, uint64_t boundVs, uint64_t boundPs) {
    return (kind == 'N' || kind == 'D') && count == 3 && instances == 1 &&
           boundVs == flat_mono_detail::kCopyVs && boundPs == flat_mono_detail::kCopyPs;
}

// The 5 s window of the census line, zeros included while a temporal mode runs: an absent line is what "the recovery never ran" looks
// like (and what a temporal mode that is off looks like), and `no-scene=N treated=0 declined=N` names why it did not.
struct FlatNoSceneSpatialWindow {
    uint64_t noScene = 0;         // final copies of watched frames the copy route ruled no-scene
    uint64_t standingDown = 0;    // final copies of stood-down frames that passed the cheap question
    uint64_t treated = 0, easu = 0, smaa = 0;
    uint64_t declined = 0;
    uint64_t why[static_cast<size_t>(FlatNoSceneSpatialWhy::Count)]{};
    uint64_t refused = 0;         // the recovery ran and refused (the frame stays the game's)
    uint64_t firstWriteCopies = 0;   // verified image copies that were the first write into their HDR (flat_runtime_model.h)
    const char* last = "none";
    const char* lastRefusal = "none";
    uint32_t srcWidth = 0, srcHeight = 0, outputWidth = 0, outputHeight = 0;
    void noteFrame(bool stoodDown) { if (stoodDown) ++standingDown; else ++noScene; }
    void noteSize(uint32_t sw, uint32_t sh, uint32_t ow, uint32_t oh) { srcWidth = sw; srcHeight = sh; outputWidth = ow; outputHeight = oh; }
    void noteDecline(FlatNoSceneSpatialWhy w) {
        ++declined; ++why[static_cast<size_t>(w)]; last = flatNoSceneSpatialWhyName(w);
    }
    void noteTreated(bool usedEasu, bool usedSmaa) {
        ++treated; if (usedEasu) ++easu; if (usedSmaa) ++smaa; last = flatNoSceneSpatialWhyName(FlatNoSceneSpatialWhy::Treat);
    }
    void noteRefused(const char* reason) {
        ++refused; lastRefusal = reason && *reason ? reason : "unknown"; last = "spatial-refused";
    }
    void reset() { *this = FlatNoSceneSpatialWindow{}; }
};
inline int flatNoSceneSpatialFormatWindow(char* out, size_t size, bool pausedReady, const FlatNoSceneSpatialWindow& w) {
    using W = FlatNoSceneSpatialWhy;
    const auto n = [&](W why) { return static_cast<unsigned long long>(w.why[static_cast<size_t>(why)]); };
    return std::snprintf(out, size,
        "flat no-scene spatial 5s: no-scene=%llu stood-down=%llu treated=%llu easu=%llu smaa=%llu declined=%llu "
        "(not-below-output=%llu unknown-copy-pair=%llu not-to-output=%llu source-not-ldr=%llu source-unfit=%llu jittered=%llu "
        "not-no-scene=%llu) refused=%llu (%s) image-copy-first-writes=%llu paused-ready=%s last=%s source=%ux%u output=%ux%u; "
        "SMAA 1x then EASU at the output copy, no jitter, no history",
        static_cast<unsigned long long>(w.noScene), static_cast<unsigned long long>(w.standingDown),
        static_cast<unsigned long long>(w.treated), static_cast<unsigned long long>(w.easu),
        static_cast<unsigned long long>(w.smaa), static_cast<unsigned long long>(w.declined),
        n(W::NotBelowOutput), n(W::UnknownCopyPair), n(W::NotToOutput), n(W::SourceNotLdr), n(W::SourceUnfit), n(W::Jittered),
        n(W::NotNoScene), static_cast<unsigned long long>(w.refused), w.lastRefusal,
        static_cast<unsigned long long>(w.firstWriteCopies), pausedReady ? "yes" : "no", w.last,
        w.srcWidth, w.srcHeight, w.outputWidth, w.outputHeight);
}

}  // namespace edvr
