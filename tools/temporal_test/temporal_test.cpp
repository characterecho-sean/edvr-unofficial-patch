// temporal_test -- table tests for the temporal pass's arithmetic
// (src/common/temporal_math.h), without a headset, a game, a device, or
// either DLL: everything under test is header-only.
//
// What a build can pin that a flight cannot cheaply: the jitter sequence
// and the sign of its tangent shift (a jitter told to the game as the wrong
// sign un-jitters the wrong way and wobbles the image by a pixel every
// frame); the pixel-to-direction mapping and its inverse, on a real
// headset's lopsided frustum; the rotation deltas from the runtime's pose
// and from the game's view rows; the whole reprojection walked by hand
// against a known head turn; and the world path's gate on the rows' delta,
// replayed over a dumped event (eye run 050423: a parked camera's zero
// taken as the view's and carried). The shader in
// src/d3d11/temporal_shader_source.h transcribes the same functions.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../../src/common/temporal_math.h"
#include "../../src/common/temporal_mode.h"
#include "../../src/d3d11/temporal_history.h"

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

// View rows turned `deg` about +Y and standing at (x, y, z).
void rows34(float deg, float x, float y, float z, float m34[12]) {
    yaw34(deg * 0.0174532925f, m34);
    m34[3] = x; m34[7] = y; m34[11] = z;
}

// One eye's pass through the world path's gate, as temporal_pass.cpp runs
// it: the rows' delta, its angle from the head's delta, then the verdict.
// W and tv come back as the delta the pass would use.
edvr::TemporalCameraStep gateEye(edvr::TemporalCameraGate& g, const float prev[12],
                                 const float now[12], const float head[9],
                                 float W[9], float tv[3], float* diffDeg = nullptr) {
    float mv[3], hT[9], diff[9];
    edvr::temporalWorldFromRows(prev, now, W, tv, mv);
    edvr::temporalTranspose3(head, hT);
    edvr::temporalMul3(W, hT, diff);
    const float d = edvr::temporalRotationAngleDeg(diff);
    if (diffDeg) *diffDeg = d;
    const double move = sqrt(static_cast<double>(mv[0]) * mv[0] +
                             static_cast<double>(mv[1]) * mv[1] +
                             static_cast<double>(mv[2]) * mv[2]);
    return edvr::temporalCameraGateStep(g, prev, now, move, d, W, tv);
}

