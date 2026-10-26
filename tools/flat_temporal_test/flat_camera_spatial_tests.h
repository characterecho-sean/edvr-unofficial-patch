#pragma once
// The Full System Scanner's zoom (docs/design-flat-ui-quality-2026-10-05.md, 2026-10-22). When the scanner zooms onto a body the game adds
// its reveal composite (VS 953C8123AD8DC13B, PS A473A867BDDBC342) as a second camera-bearing writer of the scene HDR, bound to the same b1
// as the deferred lighting pass whose witness camera is the HDR's reference, and the camera rows it binds differ from the reference by one
// float ulp in a few words. flatRuntimeSameCamera is an exact compare, so the HDR went bad (hdr-camera-changed) and the copy was refused
// (conflicting-hdr-target-or-camera) on a frame the map-temporal-off treatment would have recovered spatially: no temporal resolve, no
// jitter, no history, so no reprojection for a camera disagreement to corrupt.
//
// What this file pins (flat_runtime_model.h: FlatRuntimeTarget::badCauses, flatRuntimeCameraConflictSpatialOnly, the copy branch):
//   - the lighting-pass-then-reveal sequence is selected, with the reference camera, when the prefix says temporal AA is off;
//   - the same sequence with temporal AA on is refused as it always was, and a prefix nothing set is such a frame;
//   - every other fault stays a refusal under temporal-off: layout, target identity (depth), provenance, an explicit write, a mark that
//     names no cause, an uncertain frame; a camera disagreement that other faults also marked is refused and counted as kept;
//   - the counters' names and wiring in the runtime, which no rig can run.
// The Full System Scanner's tone pass (VS F9CFC798F21E9AEA, PS 6DE70EBBF82FCBE6) is in no whitelist, so the reducer's copy branch answers
// no-known-tone-pass before it asks about the HDR's faults, and the admission by structure (flat_copy_structure.h) takes the frame and
// asks the HDR route's selection (flatSelectHdrRouteAt), whose own test of the bad target refused it: flight 135701 (the first waiver, in the copy
// branch alone, read camera-conflict-spatial=0 camera-conflict-kept=0 with 5 s refusals of 59 to 291). The same cases run on both paths.
#include "flat_laser_scene_tone_tests.h"
#include "../../src/d3d11/flat_deferred_camera.h"
#include "../../src/d3d11/flat_frame_contract.h"
#include "../../src/d3d11/flat_copy_structure.h"
#include "../../src/d3d11/flat_hdr_route.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace camera_spatial_test {
using namespace edvr;
using laser_scene_tone_test::token;
using laser_scene_tone_test::draw;

constexpr uint64_t kFrame = 9001;
constexpr uint64_t kRevealVs = 0x953C8123AD8DC13Bull, kRevealPs = 0xA473A867BDDBC342ull;
// Frame 9201's donor camera and the lighting pass's CB2 rows (flat_deferred_camera_tests.h), a camera the deferred proof certifies.
inline const uint32_t kDonor[24] = {0xBF06B496,0xBFBAA737,0,0xB6C74B8F,0xBF695104,0x3F578757,0,0xB76E7C54,0x3786E4D9,0xB65DE5B1,0,0xBF800000,0,0,0x3CCCCCCD,0,0xB6C74B8F,0xB76E7C54,0xBF800000,0,0xC0D2CE6F,0x40FD9541,0xC1F57045,0};
inline const uint32_t kCb2[28] = {0xBF06B496,0xBF695104,0x3786E4D9,0x407068BF,0xBFBAA737,0x3F578757,0xB65DE5B1,0xC18239A7,0,0,0,0x3CCCCCCD,0xB6C74B8F,0xB76E7C54,0xBF800000,0xC1F5701E,0xBF7341C8,0x3F83AAA6,0x80000000,0xBD20A25A,0xBFD2AAA5,0xBF18091D,0,0x3F8F5723,0x37F398F1,0x361C872D,0,0xBF800084};

