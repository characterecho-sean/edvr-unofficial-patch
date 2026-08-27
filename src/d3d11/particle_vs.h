// The replacement vertex shader for Elite's particle billboards.
//
// Written against the disassembly of the game's own vs EB787F983BC1F5A3
// (dumped 2026-08-23, kept at docs/shaders/particle-vs.asm), and written as a
// MECHANICAL TRANSCRIPTION of it: register for register, instruction for
// instruction, with the same eleven outputs feeding a pixel shader we do
// not control. That style is deliberate. A semantic rewrite would have to
// re-derive what every output means -- lighting normal, lighting tangent,
// atlas indices, the flipbook blend -- and a single misread would light
// or animate the smoke wrongly in a way that is hard to see and harder to
// bisect. A transcription can be checked instead of trusted: compile it,
// disassemble it, and compare against the original.
//
// ONE thing is changed, and it is the bug:
//
//     original:  right = normalize(cross(cb1[278], cb1[279]))
//                up    = normalize(cross(cb1[279], right))
//
// cb1[278] and cb1[279] are the camera's up and forward, so every quad in
// a draw shares one screen-plane-aligned basis. In a headset that couples
// the whole plume to the head: it rolls when you roll (fixed already by
// substituting world up for cb1[278]) and, because each quad's normal
// points along the VIEW AXIS rather than at the viewer, a plume off to
// one side is drawn foreshortened -- and the foreshortening changes as
// you look around, which reads as the smoke rotating about its own axis.
// The same disease, and the same cure, as the sun glare's disc.
//
// The cure needs a per-PARTICLE direction, which no constant can hold --
// that is why this is a shader and not another substitution. Each vertex
// aims at the viewer:
//
//     face  = normalize(particlePosition - cameraPosition)
//     right = normalize(cross(worldUp, face))
//     up    = cross(face, right)
//
// The camera position arrives in our own constant buffer at b3, solved
// CPU-side from the game's own view-projection rows, because the space
// those particle positions live in is the game's and not one we get to
// choose. Everything else below -- the fades, the four alignment modes,
// the per-particle spin, the atlas flipbook, every output -- is the
// game's own arithmetic, copied.
#pragma once

