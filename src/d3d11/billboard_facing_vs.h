// The replacement vertex shaders for the EXPLOSION billboards (docs/particle-billboards.md, 2026-10-16: "facing").
//
// Three shaders draw the explosion: 9F4BBCFCD3B68BC9, BBAD1CA808E1E292 and 9AEC596A2B036EA6 (it also draws the
// witchspace starfield). They rose and decayed together in the 2026-10-16 flight and later drew identical counts per
// window. The roll-free basis (billboard_rollfree.h) already stops them rolling, but it keeps the camera's FORWARD, so
// the quad plane follows head PITCH and the explosion flexes. Facing is per particle, so only a vertex shader can do it:
// the same fix the plume (particle_vs.h) and the flare (flare_vs.h) have.
//
// The eye dump of the next flight (2026-10-16, edvr_gfx_20261016_063230.log) then showed the FIREBALL PUFFS are drawn by
// AFBCD3ADB9092F78 (PS 2BAE3742FEB916D9), 9F4B's no-near-fade twin, and 4BDE4DBF88C2BA27 (PS E54F2A902E5631F6) is BBAD's
// no-fade twin; both had only the roll-free rebind and still flexed. They are transcribed below as well, five in all.
//
// THESE ARE TRANSCRIPTIONS, NOT REWRITES, AND EACH IS GRANTED TO ONE EXACT HASH. The 0.12.3 regression (memory note
// edvr-geyser-workstream.md) shipped the flare's replacement on a shader whose body looked the same and whose OUTPUTS
// were not, and made the witchspace starfield vanish. So:
//   * each text below was written from the disassembly of ITS OWN shader, from `vs_5_0` down, dcl lines and the
//     signature block included (tools/billboard_facing_test/fixtures holds the three originals);
//   * its input signature, output signature (names, indices, registers, masks, order) and constant buffer sizes
//     (b0[12], b1[280], and b2[3] where the original has one) are the original's, held by the rig on reflection;
//   * the rig runs the ORIGINAL bytecode and this one on the same inputs through stream output and compares every
//     output register: bit-for-bit-ish (1e-5) with the viewer feed absent (pCam.w = 0, the game's own look), on every
//     alignment mode, and with the feed present only the basis may differ.
// The body of all three up to the corner is the same instruction sequence in the original bytes (the sprite family), so
// the basis, the alignment cascade and the spin are one HLSL function; everything that differs between them (the
// outputs, the near fade and the corner collapse in 9F4B and BBAD, BBAD's soft-depth term) is written per shader.
//
// WHAT CHANGES: the camera-facing mode's basis (alignment byte 0) and mode 4, which keeps that basis. The original
// builds right = normalize(cross(cb1[278], cb1[279])) and up = normalize(cross(cb1[279], right)) once per draw. Ours aims
// at the viewer per vertex:
//     face  = normalize(particlePosition - viewerPosition)
//     right = normalize(cross(worldUp, face))
//     up    = cross(face, right)
// and the spin axis of the camera-facing mode becomes the face axis. The aligned modes reference the particle's own
// direction and axis and are copied untouched, as are the near fade (cb1[279] stays the DEPTH axis) and every other
// output. The viewer position and world up arrive at b3, exactly as for the plume and flare; pCam.w = 0 means the solve
// was not available and the original camera-plane basis is used, so a missing feed degrades to the game's own look.
#pragma once

