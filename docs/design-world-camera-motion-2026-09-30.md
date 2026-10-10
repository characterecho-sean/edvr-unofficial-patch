# All view motion from the world camera (design, 2026-09-30)

## Status

- **State (2026-10-01):** Phase 0 (census episodes) and Phase 1 (the
  on-foot panel gate, with the layer-only door under it) are on main
  (df9172db) and FLOWN ONCE (Frontier, `edvr_gfx_20261001_082459.log`, route
  off): Sean, "it all looks perfect to my eye". The reader said WARN; section
  8.10 says why: a boarding it could not tell from a defect. No Phase 1
  defect; the 5 s line gains `screen-draws=` and the reader judges by it (a
  Phase 1 fix on branch `claude/vr-route-curvature`, NOT FLOWN). Sean approved
  the design 2026-10-01: (i) for Phase 1; any non-world panel to the layer, not
  just the maps; the eyes skip the upscaler under a held panel. Implementation
  note and flight plan: sections 8 and 9. Phases 2 and 3 untouched. Origin:
  flight 1 of the VR world route (design-flat-temporal-aa-2026-09-23.md
  section 82; `edvr_gfx_20260930_161545.log`, v0.18.0-rc.4-104-gbde47f81).
  Sean's direction: every view's temporal-AA motion comes from the game camera
  that renders it, not head heuristics.
