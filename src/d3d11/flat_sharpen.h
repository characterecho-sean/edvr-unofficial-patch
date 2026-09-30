// Sharpening for the flat profile: fix.render_sharpness, the setting VR has, on the
// picture the flat resolve hands the game's output copy.
//
// WHERE. The flat runtime swaps the game's output copy's first pixel-shader input
// for the temporal resolve's result (flat_runtime.cpp, FlatRuntimeDrawScope). The
// game's interface is drawn after that copy, into the back buffer. So the one place
// that sharpens the picture and never the text is between the resolve and that swap:
// flatSharpenView takes the resolve's view and returns the view to hand the copy.
// It is VR's order too -- RCAS, then the UI layer (native_sharpen.cpp) -- and it is
// not a copy of anything: the pass is sharpen_pass.cpp's, reached through the same
// export (edvrSharpen), with the same shader and the same strength setting.
//
// WHAT IT NEVER DOES.
//  - Write the resolve's own output. The resolve reads its previous output back as
//    history (flat_mono_resolve.cpp), so a sharpened copy fed back would compound
//    every frame. The pass writes an EDVR-owned texture of its own.
//  - Sharpen at strength 0. The resolve's view comes straight back, the pass is not
//    called, and nothing is allocated: the frame is bit-identical to no sharpening.
//  - Sharpen a frame the resolve did not produce. A frame the temporal pass could not
//    resolve falls back to a plain spatial one (flat_runtime.cpp); that frame goes to
//    the game's copy as it is.
//  - Retry a pass that refused. One refusal stands the sharpening down for the session
//    (a new device starts it afresh), with a line saying so.
//
// The view it returns is over the sharpened texture in the resolve view's own format
// (sRGB or plain, as the resolve chose to match the game's), so the game's copy decodes
// it exactly as it would have decoded the resolve's. RCAS itself runs on the stored
// bytes, the space AMD wrote it for.
//
// Owner thread only, like the flat runtime that calls it.
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace edvr {

// The view the game's output copy should sample: `resolved` itself when there is
// nothing to do (setting 0, a view or context this cannot use, or the sharpening stood
// down), otherwise a view over the sharpened texture. The pointer is owned here and
// good until the next call; the flat runtime binds it for one draw and puts the
// game's own view back after.
ID3D11ShaderResourceView* flatSharpenView(ID3D11DeviceContext* ctx,
                                          ID3D11ShaderResourceView* resolved);

// What the wrapper has done, for the rig and for reading a log against.
struct FlatSharpenCounts {
    uint64_t sharpened = 0;        // frames handed a sharpened view
    uint64_t passedOff = 0;        // frames passed through at strength 0
    uint64_t passedStoodDown = 0;  // frames passed through after a refusal
    uint64_t refusals = 0;         // times the pass (or a view over its result) failed
    bool     stoodDown = false;
};
FlatSharpenCounts flatSharpenCounts();

// Forget the slots, the cached views and any stand-down: a new session. The wrapper
// does this itself when the resolve's device changes; the rig calls it between cases.
void flatSharpenReset();

}  // namespace edvr
