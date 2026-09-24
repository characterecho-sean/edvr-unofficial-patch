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
    testAdmissionAndWindow();
    if (failures) return 1;
    std::puts("flat temporal collector policy: PASS");
    return 0;
}
