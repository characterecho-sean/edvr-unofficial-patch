#include "src/d3d11/flat_origin_capture.h"
#include "src/d3d11/flat_stationary_camera_core.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <limits>
#include <string>
#include <unordered_map>

using namespace edvr::stationcam;

struct Memory {
    std::unordered_map<uintptr_t, uint8_t> cells;
    uintptr_t denied = 0;
    uintptr_t changeOnRead = 0, changeAddress = 0;
    uintptr_t changeValue = 0;
    unsigned watchedReads = 0, changeAfter = 0;
    template<class T> void put(uintptr_t at, const T& value) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        for (size_t i = 0; i < sizeof(value); ++i) cells[at + i] = bytes[i];
    }
    void putBytes(uintptr_t at, const void* value, size_t count) {
        const auto* bytes = static_cast<const uint8_t*>(value);
        for (size_t i = 0; i < count; ++i) cells[at + i] = bytes[i];
    }
    static bool read(void* opaque, uintptr_t at, void* output, size_t count) noexcept {
        auto& m = *static_cast<Memory*>(opaque);
        if (at == m.changeOnRead && ++m.watchedReads == m.changeAfter)
            m.put(m.changeAddress, m.changeValue);
        auto* bytes = static_cast<uint8_t*>(output);
        for (size_t i = 0; i < count; ++i) {
            if (at + i == m.denied) return false;
            const auto found = m.cells.find(at + i);
            if (found == m.cells.end()) return false;
            bytes[i] = found->second;
        }
        return true;
    }
};

constexpr uintptr_t kBase = 0x10000000, kB = 0x20000000, kC = 0x21000000;
constexpr uintptr_t kName = 0x22000000, kRoster = 0x23000000;
constexpr uintptr_t kActivity = 0x24000000, kVtable = 0x25000000, kActivityName = 0x26000000;
constexpr uintptr_t kNestedStack = 0x27000000, kNestedConfig = 0x28000000;
constexpr uintptr_t kNestedName = 0x29000000, kNestedFirst = 0x30000000;
constexpr uintptr_t kPlacedRecord = 0x31000000, kProvider = 0x32000000;
constexpr uintptr_t kConfig = 0x33000000, kOverride = 0x34000000, kTarget = 0x35000000;

Memory fixture() {
    Memory m;
    m.put(kC, kBase + 0x51890B0);
    m.put(kC + 0x10, kBase + 0x51890B8);
    m.put(kC + 0x30, kB);
    m.put(kC + 0x40, kB + 0xB8);
    m.put(kC + 0x58, uint64_t{1});
    m.put(kC + 0x60, kRoster);
    m.put(kC + 0x68, uint32_t{0});
    m.put(kB, kBase + 0x51893B0);
    m.put(kB + 0x20, kName);
    for (unsigned i = 0; i < 11; ++i) m.put(kB + 0x28 + 8*i, i == 10 ? kC : uintptr_t{0});
    m.put(kName, uint64_t{8});
    m.put(kName + 8, uint64_t{16});
    m.putBytes(kName + 0x14, "MainView", 8);
    float poseData[16]{};
    poseData[0] = poseData[5] = poseData[10] = poseData[15] = 1.0f;
    m.putBytes(kB + 0xB8 + 0x40, poseData, sizeof(poseData));
    poseData[12] = 42.0f;
    m.putBytes(kC + 0x70 + 0x40, poseData, sizeof(poseData));
    m.put(kRoster, kActivity);
    m.put(kActivity, kVtable);
    m.put(kVtable + 0x20, kBase + 0x8BEA80);
    m.put(kVtable + 0x28, kBase + 0x529540);
    m.put(kActivity - 4, uint8_t{1});
    m.put(kActivity + 8, kActivityName);
    m.put(kActivity - 0x20, kBase + 0x12345);
    m.putBytes(kActivityName, "Headlook\0", 9);
    poseData[12] = 42.0f;
    m.putBytes(kActivity + 0x10 + 0x40, poseData, sizeof(poseData));
    return m;
}

