#pragma once
namespace edvr {
inline constexpr char kFlatForegroundMotionVs[]=R"HLSL(
Buffer<float4> Now:register(t0);
Buffer<float4> Before:register(t1);
StructuredBuffer<uint4> Identity:register(t2);
StructuredBuffer<uint4> PreviousIdentity:register(t3);
Buffer<uint> InstanceIndex:register(t4);
cbuffer Settings:register(b0){float4 extentPhase;float4 previousPhaseDepth;uint4 expected;uint4 provenance;}
struct O{float4 p:SV_Position;float4 old:TEXCOORD0;nointerpolation uint valid:TEXCOORD1;nointerpolation float actualNear:TEXCOORD2;};
bool validPosition(float4 p){return all(isfinite(p)) && p.z>0;}
O main(uint id:SV_VertexID){
 O o;o.p=Now[id];o.old=0;uint first=id-id%3;o.actualNear=Now[first].z;
 bool valid=Identity[0].z && all(Identity[0].xy==expected.yz) && InstanceIndex[0]==expected.x;
 if(expected.w==1){
  valid=valid && PreviousIdentity[0].z && all(PreviousIdentity[0].xy==expected.yz);
  [unroll]for(uint i=0;i<3;++i){float4 now=Now[first+i],old=Before[first+i];
   valid=valid && validPosition(now) && validPosition(old) && asuint(now.z)==asuint(o.actualNear);}
  o.old=Before[id];
 } else {[unroll]for(uint i=0;i<3;++i)valid=valid && validPosition(Now[first+i]) && asuint(Now[first+i].z)==asuint(o.actualNear);}
 o.valid=valid?expected.w:0;return o;
}
)HLSL";
inline constexpr char kFlatForegroundMotionPs[]=R"HLSL(
Texture2D<float4> FinalOwner:register(t0);
Texture2D<float> RawDepth:register(t1);
cbuffer Settings:register(b0){float4 extentPhase;float4 previousPhaseDepth;uint4 expected;uint4 provenance;}
float4 main(float4 p:SV_Position,float4 old:TEXCOORD0,nointerpolation uint valid:TEXCOORD1,nointerpolation float actualNear:TEXCOORD2,uint primitive:SV_PrimitiveID):SV_Target{
 int2 q=int2(p.xy);float4 owner=FinalOwner.Load(int3(q,0));
 float marker=-float(expected.x*2+3);
 if(asuint(owner.x)!=asuint(marker) || asuint(owner.y)!=asuint(p.z) ||
    asuint(owner.z)!=asuint(float(primitive)) || asuint(owner.w)!=asuint(float(provenance.x)) ||
    asuint(RawDepth.Load(int3(q,0)))!=asuint(owner.y))discard;
 float canonical=p.z*previousPhaseDepth.z/actualNear;
 if(!valid || !isfinite(canonical) || canonical<0 || canonical>1)return 0;
 if(valid==2)return float4(0,0,canonical,2);
 // Negative W retains the actual previous homogeneous projection. Clipping
 // has already established visibility of the current fragment. At W==0 a
 // nonzero XY has a real infinite offscreen limit; saturate that projection
 // to the representable SDK motion range, never a fabricated reset vector.
 if(!all(isfinite(old)) || (old.w==0 && all(old.xy==0)))return 0;
 float2 projected=old.w!=0?old.xy/old.w:sign(old.xy)*3.402823466e+38;
 float2 previous=(projected*float2(.5,-.5)+.5)*extentPhase.xy;
 float2 motion=(previous-previousPhaseDepth.xy)-(p.xy-extentPhase.zw);
 return float4(clamp(motion,-65504,65504),canonical,1);
}
)HLSL";
} // namespace edvr
