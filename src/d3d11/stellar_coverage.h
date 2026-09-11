#pragma once
// Coverage-only transcriptions of the captured ring and orbital materials.
// The original game colour/depth draw is never changed. Ring opacity follows
// PS 42AC0CACC9CDF72B; orbital alpha follows PS 6EEF165A350DA30F, recovered
// from the Steam Effects2 archive and verified against its captured hash.
namespace edvr {
constexpr char kRingCoverage[] = R"HLSL(
Texture2D<float4> Bands:register(t0); Texture2D<float4> Radial:register(t1);
Texture2D<float4> Detail:register(t3); SamplerState SampleRing:register(s1);
cbuffer Scene:register(b1){float4 scene[294];}
cbuffer Material:register(b2){float4 material[7];}
cbuffer Motion:register(b12){uint4 motionInfo;}
cbuffer P:register(b13){float4 floorAndStrength;float4 sceneProjection;}
struct In {float4 normalRadius:TEXCOORD1;float4 positionBand:TEXCOORD2;
    float3 tangent:TEXCOORD3;float2 uv:TEXCOORD4;float4 pos:SV_Position;};
float ringAlpha(In i) {
    float detail=Detail.SampleBias(SampleRing,i.uv,material[0].z).w;
    float inward=i.normalRadius.w-scene[137].x;
    float edge=min(saturate((scene[137].y-i.normalRadius.w)/material[0].w),saturate(inward/material[0].w));
    detail*=edge;
    float band=Bands.Sample(SampleRing,float2(i.positionBand.w,scene[141].w)).a;
    float radial=Radial.Sample(SampleRing,float2(saturate(inward/(scene[137].y-scene[137].x)),0)).x;
    float distance=length(i.positionBand.xyz);
    float farFade=saturate((distance-material[2].x)/(material[2].y-material[2].x));
    float nearWeight=1-saturate((distance-material[0].x)/(material[0].y-material[0].x));
    float close=saturate(radial*detail*material[4].x+material[4].y);
    float distant=saturate(radial*band*material[3].z);
    float opacity=saturate(lerp(distant,close,nearWeight));
    float view=1-pow(1-min(abs(dot(i.normalRadius.xyz,normalize(-i.positionBand.xyz))),1),material[2].w);
    float angle=material[2].z*view+1-material[2].z;
    return opacity*saturate(angle*saturate(abs(band*farFade)*material[4].z));
}
float4 main(In i,out float2 motion:SV_Target1):SV_Target0 {
    // Only the opaque part can own a single temporal depth. Transparent
    // radial gaps and stars behind them retain their scene motion.
    clip(ringAlpha(i)-floorAndStrength.x);
    motion=float2(motionInfo.x+1,i.pos.z);return 0;
}
)HLSL";

constexpr char kOrbitalCoverageVs[] = R"HLSL(
cbuffer Scene:register(b1){float4 scene[333];}
cbuffer Motion:register(b12){uint4 motionInfo;}
struct In {float4 vertex:POSTANGENT;float4 translation:OSTOWST;float4 rotation:OSTOWSR;
    float3 scale:OSTOWSS;float4 colour:COLOUR;uint vertexId:SV_VertexID;uint instance:SV_InstanceID;};
struct Out {float4 colour:__USER_STELLARVERTEX_COLOUR;
    float distance:__USER_STELLARVERTEX_STABLESIGNEDUNITDISTANCEPERSPECTIVE;
    float4 pos:SV_Position;nointerpolation uint record:TEXCOORD15;};
float3 turn(float4 q,float3 v) {
    precise float3 a=v*((q.w+q.w)*q.w);a=a-v;
    precise float3 b=(q.xyz+q.xyz)*dot(q.xyz,v);
    precise float3 c=(v.zxy*q.yzx-v.yzx*q.zxy)*(q.w+q.w); return (a+b)+c;
}
Out main(In i) {
    Out o; o.colour=i.colour; o.record=motionInfo.x+i.instance+1;
    precise float3 relative=turn(i.rotation,float3(i.vertex.xy,0)*i.scale)+i.translation.xyz;
    relative=relative-scene[275].xyz;
    precise float4 p=relative.x*scene[270]+relative.y*scene[271];
    p=p+relative.z*scene[272];p=p+scene[273];
    float sign=(i.vertexId&1)?-1:1; o.distance=p.w*sign;
    float3 tangent=turn(i.rotation,float3(i.vertex.zw,0)*i.scale);
    float3 v=float3(dot(scene[277].xyz,tangent),dot(scene[278].xyz,tangent),dot(scene[279].xyz,tangent));
    v=normalize(v);
    float2 projected=v.xy-v.z/dot(scene[279].xyz,relative)*float2(dot(scene[277].xyz,relative),dot(scene[278].xyz,relative));
    projected=normalize(projected);
    p.xy+=float2(-projected.y,projected.x)*sign*i.translation.w*(scene[332].zw*2*p.w);
    o.pos=p;return o;
}
)HLSL";
constexpr char kOrbitalCoveragePs[] = R"HLSL(
cbuffer Material:register(b2){float4 material[2];}
struct In {float4 colour:__USER_STELLARVERTEX_COLOUR;
    float distance:__USER_STELLARVERTEX_STABLESIGNEDUNITDISTANCEPERSPECTIVE;
    float4 pos:SV_Position;nointerpolation uint record:TEXCOORD15;};
float4 main(In i,out float2 motion:SV_Target1):SV_Target0 {
    float a=material[1].z==1 ? 1 : smoothstep(1,material[1].z,abs(i.distance/i.pos.w));
    clip(a*i.colour.a-1.0/255.0);
    motion=float2(i.record,i.pos.z); return 1.0/255.0;
}
)HLSL";
}
