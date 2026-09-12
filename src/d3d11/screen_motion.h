#pragma once
#include <cstdint>
#include "panel_curve.h"
struct ID3D11ShaderResourceView;
namespace edvr {
class Config;
void screenMotionConfigure(Config&);
void screenMotionSource(ID3D11DeviceContext*,unsigned width,unsigned height);
void screenMotionUiDraw(ID3D11DeviceContext*,PanelCurveDrawFn,unsigned count,unsigned instances,
                        unsigned start,int base,unsigned startInstance);
bool screenMotionRecognize();
void screenMotionDraw(ID3D11DeviceContext*,PanelCurveDrawFn,unsigned count,unsigned instances,
                      unsigned start,int base,unsigned startInstance,const float* curve=nullptr);
void screenMotionFrameBoundary();
void screenMotionShutdown();
ID3D11ShaderResourceView* screenMotionView(int eye,unsigned width,unsigned height);

// The game's screen VS supplies exact perspective-correct source UVs,
// including the curved mesh and the live panel-distance transform.
// Recover scene motion inside that image, then place its previous UV on
// the previous screen. There is no headset approximation in either step.
constexpr char kScreenMotionPs[]=R"HLSL(
Texture2D<float> Depth:register(t8);
ByteAddressBuffer Sizes:register(t9);
Texture2D<float> UiTransparency:register(t10);
Texture2D<uint2> SourceStencil:register(t11);
Texture2D<float4> WeaponMotion:register(t12);
cbuffer SourceNow:register(b2){float4 src[276];}
cbuffer SourceBefore:register(b3){float4 old[276];}
cbuffer ScreenBefore:register(b4){float4 model[12];}
cbuffer EyeBefore:register(b5){float4 eye[274];}
cbuffer Settings:register(b6){float4 shape;float4 extent;}
float4 main(float2 uv:__USER_VERTEX_M_TEXCOORD0,float4 pos:SV_Position):SV_Target {
    uint w,h;Depth.GetDimensions(w,h);
    float z=Depth.Load(int3(clamp(int2(uv*float2(w,h)),0,int2(w,h)-1),0));
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
    if(!ui && !attached) {
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
    if(any(prev<0) || any(prev>1))return float4(0,0,z,2);
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
    return float4(previous-pos.xy,z,ui?3:1);
}
)HLSL";
}
