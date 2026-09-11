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
RWTexture2D<float4> Output:register(u0);
RWTexture2D<float4> Next:register(u1);
cbuffer P:register(b0){int4 region;int2 size;int2 texSize;float4 tanNow;float4 tanPrev;float4 jit;}
bool marked(int2 p){uint k=uint(Coverage.Load(int3(clamp(p,0,size-1),0))*255+.5)&3u;return k==1u||k==2u;}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){
    if(any(id.xy>=uint2(size)))return;
    int2 q=int2(id.xy),r=int2(floor(float2(q)+jit.xy+.5));
    bool here=false;
    [unroll] for(int y=-1;y<=1;++y)[unroll] for(int x=-1;x<=1;++x)here=here||marked(r+int2(x,y));
    float remaining=Previous.Load(int3(q,0)).a;
    // DLSS can transport an old glyph along the newly exposed world's
    // vector, outside both its current and stationary previous footprint.
    // Follow that same vector through the unjittered influence history.
    // A max over the bilinear footprint retains subpixel coverage without
    // spreading it to unrelated neighbours. Off-screen history is invalid.
    if(!here && remaining<1){
        float2 before=float2(q)+Motion.Load(int3(clamp(r,0,size-1),0));
        if(all(isfinite(before)) && all(before>=0) && all(before<=float2(size-1))){
            int2 corner=int2(floor(before));
            int2 step=int2(ceil(before))-corner;
            [unroll] for(int y=0;y<2;++y)[unroll] for(int x=0;x<2;++x)
                remaining=max(remaining,Previous.Load(int3(corner+int2(x,y)*step,0)).a);
        }
    }
    bool active=here||remaining>0;
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
            int2 corner=int2(floor((float2(ox,oy)+.5)*float2(size)/float2(extent)-.5+jit.xy));
            float3 lo=1e10,hi=-1e10;
            [unroll] for(int cy=0;cy<2;++cy)[unroll] for(int cx=0;cx<2;++cx){
                float3 colour=Raw.Load(int3(clamp(corner+int2(cx,cy),0,size-1),0)).rgb;
                lo=min(lo,colour);hi=max(hi,colour);
            }
            float3 bounded=clamp(v.rgb,lo,hi);stale=stale||any(abs(bounded-v.rgb)>1.0/255.0);v.rgb=bounded;
        }
        Output[uint2(ox,oy)]=v;
    }
    // Bound the departing footprint to 32 frames. Unrelated world detail
    // returning here must not keep an old UI clip alive indefinitely.
    Next[id.xy]=float4(0,0,0,here?1:stale?max(remaining-1.0/32.0,0):0);
}
)HLSL";
}
