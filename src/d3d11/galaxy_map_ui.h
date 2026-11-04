// The VR Galaxy Map's drawn UI (docs/galaxy-map-vr-2026-11-03.md, Fix 3): the icons and labels facing the eye, the mouse arrow at the stars' depth when its tip is not on a
// panel, and the probe that says so.
//
// THE DESIGN. The panels (vertex shaders C4B4B334B26E81A9, B10B032BDFD46700, 5DAD41AA66757CE3) are drawn as the game made them, at about 1 m in the UI frame, and the
// arrow (4EF6DDB075A927FA) at the stars' depth (its vertex shader taking the eye origin out of its record, galaxy_map_vs.h) while its tip is off the panels; on a panel it
// is the game's own, at the panels' depth, so it neither clips through nor doubles over them in stereo. Whether the tip is on a panel is read back from the GPU:
//   * the panels' alpha. The panels are one full-screen shell mesh whose alpha comes from a mask texture, so no geometry test can tell where a panel is. Each panel draw the
//     UI layer takes while the map is open is issued once more into a per-eye A8_UNORM texture at the layer's size (ui_layer.cpp), with a blend that accumulates the pixel
//     shader's alpha (uiLayerMapMaskBegin).
//   * the probe. At each arrow-shader draw the layer takes, after its issue, one point is drawn at the arrow's tip in the game's own depth (galaxy_map_vs.h kMapProbeVS) with
//     a pixel shader that discards where the mask is clear, inside an occlusion query. A pixel counted means the tip is on a panel in that eye. The result is read at a later
//     probe, never waited for: over = over[0] || over[1], read by the arrow's swap decision. Before any answer over is false; if the mask or the query cannot be made it is
//     true (the game's arrow).
#pragma once

#include <d3d11.h>

#include <atomic>
#include <cstdint>

namespace edvr {
namespace mapui {

constexpr uint64_t kVsIconA = 0xC31238331D8AC3D4ull, kVsIconB = 0x12382D2EA45E9632ull, kVsArrow = 0x4EF6DDB075A927FAull;
constexpr uint64_t kVsPanelA = 0xC4B4B334B26E81A9ull, kVsPanelB = 0xB10B032BDFD46700ull, kVsPanelC = 0x5DAD41AA66757CE3ull;

inline bool isPanelVs(uint64_t vs) { return vs == kVsPanelA || vs == kVsPanelB || vs == kVsPanelC; }
inline int panelIndex(uint64_t vs) { return vs == kVsPanelA ? 0 : vs == kVsPanelB ? 1 : vs == kVsPanelC ? 2 : -1; }

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

enum Swapped : uint32_t { kSwapIconA, kSwapIconB, kSwapArrow, kSwapCount };

// Counters, since the last 30 s line (galaxy_map_pointer.cpp reads and clears them). Atomics: the line is said on the frame thread, the draws run on the render thread.
struct Counters {
    std::atomic<uint64_t> swapped[kSwapCount];
    std::atomic<uint64_t> arrowKept;        // arrow draws left as the game made them because the tip was over a panel (or the probe could not say)
    std::atomic<uint64_t> maskIssued[3];    // panel draws issued into the mask, by shader (C4B4B3, B10B03, 5DAD41)
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

// Set by mapPointerFrame (galaxy_map_pointer.cpp): the VR profile and the Galaxy Map open (the icons face the eye), and that with the pointer hooks armed (the arrow and the
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

// The gate states from the frame tick (a close lets the queries and the answers go at the next probe, and over is false).
void setGates(bool vrOpen, bool pointerOpen);

} // namespace mapui
} // namespace edvr
