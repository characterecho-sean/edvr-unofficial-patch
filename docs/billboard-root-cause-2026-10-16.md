# Particle billboards: is there one engine-side root cause? (2026-10-16)

## Status

- **State (2026-10-16, after the flight):** the narrow E1 is FLOWN and approved for main (branch
  `claude/billboard-roll-free`): explosions no longer roll. Remaining: they FLEX when the head pitches (see
  "Flight", below). The three explosion shaders therefore moved to per-particle facing TRANSCRIPTIONS on branch
  `claude/billboard-facing` (not flown): first the three, then, after the eye dump found the fireball puffs, `AFBC` and `4BDE` too; 11 stay roll-free. The analysis further down is unchanged.
  Companion to `docs/particle-billboards.md`, whose 2026-10-16 "facing" entry holds the proof.
- **Global E1 is REFUTED** (below, "2026-10-16: E1 narrowed"): about 190 G-buffer pixel shaders
  encode normals with `cb1[277..279]`, so rows written into the game's block would change lighting.
  What is built is vertex-slot-1-only, per draw, for 16 billboard vertex shaders, as part of
  `fix.particle_billboard = steady` (no new key).
- **Next flight:** the per-particle facing transcriptions for the explosion shaders (branch
  `claude/billboard-facing`), read with the same 10 s `billboard roll-free` line.
- **Verdict:** the single engine-side root cause exists and the arc already
  named it: every camera-facing billboard family spans its quad on the camera
  basis in the scene constant buffer, `cb1[277]` (right, inferred), `cb1[278]`
  (up) and `cb1[279]` (forward). That is a GEOMETRY bug. It shows with temporal
  AA off and on a monitor. It is not a motion-vector bug, so no velocity fix can
  make `fix.particle_billboard = steady` unnecessary.
- **What steady changes:** only the basis, per vertex (face = particle minus
  the viewer, up = world up). It leaves motion vectors, depth, blend and history
  weight alone. Its design says the bug is the basis.
- **The cheaper engine-level fix that exists (E1; the global form below is refuted, the narrow form is built):** rewrite
  `cb1[277]`/`cb1[278]` once per view, in the game's mapped scene CB at Unmap, to
  a roll-free basis. That fixes ROLL for every family at once (the 7 shaders,
  plus `68DDDEF04D9894AF` and the untranscribed ones), with no shader swap and
  no per-draw work. It cannot fix the eccentric foreshortening, because facing
  the viewer is per particle and only a vertex shader can read the particle.
  Steady is the minimal COMPLETE fix, and its cost is in how it is implemented,
  not in what it does (see "Cost" below).
- **Open hypotheses:**
  - H1: REFUTED for a global rewrite (190 normal-encoding pixel shaders, some light-vector vertex
    shaders); TRUE for the 16 vertex shaders in the list below, each disassembled.
  - H2: `[277..279]` are per eye, and differ between eyes on canted displays
    (Pimax), which gives the stock quads a different orientation in each eye.
  - H3: part of steady's frame cost is inherent: un-foreshortened peripheral
    quads mean more translucent fill. The rest is CPU overhead.
- **Ruled out:**
  - ruled out: a motion-vector root cause that would retire steady, because
    steady changes only the quad's geometry, and the roll and swim show with
    temporal AA off and on a monitor (particle-billboards.md "The symptom").
    Correct motion vectors would make the TAA faithfully reconstruct a plume
    that still rolls with the head.
  - ruled out: a per-draw constant fix for foreshortening, because the facing
    is per particle (particle_fix.cpp:228-233; the per-emitter facing retired
    2026-09-23, particle_fix.cpp:208-224).
- **Evidence needed before E1:** (1) a corpus scan of every dumped VS and PS for
  `cb1[277]`/`[278]` reads, where cb1 is the 5376-byte scene CB (H1); (2) one
  log of `cb1[276..279]` for both eyes on Pimax and on SteamVR, which settles
  H2 and the "[277] = right" inference. The probe that particle_fix.h:15-18
  describes no longer exists; only `shapeOk` reads those registers.
- **Temporary keys:** none added.

## The engine side, from the bytecode and the census

Plume VS `EB787F983BC1F5A3` (docs/shaders/particle-vs.asm):

- Position: `v0` (per vertex) times `cb1[222].z`, then through `cb0[9..11]`, the
  208-byte camera block's world rows. The result is camera-relative world space.
- Basis: `right = normalize(cross(cb1[278], cb1[279]))` and
  `up = normalize(cross(cb1[279], right))`, built in the VS from the scene
  constants. It is not built on the CPU and not stored per particle. The
  velocity-aligned and axis-aligned modes (ALIGNBLENDBRIGHT) use the particle's
  own DIRECTION and AXIS instead and are correct.
