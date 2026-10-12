// The frame contract: the flat temporal reducer's immutable per-frame
// artifact (docs/design-flat-temporal-aa-2026-09-23.md's staged program,
// gate 1, and docs/review-flat-temporal-aa-2026-09-26.md). One streaming
// reducer -- flatRuntimeObserve over FlatRuntimeDraw events -- produces it
// across the frame's output-copy draws; the online driver consumes the
// first copy's selection, the diagnostic replay consumes and hashes the
// whole artifact. Neither the artifact nor the hash changes any decision:
// gate 1 is consolidation only.
//
// Hash schema v3 (the review's G1-1/G1-2): every copy draw's outcome is
// recorded, including the early refusals that assemble no fixture records;
// the hash covers the full semantic selection (camera rows, extents,
// instance counts), not a subset. The stored corpus hashes were regenerated
// from the same saved events by the rig's migration pass -- no new flight.
#pragma once
#include "flat_runtime_model.h"

namespace edvr {

// The online assembly's bound (flat_runtime_model.h's copy branch builds at
// most 36 fixture records: HDR aggregates, camera, sources, tone, copy).
constexpr uint32_t kFlatFrameContractRecordCapacity = 36;

struct FlatFrameContract {
    uint64_t frame = 0, epoch = 0;
    const void* output = nullptr;
    uint32_t outputWidth = 0, outputHeight = 0, outputFormat = 0;
    // The fixture records from the first copy draw that assembled any; later
    // copies never overwrite them. key.camera aliases are rebased to each
    // record's own camera bytes, so the artifact is self-contained.
    FlatContractRecord records[kFlatFrameContractRecordCapacity]{};
    uint32_t recordCount = 0;
    // Every output-copy draw's selection outcome this frame, in order; the
    // first is the one the driver consumed. Early refusals (no fixture
    // assembled) are outcomes too. Bounded; past the bound the reducer has
    // already refused the frame as truncated, and copiesOverflowed records it.
    static constexpr uint32_t kMaxCopies = 4;
    uint32_t copiesUsed = 0;
    bool copiesOverflowed = false;
    FlatMonoFrame copies[kMaxCopies]{};
    FlatRuntimeWitness copyConflicts[kMaxCopies]{};
    bool produced = false;  // at least one copy draw ran this frame
};

// Identity over exactly the fields that decide treatment. Pointer/token
// identities compare as values -- a trace replays within its own frame's
// namespace. Field-wise only: struct padding is not deterministic.
inline uint64_t flatFrameContractHash(const FlatFrameContract& c) {
    uint64_t hash = 14695981039346656037ull;
    auto mix = [&hash](const void* data, size_t bytes) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < bytes; ++i) hash = (hash ^ p[i]) * 1099511628211ull;
    };
    mix(&c.frame, sizeof(c.frame)); mix(&c.epoch, sizeof(c.epoch));
    mix(&c.output, sizeof(c.output));
    mix(&c.outputWidth, sizeof(c.outputWidth)); mix(&c.outputHeight, sizeof(c.outputHeight));
    mix(&c.outputFormat, sizeof(c.outputFormat));
    mix(&c.recordCount, sizeof(c.recordCount));
    for (uint32_t i = 0; i < c.recordCount; ++i) {
        const auto& r = c.records[i];
        const auto& k = r.key;
        mix(&k.color, sizeof(k.color)); mix(&k.depth, sizeof(k.depth));
        mix(&k.rtv, sizeof(k.rtv)); mix(&k.dsv, sizeof(k.dsv)); mix(&k.b1, sizeof(k.b1));
        mix(k.srvView, sizeof(k.srvView)); mix(k.srvResource, sizeof(k.srvResource));
        mix(&k.cameraHash, sizeof(k.cameraHash));
        mix(&k.vs, sizeof(k.vs)); mix(&k.ps, sizeof(k.ps));
        mix(&k.writeEpoch, sizeof(k.writeEpoch)); mix(&k.writeSeq, sizeof(k.writeSeq));
        mix(&k.width, sizeof(k.width)); mix(&k.height, sizeof(k.height));
        mix(&k.format, sizeof(k.format));
        mix(&k.depthWidth, sizeof(k.depthWidth)); mix(&k.depthHeight, sizeof(k.depthHeight));
        mix(&k.depthFormat, sizeof(k.depthFormat));
        mix(k.viewport, sizeof(k.viewport)); mix(&k.viewportCount, sizeof(k.viewportCount));
        mix(&k.sequence, sizeof(k.sequence)); mix(&k.count, sizeof(k.count));
        mix(&k.instances, sizeof(k.instances)); mix(&k.kind, sizeof(k.kind));
        for (uint32_t slot = 0; slot < kFlatProjectionSlots; ++slot) {
            const auto& pr = k.projection[slot];
            mix(&pr.resource, sizeof(pr.resource)); mix(&pr.width, sizeof(pr.width));
            mix(&pr.copied, sizeof(pr.copied)); mix(&pr.hash, sizeof(pr.hash));
            mix(&pr.writeSeq, sizeof(pr.writeSeq)); mix(&pr.writeEpoch, sizeof(pr.writeEpoch));
            mix(&pr.status, sizeof(pr.status));
        }
        mix(r.camera, sizeof(r.camera));
        mix(&r.draws, sizeof(r.draws)); mix(&r.first, sizeof(r.first)); mix(&r.last, sizeof(r.last));
        mix(&r.firstInstances, sizeof(r.firstInstances));
        mix(&r.lastInstances, sizeof(r.lastInstances));
        mix(&r.firstCount, sizeof(r.firstCount)); mix(&r.lastCount, sizeof(r.lastCount));
        mix(&r.firstWriteEpoch, sizeof(r.firstWriteEpoch));
        mix(&r.lastWriteEpoch, sizeof(r.lastWriteEpoch));
        mix(&r.firstWriteSeq, sizeof(r.firstWriteSeq)); mix(&r.lastWriteSeq, sizeof(r.lastWriteSeq));
        // Preserve every legacy corpus hash; the new typed witness contributes
        // its separate donor identity only when it actually exists.
        if(r.deferredCamera) {
            if(r.deferredInverseRepaired)mix(&r.deferredInverseRepaired,sizeof(r.deferredInverseRepaired));
            mix(&r.deferredCamera,sizeof(r.deferredCamera));mix(&r.sceneCameraB1,sizeof(r.sceneCameraB1));
            mix(&r.sceneCameraWriteEpoch,sizeof(r.sceneCameraWriteEpoch));mix(&r.sceneCameraWriteSeq,sizeof(r.sceneCameraWriteSeq));
        }
    }
    mix(&c.copiesUsed, sizeof(c.copiesUsed));
    mix(&c.copiesOverflowed, sizeof(c.copiesOverflowed));
    mix(&c.produced, sizeof(c.produced));
    for (uint32_t i = 0; i < c.copiesUsed && i < FlatFrameContract::kMaxCopies; ++i) {
        const auto& sel = c.copies[i];
        mix(&sel.reason, sizeof(sel.reason));
        mix(&sel.frame, sizeof(sel.frame)); mix(&sel.epoch, sizeof(sel.epoch));
        mix(&sel.color, sizeof(sel.color)); mix(&sel.hdr, sizeof(sel.hdr));
        mix(&sel.depth, sizeof(sel.depth)); mix(&sel.dsv, sizeof(sel.dsv));
        mix(&sel.sceneConstants, sizeof(sel.sceneConstants));
        mix(&sel.output, sizeof(sel.output));
        mix(&sel.renderWidth, sizeof(sel.renderWidth)); mix(&sel.renderHeight, sizeof(sel.renderHeight));
        mix(&sel.outputWidth, sizeof(sel.outputWidth)); mix(&sel.outputHeight, sizeof(sel.outputHeight));
        mix(&sel.depthFormat, sizeof(sel.depthFormat));
        mix(sel.camera, sizeof(sel.camera));
        mix(&sel.cameraHash, sizeof(sel.cameraHash)); mix(&sel.nearPlane, sizeof(sel.nearPlane));
        mix(&sel.sourceFirst, sizeof(sel.sourceFirst)); mix(&sel.sourceLast, sizeof(sel.sourceLast));
        mix(&sel.hdrFirst, sizeof(sel.hdrFirst)); mix(&sel.hdrLast, sizeof(sel.hdrLast));
        mix(&sel.toneSequence, sizeof(sel.toneSequence)); mix(&sel.copySequence, sizeof(sel.copySequence));
        mix(&sel.firstLaterOutput, sizeof(sel.firstLaterOutput));
        mix(&sel.supportedDraws, sizeof(sel.supportedDraws));
        mix(&sel.unsupportedDraws, sizeof(sel.unsupportedDraws));
        mix(&c.copyConflicts[i].cause, sizeof(c.copyConflicts[i].cause));
    }
    return hash;
}

