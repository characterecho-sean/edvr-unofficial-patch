// C2-A derive reducer (docs/design-flat-camera-integration.md, C2 test plan
// addendum). Reimplements ONLY the semantics read out of the decompiles; the
// derive half lives in c2_derive_model.h (shared with c2_warp_test), whose
// header comment names the artifacts:
//
//   view finalizer     FUN_1404f4910  camera_cache_helpers.txt:550-618
//   projection builder FUN_1404f2ff0  camera_cache_helpers.txt:255-493
//   VP finalizer       FUN_1404f49f0  camera_cache_helpers.txt:57-195
//   scene CB composer  FUN_140596830  camera_producer.txt:23-124
//   refresh ctx copy   FUN_1405921f0  camera_producer.txt:186-249
//   ray snapshot       FUN_1406be790  camera_ray_writers2.txt:13-30
//
// --self-test runs A1-A8 and prints "c2 derive: PASS" only when every check
// holds. Exit 1 with the failed checks named otherwise.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>

#include "c2_derive_model.h"
#include "../../src/d3d11/flat_camera_phase.h"
#include "../../src/d3d11/flat_live_phase.h"
#include "../../src/d3d11/flat_projection_ownership.h"

namespace {

using namespace c2derive;

int g_failures = 0;
void check(bool ok, const char* what) {
    if (ok) { std::printf("  ok    %s\n", what); return; }
    ++g_failures;
    std::printf("  FAIL  %s\n", what);
}

bool feq(float a, float b, float tol) {
    const float d = std::fabs(a - b);
    return d <= tol || d <= tol * (std::fabs(a) + std::fabs(b));
}

// ---------------------------------------------------------------------------
// A1 exactly-once: jitter at the frustum params; the raster-pixel shift after
// the final projection equals the phase; deriving twice changes nothing.
// ---------------------------------------------------------------------------
void testA1() {
    std::printf("A1 exactly-once\n");
    Cam base; makeCamera(base);
    derive(base);
    const float w = 3840.0f, h = 2160.0f;
    float jx, jy;
    edvr::temporalJitter(3, &jx, &jy);
    // The bound-pair delta for the phase, in the D3D rasterizer's
    // coordinates (ndc.y = +1 is the TOP, pixel row 0 is the top):
    // content right by jx needs boundX += jx/w; content down by jy needs
    // boundY += -jy/h. ndc.x = proj[0]*(x/z) + proj[8], and pixel.y runs
    // (1 - ndc.y)*h/2 - 0.5.
    Cam j = base;
    camU(j, kCamFlags) |= kFlagProj | kFlagVP;
    camF(j, kCamBoundX) += jx / w;
    camF(j, kCamBoundY) += -jy / h;
    derive(j);
    static const float kPts[5][3] = {
        {0.0f, 0.0f, 5.0f}, {1.5f, -0.5f, 8.0f}, {-2.0f, 1.0f, 12.0f},
        {0.25f, 0.75f, 3.0f}, {-0.4f, -1.1f, 20.0f},
    };
    bool shiftOk = true;
    for (const auto& pt : kPts) {
        float b[3], jj[3];
        projectPoint(base, pt, b);
        projectPoint(j, pt, jj);
        const float ndcxB = b[0] / pt[2], ndcxJ = jj[0] / pt[2];
        const float ndcyB = b[1] / pt[2], ndcyJ = jj[1] / pt[2];
        const float pxB = (ndcxB + 1.0f) * w * 0.5f - 0.5f;
        const float pxJ = (ndcxJ + 1.0f) * w * 0.5f - 0.5f;
        const float pyB = (1.0f - ndcyB) * h * 0.5f - 0.5f;
        const float pyJ = (1.0f - ndcyJ) * h * 0.5f - 0.5f;
        shiftOk &= feq(pxJ - pxB, jx, 1e-3f) && feq(pyJ - pyB, jy, 1e-3f);
    }
    check(shiftOk, "A1 raster shift equals the phase for every test point");

    // Derive twice: identical products (no accumulation).
    Cam d1 = base, d2 = base;
    camU(d1, kCamFlags) |= kFlagProj | kFlagVP;
    derive(d1);
    camU(d1, kCamFlags) |= kFlagProj | kFlagVP;
    derive(d1);
    camU(d2, kCamFlags) |= kFlagProj | kFlagVP;
    derive(d2);
    bool same = true;
    for (int i = 0; i < 16; ++i) same &= feq(*projSlot(d1, i), *projSlot(d2, i), 1e-7f);
    check(same, "A1 repeat derivation within one execution: identical products");

    // The algebraic identities the transcription must satisfy (kind 3).
    const float t = std::tan(camF(base, kCamAngular) * 0.5f);
    check(feq(*projSlot(base, 0), 1.0f / (t * camF(base, 0x260)), 1e-5f), "A1 identity proj[0] = 1/(tan*scaleX)");
    check(feq(*projSlot(base, 5), 1.0f / t, 1e-5f), "A1 identity proj[5] = 1/tan");
    check(feq(*projSlot(base, 8), 2.0f * camF(base, kCamBoundX), 1e-6f), "A1 identity proj[8] = 2*boundX");
    check(feq(*projSlot(base, 9), 2.0f * camF(base, kCamBoundY), 1e-6f), "A1 identity proj[9] = 2*boundY");
    check(feq(*projSlot(base, 10), 0.0f, 1e-7f) && feq(*projSlot(base, 11), 1.0f, 1e-7f) &&
          feq(*projSlot(base, 14), (camF(base, kCamCompoundNear) + 1.0f) * camF(base, kCamNear), 1e-5f) &&
          feq(*projSlot(base, 15), 0.0f, 1e-7f),
          "A1 reversed-Z infinite-far depth row");
}

// ---------------------------------------------------------------------------
// A2 bit protocol: mutate without bits -> stale reads; with bits -> the
// corresponding blocks re-derive; the ray snapshot is independent state.
// ---------------------------------------------------------------------------
void testA2() {
    std::printf("A2 bit protocol (negative control)\n");
    Cam c; makeCamera(c);
    derive(c);
    float ctxFresh[16], ctxStale[16];
    refreshCopyVp(c, ctxFresh);

    const float oldAngular = camF(c, kCamAngular);
    camF(c, kCamAngular) = oldAngular * 1.01f;
    refreshCopyVp(c, ctxStale);
    bool stale = true;
    for (int i = 0; i < 16; ++i) stale &= feq(ctxStale[i], ctxFresh[i], 1e-7f);
    check(stale, "A2 mutation without bits leaves the refresh copy stale");

    float cbBefore[16], cbAfter[16];
    composeSceneCb(c, cbBefore);
    camF(c, kCamAngular) = oldAngular * 1.02f;
    composeSceneCb(c, cbAfter);
    bool cbStale = true;
    for (int i = 0; i < 16; ++i) cbStale &= feq(cbAfter[i], cbBefore[i], 1e-7f);
    check(cbStale, "A2 composer reads the stale projection while bit 4 is clear");

    camU(c, kCamFlags) |= kFlagProj;
    composeSceneCb(c, cbAfter);
    bool cbFresh = false;
    for (int i = 0; i < 16; ++i) cbFresh |= !feq(cbAfter[i], cbBefore[i], 1e-7f);
    check(cbFresh, "A2 bit 4 re-derives the projection for the composer");
    refreshCopyVp(c, ctxStale);
    bool ctxStillStale = true;
    for (int i = 0; i < 16; ++i) ctxStillStale &= feq(ctxStale[i], ctxFresh[i], 1e-7f);
    check(ctxStillStale, "A2 bit 4 alone leaves the cached-VP copy stale (bit 8 is load-bearing)");
    camU(c, kCamFlags) |= kFlagVP;
    float ctxNow[16];
    refreshCopyVp(c, ctxNow);
    bool ctxMoved = false;
    for (int i = 0; i < 16; ++i) ctxMoved |= !feq(ctxNow[i], ctxStale[i], 1e-7f);
    check(ctxMoved, "A2 bit 8 refreshes the context copy");

    Cam s; makeCamera(s);
    derive(s);
    snapshotRay(s);
    const float snapY = camF(s, kCamRayBasis + 4 * 5);
    camF(s, kCamAxes + 4 * 5) += 0.05f;
    camU(s, kCamFlags) |= kFlagView;
    finalizeViewRows(s);
    const float rowsY = s.F(0x184);
    bool snapshotStale = !feq(camF(s, kCamRayBasis + 4 * 5), rowsY, 1e-7f) &&
                         feq(camF(s, kCamRayBasis + 4 * 5), snapY, 1e-7f);
    check(snapshotStale, "A2 stale ray snapshot is detectable (not silently refreshed)");
    snapshotRay(s);
    check(feq(camF(s, kCamRayBasis + 4 * 5), rowsY, 1e-7f), "A2 the typed writer restores the snapshot");
}

// ---------------------------------------------------------------------------
// A3 canonical mutation form: frustum-param mutation vs direct projection
// edit produce identical VP within tolerance, per kind branch; kinds 4/5 are
// custom-matrix (direct edit trivially equivalent) and must be named.
// ---------------------------------------------------------------------------
void testA3() {
    std::printf("A3 canonical mutation form\n");
    const float jDelta = 0.013f;
    Cam a; makeCamera(a);
    derive(a);
    Cam b = a;
    camU(b, kCamFlags) |= kFlagProj | kFlagVP;
    camF(b, kCamBoundX) += jDelta;
    derive(b);
    Cam d = a;
    camU(d, kCamFlags) |= kFlagVP;
    *projSlot(d, 8) += 2.0f * jDelta;
    derive(d);
    bool eq = true;
    for (int i = 0; i < 16; ++i) eq &= feq(*projSlot(b, i), *projSlot(d, i), 1e-6f);
    check(eq, "A3 kind 3: bound-pair mutation == direct off-centre edit");

    Cam e = a;
    camU(e, kCamFlags) |= kFlagProj | kFlagVP;
    *projSlot(e, 8) += 2.0f * jDelta;
    derive(e);
    check(feq(*projSlot(e, 8), 2.0f * camF(a, kCamBoundX), 1e-6f),
          "A3 a direct edit is overwritten whenever bit 4 rebuilds (must not raise it)");

    Cam c4; makeCamera(c4);
    camU(c4, kCamKind) = 4;
    for (int i = 0; i < 16; ++i) camF(c4, kCamCustom + 4 * i) = (i % 5 == 0) ? 1.0f : 0.1f * i;
    camU(c4, kCamFlags) |= kFlagProj;
    buildProjection(c4);
    check(feq(*projSlot(c4, 2), camF(c4, kCamCustom + 12) - camF(c4, kCamCustom + 8), 1e-6f) &&
          feq(*projSlot(c4, 3), camF(c4, kCamCustom + 12), 1e-6f),
          "A3 kind 4: custom-matrix copy with per-row adjustments");
    std::printf("  note  kinds 4/5: jitter is expressed through +0x2D0..+0x30C or REFUSED; bound-pair fields are not read by these branches\n");

    // Default branch: identity-ish with proj[10] = -1, proj[14] = 1. Kind 0
    // runs the TRIG branch (the decompile's fall-through); the default is the
    // else of kinds 4/5, reached by any other value (here 2).
    Cam cd; makeCamera(cd);
    camU(cd, kCamKind) = 2;
    camU(cd, kCamFlags) |= kFlagProj;
    buildProjection(cd);
    check(feq(*projSlot(cd, 0), 1.0f, 1e-7f) && feq(*projSlot(cd, 5), 1.0f, 1e-7f) &&
          feq(*projSlot(cd, 10), -1.0f, 1e-7f) && feq(*projSlot(cd, 14), 1.0f, 1e-7f),
          "A3 default branch shape (kind 2: identity, proj[10] = -1, proj[14] = 1)");
}

// ---------------------------------------------------------------------------
// A4 override composition: jitter, then the per-item setter shape
// (FUN_140591f30: +0x25C = f2+f14, +0x280 = f14), then the section refresh;
// the override lands and the jitter term survives in raster-pixel shift.
// ---------------------------------------------------------------------------
void testA4() {
    std::printf("A4 override composition\n");
    Cam c; makeCamera(c);
    derive(c);
    const float w = 3840.0f;
    float jx, jy;
    edvr::temporalJitter(5, &jx, &jy);
    camF(c, kCamBoundX) += jx / w;
    camF(c, kCamCompoundNear) = 0.25f;
    camF(c, kCamAngular) = camF(c, kCamAngular) * 1.05f;
    camU(c, kCamFlags) |= kFlagProj | kFlagVP | kFlagRay;
    derive(c);
    check(feq(*projSlot(c, 14), (0.25f + 1.0f) * camF(c, kCamNear), 1e-5f),
          "A4 the per-item compound-near poke reaches the projection");
    check(feq(*projSlot(c, 8), 2.0f * (jx / w), 1e-6f),
          "A4 jitter term survives the override unscaled");
    const float t = std::tan(camF(c, kCamAngular) * 0.5f);
    check(feq(*projSlot(c, 5), 1.0f / t, 1e-5f), "A4 the angular poke reaches the projection");
    const float pt[3] = {0.0f, 0.0f, 5.0f};
    float out[3];
    projectPoint(c, pt, out);
    const float px = ((out[0] / pt[2]) + 1.0f) * w * 0.5f - 0.5f;
    check(feq(px - (w * 0.5f - 0.5f), jx, 1e-3f), "A4 centre-point raster shift equals the phase after composition");
}

// ---------------------------------------------------------------------------
// A5 phase, generation and replay: the five cases, exercised through the
// PRODUCTION FlatLivePhase machine (src/d3d11/flat_live_phase.h).
// ---------------------------------------------------------------------------
void testA5() {
    std::printf("A5 phase, generation and replay (production FlatLivePhase)\n");
    edvr::FlatLivePhase ph;
    ph.beginFrame(true, true, 3840, 2160); ph.finish(true, true);
    ph.beginFrame(true, true, 3840, 2160); ph.finish(true, true);
    ph.beginFrame(true, true, 3840, 2160);

    const float x1 = ph.currentX, y1 = ph.currentY;
    ph.noteApplied();
    const float xa = ph.currentX;
    ph.noteApplied();
    check(ph.applied == 2 && feq(xa, x1, 0.0f) && feq(ph.currentY, y1, 0.0f),
          "A5.1 repeat derivation within the execution keeps one phase");

    ph.finish(true, true);
    const uint32_t seqBefore = ph.phaseSequence;
    ph.beginFrame(true, true, 3840, 2160);
    check(ph.phaseSequence == seqBefore + 1 &&
          (!feq(ph.currentX, x1, 0.0f) || !feq(ph.currentY, y1, 0.0f)),
          "A5.2 new execution advances the phase with unchanged sources");

    // Case 3: a per-item source revision WITHIN the execution -- inject a
    // real source edit (the per-item setter's shape), re-derive, and require
    // the derivatives to rebuild while the group's pixel phase is preserved.
    const float mx = ph.currentX, my = ph.currentY;
    {
        Cam rev; makeCamera(rev);
        derive(rev);
        const float before = *projSlot(rev, 14);
        camF(rev, kCamCompoundNear) = 0.25f;                       // the +0x25C poke
        camF(rev, kCamAngular) = camF(rev, kCamAngular) * 1.05f;   // the +0x280 poke
        camU(rev, kCamFlags) |= kFlagProj | kFlagVP;
        derive(rev);
        check(!feq(*projSlot(rev, 14), before, 1e-6f),
              "A5.3 an injected source revision rebuilds the derivatives");
        check(feq(ph.currentX, mx, 0.0f) && feq(ph.currentY, my, 0.0f),
              "A5.3 the group's pixel phase is preserved across the revision");
    }

    edvr::FlatLivePhase g;
    g.beginFrame(true, true, 3840, 2160); g.finish(true, true);
    g.beginFrame(true, true, 3840, 2160); g.finish(true, true);
    g.beginFrame(true, true, 3840, 2160);
    g.finish(true, true);
    g.beginFrame(true, false, 3840, 2160);
    check(!g.previousAcceptedValid && g.warmFrames == 0,
          "A5.4 generation change rejects stale ownership/history");

    // Case 5: replay -- a recorded execution (source generation + recorded
    // products) replays bit-identically, and a stale generation cannot
    // masquerade as fresh: the products change with the source, and the
    // phase machine's failed closure invalidates an established history.
    Cam r1; makeCamera(r1);
    camF(r1, kCamBoundX) += -0.0002f;
    derive(r1);
    Cam r2; makeCamera(r2);
    camF(r2, kCamBoundX) += -0.0002f;
    derive(r2);
    bool replay = true;
    for (int i = 0; i < 16; ++i) replay &= feq(*projSlot(r1, i), *projSlot(r2, i), 0.0f);
    check(replay, "A5.5 recorded execution replays bit-identically");
    Cam r3; makeCamera(r3);
    camF(r3, kCamAngular) = camF(r3, kCamAngular) * 1.01f; // a new source generation
    camU(r3, kCamFlags) |= kFlagProj | kFlagVP;
    camF(r3, kCamBoundX) += -0.0002f;
    derive(r3);
    bool stale = true;
    for (int i = 0; i < 16; ++i) stale &= feq(*projSlot(r1, i), *projSlot(r3, i), 1e-7f);
    check(!stale, "A5.5 a new source generation changes the products (stale replay detectable)");
    g.beginFrame(true, true, 3840, 2160); g.finish(true, true); // re-establish
    check(g.previousAcceptedValid, "A5.5 a clean closure first validates history");
    g.finish(false, false);
    check(!g.previousAcceptedValid,
          "A5.5 a late/failed execution invalidates established history");
}

// ---------------------------------------------------------------------------
// A6 failure and disable, split at the consumption boundary (the production
// machine's own semantics, flat_live_phase.h:49-75).
// ---------------------------------------------------------------------------
void testA6() {
    std::printf("A6 failure/disable at the consumption boundary (production FlatLivePhase)\n");
    // Before preparation: disabled means no camera mutation may reach
    // rendering at all.
    edvr::FlatLivePhase off;
    off.beginFrame(false, true, 3840, 2160);
    check(feq(off.currentX, 0.0f, 0.0f) && feq(off.currentY, 0.0f, 0.0f) &&
          !off.previousAcceptedValid,
          "A6 before preparation: disabled keeps everything at zero");

    // Before any consumer: a failure keeps the whole frame at zero.
    edvr::FlatLivePhase pre;
    pre.beginFrame(true, true, 3840, 2160); pre.finish(true, true);
    pre.beginFrame(true, true, 3840, 2160); pre.finish(true, true);
    pre.beginFrame(true, true, 3840, 2160);
    pre.fail();
    check(feq(pre.currentX, 0.0f, 0.0f) && feq(pre.currentY, 0.0f, 0.0f) &&
          !pre.needsSpatialFallback(),
          "A6 after mutation before consumption: refusal keeps the frame unjittered");

    // After the first consumer: the committed phase is retained for the
    // frame's remaining work, history is invalidated, and the fallback is
    // the spatial path -- no perfect-rollback claim.
    edvr::FlatLivePhase post;
    post.beginFrame(true, true, 3840, 2160); post.finish(true, true);
    post.beginFrame(true, true, 3840, 2160); post.finish(true, true);
    post.beginFrame(true, true, 3840, 2160);
    const float cx = post.currentX, cy = post.currentY;
    post.noteApplied();
    post.fail();
    check(feq(post.currentX, cx, 0.0f) && feq(post.currentY, cy, 0.0f) &&
          post.needsSpatialFallback(),
          "A6 after first consumer: committed phase retained, spatial fallback named");

    // At backend evaluation: a failed evaluation publishes no history.
    post.finish(false, true);
    check(!post.previousAcceptedValid,
          "A6 at backend evaluation: failure publishes no temporal history");

    // At handoff: the failed frame's remaining work goes through the named
    // spatial recovery, not a hidden temporal fallback -- and the next frame
    // restarts without a stale phase.
    post.beginFrame(true, false, 3840, 2160);
    check(feq(post.currentX, 0.0f, 0.0f) && feq(post.currentY, 0.0f, 0.0f) &&
          !post.previousAcceptedValid,
          "A6 at handoff: the next frame restarts clean (no stale phase)");
}

// ---------------------------------------------------------------------------
// A7 branch fixtures, independently computed (the C2-work review's
// counterexamples, encoded as regression fixtures).
// ---------------------------------------------------------------------------
void testA7() {
    std::printf("A7 branch fixtures (independently computed)\n");
    // Ortho fixture (the review's probe): helper+0x264 = 2.0 (scale),
    // +0x268 = 1.0, +0x240 = 1.6. Expected: p0 = 2, p5 = 3.2, p11 = 0,
    // p15 = 1, and NO fall-through into the trigonometric block.
    Cam o; makeCamera(o);
    camU(o, kCamKind) = 1;
    o.F(0x264) = 2.0f;
    o.F(0x268) = 1.0f;
    o.F(0x240) = 1.6f;
    camU(o, kCamFlags) |= kFlagProj;
    buildProjection(o);
    check(feq(*projSlot(o, 0), 2.0f, 1e-6f) && feq(*projSlot(o, 5), 3.2f, 1e-6f) &&
          feq(*projSlot(o, 11), 0.0f, 1e-7f) && feq(*projSlot(o, 15), 1.0f, 1e-7f),
          "A7 ortho branch: p0 = 2, p5 = 3.2, p11 = 0, p15 = 1 (no trig fall-through)");

    // Oblique fixture (the review's counterexample, computed from the
    // decompile by hand): kind 3, window x [-0.3, 0.7], y [0.1, 0.7], and an
    // unrelated +0x28C = -0.3 that the correct transcription never reads.
    // Expected: proj9 = (f18b+f22)/(f18b-f22) = 1/3, proj8 = 0.6,
    // proj0 = 1.0194, proj5 = 2.7184.
    Cam q; makeCamera(q);
    camU(q, kCamKind) = 3;
    camU(q, kCamFlags) |= kFlagProj;
    q.B(0x2A0) = 1;
    q.F(0x280) = -0.3f; q.F(0x290) = 0.7f;   // x window
    q.F(0x284) = 0.1f;  q.F(0x294) = 0.7f;   // y window
    q.F(0x28C) = -0.3f;                      // the unrelated field
    buildProjection(q);
    check(feq(*projSlot(q, 9), 1.0f / 3.0f, 1e-4f) &&
          feq(*projSlot(q, 8), 0.6f, 1e-4f) &&
          feq(*projSlot(q, 0), 1.0194f, 1e-3f) &&
          feq(*projSlot(q, 5), 2.7184f, 1e-3f),
          "A7 oblique adjust: proj9 = 1/3, proj8 = 0.6 with unrelated +0x28C unread");

    // The no-op branches (the review's kind-2/default probe): the default
    // branch and the repaired ortho branch do NOT read the bound pair, so a
    // bound-pair mutation changes nothing there. These domains need a
    // separately proven mutation form or an explicit refusal -- and the rig
    // now proves the refusal is required.
    Cam d1; makeCamera(d1);
    camU(d1, kCamKind) = 2;
    camF(d1, kCamBoundX) = 0.0f; camF(d1, kCamBoundY) = 0.0f;
    camU(d1, kCamFlags) |= kFlagProj;
    buildProjection(d1);
    Cam d2 = d1;
    camF(d2, kCamBoundX) = 0.013f; camF(d2, kCamBoundY) = -0.007f;
    camU(d2, kCamFlags) |= kFlagProj;
    buildProjection(d2);
    bool noop = true;
    for (int i = 0; i < 16; ++i) noop &= feq(*projSlot(d1, i), *projSlot(d2, i), 0.0f);
    check(noop, "A7 default branch: bound-pair mutation is a no-op (mutation must be refused here)");
    Cam o2; makeCamera(o2);
    camU(o2, kCamKind) = 1;
    o2.F(0x264) = 2.0f; o2.F(0x268) = 1.0f; o2.F(0x240) = 1.6f;
    camU(o2, kCamFlags) |= kFlagProj;
    buildProjection(o2);
    Cam o3 = o2;
    camF(o3, kCamBoundX) = 0.013f;
    camU(o3, kCamFlags) |= kFlagProj;
    buildProjection(o3);
    bool onoop = true;
    for (int i = 0; i < 16; ++i) onoop &= feq(*projSlot(o2, i), *projSlot(o3, i), 0.0f);
    check(onoop, "A7 ortho branch: bound-pair mutation is a no-op (mutation must be refused here)");
}

// ---------------------------------------------------------------------------
// A8 row provenance (the 2026-09-29 wiring addendum). The phase the injector
// applies at the source is IN the scene rows the game composes and uploads,
// it is exactly the legacy scope's shift, it is exactly removable, the
// measured exact-zero encoding survives it, and the live static-pair check
// tells a right provenance claim from a wrong one. Everything is computed on
// the C2 derive model, whose composer is the decompiled FUN_140596830.
// ---------------------------------------------------------------------------
// The six camera rows b1[270..275] as the flat runtime captures them: rows
// 0..3 are the composer's output, row 4 repeats the view direction (the
// w-column) and row 5 is the camera position.
void composeCameraRows(Cam& c, float rows[6][4]) {
    float out[16];
    composeSceneCb(c, out);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) rows[i][j] = out[i * 4 + j];
    rows[4][0] = out[3]; rows[4][1] = out[7]; rows[4][2] = out[11]; rows[4][3] = 0.0f;
    rows[5][0] = camF(c, kCamOrigin + 0); rows[5][1] = camF(c, kCamOrigin + 4);
    rows[5][2] = camF(c, kCamOrigin + 8); rows[5][3] = 0.0f;
}

