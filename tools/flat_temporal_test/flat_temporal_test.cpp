#include <memory>
#include <algorithm>
#include <utility>
#include "../../src/d3d11/flat_temporal_model.h"
#include "../../src/d3d11/flat_mono_frame.h"
#include "../../src/d3d11/flat_runtime_model.h"
#include "../../src/d3d11/engine_velocity_families.h"
#include "flat_shader_capture_tests.h"
#include "flat_projection_math_tests.h"
#include "flat_projection_bindings_tests.h"
#include "flat_projection_recipe_tests.h"
#include "flat_projection_viewport_tests.h"
#include "flat_projection_ownership_tests.h"
#include "flat_compute_tests.h"
#include "flat_lighting_tests.h"
#include "flat_live_phase_tests.h"
#include "flat_pixel_capture_tests.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <limits>

namespace {
struct Pair {
    void* color = nullptr;
    void* depth = nullptr;
    unsigned draws = 0;
};
struct Route {
    const void* src = nullptr;
    const void* dst = nullptr;
    char kind = 0;
    unsigned count = 0, first = 0, last = 0;
};
int failures = 0;
void check(bool condition, const char* what) {
    if (!condition) { std::printf("FAIL: %s\n", what); ++failures; }
}
void testProjectionSlices() {
    using namespace edvr;
    using Status = FlatProjectionStatus;
    unsigned char prefix[4096]{};
    for (unsigned i = 0; i < sizeof(prefix); ++i) prefix[i] = static_cast<unsigned char>(i);
    FlatProjectionBinding vs{}, ps{};
    const void* buffer = reinterpret_cast<void*>(0xB200);
    auto observe = [&](const FlatProjectionBinding& binding, uint32_t slot, uint32_t width,
                       uint32_t bytes, uint64_t epoch = 11, uint32_t seq = 20,
                       bool tracked = true) {
        const uint32_t offset = flatProjectionOffset(slot);
        const uint32_t copied = bytes > offset ? std::min(bytes - offset, flatProjectionCapacity(slot)) : 0;
        return flatObserveProjection(binding, tracked, width, prefix, bytes,
            epoch, seq, 11, 30, slot, flatProjectionHash(prefix + offset, copied));
    };
    check(observe(vs, 1, 272, 272).status == Status::BindingUnknown,
          "unseen setter is unknown even when CPU bytes exist");
    check(!flatProjectionBind(vs, 2, 3, 1, buffer) && !vs.observed,
          "setter range cannot invent a b2 binding");
    check(flatProjectionBind(vs, 2, 0, 3, buffer) && vs.observed && vs.resource == buffer && !ps.observed,
          "VS b2 observation does not establish PS b2 ownership");
    flatProjectionBind(ps, 2, 2, 1, nullptr);
    check(observe(ps, 2, 272, 272).status == Status::Unbound,
          "explicit PS null binding differs from unknown");
    check(observe(vs, 1, 272, 272, 11, 20, false).status == Status::MissingWrite,
          "bound resource without CPU write is missing");
    check(observe(vs, 1, 272, 0).status == Status::InvalidWrite &&
          observe(vs, 1, 272, 273).status == Status::InvalidWrite,
          "invalidated or impossible CPU prefix is not available");
    check(observe(vs, 1, 272, 272, 10).status == Status::OldFrame &&
          observe(vs, 1, 272, 272, 11, 31).status == Status::LaterWrite,
          "old-frame and post-draw writes are distinguished");
    auto b0 = observe(vs, 0, 128, 128);
    check(b0.status == Status::Available && b0.copied == 64 && b0.bytes == prefix + 64,
          "b0 rows4..7 require exactly 128 bytes and skip rows0..3");
    check(observe(vs, 0, 127, 127).copied == 63 && observe(vs, 0, 127, 127).status == Status::ShortRange &&
          observe(vs, 0, 80, 80).copied == 16 && observe(vs, 0, 64, 64).copied == 0,
          "short b0 widths expose only available requested bytes");
    check(observe(vs, 1, 272, 272).status == Status::Available &&
          observe(vs, 2, 271, 271).status == Status::ShortRange &&
          observe(vs, 1, 65536, 4096).copied == 272,
          "VS and PS b2 stop at row16 and respect exact 272-byte boundary");

    FlatContractRecord records[12]{};
    uint32_t used = 0, dropped = 0;
    unsigned char camera[kFlatCameraBytes]{};
    FlatContractObservation draw{};
    draw.kind = kFlatContractScreen; draw.camera = camera; draw.cameraHash = flatCameraHash(camera);
    draw.b1 = reinterpret_cast<void*>(0xB100); draw.sequence = 30;
    draw.projection[1] = observe(vs, 1, 272, 272);
    auto* first = flatRecordContract(records, used, dropped, draw);
    const unsigned char old = prefix[0]; prefix[0] ^= 1;
    // Force equal hashes: exact bytes must distinguish reuse and NaN payloads.
    auto* changed = flatRecordContract(records, used, dropped, draw);
    check(first != changed && used == 2 && first->vsB2[0] == old && changed->vsB2[0] == prefix[0],
          "same camera and reused b2 resource freeze different bytes despite hash collision");
    draw.projection[2] = draw.projection[1];
    auto* psBound = flatRecordContract(records, used, dropped, draw);
    check(psBound != changed, "independent PS b2 binding is part of the contract key");
    draw.projection[2].resource = reinterpret_cast<void*>(0xB201);
    check(flatRecordContract(records, used, dropped, draw) != psBound,
          "identical bytes with distinct PS buffer identities remain distinct");
    draw.projection[2] = {}; draw.projection[1].writeSeq = 24; draw.sequence = 35;
    check(flatRecordContract(records, used, dropped, draw) == changed && changed->firstProjectionSeq[1] == 20 &&
          changed->lastProjectionSeq[1] == 24 && changed->first == 30 && changed->last == 35,
          "equal frozen bytes aggregate first and last write/draw provenance");
    uint32_t nanBits = 0x7FC00001u; std::memcpy(prefix, &nanBits, sizeof(nanBits));
    auto* nanOne = flatRecordContract(records, used, dropped, draw);
    nanBits = 0x7FC00002u; std::memcpy(prefix, &nanBits, sizeof(nanBits));
    auto* nanTwo = flatRecordContract(records, used, dropped, draw);
    uint32_t frozenBits = 0; std::memcpy(&frozenBits, nanOne->vsB2, sizeof(frozenBits));
    check(nanOne != nanTwo && frozenBits == 0x7FC00001u,
          "different NaN payload bit patterns survive freezing and never coalesce");
    draw.projection[1] = observe(vs, 1, 272, 272, 10);
    auto* stale = flatRecordContract(records, used, dropped, draw);
    draw.projection[1] = observe(vs, 1, 272, 272, 11, 31);
    auto* later = flatRecordContract(records, used, dropped, draw);
    draw.projection[1] = observe(vs, 1, 272, 272, 11, 20, false);
    auto* missing = flatRecordContract(records, used, dropped, draw);
    check(stale != later && later != missing && stale->key.projection[1].copied == 0 &&
          later->key.projection[1].copied == 0, "unavailable write causes stay separate without stale payload");
    draw.projection[1].copied = 273; draw.projection[1].bytes = prefix;
    const uint32_t before = used;
    check(!flatRecordContract(records, used, dropped, draw) && dropped == 1 && used == before,
          "oversized projection observation is refused before copying");
    check(flatContractKind(false, buffer, buffer, 960, 540, 60, 1280, 720, false) == kFlatContractScreen &&
          flatContractKind(false, buffer, buffer, 320, 180, 60, 1280, 720, false) == kFlatContractNone,
          "screen-sized format60 inverse pass captured without admitting small auxiliary target");
    FlatContractRecord inverse[2]{}; used = dropped = 0;
    draw = {}; draw.kind = flatContractKind(false, buffer, buffer, 960, 540, 60, 1280, 720, false);
    draw.color = draw.depth = buffer; draw.width = 960; draw.height = 540; draw.format = 60;
    draw.vs = 0x53211E8C072CD02Eull; draw.ps = 0x7EAC71963E66C5FEull;
    draw.sequence = 322; draw.projection[2] = observe(vs, 2, 272, 272);
    auto* inverseFirst = flatRecordContract(inverse, used, dropped, draw);
    draw.sequence = 323; draw.ps ^= 1;
    auto* inverseSecond = flatRecordContract(inverse, used, dropped, draw);
    check(inverseFirst && inverseSecond && inverseFirst != inverseSecond && inverseFirst->key.format == 60 &&
          inverseFirst->key.projection[2].status == Status::Available && inverseFirst->first == 322 &&
          inverseSecond->first == 323 && used == 2 && dropped == 0,
          "format60's two draw-time shader pairs and PS-owned inverse constants remain distinguishable");
}
void testDetailBudget() {
    using namespace edvr;
    uint32_t remaining = 2; bool refusal = false;
    check(flatTakeDetailSample(false, 10, 70, true, remaining, refusal) == FlatDetailAdmission::Startup && remaining == 2,
          "startup never emits full capture details");
    check(flatTakeDetailSample(true, 0, 70, true, remaining, refusal) == FlatDetailAdmission::Empty &&
          flatTakeDetailSample(true, 10, 0, false, remaining, refusal) == FlatDetailAdmission::Empty && remaining == 2,
          "rearm and empty presents cannot spend a detail sample");
    check(flatTakeDetailSample(true, 10, 70, false, remaining, refusal) == FlatDetailAdmission::Refusal && remaining == 1,
          "first useful manual refusal retains diagnostic contracts");
    check(flatTakeDetailSample(true, 11, 70, false, remaining, refusal) == FlatDetailAdmission::RefusalAlreadyReported && remaining == 1,
          "repeated refusal preserves remaining selected sample");
    check(flatTakeDetailSample(true, 12, 70, true, remaining, refusal) == FlatDetailAdmission::Selected && remaining == 0 &&
          flatTakeDetailSample(true, 13, 70, true, remaining, refusal) == FlatDetailAdmission::Exhausted,
          "refusal plus selected frame cannot exceed two whole detailed reports");
    remaining = 2; refusal = false;
    check(flatTakeDetailSample(true, 20, 70, true, remaining, refusal) == FlatDetailAdmission::Selected &&
          flatTakeDetailSample(true, 21, 70, true, remaining, refusal) == FlatDetailAdmission::Selected &&
          flatTakeDetailSample(true, 22, 70, false, remaining, refusal) == FlatDetailAdmission::Exhausted && remaining == 0,
          "two selected reports exhaust budget before any later refusal");
}
void testAssociationAndBounds() {
    Pair pairs[2]{};
    uint32_t used = 0, overflow = 0;
    void* c = reinterpret_cast<void*>(0x1000);
    void* d1 = reinterpret_cast<void*>(0x2000);
    void* d2 = reinterpret_cast<void*>(0x3000);
    Pair* a = edvr::flatFindOrAdd(pairs, used, overflow,
        [=](const Pair& p) { return p.color == c && p.depth == d1; });
    check(a != nullptr, "first pair admitted");
    a->color = c; a->depth = d1; a->draws = 7;
    Pair* same = edvr::flatFindOrAdd(pairs, used, overflow,
        [=](const Pair& p) { return p.color == c && p.depth == d1; });
    check(same == a && same->draws == 7 && used == 1,
          "same color and depth associate without duplicate");
    Pair* other = edvr::flatFindOrAdd(pairs, used, overflow,
        [=](const Pair& p) { return p.color == c && p.depth == d2; });
    check(other != nullptr && other != a, "same color with different depth stays separate");
    other->color = c; other->depth = d2;
    Pair* refused = edvr::flatFindOrAdd(pairs, used, overflow,
        [](const Pair&) { return false; });
    check(refused == nullptr && used == 2 && overflow == 1,
          "full table refuses without overwriting evidence");
    check(a->draws == 7 && other->depth == d2, "overflow leaves existing entries intact");

    Route routes[2]{};
    used = overflow = 0;
    Route* r = edvr::flatFindOrAdd(routes, used, overflow,
        [=](const Route& e) { return e.src == c && e.dst == d1 && e.kind == 'S'; });
    r->src = c; r->dst = d1; r->kind = 'S'; r->count = 1;
    Route* repeat = edvr::flatFindOrAdd(routes, used, overflow,
        [=](const Route& e) { return e.src == c && e.dst == d1 && e.kind == 'S'; });
    check(repeat == r && used == 1, "bound-SRV route coalesces");
    Route* copy = edvr::flatFindOrAdd(routes, used, overflow,
        [=](const Route& e) { return e.src == c && e.dst == d1 && e.kind == 'R'; });
    check(copy != nullptr && copy != r, "copy and sampled routes are distinct evidence");
}
void testFrozenEvidence() {
    void* color = reinterpret_cast<void*>(0x1000);
    void* depth = reinterpret_cast<void*>(0x2000);
    check(!edvr::flatSceneCandidateEligible(nullptr, depth, 80),
          "depth-only high-draw target excluded from scene candidate");
    check(!edvr::flatSceneCandidateEligible(color, nullptr, 80),
          "color without depth excluded from scene candidate");
    check(edvr::flatSceneCandidateEligible(color, depth, 2),
          "color and depth draw remains candidate, not certificate");

    unsigned char bytes[4] = {1, 2, 3, 4};
    edvr::FlatCbExemplar a{}, b{}, b1{};
    const void* buffer = reinterpret_cast<void*>(0x3000);
    check(edvr::flatFreezeCbExemplar(a, buffer, bytes, 4096, 4,
                                     11, 101, 11, 105, 0xA1),
          "target A freezes same-frame write at its draw");
    bytes[0] = 9;
    check(edvr::flatFreezeCbExemplar(b, buffer, bytes, 4096, 4,
                                     11, 594, 11, 601, 0xB2),
          "same buffer can have distinct cross-pass exemplar");
    check(a.bytes[0] == 1 && a.writeSeq == 101 && a.drawSeq == 105 &&
          a.shader == 0xA1 && b.bytes[0] == 9 && b.shader == 0xB2,
          "later buffer reuse cannot replace earlier target bytes or shader");
    check(!edvr::flatFreezeCbExemplar(a, buffer, bytes, 4096, 4,
                                      11, 594, 11, 601, 0xB2) && a.bytes[0] == 1,
          "a second draw cannot overwrite frozen target exemplar");
    check(!edvr::flatFreezeCbExemplar(b1, buffer, bytes, 4096, 4,
                                      10, 80, 11, 105, 0xA1),
          "prior-frame shadow is not draw-frozen evidence");
    check(!edvr::flatFreezeCbExemplar(b1, buffer, bytes, 4096, 4,
                                      11, 106, 11, 105, 0xA1),
          "later write cannot be associated with earlier draw");
    check(!edvr::flatFreezeCbExemplar(b1, buffer, bytes, 4096, 4,
                                      0, 0, 11, 105, 0xA1),
          "invalidated unsupported write cannot supply old shadow bytes");
    check(edvr::flatFreezeCbExemplar(b1, buffer, bytes, 4096, 4,
                                     11, 104, 11, 105, 0xA1),
          "independent b1 slot freezes a valid draw write");
}
void testOutputEdgeReservation() {
    Route all[2]{}, output[2]{};
    uint32_t used = 0, overflow = 0, outputUsed = 0, outputOverflow = 0;
    const void* backbuffer = reinterpret_cast<void*>(0x9000);
    const void* mid = reinterpret_cast<void*>(0x8000);
    edvr::flatRecordEdge(all, used, overflow, output, outputUsed, outputOverflow,
                         reinterpret_cast<void*>(0x1000), mid, 'S', backbuffer, 1);
    edvr::flatRecordEdge(all, used, overflow, output, outputUsed, outputOverflow,
                         reinterpret_cast<void*>(0x2000), mid, 'S', backbuffer, 2);
    edvr::flatRecordEdge(all, used, overflow, output, outputUsed, outputOverflow,
                         reinterpret_cast<void*>(0x3000), backbuffer, 'R', backbuffer, 3);
    check(used == 2 && overflow == 1 && outputUsed == 1 && outputOverflow == 0,
          "full general edge table still retains output route");
    check(output[0].src == reinterpret_cast<void*>(0x3000) &&
          output[0].dst == backbuffer && output[0].first == 3,
          "reserved output edge has exact route and order");
    edvr::flatRecordEdge(all, used, overflow, output, outputUsed, outputOverflow,
                         reinterpret_cast<void*>(0x3000), backbuffer, 'R', backbuffer, 4);
    check(output[0].count == 2 && output[0].last == 4,
          "output route coalesces despite general saturation");
}
void testSparseCameraRows() {
    using namespace edvr;
    unsigned char source[kFlatCameraOffset + kFlatCameraBytes] = {};
    for (uint32_t i = 0; i < kFlatCameraBytes; ++i)
        source[kFlatCameraOffset + i] = static_cast<unsigned char>(i + 1);
    unsigned char rows[kFlatCameraBytes] = {};
    check(kFlatCameraOffset == 4320 && sizeof(source) == 4416,
          "camera registers 270..275 have exact 4320..4415 byte range");
    check(!flatCaptureCameraRows(rows, source, 4415) && rows[0] == 0,
          "one byte short cannot produce camera rows");
    check(!flatCaptureCameraRows(rows, nullptr, 4416), "null CPU source rejected");
    check(flatCaptureCameraRows(rows, source, 4416) && rows[0] == 1 && rows[95] == 96,
          "exact 4416-byte complete write captures entire sparse range");
    check(flatCameraAvailability(false, false, 0, 0, 12, 80) == kFlatCameraMissingBuffer,
          "unobserved buffer is distinct from known missing rows");
    check(flatCameraAvailability(true, false, 12, 70, 12, 80) == kFlatCameraInvalidWrite,
          "invalidated/short CPU write cannot supply old rows");
    check(flatCameraAvailability(true, true, 11, 70, 12, 80) == kFlatCameraOldFrame,
          "old-frame rows never become current draw evidence");
    check(flatCameraAvailability(true, true, 12, 81, 12, 80) == kFlatCameraLaterWrite,
          "later write cannot supply earlier draw evidence");
    check(flatCameraAvailability(true, true, 12, 70, 12, 80) == kFlatCameraAvailable,
          "same-frame preceding write can supply sparse rows");
}
void testContractKeysAndReuse() {
    using namespace edvr;
    FlatContractRecord records[32]{};
    uint32_t used = 0, dropped = 0;
    unsigned char rows[kFlatCameraBytes] = {};
    rows[0] = 1;
    FlatContractObservation draw{};
    draw.kind = kFlatContractPool;
    draw.color = reinterpret_cast<void*>(0x1000);
    draw.depth = reinterpret_cast<void*>(0x2000);
    draw.b1 = reinterpret_cast<void*>(0x3000);
    draw.vs = 0x123; draw.ps = 0x456;
    draw.camera = rows; draw.cameraHash = flatCameraHash(rows);
    draw.width = draw.depthWidth = 960; draw.height = draw.depthHeight = 540;
    draw.format = 23; draw.depthFormat = 39;
    draw.viewportCount = 1; draw.viewport[2] = 960; draw.viewport[3] = 540; draw.viewport[5] = 1;
    draw.sequence = 80; draw.count = 128; draw.instances = 1;
    draw.writeEpoch = 12; draw.writeSeq = 70;
    FlatContractRecord* first = flatRecordContract(records, used, dropped, draw);
    check(first && first->key.camera == first->camera && first->camera[0] == 1,
          "new contract owns immutable sparse rows, not CB shadow pointer");
    draw.sequence = 100; draw.count = 64; draw.instances = 2; draw.writeSeq = 95;
    check(flatRecordContract(records, used, dropped, draw) == first && used == 1 &&
          first->draws == 2 && first->first == 80 && first->last == 100 &&
          first->firstCount == 128 && first->lastCount == 64 &&
          first->firstInstances == 1 && first->lastInstances == 2 &&
          first->firstWriteSeq == 70 && first->lastWriteSeq == 95,
          "identical camera key aggregates draw/count/write ranges");
    rows[0] = 2;
    // Keep the old hash deliberately: equality must compare all 96 bytes too.
    FlatContractRecord* reused = flatRecordContract(records, used, dropped, draw);
    check(reused && reused != first && reused->camera[0] == 2 && first->camera[0] == 1,
          "same CB pointer and camera hash cannot merge different row bytes");
    rows[95] = 7;
    draw.cameraHash = flatCameraHash(rows);
    check(flatRecordContract(records, used, dropped, draw) != reused && reused->camera[95] == 0,
          "later source mutation cannot alter either frozen record");
    auto distinct = [&](const FlatContractObservation& changed, const char* message) {
        const uint32_t before = used;
        check(flatRecordContract(records, used, dropped, changed) && used == before + 1, message);
    };
    FlatContractObservation changed = draw; changed.vs++;
    distinct(changed, "different VS stays separate");
    changed = draw; changed.ps++;
    distinct(changed, "different PS stays separate");
    changed = draw; changed.b1 = reinterpret_cast<void*>(0x3010);
    distinct(changed, "different b1 identity stays separate despite equal rows");
    changed = draw; changed.depth = reinterpret_cast<void*>(0x2010);
    distinct(changed, "different depth stays separate");
    changed = draw; changed.color = reinterpret_cast<void*>(0x1010);
    distinct(changed, "different color stays separate");
    for (uint32_t i = 0; i < 4; ++i) {
        changed = draw; changed.srvView[i] = reinterpret_cast<void*>(0x4000);
        distinct(changed, "each PS source view participates in contract key");
        changed = draw; changed.srvResource[i] = reinterpret_cast<void*>(0x5000);
        distinct(changed, "each PS source resource participates in contract key");
    }
    for (uint32_t i = 0; i < 6; ++i) {
        changed = draw; changed.viewport[i] += 0.25f;
        distinct(changed, "each exact viewport field participates in contract key");
    }
    changed = draw; changed.viewportCount = 2;
    distinct(changed, "multiple viewports cannot merge with single viewport evidence");
    changed = draw; changed.camera = nullptr; changed.cameraHash = 0;
    distinct(changed, "unavailable camera cannot merge with known camera");
    check(dropped == 0, "distinct-key regression fits fixed table");
}
void testContractAdmissionAndReservation() {
    using namespace edvr;
    const void* color = reinterpret_cast<void*>(0x1000);
    const void* depth = reinterpret_cast<void*>(0x2000);
    check(flatContractKind(false, color, depth, 960, 540, 23, 1280, 720, false) == kFlatContractScreen,
          "screen-sized stencil draw is screen evidence, never declared motion family");
    check(flatContractKind(true, color, depth, 960, 540, 23, 1280, 720, false) == kFlatContractPool,
          "only known VS family with color/depth can be declared pool draw");
    check(flatContractKind(true, nullptr, depth, 0, 0, 0, 1280, 720, false) == kFlatContractNone,
          "depth-only pass is not a color motion contract");
    check(flatContractKind(true, color, depth, 1280, 720, 28, 1280, 720, true) == kFlatContractOutput,
          "output classification takes precedence over motion-family declaration");
    check(flatContractKind(false, color, depth, 1024, 1024, 23, 1280, 720, false) == kFlatContractNone,
          "square shadow-like target is not screen-shaped handoff evidence");
    check(flatContractKind(false, color, nullptr, 960, 540, 26, 1280, 720, false) == kFlatContractScreen &&
          flatContractKind(false, color, nullptr, 960, 540, 27, 1280, 720, false) == kFlatContractScreen,
          "measured format 26 and 27 handoff remains eligible without depth");
    FlatContractRecord world[1]{}, handoff[2]{};
    uint32_t used = 0, dropped = 0, lateUsed = 0, lateDropped = 0;
    FlatContractObservation draw{};
    auto record = [&]() { return flatRecordContractReserved(world, used, dropped,
        handoff, lateUsed, lateDropped, draw); };
    check(!record() && used == 0 && dropped == 0 && lateUsed == 0 && lateDropped == 0,
          "no eligible draw is distinct from dropped evidence in empty report");
    draw.kind = kFlatContractPool; draw.color = color; draw.depth = depth;
    draw.sequence = 1;
    check(record() == &world[0], "first world record admitted");
    draw.ps = 1; draw.sequence = 2;
    check(!record() && used == 1 && dropped == 1 && world[0].last == 1,
          "world saturation reports dropped draw without overwriting evidence");
    draw.kind = kFlatContractScreen; draw.format = 27; draw.sequence = 3;
    check(record() == &handoff[0] && lateUsed == 1 && lateDropped == 0,
          "format-27 handoff survives full world bank");
    draw.kind = kFlatContractOutput; draw.format = 28; draw.sequence = 4;
    check(record() == &handoff[1] && lateUsed == 2,
          "final output survives full world bank");
    draw.ps = 2;
    check(!record() && lateDropped == 1 && dropped == 1,
          "handoff overflow is explicit and separate from world overflow");
    draw.ps = 1; draw.sequence = 5;
    check(record() == &handoff[1] && handoff[1].draws == 2 && handoff[1].last == 5,
          "matching output records still update when both banks are full");
}
void testAdmissionAndWindow() {
    check(!edvr::flatCaptureThreadEligible(false, 7, 7),
          "disarmed capture has no owner callbacks");
    check(!edvr::flatCaptureThreadEligible(true, 0, 7),
          "warmup before first Present has no owner callbacks");
    check(!edvr::flatCaptureThreadEligible(true, 7, 8),
          "foreign render thread cannot mutate collector");
    check(edvr::flatCaptureThreadEligible(true, 7, 7),
          "owned Present thread can collect");
    edvr::FlatTemporalProof proof{};
    check(!edvr::flatTemporalEvidenceComplete(proof), "no observation is no certificate");
    proof.sceneAndCamera = proof.consumedProjection = true;
    proof.matchedDepthAndMotion = proof.completionAndUiOrder = true;
    proof.outputAndModOrder = proof.jitterRollback = true;
    check(edvr::flatTemporalEvidenceComplete(proof), "all six independent certificates required");
    proof.unknownDeferredWork = true;
    check(!edvr::flatTemporalEvidenceComplete(proof), "unknown deferred list refuses treatment");
    proof.unknownDeferredWork = false;
    proof.duplicateTreatment = true;
    check(!edvr::flatTemporalEvidenceComplete(proof), "duplicate render treatment refused");
    proof.duplicateTreatment = false;
    proof.consumedProjection = false;
    check(!edvr::flatTemporalEvidenceComplete(proof), "target shape cannot substitute for projection proof");
    check(!edvr::flatCaptureExpired(100, 1099, 9, 1000, 10),
          "capture stays active inside both budgets");
    check(edvr::flatCaptureExpired(100, 1100, 9, 1000, 10),
          "time budget is exact");
    check(edvr::flatCaptureExpired(100, 1099, 10, 1000, 10),
          "Present budget is exact");
    check(!edvr::flatCaptureExpired(2000, 2000, 0, 1000, 10),
          "rearmed window starts fresh");
    check(!edvr::flatCaptureExpired(100, 200, 0, 1000, 10),
          "startup Present traffic with zero useful frames does not exhaust frame budget");
}

// Reconstructed metadata from Epic frames 36865 (960x540) and 41961
// (1280x720). Resource tokens are local stand-ins; shader pairs, camera rows,
// sequence ranges and the 21 supported / 5 unsupported draw split are captured.
struct MonoFixture {
    edvr::FlatContractRecord world[40]{}, handoff[8]{};
    edvr::FlatMonoFrameInput input{};
    float rows[6][4] = {
        {.384289086f, -.446604401f, 0, .904584467f},
        {-.540786624f, -1.65509605f, 0, -.0248376597f},
        {.848400056f, -.852697492f, 0, -.42557025f},
        {0, 0, .0250000004f, 0},
        {.904584467f, -.0248376597f, -.42557025f, 0},
        {-159.188187f, 1.31854951f, 72.945961f, 0}
    };
    static const void* token(uintptr_t n) { return reinterpret_cast<const void*>(n); }
    static void setCamera(edvr::FlatContractRecord& r, const float (&camera)[6][4]) {
        std::memcpy(r.camera, camera, sizeof(camera));
        r.key.camera = r.camera;
        r.key.cameraHash = edvr::flatCameraHash(r.camera);
    }
    void applyEpic63521CameraWords() {
        // The log captured these 15 differing raw words at frame 63521:
        // tone's unused VS b1 on the left, current HDR camera on the right.
        // Unreported words retain the valid shape from the older fixture;
        // these are a partial witness, not the full captured camera hashes.
        struct Difference { uint8_t row, column; uint32_t tone, hdr; };
        const Difference differences[] = {
            {0,0,0xBF7D7BF9,0xBDB4CA1D}, {0,1,0xBDE8121A,0xBE5E301D},
            {0,3,0xBDA7D984,0x3F7D7BFA}, {1,0,0xBDC84103,0xBE3F6EF5},
            {1,1,0x3F7ADE67,0x3FF02F7C}, {1,3,0xBE31BB57,0x3DC84104},
            {2,0,0x3DCCC3D1,0xBF874DE2}, {2,1,0xBE27C76E,0xBEA0A256},
            {2,3,0xBF7B3D6D,0xBDCCC3D2}, {4,0,0xBDA7D984,0x3F7D7BFA},
            {4,1,0xBE31BB57,0x3DC84104}, {4,2,0xBF7B3D6D,0xBDCCC3D2},
            {5,0,0x41474D18,0x4148624A}, {5,1,0x40930C74,0x4096BEB6},
            {5,2,0xBFED2CC0,0xBFF08358}
        };
        float toneRows[6][4]; std::memcpy(toneRows, rows, sizeof(rows));
        for (const auto& d : differences) {
            std::memcpy(&toneRows[d.row][d.column], &d.tone, sizeof(d.tone));
            std::memcpy(&rows[d.row][d.column], &d.hdr, sizeof(d.hdr));
        }
        for (uint32_t i = 0; i < 7; ++i) setCamera(world[i], rows);
        for (uint32_t i : {9u, 19u, 20u}) setCamera(world[i], rows);
        setCamera(handoff[0], toneRows); setCamera(handoff[1], toneRows);
    }
    void fill(edvr::FlatContractRecord& r, edvr::FlatContractKind kind,
              uintptr_t color, uint32_t fmt, uint32_t first, uint32_t last,
              uint32_t draws, uint64_t vs, uint64_t ps, uint32_t firstWrite,
              uint32_t lastWrite, bool camera = true) {
        r = edvr::FlatContractRecord{};
        auto& k = r.key;
        k.kind = kind; k.color = token(color); k.rtv = token(color + 1);
        k.width = input.outputWidth == 1280 && input.frame == 41961 ? 1280 : 960;
        k.height = k.width * 9 / 16; k.format = fmt;
        k.depth = token(0xD000); k.dsv = token(0xD001);
        k.depthWidth = k.width; k.depthHeight = k.height; k.depthFormat = 19;
        k.viewportCount = 1; k.viewport[2] = static_cast<float>(k.width);
        k.viewport[3] = static_cast<float>(k.height); k.viewport[5] = 1;
        k.vs = vs; k.ps = ps; k.sequence = first;
        r.draws = draws; r.first = first; r.last = last;
        r.firstCount = r.lastCount = 3; r.firstInstances = r.lastInstances = 1;
        if (camera) {
            k.b1 = token(0xB100); setCamera(r, rows);
            k.writeEpoch = r.firstWriteEpoch = r.lastWriteEpoch = input.epoch;
            k.writeSeq = r.firstWriteSeq = firstWrite; r.lastWriteSeq = lastWrite;
        }
    }
    explicit MonoFixture(uint32_t width = 960) {
        using namespace edvr;
        const bool full = width == 1280;
        input.world = world; input.handoff = handoff;
        input.worldCount = 21; input.handoffCount = 3;
        input.frame = full ? 41961 : 36865; input.epoch = full ? 1802 : 451;
        input.output = token(0xF000); input.outputWidth = 1280;
        input.outputHeight = 720; input.outputFormat = 28;
        input.supportedPair = engine_velocity_family::supportedPair;
        input.droppedSmallCb = 27;  // observed; all required scene rows survived
        if (full) {
            rows[5][0] = -90.3799591f; rows[5][1] = -.570732951f; rows[5][2] = 40.5745583f;
        }
        fill(world[0], kFlatContractPool, 0x2300, 23, full ? 644 : 238, full ? 649 : 243, 6,
            0xEB5234DB6ADB491Dull, 0x3434972DB5336AA4ull, full ? 643 : 237, full ? 643 : 237);
        fill(world[1], kFlatContractPool, 0x2300, 23, full ? 670 : 264, full ? 670 : 264, 1,
            0xDE545DC8EE4FBB87ull, 0xE46E3E4832B2FDB0ull, full ? 666 : 260, full ? 666 : 260);
        fill(world[2], kFlatContractPool, 0x2300, 23, full ? 674 : 269, full ? 679 : 278, 3,
            0x66DE2CADB1F4AE6Bull, 0x864F1F949851B8DEull, full ? 666 : 260, full ? 676 : 272);
        fill(world[3], kFlatContractPool, 0x2300, 23, full ? 675 : 271, full ? 680 : 280, 3,
            0x61AE8EB05FDC18DDull, 0xFC43E42710010343ull, full ? 666 : 260, full ? 676 : 272);
        fill(world[4], kFlatContractPool, 0x2300, 23, full ? 681 : 281, full ? 681 : 281, 1,
            0xAACFDCF2FB9AD809ull, 0xCF534B32F491561Aull, full ? 676 : 272, full ? 676 : 272);
        fill(world[5], kFlatContractPool, 0x2300, 23, full ? 691 : 295, full ? 693 : 297, 2,
            0x5B4D8E894EEDA8B4ull, 0x4375B72964F386CDull, full ? 689 : 293, full ? 692 : 296);
        fill(world[6], kFlatContractPool, 0x2300, 23, full ? 706 : 310, full ? 710 : 314, 5,
            0xBBE58E40FE88EC80ull, 0xDB3E8D20CF53FBC0ull, full ? 702 : 306, full ? 702 : 306);
        fill(world[7], kFlatContractPool, 0x2300, 23, full ? 652 : 246, full ? 656 : 250, 4,
            0xEB5234DB6ADB491Dull, 0xB7D50283329322C3ull, full ? 651 : 245, full ? 654 : 248);
        fill(world[8], kFlatContractPool, 0x2300, 23, full ? 673 : 267, full ? 673 : 267, 1,
            0xDE545DC8EE4FBB87ull, 0x91F8937EDA723663ull, full ? 666 : 260, full ? 666 : 260);
        fill(world[9], kFlatContractScreen, 0x2600, 26, full ? 917 : 491, full ? 919 : 493, 3,
            0x81216C77F90DEDD6ull, 0xA2965EC2931A39C8ull, full ? 916 : 490, full ? 916 : 490);
        // Nine additional HDR records without scene constants remain allowed.
        for (uint32_t i = 10; i < 19; ++i) {
            const uint32_t q = (full ? 735u : 339u) + i - 10;
            fill(world[i], kFlatContractScreen, 0x2600, 26, q, q, 1,
                0x7E38A6AA1269C901ull, 0x7CECABDE34FFBE9Eull, 0, 0, false);
            world[i].key.srvView[0] = token(0x2302); world[i].key.srvResource[0] = token(0x2300);
        }
        // Actual full-record replay exceptions: these three HDR draws have
        // full XY coverage but a zero-width depth range, in both captures.
        fill(world[14], kFlatContractScreen, 0x2600, 26, full ? 772 : 376, full ? 772 : 376, 1,
            0xF8FA801F2CB1E27Cull, 0x84965D3C050FB01Bull, 0, 0, false);
        fill(world[19], kFlatContractScreen, 0x2600, 26, full ? 775 : 379, full ? 775 : 379, 1,
            0x68DDDEF04D9894AFull, 0x06332CA168B6DA63ull, full ? 774 : 378, full ? 774 : 378);
        fill(world[20], kFlatContractScreen, 0x2600, 26, full ? 776 : 380, full ? 776 : 380, 1,
            0xF7A6E916F14A3B1Aull, 0x06332CA168B6DA63ull, full ? 774 : 378, full ? 774 : 378);
        world[14].key.viewport[5] = world[19].key.viewport[5] = world[20].key.viewport[5] = 0;
        world[14].firstCount = world[14].lastCount = 6;
        world[19].firstCount = world[19].lastCount = world[20].firstCount = world[20].lastCount = 12;
        world[19].firstInstances = world[19].lastInstances = 6655;
        world[20].firstInstances = world[20].lastInstances = 19;
        fill(handoff[0], kFlatContractScreen, 0x2700, 27, full ? 937 : 511, full ? 937 : 511, 1,
            flat_mono_detail::kToneVs, flat_mono_detail::kTonePs, full ? 916 : 490, full ? 916 : 490);
        fill(handoff[1], kFlatContractOutput, 0xF000, 28, full ? 941 : 515, full ? 941 : 515, 1,
            flat_mono_detail::kCopyVs, flat_mono_detail::kCopyPs, full ? 916 : 490, full ? 916 : 490);
        for (uint32_t i = 0; i < 2; ++i) {
            auto& k = handoff[i].key;
            k.depth = k.dsv = nullptr; k.depthWidth = k.depthHeight = k.depthFormat = 0;
        }
        handoff[0].key.srvView[1] = token(0x2602); handoff[0].key.srvResource[1] = token(0x2600);
        handoff[1].key.srvView[0] = token(0x2702); handoff[1].key.srvResource[0] = token(0x2700);
        handoff[1].key.width = 1280; handoff[1].key.height = 720;
        handoff[1].key.viewport[2] = 1280; handoff[1].key.viewport[3] = 720;
        handoff[1].firstCount = handoff[1].lastCount = full ? 4 : 3;
        fill(handoff[2], kFlatContractOutput, 0xF000, 28, full ? 946 : 520, full ? 946 : 520, 1,
            0xA888D51024D9798Eull, 0x015EF9349EC097E8ull, full ? 943 : 517, full ? 943 : 517);
        handoff[2].key.depth = token(0xDD00); handoff[2].key.dsv = token(0xDD01);
        handoff[2].key.width = handoff[2].key.depthWidth = 1280;
        handoff[2].key.height = handoff[2].key.depthHeight = 720;
        handoff[2].key.viewport[2] = 1280; handoff[2].key.viewport[3] = 720;
        const float panel[6][4] = {{1.07699156f, 0, 0, 0}, {0, 1.91465175f, 0, 0},
            {0, 0, -.000100016594f, 1}, {0, 0, .10001f, 0}, {0, 0, 1, 0}, {0, 0, 0, 0}};
        setCamera(handoff[2], panel);
    }
};

void testMonoFrameSelection() {
    using namespace edvr;
    for (uint32_t width : {960u, 1280u}) {
        MonoFixture f(width);
        const FlatMonoFrame out = flatSelectMonoFrame(f.input);
        check(out.selected() && out.renderWidth == width && out.renderHeight == width * 9 / 16 &&
              out.outputWidth == 1280 && out.outputHeight == 720 && out.nearPlane == .025f,
              "captured native and scaled mono inputs select correct render/output sizes and near");
        check(out.color == MonoFixture::token(0x2700) && out.hdr == MonoFixture::token(0x2600) &&
              out.depth == MonoFixture::token(0xD000) && out.sceneConstants == MonoFixture::token(0xB100),
              "selector joins exact post-tone/HDR/depth/camera identities");
        check(out.supportedDraws == 21 && out.unsupportedDraws == 5,
              "actual producer table admits 21 supported pairs, not all 26 VS-family draws");
        check(out.toneSequence == (width == 960 ? 511u : 937u) &&
              out.copySequence == (width == 960 ? 515u : 941u) &&
              out.firstLaterOutput == (width == 960 ? 520u : 946u),
              "observed tone/copy/later-panel ordering is preserved");
        f.world[19].camera[0] ^= 0xFF;
        check(std::memcmp(out.camera, f.rows, sizeof(out.camera)) == 0,
              "selected metadata owns camera rows independently of collector reuse");
    }
    auto reject = [](auto mutate, FlatMonoReason reason, const char* message) {
        MonoFixture f;
        mutate(f);
        const auto out = flatSelectMonoFrame(f.input);
        check(!out.selected() && out.reason == reason, message);
    };
    auto acceptHandoffCamera = [](auto mutate, const char* message) {
        MonoFixture f; mutate(f);
        const auto out = flatSelectMonoFrame(f.input);
        check(out.selected() && out.sceneConstants == f.world[19].key.b1 &&
              out.cameraHash == f.world[19].key.cameraHash &&
              std::memcmp(out.camera, f.world[19].camera, sizeof(out.camera)) == 0, message);
    };
    acceptHandoffCamera([](auto& f) {
        f.handoff[0].firstWriteEpoch--; f.handoff[1].firstWriteEpoch--;
    }, "stale tone and copy camera writes do not name the scene camera");
    acceptHandoffCamera([](auto& f) {
        f.handoff[0].key.camera = f.handoff[1].key.camera = nullptr;
        f.handoff[0].key.b1 = f.handoff[1].key.b1 = nullptr;
    }, "tone and copy need no VS camera binding");
    acceptHandoffCamera([](auto& f) {
        f.handoff[0].key.b1 = f.handoff[1].key.b1 = MonoFixture::token(0xBAAD);
        f.handoff[0].camera[0] ^= 1; f.handoff[1].camera[1] ^= 1;
    }, "rebound or mismatched fullscreen VS constants do not replace scene camera");
    acceptHandoffCamera([](auto& f) {
        float invalid[6][4]{}; invalid[0][0] = std::numeric_limits<float>::quiet_NaN();
        MonoFixture::setCamera(f.handoff[0], invalid);
        MonoFixture::setCamera(f.handoff[1], invalid);
    }, "malformed fullscreen camera bytes are irrelevant to texture-only passes");
    for (uint32_t width : {960u, 1280u}) {
        MonoFixture f(width); f.applyEpic63521CameraWords();
        const auto out = flatSelectMonoFrame(f.input);
        check(out.selected() && out.renderWidth == width &&
              out.cameraHash == f.world[19].key.cameraHash &&
              std::memcmp(out.camera, f.rows, sizeof(out.camera)) == 0,
              "Epic 63521 changed raw words preserve HDR camera authority at both extents");
    }
    reject([](auto& f) { f.input.worldCount = f.input.handoffCount = 0; }, FlatMonoReason::NoOutputCopy,
        "empty capture reports no copy instead of a selected empty frame");
    reject([](auto& f) { f.handoff[1].key.srvResource[0] = MonoFixture::token(0xBAD); }, FlatMonoReason::NoTonePass,
        "copy must read the exact observed tone target");
    reject([](auto& f) { f.handoff[0].key.srvResource[1] = MonoFixture::token(0xBAD); }, FlatMonoReason::NoHdr,
        "tone must read the exact observed HDR target at PS1");
    reject([](auto& f) { f.handoff[0].first = f.handoff[0].last = 516; }, FlatMonoReason::WrongOrder,
        "tone after output copy is refused");
    reject([](auto& f) { f.world[9].last = 512; }, FlatMonoReason::WrongOrder,
        "coalesced HDR write crossing tone boundary is refused");
    reject([](auto& f) { f.handoff[1].draws = 2; f.handoff[1].last++; }, FlatMonoReason::AmbiguousOutputCopy,
        "coalesced repeated output copy is not a unique pass");
    reject([](auto& f) { f.handoff[3] = f.handoff[1]; f.input.handoffCount = 4; }, FlatMonoReason::AmbiguousOutputCopy,
        "distinct output-copy records are ambiguous even when their sources agree");
    reject([](auto& f) { f.handoff[0].draws = 2; f.handoff[0].last++; }, FlatMonoReason::AmbiguousTonePass,
        "coalesced repeated tone pass is not unique");
    reject([](auto& f) { f.handoff[1].key.viewport[0] = .25f; }, FlatMonoReason::InvalidOutputCopy,
        "fractional output viewport offset is not fullscreen");
    reject([](auto& f) { f.handoff[0].key.viewportCount = 2; }, FlatMonoReason::InvalidTonePass,
        "unobserved second viewport prevents selection");
    reject([](auto& f) { f.handoff[0].key.viewport[5] = 0; }, FlatMonoReason::InvalidTonePass,
        "HDR zero-depth viewport exception cannot qualify tone pass");
    reject([](auto& f) { f.handoff[1].key.viewport[5] = 0; }, FlatMonoReason::InvalidOutputCopy,
        "HDR zero-depth viewport exception cannot qualify output copy");
    reject([](auto& f) { f.world[0].key.viewport[4] = .25f; }, FlatMonoReason::InvalidSource,
        "source depth viewport range must match measured contract");
    reject([](auto& f) { f.world[9].firstWriteEpoch--; }, FlatMonoReason::ConflictingHdr,
        "old-frame HDR camera cannot name this scene");
    reject([](auto& f) { f.world[19].firstWriteSeq = f.world[19].first + 1; }, FlatMonoReason::ConflictingHdr,
        "later HDR camera write cannot supply an earlier draw");
    reject([](auto& f) { f.world[0].firstWriteSeq = f.world[0].first + 1; }, FlatMonoReason::InvalidSource,
        "later write cannot supply an earlier source draw");
    reject([](auto& f) { f.world[0].lastWriteSeq = f.world[0].last + 1; }, FlatMonoReason::InvalidSource,
        "coalesced last draw also requires preceding write provenance");
    reject([](auto& f) { f.rows[5][0] += 1; MonoFixture::setCamera(f.world[0], f.rows); }, FlatMonoReason::AmbiguousSource,
        "supported source under a different camera cannot be silently ignored");
    reject([](auto& f) { f.world[21] = f.world[0]; f.world[21].key.depth = MonoFixture::token(0xD900);
        f.world[21].key.dsv = MonoFixture::token(0xD901); f.input.worldCount = 22; }, FlatMonoReason::AmbiguousSource,
        "same screen camera naming another depth is ambiguous");
    reject([](auto& f) { f.world[10].key.dsv = MonoFixture::token(0xBAD); }, FlatMonoReason::ConflictingHdr,
        "HDR writes must bind the same scene DSV");
    reject([](auto& f) { f.world[9].key.viewport[4] = .25f; }, FlatMonoReason::ConflictingHdr,
        "unmeasured HDR depth-range variant remains refused");
    reject([](auto& f) { f.rows[5][0] += 1; MonoFixture::setCamera(f.world[9], f.rows); }, FlatMonoReason::ConflictingHdr,
        "known conflicting HDR camera is not equivalent to absent fullscreen constants");
    reject([](auto& f) { for (uint32_t i : {9u, 19u, 20u}) {
        f.world[i].key.camera = nullptr; f.world[i].key.cameraHash = 0; } }, FlatMonoReason::NoHdrCamera,
        "HDR must contain at least one observed matching camera");
    reject([](auto& f) { for (uint32_t i = 0; i < 7; ++i) f.world[i].key.ps = 0; }, FlatMonoReason::NoSupportedSource,
        "VS families with unsupported PSs cannot name a motion source");
    reject([](auto& f) { f.input.supportedPair = nullptr; }, FlatMonoReason::InvalidInput,
        "missing production pair validator cannot select metadata");
    reject([](auto& f) { f.input.unknownLists = 1; }, FlatMonoReason::ForeignWork,
        "unknown command list makes input selection uncertain");
    reject([](auto& f) { f.input.foreignCalls = 1; }, FlatMonoReason::ForeignWork,
        "foreign-context observations make input selection uncertain");
    for (uint32_t i = 0; i < 5; ++i) {
        reject([i](auto& f) {
            uint32_t* drops[] = {&f.input.droppedViews, &f.input.droppedTargets, &f.input.droppedWorld,
                &f.input.droppedHandoff, &f.input.droppedLargeCb};
            *drops[i] = 1;
        }, FlatMonoReason::Truncated, "each relevant truncated observation bank refuses selection");
    }
    for (uint32_t bad = 0; bad < 5; ++bad) {
        reject([bad](auto& f) {
            if (bad == 0) f.rows[3][2] = 0;
            if (bad == 1) f.rows[0][0] = std::numeric_limits<float>::quiet_NaN();
            if (bad == 2) f.rows[0][2] = .001f;
            if (bad == 3) for (uint32_t i = 0; i < 3; ++i) f.rows[i][0] = 0;
            if (bad == 4) f.rows[4][0] += 1;
            for (uint32_t i : {9u, 19u, 20u}) MonoFixture::setCamera(f.world[i], f.rows);
        }, FlatMonoReason::InvalidCamera, "invalid camera near/finite/clip/basis/axis shape refuses selection");
    }
    reject([](auto& f) { f.world[21] = f.world[9]; f.input.worldCount = 22;
        f.world[21].key.color = MonoFixture::token(0x2700); f.world[21].first = f.world[21].last = 514;
        f.world[21].draws = 1; }, FlatMonoReason::BrokenLineage,
        "an intervening write to tone output breaks the observed handoff");
    check(!engine_velocity_family::supportedPair(0xEB5234DB6ADB491Dull, 0xB7D50283329322C3ull) &&
          !engine_velocity_family::supportedPair(0xDE545DC8EE4FBB87ull, 0x91F8937EDA723663ull),
          "historically unsupported captured pairs remain outside unchanged producer table");
}
} // namespace

