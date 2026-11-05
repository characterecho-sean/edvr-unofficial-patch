# Legacy cockpit glass as a Visual option

## Status

State 2026-11-04: **FLOWN ON STEAM FLAT, NOT FLOWN IN VR, MERGING TO MAIN FOR USER FEEDBACK.** `visual.cockpit_glass = odyssey | legacy` (default `odyssey`, the game
unchanged), flat and VR, Visual tab. Branch `claude/cockpit-glass-shader-port-1852e3`. With `legacy`, EDVR's
own pixel shader (`cockpit_glass_shader.h`) is handed to the real `PSSetShader` in place of the game's intact
quality-3 canopy shader `4E4FF61E8A08FC7E`, at bind time. What was built, the constants chosen and what the
flight must check: section 8.

Settled offline (sections 1 to 3):

- Both builds' glass shaders are in `Win64\EffectsBinary\*.arc`, which `tools\effects_arc.py` reads. Legacy
  (Horizons 3.8, build 312425): effect `CockpitGlassEffects3`. Live: effect `CockpitGlass_Forward`.
  CONFIRMED that it is the canopy: its VS is byte-identical to `8C091FFD08644E02`, the canopy VS EDVR logs.
- **The legacy shader cannot be dropped in** (cb0 against CB1/CB2, other slots, other VS outputs), so the port
  is a new EDVR pixel shader that works the legacy lighting out from live's own bindings (section 4).
- EDVR's shader hash (`src\common\log.cpp`) uses offset basis 1469598103934665603, not the standard FNV basis.
  That is why `fss-scanner.md` and `bloom-artifact-2026-10-20.md` found nothing in these archives.

Probe FLOWN 2026-11-03 (Steam, flat; section 7): only the intact q3 canopy draws; blend is straight alpha;
t1 holds one light, direction in view space; docked, the sun is off and CB1[257] ambient is 0 everywhere.

Decisions (Sean, 2026-11-03), all applied in section 8: an approximation of the legacy look is accepted; key
`visual.cockpit_glass = odyssey | legacy`; the intact canopy only, ice and condensation stay stock; live's
constant dirt colour stands in for the legacy RGB dirt texture (reopened by the third flight, section 9); the canopy
keeps live's reflection-probe lighting as its ambient, so it stays lit in a hangar.

Flown once (3ae212ae, Steam, flat): the swap ran (binds = canopy-seen, about 3,050 per 30 s) and little
differed, because the shader scaled dirt colour and glare by live's alpha-only factor k (0.075) and lit by
live's tilted normal. Fixed in 5b9c36f9 (section 8, "After the first flight"). Flown again: the canopy was covered in bright golden
dirt speckle in sunlight, because the direct light lacked live's 1/pi (section 8, "After the second flight").

Third flight (0756063a, Sean): the dirt sits where the ice forms, which is not realistic. Live's masks put
dirt and smudges on the canopy mesh's blue vertex channel and ice on its green one; on this canopy the two
cover the same areas by eye (not measured). Where legacy put its dirt cannot be read offline (section 9).

Legacy MEASURED by RenderDoc (section 10): premultiplied blend, alpha a 0.02 base, dirt a faint swirled
streak texture over the whole pane (mask 0.025 x vertex B), smudge and crack glare 1,000x and 50x weaker
than the port's guesses. BUILT (section 11, not flown): the shader takes those values; EDVR's own generated
streak texture (256 x 256, worker thread, at t12) stands in for legacy's; the quality-3 ice and condensation draws
get the legacy dirt drawn over them (an overlay, ONE + ONE), the game's ice and condensation untouched. Open: the
next flight; the quality 0-2 ice and condensation shaders (other slot layout, not overlaid); cost.
Flown with 5 x (probe(N) + probe(-N)) ambient (268b292c): still almost no dirt. That x5 was WRONG (legacy's cube was black, section 12) and is withdrawn: the build is back to one probe sample at scale 1.0 and multiplies the dirt albedo by kDirtStrength = 16 (Sean's choice of visible dirt, section 11). The streak texture is reworked to legacy's character (512 x 512, section 13), and again as diagonal strokes in every band (dbc5ee33), then PAINTED as overlapping brush strokes (section 13), first thin and hairy, now 210 wide soft smears with a faint grain. The dirt mask is continuous (no polygon edges), kDirtStrength is 4 and the streak texture's brightness distribution is legacy's by quantile mapping (section 13); the dirt is placed as a film plus a soft edge gather plus patches, not by the vertex mask alone (section 13). Replay and display measurements: section 12.

15619672 flown: too faint, no visible difference from `odyssey`; film 0.4 and edge 0.3 flown 25cb4723, "looks better" (end of section 13). CB2[17].w behaves like per-ship wear driving Odyssey's dirt; Horizons' glass shader has no such term, and Sean decided on 2026-11-04 that EDVR's grime stays fixed (section 16). A dashed bright line under the top strut turned out not to be the glass, it shows with `odyssey` too (section 15). Whether the film and patches read naturally is still Sean's call.

BUILT 2026-11-05, NOT FLOWN (section 17): `visual.cockpit_glass_dirt`, 0 to 300 percent, shipped 100, live, flat and VR, under Cockpit glass.
A one-register block at b13 scales the dirt only; the row is dimmed unless the look is `legacy`. It makes Sean's strength call a slider.

Temporary: `experimental.cockpit_glass_probe` (`cockpit_glass_probe.*`, `tools\cockpit_glass_probe_test`, the
`:rig_cockpit_glass_probe_test` rig, its flat allow-list line). Default off, kept on main while users try `legacy` (Sean, 2026-11-04), so a report can turn it on. Remove all of it once that feedback is in. Pre-merge review follow-ups, none seen in a flight: issue #188.

Analysis: `analysis\decomp\verify_20261103_cockpit_glass_legacy.txt` (312425) and `..._live.txt` (334925),
filed 2026-11-03: permutation tables, the cb0 layout, the legacy PS written out. `..._live_mapping.txt` holds
the legacy-to-live input table. `analysis\decomp\verify_20261103_glass_wear_312425.txt` and `..._334925.txt`, filed
2026-11-05: the wear findings of section 16.

Ruled out: see the end of this doc.

## 1. The archives and the hash

