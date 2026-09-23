// The luma probe -- docs/edhm-black-cockpit-2026-09-15.md.
//
// A flight with EDHM (3Dmigoto) chained showed a black headset, and nothing
// in the log could say which pipeline stage went black. This probe samples
// the stages -- the game's own submit, what the upscaler hands on, and the
// final texture handed to the VR half -- roughly every two seconds per eye,
// on a 16x16 grid, and reports mean/max luma and the percentage of
// near-black samples for each: cheap enough to leave armed, so one flight
// can localize the fault instead of guessing. (Two more stages, the deferred
// UI replay's world snapshot and clean input, went with the replay,
// 2026-09-23.)
//
// Runs only on the texture stages it is handed; never forces a device
// wait beyond the documented 30-frame fallback; stands a stage down on
// an unsupported format or a multisampled texture without touching the
// picture.
#pragma once

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace edvr {

// Call once at the very top of the per-eye pass, before any of the
// stages below. Only marks the probe live (the one-shot "armed" line);
// rounds are armed by lumaProbeEnd for the next pass. If a pass returns
// early, the armed round can finish on a later pass.
void lumaProbeBegin(int eye);

// Call with each of the three pipeline stages as its texture becomes
// available this frame, only while still inside the eye's own pass:
//   0 game       -- the texture the game submits
//   1 dlss_out   -- the upscaler's output as the pass hands it on (the UI
//                   resolve applied, where it runs)
//   2 final      -- the texture handed to the VR half
// A null `tex` marks that stage absent for this report. Outside an
// armed sampling round for `eye` this is a no-op (no staging texture is
// touched, no CopyResource issued). A no-op when eye is not 0 or 1, or
// stage is outside 0..2.
void lumaProbeSample(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int eye, int stage);

// Call once at the true end of the eye's pass, after the last point any
// of the stages' textures can still change this frame. Polls any pending
// readbacks without blocking before frame 30 of the wait (then does one
// blocking Map so the round always eventually completes), and once every
// stage for the round is resolved (read, absent or marked unsupported),
// logs the report line and -- only when it changes -- the first-black-stage
// line, then starts the throttle for the next round. Finally arms the next
// round when at least two seconds have passed since the last completed
// report (at once, for the first). A no-op when eye is not 0 or 1.
void lumaProbeEnd(ID3D11DeviceContext* ctx, int eye);

}  // namespace edvr