Memory nestedFixture(float weight) {
    Memory m = fixture();
    const uintptr_t nestedActivity = kNestedStack + 0x98;
    m.put(kRoster, nestedActivity);
    m.put(nestedActivity, kBase + 0x5189AD0);
    m.put(kBase + 0x5189AD0 + 0x20, kBase + 0x108BEE0);
    m.put(kBase + 0x5189AD0 + 0x28, kBase + 0x108BA8C);
    m.put(nestedActivity - 0xA0, kNestedConfig);
    m.put(kNestedConfig + 0x34, weight);
    m.put(kNestedStack, kBase + 0x5189A58);
    m.put(kNestedStack + 0x20, kNestedName);
    m.put(kNestedStack + 0x1B8, kC);
    m.put(kNestedName, uint64_t{10});
    m.put(kNestedName + 8, uint64_t{16});
    m.putBytes(kNestedName + 0x14, "NestedView", 10);
    float poseData[16]{};
    poseData[0] = poseData[5] = poseData[10] = poseData[15] = 1.0f;
    for (unsigned i = 0; i < 11; ++i) {
        const uintptr_t child = kNestedFirst + 0x1000 * i;
        m.put(kNestedStack + 0x28 + 8 * i, child);
        m.put(child, kBase + 0x51890B0);
        m.put(child + 0x10, kBase + 0x51890B8);
        m.put(child + 0x30, kNestedStack);
        m.put(child + 0x40, i ? kNestedFirst + 0x1000 * (i - 1) + 0x70 : kB + 0xB8);
        m.put(child + 0x58, uint64_t{0});
        m.put(child + 0x60, uintptr_t{0});
        m.put(child + 0x68, uint32_t{0});
        poseData[12] = static_cast<float>(i + 1);
        m.putBytes(child + 0x70 + 0x40, poseData, sizeof(poseData));
    }
    return m;
}

Memory placedFixture() {
    Memory m = fixture();
    const uintptr_t object = kActivity - 0x20;
    const uintptr_t placedVtable = kBase + 0x518CFC8;
    m.put(kActivity, placedVtable);
    m.put(object, kBase + 0x518CF58);
    m.put(placedVtable + 0x20, kBase + 0x8BEA80);
    m.put(placedVtable + 0x28, kBase + 0x529540);
    m.put(object + 0x1E8, kPlacedRecord);
    m.put(kPlacedRecord + 8, kTarget);
    m.put(kPlacedRecord + 0x10, kProvider);
    m.put(kProvider, kBase + 0x530E530); // class is observed, semantics unknown
    m.put(kBase + 0x530E530 + 0x20, kBase + 0x529540);
    m.put(kTarget + 0xC0, int32_t{8});
    m.put(object + 0x10, kConfig);
    m.put(kConfig + 0x14, int32_t{1});
    m.put(kConfig + 0x18, int32_t{0});
    m.put(object + 0x188, int32_t{-1});
    m.put(object + 0x190, kOverride);
    m.put(kOverride, kBase + 0x5358518);
    m.put(kBase + 0x5358518 + 0x70, kBase + 0x123456);
    float matrix[16]{};
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    matrix[12] = 10.0f;
    m.putBytes(kPlacedRecord + 0x20, matrix, sizeof(matrix));
    matrix[12] = 20.0f;
    m.putBytes(kPlacedRecord + 0x60, matrix, sizeof(matrix));
    matrix[12] = 21.0f;
    m.putBytes(object + 0x70, matrix, sizeof(matrix));
    return m;
}

bool testCaptureControl() {
    edvr::FlatOriginCaptureControl absent, off, on;
    if (absent.configured() || absent.enabled()) return false;
    off.initialize(false); off.initialize(true);
    if (!off.configured() || off.enabled()) return false; // late enable requires restart
    on.initialize(true); on.initialize(false);
    if (!on.configured() || !on.enabled()) return false; // reload cannot change startup choice
    if (edvr::flatOriginCaptureEnabled()) return false;
    edvr::flatOriginCaptureInitialize(true); // remaining pure capture fixtures opt in
    return edvr::flatOriginCaptureEnabled();
}

bool testGate() {
    ParkGate gate;
    ParkInput in;
    in.active = in.gameplay = in.dockedKnown = in.docked = in.seatedKnown = in.seated = in.guiKnown = true;
    in.sceneWitness = true; in.depth = 0xABC; in.origin[0] = 100;
    // Parked journal facts alone cannot arm the one-shot capture.
    in.sceneWitness = false;
    for (unsigned frame = 1; frame <= 60; ++frame) {
        in.frame = frame; in.statusSamples = frame;
        if (gate.step(in) || gate.stable || gate.fresh) return false;
    }
    in.sceneWitness = true;
    // A menu can keep stale docked/seated flags for hundreds of frames. It
    // cannot spend the freshness/stability budget.
    in.menu = true;
    for (unsigned frame = 1; frame <= 500; ++frame) {
        in.frame = frame; in.statusSamples = frame / 60;
        if (gate.step(in) || gate.stable || gate.fresh) return false;
    }
    in.menu = false;
    for (unsigned frame = 501; frame <= 560; ++frame) {
        in.frame = frame;
        in.depth = frame & 1 ? 0xABC : 0xDEF; // alternating valid render resources
        in.origin[0] = 100.0f + static_cast<float>(frame) * 0.1f; // no kinematic stillness requirement
        in.reset = (frame & 1) != 0; // previous-history reset does not invalidate current rows
        in.statusSamples = 8 + (frame >= 520 ? 1u : 0u) + (frame >= 540 ? 1u : 0u);
        const bool ready = gate.step(in);
        if (ready != (frame >= 540)) return false;
    }
    in.gui = 6; in.frame = 561;
    if (gate.step(in) || gate.stable || gate.fresh) return false;
    in.gui = 0; in.frame = 562;
    if (gate.step(in) || gate.fresh) return false; // stale status sample cannot re-arm
    in.frame = 563; in.origin[0] = std::numeric_limits<float>::quiet_NaN();
    if (gate.step(in) || gate.stable || gate.fresh) return false;
    return true;
}

