#include <memory>
#include <algorithm>
#include <utility>
#include "../../src/d3d11/flat_temporal_model.h"
#include "../../src/d3d11/flat_mono_frame.h"
#include "../../src/d3d11/flat_mono_resolve.h"
#include "../../src/d3d11/flat_runtime_model.h"
#include "../../src/d3d11/flat_dlss_negotiate.h"
#include "../../src/d3d11/flat_trace.h"
#include "../../src/d3d11/engine_velocity_families.h"
#include "flat_shader_capture_tests.h"
#include "flat_camera_probe_tests.h"
#include "flat_projection_math_tests.h"
#include "flat_projection_bindings_tests.h"
#include "flat_projection_recipe_tests.h"
#include "flat_shader_classifier_tests.h"
#include "flat_projection_viewport_tests.h"
#include "flat_projection_ownership_tests.h"
#include "flat_compute_tests.h"
#include "flat_lighting_tests.h"
#include "flat_live_phase_tests.h"
#include "flat_pixel_capture_tests.h"
#include "flat_local_reject_tests.h"
#include "flat_negotiated_eval_tests.h"
#include "flat_standdown_tests.h"
#include "flat_elite_settings_tests.h"
#include "flat_cpu_tests.h"
#include "flat_witness_bound_tests.h"
#include "flat_camera_table_tests.h"
#include "flat_query_cut_tests.h"
#include "flat_wrapper_note_tests.h"
#include "flat_hdr_route_tests.h"
#include "flat_copy_structure_tests.h"
#include "flat_hdr_crumbs_tests.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <vector>

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
// sequence ranges and the 22 supported / 4 unsupported draw split are captured
// under the current flat-only admission of DE54/PS91.
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
        setCamera(world[8], rows);  // PS91 now names a supported source too.
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
        check(out.supportedDraws == 22 && out.unsupportedDraws == 4,
              "producer admits the qualified front-face draw while four leg draws remain unsupported");
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
    auto hdrAtPs0 = [](MonoFixture& f) {
        // The HDR moves to PS0; PS1 becomes the variant's blur/bloom with its
        // own tokens, so a wrong-slot read cannot pass as the HDR.
        auto& k = f.handoff[0].key;
        k.srvView[0] = MonoFixture::token(0x2602); k.srvResource[0] = MonoFixture::token(0x2600);
        k.srvView[1] = MonoFixture::token(0x2B12); k.srvResource[1] = MonoFixture::token(0x2B10);
    };
    {   // Epic 20260926_073622, all settings maxed: the DoF-composite tone
        // variant is the same tone VS with a different PS, and it binds the
        // HDR at PS0 (its PS1 is the quarter-res DoF blur). The chain's role
        // and ordering checks are unchanged.
        MonoFixture f;
        f.handoff[0].key.ps = flat_mono_detail::kToneDofCompositePs;
        hdrAtPs0(f);
        const auto out = flatSelectMonoFrame(f.input);
        check(out.selected() && out.hdr == MonoFixture::token(0x2600) &&
              out.toneSequence == 511u,
            "DoF-composite tone variant selects with its HDR lineage at PS0");
    }
    {   // Epic 20260926_124418 frame 33504 (EDHM chained): the settings-tier
        // tone rides the no-constant passthrough VS with the bloom-composite
        // PS; its HDR lineage is at PS0.
        MonoFixture f;
        f.handoff[0].key.vs = flat_mono_detail::kToneVsNoConst;
        f.handoff[0].key.ps = flat_mono_detail::kToneBloomCompositePs;
        hdrAtPs0(f);
        const auto out = flatSelectMonoFrame(f.input);
        check(out.selected() && out.hdr == MonoFixture::token(0x2600) &&
              out.toneSequence == 511u,
            "bloom-composite tier tone selects with its HDR lineage at PS0");
    }
    {   // Epic 20260926_124418 frame 33939 (EDHM chained): EDHM's recolor
        // grade PS rides the stock tone VS; the HDR stays at PS1.
        MonoFixture f;
        f.handoff[0].key.ps = flat_mono_detail::kToneEdhmGradePs;
        const auto out = flatSelectMonoFrame(f.input);
        check(out.selected() && out.hdr == MonoFixture::token(0x2600) &&
              out.toneSequence == 511u,
            "EDHM-grade tone selects with its HDR lineage at PS1");
    }
    {   // Epic 20260926_075702 frame 32865: the same grade PS with the cb2-z
        // passthrough VS -- the observed frames mix tone VS against tone PS.
        MonoFixture f;
        f.handoff[0].key.vs = flat_mono_detail::kToneVsCbZ;
        f.handoff[0].key.ps = flat_mono_detail::kToneEdhmGradePs;
        const auto out = flatSelectMonoFrame(f.input);
        check(out.selected() && out.hdr == MonoFixture::token(0x2600),
            "tone VS variants are interchangeable against a known tone PS");
    }
    for (uint32_t width : {960u, 1280u}) {
        MonoFixture f(width); f.applyEpic63521CameraWords();
        const auto out = flatSelectMonoFrame(f.input);
        check(out.selected() && out.renderWidth == width &&
              out.cameraHash == f.world[19].key.cameraHash &&
              std::memcmp(out.camera, f.rows, sizeof(out.camera)) == 0,
              "Epic 63521 changed raw words preserve HDR camera authority at both extents");
    }
    {   // The gate-2 review's rounded mapping: a 960x540 render at a
        // 1366x768 output is 0.703x per axis, unequal to 0.75 exactly -- the
        // lineage band admits it (exact aspect equality refused it before).
        MonoFixture f;
        f.input.outputWidth = 1366; f.input.outputHeight = 768;
        auto& ck = f.handoff[1].key;
        ck.width = 1366; ck.height = 768; ck.viewport[2] = 1366; ck.viewport[3] = 768;
        const auto out = flatSelectMonoFrame(f.input);
        check(out.selected() && out.renderWidth == 960 && out.renderHeight == 540 &&
              out.outputWidth == 1366 && out.outputHeight == 768,
            "a rounded render-to-output mapping selects through the lineage band");
    }
    reject([](auto& f) { f.input.outputWidth = 2560; f.input.outputHeight = 1440;
            auto& ck = f.handoff[1].key; ck.width = 2560; ck.height = 1440;
            ck.viewport[2] = 2560; ck.viewport[3] = 1440; },
        FlatMonoReason::InvalidTonePass,
        "a tone under half the output on an axis is outside the band");
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
    reject([](auto& f) { f.handoff[0].key.ps = 0x0BAD0BAD0BAD0BADull; }, FlatMonoReason::NoTonePass,
        "an unreviewed tone PS remains refused");
    reject([](auto& f) { f.handoff[0].key.vs = 0x0BAD0BAD0BAD0BADull; }, FlatMonoReason::NoTonePass,
        "an unreviewed tone VS remains refused");
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
    reject([](auto& f) {
        for (uint32_t i = 0; i < 7; ++i) f.world[i].key.ps = 0;
        f.world[8].key.ps = 0xA6070F9DD1CFB601ull;  // Captured DE54 PS still unqualified.
    }, FlatMonoReason::NoSupportedSource,
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
          !engine_velocity_family::supportedPair(0xDE545DC8EE4FBB87ull, 0xA6070F9DD1CFB601ull) &&
          engine_velocity_family::supportedPair(0xDE545DC8EE4FBB87ull, 0x91F8937EDA723663ull),
          "flat metadata admits PS91 but keeps unqualified captured pairs excluded");
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
        std::unique_ptr<FlatRuntimePrefix> beforeCopy, beforeTone;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& r = *events[i].r; FlatRuntimeDraw d{}; d.key = r.key;
            std::memcpy(d.camera, r.camera, sizeof(d.camera));
            d.key.writeEpoch = prefix->frame; d.key.writeSeq = prefix->sequence + 1;
            d.supported = engine_velocity_family::supportedPair(d.key.vs, d.key.ps);
            d.instances = r.firstInstances;
            if (i + 1 == count) beforeCopy = std::make_unique<FlatRuntimePrefix>(*prefix);
            if (events[i].r == &fixture.handoff[0]) beforeTone = std::make_unique<FlatRuntimePrefix>(*prefix);
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

        auto weaponWitness = std::make_unique<FlatRuntimePrefix>(*beforeTone);
        FlatRuntimeDraw weapon{}; weapon.key = fixture.world[19].key;
        std::memcpy(weapon.camera, fixture.world[19].camera, sizeof(weapon.camera));
        weapon.camera[20] ^= 1; weapon.key.cameraHash = flatCameraHash(weapon.camera);
        weapon.key.writeEpoch = weaponWitness->frame;
        weapon.key.writeSeq = weaponWitness->sequence + 1;
        weapon.key.vs = 0x88DCF1164C640EC3ull; weapon.key.ps = 0x494506A63091DF8Cull;
        flatRuntimeObserve(*weaponWitness, weapon);
        FlatRuntimeDraw toneAfter{}; toneAfter.key = fixture.handoff[0].key;
        std::memcpy(toneAfter.camera, fixture.handoff[0].camera, sizeof(toneAfter.camera));
        toneAfter.key.writeEpoch = weaponWitness->frame;
        toneAfter.key.writeSeq = weaponWitness->sequence + 1;
        toneAfter.instances = fixture.handoff[0].firstInstances;
        flatRuntimeObserve(*weaponWitness, toneAfter);
        check(flatRuntimeObserve(*weaponWitness, copy).selected() &&
              weaponWitness->selectedConflict.cause == FlatRuntimeConflict::None,
              "the qualified second-camera weapon pass neither vetoes nor owns the scene camera");

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
    {   // Epic 20260926_131921: the online model hardcoded the tone pass's
        // HDR input at PS1. The DoF composite (kToneDofCompositePs) and
        // bloom composite (kToneBloomCompositePs) bind their HDR at PS0 and
        // their own blur/bloom at PS1 (129F602B2A9CA439/8826CACC6382C78D);
        // treating that blur/bloom as the HDR refused every frame.
        auto blurWrite = [](MonoFixture& f, uint32_t q) {
            FlatContractRecord r;
            f.fill(r, kFlatContractScreen, 0x2B10, 26, q, q, 1,
                0x129F602B2A9CA439ull, 0x8826CACC6382C78Dull, 0, 0, false);
            r.key.width /= 2; r.key.height /= 2;
            r.key.viewport[2] /= 2; r.key.viewport[3] /= 2;
            r.key.depth = r.key.dsv = nullptr;
            r.key.depthWidth = r.key.depthHeight = r.key.depthFormat = 0;
            return r;
        };
        auto hdrAtPs0Slot = [](MonoFixture& f) {
            // The HDR moves to PS0; PS1 becomes the variant's blur/bloom with
            // its own tokens, so a wrong-slot read cannot pass as the HDR.
            auto& k = f.handoff[0].key;
            k.srvView[0] = MonoFixture::token(0x2602); k.srvResource[0] = MonoFixture::token(0x2600);
            k.srvView[1] = MonoFixture::token(0x2B12); k.srvResource[1] = MonoFixture::token(0x2B10);
        };
        auto replayTone = [&](MonoFixture& fixture, bool chain) {
            auto prefix = std::make_unique<FlatRuntimePrefix>();
            prefix->frame = fixture.input.frame; prefix->output = fixture.input.output;
            prefix->width = 1280; prefix->height = 720; prefix->format = 28;
            FlatContractRecord chainRecords[2]{};
            if (chain) {
                chainRecords[0] = blurWrite(fixture, fixture.handoff[0].first - 6);
                chainRecords[1] = blurWrite(fixture, fixture.handoff[0].first - 3);
            }
            struct Event { const FlatContractRecord* r; uint32_t q; } events[200]{};
            uint32_t count = 0;
            for (uint32_t i = 0; i < fixture.input.worldCount; ++i) {
                const auto& r = fixture.world[i];
                for (uint32_t n = 0; n < r.draws; ++n)
                    events[count++] = {&r, r.first + (r.last-r.first)*n/(r.draws>1?r.draws-1:1)};
            }
            if (chain) {
                events[count++] = {&chainRecords[0], chainRecords[0].first};
                events[count++] = {&chainRecords[1], chainRecords[1].first};
            }
            events[count++] = {&fixture.handoff[0], fixture.handoff[0].first};
            events[count++] = {&fixture.handoff[1], fixture.handoff[1].first};
            std::sort(events, events + count, [](const Event& a, const Event& b) { return a.q < b.q; });
            FlatMonoFrame selected{};
            for (uint32_t i = 0; i < count; ++i) {
                const auto& r = *events[i].r; FlatRuntimeDraw d{}; d.key = r.key;
                std::memcpy(d.camera, r.camera, sizeof(d.camera));
                d.key.writeEpoch = prefix->frame; d.key.writeSeq = prefix->sequence + 1;
                d.supported = engine_velocity_family::supportedPair(d.key.vs, d.key.ps);
                d.instances = r.firstInstances;
                selected = flatRuntimeObserve(*prefix, d);
            }
            return selected;
        };
        // Replays fixture.handoff[0] as each tone variant, at both render
        // extents, and checks the online model selects the HDR at token
        // 0x2600 rather than refusing or aggregating the blur/bloom.
        auto toneVariant = [&](const char* message, uint64_t vs, uint64_t ps, bool ps0, bool chain) {
            for (uint32_t width : {960u, 1280u}) {
                MonoFixture f(width);
                f.handoff[0].key.vs = vs; f.handoff[0].key.ps = ps;
                if (ps0) hdrAtPs0Slot(f);
                const auto out = replayTone(f, chain);
                check(out.selected() && out.hdr == MonoFixture::token(0x2600), message);
            }
        };
        toneVariant("stock tone selects its HDR at PS1",
            flat_mono_detail::kToneVs, flat_mono_detail::kTonePs, false, false);
        toneVariant("EDHM-grade control selects its HDR at PS1",
            flat_mono_detail::kToneVs, flat_mono_detail::kToneEdhmGradePs, false, false);
        toneVariant("DoF-composite tone selects its own PS0 HDR, not its PS1 blur",
            flat_mono_detail::kToneVs, flat_mono_detail::kToneDofCompositePs, true, false);
        toneVariant("DoF-composite tone selects its HDR through an active blur chain",
            flat_mono_detail::kToneVs, flat_mono_detail::kToneDofCompositePs, true, true);
        toneVariant("bloom-composite tone selects its own PS0 HDR, not its PS1 bloom",
            flat_mono_detail::kToneVsNoConst, flat_mono_detail::kToneBloomCompositePs, true, false);
        toneVariant("bloom-composite tone selects its HDR through an active bloom chain",
            flat_mono_detail::kToneVsNoConst, flat_mono_detail::kToneBloomCompositePs, true, true);
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
    auto replay = [](Scenario scenario, uint64_t imagePs = 0xFCFAD73924BF45B9ull) {
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
            source[i].key.ps = imagePs;
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
    for (uint64_t imagePs : {0xFCFAD73924BF45B9ull, 0x07B3F82100F29401ull}) {
        for (Scenario s : {AllInertAbsent, AllInertArbitrary, MixedInertFirst, MixedGeometryFirst}) {
            auto result = replay(s, imagePs);
            check(result.second.selected() && result.first->imageCopiesAccepted == 1 &&
                  result.first->imageCopiesRefused == 0,
                  "both exact image pairs qualify with absent/arbitrary b1 in either order");
        }
        for (Scenario s : {InertUnverified, InertWrongPair, InertDepth, InertViewport, InertExplicitWrite,
                           MixedStaleGeometry, MixedChangedGeometry}) {
            auto result = replay(s, imagePs);
            check(!result.second.selected() && result.first->imageCopiesAccepted == 0 &&
                  result.first->imageCopiesRefused == 1 &&
                  result.first->selectedConflict.cause == FlatRuntimeConflict::ImageCopySource,
                  "both exact image pairs retain verification, shader, depth and camera gates");
        }
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
        // The menu-scoped stale-slot policy asks exactly this: the selected HDR is the copy's inherited
        // destination. The copy's SOURCE (0x2800, an ordinary scene HDR) and no pointer at all are not.
        check(flatFrameThroughMenuCopy(*result.first, result.second.hdr),
              "a frame selected through a verified menu copy qualifies for the menu's stale-slot policy");
        check(!flatFrameThroughMenuCopy(*result.first, MonoFixture::token(0x2800)) &&
              !flatFrameThroughMenuCopy(*result.first, nullptr),
              "the copy's source HDR and a null HDR do not qualify: it is the inherited destination that does");
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
        check(!flatFrameThroughMenuCopy(*result.first, result.second.hdr) &&
              !flatFrameThroughMenuCopy(*result.first, MonoFixture::token(0x2600)),
              "a refused menu copy never qualifies a frame for the menu's stale-slot policy");
    }
    auto overwritten = replay(PostCopyDestinationCompute);
    check(!overwritten.second.selected() && overwritten.first->menuCopiesAccepted == 1 &&
          overwritten.first->selectedConflict.cause == FlatRuntimeConflict::ExplicitWrite,
          "compute overwrite after accepted menu copy invalidates inherited HDR before tone");
    check(!flatFrameThroughMenuCopy(*overwritten.first, overwritten.second.hdr),
          "a frame whose inherited HDR was overwritten before tone is not selected, carries no HDR and does not qualify");
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

void testFrameContractTrace() {
    using namespace edvr;
    // Gate 1 (the staged program): the frame contract is produced by the same
    // reducer online and under trace replay, with identical decisions.
    for (uint32_t width : {960u, 1280u}) {
        MonoFixture fixture(width);
        auto prefix = std::make_unique<FlatRuntimePrefix>();
        prefix->frame = fixture.input.frame; prefix->output = fixture.input.output;
        prefix->width = 1280; prefix->height = 720; prefix->format = 28;
        auto ring = std::make_unique<FlatTraceRing>();
        flatTraceBeginFrame(*ring, prefix->frame, prefix->output, prefix->width, prefix->height, prefix->format);
        struct Event { const FlatContractRecord* r; uint32_t q; } events[200]{};
        uint32_t count = 0;
        for (uint32_t i = 0; i < fixture.input.worldCount; ++i) {
            const auto& r = fixture.world[i];
            for (uint32_t n = 0; n < r.draws; ++n) events[count++] = {&r, r.first + (r.last-r.first)*n/(r.draws>1?r.draws-1:1)};
        }
        events[count++] = {&fixture.handoff[0], fixture.handoff[0].first};
        events[count++] = {&fixture.handoff[1], fixture.handoff[1].first};
        std::sort(events, events + count, [](const Event& a, const Event& b) { return a.q < b.q; });
        auto isCopy = [](const FlatRuntimeDraw& d, const FlatRuntimePrefix& p) {
            return d.key.vs == flat_mono_detail::kCopyVs && d.key.ps == flat_mono_detail::kCopyPs &&
                d.key.color == p.output;
        };
        auto contract = std::make_unique<FlatFrameContract>();
        for (uint32_t i = 0; i < count; ++i) {
            const auto& r = *events[i].r;
            FlatRuntimeDraw d{}; d.key = r.key;
            std::memcpy(d.camera, r.camera, sizeof(d.camera));
            d.key.writeEpoch = prefix->frame; d.key.writeSeq = prefix->sequence + 1;
            d.supported = engine_velocity_family::supportedPair(d.key.vs, d.key.ps);
            d.instances = r.firstInstances;
            if (isCopy(d, *prefix))
                flatTraceResolve(*ring, flat_mono_detail::toneHdrInput(fixture.handoff[0].key),
                    fixture.handoff[0].key.vs, fixture.handoff[0].key.ps, prefix->sequence,
                    static_cast<uint32_t>(FlatMonoReason::Selected));
            if (isCopy(d, *prefix)) flatRuntimeObserveContract(*prefix, d, *contract);
            else flatRuntimeObserve(*prefix, d);
            flatTraceRecord(*ring, d, false);
        }
        flatTraceSeal(*ring, contract->produced, contract->produced ? flatFrameContractHash(*contract) : 0);
        // Exercise the non-draw kinds: recorded after the copy, they apply
        // post-contract on both sides and must not perturb the sealed hash.
        flatTraceMark(*ring, kFlatTraceEventWriteResource, MonoFixture::token(0xDEAD));
        flatTraceMark(*ring, kFlatTraceEventMarkUncertain, nullptr);
        // The present boundary: seal happened above; the next frame opens a
        // new slot, and the dump skips the in-flight (current) slot.
        flatTraceBeginFrame(*ring, prefix->frame + 1, prefix->output, prefix->width, prefix->height, prefix->format);
        check(contract->produced && contract->copiesUsed == 1 && contract->copies[0].selected(),
              "trace online run produces a selected frame contract");
        const uint64_t wantHash = flatFrameContractHash(*contract);
        std::vector<unsigned char> bytes;
        flatTraceDump(*ring, [&](const void* data, uint32_t n) {
            const auto* p = static_cast<const unsigned char*>(data);
            bytes.insert(bytes.end(), p, p + n); return n;
        });
        uint32_t framesReplayed = 0, framesMatched = 0, resolveMarkers = 0, copiesAfterResolve = 0;
        FlatRuntimePrefix replay{};
        FlatFrameContract rc{};
        FlatTraceFrameHeader cur{};
        auto finishFrame = [&]() {
            if (!cur.eventCount) return;
            ++framesReplayed;
            if (rc.produced == (cur.produced != 0) &&
                (!rc.produced || flatFrameContractHash(rc) == cur.contractHash)) ++framesMatched;
        };
        const bool parsed = flatTraceParse(bytes.data(), bytes.size(),
            [&](const FlatTraceFrameHeader& h) {
                finishFrame(); cur = h;
                replay = FlatRuntimePrefix{}; replay.frame = h.frame; replay.output = h.output;
                replay.width = h.width; replay.height = h.height; replay.format = h.format;
                rc = FlatFrameContract{};
            },
            [&](const FlatTraceEvent& e) {
                if (e.kind == kFlatTraceEventWriteResource) { flatRuntimeWritten(replay, e.key.color); return; }
                if (e.kind == kFlatTraceEventDispatchWritten) { flatRuntimeDispatchObserveWritten(replay, e.key.color); return; }
                if (e.kind == kFlatTraceEventMarkUncertain) { replay.uncertain = true; return; }
                if (e.kind == kFlatTraceEventCameraCapture) { ++replay.sequence; return; }
                if (e.kind == kFlatTraceEventResolve) { ++resolveMarkers; return; }
                FlatRuntimeDraw d = flatTraceEventToDraw(e);
                // The traced writeEpoch/writeSeq are the online-resolved
                // values; replaying them verbatim keeps the shared camera/
                // draw sequence counter's online interleaving intact.
                if (e.flags & kFlatTraceForeignWork) replay.uncertain = true;
                if (isCopy(d, replay)) {
                    if (resolveMarkers == 1) ++copiesAfterResolve;
                    flatRuntimeObserveContract(replay, d, rc);
                }
                else flatRuntimeObserve(replay, d);
            });
        finishFrame();
        check(parsed && framesReplayed == 1 && framesMatched == 1 && wantHash == cur.contractHash &&
              resolveMarkers == 1 && copiesAfterResolve == 1 && rc.copies[0].selected(),
              "resolve marker before final copy is parsed but does not change selected contract or draw order");
    }
}

// Committed corpus: every trace replays to its recorded contract hashes.
// The gate must not silently pass: a missing corpus, an unreadable entry,
// an empty directory or a manifest mismatch all fail the build.
//
// Its own function since 2026-09-29: with the round-trip block above it shared one
// stack frame (a MonoFixture, an event table, two replay prefixes and two contracts,
// each ~100 KB), and the classification below tipped that frame past the 1 MB
// default stack -- a stack overflow before the first line printed. The replay state
// lives on the heap here for the same reason.
void testFrameContractCorpus() {
    using namespace edvr;
    namespace fs = std::filesystem;
    const fs::path dir("tools/flat_temporal_test/traces");
    const fs::path manifestPath = dir / "manifest.txt";
    if (!fs::exists(dir) || !fs::exists(manifestPath)) {
        check(false, "trace corpus or its manifest is missing");
        return;
    }
    // The manifest pins the required scenarios: one "file frames scenario
    // [selected] [static]" per line, '#' for comments. Adding a file never replaces a
    // required one. A "selected" flag makes every frame of that trace replay to
    // a Selected mono frame (the trace was captured on a treated stretch and must
    // stay one), not merely to its recorded hash. A "static" flag says the trace is the
    // 3D main menu: every Selected frame of it came through the verified menu HDR copy
    // (FlatMonoResolveFrame::staticScene); and a trace WITHOUT it must have none, which
    // is the corpus holding "flag 0 everywhere else" on real flight, station and
    // on-foot frames.
    std::map<std::string, uint32_t> required;
    std::set<std::string> mustSelect, mustStatic;
    {
        std::ifstream mf(manifestPath);
        std::string line;
        while (std::getline(mf, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream iss(line);
            std::string name, scenario, flag;
            uint32_t frames = 0;
            if (!(iss >> name >> frames >> scenario) || !frames) {
                check(false, "trace corpus manifest line malformed");
                continue;
            }
            while (iss >> flag) {
                if (flag == "selected") mustSelect.insert(name);
                else if (flag == "static") mustStatic.insert(name);
                else check(false, "trace corpus manifest line carries an unknown flag");
            }
            required[name] = frames;
        }
    }
    check(!required.empty(), "trace corpus manifest names no traces");
    uint32_t files = 0, framesMatched = 0, framesTotal = 0;
    uint32_t menuSelected = 0, menuStatic = 0, otherSelected = 0, otherStatic = 0, menuFiles = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".bin") continue;
        std::ifstream file(entry.path(), std::ios::binary | std::ios::ate);
        if (!file) { check(false, "trace corpus entry unreadable"); continue; }
        std::vector<unsigned char> bytes(static_cast<size_t>(file.tellg()));
        file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!file) { check(false, "trace corpus entry unreadable"); continue; }
        ++files;
        const std::string name = entry.path().filename().string();
        auto wanted = required.find(name);
        check(wanted != required.end(), "trace corpus file is not in the manifest");
        auto replayHeap = std::make_unique<FlatRuntimePrefix>();
        auto contractHeap = std::make_unique<FlatFrameContract>();
        FlatRuntimePrefix& replay = *replayHeap;
        FlatFrameContract& rc = *contractHeap;
        // A frame's state is reset from a fresh heap object: `x = T{}` would build a ~100 KB temporary on the stack.
        auto freshReplay = [&]() { auto z = std::make_unique<FlatRuntimePrefix>(); replay = *z; };
        auto freshContract = [&]() { auto z = std::make_unique<FlatFrameContract>(); rc = *z; };
        FlatTraceFrameHeader cur{};
        uint32_t fileFrames = 0;
        const bool needSelected = mustSelect.count(name) != 0;
        const bool isMenu = mustStatic.count(name) != 0;
        menuFiles += isMenu;
        auto finish = [&]() {
            if (!cur.eventCount) return;
            ++framesTotal; ++fileFrames;
            if (rc.produced == (cur.produced != 0) &&
                (!rc.produced || flatFrameContractHash(rc) == cur.contractHash)) ++framesMatched;
            if (needSelected)
                check(rc.produced && rc.copiesUsed && rc.copies[0].selected(),
                      "a corpus trace marked selected replays every frame to a Selected mono frame");
            // The menu-scoped stale-slot policy's classification, on the frames a resolve would take.
            if (rc.produced && rc.copiesUsed && rc.copies[0].selected()) {
                const bool through = flatFrameThroughMenuCopy(replay, rc.copies[0].hdr);
                (isMenu ? menuSelected : otherSelected) += 1;
                (isMenu ? menuStatic : otherStatic) += through;
                check(through == isMenu,
                      isMenu ? "every Selected frame of a main-menu trace came through the verified menu HDR copy"
                             : "no Selected frame of a flight, station or on-foot trace came through the menu HDR copy");
            }
        };
        const bool parsed = flatTraceParse(bytes.data(), bytes.size(),
            [&](const FlatTraceFrameHeader& h) {
                finish(); cur = h;
                freshReplay(); replay.frame = h.frame; replay.output = h.output;
                replay.width = h.width; replay.height = h.height; replay.format = h.format;
                freshContract();
            },
            [&](const FlatTraceEvent& e) {
                if (e.kind == kFlatTraceEventWriteResource) { flatRuntimeWritten(replay, e.key.color); return; }
                if (e.kind == kFlatTraceEventDispatchWritten) { flatRuntimeDispatchObserveWritten(replay, e.key.color); return; }
                if (e.kind == kFlatTraceEventMarkUncertain) { replay.uncertain = true; return; }
                if (e.kind == kFlatTraceEventCameraCapture) { ++replay.sequence; return; }
                FlatRuntimeDraw d = flatTraceEventToDraw(e);
                if (e.flags & kFlatTraceForeignWork) replay.uncertain = true;
                const bool copy = d.key.vs == flat_mono_detail::kCopyVs &&
                    d.key.ps == flat_mono_detail::kCopyPs && d.key.color == replay.output;
                if (copy) flatRuntimeObserveContract(replay, d, rc); else flatRuntimeObserve(replay, d);
            });
        finish();
        check(parsed, "trace corpus file parses");
        if (wanted != required.end()) {
            check(wanted->second == fileFrames, "trace corpus frame count matches the manifest");
            required.erase(wanted);
        }
    }
    check(required.empty(), "manifest scenario missing from the corpus");
    check(files && framesTotal && framesMatched == framesTotal,
          "trace corpus replays to the recorded frame contracts");
    std::printf("frame-contract corpus: %u file(s), %u/%u frames replay identical\n",
                files, framesMatched, framesTotal);
    // Both sides of the classification must be populated, or "flag 0 elsewhere" proves nothing.
    check(menuFiles && menuSelected && otherSelected,
          "the corpus holds Selected frames both in the main menu and outside it");
    std::printf("menu-scoped stale-slot policy: %u/%u Selected frames of %u main-menu trace(s) qualify; "
                "%u/%u Selected frames of the other traces do\n",
                menuStatic, menuSelected, menuFiles, otherStatic, otherSelected);
}

