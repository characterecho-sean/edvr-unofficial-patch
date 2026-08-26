// The eye-image checkpoint dump: both eyes' render targets, captured at
// fixed moments inside ONE build frame and written to disk raw.
//
// WHY (2026-08-26, round twenty of the black squares): nineteen rounds
// probed CHANNELS -- textures, constants, depth, blend, masks, history,
// delivery -- and every per-draw channel of the composite and the ring
// family is now forced-identical or off, with the squares surviving in one
// eye. The remaining question is not "which channel" but "WHERE inside the
// frame does the left eye's image start differing from the right's" -- a
// question answered by looking at the pixels, not the bindings. Five
// checkpoints per eye: before and after the ring quad, before and after
// the composite, and at frame end. The offline tile-diff
// (tools/diff_eye_dump.py) then names the interval that introduces the
// divergence -- or proves the eye images identical end to end, which moves
// the defect past the game's rendering entirely.
//
// advanced.fss_eye_dump = N: dump on the Nth body frame after arming
// (N >= 1; mid-build is ~10-15 at 90Hz). One dump per arming; ~700 MB of
// raw pixels in edvr_logs\dumps\. Empty is off and the only shipped state.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

class Config;

void fssDumpConfigure(Config& cfg);

// One bool for the draw path's early-out set and the body-frame gate.
bool fssDumpWantsDraws();

// Called for eye draws behind the body-frame gate; matches the ring quad
// and the composite by vertex hash. True wraps the draw in Begin/End so
// both the before and after images are captured.
bool fssDumpOnEyeDraw(ID3D11DeviceContext* ctx, char kind, uint32_t count,
                      uint32_t instances);

void fssDumpBegin(ID3D11DeviceContext* ctx);
void fssDumpEnd(ID3D11DeviceContext* ctx);

// Around every owner-context Dispatch while a dump is armed: the
// reconstruction runs as compute -- input readable only before the
// dispatch, output only after.
void fssDumpDispatchPre(ID3D11DeviceContext* ctx);
void fssDumpDispatchPost(ID3D11DeviceContext* ctx);

// Frame boundary, with the owner context: counts body frames, takes the
// frame-end checkpoint on the dump frame, then maps and writes everything.
void fssDumpFrameBoundary(ID3D11DeviceContext* ctx);

void fssDumpShutdown();

}  // namespace edvr