namespace edvr {

#define EDVR_FACING_COMMON_HLSL R"HLSL(
cbuffer CB0 : register(b0) { float4 cb0[12]; };
cbuffer CB1 : register(b1) { float4 cb1[280]; };
cbuffer CBP : register(b3) {
    float4 pCam;
    float4 pUp;
};

// The original's basis, alignment cascade and per-particle spin, up to and including the quaternion rotation of right and
// up. p is the particle's position in camera-relative world space (the original's r2). v3x is the alignment byte over 255;
// the thresholds are the original's, printed to six places in its disassembly. Outputs the rotated right and up.
void spriteBasis(float v0w, float3 v1, float v3x, float3 v5, float3 p, out float3 rightOut, out float3 upOut) {
    float3 fwd = cb1[279].xyz;
    float3 faceAxis = fwd;
    float3 r0, r3, r4;
    float  angle = v0w * 0.5;

    [branch] if (pCam.w > 0.5) {
        // Aimed at the viewer. The cross is guarded for real: the game never needed a guard (its two vectors are the
        // camera's own up and forward), ours can be parallel when a particle is directly above or below the viewer, and
        // rsqrt(0) times zero is a NaN. The game's own guard tests (x != 0) AFTER normalising, which keeps a NaN.
        float3 toCam = p - pCam.xyz;
        float  lenToCam = length(toCam);
        if (lenToCam > 1e-4) faceAxis = toCam / lenToCam;
        float3 bRight = cross(pUp.xyz, faceAxis);
        float  bLen2 = dot(bRight, bRight);
        [branch] if (bLen2 < 1e-8) {
            bRight = cross(float3(0.0, 0.0, 1.0), faceAxis);
            bLen2 = dot(bRight, bRight);
            [branch] if (bLen2 < 1e-8) {
                bRight = float3(1.0, 0.0, 0.0);
                bLen2 = 1.0;
            }
        }
        r0 = bRight * rsqrt(bLen2);
        r3 = cross(faceAxis, r0);
        r3 = r3 * rsqrt(max(dot(r3, r3), 1e-12));
    } else {
        // The original, as written: right from the camera's up and forward, the (x != 0) guard after the divide.
        r0 = cross(cb1[278].xyz, fwd);
        r0 = r0 * rsqrt(dot(r0, r0));
        [flatten] if (!(dot(r0, r0) != 0.0)) r0 = float3(fwd.y, 0.0, 0.0);
        r3 = cross(fwd, r0);
        r3 = r3 * rsqrt(dot(r3, r3));
    }

    [branch] if (v3x < 0.001961) {
        // Mode 0, the camera-facing mode: the spin axis is the face axis (the view axis in the original).
        r4 = faceAxis;
    } else {
        float3 d = v1 * 2.007874 - 1.0;
        float3 r5 = float3(dot(cb0[9].xyz, d), dot(cb0[10].xyz, d), dot(cb0[11].xyz, d));
        r5 = r5 * rsqrt(dot(r5, r5));
        [branch] if (v3x < 0.005882 || v3x < 0.009804) {
            // Modes 1 and 2 (two identical bodies in the original): across the line of sight, spin zeroed.
            float3 r6 = cross(p, r5);
            r0 = r6 * rsqrt(dot(r6, r6));
            r4 = cross(r0, fwd);
            r3 = r5;
            angle = 0.0;
        } else {
            float3 a = v5 * 2.007874 - 1.0;
            float3 r7 = float3(dot(cb0[9].xyz, a), dot(cb0[10].xyz, a), dot(cb0[11].xyz, a));
            r4 = r7 * rsqrt(dot(r7, r7));
            [branch] if (v3x < 0.013725) {
                r0 = cross(r4, r5);
                r3 = cross(r0, r4);
            } else {
                [branch] if (!(v3x < 0.017647)) {
                    r0 = cross(r4, r5);
                    r3 = cross(r0, r4);
                }
                // else mode 4: right and up stay the camera-facing basis above, spun about the axis.
            }
        }
    }

    // The spin as a quaternion, in the shape the original builds it: qv = axis * sin(half), qc = cos(half), and
    //     v' = v(2c^2 - 1) + 2*qv*dot(qv, v) + 2c*cross(qv, v)       (the axis is NOT renormalised, as in the original)
    float spinS, spinC;
    sincos(angle, spinS, spinC);
    float3 qv = r4 * spinS;
    float  twoC = spinC + spinC;
    float3 twoQv = qv + qv;
    float  scale = spinC * twoC - 1.0;
    rightOut = r0 * scale + twoQv * dot(qv, r0) + cross(qv, r0) * twoC;
    upOut    = r3 * scale + twoQv * dot(qv, r3) + cross(qv, r3) * twoC;
}
)HLSL"

// ---- 9F4BBCFCD3B68BC9: 12 in, 6 out; b0[12], b1[280], b2[3] ----
constexpr const char kFacing9F4BWorldVS[] = EDVR_FACING_COMMON_HLSL R"HLSL(
cbuffer CB2 : register(b2) { float4 cb2[3]; };

struct VSIn {
    float4 v0  : POSITION0;
    float4 v1  : DIRECTION0;
    float3 v2  : DIMENSIONS0;
    float4 v3  : ALIGNBLENDBRIGHT0;
    float  v4  : BRIGHTNESS0;
    float4 v5  : AXIS0;
    float2 v6  : VERTEXALPHA0;
    float4 v7  : UVSCURRENTDIFFUSE0;
    float4 v8  : UVSNEXTDIFFUSE0;
    float  v9  : TEXBLENDDIFFUSE0;
    float  v10 : ATLASINDICESDIFFUSE0;
    float4 v11 : COLOUR0;
};

struct VSOut {
    float4 o0 : TEXCOORD1;
    float4 o1 : TEXCOORD5;
    float2 o2 : TEXCOORD9;
    float4 o3 : TEXCOORD11;
    float4 o4 : TEXCOORD12;
    float4 o5 : SV_POSITION;
};