- **Keys:** none. `experimental.on_foot_maps_sharp` (default ON since
  2026-10-01; it covered menus too) and `experimental.temporal_aa_on_foot_world`
  were removed in the 2026-10 key cull: the gate and the VR world route are
  always on, so there is no "off" leg to compare against. `advanced.vr_camera_census`
  (episodes, naming runs and the detour's CPU ride it) is the one key left here.
- **Trigger (Sean's correction):** the maps smear when dragged ON FOOT,
  where the game draws them into the 2D panel. The gate keeps the panel in
  the eye route (the journal says on foot), nothing names its camera, and
  DLSS gets head-only motion (section 1). The cockpit's stereo maps are
  fine: a regression check, not a phase.
- **Recommendation:** Phase 1 is option (i): on foot the panel counts as the
  world only while a draw that reads the world camera names it; otherwise the
  UI layer composites it after the upscaler. Option (ii), the map's own
  camera, waits on Phase 0 (section 4).
- **Phases (section 6):** 0 census episodes, 1 the on-foot panel gate, 2 the
  cockpit's rows from its eye cameras, 3 the rest. The VR world route's
  stage 2 (another worktree) is untouched.
- **Open hypotheses:** H1: on-foot maps and menus never name a source
  (flight 1, both gaps: "no pool family draw went to a depth of the screen's
  size"). H2: in an on-foot world, naming never drops for 3 frames (station:
  none outside the gaps in 14,220 frames; settlements unmeasured). H3: the
  cockpit maps are fine because kind-5 eye cameras' rows drive the world
  path (code reading).
- **Ruled out:** end of section 1.
- **Next flight:** what can still be measured (section 9's off/on legs cannot
  be flown: the key and the route switch are gone): census on, one on-foot leg
  with the gate and the route as built, and the arrival leg (load into on-foot
  play), judged against the logged baselines in this doc rather than a live
  comparison. Read with `python tools\edvr_log.py --target frontier
  --expect-build HEAD --maps-sharp` and `--camera-census`.
- **Open (2026-10-01, corrected):** the arrival spell, not Cinema mode. After
  a load into on-foot play every frame is declined for 27 s
  (`edvr_gfx_20261001_074129.log`, 07:42:53-07:43:20) to 62 s
  (`..._060011.log`, 06:00:58-06:02:00): the gate open (journal: no Flags2;
  screen depth 4 draws a frame), the screen black at every luma stage; then the
  world appears and the route owns 0.30 s later. Section 8.6 says what the
  build did there with the key off and on (the key is gone; the "on" behaviour is the build's). No fix for the spell is built.
- **Environment:** EDVR's OpenXR runtime; Pimax Crystal Super, 90 Hz, HMD
  quality 0.65 (eye 2620x2533, output 4032x3898); 2D screen 5040x2835 in
  flight 1 (`fix.vscreen_res_width` auto, the legacy rule; main's auto-fit,
  merged here, fits it to the eye when the game LAUNCHES with the route auto:
  section 9); DLSS preset K. Phase 1 needs the UI layer
  live (`fix.ui_quality` above 0, on by default, and `fix.temporal_aa` on);
  with it off, or under the Oculus native SDK, the on-foot map keeps its
  smear.

## 1. The trigger

On foot Odyssey draws the world once into the 2D screen's texture; each eye
shows it through one composite draw (VS 5C36AF05, PS CFE84157). A pool-family
or terrain draw into a screen-sized depth names the world camera (its b1)
and depth for screen motion (screen_motion.cpp:260-315), which maps the
motion to eye pixels at each composite (:387-506). The on-foot maps make no
such draw. Flight 1, gaps 16:22:41.9-16:22:59.96 and 16:23:01.3-16:23:12.0:

- 16:22:41.911 `auxiliary camera rows do not follow the head`; 16:22:43.103
  `the 2D screen showed for 90 frames and nothing named its source`.
- 30 s world-screen lines: held by the journal alone on 238 frames
  (16:22:45.8) and 1,269 (16:23:15.8); screen depth 22 draws a frame; the
  screen `left in the picture ... the temporal pass keeps it`; GuiFocus
  unknown on every on-foot frame. DLSS ran on it (2.5 ms a pair).

With no source screenMotionView is null (:507-510), and on foot the eyes have
no scene depth (16:20:05.790), so the pass runs on the head's rotation alone
(temporal_pass.cpp:3533-3545): the panel as a still picture at infinity,
while a drag moves its content. The world route released as designed (2,396
`depth-not-screen-motion-source` declines, section 82). The gate's comment
expected a map to be held like the world (ui_layer_math.h:902-907); on foot
no camera names one.

**Why the cockpit maps are fine (H3).** There kind-5 eye cameras (the census
saw the left eye's object as kind 5 in the cockpit) draw the map into eye
targets with an eye-sized depth. The pass's rows come from the block bound at
the scene's first draw (temporal_pass.cpp:6650-6680), so they carry the head
and the map's orbit; a bound block in a populated scene pins the follow score
at 30 (temporal_math.h:50); pixels past 10 m take the rows' delta
(temporal_shader_source.h:716-722). A drag moves the rows and the vectors
follow: the camera that renders the content drives its motion.

- ruled out: Status.json's GuiFocus as the on-foot map signal, because every
  on-foot frame of flight 1 read it unknown, maps included.
- ruled out: rejecting the unnamed panel's history, or a reactive mask, as
  the fix, because DLSS would still be told the content is still.
- ruled out: the screen's depth count as the world witness, because a 3D map
  draws into the same depth (22 a frame here; a busier one could pass the
  64-draw hold); only a draw that reads the world camera proves the world.
- ruled out: "in the arrival spell the eye route serves the panel without
  screen motion, and that is the ghost Sean saw" (design-flat-temporal-aa
  section 82, FLIGHT 4), because the layer's world-screen line reads 5,174 and
  5,396 2D screen draws asked, 0 left in the picture (060011, 30 s windows
  ending 06:01:12 and 06:01:42): the layer took every one, and the luma probe's
  final stage, read after the layer's composite (native_sharpen.cpp), is 100%
  black at every sample of both spells. A ghost Sean saw was not the eye route
  filtering the panel in the spell, or it was not in the spell (8.6).

## 2. Inventory

| # | Where | Uses | When | Guards against |
|---|---|---|---|---|
| 1 | eye shift, native_temporal.cpp:349-360 | Halton tangent shift | unless the route owns | (upscaler samples) |
| 2 | head delta, native_temporal.cpp:436-461 | runtime poses | every eye | (no other source) |
| 3 | rotation alone, temporal_pass.cpp:3439-3447, 3533-3545 | item 2 | no eye depth (on foot) | translation without depth |
| 4 | head with depth, temporal_pass.cpp:3564-3579, 3787-3795 | item 2, translation | cockpit | near parallax |
| 5 | world rows, temporal_pass.cpp:6599-6648, 6650-6680, 2556-2717 | scene-block writes at float 932; continuity, bound block | cockpit | 100+ writes a frame, mostly auxiliary |
| 6 | follow score, temporal_math.h:44-53; temporal_pass.cpp:3665-3674 | rows' turn vs the head's | unbound, or under 8 draws | menu backdrops, station chains (09-04, 09-08) |
| 7 | camera gate, temporal_math.h:355-444; temporal_pass.cpp:3686-3707 | 3 deg, unturned rows, 50 m | cockpit | another camera; a parked zero (4,674-frame stays); origin jumps |
| 8 | floor, split, temporal_math.h:42; temporal_pass.cpp:3809-3819; shader :681-733 | 8 draws; 10 m | cockpit | menu backdrops; ship geometry |
| 9 | FSS interface, temporal_pass.cpp:3878-3890; shader :715 | head for UI pixels | scanner up | a panning camera, a still panel |
| 10 | screen motion, screen_motion.cpp:260-315, 387-506; shader :670-680 | naming draw's b1, depth | on foot, named | (exact; GUI panel-fixed) |
| 11 | engine records, engine_velocity.cpp:2453-2490, 2521-2576; shader :736 | poses, draws' b1 now/last | cockpit; on-foot source | (exact per object) |
| 12 | world route, vr_world_route.cpp:204-336, 482 | item 11's rows | on foot, key auto | (one resolve) |
| 13 | flat, flat_runtime.cpp:2770-2794; flat_camera_inject.cpp:404-460, 732; flat_camera_phase.h:45-60, 236-246 | b1[270..275], kind-3 injector, rowsJitter | flat | (C3) |
| 14 | transition flash, glitch_frame.cpp:170-219; native_temporal.cpp:479-491 | magnitude floor 250 | VR | composition read as a jump |

Explorer Cam's game read is gone (explorer-cam.md); external views go
through items 5-8. Items 5-8 and 14 exist because EDVR reads rows from
anonymous buffer writes and must guess the camera; the census's detour
(vr_camera_census.cpp:95-170) names it by kind, caller and place.

## 3. The world camera, view by view

- **Cockpit:** kind-5 eye cameras, known by kind at the refresh (exact over
  4.63 million calls) and by joining an eye scene draw's b1 to a call's
  rows. Correct with depth; ship geometry takes engine records where
  certified, else the split; holograms and HUD keep their paths. Unknown:
  calls and call sites per frame. Cockpit maps: presumably the same (H3).
- **On-foot maps:** some camera drawing into the panel. The 60 s census
  window over both gaps (16:23:22) implies about 30 kind-3 calls a map frame
  (28-34; the world has 73-79), no kind 1, kind 5 still 6.0: an estimate.
  Unknown: which camera, whether stars, lines and labels write depth,
  whether b1 holds a scene block.
- **On foot:** the kind-3 world camera (54 refreshes a frame, before the
  tone), named by screen motion; the helmet HUD is GUI, masked panel-fixed;
  the weapon has its own camera.
- **FSS, DSS:** a panning scanner camera, a head-locked interface, a mono
  zoomed body (fss-scanner.md). Kinds unknown.
- **Explorer Cam, external views:** stereo, presumably eye cameras.
- **Main menu, loading:** 2D screens, layer; the VR menu backdrop's camera
  ignores the head (item 6), kind unknown.
- **Flat:** kind 3 through the injector, already the world camera.

## 4. A panel that is not the world: two options

**(i) The world camera decides the gate.** On foot the panel stays in the
eye route only while screen motion names a source; otherwise the layer takes
it. A pure step in ui_layer_math.h: 2 named frames hold, 3 unnamed release (3
is the world route's grace, so both let go on one boundary). With the key on
and screen motion live it alone decides; otherwise the gate is today's
`byJournal || byDepth` (ui_layer.cpp:2118-2186). It needs recognition of the
composite whoever takes it (today only the route's re-issue,
vscreen.cpp:4735), or naming stops two frames after a take
(screen_motion.cpp:261) and the gate never holds again; the last completed
frame's verdict, whichever boundary runs first (ui_layer.cpp:4260); and a
release line with the reason.

Handover. Opening a map: 3 frames through DLSS as a cut, then the layer
composites the map after the upscaler; the route, if on, releases on the
same boundary and the eye shift returns. Closing: the world is named on the
first or second frame, the gate holds two later, the route warms 8 and owns.
The eye history under a taken panel is black; DLSS clamps it on return, as at
every disembark (16:20:05.76-.79); a visible dark fade would call for a reset
at the hold edge (a cut, not the fix). Side effects: on-foot terminal menus
go to the layer too; thin map lines may shimmer under head motion, as the
layer samples the 5040-wide texture at mip 0 (cure: the route's mipped
screen, vr_world_mips).

**(ii) The map's own camera.** A map naming rule in screen motion: the map's
scene draws into the screen-sized depth name its camera and depth, and the
panel keeps DLSS with true motion. It needs the census to name the camera,
draw hashes and b1 layout, and depth on the moving content. Depthless stars
cannot be reprojected under pan or zoom by any camera, and labels drawn as
GUI would get panel-fixed motion while their stars move: a new smear.

**Recommendation: (i).** It removes the false claim at its source (the
journal, not a camera, declared the panel the world), hands DLSS nothing it
has no vectors for, needs no census, and is a predicate plus one call site.
(ii) needs depth on moving stars and labels, for map AA nobody has asked for.

## 5. What it replaces, and the risks

- Chooser and follow score (5-6): the view's camera is the kind-5 call whose
  composed rows equal the eye scene draw's b1; kinds 0, 1 and other kind-3
  objects never are. The score becomes a logged assertion.
- Another and Parked (7): a parked camera is one not refreshed this frame;
  the refresh count says so, nothing is carried. The 50 m jump stays.
- Rotation alone (3): only frames with neither camera nor depth; (i) takes
  the unnamed panels away from it.
- Eye jitter: kind-5 projections carry EDVR's shift, the view axes (camera
  +0x20, float-932 rows) do not; composed-row consumers remove it, as the
  engine and hologram motion already do (temporal_shader_source.h:519-575).
- Stage 2's phase: the route removes it from the kind-3 rows
  (flat_mono_shader_source.h:48-51, 92-95). Cautions: in the route's grace
  frames the eye route serves a jittered world whose screen-motion rows keep
  the phase (under half a source pixel, 3 frames); a map frame refreshes
  about 30 kind-3 calls, so shut the window after an unnamed frame.

## 6. Phases

**Phase 0: census episodes.** Key `advanced.vr_camera_census` (existing);
key off unchanged (nothing installed, allocated or logged; operator-new rig).
Today it records the session's first three on-foot frames only and never an
aboard frame (vr_camera_census_core.h:240-244, 522-531): it cannot see the
maps or the cockpit. Extend: (a) an episode, one sequence and one join 30
frames after a journal on-foot flip, a naming flip held 3 frames, a GuiFocus
change or key-on, aboard frames included, 10 a session (the call-line cap
grows to fit); (b) the join: at that frame's first draw into a screen-sized
or eye-sized depth, per eye, VS/PS, depth write, b1 size, b1 rows 270..273
read back and matched to the calls, and the pass's chosen rows matched to the
calls' view axes; (c) on-foot named and unnamed run lengths (1, 2, 3, 4-8,
9-30, 31-89, 90+) in the 5 s line; (d) the detour's own CPU; (e)
`--camera-census` per episode. Rigs: vr_camera_census_test and the reader's
self-test, both in build.bat's gate.
Flight (Frontier, the environment above; live `edvr.ini` by the Edit tool):
`advanced.vr_camera_census = on`; `fix.panel_curvature = 0` (the join reads
the eye composite, which a curved screen replaces);
`experimental.temporal_aa_on_foot_world = off` (today's eye route is the
reference). Note the clock at each open and close: cockpit 30 s; its galaxy
map (5 s still, 10 s drag and rotate, 5 s zoom), close; its system map the
same; disembark; 30 s walking; the on-foot galaxy and system maps the same,
10 s of world between; a terminal menu 10 s; board. Census off after. Read,
each with
`--target frontier --expect-build HEAD`: `--version`, `--camera-census`,
`--grep "vr camera census|world screen|screen motion:|auxiliary camera"`.
Answers: H1; H2 (the longest unnamed run in the world stays under 3, else
the release count goes above it); H3; the map's camera, depth and GUI pairs
for (ii); the cockpit's kind-5 calls and where the chooser strays; the
detour's CPU.

**Phase 1: the panel is the world only when the world camera names it**
(option (i)). Key `experimental.on_foot_maps_sharp = off|on`, default off,
on the in-VR Experimental page. Key off: the gate equals a frozen copy of
today's for every input (ui_quality_test, exhaustive), recognition unchanged
(source pin), no new line. Rigs: ui_quality_test (the step; flight 1's runs
replayed, 13,044 named, 1,597 unnamed, 98 named, 929 unnamed, then named:
a hold and four flips), screen_motion_test (recognition under a take),
vr_world_route_test (release with the gate); mutants. Flight: Phase 0's
on-foot legs, key toggled off then on, one map with the world route on.
PASS: a release within 3 frames of each map or menu opening, a hold within 3
of closing, no flip in the world, the map sharp under a drag, HUD intact,
route and gate letting go together. STOP: a release in the world, flapping,
a black eye, a lasting dark fade. WATCH: map lines shimmering (mips).

**Phase 2: the cockpit's rows from its eye cameras.** Key
`experimental.temporal_aa_game_camera = off|auto`. The detour, a quiet
observer in VR, publishes each eye's kind-5 view axes once a frame (eye by
the join or the off-centre sign); the world path takes them instead of the
chooser; score and gate become logged assertions. Key off: the chooser byte
for byte (eye-run traces replayed). Rigs: replay of Phase 0's episodes and
traces, the join convention, mutants. Flight: camera-rows-carry's roll and
turn protocol, a station approach, a supercruise arrival; cockpit maps and
FSS as regressions. PASS: no carried frame; docked, rows turn with the head.

**Phase 3: the rest,** each on its census: FSS and DSS (scanner camera for
the scene, head for the interface), external views, the menu backdrop, and
(ii) if wanted.

## 7. Open questions for Sean

1. Phase 1 as (i) or (ii)? Recommend (i), for section 4's reasons.
2. Phase 0 and 1 in one build and flight, Phase 1's key live in a second
   on-foot leg? Recommend yes: Phase 1 does not depend on the census.
3. On-foot terminal and station menus to the layer too (sharp, no temporal
   AA)? Recommend yes: the gate's comment sends static screens there.
4. Later, skip the eye upscaler under a layer-held non-world panel (a black
   eye, 2.5 ms a pair for nothing)? Recommend yes, as its own change.
5. The detour in VR by default once Phase 2 flies (observe-only; 4.63
   million calls without a fault)? Recommend yes, behind its key, once
   Phase 0 has measured its CPU.

Sean's answers, 2026-10-01: 1 (i). 2 yes, one build and one flight, Phase 1's
key live in its own on-foot leg. 3 broader than asked: any non-world panel,
menus included, under the same key. 4 yes, now: the eyes skip the upscaler
while the layer holds a non-world panel. 5 open (needs the detour's CPU,
which this build measures).

## 8. Phase 0 and Phase 1 as built (2026-10-01)

Branch `claude/on-foot-maps-sharp`, from origin/main `e35b0725`. Built, rigged
and mutated, NOT FLOWN.

**8.1 What Phase 1 does.** With `experimental.on_foot_maps_sharp = on`, the
layer's world-screen gate (ui_layer.cpp `onFootGateTick`, then `mapsGate`) is
decided by the world camera alone: a frame is named when a draw that reads the
world camera names the screen's source (screen_motion.cpp, the one place it
sets `g.sourceFrame`, which now tells the layer through
`uiLayerNoteScreenNamed`). Two named frames in a row hold the panel as the
world (it stays in the eye route); three unnamed release it (the layer takes
the composite, sharp, after the upscaler). Three is the route's grace
`kVrWorldGraceFrames`, asserted in vr_world_route_test, so the route and the
gate let go on one boundary. Today's journal-or-depth gate is still stepped
every frame, so a key flip is a carry, not a restart: switching in takes
today's verdict as its first state (no two-frame glitch in a held world), and
switching out hands the gate back at once. The naming is attributed to the
layer's own frame count, so it is right whichever boundary runs first (the
design's "whichever boundary runs first").

