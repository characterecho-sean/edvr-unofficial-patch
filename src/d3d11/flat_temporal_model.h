// Small, platform-independent parts of the flat discovery admission contract.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace edvr {

template<class T, size_t N, class Match>
T* flatFindOrAdd(T (&items)[N], uint32_t& used, uint32_t& overflow,
                 Match&& matches) {
    for (uint32_t i = 0; i < used; ++i)
        if (matches(items[i])) return &items[i];
    if (used == N) { ++overflow; return nullptr; }
    T* item = &items[used++];
    *item = T{};
    return item;
}

inline bool flatCaptureExpired(uint64_t startMs, uint64_t nowMs,
                               uint32_t presents, uint64_t maxMs,
                               uint32_t maxPresents) {
    return nowMs - startMs >= maxMs || presents >= maxPresents;
}

inline bool flatCaptureThreadEligible(bool active, uint32_t owner,
                                      uint32_t caller) {
    return active && owner != 0 && caller == owner;
}

inline bool flatSceneCandidateEligible(const void* color, const void* depth,
                                       uint32_t depthDraws) {
    return color && depth && depthDraws != 0;
}

// An exemplar belongs to one target and one VS CB slot in one frame. Capture
// it at the draw, so later writes to the same buffer cannot change evidence.
constexpr uint32_t kFlatCbExemplarBytes = 4096;
struct FlatCbExemplar {
    const void* resource = nullptr;
    uint64_t shader = 0, writeEpoch = 0, drawEpoch = 0;
    uint32_t width = 0, copied = 0, writeSeq = 0, drawSeq = 0;
    unsigned char bytes[kFlatCbExemplarBytes] = {};
};
inline bool flatFreezeCbExemplar(FlatCbExemplar& out, const void* resource,
                                 const unsigned char* bytes, uint32_t width,
                                 uint32_t copied, uint64_t writeEpoch,
                                 uint32_t writeSeq, uint64_t drawEpoch,
                                 uint32_t drawSeq, uint64_t shader) {
    if (out.copied || !resource || !bytes || !copied ||
        copied > kFlatCbExemplarBytes ||
        writeEpoch != drawEpoch || writeSeq > drawSeq) return false;
    out.resource = resource; out.width = width; out.copied = copied;
    out.writeEpoch = writeEpoch; out.writeSeq = writeSeq;
    out.drawEpoch = drawEpoch; out.drawSeq = drawSeq; out.shader = shader;
    std::memcpy(out.bytes, bytes, copied);
    return true;
}

// Engine scene constants rows 270..275 are outside the 4 KiB general CB
// exemplar. This 96-byte slice is copied from a complete observed CPU write.
constexpr uint32_t kFlatCameraOffset = 270u * 16u;
constexpr uint32_t kFlatCameraBytes = 6u * 16u;
inline bool flatCaptureCameraRows(unsigned char (&out)[kFlatCameraBytes],
                                  const void* source, uint32_t width) {
    if (!source || width < kFlatCameraOffset + kFlatCameraBytes) return false;
    std::memcpy(out, static_cast<const unsigned char*>(source) + kFlatCameraOffset,
                kFlatCameraBytes);
    return true;
}
inline uint64_t flatCameraHash(const unsigned char* bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t i = 0; i < kFlatCameraBytes; ++i)
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}
enum FlatCameraAvailability : uint8_t {
    kFlatCameraMissingBuffer = 0, kFlatCameraInvalidWrite = 1,
    kFlatCameraOldFrame = 2, kFlatCameraLaterWrite = 3,
    kFlatCameraAvailable = 4, kFlatCameraAvailabilityCount = 5
};
inline FlatCameraAvailability flatCameraAvailability(bool bufferObserved,
        bool validRows, uint64_t writeEpoch, uint32_t writeSeq,
        uint64_t drawEpoch, uint32_t drawSeq) {
    if (!bufferObserved) return kFlatCameraMissingBuffer;
    if (!validRows) return kFlatCameraInvalidWrite;
    if (writeEpoch != drawEpoch) return kFlatCameraOldFrame;
    if (writeSeq > drawSeq) return kFlatCameraLaterWrite;
    return kFlatCameraAvailable;
}