namespace edvr {

constexpr const char kParticleWorldVS[] = R"HLSL(
cbuffer CB0 : register(b0) { float4 cb0[12]; };
cbuffer CB1 : register(b1) { float4 cb1[280]; };
cbuffer CB2 : register(b2) { float4 cb2[4]; };
// Ours: the viewer's position in the same space the particle positions
// end up in, and the world's up axis. pCam.w is a flag -- zero means the
// solve was not available this draw and the original camera-plane basis
// is used, so a missing feed degrades to the game's own look.
cbuffer CBP : register(b3) {
    float4 pCam;
    float4 pUp;
};

struct VSIn {
    float4 v0  : POSITION0;              // xyz local position, w spin
    float4 v1  : DIRECTION0;           // declared wide to match
    float3 v2  : DIMENSIONS0;          // SIGNED half extents per corner
    float4 v3  : ALIGNBLENDBRIGHT0;    // x align mode (x255), y blend
    float  v4  : BRIGHTNESS0;
    float4 v5  : AXIS0;
    float2 v6  : VERTEXALPHA0;           // the corner, 0..1
    int4   v7  : LIGHTINGATLASINDEX0;
    float4 v8  : UVSCURRENTDIFFUSE0;
    float4 v9  : UVSNEXTDIFFUSE0;
    float  v10 : TEXBLENDDIFFUSE0;
    float  v11 : ATLASINDICESDIFFUSE0;   // two uint16 packed in the bits
    float4 v12 : COLOUR0;
};

struct VSOut {
    float4 o0  : TEXCOORD2;
    float4 o1  : TEXCOORD3;
    float4 o2  : TEXCOORD5;
    float4 o3  : TEXCOORD6;
    float4 o4  : TEXCOORD11;
    int    o5  : TEXCOORD13;
    float4 o6  : TEXCOORD14;
    float4 o7  : TEXCOORD15;
    float4 o8  : TEXCOORD21;
    float  o9  : TEXCOORD22;
    float4 o10 : TEXCOORD24;
    float4 o11 : SV_POSITION;
};

VSOut main(VSIn i) {
    VSOut o = (VSOut)0;
    float4 r0, r1, r2, r3, r4, r5, r6, r7;

    r0.xyz = i.v0.xyz * cb1[222].z;
    r1.xy  = i.v2.xy * cb1[222].z;
    r1.z   = max(abs(r1.y), abs(r1.x));

    uint packed = asuint(i.v11);
    uint lowIdx = packed & 0x0000ffffu;
    uint highIdx = packed >> 16u;
    o.o3.w = (float)lowIdx;
    o.o6.w = (float)highIdx;

    o.o0.z = i.v6.x * i.v8.y + i.v8.x;
    o.o0.w = i.v6.y * i.v8.w + i.v8.z;

    r0.w = 1.0;
    r2.x = dot(cb0[9],  r0);
    r2.y = dot(cb0[10], r0);
    r2.z = dot(cb0[11], r0);

    // ---- the billboard basis ----
    // The game's own: perpendicular to the camera's forward, which is the
    // whole artifact. Ours: perpendicular to the direction from the
    // viewer to THIS particle, which is what facing the viewer means.
    float3 faceAxis = cb1[279].xyz;
    if (pCam.w > 0.5) {
        float3 toCam = r2.xyz - pCam.xyz;
        float lenToCam = length(toCam);
        if (lenToCam > 1e-4) faceAxis = toCam / lenToCam;
    }
    float3 basisUp = (pCam.w > 0.5) ? pUp.xyz : cb1[278].xyz;

    // The cross, guarded for real. The game never needed this: its two
    // vectors are the camera's own up and forward, which cannot be
    // parallel. Ours can -- faceAxis points at THIS particle, and a
    // particle directly overhead or underfoot lines up with world up
    // exactly. There the cross collapses, rsqrt(0) is infinity, and
    // zero times infinity is a NaN that propagates into the vertex
    // position: one particle becomes a triangle with no finite corner,
    // which the rasteriser is free to smear across the frame. That is
    // what took the terrain out.
    //
    // The game's own guard cannot help, because it tests the NaN with
    // (x != 0), and a NaN compares unequal to everything -- so it keeps
    // the NaN it meant to reject. Testing the LENGTH before dividing is
    // the version that works.
    float3 bRight = cross(basisUp, faceAxis);
    float bLen2 = dot(bRight, bRight);
    if (bLen2 < 1e-8) {
        // Straight up or straight down: any perpendicular will do, and
        // the roll it implies is unobservable on a quad this symmetric.
        bRight = cross(float3(0.0, 0.0, 1.0), faceAxis);
        bLen2 = dot(bRight, bRight);
        if (bLen2 < 1e-8) {
            bRight = float3(1.0, 0.0, 0.0);
            bLen2 = 1.0;
        }
    }
    r0.xyz = bRight * rsqrt(bLen2);

    r3.xyz = cross(faceAxis, r0.xyz);
    r0.w = max(dot(r3.xyz, r3.xyz), 1e-12);
    r3.xyz = r3.xyz * rsqrt(r0.w);

    // ---- the near fade, verbatim: still measured along the true view
    // axis, because that is what a depth fade means ----
    r0.w = dot(cb1[279].xyz, r2.xyz);
    r1.w = cb2[2].w * cb2[3].x;
    r2.w = -cb2[2].w * cb2[3].x + cb2[2].w;
    r3.w = -r0.w + cb2[2].w;
    r1.w = saturate(r3.w / r1.w);
    r1.w = -r1.w + 1.0;
    r0.w = (r2.w < r0.w) ? 1.0 : 0.0;
    r0.w = r0.w * r1.w;

    // ---- alignment mode ----
    r1.w = i.v0.w * 0.5;
    uint mode = (uint)(i.v3.x * 255.0 + 0.5);
    if (mode == 0) {
        r4.xyz = faceAxis;
    } else {
        r5.xyz = i.v1.xyz * 2.007874 - 1.0;
        r6.x = dot(cb0[9].xyz,  r5.xyz);
        r6.y = dot(cb0[10].xyz, r5.xyz);
        r6.z = dot(cb0[11].xyz, r5.xyz);
        r3.w = rsqrt(dot(r6.xyz, r6.xyz));
        r5.xyz = r6.xyz * r3.w;
        if (mode == 1 || mode == 2) {
            r6.xyz = cross(r2.xyz, r5.xyz);
            r3.w = rsqrt(dot(r6.xyz, r6.xyz));
            r0.xyz = r6.xyz * r3.w;
            r4.xyz = cross(r0.xyz, cb1[279].xyz);
            r3.xyz = r5.xyz;
            r1.w = 0.0;
        } else {
            r6.xyz = i.v5.xyz * 2.007874 - 1.0;
            r7.x = dot(cb0[9].xyz,  r6.xyz);
            r7.y = dot(cb0[10].xyz, r6.xyz);
            r7.z = dot(cb0[11].xyz, r6.xyz);
            r3.w = rsqrt(dot(r7.xyz, r7.xyz));
            r4.xyz = r7.xyz * r3.w;
            if (mode != 4) {
                r5.xyz = cross(r4.xyz, r5.xyz);
                r2.w = rsqrt(dot(r5.xyz, r5.xyz));
                r0.xyz = r5.xyz * r2.w;
                r3.xyz = cross(r0.xyz, r4.xyz);
            }
        }
    }

    // ---- the per-particle spin about the facing axis ----
    r1.w = r1.w + r1.w;
    float spinS, spinC;
    sincos(r1.w, spinS, spinC);
    float3 axisN = r4.xyz * rsqrt(dot(r4.xyz, r4.xyz));
    float oneMinusC = 1.0 - spinC;
    // Rodrigues, in the shape the original builds it: a 3x3 from the
    // normalised axis, then both basis vectors rotated through it.
    float3x3 spin;
    spin[0] = float3(axisN.x * axisN.x * oneMinusC + spinC,
                     axisN.x * axisN.y * oneMinusC - axisN.z * spinS,
                     axisN.x * axisN.z * oneMinusC + axisN.y * spinS);
    spin[1] = float3(axisN.y * axisN.x * oneMinusC + axisN.z * spinS,
                     axisN.y * axisN.y * oneMinusC + spinC,
                     axisN.y * axisN.z * oneMinusC - axisN.x * spinS);
    spin[2] = float3(axisN.z * axisN.x * oneMinusC - axisN.y * spinS,
                     axisN.z * axisN.y * oneMinusC + axisN.x * spinS,
                     axisN.z * axisN.z * oneMinusC + spinC);
    r0.xyz = normalize(mul(spin, r0.xyz));
    r3.xyz = normalize(mul(spin, r3.xyz));

    // ---- the quad corner, and the collapse when invisible ----
    r5.xyz = r0.xyz * r1.x + r2.xyz;
    r5.xyz = r3.xyz * r1.y + r5.xyz;
    r2.w = (r0.w < 0.001) ? 0.0 : 1.0;
    r5.xyz = -r2.xyz + r5.xyz;
    r2.xyz = r5.xyz * r2.w + r2.xyz;

    // ---- the lighting normal, from the corner's signs ----
    float2 sgn;
    sgn.x = (float)((r1.x < 0.0 ? 1 : 0) - (0.0 < r1.x ? 1 : 0));
    sgn.y = (float)((r1.y < 0.0 ? 1 : 0) - (0.0 < r1.y ? 1 : 0));
    r5.yzw = r3.xyz * sgn.y;
    r5.xyz = r0.xyz * sgn.x + r5.yzw;
    r4.xyz = -r4.xyz * r1.w + r5.xyz;
    o.o7.xyz = r4.xyz * rsqrt(dot(r4.xyz, r4.xyz));

    // ---- the remaining outputs, verbatim ----
    r4.xyz = r2.y * cb1[230].xyz;
    r4.xyz = r2.x * cb1[229].xyz + r4.xyz;
    r4.xyz = r2.z * cb1[231].xyz + r4.xyz;
    o.o8.xyz = r4.xyz + cb1[232].xyz;

    r4 = r2.y * cb1[271];
    r4 = r2.x * cb1[270] + r4;
    r4 = r2.z * cb1[272] + r4;
    r4 = r4 + cb1[273];

    if (i.v7.x >= 0) {
        r5.xy = r1.xy / r1.z;
        r5.xy = r5.xy + 1.0;
        r5.xy = r5.xy * 0.5;
        float4 idx = float4(i.v7.w, i.v7.z, i.v7.x, i.v7.y);
        r7.xyz = idx.xxy * cb1[222].xyy;
        r5.x = r5.x * r7.x;
        r5.z = r5.y * r7.y + r7.z;
        r5.xy = r7.xy * idx.zw + r5.xz;
        r5.z = -r5.y + 1.0;
        o.o0.xy = r5.xz;
    } else {
        o.o0.xy = float2(0.0, 0.0);
    }

    o.o10.w = r0.w * i.v12.w;
    o.o10.xyz = i.v4.xxx * i.v12.xyz;
    o.o8.w = dot(r2.xyz, cb1[279].xyz);
    o.o4.xy = i.v6.xy * i.v9.yw + i.v9.xz;
    o.o1.xyz = r4.xyw;
    o.o1.w = i.v3.y * 255.0;
    o.o2.w = i.v5.w;
    o.o2.xyz = r0.xyz;
    o.o3.xyz = r3.xyz;
    o.o4.zw = r1.xy;
    o.o6.xyz = r2.xyz;
    o.o7.w = i.v10;
    o.o11 = r4;
    o.o5 = i.v7.x;
    o.o9 = r1.z;
    return o;
}
)HLSL";

}  // namespace edvr
