#pragma once
// The second-camera line (flat_runtime.cpp: noteSecondCamera). A deferred lighting pass with no source record on its depth, whose witness
// entry raised conflict=second-camera, had its two cameras reduced to hashes, so no log could say whether the second one is a real second
// view, a stale first camera, or the same camera with a rounding difference. The entry now keeps the offender's 96 bytes, the donor hands
// both out, and flatExplainSecondCamera sets each against the pass's CB2 and against the other. Everything here is log only: these tests pin
// that the bytes are the writers' own, that nothing a decision reads moved, and that the report agrees with the matcher it explains.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include "flat_deferred_witness_tests.h"
#include "../../src/d3d11/flat_deferred_reject.h"
#include "flat_deferred_forward_omission_captures.h"
namespace deferred_second_camera {
using namespace edvr;
using deferred_witness_test::Scene;
using laser_scene_tone_test::token;

inline unsigned run() {
    unsigned failures = 0;
    const auto expect = [&](bool ok, const char* name) { if (!ok) { ++failures; std::printf("FAIL: deferred second camera %s\n", name); } };
    using Why = FlatDeferredDonorWhy;

    // ---- the entry keeps the offender's bytes, the donor hands them out ---------------------------------------------------------------
    {
        Scene s;
        const auto first = s.writer(nullptr, 0x100);
        s.see(first);
        auto second = s.writer(nullptr, 0x200);
        second.camera[80] ^= 1; second.key.cameraHash = flatCameraHash(second.camera);
        s.see(second);
        const auto* e = s.entry(s.gbuf);
        const auto d = s.donor(s.lighting());
        expect(e && e->conflict == FlatWitnessConflict::Camera && e->offenderHasCamera, "a different camera is a camera conflict that keeps the offender's rows");
        expect(e && std::memcmp(e->offenderCamera, second.camera, kFlatCameraBytes) == 0 && std::memcmp(e->camera, first.camera, kFlatCameraBytes) == 0 &&
               std::memcmp(e->offenderCamera, e->camera, kFlatCameraBytes) != 0, "the offender's rows are the second writer's, the entry's the first's");
        expect(!d && d.why == Why::Conflict && d.conflict == FlatWitnessConflict::Camera && d.entryCamera == e->camera && d.offenderCamera == e->offenderCamera,
               "the donor stays refused and names both cameras");
        expect(d.camera == nullptr && d.role == FlatDeferredDonorRole::None, "neither camera becomes a donor");
        Scene b;   // the same bytes behind another constant buffer: a conflict whose two cameras are equal byte for byte
        b.see(b.writer(nullptr, 0x100));
        auto other = b.writer(nullptr, 0x200); other.key.b1 = token(0x3001); b.see(other);
        const auto* be = b.entry(b.gbuf);
        expect(be && be->conflict == FlatWitnessConflict::Camera && be->offenderHasCamera &&
               std::memcmp(be->offenderCamera, be->camera, kFlatCameraBytes) == 0, "another b1 with equal bytes keeps equal rows");
        Scene n;   // a provenance conflict (no camera rows) keeps nothing
        n.see(n.writer(nullptr, 0x100));
        auto bare = n.writer(nullptr, 0x200); bare.key.camera = nullptr; bare.key.cameraHash = 0; n.see(bare);
        const auto nd = n.donor(n.lighting());
        expect(n.entry(n.gbuf)->conflict == FlatWitnessConflict::CameraProvenance && !n.entry(n.gbuf)->offenderHasCamera &&
               nd.offenderCamera == nullptr && nd.entryCamera != nullptr, "a conflict that is not a second camera keeps no offender rows");
        Scene ok;
        ok.see(ok.writer(nullptr, 0x100)); ok.see(ok.writer(nullptr, 0x200));
        const auto od = ok.donor(ok.lighting());
        expect(od.role == FlatDeferredDonorRole::Witness && od.offenderCamera == nullptr && od.entryCamera == od.camera,
               "an agreeing entry is still a witness donor and has no offender");
        flatRuntimeWritten(*s.p, s.gbuf);
        expect(!s.entry(s.gbuf)->offenderHasCamera && s.donor(s.lighting()).offenderCamera == nullptr, "a colour write restarts the entry and forgets the offender");
        Scene late;   // the first conflict is the one kept: a later, different camera does not replace the offender
        late.see(late.writer(nullptr, 0x100));
        auto a = late.writer(nullptr, 0x200); a.camera[80] ^= 1; a.key.cameraHash = flatCameraHash(a.camera); late.see(a);
        auto c = late.writer(nullptr, 0x300); c.camera[84] ^= 1; c.key.cameraHash = flatCameraHash(c.camera); late.see(c);
        expect(late.entry(late.gbuf)->offender.vs == 0x200 && std::memcmp(late.entry(late.gbuf)->offenderCamera, a.camera, kFlatCameraBytes) == 0,
               "the first offender's rows are the ones kept");
    }
    // ---- the report agrees with the matcher it explains -------------------------------------------------------------------------------
    unsigned used = 0;
    for (const auto& cap : deferred_forward_omission::kCaptures) {
        unsigned char donor[96], original[112], repaired[112];
        float px, py; std::memcpy(&px, &cap.phaseX, 4); std::memcpy(&py, &cap.phaseY, 4);
        FlatProjectionJitter jitter{};
        if (!(deferred_forward_omission::hexBytes(cap.donor, donor, 96) && deferred_forward_omission::hexBytes(cap.original, original, 112) &&
              flatProjectionJitter(px, py, cap.width, cap.height, jitter))) { expect(false, "capture decodes"); continue; }
        if (!flatRepairDeferredWholeOmission(donor, original, jitter, repaired)) continue;
        ++used;
        unsigned char moved[96]; std::memcpy(moved, donor, 96); reinterpret_cast<float*>(moved)[0] += .01f;
        // the first camera is the one the repaired rows belong to: it matches under the repair's rematch, the moved one does not
        auto r = flatExplainSecondCamera(donor, moved, repaired);
        expect(r.first == flatMatchDeferredCamera(donor, repaired) && r.firstScaled == flatMatchDeferredCamera(donor, repaired, true) &&
               r.offender == flatMatchDeferredCamera(moved, repaired) && r.offenderScaled == flatMatchDeferredCamera(moved, repaired, true),
               "the four answers are the matcher's own");
        expect(r.firstScaled == FlatDeferredCameraMatch::Match && r.offenderScaled != FlatDeferredCameraMatch::Match,
               "the repaired rows match the camera they were built from and not the moved one");
        expect(r.compared && r.differing == 1 && r.firstDiff == 0 && r.maxAbs > .009 && r.maxAbs < .011 && r.maxRel > 0,
               "one moved float is one differing float of about the moved size");
        // the other way round: the offender is the camera the pass carries
        r = flatExplainSecondCamera(moved, donor, repaired);
        expect(r.offenderScaled == FlatDeferredCameraMatch::Match && r.firstScaled != FlatDeferredCameraMatch::Match,
               "swapping the writers swaps the answers");
        // the same camera twice, and one ulp apart (the landed-phase rounding the earlier fix covers)
        r = flatExplainSecondCamera(donor, donor, repaired);
        expect(r.compared && r.differing == 0 && r.maxAbs == 0 && r.maxRel == 0 && r.first == r.offender && r.firstScaled == r.offenderScaled,
               "equal cameras differ in nothing and answer alike");
        unsigned char ulp[96]; std::memcpy(ulp, donor, 96);
        uint32_t bits; std::memcpy(&bits, ulp + 4 * 5, 4); ++bits; std::memcpy(ulp + 4 * 5, &bits, 4);
        r = flatExplainSecondCamera(donor, ulp, repaired);
        expect(r.differing == 1 && r.firstDiff == 5 && r.maxRel > 0 && r.maxRel < 3e-7, "a one-ulp difference is one float of relative size 1e-7");
        // absent inputs
        r = flatExplainSecondCamera(donor, nullptr, repaired);
        expect(!r.compared && r.differing == 0 && r.offender == FlatDeferredCameraMatch::Unavailable && std::strcmp(r.offenderWhy.check, "no-bytes") == 0 &&
               r.firstScaled == FlatDeferredCameraMatch::Match, "no offender rows: not compared, the first still answers");
        r = flatExplainSecondCamera(donor, moved, nullptr);
        expect(r.compared && r.first == FlatDeferredCameraMatch::Unavailable && r.offender == FlatDeferredCameraMatch::Unavailable &&
               r.firstScaled == FlatDeferredCameraMatch::Unavailable && std::strcmp(r.firstWhy.check, "no-bytes") == 0, "no CB2: both answers unavailable, cameras still compared");
        r = flatExplainSecondCamera(nullptr, nullptr, nullptr);
        expect(!r.compared && r.first == FlatDeferredCameraMatch::Unavailable && r.offender == FlatDeferredCameraMatch::Unavailable, "nothing at all reports nothing");
        // a non-finite float is a difference with no size, and never a NaN in the report
        unsigned char nan[96]; std::memcpy(nan, donor, 96);
        const uint32_t nanBits = 0x7FC00000u; std::memcpy(nan + 8, &nanBits, 4);
        r = flatExplainSecondCamera(donor, nan, repaired);
        expect(r.differing == 1 && std::isfinite(r.maxAbs) && std::isfinite(r.maxRel) && r.offender == FlatDeferredCameraMatch::Unavailable,
               "a non-finite camera is counted and reported without a NaN");
    }
    expect(used >= 20, "the captures the repair accepts were exercised");
    // ---- wiring: where it runs, and that it decides nothing ---------------------------------------------------------------------------
    std::ifstream in("src/d3d11/flat_runtime.cpp"); std::string source, line;
    while (std::getline(in, line)) source += line + '\n';
    const auto witnessLine = source.find("Log::get().note(\"flat deferred witness: frame=");
    const auto call = source.find("noteSecondCamera(s,requestedFrame,requestedQ,k.width,k.height,donor,copied?raw:nullptr,");
    const auto guard = source.rfind("if(!donor.poolRecords && donor.entry && donor.conflict==FlatWitnessConflict::Camera)", call);
    const auto draws = source.find("draw.deferredHdrVerified=firstWitness && executionCoherent;");
    expect(witnessLine != std::string::npos && call != std::string::npos && guard != std::string::npos && draws != std::string::npos &&
           draws < witnessLine && witnessLine < guard && guard < call,
           "the second-camera line runs after the verdict, beside the witness line, only for a poolless second-camera conflict");
    const auto after = source.substr(call, source.find(';', call) - call);   // the statement alone
    expect(after.find("draw.") == std::string::npos && after.find("fail") == std::string::npos && after.find("flatRuntimeMarkUncertain") == std::string::npos,
           "the call sits in no branch that decides");
    const auto body = deferred_witness_test::between(source, "static void noteSecondCamera(", "const FlatProjectionBindingPlan* verifyDeferredHdrCamera(");
    expect(!body.empty() && body.find("draw") == std::string::npos && body.find("failPhase") == std::string::npos && body.find("flatRuntimeMarkUncertain") == std::string::npos &&
           body.find("s.prefix") == std::string::npos && body.find("deferredSecondCameras") != std::string::npos,
           "the logger touches no draw, no phase and no prefix");
    expect(deferred_witness_test::countOf(source, "flat deferred second-camera: frame=") == 1 && deferred_witness_test::countOf(source, "flat deferred second-camera check: frame=") == 1 &&
           deferred_witness_test::countOf(source, "flat deferred second-camera bytes: frame=") == 1, "each of the three lines is written in one place");
    std::ifstream header("src/d3d11/flat_deferred_camera.h"); std::string camera;
    while (std::getline(header, line)) camera += line + '\n';
    expect(camera.find("SecondCamera") == std::string::npos, "the matcher header carries no diagnostic");
    std::printf("flat deferred second camera: %u failure(s)\n", failures);
    return failures;
}
}
