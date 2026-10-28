#include "fixed_shader_source.h"
#pragma once
#include <cstdint>
#include <string>
#include "panel_curve.h"
struct ID3D11ShaderResourceView;
namespace edvr {
class Config;
void screenMotionConfigure(Config&);

// Is this fix live? The first term of screenMotionSource, screenMotionUiDraw
// and screenMotionRecognize, published so the draw path can decline without a
// call -- each is invoked once per draw and, with fix.temporal_aa off (which
// is what arms this whole feature), each only ever returned immediately. 161
// innermost samples of the 1349-frame window of 2026-09-22 between the two
// draw entries. None of them touches anything before that first test.
//
// The flags live outside the module's State because State is reset wholesale
// in three places; screen_motion.cpp says what each reset does to them, and
// screen_motion_test asserts it.
namespace detail {
extern bool g_screenMotionEnabled;
extern bool g_screenMotionFailed;
}  // namespace detail
inline bool screenMotionLive() {
    return detail::g_screenMotionEnabled && !detail::g_screenMotionFailed;
}

void screenMotionSource(ID3D11DeviceContext*,unsigned width,unsigned height);
void screenMotionUiDraw(ID3D11DeviceContext*,PanelCurveDrawFn,unsigned count,unsigned instances,
                        unsigned start,int base,unsigned startInstance);
bool screenMotionRecognize();
void screenMotionDraw(ID3D11DeviceContext*,PanelCurveDrawFn,unsigned count,unsigned instances,
                      unsigned start,int base,unsigned startInstance,const float* curve=nullptr);
struct ScreenMotionGpuDiagnostics {
    uint64_t scope=0,sourceFrames=0;
    uint64_t uiClearCalls=0,uiDrawCalls=0,eyeClearCalls=0,projectionCalls=0;
    uint64_t uiClearSelected=0,uiDrawSelected=0,eyeClearSelected=0,projectionSelected=0;
    uint64_t uiClearSubmitted=0,uiDrawSubmitted=0,eyeClearSubmitted=0,projectionSubmitted=0;
    unsigned uiClearReady=0,uiDrawReady=0,eyeClearReady=0,projectionReady=0;
    unsigned uiClearSkipped=0,uiDrawSkipped=0,eyeClearSkipped=0,projectionSkipped=0;
    unsigned uiClearInvalid=0,uiDrawInvalid=0,eyeClearInvalid=0,projectionInvalid=0;
    bool collecting=false,draining=false;
};
// ctx is supplied by production's owner-thread frame boundary. The default
// keeps source-only fixtures able to advance history without a timing owner.
void screenMotionFrameBoundary(ID3D11DeviceContext* ctx=nullptr);
ScreenMotionGpuDiagnostics screenMotionGpuDiagnostics();
void screenMotionShutdown();
ID3D11ShaderResourceView* screenMotionView(int eye,unsigned width,unsigned height);

// The game's screen VS supplies exact perspective-correct source UVs,
// including the curved mesh and the live panel-distance transform.
// Recover scene motion inside that image, then place its previous UV on
// the previous screen. There is no headset approximation in either step.
//
// Engine-record motion on foot (docs/kinematic-motion-injection-2026-09-19.md,
// 2026-09-23 "On foot"): the shader is compiled with the engine-motion CORE in
// front of it (kEngineMotionCoreHlsl, the text the compose's enginePixel uses;
// screen_motion.cpp joins them). While engine.x says the source's views are
// bound, a source pixel whose MRT6 slot holds a certified rig record is
// carried by the record's own two pose blocks through the source pool draws'
// own scene constants (this frame's and last: SEN/SEB, the source rows) to
// its previous source UV; the panel mapping below takes it from there. A rig
// record EDVR cannot follow keeps no history (code 2); a pool surface that is
// not a rig record, a stale slot and a corrupt code keep the camera term.
// engine.y (the motion_source view): the validity carries 16 + that source
// kind instead, for the compose to paint through the panel. engine.w (F2 on foot): the source's target 7 is bound at t15, and a skinned record
// (a nonzero palette base) takes its exact motion from the texel (kind 7, painted and counted as joined) or keeps no history (2). engine.z: count
// the kinds into PanelCounts on the eye pixels of a grid of that stride -- 1,
// every pixel, with diagnostics or motion_source; kPanelSampleStride on the
// sampled frames otherwise (engine_velocity.h); 0, not counted.

// The screen shader's whole text: the engine-motion core (kEngineMotionCoreHlsl,
// generated from temporal_shader_source.h) in front of kScreenMotionPs. One
// function, so production and tools/engine_velocity_test compile the same.

}
