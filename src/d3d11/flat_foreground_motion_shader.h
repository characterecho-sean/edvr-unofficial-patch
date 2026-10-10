#pragma once
#include <array>
#include <cstddef>
namespace edvr {
// IDENTITY BY CONTENT (design doc section 104; docs\per-object-motion.md, "The slot is not an identity, even at rest"). A draw takes the
// history of the previous frame's draw of the same geometry whose pool record has the same identity: identity.x (the record's first word,
// the bone base) and identity.y (the word at byte 28, the record's signature) with byte 30 left out. Byte 30 is a per-instance parameter
// that changes on 10 to 40 percent of rewritten records; bytes 28, 29 and 31 never change. The pool SLOT is not compared: Elite re-orders
// the pool every live frame (160 of 279 records at a new slot with the ship still), so requiring the old slot refused the whole weapon
// on four frames in five. The VR matcher (kWeaponMotionVs) has always compared identity alone.
//
// WHY A DRAW HAS NO USABLE HISTORY. A rejected draw says why in the x channel of its map samples (x is motion only where the sample is
// valid, and the prep reads it only there); the prep keeps the code in bits 4-6 of the class byte of a refused first-person pixel
// (flat_mono_refusal.h, kFlatMonoWeaponReason*, which tools\flat_mono_resolve_test holds to these numbers) and the refusal census prints
// the count of each:
//  1 the current draw is not valid: a vertex that is not finite, has no positive clip z or has another z than the rest of its triangle,
//    or a slot out of range
//  2 not authentic: the instance index carries flag bits, or the pool has no row for it
//  3 no prior was supplied: the previous frame has no draw of this geometry under this pool and near
//  4 the prior that matched has vertices that are not finite or are behind the camera
//  5 priors were supplied and read, and none has this identity
//  6 priors were supplied and none could be read: their pool rows were out of range, an invalid identity the frame before
//  7 several priors matched and their previous positions or phases differ (equal-or-reject: no pose is chosen, and no slot breaks the tie)
// 0 is "no reason given": no sample at the pixel, or a valid sample the prep refused (a reset frame, a previous position off the raster).
//
// A DRAW OF AN OBJECT THAT HAS OTHER PIECES (design doc section 104, the pistol's LOD swap). Reasons 3, 5 and 6 are a draw whose own history
// is missing: the mesh was swapped for another level of detail, a piece reappeared after some frames, a key was never seen. The object it
// belongs to has not stopped moving, and the other pieces of that object, drawn this frame under the same pool identity, did match their
// history. When a sibling pass has run (Settings.sibling.x), such a draw takes the motion of its siblings, if they agree:
//  - the donors are this frame's draws with a matched prior and the same identity (x and y with byte 30 left out), vertex by vertex, in
//    pixels, as the map would have written them (kFlatForegroundDonorCs: one thread group a draw, no atomics);
//  - their mean is taken only where their motion is uniform to within a pixel (the spread of their vertices, per axis, is at most
//    kFlatSiblingSpreadPixels, the pass's constant): a held object swung through the view, whose pieces move differently, is refused
//    as it was (kFlatForegroundFitCs, mode 3);
//  - with no donor at all the piece is the view's own: a first-person draw is attached to the view, so its motion against the previous
//    frame is none, and its history is kept (mode 2);
//  - what stays refused is what nothing justifies a motion for: a current draw that is not valid (1), a pool row that is not authentic (2),
//    prior positions that cannot be read or are ambiguous (4, 7), and an identity the pool cannot read (mode 4).
// The fit table is two float4 a draw: (x, y, mode, spread) and (donor draws, donor vertices, matched, 0). Modes: 0 the draw matched its own
// history and needs none, 1 siblings' mean, 2 view-attached, 3 siblings disagree, 4 identity unreadable.
inline constexpr char kFlatForegroundMatchHlsl[]=R"HLSL(
Buffer<float4> Now:register(t0);
Buffer<float4> Before0:register(t1);
Buffer<float4> Before1:register(t2);
Buffer<float4> Before2:register(t3);
Buffer<float4> Before3:register(t4);
StructuredBuffer<uint4> Identity:register(t5);
StructuredBuffer<uint4> PreviousIdentity0:register(t6);
StructuredBuffer<uint4> PreviousIdentity1:register(t7);
StructuredBuffer<uint4> PreviousIdentity2:register(t8);
StructuredBuffer<uint4> PreviousIdentity3:register(t9);
Buffer<uint> InstanceIndex:register(t10);
cbuffer Settings:register(b0){float4 extentPhase;float4 previousPhaseDepth;uint4 expected;uint4 provenance;
 float4 priorPhase[4];uint4 identityMode;uint4 sibling;}
static const uint kReasonNone=0,kReasonInvalidCurrent=1,kReasonNotAuthentic=2,kReasonNoPrior=3,kReasonPriorPositions=4,
 kReasonIdentityDiffers=5,kReasonPriorIdentityInvalid=6,kReasonAmbiguous=7;
// The bits of identity.y that name the record: byte 30 (bits 16-23 of the word at byte 28) is a per-instance parameter and is not part of it.
static const uint kIdentityParameterMask=0xFF00FFFFu;
bool validPosition(float4 p){return all(isfinite(p)) && p.z>0;}
bool samePosition(float4 a,float4 b){return all(asuint(a)==asuint(b));}
bool sameIdentity(uint4 prior,uint4 current){return prior.x==current.x && ((prior.y^current.y)&kIdentityParameterMask)==0;}
// Which of the draw's priors (bit i for prior i) have its identity, and how many of them could be read at all.
uint matchedPriors(uint4 identity,out uint readable){
 uint matches=0;readable=0;
 if(identityMode.y>0 && PreviousIdentity0[0].z!=0){++readable;if(sameIdentity(PreviousIdentity0[0],identity))matches|=1;}
 if(identityMode.y>1 && PreviousIdentity1[0].z!=0){++readable;if(sameIdentity(PreviousIdentity1[0],identity))matches|=2;}
 if(identityMode.y>2 && PreviousIdentity2[0].z!=0){++readable;if(sameIdentity(PreviousIdentity2[0],identity))matches|=4;}
 if(identityMode.y>3 && PreviousIdentity3[0].z!=0){++readable;if(sameIdentity(PreviousIdentity3[0],identity))matches|=8;}
 return matches;
}
struct Resolved{uint valid;uint reason;uint slot;float4 old;float2 oldPhase;};
// The map's decision for one vertex of a draw in GPU identity mode: the vertex shader's, and the donor pass reads the same one.
Resolved resolveGpuIdentity(uint id,bool currentValid){
 Resolved r;r.valid=0;r.reason=kReasonNone;r.old=0;r.oldPhase=previousPhaseDepth.xy;
 uint first=id-id%3;
 uint raw=InstanceIndex[0],slot=raw&0x007fffff;
 r.slot=slot;
 if(!currentValid || slot>=0x007fffff){r.valid=0;r.reason=kReasonInvalidCurrent;return r;}
 uint4 identity=Identity[0];
 if(raw!=slot || identity.z==0){r.valid=2;r.reason=kReasonNotAuthentic;return r;}
 uint readable;uint matches=matchedPriors(identity,readable);
 if(matches==0){
  r.valid=2;r.reason=identityMode.y==0?kReasonNoPrior:readable==0?kReasonPriorIdentityInvalid:kReasonIdentityDiffers;
  return r;
 }
 uint chosen=(matches&1)?0:(matches&2)?1:(matches&4)?2:3;
 float4 selected[3];
 [unroll]for(uint i=0;i<3;++i){
  if(chosen==0)selected[i]=Before0[first+i];
  else if(chosen==1)selected[i]=Before1[first+i];
  else if(chosen==2)selected[i]=Before2[first+i];
  else selected[i]=Before3[first+i];
 }
 bool usable=true;
 [unroll]for(uint i=0;i<3;++i)usable=usable && validPosition(selected[i]);
 bool equivalent=usable;
 [unroll]for(uint c=0;c<4;++c)if((matches&(1u<<c))!=0 && c!=chosen){
  equivalent=equivalent && all(asuint(priorPhase[c].xy)==asuint(priorPhase[chosen].xy));
  [unroll]for(uint i=0;i<3;++i){
   float4 other=(c==0)?Before0[first+i]:(c==1)?Before1[first+i]:(c==2)?Before2[first+i]:Before3[first+i];
   equivalent=equivalent && samePosition(selected[i],other);
  }
 }
 r.old=selected[id%3];r.oldPhase=priorPhase[chosen].xy;
 r.valid=equivalent?1:2;
 r.reason=equivalent?kReasonNone:usable?kReasonAmbiguous:kReasonPriorPositions;
 return r;
}
)HLSL";
inline constexpr char kFlatForegroundMotionVsBody[]=R"HLSL(
StructuredBuffer<float4> Fit:register(t11);
struct O{float4 p:SV_Position;float4 old:TEXCOORD0;nointerpolation uint valid:TEXCOORD1;
 nointerpolation float actualNear:TEXCOORD2;nointerpolation uint slot:TEXCOORD3;
 nointerpolation float2 oldPhase:TEXCOORD4;nointerpolation uint reason:TEXCOORD5;};
