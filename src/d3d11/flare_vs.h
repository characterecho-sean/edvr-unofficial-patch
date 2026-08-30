// The replacement vertex shader for the SOLAR FLARE billboards.
//
// Written against the disassembly of the game's own vs 6041FD2D3D0164E1,
// dumped 2026-08-29 and field-confirmed the same evening: with that hash in
// census_skip the prominences erupting off star surfaces disappear, and
// nothing else does.
//
// It is the SAME BUG as the geyser plume -- the billboard basis built once
// per draw from the camera's own up and forward:
//
//     right = normalize(cross(cb1[278], cb1[279]))
//     up    = normalize(cross(cb1[279], right))
//
// so every quad in the draw is parallel to the screen plane instead of
// facing the viewer. In a headset that couples the flare to the head: it
// rolls when you roll, and because each quad's normal points along the view
// axis rather than at your eye, a flare off to one side is foreshortened by
// its eccentricity -- and that foreshortening changes as you look around.
// See docs/particle-billboards.md; cb1[278] is cameraUp and cb1[279]
// cameraForward, corroborated by cb1[279]'s second use as the depth term.
//
// WHY THIS IS A SECOND FILE AND NOT A SECOND HASH
//
// particle_vs.h cannot be pointed at this draw. The plume's shader takes
// THIRTEEN inputs and writes TWELVE outputs; this one takes ten and writes
// five (TEXCOORD 2, 4, 7, 9 and SV_POSITION). Substituting a shader whose
// signature does not match the input layout and the pixel shader behind it
// is not a fix, it is a different kind of breakage. The particle family has
// at least seven members and they do not share a signature, so each
// signature group needs its own transcription. This is the second.
//
// TWO PLACES THIS SHADER DIFFERS FROM THE PLUME'S, both transcribed from
// THIS bytecode rather than assumed from the other:
//
//   * Alignment modes 3 and 5+ build their basis from the AXIS input with
//     NO normalise -- the plume's equivalent path normalises. Copying the
//     plume here would silently change every axis-aligned flare's size.
//   * There is no cb2 and no near-fade. The plume multiplies its colour by
//     a distance fade computed from cb2[2]/cb2[3]; this shader declares
//     only b0 and b1 and has no such term. Its o2.x is a plain depth along
//     cameraForward, nothing more.
//
// ONE thing is changed, and it is the bug: in the camera-facing mode the
// basis is rebuilt PER VERTEX, aimed at the viewer.
//
//     face  = normalize(particlePosition - viewerPosition)
//     right = normalize(cross(worldUp, face))
//     up    = cross(face, right)
//
// The viewer position arrives in our own constant buffer at b3, solved
// CPU-side from the game's view-projection rows, because the space these
// particle positions live in is the game's and not one we get to choose.
// The aligned modes reference the particle's own direction and axis; they
// were never wrong and are copied untouched.
#pragma once

