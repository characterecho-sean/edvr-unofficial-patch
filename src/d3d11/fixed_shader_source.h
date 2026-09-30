#pragma once
#include <string>
// Fixed HLSL shared by runtime declarations and the build-time compiler.
#define EDVR_UI_CHANGE_INPUT R"HLSL(
Texture2D<float> UiEdits:register(t14);
float uiEdit(float2 uv){
    uint w,h;UiEdits.GetDimensions(w,h);if(w==0||h==0)return 0;
    int2 p=int2(floor(uv*float2(w,h)-.5));float edit=0;
    [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)
        edit=max(edit,UiEdits.Load(int3(clamp(p+int2(x,y),0,int2(w,h)-1),0)));
    return edit;
}
)HLSL"

namespace edvr {
constexpr char kHoloMotionBuild[] = R"HLSL(
struct Record { uint4 key[8]; float4 clip[3]; float4 map[3]; float4 meta; };
cbuffer Model : register(b0) { float4 model[8]; }
cbuffer Scene : register(b1) { float4 scene[276]; }
cbuffer Material : register(b2) { float4 material[4]; }
cbuffer Draw : register(b3) { uint4 info; uint4 mesh[4]; float4 limits; }
StructuredBuffer<Record> Previous : register(t0);
struct PoolRecord { uint4 data[21]; };
StructuredBuffer<PoolRecord> Pool : register(t1);
ByteAddressBuffer Instance : register(t2);
RWStructuredBuffer<Record> Current : register(u0);
// Preserve the game's non-normalized UNORM16 quaternion operation exactly.
float3 turn(float4 q, float3 v) {
    return (2*q.w*q.w-1)*v + 2*dot(q.xyz,v)*q.xyz + 2*q.w*cross(q.xyz,v);
}
[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) {
    Record n=(Record)0;
    // b2 animates the material's glow coordinates every frame; it does
    // not move vertices or the primary surface UVs and is not identity.
    [unroll] for(uint k=0;k<4;++k) n.key[k]=mesh[k];
    bool valid=true;
    if(info.z==1 || info.z==4 || info.z==5) {
        [unroll] for(uint r=0;r<3;++r) n.clip[r]=model[r==2?7:4+r];
        // Planet material constants shade the sphere; they are not geometry
        // identity. The captured surface's mesh and texture identify it.
        if(info.z==1) [unroll] for(uint k=0;k<4;++k) n.key[4+k]=asuint(material[k]);
        if(info.z==5) {
            n.key[4]=asuint(material[0].w);
            n.key[5]=asuint(material[1].x);
            n.key[6]=asuint(material[1].y);
            valid=all(isfinite(material[0].w)) && all(isfinite(material[1].xy)) &&
                  material[0].w>0 && material[1].x>0 && material[1].y>0;
        }
        valid=valid && all(abs(model[6].xyz)<1e-10) && model[6].w>0;
    } else {
        float3 scale,pos; float4 q;
        if(info.z==2) {
            uint at=id.x*60;
            pos=asfloat(Instance.Load3(at))-scene[275].xyz;
            q=asfloat(Instance.Load4(at+16)); scale=asfloat(Instance.Load3(at+32));
            n.key[4]=Instance.Load4(at+16); n.key[5]=uint4(Instance.Load3(at+32),Instance.Load(at+12));
            // Geometry lies at local Z=0. A comparable unused Z axis avoids
            // an ill-conditioned inverse for billion-metre orbital ellipses.
            scale.z=max(abs(scale.x),abs(scale.y));
            // The RGBA float4 at byte offset 44 is the instance
            // colour/alpha. It is useful only for disambiguating the
            // fallback below: intensity and alpha can animate, while
            // chromaticity identifies the stroke.
            n.key[6]=Instance.Load4(at+44);
        } else {
            uint index=Instance.Load(0), count,stride; Pool.GetDimensions(count,stride);
            if(index>=count) { Current[info.x]=n; return; }
            PoolRecord p=Pool[index]; valid=p.data[0].x==0;
            scale=asfloat(p.data[0].y);
            uint2 packed=p.data[0].zw;
            q=float4(packed.x&65535,packed.x>>16,packed.y&65535,packed.y>>16)*(2.0/65535.0)-1;
            pos=asfloat(p.data[1].xyz)-scene[275].xyz;
        }
        float3 x=turn(q,float3(scale.x,0,0)),y=turn(q,float3(0,scale.y,0)),z=turn(q,float3(0,0,scale.z));
        [unroll] for(uint r=0;r<3;++r) {
            uint row=r==2?3:r;
            float4 c=info.z==2 ? float4(scene[270][row],scene[271][row],scene[272][row],scene[273][row]) : model[r==2?7:4+r];
            n.clip[r]=float4(dot(c.xyz,x),dot(c.xyz,y),dot(c.xyz,z),dot(c,float4(pos,1)));
        }
        valid=valid && all(isfinite(scale)) && all(abs(scale)>1e-8) && all(isfinite(pos)) && abs(dot(q,q)-1)<.002;
        if(info.z==0) valid=valid && all(abs(model[6].xyz)<1e-10) && model[6].w>0 && n.clip[2].w<limits.x;
        // Sprite VS uses the same pool transform and clip X/Y/W, but forces
        // clip Z=W and may billboard at planetary distances. UV tiles name
        // distinct atlas quads; changing brightness does not move geometry.
        if(info.z==3) n.key[4]=asuint(material[1]);
    }
    valid=valid && n.clip[2].w>.025 && all(isfinite(n.clip[0])) && all(isfinite(n.clip[1])) && all(isfinite(n.clip[2]));
    float3 a=cross(n.clip[1].xyz,n.clip[2].xyz),b=cross(n.clip[2].xyz,n.clip[0].xyz),c=cross(n.clip[0].xyz,n.clip[1].xyz);
    float det=dot(n.clip[0].xyz,a);
    valid=valid && isfinite(det) && abs(det)>1e-12;
    n.meta=float4(valid?1:0,limits.yz,0);
    uint matches=0,match=0;
    [loop] for(uint i=0;i<info.y;++i) {
        Record old=Previous[i]; bool same=old.meta.x==1;
        [unroll] for(uint j=0;j<8;++j) {
            // Mode 2 keeps key[6] as diagnostic identity data, but colour
            // intensity/alpha changes must not make an otherwise exact
            // orbital record miss the existing strict path.
            if(info.z!=2 || j!=6) same=same && all(n.key[j]==old.key[j]);
        }
        // Identical meshes can occur on multiple panels. Require one nearby
        // projected origin; an ambiguous match or a newly opened panel declines.
        float2 here=float2(n.clip[0].w,n.clip[1].w)/n.clip[2].w;
        float2 there=float2(old.clip[0].w,old.clip[1].w)/old.clip[2].w;
        same=same && all(abs(here-there)<.2) && old.clip[2].w>n.clip[2].w*.5 && old.clip[2].w<n.clip[2].w*2;
        if(same) {
            // Identical ring passes may repeat the same geometry/material.
            // They are interchangeable only when their complete transforms
            // are identical. Cockpit ambiguity remains a hard rejection.
            bool duplicate=(info.z==1 || info.z==4) && matches==1;
            [unroll] for(uint row=0;row<3;++row) duplicate=duplicate && all(old.clip[row]==Previous[match].clip[row]);
            if(!duplicate) { ++matches; match=i; }
        }
    }
    // A changing orbital instance can alter its quaternion/scale while
    // retaining the same draw/mesh, width and colour family. Only use this
    // relaxed identity when the original exact search found no candidate;
    // every candidate must be unique and retain the same continuity checks.
    if(valid && info.z==2 && matches==0) {
        uint fallbackMatches=0,fallbackMatch=0;
        float3 newRgb=asfloat(n.key[6].xyz); float newMax=max(newRgb.x,max(newRgb.y,newRgb.z));
        bool newRgbValid=all(isfinite(newRgb)) && isfinite(newMax) && newMax>0;
        [loop] for(uint i=0;i<info.y;++i) {
            Record old=Previous[i]; bool candidate=old.meta.x==1;
            [unroll] for(uint j=0;j<4;++j) candidate=candidate && all(n.key[j]==old.key[j]);
            candidate=candidate && n.key[5].w==old.key[5].w;
            float3 oldRgb=asfloat(old.key[6].xyz); float oldMax=max(oldRgb.x,max(oldRgb.y,oldRgb.z));
            bool oldRgbValid=all(isfinite(oldRgb)) && isfinite(oldMax) && oldMax>0;
            bool sameChroma=false;
            if(newRgbValid && oldRgbValid) sameChroma=all(abs(newRgb/newMax-oldRgb/oldMax)<=1e-6);
            candidate=candidate && sameChroma;
            float2 here=float2(n.clip[0].w,n.clip[1].w)/n.clip[2].w;
            float2 there=float2(old.clip[0].w,old.clip[1].w)/old.clip[2].w;
            candidate=candidate && all(abs(here-there)<.2) && old.clip[2].w>n.clip[2].w*.5 && old.clip[2].w<n.clip[2].w*2;
            if(candidate) { ++fallbackMatches; fallbackMatch=i; }
        }
        if(fallbackMatches==1) { matches=1; match=fallbackMatch; }
    }
    if(valid && matches==1) {
        Record old=Previous[match]; float3 t=float3(n.clip[0].w,n.clip[1].w,n.clip[2].w);
        [unroll] for(uint row=0;row<3;++row) {
            float3 v=float3(dot(old.clip[row].xyz,a),dot(old.clip[row].xyz,b),dot(old.clip[row].xyz,c))/det;
            n.map[row]=float4(v,old.clip[row].w-dot(v,t));
        }
        n.meta.w=all(isfinite(n.map[0])) && all(isfinite(n.map[1])) && all(isfinite(n.map[2])) ? 1:0;
    }
    Current[info.x+id.x]=n;
}
)HLSL";

