# Cockpit holograms and icons over the sky: depth for temporal AA

## Status

- **State (2026-09-25):** the holograms, the radar, the distance digits
  and the weapons-panel text are FIXED and flown (`## Round history`
  below). Round 8 (411751ec) is flown in the hangar only, so the
  triangles are untested:
  - the target reticle's triangles (vs `71DD8B8B09060A81` writes z = 0
    but keeps a real w) take depth = a + b / w from an EDVR pixel shader
    matched to that VS's output signature (the rig proved on WARP that a
    D3D11 pixel shader's SV_Position.w is that clip w, not 1/w);
  - the near-light pass runs one thread per pixel; one thread scanning
    each 8x8 block serially cost ~0.3 ms/frame in round 7.

  Round 9 (d9f86b09) is flown in the hangar (dump 123118) and fixed the
  station text. Rounds 6 and 7's dark rule had stamped the gaps around
  the letters at the element's own depth (dumps 115012/115037). A dark
  gap now takes a filler depth just inside the cockpit radius, and
  GREATER leaves any nearer scene surface alone.

  Round 10 BUILT, NOT FLOWN: the HEATSINK label still doubled. The
  resolve had overwritten its UI-covered letters with another listed
  draw's nearer footprint depth, breaking their exact record motion.
  Now the resolve never writes a UI-covered pixel. The UI depth pass
  owns those pixels, and their record-depth match was 100% with the
  pass off.
- **Open:**
  - Which of the five radar-contact families paints the bars (listed
    from the draw ledger, not individually confirmed).
  - Target markers on a target inside 10 m: the holo material draws them
    at the target, so the cockpit radius stops excluding them there.
- **Keys:** `advanced.temporal_aa_hologram_depth` (default on), with
  `advanced.temporal_aa_hologram_families`, `_floor` and `_share`, all
  live. The pass runs inside `fix.temporal_aa`'s interface depth
  (`ui_depth.cpp`) and needs it on.
- **Mechanism:** each listed draw is issued twice more after the game's:
  first its blended light into a contribution scratch (a cockpit family
  only where nearer than `advanced.temporal_aa_ship_metres`, 10 m), then
  its nearest depth. Once per eye, before the temporal pass reads the
  private AA depth copy, a resolve stamps that depth where the displayed
  pixel clears the floor and the element supplies at least `share` of
  the game's own HDR light there. A dark pixel of a cockpit element is
  covered only where its own near-light block or a neighbour holds the
  element's light, and from round 9 at a filler depth just inside the
  radius, not the element's. The lists: `## Families` below.
- **Ruled out:** `## Ruled out` below, plus the `ruled out:` lines in the
  flight entries.