ParkInput parkedInput(uint64_t frame, uint32_t samples) {
    ParkInput in;
    in.active = in.gameplay = in.dockedKnown = in.docked = in.seatedKnown = in.seated = in.guiKnown = true;
    in.frame = frame; in.statusSamples = samples;
    return in;
}
bool sceneCheck(bool passed, const char* name) {
    if (!passed) std::fprintf(stderr, "completed scene gate: %s failed\n", name);
    return passed;
}
bool warmSceneGate(SceneFrameGate& gate) {
    const float origin[3] = {1, 2, 3};
    for (unsigned frame = 1; frame <= 29; ++frame) {
        gate.candidate(frame, origin, 0xABC);
        const auto out = gate.finish(frame, parkedInput(frame, 10 + (frame >= 10) + (frame >= 20)));
        if (!out.processed || !out.qualified || out.ready) return false;
    }
    return gate.park.stable == 29 && gate.park.fresh == 2;
}
bool testCompletedSceneGate() {
    const float origin[3] = {1, 2, 3};
    SceneFrameGate happy;
    if (!sceneCheck(warmSceneGate(happy), "29 good frames and two new samples")) return false;
    happy.candidate(30, origin, 0xABC);
    happy.candidate(30, origin, 0xABC);
    if (!sceneCheck(happy.park.stable == 29, "callbacks cannot advance readiness")) return false;
    auto out = happy.finish(30, parkedInput(30, 12));
    if (!sceneCheck(out.processed && out.qualified && out.ready && happy.park.stable == 30 &&
                    happy.park.fresh == 2, "30-frame happy path")) return false;
    out = happy.finish(30, parkedInput(30, 13));
    if (!sceneCheck(!out.processed && happy.park.stable == 30 && happy.park.fresh == 2,
                    "repeated Present cannot count another sample")) return false;

    SceneFrameGate late;
    if (!warmSceneGate(late)) return false;
    late.candidate(30, origin, 0xABC);
    late.reject(30, "late-camera-conflict");
    out = late.finish(30, parkedInput(30, 12));
    if (!sceneCheck(!out.ready && !out.qualified && late.park.stable == 0 && late.park.fresh == 0 &&
                    std::strcmp(out.sceneRefusal, "late-camera-conflict") == 0,
                    "candidate then reject on 30th frame")) return false;

    SceneFrameGate early;
    if (!warmSceneGate(early)) return false;
    early.reject(30, "first-camera-conflict");
    early.candidate(30, origin, 0xABC);
    early.candidate(30, origin, 0xABC);
    out = early.finish(30, parkedInput(30, 12));
    if (!sceneCheck(!out.ready && !out.qualified && !early.park.stable && !early.park.fresh &&
                    std::strcmp(out.sceneRefusal, "first-camera-conflict") == 0,
                    "candidate cannot rescue rejection")) return false;

    SceneFrameGate missing;
    if (!warmSceneGate(missing)) return false;
    out = missing.finish(30, parkedInput(30, 12));
    if (!sceneCheck(!out.qualified && !out.ready && !missing.park.stable && !missing.park.fresh &&
                    std::strcmp(out.sceneRefusal, "no-trigger-or-camera-witness") == 0,
                    "missing observer after 29 frames resets both budgets")) return false;
    for (unsigned frame = 31; frame <= 65; ++frame) {
        missing.candidate(frame, origin, 0xABC);
        out = missing.finish(frame, parkedInput(frame, 12));
        if (!sceneCheck(!out.ready && !missing.park.fresh, "stale status after observer gap cannot arm")) return false;
    }
    missing.candidate(66, origin, 0xABC);
    out = missing.finish(66, parkedInput(66, 13));
    if (!sceneCheck(!out.ready && missing.park.fresh == 1, "first new post-gap sample")) return false;
    missing.candidate(67, origin, 0xABC);
    out = missing.finish(67, parkedInput(67, 14));
    if (!sceneCheck(out.ready && missing.park.fresh == 2, "two fresh samples after gap")) return false;

    SceneFrameGate skipped;
    if (!warmSceneGate(skipped)) return false;
    skipped.candidate(32, origin, 0xABC);
    out = skipped.finish(32, parkedInput(32, 12));
    if (!sceneCheck(!out.ready && skipped.park.stable == 1 && !skipped.park.fresh,
                    "skipped Present gap resets freshness")) return false;

    // Same production helper supplies Capturing's qualified/refusal verdict.
    // A provisional post-arm witness must not survive a late rejection.
    happy.candidate(31, origin, 0xABC);
    happy.reject(31, "post-arm-conflict");
    out = happy.finish(31, parkedInput(31, 12));
    if (!sceneCheck(!out.qualified && !out.ready &&
                    std::strcmp(out.sceneRefusal, "post-arm-conflict") == 0,
                    "post-arm valid then reject")) return false;
    SceneFrameGate captureMissing;
    if (!warmSceneGate(captureMissing)) return false;
    captureMissing.candidate(30, origin, 0xABC);
    if (!captureMissing.finish(30, parkedInput(30, 12)).ready) return false;
    out = captureMissing.finish(31, parkedInput(31, 12));
    if (!sceneCheck(!out.qualified && !out.ready && !captureMissing.park.fresh,
                    "missing capture-frame witness")) return false;

    SceneFrameGate journal;
    if (!warmSceneGate(journal)) return false;
    journal.candidate(30, origin, 0xABC);
    auto menu = parkedInput(30, 12); menu.gui = 6;
    out = journal.finish(30, menu);
    if (!sceneCheck(!out.qualified && !out.ready && !journal.park.stable && !journal.park.fresh,
                    "current Present journal vetoes earlier candidate")) return false;
    if (!sceneCheck(!journal.candidate(30, origin, 0xABC) && !journal.reject(30, "too-late"),
                    "completed frame callbacks cannot alter finalized gate")) return false;

    SceneFrameGate nullCandidate;
    nullCandidate.candidate(1, origin, 0xABC);
    nullCandidate.candidate(1, nullptr, 0xABC);
    out = nullCandidate.finish(1, parkedInput(1, 12));
    if (!sceneCheck(!out.qualified && std::strcmp(out.sceneRefusal, "scene-origin-nonfinite") == 0,
                    "null repeat vetoes prior candidate")) return false;

    SceneFrameGate invalid;
    invalid.candidate(1, origin, 0);
    invalid.candidate(1, origin, 0xABC);
    out = invalid.finish(1, parkedInput(1, 12));
    if (!sceneCheck(!out.qualified && std::strcmp(out.sceneRefusal, "no-scene-depth") == 0,
                    "valid repeat cannot hide invalid candidate")) return false;
    return true;
}