void flatRuntimePrefixTests() {
    using namespace edvr;
    check(flatRuntimeDepthReadFormat(19) == 21 && flatRuntimeDepthReadFormat(39) == 41 && flatRuntimeDepthReadFormat(44) == 46 && !flatRuntimeDepthReadFormat(45), "captured format19 depth maps to depth-only float view; typed incompatible formats refuse");
    for (uint32_t width : {960u, 1280u}) for (bool epicWords : {false, true}) {
        MonoFixture fixture(width);
        if (epicWords) fixture.applyEpic63521CameraWords();
        auto prefix = std::make_unique<FlatRuntimePrefix>();
        prefix->frame = fixture.input.frame; prefix->output = fixture.input.output;
        prefix->width = 1280; prefix->height = 720; prefix->format = 28;
        struct Event { const FlatContractRecord* r; uint32_t q; } events[200]{};
        uint32_t count = 0;
        for (uint32_t i = 0; i < fixture.input.worldCount; ++i) {
            const auto& r = fixture.world[i];
            for (uint32_t n = 0; n < r.draws; ++n) events[count++] = {&r, r.first + (r.last-r.first)*n/(r.draws>1?r.draws-1:1)};
        }
        events[count++] = {&fixture.handoff[0], fixture.handoff[0].first};
        events[count++] = {&fixture.handoff[1], fixture.handoff[1].first};
        std::sort(events, events + count, [](const Event& a, const Event& b) { return a.q < b.q; });
        FlatMonoFrame selected{};
        FlatRuntimeDraw copy{};
        std::unique_ptr<FlatRuntimePrefix> beforeCopy;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& r = *events[i].r; FlatRuntimeDraw d{}; d.key = r.key;
            std::memcpy(d.camera, r.camera, sizeof(d.camera));
            d.key.writeEpoch = prefix->frame; d.key.writeSeq = prefix->sequence + 1;
            d.supported = engine_velocity_family::supportedPair(d.key.vs, d.key.ps);
            d.instances = r.firstInstances;
            if (i + 1 == count) beforeCopy = std::make_unique<FlatRuntimePrefix>(*prefix);
            selected = flatRuntimeObserve(*prefix, d); copy = d;
        }
        check(selected.selected() && selected.renderWidth == width &&
              selected.cameraHash == fixture.world[19].key.cameraHash &&
              std::memcmp(selected.camera, fixture.rows, sizeof(selected.camera)) == 0,
              "online native/scaled replay selects current HDR camera including overwritten handoff words");
        check(!flatRuntimeObserve(*prefix, copy).selected(), "second output copy in same prefix is rejected");
        auto refusal = [&](auto change, const char* message) {
            auto p = std::make_unique<FlatRuntimePrefix>(*beforeCopy); auto d = copy;
            change(*p, d); check(!flatRuntimeObserve(*p, d).selected(), message);
        };
        refusal([](auto& p, auto&) { p.uncertain = true; }, "unknown/foreign/truncated work denies online resolve");
        refusal([](auto&, auto& d) { d.key.viewport[0] = 1; }, "actual copy viewport must cover full backbuffer");
        auto inertCopy = std::make_unique<FlatRuntimePrefix>(*beforeCopy);
        auto staleCopy = copy; --staleCopy.key.writeEpoch;
        staleCopy.key.writeSeq = inertCopy->sequence + 100;
        staleCopy.key.b1 = MonoFixture::token(0xBAAD);
        staleCopy.key.camera = nullptr;
        check(flatRuntimeObserve(*inertCopy, staleCopy).selected(),
              "stale, later or missing copy VS camera does not gate current HDR scene");
        auto inertTone = std::make_unique<FlatRuntimePrefix>(*beforeCopy);
        for (uint32_t i = 0; i < inertTone->targetsUsed; ++i) {
            auto& t = inertTone->targets[i];
            if (t.resource != MonoFixture::token(0x2700)) continue;
            t.tone.key.b1 = MonoFixture::token(0xBAAD);
            t.tone.key.camera = nullptr;
            t.tone.firstWriteEpoch = 0;
        }
        check(flatRuntimeObserve(*inertTone, copy).selected(),
              "missing or rebound tone VS camera does not gate current HDR scene");
        refusal([](auto&, auto& d) { d.key.srvResource[0] = MonoFixture::token(0xDEAD); }, "output copy cannot use unrelated tone resource");
        refusal([](auto& p, auto&) { p.sourcesUsed = 0; }, "no supported current-frame source denies treatment");
        refusal([](auto& p, auto&) { auto r = p.sources[0]; r.key.depth = MonoFixture::token(0xBAD0); p.sources[p.sourcesUsed++] = r; }, "another same-camera scene depth is ambiguous");
        refusal([](auto& p, auto&) { p.sources[0].key.camera = nullptr; }, "overwritten scene depth invalidates source provenance");
        refusal([](auto& p, auto&) { for (uint32_t i=0;i<p.targetsUsed;++i) if (p.targets[i].tones) ++p.targets[i].tones; }, "multiple tone draws refuse online handoff");
        refusal([](auto& p, auto&) { flatRuntimeWritten(p, MonoFixture::token(0x2600)); }, "unknown HDR transfer after tone denies lineage");
        auto unchanged = std::make_unique<FlatRuntimePrefix>(*beforeCopy);
        flatRuntimeWritten(*unchanged, MonoFixture::token(0xDEAD));
        check(flatRuntimeObserve(*unchanged, copy).selected(), "unrelated resource writes do not invalidate established lineage");

        auto witness = std::make_unique<FlatRuntimePrefix>(*beforeCopy);
        FlatRuntimeDraw bad{}; bad.key = fixture.world[9].key;
        std::memcpy(bad.camera, fixture.world[9].camera, sizeof(bad.camera));
        bad.key.writeEpoch = witness->frame; bad.key.writeSeq = witness->sequence + 1;
        bad.key.viewport[4] = .25f;
        flatRuntimeObserve(*witness, bad);
        bad.key.depth = MonoFixture::token(0xBAD0);
        bad.key.dsv = MonoFixture::token(0xBAD1);
        flatRuntimeObserve(*witness, bad);
        bool hdrWitness = false;
        for (uint32_t i = 0; i < witness->targetsUsed; ++i)
            if (witness->targets[i].resource == MonoFixture::token(0x2600))
                hdrWitness = witness->targets[i].firstBad.cause == FlatRuntimeConflict::Viewport;
        check(hdrWitness, "a malformed HDR draw stores its first bad event");
        auto conflicted = flatRuntimeObserve(*witness, copy);
        check(conflicted.reason == FlatMonoReason::ConflictingHdr &&
              witness->selectedConflict.cause == FlatRuntimeConflict::Viewport &&
              witness->selectedConflict.current.viewport[4] == .25f,
              "selected HDR witness preserves the first cause despite later mismatches");

        auto cameraWitness = std::make_unique<FlatRuntimePrefix>(*beforeCopy);
        FlatRuntimeDraw changed{}; changed.key = fixture.world[19].key;
        std::memcpy(changed.camera, fixture.world[19].camera, sizeof(changed.camera));
        changed.camera[20] ^= 1; changed.key.cameraHash = flatCameraHash(changed.camera);
        changed.key.writeEpoch = cameraWitness->frame;
        changed.key.writeSeq = cameraWitness->sequence + 1;
        flatRuntimeObserve(*cameraWitness, changed);
        conflicted = flatRuntimeObserve(*cameraWitness, copy);
        check(conflicted.reason == FlatMonoReason::ConflictingHdr &&
              cameraWitness->selectedConflict.cause == FlatRuntimeConflict::CameraChange &&
              cameraWitness->selectedConflict.reference.b1 == fixture.world[19].key.b1 &&
              cameraWitness->selectedConflict.current.b1 == changed.key.b1,
              "changed HDR camera records HDR-to-HDR witness without naming tone");

        auto noisy = std::make_unique<FlatRuntimePrefix>(*beforeCopy);
        auto* unrelated = flatRuntimeTarget(*noisy, MonoFixture::token(0xDEAD));
        check(unrelated != nullptr, "unrelated HDR target fits bounded prefix");
        if (unrelated) {
            unrelated->writes = noisy->targets[0].writes;
            flatRuntimeWritten(*noisy, MonoFixture::token(0xDEAD));
            check(flatRuntimeObserve(*noisy, copy).selected() &&
                  noisy->selectedConflict.cause == FlatRuntimeConflict::None,
                  "bad unrelated HDR target does not contaminate selected witness or admission");
        }

        prefix->copies = 0; flatRuntimeWritten(*prefix, selected.color);
        check(!flatRuntimeObserve(*prefix, copy).selected(), "write after tone invalidates current handoff");
    }
}

