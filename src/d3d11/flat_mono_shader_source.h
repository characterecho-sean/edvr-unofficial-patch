#pragma once
namespace edvr {
// Prepend the generated kEngineMotionCoreHlsl: rigid-record arithmetic is shared
// verbatim with VR. This source has no eye, headset or panel globals.
inline constexpr char kFlatMonoShaderSource[] = R"HLSL(
cbuffer Mono : register(b0) {
    float4 now[6]; float4 old[6];
    uint4 size; // render width/height, output width/height
    uint4 flags; // reset, complete engine views, TAA, stale-slot policy (w: 0 refuse, 1 camera term for every stale slot, only ever set in the
                 // 3D main menu, 2 camera term for a stale slot where last frame's depth (t8) confirms it, the steady-detail key)
    float4 jitter; // current xy, previous zw; actual raster phase in render pixels
    float4 rowsJitter; // NDC shift the camera rows themselves carry: current xy, previous zw; all zero = unjittered rows
    uint4 route; // x: the HDR route (section 81): Color is R11G11B10F scene radiance and OutColor is fp16; y: with x, the
                 // TAA output is final and the pixel-shader finish only copies it into H. z: the first-person map (t9) and
                 // stencil (t10) are bound and valid (section 82, prep only); w: with z, the first-person phase mode (0 the map's
                 // vector as given, 1 the two phases' difference is added to it, any other value rejects attached pixels' history).
                 // All zero on the copy route without them.
    uint4 debug; // x: this frame samples the refusal census (prep writes the class texture, the census kernel counts it), y: this frame
                 // paints the refusal view (prep writes the class texture, the HDR finish paints from it). Both zero on every frame that
                 // asks for neither, and the flat profile never asks: the prep's arithmetic and its outputs are then what they were.
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
Texture2D<float> HistoryDepth : register(t8);         // taa's: last frame's depth; and the prep's, bound only when flags.w==2 (the steady-detail depth check)
Texture2D<float4> FirstPersonMotion : register(t9);   // prep only, bound when route.z != 0: the VR weapon map, render size,
                                                      // xy previous minus current in render pixels, z depth, w 1 valid / 2 new / 0 none
Texture2D<uint2> FirstPersonStencil : register(t10);  // prep only, bound when route.z != 0: the depth texture's stencil plane (.y)
Texture2D<uint> ClassMap : register(t11);             // the census kernel's and the HDR finish's, bound only when debug.x or debug.y: the prep's class per pixel
SamplerState LinearClamp : register(s0);
RWTexture2D<float> OutDepth : register(u0);
RWTexture2D<float2> OutMotion : register(u1);
RWTexture2D<float> OutRejection : register(u2);
RWTexture2D<float> OutExpected : register(u3);
RWTexture2D<float4> OutColor : register(u4);
RWTexture2D<uint> OutClass : register(u5);            // prep only, bound when debug.x or debug.y: what the pixel is and whether its history was refused
RWByteAddressBuffer RefusalCounts : register(u6);     // the census kernel's: 16 stripes of 16 counters (flat_mono_refusal.h)

// The pixel classes (flat_mono_refusal.h kFlatMonoClass*, which tools\flat_mono_resolve_test holds these to). Bit 7 of the byte the prep
// writes says its history was refused.
static const uint kClassNone=0, kClassJoined=1, kClassMasked=2, kClassNotRig=3, kClassStale=4, kClassCorrupt=5, kClassStaleStamp=6,
    kClassSentinel=7, kClassUnreprojectable=8, kClassCamera=9, kClassRange=10, kClassDepth=11, kClassWeapon=12,
    kClassWeaponRefused=13, kClassReset=14;

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
// THE STEADY-DETAIL DEPTH CHECK (flags.w==2; design doc section 82, the depth-validated steady detail). A stale slot's pixel is an
// overdrawn one: its record says nothing about it, so the camera term is the only motion it has, and the camera term is right only
// when the surface did not move. Last frame's depth says whether it did not: the surface that was at the position the camera term sends
// this pixel to, in last frame's own raster (that position plus the previous phase), must be this surface, i.e. its depth must be the
// one this surface would have had there had it not moved (`expected`, the camera term's own). Depth is reversed-Z, float32, d = near/z,
// so a relative error in d is the same relative error in z at any range and a relative tolerance is the right form; the tolerance is the
// 1% (floor 1e-6) the resolver's own TAA applies before it trusts history (taa(), below), against the best of the four texels around the
// previous raster position: a thin line the jitter put on the neighbouring texel last frame still finds itself, and a static slanted
// surface is within half a texel's gradient of one of them (tools\flat_mono_resolve_test holds the numbers; the simulation is in the
// design doc). True = confirmed; false = refused, as a stale slot always was. Called for a stale pixel whose camera term was already
// formed and in range, and only then.
static const float kStaleDepthRel=.01, kStaleDepthFloor=1e-6;
bool stalePreviousDepthMatches(float2 prev,float expected) {
    const int2 hi=int2(size.xy)-1;
    const float2 raster=prev*float2(size.xy)+jitter.zw-.5;   // the previous raster position in texel-centre coordinates: the 2x2 below surrounds it
    const int2 base=int2(floor(raster));
    const float tol=max(kStaleDepthFloor,expected*kStaleDepthRel);
    float best=3.402823e38;
    [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)best=min(best,abs(HistoryDepth.Load(int3(clamp(base+int2(x,y),0,hi),0))-expected));
    return best<=tol;
}
// 0 = camera term, 1 = exact engine motion, 2 = explicitly reject history, 3 = the camera term if the depth check confirms it (a stale
// slot with flags.w==2). `cls` is what the pixel is (kClass*): the refusal census and view read it, nothing else does, and no return
// below depends on it.
uint engineBefore(int2 q,float2 uv,float depth,out float4 before,out uint cls) {
    before=0; cls=kClassNone;
    if(flags.y==0)return 0;
    float2 es=Slots.Load(int3(q,0));
    if(!(es.x>=1))return 0;
    if(!(depth>0) || es.x>=4294967296.0){cls=kClassSentinel;return 2;}
    // A slot written by a keyed draw that something has since drawn over: the pixel's owner is not the
    // one that wrote the slot, so its record says nothing about it. By default (flags.w==0) that is not
    // knowable and history is refused. In the 3D main menu (flags.w==1, set by the runtime only for a frame
    // whose contract came through the verified menu copy) nothing on screen moves but the camera, so the
    // pixel takes the camera term -- an unkeyed hull that overdraws a keyed one no longer aliases. With
    // experimental.temporal_aa_on_foot_world_steady_detail on (flags.w==2: the VR world route and the flat
    // profile on foot) the pixel takes the camera term only where last frame's depth confirms it (return 3: the
    // caller asks stalePreviousDepthMatches) and is refused everywhere else, as by default. Only this refusal is
    // relaxed, either way: a masked record stays refused.
    if(asuint(depth)!=asuint(es.y)){cls=kClassStale;return flags.w==1?0:(flags.w==2?3:2);}
    uint code=uint(es.x);
    if(float(code)!=es.x || (code&1)==0){cls=kClassCorrupt;return 2;}
    uint count,stride; Pool.GetDimensions(count,stride);
    uint slot=code>>1;
    if(slot>=count || stride!=336){cls=kClassCorrupt;return 2;}
    EnginePoolRecord r=Pool[slot];
    uint kind=engineRecordKind(r,asuint(EN[276].x));
    if(kind==2){cls=kClassMasked;return 2;}
    if(kind==3)kind=engineStaleStampKind(r,asuint(EN[276].x));
    if(kind==2){cls=kClassMasked;return 2;}   // an older masked marker keeps no history, as masked always did
    if(kind!=1){cls=kind==6?kClassStaleStamp:kClassNotRig;return 0;}   // not a rig record, or an older joined marker: the camera term
    // Freshness: a joined marker certifies only at the frame it was written.
    if(r.data[18].x!=(0x7FC0ED01u^engineMarkerHash(r,asuint(EN[276].x)))){cls=kClassStaleStamp;return 0;}
    cls=kClassJoined;
    // The engine's own scene snapshots are the game's b1 upload too: same phase as the rows above, the
    // before-snapshot's being the previous frame's.
    if(!engineReprojectRows(r,uv*float2(2,-2)+float2(-1,1),depth,
        unjitterRow(EN[270],rowsJitter.xy),unjitterRow(EN[271],rowsJitter.xy),
        unjitterRow(EN[272],rowsJitter.xy),unjitterRow(EN[273],rowsJitter.xy),EN[275].xyz,
        unjitterRow(EB[270],rowsJitter.zw),unjitterRow(EB[271],rowsJitter.zw),
        unjitterRow(EB[272],rowsJitter.zw),unjitterRow(EB[273],rowsJitter.zw),EB[275].xyz,before)) {
        if(engineRecordMoved(r)){cls=kClassUnreprojectable;return 2;}
        return 0;
    }
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
    // What the pixel is (kClass*), for the refusal census and view alone: nothing below reads it back. Until a check says
    // otherwise a pixel the checks below do not reach is a reset frame's, or one whose own depth is no depth.
    uint cls=flags.x!=0?kClassReset:kClassDepth;
    // First-person pixels (section 82). The depth texture's stencil bit 0x10 marks what the first-person draws wrote, and the
    // VR weapon map says, per texel, where that surface was a frame ago. Such a pixel takes the map's motion or rejects its
    // history, and NEVER the engine or camera term below: the weapon is not world geometry, and the camera term at its depth
    // and field of view would be wrong (a screen-fixed weapon ghosting in turns). Without the inputs route.z is zero and both
    // views are unbound, so no pixel is attached and the branch below is the code that was here before.
    bool attached=route.z!=0 && (FirstPersonStencil.Load(int3(q,0)).y&16)!=0;
    if(attached) {
        float4 m=FirstPersonMotion.Load(int3(q,0));
        // The previous position in render pixels. Leaving the frame is disocclusion, not something to extrapolate.
        float2 prevPx=float2(q)+.5+m.xy;
        // The map's depth is fp16, within half an ulp (a relative 2^-11) of the surface; the .0005 covers that. A missing,
        // occluded, new or ambiguous mesh (w != 1) must not borrow another surface's history, and a reset frame has none.
        bool valid=flags.x==0 && m.w==1 && all(isfinite(m)) && abs(m.z-depth)<=max(abs(depth)*.0005,3e-8) &&
                   all(prevPx>=0) && all(prevPx<=float2(size.xy));
        // THE PHASE of a jittered world (route.w, FlatMonoResolveFrame::firstPersonPhaseMode). A surface at true (unjittered)
        // positions P_cur now and P_prev a frame ago is RASTERISED at P_cur + c and P_prev + p, c the current phase (jitter.xy)
        // and p the previous one (jitter.zw), so the map holds m.xy = (P_prev + p) - (P_cur + c) = (P_prev - P_cur) + (p - c).
        // The backend wants the true motion, P_prev - P_cur, both phases out of it as the camera and engine terms have them
        // out: m.xy + (c - p). Mode 0 leaves m.xy exactly as given (the world is unjittered, or the map was built without the
        // phase) and its arithmetic is what it was before the mode existed. Mode 1 (the first-person camera carried the
        // world's phase in both frames) adds the term. Any other value means that is not known, and history is refused as for
        // an invalid texel. The validity test above runs on the UNcorrected m: prevPx is a position in the previous RASTER,
        // and that is where the map says the surface was drawn. The sign is proven against a map built from explicit
        // positions (tools\flat_mono_resolve_test, flat_first_person_phase_gpu_tests.h).
        if(route.w==1)m.xy+=jitter.xy-jitter.zw; else if(route.w!=0)valid=false;
        motion=valid?m.xy:0; reject=valid?0:1; expected=valid?depth:0;
        cls=valid?kClassWeapon:kClassWeaponRefused;
    } else if(flags.x==0 && isfinite(depth) && depth>=0 && depth<=1) {
        float4 before;
        uint kind=engineBefore(q,rawUv,depth,before,cls);
        // HLSL logical operators do not short-circuit: putting cameraBefore's
        // out parameter in || would overwrite the exact engine result.
        bool valid=kind==1;
        if(kind==0||kind==3)valid=cameraBefore(rawUv,depth,before);
        if(valid) {
            float2 prev=before.xy/before.w*float2(.5,-.5)+.5;
            // SDK vectors exclude both raster phases; the backend receives
            // the actual current phase separately and tracks its own history.
            motion=(prev-rawUv)*float2(size.xy);
            expected=before.z/before.w;
            valid=all(isfinite(motion)) && all(abs(motion)<=65504) &&
                  all(prev>=0) && all(prev<=1) && isfinite(expected) && expected>=0 && expected<=1;
            if(!valid)cls=kClassRange;
            // A stale slot under the steady-detail rule (kind 3): the camera term stands only if last frame's depth confirms it. A pixel
            // the check refuses stays a stale-slot pixel (cls kClassStale, refused): the census counts it as stale-refused.
            if(valid && kind==3)valid=stalePreviousDepthMatches(prev,expected);
            reject=valid?0:1;
        } else if(kind!=2)cls=kClassCamera;
    }
    if(reject!=0)motion=0;
    OutDepth[q]=isfinite(depth)?saturate(depth):0;
    OutMotion[q]=motion; OutRejection[q]=reject;
    if(flags.z!=0)OutExpected[q]=expected;
    // The refusal census and view: one byte, the class and (bit 7) whether this pixel's history was refused.
    if(debug.x!=0 || debug.y!=0)OutClass[q]=cls|(reject!=0?0x80u:0u);
}
)HLSL"
R"HLSL(
// The refusal census (flat_mono_refusal.h): one pass over the class texture the prep wrote, on a frame that sampled. Per group the
// refused classes are counted in shared memory, and each non-zero count is added to one of 16 stripes of the global buffer (spread by
// group column, so the atomics of 220 000 groups do not queue on sixteen addresses). Slot 15 counts a stale pixel the prep did NOT
// refuse (the steady-detail rule kept it: the camera term, confirmed by last frame's depth); a stale pixel it did refuse stays in the
// stale slot. Accepted pixels are not counted. No early return: every thread reaches both barriers.
groupshared uint gRefusal[16];
[numthreads(8,8,1)]
void census(uint3 id:SV_DispatchThreadID,uint3 gid:SV_GroupID,uint gi:SV_GroupIndex) {
    if(gi<16)gRefusal[gi]=0;
    GroupMemoryBarrierWithGroupSync();
    if(all(id.xy<size.xy)) {
        const uint v=ClassMap.Load(int3(id.xy,0));
        const uint kind=v&0x7Fu;
        if((v&0x80u)!=0)InterlockedAdd(gRefusal[min(kind,14u)],1u);
        else if(kind==kClassStale)InterlockedAdd(gRefusal[15],1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if(gi<16 && gRefusal[gi]!=0)RefusalCounts.InterlockedAdd(((gid.x&15u)*16u+gi)*4u,gRefusal[gi]);
}

// The HDR route accumulates in a bounded space, c / (1 + max3(c)), and inverts it on output: a sun disc or a hot
// particle in a linear 3x3 clamp would set the box and the blend alone and ring around every highlight. The history
// stays linear fp16, so a route flip needs no conversion and the tone pass sees linear radiance again.
float3 hdrCompress(float3 c) { c=max(c,0); return c/(1+max(c.r,max(c.g,c.b))); }
float3 hdrExpand(float3 c) { float m=max(c.r,max(c.g,c.b)); return min(c/max(1-m,1.0/65504.0),65504); }
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
    bool hdr=route.x!=0;
    float3 cur=hdr?hdrCompress(current.rgb):current.rgb;
    float3 lo=cur,hi=cur;
    [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x) {
        float3 value=Color.Load(int3(clamp(q+int2(x,y),0,int2(size.xy)-1),0)).rgb;
        if(hdr)value=hdrCompress(value);
        lo=min(lo,value);hi=max(hi,value);
    }
    float3 history=History.SampleLevel(LinearClamp,previous,0).rgb;
    if(hdr)history=hdrCompress(history);
    float3 mixed=lerp(cur,clamp(history,lo,hi),weight);
    OutColor[id.xy]=float4(hdr?hdrExpand(mixed):mixed,current.a);
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
// The HDR route's pixel-shader half (section 81). The result goes back into H, a render target that need not have an
// unordered-access view, so the compute finish cannot carry it: one triangle over the render-size target, the
// finish or the spatial recovery as a pixel shader. R11G11B10_FLOAT holds no NaN, no negative, at most 65024 in red
// and green (6-bit mantissas) and 64512 in blue (5 bits); the game's own tone pass sees a value the format can
// represent, which is what requantisation to its precision means here.
float hdrSafe(float v,float top) { return isnan(v)?0:clamp(v,0,top); }
float3 hdrRepresentable(float3 c) { return float3(hdrSafe(c.x,65024),hdrSafe(c.y,65024),hdrSafe(c.z,64512)); }
float4 hdrVs(uint id:SV_VertexID):SV_Position {
    float2 p=float2((id<<1)&2,id&2);
    return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
// The refusal view (advanced.temporal_aa_debug = motion_source on the VR world route; debug.y): the prep's class, painted in the eye
// path's colours (temporal_shader_source.h, the motion_source view). The picture is H, which the game's tone pass reads next, so each
// colour is scaled by the pixel's own level (twice its luma, never below .04): the hue survives the tone pass, the absolute value
// does not. green 1 joined, red 2 masked, blue 3 not a rig record, yellow 4 stale slot, magenta 5 corrupt, orange 6 stale stamp,
// cyan 12 first-person, white any other refusal, and a pixel with no engine slot is dimmed to a quarter.
float3 refusalPaint(float3 c,uint v) {
    const uint kind=v&0x7Fu;
    const float y=max(dot(c,float3(.2126,.7152,.0722)),.02)*2;
    return kind==kClassJoined?float3(0,y,0):kind==kClassMasked?float3(y,0,0)
         :kind==kClassNotRig?float3(0,.3*y,y):kind==kClassStale?float3(y,y,0)
         :kind==kClassCorrupt?float3(y,0,y):kind==kClassStaleStamp?float3(y,.5*y,0)
         :kind==kClassWeapon?float3(0,y,y):(kind>=kClassSentinel && kind<=kClassWeaponRefused)?float3(y,y,y):c*.25;
}
// The class an output pixel is painted by: a refused one among the four raster texels under its +jitter sample (those are the pixels
// the finish shows raw, so the colour says why), else the nearest texel's.
uint refusalClassAt(float2 rasterUv) {
    const int2 hi=int2(size.xy)-1;
    const int2 q=int2(floor(rasterUv*float2(size.xy)-.5));
    uint best=0;
    [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x) {
        const uint v=ClassMap.Load(int3(clamp(q+int2(x,y),0,hi),0));
        if((v&0x80u)!=0)best=max(best,v);
    }
    return best!=0?best:ClassMap.Load(int3(clamp(int2(floor(rasterUv*float2(size.xy))),0,hi),0));
}
float4 finishHdr(float4 pos:SV_Position):SV_Target {
    int2 p=int2(pos.xy);
    float2 uv=pos.xy/float2(size.zw);
    float2 rasterUv=uv+jitter.xy/float2(size.xy);
    float3 c;
    if(route.y!=0) {
        // EDVR's TAA output is final (its own rejection ran inside it): a plain copy.
        c=History.Load(int3(p,0)).rgb;
    } else {
        int2 q=int2(floor(rasterUv*float2(size.xy)-.5));
        float reject=0;
        [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x)
            reject=max(reject,Rejection.Load(int3(clamp(q+int2(x,y),0,int2(size.xy)-1),0)));
        c=reject>0?Color.SampleLevel(LinearClamp,rasterUv,0).rgb:History.Load(int3(p,0)).rgb;
    }
    if(debug.y!=0)c=refusalPaint(c,refusalClassAt(rasterUv));
    return float4(hdrRepresentable(c),1);
}
float4 spatialHdr(float4 pos:SV_Position):SV_Target {
    float2 uv=pos.xy/float2(size.zw);
    return float4(hdrRepresentable(Color.SampleLevel(LinearClamp,uv+jitter.xy/float2(size.xy),0).rgb),1);
}
)HLSL";
} // namespace edvr