VSOut main(VSIn i) {
    VSOut o = (VSOut)0;
    float3 r0 = i.v0.xyz * cb1[222].z;
    float2 size = i.v2.xy * cb1[222].z;
    o.o2.y = max(abs(size.y), abs(size.x));

    uint packed = asuint(i.v10);
    o.o0.y = (float)(packed & 0x0000ffffu);
    o.o0.z = (float)(packed >> 16);
    o.o1.xy = i.v6.xy * i.v7.yw + i.v7.xz;

    float4 pos4 = float4(r0, 1.0);
    float3 p = float3(dot(cb0[9], pos4), dot(cb0[10], pos4), dot(cb0[11], pos4));

    // The near fade, from the true camera forward (cb1[279] stays the depth axis).
    float depth = dot(cb1[279].xyz, p);
    float fadeSpan = cb2[1].w * cb2[2].x;
    float nearEdge = -cb2[1].w * cb2[2].x + cb2[1].w;
    float t = saturate((-depth + cb2[1].w) / fadeSpan);
    float fade = (nearEdge < depth ? 1.0 : 0.0) * (1.0 - t);

    float3 right, up;
    spriteBasis(i.v0.w, i.v1.xyz, i.v3.x, i.v5.xyz, p, right, up);

    float3 corner = right * size.x + p;
    corner = up * size.y + corner;
    float keep = (fade < 0.001) ? 0.0 : 1.0;
    corner = (corner - p) * keep + p;

    float4 clip = corner.y * cb1[271];
    clip = corner.x * cb1[270] + clip;
    clip = corner.z * cb1[272] + clip;
    o.o5 = clip + cb1[273];

    o.o3.w = fade * i.v11.w;
    o.o3.xyz = i.v4.xxx * i.v11.xyz;
    o.o2.x = dot(corner, cb1[279].xyz);
    o.o1.zw = i.v6.xy * i.v8.yw + i.v8.xz;
    o.o0.x = i.v5.w;
    o.o0.w = i.v9;
    o.o4.xyz = float3(1.0, 1.0, 1.0);
    o.o4.w = i.v3.y * 255.0;
    return o;
}
)HLSL";

// ---- BBAD1CA808E1E292: 10 in, 3 out; b0[12], b1[280], b2[3] ----
constexpr const char kFacingBBADWorldVS[] = EDVR_FACING_COMMON_HLSL R"HLSL(
cbuffer CB2 : register(b2) { float4 cb2[3]; };

struct VSIn {
    float4 v0 : POSITION0;
    float4 v1 : DIRECTION0;
    float3 v2 : DIMENSIONS0;
    float4 v3 : ALIGNBLENDBRIGHT0;
    float  v4 : BRIGHTNESS0;
    float4 v5 : AXIS0;
    float2 v6 : VERTEXALPHA0;
    float4 v7 : UVSCURRENTDIFFUSE0;
    float  v8 : ATLASINDICESDIFFUSE0;
    float4 v9 : COLOUR0;
};

struct VSOut {
    float3 o0 : TEXCOORD3;
    float4 o1 : TEXCOORD5;
    float4 o2 : SV_POSITION;
};

VSOut main(VSIn i) {
    VSOut o = (VSOut)0;
    float3 r0 = i.v0.xyz * cb1[222].z;
    float2 size = i.v2.xy * cb1[222].z;

    uint packed = asuint(i.v8);
    o.o0.y = (float)(packed & 0x0000ffffu);
    o.o1.xy = i.v6.xy * i.v7.yw + i.v7.xz;

    float4 pos4 = float4(r0, 1.0);
    float3 p = float3(dot(cb0[9], pos4), dot(cb0[10], pos4), dot(cb0[11], pos4));

    float depth = dot(cb1[279].xyz, p);
    float fadeSpan = cb2[1].w * cb2[2].x;
    float nearEdge = -cb2[1].w * cb2[2].x + cb2[1].w;
    float t = saturate((-depth + cb2[1].w) / fadeSpan);
    float fade = (nearEdge < depth ? 1.0 : 0.0) * (1.0 - t);

    float3 right, up;
    spriteBasis(i.v0.w, i.v1.xyz, i.v3.x, i.v5.xyz, p, right, up);

    float3 corner = right * size.x + p;
    corner = up * size.y + corner;
    float keep = (fade < 0.001) ? 0.0 : 1.0;
    corner = (corner - p) * keep + p;

    float4 clip = corner.y * cb1[271];
    clip = corner.x * cb1[270] + clip;
    clip = corner.z * cb1[272] + clip;
    clip = clip + cb1[273];

    // The soft-depth term: with cb2[0].y zero, one over the square root of clip w; otherwise it scales with the quad's
    // own size.
    float soft;
    [branch] if (cb2[0].y == 0.0) {
        soft = rsqrt(max(clip.w, 0.0));
    } else {
        float s = max(abs(size.y), abs(size.x));
        float w = max(clip.w, 0.0);
        s = s * s;
        w = w * w;
        soft = (s * cb2[0].y) / (w * 1.944738 + s);
    }
    o.o0.z = fade * soft;
    o.o1.z = fade * i.v9.w;
    o.o1.w = i.v4.x;
    o.o2 = clip;
    o.o0.x = clip.w;
    return o;
}
)HLSL";

