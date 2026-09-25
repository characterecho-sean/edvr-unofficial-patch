#pragma once
#include "../../src/d3d11/flat_projection_math.h"
#include <cstdio>
#include <initializer_list>
#include <utility>

namespace flat_projection_test {
inline bool near(double a, double b, double tolerance = 2e-6) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance;
}
inline void columns(const float (&m)[4][4], const double (&p)[4], double (&out)[4]) {
    for (size_t c = 0; c < 4; ++c) {
        out[c] = 0;
        for (size_t r = 0; r < 4; ++r) out[c] += p[r] * m[r][c];
    }
}
inline void dp4(const float (&m)[4][4], const double (&p)[4], double (&out)[4]) {
    for (size_t r = 0; r < 4; ++r) {
        out[r] = 0;
        for (size_t c = 0; c < 4; ++c) out[r] += m[r][c] * p[c];
    }
}
template<size_t N, class Patch> inline bool rejectionTests(const float (&fixture)[N][4], Patch patch) {
    using namespace edvr;
    float rows[N][4], before[N][4];
    const float huge = (std::numeric_limits<float>::max)();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    FlatProjectionJitter jitter{};
    if (!flatProjectionJitter(.375f, -.25f, 960, 540, jitter)) return false;
    // Invalid source coefficients and overflowing arithmetic must leave no writes.
    std::memcpy(rows, fixture, sizeof(rows)); rows[N - 1][3] = nan;
    std::memcpy(before, rows, sizeof(rows));
    if (patch(rows, jitter) || std::memcmp(rows, before, sizeof(rows))) return false;
    for (auto& row : rows) for (float& value : row) value = huge;
    std::memcpy(before, rows, sizeof(rows));
    if (patch(rows, FlatProjectionJitter{huge, huge, huge, huge}) ||
        std::memcmp(rows, before, sizeof(rows))) return false;
    const FlatProjectionJitter bad[] = {{nan, 0, 0, 0}, {0, nan, 0, 0},
                                      {0, 0, nan, 0}, {0, 0, 0, nan}};
    for (const auto& invalid : bad) {
        std::memcpy(rows, fixture, sizeof(rows));
        if (patch(rows, invalid) || std::memcmp(rows, fixture, sizeof(rows))) return false;
    }
    std::memcpy(rows, fixture, sizeof(rows));
    return patch(rows, FlatProjectionJitter{}) && !std::memcmp(rows, fixture, sizeof(rows));
}
template<size_t N, class Patch, class SetOverflow>
inline bool lateOverflowTest(const float (&fixture)[N][4], Patch patch, SetOverflow setOverflow) {
    float rows[N][4], before[N][4]; std::memcpy(rows, fixture, sizeof(rows));
    setOverflow(rows, (std::numeric_limits<float>::max)());
    std::memcpy(before, rows, sizeof(rows));
    return !patch(rows, edvr::FlatProjectionJitter{1, 1, 1, 1}) &&
        !std::memcmp(rows, before, sizeof(rows));
}
} // namespace flat_projection_test

