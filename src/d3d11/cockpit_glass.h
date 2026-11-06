#pragma once
// visual.cockpit_glass = odyssey | legacy (docs\cockpit-glass-legacy-port.md). With `legacy`, the ship canopy is drawn by EDVR's pixel shader
// (cockpit_glass_shader.h) in place of the game's intact quality-3 canopy shader, and the glass takes the Horizons 3.8 look on live's bindings.
// With `odyssey` (the default) nothing is changed.
//
// The swap is at bind time. vscreen.cpp's PSSetShader hook asks cockpitGlassPsFor() for the shader to hand the real call; the binding shadow still
// records the game's own shader and hash, so the draw census and the glass probe see what the game bound. Off, the hook pays one load.
//
// visual.cockpit_glass_dirt (percent, 100 = the look as shipped) scales the legacy dirt. It reaches the shaders as a two-register constant buffer at b13 (the second register is the layer's
// pixel map, cockpit_glass_core.h LevelBlock), which the swap
// binds with the streak texture and puts back; it does nothing with `odyssey`, and the menus dim its row then (cockpit_glass_core.h dirtRowDim).
//
// Both profiles. Render thread, owner context only. The keys are read at the frame boundary (live): cockpitGlassFrameBoundary().
// Log lines (each starts "cockpit glass:"): the key changing, the replacement made or refused, and every 30 s while `legacy` is on a window line
// with binds=N canopy-seen=M ice-overlays=K, so a key that is on but never took effect reads differently from one that worked.
#include <d3d11.h>

#include <atomic>
#include <cstdint>

namespace edvr {

namespace detail {
extern std::atomic<bool> g_cockpitGlassLegacy;
}  // namespace detail

// One load. True while visual.cockpit_glass is legacy.
inline bool cockpitGlassLegacyLive() { return detail::g_cockpitGlassLegacy.load(std::memory_order_relaxed); }

// At a pixel-shader bind on the owner context, while cockpitGlassLegacyLive(): `ps` is what the game binds and `hash` its EDVR shader hash (0 when
// unknown). Returns the shader to bind: the replacement for the intact quality-3 canopy shader, else `ps`. The replacement is made the first time it is
// wanted; if that fails, `ps` comes back and the glass is drawn stock.
ID3D11PixelShader* cockpitGlassPsFor(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, uint64_t hash);
// The game's shader hash when `actual` is the replacement the swap above hands the real call, else 0. A check of the actual binding against what the game bound
// asks this: the swap is EDVR's own, so the binding is still the game's canopy.
uint64_t cockpitGlassReplacedHash(ID3D11PixelShader* actual);
// True between the replacement's bind (cockpitGlassPsFor handing it back) and the next bind of another pixel shader or the frame boundary: the bound canopy shader is EDVR's, with t12 and
// b13 ours. Owner context. The UI layer takes the canopy into the weapons layer (ui_layer_math.h kLegacyGlass) only while this holds, so a replacement that was refused leaves the game's own
// stock canopy draw in the scene.
bool cockpitGlassSwapActive();
// The weapons layer takes the canopy draw (flat): its SV_Position is in the layer's pixels, and the shader looks up its light cluster by the game's. Sets the block at b13 so that
// game = layer * (scaleX, scaleY) + (biasX, biasY) for the draw about to be issued (ui_holo_remap::params is the model: the target's size over the layer's, and the frame's jitter in the
// game's pixels). Uploads on a change only. False when no swap is in or there is no block: nothing is changed. cockpitGlassClearLayerMap puts the identity back after the draw (a no-op when
// no map is set); every scene bind of the block starts from the identity anyway.
bool cockpitGlassSetLayerMap(ID3D11DeviceContext* ctx, float scaleX, float scaleY, float biasX, float biasY);
void cockpitGlassClearLayerMap(ID3D11DeviceContext* ctx);
// Before each draw on the owner context, while cockpitGlassLegacyLive(): the swap binds t12 and b13 at the shader's bind, and something can write t12 after that and before the draw
// (flat, after a velocity change in normal space, every canopy draw until a reload: docs\cockpit-glass-legacy-port.md, "Clean glass after a supercruise exit"), which leaves the
// legacy shader with no dirt. While the swap's inputs are bound this puts them back, and keeps what it found for the hand-back. One test and out at any other draw.
void cockpitGlassBeforeDraw(ID3D11DeviceContext* ctx);

// The ice and condensation draws (docs\cockpit-glass-legacy-port.md section 11). The game draws the canopy with its own ice or condensation shader while they show, so the replacement
// is not bound and the legacy dirt would vanish. While `legacy`, the draw thunks (owner context) call cockpitGlassOverlayBegin() after the game's draw: when the bound pixel shader is the
// quality-3 ice or condensation one it binds EDVR's overlay pixel shader, a blend of colour ONE + ONE (alpha untouched) and the streak texture, and returns true; the thunk then issues
// the same draw again and calls cockpitGlassOverlayEnd(), which puts the game's pixel shader, blend state, blend factor, sample mask and t12 back. False means nothing was changed.
bool cockpitGlassOverlayBegin(ID3D11DeviceContext* ctx);
void cockpitGlassOverlayEnd(ID3D11DeviceContext* ctx);

// Once a frame from vScreenFrameBoundary (the owner context): reads the key (live), gives back t12 and prints the 30 s window line while it is on.
void cockpitGlassFrameBoundary(ID3D11DeviceContext* ctx);

}  // namespace edvr
