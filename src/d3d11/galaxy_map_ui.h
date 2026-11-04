// The VR Galaxy Map's drawn UI (docs/galaxy-map-vr-2026-11-03.md, Fix 3 and "After the second flight"): the icons, labels and rings fixed in the world, the mouse arrow at the stars' depth when its tip is not on a
// panel, and the probe that says so.
//
// THE DESIGN. The panels (vertex shaders C4B4B334B26E81A9 and 5DAD41AA66757CE3) are drawn as the game made them, at about 1 m in the UI frame, and the
// arrow (4EF6DDB075A927FA) at the stars' depth (its vertex shader taking the eye origin out of its record, galaxy_map_vs.h) while its tip is off the panels; on a panel it
// is the game's own, at the panels' depth, so it neither clips through nor doubles over them in stereo. Whether the tip is on a panel is read back from the GPU:
//   * the panels' alpha. The panels are one full-screen shell mesh whose alpha comes from a mask texture, so no geometry test can tell where a panel is. Each panel draw the
//     UI layer takes while the map is open is issued once more into a per-eye A8_UNORM texture at the layer's size (ui_layer.cpp), with a blend that accumulates the pixel
//     shader's alpha (uiLayerMapMaskBegin).
//   * the probe. At each arrow-shader draw the layer takes, after its issue, one point is drawn at the arrow's tip in the game's own depth (galaxy_map_vs.h kMapProbeVS) with
//     a pixel shader that discards where the mask is clear, inside an occlusion query. A pixel counted means the tip is on a panel in that eye. The result is read at a later
//     probe, never waited for: over = over[0] || over[1], read by the arrow's swap decision. Before any answer over is false; if the mask or the query cannot be made it is
//     true (the game's arrow). "On a panel" is the mask under the tip, or under a point within reach on both opposite sides along one of four axes (kMapProbePS), so the
//     gaps between panels close.
//
// THE MAP CAMERA'S UP. The icons', labels' and rings' replacement shaders (C31238, 12382D, 94EA99) take a head-free up from a 16-byte constant buffer at vertex slot 10 (MapCam),
// written from mapCameraPose's stage 0 rows[1] when mapCameraChanges() has moved since the last write. A draw is swapped only once the pose has answered; before that it is the
// game's, counted (Counters::noPose). The buffer is bound at the first swap of each draw when slot 10 does not already hold it, and left bound after the map closes: no
// vertex shader of the game declares slot 10.
#pragma once

#include <d3d11.h>

#include <atomic>
#include <cstdint>

