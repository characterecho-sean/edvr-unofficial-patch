#pragma once
#include "panel_curve.h"
struct ID3D11Resource;
namespace edvr {
class Config;
void weaponStabilityConfigure(Config&);
void weaponStabilityObserveScreen();
// nullptr invalidates all cached inputs (command-list execution); otherwise
// only writes to the source instance, bone or camera buffer invalidate them.
void weaponStabilityResourceWritten(ID3D11Resource*);
// Source-image first-person attachments. Independent of temporal AA and
// runtime reprojection; returns true only when it issued the original draw.
bool weaponStabilityDraw(ID3D11DeviceContext*,PanelCurveDrawFn,unsigned count,unsigned instances,
                         unsigned start,int base,unsigned startInstance,unsigned width,unsigned height);
void weaponStabilityFrameBoundary(ID3D11DeviceContext*);
void weaponStabilityShutdown();

constexpr char kWeaponStabilityCs[]=R"HLSL(
struct Instance {uint4 row[21];};
struct Bone {float4 x,y,z;};
StructuredBuffer<Instance> Pool:register(t0);
StructuredBuffer<Bone> Bones:register(t1);
cbuffer Camera:register(b0){float4 camera[276];}
RWStructuredBuffer<Instance> Fixed:register(u0);
RWStructuredBuffer<float4> Anchor:register(u1);
groupshared float distances[64];
groupshared uint indices[64],partners;
float3 position(Instance r){return asfloat(r.row[1].xyz);}
bool root(uint i,uint nb) {
    Instance r=Pool[i];uint base=r.row[0].x;
    float3 p=position(r);float scale=asfloat(r.row[0].y);
    if(base==0 || base>=nb || !all(isfinite(p)) || !isfinite(scale) || scale<.5 || scale>2 ||
       dot(p-camera[275].xyz,p-camera[275].xyz)>4)return false;
    Bone b=Bones[base];
    // First-person arms have a shared eye-relative bind root: identity
    // orientation and negative eye height. The world-body roots in the
    // capture do not have this encoding. No fixed record/bone indices.
    return all(isfinite(b.x)) && all(isfinite(b.y)) && all(isfinite(b.z)) &&
        all(abs(b.x.xyz-float3(1,0,0))<.001) && all(abs(b.y.xyz-float3(0,1,0))<.001) &&
        all(abs(b.z.xyz-float3(0,0,1))<.001) && b.y.w<-.5 && b.y.w>-2.5 && abs(b.x.w)<.1 && abs(b.z.w)<.3;
}
[numthreads(64,1,1)]void findAnchor(uint lane:SV_GroupIndex) {
    uint n,stride,nb;Pool.GetDimensions(n,stride);Bones.GetDimensions(nb,stride);
    float best=1e30;uint index=0xffffffff;
    bool projection=camera[273].z>0 && camera[273].z<1 &&
        all(abs(float3(camera[270].z,camera[271].z,camera[272].z))<1e-8) &&
        all(abs(camera[273].xyw)<1e-6) && all(isfinite(camera[275]));
    if(projection)for(uint i=lane;i<n;i+=64)if(root(i,nb)) {
        float3 d=position(Pool[i])-camera[275].xyz;float dist=dot(d,d);
        if(dist<best){best=dist;index=i;}
    }
    distances[lane]=best;indices[lane]=index;if(lane==0)partners=0;
    GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint step=32;step;step>>=1) {
        if(lane<step && distances[lane+step]<distances[lane]){distances[lane]=distances[lane+step];indices[lane]=indices[lane+step];}
        GroupMemoryBarrierWithGroupSync();
    }
    index=indices[0];
    if(index!=0xffffffff)for(uint i=lane;i<n;i+=64)if(root(i,nb)) {
        float3 d=position(Pool[i])-position(Pool[index]);
        if(Pool[i].row[0].x!=Pool[index].row[0].x && dot(d,d)<1e-8 &&
           all(Pool[i].row[0].zw==Pool[index].row[0].zw))InterlockedAdd(partners,1);
    }
    GroupMemoryBarrierWithGroupSync();
    if(lane==0) {
        bool valid=index!=0xffffffff && partners>=1;
        float3 p=valid?position(Pool[index]):camera[275].xyz;
        Anchor[0]=float4(valid?camera[275].xyz-p:0,valid?1:0);
        Anchor[1]=float4(p,valid?partners+1:0);Anchor[2]=0;
    }
}
[numthreads(64,1,1)]void applyAnchor(uint id:SV_DispatchThreadID) {
    uint n,stride;Pool.GetDimensions(n,stride);if(id>=n)return;
    Instance r=Pool[id];float3 p=position(r),d=p-Anchor[1].xyz;float distance2=dot(d,d);
    // Rigid pieces near the attachment, and only skinned pieces sharing
    // that root. The player's world body remains at its original position.
    if(Anchor[0].w>0 && distance2<1 && (r.row[0].x==0 || distance2<1e-8))
        r.row[1].xyz=asuint(p+Anchor[0].xyz);
    Fixed[id]=r;
}
)HLSL";
}