- **Next flight** (Frontier, Pimax OpenXR, the log's build line naming
  round 10):
  1. Docked in the hangar, the same head motion with the pass on: the
     HEATSINK label and the station text should be as crisp as with it
     off. An eye dump: its `Z` should equal its `HoloCoverage` depth on
     ~100% of UI-covered pixels with a record, as in pass-off 115037.
  2. In space, rolling with the weapons panel over sky: the text must
     stay crisp (round 6's gain must survive the filler). An eye dump.
  3. A target locked, flying toward it and turning: the reticle's
     triangles should stay sharp.

  Read these log lines:
  - the 30 s census's `world markers N draws/frame, element-depth
    samples p50`: above 0 with a target locked (it read 0 through round
    7; 0 now means the matched PS never drew);
  - no `hologram depth: shader compile failed` line;
  - `EDVR GPU census`: "hologram resolve+celestial" well under round 7's
    ~0.32 ms/frame.

## The defect

Cockpit holograms and the supercruise radar's star icon write no depth.
Where the sky is behind them, their pixels take the sky's depth and
motion while they move with the cockpit, and FSR's history lands about a
pixel a frame off (worst under FSR, Sean 2026-09-24). Over the cockpit
they inherit cockpit motion and stay sharp.

## Round history

- Round 1, the first draft, never flew: review found it alpha-blind,
  among other defects (`## Ruled out`).
- Round 2 (986ebaad) FLOWN 2026-09-24 15:56, still blurred: the target
  sphere and the radar contacts were unlisted, and the share test
  compared HDR light against the tonemapped image.
- Round 3 (bbaf99f4) FLOWN OK 16:30 (Sean: "That fixed the holograms and
  the radar"): the share reads the game's own RT0 in its view format,
  the floor sits on the displayed pixel, and eleven families are listed.
- Round 4 (36b94518) FLOWN 17:51, triangles and digits unchanged. It
  added a world-marker class for the reticle's triangles and a census of
  the UI content tracker; both found their causes (flight
  20260924_175113, below).
- Round 5 (c9cab91e) FLOWN 18:50: FIXED the ghosting digits (UiContent
  compares 4x4 blocks by hash) and proved the triangles' vertex shader
  writes z = 0.
- Round 6 (5165a76f) FLOWN 2026-09-25 08:04. For the weapons-panel blur
  (flight 20260925_050051, below), a dark pixel inside a listed element's
  own footprint, within the cockpit radius, took that element's depth,
  so the gaps between glyphs moved with the panel. It REGRESSED (flight
  20260925_080452, dump eye_080707): sky outside the panel's visible
  frame, inside its oversized null-PS footprint, was stamped too, and its
  parallax made the HUD swim with head translation (0.7-3.6 mm/frame,
  ~2 px/mm at 0.6 m). A steady roll has no head translation, so it
  stayed sharp.
- Round 7 (a5edd2da) FLOWN 10:32 (flight 20260925_103225, below): a dark
  pixel takes the element's depth only near the element's own light,
  through a per-eye near-light map (1/8 res, R8_UNORM, a compute pass
  marking a block when one of its own pixels passes the bright branch's
  test). The resolve covers a dark pixel only when its own block or one
  of its 8 neighbours is set: roughly the 8-16 px around glyph light (sky
  MV 5-13 px/frame during rolls), not an open quad 40 px away. In the
  dump the sky beside the weapons panel is 5% stamped (35% in round 6).
- Round 8 BUILT, NOT FLOWN (Status).

## Families

Eleven cockpit families are built in: holo panels `81216C77F90DEDD6`
(also the ship/shield hologram, ps `A2965EC2931A39C8`); the icon core
`F8D8A92E96419901` (ps `16196F69ADE35E77`); the corona family
`D1281DF454A153AD` (ps `97DBC87FCAA429C4`: the icon's glow AND the real
sun's corona); the stalks `DF3503CD07F9B10C` (caught by the pixel probe
at the radar, frame 8548) and `5453D19B6D362364` (drawn next to it in
the ledger's radar section); the target hologram sphere
`5559BD94B6852E83` (two premultiplied quads, ps
`EA02FAC2BD6C643C`/`E95634B0F61D218F`, named by the pixel probe, flight
20260924_155636); the five radar-contact families `A2C2D5510BF1926D`,
`9B34C331902DC1ED`, `9611A454527F7FEB`, `B932058F26B76691`,
`94D5C556DFD6D705` (named by their position in the draw ledger, right
after the two stalks in each eye's cockpit section -- which one paints
the visible bars is still open). The canopy `8C091FFD08644E02` is
refused even if listed.

A separate, second built-in list holds one WORLD MARKER, not
radius-clipped: the target reticle's 3D triangles `71DD8B8B09060A81`
(round 4), whose depth comes from its own matched pixel shader (round
8). Extras from `advanced.temporal_aa_hologram_families` always join the
cockpit list, never this one.

## Ruled out (do not re-propose)

- `advanced.ui_replay` (removed 48ad7689) would not have fixed it: it
  logged captured=0 on every flown rig and took only draws already
  marked UI.
- Reactive bias or sharpening: the motion is wrong, fix the motion.
- kHoloPanel's alpha-floor coverage for the ship hologram: its strokes
  never reach alpha 0.5 (0 of 782 over-sky pixels covered, eye_135907).
- Listing the icon in `advanced.ui_depth_families`: every direct family
  needs a coverage shader, else "no supported coverage shader".
- A contribution read as the raw shader output's luma under a MAX blend
  (the first draft, never flown): it ignores the blend's alpha, so a
  constant-tint glow with an alpha falloff would stamp its whole quad --
  the "small blurry quads under each bracket" of 2026-09-09 again.
- The share test against the submitted (tonemapped) image, as flown in
  986ebaad: the holograms draw into the HDR scene target
  (R11G11B10_FLOAT) before tonemapping, so the contribution is about
  3.6x smaller than the displayed pixel. The test passed 2% of the
  over-sky pixels that cleared the floor (eye_155832).

## Evidence, 2026-09-24

All Frontier, Pimax OpenXR, FSR 3.1.2 at 2037x1969 per eye.

- **eye_113353:**
  - Frame 17103: the raw input `C00` is crisp. FSR's output `P00` is
    smeared by a vertical streak, and the final `T00` is the same.
  - At the icon's orange pixels: Z 0 (sky), MV (-0.12,+1.05) px (the
    sky's), and UI, Bias, UiEdits and HoloCoverage all 0.
  - On the radar disc beneath: Z 0.011, MV (-0.02,+0.10).
- **eye_133811:** `advanced.pixel_probe` at frame 15489 named the icon core
  (quad x3, SRC_ALPHA/ONE, depth off). The target hologram (grey sphere)
  and the ship hologram (red wireframe) show the same split: over sky
  they carry the sky's MV (+0.29..+0.32, +0.06..+0.13) with no coverage;
  over the cockpit, cockpit MV.
- **eye_135907:** probe frame 17848, with `advanced.glare_shader_dump`
  on. The bytecode is in `edvr_logs\shaders`.
  - The icon's glow is the corona family (SRC_ALPHA/ONE, depth off).
  - The ship hologram is `81216C77`/`A2965EC2931A39C8`, 435 vertices,
    blended ONE/INV_SRC_ALPHA (premultiplied). Its GREATER_EQUAL depth
    test writes nothing.
  - Its existing coverage marked 0 of 782 over-sky pixels and 4% over
    the cockpit.
  - The eye target is `R8G8B8A8_TYPELESS`, viewed `R8G8B8A8_UNORM` by the
    temporal pass.

## Flight 20260924_155636 (986ebaad, Frontier, FSR, rolling)

Sean: "just rolling the ship visibly blurs those inner quarters". The
eye dump eye_155832 was taken while rolling. Sky MV p50 was 13.3 px,
cockpit 0.26 px.

- The pass ran: 27.5 listed draws a frame, both eyes resolved every
  frame, no declines, 5788 stamped pixels per eye-frame (p50).
- **Target hologram sphere:** vs `5559BD94B6852E83`, two premultiplied
  quads with depth off (ps `EA02FAC2BD6C643C`, `E95634B0F61D218F`),
  named by the pixel probe (frame 8548, eye 0, point 3). It was not
  listed. Over sky (1080 px) it had 1% contribution and 1% stamped, with
  MV (-3.85,-6.14), which is the sky's.
- **Radar contact bars over sky (1103 px):** 0% contribution. The draw
  ledger (`pool\draws_155832.bin`) shows the radar cluster right after
  the stalks in each eye's cockpit section, and none of it was listed:
  - `A2C2D5510BF1926D` (6 verts, 5 instances);
  - `9B34C331902DC1ED`;
  - `9611A454527F7FEB`;
  - `B932058F26B76691`;
  - `94D5C556DFD6D705` (6 verts, 19 instances).
- **Listed elements:** the share-space defect above refused them.
- The first-listed-draw line gave the target as format 26 (HDR). The
  hologram section is the last group of draws into each eye's HDR target.

## Flight 20260924_163011 (bbaf99f4, round 3, Frontier, FSR)

Sean: "That fixed the holograms and the radar." The census stamped a
median of 34k-63k pixels per eye-frame, and both fallback counters read
0. Two further reports came with dumps eye_163401 and eye_163405
(supercruise, MACLEOD MARKET targeted) and eye_163515 (normal space,
ship BOKESY at 1.65 km):

- **The target reticle's 3D triangles go indistinct with speed.** They
  have no coverage at all: Z 0, UI 0, HoloCoverage 0, Bias 0. They carry
  the sky's MV (+0.59,-0.89), while the ship they bracket moves at
  (+0.14,+0.27): parallax at 1.6 km in normal space.
  - In the ledger (frame 27528), vs `71DD8B8B09060A81` draws 72
    vertices, right after the canopy and only with a ship targeted.
    That is three triangular prisms of 24 vertices each. Believed, by
    shape, to be the triangles.
  - Fix BUILT (round 4): a world-marker class, not clipped to the
    cockpit radius, so the triangles get their own depth at the target.
    Not yet flown.
- **Changing distance digits ghost.** In normal space, "1.65km BOKESY"
  is already stamped at the target's depth (1646 m) by the holo family
  coverage, and moves with the ship's MV. In supercruise the reticle and
  text are stamped at ~34,000 km with MV equal to the sky's. So the
  motion is right; the upscaler blends the previous frame's digits in.
  - UiEdits reads 0 on the digits, and Bias is 125 (half reactive).
  - The UI content tracker is thrashing: 1,545 evictions and 281k
    declines against 39k comparisons by 16:35.
  - Not yet known whether the text's source is a glyph atlas (declined
    by design) or an evicted surface. A per-draw census at the eye run's
    first frame is BUILT (round 4): `UiContent::lastDecision` names why
    (nine decline reasons, hit, reset, updated), with the entry's age and
    evictions this call; capped at 64 lines, one summary line after.
    Not yet flown -- the next dump settles which reason the digits give.

## Flight 20260924_175113 (36b94518, round 4, Frontier, FSR)

Sean: "still looks the same moving towards a target". Eye dump
eye_175314: a friendly ship, MALTE TITZE, at 1.99 km.

- **Triangles:** the world marker `71DD8B8B09060A81` is confirmed as
  the triangles, with HoloContribution > 0 on 100% of their pixels. Yet
  0% were stamped, with Z sky and the sky's MV. Every resolve test
  should pass there except "element depth > 0", so their raster depth
  is 0: drawn at infinity.
  - The diamond bracket and the text beside them are stamped at ~1971 m
    by the holo family, with MV (+0.57,+0.26); the ship reads
    (+0.40,+0.25) at 2138 m.
  - Two mechanisms remain: a viewport with MinDepth = MaxDepth = 0, or
    a vertex shader writing z = 0. Round 5 overrides the first and
    counts samples to tell them apart.
- **Digits:** the census (frame 12561) found the target's name and
  distance text composited by vs `E508648660A352B2` from a 5895x5158
  RGBA8 surface (fix.ui_quality 1.25: the engine sizing panels x2.5).
  - At 6 bytes a texel that is 182 MB, so it was "declined (over
    budget)" every frame.
  - Two cockpit panels (2620x1637 and 2814x2302) filled 64.6 MB of the
    64 MiB, so the other two panels (1310x1962 and 3930x844) were
    declined with "no free entry".
- ruled out: the tracker thrashing through evictions as the digits'
  cause, because the census showed 0 evictions in the frame. The
  surface is declined before any entry is considered.

## Flight 20260924_185058 (c9cab91e, round 5, Frontier, FSR)

- **Digits FIXED.** Sean: "That fixed the numbers!" The block-digest
  tracker now covers the 5895x5158 panel.
- **Triangles unchanged**; eye dump eye_185339 shows a Condor at
  1.21 km. Round 5's diagnostics settle the mechanism:
  - The first-world-marker line: game viewport depth 0.000..1.000, DSS
    enable 0, RS DepthClipEnable 1, blend SRC_ALPHA/ONE.
  - The census: world markers 0.89 draws/frame, element-depth samples
    p50 0 over 512 passes, with the viewport override in place.
  - So the triangles' vertex shader writes z = 0 itself.
  - ruled out: a viewport with MinDepth = MaxDepth = 0 as the cause,
    because the game's viewport reads 0..1.
- **Next:** the triangles' VS bytecode. `advanced.glare_shader_dump`
  was set on disk after launch, so the next launch captures it.
  - If clip w carries the view distance, a pixel shader matched to that
    VS's output signature can write the true depth from SV_Position.w.
  - Elite's VS outputs put SV_POSITION after the user outputs (o3 in
    vs_F8D8A92E96419901), so a generic SV_Position-only PS would not
    link.
  - `scratchpad radar\disasm.py` wraps D3DDisassemble for reading it.

