// fix.ui_quality -- the engine-side panel sizing (docs/ui-layer-2026-09-23.md,
// "The engine's panel sizing"; docs/ui-sizing-owner-2026-09-23.md sections
// 3 and 5; the arithmetic and the build-332841 bytes in ui_sizing_math.h).
//
// Every Scaleform render-to-texture panel -- the cockpit's panels, the
// menus, station services, the menu's 16:9 screen -- is sized by the game as
// stage x W_ui x k / 1920 (/ 1080 for a view wider than 16:9), inlined in
// the panel init FUN_144570500 and the view-change recompute FUN_144571180.
// The four DIVSS operands of those divisions are pointed at two EDVR floats
// in a page within rel32 of the image, holding 1920 x f and 1080 x f with
// f = (W_ui x k) / (W_out x k_out) / T (clamped to [1/4, 1]), so the game
// itself makes, lays out, viewports and depth-partners every panel at its
// untrimmed size at HMD Quality = the key's target. The scene, the eye
// targets, the UI screen record and its 28 readers are not touched.
//
// SAFETY. Build-keyed: the PE stamp and image size, both sites' 30 bytes,
// the two functions' prologues and the three constants they read must be
// build 332841's exactly, or nothing is written and one line says which
// check failed. The operands are swapped once, the first time the key is on
// (all four or none, VirtualProtect and FlushInstructionCache as
// vscreen_res.cpp does); after that only the floats change -- aligned data
// stores -- and only when the factor's inputs settle on a new value, never
// per frame (the recompute runs twice per view change; a float changed
// between the two would part a viewport from its target). Key off: the
// floats go back to 1080 and 1920, the game's own values. Unload: the
// original operands are written back. The float page is never freed.
//
// THE CURSOR WINDOW. The one side reader of a panel's (w, h), FUN_14453EF80
// (panel vtbl +0x198), divides the panel's own size (+0x3B8/+0x3C0, which
// FUN_144571180 writes from these divisions) by the UI screen's (the
// renderer record's width and height, which this does not scale): a
// mouse-driven render-to-texture panel's cursor window grows by 1/f, up to
// the whole screen, and the cursor moves across the panel more slowly for
// the same mouse motion. Panels driven by focus (the cockpit's) are not
// affected.
#pragma once

#include <cstddef>

namespace edvr {

// From uiLayerConfigure: the key's target (0 off, 1.0, 1.25). The first time
// it is on, the sites are checked and the operands swapped (the floats at
// the game's own values until the factor's inputs are known).
void uiPanelScaleSetTarget(float target);

// Once a frame, on the render thread (uiLayerFrameBoundary): the factor from
// its inputs, written when they have settled on a new value.
void uiPanelScaleFrameBoundary();

// The engine is sizing the panels: the operands swapped, the key on and a
// factor written. The surfaces' CreateTexture2D matcher stands aside while
// it is (it would size a panel twice). Lock-free.
bool uiPanelScaleLive();

// The factor the floats hold (1 when not live).
double uiPanelScaleFactor();

// The operands written back (DLL unload). Idempotent.
void uiPanelScaleShutdown();

// The 30-second line (from ui_layer's totals).
void uiPanelScaleLog();

}  // namespace edvr
