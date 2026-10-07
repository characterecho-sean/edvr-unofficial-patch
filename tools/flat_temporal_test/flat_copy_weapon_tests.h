// The copy route's weapon support on the rig (design section 104; flat_copy_structure.h: flatWeaponRoute, the mixed-camera mark in
// flatCopyAdmit and in the reducer's copy branch; flat_runtime_model.h: FlatRuntimeDraw::firstPersonCohort and alternateHdr).
//
// WHAT THE FRAME LOOKS LIKE. A weapon up adds two kinds of draw to a frame the scene-camera model cannot take as they are: the
// first-person pool cohort (an alternate camera, into the scene's depth, before the world) and the glow pass (an HDR draw from a
// second camera, no depth write). The HDR route judges them when it treats the frame: a protected overlay for the glow pass, the
// qualified alternate source for the cohort. The copy route has neither, so every such frame was refused at render below the output
// (AmbiguousSource for the cohort, ConflictingHdr for the glow). This file pins the replacement: the runtime flags both kinds of
// draw (only where flatWeaponRoute says the copy route judges the frame), the flags ride the trace, the pure model takes the cohort
// out of the sources and the glow out of the camera conflict, and the selected frame is mixed-camera, which makes the runtime ask the
// foreground contract at the final copy. The five experiments of the planning (A to E, then F) are the model's first pins:
//   A  weapon down                                 -> selected                  (control)
//   B  glow pass as a protected overlay, unsealed  -> ConflictingHdr (overlay-suffix)   the 11:49 flight
//   C  glow pass as a plain second camera          -> ConflictingHdr (camera-change)
//   D  the cohort alone                            -> AmbiguousSource
//   E  the laser pair alone                        -> selected                  (exempt by shader hash)
//   F  the cohort and the glow pass                -> AmbiguousSource today; selected and mixed-camera with the flags
// A to E are what a frame whose draws carry no flag has always got, and stay so. Only F changes, and only for flagged draws.
//
// THE SECOND WITNESS. The cohort flag covers supported first-person pool draws. A weapon whose first-person draws are unsupported pairs (the
// plasma weapon) reaches the model as nothing, so the runtime also takes the domain's word: a first-person draw it planned into the
// frame's depth (DomainCandidate::foreignPlanned) makes the copy route's selected frame mixed-camera too (flatCopyMixedCamera). The
// domain's side is a runtime count no rig drives, so it is pinned as a truth table and as source (with mutation controls) below.
#pragma once
#include "../../src/d3d11/flat_copy_structure.h"
#include "../../src/d3d11/flat_trace.h"
#include "flat_copy_structure_tests.h"
#include "flat_hdr_route_tests.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace copy_weapon_test {

using hdr_route_test::Scene;
using hdr_route_test::Stream;
using hdr_route_test::tok;

// The model never reads these (the runtime's predicate names the weapon family; the model sees only the flags and the cameras). They
// are the pairs the flights name, so a trace dump of a weapon frame reads like these streams.
constexpr uint64_t kCohortVs = 0xAACFDCF2FB9AD809ull, kCohortPs = 0xCF534B32F491561Aull;
constexpr uint64_t kGlowVs = 0x025B4B9FF54622EDull, kGlowPs = 0x46F92DC71BF8DFA5ull;
constexpr uint64_t kLaserVs = 0x88DCF1164C640EC3ull, kLaserPs = 0x494506A63091DF8Cull;

// The weapon's draws, each a way a frame can differ. The defaults are the weapon-down frame.
struct Weapon {
    uint32_t cohort = 0;               // first-person pool draws, drawn before the world, under an alternate camera, into the scene's depth
    bool cohortFlag = false;           // ... carried as FlatRuntimeDraw::firstPersonCohort
    bool cohortOtherDepth = false;     // ... into a depth of their own (flagged or not as above), not the scene's
    bool cohortTwoDepths = false;      // ... half of them in the scene's depth and half in another (a cohort in two depths)
    bool glow = false;                 // the glow pass: an HDR draw from a second camera that writes no depth, after the scene's draws
    bool glowAlternate = false;        // ... carried as FlatRuntimeDraw::alternateHdr
    bool glowOverlay = false;          // ... planned as a protected overlay, the HDR route's way,
    bool glowOverlayFailed = false;    //     and refused as unsealed at the final copy (the runtime's overlay-unsealed-at-final-copy)
    bool glowDepthWrite = false;       // ... with an effective depth write
    bool glowOtherDepth = false;       // ... into the scene's HDR with another depth bound
    bool glowAfterHdrBad = false;      // ... after a plain second camera already made the scene HDR conflict
    bool glowFirst = false;            // ... drawn before the scene's own draws: it is the first camera on H, not a second one
    bool laser = false;                // the laser pair: a second camera by its shader hashes
    bool knownTone = true;             // a whitelisted tone pass wrote the copy's source (else an unknown one: only the structure admits)
};

struct Built {
    std::unique_ptr<Stream> stream;
    std::unique_ptr<edvr::FlatFrameContract> contract = std::make_unique<edvr::FlatFrameContract>();
    std::vector<edvr::FlatTraceEvent> events;    // what the runtime's trace would hold for the frame, in order
    edvr::FlatRuntimeDraw copy{};
    edvr::FlatMonoFrame selected{};              // the copy draw's own verdict (the reducer's: the whitelist's, or no-known-tone-pass)
    copy_structure_test::ParsedFrame frame;      // the same events as a parsed trace frame, header and contract hash included
};

// The first-person camera: the world's pose with another near (0.0675 against 0.025) and another x scale.
inline edvr::FlatRuntimeDraw alternateCamera(edvr::FlatRuntimeDraw d) {
    float rows[6][4]; std::memcpy(rows, d.camera, sizeof(rows));
    rows[3][2] += 0.0425f; rows[0][0] *= 1.3f;
    std::memcpy(d.camera, rows, sizeof(d.camera));
    d.key.camera = d.camera; d.key.cameraHash = edvr::flatCameraHash(d.camera);
    return d;
}

