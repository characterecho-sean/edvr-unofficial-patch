# On-foot weapon judder, 2026-09-11

Scope: weapon judder while walking/running and on mouse turns. HUD
resolution work is paused.

## Status

SOLVED and PROMOTED, 2026-09-17: `fix.weapon_stability` (default on,
live, "Weapon stability" in the menu) now means deferred frame pacing
while on foot -- OpenXR Toolkit's "Turbo mode" in EDVR's own runtime,
engaged from the journal's Status.json on-foot flag: `WaitGetPoses`
returns at once with a synthesized display time, `xrWaitFrame` runs on
a pacer thread and the game blocks at the second `Submit` instead.
Built as `experimental.turbo_mode = on_foot` (117348d), FLOWN
2026-09-17 14:15 on Steam (Pimax OpenXR, Crystal Super, DLSS, 90 Hz)
with the D3D11 correction switched OFF in the menu at 14:16:30, and
CONFIRMED: Sean "That actually fixes the weapon stability issue nicely
without any additional judders." So the pacing alone was the test, and
it passed. Promoted the same day: the key drives the pacing, the
`experimental.turbo_mode` key is gone, and the D3D11 arms correction
(`src/d3d11/weapon_stability.{h,cpp}`, its rig, `family()`, the
particle and light paths, the ADS acquisition) is DELETED; the
mechanism the dated entries below describe is history. The weapon's
temporal-AA motion vectors (`weapon_motion.{h,cpp}`) stay, hosted
directly by `vscreen.cpp`'s DrawIndexedInstanced hook after the
original draw (`weaponMotionWants`), still gated by the same key
through `screen_motion.cpp`. Entry: "Turbo mode promoted" at the end.

What the flight measured (entry at the end): under turbo the runtime's
`wait_frame` phase went to 0 and `pacer_block` carried the same 5.9-7.3
ms p50 the wait used to take; 8943 deferred frames, 37 whose wait had
already returned, nothing drained. Sean A/B'd it in the menu at
14:18:42 (off, `pacing=runtime`) and 14:18:45 (on_foot again).

Which reading won is NOT settled: the flight confirms the symptom is
gone, not the mechanism ("race" vs "cadence" in the turbo entry's
hypothesis). The eye dump of a mouse turn under turbo that would decide
it was not taken; the VS rotation-source hunt is closed as moot rather
than answered.