- `cb1[279]` also drives the near-fade (`dp3 cb1[279], pos`, line 102). A fade
  under 0.001 collapses the quad (lines 188-191).
- Clip: `cb1[270..273]`, the view projection. `o1` passes the clip x, y and w to
  the PS, the soft-particle depth fade.
- No previous-frame position is output, and nothing writes velocity.
- The stretched sky billboard `68DDDEF04D9894AF` takes `cb1[277]` and `cb1[278]`
  directly (particle-stretched-vs.asm:12-16). Same registers, different body.
  `DB113566B6A59C5B` (a supplied axis or world up) is the correct member.

Census row (analysis/npc_blur/log_1112_1115.txt, 11:13:50, eye-sized viewport
2016x1949):

    X n=72 i=1 vh=EB787F983BC1F5A3 sd=92 ia=0 / ia=72 ds=02wA bl=12,6,1/2,6,1

- `n=72 i=1`, stride 92: 12 quads in one draw, with the whole particle record
  repeated on each vertex and a signed DIMENSIONS value per corner. The CPU
  writes 4 vertices per particle, which implies a buffer simulated and refilled
  each frame. The buffer's Usage is not in the census.
- `ds=02wA`: the depth test is DISABLED, so nothing is written to depth (the
  write mask is moot).
- `bl=1 2,6,1`: premultiplied alpha (ONE, INV_SRC_ALPHA, ADD).
- per-object-motion.md:1152-1160: the 336-stride engine record never reaches
  these families. (That passage's "instanced-billboard particle cluster",
  `0357BBB2...`/`8289669D...`, is the light volumes per gpu-pass-profile-2026-10-12.md.
  The label is wrong and nothing here depends on it.)

Ghidra: the CPU writer of registers 277-279 was not located, and nothing here
needs it. `FUN_143d0cf10` stores doubles at +0x1150..+0x1170 of a CPU camera
object (analysis/decomp/flash/decomp_3D0CF10.txt), which is not the CB. EDVR's
hook is the D3D Map/Unmap of the scene CB. vscreen.cpp already tees that
buffer: `camResource`, then `temporalPassNoteSceneWrite`.

## The candidates

| | candidate | verdict | discriminating evidence |
|---|---|---|---|
| a | view-dependent orientation missing from the motion vectors | TRUE, but it is geometry first | roll and swim show with AA off (the symptom list); steady changes only the basis |
| b | per-eye orientation | INFERRED for canted displays (H2) | `cb1[270..273]` is necessarily per eye, so the block is too; `[277..279]` per eye is unlogged |
| c | no previous state for dynamic particle buffers | TRUE, and out of reach | CPU-expanded stride-92 stream, no record, no previous output; the flipbook (UVSCURRENT/NEXT, TEXBLEND) cannot be expressed as motion at all |
| d | translucency without depth, so the motion vectors belong to whatever is behind | TRUE, and it is the TAA problem | `ds=02wA`; dlss-blur-elements-2026-10-14.md group G1; fixed EDVR-side (coverage or reactive), not by steady |

Steady encodes (a)+(b) as geometry. Under TAA it also helps (a) incidentally:
a steady quad is world-fixed under a pure head rotation, so the camera-term
reprojection is right for it, apart from (d).

## Engine-level fixes

- **E1, the roll-free basis in the scene CB.** At the scene CB's Unmap, before
  the real Unmap, write `[277] = normalize(cross(worldUp, fwd))` and
  `[278] = cross(fwd, [277])` into the game's mapped memory. Take `fwd` from the
  view rows EDVR already copies, or read `[279]` (16 bytes). Cost: once per
  view, 32 bytes written. Writes to write-combined memory are cheap; it is the
  reads that are slow (issue-65 doc). It needs no draw-ladder term, no shader
  swap and no per-family transcription. Covers: roll, for every family,
  including the ones never transcribed. Leaves: the foreshortening, and the
  per-eye forward. Risk: any non-billboard reader of `[277]`/`[278]` (H1); and
  the world-up assumption, which steady also makes ((0,1,0); level up measured
  at (0.056, 0.981, 0.185)).
- **E2, one basis for both eyes.** Write the head-centre forward into both
  eyes' `[279]` as well. That fixes the canted-display stereo mismatch (H2), but
  it also moves the near-fade depth axis per eye. Only if H2 is measured.
- **E3, full facing.** Per particle, so a vertex shader is required: that is
  steady. No single-point engine fix exists outside the game's code.
- **E4, previous-basis motion vectors.** Rejected: (d) means the TAA never
  samples a billboard's own motion, and correct motion would preserve the roll.

## Cost: where steady's price can come from (for the pricing pass, not priced here)

