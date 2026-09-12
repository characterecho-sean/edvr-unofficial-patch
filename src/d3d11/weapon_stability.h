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
cbuffer LightCamera:register(b2){float4 lightCamera[14];}
RWStructuredBuffer<Instance> Fixed:register(u0);
RWStructuredBuffer<float4> Anchor:register(u1);
RWStructuredBuffer<float4> EmitterFixed:register(u2);
RWByteAddressBuffer LightFixed:register(u3);
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
// The local particle variant has a camera-relative emitter transform.
// Hip fire uses near 0.0675; aiming uses 0.025, shared by world particles.
// Its particles do not read t33, so correcting the mesh pool alone
// separates them during crouching. Require an exact rigid-part match for
// the aiming projection and retain the same one-metre attachment volume
// used by applyAnchor. No colour/atlas rule.
groupshared uint emitterPartMatched;
[numthreads(64,1,1)]void applyEmitter(uint lane:SV_GroupIndex) {
    float3 origin=float3(emitter[9].w,emitter[10].w,emitter[11].w)+camera[275].xyz;
    float3 d=origin-Anchor[1].xyz;
    bool valid=Anchor[0].w>0 &&
        all(isfinite(origin)) && dot(d,d)<1 &&
        all(abs(float3(dot(emitter[9].xyz,emitter[9].xyz),dot(emitter[10].xyz,emitter[10].xyz),dot(emitter[11].xyz,emitter[11].xyz))-1)<.001) &&
        all(abs(float3(dot(emitter[9].xyz,emitter[10].xyz),dot(emitter[9].xyz,emitter[11].xyz),dot(emitter[10].xyz,emitter[11].xyz)))<.001) &&
        dot(cross(emitter[9].xyz,emitter[10].xyz),emitter[11].xyz)>.999;
    // Aiming uses the WORLD near plane (06:08:57), so projection alone
    // cannot identify attachments. Its emitter coincides with a corrected
    // rigid weapon-part origin in all 19 frames (within 2.2 micrometres).
    // Require that match for this projection; proximity alone is not enough.
    if(lane==0)emitterPartMatched=0;
    GroupMemoryBarrierWithGroupSync();
    if(valid && abs(camera[273].z-.025)<1e-7) {
        uint n,stride;Pool.GetDimensions(n,stride);
        for(uint i=lane;i<n;i+=64) {
            Instance part=Pool[i];float3 delta=position(part)-origin;
            if(part.row[0].x==0 && dot(delta,delta)<1e-8)InterlockedOr(emitterPartMatched,1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    valid=valid && (abs(camera[273].z-.0675)<1e-7 || emitterPartMatched!=0);
    if(lane<13) {
        uint i=lane;
        float4 r=emitter[i];if(valid && i>=9 && i<12)r.w+=Anchor[0][i-9];
        EmitterFixed[i]=r;
    }
}
// 05:21:41: the separate point light adds the upper pink fleck. Its
// placement is scaled about the OLD arms origin, so translating that
// origin requires the full mesh delta, even though its radius/projection
// use world units. Preserve the packed light payload and radius exactly.
[numthreads(64,1,1)]void applyLights(uint id:SV_DispatchThreadID) {
    uint bytes;LightFixed.GetDimensions(bytes);if(id>=bytes/32)return;
    float4 p=asfloat(LightFixed.Load4(id*32));
    float3 eye=float3(lightCamera[6].w,lightCamera[7].w,lightCamera[8].w);
    float3 d=p.xyz-Anchor[1].xyz;
    bool valid=Anchor[0].w>0 && all(isfinite(p)) && all(isfinite(eye)) &&
        all(abs(eye-camera[275].xyz)<1e-5) &&
        abs(lightCamera[12].w-.025)<1e-7 &&
        all(abs(float3(dot(lightCamera[2].xyz,lightCamera[2].xyz),dot(lightCamera[3].xyz,lightCamera[3].xyz),dot(lightCamera[4].xyz,lightCamera[4].xyz))-1)<.001) &&
        all(abs(float3(dot(lightCamera[2].xyz,lightCamera[3].xyz),dot(lightCamera[2].xyz,lightCamera[4].xyz),dot(lightCamera[3].xyz,lightCamera[4].xyz)))<.001) &&
        dot(cross(lightCamera[2].xyz,lightCamera[3].xyz),lightCamera[4].xyz)>.999 &&
        dot(d,d)<1 && p.w>0 && p.w<=.1;
    if(valid)LightFixed.Store3(id*32,asuint(p.xyz+Anchor[0].xyz));
}
)HLSL";
}
