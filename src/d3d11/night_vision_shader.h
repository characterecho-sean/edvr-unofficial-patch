#pragma once
// Night PS F786D34B5E118D5E, transcribed and replayed against captured
// depth, normals, exposure, mask, geometry and constants. The fixed path
// outlines depth geometry and gently brightens the existing surface texture,
// without a colour fill or fine normal-map outlines. It corrects pulse distance.
// EDVR_NIGHT_STOCK is a test-only reference; the live Off setting binds
// the game's original shader. See review-planet-performance-2026-09-11.md.
namespace edvr {
// A compact classification surface avoids simultaneous stencil sampling
// and writing on the original DSV. No scene copy or CPU readback.
constexpr char kNightExteriorCs[]=R"HLSL(
Texture2D<uint2> Stencil:register(t0);
RWTexture2D<float> Outside:register(u0);
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
    uint w,h;Outside.GetDimensions(w,h);if(id.x>=w || id.y>=h)return;
    Outside[id.xy]=(Stencil.Load(int3(id.xy,0)).y&16)==0?1:0;
}
)HLSL";
constexpr char kNightVisionPs[]=R"HLSL(
cbuffer Camera:register(b1){float4 c[333];}
cbuffer Night:register(b2){float4 n[12];}
cbuffer NightControl:register(b3){float4 edvrNight;}
Texture2D<float4> Exposure:register(t0);
Texture2D<float> Depth:register(t1);
Texture2D<float4> Normals:register(t2);
Texture2D<float4> Colour:register(t3);
Texture2D<float> Mask:register(t4);
Texture2D<float> Exterior:register(t5);
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
float inverseDepthAt(float2 uv,float2 dx,float2 dy){
    float d=Depth.SampleGrad(Point,coord(uv),dx,dy);
    // Missing/background samples represent infinity, never NaN or division
    // by zero. Their silhouettes still border valid foreground samples.
    float inverse=rcp(d);
    return d>0 && isfinite(d) && isfinite(inverse)?inverse:0;
}
float geometryEdge(float2 uv,float2 dx,float2 dy){
    float2 x=dx*n[11].y,y=dy*n[11].y;
    float a=inverseDepthAt(uv-x-y,dx,dy),b=inverseDepthAt(uv-y,dx,dy),c0=inverseDepthAt(uv+x-y,dx,dy);
    float d=inverseDepthAt(uv-x,dx,dy),e=inverseDepthAt(uv,dx,dy),f=inverseDepthAt(uv+x,dx,dy);
    float g=inverseDepthAt(uv-x+y,dx,dy),h=inverseDepthAt(uv+y,dx,dy),i=inverseDepthAt(uv+x+y,dx,dy);
    float sum=a+b+c0+d+e+f+g+h+i;
    if(sum<=0 || !isfinite(sum))return 0;
    // Inverse forward-depth is affine over a projected plane. Its Hessian
    // is zero there, including a sloped plane; textured surface normals do
    // not affect it. The mixed derivative includes diagonal silhouettes.
    // Normalize by all nine samples to bound near/far discontinuities and
    // remove absolute-distance scaling, rather than multiplying by centre Z.
    float xx=(d+f-2*e)/sum,yy=(b+h-2*e)/sum;
    float xy=(a+i-c0-g)*.25/sum;
    return sqrt(xx*xx+yy*yy+2*xy*xy);
}
float exteriorAt(float2 uv){
    uint w,h;Exterior.GetDimensions(w,h);
    return Exterior.Load(int3(clamp(int2(uv*float2(w,h)),int2(0,0),int2(w-1,h-1)),0));
}
float exteriorKernel(float2 uv,float2 dx,float2 dy,float radius){
    // Include every point used by the depth filter. A cockpit silhouette
    // must not emit onto a background pixel carrying terrain/sky motion.
    float2 x=dx*radius,y=dy*radius;
    return min(min(min(exteriorAt(uv-x-y),exteriorAt(uv-y)),min(exteriorAt(uv+x-y),exteriorAt(uv-x))),
        min(min(exteriorAt(uv),exteriorAt(uv+x)),min(min(exteriorAt(uv-x+y),exteriorAt(uv+y)),exteriorAt(uv+x+y))));
}
float exteriorFootprint(float2 uv,float2 dx,float2 dy){
    float base=exteriorKernel(uv,dx,dy,1);
    return n[11].y==1?base:min(base,exteriorKernel(uv,dx,dy,n[11].y));
}
float grid(float2 uv,float4 setting,float fade){
    float2 p=uv*setting.z;
    float2 f=abs(frac(p-.5)-.5)/(abs(ddx_coarse(p))+abs(ddy_coarse(p)));
    return (min(min(f.x,f.y)*setting.w,1)<=.5?1:0)*(fade+setting.x)*setting.y;
}
#if EDVR_NIGHT_STOCK
float4 main(float2 tex:TEXCOORD4,float4 pos:SV_Position):SV_Target{
#else
// Target 1 is a dual-source blend factor for the existing target 0 colour.
// This preserves texture contrast without reading/copying the scene colour.
struct NightOutput{float4 colour:SV_Target0;float4 scene:SV_Target1;};
NightOutput main(float2 tex:TEXCOORD4,float4 pos:SV_Position){
#endif
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
#if !EDVR_NIGHT_STOCK
    // Preserve deliberately pixelated or sampled-colour artistic modes.
    // The measured terrain night-vision pass uses neither of these flags.
    bool geometry=!asuint(n[11].z) && !asuint(n[10].w);
    // Identity blending, not discard: the original pass must still stamp
    // its stencil bit 4 on body pixels for subsequent game passes.
    if(geometry && exteriorAt(uv)==0){NightOutput o;o.colour=0;o.scene=1;return o;}
    float outside=geometry?exteriorFootprint(uv,dx,dy):1;
#endif
    float edge=0,orientation=0;float3 worldNormal=.5;
    if(d>=n[7].x && d<=n[7].y){
#if !EDVR_NIGHT_STOCK
        if(geometry)edge=outside>0?geometryEdge(uv,dx,dy):0;
        else {
#endif
        float2 x=dx*n[11].y,y=dy*n[11].y;
        float3 tl=normalAt(coord(uv-y-x),dx,dy),tm=normalAt(coord(uv-y),dx,dy),tr=normalAt(coord(uv-y+x),dx,dy);
        float3 ml=normalAt(coord(uv-x),dx,dy),mr=normalAt(coord(uv+x),dx,dy);
        float3 bl=normalAt(coord(uv+y-x),dx,dy),bm=normalAt(coord(uv+y),dx,dy),br=normalAt(coord(uv+y+x),dx,dy);
        float3 gx=(tr+br-tl-bl)*.09375+(mr-ml)*.3125;
        float3 gy=(bl+br-tl-tr)*.09375+(bm-tm)*.3125;
        edge=length(float2(gx.z,gy.z));
#if !EDVR_NIGHT_STOCK
        }
#endif
        edge*=fade*fade*n[11].x;
#if !EDVR_NIGHT_STOCK
        if(!geometry){
        float3 local=normalAt(coord(uv),dx,dy);
#else
        float3 local=normalAt(quantize(uv),dx,dy);
#endif
        worldNormal=local.x*c[277].xyz+local.y*c[278].xyz+local.z*c[279].xyz;
        orientation=fade*pow(abs(worldNormal.z+n[9].z),n[9].y)*n[9].x;
        worldNormal=worldNormal*.5+.5;
#if !EDVR_NIGHT_STOCK
        }
#endif
    }
    float lattice=saturate(grid(tex-.5,n[2],fade)+grid(tex-.5,n[3],fade)+grid(tex-.5,n[4],fade));
    if(d<n[7].x)lattice=0;
#if !EDVR_NIGHT_STOCK
    // Leave the original scene visible between contours. No constant green
    // fill, colour tint or normal-map orientation shading in geometry mode.
    float intensity=(lattice+(geometry?edge:fade*.01+edge+orientation))*mask*n[6].z*nearFade;
#else
    float intensity=(lattice+fade*.01+edge+orientation)*mask*n[6].z*nearFade;
#endif
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
    float3 radiance=colour/(worldNormal+.5)*c[1].w*alpha*exposure*c[90].y;
#if !EDVR_NIGHT_STOCK
    // Lift contour radiance without thickening its footprint. Keep opacity,
    // footprint and the original stencil writes intact; do not darken the
    // underlying terrain by increasing blend alpha or add a surface fill.
    if(geometry)radiance*=edvrNight.x;
#endif
#if EDVR_NIGHT_STOCK
    return float4(radiance,saturate(alpha));
#else
    NightOutput o;o.colour=float4(radiance,saturate(alpha));
    // Neutral exposure lift, not a green fill: multiply all existing colour
    // channels equally. Retain the original alpha attenuation at contours,
    // and the game's mask/range/fade and cockpit stencil. The source colour
    // is already exposed, so the Exposure texture must not be applied again.
    float amount=geometry && n[6].z>0 && d>0 && d>=n[7].x && d<=n[7].y?
        saturate(mask*nearFade*fade*n[0].w)*outside:0;
    o.scene=(1-saturate(alpha))*(1+(edvrNight.x-1)*amount);
    return o;
#endif
}

)HLSL";
}
