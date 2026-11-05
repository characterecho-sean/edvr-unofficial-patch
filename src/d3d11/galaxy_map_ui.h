// The VR Galaxy Map's drawn UI (docs/galaxy-map-vr-2026-11-03.md, Fix 3 and "After the second flight"): the icons, labels, rings and stems fixed in the world, the mouse arrow at the stars' depth when its tip is not on a
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
//     gaps between panels close. While the arrow is on a panel, mask within reach on either side keeps it there (kMapProbeStickyPS), so it does not drop off a narrow strip of buttons.
//
// THE MAP CAMERA'S CONSTANTS. The icons', labels', rings' and stems' replacement shaders (C31238, 12382D, 94EA99, 3028E7) take the one pair of axes the world quads are spanned on (galaxy_map_axes.h), the map camera's position and the two eyes'
// offsets from a 96-byte constant buffer at vertex slot 10 (MapCam), written from mapCameraPose's stage 0 (rows, position) and mapCameraEyes (flat_stationary_camera_capture.h) when
// mapCameraChanges() has moved since the last write or the eyes' pair has changed. A draw is swapped only once the pose has answered; before that it is the game's, counted
// (Counters::noPose). Before the eyes' pair is whole the buffer says so (mapPos.w 0) and the shaders keep the game's eye. The buffer is bound at the first swap of each draw when
// slot 10 does not already hold it, and left bound after the map closes: no vertex shader of the game declares slot 10.
#pragma once

#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "galaxy_map_vs.h"   // MapBodies's rows, and the shell and float the arrow and the reticule share

