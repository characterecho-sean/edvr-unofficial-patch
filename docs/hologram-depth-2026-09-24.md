# Cockpit holograms and icons over the sky: depth for temporal AA

## Status

- **State:** round 3 (bbaf99f4) FLOWN OK 2026-09-24 16:30. Sean: "That
  fixed the holograms and the radar." It was the third build:
  - 986ebaad blurred, for the two defects below;
  - round 3 reads the share from the game's own RT0 in its view format,
    puts the floor on the displayed pixel, and lists eleven families.

  Round 4 BUILT, NOT FLOWN: a world-marker class for the target reticle's
  3D triangles (`71DD8B8B09060A81`, its own built-in list, not radius-
  clipped -- `ContributionBegin` picks a DepthEnable-FALSE state for it;
  `holoScratchPrepare`'s element-depth clear moved to 0 so its depth
  survives at any range; the resolve's radius check is now `d>0`), and an
  eye-run census of the UI content tracker for the ghosting digits
  (flight 20260924_163011, below) -- `UiContent::lastDecision` plus a
  capped per-draw log and one frame summary, gated on
  `objectProbeLedgerActive()`'s rising edge (ui_depth.cpp, ui_content.h).
  `advanced.temporal_aa_hologram_depth` (default on), with
  `advanced.temporal_aa_hologram_families`, `_floor` and `_share`. It runs
  inside `fix.temporal_aa`'s interface depth (`ui_depth.cpp`) and needs it on.
- **The defect:** cockpit holograms and the supercruise radar's star icon
  write no depth. Where the sky is behind them, their pixels take the
  sky's depth and motion while they move with the cockpit, and FSR's
  history lands about a pixel a frame off (worst under FSR, Sean
  2026-09-24). Over the cockpit they inherit cockpit motion and stay sharp.
- **Families (eleven built in):** holo panels `81216C77F90DEDD6` (also the
  ship/shield hologram, ps `A2965EC2931A39C8`); the icon core
  `F8D8A92E96419901` (ps `16196F69ADE35E77`); the corona family
  `D1281DF454A153AD` (ps `97DBC87FCAA429C4`: the icon's glow AND the real
  sun's corona); the stalks `DF3503CD07F9B10C` (caught by the pixel probe
  at the radar, frame 8548) and `5453D19B6D362364` (drawn next to it in
  the ledger's radar section); the target hologram sphere `5559BD94B6852E83` (two premultiplied
  quads, ps `EA02FAC2BD6C643C`/`E95634B0F61D218F`, named by the pixel
  probe, flight 20260924_155636); the five radar-contact families
  `A2C2D5510BF1926D`, `9B34C331902DC1ED`, `9611A454527F7FEB`,
  `B932058F26B76691`, `94D5C556DFD6D705` (named by their position in the
  draw ledger, right after the two stalks in each eye's cockpit section --
  which one paints the visible bars is still open). The canopy
  `8C091FFD08644E02` is refused even if listed. A separate, second
  built-in list holds one WORLD MARKER, not radius-clipped: the target
  reticle's 3D triangles `71DD8B8B09060A81` (round 4). Extras from
  `advanced.temporal_aa_hologram_families` always join the cockpit list,
  never this one.
- **Mechanism:** each listed draw is issued twice more after the game's.
  The first pass adds the element's own blended light into a scratch
  target. It counts only fragments nearer than the cockpit radius
  (`advanced.temporal_aa_ship_metres`, 10 m), clipped by a depth target
  cleared at that radius. The second pass writes the element's nearest
  depth. Once per eye, before the temporal pass reads the private AA depth
  copy, a resolve stamps that depth wherever two things hold:
  - the pixel as displayed clears the floor on its brightest channel;
  - the element supplies at least `share` of that pixel's light in the
    game's own HDR target.
- **Open:** which of the five radar-contact families paints the bars
  (all five are listed, taken from the ledger's cockpit section, not
  individually confirmed by the pixel probe). The GPU cost of two extra
  passes per listed draw is unmeasured. Round 3 itself is unflown --
  20260924_155636 is evidence for the defect it fixes, not for the fix.
- **Risks the first flight must look at:**
  - target markers on a target inside 10 m (the holo material draws the
    markers at the target; beyond the radius they are clipped);
  - glow over a bright outside scene, which the share test is there for;
  - the extra GPU time.
- **Ruled out (do not re-propose):**
  - `advanced.ui_replay` (removed 48ad7689) would not have fixed it: it
    logged captured=0 on every flown rig and took only draws already marked UI.
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
- **Next flight:**
  1. Supercruise under FSR, rolling the ship, with the star icon above
     the radar disc, the contact bars, and the ship and target holograms
     over sky. Take an eye dump while rolling.
  2. Near a station with a target locked, with a hologram over the
     station. Take a second dump.
  3. Fly a key-off leg for the GPU time.

  Read these log lines:
  - the `hologram depth:` configure line, now "N cockpit families, M world
    markers" (round 4: M should read 1);
  - the first-listed-draw line (the blend space, and now target view
    yes/no -- no should not happen for the HDR scene target itself);
  - the 30 s census: listed draws per frame, stamped pixels p50, share
    test skipped (no target view) and floor on contribution (no display
    view) should both read at or near 0 -- either climbing back up means
    round 3's own fix is not reaching its inputs;
  - round 4: the target reticle's triangles should now show nonzero Z and
    HoloCoverage at any range in the eye dump; the `UI content census:`
    lines on the run's first frame, for the ghosting-digits question.

  In the dumps, read the new `HoloContribution` input against `C00` and
  `Z` at the icon, the holograms and the marker corners.

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
