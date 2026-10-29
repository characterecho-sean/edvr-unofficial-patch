#pragma once
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

// visual.desktop_window (docs\desktop-window.md): the runtime hands the graphics half each eye's FINISHED image, the texture it is about to give the headset,
// and the graphics half draws it into Elite's own desktop window at Elite's Present. Optional on both sides: a graphics half without the export, or a runtime
// that never calls it, leaves Elite's window exactly as it was.
//
// VERSIONS. 1: noteEye(context, eye, texture, bounds). 2 (round 6, retired in round 8, never answered again): noteEye also carried the field of view the image's pixels cover.
// 3 (round 8): noteEye also carries whether the runtime has served the game a hidden-area mesh, which decides how the graphics half crops a chosen shape under
// visual.desktop_window_crop = native. The runtime asks for version 3 and falls back to 1 (no answer to carry: no mesh); the graphics half answers either, with the table's
// `version` saying which noteEye it holds (the same slot, two signatures). The two DLLs ship as a pair.
#define EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1 1u
#define EDVR_NATIVE_DESKTOP_WINDOW_VERSION_3 3u
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
// Version 3: the same, and whether the runtime has answered a hidden-area-mesh query from the game with a non-empty mesh (nonzero) or has not (zero: no mesh, or no query).
typedef HRESULT(WINAPI *EdvrNativeDesktopWindowNoteEye3)(void*, uint32_t, ID3D11Texture2D*, const float*, uint32_t);
typedef HRESULT(WINAPI *EdvrNativeDesktopWindowClose)(void*);
struct EdvrNativeDesktopWindowTable {
  uint32_t size, version;
  void* context;
  EdvrNativeDesktopWindowNoteEye noteEye;   // version 1's signature; in a version 3 table this slot holds an EdvrNativeDesktopWindowNoteEye3
  EdvrNativeDesktopWindowClose close;
};
#ifdef __cplusplus
extern "C" {
#endif
HRESULT WINAPI edvrAcquireDesktopWindow(const EdvrNativeDesktopWindowRequest*, EdvrNativeDesktopWindowTable*);
#ifdef __cplusplus
}
#endif