inline int flatProjectionMathTests() {
    using namespace edvr;
    using namespace flat_projection_test;
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { std::printf("FAIL: flat projection math %s\n", what); ++failures; }
    };
    // Epic 053756 frame36865, section 10 of the flat design investigation.
    const float earlierScene[4][4] = {
        {.384289086f, -.446604401f, 0, .904584467f},
        {-.540786624f, -1.65509605f, 0, -.0248376597f},
        {.848400056f, -.852697492f, 0, -.42557025f},
        {0, 0, .0250000004f, 0}};
    // Epic projection capture frame71751: b1[270..273], payload4 b2[10..16],
    // payload5 b2[7..14], payload3 b2[1..4]. These are recorded float values,
    // not inverses synthesized by the implementation under test. Replaying at
    // 1280x720 below tests render-size algebra; this capture itself was 960x540.
    const float scene[4][4] = {
        {.674860716f, -.714774430f, 0, .684166729f},
        {-.804393589f, -.069881566f, 0, .664024174f},
        {-.240084499f, -1.77504551f, 0, -.301641792f},
        {0, 0, .0250000004f, 0}};
    const float forward[4][4] = {
        {.674860716f, -.804393589f, -.240084499f, -5.74853468f},
        {-.714774430f, -.069881566f, -1.77504551f, -18.7601318f},
        {0, 0, 0, .0250000004f},
        {.684166729f, .664024174f, -.301641792f, 30.3725128f}};
    const float rays[3][4] = {
        {1.16342604f, .389886975f, 0, -.0924897790f},
        {-1.38673425f, .0381181836f, 0, 1.33833230f},
        {-.413893640f, .968231559f, 0, -.578810811f}};
    const float skyForward[4][4] = {
        {.837625027f, -.508776069f, 0, .569760323f},
        {.304977506f, -1.26595712f, 0, -.694787562f},
        {.604565740f, 1.34352958f, 0, -.438911676f},
        {0, 0, .0250000004f, 0}};
    const float skyInverse[4][4] = {
        {.722011685f, .262882948f, .521120429f, -0.0f},
        {-.138760686f, -.345269948f, .366426617f, -0.0f},
        {-0.0f, -0.0f, -0.0f, 39.9999962f},
        {.569760382f, -.694787562f, -.438911676f, -0.0f}};
    const float screenInverse[4][4] = {
        {.928426087f, -0.0f, -0.0f, -0.0f},
        {-0.0f, .522239685f, 0, -0.0f},
        {-0.0f, -0.0f, -0.0f, 40}, {-0.0f, -0.0f, 1, -0.0f}};
    const uint32_t sizes[][2] = {{960, 540}, {1280, 720}};
    const float offsets[][2] = {{.375f, -.25f}, {-.375f, .25f}, {.5f, .5f}, {0, -0.0f}};
    const double points[][4] = {{1, 2, -3, 1}, {10, -4, -8, 1}, {-.2, .3, -1, 1}};
    for (const auto& size : sizes) for (const auto& offset : offsets) {
        FlatProjectionJitter jitter{};
        check(flatProjectionJitter(offset[0], offset[1], size[0], size[1], jitter), "finite offsets admitted");
        check(near(jitter.ndcX * size[0] / 2.0, offset[0]) &&
              near(-jitter.ndcY * size[1] / 2.0, offset[1]), "right/down pixel convention");
        for (const auto* original : {&earlierScene, &scene, &skyForward}) {
            float changed[4][4]; std::memcpy(changed, original, sizeof(changed));
            check(flatJitterForwardColumns(changed, jitter), "column projection admitted");
            for (const auto& point : points) {
                double a[4], b[4]; columns(*original, point, a); columns(changed, point, b);
                check(near((b[0]/b[3] - a[0]/a[3]) * size[0]/2, offset[0], .002) &&
                      near((a[1]/a[3] - b[1]/b[3]) * size[1]/2, offset[1], .002) &&
                      a[2] == b[2] && a[3] == b[3], "projected geometry moves requested pixels with depth unchanged");
            }
            for (size_t i = 0; i < 4; ++i)
                check(!std::memcmp(&changed[i][2], &(*original)[i][2], 2*sizeof(float)), "column z/w bit preservation");
        }
        float changedForward[4][4]; std::memcpy(changedForward, forward, sizeof(forward));
        check(flatJitterForwardDp4(changedForward, jitter), "dp4 projection admitted");
        for (const auto& point : points) {
            double a[4], b[4]; dp4(forward, point, a); dp4(changedForward, point, b);
            check(near((b[0]/b[3] - a[0]/a[3])*size[0]/2, offset[0], .002) &&
                  near((a[1]/a[3] - b[1]/b[3])*size[1]/2, offset[1], .002), "dp4 world positions move requested pixels");
        }
        check(!std::memcmp(changedForward[2], forward[2], 8*sizeof(float)), "dp4 depth/W rows unchanged");
        // Solar surface and smoke use local coordinates. Compose a translated,
        // rotated and nonuniformly scaled model with the captured camera; its
        // coefficients deliberately cannot equal the scene camera. The actual
        // shader contract is screen displacement and preserved clip z/w, not
        // camera equality. Check both branches of the smoke VS vertex scaling
        // and its post-projection +15.01 z adjustment with the same fixture.
        const double model[4][4]={{0,-2,0,31},{.5,0,0,-17},{0,0,3,9},{0,0,0,1}};
        float local[4][4]{}, shifted[4][4];
        for(size_t r=0;r<4;++r)for(size_t c=0;c<4;++c) {
            double value=0;for(size_t k=0;k<4;++k)value+=forward[r][k]*model[k][c];
            local[r][c]=static_cast<float>(value);
        }
        std::memcpy(shifted,local,sizeof(local));
        check(flatJitterForwardDp4(shifted,jitter),"local projection admitted without scene-matrix equality");
        for(double v : {.25,.75})for(const auto& point:points) {
            const double radius=4,inner=.3,outer=.7;
            const double scale=radius-radius*(v>.5?outer:inner)*(2*v-1);
            double vertex[4]={point[0]*scale,point[1]*scale,point[2]*scale,1};
            double a[4],b[4];dp4(local,vertex,a);dp4(shifted,vertex,b);
            // Both real PS shaders form depth UV from interpolated clip xy/w.
            const double u0=.5+.5*a[0]/a[3],v0=.5-.5*a[1]/a[3];
            const double u1=.5+.5*b[0]/b[3],v1=.5-.5*b[1]/b[3];
            check(near((u1-u0)*size[0],offset[0],.002) &&
                  near((v1-v0)*size[1],offset[1],.002),"local depth UV follows raster jitter");
            check(a[3]==b[3] && a[2]==b[2] &&
                  (a[2]+15.01)/a[3]==(b[2]+15.01)/b[3],"local and smoke biased depth remain unchanged");
        }
        float changedScene[4][4], changedRays[3][4];
        std::memcpy(changedScene, scene, sizeof(scene)); std::memcpy(changedRays, rays, sizeof(rays));
        check(flatJitterForwardColumns(changedScene, jitter) && flatJitterInverseUvRay(changedRays, jitter), "paired forward/deferred admitted");
        for (const auto& uv : {std::pair<double, double>{.13, .28}, {.5, .5}, {.91, .77}}) {
            double ray[4] = {0, 0, 0, 0};
            for (size_t i = 0; i < 3; ++i) {
                ray[i] = rays[i][0]*uv.first + rays[i][1]*uv.second + rays[i][3];
                const double reconstructed = changedRays[i][0]*(uv.first + jitter.uvX) +
                    changedRays[i][1]*(uv.second + jitter.uvY) + changedRays[i][3];
                check(near(ray[i], reconstructed), "displaced UV reconstructs unchanged world ray");
                check(!std::memcmp(changedRays[i], rays[i], 3*sizeof(float)), "ray xyz coefficients unchanged");
            }
            double clip[4]; columns(changedScene, ray, clip);
            check(near((clip[0]/clip[3]+1)/2, uv.first + jitter.uvX) &&
                  near((1-clip[1]/clip[3])/2, uv.second + jitter.uvY), "captured deferred inverse ray reprojects through jittered scene camera");
        }
        float changedSky[4][4], changedInverse[4][4], changedScreen[4][4];
        std::memcpy(changedSky, skyForward, sizeof(skyForward));
        std::memcpy(changedInverse, skyInverse, sizeof(skyInverse));
        std::memcpy(changedScreen, screenInverse, sizeof(screenInverse));
        check(flatJitterForwardColumns(changedSky, jitter) && flatJitterInverseClip(changedInverse, jitter) &&
              flatJitterInverseScreenRay(changedScreen, jitter), "sky/screen inverse admitted");
        for (const auto& point : points) {
            double clip[4], reconstructed[4]; columns(changedSky, point, clip); columns(changedInverse, clip, reconstructed);
            for (size_t i = 0; i < 4; ++i) check(near(point[i], reconstructed[i]), "sky projected point reconstructs with original homogeneous coordinates");
            // Screen-space reconstruction uses NDC, then scales ray to view depth.
            const double x = point[0]/point[2]/screenInverse[0][0];
            const double y = point[1]/point[2]/screenInverse[1][1];
            double ray[3]{};
            for (size_t i = 0; i < 3; ++i)
                ray[i] = (x+jitter.ndcX)*changedScreen[0][i] + (y+jitter.ndcY)*changedScreen[1][i] + changedScreen[3][i];
            for (size_t i = 0; i < 3; ++i) check(near(ray[i]*point[2]/ray[2], point[i]), "screen pixel and depth reconstruct original view point");
        }
        check(!std::memcmp(changedInverse, skyInverse, 12*sizeof(float)), "sky inverse basis rows preserved");
        check(!std::memcmp(changedScreen, screenInverse, 12*sizeof(float)) &&
              !std::memcmp(&changedScreen[3][3], &screenInverse[3][3], sizeof(float)), "screen inverse depth/basis/w bits preserved");
        // AFFEF's captured instructions add BOTH CB2[43].xyz and [44].xyz
        // before the view-orientation transform. A nonzero third row must
        // survive; shifting the fourth row still reconstructs the same ray.
        float volumeRay[4][4]={{1.7f,.2f,-.1f,8},{-.3f,2.1f,.4f,9},
                              {.7f,-.9f,1.3f,10},{.1f,.6f,-.2f,11}};
        float shiftedVolume[4][4];std::memcpy(shiftedVolume,volumeRay,sizeof(volumeRay));
        check(flatJitterInverseScreenRay(shiftedVolume,jitter),"volume inverse ray admitted");
        for(const auto& xy:{std::pair<double,double>{-.8,.6},{.25,-.75},{0,0}})
            for(size_t i=0;i<3;++i) {
                const double original=xy.first*volumeRay[0][i]+xy.second*volumeRay[1][i]+
                    volumeRay[2][i]+volumeRay[3][i];
                const double displaced=(xy.first+jitter.ndcX)*shiftedVolume[0][i]+
                    (xy.second+jitter.ndcY)*shiftedVolume[1][i]+shiftedVolume[2][i]+shiftedVolume[3][i];
                check(near(original,displaced),"volume displaced sample preserves two-offset ray");
            }
        check(!std::memcmp(volumeRay,shiftedVolume,12*sizeof(float)) &&
              volumeRay[3][3]==shiftedVolume[3][3],"volume basis, third row and unused w remain unchanged");
    }
    check(rejectionTests(scene, flatJitterForwardColumns), "columns atomic rejection / zero identity");
    check(rejectionTests(forward, flatJitterForwardDp4), "dp4 atomic rejection / zero identity");
    check(rejectionTests(rays, flatJitterInverseUvRay), "UV inverse atomic rejection / zero identity");
    check(rejectionTests(skyInverse, flatJitterInverseClip), "clip inverse atomic rejection / zero identity");
    check(rejectionTests(screenInverse, flatJitterInverseScreenRay), "screen inverse atomic rejection / zero identity");
    check(lateOverflowTest(scene, flatJitterForwardColumns, [](auto& m, float max) {m[3][0] = max; m[3][3] = max;}),
          "column overflow after valid earlier rows is atomic");
    check(lateOverflowTest(forward, flatJitterForwardDp4, [](auto& m, float max) {m[0][3] = max; m[3][3] = max;}),
          "dp4 overflow after valid earlier coefficients is atomic");
    check(lateOverflowTest(rays, flatJitterInverseUvRay, [](auto& m, float max) {m[2][3] = max; m[2][0] = -max;}),
          "UV inverse overflow after valid earlier rows is atomic");
    check(lateOverflowTest(skyInverse, flatJitterInverseClip, [](auto& m, float max) {m[3][3] = max; m[0][3] = -max;}),
          "clip inverse overflow after valid earlier coefficients is atomic");
    check(lateOverflowTest(screenInverse, flatJitterInverseScreenRay, [](auto& m, float max) {m[3][2] = max; m[0][2] = -max;}),
          "screen inverse overflow after valid earlier coefficients is atomic");
    FlatProjectionJitter sentinel{1, 2, 3, 4}, out = sentinel;
    const float inf = std::numeric_limits<float>::infinity(), huge = (std::numeric_limits<float>::max)();
    check(!flatProjectionJitter(0, 0, 0, 540, out) && !flatProjectionJitter(0, 0, 960, 0, out) &&
          !flatProjectionJitter(inf, 0, 960, 540, out) && !flatProjectionJitter(0, -inf, 960, 540, out) &&
          !flatProjectionJitter(huge, 0, 1, 1, out) && !std::memcmp(&out, &sentinel, sizeof(out)), "invalid dimensions/jitter leave output unchanged");
    return failures;
}
