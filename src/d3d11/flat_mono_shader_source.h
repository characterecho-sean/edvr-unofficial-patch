#pragma once
namespace edvr {
// Prepend the generated kEngineMotionCoreHlsl: rigid-record arithmetic is shared
// verbatim with VR. This source has no eye, headset or panel globals.
inline constexpr char kFlatMonoShaderSource[] = R"HLSL(
cbuffer Mono : register(b0) {
    float4 now[6]; float4 old[6];
    uint4 size; // render width/height, output width/height
    uint4 flags; // reset, complete engine views, TAA, static scene (w: only ever nonzero in the 3D main menu)
    float4 jitter; // current xy, previous zw; actual raster phase in render pixels
    float4 rowsJitter; // NDC shift the camera rows themselves carry: current xy, previous zw; all zero = unjittered rows
};
cbuffer EngineNow : register(b1) { float4 EN[277]; };   // EN[276].x: the frame stamp
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

// Under the upstream camera injector the game derives the b1 rows from a jittered
// frustum, so rows 0..3 carry the raster phase (x += ndc.x*w, y += ndc.y*w, w =
// component 3, the same terms flat_camera_phase.h subtracts and the legacy scope
// adds). Every uv this shader evaluates them at is unjittered, so the phase comes
// out of the rows first. No phase returns the row untouched, whatever it holds.
float4 unjitterRow(float4 r, float2 ndc) {
    if(ndc.x==0 && ndc.y==0)return r;
    r.xy-=ndc*r.w;
    return r;
}
bool cameraBefore(float2 uv, float depth, out float4 before) {
    float4 n0=unjitterRow(now[0],rowsJitter.xy), n1=unjitterRow(now[1],rowsJitter.xy);
    float4 n2=unjitterRow(now[2],rowsJitter.xy), n3=unjitterRow(now[3],rowsJitter.xy);
    float4 o0=unjitterRow(old[0],rowsJitter.zw), o1=unjitterRow(old[1],rowsJitter.zw);
    float4 o2=unjitterRow(old[2],rowsJitter.zw), o3=unjitterRow(old[3],rowsJitter.zw);
    float3 a=float3(n0.x,n1.x,n2.x);
    float3 b=float3(n0.y,n1.y,n2.y);
    float3 c=float3(n0.w,n1.w,n2.w);
    float3 ca=cross(b,c), cb=cross(c,a), cc=cross(a,b);
    float det=dot(a,ca), iz=depth/n3.z;
    float3 rhs=float3(uv*float2(2,-2)+float2(-1,1),1)-n3.xyw*iz;
    float3 position=(ca*rhs.x+cb*rhs.y+cc*rhs.z)/det;
    position+=(now[5].xyz-old[5].xyz)*iz;
    before=position.x*o0+position.y*o1+position.z*o2+iz*o3;
    return before.w>0 && all(isfinite(before));
}
// 0 = camera term, 1 = exact engine motion, 2 = explicitly reject history.
uint engineBefore(int2 q,float2 uv,float depth,out float4 before) {
    before=0;
    if(flags.y==0)return 0;
    float2 es=Slots.Load(int3(q,0));
    if(!(es.x>=1))return 0;
    if(!(depth>0) || es.x>=4294967296.0)return 2;
    // A slot written by a keyed draw that something has since drawn over: the pixel's owner is not the
    // one that wrote the slot, so its record says nothing about it. Everywhere but the 3D main menu that
    // is not knowable and history is refused. In the menu (flags.w, set by the runtime only for a frame
    // whose contract came through the verified menu copy) nothing on screen moves but the camera, so the
    // pixel takes the camera term -- an unkeyed hull that overdraws a keyed one no longer aliases.
    if(asuint(depth)!=asuint(es.y))return flags.w!=0?0:2;
    uint code=uint(es.x);
    if(float(code)!=es.x || (code&1)==0)return 2;
    uint count,stride; Pool.GetDimensions(count,stride);
    uint slot=code>>1;
    if(slot>=count || stride!=336)return 2;
    EnginePoolRecord r=Pool[slot];
    uint kind=engineRecordKind(r,asuint(EN[276].x));
    if(kind==2)return 2;
    if(kind==3)kind=engineStaleStampKind(r,asuint(EN[276].x));
    if(kind==2)return 2;   // an older masked marker keeps no history, as masked always did
    if(kind!=1)return 0;   // not a rig record, or an older joined marker: the camera term
    // Freshness: a joined marker certifies only at the frame it was written.
    if(r.data[18].x!=(0x7FC0ED01u^engineMarkerHash(r,asuint(EN[276].x))))return 0;
    // The engine's own scene snapshots are the game's b1 upload too: same phase as the rows above, the
    // before-snapshot's being the previous frame's.
    if(!engineReprojectRows(r,uv*float2(2,-2)+float2(-1,1),depth,
        unjitterRow(EN[270],rowsJitter.xy),unjitterRow(EN[271],rowsJitter.xy),
        unjitterRow(EN[272],rowsJitter.xy),unjitterRow(EN[273],rowsJitter.xy),EN[275].xyz,
        unjitterRow(EB[270],rowsJitter.zw),unjitterRow(EB[271],rowsJitter.zw),
        unjitterRow(EB[272],rowsJitter.zw),unjitterRow(EB[273],rowsJitter.zw),EB[275].xyz,before))
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