// One frame at supersampling 0.75 (2880x1620 into 3840x2160: the copy route's frame), the draws in the order the game makes them, each
// fed to the model and recorded the way the runtime's trace records it.
inline Built build(const Weapon& w) {
    using namespace edvr;
    Built out;
    Scene sc; sc.hW = 2880; sc.hH = 1620;
    out.stream = std::make_unique<Stream>(sc);
    Stream& s = *out.stream;
    out.events.push_back(flatTraceEventMarker(kFlatTraceEventCameraCapture, nullptr));   // the camera's capture opens the frame
    // The runtime asks the detector's cheap gate before the model sees the draw, and records the four slots only when it asks.
    const auto emit = [&](FlatRuntimeDraw d, const void* t0 = nullptr, const void* t1 = nullptr) {
        if (d.key.camera) d.key.camera = d.camera;
        FlatTraceEvent e = flatTraceEventFromDraw(d, false);
        const void* srv[4] = {t0, t1, nullptr, nullptr};
        if (flatHdrCouldConsume(s.hdr, d.key)) flatTraceEventSetSrv(e, srv);
        out.events.push_back(e);
        s.draw(d, t0, t1);
    };
    const void* gbuf = tok(0x3200);
    s.write(sc.h);
    out.events.push_back(flatTraceEventMarker(kFlatTraceEventWriteResource, sc.h));
    const auto glowDraw = [&]() {
        FlatRuntimeDraw d = alternateCamera(s.make(sc.h, w.glowOtherDepth ? sc.h2Depth : sc.hDepth, sc.hW, sc.hH, 26, kGlowVs, kGlowPs, true, false));
        d.alternateHdr = w.glowAlternate;
        d.overlayProtected = w.glowOverlay;
        d.effectiveDepthWrite = w.glowDepthWrite;
        emit(d);
    };
    if (w.glow && w.glowFirst) glowDraw();
    for (uint32_t i = 0; i < w.cohort; ++i) {
        const bool other = w.cohortOtherDepth || (w.cohortTwoDepths && i >= w.cohort / 2);
        FlatRuntimeDraw d = alternateCamera(s.make(gbuf, other ? sc.h2Depth : sc.hDepth, sc.hW, sc.hH, 23, kCohortVs, kCohortPs, true, true));
        d.firstPersonCohort = w.cohortFlag;
        emit(d);
    }
    // The scene's draws into H (Stream::sceneDraws, recorded): four pool-family draws, the motion source, and six of lighting and glass.
    for (uint32_t i = 0; i < 4; ++i) emit(s.make(sc.h, sc.hDepth, sc.hW, sc.hH, 26, 0xA1, 0xB1, true, true));
    for (uint32_t i = 0; i < 6; ++i) emit(s.make(sc.h, sc.hDepth, sc.hW, sc.hH, 26, 0xA2 + i % 3, 0xB2, true, false));
    if (w.glowAfterHdrBad) {
        emit(alternateCamera(s.make(sc.h, sc.hDepth, sc.hW, sc.hH, 26, 0x1234, 0x5678, true, false)));   // a plain second camera: H is bad now
    }
    if (w.glow && !w.glowFirst) {
        glowDraw();
        if (w.glowOverlayFailed) {
            flatRuntimeOverlayFailed(*s.prefix, sc.h);
            out.events.push_back(flatTraceEventMarker(kFlatTraceEventOverlayFailed, sc.h));
        }
    }
    if (w.laser) emit(alternateCamera(s.make(sc.h, sc.hDepth, sc.hW, sc.hH, 26, kLaserVs, kLaserPs, true, false)));
    // The tone pass reads H (a whitelisted pair at t1, or a pair the whitelist has never met) and writes S; the game's copy reads S.
    FlatRuntimeDraw t = s.make(sc.tone, nullptr, sc.hW, sc.hH, 27, w.knownTone ? flat_mono_detail::kToneVs : 0xF9,
                               w.knownTone ? flat_mono_detail::kTonePs : 0xFE, false, false);
    if (w.knownTone) { t.key.srvView[1] = tok(0x7001); t.key.srvResource[1] = sc.h; }
    emit(t, nullptr, sc.h);
    FlatRuntimeDraw c = s.make(sc.output, nullptr, sc.outW, sc.outH, 28, flat_mono_detail::kCopyVs, flat_mono_detail::kCopyPs, false, false);
    out.copy = copy_structure_test::withSrv(c, sc.tone);
    out.selected = flatRuntimeObserveContract(*s.prefix, out.copy, *out.contract);
    {
        const void* srv[4] = {out.copy.key.srvResource[0], nullptr, nullptr, nullptr};
        const bool known = flatHdrCouldConsume(s.hdr, out.copy.key);
        FlatTraceEvent e = flatTraceEventFromDraw(out.copy, false);
        if (known) flatTraceEventSetSrv(e, srv);
        out.events.push_back(e);
        flatHdrObserveDraw(s.hdr, out.copy.key, s.prefix->sequence, known ? srv : nullptr, known);
    }
    out.frame.header.frame = hdr_route_test::kFrame; out.frame.header.output = sc.output;
    out.frame.header.width = sc.outW; out.frame.header.height = sc.outH; out.frame.header.format = 28;
    out.frame.header.eventCount = uint32_t(out.events.size());
    out.frame.header.produced = 1; out.frame.header.contractHash = flatFrameContractHash(*out.contract);
    out.frame.events = out.events;
    return out;
}
inline edvr::FlatMonoFrame admit(Built& b, edvr::FlatMonoResolveMode mode, edvr::FlatCopyDiag* diag = nullptr) {
    return edvr::flatCopyAdmit(*b.stream->prefix, b.stream->hdr, b.copy, b.selected, copy_structure_test::policy(true, mode), diag);
}

}  // namespace copy_weapon_test