constexpr char kUiContentCs[] = R"HLSL(
Texture2D<float4> Current:register(t0);
Texture2D<float> AgeIn:register(t1);
RWTexture2D<uint> Digest:register(u0);
RWTexture2D<float> Next:register(u1);
cbuffer C:register(b0){uint seed;uint srcW;uint srcH;uint pad0;}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){
    uint bw,bh;Digest.GetDimensions(bw,bh);if(any(id.xy>=uint2(bw,bh)))return;
    uint h=2166136261u;
    [unroll]for(uint y=0;y<4;++y)[unroll]for(uint x=0;x<4;++x){
        uint2 texel=id.xy*4+uint2(x,y);
        if(texel.x>=srcW||texel.y>=srcH)continue; // block may overhang the surface
        float4 c=Current.Load(int3(texel,0));
        c.rgb*=c.a; // RGB behind zero alpha is not visible content.
        h=(h^asuint(c.r))*16777619u;h=(h^asuint(c.g))*16777619u;
        h=(h^asuint(c.b))*16777619u;h=(h^asuint(c.a))*16777619u;
    }
    // Seed still hashes the real block -- a literal 0 is not a safe "no
    // prior content" sentinel, since nothing rules out a real block
    // hashing to 0 too, and doing so would read as changed on the very
    // next (comparing) frame even when nothing actually did. Seed differs
    // from a compare only in what it reports, never in what it stores.
    if(seed){Digest[id.xy]=h;Next[id.xy]=0;return;}
    uint prev=Digest[id.xy]; // in-place RW: this thread owns only this block
    Next[id.xy]=(h!=prev)?1.0:max(AgeIn.Load(int3(id.xy,0))-1.0/32.0,0.0);
    Digest[id.xy]=h;
}
)HLSL";


constexpr char kUiLayerCompositeHlsl[] = R"HLSL(
Texture2D<float4> Frame : register(t0);
Texture2D<float4> Layer : register(t1);
Texture2D<float4> Mult : register(t2);
RWTexture2D<float4> Out : register(u0);
cbuffer P : register(b0) {
    int4   region;     // the frame's region in Frame: x0, y0, x1, y1 (exclusive)
    float4 uv;         // the layer's rectangle for that region: u0, v0, u1, v1
    float2 layerSize;  // the layer, texels
    uint2  outSize;    // the region's size, which is the output's
    uint   mode;       // 0 the composite, 1 the ui_layer debug view
    uint   useMult;    // 1: a multiply drew into M this frame
    uint2  pad;
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outSize.x || id.y >= outSize.y) return;
    int2 p = region.xy + int2(id.xy);
    float4 f = Frame.Load(int3(p, 0));
    float2 span = (uv.zw - uv.xy) * layerSize / float2(outSize);
    float2 x0 = uv.xy * layerSize + float2(id.xy) * span;
    float2 x1 = x0 + span;
    int2 k0 = max(int2(floor(x0)), int2(0, 0));
    int2 k1 = min(int2(ceil(x1)) - 1, int2(layerSize) - 1);
    float4 acc = float4(0, 0, 0, 0);
    float3 accM = float3(0, 0, 0);
    float wsum = 0;
    [loop] for (int j = k0.y; j <= min(k1.y, k0.y + 3); ++j) {
        float wy = min(x1.y, (float)j + 1.0) - max(x0.y, (float)j);
        if (wy <= 0) continue;
        [loop] for (int i = k0.x; i <= min(k1.x, k0.x + 3); ++i) {
            float wx = min(x1.x, (float)i + 1.0) - max(x0.x, (float)i);
            if (wx <= 0) continue;
            acc += Layer.Load(int3(i, j, 0)) * (wx * wy);
            if (useMult != 0) accM += Mult.Load(int3(i, j, 0)).rgb * (wx * wy);
            wsum += wx * wy;
        }
    }
    float4 l = wsum > 0 ? acc / wsum : float4(0, 0, 0, 1);
    float3 m = (useMult != 0 && wsum > 0) ? accM / wsum : float3(1, 1, 1);
    float3 c = mode == 1 ? l.rgb + float3(0.0, 0.08, 0.25) * (1.0 - l.a * dot(m, 1.0 / 3.0))
                         : l.rgb + f.rgb * l.a * m;
    Out[id.xy] = float4(saturate(c), f.a);
}
)HLSL";

