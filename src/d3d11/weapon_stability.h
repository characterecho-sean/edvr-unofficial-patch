#pragma once
#include "panel_curve.h"
#include <cstdint>
struct ID3D11Resource;
namespace edvr {
class Config;
void weaponStabilityConfigure(Config&);
void weaponStabilityObserveScreen();
// nullptr invalidates all cached inputs (command-list execution); otherwise
// only writes to the source instance, bone or camera buffer invalidate them.
void weaponStabilityResourceWritten(ID3D11Resource*,uint64_t first=0,uint64_t end=~uint64_t(0));
// Source-image first-person attachments. Independent of temporal AA and
// runtime reprojection; returns true only when it issued the original draw.
bool weaponStabilityDraw(ID3D11DeviceContext*,PanelCurveDrawFn,unsigned count,unsigned instances,
                         unsigned start,int base,unsigned startInstance,unsigned width,unsigned height);
void weaponStabilityFrameBoundary(ID3D11DeviceContext*);
// Eye capture also retains timing BEFORE its readback hitch. GPU-only
// rolling history; one nonblocking readback when explicitly requested.
void weaponStabilityArmTrace();
void weaponStabilityShutdown();

constexpr unsigned kWeaponTraceBase=23,kWeaponTraceFrames=128,kWeaponTraceRows=16;

constexpr char kWeaponStabilityCs[]=R"HLSL(
struct Instance {uint4 row[21];};
struct Bone {float4 x,y,z;};
StructuredBuffer<Instance> Pool:register(t0);
StructuredBuffer<Bone> Bones:register(t1);
cbuffer Camera:register(b0){float4 camera[276];}
cbuffer Emitter:register(b1){float4 emitter[13];}
cbuffer LightCamera:register(b2){float4 lightCamera[14];}
cbuffer Sample:register(b3){uint sampleFrame,sampleEpoch,sampleWrite,sampleUnused;}
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
// Two eight-row samples after the three diagnostic rows. Every draw in
// a frame reads only the previous frame's bank: material passes and camera
// rewrites cannot train the ADS offset multiple times in one frame.
float3 aimingTranslation(float3 p,bool valid,out float status) {
    status=1;
    uint now=3+(sampleFrame&1)*8,prev=3+((sampleFrame-1)&1)*8;
    uint lagNow=19+(sampleFrame&1)*2,lagPrev=19+((sampleFrame-1)&1)*2;
    float3 r=float3(camera[270].x,camera[271].x,camera[272].x);
    float3 u=float3(camera[270].y,camera[271].y,camera[272].y);
    float3 f=float3(camera[270].w,camera[271].w,camera[272].w);
    float sx=length(r),sy=length(u);r/=max(sx,1e-8);u/=max(sy,1e-8);
    valid=valid && abs(camera[273].z-.025)<1e-7 && sx>0 && sy>0 &&
        all(isfinite(r)) && all(isfinite(u)) && all(isfinite(f)) &&
        abs(dot(f,f)-1)<.001 && abs(dot(r,u))<.001 &&
        abs(dot(r,f))<.001 && abs(dot(u,f))<.001 && dot(cross(r,u),f)>.999;
    float3 delta=camera[275].xyz-p;
    float3 local=float3(dot(delta,r),dot(delta,u),dot(delta,f));
    float3 learned=local,correction=0,armStep=0;float confidence=1,overshoots=0;
    float3 lagLocal=local,cameraStep=0;float lagConfidence=0,lagMode=0;
    bool history=valid && asuint(Anchor[prev].w)==sampleFrame-1 &&
        asuint(Anchor[prev+1].w)==sampleEpoch && Anchor[prev+2].w>0 &&
        abs(Anchor[prev+4].w-sx)<1e-5*max(sx,1) &&
        abs(Anchor[prev+5].w-sy)<1e-5*max(sy,1);
    uint traceFlags=(valid?1u:0u)|(history?2u:0u);
    if(history) {
        float3 dc=camera[275].xyz-Anchor[prev].xyz,dp=p-Anchor[prev+1].xyz;
        armStep=dp;cameraStep=dc;
        float lc=length(dc),lp=length(dp);
        // The first forward capture also contains a camera advance with a
        // repeated arms origin. Require the previous measured movement for
        // that case; a stationary aim/camera adjustment is not locomotion.
        float3 travel=lp>.001?dp:Anchor[prev+3].xyz;
        float3 residual=delta-(r*Anchor[prev+2].x+u*Anchor[prev+2].y+f*Anchor[prev+2].z);
        // 0.1 mm is the existing paired-root precision, not a motion filter.
        // Synchronized samples remain byte-identical to Elite, including bob,
        // recoil and crouching. A changed offset must establish itself anew.
        bool synchronized=all(abs(local-Anchor[prev+2].xyz)<.0001);
        bool steady=all(abs(r-Anchor[prev+4].xyz)<1e-5) &&
            all(abs(u-Anchor[prev+5].xyz)<1e-5) && all(abs(f-Anchor[prev+6].xyz)<1e-5);
        traceFlags|=(synchronized?4u:0u)|(steady?8u:0u)|
            (lc<1 && lp<1?16u:0u)|(lc>lp+.001?32u:0u)|
            (Anchor[prev+2].w>=3?64u:0u)|(Anchor[prev+3].w<2?128u:0u);
        // 07:44 prehistory: arms consistently use the PREVIOUS camera
        // position, with variable frame steps. Same-frame calibration never
        // settles. Require the measured one-frame correspondence and its
        // stable ADS offset; constant-speed motion alone is ambiguous.
        float3 lagDelta=Anchor[prev].xyz-p;
        lagLocal=float3(dot(lagDelta,r),dot(lagDelta,u),dot(lagDelta,f));
        if(steady && lc<1 && lp<1 && (lc>.001 || lp>.001)) {
            bool lagMatches=Anchor[lagPrev].w>0 &&
                all(abs(lagLocal-Anchor[lagPrev].xyz)<.0001) &&
                all(abs(dp-Anchor[lagPrev+1].xyz)<.0001);
            lagConfidence=lagMatches?min(Anchor[lagPrev].w+1,3):1;
            lagMode=lagMatches?Anchor[lagPrev+1].w:0;
            if(lagMatches && any(abs(dc-dp)>.0001))lagMode=min(lagMode+1,4);
        }
        // 08:53 prehistory switches from synchronized updates back to lag
        // after an isolated overshoot. The aiming offset is already known:
        // do not spend three more frames rediscovering that same offset.
        bool knownLag=steady && lc<1 && lp<1 && Anchor[prev+2].w>=3 &&
            all(abs(lagLocal-Anchor[prev+2].xyz)<.0001);
        if(knownLag && lp<=.001) {
            // At a stop, a camera-only change could also be an intentional
            // aiming adjustment. Retain calibration for the next measured
            // arms step, but leave this ambiguous frame's geometry alone.
            learned=Anchor[prev+2].xyz;confidence=3;
        }
        if(!synchronized && lp>.001 && ((lagConfidence>=3 && lagMode>=3) || knownLag)) {
            // Translate by this camera step, not by the whole camera/arms
            // offset. The verified aiming offset and original animation stay.
            correction=dc;learned=lagLocal;confidence=3;status=4;lagMode=4;
            traceFlags|=256u;
        } else if(lc<1 && lp<1 && synchronized) {
            confidence=min(Anchor[prev+2].w+1,3);status=confidence>=3?2:1;
        } else if(lc<1 && length(travel)>.001 && lc>lp+.001 && steady &&
            Anchor[prev+2].w>=3 && Anchor[prev+3].w<2 &&
            dot(dc,travel)>.998*lc*length(travel) && dot(residual,dc)>.998*length(residual)*lc &&
            length(residual)<=lc+.0001 && all(abs(Anchor[prev+7].xyz+dc-dp-residual)<.0001)) {
            // The camera advanced ahead of the independently updated arms.
            // Remove only that extra translation, retaining this weapon's
            // verified ADS offset. Never substitute a universal sight offset.
            correction=residual;learned=Anchor[prev+2].xyz;confidence=3;
            overshoots=Anchor[prev+3].w+1;status=3;
        }
    }
    if(sampleWrite!=0) {
        // First and last mesh samples expose same-frame replacement as well
        // as failed acquisition. These rows never feed the correction.
        uint first=23+(sampleFrame%128)*16,last=first+8;
        bool fresh=asuint(Anchor[first].x)!=sampleFrame || asuint(Anchor[first].y)!=sampleEpoch;
        float calls=fresh?1:Anchor[last].w+1;
        for(uint copy=0;copy<2;++copy)if(copy!=0 || fresh) {
            uint dst=copy!=0?last:first;
            Anchor[dst]=float4(asfloat(sampleFrame),asfloat(sampleEpoch),float(traceFlags),calls);
            Anchor[dst+1]=float4(camera[275].xyz,camera[273].z);
            Anchor[dst+2]=float4(p,confidence);
            Anchor[dst+3]=float4(r,sx);Anchor[dst+4]=float4(u,sy);
            Anchor[dst+5]=float4(f,status);Anchor[dst+6]=float4(correction,lagConfidence+4*lagMode);
            Anchor[dst+7]=float4(learned,Anchor[prev+2].w);
        }
        Anchor[now]=float4(camera[275].xyz,asfloat(sampleFrame));
        Anchor[now+1]=float4(p,asfloat(valid?sampleEpoch:0));
        Anchor[now+2]=float4(learned,valid?confidence:0);
        Anchor[now+3]=float4(armStep,overshoots);
        Anchor[now+4]=float4(r,sx);Anchor[now+5]=float4(u,sy);Anchor[now+6]=float4(f,0);
        Anchor[now+7]=float4(correction,0);
        Anchor[lagNow]=float4(lagLocal,lagConfidence);
        Anchor[lagNow+1]=float4(cameraStep,lagMode);
    }
    return correction;
}
[numthreads(64,1,1)]void findAnchor(uint lane:SV_GroupIndex) {
    uint n,stride,nb;Pool.GetDimensions(n,stride);Bones.GetDimensions(nb,stride);
    float best=1e30;uint index=0xffffffff;
    bool projection=camera[273].z>0 && camera[273].z<1 &&
        all(abs(float3(camera[270].z,camera[271].z,camera[272].z))<1e-8) &&
        all(abs(camera[273].xyw)<1e-6) && all(isfinite(camera[275]));
    // Hip fire attaches directly. ADS retains its deliberate camera offset
    // and corrects only a verified translation update disagreement.
    bool attachmentProjection=projection && abs(camera[273].z-.0675)<1e-7;
    bool aimingProjection=projection && abs(camera[273].z-.025)<1e-7;
    if(attachmentProjection || aimingProjection)for(uint i=lane;i<n;i+=64)if(root(i,nb)) {
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
        float status;
        float3 timing=aimingTranslation(p,valid && aimingProjection,status);
        bool apply=valid && (attachmentProjection || (aimingProjection && (status==3 || status==4) && any(timing!=0)));
        Anchor[0]=float4(apply?(attachmentProjection?camera[275].xyz-p:timing):0,apply?1:0);
        Anchor[1]=float4(p,valid?partners+1:0);
        Anchor[2]=float4(projection && !attachmentProjection?(aimingProjection?status:1):0,camera[273].z,0,0);
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
