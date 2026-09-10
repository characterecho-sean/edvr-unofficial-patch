// UI coverage for temporal AA. The game's color/depth draw runs unchanged;
// supported HUD, hologram and interface composites are then reissued into
// a private scene-depth copy, optionally also marking the reactive mask.
//
// A copy seeded once per eye/frame preserves the scene's occlusion test.
// Only the temporal pass reads these added depths. Writing them into the
// game's live depth caused later smoke to have rectangular holes, confirmed
// by the 2026-09-10 UI-depth-off test (review-smoke-capture-1219.md).
// Unknown coverage shaders and unsupported targets are declined.
//
// Offscreen GUI draws teach the classifier which textures are UI surfaces.
// A composite sampling one, or a supported direct family, can contribute
// depth. Eyes are identified separately for each target size and format,
// because HDR and tonemapped composites use different pairs in one frame.
// Interface-projection composites convert their depth to scene encoding.
//
#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace edvr {

class Config;

// Reads fix.ui_depth (on | off), fix.temporal_aa (the gate),
// advanced.ui_depth_families, advanced.ui_depth_exclude and
// advanced.ui_depth_test. Install and reload; all live.
void uiDepthConfigure(Config& cfg);

// True while the key is on, the pass is on and nothing stood down: the
// draw path's one bool.
bool uiDepthWantsDraws();

// Every draw that did NOT land in an eye texture: learn the target as a UI
// surface when the bound vertex shader is one of the GUI renderer's
// families. The hash is asked only while a target is new, and a target
// checked to exhaustion is not asked again for a while.
void uiDepthNoteOffscreenDraw(ID3D11DeviceContext* ctx);

// Every eye draw: a UI composite (samples a learned surface in a
// pixel-stage slot 0..3) or a named direct family, with a depth target
// bound that is the scene pair's. True means the draw should write its
// coverage depth; the caller reissues it after the original draw.
bool uiDepthOnEyeDraw(ID3D11DeviceContext* ctx);

// Clear per-draw classification, including when another fix skipped the
// draw. The original draw's depth state is never changed by this module.
void uiDepthEnd(ID3D11DeviceContext* ctx);

// AFTER the real draw, for a composite drawn through the interface
// projection (the menus, the loading screen, the modals): the same
// geometry drawn once more with no colour target, EDVR's pixel shader
// clipping below the alpha floor, a private scene-depth copy for its eye,
// the nearer-wins test, and the viewport
// depth range converting the encoding. The caller issues the second draw
// between Begin and End when WantsReissue says so AND Begin returns true.
bool uiDepthWantsReissue();
// True when the depth-only draw is set up and the caller should issue it.
// False means this call declined and left the game's own state untouched,
// so issuing the draw anyway would be the game's composite a second time,
// in full colour, over itself.
bool uiDepthReissueBegin(ID3D11DeviceContext* ctx);
void uiDepthReissueEnd(ID3D11DeviceContext* ctx);

// Coverage also supports UI history decisions. It is marked at zero fixed
// bias and under native TAA. The temporal pass keeps per-eye raw UI colour
// and coverage, aligns it with UI motion, and rejects detected changes.
// Stable strokes can accumulate; new, erased or recoloured strokes favour
// the current frame. This is independent of the legacy fixed NVIDIA bias.
//
// The legacy fixed-bias texture: null at zero strength, with no marked
// coverage, or when the requested dimensions do not match.
bool uiDepthReactiveMask(uint32_t w, uint32_t h, int eye, ID3D11Texture2D** tex);

// Motion classification exists independently of NVIDIA reactivity. The
// R8 value's low two bits are 1 floating UI, 2 attached UI, 3 smoke;
// its upper six bits carry fixed bias. Classification remains at zero
// fixed bias and under native TAA. Smoke is not adaptive UI evidence.
bool uiDepthCoverageMask(uint32_t w, uint32_t h, int eye, ID3D11Texture2D** tex);

// The drives' smoke's own depth for one eye (fix.temporal_aa_smoke): the
// coverage pass writes it into a target of EDVR's, the scene depth's size,
// rather than the game's -- which cut out the game's later depth-tested
// draws behind the trail (the review of 2026-09-10) -- and the temporal
// pass folds it into the scene's depth as it reads. Null when nothing was
// drawn this frame or the size is not the scene depth's.
bool uiDepthSmokeDepth(uint32_t w, uint32_t h, int eye, ID3D11ShaderResourceView** srv);

// HUD and interface coverage in a private scene-depth copy. Seeded once
// per eye/frame to preserve scene occlusion, then merged by the temporal
// pass with the latest scene and smoke depth. The scene identity must still
// match the probe's selection. Returns a borrowed view, or null while off,
// before this frame's first coverage draw, or after an eye/target change.
bool uiDepthTemporalDepth(uint32_t w, uint32_t h, int eye, ID3D11Texture2D* scene,
                          ID3D11ShaderResourceView** srv);

// The strength the interface proper is marked at (advanced.ui_depth_reactive;
// 0 = no fixed NVIDIA bias; motion classification and adaptive history remain).
// HUD strokes use half this strength. Coverage shaders choose motion class
// separately: opaque HUD cores drawn at a surface can ride its motion.
float uiDepthReactive();

// Once per frame: the masks cleared, the engage line, the totals every 20 s.
void uiDepthFrameBoundary(ID3D11DeviceContext* ctx);

void uiDepthShutdown();

}  // namespace edvr
