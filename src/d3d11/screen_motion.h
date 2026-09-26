#pragma once
#include <cstdint>
#include <string>
#include "panel_curve.h"
struct ID3D11ShaderResourceView;
namespace edvr {
class Config;
void screenMotionConfigure(Config&);

// Is this fix live? The first term of screenMotionSource, screenMotionUiDraw
// and screenMotionRecognize, published so the draw path can decline without a
// call -- each is invoked once per draw and, with fix.temporal_aa off (which
// is what arms this whole feature), each only ever returned immediately. 161
// innermost samples of the 1349-frame window of 2026-09-22 between the two
// draw entries. None of them touches anything before that first test.
//
// The flags live outside the module's State because State is reset wholesale
// in three places; screen_motion.cpp says what each reset does to them, and
// screen_motion_test asserts it.
namespace detail {
extern bool g_screenMotionEnabled;
extern bool g_screenMotionFailed;
}  // namespace detail
inline bool screenMotionLive() {
    return detail::g_screenMotionEnabled && !detail::g_screenMotionFailed;
}

void screenMotionSource(ID3D11DeviceContext*,unsigned width,unsigned height);
void screenMotionUiDraw(ID3D11DeviceContext*,PanelCurveDrawFn,unsigned count,unsigned instances,
                        unsigned start,int base,unsigned startInstance);
bool screenMotionRecognize();
void screenMotionDraw(ID3D11DeviceContext*,PanelCurveDrawFn,unsigned count,unsigned instances,
                      unsigned start,int base,unsigned startInstance,const float* curve=nullptr);
struct ScreenMotionGpuDiagnostics {
    uint64_t scope=0,sourceFrames=0;
    uint64_t uiClearCalls=0,uiDrawCalls=0,eyeClearCalls=0,projectionCalls=0;
    uint64_t uiClearSelected=0,uiDrawSelected=0,eyeClearSelected=0,projectionSelected=0;
    uint64_t uiClearSubmitted=0,uiDrawSubmitted=0,eyeClearSubmitted=0,projectionSubmitted=0;
    unsigned uiClearReady=0,uiDrawReady=0,eyeClearReady=0,projectionReady=0;
    unsigned uiClearSkipped=0,uiDrawSkipped=0,eyeClearSkipped=0,projectionSkipped=0;
    unsigned uiClearInvalid=0,uiDrawInvalid=0,eyeClearInvalid=0,projectionInvalid=0;
    bool collecting=false,draining=false;
};
// ctx is supplied by production's owner-thread frame boundary. The default
// keeps source-only fixtures able to advance history without a timing owner.
void screenMotionFrameBoundary(ID3D11DeviceContext* ctx=nullptr);
ScreenMotionGpuDiagnostics screenMotionGpuDiagnostics();
void screenMotionShutdown();
ID3D11ShaderResourceView* screenMotionView(int eye,unsigned width,unsigned height);

// The game's screen VS supplies exact perspective-correct source UVs,
// including the curved mesh and the live panel-distance transform.
// Recover scene motion inside that image, then place its previous UV on
// the previous screen. There is no headset approximation in either step.
//
// Engine-record motion on foot (docs/kinematic-motion-injection-2026-09-19.md,
// 2026-09-23 "On foot"): the shader is compiled with the engine-motion CORE in
// front of it (kEngineMotionCoreHlsl, the text the compose's enginePixel uses;
// screen_motion.cpp joins them). While engine.x says the source's views are
// bound, a source pixel whose MRT6 slot holds a certified rig record is
// carried by the record's own two pose blocks through the source pool draws'
// own scene constants (this frame's and last: SEN/SEB, the source rows) to
// its previous source UV; the panel mapping below takes it from there. A rig
// record EDVR cannot follow keeps no history (code 2); a pool surface that is
// not a rig record, a stale slot and a corrupt code keep the camera term.
// engine.y (the motion_source view): the validity carries 16 + that source
// kind instead, for the compose to paint through the panel. engine.z: count
// the kinds into PanelCounts on the eye pixels of a grid of that stride -- 1,
// every pixel, with diagnostics or motion_source; kPanelSampleStride on the
// sampled frames otherwise (engine_velocity.h); 0, not counted.
constexpr char kScreenMotionPs[]=R"HLSL(
Texture2D<float> Depth:register(t8);
ByteAddressBuffer Sizes:register(t9);
Texture2D<float> UiTransparency:register(t10);
Texture2D<uint2> SourceStencil:register(t11);
Texture2D<float4> WeaponMotion:register(t12);
Texture2D<float2> SourceSlots:register(t13);
StructuredBuffer<EnginePoolRecord> SourcePool:register(t14);
cbuffer SourceNow:register(b2){float4 src[276];}
cbuffer SourceBefore:register(b3){float4 old[276];}
cbuffer ScreenBefore:register(b4){float4 model[12];}
cbuffer EyeBefore:register(b5){float4 eye[274];}
cbuffer Settings:register(b6){float4 shape;float4 extent;float4 engine;}
cbuffer EngineSourceNow:register(b7){float4 SEN[277];}   // SEN[276].x: the frame stamp, as EN's
cbuffer EngineSourceBefore:register(b8){float4 SEB[276];}
RWStructuredBuffer<uint> PanelCounts:register(u1);
// enginePixel's kinds for the source texel q: 0 no engine data, 1 joined
// (prev = the surface's previous source UV), 2 masked, 3 not a rig record,
// 4 stale slot, 5 corrupt slot code, 6 stale stamp (a joined marker from an
// older frame: the camera term) -- the same tests in the same order.
uint sourceEngine(int2 q,float2 uv,float z,out float2 prev) {
    prev=uv;
    if(engine.x==0)return 0u;
    const float2 es=SourceSlots.Load(int3(q,0));
    // The patched pool shaders write 2 * slot + 1: the cleared -1 and an
    // untouched texel both fall below 1.
    if(!(es.x>=1.0))return 0u;
    if(!(z>0.0) || asuint(z)!=asuint(es.y))return 4u;
    const uint code=uint(es.x);
    if(float(code)!=es.x || (code&1u)==0u)return 5u;
    uint count,stride;SourcePool.GetDimensions(count,stride);
    const uint slot=code>>1u;
    if(slot>=count)return 0u;
    const EnginePoolRecord r=SourcePool[slot];
    const uint token=asuint(SEN[276].x);
    uint kind=engineRecordKind(r,token);
    if(kind==3u)kind=engineStaleStampKind(r,token);   // 6 stale stamp, 2 an older masked marker, 3 neither
    if(kind!=1u)return kind;
    // Freshness: a joined marker certifies only at the frame it was written;
    // an older frame's pose pair would replay a phantom delta. The camera
    // term stands, counted separately (kind 6).
    if(r.data[18].x!=(0x7FC0ED01u^engineMarkerHash(r,token)))return 6u;
    float4 before;
    if(!engineReprojectRows(r,uv*float2(2,-2)+float2(-1,1),z,SEN[270],SEN[271],SEN[272],SEN[273],SEN[275].xyz,
                            SEB[270],SEB[271],SEB[272],SEB[273],SEB[275].xyz,before))
        return engineRecordMoved(r)?2u:0u;
    prev=before.xy/before.w*float2(.5,-.5)+.5;
    if(all(isfinite(prev)))return 1u;
    prev=uv;
    return engineRecordMoved(r)?2u:0u;
}
// Under the motion_source view the validity carries the source kind.
float4 tagged(float4 v,uint kind){return engine.y!=0 && kind!=0u?float4(v.xyz,16.0+kind):v;}
float4 main(float2 uv:__USER_VERTEX_M_TEXCOORD0,float4 pos:SV_Position):SV_Target {
    uint w,h;Depth.GetDimensions(w,h);
    const int2 texel=clamp(int2(uv*float2(w,h)),0,int2(w,h)-1);
    float z=Depth.Load(int3(texel,0));
    bool ui=false;
    if(extent.z>0) {
        // Cover the actual source-filter footprint, including thin strokes.
        int2 at=int2(floor(uv*float2(w,h)-.5));
        [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)
            ui=ui || UiTransparency.Load(int3(clamp(at+int2(x,y),0,int2(w,h)-1),0))<1;
    }
    float2 prev=uv;
    // Stencil identifies the opaque source mesh; it does not tell us how
    // that mesh animated. Use its actual post-VS movement, including aiming.
    bool attached=false;
    if(extent.w>0)attached=(SourceStencil.Load(int3(clamp(int2(uv*float2(w,h)),0,int2(w,h)-1),0)).y&16)!=0;
    if(!ui && attached) {
        float4 motion=WeaponMotion.Load(int3(clamp(int2(uv*float2(w,h)),0,int2(w,h)-1),0));
        // R16 depth has at most half a ULP of rounding. Missing, occluded,
        // new or ambiguous meshes must not borrow world or fixed-UV history.
        if(motion.w!=1 || abs(motion.z-z)>max(abs(z)*.0005,3e-8) || !all(isfinite(motion)))return float4(0,0,z,2);
        prev+=motion.xy/float2(w,h);
        if(any(prev<0) || any(prev>1))return float4(0,0,z,2);
    }
    uint sk=0u;   // the source-space engine kind of this pixel (sourceEngine)
    if(!ui && !attached) {
    float2 carried;
    sk=sourceEngine(texel,uv,z,carried);
    if(engine.z!=0 && sk!=0u && all(uint2(pos.xy)%uint(engine.z)==0u))InterlockedAdd(PanelCounts[sk-1u],1u);
    // A rig record EDVR cannot follow keeps no history, as in the eye.
    if(sk==2u)return tagged(float4(0,0,z,2),sk);
    if(sk==1u) {
        prev=carried;
        if(any(prev<0) || any(prev>1))return tagged(float4(0,0,z,2),sk);
    } else {
    float3 a=float3(src[270].x,src[271].x,src[272].x);
    float3 b=float3(src[270].y,src[271].y,src[272].y);
    float3 c=float3(src[270].w,src[271].w,src[272].w);
    float3 ca=cross(b,c),cb=cross(c,a),cc=cross(a,b);float det=dot(a,ca);
    // Known source projection is infinite reversed Z (0.025/W). Reject a
    // changed shader encoding, singular camera or discontinuous origin.
    if(abs(det)<1e-8 || !isfinite(det) || src[273].z<=0 ||
       any(abs(float3(src[270].z,src[271].z,src[272].z))>1e-8))return 0;
    float iz=z/src[273].z;
    float3 rhs=float3(uv*float2(2,-2)+float2(-1,1),1)-src[273].xyw*iz;
    float3 position=(ca*rhs.x+cb*rhs.y+cc*rhs.z)/det;
    float3 delta=src[275].xyz-old[275].xyz;
    if(any(abs(delta)>50) || !all(isfinite(delta)))return 0;
    position+=delta*iz;
    float4 before=position.x*old[270]+position.y*old[271]+position.z*old[272]+iz*old[273];
    if(before.w<=0 || !all(isfinite(before)))return 0;
    prev=before.xy/before.w*float2(.5,-.5)+.5;
    // Leaving the source image is disocclusion, not an extrapolated panel.
    if(any(prev<0) || any(prev>1))return tagged(float4(0,0,z,2),sk);
    }
    }
    float2 size=asfloat(Sizes.Load2(0));
    float x=prev.x*2-1,zz=0;
    if(shape.x>0) {
        // Match the drawn mesh's linear segments exactly. An analytic
        // cylinder differs between vertices and makes fine detail crawl.
        float column=prev.x*shape.y,lo=floor(column),t=column-lo;
        float2 angles=(float2(lo,lo+1)/shape.y*2-1)*shape.x;
        x=lerp(sin(angles.x),sin(angles.y),t)/shape.x;
        zz=shape.z*(1-lerp(cos(angles.x),cos(angles.y),t))/shape.x;
    }
    float4 local=float4(x*size.x,(1-prev.y*2)*size.y,zz+shape.w,1);
    float3 world=float3(dot(model[9],local),dot(model[10],local),dot(model[11],local));
    float4 clip=world.x*eye[270]+world.y*eye[271]+world.z*eye[272]+eye[273];
    if(clip.w<=0 || !all(isfinite(clip)))return 0;
    float2 previous=(clip.xy/clip.w*float2(.5,-.5)+.5)*extent.xy;
    // UI is already composited into the source image. Its visible current
    // samples must not accumulate history along the scenery behind it.
    return tagged(float4(previous-pos.xy,z,ui?3:1),sk);
}
)HLSL";
// The screen shader's whole text: the engine-motion core (kEngineMotionCoreHlsl,
// generated from temporal_shader_source.h) in front of kScreenMotionPs. One
// function, so production and tools/engine_velocity_test compile the same.
inline std::string screenMotionPsSource(const char* engineCore) { return std::string(engineCore)+kScreenMotionPs; }
}