`Win64\EffectsBinary\` holds `Effects.arc` in the legacy client and
`Effects_Win64_SM50.arc` plus `Effects2_Win64_SM50.arc` in Odyssey. Each
is a 16-byte header and then a raw-deflate stream. Inflated, it is a run
of records, each a u16 version, a two-character tag, a 4cc (`sd3f` shader,
`xd3f` effect) and a u32 size. A shader record carries one DXBC blob and a
16-byte id. An effect record carries its parameter names, its own name,
and pass tables that name shaders by that id. All the format notes are in
`tools\effects_arc.py`.

Counts: legacy 884 effects and 3,841 shaders; live 976 effects and 2,728
shaders.

The flight dump (`edvr_logs\shaders`, 1,187 blobs on Frontier, newest
2026-11-03) shares no hash with the archives under the standard FNV
basis. It shares 634 under EDVR's. An archive blob is byte for byte what
the game passes to `CreatePixelShader`, so

    python tools\effects_arc.py find --arc <Effects_Win64_SM50.arc> --arc <Effects2_Win64_SM50.arc> --hash <EDVR hash>

names the effect behind any hash in an EDVR log. The basis itself is not
a bug to fix: every shader constant in `src\` is keyed on it.

## 2. The two effects

**Legacy `CockpitGlassEffects3`: 144 passes.** The selectors are the
directional-light count (0 to 2), the directional-shadow count, a
high-resolution screen-capture flag, `gCockpitGlassState` (0 intact, 1
ice, 2 condensation) and `gMaterialQualityLevel` (0 to 3). Shadows apply
only at quality 2 or above. For the intact canopy, the VS is
`BD3029C5D0C713EF` with lights, or `682879948FBB8604` with none. The PS
depends on the light and shadow counts:

| Lights, shadows | PS (EDVR hash) |
|---|---|
| 1, 1 (likely in a lit cockpit) | `A816DE90739BF4C0` |
| 2, 1 | `B66C914B1CF50AC0` |
| 1, 0 | `AD41A6B822371609` |
| 2, 0 | `88CCD990D3DA2280` |
| 0 | `E8E2A17AD8C64F5E` |

**Live `CockpitGlass_Forward`: 24 passes**, 12 distinct ones: state 0 to 2
by quality. The VS depends on state only. Quality 0 to 2 read the
directional-light buffer alone. Quality 3 adds the clustered light lists,
the reflection-probe cube array and the specular LUT. The flight dump
holds only state 0 at quality 3 (VS `8C091FFD08644E02`, PS
`4E4FF61E8A08FC7E`). Its slots: t0 light lists, t1 directional lights, t2
lights, t3 reflection cube array, t4 specular LUT, t5 shadow map, t6
crack normal and alpha, t7 crack interior normal, t8 crack id, t9 packed
blemish maps.

`Metalness_Roughness_CockpitSimpleGlass_Forward` and the `GlassAdvanced_*`
and `GlassSimple_*` effects are other glass, not the canopy.

## 3. What the legacy look is

All of this was read from the legacy PS for the intact canopy. The
decompiled form goes to `analysis\` (Status), not into this repository.

- **Coverage.** A dirt layer (an RGB dirt texture times a vertex dirt
  mask), an imperfection mask, and cracks gated by a vertex crack mask
  against a threshold. The crack normal map tilts the normal.
- **Lighting.** Up to two directional lights, as Lambert times shadow,
  plus an ambient term the VS samples from a cube map. A
  normal-independent glare lobe toward each light is added on top. The
  shadow uses up to 8 cascades with a 4x4 PCF of a depth map.
- **No** reflection probes, GGX or Fresnel, local lights, fog, exposure
  or tone-map in the PS. Exposure enters only through the VS ambient
  scale.
- **Output.** Colour already weighted by coverage. Alpha is the crack
  term plus the tint alpha. That reads as premultiplied, but the blend
  state is not in the archive.

Live's canopy instead lights the glass with GGX and Fresnel, adds the
reflection probes at quality 3, scales albedo and roughness by one minus
the tint alpha, and writes in HDR units (`gHDRScalingWrite`, CB1[90].y).
The probe reflections and the specular model are where the two looks
part.

## 4. Design of the port

- **Key.** `visual.cockpit_glass`, `odyssey` (default) or `legacy`, in the
  Visual tab, both profiles: add it to the flat allow-list
  (`runtime_profile.h`) and to `edvr.ini` (`check_config_contract`). The
  key can change live: the swap is made at bind time.
- **Where.** `hookedCreatePS` notes the live canopy permutations by hash.
  The `hookedPSSetShader` path binds EDVR's replacement in their place
  while the key is `legacy`. That is a pointer compare per bind and no
  new D3D call per draw, as the plugin design requires. It belongs in the
  cockpit-visuals plugin.
- **The replacement.** An EDVR HLSL pixel shader with the live VS's
  output signature, so it links with `8C091FFD08644E02`. It reads only
  what the game has already bound: CB1, CB2, t1, t5, t6, t9, s0 and s1. It
  computes the section 3 model: the live crack and blemish masks, two
  directional lights from t1 times live's shadow resolve, the legacy
  glare lobe, ambient from CB1[257], legacy alpha, and output scaled by
  CB1[90].y. Quality 0 to 2 bind different slot layouts, so each layout
  needs its own variant. The first version covers state 0 only, and ice
  and condensation stay stock.
- **Gaps** (section 6): live has no RGB dirt texture, only a constant
  dirt colour `pDirtDiffuse` (CB2[11]) times a blemish channel. The
  legacy depth PCF becomes live's moment-shadow resolve.
- **Acceptance.** Screenshots from the same ship and station: the legacy
  client, EDVR `legacy`, and EDVR `odyssey`, flat first, then VR. Plus
  frame time at the same spot: with the probes gone the replacement
  should cost less than stock, never more.

Environment: game build 334925 with `Effects2_Win64_SM50.arc` as
rewritten 2026-10-20. A game update that rebuilds the archive changes the
hashes. `effects_arc.py find` re-identifies them offline. Runtime and
headset independent: one draw, both eyes, flat or VR.

## 5. The capture flight

One flight answers all of these together.

Every line starts `glass probe:`. A sample's lines all carry
`sample N`.

| # | Question | Log signature that settles it |
|---|---|---|
| C1 | Blend, depth and write mask of the canopy draw, and its target format | `sample N om blend rt0` / `rt1`, `om depth`, `om raster`, `om rtvK`, `om dsv` |
| C2 | Permutations drawn in flight | `window 30 s draws=… ps: <hash>(intact q3)=… vs: … other-ps=…` |
| C3 | Frame constants the port reads | `sample N cb1 [r] x y z w …` for registers 55-56, 61, 90, 245, 248, 257, 261, 275-279, 290-326 |
| C4 | Material constants | `sample N cb2 [0]…[17]` |
| C5 | Directional-light buffer layout (colour at +0, direction at +16, stride 48, as read from the PS) | `sample N t1 [0]…[11]`: the first 192 bytes from the view's first element |
| C6 | Slot formats | `sample N srv tK …` (t0 to t11) and `sample N smp sK …` (s0 to s2); `cb bK bytes= range=` says whether register 0 is byte 0 |

A component that is an integer in the shader's eyes (its bits are a
denormal) prints `#n`, and a register past the copied range prints `-`.

When it logs: the first canopy draw (`sample 1 draw`), then at most once
every 30 s, 12 at most. The constants and t1 are GPU copies made at the
draw and read at a frame boundary two or more frames later
(`sample N read back`). What a failure looks like:

- the key never read: no `armed` line;
- no canopy draw reached the hooks: `window` lines with `draws=0`;
- a readback that failed: `readback map failed` or `dropped`.

Flat is the cheaper flight; a lit station hangar and open space in
sunlight give two light setups.

Not answerable from the CPU: the sign of the live `faceinvariant` input
and whether the lighting normal faces in or out. The replacement's first
build carries a debug view for those.

## 6. Decisions for Sean

1. **Approximate look.** The option gives legacy lighting on live's glass
   material, not legacy pixels. The dirt texture and the shadow filter
   differ (section 4). Proceed on that basis?
2. **Key.** `visual.cockpit_glass = odyssey | legacy`?
3. **Scope.** Is the intact canopy only enough for the first version,
   with ice and condensation left stock?
4. **Legacy RGB dirt.** The shader side ports. The blocker is the
   texture. The legacy client's Win64 asset files (a 400-file sample:
   70% `FREA`, 30% zlib with an unknown inner format, no texture names)
   are not decoded, and the texture is Frontier's artwork, so EDVR does
   not ship it. Shippable routes: read it at run time from the player's
   own legacy install, or an EDVR-made look-alike. A legacy-client frame
   capture in flat would show it. Decided 2026-11-03: wait for the probe
   flight's `pDirtDiffuse` (CB2[11]) and t9 values, which say how far
   live's flat-colour dirt already is from legacy, then choose. The
   flight (section 7) gives the live side only.

## 7. Probe flight 2026-11-03 (Steam, flat)

Log `edvr_gfx_20261103_090854.log`, build `v0.22.0-rc.5-50-g625de17e`
(matches). Flat 3840x2160. Eight samples: 1 to 4 docked in a station
hangar, 5 leaving, 6 to 8 in open space in sunlight. No readback was
dropped. One ship throughout.

- **C2, permutations.** Only PS `4E4FF61E8A08FC7E` (intact, q3) with VS
  `8C091FFD08644E02`, one draw a frame (about 3,050 per 30 s), docked
  and in space. `other-ps=0`. Ice, condensation and q0 to q2 never drew
  on this rig.
- **C1, output state.** Blend is straight alpha on colour and alpha
  (`SRC_ALPHA`, `INV_SRC_ALPHA`), write mask 0xF, one RTV,
  R11G11B10_FLOAT. Depth test `GREATER_EQUAL` (reverse Z), no depth
  write. Stencil writes 5 under mask 0x5. Back faces culled. The blend
  multiplies colour by alpha again, though live's albedo already carries
  coverage, so live's dirt colour is scaled by alpha twice.
- **C5, the light buffer.** t1 is 48 bytes: one light, and
  `g_numDirectionalLights` CB1[245].y = 1. Offset +0 is the colour in HDR
  units, the same as CB1[293] (about 800 to 2,360 in sunlight). Offset
  +16 is the direction in view space: CB1[277] and [278] dotted with the
  world sun direction CB1[291] give t1's x and y (checked on sample 6).
  Offset +32 reads (1, 0, 0, int 1). Legacy's second light has no source.
- **Docked, the sun is off.** Its colour is 1e-13 in every docked
  sample. A hangar is lit only by local lights and the reflection probes
  (t0, t2, t3), none of which the legacy model reads.
- **No ambient constant.** `gAmbientColour` CB1[257] is 0 in all eight
  samples. The legacy VS ambient has to come from the probes (t3) or not
  at all.
- **C3, other frame constants.** `gHDRScalingWrite` CB1[90].y = 1/256
  (read scale .z = 256). Shadow: t5 3072x2048 R16G16B16A16, six cascades
  (CB1[326].x) in a 3x2 atlas (CB1[318..323]). CB1[55] = (1, 0, 0, 0):
  no cracks, ice or condensation. CB1[56] = (0, 1): tint alpha 0, alpha
  multiplier 1.
- **C4, material** (CB2, unchanged across samples): `pDirtDiffuse` (0.274,
  0.246, 0.208), a warm grey; dirt alpha multiplier CB2[0].x 0.5; mask
  scale and offset CB2[6].zw 0.5 and -0.2; dirt roughness CB2[13].y 0.8.
  CB2[17].w = 0.8787, so dirt and imperfection coverage is scaled by
  1 - saturate(0.8787 x 1.0526) = 0.075. On this ship live's dirt is a
  constant warm grey at 7.5% of its mask. What CB2[17].w is (a per-ship
  wear value, or fixed per material) is unknown from one ship.