enum FlatContractKind : uint8_t {
    kFlatContractNone = 0, kFlatContractPool = 1,
    kFlatContractScreen = 2, kFlatContractOutput = 3
};
inline FlatContractKind flatContractKind(bool knownPoolFamily,
                                         const void* color, const void* depth,
                                         uint32_t width, uint32_t height,
                                         uint32_t format, uint32_t outputWidth,
                                         uint32_t outputHeight, bool isOutput) {
    if (isOutput && color) return kFlatContractOutput;
    if (knownPoolFamily && color && depth) return kFlatContractPool;
    const bool screenFormat = format == 23 || format == 26 || format == 27 || format == 60;
    const bool screenExtent = outputWidth && outputHeight && width && height &&
        uint64_t(width) * outputHeight == uint64_t(height) * outputWidth &&
        uint64_t(width) * 2 >= outputWidth &&
        uint64_t(height) * 2 >= outputHeight &&
        width <= outputWidth && height <= outputHeight;
    return color && screenFormat && screenExtent
        ? kFlatContractScreen : kFlatContractNone;
}

constexpr uint32_t kFlatProjectionSlots = 3;  // VS b0, VS b2, PS b2
enum class FlatDetailAdmission : uint8_t { Startup, Empty, Exhausted, RefusalAlreadyReported, Selected, Refusal };
inline FlatDetailAdmission flatTakeDetailSample(bool manual, uint64_t frame, uint32_t draws,
        bool selected, uint32_t& remaining, bool& refusalReported) {
    if (!manual) return FlatDetailAdmission::Startup;
    if (!frame || !draws) return FlatDetailAdmission::Empty;
    if (!remaining) return FlatDetailAdmission::Exhausted;
    if (!selected && refusalReported) return FlatDetailAdmission::RefusalAlreadyReported;
    --remaining;
    if (!selected) refusalReported = true;
    return selected ? FlatDetailAdmission::Selected : FlatDetailAdmission::Refusal;
}
inline const char* flatDetailAdmissionName(FlatDetailAdmission admission) {
    switch (admission) {
    case FlatDetailAdmission::Startup: return "startup-summary-only";
    case FlatDetailAdmission::Empty: return "empty-or-rearm-budget-unspent";
    case FlatDetailAdmission::Exhausted: return "two-detail-budget-exhausted";
    case FlatDetailAdmission::RefusalAlreadyReported: return "refusal-already-detailed-budget-unspent";
    case FlatDetailAdmission::Selected: return "selected-frame-details";
    case FlatDetailAdmission::Refusal: return "first-refusal-details";
    }
    return "unknown";
}
constexpr uint32_t kFlatB0Bytes = 4u * 16u;
constexpr uint32_t kFlatB2Bytes = 17u * 16u;
inline uint32_t flatProjectionOffset(uint32_t slot) { return slot ? 0u : 4u * 16u; }
inline uint32_t flatProjectionCapacity(uint32_t slot) { return slot ? kFlatB2Bytes : kFlatB0Bytes; }
inline uint32_t flatProjectionHash(const unsigned char* data, uint32_t bytes) {
    if (!bytes) return 0;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < bytes; ++i) hash = (hash ^ data[i]) * 16777619u;
    return hash;
}
enum class FlatProjectionStatus : uint8_t {
    BindingUnknown, Unbound, MissingWrite, InvalidWrite, OldFrame, LaterWrite, ShortRange, Available
};
inline const char* flatProjectionStatusName(FlatProjectionStatus status) {
    switch (status) {
    case FlatProjectionStatus::BindingUnknown: return "binding-unobserved";
    case FlatProjectionStatus::Unbound: return "explicitly-unbound";
    case FlatProjectionStatus::MissingWrite: return "cpu-write-unobserved";
    case FlatProjectionStatus::InvalidWrite: return "write-invalidated";
    case FlatProjectionStatus::OldFrame: return "prior-frame-write";
    case FlatProjectionStatus::LaterWrite: return "write-after-draw";
    case FlatProjectionStatus::ShortRange: return "buffer-shorter-than-requested-rows";
    case FlatProjectionStatus::Available: return "same-frame-draw-frozen";
    }
    return "unknown";
}
struct FlatProjectionBinding { const void* resource = nullptr; bool observed = false; };
inline bool flatProjectionBind(FlatProjectionBinding& binding, uint32_t slot,
        uint32_t start, uint32_t count, const void* resource) {
    if (start > slot || slot - start >= count) return false;
    binding.resource = resource; binding.observed = true;
    return true;
}
struct FlatProjectionObservation {
    const void* resource = nullptr;
    const unsigned char* bytes = nullptr;
    uint32_t width = 0, copied = 0, hash = 0, writeSeq = 0;
    uint64_t writeEpoch = 0;
    FlatProjectionStatus status = FlatProjectionStatus::BindingUnknown;
};
inline FlatProjectionObservation flatObserveProjection(const FlatProjectionBinding& binding,
        bool tracked, uint32_t width, const unsigned char* prefix, uint32_t prefixBytes,
        uint64_t writeEpoch, uint32_t writeSeq, uint64_t drawEpoch, uint32_t drawSeq,
        uint32_t slot, uint32_t hash) {
    FlatProjectionObservation out{};
    out.resource = binding.resource;
    if (!binding.observed) return out;
    if (!binding.resource) { out.status = FlatProjectionStatus::Unbound; return out; }
    if (!tracked) { out.status = FlatProjectionStatus::MissingWrite; return out; }
    out.width = width; out.writeEpoch = writeEpoch; out.writeSeq = writeSeq;
    if (!prefix || !prefixBytes || prefixBytes > width) {
        out.status = FlatProjectionStatus::InvalidWrite; return out;
    }
    if (writeEpoch != drawEpoch) { out.status = FlatProjectionStatus::OldFrame; return out; }
    if (writeSeq > drawSeq) { out.status = FlatProjectionStatus::LaterWrite; return out; }
    const uint32_t offset = flatProjectionOffset(slot), capacity = flatProjectionCapacity(slot);
    out.copied = prefixBytes > offset ? prefixBytes - offset : 0;
    if (out.copied > capacity) out.copied = capacity;
    out.bytes = out.copied ? prefix + offset : nullptr;
    out.hash = out.copied ? hash : 0;
    out.status = out.copied == capacity ? FlatProjectionStatus::Available : FlatProjectionStatus::ShortRange;
    return out;
}

