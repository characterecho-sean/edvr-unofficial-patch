#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Pure, bounded decoding of the game's camera pipeline. The caller supplies
// guarded reads; this code never invokes a game method or changes game memory.
namespace edvr { namespace stationcam {

constexpr unsigned kStages = 96; // root and several nested eleven-child stacks
constexpr unsigned kActivities = 16;
constexpr unsigned kPlacedDetails = 32;
constexpr unsigned kNameBytes = 48;
using Read = bool (*)(void*, uintptr_t, void*, size_t) noexcept;

struct ParkInput {
    bool active = false, gameplay = false, dockedKnown = false, docked = false;
    bool seatedKnown = false, seated = false, guiKnown = false;
    uint32_t gui = 0, statusSamples = 0;
    bool sceneWitness = false, menu = false, reset = false; // reset is metadata; current rows remain valid
    uint64_t frame = 0;
    uintptr_t depth = 0;
    float origin[3]{};
};
inline bool qualifiedScene(const ParkInput& in) noexcept {
    if (!in.active || !in.gameplay || !in.dockedKnown || !in.docked ||
        !in.seatedKnown || !in.seated || !in.guiKnown || in.gui != 0 ||
        !in.sceneWitness || in.menu || !in.depth) return false;
    for (unsigned i = 0; i < 3; ++i) if (!std::isfinite(in.origin[i])) return false;
    return true;
}
struct ParkGate {
    uint64_t lastFrame = 0;
    uint32_t lastSample = 0, fresh = 0, stable = 0;
    bool sampled = false;
    const char* reason = "no passive scene witness";
    bool step(const ParkInput& in) noexcept {
        const bool good = qualifiedScene(in);
        if (!good) {
            stable = fresh = 0; lastFrame = 0;
            lastSample = in.statusSamples; sampled = true;
            reason = !in.active ? "journal-watcher-inactive" : !in.gameplay ? "no-LoadGame-in-this-process" :
                !in.dockedKnown ? "docked-unknown" : !in.docked ? "not-docked" :
                !in.seatedKnown ? "seated-unknown" : !in.seated ? "not-seated" :
                !in.guiKnown ? "GuiFocus-unknown" : in.gui != 0 ? "GuiFocus-not-gameplay" :
                !in.sceneWitness ? "no-passive-scene-witness" : in.menu ? "menu-scene-contract" :
                !in.depth ? "no-scene-depth" : "scene-origin-nonfinite";
            return false;
        }
        // A missed completed frame loses both budgets. Fresh samples from
        // before an observer gap must not qualify a later parked scene.
        if (lastFrame && lastFrame + 1 != in.frame) {
            stable = fresh = 0;
            lastSample = in.statusSamples; sampled = true;
        }
        if (!sampled) { lastSample = in.statusSamples; sampled = true; }
        else if (in.statusSamples != lastSample) {
            lastSample = in.statusSamples;
            if (fresh < 2) ++fresh;
        }
        if (lastFrame + 1 == in.frame) ++stable;
        else stable = 1;
        lastFrame = in.frame;
        reason = fresh < 2 ? "waiting-for-two-fresh-Status-samples" :
            stable < 30 ? "waiting-for-30-qualified-scene-frames" : "qualified";
        return fresh >= 2 && stable >= 30;
    }
};

// Present finalizes a frame once. Callbacks only accumulate evidence;
// rejection dominates every candidate in the same frame, in either order.
struct CompletedScene {
    bool processed = false, qualified = false, ready = false;
    const char* sceneRefusal = "no-trigger-or-camera-witness";
    float origin[3]{};
};
struct SceneFrameGate {
    ParkGate park;
    uint64_t pendingFrame = 0, completedFrame = 0;
    bool pending = false, candidateSeen = false, rejected = false, completed = false;
    uintptr_t depth = 0;
    float origin[3]{};
    const char* refusal = "none";

