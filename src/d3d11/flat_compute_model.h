// Bounded, platform-independent policy for the manually armed compute probe.
// Classification grants capture only, never projection modification eligibility.
#pragma once
#include <cstdint>
#include <cstring>
#include <limits>

namespace edvr {

enum class FlatR32ProbeExtent { Unsupported, Native, ThreeQuarter };
inline FlatR32ProbeExtent flatR32ProbeExtent(uint32_t width, uint32_t height,
    uint32_t outputWidth, uint32_t outputHeight) {
    if (!width || !height || !outputWidth || !outputHeight ||
        width > 8192 || height > 8192 || outputWidth > 8192 || outputHeight > 8192)
        return FlatR32ProbeExtent::Unsupported;
    if (width == outputWidth && height == outputHeight) return FlatR32ProbeExtent::Native;
    return uint64_t(width)*4 == uint64_t(outputWidth)*3 &&
           uint64_t(height)*4 == uint64_t(outputHeight)*3 ?
        FlatR32ProbeExtent::ThreeQuarter : FlatR32ProbeExtent::Unsupported;
}

enum class FlatR32WriterRelation { Unrelated, BeforeConsumer, SpansConsumer };
inline FlatR32WriterRelation flatR32WriterRelation(const void* writerResource,
    uint32_t first, uint32_t last, const void* consumerResource, uint32_t consumerSequence) {
    if (!writerResource || writerResource != consumerResource || !first ||
        first >= consumerSequence || last < first) return FlatR32WriterRelation::Unrelated;
    return last < consumerSequence ? FlatR32WriterRelation::BeforeConsumer :
        FlatR32WriterRelation::SpansConsumer;
}
// Four graphics rows and CS fallbacks use the same exact byte-boundary rule.
inline bool flatComputeCopyRow(uint8_t (&out)[16], const void* source, uint32_t width, uint32_t row) {
    const uint64_t offset = uint64_t(row) * 16;
    if (!source || offset + 16 > width) return false;
    std::memcpy(out, static_cast<const uint8_t*>(source) + offset, 16);
    return true;
}
// Banks0..7 are the actual CS hashes,8/9 the PS-cluster/VS-flare roles.
inline bool flatComputeClaimCbFallback(uint16_t& used, uint32_t bank) {
    if (bank >= 10 || (used & (1u << bank))) return false;
    used = static_cast<uint16_t>(used | (1u << bank)); return true;
}

enum class FlatComputeRole { Unknown, BoundsT2, BoundsT1, Consumer };
inline constexpr uint64_t kFlatComputeHashes[] = {
    0x074CB657FDBD43E6ull, 0x593EA04AEB69A3CEull,
    0x76BFC737F1F8CB83ull, 0xF7CED6628283EAD1ull,
    0x46A041BDEB7440CBull, 0x5998146D464F5C0Eull,
    0x823CC578F5510B24ull, 0xEB0245DE0BB23BB6ull,
};
inline constexpr uint32_t kFlatComputeHashCount = 8;
inline constexpr int flatComputeHashIndex(uint64_t hash) {
    for (uint32_t i = 0; i < kFlatComputeHashCount; ++i)
        if (kFlatComputeHashes[i] == hash) return static_cast<int>(i);
    return -1;
}
inline constexpr FlatComputeRole flatComputeRole(uint64_t hash) {
    const int index = flatComputeHashIndex(hash);
    return index < 0 ? FlatComputeRole::Unknown : index == 0 ? FlatComputeRole::BoundsT2 :
        index == 1 ? FlatComputeRole::BoundsT1 : FlatComputeRole::Consumer;
}

enum class FlatComputeRefusal {
    None, InvalidStride, ZeroDimension, IndexOverflow, ViewRangeOverflow,
    GridOutsideView, ViewOutsideBuffer
};
inline const char* flatComputeRefusalName(FlatComputeRefusal reason) {
    switch (reason) {
    case FlatComputeRefusal::None: return "none";
    case FlatComputeRefusal::InvalidStride: return "stride-not-32";
    case FlatComputeRefusal::ZeroDimension: return "zero-grid-dimension";
    case FlatComputeRefusal::IndexOverflow: return "grid-index-overflow";
    case FlatComputeRefusal::ViewRangeOverflow: return "view-range-overflow";
    case FlatComputeRefusal::GridOutsideView: return "grid-outside-view";
    case FlatComputeRefusal::ViewOutsideBuffer: return "view-outside-buffer";
    }
    return "unknown-refusal";
}

// CSb0[1].xyz is UINT bits, not float dimensions. XYZ is linearized X-fastest.
// Output order: z=0, z=Nz/2, z=Nz-1; within each plane (0,0), (lastX,0),
// (0,lastY), (lastX,lastY). Degenerate planes/corners intentionally repeat.
// Offsets address the original buffer, including SRV FirstElement. Failure is atomic.
inline FlatComputeRefusal flatComputeCornerOffsets(const uint32_t (&counts)[3],
    uint64_t firstElement, uint64_t numElements, uint64_t bufferBytes,
    uint32_t stride, uint64_t (&byteOffsets)[12]) {
    if (stride != 32) return FlatComputeRefusal::InvalidStride;
    if (!counts[0] || !counts[1] || !counts[2]) return FlatComputeRefusal::ZeroDimension;
    const uint64_t max = (std::numeric_limits<uint64_t>::max)();
    const uint64_t plane = uint64_t(counts[0]) * counts[1];
    if (plane > max / counts[2]) return FlatComputeRefusal::IndexOverflow;
    const uint64_t volume = plane * counts[2];
    if (firstElement > max - numElements) return FlatComputeRefusal::ViewRangeOverflow;
    if (volume > numElements) return FlatComputeRefusal::GridOutsideView;
    // Division avoids overflow of the exclusive byte endpoint (first+count)*stride.
    if (firstElement + numElements > bufferBytes / stride)
        return FlatComputeRefusal::ViewOutsideBuffer;
    uint64_t next[12]{};
    const uint64_t z[] = {0, counts[2] / 2u, counts[2] - 1u};
    for (uint32_t slice = 0; slice < 3; ++slice)
        for (uint32_t corner = 0; corner < 4; ++corner) {
            const uint64_t x = (corner & 1) ? counts[0] - 1u : 0;
            const uint64_t y = (corner & 2) ? counts[1] - 1u : 0;
            next[slice * 4 + corner] = (firstElement + z[slice] * plane + y * counts[0] + x) * stride;
        }
    std::memcpy(byteOffsets, next, sizeof(next));
    return FlatComputeRefusal::None;
}

struct FlatComputeAttempts {
    bool armed = false, frameOpen = false;
    uint32_t attempts = 0;
    uint64_t armPresent = 0, lastBeginPresent = 0;
};
inline void flatComputeArm(FlatComputeAttempts& state, uint64_t present) {
    state = {};
    state.armed = true;
    state.armPresent = state.lastBeginPresent = present;
}
// The caller invokes this only after an owned successful Present. Startup cannot
// arm it; empty, rejected and accepted opened frames all consume an attempt.
inline bool flatComputeBeginAfterOwnedPresent(FlatComputeAttempts& state, uint64_t present) {
    if (!state.armed || state.frameOpen || state.attempts >= 2 ||
        present <= state.armPresent || present <= state.lastBeginPresent) return false;
    state.lastBeginPresent = present;
    state.frameOpen = true;
    return true;
}
inline uint32_t flatComputeAttempt(const FlatComputeAttempts& state) {
    return state.frameOpen && state.attempts < 2 ? state.attempts + 1 : 0;
}
inline bool flatComputeFinishAttempt(FlatComputeAttempts& state) {
    if (!state.frameOpen || state.attempts >= 2) return false;
    state.frameOpen = false;
    ++state.attempts;
    if (state.attempts == 2) state.armed = false;
    return true;
}
inline void flatComputeRetire(FlatComputeAttempts& state) {
    state.armed = state.frameOpen = false;
}
// Asynchronous readback polling has both frame and wall-clock budgets. Clock or
// counter regression retires it too; callers never wait for the GPU here.
inline bool flatComputeDeadline(uint64_t startPresent, uint64_t currentPresent,
                                 uint64_t startMs, uint64_t currentMs) {
    return currentPresent < startPresent || currentMs < startMs ||
        currentPresent - startPresent >= 120 || currentMs - startMs >= 2000;
}

} // namespace edvr
