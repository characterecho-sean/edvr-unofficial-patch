#pragma once
namespace edvr {
// Prepend the generated kEngineMotionCoreHlsl: rigid-record arithmetic is shared
// verbatim with VR. This source has no eye, headset or panel globals.
inline constexpr char kFlatMonoShaderSource[] = R"HLSL(
cbuffer Mono : register(b0) {
    float4 now[6]; float4 old[6];
    uint4 size; // render width/height, output width/height
    uint4 flags; // reset, complete engine views, TAA, reserved
    float4 jitter; // current xy, previous zw; actual raster phase in render pixels
};
cbuffer EngineNow : register(b1) { float4 EN[276]; };
cbuffer EngineBefore : register(b2) { float4 EB[276]; };
Texture2D<float4> Color : register(t0);
Texture2D<float> SceneDepth : register(t1);
Texture2D<float2> Slots : register(t2);
StructuredBuffer<EnginePoolRecord> Pool : register(t3);
Texture2D<float2> Motion : register(t4);
Texture2D<float> Rejection : register(t5);
Texture2D<float> ExpectedDepth : register(t6);
Texture2D<float4> History : register(t7);
Texture2D<float> HistoryDepth : register(t8);
SamplerState LinearClamp : register(s0);
RWTexture2D<float> OutDepth : register(u0);
RWTexture2D<float2> OutMotion : register(u1);
RWTexture2D<float> OutRejection : register(u2);
RWTexture2D<float> OutExpected : register(u3);
RWTexture2D<float4> OutColor : register(u4);

bool cameraBefore(float2 uv, float depth, out float4 before) {
    float3 a=float3(now[0].x,now[1].x,now[2].x);
    float3 b=float3(now[0].y,now[1].y,now[2].y);
    float3 c=float3(now[0].w,now[1].w,now[2].w);
    float3 ca=cross(b,c), cb=cross(c,a), cc=cross(a,b);
    float det=dot(a,ca), iz=depth/now[3].z;
    float3 rhs=float3(uv*float2(2,-2)+float2(-1,1),1)-now[3].xyw*iz;
    float3 position=(ca*rhs.x+cb*rhs.y+cc*rhs.z)/det;
    position+=(now[5].xyz-old[5].xyz)*iz;
    before=position.x*old[0]+position.y*old[1]+position.z*old[2]+iz*old[3];
    return before.w>0 && all(isfinite(before));
}
// 0 = camera term, 1 = exact engine motion, 2 = explicitly reject history.
uint engineBefore(int2 q,float2 uv,float depth,out float4 before) {
    before=0;
    if(flags.y==0)return 0;
    float2 es=Slots.Load(int3(q,0));
    if(!(es.x>=1))return 0;
    if(!(depth>0) || asuint(depth)!=asuint(es.y) || es.x>=4294967296.0)return 2;
    uint code=uint(es.x);
    if(float(code)!=es.x || (code&1)==0)return 2;
    uint count,stride; Pool.GetDimensions(count,stride);
    uint slot=code>>1;
    if(slot>=count || stride!=336)return 2;
    EnginePoolRecord r=Pool[slot];
    uint kind=engineRecordKind(r);
    if(kind==2)return 2;
    if(kind!=1)return 0;
    if(!engineReprojectRows(r,uv*float2(2,-2)+float2(-1,1),depth,
        EN[270],EN[271],EN[272],EN[273],EN[275].xyz,
        EB[270],EB[271],EB[272],EB[273],EB[275].xyz,before))
        return engineRecordMoved(r)?2:0;
    return 1;
}
[numthreads(8,8,1)]
void prep(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size.xy))return;
    int2 q=int2(id.xy); float2 uv=(float2(q)+.5)/float2(size.xy);
    // The depth belongs to the raster pixel q. Both raw camera and engine
    // rows describe the same surface at its unjittered screen coordinate.
    float2 rawUv=uv-jitter.xy/float2(size.xy);
    float depth=SceneDepth.Load(int3(q,0));
    float2 motion=0; float reject=1, expected=0;
    if(flags.x==0 && isfinite(depth) && depth>=0 && depth<=1) {
        float4 before;
        uint kind=engineBefore(q,rawUv,depth,before);
        // HLSL logical operators do not short-circuit: putting cameraBefore's
        // out parameter in || would overwrite the exact engine result.
        bool valid=kind==1;
        if(kind==0)valid=cameraBefore(rawUv,depth,before);
        if(valid) {
            float2 prev=before.xy/before.w*float2(.5,-.5)+.5;
            // SDK vectors exclude both raster phases; the backend receives
            // the actual current phase separately and tracks its own history.
            motion=(prev-rawUv)*float2(size.xy);
            expected=before.z/before.w;
            valid=all(isfinite(motion)) && all(abs(motion)<=65504) &&
                  all(prev>=0) && all(prev<=1) && isfinite(expected) && expected>=0 && expected<=1;
            reject=valid?0:1;
        }
    }
    if(reject!=0)motion=0;
    OutDepth[q]=isfinite(depth)?saturate(depth):0;
    OutMotion[q]=motion; OutRejection[q]=reject;
    if(flags.z!=0)OutExpected[q]=expected;
}
[numthreads(8,8,1)]
void taa(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size.zw))return;
    float2 uv=(float2(id.xy)+.5)/float2(size.zw);
    float2 rasterUv=uv+jitter.xy/float2(size.xy);
    int2 q=clamp(int2(rasterUv*float2(size.xy)),0,int2(size.xy)-1);
    float4 current=Color.SampleLevel(LinearClamp,rasterUv,0);
    float2 previous=uv+Motion.Load(int3(q,0))/float2(size.xy);
    float weight=0;
    if(flags.x==0 && Rejection.Load(int3(q,0))==0 && all(previous>=0) && all(previous<=1)) {
        int2 oldQ=clamp(int2(previous*float2(size.xy)+jitter.zw),0,int2(size.xy)-1);
        float was=HistoryDepth.Load(int3(oldQ,0)), predicted=ExpectedDepth.Load(int3(q,0));
        if(abs(was-predicted)<=max(1e-6,predicted*.01))weight=.9;
    }
    if(weight==0) {OutColor[id.xy]=current;return;}
    float3 lo=current.rgb,hi=current.rgb;
    [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x) {
        float3 value=Color.Load(int3(clamp(q+int2(x,y),0,int2(size.xy)-1),0)).rgb;
        lo=min(lo,value);hi=max(hi,value);
    }
    float3 history=History.SampleLevel(LinearClamp,previous,0).rgb;
    OutColor[id.xy]=float4(lerp(current.rgb,clamp(history,lo,hi),weight),current.a);
}
// Modern DLSS presets ignore NGX's bias-current-colour mask. Explicitly rejected
// pixels must display current colour even when the backend declines that hint.
[numthreads(8,8,1)]
void finish(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size.zw))return;
    float2 uv=(float2(id.xy)+.5)/float2(size.zw);
    float2 rasterUv=uv+jitter.xy/float2(size.xy);
    int2 q=int2(floor(rasterUv*float2(size.xy)-.5));
    float reject=0;
    [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)
        reject=max(reject,Rejection.Load(int3(clamp(q+int2(x,y),0,int2(size.xy)-1),0)));
    OutColor[id.xy]=reject>0?Color.SampleLevel(LinearClamp,rasterUv,0):History.Load(int3(id.xy,0));
}
// Single-frame recovery after a temporal backend declines already-jittered
// input. The result lands on the same output grid as the successful backend.
[numthreads(8,8,1)]
void spatial(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=size.zw))return;
    float2 uv=(float2(id.xy)+.5)/float2(size.zw);
    OutColor[id.xy]=Color.SampleLevel(LinearClamp,uv+jitter.xy/float2(size.xy),0);
}
)HLSL";
} // namespace edvr
