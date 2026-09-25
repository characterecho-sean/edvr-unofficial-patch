#pragma once
#include "../../src/d3d11/flat_projection_bindings.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

inline int flatProjectionBindingsTests() {
    using namespace edvr;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: flat projection bindings %s\n", name); ++failures; }
    };

    FlatProjectionShadowBank<2> bank;
    int first = 0, second = 0, overflow = 0;
    static unsigned char source[65536], changed[65536];
    std::memset(source, 0x65, sizeof(source));
    std::memset(changed, 0x42, sizeof(changed));
    source[65535] = 0xD7;
    FlatProjectionShadowView view{};
    check(bank.ready() && bank.storageBytes() == 131072 &&
          bank.registerBuffer(&first, 11, 65536) &&
          bank.registerBuffer(&second, 1, 16) &&
          !bank.registerBuffer(&overflow, 1, 16) &&
          !bank.registerBuffer(&first, 12, 65536) &&
          !bank.lookup(&first, 11, view), "bounded registration and no inferred bytes");
    check(bank.captureFullWrite(&first, 11, source, sizeof(source)) &&
          bank.lookup(&first, 11, view) && view.width == sizeof(source) &&
          view.bytes[0] == 0x65 && view.bytes[65535] == 0xD7 &&
          view.writeGeneration == 1, "full 64 KiB write has complete provenance");
    const auto key0 = flatProjectionCacheKey(view, 3);
    check(bank.matches(key0, 3) && !bank.matches(key0, 4) &&
          bank.beginMap(&first, 11) && !bank.lookup(&first, 11, view) &&
          !bank.matches(key0, 3) &&
          !bank.captureFullWrite(&first, 11, changed, sizeof(changed)),
          "in-progress map invalidates old bytes and short write fails");
    check(!bank.lookup(&first, 11, view) &&
          !bank.finishMapFull(&first, 11, changed, sizeof(changed)) &&
          bank.beginMap(&first, 11) &&
          !bank.finishMapFull(&first, 11, changed, 16) &&
          !bank.lookup(&first, 11, view), "failed map completion stays invalid");
    check(bank.beginMap(&first, 11) &&
          bank.finishMapFull(&first, 11, changed, sizeof(changed)) &&
          bank.lookup(&first, 11, view) && view.bytes[65535] == 0x42 &&
          view.writeGeneration > key0.writeGeneration &&
          flatProjectionCacheKey(view, 4).phase != key0.phase &&
          !bank.matches(key0, 3),
          "complete map updates generation and phase cache key");
    check(bank.invalidate(&first, 11) && !bank.lookup(&first, 11, view) &&
          !bank.captureFullWrite(&first, 11, nullptr, 65536) &&
          !bank.lookup(&first, 11, view), "copy, partial and unknown writes fail closed");
    check(bank.releaseBuffer(&first, 11) &&
          !bank.lookup(&first, 11, view) &&
          bank.registerBuffer(&first, 12, 65536) &&
          !bank.lookup(&first, 12, view) &&
          bank.captureFullWrite(&first, 12, source, sizeof(source)) &&
          bank.lookup(&first, 12, view) &&
          view.identityGeneration != key0.identityGeneration,
          "identity reuse requires new lifetime generation");
    bank.reset();
    check(!bank.lookup(&first, 12, view) &&
          bank.registerBuffer(&first, 12, 65536) &&
          bank.captureFullWrite(&first, 12, source, sizeof(source)) &&
          bank.lookup(&first, 12, view) && view.bankEpoch != key0.bankEpoch,
          "reset cannot reuse old private-patch key");

    unsigned char raw[256], output[256], before[256];
    std::memset(raw, 0xA5, sizeof(raw));
    float columns[4][4] = {{1,0,0,0.25f},{0,1,0,0.5f},{0,0,1,0},{0,0,0,1}};
    float rays[3][4] = {{2,0,0,3},{0,4,0,5},{1,2,3,7}};
    float lighting[3][4] = {{1,0,0,2},{0,1,0,3},{4,5,6,7}};
    float clip[4][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0},{2,3,4,1}};
    std::memcpy(raw, columns, sizeof(columns));
    std::memcpy(raw + 64, rays, sizeof(rays));
    std::memcpy(raw + 112, lighting, sizeof(lighting));
    std::memcpy(raw + 192, clip, sizeof(clip));
    std::memcpy(before, raw, sizeof(raw));
    FlatProjectionJitter jitter{};
    check(flatProjectionJitter(0.25f, -0.25f, 960, 540, jitter), "pixel phase prepared");
    FlatProjectionPatchRequest requests[4] = {
        {FlatProjectionPatchLayout::ForwardColumns, 0, {}},
        {FlatProjectionPatchLayout::InverseUvRay, 64, {}},
        {FlatProjectionPatchLayout::LightingUvRay, 112,
            {0.25f,-0.25f,960,540,120,8,5,1}},
        {FlatProjectionPatchLayout::InverseClip, 192, {}}
    };
    std::memset(output, 0xC3, sizeof(output));
    check(flatPrepareProjectionPatch(raw, sizeof(raw), requests, 4, jitter,
                                     output, sizeof(output)) &&
          !std::memcmp(raw, before, sizeof(raw)) &&
          !std::memcmp(output + 160, raw + 160, 32),
          "multiple qualified spans patch with raw bytes untouched");
    float gotColumns[4][4], gotRays[3][4], gotLight[3][4], gotClip[4][4];
    std::memcpy(gotColumns, output, sizeof(gotColumns));
    std::memcpy(gotRays, output + 64, sizeof(gotRays));
    std::memcpy(gotLight, output + 112, sizeof(gotLight));
    std::memcpy(gotClip, output + 192, sizeof(gotClip));
    float expectedColumns[4][4], expectedRays[3][4], expectedLight[3][4], expectedClip[4][4];
    std::memcpy(expectedColumns, columns, sizeof(columns));
    std::memcpy(expectedRays, rays, sizeof(rays));
    std::memcpy(expectedLight, lighting, sizeof(lighting));
    std::memcpy(expectedClip, clip, sizeof(clip));
    flatJitterForwardColumns(expectedColumns, jitter);
    flatJitterInverseUvRay(expectedRays, jitter);
    flatJitterInverseUvRay(expectedLight, jitter);
    flatJitterInverseClip(expectedClip, jitter);
    check(!std::memcmp(gotColumns, expectedColumns, sizeof(gotColumns)) &&
          !std::memcmp(gotRays, expectedRays, sizeof(gotRays)) &&
          !std::memcmp(gotLight, expectedLight, sizeof(gotLight)) &&
          !std::memcmp(gotClip, expectedClip, sizeof(gotClip)),
          "layout-specific nonzero transform matches pure math");
    check(flatPrepareProjectionPatch(raw, sizeof(raw), requests, 4,
                                     FlatProjectionJitter{}, output, sizeof(output)) == false,
          "lighting contract rejects mismatched phase");
    const FlatProjectionPatchRequest one{FlatProjectionPatchLayout::ForwardColumns, 0, {}};
    check(flatPrepareProjectionPatch(raw, sizeof(raw), &one, 1,
                                     FlatProjectionJitter{}, output, sizeof(output)) &&
          !std::memcmp(output, raw, sizeof(raw)), "zero phase is byte identity");
    auto zeroLighting=requests[2];zeroLighting.lighting.pixelX=zeroLighting.lighting.pixelY=0;
    check(flatPrepareProjectionPatch(raw,sizeof(raw),&zeroLighting,1,FlatProjectionJitter{},output,sizeof(output)) &&
          !std::memcmp(output,raw,sizeof(raw)),"lighting zero phase accepts either sign of zero");

    std::memset(output, 0xC3, sizeof(output));
    float bad[4][4]; std::memcpy(bad, clip, sizeof(bad));
    bad[3][3] = (std::numeric_limits<float>::infinity)();
    std::memcpy(raw + 192, bad, sizeof(bad));
    check(!flatPrepareProjectionPatch(raw, sizeof(raw), requests, 4, jitter,
                                      output, sizeof(output)) &&
          output[0] == 0xC3 && output[255] == 0xC3,
          "late invalid span leaves entire destination unchanged");
    std::memcpy(raw + 192, clip, sizeof(clip));
    const FlatProjectionPatchRequest overlap[2] = {one, one};
    check(!flatPrepareProjectionPatch(raw, sizeof(raw), overlap, 2, jitter,
                                      output, sizeof(output)) &&
          !flatPrepareProjectionPatch(raw, sizeof(raw), &one, 1, jitter,
                                      raw, sizeof(raw)) &&
          !flatPrepareProjectionPatch(raw, sizeof(raw), &one, 1, jitter,
                                      output, sizeof(output)-1) &&
          output[0] == 0xC3 && output[255] == 0xC3,
          "overlap, alias, capacity rejection is atomic");
    float dp4[4][4] = {{1,2,3,4},{5,6,7,8},{9,10,11,12},{13,14,15,16}};
    float screen[4][4]; std::memcpy(screen, dp4, sizeof(screen));
    std::memcpy(raw, dp4, sizeof(dp4));
    std::memcpy(raw + 64, screen, sizeof(screen));
    const FlatProjectionPatchRequest other[2] = {
        {FlatProjectionPatchLayout::ForwardDp4, 0, {}},
        {FlatProjectionPatchLayout::InverseScreenRay, 64, {}}
    };
    check(flatPrepareProjectionPatch(raw, sizeof(raw), other, 2, jitter,
                                     output, sizeof(output)), "dp4 and screen spans admitted");
    float expectedDp4[4][4], expectedScreen[4][4];
    std::memcpy(expectedDp4, dp4, sizeof(dp4));
    std::memcpy(expectedScreen, screen, sizeof(screen));
    flatJitterForwardDp4(expectedDp4, jitter);
    flatJitterInverseScreenRay(expectedScreen, jitter);
    check(!std::memcmp(output, expectedDp4, sizeof(expectedDp4)) &&
          !std::memcmp(output + 64, expectedScreen, sizeof(expectedScreen)) &&
          !std::memcmp(output + 128, raw + 128, 128),
          "dp4/screen exact math and unrelated bytes preserved");
    return failures;
}
