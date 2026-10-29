#pragma once
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

// visual.desktop_window (docs\desktop-window.md): the runtime hands the graphics half each eye's FINISHED image, the texture it is about to give the headset,
// and the graphics half draws it into Elite's own desktop window at Elite's Present. Optional on both sides: a graphics half without the export, or a runtime
// that never calls it, leaves Elite's window exactly as it was.
//
// VERSIONS. 1: noteEye(context, eye, texture, bounds). 2 (round 6): noteEye also carries the field of view the image's pixels cover, so the graphics half can crop each eye to
// the shape the user chose (visual.desktop_window_aspect) around where that eye looks straight ahead. The runtime asks for version 2 and falls back to 1; the graphics half
// answers either, with the table's `version` saying which noteEye it holds (the same slot, two signatures).
#define EDVR_NATIVE_DESKTOP_WINDOW_VERSION_1 1u
#define EDVR_NATIVE_DESKTOP_WINDOW_VERSION_2 2u
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
// Version 2: the same, and the field of view the image's pixels cover as {angleLeft, angleRight, angleUp, angleDown} in radians (OpenXR's XrFovf order: left and down
// negative), or null when the image has none. The angles describe the bounds sub-rect.
typedef HRESULT(WINAPI *EdvrNativeDesktopWindowNoteEye2)(void*, uint32_t, ID3D11Texture2D*, const float*, const float*);
typedef HRESULT(WINAPI *EdvrNativeDesktopWindowClose)(void*);
struct EdvrNativeDesktopWindowTable {
  uint32_t size, version;
  void* context;
  EdvrNativeDesktopWindowNoteEye noteEye;   // version 1's signature; in a version 2 table this slot holds an EdvrNativeDesktopWindowNoteEye2
  EdvrNativeDesktopWindowClose close;
};
#ifdef __cplusplus
extern "C" {
#endif
HRESULT WINAPI edvrAcquireDesktopWindow(const EdvrNativeDesktopWindowRequest*, EdvrNativeDesktopWindowTable*);
#ifdef __cplusplus
}
#endif
