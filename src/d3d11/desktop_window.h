// visual.desktop_window (docs\desktop-window.md): while Elite is in VR with 3D on, EDVR reshapes Elite's own desktop window to the shape of the eye image and fills it
// with the FINISHED eye image (after the upscaler, sharpening, the UI layer and EDVR's menu). Elite's own drawing into that window is skipped. When no eye image
// arrives any more (3D off) EDVR stops, puts the window back where Elite had it, and Elite's window takes over again.
//
// THE TWO HALVES. The runtime (src\openxr) hands each eye's finished texture over through edvrAcquireDesktopWindow's noteEye (src\common\native_desktop_window.h),
// on the XR owner thread, after the eye is captured. The graphics half keeps a reference to the latest texture of each eye the setting needs and uses it at Elite's
// Present on the render thread: desktopWindowPresent, called from hookedPresent before the real Present. Elite calls Submit left, Submit right, then Present, so at
// the Present the frame's images are ready.
//
// THE DRAW SKIP. While the feature draws, every Elite draw whose render target 0 is the swap chain's buffer 0 is skipped (vscreen.cpp, beginPanelOverride and the
// other draw thunks): the identity of that buffer is recorded at each Present, as a raw pointer used only for comparison (no reference is kept to the buffer, so
// Elite stays free to ResizeBuffers). The check on the draw path is one relaxed load and, only while the feature is drawing, a compare against the render target
// the binding shadow already resolved.
#pragma once

#include <atomic>
#include <cstdint>

struct IDXGISwapChain;
struct ID3D11Device;

namespace edvr {

// The identity of the swap chain's buffer 0 while the feature draws into it, or null. Written at each Present by desktopWindowPresent, read on every Elite draw.
extern std::atomic<void*> g_desktopWindowBack;
inline void* desktopWindowBackIdentity() { return g_desktopWindowBack.load(std::memory_order_relaxed); }
inline bool desktopWindowSkipping() { return desktopWindowBackIdentity() != nullptr; }
// A draw was skipped because its render target 0 was the buffer. Counts for the 10 s line.
void desktopWindowNoteSkippedDraw();

// Elite's Present, before the real call, on the render thread. flatProfile: the flat profile is active (the feature never runs there). Returns true when the feature
// was doing anything this Present (the caller marks the frame tick). Never throws; a fault stands the feature down for the session.
bool desktopWindowPresent(IDXGISwapChain* swapChain, ID3D11Device* device, unsigned presentFlags, bool flatProfile);

}  // namespace edvr