1. `particleWantsDraws()` holds the per-draw ladder open by itself
   (vscreen.cpp:1689). With temporal AA and FSR off it may be the term that
   keeps the gate open, if nothing else default-on subscribes.
2. `particleCapture` memcpys the WHOLE scene CB (ByteWidth, 5376 B) out of the
   game's mapped pointer at every Unmap (vscreen.cpp:3345-3349, 3437-3439;
   particle_fix.cpp:390-398). That pointer is write-combined on AMD (issue-65
   doc:324, Windows AMD 1-3 GB/s). The fix uses 160 bytes, `[270..279]`. This
   is a second read of a buffer EDVR's camera tee already reads.
3. Per matched draw: VSGetConstantBuffers, a 32-byte Map, the 3x3 solve
   (`solveCamera`, which runs per draw rather than per Unmap), VSGetShader plus
   two Sets plus two restores.
4. GPU, inherent to correct facing: peripheral quads are no longer
   foreshortened (cos of the eccentricity), so wide-FOV headsets draw more
   soft-particle fill. An engine fix with full facing would pay this too.

Discriminator: steady against stock in a scene where the steady note reports
0 draws. If fps still drops there, the cost is 1 or 2, not the substitution.

## 2026-10-16: E1 narrowed, and built (branch `claude/billboard-roll-free`)

### Global E1 is refuted

The RE pass over the dumped corpus found about 190 G-buffer pixel shaders that encode normals with
`cb1[277..279]`, and vertex shaders that move sun and light vectors into view space with them. Rows written
into the game's mapped block (E1 as designed above) would change lighting everywhere. H1 is false for a
rewrite of the game's block.

### The family is 18 vertex shaders, the fix is vertex-slot-1-only