// reviews/flat-temporal-main-review-2026-09-26.md, G1-1: run one MonoFixture
// stream through the reducer's contract wrapper with optional mutations.
static std::unique_ptr<edvr::FlatFrameContract> runContractStream(uint32_t width, bool dropTone,
        bool dropCopy, bool uncertainBeforeCopy, bool duplicateCopy) {
    using namespace edvr;
    MonoFixture fixture(width);
    auto prefix = std::make_unique<FlatRuntimePrefix>();
    prefix->frame = fixture.input.frame; prefix->output = fixture.input.output;
    prefix->width = 1280; prefix->height = 720; prefix->format = 28;
    struct Event { const FlatContractRecord* r; uint32_t q; } events[200]{};
    uint32_t count = 0;
    for (uint32_t i = 0; i < fixture.input.worldCount; ++i) {
        const auto& r = fixture.world[i];
        for (uint32_t n = 0; n < r.draws; ++n) events[count++] = {&r, r.first + (r.last-r.first)*n/(r.draws>1?r.draws-1:1)};
    }
    if (!dropTone) events[count++] = {&fixture.handoff[0], fixture.handoff[0].first};
    if (!dropCopy) events[count++] = {&fixture.handoff[1], fixture.handoff[1].first};
    std::sort(events, events + count, [](const Event& a, const Event& b) { return a.q < b.q; });
    auto contract = std::make_unique<FlatFrameContract>();
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = *events[i].r;
        FlatRuntimeDraw d{}; d.key = r.key;
        std::memcpy(d.camera, r.camera, sizeof(d.camera));
        d.key.writeEpoch = prefix->frame; d.key.writeSeq = prefix->sequence + 1;
        d.supported = engine_velocity_family::supportedPair(d.key.vs, d.key.ps);
        d.instances = r.firstInstances;
        const bool isCopy = d.key.vs == flat_mono_detail::kCopyVs &&
            d.key.ps == flat_mono_detail::kCopyPs && d.key.color == prefix->output;
        if (!isCopy) { flatRuntimeObserve(*prefix, d); continue; }
        if (uncertainBeforeCopy) prefix->uncertain = true;
        flatRuntimeObserveContract(*prefix, d, *contract);
        if (duplicateCopy) flatRuntimeObserveContract(*prefix, d, *contract);
    }
    return contract;
}

void testFrameContractOutcomes() {
    using namespace edvr;
    // G1-1: every copy outcome is recorded and hashed, including the early
    // refusals that assemble no fixture records.
    const auto baseline = runContractStream(960, false, false, false, false);
    check(baseline->produced && baseline->copiesUsed == 1 &&
          baseline->copies[0].selected() && baseline->recordCount != 0 &&
          flatFrameContractHash(*baseline) != 0,
          "a selected copy produces a hashed contract");
    const auto noTone = runContractStream(960, true, false, false, false);
    check(noTone->produced && noTone->copiesUsed == 1 &&
          noTone->copies[0].reason == FlatMonoReason::NoTonePass &&
          noTone->recordCount == 0 && flatFrameContractHash(*noTone) != 0 &&
          flatFrameContractHash(*noTone) != flatFrameContractHash(*baseline),
          "a missing tone pass records and hashes its refusal");
    const auto uncertain = runContractStream(960, false, false, true, false);
    check(uncertain->copiesUsed == 1 &&
          uncertain->copies[0].reason == FlatMonoReason::Truncated &&
          flatFrameContractHash(*uncertain) != flatFrameContractHash(*baseline),
          "uncertain input records and hashes its refusal");
    const auto duplicate = runContractStream(960, false, false, false, true);
    check(duplicate->produced && duplicate->copiesUsed == 2 &&
          duplicate->copies[0].selected() &&
          duplicate->copies[1].reason == FlatMonoReason::Truncated &&
          duplicate->recordCount != 0 &&
          flatFrameContractHash(*duplicate) != flatFrameContractHash(*baseline),
          "a duplicate copy keeps the first outcome's records and hashes both");
    const auto noCopy = runContractStream(960, false, true, false, false);
    check(!noCopy->produced && !noCopy->copiesUsed,
          "a frame without a copy produces no contract");
}