constexpr char kUiLayerCoverageVsHlsl[] = R"HLSL(
struct V { float4 p : SV_POSITION; };
V main(uint id : SV_VertexID) {
    float2 p = id == 0 ? float2(-1, -1) : (id == 1 ? float2(-1, 3) : float2(3, -1));
    V v; v.p = float4(p, 0, 1); return v;
}
)HLSL";

constexpr char kUiLayerCoveragePsHlsl[] = R"HLSL(
Texture2D<float4> HdrLayer : register(t0);
float4 main(float4 p : SV_POSITION) : SV_Target {
    return float4(0, 0, 0, HdrLayer.Load(int3(p.xy, 0)).a);
}
)HLSL";

inline constexpr const char* kUiSeedVs = R"(
struct V { float4 p:SV_POSITION; };
V main(uint id:SV_VertexID) {
  float2 p = id==0 ? float2(-1,-1) : (id==1 ? float2(-1,3) : float2(3,-1));
  V v; v.p=float4(p,0,1); return v;
})";

inline constexpr const char* kUiSeedPsDepthStencil = R"(
cbuffer C:register(b0) { uint2 inSize; uint2 outSize; float2 jitter; uint writeBit; };
Texture2D<float> D:register(t0); Texture2D<uint4> S:register(t1);
struct O { float d:SV_Depth; uint s:SV_StencilRef; };
O main(float4 p:SV_POSITION) {
  int2 q=int2(floor(p.xy*float2(inSize)/float2(outSize)+float2(jitter)));
  q=clamp(q,int2(0,0),int2(inSize)-1); uint s=S.Load(int3(q,0)).y;
  O o; o.d=D.Load(int3(q,0)); o.s=s; return o;
})";

inline constexpr const char* kUiSeedPsDepthOnly = R"(
cbuffer C:register(b0) { uint2 inSize; uint2 outSize; float2 jitter; uint writeBit; };
Texture2D<float> D:register(t0); Texture2D<uint4> S:register(t1);
struct O { float d:SV_Depth; };
O main(float4 p:SV_POSITION) {
  int2 q=int2(floor(p.xy*float2(inSize)/float2(outSize)+float2(jitter)));
  q=clamp(q,int2(0,0),int2(inSize)-1); uint s=S.Load(int3(q,0)).y;
  if ((s & writeBit)==0) discard; O o; o.d=D.Load(int3(q,0)); return o;
})";

inline constexpr const char* kUiSeedPsDepthNoStencil = R"(
cbuffer C:register(b0) { uint2 inSize; uint2 outSize; float2 jitter; uint writeBit; };
Texture2D<float> D:register(t0); float main(float4 p:SV_POSITION):SV_Depth {
  int2 q=int2(floor(p.xy*float2(inSize)/float2(outSize)+float2(jitter)));
  q=clamp(q,int2(0,0),int2(inSize)-1); return D.Load(int3(q,0));
})";

const char kPanelDepthHlsl[] = EDVR_UI_CHANGE_INPUT
    "Texture2D<float4> Surf : register(t1);\n"
    "SamplerState Smp : register(s1);\n"
    "cbuffer P : register(b13) { float4 floorAndStrength; };\n"
    "struct In {\n"
    "    float4 tc0 : TEXCOORD0;\n"
    "    float3 tc2 : TEXCOORD2;\n"
    "    float3 tc4 : TEXCOORD4;\n"
    "    float3 tc5 : TEXCOORD5;\n"
    "    float2 tc6 : TEXCOORD6;\n"
    "};\n"
    "float4 main(In i, out float edit : SV_Target2) : SV_Target0 {\n"
    "    float a = Surf.Sample(Smp, i.tc6).a;\n"
    "    edit = uiEdit(i.tc6);\n"
    "    clip(max(a - floorAndStrength.x, edit - 1.0/255.0));\n"
    "    return floorAndStrength.w;\n"
    "}\n";

const char kScreenDepthHlsl[] = EDVR_UI_CHANGE_INPUT
    "Texture2D<float4> Surf : register(t0);\n"
    "SamplerState Smp : register(s0);\n"
    "cbuffer P : register(b13) { float4 floorAndStrength; };\n"
    "struct In { float2 tc0 : TEXCOORD0; };\n"
    "float4 main(In i, out float edit : SV_Target2) : SV_Target0 {\n"
    "    float a = Surf.Sample(Smp, i.tc0).a;\n"
    "    edit = uiEdit(i.tc0);\n"
    "    clip(max(a - min(floorAndStrength.x, 1.0 / 255.0), edit - 1.0/255.0));\n"
    "    return floorAndStrength.w;\n"
    "}\n";

const char kSpriteDepthHlsl[] = EDVR_UI_CHANGE_INPUT R"HLSL(
Texture2D<float4> Surf : register(t0);
Texture2D<float> SceneDeviceDepth : register(t2);
SamplerState Smp : register(s0);
cbuffer P : register(b13) { float4 floorAndStrength; float4 sceneProjection; };
cbuffer Motion : register(b12) { uint4 motionInfo; };
struct In { float2 tc0 : TEXCOORD0; float4 pos : SV_Position; };
float4 main(In i, out float depth : SV_Depth, out float2 motion : SV_Target1, out float edit : SV_Target2) : SV_Target0 {
    // This composite has no holo glow. Retain its faint antialiased strokes,
    // but never turn erased scrolling ticks into 32-frame terrain strips.
    // Departed text is already cleared by the post-resolve influence history.
    clip(Surf.Sample(Smp, i.tc0).a - min(floorAndStrength.x, 1.0/255.0));
    edit = uiEdit(i.tc0);
    float own = sceneProjection.x + sceneProjection.y / max(i.pos.w, 0.000001);
    depth = max(own, SceneDeviceDepth.Load(int3(int2(i.pos.xy),0)));
    motion = float2(motionInfo.x+1, depth);
    return floorAndStrength.w;
}
)HLSL";

