# Head steering for Explorer Cam: a design

*A design document, written before the code. Claims about EDVR cite the
source; claims about the game are labelled measured or believed; what can
only be settled in a live session is collected under Phase 0. Nothing here
is implemented yet.*

## The ask, and the bug that led here

Two requests, one feature:

1. **Head-tracked movement** (the ask): in Explorer Cam, pressing forward
   should move you in the direction you are *looking*, not the direction
   the game camera happens to face. The game only reorients the external
   camera's yaw and pitch from mouse input.
2. **A standing yaw trim** (the origin): the Commander Right Shoulder view
   sits slightly yawed left, and the obvious knob — `head_yaw_degrees`,
   which rotates the reported headset pose — is structurally broken and
   cannot be fixed where it lives. Measured chain (2026-08-23): the pose
   math is correct and unit-tested, but a pose-level yaw is fought by the
   VR runtime — the game renders the world turned while the runtime's own
   record holds the true orientation, and its exact rotational reprojection
   turns the frame back at display time. Net: culling and content shift as
   if the camera moved, the view barely holds the turn, and the edges pull
   artifacts. The xyz offsets escape only because runtimes reproject
   position weakly or not at all. No pose-level yaw can ever behave like
   the offsets, on any modern runtime.

The common fix is to stop telling anybody lies about poses and instead
**turn the game's own camera, through the input path the game already
accepts**. When the game turns its own camera: its culling follows, its
movement basis follows (forward is wherever the camera faces — which is the
head-tracked movement ask), and the runtime is never contradicted because
the pose was never touched. The yaw trim becomes a constant offset in the
same loop.

## The UX model

**Gaze steering**: while you hold a movement key, the camera's anchor
re-aims toward where your head is pointing, at a capped rate. Walk forward
looking left, and the camera comes around to your gaze; your head
naturally re-centres as the world comes to you, and you end up walking
where you look — the way every gaze-steer implementation (and a swivel
chair) settles.

Comfort defaults, each a knob:

- **Only while moving** (default on): the camera never turns under you
  while you stand and look around — looking is free, steering happens when
  you move. This is also what prevents the degenerate spin: a stationary
  player whose head stays turned is not steered.
- **Rate cap** (default modest, degrees/second): the world never whips.
- **Deadband** (~2°): straight-enough ahead is ahead; no hunting.
- **Yaw only by default**; pitch steering behind its own knob (the game
  clamps camera pitch anyway, and vertical vection is the least
  comfortable kind).
- **Yaw trim** (degrees): the constant correction the shoulder view needs,
  applied as an offset on the target. This retires the broken
  `head_yaw_degrees` — the key stays documented with a breadcrumb here.

## Architecture

Everything the loop needs already exists in the tree; the one new
capability is injecting mouse input.

**The error signal — where is the head pointing?** The `openvr` half
publishes `headForward` on the shared channel every frame (`frame_flag`
`_v9`, field 4): forward in the current head frame, tangent-space — built
for the retired witchspace fix and still live. The tangents of the head's
yaw/pitch away from forward are exactly the steering error. Phase 0
re-verifies its sign conventions and whether it is taken from the raw or
the offset pose (it must be raw: the Explorer Cam translation offsets must
not read as gaze).

**The feedback — where is the camera pointing?** The camera-records buffer
the d3d11 half already locates and tees (the 5376-byte block; position at
float offset 1100, camera right-vector at 1108, verified in EVIDENCE 6s.3)
carries the anchor's basis. Reading the anchor's yaw makes the servo a
true closed loop: inject counts, measure degrees, and the loop
self-calibrates against the player's mouse sensitivity and invert
settings, which EDVR can never know from the outside.

**The actuation — synthetic mouse input.** `SendInput` with relative mouse
deltas, injected only when every gate agrees (below). Elite consumes
relative mouse motion for camera look in the external camera whether it
reads Raw Input or DirectInput — both see system-injected relative motion.
Precedent: the community runs VoiceAttack and pedal-to-key mappers as
normal practice; this is an assistive remap that only ever acts while the
player is actively holding a movement key, never autonomously.
Alternatives considered: a DirectInput proxy (heavier, another hooked
surface, no added benefit while SendInput works — Phase 0 proves whether
it does) and pose manipulation (measured dead, above).

**The movement-key awareness.** `elite_binds` already reads the player's
own on-foot bindings from the game's config (it is how Explorer Cam's
camera keys work); the same read gives the forward/back/strafe keys for
the only-while-moving gate, watched with the same key-state polling the
hotkey module already runs.

