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

namespace edvr {

void hookDevice(ID3D11Device* device);
void hookSwapChain(IDXGISwapChain* swapChain);
void hookFactoryForDevice(ID3D11Device* device);
void shutdownDeviceHooks();

}  // namespace edvr
