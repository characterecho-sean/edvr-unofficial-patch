// The replacement vertex shaders for the FSS panel composite pair.
//
// Written against the disassemblies of the game's own vs
// B018D143700AB803 (the position-only depth prepass) and vs
// A888D51024D9798E (the textured pass) -- dumped 2026-08-25, kept at
// docs/shaders/fss-panel-vs.asm -- and written as MECHANICAL TRANSCRIPTIONS of
// them, the particle billboard's discipline (particle_vs.h): register for
// register, swizzle for swizzle, feeding a pixel shader we do not
// control. A transcription can be checked instead of trusted: compile it,
// disassemble it, compare.
//
// ONE thing is changed in each, and it is the feature -- in CLIP SPACE,
// which the first field flight taught the hard way. v1 scaled the world
// position, assuming cb1[275] was the eye; it is Elite's world-rebase
// origin, near the camera but not at it, and scaling toward a point that
// is not the projection centre translates the quad on screen (the screen
// slid up and left, 2026-08-25). v2 scales where the geometry is exact by
// construction: for any point, the ray from the eye is fixed in NDC, and
// the point at fraction k of the eye distance has clip coordinates
//
//     clip' = (k*C.x, k*C.y, b + k*(C.z - b), k*C.w)
//
// where C is the stock clip position and b is the clip z at the eye
// itself -- recovered from the fused view-projection rows alone, because
// the projection's w row is the view depth: the z row equals s times the
// w row plus (0,0,0,b), so b = zrow.w - s*wrow.w with s read off any
// component pair. NDC x and y are C.x/C.w exactly, per eye, at every k:
// the screen moves along each eye's own line of sight and nowhere else.
// EDVR_FSS_DIST arrives as a compile-time macro and the pair is
// recompiled when the setting moves; both shaders carry the same factor
// or the depth prepass and the colour pass disagree and the quad eats
// itself.
//
// The packed-vertex decode (two position encodings selected by a format
// byte), the per-instance record at t33 (stride 336: bone base, uniform
// scale, a unorm16x4 orientation quaternion, position at byte 16), the
// optional four-bone skinning over t38 (stride 48), and every output are
// the game's own arithmetic, copied.
#pragma once

namespace edvr {

// Shared preamble: the buffers and records both shaders read. Kept as one
// string so the two transcriptions cannot drift on a struct layout.
constexpr const char kFssPanelCommon[] = R"HLSL(
cbuffer CB0 : register(b0) { float4 cb0[12]; };
cbuffer CB1 : register(b1) { float4 cb1[276]; };
cbuffer CB2 : register(b2) { float4 cb2[8]; };

struct EdvrInst {          // t33, stride 336; only the head is read
    uint   boneBase;       // byte 0
    float  scale;          // byte 4
    uint   quatXY;         // byte 8   x lo16, y hi16, unorm16 -> [-1,1)
    uint   quatZW;         // byte 12
    float3 pos;            // byte 16
    uint   pad[77];        // to 336
};
struct EdvrBone {          // t38, stride 48: three rows of a 3x4
    float4 row0;
    float4 row1;
    float4 row2;
};
StructuredBuffer<EdvrInst> instData : register(t33);
StructuredBuffer<EdvrBone> boneData : register(t38);

// The packed position, two encodings picked by bits 24..30 of pva.z.
// Format 64: two uint16 pairs, a shared exponential scale in the fourth.
// Otherwise: 21-bit fixed point split across pva.x/y, scaled by the
// format value, offset by 1 << (pva.z >> 24). Shift-by->=32 relies on
// HLSL's mod-32 shift, which is also what the hardware ran for the game.
float3 edvrDecodePos(uint4 pva) {
    uint fmt = (pva.z >> 24u) & 0x7Fu;
    float3 p;
    if (fmt == 64u) {
        float lx = (float)(pva.x & 0xFFFFu);
        float hx = (float)(pva.x >> 16u);
        float ly = (float)(pva.y & 0xFFFFu);
        float hy = (float)(pva.y >> 16u);
        float s = exp2(hy * 0.000244) - 1.0;
        p = (float3(lx, hx, ly) * 0.000031 - 1.0) * s;
    } else {
        uint  sh   = pva.z >> 24u;
        float scl  = 1.0 / (float)((1 << (20u - fmt)) - 1);
        float offs = (float)(1u << sh);
        uint xb = pva.x & 0x1FFFFFu;
        uint yb = (pva.x >> 21u) + ((pva.y & 0x3FFu) << 11u);
        uint zb = (pva.y >> 10u) & 0x1FFFFFu;
        p = float3((float)xb, (float)yb, (float)zb) * scl - offs;
    }
    return p;
}

// The unorm16x4 orientation quaternion from the instance record.
float4 edvrDecodeQuat(uint quatXY, uint quatZW) {
    float4 q;
    q.x = (float)(quatXY & 0xFFFFu) * 0.000031 - 1.0;
    q.y = (float)(quatXY >> 16u)    * 0.000031 - 1.0;
    q.z = (float)(quatZW & 0xFFFFu) * 0.000031 - 1.0;
    q.w = (float)(quatZW >> 16u)    * 0.000031 - 1.0;
    return q;
}

// p' = (2w^2 - 1) p + 2 (q.p) q + 2 w (q x p) -- the expansion the game's
// own code builds, in its order.
float3 edvrQuatRotate(float4 q, float3 p) {
    float3 c = float3(q.y * p.z - p.y * q.z,
                      q.z * p.x - p.z * q.x,
                      q.x * p.y - p.x * q.y);
    float  d = dot(q.xyz, p);
    float  t = q.w * (q.w + q.w);
    float3 r = t * p - p;
    r = d * (q.xyz + q.xyz) + r;
    r = c * (q.w + q.w) + r;
    return r;
}

// The distance move, in clip space (see the header comment): scale x, y
// and w by k, and z about its value at the eye, which the fused rows
// yield as b = zrow.w - s*wrow.w with s = zrow/wrow on the largest
// w-row component.
float4 edvrClipAtDistance(float4 c, float4 zrow, float4 wrow, float k) {
    float3 aw = abs(wrow.xyz);
    float s;
    if (aw.x >= aw.y && aw.x >= aw.z) {
        s = zrow.x / wrow.x;
    } else if (aw.y >= aw.z) {
        s = zrow.y / wrow.y;
    } else {
        s = zrow.z / wrow.z;
    }
    float b = zrow.w - s * wrow.w;
    return float4(k * c.x, k * c.y, b + k * (c.z - b), k * c.w);
}

// The four-bone blend: up to four (index, weight) byte pairs, weights in
// 1/255ths, rows accumulated then applied. Returns the blended 3x4.
void edvrBlendBones(uint boneBase, uint idxPacked, uint wtPacked,
                    out float4 m0, out float4 m1, out float4 m2) {
    m0 = 0; m1 = 0; m2 = 0;
    uint idx = idxPacked;
    uint wts = wtPacked;
    uint n = 4u;
    [loop] while (wts != 0u && n != 0u) {
        uint  bi = idx & 255u;
        float w  = (float)(wts & 255u) * 0.003922;
        EdvrBone b = boneData[boneBase + bi];
        m0 += b.row0 * w;
        m1 += b.row1 * w;
        m2 += b.row2 * w;
        idx >>= 8u;
        wts >>= 8u;
        n = n - 1u;
    }
}
)HLSL";

