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
    const bool screenFormat = format == 23 || format == 26 || format == 27;
    const bool screenExtent = outputWidth && outputHeight && width && height &&
        uint64_t(width) * outputHeight == uint64_t(height) * outputWidth &&
        uint64_t(width) * 2 >= outputWidth &&
        uint64_t(height) * 2 >= outputHeight &&
        width <= outputWidth && height <= outputHeight;
    return color && screenFormat && screenExtent
        ? kFlatContractScreen : kFlatContractNone;
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
};
struct FlatContractRecord {
    FlatContractObservation key{};  // key.camera is not retained
    unsigned char camera[kFlatCameraBytes] = {};
    uint32_t draws = 0, first = 0, last = 0;
    uint32_t firstCount = 0, firstInstances = 0;
    uint32_t lastCount = 0, lastInstances = 0;
    uint64_t firstWriteEpoch = 0, lastWriteEpoch = 0;
    uint32_t firstWriteSeq = 0, lastWriteSeq = 0;
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
    return !draw.camera ||
        std::memcmp(record.camera, draw.camera, kFlatCameraBytes) == 0;
}
template<size_t N>
FlatContractRecord* flatRecordContract(FlatContractRecord (&records)[N],
                                       uint32_t& used, uint32_t& dropped,
                                       const FlatContractObservation& draw) {
    if (draw.kind == kFlatContractNone) return nullptr;
    for (uint32_t i = 0; i < used; ++i) {
        FlatContractRecord& r = records[i];
        if (!flatContractMatches(r, draw)) continue;
        ++r.draws; r.last = draw.sequence;
        r.lastCount = draw.count; r.lastInstances = draw.instances;
        r.lastWriteEpoch = draw.writeEpoch; r.lastWriteSeq = draw.writeSeq;
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