O main(uint id:SV_VertexID){
 O o;o.p=Now[id];o.old=0;o.oldPhase=previousPhaseDepth.xy;o.reason=kReasonNone;
 uint first=id-id%3;o.actualNear=Now[first].z;o.slot=expected.x;
 bool currentValid=true;
 [unroll]for(uint i=0;i<3;++i){float4 now=Now[first+i];
  currentValid=currentValid && validPosition(now) && asuint(now.z)==asuint(o.actualNear);}
 if(identityMode.x==0){
  bool valid=currentValid && Identity[0].z && all(Identity[0].xy==expected.yz) && InstanceIndex[0]==expected.x;
  if(expected.w==1){
   valid=valid && PreviousIdentity0[0].z && all(PreviousIdentity0[0].xy==expected.yz);
   [unroll]for(uint i=0;i<3;++i)valid=valid && validPosition(Before0[first+i]);
   o.old=Before0[id];
  }
  o.valid=valid?expected.w:0;return o;
 }
 Resolved r=resolveGpuIdentity(id,currentValid);
 o.slot=r.slot;o.valid=r.valid;o.reason=r.reason;o.old=r.old;o.oldPhase=r.oldPhase;
 // A draw with no history of its own (3, 5, 6) takes what the sibling pass made of it, when one ran: its siblings' mean motion (fit mode 1)
 // or none, the view's own (mode 2). The previous position is this vertex moved by that many pixels, in homogeneous clip space so that
 // the pixel shader's interpolation is exact at every pixel; the previous phase is this frame's, so the sample is the motion itself.
 if(r.valid==2 && sibling.x!=0 && (r.reason==kReasonNoPrior || r.reason==kReasonIdentityDiffers || r.reason==kReasonPriorIdentityInvalid)){
  float4 fit=Fit[sibling.y*2];
  if(fit.z==1.0 || fit.z==2.0){
   float2 shift=float2(fit.x/(.5*extentPhase.x),fit.y/(-.5*extentPhase.y));
   o.old=float4(o.p.xy+shift*o.p.w,o.p.zw);o.oldPhase=extentPhase.zw;o.valid=1;o.reason=kReasonNone;
  }
 }
 return o;
}
)HLSL";
inline constexpr char kFlatForegroundMotionPs[]=R"HLSL(
Texture2D<float4> FinalOwner:register(t0);
Texture2D<float> RawDepth:register(t1);
cbuffer Settings:register(b0){float4 extentPhase;float4 previousPhaseDepth;uint4 expected;uint4 provenance;
 float4 priorPhase[4];uint4 identityMode;uint4 sibling;}
