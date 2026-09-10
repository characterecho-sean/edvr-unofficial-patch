// Minimal device plumbing for the exposure fix.
//
// Three things are needed and nothing more: the compute shaders the game
// creates (so the exposure pass can be identified by its bytecode hash), the
// swapchain's Present (the frame boundary, and where the toggle hotkey is
// polled), and the DXGI factory (because the game creates its swapchain
// separately from its device, so there is nothing to hook until it does).
#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include "../common/vtable_hook.h"  // HookMode

namespace edvr {

// Which hook mechanism the context fixes should use, decided from the
// immediate context's vtable: does the runtime's own code back its methods
// (CopyVptr, immune to the runtime re-pointing its shared table between
// variants -- measured 2026-08-18) or does a wrapper like ReShade (InPlace,
// because swapping a wrapper's object vptr is issue #6)?
//
// Defined in d3d11_proxy.cpp, which owns the system module handle. Returns
// InPlace when the module or context is unavailable: the mode that never
// breaks a stranger is the safe default.
//
// DECIDED ONCE PER DEVICE, in hookDevice, and passed to BOTH context
// installers. It must not be computed twice: the probe samples live vtable
// entries and the runtime re-points a few of them between two calls (measured
// 96/96 one call, 93/96 the next in one session), so two independent
// decisions could straddle the threshold and land on OPPOSITE modes for the
// SAME context -- one swapping the vptr while the other patches the table it
// just orphaned. Sharing one answer removes the straddle entirely.
HookMode contextHookModeFor(ID3D11DeviceContext* ctx);

void hookDevice(ID3D11Device* device);
void hookSwapChain(IDXGISwapChain* swapChain);
void hookFactoryForDevice(ID3D11Device* device);
// The process is exiting cleanly, so the crash sentinel must not be left
// armed. Called from BOTH detach paths -- see the definition for why the
// FreeLibrary one alone was not enough, and why a real crash still trips.
void deviceHookNoteCleanExit();

// The render fraction advanced.texture_lod_bias = auto derived its bias
// from, read out of Elite's graphics preset before any sampler existed.
// False when auto was not used, or the preset gave nothing.
//
// Worth checking against the fraction the frame turns out to have. A mip
// bias is baked into every sampler at creation and cannot be changed
// after, so if the commander moves HMD Quality mid-session -- or if that
// multiplier ever stops meaning what it means today -- the mips are wrong
// for the rest of the session and nothing else would say so.
bool deviceHookAutoBiasSource(float* multiplier, float* bias);

// Current saved HMD Quality for menu labels, independent of mip overrides.
// Reads the graphics preset at most once a second per menu thread. False
// means unknown; callers should show a neutral DLSS/DLAA label.
bool deviceHookHmdQuality(float* multiplier);

// The FSS theater's mode latch: true while the player is (believed to
// be) in the Full System Scanner -- keyed by their own FSS bindings for
// frame-exact edges, reconciled against the game's GuiFocus. vscreen's
// frame boundary turns this into the theater's stamp.
bool deviceHookFssModeLatch();

// A zoom press was seen since the last take (stepped or held, keyboard
// or pad, only while the mode latch is open): the arrival window's
// earliest marker. Consumed on read.
bool deviceHookTakeFssZoomPress();

// The game's own resource creations since the last take (the monitor takes
// them once a frame for its long-frame line): counts, and the textures' and
// buffers' bytes about.
struct DeviceCreates {
    uint32_t textures = 0;
    uint32_t buffers = 0;
    uint32_t shaders = 0;
    uint64_t textureBytes = 0;
    uint64_t bufferBytes = 0;
};
DeviceCreates deviceCreatesTake();

void shutdownDeviceHooks();

}  // namespace edvr
