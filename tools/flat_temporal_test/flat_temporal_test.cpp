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
    void* source = nullptr;
    void* dest = nullptr;
    char kind = 0;
    unsigned uses = 0;
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
        [=](const Route& e) { return e.source == c && e.dest == d1 && e.kind == 'S'; });
    r->source = c; r->dest = d1; r->kind = 'S'; r->uses = 1;
    Route* repeat = edvr::flatFindOrAdd(routes, used, overflow,
        [=](const Route& e) { return e.source == c && e.dest == d1 && e.kind == 'S'; });
    check(repeat == r && used == 1, "bound-SRV route coalesces");
    Route* copy = edvr::flatFindOrAdd(routes, used, overflow,
        [=](const Route& e) { return e.source == c && e.dest == d1 && e.kind == 'R'; });
    check(copy != nullptr && copy != r, "copy and sampled routes are distinct evidence");
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
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("usage: flat_temporal_test --self-test");
        return 2;
    }
    testAssociationAndBounds();
    testAdmissionAndWindow();
    if (failures) return 1;
    std::puts("flat temporal collector policy: PASS");
    return 0;
}