const char kHoloDepthBody[] = EDVR_UI_CHANGE_INPUT
    "SamplerState Smp : register(s1);\n"
    "cbuffer P : register(b13) { float4 floorAndStrength; float4 sceneProjection; };\n"
    "cbuffer Motion : register(b12) { uint4 motionInfo; };\n"
    "struct In {\n"
    "    float4 tc0 : TEXCOORD0;\n"
    "    float3 tc4 : TEXCOORD4;\n"
    "    float3 tc6 : TEXCOORD6;\n"
    "    float3 tc7 : TEXCOORD7;\n"
    "    float2 tc8 : TEXCOORD8;\n"
    "    float4 pos : SV_Position;\n"
    "};\n"
    "float4 main(In i, out float2 motion : SV_Target1, out float edit : SV_Target2) : SV_Target0 {\n"
    "    float a = Surf.Sample(Smp, i.tc8).a;\n"
    "    float den = i.pos.z - sceneProjection.x;\n"
    "    bool cockpit = den > 0 && sceneProjection.y > 0 && sceneProjection.y / den < sceneProjection.z;\n"
    "    edit = uiEdit(i.tc8);\n"
    "    clip(max(a - (cockpit ? min(floorAndStrength.x, 1.0 / 255.0) : floorAndStrength.x), edit - 1.0/255.0));\n"
    "    motion = float2(motionInfo.x+1, i.pos.z);\n"
    "    return floorAndStrength.w;\n"
    "}\n";

inline const std::string kHoloDepthHlsl = "Texture2D<float4> Surf : register(t2);\n" + std::string(kHoloDepthBody);

inline const std::string kHoloUnlitDepthHlsl = "Texture2D<float4> Surf : register(t1);\n" + std::string(kHoloDepthBody);

const char kSmokeDepthHlsl[] =
    "Texture2D<float4> Depth : register(t0);\n"
    "Texture2D<float4> Streak : register(t1);\n"
    "SamplerState Smp0 : register(s0);\n"
    "SamplerState Smp1 : register(s1);\n"
    "cbuffer CB1 : register(b1) { float4 cb1[211]; };\n"
    "cbuffer CB2 : register(b2) { float4 cb2[3]; };\n"
    "#ifdef CORONA_MOTION\n"
    "Texture2D<float> SceneDepth : register(t2);\n"
    "cbuffer Motion : register(b12) { uint4 motionInfo; };\n"
    "#endif\n"
    "cbuffer P : register(b13) { float4 floorAndStrength; float4 proj; };\n"
    "struct In {\n"
    "    float3 tc0 : TEXCOORD0;\n"
    "    float3 tc1 : TEXCOORD1;\n"
    "    float3 tc2 : TEXCOORD2;\n"
    "    float2 tc3 : TEXCOORD3;\n"
    "    float4 pos : SV_Position;\n"
    "};\n"
    "#ifdef CORONA_MOTION\n"
    "float4 main(In i, out float oDepth : SV_Depth, out float4 motion : SV_Target1) : SV_Target0 {\n"
    "#else\n"
    "float4 main(In i, out float oDepth : SV_Depth) : SV_Target0 {\n"
    "#endif\n"
    "    float3 d = i.tc2 - i.tc0;\n"
    "    float dd = (dot(d, d) - cb1[126].x * cb1[126].x) * 4.0;\n"
    "    float3 n = normalize(i.tc2);\n"
    "    float a = dot(-n, d);\n"
    "    float b = a + a;\n"
    "    float disc = sqrt(b * b - dd);\n"
    "    bool hit = 0.0 < disc;\n"
    "    float t = hit ? (-a * 2.0 + disc) * 0.5 : 0.0;\n"
    "    float sphereZ = -n.z * t + i.tc2.z;\n"
    "    float2 uv = i.tc1.xy / i.tc1.z * float2(0.5, -0.5) + 0.5;\n"
    "    float sceneZ = Depth.Sample(Smp1, uv).x;\n"
    "    if (sceneZ - sphereZ + cb1[126].x * 0.0001 < 0.0) discard;\n"
    "    float fade = saturate((sceneZ - i.tc1.z) / (cb1[126].x * 0.4));\n"
    "    fade = hit ? 1.0 : fade;\n"
    "    fade *= cb1[126].z * cb2[1].z;\n"
    "    float2 uv1 = float2(i.tc3.x - cb1[210].y * cb2[1].w, (i.tc3.y + 1.0) * 0.5);\n"
    "    float2 uv2 = float2(i.tc3.x + cb1[210].y * cb2[2].x, i.tc3.y * 0.5);\n"
    "    float streak = Streak.Sample(Smp0, uv1).x + Streak.Sample(Smp0, uv2).x;\n"
    "    float alpha = fade * streak;\n"
    "    // The mask (the review of 2026-09-10): w is the strength at full\n"
    "    // opacity, and the value follows the smoke's own alpha up to it,\n"
    "    // quantised to an ODD quantum -- the pass keeps the camera's path\n"
    "    // under an odd one (floorBuffer). w of nought is the one-quantum\n"
    "    // mark of before, as good as unmarked to NVIDIA.\n"
    "    float q = floor(saturate(alpha * floorAndStrength.w) * 63.0 + 0.5);\n"
    "    // The dense core (alpha at the floor or above) writes its depth;\n"
    "    // the fringe under it writes none -- the target is EDVR's own,\n"
    "    // cleared to the far value, so a far depth changes nothing -- and\n"
    "    // marks the mask only where the strength gives it a quantum, so\n"
    "    // the mark fades with the smoke instead of stepping at the floor\n"
    "    // (the review's second note).\n"
    "    bool core = alpha >= floorAndStrength.z;\n"
    "    clip((core || q > 0.0) ? 1.0 : -1.0);\n"
    "    // The depth from the ribbon's own view depth (TEXCOORD1.z, the\n"
    "    // value its shader compares with the depth resolve) in the scene's\n"
    "    // encoding: the raster's z is the vertex shader's clip z plus a\n"
    "    // constant (15.01, before the divide) whose meaning rests on the\n"
    "    // matrix the game uploads (the review's lead), while this is exact.\n"
    "    // The raster's z when no projection is known.\n"
    "    float own = proj.y != 0.0 ? proj.x + proj.y / max(i.tc1.z, 0.01) : i.pos.z;\n"
    "    oDepth = core ? own : 0.0;\n"
    "#ifdef CORONA_MOTION\n"
    "    bool coronaVisible = core && own >= SceneDepth.Load(int3(int2(i.pos.xy),0));\n"
    "    motion = float4(motionInfo.x+1u, own, 0.0, coronaVisible ? 1.0 : 0.0);\n"
    "#endif\n"
    "    return (4.0 * q + 3.0) / 255.0; // class 3: smoke, not UI evidence\n"
    "}\n";

