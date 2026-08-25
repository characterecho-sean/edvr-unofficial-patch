// The FSS scan-reveal's tile dissolve, held uniform.
//
// THE CLASS (measured 2026-08-25, docs/fss-scanner.md round six): the
// scanner's body layer draws its ring through a 16x16 R16_UINT texture --
// session-stable, never written by any hooked path, bound at PS slot 0 of
// the ring draw and slot 3 of its detail pair. A tiny repeating uint matrix
// feeding a reveal animation is an ordered-dissolve threshold pattern: the
// shader compares scan progress against the matrix, and cells whose
// threshold is not yet crossed render dark -- the black 16-pixel squares
// that pop tile by tile while a body resolves. Deliberate art on a monitor;
// in a headset it is a high-contrast flicker on a zero-disparity plane,
// which five instrument rounds measured as the one genuinely hard-to-fuse
// stimulus the scanner shows.
//
// THE FIX is the loading hologram's, applied here: for exactly the body-
// layer draws that bind the matrix, substitute a uniform one -- every cell
// carries the same threshold, so the tile-by-tile pop becomes one clean
// transition -- and restore the game's texture after each draw. Which
// uniform VALUE means "revealed" depends on the shader's comparison
// direction, so the level is live-tunable: 0 first, 65535 if the ring
// hides instead (the holo fix's 255-vs-0 probe, one flip, same lesson).
//
// Off by default until field-verified. Free when off.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

// fix.fss_scan (stock | steady) and advanced.fss_scan_level (0..65535).
// Read at install and on the reload path; both are live.
void fssScanConfigure(Config& cfg);

// One bool for the draw path's early-out set.
bool fssScanWantsDraws();

// Called for a draw into the BODY-LAYER target only (vscreen gates on the
// target being eye/2-sized or fss_res-inflated, cached per binding
// generation). Scans PS slots 0-3 for the 16x16 fmt-60 matrix and remembers
// which slots carry it; true means wrap the draw in fssScanBegin/End.
bool fssScanOnBodyDraw();

// Substitute the uniform matrix into every remembered slot / restore the
// game's own. The holo fix's begin/end contract exactly.
void fssScanBegin(ID3D11DeviceContext* ctx);
void fssScanEnd(ID3D11DeviceContext* ctx);

void fssScanShutdown();

}  // namespace edvr