## Flight 20260925_050051 (c9cab91e = tag v0.18.0-rc.1, Frontier, FSR)

Sean: the left-side weapons text "can sometimes go a little blurry"
when rolling. Eye dump eye_050423, rolling ~1.6 deg/frame. The weapons
panel ("SECONDARY A", three MULTI-CANNON rows) is holo family, over sky
through the canopy.

- **Stamping:**
  - glyphs 100% stamped at 0.6 m, |MV| 0.39;
  - AA edges 98% stamped;
  - the dark gaps between glyphs and rows (display < 13/255) only 8%,
    carrying the sky's motion (|MV| p50 5.0 px).
- **Sharpness:** across the 16 crops the output text keeps 0.60-0.65 of
  its input edge strength (99th-percentile gradient), except crops 2-3
  at 0.70-0.73.
- **Natural experiment:** crops 2-4 are a camera-registration glitch.
  - Rows from the "parked" auxiliary camera the transition-flash
    tracker logs were rejected at 19996 and 19998 (rowsOk 0).
  - 19997's prev and now were both that camera: a zero delta, accepted.
  - 19998 then carried that zero.
  - For those frames the world, the gaps included, moved ~zero like the
    glyphs, and the text was crisp.
  - So sky-motion gaps pull glyph history across the motion edge and
    soften the text; gaps moving with the panel keep it crisp.
