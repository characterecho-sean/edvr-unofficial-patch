# The Full System Scanner in VR

## Status

*Rewritten 2026-10-15 at the close of the arc. The block as it stood before is kept in
"Status detail (before the 2026-10-15 close)"; the one before that in "Status detail
(moved out of Status 2026-09-29)". The journal below is the whole history.*

- **State: SOLVED. The confirmation flight of the cleanup branch is pending.**
  The Full System Scanner's zoom showed black squares in the LEFT eye only, a
  stock-game bug (reproduced clean). It is fixed by one small replayed draw
  (`fss_reveal*`, VR only, no key). The ~48-round hunt, the instruments of
  v2-v11 and the old fix are in the journal.
- **Cause (flight d7769063):** the reveal composite (VS 953C8123 / PS A473A867)
  shows each unrevealed cell's coarse input t1, a 63x63 downsample of the ring
  layer (VS DEF19B03 / PS 831DF02E). The left eye's pass draws that downsample
  between the ring's two draw groups, so the left t1 lacks the front arc and its
  unrevealed cells are black. A second downsample, from the finished layer, runs
  before the right composite, so the right eye shows a blurry ring. Flat has one
  eye and the left's order: the black cells there are the game's scan art.
- **Fix:** at the first reveal composite of a frame, run the frame's first
  downsample again from the finished layer, with the state it was issued with
  (constant buffers copied at the draw), and put the context back exactly. It
  matches only when the downsample's render target is the composite's t1. Both
  eyes then show the right eye's look. Flown on the test branch (build T,
  window 5): t1 never differed across 20 eye pairs, the replay fired in 17
  frames, 0 failed, 8 of 8 state read-backs equal.
- **Removed 2026-10-15 (Sean approved):** `fix.fss_eye_sync` (shipped `on`) and
  everything behind it: the arrival fill (heal), mirror, the composite input
  freeze (sync), the constants snapshot (steady), tiles; the native-runtime
  heal path (`native_fss`, the `edvrFssHealLeft` and `edvrAcquireNativeFss`
  exports, the host's deferred-eye logic); `advanced.census_fss_jump`; the
  arrival window, the zoom-press watchers and the FSS pacing log. All the
  v2-v11 instruments stay on `claude/fss-tile-lockstep`. Full list: the closing entry.
- **Kept:** `fix.fss_res` (opt-in full-resolution body), `fix.fss_panel_distance`,
  the FSS mode latch, the chrome tracker, the panel rect and its scenery skip
  (the rect's only reader was the heal; the skip is shipped behaviour and was
  left alone), and docs/frontier-fss-bug-report.md.
- **Confirmation flight:** the cleanup branch installed as is; `fix.fss_res` as
  you fly it. Zoom the scanner on a ringed planet. Expect no black squares in
  either eye, `fss reveal fix: armed` at startup and one `fss reveal fix: scanner
  session` line per scanner visit ending "the replay fired" (replays > 0, the
  state read-back all equal). A session saying "THE REPLAY NEVER FIRED" is not a pass.
- **Open:**
  - Re-verify with the OpenXR Toolkit ON (the old fix was proven Toolkit-off).
  - `fix.fss_res` stays opt-in; a default-on ship is a release-train call.
  - The flash detector withholds frames at every fresh FSS zoom (own arc).
  - Candidates for a later cleanup, not done: the panel rect (publishes to no
    reader; only its scenery skip is live), the dead shared-memory slots in
    frame_flag, the world-jump latch (no reader since the arrival census went).
- **Environment:** reproduces under OpenComposite and native SteamVR, two
  headsets, eye 4340x4284 and 2016x2016 at the flights that decided it. The
  fix is a D3D11 draw in the game process: it does not depend on the VR runtime.
- **Detail:** "Finding 1", "Finding 2", "The fix: `fix.fss_res`"; the hunt from
  "Round four" to the v12 entries at the end; "Method notes" is process
  lessons, read first. Vendor report: frontier-fss-bug-report.md.

## Status detail (before the 2026-10-15 close)

*Verbatim from the Status block as it stood on the instrument branch at flight d7769063.*

*Written 2026-09-15 from the entries of 2026-08-24 to 2026-08-27; the
2026-09-16 items are from a log read, not a journal entry. Restates the
journal below; update it whenever this doc changes. The status as it stood
before 2026-09-29 is kept in "Status detail".*

- **Retired 2026-09-29:** the theater (`experimental.fss_theater`, 6ecbd241;
  the latch's tick is now `fss_mode_latch`, 73b31d3f; latch, `fss_res`,
  `fss_eye_sync` and the rest stay) and the black-square-hunt instruments
  (`fix.fss_scan`, `advanced.fss_scan_level`, `experimental.fss_ring_feed`,
  `advanced.fss_composite_probe`; c61c43af, code at `c61c43af^`;
  `fss_eye_sync` superseded all four). Full lists in "Status detail".
- **State:** SOLVED and shipped, field-verified 2026-08-27. Findings 1-2
  (zoomed body mono, half eye resolution) are addressed by the opt-in
  `fix.fss_res`. The "black squares" hunt ran ~48 rounds to round 33's
  finding: for ~10 frames at each zoom's arrival, the primary (left) eye's
  submitted image carries not-yet-resolved tiles the right does not.
  Shipped and on by default: `fix.fss_eye_heal = 1` + `fix.fss_reveal_sync
  = on`. Bug is ring-only.
- **Fixed 2026-09-16 (Quest 3 / VirtualDesktopXR, native path, DLSS on),
  all flown;** receipts in "Status detail" and the journal entries named there:
  - Body double ghost while zooming with `fix.fss_eye_sync = on` (fill's
    donor was the previous frame's right eye): same-frame donor, 10705da /
    main e08e899, flight 3 "looks good".
  - FSS UI shimmer under DLSS, in steps: interface on the head's path
    (3799899 / 36a9fa1, 18:20); scene-draw floor 8 (79b1879 / cc10e79,
    18:40); tracker learn (b43e3ce, 19:00, idle, WRONG); graph under the
    alpha floor (653671d, 19:40, CONFIRMED); chrome depth down to one alpha
    step (3277ba6, on main, 20:26). Sean: "while head still it was crisp".
- **Open:**
  - Re-verify the healed pair with the OpenXR Toolkit ON (proven Toolkit-off).
  - `fix.fss_res` stays opt-in; a default-on ship is a release-train call.
  - The flash detector withholds frames at every fresh FSS zoom (own arc).
  - The census still misses `GenerateMips`, `ClearUnorderedAccessView*` and
    command-list contents.
  - Sky ~+0.4 px/frame more than the camera rows predict under a head turn
    (dump 202857, not 194158): unexplained, small. "The screen follows the
    head" is withdrawn (194158's label shortfall not reproduced).
- **Ruled out:** eleven items with reasons in "Status detail"; the
  refutations (rounds two to fourteen, 31, 33, Method notes 3-4) are in
  the journal chapters of those names, the ghost ones in "2026-09-16: the
  Quest 3 ghost under the native path".
- **ROOT CAUSE (flight d7769063, v11):** the reveal's coarse input t1 (a 63x63
  downsample of the layer) is drawn in the left pass BETWEEN the ring's two
  draw groups, so it lacks the near arc; it is drawn again before the right
  composite. Unrevealed cells show t1: black in the left only. Fix v12 (built,
  not flown; VR only, no key): replay the first downsample from the finished
  layer before the first composite. Read `reveal replay`, t1 in `composite diff`.
- **Environment:** The OpenXR Toolkit's own upscaler (`E861`/`B742`)
  confounded many rounds; the fix is proven Toolkit-OFF only. Reproduces
  under OpenComposite and native SteamVR (also via Steam Link), two
  headsets, eye 4340x4284, 16x16-tile (272x268) grid; the body layer
  defaults to half that and `fix.fss_res` doubles it.
- **Detail:** "Finding 1", "Finding 2", "The fix: `fix.fss_res`" (mono,
  half-res). Hunt: "Round four" to "Rounds forty-five through forty-eight";
  decisive "Round thirty-three: the arrival window, measured"; shipped
  state "The ring, and the shipping state"; "Method notes" is process
  lessons, read first. Companion: frontier-fss-bug-report.md.

## Status detail (moved out of Status 2026-09-29)

*Verbatim from the Status block as it stood before 2026-09-29; the
Status block above summarises it.*

- **State: the theater retired 2026-09-29 (code removed, 6ecbd241).**
  `experimental.fss_theater`, fss_theater.cpp and the export `edvrFssTheater`
  are gone: the theater could only run from a door call the deleted openvr half
  made. The FSS mode latch, `fss_panel_rect`, `fss_eye_sync`, `fss_res`,
  `fss_panel`, `fss_dump` and `fss_reveal` stay; the latch's frame tick, once
  `fss_theater`, is `fss_mode_latch` (73b31d3f).
- **State: the black-square-hunt instruments retired 2026-09-29 (code removed,
  c61c43af).** `fix.fss_scan`, `advanced.fss_scan_level`,
  `experimental.fss_ring_feed` and `advanced.fss_composite_probe` are gone with
  fss_scan, fss_probe and fss_ring (1,608 lines). `fss_eye_sync` (heal and
  lockstep) superseded all four. The entries below that flew them are history;
  the code is at `c61c43af^`.
- **State:** SOLVED and shipped, field-verified 2026-08-27. Findings
  1-2 (the zoomed body renders mono, at half eye resolution) are
  addressed by the opt-in `fix.fss_res`. The "black squares" hunt ran
  ~48 rounds to round 33's finding: for ~10 frames at each zoom's
  arrival, the primary (left) eye's submitted image carries
  not-yet-resolved tiles the right does not. Shipped and on by
  default: `fix.fss_eye_heal = 1` + `fix.fss_reveal_sync = on`. Bug is
  ring-only.
- **Open:**
  - 2026-09-16, Quest 3 / VirtualDesktopXR, native path, DLSS on: a
    slight double ghost on the body while zooming in with
    `fix.fss_eye_sync = on`. Flight 1 (heal toggled live) put it in the
    heal half; flight 2 on build 0522215 (heal on the DLSS output, no
    temporal skip) fixed the blur while moving but not the ghost.
    Cause, from eye dump 154720: the fill's donor was the PREVIOUS
    frame's right eye (the left submits first) and the view moves 4-20
    px per frame while zooming, so every hard-black pixel beside the
    limb took a displaced copy of it. Journal entry "2026-09-16: the
    Quest 3 ghost under the native path". FIXED by the same-frame donor
    (10705da, merged to main as e08e899): flight 3 "looks good",
    receipts `first_healed_eye=0,...,donor=pair`, `fss_healed=4009/0,
    fss_deferred=4009`, `native_temporal_summary left=19776
    right=19776`, `donor_refusals=0`.
  - 2026-09-16, Quest 3, DLSS: the FSS's own UI (the bottom bar, its
    text, the signal markers) shimmers on the initial screen while
    panning. MEASURED from eye dump 170752, not fixed: the raw frame
    draws the UI crisp and static in the eye (0 px/frame), DLSS returns
    it doubled BEFORE the UI resolve touches it (`DlssBeforeUi` = `L0`
    at the marker), and the vector DLSS was given at the UI pixels is
    the scene camera's (-3.8 px at the marker, -5.5 px on the bar) while
    the head was still (`headR` 0.01 deg against `cameraR` 0.23 deg that
    frame, 2.1 and 1.5 deg on the next). In the FSS the camera EDVR
    reads is the scanner's panning view, which the panel's UI does not
    follow; the "interface keeps the camera's path" rule
    (temporal_shader_source.h ~359) is right for a camera that is the
    head and wrong here. Journal entry "2026-09-16: the FSS UI under
    DLSS". FIXED for the interface (journal entry "2026-09-16: the
    scanner's interface on the head's path", 3799899 / main 36a9fa1):
    while the scanner's screen is up (the chrome tracker's stamp moved
    this frame), every interface-marked pixel takes the head's delta at
    whatever depth it reads, in both copies of the world/ship split;
    `probe.w` bit 128. FLOWN 2026-09-16 18:20 (`edvr_gfx_20260916_
    181837.log`, eye dump 182049): the note fired, the header carries
    0x91, the bar's pixels read +0.07/+0.13 px under a 0.2-1.6 deg/frame
    pan with the head at 0.02 deg, and the text is as crisp as the raw
    in all 16 treated crops. Sean: "still blurs with head still" -- and
    the same dump names the blur: the SKY (the scanner's grid, the
    signal dots, the stars) read +0.09 px too, because the world path
    stood down on 4 frames in 5: `sceneDraws` 49 against the 50-draw
    floor (50 on every 5th frame, `w 1`; the registration line said
    24.1% world-path pixels in the scanner against 94.8% twenty
    seconds later), and the treated crops show the grid and dots smeared
    against the raw. Journal entry "2026-09-16: the scanner's sky at 49
    draws". The floor is 8 now (`kTemporalSceneDrawFloor`, shared by the
    world path, the camera follow score and the menu depth; 79b1879 /
    main cc10e79) and FLEW 2026-09-16 18:40 (dump 184002): `w 1` on
    every frame at 35-36 draws, the sky at -7 px under the pan, the bar
    at -0.10 px, its text crisp. Sean: "still blurred" -- and the dump
    shows what: "FILTERED SPECTRAL ANALYSIS" and the graph's strokes,
    which are NOT in the interface mask (94% zero at the text), read no
    depth, and so take the world path's far-plane pan (-5.04 px) while
    they stand still in the raw (0-1 px): a 3-4 copy smear in every
    treated crop. Journal entry "2026-09-16: the spectral graph is not
    interface". The "unlearned stratum" cause (b43e3ce: the tracker
    hands the surface to `uiDepthLearnScannerChrome`) FLEW 2026-09-16
    19:00 (dump 190129) and was WRONG: no learn note, the strokes still
    unmasked at +2.06 px, and the mask in both dumps is a clean
    threshold on the composite's brightness (luma 80+ masked, under 60
    never) -- the alpha floor, not classification. CAUSE, as far as two
    dumps can say: both strata are learned and reissued, and the graph
    is drawn DIM, below `advanced.ui_depth_alpha` = 0.5, so
    `clip(a - floor)` drops its strokes and its label. Journal entry
    "2026-09-16: the graph is under the alpha floor". CONFIRMED by the
    instrument build's flight (653671d, 19:40, `edvr_gfx_20260916_
    193918.log`, dump 194158, ini at 0.004): `already learned when the
    tracker offered it (9 surfaces known)`, `Chrome0` (2479x1394)
    written, the label and every stroke in the mask (493/493 bright px
    at 129, MV = the head's path) and crisp in the treated crops; the
    chrome itself reads alpha 51-77 at the dim strokes, 166+ at the
    labels, zero everywhere else (97.7%; nothing translucent). Sean:
    "mostly fixed, it only slightly shimmers when both moving the panel
    via the controls and moving my head". FIXED (3277ba6, on main):
    the scanner's chrome writes depth down to one alpha step while the
    tracker holds its surface (`kChromeFloorSlot`,
    `samplesScannerChrome`); the general floor is untouched and the
    Frontier ini is back at `#ui_depth_alpha = 0.5`. FLOWN 2026-09-16
    20:26 (`edvr_gfx_20260916_202609.log`, dump 202857, ini at its
    defaults): both family lines say `the scanner's chrome; ...
    down to one alpha step`, the whole chrome is in the mask at the 0.5
    general floor (label, ticks, bar, cursor, brackets, markers, the
    right-hand texts) with the grid and stars out of it, and Sean:
    "while head still it was crisp". Journal entries "2026-09-16: the
    chrome at one alpha step" and "...: the one-step floor flown".
    Under a head turn: dump 194158 had the label moving 3-6% LESS than
    the head path (+0.23..+0.84 px/frame), which read as "the screen
    follows the head"; dump 202857 does NOT reproduce it -- the label
    and the bar's text follow the head path within +-0.3 px/frame (mean
    ~0) at 0.25-0.5 deg/frame, and the treated crops at 3x show them as
    crisp as the raw. What 202857 does show is the SKY moving ~+0.4
    px/frame MORE than the camera rows predict (the grid and stars
    slightly soft under the turn; not the UI). Nothing owed on the
    strokes; the sky's 0.4 px is unexplained and small.
  - Re-verify the healed pair with the OpenXR Toolkit ON (proven so
    far only Toolkit-off).
  - `fix.fss_res` stays opt-in; a default-on ship is a release-train
    call.
  - The flash detector withholds frames at every fresh FSS zoom (its
    own arc).
  - The census still misses `GenerateMips`, `ClearUnorderedAccessView*`
    and command-list contents.
- **Ruled out:**
  - "The eyes sweep the ring in opposite directions": died to a
    single-eye look.
  - A write landing between the two composite reads: zero writes found
    in a q-ordered capture (round two).
  - A per-eye fill-rate difference and a desynced per-eye resolve map
    (Method notes 3-4): a "working" fix changed nothing under a
    magenta control; the map proved an unrelated screen-space grid.
  - The composite draw itself (constants, depth, blend, discard):
    every probe (rounds three-four, seven-thirteen) came back null;
    the two draws are byte-equivalent.
  - "Harmless mono scan-resolve art, symmetric in both eyes" (round
    six): superseded by round seven's monocular re-test, a genuine
    per-eye split.
  - "The second eye's reveal is the smoothed, wrong one" (round 31):
    superseded by round 33 -- the PRIMARY eye carries the defect.
  - A carried/stale temporal accumulator: round fourteen's pair-sync
    was null; the left eye regenerates its squares every frame.
  - The Quest 3 ghost as an asymmetric-frustum error in the heal's
    shift (2026-09-16): the game renders the guard's symmetric lie and
    0 px is the infinity shift for that image (receipts above). "Not
    on the Pimax" is untested on the native path: no Pimax session in
    the last 60 Frontier logs reached the heal (`snapshots=0` in every
    one), and the only two native heals on record were both Quest 3
    rigs at 0 px (today, and 2026-09-14 via Steam Link).
  - The lockstep half lending a differently-jittered body layer: one
    `temporalJitter(frameCounter)` serves both eyes, so the lent
    texture carries the jitter the receiving eye rendered with. And
    the lockstep half at all: flight 1 (sync-only) had no ghost.
  - The healed eye's temporal skip as the ghost (2026-09-16): flight 2
    on 0522215 kept the DLSS frame in the healed eye
    (`native_temporal_summary left=12192 right=12192`, `skipped=10`,
    `native_fss,first_healed_eye=0,...,source=temporal`) and the ghost
    stayed. The skip was the blur while moving, which that build fixed.
  - "The stale donor is null on a zoom-in" (claimed 2026-09-16 before
    the dump was read): wrong. The fill reads the donor at the SAME
    pixel, and after a frame of motion that pixel holds content that
    has since moved; the simulated fill on dump 154720 paints the limb
    into the black gap, displaced by one frame's motion.
