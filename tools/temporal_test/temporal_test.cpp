// temporal_test -- table tests for the temporal pass's arithmetic
// (src/common/temporal_math.h), without a headset, a game, a device, or
// either DLL: everything under test is header-only.
//
// What a build can pin that a flight cannot cheaply: the jitter sequence
// and the sign of its tangent shift (a jitter told to the game as the wrong
// sign un-jitters the wrong way and wobbles the image by a pixel every
// frame); the pixel-to-direction mapping and its inverse, on a real
// headset's lopsided frustum; the rotation deltas from the runtime's pose
// and from the game's view rows; and the whole reprojection walked by hand
// against a known head turn. The shader in src/d3d11/temporal_pass.cpp
// transcribes the same functions.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../../src/common/temporal_math.h"
#include "../../src/common/temporal_mode.h"

namespace {

int g_fails = 0;

void ok(const char* what) { printf("  ok    %s\n", what); }

void check(bool got, const char* what) {
    if (got) {
        ok(what);
        return;
    }
    printf("  FAIL  %s\n", what);
    ++g_fails;
}

void checkNear(float got, float want, float tol, const char* what) {
    if (fabsf(got - want) <= tol) {
        ok(what);
        return;
    }
    printf("  FAIL  %s -- got %g, wanted %g (tolerance %g)\n", what, got, want, tol);
    ++g_fails;
}

// A rotation about +Y by theta, as the 3x3 of a row-major 3x4.
void yaw34(float theta, float m34[12]) {
    memset(m34, 0, sizeof(float) * 12);
    const float c = cosf(theta), s = sinf(theta);
    m34[0] = c;  m34[2] = s;
    m34[5] = 1.0f;
    m34[8] = -s; m34[10] = c;
}

}  // namespace