// --- B018D143700AB803: the depth prepass, position only ---------------------
constexpr const char kFssPanelPrepassVS[] = R"HLSL(
struct VSIn {
    uint2 inst : INSTANCEANDMODELDATAINDEX0;
    uint4 pva  : PACKEDVERTEXDATAA0;
    uint4 pvc  : PACKEDVERTEXDATAC0;
};

float4 main(VSIn i) : SV_POSITION {
    float3 p = edvrDecodePos(i.pva);

    EdvrInst inst = instData[i.inst.x];
    if (inst.boneBase != 0u) {
        float4 m0, m1, m2;
        edvrBlendBones(inst.boneBase, i.pvc.x, i.pvc.y, m0, m1, m2);
        float4 p4 = float4(p, 1.0);
        p = float3(dot(m0, p4), dot(m1, p4), dot(m2, p4));
    }

    float3 camRel = inst.pos - cb1[275].xyz;
    float4 q = edvrDecodeQuat(inst.quatXY, inst.quatZW);
    float3 world = edvrQuatRotate(q, p) * inst.scale + camRel;

    float4 w4 = float4(world, 1.0);
    float4 o;
    o.x = dot(cb0[4], w4);
    o.y = dot(cb0[5], w4);
    o.z = dot(cb0[6], w4);
    o.w = dot(cb0[7], w4);
    // EDVR: the panel distance, the one change -- in clip space, so each
    // eye's angular position is untouched by construction.
    return edvrClipAtDistance(o, cb0[6], cb0[7], EDVR_FSS_DIST);
}
)HLSL";

// --- A888D51024D9798E: the textured pass ------------------------------------
//
// Outputs, in the game's own signature order. o1 is the lighting normal
// with a per-vertex flip riding bit 31 of pva.z; o2/o3 are position and
// tangent through the secondary transform rows cb0[9..11] (identity for
// these draws, kept anyway -- transcription, not interpretation); o0 is a
// planar projection of the LOCAL vertex against its decoded normal and
// bitangent, mapped through the cb2[6..7] atlas transform; o4 is the
// vertex UV scaled by 16.
constexpr const char kFssPanelColorVS[] = R"HLSL(
struct VSIn {
    uint2 inst : INSTANCEANDMODELDATAINDEX0;
    uint4 pva  : PACKEDVERTEXDATAA0;
    uint4 pvb  : PACKEDVERTEXDATAB0;
    uint4 pvc  : PACKEDVERTEXDATAC0;
};