const char kHudDepthHlsl[] = R"HLSL(
Texture2D<float4> Depth : register(t0);
Texture2D<float4> Noise : register(t1);
Texture2D<float> SceneDeviceDepth : register(t2);
SamplerState Smp0 : register(s0);
SamplerState Smp1 : register(s1);
cbuffer CB1 : register(b1) { float4 cb1[205]; };
cbuffer P : register(b13) { float4 floorAndStrength; float4 sceneProjection; };
struct In {
    float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1;
    float4 tc2 : TEXCOORD2; float4 tc5 : TEXCOORD5;
    float4 tc9 : TEXCOORD9; float4 tc10 : TEXCOORD10;
    float4 tc13 : TEXCOORD13; float4 tc16 : TEXCOORD16;
    float2 tc17 : TEXCOORD17; float3 tc18 : TEXCOORD18;
    float4 pos : SV_Position;
};
// ps 8DEF46452FA459F5, instructions 120..131 (repeated for three octaves).
float hudNoise(float3 p) {
    float3 cell=floor(p), f=frac(p);
    float3 u=f*f*(3.0-2.0*f);
    float2 uv=(cell.xy+cell.z*float2(37,17)+u.xy+0.5)/256.0;
    float2 n=Noise.SampleLevel(Smp0,uv,-100.0).xy;
    return lerp(n.y,n.x,u.z);
}
float4 main(In i, out float oDepth : SV_Depth) : SV_Target {
    float2 uv=i.tc1.xy/i.tc1.z*0.5+0.5;
    float sceneZ=Depth.Sample(Smp1,float2(uv.x,1.0-uv.y)).x;
    clip(sceneZ-i.tc1.z); clip(i.tc5.w-0.01);
    float k=cb1[204].x/(cb1[204].x+0.00001);
    float fade=saturate(length(i.tc10.xyz)*0.05-i.tc13.w*0.5);
    float near=saturate(i.tc9.w/max(cb1[204].x,0.01));
    float opacity=k*(near-fade)+fade;
    float floorAlpha=max(floorAndStrength.x,0.7);
    clip(opacity-floorAlpha);
    float3 seg=i.tc5.xyz-i.tc16.xyz; clip(length(seg)-0.01);
    float3 e=i.tc18-i.tc5.xyz;
    float t=dot(e,-seg)/(seg.x*seg.x);
    float dist=t<0.0?length(e):t>1.0?length(i.tc18-i.tc16.xyz):length(e+t*seg);
    bool screen=i.tc2.w>0.5;
    float nd=screen?i.tc17.x*2.0-1.0:saturate(dist/i.tc13.w);
    float q=nd*nd; clip(1.0-q);
    float3 ray=normalize(i.tc18-i.tc0.xyz);
    float radius=max(i.tc16.w,0.176809);
    float halfSpan=max(sqrt(1.0-q)*i.tc13.w/radius,0.000001);
    float3 origin=i.tc18-ray*(i.tc17.y/radius);
    float start=-halfSpan, finish=halfSpan;
    if(screen) {
        float distance=length(origin-i.tc0.xyz);
        start=max(i.tc10.w-distance,-halfSpan);
        finish=min(i.tc1.w-distance,halfSpan);
    }
    clip(finish-start);
    int steps=asint(cb1[203].w); clip(float(steps)-0.5);
    float step=(finish-start)/float(steps);
    float strength=min(sqrt(i.tc5.w),1.0);
    strength=min(strength*strength*(3.0-2.0*strength),1.0);
    strength*=i.tc0.w*0.2*lerp(cb1[203].x,cb1[202].w,i.tc2.w);
    strength=saturate(strength*3.333333);
    strength=strength*strength*(3.0-2.0*strength);
    float3 axis=normalize(-seg);
    float transmission=1.0, travel=start;
    // The original opacity march, including its live noise table. A
    // geometric upper bound marks pixels whose real opacity may be zero.
    [loop] for(int n=0;n<steps;++n) {
        float3 p=ray*travel+origin;
        float3 offset=axis*length(i.tc13.xyz-p)*0.1;
        float noise=hudNoise(p*0.25+offset)+0.5*hudNoise(p*0.5+offset)+0.25*hudNoise(p+offset);
        float density=saturate(noise*0.571429-(travel/halfSpan)*(travel/halfSpan)-q);
        density=density*density*(3.0-2.0*density);
        transmission*=exp2(-density*strength*step);
        travel+=step;
    }
    clip(opacity*(1.0-transmission)-floorAlpha);
    // The resolved depth and clip W support the game's own occlusion test.
    // They need not share the temporal pass's metre encoding. Preserve the
    // actual device depth under floating strokes, without re-encoding it.
    bool attached=i.tc1.z*1.5>=sceneZ;
    oDepth=attached?i.pos.z:SceneDeviceDepth.Load(int3(int2(i.pos.xy),0));
    return attached?floorAndStrength.y:floorAndStrength.w;
}
)HLSL";

constexpr char kHoloResolveVsHlsl[] =
    "float4 main(uint id : SV_VertexID) : SV_POSITION {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";

constexpr char kHoloResolvePsHlsl[] =
    "Texture2D<float4> Contribution : register(t0);\n"
    "Texture2D<float> ElementDepth : register(t1);\n"
    "Texture2D<float4> Target : register(t2);\n"
    "Texture2D<float4> Display : register(t3);\n"
    "Texture2D<float> NearLight : register(t4);\n"
    "Texture2D<float> UiMask : register(t5);\n"
    "cbuffer HoloResolveCB : register(b0) { float floorValue; float share; uint flags; float radiusDepth;\n"
    "                                       float fillerDepth; float pad0; float pad1; float pad2; };\n"
    "float3 srgbEncode(float3 c) { c = saturate(c); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0/2.4) - 0.055; }\n"
    "void main(float4 pos : SV_POSITION, out float depth : SV_Depth) {\n"
    "    int3 p = int3(int2(pos.xy), 0);\n"
    "    if ((flags & 16u) != 0u) {\n"
    "        uint mv = uint(UiMask.Load(p) * 255.0 + 0.5);\n"
    "        if ((mv & 1u) != 0u) discard;\n"
    "    }\n"
    "    float d = ElementDepth.Load(p);\n"
    "    if (!(d > 0.0)) discard;\n"
    "    float3 e = max(Contribution.Load(p).rgb, 0.0);\n"
    "    bool linearBlend = (flags & 1u) != 0;\n"
    "    bool haveTarget = (flags & 2u) != 0;\n"
    "    bool haveDisplay = (flags & 4u) != 0;\n"
    "    bool displaySrgb = (flags & 8u) != 0;\n"
    "    float3 dDisplay;\n"
    "    if (haveDisplay) {\n"
    "        float3 disp = Display.Load(p).rgb;\n"
    "        dDisplay = displaySrgb ? srgbEncode(disp) : saturate(disp);\n"
    "    } else {\n"
    "        dDisplay = linearBlend ? srgbEncode(e) : saturate(e);\n"
    "    }\n"
    "    bool dark = max(max(dDisplay.r, dDisplay.g), dDisplay.b) <= floorValue;\n"
    "    bool cockpitRange = d > radiusDepth;\n"
    "    if (dark) {\n"
    "        if (!cockpitRange) discard;\n"
    "        uint nw, nh; NearLight.GetDimensions(nw, nh);\n"
    "        int2 block = int2(pos.xy) / 8;\n"
    "        bool near = false;\n"
    "        [unroll] for (int by = -1; by <= 1; ++by)\n"
    "        [unroll] for (int bx = -1; bx <= 1; ++bx) {\n"
    "            int2 nb = clamp(block + int2(bx, by), int2(0, 0), int2(nw, nh) - 1);\n"
    "            if (NearLight.Load(int3(nb, 0)) > 0.5) near = true;\n"
    "        }\n"
    "        if (!near) discard;\n"
    "        depth = fillerDepth;\n"
    "    } else {\n"
    "        if (haveTarget) {\n"
    "            float3 f = max(Target.Load(p).rgb, 0.0);\n"
    "            float lumaE = dot(e, float3(0.299, 0.587, 0.114));\n"
    "            float lumaF = dot(f, float3(0.299, 0.587, 0.114));\n"
    "            if (lumaE < share * lumaF) discard;\n"
    "        }\n"
    "        depth = d;\n"
    "    }\n"
    "}\n";