void testFrameContractHashCoverage() {
    using namespace edvr;
    // G1-2: the hash covers the full semantic selection and fixture fields.
    const auto base = runContractStream(960, false, false, false, false);
    const uint64_t want = flatFrameContractHash(*base);
    auto mutated = *base;
    mutated.copies[0].camera[0][0] += 1.0f;
    check(flatFrameContractHash(mutated) != want,
          "a selected camera row mutation changes the hash");
    mutated = *base; mutated.copies[0].outputWidth ^= 1;
    check(flatFrameContractHash(mutated) != want,
          "a selected output extent mutation changes the hash");
    mutated = *base; mutated.records[0].firstInstances ^= 1;
    check(flatFrameContractHash(mutated) != want,
          "an aggregate instance-count mutation changes the hash");
    mutated = *base; mutated.copies[0].reason = FlatMonoReason::Truncated;
    check(flatFrameContractHash(mutated) != want,
          "a selection outcome mutation changes the hash");
}


// Diagnostic: replay one trace file frame by frame, printing the stored and
// replayed contract hashes and the replayed selection's shape per frame.
// Nonzero exit on any mismatch: the diagnostic must not read green on red.
int flatTraceCheck(const char* path) {
    using namespace edvr;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) { std::printf("cannot read %s\n", path); return 2; }
    std::vector<unsigned char> bytes(static_cast<size_t>(file.tellg()));
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    FlatRuntimePrefix replay{};
    FlatFrameContract rc{};
    FlatTraceFrameHeader cur{};
    uint32_t draws = 0, markers = 0, mismatches = 0;
    auto finish = [&]() {
        if (!cur.eventCount) return;
        const uint64_t got = rc.produced ? flatFrameContractHash(rc) : 0;
        const bool match = rc.produced == (cur.produced != 0) && (!rc.produced || got == cur.contractHash);
        if (!match) ++mismatches;
        std::printf("frame %llu: events=%u draws=%u markers=%u produced(stored=%u replay=%u) hash(stored=%016llx replay=%016llx) reason=%s records=%u%s\n",
            (unsigned long long)cur.frame, cur.eventCount, draws, markers, cur.produced, rc.produced ? 1u : 0u,
            (unsigned long long)cur.contractHash, (unsigned long long)got,
            flatMonoReasonName(rc.copiesUsed ? rc.copies[0].reason : FlatMonoReason::InvalidInput),
            rc.recordCount, match ? "" : "  MISMATCH");
        cur = FlatTraceFrameHeader{};
    };
    const bool parsed = flatTraceParse(bytes.data(), bytes.size(),
        [&](const FlatTraceFrameHeader& h) {
            finish(); cur = h;
            replay = FlatRuntimePrefix{}; replay.frame = h.frame; replay.output = h.output;
            replay.width = h.width; replay.height = h.height; replay.format = h.format;
            rc = FlatFrameContract{}; draws = markers = 0;
        },
        [&](const FlatTraceEvent& e) {
            if (e.kind == kFlatTraceEventWriteResource) { flatRuntimeWritten(replay, e.key.color); ++markers; return; }
            if (e.kind == kFlatTraceEventDispatchWritten) { flatRuntimeDispatchObserveWritten(replay, e.key.color); ++markers; return; }
            if (e.kind == kFlatTraceEventMarkUncertain) { replay.uncertain = true; ++markers; return; }
            if (e.kind == kFlatTraceEventCameraCapture) { ++replay.sequence; ++markers; return; }
            if (e.kind == kFlatTraceEventResolve) { ++markers; return; }
            FlatRuntimeDraw d = flatTraceEventToDraw(e);
            if (e.flags & kFlatTraceForeignWork) replay.uncertain = true;
            const bool copy = d.key.vs == flat_mono_detail::kCopyVs &&
                d.key.ps == flat_mono_detail::kCopyPs && d.key.color == replay.output;
            if (copy) flatRuntimeObserveContract(replay, d, rc); else flatRuntimeObserve(replay, d);
            ++draws;
        });
    finish();
    if (!parsed) { std::printf("malformed trace %s\n", path); return 2; }
    if (mismatches) { std::printf("%u mismatched frame(s)\n", mismatches); return 1; }
    return 0;
}

void testFlatScreenBand() {
    using namespace edvr;
    // The lineage band (the gate-2 review's rounded/cropped table): a mapping
    // inside the band is admitted; sub-half chains stay excluded by the floor.
    struct Row { uint32_t w, h, ow, oh; bool screen; };
    const Row rows[] = {
        {1366,768, 1366,768, true},    // native
        {1708,960, 1366,768, true},    // rounded 1.25x
        {888,499, 1366,768, true},     // mild crop
        {320,180, 1280,720, false},    // under the per-axis floor
        {5760,3240, 3840,2160, true},  // 1.5x supersample
        {960,540, 3840,2160, false},   // quarter-res blur chain
        {1024,1024, 1280,720, false},  // square shadow-like target, not a mapping
        {888,540, 1366,768, false},    // non-uniform crop: waits for rectangle lineage
        {7680,4320, 3840,2160, true},  // 2x supersample, the cap
        {7681,4320, 3840,2160, false}, // past the cap
    };
    for (const auto& r : rows) {
        const auto kind = flatContractKind(false, reinterpret_cast<const void*>(1),
            reinterpret_cast<const void*>(2), r.w, r.h, 26, r.ow, r.oh, false);
        check((kind == kFlatContractScreen) == r.screen,
              "the screen band admits rounded scales and crops, not sub-half chains");
    }
}

void testFlatResolveRoute() {
    using namespace edvr;
    // Gate 2 discovery (section 72): today's effective route per size pairing,
    // honest refusals included. Mirrors the resolve's own predicates.
    struct Case { FlatMonoResolveMode mode; uint32_t rW, rH, dW, dH;
                  uint32_t eW, eH; bool refused; const char* name; };
    const Case cases[] = {
        // The flown stock pairing: SS 0.65 render upscaled by DLSS.
        {FlatMonoResolveMode::Dlss, 2496,1404, 3840,2160, 3840,2160, false, "trained-upscale"},
        {FlatMonoResolveMode::Dlss, 3840,2160, 3840,2160, 3840,2160, false, "trained-native"},
        {FlatMonoResolveMode::Fsr,  2496,1404, 3840,2160, 3840,2160, false, "trained-upscale"},
        // R > D (section 72 step 2): NVIDIA evaluates DLAA at R and the game's
        // copy downsamples E = R to D; FSR mirrors it via Native AA at 1.0x.
        {FlatMonoResolveMode::Dlss, 5760,3240, 3840,2160, 5760,3240, false, "dlss-as-dlaa-supersample"},
        {FlatMonoResolveMode::Dlaa, 5760,3240, 3840,2160, 5760,3240, false, "dlaa-supersample"},
        {FlatMonoResolveMode::Fsr,  5760,3240, 3840,2160, 5760,3240, false, "fsr-native-aa-supersample"},
        {FlatMonoResolveMode::Taa,  5760,3240, 3840,2160, 3840,2160, false, "taa-display-grid-down"},
        {FlatMonoResolveMode::Dlaa, 3840,2160, 3840,2160, 3840,2160, false, "dlaa-native"},
        {FlatMonoResolveMode::Taa,  3840,2160, 3840,2160, 3840,2160, false, "taa-native"},
        {FlatMonoResolveMode::Taa,  2496,1404, 3840,2160, 3840,2160, false, "taa-display-grid-up"},
        // DLAA never upscales; mixed axes route NVIDIA to the supersample path.
        {FlatMonoResolveMode::Dlaa, 2496,1404, 3840,2160, 0,0, true, "dlaa-requires-native"},
        {FlatMonoResolveMode::Dlss, 3000,2160, 3840,1404, 3000,2160, false, "dlss-as-dlaa-supersample"},
        {FlatMonoResolveMode::Taa,  3000,2160, 3840,1404, 3840,1404, false, "taa-display-grid-down"},
    };
    for (const auto& c : cases) {
        const auto route = flatResolveRoute(c.mode, c.rW, c.rH, c.dW, c.dH);
        check(route.refused == c.refused && route.evalWidth == c.eW && route.evalHeight == c.eH &&
              std::strcmp(route.name, c.name) == 0, "resolve route names the effective treatment honestly");
    }
    check(std::strcmp(flatResolveRoute(FlatMonoResolveMode::Dlaa, 2496, 1404, 3840, 2160).failReason,
                      "flat-dlaa-requires-native-render-size") == 0,
          "the DLAA upscale refusal's log token is stable");
    check(flatResolveRoute(FlatMonoResolveMode::Taa, 0, 2160, 3840, 2160).refused &&
          flatResolveRoute(FlatMonoResolveMode::Dlss, 3840, 2160, 3840, 0).refused,
          "a zero on any axis refuses the route");
}

// Expectation regeneration (the review's path: same saved events, unchanged
// selector, no new flight): replay every trace in a directory with the
// current reducer and hash schema, rewriting each frame header's stored
// contract hash in place.
int flatTraceMigrate(const char* dirPath) {
    using namespace edvr;
    namespace fs = std::filesystem;
    uint32_t files = 0, failed = 0;
    for (const auto& entry : fs::directory_iterator(dirPath)) {
        if (entry.path().extension() != ".bin") continue;
        std::ifstream file(entry.path(), std::ios::binary | std::ios::ate);
        if (!file) { std::printf("migrate: cannot read %s\n", entry.path().string().c_str()); ++failed; continue; }
        std::vector<unsigned char> bytes(static_cast<size_t>(file.tellg()));
        file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!file || bytes.size() < sizeof(FlatTraceHeader)) { ++failed; continue; }
        FlatTraceHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        // Input may be EDVRFTR2 (same event layout, old hash schema); the
        // output is always EDVRFTR3 with the current schema's hashes.
        if (std::memcmp(header.magic, "EDVRFTR3", 8) != 0 &&
            std::memcmp(header.magic, "EDVRFTR2", 8) != 0) {
            std::printf("migrate: %s is not EDVRFTR2/3\n", entry.path().string().c_str()); ++failed; continue;
        }
        size_t at = sizeof(FlatTraceHeader);
        bool ok = true;
        for (uint32_t f = 0; f < header.frameCount && ok; ++f) {
            if (bytes.size() - at < sizeof(FlatTraceFrameHeader)) { ok = false; break; }
            const size_t headerAt = at;
            FlatTraceFrameHeader fh{};
            std::memcpy(&fh, bytes.data() + at, sizeof(fh));
            at += sizeof(fh);
            // The corpus files are EDVRFTR3: the event layout stays the V3 one here, and the rewritten file stays FTR3.
            if (!fh.eventCount || fh.eventCount > kFlatTraceEventsPerFrame ||
                bytes.size() - at < fh.eventCount * sizeof(FlatTraceEventV3)) { ok = false; break; }
            FlatRuntimePrefix replay{};
            replay.frame = fh.frame; replay.output = fh.output;
            replay.width = fh.width; replay.height = fh.height; replay.format = fh.format;
            FlatFrameContract rc{};
            for (uint32_t i = 0; i < fh.eventCount; ++i) {
                FlatTraceEventV3 v3{};
                std::memcpy(&v3, bytes.data() + at, sizeof(v3)); at += sizeof(v3);
                const FlatTraceEvent e = flatTraceEventFromV3(v3);
                if (e.kind == kFlatTraceEventWriteResource) { flatRuntimeWritten(replay, e.key.color); continue; }
                if (e.kind == kFlatTraceEventDispatchWritten) { flatRuntimeDispatchObserveWritten(replay, e.key.color); continue; }
                if (e.kind == kFlatTraceEventMarkUncertain) { replay.uncertain = true; continue; }
                if (e.kind == kFlatTraceEventCameraCapture) { ++replay.sequence; continue; }
                FlatRuntimeDraw d = flatTraceEventToDraw(e);
                if (e.flags & kFlatTraceForeignWork) replay.uncertain = true;
                const bool copy = d.key.vs == flat_mono_detail::kCopyVs &&
                    d.key.ps == flat_mono_detail::kCopyPs && d.key.color == replay.output;
                if (copy) flatRuntimeObserveContract(replay, d, rc); else flatRuntimeObserve(replay, d);
            }
            fh.produced = rc.produced ? 1u : 0u;
            fh.contractHash = rc.produced ? flatFrameContractHash(rc) : 0;
            std::memcpy(bytes.data() + headerAt, &fh, sizeof(fh));
        }
        if (!ok || at != bytes.size()) { std::printf("migrate: %s malformed\n", entry.path().string().c_str()); ++failed; continue; }
        header.magic[7] = '3';
        std::memcpy(bytes.data(), &header, sizeof(header));
        std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!out) { std::printf("migrate: cannot write %s\n", entry.path().string().c_str()); ++failed; continue; }
        std::printf("migrate: %s rehashed (%u frames)\n", entry.path().string().c_str(), header.frameCount);
        ++files;
    }
    if (!files && !failed) { std::printf("migrate: no traces in %s\n", dirPath); return 2; }
    return failed ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Re-key replay (2026-09-29, the Krait's hull pair). The corpus replays each
// frame's RECORDED pool-family decision -- the kFlatTraceSupported flag and
// key.kind -- which is what keeps a stored hash independent of the keyed table,
// and what makes --trace-migrate a no-op when the table grows. To ask "what would
// the frame contract be under table X" the decision has to be taken again:
// supported from X, kind from flatContractKind with the arguments
// flat_runtime.cpp passes per draw. A null table replays the recorded decisions.
// ---------------------------------------------------------------------------
namespace {
constexpr uint64_t kHullVs = 0x66DE2CADB1F4AE6Bull, kHullPs = 0x235567BE2840B3EDull;
bool tableWithoutHullPair(uint64_t vs, uint64_t ps) {
    return !(vs == kHullVs && ps == kHullPs) && edvr::engine_velocity_family::supportedPair(vs, ps);
}
bool tableNow(uint64_t vs, uint64_t ps) { return edvr::engine_velocity_family::supportedPair(vs, ps); }
struct RekeyFrame {
    uint64_t frame = 0, storedHash = 0, hash = 0;
    bool storedProduced = false, produced = false;
    edvr::FlatMonoReason reason = edvr::FlatMonoReason::InvalidInput;
    uint32_t supportedDraws = 0, recordCount = 0, hullDraws = 0;
    // Against the recorded decisions, per draw: the table grew (recorded false,
    // now true), the table dropped a pair (recorded true, now false), or the
    // supported flag agrees and flatContractKind still gives another kind.
    uint32_t grew = 0, dropped = 0, kindDiffers = 0;
};
bool rekeyReplay(const std::vector<unsigned char>& bytes, bool (*table)(uint64_t, uint64_t),
                 std::vector<RekeyFrame>& out) {
    using namespace edvr;
    FlatRuntimePrefix replay{};
    FlatFrameContract rc{};
    FlatTraceFrameHeader cur{};
    RekeyFrame row{};
    auto finish = [&]() {
        if (!cur.eventCount) return;
        row.frame = cur.frame; row.storedHash = cur.contractHash; row.storedProduced = cur.produced != 0;
        row.produced = rc.produced; row.hash = rc.produced ? flatFrameContractHash(rc) : 0;
        row.reason = rc.copiesUsed ? rc.copies[0].reason : FlatMonoReason::InvalidInput;
        row.supportedDraws = rc.copiesUsed ? rc.copies[0].supportedDraws : 0;
        row.recordCount = rc.recordCount;
        out.push_back(row); row = RekeyFrame{}; cur = FlatTraceFrameHeader{};
    };
    const bool parsed = flatTraceParse(bytes.data(), bytes.size(),
        [&](const FlatTraceFrameHeader& h) {
            finish(); cur = h;
            replay = FlatRuntimePrefix{}; replay.frame = h.frame; replay.output = h.output;
            replay.width = h.width; replay.height = h.height; replay.format = h.format;
            rc = FlatFrameContract{};
        },
        [&](const FlatTraceEvent& e) {
            if (e.kind == kFlatTraceEventWriteResource) { flatRuntimeWritten(replay, e.key.color); return; }
            if (e.kind == kFlatTraceEventDispatchWritten) { flatRuntimeDispatchObserveWritten(replay, e.key.color); return; }
            if (e.kind == kFlatTraceEventMarkUncertain) { replay.uncertain = true; return; }
            if (e.kind == kFlatTraceEventCameraCapture) { ++replay.sequence; return; }
            FlatRuntimeDraw d = flatTraceEventToDraw(e);
            if (table) {
                const bool recordedSupported = d.supported;
                const auto recordedKind = d.key.kind;
                d.supported = table(d.key.vs, d.key.ps);
                d.key.kind = flatContractKind(d.supported, d.key.color, d.key.depth, d.key.width, d.key.height,
                                              d.key.format, replay.width, replay.height, d.key.color == replay.output);
                if (d.supported && !recordedSupported) ++row.grew;
                else if (!d.supported && recordedSupported) ++row.dropped;
                else if (d.key.kind != recordedKind) ++row.kindDiffers;
            }
            if (d.key.vs == kHullVs && d.key.ps == kHullPs) ++row.hullDraws;
            if (e.flags & kFlatTraceForeignWork) replay.uncertain = true;
            const bool copy = d.key.vs == flat_mono_detail::kCopyVs &&
                d.key.ps == flat_mono_detail::kCopyPs && d.key.color == replay.output;
            if (copy) flatRuntimeObserveContract(replay, d, rc); else flatRuntimeObserve(replay, d);
        });
    finish();
    return parsed;
}
bool readTrace(const std::filesystem::path& path, std::vector<unsigned char>& bytes) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    bytes.assign(static_cast<size_t>(file.tellg()), 0);
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    return bool(file);
}
} // namespace