bool testPassiveSceneWitness() {
    SceneWitnesses census;
    float camera[6][4]{};
    camera[0][0] = camera[1][1] = camera[2][2] = 1;
    SceneDraw draw;
    draw.color = 0x100; draw.rtv = 0x110; draw.depth = 0x120; draw.dsv = 0x130;
    draw.b1 = 0x140; draw.format = 26; draw.width = draw.depthWidth = 1920;
    draw.height = draw.depthHeight = 1080; draw.depthFormat = 45;
    draw.cameraHash = 0x42; draw.camera = reinterpret_cast<const unsigned char*>(camera);
    draw.viewport = draw.cameraCurrent = true;
    census.observe(draw, true);
    const SceneTarget* target = census.find(draw.color);
    if (!target || target->bad || !target->cameraSeen || target->draws != 1) return false;
    // An image-copy-source refusal is external to this diagnostic census.
    // Its camera remains usable only if all actual H writes agree.
    census.observe(draw, true);
    target = census.find(draw.color);
    if (!target || target->bad || target->draws != 2) return false;
    SceneDraw mixed = draw; float other[6][4]{};
    std::memcpy(other, camera, sizeof(other)); other[5][0] = 1;
    mixed.camera = reinterpret_cast<const unsigned char*>(other);
    census.observe(mixed, true);
    if (!census.find(draw.color)->bad ||
        std::strcmp(census.find(draw.color)->reason, "scene-camera-mixed") != 0) return false;
    census.reset();
    SceneDraw stale = draw; stale.cameraCurrent = false;
    census.observe(stale, true);
    if (!census.find(draw.color)->bad) return false;
    census.reset();
    SceneDraw overlay = draw; overlay.overlayProtected = true;
    census.observe(overlay, true);
    if (!census.find(draw.color)->bad) return false;
    census.reset();
    census.observe(draw, true);
    SceneDraw depth = draw; depth.depth = 0x200;
    census.observe(depth, true);
    if (!census.find(draw.color)->bad) return false;
    census.reset();
    census.observe(draw, true);
    census.explicitWrite(draw.color);
    if (!census.find(draw.color)->bad ||
        std::strcmp(census.find(draw.color)->reason, "scene-explicit-resource-write") != 0) return false;
    census.reset();
    SceneDraw viewport = draw; viewport.viewport = false;
    census.observe(viewport, true);
    if (!census.find(draw.color)->bad) return false;
    census.reset();
    SceneDraw missing = draw; missing.camera = nullptr;
    census.observe(missing, true);
    if (!census.find(draw.color) || census.find(draw.color)->cameraSeen) return false;
    census.reset();
    for (unsigned i = 0; i < kSceneTargets + 1; ++i) {
        SceneDraw d = draw; d.color += i;
        census.observe(d, true);
    }
    return census.overflow;
}

