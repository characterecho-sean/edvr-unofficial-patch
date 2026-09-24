#pragma once
#include "../../src/d3d11/flat_projection_ownership.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

inline int flatProjectionOwnershipTests() {
    using namespace edvr;
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::printf("FAIL: flat projection ownership %s\n", what); ++failures; }
    };

    // Epic frame 71751, b1[270..275] and deferred VS b2[10..13].
    const float scene[6][4] = {
        {.674860716f, -.714774430f, 0, .684166729f},
        {-.804393589f, -.069881566f, 0, .664024174f},
        {-.240084499f, -1.77504551f, 0, -.301641792f},
        {0, 0, .0250000004f, 0},
        {.684166729f, .664024174f, -.301641792f, 0},
        {-21.0930309f, -24.5114784f, -1.11009693f, 0}};
    const float world[4][4] = {
        {.674860716f, -.804393589f, -.240084499f, -5.74853468f},
        {-.714774430f, -.069881566f, -1.77504551f, -18.7601318f},
        {0, 0, 0, .0250000004f},
        {.684166729f, .664024174f, -.301641792f, 30.3725128f}};
    int sceneIdentity = 0, worldIdentity = 0, otherIdentity = 0;
    FlatProjectionOwnershipInput in{};
    in.referenceBuffer = &sceneIdentity;
    in.referenceCamera = reinterpret_cast<const unsigned char*>(scene);
    in.referenceBytes = sizeof(scene);
    in.currentReference = true;
    in.candidateBuffer = &worldIdentity;
    in.candidateRows = reinterpret_cast<const unsigned char*>(world);
    in.candidateBytes = sizeof(world);
    in.currentCandidate = true;
    in.layout = FlatProjectionOwnershipLayout::ForwardDp4;
    auto result = flatClassifyProjectionOwnership(in);
    check(result.kind == FlatProjectionOwnershipKind::SceneBasisMatch &&
          result.residualsAvailable && result.spatialDepthError == 0 &&
          result.translationResidual > 0 && result.translationResidual < 1.17e-6,
          "captured dp4 scene basis matches exactly; translation is reported");

    // A camera-relative forward matrix has the same scene basis but a very
    // different translation. The result must never imply ownership/admission.
    float changed[4][4];
    std::memcpy(changed, world, sizeof(changed));
    changed[0][3] = 123.0f;
    in.candidateRows = reinterpret_cast<const unsigned char*>(changed);
    result = flatClassifyProjectionOwnership(in);
    check(result.kind == FlatProjectionOwnershipKind::SceneBasisMatch &&
          result.residualsAvailable && result.translationResidual > 100,
          "arbitrary local translation reports basis only with large residual");

    // Captured embedded-HUD b2[6..9] from the same frame has a different
    // local transform, even though its near coefficient is the scene near.
    const float hud[4][4] = {
        {1.06216228f, -.0637366176f, 0, .162549078f},
        {.0206574947f, 1.90711617f, 0, .0875955522f},
        {-.177515388f, -.159436166f, 0, .982804894f},
        {-.338617325f, -2.03848171f, .0250000004f, 1.69080925f}};
    in.candidateRows = reinterpret_cast<const unsigned char*>(hud);
    result = flatClassifyProjectionOwnership(in);
    check(result.kind == FlatProjectionOwnershipKind::Unmatched &&
          result.residualsAvailable && result.spatialDepthError > 0,
          "captured embedded-HUD local projection is unmatched");

    std::memcpy(changed, world, sizeof(changed));
    in.candidateRows = reinterpret_cast<const unsigned char*>(changed);
    changed[0][0] *= 2; // same normalized direction, wrong projection scale
    result = flatClassifyProjectionOwnership(in);
    check(result.kind == FlatProjectionOwnershipKind::Unmatched &&
          result.residualsAvailable && result.spatialDepthError > 0,
          "matching normalized axis does not establish the scene projection");
    std::memcpy(changed, world, sizeof(changed));
    changed[2][3] *= 2;
    result = flatClassifyProjectionOwnership(in);
    check(result.kind == FlatProjectionOwnershipKind::Unmatched &&
          result.spatialDepthError > 0, "different near term is unmatched");

    in.layout = FlatProjectionOwnershipLayout::CanonicalVsB1;
    in.candidateBuffer = &sceneIdentity;
    in.candidateRows = reinterpret_cast<const unsigned char*>(scene);
    in.candidateBytes = sizeof(scene);
    result = flatClassifyProjectionOwnership(in);
    check(result.kind == FlatProjectionOwnershipKind::CanonicalSceneCamera &&
          !result.residualsAvailable, "same current VS b1 and exact 96 bytes");
    in.candidateBuffer = &otherIdentity;
    check(flatClassifyProjectionOwnership(in).kind == FlatProjectionOwnershipKind::Unmatched,
          "matching bytes on another buffer do not prove canonical identity");
    in.candidateBuffer = &sceneIdentity;
    float alteredCamera[6][4];
    std::memcpy(alteredCamera, scene, sizeof(alteredCamera));
    alteredCamera[5][0] += 1;
    in.candidateRows = reinterpret_cast<const unsigned char*>(alteredCamera);
    check(flatClassifyProjectionOwnership(in).kind == FlatProjectionOwnershipKind::Unmatched,
          "changed pose row cannot be canonical even when projection rows match");
    in.candidateRows = reinterpret_cast<const unsigned char*>(scene);
    in.candidateBytes = sizeof(scene) - 1;
    check(flatClassifyProjectionOwnership(in).kind == FlatProjectionOwnershipKind::Unavailable,
          "short canonical capture unavailable");

    in.layout = FlatProjectionOwnershipLayout::ForwardDp4;
    in.candidateBuffer = &worldIdentity;
    in.candidateRows = reinterpret_cast<const unsigned char*>(world);
    in.candidateBytes = sizeof(world);
    in.currentReference = false;
    result = flatClassifyProjectionOwnership(in);
    check(result.kind == FlatProjectionOwnershipKind::Unavailable &&
          !result.residualsAvailable, "old or unassociated reference unavailable");
    in.currentReference = true;
    in.currentCandidate = false;
    check(flatClassifyProjectionOwnership(in).kind == FlatProjectionOwnershipKind::Unavailable,
          "stale candidate unavailable");
    in.currentCandidate = true;
    float nonfinite[4][4];
    std::memcpy(nonfinite, world, sizeof(nonfinite));
    nonfinite[0][0] = (std::numeric_limits<float>::quiet_NaN)();
    in.candidateRows = reinterpret_cast<const unsigned char*>(nonfinite);
    check(flatClassifyProjectionOwnership(in).kind == FlatProjectionOwnershipKind::Unavailable,
          "nonfinite candidate unavailable");
    in.candidateRows = reinterpret_cast<const unsigned char*>(world);
    float invalidScene[6][4];
    std::memcpy(invalidScene, scene, sizeof(invalidScene));
    invalidScene[5][0] = (std::numeric_limits<float>::infinity)();
    in.referenceCamera = reinterpret_cast<const unsigned char*>(invalidScene);
    check(flatClassifyProjectionOwnership(in).kind == FlatProjectionOwnershipKind::Unavailable,
          "nonfinite reference unavailable");
    std::memcpy(invalidScene, scene, sizeof(invalidScene));
    std::memcpy(invalidScene[1], invalidScene[0], sizeof(invalidScene[1]));
    invalidScene[4][1] = invalidScene[1][3];
    check(flatClassifyProjectionOwnership(in).kind == FlatProjectionOwnershipKind::Unavailable,
          "degenerate scene basis unavailable");
    // Every pair/order, including exact positive/negative proportional rows.
    // A different cofactor expansion can leave a nonzero rounding residue.
    for (size_t source = 0; source < 3; ++source) {
        for (size_t target = 0; target < 3; ++target) {
            if (source == target) continue;
            for (float scale : {1.0f, 2.0f, -2.0f}) {
                std::memcpy(invalidScene, scene, sizeof(invalidScene));
                for (size_t column = 0; column < 4; ++column)
                    invalidScene[target][column] = scale * invalidScene[source][column];
                invalidScene[4][target] = invalidScene[target][3];
                result = flatClassifyProjectionOwnership(in);
                check(result.kind == FlatProjectionOwnershipKind::Unavailable &&
                      !result.residualsAvailable, "proportional scene rows unavailable");
            }
        }
    }
    const float dependent[3][3] = {{1, 2, 3}, {4, 5, 7}, {5, 7, 10}};
    std::memcpy(invalidScene, scene, sizeof(invalidScene));
    for (size_t row = 0; row < 3; ++row) {
        invalidScene[row][0] = dependent[row][0];
        invalidScene[row][1] = dependent[row][1];
        invalidScene[row][3] = dependent[row][2];
        invalidScene[4][row] = dependent[row][2];
    }
    check(flatClassifyProjectionOwnership(in).kind == FlatProjectionOwnershipKind::Unavailable,
          "sum-dependent nonproportional scene rows unavailable");
    // Full rank is scale independent; an absolute epsilon would reject the
    // tiny matrix. Negative determinant is equally valid evidence.
    for (float scale : {1.0e-30f, -1.0e-30f, 1.0e30f, -1.0e30f}) {
        std::memset(invalidScene, 0, sizeof(invalidScene));
        invalidScene[0][0] = scale;
        invalidScene[1][1] = scale;
        invalidScene[2][3] = scale;
        invalidScene[3][2] = scene[3][2];
        invalidScene[4][2] = scale;
        std::memset(changed, 0, sizeof(changed));
        changed[0][0] = scale;
        changed[1][1] = scale;
        changed[3][2] = scale;
        changed[2][3] = scene[3][2];
        in.candidateRows = reinterpret_cast<const unsigned char*>(changed);
        result = flatClassifyProjectionOwnership(in);
        check(result.kind == FlatProjectionOwnershipKind::SceneBasisMatch &&
              result.residualsAvailable && result.spatialDepthError == 0,
              "tiny or large full-rank scene basis matches without scale cutoff");
    }
    in.candidateRows = reinterpret_cast<const unsigned char*>(world);
    in.referenceCamera = reinterpret_cast<const unsigned char*>(scene);
    in.referenceBytes = sizeof(scene) - 1;
    check(flatClassifyProjectionOwnership(in).kind == FlatProjectionOwnershipKind::Unavailable,
          "short reference unavailable");

    in = FlatProjectionOwnershipInput{};
    in.layout = FlatProjectionOwnershipLayout::Unsupported;
    result = flatClassifyProjectionOwnership(in);
    check(result.kind == FlatProjectionOwnershipKind::Unsupported &&
          !result.residualsAvailable,
          "inverse, lighting and unproven layouts remain unsupported without bytes");
    return failures;
}
