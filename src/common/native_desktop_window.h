#pragma once
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

// visual.desktop_window (docs\desktop-window.md): the runtime hands the graphics half each eye's FINISHED image, the texture it is about to give the headset,
// and the graphics half draws it into Elite's own desktop window at Elite's Present. Optional on both sides: a graphics half without the export, or a runtime
// that never calls it, leaves Elite's window exactly as it was.
#define EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1 1u
struct EdvrNativeDesktopWindowRequest {
  uint32_t size, version;
  ID3D11Device* gameDevice;
  uint64_t generation;
};
// The eye (0 left, 1 right), the finished eye texture on the game's device and its bounds as {uMin, vMin, uMax, vMax} (UV; a min above its max is a flip, as
// submitted). S_OK: the provider took a reference and holds the texture until the next call for that eye or a close; S_FALSE: the setting is off, nothing held.
// The caller's reference is its own either way. Callable from the XR owner thread: the provider's state is guarded, and it never draws or releases on the
// calling thread (the render thread does both, at the game's Present).
typedef HRESULT(WINAPI *EdvrNativeDesktopWindowNoteEye)(void*, uint32_t, ID3D11Texture2D*, const float*);
typedef HRESULT(WINAPI *EdvrNativeDesktopWindowClose)(void*);
struct EdvrNativeDesktopWindowTable {
  uint32_t size, version;
  void* context;
  EdvrNativeDesktopWindowNoteEye noteEye;
  EdvrNativeDesktopWindowClose close;
};
#ifdef __cplusplus
extern "C" {
#endif
HRESULT WINAPI edvrAcquireDesktopWindow(const EdvrNativeDesktopWindowRequest*, EdvrNativeDesktopWindowTable*);
#ifdef __cplusplus
}
#endif