- **C6, slots.** t3 is a BC6H 256x256 cube array of 64 cubes, t4 a
  512x512 R16G16_FLOAT LUT, t6 and t7 BC7, t8 BC4, t9 BC7 1024x1024. s0 anisotropic
  x16 wrap; s1 and s2 trilinear clamp. b1 is 5,376 bytes, b2 288, both
  bound from byte 0.

What this changes in section 4: the replacement needs the probes (t0,
t2, t3) for ambient and for any light in a hangar, one directional
light, and output written for a straight-alpha blend. Only the q3
intact variant is needed on this rig.

## 8. The replacement shader (built 2026-11-03, not flown)

**Built.** `src\d3d11\cockpit_glass_shader.h` (the HLSL), `cockpit_glass_core.h` (the key, the swap rule, the
live signature table, the log wording), `cockpit_glass.{h,cpp}` (the swap and the 30 s window), a hook in
`hookedPSSetShader` (`vscreen.cpp`) and a tick at the frame boundary. The HLSL is compiled by
`tools\temporal_shader_build` into `kCockpitGlassLegacyPsBytecode` (optimization level 3; the build's self-test
holds its input signature to the game's, element for element, and its bindings to b1, b2, t0 to t3, t5, t6, t9,
s0 to s2) and created with `shaderSwapCreatePs`; null means draw stock. `:rig_cockpit_glass_test` runs the
bytecode on WARP with the debug layer on (it draws with a vertex shader carrying the game's canopy outputs, so the
link is the game's; no layer message): no coverage is transparent, and dirt under the sun, a cascade's shadow, a
hangar lit by one probe, a lit crack, the glare lobe and the tint alpha each match the legacy model worked out in
C++ to half-float precision (the dirt case 1.594 against 1.595). The key is in `edvr.ini` `[visual]`, the flat allow-list, the flat
Visual tab and the headset menu's Visual page.

**The swap.** With the key `legacy`, a PS bind whose hash is `4E4FF61E8A08FC7E` is handed the replacement; the
binding shadow still records the game's shader and hash, so the census and the probe see the game's. Any other
bind, and every bind under `odyssey`, goes through unchanged (one load of a flag, then one hash compare).
The key is read on each config generation, so it switches live; the next canopy bind takes effect.

**What the shader takes from live, as it is** (from the disassembly): the tangent frame and the face flag, the
crack and blemish masks and their scales, the crack normal and the lit normal N_in, the moment shadow resolve
(cascade selection, the two-slot blend, the B-spline filter from four bilinear taps, the Hamburger solve and the
light-bleed cutoff), the probe lookup (cluster from tile and depth, the probe weights by volume type, the sum to
weight 1), the HDR write scale. **What it takes from legacy:** Lambert times shadow on the one directional
light, the normal-independent glare lobe `pow(saturate(L . V), gloss) * (gloss + 6) / 32`, an ambient term in
the albedo's lighting, and the colour arithmetic (the dirt colour weighted by the dirt coverage, crack colour in
a crack). Ambient comes from the probes, sampled once at mip 7 along N_in, not from legacy's VS cube.

**The blend.** Straight alpha, so the colour written is the legacy radiance divided by the alpha written; the
blend then adds that radiance once (see "After the first flight" for the masks that drive it). Alpha is the legacy formula (crack term, scaled, plus the tint
alpha) plus the dirt and imperfection coverage. Legacy alpha ignored those two (its blend took the colour as
it was), and under straight alpha that would hide the dirt entirely.

**Constants chosen, not measured** (the legacy client's values were not recoverable from its archive, which
holds the shaders and not the materials' cb0 values):

| Constant | Value | Why |
|---|---|---|
| imperfection glare strength | 1.0 | legacy's `cb0[39].x`; unity as the neutral start |
| imperfection gloss exponent | 24 | a broad smudge, about 12 degrees wide |
| crack glare strength | 1.0, times the frame's alpha multiplier | legacy's `cb0[38].y * AlphaMultiplier` |
| crack gloss exponent | 64 | a tighter glint than a smudge; legacy has `cb0[38].z` |
| ambient scale | 1.0 | the probe cube in the light's own HDR units (times CB1[90].z) |
| ambient mip | 7 | the roughest mip live samples (`sqrt(roughness) * 8`, capped at 7) |
| probe cap | 16 | live stops when the weights reach 1; this bounds a cluster that never does |

**Findings while porting** (all from the live disassembly, none flown):

1. **Live runs the probe loop only in cracks.** The whole probe block sits under a flag that is the crack mask
   times a crack-id difference, so on an intact canopy live lights the glass by the sun's GGX alone and the
   probes are not read. The legacy ambient from the probes is therefore new work on the intact glass, not
   work live already does there; the cost is one cluster lookup and one probe sample (a few probes in a
   cluster at most) per pixel that has any mask (after the first flight the early out is on the unscaled masks).
2. **Live's no-crack normal weights are (0.5, 0.5, 1.0)** (not ported after the first flight) on (tangent, normal, bitangent), not (0, 1, 0): the
   lerp between "no crack" and the crack normal starts from the flat normal map's encoded value. So the intact
   glass is lit by a normal tilted about 66 degrees off the surface normal. Ported as it is, as asked. If a
   flight shows the lighting depending oddly on which way the tangent runs, this is the first place to look:
   the constant is one vector in `main` (`float3(0.5, 0.5, 1.0)`).
3. **The blemish map's channels:** x is the dirt (times the dirt vertex mask, `masks.y`), y the imperfection
   (times `masks.z`). The decode note's "x with `v0.z`" has them the other way round; the instruction order in
   the PS (the `mul_sat` swizzle `v0.zzzy`, then the dirt roughness on the x-derived term) gives this.
4. Legacy's second directional light has no source in live (t1 holds one light), so the port has one.

**After the first flight (2026-11-03, 3ae212ae).** The swap ran and the picture hardly changed. Two causes, both
in the shader:

- **k scaled the colour.** `k = 1 - saturate(CB2[17].w * 1.0526)` (0.075 on the flown ship) is in live's coverage
  and nowhere else; the legacy client had no such factor. The port had put it into the dirt colour and the
  smudge glare, 13 times too weak. Now the unscaled masks (`dirt = saturate(blemish.x * masks.y)`,
  `imp = saturate(blemish.y * masks.z)`) drive the albedo and the glare, and k scales alpha only
  (`crackA + k * (dirt + imp)`, as live has it). The colour written is still the radiance divided by alpha, so
  the blend adds the legacy radiance once. A pixel is returned transparent only when its alpha and its unscaled
  `crackA + dirt + imp` are both under 1/1024; a pixel with radiance and a smaller alpha is written at alpha 1/1024.
  More pixels are shaded than before (the masks are no longer cut by k), so the cost is now to be measured.
- **The lit normal is legacy's.** `w = lerp((0, 1, 0), (nx, nz, ny), crackMix)`, `nIn = w.x T - w.y N + w.z B`: -N
  on flat glass, bent by a crack. Live's start value (0.5, 0.5, 1.0) tilted the intact glass about 66 degrees and is
  no longer used (finding 2).

The rig's legacy model was changed to the same spec. New numbers (WARP, debug layer on): dirt under the sun 1.9531
(wanted 1.9531), alpha 0.2; the same dirt with k = 0.075 gives alpha 0.015 and colour 26.02 (wanted 26.04), so
alpha times colour is unchanged; a smudge (imperfection mask 0.5) under the light straight ahead gives radiance
468.75 at k = 1 (alpha 0.5, colour 3.662) and at k = 0.075 (alpha 0.0375, colour 48.78); a smudge of 0.01 at
k = 0.075 has alpha 0.00075, below the floor, and is written at alpha 1/1024 with its radiance (colour 37.47,
wanted 9.375 / 256 / (1/1024)).

**After the second flight (2026-11-03, 5b9c36f9).** With the masks unscaled the dirt was visible, and far too bright:
golden speckle over the canopy in sunlight. Live multiplies its Lambert term by 0.318310 before the light colour
(and again in its lights loop): its t1 colour is in units where diffuse = albedo * ndl / pi * colour. The legacy
formula has no such factor, because the legacy light colour was in its own units, so on live's colour the direct
light was pi times too bright. Both direct terms (Lambert times shadow, and the glare) are now scaled by
`kInvPi = 0.318310`; the probe ambient is not (it is radiance, and albedo times radiance is already the diffuse
response). Nothing else changed. Rig numbers (WARP, debug layer on): dirt under the sun 0.6216 (wanted 0.6217),
alpha 0.2; at k = 0.075 colour 8.281 (wanted 8.289), alpha 0.015; smudge glare radiance 149.2 wanted, got colour
1.165 at alpha 0.5 and 15.53 at alpha 0.0375 (both radiance 149.2); the hangar case is unchanged at 0.125.

**What the flight must check** (flat first, Steam; then the headset):

- `python tools\edvr_log.py --target steam --expect-build HEAD --version`: the right build.
- With the key `legacy`: `cockpit glass: legacy on`, then once `cockpit glass: replacement pixel shader ready`
  (and the shader-swap line `cockpit glass: precompiled pixel shader cockpit_glass_legacy_ps created`), then every
  30 s `cockpit glass: window 30 s legacy binds=N canopy-seen=M replacement=ready` with N equal to M and both
  about 3,000 on this rig. `binds=0 canopy-seen>0 replacement=refused` is a replacement that did not create;
  `binds=0 canopy-seen=0` is a canopy the game never drew with that shader; no window line at all means the key
  was never read.
- The picture, docked in a hangar, then in space in sunlight, then the headset: the canopy is lit in the hangar
  (the probe ambient), glares toward the sun, shows the dirt at the amounts live gives it, and nothing flickers
  between the eyes. Compare with `odyssey` on the same ship. Then the draw cost at the same spot.
- Switching the key in the F8 menu while in the cockpit takes effect within a few frames, both ways.

## 9. Where the dirt goes (third flight, 2026-11-03)

Flight 0756063a (Steam, flat; log `edvr_gfx_20261103_104211.log`). Sean: as ice forms it takes the place of
the dirt; the dirt sits just where the other effects would be, which is not realistic.

What the live shaders say (read from the disassembly):

- The port's dirt is placed by live's arithmetic: blemish map t9 channel x (checked against the swizzles of
  the three hex-tiled samples, instructions 112-119 of the intact PS) times the dirt vertex mask. The VS
  takes the dirt mask from the vertex colour dotted with CB2[3] and the smudge mask with CB2[4]; the probe
  read both as (0, 0, 1, 0), the blue channel.
- The ice VS (`63C6E375A08C78AA`) takes the ice mask from CB2[2], (0, 1, 0, 0), the green channel, and the
  ice PS (`7F770D61DEA7CD30`) thresholds its own texture (t10.z) against it. Dirt and ice read different
  channels, so what Sean saw means the artwork's blue and green channels cover the same areas (by eye, not
  measured). Live hides that by
  drawing its dirt at 7.5% opacity; at legacy strength it shows.

