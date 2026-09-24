# Cockpit holograms and icons over the sky: depth for temporal AA

## Status

- **State:** BUILT 2026-09-24 on `claude/openxr-perf-gaps`, NOT FLOWN.
  `advanced.temporal_aa_hologram_depth` (default on), with
  `advanced.temporal_aa_hologram_families`, `_floor` and `_share`. It runs
  inside `fix.temporal_aa`'s interface depth (`ui_depth.cpp`) and needs it on.
- **The defect:** cockpit holograms and the supercruise radar's star icon
  write no depth. Where the sky is behind them, their pixels take the
  sky's depth and motion while they move with the cockpit, and FSR's
  history lands about a pixel a frame off (worst under FSR, Sean
  2026-09-24). Over the cockpit they inherit cockpit motion and stay sharp.
- **Families (default list):** holo panels `81216C77F90DEDD6` (also the
  ship/shield hologram, ps `A2965EC2931A39C8`); the icon core
  `F8D8A92E96419901` (ps `16196F69ADE35E77`); the corona family
  `D1281DF454A153AD` (ps `97DBC87FCAA429C4`: the icon's glow AND the real
  sun's corona); the stalks `DF3503CD07F9B10C` and `5453D19B6D362364`
  (believed: named by the probe's neighbourhood, not caught painting the
  stalk). The canopy `8C091FFD08644E02` is refused even if listed.
- **Mechanism:** each listed draw is issued twice more after the game's.
  The first pass adds the element's own blended light into a scratch
  target. It counts only fragments nearer than the cockpit radius
  (`advanced.temporal_aa_ship_metres`, 10 m), clipped by a depth target
  cleared at that radius. The second pass writes the element's nearest
  depth. Once per eye, before the temporal pass reads the private AA depth
  copy, a resolve stamps that depth wherever two things hold: the
  element's light clears the floor (display brightness) and it supplies at
  least `share` of the finished pixel's light.
- **Open:** the target hologram's draw is not caught (the probe point
  missed it). The stalk hashes are believed, not measured. The GPU cost of
  two extra passes per listed draw is unmeasured.
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
- **Next flight:**
  1. Supercruise under FSR, with the star icon above the radar disc and
     the ship and target holograms over sky. Take an eye dump.
  2. Near a station with a target locked, with a hologram over the
     station. Take a second dump.
  3. Fly a key-off leg for the GPU time.

  Read these log lines:
  - the `hologram depth:` configure line;
  - the first-listed-draw line (the blend space);
  - the 30 s census: listed draws per frame, stamped pixels p50, share
    skipped, declines.

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
