#include "../../src/d3d11/flat_temporal_model.h"

#include <cstdio>
#include <cstring>

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
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("usage: flat_temporal_test --self-test");
        return 2;
    }
    testAssociationAndBounds();
    testFrozenEvidence();
    testOutputEdgeReservation();
    testSparseCameraRows();
    testContractKeysAndReuse();
    testContractAdmissionAndReservation();
    testAdmissionAndWindow();
    if (failures) return 1;
    std::puts("flat temporal collector policy: PASS");
    return 0;
}