void flatRuntimeImageCopyTests() {
    using namespace edvr;
    enum Scenario { Valid, SourceDepth, SourceCamera, SourceViewport, MissingSource,
                    SourceExplicitWrite, SourceStale, Unverified, WrongPair,
                    Alias, PriorHdrBad, PriorAndSourceBad, CopyCameraUnused,
                    AllInertAbsent, AllInertArbitrary, MixedInertFirst,
                    MixedGeometryFirst, InertUnverified, InertWrongPair,
                    InertDepth, InertViewport, MixedStaleGeometry,
                    MixedChangedGeometry, InertExplicitWrite };
    auto replay = [](Scenario scenario) {
        MonoFixture fixture;
        FlatContractRecord source[2]{}, imageCopy{};
        for (uint32_t i = 0; i < 2; ++i)
            fixture.fill(source[i], kFlatContractScreen, 0x2900, 9, 485 + i, 485 + i,
                         1, 0x10203040ull, 0x50607080ull, 484 + i, 484 + i);
        const bool firstInert = scenario == AllInertAbsent || scenario == AllInertArbitrary ||
            scenario == MixedInertFirst || scenario == InertUnverified ||
            scenario == InertWrongPair || scenario == InertDepth ||
            scenario == InertViewport || scenario == InertExplicitWrite || scenario == MixedStaleGeometry ||
            scenario == MixedChangedGeometry;
        const bool secondInert = scenario == AllInertAbsent || scenario == AllInertArbitrary ||
            scenario == MixedGeometryFirst;
        for (uint32_t i = 0; i < 2; ++i) if ((i == 0 && firstInert) || (i == 1 && secondInert)) {
            source[i].key.vs = 0xCFA91824129ECBBCull;
            source[i].key.ps = 0xFCFAD73924BF45B9ull;
            source[i].key.camera = nullptr;
            source[i].key.cameraHash = 0;
            if (scenario == AllInertAbsent) source[i].key.b1 = nullptr;
        }
        if (scenario == InertWrongPair) ++source[0].key.ps;
        fixture.fill(imageCopy, kFlatContractScreen, 0x2600, 26, 494, 494, 1,
                     0xCFA91824129ECBBCull, 0xDFCBA0EC70B03C9Bull, 0, 0, false);
        imageCopy.key.depth = imageCopy.key.dsv = nullptr;
        imageCopy.key.depthWidth = imageCopy.key.depthHeight = imageCopy.key.depthFormat = 0;
        imageCopy.key.srvResource[0] = MonoFixture::token(0x2900);
        imageCopy.key.srvView[0] = MonoFixture::token(0x2902);
        if (scenario == SourceDepth || scenario == PriorAndSourceBad || scenario == InertDepth)
            source[1].key.dsv = MonoFixture::token(0xDEAD);
        if (scenario == SourceCamera || scenario == MixedChangedGeometry) {
            float changed[6][4]; std::memcpy(changed, fixture.rows, sizeof(changed));
            changed[5][0] += 1;
            MonoFixture::setCamera(source[1], changed);
        }
        if (scenario == SourceViewport || scenario == InertViewport) source[1].key.viewport[0] = 1;
        if (scenario == WrongPair) ++imageCopy.key.ps;
        if (scenario == Alias) imageCopy.key.srvResource[0] = imageCopy.key.color;
        if (scenario == PriorHdrBad || scenario == PriorAndSourceBad)
            fixture.world[20].key.viewport[0] = 1;
        struct Event { const FlatContractRecord* r; uint32_t q; } events[200]{};
        uint32_t count = 0;
        for (uint32_t i = 0; i < fixture.input.worldCount; ++i) {
            const auto& r = fixture.world[i];
            for (uint32_t n = 0; n < r.draws; ++n)
                events[count++] = {&r, r.first + (r.last-r.first)*n/(r.draws>1?r.draws-1:1)};
        }
        if (scenario != MissingSource) for (const auto& r : source) events[count++] = {&r, r.first};
        events[count++] = {&imageCopy, imageCopy.first};
        events[count++] = {&fixture.handoff[0], fixture.handoff[0].first};
        events[count++] = {&fixture.handoff[1], fixture.handoff[1].first};
        std::sort(events, events + count, [](const Event& a, const Event& b) { return a.q < b.q; });
        auto prefix = std::make_unique<FlatRuntimePrefix>();
        prefix->frame = fixture.input.frame; prefix->output = fixture.input.output;
        prefix->width = 1280; prefix->height = 720; prefix->format = 28;
        FlatMonoFrame selected{};
        for (uint32_t i = 0; i < count; ++i) {
            const auto& r = *events[i].r;
            FlatRuntimeDraw d{}; d.key = r.key;
            std::memcpy(d.camera, r.camera, sizeof(d.camera));
            d.key.writeEpoch = prefix->frame; d.key.writeSeq = prefix->sequence + 1;
            if ((scenario == SourceStale || scenario == MixedStaleGeometry) && events[i].r == &source[1]) --d.key.writeEpoch;
            if (events[i].r == &source[0] || events[i].r == &source[1]) {
                const bool inert = events[i].r == &source[0] ? firstInert : secondInert;
                d.imageSourceCameraIndependentVerified = inert && scenario != InertUnverified;
                if (inert && (scenario == AllInertArbitrary || scenario == MixedGeometryFirst)) {
                    d.key.b1 = MonoFixture::token(0xBAAD + i);
                    d.key.camera = d.camera;
                    d.key.cameraHash = 0xBAD;
                    d.key.writeEpoch = 0;
                }
            }
            if (scenario == CopyCameraUnused && events[i].r == &imageCopy) {
                d.key.b1 = MonoFixture::token(0xBAAD);
                d.key.camera = d.camera; d.key.cameraHash = 0xBAD;
                d.key.writeEpoch = 0;
            }
            d.hdrCopyVerified = events[i].r == &imageCopy && scenario != Unverified;
            d.supported = engine_velocity_family::supportedPair(d.key.vs, d.key.ps);
            d.instances = r.firstInstances;
            if ((scenario == SourceExplicitWrite || scenario == InertExplicitWrite) && events[i].r == &imageCopy)
                flatRuntimeWritten(*prefix, MonoFixture::token(0x2900));
            selected = flatRuntimeObserve(*prefix, d);
        }
        return std::make_pair(std::move(prefix), selected);
    };
    for (Scenario s : {Valid, CopyCameraUnused}) {
        auto result = replay(s);
        check(result.second.selected() && result.first->imageCopiesAccepted == 1 &&
              result.first->imageCopiesRefused == 0 && result.second.hdr == MonoFixture::token(0x2600),
              "verified image copy continues prior HDR lineage despite unused copy VS camera");
    }
    for (Scenario s : {AllInertAbsent, AllInertArbitrary, MixedInertFirst, MixedGeometryFirst}) {
        auto result = replay(s);
        check(result.second.selected() && result.first->imageCopiesAccepted == 1 &&
              result.first->imageCopiesRefused == 0,
              "verified inert image writes qualify with absent/arbitrary b1 in either order");
    }
    for (Scenario s : {InertUnverified, InertWrongPair, InertDepth, InertViewport, InertExplicitWrite,
                       MixedStaleGeometry, MixedChangedGeometry}) {
        auto result = replay(s);
        check(!result.second.selected() && result.first->imageCopiesAccepted == 0 &&
              result.first->imageCopiesRefused == 1 &&
              result.first->selectedConflict.cause == FlatRuntimeConflict::ImageCopySource,
              "inert exception keeps verification, shader, view, depth and geometry camera gates");
    }
    for (Scenario s : {SourceDepth, SourceCamera, SourceViewport, MissingSource,
                       SourceExplicitWrite, SourceStale, Unverified, Alias}) {
        auto result = replay(s);
        check(!result.second.selected() && result.first->imageCopiesAccepted == 0 &&
              result.first->imageCopiesRefused == 1 &&
              result.first->selectedConflict.cause == FlatRuntimeConflict::ImageCopySource,
              "image copy source or prior HDR inconsistency preserves refusal witness");
    }
    auto depth = replay(SourceDepth);
    check(depth.first->selectedConflict.reference.dsv == MonoFixture::token(0xD001) &&
          depth.first->selectedConflict.current.dsv == MonoFixture::token(0xDEAD),
          "image copy witness names the conflicting second source write");
    for (Scenario s : {PriorHdrBad, PriorAndSourceBad}) {
        auto result = replay(s);
        check(!result.second.selected() && result.first->imageCopiesRefused == 1 &&
              result.first->selectedConflict.cause == FlatRuntimeConflict::Viewport &&
              result.first->selectedConflict.current.viewport[0] == 1,
              "earlier HDR viewport failure remains the first witness after refused image copy");
    }
    auto wrong = replay(WrongPair);
    check(!wrong.second.selected() && wrong.first->imageCopiesAccepted == 0 &&
          wrong.first->imageCopiesRefused == 0,
          "unrecognized depthless HDR shader pair remains refused by ordinary HDR rules");
}

