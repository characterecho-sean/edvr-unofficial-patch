#pragma once
#include "../../src/d3d11/flat_shader_capture.h"
#include <atomic>
#include <cstdio>

inline int flatShaderCaptureTests() {
    using edvr::flatProbeShaderFits;
    static_assert(flatProbeShaderFits(0, 0, 1), "first shader fits");
    static_assert(flatProbeShaderFits(2047, 15u * 1024u * 1024u, 1024u * 1024u), "exact capacity fits");
    static_assert(!flatProbeShaderFits(2048, 0, 1), "count capacity refuses");
    static_assert(!flatProbeShaderFits(0, 16u * 1024u * 1024u, 1), "byte capacity refuses");
    static_assert(!flatProbeShaderFits(0, size_t(-1), 1), "overflow cannot admit");
    static_assert(!flatProbeShaderFits(0, 0, 0), "empty shader refuses");
    static_assert(!flatProbeShaderFits(0, 0, 1024u * 1024u + 1), "single shader size is bounded");
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::printf("FAIL: flat shader capture %s\n", what); ++failures; }
    };
    std::atomic<uint32_t> attempted{0};
    uint32_t all = 0;
    for (const auto& key : edvr::kFlatShaderCaptureKeys) {
        const uint32_t bit = edvr::flatShaderCaptureBit(true, key.stage, key.hash);
        check(bit && !(bit & (bit - 1)) && !(all & bit), "unique single-bit admission");
        all |= bit;
        check(!edvr::flatShaderCaptureBit(false, key.stage, key.hash), "non-flat refusal");
        check(!edvr::flatShaderCaptureBit(true, key.stage == 'v' ? 'p' : 'v', key.hash), "stage refusal");
        check(!edvr::flatShaderCaptureBit(true, 'c', key.hash), "compute refusal");
        check(!(attempted.fetch_or(bit) & bit), "first admission");
        check((attempted.fetch_or(bit) & bit) != 0, "duplicate refusal");
    }
    check(all == 0x1fffu, "exact thirteen targets");
    check(!edvr::flatShaderCaptureBit(true, 'v', 0), "unknown refusal");
    check(!edvr::flatShaderCaptureBit(true, 'v', 0xEB5234DB6ADB491Dull), "already-audited refusal");
    check(!edvr::flatShaderCaptureBit(true, 'p', 0x7CECABDE34FFBE9Eull), "already-audited PS refusal");
    return failures;
}
