// fix.ui_quality -- the supercruise bars in the HDR layer (docs/ui-layer-2026-09-23.md, "2026-10-07: orbit lines,
// supercruise bars and space dust in the layer").
//
// The faint vertical yellow bars of supercruise are one draw (vs A47A3315FFF5E2E4 / ps 869FFF43E875906E: supercruise_lines.h
// has the shader pair and the evidence for what they are): a LINELIST of 70 one-pixel segments into the lit HDR scene
// target, upscaled with the world, so they alias. The layer takes the draw (ui_layer_math.h's kSupercruiseBars) into the eye's
// HDR layer, where a pixel is 1 / 2.5 of a render pixel at 125% of a 2016 wide render -- and a one-pixel hardware line there
// would be 0.4 of the weight the game drew and still stair-stepped. So the issue into the layer is made through a PRIVATE
// GEOMETRY SHADER that gives each segment a strip with a tent alpha profile (supercruise_bars_shader.h), the same integrated
// weight as the one render pixel the game's line had, the game's own pixel shader unchanged.
//
// WHAT IS BOUND, for the layered issue only and put back right after it: the strip shader, its four-number constant buffer
// (GS slot 0: the layer's viewport, the tent's half-width, the cut plane), and a rasterizer state that culls nothing (the
// game's state was made for lines, which no cull mode touches; the strip is triangles). supercruise_bars_binding.h holds the
// classes, which tools/supercruise_bars_test runs on WARP. The game's geometry stage, its constant buffer slot and its
// rasterizer state go back exactly; a restore that fails is settled at the frame boundary.
//
// WHAT THE LOG SHOWS. Once, when the shader is made: "supercruise bars: precompiled geometry shader supercruise bars strip
// created ..." (shader_swap.cpp), or why it was not. Once, at the first draw through it: "supercruise bars: first draw ...".
// Every 30 s, beside the orbit lines' line: "ui quality: supercruise bars: ..." with the draws issued through the strip
// shader, what was asked and refused by reason, and the layer's half (how many the HDR layer took, how many it left in the
// scene and why). A session in which none of this ran has no first-draw line and says "0 in the HDR layer".
#pragma once

#include "supercruise_lines.h"

#include <cstdint>

struct ID3D11DeviceContext;

namespace edvr {

// ui_layer.cpp's decision, for a draw of the bars family that passed every other test: is the private pass available -- the
// strip shader made (once per device), its constants and both rasterizer states, no geometry shader of the game's bound, no
// binding waiting to be settled? False says why in *why (a constant string); the draw then stays in the scene and the module
// counts the refusal by reason, said once each.
bool supercruiseBarsReady(ID3D11DeviceContext* ctx, const char** why);

// vscreen.cpp, around the game's own issue of a bars draw the layer decided to take. Prepare (BEFORE the layer's Begin) reads
// the game's own viewport width, the render pixel the tent's width is measured in. Begin (AFTER the layer's Begin, with the
// layer's remapped viewport bound) binds the strip shader and answers true, End puts the game's state back; End is called only
// after a true Begin. A false Begin leaves the game's state as it was and the draw is issued into the layer as the plain
// lines the game drew (counted: it is the rare path of a state that changed between the decision and the issue).
void supercruiseBarsPrepare(ID3D11DeviceContext* ctx);
bool supercruiseBarsBegin(ID3D11DeviceContext* ctx, uint32_t vertices);
void supercruiseBarsEnd(ID3D11DeviceContext* ctx);

// Once a frame, render thread, after the layer's own boundary: settles a binding that did not restore.
void supercruiseBarsFrameBoundary(ID3D11DeviceContext* ctx);

// The 30 s line, from ui_layer's totals beside the orbit lines'. `layerText` is the layer's half of it (see
// orbitalWidthLog's).
void supercruiseBarsLog(const char* layerText);

// DLL unload: the shader, the constants, the states and the saved bindings released.
void supercruiseBarsShutdown();

}  // namespace edvr