constexpr char kHoloNearLightCsHlsl[] =
    "Texture2D<float4> Contribution : register(t0);\n"
    "Texture2D<float> ElementDepth : register(t1);\n"
    "Texture2D<float4> Target : register(t2);\n"
    "Texture2D<float4> Display : register(t3);\n"
    "RWTexture2D<float> NearLight : register(u0);\n"
    "cbuffer HoloResolveCB : register(b0) { float floorValue; float share; uint flags; float radiusDepth;\n"
    "                                       float fillerDepth; float pad0; float pad1; float pad2; }\n"
    "float3 srgbEncode(float3 c) { c = saturate(c); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0/2.4) - 0.055; }\n"
    "groupshared uint lightAny;\n"
    "[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID, uint3 tid : SV_GroupThreadID, uint3 gid : SV_GroupID) {\n"
    "    if (all(tid.xy == uint2(0, 0))) lightAny = 0;\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    bool linearBlend = (flags & 1u) != 0;\n"
    "    bool haveTarget = (flags & 2u) != 0;\n"
    "    bool haveDisplay = (flags & 4u) != 0;\n"
    "    bool displaySrgb = (flags & 8u) != 0;\n"
    "    int3 p = int3(int2(id.xy), 0);\n"
    "    float d = ElementDepth.Load(p);\n"
    "    if (d > radiusDepth) {\n"
    "        float3 e = max(Contribution.Load(p).rgb, 0.0);\n"
    "        float3 dDisplay;\n"
    "        if (haveDisplay) {\n"
    "            float3 disp = Display.Load(p).rgb;\n"
    "            dDisplay = displaySrgb ? srgbEncode(disp) : saturate(disp);\n"
    "        } else {\n"
    "            dDisplay = linearBlend ? srgbEncode(e) : saturate(e);\n"
    "        }\n"
    "        if (max(max(dDisplay.r, dDisplay.g), dDisplay.b) > floorValue) {\n"
    "            bool ok = true;\n"
    "            if (haveTarget) {\n"
    "                float3 f = max(Target.Load(p).rgb, 0.0);\n"
    "                float lumaE = dot(e, float3(0.299, 0.587, 0.114));\n"
    "                float lumaF = dot(f, float3(0.299, 0.587, 0.114));\n"
    "                ok = lumaE >= share * lumaF;\n"
    "            }\n"
    "            if (ok) InterlockedOr(lightAny, 1u);\n"
    "        }\n"
    "    }\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    if (all(tid.xy == uint2(0, 0))) NearLight[gid.xy] = lightAny ? 1.0 : 0.0;\n"
    "}\n";

constexpr char kHoloMarkerReticleDepthPsHlsl[] =
    "cbuffer HoloMarkerDepthCb : register(b0) { float projA; float projB; float pad0; float pad1; };\n"
    "struct In { float4 tc6 : TEXCOORD6; float3 tc7 : TEXCOORD7; float4 pos : SV_Position; };\n"
    "void main(In i, out float depth : SV_Depth) {\n"
    "    depth = saturate(projA + projB / i.pos.w);\n"
    "}\n";

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

inline std::string screenMotionPsSource(const char* engineCore) { return std::string(engineCore)+kScreenMotionPs; }

constexpr char kWeaponIdentityCs[]=R"HLSL(
Buffer<uint> Index:register(t0);
struct Instance{uint4 row[21];};
StructuredBuffer<Instance> Pool:register(t1);
RWStructuredBuffer<uint4> Identity:register(u0);
[numthreads(1,1,1)]void main(){
    uint n,stride;Pool.GetDimensions(n,stride);uint id=Index[0];
    Identity[0]=id<n?uint4(Pool[id].row[0].x,Pool[id].row[1].w,1,0):0;
}
)HLSL";

constexpr char kWeaponMotionVs[]=R"HLSL(
Buffer<float4> Now:register(t0);
Buffer<float4> Before0:register(t1);
Buffer<float4> Before1:register(t2);
Buffer<float4> Before2:register(t3);
Buffer<float4> Before3:register(t4);
StructuredBuffer<uint4> Identity:register(t5);
StructuredBuffer<uint4> OldIdentity0:register(t6);
StructuredBuffer<uint4> OldIdentity1:register(t7);
StructuredBuffer<uint4> OldIdentity2:register(t8);
StructuredBuffer<uint4> OldIdentity3:register(t9);
cbuffer Settings:register(b0){float4 extent;}
struct O{float4 p:SV_Position;float4 old:TEXCOORD0;nointerpolation uint valid:TEXCOORD1;};
void candidate(float4 now,float4 old,uint4 identity,inout float4 selected,inout uint matches){
    // All measured packed families use an infinite reversed-Z projection:
    // clip Z is the projection near plane, invariant under camera/object
    // motion. Skeleton identity separates body/viewmodel even when aiming
    // makes their projections equal. Remaining duplicates are never guessed.
    if(Identity[0].z && identity.z && all(Identity[0].xy==identity.xy) && asuint(now.z)==asuint(old.z) && old.z>0 && all(isfinite(old))){selected=old;++matches;}
}
O main(uint id:SV_VertexID){
    O o;o.p=Now[id];o.old=0;uint matches=0;
    if(extent.z>0)candidate(o.p,Before0[id],OldIdentity0[0],o.old,matches);
    if(extent.z>1)candidate(o.p,Before1[id],OldIdentity1[0],o.old,matches);
    if(extent.z>2)candidate(o.p,Before2[id],OldIdentity2[0],o.old,matches);
    if(extent.z>3)candidate(o.p,Before3[id],OldIdentity3[0],o.old,matches);
    o.valid=matches==1;return o;
}
)HLSL";

