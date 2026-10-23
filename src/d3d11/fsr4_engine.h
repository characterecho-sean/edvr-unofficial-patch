// FSR 4 at the door -- AMD's FidelityFX Super Resolution 4.1, reached through AMD's own signed D3D12 DLLs
// (amd_fidelityfx_loader_dx12.dll and amd_fidelityfx_upscaler_dx12.dll beside the game, staged by
// tools\fetch_fsr4.py) over a D3D11/D3D12 bridge of the kind xess_engine.cpp runs: a D3D12 device on the game's
// adapter, shared textures and a shared fence (docs\fsr-upscaler-design-2026-09-16.md, section 5 and the
// 2026-10-23 journal entry). fix.temporal_aa = fsr4 selects it; fsr stays route 1, the D3D11 port of FSR 3.1.
//
// fsr4 runs only when AMD's runtime offers a 4.x provider for the device (decision D7): on a GPU where it offers
// only FSR 3.1.5 it refuses, naming what it was offered, and never runs 3.1.5 in fsr4's name. Its bodies compile
// only when the build found the SDK (EDVR_HAVE_FSR4); without it every entry answers "not built in".
#pragma once

#include <cstdint>

#include "dlaa.h"   // kUpscalerSlots: slots 0 and 1 are the eyes', 2 the VR world route's; flat uses 0

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace edvr {

// Was the SDK compiled into this build (EDVR_HAVE_FSR4)? Compile-time, initialises nothing.
bool fsr4BuiltIn();

// Is FSR 4 usable on this device? The first ask (per D3D11 device) makes the D3D12 device on the same adapter,
// loads AMD's loader from beside EDVR's module, and asks it which upscaler versions it offers for that device;
// true only when one of them is 4.x. why is a static or session-lived string for the log. Cheap after the first
// call; a refusal is remembered with the device it ran on, and a different device is probed afresh.
bool fsr4Available(ID3D11Device* dev, const char** why);

// One slot, one frame, in fsr3Evaluate's terms (fsr3_engine.h): colour, depth and motion vectors at w x h
// (motion current -> previous in render pixels; jitter jx, jy in render pixels, positive right/down, as dlaa.h
// carries them), into out at outW x outH. reset breaks the history. frameMs is the time since this slot's previous
// evaluation (0 = unknown). nearZ/farZ are the reversed-Z planes (0 = unknown), fovY the vertical field of view in
// radians (0 = unknown). infiniteDepth and hdr have fsr3Evaluate's meaning: on the flat HDR route the colour is
// HDR scene radiance (R11G11B10_FLOAT), out is R16G16B16A16_FLOAT, and the context takes the HDR bit; otherwise
// colour and out are display-referred RGBA8 and declared non-linear sRGB to AMD. Both routes take AMD's automatic
// exposure (fsr4_version.h, fsr4CreateFlags).
// Both are part of the context key, as are the sizes: a change remakes the slot's context.
// The inputs are copied into the bridge's shared textures and out is written by a D3D11 copy, so no particular
// bind flags are needed on any of them. False on any refusal, with why.
bool fsr4Evaluate(ID3D11DeviceContext* ctx, unsigned slot, ID3D11Texture2D* colour,
                  ID3D11Texture2D* depth, ID3D11Texture2D* mv, ID3D11Texture2D* out,
                  uint32_t w, uint32_t h, uint32_t outW, uint32_t outH, float jx, float jy,
                  bool reset, float frameMs, float nearZ, float farZ, float fovY,
                  const char** why, bool infiniteDepth = false, bool hdr = false);

// Releases every slot's context (each after its bounded GPU wait) and clears the create-failure latches; the DLLs
// and the D3D12 device stay loaded. For a size, profile or engine change. Render thread only. Safe when FSR 4
// never started.
void fsr4ReleaseFeatures();

// Releases every context and the D3D12 device; AMD's loader stays loaded for the process, since its unload path
// has never run here. Safe when FSR 4 never started.
void fsr4Shutdown();

// "fsr 4.1.1" (the provider AMD's runtime reported for the last context made), or "fsr4" before one exists.
// What the price line and the perf tile print for this engine.
const char* fsr4VersionLabel();

// The slot's measured price, as xessSlotTotals gives XeSS's: evaluations, resets, and the mean and maximum CPU
// milliseconds per successful evaluate. False until the slot has evaluated, and in a build without the SDK.
bool fsr4SlotTotals(unsigned slot, uint32_t* evaluations, uint32_t* resets, double* avgMs, double* maxMs);

// Rig seams: contexts a slot has made since the backend started, and whether a slot holds one now.
uint32_t fsr4TestContextCreations(unsigned slot);
bool fsr4TestContextLive(unsigned slot);

}  // namespace edvr