struct FlatContractObservation {
    const void* color = nullptr, *depth = nullptr, *rtv = nullptr, *dsv = nullptr;
    const void* b1 = nullptr;
    const void* srvView[4] = {}, *srvResource[4] = {};
    const unsigned char* camera = nullptr;  // valid only during this draw
    uint64_t cameraHash = 0, vs = 0, ps = 0, writeEpoch = 0;
    uint32_t width = 0, height = 0, format = 0;
    uint32_t depthWidth = 0, depthHeight = 0, depthFormat = 0;
    float viewport[6] = {};  // x, y, width, height, min depth, max depth
    uint32_t viewportCount = 0;
    uint32_t sequence = 0, count = 0, instances = 0, writeSeq = 0;
    FlatContractKind kind = kFlatContractNone;
    FlatProjectionObservation projection[kFlatProjectionSlots] = {};
};
struct FlatContractRecord {
    FlatContractObservation key{};  // key.camera is not retained
    unsigned char camera[kFlatCameraBytes] = {};
    uint32_t draws = 0, first = 0, last = 0;
    uint32_t firstCount = 0, firstInstances = 0;
    uint32_t lastCount = 0, lastInstances = 0;
    uint64_t firstWriteEpoch = 0, lastWriteEpoch = 0;
    uint32_t firstWriteSeq = 0, lastWriteSeq = 0;
    unsigned char b0[kFlatB0Bytes] = {}, vsB2[kFlatB2Bytes] = {}, psB2[kFlatB2Bytes] = {};
    uint64_t firstProjectionEpoch[kFlatProjectionSlots] = {}, lastProjectionEpoch[kFlatProjectionSlots] = {};
    uint32_t firstProjectionSeq[kFlatProjectionSlots] = {}, lastProjectionSeq[kFlatProjectionSlots] = {};
    unsigned char* projectionBytes(uint32_t slot) { return slot == 0 ? b0 : slot == 1 ? vsB2 : psB2; }
    const unsigned char* projectionBytes(uint32_t slot) const { return slot == 0 ? b0 : slot == 1 ? vsB2 : psB2; }
};
inline bool flatContractMatches(const FlatContractRecord& record,
                                const FlatContractObservation& draw) {
    const auto& k = record.key;
    if (k.kind != draw.kind || k.color != draw.color || k.depth != draw.depth ||
        k.rtv != draw.rtv || k.dsv != draw.dsv || k.b1 != draw.b1 ||
        k.vs != draw.vs || k.ps != draw.ps ||
        k.width != draw.width || k.height != draw.height || k.format != draw.format ||
        k.depthWidth != draw.depthWidth || k.depthHeight != draw.depthHeight ||
        k.depthFormat != draw.depthFormat ||
        k.viewportCount != draw.viewportCount ||
        std::memcmp(k.viewport, draw.viewport, sizeof(k.viewport)) != 0 ||
        bool(k.camera) != bool(draw.camera) ||
        k.cameraHash != draw.cameraHash) return false;
    for (uint32_t i = 0; i < 4; ++i)
        if (k.srvView[i] != draw.srvView[i] ||
            k.srvResource[i] != draw.srvResource[i]) return false;
    for (uint32_t i = 0; i < kFlatProjectionSlots; ++i) {
        const auto& a = k.projection[i]; const auto& b = draw.projection[i];
        if (a.resource != b.resource || a.status != b.status || a.width != b.width ||
            a.copied != b.copied || a.hash != b.hash ||
            (b.copied && std::memcmp(record.projectionBytes(i), b.bytes, b.copied) != 0)) return false;
    }
    return !draw.camera ||
        std::memcmp(record.camera, draw.camera, kFlatCameraBytes) == 0;
}
template<size_t N>
FlatContractRecord* flatRecordContract(FlatContractRecord (&records)[N],
                                       uint32_t& used, uint32_t& dropped,
                                       const FlatContractObservation& draw) {
    if (draw.kind == kFlatContractNone) return nullptr;
    for (uint32_t i = 0; i < kFlatProjectionSlots; ++i)
        if (draw.projection[i].copied > flatProjectionCapacity(i) ||
            (draw.projection[i].copied && !draw.projection[i].bytes)) { ++dropped; return nullptr; }
    for (uint32_t i = 0; i < used; ++i) {
        FlatContractRecord& r = records[i];
        if (!flatContractMatches(r, draw)) continue;
        ++r.draws; r.last = draw.sequence;
        r.lastCount = draw.count; r.lastInstances = draw.instances;
        r.lastWriteEpoch = draw.writeEpoch; r.lastWriteSeq = draw.writeSeq;
        for (uint32_t j = 0; j < kFlatProjectionSlots; ++j) {
            r.lastProjectionEpoch[j] = draw.projection[j].writeEpoch;
            r.lastProjectionSeq[j] = draw.projection[j].writeSeq;
        }
        return &r;
    }
    if (used == N) { ++dropped; return nullptr; }
    FlatContractRecord& r = records[used++];
    r = FlatContractRecord{};
    r.key = draw;
    // Do not retain a transient pointer into a CB shadow. Only the admitted
    // record owns camera bytes; equality uses hash and then all 96 bytes.
    r.key.camera = draw.camera ? r.camera : nullptr;
    if (draw.camera) std::memcpy(r.camera, draw.camera, kFlatCameraBytes);
    r.draws = 1; r.first = r.last = draw.sequence;
    r.firstCount = r.lastCount = draw.count;
    r.firstInstances = r.lastInstances = draw.instances;
    r.firstWriteEpoch = r.lastWriteEpoch = draw.writeEpoch;
    r.firstWriteSeq = r.lastWriteSeq = draw.writeSeq;
    for (uint32_t i = 0; i < kFlatProjectionSlots; ++i) {
        const auto& p = draw.projection[i];
        r.key.projection[i].bytes = p.copied ? r.projectionBytes(i) : nullptr;
        if (p.copied) std::memcpy(r.projectionBytes(i), p.bytes, p.copied);
        r.firstProjectionEpoch[i] = r.lastProjectionEpoch[i] = p.writeEpoch;
        r.firstProjectionSeq[i] = r.lastProjectionSeq[i] = p.writeSeq;
    }
    return &r;
}

