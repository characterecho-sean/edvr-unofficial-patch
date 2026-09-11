# On-foot weapon judder, 2026-09-11

Scope: weapon judder while walking/running. HUD resolution work is
paused. Preserve runtime reprojection; do not substitute Turbo mode.

## Evidence from 17:09:53

Steam flight `edvr_gfx_20260911_170632.log`, VR log
`edvr_vr_20260911_170651.log`, eye run `170953`. The sanctioned log tool
verified dcdcf41. HEAD aed65b4 differs only in unrelated menu/install
work; screen_motion, temporal_pass and vscreen are unchanged from
dcdcf41.

This run used Valve SteamVR, 90 Hz, DLSS performance preset K, 2268x2240
input / 4536x4480 output per eye. Source colour/depth are 5120x2880. The
source motion map was bound (input flag 32).

The pistol jumps in C01, C07 and C11 and returns in C02, C08 and C12.
These are the raw frames BEFORE temporal reconstruction, also before
runtime composition. The corresponding T frames preserve the same
geometric jumps. The user confirms it also happens with AA Off.

The source camera origin, VS b1[275], advances twice then repeats on
those exact pairs:

| Raw frame | Snapshot frame | Origin XYZ (metres) |
|---|---|---|
| C00 | 17597 | 6.5319815, 39.810413, 36.251490 |
| C01 | 17598 | 6.5433702, 39.885242, 36.312115 |
| C02 | 17599 | same as C01 |
| C06 | 17603 | 6.5680957, 40.050674, 36.443867 |
| C07 | 17604 | 6.5789695, 40.123234, 36.501790 |
| C08 | 17605 | same as C07 |
| C10 | 17607 | 6.5893980, 40.193733, 36.557415 |
| C11 | 17608 | 6.6001887, 40.265167, 36.614860 |
| C12 | 17609 | same as C11 |

Regular camera steps are about 4.5-5 cm; the doubled steps are about
9-10 cm. This supports a disagreement between the camera update and the
weapon transform/animation update, but the current dump does not contain
weapon instance records or bones to identify the exact stage.

The later VR pacing burst reports 1757 frames, 6 over 13 ms, no split
eye submits. The dump itself introduces a 24.2 ms frame with 374 MB of
staging allocations. Do not use that first-frame stall as proof of a
normal-play timing defect. Earlier long frames/flash withholds belong to
the ship portion of this flight, before the on-foot screen engages at
17:09:34.

Ruled out: DLSS as the source of these geometric jumps, because they are
already present in C and the user reproduces them with AA Off. Ruled
out: runtime reprojection as the sole source, because these raw frames
have not reached the compositor. Runtime pacing can still influence
which simulation state the game renders.

There is also a separate temporal-input defect: weapon pixels receive
world-camera motion (examples -47,-94 and -77,-99 input pixels), while
the gun is carried by the player. Correcting that alone cannot solve the
AA-Off judder. Leave that consumer unchanged until actual weapon
transforms are available.

## Discriminating capture

Drawstate v4 extends the existing explicit eye dump. Only the existing
source-sized offscreen draw hook calls the new mesh capture; normal
rendering and compositor timing are unchanged.

It records the source mesh families observed in this census, including
scenery (a shader hash alone does not identify a first-person weapon).
The 7B0DC42D383F694C and EB5234DB6ADB491D original shaders both read
instance t33, skin t38 and camera b1[275]/b1[270..273]. The capture
saves:

- The original per-draw camera/material constants, counts and offsets.
- Full t33, t38 and instance VB0 buffers, once per resource per frame.
  Each reference names the first draw that made that copy. This is
  explicit provenance, not a claim of immutable contents within a frame.
- Bounded first-frame packed geometry/index windows for offline
  identification of the weapon and arm draws.

Mesh buffers have a 16 MiB individual / 256 MiB run budget. Original
palette @100 is 8,388,624 bytes, so the old ledger's 1 MiB bone prefix
would not establish that all relevant bone indices were captured. Draws,
geometry and texture budgets remain bounded separately. Only GPU copies
happen during capture; nonblocking readback follows the existing ledger
grace period. Declines, missing copies and zero mesh draws are reported
explicitly.

The next run should distinguish:

1. Instance translation advances smoothly while the draw camera
   doubles/repeats: mismatched camera and attached-object updates.
2. Instance translation matches the camera, but bones jump: animation
   pose timing or interpolation.
3. Weapon draw camera differs from the terrain camera in the same frame:
   separate viewmodel projection/origin handling.

Required capture: briefly run straight with the weapon visible and take
an eye dump. AA may remain Off, and reprojection should stay unchanged.
Do not change rendering on the timing hypothesis until these records
confirm which transform disagrees.

## Validation

Full NVIDIA SDK build and all build gates passed, including the GPU
capture fixture and 243-key config contract. The fixture verifies
same-frame deduplication, separate copies after next-frame buffer reuse,
the full palette past 1 MiB, unchanged bindings and explicit budget
declines. The reader rejects bad frame/role/reference metadata and
continues to read the original v3 flight. NVIDIA TAA/DLAA/DLSS,
foveation and motion-convention smoke checks passed. This is a tested
diagnostic build, not a verified correction to weapon judder.
