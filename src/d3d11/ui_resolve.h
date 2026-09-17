#pragma once
namespace edvr {
// DLSS transformer presets ignore BiasCurrentColor. Bound UI reconstruction
// against the current raster before submission, independently of the model.
// One input-grid thread owns a disjoint rectangle of output pixels. The two
// existing UI-history textures carry influence until stale colour is gone;
// otherwise a model's multi-frame trail could outlive the raw coverage mask.
constexpr char kUiResolve[] = R"HLSL(
Texture2D<float4> Raw:register(t0);
Texture2D<float4> Trained:register(t1);
Texture2D<float> Coverage:register(t2);
Texture2D<float4> Previous:register(t3);
Texture2D<float2> Motion:register(t4);
Texture2D<float> Edits:register(t5);
Texture2D<float4> Screen:register(t6);
Texture2D<float4> FullCurrent:register(t7);
Texture2D<float> UiInfluence:register(t8);
RWTexture2D<float4> Output:register(u0);
RWTexture2D<float4> Next:register(u1);
cbuffer P:register(b0){int4 region;int2 size;int2 texSize;float4 tanNow;float4 tanPrev;float4 jit;}
cbuffer R:register(b1){float4 resolve;} // b1 = {ghost tolerance, hold brightness limit (0 = off), 0, 0}
bool marked(int2 p){uint k=uint(Coverage.Load(int3(clamp(p,0,size-1),0))*255+.5)&3u;return k==1u||k==2u;}
float4 cubic(float t){float t2=t*t,t3=t2*t;return float4(-.5*t+t2-.5*t3,1-2.5*t2+1.5*t3,.5*t+2*t2-1.5*t3,-.5*t2+.5*t3);}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){
    if(any(id.xy>=uint2(size)))return;
    int2 q=int2(id.xy),r=int2(floor(float2(q)+jit.xy+.5));
    uint screenW,screenH;Screen.GetDimensions(screenW,screenH);
    bool here=false,edited=false;
    [unroll] for(int y=-1;y<=1;++y)[unroll] for(int x=-1;x<=1;++x){
        int2 p=r+int2(x,y);here=here||marked(p);
        edited=edited||Edits.Load(int3(clamp(p,0,size-1),0))>0;
        if(screenW>0) {
            bool screenUi=Screen.Load(int3(region.xy+clamp(p,0,size-1),0)).w==3;
            here=here||screenUi;edited=edited||screenUi;
        }
    }
    float remaining=Previous.Load(int3(q,0)).a;
    // DLSS can transport an old glyph along the newly exposed world's
    // vector, outside both its current and stationary previous footprint.
    // Follow that same vector through the unjittered influence history.
    // A max over the four bilinear taps gives every fractional neighbour
    // full influence, so a tiny motion can grow a one-pixel halo every frame.
    // Interpolate the transported age, then retain the stationary sample as
    // a separate candidate. Off-screen history is invalid.
    if(!here && remaining<1){
        float2 before=float2(q)+Motion.Load(int3(clamp(r,0,size-1),0));
        if(all(isfinite(before)) && all(before>=0) && all(before<=float2(size-1))){
            int2 corner=int2(floor(before));
            int2 upper=int2(ceil(before));
            float2 f=saturate(before-float2(corner));
            float lower=lerp(Previous.Load(int3(corner,0)).a,
                             Previous.Load(int3(int2(upper.x,corner.y),0)).a,f.x);
            float upperRow=lerp(Previous.Load(int3(int2(corner.x,upper.y),0)).a,
                                Previous.Load(int3(upper,0)).a,f.x);
            remaining=max(remaining,lerp(lower,upperRow,f.y));
        }
    }
    bool active=here||edited||remaining>0;
    uint ow,oh;Output.GetDimensions(ow,oh);uint2 extent=uint2(ow,oh);
    uint2 begin=(id.xy*extent+uint2(size)-1)/uint2(size);
    uint2 end=((id.xy+1)*extent+uint2(size)-1)/uint2(size);
    bool stale=false;
    [loop] for(uint oy=begin.y;oy<end.y;++oy)[loop] for(uint ox=begin.x;ox<end.x;++ox){
        float4 v=Trained.Load(int3(ox,oy,0));
        if(active){
            // The four source texels around this output sample bound its
            // reconstruction. A whole 3x3 input neighbourhood admitted the
            // preceding letter into adjacent gaps on small changing text.
            float2 at=(float2(ox,oy)+.5)*float2(size)/float2(extent)-.5+jit.xy;
            int2 corner=int2(floor(at));
            float3 lo=1e10,hi=-1e10;
            [unroll] for(int cy=0;cy<2;++cy)[unroll] for(int cx=0;cx<2;++cx){
                float3 colour=Raw.Load(int3(clamp(corner+int2(cx,cy),0,size-1),0)).rgb;
                lo=min(lo,colour);hi=max(hi,colour);
            }
            // NVIDIA's own DLSS output legitimately leaves this 2x2 raw range:
            // a median 6/255 and a 99th percentile 12..16/255 on the star
            // corona (Steam eye dumps 131013 and 134857, 2026-09-16). An exact
            // bound clamped nearly every corona pixel there, dimming a ~30 px
            // polygon around every targeting label by 4-7 luma and trailing it
            // for up to 32 frames (issue 36). Widen the bound by a caller-
            // supplied tolerance so an ordinary reconstruction offset never
            // trips the clamp, while a departed glyph -- tens of steps away --
            // is still pulled back to within it and still marks the footprint
            // stale. An unbound b1 reads zero: the old exact clamp, which is
            // what the test rigs get unless they bind one.
            lo-=resolve.x;hi+=resolve.x;
            float3 bounded=clamp(v.rgb,lo,hi);stale=stale||any(abs(bounded-v.rgb)>1.0/255.0);v.rgb=bounded;
            if(edited){
                // A previous digit can satisfy today's colour bounds while
                // spelling the wrong number. Reconstruct real content edits
                // from current samples until the source edit expires. Static
                // text keeps the model's subpixel reconstruction above.
                // Rebuild only where the fresh frame disagrees with the
                // temporal output beyond the tolerance: while a label's
                // distance readout ticks, its whole crop is marked edited,
                // and an unconditional rebuild pulled the label's unchanged
                // background down to the raw cubic -- which sits below
                // NVIDIA's level on the corona -- turning the label into a
                // faint box the size of the crop (issue 36). Where a changed
                // digit's temporal blend still differs from the fresh frame
                // by more than the tolerance it is rebuilt; elsewhere the
                // bound above already holds it within the tolerance of the
                // fresh frame, so a stale residual is bounded by the same
                // tolerance as a ghost.
                float4 wx=cubic(frac(at.x)),wy=cubic(frac(at.y));float3 fresh=0;
                [unroll]for(int y=0;y<4;++y)[unroll]for(int x=0;x<4;++x)
                    fresh+=Raw.Load(int3(clamp(corner+int2(x-1,y-1),0,size-1),0)).rgb*wx[x]*wy[y];
                fresh=clamp(fresh,lo,hi);
                if(any(abs(fresh-v.rgb)>resolve.x))v.rgb=fresh;
            }
        }
        if(!active && resolve.y>0){
            // NVIDIA's accumulated history lifts faint, smooth glow above the
            // frame's own level after a stretch of motion (+2..3.5/255 on a
            // 2..5/255 star glow, Steam eye dumps 174233..174334, 2026-09-16),
            // while beside anything whose surroundings get fresh history --
            // HUD text, orbit lines, the cockpit struts -- the output stays
            // at the frame's level. The un-lifted zone reads as a dark halo
            // and, with the background streaming past, a wake (issue 36).
            // Hold faint flat pixels within one step of the raw 2x2 range, the
            // way UI pixels are held above; the accumulation has nothing to
            // add on a flat field (its residual noise there is no lower than
            // the frame's). resolve.y is the brightness limit, 0 = off; the
            // weight fades out toward it and toward texture, so the hold has
            // no seam. Rigs that bind no b1 read zero: no hold.
            float2 at=(float2(ox,oy)+.5)*float2(size)/float2(extent)-.5+jit.xy;
            int2 corner=int2(floor(at));int2 centre=int2(round(at));
            float3 lo=1e10,hi=-1e10;float lumaLo=1e10,lumaHi=-1e10;
            [unroll] for(int ny=-1;ny<=1;++ny)[unroll] for(int nx=-1;nx<=1;++nx){
                float3 c=Raw.Load(int3(clamp(centre+int2(nx,ny),0,size-1),0)).rgb;
                float l=dot(c,float3(.299,.587,.114));lumaLo=min(lumaLo,l);lumaHi=max(lumaHi,l);
            }
            [unroll] for(int cy=0;cy<2;++cy)[unroll] for(int cx=0;cx<2;++cx){
                float3 c=Raw.Load(int3(clamp(corner+int2(cx,cy),0,size-1),0)).rgb;
                lo=min(lo,c);hi=max(hi,c);
            }
            float faint=1-smoothstep(resolve.y*.75,resolve.y,lumaHi);
            float flat=1-smoothstep(8.0/255.0,16.0/255.0,lumaHi-lumaLo);
            float w=faint*flat;
            if(w>0){float3 held=clamp(v.rgb,lo-1.0/255.0,hi+1.0/255.0);v.rgb=lerp(v.rgb,held,w);}
        }
        uint fullW,fullH;FullCurrent.GetDimensions(fullW,fullH);
        if(fullW>0) {
            // This target never entered NGX. Restore only its current-frame
            // contribution, using matching clean/full tone-map samples. The
            // signed influence is not clamped: the game can export alpha >1.
            // Bilinear weights keep the current footprint compact and avoid
            // a cubic kernel adding dark lobes outside thin glyphs.
            float2 at=(float2(ox,oy)+.5)*float2(size)/float2(extent)-.5+jit.xy;
            int2 corner=int2(floor(at));float2 f=frac(at);
            float3 full=0,clean=0;float a=0;bool affected=false;
            [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x){
                int2 p=clamp(corner+int2(x,y),0,size-1);
                float weight=(x?f.x:1-f.x)*(y?f.y:1-f.y);
                float3 c=Raw.Load(int3(p,0)).rgb,original=FullCurrent.Load(int3(p,0)).rgb;
                float influence=UiInfluence.Load(int3(p,0));
                affected=affected||any(c!=original)||influence!=0;
                full+=original*weight;clean+=c*weight;a+=influence*weight;
            }
            if(affected)v.rgb=full+(v.rgb-clean)*(1-a);
        }
        Output[uint2(ox,oy)]=v;
    }
    // Bound the departing footprint to 32 frames. Unrelated world detail
    // returning here must not keep an old UI clip alive indefinitely.
    Next[id.xy]=float4(0,0,0,here?1:stale?max(remaining-1.0/32.0,0):0);
}
)HLSL";
}