namespace edvr {

constexpr const char kFlareWorldVS[] = R"HLSL(
cbuffer CB0 : register(b0) { float4 cb0[12]; };
cbuffer CB1 : register(b1) { float4 cb1[280]; };
// Ours: the viewer's position in the same space the particle positions end
// up in, and the world's up axis. pCam.w is a flag -- zero means the solve
// was not available this draw and the original camera-plane basis is used,
// so a missing feed degrades to the game's own look rather than to garbage.
cbuffer CBP : register(b3) {
    float4 pCam;
    float4 pUp;
};

struct VSIn {
    float4 v0 : POSITION0;              // xyz local position, w spin angle
    float4 v1 : DIRECTION0;
    float3 v2 : DIMENSIONS0;            // SIGNED half extents per corner
    float4 v3 : ALIGNBLENDBRIGHT0;      // x align mode (x255), y blend
    float  v4 : BRIGHTNESS0;
    float4 v5 : AXIS0;
    float2 v6 : VERTEXALPHA0;           // the corner, 0..1
    float4 v7 : UVSCURRENTDIFFUSE0;
    float  v8 : ATLASINDICESDIFFUSE0;   // two uint16 packed in the bits
    float4 v9 : COLOUR0;
};

struct VSOut {
    float4 o0 : TEXCOORD2;
    float4 o1 : TEXCOORD4;
    float2 o2 : TEXCOORD7;
    float4 o3 : TEXCOORD9;
    float4 o4 : SV_POSITION;
};

VSOut main(VSIn i) {
    VSOut o = (VSOut)0;
    float4 r0, r1, r2, r3, r4, r5, r6, r7;

    r0.xyz = i.v0.xyz * cb1[222].z;
    r1.xy  = i.v2.xy * cb1[222].z;
    o.o2.y = max(abs(r1.y), abs(r1.x));

    uint packed = asuint(i.v8);
    o.o1.w = (float)(packed & 0x0000ffffu);

    o.o1.xy = i.v6.xy * i.v7.yw + i.v7.xz;

    r0.w = 1.0;
    r2.x = dot(cb0[9],  r0);
    r2.y = dot(cb0[10], r0);
    r2.z = dot(cb0[11], r0);

    // ---- the billboard basis ----
    // The game's own is perpendicular to the camera's forward, which is the
    // whole artifact. Ours is perpendicular to the direction from the viewer
    // to THIS particle, which is what facing the viewer means.
    float3 faceAxis = cb1[279].xyz;
    if (pCam.w > 0.5) {
        float3 toCam = r2.xyz - pCam.xyz;
        float lenToCam = length(toCam);
        if (lenToCam > 1e-4) faceAxis = toCam / lenToCam;
    }
    float3 basisUp = (pCam.w > 0.5) ? pUp.xyz : cb1[278].xyz;

    // The cross, guarded for real. The game never needed a guard here: its
    // two vectors are the camera's own up and forward, which cannot be
    // parallel. Ours can -- faceAxis points at THIS particle, and a
    // prominence directly above or below the viewer lines up with world up
    // exactly. There the cross collapses, rsqrt(0) is infinity, and zero
    // times infinity is a NaN that propagates into the vertex position: one
    // particle becomes a triangle with no finite corner, which the
    // rasteriser is free to smear across the frame. That is what took the
    // terrain out when the plume fix was built.
    //
    // The game's own guard cannot help. It tests the result with (x != 0),
    // and a NaN compares unequal to everything, so it keeps the NaN it
    // meant to reject. Testing the LENGTH before dividing is the version
    // that works.
    float3 bRight = cross(basisUp, faceAxis);
    float bLen2 = dot(bRight, bRight);
    [branch] if (bLen2 < 1e-8) {
        bRight = cross(float3(0.0, 0.0, 1.0), faceAxis);
        bLen2 = dot(bRight, bRight);
        [branch] if (bLen2 < 1e-8) {
            bRight = float3(1.0, 0.0, 0.0);
            bLen2 = 1.0;
        }
    }
    r0.xyz = bRight * rsqrt(bLen2);

    r3.xyz = cross(faceAxis, r0.xyz);
    r0.w = max(dot(r3.xyz, r3.xyz), 1e-12);
    r3.xyz = r3.xyz * rsqrt(r0.w);

    // ---- alignment mode ----
    // The thresholds are the original's: v3.x carries a mode byte over 255,
    // tested against 0.5/255, 1.5/255, 2.5/255, 3.5/255 and 4.5/255.
    r0.w = i.v0.w * 0.5;
    uint mode = (uint)(i.v3.x * 255.0 + 0.5);
    [branch] if (mode == 0) {
        // The camera-facing mode -- the one carrying the bug. The spin axis
        // becomes the direction to the viewer, so the quad turns about its
        // own normal instead of about the view axis.
        r4.xyz = faceAxis;
    } else {
        r5.xyz = i.v1.xyz * 2.007874 - 1.0;
        r6.x = dot(cb0[9].xyz,  r5.xyz);
        r6.y = dot(cb0[10].xyz, r5.xyz);
        r6.z = dot(cb0[11].xyz, r5.xyz);
        r5.xyz = r6.xyz * rsqrt(dot(r6.xyz, r6.xyz));
        [branch] if (mode == 1 || mode == 2) {
            r6.xyz = cross(r2.xyz, r5.xyz);
            r0.xyz = r6.xyz * rsqrt(dot(r6.xyz, r6.xyz));
            r4.xyz = cross(r0.xyz, cb1[279].xyz);
            r3.xyz = r5.xyz;
            r0.w = 0.0;
        } else {
            r6.xyz = i.v5.xyz * 2.007874 - 1.0;
            r7.x = dot(cb0[9].xyz,  r6.xyz);
            r7.y = dot(cb0[10].xyz, r6.xyz);
            r7.z = dot(cb0[11].xyz, r6.xyz);
            r4.xyz = r7.xyz * rsqrt(dot(r7.xyz, r7.xyz));
            [branch] if (mode != 4) {
                // NOT normalised, in this shader. The plume's equivalent
                // path normalises; copying that here would change the size
                // of every axis-aligned flare.
                r0.xyz = cross(r4.xyz, r5.xyz);
                r3.xyz = cross(r0.xyz, r4.xyz);
            }
        }
    }

    // ---- the per-particle spin, as a quaternion, in the shape the
    // original builds it: qv = axis * sin(half), qc = cos(half), and
    //     v' = v(2c^2 - 1) + 2*qv*dot(qv, v) + 2c*cross(qv, v)
    // The axis is NOT renormalised first here, matching the original ----
    float spinS, spinC;
    sincos(r0.w, spinS, spinC);
    float3 qv = r4.xyz * spinS;
    float  qc = spinC;
    float  twoC = qc + qc;
    float3 twoQv = qv + qv;
    float  scale = qc * twoC - 1.0;

    float3 crossR = cross(qv, r0.xyz);
    float  dotR   = dot(qv, r0.xyz);
    float3 rotR   = r0.xyz * scale + twoQv * dotR + crossR * twoC;

    float3 crossU = cross(qv, r3.xyz);
    float  dotU   = dot(qv, r3.xyz);
    float3 rotU   = r3.xyz * scale + twoQv * dotU + crossU * twoC;

    r0.xyz = rotR;
    r3.xyz = rotU;

    // ---- the quad corner, then to clip space ----
    r0.xyz = r0.xyz * r1.x + r2.xyz;
    r0.xyz = r3.xyz * r1.y + r0.xyz;

    r1 = r0.y * cb1[271];
    r1 = r0.x * cb1[270] + r1;
    r1 = r0.z * cb1[272] + r1;
    r1 = r1 + cb1[273];

    // ---- the remaining outputs, verbatim ----
    o.o3.xyz = i.v4.xxx * i.v9.xyz;
    o.o2.x   = dot(r0.xyz, cb1[279].xyz);
    o.o0.xyz = r1.xyw;
    o.o0.w   = i.v3.y * 255.0;
    o.o1.z   = i.v5.w;
    o.o3.w   = i.v9.w;
    o.o4     = r1;
    return o;
}
)HLSL";

}  // namespace edvr
