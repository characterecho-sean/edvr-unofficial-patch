// A STRETCHED, AGE-RAMPED PARTICLE BILLBOARD, vh=68DDDEF04D9894AF, dumped
// from the game 2026-09-06 (edvr_logs\shaders, glare_shader_dump) and
// disassembled with fxc. Read here on 2026-09-07 while hunting the
// explosion sprites Sean reported swimming with head motion. It is NOT
// those (see FIELD PROVENANCE below) -- but it is a camera-locked
// billboard family nobody had named, with the same complaint the smoke
// plume's fix answers, so the reading is kept.
//
// WHY IT IS HERE, AND WHAT IT IS NOT
//
// It is camera-facing, and that is the swimming mechanism: the quad's
// corners are laid out along cb1[277] and cb1[278], so rolling your head
// rolls every sprite. See the last third of the program:
//
//     mad r0.xyz, cb1[277].xyzx, r1.xxxx, r0.yzwy    // += basis A * sx
//     mad r0.xyz, cb1[278].xyzx, r1.yyyy, r0.xyzx    // += basis B * sy
//
// with r1.xy the size curve times the corner offsets in v0.zw.
//
// It is NOT the smoke plume (EB787F983BC1F5A3, docs/shaders/particle-vs.asm)
// and must not be granted the plume's replacement. The plume DERIVES its
// right vector at runtime with a cross product:
//
//     mul r0.xyz, cb1[278].zxyz, cb1[279].yzxy
//     mad r0.xyz, cb1[278].yzxy, cb1[279].zxyz, -r0.xyzx   // cross(up, fwd)
//
// This shader computes no cross at all -- it is handed both axes and uses
// them directly. Different body, different registers consumed, so a
// transcription written for one is wrong for the other. That is the trap
// src\d3d11\particle_fix.cpp:61-67 exists to name: a hash earns a
// replacement only by having its own disassembly read and its own
// transcription written, because granting one on likeness deleted the
// witchspace starfield in 0.12.3.
//
// WHAT ELSE IT SAYS (measured from the bytecode below)
//
//   * A velocity stretch, gated on v1.x >= 0.01, built from cb1[279] and a
//     direction taken from v2.xyz, normalised, then rotated by the 3x3 at
//     cb1[64..66]. A stretched spark rather than a round puff.
//   * v2.w is a birth time; cb2[0..5] carry fade and colour ramps driven by
//     age; the size is an exponential curve times (v4.x + 1).
//   * v0.xy is the UV, v0.zw the corner offsets, v3.xyz a colour scaled by
//     cb1[72].w, v4 per-particle data passed straight through to o4.
//   * Position leaves through cb1[270..273], the usual view-projection.
//
// INFERRED, NOT MEASURED: that cb1[277] is the camera's RIGHT. It is used
// as one of two axes spanning the quad, and cb1[278] is the camera's up in
// the plume's cross, so right is the natural partner -- but no log has ever
// printed these registers' contents. particle_fix.cpp already samples
// cb1[276..279] (kFirstReg/kRegs), so one session with
// fix.particle_billboard = steady would settle it.
//
// FIELD PROVENANCE, and a correction worth reading before you trust any
// earlier note: THIS IS NOT THE EXPLOSION SHADER. It was first written up
// as the candidate for the explosion sprites, and that was wrong. The
// explosions were flown in edvr_gfx_20260907_182944.log; this hash appears
// ZERO times in that log. It appears only in the 18:51 re-fly
// (edvr_gfx_20260907_185152.log), which had no explosions in it -- 6 draws
// across censuses 1, 2 and 3, up to 8,869 instances in a single draw,
// ds=17wZ, late in the frame.
//
// The explosion family remains UNIDENTIFIED, and the reason is instrumental
// rather than mysterious: both censuses in the explosion flight were cut by
// the log's 1 MB buffer before the effects pass ran, so neither contains a
// particle billboard of ANY family. The largest instanced draws surviving
// in that log are 49 and 14 instances, which are the two depth-disabled
// bit-4 readers, not particles. Naming the explosion shader needs another
// flight with explosions in it and the buffer fixed.
//
// WHAT THIS SHADER IS, from its census lines (2026-09-07). Sean's guess was
// station holograms; the draw state argues against it, and for a BACKGROUND
// STAR OR DUST FIELD:
//
//   * Its viewport collapses the depth range to z=0.000-0.000. In Elite's
//     reversed-Z that is the FAR plane, and only 18 of 15,070 recorded draws
//     in that log do it -- 14,519 use the ordinary 0.000-1.000.
//   * With the depth test GEQUAL and the fragment forced to 0, it passes
//     ONLY where the depth buffer is still at the far plane, i.e. on
//     unoccluded sky. A hologram inside a station sits at a finite distance
//     and has to depth-test normally against the structure around it.
//   * n=12 i=7273..8869: two triangles an instance, thousands of them,
//     additive (bl=15,2,1 = SRC_ALPHA + ONE), no depth write (ds=17wZ).
//   * At the one moment near census 1 where the probe sampled the scene
//     pair, 141-174 of 256 sampled pixels were at the far plane and most of
//     the rest under 2 m: open sky with the cockpit in the near field, not
//     an enclosed interior.
//
// So: an unfixed camera-locked billboard family in no list this project
// keeps, worth pinning on its own merit for the same reason the plume was --
// but it is the sky layer, not the holograms and not the explosions.
//
// STILL UNCAPTURED, both of them: the explosion sprites (flown, but both
// censuses were cut before the effects pass) and the station holograms (no
// census on this machine has been taken inside a station).
//
// NOT YET TRANSCRIBED. Nothing replaces this shader today.
//
// Generated by Microsoft (R) D3D Shader Disassembler
//
//
// Input signature:
//
// Name                 Index   Mask Register SysValue  Format   Used
// -------------------- ----- ------ -------- -------- ------- ------
// TEXCOORD                 0   xyzw        0     NONE   float   xyzw
// TEXCOORD                 2   x           1     NONE   float   x   
// POSITION                 0   xyzw        2     NONE   float   xyzw
// POSITION                 1   xyzw        3     NONE   float   xyz 
// TEXCOORD                 3   xyzw        4     NONE   float   xyzw
//
//
// Output signature:
//
// Name                 Index   Mask Register SysValue  Format   Used
// -------------------- ----- ------ -------- -------- ------- ------
// TEXCOORD                 0   xyz         0     NONE   float   xyz 
// TEXCOORD                 1   xyzw        1     NONE   float   xyzw
// TEXCOORD                 2   xyz         2     NONE   float   xyz 
// TEXCOORD                 3   xyz         3     NONE   float   xyz 
// TEXCOORD                 5   xyzw        4     NONE   float   xyzw
// TEXCOORD                 6   xy          5     NONE   float   xy  
// SV_POSITION              0   xyzw        6      POS   float   xyzw
//
vs_5_0
dcl_globalFlags refactoringAllowed
dcl_constantbuffer CB0[2], immediateIndexed
dcl_constantbuffer CB1[280], immediateIndexed
dcl_constantbuffer CB2[6], immediateIndexed
dcl_input v0.xyzw
dcl_input v1.x
dcl_input v2.xyzw
dcl_input v3.xyz
dcl_input v4.xyzw
dcl_output o0.xyz
dcl_output o1.xyzw
dcl_output o2.xyz
dcl_output o3.xyz
dcl_output o4.xyzw
dcl_output o5.xy
dcl_output_siv o6.xyzw, position
dcl_temps 4
add r0.x, -cb1[72].z, l(1.000000)
mul r0.x, r0.x, cb2[1].x
add r0.y, cb1[72].z, l(-1.000000)
mul r0.xy, r0.xyxx, cb2[0].wxww
ge r0.z, cb1[72].z, l(1.000000)
movc r0.x, r0.z, -r0.y, r0.x
add r0.x, r0.x, v2.w
add r0.y, r0.x, -cb2[1].x
mad r0.x, -cb2[0].z, r0.y, r0.x
ge r0.y, r0.x, cb2[1].x
and r0.y, r0.y, l(0x3f800000)
mul r1.xyz, cb1[279].xyzx, l(10.000000, 10.000000, 10.000000, 0.000000)
mad r2.xyz, v2.xyzx, cb0[0].xyzx, cb0[1].xyzx
dp3 r0.z, r2.xyzx, r2.xyzx
rsq r0.z, r0.z
mul r2.xyz, r0.zzzz, r2.xyzx
dp3 r3.x, cb1[64].xyzx, r2.xyzx
dp3 r3.y, cb1[65].xyzx, r2.xyzx
dp3 r3.z, cb1[66].xyzx, r2.xyzx
mad r1.xyz, -r3.xyzx, l(10.000000, 10.000000, 10.000000, 0.000000), -r1.xyzx
mul r2.xyz, r3.xyzx, l(10.000000, 10.000000, 10.000000, 0.000000)
mad r0.yzw, r0.yyyy, r1.xxyz, r2.xxyz
mad r1.xyz, -cb1[279].xyzx, l(10.000000, 10.000000, 10.000000, 0.000000), -r0.yzwy
ge r1.w, v1.x, l(0.010000)
and r1.w, r1.w, l(0x3f800000)
mad r0.yzw, r1.wwww, r1.xxyz, r0.yyzw
mov o0.xyz, r0.yzwy
add r1.x, r0.x, -cb2[1].x
add r0.x, r0.x, -cb2[5].z
add r1.y, cb2[0].y, -cb2[1].x
div_sat r1.x, r1.x, r1.y
lt r1.y, l(0.000000), r1.x
add r1.zw, -cb2[3].xxxy, l(0.000000, 0.000000, 1.000000, 1.000000)
mad r1.x, r1.z, r1.x, cb2[3].x
and r1.x, r1.x, r1.y
log r1.x, r1.x
mul r1.x, r1.x, cb2[5].x
exp r1.x, r1.x
min r1.x, r1.x, l(1.000000)
mov o1.w, r1.x
mul o1.xyz, v3.xyzx, cb1[72].wwww
add r1.y, cb2[0].y, -cb2[5].z
div r1.y, l(1.000000, 1.000000, 1.000000, 1.000000), r1.y
mul_sat r0.x, r0.x, r1.y
mad r1.y, r0.x, l(-2.000000), l(3.000000)
mul r0.x, r0.x, r0.x
mad r0.x, -r1.y, r0.x, l(1.000000)
log r0.x, r0.x
mul r0.x, r0.x, cb2[5].w
exp r0.x, r0.x
add_sat r1.y, r0.x, -cb2[3].y
div r1.y, r1.y, r1.w
mul r2.z, r1.y, cb2[3].z
add_sat r1.y, r0.x, -cb2[2].z
add r1.z, -cb2[2].z, l(1.000000)
div r1.y, r1.y, r1.z
mul r2.x, r1.y, cb2[2].w
add_sat r1.y, r0.x, -cb2[4].z
add r1.z, -cb2[4].z, l(1.000000)
div r1.y, r1.y, r1.z
mul r2.y, r1.y, cb2[4].w
mul o2.xyz, r2.xyzx, cb2[1].zzzz
add r1.y, cb2[3].w, -cb2[4].x
mad r1.x, r1.x, r1.y, cb2[4].x
add_sat r1.y, -cb2[3].w, cb2[4].y
mad r0.x, r0.x, r1.y, r1.x
log r0.x, r0.x
mul r0.x, r0.x, cb2[5].y
exp r0.x, r0.x
add r1.x, v4.x, l(1.000000)
mul r0.x, r0.x, r1.x
mul r1.xy, r0.xxxx, v0.zwzz
mad r0.xyz, cb1[277].xyzx, r1.xxxx, r0.yzwy
mad r0.xyz, cb1[278].xyzx, r1.yyyy, r0.xyzx
mov o3.xyz, r0.xyzx
mov o4.xyzw, v4.xyzw
mov o5.xy, v0.xyxx
mul r1.xyzw, r0.yyyy, cb1[271].xyzw
mad r1.xyzw, r0.xxxx, cb1[270].xyzw, r1.xyzw
mad r0.xyzw, r0.zzzz, cb1[272].xyzw, r1.xyzw
add o6.xyzw, r0.xyzw, cb1[273].xyzw
ret 
// Approximately 0 instruction slots used