// Diagnostic: one trace file, or every trace in a directory, frame by frame under
// the recorded decisions, today's table without the hull pair (the control), and
// today's table. Prints what moved. Nonzero exit when the re-key itself is wrong:
// a pair the recording held supported that the table no longer does, a contract
// kind that differs where the supported flag agrees, or an outcome the hull pair
// changed. A control that differs from the recording because the table has grown
// since it was made is expected and only counted.
int flatTraceRekey(const char* path) {
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    if (fs::is_directory(path)) {
        for (const auto& entry : fs::directory_iterator(path))
            if (entry.path().extension() == ".bin") files.push_back(entry.path());
        std::sort(files.begin(), files.end());
    } else files.push_back(path);
    uint32_t frames = 0, moved = 0, refused = 0, controlBad = 0, olderTable = 0;
    for (const auto& file : files) {
        std::vector<unsigned char> bytes;
        std::vector<RekeyFrame> recorded, control, now;
        if (!readTrace(file, bytes) || !rekeyReplay(bytes, nullptr, recorded) ||
            !rekeyReplay(bytes, tableWithoutHullPair, control) || !rekeyReplay(bytes, tableNow, now) ||
            recorded.size() != control.size() || control.size() != now.size()) {
            std::printf("rekey: %s unreadable or malformed\n", file.string().c_str());
            return 2;
        }
        std::printf("%s\n", file.filename().string().c_str());
        for (size_t i = 0; i < now.size(); ++i) {
            ++frames;
            const bool controlOk = control[i].hash == recorded[i].hash && control[i].produced == recorded[i].produced;
            const bool same = now[i].hash == control[i].hash && now[i].reason == control[i].reason;
            if (!controlOk) ++olderTable;
            if (!same) ++moved;
            if (control[i].reason != now[i].reason) ++refused;
            std::printf("  frame %llu: stored=%016llx recorded=%016llx control=%016llx now=%016llx reason(control=%s now=%s) "
                        "supportedDraws(control=%u now=%u) hullDraws=%u vs-recorded(grew=%u dropped=%u kind=%u)%s%s\n",
                (unsigned long long)now[i].frame, (unsigned long long)recorded[i].storedHash,
                (unsigned long long)recorded[i].hash, (unsigned long long)control[i].hash,
                (unsigned long long)now[i].hash,
                edvr::flatMonoReasonName(control[i].reason), edvr::flatMonoReasonName(now[i].reason),
                control[i].supportedDraws, now[i].supportedDraws, now[i].hullDraws,
                control[i].grew, control[i].dropped, control[i].kindDiffers,
                same ? "" : "  MOVED", controlOk ? "" : "  (control differs from recorded: the table grew since)");
            if (control[i].dropped || control[i].kindDiffers) ++controlBad;
        }
    }
    std::printf("rekey: %u frame(s), %u moved by the hull pair, %u changed outcome, %u recorded under an older table, "
                "%u where the re-key disagrees with a recording in a way table growth cannot explain\n",
                frames, moved, refused, olderTable, controlBad);
    return (controlBad || refused) ? 1 : 0;
}

void testHullPairKeying() {
    using namespace edvr;
    namespace fs = std::filesystem;
    using engine_velocity_family::supportedPair;
    using engine_velocity_family::keyedPs;
    using engine_velocity_family::familyOfVs;
    // The table, exactly: the hull pair joined, its siblings and the deferred pairs did not.
    check(supportedPair(kHullVs, kHullPs), "the Krait hull pair 66DE2CAD/235567BE is keyed for flat");
    check(supportedPair(kHullVs, 0x864F1F949851B8DEull) && supportedPair(kHullVs, 0xBBDE4E71FB78528Aull),
          "the family's two earlier pixel shaders are still keyed");
    check(!supportedPair(0xAACFDCF2FB9AD809ull, 0xCAD1F585EDDC5641ull),
          "CAD1F585 (EDHM-patched, reads t120) is left for later: unkeyed");
    check(!supportedPair(0xBBE58E40FE88EC80ull, 0x7311054AB3AAE1DCull),
          "BBE58E40/7311054A (SV_Position input) is left for later: unkeyed");
    check(!supportedPair(kHullVs, 0x818212B5F404C002ull), "a third 66DE2CAD pixel shader stays unkeyed");
    // Flat only: the VR profiles never see it (keyedPs with flat = false).
    check(keyedPs(familyOfVs(kHullVs), 0x864F1F949851B8DEull, false) &&
          !keyedPs(familyOfVs(kHullVs), kHullPs, false),
          "VR does not key the hull pair: only the flat companion slot holds it");
    // The corpus, re-keyed: the pair may move contracts that hold its draws and nothing else.
    const fs::path dir("tools/flat_temporal_test/traces");
    uint32_t files = 0, frames = 0, movedFrames = 0, hullFrames = 0;
    bool kraitSeen = false;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".bin") continue;
        std::vector<unsigned char> bytes;
        std::vector<RekeyFrame> recorded, control, now;
        const bool ok = readTrace(entry.path(), bytes) && rekeyReplay(bytes, nullptr, recorded) &&
            rekeyReplay(bytes, tableWithoutHullPair, control) && rekeyReplay(bytes, tableNow, now) &&
            recorded.size() == control.size() && control.size() == now.size();
        check(ok, "re-key replay parses every corpus trace under all three decisions");
        if (!ok) continue;
        ++files;
        const bool krait = entry.path().filename() == "flat_trace_67594.bin";
        kraitSeen = kraitSeen || krait;
        for (size_t i = 0; i < now.size(); ++i) {
            ++frames;
            // The re-key's own soundness: a table that only grows never drops a pair a
            // recording held supported, and flatContractKind reproduces the recorded kind
            // wherever the supported flag agrees. What the control may differ in is the
            // draws a LATER table keyed (recordings from before the newest pairs).
            check(control[i].dropped == 0 && control[i].kindDiffers == 0,
                  "the re-key reproduces every recorded pool-family decision the table still makes");
            check(now[i].produced == control[i].produced && now[i].reason == control[i].reason,
                  "keying the hull pair never changes a frame's selection outcome");
            if (now[i].hullDraws == 0)
                check(now[i].hash == control[i].hash,
                      "a frame with no hull-pair draw is byte-identical under the new table");
            else {
                ++hullFrames;
                if (now[i].produced && now[i].reason == FlatMonoReason::Selected)
                    check(now[i].hash != control[i].hash &&
                          now[i].supportedDraws == control[i].supportedDraws + now[i].hullDraws,
                          "a selected frame's contract gains exactly its hull-pair draws as supported sources");
            }
            if (now[i].hash != control[i].hash) ++movedFrames;
            if (krait) {
                check(control[i].hash == recorded[i].storedHash && recorded[i].hash == recorded[i].storedHash,
                      "the Krait trace: the control (today's table without the pair) reproduces the stored hash");
                check(now[i].reason == FlatMonoReason::Selected && now[i].hullDraws == 5,
                      "the Krait trace stays Selected under the new table, with its five hull-pair draws");
                check(now[i].hash != recorded[i].storedHash,
                      "the Krait trace: the new table moves the contract (negative control: the control does not)");
            }
        }
    }
    check(kraitSeen, "the Krait trace (flat_trace_67594.bin) is in the corpus");
    check(files && frames && hullFrames, "the corpus holds frames with hull-pair draws");
    std::printf("hull pair re-key: %u trace(s), %u frame(s), %u with hull-pair draws, %u contract(s) moved -- only those frames\n",
                files, frames, hullFrames, movedFrames);
}

void testFlatDlssNegotiate() {
    using namespace edvr;
    // Gate 2 step 4: the served-floor negotiation, with ranges scaled like
    // the flight's 0.5x-floor ladder at a 3840x2160 display.
    DlssModeRange modes[kDlssModeCount]{};
    auto range = [](DlssModeRange& m, unsigned ow, unsigned oh, unsigned minW, unsigned minH,
                    unsigned maxW, unsigned maxH) {
        m.ok = true; m.optW = ow; m.optH = oh; m.minW = minW; m.minH = minH;
        m.maxW = maxW; m.maxH = maxH;
    };
    range(modes[0], 2560, 1440, 1920, 1080, 3840, 2160);   // quality
    range(modes[1], 2225, 1252, 1920, 1080, 3840, 2160);   // balanced
    range(modes[2], 1920, 1080, 1920, 1080, 3840, 2160);   // performance
    range(modes[3], 1280,  720, 1280,  720, 1280,  720);   // ultra performance (a point)
    {
        const auto neg = flatDlssNegotiate(modes, 2496, 1404, 3840, 2160);
        check(neg.known && neg.served && !neg.cut && neg.evalWidth == 3840 && neg.evalHeight == 2160 &&
              neg.mode == DlssMode::Quality && neg.fromRange,
              "a served input evaluates at the door output with the named mode");
    }
    {
        const auto neg = flatDlssNegotiate(modes, 1280, 720, 3840, 2160);
        check(neg.served && !neg.cut && neg.mode == DlssMode::UltraPerformance,
              "ultra performance's single point serves exactly itself");
    }
    {
        const auto neg = flatDlssNegotiate(modes, 1500, 1000, 3840, 2160);
        check(neg.known && neg.served && neg.cut && neg.evalWidth == 3000 && neg.evalHeight == 2000 &&
              neg.mode == DlssMode::Quality,
              "an under-floor input cuts the evaluation to the floor it reaches");
    }
    {
        const auto neg = flatDlssNegotiate(modes, 1000, 1000, 3840, 2160);
        check(neg.known && neg.served && neg.cut && neg.evalWidth == 2000 && neg.evalHeight == 2000,
              "the cut scales with how far under the floor the input is");
    }
    {
        DlssModeRange none[kDlssModeCount]{};
        const auto neg = flatDlssNegotiate(none, 2496, 1404, 4074, 4076);
        check(!neg.known && !neg.served, "an unanswered vendor query is not a negotiation");
    }
}

// The menu-scoped stale-slot policy has three links: the classification (flatFrameThroughMenuCopy,
// held on the real corpus above), the resolver's cbuffer (held by the WARP fixture in
// flat_mono_resolve_test), and the two lines that join them, which no rig can reach because the
// flat runtime needs a game. Held here by a source scan of exactly those lines -- without them the
// policy would be built, tested and never on. The scan runs on a copy with each line removed
// first, so it is known to be able to fail.
void testStaticSceneWiring() {
    auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string runtimeCpp = slurp("src/d3d11/flat_runtime.cpp");
    const std::string resolveCpp = slurp("src/d3d11/flat_mono_resolve.cpp");
    check(!runtimeCpp.empty() && !resolveCpp.empty(), "the runtime and resolver sources are readable from the repo root");
    struct Link { const std::string* text; const char* needle; const char* what; };
    const Link links[] = {
        {&runtimeCpp, "f.staticScene=flatFrameThroughMenuCopy(s.prefix,selected.hdr);",
         "the flat runtime sets the resolve frame's staticScene from the selected frame's own prefix"},
        {&runtimeCpp, "if(f.staticScene)++s.staticSceneFrames;",
         "the flat runtime counts the frames it hands the resolver with the policy on"},
        {&runtimeCpp, "static-scene-frames=%llu",
         "the menu HDR copy line carries the static-scene-frames field"},
        {&resolveCpp, "constants.flags[3]=f.staticScene?1u:(depthCheck?2u:0u);",
         "the resolver hands the frame's staticScene to the shader as flags.w (1), the steady-detail depth check's frame as 2 behind it, else 0"},
    };
    for (const Link& link : links) {
        const size_t at = link.text->find(link.needle);
        check(at != std::string::npos && link.text->find(link.needle, at + 1) == std::string::npos, link.what);
        // Control: with the line removed the same scan reports it missing.
        std::string without = *link.text;
        if (at != std::string::npos) without.erase(at, std::strlen(link.needle));
        check(without.find(link.needle) == std::string::npos,
              "static scene wiring control: a source with the line removed no longer contains it");
    }
    // The runtime's assignment sits in the frame-building scope, before the resolve call it feeds.
    const size_t assign = runtimeCpp.find("f.staticScene=flatFrameThroughMenuCopy(");
    const size_t resolve = runtimeCpp.find("flatMonoResolve(s.device.Get(), ctx, f,");
    check(assign != std::string::npos && resolve != std::string::npos && assign < resolve,
          "the staticScene assignment precedes the resolve call that reads it");
}

// Replays a MonoFixture frame (plus `extra` records between the tone pass and the copy) through
// the online prefix model, as flatRuntimePrefixTests does, and returns the copy draw's verdict.
edvr::FlatMonoFrame standDownReplay(MonoFixture& fixture, const edvr::FlatContractRecord* extra, uint32_t extraCount) {
    using namespace edvr;
    auto prefix = std::make_unique<FlatRuntimePrefix>();
    prefix->frame = fixture.input.frame; prefix->output = fixture.input.output;
    prefix->width = 1280; prefix->height = 720; prefix->format = 28;
    struct Event { const FlatContractRecord* r; uint32_t q; } events[240]{};
    uint32_t count = 0;
    for (uint32_t i = 0; i < fixture.input.worldCount; ++i) {
        const auto& r = fixture.world[i];
        for (uint32_t n = 0; n < r.draws; ++n)
            events[count++] = {&r, r.first + (r.last-r.first)*n/(r.draws>1?r.draws-1:1)};
    }
    for (uint32_t i = 0; i < extraCount; ++i) events[count++] = {&extra[i], extra[i].first};
    events[count++] = {&fixture.handoff[0], fixture.handoff[0].first};
    events[count++] = {&fixture.handoff[1], fixture.handoff[1].first};
    std::sort(events, events + count, [](const Event& a, const Event& b) { return a.q < b.q; });
    FlatMonoFrame selected{};
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = *events[i].r; FlatRuntimeDraw d{}; d.key = r.key;
        std::memcpy(d.camera, r.camera, sizeof(d.camera));
        d.key.writeEpoch = prefix->frame; d.key.writeSeq = prefix->sequence + 1;
        d.supported = engine_velocity_family::supportedPair(d.key.vs, d.key.ps);
        d.instances = r.firstInstances;
        selected = flatRuntimeObserve(*prefix, d);
    }
    return selected;
}

