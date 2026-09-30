#include "fixed_shader_source.h"
#pragma once
#include <cstddef>
#include <cstdint>
#include "panel_curve.h"
struct ID3D11VertexShader;
struct ID3D11Texture2D;
struct ID3D11Resource;
struct ID3D11ShaderResourceView;
namespace edvr {
// Store only the original, measured packed weapon shader families. Private
// data follows the shader's lifetime and is independent of eye diagnostics.
void weaponMotionRememberShader(ID3D11VertexShader*,uint64_t,const void*,size_t);
void weaponMotionConfigure(bool);
void weaponMotionSource(ID3D11Texture2D*);
// Call after the original opaque draw, with the pool it read still bound at VS SRV 33.
void weaponMotionDraw(ID3D11DeviceContext*,PanelCurveDrawFn,unsigned count,unsigned instances,
                       unsigned start,int base,unsigned startInstance);
// The host's per-draw gate, cheap enough for every DrawIndexedInstanced:
// on, the on-foot source depth seen this frame, and a weapon family VS.
bool weaponMotionWants(uint64_t vsHash);
// The weapon and tool shaders alone (no state): screen_motion's naming
// without terrain skips them, since their camera is the first person's.
bool weaponMotionFamilyVs(uint64_t vsHash);
ID3D11ShaderResourceView* weaponMotionView();
struct WeaponMotionGpuDiagnostics {
    uint64_t scope=0,sourceFrames=0,calls=0,selected=0,submitted=0;
    unsigned identifyReady=0,captureReady=0,rasterReady=0;
    unsigned identifySkipped=0,captureSkipped=0,rasterSkipped=0;
    unsigned identifyInvalid=0,captureInvalid=0,rasterInvalid=0;
    bool collecting=false,draining=false;
};
void weaponMotionFrameBoundary(ID3D11DeviceContext* ctx=nullptr);
WeaponMotionGpuDiagnostics weaponMotionGpuDiagnostics();
void weaponMotionResourceWritten(ID3D11Resource*);
void weaponMotionShutdown();




}
