// The spatial recovery at the output copy of a frame with no 3D scene on the rig (flat_noscene_spatial.h; the model's first-write
// image copy in flat_runtime_model.h; flatCopyAdmit's no-scene verdicts in flat_copy_structure.h).
//
// THE FRAME. The loading screen's hologram at game supersampling 0.5 (960x600 on a 1920x1200 output, edvr_gfx_20261021_165632, census
// 16:57:57): the scene HDR (R11G11B10F) is cleared; a camera-independent R16G16B16A16 image pass draws into the hologram's depth; the
// glow layer's additive copy (VS CFA91824 / PS DFCBA0EC, the HDR image copy) is the FIRST write into the HDR, with no depth bound; the
// hologram quad (VS 525D47E3 / PS F0BAE053, an inert pair, packed UV to clip with no camera) draws into the HDR and the depth; the
// bloom composite tone (VS CFA91824 / PS 9270C355) makes the R8G8B8A8 image; the game's output copy stretches it to the display.
// Before this the image copy was refused (image-copy-source) because no earlier write had given the HDR a camera, every frame of the
// screen was conflicting-hdr-target-or-camera, and the stand-down never began.
//
// WHAT IS PINNED. 1 the model: the first-write copy conflicts with nothing and is not recorded; every other image-copy case keeps its
// refusal, and so does a first-write copy once a pair that reads b1 writes the same HDR. 2 the verdicts the copy route gives the
// frame (no-3d-scene, whichever way the whitelist read it) and the frames it leaves alone. 3 the admission predicate, condition by
// condition. 4 the census line. 5 the runtime wiring no rig can run, as source pins: where the recovery runs, that it touches no
// history and no jitter, that the maps' recovery is the same call it was, and that the paused frame's question is the one cheap gate.
#pragma once
#include "../../src/d3d11/flat_noscene_spatial.h"
#include "../../src/d3d11/flat_history_report.h"
#include "flat_hdr_route_tests.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace noscene_spatial_test {

using hdr_route_test::Scene;
using hdr_route_test::Stream;
using hdr_route_test::tok;

constexpr uint64_t kGlowVs = 0xCFA91824129ECBBCull, kGlowPs = 0xDFCBA0EC70B03C9Bull;     // the HDR image copy
constexpr uint64_t kImageVs = 0xCFA91824129ECBBCull, kImagePs = 0x07B3F82100F29401ull;    // the camera-independent image pass
constexpr uint64_t kQuadVs = 0x525D47E3D5E2EFF4ull, kQuadPs = 0xF0BAE053476F8730ull;      // the hologram quad: inert
constexpr uint64_t kToneVs = 0xCFA91824129ECBBCull, kTonePs = 0x9270C355389DA302ull;      // the bloom composite tone (HDR at slot 0)

