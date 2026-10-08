#pragma once

// A vertex shader and a pixel shader written to the shape of the game's skinned pair (not the game's code: the
// game's shaders are not in the repository). The vertex shader is what the patcher in src/d3d11/dxbc_skin_clone.h
// was written against, instruction for instruction at the places it looks:
//   * the instance's pool slot at v0.x, the packed vertex streams at v1..v3;
//   * a record read from t33 (stride 336): word 0 the palette base, word 1 the scale, words 2-3 a packed quaternion
//     at byte offset 0, the position at byte offset 16;
//   * a skin loop of up to four bones over the palette at t38 (stride 48: three float4 rows, translation in .w),
//     bone bytes in C.x and weight bytes in C.y, taken only when the base is nonzero;
//   * the record's rotation applied by the game's quaternion formula, the scale, the position minus the camera
//     (cb1[275]), added;
//   * an optional displacement block under cb2 that moves the position after the pose chain, and the clip-space
//     position as three multiplies and an add against cb1[270..273];
//   * `precise` throughout, so no multiply-add is fused (the game's chain is marked the same way).
// Variant bits change the shape the way the five real shaders differ among themselves: a displacement block, a
// second vertex-data branch, a different register for the position.
#include <cstring>
#include <string>

namespace skin_clone_synthetic {

// The shape variants. Each of the declines in the rig is a vertex shader that differs from the plain one in one respect.
struct Variant {
    bool displacement = false;   // a block under cb2 after the pose chain, as four of the five real shaders have
    bool noPose = false;         // the position does not come from the pool record: no load at byte offset 16
    bool sqrtInChain = false;    // an opcode the clone does not know, inside the cloned range
    bool relativeCb = false;     // a constant buffer read with a computed index inside the cloned range
    bool occupied = false;       // a resource at t109 already
    bool noMatrix = false;       // no multiplies against cb1[270..272]
};

inline std::string vertexSource(const Variant& v) {
    const bool displacement = v.displacement;
    std::string s = R"HLSL(
struct Rec { uint4 head; float4 pos; uint4 pad[19]; };
struct Row { float4 r0; float4 r1; float4 r2; };
StructuredBuffer<Rec> Pool : register(t33);
StructuredBuffer<Row> Pal : register(t38);
cbuffer Scene : register(b1) { float4 S[276]; };
cbuffer Mat : register(b2) { float4 M[8]; };
StructuredBuffer<Row> Extra : register(t109);
struct VIn {
 uint2 inst : INSTANCEANDMODELDATAINDEX;
 uint4 a : PACKEDVERTEXDATAA;
 uint4 b : PACKEDVERTEXDATAB;
 uint4 c : PACKEDVERTEXDATAC;
};
struct VOut {
 nointerpolation uint id : __USER_VERTEX_FACEINVARIANT;
 float4 n : __USER_VERTEX_M_LIGHTINGNORMAL;
 float4 clip : SV_Position;
};
VOut main(VIn i) {
 VOut o;
 const uint idx = i.inst.x;
 precise float3 local;
 precise float scaleE = exp2(16.0 * float(i.a.y >> 16) * 0.000015259022) - 1.0;
 precise float3 unit = float3(float(i.a.x & 0xFFFF), float(i.a.x >> 16), float(i.a.y & 0xFFFF)) * 0.000030518044 - 1.0;
 local = scaleE * unit;
SQRTLINE
 const uint4 head = Pool[idx].head;
 if (head.x != 0u) {
  precise float4 m0 = float4(0, 0, 0, 0), m1 = float4(0, 0, 0, 0), m2 = float4(0, 0, 0, 0);
  uint bones = i.c.x, weights = i.c.y;
  [loop] for (uint k = 4u; k > 0u && weights != 0u; --k) {
   const uint bone = head.x + (bones & 255u);
   precise float w = float(weights & 255u) * 0.003921569;
   m0 += w * Pal[bone].r0;
   m1 += w * Pal[bone].r1;
   m2 += w * Pal[bone].r2;
   bones >>= 8u;
   weights >>= 8u;
  }
  precise float4 hp = float4(local, 1.0);
  precise float3 skinned = float3(dot(m0, hp), dot(m1, hp), dot(m2, hp));
  local = skinned;
 }
 precise float4 q = float4(float(head.z & 0xFFFFu), float(head.z >> 16), float(head.w & 0xFFFFu), float(head.w >> 16)) * 0.000030518044 - 1.0;
 precise float3 cr = cross(q.xyz, local);
 precise float qd = dot(q.xyz, local);
 precise float w2 = q.w * (q.w + q.w);
 precise float3 rot = (-local + local * w2) + (q.xyz + q.xyz) * qd + (q.w + q.w) * cr;
 precise float3 rel = POSELINE - CAMERA;
 rel = rel + asfloat(head.y) * rot;
)HLSL";
    if (displacement) s += R"HLSL(
 [branch] if (M[3].w != 0.0) {
  precise float3 world = rel + S[275].xyz;
  precise float d = dot(world, M[6].xyz) - M[6].w;
  rel = rel + rot * (saturate(d * 0.1) * 0.05);
 }
)HLSL";
    s += R"HLSL(
MATRIXLINES
 o.id = idx | 0x80000000u;
 o.n = float4(rot, 1.0) + EXTRA;
 return o;
}
)HLSL";
    const auto replace = [&](const char* key, const std::string& with) {
        for (size_t at; (at = s.find(key)) != std::string::npos;) s.replace(at, std::strlen(key), with);
    };
    replace("SQRTLINE", v.sqrtInChain ? " local = local * sqrt(1.0 + local.x * local.x);\n" : "");
    replace("POSELINE", v.noPose ? "float3(1.0, 2.0, 3.0)" : "Pool[idx].pos.xyz");
    replace("CAMERA", v.relativeCb ? "S[260 + (i.c.z & 15u)].xyz" : "S[275].xyz");
    replace("EXTRA", v.occupied ? "Extra[idx].r0" : "float4(0, 0, 0, 0)");
    replace("MATRIXLINES", v.noMatrix ? " o.clip = float4(rel, 1.0);\n"
                                      : " precise float4 clip = rel.x * S[270] + rel.y * S[271];\n precise float4 clip2 = rel.z * S[272];\n clip = clip + clip2;\n o.clip = clip + S[273];\n");
    return s;
}
inline std::string vertexSource(bool displacement) {
    Variant v;
    v.displacement = displacement;
    return vertexSource(v);
}

inline const char* pixelSource() {
    return R"HLSL(
struct PIn {
 nointerpolation uint id : __USER_VERTEX_FACEINVARIANT;
 float4 n : __USER_VERTEX_M_LIGHTINGNORMAL;
 float4 pos : SV_Position;
};
struct POut { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3; };
POut main(PIn i) {
 POut o;
 o.c0 = float4(i.n.xyz * 0.5 + 0.5, 1.0);
 o.c1 = float4(float(i.id & 0xFFFFu), i.pos.z, 0.25, 1.0);
 o.c2 = float4(i.pos.xy * 0.015625, 0.5, 1.0);
 o.c3 = float4(0.1, 0.2, 0.3, 0.4);
 return o;
}
)HLSL";
}

}  // namespace skin_clone_synthetic