bool testCapture() {
    Memory m = fixture();
    auto s = std::make_unique<Snapshot>(); s->source = kC + 0x70;
    capture(&Memory::read, &m, kBase, *s);
    if (!s->complete || !s->consistent || !s->validSource || s->stageCount != 1) return false;
    const Stage& stage = s->stages[0];
    if (!stage.nameRead || std::strcmp(stage.stackName, "MainView") != 0 ||
        stage.activityCount != 1 || !stage.baseRead || !stage.outputRead || stage.outputPose[12] != 42.0f) return false;
    const Activity& a = stage.activities[0];
    if (!a.weightKnown || a.weight != 1 || !a.stateKnown || !a.poseRead ||
        !a.nameRead || std::strcmp(a.name, "Headlook") != 0) return false;

    Memory cycle = fixture(); cycle.put(kC + 0x40, kC + 0x70);
    auto cycleResult = std::make_unique<Snapshot>(); cycleResult->source = kC + 0x70;
    capture(&Memory::read, &cycle, kBase, *cycleResult);
    if (cycleResult->complete || std::strcmp(cycleResult->failure, "cycle") != 0) return false;

    Memory overflow = fixture(); overflow.put(kC + 0x58, uint64_t{kActivities + 1});
    auto overflowResult = std::make_unique<Snapshot>(); overflowResult->source = kC + 0x70;
    capture(&Memory::read, &overflow, kBase, *overflowResult);
    if (overflowResult->complete || std::strcmp(overflowResult->failure, "activity-bound") != 0) return false;

    Memory partial = fixture(); partial.denied = kC + 0x68;
    auto partialResult = std::make_unique<Snapshot>(); partialResult->source = kC + 0x70;
    capture(&Memory::read, &partial, kBase, *partialResult);
    if (partialResult->complete || std::strcmp(partialResult->failure, "stage-fields-read") != 0) return false;

    Memory changing = fixture(); changing.put(kRoster, kActivity + 8);
    auto changingResult = std::make_unique<Snapshot>(); changingResult->source = kC + 0x70;
    capture(&Memory::read, &changing, kBase, *changingResult);
    if (changingResult->complete || std::strcmp(changingResult->failure, "activity-vtable-read") != 0) return false;

    Memory inactive = nestedFixture(0.0f);
    auto inactiveResult = std::make_unique<Snapshot>(); inactiveResult->source = kC + 0x70;
    capture(&Memory::read, &inactive, kBase, *inactiveResult);
    if (!inactiveResult->complete || inactiveResult->stageCount != 1 ||
        inactiveResult->stages[0].inactiveNestedOmitted != 1 ||
        inactiveResult->stages[0].activityCount != 1) return false;

    Memory active = nestedFixture(1.0f);
    auto activeResult = std::make_unique<Snapshot>(); activeResult->source = kC + 0x70;
    capture(&Memory::read, &active, kBase, *activeResult);
    if (!activeResult->complete || activeResult->stageCount != 12 ||
        activeResult->stages[0].inactiveNestedOmitted != 0) return false;

    Memory unknown = nestedFixture(1.0f);
    unknown.put(kBase + 0x5189AD0 + 0x20, kBase + 0x123456);
    auto unknownResult = std::make_unique<Snapshot>(); unknownResult->source = kC + 0x70;
    capture(&Memory::read, &unknown, kBase, *unknownResult);
    if (!unknownResult->complete || unknownResult->stageCount != 12 ||
        unknownResult->stages[0].activities[0].weightKnown) return false;
    return true;
}