- **Next flight:** none owed for the ghost: flight 3 (2026-09-16
  17:05, `edvr_gfx_20260916_170514.log`, the pre-commit build of
  10705da, version string `0522215-dirty`) confirmed the same-frame
  donor and it is on main (e08e899). The head path (18:20) and the
  8-draw floor (18:40) both flew and did what they said; the learn
  (19:00) flew and was idle; the instrument build (19:40) flew and
  confirmed the floor; the one-step build (20:26) flew and did what it
  said (receipts under Open). None owed for the scanner's UI. Known
  and not fixed: the ~220 m remap under head sway (194158 says it is
  NOT what moves the label: a panel at 1 m would move MORE than the
  head's rotation); the "scanner is up" bit and the one-step floor
  also fire on the loading screen's panel (same composite family) --
  nothing translucent is in that surface under `loading_dim = screen`,
  where the scrim is withheld, and under a stock scrim the ship model
  beneath it takes interface depth for that dialog's duration; and the
  sky's ~+0.4 px/frame against the camera rows under a head turn in
  202857 (not seen in 194158). The Toolkit-ON confirmation under Open
  still stands.
- **Environment:** The OpenXR Toolkit's own upscaler (`E861`/`B742`)
  confounded many rounds until identified and excluded; the shipped
  fix is proven Toolkit-OFF only. Reproduces under OpenComposite and
  native SteamVR (also SteamVR-via-Steam-Link) on two headsets, at eye
  size 4340x4284 with a 16x16-tile (272x268) grid; the body layer
  defaults to half that and `fix.fss_res` doubles it.
- **Detail:** "Finding 1", "Finding 2" and "The fix: `fix.fss_res` --
  the body layer at full eye resolution" cover the mono/half-res bug.
  The black-squares hunt runs "Round four" through "Rounds forty-five
  through forty-eight"; the decisive measurement is "Round
  thirty-three: the arrival window, measured", the shipped state is
  "The ring, and the shipping state". "Method notes -- five theories
  died here, and how" is process lessons worth reading first.
  Companion: frontier-fss-bug-report.md.

## The report (moved out of Status 2026-09-29)

Investigated 2026-08-24, from a field report: zooming the Full System Scanner
onto a planet **with rings** looks wrong in a headset. The ring "tiles in" over
a few frames and the two eyes appear to disagree about how far that build has
got — one eye still filling while the other is already solid.

Two findings came out of the first day. A third section, added 2026-08-25,
reopens the part of the conclusion the field then contradicted, and records
what was built to decide it. The process of the first day is kept at the end
because it was expensive and the lessons are reusable.

> **STATUS: SOLVED, field-verified 2026-08-27.** The black squares are
> ring-only, appear during the ~10-frame zoom-arrival window in the
> primary eye's submitted image, and are healed by the shipping pair
> `fix.fss_eye_heal = 1` + `fix.fss_reveal_sync = on`. The final
> mechanism and the fix are in the chapters from round thirty-three
> onward; the vendor-facing writeup is
> [frontier-fss-bug-report.md](frontier-fss-bug-report.md). Everything
> between here and there is the road, kept because its receipts and
> retractions are the reusable part.

## Finding 1: the zoomed body is rendered MONO

**In VR, the Full System Scanner renders the zoomed body once, into a single
shared render target, and hands both eyes the same pixels.** There is no
per-eye render of the body at all — no stereo, no disparity, no depth. The
planet you are "zoomed into" is a flat picture pasted in front of you, inside
an otherwise stereo scene.

Field-confirmed from the other direction on 2026-08-25: at the eyepiece the
zoomed body reads as **a 2D sprite of a projected 3D model**, slightly
pixelated against what the same body looks like approached in the ship. That
is this finding and Finding 2 seen without instruments — flat because mono,
pixelated because half resolution upscaled.

Measured three independent ways, all from one draw census taken in the scanner
with `advanced.census_offscreen = 1`:

1. **One target, drawn once per frame.** The body is produced by ten draws into
   a single `2170x2142` target, issued *after* both eye blocks, identically in
   every captured frame:

   ```
   EYE @19  (first eye)   1 clear + 10 draws
   EYE @23  (second eye)  1 clear + 10 draws
   HALF-RES @79           10 draws   <- once, not per eye
   ```

2. **Both eyes composite from the same source.** Exactly two draws sample the
   half-res target, one per eye, and their bound resources are identical:

   ```
   DC 0 #36  r=@61 (eye A)  s=@38,@103,@104,@105
   DC 0 #60  r=@74 (eye B)  s=@38,@103,@104,@105
   ```

3. **Suppressing that target removes the body and nothing else.**
   `advanced.census_skip_offscreen = 2170x2142` leaves the scanner's chrome —
   body name, object type, reticle arcs, spectral scale, COMMS — and deletes
   the planet and its rings entirely.

The geometry going into it is real: index counts of 36864, 15360 and 5334.

Reproduced identically under OpenComposite and native SteamVR, so none of this
is runtime-side.

## Finding 2: the body renders at exactly half eye resolution

`2170x2142` against eye textures of `4340x4284` is **exactly 2.0x down on both
axes**, then upscaled for display. This is the measured cause of the separate
field complaint that FSS bodies "look lower resolution", and of the pixelated
sprite look above.

Note it is derived from the eye size, not from the panel: an earlier reading in
this investigation attributed the softness to `3408x1917` (which *is* the panel
resolution divided by 1.5023) — that target is the scanner's **chrome** layer,
confirmed by probe, and the attribution was wrong.

The clean 2.0 relationship makes this the same shape of problem
`vscreen_res.cpp` already solves for the on-foot panel: a size chosen in game
code that can be recognised by shape and raised. Not built yet, and now
deliberately sequenced AFTER the build capture below: raising the target means
every writer that addresses it in pixel coordinates has to be known first, and
the capture is what produces that inventory.

## The ring split, REOPENED (2026-08-25)

The first day ended by explaining the two-eye report as misfusion: the eyes
receive the same pixels, so a difference between them "cannot" be real, and a
flat zero-disparity image carrying a thin high-contrast edge inside a stereo
scene is genuinely hard to fuse. The field then said no: tested one eye at a
time, **the left and right eye render differently while the ring section is
loading**. A perceptual explanation does not survive a monocular observation.
The conclusion, not the observation, had to give.

Rereading the evidence, the impossibility argument has a hole a measurement
never closed:

**Both eyes sampling the same texture is not both eyes sampling the same
content.** The two composite draws are ~24 draws apart within the frame. If
anything writes the body texture **between** them, eye B reads newer content
than eye A — every frame of the build, converging the moment the build
finishes. That is precisely the reported shape: it needs the build, it needs a
feature where one step of progress is conspicuous (an edge-on ring), and it
needs per-eye inspection to see. A static census diff can never show it,
because by the time anything is steady the eyes agree.

And the ordering evidence was never taken during the build. The "ten draws
after both eye blocks" pattern comes from censuses armed by keypress — which,
with human reaction, means the settled scanner, ~20 frames after the moment
that matters at 90Hz. The build phase, the only frames the bug exists in, was
never captured. Worse, the instrument could not have seen the writers even if
the timing had landed:

- **Copies were recorded on their own counter**, so a copy's position among
  the draws was implicit in log line order and nothing else; nobody read it
  that way.
- **`UpdateSubresource` was unhooked** — the classic CPU path for streamed
  tile uploads, which is what a build that "tiles in" most resembles.
- **`ResolveSubresource` was unhooked** — the only call that turns an MSAA
  render into a sampleable texture. The body is DRAWN into @79 while the
  composites SAMPLE @38, and no recorded event connects them; a resolve is
  the likeliest bridge, and its position in the frame is the whole question.
- **Compute dispatches were not recorded** — the third way a texture changes
  without a draw — even though the exposure fix has always owned the Dispatch
  hook.
- **The census interned views, not resources**, so even asking "is @79 the
  same texture as @38" was unanswerable from a log.

### What EDVR itself is ruled out of

The one EDVR mechanism that could split the eyes — the transition-flash
withhold, which declines Submits per eye — was checked in code and is
latch-protected against exactly this: `SubmitPairLatch` samples the verdict
once per frame at whichever eye submits first and the second eye follows,
reset at WaitGetPoses (frame_flag.h, "CONSISTENT-LATE BEATS SPLIT"). The
resubmit shadow substitutes per-eye copies of the *last forwarded frame*, so
even a substitution/fallback disagreement shows both eyes same-age content.
The split is game-side content, or it is nothing.

### The instruments built to decide it

All census-side, all free when off, shipped in the DLL as of this commit:

- **`q=` on every recorded line** — one shared per-frame ordinal across
  draws (DC/DCO), copies (DCC), and dispatches (DCX). "Did anything write the
  body between the two composites" becomes a grep: find the two composite
  draws' q values, look for DCC/DCX lines whose dst falls between them.
- **`res=` on the interned-id table** — the underlying resource pointer, so
  an SRV and an RTV over the same texture finally show one identity. This is
  what connects (or separates) @79 and @38.
- **DCC kind `U`** — `UpdateSubresource`, with the destination box: a tiled
  CPU upload names its tile.
- **DCC kind `V`** — `ResolveSubresource`, with the format: dst, src, and
  where in the frame it ran.
- **DCX lines** — every compute dispatch while a census runs, with the
  compute shader's content hash (`ch=`) and what UAV slots 0–3 resolve to.
  Recorded through the exposure fix's existing Dispatch hook; no new patch.
- **`advanced.census_frames` / `advanced.census_lines`** — a census can now
  span 30 frames and 16384 lines, because a build outlives three frames and
  a 4096-line cap drops the tail of an offscreen capture exactly where the
  build finishes.
- **`advanced.census_auto = WxH`** — the decisive one. The census arms
  ITSELF on the first draw into a target of that size after two quiet
  seconds, as if the key were pressed at that moment, and records offscreen
  draws regardless of `census_offscreen`. The FSS body target is drawn every
  frame while zoomed and not at all otherwise, so this fires exactly once
  per zoom-in — on the first build frame, which no keypress can catch. At
  most 8 firings a session; re-saving the ini resets the allowance.

Also fixed on the way: `tools/diff_draw_census.py` could not parse a single
census from the current DLL — the emitter grew `off=`/`copies=`/`offscreen=`
fields on 2026-08-24 and the tool's anchored regexes still demanded the old
text, so a field log yielded "0 complete censuses" while the tool's self-test
stayed green on its own synthesized old-format lines. The regexes now
tolerate additive fields and the self-test carries one census in each
vintage.

### The field procedure

One session, one ringed body:

```ini
[advanced]
census_auto = 2170x2142
census_frames = 30
census_lines = 16384
```

(2170x2142 is the body target on the measured headset — eye size / 2. On the
other headset, take any FSS census with `census_offscreen = 1` first and read
the body target's size from the interned table, or just compute eye/2 from
the log's published eye size.)

Then: open the FSS, zoom onto a ringed body, hold still until the ring is
solid. The log should show "census auto: a draw landed in a 2170x2142 target
…" followed by a 30-frame census. Do it twice (leave the FSS, wait a few
seconds, zoom again) for a second capture free. Send the gfx log.

### Round two: the capture came back, and the texture channel is CLOSED

Flown 2026-08-25 (log `edvr_gfx_20260825_103005`). The auto-arm fired on
both zooms — thirty build frames each, q-clocked, with copies, uploads,
resolves and dispatches all visible. The measurements:

- **The body target IS the composite's source.** `res=` identity: the RTV
  the six body draws render into and the SRV both composites sample carry
  one resource pointer. There was never a copy or resolve to find between
  "@79" and "@38" — they were two views of one texture, which the old
  census had no way to say.
- **The body renders complete before either eye reads it.** Per frame, on
  the q clock: six body draws (q=106–111 in the recorded frame), then eye
  A's composite (q=112), then eye B's (q=137). The first capture's "issued
  after both eye blocks" was the same position read without the cycle —
  end of frame N is start of frame N+1.
- **Zero writes to any composite input between the two reads.** The watch
  covered all four sampled resources (the 6x1 strip, the 135x133 and
  542x535 downsample pyramid, the 2170x2142 body) across 60 frames, both
  zooms: nothing. The pyramid rebuild, the strip copies, and both
  per-eye exposure dispatches (`F1FB2EEFB662F3AA`, q=146/157) all land
  after composite B; next frame's composites read them settled.
- **The "tiling in" is not geometry and not body-target writes.** The six
  draws carry identical index counts (36864, 5334×2, 15360, 5334×2) from
  the first captured frame to the last, both zooms. What distinguishes the
  first zoom is a burst of 2,986 copies — texture streaming into the
  families the body render samples. The visible build is the body's INPUT
  textures arriving (plus any shader-side reveal), re-rendered fresh each
  frame — identically for both eyes.
- **EDVR's withholds are pair-symmetric in practice, not just by design.**
  The vr log shows every withheld frame replaced for eye 0 and eye 1
  together, same millisecond, consecutive counters. And notably: the flash
  detector trips ON the FSS zoom itself — the body-render camera sits
  ~9,880 units out and reads as a teleport until "parked" certification
  catches it, costing a few reprojected frames per zoom-in. A judder lead
  worth its own arc, but it cannot split the eyes.

**What is left is the one input the census never recorded: constant-buffer
CONTENTS.** Both composites bind the *same* 208-byte object at VS b0
(`c=@70` — the engine's camera block, bound by body draws and UI draws
alike), which is therefore rewritten between the two eye passes through
Map/Unmap the census cannot see; pixel-stage buffers were not recorded at
all. A scan-reveal progress value that advances per *write* rather than per
*frame* would compose eye B a more-revealed body every frame of the build —
the reported split exactly, through a channel every capture so far was
blind to. This is also the mechanism class this game keeps exhibiting: the
exposure pass runs "once per eye, first eye then second", RemLok draws into
per-eye passes from shared state, and the particle ring showed one buffer
serving many draws through a moving offset.

### The third instrument: the CB watch

`advanced.census_cb_watch = <vh hash>` (2026-08-25). While a census runs,
every recorded draw running that vertex shader dumps `DCW` lines: the
current contents of its VS b0 and PS b0 — refreshed CPU-side from the
Map/Unmap and UpdateSubresource tees, so a dump is exactly the bytes the
GPU reads for that draw — with the draw's q.

### Round three: the constants are clean too, and the game side is CLOSED

Flown 2026-08-25 (log `edvr_gfx_20260825_125743`), 59 frame-pairs of DCW
dumps across two zooms. The composite binds **no pixel-stage b0 at all**;
its 208-byte VS block was rewritten between the two eye passes every frame,
and the diff of eye A's bytes against eye B's says the rewrite is exactly
what it should be:

- f[16..19] — one matrix row plus translation, the ±35-unit eye offset —
  differs every frame: the per-eye placement transform.
- f[39], f[43], f[47] — the same offset expressed in two more spaces.
- A handful of frames differ at 1e-7 in shared fields: float jitter from
  recomputing the same pose, not content.
- **Nothing else, ever.** No reveal field, no progress scalar, nothing
  steps across the build, and the eye order never swaps (first composite
  is the +35 eye in 59 of 59 frames).

With round two this closes the game side end to end: both eyes composite
the same completed body texture through correct per-eye transforms, every
frame, all the way through the build. **The split the headset shows is not
in what the game renders. What no CPU-side capture can see is WHEN the
compositor samples each eye's texture** — Elite reuses one texture per eye
with no fence, the second eye's GPU work is issued last, and under build
load (the streaming burst is exactly that) the compositor can catch eye A
finished and eye B still drawing: one eye persistently a frame ahead,
monocularly real, needing the build and a headset. Every part of the
report, one channel left.

### The candidate fix: snapshot submission

`experimental.submit_snapshot = 1` (2026-08-25, openvr side, live-
reloadable). Every forwarded frame is delivered to the compositor as a
per-eye COPY taken at Submit — the same copies the transition-flash
resubmit already makes every frame, now handed over instead of the live
textures. The two copies are enqueued back to back behind the frame's
rendering, so whatever the compositor's timing, both eyes' delivered
content is latched at the same point: split staleness becomes symmetric
staleness, the pair latch's CONSISTENT-LATE-BEATS-SPLIT rule applied to
pixels. Any refusal (shape change, fault, budget) falls through to the
live texture for that eye-submit, exactly as stock.

**Flown 2026-08-25 (log `edvr_gfx_20260825_133008`): NULL.** A full A/B/A —
ON at startup, OFF at 13:32:30, ON again at 13:33:25, receipts in the vr
log, no shadow faults, and the same copies visibly correct through fifteen
withholds — and the split did not move. The delivery channel is closed with
the other two.

### Where every measurement converges: the resample

What survives all three rounds AND the snapshot null is not a channel that
delivers different content — it is the one legitimate per-eye operation in
the whole pipeline: **each eye resamples the same half-resolution mono
image through its own placement transform** (the ±35 row the DCW dumps
measured). A ring seen near edge-on is a feature a fraction of a pixel
thick in a 2170-wide image; two sample grids that differ by a per-eye
homography catch and miss its pixels DIFFERENTLY — segments present in one
eye and absent in the other. Monocularly real. Worst exactly while the
ring is faint and partial (the streaming build), converging as it
saturates — "one eye still filling while the other is already solid".
Needs the ring (thin), needs the build (low contrast), needs the headset
(two resamples), immune to latching (content identical by construction).
It is also Finding 2 wearing the report's clothes: the pixelated sprite
and the ring split are one under-resolution with two symptoms.

## The fix: `fix.fss_res` — the body layer at full eye resolution

Built 2026-08-25, off by default. Three identity-tracked moves:

1. **CreateTexture2D**: a single-mip, non-MSAA render-target or depth
   texture asked for at exactly eye/2 per axis is created at double the
   requested size. The auto-census's own trigger measured that NOTHING
   else ever draws at that size — it waited 15,654 frames of menus,
   flight and supercruise for the first such draw.
2. **RSSetViewports**: a viewport of exactly the requested half size, set
   while an inflated texture is bound, is scaled ×2 — plus a draw-time
   backstop for a viewport set before the bind. Receipts logged, counted.
3. **The eye test**: the inflated textures are now exactly eye-sized and
   are excluded by identity — the panel-size collision, solved the same
   way.

The composite needs nothing: it samples through normalized UVs and simply
receives a sharper image. Round two's writer inventory (draws only, no
copies, no compute, no resolves into the body target) is what makes the
whole fix this small. Cost: 4× the pixels for one offscreen layer, only
while zoomed. Live — a flip applies at the next zoom, because the textures
are created per zoom.

The A/B doubles as the ring-split test: at full resolution the ring has
four times the pixels, and the per-eye grids agree about a feature two
pixels wide where they disagreed about one. Sharper body either way; if
the split dies with it, the aliasing story is confirmed. If the split
survives full resolution, what remains is the observation procedure — a
sequential monocular check during an evolving build sees different build
moments in each eye by construction, so the decisive re-test is one
deliberate check with the eye order reversed.

## The layers, as measured

| target | contents | confirmed by |
|---|---|---|
| `2170x2142` | **the zoomed body and its rings** | skip removes body, leaves chrome |
| `3408x1917` | scanner framing UI / chrome | skip removes framing UI |
| `4089x3578` | body icon atlas, zoom UI circles | skip removes pre-zoom icons; `vs:E508648660A352B2` removes zoom circles |
| `4340x4284` | the eye textures | published by `openvr_api.dll` |

Shader families that appear **only** inside the scanner (from a diff of a
census taken outside the FSS against one taken inside):

```
26FC402B1274EE7B  X n=36864  -> 2170x2142    9FFA5D5E79F04873  X n=15360  -> 2170x2142
B12F7A618E1BDE98  X n=5334   -> 2170x2142    203DF51758AADC4D  X n=5334   -> 2170x2142
E508648660A352B2  X n=6  x24 -> eye          889A5279E68F0672  X n=13248  -> eye
953C8123AD8DC13B  N n=6       -> eye         B018D143700AB803  X n=6      -> eye
DC937A645B8DB067  N n=6       -> eye
```

## Method notes — five theories died here, and how

Recorded because each died to a *measurement*, and the same traps are latent in
any future hunt on this codebase.

**1. "The eyes sweep the ring in opposite directions."** Died to the cheapest
test available and one that should have been first: closing one eye at a time.
The report changed from "opposite directions" to "one solid, one filling",
which are different bugs. *Ask for the single-eye observation before theorising.*

**2. "The eyes catch a shared texture at different points mid-frame."** Died to
a census: the two eye blocks are identical draw for draw, and the body's draws
are issued after both of them. **Half-revived 2026-08-25**: the census that
killed it was of the settled state, and could not see copies-in-order,
uploads, resolves, or compute at all. The revival is narrower than the
original — not "the eyes read mid-write", but "a write lands between the two
composite reads during the build" — and it is now instrumented to live or die
on a q=-ordered capture rather than an argument.

**3. "The eyes sample per-eye textures that fill at different rates."** A fix was
built, and its counters were perfect — 10,988 matched draws in each eye, 10,988
snapshots, 10,988 substitutions. It changed nothing. A positive control that
substituted flat **magenta** over those 11,000 draws produced no visible magenta
anywhere, proving the matched draws were invisible. *A counter that climbs is
not evidence that the right object was found. Build the positive control first.*

**4. "A per-eye resolve map is copied out of sync."** `CopyResource` and
`CopySubresourceRegion` were hooked for the census, and the first capture found
two distinct `272x268` R8_UINT textures packed side by side into one `544x268`
buffer and into slices 0/1 of an array — a stereo pair by construction. A fix
substituted the second half's source; 30,575 substitutions, no change. Two
things killed it, both findable beforehand:
- `ceil(4340/16) x ceil(4284/16) = 272 x 268` **exactly**. It is a 16x16 tile
  classification map of the *eye* texture — a screen-space engine fixture, not
  scanner state. Its aspect "matches" the eye because it is *derived* from it.
- The substitution counter climbed to 3,419 **while the game was still in a
  menu**, seventeen seconds before it drew a scene at all.

**5. "The difference is misfusion, not pixels."** Died in the field to the
monocular report above, after shipping in the first version of this document
as a conclusion. It was the only theory of the five that was never given a
falsifiable test before being written down. *An explanation that makes the
observer the mechanism needs the same standard of evidence as one that makes
the renderer the mechanism.*

**The instrument was the bottleneck throughout.** The census recorded only
draws into eye-sized targets, which is why the body — rendered offscreen at
half resolution — was invisible to it for most of a day. Three separate
`census_skip` probes came back "nothing changed" and were read as "wrong
family" when they meant "wrong render path". The decisive test, when it finally
ran, was trivial: `census_skip_range = 1-9999` suppressed **every** eye draw for
six seconds (122,040 draws) and neither the mirror nor the headset changed at
all.

**Two traps worth naming for next time:**
- The **desktop mirror is not the eye textures.** Screenshots of it cannot
  validate an eye-path probe; six probes were run through it before that was
  checked, and their results were void.
- **`foreignContext()` declines silently, and in `CopyVptr` mode it cannot see
  other devices at all** — a second device's contexts keep the runtime's own
  vtable and never reach EDVR's thunks. The declined-draw counter added here
  reports what is refused, but only for contexts that dispatch through our copy.

## Instruments this left behind

- `advanced.census_offscreen` — the census records draws into non-eye targets
  as `DCO` lines. Without this the scanner is invisible.
- `CopyResource` / `CopySubresourceRegion` hooks (slots 47 / 46), census-only —
  copies recorded as `DCC` lines with source, destination, subresource,
  destination x/y and source box.
- `UpdateSubresource` / `ResolveSubresource` hooks (slots 48 / 57),
  census-only — `DCC` kinds `U` and `V` (2026-08-25).
- `DCX` dispatch lines through the exposure fix's Dispatch hook, with compute
  shader hash and UAV resolutions (2026-08-25).
- The shared `q=` event ordinal and the `res=` resource identity column
  (2026-08-25).
- `advanced.census_skip_offscreen = WxH` — suppress draws into an offscreen
  target by size. This is what identified every layer in the table above.
- `advanced.census_frames`, `advanced.census_lines`,
  `advanced.census_auto = WxH` — long censuses, and the self-arming capture
  for builds too brief to catch by hand (2026-08-25).
- `advanced.census_cb_watch = <vh>` — DCW dumps of a watched draw's b0
  constant contents, VS and PS, at the draw, from the Map/Unmap tee
  (2026-08-25).
- The `vScreen declined` totals line, and a note when a **second D3D11 device**
  is created and not hooked (EDVR hooks the first device only).

## Round four: the black squares, and the per-eye tile stack

Flown 2026-08-25 (log `edvr_gfx_20260825_140928`, five captures — four at
stock geometry, one inflated). The field first: `fss_res` is confirmed
sharper and confirmed INNOCENT — the black squares appear at stock
resolution too. And the report sharpened into its final shape: the right
eye's inner-radius-outward sweep is the reveal working as designed; the
left eye showing BLACK 16-pixel squares within the ring AT ALL is the
defect. (Outlined squares animating in both eyes during the
pixelated-to-sharp resolve are symmetric and almost certainly the game's
own scan-tile art.)

What the extended captures measured:

- **Every paired eye draw samples identical resources in ALL EIGHT
  recorded slots** (the x= extension). The per-eye state does not enter
  through sampler identities 0–7.
- **The tile stack exists and is per-eye**: twice per frame, once per eye,
  `9347F8FC2DCE0248` (34,34,16 groups → a 543x536 f60 map) feeds
  `22786F6DE290C577` (272,268 groups → an **eye-sized R16_UINT surface
  written one thread-group per 16x16 tile**) — the black squares' exact
  granularity, refreshed before both composites. Alongside:
  `E65498AE6C2C9F1B` writing THREE 272x268 f62 maps each frame, and two
  shaders (`074CB657FDBD43E6`, `76BFC737F1F8CB83`) writing a 101 MB pool —
  the moving-offset record class this game keeps using.
- The consumer is therefore in a channel still unrecorded: PS slots 8+,
  another shader stage's SRVs, or per-draw offsets into the pool carried
  in constants b1+ (the DCW watch read b0 only).

## Round five: the dispatch probe, and what it taught about probing

`advanced.census_skip_dispatch` (2026-08-25): compute dispatches named by
hash are not forwarded while the spec is set — the draw skips' completing
half. `HASH:N` narrows it to the Nth occurrence per frame (one eye of a
per-eye pair); `experimental.dispatch_pair_sync = HASH[:r]` copies the
first occurrence's UAV0 over the second's, probe and equaliser in one.

The first probe flight produced five nulls and one lesson, and the log
voided the nulls: the session's only zoom preceded every skip window, and
the squares exist only during a build — every skip probed a settled body
with nothing left to change. *A probe against a transient must bracket the
transient: set the spec, THEN trigger the build.* The flight still paid:
four of the five candidates can be skipped for thousands of dispatches
without visibly harming the settled scene, and `E65498AE6C2C9F1B` cannot
be skipped at all — it runs at the top of every frame and the engine
stalls presentation waiting on something derived from it (recovered on
clearing the spec). It is struck from every candidate list.

## Round six: cinema mode, and what the squares turned out to be

The probe redo never needed to fly. Two field observations and two
measurements closed it:

1. **The black squares appear in HMD Cinema Mode** — a mono pipeline, one
   render, both eyes shown the same panel. The squares are born in the
   single render, not in per-eye machinery.
2. **They replay on every re-zoom.** The four back-to-back phase-A
   captures carry identical, flat copy traffic (544/544/540/540 — no
   cold-streaming burst), and the squares showed on each. They are not
   asset arrival.
3. **The streaming moves whole mips, not tiles** — 64 of 544 copies carry
   boxes at all. Sixteen-pixel black squares cannot be missing content
   tiles; sixteen pixels is the granularity of the game's tile
   classification systems and of its scan-effect art.
4. The outlined squares animating in both eyes during the resolve are
   unambiguously styled scan art — the same visual family.

**Conclusion: the black squares are, with high confidence, part of the
game's designed scan-resolve tile animation**, playing in the mono body
layer and composited identically to both eyes. *(Superseded — the
monocular re-test and rounds seven through thirteen, below.)*

## Where that leaves the original report

Five instrument rounds measured every objective channel between the game
and the two eyes, and all of them came back symmetric:

- the body texture: one resource, rendered before both composites, zero
  writes between the reads (round two);
- the composite constants: pure per-eye camera placement, no reveal
  field, stable eye order (round three, the DCW dumps);
- the delivery: pair-latched withholds in practice, and a snapshot-
  submission A/B/A that changed nothing (the null that closed it);
- the sampled inputs: identical resources in all eight recorded slots
  (round four);
- the squares themselves: mono-origin, designed (round six).

What remains is the stimulus itself: a rapid, high-contrast, tile-
granular animation playing on a ZERO-DISPARITY mono plane inside a stereo
scene — the hardest case binocular fusion has. Day one's misfusion
explanation died, correctly, for being asserted without evidence; it
survives at the end because five rounds of evidence eliminated everything
else and named the exact stimulus. The percept is real; the pixels are
the same in both eyes.

What EDVR ships from this arc: **`fix.fss_res`** — the body at full eye
resolution, field-confirmed sharper, which softens the whole effect by
giving the animation four times the pixels — plus the instrument suite
(q=, res=, x=, DCC U/V, DCX, DCW, census_auto, census_frames/lines,
census_skip_dispatch with :N, dispatch_pair_sync, submit_snapshot), every
piece of which the next hunt inherits.

## Rounds seven through thirteen: the split is real, the composite is innocent

The monocular re-test round six asked for was done: viewed one eye at a
time, **the left eye shows black squares filling in and the right eye a
smooth inner-to-outer reveal, and it never swaps sides.** A percept
cannot survive a monocular observation; the split is in the pixels, and
the second conclusion gave the way the first one had to.

Six probe rounds then eliminated the composite draw itself, every one
with an engaged receipt in the log:

- **Slot probes** (`advanced.fss_composite_probe = SLOT:COLOUR`): each of
  the four sampled layers held flat magenta/black/white for exactly the
  composite's draws. Every layer is visible in both eyes; none carries a
  per-eye difference. (`black:2` blanked the whole body in BOTH eyes —
  slot 2 is the planet's visible base; a lead that went nowhere, twice.)
- **Constant sync** (`fix.fss_reveal_sync = steady`): the second eye's
  composite drawn with a byte snapshot of the first eye's PS b1 scene
  block — the dissolve's progress values forced identical. Engaged, null.
- **Depth off** (`fss_composite_probe = depth`): the composite drawn with
  depth test disabled — per-eye depth culling. Engaged, null.
- **Opaque** (`fss_composite_probe = blend`): the composite drawn with
  blending off — per-eye destination showing through dark output.
  Engaged, null.
- The composite's PS disassembly contains **no discard** — every covered
  pixel writes.

Identical inputs (res-proven), identical constants (forced), no depth, no
blend, no discard: **the two composite draws are byte-equivalent. The
squares are not painted by the composite.** They are painted after it, by
something the draw census cannot see — and re-mining the round-four
capture with that question found it.

**The per-eye temporal reconstruction pipeline**, every frame, in q=
order: `E65498AE6C2C9F1B` writes three 272x268 R8_UINT tile maps; copies
assemble a 544x268 stereo map; `22786F6DE290C577` runs **once per eye**
expanding them to two full-eye 4340x4284 R8 masks (@74/@97) — one thread
group per 16x16 tile, the squares' exact granularity; the two composites
draw; a full-eye HDR copy follows **each** composite (per-eye history
snapshots); then per eye, `E861F611375E7ECC` writes a 5426x5356 f11
**temporal accumulator** (@235/@240) and `B74273EC13F7CD59` writes the
5426x5356 f24 output — the reconstruction that upscales 4340 to the
submitted 5426. No recorded draw ever samples the masks or accumulators:
the consumers are compute, which is why eleven rounds of draw-side
instruments could not see the writer.

The elimination already run maps onto that pipeline: the wholesale 2278
skip and the 2278 pair-sync both flew and nulled — **the masks are
exonerated**. The one per-eye state never touched is the **accumulator**:
if the left eye's history holds black at freshly-changed tiles while the
right's holds converged content, the reconstruction paints black squares
in exactly one eye, filling in as accumulation converges — the field
report, verbatim. Round thirteen is the staged-and-never-flown arm from
the freeze-aborted batch: `dispatch_pair_sync = E861F611375E7ECC:r`
(both eyes keep one accumulator). Squares dying in the left eye convicts
its history; squares growing into the right under the un-reversed
direction confirms the mapping.

## Round fourteen: the first hard asymmetry — a constant buffer bound for one eye

The accumulator pair-sync (`E861F611375E7ECC:r`) came back engaged-and-null
(receipts: ARMED and first-copy lines), which with the mask nulls means the
left eye REGENERATES its squares every frame from per-frame inputs — no
carried state involved. And the post-composite draw sequences are fully
paired across all 30 captured frames: the draw side is exhausted outright.

So round fourteen taught the census the one binding class it never carried:
what a dispatch READS. DCX lines gained `s=` (CS SRVs 0–7) and `cb=` (CS
b0–b1), and one auto-census flight measured, 30/30 frames:

- `E861`/`B742` (accumulate + output): symmetric end to end — per-eye
  3-deep input rings, one shared 80-byte cb.
- `E654` (tile maps): symmetric.
- **`22786F6DE290C577` (the mask builder): CS b0 per-eye as expected — and
  CS b1 BOUND for one occurrence (a 480-byte parameter block) and UNBOUND
  for the other.** An unbound constant buffer reads as zeros: one eye's
  per-pixel masks are built with default parameters every frame. This is
  the first hard per-eye difference in bound state the hunt has measured,
  it sits at exactly the pass whose output granularity is the squares, and
  it explains why syncing the mask OUTPUTS never healed anything — the
  divergence re-enters through the parameters each frame.

## Round fifteen (armed): equalise b1, both directions

`experimental.dispatch_cb1_lend = 22786F6DE290C577` — a dispatch arriving
with b1 empty runs with the buffer the same shader last ran bound with,
restored to empty after. If the squares survive, the flip side
(`dispatch_cb1_strip`) runs the bound eye parameterless instead. Whichever
direction heals names the good state; the healing arm is the fix's shape
(gated, shipped as a fix key) if Frontier's own binding is the bug.

## Rounds fourteen through thirty-one: the hunt below the game, and the ending

The short version of eighteen further rounds: every layer between the ring
draws and the headset panels was instrumented, forced identical, or
pixel-dumped, and the black squares survived all of it -- because the
question was wrong. The arc's ending, in the field's own words: the
squares are the game's intended scan-resolve animation of its PRIMARY
view, and the anomaly is that the second eye shows a smoothed variant.
*(Superseded -- round thirty-three's arrival measurement, below,
inverted this: the primary eye carries a genuine defect the second eye
does not.)*

What the long road measured, for whoever walks near it again:

- The census gained: what dispatches read (CS SRVs/CBs), DispatchIndirect
  and indirect draws, CopyStructureCount, foreign-context work, eight UAV
  slots, and the hard lesson that tokens interned PER VIEW so one resource
  could wear two ids (@36==@115 proved it; pointer-keyed joins are the
  law now).
- The FSS frame is fully mapped: per-eye ring G-buffer (budgeted tile
  subsets per frame over a cleared target -- the 50/50 arrive/vanish
  oscillation in the two-pass dumps), the ring quad's three black
  mechanisms (flag bit 4, the cb2[46] fade, the t3 illumination-map
  exposure), the segment mesh's vertex-driven fade, the GPU-driven tile
  passes behind DispatchIndirect, the tonemap/LDR chain, and the
  reconstruction pair (E861 accumulate -> B742 pack, alias-closed by
  pointer).
- Every mechanism was neutralized -- transcribed shaders through the new
  ps/cs compile paths, GPU-side constant patches, an injected compute
  hold -- with receipts, and the VISIBLE squares survived every one:
  they are painted into BOTH submitted eye images equally (measured
  three times: early build, mid build, strict-black, clustered), and the
  flat screen shows them too, because the flat screen mirrors the
  primary view.
- The percept asymmetry (left crisp, right smooth) reproduces on a stock
  install, across OpenComposite AND SteamVR-via-Steam-Link, on two
  headsets. It is the engine's own presentation of its second eye, below
  every lever a proxy DLL owns; the mono-submit probe (both eyes shown
  one image) moved the squares into both eyes, proving the display chain
  faithful.

What ships from the arc: `fix.fss_res` (the body at full eye resolution,
field-confirmed sharper), the transcription/compile machinery
(shaderSwapCompilePs/Cs), the ring shader disassemblies
(docs/shaders/fss-ring-ps.asm, docs/shaders/fss-ring-mesh-ps.asm, docs/shaders/fss-chrome-ps.asm),
the checkpointed eye-image dump with two-pass blink analysis, the frame
pacing log, and a census that can no longer be lied to by omission.
`fss_ring_feed` stays stock: its arms are probe-grade history, kept for
the next hunt.

The evidence package here -- stock reproduction on two stacks, symmetric
submitted images, the monocular field reports -- read at the time like a
complete Frontier bug report: the second eye's FSS scan animation does
not match the primary view's. *(Superseded -- the next chapter's
measurement flipped which eye carries the defect, and the report that
was eventually filed says the opposite.)*

