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

constexpr char kWeaponIdentityCs[]=R"HLSL(
Buffer<uint> Index:register(t0);
struct Instance{uint4 row[21];};
StructuredBuffer<Instance> Pool:register(t1);
RWStructuredBuffer<uint4> Identity:register(u0);
[numthreads(1,1,1)]void main(){
    uint n,stride;Pool.GetDimensions(n,stride);uint id=Index[0];
    Identity[0]=id<n?uint4(Pool[id].row[0].x,Pool[id].row[1].w,1,0):0;
}
)HLSL";
constexpr char kWeaponMotionVs[]=R"HLSL(
Buffer<float4> Now:register(t0);
Buffer<float4> Before0:register(t1);
Buffer<float4> Before1:register(t2);
Buffer<float4> Before2:register(t3);
Buffer<float4> Before3:register(t4);
StructuredBuffer<uint4> Identity:register(t5);
StructuredBuffer<uint4> OldIdentity0:register(t6);
StructuredBuffer<uint4> OldIdentity1:register(t7);
StructuredBuffer<uint4> OldIdentity2:register(t8);
StructuredBuffer<uint4> OldIdentity3:register(t9);
cbuffer Settings:register(b0){float4 extent;}
struct O{float4 p:SV_Position;float4 old:TEXCOORD0;nointerpolation uint valid:TEXCOORD1;};
void candidate(float4 now,float4 old,uint4 identity,inout float4 selected,inout uint matches){
    // All measured packed families use an infinite reversed-Z projection:
    // clip Z is the projection near plane, invariant under camera/object
    // motion. Skeleton identity separates body/viewmodel even when aiming
    // makes their projections equal. Remaining duplicates are never guessed.
    if(Identity[0].z && identity.z && all(Identity[0].xy==identity.xy) && asuint(now.z)==asuint(old.z) && old.z>0 && all(isfinite(old))){selected=old;++matches;}
}
O main(uint id:SV_VertexID){
    O o;o.p=Now[id];o.old=0;uint matches=0;
    if(extent.z>0)candidate(o.p,Before0[id],OldIdentity0[0],o.old,matches);
    if(extent.z>1)candidate(o.p,Before1[id],OldIdentity1[0],o.old,matches);
    if(extent.z>2)candidate(o.p,Before2[id],OldIdentity2[0],o.old,matches);
    if(extent.z>3)candidate(o.p,Before3[id],OldIdentity3[0],o.old,matches);
    o.valid=matches==1;return o;
}
)HLSL";
constexpr char kWeaponMotionPs[]=R"HLSL(
cbuffer Settings:register(b0){float4 extent;}
float4 main(float4 pos:SV_Position,float4 old:TEXCOORD0,nointerpolation uint valid:TEXCOORD1):SV_Target {
    if(!valid || old.w<=0 || !all(isfinite(old)))return float4(0,0,pos.z,2);
    float2 previous=(old.xy/old.w*float2(.5,-.5)+.5)*extent.xy;
    if(any(previous<0) || any(previous>extent.xy))return float4(0,0,pos.z,2);
    return float4(previous-pos.xy,pos.z,1);
}
)HLSL";
}