// The stand-down against real chains: the captured frame that selects, and the two rc.4 users'
// refused chains (section 79) built from the same fixture. The verdicts come from the online
// prefix model, the very function the runtime's copy draw calls; the machine is driven with them
// at 60 fps on a mock clock.
void testStandDownAgainstModel() {
    using namespace edvr;
    using stand_down_test::Sim;
    auto verdictOf = [](const FlatMonoFrame& f) { return flatFrameSeenFor(f.selected(), f.reason); };

    MonoFixture stock(1280);
    const FlatMonoFrame ok = standDownReplay(stock, nullptr, 0);
    check(ok.selected() && verdictOf(ok) == FlatFrameSeen::Treatable,
          "the captured stock chain selects and is treatable");

    // User 1 (4K, game AA on): the tone pass has a PS the selector has never seen, and two plain
    // image passes sit between it and the copy, the second a format-27 target the copy reads.
    MonoFixture user1(1280);
    user1.handoff[0].key.ps = 0x6E83D02E7422C5BAull;
    FlatContractRecord passes1[2]{};
    user1.fill(passes1[0], kFlatContractScreen, 0x2710, 27, 938, 938, 1,
               0x03D186CE0EC031E3ull, 0xBAB75803059C271Dull, 0, 0, false);
    user1.fill(passes1[1], kFlatContractScreen, 0x2720, 27, 939, 939, 1,
               0x98E6F9986FDC9A53ull, 0x4168985B52C5D7C4ull, 0, 0, false);
    user1.handoff[1].key.srvView[0] = MonoFixture::token(0x2722);
    user1.handoff[1].key.srvResource[0] = MonoFixture::token(0x2720);
    const FlatMonoFrame refused1 = standDownReplay(user1, passes1, 2);
    check(!refused1.selected() && refused1.reason == FlatMonoReason::NoTonePass &&
          verdictOf(refused1) == FlatFrameSeen::Structural,
          "user 1's chain (new tone PS, two passes before the copy) is refused for no-known-tone-pass: structural");

    // User 2 (EDHM chained, bloom and DoF on): a KNOWN DoF-composite tone pass, then two passes on
    // the copy's shared VS, the second a format-27 target the copy reads.
    MonoFixture user2(1280);
    user2.handoff[0].key.ps = flat_mono_detail::kToneDofCompositePs;
    user2.handoff[0].key.srvView[0] = MonoFixture::token(0x2602);
    user2.handoff[0].key.srvResource[0] = MonoFixture::token(0x2600);
    user2.handoff[0].key.srvView[1] = MonoFixture::token(0x2B12);
    user2.handoff[0].key.srvResource[1] = MonoFixture::token(0x2B10);
    FlatContractRecord passes2[2]{};
    user2.fill(passes2[0], kFlatContractScreen, 0x2710, 27, 938, 938, 1,
               0x20F383BBAC05C031ull, 0x5AA08A96E3C14B10ull, 0, 0, false);
    user2.fill(passes2[1], kFlatContractScreen, 0x2720, 27, 939, 939, 1,
               0x20F383BBAC05C031ull, 0x2375CCCCBBFE7A4Dull, 0, 0, false);
    user2.handoff[1].key.srvView[0] = MonoFixture::token(0x2722);
    user2.handoff[1].key.srvResource[0] = MonoFixture::token(0x2720);
    const FlatMonoFrame refused2 = standDownReplay(user2, passes2, 2);
    check(!refused2.selected() && refused2.reason == FlatMonoReason::NoTonePass &&
          verdictOf(refused2) == FlatFrameSeen::Structural,
          "user 2's chain (known tone pass, passes after it) is refused for no-known-tone-pass: structural");

    // A treated session: the selecting chain, 90 s at 60 fps, never leaves Full and never probes.
    {
        Sim sim;
        for (int i = 0; i < 60 * 90; ++i) sim.frame(verdictOf(ok), ok.reason);
        check(sim.machine.entries == 0 && sim.work == FlatWork::Full && sim.machine.probes == 0,
              "frames the selector selects never stand the runtime down");
    }
    // Supported, then an unsupported chain, then supported again -- the qualification lifecycle the
    // motion-CPU review asks the stand-down to survive (reviews/flat-motion-cpu-review-2026-09-29.md,
    // C2): nothing that pauses the work may wait for a treated frame to resume it, or warm-up
    // deadlocks. Treated for 10 s; refused every frame until it stands down and has probed for
    // 20 s; the chain becomes recognised again; after the resume the first frames are warm-up
    // frames (the selector selects but the resolve refuses, or the prefix is transiently
    // truncated), then treated frames: the runtime must never fall back into the stand-down.
    {
        Sim sim;
        for (int i = 0; i < 60 * 10; ++i) sim.frame(verdictOf(ok), ok.reason);
        check(sim.machine.entries == 0 && sim.work == FlatWork::Full, "supported: full, no stand-down");
        FlatStandDownEvent event = FlatStandDownEvent::None;
        while (event != FlatStandDownEvent::Entered) event = sim.frame(verdictOf(refused1), refused1.reason);
        const uint64_t enteredAt = sim.now;
        while (sim.now - enteredAt < 20000) sim.frame(verdictOf(refused1), refused1.reason);
        check(sim.machine.standing && sim.work != FlatWork::Full, "unsupported: stood down and probing");
        while (event != FlatStandDownEvent::Resumed) event = sim.frame(verdictOf(ok), ok.reason);
        check(!sim.machine.standing && sim.work == FlatWork::Full && sim.machine.entries == 1 && sim.machine.resumes == 1,
              "the chain becomes recognised again: the very next probe resumes, with no treated frame needed first");
        bool fellBack = false;
        for (int i = 0; i < 90; ++i) {   // warm-up: transient frames, then selected frames the resolve still refuses
            const auto e = i < 45 ? sim.frame(FlatFrameSeen::Transient, FlatMonoReason::Truncated)
                                  : sim.frame(verdictOf(ok), ok.reason);
            if (e != FlatStandDownEvent::None || sim.work != FlatWork::Full) fellBack = true;
        }
        for (int i = 0; i < 60 * 30; ++i) {   // then treated frames
            if (sim.frame(verdictOf(ok), ok.reason) != FlatStandDownEvent::None || sim.work != FlatWork::Full) fellBack = true;
        }
        check(!fellBack && sim.machine.entries == 1, "warm-up after the resume is never mistaken for a refusal: it stays full");
    }
    // Each user's session: refused every frame stands the work down after 5 s; the user turns the
    // setting off in game (the chain becomes the stock one) at an arbitrary moment; the probe
    // that sees it ends the stand-down within two seconds.
    for (const FlatMonoFrame* refused : {&refused1, &refused2}) {
        Sim sim;
        FlatStandDownEvent event = FlatStandDownEvent::None;
        while (event != FlatStandDownEvent::Entered)
            event = sim.frame(verdictOf(*refused), refused->reason);
        check(sim.machine.enteredReason == FlatMonoReason::NoTonePass && sim.work == FlatWork::Paused,
              "a refused chain stands the work down and names the reason");
        // 40 s stood down, probing on the cadence, every probe refused again.
        const uint64_t enteredAt = sim.now;
        while (sim.now - enteredAt < 40000) sim.frame(verdictOf(*refused), refused->reason);
        check(sim.machine.standing && sim.machine.probes >= 24 && sim.machine.probes <= 27,
              "40 s stood down is about 26 probes, each refused again");
        const uint64_t settingChangedAt = sim.now + 333;
        uint64_t resumedAt = 0;
        while (!resumedAt && sim.now < settingChangedAt + 6000) {
            const bool changed = sim.now >= settingChangedAt;
            const FlatMonoFrame& shown = changed ? ok : *refused;
            if (sim.frame(verdictOf(shown), shown.reason) == FlatStandDownEvent::Resumed) resumedAt = sim.now;
        }
        check(resumedAt && resumedAt - settingChangedAt <= 2000 && !sim.machine.standing && sim.work == FlatWork::Full,
              "turning the setting off in game resumes the work within two seconds");
    }
}

// The stand-down's wiring in the runtime and its neighbours, which no rig can run because the
// flat runtime needs a game: held by a source scan of the exact lines that gate each piece, the
// way testStaticSceneWiring holds the menu policy. Each needle is counted, and the count is
// checked against the same text with the needle removed, so the scan is known to be able to fail.
void testStandDownWiring() {
    auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string runtimeCpp = slurp("src/d3d11/flat_runtime.cpp");
    const std::string temporalCpp = slurp("src/d3d11/flat_temporal.cpp");
    const std::string temporalH = slurp("src/d3d11/flat_temporal.h");
    const std::string injectCpp = slurp("src/d3d11/flat_camera_inject.cpp");
    check(!runtimeCpp.empty() && !temporalCpp.empty() && !temporalH.empty() && !injectCpp.empty(),
          "the runtime, discovery and injector sources are readable from the repo root");
    auto count = [](const std::string& text, const std::string& needle) {
        unsigned n = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
        return n;
    };
    struct Pin { const std::string* text; const char* needle; unsigned times; const char* what; };
    const Pin pins[] = {
        // The mode of every frame, its verdict, and the Present that decides both.
        {&runtimeCpp, "standDownFrame(s, frame);", 1, "the Present runs the stand-down's frame boundary once"},
        {&runtimeCpp, "const FlatFrameSeen seen = flatFrameSeenFor(selected.selected(), selected.reason);", 1,
         "the copy draw records its verdict from the selector's own result"},
        {&runtimeCpp, "if (s.work == FlatWork::Probe) return;", 1, "a Probe frame ends after the contract observation"},
        // The per-draw and per-call pieces, Paused frames.
        {&runtimeCpp, "if (s.work == FlatWork::Paused) return;", 3, "draw scope, dispatch scope and flatRuntimeUnknown return in a Paused frame"},
        {&runtimeCpp, "if (!owner() || state().work == FlatWork::Paused) return;", 4,
         "Written, Uavs, Unmap and Update return after owner() in a Paused frame"},
        {&runtimeCpp, "if (!owner() || type == D3D11_MAP_READ || state().work == FlatWork::Paused) return;", 1,
         "Map returns after owner() in a Paused frame"},
        // The camera witness runs in Full frames only.
        {&runtimeCpp, "capture(*c, c->mapped); if (state().work == FlatWork::Full) cameraWitness(res);", 1,
         "Unmap's camera witness is Full-only"},
        {&runtimeCpp, "capture(*c, bytes); if (state().work == FlatWork::Full) cameraWitness(res);", 1,
         "Update's camera witness is Full-only"},
        // Legacy projection readiness, coverage, jitter preparation: released, and created in Full only.
        {&runtimeCpp, "if (s.projection) s.projection.reset();", 1, "the stand-down releases legacy projection readiness"},
        {&runtimeCpp, "if(wanted && !s.projection && s.work == FlatWork::Full) {", 1,
         "the Present creates legacy projection readiness in Full frames only"},
        // Engine motion, the camera hook, the discovery observers.
        {&runtimeCpp, "engineVelocityConfigure(enabled && !enginePausedThen);", 1, "the Present hands the pause to engine motion"},
        {&runtimeCpp, "engineVelocityConfigure(enabled && !s.enginePaused);", 1, "and again when the stand-down changed it"},
        {&runtimeCpp, "flatCameraInjectPause(next != FlatWork::Full);", 1, "the stand-down pauses the camera refresh hook"},
        {&runtimeCpp, "flatTemporalSetPaused(next == FlatWork::Paused);", 1, "the stand-down pauses the discovery observers on Paused frames"},
        // The trace ring keeps the last watched frames.
        {&runtimeCpp, "if (s.work != FlatWork::Paused) s.prefix = FlatRuntimePrefix{};", 1, "a Paused frame does not clear the prefix"},
        {&runtimeCpp, "if (s.work != FlatWork::Paused) {", 1, "a Paused frame neither rotates the trace ring nor resets its contract"},
        {&temporalH, "if (detail::g_flatTemporalPaused.load(std::memory_order_relaxed)) return false;", 1,
         "discovery observers see nothing while paused"},
        {&temporalCpp, "flatMonoReasonStructural(mono.reason)", 1, "the chain dump asks the shared structural-reason question"},
        {&injectCpp, "if (paused && !g_inject.injected.empty()) return;", 1,
         "the camera hook closes only when no camera holds an injected phase"},
        {&injectCpp, "if (g_inject.permanentlyDown.load(std::memory_order_acquire)) return;", 1,
         "a pause never reopens a hook that stood down for good"},
    };
    for (const Pin& pin : pins) {
        check(count(*pin.text, pin.needle) == pin.times, pin.what);
        // Control: with every occurrence removed the same scan finds none.
        std::string without = *pin.text;
        for (size_t at = without.find(pin.needle); at != std::string::npos; at = without.find(pin.needle))
            without.erase(at, std::strlen(pin.needle));
        check(count(without, pin.needle) == 0, "stand-down wiring control: a source with the line removed no longer contains it");
    }
    // The old eight-way reason list is gone from the discovery dump: one definition of "structural".
    check(count(temporalCpp, "mono.reason == FlatMonoReason::NoTonePass") == 0,
          "the chain dump no longer carries its own copy of the structural reasons");
    // ORDER in the draw scope. The Probe frame ends after the contract observation -- the menu copy
    // verification, the online prefix model and the selector all run for it -- and before every
    // piece of per-draw work the stand-down pauses.
    auto at = [&](const char* needle) { return runtimeCpp.find(needle); };
    const size_t menuVerify = at("d.menuHdrCopyVerified=verifyMenuHdrCopy(ctx,d);");
    const size_t observe = at("? flatRuntimeObserveContract(s.prefix, d, s.traceContract)");
    const size_t record = at("flatTraceRecord(s.traceRing, d, foreignWork.load(std::memory_order_acquire), hdrSrvKnown ? hdrSrv : nullptr);");
    const size_t verdict = at("const FlatFrameSeen seen = flatFrameSeenFor(");
    const size_t probeReturn = at("if (s.work == FlatWork::Probe) return;");
    check(menuVerify != std::string::npos && observe != std::string::npos && record != std::string::npos &&
          verdict != std::string::npos && probeReturn != std::string::npos &&
          menuVerify < observe && observe < record && record < verdict && verdict < probeReturn,
          "in the draw scope the menu copy verification, the model, the trace record and the verdict all precede the Probe return");
    const char* afterProbe[] = {
        "const bool sourceCandidate=", "engineVelocityNoteSource(", "++s.covSceneDraws;",
        "qualifyProjection(s,recipes,", "engineVelocityFlatBeginDraw(ctx, &gameHadTarget6);",
        "flatMonoResolve(s.device.Get(), ctx, f,", "projection.emplace(*projectionPlan);"};
    for (const char* needle : afterProbe) {
        const size_t where = at(needle);
        check(where != std::string::npos && probeReturn != std::string::npos && probeReturn < where,
              "coverage, source naming, substitution, jitter and the resolve all follow the Probe return");
    }
    // NO DEADLOCK: nothing that pauses or resumes the work reads whether a frame was treated or
    // accepted. The pause is decided by the chain verdict alone, so warm-up can always start.
    const size_t first = at("void applyWork(State& s, FlatWork next) {");
    const size_t last = at("bool flatRuntimeStructuralRefusal(");
    check(first != std::string::npos && last != std::string::npos && first < last,
          "the stand-down functions can be delimited in the runtime source");
    if (first != std::string::npos && last != std::string::npos && first < last) {
        const std::string body = runtimeCpp.substr(first, last - first);
        check(body.find("s.treated") == std::string::npos && body.find("s.accepted") == std::string::npos &&
              body.find("temporalAccepted") == std::string::npos && body.find("havePrevious") == std::string::npos &&
              body.find("previousAcceptedValid") == std::string::npos,
              "applyWork, endStandDown and standDownFrame never read whether a frame was treated: a pause cannot wait for one");
        check(body.find("applyWork(") != std::string::npos && body.find("wake(") != std::string::npos,
              "(control: the delimited text is the stand-down code)");
    }
}