## Round thirty-three: the arrival window, measured

The instrument that ended the stalemate was the smallest one: a per-frame
tile-luminance series over both **submitted** eye textures, folded on the
GPU into ~14 MB of atlases so it could run every scanner frame without
hitching. It caught what every earlier capture had missed by aiming at
the wrong moment:

**For roughly ten frames at each zoom's ARRIVAL — before the body-layer
gate opens — the primary (left) submitted image carries hard-black
(0.000-luma) not-yet-resolved tiles the right does not: 16 versus 2 on
the worst frame, resolving in unison when the gate opens.** Every earlier
symmetric dump had measured the build proper, after arrival; the squares
live in the arrival, and the arrival had never been captured.

That one measurement inverted the round-thirty-one ending. The right
eye's smooth reveal is not a smoothed variant of the primary view — it
is the correct image, and the primary view briefly shows tiles its
amortized renderer has not reached yet. The flat screen mirrors the
primary view, so flat players see the same tiles; on a monitor they read
as part of the dissolve art, which is why the defect survived flat QA
for years.

Two corrections landed with it. First, the eye images ARE genuinely
different at arrival — the round-30b "both submitted images equal"
reading was true of the build but not of the arrival window. Second,
mid-campaign the OpenXR Toolkit was found enabled and disabled: the
E861/B742 "reconstruction" pair that rounds 13-21 had hunted was almost
certainly the Toolkit's scaler (it vanished from sessions with the
Toolkit off), and the squares survive without it. The arrival content's
real producer, measured Toolkit-off by a census armed on the player's
own zoom press, is the game's own GPU-driven amortized tile renderer:
`DispatchIndirect` compute (`5998146D464F5C0E` + `EB0245DE0BB23BB6`),
per-eye argument buffers and tile lists, one thread group per 16x16
tile, per-frame history copies — and **zero body-composite draws in the
arrival frames**. The entire draw-level campaign had been structurally
blind to the frames that matter.

## Rounds thirty-four through forty-four: the roads not shipped

Kept short, because each closed cleanly:

- **Arrival mono** (submit the right's image to both eyes for the
  window) worked mechanically and was rejected on comfort grounds: "the
  eyes must not see a difference" cuts both ways — a stereo scene going
  flat for 10 frames is itself a difference.
- **The heal, first attempt** (fill the left's hard-black pixels from
  the right at the optical-infinity shift) removed the squares — the
  only intervention that ever had — but its classifier misfired on menu
  text and dim ring content through four gate revisions, and was
  retired to await better scoping.
- **The theater** (rounds 40-44): the whole scanner as a world-anchored
  virtual screen showing the flat renderer's image — built through five
  versions (screen-in-space, whole-mode, void-only, panel-rect crop
  with homography rectification and a dual-eye nose-shadow stitch, a
  probe-calibrated centering servo) and **parked as a feature**
  (`fss_theater = 0`), superseded for the bug by the heal. Its
  panel-rect deriver survives in the shipping fix as the heal's spatial
  scope.
- **Cinema-mode automation**: the game's own HMD Cinema mode
  (`StereoscopicMode 5`) was investigated as a lever. The game writes
  its settings file on menu-apply but never re-reads it mid-session;
  the only trigger is the game's own menu code, and injection was
  declined on principle (a proxy shims, it does not patch). The field
  A/B then made it moot: cinema mode still shows the VR-variant FSS —
  the flat renderer's extras are gated on HMD-session-active, not
  presentation mode.

## Rounds forty-five through forty-eight: the pair that shipped

The composite-level channel closed first. `steady` re-flown Toolkit-off
with clean receipts: engaged, null — the constants alone are not the
composite's split. `redraw` (re-issue the first eye's composite after
the second's, its own transforms with the late content) bought the
frame-order fact that retired it: the game draws the first eye's UI
BETWEEN the two body composites, so any late repaint buries that UI —
there is no safe moment. `lockstep` inverted the direction — the second
eye's composite drawn reading byte-identical CopyResource'd copies of
the first eye's four input textures and its scene block — and round
forty-six gave it the verdict: the composite-drawn resolve is identical
in both eyes, fused, fully native stereo. That is half the fix.

The arrival half could not be fixed at the dispatch layer. The surgery
ladder closed structural: overwriting the accumulator poisons the
temporal feedback (artifacts, cockpit bleed as history decays);
overwriting the resolve output arrives pre-consumed (the per-eye chains
INTERLEAVE — the first eye's consumers read between the two
dispatches); the previous-frame reverse still artifacts. The loop
defends itself, so the only safe layer is the submitted image — and the
heal came back with the scoping it had lacked:

- **One test, no gates** (v6): a left pixel under 0.004 luma takes the
  right eye's pixel at the infinity shift, dx = W(outer-inner) /
  (outer+inner) ≈ 1053 px at 5424 wide, computed from the headset's own
  published eye tangents. Every added cleverness (bright-region taps,
  interior gates) had only vetoed real fills and speckled.
- **The window is the arrival, 600 frames from the player's own zoom
  press** — the killer timing insight: the zoom TRANSIT takes ~3
  seconds and the squares appear at its arrival; every earlier
  30-frame window had covered only the departure. The press is read
  from the player's adopted Elite binds (keyboard and XInput pad), the
  window is journal-gated on `GuiFocus 9` so a stale jump or an
  ignored keypress can never open it in the cockpit.
- **Only inside the screen's own rectangle**: the theater's panel-rect
  deriver publishes the scanner screen's corners per engage; the fill
  exists only inside that AABB. Unscoped, the heal had doubled the
  near-field neon frame's lines — content at sub-metre depth where the
  infinity disparity is the wrong shift.

Field-verified 2026-08-27: no squares, no artifacts, stereo intact.

## The ring, and the shipping state

A field fact sharpened the whole story after the fix landed: **the
black tiles only ever appear on ringed bodies — no ring, no bug.** It
fits the mechanism exactly: the ring is the temporally amortized
content (its offscreen buffers render as a budgeted subset of 16x16
tiles per frame — the round-27 oscillation measurement), so only ring
pixels ever have not-yet-reached tiles to expose. A ring-less body has
no amortized layer and arrives whole in both eyes.

The shipping state, both repo-ini defaults:

- `fix.fss_eye_heal = 1` — the arrival window's fill, scoped as above.
- `fix.fss_reveal_sync = on` — the composite-drawn resolve held
  byte-identical (the campaign's "lockstep" mechanism).

The campaign's scaffolding was removed once the fix stood alone (the
survey probe, the press census, the pair-sync's indirect extensions,
the redraw mode); the instruments that earn their keep stay. The
vendor-facing report — stock repro on two stacks, the ring-only fact,
the mechanism, three fix directions — is
[frontier-fss-bug-report.md](frontier-fss-bug-report.md).

## 2026-09-16: the Quest 3 ghost under the native path

Report: on the Frontier install, Quest 3 via VirtualDesktopXR, zooming
into a planet in the FSS shows a slight double ghost on the body with
`fix.fss_eye_sync = on`; not seen on the Pimax. Sean suspected the
asymmetric frustum. Two flights and one eye dump later:

- Frustum: ruled out from the logs before any build. The cull guard's
  Symmetric mode has the game render +-1.2799 horizontally
  (`projection_query m02=0`), so the heal's 0 px infinity shift is right
  for that image, and the panel centre projects to the same u in both
  eyes (`fss panel rect ... uA=0.496 uB=0.496`).
- Flight 1 (`edvr_gfx_20260916_151842.log`, build e8c5bfe, heal
  toggled live): heal on = ghost, off = clean. The heal half owns it;
  the lockstep half does not.
- Build 0522215: until then the native heal read the raw source and
  the healed eye skipped the temporal pass, so for the 600-frame window
  after a zoom press the left eye reached the headset as the raw
  jittered 2307x1652 input beside a 3550x2542 DLSS right
  (`native_temporal_summary left=19125 right=22322`, the difference
  exactly `healed=3197`). The host now runs the temporal pass first and
  heals its output. Flight 2 on it: `left=12192 right=12192`,
  `skipped=10`, `source=temporal`; the blur while moving is gone, the
  ghost is not.
- Eye dump 154720 (16 consecutive treated left crops, 2155^2, INSERT
  during the zoom): the phase-correlated shift between consecutive
  frames is +208 (the dump hitch), +76, +16, +20, +16, +16, +4, 0, -4,
  -4, -4, -8, -12, -16, -16 px horizontally, and the body's lit radius
  is constant to 0.3%: the whole panel translates (head motion; the
  text moves with the planet), it does not grow. Per frame 30-100k
  pixels are hard black now and lit a frame earlier. A simulated fill
  (the previous frame as the donor at the same pixel, which is what
  the provider did with the previous right; the panel is mono at 0 px)
  paints a second copy of the limb into the black gap, displaced by one
  frame's motion: the ghost. My earlier claim that a one-frame-old
  donor is null on a zoom-in was wrong; it would be null only for a
  pure scale-up with no translation, and the view translates.
- Fix: the FSS provider no longer heals at the left's submit with the
  previous frame's right. `treatEye` snapshots only; the new table
  entry `healPair` delivers the heal once both of this frame's eyes are
  snapshotted, and the host defers the target eye's later stages
  (sharpen, menu, capture) to the other eye's submit when the provider
  reports the heal pending. Same frame, same pose: the donor pixel is
  where the target's black is. The mirror mode (`fss_eye_sync =
  mirror`, developer value) rides the same entry and needs no
  deferral, the right is the target and it submits second.
- Flight 3 (17:05, the built tree of 10705da before its commit, so the
  version string reads `0522215-dirty`; DLL linked 22:59:35 UTC):
  "looks good". `native_fss,first_healed_eye=0,sequence=13748,
  source=temporal,donor=pair`; `native_features_summary ...
  fss_healed=4009/0,fss_deferred=4009`; `native_temporal_summary
  frames=19791,left=19776,right=19776`; `native fss totals:
  snapshots=21358, healed=4009, gated_out=18194, donor_refusals=0`.
  Merged to main as e08e899 and pushed.

## 2026-09-16: the FSS UI under DLSS

Sean, after flight 3: the FSS's own UI shimmers on the initial screen
"when moving the UI around" (panning the scanner). Eye dump 170752
(17:07:52, before the first zoom of that session, Quest 3, DLSS
quality, preset K, `ui_depth = on`, `ui_depth_reactive = 0.5` in his
ini, which preset K ignores). Read offline; the scripts are in the
session scratchpad, the numbers are the evidence:

- The UI covers 2.33% of the input pixels (0.20% solid): the bottom
  bar and its text, the signal markers, the reticle and the panel's
  grid lines are all interface-projection composites. The UI surface
  itself did not change in the dumped frame (`UiEdits` 0.00% at 1,
  0.21% decaying), so the UI resolve's edits are small (0.14% of the
  output over 8/255).
- Raw against treated on the marker at input (1290-1390, 925-995): the
  raw crops C00-C04 draw the marker crisp and static, 0 px/frame (one
  (-3,-2) reading at a correlation of 0.42); `DlssBeforeUi` already
  holds it doubled and `L0` matches it, so the doubling is DLSS's
  reconstruction, not the UI resolve; the treated crops T00-T03 show it
  doubled, doubled, cleaner, clean: the shimmer.
- The vector DLSS was given at the marker's UI pixels: x -3.84 px; on
  the bottom bar: x -5.5 px. The sky beside them at depth 0: +316/+229
  px (history refused there, harmless on a dark sky). `motion.csv`
  frame 16269: `cameraR` yaw 0.23 deg (5 px at this eye) while `headR`
  is 0.01 deg; the next frames 0.01, 2.1 and 1.5 deg of camera against
  0.21, 0.07 and 0.00 deg of head. In the FSS the camera EDVR latches
  is the scanner's panning view, which moves the sky inside the panel
  and nothing else; the panel's UI moves with the head only. The
  "interface keeps the camera's path at the pixel's depth" rule
  (temporal_shader_source.h, the note above `uiEvidence`) assumes the
  camera is the head, true in the cockpit, false here.
- Ruled out: the UI resolve (identical before and after), the bias
  mask (preset K ignores it, and the mask is 0.49-0.51 over the whole
  coverage either way), the heal and the deferral (the gate was closed:
  no zoom press yet, `first_healed_eye` comes 4 s later).

Candidate fix, not built: a head-path kind for interface-projection
composites. ui_depth already tells the temporal pass the path per
family through the mask value's parity (even rides the body's path, odd
keeps the camera's); a third kind would reproject with the head delta,
which the pass already computes for `motion.csv` (`headR/headTv`,
`temporalHeadDelta`). The flight HUD's world-tracking strokes (the
station brackets that "track the station's centre") keep the camera's
path. Before the kind is applied outside the FSS, check what the
cockpit's read camera is relative to the head there (the HUD is clear
in flight today, so either the read camera is already cockpit-relative
or something else carries the ship's motion); the FSS alone can gate
the kind on the panel rect being live. Testable in the FSS first, then
on a rolling ship with the HUD up.

## 2026-09-16: the scanner's interface on the head's path

Built on Sean's "build the head path", not flown. What was built is
the FSS-gated form of the candidate above, not a new mask kind: a
kind would need ui_depth to know which composites are the scanner's,
and the scanner's own screen is already tracked per frame.

- The gate. `readFssPanelRect` was the first thought and is the wrong
  signal: it returns true forever after the first publish
  (`fssPanelRectSeq != 0`, frame_flag.cpp:379), a "was ever" not an
  "is". The per-frame word is the chrome tracker in
  `beginPanelOverride` (vscreen.cpp ~1496): on every frame it sees the
  scanner's screen composited by the two world-quad pipelines it bumps
  `fssChromeStamp`, before the eye is submitted. The temporal pass
  (`fssInterfaceLive`, temporal_pass.cpp) treats a stamp that moved
  this frame or last as "the scanner is up" -- last, because the heal's
  deferred eye is finished at its partner's submit and the boundary
  can land between. The tracker ran only for the heal, the census jump
  and the theater; `temporalPassWantsFssChrome()` now keeps it running
  whenever the pass is on, so a rig with `fix.fss_eye_sync = off` still
  gets the path.
- The flag. `probe.w` bit 128 (bit 64 was taken by the eye dump's
  header, which ORs "separated" into the same word). The `uiFlags`
  lambda sets it for both `p.probe[3]` sites, and `wantUi`/`uiOwn`
  bind the interface mask when it is set, so the shader can ask
  `uiCovered` even on a frame nothing else wanted the mask.
- The shader. Both copies of the world/ship split -- `fetchHistoryT`
  and the mv pass -- take `&& !scannerUi` on the world branch, where
  `scannerUi` is the flag and `uiCovered` at the pixel. An interface
  pixel then falls through to the head's rows at its own depth
  (`dp * z + tvUsed`), or the rotation-only head path when it reads no
  depth. The sky beside a stroke keeps the pan, as before; the body
  path's `!uiCovered` exclusion already skipped these pixels.
- The receipts. A once-note in the gfx log when the flag first sets,
  with the frame; the eye dump's header `uiFlags` carries the bit on
  every engaged frame; `g_fssInterfaceFrames` counts left-eye frames
  (not printed yet; add it to a summary line if a flight needs it).
  If the pass never ran the new code there is no note and no bit.
- Not a rig test: no rig runs the split (the shader rigs extract
  single functions), and `fetchHistoryT` drags in every layer texture.
  The eye dump is the test; brief in the Status block.
- Seen in passing, not folded in: `uiCovered(q)` indexes the
  region-sized `UM` with the full-target coordinate its callers pass
  (`region.xy + p`), while every other `UM` read subtracts `region.xy`
  or uses the local index. Harmless where `region.xy` is 0 (the native
  path, a whole-texture submit), a one-region offset otherwise.

## 2026-09-16: the scanner's sky at 49 draws

Flight on 36a9fa1 (`edvr_gfx_20260916_181837.log`, right build), eye
dump 182049 taken on the initial FSS screen while panning with the head
still. Sean: "still blurs with head still".

What the head path did: the once-note fired at frame 5633; the dump's
header `uiFlags` reads 0x91 (bit 0x80 set, with the holo bit and
ui_depth's bias bit); the interface mask holds 9,332 solid pixels (value 129,
~220 m) and 131,022 faint ones (125, 0.7 m); the solid pixels' `MV` is
+0.07/+0.13 px (p10-p90 +0.06..+0.12) while `motion.csv` has the camera
at 0.12-1.57 deg/frame and the head at 0.014-0.095 deg. The bar text
("STEPPED ZOOM ?? |") in the 16 treated crops has 0.93-1.00 of the
raw-upsampled crop's edge gradient and the same peak luma: not doubled,
not blurred. The head path is right and is not what Sean sees.

What blurs: the sky. The no-UI pixels' `MV` p50 is +0.09 px under the
same pan (a 0.21 deg/frame pan is ~5 px at this width, 1.5 deg ~40
px), because the world path was OFF on the dumped frame and on 12 of
the 16: `cameraTv3` (the shader's `tvCam.w`) is 1 only on frames 12488,
12492, 12497, 12502, and those are exactly the frames `sceneDraws`
reads 50; the rest read 49, one under the `sceneDraws >= 50` floor that
stood in for "a real scene, not the main menu's one-or-two-draw
backdrop". Dump 170752 (the previous flight, another system) had 64
draws on every frame and the world path on, which is why THAT dump
showed the pan vector at the sky and the UI doubled by it. The
registration line at 18:20:58 covers the scanner: "the world path took
24.1% of pixels" (one frame in five); the next one, at 18:21:18, says
94.8%. In the crops, a 160x100 sky window with 111 star-like
points shows the treated grid lines and the orange signal dots smeared
to near nothing against the raw's (`ui_sky_182049.png`): DLSS given
zero motion for content moving 5-40 px a frame. The same floor also
turned on the menu's assumed depth for the sky (`split.w`), a wall a
few metres off under head sway, and `temporalCameraFollowScore`'s
"trust the bound rows in a populated scene" short-circuit, which is
why the log has 36 s of "the world path waits for the scene camera"
right after the scanner opened (18:19:21 to 18:20:06).

The fix, built, not flown: the floor is 8 (`kTemporalSceneDrawFloor`
in temporal_math.h, used by the world path, the follow score and the
menu depth). The menu was measured at one or two draws; a scene at 49
in the sparsest system seen so far and hundreds in the cockpit; the
floor sits between with margin on both sides. A counter on the
registration line's second half says when the floor alone stood the
path down and with what count, so the next log tells a floor still in
the way from a menu. temporal_test checks 49 is a scene and 2 is not.

- Ruled out: the interface's own path as this flight's blur (the
  numbers above); the UI resolve (identical to `DlssBeforeUi` at the
  bar, as before).
- Not measured: whether the scanner's draw count varies with the
  system's body count (64 in 170752's system, 49 here). If a system
  ever draws under 8, the floor is in the way again and the counter
  says so.

## 2026-09-16: the spectral graph is not interface

Flight on cc10e79 (`edvr_gfx_20260916_183753.log`, right build), eye
dump 184002, the initial FSS screen, panning with the head still. Sean:
"still blurred".

The floor did its job: `sceneDraws` 35-36 this time (a third system),
`cameraTv3` 1 on all 16 frames, the sky's `MV` p50 -7.09 px under a
0.29 deg/frame pan (the camera at 0.25-2.10 deg/frame, the head at
0.014-0.056), the bar's solid pixels at -0.10 px and its text at
0.94-0.99 of the raw's edge gradient in the treated crops. And the
"scene camera accepted" note came 13 s after the scanner opened instead
of 36.

What still blurs, from the star-dense window the sky script picked
(input x 1215-1375, y 1329-1429): the "FILTERED SPECTRAL ANALYSIS"
label and the graph's strokes, crisp in the raw and a three-to-four
copy smear in every treated crop (`ui_sky_184002.png`). At the text
(x 1180-1400, y 1318-1350, 181 bright pixels): the interface mask is
zero on 94% of them (the 6% at 129 are the bar's edge), `Z`, `SceneZ`,
`HoloCoverage` and `PrevZ` all zero (the far plane), `UiEdits` zero,
`MV` -5.04 px on every one -- the world path's far-plane pan -- while
the raw crops move the window's content by 0 to 1 px (jitter). So the
label is static on the panel, unmarked, depthless, and reprojected as
sky. Before the floor fix it happened to be still on four frames in
five; before the head path it was doubled with the bar. It was the
visible part of the original complaint all along.

Why unmarked. docs/fss-panel.md has the anatomy: the chrome is two
persistent 3408x1917 surfaces updated damage-style, a draw or two a
frame across four families (`1012E00B3CB44469`, `666EF0C4C616F67E`,
`A3E5D3FCBC1165F8` = the GUI icons family, `019E81F4EECFB371`), and
composited by the general world-quad pipeline (vs A888D51024D9798E, the
menu panel's) sampling them at PS slot 1; B018D143700AB803 is the
position-only depth prepass of the same quad, and is on ui_depth's
built-in exclude list for its own reason. ui_depth's offscreen learner
asks a newly bound target's vertex shader sixty-four times for a GUI
family and then leaves it alone for 600 frames; the graph's stratum is
drawn by the other three families and its labels once, so it is never
learned, its composite is never a "composite of a learned surface", and
nothing is logged because nothing was declined (the `ui depth totals`
line's "left alone" counters read 0.0 through the scanner). The bar's
stratum IS learned (its text changes, the GUI renderer draws into it),
which is why the bar is marked at 129 and ~220 m through the interface-
projection remap (family line `vs A888... ps 015EF9349EC097E8:
interface projection; alpha-aware depth`, 18:39:52).

The fix, built, not flown: the chrome tracker in `beginPanelOverride`
already recognises the composite by content hash and the slot-1
surface's size, before `uiDepthOnEyeDraw` sees the same draw; it now
hands the surface to `uiDepthLearnScannerChrome` (ui_depth.cpp), which
adds it to the learned set and overwrites the view memo (which would
otherwise answer "not a surface" for a hundred frames). From that draw
on the composite is classified like the bar's: the A888 family's
transcription reads t1, so a pixel shader this build has no name for
stands in by slot. The label and the strokes then carry the mask, my
scanner exemption gives them the head's path, and the remap's ~220 m
is harmless under a still or turning head.

- Ruled out: the world path's own vectors (right at the sky and the
  bar), the head path (right at the bar), the UI resolve (`UiEdits` 0
  at the text -- it never touches an unmarked pixel).
- Known, not fixed: the ~220 m remap under head SWAY (above, Next
  flight).

## 2026-09-16: the graph is under the alpha floor

Flight on 90553bc (`edvr_gfx_20260916_185845.log`, right build), eye
dump 190129, the initial FSS screen, panning with the head still.
Sean: "still blurred".

Everything the previous builds fixed held (`uiFlags` 0x91 on every
frame, `w 1` at 41 draws, the bar's solid pixels at +0.09 px, the sky
at +2.9 px under the pan), and the learn note never came. What the
dump says about the graph, at the same window as before (input x
1180-1400, y 1318-1350, which this time holds the graph's bars, its
frame lines and a bright cursor line rather than the label):
`ui_stroke_probe.py` finds 845 blue stroke pixels of which 833 are
unmasked and carry the pan (+2.06 px); the 66 masked pixels in the
window are the cursor line, at +0.05 px and ~225 m. And the mask is
a THRESHOLD, not a per-element verdict: `ui_thresh.py` on 190129
gives luma 20-60 masked 0.0% (2499 px), 60-80 44%, 80+ 100%; on
184002 (the label's window) 20-60 0.0% (1334 px), 60-80 0.6%, 80-100
83%, 100+ 100%. The masked pixels of the label were its brightest
glyph cores. A composite that was never classified would mask nothing
at all; a composite reissued through `kPanelDepthHlsl` masks exactly
the pixels whose surface alpha clears `advanced.ui_depth_alpha` (0.5),
and over a black sky the composite's brightness IS that alpha times
the stroke's colour. So both strata were learned and reissued all
along, and the graph -- its bars, its frame, its label -- is drawn
dim, under the floor that exists so the loader's 40% scrim over the
ship model writes no depth. The bar's strokes and labels are bright
and clear it; the cursor line clears it; nothing else in the graph
does.

Why the learn was silent, in hindsight: the only silent early return
in `uiDepthLearnScannerChrome` was "already learned"; the other three
(no view in the shadow, not a texture, wrong size) are ruled out by
the census lines of the same frame (`s=@652,@653` and `s=@652,@663`
on both A888 composites, read off the same shadow slot). The
19:01:17 family line for the tinted composite is the FSS opening
(the orbital-line note is 19:01:04), not a late learn.

The next build is instruments, not a fix, since the floor's role is
inferred through the composite and the fix wants a number: the learn
says its outcome once, four distinguishable lines; and an eye run
staged while the tracker holds the chrome writes both surfaces whole
as `eye_<stamp>_Chrome0.bin` / `Chrome1.bin` (EDVRTEX1, RGBA8,
3408x1917 expected; `tools/eye_inputs.py` reads them). The tracker
now hands ui_depth the view and the resource it already resolved,
held for the frame (a reference each, released at the frame
boundary), and the copies are taken at the pass. The Frontier ini
carries `ui_depth_alpha = 0.004` for this one flight -- the knob is
live and the same in effect, on the scanner, as the fix candidate (a
scanner-scoped floor of one alpha step, the precedent being the screen
composites' `min(floor, 1/255)` for the comms panel's inactive
icons); what it also does is put depth under the graph's translucent
box, so the stars seen through it ride the head's path and ghost
under the pan, and the loader's scrim moves the ship model with the
dialog. Both are the flight's to judge, with the chrome's alpha
histogram in hand for a floor between the box and the strokes if one
exists.

- Ruled out: the graph's chrome stratum unlearned (b43e3ce's premise),
  because the learn had nothing to add and the mask follows a
  brightness threshold inside one composite.
- Ruled out: the composite declined after classification, because
  every classified draw wrote (`22.8 wrote = 17.7 composites + 5.1
  direct`, "left alone" 0.0 through the scanner).
- Noticed, not fixed: the "scanner is up" stamp fires on the loading
  screen (18:59:53 this log, 13-16 fps, the loader's panel is the same
  world-quad family with a big slot-1 surface); the head's path is
  right there too, so it is harmless, but the note's wording dates the
  loader, not the scanner.

## 2026-09-16: the chrome at one alpha step

Flight on 653671d (`edvr_gfx_20260916_193918.log`, right build), eye
dump 194158, the initial FSS screen, panning WITH the head moving
(0.3-0.7 deg/frame of yaw in the dump), the Frontier ini at
`ui_depth_alpha = 0.004`. Sean: "mostly fixed, it only slightly
shimmers when both moving the panel via the controls and moving my
head at the same time".

The floor was the cause. The learn said the expected line (`the
scanner's chrome surface (2479x1394, DXGI format 27) was already
learned when the tracker offered it (9 surfaces known)`), the run
wrote `Chrome0` (2479x1394 this session; the 3408x1917 of the earlier
census was another session's size, and only one surface was held this
run), and at the label's window (input x 1520-1830, y 1255-1290)
`ui_stroke_probe.py` finds every bright pixel masked (493/493 at 129)
with the head path's vector, the graph's strokes, frame lines, ticks
and cursor likewise, and the box's dark background NOT masked -- the
feared trade-off (stars through a translucent box riding the head's
path) does not arise, because there is no translucent box. The chrome
itself says why, in one histogram (`ui_chrome.py`): 97.7% of the
surface is alpha 0; the dim strokes (luma 20-40) sit at alpha 51-77 of
255, the mid tones (40-80) at 78-221, the labels (140+) at 166-254;
colour never exceeds alpha (premultiplied); no pixel is translucent
fill. Under the 0.5 floor the dim strokes and the graph's label were
clipped and took the sky's path; at one step everything drawn is a
stroke and writes.

The fix, built on this branch and NOT flown: a fifth floor buffer
(`kChromeFloorSlot`) at `min(advanced.ui_depth_alpha, 1/255)` and the
interface proper's floating parity, selected for a composite whose
surface resolves to one the scanner tracker holds this frame
(`samplesScannerChrome`: identity through the view's resource against
`g_chromeHeld`, released at the frame boundary; both eyes and both
strata; zero cost on a frame the tracker matched nothing). The general
floor stays 0.5 for the loader's scrim, and the Frontier ini is back
at `#ui_depth_alpha = 0.5` (the "put back" owed since 19:32). Receipts
are in the Status block's next-flight brief.