- **Fix, round 6 (BUILT, NOT FLOWN):** dark pixels inside a cockpit-range
  element's own footprint take its depth. Far elements (the corona,
  markers at a target) never claim dark pixels; bright pixels keep the
  share test.
- **Separate finding, not this arc:** the session's census reads "the
  camera's delta was dropped on 120 eye-frames as another camera's",
  about one event every 3.5 s. When a rejected frame is followed by an
  aux-to-aux zero delta, that zero is accepted and then carried: two
  frames of wrong world motion during a roll. That belongs to
  temporal_pass.cpp's registration (after a rejected frame, prev should
  stay the last accepted rows).

## Flight 20260925_080452 (e52089de: round 6 + camera fix + GPU census)

Sean: HUD elements "swim with head movement but under constant ship roll
they look sharp". Setting `temporal_aa_hologram_depth = off` FIXES it (live
toggle, same flight).

Eye dump eye_080707:
- The weapons panel's glyphs and gaps are all stamped at 0.60 m (round 6
  works for the text).
- A sky patch OUTSIDE the panel's visible frame is 35% stamped at 0.57 m.
  The panel's null-PS footprint (its quad) reaches far past what it draws.
- The head translates 0.7-3.6 mm per frame, about 2 px of parallax per mm
  at 0.6 m. Stamped sky takes that parallax while its stars stay still.

