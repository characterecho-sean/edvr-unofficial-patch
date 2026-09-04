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

#include <cstddef>
#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads experimental.temporal_aa (the warm compile and the camera capture want to
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
// res is the buffer object written: the rows are kept PER OBJECT, and the
// latch takes the object bound at the frame's first scene draw -- the game
// maps several blocks of the scene block's size a frame (a reflection or
// environment pass has its own camera), and the last write before the
// draw was another camera's on half the frames in space (2026-09-04).
void temporalPassNoteSceneWrite(const void* res, const void* data, uint32_t bytes);
void temporalPassNoteFirstEyeDraw(ID3D11DeviceContext* ctx);
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

// The registration instrument, for the same totals line. The second
// flight (2026-09-03) ghosted and softened the cockpit's text under head
// motion, which is what a history that does not land on near content
// looks like; before v2 builds on depth, the frame-to-pose association
// has to be exact. So every treated frame also asks what FOUR candidate
// deltas would have fetched and judges each by the same clip, counting
// and not using: the head's delta as used, the head's delta one frame
// earlier (a pipelined renderer lands its frame a pose late), the game's
// camera rows read as world->view, and the delta of the game-pose array's
// HMD pose (a renderer that draws with those, predicted a frame further,
// registers with their delta). The transposed camera reading held the
// fourth slot on the third flight and lost to the untransposed one by a
// wide margin (62% clipped by 6.5/255 against 53% by 5.0), so the rows
// are world->view and the slot went to the game pose. And the
// selected candidate's clip share split by head speed (still, slow,
// fast). Each share carries the mean SIZE of its clips in luma: a nudge
// on a text edge is a few 255ths, a history that landed somewhere else
// is tens, and a count alone cannot tell them apart (the main menu's
// turning ship model clips under every candidate alike, for instance).
// Formats the text; false when nothing has run. With the scene still and
// the head turning, the lowest share and size name the exact
// association, and a size that climbs with head speed for ALL of them is
// the translation v2's depth is for.
// Two lines: the candidates and the world figures, then the latch, the
// drops, the per-class clip shares and the probes (the logger caps a line
// at 1200 characters). buf2 may be null.
bool temporalPassRegistration(char* buf, size_t n, char* buf2, size_t n2, char* buf3, size_t n3);

// The trained pass's totals (experimental.temporal_aa = dlaa | dlss): eye-frames it
// took, its measured price, and how many evaluations started NVIDIA's
// history afresh (a handful per session on a good build: each eye's first
// frame, each size change, each withhold). False when it never ran.
bool temporalPassDlaaTotals(uint32_t* frames, double* avgMs, double* maxMs,
                            uint32_t* resets);

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
//            far right and down from the unjittered grid). The current
//            sample is a small jitter-aware Gaussian over the 3x3 around
//            the pixel, and the clip's moments are weighted the same way
//            (the shader says why); advanced.temporal_aa_current = raw
//            restores the point sample for an A/B.
// deltaHead: 9 floats, row-major, the rotation taking this frame's view
//            directions to last frame's (temporalHeadDelta); may be null.
// headTrans: 3 floats, the translation term of the depth reprojection for
//            this eye (temporalHeadTranslation), and headTransSwapped the
//            same for the OTHER eye's offset (the instrument's check of
//            the eye assignment); may be null. nearZ/farZ: the game's
//            planes, for its reversed-Z depth (0 = unknown, no depth).
// headDeg:   the head's turn this frame in degrees, for the instrument.
// motion:    0 none (no reprojection), 1 head (deltaHead, rotation only),
//            2 camera (the pass's own capture; falls back to none until a
//            pair exists), 3 depth (the head's rotation AND translation,
//            per pixel through the scene's depth from the depth probe;
//            rotation only where there is no depth).
// blend:     history weight 0.5..0.95. clampSigma: the clip's half-width in
//            standard deviations of the 3x3 neighbourhood.
// outW/outH: with flags bit 1, the size to come back at -- larger than the
//            frame for DLSS proper (the game rendered a fraction of it),
//            zero or equal for DLAA. Ignored by the pass's own history.
// flags:     bit 0 -- reset the history before this frame (a withheld frame
//            broke continuity; the first frame after an engage); bit 1 --
//            NVIDIA's history (DLAA) instead of the pass's own, when the
//            runtime is there; the pass says so once either way.
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
                                           const float* headTrans,
                                           const float* headTransSwapped,
                                           float nearZ, float farZ,
                                           float headDeg, int motion,
                                           float blend, float clampSigma,
                                           unsigned outW, unsigned outH,
                                           unsigned flags);

// The headset's pose for the frame the history holds and for this one
// (the runtime's 3x4 rows, head -> tracking, the poses the game rendered
// from) and this eye's offset in head space, noted before each
// edvrTemporalAa: the world path composes the ship's camera rows with
// them, since the rows carry no headset. Null pointers clear the note.
__declspec(dllexport) void edvrTemporalAaNoteHead(int eye, const float* prevPose,
                                                  const float* nowPose,
                                                  const float* eyeOffset);
}
