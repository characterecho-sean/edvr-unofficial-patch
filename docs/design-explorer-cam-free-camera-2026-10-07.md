# Explorer Cam redesign: place the free camera, hide the head

## Status

- **State: DESIGN, 2026-10-07. Nothing built, nothing flown.** Replaces
  press counting and headset-pose offsets with a placement of Elite's own
  free camera at the commander's head, and hides the head only while the
  camera sits in it.
- **Decision for Sean (D1):** phase 1 writes the game's free-camera state.
  That is the first EDVR write into game-side camera controller memory, it
  retires explorer-cam.md's "never writes", and it must not touch anything
  the game replicates (H5). Phase 0 writes nothing and does not wait on D1.
- **Hypotheses after Phase 0a** (static, 2026-10-07; the signature for each
  is in Phase 0; RVAs are build 332841, exe SHA-256 e6be8bbe...e988, the
  same in the Steam, Epic and Frontier installs; evidence in
  `analysis\decomp\explorer_cam\INDEX.txt`):
  - H1: **READ true.** `FreeCameraActivity`'s update (0x1071980) composes
    world = commander-local (+0x3B0) x commander frame under the relative
    lock and re-derives local every frame.
  - H2: **READ true.** The state is the activity object (rcx at 0x1071980):
    world pose +0x70, local +0x3B0, flags +0x470/+0x471/+0x473, state +0x48C.
  - H3: open, flight F1. A write before the update is upstream of the
    camera and culling view by ordering (stack not traced to 0x5921F0).
  - H4: **no head flag found** (about 25% one exists). The game flips a
    whole-body FirstPerson/ThirdPerson avatar pair (0x2A96740).
  - H5: **inferred not replicated, about 65%.** The pose lives only in the
    activity; the replicator holds handle slots. Write the activity, never
    the shared record (mode, speed, range).
  - H6: **READ, with a change.** Shared record +0x30 = activity +0x48C: 3
    free, 4 relative lock, 5 world lock. Reloaded each frame: watch only.
  - H7 (new): the update's camera collision runs between the compose and
    the re-derive, so if it treats the commander's own body as an obstacle
    it pushes a placement out of the head. Sean can check it by hand,
    before any build: steer the free camera into the head and see if it
    stops.
- **Ruled out:**
  - Counting presses as the source of truth, because it has no origin
    (6ac.6d), and the free camera can be moved by hand after the preset is
    chosen, which no count can see.
  - Driving the camera with synthetic mouse input (SendInput), because the
    head-steer probe on `claude/headlook-functionality-9bada8` moved the
    camera zero at both 200 and 5000 counts.
  - The branch's draw-count ownership test as proof a head is yours,
    because it cannot tell yours from another of the same mesh when yours
    is out of shot (review below).
- **Set aside, not flown:** writing the origin inside the refresh detour at
  `+0x592200`. The culling view (render context +0x40, frustum planes) is
  built upstream of that call (design-occlusion-culling-2026-09-22.md), and
  6s.9 saw holes and body culling from a downstream move.
- **Next:** 0b instruments (I3 detours 0x1071980, rcx = activity), then
  flight F0. 0a is done; its findings are at the end of this doc.
- **Temporary keys:** none yet. Any key Phase 0 adds is listed here and
  removed when the arc closes.

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

## Phase 3: delete the counting (Sean signs off key by key)

These keys and their code go, quoted with behaviour per Scope control
before anything is removed:

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