// The frame: eight pool draws (the source), eight lighting draws (the HDR's reference camera, by the witness), the reveal composite's
// draws, the tone pass and the game's copy. Each option is one way the frame can differ.
struct Options {
    bool temporalOff = true;       // the prefix's temporalOff
    unsigned reveals = 0;          // reveal composite draws into H, after the lighting pass
    bool ulp = true;               // ... each with the camera one float ulp from the reference in the four words the flight logged
    bool otherDepth = false;       // ... into another depth (DepthMismatch)
    bool wider = false;            // ... at another width (the HDR's layout changed)
    bool stale = false;            // ... with a camera written last frame (CameraProvenance after the camera change)
    bool lateOtherDepth = false;   // a further reveal draw after those, into another depth, with the reference camera
    bool explicitWrite = false;    // a write of H after the reveal draws (ExplicitWrite)
    bool unnamedMark = false;      // a mark that names no cause, as the forward authority's revocation makes (cause None)
    bool uncertain = false;        // the frame is uncertain
    bool viaContract = false;      // the copy goes through the frame contract's entry point, as the runtime's does
    bool admission = false;        // the scanner's frame: the scene at half the output, a tone pass the whitelist has never met, the HDR detector
                                   // watching every draw, and the verdict the admission by structure gives (Run::selected is that verdict)
};
struct Run {
    std::unique_ptr<FlatRuntimePrefix> p = std::make_unique<FlatRuntimePrefix>();
    std::unique_ptr<FlatFrameContract> contract = std::make_unique<FlatFrameContract>();
    std::unique_ptr<FlatHdrFrame> hdrFrame = std::make_unique<FlatHdrFrame>();
    FlatMonoFrame whitelist{};     // the reducer's answer at the copy draw (admission: no-known-tone-pass)
    FlatCopyDiag diag{};           // the admission's account of the frame
    FlatMonoFrame selected{};
    const void* hdr = token(0x4000);
    FlatRuntimeTarget* target = nullptr;
    unsigned char revealCamera[kFlatCameraBytes]{};
};
inline Run run(const Options& o) {
    Run r;
    FlatRuntimePrefix& p = *r.p;
    p.frame = kFrame; p.output = token(0x9000); p.width = 1920; p.height = 1200; p.format = 28;
    p.temporalOff = o.temporalOff;
    p.uncertain = o.uncertain;
    flatHdrBeginFrame(*r.hdrFrame, p.width, p.height);
    // The scene's size: the output's in the whitelist's frame, half of it in the scanner's (960x600 into 1920x1200).
    const uint32_t sceneW = o.admission ? 960u : 1920u, sceneH = o.admission ? 600u : 1200u;
    const auto sized = [&](FlatRuntimeDraw d, uint32_t w, uint32_t h) {
        d.key.width = w; d.key.height = h;
        if (d.key.depth) { d.key.depthWidth = w; d.key.depthHeight = h; }
        d.key.viewport[2] = float(w); d.key.viewport[3] = float(h);
        return d;
    };
    // A draw as the runtime sees it: the reducer, then (the scanner's frame) the HDR detector, handed the slots it asks for.
    const auto observe = [&](FlatRuntimeDraw& d, const void* t1 = nullptr) {
        flatRuntimeObserve(p, d);
        if (!o.admission) return;
        const void* srv[4] = {nullptr, t1, nullptr, nullptr};
        const bool known = flatHdrCouldConsume(*r.hdrFrame, d.key);
        flatHdrObserveDraw(*r.hdrFrame, d.key, p.sequence, known ? srv : nullptr, known);
    };
    auto pool = sized(draw(token(0x1000), 23, token(0x2000), token(0x2001), 0xBFE51414CC3024B4ull, 0xDB79AE788E049DFDull), sceneW, sceneH);
    pool.supported = true; pool.key.kind = kFlatContractPool;
    laser_scene_tone_test::camera(pool, p.frame, 1);
    std::memcpy(pool.camera, kDonor, sizeof(pool.camera)); pool.key.cameraHash = flatCameraHash(pool.camera);
    for (unsigned i = 0; i < 8; ++i) observe(pool);
    auto h = sized(draw(r.hdr, 26, pool.key.depth, pool.key.dsv, 0x7E38A6AA1269C901ull, 0x7CECABDE34FFBE9Eull), sceneW, sceneH);
    h.key.kind = kFlatContractScreen; h.key.srvResource[0] = pool.key.color; h.key.srvView[0] = token(0x1002);
    std::memcpy(h.camera, kDonor, sizeof(h.camera)); h.key.cameraHash = pool.key.cameraHash;
    auto& meta = h.key.projection[1];
    meta.resource = token(0x6000); meta.width = 784; meta.copied = 112; meta.hash = 123;
    meta.status = FlatProjectionStatus::Available; meta.writeEpoch = p.frame; meta.writeSeq = p.sequence;
    h.deferredHdrVerified = flatRuntimeDeferredDonor(p, h.key, p.sequence + 1) &&
        flatMatchDeferredCamera(reinterpret_cast<const unsigned char*>(kDonor), reinterpret_cast<const unsigned char*>(kCb2)) == FlatDeferredCameraMatch::Match;
    for (unsigned i = 0; i < 8; ++i) { observe(h); h.deferredHdrVerified = false; }
    // One reveal draw: the lighting pass's b1, its camera (or one ulp from it), into H.
    const auto reveal = [&](bool otherDepth, bool ulp, bool wider, bool stale) {
        auto d = sized(draw(r.hdr, 26, otherDepth ? token(0x2100) : pool.key.depth, otherDepth ? token(0x2101) : pool.key.dsv, kRevealVs, kRevealPs),
                       sceneW, sceneH);
        d.key.kind = kFlatContractScreen;
        laser_scene_tone_test::camera(d, p.frame, p.sequence + 1);
        std::memcpy(d.camera, kDonor, sizeof(d.camera));
        if (ulp) for (const unsigned word : {0u, 4u, 8u, 9u}) {   // 270.0, 271.0, 272.0, 272.1: the words the 13:08:27.509 line names
            uint32_t value = 0; std::memcpy(&value, d.camera + word * 4, 4); --value; std::memcpy(d.camera + word * 4, &value, 4);
        }
        d.key.cameraHash = flatCameraHash(d.camera);
        if (stale) d.key.writeEpoch = p.frame - 1;
        if (wider) { d.key.width = d.key.depthWidth = 2 * sceneW; d.key.viewport[2] = float(2 * sceneW); }
        std::memcpy(r.revealCamera, d.camera, sizeof(r.revealCamera));
        observe(d);
    };
    for (unsigned i = 0; i < o.reveals; ++i) reveal(o.otherDepth, o.ulp, o.wider, o.stale);
    if (o.lateOtherDepth) reveal(true, false, false, false);
    r.target = flatRuntimeTarget(p, r.hdr);
    if (o.explicitWrite) flatRuntimeWritten(p, r.hdr);
    if (o.unnamedMark && r.target) flatRuntimeNoteBad(*r.target, FlatRuntimeConflict::None);
    // The tone pass: a pair the whitelist knows (the cockpit's), or the scanner's, which no table names. Either writes the R-sized
    // R8G8B8A8 target the game's copy reads.
    auto tone = sized(draw(token(0x5000), 27, nullptr, nullptr, o.admission ? 0xF9ull : flat_mono_detail::kToneVs,
                           o.admission ? 0xFEull : flat_mono_detail::kTonePs), sceneW, sceneH);
    tone.key.kind = kFlatContractScreen; tone.key.depthWidth = tone.key.depthHeight = 0;
    if (!o.admission) { tone.key.srvResource[1] = r.hdr; tone.key.srvView[1] = token(0x4002); }
    observe(tone, r.hdr);
    auto copy = draw(p.output, 28, nullptr, nullptr, flat_mono_detail::kCopyVs, flat_mono_detail::kCopyPs);
    copy.key.kind = kFlatContractOutput; copy.key.depthWidth = copy.key.depthHeight = 0;
    copy.key.srvResource[0] = tone.key.color; copy.key.srvView[0] = token(0x5002);
    r.whitelist = (o.viaContract || o.admission) ? flatRuntimeObserveContract(p, copy, *r.contract) : flatRuntimeObserve(p, copy);
    r.selected = r.whitelist;
    if (o.admission) {
        const void* srv[4] = {copy.key.srvResource[0], nullptr, nullptr, nullptr};
        const bool known = flatHdrCouldConsume(*r.hdrFrame, copy.key);
        flatHdrObserveDraw(*r.hdrFrame, copy.key, p.sequence, known ? srv : nullptr, known);
        FlatCopyPolicy policy; policy.structure = true; policy.mode = FlatMonoResolveMode::Xess;   // the flight's mode: XeSS, 960x600 into 1920x1200
        r.selected = flatCopyAdmit(p, *r.hdrFrame, copy, r.whitelist, policy, &r.diag);
    }
    r.target = flatRuntimeTarget(p, r.hdr);
    return r;
}
inline uint32_t bit(FlatRuntimeConflict cause) { return 1u << static_cast<uint32_t>(cause); }

