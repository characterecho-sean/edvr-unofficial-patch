#pragma once
// Night PS F786D34B5E118D5E, transcribed and replayed against captured
// depth, normals, exposure, mask, geometry and constants. Only the pulse
// distance and unconditional centre-normal pixelation are corrected.
// EDVR_NIGHT_STOCK is a test-only reference; the live Off setting binds
// the game's original shader. See review-planet-performance-2026-09-11.md.
namespace edvr {
constexpr char kNightVisionPs[]=R"HLSL(
cbuffer Camera:register(b1){float4 c[333];}
cbuffer Night:register(b2){float4 n[12];}
Texture2D<float4> Exposure:register(t0);
Texture2D<float> Depth:register(t1);
Texture2D<float4> Normals:register(t2);
Texture2D<float4> Colour:register(t3);
Texture2D<float> Mask:register(t4);
SamplerState Linear:register(s0);
SamplerState Point:register(s1);
#ifndef EDVR_NIGHT_STOCK
#define EDVR_NIGHT_STOCK 0
#endif
float2 quantize(float2 uv){
    float cell=max(floor(n[5].y),1);if(frac(cell*.5)>0)cell+=1;
    float2 step=cell/c[332].xy;
    return (floor(uv/step)+.5)*step;
}
float2 coord(float2 uv){return asuint(n[11].z)?quantize(uv):uv;}
float3 normalAt(float2 uv,float2 dx,float2 dy){
    float2 e=Normals.SampleGrad(Point,uv,dx,dy).gb*4-2;
    float d=dot(e,e);return normalize(float3(e*sqrt(max(1-d*.25,0)),d*.5-1));
}
float grid(float2 uv,float4 setting,float fade){
    float2 p=uv*setting.z;
    float2 f=abs(frac(p-.5)-.5)/(abs(ddx_coarse(p))+abs(ddy_coarse(p)));
    return (min(min(f.x,f.y)*setting.w,1)<=.5?1:0)*(fade+setting.x)*setting.y;
}
float4 main(float2 tex:TEXCOORD4,float4 pos:SV_Position):SV_Target{
    float2 uv=pos.xy*c[332].zw,dx=ddx_coarse(uv),dy=ddy_coarse(uv);
    float mask=Mask.Sample(Linear,uv);clip(mask);
    float d=Depth.SampleGrad(Point,coord(uv),dx,dy);
    d=min(d,Depth.SampleGrad(Point,coord(uv+c[332].zw),dx,dy));
    d=min(d,Depth.SampleGrad(Point,coord(uv+c[332].zw*float2(1,-1)),dx,dy));
    d=min(d,Depth.SampleGrad(Point,coord(uv+c[332].zw*float2(-1,1)),dx,dy));
    d=min(d,Depth.SampleGrad(Point,coord(uv-c[332].zw),dx,dy));
    float range=n[5].z-n[5].w;
    float fade=saturate((d-n[5].w)/range);
    float nearFade=n[5].x>0?saturate((d-n[7].x)/n[5].x):1;
    float edge=0,orientation=0;float3 worldNormal=.5;
    if(d>=n[7].x && d<=n[7].y){
        float2 x=dx*n[11].y,y=dy*n[11].y;
        float3 tl=normalAt(coord(uv-y-x),dx,dy),tm=normalAt(coord(uv-y),dx,dy),tr=normalAt(coord(uv-y+x),dx,dy);
        float3 ml=normalAt(coord(uv-x),dx,dy),mr=normalAt(coord(uv+x),dx,dy);
        float3 bl=normalAt(coord(uv+y-x),dx,dy),bm=normalAt(coord(uv+y),dx,dy),br=normalAt(coord(uv+y+x),dx,dy);
        float3 gx=(tr+br-tl-bl)*.09375+(mr-ml)*.3125;
        float3 gy=(bl+br-tl-tr)*.09375+(bm-tm)*.3125;
        edge=length(float2(gx.z,gy.z));
        edge*=fade*fade*n[11].x;
#if !EDVR_NIGHT_STOCK
        float3 local=normalAt(coord(uv),dx,dy);
#else
        float3 local=normalAt(quantize(uv),dx,dy);
#endif
        worldNormal=local.x*c[277].xyz+local.y*c[278].xyz+local.z*c[279].xyz;
        orientation=fade*pow(abs(worldNormal.z+n[9].z),n[9].y)*n[9].x;
        worldNormal=worldNormal*.5+.5;
    }
    float lattice=saturate(grid(tex-.5,n[2],fade)+grid(tex-.5,n[3],fade)+grid(tex-.5,n[4],fade));
    if(d<n[7].x)lattice=0;
    float intensity=(lattice+fade*.01+edge+orientation)*mask*n[6].z*nearFade;
    float alpha;float3 colour;
    if(asuint(n[10].w)){
        float4 sampled=Colour.Sample(Point,uv);colour=sampled.rgb*n[8].rgb;
        alpha=saturate((d-n[6].x)/(n[6].y-n[6].x))*sampled.a*intensity;
    }else{
        float pulseFade=fade;
#if !EDVR_NIGHT_STOCK
        float3 a=float3(c[270].x,c[271].x,c[272].x),b=float3(c[270].y,c[271].y,c[272].y),f=float3(c[270].w,c[271].w,c[272].w);
        float3 ca=cross(b,f),cb=cross(f,a),cc=cross(a,b);
        float det=dot(a,ca);
        float3 rhs=float3(uv*float2(2,-2)+float2(-1,1),1);
        // The observed projection is camera-relative, infinite reversed Z.
        // A changed/singular camera keeps the original pulse, not NaNs.
        if(abs(det)>1e-8 && isfinite(det) && c[273].z>0 &&
           all(abs(c[273].xyw)<1e-8) &&
           all(abs(float3(c[270].z,c[271].z,c[272].z))<1e-8)){
            float3 ray=(ca*rhs.x+cb*rhs.y+cc*rhs.z)/det;
            float distance=d*length(ray);
            if(isfinite(distance))pulseFade=saturate((distance-n[5].w)/range);
        }
#endif
        float x=(pulseFade-saturate((n[1].x-n[5].w)/range))*n[10].z;
        alpha=max(n[10].x,n[10].y*x*exp(1-x))*n[0].w*intensity;
        colour=n[8].rgb;
    }
    float exposure=1;
    if(asuint(n[1].z)){
        uint mode=asuint(n[1].w);
        exposure=mode==2?Exposure.Load(int3(1,0,0)).x*.125:mode==1?Exposure.Load(int3(0,0,0)).x*.125:mode==0?c[61].z:1;
        exposure*=exp2(n[1].y);
    }
    return float4(colour/(worldNormal+.5)*c[1].w*alpha*exposure*c[90].y,saturate(alpha));
}

)HLSL";
}