float4 main(float4 p:SV_Position,float4 old:TEXCOORD0,nointerpolation uint valid:TEXCOORD1,
 nointerpolation float actualNear:TEXCOORD2,nointerpolation uint actualSlot:TEXCOORD3,
 nointerpolation float2 oldPhase:TEXCOORD4,nointerpolation uint reason:TEXCOORD5,uint primitive:SV_PrimitiveID):SV_Target{
 uint slot=identityMode.x!=0?actualSlot:expected.x;
 if(slot>=0x007fffff)discard;
 int2 q=int2(p.xy);float4 owner=FinalOwner.Load(int3(q,0));
 float marker=-float(slot*2+3);
 if(asuint(owner.x)!=asuint(marker) || asuint(owner.y)!=asuint(p.z) ||
    asuint(owner.z)!=asuint(float(primitive)) || asuint(owner.w)!=asuint(float(provenance.x)) ||
    asuint(RawDepth.Load(int3(q,0)))!=asuint(owner.y))discard;
 float canonical=p.z*previousPhaseDepth.z/actualNear;
 if(!isfinite(canonical) || canonical<0 || canonical>1)return float4(0,0,-1,0);
 // A rejected sample has no motion; its x says why (the table above the vertex shader's source).
 if(valid!=1)return float4(float(reason),0,canonical,2);
 // Preserve the actual homogeneous projection, including negative W and its
 // infinite offscreen limit at W=0. No synthetic previous pose is selected.
 if(!all(isfinite(old)) || (old.w==0 && all(old.xy==0)))return float4(0,0,canonical,2);
 float2 projected=old.w!=0?old.xy/old.w:sign(old.xy)*3.402823466e+38;
 float2 previous=(projected*float2(.5,-.5)+.5)*extentPhase.xy;
 float2 motion=(previous-oldPhase)-(p.xy-extentPhase.zw);
 return float4(clamp(motion,-65504,65504),canonical,1);
}
)HLSL";
// The sibling pass, first half: one thread group a draw that matched its own history. It runs the map's own decision for every vertex of the
// draw (resolveGpuIdentity) and sums the motion each valid vertex would have written (pixels, the pixel shader's expression at the vertex), with
// its minimum and maximum per axis, then writes one record of three uint4 at Donors[3 * draw]: (matched, vertices, identity.x, identity.y with
// byte 30 left out), the mean as two float bit patterns, and (min x, min y, max x, max y) the same way. A draw this pass never dispatches for
// (no prior) keeps the zero record its buffer was cleared to, and a draw whose prior matched but whose vertices were all refused (the positions,
// an ambiguity) has matched set and no vertices, which makes it no donor.
inline constexpr char kFlatForegroundDonorBody[]=R"HLSL(
RWStructuredBuffer<uint4> Donors:register(u0);
groupshared float4 gSum[256];
groupshared float4 gMin[256];
groupshared float4 gMax[256];
static const float kMotionLimit=256.0;
[numthreads(256,1,1)]
void main(uint tid:SV_GroupIndex){
 const uint draw=sibling.y,count=sibling.z;
 float4 sum=0,lo=float4(1e30,1e30,0,0),hi=float4(-1e30,-1e30,0,0);
 for(uint id=tid;id<count;id+=256){
  uint first=id-id%3;
  float4 now=Now[id],n0=Now[first],n1=Now[first+1],n2=Now[first+2];
  bool currentValid=validPosition(n0) && validPosition(n1) && validPosition(n2) &&
   asuint(n1.z)==asuint(n0.z) && asuint(n2.z)==asuint(n0.z);
  Resolved r=resolveGpuIdentity(id,currentValid);
  if(r.valid!=1 || !(r.old.w>0) || !(now.w>0) || !all(isfinite(r.old)))continue;
  float2 cur=(now.xy/now.w*float2(.5,-.5)+.5)*extentPhase.xy;
  float2 prev=(r.old.xy/r.old.w*float2(.5,-.5)+.5)*extentPhase.xy;
  float2 m=(prev-r.oldPhase)-(cur-extentPhase.zw);
  if(!all(isfinite(m)) || any(abs(m)>kMotionLimit))continue;
  sum+=float4(m,1,0);lo.xy=min(lo.xy,m);hi.xy=max(hi.xy,m);
 }
 gSum[tid]=sum;gMin[tid]=lo;gMax[tid]=hi;
 GroupMemoryBarrierWithGroupSync();
 [unroll]for(uint s=128;s>0;s>>=1){
  if(tid<s){gSum[tid]+=gSum[tid+s];gMin[tid].xy=min(gMin[tid].xy,gMin[tid+s].xy);gMax[tid].xy=max(gMax[tid].xy,gMax[tid+s].xy);}
  GroupMemoryBarrierWithGroupSync();
 }
 if(tid==0){
  uint4 identity=Identity[0];uint raw=InstanceIndex[0],slot=raw&0x007fffff;
  uint readable;
  const bool authentic=raw==slot && slot<0x007fffff && identity.z!=0;
  const uint matched=(authentic && matchedPriors(identity,readable)!=0)?1u:0u;
  const float n=gSum[0].z;
  const float2 mean=n>0?gSum[0].xy/n:float2(0,0);
  Donors[draw*3]=uint4(matched,uint(n),identity.x,identity.y&kIdentityParameterMask);
  Donors[draw*3+1]=uint4(asuint(mean.x),asuint(mean.y),0,0);
  Donors[draw*3+2]=uint4(asuint(gMin[0].x),asuint(gMin[0].y),asuint(gMax[0].x),asuint(gMax[0].y));
 }
}
)HLSL";
// The sibling pass, second half: one thread a draw, over every draw of the frame. A draw that matched its own history needs nothing. One that
// did not looks for donors among the others by identity, pools their vertices (the mean weighted by vertex count, the extremes of all), and
// decides: no donor, the view's own motion (2); donors that agree to within the limit, their mean (1); donors that do not, a refusal (3). A
// draw whose pool identity cannot be read is a refusal too (4). The limits ride a constant buffer: counts.x draws, limits.x the spread in
// pixels, limits.y the fewest donor vertices.
inline constexpr char kFlatForegroundFitBody[]=R"HLSL(
StructuredBuffer<uint4> Donors:register(t0);
StructuredBuffer<uint4> Receivers:register(t1);
RWStructuredBuffer<float4> Fit:register(u0);
cbuffer FitSettings:register(b0){uint4 counts;float4 limits;}
static const uint kIdentityParameterMask=0xFF00FFFFu;
[numthreads(64,1,1)]
void main(uint3 t:SV_DispatchThreadID){
 const uint d=t.x;if(d>=counts.x)return;
 const uint4 own=Donors[d*3];
 if(own.x!=0){Fit[d*2]=float4(0,0,0,0);Fit[d*2+1]=float4(0,0,1,0);return;}
 const uint4 id=Receivers[d];
 if(id.z==0){Fit[d*2]=float4(0,0,4,0);Fit[d*2+1]=float4(0,0,0,0);return;}
 float n=0,donors=0;float2 sum=float2(0,0),lo=float2(1e30,1e30),hi=float2(-1e30,-1e30);
 for(uint e=0;e<counts.x;++e){
  const uint4 m=Donors[e*3];
  if(m.x==0 || m.y==0 || m.z!=id.x || m.w!=(id.y&kIdentityParameterMask))continue;
  const uint4 mean=Donors[e*3+1],range=Donors[e*3+2];
  const float c=float(m.y);
  sum+=asfloat(mean.xy)*c;n+=c;donors+=1;
  lo=min(lo,asfloat(range.xy));hi=max(hi,asfloat(range.zw));
 }
 if(donors==0 || n<limits.y){Fit[d*2]=float4(0,0,2,0);Fit[d*2+1]=float4(donors,n,0,0);return;}
 const float2 spread=hi-lo;const float worst=max(spread.x,spread.y);
 if(worst<=limits.x)Fit[d*2]=float4(sum/n,1,worst);
 else Fit[d*2]=float4(0,0,3,worst);
 Fit[d*2+1]=float4(donors,n,0,0);
}
)HLSL";
// THE SHADOW OF THE SIBLING MODEL, the two compute shaders (flat_foreground_shadow.h states what it measures and why; flat_foreground_shadow_gpu.h
// dispatches them). Both are the map's match text (Now, Before0..3, Identity, PreviousIdentity0..3, InstanceIndex, Settings, resolveGpuIdentity) and
// a body, so a vertex's motion is exactly the one the donor pass and the map take. Nothing here is read by the map: the tables are the shadow's own.
//
// The first, one thread group a draw that has priors: the draw's vertices (those the map would give a valid sample, the donor pass's own test) are
// accumulated into centered moments over (u, v, mx, my) -- u and v the screen position in units of half the render size, mx and my the motion in
// pixels -- by Welford's update in each thread and Chan's merge across the group. Eight uint4 a draw (floats as bit patterns): (n, identity.x,
// identity.y without byte 30, matched), the mean, (Suu, Suv, Svv, Sxx), (Sux, Suy, Svx, Svy), Syy, the minimum, the maximum, and the view depth range.
// The second, one thread group a draw of the frame: the draws of its identity that matched, others than itself, are merged into the donors it would
// have had; the mean model and the affine model are formed from them; and the group measures both against the draw's own vertices when it matched
// (leave-one-out), or only the draw's extent when it did not. Three uint4 a draw: (kind, donor draws, rms error of the mean model, rms error of the
// affine model), (the donors' fit residual, their spread, their vertices, the gate bits), (the draw's own mean motion, vertices compared, 0).
inline constexpr char kFlatForegroundShadowCommon[]=R"HLSL(
static const float kMotionLimit=256.0;
static const uint kMinVertices=3,kMinFitVertices=24;
static const float kSpreadLimit=1.0,kResidualLimit=0.5,kConditioning=0.005,kHullMargin=0.05,kHullFloor=0.01,kDepthRatio=1.25;
struct Acc{float n;float4 mean;float4 a;float4 b;float c;float4 lo;float4 hi;float2 w;};
Acc emptyAcc(){Acc s;s.n=0;s.mean=0;s.a=0;s.b=0;s.c=0;s.lo=float4(1e30,1e30,1e30,1e30);s.hi=-s.lo;s.w=float2(1e30,-1e30);return s;}
void addSample(inout Acc s,float4 x,float w){
 s.n+=1;float4 d=x-s.mean;s.mean+=d/s.n;float4 e=x-s.mean;
 s.a+=float4(d.x*e.x,d.x*e.y,d.y*e.y,d.z*e.z);
 s.b+=float4(d.x*e.z,d.x*e.w,d.y*e.z,d.y*e.w);
 s.c+=d.w*e.w;
 s.lo=min(s.lo,x);s.hi=max(s.hi,x);s.w=float2(min(s.w.x,w),max(s.w.y,w));
}
Acc mergeAcc(Acc p,Acc q){
 if(q.n==0)return p;
 if(p.n==0)return q;
 Acc r;r.n=p.n+q.n;float4 d=q.mean-p.mean;float f=p.n*q.n/r.n;
 r.mean=p.mean+d*(q.n/r.n);
 r.a=p.a+q.a+f*float4(d.x*d.x,d.x*d.y,d.y*d.y,d.z*d.z);
 r.b=p.b+q.b+f*float4(d.x*d.z,d.x*d.w,d.y*d.z,d.y*d.w);
 r.c=p.c+q.c+f*d.w*d.w;
 r.lo=min(p.lo,q.lo);r.hi=max(p.hi,q.hi);r.w=float2(min(p.w.x,q.w.x),max(p.w.y,q.w.y));
 return r;
}
// A vertex of the draw as the donor pass takes it: its screen position (u, v in half-extents), motion in pixels, view depth. False when the map would
// give it no valid sample.
bool shadowVertex(uint id,out float4 x,out float w){
 x=0;w=0;
 const uint first=id-id%3;
 const float4 now=Now[id],n0=Now[first],n1=Now[first+1],n2=Now[first+2];
 const bool currentValid=validPosition(n0) && validPosition(n1) && validPosition(n2) &&
  asuint(n1.z)==asuint(n0.z) && asuint(n2.z)==asuint(n0.z);
 const Resolved r=resolveGpuIdentity(id,currentValid);
 if(r.valid!=1 || !(r.old.w>0) || !(now.w>0) || !all(isfinite(r.old)))return false;
 const float2 cur=(now.xy/now.w*float2(.5,-.5)+.5)*extentPhase.xy;
 const float2 prev=(r.old.xy/r.old.w*float2(.5,-.5)+.5)*extentPhase.xy;
 const float2 m=(prev-r.oldPhase)-(cur-extentPhase.zw);
 if(!all(isfinite(m)) || any(abs(m)>kMotionLimit))return false;
 x=float4(cur/extentPhase.xy*2-1,m);w=now.w;
 return true;
}
)HLSL";
inline constexpr char kFlatForegroundShadowMomentsBody[]=R"HLSL(
RWStructuredBuffer<uint4> Moments:register(u0);
groupshared Acc gAcc[256];
[numthreads(256,1,1)]
void main(uint tid:SV_GroupIndex){
 const uint draw=sibling.y,count=sibling.z;
 Acc acc=emptyAcc();
 for(uint id=tid;id<count;id+=256){
  float4 x;float w;
  if(shadowVertex(id,x,w))addSample(acc,x,w);
 }
 gAcc[tid]=acc;
 GroupMemoryBarrierWithGroupSync();
 [unroll]for(uint s=128;s>0;s>>=1){
  if(tid<s)gAcc[tid]=mergeAcc(gAcc[tid],gAcc[tid+s]);
  GroupMemoryBarrierWithGroupSync();
 }
 if(tid==0){
  const Acc t=gAcc[0];
  const uint4 identity=Identity[0];const uint raw=InstanceIndex[0],slot=raw&0x007fffff;
  uint readable;
  const bool authentic=raw==slot && slot<0x007fffff && identity.z!=0;
  const uint matched=(authentic && matchedPriors(identity,readable)!=0)?1u:0u;
  const uint b=draw*8;
  Moments[b]=uint4(asuint(t.n),identity.x,identity.y&kIdentityParameterMask,matched);
  Moments[b+1]=asuint(t.mean);
  Moments[b+2]=asuint(t.a);
  Moments[b+3]=asuint(t.b);
  Moments[b+4]=uint4(asuint(t.c),0,0,0);
  Moments[b+5]=asuint(t.lo);
  Moments[b+6]=asuint(t.hi);
  Moments[b+7]=uint4(asuint(t.w.x),asuint(t.w.y),0,0);
 }
}
)HLSL";
inline constexpr char kFlatForegroundShadowEvalBody[]=R"HLSL(
StructuredBuffer<uint4> Moments:register(t11);
RWStructuredBuffer<uint4> Results:register(u0);
Acc loadAcc(uint j){
 Acc q;const uint b=j*8;
 q.n=asfloat(Moments[b].x);q.mean=asfloat(Moments[b+1]);q.a=asfloat(Moments[b+2]);q.b=asfloat(Moments[b+3]);
 q.c=asfloat(Moments[b+4].x);q.lo=asfloat(Moments[b+5]);q.hi=asfloat(Moments[b+6]);
 const uint4 w=Moments[b+7];q.w=float2(asfloat(w.x),asfloat(w.y));
 return q;
}
groupshared Acc gPool;
groupshared uint gDonors;
groupshared float4 gModel;      // m0.x, m0.y, u0, v0
groupshared float4 gAffine;     // ax, bx, ay, by
groupshared float4 gFit;        // residual rms, spread, conditioning, solved
groupshared float4 gSum[256];   // sum of the mean model's squared error, the affine's, vertices compared, 0
groupshared float4 gLo[256];    // u, v, w minima of the draw's valid current vertices
groupshared float4 gHi[256];
[numthreads(256,1,1)]
void main(uint tid:SV_GroupIndex){
 const uint self=sibling.y,count=sibling.z,total=sibling.w;
 const uint4 identity=Identity[0];
 const bool unreadable=identity.z==0;
 const uint4 own0=Moments[self*8];
 const bool matchedFlag=own0.w!=0;
 const bool hasOwn=matchedFlag && asfloat(own0.x)>0 && !unreadable;
 if(tid==0){
  Acc p=emptyAcc();uint donors=0;
  for(uint j=0;j<total && !unreadable;++j){
   if(j==self)continue;
   const uint4 r0=Moments[j*8];
   if(r0.w==0 || !(asfloat(r0.x)>0) || r0.y!=identity.x || r0.z!=(identity.y&kIdentityParameterMask))continue;
   p=mergeAcc(p,loadAcc(j));++donors;
  }
  gPool=p;gDonors=donors;
  const float det=p.a.x*p.a.z-p.a.y*p.a.y,tr=p.a.x+p.a.z;
  const bool solved=det>0 && tr>0;
  float ax=0,bx=0,ay=0,by=0,resid=0;
  if(solved){
   ax=(p.a.z*p.b.x-p.a.y*p.b.z)/det;bx=(-p.a.y*p.b.x+p.a.x*p.b.z)/det;
   ay=(p.a.z*p.b.y-p.a.y*p.b.w)/det;by=(-p.a.y*p.b.y+p.a.x*p.b.w)/det;
   const float rssx=p.a.w-(ax*p.b.x+bx*p.b.z),rssy=p.c-(ay*p.b.y+by*p.b.w);
   resid=sqrt(max(rssx+rssy,0)/p.n);
  }
  gModel=float4(p.mean.z,p.mean.w,p.mean.x,p.mean.y);
  gAffine=float4(ax,bx,ay,by);
  gFit=float4(resid,max(p.hi.z-p.lo.z,p.hi.w-p.lo.w),tr>0?det/(tr*tr):0,solved?1:0);
 }
 GroupMemoryBarrierWithGroupSync();
 const uint donors=gDonors;
 float4 sum=0,lo=float4(1e30,1e30,1e30,0),hi=float4(-1e30,-1e30,-1e30,0);
 for(uint id=tid;id<count;id+=256){
  const float4 now=Now[id];
  if(unreadable || !(validPosition(now) && now.w>0))continue;
  const float2 cur=(now.xy/now.w*float2(.5,-.5)+.5)*extentPhase.xy;
  const float2 uv=cur/extentPhase.xy*2-1;
  lo=float4(min(lo.xy,uv),min(lo.z,now.w),0);hi=float4(max(hi.xy,uv),max(hi.z,now.w),0);
  if(hasOwn && donors>0){
   float4 x;float w;
   if(shadowVertex(id,x,w)){
    const float2 em=x.zw-gModel.xy;
    const float2 fm=gModel.xy+float2(gAffine.x*(x.x-gModel.z)+gAffine.y*(x.y-gModel.w),gAffine.z*(x.x-gModel.z)+gAffine.w*(x.y-gModel.w));
    const float2 ea=x.zw-fm;
    sum+=float4(dot(em,em),dot(ea,ea),1,0);
   }
  }
 }
 gSum[tid]=sum;gLo[tid]=lo;gHi[tid]=hi;
 GroupMemoryBarrierWithGroupSync();
 [unroll]for(uint s=128;s>0;s>>=1){
  if(tid<s){gSum[tid]+=gSum[tid+s];gLo[tid]=min(gLo[tid],gLo[tid+s]);gHi[tid]=max(gHi[tid],gHi[tid+s]);}
  GroupMemoryBarrierWithGroupSync();
 }
 if(tid==0){
  const Acc p=gPool;
  float kind=0;
  if(unreadable)kind=5.0;
  else if(matchedFlag)kind=!hasOwn?0.0:(donors>0?1.0:2.0);
  else kind=donors>0?3.0:4.0;
  uint gates=0;
  float rmsMean=0,rmsAffine=0,evaluated=0;
  if(donors>0){
   if(p.n>=kMinVertices)gates|=1;
   if(gFit.y<=kSpreadLimit)gates|=2;
   if(p.n>=kMinFitVertices)gates|=4;
   if(gFit.w!=0 && gFit.z>=kConditioning)gates|=8;
   if(gFit.w!=0 && gFit.x<=kResidualLimit)gates|=16;
   const float mu=kHullMargin*(p.hi.x-p.lo.x)+kHullFloor,mv=kHullMargin*(p.hi.y-p.lo.y)+kHullFloor;
   const float3 rl=gLo[0].xyz,rh=gHi[0].xyz;
   if(rl.x<=rh.x && rl.x>=p.lo.x-mu && rh.x<=p.hi.x+mu && rl.y>=p.lo.y-mv && rh.y<=p.hi.y+mv)gates|=32;
   if(rl.x<=rh.x && rl.z>=p.w.x/kDepthRatio && rh.z<=p.w.y*kDepthRatio)gates|=64;
  }
  if(kind==1.0){
   const float nv=max(gSum[0].z,1.0);
   evaluated=gSum[0].z;rmsMean=sqrt(gSum[0].x/nv);rmsAffine=gFit.w!=0?sqrt(gSum[0].y/nv):rmsMean;
  }
  Results[self*3]=uint4(asuint(kind),asuint(float(donors)),asuint(rmsMean),asuint(rmsAffine));
  Results[self*3+1]=uint4(asuint(gFit.x),asuint(gFit.y),asuint(p.n),asuint(float(gates)));
  Results[self*3+2]=uint4(asuint(matchedFlag?asfloat(Moments[self*8+1].z):0.0),asuint(matchedFlag?asfloat(Moments[self*8+1].w):0.0),asuint(evaluated),0);
 }
}
)HLSL";
namespace flat_foreground_detail {
template<std::size_t A,std::size_t B>
constexpr std::array<char,A+B-1> concat(const char (&a)[A],const char (&b)[B]) {
    std::array<char,A+B-1> r{};
    for(std::size_t i=0;i+1<A;++i)r[i]=a[i];
    for(std::size_t i=0;i<B;++i)r[A-1+i]=b[i];
    return r;
}
// The same for three pieces: the shadow's shaders are the map's match text, the shadow's common text and a body.
template<std::size_t A,std::size_t B,std::size_t C>
constexpr std::array<char,A+B+C-2> concat3(const char (&a)[A],const char (&b)[B],const char (&c)[C]) {
    std::array<char,A+B+C-2> r{};
    for(std::size_t i=0;i+1<A;++i)r[i]=a[i];
    for(std::size_t i=0;i+1<B;++i)r[A-1+i]=b[i];
    for(std::size_t i=0;i<C;++i)r[A+B-2+i]=c[i];
    return r;
}
} // namespace flat_foreground_detail
// The three shaders that share the match are the common text and a body each, joined at compile time; the generator takes the pointers.
inline constexpr auto kFlatForegroundMotionVsText=flat_foreground_detail::concat(kFlatForegroundMatchHlsl,kFlatForegroundMotionVsBody);
inline constexpr auto kFlatForegroundDonorCsText=flat_foreground_detail::concat(kFlatForegroundMatchHlsl,kFlatForegroundDonorBody);
inline constexpr const char* kFlatForegroundMotionVs=kFlatForegroundMotionVsText.data();
inline constexpr const char* kFlatForegroundDonorCs=kFlatForegroundDonorCsText.data();
inline constexpr const char* kFlatForegroundFitCs=kFlatForegroundFitBody;
inline constexpr auto kFlatForegroundShadowMomentsCsText=flat_foreground_detail::concat3(kFlatForegroundMatchHlsl,kFlatForegroundShadowCommon,kFlatForegroundShadowMomentsBody);
inline constexpr auto kFlatForegroundShadowEvalCsText=flat_foreground_detail::concat3(kFlatForegroundMatchHlsl,kFlatForegroundShadowCommon,kFlatForegroundShadowEvalBody);
inline constexpr const char* kFlatForegroundShadowMomentsCs=kFlatForegroundShadowMomentsCsText.data();
inline constexpr const char* kFlatForegroundShadowEvalCs=kFlatForegroundShadowEvalCsText.data();
} // namespace edvr