// The F8 panel's settings warning in the panel and the runtime: shown only while the runtime says
// the work is stood down for the shape of a post chain whose output copy it found, Elite's files
// read when the panel opens, and the installer's log bundler sharing the folder's spelling. A
// source scan, the way the stand-down pins are, with the same removal controls.
void testFlatWarningWiring() {
    auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string menuCpp = slurp("src/d3d11/menu.cpp");
    const std::string runtimeCpp = slurp("src/d3d11/flat_runtime.cpp");
    const std::string bundleCpp = slurp("src/installer/logbundle.cpp");
    const std::string standdownH = slurp("src/d3d11/flat_standdown.h");
    const std::string settingsH = slurp("src/d3d11/flat_elite_settings.h");
    check(!menuCpp.empty() && !runtimeCpp.empty() && !bundleCpp.empty() && !standdownH.empty() && !settingsH.empty(),
          "the menu, runtime, stand-down policy, settings header and log bundler sources are readable from the repo root");
    auto count = [](const std::string& text, const std::string& needle) {
        unsigned n = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
        return n;
    };
    struct Pin { const std::string* text; const char* needle; unsigned times; const char* what; };
    const Pin pins[] = {
        {&menuCpp, "const bool refusing = flatRuntimeStructuralRefusal(&reason, &standing) &&", 1,
         "the warning follows the runtime's structural-refusal state and nothing else"},
        // The hold on a changed cause (flat_elite_settings.h, FlatWarnHold; the rule and its controls are in flat_elite_settings_tests.h).
        {&menuCpp, "if (!s.flatWarnHold.admit(refusing, s.flatWarnActive, s.flatWarnKey, key, now)) {", 1,
         "a cause that differs from the one on show waits out its hold: the helper is asked once a tick with the shown key, the computed "
         "one and the tick's own time"},
        {&menuCpp, "if (refusing == s.flatWarnActive && key == s.flatWarnKey) return;", 1,
         "and a tick with nothing to change still leaves before the switch"},
        {&menuCpp, "if (s.flatWarnHold.began() && s.flatWarnHeldLogged < kFlatWarnHeldLogMax) {", 1,
         "a hold that begins is logged once, within its own bound"},
        {&menuCpp, "flatFormatWarnHeldLog(held, sizeof(held), s.flatWarnCause, cause);", 1,
         "the line names the cause on show and the one waiting"},
        {&menuCpp, "Log::get().note(\"%s\", held);", 1, "and is written to the log: it is the one trace that the hold ran"},
        {&menuCpp, "char held[360];", 1, "in a buffer the longest such line fits (the rig formats the longest one against 360)"},
        {&menuCpp, "constexpr int kFlatWarnHeldLogMax = 8;", 1, "at most eight times a session, apart from the panel's own 24"},
        {&menuCpp, "flat settings warning: further held changes are not logged this session", 1,
         "and the bound says so (a line that is not shown, changed or hidden, the three the log's reader counts)"},
        {&settingsH, "constexpr uint64_t kFlatWarnHoldMs = 2000;", 1,
         "the hold is 2000 ms: more than one stand-down probe interval, under two"},
        {&menuCpp, "temporalModeEnabled(Config::get().requestedTemporalMode());", 1,
         "and only while a temporal mode is selected"},
        {&menuCpp, "if (runtimeFlatProfile() && s.flatWarnActive) {", 1, "the panel draws the warning only while it is active"},
        {&menuCpp, "if (c.lineCount < kMenuMaxLines) c.lines[c.lineCount++].style = kMenuNote;", 2,
         "the warning, and the wrapper note after it, are note lines below the rows"},
        {&menuCpp, "FlatWarnRuler ruler{c.capPx * 8 / 7};", 2,
         "wrapped with the panel's own ruler at the note face's em (the flat warning and the wrapper note: no VR note takes a line)"},
        {&menuCpp, "flatWarningTick(now);", 1, "the flat tick runs the warning"},
        {&menuCpp, "s.flatSettingsForce = true;", 1, "Elite's files are looked at when the panel opens"},
        {&menuCpp, "s.flatSettings.setFolder(flatEliteGraphicsFolder());", 1, "from %LOCALAPPDATA%, resolved once"},
        {&menuCpp, "if (s.flatWarnLogged >= kFlatWarnLogMax) return;", 1, "the state-change lines are bounded per session"},
        {&runtimeCpp, "publishRefusal(s, true);", 1, "the Present publishes the refusal state after the stand-down's verdict"},
        {&runtimeCpp, "publishRefusal(s, false);", 1, "and a wake clears it"},
        {&runtimeCpp, "bool flatRuntimeStructuralRefusal(const char** reasonName, bool* standingDown) {", 1,
         "the accessor the panel reads"},
        {&runtimeCpp, "if (warn && s.standDown.warningActive())", 1,
         "the runtime publishes the warning from the stand-down's own gate"},
        {&bundleCpp, "return edvr::eliteGraphicsFolderUnder(base);", 1, "the log bundler composes the folder through the shared header"},
        // The gate itself: the stand-down, for a reason that found an output copy, and no clock.
        {&standdownH, "bool warningActive() const { return standing && flatMonoReasonWarrantsWarning(standReason); }", 1,
         "the warning is on while stood down for a reason that found an output copy"},
        {&standdownH, "return flatMonoReasonStructural(reason) && reason != FlatMonoReason::NoOutputCopy &&\n"
                      "           reason != FlatMonoReason::NoScene;", 1,
         "and never for no-known-output-copy or no-3d-scene: a startup or loading frame has no final copy, or no scene"},
        {&standdownH, "standReason = runReason;", 1, "a stand-down starts with the reason that entered it"},
        {&standdownH, "standReason = reason;", 1, "and follows each probe frame's own finding"},
    };
    for (const Pin& pin : pins) {
        check(count(*pin.text, pin.needle) == pin.times, pin.what);
        std::string without = *pin.text;
        for (size_t at = without.find(pin.needle); at != std::string::npos; at = without.find(pin.needle))
            without.erase(at, std::strlen(pin.needle));
        check(count(without, pin.needle) == 0, "warning wiring control: a source with the line removed no longer contains it");
    }
    // ORDER of the hold on a changed cause. It is asked after the key is computed and before the comparison that leaves the tick:
    // asked on every tick, so a cause that comes back to the one on show drops the change that was waiting, and a held change
    // returns before the switch, so it neither replaces what is on show nor is logged as changed. Two controls: the same text with
    // the hold asked after the early return, and with the held branch falling through to the switch, must fail the check.
    {
        const char* keyLine = "const std::string key = refusing ? flatSettingsWarningKey(";
        const char* askLine = "if (!s.flatWarnHold.admit(refusing, s.flatWarnActive, s.flatWarnKey, key, now)) {";
        const char* sameLine = "if (refusing == s.flatWarnActive && key == s.flatWarnKey) return;";
        const char* switchLine = "s.flatWarnActive = refusing;";
        const auto holdOrderOk = [&](const std::string& text) {
            const size_t key = text.find(keyLine), ask = text.find(askLine), same = text.find(sameLine), sw = text.find(switchLine);
            if (key == std::string::npos || ask == std::string::npos || same == std::string::npos || sw == std::string::npos)
                return false;
            if (!(key < ask && ask < same && same < sw)) return false;
            return text.substr(ask, same - ask).find("\n        return;\n    }\n") != std::string::npos;
        };
        check(holdOrderOk(menuCpp),
              "the hold is asked after the key is computed and before the comparison that leaves the tick, and a held change returns before the switch");
        std::string late = menuCpp;
        const size_t ask = late.find(askLine), same = late.find(sameLine);
        if (ask != std::string::npos && same != std::string::npos && ask < same) {
            const std::string block = late.substr(ask, same - ask);
            late.erase(ask, same - ask);
            late.insert(late.find(sameLine) + std::strlen(sameLine), "\n    " + block);
        }
        check(late != menuCpp && !holdOrderOk(late), "(control) the order check fails with the hold asked after the early return");
        std::string falls = menuCpp;
        const std::string heldReturn = "\n        return;\n    }\n    if (refusing == s.flatWarnActive && key == s.flatWarnKey) return;";
        const size_t tail = falls.find(heldReturn);
        if (tail != std::string::npos) falls.erase(tail, std::strlen("\n        return;"));
        check(falls != menuCpp && !holdOrderOk(falls), "(control) the order check fails with the held branch falling through to the switch");
    }
    check(count(bundleCpp, "Frontier Developments") == 1,
          "the log bundler no longer spells the folder itself (its comment names it once)");
    // NO TIMER. The first version warned after a 2 s run of refusals and flickered across a
    // transition; the gate is the stand-down now, so the constant is gone and the publisher reads no clock.
    check(count(standdownH, "kFlatStandDownWarnMs") == 0 && count(runtimeCpp, "kFlatStandDownWarnMs") == 0 &&
          count(menuCpp, "kFlatStandDownWarnMs") == 0,
          "the warning has no timer of its own: its constant is gone from the policy, the runtime and the panel");
    const size_t publishFrom = runtimeCpp.find("void publishRefusal(const State& s, bool warn) {");
    const size_t publishTo = runtimeCpp.find("// --- Stand-down: what each mode does");
    check(publishFrom != std::string::npos && publishTo != std::string::npos && publishFrom < publishTo,
          "the publisher can be delimited in the runtime source");
    if (publishFrom != std::string::npos && publishTo != std::string::npos && publishFrom < publishTo) {
        const std::string publisher = runtimeCpp.substr(publishFrom, publishTo - publishFrom);
        check(publisher.find("GetTickCount64") == std::string::npos && publisher.find("nowMs") == std::string::npos &&
              publisher.find("warningActive()") != std::string::npos,
              "the publisher reads the stand-down's gate and no clock (control: it names warningActive)");
    }
}

// The CPU and GPU census's wiring (flat_cpu.h): which entry point carries which family's scope,
// where the once-a-frame tick sits, that it stops with the mode, and that nothing it measures is
// read by a decision. A source scan with removal controls, the way the stand-down pins are.
void testFlatCpuWiring() {
    auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string runtimeCpp = slurp("src/d3d11/flat_runtime.cpp");
    const std::string resolveCpp = slurp("src/d3d11/flat_mono_resolve.cpp");
    const std::string resolveH = slurp("src/d3d11/flat_mono_resolve.h");
    const std::string injectCpp = slurp("src/d3d11/flat_camera_inject.cpp");
    const std::string temporalCpp = slurp("src/d3d11/flat_temporal.cpp");
    const std::string engineCpp = slurp("src/d3d11/engine_velocity.cpp");
    const std::string engineH = slurp("src/d3d11/engine_velocity.h");
    const std::string cpuH = slurp("src/d3d11/flat_cpu.h");
    const std::string menuCpp = slurp("src/d3d11/menu.cpp");
    check(!runtimeCpp.empty() && !resolveCpp.empty() && !resolveH.empty() && !injectCpp.empty() && !temporalCpp.empty() &&
          !engineCpp.empty() && !engineH.empty() && !cpuH.empty() && !menuCpp.empty(),
          "the census's sources are readable from the repo root");
    auto count = [](const std::string& text, const std::string& needle) {
        unsigned n = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
        return n;
    };
    struct Pin { const std::string* text; const char* needle; unsigned times; const char* what; };
    const Pin pins[] = {
        // Every family the census names has its scope at the entry points that family stands for.
        {&runtimeCpp, "flatcpu::Scope shell(flatcpu::kOther);", 3, "the draw scope (both halves) and the dispatch scope time their own shells"},
        {&runtimeCpp, "flatcpu::kReduce", 2, "contract reduction times the reducer and the final copy's admission by structure"},
        {&runtimeCpp, "flatcpu::kCopyChecks", 2, "the exact-shader verifications and the F10 captures are timed"},
        {&runtimeCpp, "flatcpu::kCameraRows", 2, "the camera lookup and hash, and capture()"},
        {&runtimeCpp, "flatcpu::kTrace", 5, "every trace-ring copy is timed: capture, dispatch, write, record and the HDR route's resolve marker"},
        {&runtimeCpp, "flatcpu::kResource", 4, "Written, Map, Unmap and Update time their lookups"},
        {&runtimeCpp, "flatcpu::kCoverage", 1, "coverage classification"},
        {&runtimeCpp, "flatcpu::kProjection", 3, "qualifyProjection, the private binding and its restore"},
        {&runtimeCpp, "flatcpu::kShadows", 7, "the constant-buffer shadow observers and map-cache install/flush"},
        {&runtimeCpp, "flatcpu::kWitness", 1, "the camera witness"},
        {&runtimeCpp, "flatcpu::kEngineDraw", 4, "engine motion's draw wrapper: naming, its begin (BeforeDraw), its end, and the flush of what it kept bound"},
        {&runtimeCpp, "flatcpu::kResolve", 2, "the treatment at the copy draw and at the HDR route's trigger"},
        {&runtimeCpp, "flatcpu::kHdrRoute", 2, "the HDR route's trigger detector on every draw, and its selection at the trigger"},
        {&runtimeCpp, "flatcpu::kTrackers", 5, "the state trackers"},
        {&resolveCpp, "flatcpu::Scope backendScope(flatcpu::kBackend);", 1, "the backend evaluation inside the resolver"},
        {&injectCpp, "flatcpu::Scope timed(flatcpu::kInject);", 2, "the camera inject callback, both halves"},
        {&temporalCpp, "flatcpu::Scope timed(flatcpu::kDiscovery);", 16, "each discovery observer"},
        // The resolver's GPU span: the hooks, the guard around its dispatches, and the install.
        {&resolveH, "void flatMonoResolveSetSpanHooks(FlatMonoResolveSpanFn begin, FlatMonoResolveSpanFn end);", 1, "the resolver takes span hooks"},
        {&resolveCpp, "SpanGuard span(context);", 1, "the resolver's dispatches and backend call are one GPU span"},
        {&runtimeCpp, "flatMonoResolveSetSpanHooks(&resolveSpanBegin, &resolveSpanEnd);", 1, "the Present installs the span hooks"},
        // The frame: its GPU span opens at the first game draw and closes before Present; the tick cuts it after.
        {&runtimeCpp, "if (!s.gpuFrameTried) gpuFrameOpen(s, context);", 1, "the whole-frame GPU span opens at the frame's first game draw"},
        {&runtimeCpp, "if (owner()) gpuFrameClose(state());", 1, "and closes just before the real Present"},
        {&runtimeCpp, "s.census.onFrame(censusNow, censusFreq, endedPaused);", 1, "the Present cuts the census once a frame"},
        {&runtimeCpp, "s.census.idle();", 1, "and stops it when no temporal mode is selected"},
        {&runtimeCpp, "const EngineVelocityWrapperCounts wrapper = engineVelocityTakeWrapperCounts();", 1, "the draw wrapper's counts are drained every frame"},
        {&runtimeCpp, "if (censusWasRunning) s.census.noteWrapper(", 1, "and handed to the census only while it runs: no backlog"},
        {&runtimeCpp, "const FlatQueryCounts queries = flatQueryCut().take();", 1, "the query shortcuts' counts are drained every frame too"},
        {&runtimeCpp, "if (censusWasRunning) s.census.noteQueries(queries);", 1, "and handed over only while the census runs"},
        {&runtimeCpp, "Log::get().note(\"%s\", lines.line[i]);", 1, "the lines go to the log as they are"},
        // The census drives engine motion's clock in the flat profile; nothing else does.
        {&cpuH, "emcpu::g_gate.store(gate, std::memory_order_relaxed);", 1, "the census opens engine motion's gate with its own sampling decision"},
        {&cpuH, "emcpu::g_gate.store(0, std::memory_order_relaxed);", 1, "and closes it when it stops"},
        {&menuCpp, "perfMonitorFrame(dev);", 1, "perfMonitorFrame, which cuts engine motion's own recorder, is called once"},
        // The draw wrapper's D3D calls are counted where they are made.
        {&engineH, "inline void engineVelocityNoteStateCalls(unsigned n) noexcept { engine_velocity_detail::g_stateCalls += n; }", 1, "the count is one owner-thread add"},
        {&engineCpp, "EngineVelocityWrapperCounts engineVelocityTakeWrapperCounts() noexcept {", 1, "and drained by one function"},
        {&engineCpp, "g_substitutedBase += familyDraws[f];", 1, "a summary between two drains loses none of the substituted draws"},
    };
    for (const Pin& pin : pins) {
        check(count(*pin.text, pin.needle) == pin.times, pin.what);
        std::string without = *pin.text;
        for (size_t at = without.find(pin.needle); at != std::string::npos; at = without.find(pin.needle))
            without.erase(at, std::strlen(pin.needle));
        check(count(without, pin.needle) == 0, "census wiring control: a source with the line removed no longer contains it");
    }
    check(count(engineCpp, "engineVelocityNoteStateCalls(") >= 30,
          "the draw wrapper counts its D3D calls at every call site (thirty and more)");
    // ORDER. The census's whole-frame span opens before the Paused return (a stood-down frame is
    // still a frame), the Present's census block sits after the stand-down's frame boundary and
    // before anything that reads the frame's draw capture, and the flat branch of the menu tick
    // returns before perfMonitorFrame -- so the census is the only driver of engine motion's clock.
    auto at = [&](const std::string& text, const char* needle) { return text.find(needle); };
    const size_t open = at(runtimeCpp, "if (!s.gpuFrameTried) gpuFrameOpen(s, context);");
    const size_t lastPausedReturn = runtimeCpp.rfind("if (s.work == FlatWork::Paused) return;");
    check(open != std::string::npos && lastPausedReturn != std::string::npos && open < lastPausedReturn && lastPausedReturn - open < 400,
          "the frame's GPU span opens just before the draw scope's Paused return");
    const size_t standDown = at(runtimeCpp, "standDownFrame(s, frame);");
    const size_t tick = at(runtimeCpp, "s.census.onFrame(censusNow, censusFreq, endedPaused);");
    const size_t capture = at(runtimeCpp, "s.drawCapture.present(s.context.Get()");
    check(standDown != std::string::npos && tick != std::string::npos && capture != std::string::npos &&
          standDown < tick && tick < capture,
          "the census tick follows the stand-down's frame boundary and precedes the draw capture");
    const size_t flatReturn = at(menuCpp, "if (!g_budget.shouldRun()) inputGateSetPrivate(false);");
    const size_t perf = at(menuCpp, "perfMonitorFrame(dev);");
    check(flatReturn != std::string::npos && perf != std::string::npos && flatReturn < perf,
          "the flat branch of the menu tick ends before perfMonitorFrame: the census is the flat profile's only driver of engine motion's clock");
    // INSTRUMENT ONLY. The stand-down functions never read the census, and no decision in the
    // runtime reads a figure it produced.
    const size_t first = at(runtimeCpp, "void applyWork(State& s, FlatWork next) {");
    const size_t last = at(runtimeCpp, "bool flatRuntimeStructuralRefusal(");
    check(first != std::string::npos && last != std::string::npos && first < last, "the stand-down functions can be delimited");
    if (first != std::string::npos && last != std::string::npos && first < last)
        check(runtimeCpp.substr(first, last - first).find("census") == std::string::npos,
              "the stand-down never reads the census: it measures, it does not decide");
    // The same for the numbers: the runtime touches the census in eleven places and no other -- the four
    // GPU notes and the skipped one, idle, and the block at the Present (running, onFrame, noteWrapper,
    // noteQueries, take) -- and none of them reads a figure back into the runtime's state.
    check(count(runtimeCpp, "s.census.") == 11,
          "the runtime touches the census in exactly its known places (the GPU notes, idle, and the Present block)");
}

