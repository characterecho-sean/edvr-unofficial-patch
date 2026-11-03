#pragma once
// visual.cockpit_glass = odyssey | legacy (docs\cockpit-glass-legacy-port.md). With `legacy`, the ship canopy is drawn by EDVR's pixel shader
// (cockpit_glass_shader.h) in place of the game's intact quality-3 canopy shader, and the glass takes the Horizons 3.8 look on live's bindings.
// With `odyssey` (the default) nothing is changed.
//
// The swap is at bind time. vscreen.cpp's PSSetShader hook asks cockpitGlassPsFor() for the shader to hand the real call; the binding shadow still
// records the game's own shader and hash, so the draw census and the glass probe see what the game bound. Off, the hook pays one load.
//
// Both profiles. Render thread, owner context only. The key is read at the frame boundary (live): cockpitGlassFrameBoundary().
// Log lines (each starts "cockpit glass:"): the key changing, the replacement made or refused, and every 30 s while `legacy` is on a window line
// with binds=N canopy-seen=M, so a key that is on but never took effect reads differently from one that worked.
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

// Once a frame from vScreenFrameBoundary: reads the key (live) and prints the 30 s window line while it is on.
void cockpitGlassFrameBoundary();

}  // namespace edvr