bool same9(const float a[9], const float b[9]) { return memcmp(a, b, sizeof(float) * 9) == 0; }

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
        // The scanner's initial screen in a sparse system: 49 draws (eye
        // dump 182049, 2026-09-16), a scene, not a menu.
        check(temporalCameraFollowScore(-30, 49, true, 0, .25f) == 30,
              "the scanner's sparse screen is a populated scene");
        check(edvr::kTemporalSceneDrawFloor <= 49u && edvr::kTemporalSceneDrawFloor > 2u,
              "the scene floor sits between the menu's draws and the scanner's");
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
    {
        edvr::TemporalHistory<3> history;
        check(history.size()==0,"temporal history starts explicitly empty");
        edvr::TemporalHistoryEntry entry{};
        for(uint32_t frame=1;frame<=5;++frame) {
            entry.frame=frame;entry.flags=frame==4?33u:2u;entry.output=frame==5?0u:2u;
            entry.selectedSeq=100u+frame;entry.drawSeq=200u+frame;
            entry.cameraChoiceFlags=frame;entry.cameraDrawFlags=frame<<4;
            entry.selectedRows[3]=float(frame)+0.25f;entry.drawRows[11]=-float(frame);
            entry.events=frame==4?17u:8u;history.record(entry);
        }
        check(history.size()==3 && history.oldest(0).frame==3 && history.oldest(2).frame==5,
              "bounded temporal history retains chronological entries after wrap");
        check(history.oldest(1).flags==33 && history.oldest(1).events==17 && history.oldest(2).output==0,
              "reset requests and failed output survive alongside successful calls");
        check(history.oldest(0).selectedSeq==103 && history.oldest(2).drawSeq==205 &&
              history.oldest(1).cameraChoiceFlags==4 && history.oldest(1).cameraDrawFlags==64 &&
              history.oldest(0).selectedRows[3]==3.25f && history.oldest(2).drawRows[11]==-5.0f,
              "camera provenance and full selected/draw rows survive history wrap");
        history.clear();check(history.size()==0,"cleared temporal history cannot report stale success");
        entry.frame=9;history.record(entry);
        check(history.size()==1 && history.oldest(0).frame==9,"history restarts at its first new entry");
    }

    // ---- The world path's gate over a parked camera (eye run 050423). -----
    // Eye 0's rows and head deltas from eye_050423_motion.csv (build
    // c9cab91e, 2026-09-25 05:04, the ship rolling): frames 19994..19999 are
    // real, real, an auxiliary pass parked 148 deg from the view, the same
    // pass's identical write again, real, real. The gate before 2026-09-25
    // accepted 19997's zero as the view's (a still head, well inside the 3
    // degrees) and 19998 carried it: two frames of the world standing still
    // under the roll. docs/camera-rows-carry-2026-09-25.md.
    {
        using edvr::TemporalCameraVerdict;
        using edvr::temporalRotationAngleDeg;
        const float r93[12] = {0.780769169f, 0.194406167f, -0.593806207f, -198.458389f,
                               0.423516452f, 0.534070432f, 0.731712043f, 1471.88684f,
                               0.459383726f, -0.822784841f, 0.33465144f, 767.823914f};
        const float r94[12] = {0.778171837f, 0.206665993f, -0.593074799f, -198.482376f,
                               0.418339074f, 0.53377068f, 0.734902203f, 1472.50903f,
                               0.468445241f, -0.819986522f, 0.32890889f, 768.194946f};
        const float r95[12] = {0.773163915f, 0.220272169f, -0.594724894f, -198.508804f,
                               0.41296041f, 0.536850452f, 0.735700428f, 1473.16528f,
                               0.48133266f, -0.814414859f, 0.324109703f, 768.586121f};
        const float aux[12] = {-0.950436831f, 0.297400296f, 0.0906804204f, -199.47821f,
                               0.0727622211f, 0.496309847f, -0.865090966f, 1491.63611f,
                               -0.302283823f, -0.815616131f, -0.493350685f, 779.845398f};
        const float r98[12] = {0.598011613f, 0.622053683f, -0.505402088f, -199.778397f,
                               0.234719515f, 0.467010468f, 0.852530301f, 1498.0531f,
                               0.766347706f, -0.628450692f, 0.133269534f, 783.657043f};
        const float r99[12] = {0.582894862f, 0.642890394f, -0.496915996f, -199.863831f,
                               0.225240201f, 0.459744751f, 0.859011889f, 1499.52124f,
                               0.780705154f, -0.61263907f, 0.12317808f, 784.564819f};
        const float h94[9] = {0.999999762f, 0.000184280798f, 0.00026550889f,
                              -0.000184312463f, 0.99999994f, 4.96953726e-05f,
                              -0.000265479088f, -4.97214496e-05f, 0.999999881f};
        const float h95[9] = {0.999989986f, 0.00352385081f, 0.00276330113f,
                              -0.00353198126f, 0.99998939f, 0.00295353308f,
                              -0.00275287032f, -0.00296327844f, 0.999991775f};
        const float h96[9] = {0.999999285f, 0.000695446506f, 0.000980585814f,
                              -0.000695226714f, 0.999999762f, -0.000203188509f,
                              -0.000980764627f, 0.00020249933f, 0.999999523f};
        const float h97[9] = {0.999999881f, 9.8310411e-05f, 0.00028476119f,
                              -9.83420759e-05f, 1.0f, 1.80006027e-05f,
                              -0.000284701586f, -1.80006027e-05f, 0.999999881f};
        const float h98[9] = {0.999999762f, -0.000324182212f, 0.00046429038f,
                              0.000324141234f, 0.99999994f, -5.66542149e-05f,
                              -0.000464260578f, 5.68702817e-05f, 0.999999762f};
        const float h99[9] = {0.999999881f, -0.00010949932f, 0.000200748444f,
                              0.000109475106f, 0.99999994f, -3.79234552e-05f,
                              -0.000200778246f, 3.79942358e-05f, 1.0f};
        // What the DLL wrote for 19995 and 19999 (cameraR, cameraTv): the
        // header's arithmetic must be the pass's to the dump's precision.
        const float cam95[9] = {0.99988991f, 0.0144863427f, 0.00319825113f,
                                -0.0144734383f, 0.99988699f, -0.00402030349f,
                                -0.00325605273f, 0.00397354364f, 0.999986768f};
        const float tv95[3] = {0.437213093f, 0.0240675509f, -0.626614213f};
        const float cam99[9] = {0.99973774f, 0.0228724182f, 0.00113745779f,
                                -0.0228532553f, 0.999631405f, -0.0146477595f,
                                -0.00147202611f, 0.0146179274f, 0.999891937f};
        struct Frame { const float* prev; const float* now; const float* head; };
        const Frame run[6] = {{r93, r94, h94}, {r94, r95, h95}, {r95, aux, h96},
                              {aux, aux, h97}, {aux, r98, h98}, {r98, r99, h99}};
        edvr::TemporalCameraGate gate;
        float ownW[6][9], ownTv[6][3], usedW[6][2][9], usedTv[6][2][3], diffDeg[6];
        edvr::TemporalCameraStep step[6][2];
        uint32_t stay[6];
        for (int f = 0; f < 6; ++f) {
            float mv[3];
            edvr::temporalWorldFromRows(run[f].prev, run[f].now, ownW[f], ownTv[f], mv);
            for (int eye = 0; eye < 2; ++eye) {
                step[f][eye] = gateEye(gate, run[f].prev, run[f].now, run[f].head,
                                       usedW[f][eye], usedTv[f][eye], &diffDeg[f]);
            }
            edvr::temporalCameraGateAdvance(gate, true);
            stay[f] = gate.parkedRun;
        }
        float worst = 0.0f;
        for (int i = 0; i < 9; ++i) {
            worst = fmaxf(worst, fabsf(ownW[1][i] - cam95[i]));
            worst = fmaxf(worst, fabsf(ownW[5][i] - cam99[i]));
        }
        for (int i = 0; i < 3; ++i) worst = fmaxf(worst, fabsf(ownTv[1][i] - tv95[i]));
        checkNear(worst, 0.0f, 1e-5f, "050423: the rows' delta is the pass's own (19995 and 19999 as dumped)");
        check(step[1][0].verdict == TemporalCameraVerdict::Own &&
              step[1][1].verdict == TemporalCameraVerdict::Own,
              "050423 19995: the view's delta is accepted");
        check(step[2][0].verdict == TemporalCameraVerdict::Another &&
              step[2][0].carried && same9(usedW[2][0], ownW[1]),
              "050423 19996: the parked pass, 148 deg away, is dropped and 19995's delta carried");
        check(temporalRotationAngleDeg(ownW[3]) < 0.001f && diffDeg[3] <= 3.0f,
              "050423 19997: the parked pass's own delta is a zero the 3-degree test alone passes");
        check(step[3][0].verdict == TemporalCameraVerdict::Parked &&
              step[3][1].verdict == TemporalCameraVerdict::Parked,
              "050423 19997: a zero from rows a drop left behind is refused, by both eyes");
        check(step[3][0].carried && same9(usedW[3][0], ownW[1]) && same9(usedW[3][1], ownW[1]) &&
              memcmp(usedTv[3][0], ownTv[1], sizeof(float) * 3) == 0,
              "050423 19997: the world delta is the carried real one, not zero");
        check(step[4][0].verdict == TemporalCameraVerdict::Another && step[4][0].carried &&
              same9(usedW[4][0], ownW[1]) && temporalRotationAngleDeg(usedW[4][0]) > 0.5f,
              "050423 19998: back on the view, the carried delta is 19995's roll, not the zero");
        check(step[5][0].verdict == TemporalCameraVerdict::Own && !step[5][0].carried &&
              same9(usedW[5][0], ownW[5]) && same9(gate.lastGoodC, ownW[5]),
              "050423 19999: the view's turning delta is accepted at once, as before");
        check(gate.refOwn, "050423: the rows are the view's own again after 19999");
        check(stay[2] == 0 && stay[3] == 1 && stay[4] == 0,
              "050423: the stay is one parked frame, counted once for both eyes");
    }
    // ---- The same gate on the cases the fix must not disturb. -------------
    {
        using edvr::TemporalCameraVerdict;
        using edvr::temporalRotationAngleDeg;
        const float still[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        float a0[12], a1[12], a2[12], a3[12], p[12], pMoved[12], b5[12], b6[12];
        rows34(0.0f, 0, 0, 0, a0);
        rows34(1.0f, 0, 0, 1, a1);
        rows34(15.0f, 0, 0, 10, a2);     // a hitch: a real jump of 14 degrees
        rows34(16.5f, 0, 0, 11, a3);
        rows34(150.0f, 5, 0, 5, p);      // a parked pass
        rows34(150.0f, 5, 0, 6, pMoved); // ...re-parked a metre on, the same turn
        rows34(4.0f, 0, 0, 3, b5);
        rows34(5.5f, 0, 0, 4, b6);
        float W[9], tv[3], d1[9], d1tv[3];
        // A hitch: the view jumps 14 degrees (dropped), then turns on; the
        // turning frame is accepted at once -- recovery costs no frame.
        edvr::TemporalCameraGate g;
        gateEye(g, a0, a1, still, d1, d1tv); edvr::temporalCameraGateAdvance(g, true);
        const auto hitch = gateEye(g, a1, a2, still, W, tv); edvr::temporalCameraGateAdvance(g, true);
        const auto after = gateEye(g, a2, a3, still, W, tv); edvr::temporalCameraGateAdvance(g, true);
        check(hitch.verdict == TemporalCameraVerdict::Another && hitch.carried,
              "a real 14-degree hitch is dropped and the last delta carried");
        check(after.verdict == TemporalCameraVerdict::Own && !after.carried &&
              fabsf(temporalRotationAngleDeg(W) - 1.5f) < 0.01f,
              "the frame after a hitch is accepted on its own turning delta");
        // The view that truly stood still (the chooser's twin fallback) is
        // still the view's own when the rows before it were.
        edvr::TemporalCameraGate s;
        gateEye(s, a0, a1, still, W, tv); edvr::temporalCameraGateAdvance(s, true);
        const auto twin = gateEye(s, a1, a1, still, W, tv);
        check(twin.verdict == TemporalCameraVerdict::Own && temporalRotationAngleDeg(W) < 0.001f,
              "a still twin after the view's own rows is accepted as the identity");
        // A longer stay: every frame on the parked pass, and the exit, carry
        // the view's last delta; a re-parked pass (moved, not turned) too.
        edvr::TemporalCameraGate c;
        gateEye(c, a0, a1, still, d1, d1tv); edvr::temporalCameraGateAdvance(c, true);
        bool allCarried = true;
        uint32_t longest = 0;
        const float* chain[6][2] = {{a1, p}, {p, p}, {p, p}, {p, pMoved}, {pMoved, pMoved}, {pMoved, b5}};
        for (auto& link : chain) {
            const auto r = gateEye(c, link[0], link[1], still, W, tv);
            allCarried = allCarried && r.verdict != TemporalCameraVerdict::Own && r.carried &&
                         same9(W, d1) && memcmp(tv, d1tv, sizeof(tv)) == 0;
            edvr::temporalCameraGateAdvance(c, true);
            if (c.parkedRun > longest) longest = c.parkedRun;
        }
        check(allCarried, "a parked stay of five frames, re-parked once, carries the view's delta throughout");
        check(longest == 4 && c.parkedRun == 0,
              "the stay's length is counted (four parked frames) and ends with it");
        const auto back = gateEye(c, b5, b6, still, W, tv);
        check(back.verdict == TemporalCameraVerdict::Own && same9(c.lastGoodC, W),
              "the view's first turning delta after the stay is accepted");
        // The standing moves on once a frame and needs both eyes: within a
        // frame the second eye judges against the same standing as the
        // first, one eye's drop leaves the rows suspect for the next frame,
        // and rows no delta reached (a frame with no write before them) are
        // not the view's.
        edvr::TemporalCameraGate e;
        float mvs[3];
        gateEye(e, a0, a1, still, W, tv); edvr::temporalCameraGateAdvance(e, true);
        edvr::temporalWorldFromRows(a1, a1, W, tv, mvs);
        const auto eye0 = edvr::temporalCameraGateStep(e, a1, a1, 0.0, 3.5f, W, tv);
        edvr::temporalWorldFromRows(a1, a1, W, tv, mvs);
        const auto eye1 = edvr::temporalCameraGateStep(e, a1, a1, 0.0, 2.5f, W, tv);
        check(eye0.verdict == TemporalCameraVerdict::Another &&
              eye1.verdict == TemporalCameraVerdict::Own,
              "the second eye judges a frame against the same standing as the first");
        edvr::temporalCameraGateAdvance(e, true);
        edvr::temporalWorldFromRows(a1, a1, W, tv, mvs);
        const auto next = edvr::temporalCameraGateStep(e, a1, a1, 0.0, 0.0f, W, tv);
        check(!e.refOwn && next.verdict == TemporalCameraVerdict::Parked,
              "one eye's drop leaves the rows suspect for the next frame");
        edvr::TemporalCameraGate n;
        n.refOwn = true;
        edvr::temporalCameraGateAdvance(n, true);
        check(!n.refOwn, "rows no delta was measured to are not known to be the view's");
        n.refOwn = true;
        n.measured = true;
        edvr::temporalCameraGateAdvance(n, false);
        check(!n.refOwn, "a frame with no rows leaves no reference");
        // The floating origin: a jump's translation is dropped before the
        // last-good store, so a later drop never carries it.
        edvr::TemporalCameraGate j;
        float far1[12];
        rows34(2.0f, 0, 0, 5001, far1);
        gateEye(j, a0, a1, still, d1, d1tv); edvr::temporalCameraGateAdvance(j, true);
        const auto jump = gateEye(j, a1, far1, still, W, tv); edvr::temporalCameraGateAdvance(j, true);
        check(jump.jump && jump.verdict == TemporalCameraVerdict::Own &&
              tv[0] == 0.0f && tv[1] == 0.0f && tv[2] == 0.0f &&
              memcmp(j.lastGoodTv, d1tv, sizeof(d1tv)) == 0 && same9(j.lastGoodC, W),
              "a jump keeps its turn as the last good and the translation before it");
        const auto dropped = gateEye(j, far1, p, still, W, tv);
        check(dropped.carried && !dropped.carriedJump && memcmp(tv, d1tv, sizeof(tv)) == 0,
              "a drop after a jump carries the translation before it (a jump carried: zero)");
    }
    if (g_fails) {
        printf("\nTEMPORAL TEST FAILED (%d)\n", g_fails);
        return 1;
    }
    printf("\nTEMPORAL TEST PASSED\n");
    return 0;
}
