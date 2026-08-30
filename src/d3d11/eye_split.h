// Both eyes' render targets, at every stage of one frame, written to disk.
//
// WHY THIS EXISTS (2026-08-30, the black planet in the right eye)
//
// A commander sees a planetary body render as a featureless black disc in
// the right eye and correctly in the left, at any position in view, on a
// stock game -- the fault predates EDVR. Eight rounds of the draw census
// eliminated every channel the census can read: the two eyes receive the
// same draws, in the same order, with the same textures and the same
// per-eye resources correctly swapped. Live skip probes removed every draw
// we could name and the body never moved. The CS b1 asymmetry that looked
// so promising was equalised in both directions, on all five shaders that
// carry it, and changed nothing.
//
// That is the same wall the FSS black-square hunt hit at round seventeen,
// and it was broken the same way it will be broken here: stop reading what
// the game ASKS FOR and look at the pixels it PRODUCES.
//
// WHY NOT fss_eye_dump
//
// That instrument answers this question already, but only inside the Full
// System Scanner: it recognises the ring quad and the body composite by
// vertex-shader hash and counts "body frames" from them. A world body goes
// through neither, so it never arms. This is the same idea with the FSS
// knowledge removed and replaced with something that needs no knowledge at
// all.
//
// WHAT IT DOES
//
// On one armed frame it remembers every distinct render target the scene
// draws into -- the census's own eye-sized gate decides what counts -- and
// at the frame boundary copies each one and writes it out raw. A deferred
// renderer hands us its stages for free that way: the geometry buffer, the
// lit HDR image and the tonemapped LDR image are each a separate target,
// and each eye has its own. A measured field frame carries SIXTEEN such
// targets, not the six the headline stages suggest -- which at full
// resolution would be 322 MB, so each is written at every fourth texel of
// every fourth row and the set comes to about 20 MB.
//
// The offline comparison (tools\diff_eye_split.py) then pairs the targets
// by shape and reports where the two eyes differ. The FIRST stage at which
// they diverge names the pass that breaks, and no shader hash, draw
// signature or guess about which draw is the body is needed to get there --
// which matters, because every such guess this hunt has made has been wrong.
//
// WHY THE COPIES HAPPEN AT THE BOUNDARY, NOT AT EACH PASS
//
// The two eyes' draws INTERLEAVE -- measured, q ordinals 109..151 against
// 119..181 for one HDR pair -- so "capture when the render target changes"
// would fire dozens of times and copy a gigabyte. Every one of these
// targets keeps its final contents until the next frame overwrites it, so
// one copy each at the end of the frame is the same picture for a tenth of
// the cost. A target the game genuinely recycles mid-frame would be caught
// in its later state, and the manifest's resource pointer is what would
// show that up.
//
// advanced.eye_split = N: dump on the Nth frame after arming (N >= 1;
// counted only over frames that actually drew a scene). One dump per
// arming, one visible hitch. Empty or 0 is off and the only shipped state.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

void eyeSplitConfigure(Config& cfg);

// One bool for the draw path's early-out set.
bool eyeSplitWantsDraws();

// Called for every eye-texture draw while armed: notes which target is
// bound, so the boundary knows what to copy. Changes nothing about the draw.
void eyeSplitOnEyeDraw(ID3D11DeviceContext* ctx);

// Frame boundary, with the owner context: counts scene frames, and on the
// dump frame copies every remembered target and writes it out.
void eyeSplitFrameBoundary(ID3D11DeviceContext* ctx);

void eyeSplitShutdown();

}  // namespace edvr
