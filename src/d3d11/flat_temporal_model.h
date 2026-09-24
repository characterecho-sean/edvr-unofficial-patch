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