float maxRowDiff(const float (&a)[6][4], const float (&b)[6][4]) {
    float worst = 0.0f;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            const float d = std::fabs(a[i][j] - b[i][j]);
            if (!(d <= worst)) worst = d;
        }
    return worst;
}

void jitterCamera(const Cam& base, Cam& out, float jx, float jy, float w, float h) {
    out = base;
    camU(out, kCamFlags) |= kFlagProj | kFlagVP;
    camF(out, kCamBoundX) += jx / w;
    camF(out, kCamBoundY) += -jy / h;
    derive(out);
}

void testA8() {
    std::printf("A8 row provenance: the injected phase is in the uploaded rows and exactly removable\n");
    struct Size { uint32_t w, h; };
    static const Size kSizes[] = {{1280, 720}, {2560, 1440}, {3840, 2160}};
    bool legacyShift = true, removable = true, restKept = true, encodingKept = true,
         classifierKept = true, notVacuous = true;
    float worstLegacy = 0.0f, worstBack = 0.0f;
    int idRef = 0;
    for (const auto& sz : kSizes) {
        const float w = static_cast<float>(sz.w), h = static_cast<float>(sz.h);
        Cam base; makeCamera(base); derive(base);
        float rows0[6][4]; composeCameraRows(base, rows0);
        for (uint32_t p = 0; p < 16; ++p) {
            float jx = 0, jy = 0;
            edvr::temporalJitter(p, &jx, &jy);
            Cam jc; jitterCamera(base, jc, jx, jy, w, h);
            float rowsJ[6][4]; composeCameraRows(jc, rowsJ);

            // The game's shift equals the legacy scope's shift.
            edvr::FlatProjectionJitter fj{};
            legacyShift &= edvr::flatProjectionJitter(jx, jy, sz.w, sz.h, fj);
            float legacy[4][4]; std::memcpy(legacy, rows0, sizeof(legacy));
            legacyShift &= edvr::flatJitterForwardColumns(legacy, fj);
            float dl = 0.0f;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) { const float d = std::fabs(legacy[i][j] - rowsJ[i][j]); if (!(d <= dl)) dl = d; }
            if (dl > worstLegacy) worstLegacy = dl;
            legacyShift &= dl <= 1.0e-6f;

            // The exact-zero encoding is independent of the phase, bit for bit.
            for (int i = 0; i < 3; ++i) encodingKept &= rowsJ[i][2] == 0.0f && std::memcmp(&rowsJ[i][2], &rows0[i][2], 4) == 0;
            encodingKept &= std::memcmp(rowsJ[3], rows0[3], sizeof(rowsJ[3])) == 0;       // depth row: no x/y/w to shift
            encodingKept &= std::memcmp(rowsJ[4], rows0[4], sizeof(rowsJ[4])) == 0;       // repeated view direction
            for (int i = 0; i < 3; ++i) encodingKept &= std::memcmp(&rowsJ[i][3], &rows0[i][3], 4) == 0; // w-column
            encodingKept &= rowsJ[3][3] == 0.0f && rowsJ[3][2] > 0.0f;

            // The phase is really there (the test cannot pass on a no-op)...
            if (std::max(std::fabs(jx), std::fabs(jy)) >= 0.05f) notVacuous &= maxRowDiff(rowsJ, rows0) > 4.0e-6f;

            // ...and unjittering it gives the unjittered rows back.
            float back[6][4]; std::memcpy(back, rowsJ, sizeof(back));
            removable &= edvr::flatCameraUnjitterRows(back, jx, jy, sz.w, sz.h);
            const float db = maxRowDiff(back, rows0);
            if (db > worstBack) worstBack = db;
            removable &= db <= 1.0e-6f;
            for (int i = 0; i < 3; ++i) restKept &= std::memcmp(&back[i][2], &rowsJ[i][2], 8) == 0; // components 2 and 3 untouched
            restKept &= std::memcmp(back[3], rowsJ[3], sizeof(back[3])) == 0 &&
                        std::memcmp(back[4], rowsJ[4], sizeof(back[4])) == 0 &&
                        std::memcmp(back[5], rowsJ[5], sizeof(back[5])) == 0;

            // The classifier reads the same verdict from both.
            for (const auto* rows : {&rowsJ, &back}) {
                edvr::FlatProjectionOwnershipInput in{};
                in.referenceBuffer = in.candidateBuffer = &idRef;
                in.referenceCamera = in.candidateRows = reinterpret_cast<const unsigned char*>(*rows);
                in.referenceBytes = in.candidateBytes = sizeof(rowsJ);
                in.currentReference = in.currentCandidate = true;
                in.layout = edvr::FlatProjectionOwnershipLayout::CanonicalVsB1;
                classifierKept &= edvr::flatClassifyProjectionOwnership(in).kind ==
                                  edvr::FlatProjectionOwnershipKind::CanonicalSceneCamera;
            }
        }
    }
    std::printf("      worst |game shift - legacy shift| = %.3g, worst |unjittered - original| = %.3g (row units)\n",
                worstLegacy, worstBack);
    check(legacyShift, "A8 the game's composed rows shift exactly like the legacy scope (x += ndcX*w, y += ndcY*w), 16 phases x 3 sizes");
    check(encodingKept, "A8 rows[i][2], the w-column, the depth row and the view-direction row are bit-identical under jitter");
    check(notVacuous, "A8 the phase is present in the rows (rows differ from the unjittered ones)");
    check(removable, "A8 unjittering recovers the unjittered rows within 1e-6");
    check(restKept, "A8 unjittering touches only the x/y components of rows 0..2");
    check(classifierKept, "A8 the ownership classifier reads CanonicalSceneCamera from jittered and unjittered rows alike");

    // Zero phase is a bit-exact no-op whatever the rows hold; bad input fails without touching them.
    float junk[6][4];
    for (int i = 0; i < 6; ++i) for (int j = 0; j < 4; ++j) junk[i][j] = static_cast<float>(i * 4 + j) * 0.37f - 2.0f;
    junk[1][3] = std::numeric_limits<float>::quiet_NaN();
    float junkCopy[6][4]; std::memcpy(junkCopy, junk, sizeof(junk));
    check(edvr::flatCameraUnjitterRows(junk, 0.0f, 0.0f, 1280, 720) && std::memcmp(junk, junkCopy, sizeof(junk)) == 0,
          "A8 zero phase leaves the rows bit-identical (key-off and legacy frames)");
    check(!edvr::flatCameraUnjitterRows(junk, 0.25f, 0.25f, 1280, 720) && std::memcmp(junk, junkCopy, sizeof(junk)) == 0,
          "A8 a non-finite row refuses and leaves the rows untouched");
    float good[6][4]; { Cam c; makeCamera(c); derive(c); composeCameraRows(c, good); }
    float goodCopy[6][4]; std::memcpy(goodCopy, good, sizeof(good));
    check(!edvr::flatCameraUnjitterRows(good, 0.25f, 0.25f, 0, 720) && !edvr::flatCameraUnjitterRows(good, 0.25f, 0.25f, 1280, 0) &&
          std::memcmp(good, goodCopy, sizeof(good)) == 0, "A8 a zero render extent refuses and leaves the rows untouched");

    // The phase is measurable from one frame's rows (the shift is the x/y
    // column's component along the view direction), and two frames' measured
    // difference is the live check of the provenance claim.
    {
        const float w = 2560.0f, h = 1440.0f;
        Cam base; makeCamera(base); derive(base);
        double mx = 1, my = 1;
        float rawRows[6][4]; composeCameraRows(base, rawRows);
        check(edvr::flatCameraMeasureRowShift(rawRows, mx, my) && std::fabs(mx) <= 1.0e-6 && std::fabs(my) <= 1.0e-6,
              "A8 unjittered rows measure zero shift (right and up are perpendicular to the view direction)");
        float ax = 0, ay = 0, bx = 0, by = 0;
        edvr::temporalJitter(5, &ax, &ay);
        edvr::temporalJitter(6, &bx, &by);
        Cam ca, cb; jitterCamera(base, ca, ax, ay, w, h); jitterCamera(base, cb, bx, by, w, h);
        float ra[6][4], rb[6][4];
        composeCameraRows(ca, ra); composeCameraRows(cb, rb);
        edvr::FlatProjectionJitter claimA{};
        edvr::flatProjectionJitter(ax, ay, 2560, 1440, claimA);
        check(edvr::flatCameraMeasureRowShift(ra, mx, my) && std::fabs(mx - claimA.ndcX) <= 5.0e-7 && std::fabs(my - claimA.ndcY) <= 5.0e-7,
              "A8 jittered rows measure the phase they carry (2*jx/W, -2*jy/H) within 5e-7");
        const auto right = edvr::flatCameraCheckRowPair(rb, bx, by, ra, ax, ay, 2560, 1440);
        check(right.verdict == edvr::FlatCameraPairVerdict::Consistent && right.maxError <= edvr::kFlatCameraPairTolerance,
              "A8 pair, jittered rows, correct claim: Consistent");
        const auto noClaim = edvr::flatCameraCheckRowPair(rb, 0.0f, 0.0f, ra, 0.0f, 0.0f, 2560, 1440);
        check(noClaim.verdict == edvr::FlatCameraPairVerdict::Skipped,
              "A8 pair with equal claimed phases has no power and is skipped");
        const auto wrongA = edvr::flatCameraCheckRowPair(rb, bx, by, ra, 0.0f, 0.0f, 2560, 1440);
        check(wrongA.verdict == edvr::FlatCameraPairVerdict::Inconsistent && wrongA.maxError > 10.0 * edvr::kFlatCameraPairTolerance,
              "A8 negative control: jittered rows whose previous frame claims no phase are Inconsistent");
        const auto wrongB = edvr::flatCameraCheckRowPair(rawRows, bx, by, rawRows, ax, ay, 2560, 1440);
        check(wrongB.verdict == edvr::FlatCameraPairVerdict::Inconsistent,
              "A8 negative control: unjittered rows that claim a phase are Inconsistent");

        // The camera need not be still: a roll about the view direction, a turn, and a camera with an
        // off-centre bound of its own all keep the measured difference equal to the claimed one.
        auto rotateAxes = [&](Cam& c, float angle, bool roll) {
            // axes rows: x = a[0..2], y = a[4..6], forward = a[8..10]
            float x[3], y[3], f[3];
            for (int k = 0; k < 3; ++k) { x[k] = camF(c, kCamAxes + 4 * k); y[k] = camF(c, kCamAxes + 16 + 4 * k); f[k] = camF(c, kCamAxes + 32 + 4 * k); }
            const float ca2 = std::cos(angle), sa2 = std::sin(angle);
            for (int k = 0; k < 3; ++k) {
                if (roll) { camF(c, kCamAxes + 4 * k) = ca2 * x[k] + sa2 * y[k]; camF(c, kCamAxes + 16 + 4 * k) = -sa2 * x[k] + ca2 * y[k]; }
                else      { camF(c, kCamAxes + 4 * k) = ca2 * x[k] + sa2 * f[k]; camF(c, kCamAxes + 32 + 4 * k) = -sa2 * x[k] + ca2 * f[k]; }
            }
        };
        Cam rolled = base; rotateAxes(rolled, 0.2f, true); derive(rolled);
        Cam turned = base; rotateAxes(turned, 0.03f, false); derive(turned);
        // A camera with an off-centre bound of its own (its rows measure that offset on top of the phase).
        Cam offc = base; camF(offc, kCamBoundX) = 0.013f; camF(offc, kCamBoundY) = -0.007f; camU(offc, kCamFlags) |= kFlagProj | kFlagVP; derive(offc);
        Cam offcRolled = offc; rotateAxes(offcRolled, -0.15f, true); derive(offcRolled);
        auto pairWorst = [&](std::initializer_list<Cam*> set, bool& allConsistent) {
            double worst = 0;
            for (Cam* prevCam : set)
                for (Cam* curCam : set) {
                    Cam pj, cj; jitterCamera(*prevCam, pj, ax, ay, w, h); jitterCamera(*curCam, cj, bx, by, w, h);
                    float rp[6][4], rc[6][4]; composeCameraRows(pj, rp); composeCameraRows(cj, rc);
                    const auto r = edvr::flatCameraCheckRowPair(rc, bx, by, rp, ax, ay, 2560, 1440);
                    allConsistent &= r.verdict == edvr::FlatCameraPairVerdict::Consistent;
                    worst = std::max(worst, static_cast<double>(r.maxError));
                }
            return worst;
        };
        bool centredOk = true, offOk = true;
        const double centredWorst = pairWorst({&base, &rolled, &turned}, centredOk);
        const double offWorst = pairWorst({&offc, &offcRolled}, offOk);
        check(centredOk && offOk, "A8 pair stays Consistent across a roll and a turn, for a centred and an off-centre camera (no static camera needed)");
        // The one thing the pair cannot survive is the camera's OWN off-centre bound changing between the
        // frames: that reads as an inconsistency, so a camera-type switch can over-report (the reset frame is not checked).
        Cam pj, cj; jitterCamera(base, pj, ax, ay, w, h); jitterCamera(offc, cj, bx, by, w, h);
        float rp[6][4], rc[6][4]; composeCameraRows(pj, rp); composeCameraRows(cj, rc);
        check(edvr::flatCameraCheckRowPair(rc, bx, by, rp, ax, ay, 2560, 1440).verdict == edvr::FlatCameraPairVerdict::Inconsistent,
              "A8 documented limit: a change of the camera's own off-centre bound between the frames reads as Inconsistent");
        std::printf("      pair check: error %.3g NDC (correct claim), %.3g (previous claims no phase), %.3g (raw rows claiming a phase), worst over motions %.3g (centred) %.3g (off-centre); tolerance %.3g\n",
                    static_cast<double>(right.maxError), static_cast<double>(wrongA.maxError), static_cast<double>(wrongB.maxError),
                    centredWorst, offWorst, edvr::kFlatCameraPairTolerance);
    }
}

int runSelfTest() {
    testA1();
    testA2();
    testA3();
    testA4();
    testA5();
    testA6();
    testA7();
    testA8();
    if (g_failures == 0) {
        std::printf("c2 derive: PASS\n");
        return 0;
    }
    std::printf("c2 derive: %d FAILED check(s)\n", g_failures);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0) return runSelfTest();
    if (argc == 2 && std::strcmp(argv[1], "--dry-run") == 0) {
        std::printf("c2 derive test: dry run (no checks run)\n");
        return 0;
    }
    std::printf("usage: c2_derive_test --self-test|--dry-run\n");
    return 2;
}