Separately, a head flick of 4.5 deg/frame outran the camera-row chooser's
3-deg window (rowsOk 0 at crops 1 and 4). That belongs to the camera-rows
arc.

- ruled out: round 6's dark rule over the element's WHOLE footprint,
  because it makes the region around the HUD swim with head translation.
  Round 7 limits dark stamping to pixels within about 8-16 px of the
  element's own visible light, via a 1/8-resolution near-light map.

## Flight 20260925_103225 (290c76e4: round 7 + census fix)

Two eye dumps: eye_103555 (the HUD "smearing/blurring under ship/head
motion") and eye_103612 (the triangles on a targeted ship).
`glare_shader_dump` was on at launch.

- **Triangles' vertex shader captured** (`vs_71DD8B8B09060A81.dxbc`, 640
  bytes).
  - Output signature: TEXCOORD6 at o0, TEXCOORD7 at o1, SV_POSITION at o2.
  - `mov o2.z, l(0)` forces depth to 0, while x, y and w come from cb0
    rows 4, 5 and 7: a real perspective projection whose w is the view
    distance.
  - Round 8 writes the triangles' depth as near/w from a pixel shader
    matched to that signature.
- **HUD dump (eye_103555):**
  - Round 7 behaves as designed: the sky beside the weapons panel is 5%
    stamped (35% in round 6), the glyphs 100% at 0.59 m with cockpit
    motion, and the gaps near the glyphs are stamped.
  - The camera rows are rejected on 10 of the 16 crops, alternating
    ~6 deg and ~0 deg frame to frame (two cameras); crops 6-7 accepted
    2.60 deg and 0.00 deg against a head turn of ~0.2 deg. So for those
    frames the world path's motion is wrong.
  - The session drop count (128 eye-frames) is in line with earlier
    flights (120 and 46), and the camera-rows doc notes the eye run's
    own hitch perturbs registration. So the burst may be the dump's own
    doing.
  - Next: the same live A/B. If the smear persists with the pass off, it
    belongs to the camera path or the upscaler, not this arc.
- **Census:** the per-draw figures are now sensible (engine velocity 0.034
  ms at 3.7 calls/frame), and the timer floor reads 0.0 us/pair on this
  GPU. The near-light pass took "hologram resolve+celestial" from ~0.04
  to 0.32 ms/frame (one thread per block, serial); round 8 makes it one
  thread per pixel.