// One frame of the screen, each option a way it can differ.
struct Loading {
    uint32_t outW = 1920, outH = 1200, rW = 960, rH = 600;
    bool glowCopy = true;          // the HDR image copy is drawn at all
    bool glowVerified = true;      // ...and the bridge verified it (hdrCopyVerified)
    bool glowDepth = false;        // ...with a depth buffer bound
    bool sourceDrawn = true;       // the image the copy reads was drawn this frame
    bool quad = true;              // the hologram quad is drawn into the HDR
    bool quadCamera = true;        // ...with a camera current in its b1
    bool sceneDraws = false;       // `sceneN` ordinary scene draws (a real pair, with the camera) after the quad
    uint32_t sceneN = 0;
    bool priorDraw = false;        // an ordinary camera-less HDR draw BEFORE the copy
    bool priorCamera = false;      // an ordinary camera-bearing HDR draw BEFORE the copy
};
struct Built {
    std::unique_ptr<Stream> stream;
    edvr::FlatRuntimeDraw copy{};
    edvr::FlatMonoFrame whitelist{};
};
inline Built build(const Loading& o) {
    using namespace edvr;
    Scene sc;
    sc.outW = o.outW; sc.outH = o.outH; sc.hW = o.rW; sc.hH = o.rH;
    Built out;
    out.stream = std::make_unique<Stream>(sc);
    auto& s = *out.stream;
    const void* image = tok(0x7000);
    s.write(s.sc.h);   // the clear that opens the HDR
    if (o.priorDraw || o.priorCamera) s.draw(s.make(s.sc.h, s.sc.hDepth, o.rW, o.rH, 26, 0xA2, 0xB2, o.priorCamera, false));
    if (o.sourceDrawn) {
        FlatRuntimeDraw src = s.make(image, s.sc.hDepth, o.rW, o.rH, 9, kImageVs, kImagePs, false, false);
        src.imageSourceCameraIndependentVerified = true;
        s.draw(src);
    }
    if (o.glowCopy) {
        FlatRuntimeDraw glow = s.make(s.sc.h, o.glowDepth ? s.sc.hDepth : nullptr, o.rW, o.rH, 26, kGlowVs, kGlowPs, false, false);
        glow.hdrCopyVerified = o.glowVerified;
        glow.key.srvView[0] = tok(0x7002); glow.key.srvResource[0] = image;
        s.draw(glow);
    }
    if (o.quad) s.draw(s.make(s.sc.h, s.sc.hDepth, o.rW, o.rH, 26, kQuadVs, kQuadPs, o.quadCamera, false));
    for (uint32_t i = 0; o.sceneDraws && i < o.sceneN; ++i)
        s.draw(s.make(s.sc.h, s.sc.hDepth, o.rW, o.rH, 26, 0xA2 + i % 3, 0xB2, true, false));
    FlatRuntimeDraw tone = s.make(s.sc.tone, nullptr, o.rW, o.rH, 27, kToneVs, kTonePs, false, false);
    tone.key.srvView[0] = tok(0x1002); tone.key.srvResource[0] = s.sc.h;
    s.draw(tone);
    FlatRuntimeDraw copy = s.make(s.sc.output, nullptr, o.outW, o.outH, 28, flat_mono_detail::kCopyVs, flat_mono_detail::kCopyPs, false, false);
    copy.key.srvView[0] = tok(0x2202); copy.key.srvResource[0] = s.sc.tone;
    out.copy = copy;
    out.whitelist = flatRuntimeObserve(*s.prefix, out.copy);
    return out;
}
inline const edvr::FlatRuntimeTarget* hdrTarget(const Built& b) {
    for (uint32_t i = 0; i < b.stream->prefix->targetsUsed; ++i)
        if (b.stream->prefix->targets[i].resource == b.stream->sc.h) return &b.stream->prefix->targets[i];
    return nullptr;
}
inline edvr::FlatCopyPolicy policy(bool structure, edvr::FlatMonoResolveMode mode, bool latched = false) {
    edvr::FlatCopyPolicy p; p.structure = structure; p.mode = mode; p.routeLatched = latched;
    return p;
}

}  // namespace noscene_spatial_test

