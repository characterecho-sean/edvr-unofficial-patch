#pragma once
#include <d3d11.h>
#include <cstdint>
#include "panel_curve.h"
namespace edvr {
struct NativeBenchmarkReport;
// Exact rigid mesh motion in either eye. Animated meshes retain their
// existing path; a pool index is never an object's persistent identity.
void meshMotionConfigure(bool on);
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

// Explicit one-run A/B/A diagnostic. Idle rendering uses normal packed capture
// for oversized instance streams. The menu action arms/cancels;
// the native benchmark owner supplies the exact 30-second sampling gate and its
// completed report so component counters cannot drift across benchmark windows.
void meshMotionRequestComparison();
uint64_t meshMotionComparisonScopeEpoch();
void meshMotionComparisonNativeSampling(bool active,uint64_t window,uint64_t scope);
void meshMotionComparisonNativeReport(const NativeBenchmarkReport&);
}
