# Explorer Cam redesign: place the free camera, hide the head

## Status

- **State: Phase 0 FLOWN (F0, 2026-10-07); Phase 1a placement BUILT, not
  flown (flight F1 next).** Replaces press counting and headset-pose offsets
  with a placement of Elite's own free camera at the commander's head, and
  hides the head only while the camera sits in it. Keys `fix.explorer_cam`
  and `fix.explorer_cam_eye_up/_forward/_right` are real, user-facing.
- **Branch and scope (Sean, 2026-10-07):** this stays on
  `claude/explorer-cam-redesign-86b9b4` until it ships, and it REPLACES the
  old Explorer Cam route entirely; deleting the old route is authorized.
- **Decision for Sean (D1):** phase 1 writes the game's free-camera state,
  the first EDVR write into game-side camera controller memory; it retires
  explorer-cam.md's "never writes" and must not touch anything replicated.
- **Hypotheses** (RVAs are build 332841, exe SHA-256 e6be8bbe...e988, all
  three installs; static evidence in `analysis\decomp\explorer_cam\`):
  - H1 **READ + FLOWN F0:** `FreeCameraActivity`'s update (0x1071980)
    composes world = commander-local (+0x3B0) x commander frame under the
    relative lock; in F0 local held bit-identical through a 15 m walk and
    turns of up to 166 degrees.
  - H2 **READ + FLOWN:** the state is the activity (rcx at 0x1071980):
    world +0x70, local +0x3B0, flags +0x470/1/3, state +0x48C.
  - H3 open, flight F1: a write before the update is upstream of the camera
    and culling view by ordering (stack not traced to 0x5921F0).
  - H4 no head flag found (about 25% one exists); the game swaps whole-body
    avatars (0x2A96740). Head hiding stays EDVR's draw skip.
  - H5 inferred not replicated (about 65%): write the activity, never the
    shared record (mode, speed, range).
  - H6 **READ + FLOWN:** +0x48C is 3 at TAB (first call), 4 relative lock,
    0 at exit; mirrored from a shared record, so watch it, never write it.
  - H7 **READ (0a-2):** the update's collision is a 0.4 m sphere sweep, or
    a 0.8 m ray when the pose is static, in FUN 0x1091140, which only this
    activity calls; 0.70 = 0.30 body surface + 0.40 radius. Neutralise it
    with a CodeHook on 0x1091140 while placed (option A below).
- **Ruled out:**
  - Counting presses as the source of truth, because it has no origin
    (6ac.6d), and the free camera can be moved by hand after the preset is
    chosen, which no count can see.
  - Synthetic mouse input (SendInput), because the head-steer probe moved
    the camera zero at 200 and 5000 counts.
  - The branch's draw-count test as proof a head is yours, because it
    cannot tell yours from another of the same mesh with yours out of shot.
  - I2's 5376-block fingerprint as the commander's position, because in F0
    its translation tracked the free camera's own origin within 0.01-0.09
    m; the commander's frame comes from I3 (world with local taken out).
- **Set aside, not flown:** writing the origin inside the refresh detour at
  `+0x592200`; the culling view is built upstream of it
  (design-occlusion-culling-2026-09-22.md), and 6s.9 saw holes from that.
- **F0 FLOWN** on Frontier, 2d103d84, `edvr_gfx_20261007_172543.log`,
  training scenario; findings at the end. Frontier's ini: probe off again,
  `head_offset_gate = 0`.
- **Next:** F1 on Frontier (install, grep `explorer cam:`; checks H3: no
  holes, no body culling, no pop at entry, walking and turning, HMD pose not
  doubled), tune the eye keys, then Phase 2 (hide the head) and the old
  route's deletion.
- **Temporary keys:** `[advanced] explorer_cam_probe = off|on` (default off;
  all three 0b instruments plus the VR camera census). Removed at arc close.

## Why today's Explorer Cam is half-baked

It knows which camera preset is showing only by counting next-view presses,
and a missed press leaves the count wrong until the next wake. On the right
preset it adds a fixed offset to the headset pose the game is told about,
tuned for Commander Right Shoulder.

The 2026-08-19 field report shows the stereo scene appears only once the
player enters free camera (TAB). So the preset only decides where the free
camera starts. A player who then moves the free camera by hand carries the
offset somewhere else, and no count can see that.

Index 0, where the camera opens after a reset, is a selfie view facing the
commander. Starting there needs a turn of about 180 degrees, and a
translation offset cannot turn anything.

## What the game already offers

From Sean (2026-10-07) and the binding names in his live `.binds`:

- Free camera (`ToggleFreeCam`, `MoveFreeCam*`, the mouse) moves and turns
  the camera by hand.
- A lock fixes it once placed. By name these are `FixCameraRelativeToggle`
  and `FixCameraWorldToggle`. Under the lock the camera stays attached to
  the commander as they walk and turns with them on mouse yaw. Mouse pitch
  bends the body but does not move the camera vertically.

A player can already put the camera at their own head by hand and lock it.
Explorer Cam automates that placement, so explorer-cam.md's "no capability
you do not already have" stays true word for word.

## The design

### Camera: place, then lock

On free-camera entry EDVR writes the free camera's commander-relative pose
once: an eye point (right, up, forward) in the commander's frame and a yaw
of zero, i.e. facing the way the commander faces. It then sets the
relative lock. The game handles walking, turning, stereo, the headset pose
on top and culling. EDVR stops offsetting poses, and the preset no longer
matters, the selfie view included.

The starting eye point comes from the head-anchor measurement on build
332753: up 1.72, forward 0.20, right 0.12 from the commander's root at the
feet. F0 rechecks it.

If H1 fails because the free camera stores a world pose, placement needs
the commander's root on each entry. I2 below gives the root relative to the
eye, which with the camera's own origin is enough to compute a world
target. The lock then holds it, so it is still one write per entry.

### Head: hidden only while the camera is in it

In order of preference:

1. **The game's own switch (H4).** The VR cockpit shows the commander's
   body without a head. If that is a flag on the character, setting it in
   free camera covers every suit, helmet and body with no per-suit data.
2. **EDVR's draw skip**, the branch's chokepoint with the review's fixes:
   - Gate on "placed", not "in camera".
   - Prove ownership by position: the draw's own root must sit where we
     put the commander, about 1.72 m below the eye and within about 0.5 m
     horizontally.
   - Count instances, match overlapping parts safely, and let a stand-down
     decay rather than latch for the session.
3. **No hand-written specs.** A player cannot run a census A/B. One
   candidate: among the draws whose root is the commander's, a mesh whose
   model-space bounds sit in the top 0.35 m of the body is head, hair,
   eyes or helmet. That needs one staging read per unique vertex buffer.
   It is a candidate only; F0 checks it against the branch's ten-part list.

## Phase 0: find it, write nothing

**0a, static, no flight.** Use Ghidra 12.1.3 headless on
`analysis\ghidra\EDAnalysis`. It was analysed 2026-09-19, so first confirm
it matches the installed exe. `analysis\ghidra_scripts\CameraSetter*.java`
are templates.

- From the binding names `ToggleFreeCam`, `FixCameraRelativeToggle`,
  `MoveFreeCamX` and `Bindings_VanityCameraHeadLook`, and from
  `FreeCameraActivity` (registration stub 0x141086100), find the action
  dispatch and the free-camera update. Name the struct they write and its
  fields: position, yaw and pitch, lock mode, commander reference
  (H1, H2, H6).
- Check whether `VanityCameraDataReplicator`'s serialize path reaches the
  network layer (H5).
- Find what removes the head in the VR cockpit: a render flag on the
  character that differs by camera mode (H4).
- Deliverable: function RVAs with prologue bytes for an observation hook,
  field offsets, and the H5 answer.

**0b, instruments, log only.**

- **I1, camera census.** Extend `advanced.vr_camera_census`, which already
  snapshots camera +0x20..+0x2AF, to log origin (+0x50) and axes (+0x20) by
  kind and call site, at 1 Hz and on change, while in the external camera.
  A per-5 s line of calls per kind separates "the hook never saw the
  external camera" from "nothing changed".
- **I2, commander root.** Port the branch's 5376-byte fingerprint read
  (`head_probe.cpp`, `headAnchorNote5376`) as log-only: the nearest skinned
  root relative to the eye, plus the 3x3 at float 932 decoded to a yaw
  relative to the camera. It has no head spec and takes the nearest root,
  so F0 is flown alone.
- **I3, free-camera state.** An observation detour at 0a's function: the
  existing stub emitters with 0x20 bytes of home space, a prologue check
  and stand-down on mismatch. It logs the candidate fields on change, and
  prints its call count per 5 s so "never ran" is visible.

**0c, flight F0.** VR, on foot, alone; the log's version line must name the
build. Steps:

1. Open the camera (index 0, selfie) and press TAB.
2. Move the free camera by hand about 2 m, then set the relative lock.
3. Walk 10 m straight, turn 90 degrees with the mouse, then pitch up and
   down.
4. Set the world lock and walk 5 m.
5. Leave the camera, then sit 30 s in the cockpit for H4's contrast.

| Hypothesis | Confirmed if | Refuted if |
|---|---|---|
| H1 | I3's pose field is constant through step 3 and changes in step 2 | it changes with every step of the walk |
| H2 | I3 sees the field move when the camera is moved by hand | no candidate tracks step 2 |
| H6 | I3's flag flips at TAB, within a frame of the panel stopping | no field flips at TAB |
| H4 | 0a names the flag and its value differs between cockpit and camera | no such flag; fall back to the draw skip |
| I1 frame | an external-camera origin moves in step 2 and follows the walk in step 3 | no kind's origin tracks step 2 |

## Phase 1: place (needs D1, H1, H2 and H5)

Write the placement at entry through the game's own setter if 0a finds one,
otherwise the field directly, and set the relative lock. Remove the pose
offset from Explorer Cam's path. Flight F1 checks H3: no holes, no body
culling, no pop at entry, the selfie preset, walking and turning.

## Phase 2: hide the head

Use H4 if 0a found it. Otherwise port `commander_head.{h,cpp}`, which
depends only on Config and Log, with the fixes above. Main's
`bindingShaderHash(BindSlot::Vs)` replaces the per-draw `VSGetShader`.

## Phase 3: replace the old route (authorized by Sean, 2026-10-07)

The new route replaces the old Explorer Cam entirely, and Sean authorized
deleting it. It goes in the same build as Phase 1, so the branch never
carries two Explorer Cams. The old gate, press counting, camera-key
adoption and pose offsets go with their keys and `gate_test` sections; the
commit lists every key removed. Shared pieces stay: the journal watch, the
binding lookups FSS uses, the Disembark/Embark counters `static_prop_gate`
uses, and `hotkey.read_game_bindings` (the menu's panel keys). The keys
that go include:

- `advanced.head_offset_view` and `fix.head_offset_view_count`.
- The menu action "Reset Explorer Cam's counted view to 0".
- `experimental.keyless_camera`.
- `fix.head_offset_intent_grace_ms` and `fix.head_offset_enter_window_ms`,
  if H6 replaces key entry.
- `openvr.head_offset_*` and `openvr.head_yaw_degrees`, once placement
  replaces them.

The code is roughly 400 lines across `head_offset_gate.cpp`,
`device_hook.cpp`, `eliteBindsLookupPadMod` and `xinputVeto`, plus about
200 lines of `gate_test`.

## Fallback if the free camera cannot be written

EDVR is now the runtime, and the projection layer carries the real tracked
pose (`d3d11_stereo.cpp:338`, `native_runtime_host.h:1229`). Only the game
sees the offset. The August "reprojection undoes the yaw" was measured on
SteamVR, so a pose-level yaw probably survives now. Test it with
`openvr.head_yaw_degrees = 180` on index 0.

Two code changes would be needed:

- `applyNativeHeadOffset` rotates orientation only; position and
  velocities must turn with the yaw, and the test at
  `launch_centre_cases.h:209` pins the old behaviour.
- The offset should be servoed from I2's measured root rather than counted,
  which also removes the counting.

This keeps lying to the game, so it is the fallback, not the plan.

## Review: the head-hiding work on `claude/headlook-functionality-9bada8`

The work is from 2026-08-30/31, unmerged, and built on the deleted
`src\openvr` proxy; main is about 2,100 commits ahead. It was flown on one
rig with one commander and one suit, and a ten-part spec hid the head
fully.

**Sound:** the chokepoint. Each eye draw is skipped after the census and
probe have seen it, so instruments still see the head. It holds no state
and fails safe: a stale spec matches nothing and the head shows, with a
log line.

**Defects, most severe first.** The first four were checked against the
branch code:

1. **Identity is a count, not ownership.** A part matches on draw kind,
   index count and vertex-shader hash only (`commander_head.cpp:136`). It
   is "proven yours" if no frame in 12 draws it more than the declared
   number of times. An NPC wearing the same mesh, with your own head out of
   shot, proves itself and loses its head.
2. **Wrong gate.** `commanderHeadSetInCamera(headOffsetGateInCamera())`
   (`vscreen.cpp:4387`) hides the head on every camera preset, including
   ones facing the commander where the viewpoint never moved.
3. **A stand-down latches for the session** (`commander_head.cpp:288`).
   One passer-by restores that part until a config edit. Parts stand down
   one at a time, so a partial head (floating hair or helmet) is possible.
4. **The first matching part wins** (`commanderHeadOnEyeDraw`). A
   shader-only part listed beside a size-and-shader part steals its counts,
   so the guard is blind to the overlap the doc recommends.
5. **Instances are never passed to the matcher,** so instanced crowds would
   count as one character.
6. **Specs are hand-made per suit.** They come from a census A/B only Sean
   can run, the shipped default is empty so the feature does nothing out of
   the box, and the cap of 16 parts holds about 1.6 heads.
7. **Eye-sized targets only.** On a rig whose eye render size is not
   recognised, the head silently stays visible. Shadows keep the head,
   which is arguably right.
8. `docs/commander-head.md` on the branch still carries an overturned
   "negative result" section.

**Never flown:** a real stand-down with an NPC in shot, other suits,
helmet on and off, body variants, EDHM, other headsets and eye sizes.

**What to keep:** the chokepoint, the per-part judge, the parser and
`tools\head_test`. Replace the identity test with Phase 2's positional
one. The head-anchor servo stays behind: it needed the rig-follow term
(float 1100) and was written against the proxy that no longer exists, and
the placement design does not need it.

## 2026-10-07 Phase 0a: static findings

Build 332841, SHA-256 e6be8bbe...e988; every RVA below is for it. The
Steam, Epic and Frontier installs and the Ghidra project all hash the same.
READ = seen in the decompilation or disassembly, INFERRED = reasoned.
Evidence and tools: `analysis\decomp\explorer_cam\INDEX.txt`.

**The object (H2, READ).** The free camera is `FreeCameraActivity`. Its
update is 0x1071980 (rcx = the activity), run through job thunk 0x102FC20
(slot 22 of the "Camera" table at 0x51854A0). It serves on foot too: the
binding builder 0x1094860 picks Driving for context 2, Humanoid for 4,
Flight otherwise, and the lock actions exist only in its binding set.
Fields (row-major 4x4, axes in rows 0-2, origin in row 3):

- +0x70..0xAC world pose; +0x370..0x3AC last world; +0x3B0..0x3EC
  commander-local pose (origin +0x3E0/E4/E8); +0x440..0x46C captured frame.
- +0x470 relative (1) or world (0); +0x471 rotation lock (1 = live frame);
  +0x473 preset placement pending; +0x48C state: 0 off, 3 free, 4 relative
  lock, 5 world lock, 6 a variant that enters with +0x471 = 0.
- Action handles (+0x18 float, +0x1C int pressed): +0x4C8 X, 4D0 Y, 4F0 Z,
  4D8 pitch, 4E0 yaw, 4E8 roll, 4F8 ToggleRotationLock, 500 FixCameraWorld,
  508 FixCameraRelative. +0x2C8 is the target's transform (matrix via
  vtable +0x20), the commander on foot.

**H1 true (READ).** With +0x473 == 0 and +0x470 != 0 the update builds
world = local x frame: `fVar36 = fVar58*fVar36 + local_398 + fVar59*fVar42
+ fVar60*fVar29` (decomp_1071980.txt line 688; local origin rotated by the
frame, plus the target origin). After the move and collision code it
re-derives local: `fVar59 = fVar59 - fVar34` (line 1269), then +0x3B0 =
world rows . frame rows (1281-1296). The origin-shift rebase 0x1091520
rewrites +0x70/+0x370/+0x440 but not +0x3B0, so local is the invariant:
walking changes only the frame. The first update (ctor 0x1060190 sets
+0x470..+0x473 = 1) seeds the pose from the preset matrix plus config
offsets (0x106F0A0), so the preset only decides the start.

**H6 changed (READ).** +0x48C is reloaded from a shared record every update
(line 254) and stored back (1297); the vanity controller (0x2DF14C0,
SetMode 0x2E01550) writes the same byte. Watch it, do not write it. Lock
levers: the pressed state of action +0x500/+0x508 (read every frame), or
the record byte, which is the H5 risk.

**To the camera (INFERRED).** The update leaves the final pose at +0x70,
inside the activity's camera-params blob (+0x30..+0x138, copied by
0x1066D80). The game's translate 0x4F2AC0 has one caller, 0x28A4D30, not
this path. Not traced to 0x5921F0; a pre-update write is upstream by order.

**Hook for I3.** Entry 0x1071980, rcx = activity. Prologue `48 89 5C 24 20
55 57 41 55 41 56 41 57 48 8D AC 24 40 FD FF FF 48 81 EC C0 03 00 00`,
no rip-relative bytes in it; boundaries at 5, 6, 7, 9, 11, 13, 21, 28.
Expect once per frame while the activity exists (INFERRED; the call
counter proves it), maybe on a worker thread: copy, take no lock.
Edges: OnEnter 0x10996A0 (`40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D
6C 24 E1 48 81 EC A8 00 00 00`), OnExit 0x10888E0 (a rel32 call at byte
15). A later write goes before the compose: with +0x473 == 0 and +0x470 ==
1, 16 floats at +0x3B0 and +0x471 = 1. No dirty bits at this level; the
stack sets the camera's own.

**H5 (INFERRED, about 65% not replicated).** Both types are pooled
components (0x5F52370 block 0x120, 0x5F52460 block 0xF0; factories
0x10857C0, 0x1085830). Their constructors register only handle slots
(0x529550 links a list node); the replicator exposes two 16-byte slots and
two flag bits, has no Read/Write virtual of its own, and the activity never
touches it. The activity's writes to the shared record are range, state,
rotation lock and speed (+0x20/24/30/40/44/50), never a pose. Not found:
the ReplicationService's property walk, so not proven.

**H4 (about 25% a flag exists).** The local humanoid has two avatars,
FirstPersonAvatar and ThirdPersonAvatar (0x1C2EC60); 0x2A96740 flips them
and sets data key "HumanoidFirstPersonMode": whole body, not head. Avatars
are also built per ResourceContext (Cockpit, Humanoid_FirstPerson, ...,
table 0x5E9C780) from parts Head, Eyes, Helmet, SkullCap, Hair, Beard,
Teeth (0x5E9C7D0), so the headless cockpit body is probably a build choice.
Phase 2 stays on the draw skip.

**Unknown:** what calls the job and on which thread; the row convention
(right/up/forward); whether TAB on foot always reaches this activity (F0's
call counter and +0x48C answer it).

## 2026-10-07 Phase 0b: instruments built

Key: `[advanced] explorer_cam_probe = off|on`, temporary, VR profile only.
On also switches on `advanced.vr_camera_census`. Log only: nothing in the
game is written. Every line starts `explorer cam probe`; F0 greps these:

- `: on` / `: off` once. `I3 armed:` (stolen bytes) or `I3 stood down:`
  (build differs; I1 and I2 still run, their 1 Hz lines do not).
- `I3 heartbeat:` every 5 s: calls, activity pointers, thread ids, +0x48C.
  Idle reads `idle=no-call-yet` (hook never reached) or
  `idle=no-call-in-window`.
- `I3 change:` at once on a change of +0x48C, +0x470, +0x471 or +0x473:
  old->new for all four, `first-call` on first sight of an activity.
- `I3 pose:` at 1 Hz while +0x48C != 0 and 2 s after (`phase=active|hold`):
  local origin, local and world basis rows, world origin. `I3 stale:` when
  an activity stops being called with +0x48C != 0.
- `I1 armed:`, `I1 heartbeat:` (`census_calls`, `kinds`; idle reads
  `idle=no-refresh-call-reached-the-probe`), `I1 cam:` per kind and call
  site at 1 Hz: calls per frame, origin, axes rows.
- `I2 armed:`, `I2 heartbeat:` (`blocks_offered`, `skinned`, `far`; idle
  reads `idle=no-5376-byte-block-offered` or `idle=blocks-offered-none-
  skinned`), `I2 root:` at 1 Hz: distinct blocks, nearest root in view
  space, model-axis yaw and pitch, the convention printed in the line.

Yaw and pitch assume view x right, y up, z forward (unmeasured); the 3x3 is
logged too, so F0 can recompute. The hook forwards four integer registers
and snapshots after the original returns.
## 2026-10-07 F0 flown: the free camera, measured

Frontier, build v0.18.3-13-g2d103d84 (log version line checked),
`edvr_gfx_20261007_172543.log`, the on-foot training scenario. Parsed with
a scratch script over the `explorer cam probe` lines. Commander-local axes
as measured: +y up, +z the way the commander faces.

- **Hook.** Armed, 28/28 bytes. It saw no call before TAB, then one
  activity on one thread (56076), called once per frame: 9,229 calls over
  frames 12,535 to 21,764. No faults, drops or stale lines.
- **State byte.** 3 at TAB, 3->4 at the relative lock, 4->3, 3->4, 4->0 at
  exit; 5 (world lock) never appeared. The activity's first call is a
  usable entry signal.
- **Preset 0.** It opens at local (0, 1.50, 1.90), facing back at the
  commander, the selfie Sean described.
- **H7.** Steered in by hand, the camera stopped at local (±0.02, 1.70,
  0.70). Sean saw it stop about 0.25 m in front of his face.
- **H1.** Locked, local stayed at (-0.024, 1.705, 0.934), bit-identical
  over 60 s, while the commander's root moved about 15 m and its frame
  yawed by up to 166 degrees with zero tilt. Only once did local change
  under the lock: from 0.70 to 0.93 at 17:28:29-31, during the first turn
  after a walk, with the root standing still. That is the body pushing the
  camera, not a radius.
- **I2.** Its "nearest root" equals the free camera's world origin within
  0.01-0.09 m (one second late while moving), so it is not the commander;
  ruled out in Status.
- **I1.** Kind 5 (the eyes, about 63 calls a frame) sits at about the
  render origin, so the render frame is camera-relative. Kind 3 appeared in
  only 10 of 105 seconds. No census camera matched the free camera's
  world origin directly; the I3 world frame is the game's, not the
  render's.

## 2026-10-07 Phase 0a-2: the collision step

Build 332841. READ = seen in `analysis\decomp\explorer_cam\` decompiles
or disassembly; INFERRED = reasoned. Line numbers are `decomp_1071980.txt`.

**Pipeline (READ).** After input is applied (0x106DEC0), the update runs:
1. Containment and range, lines 766/770: 0x108F420 puts the origin back into
   the bound object's oriented box (object from handles +0x2B0/+0x340; null
   skips it), 0x108F7E0 clamps to the target's max range (config +0x18/+0x1C
   minus 0.5). Both edit the origin in place; other activities use them too.
2. 0x1091140 (lines 881, 1019; call sites 0x1072710, 0x1072930; no other
   caller in the exe). If the origin moved more than 1/1024 (0x518C738) it
   sweeps a sphere of radius 0.4 (0x518C73C) through the physics world
   (0x10A9E00) and writes the stop point. If it did not move it casts a
   0.8 m ray centred on the camera along commander-to-camera and moves the
   origin along that axis by (hit - 2 x 0.4); its sign was not resolved.
   Stage 2 accepts the result unless containment objects, which sets
   +0x478 and runs the recovery solver 0x1086C00 from then on.
3. Lines 998-1019: 0x108F1B0 and 0x108EEE0 only count overlaps (other
   entities, and the commander as a box expanded by 0.25 for a humanoid,
   0x5F518B0; 1.0 SRV, 2.0 ship; 5 m neighbour box). Counts feed the shared
   flag +0x1D and +0x47C, never the position; asm 0x10728B6-0x1072954
   discards the edited copy.

**Why 0.70 and 0.93 (INFERRED).** The commander is not ignored: the sweep
passes the commander handle as its ignore entity only when 0x108EBC0 is
false or +0x473 is set (0x1091194), and F0 stopped at 0.70 = 0.30 + 0.40.
The turn push moved only z (x stayed -0.024), which fits the radial ray, not
the chord of a sweep. The ray filter 0x1095F40 skips hits on the commander's
own bodies unless a second component is present, so the pusher may be an
attached entity (weapon) or the body itself; 0.934 - 0.4 = 0.53 m of reach.
Nothing pulls the camera back, so the push ratchets. I4 decides.

**Gate (READ).** `VanityCameraCheckForCameraCollision` and
`VanityCameraInertialSimulation` are camera activity types (enter 0x109D3E0,
0x109DD50), not options; the free camera's collision is hard-wired. No
activity or shared-record field skips it. `+0x473` ignores the commander
on the first update only.

**Ordering (INFERRED).** No call in the update publishes the matrix; it
ends by storing +0x70 and +0x370 and deriving +0x3B0 (line 1281 on), so the
stack pulls the blob in a later job. A post-call overwrite lands before that
pull unless the pull already ran this frame; the consumer is not named. A
write to +0x3B0 is read by the next update's compose either way.

**Options.** A (best): CodeHook 0x1091140, prologue `40 55 53 56 57 41 54
41 56 41 57 48 8D AC 24 B0 FE FF FF 48 81 EC 50 02`, boundaries at 2, 3, 4,
5, 7, 9, 11, 19, 26. rcx = activity, r9 = result pose; return `xor eax,eax`
for rcx = ours while placed. Touches no game memory; the pose stays the
activity's own, so the stack and culling follow as for any free-camera pose
(H3, F1 checks). Risks: no wall stop (as in first person); the flag +0x1D
can still go to 1 via the overlap counts. B: post-update write of +0x70,
+0x370, +0x3B0: the game re-pushes every frame, +0x478 recovery and
0x1086C00 run, and the shared flags flicker. C: write +0x3B0 once at the
second update (state 3, +0x473 = 0) with A on.

**HMD (INFERRED).** The update reads no tracking pose; the game folds it in
downstream ("the game moves its own camera", explorer-cam.md). EDVR's runtime
reports poses in OpenXR LOCAL (`src/openxr/seated_space.h:29`,
`session_binding.cpp:51`), zero at recentre, so an eye-height placement is
not doubled; F1 confirms.

**Inertia (READ + INFERRED).** The activity holds no velocity: 0x106DEC0 is
dt x speed x input, with only the speed index +0x474 and timers. Damping is
the separate InertialSimulation activity (enter 0x109DD50), switched by the
bag key at +0x490, which the update sets 0 (line 502), then 1 under the
live-frame lock (line 662). Its smoothing of a jump was not read.


**Checked by the overseer against the exe on disk (2026-10-07).** The
prologue bytes, 0.4 at 0x518C73C and 1/1024 at 0x518C738 all match, and the
only direct references to 0x1091140 are the two calls at 0x1072710 and
0x1072930. Before the first call, 0x10726B3-0x10726D8 copies the candidate
pose (frame +0xF0..+0x120) into the result buffer and keeps it in
xmm7-xmm10. A return of 0 then carries that candidate on unchanged at both
sites: site 1 jumps to 0x107289C, which stores xmm7-xmm10, and site 2
jumps to 0x1072A8D, which copies +0x40..+0x70 back. So returning 0 means
"no collision edit": it neither freezes the camera nor exposes an unset
pose. **0x1091140 takes a FIFTH argument on the stack** (a byte, `mov
[rsp+0x20], al` before each call). A C replacement must declare and forward
five arguments. Better, the relay returns `xor eax,eax; ret` itself for
the placed activity and jumps to the trampoline for every other caller.

## 2026-10-07 Phase 1a: placement built

Compiled and gated, not flown. Keys (VR profile; flat reads them off): `[fix] explorer_cam = on|off` (default on),
`explorer_cam_eye_up = 1.68`, `_eye_forward = 0.10`, `_eye_right = 0.0` (metres from the commander's feet, live,
clamped 0.5..2.5 and -0.5..0.5). Code: `explorer_cam_core.h` (pure), `explorer_cam.{h,cpp}` (hooks, config, log); the
probe attaches to the same 0x1071980 hook. Rig: `tools\explorer_cam_test`.

**Writes (D1), nothing else.** Before each free-camera update: the 16 floats at +0x3B0 (identity rows, origin right/up/
forward, each row's 4th float the game's) and, once per entry, the int at `*(+0x508)+0x1C` set to 1 and restored after
the update. A relay on 0x1091140 returns 0 when rcx is the placed activity.

**State machine, per activity (hook thread).** (1) Idle until a fresh session (+0x48C 0, +0x473 1, or a new pointer)
shows +0x48C = 3. (2) Entered; waits for +0x473 = 0 and +0x470 != 0. (3) First write: publishes the activity to the
relay and presses the lock if +0x48C = 3, once per session even after the user unlocks. (4) Every update at +0x48C 3 or
4 writes. (5) Releases on +0x48C 0, 5, 6 or unknown, key off, 30 silent frames (frame thread) or a fault (8 faults end it
for the session); after 5, 6 or a fault it re-enters only after a 0.

**F1: grep `explorer cam:`** (the probe's lines start `explorer cam probe`). `on (fix.explorer_cam = on)`; `free-camera
hook armed` and `collision hook armed` (stolen=5, 28/28 and 26/26) or `... stood down`; `first free-camera update
reached the hook` (the hook ran); `entered the free camera`; `placed: ... eye(...)`; `lock pressed: ... before=3
after=4` (read on the next update); `heartbeat:` every 5 s while placed (`updates_placed`, `collision_bypassed`,
`collision_forwarded`, `hook_calls`, `faults`); `released: ... why=`; `eye changed`; `fault N of 8`.