bool testPlaced() {
    Memory m = placedFixture();
    auto s = std::make_unique<Snapshot>(); s->source = kC + 0x70;
    capture(&Memory::read, &m, kBase, *s);
    if (!s->complete || s->placedSeen != 1 || s->placedCount != 1 || s->placedOverflow) return false;
    const PlacedDetail& d = s->placed[0];
    if (!d.identityComplete || !d.providerPresent || !d.providerKnown || !d.overridePresent ||
        d.provider != kProvider || d.providerVtable != kBase + 0x530E530 ||
        d.providerFn != kBase + 0x529540 || d.overrideFn != kBase + 0x123456 ||
        d.targetRef != kTarget || !d.targetStateRead || d.targetState != 8 ||
        !d.validatorKnown || !d.valid5 || !d.valid8 || !d.overrideEligible || !d.poseStable ||
        !d.localRead || !d.cacheRead || !d.finalRead ||
        d.local[12] != 10 || d.cache[12] != 20 || d.final[12] != 21) return false;

    Memory noProvider = placedFixture(); noProvider.put(kPlacedRecord + 0x10, uintptr_t{0});
    auto nullResult = std::make_unique<Snapshot>(); nullResult->source = kC + 0x70;
    capture(&Memory::read, &noProvider, kBase, *nullResult);
    if (nullResult->placedCount != 1 || !nullResult->placed[0].identityComplete ||
        nullResult->placed[0].providerPresent ||
        std::strcmp(nullResult->placed[0].reason, "provider-null") != 0) return false;

    Memory unknownProvider = placedFixture();
    unknownProvider.put(kProvider, kBase + 0x5310000);
    unknownProvider.put(kBase + 0x5310000 + 0x20, kBase + 0x100000);
    auto unknownProviderResult = std::make_unique<Snapshot>(); unknownProviderResult->source = kC + 0x70;
    capture(&Memory::read, &unknownProvider, kBase, *unknownProviderResult);
    if (!unknownProviderResult->placed[0].identityComplete ||
        !unknownProviderResult->placed[0].providerPresent ||
        unknownProviderResult->placed[0].providerKnown) return false;

    Memory noRecord = placedFixture(); noRecord.put(kActivity - 0x20 + 0x1E8, uintptr_t{0});
    auto noRecordResult = std::make_unique<Snapshot>(); noRecordResult->source = kC + 0x70;
    capture(&Memory::read, &noRecord, kBase, *noRecordResult);
    if (noRecordResult->placedCount != 1 || !noRecordResult->placed[0].identityComplete ||
        noRecordResult->placed[0].record || noRecordResult->placed[0].providerPresent ||
        noRecordResult->placed[0].localRead || noRecordResult->placed[0].cacheRead ||
        !noRecordResult->placed[0].finalRead || !noRecordResult->placed[0].eligibilityKnown ||
        noRecordResult->placed[0].overrideEligible ||
        std::strcmp(noRecordResult->placed[0].reason, "record-null") != 0) return false;

    Memory noTarget = placedFixture(); noTarget.put(kPlacedRecord + 8, uintptr_t{0});
    auto noTargetResult = std::make_unique<Snapshot>(); noTargetResult->source = kC + 0x70;
    capture(&Memory::read, &noTarget, kBase, *noTargetResult);
    if (!noTargetResult->placed[0].identityComplete || noTargetResult->placed[0].targetRef ||
        noTargetResult->placed[0].targetStateRead || !noTargetResult->placed[0].validatorKnown ||
        !noTargetResult->placed[0].eligibilityKnown ||
        noTargetResult->placed[0].valid5 || noTargetResult->placed[0].valid8 ||
        noTargetResult->placed[0].overrideEligible) return false;

    const int32_t states[] = {-1, 4, 5, 7, 8};
    for (const int32_t state : states) {
        Memory boundary = placedFixture(); boundary.put(kTarget + 0xC0, state);
        auto boundaryResult = std::make_unique<Snapshot>(); boundaryResult->source = kC + 0x70;
        capture(&Memory::read, &boundary, kBase, *boundaryResult);
        const auto& value = boundaryResult->placed[0];
        if (!value.identityComplete || !value.targetStateRead || value.targetState != state ||
            value.valid5 != (state >= 5) || value.valid8 != (state >= 8) ||
            !value.eligibilityKnown || value.overrideEligible != (state >= 8)) return false;
    }

    Memory unreadable = placedFixture(); unreadable.denied = kProvider;
    auto unreadableResult = std::make_unique<Snapshot>(); unreadableResult->source = kC + 0x70;
    capture(&Memory::read, &unreadable, kBase, *unreadableResult);
    if (unreadableResult->placedCount != 1 || unreadableResult->placed[0].identityComplete ||
        std::strcmp(unreadableResult->placed[0].reason, "provider-vtable-read") != 0) return false;

    Memory changed = placedFixture();
    changed.changeOnRead = kPlacedRecord + 0x10;
    changed.changeAddress = kPlacedRecord + 0x10;
    changed.changeValue = kProvider + 8;
    changed.changeAfter = 2;
    auto changedResult = std::make_unique<Snapshot>(); changedResult->source = kC + 0x70;
    capture(&Memory::read, &changed, kBase, *changedResult);
    if (changedResult->placedCount != 1 || changedResult->placed[0].identityComplete ||
        changedResult->placed[0].eligibilityKnown ||
        std::strcmp(changedResult->placed[0].reason, "identity-changed") != 0) return false;

    Memory movingPose = placedFixture();
    movingPose.changeOnRead = kPlacedRecord + 0x20;
    movingPose.changeAddress = kPlacedRecord + 0x20;
    movingPose.changeValue = uintptr_t{0};
    movingPose.changeAfter = 2;
    auto movingResult = std::make_unique<Snapshot>(); movingResult->source = kC + 0x70;
    capture(&Memory::read, &movingPose, kBase, *movingResult);
    if (movingResult->placedCount != 1 || !movingResult->placed[0].identityComplete ||
        !movingResult->placed[0].localRead || movingResult->placed[0].poseStable) return false;

    Memory targetUnreadable = placedFixture(); targetUnreadable.denied = kTarget + 0xC0;
    auto targetResult = std::make_unique<Snapshot>(); targetResult->source = kC + 0x70;
    capture(&Memory::read, &targetUnreadable, kBase, *targetResult);
    if (targetResult->placedCount != 1 || targetResult->placed[0].identityComplete ||
        targetResult->placed[0].validatorKnown || targetResult->placed[0].eligibilityKnown ||
        std::strcmp(targetResult->placed[0].reason, "target-state-read") != 0) return false;

    Memory targetUnreadableButQAbsent = placedFixture();
    targetUnreadableButQAbsent.denied = kTarget + 0xC0;
    targetUnreadableButQAbsent.put(kActivity - 0x20 + 0x190, uintptr_t{0});
    auto knownFalseResult = std::make_unique<Snapshot>(); knownFalseResult->source = kC + 0x70;
    capture(&Memory::read, &targetUnreadableButQAbsent, kBase, *knownFalseResult);
    if (!knownFalseResult->placed[0].eligibilityKnown || knownFalseResult->placed[0].overrideEligible ||
        knownFalseResult->placed[0].identityComplete) return false;

    Memory targetUnreadableButCfgZero = placedFixture();
    targetUnreadableButCfgZero.denied = kTarget + 0xC0;
    targetUnreadableButCfgZero.put(kConfig + 0x14, int32_t{0});
    auto cfgFalseResult = std::make_unique<Snapshot>(); cfgFalseResult->source = kC + 0x70;
    capture(&Memory::read, &targetUnreadableButCfgZero, kBase, *cfgFalseResult);
    if (!cfgFalseResult->placed[0].eligibilityKnown || cfgFalseResult->placed[0].overrideEligible ||
        cfgFalseResult->placed[0].identityComplete) return false;

    Memory cfg18 = placedFixture(); cfg18.put(kConfig + 0x18, int32_t{1});
    auto cfg18Result = std::make_unique<Snapshot>(); cfg18Result->source = kC + 0x70;
    capture(&Memory::read, &cfg18, kBase, *cfg18Result);
    if (!cfg18Result->placed[0].identityComplete || cfg18Result->placed[0].config18 != 1 ||
        !cfg18Result->placed[0].overrideEligible) return false;

    Memory rootGuard = placedFixture(); rootGuard.put(kActivity - 0x20, kBase + 0x12345);
    auto guardResult = std::make_unique<Snapshot>(); guardResult->source = kC + 0x70;
    capture(&Memory::read, &rootGuard, kBase, *guardResult);
    if (guardResult->placedSeen != 1 || guardResult->placed[0].identityComplete ||
        std::strcmp(guardResult->placed[0].reason, "placed-root-guard-refused") != 0) return false;

    Memory noOverride = placedFixture(); noOverride.put(kActivity - 0x20 + 0x190, uintptr_t{0});
    auto noOverrideResult = std::make_unique<Snapshot>(); noOverrideResult->source = kC + 0x70;
    capture(&Memory::read, &noOverride, kBase, *noOverrideResult);
    if (noOverrideResult->placedCount != 1 || !noOverrideResult->placed[0].identityComplete ||
        noOverrideResult->placed[0].overridePresent || !noOverrideResult->placed[0].eligibilityKnown ||
        noOverrideResult->placed[0].overrideEligible) return false;

    Memory noPlaced = fixture();
    auto absentResult = std::make_unique<Snapshot>(); absentResult->source = kC + 0x70;
    capture(&Memory::read, &noPlaced, kBase, *absentResult);
    if (!absentResult->complete || absentResult->placedSeen || absentResult->placedCount) return false;

    auto bounded = std::make_unique<Snapshot>();
    bounded->placedCount = kPlacedDetails;
    bounded->placedSeen = kPlacedDetails;
    placedDetail(&Memory::read, &m, kBase, s->stages[0].activities[0], 0, 0, *bounded);
    if (!bounded->placedOverflow || bounded->placedSeen != kPlacedDetails + 1 ||
        bounded->placedCount != kPlacedDetails) return false;
    return true;
}