inline int flatCopyWeaponTests() {
    using namespace edvr;
    using namespace copy_weapon_test;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: copy weapon %s\n", name); ++failures; }
    };
    const auto conflict = [](const Built& b) { return b.stream->prefix->selectedConflict.cause; };

    // ---- where a frame is judged: flatWeaponRoute's whole table, at the sizes the sessions run ---------------------------
    {
        constexpr uint32_t dW = 3840, dH = 2160;
        struct Size { uint32_t w, h; const char* name; };
        const Size below{2880, 1620, "SS 0.75"}, at{3840, 2160, "SS 1.0"}, above{5760, 3240, "SS 1.5"};
        struct Cell { bool autoKey, latched; FlatMonoResolveMode mode; const Size* size; FlatWeaponRoute want; const char* name; };
        using M = FlatMonoResolveMode;
        using R = FlatWeaponRoute;
        const Cell cells[] = {
            // key auto and not latched: the HDR route treats every frame whose mode evaluates at the render size (R >= D; EDVR's TAA only at R = D)
            {true, false, M::Dlss, &at, R::Hdr, "auto, DLSS at the output"},
            {true, false, M::Dlss, &above, R::Hdr, "auto, DLSS supersampled"},
            {true, false, M::Fsr, &at, R::Hdr, "auto, FSR at the output"},
            {true, false, M::Fsr, &above, R::Hdr, "auto, FSR supersampled"},
            {true, false, M::Dlaa, &at, R::Hdr, "auto, DLAA at the output"},
            {true, false, M::Taa, &at, R::Hdr, "auto, TAA at the output"},
            // ... and the copy route judges the rest: the upscalers below the output
            {true, false, M::Dlss, &below, R::Copy, "auto, DLSS below the output"},
            {true, false, M::Fsr, &below, R::Copy, "auto, FSR below the output"},
            // EDVR's TAA and DLAA keep what they had where the HDR route does not treat the frame (DLAA has no route below the output)
            {true, false, M::Dlaa, &below, R::Unchanged, "auto, DLAA below the output (no route)"},
            {true, false, M::Taa, &below, R::Unchanged, "auto, TAA below the output"},
            {true, false, M::Taa, &above, R::Unchanged, "auto, TAA supersampled (the display grid: the copy route's)"},
            // the key off: the HDR route treats nothing, at any size; the upscalers are the copy route's
            {false, false, M::Dlss, &below, R::Copy, "key off, DLSS below the output"},
            {false, false, M::Dlss, &at, R::Copy, "key off, DLSS at the output"},
            {false, false, M::Dlss, &above, R::Copy, "key off, DLSS supersampled"},
            {false, false, M::Fsr, &at, R::Copy, "key off, FSR at the output"},
            {false, false, M::Dlaa, &at, R::Unchanged, "key off, DLAA"},
            {false, false, M::Taa, &at, R::Unchanged, "key off, TAA"},
            // the route latched off for the session: as the key off
            {true, true, M::Dlss, &at, R::Copy, "auto but latched, DLSS at the output"},
            {true, true, M::Fsr, &above, R::Copy, "auto but latched, FSR supersampled"},
            {true, true, M::Dlaa, &at, R::Unchanged, "auto but latched, DLAA"},
        };
        bool all = true;
        for (const Cell& c : cells) {
            const FlatWeaponRoute got = flatWeaponRoute(c.autoKey, c.latched, c.mode, c.size->w, c.size->h, dW, dH);
            if (got != c.want) {
                std::printf("  flatWeaponRoute(%s) = %s, wanted %s\n", c.name, flatWeaponRouteName(got), flatWeaponRouteName(c.want));
                all = false;
            }
        }
        expect(all, "flatWeaponRoute: the HDR route's frames stay its own, the upscalers get the copy route where it will not treat them, TAA and DLAA keep what they had");
        expect(std::strcmp(flatWeaponRouteName(R::Hdr), "hdr") == 0 && std::strcmp(flatWeaponRouteName(R::Copy), "copy") == 0 &&
                   std::strcmp(flatWeaponRouteName(R::Unchanged), "unchanged") == 0,
               "the routes have their words");
    }

    // ---- experiments A to E: a frame whose draws carry no flag is judged as it always was ------------------------------------
    {
        const Built a = build(Weapon{});
        expect(a.selected.selected() && !a.selected.mixedCamera && a.selected.supportedDraws == 4, "A: the weapon-down frame is selected, one camera, four sources");
        Weapon bw; bw.glow = true; bw.glowOverlay = true; bw.glowOverlayFailed = true;
        const Built b = build(bw);
        expect(b.selected.reason == FlatMonoReason::ConflictingHdr && conflict(b) == FlatRuntimeConflict::OverlaySuffix,
               "B: a glow pass planned as a protected overlay and never sealed refuses the frame (overlay-suffix): what the copy route does not do any more");
        Weapon cw; cw.glow = true;
        const Built c = build(cw);
        expect(c.selected.reason == FlatMonoReason::ConflictingHdr && conflict(c) == FlatRuntimeConflict::CameraChange,
               "C: a glow pass with no flag is a plain second camera on H and refuses the frame (hdr-camera-changed), as it always did");
        Weapon dw; dw.cohort = 7;
        const Built d = build(dw);
        expect(d.selected.reason == FlatMonoReason::AmbiguousSource && !d.selected.mixedCamera,
               "D: the cohort with no flag is an alternate-camera source in the scene's depth: ambiguous, as it always was");
        Weapon ew; ew.laser = true;
        const Built e = build(ew);
        expect(e.selected.selected() && !e.selected.mixedCamera && e.stream->prefix->firstPersonDraws == 0,
               "E: the laser pair alone is exempt by its shader hashes: selected, one camera, no cohort counted");
        expect(a.stream->prefix->firstPersonDraws == 0 && c.stream->prefix->firstPersonDraws == 0 && d.stream->prefix->firstPersonDraws == 0,
               "an unflagged frame counts no first-person draw");
    }

    // ---- experiment F, and the cohort and the glow pass each alone: the flags change what the model does -------------------------
    {
        Weapon fw; fw.cohort = 7; fw.cohortFlag = true; fw.glow = true; fw.glowAlternate = true;
        const Built f = build(fw);
        expect(f.selected.selected() && f.selected.mixedCamera,
               "F: the cohort and the glow pass, both flagged: the copy route selects the frame and it is mixed-camera (AmbiguousSource before the flags)");
        expect(f.selected.supportedDraws == 4 && f.stream->prefix->firstPersonDraws == 7 && f.stream->prefix->firstPersonDepth == f.stream->sc.hDepth,
               "F: the cohort is counted (seven draws, the scene's depth) and joins no source record: the world's four draws are the sources");
        expect(f.selected.depth == f.stream->sc.hDepth && f.stream->prefix->selectedConflict.cause == FlatRuntimeConflict::None,
               "F: the selection names the scene's depth and no conflict was left behind");
        Weapon cohortOnly; cohortOnly.cohort = 7; cohortOnly.cohortFlag = true;
        const Built g = build(cohortOnly);
        expect(g.selected.selected() && g.selected.mixedCamera, "the flagged cohort alone selects the frame, mixed-camera");
        Weapon glowOnly; glowOnly.glow = true; glowOnly.glowAlternate = true;
        const Built h = build(glowOnly);
        expect(h.selected.selected() && !h.selected.mixedCamera,
               "the flagged glow pass alone selects the frame and it is not mixed-camera: no cohort, nothing for the foreground contract to qualify");
        Weapon both; both.cohort = 7; both.cohortFlag = true; both.laser = true;
        const Built i = build(both);
        expect(i.selected.selected() && i.selected.mixedCamera, "the laser pair beside the flagged cohort: selected, mixed-camera (the hash exemption stands beside the flag)");
        Weapon glowFlagOnly; glowFlagOnly.cohort = 7; glowFlagOnly.glow = true; glowFlagOnly.glowAlternate = true;
        const Built j = build(glowFlagOnly);
        expect(j.selected.reason == FlatMonoReason::AmbiguousSource,
               "the flagged glow pass does not rescue an unflagged cohort: the cohort is what makes the frame ambiguous");
        Weapon cohortFlagOnly; cohortFlagOnly.cohort = 7; cohortFlagOnly.cohortFlag = true; cohortFlagOnly.glow = true;
        const Built k = build(cohortFlagOnly);
        expect(k.selected.reason == FlatMonoReason::ConflictingHdr && conflict(k) == FlatRuntimeConflict::CameraChange,
               "the flagged cohort does not rescue an unflagged glow pass: the glow pass is what conflicts on H");
    }

    // ---- the flags are conditions of the draw, not exemptions: a flag on a draw that does not meet them is no flag -----------
    {
        Weapon depthWrite; depthWrite.cohort = 7; depthWrite.cohortFlag = true; depthWrite.glow = true; depthWrite.glowAlternate = true;
        depthWrite.glowDepthWrite = true;
        const Built a = build(depthWrite);
        expect(!a.selected.selected() && a.selected.reason == FlatMonoReason::ConflictingHdr,
               "an alternate HDR draw that writes depth is a second camera on H and refuses the frame, flag or not");
        Weapon overlay; overlay.cohort = 7; overlay.cohortFlag = true; overlay.glow = true; overlay.glowAlternate = true;
        overlay.glowOverlay = true; overlay.glowOverlayFailed = true;
        const Built b = build(overlay);
        expect(!b.selected.selected() && conflict(b) == FlatRuntimeConflict::OverlaySuffix,
               "a draw planned as a protected overlay stays on the overlay's rules: the alternate flag does not rescue an unsealed suffix");
        Weapon otherDepth; otherDepth.cohort = 7; otherDepth.cohortFlag = true; otherDepth.glow = true; otherDepth.glowAlternate = true;
        otherDepth.glowOtherDepth = true;
        const Built c = build(otherDepth);
        expect(!c.selected.selected(), "an alternate HDR draw into H with another depth bound is not the scene's second camera: the frame is refused");
        Weapon bad; bad.cohort = 7; bad.cohortFlag = true; bad.glowAfterHdrBad = true; bad.glow = true; bad.glowAlternate = true;
        const Built d = build(bad);
        expect(!d.selected.selected() && d.selected.reason == FlatMonoReason::ConflictingHdr,
               "an alternate flag does not clear an H that a plain second camera already made conflicting");
        Weapon first; first.cohort = 7; first.cohortFlag = true; first.glow = true; first.glowAlternate = true; first.glowFirst = true;
        const Built firstBuilt = build(first);
        expect(!firstBuilt.selected.selected() && firstBuilt.selected.reason == FlatMonoReason::ConflictingHdr,
               "the alternate flag exempts a second camera only: a flagged glow pass that is the first camera on H is the scene's camera, and the world after it conflicts");
        Weapon twoDepths; twoDepths.cohort = 8; twoDepths.cohortFlag = true; twoDepths.cohortTwoDepths = true;
        const Built e = build(twoDepths);
        expect(!e.selected.selected() && e.selected.reason == FlatMonoReason::Truncated && e.stream->prefix->uncertain,
               "a flagged cohort in two depths cannot name the scene it belongs to: the frame is uncertain and refused");
        Weapon elsewhere; elsewhere.cohort = 7; elsewhere.cohortFlag = true; elsewhere.cohortOtherDepth = true;
        const Built f = build(elsewhere);
        expect(f.selected.selected() && !f.selected.mixedCamera && f.stream->prefix->firstPersonDraws == 7 &&
                   f.stream->prefix->firstPersonDepth == f.stream->sc.h2Depth,
               "a flagged cohort in a depth that is not the scene's leaves the frame as it was: selected, one camera, nothing for the contract to qualify");
    }

    // ---- the structure admission (the whitelist has never met the tone pass): the same frames, the same answers ----------------
    {
        const auto policyMode = FlatMonoResolveMode::Dlss;
        Weapon plain; plain.knownTone = false;
        Built a = build(plain);
        FlatCopyDiag da;
        const FlatMonoFrame sa = admit(a, policyMode, &da);
        expect(sa.selected() && !sa.mixedCamera && da.outcome == FlatCopyOutcome::Admitted,
               "structure: the weapon-down frame is admitted, one camera (control)");
        Weapon fw; fw.knownTone = false; fw.cohort = 7; fw.cohortFlag = true; fw.glow = true; fw.glowAlternate = true;
        Built f = build(fw);
        FlatCopyDiag df;
        const FlatMonoFrame sf = admit(f, policyMode, &df);
        expect(sf.selected() && sf.mixedCamera && df.outcome == FlatCopyOutcome::Admitted,
               "structure: the flagged cohort and glow pass are admitted and the frame is mixed-camera");
        Weapon noFlags; noFlags.knownTone = false; noFlags.cohort = 7; noFlags.glow = true;
        Built n = build(noFlags);
        FlatCopyDiag dn;
        const FlatMonoFrame sn = admit(n, policyMode, &dn);
        expect(!sn.selected() && dn.outcome == FlatCopyOutcome::Refused,
               "structure: the same draws with no flags are refused, as they always were");
    }

    // ---- the flags ride the trace ----------------------------------------------------------------------------------------
    {
        const uint32_t older = kFlatTraceHasCamera | kFlatTraceSupported | kFlatTraceHdrCopyVerified | kFlatTraceMenuCopyVerified |
                               kFlatTraceImageSourceVerified | kFlatTraceForeignWork | kFlatTraceHdrSrvKnown | kFlatTraceOverlayProtected |
                               kFlatTraceDepthWrite | kFlatTraceStencilWrite;
        expect((kFlatTraceFirstPersonCohort & older) == 0 && (kFlatTraceAlternateHdr & older) == 0 &&
                   kFlatTraceFirstPersonCohort != kFlatTraceAlternateHdr,
               "the two new trace bits are bits no older flag uses");
        Stream s;
        FlatRuntimeDraw d = s.make(s.sc.h, s.sc.hDepth, s.sc.hW, s.sc.hH, 26, 0xA1, 0xB1, true, true);
        FlatRuntimeDraw both = d; both.firstPersonCohort = true; both.alternateHdr = true;
        const FlatTraceEvent eb = flatTraceEventFromDraw(both, false);
        const FlatRuntimeDraw back = flatTraceEventToDraw(eb);
        expect((eb.flags & kFlatTraceFirstPersonCohort) && (eb.flags & kFlatTraceAlternateHdr) && back.firstPersonCohort && back.alternateHdr,
               "both flags survive the trace event and come back as the draw's");
        FlatRuntimeDraw cohortOnly = d; cohortOnly.firstPersonCohort = true;
        FlatRuntimeDraw glowOnly = d; glowOnly.alternateHdr = true;
        const FlatTraceEvent ec = flatTraceEventFromDraw(cohortOnly, false), eg = flatTraceEventFromDraw(glowOnly, false);
        const FlatRuntimeDraw bc = flatTraceEventToDraw(ec), bg = flatTraceEventToDraw(eg);
        expect((ec.flags & kFlatTraceFirstPersonCohort) && !(ec.flags & kFlatTraceAlternateHdr) && bc.firstPersonCohort && !bc.alternateHdr &&
                   !(eg.flags & kFlatTraceFirstPersonCohort) && (eg.flags & kFlatTraceAlternateHdr) && !bg.firstPersonCohort && bg.alternateHdr,
               "each flag is its own bit");
        const FlatTraceEvent en = flatTraceEventFromDraw(d, false);
        const FlatRuntimeDraw bn = flatTraceEventToDraw(en);
        expect(!(en.flags & (kFlatTraceFirstPersonCohort | kFlatTraceAlternateHdr)) && !bn.firstPersonCohort && !bn.alternateHdr,
               "a draw with no flag records none, and replays with none");
    }
    {
        Weapon fw; fw.cohort = 7; fw.cohortFlag = true; fw.glow = true; fw.glowAlternate = true;
        const Built live = build(fw);
        const hdr_route_test::FrameFacts replay = hdr_route_test::replayFrame(live.frame);
        expect(replay.copySelected && replay.hashMatches,
               "a flagged weapon frame replays from its trace to the live verdict and the live contract hash");
        const auto replayed = copy_structure_test::replayCopy(live.frame, copy_structure_test::policy(false, FlatMonoResolveMode::Dlss));
        expect(replayed.copy && replayed.whitelist.selected() && replayed.whitelist.mixedCamera &&
                   copy_structure_test::sameSelection(replayed.whitelist, live.selected),
               "the replay's copy selection is the live one, mixed-camera included");
        copy_structure_test::ParsedFrame stripped = live.frame;
        for (auto& e : stripped.events) e.flags &= ~(kFlatTraceFirstPersonCohort | kFlatTraceAlternateHdr);
        const auto without = copy_structure_test::replayCopy(stripped, copy_structure_test::policy(false, FlatMonoResolveMode::Dlss));
        expect(without.copy && !without.whitelist.selected() && without.whitelist.reason == FlatMonoReason::ConflictingHdr,
               "the same trace with the two bits cleared replays to a refusal: the bits are what the replay reads, not something it infers");
    }
    {
        // The corpus replays as it always did: no committed trace carries either bit, so the new model paths never run on it.
        namespace fs = std::filesystem;
        const fs::path dir("tools/flat_temporal_test/traces");
        uint32_t files = 0, events = 0, flagged = 0;
        if (fs::exists(dir)) {
            for (const auto& entry : fs::directory_iterator(dir)) {
                if (entry.path().extension() != ".bin") continue;
                std::vector<unsigned char> bytes;
                std::vector<hdr_route_test::ParsedFrame> frames;
                if (!hdr_route_test::readFile(entry.path(), &bytes) || !hdr_route_test::parseTrace(bytes, &frames)) continue;
                ++files;
                for (const auto& pf : frames)
                    for (const auto& e : pf.events) {
                        ++events;
                        if (e.flags & (kFlatTraceFirstPersonCohort | kFlatTraceAlternateHdr)) ++flagged;
                    }
            }
        }
        expect(files == 17 && events > 0 && flagged == 0, "no committed trace carries a weapon flag: the 17-trace corpus replays through the unchanged paths");
    }

    // ---- the copy route's second mixed-camera witness: the domain's ----------------------------------------------------------
    // The model's cohort flag covers SUPPORTED first-person pool draws. A weapon whose draws are unsupported pairs (the plasma weapon: an
    // F10 trace has 8B589D25, 7B0DC42D, AACFDCF2/CAD1F585, CFCA8FFC supported=0 and F516BF02 in no family) reaches the model as nothing at
    // all, so only the domain, which planned the draws by their own bytes, knows the frame is mixed-camera.
    {
        using R = FlatWeaponRoute;
        expect(!flatCopyMixedCamera(false, R::Copy, 0), "no witness, no mixed-camera frame (a weapon-down frame with no first-person draw)");
        expect(flatCopyMixedCamera(true, R::Copy, 0), "the model's witness alone makes the frame mixed-camera");
        expect(flatCopyMixedCamera(false, R::Copy, 1) && flatCopyMixedCamera(false, R::Copy, 5),
               "the domain's witness alone makes the frame mixed-camera: an unsupported weapon, holstered arms");
        expect(flatCopyMixedCamera(true, R::Copy, 3), "both witnesses agree");
        expect(!flatCopyMixedCamera(false, R::Unchanged, 4) && !flatCopyMixedCamera(false, R::Hdr, 4),
               "the domain's witness counts only where the copy route judges the weapon: EDVR's TAA and DLAA, and the HDR route's frames, keep what they had");
        expect(flatCopyMixedCamera(true, R::Unchanged, 0) && flatCopyMixedCamera(true, R::Hdr, 0),
               "the model's own verdict is never taken back");
    }

    // ---- the census line ---------------------------------------------------------------------------------------------------
    {
        FlatCopyWeaponWindow w;
        char line[600];
        flatCopyWeaponFormatWindow(line, sizeof(line), "dlss", w);
        expect(std::strstr(line, "flat copy weapon 5s: mode=dlss cohort-draws=0 alternate-glow-draws=0 mixed-frames=0 domain-mixed-frames=0 H-attempts=0 H-qualified=0") == line,
               "the weapon census line prints every field, zeros included (an absent line is what the wiring never running looks like)");
        w.cohortDraws = 70; w.alternateDraws = 30; w.mixedFrames = 10; w.domainMixedFrames = 3; w.hAttempts = 10; w.hQualified = 4;
        flatCopyWeaponFormatWindow(line, sizeof(line), "fsr", w);
        expect(std::strstr(line, "mode=fsr cohort-draws=70 alternate-glow-draws=30 mixed-frames=10 domain-mixed-frames=3 H-attempts=10 H-qualified=4") != nullptr,
               "the weapon census line carries the window's counts");
        w.reset();
        expect(w.cohortDraws == 0 && w.alternateDraws == 0 && w.mixedFrames == 0 && w.domainMixedFrames == 0 && w.hAttempts == 0 && w.hQualified == 0,
               "the window resets");
        char tiny[24];
        flatCopyWeaponFormatWindow(tiny, sizeof(tiny), "dlss", w);
        expect(std::strlen(tiny) < sizeof(tiny), "the line is truncated, never overrun, in a small buffer");
    }
    return failures;
}