// ---- 9AEC596A2B036EA6: 10 in, 5 out; b0[12], b1[280], NO b2 ----
constexpr const char kFacing9AECWorldVS[] = EDVR_FACING_COMMON_HLSL R"HLSL(
struct VSIn {
    float4 v0 : POSITION0;
    float4 v1 : DIRECTION0;
    float3 v2 : DIMENSIONS0;
    float4 v3 : ALIGNBLENDBRIGHT0;
    float  v4 : BRIGHTNESS0;
    float4 v5 : AXIS0;
    float2 v6 : VERTEXALPHA0;
    float4 v7 : UVSCURRENTDIFFUSE0;
    float  v8 : ATLASINDICESDIFFUSE0;
    float4 v9 : COLOUR0;
};

struct VSOut {
    float4 o0 : TEXCOORD3;
    float2 o1 : TEXCOORD6;
    float4 o2 : TEXCOORD8;
    float4 o3 : TEXCOORD9;
    float4 o4 : SV_POSITION;
};

VSOut main(VSIn i) {
    VSOut o = (VSOut)0;
    float3 r0 = i.v0.xyz * cb1[222].z;
    float2 size = i.v2.xy * cb1[222].z;
    o.o1.y = max(abs(size.y), abs(size.x));

    uint packed = asuint(i.v8);
    o.o0.w = (float)(packed & 0x0000ffffu);
    o.o0.xy = i.v6.xy * i.v7.yw + i.v7.xz;

    float4 pos4 = float4(r0, 1.0);
    float3 p = float3(dot(cb0[9], pos4), dot(cb0[10], pos4), dot(cb0[11], pos4));

    float3 right, up;
    spriteBasis(i.v0.w, i.v1.xyz, i.v3.x, i.v5.xyz, p, right, up);

    float3 corner = right * size.x + p;
    corner = up * size.y + corner;

    float4 clip = corner.y * cb1[271];
    clip = corner.x * cb1[270] + clip;
    clip = corner.z * cb1[272] + clip;
    o.o4 = clip + cb1[273];

    o.o2.xyz = i.v4.xxx * i.v9.xyz;
    o.o1.x = dot(corner, cb1[279].xyz);
    o.o0.z = i.v5.w;
    o.o2.w = i.v9.w;
    o.o3.xyz = float3(1.0, 1.0, 1.0);
    o.o3.w = i.v3.y * 255.0;
    return o;
}
)HLSL";

// ---- AFBCD3ADB9092F78: 12 in, 5+position out (9F4B's signature); b0[12], b1[280], NO b2 ----
// The fireball puffs (eye dump 2026-10-16: AFBC + PS 2BAE3742FEB916D9). 9F4B's twin without the near fade: no cb2, no
// fade ramp, no corner collapse, and o3.w = v11.w. Written from AFBC's own bytecode; it resembles 9AEC (body similarity
// 0.864) more than its true twin 9F4B (0.727), which is exactly why nothing here is granted on a likeness.
constexpr const char kFacingAFBCWorldVS[] = EDVR_FACING_COMMON_HLSL R"HLSL(
struct VSIn {
    float4 v0  : POSITION0;
    float4 v1  : DIRECTION0;
    float3 v2  : DIMENSIONS0;
    float4 v3  : ALIGNBLENDBRIGHT0;
    float  v4  : BRIGHTNESS0;
    float4 v5  : AXIS0;
    float2 v6  : VERTEXALPHA0;
    float4 v7  : UVSCURRENTDIFFUSE0;
    float4 v8  : UVSNEXTDIFFUSE0;
    float  v9  : TEXBLENDDIFFUSE0;
    float  v10 : ATLASINDICESDIFFUSE0;
    float4 v11 : COLOUR0;
};

struct VSOut {
    float4 o0 : TEXCOORD1;
    float4 o1 : TEXCOORD5;
    float2 o2 : TEXCOORD9;
    float4 o3 : TEXCOORD11;
    float4 o4 : TEXCOORD12;
    float4 o5 : SV_POSITION;
};