std::string sourceText(const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (!file) return {};
    std::string source; char bytes[4096]; size_t count;
    while ((count=std::fread(bytes,1,sizeof(bytes),file))!=0) source.append(bytes,count);
    const bool okay=std::ferror(file)==0; std::fclose(file);
    return okay?source:std::string{};
}
bool firstGuard(const std::string& source, const char* name) {
    const auto at=source.find(name);
    if(at==std::string::npos)return false;
    const auto brace=source.find('{',at);
    if(brace==std::string::npos)return false;
    const auto first=source.find_first_not_of(" \r\n\t",brace+1);
    const std::string guard="if (!flatOriginCaptureEnabled()) return";
    return first!=std::string::npos && source.compare(first,guard.size(),guard)==0;
}
bool startupOrder(const std::string& source) {
    const auto commit=source.find("tkModeWatch.run([] { modeWatchFrameBoundary(); });");
    const auto latch=source.find("flatOriginCaptureInitialize(runtimeFlatProfile() && Config::get().getBool(\"diag.flat_origin_capture\", false))");
    const auto clock=source.find("tkEngineVelocityClock.run(");
    const auto tick=source.find("tkFlatOriginCapture.run([] {");
    const auto end=tick==std::string::npos?std::string::npos:source.find("});",tick);
    const auto declaration=source.find("EDVR_BOUNDARY_TICK(tkFlatOriginCapture, \"flat_origin_capture\");");
    return commit!=std::string::npos && latch!=std::string::npos && clock!=std::string::npos &&
        tick!=std::string::npos && end!=std::string::npos && declaration!=std::string::npos &&
        commit<tick && tick<latch && latch<end && end<clock;
}
bool testCaptureWiring() {
    const auto capture=sourceText("src/d3d11/flat_stationary_camera_capture.cpp");
    const char* entries[]={"State* ensureState()", "void flatStationaryCameraFrame(",
        "void flatStationaryCameraSceneCandidate(", "void flatStationaryCameraSceneRejected(",
        "void flatStationaryCameraSelected(", "void flatStationaryCameraRefresh("};
    for(const auto* name:entries) {
        if(!firstGuard(capture,name))return false;
        auto mutant=capture;const auto start=mutant.find(name);const auto at=mutant.find("if (!flatOriginCaptureEnabled()) return",start);
        mutant.erase(at,std::strlen("if (!flatOriginCaptureEnabled()) return"));
        if(firstGuard(mutant,name))return false;
    }
    const auto runtime=sourceText("src/d3d11/flat_runtime.cpp");
    if(!firstGuard(runtime,"static void observePassiveSceneDraw(") ||
       !firstGuard(runtime,"static void passiveSceneAtHdrTrigger(") ||
       runtime.find("if (flatOriginCaptureEnabled()) s.passiveScene.explicitWrite(")==std::string::npos)return false;
    const auto device=sourceText("src/d3d11/device_hook.cpp");
    if(!startupOrder(device))return false;
    auto early=device;const auto at=early.find("flatOriginCaptureInitialize(");
    const auto end=early.find(';',at)+1;const auto statement=early.substr(at,end-at);
    early.erase(at,end-at);early.insert(0,statement);
    if(startupOrder(early))return false;
    auto shared=device;const auto tick=shared.find("tkFlatOriginCapture.run(");
    shared.replace(tick,std::strlen("tkFlatOriginCapture.run("),"tkModeWatch.run(");
    return !startupOrder(shared); // diagnostic faults must not spend the mode-watch budget
}