// ---- the runtime wiring no rig can run, as source pins with mutation controls ---------------------------------------------
inline int flatCopyWeaponWiringTests() {
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: copy weapon wiring %s\n", name); ++failures; }
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
    const auto count = [](const std::string& text, const std::string& needle) {
        size_t n = 0, at = 0;
        while ((at = text.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
        return n;
    };
    // The needles occur in this order (each after the one before), compared with every space removed.
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
    // The first `needle` after the first `anchor`, taken out (the one that matters where the needle's text is not unique).
    const auto withoutAfter = [&](std::string text, const char* anchor, const char* needle) {
        const std::string a = compact(anchor), n = compact(needle);
        const size_t from = text.find(a);
        const size_t at = from == std::string::npos ? std::string::npos : text.find(n, from);
        if (at != std::string::npos) text.erase(at, n.size());
        return text;
    };

    const std::string runtime = slurp("src/d3d11/flat_runtime.cpp");
    const std::string resolve = slurp("src/d3d11/flat_mono_resolve.cpp");
    const std::string model = slurp("src/d3d11/flat_runtime_model.h");
    expect(!runtime.empty() && !resolve.empty() && !model.empty(), "the runtime, resolver and model sources are readable from the repo root");

    // -- the draw hook: the disposition, then the flags, then the model; no overlay where the copy route judges the frame --
    const std::string scope = compact(body(runtime, "FlatRuntimeDrawScope::FlatRuntimeDrawScope("));
    const auto dispositionValid = [&](const std::string& text) {
        return ordered(text, {
            "flatWeaponRoute(s.hdrKey==FlatHdrKey::Auto,s.hdrLatch.tripped,s.engine,k.width,k.height,", "==FlatWeaponRoute::Copy;",
            "weaponMotionFamilyVs(k.vs)", "copyWeapon()) {", "d.firstPersonCohort=!predictedWorld&&!worldCamera;",
            "(s.hdrKey==FlatHdrKey::Auto || copyWeapon())", "if(copyWeapon()) {", "d.alternateHdr=true;", "} else {",
            "d.overlayProtected=true;", "overlayPlanned=true;", "FlatMonoFrame selected = [&]() -> FlatMonoFrame {",
            "flatRuntimeObserve(s.prefix, d);"}) &&
            count(text, "d.overlayProtected=true;") == 1 && count(text, "overlayPlanned=true;") == 1 &&
            count(text, "d.alternateHdr=true;") == 1 && count(text, "d.firstPersonCohort=") == 1;
    };
    expect(dispositionValid(scope),
           "the draw hook asks flatWeaponRoute, flags the cohort, plans an overlay only where the copy route does not judge the frame (and admits the glow pass as an alternate there), all before the model sees the draw");
    expect(!dispositionValid(without(scope, "d.alternateHdr=true;")),
           "mutation control: the glow pass admitted as nothing fails the wiring");
    expect(!dispositionValid(withoutAfter(scope, "d.alternateHdr=true;", "} else {")), "mutation control: an overlay planned in both routes fails the wiring");
    expect(!dispositionValid(without(scope, "d.firstPersonCohort=!predictedWorld&&!worldCamera;")), "mutation control: an unflagged cohort fails the wiring");
    expect(!dispositionValid(without(scope, "||copyWeapon())")), "mutation control: an overlay gate that ignores the copy route's disposition fails the wiring");
    {
        std::string moved = scope;   // the overlay planned before the disposition
        const std::string plan = compact("d.overlayProtected=true;");
        const size_t at = moved.find(plan), before = moved.find(compact("if(copyWeapon()) {"));
        if (at != std::string::npos && before != std::string::npos && before < at) { moved.erase(at, plan.size()); moved.insert(before, plan); }
        expect(!dispositionValid(moved), "mutation control: an overlay planned ahead of the disposition fails the wiring");
    }
    expect(model.find("k.vs == 0x88DCF1164C640EC3ull && k.ps == 0x494506A63091DF8Cull") != std::string::npos &&
               compact(model).find(compact("const bool secondCamera = laserPair || alternate;")) != std::string::npos,
           "the laser pair stays exempt by its shader hashes, beside the flag");

    // -- the copy route's treatment: the foreground contract after the engine source views, before the resolver --
    const auto copyCallValid = [&](const std::string& text) {
        return ordered(text, {"engineVelocitySourceViews(", "FlatForegroundMotion::Output foregroundOutput;",
                              "foregroundContractAtH(s,ctx,selected,f,foregroundOutput);", "flatMonoResolve(s.device.Get(), ctx, f, &outputView, &s.reason)"});
    };
    expect(copyCallValid(scope), "the copy route asks the foreground contract after its engine source views and before the resolver");
    expect(!copyCallValid(without(scope, "foregroundContractAtH(s,ctx,selected,f,foregroundOutput);")),
           "mutation control: a copy route that never asks the contract fails the wiring");
    const auto mixedValid = [&](const std::string& text) {
        return ordered(text, {"const auto* planned = domainCandidate(s, selected.depth);", "const bool modelMixed = selected.mixedCamera;",
                              "selected.mixedCamera = flatCopyMixedCamera(modelMixed,", "flatWeaponRoute(s.hdrKey == FlatHdrKey::Auto, s.hdrLatch.tripped, s.engine,",
                              "planned ? planned->foreignPlanned : 0);", "++s.copyWeaponWindow.mixedFrames;", "++s.copyWeaponWindow.domainMixedFrames;",
                              "FlatComputeInternalScope guard;", "foregroundContractAtH("});
    };
    expect(mixedValid(scope),
           "the copy route's treatment takes the model's witness and the domain's (the depth candidate's planned first-person draws) before the contract is asked, counting the domain's share");
    expect(!mixedValid(without(scope, "selected.mixedCamera = flatCopyMixedCamera(modelMixed,")),
           "mutation control: a copy route that ignores the domain's witness fails the wiring");
    expect(!mixedValid(without(scope, "planned ? planned->foreignPlanned : 0);")), "mutation control: a witness that is never asked of the candidate fails the wiring");
    {
        // The count itself: kept per frame on the depth candidate, where the planner counts the foreign draw.
        const std::string all2 = compact(runtime);
        const auto countValid = [&](const std::string& text) {
            return ordered(text, {"uint32_t foreignPlanned=0;", "void beginFrame(uint64_t next) {", "foreignPlanned=0;"}) &&
                   ordered(text, {"++s.foregroundCounts.foreignSeen;++candidate->foreignPlanned;"}) &&
                   count(text, "++candidate->foreignPlanned;") == 1;
        };
        expect(countValid(all2), "the depth candidate counts the first-person draws planned into it, beside foreignSeen, and zeroes the count with its frame");
        expect(!countValid(without(all2, "++candidate->foreignPlanned;")), "mutation control: a planner that does not count its draws fails the wiring");
        expect(!countValid(without(all2, "colorWritten=false;foreignPlanned=0;")), "mutation control: a count never zeroed with the frame fails the wiring");
    }
    const std::string treat = compact(body(runtime, "void FlatRuntimeDrawScope::treatHdr("));
    const auto treatValid = [&](const std::string& text) {
        return ordered(text, {"engineVelocitySourceViews(", "FlatForegroundMotion::Output foregroundOutput;",
                              "foregroundContractAtH(s,ctx,selected,f,foregroundOutput);", "reach(\"resolve\");"});
    };
    expect(treatValid(treat), "the HDR route asks the same contract, from the same helper, at the same point");
    expect(!treatValid(without(treat, "foregroundContractAtH(s,ctx,selected,f,foregroundOutput);")),
           "mutation control: an HDR route that never asks the contract fails the wiring");
    const std::string all = compact(runtime);
    expect(count(all, "motion.prepareH(") == 1 && count(all, "foregroundContractAtH(s,ctx,selected,f,foregroundOutput);") == 2,
           "the foreground map is qualified in one place, called from both routes");

    // -- the shared helper: the HDR route's block, moved whole, its pending-null mismatch line intact --
    const std::string helper = compact(body(runtime, "static void foregroundContractAtH("));
    const auto helperValid = [&](const std::string& text) {
        return ordered(text, {
            "f.foregroundRequired=f.mode!=FlatMonoResolveMode::Taa&&selected.mixedCamera;", "if(!f.foregroundRequired)return;",
            "s.foregroundRoute.pin(selected.depth);", "++s.foregroundCounts.hAttempts;", "if(!f.hdr)++s.copyWeaponWindow.hAttempts;",
            "candidate->pendingNull.matches(", "failH(\"foreground-pending-null-not-selected-world\");",
            "candidate->pendingNull.describeMismatch(", "flat foreground pending-null mismatch %u/12",
            "failH(\"foreground-provisional-HDR-not-selected\");", "failH(\"foreground-uncertain-frame\");",
            "failH(\"foreground-prior-unknown-mutation\");", "failH(\"foreground-current-owner-plane-unavailable\");",
            "candidate->motion.prepareH(", "f.foregroundMotion=foregroundOutput.motion.Get();", "f.foregroundQualified=foregroundOutput.qualified;",
            "f.foregroundResetRequired=foregroundOutput.resetRequired;", "f.foregroundFrame=foregroundOutput.frame;",
            "f.foregroundDepthNear=foregroundOutput.depthNear;", "++s.foregroundCounts.hQualified;", "if(!f.hdr)++s.copyWeaponWindow.hQualified;"});
    };
    expect(helperValid(helper), "the helper holds the whole H qualification, in the HDR route's order, and counts the copy route's attempts apart");
    expect(!helperValid(without(helper, "f.foregroundFrame=foregroundOutput.frame;")), "mutation control: a map handed over without its frame fails the wiring");
    expect(!helperValid(without(helper, "flat foreground pending-null mismatch %u/12")), "mutation control: losing the mismatch line fails the wiring");
    expect(!helperValid(without(helper, "f.foregroundResetRequired=foregroundOutput.resetRequired;")), "mutation control: losing the reset request fails the wiring");
    expect(!helperValid(without(helper, "if(!f.foregroundRequired)return;")), "mutation control: a helper that asks TAA or a one-camera frame fails the wiring");

    // -- the census line --
    const std::string present = compact(body(runtime, "void flatRuntimePresent(IDXGISwapChain* swap, uint64_t frame, HRESULT hr, UINT flags) {"));
    const auto lineValid = [&](const std::string& text) {
        return ordered(text, {"flatCopyFormatWindow(copyText,", "s.copyWindow.reset();", "flatCopyWeaponFormatWindow(weaponText,",
                              "s.copyWeaponWindow);", "Log::get().note(\"%s\", weaponText);", "s.copyWeaponWindow.reset();"});
    };
    expect(lineValid(present), "the weapon census line is printed every window, beside the copy structure's, and its window reset");
    expect(!lineValid(without(present, "s.copyWeaponWindow.reset();")), "mutation control: a window that is never reset fails the wiring");

    // -- the resolver: the foreground contract belongs to neither route; a mask still needs the HDR route --
    const std::string resolver = compact(resolve);
    const auto resolverValid = [&](const std::string& text) {
        return text.find(compact("(!foreground || !f.foregroundQualified || f.foregroundFrame!=f.frame))")) != std::string::npos &&
               text.find(compact("(!foreground || !hdr || !f.foregroundQualified")) == std::string::npos &&
               text.find(compact("if(untrusted && ((!foreground && f.mode!=FlatMonoResolveMode::Taa) || !hdr ||")) != std::string::npos;
    };
    expect(resolverValid(resolver), "the resolver's foreground clause names no route, and the mask clause keeps the HDR route");
    {
        std::string restored = resolver;   // the old clause, with the route's requirement back in
        const std::string clause = compact("(!foreground || !f.foregroundQualified || f.foregroundFrame!=f.frame))");
        const size_t at = restored.find(clause);
        if (at != std::string::npos) restored.replace(at, clause.size(), compact("(!foreground || !hdr || !f.foregroundQualified || f.foregroundFrame!=f.frame))"));
        expect(!resolverValid(restored), "mutation control: the foreground clause that demands the HDR route again fails the wiring");
        std::string noMask = resolver;
        const std::string maskNeedle = compact("(!foreground && f.mode!=FlatMonoResolveMode::Taa) || !hdr ||");
        const size_t mask = noMask.find(maskNeedle);
        if (mask != std::string::npos) noMask.replace(mask, maskNeedle.size(), compact("(!foreground && f.mode!=FlatMonoResolveMode::Taa) ||"));
        expect(!resolverValid(noMask), "mutation control: a mask the copy route would take fails the wiring");
    }
    return failures;
}
