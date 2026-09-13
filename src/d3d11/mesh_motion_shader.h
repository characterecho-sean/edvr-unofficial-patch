#pragma once
namespace edvr {
// Same 240-byte layout as holo records, but with an independent history.
// Capture batches draws sharing unchanged scene/pool inputs. One further
// dispatch at the eye's temporal pass matches all records in parallel.
constexpr char kMeshMotionHlsl[]=R"HLSL(
struct Record { uint4 key[8]; float4 clip[3]; float4 map[3]; float4 meta; };
struct PoolRecord { uint4 data[21]; };
struct CaptureInput { uint4 info; uint4 mesh[4]; float4 dimensions; };
cbuffer Scene:register(b1) { float4 scene[276]; }
cbuffer Draw:register(b3) { uint4 info; uint4 mesh[4]; float4 dimensions; }
StructuredBuffer<PoolRecord> Pool:register(t0);
ByteAddressBuffer Instance:register(t1);
StructuredBuffer<Record> Previous:register(t2);
StructuredBuffer<CaptureInput> Inputs:register(t3);
RWStructuredBuffer<Record> Current:register(u0);
float3 turn(float4 q,float3 v) {
    return (2*q.w*q.w-1)*v+2*dot(q.xyz,v)*q.xyz+2*q.w*cross(q.xyz,v);
}
[numthreads(64,1,1)] void capture(uint lane:SV_DispatchThreadID) {
    if(lane>=info.z)return;
    CaptureInput input=Inputs[lane];uint at=input.info.x;
    Record n=(Record)0;
    [unroll]for(uint k=0;k<4;++k)n.key[k]=input.mesh[k];
    uint index=Instance.Load(input.info.w),count,stride;Pool.GetDimensions(count,stride);
    n.key[7].w=index; // coverage lookup only; excluded from persistent key
    if(index>=count){Current[at]=n;return;}
    PoolRecord p=Pool[index];
    n.key[4]=uint4(p.data[1].w,p.data[20].x,0,0);
    float scale=asfloat(p.data[0].y);
    uint2 packed=p.data[0].zw;
    // Original DXBC immediate is 0x38000100 (1/32767), not 2/65535.
    // That small distinction is visible on cockpit meshes around the eye.
    float4 q=float4(packed.x&65535,packed.x>>16,packed.y&65535,packed.y>>16)*(1.0/32767.0)-1;
    float3 pos=asfloat(p.data[1].xyz)-scene[275].xyz;
    float3 x=turn(q,float3(scale,0,0)),y=turn(q,float3(0,scale,0)),z=turn(q,float3(0,0,scale));
    [unroll]for(uint r=0;r<3;++r){
        uint row=r==2?3:r;
        float4 c=float4(scene[270][row],scene[271][row],scene[272][row],scene[273][row]);
        n.clip[r]=float4(dot(c.xyz,x),dot(c.xyz,y),dot(c.xyz,z),dot(c,float4(pos,1)));
    }
    // These original shaders have constant reversed clip Z. A skin
    // palette changes individual vertices and cannot use a rigid inverse.
    bool valid=p.data[0].x==0 && isfinite(scale) && abs(scale)>1e-8 && abs(dot(q,q)-1)<.002 &&
        all(isfinite(pos)) && all(isfinite(n.clip[0])) && all(isfinite(n.clip[1])) && all(isfinite(n.clip[2])) && scene[273].z>0 &&
        scene[270].z==0 && scene[271].z==0 && scene[272].z==0 && scene[273].w==0;
    n.meta=float4(valid?1:0,input.dimensions.xy,0);
    Current[at]=n;
}
groupshared uint counts[64],indices[64];
[numthreads(64,1,1)] void match(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    Record n=Current[group.x];
    uint found=0,index=0;
    if(n.meta.x==1){
        // A cockpit mesh can surround the eye with its local origin behind
        // it. Compare homogeneous origin directions, not projected X/W:
        // the latter becomes undefined as that invisible origin crosses Z=0.
        float3 here=float3(n.clip[0].w,n.clip[1].w,n.clip[2].w);
        float distance=max(length(here),.025);
        [loop]for(uint i=lane;i<info.y;i+=64){
            Record p=Previous[i];bool same=p.meta.x==1;
            [unroll]for(uint k=0;k<5;++k)same=same && all(n.key[k]==p.key[k]);
            float3 there=float3(p.clip[0].w,p.clip[1].w,p.clip[2].w);
            float oldDistance=max(length(there),.025);
            same=same && all(abs(here/distance-there/oldDistance)<.2) && oldDistance>distance*.5 && oldDistance<distance*2;
            if(same){++found;index=i;}
        }
    }
    counts[lane]=found;indices[lane]=index;GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint step=32;step;step>>=1){
        if(lane<step){counts[lane]+=counts[lane+step];indices[lane]=max(indices[lane],indices[lane+step]);}
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane!=0)return;
    if(counts[0]==1){
        Record p=Previous[indices[0]];
        float3 a=cross(n.clip[1].xyz,n.clip[2].xyz),b=cross(n.clip[2].xyz,n.clip[0].xyz),c=cross(n.clip[0].xyz,n.clip[1].xyz);
        float det=dot(n.clip[0].xyz,a);
        float3 t=float3(n.clip[0].w,n.clip[1].w,n.clip[2].w);
        [unroll]for(uint r=0;r<3;++r){
            float3 v=float3(dot(p.clip[r].xyz,a),dot(p.clip[r].xyz,b),dot(p.clip[r].xyz,c))/det;
            n.map[r]=float4(v,p.clip[r].w-dot(v,t));
        }
        n.meta.w=abs(det)>1e-12 && all(isfinite(n.map[0])) && all(isfinite(n.map[1])) && all(isfinite(n.map[2])) ? 1:0;
    }
    Current[group.x]=n;
}
)HLSL";
constexpr char kMeshCoverageHlsl[]=R"HLSL(
ByteAddressBuffer Instance:register(t15);
cbuffer Draw:register(b13) { uint4 info; }
float2 coverage(float4 p,uint id){
    id &= 0x7fffffff;
    [loop]for(uint i=0;i<info.z;++i){
        uint at=info.x+i;
        if(Instance.Load(info.w+i*8)==id)return float2(at+1,p.z);
    }
    discard;return 0;
}
// Preserve the original VS output register layout, including unused
// lighting/UV registers. D3D11 does not remap inter-stage registers.
struct MaterialInput {
    nointerpolation uint3 id:__USER_MATERIALMODULATION_DATAID;
    float3 normal:__USER_VERTEX_M_LIGHTINGNORMAL;
    float3 tangent:__USER_VERTEX_M_LIGHTINGTANGENT;
    float2 uv:__USER_VERTEX_M_TEXCOORD;
    float4 p:SV_Position;
};
struct FaceInput {
    nointerpolation uint id:__USER_VERTEX_FACEINVARIANT;
    float3 normal:__USER_VERTEX_M_LIGHTINGNORMAL;
    float3 tangent:__USER_VERTEX_M_LIGHTINGTANGENT;
    float2 uv:__USER_VERTEX_M_TEXCOORD;
    float4 p:SV_Position;
};
float2 material(MaterialInput v):SV_Target{return coverage(v.p,v.id.y);}
float2 face(FaceInput v):SV_Target{return coverage(v.p,v.id);}
struct MultiUvInput {
    nointerpolation uint id:__USER_VERTEX_FACEINVARIANT;
    float3 normal:__USER_VERTEX_M_LIGHTINGNORMAL;
    float3 tangent:__USER_VERTEX_M_LIGHTINGTANGENT;
    float4 uv:__USER_VERTEX_M_TEXCOORD;
    float4 p:SV_Position;
};
struct LitMultiUvInput {
    nointerpolation uint id:__USER_VERTEX_FACEINVARIANT;
    float3 normal:__USER_VERTEX_M_LIGHTINGNORMAL;
    float3 position:__USER_VERTEX_M_LIGHTINGPOSITION;
    float3 tangent:__USER_VERTEX_M_LIGHTINGTANGENT;
    float4 uv:__USER_VERTEX_M_TEXCOORD;
    float2 uv2:__USER_VERTEX_M_TEXCOORD2;
    float4 p:SV_Position;
};
struct DetailInput {
    nointerpolation uint2 id:__USER_VERTEX_FACEINVARIANT;
    float3 normal:__USER_VERTEX_M_LIGHTINGNORMAL;
    float3 tangent:__USER_VERTEX_M_LIGHTINGTANGENT;
    float2 uv:__USER_VERTEX_M_TEXCOORD;
    float4 p:SV_Position;
};
float2 multiUv(MultiUvInput v):SV_Target{return coverage(v.p,v.id);}
float2 litMultiUv(LitMultiUvInput v):SV_Target{return coverage(v.p,v.id);}
float2 detail(DetailInput v):SV_Target{return coverage(v.p,v.id.x);}
)HLSL";
}
