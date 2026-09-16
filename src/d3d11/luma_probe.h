// The luma probe -- docs/edhm-black-cockpit-2026-09-15.md.
//
// A flight with EDHM (3Dmigoto) chained showed a black headset while the
// deferred UI feature was active, and nothing in the log could say which
// pipeline stage went black: the game's own submit, the deferred UI's
// clean snapshot, what DLSS received, what DLSS returned, or the final
// texture handed to the VR half. This probe samples all five, roughly
// every two seconds per eye, on a 16x16 grid, and reports mean/max luma
// and the percentage of near-black samples for each -- cheap enough to
// leave armed, so one flight can localize the fault instead of guessing.
//
// Runs only on the texture stages it is handed; never forces a device
// wait beyond the documented 30-frame fallback; stands a stage down on
// an unsupported format or a multisampled texture without touching the
// picture.
#pragma once

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace edvr {

// The deferred-UI state at the moment a pass finishes, for the probe's
// report line. Filled from ui_deferred's own uiDeferredEyeState(eye) so
// this file need not reach into ui_deferred's internals.
struct LumaProbeState {
    bool deferredEnabled = false;
    bool sampled = false;
    unsigned aliases = 0;
    unsigned draws = 0;
    bool applied = false;
};

// Call once at the very top of the per-eye pass, before any of the
// stages below. Only marks the probe live (the one-shot "armed" line);
// rounds are armed by lumaProbeEnd, so that a round spans the next
// frame's draws (where clean_hdr is sampled, before the pass) and the
// next pass (the other four stages) for the same frame. If the pass
// returns early after arming, nothing is lost: the round finishes on a
// later pass, its clean_hdr sample at most a frame older than the rest.
void lumaProbeBegin(int eye);

// Call with each of the five pipeline stages as its texture becomes
// available this frame, only while still inside the eye's own pass:
//   0 game       -- the texture the game submits
//   1 clean_hdr  -- the deferred UI's world snapshot (ui_deferred.cpp)
//   2 dlss_in    -- what DLSS receives
//   3 dlss_out   -- DLSS output before the UI replay
//   4 final      -- the texture handed to the VR half
// A null `tex` marks that stage absent for this report. Outside an
// armed sampling round for `eye` this is a no-op (no staging texture is
// touched, no CopyResource issued). A no-op when eye is not 0 or 1, or
// stage is outside 0..4.
void lumaProbeSample(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int eye, int stage);

// Call once at the true end of the eye's pass, after the last point any
// of the five stages' textures can still change this frame. Polls any
// pending readbacks without blocking before frame 30 of the wait (then
// does one blocking Map so the round always eventually completes), and
// once every stage for the round is resolved (read, absent or marked
// unsupported), logs the report line and -- only when it changes -- the
// first-black-stage line, then starts the throttle for the next round.
// Finally arms the next round when at least two seconds have passed
// since the last completed report (at once, for the first). A no-op
// when eye is not 0 or 1.
void lumaProbeEnd(ID3D11DeviceContext* ctx, int eye, const LumaProbeState& state);

}  // namespace edvr