// The reducer's contract-producing entry point, called at each output-copy
// draw. Returns the same FlatMonoFrame the two-argument flatRuntimeObserve
// would return for the same prefix and draw -- consolidation, not a decision
// change. The wrapper records every copy's outcome, including the early
// refusals that assemble no fixture records; the first record-assembling
// copy's fixture is the frame's, and its key.camera aliases are rebased to
// the artifact's own bytes.
inline FlatMonoFrame flatRuntimeObserveContract(FlatRuntimePrefix& p, const FlatRuntimeDraw& d,
                                                FlatFrameContract& contract) {
    FlatRuntimeContractSink sink{};
    sink.records = contract.recordCount ? nullptr : contract.records;
    sink.capacity = contract.recordCount ? 0 : kFlatFrameContractRecordCapacity;
    const FlatMonoFrame selection = flatRuntimeObserve(p, d, &sink);
    if (!contract.copiesUsed) {
        contract.frame = contract.epoch = p.frame;
        contract.output = p.output;
        contract.outputWidth = p.width; contract.outputHeight = p.height;
        contract.outputFormat = p.format;
    }
    contract.produced = true;
    if (sink.count) {
        contract.recordCount = sink.count;
        for (uint32_t i = 0; i < sink.count; ++i)
            if (contract.records[i].key.camera)
                contract.records[i].key.camera = contract.records[i].camera;
    }
    const uint32_t slot = contract.copiesUsed;
    if (slot < FlatFrameContract::kMaxCopies) {
        contract.copies[slot] = selection;
        contract.copyConflicts[slot] = p.selectedConflict;
    } else {
        contract.copiesOverflowed = true;
    }
    ++contract.copiesUsed;
    return selection;
}
} // namespace edvr