The billboard vertex shaders, each disassembled with `D3DDisassemble` from the dumps in
`edvr_logs\shaders` (Frontier and Steam copies) before inclusion. Rule: every use of `cb1[277]` and
`cb1[278]` must be the quad span (or a span's fallback up), nothing for lighting or a view-space vector;
`cb1[279]` is never changed, so its uses do not matter.

| form | hashes | uses of [277]/[278] |
|---|---|---|
| cross | `AFBCD3ADB9092F78` `BBAD1CA808E1E292` `1B285CBC9F185D4D` `4BDE4DBF88C2BA27` `78F5F08D02EE38CC` `9F4BBCFCD3B68BC9` `2BF557356A28AA0D` `9AEC596A2B036EA6` | `cross(cb1[278], cb1[279])` only; [277] unread |
| cross with a supplied axis | `1FBA99C4911D7ACF` `DB113566B6A59C5B` | `movc axis, cb3[1], cb1[278]`: [278] only as the fallback up |
| direct span | `68DDDEF04D9894AF` `F7A6E916F14A3B1A` | `pos += cb1[277]*x + cb1[278]*y` |
| pool-record sprite | `12382D2EA45E9632` `94EA991C3A03DBD9` `C31238331D8AC3D4` `F1502353BE051A2D` | the same direct span (these declare `cb1[279]`) |

Not touched: `EB787F983BC1F5A3` (plume) and `6041FD2D3D0164E1` (flare) already have transcriptions; no
transcription exists for `9AEC596A2B036EA6` (there is no `sprite_vs.h`), so it is included, and left alone
(not rebound) while `fix.witchspace_stars = off` withholds it. EXCLUSIONS ON THE LIGHTING TEST: none; all 16
passed, no dynamic indexing of cb1 in any of them. The explosion suspects (`9F4BBCFCD3B68BC9`, then
`78F5F08D02EE38CC`) are in. Risks to read in the flight: `9AEC596A2B036EA6` is the hyperspace tunnel's
starfield (roll-free streaks there are a judgement call); `2BF557356A28AA0D` declares `cb1[294]`.

### The design (`billboard_rollfree.h`, `particle_fix.cpp`)

- **Basis.** From the block's own forward `[279]` and WORLD UP (0,1,0), steady's convention:
  `right = normalize(cross(up, fwd))`, `up = cross(fwd, right)`. The new [277]/[278] keep the game's row
  LENGTHS, w components and HANDEDNESS (the sign of `[277] . cross([278],[279])`), so quad size and mirroring
  do not change, only the roll. Forward within about a degree of world up has no defined roll: no rebind.
- **GPU-side, no new CPU read.** The 160 bytes the particle fix already keeps give `[277..279]`. Per watched block
  (a table of four, each with its shadow, write counter and copy): `CopyResource` of the game's block into an
  EDVR DEFAULT constant buffer, `UpdateSubresource` of a 32-byte patch buffer, `CopySubresourceRegion` of the
  patch into offset 277*16. This route is legal for constant-buffer-bound resources: clean under the WARP debug
  layer (rig R2/R3/R5/R7), where a partial `UpdateSubresource` of a constant buffer is not before D3D11.1.
- **Lazy.** The copy is made at the first billboard draw after a write of that block, once however many draws
  follow (rig R5: 25 draws, 1 copy). Ten writes and no draw copy nothing.
- **Per draw.** `particleOnDraw` matches by hash like the plume and flare, arms a pending flag and RETURNS FALSE:
  the draw keeps its place in the ladder (it is still counted as an eye draw, still routed). `forwardWithVerdict`
  binds the copy at VERTEX slot 1 for the draw and every re-issue, and restores on every exit. The pixel stage is
  never set (rig R3: the pixel shader still reads the game's rows). EDVR's own copies and binds run inside
  `FlatComputeInternalScope`, so the game's tees do not see them.
- **Retirement.** The same 300-frame idle logic: a session with no billboard of the family drawn lets every
  watched block go; the next matched draw learns again. Stock (`fix.particle_billboard = stock`, hot reload)
  clears the table and disables it. No new key.
- **Staleness.** The basis comes from the shadow of the last Map/Unmap write, as the plume's always has. A block
  the game updates with `UpdateSubresource` instead would leave the shadow stale (small error: the forward only).

### Instruments

- Every 10 s: `billboard roll-free (N frame(s)), rebound/seen by vertex shader: HASH b/s; ... block copies X/frame
  for B rebound draw(s), ...; cpu U us/draw (n timed); gpu copy M ms (n timed)`. Reads "not exercised" when none of
  the 16 drew. The flight shows which hashes draw during an explosion.
- Once per hash, at its first rebind: `roll-free scene block first bound for vs HASH`.
- The CPU part of the capture is the existing `particle` reader of the unmap readers line (still 160 bytes).
- GPU time of the copy is sampled one copy in 16 with `GpuIntervals` (the census's own primitive) and printed on the
  roll-free line. A `GpuCensusSection` of its own was not added: the section table is pinned in the census rig in
  several places (a follow-up if wanted).

### Flight plan

Frontier VR, `fix.particle_billboard = steady`: fly an explosion (a ship kill, a mining blast) with a rolled head.
Read the `billboard roll-free` lines for the hashes that drew; the quads should stay upright, not mirrored, not
mis-sized. A/B `stock` by editing the ini (hot). Pass: no roll on explosions and the other billboards, no change
to lighting. Fail signals: mirrored or flipped sprites (handedness), pop at the pole, a quad size change.

### Flight (Frontier, SteamVR aapvr, RTX 5090, `edvr_gfx_20261016_054341.log`)

- Explosions no longer roll. Steady costs about 0.02 ms CPU and 0.05-0.08 ms GPU a frame; no FAILED copies.
- Sean still sees explosions FLEX when he pitches his head. That is the expected limit of the roll-free basis:
  it keeps the camera's FORWARD, so the quad plane follows the head's pitch. Facing the viewer is per particle,
  and only a vertex shader reads the particle (E3 above): hence transcriptions.
- Which hashes draw: `9F4BBCFCD3B68BC9`, `BBAD1CA808E1E292` and `9AEC596A2B036EA6` rose and decayed together and
  later drew identical counts per window, so they are one effect (the explosion); `AFBCD3ADB9092F78`,
  `4BDE4DBF88C2BA27` and `78F5F08D02EE38CC` drew around it.
- The cb1 rows burst (instrument removed): `[277..279]` identical per eye; 63-122 writes of the scene block a frame.
- The mapped read probe: write-combined mappings on NVIDIA too (13.6 us per full 5376-byte read).

### Follow-up: the explosion left the roll-free list

The flex is the roll-free basis keeping the camera forward, so `9F4BBCFCD3B68BC9`, `BBAD1CA808E1E292` and `9AEC596A2B036EA6` now have
their own facing transcriptions (`src/d3d11/billboard_facing_vs.h`) and are no longer rebound; `AFBCD3ADB9092F78`, `4BDE4DBF88C2BA27` and
`78F5F08D02EE38CC` (which drew around the explosion) and the other ten remain roll-free. The table above lists the sixteen as built for the
flight; the live split is now 5 transcribed, 11 rebound (see Follow-up 2).

### Follow-up 2: the fireball puffs

The first facing flight still showed a flex. The eye dump found the puffs are drawn by `AFBCD3ADB9092F78` (the no-near-fade twin of `9F4B`)
and `4BDE4DBF88C2BA27` (the no-fade twin of `BBAD`); both had only the roll-free rebind, which keeps the camera forward. They now have their
own facing transcriptions too (five in all; 11 shaders remain roll-free). Evidence and rig: `particle-billboards.md`, "the fireball puffs are
the no-fade twins".
