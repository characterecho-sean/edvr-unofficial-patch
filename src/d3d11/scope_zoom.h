// The scope lens (docs\design-vr-scope-magnification-2026-10-28.md, section 8; the pure parts are scope_zoom_s1.h, the passes' text scope_zoom_shaders.h).
//
// The frame thread works the lens out each frame (native_on_foot.cpp: c, M and the gate) and publishes it here by seqlock. The door (ui_layer.cpp uiLayerComposite, right after the
// shield ripple) takes it for the right eye and, in the aiming eye's picture after the upscaler and RCAS, rewrites the hole of the scope's frame as the picture magnified by M about c:
// pass 1 marches 128 rays from c over the eye's depth to find the hole, pass 2 samples the picture at c + (p - c) / M inside it.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

#include "scope_zoom_b3.h"

namespace edvr {

// B3 (the scoped sniper's lens at full detail; scope_zoom_b3.h): what the frame thread tells the doors about the left eye's zoomed window.
struct ScopeZoomB3Frame {
    uint32_t asked = 0;      // 1: the full-detail key is on and the held entry is a rendered one, on a frame the lens qualifies for: the left door copies the zoomed picture's window and the lens reads it
    uint32_t matOk = 0;      // 1: T and u are made from the two eyes' rows of one frame, and the left rows describe the window the runtime told the game
    float tc[2] = {};        // the window's centre, in the left eye's tangent plane
    float T[16] = {};        // a point of the right eye's clip space, as a row (ndc x, ndc y, depth, 1), to the left eye's: b3Matrices
    float u[4] = {};
};

// What the frame thread publishes each frame the tick runs (every other field is meaningful for a kQualifies verdict only).
struct ScopeZoomFrame {
    uint32_t verdict = 0;            // scopezoom::LensVerdict
    uint32_t press = 0;
    float m = 1.0f;                  // M after the ease
    float mTarget = 1.0f;            // M the ease is going to
    float uv[2] = {};                // c as a uv of the render (0..1, y down), as the game drew it (the jitter in)
    float jitterUv[2] = {};          // the draw's jitter in the uv of the render's size (the game's content sits this far right and down of its unjittered place)
    uint64_t jitterSeq = 0;          // the temporal sequence of the frame that jitter, and the rows c came from, were drawn in (0: no jitter reading); the door's sequence is the picture's
    float tan4[4] = {};              // the tangents {L, R, D, U} the render's NDC square spans
    ScopeZoomB3Frame b3;             // B3 (meaningful for a kQualifies verdict only)
    uint64_t stampUs = 0;            // set by scopeZoomPublish
};

// The eye's scene depth the temporal pass found for the door's frame (temporal_pass.cpp sets it as it makes the depth view for the upscaler, and clears it at the top of the eye's
// treat). The view is the temporal pass's own, over the game's texture and valid until that pass's next treat; the door runs on the same thread straight after it.
struct ScopeZoomDepth {
    ID3D11ShaderResourceView* srv = nullptr;
    uint32_t w = 0, h = 0;           // the depth texture's size (the render's size)
    float nearDepth = 0.0f;          // the reverse-Z depth of scopezoom::kLensNearM from the scene's projection row
    bool covers = false;             // the Submit region is the whole source and the depth texture is the source's size: a uv of the render is a uv of the depth
};
inline ScopeZoomDepth g_scopeZoomDepth[2];
inline void scopeZoomDepthClear(int eye) noexcept {
    if (eye >= 0 && eye <= 1) g_scopeZoomDepth[eye] = ScopeZoomDepth{};
}
inline void scopeZoomDepthSet(int eye, ID3D11ShaderResourceView* srv, uint32_t w, uint32_t h, float nearDepth, bool covers) noexcept {
    if (eye < 0 || eye > 1) return;
    g_scopeZoomDepth[eye].srv = srv;
    g_scopeZoomDepth[eye].w = w;
    g_scopeZoomDepth[eye].h = h;
    g_scopeZoomDepth[eye].nearDepth = nearDepth;
    g_scopeZoomDepth[eye].covers = covers;
}

// The frame thread: publish this frame's lens (the verdict kIdle for a frame that asks nothing of it). The microsecond clock is the door's.
void scopeZoomPublish(const ScopeZoomFrame& frame);
// The door: true while a press is being counted or rays are in flight (so a log of presses is complete); a frame with the lens idle costs the door nothing else.
bool scopeZoomWanted();
// The scope's gunsight mark (ui_layer_math.h kScopeMark): true while the last published frame qualifies for the lens and is no older than the door takes a c, which is how the door
// reads it. Asked by the draw path for the mark's one pair, so the family is named only for a sight the lens magnifies.
bool scopeZoomMarkArmed();
// The layer took a mark draw out of the scene into an eye's weapons layer (0 left, 1 right) in the temporal sequence seq: counted for the press line, and the right eye's sequence is
// what the door compares its own against to count the armed frames that had no mark draw. path (scope_zoom_s1.h kMarkPath*) is what the layer did with the draw's depth and stencil tests.
void scopeZoomNoteMark(uint32_t eye, uint64_t seq, uint32_t path);
// An eye-frame whose weapons layer held a mark has been through the composite (uiLayerComposite): kind (scope_zoom_s1.h kMarkComposite*) is how the composite served it.
void scopeZoomNoteMarkComposite(uint32_t kind);
// The gunsight mark's route through the weapons layer stood down (ui_layer.cpp markStandDown): counted for the press line.
void scopeZoomNoteMarkStandDown();
// B3: the left eye's door, called with eye 0's picture after the upscaler and RCAS and before the left eye's UI composite. When the lens is to be rendered (the frame record says so) and the
// frame has the left eye drawn with the zoomed window, it copies the part of the picture the lens may read (scope_zoom_b3.h b3CopyRect) for the right eye's door a moment later.
struct ScopeZoomLeftArgs {
    uint64_t sequence = 0;
    ID3D11Texture2D* frame = nullptr;
    const uint32_t* region = nullptr;   // x0, y0, x1, y1 in the frame's pixels: the whole texture, or the copy is not made
    bool resolved = false;              // the frame is the upscaler's picture
};
void scopeZoomLeftDoor(const ScopeZoomLeftArgs& args);
// The doors ask the temporal pass whether eye 0 is zoomed in their frame through this pointer, set once by the frame tick (native_on_foot.cpp: scopeZoomSetTemporalZoom(&nativeTemporalZoom)),
// so the door links without the temporal pass (the rigs that link it stub nothing). Not set: eye 0 is never zoomed, and a frame asked for B3 falls back to the plain lens, counted.
using ScopeZoomTemporalFn = bool (*)(uint64_t sequence, float zoomed[4], float pre[4]);
void scopeZoomSetTemporalZoom(ScopeZoomTemporalFn fn);
// B3: the door's half of the press line's figures (the lens's source, the copies, the parallax at c); zero at scopeZoomPressBegin.
void scopeZoomB3Take(scopezoom::B3Press* out);
// A press begins (zero the door's counters) and ends (the counters, finished, for the press line).
void scopeZoomPressBegin(uint32_t press);
// The press the last scopeZoomPressBegin named (0 before any): the mark's stand-down lasts until it changes.
uint32_t scopeZoomPressId();
void scopeZoomPressTake(scopezoom::LensDoorStats* out);

struct ScopeZoomDoorArgs {
    uint32_t eye = 0;
    uint64_t sequence = 0;
    ID3D11Texture2D* frame = nullptr;
    const uint32_t* region = nullptr;   // x0, y0, x1, y1 in the frame's pixels
    bool inPlace = false;               // the frame is EDVR's own and is rewritten in full every frame: the passes may write it. The region must be the whole texture either way
    bool resolved = false;              // the frame is the upscaler's picture (unjittered)
};
// The door, called with the frame the weapons/HUD composite is about to take. Rewrites the right eye's picture in place, or, for a frame it may not write (or whose format has no
// UAV), makes its own copy of it, rewrites that and hands it back in *copy. Counts every frame it is called on under its verdict or its reason (scope_zoom_s1.h LensDoorStats).
void scopeZoomDoor(const ScopeZoomDoorArgs& args, Microsoft::WRL::ComPtr<ID3D11Texture2D>* copy);
// The device went, or the DLL is shutting down: every device child released.
void scopeZoomRelease();

}  // namespace edvr
