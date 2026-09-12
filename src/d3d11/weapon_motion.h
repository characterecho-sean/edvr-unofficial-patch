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
// Call after the original opaque draw, before restoring its corrected pool.
void weaponMotionDraw(ID3D11DeviceContext*,PanelCurveDrawFn,unsigned count,unsigned instances,
                       unsigned start,int base,unsigned startInstance);
ID3D11ShaderResourceView* weaponMotionView();
void weaponMotionFrameBoundary();
void weaponMotionResourceWritten(ID3D11Resource*);
void weaponMotionShutdown();

constexpr char kWeaponMotionVs[]=R"HLSL(
Buffer<float4> Now:register(t0);
Buffer<float4> Before:register(t1);
struct O{float4 p:SV_Position;float4 old:TEXCOORD0;};
O main(uint id:SV_VertexID){O o;o.p=Now[id];o.old=Before[id];return o;}
)HLSL";
constexpr char kWeaponMotionPs[]=R"HLSL(
cbuffer Settings:register(b0){float4 extent;}
float4 main(float4 pos:SV_Position,float4 old:TEXCOORD0):SV_Target {
    if(extent.z==0 || old.w<=0 || !all(isfinite(old)))return float4(0,0,pos.z,2);
    float2 previous=(old.xy/old.w*float2(.5,-.5)+.5)*extent.xy;
    if(any(previous<0) || any(previous>extent.xy))return float4(0,0,pos.z,2);
    return float4(previous-pos.xy,pos.z,1);
}
)HLSL";
}