int main() {
    for (float scale : {0.5f, 0.65f, 0.999f}) {
        if (strcmp(edvr::temporalNvidiaLabel(scale), "DLSS")) { ++g_fails; puts("FAIL: upscaling label"); }
    }
    for (float scale : {1.0f, 1.25f, 2.0f}) {
        if (strcmp(edvr::temporalNvidiaLabel(scale), "DLAA")) { ++g_fails; puts("FAIL: native or supersampled label"); }
    }
    for (float scale : {0.0f, -1.0f, NAN, INFINITY}) {
        if (strcmp(edvr::temporalNvidiaLabel(scale), "DLSS / DLAA")) { ++g_fails; puts("FAIL: unknown scale label"); }
    }
    for (const char* mode : {"on", "ON", "dlaa", "DLAA", "dlss", "DLSS"}) {
        if (!edvr::temporalModeEnabled(mode)) { ++g_fails; printf("FAIL: bundled temporal mode %s\n", mode); }
    }
    for (const char* mode : {"off", "OFF", "", "bogus"}) {
        if (edvr::temporalModeEnabled(mode)) { ++g_fails; printf("FAIL: inactive temporal mode %s\n", mode); }
    }
    {
        using edvr::temporalCameraFollowScore;
        int score = -30;
        // Recorded yaw/head cancellation: the scene camera barely turns,
        // but its rows predict the stars correctly. Recover on a still
        // head too, rather than waiting indefinitely for another head turn.
        score = temporalCameraFollowScore(score, 1100, true, .25f, .03f);
        check(score == 30, "bound scene camera survives opposing ship/head yaw");
        check(temporalCameraFollowScore(-30, 1100, true, 0, .25f) == 30,
              "bound scene camera recovers with the head stationary");
        for (int i=0;i<16;++i) score = temporalCameraFollowScore(score, 1100, true, .25f, 0);
        check(score == 30, "sustained cancellation cannot disable scene motion");
        check(temporalCameraFollowScore(0, 2, true, .25f, 0) == -4,
              "sparse menu backdrop still uses head-follow detector");
        check(temporalCameraFollowScore(0, 1100, false, .25f, 0) == -4,
              "auxiliary camera chain still triggers resynchronization");
        check(temporalCameraFollowScore(-30, 1100, false, .25f, .25f) == -29,
              "auxiliary camera can regain confidence");
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("edvr temporal test\n\n");

    // ---- The Halton sequence and the jitter drawn from it. ----------------
    {
        using edvr::temporalHalton;
        const float b2[8] = {0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f, 0.0625f};
        const float b3[4] = {1.0f / 3, 2.0f / 3, 1.0f / 9, 4.0f / 9};
        bool h2 = true, h3 = true;
        for (uint32_t i = 0; i < 8; ++i) {
            if (fabsf(temporalHalton(i + 1, 2) - b2[i]) > 1e-6f) h2 = false;
        }
        for (uint32_t i = 0; i < 4; ++i) {
            if (fabsf(temporalHalton(i + 1, 3) - b3[i]) > 1e-6f) h3 = false;
        }
        check(h2, "halton: base 2 is 1/2, 1/4, 3/4, 1/8, 5/8, 3/8, 7/8, 1/16");
        check(h3, "halton: base 3 is 1/3, 2/3, 1/9, 4/9");

        float jx[edvr::kTemporalJitterCount], jy[edvr::kTemporalJitterCount];
        bool inRange = true, distinct = true, periodic = true;
        for (uint32_t n = 0; n < edvr::kTemporalJitterCount; ++n) {
            edvr::temporalJitter(n, &jx[n], &jy[n]);
            if (jx[n] < -0.5f || jx[n] >= 0.5f || jy[n] < -0.5f || jy[n] >= 0.5f) inRange = false;
            for (uint32_t m = 0; m < n; ++m) {
                if (jx[m] == jx[n] && jy[m] == jy[n]) distinct = false;
            }
            float px, py;
            edvr::temporalJitter(n + edvr::kTemporalJitterCount, &px, &py);
            if (px != jx[n] || py != jy[n]) periodic = false;
        }
        check(inRange, "jitter: every offset lies inside the pixel");
        check(distinct, "jitter: the eight offsets are all different");
        check(periodic, "jitter: the sequence repeats after eight frames");
    }

    // ---- The jitter as a tangent shift: the sign, pinned. -----------------
    {
        const float tan[4] = {-1.0f, 1.0f, -1.0f, 1.0f};   // a square 90-degree frustum
        const uint32_t w = 1000, h = 1000;
        float dx = 0.0f, dy = 0.0f;
        edvr::temporalJitterToTangents(0.5f, 0.5f, tan, w, h, &dx, &dy);
        checkNear(dx, -0.001f, 1e-7f, "jitter shift: +0.5 px right is l and r moved by -(r-l)/2w");
        checkNear(dy, 0.001f, 1e-7f, "jitter shift: +0.5 px down is t and b moved by +(b-t)/2h");
        // A fixed direction straight ahead lands on the centre pixel
        // through the unshifted frustum and half a pixel right and down
        // through the shifted one: the content moved by the jitter.
        const float ahead[3] = {0.0f, 0.0f, -1.0f};
        float px0 = 0.0f, py0 = 0.0f, px1 = 0.0f, py1 = 0.0f;
        const float shifted[4] = {tan[0] + dx, tan[1] + dx, tan[2] + dy, tan[3] + dy};
        check(edvr::temporalDirToPixel(ahead, tan, w, h, &px0, &py0) &&
                  edvr::temporalDirToPixel(ahead, shifted, w, h, &px1, &py1),
              "jitter shift: straight ahead projects through both frusta");
        checkNear(px0, 499.5f, 1e-3f, "jitter shift: unshifted, straight ahead is the centre pixel");
        checkNear(px1 - px0, 0.5f, 1e-3f, "jitter shift: the content moved right by the jitter");
        checkNear(py1 - py0, 0.5f, 1e-3f, "jitter shift: ...and down by the jitter");
    }

    // ---- Pixel <-> direction, on the Quest 3's lopsided frustum. ----------
    {
        const float tan[4] = {-1.3764f, 0.8391f, -1.4281f, 0.9657f};
        const uint32_t w = 3096, h = 3312;
        bool roundTrip = true;
        const float probes[5][2] = {{0, 0}, {10.25f, 3300.5f}, {1547.5f, 1655.5f}, {3095, 3311}, {700, 40}};
        for (const float* pr : probes) {
            float d[3], px, py;
            edvr::temporalPixelToDir(pr[0], pr[1], tan, w, h, d);
            if (!edvr::temporalDirToPixel(d, tan, w, h, &px, &py)) roundTrip = false;
            if (fabsf(px - pr[0]) > 1e-2f || fabsf(py - pr[1]) > 1e-2f) roundTrip = false;
        }
        check(roundTrip, "mapping: pixel -> direction -> pixel is the identity across the image");
        // Row 0 looks along the b tangent (up, +0.9657) and the last row
        // along t (down, -1.4281): the guard's field-verified orientation.
        float top[3], bottom[3];
        edvr::temporalPixelToDir(1547.5f, -0.5f, tan, w, h, top);
        edvr::temporalPixelToDir(1547.5f, 3311.5f, tan, w, h, bottom);
        checkNear(top[1], 0.9657f, 1e-4f, "mapping: the top edge looks along b, upward");
        checkNear(bottom[1], -1.4281f, 1e-4f, "mapping: the bottom edge looks along t, downward");
        float d[3];
        d[0] = 0.0f; d[1] = 0.0f; d[2] = 1.0f;   // behind the eye
        float px, py;
        check(!edvr::temporalDirToPixel(d, tan, w, h, &px, &py),
              "mapping: a direction behind the eye has no pixel");
    }

    // ---- The rotation deltas. ---------------------------------------------
    {
        float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        float turned[12];
        const float theta = 1.0f * 3.14159265f / 180.0f;   // one degree
        yaw34(theta, turned);
        check(edvr::temporalRowsAreRotation(ident) && edvr::temporalRowsAreRotation(turned),
              "rotation test: a rotation is one");
        float scaled[12];
        memcpy(scaled, ident, sizeof(scaled));
        scaled[0] = 2.0f;
        float zeros[12] = {};
        check(!edvr::temporalRowsAreRotation(scaled) && !edvr::temporalRowsAreRotation(zeros),
              "rotation test: a scaled or empty matrix is not");

        // The head: identity last frame, one degree of yaw now. The delta
        // takes this frame's directions to last frame's, so straight ahead
        // now is one degree of yaw away from straight ahead then.
        float delta[9];
        edvr::temporalHeadDelta(ident, turned, delta);
        const float ahead[3] = {0.0f, 0.0f, -1.0f};
        float then[3];
        edvr::temporalApply3(delta, ahead, then);
        checkNear(then[0], -sinf(theta), 1e-6f, "head delta: straight ahead now, seen last frame, is turned by the yaw");
        checkNear(then[2], -cosf(theta), 1e-6f, "head delta: ...and still nearly straight ahead");
        // The same turn, undone: last frame's directions through the
        // inverse delta come back.
        float back[9], again[3];
        edvr::temporalHeadDelta(turned, ident, back);
        edvr::temporalApply3(back, then, again);
        checkNear(again[0], 0.0f, 1e-6f, "head delta: the reverse delta undoes it");
        checkNear(again[2], -1.0f, 1e-6f, "head delta: ...exactly");
        // No motion, no delta.
        float none[9];
        edvr::temporalHeadDelta(turned, turned, none);
        bool identity = true;
        for (int i = 0; i < 9; ++i) {
            if (fabsf(none[i] - ((i % 4 == 0) ? 1.0f : 0.0f)) > 1e-6f) identity = false;
        }
        check(identity, "head delta: the same pose twice is the identity");
        // The turn's size, for the registration instrument's speed
        // buckets: one degree reads as one degree, no turn as none.
        checkNear(edvr::temporalRotationAngleDeg(delta), 1.0f, 1e-3f,
                  "rotation angle: a one-degree yaw measures one degree");
        checkNear(edvr::temporalRotationAngleDeg(none), 0.0f, 1e-3f,
                  "rotation angle: no turn measures zero");

        // The translation term. A head that steps 10 cm to the right, no
        // turn: a point 1 m ahead was, last frame, 10 cm further LEFT in
        // eye space (the world moved the other way), whichever eye.
        {
            float stepped[12];
            memcpy(stepped, ident, sizeof(stepped));
            stepped[3] = 0.10f;   // +x translation, row-major 3x4
            const float eyeOff[3] = {-0.032f, 0.0f, 0.0f};
            float tv[3];
            edvr::temporalHeadTranslation(ident, stepped, eyeOff, tv);
            checkNear(tv[0], 0.10f, 1e-6f, "translation: a 10 cm step right moves last frame's view 10 cm along x");
            checkNear(tv[1], 0.0f, 1e-6f, "translation: ...and nothing along y");
            checkNear(tv[2], 0.0f, 1e-6f, "translation: ...or z");
            // The same step seen through a one-degree yaw: the eye's own
            // offset enters through the turn, by (delta - I) e.
            float tv2[3];
            edvr::temporalHeadTranslation(ident, turned, eyeOff, tv2);
            float de[3];
            edvr::temporalApply3(delta, eyeOff, de);
            checkNear(tv2[0], de[0] - eyeOff[0], 1e-6f, "translation: a pure turn moves the eye by (delta - I) e, x");
            checkNear(tv2[2], de[2] - eyeOff[2], 1e-6f, "translation: ...z");
        }
        // Reversed-Z to metres, with the game's planes: the values the
        // cockpit census read (2026-09-03) land where a cockpit is.
        checkNear(edvr::temporalDepthToMetres(0.02007425f, 0.025f, 50000.0f), 1.245f, 0.01f,
                  "depth: 0.0201 reads as 1.25 m with near 0.025 m");
        checkNear(edvr::temporalDepthToMetres(0.00184340f, 0.025f, 50000.0f), 13.56f, 0.05f,
                  "depth: 0.00184 reads as 13.6 m");
        check(edvr::temporalDepthToMetres(0.0f, 0.025f, 50000.0f) > 40000.0f,
              "depth: the far plane reads as the far distance");

        // The mover test (tier 1 of docs/per-object-motion.md): a surface
        // where last frame's depth put it is not a mover; one behind what
        // was there is a disocclusion; one where only sky was has moved in;
        // sky over sky is consistent and sky where a hull was is its trail.
        // The 3x3 range absorbs a grazing floor's gradient and the jitter.
        using edvr::temporalMoverTest;
        check(!temporalMoverTest(10.0f, 9.9f, 10.1f, false, 0.03f),
              "mover: a surface where last frame's depth put it is not a mover");
        check(temporalMoverTest(10.0f, 2.0f, 2.1f, false, 0.03f),
              "mover: a surface behind what was there is a disocclusion");
        check(temporalMoverTest(10.0f, 0.0f, 0.0f, true, 0.03f),
              "mover: a surface where only sky was has moved in");
        check(!temporalMoverTest(0.0f, 0.0f, 0.0f, true, 0.03f),
              "mover: sky over sky is consistent");
        check(temporalMoverTest(0.0f, 5.0f, 5.0f, false, 0.03f),
              "mover: sky where a hull was is the hull's trail");
        check(!temporalMoverTest(100.0f, 96.0f, 104.0f, false, 0.03f),
              "mover: a grazing floor stays inside its 3x3's range");
        check(!temporalMoverTest(10.25f, 10.0f, 10.0f, false, 0.03f),
              "mover: 2.5 percent off is within a 3 percent tolerance");
        check(temporalMoverTest(10.4f, 10.0f, 10.0f, false, 0.03f),
              "mover: 4 percent off is not");
        check(!temporalMoverTest(10.0f, 4.0f, 10.0f, true, 0.03f),
              "mover: a hull's edge against sky, inside the range, is not a mover");
        // A THIN feature -- a text stroke the interface wrote depth under --
        // landing on depthless texels is not a mover; only a thick surface
        // arriving over empty space is (the 2026-09-08 text swim).
        check(!temporalMoverTest(1.4f, 0.0f, 0.0f, true, 0.03f, false),
              "mover: a thin stroke over texels that had no depth is not a mover");
        check(temporalMoverTest(1.4f, 0.0f, 0.0f, true, 0.03f, true),
              "mover: a thick surface over texels that had no depth has moved in");

        // The game's camera: the two readings of the rows differ by a
        // transpose, and the transposed reading is the other's inverse.
        float dv[9], dvT[9], prod[9];
        edvr::temporalViewDelta(turned, ident, false, dv);
        edvr::temporalViewDelta(turned, ident, true, dvT);
        edvr::temporalMul3(dv, dvT, prod);
        bool inverse = true;
        for (int i = 0; i < 9; ++i) {
            if (fabsf(prod[i] - ((i % 4 == 0) ? 1.0f : 0.0f)) > 1e-5f) inverse = false;
        }
        check(inverse, "view delta: the transposed reading is the inverse of the plain one");
    }

    // ---- The reprojection as a whole, by hand. ----------------------------
    {
        const float tan[4] = {-1.0f, 1.0f, -1.0f, 1.0f};
        const uint32_t w = 1000, h = 1000;
        float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        float turned[12];
        const float theta = 1.0f * 3.14159265f / 180.0f;
        yaw34(theta, turned);
        float delta[9];
        edvr::temporalHeadDelta(ident, turned, delta);
        // The centre pixel now was 500 tan(1 deg) = 8.73 pixels to the
        // left last frame (the head turned toward -X, so what is ahead now
        // was to the left of ahead then... and the image records that as a
        // smaller column). The number is the frustum's pixels per unit
        // tangent times the tangent of the turn.
        float ppx, ppy;
        check(edvr::temporalReproject(499.5f, 499.5f, tan, tan, delta, w, h, &ppx, &ppy),
              "reproject: the centre pixel lands on the image after a one-degree turn");
        checkNear(ppx, 499.5f - 500.0f * tanf(theta), 1e-2f,
                  "reproject: ...8.73 pixels along the row, the frustum's scale times tan(1 deg)");
        checkNear(ppy, 499.5f, 1e-3f, "reproject: ...and on the same row");
        // The identity delta is the identity map.
        float none[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        check(edvr::temporalReproject(123.0f, 456.0f, tan, tan, none, w, h, &ppx, &ppy) &&
                  fabsf(ppx - 123.0f) < 1e-3f && fabsf(ppy - 456.0f) < 1e-3f,
              "reproject: no motion maps every pixel to itself");
        // A turn large enough to carry the edge off the image says so.
        float big[12];
        yaw34(60.0f * 3.14159265f / 180.0f, big);
        edvr::temporalHeadDelta(ident, big, delta);
        check(!edvr::temporalReproject(0.0f, 499.5f, tan, tan, delta, w, h, &ppx, &ppy),
              "reproject: a pixel that lands off the image is refused");
        // Under the guard the two frames' frusta can differ (a re-stage);
        // the same direction through a wider previous frustum lands
        // nearer the centre.
        const float wide[4] = {-1.2f, 1.2f, -1.0f, 1.0f};
        check(edvr::temporalReproject(999.0f, 499.5f, tan, wide, none, w, h, &ppx, &ppy) &&
                  ppx < 999.0f && ppx > 900.0f,
              "reproject: a wider previous frustum pulls the same direction inward");
    }

    {
        // Tier 2's body path (temporalBodyPath): with no body motion it is
        // the camera path exactly -- W = R_p^T R_n and tv = R_p^T (c_n -
        // c_p), z-flipped -- and with a fixed camera it is the body's own
        // delta, z-flipped. Two camera poses a yaw apart and a step along
        // x, then a body turning 1 degree about y.
        float prev[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        float now[12];
        yaw34(2.0f * 3.14159265f / 180.0f, now);
        now[3] = 5.0f;   // the camera stepped 5 m along x
        const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const float none3[3] = {0, 0, 0};
        float W[9], tv[3];
        edvr::temporalBodyPath(prev, now, ident, none3, W, tv);
        // The camera path by hand, the way worldFromRows builds it.
        float Rp[9], Rn[9], RpT[9], Wc[9], tvc[3];
        edvr::temporalRot3Of34(prev, Rp);
        edvr::temporalRot3Of34(now, Rn);
        edvr::temporalTranspose3(Rp, RpT);
        edvr::temporalMul3(RpT, Rn, Wc);
        const float dc[3] = {now[3] - prev[3], now[7] - prev[7], now[11] - prev[11]};
        edvr::temporalApply3(RpT, dc, tvc);
        Wc[2] = -Wc[2]; Wc[5] = -Wc[5]; Wc[6] = -Wc[6]; Wc[7] = -Wc[7];
        tvc[2] = -tvc[2];
        bool same = true;
        for (int i = 0; i < 9; ++i) same = same && fabsf(W[i] - Wc[i]) < 1e-6f;
        for (int i = 0; i < 3; ++i) same = same && fabsf(tv[i] - tvc[i]) < 1e-6f;
        check(same, "body path: with no body motion it is the camera path");
        // The carried form: the same path composed with the camera delta
        // handed in rather than the rows. With the camera turned but not
        // moved (c_n = c_p, the one approximation exact) and a body turn of
        // one degree about y, it must equal temporalBodyPath to the float.
        {
            float nowTurn[12];
            yaw34(2.0f * 3.14159265f / 180.0f, nowTurn);
            const float cb = cosf(1.0f * 3.14159265f / 180.0f);
            const float sb = sinf(1.0f * 3.14159265f / 180.0f);
            const float RdY[9] = {cb, 0, sb, 0, 1, 0, -sb, 0, cb};
            const float tdY[3] = {0.3f, -0.2f, 0.1f};
            float Wd[9], tvd[3];
            edvr::temporalBodyPath(prev, nowTurn, RdY, tdY, Wd, tvd);
            float Rn2[9], Wc2[9], tvc2[3];
            edvr::temporalRot3Of34(nowTurn, Rn2);
            edvr::temporalMul3(RpT, Rn2, Wc2);
            const float dc2[3] = {nowTurn[3] - prev[3], nowTurn[7] - prev[7], nowTurn[11] - prev[11]};
            edvr::temporalApply3(RpT, dc2, tvc2);
            Wc2[2] = -Wc2[2]; Wc2[5] = -Wc2[5]; Wc2[6] = -Wc2[6]; Wc2[7] = -Wc2[7];
            tvc2[2] = -tvc2[2];
            float Wk[9], tvk[3];
            edvr::temporalBodyPathCarried(prev, RdY, tdY, Wc2, tvc2, Wk, tvk);
            bool sameK = true;
            for (int i = 0; i < 9; ++i) sameK = sameK && fabsf(Wk[i] - Wd[i]) < 1e-5f;
            for (int i = 0; i < 3; ++i) sameK = sameK && fabsf(tvk[i] - tvd[i]) < 1e-5f;
            check(sameK, "body path: the carried form composes to the rows' form when the camera only turns");
        }
        // A fixed camera at the origin: the path is the body's delta itself
        // (its rotation about y keeps the xz entries, which the flip negates).
        const float c = cosf(1.0f * 3.14159265f / 180.0f);
        const float s = sinf(1.0f * 3.14159265f / 180.0f);
        const float Rd[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
        const float td[3] = {0.5f, 0.0f, -0.25f};
        edvr::temporalBodyPath(prev, prev, Rd, td, W, tv);
        checkNear(W[0], c, 1e-6f, "body path: a fixed camera keeps the body's rotation (xx)");
        checkNear(W[2], -s, 1e-6f, "body path: ...and flips its xz entry into the eye's frame");
        checkNear(W[6], s, 1e-6f, "body path: ...and its zx entry");
        checkNear(tv[0], 0.5f, 1e-6f, "body path: the body's translation passes (x)");
        checkNear(tv[2], 0.25f, 1e-6f, "body path: ...z-flipped");
        // A station part that turned with the body: its world position now,
        // carried back by [Rd | td], must be what the path predicts through
        // the camera -- the algebra's whole claim, checked at one point.
        const float pw[3] = {100.0f, 20.0f, -300.0f};
        float pv[3];   // view-space now: v = R_n^T (w - c_n)
        float RnT[9];
        edvr::temporalTranspose3(Rn, RnT);
        const float wc[3] = {pw[0] - now[3], pw[1] - now[7], pw[2] - now[11]};
        edvr::temporalApply3(RnT, wc, pv);
        edvr::temporalBodyPath(prev, now, Rd, td, W, tv);
        // The path is conjugated into the eye's frame (z back), so it takes
        // an eye-space point and returns one: flip in, flip out, and the
        // compare is in the game's own view space.
        const float pvEye[3] = {pv[0], pv[1], -pv[2]};
        float got[3];
        edvr::temporalApply3(W, pvEye, got);
        for (int i = 0; i < 3; ++i) got[i] += tv[i];
        got[2] = -got[2];
        float wprev[3], want[3];
        edvr::temporalApply3(Rd, pw, wprev);
        for (int i = 0; i < 3; ++i) wprev[i] += td[i];
        const float wp[3] = {wprev[0] - prev[3], wprev[1] - prev[7], wprev[2] - prev[11]};
        edvr::temporalApply3(RpT, wp, want);
        checkNear(got[0], want[0], 1e-3f, "body path: a turned part lands where the world says (x)");
        checkNear(got[1], want[1], 1e-3f, "body path: ...(y)");
        checkNear(got[2], want[2], 1e-3f, "body path: ...(z)");
    }

    {
        // A station's carried motion must be independent of the world's
        // origin, also with simultaneous camera translation and head turn.
        // Compare against direct world-point composition, then shift the
        // entire scene by 13 km while keeping the same physical camera delta.
        const float I[9] = {1,0,0, 0,1,0, 0,0,1};
        const float zero[3] = {};
        float worst = 0.0f, oldError = 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
            for (int sign = -1; sign <= 1; sign += 2) {
                for (int moving = 0; moving < 2; ++moving) {
                    float prev[12], now[12];
                    yaw34(0.4f, prev);
                    yaw34(0.417f, now);
                    for (int k = 0; k < 3; ++k) {
                        prev[k * 4 + 3] = 100.0f * (k + 1);
                        now[k * 4 + 3] = prev[k * 4 + 3] + moving * (k - 1.5f) * 4.0f;
                    }
                    float w[3] = {}, R[9];
                    w[axis] = sign * 0.0006981317f; // 0.04 degrees per frame
                    edvr::temporalRodrigues(w, R);
                    const float t[3] = {0.3f, -0.2f, 0.1f};
                    float cameraW[9], cameraTv[3], wantW[9], wantTv[3];
                    edvr::temporalBodyPath(prev, now, I, zero, cameraW, cameraTv);
                    edvr::temporalBodyPath(prev, now, R, t, wantW, wantTv);
                    for (int shifted = 0; shifted < 2; ++shifted) {
                        float shift[3] = {}, rs[3], ts[3];
                        shift[(axis + 1) % 3] = shifted * sign * 13000.0f;
                        edvr::temporalApply3(R, shift, rs);
                        for (int k = 0; k < 3; ++k) ts[k] = t[k] + shift[k] - rs[k];
                        for (int eye = 0; eye < 2; ++eye) {
                            float gotW[9], gotTv[3];
                            edvr::temporalBodyPathCarried(prev, R, ts, cameraW, cameraTv, gotW, gotTv, shift);
                            for (int k = 0; k < 9; ++k) worst = fmaxf(worst, fabsf(gotW[k] - wantW[k]));
                            for (int k = 0; k < 3; ++k) worst = fmaxf(worst, fabsf(gotTv[k] - wantTv[k]));
                            // Positive control: the old call site rebased the
                            // body translation but left the camera in the old
                            // origin. It must expose metres of error here.
                            if (shifted && !moving) {
                                edvr::temporalBodyPathCarried(prev, R, ts, cameraW, cameraTv, gotW, gotTv);
                                for (int k = 0; k < 3; ++k) oldError = fmaxf(oldError, fabsf(gotTv[k] - wantTv[k]));
                            }
                        }
                    }
                }
            }
        }
        printf("  carried origin regression: corrected max %.6f m, old-origin control %.3f m\n", worst, oldError);
        check(worst < 0.004f, "carried body: origin shift preserves motion under combined head and ship movement");
        check(oldError > 8.0f, "carried body: old camera origin reproduces whole-body displacement");
    }

    {
        // The rigid fit (temporalRigidFit) and Rodrigues: twenty points of a
        // body ten kilometres off, turned 0.05 deg about a tilted axis
        // through a point away from the origin and moved 0.2 m, recovered
        // from their (now, prev) pairs to the metre-per-thousand.
        const float axis[3] = {0.6f, 0.8f, 0.0f};
        const float ang = 0.05f * 3.14159265f / 180.0f;
        const float wTrue[3] = {axis[0] * ang, axis[1] * ang, axis[2] * ang};
        float Rt[9];
        edvr::temporalRodrigues(wTrue, Rt);
        checkNear(edvr::temporalRotationAngleDeg(Rt), 0.05f, 1e-4f, "rodrigues: the angle round-trips");
        const float centre[3] = {8000.0f, -2000.0f, 5000.0f};
        const float shift[3] = {0.2f, 0.0f, -0.1f};
        float pNow[60], pPrev[60];
        for (int i = 0; i < 20; ++i) {
            const float p[3] = {centre[0] + 700.0f * sinf(i * 1.7f), centre[1] + 500.0f * cosf(i * 2.3f),
                                centre[2] + 900.0f * sinf(i * 0.9f + 1.0f)};
            // prev = R (p - centre) + centre + shift: a turn about the centre, then a step
            const float rel[3] = {p[0] - centre[0], p[1] - centre[1], p[2] - centre[2]};
            float rr[3];
            edvr::temporalApply3(Rt, rel, rr);
            for (int k = 0; k < 3; ++k) {
                pNow[i * 3 + k] = p[k];
                pPrev[i * 3 + k] = rr[k] + centre[k] + shift[k];
            }
        }
        float w[3], t[3], rms = 0.0f;
        check(edvr::temporalRigidFit(pNow, pPrev, 20, w, t, &rms), "rigid fit: solves");
        // Float positions at eight kilometres carry half a millimetre; over
        // lever arms of a kilometre that is a few millionths of a radian.
        checkNear(w[0], wTrue[0], 5e-6f, "rigid fit: the axis-angle (x)");
        checkNear(w[1], wTrue[1], 5e-6f, "rigid fit: ...(y)");
        checkNear(w[2], wTrue[2], 5e-6f, "rigid fit: ...(z)");
        // t = centre - R centre + shift
        float rc[3];
        edvr::temporalApply3(Rt, centre, rc);
        checkNear(t[0], centre[0] - rc[0] + shift[0], 0.01f, "rigid fit: the translation (x)");
        checkNear(t[2], centre[2] - rc[2] + shift[2], 0.01f, "rigid fit: ...(z)");
        check(rms < 0.01f, "rigid fit: the residual is millimetres");
    }

    {
        float a=0, b=0;
        check(!edvr::temporalSceneProjection(0,0,.025f,&a,&b), "missing scene row uses the scene fallback");
        const float distances[] = {30.0f,3000.0f,15000.0f,25000.0f};
        for (float metres : distances) {
            const float captured=.025f/metres;
            checkNear(b/(captured-a),metres,.005f,"missing projection still reconstructs actual station distance");
        }
        check(edvr::temporalSceneProjection(0,.025f,.025f,&a,&b),"measured infinite scene row accepted");
        check(!edvr::temporalSceneProjection(1,2,.025f,&a,&b),"unrelated projection cannot create near-plane HUD depth");
        checkNear(a,0,0,"invalid projection leaves infinite scene offset");
        checkNear(b,.025f,0,"invalid projection retains scene near scale");
        const float fa=.025f/(.025f-50000.0f),fb=.025f*50000.0f/(50000.0f-.025f);
        check(edvr::temporalSceneProjection(fa,fb,.025f,&a,&b),"explicit measured finite row is respected");
        checkNear(a,fa,0,"measured finite offset retained");
    }
    if (g_fails) {
        printf("\nTEMPORAL TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("\nTEMPORAL TEST PASSED\n");
    return 0;
}