The residual, measured in the same dump with `ui_parallax.py` (the
window's content correlated between consecutive raw crops at quarter
pixels, this frame's jitter delta removed -- sign calibrated on the
still-head dump 190129 to <=0.1 px -- against the shift the head rows
predict for a world-fixed point at infinity, 1 m and 220 m):

- The sky (x 900-1900, y 450-690, predicted from the camera rows)
  matches the camera's rotation to +-0.2 px on every frame: the
  projection, the tangents and the rotation are exact.
- The label moves LESS than the head's rotation predicts by
  +0.23..+0.84 px/frame (mean +0.4) at 0.3-0.7 deg/frame of yaw -- a
  3-6% shortfall. A world-fixed panel at 1 m would move MORE than the
  rotation (parallax adds; a flipped translation sign was argued
  against -- `headTv` and `cameraTv` both carry +x for a right turn --
  not proven), so this is not the 220 m remap. The reading at the
  time: the screen itself follows the head a little (a damped follow,
  as Elite's map screens do), which the head path (a world-fixed
  panel) cannot know. Effect: <=1 px/frame of vector error at the
  strokes while the head turns, seen in crop 6 as ~1-2 output px of
  horizontal softening of the moving text; nothing with the head
  still. WITHDRAWN the same evening: the next flight's dump (202857)
  has the label following the head path within +-0.3 px/frame -- see
  "2026-09-16: the one-step floor flown".
- A root-cause fix, had the follow been real, would take the screen's
  own per-frame motion off its composite draw (the post-VS
  screen-motion machinery) instead of the head's delta. Not started,
  and not owed on 202857's reading.

- Ruled out: the projection or rotation scale as the residual, because
  the sky matches the camera rows to +-0.2 px in the same frames.
- Ruled out: the 220 m remap as the residual (in its naive form),
  because a nearer world-fixed panel moves MORE than the rotation and
  the label moves less.
- Ruled out (again, by the chrome itself): the graph's stratum
  unlearned, and the composite declined -- both composites are
  classified and the strokes' alpha is simply under 0.5.
- Noticed: `cameraTv` z (+0.35 mm) and `headTv` z (-0.23 mm) carry
  opposite signs in the same frame; not chased, the residual is a
  rotation-scale effect.
- Noticed, not fixed: the one-step floor rides the tracker, so it
  also reaches the loading screen's panel (see the Status brief).

## 2026-09-16: the one-step floor flown

Flight on 3277ba6 (`edvr_gfx_20260916_202609.log`, right build), the
Frontier ini at its defaults (general floor 0.50 in the ON line), eye
dump 202857 taken while turning the head (0.25-0.5 deg/frame of yaw
after a 4 deg/frame flick at the start) and panning. Sean: "while head
still it was crisp".

Receipts, all present: at 20:27:59 the chrome was learned FROM the
tracker this time (`is learned from the screen's composite ... down to
one alpha step while the surface is held`; the offscreen learner had
not reached it yet, 17 surfaces known by the second offer, which said
`already learned ... one alpha step rather than the general floor
(0.500)`), and both composites got the new family line -- vs A888 with
ps 9107E72CB016CC02 and with the tinted 015EF9349EC097E8: `interface
projection, the scanner's chrome; alpha-aware depth down to one alpha
step into private scene copy for AA`. `Chrome0` (2479x1394) written
with the dump.

The mask, at the general floor: `ui_stroke_probe.py 202857 900 1900
850 1160` shows every element of the chrome masked -- "FILTERED
SPECTRAL ANALYSIS", the spectral bar, its ticks, "HIGH", the cursor
line, the target brackets, the signal marker's ring, "NAV BEACON" and
its icon, "SIGNAL ANALYSIS / HIGH METAL CONTENT WORLD", "FSS SCANNER"
-- and the grid, the stars and the bar's translucent band out of it
(the band is not in the chrome's alpha: an eye-level layer, like the
loader's backing). The treated crops at 3x (`ui_zoomk.py 202857 6
1760 1560 360 70`, and crop 12) show the ticks and the label as crisp
as the raw while the head turns at ~10 px/frame; the star beside them
is slightly softer (world path).

The head-turn residual, re-measured with `ui_parallax.py` on this
dump: the graph's label (x 1010-1185, y 965-995) against the head
rows reads measured minus predicted of +0.16, +1.07, +0.01, -0.30,
-0.12, -0.17, +0.01, -0.34, +0.24, +0.06, +0.20, -0.21 px (mean ~0);
the bar's text (x 1000-1300, y 1150-1195) -0.46..+0.38 with one +2.0
at the end of the flick's deceleration. So the 3-6% shortfall of
194158 (+0.23..+0.84 px/frame, all one sign) is NOT reproduced: in
202857 the panel follows the head path to the measurement's noise,
and "the screen follows the head" is withdrawn as a mechanism -- two
dumps disagree, and the smaller reading is the later one. Meanwhile
the SKY here (x 900-1900, y 1250-1450, camera rows) moves +0.3..+0.57
px/frame MORE than predicted on eleven consecutive frames at corr
0.95-0.98, where 194158 had it exact to +-0.2. Both effects are under
half a pixel a frame and change between dumps; a pose-timing offset
between the rows and the render would do that. Not chased: the UI's
strokes, the complaint, are within noise of the head path.

- Confirmed: the alpha floor was the cause; at one step the whole
  chrome writes and the graph is crisp with the head still.
- Withdrawn: "the screen follows the head" (194158's label shortfall)
  as an established mechanism, because 202857's label and bar text
  follow the head path within +-0.3 px/frame.
- Noticed, not chased: the sky against the camera rows, +0.4 px/frame
  in 202857 under a head turn, exact in 194158.

## 2026-10-15: the tile renderer, per view (instrument and candidate)

Sean (2026-10-15): in the FSS in VR the ring's black-square reveal plays in
the LEFT eye only, as stock. He wants it in BOTH eyes, as the flat screen
shows it, fixed on the engine side. Round 33 already says why the eyes
differ (the engine budgets the primary view's ring tiles); nothing has
yet read the budget. This entry builds the reading and one candidate.

What the code and the logs say (branch `claude/fss-tile-lockstep`):

- READ (census `edvr_gfx_20261014_144320.log`, build 332841, DCX lines):
  the tile renderer is `5998146D464F5C0E` at args+0 and `EB0245DE0BB23BB6`
  at args+12 of ONE argument buffer, and a frame has two such pairs, each
  with its own argument buffer, its own UAV pair (`u0,u1`), its own list
  buffer in SRV slot 2 (a different one for each shader) and its own
  constants. The hashes are current on 332841. Which pair is which eye is
  INFERRED from the order; the instrument below names it.
- READ (Ghidra, build 332841): the ring material's `gPlanetRingScanAlpha`
  is a float at planet-object +0x718, ramped once per frame by
  `FUN_14365b1c0` (RVA 0x365b1c0, dt-driven, up to 1.0 or down to 0.0) and
  copied into the constant buffer by `FUN_143660e50` (RVA 0x3660e50) for
  every view. It cannot differ between eyes. UNKNOWN whether the
  `PlanetRingScan` material (RVA 0x3647a30, index 7) is the tile reveal.
  The tile budget's own code, and the primary-view test, are NOT found.
- `experimental.fss_res` could not have caused this: the stock repro has
  no EDVR (round 21), and the matcher only inflates targets of exactly
  eye/2 (the mono body target); the tile maps are full-eye 272x268.
  INFERRED from `fss_res.h` and the census.
- The exposure engine path (`exposure_chain.h`) copies exposure UAVs
  `u0`-`u4` only; it touches no tile map. The bug predates it (reported
  2026-08-24; the stock repro stands).

The instrument, `fss tiles:` (no key; free outside a zoom window). In each
zoom window (vscreen's arrival window; at most 6 a session, at most 10 s
and 160 lines each) every dispatch of the two shaders is copied, 12 bytes
of arguments, into a staging ring and mapped two frames later with
`DO_NOT_WAIT`. Lines:

    fss tiles: instrument armed -- ...                   (every session, at configure)
    fss tiles: window N opened at frame F (...; lockstep=on|off)
    fss tiles: f=N eye=E cs=<16 hex> groups=(x,y,z) occ=K args=A<id>+<off> srv=[...] uav=[a,b]
    fss tiles: window N closed -- frames= tile dispatches= readbacks= dropped= lines= lockstep=... substituted=... declined(...)
    fss tiles: window N groups -- eye0 large n= x=lo..hi mean= yz<=  | eye0 small ... | eye1 ...
    fss tiles: window N sharing -- view pairs= sameArgsBuffer= sameUav0=; per SRV slot 0-7, same resource in both views / bound in both: ...

`eye` is the order of first appearance of the argument buffer in the frame.
`srv=[...]` shows each slot as `-` (unbound), `T<id>` (a texture) or
`B<id>:<bytes>` (a buffer), ids interned by RESOURCE, so equal ids across
the two eyes mean the eyes share it. No `armed` line: the build is wrong.
`armed` but no `window opened`: no zoom window was reached (the journal
gate and the zoom press are vscreen's). `window opened` and `tile
dispatches=0`: the hashes changed on this build (the closing line lists
the other indirect shaders it saw).

Reading it: if eye 1's `groups` x is far above eye 0's, or eye 0's grows
with the reveal while eye 1's is full, the budget is per view and the
candidate is the right lever. If the two are equal and `sameArgsBuffer` is
0, the budget is not in the arguments: the lists' contents or the
constants carry it, and `tiles` should change nothing visible. If
`sameArgsBuffer` is 1 or `sameUav0` is 1, the views share outputs and the
whole model is wrong.

The candidate, `fix.fss_eye_sync` = `tiles` (alone: no heal, no composite
freeze). In the zoom window, when the PREVIOUS frame had exactly two
argument buffers, eye 1's dispatch of either shader is issued with eye 0's
argument buffer (same offset) and with eye 0's buffer SRVs for every slot
where both views have a buffer of equal size and different resource (the
list; slot 2 on 332841), then eye 1's own SRVs are put back. Textures and
constants stay eye 1's. Every decline is counted and named once a window
(`shape`, `noEye0`, `sameArgs`, `offset`, `noBuffers`). The rig
(`tools\fss_tiles_test`) proves the restore on a fake context and on WARP
with the debug layer, through the production ops. The name is not
`lockstep`: that spelling is the legacy name of the composite freeze
(`sync`) and live inis carry it.

Flight plan (Sean's headset, VR, a ringed planet, Frontier install,
`python tools\edvr_log.py --target frontier --expect-build HEAD --version`
first):

1. `fss_eye_sync = off`. Zoom onto a ringed planet and watch the reveal.
   Expect the stock look: tiles in the left eye only. Read the `fss tiles:`
   lines: this is the baseline of the group counts per eye.
2. `fss_eye_sync = tiles` (live; edit with the Edit tool, diff before and
   after). Zoom again. Expect the reveal in both eyes. Read
   `lockstep engaged at f=` and the `substituted=` count; a `declined`
   reason says which precondition failed.
3. `fss_eye_sync = mirror`, for comparison (the image-level stamp, which
   puts the left's blacks into the right).

Verdicts: `tiles` shows the reveal in both eyes with `substituted>0` and a
clean log: ship the substitution as the behaviour. `tiles` engaged and the
right eye still differs: the budget is not in the lists or arguments;
record `ruled out: eye-1 tile lists and arguments, because the right eye
still differs with them replaced`, and the next lever is the constants
(`cb`) or the producer of the lists. `declined`: read the reason; if it is
`shape`, the frame has more than two argument buffers and the eye numbering
needs the instrument's ids.

Key retirement, if `tiles` works (proposed; nothing removed here):

- Ship the substitution as the behaviour, drop the `tiles` value, and
  propose removing `fix.fss_eye_sync` (`fss_eye_sync = on`; values on, off,
  heal, mirror, sync, steady, tiles) together with the heal and the
  composite freeze it switches, if the tiles make them unneeded.
- Propose removing `advanced.census_fss_jump` (`#census_fss_jump = 0`).
- `experimental.fss_res` (`#fss_res = 0`) and
  `experimental.fss_panel_distance` (`#fss_panel_distance = 1.0`) are
  separate decisions: neither touches the reveal; one is a sharpness cost,
  the other a user preference.

## 2026-10-15 (later): flight f9f62bbc, the budget refuted, the window bug

Flight on test build I (`v0.20.0-rc.1-65-gf9f62bbc`, Frontier, VR, SteamVR;
`edvr_gfx_20261015_061452.log`). Two findings.

**The per-view tile budget is refuted.** ruled out: a primary-view-only
tile budget -- flight f9f62bbc windows 1-2 show equal group counts per eye.
Window 1 (06:17, `on`): eye 0 large mean 437.3 (x 0..509), eye 1 large 452.0;
eye 0 small 71.7, eye 1 small 57.0. Window 2: both eyes a constant 509 large,
0 small. Every frame's large + small is 509 in BOTH eyes (e.g. f11759: eye 0
382 + 127, eye 1 509 + 0): the same amount of tile work, split between the
two shaders differently. Sharing: `sameArgsBuffer=0`, `sameUav0=0`; SRV
slots 0-4 differ per eye, slots 5-7 are shared. A third pair (`eye=2`, small
buffers, args A22) appears in some frames: another consumer of the same two
shaders. So the left eye's black tiles are not a smaller budget. Open
candidates: which tiles each eye's LISTS hold (the split between the two
shaders differs), or the state of the eye's own output (cleared or not at
the zoom). Copying eye 0's args and lists into eye 1 (`tiles`) is therefore
probably the wrong lever and could spread the blacks to the right eye.

READ: the shader declares t2 as a structured buffer of 8-byte entries, t0
and t1 as raw buffers (the 21,936,256-byte and 149,248-byte ones), a cube
array and typed UAV textures u0/u1 (`docs/shaders/tile-renderer-cs.asm`),
and the engine's strings name a tiled deferred shading output (RVA
0x140593850 references `TiledDeferredShadingOutput_Luminance`). INFERRED:
this is the tiled lighting pass, whose output stays black where no tile
writes it.

**The window bug.** With `tiles` the zoom window did not open at the first
attempts (06:19:38-06:21:36) and, when it did (windows 3 and 4, 06:22), it
closed after about 0.9 s. CONFIRMED from `device_hook.cpp`: the zoom-press
watchers, the mode latch and the eager 100 ms Status.json poll run only
while `fssModeLatchWanted`, which was `eyeSyncFromConfig(...).any()`, and
`tiles` leaves `any()` false by design. Without them nothing sets the zoom
press, and the journal's GuiFocus is read slowly, so `journalFssFocus()`
the window needs is stale. Fixed: `EyeSync::needsFssSignals()` (`any() ||
tiles`) feeds both sites. If a zoom is ever signalled and no window opens,
`fss tiles: a zoom was signalled (...) but no window opened: <why>` says
which of: the journal unreadable, the journal not in the FSS, the window
cap, or the key watcher not running. Windows 3 and 4 also show the
candidate engaging (`substituted=162`, SRV slots 0x07) with the previous
frame's shape check declining the first frame only.

Ghidra, the pass itself (build 332841): `FUN_140593850` (RVA 0x593850, 4.9 KB)
allocates the `TiledDeferredShadingOutput_Luminance` and `_Illuminance`
textures from the frame's transient allocator on every call and stores the pair
in the pass object at +0x90/+0x98; its flag byte (bits 1, 2, 4) is gated by
settings bytes at +0xe8..+0xeb. READ: no per-view tile cache and no
invalidation in it. UNKNOWN: whether the allocator clears a recycled texture
and whether the pass object is per view. INFERRED: a tile that no dispatch
writes shows whatever the pooled texture held, which can differ per call.

The instrument, v2 (built, not flown; read-only, inside the same zoom window,
every cap named): per eye per frame it copies the two list buffers (SRV slot 2
of each shader, 2312 bytes) and the tile-info buffer (SRV slot 1, 149,248
bytes) into staging twins and reads them back two frames later with
`DO_NOT_WAIT`. New lines:

    fss tiles: lists f=N split eye0 small=a/T (p%) eye1 small=b/T (q%) | eye0 L n=<nonzero>/<entries> last@<i> hash=<16 hex> first=[w0:w1,..] last=[..] | eye0 S ...
    fss tiles: lists f=N eye1 L ... | eye1 S ...
    fss tiles: info f=N tile-info buffer eye0 bytes= nonzero= hash= | eye1 ... | identical=yes|no|n/a
    fss tiles: write f=N <call> -> <args|output|list|info|t0> A<id> (eye E)
    fss tiles: eye-dump key at f=N -- the raw lists ... of the next 16 frames go to edvr_logs\fsstiles ...
    fss tiles: window N lists -- samples read= lines= raw files written for K frame(s) ...; mean split small/(large+small): eye0 p% eye1 q%
    fss tiles: window N writes -- watched K buffer(s) ...; <call->role xN, ...> | no clear, copy or update touched any of them

Lines appear for the first four frames of a window, one frame in 45, and any
frame whose split moved ten points; at most 120 list lines and 60 write lines
a window. The write watch hooks nothing new: vscreen's existing Clear*,
CopyResource, CopySubresourceRegion and UpdateSubresource thunks offer their
destination (owner context, one bool outside a window). Raw files
`edvr_logs\fsstiles\f<frame>_e<eye>_<listL|listS|info>.bin` are written for
the first three frames of a window and, after the eye-dump key, the next
sixteen (at most 24 frames a window). `armed` / `opened` / the two closing
lines above are printed even when every count is zero, so "nothing happened"
reads differently from "never ran".

The u0 validity mask is not a GPU reduction: the output textures are about
150 MB each and a new precompiled shader means the build's shader tables and
pinned hashes. The eye-dump key (hotkey.dump_eyes, or the menu's "Dump both
eyes as seen") already writes both treated eyes as `eye_HHMMSS_L.bmp` and
`_R.bmp`; the black 16x16 tiles there, read with `tools\diff_eye_dump.py`,
are the mask, set against the raw lists written for the same frames.

Reading it: black tiles equal to the small list's entries in the left eye and
the right eye's small list empty: the classification (a producer upstream of
the dispatch) differs per eye, and the fix lies in what feeds the lists. A
`write` line for eye 0's output at the zoom with none for eye 1: that call is
the lever. Neither: the blacks are in the shader's inputs (the tile-info
buffer or the 21 MB raw buffer), and the next step is their producers.

Flight (Sean, VR, a ringed planet): set `fix.fss_eye_sync = off` (stock is what
is measured; `tiles` is the candidate the budget finding made doubtful). Open
the FSS, press zoom, and within the next ten seconds, when the left eye shows
the black tiles, press the eye-dump key once (the run takes sixteen frames,
one hitch). Do not touch the key again until the log says the window closed.
Repeat once more on the same planet without the key for the timing baseline.
Read: the `window N lists` and `writes` lines, the split lines around the
dump, and the files in `edvr_logs\fsstiles` and `edvr_logs\eyes`.

## 2026-10-15 (evening): flight e2e182a6, the lists read

Flight on test build J (`v0.20.0-rc.1-75-ge2e182a6`, Frontier, VR;
`edvr_gfx_20261015_070826.log`, raw files in `edvr_logs\fsstiles`, eye dumps
`eye_071319_*` and `eye_071341_*`). Read-only analysis; no code changed.

**The A/B was not the one planned.** The ini read `tiles` at window 1
(07:13:10, `lockstep=on`, `substituted=1560`) and Sean flipped it to `on`
from the menu at 07:13:28 (`fix.fss_eye_sync tiles -> on`), so windows 2
(07:13:41) and 3 (07:14:45) ran with the heal and the composite freeze, not
stock. There is no `off` baseline. Dump 071319 is `tiles`; dump 071341 is
`on`. Window 1's dump key came 8.5 s after the press, window 2's 0.4 s after:
neither caught the arrival (about 3 s after the press).

What the lines say (all three windows opened; `fss tiles: window N` lines):

- READ: the two lists PARTITION the whole 17 x 17 grid of 128-pixel cells in
  both eyes (8-byte entries `(x, y)`, sentinel `0x11:0x11` after the valid
  ones): at f28842 eye 0 and eye 1 both hold 136 cells in the large list (rows
  9-16) and 153 in the small (rows 0-8), 136 + 153 = 289, no cell twice and
  none missing. The tail the dispatch skips (about 12 % of each list: the
  first `groups * 128 / 256` entries run) is the same set of interior cells in
  both eyes (e.g. large (10,14), (3,12), (12,14), (1,12); small (11,2), (8,5),
  (2,3), (6,7)).
- READ: the SRV-slot-1 buffer (14,336 bytes, 3,584 words) is not a tile table:
  it repeats a 23-word record of floats and masks (about 155 records), 9.4-9.8 k
  non-zero bytes, never identical between the eyes (2,768 bytes differ at one
  frame) and moving 4.7-5.0 k bytes frame to frame. INFERRED: per-view light
  data, culled per view.
- READ: the writes are the same in every frame and both eyes:
  `ClearUnorderedAccessViewUint` on each eye's argument buffer and both lists
  (2 args + 4 lists a frame over the two eyes) and `ClearRenderTargetView` on
  each eye's two outputs (4 a frame): 1,556 / 3,112 / 3,112 in window 1. No copy, no
  update, nothing at the zoom, nothing for eye 0 alone.
- READ: window 3, the zoom itself (f34169-34172): the SPLIT BAND differs by a
  row between the eyes. f34170: eye 0 large 136 cells, small 153 (groups
  240 / 269); eye 1 large 152, small 137 (groups 268 / 241). f34171 and on:
  both eyes large 0, small all 289 cells (groups 0 / 509). Window 2 (on) moves
  the same way over 600 frames: eye 0 large x 127..255 and small 254..382, eye
  1 large 210..305 and small 204..299 (mean small share 60.3 % against
  52.0 %). Large + small is 509 in every frame of both eyes.
- READ, the dumps (16 consecutive crops, 1400 px about the centre, 16 x 16
  blocks that are black with lit blocks on both sides in time): window 2 dump,
  left 21 at its worst frame and right 35; window 1 dump (tiles), left 78 at
  one frame and right 16, 24 against 30 at another. Both eyes flicker; the
  centre crop sees different content in the two eyes (the body is at
  infinity), so the counts are not a paired measurement.

Classification of the cause:

- (b) a clear or copy hitting eye 0's output at the zoom: ruled out: every
  frame clears both eyes' args, lists and outputs the same way, and nothing
  else writes to them (windows 1-3).
- (a) per-eye classification: the cells are all covered in both eyes, so
  nothing is MISSING from a list; what differs is the row at which the cells
  change from the large to the small shader, by a whole row at the zoom
  (f34170) and over the window. SUPPORTED, not proven to make the squares: a
  band of 16 cells is shaded by the large shader in one eye and the small in
  the other for a frame or two.
- (c) the shader inputs: slot 1 is per-view light records, not a validity
  table; slot 0 (21,936,256 bytes) and the cube array are untested.
- (d) the dumps do not show the left-only squares: the eye textures flicker
  in both eyes (round 30b again), and the arrival was not captured.

ruled out: an eye-0-only clear or copy of the tile outputs, lists or
arguments at the zoom, because windows 1-3 show the same per-frame clears for
both eyes and no other write.
ruled out: a cell missing from one eye's lists, because both lists partition
all 289 cells in both eyes and the skipped tails match.
ruled out: the SRV-slot-1 buffer as a per-tile validity table, because it is
a repeating 23-word record array that differs per eye by content.
not tested: `fix.fss_eye_sync = off` (Sean ran `tiles`, then `on`), and any
frame at the zoom's arrival.

