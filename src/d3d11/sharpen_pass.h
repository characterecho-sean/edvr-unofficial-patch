// Render sharpening at the door -- AMD's RCAS on the outgoing frame, the
// d3d11 half.
//
// The LAST treatment before an eye frame leaves for the compositor: one
// compute dispatch of AMD's RCAS (the FSR 1 sharpener, vendored under
// src/d3d11/fsr/ and already carried by the intro upscale) over the eye's
// region, into an EDVR-owned texture of the region's size in the source's
// own format, which the openvr half forwards with full bounds. It exists
// for the pass ahead of it: the temporal pass's history trades a little
// edge contrast for calm, and this hands some of it back at the
// player's chosen strength --
// docs/anti-aliasing.md's "sharpen" in the order at the door, and the seam
// the resolve left marked for exactly this. Built 2026-09-03, after the
// temporal pass's first flight found text a little soft.
//
// RCAS reads a pixel and its four neighbours, and the loads clamp INTO the
// region the way the resolve's taps do, so a double-wide texture's other
// eye is never read at the seam and the frame's edge is not ringed by the
// zero D3D answers for a load off the texture. Nothing here changes size,
// format or orientation.
//
// native_sharpen.cpp now owns the decision (fix.render_sharpness), not the
// openvr half -- src/openvr/sharpen.cpp used to decide, before it was
// retired with the legacy OpenVR proxy. This half owns the pass, behind one
// export it resolves by GetProcAddress and stands down without in the
// theater's "mismatched pair?" voice. Null from the export means "forward
// what you had", and every refusal says why once.
//
// The flat profile calls the same export (flat_sharpen.cpp), on the frame the
// flat resolve hands the game's output copy, with the same setting and the same
// pass and no copy of the shader. Its lines say "frame" where VR's say "eye",
// and its never-ran note names the flat runtime instead of a compositor hook.
//
// One device at a time. The game can recreate its D3D11 device in a running
// process, and nothing this pass makes can be used on another device than its own,
// so the pass remembers the device of the frame it last worked on and, when a frame
// arrives from a different one, releases every resource it made (shader, parameter
// buffer, both eyes' textures and views, the price ring, what it learned about the
// device's formats) and makes them again on the new device, saying so once per
// change. The shader's warm compile from the frame boundary follows the same rule:
// it runs on the device the pass has, or the first device it sees, and never moves
// the pass to another.
#pragma once

#include <cstddef>
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace edvr {

class Config;

// Reads fix.render_sharpness, for two things only this half can do: warm
// the shader ahead of the first engage, and say so when the strength is
// set but no compositor hook ever announces itself (an openvr-only
// install, or every feature that needs that hook was off at launch).
void sharpenPassConfigure(Config& cfg);

// Once per frame, from the frame boundary: the warm compile when wanted
// (the resolve's reason -- a first-use D3DCompile on the submit thread),
// and the missing-hook note.
void sharpenPassTick(ID3D11DeviceContext* ctx);

// For the periodic totals line: how many eye-submits (flat: frames) have been
// sharpened, and the measured price. False when nothing has run.
bool sharpenPassTotals(uint32_t* treated, double* avgMs, double* maxMs);

// The periodic totals line itself, said only while the count moved. Called from
// the frame boundary's 30-second report; the wording is the profile's.
void sharpenPassNoteTotals();

// The note the tick says, once, when the setting has been on for 30 seconds and
// nothing has been sharpened. Pure, so a rig can read every wording: VR names the
// missing compositor hook; flat has no hook and names the anti-aliasing that is
// off, or the flat runtime that handed over no resolved frame.
void sharpenPassNeverRanText(char* out, size_t cap, bool flat, bool antiAliasingOn,
                             float strength);

void sharpenPassShutdown();

// Rigs only. The pass keeps everything it makes -- shader, parameter buffer, both
// eyes' textures and views -- on one D3D11 device, and a frame whose source lives on
// another device releases all of it and starts over (a view over a texture another
// device made is refused, which stood the flat sharpening down for the session: RC4
// review, F5). `true` turns that off and drops what the pass holds, which is how the
// pass behaved before it existed, so a rig can show the failure the reset prevents;
// `false` (the default) turns it back on, again from a clean start.
void sharpenPassDeviceResetOffForTest(bool off);

// Rigs only. What the pass holds and the device each thing was made on (null where it holds
// nothing), so a rig can assert that a device change released every one of them instead of
// trusting a driver to refuse the mix: WARP tolerates a shader, buffer or UAV from another
// device, so a stale one shows nowhere else. Devices are for comparing identity only.
struct SharpenPassHeld {
    ID3D11Device* owner = nullptr;          // the device the pass works on
    ID3D11Device* shader = nullptr;         // the compute shader's
    ID3D11Device* buffer = nullptr;         // the parameter buffer's
    ID3D11Device* eyeOut[2] = {};           // each eye's result texture's
    ID3D11Device* eyeSrcView[2] = {};       // each eye's cached view over its source's
    ID3D11Device* eyeCopy[2] = {};          // each eye's copy-through texture's
    bool          shaderTried = false;      // the latch that says a compile was attempted
    uint32_t      formatSupportAsks = 0;    // how often it asked a device what formats it can store
    int           queriesInFlight = 0;      // price-ring slots holding a query
};
void sharpenPassHeldForTest(SharpenPassHeld* out);

}  // namespace edvr

extern "C" {
// srcTex:   an ID3D11Texture2D* a submit path is about to forward -- the
//           game's own, or the texture the pass before this one produced.
// eye:      0 left, 1 right; selects the per-eye owned resources.
// bounds:   the Submit's uMin, vMin, uMax, vMax naming this eye's region of
//           srcTex, or null for the whole texture. Flipped spans name the
//           same pixels as their unflipped twins; the openvr half keeps the
//           direction for the outgoing bounds.
// strength: 0 to 1, 1 the sharpest -- AMD's RCAS at 2 * (1 - strength)
//           stops of sharpness reduction. 0 has nothing to do and answers
//           null; the openvr half never calls with it.
//
// Returns the sharpened texture (EDVR-owned, per eye, the source's own
// format family, region-sized, full-span content), or null: the pass
// refused or failed, its own log line says why, and the caller must
// forward what it had and stand the sharpening down. The source is never
// written to, and no reference to it outlives the call except a cached
// shader view over it, released the moment a different texture arrives
// for this eye.
__declspec(dllexport) void* edvrSharpen(void* srcTex, int eye,
                                        const float* bounds, float strength);
}