## Flight 20260925_112335 (411751ec: round 8), docked in a hangar

- Right build (v0.18.0-rc.1-20-g411751ec). Only the hangar was flown, so
  round 8's triangles are untested (no target).
- EDVR's depth probe lost the scene depth around 11:25:30 and found it
  again at 11:26:08 ("the scene's depth is in hand"). The hologram
  resolve never ran in between: that window's census shows 0 resolved
  and 0 declined. From 11:26:08 the passes ran (GPU census: hologram
  passes 21.5 calls/frame, resolve 1.85/frame) until Sean turned
  `advanced.temporal_aa_hologram_depth` off at 11:26:31.
- Sean: docked, head moving, the HUD smears with the pass on, and
  turning it off fixes it. The smear belongs to this pass.
- ruled out: the camera rows (or the eye dump's own hitch) as the cause
  of the HUD smear, because it follows the pass's live key with the ship
  docked and only the head moving.
- The DLSS reset Sean saw: two fresh history starts (one per eye)
  between 11:25:55 and 11:26:15. That is the moment the depth returned
  and the game camera flipped (11:26:02-04, 11:26:08). The log names no
  cause.
- Cost: "hologram resolve+celestial" read 0.177 ms/frame at 1.85
  calls/frame over the hangar window. Round 7 read ~0.32 in flight, a
  different scene, so this is not yet a clean comparison.
- Open hypotheses for the smear:
  - (a) see-through: a panel is translucent, so the lit hangar seen
    through it takes the panel's motion (0.6 m parallax) instead of its
    own (~20 m). Over black sky nothing behind it can smear;
  - (b) the stamped foreground depth reaches the upscaler's depth input
    or EDVR's occlusion rejection, which drops history along the panel's
    edges as the head moves.

  The discriminating evidence is a hangar dump pair, on then off, with
  the head translating.
- Code (read-only trace, 2026-09-25):
  - The temporal shader merges its depths, `max(Z, ZS, ZUI)`
    (`temporal_shader_source.h:275`); ZUI is the private copy the
    stamps land in. That merged depth drives `mv()`, and so the ship
    split at 10 m and the engine-record ownership test (a record whose
    depth does not match bit for bit is refused as stale). It is also
    written out as the upscaler's depth input (`ZC`, line 1053).
  - `backgroundHistoryHidden` (lines 521-541) compares last frame's
    merged depth around the reprojected position with this frame's. When
    something was nearer before, it writes an out-of-range motion
    vector, a history reset for that pixel. Both sides include the
    stamps, so a see-through layer moving over the background resets the
    pixels its edges uncover, by construction.
- Existing dumps confirm (b) happens. A reset shows in the MV input as
  the sentinel. Counting the 6 px band just outside stamped pixels
  (Z nearer than SceneZ) against pixels beyond 24 px of any stamp:

  | dump | band reset/frame | far reset/frame |
  |---|---|---|
  | 103612 (r7) | 1.07% by holo, 1.56% by dark stamps | 0.003% |
  | 103555 (r7) | 3.26% by holo, 12.74% by dark stamps | 1.64% |
  | 080707 (r6) | 0.82% / 0.65% | 0.004% |
  | 050423 (r5) | 5.42% by holo | 0.96% |
  | 185339 (r5) | 0.04% by holo | 0.03% |

  The far rates in 103555 and 050423 are the camera-row trouble those
  dumps also caught. "Dark stamps" are stamped pixels with no hologram
  contribution: rounds 6 and 7's dark rule, plus a few UI stamps.
  Whether (a) or (b) is what Sean sees in the hangar waits on that
  dump pair.

## Eye dumps 115012 (on) / 115037 (off), docked, head moving

Same session and build (411751ec), flight 20260925_114704: the pass on
for eye_115012, then off at 11:50:31 for eye_115037. The station
information text beside the station hologram ("MACLEOD MARKET ...") is
the smear.

- The output is doubled and bolded with the pass on, even though the
  head was nearly still in that frame (0.23 deg/frame). With the pass
  off it is crisp at 0.67 deg/frame and an MV median of 11.8 px/frame.
  DLSS's own output (`DlssBeforeUi`) already shows it, and the game's
  input frame does not.
- On the letters themselves nothing differs between the two dumps. Both
  are 100% UI-stamped at 1.60 m (the UI depth pass, not this one), with
  the same UI and bias masks, 0 resets, and exact panel motion (a
  `holoPixel` record claim).
- What differs is the dark gaps around the letters. 75% are stamped with
  the pass on, against 11% (UI stamps) with it off. All of them are the
  DARK branch: display <= floor, median 7/255, none from the bright
  branch. They sit at the element's depth, a median 1 mm NEARER than the
  letters. They take motion from the depth path, 0.17 px/frame off from
  the letters' exact record motion (the UI stamps in the off-dump match
  the letters to 0.00 px). With the pass off, the gaps are the console
  at 3.9 m behind the text.