// Late format-27 and output draws must remain observable when scene/material
// diversity saturates the world bank. Both banks have fixed independent caps.
template<size_t N, size_t M>
FlatContractRecord* flatRecordContractReserved(
        FlatContractRecord (&world)[N], uint32_t& worldUsed, uint32_t& worldDropped,
        FlatContractRecord (&handoff)[M], uint32_t& handoffUsed, uint32_t& handoffDropped,
        const FlatContractObservation& draw) {
    const bool late = draw.kind == kFlatContractOutput ||
        (draw.kind == kFlatContractScreen && draw.format == 27);
    return late ? flatRecordContract(handoff, handoffUsed, handoffDropped, draw) :
                  flatRecordContract(world, worldUsed, worldDropped, draw);
}

// Keep direct output routes even when the general edge inventory fills.
template<class Edge, size_t N, size_t M>
void flatRecordEdge(Edge (&all)[N], uint32_t& allUsed, uint32_t& allOverflow,
                    Edge (&outputEdges)[M], uint32_t& outputUsed,
                    uint32_t& outputOverflow, const void* src,
                    const void* dst, char kind, const void* output,
                    uint32_t sequence) {
    if (!src || !dst) return;
    auto record = [&](auto& entries, uint32_t& used, uint32_t& overflow) {
        Edge* e = flatFindOrAdd(entries, used, overflow,
            [=](const Edge& x) { return x.src == src && x.dst == dst && x.kind == kind; });
        if (!e) return;
        if (!e->count) {
            e->src = src; e->dst = dst; e->kind = kind;
            e->first = sequence;
        }
        ++e->count; e->last = sequence;
    };
    record(all, allUsed, allOverflow);
    if (output && dst == output)
        record(outputEdges, outputUsed, outputOverflow);
}

// A discovered shape is never a certificate. These flags require independent
// desktop evidence; this discovery build deliberately sets none of them.
struct FlatTemporalProof {
    bool sceneAndCamera = false;
    bool consumedProjection = false;
    bool matchedDepthAndMotion = false;
    bool completionAndUiOrder = false;
    bool outputAndModOrder = false;
    bool jitterRollback = false;
    bool unknownDeferredWork = false;
    bool duplicateTreatment = false;
};
inline bool flatTemporalEvidenceComplete(const FlatTemporalProof& p) {
    return p.sceneAndCamera && p.consumedProjection &&
           p.matchedDepthAndMotion && p.completionAndUiOrder &&
           p.outputAndModOrder && p.jitterRollback &&
           !p.unknownDeferredWork && !p.duplicateTreatment;
}
}  // namespace edvr