constexpr char kWeaponMotionPs[]=R"HLSL(
cbuffer Settings:register(b0){float4 extent;}
float4 main(float4 pos:SV_Position,float4 old:TEXCOORD0,nointerpolation uint valid:TEXCOORD1):SV_Target {
    if(!valid || old.w<=0 || !all(isfinite(old)))return float4(0,0,pos.z,2);
    float2 previous=(old.xy/old.w*float2(.5,-.5)+.5)*extent.xy;
    if(any(previous<0) || any(previous>extent.xy))return float4(0,0,pos.z,2);
    return float4(previous-pos.xy,pos.z,1);
}
)HLSL";

constexpr char kCelestialBuildHlsl[] = R"HLSL(
struct Record { uint4 key[12]; float4 q; float4 t; float4 r[3]; };
// Per-draw snapshot, 26 float4's per record. In is a typed Buffer, not a
// StructuredBuffer (see createRecords for why), so there is no struct to
// declare here -- just the layout: model = In[index*26+0 .. +7] (8),
// scene = In[index*26+8 .. +11] (4), patch = In[index*26+12 .. +25] (14).
cbuffer Batch : register(b0) { uint4 batch; }
StructuredBuffer<Record> Previous : register(t0);
Buffer<float4> In : register(t1);
Buffer<uint4> Keys : register(t2);
RWStructuredBuffer<Record> Current : register(u0);
float3 rotate(float4 q, float3 v) { return v + 2 * cross(q.xyz, cross(q.xyz,v) + q.w*v); }
float4 multiply(float4 a, float4 b) {
    return float4(a.w*b.xyz + b.w*a.xyz + cross(a.xyz,b.xyz), a.w*b.w-dot(a.xyz,b.xyz));
}
// A local-space eye can contain dozens of patches. Serial comparison of
// every 192-byte key stalls one GPU lane for milliseconds across the eye.
// Search independent predecessors in parallel, retaining exact full keys
// and the unique-match rule (including duplicates in different lanes).
groupshared uint matchCounts[64],matchIndices[64];
[numthreads(64,1,1)] void main(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex) {
    const uint index = batch.x + gid.x; const uint base = index*26;
    Record n = (Record)0;
    n.key[0] = asuint(In[base+12]);
    [unroll] for (uint k=0;k<5;++k) n.key[k+1] = asuint(In[base+15+k]);
    [unroll] for (uint k=0;k<4;++k) n.key[k+6] = asuint(In[base+22+k]);
    n.key[10] = Keys[index*2]; n.key[11] = Keys[index*2+1];
    n.q = normalize(In[base+20]); n.t = float4(In[base+13].xyz,0);
    // Confirm this shader's local-to-clip chain still reduces to view-space
    // perspective: scene[270..273] * model[9..11] == model[4..7].
    bool valid = all(isfinite(n.q)) && all(isfinite(n.t)) &&
        abs(dot(In[base+20],In[base+20])-1) < 0.002 && In[base+2].w > 0 &&
        abs(In[base+3].z-1) < 0.0001 && abs(In[base+2].z) < 0.0001;
    [unroll] for (uint c=0;c<4;++c) {
        float4 col = In[base+8]*In[base+5][c] + In[base+9]*In[base+6][c] +
                     In[base+10]*In[base+7][c] + (c==3 ? In[base+11] : 0);
        float4 expected = float4(In[base+0][c],In[base+1][c],In[base+2][c],In[base+3][c]);
        valid = valid && all(abs(col-expected) < 0.0002);
    }
    uint found=0, match=0;
    [loop] for (uint i=lane;i<batch.y;i+=64) {
        bool same=true;
        [unroll] for (uint k=0;k<12;++k) same = same && all(n.key[k]==Previous[i].key[k]);
        if (same) { ++found; match=i; }
    }
    matchCounts[lane]=found;matchIndices[lane]=match;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step=32;step;step>>=1) {
        if (lane<step) {
            matchCounts[lane]+=matchCounts[lane+step];
            matchIndices[lane]=max(matchIndices[lane],matchIndices[lane+step]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane!=0) return;
    found=matchCounts[0];match=matchIndices[0];
    if (valid && found==1) {
        Record p=Previous[match];
        float4 dq=normalize(multiply(p.q,float4(-n.q.xyz,n.q.w)));
        float3 a=rotate(dq,float3(1,0,0)), b=rotate(dq,float3(0,1,0)), c=rotate(dq,float3(0,0,1));
        float3 t=p.t.xyz-rotate(dq,n.t.xyz);
        n.r[0]=float4(a.x,b.x,c.x,t.x);
        n.r[1]=float4(a.y,b.y,c.y,t.y);
        n.r[2]=float4(a.z,b.z,c.z,t.z);
        n.t.w=all(isfinite(t)) && all(isfinite(dq)) && abs(dot(p.q,p.q)-1)<0.002 ? 1 : 0;
    }
    // Invalid current geometry must not become a valid predecessor.
    if (!valid) n.q=0;
    Current[index]=n;
}
)HLSL";

constexpr char kCelestialIndexHlsl[] = R"HLSL(
cbuffer Draw : register(b13) { uint4 info; uint4 texKey[2]; }
uint main(float4 p:SV_Position):SV_Target { return info.x+1; }
struct Coverage { uint index:SV_Target0; float depth:SV_Target1; };
Coverage original(float4 p:SV_Position) {
    Coverage o; o.index=info.x+1; o.depth=p.z; return o;
}
)HLSL";

constexpr char kPlanetCoverageHlsl[]=R"HLSL(
cbuffer Motion:register(b12){uint4 info;}
float2 main(float4 p:SV_Position):SV_Target{return float2(info.x+1,p.z);}
)HLSL";