// The camera-write witness's bound in the runtime (flat_witness_bound.h holds the policy itself): a
// walk is asked for only while the bound wants one, the bound is told what each walk learned, an F10
// audit re-arms it, and the camera data capture is a different function that never reads any of it.
void testFlatWitnessWiring() {
    auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string runtimeCpp = slurp("src/d3d11/flat_runtime.cpp");
    check(!runtimeCpp.empty(), "the runtime source is readable from the repo root");
    auto count = [](const std::string& text, const std::string& needle) {
        unsigned n = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
        return n;
    };
    struct Pin { const char* needle; unsigned times; const char* what; };
    const Pin pins[] = {
        {"#include \"flat_witness_bound.h\"", 1, "the runtime takes the bound from its header"},
        {"FlatWitnessBound bound;", 1, "the witness carries the bound"},
        {"if (w.sitesFull || !w.bound.wantsWalk()) { ++w.dedupHits; return; }", 1,
         "a write is counted and returns before any stack walk when the witness is full or disarmed"},
        {"const FlatWitnessStop stopped = w.bound.noteWalk(learned);", 1, "every walk tells the bound what it learned"},
        {"witnessRearm();", 1, "an F10 audit re-arms the witness"},
        {"CaptureStackBackTrace(", 1, "there is one stack walk in the runtime"},
        {"witnessWalk(", 2, "and it is reached from one place: the bounded cameraWitness"},
        {"bool learned = witnessWalk(buffer);", 1, "the walk's result is what the bound is told"},
    };
    for (const Pin& pin : pins) {
        check(count(runtimeCpp, pin.needle) == pin.times, pin.what);
        std::string without = runtimeCpp;
        for (size_t at = without.find(pin.needle); at != std::string::npos; at = without.find(pin.needle))
            without.erase(at, std::strlen(pin.needle));
        check(count(without, pin.needle) == 0, "witness wiring control: a source with the line removed no longer contains it");
    }
    // The re-arm sits in the F10 audit's block, right after the stand-down ends.
    const size_t audit = runtimeCpp.find("projectionAuditRequested.exchange(false");
    const size_t rearm = runtimeCpp.find("witnessRearm();");
    check(audit != std::string::npos && rearm != std::string::npos && audit < rearm && rearm - audit < 500,
          "the re-arm is inside the F10 audit's block");
    // The camera data capture is not the witness: nothing in the bounded region captures or invalidates a camera.
    const size_t from = runtimeCpp.find("bool witnessWalk(const void* buffer) {");
    const size_t to = runtimeCpp.find("bool depthView(ID3D11Texture2D* depth) {");
    check(from != std::string::npos && to != std::string::npos && from < to, "the witness functions can be delimited");
    if (from != std::string::npos && to != std::string::npos && from < to) {
        const std::string region = runtimeCpp.substr(from, to - from);
        check(region.find("capture(") == std::string::npos && region.find("flatCaptureCameraRows") == std::string::npos &&
              region.find(".valid") == std::string::npos && region.find("->valid") == std::string::npos &&
              region.find("cameras[") == std::string::npos && region.find("prefix.sequence") == std::string::npos,
              "the witness never captures or touches a camera: the data motion correctness needs is capture(), apart from it");
        check(region.find("cameraWitness(") != std::string::npos && region.find("witnessRearm(") != std::string::npos,
              "(control: the delimited text is the witness code)");
    }
}

// The camera table in the runtime (flat_camera_table.h holds the table and its kept answer, and the rig above
// holds THEM): every entry of the table is changed through the table's own operations, each of which
// invalidates the draw path's kept answer, and the draw path asks the table for its answer. A source scan
// with removal controls, so a line that goes back to assigning into an entry, or a draw that goes back
// to searching for itself, fails here and not in a flight.
void testFlatCameraTableWiring() {
    auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string runtimeCpp = slurp("src/d3d11/flat_runtime.cpp");
    check(!runtimeCpp.empty(), "the runtime source is readable from the repo root");
    auto count = [](const std::string& text, const std::string& needle) {
        unsigned n = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
        return n;
    };
    struct Pin { const char* needle; unsigned times; const char* what; };
    const Pin pins[] = {
        {"#include \"flat_camera_table.h\"", 1, "the runtime takes the table from its header"},
        {"using CameraTable = FlatCameraTable<Ptr<ID3D11Buffer>>;", 1, "the runtime's table is the header's, holding COM references"},
        // The draw path asks the table, and times the search only when it is made afresh.
        {"const uint32_t b1Binding = bindingGeneration(BindSlot::VsCb1);", 1, "the draw reads the b1 slot's binding generation"},
        {"s.cameras.probe(k.b1, b1Binding, s.prefix.frame)", 1, "the draw asks for the kept answer"},
        {"s.cameras.refresh(k.b1, b1Binding, s.prefix.frame)", 1, "and makes it afresh when there is none"},
        // Every change of an entry, and where it comes from.
        {"s.cameras.claim(std::move(buffer), d.ByteWidth, s.prefix.frame)", 1, "a buffer joins through the table"},
        {"s.cameras.invalidate(*c)", 1, "a write into a buffer invalidates through the table"},
        {"state().cameras.setMapped(*c, bytes)", 1, "a Map notes it through the table"},
        {"state().cameras.setMapped(*c, nullptr)", 1, "and so does the Unmap"},
        {"s.cameras.capture(c, bytes, s.prefix.frame, ++s.prefix.sequence)", 1, "a capture goes through the table, with the frame and the write sequence"},
        {"s.cameras.invalidateAll();", 1, "the game's state going unknown"},
        {"s.cameras.newFrame();", 1, "the frame boundary"},
        {"s.cameras.clear();", 1, "the reset"},
        {"s.cameras.find(resource)", 1, "a search for a buffer is the table's"},
        {"const Camera* camera(ID3D11Resource* resource, bool add) {", 1, "the runtime's finder hands out entries for reading only"},
    };
    for (const Pin& pin : pins) {
        check(count(runtimeCpp, pin.needle) == pin.times, pin.what);
        std::string without = runtimeCpp;
        for (size_t at = without.find(pin.needle); at != std::string::npos; at = without.find(pin.needle))
            without.erase(at, std::strlen(pin.needle));
        check(count(without, pin.needle) == 0, "camera table wiring control: a source with the line removed no longer contains it");
    }
    // Nothing assigns into an entry, or indexes the table, behind the table's back.
    const char* const bypasses[] = {"->valid = false", ".valid = false", "->mapped =", ".mapped =", "Camera{}", "s.cameras[",
                                    "cameraCount", "->frame =", "->sequence =", "->width ="};
    for (const char* bypass : bypasses)
        check(count(runtimeCpp, bypass) == 0, "the runtime does not change a camera entry except through the table (no such text in it)");
    // (control: the scan does find what it looks for)
    check(count("c->valid = false;", "->valid = false") == 1, "camera table wiring control: the bypass scan finds an assignment");
}

// The lazy draw bracket in the runtime and the hooks (flat_substitution.h holds the policy, the engine rig's
// flat_lazy_tests.h the engine's half of it and the policy's own mistakes): every hooked call that could observe or
// depend on engine motion's state puts the game's back first, and EDVR's own reads of the context follow a flush. A
// source scan with removal controls, so a hook that loses its line, a flush that moves behind the real call or behind
// what reads the context, or an event nothing raises, fails here and not in a flight.
void testFlatSubstitutionWiring() {
    auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    auto normalizeNewlines = [](std::string text) {
        std::string normalized;
        normalized.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
            normalized.push_back(text[i]);
        }
        return normalized;
    };
    const std::string runtimeCpp = slurp("src/d3d11/flat_runtime.cpp");
    const std::string vscreenCpp = normalizeNewlines(slurp("src/d3d11/vscreen.cpp"));
    const std::string exposureCpp = slurp("src/d3d11/exposure_fix.cpp");
    const std::string deviceCpp = slurp("src/d3d11/device_hook.cpp");
    const std::string policyH = slurp("src/d3d11/flat_substitution.h");
    check(!runtimeCpp.empty() && !vscreenCpp.empty() && !exposureCpp.empty() && !deviceCpp.empty() && !policyH.empty(),
          "the runtime, hook and policy sources are readable from the repo root");
    auto count = [](const std::string& text, const std::string& needle) {
        unsigned n = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
        return n;
    };
    const std::string mixedLineEndings =
        "void STDMETHODCALLTYPE firstHook() {\r\n"
        "    flatRuntimeSubstitution(self, FlatSubstEvent::kClear);\r\n"
        "}\r\n"
        "void STDMETHODCALLTYPE nextHook() {\r\n"
        "    flatRuntimeSubstitution(self, FlatSubstEvent::kClear);\n"
        "    flatRuntimeSubstitution(self, FlatSubstEvent::kCopy);\r\n"
        "}\n";
    const std::string normalizedMixed = normalizeNewlines(mixedLineEndings);
    const size_t firstStart = normalizedMixed.find("void STDMETHODCALLTYPE firstHook(");
    const size_t firstEnd = normalizedMixed.find("\n}\n", firstStart);
    const std::string firstBody = firstStart == std::string::npos || firstEnd == std::string::npos
        ? std::string() : normalizedMixed.substr(firstStart, firstEnd - firstStart);
    check(count(firstBody, "flatRuntimeSubstitution(self, FlatSubstEvent::kClear);") == 1,
          "mixed CRLF/LF hook extraction stays scoped to the first hook");
    check(count(firstBody, "flatRuntimeSubstitution(self, FlatSubstEvent::kCopy);") == 0,
          "mixed-line-ending extraction does not inherit the next hook's event");
    struct Pin { const std::string* text; const char* needle; unsigned times; const char* what; };
    const Pin pins[] = {
        // The runtime's own sites: what each says to the policy.
        {&runtimeCpp, "engineVelocityFlatLazy(!diagnostics);", 1, "the draw scope turns the lazy form off while a diagnostic capture is armed"},
        {&runtimeCpp, "if (diagnostics) flatRuntimeSubstitution(context, FlatSubstEvent::kOtherDraw);", 1, "and puts the game's state back at once"},
        {&runtimeCpp, "if (!d.supported) flatRuntimeSubstitution(context, FlatSubstEvent::kOtherDraw);", 1,
         "a draw that is not a pool-family draw puts the game's state back"},
        {&runtimeCpp, "if (d.supported && (!continuesRun || coverageReads)) flatRuntimeSubstitution(context, FlatSubstEvent::kOtherDraw);", 1,
         "so does a pool-family draw that is not a plain continuation of the run, or that EDVR reads the context for"},
        {&runtimeCpp, "flatRuntimeSubstitution(ctx, FlatSubstEvent::kDispatch);", 1, "a dispatch puts the game's state back"},
        {&runtimeCpp, "flatRuntimeSubstitution(state().context.Get(), FlatSubstEvent::kPresent);", 1, "the Present puts it back before the real Present"},
        {&runtimeCpp, "flatRuntimeSubstitution(nullptr, FlatSubstEvent::kResize);", 1, "a resize forgets it, touching no context"},
        {&runtimeCpp, "flatRuntimeSubstitution(nullptr, FlatSubstEvent::kClearState);", 1, "ClearState forgets it, touching no context"},
        {&runtimeCpp, "if (!engineVelocityFlatPending()) return;", 1, "with nothing of engine motion's bound the policy costs one load"},
        {&runtimeCpp, "switch (flatSubstAction(event)) {", 1, "the runtime asks the policy what to do"},
        {&runtimeCpp, "engineVelocityFlatFlush(ctx, flushCauseOf(event));", 1, "a flush names its cause"},
        {&runtimeCpp, "engineVelocityFlatAbandon();", 1, "an abandon"},
        {&runtimeCpp, "producer = engineVelocityFlatBeginDraw(ctx, &gameHadTarget6);", 1, "the producer branch opens the lazy bracket"},
        {&runtimeCpp, "engineVelocityFlatEndDraw(ctx);", 1, "and closes it"},
        {&runtimeCpp, "case FlatSubstEvent::kDispatch: return EngineVelocityFlushCause::kDispatch;", 1, "a dispatch's cause"},
        {&runtimeCpp, "case FlatSubstEvent::kClear: return EngineVelocityFlushCause::kClear;", 1, "a clear's cause"},
        {&runtimeCpp, "case FlatSubstEvent::kCopy: return EngineVelocityFlushCause::kCopy;", 1, "a copy's cause"},
        {&runtimeCpp, "case FlatSubstEvent::kResolve: return EngineVelocityFlushCause::kResolve;", 1, "a resolve's cause"},
        {&runtimeCpp, "case FlatSubstEvent::kKeepTargets: return EngineVelocityFlushCause::kKeepTargets;", 1, "a targets-keeping set's cause"},
        {&runtimeCpp, "case FlatSubstEvent::kExecuteCommandList: return EngineVelocityFlushCause::kCommandList;", 1, "a command list's cause"},
        {&runtimeCpp, "case FlatSubstEvent::kPresent: return EngineVelocityFlushCause::kPresent;", 1, "the Present's cause"},
        // Who reaches the runtime's scopes.
        {&vscreenCpp, "FlatRuntimeDrawScope flatDraw(self,", 7, "every draw entry point (D, A, I, N, X, and the two indirect ones) opens the draw scope"},
        {&exposureCpp, "FlatRuntimeDispatchScope flatDispatch(self);", 2, "Dispatch and DispatchIndirect open the dispatch scope"},
        {&deviceCpp, "menuFlatResize(); flatRuntimeResize(); }", 2, "both ResizeBuffers hooks tell the runtime before the real call"},
    };
    for (const Pin& pin : pins) {
        check(count(*pin.text, pin.needle) == pin.times, pin.what);
        std::string without = *pin.text;
        for (size_t at = without.find(pin.needle); at != std::string::npos; at = without.find(pin.needle))
            without.erase(at, std::strlen(pin.needle));
        check(count(without, pin.needle) == 0, "substitution wiring control: a source with the line removed no longer contains it");
    }
    // The hooks: one event each, ahead of the real call the hook forwards to (the last one in its body: the early returns
    // for an internal or foreign call forward untouched, and the void fix in ClearRenderTargetView is another way out).
    struct Hook { const char* name; const char* event; const char* real; };
    const Hook hooks[] = {
        {"hookedClearRtv", "kClear", "realClearRtv("},
        {"hookedClearUavUint", "kClear", "realClearUavUint("},
        {"hookedClearUavFloat", "kClear", "realClearUavFloat("},
        {"hookedClearDsv", "kClear", "realClearDsv("},
        {"hookedGenerateMips", "kCopy", "realGenerateMips("},
        {"hookedCopyResource", "kCopy", "realCopyResource("},
        {"hookedCopyStructureCount", "kCopy", "realCopyStructureCount("},
        {"hookedCopySubresourceRegion", "kCopy", "realCopySubresourceRegion("},
        {"hookedUpdateSubresource", "kCopy", "realUpdateSubresource("},
        {"hookedResolveSubresource", "kResolve", "realResolveSubresource("},
        {"hookedOMSetRtvAndUav", "kKeepTargets", "realOMSetRtvAndUav("},
        {"hookedExecuteCommandList", "kExecuteCommandList", "realExecuteCommandList("},
    };
    for (const Hook& hook : hooks) {
        const std::string head = std::string("void STDMETHODCALLTYPE ") + hook.name + "(";
        const size_t from = vscreenCpp.find(head);
        const size_t to = from == std::string::npos ? std::string::npos : vscreenCpp.find("\n}\n", from);
        check(from != std::string::npos && to != std::string::npos, (std::string("the hook ") + hook.name + " can be delimited").c_str());
        if (from == std::string::npos || to == std::string::npos) continue;
        const std::string body = vscreenCpp.substr(from, to - from);
        const std::string call = std::string("flatRuntimeSubstitution(self, FlatSubstEvent::") + hook.event + ");";
        const size_t sub = body.find(call);
        const size_t real = body.rfind(hook.real);
        check(count(body, call) == 1, (std::string(hook.name) + " raises " + hook.event + " exactly once").c_str());
        check(sub != std::string::npos && real != std::string::npos && sub < real,
              (std::string(hook.name) + " raises it before the real call").c_str());
        check(body.find("flatRuntimeActive()") != std::string::npos && body.find("flatRuntimeActive()") < sub,
              (std::string(hook.name) + " raises it only while the flat runtime is active").c_str());
    }
    // The event the policy names all have a caller, and a caller for an event the policy does not name does not compile.
    const char* const events[] = {"kOtherDraw", "kDispatch", "kClear", "kCopy", "kResolve", "kKeepTargets", "kExecuteCommandList", "kPresent",
                                  "kClearState", "kResize"};
    for (const char* event : events) {
        const std::string enumerator = std::string("FlatSubstEvent::") + event;
        check(count(policyH, std::string("    ") + event) >= 1 || count(policyH, std::string(event) + ",") >= 1,
              (std::string("the policy names ") + event).c_str());
        check(count(runtimeCpp, enumerator) + count(vscreenCpp, enumerator) >= 1, (std::string("something raises ") + event).c_str());
    }
    // Order inside the runtime's draw scope: the lazy switch and the flushes come before anything reads the context, and the
    // bracket opens after the coverage classification and the qualification, which read it.
    const size_t scope = runtimeCpp.find("FlatRuntimeDrawScope::FlatRuntimeDrawScope(");
    const char* const order[] = {
        "engineVelocityFlatLazy(!diagnostics);",
        "if (diagnostics) flatRuntimeSubstitution(context, FlatSubstEvent::kOtherDraw);",
        "d.supported = engineVelocityPoolFamilyPair(k.vs, k.ps);",
        "if (!d.supported) flatRuntimeSubstitution(context, FlatSubstEvent::kOtherDraw);",
        "d.hdrCopyVerified=verifyHdrCopy(ctx,d);",
        "const bool continuesRun =",
        "if (d.supported && (!continuesRun || coverageReads)) flatRuntimeSubstitution(context, FlatSubstEvent::kOtherDraw);",
        "flatcpu::Scope coverage(flatcpu::kCoverage);",
        "qualifyProjection(s,recipes,",
        "producer = engineVelocityFlatBeginDraw(ctx, &gameHadTarget6);",
        "flatMonoResolve(s.device.Get(), ctx, f,",
    };
    size_t previous = scope;
    bool ordered = scope != std::string::npos;
    for (const char* needle : order) {
        const size_t where = scope == std::string::npos ? std::string::npos : runtimeCpp.find(needle, scope);
        if (where == std::string::npos || where < previous) { ordered = false; std::printf("  out of order or missing: %s\n", needle); }
        else previous = where;
    }
    check(ordered, "in the draw scope the lazy switch and the flushes precede every read of the context, and the bracket opens after the coverage reads");
    // The frame's end: the flush comes before the census closes its span, and the menu and the real Present follow it.
    const size_t before = runtimeCpp.find("void flatRuntimeBeforePresent() {");
    const size_t flushAt = runtimeCpp.find("flatRuntimeSubstitution(state().context.Get(), FlatSubstEvent::kPresent);", before);
    const size_t closeAt = runtimeCpp.find("gpuFrameClose(state());", before);
    check(before != std::string::npos && flushAt != std::string::npos && closeAt != std::string::npos && flushAt < closeAt,
          "the Present's flush comes before the census closes its span");
    const size_t hookFrom = deviceCpp.find("HRESULT STDMETHODCALLTYPE hookedPresent(");
    const size_t hookTo = hookFrom == std::string::npos ? std::string::npos : deviceCpp.find("\n}\n", hookFrom);
    if (hookFrom != std::string::npos && hookTo != std::string::npos) {
        const std::string body = deviceCpp.substr(hookFrom, hookTo - hookFrom);
        const size_t flush = body.find("flatRuntimeBeforePresent();");
        const size_t menu = body.find("menuFlatBeforePresent(self, flags);");
        const size_t real = body.rfind("g_state->realPresent(self, syncInterval, flags);");   // (the first is the foreign swap chain's)
        check(flush != std::string::npos && menu != std::string::npos && real != std::string::npos && flush < menu && menu < real,
              "the Present hook flushes before the menu draws and before the real Present");
    } else {
        check(false, "the Present hook can be delimited");
    }
}

