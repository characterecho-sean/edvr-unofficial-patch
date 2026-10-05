#pragma once
namespace edvr {
inline constexpr char kFlatForegroundMotionVs[]=R"HLSL(
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
Buffer<uint> PreviousIndex0:register(t11);
Buffer<uint> PreviousIndex1:register(t12);
Buffer<uint> PreviousIndex2:register(t13);
Buffer<uint> PreviousIndex3:register(t14);
cbuffer Settings:register(b0){float4 extentPhase;float4 previousPhaseDepth;uint4 expected;uint4 provenance;
 float4 priorPhase[4];uint4 identityMode;}
struct O{float4 p:SV_Position;float4 old:TEXCOORD0;nointerpolation uint valid:TEXCOORD1;
 nointerpolation float actualNear:TEXCOORD2;nointerpolation uint slot:TEXCOORD3;
 nointerpolation float2 oldPhase:TEXCOORD4;};
bool validPosition(float4 p){return all(isfinite(p)) && p.z>0;}
bool samePosition(float4 a,float4 b){return all(asuint(a)==asuint(b));}
O main(uint id:SV_VertexID){
 O o;o.p=Now[id];o.old=0;o.oldPhase=previousPhaseDepth.xy;
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
 uint raw=InstanceIndex[0],slot=raw&0x007fffff;
 o.slot=slot;
 if(!currentValid || slot>=0x007fffff){o.valid=0;return o;}
 uint4 identity=Identity[0];
 bool authentic=raw==slot && identity.z!=0;
 uint matches=0;
 if(authentic){
  if(identityMode.y>0 && PreviousIdentity0[0].z && PreviousIndex0[0]==raw && all(PreviousIdentity0[0].xy==identity.xy))matches|=1;
  if(identityMode.y>1 && PreviousIdentity1[0].z && PreviousIndex1[0]==raw && all(PreviousIdentity1[0].xy==identity.xy))matches|=2;
  if(identityMode.y>2 && PreviousIdentity2[0].z && PreviousIndex2[0]==raw && all(PreviousIdentity2[0].xy==identity.xy))matches|=4;
  if(identityMode.y>3 && PreviousIdentity3[0].z && PreviousIndex3[0]==raw && all(PreviousIdentity3[0].xy==identity.xy))matches|=8;
 }
 if(matches==0){o.valid=2;return o;}
 uint chosen=(matches&1)?0:(matches&2)?1:(matches&4)?2:3;
 float4 selected[3];
 [unroll]for(uint i=0;i<3;++i){
  if(chosen==0)selected[i]=Before0[first+i];
  else if(chosen==1)selected[i]=Before1[first+i];
  else if(chosen==2)selected[i]=Before2[first+i];
  else selected[i]=Before3[first+i];
 }
 bool equivalent=true;
 [unroll]for(uint i=0;i<3;++i)equivalent=equivalent && validPosition(selected[i]);
 [unroll]for(uint c=0;c<4;++c)if((matches&(1u<<c))!=0 && c!=chosen){
  equivalent=equivalent && all(asuint(priorPhase[c].xy)==asuint(priorPhase[chosen].xy));
  [unroll]for(uint i=0;i<3;++i){
   float4 other=(c==0)?Before0[first+i]:(c==1)?Before1[first+i]:(c==2)?Before2[first+i]:Before3[first+i];
   equivalent=equivalent && samePosition(selected[i],other);
  }
 }
 o.old=selected[id%3];o.oldPhase=priorPhase[chosen].xy;
 o.valid=equivalent?1:2;
 return o;
}
)HLSL";
inline constexpr char kFlatForegroundMotionPs[]=R"HLSL(
Texture2D<float4> FinalOwner:register(t0);
Texture2D<float> RawDepth:register(t1);
cbuffer Settings:register(b0){float4 extentPhase;float4 previousPhaseDepth;uint4 expected;uint4 provenance;
 float4 priorPhase[4];uint4 identityMode;}
float4 main(float4 p:SV_Position,float4 old:TEXCOORD0,nointerpolation uint valid:TEXCOORD1,
 nointerpolation float actualNear:TEXCOORD2,nointerpolation uint actualSlot:TEXCOORD3,
 nointerpolation float2 oldPhase:TEXCOORD4,uint primitive:SV_PrimitiveID):SV_Target{
 uint slot=identityMode.x!=0?actualSlot:expected.x;
 if(slot>=0x007fffff)discard;
 int2 q=int2(p.xy);float4 owner=FinalOwner.Load(int3(q,0));
 float marker=-float(slot*2+3);
 if(asuint(owner.x)!=asuint(marker) || asuint(owner.y)!=asuint(p.z) ||
    asuint(owner.z)!=asuint(float(primitive)) || asuint(owner.w)!=asuint(float(provenance.x)) ||
    asuint(RawDepth.Load(int3(q,0)))!=asuint(owner.y))discard;
 float canonical=p.z*previousPhaseDepth.z/actualNear;
 if(!isfinite(canonical) || canonical<0 || canonical>1)return float4(0,0,-1,0);
 if(valid!=1)return float4(0,0,canonical,2);
 // Preserve the actual homogeneous projection, including negative W and its
 // infinite offscreen limit at W=0. No synthetic previous pose is selected.
 if(!all(isfinite(old)) || (old.w==0 && all(old.xy==0)))return float4(0,0,canonical,2);
 float2 projected=old.w!=0?old.xy/old.w:sign(old.xy)*3.402823466e+38;
 float2 previous=(projected*float2(.5,-.5)+.5)*extentPhase.xy;
 float2 motion=(previous-oldPhase)-(p.xy-extentPhase.zw);
 return float4(clamp(motion,-65504,65504),canonical,1);
}
)HLSL";
} // namespace edvr