inline int flatNoSceneSpatialTests() {
    using namespace edvr;
    using namespace noscene_spatial_test;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: no-scene spatial %s\n", name); ++failures; }
    };
    const auto verdictOf = [](const Built& b, const FlatCopyPolicy& p, FlatCopyDiag* diag) {
        return flatCopyAdmit(*b.stream->prefix, b.stream->hdr, b.copy, b.whitelist, p, diag);
    };
    const FlatCopyPolicy autoXess = policy(true, FlatMonoResolveMode::Xess);

    // ---- 1. the model: the first-write image copy -----------------------------------------------------------------------
    {
        Loading o; o.quad = false;
        Built b = build(o);
        const auto* h = hdrTarget(b);
        expect(h && !h->hdrBad && h->firstBad.cause == FlatRuntimeConflict::None && h->imageCopyFirst && h->writes.draws == 0 &&
               b.stream->prefix->imageCopiesFirstWrite == 1 && b.stream->prefix->imageCopiesRefused == 0 &&
               b.stream->prefix->imageCopiesAccepted == 0,
               "the glow copy as the first write into the HDR conflicts with nothing, is not recorded as a write, and is counted apart");
    }
    {
        // The frame as the census has it, the quad carrying the camera its b1 held. The whitelist's own verdict is a selection with no
        // motion source (a source-free scene), which the copy route calls a blank scene: no-3d-scene.
        Built b = build(Loading{});
        const auto* h = hdrTarget(b);
        expect(h && !h->hdrBad && h->hdrCamera && h->writes.draws == 1 && h->imageCopyFirst,
               "the hologram quad then writes the HDR as its first recorded write and gives it its camera, still no conflict");
        expect(b.whitelist.selected() && b.whitelist.sourceFree && flatFrameBlankScene(b.whitelist),
               "the whitelist selects the frame as a source-free scene whose depth holds one inert filter: a blank scene");
        FlatCopyDiag diag;
        const FlatMonoFrame out = verdictOf(b, autoXess, &diag);
        expect(!out.selected() && out.reason == FlatMonoReason::NoScene && diag.outcome == FlatCopyOutcome::NoScene &&
               flatMonoReasonStructural(out.reason) && !flatMonoReasonWarrantsWarning(out.reason) &&
               flatFrameSeenFor(out.selected(), out.reason) == FlatFrameSeen::Structural,
               "the copy route rules it no-3d-scene (structural, silent) at 960x600 on 1920x1200: not a selection that would resolve");
    }
    {
        // The same frame when the quad's b1 held no camera: nothing gives the HDR a camera, and the whitelist says so.
        Loading o; o.quadCamera = false;
        Built b = build(o);
        const auto* h = hdrTarget(b);
        expect(h && !h->hdrBad && !h->hdrCamera && b.whitelist.reason == FlatMonoReason::NoHdrCamera,
               "with no camera current at the quad the whitelist reads hdr-camera-not-observed");
        FlatCopyDiag diag;
        const FlatMonoFrame out = verdictOf(b, autoXess, &diag);
        expect(out.reason == FlatMonoReason::NoScene && diag.outcome == FlatCopyOutcome::NoScene && diag.whitelist == FlatMonoReason::NoHdrCamera,
               "...and the copy route rules that no-3d-scene too: no HDR camera and no scene in the frame");
    }
    {
        // Without the quad nothing writes the HDR but the glow copy: the whitelist finds no HDR writes at all.
        Loading o; o.quad = false;
        Built b = build(o);
        FlatCopyDiag diag;
        const FlatMonoFrame out = verdictOf(b, autoXess, &diag);
        expect(b.whitelist.reason == FlatMonoReason::NoHdr && out.reason == FlatMonoReason::NoScene && diag.outcome == FlatCopyOutcome::NoScene,
               "a glow copy alone: no-observed-hdr-writes from the whitelist, no-3d-scene from the copy route");
    }
    // The unchanged cases. Each is the frame above with one thing made wrong, and each is refused as it was.
    {
        Loading o; o.priorDraw = true;   // an HDR write with no camera BEFORE the copy: the copy is not the first write
        Built b = build(o);
        const auto* h = hdrTarget(b);
        expect(h && h->hdrBad && h->firstBad.cause == FlatRuntimeConflict::ImageCopySource && b.stream->prefix->imageCopiesRefused == 1 &&
               b.stream->prefix->imageCopiesFirstWrite == 0 && !h->imageCopyFirst,
               "an image copy onto an HDR that already has a camera-less write is still image-copy-source");
        FlatCopyDiag diag;
        const FlatMonoFrame out = verdictOf(b, autoXess, &diag);
        expect(b.whitelist.reason == FlatMonoReason::ConflictingHdr && out.reason == FlatMonoReason::ConflictingHdr,
               "...and the frame is conflicting-hdr-target-or-camera, the admission leaving it alone");
    }
    {
        Loading o; o.priorCamera = true;   // an HDR write WITH a camera before the copy: the continuing case
        Built b = build(o);
        const auto* h = hdrTarget(b);
        expect(h && !h->hdrBad && h->hdrCamera && b.stream->prefix->imageCopiesAccepted == 1 && b.stream->prefix->imageCopiesFirstWrite == 0 &&
               !h->imageCopyFirst,
               "an image copy onto an HDR that already has camera writes is accepted as a continuation, exactly as before");
    }
    {
        Loading o; o.glowVerified = false;
        Built b = build(o);
        const auto* h = hdrTarget(b);
        expect(h && h->hdrBad && h->firstBad.cause == FlatRuntimeConflict::ImageCopySource && b.stream->prefix->imageCopiesFirstWrite == 0,
               "a first-write copy the bridge did not verify is refused as before");
        Loading d; d.glowDepth = true;
        Built bd = build(d);
        const auto* hd = hdrTarget(bd);
        expect(hd && hd->hdrBad && hd->firstBad.cause == FlatRuntimeConflict::ImageCopySource && bd.stream->prefix->imageCopiesFirstWrite == 0,
               "...and so is one with a depth buffer bound");
    }
    {
        Loading o; o.sceneDraws = true; o.sceneN = 10;   // a real scene after the glow copy: pairs that read b1, with the camera
        Built b = build(o);
        const auto* h = hdrTarget(b);
        expect(h && h->hdrBad && h->firstBad.cause == FlatRuntimeConflict::ImageCopySource && b.stream->prefix->imageCopiesFirstWrite == 1,
               "a first-write copy followed by a pair that reads b1 is the conflict it was: the scene frames keep their refusal");
        expect(b.whitelist.reason == FlatMonoReason::ConflictingHdr && verdictOf(b, autoXess, nullptr).reason == FlatMonoReason::ConflictingHdr,
               "...and the frame stays conflicting-hdr-target-or-camera (a scene, not a no-scene frame)");
    }
    {
        // The inert quad before and after a real pair: the real pair still raises it, wherever the quad stood.
        Loading o; o.sceneDraws = true; o.sceneN = 1;
        Built b = build(o);
        const auto* h = hdrTarget(b);
        expect(h && h->hdrBad && h->firstBad.cause == FlatRuntimeConflict::ImageCopySource,
               "one real pair after the inert quad is enough to raise it");
    }

    // ---- 2. the copy route's verdicts around it -------------------------------------------------------------------------
    {
        // The same blank selection at R = D, where the HDR route evaluates at the render size and declines the frame ("blank-scene"):
        // the frame it left to the copy route is no scene there too, for every policy, so nothing resolves it temporally.
        Loading o; o.outW = 960; o.outH = 600;
        Built b = build(o);
        FlatCopyDiag diag;
        const FlatMonoFrame out = verdictOf(b, policy(true, FlatMonoResolveMode::Dlss), &diag);
        expect(b.whitelist.selected() && flatHdrRouteEvaluatesAtRender(FlatMonoResolveMode::Dlss, 960, 600, 960, 600) &&
                   out.reason == FlatMonoReason::NoScene && diag.outcome == FlatCopyOutcome::NoScene,
               "at R = D, where the HDR route declines a blank scene, the copy route rules it no-3d-scene instead of resolving it");
        const FlatMonoFrame keyOff = verdictOf(b, policy(false, FlatMonoResolveMode::Dlss), &diag);
        expect(keyOff.reason == FlatMonoReason::NoScene && diag.outcome == FlatCopyOutcome::NoScene,
               "...with the key off, where the copy route is the only route");
        const FlatMonoFrame latched = verdictOf(b, policy(true, FlatMonoResolveMode::Dlss, true), &diag);
        expect(latched.reason == FlatMonoReason::NoScene, "...and with the route latched off");
        const FlatMonoFrame taa = verdictOf(b, policy(true, FlatMonoResolveMode::Taa), &diag);
        expect(taa.reason == FlatMonoReason::NoScene, "...and for EDVR's TAA");
    }
    {
        // A selected scene with a world is never touched: the maps' frames and every frame the whitelist selects with a source.
        // A scene's worth of ordinary draws with the same camera, enough for the scene facts: ground and sky, no motion source.
        Loading o; o.quad = false; o.glowCopy = false; o.sourceDrawn = false; o.sceneDraws = true; o.sceneN = 9;
        Built b = build(o);
        FlatCopyDiag diag;
        const FlatMonoFrame out = verdictOf(b, autoXess, &diag);
        expect(b.whitelist.reason == FlatMonoReason::Selected && !flatFrameBlankScene(b.whitelist) && out.selected() &&
               diag.outcome == FlatCopyOutcome::Untouched && out.reason == b.whitelist.reason,
               "a selected frame whose depth holds world draws is not blank, and the copy stage returns it untouched");
    }
    {
        // The refusals the admission has nothing to say about stay what they were, scene or not.
        Loading o; o.quadCamera = false;
        Built b = build(o);
        for (FlatMonoReason r : {FlatMonoReason::ConflictingHdr, FlatMonoReason::NoSupportedSource,
                                 FlatMonoReason::AmbiguousSource, FlatMonoReason::InvalidSource, FlatMonoReason::WrongOrder,
                                 FlatMonoReason::Truncated, FlatMonoReason::NoOutputCopy}) {
            FlatMonoFrame w = b.whitelist; w.reason = r;
            FlatCopyDiag diag;
            const FlatMonoFrame out = flatCopyAdmit(*b.stream->prefix, b.stream->hdr, b.copy, w, autoXess, &diag);
            expect(out.reason == r && diag.outcome == FlatCopyOutcome::Untouched, "a refusal for any other reason comes back untouched");
        }
        // ...and the no-usable-camera readings (none observed, rows that are no camera) are touched only where the frame has no scene.
        {
            FlatMonoFrame w = b.whitelist; w.reason = FlatMonoReason::InvalidCamera;
            FlatCopyDiag diag;
            const FlatMonoFrame out = flatCopyAdmit(*b.stream->prefix, b.stream->hdr, b.copy, w, autoXess, &diag);
            expect(out.reason == FlatMonoReason::NoScene && diag.outcome == FlatCopyOutcome::NoScene && diag.whitelist == FlatMonoReason::InvalidCamera,
                   "invalid-camera-rows with no scene in the frame is no-3d-scene too");
        }
        Loading s; s.quad = false; s.glowCopy = false; s.sourceDrawn = false; s.sceneDraws = true; s.sceneN = 9;
        Built sceneful = build(s);
        for (FlatMonoReason r : {FlatMonoReason::NoHdrCamera, FlatMonoReason::InvalidCamera, FlatMonoReason::NoHdr}) {
            FlatMonoFrame w = sceneful.whitelist; w.reason = r;
            FlatCopyDiag diag;
            const FlatMonoFrame out = flatCopyAdmit(*sceneful.stream->prefix, sceneful.stream->hdr, sceneful.copy, w, autoXess, &diag);
            expect(flatSceneFacts(*sceneful.stream->prefix, 1920, 1200).onShape && out.reason == r && diag.outcome == FlatCopyOutcome::Untouched,
                   "no usable HDR camera in a frame whose HDR has the draws of a scene stays what it was");
        }
    }

    // ---- 3. the admission predicate -------------------------------------------------------------------------------------
    {
        using W = FlatNoSceneSpatialWhy;
        FlatNoSceneSpatialFacts ok;
        ok.temporalOn = true; ok.noScene = true; ok.copyVs = flat_mono_detail::kCopyVs; ok.copyPs = flat_mono_detail::kCopyPs;
        ok.toOutput = true; ok.outputWidth = 1920; ok.outputHeight = 1200; ok.srcViewFormat = 28; ok.srcWidth = 960; ok.srcHeight = 600;
        expect(flatNoSceneSpatialAdmit(ok) == W::Treat, "the loading screen's copy (960x600 R8G8B8A8 into 1920x1200) is treated");
        auto with = [&](auto&& edit) { FlatNoSceneSpatialFacts f = ok; edit(f); return flatNoSceneSpatialAdmit(f); };
        expect(with([](auto& f) { f.srcViewFormat = 29; }) == W::Treat, "...and an sRGB view of it");
        expect(with([](auto& f) { f.temporalOn = false; }) == W::TemporalOff, "the temporal engine off: declined (F7 or fix.temporal_aa)");
        expect(with([](auto& f) { f.noScene = false; }) == W::NotNoScene, "a frame that is not no-scene: declined");
        expect(with([](auto& f) { f.copyVs = 0x1234; }) == W::UnknownCopyPair && with([](auto& f) { f.copyPs = 0x1234; }) == W::UnknownCopyPair &&
               with([](auto& f) { f.copyVs = f.copyPs = 0; }) == W::UnknownCopyPair,
               "an unknown copy pair, in either shader: declined");
        expect(with([](auto& f) { f.toOutput = false; }) == W::NotToOutput && with([](auto& f) { f.outputWidth = 0; }) == W::NotToOutput,
               "a copy that does not write the whole output: declined");
        expect(with([](auto& f) { f.srcViewFormat = 26; }) == W::SourceNotLdr && with([](auto& f) { f.srcViewFormat = 10; }) == W::SourceNotLdr &&
               with([](auto& f) { f.srcViewFormat = 0; }) == W::SourceNotLdr && with([](auto& f) { f.srcViewFormat = 87; }) == W::SourceNotLdr,
               "a source that is not an R8G8B8A8 view (R11G11B10F, R16G16B16A16F, none, B8G8R8A8): declined");
        expect(with([](auto& f) { f.srcWidth = 1920; f.srcHeight = 1200; }) == W::NotBelowOutput &&
               with([](auto& f) { f.srcWidth = 3840; f.srcHeight = 2400; }) == W::NotBelowOutput &&
               with([](auto& f) { f.srcWidth = 0; }) == W::NotBelowOutput,
               "a source at or above the output on both axes, or of no size: declined as not below the output");
        expect(with([](auto& f) { f.srcWidth = 1919; f.srcHeight = 1199; }) == W::Treat && with([](auto& f) { f.srcWidth = 2560; f.srcHeight = 1100; }) == W::SourceUnfit,
               "below the output on one axis counts when the size is a uniform scale of it; one axis below and one above is not");
        expect(with([](auto& f) { f.srcWidth = 960; f.srcHeight = 600; }) == W::Treat && with([](auto& f) { f.srcWidth = 950; f.srcHeight = 594; }) == W::SourceUnfit &&
               with([](auto& f) { f.srcWidth = 480; f.srcHeight = 300; }) == W::SourceUnfit && with([](auto& f) { f.srcWidth = 1024; f.srcHeight = 512; }) == W::SourceUnfit,
               "half of the output is the floor of the band; a hair under it, a quarter, or a shape that is not the output's, is declined as unfit");
        expect(with([](auto& f) { f.jittered = true; }) == W::Jittered, "a frame some draw of which was jittered: declined (the early recovery owns it)");
        // The order a frame is judged in: the first false condition names the decline.
        FlatNoSceneSpatialFacts all;   // every condition false at once
        expect(flatNoSceneSpatialAdmit(all) == W::TemporalOff, "with nothing true the first decline is the engine being off");
        all.temporalOn = true;
        expect(flatNoSceneSpatialAdmit(all) == W::NotNoScene, "...then the frame not being no-scene");
        all.noScene = true;
        expect(flatNoSceneSpatialAdmit(all) == W::UnknownCopyPair, "...then the copy pair");
        // Every decline has a name, all different, and the line counts each of them.
        bool named = true;
        for (unsigned i = 0; i < static_cast<unsigned>(W::Count); ++i)
            for (unsigned j = i + 1; j < static_cast<unsigned>(W::Count); ++j)
                named = named && std::strcmp(flatNoSceneSpatialWhyName(static_cast<W>(i)), flatNoSceneSpatialWhyName(static_cast<W>(j))) != 0;
        expect(named, "every verdict has its own name");

        // The paused frame's one question.
        constexpr uint64_t vs = flat_mono_detail::kCopyVs, ps = flat_mono_detail::kCopyPs;
        expect(flatNoScenePausedCopyDraw('N', 3, 1, vs, ps) && flatNoScenePausedCopyDraw('D', 3, 1, vs, ps) &&
               !flatNoScenePausedCopyDraw('N', 6, 1, vs, ps) && !flatNoScenePausedCopyDraw('N', 3, 2, vs, ps) &&
               !flatNoScenePausedCopyDraw('X', 3, 1, vs, ps) && !flatNoScenePausedCopyDraw('N', 3, 1, vs + 1, ps) &&
               !flatNoScenePausedCopyDraw('N', 3, 1, vs, ps + 1) && !flatNoScenePausedCopyDraw('N', 0, 0, 0, 0),
               "a paused draw is asked about only as a fullscreen triangle with the copy's two shader hashes");
    }

    // ---- 4. the census line ---------------------------------------------------------------------------------------------
    {
        FlatNoSceneSpatialWindow w;
        char line[1200];
        flatNoSceneSpatialFormatWindow(line, sizeof(line), false, w);
        const std::string zero(line);
        expect(zero == "flat no-scene spatial 5s: no-scene=0 stood-down=0 treated=0 easu=0 smaa=0 declined=0 (not-below-output=0 unknown-copy-pair=0 "
                       "not-to-output=0 source-not-ldr=0 source-unfit=0 jittered=0 not-no-scene=0) refused=0 (none) image-copy-first-writes=0 "
                       "paused-ready=no last=none source=0x0 output=0x0; SMAA 1x then EASU at the output copy, no jitter, no history",
               "a window with nothing in it prints every counter at zero (the absent line is what never-ran looks like)");
        w.noteFrame(false); w.noteFrame(false); w.noteFrame(true);
        w.noteSize(960, 600, 1920, 1200);
        w.noteTreated(true, true); w.noteTreated(true, true);
        w.noteDecline(FlatNoSceneSpatialWhy::NotBelowOutput);
        w.noteRefused("flat-spatial-input-view-mismatch");
        w.firstWriteCopies = 3;
        flatNoSceneSpatialFormatWindow(line, sizeof(line), true, w);
        const std::string text(line);
        expect(text.find("no-scene=2 stood-down=1 treated=2 easu=2 smaa=2 declined=1 (not-below-output=1 ") != std::string::npos &&
               text.find("refused=1 (flat-spatial-input-view-mismatch) image-copy-first-writes=3 paused-ready=yes last=spatial-refused "
                         "source=960x600 output=1920x1200;") != std::string::npos,
               "the line counts the frames, the treatments, each decline by reason, the refusal and the first-write copies");
        w.reset();
        flatNoSceneSpatialFormatWindow(line, sizeof(line), false, w);
        expect(zero == line, "the window resets to zeros");
        // The worst digits, under the log line budget (src/common/log.cpp keeps 1167 characters of a line).
        FlatNoSceneSpatialWindow big;
        for (auto& c : big.why) c = ~0ull;
        big.noScene = big.standingDown = big.treated = big.easu = big.smaa = big.declined = big.refused = big.firstWriteCopies = ~0ull;
        big.noteSize(~0u, ~0u, ~0u, ~0u);
        big.lastRefusal = "flat-spatial-hdr-requires-render-at-least-output"; big.last = "spatial-refused";
        char wide[1400];
        const int n = flatNoSceneSpatialFormatWindow(wide, sizeof(wide), true, big);
        expect(n > 0 && static_cast<size_t>(n) < kFlatLogLineBudget, "at twenty-digit counters the line is under the log line budget");
        char tiny[40];
        flatNoSceneSpatialFormatWindow(tiny, sizeof(tiny), true, big);
        expect(std::strlen(tiny) == sizeof(tiny) - 1, "a buffer too small truncates the line rather than overrunning");
    }

    // ---- 5. the wiring no rig can run -----------------------------------------------------------------------------------
    {
        const auto slurp = [](const char* path) {
            std::ifstream in(path, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        const auto count = [](const std::string& text, const std::string& needle) {
            size_t n = 0, at = 0;
            while ((at = text.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
            return n;
        };
        const auto body = [](const std::string& text, const char* signature) {
            const size_t at = text.find(signature);
            if (at == std::string::npos) return std::string();
            const size_t end = text.find("\n}\n", at);
            return text.substr(at, end == std::string::npos ? std::string::npos : end + 3 - at);
        };
        const std::string runtime = slurp("src/d3d11/flat_runtime.cpp");
        const std::string header = slurp("src/d3d11/flat_runtime.h");
        const std::string model = slurp("src/d3d11/flat_runtime_model.h");
        expect(!runtime.empty() && !header.empty() && !model.empty(), "the runtime and model sources are readable from the repo root");
        const std::string ctor = body(runtime, "FlatRuntimeDrawScope::FlatRuntimeDrawScope(");
        const std::string treat = body(runtime, "bool FlatRuntimeDrawScope::treatNoScene(bool stoodDown) {");
        expect(!ctor.empty() && !treat.empty(), "the draw scope's constructor and the recovery are found");
        const auto at = [](const std::string& text, const char* needle) { return text.find(needle); };

        // The recovery itself: the maps' call, with the maps' two flags, and nothing of the temporal path.
        expect(count(treat, "flatMonoResolveSpatialFallback(s.device.Get(),ctx,f,&outputView,&reason)") == 1 && count(treat, "f.easu=true;") == 1 &&
                   count(treat, "f.smaa") == 0 && count(treat, "f.hdr") == 0 && count(treat, "flatMonoResolve(") == 0,
               "the recovery is the spatial fallback with EASU set (SMAA on by default), the map frames' call, and never the temporal resolve");
        expect(count(treat, "s.havePrevious=false;") == 1 && count(treat, "s.temporalAccepted=false;") == 1 && count(treat, "s.previous") == 0 &&
                   count(treat, "havePrevious=true") == 0 && count(treat, "++s.accepted") == 0 && count(treat, "s.phase") == 1 &&
                   count(treat, "refuse(s)") == 0 && count(treat, "reset()") == 0,
               "it keeps no history (the previous frame is forgotten, nothing is accepted), reads the phase only to decline a jittered frame, and refuses nothing");
        expect(count(treat, "f.jitterX") == 0 && count(treat, "f.jitterY") == 0 && count(treat, "facts.jittered=s.phase.applied!=0;") == 1,
               "no jitter is handed to it, and a jittered frame is declined");
        expect(count(treat, "facts.temporalOn=true;") == 1 && count(treat, "FlatNoSceneSpatialWhy::Treat") >= 1,
               "the facts come from the context, the temporal engine's being on from the scope running at all");
        {
            const size_t admit = at(treat, "flatNoSceneSpatialAdmit(facts)"), call = at(treat, "flatMonoResolveSpatialFallback("),
                         swap = at(treat, "ctx->PSSetShaderResources(0,1,&spatial);replaced=true;");
            expect(admit != std::string::npos && call != std::string::npos && swap != std::string::npos && admit < call && call < swap,
                   "the facts are judged first, then the recovery runs, then the copy's slot 0 is swapped (the destructor puts the game's back)");
        }
        // The maps' recovery is what it was: one call, EASU asked, gated by the map focus alone.
        expect(count(runtime, "FlatMonoResolveFrame mapFrame=f;\n        mapFrame.easu=true;") == 1 &&
                   count(runtime, "flatMonoResolveSpatialFallback(s.device.Get(),ctx,mapFrame,&outputView,&spatialReason)") == 1 &&
                   count(runtime, "    if (flatRuntimeMapTemporalOff()) {") == 1,
               "the map frames' recovery is the call it was, behind the map focus alone");
        expect(count(runtime, "mapFrame.smaa") == 0, "...and still takes SMAA from the frame's default");

        // Where it runs. At the copy's verdict, for the frames the copy route called no-scene, before the Probe return, so a probe frame
        // is treated as a watched one; and a treated copy leaves the rest of the draw to the UI layer's composite and nothing else.
        const size_t verdict = at(ctor, "noSceneDone = selected.reason == FlatMonoReason::NoScene && !s.hdrTreated && treatNoScene(false);");
        const size_t flag = at(ctor, "s.noSceneFlag = noSceneDone;"), probe = at(ctor, "if (s.work == FlatWork::Probe) return;");
        const size_t ui = at(ctor, "if (flatUiLayerOn()) {"), early = at(ctor, "if (noSceneDone) return;"), hdr = at(ctor, "if (s.hdrTreated) {");
        const size_t merge = at(ctor, "const FlatFrameSeen seen = flatFrameSeenFor(selected.selected(), selected.reason);");
        expect(verdict != std::string::npos && flag != std::string::npos && probe != std::string::npos && ui != std::string::npos &&
                   early != std::string::npos && hdr != std::string::npos && merge != std::string::npos &&
                   merge < verdict && verdict < flag && flag < probe && probe < ui && ui < early && early < hdr,
               "the verdict (after the stand-down merge) treats and sets the flag before the Probe return; the early return follows the UI layer's block");
        expect(count(ctor, "treatNoScene(") == 2,
               "the recovery has two callers in the draw scope: the watched copy and the paused copy");
        // The paused frame: after the activity, owner and context checks, before the GPU span and the Paused return, behind the flag.
        const size_t active = at(ctor, "if (!flatRuntimeActive()) {"), owner = at(ctor, "if (!owner()) {"), context = at(ctor, "if (context != s.context.Get()) {");
        const size_t paused = at(ctor, "if (s.work == FlatWork::Paused && s.noScenePausedReady() &&"), open = at(ctor, "if (!s.gpuFrameTried) gpuFrameOpen(s, context);");
        const size_t pausedReturn = at(ctor, "if (s.work == FlatWork::Paused) return;");
        expect(active != std::string::npos && owner != std::string::npos && context != std::string::npos && paused != std::string::npos &&
                   open != std::string::npos && pausedReturn != std::string::npos &&
                   active < owner && owner < context && context < paused && paused < open && open < pausedReturn,
               "a paused draw is asked after the engine-on, owner and context checks, ahead of the GPU span and the Paused return");
        expect(count(ctor, "flatNoScenePausedCopyDraw(kind, count, instances, bindingShaderHash(BindSlot::Vs), bindingShaderHash(BindSlot::Ps))") == 1 &&
                   count(ctor, "if (!treatNoScene(true)) ctx = nullptr;") == 1,
               "the paused question is the flag, then the pure gate on the draw's own arguments and the two shadow hashes; a decline leaves the scope inert");
        expect(count(runtime, "if (s.work == FlatWork::Paused) return;") == 3,
               "the Paused returns the stand-down's wiring scan counts are all still there");
        // The flag: raised only by a treated watched no-scene copy; asked only with the stand-down standing for no-scene.
        expect(count(runtime, "return noSceneFlag && standDown.standing && standDown.standReason == FlatMonoReason::NoScene;") == 1 &&
                   count(runtime, "s.noSceneFlag = noSceneDone;") == 1 && count(runtime, "s.noSceneFlag = false;") == 1,
               "the flag is set from the watched copy's outcome, cleared when the stand-down ends, and asked with the standing reason no-scene");
        // The line: every window beside the copy route's, then reset.
        const std::string present = body(runtime, "void flatRuntimePresent(IDXGISwapChain* swap, uint64_t frame, HRESULT hr, UINT flags) {");
        const size_t weapon = at(present, "s.copyWeaponWindow.reset();"), line = at(present, "flatNoSceneSpatialFormatWindow(noSceneText, sizeof(noSceneText), s.noScenePausedReady(), s.noSceneWindow);"),
                     print = at(present, "Log::get().note(\"%s\", noSceneText);"), reset = at(present, "s.noSceneWindow.reset();");
        expect(weapon != std::string::npos && line != std::string::npos && print != std::string::npos && reset != std::string::npos &&
                   weapon < line && line < print && print < reset,
               "the census line is printed every window, after the weapon census, and its window reset");
        expect(count(runtime, "s.noSceneWindow.firstWriteCopies+=s.prefix.imageCopiesFirstWrite-oldImageFirstWrite;") == 1,
               "the first-write copies are counted from the model into the line's window");
        // The model: the exemption's whole condition, and the strictness that follows it.
        expect(count(model, "if (!valid && !t->writes.draws && !t->hdrBad && d.hdrCopyVerified && k.srvView[0] && k.srvResource[0] &&") == 1 &&
                   count(model, "k.srvResource[0] != k.color && fullViewport(k, k.width, k.height) && !k.depth && !k.dsv) {") == 1 &&
                   count(model, "if (t->imageCopyFirst && !flatProjectionB1Unread(k.vs, k.ps))") == 1,
               "the model's first-write exemption and its revocation by a pair that reads b1");
    }
    return failures;
}
