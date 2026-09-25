#pragma once
#include "../../src/d3d11/flat_compute_model.h"
#include <cstdio>

inline int flatComputeTests() {
    using namespace edvr;
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::printf("FAIL: flat compute %s\n", what); ++failures; }
    };
    uint8_t cb[5376]{}, row[16]{};
    cb[281*16]=0x31;cb[332*16]=0x72;
    check(flatComputeCopyRow(row,cb,4512,281)&&row[0]==0x31,"row281 exact4512-byte boundary");
    check(!flatComputeCopyRow(row,cb,4511,281)&&row[0]==0x31,"row281 short input preserves destination");
    check(flatComputeCopyRow(row,cb,5328,332)&&row[0]==0x72,"flare dimensions row332 exact5328-byte boundary");
    check(!flatComputeCopyRow(row,cb,5327,332)&&!flatComputeCopyRow(row,cb,5376,0xffffffffu)&&row[0]==0x72,"short and overflow rows refused atomically");
    uint16_t fallback=0;
    check(flatComputeClaimCbFallback(fallback,0)&&!flatComputeClaimCbFallback(fallback,0),"repeat bounds shader cannot consume another fallback");
    for(uint32_t bank=1;bank<10;++bank)check(flatComputeClaimCbFallback(fallback,bank),"lighting and graphics fallback banks survive repeated bounds admission");
    check(fallback==1023&&!flatComputeClaimCbFallback(fallback,10),"exact10 per-sample fallback bound");
    for (uint32_t i = 0; i < kFlatComputeHashCount; ++i) {
        check(flatComputeHashIndex(kFlatComputeHashes[i]) == static_cast<int>(i), "unique per-hash bank");
        check(flatComputeRole(kFlatComputeHashes[i]) == (i == 0 ? FlatComputeRole::BoundsT2 :
            i == 1 ? FlatComputeRole::BoundsT1 : FlatComputeRole::Consumer), "capture role classification");
    }
    check(flatComputeHashIndex(0) == -1 && flatComputeRole(0) == FlatComputeRole::Unknown &&
          flatComputeHashIndex(0x074CB657FDBD43E7ull) == -1, "unknown and near hashes refused");
    const void* r32 = reinterpret_cast<const void*>(uintptr_t(0x1234));
    const void* other = reinterpret_cast<const void*>(uintptr_t(0x5678));
    check(flatR32WriterRelation(r32,10,12,r32,13)==FlatR32WriterRelation::BeforeConsumer &&
          flatR32WriterRelation(r32,10,13,r32,13)==FlatR32WriterRelation::SpansConsumer,
          "R32 producer order separates definite writes from coalesced overlap");
    check(flatR32WriterRelation(r32,10,12,other,13)==FlatR32WriterRelation::Unrelated &&
          flatR32WriterRelation(r32,13,13,r32,13)==FlatR32WriterRelation::Unrelated &&
          flatR32WriterRelation(r32,12,10,r32,13)==FlatR32WriterRelation::Unrelated,
          "R32 provenance rejects other resources, late writes and invalid ranges");
    check(flatR32ProbeExtent(1280,720,1280,720)==FlatR32ProbeExtent::Native &&
          flatR32ProbeExtent(960,540,1280,720)==FlatR32ProbeExtent::ThreeQuarter &&
          flatR32ProbeExtent(1920,1080,2560,1440)==FlatR32ProbeExtent::ThreeQuarter,
          "R32 producer probe admits only measured native and 0.75x extents");
    check(flatR32ProbeExtent(960,539,1280,720)==FlatR32ProbeExtent::Unsupported &&
          flatR32ProbeExtent(959,539,1279,719)==FlatR32ProbeExtent::Unsupported &&
          flatR32ProbeExtent(1024,1024,1280,720)==FlatR32ProbeExtent::Unsupported &&
          flatR32ProbeExtent(0,540,1280,720)==FlatR32ProbeExtent::Unsupported &&
          flatR32ProbeExtent(960,540,0xffffffffu,0xffffffffu)==FlatR32ProbeExtent::Unsupported,
          "R32 producer probe refuses off-by-one, odd unsupported, unrelated and invalid extents");
    const uint32_t counts[3] = {3, 5, 4}; // Raw UINT bits, never float(3)/float(5)/float(4).
    uint64_t offsets[12]{};
    const uint64_t expectedIndices[12] = {0, 2, 12, 14, 30, 32, 42, 44, 45, 47, 57, 59};
    check(flatComputeCornerOffsets(counts, 7, 60, 67*32, 32, offsets) == FlatComputeRefusal::None,
          "exact SRV and buffer boundary admitted");
    for (uint32_t i = 0; i < 12; ++i)
        check(offsets[i] == (expectedIndices[i]+7)*32, "XYZ corners include view first element");
    const uint32_t odd[3] = {2, 2, 5};
    check(flatComputeCornerOffsets(odd, 0, 20, 640, 32, offsets) == FlatComputeRefusal::None &&
          offsets[4] == 8*32 && offsets[8] == 16*32, "odd depth uses integer midpoint");
    const uint32_t one[3] = {1, 1, 1};
    check(flatComputeCornerOffsets(one, 9, 1, 320, 32, offsets) == FlatComputeRefusal::None,
          "degenerate dimensions valid");
    for (uint64_t value : offsets) check(value == 9*32, "duplicate corners preserved");
    const uint32_t wide[3] = {0xffffffffu, 1, 1};
    check(flatComputeCornerOffsets(wide, 0, 0xffffffffull, 0xffffffffull*32, 32, offsets) == FlatComputeRefusal::None &&
          offsets[1] == 0xfffffffeull*32, "64-bit byte offsets without 32-bit truncation");
    uint64_t sentinel[12]; for (auto& value : sentinel) value = 0xbadc0ffeeull;
    auto refused = [&](const uint32_t (&grid)[3], uint64_t first, uint64_t count,
                       uint64_t bytes, uint32_t stride, FlatComputeRefusal reason) {
        std::memcpy(offsets, sentinel, sizeof(offsets));
        check(flatComputeCornerOffsets(grid, first, count, bytes, stride, offsets) == reason &&
              !std::memcmp(offsets, sentinel, sizeof(offsets)), "explicit refusal preserves all offsets");
        check(std::strcmp(flatComputeRefusalName(reason), "unknown-refusal") != 0, "refusal has diagnostic name");
    };
    const uint64_t max = (std::numeric_limits<uint64_t>::max)();
    const uint32_t zero[3] = {2, 0, 4}, enormous[3] = {0xffffffffu, 0xffffffffu, 0xffffffffu};
    const uint32_t floatBits[3] = {0x40000000u, 0x40000000u, 0x40000000u};
    refused(counts, 0, 60, 1920, 16, FlatComputeRefusal::InvalidStride);
    refused(zero, 0, 60, 1920, 32, FlatComputeRefusal::ZeroDimension);
    refused(enormous, 0, max, max, 32, FlatComputeRefusal::IndexOverflow);
    refused(floatBits, 0, max, max, 32, FlatComputeRefusal::IndexOverflow);
    refused(one, max, 1, max, 32, FlatComputeRefusal::ViewRangeOverflow);
    refused(counts, 7, 59, 67*32, 32, FlatComputeRefusal::GridOutsideView);
    refused(counts, 7, 60, 67*32-1, 32, FlatComputeRefusal::ViewOutsideBuffer);
    refused(one, max/32, 1, max, 32, FlatComputeRefusal::ViewOutsideBuffer);
    refused(one, 0, 0, 0, 32, FlatComputeRefusal::GridOutsideView);

    FlatComputeAttempts state{};
    check(!flatComputeBeginAfterOwnedPresent(state, 100) && !flatComputeFinishAttempt(state), "startup and unopened finish inert");
    flatComputeArm(state, 100);
    check(!flatComputeBeginAfterOwnedPresent(state, 100) && !flatComputeBeginAfterOwnedPresent(state, 99), "same arm frame and backwards Present refused");
    check(flatComputeBeginAfterOwnedPresent(state, 101) && flatComputeAttempt(state) == 1 &&
          !flatComputeBeginAfterOwnedPresent(state, 102), "first attempt stable until frame finished");
    check(flatComputeFinishAttempt(state) && state.attempts == 1 && !flatComputeFinishAttempt(state) &&
          !flatComputeBeginAfterOwnedPresent(state, 101), "empty or refused first frame spends exactly one attempt");
    check(flatComputeBeginAfterOwnedPresent(state, 102) && flatComputeAttempt(state) == 2 &&
          flatComputeFinishAttempt(state) && state.attempts == 2 && !state.armed, "second attempt saturates budget");
    for (uint64_t present = 103; present < 140; ++present)
        check(!flatComputeBeginAfterOwnedPresent(state, present) && !flatComputeFinishAttempt(state) && state.attempts == 2,
              "exhausted probe cannot retry");
    flatComputeArm(state, 200);
    check(flatComputeBeginAfterOwnedPresent(state, 201) && flatComputeAttempt(state) == 1, "manual rearm resets attempt budget");
    flatComputeRetire(state);
    check(!state.frameOpen && !state.armed && flatComputeAttempt(state) == 0 &&
          !flatComputeBeginAfterOwnedPresent(state, 202), "retirement cancels pending frame");
    check(!flatComputeDeadline(100, 219, 1000, 2999) && flatComputeDeadline(100, 220, 1000, 2999) &&
          flatComputeDeadline(100, 219, 1000, 3000), "exact independent Present/time deadlines");
    check(flatComputeDeadline(100, 99, 1000, 1001) && flatComputeDeadline(100, 101, 1000, 999),
          "backwards clocks and counters expire");
    check(!flatComputeDeadline(max-119, max, max-1999, max), "deadline subtraction safe near counter maximum");
    return failures;
}