What to watch: about one frame of extra latency on foot (the rotation
part hidden by the headset's reprojection), and SteamVR motion
smoothing / Oculus ASW cannot engage while the wait is deferred. Flown
on ONE runtime and headset; other runtimes are unflown. With the
journal watcher off the fix never engages and the log says so.

Log signatures: d3d11 log `weapon stability: on (live; the frame wait
moves to the eye submit while on foot)` at configure and on each
change; `native frame: begin #N ... pacing=turbo` on each engagement,
`pacing=runtime` standing down; runtime trace
`native_pacing,mode=deferred|runtime` at the frame it took effect,
`pacing=1` with `wait_frame=0` and a non-zero `pacer_block` in
`native_submit_phases`, `native_pacing_summary` at close.

Not flown: the promotion build itself (the correction removed, the key
rewired). The 14:15 flight had the correction off and turbo on, which
is the same state; the next on-foot session is the regression check.

Ruled out: see the "Ruled out" lines of each dated entry; the
2026-09-17 entries close the ammo panels, the HUD polyline shader
`B7790CBFC6554097` and the 2026-09-11 "full-screen triangle" label on
`CFCA8FFC6B058630`.

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

## ADS "ghosting of the sights", 2026-09-17 Steam dump 102403

Sean, after the panel fix flew (10:22 flight, DLL built from the
8fb4b6a source before its commit, so the version line reads
`00c27ed-dirty`): "moving side to side while ADS, there's still some
ghosting of the sights". Pistol with the holographic sight, strafing on
the ground beside the ship. Dump: `eye_102403_*` (paired run, 16 raw
C and 16 treated T crops of the left eye, frames 10962-10977, plus the
frame-10962 inputs) and `drawstate_102403.bin` (19 frames 10963-10981),
and the 128-frame `weapon timing` trace printed at the dump (10834-10961,
BEFORE the dump's own hitch).

**What the trace says about the correction (the 1.4 s in which Sean saw
it):** stationary until 10866, a strafe from 10867 through a reversal at
10930 to 10961. From 10870 on, status 4 every frame with `correction ==
dc` (this frame's camera step) and `dp_k == dc_{k-1}` exactly: in
steady 92-fps play the game's ADS arms use the PREVIOUS frame's camera
position, and the correction restores camera lock to 0.1 mm. Status 1
(nothing applied) on three frames at the strafe start (camera steps
0.15/0.48/4.8 mm, the arms 0.15 and 0.48 mm behind and then 4.8 mm)
and one at the reversal (10930: 1.5 mm behind). The camera basis did
not change in the whole window (`steady` on every frame). The dump's
own stall is handled: 10959 (camera step 0, arms +17.1 mm, synchronized)
and the 10960 catch-up (status 3 pins the residual 15.8 mm exactly).

- ruled out: the ADS timing correction as the ghost of a STEADY strafe,
  because the trace is exact through the strafe and its gentle reversal.
  Its acquisition is what fails on rapid changes; see below.

**What the drawstate says:** after the hitch (10965-10979, camera steps
20-30 mm) the arms are SYNCHRONIZED with the camera (`p - c` constant to
the micrometre, root `rec 301` projecting to (2915.62, 1799.9) panel px
every frame) -- the other regime the shader's comments describe; the
correction correctly went to 0 (the raw crops move ~1 px/frame, whole
weapon, body and sight together; the DLSS jitter is +-0.44 px of that).
So the game's arms alternate between "previous camera" (steady frames)
and "this camera" (after a long frame), and both are handled; each
regime flip costs 1-3 frames of ~17 mm (about 2 degrees at the sight).
Not what a steady strafe shows.

**What the treated crops say:** the housing is crisp in T04-T15 (edges
anti-aliased, no second copy); the reticle dot's glow is symmetric in
the raw C and skewed 5 px leftward (against the terrain's motion) in
T04/T06 only.

**The defect the inputs DO show (frame 10962, ScreenMotion.bin):** the
glass interior (r < 140 px about the dot) is `w=1` with motion
(2.12, -0.25) px = the terrain's (2.67, -0.49); the housing ring gets
its own (0.82, 0.22) via WeaponMotion (with 18 % of the ring `w=2`,
rejected). The reticle dot, the two lens-reflection arcs and the three
indicator dots inside the glass are camera-locked but transparent: they
write no stencil bit 16, so `kScreenMotionPs` takes the world path for
them (`attached` is false) and DLSS reprojects their history along the
ground behind the glass. That is a trail proportional to the world
motion behind the sight -- 2 px/frame here with the ground metres away,
far more against a near wall. Hip fire shows no reticle, which fits.

That glass trail is a separate, smaller defect, not this ghost: Sean's
answer (below) puts the ghost in the mesh, at movement changes. If it
is ever chased: a coverage the weapon-motion pass writes for the
transparent family draws (`025B4B9FF54622ED` reticle, `7F9B650EC1A1E570`
glass) with their own motion, read by the screen motion PS without the
stencil/depth gates; the see-through ground inside the glass then pays
with the opposite error, so the reticle quad's alpha decides which.
Probes: scratchpad `ads_lag_probe.py`, `ads_reruns_probe.py`,
`eye_track.py`, `glass_probe.py`, `dot_profile.py`, `timing_flags.py`
(the per-frame gate table of a `weapon timing` trace).

**Sean's answer:** the whole sight housing doubles (a faint second copy
offset sideways) while strafing, and in his words "this is an artifact
of the judder and the fix not acquiring on rapid movement changes". So
the target is the acquisition latency of `aimingTranslation`
(`src/d3d11/weapon_stability.h`), and the trace above shows exactly
where it waits: every start, every reversal that passes through a
sub-millimetre step, any aim turn, and every reacquisition after a frame
the exact tests could not explain.

**What the old gates cost, read off the 102403 trace with
`timing_flags.py`:**

- Strafe start: the known-lag branch required the arms' own step to
  exceed 1 mm (`lp>.001`), so with the game ramping (arms steps 0,
  0.15, 0.48 mm) the correction started on the fourth moving frame;
  the third was 4.8 mm behind (about 1 degree at the sight, one frame).
  With a harder acceleration the uncorrected step is whatever the camera
  moved by the time the arms' step passed 1 mm.
- Reversal through a near-stop: 10930 had an arms step of 0.26 mm, so
  the same gate left it 1.5 mm behind.
- Aim turn: `knownLag` required `steady` (basis unchanged to 1e-5), so
  the whole of any aim adjustment while moving ran uncorrected (17 mm,
  about 4 degrees, every frame), and afterwards the calibration had been
  reset (status 1 sets `learned=local`, confidence 1), so re-acquisition
  needed three more varying-speed frames. Not in this window (Sean held
  his aim), but the same code.
- Any unexplained arms frame (a recoil kick, a hitch pattern the pin
  does not cover) reset the calibration the same way: three frames of
  the full lag after every one.

Each of those is a one-frame (or few-frame) sideways displacement of
the whole sight of 1.5-17 mm at the moment of a movement change, which
DLSS blends with its history into a faint second copy -- Sean's
description.

**The fix (this commit), all inside `aimingTranslation`; the ADS offset
itself is never touched, the correction stays the camera step `dc` or
the status-3 residual, exactly as before (the 2026-09-13 sight-alignment
bug came from removing the offset; nothing here can):**

1. A known lag corrects on a sub-millimetre arms step when that step is
   exactly the previous camera step (`lagStep`: `lp>.0001` and
   `dp == previous dc` to 0.3 mm). A camera-only change with the arms at
   rest still waits one frame (ambiguous with an intentional aiming
   adjustment; the rig's "camera-only restart stays conservative" and
   "intentional stationary aiming offset change" cases keep their
   expectations). Trace flag 1024 marks a correction on a sub-millimetre
   step: the frames the old gate would have left behind.
2. `knownLag` no longer needs `steady`; the known offset is also tried in
   the PREVIOUS frame's basis (`lagLocalPrevious`), the arms carrying the
   previous frame's whole camera transform (confirmed offline by the
   112905 mouse-turn dump, below, before this was built). Flag 4096 =
   matched only in the previous basis.
3. A calibrated offset is retained through frames it cannot explain
   (`learned=Anchor[prev+2]`, confidence 3, instead of `local`, 1). The
   exact tests cannot pass on a stale value by accident, and the status-3
   pin's agreement test (`corr'+dc-dp == residual`) fails on one, so a
   stale offset can only delay, never misplace. A genuinely new offset
   replaces it after three frames of constancy (`fresh` count in bank
   row 7.w; flag 2048).
4. Match tolerance 0.1 -> 0.3 mm (`kAimTolerance`; the paired-root
   precision is 0.1 mm, the measured lag exactness 0.01 mm). The
   lag-mode "varying step" test stays at 0.1 mm so a synchronized
   frame (`dc == dp` exactly) can never count as a varying lag frame.

Rig (`tools/weapon_stability_test`, 2866790 checks): the 102403 ramp
and reversal in millimetres, a recoil kick during the lag followed by an
immediate correction, an aim turn through three basis rotations with
the correction continuing (flags 4096 and 256 asserted), on top of
every previous case unchanged. Trace flags now: valid=1 history=2
synchronized=4 steady=8 bounded-step=16 camera-ahead=32 calibrated=64
short-overshoot=128 lag-applied=256 known-lag=512
sub-millimetre-step=1024 fresh-offset=2048 previous-basis=4096; the
`confidence=a/b` field became `confidence=a fresh=b`.

- ruled out (by construction, kept for the record): the fix substituting
  a universal or stale sight offset, because the correction is only ever
  `dc` (status 4) or the residual to the offset the arms themselves held
  the frame before (status 3, gated by the agreement test).

**Mouse turns, dump 112905 (Sean, same build, 11:29: "ADS also judders
with mouse movement"):** standing still (camera step 0.00 mm on all 128
frames) and turning the aim with the mouse in four bursts (basis change
up to 0.017 rad per frame). On every turning frame the old code gave
status 1 (`steady` failed) and the calibration was reset after the
first one (flags 0D3 then 093). The arms moved 0.2-1.3 mm per frame
with the camera still: `turn_model.py` (scratchpad) tests three models
against the calibrated offset (0.0035, -0.0035, 0.0800) and the arms
sit EXACTLY (0.00 mm) at the previous camera position with the offset
in the PREVIOUS frame's basis on all 30 turning frames, against 0.2-1.3
mm for the current basis or a synchronized arms. So the game's ADS arms
carry the previous frame's whole camera transform, rotation included:
during a turn the sight is drawn one frame behind the aim, about 1
degree behind at 1 degree per frame, and the per-frame mouse steps vary
(0.001-0.017 rad), which is the judder Sean sees. Item 2 above is that
model, now confirmed. What the shipped correction can do about it: at a
standstill nothing (the camera step is zero, the lag is pure rotation);
while moving it restores the translation and leaves the rotation. A
translation of the root alone would only reduce the sight's error from
about 1 degree to 0.7 (the sight sits 0.17 m beyond the root), so it is
not added.

- ruled out: the instance record as the place to rotate the arms,
  because it carries no orientation: through the 19 turning frames of
  drawstate 112905 only the position words (4-6, and their copy at
  73-75) of the root (record 247) and the nearest rigid part (record
  96) change, every other word is zero or constant. The orientation
  lives in a VS SRV the drawstate does not capture or in the bone
  palette. A rotation correction (rotate every arms record and its
  skinned bones about the camera by this frame's rotation over the last)
  needs that path found first: disassemble `vs_7B0DC42D383F694C.dxbc`
  (in the pool directory since 11:29) for the per-instance rotation
  source. Separate workstream if Sean wants the mouse judder gone.

**Flight brief:** ADS with the pistol, on the ground. Strafe left-right
with quick taps and hard reversals, then strafe while adjusting the aim
slightly, then fire a few shots while strafing; take an eye dump right
after a burst of quick reversals. Read the `weapon timing` trace with
`timing_flags.py` (scratchpad) or by eye: moving frames (`|dc|` above 2
mm) must be status 4 apart from the single camera-only first step out
of rest; count the flag-1024 lines (each is a frame the old code left
behind) and, on the aim-adjust pass, flag-4096 lines with status 4 (the
previous-basis model at work). Mouse turns at a standstill will still
judder (rotation lag, above); that is the next arc, not a regression.
Verify the build first: `python tools\edvr_log.py --target steam
--expect-build HEAD`.

## Turbo mode on foot, built 2026-09-17 (experiment; FLOWN 14:15, CONFIRMED, PROMOTED -- next entry)

Sean asked for OpenXR Toolkit's "Turbo mode", engaged only on foot, as
an experimental lever against the weapon judder. The 2026-09-13 review
declined it in favour of the targeted ADS correction and reprojection;
that correction has since landed (entries above), and what is left --
mouse turns in ADS, the arms carrying the previous frame's rotation --
has no correction path until the VS rotation source is found. So the
lever is now worth one flight. `docs\performance.md`'s "Turbo mode:
No" row was about frame rate; this is not a frame-rate feature.

**What the toolkit actually does** (read from its source in
`C:\Users\seanm\projects\OpenXR-Toolkit`, main at 6b9ecb6, `layer.cpp`
2094-2213, 2215-2243, 3243-3291): its `xrWaitFrame` never calls the
runtime while a wait is in flight; it returns at once with
`predictedDisplayTime` = the last real one plus the wall-clock time
between the app's two `xrWaitFrame` calls (the real value if the async
wait has already returned), the last real period, and `shouldRender`
true. Its `xrBeginFrame` is a no-op in that state. Its `xrEndFrame`
waits for the async `xrWaitFrame` (1 s bound), then calls the real
`xrBeginFrame` and `xrEndFrame` back to back, then kicks the next async
`xrWaitFrame` (`std::async`, one task per frame). It passes the app's
own (extrapolated) displayTime through. Its menu warns that Turbo
"prevents Motion Smoothing" on SteamVR, "prevents ASW" on Oculus.

**EDVR's version** (`experimental.turbo_mode = off | on_foot | on`,
default off, live): the d3d11 half decides per frame from the key and
`journalOnFoot()` (Status.json Flags2, the same flag the head-offset
gate uses) and hands the runtime a version-3 field
(`EdvrNativeFrameOutput::deferredPacing`) across the native-frame
bridge; a version-2 openvr_api.dll or d3d11.dll on the other side of
that bridge gets or gives no field and runs as before. The runtime
(`src\openxr\frame_pacer.h`, `frame_boundary.h`, `session_state.h`):
`WaitGetPoses` with a deferred pacing kicks `xrWaitFrame` on a pacer
thread (spec: `xrWaitFrame` is externally synchronized only against
other `xrWaitFrame` calls) and hands the game a synthesized frame with
NO `xrBeginFrame`. The synthesized time is the last real prediction
plus the game's own wall-clock time since that prediction was obtained,
clamped to one..two periods -- NOT the toolkit's entry-to-entry
formula: EDVR's reference-change policy (`reference_changes.h`) retires,
fatally, on a display time that goes backwards, and the toolkit's
formula overshoots by the block time when turbo follows a long blocking
wait, so the next real value would come back lower. Every time handed
to the host is also clamped to never fall below the previous one (a
no-op under runtime pacing, where real predictions already advance);
`xrEndFrame` still gets the runtime's real time. The second eye's `Submit`
blocks until that wait returns, calls the real `xrBeginFrame`, composes
and ends the frame with the runtime's REAL predicted time as
displayTime (deliberately not the toolkit's pass-through: the layer's
view poses carry what the game rendered with, and a runtime that checks
displayTime against its own prediction sees its own number), then
kicks the next wait. A wait that has already returned by the time the
game asks is taken as-is (no synthesis, the frame is then paced exactly
as today). The mode of a frame comes from the PREVIOUS frame's
features.begin, one frame of lag at each transition. The first turbo
frame kicks and synthesizes at once. `ClearLastSubmittedFrame` and the
next `WaitGetPoses` drain a deferred frame (wait, begin, empty end), and
the STOPPING path and close() drain a dangling wait before
`xrEndSession`/`xrDestroySession` and join the pacer thread. Nothing
here touches `xrLocateViews`: the poses are located at the synthesized
time, which is what makes the arms and the world share a moment.

**Hypothesis, stated so the flight can refute it:** the arms' one-frame
rotation lag is not a fixed pipeline offset but a race between Elite's
update of the arms transform and the moment `WaitGetPoses` returns the
frame's head pose; with the render thread no longer parked in
`WaitGetPoses`, the update runs against the same pose the world is drawn
with, and the ADS mouse-turn judder goes. The alternative reading is
that turbo only regularises frame cadence and the lag stays but stops
jittering. And the null: nothing changes, because the lag is a frame of
the game's own pipelining that no pacing can move.

**What the log shows.** Verify the build first (`python
tools\edvr_log.py --target steam --expect-build HEAD`). The EDVR log's
`native frame: begin ... pacing=turbo` line at each disembark and
`pacing=runtime` at each embark (the d3d11 half's decision); if
`turbo_mode = on_foot` is set and the journal watcher is off, the log
says so once and turbo never engages. The runtime trace's
`native_pacing,mode=deferred,...` and `mode=runtime` at the same
transitions (the runtime acting on it; absent = the field never crossed
the bridge, which is a mixed-version install). Per 30 s window,
`native_submit_phases ... pacing=1 ... wait_frame=p50/p95/p99,
pacer_block=...`: under turbo `wait_frame` must sit near 0 and
`pacer_block` carry the wait the game used to sit in; under runtime
pacing the reverse. `native_pacing_summary` at close counts deferred,
synthesized, ready-at-wait (frames whose wait had already returned --
many of these mean the game is slower than the display and turbo is
buying nothing), kicks and drains. The performance overlay's "wait"
number goes to ~0 on foot and its submit time grows by the same amount;
that is the block moving, not a cost.

**Flight brief (A/B in one session).** Set `turbo_mode = on_foot` under
`[experimental]` in the live ini (it is live). Disembark. ADS and turn
with the mouse at a steady rate, then strafe in ADS and walk forward,
the same passes as the 09:47 and 102403 briefs. Then, still on foot,
set `turbo_mode = off`, wait for the `pacing=runtime` line (the next
frame after the ini reload), and repeat; then `on_foot` again. Watch
SteamVR's frame timing graph for motion smoothing engaging or not. If
the judder goes under turbo: take an eye dump of a mouse turn under
turbo and read the arms' rotation against the camera's the way the
112905 dump was read -- the lag either went (the race reading) or stayed
while the judder went (the cadence reading), and that decides where the
arc goes next. If it stays: ruled out, and the VS rotation source
remains the only path; the key can then be deleted rather than left as
a dead lever.

## Turbo mode promoted, 2026-09-17: fix.weapon_stability = deferred pacing on foot

**The flight.** Steam, 14:15:57, `v0.17.0-rc.3-87-g117348d` (verified
with `edvr_log.py --expect-build 117348d`), Pimax OpenXR on a Pimax
Crystal Super, DLSS, 90 Hz, the game holding 83-90 fps. Sean switched
the D3D11 correction OFF in the menu at 14:16:30 (`weapon stability:
off`) before disembarking, so every on-foot frame of the session ran
turbo alone. `pacing=turbo` / `native_pacing,mode=deferred,sequence=
9941` at 14:17:58 (the disembark), then a menu A/B: `on_foot -> on ->
off` at 14:18:42 (`mode=runtime`), `on_foot` again at 14:18:45
(`mode=deferred`), `mode=runtime` at 14:19:42 (the embark), summary at
close `deferred=8943, synthesized=8943, ready_at_wait=37, kicks=8982,
drained_frames=0, drained_waits=0`. Per-window phases did exactly what
the brief said they must: runtime pacing `wait_frame` p50 4.0-9.2 ms
with `pacer_block` 0; turbo `wait_frame` 0 with `pacer_block` p50
5.4-7.3 ms. Verdict, Sean: "That actually fixes the weapon stability
issue nicely without any additional judders."

**Promotion (this build).** `fix.weapon_stability = 1` now means
deferred pacing while on foot: `src/d3d11/native_frame.cpp` derives
`EdvrNativeFrameOutput::deferredPacing` from the key and the journal's
on-foot flag (the runtime side is unchanged from 117348d), the
`experimental.turbo_mode` key is deleted, and the D3D11 correction is
deleted whole: `src/d3d11/weapon_stability.{h,cpp}`,
`tools/weapon_stability_test`, the `family()` list, the particle and
point-light paths, the ADS acquisition and the `weapon timing` eye-dump
trace. What it had been hosting moves out: `weaponMotionDraw` (the
weapon's temporal-AA motion vectors, `weapon_motion.{h,cpp}`, kept
whole) is now called from `vscreen.cpp`'s DrawIndexedInstanced hook
straight after the original draw behind a cheap `weaponMotionWants(vs
hash)` gate, and the resource-written fan-out to weapon, mesh and
UI-depth motion is a file-local `motionResourceWritten` there.
`screen_motion.cpp` still gates the weapon motion vectors on the same
key, as before. `native_frame_test` now stubs the journal functions and
covers all four cases (watcher off, on foot, in a ship, key off).

**Not decided by the flight:** which of the entry's two readings (race
vs cadence) is true -- the symptom is gone either way, and the eye dump
that would tell them apart was not taken. Ruled out: nothing new; the
VS rotation-source hunt ("Mouse turns") is closed as moot, not
answered. The correction's last full state is commit ec7edba and
before, should a runtime turn up where deferred pacing cannot be used.

**Caveats that ship with the default.** One frame of added latency on
foot (rotation hidden by the headset's own reprojection); SteamVR
motion smoothing and Oculus ASW cannot engage while the wait is
deferred, so a rig that lives on reprojection on foot loses it there;
flown on one runtime and headset. With `d3d11.journal_watch = 0` the
fix never engages and the log says so once.
