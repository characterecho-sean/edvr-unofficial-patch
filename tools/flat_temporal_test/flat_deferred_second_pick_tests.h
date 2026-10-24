#pragma once
// Two cameras drew into the deferred lighting pass's G-buffer and no source record stands on the depth (the witness's conflict=second-camera).
// The pass's CB2 belongs to one of them. The resolver cannot say which (it never sees CB2), so the verifier (flat_runtime.cpp, verifyDeferredHdrCamera)
// asks of each camera whether its relation to CB2 holds -- it matches, or one of the two certified phase omissions repairs it -- and when exactly one
// does, names it back to the resolver (`select`), which hands it out as the witness donor if the entry is clean. These tests judge the resolver's
// entry (writers counted by camera, the clean flag, what stops a selection), the real triples the Steam rig logged (edvr_gfx_20261024_131540), the
// camera-phase estimate that instrument prints, and the wiring in the verifier.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include "flat_deferred_witness_tests.h"
#include "../../src/d3d11/flat_deferred_reject.h"
#include "flat_deferred_forward_omission_captures.h"
#include "flat_deferred_second_pair_captures.h"
namespace deferred_second_pick {
using namespace edvr;
using deferred_witness_test::Scene;
using laser_scene_tone_test::token;

inline bool holds(const unsigned char* camera, const unsigned char* cb2, const FlatProjectionJitter& jitter, bool jitterReady) {
    unsigned char scratch[112];
    return flatMatchDeferredCamera(camera, cb2) == FlatDeferredCameraMatch::Match ||
        (jitterReady && (flatRepairDeferredMissingPhase(camera, cb2, jitter, scratch) || flatRepairDeferredWholeOmission(camera, cb2, jitter, scratch)));
}
inline bool hex(const char* text, unsigned char* out, size_t bytes) { return deferred_forward_omission::hexBytes(text, out, bytes); }

inline unsigned run() {
    unsigned failures = 0;
    const auto expect = [&](bool ok, const char* name) { if (!ok) { ++failures; std::printf("FAIL: deferred second pick %s\n", name); } };
    using Why = FlatDeferredDonorWhy;
    using Role = FlatDeferredDonorRole;

    // ---- the real triples ---------------------------------------------------------------------------------------------------------------
    unsigned decoded = 0, zeroPhase = 0;
    for (const auto& cap : deferred_second_pair::kCaptures) {
        unsigned char first[96], offender[96], cb2[112];
        float px, py; std::memcpy(&px, &cap.phaseX, 4); std::memcpy(&py, &cap.phaseY, 4);
        FlatProjectionJitter jitter{};
        const bool ready = (px != 0 || py != 0) && flatProjectionJitter(px, py, cap.width, cap.height, jitter);
        if (!(hex(cap.first, first, 96) && hex(cap.offender, offender, 96) && hex(cap.cb2, cb2, 112))) { expect(false, "capture decodes"); continue; }
        ++decoded; if (!ready) ++zeroPhase;
        char name[160];
        std::snprintf(name, sizeof(name), "frame %u: the first writer's camera does not hold against CB2", cap.frame);
        expect(!holds(first, cb2, jitter, ready), name);
        std::snprintf(name, sizeof(name), "frame %u: the offender's camera holds against CB2", cap.frame);
        expect(holds(offender, cb2, jitter, ready), name);
        if (!ready) {
            std::snprintf(name, sizeof(name), "frame %u: at zero phase the offender matches outright", cap.frame);
            expect(flatMatchDeferredCamera(offender, cb2) == FlatDeferredCameraMatch::Match, name);
        } else {
            unsigned char corrected[112];
            std::snprintf(name, sizeof(name), "frame %u: at a nonzero phase CB2 lacks only the rays' phase, the missing-phase repair", cap.frame);
            expect(flatMatchDeferredCamera(offender, cb2) == FlatDeferredCameraMatch::Inverse && flatRepairDeferredMissingPhase(offender, cb2, jitter, corrected), name);
            const auto report = flatExplainSecondCamera(first, offender, cb2);
            expect(report.compared && report.differing == 7 && report.maxRel > .5, "the two cameras differ in seven floats by more than half");
            const auto fp = flatEstimateCameraPhase(first), op = flatEstimateCameraPhase(offender);
            std::printf("  second-pair frame %u: requested ndc=(%.6g,%.6g) first=(%.6g,%.6g) scale=(%.6g,%.6g) offender=(%.6g,%.6g) scale=(%.6g,%.6g)\n",
                        cap.frame, jitter.ndcX, jitter.ndcY, fp.ndcX, fp.ndcY, fp.scaleX, fp.scaleY, op.ndcX, op.ndcY, op.scaleX, op.scaleY);
            std::snprintf(name, sizeof(name), "frame %u: the offender's camera carries the requested phase", cap.frame);
            expect(op.valid && std::abs(op.ndcX - jitter.ndcX) < 2e-6 && std::abs(op.ndcY - jitter.ndcY) < 2e-6, name);
        }
    }
    expect(decoded == 8 && zeroPhase == 3, "all eight Steam captures decode, three at zero phase");
    // ---- the camera-phase estimate -------------------------------------------------------------------------------------------------------
    unsigned estimated = 0;
    for (const auto& cap : deferred_forward_omission::kCaptures) {   // the donor camera always carries the requested phase
        unsigned char donor[96], original[112];
        float px, py; std::memcpy(&px, &cap.phaseX, 4); std::memcpy(&py, &cap.phaseY, 4);
        FlatProjectionJitter jitter{};
        if (!(deferred_forward_omission::hexBytes(cap.donor, donor, 96) && deferred_forward_omission::hexBytes(cap.original, original, 112) &&
              flatProjectionJitter(px, py, cap.width, cap.height, jitter))) continue;
        const auto p = flatEstimateCameraPhase(donor);
        ++estimated;
        if (!(p.valid && std::abs(p.ndcX - jitter.ndcX) < 2e-6 && std::abs(p.ndcY - jitter.ndcY) < 2e-6 && p.scaleX > 0 && p.scaleY > 0 && p.nearPlane > 0))
            expect(false, "a donor camera's estimated phase is the requested ndc phase");
    }
    expect(estimated >= 20, "the phase estimate was exercised on the captures");
    expect(!flatEstimateCameraPhase(nullptr).valid, "no bytes, no phase");
    {
        unsigned char zero[96] = {};
        expect(!flatEstimateCameraPhase(zero).valid, "a zero w row has no phase");
        float cam[24] = {}; cam[0] = 1.5f; cam[2 * 4 + 0] = .003f; cam[1 * 4 + 1] = 2.f; cam[2 * 4 + 1] = -.002f; cam[2 * 4 + 3] = 1.f; cam[3 * 4 + 2] = .025f;
        unsigned char bytes[96]; std::memcpy(bytes, cam, 96);
        const auto p = flatEstimateCameraPhase(bytes);
        expect(p.valid && std::abs(p.ndcX - .003) < 1e-9 && std::abs(p.ndcY + .002) < 1e-9 && std::abs(p.scaleX - 1.5) < 1e-6 && std::abs(p.scaleY - 2) < 1e-6 && p.nearPlane == .025f,
               "a built camera reads back its phase, scale and near");
    }

    // ---- the entry and the resolver ------------------------------------------------------------------------------------------------------
    {
        Scene s;
        const auto a = s.writer(nullptr, 0x100);
        s.see(a);
        auto b = s.writer(nullptr, 0x200, .0675f); s.see(b);                 // the second camera: a different near plane
        const auto lighting = s.lighting();
        const auto* e = s.entry(s.gbuf);
        const auto plain = s.donor(lighting);
        expect(!plain && plain.why == Why::Conflict && plain.conflict == FlatWitnessConflict::Camera && plain.pairSelectable && plain.pairClean &&
               plain.firstWriters == 1 && plain.offenderWriters == 1, "a clean two-camera entry is selectable and, unnamed, gives no donor");
        const auto named = [&](const unsigned char* camera) { return flatRuntimeDeferredDonor(*s.p, lighting.key, s.p->sequence + 1, nullptr, camera); };
        const auto first = named(a.camera);
        expect(first && first.role == Role::Witness && first.selected && first.why == Why::None && first.camera == e->camera &&
               first.cameraHash == a.key.cameraHash && first.b1 == a.key.b1 && first.writeSeq == a.key.writeSeq && first.writeEpoch == a.key.writeEpoch &&
               first.color == s.gbuf && first.last == e->last, "naming the first camera hands out the first writer's rows and provenance");
        const auto off = named(b.camera);
        expect(off && off.role == Role::Witness && off.selected && off.camera == e->offenderCamera && off.cameraHash == b.key.cameraHash &&
               off.b1 == b.key.b1 && off.writeSeq == b.key.writeSeq && off.lastWriteSeq == b.key.writeSeq && off.first == e->offender.q &&
               off.last == e->last, "naming the offender's camera hands out the offender's rows and provenance");
        unsigned char stranger[96]; std::memcpy(stranger, a.camera, 96); stranger[84] ^= 1;
        expect(!named(stranger) && !named(stranger).selected, "bytes that name neither camera select nothing");
        unsigned char zero[96] = {};
        expect(!named(zero), "an unverified draw's empty camera selects nothing");
        expect(!flatRuntimeDeferredDonor(*s.p, lighting.key, s.p->sequence + 1), "no name, no donor, as before");
        // the qualification asks the same way, with the draw's own camera
        Scene q;
        q.see(q.writer(nullptr, 0x100)); auto qb = q.writer(nullptr, 0x200, .0675f); q.see(qb);
        auto verified = q.lighting(); std::memcpy(verified.camera, qb.camera, 96); verified.key.cameraHash = qb.key.cameraHash;
        const auto qd = flatRuntimeDeferredDonor(*q.p, verified.key, q.p->sequence + 1, nullptr, verified.camera);
        expect(qd && qd.selected && qd.cameraHash == verified.key.cameraHash && std::memcmp(verified.camera, qd.camera, 96) == 0,
               "the qualification's donor is the camera the verified draw carries");
    }
    {
        Scene s;   // writers interleaved by camera
        const auto a = s.writer(nullptr, 0x100); const auto b = s.writer(nullptr, 0x200, .0675f);
        s.see(a); s.see(b); s.see(s.writer(nullptr, 0x101)); s.see(s.writer(nullptr, 0x201, .0675f)); s.see(s.writer(nullptr, 0x102));
        const auto d = s.donor(s.lighting());
        expect(d.pairClean && d.pairSelectable && d.firstWriters == 3 && d.offenderWriters == 2 && s.entry(s.gbuf)->writers == 5,
               "interleaved writers are counted by camera");
        expect(s.entry(s.gbuf)->lastWriteSeq == 5 && s.entry(s.gbuf)->offenderLastWriteSeq == 4,
               "each camera's last write is the last writer that bound it");
        const auto picked = flatRuntimeDeferredDonor(*s.p, s.lighting().key, s.p->sequence + 1, nullptr, b.camera);
        expect(picked && picked.lastWriteSeq == 4 && picked.last == 5, "the offender donor's last write is its own, the last writer the entry's");
    }
    {
        Scene s;   // a third camera ends the pair
        s.see(s.writer(nullptr, 0x100)); s.see(s.writer(nullptr, 0x200, .0675f)); s.see(s.writer(nullptr, 0x300, .09f));
        const auto d = s.donor(s.lighting());
        expect(!d.pairClean && !d.pairSelectable && d.firstWriters == 1 && d.offenderWriters == 1, "a third camera is no pair");
        expect(!flatRuntimeDeferredDonor(*s.p, s.lighting().key, s.p->sequence + 1, nullptr, s.writer(nullptr, 0x100).camera),
               "and selects nothing, whichever camera is named");
        Scene bad;   // a later writer with a stale camera write
        bad.see(bad.writer(nullptr, 0x100)); bad.see(bad.writer(nullptr, 0x200, .0675f));
        auto stale = bad.writer(nullptr, 0x101); stale.key.writeEpoch = deferred_witness_test::kFrame - 1; bad.see(stale);
        expect(!bad.donor(bad.lighting()).pairClean, "a later writer whose camera is not current ends the pair");
        Scene lay;   // a later writer in another layout
        lay.see(lay.writer(nullptr, 0x100)); lay.see(lay.writer(nullptr, 0x200, .0675f));
        auto other = lay.writer(nullptr, 0x101); other.key.dsv = token(0x2BAD); lay.see(other);
        expect(!lay.donor(lay.lighting()).pairClean, "a later writer in another layout ends the pair");
        Scene vp;    // a later writer with a partial viewport
        vp.see(vp.writer(nullptr, 0x100)); vp.see(vp.writer(nullptr, 0x200, .0675f));
        auto part = vp.writer(nullptr, 0x101); part.key.viewport[2] = 960; vp.see(part);
        expect(!vp.donor(vp.lighting()).pairClean, "a later writer with a partial viewport ends the pair");
        Scene nc;    // a later writer with no camera
        nc.see(nc.writer(nullptr, 0x100)); nc.see(nc.writer(nullptr, 0x200, .0675f));
        auto none = nc.writer(nullptr, 0x101); none.key.camera = nullptr; none.key.cameraHash = 0; nc.see(none);
        expect(!nc.donor(nc.lighting()).pairClean, "a later writer with no camera ends the pair");
    }
    {
        Scene s;   // a second entry on the depth (another colour) is not a clean pair on its own
        s.see(s.writer(nullptr, 0x100)); s.see(s.writer(nullptr, 0x200, .0675f)); s.see(s.writer(token(0x1100), 0x300));
        const auto d = s.donor(s.lighting());
        expect(!d.pairSelectable && d.witnessEntries == 2, "a depth with another witness entry is not selectable");
        Scene e;   // the same bytes behind another b1: both candidates are the same camera, nothing to choose
        e.see(e.writer(nullptr, 0x100)); auto other = e.writer(nullptr, 0x200); other.key.b1 = token(0x3001); e.see(other);
        const auto a = e.writer(nullptr, 0x100);
        expect(e.donor(e.lighting()).pairSelectable && !flatRuntimeDeferredDonor(*e.p, e.lighting().key, e.p->sequence + 1, nullptr, a.camera),
               "two cameras of equal bytes cannot be told apart by their bytes: no selection");
        Scene late;  // a writer at or after the pass is Late, as for any donor
        late.see(late.writer(nullptr, 0x100)); late.see(late.writer(nullptr, 0x200, .0675f));
        auto h = late.lighting();
        const auto at = flatRuntimeDeferredDonor(*late.p, h.key, 2, nullptr, late.writer(nullptr, 0x100).camera);
        expect(!at && !at.pairSelectable, "a pass that does not come after the entry's last writer selects nothing");
        Scene same;  // the entry's colour is the pass's own target
        same.see(same.writer(nullptr, 0x100)); same.see(same.writer(nullptr, 0x200, .0675f));
        auto self = same.lighting(); self.key.color = same.gbuf;
        expect(!flatRuntimeDeferredDonor(*same.p, self.key, same.p->sequence + 1, nullptr, same.writer(nullptr, 0x100).camera) , "the pass writing its own source selects nothing");
        Scene un;    // an unconflicted entry ignores the name entirely
        un.see(un.writer(nullptr, 0x100)); un.see(un.writer(nullptr, 0x101));
        const auto plain = un.donor(un.lighting());
        const auto named = flatRuntimeDeferredDonor(*un.p, un.lighting().key, un.p->sequence + 1, nullptr, un.writer(nullptr, 0x100).camera);
        expect(plain.role == Role::Witness && !named.selected && named.camera == plain.camera && !named.pairSelectable, "an agreeing entry is unaffected by a name");
        Scene prov;  // a provenance conflict is not a pair
        prov.see(prov.writer(nullptr, 0x100));
        auto bare = prov.writer(nullptr, 0x200); bare.key.camera = nullptr; bare.key.cameraHash = 0; prov.see(bare);
        expect(!prov.donor(prov.lighting()).pairSelectable && !prov.donor(prov.lighting()).pairClean, "a camera-not-current conflict is no pair");
    }
    // ---- wiring ---------------------------------------------------------------------------------------------------------------------------
    std::ifstream in("src/d3d11/flat_runtime.cpp"); std::string source, line;
    while (std::getline(in, line)) source += line + '\n';
    const auto verifier = deferred_witness_test::between(source, "const FlatProjectionBindingPlan* verifyDeferredHdrCamera(", "void reportHdrProjectionCapture(");
    const auto resolved = verifier.find("const auto resolved=flatRuntimeDeferredDonor(s.prefix,k,s.prefix.sequence+1,gbuffer);");
    const auto copiedAt = verifier.find("const bool copied=");
    const auto donorAt = verifier.find("const auto donor=[&]()->FlatDeferredDonor {");
    const auto matchAt = verifier.find("const auto match=donor && copied");
    const auto repairAt = verifier.find("const bool missingPhase=repairable && flatRepairDeferredMissingPhase(");
    expect(resolved != std::string::npos && copiedAt != std::string::npos && donorAt != std::string::npos && matchAt != std::string::npos && repairAt != std::string::npos &&
           resolved < copiedAt && copiedAt < donorAt && donorAt < matchAt && matchAt < repairAt,
           "the donor is chosen after CB2 is copied and before the match and the repair that use it");
    const auto picker = deferred_witness_test::between(verifier, "const auto donor=[&]()->FlatDeferredDonor {", "const auto match=donor && copied");
    expect(!picker.empty() && picker.find("if(!resolved.pairSelectable)return resolved;") != std::string::npos &&
           picker.find("actual && input && copied && !foreignWork") != std::string::npos &&
           picker.find("flatMatchDeferredCamera(camera,raw)==FlatDeferredCameraMatch::Match") != std::string::npos &&
           picker.find("flatRepairDeferredMissingPhase(camera,raw,jitter,scratch)") != std::string::npos &&
           picker.find("flatRepairDeferredWholeOmission(camera,raw,jitter,scratch)") != std::string::npos &&
           picker.find("if(holdsFirst==holdsOffender)return resolved;") != std::string::npos &&
           picker.find("holdsFirst?resolved.entryCamera:resolved.offenderCamera") != std::string::npos,
           "the verifier selects only a qualified pass, only when exactly one camera holds, by the relations the repairs certify");
    expect(deferred_witness_test::countOf(picker, "flatRuntimeDeferredDonor(") == 1 && picker.find("certified") == std::string::npos && picker.find("draw.") == std::string::npos,
           "the choice touches no certified upload and no draw: the repair runs afterwards, on the donor");
    expect(deferred_witness_test::countOf(source, "flatRuntimeDeferredDonor(s.prefix,k,s.prefix.sequence+1,nullptr,d.camera)") == 1, "the binding failure report names the draw's camera");
    std::ifstream modelIn("src/d3d11/flat_runtime_model.h"); std::string model;
    while (std::getline(modelIn, line)) model += line + '\n';
    expect(deferred_witness_test::countOf(model, "flatRuntimeDeferredDonor(p,d.key,q,nullptr,d.camera)") == 1 &&
           deferred_witness_test::countOf(model, "flatRuntimeDeferredDonor(p,k,q,nullptr,d.camera)") == 1,
           "both qualification calls name the verified draw's camera");
    expect(deferred_witness_test::countOf(source, "flat deferred second-camera phase: frame=") == 1, "the phase line is written in one place");
    std::printf("flat deferred second pick: %u failure(s)\n", failures);
    return failures;
}
}