struct VSOut {
    float4 o0 : TEXCOORD0;
    float3 o1 : TEXCOORD2;
    float3 o2 : TEXCOORD4;
    float3 o3 : TEXCOORD5;
    float2 o4 : TEXCOORD6;
    float4 o5 : SV_POSITION;
};

VSOut main(VSIn i) {
    VSOut o = (VSOut)0;

    // Local position, and the byte-packed frame: normal from pva.w's low
    // three bytes, tangent from (pva.w & 255, pva.z >> 8, pva.z >> 16) --
    // the shared first byte is the game's own packing, copied.
    float3 p = edvrDecodePos(i.pva);

    float3 n;
    n.x = (float)(i.pva.w & 255u)          * 0.007874 - 1.0;
    n.y = (float)((i.pva.w >> 8u) & 255u)  * 0.007874 - 1.0;
    n.z = (float)((i.pva.w >> 16u) & 255u) * 0.007874 - 1.0;
    float3 t;
    t.x = (float)(i.pva.w & 255u)          * 0.007874 - 1.0;
    t.y = (float)((i.pva.z >> 8u) & 255u)  * 0.007874 - 1.0;
    t.z = (float)((i.pva.z >> 16u) & 255u) * 0.007874 - 1.0;
    uint flip = i.pva.z & 0x80000000u;

    // b = cross(t, n), the game's operand order.
    float3 b = float3(t.y * n.z - n.y * t.z,
                      t.z * n.x - n.z * t.x,
                      t.x * n.y - n.x * t.y);

    // The atlas coordinate: the LOCAL vertex projected on (n, b), before
    // any skinning or placement.
    float2 pr;
    pr.x = dot(p, n);
    pr.y = dot(p, b);
    o.o0 = pr.xyxy * cb2[7].xyxy + cb2[6];

    // The vertex UV, unorm16 pair in pvb.x, times the 16-texel tile the
    // original bakes as a literal.
    float2 uv;
    uv.x = (float)(i.pvb.x & 0xFFFFu) * 0.000031 - 1.0;
    uv.y = (float)(i.pvb.x >> 16u)    * 0.000031 - 1.0;
    o.o4 = uv * 16.0;

    // Instance record, optional skinning of all three vectors.
    EdvrInst inst = instData[i.inst.x];
    if (inst.boneBase != 0u) {
        float4 m0, m1, m2;
        edvrBlendBones(inst.boneBase, i.pvc.x, i.pvc.y, m0, m1, m2);
        float4 p4 = float4(p, 1.0);
        float3 sp = float3(dot(m0, p4), dot(m1, p4), dot(m2, p4));
        float3 st = float3(dot(m0.xyz, t), dot(m1.xyz, t), dot(m2.xyz, t));
        float3 sn = float3(dot(m0.xyz, n), dot(m1.xyz, n), dot(m2.xyz, n));
        p = sp;
        t = st;
        n = sn;
    }

    // Camera-relative placement: rotate, scale, offset. The original
    // rotates the tangent and normal through zxy/yzx-permuted copies of
    // the same formula and un-permutes at the consumers; the permutations
    // cancel, so the straight rotations below are the same values -- the
    // one place this transcription simplifies, verified by tracing every
    // swizzle (t' lands as .zxy, n' as .yzx, and o3/o1 read them back in
    // .wxz/.zxy order, restoring both).
    float3 camRel = inst.pos - cb1[275].xyz;
    float4 q = edvrDecodeQuat(inst.quatXY, inst.quatZW);
    float3 world = edvrQuatRotate(q, p) * inst.scale + camRel;
    float3 wt = edvrQuatRotate(q, t);
    float3 wn = edvrQuatRotate(q, n);

    float4 w4 = float4(world, 1.0);
    o.o2.x = dot(cb0[9],  w4);
    o.o2.y = dot(cb0[10], w4);
    o.o2.z = dot(cb0[11], w4);
    o.o5.x = dot(cb0[4], w4);
    o.o5.y = dot(cb0[5], w4);
    o.o5.z = dot(cb0[6], w4);
    o.o5.w = dot(cb0[7], w4);
    // EDVR: the panel distance, the one change -- clip space only; the
    // world-space outputs above stay exactly stock.
    o.o5 = edvrClipAtDistance(o.o5, cb0[6], cb0[7], EDVR_FSS_DIST);
    o.o3.x = dot(wn, cb0[9].xyz);
    o.o3.y = dot(wn, cb0[10].xyz);
    o.o3.z = dot(wn, cb0[11].xyz);

    // The lighting output: cross(tangent', normal'), negated when the
    // flip bit rides pva.z's top bit -- the bitangent rebuilt after
    // rotation, in the game's operand order.
    float3 ln = float3(wt.y * wn.z - wt.z * wn.y,
                       wt.z * wn.x - wt.x * wn.z,
                       wt.x * wn.y - wt.y * wn.x);
    o.o1 = (flip != 0u) ? -ln : ln;

    return o;
}
)HLSL";

}  // namespace edvr