namespace edvr {
namespace mapui {

constexpr uint64_t kVsIconA = 0xC31238331D8AC3D4ull, kVsIconB = 0x12382D2EA45E9632ull, kVsArrow = 0x4EF6DDB075A927FAull, kVsRing = 0x94EA991C3A03DBD9ull, kVsStem = 0x3028E7C6E0E097F9ull;
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
// Map is open in VR the layer gives each eye a D16_UNORM depth at the HDR layer's size and the sprites' colour issue tests it (no write); a second issue, with the HDR layer's render
// target kept at write mask 0 (the probe's blend), writes it where the sprite is solid (kMapDepthAlpha, galaxy_map_vs.h). The stencil write-back into the game's depth-stencil is not made
// for the map's sprites and canvases while the map is open in VR (ui_layer.cpp uiLayerWriteBackBegin), and nothing reads the bit; the write-backs were one of two alternations of the private D16 with the game's depth-stencil,
// the rest was restore() after every issue, which one bind for the run of sprites replaces (map_run_core.h, docs/galaxy-map-vr-2026-11-03.md "After the fifth flight"); any draw on the game's buffer inside a run would end it. The game's depth is reversed (clip.z 0.1, clip.w the distance, nearer larger:
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
// Stencil write-backs declined for the map's sprites and canvases while the map is open in VR (ui_layer.cpp uiLayerWriteBackBegin), since the last 30 s line.
inline std::atomic<uint64_t> g_writeBackDeclined{0};

// The run of sprites (map_run_core.h), since the last 30 s line. Inline for the same reason as the depth's: ui_layer.cpp counts them.
struct RunCounters {
    std::atomic<uint64_t> binds;        // real render target binds the run path made: an issue's bind of a run kind, and every end
    std::atomic<uint64_t> started;      // runs started
    std::atomic<uint64_t> ended;        // runs ended, by a bind or dropped
    std::atomic<uint64_t> setsKept;     // the game's own sets of the targets the run saved, inside a run (the real call not made)
    std::atomic<uint64_t> setsDropped;  // the game's sets of other targets inside a run (the run dropped)
    std::atomic<uint64_t> setsOwn;      // the game's sets of the layer's own targets inside a run, read back from the context (the real call not made)
};
inline RunCounters g_run;

enum Swapped : uint32_t { kSwapIconA, kSwapIconB, kSwapArrow, kSwapRing, kSwapStem, kSwapMarker, kSwapReticule, kSwapCount };

// Counters, since the last 30 s line (galaxy_map_pointer.cpp reads and clears them). Atomics: the line is said on the frame thread, the draws run on the render thread.
struct Counters {
    std::atomic<uint64_t> swapped[kSwapCount];
    std::atomic<uint64_t> noPose;           // an icon, label or ring draw left as the game made it because the map camera had never published a pose
    std::atomic<uint64_t> camWrites;        // writes of the map camera's constants into the slot-10 buffer
    std::atomic<uint64_t> eyeGenSteps;      // changes of the eyes' pair that the buffer's writes picked up (mapCameraEyes' generation)
    std::atomic<uint64_t> eyesValid;       // buffer writes that carried the two eyes' offsets (mapPos.w 1); the rest carried the game's eye only
    std::atomic<uint64_t> arrowKept;        // arrow draws left as the game made them because the tip was over a panel (or the probe could not say)
    std::atomic<uint64_t> maskIssued[2];    // panel draws issued into the mask, by shader (C4B4B3, 5DAD41)
    std::atomic<uint64_t> maskLate;         // a panel draw reaching the mask after the eye's probe of the same frame (the mask was not complete at the probe)
    std::atomic<uint64_t> probes;           // probe points drawn inside a query
    std::atomic<uint64_t> stickyProbes;     // of those, drawn with the arrow on a panel, so with the sticky edge
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
inline std::atomic<bool> g_swapOpen{false};   // any of these: the draw path's one load for swapBegin (the Galaxy Map, or the System Map's arrow and markers)
inline std::atomic<bool> g_over{false};
inline std::atomic<bool> g_systemOpen{false};   // the VR profile and the System Map's camera up: its flat plane in the UI layer, the cut-out, the markers' scale
inline std::atomic<uint64_t> g_bodyCutouts{0};  // the cut-outs made since the last 30 s line (ui_layer.cpp mapBodiesPunch)
inline bool vrOpen() noexcept { return g_vrOpen.load(std::memory_order_relaxed); }
inline bool pointerOpen() noexcept { return g_pointerOpen.load(std::memory_order_relaxed); }
inline bool swapOpen() noexcept { return g_swapOpen.load(std::memory_order_relaxed); }
inline bool systemOpen() noexcept { return g_systemOpen.load(std::memory_order_relaxed); }

// THE BODIES (docs/galaxy-map-vr-2026-11-03.md, "The System Map"). The game draws the atmosphere ring and the landable arc from a sprite whose world size does not follow the body in VR: 0.56 and 0.69 units on every body, against 1.14 and 1.42 in flat on planets of surface 0.886 and 0.79. No selector was found in
// the executable, so the sprites are scaled here to the measured fit: k = surface radius / kMarkerFitRadius, never below 1, which puts the ring at the atmosphere's outer radius (1.30 times the surface, which is flat's on 38 Lyncis 1) and leaves the moons, whose surface is 0.43, as they are.
// Each body's centre and surface radius come from the per-body constants at b1 (cb1[125].yzw, cb1[126].x), read at the Unmap of a constant buffer of that exact size, so a build with another size leaves the markers as the game drew them.
constexpr float kMarkerFitRadius = 0.4355f;
constexpr float kMarkerScaleMax = 4.0f;
constexpr uint32_t kBodyConstantBytes = 5376;
inline float markerScale(float surfaceRadius) {
    if (!(surfaceRadius > 0.0f)) return 1.0f;
    const float k = surfaceRadius / kMarkerFitRadius;
    return k < 1.0f ? 1.0f : (k > kMarkerScaleMax ? kMarkerScaleMax : k);
}
void bodyConstantsMapped(ID3D11Resource* res, const void* data);
void bodyConstantsUnmapped(ID3D11Resource* res);
// The table's size, and the map's opening: a new system's bodies start a new table. Frame tick.
uint32_t bodiesKnown();
void setSystemOpen(bool system);
// THE ARROW'S DEPTH on the System Map (kMapArrowDepthVS): just over what the arrow points at. Bodies there run from 80 units away to 2, so one fixed depth cannot do. The nearest body of all, the first cut, read 0.78 units in dump 103332590001 against 1.98 to the surface under the arrow: that body was behind the camera.
// rayBodyDistance: along the ray from the camera, the nearest body's shell the ray hits; failing that, the point of closest approach to the body it passes closest to in angle, which meets the hit's distance at the limb, so the depth has no step as the arrow leaves a body. A body behind the camera is nobody's. 0: none in front.
// The rows are MapBodies's (galaxy_map_vs.h kMapBodyRows).
inline float rayBodyDistance(const float origin[3], const float ray[3], const float (*rows)[4], uint32_t n) {
    float hit = 0.0f, passing = 0.0f, passingMiss = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        const float m[3] = {rows[i][0] - origin[0], rows[i][1] - origin[1], rows[i][2] - origin[2]};
        const float along = m[0] * ray[0] + m[1] * ray[1] + m[2] * ray[2];
        if (!(along > 0.0f)) continue;
        const float shell = rows[kMapRadiusRow + i / 4][i % 4] * kMapBodyShell;
        const float off2 = (std::max)(m[0] * m[0] + m[1] * m[1] + m[2] * m[2] - along * along, 0.0f);
        if (off2 < shell * shell) {
            const float t = (std::max)(along - std::sqrt(shell * shell - off2), kMapArrowMin);
            if (hit == 0.0f || t < hit) hit = t;
        } else {
            const float miss = (std::sqrt(off2) - shell) / along;
            if (passing == 0.0f || miss < passingMiss) { passing = along; passingMiss = miss; }
        }
    }
    return hit > 0.0f ? hit : passing;
}
// 1 / depth, the MapBodies row's x; a distance of 0 has no depth to give.
inline float arrowInverseDepth(float distance) { return distance > 0.0f ? 1.0f / (std::max)(distance, kMapArrowMin) + kMapArrowFloat : 0.0f; }
// The eased step toward it: a depth that jumps from one body to the next would pop in stereo. No easing from 0, the arrow's first depth. A quarter of the way a frame settles in about 16 frames at 90 fps.
constexpr float kArrowEase = 0.25f;
inline float arrowDepthStep(float current, float target) {
    if (current == 0.0f || target == 0.0f) return target;
    const float next = current + (target - current) * kArrowEase;
    return std::fabs(next - target) < 1e-4f * target ? target : next;
}
// The close view: a known body in the front of the view whose radius is at least this fraction of its distance, about 95 of the 2016 render pixels across. The far views measured stay below it (radius 0.44 at height 12.25, 0.036), the close views above (0.42 at 8.32, 0.05).
constexpr float kCloseViewRatio = 0.05f;
inline bool bodyFillsView(const float origin[3], const float forward[3], const float (*rows)[4], uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        const float m[3] = {rows[i][0] - origin[0], rows[i][1] - origin[1], rows[i][2] - origin[2]};
        const float distance = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        if (rows[kMapRadiusRow + i / 4][i % 4] >= kCloseViewRatio * distance && m[0] * forward[0] + m[1] * forward[1] + m[2] * forward[2] > 0.5f * distance) return true;
    }
    return false;
}
// Frame tick, System Map only. ray: the arrow's, in the map's axes (the camera's forward when the cursor is not known yet).
void updateArrowDepth(const float cameraPosition[3], const float ray[3], const float forward[3]);
float arrowDepth();   // the last distance, for the 30 s line; 0 when none
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

// THE MASK WITHOUT THE LAYER. With Anti-aliasing off, UI quality off or the layer stood down the layer takes no panel, so its second issues above never run and nothing would say the tip is on a panel. The hover
// does not need the layer, and the mask and the probe need only the game's own issue: after it, a panel draw goes once more into an A8_UNORM mask the size of the target the game drew it into (the game's viewport still
// fits), and the arrow's draw once more as the probe's point. Keyed by the target, so the two eyes need no name here. All state changed is put back. `draw` issues the game's draw as it was (panel) or as one point (arrow).
void directPanelIssue(ID3D11DeviceContext* ctx, uint64_t vs, const ProbeDraw& draw);
void directArrowProbe(ID3D11DeviceContext* ctx, const ProbeDraw& point);

// A panel draw reached the eye's mask (eye-frame seq): counted by shader, and noted late when the eye's probe of that frame already ran.
void noteMaskIssue(uint64_t vs, int eye, uint64_t seq);
// The mask or the query cannot be made: the arrow stays the game's (over = true), said once per cause.
void noteUnavailable(const char* what);

// The depth issue (see THE SPRITES' DEPTH), after the colour issue. `draw` issues the game's draw again as it was; this swaps in the pixel shader for `ps` (kPsSprite or kPsCanvas), calls it, and puts the
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