*The trap, closed.* A taken composite swallows screen_motion's per-eye call
(`screenMotionDraw` returns at `uiLayerRedirecting`), the only caller of the
recognition, and naming stops two frames after the last recognised composite:
the panel would never come back. `forwardWithVerdict` now recognises a taken
2D screen itself, right after the decision and before the curved-screen
substitution, behind `uiLayerMapsOn()`. screen_motion_test shows the naming
running out two frames into a taken screen without it and continuing with it.

*The door.* While the layer holds a non-world panel the eye skips the upscaler
through the layer-only door the route already uses: black base, the layer's
composite, RCAS after, no DLSS, motion prep or UI resolve, the eye's history
left alone (the next eye-route frame finds the continuity broken and resets,
so no dark fade). One predicate, `uiLayerDoorLayerOnly(eye, seq)`, answers for
both doors: the route's re-issued world first, then, with the gate on, a 2D
screen the layer TOOK in this sequence (`Eye::screenTakenSeq`, set only for a
real take, never the route's re-issue) while every draw the game made into an
eye-sized target this frame was taken (`vScreenEyeDrawsThisFrame` against the
layer's per-frame taken count; Submit comes before Present, so both are whole
at the door). Anything else drawn into the eye (a menu backdrop, a cockpit)
makes it `door-not-empty` and the eye keeps the upscaler, exactly as without
the key. That check is why this could ride the build: the log of 2026-09-30
shows on-foot eye draws at 2 a frame, the composites.

**8.2 Log lines** (all `on foot maps sharp`, none while the key is off; the
reader is `edvr_log.py --maps-sharp`, fixture `tools\maps_sharp_fixture.log`
held to the formatters by on_foot_maps_test P7):
- `ON at frame=` once per switch in, with the carried state and the journal;
  `OFF at frame=` (key went off / screen motion is not live / the layer's own
  not-live reason).
- `the layer TAKES the 2D screen at frame=N: no world camera named its source
  for 3 frames in a row (after W world frames, S s; the journal: ...)`.
- `the layer HANDS BACK the 2D screen at frame=N after P panel frames (T s; E
  eyes through the layer-only door, K kept the upscaler ...): a world camera
  named the screen's source for 2 frames in a row.`
- `experimental.on_foot_maps_sharp is on but ...: <why>` once per reason
  (layer not live, screen motion not live), and `the layer took the 2D screen
  for eye E ... the eye keeps the upscaler` (first three).
- every 5 s while the key is on, zeros included: `on foot maps sharp 5s: key=on
  5 s mode=naming|fallback gate=world|panel frames= named= unnamed= world-frames=
  panel-frames= holds= releases= screen-takes= recognised= door-layer-only=
  door-not-empty= not-live-frames=`. If the code never ran there is no such
  line; if the trap were alive the line shows `screen-takes>0 recognised=0`.
- The reader's verdict. STOP: a panel period under 10 frames (a flap in the
  world), a black eye (the sharpen door's own failure line, `native sharpen:
  LAYER-ONLY eye ... NO composite`, and only that), taken composites never
  recognised. WARN: an eye kept the upscaler, the door short of its eyes, the
  route and the gate letting go more than 50 ms apart, a layer-only decline,
  frames not decided by naming, a black luma `final` stage inside a panel period
  the journal calls on foot. A panel period also opens at an ON line whose gate
  starts as a panel (the arrival). The luma probe's `first black stage is ...`
  lines are not black-eye evidence (it prints one at every change, `none` when a
  black arrival gives way to a world); the first reader counted them and every
  real flight would have STOPPED, found when 8.6 was written against real logs,
  with the journal's `(a menu, or no file yet)` parentheses that also lost the
  arrival's ON and TAKES lines.

**8.3 Key-off contract.** The gate equals a frozen copy of today's expression
for every input: `uiMapsGateHeld` exhaustive in on_foot_maps_test R5, and the
real layer driven frame by frame over 400 scripted journal and depth inputs
against the same copy in ui_layer_world_test; the first test of `mapsGate` is
the key and it returns today's verdict before anything is read, stepped,
counted or logged (source pin P1a/b); the door answers the route's question
and one load; no line in the log; recognition's two call sites pinned
(ui_world_route_wiring_test, count 2, the new one behind `uiLayerMapsOn()`).
49 mutations of ui_maps_math.h are each caught by their own rule; the runtime
was also mutated by hand (the route's re-issue counted as a take is caught).

**8.4 The route.** What happens, frame by frame, with the route auto:
- *Map opens.* M0, M1: not named, not treated, the route's grace holds, the
  game's composite lands in the eye and the eye route serves it (the eye shift
  stays off while the route is Owned). At M2's boundary the third unnamed frame
  releases the gate and the route together (the layer's boundary runs first;
  the route's reason reads `on-foot-gate-lost`, not `frames-not-treated`).
  M3 on: the layer takes the composite, layer-only. The map goes through the
  upscaler as a cut for three frames, then sharp.
- *Map closes.* C0, C1: still the layer's: the world shows sharp without
  temporal filtering while the route, not watching (the gate is closed), waits.
  C1's boundary holds the gate. C2 on: the game's composite lands in the eye
  again, the eye route runs the upscaler with a reset history, and the route
  treats C2..C9 (Warming; the eye shift is on) and owns at C9's boundary.
  So the hand-back is to the eye route, not straight to the route: the
  route's warm-up needs eight treated frames whose eyes are the eye route's,
  by design (an upscaler needs about that many to stop reading new). Two layer
  frames plus eight warming frames, the same eye-route stretch every release
  has today. No eye-route frame can be skipped without changing the route.
  Replayed through both real machines in vr_world_route_test, flight 1's runs
  included: the route is never owned while the gate is open.

**8.5 `fix.panel_curvature` above 0.** The route needs 0 because its re-issue
repeats the game's screen draw and the curved substitution swallows it. The
take does not: with curvature on the decision is made with `substituted =
true`, the opaque composite passes it, and `panelCurveSubstitute` draws the
curved strip inside the layer's bracket, so a curved map is drawn sharp into
the layer. The recognition sits before that branch, so a curved take still
keeps the naming alive (the curved branch returns before the tail where the
route's call is). The world frames stay the eye route's and the route stays
off with its own line. Code reading only; the flight keeps 0 (the census join
reads the eye composite, which a curved screen replaces).
SUPERSEDED for the route, 2026-10-01 (branch `claude/vr-route-curvature`):
the route no longer needs 0. Its re-issue repeats the curve substitution's
strip, so a curved screen is re-issued, not refused; the take is as written
here (design-flat-temporal-aa-2026-09-23.md section 82, "The curved route",
BUILT, NOT FLOWN; the flight plan there has the Phase 1 take with curvature as
its leg E).

**8.6 The arrival spell** (coordinator's correction, 2026-10-01: the declines
in `edvr_gfx_20261001_060011.log`, v0.18.0-rc.5-19-g02c1c456, are not a Cinema
property but the first seconds of on-foot play after a load; `..._074129.log`,
v0.18.0-rc.5-22-g85119ce9, shows them again). Neither build has Phase 1, so
both show today's gate.
- *What the logs show.* After the load the route declines every frame
  (`depth-not-screen-motion-source`, the tone pass running on all of them), the
  layer's world-screen gate is open (the journal: no Flags2 in Status.json; the
  screen's depth 4 draws a frame; held 0 of 2,587 and 2,698 frames in 060011's
  30 s windows ending 06:01:12 and 06:01:42; 5,174 and 5,396 2D screen draws
  asked in them, 0 left in the picture, so the layer took every one and the eye
  route served none), the eyes hold only the two composites (2 eye draws a
  frame) and the luma probe reads 100% black at the game, dlss_out and final
  stages at every 2 s sample (060011 to 06:01:59, 074129 07:42:51-07:43:19).
  The final stage is read after the layer's composite (native_sharpen.cpp), so
  the submitted eye is black with the layer's screen in it: nothing visible
  waits on a name, and the census agrees (6 kind-3 camera calls a frame in
  074129's last three spell windows against 118-140 once the world draws). 074129:
  first decline 07:42:53.317,
  `journal: LoadGame` 07:42:53.643, the world at 07:43:19.853 (26.5 s). 060011:
  06:00:58-06:02:00.107 (62 s). The pass ran on every eye throughout (the 30 s
  `gates` lines: "the pass treated 5,174 and 5,396 eyes" in the same two 060011
  windows): 074129's lean shader priced the upscaler at 2.69-2.95 ms a pair,
  prep 0.12 and UI resolve 0.20; 060011's diagnostic shader read `full` 0.00
  and prep 0.21.
- *The arrival.* The world's draws come at once: 12,049 draws a frame into a
  4032x2268 depth (060011), 10,261 into 3504x1971 (074129). Today's depth gate
  holds the screen as the world after two frames over 64 (06:02:00.107,
  07:43:19.853). The first naming (the on-foot source slot target is made)
  follows 126 ms and 152 ms later, the route jitters at +234 and +237 ms and
  owns at +304 and +301 ms (eight treated frames). That wait for the first
  naming fits the trap (8.1): a taken screen swallows the recognition, naming
  needs one within 2 frames, so it restarts only once the depth gate has put
  the composite back in the eye route.
- *Key off:* today's, byte for byte (8.3): the journal-or-depth gate, the layer
  takes the empty screen, the pass runs on empty eyes, the depth gate hands
  back, the route owns 0.30 s later.
- *Key on, in the spell:* the gate is the naming. Nothing names, so the screen
  is a panel three unnamed frames after the world goes (today's gate lets go
  after 90 frames under 32 draws, about a second later) and stays one, the
  state today's gate is in for the rest of the spell. The layer composites the
  same empty screen, so what Sean sees does not change. The eyes hold nothing
  else (the door's emptiness test, 8.1), so the layer-only door skips the pass
  for every eye for the whole spell: upscaler, prep and UI resolve (about 3.1
  ms a pair in 074129) are not run and the composite costs 0.24-0.39, about
  2.8 ms a frame saved, of nothing visible. The recognition runs on every taken
  composite, so naming is armed. The 5 s lines read `gate=panel named=0
  unnamed=N recognised=screen-takes door-layer-only=2 x panel-frames`.
- *Key on, at the arrival:* the world's first frame names the screen at once
  (no wait for the eye route) and two named frames hand it back: expected
  within a few frames of the world, no later than today's depth hold
  (unmeasured: the trap hid the timing). The route then warms eight frames and
  owns, about 0.3 s as today. The door left the upscaler's history alone, so
  the first eye-route frame resets it, as the first treated frame does today.
- *The lines with the key on.* From launch the gate usually starts as a panel
  (the main menu), so there is no TAKES line: the ON line (`... starts as ...
  not the world (the journal: no Flags2 in Status.json (a menu, or no file
  yet))`) opens the period and the HANDS BACK line closes it. With the key
  turned on in a world, the load gives a TAKES line. `--maps-sharp` reports both
  as panel periods; its luma note is information there (black by content) and
  a WARN only when the journal called the period on foot.
- *Risks, stated.* (1) The hand-back waits for naming where today's waits for a
  draw count: a world whose draws arrive and are never named is the layer's
  (sharp, unfiltered) until something names it. Both logs name it 0.13-0.15 s
  after the hold even with the trap's delay, so with the key on it names no
  later. (2) Three unnamed frames after a hand-back release the screen again:
  074129's owned world had one two-frame gap (frames 12495-12496,
  `engine-views-unavailable`, after the source was re-made), one frame short of
  a release. (3) Today's spell runs the pass, so a DLSS feature first made
  then is made unseen; with the key on, a session whose first eye-route frame
  is the arrival makes it (58-105 ms an eye in 074129 at 07:43:39) at the
  hand-back. A session that has shown a menu or a cockpit has run the pass.
  The leg looks for it: LONG FRAME lines in the second after the hand-back.
No fix for the spell is built: there is nothing in it to name.

**8.7 Phase 0 as built** (vr_camera_census_core.h, vr_camera_census.cpp; no
new key, it rides `advanced.vr_camera_census`). An episode is ONE frame
sampled 30 frames after a trigger (the journal's on-foot flip, a naming flip
held 3 frames, a GuiFocus change, key on), whatever the frame is, aboard
included, at most 10 a session (the design's protocol has about 14
transitions: raise `kVrCensusMaxEpisodes` if it truncates, the caps follow).
Per episode: a header (`episode frame= n=k/10 trigger= foot= gui= named=
calls= recorded= printed= kinds= callers=`), every refresh call of the frame
(120 printed, the matched and kind-5 first), the pass's chosen rows matched to
the calls' view axes (`pass-rows ... axes-match= how=identity|transpose`) and
the join: at the frame's first draw into each kind of screen-sized or eye-sized
depth, per eye where known, the VS and PS, the depth write, b1's size and rows
270..273 read back and matched to the calls (`join ep= sig= depth=screen|eye
eye= size= draw= draws= vs= ps= dw= b1= bytes= rows=read|skip match=`,
`join-rows`, `join-draws seen= relevant=`). The 5 s window gains three
companion lines (the 5 s line was full at 400 characters): `episodes windows=
taken=/10 triggers= skipped= state=`, `runs ... named=1:n,2:n,3:n,4-8:n,9-30:n,
31-89:n,90+:n unnamed=... longest=`, `detour ... est-ms-frame= obs-pre-us=
obs-post-us=` (1 call in 16 timed). The reader's `--camera-census` reports each
episode, the naming runs (H2) and the detour's CPU. Cost: key off nothing (the
per-draw hook is a null pointer, set only for a sampled frame); key on about
100-150 ns a call in the sampled frame only, 4 synchronous readbacks an episode
(one frame may drop in the sampled frame). Unflown: the real join (the depth
probe's eye pair, the shadow resolver) and Status.json's GuiFocus on a real
install; the rigs stand WARP in. 70 mutants of the core header, 69 caught (one
equivalent).

**8.8 Cost and saving of Phase 1.** CPU, measured on the real layer: the gate's
step and the combine 2.5 ns a frame, the boundary +1.2 ns (+2.5 ns unnamed),
four door questions +1.6 ns, the naming told one store, the recognition two
hash loads per taken composite (about 10 ns): under 20 ns a frame in all, and
nothing measurable key off. GPU, from flight 1's own lines (4032x3898 a eye,
preset K, Pimax Crystal Super, 90 Hz): a map frame through the eye route costs
the upscaler 2.5 ms a pair, motion prep 0.12, UI resolve 0.2, about 2.9 ms; the
layer's composite adds 0.24-0.39; net about 2.5-2.7 ms a frame saved, a fifth
of the 11.1 ms budget, plus about 0.15 ms of CPU (the pass's 0.16 ms against
0.013). Flight 1's two map stretches were 1,597 and 929 frames.

**8.9 Unsure, and proposals.** (a) RCAS runs after the composite on a
layer-only eye, the door's existing order, so it sharpens menu text too (the
ordinary taken-UI path leaves text unsharpened); watch for ringing, and if it
shows the cure is RCAS off for a non-world panel eye. (b) The key name: it
covers menus and any non-world panel, so `experimental.on_foot_panels_sharp`
says what the user gets; changing it is one edit in ui_layer.cpp, edvr.ini and
the config rig. (c) The arrival (8.6): the hand-back waits for naming where
today's gate waits for a draw count. (d) A frame where the
layer is not armed (after a withheld eye) leaves the game's composite in the
eye and the upscaler runs: one frame, as today.

**8.10 The first flight (2026-10-01).** `edvr_gfx_20261001_082459.log`, Frontier,
v0.18.0-rc.5-37-gdf9172db. Sean: "it all looks perfect to my eye". Launched with
the route off (vScreen took the legacy 5040), the maps key on, the route auto
live. The reader of that build read WARN (0 STOP, 7 WARN). The log says:
- *Period 1* (ON 08:31:23.144, TAKES 08:31:23.166): a map. 1,930 frames, all 3,860
  eyes through the layer-only door, none kept the upscaler, HANDS BACK
  08:31:44.796, and the route owned the world 163 ms later (the eight warming
  frames of 8.4). The door worked for a screen the route had owned before the key.
- *Between* (08:32:35): the route declined three frames (`engine-views-unavailable`),
  released by `frames-not-treated` and owned again 207 ms later, while the gate held
  the world throughout: the route's declines and the gate are independent, as 8.4 says.
- *Period 2* (TAKES and the route's RELEASED `on-foot-gate-lost`, both 08:32:55.975):
  not a map. Sean boarded his ship. The 30 s line at 08:33:00 says `the journal:
  aboard` and lists the cockpit's families (holo panels, flight HUD);
  `settlement detail: no longer on foot (Status.json)` at 08:32:57.615 (1.6 s after the
  take: the journal lags the camera); `engine motion: on-foot source slot target
  released ... no on-foot source for 120 frames` at 08:32:57.645; the transition-flash
  lines name a change of reference frame. A cockpit draws no 2D screen composite, so
  every window after the take reads `screen-takes=0` and there was nothing for the
  door to skip. The route and the gate let go on one boundary, as designed.
- *The four black luma samples* (08:33:22.3-08:33:24.4, every stage, both eyes) are the
  game's exit fade: the first black stage is `game` (the game submitted black) and the
  shutdown totals follow at 08:33:25.110. The sharpen door's own totals for the whole
  session: `layer_only=14114, layer_only_black=0`.
- *The six flights after it* (same build; Frontier logs 084752, 085519, 085923, 090246,
  090933, 092026): stretches of the main menu (the journal: no Flags2) and of the ship
  (aboard) with the key on, and the layer took no 2D screen composite in any of them
  (`screen-takes=0` in every window: nothing for the gate to do). The reader reads each as
  WARN for "no panel period closed" and nothing else, except 085519: fix.temporal_aa was
  off for a stretch of it, and the OFF line's reason (`no temporal mode is on
  (fix.temporal_aa is off)`) has parentheses of its own, which the reader's pattern
  stopped at, so it lost the line as one it did not know. Three of the DLL's eight
  reasons are like that. Fixed: one level of nesting, all eight reasons through the OFF
  and not-live patterns in the self-test, held to the DLL's sources, and the
  unknown-line WARN quotes the line.
The door was right and the instruments were not, three ways. (1) The reader judged the
door against PANEL FRAMES (the gate's state) and not against taken composites, so a
cockpit read as six failed doors (the "46.1%"). (2) A window with panel frames and
`screen-takes=0` was ambiguous: nothing counted the composites the decision SAW, so
"nothing was drawn" and "drawn and refused" read alike. (3) The luma WARN took the
journal's reading at the TAKES (stale by 1.6 s) for a stretch that was a cockpit.
THE FIX (its own commit, `claude/vr-route-curvature`; no behaviour change):
the 5 s line gains `screen-draws=` (every 2D screen composite the layer's decision saw
while the key was on, taken or not); the reader judges the door against composites
taken, adds a WARN for composites drawn in a whole-panel window and not taken, notes a
period with no composite (`none was drawn: a cockpit, a load`) and no longer lets a
period the layer held no screen in raise the luma WARN; a log without the token (this
one) is read by takes alone and says it cannot tell the two apart. The real layer rig
(`ui_layer_world_test`, `testMapsTransitions`, two real 5 s windows) replays both
transitions: a map opened with the route owning (the composites go on, the layer takes
them, the door runs layer-only for both eyes) and closed, then the boarding (route
owning, naming stops, both let go on one boundary, no composite follows). The first
window is the whole run, checked token by token against what the rig drew and what the
layer did with it (draws = re-issued + taken, takes = taken, door = taken); the second
is the cockpit alone, the flight's shape: panel frames, draws 0, takes 0, door 0.
`on_foot_maps_test` holds the token (R9, four new mutants) and its one counting place
(P8, with controls); the reader's self-test has the boarding flight in both formats.
The reader now reads this flight as PASS (0 STOP, 0 WARN).
ruled out: "the layer-only door failed after the route's release on the gate", because
no composite was taken in those windows (nothing was drawn: a cockpit) and the sharpen
door counted 14,114 layer-only eyes and 0 black.

## 9. Flight plan (one build, one flight)

Live `edvr.ini` edited with the Edit tool (never a regex); all keys live. Set
before the flight and between legs as written:
```
[fix]          panel_curvature = 0   (ui_quality and temporal_aa as shipped)
[advanced]     vr_camera_census = on
[experimental] temporal_aa_on_foot_world = off | auto   (per leg)
               on_foot_maps_sharp = off | on             (per leg, toggled live)
```
Screen width. `fix.vscreen_res_width = auto` is decided when the game launches
and does not change live (main's vscreen auto-fit, merged into this branch;
design-flat-temporal-aa-2026-09-23.md section 82). Launched with the route off,
as leg 1 sets it, the width is the legacy 5040 of flight 1. Launched with
`temporal_aa_on_foot_world = auto` and `fix.panel_curvature = 0` it is the
fitted width (3504 on Sean's rig until a session on foot has measured it), and
the mip-0 sampling in the WATCH line below is then of a smaller screen. Launch
once, in leg 1's state, and keep every leg in that session; the log's
`vScreen resolution:` line names the width and the rule.

Note the clock at every open and close. Frontier, the environment above.
1. *Cockpit* (census on, both keys off): 30 s; galaxy map (5 s still, 10 s drag
   and rotate, 5 s zoom), close; system map the same; disembark.
2. *On foot, route off, maps key off* (the reference): world 30 s; galaxy map
   dragged 10 s, close; 10 s world; system map the same; a terminal or station
   menu 10 s.
3. *On foot, route off, key toggled live:* world 30 s; key on in the world (no
   change); open the galaxy map: 5 s still, 10 s drag; close; 10 s world;
   system map the same with the key set off for 5 s in the middle and on again;
   the menu 10 s; world 30 s.
4. *On foot, route auto, key on:* world 30 s (the route owns); galaxy map 15 s,
   close, 10 s world, system map 15 s, close; the menu 10 s; world 30 s. Then
   one map with the key off as the reference for the hand-off.
5. *Arrival, key off then on* (in the mode he flies; both modes if there is
   time): key off, to the main menu, Continue, 30 s of world; to the main menu
   again, key on in the ini, Continue, 60 s of world. If the session's first
   load goes straight to on-foot play, that load is the key-off half (leg 1's
   state): note its clock and skip it here. Note the clock at the load, at the
   world's first frame and when the route owns. This leg is for 8.6.
6. Board; a cockpit map (the stereo maps, a regression check); census off.
Read, each with `--target frontier --expect-build HEAD`: `--version`,
`--maps-sharp`, `--camera-census`, `--grep "world screen|screen motion:|vr
world route:|luma probe|journal: LoadGame|engine motion: on-foot source|temporal
aa price|LONG FRAME"`.
- PASS: a TAKES line within 3 frames of each map and menu opening, a HANDS
  BACK within 2 named frames of closing, no TAKES in a stretch of world, the map
  sharp under a drag (look), the HUD intact, `recognised` about `screen-takes`,
  `door-layer-only` about `screen-takes` and `screen-draws` equal to it in a
  whole-panel window (a cockpit period has neither: `screen-draws=0`), route and
  gate released on one boundary and the route owning again 8 frames after a
  hand-back, no luma
  black stage after a hand-back, the reader's verdict PASS. The census answers
  H1 (maps and menus unnamed), H2 (the longest unnamed run in the world under
  3), H3 and the detour's CPU.
- PASS (arrival, key on): the screen black through the spell as with the key
  off, no flash; the period (the ON line's, or a TAKES line's) closed by a
  HANDS BACK within 0.2 s of the world's first frame (the `engine motion:
  on-foot source slot target created` line; key off: 0.13-0.15 s after the
  depth hold) and the route owning within 0.3 s of it (key off: 0.15-0.18 s
  after that line); no TAKES in the 10 s after; `door-layer-only` about
  `screen-takes` through the spell (8.10), and the `temporal aa price` lines in it
  showing no upscaler (key off: `full` 2.7-2.95 ms a pair); no LONG FRAME in
  the second after the hand-back that the key-off arrival lacks.
- STOP: a TAKES in the world, a panel period under 10 frames, a black eye, a
  lasting dark fade after a hand-back, taken composites with `recognised=0`;
  at the arrival, a TAKES within 10 s of the hand-back, a hand-back more than
  1 s after the world's first frame, a flash or a dark frame at the hand-back.
- WATCH: map lines shimmering (the layer samples the screen at mip 0, 5040 wide
  in leg 1's launch state: the cure is the route's mipped screen), ringing on
  menu text (8.9a), `door-not-empty` above 0, a TAKES in a stretch of world in
  any mode (a world that does not name).

## 10. The key ships on (2026-10-01)

Sean approved the default flip on 2026-10-01 after the key and its behaviour were
quoted to him (branch `claude/key-cleanup-defaults`, BUILT, NOT FLOWN):
`experimental.on_foot_maps_sharp` is on by default, in the shipped `edvr.ini` and
in the code's fallback for an ini with no line (`uiLayerConfigure`;
`tools\config_test` holds the two to one answer, with a control that flips the
fallback). The key stays for one release candidate as the way back; off is today's
gate for every input, pinned as before. The gate still needs the UI layer and
screen motion live (`fix.ui_quality` on, a temporal mode on), so an install with
`fix.temporal_aa = off`, the shipped default, is unchanged and says once in the
log that the key is on but the layer is not live. An existing ini: a line that
still says what the previous version shipped (off), with the installer's base
copy kept, moves to on and the report says so; a hand-installed file with no base
copy keeps its off, and so does a value somebody chose (`installer_test`
`testChangedDefaultsOn`). The VR world route's key flipped to auto in the same
commit (design-flat-temporal-aa-2026-09-23.md, section 82, "The cleanup").