inline size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}
inline std::string slurp(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
inline std::string between(const std::string& text, const std::string& from, const std::string& to) {
    const size_t a = text.find(from);
    if (a == std::string::npos) return {};
    const size_t b = text.find(to, a + from.size());
    return b == std::string::npos ? std::string{} : text.substr(a, b - a);
}
}  // namespace camera_spatial_test

inline int flatCameraSpatialTests() {
    using namespace edvr;
    using namespace camera_spatial_test;
    int failures = 0;
    const auto expect = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: camera spatial %s\n", name); ++failures; }
    };
    // The same cases on both paths a frame reaches the verdict by: the reducer's copy branch (a tone pass the whitelist knows) and the
    // admission by structure (the scanner's, which no whitelist names; the path flight 135701 took).
    for (const bool admission : {false, true}) {
        const char* path = admission ? "[admission by structure] " : "[copy branch] ";
        const auto say = [&](const char* text) { return std::string(path) + text; };
        const auto check = [&](bool ok, const char* text) { expect(ok, say(text).c_str()); };
        const auto refusedAs = [](const Run& r, FlatRuntimeConflict cause) {
            return r.selected.reason == FlatMonoReason::ConflictingHdr && r.p->selectedConflict.cause == cause;
        };
        const auto make = [&](Options o) { o.admission = admission; return o; };

        // ---- the control: the lighting pass alone is the pre-zoom frame, selected under either answer ------------------------------
        for (const bool off : {true, false}) {
            Options o; o.temporalOff = off;
            const Run r = run(make(o));
            check(r.selected.selected() && r.selected.hdr == r.hdr && r.target && !r.target->hdrBad && r.target->badCauses == 0 &&
                      !r.p->cameraConflictWaived && !r.p->cameraConflictKept,
                  "the lighting pass with no reveal is selected, untouched, whichever way temporal AA is");
            check(std::memcmp(r.selected.camera, kDonor, sizeof(kDonor)) == 0, "...with the witness camera as the scene camera");
            if (admission)
                check(r.whitelist.reason == FlatMonoReason::NoTonePass && r.diag.outcome == FlatCopyOutcome::Admitted,
                      "...the whitelist knows no tone pass and the structure admits the frame");
        }

        // ---- the zoom: the reveal composite's camera one ulp from the reference ------------------------------------------------------
        Run control; { Options o; control = run(make(o)); }
        {
            Options o; o.reveals = 1; o.viaContract = true;
            const Run r = run(make(o));
            check(std::memcmp(r.revealCamera, kDonor, sizeof(kDonor)) != 0, "the reveal draw's camera is not the reference's bytes");
            check(r.target && r.target->hdrBad && r.target->firstBad.cause == FlatRuntimeConflict::CameraChange &&
                      r.target->badCauses == bit(FlatRuntimeConflict::CameraChange) && r.target->hdrCamera && !r.target->hdrLayoutChanged,
                  "the reveal makes the HDR bad for the camera change and for nothing else (the flight's hdr-camera-changed)");
            check(r.selected.selected() && r.p->cameraConflictWaived && !r.p->cameraConflictKept,
                  "temporal AA off: the camera-only fault is waived and the frame is selected");
            check(r.selected.hdr == control.selected.hdr && r.selected.depth == control.selected.depth && r.selected.dsv == control.selected.dsv &&
                      r.selected.color == control.selected.color && r.selected.sceneConstants == control.selected.sceneConstants &&
                      r.selected.renderWidth == control.selected.renderWidth && r.selected.renderHeight == control.selected.renderHeight &&
                      std::memcmp(r.selected.camera, control.selected.camera, sizeof(r.selected.camera)) == 0 &&
                      std::memcmp(r.selected.camera, kDonor, sizeof(kDonor)) == 0 &&
                      std::memcmp(r.selected.camera, r.revealCamera, sizeof(kDonor)) != 0,
                  "...as the same HDR, depth and size with the reference camera, never the reveal's: the frame the lighting pass alone gives");
            check(r.p->selectedConflict.cause == FlatRuntimeConflict::None, "...with no conflict left on the prefix");
            if (admission)
                check(r.whitelist.reason == FlatMonoReason::NoTonePass && r.diag.outcome == FlatCopyOutcome::Admitted,
                      "...admitted by structure over the whitelist's no-known-tone-pass");
            else
                check(r.contract->copiesUsed == 1 && r.contract->copies[0].selected() &&
                          r.contract->copyConflicts[0].cause == FlatRuntimeConflict::None,
                      "...and the frame contract records the copy as selected with no conflict");
        }
        for (const unsigned reveals : {1u, 3u}) {
            Options o; o.reveals = reveals; o.temporalOff = false;
            const Run r = run(make(o));
            check(refusedAs(r, FlatRuntimeConflict::CameraChange) && !r.p->cameraConflictWaived && !r.p->cameraConflictKept,
                  "temporal AA on: the same sequence is refused for the camera change, and neither waived nor kept");
            if (admission)
                check(r.diag.outcome == FlatCopyOutcome::Refused, "...by the admission's selector (selector-refused in the census)");
        }
        {
            Options o; o.reveals = 3;
            const Run r = run(make(o));
            check(r.selected.selected() && r.p->cameraConflictWaived && r.target && r.target->badCauses == bit(FlatRuntimeConflict::CameraChange),
                  "temporal AA off: a reveal drawn several times is still camera-only and waived");
        }

        // ---- every other fault stays a refusal under temporal-off ---------------------------------------------------------------------
        struct Case { const char* name; Options o; FlatRuntimeConflict first; bool kept; };
        const auto base = [] { Options o; o.reveals = 1; return o; };
        Case cases[] = {
            {"another depth, same camera (target identity)", [&] { auto o = base(); o.ulp = false; o.otherDepth = true; return o; }(), FlatRuntimeConflict::DepthMismatch, false},
            {"another depth and the camera change in one draw", [&] { auto o = base(); o.otherDepth = true; return o; }(), FlatRuntimeConflict::DepthMismatch, false},
            {"the camera change, then a draw into another depth", [&] { auto o = base(); o.lateOtherDepth = true; return o; }(), FlatRuntimeConflict::CameraChange, true},
            {"the camera change from a camera written last frame (provenance)", [&] { auto o = base(); o.stale = true; return o; }(), FlatRuntimeConflict::CameraChange, true},
            {"the camera change at another width (layout)", [&] { auto o = base(); o.wider = true; return o; }(), FlatRuntimeConflict::CameraChange, true},
            {"the camera change, then an explicit write of H", [&] { auto o = base(); o.explicitWrite = true; return o; }(), FlatRuntimeConflict::CameraChange, true},
            {"the camera change, then a mark naming no cause", [&] { auto o = base(); o.unnamedMark = true; return o; }(), FlatRuntimeConflict::CameraChange, true},
        };
        for (const Case& c : cases) {
            const Run r = run(make(c.o));
            const std::string label = std::string(c.name) + ": refused under temporal-off";
            check(!r.selected.selected() && r.selected.reason == FlatMonoReason::ConflictingHdr && refusedAs(r, c.first) &&
                      !r.p->cameraConflictWaived && r.p->cameraConflictKept == c.kept,
                  label.c_str());
            if (c.kept) {
                // ...and the same case is not camera-only: the mask holds the camera change and a second cause, or the layout changed.
                check(r.target && (r.target->badCauses & bit(FlatRuntimeConflict::CameraChange)) &&
                          (r.target->badCauses != bit(FlatRuntimeConflict::CameraChange) || r.target->hdrLayoutChanged),
                      "a kept frame's target holds the camera change and a second fault (a second cause, or a changed layout)");
            }
            if (admission)
                check(r.diag.outcome == FlatCopyOutcome::Refused, "...by the admission's selector");
        }
        {
            Options o = base(); o.uncertain = true;
            const Run r = run(make(o));
            check(r.selected.reason == FlatMonoReason::Truncated && !r.p->cameraConflictWaived && !r.p->cameraConflictKept,
                  "an uncertain frame is refused as it always was: the waiver is not reached");
        }
    }
    // The route's own selection at its trigger is not the waiver's to ask: with the same bad H it refuses, and notes nothing.
    {
        Options o; o.reveals = 1; o.admission = true;
        Run r = run(o);
        r.p->cameraConflictWaived = r.p->cameraConflictKept = false;
        const FlatMonoFrame atTrigger = flatSelectHdrRoute(*r.p, *r.hdrFrame, [](uint64_t, uint64_t) { return true; });
        expect(atTrigger.reason == FlatMonoReason::ConflictingHdr && r.p->selectedConflict.cause == FlatRuntimeConflict::CameraChange &&
                   !r.p->cameraConflictWaived && !r.p->cameraConflictKept,
               "the HDR route's selection at its trigger still refuses a bad H, camera-only or not, and sets neither flag");
        Options ok = o; ok.reveals = 0;
        Run clean = run(ok);
        expect(flatSelectHdrRoute(*clean.p, *clean.hdrFrame, [](uint64_t, uint64_t) { return true; }).reason != FlatMonoReason::ConflictingHdr,
               "...and the same call over an H with no reveal is not a conflict (the control for the line above)");
    }

    // ---- the predicate itself: each term is needed ---------------------------------------------------------------------------
    {
        const auto holder = std::make_unique<FlatRuntimePrefix>();
        FlatRuntimePrefix& p = *holder;
        FlatRuntimeTarget t;
        p.temporalOff = true; t.hdrBad = t.hdrCamera = true; t.badCauses = bit(FlatRuntimeConflict::CameraChange);
        expect(flatRuntimeCameraConflictSpatialOnly(p, t), "predicate: temporal off, bad, camera-bearing, camera change alone");
        p.temporalOff = false;
        expect(!flatRuntimeCameraConflictSpatialOnly(p, t), "predicate: not with temporal AA on");
        p.temporalOff = true;
        { auto u = t; u.hdrBad = false; expect(!flatRuntimeCameraConflictSpatialOnly(p, u), "predicate: not for a target that is not bad"); }
        { auto u = t; u.hdrCamera = false; expect(!flatRuntimeCameraConflictSpatialOnly(p, u), "predicate: not for a target with no reference camera"); }
        { auto u = t; u.hdrLayoutChanged = true; expect(!flatRuntimeCameraConflictSpatialOnly(p, u), "predicate: not for a changed layout"); }
        { auto u = t; u.badCauses = 0; expect(!flatRuntimeCameraConflictSpatialOnly(p, u), "predicate: not for a bad mark that names nothing"); }
        for (uint32_t cause = 0; cause < static_cast<uint32_t>(FlatRuntimeConflict::Count); ++cause) {
            if (cause == static_cast<uint32_t>(FlatRuntimeConflict::CameraChange)) continue;
            auto u = t; u.badCauses |= 1u << cause;
            if (flatRuntimeCameraConflictSpatialOnly(p, u)) { expect(false, "predicate: not with any other cause marked beside the camera change"); break; }
            auto w = t; w.badCauses = 1u << cause;
            if (flatRuntimeCameraConflictSpatialOnly(p, w)) { expect(false, "predicate: not for any cause but the camera change"); break; }
        }
        FlatRuntimeTarget n; flatRuntimeNoteBad(n, FlatRuntimeConflict::None);
        expect(n.hdrBad && n.badCauses == 1u && n.firstBad.cause == FlatRuntimeConflict::None,
               "a mark that names no cause sets bit 0 and the bad flag, and leaves the first witness alone");
    }

    // ---- the wiring no rig can run -----------------------------------------------------------------------------------------
    {
        const std::string runtime = slurp("src/d3d11/flat_runtime.cpp");
        const std::string model = slurp("src/d3d11/flat_runtime_model.h");
        const std::string forward = slurp("src/d3d11/flat_forward_light_runtime.inl");
        expect(!runtime.empty() && !model.empty() && !forward.empty(), "the runtime, model and forward-light sources are readable from the repo root");
        // The prefix answer is the frame's one read, put after the prefix is cleared, and it is the answer the treatment asks.
        const size_t read = runtime.find("const bool temporalOff = flatRuntimeMapTemporalOff();");
        const size_t clear = runtime.find("if (s.work != FlatWork::Paused) s.prefix = FlatRuntimePrefix{};");
        const size_t set = runtime.find("s.prefix.temporalOff = temporalOff;");
        expect(countOf(runtime, "const bool temporalOff = flatRuntimeMapTemporalOff();") == 1 && countOf(runtime, "s.prefix.temporalOff = ") == 1 &&
                   read != std::string::npos && clear != std::string::npos && set != std::string::npos && read < clear && clear < set,
               "the prefix's temporalOff is the frame's one GuiFocus read, assigned once, after the prefix is cleared");
        expect(countOf(runtime, "    if (flatRuntimeMapTemporalOff()) {") == 1,
               "the treatment the waiver feeds is the one map-temporal-off block, behind the same answer");
        // The spatial count is taken in that block, on the recovery's success; the kept count where the conflict is reported.
        const std::string treat = between(runtime, "    if (flatRuntimeMapTemporalOff()) {", "fetchCelestialRecords(ctx, f);");
        const size_t okAt = treat.find("flatUiLayerMapAaCopy(true,easu,smaa);"), countAt = treat.find("if (s.prefix.cameraConflictWaived) ++s.cameraConflictSpatialWindow;"),
                     failAt = treat.find("flatUiLayerMapAaCopy(false);");
        expect(!treat.empty() && countOf(runtime, "++s.cameraConflictSpatialWindow") == 1 && okAt != std::string::npos && countAt != std::string::npos &&
                   failAt != std::string::npos && okAt < countAt && countAt < failAt,
               "camera-conflict-spatial is counted once, on the map frame's successful spatial recovery and not on its refusal");
        const std::string refusal = between(runtime, "if (selected.reason == FlatMonoReason::ConflictingHdr) {", "if(s.phase.applied)recover(s.reason);");
        expect(countOf(runtime, "++s.cameraConflictKeptWindow") == 1 && refusal.find("reportConflict(s);") != std::string::npos &&
                   refusal.find("if (s.prefix.cameraConflictKept) ++s.cameraConflictKeptWindow;") != std::string::npos,
               "camera-conflict-kept is counted once, where the selector's conflict is reported");
        // The 5 s line names both counters (zeros print), and the window resets them.
        const std::string focus = between(runtime, "Log::get().note(\"flat map focus 5s:", ");\n            }");
        expect(countOf(runtime, "camera-conflict-spatial=%llu") == 1 && countOf(runtime, "camera-conflict-kept=%llu") == 1 &&
                   focus.find("camera-conflict-spatial=%llu camera-conflict-kept=%llu") != std::string::npos &&
                   focus.find("s.cameraConflictSpatialWindow") != std::string::npos && focus.find("s.cameraConflictKeptWindow") != std::string::npos,
               "the flat map focus 5s line prints camera-conflict-spatial and camera-conflict-kept, from the window counters");
        expect(countOf(runtime, "s.cameraConflictSpatialWindow = s.cameraConflictKeptWindow = 0;") == 1,
               "the 5 s window resets both counters");
        // The model: one place marks a target bad, every writer of the flag goes through it, and the waiver is behind the predicate.
        expect(countOf(model, "hdrBad = true") == 1 && countOf(model, "inline void flatRuntimeNoteBad(FlatRuntimeTarget& t, FlatRuntimeConflict cause) {") == 1 &&
                   countOf(forward, "hdrBad=true") == 0 && countOf(forward, "flatRuntimeNoteBad(target,FlatRuntimeConflict::None)") == 1,
               "the bad flag is set in one function, flatRuntimeNoteBad, and the forward authority's revocation names no cause through it");
        expect(countOf(model, "if (t.hdrBad && flatRuntimeBadTargetRefuses(p, t)) {") == 1 &&
                   countOf(model, "inline bool flatRuntimeBadTargetRefuses(FlatRuntimePrefix& p, const FlatRuntimeTarget& t) {") == 1 &&
                   countOf(model, "if (flatRuntimeCameraConflictSpatialOnly(p, t)) { p.cameraConflictWaived = true; return false; }") == 1 &&
                   countOf(model, "if (p.temporalOff && t.firstBad.cause == FlatRuntimeConflict::CameraChange) p.cameraConflictKept = true;") == 1,
               "the copy branch refuses a bad target unless the one question holds, which notes the waiver and the kept refusal");
        // The admission by structure: the scanner's frames (a tone pass no whitelist names) reach their verdict here, not in the copy branch.
        // It is the one asker of the route's selection with the waiver; the route's own selection at its trigger is not.
        const std::string route = slurp("src/d3d11/flat_hdr_route.h");
        const std::string structure = slurp("src/d3d11/flat_copy_structure.h");
        expect(!route.empty() && !structure.empty(), "the HDR route and the copy structure sources are readable from the repo root");
        const std::string trigger = between(route, "return flatSelectHdrRouteAt(p, f, supportedPair, f.trigger.sequence,", ");");
        expect(countOf(route, "if (target->hdrBad && (!waiveCameraOnly || flatRuntimeBadTargetRefuses(p, *target))) {") == 1 &&
                   countOf(route, "void* qualifiedAlternateUser = nullptr, bool waiveCameraOnly = false) {") == 1 &&
                   !trigger.empty() && trigger.find("true") == std::string::npos && trigger.find("waiveCameraOnly") == std::string::npos,
               "the route's selection takes the waiver as a parameter that defaults off, and its own call at the trigger does not ask it");
        expect(countOf(structure, "flatSelectHdrRouteAt(") == 1 &&
                   countOf(structure, "FlatHdrExtentGate::UniformHalfToDouble, nullptr, nullptr, nullptr, nullptr, true);") == 1,
               "the admission by structure asks the route's selection with the waiver, once");
        const std::string predicate = between(model, "inline bool flatRuntimeCameraConflictSpatialOnly(", "\n}\n");
        expect(predicate.find("p.temporalOff") != std::string::npos && predicate.find("t.hdrCamera") != std::string::npos &&
                   predicate.find("!t.hdrLayoutChanged") != std::string::npos && predicate.find("t.badCauses == (1u << static_cast<uint32_t>(FlatRuntimeConflict::CameraChange))") != std::string::npos,
               "the predicate asks temporal AA off, the reference camera, the layout, and the camera change as the only cause");
    }
    std::printf("flat camera spatial: %d failure(s)\n", failures);
    return failures;
}