bool testReportMarkers() {
    // The one-shot report must distinguish hook-never-ran and missing/partial
    // Placed detail from a successful renderer-source identity capture.
    std::FILE* file = std::fopen("src/d3d11/flat_stationary_camera_capture.cpp", "rb");
    if (!file) return false;
    std::string source;
    char buffer[4096];
    size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) != 0) source.append(buffer, count);
    const bool readOkay = std::ferror(file) == 0;
    std::fclose(file);
    return readOkay &&
        source.find("renderer-hook-never-ran") != std::string::npos &&
        source.find("Placed-not-observed") != std::string::npos &&
        source.find("Placed-bound") != std::string::npos &&
        source.find("Placed-identity-incomplete") != std::string::npos &&
        source.find("detail-complete=%u") != std::string::npos &&
        source.find("placed-overflow=%u") != std::string::npos &&
        source.find("flat stationary Placed-matrix[") != std::string::npos &&
        source.find("passive-scene-never-qualified") != std::string::npos &&
        source.find("post-arm-no-passive-scene-witness") != std::string::npos &&
        source.find("passive-scene=%llu selected=%llu") != std::string::npos &&
        source.find("refusal=%s scene-refusal=%s") != std::string::npos &&
        source.find("selected GPU donor UNPROVED") != std::string::npos;
}

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) return 2;
    if (!testCaptureControl() || !testGate() || !testCompletedSceneGate() || !testPassiveSceneWitness() || !testCapture() || !testPlaced() || !testCaptureWiring() || !testReportMarkers()) {
        std::fprintf(stderr, "flat stationary camera test failed\n"); return 1;
    }
    std::puts("flat stationary camera test passed");
    return 0;
}