    bool acceptFrame(uint64_t frame) noexcept {
        if ((completed && frame <= completedFrame) || (pending && frame < pendingFrame)) return false;
        if (!pending || frame != pendingFrame) {
            pending = true; pendingFrame = frame;
            candidateSeen = rejected = false; depth = 0; refusal = "none";
        }
        return true;
    }
    bool candidate(uint64_t frame, const float point[3], uintptr_t sceneDepth) noexcept {
        if (!acceptFrame(frame)) return false;
        // Repeated callbacks cannot rescue a rejected frame or hide an invalid
        // first witness behind a later valid one.
        bool finite = point != nullptr;
        if (point) for (unsigned i = 0; i < 3; ++i) finite = finite && std::isfinite(point[i]);
        if (!finite || !sceneDepth) {
            rejected = true; refusal = !finite ? "scene-origin-nonfinite" : "no-scene-depth";
        }
        if (!candidateSeen && point) {
            for (unsigned i = 0; i < 3; ++i) origin[i] = point[i];
            depth = sceneDepth;
        }
        candidateSeen = true;
        return true;
    }
    bool reject(uint64_t frame, const char* reason) noexcept {
        if (!acceptFrame(frame)) return false;
        if (!rejected) refusal = reason ? reason : "passive-scene-unknown";
        rejected = true;
        return true;
    }
    CompletedScene finish(uint64_t frame, ParkInput input) noexcept {
        CompletedScene result;
        if (completed && frame <= completedFrame) return result;
        completed = true; completedFrame = frame; result.processed = true;
        const bool current = pending && pendingFrame == frame;
        input.frame = frame;
        input.sceneWitness = current && candidateSeen && !rejected;
        input.depth = input.sceneWitness ? depth : 0;
        if (input.sceneWitness) {
            for (unsigned i = 0; i < 3; ++i) input.origin[i] = result.origin[i] = origin[i];
            result.sceneRefusal = "none";
        } else if (current && rejected) result.sceneRefusal = refusal;
        result.qualified = qualifiedScene(input);
        // Missing witnesses execute the same reset as explicit bad frames.
        result.ready = park.step(input);
        if (current) pending = false;
        return result;
    }
};

// Independent, passive summary of writes to each observed H candidate. The
// renderer's firstBad retains only one conflict, so it cannot certify that a
// later camera or layout conflict did not follow an image-source refusal.
constexpr unsigned kSceneTargets = 4;
struct SceneDraw {
    uintptr_t color = 0, rtv = 0, depth = 0, dsv = 0, b1 = 0;
    uint32_t format = 0, width = 0, height = 0, depthWidth = 0, depthHeight = 0, depthFormat = 0;
    uint64_t cameraHash = 0;
    const unsigned char* camera = nullptr;
    bool viewport = false, cameraCurrent = false, overlayProtected = false;
};
struct SceneTarget {
    uintptr_t color = 0, rtv = 0, depth = 0, dsv = 0, b1 = 0;
    uint32_t width = 0, height = 0, depthFormat = 0, draws = 0;
    uint64_t cameraHash = 0;
    unsigned char camera[6 * 4 * sizeof(float)]{};
    bool cameraSeen = false, bad = false;
    const char* reason = "none";
    void observe(const SceneDraw& d) noexcept {
        ++draws;
        if (bad) return;
        if (d.overlayProtected) { bad = true; reason = "scene-overlay-branch-unproven"; return; }
        if (d.format != 26 || !d.rtv || !d.depth || !d.dsv || !d.depthFormat ||
            !d.width || !d.height || d.depthWidth != d.width || d.depthHeight != d.height ||
            !d.viewport) { bad = true; reason = "scene-layout-or-viewport"; return; }
        if (draws == 1) {
            rtv = d.rtv; depth = d.depth; dsv = d.dsv;
            width = d.width; height = d.height; depthFormat = d.depthFormat;
        } else if (rtv != d.rtv || depth != d.depth || dsv != d.dsv ||
                   width != d.width || height != d.height || depthFormat != d.depthFormat) {
            bad = true; reason = "scene-layout-changed"; return;
        }
        if (!d.camera) return;
        if (!d.cameraCurrent || !d.b1) { bad = true; reason = "scene-camera-stale"; return; }
        if (!cameraSeen) {
            cameraSeen = true; b1 = d.b1; cameraHash = d.cameraHash;
            std::memcpy(camera, d.camera, sizeof(camera));
        } else if (b1 != d.b1 || cameraHash != d.cameraHash ||
                   std::memcmp(camera, d.camera, sizeof(camera)) != 0) {
            bad = true; reason = "scene-camera-mixed";
        }
    }
};
struct SceneWitnesses {
    SceneTarget targets[kSceneTargets]{};
    unsigned used = 0;
    bool overflow = false;
    void reset() noexcept { *this = SceneWitnesses{}; }
    const SceneTarget* find(uintptr_t color) const noexcept {
        for (unsigned i = 0; i < used; ++i) if (targets[i].color == color) return &targets[i];
        return nullptr;
    }
    void observe(const SceneDraw& d, bool hdrCandidate) noexcept {
        if (!d.color) return;
        SceneTarget* target = nullptr;
        for (unsigned i = 0; i < used; ++i) if (targets[i].color == d.color) { target = &targets[i]; break; }
        if (!target && hdrCandidate) {
            if (used == kSceneTargets) { overflow = true; return; }
            target = &targets[used++]; target->color = d.color;
        }
        if (target) target->observe(d);
    }
    void explicitWrite(uintptr_t color) noexcept {
        for (unsigned i = 0; i < used; ++i) if (targets[i].color == color) {
            targets[i].bad = true; targets[i].reason = "scene-explicit-resource-write";
        }
    }
};

struct Activity {
    uintptr_t iface = 0, vtable = 0, concreteVtable = 0, weightFn = 0, stateFn = 0, state = 0;
    char name[kNameBytes]{};
    bool nameRead = false;
    float weight = 0;
    bool weightKnown = false, stateKnown = false, poseRead = false;
    float pose[16]{}; // three basis rows and positive origin
};
struct Stage {
    uintptr_t child = 0, stack = 0, predecessor = 0, nestedParent = 0, activityArray = 0;
    uintptr_t stackVtable = 0;
    uint64_t activityCount = 0;
    uint32_t mode = 0;
    uint32_t inactiveNestedOmitted = 0;
    int parent = -1; // -1 renderer source, otherwise a predecessor/nested edge
    char edge = 'R';
    char stackName[kNameBytes]{};
    bool nameRead = false, baseRead = false, outputRead = false;
    float basePose[16]{}, outputPose[16]{};
    Activity activities[kActivities]{};
};
struct PlacedDetail {
    unsigned stage = 0, activity = 0;
    uintptr_t iface = 0, object = 0, record = 0, provider = 0;
    uintptr_t providerVtable = 0, providerFn = 0;
    uintptr_t config = 0, overrideObject = 0, overrideVtable = 0, overrideFn = 0;
    uintptr_t targetRef = 0;
    int32_t overrideHandle = 0, config14 = 0, config18 = 0, targetState = 0;
    bool identityComplete = false, recordRead = false;
    bool providerPresent = false, providerKnown = false, overridePresent = false;
    bool targetStateRead = false, validatorKnown = false, valid5 = false, valid8 = false;
    bool eligibilityKnown = false, overrideEligible = false;
    bool localRead = false, cacheRead = false, finalRead = false;
    bool poseRereadComplete = false, poseStable = false;
    bool localFinite = false, cacheFinite = false, finalFinite = false;
    float local[16]{}, cache[16]{}, final[16]{};
    const char* reason = "none";
};
struct Snapshot {
    uintptr_t owner = 0, source = 0, helper = 0, rootChild = 0;
    uint64_t callerRva = 0;
    float sourceBefore[4]{}, sourceAfter[4]{}, helperAfter[4]{};
    bool sourceBeforeRead = false, sourceAfterRead = false, helperAfterRead = false;
    bool validSource = false, consistent = true, complete = true;
    const char* failure = "none";
    unsigned stageCount = 0;
    unsigned placedCount = 0, placedSeen = 0;
    bool placedOverflow = false;
    PlacedDetail placed[kPlacedDetails]{};
    Stage stages[kStages]{};
};

template<class T> inline bool get(Read read, void* context, uintptr_t at, T& value) noexcept {
    return at != 0 && read(context, at, &value, sizeof(value));
}
inline bool bytes(Read read, void* context, uintptr_t at, void* value, size_t count) noexcept {
    return at != 0 && read(context, at, value, count);
}
inline bool spanFits(uintptr_t address, size_t offset, size_t length) noexcept {
    return address && offset <= UINTPTR_MAX - address && length <= UINTPTR_MAX - (address + offset);
}
inline bool finitePose(const float pose[16]) noexcept {
    for (unsigned i = 0; i < 16; ++i) if (!std::isfinite(pose[i])) return false;
    return true;
}
inline bool pose(Read read, void* context, uintptr_t state, float out[16]) noexcept {
    return state && bytes(read, context, state + 0x40, out, sizeof(float) * 16) && finitePose(out);
}
inline uintptr_t rva(uintptr_t address, uintptr_t base) noexcept {
    return address >= base && address - base < 0x6000000 ? address - base : 0;
}
inline bool knownPlacedProvider(uintptr_t base, uintptr_t vtable, uintptr_t fn) noexcept {
    constexpr uintptr_t known[][2] = {
        {0x51245B8,0x4899EF4}, {0x5124928,0x1F2370},
        {0x530B340,0x5293E0}, {0x5355318,0x30B9160},
        {0x5372368,0xF82590}, {0x5538640,0x5CB1F0},
        {0x553A8D8,0x3CB93F0}, {0x553AD70,0xF8D000}
    };
    const uintptr_t vt = rva(vtable, base), code = rva(fn, base);
    for (const auto& item : known) if (vt == item[0] && code == item[1]) return true;
    return false;
}
inline bool childGuard(Read read, void* context, uintptr_t base, uintptr_t child) noexcept {
    uintptr_t v0 = 0, v1 = 0;
    return child && get(read, context, child, v0) && get(read, context, child + 0x10, v1) &&
        v0 == base + 0x5185F90 && v1 == base + 0x5185F98;
}
inline void fail(Snapshot& out, const char* why) noexcept {
    out.complete = false;
    if (std::strcmp(out.failure, "none") == 0) out.failure = why;
}
inline bool readName(Read read, void* context, uintptr_t stack, char out[kNameBytes]) noexcept {
    uintptr_t object = 0;
    uint64_t length = 0, capacity = 0;
    if (!get(read, context, stack + 0x20, object) || !object ||
        !get(read, context, object, length) || !get(read, context, object + 8, capacity) ||
        length >= kNameBytes || length > capacity || capacity > 4096) return false;
    if (!bytes(read, context, object + 0x14, out, static_cast<size_t>(length))) return false;
    out[length] = 0;
    for (unsigned i = 0; i < length; ++i) if (static_cast<unsigned char>(out[i]) < 0x20 ||
                                                static_cast<unsigned char>(out[i]) > 0x7e) return false;
    return true;
}
inline bool readCString(Read read, void* context, uintptr_t at, char out[kNameBytes]) noexcept {
    if (!at) return false;
    for (unsigned i = 0; i < kNameBytes - 1; ++i) {
        if (!get(read, context, at + i, out[i])) return false;
        if (!out[i]) return true;
        if (static_cast<unsigned char>(out[i]) < 0x20 || static_cast<unsigned char>(out[i]) > 0x7e) return false;
    }
    out[kNameBytes - 1] = 0;
    return false;
}

inline void activity(Read read, void* context, uintptr_t base, uintptr_t iface, Activity& out,
                     Snapshot& snapshot) noexcept {
    out.iface = iface;
    if (!iface || !get(read, context, iface, out.vtable) || !out.vtable ||
        !get(read, context, out.vtable + 0x20, out.weightFn) ||
        !get(read, context, out.vtable + 0x28, out.stateFn)) {
        fail(snapshot, "activity-vtable-read"); return;
    }
    if (out.weightFn == base + 0x8BE920) {
        uint8_t enabled = 0;
        if (iface < 4 || !get(read, context, iface - 4, enabled)) fail(snapshot, "leaf-weight-read");
        else { out.weightKnown = true; out.weight = enabled ? 1.0f : 0.0f; }
    } else if (out.weightFn == base + 0x108ADE0) {
        uintptr_t config = 0;
        if (iface < 0xA0 || !get(read, context, iface - 0xA0, config) ||
            !get(read, context, config + 0x34, out.weight) || !std::isfinite(out.weight))
            fail(snapshot, "nested-weight-read");
        else out.weightKnown = true;
    }
    if (out.stateFn == base + 0x5293E0) {
        out.state = iface + 0x10;
        out.stateKnown = true;
        uintptr_t name = 0;
        if (get(read, context, iface + 8, name)) out.nameRead = readCString(read, context, name, out.name);
        if (iface >= 0x20) get(read, context, iface - 0x20, out.concreteVtable);
    } else if (out.stateFn == base + 0x108A98C) {
        if (out.vtable != base + 0x51869B0) { fail(snapshot, "nested-activity-vtable"); return; }
        uintptr_t nestedChild = 0;
        if (iface < 0x98 || !get(read, context, iface - 0x98 + 0x78, nestedChild) ||
            !childGuard(read, context, base, nestedChild)) fail(snapshot, "nested-state-read");
        else { out.state = nestedChild + 0x70; out.stateKnown = true; }
    }
    if (out.stateKnown) {
        out.poseRead = pose(read, context, out.state, out.pose);
        if (!out.poseRead) fail(snapshot, "activity-pose-read");
    }
}

// This is a passive snapshot of Placed's parent-provider identities and raw
// poses. A second read detects identity changes; pose drift is reported on its
// own because a parked camera can still animate between the two reads.
inline void placedDetail(Read read, void* context, uintptr_t base, const Activity& a,
                         unsigned stage, unsigned activityIndex, Snapshot& snapshot) noexcept {
    if (a.vtable != base + 0x5189EA8) return;
    ++snapshot.placedSeen;
    if (snapshot.placedCount == kPlacedDetails) { snapshot.placedOverflow = true; return; }
    PlacedDetail& d = snapshot.placed[snapshot.placedCount++];
    d.stage = stage; d.activity = activityIndex; d.iface = a.iface;
    if (a.iface < 0x20) { d.reason = "object-underflow"; return; }
    d.object = a.iface - 0x20;
    if (a.concreteVtable != base + 0x5189E38 || a.stateFn != base + 0x5293E0) {
        d.reason = "placed-root-guard-refused"; return;
    }
    if (!spanFits(d.object, 0x1E8, sizeof(uintptr_t)) ||
        !spanFits(d.iface, 0x28, sizeof(uintptr_t))) {
        d.reason = "address-overflow"; return;
    }
    uintptr_t objectVtable = 0, ifaceVtable = 0, stateFn = 0;
    if (!get(read, context, d.object, objectVtable) || objectVtable != base + 0x5189E38 ||
        !get(read, context, d.iface, ifaceVtable) || ifaceVtable != base + 0x5189EA8 ||
        !get(read, context, ifaceVtable + 0x28, stateFn) || stateFn != base + 0x5293E0) {
        d.reason = "placed-guard-read"; return;
    }
    if (!get(read, context, d.object + 0x1E8, d.record)) {
        d.reason = "record-unreadable"; return;
    }
    d.recordRead = true;
    if ((d.record && !spanFits(d.record, 0x60, sizeof(d.cache))) ||
        !spanFits(d.object, 0x190, sizeof(uintptr_t))) {
        d.reason = "address-overflow"; return;
    }
    if ((d.record && (!get(read, context, d.record + 0x10, d.provider) ||
        !get(read, context, d.record + 8, d.targetRef))) ||
        !get(read, context, d.object + 0x10, d.config) ||
        !get(read, context, d.object + 0x188, d.overrideHandle) ||
        !get(read, context, d.object + 0x190, d.overrideObject)) {
        d.reason = "placed-identity-read"; return;
    }
    d.providerPresent = d.provider != 0;
    d.overridePresent = d.overrideObject != 0;
    if ((d.providerPresent && !spanFits(d.provider, 0, sizeof(uintptr_t))) ||
        (d.overridePresent && !spanFits(d.overrideObject, 0, sizeof(uintptr_t))) ||
        (d.config && !spanFits(d.config, 0x18, sizeof(int32_t))) ||
        (d.targetRef && !spanFits(d.targetRef, 0xC0, sizeof(int32_t)))) {
        d.reason = "address-overflow"; return;
    }
    if (d.providerPresent && (!get(read, context, d.provider, d.providerVtable) ||
        !spanFits(d.providerVtable, 0x20, sizeof(uintptr_t)) ||
        !get(read, context, d.providerVtable + 0x20, d.providerFn))) {
        d.reason = "provider-vtable-read"; return;
    }
    if (d.overridePresent && (!get(read, context, d.overrideObject, d.overrideVtable) ||
        !spanFits(d.overrideVtable, 0x70, sizeof(uintptr_t)) ||
        !get(read, context, d.overrideVtable + 0x70, d.overrideFn))) {
        d.reason = "override-vtable-read"; return;
    }
    if (!d.config || !get(read, context, d.config + 0x14, d.config14) ||
        !get(read, context, d.config + 0x18, d.config18)) {
        d.eligibilityKnown = d.overrideHandle != -1 || !d.overridePresent;
        d.reason = "config-flags-read"; return;
    }
    if (d.targetRef) {
        if (!get(read, context, d.targetRef + 0xC0, d.targetState)) {
            d.eligibilityKnown = d.overrideHandle != -1 || !d.overridePresent || d.config14 == 0;
            d.reason = "target-state-read"; return;
        }
        d.targetStateRead = true;
        d.valid5 = d.targetState >= 5;
        d.valid8 = d.targetState >= 8;
    }
    d.validatorKnown = true; // a read null R is a known false result
    d.eligibilityKnown = true; // null E/R is a known false valid8 predicate
    d.overrideEligible = d.overrideHandle == -1 && d.overridePresent && d.config14 != 0 && d.valid8;
    if (d.record) {
        d.localRead = bytes(read, context, d.record + 0x20, d.local, sizeof(d.local));
        d.cacheRead = bytes(read, context, d.record + 0x60, d.cache, sizeof(d.cache));
    }
    d.finalRead = bytes(read, context, d.object + 0x70, d.final, sizeof(d.final));
    d.localFinite = d.localRead && finitePose(d.local);
    d.cacheFinite = d.cacheRead && finitePose(d.cache);
    d.finalFinite = d.finalRead && finitePose(d.final);

    uintptr_t objectAgain = 0, ifaceAgain = 0, fnAgain = 0, recordAgain = 0;
    uintptr_t providerAgain = 0, providerVtableAgain = 0, providerFnAgain = 0;
    uintptr_t targetAgain = 0, configAgain = 0, overrideAgain = 0;
    uintptr_t overrideVtableAgain = 0, overrideFnAgain = 0;
    int32_t handleAgain = 0, config14Again = 0, config18Again = 0, targetStateAgain = 0;
    if (!get(read, context, d.object, objectAgain) ||
        !get(read, context, d.iface, ifaceAgain) ||
        !get(read, context, ifaceVtable + 0x28, fnAgain) ||
        !get(read, context, d.object + 0x1E8, recordAgain) ||
        (d.record && (!get(read, context, d.record + 0x10, providerAgain) ||
            !get(read, context, d.record + 8, targetAgain))) ||
        !get(read, context, d.object + 0x10, configAgain) ||
        !get(read, context, d.object + 0x188, handleAgain) ||
        !get(read, context, d.object + 0x190, overrideAgain) ||
        !get(read, context, d.config + 0x14, config14Again) ||
        !get(read, context, d.config + 0x18, config18Again) ||
        (d.providerPresent && (!get(read, context, d.provider, providerVtableAgain) ||
            !get(read, context, d.providerVtable + 0x20, providerFnAgain))) ||
        (d.overridePresent && (!get(read, context, d.overrideObject, overrideVtableAgain) ||
            !get(read, context, d.overrideVtable + 0x70, overrideFnAgain))) ||
        (d.targetRef && !get(read, context, d.targetRef + 0xC0, targetStateAgain))) {
        d.eligibilityKnown = false;
        d.reason = "identity-reread-failed"; return;
    }
    if (objectAgain != objectVtable || ifaceAgain != ifaceVtable || fnAgain != stateFn ||
        recordAgain != d.record || providerAgain != d.provider || targetAgain != d.targetRef ||
        configAgain != d.config || handleAgain != d.overrideHandle || overrideAgain != d.overrideObject ||
        config14Again != d.config14 || config18Again != d.config18 ||
        (d.providerPresent && (providerVtableAgain != d.providerVtable || providerFnAgain != d.providerFn)) ||
        (d.overridePresent && (overrideVtableAgain != d.overrideVtable || overrideFnAgain != d.overrideFn)) ||
        (d.targetRef && targetStateAgain != d.targetState)) {
        d.eligibilityKnown = false;
        d.reason = "identity-changed"; return;
    }
    d.identityComplete = true;
    d.providerKnown = d.providerPresent && knownPlacedProvider(base, d.providerVtable, d.providerFn);
    d.reason = !d.record ? "record-null" : d.providerPresent ? "none" : "provider-null";
    float localAgain[16]{}, cacheAgain[16]{}, finalAgain[16]{};
    const bool localAgainRead = !d.record || bytes(read, context, d.record + 0x20, localAgain, sizeof(localAgain));
    const bool cacheAgainRead = !d.record || bytes(read, context, d.record + 0x60, cacheAgain, sizeof(cacheAgain));
    const bool finalAgainRead = bytes(read, context, d.object + 0x70, finalAgain, sizeof(finalAgain));
    d.poseRereadComplete = localAgainRead && cacheAgainRead && finalAgainRead;
    d.poseStable = (!d.record || (d.localRead && d.cacheRead)) && d.finalRead && d.poseRereadComplete &&
        (!d.record || std::memcmp(d.local, localAgain, sizeof(d.local)) == 0) &&
        (!d.record || std::memcmp(d.cache, cacheAgain, sizeof(d.cache)) == 0) &&
        std::memcmp(d.final, finalAgain, sizeof(d.final)) == 0;
}

inline void walk(Read read, void* context, uintptr_t base, uintptr_t child, int parent,
                 char edge, Snapshot& out) noexcept {
    if (!childGuard(read, context, base, child)) { fail(out, "child-guard"); return; }
    for (unsigned i = 0; i < out.stageCount; ++i) if (out.stages[i].child == child) {
        for (int p = parent; p >= 0; p = out.stages[p].parent)
            if (p == static_cast<int>(i)) { fail(out, "cycle"); return; }
        return; // an inherited parent already present in the bounded roster
    }
    if (out.stageCount == kStages) { fail(out, "stage-bound"); return; }
    const unsigned index = out.stageCount++;
    Stage& stage = out.stages[index];
    stage.child = child; stage.parent = parent; stage.edge = edge;
    uintptr_t terminal = 0;
    if (!get(read, context, child + 0x30, stage.stack) || !stage.stack ||
        !get(read, context, stage.stack, stage.stackVtable) ||
        (stage.stackVtable != base + 0x5186290 && stage.stackVtable != base + 0x5186938) ||
        !get(read, context, stage.stack + 0x78, terminal) || !childGuard(read, context, base, terminal)) {
        fail(out, "stack-identity"); return;
    }
    unsigned membership = 0;
    for (unsigned j = 0; j < 11; ++j) {
        uintptr_t memberChild = 0;
        if (!get(read, context, stage.stack + 0x28 + 8 * j, memberChild)) {
            fail(out, "child-array-read"); return;
        }
        membership += memberChild == child ? 1u : 0u;
    }
    if (membership != 1) { fail(out, "child-membership"); return; }
    if (edge == 'R' || edge == 'N') {
        if (terminal != child) { fail(out, "provider-not-terminal"); return; }
    }
    stage.nameRead = readName(read, context, stage.stack, stage.stackName);
    if (!stage.nameRead) fail(out, "stack-name-read");
    if (!get(read, context, child + 0x40, stage.predecessor) ||
        !get(read, context, child + 0x58, stage.activityCount) ||
        !get(read, context, child + 0x60, stage.activityArray) ||
        !get(read, context, child + 0x68, stage.mode)) {
        fail(out, "stage-fields-read"); return;
    }
    stage.outputRead = pose(read, context, child + 0x70, stage.outputPose);
    stage.baseRead = pose(read, context, stage.predecessor, stage.basePose);
    if (!stage.outputRead || !stage.baseRead) fail(out, "stage-pose-read");
    if (stage.activityCount > kActivities) { fail(out, "activity-bound"); return; }
    if (stage.activityCount && !stage.activityArray) { fail(out, "activity-array-null"); return; }
    for (unsigned j = 0; j < stage.activityCount; ++j) {
        uintptr_t iface = 0, again = 0;
        if (!get(read, context, stage.activityArray + 8 * j, iface)) {
            fail(out, "activity-pointer-read"); continue;
        }
        activity(read, context, base, iface, stage.activities[j], out);
        placedDetail(read, context, base, stage.activities[j], index, j, out);
        if (!get(read, context, stage.activityArray + 8 * j, again) || again != iface) {
            out.consistent = false; fail(out, "activity-pointer-changed");
        }
    }
    uintptr_t stackAgain = 0, predecessorAgain = 0, arrayAgain = 0, terminalAgain = 0;
    uint64_t countAgain = 0; uint32_t modeAgain = 0;
    if (!get(read, context, child + 0x30, stackAgain) ||
        !get(read, context, child + 0x40, predecessorAgain) ||
        !get(read, context, child + 0x58, countAgain) ||
        !get(read, context, child + 0x60, arrayAgain) ||
        !get(read, context, child + 0x68, modeAgain) ||
        !get(read, context, stage.stack + 0x78, terminalAgain) ||
        stackAgain != stage.stack || predecessorAgain != stage.predecessor ||
        countAgain != stage.activityCount || arrayAgain != stage.activityArray ||
        modeAgain != stage.mode || terminalAgain != terminal) {
        out.consistent = false; fail(out, "stage-fields-changed");
    }
    // Nested activities can source a whole other eleven-child stack. Include
    // that selected child, then the inherited predecessor edge below.
    for (unsigned j = 0; j < stage.activityCount; ++j) {
        const Activity& a = stage.activities[j];
        if (a.stateFn == base + 0x108A98C && a.stateKnown) {
            if (a.weightKnown && a.weight <= 0.0f) ++stage.inactiveNestedOmitted;
            else walk(read, context, base, a.state - 0x70, static_cast<int>(index), 'N', out);
        }
    }
    if (stage.predecessor == stage.stack + 0xB8 && stage.stackVtable == base + 0x5186290) return;
    if (stage.predecessor >= 0x70 && childGuard(read, context, base, stage.predecessor - 0x70))
        walk(read, context, base, stage.predecessor - 0x70, static_cast<int>(index), 'P', out);
    else if (stage.stackVtable == base + 0x5186938) {
        uintptr_t parentChild = 0, parentBase = 0;
        if (!get(read, context, stage.stack + 0x1B8, parentChild) ||
            !childGuard(read, context, base, parentChild) ||
            !get(read, context, parentChild + 0x40, parentBase) ||
            parentBase != stage.predecessor) fail(out, "nested-parent-base");
        else {
            stage.nestedParent = parentChild;
            uintptr_t parentStack = 0, parentStackVtable = 0;
            if (!get(read, context, parentChild + 0x30, parentStack) ||
                !get(read, context, parentStack, parentStackVtable) ||
                parentStackVtable != base + 0x5186290 || parentBase != parentStack + 0xB8)
                fail(out, "nested-parent-baseline-unknown");
        }
    } else fail(out, "predecessor-unresolved");
}

inline void capture(Read read, void* context, uintptr_t base, Snapshot& out) noexcept {
    if (!out.source || out.source < 0x70) { fail(out, "source-null"); return; }
    out.rootChild = out.source - 0x70;
    if (!childGuard(read, context, base, out.rootChild)) { fail(out, "source-child-guard"); return; }
    out.validSource = true;
    walk(read, context, base, out.rootChild, -1, 'R', out);
}

} } // namespace edvr::stationcam
