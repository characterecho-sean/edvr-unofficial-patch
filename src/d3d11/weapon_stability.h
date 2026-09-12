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
cbuffer Emitter:register(b1){float4 emitter[13];}
RWStructuredBuffer<Instance> Fixed:register(u0);
RWStructuredBuffer<float4> Anchor:register(u1);
RWStructuredBuffer<float4> EmitterFixed:register(u2);
groupshared float distances[64];
groupshared uint indices[64],partners;
float3 position(Instance r){return asfloat(r.row[1].xyz);}
bool root(uint i,uint nb) {
    Instance r=Pool[i];uint base=r.row[0].x;
    float3 p=position(r);float scale=asfloat(r.row[0].y);
    if(base==0 || base>=nb || !all(isfinite(p)) || !isfinite(scale) || scale<.5 || scale>2 ||
       dot(p-camera[275].xyz,p-camera[275].xyz)>4)return false;
    Bone b=Bones[base];
    // First-person arms share an eye-height bind root. Its orientation can
    // rotate during locomotion (18:52 capture), so require a proper rigid
    // basis rather than identity. The partner below must share this full
    // bind transform as well as the attachment position/orientation.
    return all(isfinite(b.x)) && all(isfinite(b.y)) && all(isfinite(b.z)) &&
        all(abs(float3(dot(b.x.xyz,b.x.xyz),dot(b.y.xyz,b.y.xyz),dot(b.z.xyz,b.z.xyz))-1)<.001) &&
        all(abs(float3(dot(b.x.xyz,b.y.xyz),dot(b.x.xyz,b.z.xyz),dot(b.y.xyz,b.z.xyz)))<.001) &&
        dot(cross(b.x.xyz,b.y.xyz),b.z.xyz)>.999 &&
        b.y.w<-.5 && b.y.w>-2.5 && abs(b.x.w)<.1 && abs(b.z.w)<.3;
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
        Bone a=Bones[Pool[i].row[0].x],b=Bones[Pool[index].row[0].x];
        if(Pool[i].row[0].x!=Pool[index].row[0].x && dot(d,d)<1e-8 &&
           all(Pool[i].row[0].zw==Pool[index].row[0].zw) &&
           all(abs(a.x-b.x)<.0001) && all(abs(a.y-b.y)<.0001) && all(abs(a.z-b.z)<.0001))InterlockedAdd(partners,1);
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
// The captured local particle variant uses the first-person projection
// (near 0.0675, versus 0.025 for world particles) and a camera-relative
// emitter transform. The purple rifle emitter is rigidly attached 75 mm
// from its mesh origin in all 38 captured frames. Its particles do not read
// t33, so correcting the mesh pool alone separates them during crouching.
// Deliberately decline other projections and emitters outside the same
// one-metre attachment volume used by applyAnchor. No colour/atlas rule.
[numthreads(1,1,1)]void applyEmitter() {
    float3 origin=float3(emitter[9].w,emitter[10].w,emitter[11].w)+camera[275].xyz;
    float3 d=origin-Anchor[1].xyz;
    bool valid=Anchor[0].w>0 && abs(camera[273].z-.0675)<1e-7 &&
        all(isfinite(origin)) && dot(d,d)<1 &&
        all(abs(float3(dot(emitter[9].xyz,emitter[9].xyz),dot(emitter[10].xyz,emitter[10].xyz),dot(emitter[11].xyz,emitter[11].xyz))-1)<.001) &&
        all(abs(float3(dot(emitter[9].xyz,emitter[10].xyz),dot(emitter[9].xyz,emitter[11].xyz),dot(emitter[10].xyz,emitter[11].xyz)))<.001) &&
        dot(cross(emitter[9].xyz,emitter[10].xyz),emitter[11].xyz)>.999;
    [unroll]for(uint i=0;i<13;++i) {
        float4 r=emitter[i];if(valid && i>=9 && i<12)r.w+=Anchor[0][i-9];
        EmitterFixed[i]=r;
    }
}
)HLSL";
}