constexpr char kFoveaCsHlsl[] = R"HLSL(
Texture2D<float4> PERIPH : register(t0);   // the periphery: NVIDIA's reduced DLAA or the own history, any size
Texture2D<float4> FOVEA  : register(t1);   // NVIDIA's output, native size, valid in the crop
RWTexture2D<float4> FO   : register(u0);   // the composited native frame, game format
RWTexture2D<float4> HIST : register(u1);   // the own history being written this frame (mode.y)
SamplerState SMP : register(s0);           // bilinear clamp, for the periphery upscale
cbuffer FC : register(b0) {
    float4 crop;   // output crop x0 y0 x1 y1 in native pixels, x1/y1 exclusive
    float4 band;   // x the blend band in pixels; y the output width, z the output height; w the periphery's width
    float4 disc;   // xy the fovea's centre in native pixels, zw its half-extents (the ellipse's semi-axes)
    float4 mode;   // x 1 = round fovea (else the rectangle); y 1 = write the history too; z 1 = the periphery is smaller than the output (bicubic); w the periphery's height
};
// Catmull-Rom through nine bilinear fetches (the pass's own kernel, C = 0.5),
// for a periphery smaller than the output: sharper than the bilinear
// upscale, mild ringing, the standard upscaling kernel.
float4 periphCubic(float2 uv, float2 tsize) {
    const float C = 0.5;
    float2 sp = uv * tsize;
    float2 t1 = floor(sp - 0.5) + 0.5;
    float2 f = sp - t1;
    float2 g0 = 1.0 + f;
    float2 g3 = 2.0 - f;
    float2 w0 = C * (-g0 * g0 * g0 + 5.0 * g0 * g0 - 8.0 * g0 + 4.0);
    float2 w1 = (2.0 - C) * f * f * f + (C - 3.0) * f * f + 1.0;
    float2 h = 1.0 - f;
    float2 w2 = (2.0 - C) * h * h * h + (C - 3.0) * h * h + 1.0;
    float2 w3 = C * (-g3 * g3 * g3 + 5.0 * g3 * g3 - 8.0 * g3 + 4.0);
    float2 w12 = w1 + w2;
    float2 o12 = w2 / w12;
    float2 t0 = (t1 - 1.0) / tsize;
    float2 t3 = (t1 + 2.0) / tsize;
    float2 t12 = (t1 + o12) / tsize;
    float4 r = 0.0;
    r += PERIPH.SampleLevel(SMP, float2(t0.x, t0.y), 0) * w0.x * w0.y;
    r += PERIPH.SampleLevel(SMP, float2(t12.x, t0.y), 0) * w12.x * w0.y;
    r += PERIPH.SampleLevel(SMP, float2(t3.x, t0.y), 0) * w3.x * w0.y;
    r += PERIPH.SampleLevel(SMP, float2(t0.x, t12.y), 0) * w0.x * w12.y;
    r += PERIPH.SampleLevel(SMP, float2(t12.x, t12.y), 0) * w12.x * w12.y;
    r += PERIPH.SampleLevel(SMP, float2(t3.x, t12.y), 0) * w3.x * w12.y;
    r += PERIPH.SampleLevel(SMP, float2(t0.x, t3.y), 0) * w0.x * w3.y;
    r += PERIPH.SampleLevel(SMP, float2(t12.x, t3.y), 0) * w12.x * w3.y;
    r += PERIPH.SampleLevel(SMP, float2(t3.x, t3.y), 0) * w3.x * w3.y;
    return r;
}
[numthreads(8, 8, 1)]
void fovea(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)band.y || id.y >= (uint)band.z) return;
    int2 p = int2(id.xy);
    float b = max(band.x, 1.0);
    float w;
    if (mode.x != 0.0) {
        // The disc: the normalised radius in the ellipse inscribed in the
        // crop, turned back into pixels inside its edge along the minor axis.
        float2 n = (float2(p) + 0.5 - disc.xy) / max(disc.zw, 1.0);
        float d = (1.0 - length(n)) * min(disc.z, disc.w);
        w = saturate(d / b);
    } else {
        float dx = min((float)p.x - crop.x, crop.z - 1.0 - (float)p.x);
        float dy = min((float)p.y - crop.y, crop.w - 1.0 - (float)p.y);
        w = saturate(min(dx, dy) / b);     // pixels inside the crop's nearest edge
    }
    w = w * w * (3.0 - 2.0 * w);           // smoothstep across the band
    // The periphery is sampled by normalised uv, so any size lands on the
    // native output: bicubic when it is smaller, an exact fetch at 1:1 (the
    // uv lands on texel centres). The fovea is native, read where it is
    // valid (strictly inside the fovea, where w > 0).
    float2 uv = (float2(p) + 0.5) / float2(band.y, band.z);
    float4 per = mode.z != 0.0 ? periphCubic(uv, float2(band.w, mode.w))
                               : PERIPH.SampleLevel(SMP, uv, 0);
    float4 fov = w > 0.0 ? FOVEA.Load(int3(p, 0)) : per;
    float4 o = lerp(per, fov, w);
    FO[p] = o;
    // The hand-off into the own history (mode.y, the sharp periphery at 1:1):
    // inside the fovea and its band the history takes the blended result.
    if (mode.y != 0.0 && w > 0.0) HIST[p] = float4(o.rgb, 1.0);
}
)HLSL";

constexpr char kDownCsHlsl[] = R"HLSL(
Texture2D<float4> DC : register(t0);    // the colour, render size
Texture2D<float>  DZ : register(t1);    // the depth copy, render size
Texture2D<float2> DM : register(t2);    // the motion vectors, render pixels
RWTexture2D<float4> PC : register(u0);  // the reduced colour
RWTexture2D<float>  PZ : register(u1);  // the reduced depth
RWTexture2D<float2> PM : register(u2);  // the reduced motion, reduced pixels
cbuffer DS : register(b0) {
    float4 dims;   // x reduced width, y reduced height, z render width, w render height
    float4 par;    // x unused (was the block side), y the scale (reduced / render), zw unused
};
[numthreads(8, 8, 1)]
void down(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)dims.x || id.y >= (uint)dims.y) return;
    float2 q = dims.zw / dims.xy;                    // render pixels per reduced pixel, > 1
    float2 x0 = float2(id.xy) * q;                   // the footprint [x0, x1) in render pixels
    float2 x1 = x0 + q;
    int2 k0 = int2(floor(x0));
    int2 k1 = min(int2(ceil(x1)), int2(dims.zw));    // exclusive
    float4 c = 0.0;
    float wsum = 0.0;
    float zmax = -1.0;
    float2 mv = 0.0;
    [loop] for (int y = k0.y; y < k1.y; ++y) {
        float wy = min((float)y + 1.0, x1.y) - max((float)y, x0.y);
        [loop] for (int x = k0.x; x < k1.x; ++x) {
            float wx = min((float)x + 1.0, x1.x) - max((float)x, x0.x);
            float w = wx * wy;
            if (w <= 0.0) continue;
            int2 s = int2(x, y);
            c += DC.Load(int3(s, 0)) * w;
            wsum += w;
            float z = DZ.Load(int3(s, 0));
            if (z > zmax) {
                zmax = z;
                mv = DM.Load(int3(s, 0));
            }
        }
    }
    PC[id.xy] = c / max(wsum, 1e-6);
    PZ[id.xy] = max(zmax, 0.0);
    PM[id.xy] = mv * par.y;
}
)HLSL";

} // namespace edvr
