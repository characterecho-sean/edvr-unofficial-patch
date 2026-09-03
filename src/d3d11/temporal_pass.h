// The temporal anti-aliasing pass -- the d3d11 half of docs/anti-aliasing.md
// Feature B, v1.
//
// Each frame, per eye, the game's finished LDR image is blended with a
// history of the frames before it, each pixel's history fetched from where
// that content sat last frame: the pixel's view direction through this
// frame's frustum, rotated into last frame's view by the camera's motion
// between the two, projected through last frame's frustum, and sampled with
// Catmull-Rom so the history does not blur under repeated resampling. The
// sampled history is clipped to the current frame's 3x3 neighbourhood in
// YCoCg (variance clipping: history that disagrees with everything around
// the pixel is pulled to the nearest agreeing value, which is the standard
// bound on ghosting), blended in with the configured weight, and written as
// both the output and the new history.
//
// What makes this anti-aliasing rather than smoothing is the JITTER: the
// openvr half shifts the projection the game renders through by a sub-pixel
// offset from a Halton (2,3) sequence, one per frame, so consecutive frames
// sample different positions inside every pixel and the history converges
// to a supersample of the scene even with the head held still. The pass
// samples the current frame at the offset and lands it on the unjittered
// grid, so what goes out was drawn through the true projection.
//
// v1 reprojects ROTATION only -- the head's, from the runtime's pose, or the
// game's own camera, from the view rows it writes into its scene constants
// (an experiment, advanced.temporal_aa_motion) -- and needs no depth buffer.
// Translation parallax (head motion against the cockpit, the ship against
// near terrain) is left to the clamp, which is where every injected TAA
// lives before it has depth; the depth reprojection is v2, gated on Phase 0
// item 3.
//
// Runs FIRST at the door: on the game's texture, at render size, wide under
// the cull guard, before the crop and the supersample resolve. Everything
// downstream sees a temporally settled frame. Off by default; every refusal
// stands the pass down for the session with one line.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads fix.temporal_aa (the warm compile and the camera capture want to
// know) and advanced.temporal_aa_view_transpose.
void temporalPassConfigure(Config& cfg);

// Once per frame, from the frame boundary: the warm compile when wanted.
void temporalPassTick(ID3D11DeviceContext* ctx);

// The camera capture for the `camera` motion source. Every write of the
// game's big scene-constants block passes through here (the Unmap tee
// that already feeds the flash detector and the sun-glare fix); the rows
// at float 932 are the true view matrix (measured, sunglare_fix.cpp) and
// are kept pending. The frame's FIRST eye-sized draw latches the pending
// rows as this frame's camera -- the last scene write before the first
// eye draw is that eye's view, and the first eye each frame is the same
// eye each frame, which is all a rotation delta needs.
void temporalPassNoteSceneWrite(const void* data, uint32_t bytes);
void temporalPassNoteFirstEyeDraw();
// This frame's rows become last frame's; called at the frame boundary.
void temporalPassFrameBoundary();

// For the periodic totals line: eye-submits treated, the measured price,
// and the share of pixels whose history was rejected (off the image or
// none yet) or clipped (pulled to the neighbourhood). False when nothing
// has run. The clip and reject shares are the field's instrument for
// whether the reprojection is right: with the head turning and the ship
// steady, a correct delta keeps both low.
bool temporalPassTotals(uint32_t* treated, double* avgMs, double* maxMs,
                        double* rejectPct, double* clipPct);

void temporalPassShutdown();

}  // namespace edvr

extern "C" {
// srcTex:    an ID3D11Texture2D* the game submitted -- its own frame.
// eye:       0 left, 1 right; selects the per-eye owned resources.
// bounds:    the Submit's uMin, vMin, uMax, vMax naming this eye's region of
//            srcTex, or null for the whole texture.
// tanNow:    l, r, t, b -- the frustum the game rendered THIS frame
//            through, jitter excluded (the lied tangents under the guard).
// tanPrev:   the same for the frame whose history is being sampled.
// jxNow/jyNow: this frame's jitter in render pixels (the content sits that
//            far right and down from the unjittered grid).
// deltaHead: 9 floats, row-major, the rotation taking this frame's view
//            directions to last frame's (temporalHeadDelta); may be null.
// motion:    0 none (no reprojection), 1 head (deltaHead), 2 camera (the
//            pass's own capture; falls back to none until a pair exists).
// blend:     history weight 0.5..0.95. clampSigma: the clip's half-width in
//            standard deviations of the 3x3 neighbourhood.
// flags:     bit 0 -- reset the history before this frame (a withheld frame
//            broke continuity; the first frame after an engage).
//
// Returns the treated texture (EDVR-owned, per eye, the source's own format
// family, region-sized, full-span content), or null: the pass refused or
// failed, its own log line says why, and the caller forwards the original
// and stands the pass down.
__declspec(dllexport) void* edvrTemporalAa(void* srcTex, int eye,
                                           const float* bounds,
                                           const float* tanNow,
                                           const float* tanPrev, float jxNow,
                                           float jyNow, const float* deltaHead,
                                           int motion, float blend,
                                           float clampSigma, unsigned flags);
}
