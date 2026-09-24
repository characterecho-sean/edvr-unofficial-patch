# Cockpit holograms and icons over the sky: depth for temporal AA

## Status

- **State:** first build ON MAIN (986ebaad) FLOWN ONCE (20260924_155636,
  dump eye_155832, rolling): the inner quarters still blurred, two defects
  found (below). Round 3 BUILT on `claude/openxr-perf-gaps`, green
  (`hologram_depth_test`: 1270 checks), NOT FLOWN:
  - the share test now reads the game's own RT0 resource back (a per-eye
    cached SRV, in that RTV's own view format), never the tonemapped
    `inSrv` -- comparing the two was the defect 986ebaad flew with;
  - the floor now reads the displayed (tonemapped) pixel, falling back to
    the contribution's own space only without one;
  - the target hologram and the five radar-contact families are built in
    (eleven built-in families total; `holoBuildFamilyList` is the pure,
    rig-tested builder).
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
  `8C091FFD08644E02` is refused even if listed.
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
  - the `hologram depth:` configure line;
  - the first-listed-draw line (the blend space, and now target view
    yes/no -- no should not happen for the HDR scene target itself);
  - the 30 s census: listed draws per frame, stamped pixels p50, share
    test skipped (no target view) and floor on contribution (no display
    view) should both read at or near 0 -- either climbing back up means
    round 3's own fix is not reaching its inputs.

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
