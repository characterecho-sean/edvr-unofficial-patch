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
- **Open hypotheses** (the signature for each is in Phase 0):
  - H1: under the relative lock the free camera keeps a commander-local
    pose, so a placement is two constants written once.
  - H2: that pose lives in a struct Ghidra can reach from the free-camera
    bindings or `FreeCameraActivity`.
  - H3: writing it moves culling with the picture.
  - H4: the game has its own head-hide switch for the commander model (the
    one the VR cockpit uses).
  - H5: `VanityCameraDataReplicator` does not send camera state off the
    machine.
  - H6: a free-camera-active flag in the same state can replace watching
    the camera key.
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
- **Next:** Phase 0a in Ghidra (no flight), then flight F0 with log-only
  instruments.
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