- Depth DLSS receives: with the pass on, a flat, blocky slab where the
  letters no longer show, with frame-to-frame changes at its 8 px block
  edges. With the pass off, letter-shaped depth: glyphs at 1.6 m against
  the console at 3.9 m.
- ruled out: history resets as the hangar text smear, because the text
  box holds 0 resets in eye_115012.
- ruled out: the background showing through the panel (a) as the hangar
  text smear, because the letters themselves double while their depth,
  motion and masks match the off-dump.
- Cause: rounds 6 and 7's dark rule. It writes the element's own depth
  into the gaps, which erases the depth edge DLSS uses to keep a
  letter's history apart from its surroundings. Those gaps then carry a
  motion that differs slightly from the letters' exact one, and DLSS
  bleeds it into the strokes. In the off-dump the gaps' motion differs
  more (0.5 px/frame, parallax to 3.9 m), but a 2.3 m depth edge keeps
  the text crisp.
- Round 9 (below, Status): a dark gap takes a filler depth just inside
  the cockpit radius instead of the element's own. Under roll it still
  moves with the cockpit, not the sky, which was round 6's purpose, and
  it stays behind the element, so a depth edge remains. The resolve's
  existing GREATER test leaves any scene surface nearer than the filler
  alone, such as the console at 3.9 m. There the gap keeps the depth and
  motion it had with the pass off.

## Eye dump 123118 (d9f86b09: round 9), docked, pass on

Flight 20260925_122802, pass switched on at 12:31:03 and the dump taken
at 12:31:18. Round 9 works on the station text: the crop's dark stamps
fell from 70,780 px to 3,904, because the console gaps keep their own
depth. Sean: the HEATSINK label still smears, and the dump shows its
letters and "1/3" doubled. They were doubled in round 8's pass-on dump
too, and crisp in the pass-off dump 115037.

- The label's letters are UI-covered and carry holo record 1, at a
  coverage depth of 1.52 m. With the pass off, the merged depth matches
  that on 97% of them, so `holoPixel` gives them the label's exact
  motion.
- With the pass on, in rounds 8 and 9 alike, the resolve overwrites
  them with 0.59 m. That is the element-depth pass's NEAREST listed
  footprint (a nearer panel's transparent quad, most likely), not the
  label's. The match falls to 0% (r8) and 7% (r9), so the letters fall
  back to depth-path motion at the wrong depth, and head translation
  doubles them.
- Frame-wide, the pass-off dump holds 147,616 UI-covered pixels with a
  holo record, 100% at the record's depth. With the pass on, 2.0% (r8)
  and 4.1% (r9) of them are overwritten nearer. Holo records exist only
  on UI-covered pixels (0 elsewhere).
- Cause: the element depth is the nearest listed draw's depth,
  transparent parts included, so where footprints overlap it can belong
  to an element that supplies none of the light. On UI-covered pixels
  that overrides a depth the UI pass had exactly right.
- Round 10: the resolve never writes a UI-covered pixel. Those pixels
  belong to the UI depth pass and their records. The near-light map
  still counts their light, so gaps between UI letters keep the filler.

## Decisions, 2026-09-24

- Sean chose one generic pass over per-family coverage shaders plus
  floor tuning.
- The review of the first draft, before any flight, changed four things:
  - The light is the game's own source blend factor times the shader
    output, summed. That is exactly what an additive or premultiplied
    draw adds.
  - The floor is measured on the display-encoded brightest channel, and
    a share-of-pixel test is added against the finished colour.
  - The cockpit radius is a rasterizer clip, so the real sun's corona
    never contributes.
  - The resolve draw sets its own viewport and cull state and goes past
    EDVR's hooks (`vScreen*Raw`). The classifier accepts a draw with no
    depth buffer bound.
