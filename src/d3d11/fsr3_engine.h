// FSR at the door -- AMD's FidelityFX Super Resolution 3.1, the community
// Direct3D 11 port (metarutaiga, hardened by OptiScaler; MIT), fed by the
// same plumbing dlaa.h wires NVIDIA's DLSS into: the temporal pass proved
// the inputs (docs\anti-aliasing.md), and fix.temporal_aa = fsr hands them
// to AMD's trained history instead, at the sizes dlss would use (docs\
// fsr-upscaler-design-2026-09-16.md, section 3).
//
// This file is the FFX glue and nothing else. Its bodies compile only when
// the build has AMD's port (EDVR_HAVE_FSR3, set by build.bat when the SDK
// fetched by tools\fetch_ffx_dx11.py is in third_party\ffx-dx11); without
// it, every entry answers "not built in" and the temporal pass runs its
// own history, exactly as an NGX refusal does today. Track B (this file,
// buildable and green with no SDK in the tree) versus Track C (the body
// under EDVR_HAVE_FSR3, and the link in build.bat): design doc, section 1.
#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace edvr {

// Is FSR usable on this device? Initialises the port on the first ask (once
// per session, whatever the answer) and says why not when it is not: the
// reason is a static string for the log. Cheap after the first call. In a
// build with no AMD SDK, always false with why = "this build was made
// without AMD's upscaler (EDVR_HAVE_FSR3)".
bool fsr3Available(ID3D11Device* dev, const char** why);

// The loading-screen warm-up (temporal_pass.cpp, warmTrainedOnce), FSR's
// side of what dlaaWarm does for NGX: initialises the port on ctx's device
// and makes the context FSR's next evaluate at this key (w x h in, outW x
// outH out) would need, so that call finds it made. createMs is the
// create's duration (zero when it already stood). False, with why, on the
// first refusal. Render thread only, like the rest.
// infiniteDepth is part of the context key; the default preserves the VR path.
bool fsr3Warm(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, uint32_t outW,
              uint32_t outH, double* createMs, const char** why, bool infiniteDepth = false);

// One eye, one frame: colour, depth and motion vectors in the same formats
// and sizes dlaaEvaluate takes (dlaa.h), a reactive mask (may be null: off
// unless advanced.temporal_aa_fsr_reactive = on, design doc 3.1/3.3), into
// output at outW x outH. reset breaks the history, exactly as dlaaEvaluate's
// does. frameMs is the time since this eye's previous evaluation. nearZ/
// farZ are the game's reversed-Z planes (temporal_pass.h's own nearZ/farZ,
// unchanged by this seam); fovY is the eye's vertical field of view in
// radians, computed at the call site from the eye's tangents (no ABI
// change needed: temporal_pass.cpp's temporalInner already has tanNow in
// scope where dlaaEvaluate is called, design doc 3.3's cameraFovAngleVertical
// = atan(t) + atan(b)). False on any refusal, with why.
//
// out MUST carry BOTH D3D11_BIND_UNORDERED_ACCESS and D3D11_BIND_SHADER_
// RESOURCE, even though FSR only ever writes it, and colour, depth, mv and
// reactive must carry D3D11_BIND_SHADER_RESOURCE. AMD's DX11 port's own
// RegisterResourceDX11 unconditionally creates a shader resource view for
// every texture it registers, output included, with no bind-flag check the
// way its UAV path has; a UAV-only texture makes that CreateShaderResource
// View call fail, and this port's answer to any failed D3D11 call mid-
// dispatch is an untyped `throw 1` (ffx_dx11.cpp's TIF helper) -- a bare
// C++ exception with no place in this otherwise all-FfxErrorCode C API.
// temporal_pass.cpp's own e.dlOut already carries both flags (it is DLAA's
// output too). Since the review of 2026-09-16 this is CHECKED here before
// anything is registered: a texture missing a flag is a plain false with a
// why naming the texture and the flag, not a throw to be caught -- the
// catch stays as the backstop for a failure nobody foresaw.
// infiniteDepth selects AMD's explicit infinite reversed-depth projection. nearZ
// remains the measured finite near plane; farZ is ignored in that mode.
bool fsr3Evaluate(ID3D11DeviceContext* ctx, unsigned eye, ID3D11Texture2D* colour,
                  ID3D11Texture2D* depth, ID3D11Texture2D* mv, ID3D11Texture2D* reactive,
                  ID3D11Texture2D* out, uint32_t w, uint32_t h, uint32_t outW,
                  uint32_t outH, float jx, float jy, bool reset, float frameMs,
                  float nearZ, float farZ, float fovY, const char** why, bool infiniteDepth = false);

// Releases FSR's per-eye contexts (g_ctx[2], design doc 3.2) without the
// full port shutdown below -- for a size or engine change mid-session, the
// same reason dlaa's ensureFeature recreates on a size change, but as an
// explicit call because FSR's context key includes the output size, which
// this build's own trim and HMD Quality can change live. Called from the
// render thread only (the pass calls it at the treat where it first sees the
// engine change), and it also clears any create-failure latch, so the next
// evaluate at a stood-down key is allowed to try again.
void fsr3ReleaseFeatures();

void fsr3Shutdown();

// Was AMD's port compiled into this build at all (EDVR_HAVE_FSR3)? A
// compile-time answer, with none of fsr3Available's cost: it initialises
// nothing, so a log line on NVIDIA's path may ask it without dragging AMD's
// backend and scratch into a session that never uses them. The one caller
// is the NGX refusal's "Set temporal_aa = fsr" hint, which must not be
// offered by a build that has no port to offer.
bool fsr3BuiltIn();

// "fsr 3.1.2" once the port is linked; the stub returns "fsr" -- what the
// price line and the perf tile print in place of "full-frame ngx"/"NVIDIA"
// when the engine that ran was AMD's (temporal_pass.cpp).
const char* fsr3VersionLabel();

// The measured price, mirroring dlaaTotals(dlaa.h): evaluations, the mean
// milliseconds by timestamp query, and how many evaluations carried the
// reset. False when nothing has run -- always false in a build with no SDK,
// since fsr3Evaluate's stub never runs.
bool fsr3Totals(uint32_t* evaluations, double* avgMs, double* maxMs, uint32_t* resets);

// advanced.temporal_aa_fsr_reactive (off|on, default off) and
// advanced.temporal_aa_fsr_debug (off|on, default off), both live: design
// doc 3.1. Read fresh from Config each call, like the pass's own small
// per-frame settings reads elsewhere in this codebase; called from the stub
// too (fsr3Available's #else branch), so the config contract's static scan
// of src\ sees both keys read regardless of which side of EDVR_HAVE_FSR3 a
// build compiles.
struct Fsr3Settings {
    bool reactive = false;   // hand FSR the UI-and-movers mask as `reactive`
    bool debug = false;      // FSR's own DRAW_DEBUG_VIEW
};
Fsr3Settings fsr3ReadConfig();

}  // namespace edvr
