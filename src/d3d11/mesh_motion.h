#pragma once
#include <d3d11.h>
#include <cstdint>
#include "panel_curve.h"
namespace edvr {
struct NativeBenchmarkReport;
// Exact rigid mesh motion in either eye. Animated meshes retain their
// existing path; a pool index is never an object's persistent identity.
void meshMotionConfigure(bool on);

// Is this fix live? meshMotionDraw's own first reject, published so the draw
// path can decline without a call.
//
// The module keeps an admission census of every entry, including the ones it
// rejects as Disabled -- which is why the call could not simply be skipped
// without an answer to "who reads it". The answer is that the ONLY reader is
// meshMotionFrameBoundary's 1800-frame report (mesh_motion.cpp:669-673), and
// that whole function opens with `if(!enabled)return;` (:655), with the frame
// counter driving the report incremented inside it. While the feature is off
// the census is written and never read by anyone, so declining out here
// silences nothing observable -- and mesh motion is bundled with
// fix.temporal_aa (temporal_pass.cpp), which was off for the measurement.
// 165 innermost samples of the 1349-frame window of 2026-09-22.
//
// The regression rig drives meshMotionDraw directly and flips `enabled`
// itself, so it is unaffected by a guard at vscreen's call site.
namespace mesh_motion_detail { extern bool enabled, failed; extern unsigned pendingCount; }
inline bool meshMotionLive() {
    return mesh_motion_detail::enabled && !mesh_motion_detail::failed;
}

// meshMotionBeforeMap's own first test (mesh_motion.cpp): nothing captured is
// waiting to flush. Necessary, not sufficient -- the callee still compares
// the resource it was handed against the queued scene/ids/pool buffers, or
// against nothing at all for an unknown write (resource == nullptr).
// pendingCount mirrors pending.count, kept in sync at every site that
// changes it.
inline bool meshMotionAnyPending() {
    return mesh_motion_detail::pendingCount != 0;
}

void meshMotionDraw(ID3D11DeviceContext*,PanelCurveDrawFn,unsigned count,unsigned instances,unsigned start,int base,unsigned startInstance,uint64_t vs);
void meshMotionViews(ID3D11DeviceContext*,ID3D11Texture2D* scene,ID3D11ShaderResourceView** views);
void meshMotionFrameBoundary(ID3D11DeviceContext*);
void meshMotionShutdown();
// Submit deferred captures before Map makes a source unavailable to the GPU.
void meshMotionBeforeMap(ID3D11Resource*);
// Byte interval [first,end) for a known buffer write; omitted means all.
void meshMotionResourceWritten(ID3D11Resource*,uint64_t first=0,uint64_t end=~uint64_t(0));
// sceneFrame is the temporal eye-dump frame shared with companion files;
// omit only for callers that do not have that cross-file frame identity.
void meshMotionStageDump(ID3D11DeviceContext*,ID3D11Texture2D*,unsigned sceneFrame=~0u);
void meshMotionWriteDump(ID3D11DeviceContext*,const wchar_t* directory,const wchar_t* stamp);
// Explicit eye-run classification evidence: nominate accepted source buffers,
// then retain one later complete eye with its matching source generations.
void meshMotionArmClassification();
// The raw mesh-frame counter (mesh_motion_detail::frames). Resets to 0 on
// every config re-poll via meshMotionShutdown, so absolute values are only
// meaningful between configure events.
unsigned meshMotionFrameCount() noexcept;
void meshMotionStageClassification(ID3D11DeviceContext*,ID3D11Texture2D* scene,unsigned sceneFrame,ID3D11Texture2D* colour);

// Explicit one-run A/B/A diagnostic. Idle rendering uses normal packed capture
// for oversized instance streams. The menu action arms/cancels;
// the native benchmark owner supplies the exact 30-second sampling gate and its
// completed report so component counters cannot drift across benchmark windows.
void meshMotionRequestComparison();
uint64_t meshMotionComparisonScopeEpoch();
void meshMotionComparisonNativeSampling(bool active,uint64_t window,uint64_t scope);
void meshMotionComparisonNativeReport(const NativeBenchmarkReport&);
}
