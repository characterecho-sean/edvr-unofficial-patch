# On-foot weapon judder, 2026-09-11

Scope: weapon judder while walking/running. HUD resolution work is
paused. Preserve runtime reprojection; do not substitute Turbo mode.

## Status

State, 2026-09-17: `fix.weapon_stability` (default on) pins the arms
root to the source camera in hip fire and corrects the ADS timing
disagreement; every source pass that reads the instance pool the
standard way (t33 stride 336 minus b1[275]) and whose vertex shader is
in `family()` in `src/d3d11/weapon_stability.cpp` draws from the
corrected pool. The 2026-09-17 Steam dumps found two passes still
outside that list: `BB31244E30265F2D` (the Takada laser rifle's two
cyan glow strips) and `CFCA8FFC6B058630` (a single skinned triangle at
the arms root). Both are added to `family()`, to the eye dump's
`sourceMesh()` capture list and to the rig's shared-pool loop
(cd1577a). FLOWN 2026-09-17 08:08 on Steam and CONFIRMED ("that fixed
the rifle cyan lines"). Entry: "Laser rifle glow strips" below.

Open, 2026-09-17: the point-light glow of every gun (the laser rifle's
rear-ring core, Sean: "any gun with lights") shifts while moving. The
light path (`lightDraw` / `applyLights`) runs and passes its gates, but
it read `Anchor[0]`, the LAST `findAnchor` result of the frame, and
Elite reruns that kernel under the WORLD camera rows (the body pass)
after the arms, so in hip fire the lights got no correction. Fix: the
frame's applied mesh correction is kept in its own stamped rows
(`kWeaponAppliedRow`) and the lights read those; the light dispatch
records the near plane it saw for the status line. BUILT, NOT FLOWN.
Entry: "Point lights read a rerun anchor" below.

Kept outside on purpose: the late UI labels `B10B032BDFD46700`,
`C4B4B334B26E81A9` and the panel shader `A888D51024D9798E` (ammo and
magazine readouts). Their placement is already camera-relative; the
2026-09-17 measurement put them within 3.5-9.4 source pixels of the
corrected body across 16 strafing frames.

Ruled out: see the "Ruled out" lines of each dated entry; the
2026-09-17 entry closes the ammo panels, the HUD polyline shader
`B7790CBFC6554097` and the 2026-09-11 "full-screen triangle" label on
`CFCA8FFC6B058630`.

Next flight (Steam, any gun with a light; the laser rifle's rear ring
is the known case): stand still, then strafe both ways and walk
forward; the light glow must stay put on the weapon exactly as the
mesh does. Verify the build first with
`python tools\edvr_log.py --target steam --expect-build HEAD`. Then
read the new status line `weapon stability: lights: ...` (every 1800
frames): `arms correction fresh` with `5+ lights` proves the light
dispatch found this frame's applied correction; `mesh camera near
0.0250000 then` proves the camera buffer held the world rows at the
light draw (the rerun that clobbered `Anchor[0]`), `0.0675000` means
the clobbering rerun happened earlier in the frame instead. `STALE`
with the glow still shifting means the arms were not applied in that
frame at all and the hunt moves to `findAnchor`. If the glow still
shifts with `fresh`, take an eye dump: the point-light draw is captured
in the drawstate (ordinal UINT32_MAX-2, VS `0357BBB2DEE43C1F`, its
light stream in `streams[1]`), and the per-frame method is in the
"Point lights read a rerun anchor" entry.

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

## Strafing capture at 17:36:28

Steam gfx log `edvr_gfx_20260911_173403.log` matches diagnostic build
600e7bf. Valve SteamVR, 90 Hz, DLSS preset K, 2268x2240 input /
4536x4480 output; source 5120x2880. The user's independent AA-Off test
also reproduced the judder. Drawstate v4 contains 19 source frames, 1387
source mesh draws, 57 buffer copies, zero failures or declines. The
first 16 raw eye frames correspond to mesh frames 8068..8083.

Confirmed: camera and first-person attachment origins disagree on the
raw frames where the weapon jumps. The seven rigid gun pieces use t33
records 52, 77, 83, 42, 23, 65 and 56, with bone base zero. Their world
positions advance smoothly. Both skinned arms share a separate origin in
records 262/263, which also advances smoothly. The source camera origin
b1[275] doubles its movement and then repeats, while the arms/weapon
advance each frame. In frame 8072 the arms origin is 41.29 mm ahead of
the camera along X; frame 8073 repeats the camera, and the discrepancy
returns to about 0.14 mm. The same pattern appears in frame 8068. Rigid
gun orientation also changes smoothly.

Ruled out: snapping skeletal animation as the source of the large jumps,
because the rigid gun pieces have no bone transform and jump with the
same attachment-origin discrepancy. Ruled out: a different weapon
camera, because its source-camera rows match the terrain draws in the
same frame. Ruled out: using b1[282..284].w as the correction, because
it contains current-minus-previous camera movement, not the attachment
discrepancy.

The arm roots have identity rotation and approximately (-0.000125,
-1.712854, -0.089025) bind translation. The full player body has
different roots and an origin about 1.55 m from the camera. The arm
record indices change to 260/261 in frame 8078; fixed record or bone
indices would therefore fail inside this capture alone.

## Correct the source attachment translation

`weapon_stability` applies `cameraOrigin - armsOrigin` to the
near-camera weapon/arm instance translations in a private GPU pool. The
original vertex shaders, bones, orientation, scale, materials, draws and
world camera remain intact. This preserves the game's weapon bob and
other animation relative to the attachment. It changes neither runtime
frame submission nor reprojection and does not depend on TAA or DLSS.

Activation requires the actual on-foot screen composite and a
source-sized offscreen draw. Screen recognition precedes curved-screen
substitution, which can consume the composite without forwarding the
original draw. The correction only binds on the four observed weapon/arm
shader families: F516BF0201303B87, 8B589D25B2A0ADDC, 7B0DC42D383F694C
and 114AF608F86D9ED8. Other scenery shaders never receive the private
pool.

The GPU locates two co-located, identically oriented arm records with
distinct valid bone bases and the observed eye-height bind-root
encoding. It requires the camera-centred source projection and supported
full structured-buffer views. Near-camera rigid attachments and skinned
records at the shared origin get the translation; other records are
copied unchanged. There are no fixed record indices or temporal
smoothing rules. Missing or unsupported roots produce stock geometry.
Other weapons and poses still require field coverage; this classifier is
based on the captured pistol/arms and must fail closed when that
encoding is absent.

Two small compute dispatches generate a reusable private 336-byte-stride
instance pool. The captured pool has 2048 records (688128 bytes); the
runtime allocation is bounded at 8 MiB. Camera, pool and palette writes
invalidate reuse even within one frame. Command-list execution also
invalidates the cache. All touched CS bindings and VS t33 are restored.
Normal rendering reads no geometry back to the CPU. A 48-byte
asynchronous status sample reports the matched root and correction, or a
stock fallback; screen detection is logged separately so an inactive
source hook is visible.

Independent reconstruction of the first 16 captured frames reduces the
maximum second difference of gun translation relative to the camera from
83.975 mm to 3.706 mm. The residual is the original attachment-relative
animation, not an added filter. Production GPU replay matches the
expected corrected pool byte for byte across all 19 captured frames,
including the record repacking. Only the seven visible gun records and
two arm records change among the supported draws. All five full-body
records remain stock.

The WARP regression checks the actual production draw path, attachment
selection, record repacking, same-frame buffer writes, unchanged
original data, CS/VS restoration, invalid roots/projections, source
viewport gating, screen expiry and resource release. D3D debug
validation is clean. Headset validation of the correction is still
required; the offline replay does not demonstrate final perceived
smoothness under every runtime.

Full SDK build and all repository gates passed with the correction,
including the 243-key config contract and existing source-motion tests.
NVIDIA TAA/DLAA/DLSS, foveation and motion-convention smoke checks
passed. On the local RTX 5090, an offline timestamp benchmark of the
captured 2048-record pool measured 0.0211 ms for both compute passes and
scoped bindings (three batches of 512 updates). This is an isolated
steady-state GPU cost, not an in-game total frametime measurement.

## Detached material pieces in the 18:21 captures

The user confirms a18c55d is much smoother but some weapon/tool pieces
separate while moving. Gfx log `edvr_gfx_20260911_181154.log` matches
a18c55d. Runs 182106, 182117, 182123, 182127 and 182133 cover the
pistol, rifle and three tools. All five contain 19 source mesh frames
with zero capture failures or declines. The attachment correction is
active; the captured main meshes all satisfy its instance selection.

Confirmed: the material-family allowlist missed five additional source
passes. Tool components use AACFDCF2FB9AD809; the rifle also uses
34CCFAAB1EAD90BE, 174E8D76363BE337 and 7F9B650EC1A1E570 for its optical
pieces. 025B4B9FF54622ED draws additional weapon/tool surfaces. Their
census instance IDs resolve to the same near-camera attachment records
as the corrected meshes, using the captured full instance-ID stream and
pool. For example, the 182133 tool's opaque record 57 also feeds the
uncorrected AACF 84- and 36-index draws. Rifle optical records
26/33/81/92/98 are 0.259 m from the arm origin, and record 84 is 0.438 m
away. All are rigid attachments selected in the private pool; the draw
allowlist alone prevented those passes from using it.

Recovered the missing original shaders from the installed EffectsBinary
archives, verifying EDVR's bytecode hash. All five vertex shaders read
t33 at stride 336, use its primary translation at byte 16 minus b1[275],
and project through b1[270..273]. They support the existing correction
directly; no change to its magnitude or attachment detection is needed.

Ruled out: adding a second offset to the late B10B032BDFD46700 UI
labels. Their records already contain camera-relative placement. After
transforming them back through the captured source camera, their local
offset from the corrected tool stays constant within 0.014 mm across the
first 16 frames (0.252 mm on the animated rifle). Offsetting them again
would introduce a new separation. Keep the late UI and unrelated
full-screen triangle passes outside the material extension.

Extended the correction and explicit eye-dump source-mesh capture to all
five confirmed material families. An offline WARP replay executes each
original vertex shader against the captured camera, bones and instance
pool, streaming its position output directly. Its clip-space
displacement matches the primary attachment correction within 7.5e-8
across all five shaders. No D3D debug warnings or errors. This verifies
the material transforms; headset confirmation that all visible pieces
stay attached is still required.

## Live A/B control

Added `fix.weapon_stability = 1`, exposed as **Weapon stability** under
**Fixes > The on-foot screen**. It defaults on, including existing INIs
without the new key. Off immediately bypasses the complete source
attachment correction; on applies it again using fresh camera/pool data.
The setting survives leaving and reentering the on-foot screen. Compiled
shaders and buffers can be reused across a live toggle, but prepared
corrections and pending status samples cannot. An existing GPU
initialization failure remains latched until normal resource teardown.
The setting changes neither AA nor runtime reprojection.

The production-path regression covers live off/on, fresh input after
reenabling, allocation reuse, setting persistence after inactivity, the
five added material passes and exclusion of camera-relative GUI. It
passes 149 checks. The generated menu row is a live Fix toggle, default
1, in the intended page/group. Full SDK build and the 244-key config
contract pass. NVIDIA TAA/DLAA/DLSS, foveation and motion-convention
smoke checks pass.

## Walking root rotation in the 18:52 capture

The user confirms the detached pieces are much better with 7c65d10. The
remaining walking judder has a reproduced detector failure: a small
animation rotation makes the identity-root test reject 17 of 19 frames.
The [planet performance review](review-planet-performance-2026-09-11.md)
records the rigid-root invariant, paired-bind validation and exact GPU
replay of the corrected walking frames. It also investigates the
separate terrain AA cost after entering the cockpit.

## Purple effect during crouching, capture 200256

The user confirms that weapon/tool geometry now looks good with 483471c,
but a purple gun effect briefly rises out of the weapon while crouching.
The build-matched 20:02:56 source colour capture visibly contains that
displaced glow. Its 19 source frames contain 1,330 mesh draws and 57
full frame-local buffers, with zero failed copies or range/format/budget
declines.

Ruled out: the walking root detector dropping out in this dump.
Independent replay accepts all 19 frames, with proper rigid paired roots
and a maximum correction of 61.46 mm. Every near-camera packed mesh
material draw in the source census belongs to the already supported set.
Adding another mesh hash would not address the separate billboard
placement.

The source census also includes 9AEC596A2B036EA6 (92-byte particle
vertices, 1,488 indices) and 3D05E7CF11AC9BEE (209 instanced flares).
Recovered both original vertex shaders from the installed EffectsBinary
archives and verified their EDVR hashes. They use vertex/instance data
and model/camera constants, not the weapon's t33 primary translation.
The 68DDDEF04D9894AF/F7A6E916F14A3B1A pair instead constructs angular
sky points; it is not evidence of a weapon attachment.

The existing capture does not contain the particle/flare vertex streams.
The image and draw list identify candidates, but cannot establish
whether the glow is an attached emitter with mismatched camera timing, a
camera-relative flare, or intentionally trailing particles. No
additional offset or broad particle substitution is justified from this
dump.

Extended explicit source eye dumps to retain both candidate families'
b0/b1/b2, VB0/VB1/IB binding windows and original input layouts.
Drawstate version 5 uses ordinal UINT32_MAX-2 for these effects; its
mesh/source-camera ordinals are unchanged. Effect geometry is retained
throughout the existing eye-run window under the shared 32 MiB vertex
budget. Layouts are attached to the relevant input-layout objects at
creation, with no disk writes until a requested dump. Logs count effect
draws, vertex payloads and recovered layouts, including zero counts when
a source screen is present. The parser keeps versions 1--4 readable and
validates the added layout bounds.

A crouching eye dump with this instrumentation should allow projection
of the particle centres against the recorded weapon correction across
frames. The effect itself remains unchanged, as does the working
weapon/tool correction and runtime reprojection. This issue remains open
pending that evidence.

GPU replay of all 19 captured crouching pools is byte-identical to the
independently reconstructed correction. The source-effect capture
regression verifies late-run geometry, original input-layout metadata,
bounded copies, unused index-buffer exclusion, source-only gating, and
C++/Python format agreement.

## Laser rifle glow strips, 2026-09-17 Steam dumps

The user reports that on a Takada laser rifle two faint cyan light
strips on top of the receiver lag sideways off the weapon while
strafing, and also misbehave walking forward. Three Steam eye dumps:
064839, 064854 and 064906, gfx log `edvr_gfx_20260917_063945.log`,
version `v0.17.0-rc.3-42-g2ebed2d-dirty`. HEAD `eefcb09` changes no
weapon, screen-motion or vscreen file after `2ebed2d`, so the dumps are
evidence for this build's weapon code. Weapon stability was on and
matched throughout (hip fire, near 0.0675, 3 matching roots).

The draw census of frame 49148 (drawstate frame 48601) has the rifle's
opaque body under the known families. Three late draws in the lit pass
(RTV `@216`, the body's DSV `@138`) use VS `BB31244E30265F2D` / PS
`7A4B460994E410B4` with 1068, 576 and 984 indices; their VB0 instance
entries (startInstance 507, 505, 514) select pool records 23, 10 and
67, the same rigid records the corrected body draws read, 0.33-0.44 m
from the camera. The VS is the standard template: t33 at stride 336,
position minus b1[275], bones at t38. Because the shader was not in
`family()`, the draws read the game's pool, and the strips rendered at
the game's lagging arms transform.

The arms lag: the timing trace for frames 49128-49148 holds the arms
root 0.048-0.052 m along the camera's right vector while the camera
moves 0.030 m per frame to the left, so the arms trail the camera by
about 1.6 frames. The correction for frame 48601 is T = cb1[275] minus
record 249 = (0.0427, -0.0202, -0.0007) m, 4.73 cm. Projecting records
10 and 67 through the captured camera puts their uncorrected origins in
the gap beside the receiver, where the floating strips are, and their
corrected origins (+T) on the scope housing rim, 392 px left and 73 px
up on the 5120x2880 source. The three runs agree with that geometry:
standing still (064854) the glow sits inside its groove on the
receiver, 0 px off; strafing left (064839) the strips float 300-400 px
to the right; walking forward (064906) the groove is mostly dark, the
strips pushed along the view axis into the receiver and hidden by its
depth.

A second miss: VS `CFCA8FFC6B058630` / PS `8A08FF781272C5F6`, one
skinned triangle drawn between body draws in the G-buffer pass, reads
record 252, whose position is bit-identical to the arms root 249. The
2026-09-11 rig listed it with the full-screen triangle passes because
it has three indices; it binds t33 and t38 and sits at the root, so it
belongs with `88DCF1164C640EC3`.

Ruled out: the ammo and magazine panels (`A888D51024D9798E`, PS
`015EF9349EC097E8`), because the user confirmed they are not the
symptom and masked cross-correlation over the 16 raw frames of each
run keeps them within 3.5-9.4 source pixels of the body, at most
1.4 mm at their depth. Ruled out: `B7790CBFC6554097` as the strips,
because two of its three draws have 67 and 131 indices, not triangle
lists, and it binds no pool: HUD polylines. Ruled out: a temporal or
compositor origin, because the displacement is already in the raw
source frames before reconstruction and the treated frames show the
same offset without smear.

Fix: `BB31244E30265F2D` and `CFCA8FFC6B058630` added to `family()`
and to the eye dump's `sourceMesh()` capture list; the rig's
shared-corrected-pool loop covers both and no longer asserts that
`CFCA8FFC6B058630` stays original. The correction magnitude, the
attachment detection and the ADS path are unchanged. Flown 2026-09-17
08:08 on Steam (the DLL is named `eefcb09-dirty`, linked before the
commit existed; same code) and confirmed by Sean.

## Point lights read a rerun anchor, 2026-09-17 Steam dumps

After the strips fix Sean reports the blue core glow in the laser
rifle's rear ring shifting while moving, then: "this seems to happen on
any gun with lights". Three Steam eye dumps 081113, 081119 and 081148,
gfx log `edvr_gfx_20260917_080829.log`, drawstate frames 16243-16261,
16754-16772 and 19288-19306. In 081113 the glow spills from the bore in
the first raw frame only and the bore is dark in the rest; the ring's
static trim is pixel-identical across the frames.

The light path itself is sound on these draws. The point-light batch
(VS `0357BBB2DEE43C1F` / PS `81812EF97FB4A361`, 14 indices, 39 lights,
RTV `@200`, `ia=0,0,0`) binds VB slot 1 at stride 32 offset 0 and its
b2 eye equals the mesh cb1[275] to 1.2e-7 m, so every `lightDraw` gate
passes, and the "weapon lights" kernel compiled at 08:09:52, which only
happens after those gates. Two lights sit on the weapon: 37 (0.167 m
from the root, radius 0.9 cm) and 38 (0.120 m, radius 2.6 cm); both
pass every `applyLights` gate in all 19 frames.

The glow tracks the UNCORRECTED arms: the anchor offset T is 0.4 cm in
frame 16243 (glow visible) and 2.0 cm in every later frame (bore dark)
with the camera still to 0.1 mm per frame, and the light-to-root vector
is constant, so a corrected light would have been rigid to the ring. A
2 cm miss carries a 2.6 cm light out of the bore.

Why the correction is missing: `applyLights` read `Anchor[0]`, the
result of the LAST `findAnchor` run, and the timing trace shows 11 runs
a frame (`calls=11`) because Elite rewrites the one mesh camera buffer
before most material batches. Resolving every family draw of frame
16243 through the drawstate's VB0 and t33 captures, in draw order: the
arms and rifle (records 2-61 and 260-263, 0.3-0.5 m, cb1 near 0.0675),
then 26 world props under `4435F2E50020E7F3` at 44-438 m with cb1 near
0.0250, then the player's own body under the family VS
`7B0DC42D383F694C` (records 248-258, 1.68 m, near 0.0250), then one
viewmodel draw `114AF608F86D9ED8`, the post passes, the light batch,
and the late lit passes at near 0.0675. Every family draw under the
world rows reruns `findAnchor` on the aiming projection, which in hip
fire applies nothing, so `Anchor[0]` is whatever the rerun nearest the
light batch produced. The rig encoded this as intended (near 0.025 in
that buffer read as "ADS must not detach a light"), but mid-frame it
just means the buffer holds the world rows.

Ruled out: the ammo/magazine panels and the strips (fixed above).
Ruled out: the `lightDraw` C++ gates, because the stride, offset,
window, PS hash and start arguments pass on the captured draws and the
kernel compiled. Ruled out: a command-list invalidation between the
arms and the lights, because the census shows no execute event between
draws #418 and #603. Ruled out: the sprite passes `F8FA801F2CB1E27C`
and `7E38A6AA1269C901` as the glow, because both pass POSITION through
without a camera (full-screen passes). Not settled: whether the rerun
that clobbers `Anchor[0]` is the body pass or a later rewrite before
the light batch; the spot-light batch `963B52C73B4143AC` (84 indices,
29 lights from a VS structured buffer t0 at stride 112, not captured by
the drawstate) is not handled by any path and may carry other guns'
glows. The new status line answers the first; a still-shifting glow
with `fresh` points at the second.

Fix: `findAnchor` writes the frame's applied mesh correction (shift,
root, eye, frame stamp; mesh dispatches only) to `kWeaponAppliedRow`,
and `applyLights` uses those rows, stamped to the current frame, in
place of `Anchor[0]`/`Anchor[1]`/b0[275]; the light dispatch binds the
sample stamp at b3 with its batch size and records (near plane seen,
fresh flag, lights, frame) for the status line. The rig's ADS light
case is split into the world-rewrite case (lights still corrected) and
a true ADS frame (arms applied nothing, lights stock). Not flown; the
Status block carries the flight brief.