**The gating stack**, outermost first — every layer must say yes:

1. `fix.head_steer = 1` (default 0).
2. The on-foot external camera gate, live (`externalCameraOnFootLive` —
   the same freshness-checked gate the head offsets use).
3. The game window is foreground (never inject into another app).
4. A movement key from the player's own bindings is held (when
   only-while-moving is on, the default).
5. The servo's own limits: deadband, rate cap, a per-frame delta clamp,
   and a fault budget around the whole tick.

**Where it runs**: the d3d11 half — it owns the gate publication, the
camera-buffer tee, the bindings reader and the hotkey polling thread the
tick can ride; it reads `headForward` from the channel. The `openvr` half
changes not at all (its pose path stays exactly as shipped).

## The servo

Plain proportional control with saturation, at the polling thread's
cadence:

    errorYaw = headYawTangent(atan) - trimYaw          // where you look
    step     = clamp(gain * errorYaw, -maxStep, +maxStep)
    if |errorYaw| < deadband: step = 0
    inject(step * countsPerDegree)                      // measured, Phase 0

`countsPerDegree` comes from the closed loop: compare injected counts
against the anchor-yaw delta the camera buffer reports, continuously, so a
player changing their mouse sensitivity mid-session just changes the
constant the loop measures. If the anchor read is unavailable (camera
records rebuilding — the near-planet gaps the view-bridge already
handles), the servo pauses rather than running open-loop: an unmeasured
injection is how a view runs away.

## Phases

**Phase 0 — measure (one field session, instruments only):**

1. Anchor yaw/pitch readback from the camera buffer, logged at 1 Hz while
   in the external camera: does the basis at offset 1108 track mouse look,
   what are its conventions, and does the read survive the near-planet
   record rebuilds?
2. A one-shot injection probe on a hotkey: inject a fixed burst of
   relative mouse counts, log the anchor delta. Proves injection reaches
   the game in the external camera, and measures counts-per-degree in one
   press. (Also, pressed in the cockpit with the feature off, proves the
   gates hold.)
3. `headForward` semantics: raw-vs-offset pose, signs, and magnitude
   against a deliberate 30° head turn.

**Phase 1 — yaw-only servo**: only-while-moving on, rate cap, deadband,
trim knob; field-tune the defaults on the shoulder view. This phase alone
delivers both original asks.

**Phase 2 — polish**: optional pitch steering, smoothing on the step,
possibly a "snap once on camera entry" mode (aim the camera at the gaze
once when the view opens, then leave it — the minimal-vection variant
worth trying against continuous steering).

## Config surface (proposed)

    [fix]
    head_steer = 0                # 1 = steer the external camera to your gaze

    [advanced]
    head_steer_rate = 90          # cap, degrees/second
    head_steer_deadband = 2.0     # degrees
    head_steer_while_moving = 1   # 0 = steer even while standing
    head_steer_pitch = 0          # 1 = steer pitch as well as yaw
    head_steer_trim_yaw = 0.0     # standing correction, degrees (the old ask)

`openvr.head_yaw_degrees` is retired with a breadcrumb pointing here; its
mechanism is measured-dead and its use case is `head_steer_trim_yaw`.

## Risks and unknowns, named

- **Does Elite accept injected relative motion in the external camera on
  every input mode?** Believed yes (Raw Input and DirectInput both observe
  SendInput); Phase 0's probe answers it in one press. If no: the
  DirectInput proxy is the fallback surface.
- **Camera-record gaps near planets** (measured elsewhere): the anchor
  read pauses the servo; the view-bridge lesson says expect ten-to-thirty
  second rebuild windows.
- **Comfort**: continuous steering is vection; the defaults
  (only-while-moving, rate cap, deadband) are the standard mitigations,
  and Phase 2's snap-on-entry is the fallback UX if continuous steering
  reads badly in the field.
- **Etiquette**: injected input is macro-adjacent. This feature only ever
  acts while the player holds a movement key, inside a camera mode with no
  gameplay actions available (documented in the Explorer Cam section:
  no shooting, scanning, or interacting from the external camera), and
  moves nothing but the camera. Same class as VoiceAttack; stated plainly
  in the README when it ships.
- **Two rigs, two runtimes**: nothing in the loop touches poses or the
  runtime, so headset differences reduce to mouse-sensitivity differences,
  which the closed loop absorbs.