namespace edvr {
namespace mapui {

constexpr uint64_t kVsIconA = 0xC31238331D8AC3D4ull, kVsIconB = 0x12382D2EA45E9632ull, kVsArrow = 0x4EF6DDB075A927FAull, kVsRing = 0x94EA991C3A03DBD9ull;
// The panels, whose alpha is the mask: the shell meshes (C4B4B3) and the movie (5DAD41). B10B032BDFD46700 is not one. Its n=6 records sit in the UI frame at their own depth, and one is
// the hover halo round the hovered star (flight 12:35, dump 123832862001, record 1258: scale 11.8, W (-0.308, 0.956, 6.554), tan (-0.047, 0.146) against the arrow's tan (-0.042, 0.148)),
// so with it in the mask a hovered star put the halo under the tip and the arrow flipped between the stars' depth and the panels' about every frame, a shift of 0.027 tan (about 25
// pixels at the eye's half-size image) with the arrow's own record unmoved. It keeps the game's depth and is drawn as the game draws it (docs/galaxy-map-vr-2026-11-03.md, Part 1 of the
// 11-04 afternoon build).
constexpr uint64_t kVsPanelA = 0xC4B4B334B26E81A9ull, kVsPanelC = 0x5DAD41AA66757CE3ull, kVsHoverHalo = 0xB10B032BDFD46700ull;

inline bool isPanelVs(uint64_t vs) { return vs == kVsPanelA || vs == kVsPanelC; }
inline int panelIndex(uint64_t vs) { return vs == kVsPanelA ? 0 : vs == kVsPanelC ? 1 : -1; }

// The mask's blend: the pixel shader's alpha accumulates (a' = a + a_dst * (1 - a)), colour not written.
inline D3D11_BLEND_DESC maskBlendDesc() {
    D3D11_BLEND_DESC d{};
    D3D11_RENDER_TARGET_BLEND_DESC& rt = d.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_ZERO;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
    return d;
}

// The probe's blend: nothing is written to the layer.
inline D3D11_BLEND_DESC probeBlendDesc() {
    D3D11_BLEND_DESC d{};
    D3D11_RENDER_TARGET_BLEND_DESC& rt = d.RenderTarget[0];
    rt.BlendEnable = FALSE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_ZERO;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = 0;
    return d;
}

// THE SPRITES' DEPTH (docs/galaxy-map-vr-2026-11-03.md, "Sprite order"). The game draws the map's sprites with depth off, so a far sprite drawn late paints over a near one. While a Galaxy
// Map is open in VR the layer gives each eye a D16_UNORM depth at the HDR layer's size and the sprites' colour issue tests it (no write); a second issue, after the write-back that keeps
// the game's own stencil, writes it where the sprite is solid (kMapDepthAlpha, galaxy_map_vs.h). The game's depth is reversed (clip.z 0.1, clip.w the distance, nearer larger:
// fixed_shader_source.h, dlaa.cpp), so the test is GREATER_EQUAL, the clear 0, and the bias that pulls a written value farther is negative.
constexpr uint64_t kPsSprite = 0x964221955A9E77E7ull, kPsCanvas = 0x855C469156AB997Full;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D16_UNORM;
// Written depth is pulled four steps of D16 farther (the rasterizer's DepthBias), so a later quad in the same plane, an icon's glyph on its base at the same record position, tests
// equal-or-nearer against what the base wrote and is not lost to rounding.
constexpr int kDepthBias = -4;

// The colour issue's state (test, no write) and the depth issue's (test and write); stencil off in both.
inline D3D11_DEPTH_STENCIL_DESC depthStateDesc(bool write) {
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = TRUE;
    d.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    d.StencilEnable = FALSE;
    d.StencilReadMask = 0xFF;
    d.StencilWriteMask = 0xFF;
    d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
    d.BackFace = d.FrontFace;
    return d;
}
// The depth issue's rasterizer: the game's, with the bias.
inline D3D11_RASTERIZER_DESC depthRasterDesc(const D3D11_RASTERIZER_DESC& game) {
    D3D11_RASTERIZER_DESC d = game;
    d.DepthBias = kDepthBias;
    d.DepthBiasClamp = 0.0f;
    d.SlopeScaledDepthBias = 0.0f;
    return d;
}

// Counters of the depth, since the last 30 s line. Inline: ui_layer.cpp counts the colour issues and the creations, and is linked whole into rigs that do not link galaxy_map_ui.cpp.
struct DepthCounters {
    std::atomic<uint64_t> made;          // per-eye depth textures created
    std::atomic<uint64_t> unavailable;   // a depth, its state or its pixel shader that could not be made (the sprites drew as the game made them)
    std::atomic<uint64_t> tested;        // colour issues drawn with the depth bound and tested
    std::atomic<uint64_t> issued;        // depth issues made
    std::atomic<uint64_t> faults;        // a depth issue that faulted
};
inline DepthCounters g_depth;

enum Swapped : uint32_t { kSwapIconA, kSwapIconB, kSwapArrow, kSwapRing, kSwapCount };

// Counters, since the last 30 s line (galaxy_map_pointer.cpp reads and clears them). Atomics: the line is said on the frame thread, the draws run on the render thread.
struct Counters {
    std::atomic<uint64_t> swapped[kSwapCount];
    std::atomic<uint64_t> noPose;           // an icon, label or ring draw left as the game made it because the map camera had never published a pose
    std::atomic<uint64_t> camWrites;        // writes of the map camera's up into the slot-10 buffer
    std::atomic<uint64_t> arrowKept;        // arrow draws left as the game made them because the tip was over a panel (or the probe could not say)
    std::atomic<uint64_t> maskIssued[2];    // panel draws issued into the mask, by shader (C4B4B3, 5DAD41)
    std::atomic<uint64_t> maskLate;         // a panel draw reaching the mask after the eye's probe of the same frame (the mask was not complete at the probe)
    std::atomic<uint64_t> probes;           // probe points drawn inside a query
    std::atomic<uint64_t> answered;         // eye-frames whose queries came back
    std::atomic<uint64_t> framesOver, framesClear;   // of those, the tip on a panel / not
    std::atomic<uint64_t> ringFull;         // a probe not made because the ring held four unanswered eye-frames
    std::atomic<uint64_t> perFrameFull;     // a probe not made because the eye-frame already held four
    std::atomic<uint64_t> probeFaults;      // a probe issue that faulted
    std::atomic<uint64_t> maskUnavailable;  // a panel or arrow draw seen with no mask to write or read
};
extern Counters g_counters;

// Set by mapPointerFrame (galaxy_map_pointer.cpp): the VR profile and the Galaxy Map open (the icons, labels and rings are fixed in the world), and that with the pointer hooks armed (the arrow and the
// mask). One relaxed load each on the draw path.
inline std::atomic<bool> g_vrOpen{false};   // inline: ui_layer.cpp reads them and is linked whole into rigs that do not link galaxy_map_ui.cpp
inline std::atomic<bool> g_pointerOpen{false};
inline std::atomic<bool> g_over{false};
inline bool vrOpen() noexcept { return g_vrOpen.load(std::memory_order_relaxed); }
inline bool pointerOpen() noexcept { return g_pointerOpen.load(std::memory_order_relaxed); }
inline bool over() noexcept { return g_over.load(std::memory_order_relaxed); }

// The draw is one of the swapped shaders and the map state wants it: the replacement vertex shader goes in for this draw and every re-issue (raw, so the binding shadow keeps
// naming the game's), and end() puts the game's back. A failed creation leaves the game's shader, said once. End is a no-op when begin returned false.
bool swapBegin(ID3D11DeviceContext* ctx, uint64_t vs);
void swapEnd(ID3D11DeviceContext* ctx);

// The probe's issue. `draw` issues the game's draw as one point (same start index, base vertex and start instance); this binds the probe's shaders and the mask at t0, wraps the
// draw in a query, and puts every piece of state it changed back. eye and seq name the eye-frame; mask is that eye's mask SRV.
struct ProbeDraw {
    void (*draw)(void*) = nullptr;
    void* user = nullptr;
};
void probeIssue(ID3D11DeviceContext* ctx, int eye, uint64_t seq, ID3D11ShaderResourceView* mask, const ProbeDraw& pd);

// A panel draw reached the eye's mask (eye-frame seq): counted by shader, and noted late when the eye's probe of that frame already ran.
void noteMaskIssue(uint64_t vs, int eye, uint64_t seq);
// The mask or the query cannot be made: the arrow stays the game's (over = true), said once per cause.
void noteUnavailable(const char* what);

// The depth issue (see THE SPRITES' DEPTH). `draw` issues the game's draw again as it was; this swaps in the pixel shader for `ps` (kPsSprite or kPsCanvas), calls it, and puts the
// game's pixel shader back on every way out. The layer's bracket (ui_layer.cpp) has bound the depth, the depth state, the rasterizer and the viewports. False when the shader could not
// be made or the issue faulted.
struct DepthDraw {
    void (*draw)(void*) = nullptr;
    void* user = nullptr;
};
bool depthIssue(ID3D11DeviceContext* ctx, uint64_t ps, const DepthDraw& dd);

// The gate states from the frame tick (a close lets the queries and the answers go at the next probe, and over is false).
void setGates(bool vrOpen, bool pointerOpen);

} // namespace mapui
} // namespace edvr