// The graphics-wrapper note's wiring (flat_wrapper_note.h holds the decision and the words, and the rig above them):
// the hook-mode probe names the file and publishes it once, and the panel draws the note from that name only in the
// flat profile, and logs the first time. A source scan with removal controls.
void testFlatWrapperNoteWiring() {
    auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string proxyCpp = slurp("src/d3d11/d3d11_proxy.cpp");
    const std::string menuCpp = slurp("src/d3d11/menu.cpp");
    const std::string hookH = slurp("src/d3d11/device_hook.h");
    check(!proxyCpp.empty() && !menuCpp.empty() && !hookH.empty(), "the proxy, panel and hook header sources are readable from the repo root");
    auto count = [](const std::string& text, const std::string& needle) {
        unsigned n = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
        return n;
    };
    struct Pin { const std::string* text; const char* needle; unsigned times; const char* what; };
    const Pin pins[] = {
        {&proxyCpp, "#include \"flat_wrapper_note.h\"", 1, "the probe takes the decision from its header"},
        {&proxyCpp, "edvr::vtableDominantOtherModule(vt, kSample, g_systemModule, owner, sizeof(owner))", 1,
         "the probe asks which module backs the methods, excluding Windows' d3d11.dll"},
        {&proxyCpp, "flatWrapperFile(mode, probed, owner)", 1, "and lets the header decide from the mode it used and what it alone chose"},
        {&proxyCpp, "g_wrapperFileSet.store(true, std::memory_order_release);", 1, "the name is published once, after it is written"},
        {&proxyCpp, "const char* contextWrapperFile() {", 1, "and read back through one accessor"},
        {&hookH, "const char* contextWrapperFile();", 1, "which the header declares"},
        {&menuCpp, "#include \"flat_wrapper_note.h\"", 1, "the panel takes the words from the same header"},
        {&menuCpp, "const char* wrapper = contextWrapperFile();", 1, "the panel reads the published name"},
        {&menuCpp, "flatComposeWrapperNote(temporalModeEnabled(Config::get().requestedTemporalMode()), wrapper,", 1,
         "and composes the note only for a selected temporal mode"},
        {&menuCpp, "s.flatWrapperNoteLogged = true;", 1, "said once in the log"},
        {&menuCpp, "flat wrapper note: shown in the panel", 1, "with its own line"},
    };
    for (const Pin& pin : pins) {
        check(count(*pin.text, pin.needle) == pin.times, pin.what);
        std::string without = *pin.text;
        for (size_t at = without.find(pin.needle); at != std::string::npos; at = without.find(pin.needle))
            without.erase(at, std::strlen(pin.needle));
        check(count(without, pin.needle) == 0, "wrapper note wiring control: a source with the line removed no longer contains it");
    }
    // The probe publishes after it has decided and logged the mode, and the panel's block is flat-only.
    const size_t decided = proxyCpp.find("hookModeName(mode), inSystem, kSample,");
    const size_t named = proxyCpp.find("flatWrapperFile(mode, probed, owner)");
    check(decided != std::string::npos && named != std::string::npos && decided < named,
          "the probe names the wrapper after it has decided the mode");
    // The name is written before the flag that says it is there: the panel reads it from another thread.
    const size_t written = proxyCpp.find("std::memcpy(g_wrapperFile, file, std::strlen(file) + 1);");
    const size_t flagged = proxyCpp.find("g_wrapperFileSet.store(true, std::memory_order_release);");
    check(written != std::string::npos && flagged != std::string::npos && written < flagged,
          "the probe writes the name before it publishes it");
    const size_t block = menuCpp.find("The graphics-wrapper note (flat only");
    const size_t flatOnly = block == std::string::npos ? std::string::npos : menuCpp.find("if (runtimeFlatProfile()) {", block);
    const size_t reads = block == std::string::npos ? std::string::npos : menuCpp.find("contextWrapperFile()", block);
    check(block != std::string::npos && flatOnly != std::string::npos && reads != std::string::npos && flatOnly < reads && reads - flatOnly < 200,
          "the panel reads the wrapper's name only in the flat profile");
}

// The query shortcuts in the runtime and the engine motion wrapper (flat_query_cut.h holds the policy and the rig above
// it, the engine rig the D3D side of each state): every question the flat path used to put to the context goes through the
// policy, the census is told the counts, the frame's end lets go of what was kept, and the coverage classification no longer
// reads the context itself. A source scan with removal controls.
void testFlatQueryCutWiring() {
    auto slurp = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string runtimeCpp = slurp("src/d3d11/flat_runtime.cpp");
    const std::string engineCpp = slurp("src/d3d11/engine_velocity.cpp");
    const std::string readsH = slurp("src/d3d11/flat_query_reads.h");
    check(!runtimeCpp.empty() && !engineCpp.empty() && !readsH.empty(), "the runtime, engine motion and query sources are readable from the repo root");
    auto count = [](const std::string& text, const std::string& needle) {
        unsigned n = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
        return n;
    };
    struct Pin { const std::string* text; const char* needle; unsigned times; const char* what; };
    const Pin pins[] = {
        // The runtime.
        {&runtimeCpp, "const void* depthResource=coverageDepthResource(ctx,k,depthHold);", 2, "both projection branches of the coverage classification ask for the depth view through the policy"},
        {&runtimeCpp, "if(coverageShadersMatch(ctx,k)) {", 2, "and both unchanged-shader branches ask for the shaders through it"},
        {&runtimeCpp, "flatQueryDepth(flatQueryCut(), ctx, k.depth, hold,", 1, "the depth question is the shared function's, with the draw key's depth as the shadow's answer"},
        {&runtimeCpp, "flatQueryShaders(flatQueryCut(), context, k.vs, k.ps,", 1, "and the shader question, with the key's hashes"},
        {&runtimeCpp, "flatQueryCut().beginFrame(frame);", 1, "the Present tells the policy which frame starts (one in 64 checks)"},
        {&runtimeCpp, "(s.projectionFrames != 0 || !flatCameraInjectUpstreamOwns());", 1,
         "the coverage reads that still ask the context (an F10 audit, the legacy route) put the game's state back first, and nothing else does"},
        {&runtimeCpp, "if (owner()) engineVelocityFlatFrameEnd();", 1, "the frame's end lets go of what the bracket kept"},
        // Engine motion's wrapper.
        {&engineCpp, "flatQueryCut().plan(FlatQuery::GameTargets)", 1, "the game's render-target set is kept through the policy"},
        {&engineCpp, "flatQueryCut().plan(FlatQuery::GameBlend)", 1, "and its blend state"},
        {&engineCpp, "flatQueryCut().plan(FlatQuery::TargetsKept)", 1, "and the runtime's acceptance of MRT6"},
        {&engineCpp, "flatQueryCut().compared(", 3, "each is compared with the context on a check"},
        {&engineCpp, "void engineVelocityFlatFrameEnd() noexcept {", 1, "the bracket lets go of what it kept at the frame's end"},
        // The shared reads.
        {&readsH, "cut.plan(FlatQuery::CoverageDepth)", 1, "the depth question asks the policy"},
        {&readsH, "cut.plan(FlatQuery::ShaderIdentity)", 1, "and the shader question"},
        {&readsH, "cut.compared(", 2, "and each compares on a check"},
    };
    for (const Pin& pin : pins) {
        check(count(*pin.text, pin.needle) == pin.times, pin.what);
        std::string without = *pin.text;
        for (size_t at = without.find(pin.needle); at != std::string::npos; at = without.find(pin.needle))
            without.erase(at, std::strlen(pin.needle));
        check(count(without, pin.needle) == 0, "query cut wiring control: a source with the line removed no longer contains it");
    }
    // The coverage classification no longer reads the context for what the shadow knows.
    const size_t coverageFrom = runtimeCpp.find("flatcpu::Scope coverage(flatcpu::kCoverage);");
    const size_t coverageTo = coverageFrom == std::string::npos ? std::string::npos : runtimeCpp.find("if (continuesRun) {", coverageFrom);
    check(coverageFrom != std::string::npos && coverageTo != std::string::npos && coverageFrom < coverageTo,
          "the coverage classification can be delimited");
    if (coverageFrom != std::string::npos && coverageTo != std::string::npos && coverageFrom < coverageTo) {
        const std::string region = runtimeCpp.substr(coverageFrom, coverageTo - coverageFrom);
        check(count(region, "GetRenderTargets") == 0 && count(region, "GetShader") == 0,
              "the coverage classification does not read the depth view or the shaders off the context itself");
        check(count(region, "coverageDepthResource(") == 2 && count(region, "coverageShadersMatch(") == 2,
              "(control: the delimited text is the classification, and asks through the policy)");
    }
    // The frame's end comes after the Present's flush, inside the same function.
    const size_t before = runtimeCpp.find("void flatRuntimeBeforePresent() {");
    const size_t flush = runtimeCpp.find("flatRuntimeSubstitution(state().context.Get(), FlatSubstEvent::kPresent);", before);
    const size_t end = runtimeCpp.find("if (owner()) engineVelocityFlatFrameEnd();", before);
    check(before != std::string::npos && flush != std::string::npos && end != std::string::npos && flush < end && end - flush < 400,
          "the frame's end follows the Present's flush");
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--classify-dir") == 0)
        return flatShaderClassifierSweep(argv[2]);
    if (argc == 3 && std::strcmp(argv[1], "--trace-check") == 0)
        return flatTraceCheck(argv[2]);
    if (argc == 3 && std::strcmp(argv[1], "--trace-migrate") == 0)
        return flatTraceMigrate(argv[2]);
    if (argc == 3 && std::strcmp(argv[1], "--trace-rekey") == 0)
        return flatTraceRekey(argv[2]);
    // The HDR route's own modes (flat_hdr_route_tests.h): what the trigger detector finds in a trace, frame by
    // frame, and a trace cut down to the named frames.
    if (argc == 3 && std::strcmp(argv[1], "--trace-chain") == 0)
        return hdr_route_test::traceChain(argv[2]);
    if (argc == 5 && std::strcmp(argv[1], "--trace-trim") == 0)
        return hdr_route_test::traceTrim(argv[2], argv[3], argv[4]);
    // The final copy's admission by structure (flat_copy_structure_tests.h): what it makes of each frame of a trace.
    if ((argc == 3 || argc == 4) && std::strcmp(argv[1], "--trace-structure") == 0)
        return copy_structure_test::traceStructure(argv[2], argc == 4 && std::strcmp(argv[3], "pretend") == 0);
    // --write-fixture <path> [--dry-run]: regenerate tools\flat_upscale_fixture.log from the formatters. Anything that writes a file
    // takes --dry-run, and --dry-run writes nothing at all.
    if ((argc == 3 || argc == 4) && std::strcmp(argv[1], "--write-fixture") == 0) {
        const std::string text = copy_structure_test::flatUpscaleFixtureText();
        if (argc == 4 && std::strcmp(argv[3], "--dry-run") == 0) {
            std::printf("flat_temporal_test: --dry-run: would write %zu bytes to %s; wrote nothing\n", text.size(), argv[2]);
            return 0;
        }
        std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        std::printf("flat_temporal_test: wrote %zu bytes to %s\n", text.size(), argv[2]);
        return out ? 0 : 1;
    }
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) {
        std::puts("usage: flat_temporal_test --self-test | --classify-dir <dir> | --trace-check <file> | --trace-migrate <dir> | "
                  "--trace-rekey <file|dir> | --trace-chain <file> | --trace-structure <file> | "
                  "--trace-trim <in> <out> <frame[,frame...]>");
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
    testFlatScreenBand();
    testFlatResolveRoute();
    testFlatDlssNegotiate();
    testProjectionSlices();
    testDetailBudget();
    failures += flatShaderCaptureTests();
    failures += flatCameraProbeTests();
    failures += flatProjectionMathTests();
    failures += flatProjectionBindingsTests();
    failures += flatProjectionRecipeTests();
    failures += flatShaderClassifierTests();
    failures += flatProjectionOwnershipTests();
    failures += flatComputeTests();
    failures += flatLightingTests();
    failures += flatLivePhaseTests();
    failures += flatLocalRejectTests();
    failures += flatNegotiatedEvalTests();
    flatRuntimePrefixTests();
    flatRuntimeImageCopyTests();
    flatRuntimeMenuCopyTests();
    testFrameContractTrace();
    testFrameContractCorpus();
    testFrameContractOutcomes();
    testFrameContractHashCoverage();
    testHullPairKeying();
    testStaticSceneWiring();
    failures += flatStandDownTests();
    testStandDownAgainstModel();
    testStandDownWiring();
    failures += flatEliteSettingsTests();
    testFlatWarningWiring();
    failures += flatCpuTests();
    testFlatCpuWiring();
    failures += flatWitnessBoundTests();
    testFlatWitnessWiring();
    failures += flatCameraTableTests();
    testFlatCameraTableWiring();
    testFlatSubstitutionWiring();
    failures += flatWrapperNoteTests();
    testFlatWrapperNoteWiring();
    failures += flatQueryCutTests();
    testFlatQueryCutWiring();
    failures += flatHdrRouteTests();
    failures += flatCopyStructureTests();
    failures += flatHdrCrumbTests();
    failures += flatHdrCrumbWiringTests();
    if (failures) return 1;
    std::puts("flat temporal collector policy: PASS");
    return 0;
}