VSOut main(VSIn i) {
    VSOut o = (VSOut)0;
    float3 r0 = i.v0.xyz * cb1[222].z;
    float2 size = i.v2.xy * cb1[222].z;
    o.o2.y = max(abs(size.y), abs(size.x));

    uint packed = asuint(i.v10);
    o.o0.y = (float)(packed & 0x0000ffffu);
    o.o0.z = (float)(packed >> 16);
    o.o1.xy = i.v6.xy * i.v7.yw + i.v7.xz;

    float4 pos4 = float4(r0, 1.0);
    float3 p = float3(dot(cb0[9], pos4), dot(cb0[10], pos4), dot(cb0[11], pos4));

    float3 right, up;
    spriteBasis(i.v0.w, i.v1.xyz, i.v3.x, i.v5.xyz, p, right, up);

    float3 corner = right * size.x + p;
    corner = up * size.y + corner;

    float4 clip = corner.y * cb1[271];
    clip = corner.x * cb1[270] + clip;
    clip = corner.z * cb1[272] + clip;
    o.o5 = clip + cb1[273];

    o.o3.xyz = i.v4.xxx * i.v11.xyz;
    o.o2.x = dot(corner, cb1[279].xyz);
    o.o1.zw = i.v6.xy * i.v8.yw + i.v8.xz;
    o.o0.x = i.v5.w;
    o.o0.w = i.v9;
    o.o3.w = i.v11.w;
    o.o4.xyz = float3(1.0, 1.0, 1.0);
    o.o4.w = i.v3.y * 255.0;
    return o;
}
)HLSL";

// ---- 4BDE4DBF88C2BA27: 10 in, 3 out (BBAD's interface); b0[12], b1[280], b2[1] ----
// BBAD's no-fade twin (PS E54F2A902E5631F6): the same soft-depth term on cb2[0].y, but no near fade, no collapse and no
// fade multiplied into the outputs. Its own bytecode reshuffles the registers and the mode branches; the maths is the
// sprite family's, which the rig checks output by output.
constexpr const char kFacing4BDEWorldVS[] = EDVR_FACING_COMMON_HLSL R"HLSL(
cbuffer CB2 : register(b2) { float4 cb2[1]; };

struct VSIn {
    float4 v0 : POSITION0;
    float4 v1 : DIRECTION0;
    float3 v2 : DIMENSIONS0;
    float4 v3 : ALIGNBLENDBRIGHT0;
    float  v4 : BRIGHTNESS0;
    float4 v5 : AXIS0;
    float2 v6 : VERTEXALPHA0;
    float4 v7 : UVSCURRENTDIFFUSE0;
    float  v8 : ATLASINDICESDIFFUSE0;
    float4 v9 : COLOUR0;
};

struct VSOut {
    float3 o0 : TEXCOORD3;
    float4 o1 : TEXCOORD5;
    float4 o2 : SV_POSITION;
};

VSOut main(VSIn i) {
    VSOut o = (VSOut)0;
    float3 r0 = i.v0.xyz * cb1[222].z;
    float2 size = i.v2.xy * cb1[222].z;

    uint packed = asuint(i.v8);
    float atlasLo = (float)(packed & 0x0000ffffu);
    o.o1.xy = i.v6.xy * i.v7.yw + i.v7.xz;

    float4 pos4 = float4(r0, 1.0);
    float3 p = float3(dot(cb0[9], pos4), dot(cb0[10], pos4), dot(cb0[11], pos4));

    float3 right, up;
    spriteBasis(i.v0.w, i.v1.xyz, i.v3.x, i.v5.xyz, p, right, up);

    float3 corner = right * size.x + p;
    corner = up * size.y + corner;

    float4 clip = corner.y * cb1[271];
    clip = corner.x * cb1[270] + clip;
    clip = corner.z * cb1[272] + clip;
    clip = clip + cb1[273];

    float soft;
    [branch] if (cb2[0].y == 0.0) {
        soft = rsqrt(max(clip.w, 0.0));
    } else {
        float s = max(abs(size.y), abs(size.x));
        float w = max(clip.w, 0.0);
        s = s * s;
        w = w * w;
        soft = (s * cb2[0].y) / (w * 1.944738 + s);
    }
    o.o1.z = i.v9.w;
    o.o1.w = i.v4.x;
    o.o2 = clip;
    o.o0 = float3(clip.w, atlasLo, soft);
    return o;
}
)HLSL";

#undef EDVR_FACING_COMMON_HLSL

}  // namespace edvr