Fix, if the band is the cause: the engine's per-view band row must agree
between the eyes; the `tiles` candidate does exactly that at the dispatch
(eye 1 draws from eye 0's arguments and lists) and its window 1 never moved
(240 / 269 in both eyes for all 779 frames), so it has not been tested at an
arrival. Next instrument (one flight): arm the capture on the arrival
signature instead of a keypress: when either eye's large list changes by a row,
or both go to zero (the f34171 event), log `bandRow` and the large count per
eye for that frame and the 16 around it, write the raw lists for them, and
arm the eye run once from the code (`temporalPassArmEyeDump`) so the dump
brackets the event. Flight: `fix.fss_eye_sync = off` (check the log line
`fss tiles: window N opened ... lockstep=off` and no `fss eye heal` line),
zoom a ringed planet, do not press the dump key; then the same with `tiles`.

## 2026-10-15 (night): the band watch, v3 (built, not flown)

Sean approved the next instrument after flight e2e182a6 (the lists partition
every cell in both eyes; what differs is the row where cells change from the
large list to the small one's, a whole row apart at the zoom, and at the
arrival every cell goes to the small list). It arms on that event, not on a
key. Read-only apart from the files it writes and the eye run it starts.

What it does, inside every zoom window (the same caps as v2):

- From the lists it already reads every frame it computes, per eye, the
  large list's valid cells (entries other than `(17, 17)`), its boundary row
  `17 - cells / 17` and its smallest row, and the small list's cells.
- The event is the first frame of a window (one event a window, at most 2
  automatic eye runs a session) in which either eye's large list moves by a
  whole row (17 cells) or more since the previous frame, or goes to zero. The
  eyes merely disagreeing is not an event: they disagreed for all of window 2.
- At the event it logs the 10 frames before, the event frame and the 60 after
  as `band` lines, writes those frames' raw lists (`f<frame>_e<eye>_listL|listS.bin`,
  edvr_logs\fsstiles) and starts the 16-frame eye run at the next frame
  boundary (the Present tick, where the key's own press runs), no key.

Lines:

    fss tiles: instrument armed -- ... it also watches the lists for the band event and starts the eye run itself (at most 2 a session, no key). ...
    fss tiles: window N opened ... Band watch: armed, waiting for a whole-row move of a large list or one going to zero (automatic eye run, at most 2 a session).
    fss tiles: band event at f=N (<reason>): eye0 L=a (row r), eye1 L=b (row s). The 10 frames before, this one and the 60 after are logged as `band` lines ...; the eye run starts at the next frame boundary (automatic run K of 2 this session).
    fss tiles: automatic eye run started at f=N (the band event); ...
    fss tiles: band f=N eye0 L=136 (row 9.00, minY 9) S=153 | eye1 L=152 (row 8.06, minY 8) S=137 | own row gap 0.94 [| ran: eye1 drew eye0's lists (row 9.00)]
    fss tiles: window N band watch -- B band event(s) (automatic eye runs started this session: K of 2); large-list size over the window eye0 a..b eye1 c..d cells, largest one-frame step eye0 X eye1 Y (an event needs a step of 17, ...); band lines n, raw frames written m

The window's band-watch line is printed whether or not an event happened, so
"no event" (the steps it saw are in the line) reads differently from "never
ran" (no `armed` line, or `armed` with no `window opened`).

Flight plan (Sean, VR, a ringed planet, no keys; two zooms in one session or
two sessions):

1. Set `fix.fss_eye_sync = off` in the ini before the game starts (not from
   the menu: flight e2e182a6 flipped it mid-session). Check the log head: no
   `fss eye heal: armed` and no `fss reveal: ON` line, and the instrument line
   says the candidate is `off`. Zoom onto a ringed planet, press nothing else,
   watch the reveal until it settles. Note which eye shows the black tiles.
2. Set `fix.fss_eye_sync = tiles` and zoom onto the same planet the same way.
   The window line must say `lockstep=on` and a `lockstep engaged` line must
   follow in the first frames.

Reading rules:

- Did it fire: each zoom should log one `band event` and one `automatic eye
  run`. `largest one-frame step` below 17 with no event means the lists never
  moved by a row (no arrival reached); zero `window opened` means the zoom
  window never opened (the line `a zoom was signalled ... no window` says why).
- `off`: in the `band` lines before the event, is the own row gap non-zero
  for several frames (one eye's boundary row ahead of the other's)? Take the
  eye run's `FinalCrisp_L/R` images (frame numbers in `eye_*_FinalCrisp.json`),
  mark 16 x 16 blocks that are black with lit blocks on both sides in time
  (cell x = (1316 + px) / 256 for the 1400 crop, the same for y), and set them
  against the cells of the two lists for that frame (raw files; a cell's list is
  the one that holds `(x, y)`). Black blocks falling in the cells the two eyes
  assign to different shaders, in the eye that shows them, confirm the band as
  the cause. No such correspondence, or the same black blocks in both eyes:
  ruled out as the source of the left-only squares.
- `tiles`: the band line carries `own row gap` (the engine's own, still the
  eyes' difference) and `ran: eye1 drew eye0's lists (row r)`. Both eyes were
  on the same row on a frame exactly when that suffix is present, and the
  row printed is eye 0's. A frame without the suffix drew eye 1's own list (a
  declined frame: the window's `declined(...)` counts say which reason). The
  read-back group counts of eye 1 are its OWN arguments read before the
  substitution, so they do not show what the dispatch used. `tiles` works if,
  with the suffix present on the frames around the event, the eye run shows
  the squares in neither or in both eyes alike; it does not if the left-only
  squares survive with `ran` present on every frame.
- Either run: compare the two events' frame counts from the zoom press to the
  event (about 3 s at 90 Hz is the arrival) and the large-list sizes before
  the event; a row event early in the transit is not the arrival.

## 2026-10-15 (late): flight bca9d6b8, `tiles` does not fix it

Flight on test build K (`v0.20.0-rc.1-85-gbca9d6b8`, Frontier, VR), two
launches, no keys. `edvr_gfx_20261015_075527.log` is `off` (the instrument
line says the candidate is off, `lockstep=off`); `edvr_gfx_20261015_075814.log`
is `tiles` (`lockstep=on`). Sean: "tiles doesn't fix it still". Read-only
analysis.

Sean, on both runs: only the LEFT eye shows the black squares; the right eye
shows the ring already filled (confirmed after the flight).

Did it run:

- `off`: window 1 opened at f12037; the band event fired at f12039 (a large
  list went to zero), the eye run started at f12042 (frames 12043-12058 in the
  dump). The zoom window then closed after 21 frames (f12058).
- `tiles`: window 1 (f11431, 901 frames) never left the static 136 / 136 state,
  no zoom arrival, no event. Window 2 (f12837): `lockstep engaged` at f12838,
  `substituted=634`, `declined(shape=44)` (frames after one with a third
  argument-buffer pass), band event at f13152 (a large list moved by a whole
  row: eye 0 136 -> 225, eye 1 181), large lists to zero at f13155, eye run from
  f13156. All 34 band lines carry `ran: eye1 drew eye0's lists`.

What the rows say:

- READ: the row mismatch is short. `off`: f12037-12038 eye 0 L=226 (row 3.71),
  eye 1 L=187 (row 6.00), gap 2.29 rows; f12039 on both eyes L=0, S=289 (all
  cells small) and the own row gap is 0.00 for every later frame. `tiles`:
  f13152-13154 eye 0 225 / 226 against eye 1's own 181 (gap 2.59-2.65), eye 1
  drew eye 0's (`ran` row 3.76); from f13155 both L=0 and the gap 0.00. From
  the all-small state on, the two eyes' lists are the same size and the engine
  itself puts them on one row, so the substitution has nothing to change.
- READ, the eye runs: both runs captured the transit void. `off`: frames
  12043-12054 are black in both eyes (maximum luma 0-1), and the reveal starts in
  the last four frames (16 x 16 blocks lit, left 310, 448, 563, 678; right 369,
  490, 572, 635). `tiles`: all 16 frames (13156-13171) black in both eyes. The
  windows closed (f12058, f13176) as the reveal began, so neither the instrument
  nor the dump saw it. No left-only squares were measured in these runs.

Classification: the large/small row mismatch is NOT the lever. ruled out: the
large/small list row (the `tiles` substitution) as the source of the left-only
reveal, because the mismatch lasts two or three frames before the all-small
state, the lists are identical in size from then on in both eyes, `tiles` ran
with `ran` present on every frame and Sean still sees the reveal in one eye.
ruled out: the tile pass's schedule (group counts, lists, arguments) as the
source, for the same reason and flight e2e182a6's symmetric clears. UNKNOWN
but supported: the blacks come from the per-eye INPUTS of the tile dispatch,
not its schedule: SRV slots 0 (21,936,256 bytes), 1 (light records), 3 and 4
(per-eye textures, T5 / T6 in the lines) differ per eye, while 5-7 are shared.
INFERRED: slots 3 and 4 are the ring's offscreen G-buffer, which round 27
measured as a budgeted subset of 16 x 16 tiles per frame over a cleared target
(unbudgeted tiles sample zero normals and shade black); the producer is then
the per-eye ring scene draws, upstream of this pass.

Next, ranked by evidence:

1. `mirror` now, no build: `fix.fss_eye_sync = mirror` (set in the ini before
   launch). It stamps the left eye's hard-black pixels into the right at the
   infinity shift, so the reveal plays in both eyes, as the flat screen shows
   it. It exists, is supported on the native path (`healMode 2`: target the right
   eye, donor the left; `native_fss.cpp`) and was the old round-39 answer; it is
   gated by the zoom window and the scanner chrome, so it acts while the
   scanner is up. Not flown on this build. It is the practical answer to "the
   animation in both eyes" while the engine cause stays open. Cost: a misread
   pixel stamps black on a dark surface (near invisible).
2. The ring draws, no build: `advanced.census_fss_jump = 1` arms a census at
   the zoom's first frame; read it for the per-eye ring draws (the 13,248-vertex
   ring mesh, the ring quad and mesh draws of rounds 16-18) and their instance
   counts and targets, and compare the eyes at the arrival and during the reveal.
   The census covers a fixed number of frames from the press, so check that it
   spans the 13 frames after the all-small event.
3. A longer window (small build): keep the band watch and the lists capturing
   for 600 frames after the all-small event, log the per-eye identity and a
   sampled hash of slots 3 and 4 and of slot 0 in that stretch, and start the
   eye run 20 frames after the event so it brackets the reveal's start.

## 2026-10-15 (late night): the reveal window, the inputs and the ring's draws, v4 (built, not flown)

Sean confirmed on both flight bca9d6b8 runs (`off` and `tiles`): only the LEFT
eye shows the black squares, and the right eye shows the ring already filled.
So the squares are drawn into eye 0's image only, with the tile schedule (group
counts, lists, arguments) ruled out. The instrument now looks where the
schedule's inputs come from. Read-only; the Ghidra side is another agent's.

What v4 adds (inside the same zoom window, every cap named):

- THE WINDOW. The all-small band event (a large list going to zero, the f12039
  / f13155 event of flight bca9d6b8) now also starts a reveal capture: the
  instrument keeps capturing for 600 frames after it, whatever the zoom window
  does (the journal window had closed as the reveal began in both runs). Only
  the all-small event arms it; a row move is no longer an event. Two automatic
  16-frame eye runs, at 20 and 150 frames after the event, so one brackets the
  reveal's start and one its middle; both count against the 2-a-session cap, so
  one zoom per session uses them.
- THE INPUTS. Every 10th frame, per eye, for the large dispatch: SRV slot 0
  (the 21,936,256-byte buffer), slot 3 and slot 4 (the per-eye textures) and
  slot 6 (the cube array). Identity, size and format always; contents sampled
  coarsely and safely: 16 regions of 256 bytes spread over slot 0, and for a
  colour texture an 8 x 8 grid of 16 x 16-pixel boxes (the squares' own size)
  copied into a 128 x 128 staging twin, each box shown as one character (`.` all
  zero, `#` all non-zero, `1`-`8` the share). Depth-stencil, multisampled and
  unknown formats are named, not copied; the full 21 MB is never read.
- THE DRAWS. The draws that render into slot 3 and slot 4's textures (render
  target 0, found from the binding shadow's render target) are counted per eye
  and per frame by every draw hook (one bool outside a window): draws,
  instances, and the viewport and scissor of the first and the last sampled draw
  (the first two draws of a frame and one in sixteen after). Clears, copies and
  updates into those textures are counted by the existing write watch under the
  roles `slot3` and `slot4`.

Lines (new ones only):

    fss tiles: band event at f=N (a large list went to zero ...): ... Capture continues until f=M (600 frames); 2 automatic eye run(s) at f=A and the second later (planned 2 of 2 this session); input and gbuf lines every 10 frames.
    fss tiles: input f=N eyeE | slot0 B<id> <bytes> bytes nz16=<16 chars> | slot3 T<id> WxH fmtF nz8x8=<rows/of/8> | slot4 T<id> WxH fmtF nz8x8=... | cube T<id> WxHxL fmtF
    fss tiles: gbuf f=N eyeE slotS T<id> draws=n instances=m viewport first (x,y WxH) last (...) scissor first (l,t,r,b) last (...)
    fss tiles: window N upstream -- draws into the per-eye textures of slots 3 and 4: eyeE slotS: frames with draws a, draws b (per frame lo..hi), instances c, viewport changes d | ...
    fss tiles: window N inputs -- fingerprints read=n lines=m; sampled grids that differ between the eyes: slot0 a of b, slot3 c of d, slot4 e of f frames; gbuf lines g

Both closing lines are printed whether or not anything matched (the upstream
line says when no render target matched slot 3 or 4's resources), so
"nothing drawn there" reads differently from "never ran".

Flight plan (Sean, VR, a ringed planet, no keys, `off` only):

1. Set `fix.fss_eye_sync = off` in the ini before launching. Check the log head:
   no `fss eye heal: armed`, no `fss reveal: ON`, and the instrument line says
   the candidate is off.
2. Zoom onto a ringed planet once and watch the reveal through to the end
   (about ten seconds). Press nothing. Say which eye shows the squares (flight
   bca9d6b8: the left) and roughly when they start and stop.
3. Read: `band event` (the zoom's frame), then `input`, `gbuf` and the two
   `window N` lines; the eye runs are `edvr_logs\eyes\eye_<stamp>_FinalCrisp*`.

Reading rules:

- Inputs: take the `input` lines of the reveal (from the event on). A sampled
  grid whose `.` / digit pattern differs between eye 0 and eye 1 for slot 3 or
  slot 4, with eye 0's mostly `.` (unwritten) where eye 1's is filled, says the
  per-eye G-buffer is the lever: eye 0's texture is not fully written when the
  tile pass reads it. `window N inputs` counts the frames the grids differed.
  Identical grids and identical identities for slot 0 and slot 6 across the
  eyes rule the inputs out as the difference.
- Draws: eye 0's draws into slot 3 / slot 4 per frame against eye 1's
  (`upstream` line, and the `gbuf` lines frame by frame): fewer draws, or a
  smaller viewport or scissor, for eye 0 during the reveal is round 27's budgeted
  subset made visible, and the producer is the ring's offscreen draw, not the
  tile pass. Equal draws and equal regions with different grids point at what
  the draws read (their own inputs) instead. No match at all means the
  textures are not drawn into through render target 0 (a compute or another
  slot), and the next look is the write watch's `slot3` / `slot4` counts.
- The eye runs: with the reveal inside the capture, black 16 x 16 blocks in
  eye 0's `FinalCrisp_L` frames against eye 1's `_R` (the method of the
  flight e2e182a6 entry), set against the grids at the same frames.

## 2026-10-15: engine RE of the ring reveal (Opus)

Read-only static analysis of build 332841: Ghidra (EDAnalysis, `-readOnly
-noanalysis`, scratch scripts outside the repo) and capstone on the Epic
exe. Its PE stamp is 1788384820, the flight build's; its file size is
103,469,056, not the 104,894,464 quoted for the Frontier exe (same stamp,
so the same code: INFERRED). No flight, no code change. Observed fact for
this entry (Sean, flight bca9d6b8, `off` and `tiles`): only the LEFT eye
shows the black squares, and the right eye shows the ring already filled.

**Verdict: the root cause is not settled statically.** No tile budget,
dirty-tile list, progress counter or view-0-only reset was found on the
CPU side of the ring or the tile pass. What the reading did settle is below.

Corrections to the entries above:

- **Slots 3 and 4 are not a ring G-buffer.** CONFIRMED from
  `docs/shaders/tile-renderer-cs.asm`: t3 is a float 2D texture loaded as
  one channel at the pixel (the scene depth) and t4 is the uint
  `X32_G8X24` view of the same R32G8X24 depth (its stencil; census format
  22). They are each eye's own depth-stencil and differ per eye by
  construction. The shader's G-buffer is t8-t11 (four float 2D textures),
  and `fss tiles:` reads SRV slots 0-7 only (`kSrvSlots = 8`,
  `src/d3d11/fss_tiles_core.h:29`), so the per-eye G-buffer the dispatch
  shades has never been compared. ruled out: SRV slots 3-4 as the ring's
  offscreen G-buffer, because they are the eye's depth and stencil.
- `FUN_140593850` does not allocate its outputs on every call. It caches
  them on the pass object (+0x90/+0x98/+0xa0 luminance, +0xa8/+0xb0/+0xb8
  illuminance) and reallocates only when the target size changes. Tile
  size 16 px (8 when `DAT_145e70f7c` is set). No budget or invalidation in
  it; the dispatches are issued by `FUN_140580f40` (not read). CONFIRMED.

How the engine tells views apart (CONFIRMED unless marked):

- A camera is a 0x900-byte object (ctor `FUN_1406bdce0`). +0x8e8 is its
  camera ID, a bit index taken from an allocator mask (written at
  0x6bde5b, set to -1 at 0x6bdd91); +0x8e0 is `1 << ID`.
- "Main camera": `renderer->vtbl[0xa8]()->vtbl[0x70]()` returns an ID that
  code compares with camera+0x8e8.
- camera+0x8f8 points at the owning pipeline's settings block
  (pipeline+0x70, written by `FUN_1428a4d30`). Block+0x10 is the stereo
  role: 1 first eye, 2 second eye, 0 mono (INFERRED from its one use
  below). Block+0x17 gates the PlanetRingScan and HotSpots draws
  (`FUN_1436558a0`); block+0x19 gates the atmosphere passes
  (`FUN_143654ff0`).

| RVA | What | Per view? | Status |
|---|---|---|---|
| 0x36558a0 | ring pass entry: pass type `edx` (0, 3, 6, 7 seen), camera `r8`, render data arg 8; picks draws; pass 7 with ring+0x720 and `[data+0x198]+0x6c` draws PlanetRingScan (mode 3), pass 3 with `+0x80` the hot spots | every camera | CONFIRMED |
| 0x36597f0 | ring material draw; mode 0 PlanetRing3/Miniature, 1 Fog, 2 VolumetricFog (GapFill when the pass is `renderer->vtbl[0x88]()->vtbl[0x40]()`), 3 PlanetRingScan | every camera | CONFIRMED |
| 0x3659a1d, 0x3659f78 | `cmp [rdi+0x8e8], eax; jne` inside 0x36597f0: in body mode 2 (data+0x104 == 2) with ring+0x6b0 set and `DAT_145e89355`, only the main camera draws the ring, into the offscreen target ring+0x6b0 with the FSS camera substituted; other cameras return | main camera only | CONFIRMED code; that it is Finding 1's mono body layer is INFERRED |
| 0x36540b0 | pass-6 ring draw: per-camera-ID float4 array at ring+0x640/0x650/0x658 (stored up vector). Role 1 writes its ID to ring+0x714 (0x365412c); role 2 copies role 1's entry and skips its own update (0x3654140) | role-aware | CONFIRMED |
| 0x36ea4ac | StellarBodyEffects per-view prepare (`FUN_1436ea000`): main camera only runs `FUN_14363d220` (close-body slot assignment with fade timers, planet+0x838..0x8f8) and copies planet+0x9c0..0x9d0 into atmosphere+0x7e0 and ring+0x700..0x710 | main camera only | CONFIRMED |
| 0x365b1c0 | scan alpha ramp ring+0x718 (direction +0x71c, active +0x720); also clears ring+0x6b0 every frame (0x365b1c6). Called once a frame from `FUN_1436e8430` | global, once a frame | CONFIRMED |
| 0x3660e50 | ring constants, every view; the alpha and the 0x700 block are read, never advanced | every camera | CONFIRMED |
| 0x36cf1f0 | SplineCuttingEffects: a GPU work queue (count +0xb4) dispatched and zeroed in the main camera's pass. Unrelated to the ring; it shows the engine's main-camera-consumes-the-queue pattern exists | main camera only | CONFIRMED |

The six questions:

1. What feeds slots 3-4: the eye's depth and stencil (above), per view,
   one pass object per pipeline. No CPU-side budget was found for the ring
   G-buffer; round 27's "budgeted tile subsets" has no counterpart in the
   code read. UNKNOWN where the subset comes from; the uncompared t8-t11
   are the first place to look.
2. Primary-only logic: four main-camera tests and one role test, table
   above. None is a tile budget or a consumed counter on the ring path. The
   two that touch the ring and run for the left eye only are the mode-2
   offscreen draw (0x3659a1d / 0x3659f78) and the prepare block (0x36ea4ac).
3. Flat: one camera, role 0, and it is the main camera, so every
   main-only branch runs for it and no role-2 copy happens. CONFIRMED
   structurally. This is consistent with a main-only producer but does not
   name one.
4. The reset at zoom arrival: not found. ring+0x6b0's setter is not in
   the ring module (59 writes of +0x6b0 in .text; the ring's are the ctor
   at 0x3638a69 and the per-frame clear). UNKNOWN.
5. Fix: none proposed; the mechanism is unproven. Hook points ready once
   the observation names the branch, each pinned by RVA and prologue:
   - `FUN_1436558a0` 0x36558a0, `40 55 57 41 54 41 55 41 56 48 81 EC 90 00 00 00`.
   - `FUN_1436597f0` 0x36597f0, `40 55 53 56 57 41 54 41 56 41 57 48 8D AC 24 30 FF FF FF`.
   - the main-camera tests at 0x3659a1d and 0x3659f78 (`39 87 E8 08 00 00 0F 85`):
     a call-site wrap that answers "main" for the second eye would draw the
     ring into the offscreen layer twice a frame from the FSS camera. Risk:
     double cost and a race on the one target; only worth it if the
     observation shows the left eye's squares come from that target.
   - `FUN_1436540b0` 0x36540b0, `48 89 5C 24 10 48 89 6C 24 18 4C 89 4C 24 20`
     (role copy at 0x3654140, `83 FF 02 0F 84`). Low risk; it only changes
     the distant ring's stored up vector.
6. The single observation: a read-only CodeHook at 0x36558a0 that, inside
   the zoom window, logs per call the frame, `[r8+0x8e8]` (camera ID),
   `[[r8+0x8f8]+0x10]` (role), `edx` (pass), `[[rsp+0x40]+0x104]` (body
   mode), `[rcx+0x6b0] != 0` and `[rcx+0x718]` / `[rcx+0x720]`, and once a
   window the main-camera ID. Reading: if in the arrival frames the left
   eye's calls take mode 2 (offscreen) or a pass the right eye's do not,
   the asymmetry is on this path and the table names the branch to patch.
   If both eyes run the same pass and mode sequence, the CPU ring path is
   symmetric, and the next measurement is the dispatch's G-buffer (widen
   `fss tiles:` to slots 8-11).

## 2026-10-15 (v5): the G-buffer t8-t11 and the ring pass observer (built, not flown)

The engine RE above corrected v4: SRV slots 3 and 4 of the tile dispatch are
each eye's scene depth and its stencil view (CONFIRMED, `tile-renderer-cs.asm`),
not a ring G-buffer. The G-buffer the pass shades is t8-t11, which v4 never read
(`kSrvSlots = 8`). v5 fixes the instrument and adds the RE's one observation.
Read-only throughout; no key.

What changed:

- THE SLOTS. The dispatch's SRV capture now covers slots 0-11
  (`kCaptureSlots = 12`; the substitution and the per-dispatch lines keep slots
  0-7). The `input` fingerprints (every 10th frame, through the 600-frame
  reveal capture) now read, per eye: slot 0, slot 3 and slot 4 (relabelled
  `depth(slot3)` and `stencil(slot4)`, identity only: they are depth-stencil and
  are not copied) and t8, t9, t10, t11 (identity, size, format and the 8 x 8 grid
  of 16 x 16 boxes, as v4 did for slots 3 and 4). The draw count and the write
  watch now follow t8-t11 (roles `t8` to `t11`), per eye: draws, instances,
  viewport and scissor of the first and last sampled draw, through render target
  0 (a G-buffer pass that binds the four as render targets 0-3 is caught by
  whichever is target 0).
- THE RING PASS OBSERVER. A read-only CodeHook at EliteDangerous64.exe+0x36558A0
  (`FUN_1436558a0`, the ring pass entry), VR profile only, installed at startup
  with the vertex resync's relay. It is pinned by build identity (PE stamp
  1788384820 and image size 104894464, the pair every build-keyed hook checks)
  and by the prologue `40 55 57 41 54 41 55 41 56 48 81 EC 90 00 00 00` from the
  RE's hook-point table (three whole instructions, five bytes stolen). Inside the
  zoom/reveal capture it logs per call the camera ID `[r8+0x8e8]`, the stereo
  role `[[r8+0x8f8]+0x10]`, the pass `edx`, the body mode `[[rsp+0x40]+0x104]`
  (the render data's `+0x104`), whether `ring+0x6b0` is set and `ring+0x718` /
  `+0x71c` / `+0x720`; once a window the main camera's ID, asked the way the ring
  code asks (`[ring+0x48]`, vtable `0xa8`, then `0x70`; two getter calls under
  SEH, a fault absorbed and never retried). Every argument is forwarded
  unchanged; nothing is written. If the build or the prologue differs, or
  CodeHook refuses, it logs `ring hook inert -- <why>` once and stays out.

Lines (new ones only):

    fss tiles: ring hook armed at EliteDangerous64.exe+0x36558A0 (the ring pass entry, build 332841), stolen=5 bytes, prologue 16/16 bytes verified. Read-only: ...
    fss tiles: ring hook inert -- <why>. (the build differs / the bytes differ / no memory in range / CodeHook refused; or nothing at all in the flat profile)
    fss tiles: ring main camera id=N (window W, f=F; read through [ring+0x48] vtable 0xa8 then 0x70, the way the ring code asks)
    fss tiles: ring f=F call=K cam=C role=R pass=P mode=M ring+0x6b0=set|clear alpha=A dir=D active=B (first frames|new combination|sample)
    fss tiles: input f=N eyeE | slot0 B<id> <bytes> bytes nz16=<16> | depth(slot3) T<id> WxH fmtF (not sampled: ...) | stencil(slot4) T<id> ... | cube T<id> ...
    fss tiles: input f=N eyeE G-buffer | t8 T<id> WxH fmtF nz8x8=<rows/of/8> | t9 ... | t10 ... | t11 ...
    fss tiles: gbuf f=N eyeE tK T<id> draws=n instances=m viewport first (...) last (...) scissor first (...) last (...)
    fss tiles: window N upstream -- draws into the per-eye G-buffer textures t8-t11 (render target 0): eyeE tK: frames with draws a, draws b (per frame lo..hi), instances c, viewport changes d | ...
    fss tiles: window N inputs -- fingerprints read=n lines=m; sampled grids that differ between the eyes: slot0 a of b, depth ..., t8 ..., t9 ..., t10 ..., t11 ... frames; gbuf lines g
    fss tiles: window N ring hook -- armed, T ring-pass call(s) since it armed, W inside this window's capture, K distinct combination(s): cam=.. role=.. pass=.. mode=.. 6b0=.. xN; ...; call lines n; main camera id known|not read|unreadable

A hook that never ran is told from one that saw nothing: the closing line says
`the entry has NOT been reached this session` when its total call count is zero,
`none inside this window's capture` when calls happen outside it, and
`inert (<why>)` when it never armed. Call lines are the first 12 frames of a
window, every new combination, then one frame in 10; at most 200 a window.

Flight plan (Sean, VR, a ringed planet, `off` only, no keys):

1. Set `fix.fss_eye_sync = off` in the ini before launching. The log head must show
   `ring hook armed` (an `inert` line means the build or the prologue differs: the
   rest of the instrument still runs) and no `fss eye heal: armed`.
2. Zoom a ringed planet once and wait for the reveal to finish (about ten
   seconds). Press nothing. Say which eye shows the squares.
3. Read the `ring`, `input ... G-buffer`, `gbuf` and the three `window N` lines.

Reading the hook (the RE's table, restated). Compare the left eye's camera
(the one with the squares) with the right's in the frames from the all-small band
event on; the main camera ID printed once per window says which camera the
main-only tests treat as primary:

| What the `ring` lines show | Means | Next |
|---|---|---|
| Only the main camera's calls have `mode=2` and `ring+0x6b0=set` in the reveal frames; the other camera's calls have mode 0 or `clear` | The offscreen mono body layer is drawn for the main camera only (the `0x3659a1d` / `0x3659f78` tests): the squares are that layer's reveal, seen by one eye | Patch those two tests to answer "main" for the second eye; it draws the layer twice a frame from the FSS camera (double cost, a race on the one target), so only if the G-buffer lines agree |
| The two cameras run the same passes and mode, with different `alpha` or `active` | The scan alpha ramp (`0x365b1c0`) is read at different moments, not different code | Not the lever for the squares; look at what advances it |
| Role 2's calls lack a pass role 1's have (3 or 7, the hot spots and PlanetRingScan) | The role-aware path (`0x36540b0`, block+0x17) | Hook `0x36540b0`; low risk |
| Both cameras run an identical pass / mode / `6b0` sequence | The CPU ring path is symmetric | The G-buffer t8-t11: see below |
| `ring hook inert`, or `0 ... call(s)` with `NOT been reached` | The observer did not run; nothing is learned about the ring path | Fix the pin (the prologue, the identity), not the reading |

Reading t8-t11 (the `input ... G-buffer` lines, the `gbuf` lines, `window N
upstream` and `inputs`):

- The grids: a `t8`-`t11` grid that is mostly `.` (unwritten) for eye 0 where
  eye 1's is filled or partly filled, in the reveal frames, says eye 0's G-buffer
  is incomplete when the tile pass reads it: the squares are unwritten G-buffer
  tiles, and the producer is the next question. Identical grids and identities
  for both eyes rule the G-buffer's CONTENT out as the difference (the sampling
  is coarse: 64 boxes of 16 x 16 pixels).
- The draws: `upstream` and the `gbuf` lines give each eye's draws, instances and
  viewport/scissor into the four textures per frame. Fewer draws or a smaller
  region for eye 0 during the reveal is round 27's budgeted subset made visible
  and the producer is the ring's offscreen draw. Equal draws with different grids
  point at what those draws read. `no render target of the window matched them`
  says the G-buffer is not drawn into through render target 0 (a compute or
  a UAV write): then the write watch (`writes` line, roles `t8`-`t11`) and the
  RE's next hook are the way.
- Slots 3 and 4: identity only. They are each eye's depth and stencil and differ
  per eye by construction; do not read the squares into them.

## 2026-10-15 (v5 flown): flight db982c64, the ring pass is not on the reveal's path

Flight on test build M (`v0.20.0-rc.1-93-gdb982c64`, Frontier, VR;
`edvr_gfx_20261015_092018.log`; `--expect-build db982c64` matches; eye runs
`eye_092239_*` and `eye_092243_*`). `fix.fss_eye_sync = off` (window 1
`lockstep=off`), one zoom onto a ringed planet, no keys. Sean: the LEFT eye
shows the black squares and the RIGHT eye shows none. On this rig the eyes
render at 2016 x 2016, and the temporal pass outputs 4032 x 4032. Read-only
analysis.

Did it run: window 1 opened at f14043, and the band event (both large lists
to zero) fired at f14045. The 600-frame reveal capture ran to f14647 with two
automatic eye runs (f14067 and f14197). The ring hook armed at startup
(prologue 16/16) and counted **33,600 calls since arming, none inside the
605-frame capture**. The closing line's branch for that is driven only by
calls that arrive while the capture flag is set, and the flag is refreshed
every frame from `g.win.capturing` (`fss_tiles.cpp:1331`), so the zero is a
measurement, not a dead instrument.

What the data say:

- READ: `FUN_1436558a0` (0x36558a0, the StellarBodyEffects ring pass) is
  not entered at any point during the zoom and reveal. It runs outside the
  FSS (about 2.4 calls a frame before the zoom). So no row of the RE table
  applies to the reveal: the main-camera tests at 0x3659a1d / 0x3659f78, the
  role copy at 0x36540b0, and the ring's mode 2 offscreen draw all live under
  that entry. INFERRED: the FSS ring is drawn by another renderer (the ring
  quad and ring mesh of rounds 16-18, `fss-ring-ps.asm` /
  `fss-ring-mesh-ps.asm`, whose own t0-t3 are a ring G-buffer).
- READ, t8-t11: t8 is ONE shared 512 x 512 texture (fmt 33, the same T28 in
  both eyes, identical grids in all 60 samples), so it is not a per-eye
  G-buffer. t9, t10 and t11 are per eye (2016 x 2016; T22-T24 and T25-T27).
  Draws into them through render target 0: t9 had 62 draws a frame in BOTH
  eyes at every sampled frame (window totals 37,443 and 37,442), and t11 had 1
  a frame in both. t10 had none through RT0 (it is a later target of the same
  pass). Every viewport was the full 2016 x 2016 with no scissor. Each eye
  cleared t9-t11 once a frame (1,208 clears each).
- READ, coverage: in the 8 x 8 sampled grid, the boxes written in t9 track
  each other through the whole reveal: eye 0 27-46 of 64 and eye 1 29-48. Both
  rose together at f14380 and again at f14610. Neither eye shows a
  progressive fill. "Differ 60 of 60" is the stereo offset of a coarse sample;
  no fill difference shows in it.
- READ, the eye runs (1400 px centre crop): run 1 (f14068-14083) is the
  transit void fading up in both eyes (mean 0.0 to 8.3 left, 0.0 to 10.1
  right). Run 2 (f14197-) has no isolated hard-black 16 or 32 px blocks in
  either eye. The crop did not catch the squares (the ring may lie outside
  it), so the dumps neither confirm nor refute the left-only squares.

ruled out: the StellarBodyEffects ring pass (0x36558a0 and the RE table's
branches under it) as the reveal's producer, because flight db982c64's hook
saw 33,600 calls outside the zoom and none in the 605 frames of zoom and
reveal.
ruled out: the scene G-buffer t9-t11 filling progressively in the left eye,
because both eyes draw 62 + 1 calls a frame into it at full viewport and its
sampled coverage tracks between the eyes through the reveal.
ruled out: t8 as a per-eye input, because it is one shared texture.

Where that leaves it: the tile pass's schedule (lists, arguments, clears),
its depth and stencil, and its G-buffer draws are now all symmetric between
the eyes. The left-only squares therefore enter after the tile pass, or
through a ring renderer that does not draw into t9-t11. Round 27's "budgeted
16 x 16 tile subsets over a cleared target" was measured on the ring quad's
OWN offscreen G-buffer (its t0-t3), and that is the one producer no
instrument has read per eye in the reveal frames. Root cause: OPEN.

Next observation (one flight; the census exists, so the build only re-arms
it): arm the draw census at the band event, not the press, for the 60 frames
after it, and diff the two eyes' draw lists. The producer is the draw that is
present only in eye 0, or that has a different instance count, viewport,
scissor or input identity there. Read in particular the ring quad's t0-t3:
whether they are one texture shared by both eyes or one per eye, and the
draws into them per eye per frame. If one shared ring G-buffer is filled
during eye 0's pass and read by both, the asymmetry is ordering. If there is
one per eye and only eye 0's is reset at the zoom, the fix is the reset.

## 2026-10-15 (v6): the eye diff, the ring quad's G-buffer and a whole-eye run (built, not flown)

Flight db982c64 (read above): the ring pass at 0x36558A0 is not on the
reveal's path, the scene G-buffer t9-t11 is symmetric, t8 is shared; the tile
pass is symmetric end to end. What is left is a producer after it, or the ring
quad / mesh renderer with its own t0-t3 ring G-buffer (rounds 16-18). v6 lists
the draws, per eye, and diffs them. Read-only; no key; the ring pass hook of v5
stays in.

What it does:

- THE EYE DIFF (replaces arming the DCO census, which has no in-process
  structured form and covers a few frames; the eye runs still arm their own
  census at +20 and +150). From the all-small band event, for 60 frames, every
  draw into a render target that is a texture at least 128 wide is recorded
  by the draw hooks (kind, per-draw count, vertex and pixel shader hashes, the
  shape of the target, the shapes of pixel-shader inputs t0-t3, instance count,
  viewport). A "pair" is a size/format that exactly two textures have rendered
  into in the window; the first to appear is eye 0, the second eye 1. Per frame
  and pair, the two lists are compared by (kind, count, shaders, target shape) and
  only the differences are logged: draws only one eye has, draw counts,
  instance counts, input shapes and viewports that differ. Draws into a shape that
  does not hold exactly two textures (a shared or mono target, for instance the
  FSS body layer) cannot be assigned an eye; they are counted and ranked in an
  `unpaired` line, so a mono producer is visible.
- THE RING QUAD. Draws matching the ring family (the 332753 vertex hashes
  `7E38A6AA1269C901`, `0357BBB2DEE43C1F`, `53211E8C072CD02E`, the quad's pixel
  shader `7CECABDE34FFBE9E`, or an indexed draw of 13,248 indices) are tagged.
  Their pixel-shader inputs t0-t3 (the ring's own G-buffer) are logged per eye as
  resource ids, so "shared or per eye" reads off the line and the closing
  count; the textures are watched (roles `ring_t0` to `ring_t3`), so every draw
  into them is counted per eye per frame with viewport and scissor
  (`gbuf` lines), and every clear, copy or update of them is counted in the
  `writes` line with the eye it belongs to.
- THE EYE RUN COVERS THE EYE. The two automatic runs capture their final-image
  rows (`eye_<stamp>_FinalCrisp_L<nn>.bmp` / `_R<nn>.bmp`) as the WHOLE
  submitted eye at 1/4 size (1008 x 1008 for 4032 x 4032), made by a mip chain,
  instead of the 1400 px centre crop that missed the ring in flights J, K and M.
  The manifest's `crop` is the written image's own size; the overview stays full
  size; the raw and treated crops are unchanged. A 16 px square is 4 px.

Lines (new ones only):

    fss tiles: eyediff f=N pair=WxH fmtF only-eye0: <vs8>/<ps8> X n=K xM (rt shape S) | only-eye1: ... | draws differ: ... a vs b | instances differ: ... | inputs differ: ... | viewport differ: ... WxH vs WxH
    fss tiles: ring quad f=N eye0 draws=a (vs H ps H) t0-t3 = T..,T..,T..,T.. | eye1 draws=b (...) ... | unpaired-target draws: eye2 ...
    fss tiles: gbuf f=N eyeE ring_tK T<id> draws=n instances=m viewport first (...) last (...) scissor ...
    fss tiles: window N eyediff -- ledger ran F frame(s) from the all-small event, twin-target pairs compared in P, frames with a difference D, eyediff lines L; ledger entries that did not fit O; ranked differences: [1] only-eye0 <vs>/<ps> X n=K rt-shape S in F frame(s), total T; ...
    fss tiles: window N eyediff unpaired -- U draw(s) went to render targets of a shape that does not hold exactly two textures ...; the biggest families: ...
    fss tiles: window N ring quad -- ring-family draws: eye0 in a frame(s) b draws | eye1 in c frame(s) d draws | unpaired target ...; ring PS t0-t3 the same resource in both eyes: t0 x of y, t1 ..., frames; families: <vs>/<ps> xN; ...
    fss tiles: window N upstream -- draws into ... t8-t11 and the ring quad's own inputs ring_t0-t3 (render target 0): eyeE t8..., ring_t0: frames with draws a, draws b ...

All of them print whether or not they matched: the eyediff line says when the
ledger never armed, `none: the two eyes' lists matched in every compared frame`
when it ran clean, and the ring quad line says when no draw matched the family.

Flight plan (Sean, VR, a ringed planet, `off`, no keys): the same as v5.

1. `fix.fss_eye_sync = off` in the ini before launching; the log head shows `ring
   hook armed` and no `fss eye heal: armed`.
2. Zoom a ringed planet once, wait for the reveal to finish, press nothing. Say
   which eye shows the squares (flight db982c64: the left).
3. Read the `window N eyediff`, `eyediff unpaired`, `ring quad` and `upstream`
   lines, then the `eyediff`, `ring quad` and `gbuf` lines around the band event,
   then the whole-eye images.

Reading rules:

- `window N eyediff`, the ranked list: a draw `only-eye0` (or with an `instances`,
  `inputs` or `viewport` difference that is eye 0's) in most of the 60 frames is
  the producer candidate: one the left eye has and the right does not. Look the
  hashes up in the census of the same frames (the eye runs arm one). Draws that
  differ only in the first frame or two are the transit. `none: ... matched in every
  compared frame` and a non-empty `unpaired` list says the eye-sized draws are
  symmetric and the producer draws into a shared target: the mono FSS body
  layer, composited into each eye (rounds 7-14 found the composite byte-equal).
- `ring quad`: with `t0-t3 the same resource in both eyes` N of N frames, the ring's
  G-buffer is one shared texture. Then the asymmetry is ordering or a reset:
  read the `writes` line for `ring_t0`-`ring_t3` (a clear or copy into them, and
  in which eye) and the `gbuf` lines (draws per eye into them and when). A clear
  only in eye 0, at the zoom, is the reset to fix. With 0 of N (per eye), compare
  the two eyes' `gbuf` lines: fewer draws, a smaller viewport or scissor for eye 0
  is round 27's budgeted subset, and the budget is what to find. `no draw matched
  the ring family` means the hashes changed since build 332753: use the ranked
  and unpaired candidates and the census instead.
- The whole-eye images (`FinalCrisp_L00`..): black 4 x 4 px blocks inside the ring
  in the left images against the right's (frame numbers in `eye_*_FinalCrisp.json`,
  set against the ledger's frames). Left-only blocks in frames where the diff
  shows a difference confirm the producer; blocks in neither eye mean the run was
  too early or too late: the two runs are at +20 and +150 frames.

## 2026-10-15 (v6 flown): flight e05b6be6, the windows caught the FSS exit

Flight on test build N (`v0.20.0-rc.1-96-ge05b6be6`, Frontier, VR;
`edvr_gfx_20261015_101526.log`, `--expect-build e05b6be6` matches; eye runs
`eye_101759_*` and `eye_101803_*`, whole eye at 1008 x 1008). `off`, one zoom
of a ringed planet, no keys. Sean: the squares in the LEFT eye only, again.
Read-only analysis.

**The capture was not the reveal.** CONFIRMED from four independent things:

- The whole-eye images. Run 1 (f15120-15135) is the COCKPIT fading up from
  black in both eyes. Run 2 (f15250-) is the steady cockpit, with the target
  "RINGED SAPPHIRE 698 Ls" on the HUD. There is no scanner and no ring. Both
  runs log `eye capture: no scanner chrome was held on the run's first
  frame`.
- The timeline. The scanner screen came up at 10:17:23 (f11890). Transition
  flashes at f13006 (10:17:36) and f13273 (10:17:39.095) are the zoom's
  departure and arrival (INFERRED: 267 frames apart, the ~3 s transit).
  `GuiFocus 9` was read only at 10:17:39.162, 67 ms after the arrival. The
  window opened at f15096 (10:17:59.34) with a transition flash of its own,
  and `the game says FSS focus ended` followed at 10:18:02. Flight M
  (db982c64) has the same shape: focus read 09:22:22.4 after a flash at
  09:22:21.5, window at 09:22:38.9, focus ended 09:22:39.96.
- The code (`vscreen.cpp` 6029-6069). The window opens on a world camera jump
  while `journalFssFocus()` is true. Under `off` the eager Status.json poll and
  the zoom-press watcher do not run (`needsFssSignals()` is `any() || tiles`),
  so the focus is read late. The arrival jump landed before focus was known,
  and set nothing. The exit jump back to the cockpit landed while the stale
  focus still read 9, and opened the window.
- The all-small band event (every cell to the small shader) is the black
  frame of that exit transition, not the arrival.

What the v6 lines measured, then, is the cockpit after the exit:

- `eyediff`: 60 of 60 frames differ. The scene pair (2016 x 2016 fmt 26)
  carries per-eye culling noise. An eye-sized R8 pair (fmt 60) drawn by VS
  `53211E8C` with PS `7EAC7196` / `B403F48C` (3-vertex instanced) differs in
  instance batches of about 5,800: eye 0 182,444 against eye 1 91,045 over 37
  frames for `7EAC7196`, with the larger batch in either eye on different
  frames. Unpaired (mono) targets: `DEF19B03/06CD42A9` x5253,
  `2D78DC3F/99C21CEB` x4037, `20F383BB/DED87960` x3810, `5DAD41AA/25D5F73A`
  x3565, `E8FDC0D9/5D07ACB4` x1332. None of these is tied to the reveal.
- `ring quad`: the "ring family" matched 10,331 / 10,240 draws in the
  COCKPIT (`53211E8C/7EAC7196` x5185, `53211E8C/B403F48C` x5065,
  `7E38A6AA/7CECABDE` x5074, `0357BBB2/81812EF9` x5251). On 332841 these
  hashes are not ring-specific. The tracked PS's t0-t2 are per eye and t3 is
  shared (T30, 60 of 60). ring_t0 takes 1 draw a frame per eye, ring_t3 2;
  `ClearRenderTargetView->ring_t3` x1199.
- The ring hook again saw 0 calls inside (50,736 outside). That is expected
  in the cockpit after the exit, so it says nothing about the reveal.

Withdrawn (the evidence was the FSS exit, not the reveal):

- withdrawn: flight db982c64's "ruled out: the StellarBodyEffects ring pass
  (0x36558a0) as the reveal's producer". The capture was after the exit; the
  ring pass at the arrival is UNTESTED.
- withdrawn: flight db982c64's "ruled out: the scene G-buffer t9-t11 filling
  progressively in the left eye"; UNTESTED at the arrival.
- suspect: flight bca9d6b8's `off` run (its event and black eye run match
  this exit signature) and so the list-row ruling drawn from it. The `tiles`
  run had the eager signals and may have been a real zoom; the visual
  "`tiles` does not fix it" stands on Sean's eyes, not on the window.
- ruled out: an `off`-mode window as evidence of the arrival, because
  without the eager focus signals the window opens on the exit jump.
- Unaffected: the RE's static findings, the slots 3-4 correction, and the
  e2e182a6 / f9f62bbc windows (`tiles` / `on`, eager signals running).

Root cause: OPEN, with nothing yet measured at the arrival under `off`.

Next (an instrument fix, then the same v6 flight). Make the fss tiles window
independent of `fix.fss_eye_sync`: run the eager Status.json poll and the
zoom watchers (gamepad included: Sean zooms on `Pos_GamePad_RTrigger`)
whenever the instrument is armed. Open the window only on a jump while the
scanner chrome is held (`fssChromeFrame` within 5 frames), not on the
journal alone. Refuse the band event and the eye runs unless the chrome is
held on that frame. Log `window N opened ... chrome held=yes|no` so a reader
can reject an exit capture from the log head. The proof is the eye run: the
FinalCrisp rows must show the scanner view with the ring. Only then are the
eyediff, ring quad and ring hook lines evidence.

## 2026-10-15 (v7): the window opens only at the scanner's zoom (built, not flown)

Flight e05b6be6 (read above): under `fix.fss_eye_sync = off` the instrument's
windows opened at the FSS EXIT, the "all-small" band event was the exit's black
frame, and the eye runs captured the cockpit. The cause is in the signals, not
the instrument's maths: the fast Status.json poll and the zoom watchers ran
only when `EyeSync::needsFssSignals()` was true (`any() || tiles`), which `off`
makes false, so the arrival's camera jump landed before the FSS focus was known
and set nothing, and the exit jump landed while the stale focus still read 9.
v4-v6's conclusions drawn from `off` windows stay withdrawn until a v7 flight.
v6 is otherwise unchanged.

What v7 changes (root cause first):

- THE SIGNALS. `fssModeLatchWanted` (the key and pad watchers, the mode latch,
  the eager 100 ms Status.json poll) is now
  `needsFssSignals() || fssTilesWantsSignals()` at both sites in
  `device_hook.cpp`; the instrument is armed in every VR session, so the poll
  and every zoom watcher, `Pos_GamePad_RTrigger` included, run whatever
  `fss_eye_sync` is. The scanner-chrome tracker in `vscreen.cpp` runs for the
  same reason (`fssTilesWantsChrome()`; it ran under `off` in flight N only
  because the temporal pass asked).
- THE GATE (`ZoomGate`, `fss_tiles_core.h`). A window opens only on a camera jump
  or a zoom press while the scanner's chrome is held (the tracker matched within 5
  frames). Every jump and press is offered to it with the chrome's age; a refused
  one is logged (`not a window opener: the scanner chrome is not held`). A second
  accepted signal inside an open window restarts its 600-frame clock (the zoom's
  departure and arrival are two jumps).
- THE BAND EVENT AND THE EYE RUNS. The all-small band event is refused unless
  the chrome is held on that frame (`band event at f=N refused: ...`: no reveal
  capture, no ledger, no eye run), and each automatic eye run is refused on any
  frame the chrome is not held (`automatic eye run K refused at f=N: ...`; it
  stays pending until the chrome is held or the capture ends, then logs that it
  never ran).
- THE EXIT. The window closes, with its reason, when the game says FSS focus
  ended or the chrome has been gone more than 45 frames; the reveal capture
  ends with it and pending eye runs are cancelled
  (`window N gate closed at f=N: <reason>`, `automatic eye run K cancelled ...`).
- THE LOG HEAD. The window-opened line now carries `chrome held=yes|no, last
  matched <n> frame(s) ago`, so a reader rejects an exit capture from the first line.

Rig (`tools\fss_tiles_test`): the gate's rules, and a replay of the e05b6be6
timeline: the scanner up from f11890 with the chrome matching every frame, the
zoom's departure jump at f13006 and arrival jump at f13273, `GuiFocus 9` read late
(f13279), the exit jump at f15096 after the chrome stopped at f15090 with the stale
focus still reading 9. With the arrival and exit jumps only, the window opens at
f13273 and not at f15096; with the departure flash counted too, it opens at
f13006, the arrival restarts its clock, and the exit jump is refused. Source pins
for both device_hook sites, the chrome tracker, the vscreen calls and every
refusal line.

Flight plan (Sean, VR, a ringed planet, `off`, no keys): the same as v6.

1. `fix.fss_eye_sync = off` in the ini before launching. The log head shows `ring
   hook armed` and no `fss eye heal: armed`.
2. Open the FSS, zoom a ringed planet once, wait for the reveal, press nothing,
   leave the FSS afterwards. Say which eye shows the squares.
3. The check, before any other line: the `window N opened` line says `chrome
   held=yes`, its frame is the zoom's (not the exit), and the eye-run images
   (`eye_*_FinalCrisp_L<nn>.bmp` / `_R<nn>.bmp`, the whole eye at 1/4 size) show the
   scanner view with the ring, not the cockpit. If the images show the cockpit, or a
   `refused` / `gate closed` line says why nothing ran, the capture is not the reveal
   and nothing else in the log is evidence.
4. Only then read `eyediff`, `ring quad`, `upstream`, `inputs` and the ring hook
   lines as in the v6 entry.

## 2026-10-15 (v7 flown): flight 0dbd3964, the arrival still not captured

Flight on test build O (`v0.20.0-rc.1-99-g0dbd3964`, Frontier, VR;
`edvr_gfx_20261015_104130.log`; `--expect-build 0dbd3964` matches). `off`,
one zoom of a ringed planet, no keys. Sean: the squares in the LEFT eye
again. Read-only analysis.

**Validity: NO.** The gate did its job against the cockpit: seven camera
jumps were refused with `the scanner chrome is not held`, and each window
that opened says `chrome held=yes`. But no capture covered the zoom's
arrival, and no eye run ran, so there are no images to read. Timeline
(CONFIRMED from the log):

- f15424 (10:44:15): FSS entered (`fss mode latch: OPEN`; GuiFocus 9 read at
  10:44:16.5, now promptly).
- f15841 (10:44:20.39): a zoom press opens window 1 (`chrome held=yes`). More
  presses follow at f15842, f16426 and f16760 (10:44:30.94). Window 1's
  capture is the 10 s cap (`frames=875`, closed 10:44:30.435). Its lists hold
  a static 136 / 136 and the ring hook had seen **0 calls since arming**, so
  this was the sky view, before any zoom.
- The press at f16760 came **0.5 s after the capture closed**, while the
  600-frame gate was still open (it closed at f17361 = 16760 + 601). A
  capture opens only when the gate goes from shut to open, so the real zoom
  and its arrival (about f17030 by the ~3 s transit, INFERRED) were never
  captured. Between window 1's summary and window 2, the ring pass went from
  0 to 267,840 calls (about 96 a frame): the zoomed body with its ring was on
  screen, unwatched.
- f19570 (10:45:02.5): a camera jump with the chrome held (and a transition
  flash) opens window 2. It is the zoomed view AFTER the reveal: the scan alpha
  is already 1.000 (dir 0, active 1) on its first call. There was no band
  event, so no eye run.
- f23641 (10:45:47.9): window 3 opened on the EXIT jump. `fss mode latch:
  closed (your FSS key)` came 0.18 s before it, the chrome was still held,
  and the band event at f23643 is the exit black. The gate then closed on
  `FSS focus ended` at f23660 and cancelled both eye runs (the v7 guard
  worked; the opener did not).

What window 2 does show, valid but after the reveal (the zoomed FSS view):

- Ring hook (CONFIRMED): 57,696 calls in 600 frames, 96 a frame. The main
  camera id is 1. Camera 1 is role 1 and camera 22 is role 2. Each camera
  runs passes 0, 1, 2, 5, 6, 8, four times a frame, all in **body mode 2**:
  first with ring+0x6b0 clear (calls 1-48), then with it set (calls 49-96),
  in the order cam 1, cam 22, cam 1, cam 22. By the RE (0x3659a1d /
  0x3659f78), the mode-2 + 0x6b0-set calls draw into the offscreen ring target
  for camera 1 only, and camera 22's return.
- Tile lists: eye 0 large 99-104 groups / small 405-410 against eye 1 large
  199 / small 310, steady for 600 frames (small share 80.0 % against 60.9 %).
  The two eyes classify different tile sets in the steady zoomed view. UNKNOWN
  whether that is parallax or the asymmetry itself.
- G-buffer: t9 had 39 draws a frame in both eyes and t11 1 in both. The ring
  family matched no draw (its 332753 hashes again mean nothing on 332841).

Re-test of the v5 rulings withdrawn in the v6 entry:

- ruled out (stands refuted): "the ring pass is not on the reveal's path".
  The ring pass runs in the zoomed FSS view, 96 calls a frame in mode 2 on
  both cameras (window 2). Whether it differs per eye AT the arrival is
  still UNTESTED.
- The t9-t11 symmetry ruling: still UNTESTED at the arrival (window 2 is
  after the reveal).
- ruled out: a capture opened by the first zoom press as covering the
  arrival, because presses before the real zoom start the 10 s cap, and a
  later press inside the still-open gate does not restart it.

Root cause: OPEN. The next step is an instrument fix, then the same flight:

- Restart the capture on every zoom press or chrome-held jump while the gate
  is open, not only when the gate goes from shut to open.
- Better, start it at the arrival itself: the first frame the ring pass
  reports body mode 2 (the hook already reads it), or the transition-flash
  pair.
- Refuse a jump within 1 s of `fss mode latch: closed` (the exit).
- Lengthen the capture to the 600-frame reveal from that start.
- Validity check for the flight: an `opened` line whose ring-hook calls start
  in mode 2 with alpha below 1.0, and eye-run images of the scanner with the
  ring.

## 2026-10-15 (v8): the capture starts at the arrival (built, not flown)

Flight 0dbd3964 never had a capture of the arrival: window 1 ran on an early
press, the real zoom press came 0.5 s after it closed while the gate was
open, and nothing restarted. v8 changes what starts and restarts a capture
and adds the per-eye ring path. Build: full `build.bat`, fss_tiles_test green.
Branch `claude/fss-tile-lockstep`. No new config key.

What it does:

- **The arrival starts the capture.** While the scanner chrome is held and the
  FSS latch is open, the ring hook inspects every ring-pass call (capture or
  not). The first call in body mode 2 with the scan alpha below 0.999 and
  direction 0 is the arrival: it opens a capture at once (600 frames, eye
  runs at +20 and +150, the 60-frame draw ledger, the eye diff, the
  fingerprints). One trigger per zoom; the latch re-arms when the alpha
  reaches 1.0 or the scanner is left.
- **A press or jump while the gate is open restarts the capture** (the old
  one is summarised and cancelled; pending eye runs are cancelled). Window
  cap raised from 6 to 10 for it. The press/jump opener stays as the
  secondary start.
- **A jump within 1 s of the FSS latch closing is refused** (the exit).
- **The validity line**, once per capture, first in its summary:
  `fss tiles: capture N valid=yes|no (started by ..., mode 2 alpha<1 seen at
  f=..., eye runs ran: A of B planned[, ended by: ...][; not valid because:
  ...])`. Valid needs the chrome held at the start, the arrival seen inside
  the capture, at least one eye run, and no early end.
- **The ring path per eye**, for the whole capture: every draw made inside a
  ring-pass call is attributed to that call's camera, role, pass and mode.
  - `fss tiles: ring path f=F cam=C role=R 6b0=set|clear: draws=D instances=I
    into T<id> WxH fmtN (viewport ...); samples t0-t3 = T..` for the first 12
    frames and every tenth frame after. `[samples the offscreen target]` marks
    a draw whose pixel shader reads the offscreen target.
  - `fss tiles: ring offscreen target identified: T<id> WxH fmtN` once: the
    render target of the first draw inside a call with ring+0x6b0 set.
  - `fss tiles: input f=F ring offscreen target T<id> ... nz8x8=<grid>
    (boxes written N of 64)`: the 8x8 coarse fingerprint of that target,
    every tenth frame.
  - Per-window summary: `window N ring path -- offscreen ring target
    (ring+0x6b0): ...; drawn into by the role-1 (main, left eye) camera only |
    both cameras | ...; the other camera's ring draws sample it | do not
    sample it; per camera: ...`, and `window N ring offscreen target fill --
    frame:boxes` over the capture.
- Ring hook call lines are no longer capped at 20 calls only: the first 20
  frames are logged in full (cap 4000 lines).

Flight plan, unchanged: `fix.fss_eye_sync = off`, one zoom, no keys, do not
press zoom twice.

How to read it:

1. Find the validity line for the capture that began at the arrival. If it
   says `valid=no`, nothing below counts; the reasons name what is missing.
2. `ring offscreen target ... drawn into by`: the discriminating answer. If
   only the role-1 camera draws into the offscreen target and the other
   camera's draws render directly (`do not sample it`) or are absent, the
   asymmetry is in the ring pass's own per-camera work: the right eye never
   gets the offscreen layer. If both cameras draw into it once each, the
   asymmetry is downstream (compositing).
3. The fill series (`boxes written of 64` by frame) and the `nz8x8` grid
   show when the offscreen target's squares resolve; compare with the
   eye-diff and the left/right fingerprints at the same frames.
4. If the offscreen target is not identified, no draw happened inside a call
   with ring+0x6b0 set: the RE's draw/target model is wrong and the ring
   hook call lines (cam, role, 6b0) are the evidence.

## 2026-10-15 (v8 flown): flight f5c7ad89, the ring is drawn by the left camera only

Flight on test build P (`v0.20.0-rc.1-102-gf5c7ad89`, Frontier, VR;
`edvr_gfx_20261015_110639.log`; `--expect-build f5c7ad89` matches). `off`,
no other keys. Sean: the squares in the LEFT eye only. Read-only analysis.

**Validity: NO.** All four captures say `valid=no`:

- Captures 1-3 (f17844, f17845, f18037) started on zoom presses in the sky
  view. 1 and 2 were cut short by the next press. None saw a ring-pass call.
- Capture 4 (press at f18832) holds the zoom: the first ring-pass call came
  at f18855, 23 frames after the press. It is still `valid=no` because the
  arrival trigger never fired: every ring call in the window read
  **alpha=1.000, dir=0, active=1**, from the first call on, so "mode 2 with
  alpha below 1" cannot happen. No band event, so **no eye run, no images**.
  CONFIRMED: `gPlanetRingScanAlpha` (ring+0x718) is already 1.0 when the ring
  first draws, so it is not the reveal's clock.

What capture 4 does show (in flight, at the zoom; CONFIRMED unless marked):

- **Who draws the ring.** `ring offscreen target identified: T42 252x252
  fmt26` at f18856, the render target of the first draw inside a ring-pass
  call with ring+0x6b0 set, made by camera 1 (role 1, the main camera, id 1
  read through the ring's own getter). From about f18905 the same draws go
  into **T43 1008 x 1008** (half the 2016 eye: Finding 2's mono body layer).
  The window summary: `drawn into by the role-1 (main, left eye) camera
  only`. Per camera, only `cam=1 role=1 6b0=set` has draws: 4 a frame (one
  per round with 0x6b0 set), each sampling the ring G-buffer T38-T41.
- **The other cameras draw no ring.** Camera 22 (role 2) makes the same
  calls (passes 0, 1, 2, 5, 6, 8, mode 2, 0x6b0 clear then set, about
  2,300 of each), and so does a third camera, 8 (role 0, about 125 calls of
  each), but neither issues a single draw inside a ring-pass call. This is
  the RE's main-camera test at 0x3659a1d / 0x3659f78, now seen live: in
  mode 2 with 0x6b0 set, only the main camera draws; the rest return.
- Tile pass: lists identical in both eyes and static (240 / 269 groups). The
  eye-sized scene does not carry the zoomed body. t9 11 draws a frame in both
  eyes.
- Offscreen fill: the coarse 8 x 8 grid of T42 reads 42, then 45 of 64 boxes,
  flat. That series kept sampling T42 after the draws moved to T43, so it
  says nothing about the reveal (an instrument fault). Also, the `ring path`
  `draws=` figures and the summary's "per frame 4..184 / 188..2308" are
  RUNNING TOTALS (they climb by 4 a frame), not per-frame counts.

How the right eye gets its ring: not from a ring draw of its own (none
exists). INFERRED: from the composite of the same mono layer T43 that
Finding 1 described, a draw outside the ring pass that v8 does not watch.
The eye diff did not run (no all-small event).

ruled out: the right eye drawing its own complete ring (and the left a
progressive one) in the ring pass, because in flight f5c7ad89 only camera 1
draws, into the offscreen layer, and cameras 22 and 8 draw nothing inside
any ring-pass call.
ruled out: the scan alpha ring+0x718 as the reveal's clock, because it reads
1.000 from the first ring call of the zoom.
ruled out (instrument): the v8 arrival trigger (mode 2 with alpha below 1),
because the alpha is never below 1.

Root cause: OPEN, but narrowed. One ring image is made (camera 1, into T43),
so any left-only difference must come from how each eye USES it: a different
composite draw, a different moment (the left eye compositing T43 mid-fill in
its own pass, the right after it is complete), or a left-only producer
outside the ring pass. The proposed fixes ("composite the same target into
the right eye") wait on knowing what each eye samples.

Next observation (one flight, instrument changes only):

- Trigger the capture on the first ring-pass call with ring+0x6b0 set (here
  f18855), and run the eye runs at +0, +30 and +270 so they bracket the
  arrival.
- Log every draw anywhere in the frame (not only inside ring calls) whose
  pixel shader samples the offscreen target (T42 or T43, re-identified on
  each change), per eye, with its order relative to camera 1's four draws
  into it.
- Fingerprint the target the draw actually uses, and fix the running totals.

Reading: the left eye's composite sampling T43 before camera 1's draws
complete in the frame, while the right samples it after, is the ordering
cause. The fix is then to defer the left composite or have it sample last
frame's layer. A left-only draw outside the ring pass is a different
producer, named by its hashes.

## 2026-10-15 (v9): the trigger is the ring layer; the compositors are searched (built, not flown)

Flight f5c7ad89 (v8) was invalid because the scan alpha is 1.000 from the first
ring call, so the "alpha below 1" trigger never fired. Three flights in a row
missed the arrival, so this build makes the decisions that missed it pure
functions and replays the failing flight through them in the rig. Branch
`claude/fss-tile-lockstep`; full `build.bat` green, fss_tiles_test 235 checks.

What changed:

- **Trigger.** The first ring-pass call with ring+0x6b0 set while the scanner
  chrome is held (`isArrival`, `ArrivalLatch`). One trigger per zoom; the latch
  re-arms when the layer is away for more than 60 frames or the scanner is
  left. The FSS latch no longer gates it (chrome alone does).
- **The machine.** Arming, restart, exit and the eye runs moved from the
  frame boundary into `RevealMachine` (fss_tiles_core.h), a pure function of
  frame, time, chrome age, focus and the trigger. The boundary only feeds it
  and acts on its output. Eye runs now fire at +0, +30 and +270 frames from the
  arrival (`kRunOffsets`), one a frame, each waiting for the chrome;
  `kMaxAutoRuns` is 9 a session, `kMaxWindows` 10.
- **A later press or jump** restarts a capture that has not found the arrival.
  It does not restart the reveal capture the arrival armed (the departure and
  the arrival of a zoom are two jumps; the second must not wipe the first's
  data). Logged as "does not restart the capture".
- **Bug fixed (found by the replay).** When the gate closed on leaving the
  scanner, the window machine had already cleared `capturing`, so the validity
  line never said "ended by". It does now.
- **Rig.** `testV9Replay` replays capture 4 of f5c7ad89: a press at f18832, ring
  calls from f18855 (cameras 1, 22 and 8, 6b0 clear then set, alpha 1.000), T42
  then T43 from f18905, chrome held throughout. It requires one trigger at
  f18855, the capture armed at f18856 replacing the press-started one, runs at
  f18856, f18886 and f19126, a late press ignored, the capture ending at
  f19457, the target re-identified once, and a valid capture. Further cases:
  arrival without chrome refused, exit cancels the third run, a run waits for a
  lost chrome, the latch re-arms, the order classification.

New and changed log lines (all inside a capture):

- `ring offscreen target identified: T.. WxH fmtN` once, then
  `ring offscreen target CHANGED at f=..: T42 .. -> T43 ..` on every change.
  Window summary `ring offscreen target history -- N change(s): T.. from f=..`.
- `ring path f=.. cam= role= 6b0=set|clear: draws=D ...` now counts draws of
  that frame. v8's figure ran on across frames (the entry was not cleared), as
  was the summary's "per frame a..b".
- `ring fill f=F T<id> WxH fmtN nz8x8=<grid> (boxes written N of 64)` every 5
  frames, taken at the end of the frame after camera 1's draws, of the target
  current at that frame. The summary lists `frame:target:boxes` for all of them.
  This replaces v8's `input .. ring offscreen target` line, which sampled at
  eye 0's tile dispatch and kept reading T42 after the draws moved to T43.
- `compositor f=.. eyeN draw #a..#b (n draws) vs .. ps .. into T.. WxH fmt
  (viewport ..) samples the offscreen ring layer T.. in PS t<slot>; BEFORE |
  BETWEEN | AFTER camera 1's ring draws | no camera-1 ring draw in this frame
  [camera 1's ring draws this frame: n, #a..#b]`. Every draw outside the ring
  pass whose pixel shader reads the current layer, every frame for 40 frames,
  then one in ten, and on first sighting. `#` is the draw's order index among
  all draws noted that frame. Eye comes from the order the render target
  appeared among same-sized targets (as the ledger does); `eye?` means more or
  fewer than two such targets, so read the target id.
- Summary: `window N compositor k of n -- ..: F frames, D draws; per frame its
  first draw came before camera 1's ring draws X, between them Y, after them Z,
  with no ring draw in the frame W`, or `window N compositors -- no draw
  outside the ring pass sampled the offscreen ring layer (checked: ..)`.

Limits, said in the log too: the search covers pixel-shader reads (t0-t3 of
every draw; t4-t15 only for draws with a pixel shader into a target 128+
wide). Vertex-shader and compute reads are not checked, so "no compositor"
means none of these. The wide scan costs a `PSGetShaderResources` per such
draw while a capture runs.

Flight plan, unchanged: `fix.fss_eye_sync = off`, one zoom, no keys, do not
press zoom twice. Fly into the zoom from the scanner view and wait.

How to read it:

1. `capture N valid=` for the capture that began at the arrival. `valid=no`
   voids the rest. Three `eye run` lines should follow the arrival.
2. `ring offscreen target history`: which targets the layer had, and `ring fill`
   against them. The boxes written (of 64) per sample says when the layer
   fills; the series should follow T42, then T43.
3. The compositor lines say who reads the layer, per eye, and when relative to
   camera 1's four draws. The ordering cause predicts the left eye's compositor
   BEFORE or BETWEEN the draws while the right's is AFTER. If both are AFTER
   and identical, the asymmetry is not ordering; compare their vs/ps and
   viewports. If only one eye has a compositor, the other is fed by something
   the search does not see (a vertex-shader or compute read, or a copy).
4. If the summary says no compositors, the layer reaches the eyes through a
   path outside pixel-shader reads, and the next instrument is copies and
   compute reads of the target.

## 2026-10-15 (v9 flown): flight aba75537, the first valid capture of the arrival

Flight on test build Q (`v0.20.0-rc.1-105-gaba75537`, Frontier, VR;
`edvr_gfx_20261015_113333.log`; `--expect-build aba75537` matches; eye runs
`eye_113813_*` (f23364-), `eye_113815_*` (f23394-), `eye_113820_*`
(f23634-), whole eye at 1008 x 1008). `off`, one zoom press, no other keys.
Sean: the squares in the LEFT eye. Read-only analysis.

**Validity: YES.**

- `capture 5 valid=yes`, started by the ring arrival: the first ring-pass
  call with ring+0x6b0 set, camera 1, at f23362/23363, with the chrome held.
  All 3 of 3 eye runs ran. Captures 1-4 (zoom presses at f22780-23336) were
  invalid and were restarted, as designed.
- The images show the scanner and the ringed planet (38 Lyncis 5) in both
  eyes.

What it shows (CONFIRMED unless marked):

- **The layer.** Camera 1 (role 1, main) draws the ring 4 times a frame into
  T33 (252 x 252). It switches to T93 (1008 x 1008) at f23391, logged as
  `target CHANGED`. Cameras 22 and 8 draw no ring. The coarse 8 x 8 fill of
  the layer reads 42, then 45 of 64 boxes on T33, then a flat 40 on T93 from
  f23395 to f23960. At this coarseness the layer shows no progressive fill.
- **The compositors.**
  - Compositors 4 and 5 (573 frames, after the switch): eye 0 (into T10) and
    eye 1 (into T23) composite the layer with THE SAME draw, VS `953C8123` /
    PS `A473A867` (fullscreen, n=6), sampling it in PS t3. In every frame
    both come **AFTER** camera 1's ring draws: 573 of 573 frames each, 0
    before, 0 between.
  - Compositors 2 and 3 are the same draw in the first 19 frames, sampling
    the layer in t2.
  - Order within a frame, for example f23372: ring draws #176-#182, eye 0
    composite #183, eye 1 composite #217.
  - A mono draw, VS `DEF19B03` / PS `831DF02E` into T80 (63 x 63), samples
    the layer BETWEEN the ring draws: a downsample.
  - So both eyes composite the same, finished layer state.
- **The composite is depth tested per eye.** From the census lines of the
  same draw: `ds=17wZ` (depth on, GREATER_EQUAL on reversed Z, no write),
  `d=@40` in eye 0 and `d=@44` in eye 1, each eye's own depth, and stencil off
  (`st=00`). Its PS inputs t0-t3 are the same tokens in both eyes.
- **The images.** In run 1, frames 12-15 (f23376-23379, the 252 x 252
  stage), the body and ring are a coarse mosaic in both eyes. The LEFT eye
  shows black blocks over the ring band where the RIGHT shows the ring.
  Frames 0-8 are the black void in both eyes. Run 2 (f23394-, the 1008 x
  1008 layer) looks alike in both eyes at this scale.
- **Eye diff.** 58 of 61 frames differ. The top kinds are fullscreen-quad
  families into the G-buffer pair (2016 x 2016 fmt 23, t9): VS `5DA53D8B` /
  PS `E23C4525` and `BF0CE0DA`, and VS `4A0748B6` / PS `1E1E004B` and
  `4411D5EF`. All are n=6 with depth test, no depth write, and stencil write
  (`so=r00/w05`). Their draw counts per frame differ by eye. Eye 0 has bursts
  of 100-184 (f23369, 23374, 23376, 23378, 23383, 23386-7). Eye 1 rises
  slowly from 4 to about 30. INFERRED: per-eye decal or light-volume quads.
  Not yet tied to the squares.
- Tile lists: in the capture, eye 0's large list fell from 240 to 109
  groups while eye 1's held at 210-248 (mean small share 65.8 % against
  55.7 %). The per-eye depth or G-buffer differs over the reveal.

ruled out: an ordering race on the ring layer (one eye compositing a
partly-drawn layer), because in flight aba75537 both eyes' composites
(VS 953C8123 / PS A473A867) run after camera 1's four ring draws in all 573
frames and sample the same layer.
ruled out: a second ring image for the right eye, because cameras 22 and 8
draw no ring and both eyes composite T33/T93.
ruled out (instrument): the coarse 8 x 8 fill of the layer as a reveal
measure, because it is flat while the images change.

Root cause: OPEN, narrowed to what lies UNDER or AFTER the same composite in
each eye. INFERRED, leading: the composite's per-eye depth test. It passes
only where the eye's depth is at or behind the quad. If the left eye's depth
(or something drawn after the composite in the left only) carries 16 x 16
blocks over the ring during the reveal, the composite is rejected there and
the left shows black, while the right does not.

Next observation (one flight; instrument only):

- At f+0..+30 of the arrival, per eye, copy the ring region of three things
  and dump them beside the eye run:
  - the composite's render target, just before and just after the composite
    draw;
  - its bound depth (`d=`), after the composite.
  - Keep 16 x 16 blocks at eye resolution.
- Reading:
  - Left black blocks already present right after the composite, with
    left-depth blocks nearer than the quad there: the depth test is the
    mechanism. The left eye's depth producer at those blocks is the lever,
    and the eye-0-only bursts of `5DA53D8B` / `4A0748B6` quads are the first
    suspects.
  - Blocks absent after the composite but present in the final image: a
    later left-only draw. Bisect the frame's draws after the composite.

Fix (pending that reading). Composite both eyes against the same depth
verdict: either have the role-2 path receive the same occluder, or have the
composite skip its depth test during the reveal. Proposed hook: the composite
draw, VS `953C8123` / PS `A473A867`, by shader identity at draw time. Not
built.

## 2026-10-15 (v10): what is under and after the composite, per eye (built, not flown)

Flight aba75537 (v9) ruled out timing and a second ring image: both eyes
composite the same finished layer with VS 953C8123 / PS A473A867, GREATER_EQUAL
against each eye's own depth, after camera 1's ring draws. What is left is the
depth the quad is tested against and what is drawn into the eye's target after
it. v10 measures both. Same trigger as v9 (the first ring-pass call with
ring+0x6b0 set, chrome held; the v9 replay case is kept). Full `build.bat`
green, fss_tiles_test 273 checks, no config key.

At the arrival, for frames +0..+30 (`kDumpFrames`), per eye:

- **The composite** is found as v9 found the compositors: the draw outside the
  ring pass that samples the layer into an eye-sized target (512+ square). The
  eye is the order its target first appeared.
- **Target snapshots** of that target, reduced on the GPU (copy, `GenerateMips`)
  to one texel per 16 x 16 px block: just before the composite, just after it
  (the next draw the hook sees) and at the end of the frame. Whole eye (the
  ring region is not known), 126 x 126 blocks at 2016.
- **Depth and stencil** bound at the composite, copied whole into a staging
  twin and reduced on the CPU to min and max depth and max stencil per block
  from every 4th texel (16 samples a block).
- **Files**, in `edvr_logs\fsstiles`: `v10_f<frame>_e<eye>_<name>.pgm`, names
  `rt_before`, `rt_after`, `rt_final` (brightness class x28: 0 exactly black,
  1 below 0.001, 6 is 0.3 to 1, 9 above 10), `depth_min`, `depth_max` (x255),
  `stencil_max` (as is, when the depth has stencil) and `verdict` (class x40).
  One texel is one block.
- **Verdict line** per eye-frame: `verdict f=F eyeE composite target T.., depth
  T.., WxH blocks of 16 px: black at the end of the frame N; the composite
  changed K block(s) (largest block-minimum depth it wrote through Q); of the
  black blocks: depth above everything it wrote through (rejected) a,
  overwritten after the composite b, never covered c, written black by the
  composite d, no depth to judge e`. And `verdict f=F pair`: the blocks black
  in one eye and not black in the other at the same position, by class.
  Rules: a block is black when its brightest channel mean is under 0.001 at
  the end of the frame. If the composite changed it (after != before) it is
  overwritten (the composite wrote something that is black again) or written
  black. If not: with no depth it is "no depth to judge", with the block's
  minimum depth above Q it is rejected by depth, otherwise never covered
  (depth would have let the quad through; the layer was empty there or
  something else stopped it). Q is a lower bound of the quad's depth, so
  "rejected" is "above everything the composite did write through", not
  proof of the quad's own depth. Left against right is by block position, so
  parallax of a block or two blurs it.

Per eye, per frame, for the four suspect families (VS 5DA53D8B with PS
E23C4525 or BF0CE0DA, VS 4A0748B6 with PS 1E1E004B or 4411D5EF), through the
whole capture (every frame for 40, then one in ten):

- `suspect f=F eyeE family N (vs ps) into T.. WxH fmt, depth T..: draws=D
  (before the composite b, after a) instances=I draw #a..#b viewport (..)
  scissor (..) | depth on|off func N write yes|no, stencil on|off ref N read
  mask write mask func N ops fail,depthfail,pass [the state VARIES] |
  bindings: N distinct hash(es) (top: hash xN, ..)`. The eye is the depth
  buffer's (the composite's left @40, right @44). The binding hash is the
  identity of the pixel shader's t0-t7 and vertex constant buffer 0.
- `suspect cb f=F draw #n family N into T.. (depth T..): vertex|pixel
  constant buffer 0 B.. from constant N, 64 bytes hash H, floats .., ints ..`:
  the first 6 suspect draws per depth buffer a frame, for the first 40 frames,
  read back from the GPU; to say what drives the per-eye count, compare these
  across the eyes and across the draws of a burst.
- Summary: per family and eye the frames, draws (most in a frame, mean),
  before and after the composite, instances; the draws a frame series
  `frame offset:eye0/eye1`; `ring dump` counts; verdict totals per eye and
  the asymmetry totals.

Limits. The "after" snapshot is taken before the next draw, so a clear or copy
between the two lands in it. The final snapshot is of the composite's own
target at the end of the frame, which may not be the image the headset gets
(compare the eye runs). Cost: three GPU reductions and one 16 MB depth copy per
eye per frame for 31 frames, plus a CPU pass over about 1/16 of the depth.

Flight plan, unchanged: `fix.fss_eye_sync = off`, one zoom, no keys, do not
press zoom twice.

How to read it:

1. `capture N valid=yes`, three eye runs, and `ring dump armed at f=..`.
2. The pair line first: which class holds the blocks that are black in the
   left and not in the right. `rejected by depth` points at the depth the left
   eye carries over the ring (the suspect quads and the stencil writes are the
   producers: compare `depth_min` of the two eyes and the suspect counts in
   the same frames). `overwritten after the composite` points at a later draw:
   `rt_after` against `rt_final` names the frame, the suspect lines name draws
   after the composite. `never covered` with depth that would pass points at the
   layer or the composite's coverage.
3. The grey images: `verdict` shows where the blocks are, `depth_min` what sits
   over them.

## 2026-10-15 (v10 flown): flight 38aef250, the left composite leaves the near arc uncovered

Flight on test build R (`v0.20.0-rc.1-108-g38aef250`, Frontier, VR;
`edvr_gfx_20261015_122227.log`; `--expect-build 38aef250` matches). `off`,
one zoom, no keys. Sean: the squares in the LEFT eye. Read-only analysis.

**Validity: YES.**

- `capture 4 valid=yes`, started by the ring arrival (ring+0x6b0 first set
  at f18878). All 3 of 3 eye runs ran.
- `ring dump armed at f=18879 until f=18909`: 44 eye-frames processed, none
  dropped, 308 PGMs (`edvr_logs\fsstiles\v10_f188xx_e*`).
- Captures 1-3 were zoom presses, restarted.
- The layer and compositors repeat v9: camera 1 alone draws T39 (252^2),
  then T95 (1008^2). Both eyes composite it with VS `953C8123` / PS
  `A473A867` after the ring draws, in all frames.

**Verdicts** (whole capture; per frame f18888-18895 alike), CONFIRMED:

- **Rejected by depth: 0** in every frame, both eyes. The composite wrote
  through block-minimum depth 0.000, and `depth_min` is 0 over the whole
  eye. The depth is cleared under the ring.
- **Overwritten after the composite:** 1-2 blocks a frame (14 left / 29
  right over the capture), negligible.
- **Black blocks:** left 318,762 never covered + 4,756 written black, right
  315,162 + 6,568. Most of the eye is black FSS sky in both.
- **Per frame:** e.g. f18890, the left composite changed 1,539 blocks and
  the right 2,330.
- **Asymmetry over 22 paired frames:** black in left only, never covered
  19,236 and written black 1,502. Black in right only, never covered 17,586
  and written black 1,353. Same-position pairing, so blurred by parallax.
- **f18896-18908:** the composite changed 0 blocks in both eyes (`assumed: it
  wrote nowhere`) and the final images hold no body (a gap at the T39 to T95
  hand-over, INFERRED).

**The images** (CONFIRMED; `rt_after` / `rt_final`, f18888-18895):

- The LEFT eye's composite draws the body and only the upper and back part of
  the ring. The ring's near arc, below and in front of the planet, is MISSING:
  black, `never covered`.
- The RIGHT eye's composite draws the full ellipse of the ring over the same
  frames.
- By f18909 both eyes show the full ring alike.
- This is the observed bug: the left reveals, and the right "already filled".

**Suspects:** symmetric, so ruled out.

- All four families draw 2 a frame in each eye (8/8 per frame from capture
  frame 9 on), all before the composite.
- They have identical state in both eyes: depth GEQUAL, no write, stencil
  ref 5, write mask 05, func 8, ops 1,1,3.
- They have the same binding hashes in both eyes.
- So the v9 "bursts" were not a per-eye difference in these draws.

ruled out: the composite's depth test as the source, because in flight
38aef250 the depth is 0 under the ring and 0 blocks were rejected in any frame.
ruled out: a later draw overwriting the left eye's ring, because 1-2 blocks a
frame were overwritten in either eye.
ruled out: the suspect quad families (5DA53D8B / 4A0748B6), because both
eyes draw 8 a frame with identical state and bindings.

**Root cause: narrowed, OPEN.** CONFIRMED: the same composite draw, sampling
the same layer, covers the ring's near arc in the right eye and not in the
left for about 8 frames at the arrival. The per-eye difference is therefore
in another input of that draw. INFERRED, in order:

1. Its other PS textures. In v9's census the draw reads t0 plus three textures
   in t1-t3. Only the one equal to the ring target was tracked. The ring is
   usually drawn in two parts (behind and in front of the planet), so a
   second layer for the near arc, empty or unfilled for the left, fits the
   missing near arc exactly.
2. Its constants: a per-eye UV or clip window into the layer.

No RVA ties this to engine code yet. The producer of the differing input is
the next thing to find.

Next observation (one flight; instrument only): at arrival +0..+30, at the
composite draw, per eye:

- identity, size, format and a 16 px block fingerprint of every PS SRV t0-t7;
- the VS and PS constant buffers b0-b2 (read back);
- the vertex buffer's first 6 vertices.

Then for the input that differs, name its producer draw and camera, as v8
did for the ring target. If it is a second layer drawn by camera 1 only for
the near arc, the RE's main-camera tests (0x3659a1d / 0x3659f78) and the
material mode list (0x36597f0, modes 0-3) are where to look in Ghidra.

Fix, once that input is named: make both eyes' composites read the same
state of it. Sean wants the animation in both eyes, so the right eye should
take the left's input for the reveal frames. The hook point is the composite
draw, VS `953C8123` / PS `A473A867`, by shader identity. Not built.

## 2026-10-15: the composite, statically (Opus, no flight)

Sean asked for the composite (VS `953C8123AD8DC13B` / PS `A473A867BDDBC342`)
to be settled from the code, without a flight. Read-only throughout.

**The bytecode is not reachable offline.** CONFIRMED:

- EDVR's dump directory (`edvr_logs\shaders`, 1,199 blobs from 2026-09-23
  and 2026-10-08) holds neither hash. Those sessions dumped every created
  VS/PS, but the composite is evidently created only when the FSS layer is
  first used.
- No file on C: or D: is named after either hash.
- The game's `ShaderCache` folder is empty.
- `Win64\EffectsBinary\Effects*_Win64_SM50.arc` (16.8 MB) holds no raw DXBC:
  a scan for the `DXBC` magic, FNV-1a-64 hashed the way EDVR hashes a shader,
  found 0 blobs. The archives are packed in a format I could not decode, and
  there is no Oodle library on the machine. The exe holds one unrelated
  blob.
- The cheap way to get it is EDVR's existing dump:
  `advanced.glare_shader_dump = 1` (`device_hook.cpp:2678`) writes every
  VS/PS the game creates, by hash. One session that enters the FSS dumps both
  shaders. No build is needed; set it on the v11 flight.

**What the images say about the mechanism** (v10 `rt_after` / `rt_final`,
f18888-18895, re-read). The left eye's missing region is shaped like the
ring's FRONT arc: the ring's side lobes reach lower than the gap below the
planet, and the back arc over the top is drawn. A crop (UV window, scissor,
viewport, quad extent) would cut both lobes at one height. So: INFERRED, a
near/far split. The front part of the ring reaches the left eye's composite
through an input that is empty for it in those frames, and full for the
right.

**What the census says about that input** (v9, CONFIRMED):

- The composite binds four pixel-shader textures.
- At f23364: `s=@60,@221,@221,@221`, one texture in t1, t2 and t3 alike.
- Later: `s=@70,@224,@225,@226`, three distinct textures.
- The instruments tracked only one of them: the ring target T39/T95 (t2,
  then t3). The two others have never been read.
- In the arrival frames t1-t3 are bound to ONE texture, so whatever the
  shader expects in the other two slots (the front half, or a history or mask
  of the layer) is the same image as the main one there. Then it becomes three
  separate textures.

**The engine side.**

- CONFIRMED from v8-v10: camera 1 alone draws into the target, 4 draws a
  frame (RT0 = T39/T95), through the main-camera tests at 0x3659a1d /
  0x3659f78 inside `FUN_1436597f0` (mode 2, ring+0x6b0 set). Cameras 22 and 8
  draw no ring.
- Not found statically: where the composite is issued, and what binds t1-t3
  per eye. It is not under the ring pass (no ring-pass call surrounds it).
  ring+0x6b0's setter is not in the ring module either (59 writes of `+0x6b0`
  in `.text`, none tied to the FSS).
- So no RVA ties the per-eye input to code yet.

Root cause: OPEN. The leading hypothesis (INFERRED) is that the composite's
other layer inputs (t1/t3 beside the ring target) carry the ring's front arc,
or a mask or history of the layer. For about 8 arrival frames the left eye's
composite gets them empty, stale or aliased, and the right eye's gets them
filled.

**The single v11 field that settles it:**

- Per eye at the composite, frames +9..+17 of the arrival: the identity and
  the 16 px block fingerprint of PS t1, t2 and t3, over the blocks of the
  ring's near arc.
- If one of them is black there in the left and filled in the right, that
  slot is the cause, and its producer (the draw whose RT0 it is, and its
  camera) is the next and last step.
- If t1-t3 are identical per eye, the per-eye VS/PS constants are the cause.
- Fly v11 with `advanced.glare_shader_dump = 1` so the bytecode of both
  shaders lands in `edvr_logs\shaders` the same session, and the shader can
  say what each slot means.

Fix direction, unchanged: hook the composite by shader identity and give the
right eye's composite the left eye's state of the differing input for the
reveal frames, so both eyes animate. Not built.

## 2026-10-15 (v11): every input of the composite draw, per eye (built, not flown)

Flight 38aef250 (v10): for about 8 arrival frames the left eye's composite
(VS 953C8123 / PS A473A867), sampling the same layer, leaves the ring's near
arc uncovered while the right eye's covers it. Depth, overwrite, ordering and
the suspect quads are ruled out. The static pass (above) could not reach the
shader and named one deciding field: the composite binds four pixel-shader
textures (the v9 census: `@60,@221,@221,@221` at the first arrival frame, later
`@70,@224,@225,@226`), and only the ring layer in one of them was ever read.
v11 reads all of them. Same arrival trigger as v9 (the replay case is kept).
Full `build.bat` green, fss_tiles_test, no config key.

Flight plan: `fix.fss_eye_sync = off`, one zoom, no other keys, plus
`advanced.glare_shader_dump = 1` (Sean sets it; it writes every VS/PS the game
creates to `edvr_logs\shaders` by hash, so the composite's bytecode lands the
same session and says what each slot means; set it back to 0 afterwards).

At the composite draw of each eye, arrival +0..+30 (the v10 dump window):

- **Pixel-shader textures t0-t7.** Identity (resource id), size, format,
  resource mips and array size, the view's dimension, format, mip range and
  slice range, and a 16 px block reduction of the mip and slice the view reads
  (mip chain, GPU), read back without waiting. Per slot: an exact hash of the
  block texels, the number of blocks that are not black and the span of those
  blocks (`lit blocks span x a-b y c-d`, or `NO block is lit`). A buffer view
  or a compressed or multisampled texture says why it has no fingerprint.
- **Constant buffers.** Vertex cb0-cb2 and pixel cb0-cb2, the bound range
  (first constant, constants bound), up to 512 bytes each, read back. Floats
  logged in full at arrival +0 and +5, 16 words a line.
- **Vertices.** The draw's kind and count, topology, input layout id, vertex
  buffer (id, stride, offset, size), index buffer, viewport and scissor, and
  the first 6 vertices (up to 8 floats each; by index for an indexed draw).
  "No vertex buffer is bound" is said when the vertex shader makes the quad.
- **Samplers s0-s3:** filter, address modes, border colour, LOD, anisotropy,
  comparison.

Log lines:

- `composite inputs f=F eyeE t3 T95 1008x1008 fmt26 (...) view dim 4 fmt 26 mip
  0+1 slice 0+1 | blocks 63x63 fingerprint H, N not black, lit blocks span ..`:
  every bound slot at arrival +0 and +5; later only a slot that changed since
  that eye's last frame; and t1-t3 in EVERY frame +9..+17 (the deciding field),
  at the full 16 px block resolution. Also `composite inputs ... sN ...`,
  `... vs|ps cbN ...` (header and words) and `... vertices: ...`.
- `composite diff f=F (arrival +N): ...`: one line a frame naming every input
  that differs between the eyes (identity, view, shape, content with both
  fingerprints, `[the SAME resource changed between the two draws]`, a
  constant buffer's range, a sampler, the vertex stage), or "no input differs
  between the eyes". Then for each constant buffer that differs
  `composite diff f=F <stage cbN> words that differ, eye0 | eye1`: the first 12
  words, hex and float.
- `window N composite diff tK -- ...`, `... <stage cbN> -- ...` and
  `window N composite diff -- inputs that DIFFERED between the eyes: ... |
  inputs that never differed: ...`: the per-capture summary naming each one.
- Images in `edvr_logs\fsstiles`: `v11_f<frame>_e<eye>_t<slot>.pgm` (one texel a
  16 px block, brightness class x28) for every input whose identity or content
  differs between the eyes, and for t1, t2 and t3 of both eyes in every frame
  +9..+17 whether they differ or not.

Producers (who should have filled an input). While the dump window runs,
every draw into any render target is tallied for the frame by target, VS, PS,
depth buffer and, inside a ring-pass call, camera and role. At the end of each
dump frame, for each eye's composite and each distinct texture in its t1-t3:

- `composite producer f=F eyeE's t1,t2 T224 WxH fmtN: vs .. ps .., N draw(s)
  #a..#b, inside the ring pass, camera C role R | outside the ring pass, with
  eye 0's|eye 1's depth T..; BEFORE|AFTER eye 0's composite (#n) and
  BEFORE|AFTER eye 1's (#m)`, up to six producer entries a texture; or
  `NO draw rendered into it this frame (it was filled earlier, by a compute
  shader, a copy or a clear, or never)`.
- Limit: draws only. A texture filled by a dispatch, a copy or a clear shows as
  "no draw". The tile-renderer dispatches are logged elsewhere in the capture.

How to read it:

1. `capture N valid=yes`, three eye runs, `ring dump armed`.
2. `composite diff` per frame: which inputs differ and in which frames. Frames
   +9..+17 of the arrival are the ones that matter (the v10 gap was f18888-95).
3. If t1, t2 or t3 is lit over the near-arc blocks in the right eye and not in
   the left (`lit blocks span`, the `.pgm` pair), that slot is the cause. The
   producer lines for it name the draw, the camera and whether it came before
   the composite; a producer that is absent, late (AFTER the composite) or only
   in one eye's depth is the finding.
4. If t1-t3 match per eye, the constant words that differ are next
   (`words that differ`); then the shader bytecode, from the dump.
5. If t1-t3 are the same texture as t2 at +0..+8 and three textures later, the
   identity columns show the change frame.

## 2026-10-15 (v11 flown): flight d7769063, the root cause: the coarse reveal input is drawn mid-layer in the left pass

Flight on test build S (`v0.20.0-rc.1-112-gd7769063`, Frontier, VR;
`edvr_gfx_20261015_125933.log`; `--expect-build d7769063` matches). `off`,
one zoom, `advanced.glare_shader_dump = 1`. Sean: the squares in the LEFT eye
only. Read-only analysis.

Validity: `capture 4 valid=yes`, arrival f16113/16114, dump f16114-16144. 25
paired eye-frames read, 0 skipped. 62 `v11_*.pgm`. The glare dump wrote the
composite's VS and PS (13:01:54 / 13:02:03); the downsample's shaders were
already in the dump from 2026-09-06.

**1. What differs at the composite** (`window 4 composite diff`, CONFIRMED):

- **t1** (T121, 63x63 fmt 26) has the same identity and view in both eyes,
  but its content differs in 22 of 25 frames, `[the SAME resource changed
  between the two draws]`. Fingerprints 938EDAC4 (left) | 572380F3 (right)
  through +12, then E318C506 | 413A3AA5 to +27. At +28 (f16142) they
  converge.
- **vs cb0 and vs cb2** differ every frame. The VS uses only cb0: the per-eye
  quad transform and projection (cb0[0..1], cb0[4..7]). Expected per eye
  (INFERRED).
- **Never differ:** t0, t2 (the layer T50), t3, t4, ps cb1, ps cb2, s0-s3,
  vertices, viewport and scissor. But ps cb1 is 333 constants and v11 read
  only its first 512 bytes, so the reveal's progress words (cb1[90],
  cb1[119], cb1[332]) were not compared.

**2. The producer of t1** (`composite producer`, CONFIRMED, the same in
every frame):

- VS `DEF19B035D5EDEDC` / PS `831DF02EBA8AE814` makes 2 draws a frame into
  T121, outside the ring pass, with neither eye's depth. The ordering at f16120
  (f16127 and later shift by 55, same pattern):

| Draw | What | Into |
|---|---|---|
| #212, #213 | ring group A (B12F7A61/42AC0CAC, 203DF517/EEAAC839), camera 1 role 1, inside the ring pass | the layer T50 |
| #214, #215 | planet body draws (3530A6FD/13B224F0, 9FFA5D5E/8134D09E), outside the ring pass | T50 |
| **#216** | **downsample 1**: DEF19B03/831DF02E, layer -> T121 | t1 |
| #217, #218 | ring group B (the same two materials again), camera 1 | T50 |
| #219 | LEFT composite (VS 953C8123 / PS A473A867) | T10 |
| **#252** | **downsample 2**: layer (now complete) -> T121 | t1 |
| #253 | RIGHT composite | T23 |

- The left eye's t1 is a downsample of a layer that does not yet hold ring
  group B. The right eye's t1 is taken after it.
- Cameras 22 and 8 draw no ring (v8-v10), so between #219 and #253 nothing
  changes the layer. The only per-eye difference is WHEN t1 was taken.
- INFERRED: group B is the ring's front half, drawn after the planet. The
  missing region in v10's images is the ring's front arc.

**3. The shaders** (disassembled from the glare dump):

- PS `831DF02E` is a one-texture colour-matrix copy (cb2[1..6]), and VS
  `DEF19B03` is a pass-through. So t1 is a 1/4 copy of the layer.
- PS `A473A867` (5 textures, cb1[333], cb2[2]) is the **reveal**.
  - It splits the screen into square cells. cb1[119].x is the cell count and
    cb1[332].x/y the aspect.
  - Per cell it takes the luminance of **t1** at the cell centre, plus a
    noise texture t4, as that cell's threshold.
  - The progress is `frac(0.9999 * saturate(cb1[119].z / cb1[119].y))`, and a
    cell is revealed when `saturate(1 + progress - threshold) >= 0.99`.
  - An **unrevealed pixel shows t1** (sampled at the pixel); a revealed one
    shows **t2**, the full-resolution layer.
  - A grid-line highlight is drawn in the 0.89-0.99 band, then a circular
    falloff and exposure (cb2, t0).
- So in the left eye, every unrevealed cell over the front arc shows a black
  t1: the black squares. They switch to t2, which is complete, as their cells
  reveal. Dark cells reveal first, so they are gone in about ten frames
  (round 33).
- In the right eye the unrevealed cells show a t1 that holds the ring: blurry
  but filled, the "already filled" look.
- The animation itself (cells resolving from t1 to t2) runs in both eyes;
  only the left's coarse image is missing the front arc.

**4. The engine side.**

- CONFIRMED: the order above, and that only camera 1 draws the layer (the
  main-camera tests at 0x3659a1d / 0x3659f78 in `FUN_1436597f0`, mode 2,
  ring+0x6b0 set).
- In camera 1's pass the downsample is issued after ring group A and the
  planet's draws and before ring group B. In camera 22's pass, which draws no
  layer content, it runs again on the finished layer.
- Flat has one camera, so it has the left eye's order. The black squares are
  the stock flat look, read there as dissolve art.
- NOT FOUND statically: the function that issues the downsample. It is
  outside the ring pass, and PS 831DF02E is a generic colour-matrix copy used
  elsewhere, so no RVA is given for it. The fix below does not need one.

ruled out: the layer t2, t0, t3, t4, the samplers, the vertices and the
viewport as the per-eye difference, because flight d7769063 shows them
identical in both eyes in all 25 frames.
ruled out: a per-eye reveal clock as the cause of the black squares, because
the squares are explained by t1's content alone. cb1's progress words were not
read, so a clock difference is untested; it cannot make the squares black.

**The fix** (proposed, not built). Sean wants the stock animation in both
eyes, so make the right eye's reveal read the same t1 as the left's.

- **What:** skip the second downsample in each frame. Then T121 keeps the
  left pass's copy, and both composites (and both thresholds) read it.
- **Where:** EDVR's draw hook. Match VS `DEF19B035D5EDEDC` + PS
  `831DF02EBA8AE814` whose render target is the texture that the reveal
  composite (VS `953C8123AD8DC13B` / PS `A473A867BDDBC342`) bound as its t1,
  learned from that draw. Skip it when the first eye's reveal composite has
  already drawn this frame. VR profile only, while the reveal composite is
  drawing.
- **Risks:**
  - PS 831DF02E is a generic copy, so key on the render target learned from
    the composite, never on the hashes alone.
  - If a future build draws different layer content per eye, the right eye
    would show the left's coarse image. In this build the layer is drawn
    only by camera 1.
  - After the reveal (+28) the two copies are equal, so skipping is
    harmless then.
- **Alternative,** if Sean prefers no black squares in either eye: replay the
  downsample (the same draw state) just before the LEFT composite instead.
  Both eyes then show the right eye's reveal, which is not the flat look.
- **Proving flight:**
  - `off`, one zoom with the fix: both eyes show the same black squares and
    resolve together.
  - Log: a `composite diff` with no t1 line in frames +0..+27, and a fix
    counter of skipped draws equal to the frames of the reveal.
  - The eye-run images: the same black cells in both eyes.

## 2026-10-15 (v12): the fix: replay the first downsample before the first reveal composite (built, not flown)

Flight d7769063 named the cause: the left eye's reveal composite reads a coarse
t1 (a 63x63 downsample of the ring layer, VS DEF19B03 / PS 831DF02E) that the
left pass draws BEFORE the ring's second draw group (#216, group B at #217-218);
a second downsample from the finished layer (#252) runs before the right
composite (#253), so the right eye's t1 holds the whole ring and the left's
does not. The first plan (skip the second downsample, so the right eye shows
the squares too) was dropped before it was built: Sean wants the right eye's
current look, blurry to sharp with no black squares, in BOTH eyes. So the
LEFT eye's t1 is made complete instead.

The fix, in the VR profile only, no new key (`fix.fss_eye_sync` untouched):

- In `hookedDrawInstanced`, owner context, while the scanner is on show (chrome
  held or the FSS latch open), before the instrument sees the draw:
  - A draw with VS `DEF19B035D5EDEDC` + PS `831DF02EBA8AE814` (both whole
    64-bit hashes) is a downsample. Its full pipeline state is kept for the
    frame, keyed by its render target: input layout, vertex and index buffers,
    topology, VS and PS, GS, vertex and pixel constant buffers b0-b3, vertex and
    pixel textures and samplers, rasterizer, viewports, scissors, render targets
    and depth, blend, depth-stencil, and the draw arguments. The constant
    buffers are COPIED on the GPU timeline into private buffers at that draw,
    so a Map between #216 and #219 cannot change what the replay reads.
  - The first draw of the reveal composite (VS `953C8123AD8DC13B` + PS
    `A473A867BDDBC342`) in the frame reads its t1 resource. If a downsample
    recorded this frame rendered into that same resource, that downsample is
    replayed: the state before is saved, the downsample's state is applied
    (its inputs first, so T121 is never both an input and a target), the draw
    is issued, and the state before is applied again (targets first, then the
    inputs, so T121 is an input again). The composite then draws. Only the first
    composite of a frame decides; the right composite, and the game's own second
    downsample, are untouched.
  - It never matches on the hashes alone: PS 831DF02E is a generic copy. A
    downsample into any other texture is recorded and declined.
- The replay goes through the game's un-hooked entry points (draw, render
  targets, shaders, textures, constant buffers, blend, viewports), so none of
  vscreen's state tracking sees it. The first 8 replays of a capture read the
  whole context state back and compare it with the state before.
- Flat is untouched: the check sits behind `!runtimeFlatProfile()`, and the
  watch bool is false in flat.

What the replay's inputs were checked against. The v11 log has no census line
for the downsample (the census lists draws into eye-sized targets only) and the
census table that names the composite's t1 shows it read only by the two
composites, so "no cb contents changed between #216 and #219" could not be read
from it. The design does not depend on it: constant buffers are snapshotted at
#216, and the textures and states are held by reference. The layer SRV (t0) is
the layer's own view; at #219 the layer holds group B (the whole point).
Not covered: pixel-shader UAVs on the output merger (the composite binds none
in any capture so far; a replay would unbind them).

Logging (per capture, plus the v11 composite diff, unchanged):

- `reveal replay f=F: ...` lines, at most 8 a capture: the replay done (with the
  state check result), a composite that declined and why, a downsample whose
  state could not be kept.
- `window N reveal replay -- frames with draws of the reveal shaders A, with a
  reveal composite B; downsample REPLAYED before the first composite in C
  frame(s) (D replay(s), E failed); state read back after the first replays: F
  equal, G DIFFERENT; downsamples recorded ...; composites declined, in frames:
  no downsample recorded, recorded for another texture than t1 (hash-only
  match), t1 unresolved, flat. <verdict>`. The verdict is one of: "the replay
  fired"; "no reveal composite drew in this capture, so the replay had nothing
  to do"; "THE REPLAY NEVER FIRED although reveal composites drew: this run did
  not test the fix".

Rig (fss_tiles_test, 337 checks): `RevealReplay` decisions (the stock order
replays; a hash-only match on another target is declined; no replay without a
recorded downsample that frame, including one recorded after the composite;
an unresolved t1; flat never replays; a failed record is never replayed; only
the first composite decides); and on a WARP device with the debug layer, the
real `captureState` / `replayDraw`: the replay draws the downsample's colour
from the private copy of the constant buffer after the original was rewritten,
the composite's target is untouched, and every stage the draw touches (target,
t1, constants, rasterizer, blend, depth-stencil, sampler, viewport, scissor,
stencil reference, sample mask, blend factor) reads back equal to what it was.

Proving flight: `fix.fss_eye_sync = off`, one zoom (the glare shader dump is no
longer needed). Expected:

1. `capture N valid=yes`, and `window N reveal replay -- ... replayed ... in
   about as many frames as the reveal composite drew`, with 0 DIFFERENT state
   reads.
2. `window N composite diff t1` shows no content difference between the eyes
   in frames +0..+27 (the v11 run had 22 of 25 frames differing), and the left
   fingerprint of t1 equals the right.
3. The eye runs: both eyes show the right eye's look, no black squares.
4. If the summary says THE REPLAY NEVER FIRED, the match failed and the
   declined counts say why.
Risks: the replay is one extra tiny draw a frame; if the build ever draws
different layer content per eye, the left t1 would show the finished layer (the
right eye's own look).

## 2026-10-15 (close): the cleanup branch `claude/fss-reveal-fix`

Sean approved the cleanup after flight T (the v12 replay on the test branch,
window 5): t1 never differed across 20 eye pairs; the replay fired in 17
frames, 0 failed, 8 of 8 state read-backs equal. The branch is cut from main and
carries only the replay, the removals below, and this doc.

**The replay on main.**

- `src/d3d11/fss_reveal_core.h` (the decisions, pure), `fss_reveal_replay.h`
  (capture a draw's whole pipeline state, replay it, restore and compare;
  the test branch called it fss_tiles_replay.h), `fss_reveal.h/.cpp` (the hook
  glue and the log). `hookedDrawInstanced` asks `fssRevealWatching()`, one bool.
- The gate is "the scanner is on show": the FSS mode latch is open, or the chrome
  tracker matched within five frames. The latch is now always wanted in the VR
  profile (it was wanted by `fss_eye_sync = on`, the default). Flat never arms it.
- The 6b0 arrival trigger, the capture windows, the dumps, the producer lines and
  the composite diff are not on main: the replay needs none of them.
- Rig `tools/fss_reveal_test` (49 checks): the decisions, the verdict wording,
  the real capture and replay on WARP with the debug layer (state equal before
  and after, constants read from a private copy), and source pins on the hook.

**What the log says, and what it says if the code never ran.**

1. Once a process, at the first configure: `fss reveal fix: armed.` (or
   `not armed (the flat profile ...)`). If this line is missing, the code
   never ran.
2. Once per scanner session (the watch has been off for 30 frames): `fss reveal
   fix: scanner session N (frames a-b) -- DrawInstanced calls checked, draws of
   the reveal shaders, frames with a reveal composite, with a replay (replays,
   failed), state read back after the first replays (equal, DIFFERENT),
   downsamples recorded (dropped, failed), composites declined by reason (no
   downsample recorded; recorded for another texture than t1, the hash-only
   match; t1 unresolved; flat). <verdict>`. The verdict is one of: "the replay
   fired"; "...fired, but the context state read back after a replay DIFFERED
   ...: check this run"; "no reveal composite drew in this scanner session, so
   the replay had nothing to do"; "THE REPLAY NEVER FIRED although reveal
   composites drew: this run did not test the fix"; "NO DrawInstanced call was
   checked in this scanner session: the reveal fix's hook did not run". A
   scanner visit with no session line means the gate never opened (check the
   `fss mode latch` lines).
3. At most six detail lines a session: the first replays (with the state
   check), a composite that declined and why, a downsample whose state could not
   be kept.
Armed line present, session line with "hook did not run": the hook is wired but
the watch never turned on or the draw path was bypassed. No armed line: the
module did not configure. Session line "never fired": the hashes or the t1
match failed in this build; the decline counts say which.

**Removed keys, quoted with the behaviour they had.**

- `fix.fss_eye_sync` (shipped `on`): one key for the black-squares fix.
  `on` = heal + sync; `off` = stock; developer values `heal` (the arrival fill),
  `mirror` (stamp the left's blacks into the right), `sync`/`lockstep` (the
  composite input freeze), `steady` (the composite constants snapshot), and, on
  the test branch only, `tiles`. Its legacy spellings and its moved-from aliases
  (the two retired keys it replaced) went with it; the edvr.ini section "When
  the eyes disagree", its `ui:` menu row and every test that exercised it are
  gone. An old ini that still carries the line is named by the config audit as a
  key this version does not read (the same path as every earlier removal).
- `advanced.census_fss_jump` (shipped 0, developer): armed a draw census at a
  scanner zoom's first frame. Its block is gone.

**Removed mechanisms and files.**

- The eye heal and mirror: `fss_heal.cpp/.h`, the `edvrFssHealLeft` export, the
  two compute shaders and their precompiled rows (`fss_heal_cs`, `fss_mirror_cs`;
  shader-build self-test 65 to 63 shaders, 14 to 12 extra contracts), the
  `DoorFssHeal` census section.
- The native-runtime path to the heal: `native_fss.cpp`, `native_fss.h`,
  `native_fss_client.h`, the `edvrAcquireNativeFss` export (build.bat, the
  installer's export probe, `openxr_pe.py`), the host's FSS client, its
  deferred-eye logic and the `first_healed_eye` / `fss_healed` / `fss_deferred`
  trace fields (`native_features_summary` keeps its other fields); the host's
  treatment cases lost the heal stub and its two scenarios (8 to 6).
- The composite input freeze and the constants snapshot: `fss_reveal.cpp/.h`
  were rewritten as the replay; the Map/Unmap/UpdateSubresource tees, the
  `kFssReveal` verdict with its rungs and census row (`AlteredFix::FssReveal`;
  the census rotation counts in gpu_census_test dropped by one).
- Arrival plumbing that only they read: the zoom-press watchers
  (`ExplorationFSSMiniZoomIn` / `ZoomIn` keys and pads), `deviceHookTakeFssZoomPress`,
  the arrival window with its stamp and "fss arrival" lines, and the FSS pacing
  log tick. The shared-memory slots in frame_flag stay (the layout is versioned by
  the mapping's name); their dead accessors went.
- Test-side: `tools/native_fss_test`, the installer test's pre-0.11 pair merge case.

**Kept and left alone, on purpose.** The chrome tracker and the panel rect
deriver with its scenery skip: they were switched on by the same key, but the
skip is shipped behaviour in the scanner and the tracker also feeds the temporal
pass and the UI depth. They now run unconditionally, as they did at the default.
Their only other reader, the heal's rect, is gone: whether to keep the deriver
for the skip alone is the first item for a later cleanup.

## Open

- Re-verify with the OpenXR Toolkit ON: the old fix was proven Toolkit-off
  and the replay has only flown without it; the Toolkit's scaler re-makes
  per-eye content downstream and deserves one confirmation flight.
- The theater is parked, not dead ("2.0 to return"): the virtual-screen
  treatment, the homography stitch and the centering servo are whole in
  the code behind `fss_theater = 0`.
- The flash detector withholds a couple of frames at every fresh FSS
  zoom (the body camera at ~9,880 units until parked-certification) — a
  real hitch on exactly this transition, and its own arc.
- `fix.fss_res` is still opt-in; promoting it to a shipped default is a
  release-train decision (cost: 4x the pixels for one layer, FSS only).
- The census still does not record `GenerateMips`,
  `ClearUnorderedAccessView*`, or the contents of command lists
  (`ExecuteCommandList` is seen, its interior is not). The 272x268 tile
  maps are now known to be written by compute (`22786F6DE290C577`, visible
  as DCX) — the 2026-08-24 mystery of "nothing writes them" is resolved by
  the dispatch recording.
- The CB watch reads b0 only. A reveal value living in a later CB slot
  would need the watch widened — one more slot pair, same tee.

## 2026-10-15: the replay is priced in the GPU census

Review of main 9fe5b257, finding 5: the downsample replay and its private constant-buffer copies had no cost scope, and the removal of the old
reveal section left the census with no FSS line at all. `GpuCensusSection::FrameFssReveal` ("FSS reveal replay" on the in-frame list, in the
in-frame total) now wraps the two GPU work sites in `fss_reveal_replay.h`: the pooled copies in `captureState` and the replay draw with its
two state applications in `replayDraw`. The hook glue in `fss_reveal.cpp` opens none, so the no-op watcher calls are not timed. The section
has no rotation turn until called in a window, so a session with no scanner samples as before. `fss_reveal_test` counts the scopes for a
record (one) and a replay (one) and for a failed replay or a bare read-back (none); `gpu_census_test` holds the section's place, turn and
name in the 30 s line.
