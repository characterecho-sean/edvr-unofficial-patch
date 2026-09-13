#pragma once
#include <d3d11.h>
#include <cstdint>
#include "panel_curve.h"
namespace edvr {
// Exact rigid mesh motion in either eye. Animated meshes retain their
// existing path; a pool index is never an object's persistent identity.
void meshMotionConfigure(bool on);
void meshMotionDraw(ID3D11DeviceContext*,PanelCurveDrawFn,unsigned count,unsigned instances,unsigned start,int base,unsigned startInstance,uint64_t vs);
void meshMotionViews(ID3D11DeviceContext*,ID3D11Texture2D* scene,ID3D11ShaderResourceView** views);
void meshMotionFrameBoundary(ID3D11DeviceContext*);
void meshMotionShutdown();
// Byte interval [first,end) for a known buffer write; omitted means all.
void meshMotionResourceWritten(ID3D11Resource*,uint64_t first=0,uint64_t end=~uint64_t(0));
void meshMotionStageDump(ID3D11DeviceContext*,ID3D11Texture2D*);
void meshMotionWriteDump(ID3D11DeviceContext*,const wchar_t* directory,const wchar_t* stamp);
}