What legacy did: its dirt is the RGB dirt texture (t2) times a vertex mask, `dot(colour, cb0[4])` times
cb0[3].x. Neither can be read offline. cb0[4], the channel the legacy dirt mask used, is a material value,
and the legacy archive holds only shaders. The texture is in the undecoded legacy assets. Whether the legacy
canopy mesh carries the same vertex colours as live's is also unknown.

To see it: a frame capture of the legacy client (`elite-dangerous-64`, build 312425) in the cockpit, read at
the canopy draw (PS A816DE90739BF4C0): the texture bound at t2, the vertex colour stream and cb0[3..4].
RenderDoc is not installed on this machine. Until then, any change to where the dirt goes is a guess.

## 10. The legacy canopy, measured (RenderDoc, 2026-11-03)

Sean took three RenderDoc captures of the legacy client (build 312425, flat, 4800x2700, in space; not the
Odyssey test ship's cockpit). They were read with RenderDoc 1.47's Python. The captures and everything taken
from them (Frontier's textures among it) stay on this machine, in `%LOCALAPPDATA%\EDVR\captures\`; nothing
from them is in this repository.

- **One canopy draw per frame, always PS `A816DE90739BF4C0`** (the intact lit permutation, matched by
  bytecode), VS `BD3029C5D0C713EF` (matched by its outputs, not its bytes). Target R11G11B10_FLOAT HDR, depth `GREATER_EQUAL` without write.
- **The legacy blend is premultiplied: colour `ONE`, `INV_SRC_ALPHA`; alpha the same.** That settles
  section 3's open question. The port's "divide by alpha under live's straight blend" emulates it.
- **Alpha is a constant base, about 0.02** (PS cb0[35].y: 0.0212, 0.0211, 0.0185); the crack term adds
  nothing because the VS crack amount cb0[0].x is 0. Dirt and smudges add light and never occlude.
- **Placement.** Dirt mask `0.025 x vertex B` (VS cb0[3].x 0.025, cb0[4] = (0, 0, 1, 0)); the canopy's
  vertex colour is grey (R = G = B), mean 0.72, so the mask covers the whole pane (78% of vertices above
  0.01), strongest at the top, along the struts and the lower edge, weakest in a band down the middle.
  Smudge mask `1.2 x vertex A - 0.2` (cb0[6] = (0, 0, 0, 1)): a different channel from the dirt, three soft
  patches in the upper and outer canopy, zero low in the middle.
- **The dirt texture** (t2, BC1 sRGB 512x512) is a dark grey swirled wipe-streak pattern: linear mean
  0.024, max 0.09, repeated about 25 x 22 times over the canopy (UV x 16 x 1.5). The visible legacy dirt is
  that streak pattern over the whole pane, faint: on a dark sky the canopy adds about 30% to the background's
  luminance, nothing visible over bright objects.
- **Measured constants that replace section 8's guesses:** smudge glare strength cb0[39].x 0.001, gloss
  cb0[39].y 120; crack glare strength cb0[38].y 0.02, gloss cb0[38].z 450; dirt colour scale cb0[35].z 0.97;
  VS ambient scale cb0[0].y 5; sun colour cb0[3] about 5.1 (legacy units).
- The 0.025 dirt multiplier and the 0 crack amount were the same in all three captures. Whether either
  follows game state (ship wear, damage) is not known.

What differs from the port as flown (0756063a): its dirt mask is live's `saturate((B - 0.2) x 0.5) x 0.5`
times live's blemish channel, which sits at the edges, with live's dirt colour 0.27 against legacy's 0.024
times a mask 8x smaller; its smudge glare is 1,000 times legacy's (1.0 against 0.001) and its crack glare
50 times; its alpha has no 0.02 base. Legacy's own streak texture is Frontier's and is not shipped.

## 11. Measured values, EDVR's streaks and the ice overlay (built 2026-11-03, not flown)

Sean's choices (2026-11-03): the shader takes the values section 10 measured; the dirt pattern is EDVR's own
streak texture (legacy's is Frontier's); ice and condensation stay as the game draws them, with the legacy dirt
still on top.

**Measured values now in the shader.** Alpha is `saturate(crackA + max(CB1[56].x, 0.02))` (the 0.02 base, PS
cb0[35].y): dirt and smudges no longer occlude, and k and the alpha floor are gone. The colour written is still
the radiance divided by that alpha, so live's straight blend gives the legacy premultiplied result. The dirt mask
is legacy's, `0.025 x vertex B` (VS cb0[3].x), with B recovered from live's `masks.y =
saturate((B + CB2[6].w) x CB2[6].z) x CB2[0].x` (B = masks.y / (CB2[6].z x CB2[0].x) - CB2[6].w; 0 where live's
clamp took it to 0, where legacy's mask would be under 0.005). Dirt albedo is grey: `streak x mask x 0.97` (cb0[35].z).
Smudge glare is 0.001 at gloss 120, crack glare 0.02 (times CB1[56].y) at gloss 450. Smudges keep live's
imperfection mask (`masks.z`) times the blemish map's y channel: the legacy smudge mask was a vertex alpha
channel (`1.2 A - 0.2`), which live's vertex shader does not pass. The 1/pi on the direct light, the probe
ambient and the legacy normal are unchanged (the ambient scale changed after the next flight, below).

**The streak texture** (`src\d3d11\cockpit_glass_streaks.h`; reworked in section 13, 512 x 512 and soft: this is the first version). An original pattern, not traced from the legacy
texture (only its two statistics were used, mean 0.024 and max 0.09): white noise smeared along a slowly turning
flow field (line integral convolution, a triangular kernel) into long thin curved fibres, contrast
stretched, broad patches of low-frequency noise deciding where the wipe went, and a power curve found by
bisection so the stored mean is 0.2667 of the max. Every field is periodic in the size, so it wraps with no seam.
256 x 256, R8_UNORM, stored normalised (brightest texel 255), the mip chain built on the CPU; the shader scales
it by 0.09, which gives the legacy max, and the mean comes out at the legacy 0.024. Sampled at `texcoord x
CB2[15].xy` with s0. Generation takes about 40 ms (build machine, release), so it runs on a worker thread
started when the key turns on; until it is done the replacement draws without dirt, never stock. The log says
`dirt streak texture 256x256 generated in N ms on a worker thread` and `... created (R8_UNORM 256x256, 9 mips)
and bound at t12`. A picture of it: `%LOCALAPPDATA%\EDVR\captures\edvr-streaks.png` (and a 2 x 2 tiling).
`:rig_cockpit_glass_test` holds it: deterministic, brightest texel 255, mean within 0.005 of the target, contrast,
neighbouring texels correlated (0.59 of the standard deviation against about 1.3 for white noise), the wrap seam
no larger than the neighbour difference (0.109 against 0.131), the mip chain and its means.

**Slot t12.** The streak texture is at t12 for every variant. The probe showed t10 and t11 unbound at the
intact draw, but the ice and condensation pixel shaders use them (ice normal, blemish map). Each time the
intact replacement is bound the swap saves what is at t12, binds the streaks through the raw call (the
binding shadow records only the game's slots 0 to 3 and is not told), and gives t12 back at the next pixel
shader bind of another shader or at the frame boundary.

**Ice and condensation.** While `legacy`, after the game's own draw of the canopy with the quality-3 ice
(`7F770D61DEA7CD30`) or condensation (`42747CC92CF844C4`) pixel shader, the draw thunks (owner context, flat
and VR, all four draw kinds) issue the same draw again with EDVR's overlay shader and a blend of colour
ONE + ONE, write mask RGB (alpha untouched), then put back the game's pixel shader, blend state, blend factor,
sample mask and t12. The overlay writes the legacy dirt and glare light only: `(ndl x direct + ambient) x
dirtAlbedo + glare`, premultiplied, no alpha, no crack occlusion (the game's draw did its own coverage). The
gate is one relaxed load of the key's flag. The window line gains `ice-overlays=N`.

What was read from the disassemblies (`ps_4F30562223470E06` ice q3, `ps_C29EC19DFD3363D2` condensation q3, the ice
VS `F56EB2F214D17E34` and condensation VS `70673CD0AF4C30D1`):

- Same as the intact PS: CB1 (declared 333 registers against 327; the bound buffer is 336, so the overlay's
  327 is read in range) and CB2 at b1 and b2, the layouts of t0 (cluster stride 2372, probes at +2212), t1, t2
  (160 and 48 byte records), t3 (cube array) and t5 (moment map, 16 gathers), and s0 to s2.
- **Different:** the crack normal and alpha is at t7, not t6, and the blemish map at t11, not t9 (both also use t10 as the ice or condensation normal, and t6 for a map the overlay does not read). The overlay is compiled with those slots.
- **Different:** the vertex shaders pass four mask components, in other orders. Ice: crack, ice, dirt,
  imperfection (dirt `.z`, imperfection `.w`). Condensation: crack, dirt, imperfection, condensation (dirt `.y`,
  imperfection `.z`: the same slots as the intact variant, so two overlay variants, not three). The dirt mask's
  formula (CB2[6].zw, CB2[0].x) and the crack mask's are the same in all three vertex shaders, so B is
  recovered the same way. `temporal_shader_build` checks each variant's input signature, its slots and its
  macros against these.
- **Not covered:** the quality 0 to 2 ice and condensation shaders (`F90EA0D834A8B44E`, `4BF640EED290C885`,
  `3B4EDBBAF1BA87BF`, `2A4B54153FBF97EE`) bind another slot layout (the directional light at t0, the shadow map
  at t1, and so on), which was not verified against the overlay, so those draws stay as the game draws them
  with no legacy dirt. The test rig drew only quality 3.

**Cost.** The intact variant now shades every pane pixel (the 0.02 base alpha and the mask 0.025 x B never
reach zero), where before it skipped uncovered pixels; the overlay costs one extra draw of the canopy a frame
while ice or condensation shows. Both are to be measured on the flight.

**Rig numbers** (WARP, debug layer on): dirt under the sun, vertex B 0.72, rgb 0.09766 at alpha 0.02 (wanted
0.09769); hangar (probe only) 0.01964 (0.01964); lit crack 0.3728 (0.3730); crack glare 0.3542 (0.3544);
smudge glare 0.12238 (0.12240) at alpha 0.02; ice and condensation overlays 0.001953 with alpha written 0, equal to
the intact variant's alpha times colour; no streak texture: no dirt.

**Ambient ported (2026-11-03, after the flight of bd441ccd). WITHDRAWN the same day (see "Dirt strength" below): legacy's cube was black, so the 5 multiplied zero.** That flight ran everything (streaks bound, binds
and ice-overlays about 3,050 per 30 s) and A/B looked about the same. Legacy's ambient was measured and not yet
ported: its vertex shader computes `cube(N) + cube(-N)` at mip 30 (the 1x1 average) times VS cb0[0].y, which the
captures read as 5 (section 10). The port had sampled the probe once along the normal and scaled it by 1.0,
about a tenth of legacy's. Now, in all three variants, `ambient = 5 x (probe(N) + probe(-N))` at the roughest
mip, with `kLegacyAmbientScale = 5.0` a measured constant. One cluster walk serves both directions; only the cube
sample doubles. Legacy applied exposure through this scale, so the value may be revisited by a later Odyssey
capture; it is legacy's own number, not a tuned one. No chosen constant is left in the shader. Rig (WARP): a
0.25 grey cube gives an ambient of 640 (5 x 0.5 x 256); hangar rgb 0.1964 (wanted 0.1964); dirt under the sun
with the probe 0.2939 (wanted 0.2941) against 0.09766 for the sun alone.

**Dirt strength (2026-11-03, after the flight of 268b292c).** Two changes, in every variant. (1) The x5 ambient is
withdrawn: one probe sample along the normal at the roughest mip, scale 1.0, times CB1[90].z, as before 268b292c;
`kLegacyAmbientScale` is gone. The ambient is the albedo times the probe's radiance; legacy's own ambient was zero in
space. (2) The dirt albedo is multiplied by `kDirtStrength = 16.0`, CHOSEN, not measured: Sean chose visible dirt over
matching legacy, which section 12 measures at about 4 to 6 times weaker on the display. Dirt albedo only; the smudge
and crack glare and the crack colour are unchanged. Constants now: measured (0.025, 0.97, 0.02, 0.001 / 120,
0.02 / 450, streak max 0.09); chosen (kDirtStrength 16, ambient scale 1.0 and mip 7, probe cap 16). Rig (WARP): dirt
under the sun 1.5625 (wanted 1.5631) at alpha 0.02; hangar 0.3142 (wanted 0.3143); dirt with a probe 1.8770
(wanted 1.8774); ice and condensation overlays 0.03125 with alpha 0.

**What the flight must check** (after section 8's list): the log lines above; with ice forming, the dirt streaks
stay on the canopy and the ice looks as the game draws it; `ice-overlays` counts about one a frame while ice
shows; the streak pattern's look and scale against the legacy client's; frame time with the overlay on.

## 12. The port replayed in Odyssey's own frames (2026-11-03)

Four RenderDoc captures of Odyssey (build 334925, Steam, flat, 3840x2160; EDVR did not load under RenderDoc, so
stock shaders drew), and the display images of the legacy captures. RenderDoc's `BuildTargetShader` and
`ReplaceResource` ran EDVR's shader in place of the stock one. Captures and results stay in
`%LOCALAPPDATA%\EDVR\captures\`. Each result is one frame per capture. The replay had no streak texture: a flat
0.2667, or an invented high-frequency stand-in, so no picture shows EDVR's real streaks.

- **Stock Odyssey draws nothing on the intact canopy in these frames**: zero pixels change. CB2[17].w is 0.95
  here, against 0.8787 in the probe flight, and k = 1 - saturate(CB2[17].w x 1.0526) is 0. So the value varies,
  and at 0.95 Odyssey's dirt is off. What drives it is not known.
- **Legacy's ambient was zero.** The legacy VS's cube (t0) is a 1x1 black texture and its ambient output is 0 in
  all three legacy captures (all in space; a station was not captured). Section 10's "VS ambient scale 5" only
  ever multiplied black, so `kLegacyAmbientScale` in section 11 is not a measured value. Legacy's dirt in these
  frames is lit by the sun alone.
- **The mask is not the limit.** Odyssey's canopy vertex colour is grey like legacy's, mean 0.52 against 0.72,
  with the same layout (dark mid-pane, bright at the struts and edges). 34% of the visible pane has a recovered
  mask of 0 (vertex blue below 0.2 is lost in `masks.y`). Dirt albedo is about 0.56x legacy's.
- **Legacy's dirt on the display is one or two levels out of 255** over dark sky: median 0.67, p90 1.33 to
  1.67, p99 2.33 to 2.67, max 6 to 7, from replaying each legacy frame without the canopy. These frames have the
  star ahead, so the sun's Lambert term with legacy's inward normal is probably low; a frame with the sun to the
  side or behind may show more, and none was captured. Sean's memory of visibly dirty legacy glass is not
  explained by these three frames.
- **To match legacy on the display** (ambient x1, single probe sample, dirt albedo x F): the two sunlit Odyssey
  captures agree at F about 4 to 6 on p90 and 2 to 4 on p99. At F = 16 the p90 is about 5 levels and p99 10 to 25;
  at F = 32 about 10 and 22 to 45, plainly visible in the replay. The hangar capture cannot calibrate it: 2%
  alpha darkening of a bright interior already exceeds legacy's levels.
- Legacy's black level on the display is visibly lifted and warmer than Odyssey's in these frames. That is not
  the canopy draw, and it may be part of the remembered look. Not investigated.

## 13. The streak texture reworked (2026-11-03)

Sean saw that EDVR's streaks were the wrong character, and the numbers agreed. Summary statistics of the legacy
dirt texture (t2, linearised) against the first EDVR texture, all normalised to each texture's max, measured with
the same script (`streak_stats.py`: spectrum by cycles per tile, structure tensor on a window of 1/16 of the size):

| | legacy | first EDVR (256) | reworked (512) |
|---|---|---|---|
| std | 0.093 | 0.220 | 0.088 |
| p5 / p50 / p95 | 0.158 / 0.245 / 0.442 | 0.039 / 0.169 / 0.753 | 0.149 / 0.251 / 0.431 |
| energy, cycles per tile 0-4 / 4-16 / 16-64 / 64+ | 0.418 / 0.236 / 0.161 / 0.185 | 0.228 / 0.076 / 0.268 / 0.428 | 0.391 / 0.218 / 0.211 / 0.179 |
| coherence, local / global direction agreement | 0.413 / 0.972 | 0.489 / 0.111 | 0.466 / 0.968 |

The first texture was bright hairlines curling every way over dark holes; legacy's is a soft broad wipe: low
contrast strokes running one way with a gentle bend, faint fibres on top, no black holes. `cockpit_glass_streaks.h`
is rewritten, still EDVR's own value noise, deterministic and tileable, and set only from those summary figures
(never from the legacy pixels): a flow that is mostly uniform along x with a slow periodic bend (the stroke
centreline displaced by a two-cell periodic noise of x), four standardised layers added by variance share (very low
frequency blotches 0.36, soft stroke bands 0.04 and 0.16 at two scales, short line-integrated fibres 0.18, isotropic
grain 0.30), and an exponential of the sum, so values are positive with a soft upper tail and nothing is black; the
exponent is found by bisection so the stored mean stays 0.2667 of the max (the shader's 0.09 peak and the legacy
0.024 mean still hold). Size 512 x 512, 10 mips; generation 97 ms in the rig (about 190 ms in a standalone
harness), on the worker thread, logged. The tile scale is unchanged (the on-screen repeat was measured equal to
legacy's, 6.26 against 6.27 repeats per screen width). The rig now asserts the targets: std within 0.015, the
percentiles, each band within 0.06, global agreement at least 0.85, local coherence between 0.3 and 0.55,
deterministic, tileable, the mip chain. Pictures: `%LOCALAPPDATA%\EDVR\captures\edvr-streaks.png` and the 2 x 2
tiling.

**Reworked again (2026-11-03, after 40da695a).** The first rework met the figures above and still read as blotchy static
with fine horizontal scan lines; legacy reads as long soft diagonal brush strokes. The figures were incomplete: a
per-band spectrum elongation (eigenvalue ratio of the band's second moment) and stroke angle show legacy elongated in
every band along one diagonal (6.7 / 4.1 / 6.3 / 2.1 at 44.5 / 53.4 / 52.2 / 55.0 degrees for 1-4, 4-16, 16-64, 64+ cycles
per tile), where EDVR's isotropic blotch (0.36 of the variance) and grain (0.30) carried the broad bands. The generator
now has no isotropic blotch: three stroke layers of periodic value noise in the diagonal coordinates u = x - y and v = x + y
(both of period `size`, so the layers tile on a diagonal flow with no seam, checked), the lattice coarse across and
coarser along (3 x 1, 12 x 3 and 40 x 16 cells, across by along), the across coordinate bent by a slow periodic noise of v; thin fibres
(white noise smeared along a flow at about 49 degrees); and isotropic grain at 0.07 of the variance, in the finest band only.
Variance shares 0.38 / 0.20 / 0.26 / 0.07 / 0.07, an exponential for the soft tail, mean 0.2667 kept. Measured with the
same script on the saved PNG: std 0.110 (0.093), p5 / p50 / p95 0.129 / 0.243 / 0.482 (0.158 / 0.245 / 0.442), band
energies 0.456 / 0.238 / 0.145 / 0.162 (0.418 / 0.236 / 0.161 / 0.185), elongation 5.7 / 2.7 / 6.3 / 2.0, stroke angle
48.5 / 45.9 / 44.3 / 46.1, global direction agreement 0.999 (0.972), local coherence 0.400 (0.413). Misses: the 4-16
elongation is 2.7 against 4.1 (34% low; the rig allows 40%), std and p95 are a little high (the exponential's tail), and
the picture still shows soft dark pockets between the strokes. 512 x 512, 93 ms in the rig. The rig measures the same
elongation and angle and holds them.

**Painted strokes (2026-11-03, after dbc5ee33).** Sean, seeing it beside legacy's: legacy still looks like brush strokes,
EDVR's like diagonal noise. Why the spectrum targets could not capture it: every target so far (band energy, elongation,
stroke angle, coherence, percentiles) is a power spectrum or a gradient statistic, which throws the phase away. Stretched noise
can match all of them and still have no strokes in it: nothing in it has an identity, an edge or an end. So the generator
now paints. `cockpit_glass_streaks.h` lays down 255 strokes, large first and fine last (110 large, 60 medium, 40 small,
25 fine), each composited over what is beneath it (a lerp by the stroke's own opacity), so later strokes overlap and cut across
earlier ones. A stroke is a gentle quadratic arc at about 50 degrees plus a slowly varying periodic bias across the tile and its
own spread; across its width it has its own 1D bristle noise (nearly constant along the length, slow drift), so fibres run its
whole length; opacity ramps up at the start and frays out at the end per bristle (each bristle starts and stops at its own
distance), with soft but definite edges; and it has its own value in a narrow range around a mid-dark base. Every stroke is drawn
modulo the tile, so it wraps with no seam (the rig's seam check holds). The sum goes through the exponential, found by
bisection, to hold the stored mean at 0.2667 of the max. Judged by eye against legacy's picture (family of look only, nothing
traced or fitted): overlapping brush strokes with visible bristles and ragged ends, in legacy's family but crisper and
more contrasty than its soft wipe, with more bright hairline fibres. Measured: std 0.097, p5 / p50 / p95 0.141 / 0.247 /
0.451, band energies 0.252 / 0.392 / 0.214 / 0.143 (legacy 0.418 / 0.236 / 0.161 / 0.185), elongation 4.0 / 11.1 / 14.7 / 5.3,
stroke angle 39.9 / 49.9 / 49.9 / 47.6, global agreement 0.981, local coherence 0.812 (0.413). The painted result is more
elongated and more coherent than legacy's in the finer bands and has less low-frequency energy; the rig's checks are
loosened to what it honestly gives (band energy within 0.2, every band elongated at least 2, angle within 15 degrees,
local coherence 0.3 to 0.9) and gain a bristle check (fine-band gradient energy along the stroke direction over the energy
across it: 0.15, required under 0.5). Generation takes about 490 ms in the rig and 530 ms in a standalone harness (about 10 M
stroke samples), on the worker thread: at the edge of the 500 ms line, so the stroke counts were not cut; if it matters, the
cost is the sampling step (0.5 texel) and could be coarsened.

**Wide smears, not lines (2026-11-03, after 6f5ab11c).** Sean: "more like hair rather than paint brush strokes". The painted
method was right and its proportions were wrong: the unit you could see was a thin, bright, curved line (255 strokes, a
third of them under 14 texels wide, curved, with strong bristle contrast), and the numbers saw it (local coherence 0.81
against legacy's 0.41; 0-4 band energy 0.25 against 0.42). Now there are 210 strokes in three classes, none narrower than 40
texels (40 broad smears 120-200 texels wide and 0.9-1.4 tiles long; 70 of 60-130 texels; 110 of 40-70), nearly straight (the
arc's sag is under 3% of the length, well under the width), their direction spread about +-6 degrees around 50 with the
slow periodic bias kept, opacity 0.25-0.6 with a feathered falloff of a third of the width each side, a narrow value range
(+-0.065 to +-0.075 around the base), and a faint short-wavelength bristle grain (3.5 texels, 12-14% of the value, clamped
to 0.3-0.7 so no bristle stands out). The ragged ends stay, at low contrast. The remap is now a floor of 0.16 plus an
exponential of the standardised painting clamped at 3 sigma (the plain exponential made a few bright cores; the percentile and
power remaps tried first made more contrast, not less). Measured, against legacy: std 0.107 (0.093), p5 / p50 / p95 0.173 /
0.227 / 0.494 (0.158 / 0.245 / 0.442), band energies 0.437 / 0.389 / 0.134 / 0.040 (0.418 / 0.236 / 0.161 / 0.185),
elongation 3.3 / 7.8 / 25.0 / 10.0, stroke angle 43.9 / 54.2 / 52.1 / 50.2, global agreement 0.991, local coherence
0.873 (0.413), bristle ratio 0.11. By eye it is now broad soft wipes with a faint grain and no individual hair; it is darker
and straighter than legacy's, whose sweeps curve and which carries more mid-frequency grain; it still has some short dark
dashes where bristles end. Targets still missed: local coherence (0.87, target 0.4-0.55: the wipes are very uniformly oriented), and the 4-16
band (0.39, target 0.2-0.3); the 0-4 band (0.44) is in range, and the 64+ band is far lower than legacy's (0.04). The rig keeps its
loosened checks (band energy within 0.2, every band elongated, angle within 15 degrees, local coherence 0.3 to 0.95, p95 within
0.06) and the bristle check (0.11, under 0.5). Generation fell from about 490 ms to about 360 ms (step 0.7 texel), on the worker thread.

**Continuous dirt mask, strength 8 (2026-11-03, after the flight of 901831de, on top of the main merge 1fa92578).** Sean:
"weird unnatural edges" (hard polygon edges on the panes) and too much dirt for a clean windshield. The hard edge was a bug in the
mask section 11 specified: it recovered `B = liveDirt / denom - CB2[6].w` where liveDirt > 0 and 0 elsewhere, and with
CB2[6].w = -0.2 that jumps from 0 straight to 0.2 at the boundary of every triangle whose three vertices live's vertex shader
clamped to 0, so the zero regions had hard polygon outlines. Now `dirtWeight = liveDirt / (CB2[6].z x CB2[0].x)`, floored at 0, with
no offset: it is B - 0.2 in effect and runs smoothly to 0 at the boundary; the shader says in a comment why the offset must not be
added back. `kDirtStrength` goes from 16 to 8, still CHOSEN, not measured: with the offset gone the mask is about 0.7 times what it
was, so 8 is near the 4 to 6 times that section 12 measures as matching legacy on the display. Rig (WARP, debug layer on): dirt
under the sun for B = 0.72 is 0.5645 (wanted 0.5645) at alpha 0.02; hangar 0.1135 (0.1135); dirt with a probe 0.6777 (0.6779); ice and
condensation overlays 0.01128 (0.01129); and a new continuity case: a mask of exactly 0 gives 0 and 1e-4 gives 0.000434
(wanted 0.000434), in proportion to the 0.13 mask's, with no step. `hotkey_capture_test` still passes with the merge's
four-row Visual tab (no change needed), and the full build on the merged tip passed.

**Calmer highlights, strength 4 (2026-11-03, after the flight of 711d02eb).** The polygon edges were gone; Sean: "probably still too
intense", with too much brightness in the dirt texture compared with legacy. Two changes. `kDirtStrength` 8 to 4 (still chosen).
And the streak texture's remap: with the stored mean fixed at 0.2667 of the max, the exponential remap left the bright values
at p90 / p95 / p99 of 1.54 / 1.85 / 2.49 times the mean against legacy's 1.43 / 1.59 / 2.14, in broad connected sweeps.
The remap is now a quantile mapping onto legacy's summary distribution: each texel keeps its rank in the painting and takes
its value, as a ratio to the mean, from legacy's percentile ratios (p5 0.571, p50 0.884, p90 1.43, p95 1.59, p99 2.14,
p99.9 2.55, max 3.75, a floor of 0.3 at the bottom; only those summary figures are used), so the painting's strokes are kept and
the distribution of brightness is legacy's. The painting's per-stroke value range is also cut (0.07, 0.05, 0.05) so the broad
sweeps carry less of the contrast, and the narrowest class is a little fainter. Measured over the mean, ours / legacy: p5 0.569 /
0.571, p50 0.876 / 0.884, p90 1.416 / 1.430, p95 1.576 / 1.591, p99 2.116 / 2.135 (all within 1.5%); std 0.095 (0.093);
band energies 0.434 / 0.311 / 0.195 / 0.060; stroke angle 46.4 / 51.5 / 50.8 / 49.5; local coherence 0.899. `kStreakMax` is
unchanged at 0.09: the stored max is still 255 and the stored mean is 0.2687 of it (the table's mean over its top), so the linear
mean is 0.0242 (legacy 0.024) and the peak 0.09. By eye: an even, dim, brushed wipe in which no broad area stands out brighter; the strokes still
read as streaks, with a few thin bright streaks left. The rig now holds the percentile ratios (p5, p50, p90, p95, p99 within 8% of
legacy's) instead of percentiles over the max. Rig shader values with strength 4: dirt under the sun 0.2822 (wanted 0.2822), hangar
0.05673 (0.05675), ice overlay 0.005642 (0.005645).

## 14. The Vulture's canopy (RenderDoc, 2026-11-03)

Sean's flights are in a Vulture. Section 12's Odyssey captures, and the legacy captures, are an Asp Explorer, on
different meshes (legacy 2720 vertices, Odyssey 1208). One capture of Odyssey in the Vulture, no EDVR: frame 8848.

- Same shader and constants as the Asp: intact q3 only; CB2[0], [3], [4], [6], [15], [17] identical, including
  CB2[17].w = 0.95 (stock dirt off).
- **Panes:** 14 components, 6 significant, each with one continuous unmirrored UV chart. A 50-degree texture
  direction lands at 104, 171, 50, 62, 32 and 49 degrees on screen (area 27, 27, 13, 11, 11, 10%), tight within each
  flat pane. On the Asp: 175, 76, 24, and 38 on the curved piece. Legacy's Asp: 30 to 62.
- **Mask:** 37% of the visible pane has `masks.y` exactly 0 (Asp 34%); raw vertex blue mean 0.63 (Asp 0.52).
- **Scale:** about 10 repeats per screen width (Asp 6.27, legacy's Asp 6.26), not stretched. There is no legacy
  Vulture capture to compare with.
- **Light:** sun colour 806 here, N.L 0.62 on the glass, against 350 and 0.47 on the Asp frame the ×16 replay
  pictures came from, so about 3 times the sunlight on the dirt. This is one frame each, and sun colour varies by star.

**Grime placement: film, soft edge, patches (2026-11-03, after the flight of bcb4e91f).** Sean: x4 is "nicely subtle", but the grime "clings to
the edges of the panel with a hard edge, feels artificial". The vertex mask is the cause: on the Vulture it is a bright frame against the struts,
faceted by the triangles, with 37% of the visible pane at exactly 0. Sean approved a new placement. The dirt mask is now
`kLegacyDirtMask x grime x patch` in every variant. `grime = kGrimeFilm (0.16, since raised, see below) + kGrimeEdge (0.2, likewise) x smoothstep(saturate(dirtWeight / 0.8))`,
in the units of dirtWeight (0 to 0.8 on the ships measured): a film everywhere on the pane (the main term), and a small gather toward the
frame from the vertex mask with no kink where it reaches 0, at most 0.36 against 0.8 before (the Vulture's visible mean dirtWeight was
about 0.22, so the overall level stays about where Sean likes it). `patch` samples the streak texture a second time, unrelated to the mesh:
SampleLevel at `texcoord x 0.19` (not a simple ratio, so it does not line up with the main repeat) at mip 5 of 10, normalised around the
stored mean 0.2687 as `clamp(1 + 0.7 x (stored - mean) / mean, 0.4, 1.6)`. Measured on the generator's mip 5 (16 x 16 texels): stored
0.149 to 0.494, so the multiplier runs 0.688 to 1.587 with mean 1.006. With t12 unbound the streak factor in the albedo is 0 and the dirt is off,
as before. Film, edge and patches are CHOSEN placement, not measured: legacy's dirt mask covered most of the pane, and the game's
vertex mask reads as frost at the frame. The albedo line, x4, the streak sample, glare, crack, alpha and the ambient are unchanged.
Frame time: dirtMask is now never 0, so in the overlay variants the early-out (crack + dirt + smudge below 1/1024) never fires, and the
intact variant already shaded every pane pixel (the 0.02 alpha base): every covered pixel now shades fully, which is the frame-time question
for the flight. Rig (WARP): dirt under the sun (B 0.72) 0.2637 (wanted 0.2637); film: mask 0 gives 0.1389 (wanted 0.1389), mask 1e-4 gives
0.1389 (no step), edge at its maximum 0.3125 = 2.250 times the film (the bound (0.16 + 0.2) / 0.16); hangar 0.05301 (0.05302); dirt with a
probe 0.3167 (0.3167); ice and condensation overlays 0.005272 (0.005274), alpha 0; and a CPU check that the patch multiplier stays
within 0.4 to 1.6 over mip 5 and averages 1.006, with the shader's constants pinned to the header's.

Flown (15619672): Sean could see no difference from `odyssey`, "crystal clear". Keeping the average level was the wrong
target. On frame 8848's panes the old mask's mean grime was 0.200 with p90 0.703, and this one's 0.208 with p90 0.352, so
the areas he had called subtle lost half their brightness. Raised to film 0.4, edge 0.3, at Sean's approval: mean
0.473, p50 0.401, p90 0.688, max 0.700. x4 and the patches are unchanged. In that replay, with the streak at its mean,
the mid-pane adds 0.00067 to a sky of 0.00143, up from 0.00027. Rig (WARP): dirt under the sun 0.5342 (wanted 0.5345);
film 0.3472 (0.3474), mask 1e-4 0.3472, edge at its maximum 0.6074 = 1.750 times the film; hangar 0.1074 (0.1075);
dirt with a probe 0.6416 (0.6419); ice and condensation overlays 0.01069 (0.01069), alpha 0.

## 15. A second bright line under the top strut (flight 15619672, 2026-11-03)

Sean's screenshot, flat, DLSS, Vulture: a dashed bright line parallel to the strut's straight section, a few rows
below the white line along its edge, about 111 of 255 on a background of about 8. **Not the glass:** Sean first saw
it gone with `odyssey`, then saw it with `odyssey` too, and it looks to him like a bug with AA on (not measured).
Replay of frame 8848 (no DLSS, no streak texture, another camera position), done before that, agrees:

- 111 display levels is a scene value of about 0.08 (that frame's stock scene-to-display mapping).
- The dirt reaches at most 0.009 with the streak and patch at their tops, and at most 7 display levels near the
  strut, at each of the flight's sun directions tried (samples 1, 3, 6) and three axis directions. Forcing shadow
  to 1 changed nothing.
- The crack branch is the only term with the size (albedo 0.1 x 256 x 0.00390625), but the vertex shader passes
  `masks.x = (mask + 0.0039) * CB1[55].y`, and CB1[55].y is 0 in all six flight samples and the capture.
- The canopy draw has depth writes off, so the swap cannot change the depth DLSS sees.
- The white line along the strut is in the target before the glass draws, and the glass does not cover it there.
- The glass mesh has a gap 1 to 2 rows tall about 10 rows below the strut at 4K, along the straight section. The
  film adds about 0.0005 on both sides over a sky of 0.003, and nothing in the gap: a faint dark hairline this
  film makes visible.

So the line is not this arc's. The hairline at the gap is this film's own and still stands, faint, and has not been
seen on the glass.

## 16. CB2[17].w behaves like per-ship wear (2026-11-03)

The shader's string table names it `pLayerMaskCutoff`. Live scales its dirt and smudge coverage by
`1 - saturate(w / 0.95)`, so at 0.95 they are fully off. Read from the probe lines in every Steam log of 2026-11-03:

- The Explorer_NX flights, 09:08 to 12:33 local: 0.87869 falling to 0.874599, about 0.000023 per 30 s (0.0028 an hour)
  in the windows where it moves, stalling in others, and carried across game restarts. So it is stored, not per session.
- Flights from 16:36 local: 0.95 throughout (one sample 0.949999). Sean flies a Vulture there; those journals hold no
  ship event, so that is from Sean. The Asp and Vulture RenderDoc captures also read 0.95.
- It is not hull or canopy health: the journal has hull 1.0 and ShipCockpit health 1.0 at 0.87. A RepairAll at 09:47
  local did not raise it.

INFERRED: a wear value that makes Odyssey's dirt grow as a ship is used. Not established: what resets it (a repaint was
not seen), whether the fall is linear, and why the 0.95 ships hold still. At that rate, 0.95 to 0 is about 340 hours of
flight, an extrapolation. EDVR's dirt does not read this value. It does honour `gCockpitGlassCrackAmount` (CB1[55].y)
through the vertex mask, which was 0 in every sample.

Corrections: the Vulture is a training-scenario ship (Sean), which fits its silent journals and its steady 0.95, so the
"0.95 ships hold still" question falls away for it. The Asp captures also read 0.95 and are unexplained.

**What the code says (Ghidra, read-only, builds 312425 and 334925; addresses are RVAs).** The value is written at runtime
from one float f in both builds: `pLayerMaskCutoff = 0.95 - 0.9 f` and `pDirtLevelScale = 1.2 f + 0.3` (312425
FUN_141BCCCA0, 334925 FUN_142DE6090; 312425 also sets `pFadeLevelScale = f`). So 0.95 is f = 0, 0.8787 is f = 0.079, and
Odyssey's glass dirt is shown at 1 - w / 0.95, about 0.947 f. **What f is was not resolved**: it comes from a polymorphic
object in both builds, so calling it wear is still an inference from how it behaves.

**Horizons:**

- The glass does not use it. The intact PS has no such term (dirt = texture x mask x AlphaMultiplier), and no path from
  wear to the glass was found. So legacy's dirt on the glass does not grow with use, as far as this reaches.
- Which materials legacy applied the block to is not determined.
- Crack, tint, alpha multiplier, ice and condensation come from cockpit-effect state machines (312425 FUN_142094690 holds
  them, FUN_1420951F0 pushes them). The crack's driving float was not found, and only part of the exe was read for crack writers.
- The dirt multiplier 0.025 was not located, and three captures of one ship cannot show it is constant.
- `maxWear` and `wear` are server JSON keys for repair costs and menu state (312425 FUN_1410EDCB0, 334925 FUN_141E489C0).
  `WearComponent` is a registered entity component with no reader or accumulator found.
- The station repair screen had a "repair tear and wear" option (312425 FUN_1411B2440), and its strings are gone from
  334925. What buying it did was not resolved, so whether it reset f is not known.

**Decided (Sean, 2026-11-04): the legacy grime stays fixed.** It does not read CB2[17].w, so it looks the same on every
ship, new or worn, matching Horizons as far as the code reaches. With `odyssey`, the game's own wear-driven dirt is
unchanged. The unresolved items above no longer block anything here; f matters again only if this is reopened.

## 17. A live dirt level (2026-11-05, BUILT, NOT FLOWN)

Sean's eye decides how much dirt the legacy glass shows (the measured legacy dirt is one or two display levels; sections 12 and 13), and
each value tried cost a build and a flight. So the strength is now a setting: `visual.cockpit_glass_dirt`, a whole percent from 0 to 300,
shipped 100, live, both profiles, on the Visual page (VR) and Visual tab (flat) directly under Cockpit glass. 100 is the shader's
`kDirtStrength` 4.0 as flown in 25cb4723; 0 is a clean pane (the alpha base still darkens it a little); 300 is three times the dirt.

How. The shader gets a one-register block `Level` at **b13** (`dirtLevel.x` = percent / 100), and multiplies the dirt albedo term by it
in all three variants. Nothing else is scaled: not the crack colour, the smudge and crack glare, or the alpha. `cockpit_glass.cpp` makes
a 16-byte DEFAULT buffer per device, uploads a new value only when the setting moved (`vScreenUpdateSubresourceRaw`), and binds it with
the streak texture (`bindGlassInputs`), saving what the game had at b13 and giving it back where t12 is given back (the overlay: around
its one reissued draw). b13 is the last of D3D11's fourteen slots; the canopy shaders bind b1 and b2, and `ui_depth.cpp` already
uses b13 for its own draws. `vScreenPSSetConstantBuffersRaw` is a new raw call: `hookedPSSetConstantBuffers` exists only for the flat
scope's discovery observer, and the glass's bind should not reach it.

Failure modes, both visible and neither stock: a refused block is bound as null, so the shaders read 0 and draw no dirt (logged, "the dirt
level block was refused"); an unbound b13 reads 0 the same way, which `tools\cockpit_glass_test` pins. A key outside 0 to 300 is
clamped and logged by `getIntInRange`.

The menu. The row is dimmed, and refuses a step, a typed value and the reset, unless `visual.cockpit_glass` is `legacy`
(`cockpit_glass_core.h` `dirtRowDim`, `menu.cpp` `cockpitGlassDirtRowDisabled`, read live as the glass reads it). It is dimmed and not
hidden, as the Sharpening and DLSS preset rows are, so the setting does not seem to vanish. The flat gate lists the key
(`flat_profile_keys.h`), which matters: unlisted, `getIntInRange` answers 0 in flat and the pane would be clean whatever the file says.

What the flight must check: with `legacy` the log says `legacy on ... at a dirt level of N%`, and each change says `dirt level N%`;
200 shows twice the dirt of 100 and 0 shows none; choosing `odyssey` dims the row and its steps are refused with the line
"Legacy glass dirt is inactive"; the same in the headset (both eyes) and on the flat Visual tab; and the ice and condensation draws
follow the level as the intact canopy does.

## Ruled out

- ruled out: the legacy glass causing the second line under the strut, because Sean sees it with `odyssey` too; the
  dirt and smudge light also reach only about a tenth of its brightness in replay, and the crack amount CB1[55].y
  is 0 in the flight (section 15).
- ruled out: rotating the stroke direction by the surface normal so panes differ, because both Odyssey meshes
  measured already run a single texture direction several ways across their panes, through their UV charts
  (section 14).

- ruled out: porting legacy's ambient scale of 5, because legacy's VS cube is black in all three captures
  (section 12); the scale multiplied zero.
- ruled out: the dirt mask as why the port looks clean, because Odyssey's canopy vertex colours are grey with
  the same layout as legacy's and only about a third of the pane is zero (section 12).

- ruled out: swapping the legacy bytecode in, because it reads cb0 where
  live binds CB1/CB2, its texture slots differ, and its input signature
  does not match the live VS outputs (sections 2 and 3).
- ruled out: "the Effects archives hold no reachable DXBC"
  (`fss-scanner.md`), because the earlier scans used the standard FNV
  basis; under EDVR's basis 634 dumped shaders match archive blobs
  byte for byte.
- ruled out: ambient from `gAmbientColour` CB1[257], because it was 0 in
  all eight probe samples, docked and in sunlight (section 7).
- ruled out: lighting the canopy from t1 alone in a hangar, because the
  sun's colour there is 1e-13; hangar light reaches the glass only
  through local lights and probes (section 7).