void flatRuntimeMenuCopyTests() {
    using namespace edvr;
    enum Scenario { Valid, CopyCameraUnused, PreCopySourceCompute, PostCopyDestinationCompute,
        SourceDepth, SourceViewport, SourceStale, SourceLayout,
        SourceCameraChange,
        SourceExplicitWrite, MissingSource, MissingCamera, WrongFormat, Unverified,
        WrongPair, Alias, PriorDestinationWrite, PriorDestinationBad, PriorAndSourceBad };
    auto replay = [](Scenario scenario) {
        MonoFixture fixture;
        FlatContractRecord menuCopy{};
        fixture.fill(menuCopy, kFlatContractScreen, 0x2600, 26, 500, 500, 1,
            0xDEF19B035D5EDEDCull, 0xDED8796049C7BB4Aull, 0, 0, false);
        menuCopy.key.depth = menuCopy.key.dsv = nullptr;
        menuCopy.key.depthWidth = menuCopy.key.depthHeight = menuCopy.key.depthFormat = 0;
        menuCopy.key.srvView[0] = MonoFixture::token(0x2802);
        menuCopy.key.srvResource[0] = MonoFixture::token(0x2800);
        for (uint32_t i = 9; i <= 20; ++i) {
            fixture.world[i].key.color = MonoFixture::token(0x2800);
            fixture.world[i].key.rtv = MonoFixture::token(0x2801);
            if (scenario == MissingCamera) fixture.world[i].key.camera = nullptr;
            if (scenario == WrongFormat) fixture.world[i].key.format = 23;
        }
        if (scenario == SourceDepth) fixture.world[20].key.dsv = MonoFixture::token(0xDEAD);
        if (scenario == PriorAndSourceBad) fixture.world[20].key.dsv = MonoFixture::token(0xDEAD);
        if (scenario == SourceLayout) fixture.world[20].key.format = 23;
        if (scenario == SourceViewport) fixture.world[20].key.viewport[0] = 1;
        if (scenario == SourceCameraChange) {
            float changed[6][4]; std::memcpy(changed, fixture.rows, sizeof(changed));
            changed[5][0] += 1;
            MonoFixture::setCamera(fixture.world[20], changed);
        }
        if (scenario == WrongPair) ++menuCopy.key.ps;
        if (scenario == Alias) menuCopy.key.srvResource[0] = menuCopy.key.color;
        FlatContractRecord prior{};
        if (scenario == PriorDestinationWrite || scenario == PriorDestinationBad || scenario == PriorAndSourceBad) {
            fixture.fill(prior, kFlatContractScreen, 0x2600, 26, 499, 499, 1,
                0x81216C77F90DEDD6ull, 0xA2965EC2931A39C8ull, 498, 498);
            if (scenario == PriorDestinationBad || scenario == PriorAndSourceBad) prior.key.viewport[0] = 1;
        }
        struct Event { const FlatContractRecord* r; uint32_t q; } events[200]{};
        uint32_t count = 0;
        for (uint32_t i = 0; i < fixture.input.worldCount; ++i) {
            if (scenario == MissingSource && i >= 9) continue;
            const auto& r = fixture.world[i];
            for (uint32_t n = 0; n < r.draws; ++n)
                events[count++] = {&r, r.first + (r.last-r.first)*n/(r.draws>1?r.draws-1:1)};
        }
        if (scenario == PriorDestinationWrite || scenario == PriorDestinationBad || scenario == PriorAndSourceBad)
            events[count++] = {&prior, prior.first};
        events[count++] = {&menuCopy, menuCopy.first};
        events[count++] = {&fixture.handoff[0], fixture.handoff[0].first};
        events[count++] = {&fixture.handoff[1], fixture.handoff[1].first};
        std::sort(events, events + count, [](const Event& a, const Event& b) { return a.q < b.q; });
        auto prefix = std::make_unique<FlatRuntimePrefix>();
        prefix->frame = fixture.input.frame; prefix->output = fixture.input.output;
        prefix->width = 1280; prefix->height = 720; prefix->format = 28;
        FlatMonoFrame selected{};
        for (uint32_t i = 0; i < count; ++i) {
            const auto& r = *events[i].r; FlatRuntimeDraw d{}; d.key = r.key;
            std::memcpy(d.camera, r.camera, sizeof(d.camera));
            d.key.writeEpoch = prefix->frame; d.key.writeSeq = prefix->sequence + 1;
            if (scenario == SourceStale && events[i].r == &fixture.world[19]) --d.key.writeEpoch;
            if (scenario == CopyCameraUnused && events[i].r == &menuCopy) {
                d.key.b1 = MonoFixture::token(0xBAAD); d.key.camera = d.camera;
                d.key.cameraHash = 0xBAD; d.key.writeEpoch = 0;
            }
            d.menuHdrCopyVerified = events[i].r == &menuCopy && scenario != Unverified;
            d.supported = engine_velocity_family::supportedPair(d.key.vs, d.key.ps);
            d.instances = r.firstInstances;
            if (scenario == SourceExplicitWrite && events[i].r == &menuCopy)
                flatRuntimeWritten(*prefix, MonoFixture::token(0x2800));
            if (scenario == PreCopySourceCompute && events[i].r == &menuCopy)
                flatRuntimeComputeWritten(*prefix, MonoFixture::token(0x2800));
            selected = flatRuntimeObserve(*prefix, d);
            if (scenario == PostCopyDestinationCompute && events[i].r == &menuCopy)
                flatRuntimeComputeWritten(*prefix, MonoFixture::token(0x2600));
        }
        return std::make_pair(std::move(prefix), selected);
    };
    MonoFixture baseline;
    for (Scenario s : {Valid, CopyCameraUnused, PreCopySourceCompute}) {
        auto result = replay(s);
        check(result.second.selected() && result.second.hdr == MonoFixture::token(0x2600) &&
              result.second.depth == MonoFixture::token(0xD000) &&
              result.second.sceneConstants == MonoFixture::token(0xB100) &&
              result.second.cameraHash == baseline.world[19].key.cameraHash &&
              std::memcmp(result.second.camera, baseline.rows, sizeof(result.second.camera)) == 0 &&
              result.first->menuCopiesAccepted == 1 && result.first->menuCopiesRefused == 0,
              "verified menu copy transfers current scene lineage independently of copy b1");
    }
    for (Scenario s : {SourceDepth, SourceViewport, SourceStale, SourceLayout,
                       SourceCameraChange, SourceExplicitWrite,
                       MissingSource, MissingCamera, WrongFormat, Unverified, Alias,
                       PriorDestinationWrite, PriorDestinationBad, PriorAndSourceBad}) {
        auto result = replay(s);
        check(!result.second.selected() && result.first->menuCopiesAccepted == 0 &&
              result.first->menuCopiesRefused == 1 &&
              result.second.reason == FlatMonoReason::ConflictingHdr &&
              result.first->selectedConflict.cause != FlatRuntimeConflict::None,
              "menu copy refuses invalid source, prior destination, alias and unverified views");
    }
    auto overwritten = replay(PostCopyDestinationCompute);
    check(!overwritten.second.selected() && overwritten.first->menuCopiesAccepted == 1 &&
          overwritten.first->selectedConflict.cause == FlatRuntimeConflict::ExplicitWrite,
          "compute overwrite after accepted menu copy invalidates inherited HDR before tone");
    auto depth = replay(SourceDepth);
    check(depth.first->selectedConflict.cause == FlatRuntimeConflict::DepthMismatch &&
          depth.first->selectedConflict.current.dsv == MonoFixture::token(0xDEAD),
          "menu copy reports original source depth witness");
    auto absent = replay(MissingSource);
    check(absent.first->selectedConflict.cause == FlatRuntimeConflict::MenuCopySource,
          "untracked menu copy source is visible as a specific HDR conflict");
    auto prior = replay(PriorAndSourceBad);
    check(prior.first->selectedConflict.cause == FlatRuntimeConflict::Viewport &&
          prior.first->selectedConflict.current.viewport[0] == 1,
          "prior destination conflict wins over later invalid source");
    auto wrong = replay(WrongPair);
    check(!wrong.second.selected() && !wrong.first->menuCopiesAccepted && !wrong.first->menuCopiesRefused,
          "unknown depthless shader pair does not enter menu copy path");
}

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("usage: flat_temporal_test --self-test");
        return 2;
    }
    failures += flatProjectionViewportTests();
    failures += flatPixelCaptureTests();
    testAssociationAndBounds();
    testFrozenEvidence();
    testOutputEdgeReservation();
    testSparseCameraRows();
    testContractKeysAndReuse();
    testContractAdmissionAndReservation();
    testAdmissionAndWindow();
    testMonoFrameSelection();
    testProjectionSlices();
    testDetailBudget();
    failures += flatShaderCaptureTests();
    failures += flatProjectionMathTests();
    failures += flatProjectionBindingsTests();
    failures += flatProjectionRecipeTests();
    failures += flatProjectionOwnershipTests();
    failures += flatComputeTests();
    failures += flatLightingTests();
    failures += flatLivePhaseTests();
    flatRuntimePrefixTests();
    flatRuntimeImageCopyTests();
    flatRuntimeMenuCopyTests();
    if (failures) return 1;
    std::puts("flat temporal collector policy: PASS");
    return 0;
}
